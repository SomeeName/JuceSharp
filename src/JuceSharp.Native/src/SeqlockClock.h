#pragma once

#include <atomic>
#include <cstdint>

namespace dotcharter {

/// A {count, host ticks} pair published by ONE writer thread (the audio
/// callback) and read torn-free by any thread: the render clock (ABI v26) and
/// each seat's capture sync pair (ABI v16).
///
/// THE PAYLOAD IS ATOMIC. A reader that sampled an even sequence can still be
/// overlapped by the next publish while it copies the pair; with plain fields
/// that copy is a data race, which is undefined behaviour no matter what the
/// re-check concludes afterwards. Relaxed atomic payload plus two fences is the
/// race-free seqlock:
///   - writer: odd sequence, RELEASE FENCE, payload, even sequence (release);
///   - reader: sequence (acquire), payload, ACQUIRE FENCE, sequence again.
/// A reader whose payload loads saw any value of a newer publish therefore sees
/// the sequence move on its second load, and retries.
///
/// reset() is a write with the same protocol. The sequence never goes back: a
/// reset that rewound it to 0 let reset + publish bring an even value a reader
/// had already sampled back around a DIFFERENT payload, and that reader's
/// re-check passed on a torn pair. "Nothing published" is part of the payload
/// (`published_`), not a sequence value.
///
/// Wait-free and allocation-free on both sides.
///
/// `Seam` is a test seam: `Seam::betweenPayloadLoads()` runs inside read()
/// between its two payload loads, so a test can stop a reader there and run
/// the writer side in the window. The default does nothing.
struct NoSeqlockSeam { static void betweenPayloadLoads() noexcept {} };

template <typename Count, typename Seam = NoSeqlockSeam>
class SeqlockClock
{
public:
    static_assert(std::atomic<Count>::is_always_lock_free,   "the count must be a lock-free atomic");
    static_assert(std::atomic<int64_t>::is_always_lock_free, "the ticks must be a lock-free atomic");
    static_assert(std::atomic<bool>::is_always_lock_free,    "the published flag must be a lock-free atomic");

    /// Writer thread only.
    void publish(Count count, int64_t ticks) noexcept
    {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        published_.store(true, std::memory_order_relaxed);
        count_.store(count, std::memory_order_relaxed);
        ticks_.store(ticks, std::memory_order_relaxed);
        seq_.store(s + 2u, std::memory_order_release);
    }

    /// Any thread. False when nothing was published since the last reset, or
    /// when every attempt overlapped a publish.
    bool read(Count& outCount, int64_t& outTicks) const noexcept
    {
        for (int attempt = 0; attempt < kReadAttempts; ++attempt)
        {
            const uint32_t before = seq_.load(std::memory_order_acquire);
            if (before == 0) return false;          // never written
            if ((before & 1u) != 0) continue;       // writer in flight
            const bool    valid = published_.load(std::memory_order_relaxed);
            const Count   count = count_.load(std::memory_order_relaxed);
            Seam::betweenPayloadLoads();
            const int64_t ticks = ticks_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == before)
            {
                if (! valid) return false;          // reset since the last publish
                outCount = count;
                outTicks = ticks;
                return true;
            }
        }
        return false;
    }

    /// Back to "nothing published", as a WRITE: odd sequence, release fence,
    /// payload, next even sequence, exactly like publish(). Readers may run.
    /// Callers: the writer thread, or another thread while no publish can run
    /// (it would be a second writer otherwise). The one call site is
    /// AudioEngine::resetRenderClock, from audioDeviceAboutToStart, with no
    /// callback in flight; the capture sync pairs are never reset.
    ///
    /// ORDERING REQUIREMENT for a reset from a thread other than the writer:
    /// the sequence load below is relaxed, so the last publish of the previous
    /// writer must already HAPPEN-BEFORE this call, or the reset could read a
    /// stale even sequence and store a value a reader has already seen. The
    /// device stop / start sequence provides that edge: the driver joins or
    /// signals the stopped callback thread before audioDeviceAboutToStart runs
    /// (JUCE's device lock and the driver's own stop handshake), so every
    /// publish of the last callback is visible here.
    void reset() noexcept
    {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        published_.store(false, std::memory_order_relaxed);
        count_.store(Count{}, std::memory_order_relaxed);
        ticks_.store(0, std::memory_order_relaxed);
        seq_.store(s + 2u, std::memory_order_release);
    }

    /// The count of the last publish (zero after a reset). For the WRITER
    /// thread, which reads back its own value; other threads use read().
    Count lastCount() const noexcept { return count_.load(std::memory_order_relaxed); }

private:
    static constexpr int kReadAttempts = 8;

    std::atomic<uint32_t> seq_   { 0 };
    std::atomic<bool>     published_ { false };
    std::atomic<Count>    count_ { Count{} };
    std::atomic<int64_t>  ticks_ { 0 };
};

} // namespace dotcharter
