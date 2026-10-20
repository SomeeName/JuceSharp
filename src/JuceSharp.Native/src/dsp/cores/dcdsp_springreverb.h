// dcdsp_springreverb.h
//
// PURE header-only spring reverb core for the DotCharter native DSP tone-rig.
//
// namespace dcdsp, C++17, standard headers only.
// RT-safe: all allocation happens in prepare(); process() never allocates,
// locks, or throws. Output is ALWAYS finite and bounded (|x| < 32) — every
// feedback / non-linear path is guarded and a final safety clamp is applied.
//
// Model (0.16 rework): input -> short pre-dispersion allpass chain (initial
// "drip" onset) -> THREE parallel feedback tanks per channel with detuned
// delay lengths (a real reverb pan carries 2-3 springs of slightly different
// transit time). Each tank carries its own dispersion allpass chain INSIDE the
// feedback loop, so the chirp accumulates per lap — echo N is N times more
// smeared than echo 1, which is the characteristic spring "boing". A shared
// per-channel LFO (with a per-channel start-phase offset, so stereo tails
// decorrelate) wobbles each tank's read tap with per-tank scale/sign.
// Every loop element is allpass/lossy and feedback < 1, so the tanks are
// contractive and the tail always decays. Dry/wet mixed at the end.
//
// The DotCharter engine ships a shared primitives header (Biquad, OnePole,
// InterpolatedDelayLine, Lfo, DcBlocker, ...); effect-specific helpers live in
// the springrev namespace below.

#ifndef DCDSP_SPRINGREVERB_H
#define DCDSP_SPRINGREVERB_H

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared engine primitives (real OnePole/DcBlocker/Lfo/InterpolatedDelayLine).
#include "dcdsp_primitives.h"

namespace dcdsp {

// ------------------------------------------------------------------------
// Effect-specific building blocks (dispersion allpass, Schroeder diffuser,
// sanitiser). These are NOT shared primitives, so they live in a per-core
// namespace to avoid clashing with other cores' helpers.
// ------------------------------------------------------------------------
namespace springrev {

// Sanitize a sample: replace NaN/Inf with 0 and hard-limit magnitude.
inline float sanitize(float x, float limit) {
    if (!std::isfinite(x)) return 0.0f;
    if (x > limit) return limit;
    if (x < -limit) return -limit;
    return x;
}

// First-order allpass: H(z) = (a + z^-1) / (1 + a z^-1). Unity magnitude,
// frequency-dependent group delay => dispersion. Stable for |a| < 1.
class Allpass1 {
   public:
    void reset() { x1_ = 0.0; y1_ = 0.0; }
    void setCoeff(double a) { a_ = a; }
    inline double process(double x) {
        double y = a_ * x + x1_ - a_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }
   private:
    double a_ = 0.0;
    double x1_ = 0.0;
    double y1_ = 0.0;
};

// Schroeder allpass diffuser (fixed integer delay + feedback). Unity-magnitude,
// stable for |g| < 1. Allocation only in prepare().
class SchroederAllpass {
   public:
    void prepare(int len) {
        if (len < 2) len = 2;
        len_ = len;
        buf_.assign(static_cast<std::size_t>(len_), 0.0f);
        w_ = 0;
    }
    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        w_ = 0;
    }
    void setCoeff(double g) { g_ = g; }
    inline double process(double x) {
        double bufOut = static_cast<double>(buf_[static_cast<std::size_t>(w_)]);
        double v = x + (-g_) * bufOut;   // input to the delay
        double y = bufOut + g_ * v;      // allpass output
        buf_[static_cast<std::size_t>(w_)] = static_cast<float>(v);
        if (++w_ >= len_) w_ = 0;
        return y;
    }
   private:
    std::vector<float> buf_;
    int len_ = 0;
    int w_ = 0;
    double g_ = 0.5;
};

}  // namespace springrev

// ------------------------------------------------------------------------
// Parameters
// ------------------------------------------------------------------------
struct SpringReverbParams {
    // Tail length / feedback amount.            [min, max, default]
    float decay = 0.5f;  // [0, 1, 0.5]
    // Damping tone: 0 = dark, 1 = bright.       [0, 1, 0.5]
    float tone = 0.5f;   // [0, 1, 0.5]
    // Dispersion amount ("drip"/boing chirp).   [0, 1, 0.5]
    float drip = 0.5f;   // [0, 1, 0.5]
    // Dry/wet mix. 0 = dry, 1 = fully wet.      [0, 1, 0.3]
    float mix = 0.3f;    // [0, 1, 0.3]
};

// ------------------------------------------------------------------------
// SpringReverbCore
// ------------------------------------------------------------------------
class SpringReverbCore {
   public:
    static constexpr int kPreDispersionStages  = 8;   // input chain (onset drip)
    static constexpr int kLoopDispersionStages = 6;   // per-tank, per-lap chirp
    static constexpr int kNumTanks = 3;               // detuned parallel springs
    static constexpr int kMaxChannels = 8;
    static constexpr float kSafetyLimit = 16.0f;  // final clamp (< 32)

    void prepare(double sr, int /*maxBlock*/, int nch) {
        sr_ = (sr > 0.0) ? sr : 48000.0;
        if (nch < 1) nch = 1;
        if (nch > kMaxChannels) nch = kMaxChannels;
        nchAlloc_ = nch;

        // Detuned tank transit times (~spring travel, mutually non-harmonic)
        // plus modulation headroom + interp guard.
        modDepthMax_ = 0.0035 * sr_;  // up to ~3.5 ms swing for the boing
        for (int t = 0; t < kNumTanks; ++t)
            baseDelay_[t] = kTankDelaySec[t] * sr_;

        for (int c = 0; c < nchAlloc_; ++c) {
            ChannelState& s = ch_[c];
            for (int t = 0; t < kNumTanks; ++t) {
                TankState& tk = s.tank[t];
                tk.dc.prepareLegacy48k(sr_);
                int maxLen = static_cast<int>(baseDelay_[t] + modDepthMax_) + 8;
                tk.line.prepare(maxLen);
                // One short diffuser per tank smooths the lap echoes; lengths
                // detuned per tank so the diffusion doesn't comb identically.
                tk.diff.prepare(static_cast<int>(kDiffuserSec[t] * sr_) + 4);
                tk.diff.setCoeff(0.6);
            }
            s.lfo.prepare(sr_);
            s.lfo.setShape(Lfo::Sine);
            s.lfo.setFrequency(static_cast<float>(kLfoRate));
        }
        reset();
        // `mix` RAMPS (kMixRampSeconds); as a per-block constant a mix drag
        // stepped the whole dry/wet balance at every block edge.
        mixS_.reset(sr_, kMixRampSeconds);
        modDepthS_.reset(sr_, 0.020f);
        setParams(params_);
        mixS_.snapToTarget();   // a freshly prepared spring starts AT its mix
    }

    void reset() {
        for (int c = 0; c < nchAlloc_; ++c) {
            ChannelState& s = ch_[c];
            for (int t = 0; t < kNumTanks; ++t) {
                TankState& tk = s.tank[t];
                tk.line.reset();
                tk.diff.reset();
                tk.damp.reset();
                tk.dc.reset();
                for (auto& ap : tk.disp) ap.reset();
            }
            for (auto& ap : s.preDisp) ap.reset();
            s.lfo.reset();
            // Per-channel LFO start-phase offset (golden-ratio spaced) so the
            // stereo tails wobble out of phase and decorrelate.
            s.lfo.setPhase(static_cast<float>(c) * 0.381966f);
        }
    }

    void setParams(const SpringReverbParams& p) {
        params_ = p;
        float decay = clamp01(p.decay);
        float tone = clamp01(p.tone);
        float drip = clamp01(p.drip);
        mix_ = clamp01(p.mix);
        mixS_.setTarget(mix_);

        // Feedback strictly < 1 for a contractive tank. 0.55 .. 0.93.
        feedback_ = 0.55f + 0.38f * decay;

        // Damping cutoff: dark (900 Hz) -> bright (11 kHz).
        double cutoff = 900.0 + tone * 10100.0;
        for (int c = 0; c < nchAlloc_; ++c)
            for (int t = 0; t < kNumTanks; ++t)
                ch_[c].tank[t].damp.setLowpass(sr_, cutoff);

        // Dispersion coefficient: 0 (no chirp) -> -0.78 (strong boing).
        // Negative coeff gives the upward-glide spring chirp. The loop chain
        // uses a slightly milder coefficient than the input chain: its group
        // delay is re-applied EVERY lap, so a full-strength coefficient there
        // would smear the tail into mush at high drip.
        double aPre  = -0.78 * static_cast<double>(drip);
        double aLoop = -0.62 * static_cast<double>(drip);
        for (int c = 0; c < nchAlloc_; ++c) {
            ChannelState& s = ch_[c];
            for (auto& ap : s.preDisp) ap.setCoeff(aPre);
            for (int t = 0; t < kNumTanks; ++t)
                for (auto& ap : s.tank[t].disp) ap.setCoeff(aLoop);
        }

        // Modulation depth grows with drip for a more "sproingy" tail.
        modDepth_ = modDepthMax_ * (0.15 + 0.85 * static_cast<double>(drip));
        // `drip` is ADDED to a read tap, so a write moves it instantly by
        // delta * |lfo| -- 143 samples at 48 kHz. The allpass coefficients it
        // also sets are filter coefficients and stay stepped; this is a tap.
        modDepthS_.setTarget(static_cast<float>(modDepth_));
    }

    // In-place processing. ch[c] is a buffer of nframes floats.
    void process(float* const* ch, int nch, int nframes) {
        if (nch > nchAlloc_) nch = nchAlloc_;

        // Channel-OUTER loop: one ramp line for the whole block, handed to every
        // channel, and the smoother advanced once afterwards.
        float mixStep = 0.0f;
        const float mixBase = mixS_.blockStart(nframes, mixStep);
        float mdStep = 0.0f;
        const float mdBase = modDepthS_.blockStart(nframes, mdStep);

        for (int c = 0; c < nch; ++c) {
            ChannelState& s = ch_[c];
            float* buf = ch[c];
            const float fb = feedback_;

            for (int n = 0; n < nframes; ++n) {
                const double md = static_cast<double>(mdBase + mdStep * static_cast<float>(n));
                float dry = springrev::sanitize(buf[n], kSafetyLimit);
                double x = static_cast<double>(dry);

                // ---- Pre-tank dispersion (onset "drip") ----
                for (auto& ap : s.preDisp) x = ap.process(x);

                // Shared per-channel LFO, per-tank scale/sign decorrelation.
                const double lfo = static_cast<double>(s.lfo.process());

                double wetSum = 0.0;
                for (int t = 0; t < kNumTanks; ++t) {
                    TankState& tk = s.tank[t];

                    double mod = lfo * kTankModScale[t] * md;
                    double readPos = baseDelay_[t] + mod;
                    double delayed = static_cast<double>(
                        tk.line.readAt(static_cast<float>(readPos)));

                    // ---- In-loop dispersion: chirp accumulates per lap ----
                    for (auto& ap : tk.disp) delayed = ap.process(delayed);

                    // Diffuse the delayed signal for a denser tail.
                    double diffused = tk.diff.process(delayed);

                    // Damp (tone) + DC-block in the loop.
                    double damped = static_cast<double>(
                        tk.damp.process(static_cast<float>(diffused)));
                    damped = static_cast<double>(
                        tk.dc.process(static_cast<float>(damped)));

                    // Tank input: injected dispersed signal + damped feedback.
                    double tankIn = x + static_cast<double>(fb) * damped;
                    // Guard the feedback path before it re-enters the delay.
                    float tankInF = springrev::sanitize(
                        static_cast<float>(tankIn), kSafetyLimit);
                    tk.line.push(tankInF);

                    wetSum += damped;
                }

                // Wet compensation: 3 decorrelated tanks sum ~sqrt(3) in
                // amplitude, so 1.4/sqrt(3) keeps the wet level where the old
                // single-tank core sat.
                const float wet     = mixBase + mixStep * static_cast<float>(n);
                const float dryGain = 1.0f - wet;
                const float wetGain = wet * 0.8083f;
                double out = static_cast<double>(dryGain) * dry +
                             static_cast<double>(wetGain) * wetSum;

                buf[n] = springrev::sanitize(static_cast<float>(out),
                                             kSafetyLimit);
            }
        }

        mixS_.advanceBlock(nframes);
        modDepthS_.advanceBlock(nframes);
    }

    int latencySamples() const { return 0; }

   private:
    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.0f;
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    }

    static constexpr double kLfoRate = 0.9;  // Hz, slow tail wobble

    // Detuned spring transit times and per-tank diffuser lengths (seconds).
    static constexpr double kTankDelaySec[kNumTanks] = { 0.0372, 0.0431, 0.0509 };
    static constexpr double kDiffuserSec[kNumTanks]  = { 0.0047, 0.0059, 0.0071 };
    // Per-tank modulation scale/sign (same LFO, decorrelated wobble).
    static constexpr double kTankModScale[kNumTanks] = { 1.0, -0.78, 0.55 };

    struct TankState {
        InterpolatedDelayLine line{};                      // real primitive
        std::array<springrev::Allpass1, kLoopDispersionStages> disp{};
        springrev::SchroederAllpass diff{};
        OnePole damp{};                                    // damping low-pass
        DcBlocker dc{};                                    // real primitive
    };

    struct ChannelState {
        std::array<springrev::Allpass1, kPreDispersionStages> preDisp{};
        std::array<TankState, kNumTanks> tank{};
        Lfo lfo{};                      // real primitive
    };

    double sr_ = 48000.0;
    int nchAlloc_ = 0;
    double baseDelay_[kNumTanks] = { 0.0, 0.0, 0.0 };
    double modDepth_ = 0.0;   // the TARGET; modDepthS_ carries the ramp
    SmoothedValue modDepthS_;
    double modDepthMax_ = 0.0;
    float feedback_ = 0.7f;
    float mix_ = 0.3f;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixS_;

    SpringReverbParams params_{};
    std::array<ChannelState, kMaxChannels> ch_{};
};

// Out-of-line constexpr definitions (C++17: needed for ODR-use of the arrays).
constexpr double SpringReverbCore::kTankDelaySec[SpringReverbCore::kNumTanks];
constexpr double SpringReverbCore::kDiffuserSec[SpringReverbCore::kNumTanks];
constexpr double SpringReverbCore::kTankModScale[SpringReverbCore::kNumTanks];

}  // namespace dcdsp

#endif  // DCDSP_SPRINGREVERB_H
