// dcdsp_gate.h — NoiseGateCore: hysteresis noise gate + hold + downward expander.
//
// Pure dcdsp core. RT-safe: allocation only in prepare(); process() never
// allocates, locks or throws. Header-only. Depends on dcdsp::EnvelopeFollower.
//
// Behaviour
//   * Level is sensed by a fast peak EnvelopeFollower (fixed detector times).
//   * Hysteresis: an OPEN threshold (thresholdDb) and a lower CLOSE threshold
//     (thresholdDb - kHysteresisDb) stop the gate chattering around the knee.
//   * Hold: after the level falls below the close threshold the gate stays open
//     for holdMs before it is allowed to close.
//   * Below the threshold the target follows a downward-expander curve
//     (ratio:1) rather than a hard mute, so gentle ratios expand and large
//     ratios approach a hard gate. Attenuation is floored at kFloorDb.
//   * The open/close gain is ramped with attackMs (opening) / releaseMs
//     (closing) one-pole ballistics.
//
// Multi-channel linking: a single detector = max |x| across all channels drives
// one shared gain that is applied identically to every channel, so a stereo (or
// wider) signal opens/closes as a unit and the image never shifts. (Max, not
// sum: the loudest channel keeps the gate open — the safe choice for a gate.)
//
// Auto threshold (`autoFloor`, DotCharter #155) — the threshold follows the
// input's own noise floor, so the gate is shut at idle WHATEVER the
// interface's hiss and hum, and never sits far above them. The floor is
// measured continuously, not once at a calibration step: the detector's
// envelope is reduced to one maximum per 50 ms frame (two and a half cycles
// or more of 50 or 60 Hz mains hum, so where a frame falls in the hum's cycle
// does not move its maximum), and every 100 ms the last second of frames is
// examined. When it is STATIONARY (its 90th and 10th percentiles within
// kStationaryDb: hiss and hum are, a note's decay at 4 dB/s and more is not)
// its 90th percentile is the floor: a lower one is taken at once, a higher one
// at most kFloorRiseDbPerSec, except the very first reading, which is the
// calibration. The threshold is then floor + kFloorMarginDb, never above
// `thresholdDb` (in this mode the CEILING) and never below kFloorMinDb +
// kFloorMarginDb (digital silence). Before any reading the floor is ASSUMED to
// be digital silence (kFloorStartDb == kFloorMinDb): the most open the auto
// threshold ever gets, so nothing played before the first stationary second is
// cut. All of it is fixed-size state: no allocation, two nth_element calls
// over 20 doubles every 100 ms.
#pragma once

#ifndef DCDSP_PRIMITIVES_INCLUDED
#include "dcdsp_primitives.h"
#endif

#include <array>
#include <cmath>
#include <cstddef>
#include <algorithm>

namespace dcdsp {

struct GateParams {
    float thresholdDb = -60.0f; // open threshold, dBFS      [-80 .. 0]
    float ratio       = 10.0f;  // downward-expander ratio    [1 .. 50]
    float attackMs    = 1.0f;   // gain open time             [0.1 .. 50]
    float holdMs      = 50.0f;  // stay-open time after drop  [0 .. 500]
    float releaseMs   = 100.0f; // gain close time            [5 .. 1000]
    int   autoFloor   = 0;      // 1 = follow the noise floor; thresholdDb is the ceiling
};

class NoiseGateCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int /*numChannels*/) {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        frameLen_ = std::max(1L, static_cast<long>(kFrameMs * 0.001 * sr_ + 0.5));
        detector_.prepare(sr_);
        detector_.setTimes(kDetAttackMs, kDetReleaseMs);
        recalc();
        reset();
    }

    void reset() {
        detector_.reset();
        gain_ = 0.0;      // start closed
        open_ = false;
        holdLeft_ = 0;
        floorDb_ = kFloorStartDb;
        calibrated_ = false;
        frameMaxDb_ = kSilenceDb;
        frameFill_ = 0;
        ringPos_ = 0;
        ringFill_ = 0;
        framesSinceEval_ = 0;
    }

    void setParams(const GateParams& p) {
        p_ = p;
        clamp();
        recalc();
    }

    /// `key` (ABI v34) is an EXTERNAL sidechain: the envelope follower reads it
    /// instead of the audio. Null = key off the input (every caller before v34,
    /// and still the default). Unlike the compressor there is no HPF here, so
    /// the key needs no per-channel state and is not clamped to the prepared
    /// channel count.
    void process(float* const* channels, int numChannels, int numFrames,
                 const float* const* key = nullptr, int keyChannels = 0) {
        if (numChannels <= 0 || numFrames <= 0) return;
        const float* const* det_src = (key != nullptr && keyChannels > 0) ? key : channels;
        const int detCh = (key != nullptr && keyChannels > 0) ? keyChannels : numChannels;

        const bool autoFloor = p_.autoFloor != 0;
        const double ratio   = p_.ratio;

        for (int n = 0; n < numFrames; ++n) {
            // Linked detector: max |x| across channels.
            double det = 0.0;
            for (int ch = 0; ch < detCh; ++ch) {
                const float* src = det_src[ch];
                if (src == nullptr) continue;
                const double a = std::fabs(static_cast<double>(src[n]));
                if (a > det) det = a;
            }
            const double env = detector_.process(static_cast<float>(det));
            const double envDb = 20.0 * std::log10(env + kTiny);
            if (autoFloor) trackFloor(envDb);
            const double threshold = autoFloor ? autoThresholdDb() : static_cast<double>(p_.thresholdDb);
            const double openTh  = threshold;
            const double closeTh = threshold - kHysteresisDb;

            // Hysteresis + hold state machine.
            if (envDb >= openTh) {
                open_ = true;
                holdLeft_ = holdSamples_;
            } else if (envDb < closeTh) {
                if (open_) {
                    if (holdLeft_ > 0) --holdLeft_;
                    else open_ = false;
                }
            } // else: inside hysteresis band -> keep current state.

            // Target gain (linear).
            double targetDb;
            if (open_) {
                targetDb = 0.0; // fully open -> unity
            } else {
                const double over = envDb - threshold; // < 0 below threshold
                targetDb = (over >= 0.0) ? 0.0 : (ratio - 1.0) * over;
                if (targetDb < kFloorDb) targetDb = kFloorDb;
            }
            const double target = dbToGain(targetDb);   // exp2 form

            // Attack when opening (target above current), release when closing.
            const double coef = (target > gain_) ? attackCoef_ : releaseCoef_;
            gain_ = coef * gain_ + (1.0 - coef) * target;
            // RECOVER, do not merely scrub the output. gain_ is PERSISTENT: once
            // it is non-finite every subsequent sample is multiplied by NaN, the
            // chain's post-stage scrub prints permanent silence, and a later
            // VALID setParams never repairs it — only reset() does, and reset()
            // has exactly one caller. Unity is the safe landing: the gate opens.
            if (!std::isfinite(gain_)) gain_ = 1.0;

            const float g = static_cast<float>(gain_);
            for (int ch = 0; ch < numChannels; ++ch)
                channels[ch][n] *= g;
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the measured noise floor and the threshold it gives, in dB.
    double floorDbForTest() const { return floorDb_; }
    double autoThresholdDbForTest() const { return autoThresholdDb(); }

    static constexpr double kFrameMs           = 50.0;   // one envelope maximum per frame: 2.5+ mains cycles
    static constexpr int    kWindowFrames      = 20;     // one second of frames
    static constexpr int    kEvalEveryFrames   = 2;      // examined every 100 ms
    static constexpr double kStationaryDb      = 4.0;    // p90 - p10 of a floor
    static constexpr double kFloorMarginDb     = 6.0;    // threshold over the floor: twice its peak amplitude
    static constexpr double kFloorMinDb        = -96.3;  // one 16-bit LSB, 20 log10(2^-16): digital silence
    static constexpr double kFloorStartDb      = kFloorMinDb; // the floor before any reading (see the header)
    static constexpr double kFloorRiseDbPerSec = 20.0;   // a louder floor is followed this fast: digital
                                                         // silence (-96) to a hissy -60 in under 2 s

private:
    double autoThresholdDb() const {
        return std::min(static_cast<double>(p_.thresholdDb), floorDb_ + kFloorMarginDb);
    }

    // One sample of the floor measurement (see the header): fixed-size, RT-safe.
    void trackFloor(double envDb) {
        if (envDb > frameMaxDb_) frameMaxDb_ = envDb;
        if (++frameFill_ < frameLen_) return;
        ring_[static_cast<std::size_t>(ringPos_)] = frameMaxDb_;
        ringPos_ = (ringPos_ + 1) % kWindowFrames;
        if (ringFill_ < kWindowFrames) ++ringFill_;
        frameMaxDb_ = kSilenceDb;
        frameFill_ = 0;
        if (ringFill_ < kWindowFrames || ++framesSinceEval_ < kEvalEveryFrames) return;
        framesSinceEval_ = 0;

        scratch_ = ring_;
        constexpr std::size_t lo = kWindowFrames / 10, hi = kWindowFrames - 1 - kWindowFrames / 10;
        std::nth_element(scratch_.begin(), scratch_.begin() + hi, scratch_.end());
        const double p90 = scratch_[hi];
        std::nth_element(scratch_.begin(), scratch_.begin() + lo, scratch_.begin() + hi);
        const double p10 = scratch_[lo];
        if (!(p90 - p10 <= kStationaryDb)) return;   // a NaN is not stationary either

        const double reading = std::max(p90, kFloorMinDb);
        if (!calibrated_ || reading < floorDb_) {
            floorDb_ = reading;
        } else {
            const double rise = kFloorRiseDbPerSec * kEvalEveryFrames * kFrameMs * 0.001;
            floorDb_ = std::min(reading, floorDb_ + rise);
        }
        calibrated_ = true;
    }

    void clamp() {
        p_.thresholdDb = clampf(p_.thresholdDb, -80.0f, 0.0f);
        p_.ratio       = clampf(p_.ratio, 1.0f, 50.0f);
        p_.attackMs    = clampf(p_.attackMs, 0.1f, 50.0f);
        p_.holdMs      = clampf(p_.holdMs, 0.0f, 500.0f);
        p_.releaseMs   = clampf(p_.releaseMs, 5.0f, 1000.0f);
        p_.autoFloor   = p_.autoFloor != 0 ? 1 : 0;
    }
    void recalc() {
        attackCoef_  = tcoef(p_.attackMs);
        releaseCoef_ = tcoef(p_.releaseMs);
        holdSamples_ = static_cast<long>(p_.holdMs * 0.001 * sr_ + 0.5);
    }
    double tcoef(double ms) const {
        if (ms <= 0.0) return 0.0;
        return std::exp(-1.0 / (ms * 0.001 * sr_));
    }
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr double kHysteresisDb = 3.0;   // open/close gap
    static constexpr double kFloorDb      = -120.0;// max attenuation
    static constexpr double kTiny         = 1e-9;
    static constexpr float  kDetAttackMs  = 1.0f;  // detector ballistics
    static constexpr float  kDetReleaseMs = 10.0f;
    static constexpr double kSilenceDb    = -300.0;

    GateParams p_{};
    EnvelopeFollower detector_{};
    double sr_ = 48000.0;
    double gain_ = 0.0;
    double attackCoef_ = 0.0, releaseCoef_ = 0.0;
    long holdSamples_ = 0, holdLeft_ = 0;
    bool open_ = false;

    // The auto threshold's floor measurement (sized at compile time).
    long frameLen_ = 2400, frameFill_ = 0;
    double frameMaxDb_ = kSilenceDb;
    std::array<double, kWindowFrames> ring_{};
    std::array<double, kWindowFrames> scratch_{};
    int ringPos_ = 0, ringFill_ = 0, framesSinceEval_ = 0;
    double floorDb_ = kFloorStartDb;
    bool calibrated_ = false;
};

} // namespace dcdsp
