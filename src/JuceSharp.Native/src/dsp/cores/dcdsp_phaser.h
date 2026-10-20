// dcdsp_phaser.h — pure header-only phaser core for the DotCharter in-house DSP rig.
//
// Cascade of first-order (single-state, Gray-Markel form) allpass sections whose
// break frequency is swept by a per-channel LFO, with clamped feedback wrapped
// around the cascade and a wet/dry mix. RT-safe: allocation happens only in
// prepare(); process() never allocates, locks, or throws.
//
// Deps: only <cmath> <vector> <array> <algorithm> <cstddef> <cstdint> + the
// shared primitives header (SmoothedValue, DcBlocker).
#pragma once

#ifdef DCDSP_USE_TEST_STUBS
#include "test_stubs.h"      // standalone-compile shim (SmoothedValue, DcBlocker only)
#else
#include "dcdsp_primitives.h" // production: real shared primitives
#endif

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dcdsp {

// Parameter ranges are documented per field. setParams() clamps everything, so
// out-of-range values are safe (they are folded back into range).
struct PhaserParams {
    float rateHz   = 0.5f;  // LFO sweep rate, 0.05 .. 8 Hz
    float depth    = 0.7f;  // sweep depth, 0 .. 1 (0 = static, 1 = full range)
    float feedback = 0.3f;  // resonance around cascade, -0.95 .. 0.95
    int   stages   = 6;     // number of allpass sections, 2 .. 12, forced even
    float mix      = 0.5f;  // 0 = fully dry, 1 = fully wet
};

class PhaserCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        (void) maxBlockSize;
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        int nch = (numChannels > 0) ? numChannels : 1;
        chans_.assign(static_cast<size_t>(nch), ChannelState{});

        // Per-channel LFO phase spread: quarter-cycle per channel (stereo => 90°).
        for (size_t c = 0; c < chans_.size(); ++c) {
            chans_[c].lfoPhase = 0.25 * static_cast<double>(c);
            if (chans_[c].lfoPhase >= 1.0) chans_[c].lfoPhase -= std::floor(chans_[c].lfoPhase);
            // Rate-derive the corner, do not inherit DcBlocker's rate-free
            // default (mR = 0.995 -> 38 Hz at 48 kHz, 153 Hz at 192 kHz).
            chans_[c].dcBlock.prepare(sr_);
            chans_[c].dcBlock.reset();
        }

        const float ramp = 0.02f; // 20 ms zipper-free smoothing
        depthSm_.reset(sr_, ramp);   depthSm_.setTarget(params_.depth);
        fbSm_.reset(sr_, ramp);      fbSm_.setTarget(params_.feedback);
        mixSm_.reset(sr_, ramp);     mixSm_.setTarget(params_.mix);
        // Start AT the configured values: reset() preserves mCurrent, which is 0
        // on a first prepare, so without this the first 20 ms is fully dry.
        depthSm_.snapToTarget();
        fbSm_.snapToTarget();
        mixSm_.snapToTarget();
        // The cascade-length crossfade. A freshly prepared phaser starts AT its
        // stage count, never fading into it.
        stageFade_.reset(sr_, kStageFadeSeconds);
        stageFade_.setCurrentAndTarget(1.0f);
        nStagesPrev_ = nStages_;
        clearAllpass();
    }

    void reset() {
        for (auto& ch : chans_) {
            ch.ap.fill(0.0f);
            ch.fbState = 0.0f;
            ch.dcBlock.reset();
        }
        clearAllpass();
    }

    void setParams(const PhaserParams& p) {
        params_ = p;
        // Clamp to documented ranges.
        params_.rateHz   = clampf(params_.rateHz, 0.05f, 8.0f);
        params_.depth    = clampf(params_.depth, 0.0f, 1.0f);
        params_.feedback = clampf(params_.feedback, -0.95f, 0.95f);
        params_.mix      = clampf(params_.mix, 0.0f, 1.0f);
        // Stages: even, in [2, 12].
        int s = params_.stages;
        if (s < 2)  s = 2;
        if (s > kMaxStages) s = kMaxStages;
        s &= ~1;            // force even (round down to even)
        if (s < 2) s = 2;
        params_.stages = s;
        setStages(s);

        depthSm_.setTarget(params_.depth);
        fbSm_.setTarget(params_.feedback);
        mixSm_.setTarget(params_.mix);
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (numFrames <= 0 || numChannels <= 0) return;
        const int nch = std::min<int>(numChannels, static_cast<int>(chans_.size()));

        const double phaseInc = static_cast<double>(params_.rateHz) / sr_;
        const double piOverSr = kPi / sr_;

        for (int n = 0; n < numFrames; ++n) {
            // Shared across channels this sample: the stage crossfade walks on
            // the same clock, so every channel gets the same blend.
            const float sFade = stageFade_.next();
            const int   nRun  = nStages_ > nStagesPrev_ ? nStages_ : nStagesPrev_;
            // Smoothed control values are shared across channels this sample.
            const float depth = depthSm_.next();
            const float fb    = clampf(fbSm_.next(), -0.95f, 0.95f);
            const float mix   = mixSm_.next();

            for (int c = 0; c < nch; ++c) {
                ChannelState& ch = chans_[static_cast<size_t>(c)];
                float* buf = channels[c];
                const float dry = buf[n];

                // --- LFO (own accumulator so we can phase-offset per channel) ---
                float lfo = fastSinTurns(static_cast<float>(ch.lfoPhase));     // [-1,1]
                ch.lfoPhase += phaseInc;
                if (ch.lfoPhase >= 1.0) ch.lfoPhase -= std::floor(ch.lfoPhase);

                // Map to sweep frequency (log range), scaled by depth.
                const float uni = 0.5f + 0.5f * lfo;         // [0,1]
                const float mod = depth * uni;               // [0,depth]
                // a^b == exp2(b * log2(a)); kLog2FSpan is that log2, computed
                // once at namespace scope. Same value, a quarter of the cost.
                float fc = kFMinHz * std::exp2(mod * kLog2FSpan);
                fc = clampf(fc, 20.0f, static_cast<float>(sr_ * 0.45));

                // First-order allpass coefficient a in (-1,1).
                const float t = static_cast<float>(fastTan(piOverSr * fc));
                float a = (t - 1.0f) / (t + 1.0f);
                a = clampf(a, -0.999f, 0.999f);

                // --- feedback into cascade (guard against runaway) ---
                float fbIn = ch.fbState;
                if (!std::isfinite(fbIn)) fbIn = 0.0f;
                float s = dry + fb * fbIn;
                if (!std::isfinite(s)) s = 0.0f;

                // --- allpass cascade (Gray-Markel single-state sections) ---
                // One pass to max(prev, cur), tapped at BOTH counts.
                float sPrev = s, sCur = s;
                for (int k = 0; k < nRun; ++k) {
                    const float st = ch.ap[static_cast<size_t>(k)];
                    const float v  = s - a * st;   // |a|<1 => stable
                    const float y  = a * v + st;
                    ch.ap[static_cast<size_t>(k)] = v;
                    s = y;
                    if (k + 1 == nStagesPrev_) sPrev = s;
                    if (k + 1 == nStages_)     sCur  = s;
                }
                s = (sFade >= 1.0f) ? sCur : (1.0f - sFade) * sPrev + sFade * sCur;
                if (!std::isfinite(s)) s = 0.0f;

                // Store feedback signal, hard-clamped so extreme settings stay bounded.
                ch.fbState = clampf(s, -4.0f, 4.0f);

                // DC-block the wet path (feedback can bias it) then mix.
                float wet = ch.dcBlock.process(s);
                if (!std::isfinite(wet)) wet = 0.0f;

                float out = dry * (1.0f - mix) + wet * mix;
                if (!std::isfinite(out)) out = 0.0f;
                buf[n] = out;
            }

            // The fade has landed: retire the sections above the live count, so
            // a later GROW cannot re-inject audio from before this change.
            if (nStagesPrev_ != nStages_ && !stageFade_.isRamping()) {
                for (int c = 0; c < nch; ++c) {
                    ChannelState& ch = chans_[static_cast<size_t>(c)];
                    for (int k = nStages_; k < kMaxStages; ++k)
                        ch.ap[static_cast<size_t>(k)] = 0.0f;
                }
                nStagesPrev_ = nStages_;
            }
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the smoother's CURRENT value, so a gate case can assert a
    // freshly prepared core starts AT its configured value rather than
    // ramping up to it from 0 (SmoothedValue::reset() preserves mCurrent).
    float mixCurrentForTest()   const { return mixSm_.current(); }
    float depthCurrentForTest() const { return depthSm_.current(); }

private:
    /// Switch cascade length over kStageFadeSeconds. Re-entrant the same way
    /// the shape switches in ringmod/vibrato/tremolo are.
    void setStages(int s) {
        if (s == nStages_) return;
        if (s == nStagesPrev_ && stageFade_.isRamping()) {
            const float t = stageFade_.current();
            nStagesPrev_ = nStages_;
            nStages_     = s;
            stageFade_.setCurrentAndTarget(1.0f - t);
        } else {
            nStagesPrev_ = nStages_;
            nStages_     = s;
            stageFade_.setCurrentAndTarget(0.0f);
        }
        stageFade_.setTarget(1.0f);
    }

    static constexpr int kMaxStages = 12;
    static constexpr float kStageFadeSeconds = 0.010f;
    static constexpr float kFMinHz = 200.0f;   // sweep low bound
    static constexpr float kFMaxHz = 2000.0f;  // sweep high bound
    // log2(kFMaxHz / kFMinHz) — the sweep is exp2(mod * this), not a pow.
    static constexpr float kLog2FSpan = 3.32192809488736f;   // log2(10)

    struct ChannelState {
        std::array<float, kMaxStages> ap{};
        float fbState = 0.0f;
        double lfoPhase = 0.0;
        DcBlocker dcBlock;
    };

    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    void clearAllpass() {
        for (auto& ch : chans_) {
            ch.ap.fill(0.0f);
            ch.fbState = 0.0f;
        }
    }

    double sr_ = 48000.0;
    int nStages_ = 6;
    int nStagesPrev_ = 6;
    SmoothedValue stageFade_;
    PhaserParams params_{};
    std::vector<ChannelState> chans_;
    SmoothedValue depthSm_;
    SmoothedValue fbSm_;
    SmoothedValue mixSm_;
};

} // namespace dcdsp
