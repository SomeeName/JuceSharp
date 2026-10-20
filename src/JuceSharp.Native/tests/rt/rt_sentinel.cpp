#include "rt_sentinel.h"

#include "RtDiag.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
 #include <dbghelp.h>
#endif

#if defined(_DEBUG) && defined(_MSC_VER)
 #include <crtdbg.h>
 #define DCRT_HAVE_CRT_ALLOC_HOOK 1
#else
 #define DCRT_HAVE_CRT_ALLOC_HOOK 0
#endif

namespace dcrt {

thread_local int      g_rtDepth = 0;
std::atomic<unsigned> g_currentBlock{ 0 };
std::atomic<bool>     g_armed{ false };

namespace {

HeapViolation      g_heap[kMaxHeapViolations];
std::atomic<int>   g_heapCount{ 0 };
std::atomic<int>   g_heapDropped{ 0 };

Mark               g_marks[kMaxMarks];
std::atomic<int>   g_markCount{ 0 };

std::atomic<bool>  g_installed{ false };
std::atomic<bool>  g_mallocHook{ false };

/// Reentrancy guard: the recorder itself must never be re-entered through the
/// hook (RtlCaptureStackBackTrace does not allocate, but be defensive).
thread_local bool g_inRecorder = false;

long long qpcFrequency() noexcept
{
#if defined(_WIN32)
    static const long long f = []
    {
        LARGE_INTEGER li{};
        ::QueryPerformanceFrequency(&li);
        return (long long) li.QuadPart;
    }();
    return f;
#else
    return 1000000000LL;
#endif
}

long long qpcNow() noexcept
{
#if defined(_WIN32)
    LARGE_INTEGER li{};
    ::QueryPerformanceCounter(&li);
    return (long long) li.QuadPart;
#else
    return 0;
#endif
}

long long ticksToNs(long long ticks) noexcept
{
    const long long f = qpcFrequency();
    if (f <= 0) return 0;
    return (long long) (((double) ticks * 1.0e9) / (double) f);
}

/// Fixed-array, wait-free. Never allocates.
void recordHeap(Kind kind, std::size_t bytes) noexcept
{
    if (g_inRecorder) return;
    g_inRecorder = true;

    const int idx = g_heapCount.fetch_add(1, std::memory_order_acq_rel);
    if (idx < kMaxHeapViolations)
    {
        auto& v = g_heap[idx];
        v.kind  = kind;
        v.bytes = bytes;
        v.block = g_currentBlock.load(std::memory_order_relaxed);
#if defined(_WIN32)
        // Skip 2 frames (this function + the hook thunk). No allocation.
        v.frameCount = (int) ::RtlCaptureStackBackTrace(2, kMaxFrames, v.frames, nullptr);
#else
        v.frameCount = 0;
#endif
    }
    else
    {
        g_heapCount.store(kMaxHeapViolations, std::memory_order_release);
        g_heapDropped.fetch_add(1, std::memory_order_relaxed);
    }

    g_inRecorder = false;
}

void recordMark(Kind kind, const char* site, long long ns) noexcept
{
    const int idx = g_markCount.fetch_add(1, std::memory_order_acq_rel);
    if (idx < kMaxMarks)
    {
        auto& m = g_marks[idx];
        m.kind  = kind;
        m.site  = site;
        m.ns    = ns;
        m.block = g_currentBlock.load(std::memory_order_relaxed);
    }
    else
    {
        g_markCount.store(kMaxMarks, std::memory_order_release);
    }
}

#if DCRT_HAVE_CRT_ALLOC_HOOK
int __cdecl allocHook(int allocType, void* /*userData*/, std::size_t size,
                      int blockType, long /*requestNumber*/,
                      const unsigned char* /*filename*/, int /*line*/)
{
    // Always allow the allocation — we observe, we never fail it. Failing it
    // would turn an RT-safety report into a crash and hide everything after.
    if (g_rtDepth <= 0) return TRUE;
    if (! g_armed.load(std::memory_order_relaxed)) return TRUE;
    if (_BLOCK_TYPE(blockType) == _CRT_BLOCK) return TRUE;   // CRT-internal noise

    switch (allocType)
    {
        case _HOOK_ALLOC:   recordHeap(Kind::Alloc,   size); break;
        case _HOOK_REALLOC: recordHeap(Kind::Realloc, size); break;
        case _HOOK_FREE:    recordHeap(Kind::Free,    0);    break;
        default: break;
    }
    return TRUE;
}
#endif

} // namespace

const char* kindName(Kind k) noexcept
{
    switch (k)
    {
        case Kind::Alloc:   return "ALLOC";
        case Kind::Realloc: return "REALLOC";
        case Kind::Free:    return "FREE";
        case Kind::Lock:    return "LOCK";
        case Kind::Wait:    return "WAIT";
    }
    return "?";
}

bool install() noexcept
{
    if (g_installed.exchange(true, std::memory_order_acq_rel))
        return g_mallocHook.load(std::memory_order_acquire);

#if DCRT_HAVE_CRT_ALLOC_HOOK
    _CrtSetAllocHook(&allocHook);
    g_mallocHook.store(true, std::memory_order_release);
    return true;
#else
    g_mallocHook.store(false, std::memory_order_release);
    return false;
#endif
}

bool hasMallocHook() noexcept { return g_mallocHook.load(std::memory_order_acquire); }

void arm() noexcept    { g_armed.store(true,  std::memory_order_release); }
void disarm() noexcept { g_armed.store(false, std::memory_order_release); }

void reset() noexcept
{
    g_heapCount.store(0, std::memory_order_release);
    g_heapDropped.store(0, std::memory_order_release);
    g_markCount.store(0, std::memory_order_release);
}

int heapViolationCount() noexcept
{
    return std::min(g_heapCount.load(std::memory_order_acquire), kMaxHeapViolations);
}

int markCount() noexcept
{
    return std::min(g_markCount.load(std::memory_order_acquire), kMaxMarks);
}

int allocCount() noexcept
{
    int n = 0;
    const int total = heapViolationCount();
    for (int i = 0; i < total; ++i)
        if (g_heap[i].kind == Kind::Alloc || g_heap[i].kind == Kind::Realloc) ++n;
    return n;
}

int freeCount() noexcept
{
    int n = 0;
    const int total = heapViolationCount();
    for (int i = 0; i < total; ++i)
        if (g_heap[i].kind == Kind::Free) ++n;
    return n;
}

int countAt(Kind kind, const char* site) noexcept
{
    int n = 0;
    const int total = markCount();
    for (int i = 0; i < total; ++i)
        if (g_marks[i].kind == kind && g_marks[i].site != nullptr
            && std::strcmp(g_marks[i].site, site) == 0) ++n;
    return n;
}

namespace {

std::vector<long long> samplesFor(Kind kind, const char* site)
{
    std::vector<long long> out;
    const int total = markCount();
    out.reserve((size_t) total / 4 + 8);
    for (int i = 0; i < total; ++i)
        if (g_marks[i].kind == kind && g_marks[i].site != nullptr
            && std::strcmp(g_marks[i].site, site) == 0)
            out.push_back(g_marks[i].ns);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

long long percentileOf(const std::vector<long long>& s, double pct) noexcept
{
    if (s.empty()) return 0;
    const double p = std::clamp(pct, 0.0, 1.0);
    size_t idx = (size_t) (p * (double) (s.size() - 1) + 0.5);
    if (idx >= s.size()) idx = s.size() - 1;
    return s[idx];
}

long long percentileNs(Kind kind, const char* site, double pct) noexcept
{
    return percentileOf(samplesFor(kind, site), pct);
}

std::vector<long long> samplesNs(Kind kind, const char* site)
{
    return samplesFor(kind, site);
}

long long maxNs(Kind kind, const char* site) noexcept
{
    auto s = samplesFor(kind, site);
    return s.empty() ? 0 : s.back();
}

std::vector<std::string> observedSites(Kind kind)
{
    std::vector<std::string> out;
    const int total = markCount();
    for (int i = 0; i < total; ++i)
    {
        if (g_marks[i].kind != kind || g_marks[i].site == nullptr) continue;
        std::string s(g_marks[i].site);
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// --- symbolisation --------------------------------------------------------

namespace {

#if defined(_WIN32)
std::once_flag g_symOnce;
bool           g_symReady = false;

void initSymbols()
{
    std::call_once(g_symOnce, []
    {
        ::SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        g_symReady = ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE) != FALSE;
    });
}

/// Resolve the first frame that carries source-line information — i.e. the
/// first frame in code we built with a PDB. Frames inside the CRT / system
/// DLLs have no lines here and are exactly the ones we want to skip.
std::string resolveSite(const HeapViolation& v)
{
    initSymbols();
    if (! g_symReady) return "<unresolved>";

    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512]{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 500;

    std::string fallback;
    for (int i = 0; i < v.frameCount; ++i)
    {
        const DWORD64 addr = (DWORD64) v.frames[i];
        DWORD64 disp64 = 0;
        const bool haveSym = ::SymFromAddr(::GetCurrentProcess(), addr, &disp64, sym) != FALSE;

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD dispLine = 0;
        if (::SymGetLineFromAddr64(::GetCurrentProcess(), addr, &dispLine, &line) && line.FileName != nullptr)
        {
            const char* base = std::strrchr(line.FileName, '\\');
            const char* file = base != nullptr ? base + 1 : line.FileName;
            std::string s = std::string(file) + ":" + std::to_string((unsigned long) line.LineNumber);
            if (haveSym) s += " (" + std::string(sym->Name) + ")";
            return s;
        }
        if (haveSym && fallback.empty()) fallback = sym->Name;
    }
    return fallback.empty() ? "<unresolved>" : fallback;
}
#else
std::string resolveSite(const HeapViolation&) { return "<unresolved>"; }
#endif

} // namespace

std::string formatHeapViolations()
{
    const int total = heapViolationCount();
    if (total == 0) return {};

    struct Row { std::string key; std::size_t bytes; unsigned firstBlock; int count; };
    std::vector<Row> rows;
    for (int i = 0; i < total; ++i)
    {
        std::string key = std::string(kindName(g_heap[i].kind)) + "  " + resolveSite(g_heap[i]);
        auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.key == key; });
        if (it == rows.end())
            rows.push_back({ std::move(key), g_heap[i].bytes, g_heap[i].block, 1 });
        else
            ++it->count;
    }

    std::string out = "  KIND     SITE                                                   BYTES    BLOCK  COUNT\n";
    char line[512];
    for (const auto& r : rows)
    {
        std::snprintf(line, sizeof(line), "  %-70s %8zu %8u %6d\n",
                      r.key.c_str(), r.bytes, r.firstBlock, r.count);
        out += line;
    }
    const int dropped = g_heapDropped.load(std::memory_order_acquire);
    if (dropped > 0)
        out += "  (+" + std::to_string(dropped) + " more, recorder capacity exhausted)\n";
    return out;
}

std::vector<std::string> violationKeys()
{
    std::vector<std::string> keys;
    auto add = [&keys](std::string k)
    {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(std::move(k));
    };

    const int total = heapViolationCount();
    for (int i = 0; i < total; ++i)
        add(std::string(kindName(g_heap[i].kind)) + " " + resolveSite(g_heap[i]));

    // A blocking kernel wait on the audio thread is a violation in its own
    // right; lock sites are governed by rt_lock_sites.txt instead.
    for (auto& s : observedSites(Kind::Wait))
        add("WAIT " + s);

    return keys;
}

// --- RtDiag.h back end ----------------------------------------------------

void noteLock(const char* site, long long waitNs) noexcept
{
    if (g_rtDepth <= 0 || ! g_armed.load(std::memory_order_relaxed)) return;
    recordMark(Kind::Lock, site, waitNs);
}

void noteWait(const char* site, long long waitNs) noexcept
{
    if (g_rtDepth <= 0 || ! g_armed.load(std::memory_order_relaxed)) return;
    recordMark(Kind::Wait, site, waitNs);
}

TimedLockGuard::TimedLockGuard(std::mutex& m, const char* site) noexcept
    : m_(m)
{
    // try_lock first so an uncontended acquire costs one atomic and reports a
    // near-zero wait; fall back to a blocking lock() and time the stall.
    const long long t0 = qpcNow();
    if (! m_.try_lock())
        m_.lock();
    noteLock(site, ticksToNs(qpcNow() - t0));
}

ScopedWaitMark::ScopedWaitMark(const char* site) noexcept
    : site_(site), startTicks_(qpcNow())
{
}

ScopedWaitMark::~ScopedWaitMark() noexcept
{
    noteWait(site_, ticksToNs(qpcNow() - startTicks_));
}

} // namespace dcrt

// --- portable secondary hook ----------------------------------------------
//
// Only compiled when the malloc-level debug-CRT hook is NOT available. See the
// header: this catches operator new but NOT juce::HeapBlock / AudioBuffer, so
// it is strictly weaker and must never become the gating configuration.
#if !DCRT_HAVE_CRT_ALLOC_HOOK

#include <cstdlib>
#include <new>

namespace {
void dcrtNoteNew(std::size_t n) noexcept
{
    // recordHeap lives in this TU's anonymous namespace inside dcrt, so it is
    // reachable here by qualified name and nowhere else.
    if (dcrt::g_rtDepth > 0 && dcrt::g_armed.load(std::memory_order_relaxed))
        dcrt::recordHeap(dcrt::Kind::Alloc, n);
}
} // namespace

void* operator new(std::size_t n)                              { dcrtNoteNew(n); if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n)                            { dcrtNoteNew(n); if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept   { dcrtNoteNew(n); return std::malloc(n ? n : 1); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { dcrtNoteNew(n); return std::malloc(n ? n : 1); }
void  operator delete(void* p) noexcept                        { std::free(p); }
void  operator delete[](void* p) noexcept                      { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept           { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept         { std::free(p); }

#endif // !DCRT_HAVE_CRT_ALLOC_HOOK
