// dcdsp_primitives.h
// -----------------------------------------------------------------------------
// DotCharter in-house tone-rig engine — shared DSP primitives (0.16 Phase 0+).
//
// Namespace: dcdsp.  Header-only.  C++17.  ZERO dependencies beyond the standard
// headers listed below.  Every primitive is real-time safe: allocation happens
// ONLY in prepare()/setMaxSize(); process()/next()/readAt()/push() never
// allocate, lock, or throw.
//
// Filter maths follow Robert Bristow-Johnson's "Audio EQ Cookbook" (RBJ) for the
// Biquad, Vadim Zavalishin / Andrew Simper (Cytomic) for the TPT state-variable
// filter, and Parker/Zavalishin's first-order antiderivative anti-aliasing
// (ADAA1) for the saturating tanh.  These formulas are transcribed verbatim from
// the published references — a wrong coefficient here poisons every downstream
// effect, so treat this file as load-bearing.
// -----------------------------------------------------------------------------
#ifndef DCDSP_PRIMITIVES_H
#define DCDSP_PRIMITIVES_H

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dcdsp {

// -----------------------------------------------------------------------------
// Shared constants / helpers
// -----------------------------------------------------------------------------
constexpr double kPi     = 3.14159265358979323846;
constexpr double kTwoPi  = 6.28318530717958647692;
constexpr double kLn2    = 0.69314718055994530942;

// Clamp a cutoff to a numerically safe open interval (0, Nyquist).
//
// The comparisons are written NEGATED (`!(fc >= 1.0)` rather than `fc < 1.0`)
// on purpose: both halves of the obvious form are FALSE for a NaN, so a NaN fc
// used to pass straight through into cos/sin/tan and poison every coefficient
// of the filter designed from it.  Biquad::reset() and StateVariableTPT::reset()
// clear STATE only, so NaN coefficients are permanent — the filter is silent for
// the life of the node and no later valid setParams recovers it.  This is the
// shared primitive, so the guard here covers all 47 cores at once.
inline double clampCutoff(double sampleRate, double fc) {
    // A non-finite or non-positive rate has no Nyquist; fall back to the one
    // every core already defaults to rather than propagating a NaN bound.
    const double sr  = (sampleRate > 0.0) ? sampleRate : 48000.0;
    const double nyq = 0.5 * sr;
    const double hi  = nyq * 0.4999;   // keep tan(pi*fc/sr) finite for the SVF
    if (!(fc >= 1.0)) fc = 1.0;        // NaN lands here, not on the fall-through
    if (fc > hi)      fc = hi;
    return fc;
}

// clampCutoff's ceiling above is a QUARTER of the sample rate (0.4999 x
// Nyquist: 12 kHz at 48 kHz, 11 kHz at 44.1 kHz), not the Nyquist its comment
// guards, and every core has been voiced against it since ABI v21 — so it
// stays. A design that must place a corner above it OPTS IN to this ceiling
// instead: 0.9 x Nyquist, still clear of where the RBJ forms collapse (and
// tan(pi*fc/sr) is 6.3 there, so the TPT section stays well conditioned).
//
// The opt-in is per DESIGN, never global, so a corner that sits below a
// quarter of the rate is coefficient-for-coefficient what it always was. The
// callers (DotCharter #68, #87) are every corner whose PARAM advertises a
// range past a quarter of the rate: the `eq` treble shelf, `svf.cutoffHz`
// and its LFO/envelope sweep, `graphiceq`'s 16 kHz band, the hall / gated /
// reverse reverb tanks' `dampingKHz` (1-16 kHz), `platereverb`'s damping
// (250 Hz-16 kHz) and its 16 kHz input bandwidth, `convreverb.highCutKHz`
// (2-20 kHz) and `tapesat.hfRolloffKHz` (4-20 kHz), plus the micpos tube
// mic's air roll-off (published above 12 kHz). Each passes `nearNyquist =
// true` to OnePole / Biquad / StateVariableTPT, or calls this directly.
// Pinned per core by the `filter_ceiling` native test and JuceSharp's
// FilterCornersAboveQuarterRateTests.
inline double clampCutoffNearNyquist(double sampleRate, double fc) {
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    const double hi = 0.45 * sr;
    if (!(fc >= 1.0)) fc = 1.0;        // NaN lands here, as in clampCutoff
    if (fc > hi)      fc = hi;
    return fc;
}

// Clamp a shelf/peak gain to a finite decibel range.  Same reasoning as
// clampCutoff: `std::pow(10.0, NaN / 40.0)` is NaN and the RBJ coefficients
// built from it are permanent.  +/-96 dB is far outside any musical setting in
// the rig, so this never bites a legitimate value.
inline double clampGainDb(double gainDb) {
    if (!(gainDb > -96.0)) return (gainDb < 0.0) ? -96.0 : 0.0;   // NaN -> 0 dB
    if (gainDb > 96.0)     return 96.0;
    return gainDb;
}

// Numerically stable log(cosh(x)) = |x| - ln2 + log1p(exp(-2|x|)).
// The naive log(cosh(x)) overflows once cosh(x) leaves float range; this form
// never does.  It is the antiderivative of tanh used by adaaTanh below, so it
// runs TWICE PER SAMPLE in every ADAA stage in the rig — and `log1p(exp(...))`
// is the single most expensive call in the whole core set (measured 13.860 ns
// against 4.343 for std::sin and 3.999 for std::tanh, MSVC 19.51 /O2 x64).
//
// Above |x| = 9 the correction term is exp(-18) = 1.5e-8, which a double
// cannot express against |x| >= 9 to better than its own ULP and a float
// certainly cannot; skipping it there costs 1.523e-08 ABSOLUTE / 1.833e-09
// relative, measured over |x| <= 40 against the exact form, and takes the call
// to 1.768 ns.  Every hot ADAA stage spends most of its time in the tails.
inline double logCosh(double x) {
    const double ax = std::fabs(x);
    if (ax >= 9.0) return ax - kLn2;
    return ax - kLn2 + std::log1p(std::exp(-2.0 * ax));
}

// sin(2 * pi * turns) for a MODULATOR phase — an LFO, a grain window, a
// crossfade.  Quadrant reduction to a quarter turn, then the odd Taylor series
// to x^9, which is the whole cost: 3.292 ns against std::sin's 4.343 (MSVC
// 19.51 /O2 x64), for a measured max ABSOLUTE error of 3.663e-06 over +/-2
// turns (RtFastMathHelpersStayWithinTheirStatedErrorBounds).
//
// **Modulator phases only.**  3.7e-06 is ~109 dB below full scale, which is
// nothing on a delay tap or a window gain and is NOT a licence to put this on
// an audio path or in a filter's coefficient design — the saving is one ns on
// a call that is 4 ns to begin with, so it is only worth taking where the call
// happens several times per sample.
inline float fastSinTurns(float turns) {
    turns -= std::floor(turns);                       // [0, 1)
    float sign = 1.f;
    if (turns >= 0.5f) { turns -= 0.5f; sign = -1.f; }   // sin(x + pi) = -sin(x)
    if (turns > 0.25f)   turns = 0.5f - turns;           // sin(pi - x) =  sin(x)
    const float x  = turns * static_cast<float>(kTwoPi);  // [0, pi/2]
    const float x2 = x * x;
    return sign * x * (1.f + x2 * (-1.f / 6.f + x2 * (1.f / 120.f
                        + x2 * (-1.f / 5040.f + x2 * (1.f / 362880.f)))));
}

// 1 / (20 * log10(2)) — the exponent conversion behind dbToGain below.
constexpr double kDbToLog2 = 0.16609640474436812;

// Linear gain from decibels.  `std::pow(10.0, db / 20.0)` and
// `std::exp2(db * kDbToLog2)` are the SAME function; the second is four times
// cheaper (2.838 ns against 11.210, measured as above) and agrees with the
// first to 2.175e-15 RELATIVE over -96..0 dB, i.e. to within double rounding.
// This is not an approximation and there is no reason to write the pow form on
// a per-sample path.
inline double dbToGain(double db) { return std::exp2(db * kDbToLog2); }

// tan(x) on [0, pi/2), for filter-coefficient design.
//
// `std::tan` costs 9.037 ns; the 5/4 Pade about 0 is accurate to x^9 and, with
// the cotangent identity tan(x) = 1 / tan(pi/2 - x) folding the upper half
// back under pi/4, holds to 1.347e-08 ABSOLUTE and RELATIVE across the whole
// interval the primitives use (up to clampCutoff's 0.4999 * Nyquist) at
// 2.809 ns in a LATENCY-bound micro-benchmark.
//
// **It is deliberately NOT used by StateVariableTPT::set.**  Putting it there
// was measured across every SVF-heavy core in the rig — envfilter, filterseq,
// wah, autowah — and bought between -3.4 % and +0.3 %, i.e. nothing: those
// cores are not latency-bound on the tan, and out-of-order execution hides it
// behind the rest of the sample's work.  Changing every filter coefficient in
// the rig by 1.35e-08 for no measured gain is the wrong trade.  It IS used by
// dcdsp_phaser.h, whose allpass coefficient is computed per channel per sample
// alongside two other transcendentals, where the combination measures 22-27 %.
// Pinned by RtFastMathHelpersStayWithinTheirStatedErrorBounds.
//
// Arguments outside [0, pi/2) fall back to std::tan rather than being clamped,
// because a wrong branch here is a wrong FILTER, not a wrong sample.
inline double fastTan(double x) {
    constexpr double kHalfPi    = 1.57079632679489661923;
    constexpr double kQuarterPi = 0.78539816339744830962;
    if (!(x >= 0.0) || x >= kHalfPi) return std::tan(x);
    const auto pade = [](double v) {
        const double v2 = v * v;
        return v * (945.0 + v2 * (-105.0 + v2)) / (945.0 + v2 * (-420.0 + v2 * 15.0));
    };
    return (x <= kQuarterPi) ? pade(x) : 1.0 / pade(kHalfPi - x);
}

// -----------------------------------------------------------------------------
// OnePole — first-order low/high-pass.  Highpass is the complementary
// (x - lowpass(x)) of the same one-pole lowpass, giving a proper 6 dB/oct HPF.
//
// setHighpassShelf() is the same filter with a FLOOR: below fc the response
// flattens out at `floor` instead of falling for ever.  It exists because most
// of the bass-shaping networks a guitar amp actually contains are SHELVES with
// a bounded depth (a cathode-bypass cap, a Tube Screamer's feedback-leg cut),
// not high-passes — and modelling them as high-passes and then CASCADING them
// is how a rig ends up 15-30 dB down at the low E.  See dcdsp_amp.h's coupling
// network for the measurement.  floor = 0 is bit-identical to setHighpass():
// `x - (1 - 0) * mZ` is the same IEEE operation as `x - mZ`.
// -----------------------------------------------------------------------------
class OnePole {
public:
    // nearNyquist: hold fc under 0.9 x Nyquist (clampCutoffNearNyquist) rather
    // than clampCutoff's quarter-rate ceiling. Off = the pole every caller has
    // always had.
    void setLowpass(double sampleRate, double fc, bool nearNyquist = false) {
        fc = nearNyquist ? clampCutoffNearNyquist(sampleRate, fc) : clampCutoff(sampleRate, fc);
        const double pole = std::exp(-kTwoPi * fc / sampleRate);
        mB1        = static_cast<float>(pole);
        mA0        = static_cast<float>(1.0 - pole);
        mHighpass  = false;
        mHpFloor   = 0.f;
    }
    void setHighpass(double sampleRate, double fc) {
        setLowpass(sampleRate, fc);
        mHighpass = true;
    }
    // First-order low SHELF spelled as a floored high-pass: unity above fc,
    // `floor` (linear, 0..1) below it.  floor = 0 degenerates to setHighpass().
    void setHighpassShelf(double sampleRate, double fc, float floor) {
        setLowpass(sampleRate, fc);
        mHighpass = true;
        mHpFloor  = floor < 0.f ? 0.f : (floor > 1.f ? 1.f : floor);
    }
    float process(float x) {
        // mZ holds the running lowpass output.
        mZ = mA0 * x + mB1 * mZ;
        return mHighpass ? (x - (1.f - mHpFloor) * mZ) : mZ;
    }
    void reset() { mZ = 0.f; }

private:
    float mA0 = 1.f;   // input gain (1 - pole)
    float mB1 = 0.f;   // feedback pole
    float mZ  = 0.f;   // lowpass state
    float mHpFloor = 0.f;  // highpass shelf floor (0 = a true highpass)
    bool  mHighpass = false;
};

// -----------------------------------------------------------------------------
// Biquad — RBJ Audio EQ Cookbook, evaluated in Transposed Direct Form II.
// Coefficients are normalised by a0 at design time.  H(z) =
//   (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
// -----------------------------------------------------------------------------
class Biquad {
public:
    void setPeaking(double sr, double fc, double Q, double gainDb, bool nearNyquist = false) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha, nearNyquist);
        const double A = std::pow(10.0, clampGainDb(gainDb) / 40.0);
        const double b0 =  1.0 + alpha * A;
        const double b1 = -2.0 * cw;
        const double b2 =  1.0 - alpha * A;
        const double a0 =  1.0 + alpha / A;
        const double a1 = -2.0 * cw;
        const double a2 =  1.0 - alpha / A;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }
    void setLowShelf(double sr, double fc, double Q, double gainDb) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha);
        const double A  = std::pow(10.0, clampGainDb(gainDb) / 40.0);
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        const double b0 =      A * ((A + 1.0) - (A - 1.0) * cw + tsa);
        const double b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
        const double b2 =      A * ((A + 1.0) - (A - 1.0) * cw - tsa);
        const double a0 =          (A + 1.0) + (A - 1.0) * cw + tsa;
        const double a1 = -2.0 *   ((A - 1.0) + (A + 1.0) * cw);
        const double a2 =          (A + 1.0) + (A - 1.0) * cw - tsa;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }
    // nearNyquist: hold the corner under 0.9 x Nyquist (clampCutoffNearNyquist)
    // instead of clampCutoff's quarter-rate ceiling. Off = the shelf every
    // caller has always had, coefficient for coefficient.
    void setHighShelf(double sr, double fc, double Q, double gainDb, bool nearNyquist = false) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha, nearNyquist);
        const double A  = std::pow(10.0, clampGainDb(gainDb) / 40.0);
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        const double b0 =      A * ((A + 1.0) + (A - 1.0) * cw + tsa);
        const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
        const double b2 =      A * ((A + 1.0) + (A - 1.0) * cw - tsa);
        const double a0 =          (A + 1.0) - (A - 1.0) * cw + tsa;
        const double a1 =  2.0 *   ((A - 1.0) - (A + 1.0) * cw);
        const double a2 =          (A + 1.0) - (A - 1.0) * cw - tsa;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }
    void setLowpass(double sr, double fc, double Q, bool nearNyquist = false) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha, nearNyquist);
        const double b0 = (1.0 - cw) * 0.5;
        const double b1 =  1.0 - cw;
        const double b2 = (1.0 - cw) * 0.5;
        const double a0 =  1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 =  1.0 - alpha;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }
    void setHighpass(double sr, double fc, double Q) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha);
        const double b0 =  (1.0 + cw) * 0.5;
        const double b1 = -(1.0 + cw);
        const double b2 =  (1.0 + cw) * 0.5;
        const double a0 =  1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 =  1.0 - alpha;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }
    void setBandpass(double sr, double fc, double Q) {
        // Constant 0 dB peak-gain variant.
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha);
        const double b0 =  alpha;
        const double b1 =  0.0;
        const double b2 = -alpha;
        const double a0 =  1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 =  1.0 - alpha;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }
    void setNotch(double sr, double fc, double Q) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha);
        const double b0 =  1.0;
        const double b1 = -2.0 * cw;
        const double b2 =  1.0;
        const double a0 =  1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 =  1.0 - alpha;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }

    float process(float x) {
        const float y = mB0 * x + mZ1;
        mZ1 = mB1 * x - mA1 * y + mZ2;
        mZ2 = mB2 * x - mA2 * y;
        return y;
    }
    void reset() { mZ1 = mZ2 = 0.f; }

private:
    static void common(double sr, double fc, double Q,
                       double& w0, double& cw, double& sw, double& alpha,
                       bool nearNyquist = false) {
        if (!(Q >= 1e-4)) Q = 1e-4;   // negated: a NaN Q lands here
        fc    = nearNyquist ? clampCutoffNearNyquist(sr, fc) : clampCutoff(sr, fc);
        w0    = kTwoPi * fc / sr;
        cw    = std::cos(w0);
        sw    = std::sin(w0);
        alpha = sw / (2.0 * Q);
    }
    void setCoeffs(double b0, double b1, double b2,
                   double a0, double a1, double a2) {
        const double inv = 1.0 / a0;
        mB0 = static_cast<float>(b0 * inv);
        mB1 = static_cast<float>(b1 * inv);
        mB2 = static_cast<float>(b2 * inv);
        mA1 = static_cast<float>(a1 * inv);
        mA2 = static_cast<float>(a2 * inv);
    }
    float mB0 = 1.f, mB1 = 0.f, mB2 = 0.f;  // feed-forward (normalised)
    float mA1 = 0.f, mA2 = 0.f;             // feed-back   (normalised)
    float mZ1 = 0.f, mZ2 = 0.f;             // TDF-II state
};

// -----------------------------------------------------------------------------
// BiquadD — the same RBJ cookbook sections as Biquad, but with DOUBLE
// coefficients AND double state.  Use this (not Biquad) for any filter that runs
// at an OVERSAMPLED rate: at 4x/8x the fc/sr ratio of a musical corner (say a
// 95 Hz low shelf at 192 kHz) puts the poles at z ~= 1, where float state loses
// the precision the difference equation needs and the section either detunes or
// bleeds DC.  Cost is ~the same as the float version on x64 (SSE2 scalar).
// -----------------------------------------------------------------------------
class BiquadD {
public:
    void setPeaking(double sr, double fc, double Q, double gainDb) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha);
        const double A = std::pow(10.0, clampGainDb(gainDb) / 40.0);
        setCoeffs(1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A,
                  1.0 + alpha / A, -2.0 * cw, 1.0 - alpha / A);
    }
    void setLowShelf(double sr, double fc, double Q, double gainDb) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha);
        const double A = std::pow(10.0, clampGainDb(gainDb) / 40.0);
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        setCoeffs(     A * ((A + 1.0) - (A - 1.0) * cw + tsa),
                 2.0 * A * ((A - 1.0) - (A + 1.0) * cw),
                       A * ((A + 1.0) - (A - 1.0) * cw - tsa),
                           (A + 1.0) + (A - 1.0) * cw + tsa,
                 -2.0 *   ((A - 1.0) + (A + 1.0) * cw),
                           (A + 1.0) + (A - 1.0) * cw - tsa);
    }
    void setHighShelf(double sr, double fc, double Q, double gainDb) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha);
        const double A = std::pow(10.0, clampGainDb(gainDb) / 40.0);
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        setCoeffs(      A * ((A + 1.0) + (A - 1.0) * cw + tsa),
                 -2.0 * A * ((A - 1.0) + (A + 1.0) * cw),
                        A * ((A + 1.0) + (A - 1.0) * cw - tsa),
                            (A + 1.0) - (A - 1.0) * cw + tsa,
                  2.0 *    ((A - 1.0) - (A + 1.0) * cw),
                            (A + 1.0) - (A - 1.0) * cw - tsa);
    }
    void setLowpass(double sr, double fc, double Q) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha);
        setCoeffs((1.0 - cw) * 0.5, 1.0 - cw, (1.0 - cw) * 0.5,
                  1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }
    void setHighpass(double sr, double fc, double Q) {
        double w0, cw, sw, alpha; common(sr, fc, Q, w0, cw, sw, alpha);
        setCoeffs((1.0 + cw) * 0.5, -(1.0 + cw), (1.0 + cw) * 0.5,
                  1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }

    inline float process(float x) { return static_cast<float>(processD(x)); }
    inline double processD(double x) {
        const double y = mB0 * x + mZ1;
        mZ1 = mB1 * x - mA1 * y + mZ2;
        mZ2 = mB2 * x - mA2 * y;
        return y;
    }
    void reset() { mZ1 = mZ2 = 0.0; }

private:
    static void common(double sr, double fc, double Q,
                       double& w0, double& cw, double& sw, double& alpha) {
        if (!(Q >= 1e-4)) Q = 1e-4;   // negated: a NaN Q lands here
        fc    = clampCutoff(sr, fc);
        w0    = kTwoPi * fc / sr;
        cw    = std::cos(w0);
        sw    = std::sin(w0);
        alpha = sw / (2.0 * Q);
    }
    void setCoeffs(double b0, double b1, double b2,
                   double a0, double a1, double a2) {
        const double inv = 1.0 / a0;
        mB0 = b0 * inv; mB1 = b1 * inv; mB2 = b2 * inv;
        mA1 = a1 * inv; mA2 = a2 * inv;
    }
    double mB0 = 1.0, mB1 = 0.0, mB2 = 0.0;
    double mA1 = 0.0, mA2 = 0.0;
    double mZ1 = 0.0, mZ2 = 0.0;
};

// -----------------------------------------------------------------------------
// StateVariableTPT — topology-preserving-transform state variable filter
// (Zavalishin / Cytomic).  Unconditionally stable under audio-rate cutoff and Q
// modulation, which the classic Chamberlin SVF is not.  set() then process(x),
// then read lp()/hp()/bp().  k = 1/Q is the damping.
// -----------------------------------------------------------------------------
class StateVariableTPT {
public:
    void set(double sampleRate, double fc, double Q, bool nearNyquist = false) {
        if (!(Q >= 1e-4)) Q = 1e-4;   // negated: a NaN Q lands here
        fc = nearNyquist ? clampCutoffNearNyquist(sampleRate, fc) : clampCutoff(sampleRate, fc);
        const double g = std::tan(kPi * fc / sampleRate);
        const double k = 1.0 / Q;
        const double a1 = 1.0 / (1.0 + g * (g + k));
        mG  = g;
        mK  = k;
        mA1 = a1;
        mA2 = g * a1;
        mA3 = g * mA2;
    }
    // Copy the DESIGN (coefficients) from another instance, leaving this one's
    // STATE alone.  For a core that sweeps one cutoff across N channels, set()
    // costs a std::tan and a divide PER CHANNEL for identical arguments; design
    // once and copy into the siblings.  Bit-identical to calling set() again.
    void copyCoeffsFrom(const StateVariableTPT& o) {
        mG = o.mG; mK = o.mK; mA1 = o.mA1; mA2 = o.mA2; mA3 = o.mA3;
    }
    void process(float x) {
        const double v0 = static_cast<double>(x);
        const double v3 = v0 - mIc2;
        const double v1 = mA1 * mIc1 + mA2 * v3;
        const double v2 = mIc2 + mA2 * mIc1 + mA3 * v3;
        mIc1 = 2.0 * v1 - mIc1;
        mIc2 = 2.0 * v2 - mIc2;
        mBp = static_cast<float>(v1);
        mLp = static_cast<float>(v2);
        mHp = static_cast<float>(v0 - mK * v1 - v2);
    }
    float lp() const { return mLp; }
    float hp() const { return mHp; }
    float bp() const { return mBp; }
    void reset() {
        mIc1 = mIc2 = 0.0;
        mLp = mHp = mBp = 0.f;
    }

private:
    double mG = 0.0, mK = 1.0, mA1 = 0.0, mA2 = 0.0, mA3 = 0.0;
    double mIc1 = 0.0, mIc2 = 0.0;   // integrator "equivalent current" states
    float  mLp = 0.f, mHp = 0.f, mBp = 0.f;
};

// -----------------------------------------------------------------------------
// EnvelopeFollower — peak follower with independent attack/release one-poles.
// Tracks |x|.  Coefficients are the standard exp(-1/(tau*sr)) form; a time of
// 0 ms yields an instantaneous (coefficient 0) edge.
// -----------------------------------------------------------------------------
class EnvelopeFollower {
public:
    void prepare(double sampleRate) {
        mSr = (sampleRate > 0.0) ? sampleRate : 44100.0;
        setTimes(mAttackMs, mReleaseMs);
    }
    void setTimes(float attackMs, float releaseMs) {
        mAttackMs  = attackMs;
        mReleaseMs = releaseMs;
        mAtt = coeff(attackMs);
        mRel = coeff(releaseMs);
    }
    float process(float x) {
        const float r = std::fabs(x);
        const float c = (r > mEnv) ? mAtt : mRel;
        mEnv = c * mEnv + (1.f - c) * r;
        return mEnv;
    }
    void reset() { mEnv = 0.f; }

private:
    float coeff(float ms) const {
        if (ms <= 0.f) return 0.f;
        return static_cast<float>(std::exp(-1.0 / ((ms * 0.001) * mSr)));
    }
    double mSr = 44100.0;
    float  mAttackMs = 10.f, mReleaseMs = 100.f;
    float  mAtt = 0.f, mRel = 0.f;
    float  mEnv = 0.f;
};

// -----------------------------------------------------------------------------
// Lfo — sine / triangle / saw, output in [-1, 1].  Phase accumulator in [0,1).
// -----------------------------------------------------------------------------
class Lfo {
public:
    enum Shape { Sine, Tri, Saw };

    void prepare(double sampleRate) {
        mSr = (sampleRate > 0.0) ? sampleRate : 44100.0;
        setFrequency(mHz);
    }
    void setFrequency(float hz) {
        mHz  = hz;
        mInc = static_cast<float>(static_cast<double>(hz) / mSr);
    }
    void setShape(Shape s) { mShape = s; }
    // Jump the phase accumulator to p (cycles, wrapped into [0,1)). Lets voices/
    // channels start decorrelated without pre-spinning process() thousands of
    // times in reset().
    void setPhase(float p) {
        p -= std::floor(p);
        if (!(p >= 0.f && p < 1.f)) p = 0.f;   // NaN/Inf guard
        mPhase = p;
    }

    // Shape evaluated at an explicit phase. Stateless, so a core can read TWO
    // shapes from one phase -- which is what a shape SWITCH needs: setShape()
    // swaps the waveform under a running oscillator, and at a given phase the
    // shapes differ by up to a full 1.0 (phase 0: sine 0 vs triangle -1). That
    // is a full-scale discontinuity in whatever the LFO drives.
    static float shapeAt(Shape s, float phase) {
        switch (s) {
            case Sine: return std::sin(static_cast<float>(kTwoPi) * phase);
            case Tri:  return 1.f - 4.f * std::fabs(phase - 0.5f);
            case Saw:  return 2.f * phase - 1.f;
            default:   return 0.f;
        }
    }
    // Read a shape at the CURRENT phase without advancing. Pair with advance().
    float peek(Shape s) const { return shapeAt(s, mPhase); }
    void advance() {
        mPhase += mInc;
        if (mPhase >= 1.f) mPhase -= std::floor(mPhase);
        if (mPhase <  0.f) mPhase -= std::floor(mPhase);
    }

    float process() {
        const float out = shapeAt(mShape, mPhase);
        advance();
        return out;
    }
    void reset() { mPhase = 0.f; }

private:
    double mSr   = 44100.0;
    float  mHz   = 1.f;
    float  mInc  = 0.f;
    float  mPhase = 0.f;
    Shape  mShape = Sine;
};

// -----------------------------------------------------------------------------
// InterpolatedDelayLine — circular buffer with fractional read.  readAt() uses
// 4-point 3rd-order Lagrange interpolation by default (setInterpolation() can
// select plain linear).  delaySamples is measured back from the most recently
// pushed sample (delay 0 == last push).  All taps are index-clamped so a read
// can never leave the buffer — output is always finite.
// -----------------------------------------------------------------------------
class InterpolatedDelayLine {
public:
    enum Interp { Linear, Lagrange3 };

    void prepare(int maxDelaySamples) {
        if (maxDelaySamples < 1) maxDelaySamples = 1;
        mMaxDelay = maxDelaySamples;
        mSize     = maxDelaySamples + 4;   // headroom for the id+2 Lagrange tap
        mBuffer.assign(static_cast<std::size_t>(mSize), 0.f);
        mWrite = 0;
    }
    void reset() {
        std::fill(mBuffer.begin(), mBuffer.end(), 0.f);
        mWrite = 0;
    }
    void setInterpolation(Interp mode) { mInterp = mode; }

    void push(float x) {
        mBuffer[static_cast<std::size_t>(mWrite)] = x;
        if (++mWrite >= mSize) mWrite = 0;
    }

    float readAt(float delaySamples) {
        if (mSize <= 0) return 0.f;
        if (delaySamples < 0.f) delaySamples = 0.f;
        const float maxD = static_cast<float>(mMaxDelay);
        if (delaySamples > maxD) delaySamples = maxD;

        const int   id = static_cast<int>(std::floor(delaySamples));
        const float f  = delaySamples - static_cast<float>(id);

        if (mInterp == Linear) {
            const float a = tap(id);
            const float b = tap(id + 1);
            return a + f * (b - a);
        }
        // 4-point, 3rd-order Lagrange.  Nodes at relative delays id-1..id+2;
        // fraction f runs from y0 (delay id) to y1 (delay id+1).
        const float ym1 = tap(id - 1);
        const float y0  = tap(id);
        const float y1  = tap(id + 1);
        const float y2  = tap(id + 2);
        const float fm1 = f - 1.f;
        const float fm2 = f - 2.f;
        const float fp1 = f + 1.f;
        // Lagrange basis (denominators -6, 2, -2, 6).
        const float c_m1 = f   * fm1 * fm2 * (-1.f / 6.f);
        const float c_0  = fp1 * fm1 * fm2 * ( 0.5f);
        const float c_1  = fp1 * f   * fm2 * (-0.5f);
        const float c_2  = fp1 * f   * fm1 * ( 1.f / 6.f);
        return c_m1 * ym1 + c_0 * y0 + c_1 * y1 + c_2 * y2;
    }

private:
    // Sample at integer delay d (0 == last pushed).  Index-clamped to stay valid.
    float tap(int d) const {
        if (d < 0)          d = 0;
        if (d >= mSize)     d = mSize - 1;
        int idx = mWrite - 1 - d;
        idx %= mSize;
        if (idx < 0) idx += mSize;
        return mBuffer[static_cast<std::size_t>(idx)];
    }
    std::vector<float> mBuffer;
    int    mSize     = 0;
    int    mMaxDelay = 0;
    int    mWrite    = 0;
    Interp mInterp   = Lagrange3;
};

// -----------------------------------------------------------------------------
// DcBlocker — classic one-pole/one-zero:  y[n] = x[n] - x[n-1] + R*y[n-1].
// R = 0.995 puts the corner near ~38 Hz at 48 kHz (~34 Hz at 44.1 kHz).
// -----------------------------------------------------------------------------
class DcBlocker {
public:
    // Optional: retune the corner for a given sample rate (default ~20 Hz).
    void prepare(double sampleRate, double cutoffHz = 20.0) {
        if (sampleRate <= 0.0) { mR = 0.995f; return; }
        const double r = 1.0 - (kTwoPi * cutoffHz / sampleRate);
        mR = static_cast<float>(std::max(0.0, std::min(0.99999, r)));
    }
    // Preserve the historical .995f coefficient exactly at 48 kHz while keeping
    // its analogue decay constant across device rates. Existing voicings use
    // this instead of prepare()'s intentionally different 20 Hz default.
    void prepareLegacy48k(double sampleRate) {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        mR = sr == 48000.0 ? 0.995f
                          : static_cast<float>(std::pow(static_cast<double>(0.995f), 48000.0 / sr));
    }
    float process(float x) {
        const float y = x - mX1 + mR * mY1;
        mX1 = x;
        mY1 = y;
        return y;
    }
    void reset() { mX1 = mY1 = 0.f; }

private:
    float mR  = 0.995f;
    float mX1 = 0.f, mY1 = 0.f;
};

// -----------------------------------------------------------------------------
// SmoothedValue — linear ramp toward a target over a fixed time.
// -----------------------------------------------------------------------------
class SmoothedValue {
public:
    void reset(double sampleRate, float rampSeconds) {
        const double steps = std::floor(rampSeconds * (sampleRate > 0.0 ? sampleRate : 44100.0) + 0.5);
        mSteps     = (steps < 1.0) ? 1 : static_cast<int>(steps);
        mRemaining = 0;
        mInc       = 0.f;
        // mCurrent / mTarget keep their existing values.
    }
    void setTarget(float v) {
        mTarget = v;
        if (mSteps <= 1) {
            mCurrent   = v;
            mRemaining = 0;
            mInc       = 0.f;
            return;
        }
        mInc       = (mTarget - mCurrent) / static_cast<float>(mSteps);
        mRemaining = mSteps;
    }
    float next() {
        if (mRemaining > 0) {
            mCurrent += mInc;
            if (--mRemaining == 0) mCurrent = mTarget;  // land exactly on target
        }
        return mCurrent;
    }
    float current() const { return mCurrent; }

    // Convenience: jump immediately with no ramp.
    void setCurrentAndTarget(float v) { mCurrent = mTarget = v; mRemaining = 0; mInc = 0.f; }

    // Land on the PENDING target immediately, cancelling any ramp in flight.
    // This is what prepare() wants: a freshly prepared core must start AT its
    // configured value, not ramp to it from whatever it happened to hold. It
    // exists because `setCurrentAndTarget(x.current())` reads as the same thing
    // and is not — on a first prepare `current()` is 0, so it throws away the
    // target setParams just computed and pins the value at zero (dcdsp_bigmuff
    // shipped exactly that, and a freshly added Big Muff was silent).
    void snapToTarget() { mCurrent = mTarget; mRemaining = 0; mInc = 0.f; }
    float target() const { return mTarget; }
    // True while a ramp is still walking toward the target. A "neutral" or
    // "unity, skip the loop" shortcut must check this, or it snaps the ramp.
    bool isRamping() const { return mRemaining > 0; }

    // ---- block form, for a CHANNEL-OUTER process() loop ---------------------
    // next() is a per-SAMPLE walk and only composes with a sample-outer loop. In
    // a core whose outer loop is the channel, calling next() inside the inner
    // sample loop walks the SAME smoother once per channel: the ramp then runs
    // numChannels times too fast and, worse, every channel is handed a DIFFERENT
    // gain over the same block — which tilts the stereo image while the knob
    // moves (dcdsp_eq.h and dcdsp_graphiceq.h each hand-rolled this line for
    // exactly that reason; these two methods are that pattern, shared).
    //
    // Usage: `float step; const float g0 = sm.blockStart(n, step);` BEFORE the
    // channel loop, `g0 + step * i` per sample, and `sm.advanceBlock(n)` ONCE
    // after it. Every channel gets the same line.
    //
    // A ramp with fewer than n steps left is STRETCHED to land exactly at the
    // end of the block rather than overshooting past the target — at most one
    // block longer than the configured ramp, and never a step.
    float blockStart(int numFrames, float& stepOut) const {
        if (numFrames <= 0 || mRemaining <= 0) { stepOut = 0.f; return mCurrent; }
        stepOut = (mRemaining <= numFrames)
                    ? (mTarget - mCurrent) / static_cast<float>(numFrames)
                    : mInc;
        return mCurrent;
    }
    void advanceBlock(int numFrames) {
        if (numFrames <= 0 || mRemaining <= 0) return;
        if (mRemaining <= numFrames) { mCurrent = mTarget; mRemaining = 0; mInc = 0.f; return; }
        mCurrent  += mInc * static_cast<float>(numFrames);
        mRemaining -= numFrames;
    }

private:
    int   mSteps     = 1;
    int   mRemaining = 0;
    float mInc       = 0.f;
    float mCurrent   = 0.f;
    float mTarget    = 0.f;
};

// -----------------------------------------------------------------------------
// adaaTanh — first-order antiderivative-antialiased tanh (Parker/Zavalishin
// ADAA1).  Reduces aliasing from the tanh non-linearity by integrating over the
// segment [x1, x] using the antiderivative F(x) = log(cosh(x)).  The divided
// difference degenerates when x ~= x1, so fall back to the midpoint tanh below
// a small delta.  Output stays in (-1, 1).  `x1state` is per-voice state the
// caller owns and must persist across calls (updated in place).
// -----------------------------------------------------------------------------
inline float adaaTanh(float x, float& x1state) {
    const double diff = static_cast<double>(x) - static_cast<double>(x1state);
    double y;
    if (std::fabs(diff) < 1e-5) {
        y = std::tanh(0.5 * (static_cast<double>(x) + static_cast<double>(x1state)));
    } else {
        y = (logCosh(static_cast<double>(x)) - logCosh(static_cast<double>(x1state))) / diff;
    }
    x1state = x;
    return static_cast<float>(y);
}

// -----------------------------------------------------------------------------
// adaaHardClip — first-order ADAA hard clip to [-1, 1] (same ADAA1 recipe as
// adaaTanh).  The antiderivative of clip(x) is closed-form:
// F(x) = x^2/2 inside the linear region, |x| - 1/2 outside (continuous at the
// corners).  Output stays in [-1, 1].  `x1state` is per-voice state the caller
// owns and must persist across calls (updated in place).
// -----------------------------------------------------------------------------
inline float adaaHardClip(float x, float& x1state) {
    const double x0 = static_cast<double>(x1state);
    const double x1 = static_cast<double>(x);
    const double diff = x1 - x0;
    double y;
    if (std::fabs(diff) < 1e-5) {
        const double m = 0.5 * (x1 + x0);
        y = (m > 1.0) ? 1.0 : ((m < -1.0) ? -1.0 : m);
    } else {
        const auto F = [](double v) {
            const double av = std::fabs(v);
            return (av <= 1.0) ? 0.5 * v * v : (av - 0.5);
        };
        y = (F(x1) - F(x0)) / diff;
    }
    x1state = x;
    return static_cast<float>(y);
}

} // namespace dcdsp

#endif // DCDSP_PRIMITIVES_H
