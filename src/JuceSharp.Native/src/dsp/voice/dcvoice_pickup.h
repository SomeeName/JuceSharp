// dcvoice_pickup.h — turning a recording made with one pickup / instrument into
// another (DotCharter #50). The model table is dcvoice_models.h; the per-note
// filter state lives in the sampler's players (dcvoice_sampler.h). Plain C++17.
//
// THE FILTER, per playing recording (one note on one string):
//
//   x --> [ 1 / source comb ] --> [ target comb ] --> [ resonance ratio ] --> [ body ] --> * gain
//
//   * A pickup at distance d from the bridge, on a string of vibrating length L,
//     weights harmonic n by sin(pi n d / L). With rho = d / L and T0 the period,
//     that is a feed-forward comb with taps half a delay rho * T0 / 2 either
//     side of a centre: 0.5 * (x(t - A + a) - g x(t - A - a)), a = rho T0 / 2.
//     Several points (two pickups in parallel, a humbucker's two coils) sum.
//     The target is LEAKY by the same g as the source inverse: a real pickup's
//     notch is finite (its magnetic window has a width), and modelling source
//     and target alike makes a conversion between two near-identical pickups
//     near-identity instead of a boost at the source's notches. On EGFxSet a
//     leak of 0.7 predicts the other positions at 5.37 dB against 6.03 dB for
//     an exact comb (tools/voice/calibrate_pickups.py).
//   * The SOURCE comb is divided out by a REGULARISED inverse: one point is
//     2 / (1 - gs z^-2b); two equal-weight points factor exactly into
//     sin(w m) cos(w delta) and invert as two such one-pole combs (the second
//     with a + sign). gs (SourceSound::inverseG, 0.5) < 1 caps the boost at the
//     source's notches
//     (g = 0.7: +10.5 dB; 0.5-0.7 predicted EGFxSet's positions best, 0.85+
//     worse) — a real pickup's notches are never infinitely deep.
//   * rho is a property of the STRING (and fret), so it survives resampling: a
//     recording played a tone higher keeps its notches at the same harmonic
//     numbers. Delays in samples are rho * fs / f, recomputed every control
//     step from the pitch actually playing (bends, vibrato).
//   * The resonance ratio is one biquad: zeros at the source pickup's loaded
//     resonance, poles at the target's (each frequency pre-warped).
//   * Loudness: every note is normalised by the comb's gain over its first
//     twelve harmonics (energy ~ 1/n^2), so a pickup changes the TONE and not the
//     level — the recordings are level-normalised anyway — and then the
//     target's output (levelDb) relative to the source's is applied.
//
// REAL-TIME: everything here that runs per note or per sample is allocation-
// and lock-free; plans and coefficients are built on the control thread.
#pragma once

#include "dcvoice_models.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dcvoice {

/// A biquad's coefficients (a0 normalised).
struct BiquadCoeffs
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    bool unity() const noexcept { return b0 == 1 && b1 == 0 && b2 == 0 && a1 == 0 && a2 == 0; }
};

namespace pickup {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kRegularise = 0.70f;   // g of the leaky TARGET comb (calibrated on EGFxSet);
                                              // the source inverse's g is per recording (SourceSound::inverseG)
inline constexpr int kRing = 2048;            // per-player history (power of two)
inline constexpr int kMaxDelay = kRing - 8;   // the longest delay read, samples

/// Distance (mm) of a sense point under the string whose OPEN pitch is `openMidi`.
inline float senseDistance(const SensePoint& p, double openMidi, float midiLow, float midiHigh) noexcept
{
    if (p.step)
        return openMidi < 0.5 * ((double) midiLow + midiHigh) ? p.dLowMm : p.dHighMm;
    const double t = midiHigh > midiLow ? std::clamp((openMidi - midiLow) / ((double) midiHigh - midiLow), 0.0, 1.0) : 0.0;
    return (float) (p.dLowMm + (p.dHighMm - p.dLowMm) * t);
}

/// The analog prototype (b2 s^2 + b1 s + b0) / (a2 s^2 + a1 s + a0) -> digital,
/// bilinear at rate fs (the caller pre-warps its critical frequencies).
inline BiquadCoeffs bilinear(double b2, double b1, double b0, double a2, double a1, double a0, double fs) noexcept
{
    const double k = 2.0 * fs, k2 = k * k;
    const double B0 = b2 * k2 + b1 * k + b0, B1 = 2.0 * (b0 - b2 * k2), B2 = b2 * k2 - b1 * k + b0;
    const double A0 = a2 * k2 + a1 * k + a0, A1 = 2.0 * (a0 - a2 * k2), A2 = a2 * k2 - a1 * k + a0;
    BiquadCoeffs c;
    c.b0 = (float) (B0 / A0); c.b1 = (float) (B1 / A0); c.b2 = (float) (B2 / A0);
    c.a1 = (float) (A1 / A0); c.a2 = (float) (A2 / A0);
    return c;
}

inline double prewarp(double hz, double fs) noexcept
{
    const double f = std::min(hz, 0.45 * fs);
    return 2.0 * fs * std::tan(kPi * f / fs);
}

/// One pickup's loaded resonance swapped for another's: |H| -> 1 at DC.
inline BiquadCoeffs resonanceRatio(float srcHz, float srcQ, float dstHz, float dstQ, double fs) noexcept
{
    if (std::fabs(srcHz - dstHz) < 1.0f && std::fabs(srcQ - dstQ) < 1.0e-3f)
        return {};
    const double ws = prewarp(srcHz, fs), wt = prewarp(dstHz, fs);
    // ((s^2 + s ws/Qs + ws^2) / ws^2) * (wt^2 / (s^2 + s wt/Qt + wt^2))
    const double g = (wt * wt) / (ws * ws);
    return bilinear(g, g * ws / srcQ, g * ws * ws, 1.0, wt / dstQ, wt * wt, fs);
}

/// RBJ peak / shelf, `gainDb` (0 = unity).
inline BiquadCoeffs bodyBand(const BodyBand& b, double fs, bool invert) noexcept
{
    const double gainDb = invert ? -b.gainDb : b.gainDb;
    if (gainDb == 0.0)
        return {};
    const double f = std::clamp((double) b.hz, 20.0, 0.45 * fs);
    const double A = std::pow(10.0, gainDb / 40.0), w = 2.0 * kPi * f / fs, cw = std::cos(w), sw = std::sin(w);
    const double q = std::max((double) b.q, 0.1), alpha = sw / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;
    if (b.type == 0)
    {
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    }
    else
    {
        const double sq = 2.0 * std::sqrt(A) * alpha;
        if (b.type == 1)   // low shelf
        {
            b0 = A * ((A + 1) - (A - 1) * cw + sq); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sq);
            a0 = (A + 1) + (A - 1) * cw + sq; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sq;
        }
        else               // high shelf
        {
            b0 = A * ((A + 1) + (A - 1) * cw + sq); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sq);
            a0 = (A + 1) - (A - 1) * cw + sq; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sq;
        }
    }
    BiquadCoeffs c;
    c.b0 = (float) (b0 / a0); c.b1 = (float) (b1 / a0); c.b2 = (float) (b2 / a0);
    c.a1 = (float) (a1 / a0); c.a2 = (float) (a2 / a0);
    return c;
}

} // namespace pickup

/// What one pack variant's recordings are converted from and to. Built on the
/// control thread (setPlan, then coefficients at prepare); read-only on the
/// audio thread.
struct ConversionPlan
{
    bool identity = true;          // play the recordings as they are
    SourceSound source {};
    PickupSound target {};
    float targetScaleMm = 648.0f;
    float targetMidiLow = 40.0f, targetMidiHigh = 64.0f;
    BodyEq targetBody {};
    // Derived at prepare():
    BiquadCoeffs resonance {};
    BiquadCoeffs body[4] {};
    int bodyCount = 0;
    float levelGain = 1.0f;        // target output against the source's

    /// Fills the derived coefficients for rate fs. Control thread.
    void prepare(double fs) noexcept
    {
        bodyCount = 0;
        resonance = {};
        levelGain = 1.0f;
        if (identity)
            return;
        resonance = pickup::resonanceRatio(source.pickup.resonanceHz, source.pickup.resonanceQ,
                                           target.resonanceHz, target.resonanceQ, fs);
        for (const BodyBand& b : targetBody.band)
            if (b.gainDb != 0.0f && bodyCount < 4)
                body[bodyCount++] = pickup::bodyBand(b, fs, false);
        for (const BodyBand& b : source.body.band)
            if (b.gainDb != 0.0f && bodyCount < 4)
                body[bodyCount++] = pickup::bodyBand(b, fs, true);
        levelGain = (float) std::pow(10.0, (target.levelDb - source.pickup.levelDb) / 20.0);
    }
};

/// The per-note numbers of a conversion: the comb geometry of THIS note as
/// fractions of its period (rho), and its loudness normalisation. Computed at
/// note start (and when a slide changes the fret), audio thread, no allocation.
struct NoteComb
{
    int targetCount = 0;
    float targetRho[4] {};         // rho of each target sense point (times k for a natural harmonic)
    float targetWeight[4] {};
    float sourceSinRho = 0.0f;     // 2m / T0 of the source's sine factor (0 = no inverse)
    float sourceCosRho = 0.0f;     // 2 delta / T0 of its cosine factor (0 = none)
    float gain = 1.0f;

    /// `openMidiT`/`lengthT`: the TARGET string's open pitch (from the nut) and
    /// its vibrating length (mm); `openMidiS` / `lengthS` the same for the
    /// recording. `harmonicK` > 1 for a natural harmonic: the string vibrates in
    /// mode k over its whole (open) length, so harmonic n of the sounding note is
    /// mode n*k and its rho is k * d / L_open.
    /// `sourceK` > 1: the RECORDING is itself a natural harmonic (mode k of
    /// its open string, `lengthS` the open length).
    void set(const ConversionPlan& plan, double openMidiT, double lengthT, double openMidiS, double lengthS,
             double harmonicK = 1.0, double sourceK = 1.0) noexcept
    {
        targetCount = 0;
        sourceSinRho = sourceCosRho = 0.0f;
        gain = 1.0f;
        const PickupSound& t = plan.target;
        for (int i = 0; i < t.count && i < 4; ++i)
        {
            const float d = pickup::senseDistance(t.pt[i], openMidiT, plan.targetMidiLow, plan.targetMidiHigh);
            targetRho[targetCount] = (float) (std::clamp(d / std::max(lengthT, 1.0), 0.0, 0.98) * std::max(harmonicK, 1.0));
            targetWeight[targetCount] = t.pt[i].weight;
            ++targetCount;
        }
        const PickupSound& s = plan.source.pickup;
        float rs[2] = { 0.0f, 0.0f };
        for (int j = 0; j < s.count && j < 2; ++j)
            rs[j] = (float) (std::clamp(pickup::senseDistance(s.pt[j], openMidiS, plan.source.midiLow, plan.source.midiHigh)
                                            / std::max(lengthS, 1.0), 0.0, 0.98) * std::max(sourceK, 1.0));
        if (s.count == 1)
            sourceSinRho = rs[0];                          // 2b / T0 = rho
        else if (s.count >= 2)
        {
            const float lo = std::min(rs[0], rs[1]), hi = std::max(rs[0], rs[1]);
            sourceSinRho = 0.5f * (lo + hi);              // 2m / T0
            sourceCosRho = 0.5f * (hi - lo);              // 2 delta / T0
        }
        // Loudness: the regularised transfer over harmonics 1..12, weighted by a
        // plucked string's energy (~1/n^2), so a converted note is as loud as
        // the recording — the pack builder levels every recording the same way.
        const double g = pickup::kRegularise, gs = plan.source.inverseG;
        double centre = 0.0;
        for (int i = 0; i < targetCount; ++i)
            centre = std::max(centre, 0.5 * (double) targetRho[i]);
        double num = 0.0, den = 0.0;
        for (int n = 1; n <= 12; ++n)
        {
            // The leaky target, exactly as convert() runs it (delays in periods).
            double re = 0.0, im = 0.0;
            for (int i = 0; i < targetCount; ++i)
            {
                const double a = 0.5 * targetRho[i];
                const double p1 = 2.0 * pickup::kPi * n * (centre - a), p2 = 2.0 * pickup::kPi * n * (centre + a);
                re += 0.5 * targetWeight[i] * (std::cos(p1) - g * std::cos(p2));
                im -= 0.5 * targetWeight[i] * (std::sin(p1) - g * std::sin(p2));
            }
            const double tt = std::hypot(re, im);
            double inv = 1.0;
            if (sourceSinRho > 0.0f)
            {
                const double w = 2.0 * pickup::kPi * n * sourceSinRho;   // phase of the 2m delay at harmonic n
                inv *= 2.0 / std::hypot(1.0 - gs * std::cos(w), gs * std::sin(w));
            }
            if (sourceCosRho > 0.0f)
            {
                const double w = 2.0 * pickup::kPi * n * sourceCosRho;
                inv *= 2.0 / std::hypot(1.0 + gs * std::cos(w), gs * std::sin(w));
            }
            const double h = tt * inv;
            num += h * h / ((double) n * n);
            den += 1.0 / ((double) n * n);
        }
        const double rms = std::sqrt(num / den);
        // Capped at +6 dB: a bridge pickup's weak fundamental would otherwise
        // lift every upper harmonic (and the pick attack) far past the recording.
        gain = (float) (rms > 1.0e-4 ? std::clamp(1.0 / rms, 0.05, 2.0) : 1.0);
    }
};

/// One player's conversion filter state. The rings are big (2 x 8 KB), so they
/// live beside the players, never inside a struct that is reset per note.
struct CombRing
{
    float u[pickup::kRing] {};
    float v[pickup::kRing] {};
};

inline bool samePickup(const PickupSound& a, const PickupSound& b) noexcept
{
    if (a.count != b.count || a.resonanceHz != b.resonanceHz || a.resonanceQ != b.resonanceQ || a.levelDb != b.levelDb)
        return false;
    for (int i = 0; i < a.count; ++i)
        if (a.pt[i].dLowMm != b.pt[i].dLowMm || a.pt[i].dHighMm != b.pt[i].dHighMm || a.pt[i].weight != b.pt[i].weight
            || a.pt[i].step != b.pt[i].step)
            return false;
    return true;
}

inline bool sameBody(const BodyEq& a, const BodyEq& b) noexcept
{
    for (int i = 0; i < 2; ++i)
        if (a.band[i].gainDb != b.band[i].gainDb
            || (a.band[i].gainDb != 0.0f
                && (a.band[i].type != b.band[i].type || a.band[i].hz != b.band[i].hz || a.band[i].q != b.band[i].q)))
            return false;
    return true;
}

/// The conversion from what pack `pack` (zone variant) recorded to `target` on
/// `model`. IDENTITY when they are the same pickup, body and scale — the
/// recordings then play exactly as recorded. Control thread.
inline ConversionPlan makeConversionPlan(const ModelSpec& model, const PickupSound& target, const char* pack, int variant)
{
    ConversionPlan plan;
    SourceSound src;
    if (! sourceFor(pack, variant, src))
        return plan;   // unknown provenance: play it as recorded
    plan.source = src;
    plan.target = target;
    plan.targetScaleMm = model.scaleMm;
    plan.targetMidiLow = model.midiLow;
    plan.targetMidiHigh = model.midiHigh;
    plan.targetBody = model.body;
    plan.identity = samePickup(src.pickup, target) && sameBody(src.body, model.body)
                    && std::fabs(src.scaleMm - model.scaleMm) < 0.5f;
    return plan;
}

} // namespace dcvoice
