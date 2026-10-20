// dcdsp_rhythmdelay.h — rhythmic tremolo delay (pure dcdsp core, RT-safe).
//
// A delay whose REPEATS are chopped by an eight-step gate sequencer: the tail
// comes back as a rhythm rather than a smear. Per-channel
// InterpolatedDelayLine with a DC blocker + soft limiter in the feedback path,
// and a step gate on the WET READ ONLY — outside the loop, so the pattern
// shapes what you hear without compounding itself once per lap.
//
// Easy to get wrong: the gate edge. A hard 1 -> 0 at a step boundary is a
// full-amplitude click 8 times a bar, so every step edge is a ramp whose
// length is `gateShape` of the step (never shorter than kMinSlopeMs), and a
// PATTERN change is nothing special — it only moves the smoother's target, so
// switching patterns mid-step crossfades for free.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe.
struct RhythmDelayParams {
    float timeMs    = 500.0f; // 10 .. 2000
    float feedback  = 0.4f;   // 0 .. 0.9
    int   pattern   = 0;      // 0 .. 7, see kPatterns
    float stepMs    = 125.0f; // 50 .. 1000 — one step of the sequencer
    float gateDepth = 1.0f;   // 0 .. 1 — 0 is a true no-op
    float gateShape = 0.2f;   // 0 .. 1 — edge slope, as a fraction of the step
    float mix       = 0.5f;   // 0 .. 1
};

class RhythmDelayCore {
public:
    static constexpr int kPatternCount = 8;
    static constexpr int kSteps        = 8;

    // Bit (kSteps - 1 - i) is step i, so the literal reads left to right the
    // way the label does.
    static uint8_t patternBits(int index) {
        static constexpr uint8_t kPatterns[kPatternCount] = {
            0b10101010, // "x.x.x.x."
            0b11011010, // "xx.xx.x."
            0b10010010, // "x..x..x."
            0b11101110, // "xxx.xxx."
            0b10110101, // "x.xx.x.x"
            0b10001000, // "x...x..."
            0b11110000, // "xxxx...."
            0b10101100, // "x.x.xx.."
        };
        return kPatterns[std::clamp(index, 0, kPatternCount - 1)];
    }

    /// Fallback block bound when the host passes maxBlockSize <= 0. Zero meant
    /// NO oversized-block guard at all, which is the one case every other core
    /// in the wave substitutes a safe default for.
    static constexpr int kFallbackMaxBlock = 4096;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_    = numChannels < 1 ? 1 : numChannels;
        maxBlock_ = maxBlockSize > 0 ? maxBlockSize : kFallbackMaxBlock;

        maxDelaySamp_ = (int) std::ceil(kMaxTimeMs * 0.001 * sr_) + 8;
        lines_.resize((std::size_t) numCh_);
        for (auto& l : lines_) l.prepare(maxDelaySamp_);
        dc_.assign((std::size_t) numCh_, DcBlocker{});
        for (auto& d : dc_) d.prepare(sr_);

        glideCoef_ = (float) std::exp(-1.0 / (kGlideSeconds * sr_));
        smMix_.reset(sr_, kLevelRampSeconds);
        smFb_.reset(sr_, kLevelRampSeconds);
        gate_.setCurrentAndTarget(1.0f);

        slopeSeconds_ = -1.0f;         // force the gate ramp to be re-derived
        applyDerived();
        smMix_.snapToTarget();
        smFb_.snapToTarget();
        delay_ = targetDelay_;
        primed_ = false;
        reset();
    }

    // State only — the pattern, the step length and the user's time survive.
    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& d : dc_) d.reset();
        delay_ = targetDelay_;
        step_ = 0;
        // A FULL step, not 0: the counter is decremented before it is tested,
        // so starting at 0 advanced to step 1 on the very first sample and the
        // sequencer ran a step ahead of the pattern for ever.
        stepCounter_ = stepSamples_;
        gate_.setCurrentAndTarget(gateFor(0));
    }

    void setParams(const RhythmDelayParams& p) {
        p_.timeMs    = clampf(p.timeMs, 10.0f, 2000.0f);
        p_.feedback  = clampf(p.feedback, 0.0f, 0.9f);
        p_.pattern   = std::clamp(p.pattern, 0, kPatternCount - 1);
        p_.stepMs    = clampf(p.stepMs, 50.0f, 1000.0f);
        p_.gateDepth = clampf(p.gateDepth, 0.0f, 1.0f);
        p_.gateShape = clampf(p.gateShape, 0.0f, 1.0f);
        p_.mix       = clampf(p.mix, 0.0f, 1.0f);
        applyDerived();
        // A pattern / depth edit re-targets the SMOOTHER rather than writing
        // the gate, which is what makes a pattern change a crossfade.
        gate_.setTarget(gateFor(step_));
        if (! primed_) {
            smMix_.snapToTarget();
            smFb_.snapToTarget();
            delay_ = targetDelay_;
            gate_.snapToTarget();
            primed_ = true;
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
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
            delay_ = targetDelay_ + (delay_ - targetDelay_) * glideCoef_;

            if (--stepCounter_ <= 0) {
                stepCounter_ = stepSamples_;
                step_ = (step_ + 1) % kSteps;
                gate_.setTarget(gateFor(step_));
            }
            const float gate = gate_.next();

            for (int c = 0; c < ch; ++c) {
                const auto uc = (std::size_t) c;
                const float x = channels[c][n];
                const float echo = lines_[uc].readAt(delay_);
                lines_[uc].push(x + fb * softLimit(dc_[uc].process(echo)));
                // gate is EXACTLY 1.0f at gateDepth 0, so the multiply is a
                // no-op bit for bit and the module is a plain delay.
                channels[c][n] = dry * x + wet * gate * echo;
            }
        }
    }

    int latencySamples() const { return 0; }

    /// The step the sequencer is on, for tests. Not a param.
    int stepForTest() const { return step_; }

private:
    void applyDerived() {
        smMix_.setTarget(p_.mix);
        smFb_.setTarget(p_.feedback);
        targetDelay_ = clampDelay((float) (p_.timeMs * 0.001 * sr_));
        stepSamples_ = std::max(1, (int) std::lround(p_.stepMs * 0.001 * sr_));
        // A SHORTER step takes effect on the step in flight rather than after
        // it: prepare() runs before the caller's params arrive, so the first
        // step would otherwise last the DEFAULT 125 ms and every step after it
        // would sit at a fixed offset from where the sequencer says it is.
        if (stepCounter_ > stepSamples_) stepCounter_ = stepSamples_;
        // The edge slope is a FRACTION of the step, floored at kMinSlopeMs so
        // gateShape 0 is the sharpest chop the core allows and not a click.
        const float slope = std::max(p_.gateShape * p_.stepMs, kMinSlopeMs) * 0.001f;
        if (slope != slopeSeconds_) {
            slopeSeconds_ = slope;
            gate_.reset(sr_, slope);   // keeps current/target; re-derives the step count
        }
    }

    float gateFor(int step) const {
        const bool on = (patternBits(p_.pattern) >> (kSteps - 1 - step)) & 1u;
        return on ? 1.0f : (1.0f - p_.gateDepth);
    }

    float clampDelay(float s) const {
        const float maxd = (float) (maxDelaySamp_ - 4);
        if (! (s >= 1.0f)) s = 1.0f;
        if (s > maxd) s = maxd;
        return s;
    }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float softLimit(float x) { return std::tanh(x); }

    static constexpr double kMaxTimeMs        = 2000.0;
    static constexpr double kGlideSeconds     = 0.05;
    static constexpr float  kLevelRampSeconds = 0.015f;
    static constexpr float  kMinSlopeMs       = 1.0f;

    std::vector<InterpolatedDelayLine> lines_;
    std::vector<DcBlocker> dc_;
    SmoothedValue smMix_, smFb_, gate_;
    RhythmDelayParams p_{};
    double sr_ = 48000.0;
    int    numCh_ = 2;
    int    maxBlock_ = 0;
    int    maxDelaySamp_ = 0;
    int    stepSamples_ = 1;
    int    stepCounter_ = 0;
    int    step_ = 0;
    float  glideCoef_ = 0.0f;
    float  targetDelay_ = 0.0f;
    float  delay_ = 0.0f;
    float  slopeSeconds_ = -1.0f;
    bool   primed_ = false;
};

} // namespace dcdsp
