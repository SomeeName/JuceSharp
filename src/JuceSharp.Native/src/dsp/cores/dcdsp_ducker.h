// dcdsp_ducker.h — DuckerCore: a sidechain ducker.
//
// The broadcast "voice-over" pedal: while the KEY is above the threshold the
// through signal is pulled down by a fixed number of dB, held there for as long
// as the key keeps talking plus `holdMs`, and then let back up. It is NOT a
// compressor — the attenuation does not follow how far over the threshold the
// key is, which is exactly why it sounds deliberate rather than pumping.
//
// TOPOLOGY: linked peak follower over the key -> threshold + hold timer ->
// target of 0 or -duckDb in the dB domain -> attack/release ballistics ->
// one gain applied to every channel.
//
// EASY TO GET WRONG: with no external key it must key off its OWN input (the
// engine's documented fallback for an unavailable source), never off silence —
// a ducker keyed from silence never ducks, which reads as a dead pedal, and the
// same fallback on a GATE is the difference between "quiet" and "no sound at
// all". And the hold timer must be RE-ARMED on every sample the key is over,
// not started once, or a sustained key releases in the middle of itself.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"   // EnvelopeFollower, SmoothedValue
#endif

#include <cmath>
#include <cstddef>

namespace dcdsp {

struct DuckerParams {
    float thresholdDb = -30.0f; // key level that triggers, dBFS [-60 .. 0]
    float duckDb      = 12.0f;  // how far down, dB              [0 .. 40]
    float attackMs    = 10.0f;  // how fast it dips              [1 .. 200]
    float holdMs      = 50.0f;  // stay down after the key stops [0 .. 500]
    float releaseMs   = 300.0f; // how fast it comes back        [10 .. 2000]
};

class DuckerCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_  = sampleRate > 0.0 ? sampleRate : 48000.0;
        nch_ = numChannels > 0 ? numChannels : 1;
        detector_.prepare(sr_);                       // rate-owned
        detector_.setTimes(kDetAttackMs, kDetReleaseMs);
        duckSmooth_.reset(sr_, kDuckRampSeconds);
        duckSmooth_.setTarget(p_.duckDb);
        duckSmooth_.snapToTarget();                   // start AT the configured depth
        recalc();                                     // rate-owned: coefficients + hold
        reset();
    }

    void reset() {
        detector_.reset();
        gainDb_   = 0.0;   // resting state is UP: a ducker at rest is a wire
        holdLeft_ = 0;
    }

    void setParams(const DuckerParams& p) {
        p_ = p;
        clamp();
        duckSmooth_.setTarget(p_.duckDb);
        recalc();
    }

    /// `key` (ABI v34) is the EXTERNAL sidechain and is the point of this
    /// module. Null = key off the module's own input, which is the engine's
    /// documented fallback for an unavailable source — never silence.
    void process(float* const* channels, int numChannels, int numFrames,
                 const float* const* key = nullptr, int keyChannels = 0) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        const int nch = numChannels < nch_ ? numChannels : nch_;
        const float* const* det_src = (key != nullptr && keyChannels > 0) ? key : channels;
        const int detCh = (key != nullptr && keyChannels > 0) ? keyChannels : nch;

        const double th = p_.thresholdDb;

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

            // RE-ARMED every sample the key is over (see the header note).
            if (envDb >= th) holdLeft_ = holdSamples_;
            else if (holdLeft_ > 0) --holdLeft_;

            const double depth = (double) duckSmooth_.next();
            const double targetDb = (envDb >= th || holdLeft_ > 0) ? -depth : 0.0;

            const double coef = (targetDb < gainDb_) ? attackCoef_ : releaseCoef_;
            gainDb_ = coef * gainDb_ + (1.0 - coef) * targetDb;
            // RECOVER, do not merely scrub: gainDb_ is persistent, so one NaN
            // would silence this node for the life of the chain and only
            // reset() could repair it. Unity is the safe landing — a ducker
            // that has failed should be a wire, not a mute.
            if (! std::isfinite(gainDb_)) gainDb_ = 0.0;
            // A one-pole never arrives, so snap the last hundredth of a dB or
            // an idle ducker multiplies every sample by 0.9999 for ever and
            // "at rest it is a wire" stops being true.
            if (targetDb >= 0.0 && gainDb_ > -kSnapDb) gainDb_ = 0.0;

            const float g = (float) dbToGain(gainDb_);
            for (int ch = 0; ch < nch; ++ch)
                if (channels[ch] != nullptr) channels[ch][n] *= g;
        }
    }

    int latencySamples() const { return 0; }

private:
    void clamp() {
        p_.thresholdDb = clampf(p_.thresholdDb, -60.0f, 0.0f, -30.0f);
        p_.duckDb      = clampf(p_.duckDb, 0.0f, 40.0f, 0.0f);
        p_.attackMs    = clampf(p_.attackMs, 1.0f, 200.0f, 10.0f);
        p_.holdMs      = clampf(p_.holdMs, 0.0f, 500.0f, 50.0f);
        p_.releaseMs   = clampf(p_.releaseMs, 10.0f, 2000.0f, 300.0f);
    }
    void recalc() {
        attackCoef_  = tcoef(p_.attackMs);
        releaseCoef_ = tcoef(p_.releaseMs);
        holdSamples_ = (long) (p_.holdMs * 0.001 * sr_ + 0.5);
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
    static constexpr double kSnapDb = 0.005;
    static constexpr float  kDetAttackMs  = 1.0f;
    static constexpr float  kDetReleaseMs = 10.0f;
    static constexpr float  kDuckRampSeconds = 0.010f;

    DuckerParams p_{};
    double sr_  = 48000.0;
    int    nch_ = 1;
    EnvelopeFollower detector_{};
    double gainDb_ = 0.0;
    double attackCoef_ = 0.0, releaseCoef_ = 0.0;
    long   holdSamples_ = 0, holdLeft_ = 0;
    SmoothedValue duckSmooth_;
};

} // namespace dcdsp
