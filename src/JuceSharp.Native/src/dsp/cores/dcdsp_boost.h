// dcdsp_boost.h — clean / treble booster (pure, header-only, RT-safe)
//
// The dirt-cheap, hugely-used pedal the board was missing (0.16 W5): up to
// +24 dB of level with a tilt voicing and a transparent-vs-treble-booster
// character control.
//
//   * character = 0 — TRANSPARENT: the signal path is tilt EQ -> gain only.
//     At tilt 0 the shelves are mathematically identity biquads, so a
//     zeroed boost is a bit-exact passthrough and gainDb is a pure level.
//   * character > 0 — TREBLE BOOSTER (Rangemaster-style): an input high-pass
//     sweeps up from 20 Hz to ~700 Hz (thinning the lows so the boost stays
//     tight into an amp) and a gentle first-order-ADAA tanh stage adds the
//     germanium-ish warmth once the boosted signal runs hot. The saturation
//     drive scales with character; at low input levels it stays essentially
//     linear (unity-gain-compensated).
//
//   * tilt (-1..1) — a see-saw EQ: low shelf at 250 Hz gets -tilt*6 dB while
//     the high shelf at 2.4 kHz gets +tilt*6 dB (tilt +1 = brighter).
//
// Signal path: [character HPF] -> gain -> [character tanh] -> tilt shelves.
// Gain sits BEFORE the saturation so cranking the boost drives the character
// stage exactly like a real booster drives the next pedal.
//
// Contract shape (see dsp-core-contract.md):
//   struct BoostParams { ... };
//   class  BoostCore  { prepare / reset / setParams / process / latencySamples };
//
// Real-time safety: allocation only in prepare(); process() never allocates,
// locks, or throws. Output is sanitized (finite, bounded) per sample.
//
// Easy to get wrong, and shipped wrong: whether the character branch RUNS was
// decided ONCE PER BLOCK from `satSmooth_.isRamping()`, while both smoothers
// walk per FRAME. The ramp therefore ended at the same absolute sample in every
// chunking but the branch only noticed at the next block BOUNDARY, so the same
// render came out differently depending on the host's buffer size (measured:
// chunk 1 against chunk 1024 nulled to only -45.9 dBFS). The decision is now
// taken per frame, where the quantity it reads already lives.
// Pinned by GearDriveTests.Boost_ProducesTheSameOutput_WhateverChunkTheHostHandsIt.

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

struct BoostParams
{
    float gainDb    = 6.0f;  // [0..24]  boost amount
    float tilt      = 0.0f;  // [-1..1]  dark <-> bright see-saw (0 = flat)
    float character = 0.0f;  // [0..1]   transparent <-> treble booster
};

class BoostCore
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels)
    {
        (void)maxBlockSize;
        sr_ = (sampleRate > 0.0) ? sampleRate : 44100.0;
        ch_.assign(static_cast<std::size_t>(numChannels > 0 ? numChannels : 1), Channel{});
        gainSmooth_.reset(sr_, 0.01f);
        // The 1/drive make-up keeps small signals at unity, so this step is
        // inaudible quiet and audible once the boost runs hot -- which is the
        // setting the knob exists for.
        satSmooth_.reset(sr_, 0.01f);
        // setParams FIRST, then snap: a freshly prepared core must start AT its
        // configured value. Snapping before it lands on whatever the previous
        // configuration happened to leave behind.
        setParams(params_);
        gainSmooth_.snapToTarget();
        satSmooth_.snapToTarget();
        reset();
    }

    void reset()
    {
        for (auto& c : ch_)
        {
            c.hpf.reset();
            c.low.reset();
            c.high.reset();
            c.satState = 0.0f;
        }
    }

    void setParams(const BoostParams& p)
    {
        params_ = p;
        const float gainDb = clampf(p.gainDb, 0.0f, 24.0f);
        const float tilt   = clampf(p.tilt, -1.0f, 1.0f);
        character_ = clampf(p.character, 0.0f, 1.0f);

        gainSmooth_.setTarget(std::pow(10.0f, gainDb / 20.0f));

        // Treble-booster input thinning: 20 Hz (inaudible) .. ~700 Hz.
        hpfHz_ = 20.0f + character_ * 680.0f;
        // Gentle warmth: drive grows with character; the 1/drive make-up keeps
        // small signals at unity so character never doubles as a volume knob.
        satDrive_ = 1.0f + 2.0f * character_;
        satSmooth_.setTarget(satDrive_);
        satActive_ = character_ > 1e-3f;

        for (auto& c : ch_)
        {
            c.hpf.setHighpass(sr_, hpfHz_);
            c.low.setLowShelf(sr_, 250.0, 0.707, -tilt * 6.0);
            c.high.setHighShelf(sr_, 2400.0, 0.707, tilt * 6.0);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames)
    {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0)
            return;
        const int nch = std::min(numChannels, (int) ch_.size());
        for (int n = 0; n < numFrames; ++n)
        {
            // Per FRAME, not per block. `isRamping()` flips at one absolute
            // sample whatever the host's buffer size is; reading it once per
            // block made that boundary depend on the chunking instead.
            const bool sat = satActive_ || satSmooth_.isRamping();
            const float g = gainSmooth_.next();
            // Walked per frame: the loop is sample-outer, so this composes.
            const float drive    = satSmooth_.next();
            const float invDrive = 1.0f / drive;
            for (int c = 0; c < nch; ++c)
            {
                float* x = channels[c];
                if (x == nullptr) continue;
                Channel& st = ch_[static_cast<std::size_t>(c)];

                float s = x[n];
                if (sat)
                {
                    s = st.hpf.process(s);
                    s *= g;
                    // ADAA tanh, unity-compensated: linear for small s,
                    // germanium-ish squash as the boost runs hot.
                    s = adaaTanh(s * drive, st.satState) * invDrive;
                }
                else
                {
                    s *= g;
                }
                s = st.low.process(s);
                s = st.high.process(s);

                if (!std::isfinite(s)) s = 0.0f;
                s = clampf(s, -kOutClamp, kOutClamp);
                x[n] = s;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    static constexpr float kOutClamp = 16.0f;

    static float clampf(float v, float lo, float hi)
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    struct Channel
    {
        OnePole hpf;        // character input thinning
        Biquad  low, high;  // tilt see-saw
        float   satState = 0.0f;
    };

    double sr_ = 44100.0;
    BoostParams params_{};
    float character_ = 0.0f;
    float hpfHz_ = 20.0f;
    float satDrive_ = 1.0f;
    bool  satActive_ = false;
    SmoothedValue gainSmooth_;
    SmoothedValue satSmooth_;   // satDrive_ stays the TARGET
    std::vector<Channel> ch_;
};

} // namespace dcdsp
