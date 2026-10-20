// dcdsp_detail_pitchgear.h — the four building blocks the 2026-09 PITCH drawer
// is made of (detune, pitchcorrect, twelvestring, basssynth). Requires
// dcdsp_primitives.h to be included BEFORE this one.
//
// Header-only, RT-safe: every buffer is sized in prepare(); process()/next()
// never allocate, lock or throw.
//
//   GrainShifter   two-tap equal-power crossfaded delay-line pitch shifter.
//   AutocorrTracker monophonic pitch tracker, normalised autocorrelation on a
//                  decimated copy.
//   LpcFormant     12th-order LPC whiten/re-dress pair (formant preservation).
//   PolyBlepOsc    band-limited saw / square / PWM / sine oscillator.
//
// THE ONE THING THAT IS EASY TO GET WRONG (each block repeats its own version
// below): none of these may STEP anything the audio path reads per sample. The
// shifter's window length is a DELAY TAP, the tracker's decimation is owned by
// the RATE and not by the knob, the LPC synthesis set must be the one that
// belongs to the audio LEAVING the pipeline, and the oscillator's increment
// must glide between notes. Each of those shipped as a splice at least once in
// the cores this file was written from.
#ifndef DCDSP_DETAIL_PITCHGEAR_H
#define DCDSP_DETAIL_PITCHGEAR_H

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {
namespace detail {

// ---------------------------------------------------------------------------
// Small shared helpers.
// ---------------------------------------------------------------------------

/// NaN-transparent clamps are the bug this repo keeps re-finding: `v < lo` and
/// `v > hi` are BOTH false for a NaN, so it sails through into a coefficient
/// and the module is silent for good. Written negated, a NaN lands on the
/// fallback.
inline float finiteOr(float v, float fallback) { return (v == v) ? v : fallback; }

inline float clampf(float v, float lo, float hi, float fallback) {
    if (!(v == v)) return fallback;     // NaN
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline int nextPow2i(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

/// Semitone/cent ratio, guarded so a runaway argument cannot produce an inf.
inline float centsToRatio(float cents) {
    cents = clampf(cents, -4800.0f, 4800.0f, 0.0f);
    return std::pow(2.0f, cents / 1200.0f);
}

// ---------------------------------------------------------------------------
// GrainShifter — the classic two-tap crossfaded delay-line pitch shifter (the
// "micro pitch" / bucket-brigade detuner topology, and the same overlap-add
// principle as dcdsp_harmonizer.h's WSOLA with the correlation search removed).
//
// TOPOLOGY. Reading a delay line whose delay d(n) RAMPS produces
// x[n - d(n)], whose instantaneous frequency is scaled by 1 - d'(n). So a
// constant ratio r is a delay ramping at d' = 1 - r. A ramp cannot run for
// ever, so it is wrapped over a window W and TWO taps a half window apart are
// crossfaded with sin/cos gains: each tap's gain is exactly ZERO at the instant
// it wraps, so the splice is inaudible, and sin^2 + cos^2 = 1 keeps the power
// constant.
//
// WHY NOT WSOLA HERE. WSOLA's cross-correlation splice search buys
// period-aligned grains, which matters for the +/- 24 semitones a whammy
// covers. This drawer's shifts are +/- 50 cents (detune, pitch correction) and
// one octave (a 12-string course), where the search costs more than it is
// worth — and the search is per-GRAIN, so its cost scales with the ratio.
//
// EASY TO GET WRONG, twice:
//  * AT RATIO EXACTLY 1 the two taps stop moving and the pair degenerates into
//    a fixed COMB (two fixed taps W/2 apart, summing coherently). Every user of
//    this class must crossfade to `readNominal()` — the same signal at the same
//    latency — as the shift approaches zero. `bypassBlend` below does the
//    arithmetic; the caller owns the (one, shared) smoother.
//  * `window` IS A DELAY TAP. Changing it moves d = preDelay + phase * W, so a
//    knob (or a latency-mode flip) that writes it directly is a splice. Pass a
//    SMOOTHED window; the ring is allocated for the worst case so a ramp can
//    never leave it.
//  * A SPLICE THAT IS NOT PERIOD-ALIGNED CANCELS THE NOTE. The two taps sit
//    half a window apart, so their outputs differ in phase by
//    2*pi*f_out*(W/2)/sr — a constant the ratio decides. At a small detune the
//    taps barely move and that hardly matters; at an OCTAVE the pair inverts
//    every wrap and the carrier is GONE (measured: a 110 Hz note shifted up an
//    octave put -141 dB at 220 Hz and all of its energy in +/- 40 Hz sidebands
//    — an audible ring-modulated mess). `alignMs` turns on a cross-correlation
//    search at each wrap, which is the one idea WSOLA exists for: splice where
//    the waveform matches, not where the clock says. It costs work only AT a
//    wrap, so its price scales with |1 - ratio| and a detuner pays nothing.
//    The search moves the tap by up to +/- alignMs, so `latencyFor` includes it
//    exactly as dcdsp_whammy.h's `latency_` includes its own search span.
// ---------------------------------------------------------------------------
class GrainShifter {
public:
    /// Allocate for the worst case: the longest window, the longest pre-delay
    /// and the alignment search span this instance will ever be asked for, at
    /// this rate. `alignMs` 0 disables the search entirely.
    void prepare(double sampleRate, double maxWindowMs, double maxPreDelayMs,
                 double alignMs = 0.0) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxWindow_ = (float) (sr_ * std::max(1.0, maxWindowMs) / 1000.0);
        maxPre_ = (float) (sr_ * std::max(0.0, maxPreDelayMs) / 1000.0);
        align_ = (int) std::lround(sr_ * std::max(0.0, alignMs) / 1000.0);
        if (align_ < 0) align_ = 0;
        const int need = (int) std::ceil((double) maxWindow_ + (double) maxPre_)
                       + 2 * align_ + 8;
        size_ = nextPow2i(std::max(need, 64));
        buf_.assign((std::size_t) size_, 0.0f);
        buildWindowTable();
        reset();
    }

    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        write_ = 0;
        phase_ = 0.0f;
        off1_ = off2_ = 0;
    }

    /// True while the ring covers the geometry the audio path can ask for. The
    /// RT gate pins this rather than trusting the comment above.
    bool ringCoversGeometry() const {
        return (int) buf_.size() == size_
            && (float) size_ > maxWindow_ + maxPre_ + 2.0f * (float) align_ + 4.0f;
    }

    /// How many window tables have been built since construction. prepare()
    /// builds exactly one (the table is a function of PHASE, so it is shared by
    /// every geometry); a knob may never raise this.
    long long windowBuilds() const { return windowBuilds_; }

    float maxWindowSamples() const { return maxWindow_; }

    /// Nominal group delay of the pair at this geometry — the mean of the two
    /// taps plus the alignment head start, and the delay `readNominal()` reads
    /// at. This is what a module reports as latencySamples(). The ALIGNED
    /// splice moves the instantaneous tap by up to +/- `alignSamples` around
    /// it, which is why that span is part of the figure rather than a footnote.
    static float latencyFor(float preDelay, float window, int alignSamples = 0) {
        return preDelay + (float) alignSamples + 0.5f * window;
    }
    int alignSamples() const { return align_; }

    /// One sample. `ratio` is the pitch multiplier, `window` the crossfade
    /// window and `preDelay` the fixed head start, both in SAMPLES and both
    /// expected to be smoothed by the caller.
    float process(float x, float ratio, float window, float preDelay) {
        if (size_ <= 0) return x;
        if (!std::isfinite(x)) x = 0.0f;
        buf_[(std::size_t) (write_ & (size_ - 1))] = x;
        ++write_;

        window = clampf(window, 8.0f, maxWindow_, maxWindow_);
        preDelay = clampf(preDelay, 0.0f, maxPre_, 0.0f);
        ratio = clampf(ratio, 0.25f, 4.0f, 1.0f);

        // d' = 1 - r over a window of W samples: one wrap every W/|1-r| samples.
        const float inc = (1.0f - ratio) / window;
        const float prev = phase_;
        const float raw = phase_ + inc;
        phase_ = raw - std::floor(raw);
        if (!(phase_ >= 0.0f && phase_ < 1.0f)) phase_ = 0.0f;   // NaN/Inf guard

        float p2 = phase_ + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;

        const float base = preDelay + (float) align_;
        float d1 = base + phase_ * window;
        float d2 = base + p2 * window;

        // A tap wraps when its own phase crosses zero — tap 1 at phase 0, tap 2
        // half a window later. At that instant its gain is zero, which is the
        // only moment its delay may be re-chosen.
        if (align_ > 0 && window > 16.0f) {
            const bool wrap1 = (raw < 0.0f) || (raw >= 1.0f);
            const bool wrap2 = std::floor(raw + 0.5f) != std::floor(prev + 0.5f);
            const int corrLen = (int) (0.5f * window);
            if (wrap1) off1_ = searchAlign(d1, d2 + (float) off2_, corrLen);
            if (wrap2) off2_ = searchAlign(d2, d1 + (float) off1_, corrLen);
        }
        d1 += (float) off1_;
        d2 += (float) off2_;
        return win(phase_) * readAt(d1) + win(p2) * readAt(d2);
    }

    /// The dry signal at the SAME latency the shifted pair has — the other half
    /// of the ratio-1 crossfade. Must be called after process() for the frame
    /// (both read the ring the same push wrote).
    float readNominal(float window, float preDelay) {
        window = clampf(window, 8.0f, maxWindow_, maxWindow_);
        preDelay = clampf(preDelay, 0.0f, maxPre_, 0.0f);
        return readAt(latencyFor(preDelay, window, align_));
    }

    /// Cross-correlation splice search. `dNew` is where the wrapping tap would
    /// land on the clock, `dRef` where the tap that is currently AUDIBLE sits;
    /// the answer is the offset, within +/- align_, that makes the two match.
    /// Coarse pass at stride 4, then a full-resolution refine — the same two-
    /// pass shape dcdsp_whammy.h's searchBestLag uses, for the same reason.
    int searchAlign(float dNew, float dRef, int corrLen) const {
        if (corrLen < 16) return 0;
        if (corrLen > kMaxCorr) corrLen = kMaxCorr;
        if (write_ < (long long) (dNew + (float) align_ + (float) corrLen + 4.0f)) return 0;

        auto scoreAt = [&](int k, int stride) -> float {
            double dot = 0.0, ea = 0.0, eb = 0.0;
            const float d = dNew + (float) k;
            for (int j = 0; j < corrLen; j += stride) {
                const double a = (double) tap((long long) (d + (float) j));
                const double b = (double) tap((long long) (dRef + (float) j));
                dot += a * b; ea += a * a; eb += b * b;
            }
            const double n = std::sqrt(ea * eb) + 1e-12;
            // Centre bias: with a periodic signal several offsets score alike,
            // and without this the tap hops between pitch periods grain to
            // grain, which is its own kind of warble.
            return (float) (dot / n) - 0.03f * (float) std::abs(k) / (float) align_;
        };

        float bestScore = -1e30f;
        int best = 0;
        for (int k = -align_; k <= align_; k += 4) {
            const float s = scoreAt(k, 4);
            if (s > bestScore) { bestScore = s; best = k; }
        }
        float refScore = -1e30f;
        int refK = best;
        const int lo = std::max(-align_, best - 4);
        const int hi = std::min(align_, best + 4);
        for (int k = lo; k <= hi; ++k) {
            const float s = scoreAt(k, 1);
            if (s > refScore) { refScore = s; refK = k; }
        }
        return refK;
    }

    /// blend = 1 -> the shifted pair, 0 -> the time-aligned dry. The caller
    /// owns the smoother so a stereo core walks it ONCE per frame.
    static float bypassBlend(float shifted, float nominal, float blend) {
        return blend * shifted + (1.0f - blend) * nominal;
    }

private:
    void buildWindowTable() {
        tbl_.assign((std::size_t) kTable + 1, 0.0f);
        for (int i = 0; i <= kTable; ++i)
            tbl_[(std::size_t) i] = (float) std::sin(kPi * (double) i / (double) kTable);
        ++windowBuilds_;
    }

    /// sin(pi * phase), table + linear interpolation. Phase-indexed, so ONE
    /// table serves every window length and a geometry change rebuilds nothing.
    float win(float phase) const {
        const float t = phase * (float) kTable;
        int i = (int) t;
        if (i < 0) i = 0;
        if (i >= kTable) i = kTable - 1;
        const float f = t - (float) i;
        const float a = tbl_[(std::size_t) i];
        return a + f * (tbl_[(std::size_t) (i + 1)] - a);
    }

    /// 4-point 3rd-order Lagrange read at a fractional delay back from the most
    /// recent push. Index-masked, so a read can never leave the ring.
    float readAt(float delay) const {
        if (delay < 1.0f) delay = 1.0f;
        const float maxD = (float) (size_ - 4);
        if (delay > maxD) delay = maxD;
        const long long i0 = (long long) std::floor(delay);
        const float f = delay - (float) i0;
        const float ym1 = tap(i0 - 1);
        const float y0 = tap(i0);
        const float y1 = tap(i0 + 1);
        const float y2 = tap(i0 + 2);
        const float fm1 = f - 1.0f, fm2 = f - 2.0f, fp1 = f + 1.0f;
        return f * fm1 * fm2 * (-1.0f / 6.0f) * ym1
             + fp1 * fm1 * fm2 * (0.5f) * y0
             + fp1 * f * fm2 * (-0.5f) * y1
             + fp1 * f * fm1 * (1.0f / 6.0f) * y2;
    }

    float tap(long long d) const {
        if (d < 0) d = 0;
        long long idx = write_ - 1 - d;
        if (idx < 0) return 0.0f;
        return buf_[(std::size_t) (idx & (long long) (size_ - 1))];
    }

    static constexpr int kTable = 2048;
    /// Cap on the correlation length, so a 192 kHz device does not pay a
    /// four-times-longer search for the same musical span.
    static constexpr int kMaxCorr = 1024;

    double sr_ = 48000.0;
    std::vector<float> buf_;
    std::vector<float> tbl_;
    int size_ = 0;
    long long write_ = 0;
    float phase_ = 0.0f;
    float maxWindow_ = 0.0f;
    float maxPre_ = 0.0f;
    int align_ = 0;
    int off1_ = 0, off2_ = 0;
    long long windowBuilds_ = 0;
};

// ---------------------------------------------------------------------------
// AutocorrTracker — monophonic pitch tracker, normalised autocorrelation over a
// DECIMATED copy of the input.
//
// WHY NOT dcdsp_detail_pitch.h's PeriodTracker: that one is a hysteresis
// zero-cross comparator, chosen because a sub-octave pedal needs the period at
// ATTACK latency. Pitch CORRECTION needs the opposite trade — a cents-accurate
// frequency, which a comparator cannot give (one sample of jitter on a 440 Hz
// period at 48 kHz is 40 cents). This pays ~40 ms of analysis window for an
// answer good to a few cents.
//
// It is MONOPHONIC-ONLY and says so: a chord has no one period, the normalised
// peak falls below the confidence floor, and both consumers hold their last
// note rather than chasing noise.
//
// EASY TO GET WRONG: the decimation factor, the anti-alias corner, the window
// and both lag bounds are owned by the SAMPLE RATE, not by a knob. Derive them
// all in prepare() or the tracker's range moves with the device (a 192 kHz
// device tracked a fourth of the intended span before this was written down).
// ---------------------------------------------------------------------------
class AutocorrTracker {
public:
    /// Lowest and highest fundamental this tracker will report. Under a
    /// 7-string low B (61 Hz) up to past the 24th fret of the high E.
    static constexpr double kMinHz = 50.0;
    static constexpr double kMaxHz = 1400.0;
    /// Decimated rate the geometry targets. 12 kHz resolves kMaxHz with room
    /// for the parabolic interpolator and keeps the search affordable.
    static constexpr double kTargetDecimatedHz = 12000.0;
    /// Analysis window, in DECIMATED samples (~43 ms at 12 kHz — two periods of
    /// the lowest note, which is the floor a correlation needs).
    static constexpr int kWindow = 512;

    void prepare(double sampleRate) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;

        // Everything below is RATE-owned. 4 at 48 kHz, 16 at 192 kHz.
        decim_ = (int) std::lround(sr_ / kTargetDecimatedHz);
        if (decim_ < 1) decim_ = 1;
        decSr_ = sr_ / (double) decim_;

        // Anti-alias before the decimation: two cascaded Butterworth-Q
        // low-passes at 0.4 x the decimated Nyquist.
        aa1_.setLowpass(sr_, 0.40 * decSr_ * 0.5, 0.54);
        aa2_.setLowpass(sr_, 0.40 * decSr_ * 0.5, 1.31);

        minLag_ = (int) std::floor(decSr_ / kMaxHz);
        maxLag_ = (int) std::ceil(decSr_ / kMinHz);
        if (minLag_ < 2) minLag_ = 2;
        if (maxLag_ > kWindow - 2) maxLag_ = kWindow - 2;
        if (maxLag_ <= minLag_ + 4) maxLag_ = minLag_ + 4;

        // One analysis every ~5 ms of REAL time, whatever the rate.
        hop_ = (int) std::lround(sr_ * 0.005);
        if (hop_ < 16) hop_ = 16;

        ring_.assign((std::size_t) kWindow, 0.0f);

        // FULL-RATE refinement ring. The decimated pass costs a quarter of the
        // work and finds the right PERIOD; it cannot find the right CENTS. At
        // 12 kHz one lag step at 440 Hz is 40 cents, and a parabolic fit on the
        // decimated correlation still leaves ~1.5 Hz — which a pitch CORRECTOR
        // would then faithfully apply as a 6-cent error. Nine full-rate lags
        // around the coarse answer cost ~70 MAC/sample and bring it under half
        // a cent.
        maxLagFull_ = maxLag_ * decim_;
        maxSpanFull_ = 2 * maxLagFull_;
        fullSize_ = nextPow2i(maxLagFull_ + maxSpanFull_ + 8);
        full_.assign((std::size_t) fullSize_, 0.0f);

        env_.prepare(sr_);
        env_.setTimes(2.0f, 60.0f);
        reset();
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        std::fill(full_.begin(), full_.end(), 0.0f);
        fullWrite_ = 0;
        aa1_.reset();
        aa2_.reset();
        env_.reset();
        write_ = 0;
        decCount_ = 0;
        hopCount_ = 0;
        freq_ = 0.0f;
        clarity_ = 0.0f;
        amp_ = 0.0f;
        confident_ = false;
    }

    int decimation() const { return decim_; }
    int hopSamples() const { return hop_; }
    /// Tracking latency in FULL-RATE samples: the window this answer describes
    /// is centred half a window back. Reported honestly rather than folded into
    /// a module's latencySamples() — the tracker delays no audio.
    int analysisLagSamples() const { return (kWindow / 2) * decim_; }

    float frequencyHz() const { return freq_; }
    float clarity() const { return clarity_; }
    float amplitude() const { return amp_; }
    bool confident() const { return confident_; }

    /// One full-rate mono sample. A new analysis runs every `hopSamples()`
    /// frames; between hops every getter below holds its last answer.
    void process(float x) {
        if (!std::isfinite(x)) x = 0.0f;
        amp_ = env_.process(x);
        if (fullSize_ > 0) {
            full_[(std::size_t) (fullWrite_ & (fullSize_ - 1))] = x;
            ++fullWrite_;
        }
        const float lp = aa2_.process(aa1_.process(x));

        if (++decCount_ >= decim_) {
            decCount_ = 0;
            ring_[(std::size_t) (write_ & (kWindow - 1))] = lp;
            ++write_;
        }
        if (++hopCount_ >= hop_) {
            hopCount_ = 0;
            analyse();
        }
    }

private:
    /// A Butterworth-ish cascade section. Biquad::setLowpass takes a Q, so the
    /// two sections above are one 4th-order low-pass.
    struct Section {
        Biquad b;
        void setLowpass(double sr, double fc, double q) { b.setLowpass(sr, fc, q); }
        float process(float x) { return b.process(x); }
        void reset() { b.reset(); }
    };

    float at(int offsetFromOldest) const {
        const long long a = write_ - (long long) kWindow + (long long) offsetFromOldest;
        if (a < 0) return 0.0f;
        return ring_[(std::size_t) (a & (kWindow - 1))];
    }

    /// Normalised autocorrelation, coarse (stride 2 over both lag and sample)
    /// then refined at full resolution around the winner — the same two-pass
    /// shape dcdsp_whammy.h's splice search uses, for the same reason: the
    /// full-resolution sweep is four times the work for the same answer.
    void analyse() {
        if (write_ < (long long) kWindow) return;           // not primed yet
        if (amp_ <= kAmpFloor) { confident_ = false; clarity_ = 0.0f; return; }

        const int span = kWindow - maxLag_;                 // usable correlation length
        if (span < 32) { confident_ = false; return; }

        int bestLag = minLag_;
        float bestScore = -2.0f;
        for (int lag = minLag_; lag <= maxLag_; lag += 2) {
            const float s = score(lag, span, 2);
            if (s > bestScore) { bestScore = s; bestLag = lag; }
        }
        int refLag = bestLag;
        float refScore = -2.0f;
        const int lo = std::max(minLag_, bestLag - 2);
        const int hi = std::min(maxLag_, bestLag + 2);
        for (int lag = lo; lag <= hi; ++lag) {
            const float s = score(lag, span, 1);
            if (s > refScore) { refScore = s; refLag = lag; }
        }

        // Octave guard: a normalised autocorrelation peaks at every INTEGER
        // MULTIPLE of the period, so a plain maximum reports the sub-octave
        // roughly as often as the fundamental on a bright note. Only a
        // SUBMULTIPLE of the winner is a candidate — the first version of this
        // scanned every shorter lag and a pure 110 Hz sine tracked as 1.6 kHz,
        // because a sine's normalised autocorrelation at a SHORT lag is
        // cos(2*pi*f*tau), which is near 1.0 for any small tau.
        for (int k = 4; k >= 2; --k) {
            const int cand = (int) std::lround((double) refLag / (double) k);
            if (cand < minLag_ || cand >= refLag) continue;
            int best2 = cand;
            float s2 = -2.0f;
            const int lo2 = std::max(minLag_, cand - 2);
            const int hi2 = std::min(maxLag_, cand + 2);
            for (int l2 = lo2; l2 <= hi2; ++l2) {
                const float s = score(l2, span, 1);
                if (s > s2) { s2 = s; best2 = l2; }
            }
            if (s2 > refScore * kOctaveTolerance) { refLag = best2; refScore = s2; break; }
        }

        // Parabolic interpolation on the three scores around the winner: this
        // is what turns an integer lag (40 cents of quantisation at 440 Hz on a
        // 12 kHz grid) into a cents-accurate period.
        const float sm = score(std::max(minLag_, refLag - 1), span, 1);
        const float s0 = refScore;
        const float sp = score(std::min(maxLag_, refLag + 1), span, 1);
        const float denom = sm - 2.0f * s0 + sp;
        float delta = 0.0f;
        if (std::fabs(denom) > 1e-9f) delta = 0.5f * (sm - sp) / denom;
        if (!(delta > -1.0f && delta < 1.0f)) delta = 0.0f;

        const float period = (float) refLag + delta;
        clarity_ = s0;
        if (period > 1.0f && s0 > kClarityFloor) {
            const double periodFull = refineFullRate((double) period * (double) decim_);
            freq_ = (float) (sr_ / periodFull);
            confident_ = std::isfinite(freq_) && freq_ >= (float) kMinHz && freq_ <= (float) kMaxHz;
        } else {
            confident_ = false;
        }
    }

    /// Nine full-rate lags around the coarse period, plus a parabolic fit.
    /// Returns the refined period in FULL-RATE samples; falls back to the
    /// coarse answer whenever the window is not primed or the fit degenerates.
    double refineFullRate(double coarsePeriod) const {
        if (fullSize_ <= 0 || decim_ <= 1) return coarsePeriod;
        const int centre = (int) std::lround(coarsePeriod);
        int span = 2 * centre;
        if (span > maxSpanFull_) span = maxSpanFull_;
        if (span < 64) return coarsePeriod;
        if (fullWrite_ < (long long) (span + centre + decim_ + 2)) return coarsePeriod;

        const int lo = std::max(2, centre - decim_);
        const int hi = centre + decim_;
        int best = centre;
        float bestScore = -2.0f;
        for (int lag = lo; lag <= hi; ++lag) {
            const float s = fullScore(lag, span);
            if (s > bestScore) { bestScore = s; best = lag; }
        }
        const float sm = fullScore(std::max(2, best - 1), span);
        const float sp = fullScore(best + 1, span);
        const float denom = sm - 2.0f * bestScore + sp;
        double delta = 0.0;
        if (std::fabs(denom) > 1e-9f) delta = 0.5 * (double) ((sm - sp) / denom);
        if (!(delta > -1.0 && delta < 1.0)) delta = 0.0;
        const double refined = (double) best + delta;
        return (refined > 1.0) ? refined : coarsePeriod;
    }

    float fullScore(int lag, int span) const {
        double dot = 0.0, ea = 0.0, eb = 0.0;
        const long long newest = fullWrite_ - 1;
        for (int i = 0; i < span; ++i) {
            const long long ia = newest - (long long) i;
            const long long ib = ia - (long long) lag;
            if (ib < 0) break;
            const double a = (double) full_[(std::size_t) (ia & (fullSize_ - 1))];
            const double b = (double) full_[(std::size_t) (ib & (fullSize_ - 1))];
            dot += a * b; ea += a * a; eb += b * b;
        }
        const double n = std::sqrt(ea * eb) + 1e-12;
        return (float) (dot / n);
    }

    float score(int lag, int span, int stride) const {
        double dot = 0.0, ea = 0.0, eb = 0.0;
        for (int i = 0; i < span; i += stride) {
            const double a = (double) at(kWindow - span + i);
            const double b = (double) at(kWindow - span + i - lag);
            dot += a * b; ea += a * a; eb += b * b;
        }
        const double n = std::sqrt(ea * eb) + 1e-12;
        return (float) (dot / n);
    }

    static constexpr float kAmpFloor = 3.0e-4f;       // ~-70 dBFS: silence
    static constexpr float kClarityFloor = 0.60f;     // below this it is not a note
    static constexpr float kOctaveTolerance = 0.88f;  // prefer the shorter lag

    double sr_ = 48000.0;
    double decSr_ = 12000.0;
    int decim_ = 4;
    int minLag_ = 8, maxLag_ = 240, hop_ = 240;
    std::vector<float> ring_;
    std::vector<float> full_;
    long long write_ = 0, fullWrite_ = 0;
    int fullSize_ = 0, maxLagFull_ = 0, maxSpanFull_ = 0;
    int decCount_ = 0, hopCount_ = 0;
    Section aa1_, aa2_;
    EnvelopeFollower env_;
    float freq_ = 0.0f, clarity_ = 0.0f, amp_ = 0.0f;
    bool confident_ = false;
};

// ---------------------------------------------------------------------------
// LpcFormant — the whiten / re-dress pair that keeps a shifted note's spectral
// ENVELOPE where it was (no "chipmunk" colour).
//
//     x --> A(z) --> residual --> [pitch shift] --> 1/A(z) --> y
//
// A(z) is a 12th-order LPC analysis filter recomputed every kHopMs from a
// kWinMs Hann-windowed autocorrelation (white-noise corrected, Levinson-Durbin
// so A is minimum-phase by construction and 1/A is stable, bandwidth-expanded
// so a coefficient swap at a frame boundary cannot ring). It is the same
// time-domain construction dcdsp_whammy.h uses, and deliberately NOT a phase
// vocoder, which is banned on the live input everywhere in this repo.
//
// EASY TO GET WRONG: the SYNTHESIS filter must use the coefficient set that
// belongs to the audio LEAVING the shifter, i.e. the input from `lag` samples
// ago — hence the ring of sets and `synth(..., lagFrames)`. Use the current set
// and the envelope is applied to the wrong instant, which is audible as a
// smeared attack and measures as a moved envelope.
// ---------------------------------------------------------------------------
class LpcFormant {
public:
    static constexpr int kOrder = 12;
    static constexpr int kSets = 16;            // power of two: the set ring
    static constexpr double kWinMs = 24.0;
    static constexpr double kHopMs = 12.0;
    static constexpr double kGamma = 0.985;     // bandwidth expansion
    static constexpr int kMaxWin = 8192;

    void prepare(double sampleRate, int latencySamples) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        win_ = (int) std::lround(sr_ * kWinMs / 1000.0);
        win_ = std::max(128, std::min(kMaxWin, win_));
        hop_ = (int) std::lround(sr_ * kHopMs / 1000.0);
        if (hop_ < 32) hop_ = 32;
        rawSize_ = nextPow2i(win_ + 8);

        window_.assign((std::size_t) win_, 0.0f);
        const double twoPiOverN = kTwoPi / (double) win_;
        for (int i = 0; i < win_; ++i)
            window_[(std::size_t) i] =
                (float) (0.5 - 0.5 * std::cos(twoPiOverN * ((double) i + 0.5)));

        // Lag window (a 40 Hz Gaussian on the autocorrelation) — conditions the
        // Levinson recursion so a near-singular frame cannot produce a set that
        // rings.
        for (int i = 0; i <= kOrder; ++i) {
            const double f = kTwoPi * 40.0 * (double) i / sr_;
            lagWin_[(std::size_t) i] = std::exp(-0.5 * f * f);
        }

        raw_.assign((std::size_t) rawSize_, 0.0f);
        sets_.assign((std::size_t) (kSets * kOrder), 0.0f);
        setLatency(latencySamples);
        reset();
    }

    /// The pipeline delay the synthesis filter has to look back over, in
    /// samples. Re-derived whenever the shifter's geometry changes.
    void setLatency(int latencySamples) {
        lagFrames_ = (hop_ > 0) ? (int) std::lround((double) latencySamples / (double) hop_) : 1;
        if (lagFrames_ < 1) lagFrames_ = 1;
        if (lagFrames_ > kSets - 2) lagFrames_ = kSets - 2;
    }

    void reset() {
        std::fill(raw_.begin(), raw_.end(), 0.0f);
        std::fill(sets_.begin(), sets_.end(), 0.0f);
        xh_.fill(0.0f);
        yh_.fill(0.0f);
        rawIdx_ = 0;
        hopCount_ = 0;
        setCount_ = 0;
    }

    int hopSamples() const { return hop_; }

    /// Push one raw input sample and, on a hop boundary, recompute the set.
    /// Call this BEFORE whiten() for the same frame.
    void push(float x) {
        if (!std::isfinite(x)) x = 0.0f;
        raw_[(std::size_t) (rawIdx_ & (rawSize_ - 1))] = x;
        ++rawIdx_;
        if (++hopCount_ >= hop_) { hopCount_ = 0; computeSet(); }
    }

    /// e[n] = x[n] + sum a_i s^i x[n-i]   (A(z/s), FIR, minimum-phase)
    ///
    /// `strength` is BANDWIDTH EXPANSION, not a mix: it maps every pole of the
    /// prediction filter p -> s*p, so the pair stays minimum-phase for every
    /// s in [0, 1], s = 0 is EXACTLY the identity and s = 1 is exactly the
    /// original filter. That is what makes `formant` crossfadable at all —
    /// blending the identity with `whiten` at the front and the identity with
    /// `synth` at the back does NOT give an inverse pair (an all-pole synthesis
    /// filter fed unwhitened audio rings: measured a 2.64 output peak against a
    /// 0.26 input), whereas A(z/s) and 1/A(z/s) invert each other at every s.
    float whiten(float x, float strength = 1.0f) {
        if (setCount_ <= 0) { shift(xh_, x); return x; }
        const float* a = setAt(setCount_ - 1);
        float e = x;
        if (strength >= 1.0f) {
            for (int i = 0; i < kOrder; ++i) e += a[i] * xh_[(std::size_t) i];
        } else if (strength > 0.0f) {
            float g = strength;
            for (int i = 0; i < kOrder; ++i) { e += g * a[i] * xh_[(std::size_t) i]; g *= strength; }
        }
        shift(xh_, x);
        if (!std::isfinite(e)) { xh_.fill(0.0f); return 0.0f; }
        return e;
    }

    /// y[n] = e[n] - sum a_i s^i y[n-i]   (1/A(z/s), IIR). Uses the set from
    /// `lagFrames_` hops back — the one that belongs to this output sample.
    float synth(float e, float strength = 1.0f) {
        const long long idx = setCount_ - 1 - (long long) lagFrames_;
        if (idx < 0) { shift(yh_, e); return e; }
        const float* a = setAt(idx);
        float y = e;
        if (strength >= 1.0f) {
            for (int i = 0; i < kOrder; ++i) y -= a[i] * yh_[(std::size_t) i];
        } else if (strength > 0.0f) {
            float g = strength;
            for (int i = 0; i < kOrder; ++i) { y -= g * a[i] * yh_[(std::size_t) i]; g *= strength; }
        }
        if (!std::isfinite(y) || y > 32.0f || y < -32.0f) { yh_.fill(0.0f); return 0.0f; }
        shift(yh_, y);
        return y;
    }

private:
    float* setAt(long long idx) {
        return sets_.data() + (std::size_t) ((idx & (kSets - 1)) * kOrder);
    }
    const float* setAt(long long idx) const {
        return sets_.data() + (std::size_t) ((idx & (kSets - 1)) * kOrder);
    }

    static void shift(std::array<float, kOrder>& h, float v) {
        for (int i = kOrder - 1; i > 0; --i) h[(std::size_t) i] = h[(std::size_t) (i - 1)];
        h[0] = v;
    }

    float rawAt(int offsetBack) const {
        long long a = rawIdx_ - 1 - (long long) offsetBack;
        if (a < 0) return 0.0f;
        return raw_[(std::size_t) (a & (rawSize_ - 1))];
    }

    void computeSet() {
        // Autocorrelation of the windowed frame ending at the newest sample.
        std::array<double, kOrder + 1> r{};
        for (int k = 0; k <= kOrder; ++k) {
            double acc = 0.0;
            for (int i = k; i < win_; ++i) {
                const double a = (double) rawAt(win_ - 1 - i) * (double) window_[(std::size_t) i];
                const double b = (double) rawAt(win_ - 1 - (i - k))
                               * (double) window_[(std::size_t) (i - k)];
                acc += a * b;
            }
            r[(std::size_t) k] = acc * lagWin_[(std::size_t) k];
        }
        r[0] *= (1.0 + 1e-4);                       // white-noise correction
        if (!(r[0] > 1e-12)) { writeSet(nullptr); return; }

        // Levinson-Durbin.
        std::array<double, kOrder + 1> a{}, tmp{};
        double err = r[0];
        for (int i = 1; i <= kOrder; ++i) {
            double acc = r[(std::size_t) i];
            for (int j = 1; j < i; ++j) acc += a[(std::size_t) j] * r[(std::size_t) (i - j)];
            const double k = -acc / err;
            if (!std::isfinite(k) || std::fabs(k) >= 1.0) { writeSet(nullptr); return; }
            for (int j = 1; j < i; ++j) tmp[(std::size_t) j] = a[(std::size_t) j]
                                                            + k * a[(std::size_t) (i - j)];
            for (int j = 1; j < i; ++j) a[(std::size_t) j] = tmp[(std::size_t) j];
            a[(std::size_t) i] = k;
            err *= (1.0 - k * k);
            if (!(err > 1e-18)) { writeSet(nullptr); return; }
        }
        std::array<float, kOrder> out{};
        double g = kGamma;
        for (int i = 1; i <= kOrder; ++i) {
            out[(std::size_t) (i - 1)] = (float) (a[(std::size_t) i] * g);
            g *= kGamma;
        }
        writeSet(out.data());
    }

    void writeSet(const float* src) {
        float* dst = setAt(setCount_);
        for (int i = 0; i < kOrder; ++i) dst[i] = src ? src[i] : 0.0f;
        ++setCount_;
    }

    double sr_ = 48000.0;
    int win_ = 1152, hop_ = 576, rawSize_ = 2048, lagFrames_ = 2;
    std::vector<float> window_;
    std::vector<float> raw_;
    std::vector<float> sets_;
    std::array<double, kOrder + 1> lagWin_{};
    std::array<float, kOrder> xh_{}, yh_{};
    long long rawIdx_ = 0, setCount_ = 0;
    int hopCount_ = 0;
};

// ---------------------------------------------------------------------------
// PolyBlepOsc — band-limited saw / square / PWM / sine.
//
// A naive saw or square is a step discontinuity every cycle, and a step is
// flat to Nyquist and beyond: at 220 Hz on a 48 kHz device the fold-back sits
// only ~20 dB under the fundamental and is INHARMONIC, which is the "cheap
// synth" sound. PolyBLEP subtracts a 2-sample polynomial approximation of the
// band-limited step at every discontinuity, which buys ~40 dB.
//
// EASY TO GET WRONG: the increment must GLIDE between notes (portamento) and a
// waveform switch is a full-scale discontinuity — sine and saw differ by 1.0 at
// phase 0 — so the caller crossfades shapes rather than swapping one.
// `shapeAt()` is stateless for exactly that reason.
// ---------------------------------------------------------------------------
class PolyBlepOsc {
public:
    enum Shape { Square = 0, Saw = 1, SubSine = 2, Pwm = 3 };

    void prepare(double sampleRate) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        reset();
    }
    void reset() { phase_ = 0.0f; }
    void setPhase(float p) { phase_ = p - std::floor(p); }

    /// One sample at `hz`, reading BOTH shapes so the caller can crossfade.
    /// `pw` is the PWM duty cycle in (0, 1).
    void advance(float hz) {
        inc_ = (float) ((double) clampf(hz, 0.0f, (float) (sr_ * 0.45), 0.0f) / sr_);
        phase_ += inc_;
        if (phase_ >= 1.0f) phase_ -= std::floor(phase_);
        if (!(phase_ >= 0.0f && phase_ < 1.0f)) phase_ = 0.0f;
    }

    float value(Shape s, float pw) const {
        switch (s) {
            case Saw: return saw(phase_, inc_);
            case SubSine: return (float) std::sin(kTwoPi * (double) phase_);
            case Pwm: return pulse(phase_, inc_, clampf(pw, 0.05f, 0.95f, 0.5f));
            case Square:
            default: return pulse(phase_, inc_, 0.5f);
        }
    }

    float phase() const { return phase_; }

private:
    /// The PolyBLEP residual at a discontinuity: t is the phase distance from
    /// the discontinuity in units of the increment.
    static float blep(float t, float dt) {
        if (dt <= 0.0f) return 0.0f;
        if (t < dt) { const float u = t / dt; return u + u - u * u - 1.0f; }
        if (t > 1.0f - dt) { const float u = (t - 1.0f) / dt; return u * u + u + u + 1.0f; }
        return 0.0f;
    }
    static float saw(float p, float dt) { return 2.0f * p - 1.0f - blep(p, dt); }
    static float pulse(float p, float dt, float pw) {
        float v = (p < pw) ? 1.0f : -1.0f;
        v += blep(p, dt);
        float p2 = p - pw;
        if (p2 < 0.0f) p2 += 1.0f;
        v -= blep(p2, dt);
        return v;
    }

    double sr_ = 48000.0;
    float phase_ = 0.0f;
    float inc_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Scale quantisation, shared by pitchcorrect (which snaps to it) and nothing
// else yet. Bit masks over the 12 pitch classes, rooted at `key` (0 = C).
// ---------------------------------------------------------------------------
enum class Scale { Chromatic = 0, Major = 1, Minor = 2, Pentatonic = 3, Blues = 4 };

inline unsigned scaleMask(Scale s) {
    switch (s) {
        case Scale::Major:      return 0b101010110101u;   // 0 2 4 5 7 9 11
        case Scale::Minor:      return 0b010110101101u;   // 0 2 3 5 7 8 10
        case Scale::Pentatonic: return 0b001010010101u;   // 0 2 4 7 9
        case Scale::Blues:      return 0b010011101001u;   // 0 3 5 6 7 10
        case Scale::Chromatic:
        default:                return 0b111111111111u;
    }
}

/// MIDI note (may be fractional) of a frequency, A4 = 440 Hz = note 69.
inline float freqToMidi(float hz) {
    if (!(hz > 1.0f)) return 0.0f;
    return 69.0f + 12.0f * std::log2(hz / 440.0f);
}
inline float midiToFreq(float midi) { return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f); }

/// The nearest member of `s` rooted at `key` to a (fractional) MIDI note.
inline float snapToScale(float midi, Scale s, int key) {
    const unsigned mask = scaleMask(s);
    if (mask == 0xFFFu) return std::round(midi);
    key = ((key % 12) + 12) % 12;
    float best = std::round(midi);
    float bestDist = 1e9f;
    const int centre = (int) std::round(midi);
    for (int n = centre - 7; n <= centre + 7; ++n) {
        const int pc = (((n - key) % 12) + 12) % 12;
        if ((mask & (1u << pc)) == 0u) continue;
        const float d = std::fabs((float) n - midi);
        if (d < bestDist) { bestDist = d; best = (float) n; }
    }
    return best;
}

} // namespace detail
} // namespace dcdsp

#endif // DCDSP_DETAIL_PITCHGEAR_H
