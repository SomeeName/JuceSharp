// dcdsp_transient.h — TransientCore: a level-independent transient designer.
//
// Models the SPL Transient Designer / Boss TAE shape: two envelope followers on
// the same linked detector, one FAST and one SLOW, and the DIFFERENCE between
// them says where in the note you are. Fast above slow = an attack is
// happening; fast below slow = the note is decaying. The attack knob gains the
// first, the sustain knob the second, and neither is a threshold — that is the
// whole point of this topology: it works the same on a quiet note and a loud
// one, so there is no threshold control to ride.
//
// EASY TO GET WRONG: the two followers must be driven from ONE linked detector
// (max |x| over the prepared channels), not per channel. Per-channel followers
// give each channel its own attack gain, which pans every pick across the
// stereo image. And the envelope ratio is read in dB and DIVIDED by a
// sensitivity-derived span — a raw linear ratio makes the knob do nothing for
// the first two thirds of its travel.
//
// `sensitivity` is that span, and it is therefore a MULTIPLIER on the applied
// gain, not a shape: gainDb = attackDb * diffDb / span, so 0 -> 1 moves span
// 18 -> 5 dB and multiplies the gain by up to 3.6x. It is smoothed as 1/span,
// which is what the inner loop actually wants, over the same 10 ms as the two
// dB knobs and the mix. It was a bare double until 2026-09-12 and moved the
// applied gain from +3 dB to +12 dB in ONE sample.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"   // EnvelopeFollower, SmoothedValue
#endif

#include <cmath>
#include <cstddef>

namespace dcdsp {

struct TransientParams {
    float attackDb    = 0.0f;  // gain on the attack phase   [-12 .. 12]
    float sustainDb   = 0.0f;  // gain on the decay phase    [-12 .. 12]
    float sensitivity = 0.5f;  // how little difference counts [0 .. 1]
    float mix         = 1.0f;  // dry/wet                     [0 .. 1]
};

class TransientCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_  = sampleRate > 0.0 ? sampleRate : 48000.0;
        nch_ = numChannels > 0 ? numChannels : 1;

        // Rate-owned: both followers' coefficients come from the rate.
        fast_.prepare(sr_);
        fast_.setTimes(kFastAttackMs, kFastReleaseMs);
        slow_.prepare(sr_);
        slow_.setTimes(kSlowAttackMs, kSlowReleaseMs);

        attackSmooth_.reset(sr_, kRampSeconds);
        sustainSmooth_.reset(sr_, kRampSeconds);
        mixSmooth_.reset(sr_, kRampSeconds);
        invSpanSmooth_.reset(sr_, kRampSeconds);
        pushTargets();
        attackSmooth_.snapToTarget();
        sustainSmooth_.snapToTarget();
        mixSmooth_.snapToTarget();      // start AT the configured settings
        invSpanSmooth_.snapToTarget();
        reset();
    }

    void reset() {
        fast_.reset();
        slow_.reset();
    }

    void setParams(const TransientParams& p) {
        p_ = p;
        clamp();
        pushTargets();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        const int nch = numChannels < nch_ ? numChannels : nch_;

        for (int n = 0; n < numFrames; ++n) {
            // ONE linked detector for both followers (see the header note).
            float det = 0.0f;
            for (int ch = 0; ch < nch; ++ch) {
                const float* p = channels[ch];
                if (p == nullptr) continue;
                const float a = std::fabs(p[n]);
                if (a > det) det = a;
            }

            const double f = (double) fast_.process(det);
            const double s = (double) slow_.process(det);
            // dB difference. Positive = the fast follower is ahead = an attack.
            const double diffDb = 20.0 * std::log10((f + kTiny) / (s + kTiny));

            const float attackDb  = attackSmooth_.next();
            const float sustainDb = sustainSmooth_.next();
            const float mix       = mixSmooth_.next();
            const double invSpan  = (double) invSpanSmooth_.next();

            double amt = diffDb * invSpan;
            if (amt > 1.0) amt = 1.0; else if (amt < -1.0) amt = -1.0;
            if (! std::isfinite(amt)) amt = 0.0;

            const double gainDb = (amt >= 0.0) ? (double) attackDb * amt
                                               : (double) sustainDb * -amt;
            // exp2, not pow(10, db/20): the same function, measured four
            // times cheaper, and this is the per-sample path.
            const float g = (float) dbToGain(gainDb);

            for (int ch = 0; ch < nch; ++ch) {
                float* p = channels[ch];
                if (p == nullptr) continue;
                const float dry = p[n];
                p[n] = dry + (dry * g - dry) * mix;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    void clamp() {
        p_.attackDb    = clampf(p_.attackDb, -12.0f, 12.0f, 0.0f);
        p_.sustainDb   = clampf(p_.sustainDb, -12.0f, 12.0f, 0.0f);
        p_.sensitivity = clampf(p_.sensitivity, 0.0f, 1.0f, 0.5f);
        p_.mix         = clampf(p_.mix, 0.0f, 1.0f, 1.0f);
    }

    void pushTargets() {
        attackSmooth_.setTarget(p_.attackDb);
        sustainSmooth_.setTarget(p_.sustainDb);
        mixSmooth_.setTarget(p_.mix);
        // How many dB of fast-vs-slow difference counts as "fully an attack".
        // High sensitivity = a small difference is already the whole knob.
        // Smoothed as its RECIPROCAL: that is the quantity the inner loop
        // multiplies by, so smoothing it is one walk rather than a walk plus a
        // per-sample divide.
        const double span = (double) kSpanMaxDb
                          + (double) p_.sensitivity * (kSpanMinDb - kSpanMaxDb);
        invSpanSmooth_.setTarget((float) (1.0 / span));
    }

    /// NaN lands on `fallback` — both halves of the obvious comparison are
    /// false for a NaN, so it is spelled out rather than left to `<`/`>`.
    static float clampf(float v, float lo, float hi, float fallback) {
        if (! std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr double kTiny = 1e-9;
    static constexpr float  kFastAttackMs  = 0.5f;
    static constexpr float  kFastReleaseMs = 20.0f;
    static constexpr float  kSlowAttackMs  = 20.0f;
    static constexpr float  kSlowReleaseMs = 150.0f;
    static constexpr double kSpanMaxDb = 18.0;   // sensitivity 0
    static constexpr double kSpanMinDb = 5.0;    // sensitivity 1
    static constexpr float  kRampSeconds = 0.010f;

    TransientParams p_{};
    double sr_  = 48000.0;
    int    nch_ = 1;
    EnvelopeFollower fast_{};
    EnvelopeFollower slow_{};
    SmoothedValue attackSmooth_;
    SmoothedValue sustainSmooth_;
    SmoothedValue mixSmooth_;
    SmoothedValue invSpanSmooth_;   // 1 / span, in 1/dB
};

} // namespace dcdsp
