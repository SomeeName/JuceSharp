// dcdsp_svf.h — MULTIMODE FILTER ("svf").
//
// Models a classic analogue multimode filter (the Oberheim SEM / MS-20 family):
// ONE topology-preserving-transform state-variable section per channel whose
// low / band / high / notch taps all fall out of the same two integrators, a
// soft saturator inside the resonance path so high Q sings instead of blowing
// up, and three ways to move the cutoff — the knob, an LFO, or an envelope
// follower on the input.
//
// Topology: Zavalishin/Cytomic TPT SVF, unconditionally stable under audio-rate
// cutoff modulation, with the BANDPASS INTEGRATOR STATE passed through a tanh
// before it re-enters the section. Saturating the state (a delayed quantity)
// rather than the instantaneous branch keeps the zero-delay-feedback solve
// linear, so there is no iteration on the callback and no stability cliff. The
// saturator's threshold is scaled by 1/Q: the state it acts on grows WITH the
// resonance, so a fixed threshold turns the drive knob into a mute at high Q.
//
// Easy to get wrong: the four taps are ONE filter, so `mode` is an output
// SELECTION and must never be a hard switch — the taps differ by a full scale at
// any given sample. Each of the four carries its own 10 ms SmoothedValue weight
// (not a single A/B fade), which is what makes an arbitrary switch mid-fade
// continuous rather than merely a single switch gap-free. The second trap is the
// cutoff: it is smoothed in LOG2 Hz, because a linear ramp from 20 Hz to 20 kHz
// spends its whole first half above 10 kHz and reads as a step at the low end.
//
// `source` is the same shape as `mode` and shipped as a hard switch: it moves
// the cutoff by up to +/-4 OCTAVES between two samples on a resonant section.
// It carries THREE ramped weights over the three candidate modulation depths
// (Manual contributes exactly 0 octaves), for the same reason `mode` carries
// four: an arbitrary switch mid-fade stays continuous. Blending the DEPTHS and
// not the cutoff is what keeps the LFO un-filtered - a 10 ms smoother on the
// modulation value itself would turn a 20 Hz LFO into an 8 Hz one-pole.
// The cutoff is held under 0.9 x Nyquist (clampCutoffNearNyquist), NOT the
// quarter-rate ceiling the rest of the primitives use: the knob advertises
// 20 Hz-20 kHz and the sweeps reach four octaves above it, and until 2026-10
// (DotCharter #87) every corner past 12 kHz at 48 kHz sounded at 12 kHz. A
// corner below a quarter of the rate is designed exactly as before. The TPT
// section is stable for any g = tan(pi*fc/sr) < inf, so 0.45 x sr (g = 6.3)
// costs no stability margin.
// And both modulators run in EVERY mode - `lfo_.advance()` and `env_.process()`
// unconditionally, the form dcdsp_talkbox.h uses - or the source that is about
// to be switched TO resumes from a frozen phase or a stale follower. The old
// code advanced the LFO in the Manual branch only, so its comment ("keep the
// phase running so a source switch does not jump") was false for Env -> LFO,
// which is the switch a player actually makes.
#pragma once

#include "dcdsp_primitives.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

struct SvfParams {
    // 0 = LowPass, 1 = BandPass, 2 = HighPass, 3 = Notch. NAMED discrete.
    int   mode = 0;
    // Cutoff in Hz, 20 .. 20000. The knob value IS the frequency; a UI may give
    // the control a logarithmic taper, but the value that arrives here is Hz.
    float cutoffHz = 1000.0f;
    // 0 .. 1, mapped to Q 0.707 (Butterworth) .. ~20 (self-resonant).
    float resonance = 0.3f;
    // 0 .. 1. Saturation inside the resonance path. 0 is the clean linear SVF.
    float drive = 0.0f;
    // 0 = Manual, 1 = LFO, 2 = Envelope. What sweeps the cutoff. NAMED discrete.
    int   source = 0;
    float lfoRateHz = 1.0f;     // 0.05 .. 20
    // 0 .. 1 == up to +/- 4 OCTAVES of sweep around the cutoff knob.
    float lfoDepth = 0.5f;
    // 0 .. 1 == up to +4 octaves at a full-scale input.
    float envSens = 0.5f;
    float envAttackMs = 10.0f;  // 1 .. 200
    float envReleaseMs = 200.0f;// 10 .. 2000
    float mix = 1.0f;           // 0 dry .. 1 wet
};

class SvfCore {
public:
    static constexpr int kModeLowPass  = 0;
    static constexpr int kModeBandPass = 1;
    static constexpr int kModeHighPass = 2;
    static constexpr int kModeNotch    = 3;
    static constexpr int kSourceManual = 0;
    static constexpr int kSourceLfo    = 1;
    static constexpr int kSourceEnv    = 2;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 1;
        const int nch = (numChannels > 0) ? numChannels : 1;
        states_.assign(static_cast<std::size_t>(nch), Section{});   // sole allocation

        // Everything the RATE owns, re-derived: the LFO increment, the
        // follower's two time constants and the smoothers' step counts.
        lfo_.prepare(sr_);
        lfo_.setShape(Lfo::Sine);
        lfo_.setFrequency(params_.lfoRateHz);
        env_.prepare(sr_);
        env_.setTimes(params_.envAttackMs, params_.envReleaseMs);

        cutoffSmooth_.reset(sr_, kSweepRampSeconds);
        cutoffSmooth_.setTarget(std::log2(static_cast<float>(clampCutoffNearNyquist(sr_, params_.cutoffHz))));
        cutoffSmooth_.snapToTarget();
        resSmooth_.reset(sr_, kRampSeconds);
        resSmooth_.setTarget(params_.resonance);
        resSmooth_.snapToTarget();
        driveSmooth_.reset(sr_, kRampSeconds);
        driveSmooth_.setTarget(params_.drive);
        driveSmooth_.snapToTarget();
        mixSmooth_.reset(sr_, kRampSeconds);
        mixSmooth_.setTarget(params_.mix);
        mixSmooth_.snapToTarget();
        for (int m = 0; m < 4; ++m) {
            modeWeight_[m].reset(sr_, kRampSeconds);
            modeWeight_[m].setTarget(m == params_.mode ? 1.0f : 0.0f);
            modeWeight_[m].snapToTarget();
        }
        for (int m = 0; m < 3; ++m) {
            srcWeight_[m].reset(sr_, kRampSeconds);
            srcWeight_[m].setTarget(m == params_.source ? 1.0f : 0.0f);
            srcWeight_[m].snapToTarget();
        }
        lastRes_ = lastDrive_ = -1.0f;   // force the coefficient cache to fill
        reset();
    }

    // STATE only. No user content is touched here or in prepare().
    void reset() {
        for (auto& s : states_) s = Section{};
        lfo_.reset();
        env_.reset();
    }

    void setParams(const SvfParams& p) {
        params_.mode      = clampi(p.mode, 0, 3);
        params_.source    = clampi(p.source, 0, 2);
        params_.cutoffHz  = static_cast<float>(clampCutoffNearNyquist(sr_, clampf(p.cutoffHz, 20.0f, 20000.0f)));
        params_.resonance = clamp01(p.resonance);
        params_.drive     = clamp01(p.drive);
        params_.lfoRateHz = clampf(p.lfoRateHz, 0.05f, 20.0f);
        params_.lfoDepth  = clamp01(p.lfoDepth);
        params_.envSens   = clamp01(p.envSens);
        params_.envAttackMs  = clampf(p.envAttackMs, 1.0f, 200.0f);
        params_.envReleaseMs = clampf(p.envReleaseMs, 10.0f, 2000.0f);
        params_.mix       = clamp01(p.mix);

        cutoffSmooth_.setTarget(std::log2(params_.cutoffHz));
        resSmooth_.setTarget(params_.resonance);
        driveSmooth_.setTarget(params_.drive);
        mixSmooth_.setTarget(params_.mix);
        lfo_.setFrequency(params_.lfoRateHz);
        env_.setTimes(params_.envAttackMs, params_.envReleaseMs);
        // A mode change is FOUR ramps, one per tap, each walking from wherever
        // it currently is. Switching again mid-fade is therefore still
        // continuous; a single A/B fade would step by the un-elapsed fraction.
        for (int m = 0; m < 4; ++m)
            modeWeight_[m].setTarget(m == params_.mode ? 1.0f : 0.0f);
        // ...and a source change is THREE, over the modulation depths.
        for (int m = 0; m < 3; ++m)
            srcWeight_[m].setTarget(m == params_.source ? 1.0f : 0.0f);
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        // An oversized block is skipped WHOLE. Half-processing it would splice
        // raw dry into the middle of a filtered block.
        if (numFrames > maxBlock_) return;
        const int nch = std::min(numChannels, static_cast<int>(states_.size()));
        if (nch <= 0) return;

        for (int n = 0; n < numFrames; ++n) {
            // One walk of each smoother per FRAME (the channel loop is inside).
            const float mix   = mixSmooth_.next();
            const float dryG  = 1.0f - mix;
            const float res   = resSmooth_.next();
            const float drive = driveSmooth_.next();
            const float logFc = cutoffSmooth_.next();
            float w[4];
            for (int m = 0; m < 4; ++m) w[m] = modeWeight_[m].next();
            float sw[3];
            for (int m = 0; m < 3; ++m) sw[m] = srcWeight_[m].next();

            // --- where the cutoff is this sample -----------------------------
            // BOTH modulators run every sample in every mode, so neither
            // resumes from a frozen phase or a stale follower after a switch.
            const float lfoOct = lfo_.process() * params_.lfoDepth * kMaxOctaves;
            float mono = 0.0f;
            for (int c = 0; c < nch; ++c) mono += channels[c][n];
            mono *= 1.0f / static_cast<float>(nch);
            if (!std::isfinite(mono)) mono = 0.0f;
            const float e = env_.process(mono) * 8.0f;
            const float amt = e / (1.0f + e);                  // bounded 0..1
            const float envOct = (std::isfinite(amt) ? amt : 0.0f)
                               * params_.envSens * kMaxOctaves;
            // sw[kSourceManual] contributes exactly 0 octaves, which is what
            // makes Manual an endpoint of the same blend rather than a branch.
            const float octaves = sw[kSourceLfo] * lfoOct + sw[kSourceEnv] * envOct;

            // exp2, not pow(2, .) - the primitives header measures the second at
            // 11.2 ns against 2.8, and this is a per-SAMPLE path.
            const double fc = clampCutoffNearNyquist(sr_, std::exp2(static_cast<double>(logFc + octaves)));
            // Q, k and the saturator threshold depend on `res` and `drive`
            // ALONE, so they are recomputed only when one of those two actually
            // moved - which is during a 10 ms ramp and never while a knob sits
            // still. `fc` moves every sample under an LFO and is not cacheable.
            if (res != lastRes_ || drive != lastDrive_) {
                lastRes_   = res;
                lastDrive_ = drive;
                cachedQ_   = 0.70710678 * std::exp2(static_cast<double>(res) * kLog2Of28);
                cachedK_   = 1.0 / cachedQ_;
                cachedSat_ = cachedK_ * (1.0 + static_cast<double>(drive) * 9.0);
            }
            const double g  = std::tan(kPi * fc / sr_);
            const double k  = cachedK_;
            const double a1 = 1.0 / (1.0 + g * (g + k));
            const double a2 = g * a1;
            const double a3 = g * a2;
            // Saturator gain: unity for small signals, compressing above
            // ~1/satScale. The threshold is scaled by k = 1/Q on purpose — the
            // bandpass state grows WITH Q (that is what resonance is), so a
            // Q-independent threshold crushes a high-Q filter to nothing:
            // measured -45.9 dBFS RMS at resonance 1, drive 1, i.e. the knob
            // that is supposed to make it SING silenced it. Scaled, the
            // saturator engages at roughly the same normalised resonance
            // whatever the Q.
            const double satScale = cachedSat_;

            for (int c = 0; c < nch; ++c) {
                Section& s = states_[static_cast<std::size_t>(c)];
                float x = channels[c][n];
                if (!std::isfinite(x)) x = 0.0f;

                const double v0 = static_cast<double>(x);
                // The one nonlinearity: the bandpass integrator state, softened
                // before it re-enters the section AND before it is updated, so
                // the section stays energy-consistent and cannot run away.
                // BLENDED by `drive` (and unity small-signal, tanh(x*d)/d -> x)
                // so the knob is continuous at its zero stop.
                const double d = static_cast<double>(drive);
                const double ic1 = (1.0 - d) * s.ic1 + d * (std::tanh(s.ic1 * satScale) / satScale);
                const double v3 = v0 - s.ic2;
                const double v1 = a1 * ic1 + a2 * v3;
                const double v2 = s.ic2 + a2 * ic1 + a3 * v3;
                s.ic1 = 2.0 * v1 - ic1;
                s.ic2 = 2.0 * v2 - s.ic2;
                if (!std::isfinite(s.ic1) || !std::isfinite(s.ic2)) { s.ic1 = s.ic2 = 0.0; }

                const double lp = v2;
                const double hp = v0 - k * v1 - v2;
                // The TPT bandpass peaks at Q; 1/Q normalises it to ~unity so a
                // resonance sweep is not also a 26 dB output sweep.
                const double bp = v1 * k;
                const double notch = lp + hp;

                double wet = static_cast<double>(w[0]) * lp
                           + static_cast<double>(w[1]) * bp
                           + static_cast<double>(w[2]) * hp
                           + static_cast<double>(w[3]) * notch;
                if (!std::isfinite(wet)) wet = 0.0;

                float y = dryG * x + mix * static_cast<float>(wet);
                if (!std::isfinite(y)) y = 0.0f;
                channels[c][n] = clampf(y, -24.0f, 24.0f);
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    struct Section { double ic1 = 0.0, ic2 = 0.0; };

    static float clampf(float v, float lo, float hi) {
        // Negated form: a NaN lands on `lo` instead of falling through.
        if (!(v >= lo)) return lo;
        return (v > hi) ? hi : v;
    }
    static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }
    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    static constexpr float  kRampSeconds      = 0.010f;
    static constexpr float  kSweepRampSeconds = 0.020f;  // the cutoff glide
    static constexpr float  kMaxOctaves       = 4.0f;
    // log2(28): the resonance map is Q = 0.707 * 28^res, spelled as an exp2.
    static constexpr double kLog2Of28         = 4.8073549220576042;

    SvfParams params_{};
    double sr_ = 48000.0;
    int    maxBlock_ = 1;
    std::vector<Section> states_;
    Lfo lfo_;
    EnvelopeFollower env_{};
    SmoothedValue cutoffSmooth_;   // log2 Hz
    SmoothedValue resSmooth_;
    SmoothedValue driveSmooth_;
    SmoothedValue mixSmooth_;
    SmoothedValue modeWeight_[4];
    SmoothedValue srcWeight_[3];
    // The `res`/`drive`-only coefficient cache; see process().
    float  lastRes_ = -1.0f, lastDrive_ = -1.0f;
    double cachedQ_ = 0.70710678, cachedK_ = 1.0 / 0.70710678, cachedSat_ = 1.0;
};

}  // namespace dcdsp
