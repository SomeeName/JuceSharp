// dcdsp_noisereducer.h
// -----------------------------------------------------------------------------
// NOISE REDUCER + HUM  -  header-only, RT-safe DSP core for the DotCharter
// native engine.
//
// Namespace : dcdsp
// Standard  : C++17
// Deps      : <cmath> <vector> <array> <algorithm> <cstddef> <cstdint>
//
// Effect:
//   * A smooth downward EXPANDER keyed on a user noise-floor estimate
//     (thresholdDb). Signal above the floor passes at ~unity; signal below the
//     floor is progressively attenuated with a soft knee - transparent, NOT a
//     hard gate. Expansion strength / ratio scales with `amount`. Recovery is
//     governed by `releaseMs`.
//   * Optional narrow NOTCH(es) at the mains hum frequency (`humFreq`) and its
//     integer harmonics to remove 50/60 Hz hum. Depth is continuous
//     (`humDepth` 0 = bypass, 1 = deep notch). Implemented as high-Q peaking
//     biquads with negative gain, so depth==0 is exactly flat.
//
// RT-safety contract:
//   * All allocation happens in prepare(). process() never allocates, locks,
//     or throws.
//   * Output is ALWAYS finite and bounded: every sample is NaN/Inf-guarded and
//     hard-clamped to |x| < 32 before it leaves process().
//
// Standalone note: the DotCharter engine ships a shared primitives header
// (Biquad[double coeffs], EnvelopeFollower, ...). This file is coded against
// those concepts but embeds a minimal self-contained stub in dcdsp::detail so
// it compiles on its own with no external deps.
// -----------------------------------------------------------------------------
#ifndef DCDSP_NOISEREDUCER_H
#define DCDSP_NOISEREDUCER_H

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared engine primitives (real Biquad + EnvelopeFollower).
#include "dcdsp_primitives.h"

namespace dcdsp {

// Uses the shared engine primitives (real dcdsp::Biquad peaking notches +
// dcdsp::EnvelopeFollower).

// ================================ params =====================================
struct NoiseReducerParams {
    // Downward-expansion strength. 0 = off (unity), 1 = strongest / most
    // gate-like. [0, 1] default 0.5
    float amount = 0.5f;
    // Noise-floor / expansion threshold in dBFS. Signal below this is
    // attenuated; above it passes at unity. [-80, -20] default -55
    float thresholdDb = -55.0f;
    // Mains hum fundamental frequency in Hz (50 = EU, 60 = US).
    // [50, 60] default 50
    float humFreq = 50.0f;
    // Hum notch depth. 0 = notches bypassed (flat), 1 = deepest notch.
    // [0, 1] default 0
    float humDepth = 0.0f;
    // Expander gain-recovery time in ms (how fast gain re-opens / floor decays).
    // [20, 500] default 120
    float releaseMs = 120.0f;
};

// ================================= core ======================================
class NoiseReducerCore {
public:
    void prepare(double sr, int maxBlock, int nch) {
        (void)maxBlock;
        sr_  = (sr > 0.0) ? sr : 48000.0;
        nch_ = (nch > 0) ? nch : 1;
        ch_.assign(static_cast<std::size_t>(nch_), ChannelState{});
        notchMix_.reset(sr_, kNotchFadeSeconds);
        recomputeNotches();
        notchMix_.snapToTarget();   // a freshly prepared reducer starts AT its state
        recomputeExpander();
        reset();
    }

    void reset() {
        for (auto& c : ch_) {
            for (auto& b : c.notch) b.reset();
            c.env.reset();
            c.gain = 1.0;
        }
        notchOn_ = false;   // the sections are clear; the next ON is not an edge
    }

    void setParams(const NoiseReducerParams& p) {
        // Clamp to documented ranges so process() can never see nonsense.
        p_.amount      = static_cast<float>(clampd(p.amount,       0.0,   1.0));
        p_.thresholdDb = static_cast<float>(clampd(p.thresholdDb, -80.0, -20.0));
        p_.humFreq     = static_cast<float>(clampd(p.humFreq,      50.0,  60.0));
        p_.humDepth    = static_cast<float>(clampd(p.humDepth,     0.0,   1.0));
        p_.releaseMs   = static_cast<float>(clampd(p.releaseMs,    20.0,  500.0));
        recomputeNotches();
        recomputeExpander();
    }

    // In-place. ch[c] points to nframes floats for channel c.
    void process(float* const* ch, int nch, int nframes) {
        if (!ch || nframes <= 0) return;
        const int chans = std::min(nch, nch_);
        // W4-D22: the notch bank is CROSSFADED in and out as well as cleared on
        // the way in. Clearing alone stops the Q = 50 ring, but the coefficients
        // still change in one sample, and at the hum frequency that is most of
        // the tone appearing or vanishing instantly. Channel-OUTER loop, so this
        // is blockStart/advanceBlock and never next() (JuceSharp/AGENTS.md).
        float notchStep = 0.f;
        const float notchBase = notchMix_.blockStart(nframes, notchStep);
        for (int c = 0; c < chans; ++c) {
            float* x = ch[c];
            if (!x) continue;
            ChannelState& st = ch_[static_cast<std::size_t>(c)];
            for (int n = 0; n < nframes; ++n) {
                double s = static_cast<double>(x[n]);
                if (!std::isfinite(s)) s = 0.0;

                // --- 1) Hum notch chain (peaking biquads, flat when depth==0) --
                const double preNotch = s;
                for (int h = 0; h < nActive_; ++h)
                    s = static_cast<double>(
                        st.notch[static_cast<std::size_t>(h)].process(static_cast<float>(s)));
                if (!std::isfinite(s)) s = 0.0;
                const double nw = static_cast<double>(
                    notchBase + notchStep * static_cast<float>(n));
                s = preNotch + nw * (s - preNotch);

                // --- 2) Downward expander keyed on the noise-floor threshold ---
                // Real EnvelopeFollower rectifies |x| internally.
                const double envLvl = static_cast<double>(st.env.process(static_cast<float>(s)));
                const double envDb  = 20.0 * std::log10(envLvl + 1e-12);
                const double target = staticGain(envDb);       // 0..1
                // Smooth the gain: attack fast when clamping down, release slow
                // when re-opening (release governed by releaseMs).
                const double a = (target < st.gain) ? gAtk_ : gRel_;
                st.gain = a * (st.gain - target) + target;
                // RECOVER before clamping: clampd is NaN-transparent (both `<`
                // and `>` are false for a NaN), and st.gain is PERSISTENT, so a
                // single non-finite value silences this channel for the life of
                // the node — the post-stage scrub then prints silence for ever
                // and a later valid setParams never repairs it. Unity is the
                // safe landing: the expander passes the signal through.
                if (!std::isfinite(st.gain)) st.gain = 1.0;
                st.gain = clampd(st.gain, 0.0, 1.0);

                s *= st.gain;

                // --- 3) Safety: finite + bounded |x| < 32 ----------------------
                if (!std::isfinite(s)) s = 0.0;
                s = clampd(s, -kCeil, kCeil);
                x[n] = static_cast<float>(s);
            }
        }
        notchMix_.advanceBlock(nframes);
    }

    int latencySamples() const { return 0; }

private:
    // ---- static downward-expander gain curve (dB in -> linear gain out) -----
    // Below `thresholdDb`, gain_dB = (R-1)*(envDb - threshold), soft-kneed.
    double staticGain(double envDb) const {
        const double W = kKneeDb;          // knee width (dB)
        const double delta = envDb - thresholdDb_;
        double grDb;
        if (delta >= 0.5 * W) {
            grDb = 0.0;                     // above floor: unity
        } else if (delta <= -0.5 * W) {
            grDb = (ratio_ - 1.0) * delta;  // full expansion (delta<0 -> grDb<0)
        } else {
            // quadratic soft knee, continuous value + slope with both branches
            const double d = delta - 0.5 * W;
            grDb = -(ratio_ - 1.0) * (d * d) / (2.0 * W);
        }
        if (grDb <= kMaxCutDb) grDb = kMaxCutDb; // bound max attenuation
        if (grDb >= 0.0) return 1.0;
        return dbToGain(grDb);   // exp2 form; see dcdsp_primitives.h
    }

    void recomputeExpander() {
        // amount -> expansion ratio in [1, kMaxRatio]; smoothing coefficients.
        ratio_       = 1.0 + static_cast<double>(p_.amount) * (kMaxRatio - 1.0);
        thresholdDb_ = static_cast<double>(p_.thresholdDb);
        for (auto& c : ch_) {
            c.env.prepare(sr_);
            c.env.setTimes(static_cast<float>(kEnvAtkMs), static_cast<float>(kEnvRelMs));
        }
        const double relMs = static_cast<double>(p_.releaseMs);
        gAtk_ = std::exp(-1.0 / (0.001 * kGainAtkMs * sr_));
        gRel_ = std::exp(-1.0 / (0.001 * relMs      * sr_));
    }

    void recomputeNotches() {
        // Build peaking notches at humFreq * h for as many harmonics as fit.
        const double f0    = static_cast<double>(p_.humFreq);
        const double depth = static_cast<double>(p_.humDepth);
        const double gainDb = -depth * kNotchMaxDb; // depth 0 -> 0 dB (flat)
        const double fMax   = 0.45 * sr_;
        const int prevActive = nActive_;
        nActive_ = 0;
        for (int h = 1; h <= kMaxHarmonics; ++h) {
            if (f0 * static_cast<double>(h) >= fMax) break;
            ++nActive_;
        }
        // W4-D22: CLEAR a section that is coming back ON before redesigning it.
        // At depth 0 these sections are flat (gainDb 0) but still RUNNING, so
        // their TDF-II state tracks the input; handing that state to a Q = 50
        // design is a splice into a resonator that rings for ~1/3 s at 50 Hz.
        // Two cases need it: the whole notch bank crossing off -> ON, and a
        // harmonic slot that was not in `nActive_` last time. This is the same
        // edge-reset shape dcdsp_overdrive.h uses for its preHp/mid sections
        // (`if (preHpEdge) c.preHp.reset();`). Going the other way needs
        // nothing — a deep notch's state is valid input to a flat design.
        const bool on   = depth > 0.0;
        const bool edge = on && !notchOn_;
        notchOn_ = on;
        notchMix_.setTarget(on ? 1.0f : 0.0f);

        // Configure each channel's peaking-notch biquads (shared design, own state).
        for (auto& c : ch_)
            for (int i = 0; i < nActive_; ++i) {
                if (edge || i >= prevActive) c.notch[static_cast<std::size_t>(i)].reset();
                c.notch[static_cast<std::size_t>(i)].setPeaking(
                    sr_, f0 * static_cast<double>(i + 1), kNotchQ, gainDb);
            }
    }

    // ---- constants ----------------------------------------------------------
    static constexpr int    kMaxHarmonics = 8;
    static constexpr double kNotchQ       = 50.0;   // narrow / surgical
    static constexpr double kNotchMaxDb   = 40.0;   // depth==1 -> -40 dB
    static constexpr double kMaxRatio     = 6.0;    // amount==1 -> 6:1 expander
    static constexpr double kKneeDb       = 8.0;    // soft-knee width
    static constexpr double kMaxCutDb     = -80.0;  // bound expander attenuation
    static constexpr double kEnvAtkMs     = 1.0;
    static constexpr double kEnvRelMs     = 15.0;
    static constexpr double kGainAtkMs    = 5.0;
    static constexpr double kCeil         = 31.999; // |x| < 32 guarantee

    struct ChannelState {
        std::array<Biquad, kMaxHarmonics> notch{};   // real primitive, per-channel state
        EnvelopeFollower env{};                      // real primitive
        double gain = 1.0;
    };

    static double clampd(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    double sr_  = 48000.0;
    int    nch_ = 1;
    bool   notchOn_ = false;   // humDepth > 0 last time recomputeNotches() ran
    static constexpr float kNotchFadeSeconds = 0.020f;
    SmoothedValue notchMix_;   // 0 = notches out of the path, 1 = fully in

    NoiseReducerParams p_{};

    // expander cache
    double ratio_       = 1.0 + 0.5 * (kMaxRatio - 1.0);
    double thresholdDb_ = -55.0;
    double gAtk_        = 0.0;
    double gRel_        = 0.0;

    // Active hum-harmonic count (per-channel biquad state lives in ChannelState).
    int nActive_ = 0;

    std::vector<ChannelState> ch_{};
};

} // namespace dcdsp

#endif // DCDSP_NOISEREDUCER_H
