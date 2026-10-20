// dcdsp_tapedelay.h — pure, RT-safe tape delay core.
//
// Modulated interpolated delay line with a real tape signal path (0.16):
//
//   record:   write = DcBlock( adaaTanh( drive * (in + fb * playback) ) / drive' )
//   playback: delayed -> head-bump shelf -> tone lowpass -> wet + feedback
//
//  - `drive` scales the RECORD-path saturation (normalised so low levels stay
//    unity gain); hitting the tape harder compresses and colours every
//    generation, exactly like real tape. Because the write is tanh-bounded,
//    the loop is unconditionally stable at any setting.
//  - Head bump: +2.5 dB low shelf at ~100 Hz in the playback path, normalised
//    to unity at LF (i.e. everything above the shoulder loses 2.5 dB per lap
//    instead of the lows gaining). That keeps worst-case loop gain <= feedback
//    (< 1) while the repeats grow relatively bassier per generation.
//  - Wow/flutter is multi-component: two wow sines (0.55 / 2.3 Hz) + slow
//    filtered-noise drift, and a 6.1 Hz flutter sine + faster filtered-noise
//    jitter. Component weights sum to 1 per group, so the total excursion
//    stays inside the prepared modulation headroom.
//
// Namespace dcdsp. C++17. Deps: only the shared primitives header + <cmath>,
// <vector>, <algorithm>, <cstddef>, <cstdint>.
#pragma once

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Production builds pull in the real shared primitives. The standalone test
// defines DCDSP_TAPEDELAY_TEST_STUBS to pull in a minimal local stub instead.
#ifdef DCDSP_TAPEDELAY_TEST_STUBS
  #include "test_stubs.h"
#else
  #include "dcdsp_primitives.h"
#endif

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe.
struct TapeDelayParams {
    float timeMs      = 350.0f;  // 20 .. 2000  — nominal delay time in milliseconds
    float feedback    = 0.35f;   // 0 .. 0.95   — recirculation gain (hard-capped < 1)
    float mix         = 0.35f;   // 0 .. 1      — dry(0) .. wet(1) blend
    float wowFlutter  = 0.25f;   // 0 .. 1      — depth of pitch wobble (wow + flutter)
    float toneHz      = 4000.0f; // 800 .. 8000 — feedback/wet lowpass cutoff (tape hiss roll-off)
    float drive       = 0.25f;   // 0 .. 1      — record-path tape saturation amount
};

class TapeDelayCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        (void)maxBlockSize;
        nch_ = (numChannels > 0) ? numChannels : 1;

        // Max modulation excursion (in samples) at wowFlutter = 1.
        wowMaxDepthSamples_    = static_cast<float>(kWowMaxMs    * sr_ / 1000.0);
        flutterMaxDepthSamples_= static_cast<float>(kFlutterMaxMs* sr_ / 1000.0);

        // Longest possible read tap: max time + full modulation + guard.
        const double maxTimeSamples = (kMaxTimeMs * sr_ / 1000.0);
        maxDelaySamples_ = static_cast<int>(std::ceil(maxTimeSamples
                                + wowMaxDepthSamples_ + flutterMaxDepthSamples_))
                                + 8;

        lines_.assign(static_cast<size_t>(nch_), InterpolatedDelayLine{});
        tone_.assign(static_cast<size_t>(nch_), OnePole{});
        bump_.assign(static_cast<size_t>(nch_), Biquad{});
        dc_.assign(static_cast<size_t>(nch_), DcBlocker{});
        satState_.assign(static_cast<size_t>(nch_), 0.0f);

        for (auto& d : dc_) d.prepareLegacy48k(sr_);
        for (int c = 0; c < nch_; ++c) {
            lines_[static_cast<size_t>(c)].prepare(maxDelaySamples_);
        }

        wow1_.prepare(sr_);
        wow1_.setShape(Lfo::Sine);
        wow1_.setFrequency(kWow1Hz);
        wow2_.prepare(sr_);
        wow2_.setShape(Lfo::Sine);
        wow2_.setFrequency(kWow2Hz);
        wow2_.setPhase(0.37f);                       // decorrelate from wow1
        flutter_.prepare(sr_);
        flutter_.setShape(Lfo::Sine);
        flutter_.setFrequency(kFlutterHz);
        driftLp_.setLowpass(sr_, kDriftHz);          // slow tape-path drift
        jitterLp_.setLowpass(sr_, kJitterHz);        // faster flutter jitter

        timeSmooth_.reset(sr_, 0.05f); // 50 ms glide on delay time (no zipper/clicks)
        // `drive` displaces the ADAA state's operating point in one sample as
        // well as stepping the level; the two wow/flutter DEPTHS are added to
        // the read tap timeSmooth_ above already glides for the same reason.
        smSat_.reset(sr_, 0.015f);
        smWow_.reset(sr_, 0.020f);
        smFlutter_.reset(sr_, 0.020f);

        applyToneAll();
        applyBumpAll();
        firstParams_ = true;
        setParams(params_);
        smSat_.snapToTarget();
        smWow_.snapToTarget();
        smFlutter_.snapToTarget();
        reset();
    }

    void reset() {
        for (auto& l : lines_)  l.reset();
        for (auto& t : tone_)   t.reset();
        for (auto& b : bump_)   b.reset();
        for (auto& d : dc_)     d.reset();
        std::fill(satState_.begin(), satState_.end(), 0.0f);
        wow1_.reset();
        wow2_.reset();
        wow2_.setPhase(0.37f);
        flutter_.reset();
        driftLp_.reset();
        jitterLp_.reset();
        noiseState_ = 0x9e3779b9u;
        drift_ = 0.0f;
        jitter_ = 0.0f;
        // Drive the time smoother to its steady-state value so the very first
        // block already plays at the requested delay (no glide up from zero).
        primeSmoother();
        // `mix` RAMPS too (kMixRampSeconds). The time smoother was already here;
        // the mix was still one constant per block, so a mix drag stepped a full
        // dry/wet crossfade at every block edge.
        mixSmooth_.reset(sr_, kMixRampSeconds);
        mixSmooth_.setTarget(mix_);
        mixSmooth_.snapToTarget();   // a freshly prepared delay starts AT its mix
    }

    void setParams(const TapeDelayParams& p) {
        params_ = p;

        const float timeMs = clampf(p.timeMs, kMinTimeMs, kMaxTimeMs);
        targetDelaySamples_ = static_cast<float>(timeMs * sr_ / 1000.0);
        timeSmooth_.setTarget(targetDelaySamples_);

        fbGain_ = clampf(p.feedback, 0.0f, kMaxFeedback);
        mix_    = clampf(p.mix, 0.0f, 1.0f);
        mixSmooth_.setTarget(mix_);

        const float wf = clampf(p.wowFlutter, 0.0f, 1.0f);
        wowDepth_     = wf * wowMaxDepthSamples_;
        flutterDepth_ = wf * flutterMaxDepthSamples_;
        // A depth write moves the read tap instantly by delta * |lfo| -- 144
        // samples (3 ms) at 48 kHz for wowFlutter. `timeMs` got a 50 ms glide
        // for exactly this reason; the depth ADDED to it did not.
        smWow_.setTarget(wowDepth_);
        smFlutter_.setTarget(flutterDepth_);

        // Record saturation pre-gain: 1 (unity, gentle) .. 6 (slammed tape).
        satGain_    = 1.0f + 5.0f * clampf(p.drive, 0.0f, 1.0f);
        satInvGain_ = 1.0f / satGain_;
        smSat_.setTarget(satGain_);

        const float newTone = clampf(p.toneHz, kMinToneHz, kMaxToneHz);
        if (firstParams_ || newTone != toneHz_) {
            toneHz_ = newTone;
            applyToneAll();
        }

        if (firstParams_) {
            primeSmoother();
        }
        firstParams_ = false;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (numFrames <= 0 || numChannels <= 0) return;
        const int nc = (numChannels < nch_) ? numChannels : nch_;

        const float fbGain = fbGain_;
        // satG/satIG and the two depths are walked PER FRAME below.

        const float minTap = 1.0f;
        const float maxTap = static_cast<float>(maxDelaySamples_) - 2.0f;

        for (int n = 0; n < numFrames; ++n) {
            // Advance modulation once per frame (shared across channels).
            const float base = timeSmooth_.next();
            const float mix     = mixSmooth_.next();
            const float dryGain = 1.0f - mix;
            const float satG    = smSat_.next();
            const float satIG   = 1.0f / satG;

            // Filtered-noise components: white -> one-pole LP, clamped to
            // [-1, 1] so the weighted sum below can never leave the headroom.
            const float white = nextNoise();
            drift_  = clampf(driftLp_.process(white) * kDriftBoost, -1.0f, 1.0f);
            jitter_ = clampf(jitterLp_.process(white) * kJitterBoost, -1.0f, 1.0f);

            // Weights per group sum to 1 -> |wow| <= wowDepth_, |flu| <= flutterDepth_.
            const float wow = 0.62f * wow1_.process()
                            + 0.28f * wow2_.process()
                            + 0.10f * drift_;
            const float flu = 0.80f * flutter_.process()
                            + 0.20f * jitter_;
            float tap = base + smWow_.next() * wow + smFlutter_.next() * flu;
            tap = clampf(tap, minTap, maxTap);

            for (int c = 0; c < nc; ++c) {
                float* buf = channels[c];
                const size_t sc = static_cast<size_t>(c);

                const float in = buf[n];

                // ---- playback path: tape -> head bump -> tone ----
                const float delayed = lines_[sc].readAt(tap);
                const float bumped  = bump_[sc].process(delayed) * kBumpNorm;
                const float playback = tone_[sc].process(bumped);

                // ---- record path: input + feedback -> tape saturation ----
                const float rec = in + fbGain * playback;
                const float sat = adaaTanh(rec * satG, satState_[sc]) * satIG;

                // Recirculate: DC-block to stop tape-loop DC build-up.
                float toWrite = dc_[sc].process(sat);
                // Belt-and-suspenders clamp so a NaN/huge transient can never
                // poison the delay memory.
                toWrite = clampf(toWrite, -8.0f, 8.0f);
                lines_[sc].push(toWrite);

                buf[n] = in * dryGain + playback * mix;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    void applyToneAll() {
        for (auto& t : tone_) t.setLowpass(sr_, toneHz_);
    }
    void applyBumpAll() {
        // +kBumpDb low shelf at kBumpHz; kBumpNorm renormalises the shelf to
        // unity at LF so in-loop gain never exceeds the feedback setting.
        for (auto& b : bump_) b.setLowShelf(sr_, kBumpHz, 0.707, kBumpDb);
    }

    // xorshift32 white noise in [-1, 1] — allocation-free, period 2^32-1.
    float nextNoise() {
        uint32_t x = noiseState_;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        noiseState_ = x;
        return static_cast<float>(static_cast<int32_t>(x)) * (1.0f / 2147483648.0f);
    }

    // Not RT-safe (loops for the smoother's ramp length) — only ever called from
    // prepare()/reset(), which are allowed to be heavy. Converges SmoothedValue's
    // internal "current" onto the target so the first processed block is at time.
    void primeSmoother() {
        timeSmooth_.reset(sr_, 0.05f);
        timeSmooth_.setTarget(targetDelaySamples_);
        const int steps = static_cast<int>(0.05 * sr_) + 16;
        for (int i = 0; i < steps; ++i) (void)timeSmooth_.next();
    }

    // Tunables.
    static constexpr float kMinTimeMs   = 20.0f;
    static constexpr float kMaxTimeMs   = 2000.0f;
    static constexpr float kMaxFeedback = 0.95f;   // hard cap: loop gain < 1 always
    static constexpr float kMinToneHz   = 800.0f;
    static constexpr float kMaxToneHz   = 8000.0f;
    static constexpr float kWow1Hz      = 0.55f;   // primary slow wow
    static constexpr float kWow2Hz      = 2.3f;    // secondary wow (capstan)
    static constexpr float kFlutterHz   = 6.1f;    // fast flutter
    static constexpr double kDriftHz    = 0.8;     // drift-noise LP corner
    static constexpr double kJitterHz   = 4.0;     // flutter-jitter LP corner
    // One-pole LP shrinks white-noise RMS drastically; boost back toward a
    // usable +/-1-ish range before the clamp.
    static constexpr float kDriftBoost  = 60.0f;
    static constexpr float kJitterBoost = 25.0f;
    static constexpr double kWowMaxMs    = 3.0;    // max wow excursion  @ depth 1
    static constexpr double kFlutterMaxMs= 0.45;   // max flutter excursion @ depth 1
    static constexpr double kBumpHz     = 100.0;   // head-bump shoulder
    static constexpr double kBumpDb     = 2.5;     // relative low boost per lap
    static constexpr float  kBumpNorm   = 0.74989420933f; // 10^(-2.5/20)

    double sr_ = 48000.0;
    int nch_ = 0;
    int maxDelaySamples_ = 1;

    TapeDelayParams params_{};
    bool firstParams_ = true;

    float targetDelaySamples_ = 0.0f;
    float fbGain_ = 0.0f;
    float mix_ = 0.0f;
    float toneHz_ = 4000.0f;
    float satGain_ = 1.0f;
    float satInvGain_ = 1.0f;

    float wowMaxDepthSamples_ = 0.0f;
    float flutterMaxDepthSamples_ = 0.0f;
    float wowDepth_ = 0.0f;
    float flutterDepth_ = 0.0f;

    std::vector<InterpolatedDelayLine> lines_;
    std::vector<OnePole> tone_;
    std::vector<Biquad> bump_;
    std::vector<DcBlocker> dc_;
    std::vector<float> satState_;

    Lfo wow1_;
    Lfo wow2_;
    Lfo flutter_;
    OnePole driftLp_;
    OnePole jitterLp_;
    uint32_t noiseState_ = 0x9e3779b9u;
    float drift_ = 0.0f;
    float jitter_ = 0.0f;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue timeSmooth_;
    SmoothedValue smSat_, smWow_, smFlutter_;   // the three TARGETS stay above
    SmoothedValue mixSmooth_;
};

} // namespace dcdsp
