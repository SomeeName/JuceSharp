// dcdsp_duckdelay.h — ducking (dynamic) delay (pure dcdsp core, RT-safe).
//
// The studio trick that keeps a long repeat from burying the part that made
// it: a detector watches the DRY input and pulls the REPEATS down by `duckDb`
// while you are playing, then lets them swell back at `releaseMs` when you
// stop. Per-channel InterpolatedDelayLine, one-pole tone + DC blocker + soft
// limiter in the feedback path, feedback clamped to 0.9.
//
// Easy to get wrong: WHERE the duck is applied. It scales the delay line's
// OUTPUT, never what is written into it — duck the write and the repeats are
// permanently quiet, because the attenuation is baked into the recording and
// nothing can swell back. Applied at the read, the tail is always full-level
// and only the monitoring of it moves, which is the whole effect.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe.
struct DuckDelayParams {
    float timeMs      = 400.0f; // 10 .. 2000
    float feedback    = 0.35f;  // 0 .. 0.9
    float duckDb      = 12.0f;  // 0 .. 40 — how far the repeats drop
    float sensitivity = 0.5f;   // 0 .. 1  — detector threshold
    float releaseMs   = 300.0f; // 20 .. 2000 — how fast they swell back
    float tone        = 0.5f;   // 0 .. 1  — low-pass in the feedback path
    float mix         = 0.4f;   // 0 .. 1
};

class DuckDelayCore {
public:
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
        tone_.assign((std::size_t) numCh_, OnePole{});
        dc_.assign((std::size_t) numCh_, DcBlocker{});
        for (auto& d : dc_) d.prepare(sr_);

        // The detector is rate-owned on BOTH coefficients; EnvelopeFollower
        // re-derives them from the rate it is prepared at.
        det_.prepare(sr_);
        glideCoef_ = (float) std::exp(-1.0 / (kGlideSeconds * sr_));

        smMix_.reset(sr_, kLevelRampSeconds);
        smFb_.reset(sr_, kLevelRampSeconds);
        // The duck itself: a per-sample gain, so it walks rather than steps
        // even when the detector snaps across the threshold.
        duckGain_.reset(sr_, kDuckRampSeconds);
        duckGain_.setCurrentAndTarget(1.0f);

        applyDerived();
        smMix_.snapToTarget();
        smFb_.snapToTarget();
        delay_ = targetDelay_;
        primed_ = false;
        reset();
    }

    // State only — never the params.
    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& t : tone_) t.reset();
        for (auto& d : dc_) d.reset();
        det_.reset();
        delay_ = targetDelay_;
        duckGain_.setCurrentAndTarget(1.0f);
    }

    void setParams(const DuckDelayParams& p) {
        p_.timeMs      = clampf(p.timeMs, 10.0f, 2000.0f);
        p_.feedback    = clampf(p.feedback, 0.0f, 0.9f);
        p_.duckDb      = clampf(p.duckDb, 0.0f, 40.0f);
        p_.sensitivity = clampf(p.sensitivity, 0.0f, 1.0f);
        p_.releaseMs   = clampf(p.releaseMs, 20.0f, 2000.0f);
        p_.tone        = clampf(p.tone, 0.0f, 1.0f);
        p_.mix         = clampf(p.mix, 0.0f, 1.0f);
        applyDerived();
        if (! primed_) {
            smMix_.snapToTarget();
            smFb_.snapToTarget();
            delay_ = targetDelay_;
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

        const float duckFloor = dbToGain(-p_.duckDb);

        for (int n = 0; n < numFrames; ++n) {
            const float wet = smMix_.next();
            const float dry = 1.0f - wet;
            const float fb  = smFb_.next();
            delay_ = targetDelay_ + (delay_ - targetDelay_) * glideCoef_;

            // Detector: the DRY input, summed across channels. Peak-following
            // with a fast attack and the user's release, so "still playing"
            // keeps the duck down and silence lets it climb back.
            float key = 0.0f;
            for (int c = 0; c < ch; ++c) key = std::max(key, std::abs(channels[c][n]));
            const float env = det_.process(key);
            duckGain_.setTarget(duckFloor + (1.0f - duckFloor) * (1.0f - overThreshold(env)));
            const float duck = duckGain_.next();

            for (int c = 0; c < ch; ++c) {
                const auto uc = (std::size_t) c;
                const float x = channels[c][n];
                const float echo = lines_[uc].readAt(delay_);
                const float shaped = softLimit(dc_[uc].process(tone_[uc].process(echo)));
                lines_[uc].push(x + fb * shaped);
                channels[c][n] = dry * x + wet * duck * echo;
            }
        }
    }

    int latencySamples() const { return 0; }

    /// The duck's CURRENT gain, for tests and meters. Not a param.
    float duckGainForTest() const { return duckGain_.current(); }

private:
    void applyDerived() {
        smMix_.setTarget(p_.mix);
        smFb_.setTarget(p_.feedback);
        targetDelay_ = clampDelay((float) (p_.timeMs * 0.001 * sr_));
        // Threshold: sensitivity 0 listens only to a loud attack (-6 dBFS),
        // sensitivity 1 to almost anything (-60 dBFS).
        thresholdDb_ = -6.0f - 54.0f * p_.sensitivity;
        det_.setTimes(kAttackMs, p_.releaseMs);
        const float hz = 500.0f * std::pow(24.0f, p_.tone);
        for (auto& t : tone_) t.setLowpass(sr_, (double) hz);
    }

    // 0 = below the threshold (no duck), 1 = a knee above it (full duck).
    float overThreshold(float env) const {
        const float db = gainToDb(env);
        const float over = (db - thresholdDb_) * (1.0f / kKneeDb);
        return over < 0.0f ? 0.0f : (over > 1.0f ? 1.0f : over);
    }

    float clampDelay(float s) const {
        const float maxd = (float) (maxDelaySamp_ - 4);
        if (! (s >= 1.0f)) s = 1.0f;
        if (s > maxd) s = maxd;
        return s;
    }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
    static float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 1.0e-9f)); }
    static float softLimit(float x) { return std::tanh(x); }

    static constexpr double kMaxTimeMs        = 2000.0;
    static constexpr double kGlideSeconds     = 0.05;
    static constexpr float  kLevelRampSeconds = 0.015f;
    static constexpr float  kDuckRampSeconds  = 0.005f;
    static constexpr float  kAttackMs         = 5.0f;
    static constexpr float  kKneeDb           = 6.0f;

    std::vector<InterpolatedDelayLine> lines_;
    std::vector<OnePole>   tone_;
    std::vector<DcBlocker> dc_;
    EnvelopeFollower det_;
    SmoothedValue smMix_, smFb_, duckGain_;
    DuckDelayParams p_{};
    double sr_ = 48000.0;
    int    numCh_ = 2;
    int    maxBlock_ = 0;
    int    maxDelaySamp_ = 0;
    float  glideCoef_ = 0.0f;
    float  targetDelay_ = 0.0f;
    float  delay_ = 0.0f;
    float  thresholdDb_ = -33.0f;
    bool   primed_ = false;
};

} // namespace dcdsp
