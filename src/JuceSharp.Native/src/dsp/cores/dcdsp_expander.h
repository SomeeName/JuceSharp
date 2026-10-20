// dcdsp_expander.h — ExpanderCore: a downward expander with a bounded range.
//
// The gate's quieter sibling. Where dcdsp_gate.h is a hysteresis STATE MACHINE
// that opens and shuts, this is a continuous curve: everything below the
// threshold is pushed down by (ratio - 1) times how far below it is, so a noise
// floor 10 dB under the threshold at 2:1 loses 10 dB and a note that pokes
// through loses nothing. `rangeDb` is the FLOOR — the most it may ever take —
// which is what keeps it a musical expander instead of a slow gate.
//
// Sidechain-capable (ABI v34): the detector reads the key, the audio path does
// not change. Keyed off the seat's dry input an expander late in the chain
// follows the PICK rather than the hiss the drive in front of it is making,
// which is the whole reason to reach for one.
//
// EASY TO GET WRONG: `attackMs` is how fast the gain comes UP (the expander
// opening), `releaseMs` how fast it goes DOWN. Reading them the other way round
// makes a fast attack chop the front off every note. The ballistics therefore
// branch on the GAIN's direction, not on the detector's.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"   // EnvelopeFollower, SmoothedValue
#endif

#include <cmath>
#include <cstddef>

namespace dcdsp {

struct ExpanderParams {
    float thresholdDb = -40.0f; // expansion starts below this, dBFS [-80 .. 0]
    float ratio       = 2.0f;   // downward expansion ratio          [1 .. 10]
    float attackMs    = 10.0f;  // gain rise (opening)               [0.1 .. 100]
    float releaseMs   = 120.0f; // gain fall (closing)               [5 .. 1000]
    float rangeDb     = 30.0f;  // maximum attenuation               [0 .. 60]
};

class ExpanderCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_  = sampleRate > 0.0 ? sampleRate : 48000.0;
        nch_ = numChannels > 0 ? numChannels : 1;
        detector_.prepare(sr_);                       // rate-owned
        detector_.setTimes(kDetAttackMs, kDetReleaseMs);
        rangeSmooth_.reset(sr_, kRangeRampSeconds);
        rangeSmooth_.setTarget(p_.rangeDb);
        rangeSmooth_.snapToTarget();                  // start AT the configured range
        recalc();                                     // rate-owned ballistics
        reset();
    }

    void reset() {
        detector_.reset();
        // Start OPEN, not closed. A gate starts shut because that is its resting
        // state; an expander's is unity, and starting at 0 makes the first note
        // after every prepare fade in.
        gainDb_ = 0.0;
    }

    void setParams(const ExpanderParams& p) {
        p_ = p;
        clamp();
        rangeSmooth_.setTarget(p_.rangeDb);
        recalc();
    }

    /// `key` (ABI v34) is an EXTERNAL sidechain: the detector reads it instead
    /// of the audio, and the audio path is untouched. Null = key off the input,
    /// which is the default and what an unkeyed node does.
    void process(float* const* channels, int numChannels, int numFrames,
                 const float* const* key = nullptr, int keyChannels = 0) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        const int nch = numChannels < nch_ ? numChannels : nch_;
        const float* const* det_src = (key != nullptr && keyChannels > 0) ? key : channels;
        const int detCh = (key != nullptr && keyChannels > 0) ? keyChannels : nch;

        const double T = p_.thresholdDb;
        const double slope = (double) p_.ratio - 1.0;

        for (int n = 0; n < numFrames; ++n) {
            double det = 0.0;
            for (int ch = 0; ch < detCh; ++ch) {
                const float* src = det_src[ch];
                if (src == nullptr) continue;
                const double a = std::fabs((double) src[n]);
                if (a > det) det = a;
            }
            const double env   = (double) detector_.process((float) det);
            const double envDb = 20.0 * std::log10(env + kTiny);

            // Downward expansion: nothing above the threshold, (ratio-1) dB of
            // attenuation per dB below it, floored at -rangeDb.
            const double range = (double) rangeSmooth_.next();
            const double over  = envDb - T;
            double targetDb = (over >= 0.0) ? 0.0 : slope * over;
            if (targetDb < -range) targetDb = -range;
            if (! std::isfinite(targetDb)) targetDb = 0.0;

            // Ballistics in the GAIN domain and branched on the gain's own
            // direction: attack = coming up, release = going down.
            const double coef = (targetDb > gainDb_) ? attackCoef_ : releaseCoef_;
            gainDb_ = coef * gainDb_ + (1.0 - coef) * targetDb;
            // RECOVER rather than scrub: gainDb_ is persistent, so one NaN
            // would silence the node for the life of the chain and only reset()
            // could repair it. Unity is the safe landing.
            if (! std::isfinite(gainDb_)) gainDb_ = 0.0;

            const float g = (float) dbToGain(gainDb_);
            for (int ch = 0; ch < nch; ++ch)
                if (channels[ch] != nullptr) channels[ch][n] *= g;
        }
    }

    int latencySamples() const { return 0; }

private:
    void clamp() {
        p_.thresholdDb = clampf(p_.thresholdDb, -80.0f, 0.0f, -40.0f);
        p_.ratio       = clampf(p_.ratio, 1.0f, 10.0f, 1.0f);
        p_.attackMs    = clampf(p_.attackMs, 0.1f, 100.0f, 10.0f);
        p_.releaseMs   = clampf(p_.releaseMs, 5.0f, 1000.0f, 120.0f);
        p_.rangeDb     = clampf(p_.rangeDb, 0.0f, 60.0f, 30.0f);
    }
    void recalc() {
        attackCoef_  = tcoef(p_.attackMs);
        releaseCoef_ = tcoef(p_.releaseMs);
    }
    double tcoef(double ms) const {
        if (! (ms > 0.0)) return 0.0;
        return std::exp(-1.0 / (ms * 0.001 * sr_));
    }
    static float clampf(float v, float lo, float hi, float fallback) {
        if (! std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr double kTiny = 1e-9;
    static constexpr float  kDetAttackMs  = 1.0f;
    /// 50 ms, NOT the gate's 10 ms, and the difference is measurable. This
    /// module's whole contract is a CURVE — (level - threshold) x (ratio - 1) dB
    /// — so an envelope that sags between the peaks of a waveform is an error
    /// MULTIPLIED by (ratio - 1): a 10 ms release sagged 1.1 dB on a 1 kHz sine,
    /// which came out as 2.2 dB of curve error at 3:1. A gate only compares its
    /// envelope against a threshold, so the same sag costs it nothing.
    static constexpr float  kDetReleaseMs = 50.0f;
    static constexpr float  kRangeRampSeconds = 0.010f;

    ExpanderParams p_{};
    double sr_  = 48000.0;
    int    nch_ = 1;
    EnvelopeFollower detector_{};
    double gainDb_ = 0.0;
    double attackCoef_ = 0.0, releaseCoef_ = 0.0;
    SmoothedValue rangeSmooth_;
};

} // namespace dcdsp
