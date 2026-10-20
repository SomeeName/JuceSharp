// dcdsp_detail_fir.h — private direct-convolution FIR helpers shared by the
// IR-based cores (cab, acousticsim). Header-only, RT-safe: allocation only in
// prepare(); processSample() never allocates, locks, or throws.
//
// The convolution is a straight dot product over CONTIGUOUS memory: the input
// history is a double-write ring (every sample stored at pos and pos+maxTaps)
// so the newest `taps` samples are always addressable as one ascending run,
// and the IR is stored time-REVERSED so no per-tap index arithmetic remains.
// MSVC's default /fp:precise refuses to vectorize float reductions (it may not
// reassociate), so the dot product uses explicit SSE2 intrinsics on x86-64 —
// that ISA is the build's baseline, no extra arch flags needed. 2048 taps at
// 48 kHz is ~200 MFLOP/s per channel, a few percent of one core with SSE.
// That figure is PER CHANNEL PER IR and the rig stacks them: dualcab runs two
// CabCores, acousticsim adds a third convolution, kMaxTaps is 4096 (used above
// 96 kHz, dcdsp_cab.h), and a 4-seat co-op rig multiplies all of it by four.
//
// W4-D67 named two ways out and BOTH were measured (2026-09-11).
//
// TAKEN: the runtime AVX2 + FMA dispatch below. The SSE2 baseline is still
// what a pre-Haswell machine runs; the kernel is chosen once at load time.
// Measured end to end in a single-process A/B over the real cores, ns per
// sample per stereo frame at the shipped 2,048-tap IR:
//     cab      289.30 -> 161.62   (-44.1 %)
//     dualcab  333.17 -> 182.02   (-45.4 %)
//
// NOT TAKEN: uniform-partitioned FFT convolution. The arithmetic is real —
// measured ns/sample, direct vs a partition-per-block overlap-save FFT:
//       taps   block 128   block 256   block 480   block 512
//        256        0.67        0.55        0.54        0.51   (FFT LOSES)
//        512        0.93        1.03        0.76        0.94   (a wash)
//       1024        1.47        1.69        1.64        1.76
//       2048        2.11        2.81        2.85        3.23
//       4096        2.60        3.92        4.77        5.32
//   (ratio = direct / FFT; > 1 means the FFT is faster)
// so the CROSSOVER is between 512 and 1,024 taps at every block size the
// engine uses, and at the shipped 2,048 taps the FFT would be ~2.9x at a
// 480-sample block — a real win, and about 2/3 of what the AVX2 kernel above
// already delivers, on top of it.
//
// It is not taken because of LATENCY and BLOCK SIZE, not arithmetic. Uniform
// partitioning is zero-latency only while the partition equals the call size,
// and `process()` is called with any numFrames <= the prepared maximum: the
// offline exports and half the test suite do not use the device's block size.
// Making it correct for a varying call size means either (a) a fixed partition
// with an input FIFO, which costs `partition` samples — 10 ms at 480, on the
// stage every amp-sim player monitors through, against a core whose
// latencySamples() is 0 today — or (b) re-seeding the frequency-domain
// partitions when the call size changes, which is an O(N log N) burst on the
// callback and precisely what the RT gate exists to stop. The answer that has
// neither problem is a NON-UNIFORM (Gardner) partitioning, small blocks first;
// that is a subsystem with its own latency contract, its own bit-closeness
// budget and its own RT proof, not a change to this dot product. Do not start
// it from this comment alone.
//
// A further ~3x is therefore available on the cab and is the right next thing
// to reach for when a seat-count increase makes it the budget.
#ifndef DCDSP_DETAIL_FIR_H
#define DCDSP_DETAIL_FIR_H

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstddef>

#if defined(_M_X64) || defined(__x86_64__)
  #include <emmintrin.h>
  #define DCDSP_FIR_SSE2 1
  // AVX2 + FMA is a RUNTIME choice, not a build one: the shipped binary must
  // keep running on a pre-Haswell machine, so the wide kernel is compiled in
  // and selected by CPUID. MSVC emits VEX for these intrinsics without /arch;
  // GCC and Clang need the function marked.
  #include <immintrin.h>
  #define DCDSP_FIR_AVX2 1
  #if defined(_MSC_VER)
    #include <intrin.h>
    #define DCDSP_FIR_TARGET_AVX2
  #else
    #define DCDSP_FIR_TARGET_AVX2 __attribute__((target("avx2,fma")))
  #endif
#endif

namespace dcdsp {
namespace detail {

// Dot product of two length-n float runs (n arbitrary, pointers unaligned).
inline float firDotSse2(const float* a, const float* b, int n) {
#ifdef DCDSP_FIR_SSE2
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    __m128 s2 = _mm_setzero_ps(), s3 = _mm_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i),      _mm_loadu_ps(b + i)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a + i + 4),  _mm_loadu_ps(b + i + 4)));
        s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_loadu_ps(a + i + 8),  _mm_loadu_ps(b + i + 8)));
        s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_loadu_ps(a + i + 12), _mm_loadu_ps(b + i + 12)));
    }
    __m128 s = _mm_add_ps(_mm_add_ps(s0, s1), _mm_add_ps(s2, s3));
    for (; i + 4 <= n; i += 4)
        s = _mm_add_ps(s, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 0x1));
    float acc = _mm_cvtss_f32(s);
    for (; i < n; ++i) acc += a[i] * b[i];
    return acc;
#else
    float a0 = 0.f, a1 = 0.f, a2 = 0.f, a3 = 0.f;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        a0 += a[i] * b[i];     a1 += a[i + 1] * b[i + 1];
        a2 += a[i + 2] * b[i + 2]; a3 += a[i + 3] * b[i + 3];
    }
    float acc = (a0 + a1) + (a2 + a3);
    for (; i < n; ++i) acc += a[i] * b[i];
    return acc;
#endif
}

#ifdef DCDSP_FIR_AVX2
// The same reduction 8 lanes wide with FMA.  Four accumulators again: the
// dependency chain, not the multiply, is what limits this loop.
DCDSP_FIR_TARGET_AVX2
inline float firDotAvx2(const float* a, const float* b, int n) {
    __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
    __m256 s2 = _mm256_setzero_ps(), s3 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 32 <= n; i += 32) {
        s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i),      _mm256_loadu_ps(b + i),      s0);
        s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8),  _mm256_loadu_ps(b + i + 8),  s1);
        s2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), s2);
        s3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), s3);
    }
    __m256 s = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
    for (; i + 8 <= n; i += 8)
        s = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s);
    __m128 t = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
    t = _mm_add_ps(t, _mm_movehl_ps(t, t));
    t = _mm_add_ss(t, _mm_shuffle_ps(t, t, 0x1));
    float acc = _mm_cvtss_f32(t);
    for (; i < n; ++i) acc += a[i] * b[i];
    return acc;
}

// AVX2 + FMA + the OS actually saving YMM state (a kernel that does not is a
// silent corrupter, which is what the XGETBV check is for).
inline bool firCpuHasAvx2() {
 #if defined(_MSC_VER)
    int regs[4] = { 0, 0, 0, 0 };
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;
    __cpuid(regs, 1);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx     = (regs[2] & (1 << 28)) != 0;
    const bool fma     = (regs[2] & (1 << 12)) != 0;
    if (!osxsave || !avx || !fma) return false;
    if ((_xgetbv(0) & 0x6) != 0x6) return false;     // XMM + YMM saved
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;                // AVX2
 #else
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
 #endif
}
#endif  // DCDSP_FIR_AVX2

using FirDotFn = float (*)(const float*, const float*, int);

inline FirDotFn firPickDot() {
#ifdef DCDSP_FIR_AVX2
    if (firCpuHasAvx2()) return &firDotAvx2;
#endif
    return &firDotSse2;
}

// Chosen ONCE, at load time.  A C++11 magic static would put a guard
// acquire on the audio thread's first call, and this is called per sample per
// channel; an inline variable is dynamically initialised before main.
inline const FirDotFn kFirDot = firPickDot();

// Dot product of two length-n float runs (n arbitrary, pointers unaligned).
//
// MEASURED on this machine (MSVC 19.51 /O2 x64), ns per call:
//    taps    SSE2     AVX2+FMA   ratio
//     512   44.00       25.64     1.72
//    1024   93.45       44.94     2.08
//    2048  195.90       83.23     2.35     <- the shipped cabinet IR length
//    4096  375.61      184.19     2.04     <- kMaxTaps, used above 96 kHz
// The two differ by at most 9.537e-07 absolute on a full-length kernel: FMA
// and a wider tree change the ROUNDING ORDER of the sum, not the sum.
inline float firDot(const float* a, const float* b, int n) {
    return kFirDot(a, b, n);
}

// Per-channel FIR state (the IR itself lives in the owning core, shared by
// all channels).
class FirConvolver {
public:
    void prepare(int maxTaps) {
        maxTaps_ = std::max(1, maxTaps);
        hist_.assign(static_cast<std::size_t>(maxTaps_) * 2, 0.f);
        pos_ = 0;
    }
    void reset() {
        std::fill(hist_.begin(), hist_.end(), 0.f);
        pos_ = 0;
    }
    // revTaps points at the IR stored time-reversed (revTaps[j] = h[n-1-j]);
    // n <= maxTaps passed to prepare().
    float processSample(float x, const float* revTaps, int n) {
        hist_[static_cast<std::size_t>(pos_)] = x;
        hist_[static_cast<std::size_t>(pos_) + maxTaps_] = x;
        const float* w = hist_.data() + pos_ + maxTaps_ - (n - 1);
        const float y = firDot(w, revTaps, n);
        if (++pos_ >= maxTaps_) pos_ = 0;
        return y;
    }

private:
    std::vector<float> hist_;   // double-write ring, size 2*maxTaps
    int maxTaps_ = 1;
    int pos_     = 0;
};

// Lanczos-3 resample of a 48 kHz-authored IR to the engine rate. Prepare-time
// only (O(len*7) — never on the audio path). Taps are scaled by srcRate/
// dstRate so the filter's magnitude response (e.g. its DC gain) is preserved
// across the rate change. Returns the tap count written (<= dstCap; at very
// high rates the tail is truncated to the cap, which only sheds the quietest
// end of the IR).
inline int resampleIr(const float* src, int srcLen, double srcRate,
                      float* dst, int dstCap, double dstRate) {
    if (srcLen <= 0 || dstCap <= 0) return 0;
    if (std::fabs(dstRate - srcRate) < 1.0) {
        const int n = std::min(srcLen, dstCap);
        std::copy(src, src + n, dst);
        return n;
    }
    const double step  = srcRate / dstRate;
    const double scale = step;
    const int outLen = std::min(dstCap,
        static_cast<int>(std::ceil(static_cast<double>(srcLen) / step)));
    auto lanczos3 = [](double x) -> double {
        if (x == 0.0) return 1.0;
        const double ax = std::fabs(x);
        if (ax >= 3.0) return 0.0;
        const double px = 3.14159265358979323846 * x;
        return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
    };
    for (int t = 0; t < outLen; ++t) {
        const double srcPos = t * step;
        const int i0 = static_cast<int>(std::floor(srcPos)) - 2;
        double acc = 0.0;
        for (int k = 0; k < 6; ++k) {
            const int idx = i0 + k;
            if (idx < 0 || idx >= srcLen) continue;
            acc += static_cast<double>(src[idx]) * lanczos3(srcPos - idx);
        }
        dst[t] = static_cast<float>(acc * scale);
    }
    return outLen;
}

} // namespace detail
} // namespace dcdsp

#endif // DCDSP_DETAIL_FIR_H
