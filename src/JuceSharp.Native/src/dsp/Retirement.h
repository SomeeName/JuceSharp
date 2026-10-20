#pragma once

// Retirement queue: destroy a snapshot the audio thread may still be holding,
// on a thread that is ALLOWED to free.
//
// The shape this exists for. A module publishes some heavy immutable state
// through an atomic shared_ptr; the callback copies it for the duration of one
// block; a message/worker thread swaps in a replacement. Whoever drops the LAST
// reference runs the destructor — and if that is the callback, the audio thread
// frees (and here, for NamAmpModule, runs a whole NeuralAudio model destructor).
//
// DspChain solves the same problem with a wait-one-block BARRIER: publish, take
// the mutex the callback holds for the whole block, then drop. A module that
// does not own the callback's mutex cannot do that, and the thing it must NOT
// do instead is what NamAmpModule did — spin for a bounded time on
// `use_count() > 1` and then reset anyway. That is not a fallback: it is the
// exact case the bound exists for, and it hands the free to the audio thread
// precisely when the audio thread is slow, which is when it can least afford
// it. It also blocked the MESSAGE thread for up to 500 ms clearing a model.
//
// So: park the retired object here and sweep from a thread that may block.
// `use_count() == 1` is a sound test in this one direction — the object has
// ALREADY been swapped out of the atomic slot before it is retired, so no
// reader can take a NEW reference; the count can therefore only fall, and
// seeing 1 means the queue holds the only one. (The same reasoning DspChain
// relies on for its retired order snapshot. It would NOT be sound for a
// pointer still reachable by readers.)
//
// Nothing here runs on the audio thread. The audio thread's only involvement is
// dropping its own copy, which is a refcount decrement and never a free.

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace dcdsp {

template <typename T>
class RetirementQueue
{
public:
    RetirementQueue() = default;
    RetirementQueue(const RetirementQueue&)            = delete;
    RetirementQueue& operator=(const RetirementQueue&) = delete;

    /// A LEAK BEATS A FREE ON THE AUDIO THREAD. Ordinary member destruction of
    /// `items_` would drop a reference that may not be the last one — and if it
    /// is not, the last one is the CALLBACK's, which re-creates the exact "the
    /// audio thread runs a NeuralAudio destructor" failure this class exists to
    /// eliminate. The only sound guarantee available here is the one
    /// `NativeHandleOwner` already set on the managed side: leak it, count it,
    /// and let somebody see the count. Anything still held is deliberately
    /// released into a never-freed allocation.
    ~RetirementQueue()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& item : items_)
        {
            if (item == nullptr) continue;
            if (item.use_count() > 1)
            {
                // Intentionally never deleted: the reader still owns a
                // reference and we cannot know when it drops it.
                new std::shared_ptr<T>(std::move(item));
                leaked_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        items_.clear();
        pending_.store(0, std::memory_order_release);
    }

    /// Snapshots leaked by a queue destroyed while a reader still held one.
    /// Non-zero means somebody destroyed a publisher without draining it — the
    /// audio was safe, the memory was not reclaimed. Test/diagnostic hook.
    static unsigned long leakedForTest() noexcept
    { return leaked_.load(std::memory_order_relaxed); }

    /// Parked items past which `retire()` warns. The queue is unbounded by
    /// design (its only sweeper is a worker that can be blocked inside a cold
    /// model parse for seconds, and blocking the retiring thread instead would
    /// be worse), so the bound is a SIGNAL, not a cap: a user arrowing through a
    /// model list parks one whole capture per selection with nothing reclaiming
    /// them until the parse returns.
    static constexpr int kSoftCap = 16;

    /// Has a retire() landed with `pending()` past kSoftCap since the last
    /// call? NOT edge-triggered: retire() raises the flag on EVERY retire past
    /// the cap, so inside one excursion a caller that polls between retires
    /// sees it again and again. A caller that wants one log line per excursion
    /// latches on its side — NamAmpModule does, and its test pins this exact
    /// behaviour (this comment claimed an edge-trigger until 2026-09-11).
    bool takeOverflowWarning() noexcept
    { return overflow_.exchange(false, std::memory_order_acq_rel); }

    /// Message or worker thread. Takes ownership of a snapshot that has ALREADY
    /// been swapped out of whatever atomic slot the audio thread reads, so no
    /// new reference to it can be taken. Never call this from the callback.
    void retire(std::shared_ptr<T>&& old)
    {
        if (old == nullptr) return;
        std::lock_guard<std::mutex> lk(mutex_);
        items_.push_back(std::move(old));
        pending_.store((int) items_.size(), std::memory_order_release);
        if ((int) items_.size() > kSoftCap) overflow_.store(true, std::memory_order_release);
    }

    /// Worker (or any thread that may block and free). Destroys every parked
    /// object the audio thread has finished with; leaves the rest for the next
    /// sweep. Returns how many are still waiting.
    int sweep()
    {
        // Destroyed OUTSIDE the lock: a NeuralAudio model destructor is not
        // something to run while holding a mutex a UI thread also takes.
        std::vector<std::shared_ptr<T>> dying;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (std::size_t i = items_.size(); i-- > 0;)
            {
                if (items_[i].use_count() == 1)
                {
                    dying.push_back(std::move(items_[i]));
                    items_.erase(items_.begin() + (std::ptrdiff_t) i);
                }
            }
            pending_.store((int) items_.size(), std::memory_order_release);
        }
        dying.clear();
        return pending_.load(std::memory_order_acquire);
    }

    /// Lock-free "is there anything to sweep", so a worker's wait predicate
    /// does not have to take this queue's mutex.
    int pending() const noexcept { return pending_.load(std::memory_order_acquire); }

private:
    mutable std::mutex              mutex_;
    std::vector<std::shared_ptr<T>> items_;
    std::atomic<int>                pending_{ 0 };
    std::atomic<bool>               overflow_{ false };
    static inline std::atomic<unsigned long> leaked_{ 0 };
};

} // namespace dcdsp
