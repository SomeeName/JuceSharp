#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
 #define DCLOAD_SSE 1
 #include <xmmintrin.h>
#else
 #define DCLOAD_SSE 0
#endif

// LOAD-TIME file -> device sample-rate conversion. Offline only; nothing here
// ever runs on the audio callback.
//
// WHY THIS HEADER EXISTS
// ----------------------
// juce::ResamplingAudioSource -- what AudioTransportSource uses when it is
// given a sourceSampleRateToCorrectFor -- is LINEAR interpolation plus one
// 2nd-order Butterworth, and a 44.1 kHz backing track on a 48 kHz interface
// (the ordinary case: backing tracks are 44.1 kHz, guitar interfaces run
// 48 kHz) measured -3.14 dB at 15 kHz against the same tone in a 48 kHz file.
// So the whole decoded buffer is converted ONCE, offline, on the load thread.
//
// The first version of that did it with a juce::WindowedSincInterpolator, and
// the quality was right (-0.088 dB at 15 kHz) but the COST was not: the owner's
// 2026-08-29 log has every source_open_file of a 183 s stereo 44.1 kHz file
// taking ~6.4 s, so a 6-stem project spent ~39 s in preload alone. JUCE's
// kernel is 201 taps per output sample, each tap two lookup-table loads plus a
// linear interpolation between them, inside a loop carrying a running sinc
// position, a sign and a wrapping circular index. It cannot vectorize and it
// recomputes the same 201 coefficients for every output sample even though at
// a FIXED rate ratio there are only L distinct coefficient sets in the whole
// file.
//
// A rational resampler exploits exactly that. 44.1 -> 48 kHz is 147:160
// EXACTLY, so there are 160 phases; the prototype (one 100-crossing windowed
// sinc at 160x the input rate) is built once at load and split into those 160
// phases, and each output sample is then ONE dot product of ~201 contiguous
// input samples with its phase's ~201 contiguous coefficients. No table
// interpolation, no branches, no state -- and, because every output sample is
// independent, the work splits freely across threads.
//
// THREE THINGS ARE LOAD-BEARING and each is pinned by
// RtPreloadedSourceResamplesAtLoadWithoutShiftingTime:
//
//   * BOTH DIRECTIONS, CUTOFF AT THE LOWER NYQUIST (JuceSharp #33). The
//     prototype cuts at kCutoffScale x min(1, L/M) x the input Nyquist, so an
//     UPsample cuts at the input Nyquist (nothing above it exists) and a
//     DOWNsample at the OUTPUT Nyquist (anything above it would fold back).
//     48 -> 44.1 kHz is exactly 147:160. A downsample's kernel is M/L times
//     wider in input samples (halfTapsFor), so it keeps ~100 zero crossings
//     at the OUTPUT rate and with them the same transition band.
//   * THE GROUP DELAY IS EXACTLY ZERO BY CONSTRUCTION. A windowed sinc is
//     centred, and not compensating puts the whole song ~2.3 ms LATE against
//     the chart -- a worse bug than the dull top end this fixes. Here the
//     prototype's centre is placed at a whole multiple of L (kTapsPerPhase is
//     even), so output sample n reads input time exactly n*M/L and there is no
//     latency to trim. The windowed-sinc fallback still trims explicitly.
//   * THE CALLER HOLDS THE POST-CONVERSION SIZE. Upsampling grows the buffer
//     (+8.8 % at 44.1 -> 48), so TransportSource reserves against the grown
//     size, not the decoded one.
//
// The kernel deliberately mirrors JUCE's: 100 zero crossings either side and a
// cutoff at 0.99 x the (lower) Nyquist (JUCE's own lookup table's first zero is
// at 0.99 input samples, so that 0.99 is not an invention here). The window is
// a Kaiser -- JUCE's fits beta ~= 6; this uses 8.6 for a deeper stopband, which
// costs nothing when the tap count is already fixed at 201.
namespace dcload
{

/// Half-width of the kernel in INPUT samples -- JUCE's WindowedSincInterpolator
/// uses 100 crossings and this must not be quietly reduced: it is what the
/// -0.088 dB at 15 kHz was measured with.
inline constexpr int kCrossings = 100;

/// Taps per polyphase branch. EVEN on purpose: the prototype centre then lands
/// on a whole multiple of L, which is what makes the group delay an exact
/// integer number of input samples (i.e. zero output-sample shift).
inline constexpr int kTapsPerPhase = 2 * kCrossings;

/// Cutoff as a fraction of the input Nyquist. JUCE's table zeroes at 0.99.
inline constexpr double kCutoffScale = 0.99;

/// Kaiser beta. 8.6 is the classic ~-90 dB stopband point.
inline constexpr double kKaiserBeta = 8.6;

/// Refuse a ratio that would need a silly number of phases: the coefficient
/// table is phases * (kTapsPerPhase + 1) floats, so 1024 phases is ~823 KB.
/// Every real device/file pair is far below this (44.1->48 is 160).
inline constexpr int kMaxPhases = 1024;

/// Refuse a downsample by more than this: the kernel's width grows by M/L, so
/// 8:1 is already 1,601 taps per output sample. Every real case is far below
/// (96 -> 44.1 kHz is 147:320, ~2.2).
inline constexpr int kMaxDecimation = 8;

/// Output frames below which threading is pure overhead.
inline constexpr int kMinFramesPerChunk = 1 << 15;

/// What actually happened, for the log line and for the RT scenario's
/// structural assertion that the fast path was TAKEN.
/// Which converter a source's audio actually goes through (JuceSharp #33).
enum class Converter : int
{
    None = 0,            ///< file and device rates match, or no device: nothing converts
    Polyphase,           ///< offline, at load, the rational kernel
    WindowedSinc,        ///< offline, at load, JUCE's windowed sinc (upsample fallback)
    StreamingPolyphase,  ///< per block, PolyphaseStreamSource -- the same kernel
    TransportLinear,     ///< per block, juce::ResamplingAudioSource (linear)
};

inline const char* converterName(Converter c) noexcept
{
    switch (c)
    {
        case Converter::None:               return "none";
        case Converter::Polyphase:          return "polyphase";
        case Converter::WindowedSinc:       return "windowed-sinc";
        case Converter::StreamingPolyphase: return "streaming-polyphase";
        case Converter::TransportLinear:    return "transport-linear";
    }
    return "?";
}

struct ResampleReport
{
    bool   polyphase    = false;   ///< a rational kernel ran (offline or streaming)
    bool   converted    = false;   ///< the ENGINE's own converter handles the rate
                                   ///< (at load or per block); false for None
                                   ///< and for TransportLinear
    Converter converter = Converter::None;
    double fromRate     = 0.0;     ///< the file's rate
    double toRate       = 0.0;     ///< the device rate it was converted for
    int    interpolate  = 0;       ///< L (output-side factor)
    int    decimate     = 0;       ///< M (input-side factor)
    int    tapsPerPhase = 0;
    int    threads      = 0;
    double elapsedMs    = 0.0;
};

/// Modified Bessel function of the first kind, order 0. Series form: the
/// argument here is at most kKaiserBeta, so it converges in ~20 terms.
inline double besselI0(double x) noexcept
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k)
    {
        const double t = x / (2.0 * (double) k);
        term *= t * t;
        sum  += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

inline double normalisedSinc(double x) noexcept
{
    if (std::abs(x) < 1e-12) return 1.0;
    const double px = juce::MathConstants<double>::pi * x;
    return std::sin(px) / px;
}

/// Reduce fromRate:toRate to an exact integer ratio L:M (L output samples per
/// M input samples). Returns false unless BOTH rates are (near) integers and
/// DIFFER, L stays inside kMaxPhases and a downsample is at most
/// kMaxDecimation:1. 44.1k -> 48k comes out 160:147; 48k -> 96k 2:1; the
/// downsample 48k -> 44.1k 147:160.
inline bool reduceRatio(double fromRate, double toRate, int& lOut, int& mOut) noexcept
{
    lOut = mOut = 0;
    if (! (fromRate > 0.0) || ! (toRate > 0.0)) return false;
    if (fromRate > 1.0e7 || toRate > 1.0e7) return false;

    const long long fromI = (long long) std::llround(fromRate);
    const long long toI   = (long long) std::llround(toRate);
    // A non-integer device or file rate is real (pull-up/pull-down rates exist)
    // and is exactly the case the fallback is for.
    if (std::abs(fromRate - (double) fromI) > 1e-6) return false;
    if (std::abs(toRate   - (double) toI)   > 1e-6) return false;
    if (fromI <= 0 || toI <= 0 || toI == fromI) return false;

    long long a = toI, b = fromI;
    while (b != 0) { const long long t = a % b; a = b; b = t; }
    const long long g = a;                       // gcd, > 0

    const long long l = toI / g;                 // phases
    const long long m = fromI / g;
    if (l <= 0 || m <= 0 || l > (long long) kMaxPhases) return false;
    if (m > l * (long long) kMaxDecimation) return false;

    lOut = (int) l;
    mOut = (int) m;
    return true;
}

/// Half-width of the kernel in INPUT samples for L:M. An upsample keeps
/// kCrossings exactly (and so the very kernel it always had); a downsample
/// widens by M/L so the kernel still spans ~kCrossings zero crossings of the
/// OUTPUT-rate cutoff -- the sinc's zeros are 1/cutoff input samples apart.
inline int halfTapsFor(int l, int m) noexcept
{
    if (l <= 0 || m <= l) return kCrossings;
    return (int) std::ceil((double) kCrossings * (double) m / (double) l);
}

/// Cutoff as a fraction of the INPUT Nyquist: kCutoffScale x min(1, L/M).
inline double cutoffFor(int l, int m) noexcept
{
    if (l <= 0 || m <= l) return kCutoffScale;
    return kCutoffScale * (double) l / (double) m;
}

/// The prototype split into L phases, laid out so one output sample is one
/// FORWARD dot product over contiguous memory.
///
/// Derivation (this is the whole algorithm):
///   upsample by L  -> u[k*L] = x[k], zero elsewhere
///   filter with h  -> v[i]   = sum_k h[i - k*L] * x[k]
///   decimate by M  -> y[n]   = v[n*M]
/// With H = halfTapsFor(L, M) and T = 2H (EVEN, so the centre below is a
/// whole number of input samples), put the prototype's centre at c = H * L --
/// a whole multiple of L -- and evaluate at i = n*M + c so y[n] is centred on
/// input time n*M/L with NO residual delay. With q = n*M, p = q mod L and
/// base = q/L - H:
///   y[n] = sum_{s=0}^{T} coeff[p][s] * x[base + s]
/// where coeff[p][s] = h[p + (T - s) * L] (zero past the prototype's end).
/// Nothing in that derivation assumes M < L; only the cutoff and the width
/// depend on the direction.
class PolyphaseKernel
{
public:
    /// Returns false only on allocation failure or a nonsensical ratio. `m`
    /// matters only for a DOWNSAMPLE (m > phases): an upsample kernel depends
    /// on L alone and is bit-identical to the one built before #33.
    bool build(int phases, int m = 1)
    {
        if (phases <= 0 || phases > kMaxPhases || m <= 0) return false;
        if (m > phases * kMaxDecimation) return false;

        phases_ = phases;
        half_   = halfTapsFor(phases, m);
        taps_   = 2 * half_ + 1;                 // s = 0..T inclusive
        stride_ = (taps_ + 7) & ~7;              // keep each phase 8-float aligned

        const int    span     = 2 * half_;       // T
        const int    protoLen = span * phases_ + 1;
        const double centre   = (double) (half_ * phases_);
        const double cutoff   = cutoffFor(phases, m);
        const double width    = (double) half_;

        std::vector<double> proto;
        try { proto.resize((size_t) protoLen); }
        catch (const std::bad_alloc&) { return false; }

        const double i0beta = besselI0(kKaiserBeta);
        for (int j = 0; j < protoLen; ++j)
        {
            // Offset from the centre in INPUT samples.
            const double t = ((double) j - centre) / (double) phases_;
            const double w = std::abs(t) >= width
                ? 0.0
                : besselI0(kKaiserBeta * std::sqrt(1.0 - (t / width)
                                                       * (t / width))) / i0beta;
            proto[(size_t) j] = normalisedSinc(cutoff * t) * w;
        }

        try { coeff_.assign((size_t) phases_ * (size_t) stride_, 0.0f); }
        catch (const std::bad_alloc&) { return false; }

        for (int p = 0; p < phases_; ++p)
        {
            double sum = 0.0;
            for (int s = 0; s < taps_; ++s)
            {
                const long long idx = (long long) p
                                    + (long long) (span - s) * (long long) phases_;
                const double v = (idx >= 0 && idx < protoLen) ? proto[(size_t) idx] : 0.0;
                coeff_[(size_t) p * (size_t) stride_ + (size_t) s] = (float) v;
                sum += v;
            }
            // Per-phase DC normalisation. Without it each phase has a slightly
            // different DC gain, which is an amplitude modulation at the phase
            // cycle rate rather than a harmless overall level error.
            if (std::abs(sum) > 1e-12)
            {
                const float g = (float) (1.0 / sum);
                float* row = &coeff_[(size_t) p * (size_t) stride_];
                for (int s = 0; s < taps_; ++s) row[s] *= g;
            }
        }

        return true;
    }

    int phases() const noexcept { return phases_; }
    int taps()   const noexcept { return taps_; }
    /// H: a window starts H input samples before its output's centre.
    int halfTaps() const noexcept { return half_; }
    const float* phase(int p) const noexcept { return &coeff_[(size_t) p * (size_t) stride_]; }

private:
    int phases_ = 0;
    int half_   = 0;
    int taps_   = 0;
    int stride_ = 0;
    std::vector<float> coeff_;
};

/// n taps, contiguous, no alignment assumption. Two accumulators so the two
/// dependent add chains interleave; the DLL is built for the SSE2 baseline
/// (see CMakeLists "no /arch bump here"), so this stays 4-wide on purpose.
inline float dotProduct(const float* a, const float* b, int n) noexcept
{
#if DCLOAD_SSE
    __m128 s0 = _mm_setzero_ps();
    __m128 s1 = _mm_setzero_ps();
    int i = 0;
    for (; i + 8 <= n; i += 8)
    {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i),     _mm_loadu_ps(b + i)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
    }
    for (; i + 4 <= n; i += 4)
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));

    __m128 s = _mm_add_ps(s0, s1);
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, s);
    float acc = (lanes[0] + lanes[2]) + (lanes[1] + lanes[3]);
    for (; i < n; ++i) acc += a[i] * b[i];
    return acc;
#else
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        a0 += a[i]     * b[i];
        a1 += a[i + 1] * b[i + 1];
        a2 += a[i + 2] * b[i + 2];
        a3 += a[i + 3] * b[i + 3];
    }
    float acc = (a0 + a2) + (a1 + a3);
    for (; i < n; ++i) acc += a[i] * b[i];
    return acc;
#endif
}

/// Number of output frames a conversion of `inFrames` at L:M yields. Kept
/// identical to what the windowed-sinc path produced so the preload budget,
/// the reported length in seconds and every seek are unchanged.
inline long long outputFrameCount(long long inFrames, int l, int m) noexcept
{
    if (inFrames <= 0 || l <= 0 || m <= 0) return 0;
    return (long long) std::llround((double) inFrames * (double) l / (double) m);
}

/// One channel, output frames [begin, end). Reads outside the input read as
/// zero (never as a wrap of the song's tail onto its head).
inline void resampleChannelRange(const PolyphaseKernel& k, int l, int m,
                                 const float* in, long long inFrames,
                                 float* out, long long begin, long long end) noexcept
{
    const int taps = k.taps();
    const int half = k.halfTaps();
    // q += m per output, carried as whole input samples plus phase. An
    // upsample has mWhole == 0, so this is the one-compare loop it always was.
    const int mWhole = m / l;
    const int mFrac  = m - mWhole * l;

    // Where the whole tap window is inside the input, so the inner loop needs
    // no bounds checks at all.
    long long q    = begin * (long long) m;
    long long i    = q / (long long) l;
    int       p    = (int) (q - i * (long long) l);
    long long base = i - half;

    for (long long n = begin; n < end; ++n)
    {
        const float* c = k.phase(p);

        if (base >= 0 && base + taps <= inFrames)
        {
            out[n] = dotProduct(c, in + base, taps);
        }
        else
        {
            float acc = 0.0f;
            const long long s0 = std::max<long long>(0, -base);
            const long long s1 = std::min<long long>((long long) taps, inFrames - base);
            for (long long s = s0; s < s1; ++s)
                acc += c[(size_t) s] * in[base + s];
            out[n] = acc;
        }

        base += mWhole;
        p    += mFrac;
        if (p >= l) { p -= l; ++base; }
    }
}

/// The kernel for L:M, built once and shared (W4-N17). build(160) evaluates a
/// 32,001-point prototype -- a sin and a besselI0 series per point -- then
/// normalises 160 phases, and it depends on NOTHING but L (and, for a
/// downsample, M) and the compile-time constants. It was rebuilt per CALL, so
/// a six-stem project paid for it six times.
///
/// A function-local static behind a mutex: callers are LOAD threads, which
/// this file's own header says may allocate, and stems load in parallel so the
/// map genuinely needs the lock. The kernel is handed out by shared_ptr and
/// never mutated after build, so worker threads -- and a streaming stage on
/// the audio thread, which holds its own reference (PolyphaseStreamSource) --
/// read it without holding anything. Never call this from the audio callback.
/// Null on allocation failure or an unsupported ratio.
inline std::shared_ptr<const PolyphaseKernel> acquireKernel(int l, int m)
{
    // An upsample kernel is the same for every M: key it on L alone, as before.
    const long long key = ((long long) l << 32) | (long long) (m > l ? m : 0);

    static std::mutex                                                  kernelMutex;
    static std::map<long long, std::shared_ptr<const PolyphaseKernel>> kernelCache;

    std::lock_guard<std::mutex> lock(kernelMutex);
    if (auto it = kernelCache.find(key); it != kernelCache.end())
        return it->second;

    std::shared_ptr<PolyphaseKernel> built;
    try { built = std::make_shared<PolyphaseKernel>(); }
    catch (const std::bad_alloc&) { return nullptr; }
    if (! built->build(l, m)) return nullptr;

    // Bounded: kMaxPhases caps L at 1024 and a real session sees one or two
    // distinct ratios, but a cache with no bound is a leak with a slow fuse.
    // Clear rather than evict -- rebuilding is 30 ms, and an LRU here would be
    // more machinery than the thing it manages. Holders keep their kernel.
    if (kernelCache.size() >= 8) kernelCache.clear();
    try { kernelCache.emplace(key, built); }
    catch (const std::bad_alloc&) { /* uncached is still correct */ }
    return built;
}

/// The rational-polyphase conversion, in place. Returns false when the ratio is
/// not one this path can honour -- the caller then falls back.
///
/// Runs on the LOAD thread, so allocating and spawning threads here is fine;
/// nothing in this file may ever be reached from the audio callback.
inline bool resampleBufferPolyphase(juce::AudioBuffer<float>& buf,
                                    double fromRate, double toRate,
                                    ResampleReport* report = nullptr,
                                    int maxThreads = 4)
{
    int l = 0, m = 0;
    if (! reduceRatio(fromRate, toRate, l, m)) return false;

    const long long inFrames = (long long) buf.getNumSamples();
    const int       channels = buf.getNumChannels();
    if (inFrames <= 0 || channels <= 0) return false;

    const long long wanted = outputFrameCount(inFrames, l, m);
    if (wanted <= 0 || wanted > (long long) std::numeric_limits<int>::max()) return false;
    const int outFrames = (int) wanted;

    const auto t0 = std::chrono::steady_clock::now();

    // CACHED (W4-N17) -- see acquireKernel.
    const std::shared_ptr<const PolyphaseKernel> kernelRef = acquireKernel(l, m);
    if (kernelRef == nullptr) return false;
    const PolyphaseKernel& kernel = *kernelRef;

    juce::AudioBuffer<float> out;
    try { out.setSize(channels, outFrames, false, true, false); }
    catch (const std::bad_alloc&) { return false; }

    // Chunk the work: every output sample is independent of every other, so
    // this parallelises across CHANNELS and, for a long file, within one.
    struct Chunk { int channel; long long begin, end; };
    std::vector<Chunk> chunks;
    const long long perChunk = std::max<long long>(kMinFramesPerChunk,
                                                   (outFrames + 7) / 8);
    try
    {
        for (int c = 0; c < channels; ++c)
            for (long long b = 0; b < outFrames; b += perChunk)
                chunks.push_back({ c, b, std::min<long long>(outFrames, b + perChunk) });
    }
    catch (const std::bad_alloc&) { return false; }

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    int threads = (int) std::min<unsigned>(hw, (unsigned) std::max(1, maxThreads));
    threads = std::min<int>(threads, (int) chunks.size());
    threads = std::max(1, threads);

    std::atomic<size_t> next{ 0 };
    auto worker = [&]() noexcept
    {
        for (;;)
        {
            const size_t idx = next.fetch_add(1, std::memory_order_relaxed);
            if (idx >= chunks.size()) return;
            const Chunk& ch = chunks[idx];
            resampleChannelRange(kernel, l, m,
                                 buf.getReadPointer(ch.channel), inFrames,
                                 out.getWritePointer(ch.channel), ch.begin, ch.end);
        }
    };

    std::vector<std::thread> pool;
    try
    {
        pool.reserve((size_t) threads - 1);
        for (int t = 1; t < threads; ++t) pool.emplace_back(worker);
    }
    catch (...)
    {
        // Out of threads: the calling thread still finishes every chunk below.
    }
    worker();
    for (auto& t : pool) if (t.joinable()) t.join();

    buf = std::move(out);

    if (report != nullptr)
    {
        report->polyphase    = true;
        report->converted    = true;
        report->converter    = Converter::Polyphase;
        report->fromRate     = fromRate;
        report->toRate       = toRate;
        report->interpolate  = l;
        report->decimate     = m;
        report->tapsPerPhase = kernel.taps();
        report->threads      = 1 + (int) pool.size();
        report->elapsedMs    = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count();
    }
    return true;
}

/// The original conversion: one juce::WindowedSincInterpolator per channel,
/// group delay trimmed by hand. Kept as the fallback for any upsample ratio the
/// rational path refuses (a non-integer rate, or one needing > kMaxPhases
/// phases), and as the REFERENCE the RT scenario compares the fast path
/// against.
inline bool resampleBufferWindowedSinc(juce::AudioBuffer<float>& buf,
                                       double fromRate, double toRate,
                                       ResampleReport* report = nullptr)
{
    if (! (fromRate > 0.0) || ! (toRate > fromRate)) return false;

    const double ratio    = fromRate / toRate;   // INPUT samples per output sample
    const int    inFrames = buf.getNumSamples();
    const int    channels = buf.getNumChannels();
    if (inFrames <= 0 || channels <= 0) return false;

    const long long wanted = (long long) std::llround((double) inFrames / ratio);
    if (wanted <= 0 || wanted > (long long) std::numeric_limits<int>::max()) return false;
    const int outFrames = (int) wanted;

    const auto t0 = std::chrono::steady_clock::now();

    // The kernel is symmetric and centred, so its output is delayed by
    // getBaseLatency() INPUT samples; in output samples that is /ratio.
    juce::WindowedSincInterpolator probe;
    const int latencyOut = (int) std::llround(probe.getBaseLatency() / ratio);
    const int produce    = outFrames + latencyOut;

    juce::AudioBuffer<float> out;
    try { out.setSize(channels, outFrames, false, true, false); }
    catch (const std::bad_alloc&) { return false; }

    std::vector<float> scratch;
    try { scratch.assign((size_t) produce, 0.0f); }
    catch (const std::bad_alloc&) { return false; }

    for (int c = 0; c < channels; ++c)
    {
        juce::WindowedSincInterpolator interp;
        interp.reset();
        // wrapAround 0 => feed zeroes past the end of the input rather than
        // wrapping the tail of the song onto its head.
        interp.process(ratio, buf.getReadPointer(c), scratch.data(),
                       produce, inFrames, 0);
        std::memcpy(out.getWritePointer(c), scratch.data() + latencyOut,
                    sizeof(float) * (size_t) outFrames);
    }

    buf = std::move(out);

    if (report != nullptr)
    {
        report->polyphase    = false;
        report->converted    = true;
        report->converter    = Converter::WindowedSinc;
        report->fromRate     = fromRate;
        report->toRate       = toRate;
        report->interpolate  = 0;
        report->decimate     = 0;
        report->tapsPerPhase = 2 * kCrossings + 1;
        report->threads      = 1;
        report->elapsedMs    = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count();
    }
    return true;
}

/// Whether the load converts a file at `fromRate` for a device at `toRate`:
/// any upsample (the windowed sinc catches what the rational path refuses),
/// and a downsample whose ratio the rational path accepts. A downsample it
/// refuses (a non-integer rate, more than kMaxPhases phases or more than
/// kMaxDecimation:1) has no offline converter and stays on the transport.
inline bool loadCanConvert(double fromRate, double toRate) noexcept
{
    if (! (fromRate > 0.0) || ! (toRate > 0.0) || fromRate == toRate) return false;
    if (toRate > fromRate) return true;
    int l = 0, m = 0;
    return reduceRatio(fromRate, toRate, l, m);
}

/// Convert an already-decoded buffer in place, in EITHER direction (#33): the
/// rational path when the ratio allows it, the windowed sinc for an upsample
/// it refuses. False (buffer untouched) when neither applies.
inline bool convertDecodedBuffer(juce::AudioBuffer<float>& buf,
                                 double fromRate, double toRate,
                                 ResampleReport* report = nullptr)
{
    if (! loadCanConvert(fromRate, toRate)) return false;
    if (resampleBufferPolyphase(buf, fromRate, toRate, report)) return true;
    if (toRate > fromRate) return resampleBufferWindowedSinc(buf, fromRate, toRate, report);
    return false;
}

/// UPSAMPLE an already-decoded buffer in place: the rational path when the
/// ratio allows it, the windowed sinc otherwise. dca_convert_audio_for_device
/// (ABI v43) stays on this one: its contract writes nothing for a file at or
/// above the device rate.
inline bool upsampleDecodedBuffer(juce::AudioBuffer<float>& buf,
                                  double fromRate, double toRate,
                                  ResampleReport* report = nullptr)
{
    if (! (fromRate > 0.0) || ! (toRate > fromRate)) return false;
    return convertDecodedBuffer(buf, fromRate, toRate, report);
}

} // namespace dcload
