// dcdsp_multitap.h — 1..4-tap rhythmic multitap delay (pure dcdsp core, RT-safe).
//
// Models a multi-head tape / bucket-brigade multitap: ONE mono delay line
// written from the channel sum, read at up to four taps whose offsets are
// `timeMs` times a fixed ratio set (`spread`), each tap panned alternately
// left/right and levelled by a geometric `decay`. The tap SUM (tone-shaped,
// DC-blocked, soft-limited) is the single feedback signal.
//
// Easy to get wrong: the tap COUNT. Dropping a tap is a full-amplitude splice
// in whatever that tap was playing, so `taps` is not a branch — every tap owns
// a SmoothedValue weight and a count change is a weighted fade (the shape
// dcdsp_chorus.h's `voices` uses). The same applies to the tap OFFSETS: the
// read position glides (one-pole, kGlideSeconds) so a `timeMs` or `spread`
// move is a pitch bend, never a jump.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe, NaN is
// rejected by the adapter before it ever reaches here.
struct MultitapParams {
    int   taps     = 3;      // 1 .. 4
    float timeMs   = 375.0f; // 10 .. 2000 — the BASE tap interval
    int   spread   = 0;      // 0 Even, 1 Dotted, 2 Triplet, 3 Golden
    float decay    = 0.6f;   // 0 .. 1 — per-tap level falloff
    float pan      = 0.7f;   // 0 .. 1 — alternating L/R width
    float feedback = 0.2f;   // 0 .. 0.9
    float tone     = 0.5f;   // 0 .. 1 — low-pass in the feedback path
    float mix      = 0.4f;   // 0 .. 1
};

class MultitapCore {
public:
    static constexpr int kMaxTaps = 4;

    // Tap offsets as multiples of the base time. Every set tops out at 4.0, so
    // the delay line is sized once for 4 x kMaxTimeMs and no spread can ask for
    // a read the line does not hold.
    static const float* tapRatios(int spread) {
        static constexpr float kEven[kMaxTaps]    = { 1.0f, 2.0f, 3.0f, 4.0f };
        static constexpr float kDotted[kMaxTaps]  = { 0.75f, 1.5f, 2.25f, 3.0f };
        static constexpr float kTriplet[kMaxTaps] = { 2.0f / 3.0f, 4.0f / 3.0f, 2.0f, 8.0f / 3.0f };
        // Non-repeating on purpose: successive powers of 1/phi and phi never
        // line up, so four golden taps never sound like a metre.
        static constexpr float kGolden[kMaxTaps]  = { 0.618034f, 1.0f, 1.618034f, 2.618034f };
        switch (spread) {
            case 1:  return kDotted;
            case 2:  return kTriplet;
            case 3:  return kGolden;
            default: return kEven;
        }
    }

    /// Fallback block bound when the host passes maxBlockSize <= 0. Zero meant
    /// NO oversized-block guard at all, which is the one case every other core
    /// in the wave substitutes a safe default for.
    static constexpr int kFallbackMaxBlock = 4096;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_    = numChannels < 1 ? 1 : numChannels;
        maxBlock_ = maxBlockSize > 0 ? maxBlockSize : kFallbackMaxBlock;

        // Worst case: the longest base time at the widest ratio, at THIS rate.
        maxDelaySamp_ = (int) std::ceil(kMaxTimeMs * 0.001 * kMaxRatio * sr_) + 8;
        line_.prepare(maxDelaySamp_);

        tone_.reset();
        dc_.prepare(sr_);

        // ~50 ms read-position glide, re-derived from the rate.
        glideCoef_ = (float) std::exp(-1.0 / (kGlideSeconds * sr_));

        for (int k = 0; k < kMaxTaps; ++k) {
            weight_[k].reset(sr_, kWeightRampSeconds);
            level_[k].reset(sr_, kLevelRampSeconds);
            panL_[k].reset(sr_, kLevelRampSeconds);
            panR_[k].reset(sr_, kLevelRampSeconds);
        }
        smMix_.reset(sr_, kLevelRampSeconds);
        smFb_.reset(sr_, kLevelRampSeconds);

        applyToneCorner();
        recomputeTargets();
        for (int k = 0; k < kMaxTaps; ++k) {
            weight_[k].snapToTarget();
            level_[k].snapToTarget();
            panL_[k].snapToTarget();
            panR_[k].snapToTarget();
            delay_[k] = targetDelay_[k];
        }
        smMix_.snapToTarget();
        smFb_.snapToTarget();
        primed_ = false;
        reset();
    }

    // State only. The params struct is USER CONTENT and is never touched here.
    void reset() {
        line_.reset();
        tone_.reset();
        dc_.reset();
        for (int k = 0; k < kMaxTaps; ++k) delay_[k] = targetDelay_[k];
    }

    void setParams(const MultitapParams& p) {
        p_.taps     = std::clamp(p.taps, 1, kMaxTaps);
        p_.timeMs   = clampf(p.timeMs, 10.0f, 2000.0f);
        p_.spread   = std::clamp(p.spread, 0, 3);
        p_.decay    = clampf(p.decay, 0.0f, 1.0f);
        p_.pan      = clampf(p.pan, 0.0f, 1.0f);
        p_.feedback = clampf(p.feedback, 0.0f, 0.9f);
        const float tone = clampf(p.tone, 0.0f, 1.0f);
        p_.mix      = clampf(p.mix, 0.0f, 1.0f);

        if (tone != p_.tone) { p_.tone = tone; applyToneCorner(); }
        recomputeTargets();

        // The FIRST setParams carries the caller's real settings and must land
        // instantly instead of gliding out of the defaults (dcdsp_delay.h's
        // `primed_`); every later one ramps.
        if (! primed_) {
            for (int k = 0; k < kMaxTaps; ++k) {
                weight_[k].snapToTarget();
                level_[k].snapToTarget();
                panL_[k].snapToTarget();
                panR_[k].snapToTarget();
                delay_[k] = targetDelay_[k];
            }
            smMix_.snapToTarget();
            smFb_.snapToTarget();
            primed_ = true;
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        // An oversized block is skipped WHOLE: processing the head and leaving
        // the tail dry splices full-level clean input into the middle of a
        // repeat, which is worse than one dry block.
        if (maxBlock_ > 0 && numFrames > maxBlock_) return;
        // The two guards every core in the other four drawers opens with, and
        // that this drawer's five dropped. DspChain passes a full set of valid
        // pointers today, so this is consistency and robustness rather than a
        // live crash -- but the RT gate calls process() directly and a null
        // `channels` was an access violation, not a skipped block. Truncating at
        // the first null rather than skipping past it is deliberate: multitap
        // and duckdelay SUM across channels before they write, so a hole in the
        // middle of the set is not a channel this core can render around.
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        int ch = numChannels < numCh_ ? numChannels : numCh_;
        for (int c = 0; c < ch; ++c) if (channels[c] == nullptr) { ch = c; break; }
        if (ch <= 0) return;

        for (int n = 0; n < numFrames; ++n) {
            const float wet = smMix_.next();
            const float dry = 1.0f - wet;
            const float fb  = smFb_.next();

            float in = 0.0f;
            for (int c = 0; c < ch; ++c) in += channels[c][n];
            in *= (ch > 1) ? 0.5f : 1.0f;

            float sumL = 0.0f, sumR = 0.0f, sumMono = 0.0f, norm = 0.0f;
            for (int k = 0; k < kMaxTaps; ++k) {
                delay_[k] = targetDelay_[k] + (delay_[k] - targetDelay_[k]) * glideCoef_;
                const float w = weight_[k].next();
                const float g = level_[k].next() * w;
                const float l = panL_[k].next();
                const float r = panR_[k].next();
                if (w <= 0.0f) continue;          // a parked tap costs one read
                const float s = line_.readAt(delay_[k]);
                sumL    += s * g * l;
                sumR    += s * g * r;
                sumMono += s * g;
                norm    += g;
            }

            // The feedback signal is the tap sum NORMALISED by the weights that
            // produced it, so adding a tap cannot raise the loop gain: with
            // feedback <= 0.9 and a soft limiter at unity the loop can never
            // reach 1.
            const float fed = norm > 1.0e-4f ? sumMono / norm : 0.0f;
            const float shaped = softLimit(dc_.process(tone_.process(fed)));
            line_.push(in + fb * shaped);

            if (ch > 1) {
                channels[0][n] = dry * channels[0][n] + wet * sumL;
                channels[1][n] = dry * channels[1][n] + wet * sumR;
                for (int c = 2; c < ch; ++c)
                    channels[c][n] = dry * channels[c][n] + wet * (0.5f * (sumL + sumR));
            } else {
                channels[0][n] = dry * channels[0][n] + wet * (0.5f * (sumL + sumR));
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    void applyToneCorner() {
        // 500 Hz .. 12 kHz, logarithmic — the corner is rate-derived inside
        // OnePole, so a device change re-derives it here.
        const float hz = 500.0f * std::pow(24.0f, p_.tone);
        tone_.setLowpass(sr_, (double) hz);
    }

    void recomputeTargets() {
        smMix_.setTarget(p_.mix);
        smFb_.setTarget(p_.feedback);
        const float* ratios = tapRatios(p_.spread);
        const float base = (float) (p_.timeMs * 0.001 * sr_);
        const float falloff = 1.0f - 0.9f * p_.decay;
        for (int k = 0; k < kMaxTaps; ++k) {
            targetDelay_[k] = clampDelay(base * ratios[k]);
            weight_[k].setTarget(k < p_.taps ? 1.0f : 0.0f);
            level_[k].setTarget(std::pow(falloff, (float) k));
            // Equal-power alternating placement: tap 0 right, tap 1 left, ...
            const float pos = ((k & 1) == 0 ? 1.0f : -1.0f) * p_.pan;  // -1 .. +1
            const float a = 0.25f * (float) kPi * (pos + 1.0f);        // 0 .. pi/2
            panL_[k].setTarget(std::cos(a) * kPanNorm);
            panR_[k].setTarget(std::sin(a) * kPanNorm);
        }
    }

    float clampDelay(float s) const {
        const float maxd = (float) (maxDelaySamp_ - 4);
        if (! (s >= 1.0f)) s = 1.0f;
        if (s > maxd) s = maxd;
        return s;
    }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    // Bounded, monotonic, unity-slope at 0: the feedback path can run at 0.9
    // for ever without the line diverging.
    static float softLimit(float x) { return std::tanh(x); }

    static constexpr double kMaxTimeMs         = 2000.0;
    static constexpr float  kMaxRatio          = 4.0f;
    static constexpr double kGlideSeconds      = 0.05;
    static constexpr float  kWeightRampSeconds = 0.030f;
    static constexpr float  kLevelRampSeconds  = 0.015f;
    // cos/sin of pi/4 is 0.7071 at centre; normalise so pan 0 is unity per side.
    static constexpr float  kPanNorm = 1.41421356f;

    InterpolatedDelayLine line_;
    OnePole   tone_;
    DcBlocker dc_;
    SmoothedValue weight_[kMaxTaps], level_[kMaxTaps], panL_[kMaxTaps], panR_[kMaxTaps];
    SmoothedValue smMix_, smFb_;
    float targetDelay_[kMaxTaps]{};
    float delay_[kMaxTaps]{};
    MultitapParams p_{};
    double sr_ = 48000.0;
    int    numCh_ = 2;
    int    maxBlock_ = 0;
    int    maxDelaySamp_ = 0;
    float  glideCoef_ = 0.0f;
    bool   primed_ = false;
};

} // namespace dcdsp
