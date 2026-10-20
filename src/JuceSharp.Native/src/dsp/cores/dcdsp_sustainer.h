// dcdsp_sustainer.h — SUSTAINER / CLEAN COMP+BOOST
//
// A compressor tuned for sustain: high ratio, program-dependent release, and
// upward makeup that lifts quiet decaying tails so a picked note keeps ringing.
// Followed by a gentle tone tilt and an output trim. A safety soft-clip on the
// output guarantees a bounded signal at any setting/input level.
//
// Pure, RT-safe: allocation only in prepare(); process() never allocates,
// locks, or throws.
//
// Codes against the dcdsp shared-primitive signatures. In production this pulls
// in the real dcdsp_primitives.h; the standalone test defines DCDSP_TEST_STUBS
// to substitute a minimal local header of only the primitives used here.
#pragma once

#ifdef DCDSP_TEST_STUBS
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct SustainerParams {
    // Amount of upward compression / sustain drive. 0 = nearly transparent,
    // 1 = heavy squash with strong tail lift (high ratio + max makeup). [0..1]
    float sustain = 0.5f;
    // Compressor attack feel. 0 = fast clamp (~2 ms, snappy), 1 = slow (~60 ms,
    // lets the pick transient through). [0..1]
    float attack = 0.3f;
    // Tone tilt. 0.5 = flat; below tilts dark (highs cut / lows lifted), above
    // tilts bright (highs lifted / lows cut). [0..1]
    float tone = 0.5f;
    // Output level trim. 0.5 = unity (0 dB); 0 = -24 dB, 1 = +12 dB. [0..1]
    float level = 0.5f;
};

class SustainerCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        (void)maxBlockSize;
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        nch_ = numChannels > 0 ? numChannels : 1;

        detector_.prepare(sr_);
        detector_.setTimes(1.0f, 60.0f); // fast detector; gain smoothing owns feel

        lowShelf_.assign(static_cast<size_t>(nch_), Biquad{});
        highShelf_.assign(static_cast<size_t>(nch_), Biquad{});
        clipState_.assign(static_cast<size_t>(nch_), 0.0f);

        makeupSmooth_.reset(sr_, kMakeupRampSeconds);
        outSmooth_.reset(sr_, kMakeupRampSeconds);
        setParams(params_);
        makeupSmooth_.snapToTarget(); // start AT the configured lift
        outSmooth_.snapToTarget();
        reset();
    }

    void reset() {
        detector_.reset();
        compGainDb_ = 0.0f;
        for (auto& b : lowShelf_)  b.reset();
        for (auto& b : highShelf_) b.reset();
        std::fill(clipState_.begin(), clipState_.end(), 0.0f);
    }

    void setParams(const SustainerParams& p) {
        params_ = p;
        const float s = clamp01(p.sustain);
        const float a = clamp01(p.attack);
        const float tone = clamp01(p.tone);
        const float lvl = clamp01(p.level);

        // Compression curve.
        ratio_     = 2.0f + s * 18.0f;          // 2:1 .. 20:1
        makeupDb_  = s * 14.0f;                  // 0 .. +14 dB static lift
        makeupSmooth_.setTarget(makeupDb_);      // ramped, never stepped
        upMaxDb_   = s * 18.0f;                  // 0 .. +18 dB tail lift ceiling

        // Attack/release timing.
        attackMs_  = 2.0f + a * 58.0f;           // 2 .. 60 ms
        // Release computed per-sample (program dependent) between these. Kept
        // moderate so gain recovers quickly enough to actually lift a decaying
        // tail (the whole point of a sustainer) rather than lag behind it.
        fastRelMs_ = 40.0f;
        slowRelMs_ = 150.0f;
        attackCoef_ = coefFromMs(attackMs_);

        // Tone tilt: symmetric low/high shelves around 1 kHz split.
        const float tiltDb = (tone - 0.5f) * 2.0f * 9.0f; // -9 .. +9 dB
        for (auto& b : lowShelf_)  b.setLowShelf(sr_, 250.0,  0.707, -tiltDb);
        for (auto& b : highShelf_) b.setHighShelf(sr_, 2500.0, 0.707, +tiltDb);

        // Output trim: 0->-24 dB, 0.5->0 dB, 1->+12 dB.
        const float outDb = (lvl <= 0.5f)
            ? (lvl / 0.5f) * 24.0f - 24.0f
            : (lvl - 0.5f) / 0.5f * 12.0f;
        outGain_ = dbToLin(outDb);
        outSmooth_.setTarget(outGain_);          // the level knob ramps too
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        const int nc = std::min(numChannels, nch_);

        for (int i = 0; i < numFrames; ++i) {
            // --- linked detection: peak across channels ---
            float peak = 0.0f;
            for (int c = 0; c < nc; ++c) {
                const float v = std::fabs(channels[c][i]);
                if (v > peak) peak = v;
            }
            const float env = detector_.process(peak);
            const float envDb = 20.0f * std::log10(env + 1e-6f);

            // --- static gain target (dB) ---
            // Downward compression above threshold.
            const float over = envDb - kThresholdDb;
            float grDb = 0.0f;                          // >= 0, amount reduced
            if (over > 0.0f) grDb = over * (1.0f - 1.0f / ratio_);

            // Upward compression: lift quiet tails toward the threshold, but
            // never boost silence/noise (gated by the noise floor).
            float upDb = 0.0f;
            if (envDb > kNoiseFloorDb) {
                const float under = kThresholdDb - envDb;
                if (under > 0.0f) upDb = std::min(under * kUpSlope, upMaxDb_);
            }

            // Dynamic compression gain (downward reduction + upward tail lift),
            // smoothed with attack/release. Makeup is applied separately below
            // as a static lift so it raises quiet tails immediately instead of
            // lagging behind the compressor's release.
            float compTargetDb = upDb - grDb;
            compTargetDb = clampf(compTargetDb, kMinGainDb, kMaxUpDb);

            // --- program-dependent smoothing of the compression gain ---
            float coef;
            if (compTargetDb < compGainDb_) {
                coef = attackCoef_;                     // clamping down: attack
            } else {
                // Deeper current reduction => slower release for smooth sustain.
                const float depth = clampf(grDb / 12.0f, 0.0f, 1.0f);
                const float relMs = fastRelMs_ + (slowRelMs_ - fastRelMs_) * depth;
                coef = coefFromMs(relMs);
            }
            compGainDb_ = coef * compGainDb_ + (1.0f - coef) * compTargetDb;
            if (!std::isfinite(compGainDb_)) compGainDb_ = 0.0f;

            const float totalDb = clampf(compGainDb_ + makeupSmooth_.next(),
                                         kMinGainDb, kMaxGainDb);
            const float gLin = dbToLin(totalDb);
            const float outG = outSmooth_.next(); // once per sample, before the channel loop

            // --- apply gain + tone + output trim + safety clip per channel ---
            for (int c = 0; c < nc; ++c) {
                float x = channels[c][i] * gLin;
                x = lowShelf_[static_cast<size_t>(c)].process(x);
                x = highShelf_[static_cast<size_t>(c)].process(x);
                x *= outG;
                // Bounded soft-clip guarantees |out| < kCeiling regardless of
                // input level or parameter extremes.
                x = kCeiling * adaaTanh(x * (1.0f / kCeiling),
                                        clipState_[static_cast<size_t>(c)]);
                if (!std::isfinite(x)) x = 0.0f;
                channels[c][i] = x;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    static constexpr float kThresholdDb  = -34.0f;
    static constexpr float kNoiseFloorDb = -70.0f;
    static constexpr float kUpSlope      = 0.6f;
    static constexpr float kMinGainDb    = -24.0f;
    static constexpr float kMaxUpDb      =  18.0f;   // cap on dynamic comp gain
    static constexpr float kMaxGainDb    =  34.0f;   // makeup(14)+tail(18)+margin
    static constexpr float kCeiling      =  3.0f;    // output soft-clip ceiling

    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static float dbToLin(float db) { return (float) dbToGain((double) db); }   // exp2 form
    float coefFromMs(float ms) const {
        const double t = static_cast<double>(ms) * 0.001 * sr_;
        if (t <= 0.0) return 0.0f;
        return static_cast<float>(std::exp(-1.0 / t));
    }

    double sr_ = 48000.0;
    int nch_ = 1;
    SustainerParams params_{};

    EnvelopeFollower detector_{};
    std::vector<Biquad> lowShelf_{};
    std::vector<Biquad> highShelf_{};
    std::vector<float>  clipState_{};

    float compGainDb_ = 0.0f;

    // derived
    float ratio_ = 2.0f;
    float makeupDb_ = 0.0f;
    static constexpr float kMakeupRampSeconds = 0.010f;
    SmoothedValue makeupSmooth_;
    SmoothedValue outSmooth_;
    float upMaxDb_ = 0.0f;
    float attackMs_ = 10.0f;
    float fastRelMs_ = 120.0f;
    float slowRelMs_ = 700.0f;
    float attackCoef_ = 0.0f;
    float outGain_ = 1.0f;
};

} // namespace dcdsp
