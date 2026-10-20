// dcdsp_talkbox.h — TALK BOX ("talkbox").
//
// Models the tube-and-mouth talk box (Heil / Framptone): a driver crams the amp
// signal up a plastic tube into the player's mouth, and the mouth's own resonant
// cavity — three formants — shapes it before the vocal mic picks it up. Here the
// "mouth" is three parallel resonant bandpasses whose centres slide along the
// a-e-i-o-u path, and `drive` is the saturation the little driver adds on the way
// in.
//
// Topology: dcdsp::StateVariableTPT x3 in PARALLEL (a real vocal tract is
// parallel-resonant, not cascaded), Q from the `resonance` knob, centres
// linearly interpolated between the five cardinal vowels. The formant table here
// is this core's OWN copy: the frequencies are the same Klatt/Csound vocal-tract
// data every vowel model uses, but the amplitude tilt is FIXED per formant
// (F1 > F2 > F3) rather than per vowel, so the two strongest peaks in the output
// are always F1 and F2 whatever the vowel.
//
// Easy to get wrong: `vowel` is a knob a player JUMPS, and every formant centre
// moves with it, so it has to be smoothed — a hard jump moves three high-Q
// resonators at once and clicks. It is smoothed here over 20 ms and the
// coefficients are redesigned only when the smoothed position actually moved,
// which is also what keeps three std::tan calls off every sample.
//
// The second trap: the LFO and the envelope OFFSET the knob, they do not
// replace it. Writing the modulator straight into the vowel smoother's target
// (what this core shipped with) makes `vowel` dead in both non-Manual modes,
// and in Env mode the sweep then runs 0 .. envSens, so `envSens = 0` pins the
// pedal on "a" whatever the knob says. dcdsp_svf.h's LFO/Env ADD to the smoothed
// cutoff and that is the mental model a player carries over.
//
// The third: the modulator's contribution must NOT go through the vowel
// smoother. `SmoothedValue::setTarget` recomputes its increment and reloads its
// step count on every call, so calling it once per SAMPLE degenerates a linear
// 20 ms ramp into a one-pole with tau = 20 ms (a ~8 Hz corner) — which silently
// low-passed the LFO, so the top third of a 10 Hz rate knob did progressively
// less than it said, and left `isRamping()` permanently true. The smoother now
// carries the KNOB alone (one setTarget per param write) and the modulator is
// added raw, because an LFO and a follower are already continuous.
//
// The fourth: `sweep` is a SWITCH between three modulation sources, so it
// carries three ramped weights over the three OFFSETS — the shape dcdsp_svf.h
// uses for its own `source`. Manual contributes exactly 0.
#pragma once

#include "dcdsp_primitives.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

struct TalkBoxParams {
    // 0 .. 1, a continuous sweep through A - E - I - O - U.
    float vowel = 0.5f;
    // 0 = Manual, 1 = Envelope, 2 = LFO. What moves the vowel. NAMED discrete.
    int   sweep = 0;
    float lfoRateHz = 0.5f;     // 0.05 .. 10
    float envSens = 0.5f;       // 0 .. 1
    // 0 .. 1 -> formant Q 2 .. 25.
    float resonance = 0.6f;
    // 0 .. 1. The driver's saturation, BEFORE the formants.
    float drive = 0.3f;
    float mix = 1.0f;
};

class TalkBoxCore {
public:
    static constexpr int kSweepManual = 0;
    static constexpr int kSweepEnv    = 1;
    static constexpr int kSweepLfo    = 2;

    // This core's OWN vowel table (A E I O U), deliberately not shared with
    // dcdsp_formant.h: F1/F2/F3 centres in Hz, and a FIXED per-formant tilt in
    // dB so the ordering of the peaks never depends on the vowel.
    static constexpr float kFormantHz[3][5] = {
        {  700.0f,  530.0f,  270.0f,  570.0f,  300.0f },   // F1
        { 1220.0f, 1840.0f, 2290.0f,  840.0f,  870.0f },   // F2
        { 2600.0f, 2480.0f, 3010.0f, 2410.0f, 2240.0f },   // F3
    };
    static constexpr float kFormantDb[3] = { 0.0f, -4.0f, -9.0f };

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 1;
        const int nch = (numChannels > 0) ? numChannels : 1;
        channels_.assign(static_cast<std::size_t>(nch), ChannelState{});   // sole allocation

        // Rate-owned: the LFO increment, the follower's time constants, every
        // resonator's tan() and the smoothers' step counts.
        lfo_.prepare(sr_);
        lfo_.setShape(Lfo::Sine);
        lfo_.setFrequency(params_.lfoRateHz);
        env_.prepare(sr_);
        env_.setTimes(kEnvAttackMs, kEnvReleaseMs);

        vowelSmooth_.reset(sr_, kVowelRampSeconds);
        vowelSmooth_.setTarget(params_.vowel);
        vowelSmooth_.snapToTarget();
        for (int m = 0; m < 3; ++m) {
            sweepWeight_[m].reset(sr_, kRampSeconds);
            sweepWeight_[m].setTarget(m == params_.sweep ? 1.0f : 0.0f);
            sweepWeight_[m].snapToTarget();
        }
        resSmooth_.reset(sr_, kRampSeconds);
        resSmooth_.setTarget(params_.resonance);
        resSmooth_.snapToTarget();
        driveSmooth_.reset(sr_, kRampSeconds);
        driveSmooth_.setTarget(params_.drive);
        driveSmooth_.snapToTarget();
        mixSmooth_.reset(sr_, kRampSeconds);
        mixSmooth_.setTarget(params_.mix);
        mixSmooth_.snapToTarget();

        lastVowel_ = -1.0f;      // force the first design
        lastRes_ = -1.0f;
        coeffCountdown_ = 0;
        reset();
    }

    void reset() {
        for (auto& c : channels_) {
            for (auto& f : c.svf) f.reset();
            c.satState = 0.0f;
        }
        lfo_.reset();
        env_.reset();
        lastVowel_ = -1.0f;
        lastRes_ = -1.0f;
        coeffCountdown_ = 0;
    }

    void setParams(const TalkBoxParams& p) {
        params_.vowel     = clamp01(p.vowel);
        params_.sweep     = clampi(p.sweep, 0, 2);
        params_.lfoRateHz = clampf(p.lfoRateHz, 0.05f, 10.0f);
        params_.envSens   = clamp01(p.envSens);
        params_.resonance = clamp01(p.resonance);
        params_.drive     = clamp01(p.drive);
        params_.mix       = clamp01(p.mix);

        vowelSmooth_.setTarget(params_.vowel);
        for (int m = 0; m < 3; ++m)
            sweepWeight_[m].setTarget(m == params_.sweep ? 1.0f : 0.0f);
        resSmooth_.setTarget(params_.resonance);
        driveSmooth_.setTarget(params_.drive);
        mixSmooth_.setTarget(params_.mix);
        lfo_.setFrequency(params_.lfoRateHz);
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;     // skipped WHOLE
        const int nch = std::min(numChannels, static_cast<int>(channels_.size()));
        if (nch <= 0) return;

        for (int n = 0; n < numFrames; ++n) {
            const float mix   = mixSmooth_.next();
            const float dryG  = 1.0f - mix;
            const float res   = resSmooth_.next();
            const float drive = driveSmooth_.next();

            // --- where the vowel is this sample ------------------------------
            float mono = 0.0f;
            for (int c = 0; c < nch; ++c) mono += channels[c][n];
            mono *= 1.0f / static_cast<float>(nch);
            if (!std::isfinite(mono)) mono = 0.0f;

            // Both modulators run in every mode, so neither resumes from a
            // frozen phase or a stale follower after a switch.
            const float lfoOff = kLfoDepth * lfo_.process();
            const float e = env_.process(mono) * 8.0f;
            const float amt = e / (1.0f + e);
            const float envOff = (std::isfinite(amt) ? amt : 0.0f) * params_.envSens;
            // The knob is the CENTRE; the modulator is an offset on top of it,
            // ramped in over the switch. sweepWeight_[kSweepManual] contributes
            // exactly 0, which makes Manual an endpoint of the same blend.
            const float off = sweepWeight_[kSweepLfo].next() * lfoOff
                            + sweepWeight_[kSweepEnv].next() * envOff;
            sweepWeight_[kSweepManual].next();
            const float vpos = clamp01(vowelSmooth_.next() + off);

            // --- redesign only when it moved ---------------------------------
            if (--coeffCountdown_ <= 0) {
                coeffCountdown_ = kCoeffInterval;
                if (std::fabs(vpos - lastVowel_) > 1.0e-5f || std::fabs(res - lastRes_) > 1.0e-5f) {
                    lastVowel_ = vpos;
                    lastRes_ = res;
                    design(vpos, res);
                }
            }

            // Unity small-signal gain (tanh(x*d)/d -> x), and BLENDED by `drive`
            // so the knob is continuous at 0 rather than stepping 8 % the
            // instant it leaves the stop.
            const float satD = 1.0f + drive * 11.0f;
            const float satInv = 1.0f / satD;

            for (int c = 0; c < nch; ++c) {
                ChannelState& cs = channels_[static_cast<std::size_t>(c)];
                float x = channels[c][n];
                if (!std::isfinite(x)) x = 0.0f;

                // The driver: soft saturation before the mouth, level-matched so
                // the knob is a TONE control and not also a 20 dB volume swing.
                const float sat = adaaTanh(x * satD, cs.satState) * satInv;
                float in = (1.0f - drive) * x + drive * sat;

                cs.svf[0].process(in);
                cs.svf[1].process(in);
                cs.svf[2].process(in);

                float wet = g_[0] * cs.svf[0].bp()
                          + g_[1] * cs.svf[1].bp()
                          + g_[2] * cs.svf[2].bp();
                if (!std::isfinite(wet)) { wet = 0.0f; for (auto& f : cs.svf) f.reset(); }

                float y = dryG * x + mix * (wet * kMakeup);
                if (!std::isfinite(y)) y = 0.0f;
                channels[c][n] = clampf(y, -24.0f, 24.0f);
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    struct ChannelState {
        std::array<StateVariableTPT, 3> svf{};
        float satState = 0.0f;
    };

    void design(float pos, float res) {
        float seg = pos * 4.0f;
        int i = static_cast<int>(seg);
        if (i < 0) i = 0;
        if (i > 3) i = 3;
        float t = seg - static_cast<float>(i);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        const double Q = 2.0 + static_cast<double>(res) * 23.0;    // 2 .. 25
        if (channels_.empty()) return;
        for (int f = 0; f < 3; ++f) {
            const float fc = kFormantHz[f][i] + (kFormantHz[f][i + 1] - kFormantHz[f][i]) * t;
            // Fold the bandpass 1/Q normalisation into the cached gain, so the
            // resonance knob is not also an output level. The dB tilt is a
            // constant table, so its gain is a constexpr table too.
            g_[f] = kFormantLin[f] * static_cast<float>(1.0 / Q);
            // set() costs a std::tan and a divide; the ARGUMENTS are identical
            // across channels, so it is paid ONCE and copied into the siblings.
            // dcdsp_primitives.h documents copyCoeffsFrom as bit-identical to
            // calling set() again, and design() runs every 16 samples for the
            // life of a sweeping node.
            const std::size_t fi = static_cast<std::size_t>(f);
            channels_[0].svf[fi].set(sr_, fc, Q);
            for (std::size_t c = 1; c < channels_.size(); ++c)
                channels_[c].svf[fi].copyCoeffsFrom(channels_[0].svf[fi]);
        }
    }

    static float clampf(float v, float lo, float hi) {
        if (!(v >= lo)) return lo;            // NaN lands here
        return (v > hi) ? hi : v;
    }
    static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }
    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    static constexpr float kRampSeconds = 0.010f;
    static constexpr float kVowelRampSeconds = 0.020f;
    static constexpr float kEnvAttackMs = 8.0f;
    static constexpr float kEnvReleaseMs = 120.0f;
    static constexpr int   kCoeffInterval = 16;    // ~0.33 ms at 48 kHz
    static constexpr float kMakeup = 2.4f;
    /// The LFO's swing around the `vowel` knob: +/- half the a-to-u path, so at
    /// the catalogue default (vowel 0.5) it still covers the whole of it.
    static constexpr float kLfoDepth = 0.5f;
    /// 10^(kFormantDb/20), so design() does not pay a pow on a constant table.
    static constexpr float kFormantLin[3] = { 1.0f, 0.63095734f, 0.35481339f };

    TalkBoxParams params_{};
    double sr_ = 48000.0;
    int    maxBlock_ = 1;
    std::vector<ChannelState> channels_;
    Lfo lfo_;
    EnvelopeFollower env_{};
    SmoothedValue vowelSmooth_;     // the KNOB only; the modulator is added raw
    SmoothedValue sweepWeight_[3];  // Manual / Envelope / LFO, ramped
    SmoothedValue resSmooth_;
    SmoothedValue driveSmooth_;
    SmoothedValue mixSmooth_;
    float g_[3] = { 0.0f, 0.0f, 0.0f };
    float lastVowel_ = -1.0f;
    float lastRes_ = -1.0f;
    int   coeffCountdown_ = 0;
};

// Out-of-line constexpr definitions (C++17: needed for ODR-use of the arrays).
constexpr float TalkBoxCore::kFormantHz[3][5];
constexpr float TalkBoxCore::kFormantDb[3];

}  // namespace dcdsp
