// dcdsp_bbddelay.h — analog bucket-brigade (BBD) delay (pure dcdsp core, RT-safe).
//
// An MN3005-style analog echo. A BBD is a fixed number of charge buckets
// clocked at a variable rate, so the SAMPLE RATE of the delay line IS the
// delay time: long settings clock slowly, and the anti-alias filter before the
// line and the reconstruction filter after it both have to track that clock
// down with it. That is why an analog delay gets darker as it gets longer, and
// why every repeat is darker than the one before — the filter pair is inside
// the feedback loop and compounds once per lap.
//
// Easy to get wrong: the time knob, in TWO places. (1) A BBD cannot jump;
// changing the clock drags the read head and the pitch with it, so the glide
// here is SLEW-LIMITED (kMaxSlew samples of delay per sample of audio) — an
// exponential glide alone would ask for a read rate below zero and read the
// line backwards. (2) The filter pair has to follow the read head DOWN WITH
// IT, not jump to the new corner the instant the knob moves: redesigning a
// biquad from 4.8 kHz to 800 Hz in one sample is a phase discontinuity in a
// stateful filter, i.e. exactly the click the slew limiter exists to avoid.
// The corner is therefore re-derived from the CURRENT, gliding delay whenever
// it has moved kRetuneTolerance, which is ~140 cheap redesigns across a full
// sweep and none at all while the knob is still.
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
struct BbdDelayParams {
    float timeMs    = 300.0f; // 20 .. 600
    float feedback  = 0.4f;   // 0 .. 1 — self-oscillation allowed, bounded
    float modRateHz = 0.5f;   // 0.1 .. 5
    float modDepth  = 0.1f;   // 0 .. 1
    float age       = 0.3f;   // 0 .. 1 — clock noise + extra bandwidth loss
    float mix       = 0.4f;   // 0 .. 1
};

class BbdDelayCore {
public:
    /// Fallback block bound when the host passes maxBlockSize <= 0. Zero meant
    /// NO oversized-block guard at all, which is the one case every other core
    /// in the wave substitutes a safe default for.
    static constexpr int kFallbackMaxBlock = 4096;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_    = numChannels < 1 ? 1 : numChannels;
        maxBlock_ = maxBlockSize > 0 ? maxBlockSize : kFallbackMaxBlock;

        // Worst case: the longest time plus the deepest modulation, at THIS rate.
        maxDelaySamp_ = (int) std::ceil((kMaxTimeMs + kMaxModMs) * 0.001 * sr_) + 8;
        lines_.resize((std::size_t) numCh_);
        for (auto& l : lines_) l.prepare(maxDelaySamp_);
        aa_.assign((std::size_t) numCh_, Biquad{});
        recon_.assign((std::size_t) numCh_, Biquad{});
        dc_.assign((std::size_t) numCh_, DcBlocker{});
        for (auto& d : dc_) d.prepare(sr_);

        lfo_.prepare(sr_);
        lfo_.setShape(Lfo::Sine);

        smMix_.reset(sr_, kLevelRampSeconds);
        smFb_.reset(sr_, kLevelRampSeconds);
        smNoise_.reset(sr_, kLevelRampSeconds);
        smDepth_.reset(sr_, kLevelRampSeconds);
        // `age` moves the same corner the clock does, so it glides too rather
        // than redesigning the pair under a running tail.
        smAge_.reset(sr_, kAgeRampSeconds);

        rng_ = kSeed;                      // deterministic clock noise
        applyDerived();
        smMix_.snapToTarget();
        smFb_.snapToTarget();
        smNoise_.snapToTarget();
        smDepth_.snapToTarget();
        smAge_.snapToTarget();
        delay_ = targetDelay_;
        retuneClockFilters(delay_, smAge_.current());
        primed_ = false;
        reset();
    }

    // State only — the params, including the user's time, survive a device change.
    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& f : aa_) f.reset();
        for (auto& f : recon_) f.reset();
        for (auto& d : dc_) d.reset();
        lfo_.reset();
        rng_ = kSeed;
        delay_ = targetDelay_;
    }

    void setParams(const BbdDelayParams& p) {
        p_.timeMs    = clampf(p.timeMs, 20.0f, 600.0f);
        p_.feedback  = clampf(p.feedback, 0.0f, 1.0f);
        p_.modRateHz = clampf(p.modRateHz, 0.1f, 5.0f);
        p_.modDepth  = clampf(p.modDepth, 0.0f, 1.0f);
        p_.age       = clampf(p.age, 0.0f, 1.0f);
        p_.mix       = clampf(p.mix, 0.0f, 1.0f);
        applyDerived();
        if (! primed_) {
            smMix_.snapToTarget();
            smFb_.snapToTarget();
            smNoise_.snapToTarget();
            smDepth_.snapToTarget();
            smAge_.snapToTarget();
            delay_ = targetDelay_;
            retuneClockFilters(delay_, smAge_.current());
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
            const float noiseAmp = smNoise_.next();
            const float depth = smDepth_.next();
            const float ageF = smAge_.next();

            // The clock drags: at most kMaxSlew samples of delay per sample of
            // audio, so the read head never runs backwards and the move is
            // heard as a pitch dive.
            const float diff = targetDelay_ - delay_;
            const float want = diff * glideRate_;
            delay_ += std::clamp(want, -kMaxSlew, kMaxSlew);
            if (std::abs(targetDelay_ - delay_) < 1.0e-3f) delay_ = targetDelay_;

            // The clock-tracking pair follows the gliding delay, never the
            // knob: a redesign only when it has drifted kRetuneTolerance.
            if (std::abs(delay_ - designedDelay_) > kRetuneTolerance * designedDelay_
                || std::abs(ageF - designedAge_) > kAgeTolerance)
                retuneClockFilters(delay_, ageF);

            const float mod = lfo_.peek(Lfo::Sine);
            lfo_.advance();
            const float d = clampDelay(delay_ + depth * modSamples_ * mod);

            for (int c = 0; c < ch; ++c) {
                const auto uc = (std::size_t) c;
                const float x = channels[c][n];
                const float echo = recon_[uc].process(lines_[uc].readAt(d));
                // Clock noise rides on what is WRITTEN, the way a real BBD's
                // hiss does — it is recirculated and therefore ages with the
                // repeats instead of sitting flat on the output.
                const float hiss = noiseAmp * nextNoise();
                // The limiter sits AFTER the regen gain, not before it: at the
                // top of the knob `fb` is above 1, so limiting the raw echo
                // and THEN multiplying puts kLoopCeiling * fb into the line
                // and the repeat comes back over 0 dBFS (measured 1.057).
                const float shaped = softLimit(fb * dc_[uc].process(echo));
                lines_[uc].push(aa_[uc].process(x + hiss + shaped));
                channels[c][n] = dry * x + wet * echo;
            }
        }
    }

    int latencySamples() const { return 0; }

    /// The clock-tracking filter corner the current time implies, in Hz. For
    /// tests and docs: it is what makes a long BBD setting dark.
    float clockCornerHzForTest() const { return cornerHz_; }

private:
    void applyDerived() {
        smMix_.setTarget(p_.mix);
        // A real analog delay oscillates over roughly the top fifth of its
        // regen travel, and a raw 1.0 here does NOT: the two clock-tracking
        // low-passes, the DC blocker, the interpolator and the soft limiter's
        // own curvature each take a little off every lap, so the knob's top
        // was a slow decay (measured peak 0.27 against a 0.25 input). The
        // excess is therefore weighted to the very top of the travel — loop
        // gain crosses 1.0 at about feedback 0.82 — and the soft limiter is
        // what makes what is above it a bounded oscillation and not an
        // overflow.
        const float f = p_.feedback;
        const float f6 = f * f * f * f * f * f;
        smFb_.setTarget(f * (1.0f + kRegenExcess * f6));
        smDepth_.setTarget(p_.modDepth);
        smNoise_.setTarget(kMaxNoise * p_.age * p_.age);
        smAge_.setTarget(1.0f - kAgeLoss * p_.age);
        targetDelay_ = clampDelay((float) (p_.timeMs * 0.001 * sr_));
        modSamples_  = (float) (kMaxModMs * 0.001 * sr_);
        lfo_.setFrequency(p_.modRateHz);
        // ~50 ms of exponential approach, capped by kMaxSlew above. Rate-owned.
        glideRate_ = (float) (1.0 - std::exp(-1.0 / (kGlideSeconds * sr_)));

    }

    // THE BBD IDENTITY: the filter pair tracks the clock, so the corner is
    // inversely proportional to the delay TIME the read head is actually
    // running at. 480 / t gives ~4.8 kHz at 100 ms and ~800 Hz at 600 ms;
    // `age` drags it down further.
    void retuneClockFilters(float delaySamples, float ageFactor) {
        designedDelay_ = std::max(1.0f, delaySamples);
        designedAge_   = ageFactor;
        const float seconds = std::max(1.0e-4f, designedDelay_ / (float) sr_);
        cornerHz_ = clampf(kCornerHzMs / seconds * ageFactor, kMinCornerHz, kMaxCornerHz);
        for (auto& f : aa_)    f.setLowpass(sr_, (double) cornerHz_, 0.7071);
        for (auto& f : recon_) f.setLowpass(sr_, (double) cornerHz_, 0.7071);
    }

    float nextNoise() {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return ((float) (rng_ >> 8) * (1.0f / 8388608.0f)) - 1.0f;
    }

    float clampDelay(float s) const {
        const float maxd = (float) (maxDelaySamp_ - 4);
        if (! (s >= 1.0f)) s = 1.0f;
        if (s > maxd) s = maxd;
        return s;
    }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    // Bounded at kLoopCeiling, monotonic, ~unity slope at small signals: the
    // loop settles into self-oscillation instead of diverging at feedback 1.0.
    static float softLimit(float x) { return kLoopCeiling * std::tanh(x * (1.0f / kLoopCeiling)); }

    static constexpr double kMaxTimeMs        = 600.0;
    static constexpr double kMaxModMs         = 6.0;
    static constexpr double kGlideSeconds     = 0.05;
    static constexpr float  kLevelRampSeconds = 0.015f;
    // A big move takes |diff| / kMaxSlew samples: 300 ms of delay change is
    // ~1.2 s of dive at 48 kHz, which is what an analog time knob sounds like.
    static constexpr float  kMaxSlew    = 0.25f;
    static constexpr float  kCornerHzMs = 480.0f;
    static constexpr float  kAgeLoss    = 0.6f;
    static constexpr float  kMinCornerHz = 700.0f;
    static constexpr float  kMaxCornerHz = 12000.0f;
    static constexpr float  kMaxNoise   = 0.004f;
    // 0.6, not 0.9: the line holds input + the limited feedback, so the
    // ceiling plus the loudest input the rig hands a pedal has to stay under
    // 1.0 or feedback 1.0 at mix 1 would post a repeat above 0 dBFS.
    static constexpr float  kLoopCeiling = 0.6f;
    // Enough regeneration for the top of the knob to actually oscillate.
    static constexpr float  kRegenExcess = 0.7f;
    static constexpr float  kAgeRampSeconds  = 0.050f;
    static constexpr float  kRetuneTolerance = 0.01f;   // 1 % of the current delay
    static constexpr float  kAgeTolerance    = 0.005f;
    static constexpr uint32_t kSeed = 0x1BBD0A57u;

    std::vector<InterpolatedDelayLine> lines_;
    std::vector<Biquad>    aa_, recon_;
    std::vector<DcBlocker> dc_;
    Lfo lfo_;
    SmoothedValue smMix_, smFb_, smNoise_, smDepth_, smAge_;
    BbdDelayParams p_{};
    double sr_ = 48000.0;
    int    numCh_ = 2;
    int    maxBlock_ = 0;
    int    maxDelaySamp_ = 0;
    float  glideRate_ = 0.0f;
    float  targetDelay_ = 0.0f;
    float  delay_ = 0.0f;
    float  modSamples_ = 0.0f;
    float  cornerHz_ = 0.0f;
    float  designedDelay_ = 1.0f;
    float  designedAge_ = 1.0f;
    uint32_t rng_ = kSeed;
    bool   primed_ = false;
};

} // namespace dcdsp
