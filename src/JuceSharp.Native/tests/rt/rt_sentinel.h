#pragma once

// Real-time violation sentinel for the native RT gate.
//
// WHAT IT CATCHES AND WHY THE HOOK IS WHAT IT IS
// ----------------------------------------------
// PRIMARY (Windows, MSVC debug CRT): _CrtSetAllocHook. In the debug CRT
// `operator new`, `malloc`, `calloc`, `realloc` and `_aligned_malloc` are all
// implemented on top of the debug heap, so ONE hook covers every one of them.
// That matters: juce::HeapBlock and juce::AudioBuffer::setSize go through
// malloc, NOT operator new — a global `operator new` replacement alone would
// miss the entire class of bug this harness exists to catch ("never resize a
// scratch buffer on the callback").
//
// SECONDARY (portable, non-MSVC or release CRT): replacing the eight global
// operator new / operator delete overloads. This catches std::vector,
// std::string, make_shared and juce::dsp::IIR::Coefficients, but NOT
// HeapBlock / AudioBuffer. IT IS NOT EQUIVALENT COVERAGE. The gating
// configuration is and must stay the debug-CRT one; the fallback exists so the
// sources still build and still catch *something* elsewhere.
//
// Recording is allocation-free by construction: fixed arrays plus a
// fetch_add index. The hook would otherwise recurse into itself.
//
// ARMING. g_rtDepth is thread_local and is raised by ScopedRtThread around the
// *test's* pump call, never inside engine code. Everything the engine does
// inside the device callback runs on that thread, so the flag is exact; worker
// threads (namamp model load, the read-ahead TimeSliceThread, the test's own
// param-storm thread) have depth 0 and are ignored.

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace dcrt {

inline constexpr int kMaxHeapViolations = 512;
inline constexpr int kMaxMarks          = 1 << 19;
inline constexpr int kMaxFrames         = 16;

enum class Kind : unsigned char { Alloc = 0, Realloc, Free, Lock, Wait };

const char* kindName(Kind k) noexcept;

/// A heap event observed while the audio thread was armed.
struct HeapViolation
{
    Kind         kind       = Kind::Alloc;
    std::size_t  bytes      = 0;
    unsigned     block      = 0;
    int          frameCount = 0;
    void*        frames[kMaxFrames]{};
};

/// A lock acquisition or blocking wait observed on the audio thread.
struct Mark
{
    Kind        kind  = Kind::Lock;
    const char* site  = nullptr;   // static string; never owned, never allocated
    long long   ns    = 0;
    unsigned    block = 0;
};

// --- arming ---------------------------------------------------------------

extern thread_local int              g_rtDepth;
extern std::atomic<unsigned>         g_currentBlock;
extern std::atomic<bool>             g_armed;

/// Install the allocation hook. Idempotent; call once at process start.
/// Returns false when no malloc-level hook is available (release CRT), in
/// which case only the operator-new fallback is active.
bool install() noexcept;
/// True when the malloc-level (debug CRT) hook is the active mechanism.
bool hasMallocHook() noexcept;

void arm()   noexcept;
void disarm() noexcept;
/// Clear every recorded violation and mark. Not RT-safe; call between phases.
void reset() noexcept;

/// RAII "we are on the audio thread" flag, plus the per-block counter.
struct ScopedRtThread
{
    ScopedRtThread() noexcept { ++g_rtDepth; }
    ~ScopedRtThread() noexcept { --g_rtDepth; }
    ScopedRtThread(const ScopedRtThread&)            = delete;
    ScopedRtThread& operator=(const ScopedRtThread&) = delete;
};

// --- queries (all off the RT path) ----------------------------------------

int  heapViolationCount() noexcept;
int  allocCount() noexcept;         // Alloc + Realloc
int  freeCount()  noexcept;
int  markCount()  noexcept;

int  countAt(Kind kind, const char* site) noexcept;
/// nth-percentile of the recorded ns for (kind, site). 0 when no samples.
long long percentileNs(Kind kind, const char* site, double pct) noexcept;
long long maxNs(Kind kind, const char* site) noexcept;

/// The recorded ns for (kind, site), sorted ascending. For a probe that
/// gathers samples across several reset() windows (the recorder holds only
/// kMaxMarks marks) and computes its percentile over the union.
std::vector<long long> samplesNs(Kind kind, const char* site);
/// The same percentile percentileNs() computes, over a SORTED sample set.
long long percentileOf(const std::vector<long long>& sorted, double pct) noexcept;

/// Distinct site names observed for `kind`, sorted. Allocates — off-RT only.
std::vector<std::string> observedSites(Kind kind);

/// Human-readable table of the heap violations recorded so far, with symbols
/// resolved through DbgHelp. Returns "" when there are none.
std::string formatHeapViolations();

/// Baseline keys ("ALLOC <symbol>" / "WAIT <site>") for everything recorded.
std::vector<std::string> violationKeys();

} // namespace dcrt
