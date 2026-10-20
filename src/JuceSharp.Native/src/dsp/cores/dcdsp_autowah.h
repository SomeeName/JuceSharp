// dcdsp_autowah.h — AUTO-WAH / ENVELOPE WAH DSP core (pure, header-only, RT-safe)
//
// A dedicated envelope-driven wah (0.16 W5): the funky Mu-Tron / Q-Tron
// stomp. The filter voice is the CryBaby-style inductor-tank model shared
// with dcdsp_wah — a resonant PEAK over a shelved passthrough (lows at a
// reduced floor, highs at a position-dependent shelf, Q and peak gain growing
// toward the top of the sweep):  wet = gLo*lp + gPeak*(bp/Q) + gHi*hp of one
// TPT state-variable filter. What differs from the pedal wah is the DRIVER:
// the sweep position comes from an envelope follower with USER attack/release
// times, a sensitivity control, and a direction switch (up = louder opens the
// filter, down = louder closes it — the classic "reverse quack").
//
// Distinct from dcdsp_envfilter, which is a generic resonant SVF sweep; this
// core keeps the inductor-tank peak/shelf voicing so it quacks like the
// funk boxes rather than sweeping like a synth filter.
//
// Contract shape (see dsp-core-contract.md):
//   struct AutoWahParams { ... };
//   class  AutoWahCore  { prepare / reset / setParams / process / latencySamples };
//
// Real-time safety: the only allocation happens in prepare(); process() never
// allocates, locks, or throws. Every output sample is clamped to a safe bound
// so extreme resonance + hot input can never blow up downstream stages.

#pragma once

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

#ifdef DCDSP_TEST_STUBS
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

namespace dcdsp {

// ---------------------------------------------------------------------------
// Parameters. All values are clamped internally, so out-of-range input is safe.
// ---------------------------------------------------------------------------
struct AutoWahParams
{
    float sensitivity = 0.5f;    // [0..1]  how far the input level opens the sweep
    // The bracketed ranges below are the CLAMPS setParams/prepare actually
    // apply (they used to quote a narrower, aspirational set that matched
    // neither the core nor the catalogue row, which is what a reviewer trusts
    // when writing a ParamInfo).
    float attackMs    = 10.0f;   // [0.5..200]  envelope attack
    float releaseMs   = 100.0f;  // [5..1000]   envelope release
    float rangeLoHz   = 300.0f;  // [20..8000]  sweep lower centre-frequency bound
    float rangeHiHz   = 2200.0f; // [20..12000] sweep upper centre-frequency bound
                                 // (hi is forced to at least lo + 10 Hz)
    int   direction   = 0;       // 0 = up (louder -> higher), 1 = down (reverse quack)
    float mix         = 1.0f;    // [0..1] dry/wet blend
};

// ---------------------------------------------------------------------------
// AutoWahCore
// ---------------------------------------------------------------------------
class AutoWahCore
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels)
    {
        (void)maxBlockSize;
        sampleRate_  = (sampleRate > 0.0) ? sampleRate : 44100.0;
        maxChannels_ = (numChannels > 0) ? numChannels : 1;

        svf_.assign(static_cast<std::size_t>(maxChannels_), StateVariableTPT{});

        env_.prepare(sampleRate_);
        // CLAMPED, exactly as setParams does. Applying the raw values here made a
        // device change silently revert the clamp, and attackMs <= 0 makes
        // EnvelopeFollower::coeff return 0 — an instantaneous follower, a
        // different effect from the one the user configured.
        env_.setTimes(clampf(params_.attackMs, 0.5f, 200.0f),
                      clampf(params_.releaseMs, 5.0f, 1000.0f));

        // ~12 ms sweep smoothing on top of the envelope: keeps fast attack
        // settings from stair-stepping the filter coefficient updates.
        posSmooth_.reset(sampleRate_, 0.012f);
        // `mix` RAMPS too (kMixRampSeconds). The sweep was already smoothed and
        // the mix was still one constant per block — a full dry/wet crossfade
        // stepped at every block edge while the knob moved.
        mixSmooth_.reset(sampleRate_, kMixRampSeconds);
        mixSmooth_.setTarget(mix_);
        mixSmooth_.snapToTarget();   // a freshly prepared auto-wah starts AT its mix

        reset();
    }

    void reset()
    {
        for (auto& f : svf_)
            f.reset();
        env_.reset();
        posSmooth_.reset(sampleRate_, 0.012f);
        posSmooth_.setCurrentAndTarget(direction_ == 1 ? 1.0f : 0.0f);
    }

    void setParams(const AutoWahParams& p)
    {
        params_ = p;

        // Sanitize ranges. Keep a minimum separation so the log sweep is valid.
        float lo = clampf(params_.rangeLoHz, 20.0f, 8000.0f);
        float hi = clampf(params_.rangeHiHz, 20.0f, 12000.0f);
        if (hi < lo + 10.0f) hi = lo + 10.0f;
        loHz_  = lo;
        hiHz_  = hi;
        ratio_ = hi / lo;
        // Hoisted out of the per-sample sweep mapping below: pow(r, t) is
        // exp2(log2(r) * t), and log2(r) only changes when the range does.
        log2Ratio_ = std::log2(static_cast<double>(ratio_));

        env_.setTimes(clampf(params_.attackMs, 0.5f, 200.0f),
                      clampf(params_.releaseMs, 5.0f, 1000.0f));

        mix_         = clamp01(params_.mix);
        mixSmooth_.setTarget(mix_);
        sensitivity_ = clamp01(params_.sensitivity);
        direction_   = (params_.direction != 0) ? 1 : 0;
    }

    void process(float* const* channels, int numChannels, int numFrames)
    {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0)
            return;

        const int nch = (numChannels < maxChannels_) ? numChannels : maxChannels_;
        if (nch <= 0)
            return;

        const double sr      = sampleRate_;
        const double fcMax   = sr * 0.45;   // stay clear of Nyquist
        const float  invNch  = 1.0f / static_cast<float>(nch);
        const bool   down    = (direction_ == 1);

        // Envelope drive: higher sensitivity opens the sweep further for a
        // given input level (same law as the wah core's auto mode).
        const float envGain = 0.75f + sensitivity_ * 7.25f; // ~0.75 .. 8.0

        for (int n = 0; n < numFrames; ++n)
        {
            const float mix     = mixSmooth_.next();
            const float dryGain = 1.0f - mix;

            // Drive the envelope from the mono sum of the active channels so
            // both channels sweep coherently.
            float mono = 0.0f;
            for (int c = 0; c < nch; ++c)
                mono += channels[c][n];
            mono *= invNch;

            const float e = env_.process(mono);      // peak of |mono|, >= 0
            float target = clamp01(e * envGain);
            if (down) target = 1.0f - target;         // reverse quack

            posSmooth_.setTarget(target);
            float t = clamp01(posSmooth_.next());

            // Exponential (musical) mapping from sweep position to centre Hz.
            double fc = static_cast<double>(loHz_)
                        * std::exp2(log2Ratio_ * static_cast<double>(t));
            if (fc < 20.0)    fc = 20.0;
            if (fc > fcMax)   fc = fcMax;

            // Position-dependent inductor-tank voicing (same measured-curve
            // endpoints as dcdsp_wah at stock resonance): Q and peak grow
            // toward the top of the sweep, the HF shelf drops, lows sit at a
            // constant reduced floor.
            float Q = kQHeel + (kQToe - kQHeel) * t;
            Q = clampf(Q, 0.5f, 14.0f);
            const float invQ  = 1.0f / Q;
            const float gPeak = kPeakHeel + (kPeakToe - kPeakHeel) * t;
            const float gHi   = kHiHeel + (kHiToe - kHiHeel) * t;
            const float gLo   = kLoFloor;

            // --- filter each channel with the shared centre frequency ------
            for (int c = 0; c < nch; ++c)
            {
                const float dry = channels[c][n];

                StateVariableTPT& f = svf_[static_cast<std::size_t>(c)];
                f.set(sr, fc, static_cast<double>(Q));
                f.process(dry);
                float wet = gLo * f.lp() + gPeak * (f.bp() * invQ) + gHi * f.hp();

                float out = dry * dryGain + wet * mix;

                // Safety net: guarantee a finite, bounded output regardless of
                // resonance, input level, or transient filter state.
                if (!std::isfinite(out))
                    out = 0.0f;
                out = clampf(out, -kOutClamp, kOutClamp);

                channels[c][n] = out;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    // CryBaby-curve endpoints (bottom = position 0, top = position 1), the
    // stock-resonance voicing from dcdsp_wah. Peak/HF values are LINEAR gains.
    static constexpr float kQHeel    = 1.6f;    // wide, vowel-y bottom
    static constexpr float kQToe     = 6.5f;    // sharp top honk
    static constexpr float kPeakHeel = 2.24f;   // +7 dB
    static constexpr float kPeakToe  = 5.62f;   // +15 dB
    static constexpr float kHiHeel   = 0.45f;   // ~-7 dB shelf above the peak
    static constexpr float kHiToe    = 0.25f;   // ~-12 dB toward the top
    static constexpr float kLoFloor  = 0.35f;   // ~-9 dB low passthrough
    static constexpr float kOutClamp = 16.0f;   // hard safety bound (< 32 required)

    static float clampf(float v, float lo, float hi)
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }
    static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

    double sampleRate_  = 44100.0;
    int    maxChannels_ = 1;

    AutoWahParams params_{};

    // Derived (cached) params — recomputed cheaply in setParams.
    float loHz_        = 300.0f;
    float hiHz_        = 2200.0f;
    float ratio_       = 2200.0f / 300.0f;
    double log2Ratio_  = 1.874469117916141;   // log2(2200/300); re-derived with ratio_
    float mix_         = 1.0f;
    float sensitivity_ = 0.5f;
    int   direction_   = 0;

    std::vector<StateVariableTPT> svf_;   // one filter per channel
    EnvelopeFollower              env_;   // sweep driver (user attack/release)
    static constexpr float        kMixRampSeconds = 0.010f;
    SmoothedValue                 posSmooth_;
    SmoothedValue                 mixSmooth_;
};

} // namespace dcdsp
