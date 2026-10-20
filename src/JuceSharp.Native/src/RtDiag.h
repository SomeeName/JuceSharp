#pragma once

// Real-time diagnostic marks for the audio-thread paths.
//
// These exist so the native RT gate (tests/rt, target `juce_sharp_rtcheck`)
// can answer "which locks does the audio callback take, and how long does it
// wait for them?" with a NUMBER instead of a code-review opinion — and so a
// newly added lock on the callback path fails a build instead of being
// discovered by ear.
//
// THE SHIPPED DLL MUST NOT CONTAIN ANY OF THIS. `DCA_RTCHECK` is defined only
// on the rtcheck test target; the `juce_sharp` target never sees it, so every
// macro below degrades to exactly the code that was there before:
//
//   DCA_RT_LOCK_GUARD(m, "name")  ->  std::lock_guard<std::mutex> dcaRtGuard_(m)
//   DCA_RT_WAIT_SCOPE("name")     ->  ((void) 0)
//
// i.e. identical codegen, identical section sizes, no new string literals.
//
// Why DCA_RT_LOCK_GUARD replaces the `std::lock_guard` line rather than
// sitting next to it: the metric that matters is the ACQUIRE wait, and that
// can only be measured by owning the acquisition. A separate marker placed
// before the guard would either measure nothing (marker after) or the whole
// critical section (marker before, because the guard is destroyed first).
// The instrumented form times a `try_lock` fast path with a blocking
// `lock()` fallback, so an uncontended acquire costs one `try_lock` and a
// contended one reports the real stall.

#if defined(DCA_RTCHECK) && DCA_RTCHECK

 #include <mutex>

namespace dcrt {

/// Record a lock acquisition on the audio thread: `site` is a static string,
/// `waitNs` the measured time spent acquiring. Never allocates.
void noteLock(const char* site, long long waitNs) noexcept;
/// Record a blocking kernel wait on the audio thread (bridge round trip).
void noteWait(const char* site, long long waitNs) noexcept;

/// std::lock_guard that measures its own acquire time. Same ownership
/// semantics; only used in rtcheck builds.
class TimedLockGuard
{
public:
    TimedLockGuard(std::mutex& m, const char* site) noexcept;
    ~TimedLockGuard() noexcept { m_.unlock(); }

    TimedLockGuard(const TimedLockGuard&)            = delete;
    TimedLockGuard& operator=(const TimedLockGuard&) = delete;

private:
    std::mutex& m_;
};

/// Times the scope it is declared in and reports it as a blocking wait.
class ScopedWaitMark
{
public:
    explicit ScopedWaitMark(const char* site) noexcept;
    ~ScopedWaitMark() noexcept;

    ScopedWaitMark(const ScopedWaitMark&)            = delete;
    ScopedWaitMark& operator=(const ScopedWaitMark&) = delete;

private:
    const char* site_;
    long long   startTicks_;
};

} // namespace dcrt

 #define DCA_RT_LOCK_GUARD(mutexExpr, siteName) \
     ::dcrt::TimedLockGuard dcaRtGuard_((mutexExpr), (siteName))
 #define DCA_RT_WAIT_SCOPE(siteName) \
     ::dcrt::ScopedWaitMark dcaRtWait_((siteName))

#else

 #include <mutex>

 #define DCA_RT_LOCK_GUARD(mutexExpr, siteName) \
     std::lock_guard<std::mutex> dcaRtGuard_((mutexExpr))
 #define DCA_RT_WAIT_SCOPE(siteName) ((void) 0)

#endif
