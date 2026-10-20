// dcdsp_dimension.h — Dimension-C-style dual chorus (pure, header-only, RT-safe)
//
// The lush 80s / city-pop "dimension" effect (Roland SDD-320 / Boss DC-2,
// 0.16 W5): TWO short modulated delay taps per channel driven by the SAME
// sine LFO in OPPOSITE phase (tapA = baseA + d*lfo, tapB = baseB - d*lfo,
// with slightly different base delays). Because the two taps move against
// each other, their pitch deviations largely cancel in the sum — you get the
// shimmering widening without the seasick vibrato of a regular chorus, which
// is exactly what the generic dcdsp_chorus cannot do. Stereo channels weight
// the two taps opposite ways (L leans on tapA, R on tapB), recreating the
// original's crossed dual-BBD output stage; mono sums them equally.
//
// The wet path is band-limited like the BBD original: ~150 Hz high-pass
// (keeps the low end mono/solid) and ~7.5 kHz low-pass.
//
// The four hardware buttons map to `mode` 0..3 — a rate/depth table that
// grows both together, matching the "1 subtle .. 4 wide" semantics.
//
// Contract shape (see dsp-core-contract.md):
//   struct DimensionParams { ... };
//   class  DimensionCore  { prepare / reset / setParams / process / latencySamples };
//
// Real-time safety: allocation only in prepare(); process() never allocates,
// locks, or throws. Depth changes are ramped (~15 ms) so mode switches don't
// click; output is finite-guarded and bounded.

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

struct DimensionParams
{
    int   mode = 1;   // [0..3] button 1..4: subtle .. wide (rate+depth table)
    float mix  = 0.5f; // [0..1] dry/wet (0.5 = the fixed 50/50 of the hardware)
};

class DimensionCore
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels)
    {
        (void)maxBlockSize;
        sr_ = (sampleRate > 0.0) ? sampleRate : 44100.0;
        numCh_ = numChannels < 1 ? 1 : numChannels;

        const double maxDelayMs = kBaseBMs + kMaxDepthMs + 2.0;
        const int maxSamps = (int) std::ceil(maxDelayMs * 0.001 * sr_) + 4;

        ch_.assign((size_t) numCh_, Channel{});
        for (auto& c : ch_)
        {
            c.line.prepare(maxSamps);
            c.wetHp.setHighpass(sr_, 150.0);
            c.wetLp.setLowpass(sr_, 7500.0);
        }

        lfo_.prepare(sr_);
        lfo_.setShape(Lfo::Sine);

        smDepth_.reset(sr_, 0.015f);
        smMix_.reset(sr_, 0.015f);
        setParams(params_);
        smDepth_.setCurrentAndTarget((float) (kModes[modeIndex()].depthMs * 0.001 * sr_));
        smMix_.setCurrentAndTarget(clamp01(params_.mix));

        reset();
    }

    void reset()
    {
        for (auto& c : ch_)
        {
            c.line.reset();
            c.wetHp.reset();
            c.wetLp.reset();
        }
        lfo_.reset();
        smDepth_.setCurrentAndTarget((float) (kModes[modeIndex()].depthMs * 0.001 * sr_));
        smMix_.setCurrentAndTarget(clamp01(params_.mix));
    }

    void setParams(const DimensionParams& p)
    {
        params_ = p;
        const Mode& m = kModes[modeIndex()];
        lfo_.setFrequency(m.rateHz);
        smDepth_.setTarget((float) (m.depthMs * 0.001 * sr_));
        smMix_.setTarget(clamp01(p.mix));
    }

    void process(float* const* channels, int numChannels, int numFrames)
    {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0)
            return;
        const int nch = numChannels < numCh_ ? numChannels : numCh_;
        const float baseA = (float) (kBaseAMs * 0.001 * sr_);
        const float baseB = (float) (kBaseBMs * 0.001 * sr_);
        const float minTap = (float) (kMinTapMs * 0.001 * sr_);
        const bool stereo = (nch >= 2);

        for (int n = 0; n < numFrames; ++n)
        {
            const float lv    = lfo_.process();      // shared, so L/R stay coherent
            const float depth = smDepth_.next();
            const float wet   = smMix_.next();
            const float dry   = 1.0f - wet;

            // Opposite-phase taps; clamp so the sweep can never underflow the
            // interpolator headroom.
            float dA = baseA + depth * lv;
            float dB = baseB - depth * lv;
            if (dA < minTap) dA = minTap;
            if (dB < minTap) dB = minTap;

            for (int c = 0; c < nch; ++c)
            {
                float* x = channels[c];
                if (x == nullptr) continue;
                Channel& st = ch_[(size_t) c];

                const float in = x[n];
                const float a = st.line.readAt(dA);
                const float b = st.line.readAt(dB);
                st.line.push(in);

                // Crossed output stage: L leans on tapA, R on tapB (mono 50/50).
                float w;
                if (!stereo)          w = 0.5f * (a + b);
                else if ((c & 1) == 0) w = kNearW * a + kFarW * b;
                else                   w = kFarW * a + kNearW * b;

                // BBD-ish wet band-limit.
                w = st.wetLp.process(st.wetHp.process(w));

                float out = dry * in + wet * w;
                if (!std::isfinite(out)) out = 0.0f;
                out = clampf(out, -kOutClamp, kOutClamp);
                x[n] = out;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    struct Mode { float rateHz; double depthMs; };
    // Buttons 1..4: rate and depth grow together (subtle .. wide).
    static constexpr Mode kModes[4] = {
        { 0.50f, 0.25 }, { 0.75f, 0.40 }, { 1.05f, 0.55 }, { 1.40f, 0.75 },
    };

    static constexpr double kBaseAMs   = 2.6;  // tap A centre delay
    static constexpr double kBaseBMs   = 3.4;  // tap B centre delay (decorrelates)
    static constexpr double kMaxDepthMs = 0.75;
    static constexpr double kMinTapMs  = 1.0;  // interpolator headroom
    static constexpr float  kNearW     = 0.65f; // crossed output weights
    static constexpr float  kFarW      = 0.35f;
    static constexpr float  kOutClamp  = 16.0f;

    int modeIndex() const
    {
        int m = params_.mode;
        return m < 0 ? 0 : (m > 3 ? 3 : m);
    }
    static float clampf(float v, float lo, float hi)
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }
    static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

    struct Channel
    {
        InterpolatedDelayLine line;
        OnePole wetHp, wetLp;
    };

    double sr_ = 44100.0;
    int numCh_ = 2;
    DimensionParams params_{};
    std::vector<Channel> ch_;
    Lfo lfo_;
    SmoothedValue smDepth_, smMix_;
};

} // namespace dcdsp
