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
/// enable()/disable() are called from managed worker threads (never the
/// audio thread) and are serialized by the caller (AudioEngine holds its
/// input-chain barrier while swapping). process() runs on the audio thread
/// only and performs one bounded round trip:
///   write input + MIDI -> signal request -> spin <= ONE block -> read back.
/// On timeout the block passes through DRY and the timeout counter bumps;
/// after three consecutive misses the wait degrades to a non-blocking poll
/// so a dead child costs (almost) nothing per callback until it returns.
///
/// The wait is a user-mode spin on responseSeq, NOT a timed kernel wait: a
/// WaitForSingleObject timeout rounds up to the system timer tick (~15.6 ms),
/// which is several block budgets, so "wait 2 ms" actually slept past the
/// deadline it was meant to protect (finding E-1, measured at 20.5 ms by the
/// native RT gate). See the comment in process() for the full story.
class RemoteVstBridge
{
public:
    RemoteVstBridge();
    ~RemoteVstBridge() { disable(); }

    RemoteVstBridge(const RemoteVstBridge&)            = delete;
    RemoteVstBridge& operator=(const RemoteVstBridge&) = delete;

    /// Create (or reuse) the named mapping + events and arm the bridge.
    /// Returns false when any kernel object could not be created.
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

    /// One audio-block round trip. `chainBuf` carries the (gained) live
    /// input in `numSamples` frames across kMaxChannels channels; on success
    /// with an active chain it is replaced with the child's processed output.
    /// `midi`/`midiCount` are the packed instrument events for this block and
    /// `midiOffsets[i]` the sample offset of `midi[i]` inside it (bridge v3);
    /// a null `midiOffsets` places every event at sample 0. An offset at or past
    /// `numSamples` is clamped to the last sample. `blockStartFrame` is the
    /// render-clock frame of sample 0, published to the child alongside.
    /// `instrOut`, when non-null after the call returns true with
    /// instrumentActive, points at the child's rendered instrument block
    /// (valid until the next process call).
    /// Returns true when a fresh response was received in time.
    bool process(juce::AudioBuffer<float>& chainBuf, int numSamples,
                 const uint32_t* midi, const uint32_t* midiOffsets, int midiCount,
                 int64_t blockStartFrame,
                 const float** instrOutL, const float** instrOutR,
                 bool& chainActive, bool& instrumentActive) noexcept;

    uint32_t processedBlocks()    const noexcept { return processed_.load(std::memory_order_relaxed); }
    uint32_t timeoutBlocks()      const noexcept { return timeouts_.load(std::memory_order_relaxed); }
    /// MIDI events discarded because the shared mapping's kMaxMidiEvents slots
    /// were full. Every other drop path in this engine is counted; this one was
    /// a bare std::min with nothing said anywhere.
    uint32_t midiTruncated()      const noexcept { return midiTruncated_.load(std::memory_order_relaxed); }
    uint32_t lastHeartbeat()      const noexcept;

private:
    /// Releases the kernel objects. Caller must hold viewMutex_.
    void closeHandlesLocked();

    std::atomic<bool> enabled_{false};

    /// Guards view_'s LIFETIME against the accessors that do NOT run under the
    /// AudioEngine's remoteBridgeMutex_ barrier — setSampleRate() (device
    /// thread, on every device change) and lastHeartbeat() (polled by the
    /// managed sandbox health timer). Both dereferenced view_ behind nothing
    /// but a null-check, so disable()'s UnmapViewOfFile could land between the
    /// check and the read and they would touch unmapped memory.
    ///
    /// process() deliberately does NOT take this: it runs on the audio thread
    /// and is already covered by remoteBridgeMutex_, which the disable path
    /// holds across the unmap. Locking here too would put a second mutex on
    /// the real-time path for no added safety.
    mutable std::mutex viewMutex_;

    // Kernel objects (Windows). Type-erased so the header stays portable.
    void* mapping_   = nullptr;
    void* view_      = nullptr;
    void* reqEvent_  = nullptr;
    void* respEvent_ = nullptr;

    // Audio-thread-only state.
    uint32_t seq_ = 0;
    int      consecutiveTimeouts_ = 0;

    /// QueryPerformanceFrequency is FIXED for the life of the process
    /// (documented since Windows XP SP2), and process() was calling it on every
    /// block that had to wait — on the audio thread, for a constant. Read once
    /// at construction. 1 is a safe floor: it only ever divides, so a bad value
    /// would shorten the spin rather than hang it.
    long long qpcFreq_ = 1;

    /// Blocks spent in poll-only since it latched. Poll-only used to be a
    /// ONE-WAY DOOR: it checks the response sequence immediately after posting
    /// the request, which the child cannot possibly have answered yet, so it
    /// could never succeed and consecutiveTimeouts_ (reset only on success)
    /// could never fall back below the threshold. The bridge went silent for
    /// the rest of the session. Every kRetrySyncEveryBlocks-th block now takes
    /// the full synchronous wait again so a child that was merely BUSY can be
    /// found healthy — see the comment in process().
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
