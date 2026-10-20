// dcdsp_wah.h — WAH / AUTO-WAH DSP core (pure, header-only, RT-safe)
//
// A CryBaby-style inductor-model wah whose centre frequency is swept either by
// a manual pedal position (manual mode) or by an input-following envelope
// (auto-wah mode). The sweep uses a TPT state-variable filter, which stays
// stable under audio-rate cutoff modulation.
//
// 0.16 voicing (per R.G. Keen, "The Technology of Wah Pedals", and the
// ElectroSmash GCB-95 analysis): a real wah is NOT a plain bandpass — the
// inductor tank gives a resonant PEAK over a shelved passthrough: lows sit at
// a reduced floor, highs at a position-dependent shelf, and both the peak
// gain and the Q grow toward toe-down (heel ~400 Hz wide/soft, toe ~2 kHz
// sharp/honky). Modelled as a position-weighted LP + normalised-BP + HP mix
// of the same SVF:  wet = gLo*lp + gPeak*(bp/Q) + gHi*hp.
// `resonance` scales the position-dependent Q/peak curve (0.5 = stock).
//
// Contract shape (see dsp-core-contract.md):
//   struct WahParams { ... };
//   class  WahCore  { prepare / reset / setParams / process / latencySamples };
//
// Real-time safety: the only allocation happens in prepare(); process() never
// allocates, locks, or throws. Every output sample is clamped to a safe bound so
// extreme resonance + hot input can never blow up downstream stages.

#pragma once

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

// Effect agents code against the shared dcdsp_primitives.h signatures. For a
// self-contained compile-test we pull in a minimal local stub of ONLY the
// primitives this core uses. Production builds define nothing and get the real
// shared header.
#ifdef DCDSP_WAH_TEST_STUBS
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

namespace dcdsp {

// ---------------------------------------------------------------------------
// Parameters. All values are clamped internally, so out-of-range input is safe.
// ---------------------------------------------------------------------------
struct WahParams
{
    int   mode        = 0;       // 0 = auto (envelope-driven), 1 = manual (pedal)
    float position    = 0.5f;    // [0..1] manual pedal position (0 = low/heel, 1 = high/toe)
    float sensitivity = 0.5f;    // [0..1] auto-wah envelope sensitivity (how far input opens the filter)
    float rangeLoHz   = 400.0f;  // [200..800]   sweep lower centre-frequency bound (Hz)
    float rangeHiHz   = 2000.0f; // [1200..3000] sweep upper centre-frequency bound (Hz)
    float resonance   = 0.5f;    // [0..1] filter Q amount (0 = gentle, 1 = sharp honk)
    float mix         = 1.0f;    // [0..1] dry/wet blend (0 = dry, 1 = fully wet)
};

// ---------------------------------------------------------------------------
// WahCore
// ---------------------------------------------------------------------------
class WahCore
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels)
    {
        (void)maxBlockSize;
        sampleRate_  = (sampleRate > 0.0) ? sampleRate : 44100.0;
        maxChannels_ = (numChannels > 0) ? numChannels : 1;

        svf_.assign(static_cast<std::size_t>(maxChannels_), StateVariableTPT{});

        env_.prepare(sampleRate_);
        env_.setTimes(kEnvAttackMs, kEnvReleaseMs);

        // ~15 ms sweep smoothing: fast enough to feel responsive, slow enough
        // to avoid zipper noise when the pedal / envelope moves.
        posSmooth_.reset(sampleRate_, 0.015f);
        resS_.reset(sampleRate_, kResRampSeconds);
        resS_.setTarget(resFactor_);
        resS_.snapToTarget();   // a freshly prepared wah starts AT its resonance
        // `mix` RAMPS too (kMixRampSeconds). The sweep position was already
        // smoothed; the mix was one constant per block, and the wet path carries
        // up to ~+15 dB of peak gain, so a mix drag stepped a large level
        // difference rather than a subtle one.
        mixSmooth_.reset(sampleRate_, kMixRampSeconds);
        mixSmooth_.setTarget(mix_);
        mixSmooth_.snapToTarget();   // a freshly prepared wah starts AT its mix

        reset();
    }

    void reset()
    {
        for (auto& f : svf_)
            f.reset();
        env_.reset();
        posSmooth_.reset(sampleRate_, 0.015f);
        posSmooth_.setTarget(clamp01(params_.mode == 1 ? params_.position : 0.0f));
        // ...and START there. reset() is reached on every un-bypass of a parked
        // node, so without the snap a manual wah sweeps heel -> position over
        // 15 ms every time the pedal comes back in.
        posSmooth_.snapToTarget();
    }

    void setParams(const WahParams& p)
    {
        params_ = p;

        // Sanitize ranges. Keep a minimum separation so the log sweep is valid.
        float lo = clampf(params_.rangeLoHz, 20.0f, 8000.0f);
        float hi = clampf(params_.rangeHiHz, 20.0f, 12000.0f);
        if (hi < lo + 10.0f) hi = lo + 10.0f;
        loHz_  = lo;
        hiHz_  = hi;
        ratio_ = hi / lo;
        log2Ratio_ = std::log2(static_cast<double>(ratio_));

        // Resonance scales the position-dependent Q / peak-gain curves.
        // res = 0.5 -> factor 1.0 (stock CryBaby-ish voicing).
        resFactor_ = 0.5f + clamp01(params_.resonance);
        resS_.setTarget(resFactor_);

        mix_         = clamp01(params_.mix);
        mixSmooth_.setTarget(mix_);
        sensitivity_ = clamp01(params_.sensitivity);
        position_    = clamp01(params_.position);
        auto_        = (params_.mode != 1);
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
        // W4-D4: `resonance` scales the position-dependent Q AND peak-gain
        // curves, so a knob step retuned the tank and moved up to ~+15 dB of
        // peak gain in one sample. Read per sample; at rest resS_ IS resFactor_.

        // Auto-wah drive: higher sensitivity opens the filter further for a given
        // input level. envGain scales the (unbounded) peak envelope into [0..1].
        const float envGain = 0.75f + sensitivity_ * 7.25f; // ~0.75 .. 8.0

        for (int n = 0; n < numFrames; ++n)
        {
            // --- pick the sweep target for this sample -------------------------
            float target;
            if (auto_)
            {
                // Drive the envelope from the mono sum of the active channels so
                // both channels sweep coherently.
                float mono = 0.0f;
                for (int c = 0; c < nch; ++c)
                    mono += channels[c][n];
                mono *= invNch;

                const float e = env_.process(mono);      // peak of |mono|, >= 0
                target = clamp01(e * envGain);
            }
            else
            {
                target = position_;
            }

            const float mix     = mixSmooth_.next();
            const float dryGain = 1.0f - mix;

            posSmooth_.setTarget(target);
            float t = posSmooth_.next();
            t = clamp01(t);

            // Exponential (musical) mapping from sweep position to centre Hz.
            // ratio_^t == exp2(t * log2(ratio_)); log2Ratio_ is cached by
            // setParams, so the per-sample cost is one exp2 instead of a double
            // std::pow. Same value to within a ULP.
            double fc = static_cast<double>(loHz_)
                      * std::exp2(static_cast<double>(t) * log2Ratio_);
            if (fc < 20.0)    fc = 20.0;
            if (fc > fcMax)   fc = fcMax;

            // Position-dependent inductor-tank voicing (linear interps of the
            // measured-curve endpoints; heel = t 0, toe = t 1):
            //   Q:        wide/soft heel -> sharp toe, scaled by resonance
            //   peak:     ~+7 dB heel -> ~+15 dB toe (linear-gain interp)
            //   HF shelf: highs above the peak sit lower toward toe
            //   LF floor: constant reduced low passthrough
            const float resF = resS_.next();
            appliedRes_ = resF;   // test surface; see resonanceFactorCurrentForTest
            float Q = (kQHeel + (kQToe - kQHeel) * t) * resF;
            Q = clampf(Q, 0.5f, 14.0f);
            const float invQ  = 1.0f / Q;
            const float gPeak = (kPeakHeel + (kPeakToe - kPeakHeel) * t) * resF;
            const float gHi   = kHiHeel + (kHiToe - kHiHeel) * t;
            const float gLo   = kLoFloor;

            // --- filter each channel with the shared centre frequency ----------
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

    /// Test-only: the SMOOTHED resonance factor the tank is running at right
    /// now (0.5 .. 1.5; 1.0 is the stock CryBaby voicing). Between the two
    /// endpoints while the ramp is in flight — the claim W4-D4's fix makes, and
    /// one no output-domain probe can separate from the resonant SVF's own
    /// settling (measured: DspGainZipperTests' banded theory passes with the
    /// switch hard at both a 16- and a 48-sample window).
    ///
    /// It records what process() ACTUALLY APPLIED on its last sample, not
    /// what the smoother holds: a probe reading the smoother would pass even
    /// if the core went on using the UN-smoothed member, which is exactly the
    /// mistake this accessor exists to avoid (measured — the first version of
    /// it made that mistake and the gate case passed with the fix reverted).
    float resonanceFactorCurrentForTest() const { return appliedRes_; }

    // Test-only: the smoother's CURRENT value, so a gate case can assert a
    // freshly prepared core starts AT its configured value rather than
    // ramping up to it from 0 (SmoothedValue::reset() preserves mCurrent).
    float positionCurrentForTest() const { return posSmooth_.current(); }

private:
    // CryBaby-curve endpoints (heel = position 0, toe = position 1), applied
    // at resonance = 0.5 (resFactor 1). Peak/HF values are LINEAR gains.
    static constexpr float kQHeel    = 1.6f;    // wide, vowel-y heel
    static constexpr float kQToe     = 6.5f;    // sharp toe honk
    static constexpr float kPeakHeel = 2.24f;   // +7 dB
    static constexpr float kPeakToe  = 5.62f;   // +15 dB
    static constexpr float kHiHeel   = 0.45f;   // ~-7 dB shelf above the peak
    static constexpr float kHiToe    = 0.25f;   // ~-12 dB toward toe
    static constexpr float kLoFloor  = 0.35f;   // ~-9 dB low passthrough
    static constexpr float kOutClamp     = 16.0f;  // hard safety bound (< 32 required)
    static constexpr float kEnvAttackMs  = 5.0f;
    static constexpr float kEnvReleaseMs = 80.0f;

    static float clampf(float v, float lo, float hi)
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }
    static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

    double sampleRate_  = 44100.0;
    int    maxChannels_ = 1;

    WahParams params_{};

    // Derived (cached) params — recomputed cheaply in setParams.
    float loHz_        = 400.0f;
    float hiHz_        = 2000.0f;
    float  ratio_      = 5.0f;
    double log2Ratio_  = 2.3219280948873623;   // log2(5), cached for the sweep
    float resFactor_   = 1.0f;
    static constexpr float kResRampSeconds = 0.020f;
    SmoothedValue resS_;   // the smoothed tank scale (W4-D4)
    float appliedRes_ = 1.0f;   // test surface only
    float mix_         = 1.0f;
    float sensitivity_ = 0.5f;
    float position_    = 0.5f;
    bool  auto_        = true;

    std::vector<StateVariableTPT> svf_;   // one filter per channel
    EnvelopeFollower              env_;   // shared auto-wah envelope
    static constexpr float        kMixRampSeconds = 0.010f;
    SmoothedValue                 posSmooth_;
    SmoothedValue                 mixSmooth_;
};

} // namespace dcdsp
