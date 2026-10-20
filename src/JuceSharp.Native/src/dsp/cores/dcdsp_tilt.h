// dcdsp_tilt.h — TILT EQ ("tilt").
//
// Models the one-knob "tilt" tone control of the Quad 34 / Baxandall lineage and
// of every modern tilt plugin: a single knob rocks the whole spectrum about a
// pivot frequency. It is the fastest way to take the ice off a bright rig or the
// mud out of a dark one without choosing a band.
//
// Topology: ONE complementary shelving PAIR about `pivotHz` — an RBJ low shelf at
// -tilt/2 dB and an RBJ high shelf at +tilt/2 dB, both Q 0.707. A shelf is at
// exactly half its dB gain AT its corner, so the two cancel at the pivot by
// construction and the total swing between the far ends is `tilt` dB.
//
// Easy to get wrong: the shelves must be SHELVES and not high/low passes. An
// unbounded filter looks identical over the musical range and then keeps going —
// cascade two rigs' worth and the low E is 20 dB down with nothing on screen
// saying so. The second trap is that both `tilt` and `pivotHz` are coefficient
// sets, not gains: they are smoothed (pivot in LOG2 Hz) and redesigned only when
// the smoothed value moved, while `level` is a plain per-sample ramped multiply.
#pragma once

#include "dcdsp_primitives.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace dcdsp {

struct TiltParams {
    // -12 .. +12 dB of TOTAL tilt. Positive lifts the highs by tilt/2 and cuts
    // the lows by tilt/2 about the pivot; negative does the reverse.
    float tilt = 0.0f;
    float pivotHz = 1000.0f;   // 200 .. 5000
    float level = 0.0f;        // -12 .. +12 dB of output trim
};

class TiltCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 1;
        const int nch = (numChannels > 0) ? numChannels : 1;
        channels_.assign(static_cast<std::size_t>(nch), ChannelState{});   // sole allocation

        // Rate-owned: both shelves' coefficients and the three ramps' lengths.
        tiltSmooth_.reset(sr_, kRampSeconds);
        tiltSmooth_.setTarget(params_.tilt);
        tiltSmooth_.snapToTarget();
        pivotSmooth_.reset(sr_, kSweepRampSeconds);
        pivotSmooth_.setTarget(std::log2(params_.pivotHz));
        pivotSmooth_.snapToTarget();
        levelSmooth_.reset(sr_, kRampSeconds);
        levelSmooth_.setTarget(dbToGain(params_.level));
        levelSmooth_.snapToTarget();

        lastTilt_ = std::numeric_limits<float>::quiet_NaN();
        lastPivot_ = std::numeric_limits<float>::quiet_NaN();
        coeffCountdown_ = 0;
        design(params_.tilt, std::log2(params_.pivotHz));
        reset();
    }

    void reset() {
        for (auto& c : channels_) { c.low.reset(); c.high.reset(); }
    }

    void setParams(const TiltParams& p) {
        // A NaN lands on the NEUTRAL value, never on `lo`. For a param whose
        // neutral is the MIDDLE of its range, `lo` is the most extreme setting
        // there is: a rejected NaN `tilt` used to become a full -12 dB tilt.
        params_.tilt    = clampf(p.tilt, -12.0f, 12.0f, 0.0f);
        params_.pivotHz = clampf(p.pivotHz, 200.0f, 5000.0f, 1000.0f);
        params_.level   = clampf(p.level, -12.0f, 12.0f, 0.0f);

        tiltSmooth_.setTarget(params_.tilt);
        pivotSmooth_.setTarget(std::log2(params_.pivotHz));
        levelSmooth_.setTarget(dbToGain(params_.level));
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;      // skipped WHOLE
        const int nch = std::min(numChannels, static_cast<int>(channels_.size()));
        if (nch <= 0) return;

        for (int n = 0; n < numFrames; ++n) {
            const float tilt  = tiltSmooth_.next();
            const float pivot = pivotSmooth_.next();
            const float level = levelSmooth_.next();

            if (--coeffCountdown_ <= 0) {
                coeffCountdown_ = kCoeffInterval;
                if (!(std::fabs(tilt - lastTilt_) <= 1.0e-5f)
                 || !(std::fabs(pivot - lastPivot_) <= 1.0e-5f))
                    design(tilt, pivot);
            }

            for (int c = 0; c < nch; ++c) {
                ChannelState& cs = channels_[static_cast<std::size_t>(c)];
                float x = channels[c][n];
                if (!std::isfinite(x)) x = 0.0f;
                float y = cs.high.process(cs.low.process(x)) * level;
                if (!std::isfinite(y)) { y = 0.0f; cs.low.reset(); cs.high.reset(); }
                channels[c][n] = clampf(y, -24.0f, 24.0f);
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    struct ChannelState { Biquad low, high; };

    void design(float tilt, float logPivot) {
        lastTilt_ = tilt;
        lastPivot_ = logPivot;
        const double fc = clampCutoff(sr_, std::pow(2.0, static_cast<double>(logPivot)));
        const double half = static_cast<double>(tilt) * 0.5;
        for (auto& cs : channels_) {
            cs.low.setLowShelf(sr_, fc, kShelfQ, -half);
            cs.high.setHighShelf(sr_, fc, kShelfQ, half);
        }
    }

    static float dbToGain(float db) {
        if (!std::isfinite(db)) db = 0.0f;
        return std::pow(10.0f, db * (1.0f / 20.0f));
    }
    static float clampf(float v, float lo, float hi) { return clampf(v, lo, hi, lo); }
    static float clampf(float v, float lo, float hi, float fallback) {
        if (v != v) return fallback;           // NaN
        if (v < lo) return lo;
        return (v > hi) ? hi : v;
    }

    static constexpr float  kRampSeconds = 0.010f;
    static constexpr float  kSweepRampSeconds = 0.030f;
    static constexpr double kShelfQ = 0.70710678;
    static constexpr int    kCoeffInterval = 16;

    TiltParams params_{};
    double sr_ = 48000.0;
    int    maxBlock_ = 1;
    std::vector<ChannelState> channels_;
    SmoothedValue tiltSmooth_;
    SmoothedValue pivotSmooth_;    // log2 Hz
    SmoothedValue levelSmooth_;    // LINEAR gain
    float lastTilt_ = 0.0f;
    float lastPivot_ = 0.0f;
    int   coeffCountdown_ = 0;
};

}  // namespace dcdsp
