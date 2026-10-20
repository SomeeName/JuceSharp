#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#if JUCE_WINDOWS
 // timeBeginPeriod / timeEndPeriod (W4-N14), DECLARED rather than included.
 // <mmsystem.h> requires <windows.h> to have been included first, and JUCE
 // deliberately defers that — including it here breaks the build in every
 // translation unit that reaches this header (mmsyscom.h errors on MMVERSION).
 // These two are a stable documented C entry point in winmm, which JUCE already
 // links for the Windows MIDI and WASAPI backends, so this adds no dependency.
 // MMRESULT is UINT and TIMERR_NOERROR is 0.
 extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int period);
 extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int period);
 #pragma comment(lib, "winmm.lib")
#endif

namespace dotcharter {

/// Hardware MIDI OUTPUT with a deadline scheduler (ABI v34).
///
/// WHY NOT `juce::MidiOutput::sendBlockOfMessages`. JUCE has its own background
/// sender, and it is the obvious answer until you read it: `sendBlockOfMessages`
/// takes the output's `CriticalSection` on the CALLING thread to splice into
/// the pending list, and its thread wakes on `Thread::wait`, i.e. the Windows
/// timer granularity. It also takes its deadlines in
/// `Time::getMillisecondCounter()` MILLISECONDS. A lock on the caller's side
/// rules it out of anything that might run near the audio thread, and
/// millisecond deadlines throw away the whole point of a sample-accurate
/// schedule.
///
/// WHAT THIS DOES INSTEAD. One dedicated high-priority thread per open device,
/// fed an UNSORTED queue of {message, QPC deadline} which it SCANS for anything
/// due. The queue is not kept in order — insertion is O(1) and the scan is
/// bounded by `kMaxQueued` — so the batch the scan collects is sorted by
/// deadline immediately before it goes on the wire (whenever more than one
/// message falls due in a single wake, which is routine: the sleep is capped at
/// 20 ms and any load overshoots it). The FRAME -> DEADLINE
/// conversion happens on the CALLING thread, in `dca_midi_output_send_at`,
/// against the engine's ABI v26 render clock — which is why this class never
/// needs to know about an engine and cannot outlive one it holds a pointer to.
/// The thread sleeps while more than `kSpinAheadSeconds` of its earliest
/// deadline remains, then BUSY-SPINS the rest on `getHighResolutionTicks`. That
/// spin is what buys the accuracy: a `Thread::wait(1)` is 1 ms only while some
/// process on the box holds `timeBeginPeriod(1)` and ~15.6 ms otherwise (the
/// same trap that made `RtBridgeWaitIsBounded` phase C flake), so a sleep-only
/// sender is at the mercy of whether a browser happens to be open.
///
/// **AND THAT TRAP USED TO DEFEAT THE SPIN ENTIRELY** (W4-N14). The spin-ahead
/// was 1.5 ms, so a deadline 2 ms out took the SLEEP branch, computed
/// `jlimit(1.0, 20.0, 0.5)` = 1, and called exactly the `wait(1)` the paragraph
/// above warns about — landing ~14 ms late on a box with the default 15.6 ms
/// timer. The accurate branch was only entered when the loop happened to ARRIVE
/// inside the last 1.5 ms, which is decided by the sleep it just performed:
/// every deadline between ~1.5 ms and ~20 ms out was at the mercy of whether a
/// browser was open, which is precisely what this class exists not to be.
///
/// The fix is to stop depending on the ambient timer resolution: this class
/// REQUESTS 1 ms for the life of an open device (`timeBeginPeriod`, Windows
/// only, released in `shutdown()`), so `wait(1)` means what it says and the
/// 1.5 ms spin is once again the last and only inaccurate step. Raising
/// `kSpinAheadSeconds` past one tick instead was the other candidate and is
/// worse: at 200 messages 2 ms apart it spins a whole core continuously, where
/// this costs the system a slightly higher timer interrupt rate for as long as
/// a MIDI output is open and nothing at all when one is not.
///
/// **NOTHING HERE IS EVER REACHED FROM THE AUDIO CALLBACK.** `send` / `sendAt`
/// are message/scheduler-thread entry points, which is precisely why the queue
/// is a plain mutex-guarded vector rather than a lock-free FIFO: a lock-free
/// structure here would be pure ceremony, and pretending otherwise would
/// invite someone to call this from the callback.
class MidiOutputDevice
{
public:
    /// Why a `send` did not take the message. A queue-full DROP is not a bad
    /// argument: it used to surface as DCA_ERR_INVALID_ARG, which is what the
    /// caller would then believe, while the note-off it just lost turned into a
    /// stuck note. The engine side got this right in the same wave
    /// (`midiDroppedEvents_`); this is the hardware-output half of it.
    enum class SendResult
    {
        Ok,
        BadArgs,   ///< null/short/over-long message, or no device.
        QueueFull, ///< the message was DROPPED; `droppedCount()` has moved.
    };

    /// Sleep/spin changeover. 1.5 ms comfortably clears one `wait(1)` on a box
    /// that IS holding timeBeginPeriod(1) — which, since W4-N14, this class
    /// makes sure of for the life of an open device. The spin is the accurate
    /// part; the sleep only avoids burning a core while the deadline is far
    /// away, and it is now bounded by a granularity we asked for rather than
    /// by whichever process on the machine last raised it.
    ///
    /// **Do not raise this to "cover a 15.6 ms tick" instead.** That was the
    /// other candidate fix and it turns a dense schedule (200 messages 2 ms
    /// apart is the stated use case) into one core spinning continuously.
    static constexpr double kSpinAheadSeconds = 0.0015;

    static juce::Array<juce::MidiDeviceInfo> enumerate()
    {
        return juce::MidiOutput::getAvailableDevices();
    }

    /// JUCE 8's Win32 backend absorbed two transient driver refusals INSIDE
    /// `juce::MidiOutput`, and JUCE 9 (juce_Midi_windows.cpp) dropped both:
    ///   * `midiOutOpen` answering MMSYSERR_ALLOCATED was retried 4 times,
    ///     100 ms apart — a port another process is just letting go of;
    ///   * `midiOutShortMsg` answering MIDIERR_NOTREADY was retried 50 times,
    ///     1 ms apart — a busy interface. On 9.x the message is simply lost and
    ///     `sendMessageNow` returns false, which for a note-off is a stuck note.
    /// These two helpers put the SAME bounds back on our side, so the port to
    /// JUCE 9 changes nothing a user can hear. JUCE 9 no longer reports WHY an
    /// attempt failed, so every refusal is retried; the bound is what JUCE 8
    /// spent on its worst case, and this runs on the dispatcher / control
    /// thread, never the audio callback. Templates over the attempt and the
    /// pause so `juce_sharp_rtcheck` can pin the bound without a MIDI device.
    /// `pause` returns false to stop retrying (the dispatcher's does when it is
    /// asked to exit, so a close is never held up behind a refusing driver).
    static constexpr int kOpenAttempts   = 4;
    static constexpr int kOpenRetryMs    = 100;
    static constexpr int kSendAttempts   = 50;
    static constexpr int kSendRetryMs    = 1;

    template <typename TryOpen, typename Pause>
    static auto openWithRetry(TryOpen&& tryOpen, Pause&& pause) -> decltype(tryOpen())
    {
        for (int attempt = 1;; ++attempt)
        {
            if (auto opened = tryOpen(); opened != nullptr || attempt >= kOpenAttempts
                                         || ! pause(kOpenRetryMs))
                return opened;
        }
    }

    /// True once `trySend` accepted the message; false after `attempts`
    /// refusals. A throw propagates to the caller, which counts it as failed.
    template <typename TrySend, typename Pause>
    static bool sendWithRetry(TrySend&& trySend, Pause&& pause, int attempts = kSendAttempts)
    {
        for (int attempt = 1;; ++attempt)
        {
            if (trySend()) return true;
            if (attempt >= attempts || ! pause(kSendRetryMs)) return false;
        }
    }

    static std::unique_ptr<MidiOutputDevice> open(int deviceIndex)
    {
        auto devices = enumerate();
        if (deviceIndex < 0 || deviceIndex >= devices.size()) return nullptr;
        const auto identifier = devices[deviceIndex].identifier;
        auto out = openWithRetry([&] { return juce::MidiOutput::openDevice(identifier); },
                                 [](int ms) { juce::Thread::sleep(ms); return true; });
        if (out == nullptr) return nullptr;
        auto dev = std::unique_ptr<MidiOutputDevice>(new MidiOutputDevice(std::move(out)));
        dev->raiseTimerResolution();
        return dev;
    }

    /// Stop the dispatcher and close the device, BOUNDED. Returns false when
    /// the thread did not exit inside the deadline — the caller must then LEAK
    /// this object rather than destroy it.
    ///
    /// **Never `stopThread`.** `juce::Thread::stopThread(ms)` signals, waits,
    /// and on expiry calls `killThread()` — `TerminateThread` on Windows. The
    /// one scenario this class's own `sendMessageNow` comment says it exists to
    /// survive is a wedged interface, i.e. a dispatcher thread parked inside the
    /// OS MIDI API quite possibly holding a driver lock; terminating it there
    /// and then destroying the `juce::MidiOutput` under it is process-wide
    /// corruption, not a lost message. The wrapper's `NativeHandleOwner` drain
    /// already established the answer for exactly this shape: on expiry log at
    /// Error and LEAK. A leak beats a use-after-free.
    bool shutdown(int timeoutMs = 2000)
    {
        if (shutdownComplete_) return true;
        thread_.signalThreadShouldExit();
        thread_.notify();
        if (! thread_.waitForThreadToExit(timeoutMs)) return false;
        // Anything still queued is DROPPED, deliberately: a close is a user
        // gesture and flushing a second of scheduled notes into a device that
        // is going away is worse than losing them. The one thing that must not
        // be lost is an all-notes-off, which the caller sends immediately.
        out_.reset();
        releaseTimerResolution();
        shutdownComplete_ = true;
        return true;
    }

    ~MidiOutputDevice()
    {
        // Only ever reached after a shutdown() that RETURNED TRUE, so the
        // dispatcher has already exited and ~juce::Thread has nothing to wait
        // for. A device whose shutdown timed out is leaked by the ABI close.
        jassert(shutdownComplete_);
        // …and release the timer resolution here TOO (W5-E5). shutdown() was
        // the only caller, so a device destroyed without one — an open() that
        // succeeds while a later step of dca_midi_output_open fails, i.e. a
        // unique_ptr dropped between the two — left the whole PROCESS at 1 ms
        // timer resolution for good. Idempotent, so the ordinary path is
        // unaffected; a device whose shutdown TIMED OUT is leaked along with
        // its request, which is the same trade the handle drain makes.
        releaseTimerResolution();
    }

    juce::String name() const { return out_ != nullptr ? out_->getName() : juce::String(); }

    /// Queue a message. `deadlineTicks <= 0` means "as soon as the dispatcher
    /// can" — still off the caller's thread, so a wedged MIDI driver can never
    /// block a UI.
    SendResult send(const uint8_t* data, int length, int64_t deadlineTicks)
    {
        if (out_ == nullptr || data == nullptr || length <= 0 || length > kMaxMessageBytes)
            return SendResult::BadArgs;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (queue_.size() >= kMaxQueued)
            {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return SendResult::QueueFull;
            }
            queue_.push_back(Pending{ juce::MidiMessage(data, length), deadlineTicks });
        }
        thread_.notify();
        return SendResult::Ok;
    }

    /// Test/diagnostic seam: how many messages the dispatcher has actually put
    /// on the wire. A scheduler is trivially "working" if nothing ever checks
    /// that anything came out the far end.
    int64_t sentCount() const { return sent_.load(std::memory_order_relaxed); }

    /// How many messages were THROWN AWAY because the queue was saturated.
    /// Without this a dense automation pass, or a wedged driver backing the
    /// dispatcher up, loses messages silently.
    int64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }

    /// How many messages the DRIVER threw on. Distinct from `droppedCount()`,
    /// which is "we never handed it over": these reached `sendMessageNow` and
    /// did not come out the other side.
    /// Exported since ABI v36 as `dca_midi_output_get_counts3` — a THIRD EXPORT
    /// and deliberately not a third out-param on `dca_midi_output_get_counts`,
    /// which has callers passing exactly two pointers. Managed:
    /// `JuceMidiOutput.FailedCount` / `.ReadAllCounters()`.
    /// `sentCount() + failedCount()` is what `sentCount()` alone used to report.
    /// (This block said "not yet surfaced through the ABI" for a whole release
    /// AFTER the export shipped, and prescribed the design v36 rejected — the
    /// "this is missing" rot that makes the next reader build it again.)
    int64_t failedCount() const { return failed_.load(std::memory_order_relaxed); }

    static constexpr int    kMaxMessageBytes = 3;      // short messages only, like the input side

private:
    static constexpr size_t kMaxQueued       = 4096;

    struct Pending
    {
        juce::MidiMessage msg;
        int64_t           ticks = 0;
    };

    /// `juce::Thread` rather than `std::thread` so `wait`/`notify` and the
    /// priority request go through the same layer the rest of this engine uses.
    class Dispatcher final : public juce::Thread
    {
    public:
        explicit Dispatcher(MidiOutputDevice& owner)
            : juce::Thread("DotCharter.MidiOut"), owner_(owner) {}
        void run() override { owner_.dispatchLoop(*this); }

    private:
        MidiOutputDevice& owner_;
    };

    explicit MidiOutputDevice(std::unique_ptr<juce::MidiOutput> out)
        : out_(std::move(out)), thread_(*this)
    {
        thread_.startThread(juce::Thread::Priority::highest);
    }

    void dispatchLoop(juce::Thread& self)
    {
        const double perTick = 1.0 / (double) juce::Time::getHighResolutionTicksPerSecond();
        std::vector<Pending> due;
        due.reserve(64);

        while (! self.threadShouldExit())
        {
            const int64_t now = juce::Time::getHighResolutionTicks();
            int64_t       earliest = 0;
            bool          haveEarliest = false;

            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (size_t i = 0; i < queue_.size();)
                {
                    if (queue_[i].ticks <= now)
                    {
                        due.push_back(std::move(queue_[i]));
                        queue_[i] = std::move(queue_.back());
                        queue_.pop_back();
                        continue;
                    }
                    if (! haveEarliest || queue_[i].ticks < earliest)
                    {
                        earliest     = queue_[i].ticks;
                        haveEarliest = true;
                    }
                    ++i;
                }
            }

            // DEADLINE ORDER, not queue order. The scan above collects in
            // vector order and its swap-with-back removal reorders as it goes,
            // so without this a wake that finds three due messages puts them on
            // the wire in enqueue order — fine while the caller happens to
            // enqueue in time order, and nothing enforces that. `due` is a
            // handful of entries and this is not the audio thread.
            std::stable_sort(due.begin(), due.end(),
                             [](const Pending& a, const Pending& b) { return a.ticks < b.ticks; });

            for (auto& p : due)
            {
                // OUTSIDE the mutex: sendMessageNow reaches the OS MIDI API and
                // on a busy or wedged interface it is not bounded. Holding the
                // queue lock across it would make `send` — a UI-thread call —
                // wait on the driver, which is the exact failure this class
                // exists to avoid.
                // W4-N15: INSIDE the try, AFTER the call. This counter's own
                // doc says it reports "how many messages the dispatcher has
                // actually put on the wire... a scheduler is trivially 'working'
                // if nothing ever checks that anything came out the far end" —
                // and it moved even when the driver call threw, which is the one
                // case it exists to catch. A throwing driver is now visible as
                // failed_ rather than invisible AND counted as success.
                // JUCE 9: sendMessageNow REPORTS a refusal (it returned void on
                // 8.x) and no longer retries a busy driver itself, so
                // sendWithRetry restores 8.x's 50 x 1 ms and a message still
                // refused after that is failed_, which is what failed_ means.
                // Unlike 8.x, 9.x refuses on a GONE device too (8.x returned at
                // once there), so after one message has used up its retries the
                // next ones get a single try until the driver takes one again:
                // an unplugged interface costs 50 ms once, not 50 ms a message.
                try
                {
                    const bool accepted = sendWithRetry(
                        [&] { return out_->sendMessageNow(p.msg); },
                        [&self](int ms)
                        {
                            if (self.threadShouldExit()) return false;
                            juce::Thread::sleep(ms);
                            return true;
                        },
                        driverRefusing_ ? 1 : kSendAttempts);
                    driverRefusing_ = ! accepted;
                    (accepted ? sent_ : failed_).fetch_add(1, std::memory_order_relaxed);
                }
                catch (...) { failed_.fetch_add(1, std::memory_order_relaxed); }
            }
            due.clear();

            if (! haveEarliest) { self.wait(200); continue; }

            const double remaining = (double) (earliest - juce::Time::getHighResolutionTicks()) * perTick;
            if (remaining > kSpinAheadSeconds)
            {
                // Cap the sleep so a message queued behind this one with an
                // earlier deadline is still picked up promptly (notify() also
                // wakes us, but a wake that races the sleep must not cost more
                // than one step).
                self.wait((int) juce::jlimit(1.0, 20.0, (remaining - kSpinAheadSeconds) * 1000.0));
            }
            else if (remaining > 0.0)
            {
                // The accurate part. Yielding rather than pausing keeps this
                // civil on a box where another agent is building.
                while (! self.threadShouldExit()
                       && juce::Time::getHighResolutionTicks() < earliest)
                    std::this_thread::yield();
            }
        }
    }

    std::unique_ptr<juce::MidiOutput> out_;
    /// The last message used up every retry. Dispatcher thread only.
    bool driverRefusing_ = false;
    std::mutex                        mutex_;
    std::vector<Pending>              queue_;
    std::atomic<int64_t>              sent_{ 0 };
    std::atomic<int64_t>              dropped_{ 0 };
    std::atomic<int64_t>              failed_{ 0 };
    bool                              timerRaised_ = false;

    /// Windows timer resolution, held for the life of the open device. See the
    /// class comment (W4-N14): without it `Thread::wait(1)` below is ~15.6 ms
    /// and every deadline between the spin-ahead window and one tick lands late.
    /// Idempotent. Released in `shutdown()` and again in `~MidiOutputDevice`
    /// (the second is what covers a device destroyed without a shutdown); a
    /// device whose shutdown timed out is LEAKED, and its request with it.
    /// `timeBeginPeriod` is refcounted per process by the OS, so two devices
    /// each raising and releasing once is correct.
    void raiseTimerResolution() noexcept
    {
       #if JUCE_WINDOWS
        if (! timerRaised_ && ::timeBeginPeriod(1) == 0u) timerRaised_ = true;  // 0 == TIMERR_NOERROR
       #endif
    }

    void releaseTimerResolution() noexcept
    {
       #if JUCE_WINDOWS
        if (timerRaised_) { ::timeEndPeriod(1); timerRaised_ = false; }
       #endif
    }
    bool                              shutdownComplete_ = false;
    Dispatcher                        thread_;
};

} // namespace dotcharter
