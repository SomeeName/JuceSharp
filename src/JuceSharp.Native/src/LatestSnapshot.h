#pragma once
#include <atomic>
#include <utility>

namespace dotcharter {
// One serialized publisher and one consumer. Each owns a slot exclusively;
// the middle slot transfers ownership, never references to a live reader slot.
template<class T> class LatestSnapshot
{
public:
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    void publish(const T& value) noexcept
    {
        slots_[back_] = value;
        back_ = middle_.exchange(back_ | dirty, std::memory_order_acq_rel) & mask;
    }
    bool consume() noexcept
    {
        if ((middle_.load(std::memory_order_acquire) & dirty) == 0) return false;
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & mask;
        return true;
    }
    const T& current() const noexcept { return slots_[front_]; }
private:
    static constexpr unsigned dirty = 4, mask = 3;
    T slots_[3] {};
    unsigned front_ = 0, back_ = 2;
    std::atomic<unsigned> middle_ { 1 };
};
}
