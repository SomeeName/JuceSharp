#pragma once
// dcdsp_rotary.h — Rotary speaker / Leslie emulation core.
//
// Models a two-rotor rotary cabinet: a fast-spinning treble HORN and a slow,
// heavy bass DRUM, each on its own rotation phase with independent inertia.
// A crossover splits the signal into highs (horn) and lows (drum). Each rotor
// produces:
//   * Doppler pitch shift  — a short LFO-modulated delay whose length swings
//     oppositely for the two virtual microphones (L/R),
//   * amplitude tremolo    — the rotor swings toward/away from each mic,
//   * a rotating stereo image (the L/R pan is driven in quadrature).
// Switching between slow (chorale) and fast (tremolo) ramps the rotor speeds
// through their inertia, the drum lagging far behind the horn.
//
// Pure, header-only, RT-safe: allocation happens only in prepare(); process()
// never allocates, locks, or throws. All feedback/waveshaping paths are bounded.

// Production builds include the shared primitive header; the standalone
// compile-test defines DCDSP_ROTARY_TEST_STUBS to pull in a minimal local copy.
#ifdef DCDSP_ROTARY_TEST_STUBS
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

// ---------------------------------------------------------------------------
struct RotaryParams {
    // 0 = slow / "chorale", 1 = fast / "tremolo". Continuous: sets the target
    // rotation speed the rotors ramp toward (subject to per-rotor inertia).
    float speed = 0.0f;   // range 0..1
    // Overall modulation intensity (Doppler depth + tremolo depth + pan width).
    float depth = 1.0f;   // range 0..1
    // Dry/wet blend. 0 = bypass-dry, 1 = fully rotating.
    float mix   = 1.0f;   // range 0..1
};

// ---------------------------------------------------------------------------
class RotaryCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 512;
        numCh_ = (numChannels > 0) ? numChannels : 1;

        const double ms = sr_ / 1000.0;
        // Rotor delay geometry (in samples).
        hornMid_ = float(1.5 * ms);   // treble rotor: short throw
        hornMod_ = float(0.6 * ms);
        drumMid_ = float(2.6 * ms);   // bass rotor: longer throw
        drumMod_ = float(0.9 * ms);

        // Max delay any microphone tap can request, plus interpolation guard.
        const double maxDelay = double(drumMid_ + drumMod_) + 8.0;
        hornDelay_.prepare(int(std::ceil(maxDelay)));
        drumDelay_.prepare(int(std::ceil(maxDelay)));

        // Crossover ~ 800 Hz.
        drumLp_.setLowpass(sr_, 800.0);
        hornHp_.setHighpass(sr_, 800.0);

        // Rotor-speed inertia. The drum is heavy — it accelerates far slower
        // than the horn. Linear ramps approximate the spin-up/spin-down.
        hornFreq_.reset(sr_, 1.0f);   // ~1 s to change horn speed
        drumFreq_.reset(sr_, 2.4f);   // ~2.4 s for the sluggish drum

        // Param smoothers (avoid zipper noise on host param moves).
        depthS_.reset(sr_, 0.03f);
        mixS_.reset(sr_, 0.03f);

        // Rate-derive the DC blockers. They sit on the BASS ROTOR's output,
        // whose band is < 800 Hz by construction, and DcBlocker's default
        // mR = 0.995 is a fixed COEFFICIENT, not a fixed frequency: 38 Hz at
        // 48 kHz but 153 Hz at 192 kHz, which high-passes the Leslie drum.
        dcL_.prepare(sr_);
        dcR_.prepare(sr_);

        dryBuf_.assign(size_t(numCh_), 0.0f);

        setParams(params_);
        reset();
    }

    void reset() {
        hornDelay_.reset();
        drumDelay_.reset();
        drumLp_.reset();
        hornHp_.reset();
        dcL_.reset();
        dcR_.reset();
        hornPhase_ = 0.0;
        drumPhase_ = 1.7;   // start rotors out of phase for a wider image
        // Seed smoothers at their targets so the first block is clean. setTarget
        // alone does NOT do that: SmoothedValue::reset() preserves mCurrent,
        // which is 0 on a first prepare, so the horn would spin up 0 -> 0.83 Hz
        // over a whole second and the drum over 2.4 s. snapToTarget() is what
        // makes the comment above true.
        hornFreq_.setTarget(hornTargetHz_);
        drumFreq_.setTarget(drumTargetHz_);
        depthS_.setTarget(params_.depth);
        mixS_.setTarget(params_.mix);
        hornFreq_.snapToTarget();
        drumFreq_.snapToTarget();
        depthS_.snapToTarget();
        mixS_.snapToTarget();
        std::fill(dryBuf_.begin(), dryBuf_.end(), 0.0f);
    }

    void setParams(const RotaryParams& p) {
        params_.speed = clamp01(p.speed);
        params_.depth = clamp01(p.depth);
        params_.mix   = clamp01(p.mix);

        // Slow/fast rotor speeds (Hz). Horn spins a touch faster than the drum.
        constexpr float kHornSlow = 0.83f, kHornFast = 6.9f;
        constexpr float kDrumSlow = 0.67f, kDrumFast = 5.7f;
        hornTargetHz_ = kHornSlow + params_.speed * (kHornFast - kHornSlow);
        drumTargetHz_ = kDrumSlow + params_.speed * (kDrumFast - kDrumSlow);

        hornFreq_.setTarget(hornTargetHz_);
        drumFreq_.setTarget(drumTargetHz_);
        depthS_.setTarget(params_.depth);
        mixS_.setTarget(params_.mix);
    }

    void process(float* const* ch, int nch, int nframes) {
        if (nch <= 0 || nframes <= 0) return;
        const int nc = (nch < numCh_) ? nch : numCh_;
        const float invNc = 1.0f / float(nc);

        // Tremolo (amplitude) swing and stereo pan width per rotor.
        constexpr float kHornTrem = 0.50f, kDrumTrem = 0.32f;

        const double twoPi = 6.28318530717958647692;

        for (int n = 0; n < nframes; ++n) {
            // --- gather input, form the mono rotor source -------------------
            float mono = 0.0f;
            for (int c = 0; c < nc; ++c) {
                const float in = ch[c][n];
                dryBuf_[size_t(c)] = in;
                mono += in;
            }
            mono *= invNc;
            if (!std::isfinite(mono)) mono = 0.0f;

            // --- crossover: feed each rotor its band ------------------------
            const float low  = drumLp_.process(mono);
            const float high = hornHp_.process(mono);
            drumDelay_.push(low);
            hornDelay_.push(high);

            // --- advance rotor phases (per-sample, inertial frequency) ------
            const float hHz = hornFreq_.next();
            const float dHz = drumFreq_.next();
            hornPhase_ += twoPi * double(hHz) / sr_;
            drumPhase_ += twoPi * double(dHz) / sr_;
            if (hornPhase_ >= twoPi) hornPhase_ -= twoPi;
            if (drumPhase_ >= twoPi) drumPhase_ -= twoPi;

            const float depth = depthS_.next();
            const float mix   = mixS_.next();

            const float hs = std::sin(float(hornPhase_));
            const float ds = std::sin(float(drumPhase_));

            // --- horn rotor: opposite Doppler + tremolo per mic -------------
            float hDelL = hornMid_ - depth * hornMod_ * hs;
            float hDelR = hornMid_ + depth * hornMod_ * hs;
            hDelL = clampDelay(hDelL);
            hDelR = clampDelay(hDelR);
            float hL = hornDelay_.readAt(hDelL);
            float hR = hornDelay_.readAt(hDelR);
            hL *= (1.0f + depth * kHornTrem * hs);
            hR *= (1.0f - depth * kHornTrem * hs);

            // --- drum rotor -------------------------------------------------
            float dDelL = drumMid_ - depth * drumMod_ * ds;
            float dDelR = drumMid_ + depth * drumMod_ * ds;
            dDelL = clampDelay(dDelL);
            dDelR = clampDelay(dDelR);
            float dL = drumDelay_.readAt(dDelL);
            float dR = drumDelay_.readAt(dDelR);
            dL *= (1.0f + depth * kDrumTrem * ds);
            dR *= (1.0f - depth * kDrumTrem * ds);

            float wetL = dcL_.process(hL + dL);
            float wetR = dcR_.process(hR + dR);

            // Hard safety clamp — nothing downstream should ever see garbage.
            wetL = safeClamp(wetL);
            wetR = safeClamp(wetR);

            // --- write back, honouring channel count ------------------------
            if (nc == 1) {
                const float wet = 0.5f * (wetL + wetR);
                ch[0][n] = dryBuf_[0] + mix * (wet - dryBuf_[0]);
            } else {
                ch[0][n] = dryBuf_[0] + mix * (wetL - dryBuf_[0]);
                ch[1][n] = dryBuf_[1] + mix * (wetR - dryBuf_[1]);
                // Any extra channels get the mono wet blend.
                for (int c = 2; c < nc; ++c) {
                    const float wet = 0.5f * (wetL + wetR);
                    ch[c][n] = dryBuf_[size_t(c)] + mix * (wet - dryBuf_[size_t(c)]);
                }
            }
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the smoother's CURRENT value, so a gate case can assert a
    // freshly prepared core starts AT its configured value rather than
    // ramping up to it from 0 (SmoothedValue::reset() preserves mCurrent).
    float hornFreqCurrentForTest() const { return hornFreq_.current(); }
    float mixCurrentForTest()      const { return mixS_.current(); }

private:
    static float clamp01(float v) {
        if (!(v > 0.0f)) return 0.0f;   // also traps NaN
        return (v < 1.0f) ? v : 1.0f;
    }
    // Delay taps must stay strictly positive so interpolation has history.
    static float clampDelay(float d) {
        if (!(d > 1.0f)) return 1.0f;   // also traps NaN
        return d;
    }
    static float safeClamp(float x) {
        if (!std::isfinite(x)) return 0.0f;
        if (x > 24.0f) return 24.0f;
        if (x < -24.0f) return -24.0f;
        return x;
    }

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    int numCh_ = 1;

    RotaryParams params_{};

    // Rotor delay geometry (samples).
    float hornMid_ = 72.0f, hornMod_ = 29.0f;
    float drumMid_ = 125.0f, drumMod_ = 43.0f;
    float hornTargetHz_ = 0.83f, drumTargetHz_ = 0.67f;

    double hornPhase_ = 0.0, drumPhase_ = 1.7;

    InterpolatedDelayLine hornDelay_;
    InterpolatedDelayLine drumDelay_;
    OnePole drumLp_;
    OnePole hornHp_;
    DcBlocker dcL_, dcR_;
    SmoothedValue hornFreq_, drumFreq_;
    SmoothedValue depthS_, mixS_;

    std::vector<float> dryBuf_;
};

} // namespace dcdsp
