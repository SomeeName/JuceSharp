#pragma once

#include "dotcharter_vst_bridge.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace dotcharter {

/// Parent-process side of the out-of-process VST bridge. Owns the named
/// shared-memory mapping plus the request/response events; the sandboxed
/// child host (juce_vst_host.exe) opens the same names.
///
/// prepare()/arm()/disable() are called from managed caller threads (never
/// the audio thread) and are serialized by the caller. prepare() creates and
/// clears the mapping with no barrier held; AudioEngine holds its bridge
/// barrier only for arm() (the swap) and disable(). One bridge serves ONE seat's child (ABI
/// v50, DotCharter #696: the engine holds one per seat). On the audio thread
/// each callback makes at most one bounded round trip per bridge, split in two
/// so every seat's child works at the same time:
///   post():    write input + MIDI -> signal request (no wait);
///   collect(): spin on every posted bridge's responseSeq against ONE
///              one-block deadline -> read each answer back.
/// A bridge that has not answered by the deadline passes its block through
/// DRY and bumps ITS timeout counter; after three consecutive missed callbacks
/// that bridge degrades to a non-blocking poll so a dead child costs (almost)
/// nothing per callback until it returns. One bridge's miss never stops the
/// collection of the others.
///
/// The wait is a user-mode spin on responseSeq, NOT a timed kernel wait: a
/// WaitForSingleObject timeout rounds up to the system timer tick (~15.6 ms),
/// which is several block budgets, so "wait 2 ms" actually slept past the
/// deadline it was meant to protect (finding E-1, measured at 20.5 ms by the
/// native RT gate). See the comment in collect() for the full story.
class RemoteVstBridge
{
public:
    RemoteVstBridge();
    ~RemoteVstBridge() { disable(); }

    RemoteVstBridge(const RemoteVstBridge&)            = delete;
    RemoteVstBridge& operator=(const RemoteVstBridge&) = delete;

    /// The kernel objects of one mapping, created by prepare() and swapped in
    /// by arm(). Owned by whoever holds it; release() closes them.
    struct Prepared
    {
        void* mapping   = nullptr;
        void* view      = nullptr;
        void* reqEvent  = nullptr;
        void* respEvent = nullptr;
    };

    /// Any thread, NO audio barrier needed: create (or open) the named mapping
    /// and events, clear a fresh mapping (about 198 KB) or validate a surviving
    /// one, and stamp its header. The bridge itself is not touched, so the
    /// audio thread can keep using it meanwhile. False (with `out` empty) when
    /// any kernel object could not be created or the mapping carries another
    /// layout.
    bool prepare(const char* shmName, const char* requestEventName,
                 const char* responseEventName, int sampleRate, int blockFrames,
                 Prepared& out);

    /// Caller holds the engine's bridge barrier: swap `p` in, reset the
    /// audio-thread state and arm. `p` comes back holding the objects it
    /// replaced (or nothing), for release() after the barrier is dropped.
    void arm(Prepared& p) noexcept;

    /// Close every kernel object `p` holds. Any thread; never the audio one.
    static void release(Prepared& p) noexcept;

    /// prepare() + arm() + release(), for a caller with no barrier of its own.
    /// A refused preparation disables the bridge and returns false.
    bool enable(const char* shmName, const char* requestEventName,
                const char* responseEventName, int sampleRate, int blockFrames);

    /// Disarm and release all kernel objects. Safe to call repeatedly.
    /// Must not race the audio thread — the caller (AudioEngine) flips the
    /// enabled_ atomic first and holds its process barrier across the swap.
    void disable();

    /// Flip the armed flag only (no handle teardown). The caller uses this
    /// before acquiring its audio barrier so the audio thread stops touching
    /// the mapping before disable() unmaps it.
    void markDisabled() noexcept { enabled_.store(false, std::memory_order_release); }

    bool isEnabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    /// Publish the device configuration the child must prepare for: the rate,
    /// the block size, and then a GENERATION bump so the child sees the two
    /// together. Called from audioDeviceAboutToStart, i.e. on every device
    /// change, sample-rate change and watchdog recovery.
    ///
    /// It replaces setSampleRate(), which wrote only the rate — and the child
    /// read that field exactly once, in openBridge. Every sandboxed VST3
    /// therefore ran at the rate the child was LAUNCHED with for the rest of
    /// the session, and a plugin prepared for 256 frames could be handed 1024
    /// (which entitles it to allocate, on the audio worker the parent blocks
    /// on). The in-process half of this bug is what reprepareAttachedPlugins
    /// closed.
    void setDevicePrepare(int sampleRate, int blockFrames) noexcept;

    /// Audio thread, under the engine's bridge barrier: write this callback's
    /// request and signal the child, WITHOUT waiting. `chainBuf` carries the
    /// (gained) live input in `numSamples` frames across kMaxChannels channels;
    /// collect() replaces it with the child's processed output when the child
    /// answers with an active chain, so it must stay alive and untouched until
    /// collect() returns. `midi`/`midiCount` are the packed instrument events
    /// for this block and `midiOffsets[i]` the sample offset of `midi[i]` inside
    /// it (bridge v3); a null `midiOffsets` places every event at sample 0. An
    /// offset at or past `numSamples` is clamped to the last sample.
    /// `blockStartFrame` is the render-clock frame of sample 0, published to
    /// the child alongside. `seat` (bridge v4) names whose chain the child runs
    /// on this block; the child renders the instrument only for seat 0, so a
    /// seat above 0 sends no MIDI and leaves the instrument output alone.
    /// `callbackSerial` names the audio callback the trip belongs to (any value
    /// that changes once per callback): the miss accounting is per CALLBACK
    /// (timeoutBlocks, and the poll-only threshold), and at most one request is
    /// posted per callback. A bridge in poll-only posts as usual; collect()
    /// checks its answer without ever waiting for it.
    /// Returns true when the request was posted; false (nothing written, the
    /// buffer stays dry) when the bridge is not armed, the block does not fit
    /// the mapping, the seat is out of range, or this callback already posted.
    bool post(juce::AudioBuffer<float>& chainBuf, int numSamples,
              const uint32_t* midi, const uint32_t* midiOffsets, int midiCount,
              int64_t blockStartFrame, int seat, uint32_t callbackSerial) noexcept;

    /// Audio thread, under the engine's bridge barrier: collect every request
    /// posted on `bridges` (null entries and bridges with nothing posted are
    /// skipped) against ONE deadline, one block budget after the collection
    /// starts. Every outstanding answer is checked on every pass of the spin,
    /// so a stalled child never stops the others being collected; at the
    /// deadline one last scan runs, and only the bridges that still have not
    /// answered stay dry and count a miss on their OWN counters. A bridge in
    /// poll-only is checked on every pass too but never holds the collection
    /// open. Nothing allocates, locks or makes a timed kernel wait.
    static void collect(RemoteVstBridge* const* bridges, int count) noexcept;

    /// Audio thread: the outcome of this bridge's last collected request.
    /// answered() is false until collect() saw the child's response for the
    /// request post() wrote (and after a disable() in between). When the child
    /// answered with an active instrument, instrumentL/R point at its rendered
    /// block in the mapping (valid until the next post).
    bool         answered()         const noexcept { return answered_; }
    bool         chainActive()      const noexcept { return chainActive_; }
    bool         instrumentActive() const noexcept { return instrumentActive_; }
    const float* instrumentL()      const noexcept { return instrL_; }
    const float* instrumentR()      const noexcept { return instrR_; }
    /// Audio thread: post() wrote a request that collect() has not settled.
    bool         hasPending()       const noexcept { return pending_; }

    /// Missed callbacks in a row after which the bridge stops waiting
    /// (poll-only), re-trying one synchronous wait every kRetrySyncEveryBlocks
    /// callbacks; an answered poll ends the stretch at once.
    static constexpr int kPollOnlyAfterMissedCallbacks = 3;

    /// Whether the child holds at least one chain plugin for `seat` (bridge
    /// v4, child-published). Audio thread, under the engine's bridge barrier,
    /// like post(): a seat above 0 with nothing to run costs no round trip.
    bool seatHasChain(int seat) const noexcept;

    uint32_t processedBlocks()    const noexcept { return processed_.load(std::memory_order_relaxed); }
    uint32_t timeoutBlocks()      const noexcept { return timeouts_.load(std::memory_order_relaxed); }
    /// MIDI events discarded because the shared mapping's kMaxMidiEvents slots
    /// were full. Every other drop path in this engine is counted; this one was
    /// a bare std::min with nothing said anywhere.
    uint32_t midiTruncated()      const noexcept { return midiTruncated_.load(std::memory_order_relaxed); }
    uint32_t lastHeartbeat()      const noexcept;

#if DCA_RTCHECK
    /// Native tests only (no ABI): called on the arming thread once a
    /// preparation has created (or reused) and stamped the mapping, before the
    /// bridge is armed. Null (the default) calls nothing.
    using PreparedSeamFn = void (*)(void* user);
    void setPreparedSeamForTest(PreparedSeamFn fn, void* user) noexcept { preparedSeam_ = fn; preparedSeamUser_ = user; }
#endif

private:
    /// Releases the kernel objects. Caller must hold viewMutex_.
    void closeHandlesLocked();

    std::atomic<bool> enabled_{false};

#if DCA_RTCHECK
    PreparedSeamFn preparedSeam_     = nullptr;
    void*          preparedSeamUser_ = nullptr;
#endif

    /// Guards view_'s LIFETIME against the accessors that do NOT run under the
    /// AudioEngine's remoteBridgeMutex_ barrier — setSampleRate() (device
    /// thread, on every device change) and lastHeartbeat() (polled by the
    /// managed sandbox health timer). Both dereferenced view_ behind nothing
    /// but a null-check, so disable()'s UnmapViewOfFile could land between the
    /// check and the read and they would touch unmapped memory.
    ///
    /// post() and collect() deliberately do NOT take this: they run on the
    /// audio thread and are already covered by remoteBridgeMutex_, which the
    /// disable path holds across the unmap. Locking here too would put a second
    /// mutex on the real-time path for no added safety.
    mutable std::mutex viewMutex_;

    // Kernel objects (Windows). Type-erased so the header stays portable.
    void* mapping_   = nullptr;
    void* view_      = nullptr;
    void* reqEvent_  = nullptr;
    void* respEvent_ = nullptr;

    /// Audio thread: start a new callback's bookkeeping when `callbackSerial`
    /// moves on, folding the previous callback's outcome into the miss count.
    void beginCallbackIfNew(uint32_t callbackSerial) noexcept;
    /// Audio thread: record that this callback missed (once per callback).
    void markCallbackMissed() noexcept;
    /// Audio thread (collect): whether the child published the pending
    /// request's sequence. One acquire load.
    bool responseArrived() const noexcept;
    /// Audio thread (collect): settle the pending request, answered or dry.
    void settle(bool gotResponse) noexcept;

    // Audio-thread-only state.
    uint32_t seq_ = 0;
    /// Missed CALLBACKS in a row: see beginCallbackIfNew().
    int      consecutiveTimeouts_ = 0;

    // Audio-thread-only, per callback. `cbOpen_` is false until the first trip
    // (and again after enable), so the first callback folds nothing.
    bool      cbOpen_      = false;
    uint32_t  cbSerial_    = 0;
    bool      cbMissed_    = false;  // the callback's request missed its deadline
    bool      cbAnswered_  = false;  // the callback's request was answered
    bool      cbPollOnly_  = false;  // this callback does not wait
    bool      cbPosted_    = false;  // this callback already posted its request

    // Audio-thread-only: the request post() wrote and collect() settles.
    // Cleared by enable()/disable() (under the engine barrier), so a request
    // posted to a mapping that is gone is never read back.
    bool                      pending_         = false;
    uint32_t                  pendingSeq_      = 0;
    int                       pendingFrames_   = 0;
    int                       pendingChannels_ = 0;
    juce::AudioBuffer<float>* pendingBuf_      = nullptr;
    long long                 budgetTicks_     = 0;   // one block of QPC ticks, from the pending request
    bool                      answered_         = false;
    bool                      chainActive_      = false;
    bool                      instrumentActive_ = false;
    const float*              instrL_           = nullptr;
    const float*              instrR_           = nullptr;

    /// QueryPerformanceFrequency is FIXED for the life of the process
    /// (documented since Windows XP SP2), so it is read once, at construction,
    /// never on the audio thread. 1 is a safe floor: it only ever divides, so a bad value
    /// would shorten the spin rather than hang it.
    long long qpcFreq_ = 1;

    /// Blocks spent in poll-only since it latched. Poll-only used to be a
    /// ONE-WAY DOOR: it checks the response sequence immediately after posting
    /// the request, which the child cannot possibly have answered yet, so it
    /// could never succeed and consecutiveTimeouts_ (reset only on success)
    /// could never fall back below the threshold. The bridge went silent for
    /// the rest of the session. Every kRetrySyncEveryBlocks-th block now takes
    /// the full synchronous wait again so a child that was merely BUSY can be
    /// found healthy, and a poll answered while collect() waits for another
    /// seat ends the stretch too — see the comment in collect().
    uint32_t pollOnlyBlocks_ = 0;

    /// ~0.7 s at 128 frames / 48 kHz. Long enough that a genuinely wedged child
    /// costs the audio thread almost nothing, short enough that recovery is
    /// inaudible as anything worse than a brief drop-in.
    static constexpr uint32_t kRetrySyncEveryBlocks = 256;

    std::atomic<uint32_t> processed_{0};
    std::atomic<uint32_t> timeouts_{0};
    std::atomic<uint32_t> midiTruncated_{0};
};

} // namespace dotcharter
