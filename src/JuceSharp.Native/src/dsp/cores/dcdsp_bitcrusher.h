// dcdsp_bitcrusher.h — pure, RT-safe bit crusher core.
//
// Two classic lo-fi degradations combined:
//   1. Amplitude quantization to N bits (bit-depth reduction).
//   2. Sample-and-hold decimation by an integer factor (sample-rate reduction).
// Followed by a wet/dry blend.
//
// The crushed (wet) path also has a level/drive/lo-fi section (DotCharter
// #157), laid out like McDSP's FutzBox, whose manual publishes the section
// order and the Lo-Fi controls ("DownSample - reduces sample rate", "Filter -
// reduces some aliasing artifacts created by downsampling", "Bits - selects
// bit depth"): Input gain -> Distortion (Amount, Intensity, Rectify) -> Lo-Fi
// (DownSample, Filter, Bits) -> Output gain, with Mix the wet/dry balance.
// McDSP publishes no laws, so these are this core's own: the distortion is the
// shaper dcdsp_rsamp.h runs for a FutzBox block (Amount 0..100 % = 0..+50 dB of
// gain into a tanh / hard-clip blend, Intensity the blend, Rectify scaling the
// negative half), kept identical so the two agree; the Filter is a 2-pole
// low-pass whose corner moves, in log frequency, from 0.45 x the rate (0 %, no
// effect) to the held signal's own Nyquist (100 %, a full anti-alias filter).
//
// Wet path, per sample:
//   x * inputDb -> [drive > 0: gain, rectify, tanh/clip blend, DC block]
//   -> sample-and-hold by `downsample` -> [filter > 0: low-pass]
//   -> quantize to `bits` -> * outputDb
// With every added param at its default (0 dB, 0 %) the wet path is exactly the
// classic crusher: quantize-then-hold equals hold-then-quantize.
//
// Contract: namespace dcdsp, C++17, no I/O, no JUCE. Allocation happens ONLY in
// prepare(); process() never allocates, locks, or throws. The output is bounded:
// the shaper is bounded by 1, quantization by |input| + half a step, the
// low-pass by its own (Butterworth, no overshoot past a few %) response.
//
// Depends on the shared primitives header for SmoothedValue (de-zipping the mix
// and the gains), Biquad and DcBlocker. Production builds include the real
// dcdsp_primitives.h; the standalone compile-test defines
// DCDSP_BITCRUSHER_TEST_STUBS to pull in a minimal local stub instead.
#pragma once

#if defined(DCDSP_BITCRUSHER_TEST_STUBS)
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dcdsp {

struct BitCrusherParams {
    // Effective amplitude resolution in bits. 1 = maximally crushed, 16 = clean.
    // Range [1, 16]; clamped internally.
    int bits = 16;
    // Sample-and-hold decimation factor (sample-rate reduction). 1 = no
    // decimation, N holds each captured sample for N frames. Range [1, 64];
    // clamped internally.
    int downsample = 1;
    // Wet/dry blend. 0 = fully dry (bypass sound), 1 = fully crushed.
    // Range [0, 1]; clamped internally. Smoothed to avoid zipper noise.
    float mix = 1.0f;
    // Gain into the wet path, dB. Range [-24, 48]. Ramped.
    float inputDb = 0.0f;
    // Distortion Amount, %: 0 = no distortion stage at all; above it,
    // kDriveDbPerPct dB of gain per % into the shaper. Range [0, 100]. Ramped.
    float drive = 0.0f;
    // Distortion character, %: 0 = soft (tanh), 100 = bright (hard clip).
    // Range [0, 100]. Ramped.
    float intensity = 0.0f;
    // Negative half-wave scaled by (1 - rectify / 100) before the shaper.
    // Range [0, 100]. Ramped.
    float rectify = 0.0f;
    // Lo-Fi anti-alias low-pass amount, %: 0 = off. Range [0, 100].
    float filter = 0.0f;
    // Level of the wet path, dB. Range [-48, 24]. Ramped.
    float outputDb = 0.0f;
};

class BitCrusherCore {
public:
    // Distortion Amount: dB of gain per % (the same law as dcdsp_rsamp.h's kFutzAmountDbPct).
    static constexpr float kDriveDbPerPct = 0.5f;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_   = maxBlockSize > 0 ? maxBlockSize : 0;
        numCh_      = numChannels > 0 ? numChannels : 0;

        held_.assign(static_cast<std::size_t>(numCh_), 0.0f);
        phase_.assign(static_cast<std::size_t>(numCh_), 0);
        lp_.assign(static_cast<std::size_t>(numCh_), Biquad{});
        dc_.assign(static_cast<std::size_t>(numCh_), DcBlocker{});
        for (auto& d : dc_) d.prepare(sampleRate_, 10.0);

        mixSmooth_.reset(sampleRate_, 0.01f); // 10 ms ramp
        inSmooth_.reset(sampleRate_, 0.01f);
        outSmooth_.reset(sampleRate_, 0.01f);
        driveSmooth_.reset(sampleRate_, 0.01f);
        intensitySmooth_.reset(sampleRate_, 0.01f);
        rectifySmooth_.reset(sampleRate_, 0.01f);

        lastFilterPct_ = -1.0f; // force the low-pass redesign at the new rate
        recomputeStep();
        recomputeFilter();
        setTargets();
        reset();
        // Start AT the configured values. reset() preserves mCurrent, which is 0
        // on a first prepare, so without this the first 10 ms ramps from 0.
        snapAll();
    }

    void reset() {
        std::fill(held_.begin(), held_.end(), 0.0f);
        std::fill(phase_.begin(), phase_.end(), 0);
        for (auto& f : lp_) f.reset();
        for (auto& d : dc_) d.reset();
        setTargets();
    }

    void setParams(const BitCrusherParams& p) {
        params_.bits       = clampi(p.bits, 1, 16);
        params_.downsample = clampi(p.downsample, 1, 64);
        params_.mix        = clampf(p.mix, 0.0f, 1.0f);
        params_.inputDb    = clampf(p.inputDb, -24.0f, 48.0f);
        params_.drive      = clampf(p.drive, 0.0f, 100.0f);
        params_.intensity  = clampf(p.intensity, 0.0f, 100.0f);
        params_.rectify    = clampf(p.rectify, 0.0f, 100.0f);
        params_.filter     = clampf(p.filter, 0.0f, 100.0f);
        params_.outputDb   = clampf(p.outputDb, -48.0f, 24.0f);
        recomputeStep();
        recomputeFilter();
        setTargets();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;

        const int step   = params_.downsample;
        const float qstep = quantStep_;      // amplitude quantization step
        const float invQ  = invQuantStep_;   // 1 / qstep
        const bool shaped = params_.drive > 0.0f;
        const bool filtered = filterOn_;

        // Mix and the gains are per-frame scalars shared across channels: each
        // smoother advances once per frame and every channel reads that value.
        const int chCount = std::min(numChannels, numCh_);

        for (int f = 0; f < numFrames; ++f) {
            const float wet  = mixSmooth_.next();
            const float dry  = 1.0f - wet;
            const float gIn  = inSmooth_.next();
            const float gOut = outSmooth_.next();
            const float gDrv = driveSmooth_.next();
            const float inten = intensitySmooth_.next();
            const float rect  = rectifySmooth_.next();

            for (int c = 0; c < chCount; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                const auto ci = static_cast<std::size_t>(c);

                const float x = buf[f];
                float v = std::isfinite(x) ? x * gIn : 0.0f;

                if (shaped) {
                    v *= gDrv;
                    if (v < 0.0f) v *= 1.0f - rect;
                    const float clip = std::clamp(v, -1.0f, 1.0f);
                    v = (1.0f - inten) * std::tanh(v) + inten * clip;
                    v = dc_[ci].process(v);
                }

                // Sample-and-hold decimation: capture a fresh sample when the
                // per-channel phase counter reaches zero, else hold.
                int& ph = phase_[ci];
                if (ph <= 0) {
                    held_[ci] = v;
                    ph = step;
                }
                --ph;

                float w = held_[ci];
                if (filtered) w = lp_[ci].process(w);
                const float wetSample = quantize(w, qstep, invQ) * gOut;

                float y = dry * x + wet * wetSample;

                // Belt-and-braces bound: guard against non-finite input or a
                // filter state propagating.
                if (!std::isfinite(y)) {
                    y = 0.0f;
                    lp_[ci].reset();
                    dc_[ci].reset();
                }
                buf[f] = y;
            }
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the smoother's CURRENT value, so a gate case can assert a
    // freshly prepared core starts AT its configured value rather than
    // ramping up to it from 0 (SmoothedValue::reset() preserves mCurrent).
    float mixCurrentForTest() const { return mixSmooth_.current(); }
    // Test-only: the low-pass corner the Filter param resolved to (0 = off).
    double filterHzForTest() const { return filterOn_ ? filterHz_ : 0.0; }

private:
    static int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    // NaN-safe: a NaN fails both comparisons, so it lands on `lo`.
    static float clampf(float v, float lo, float hi) { return v >= lo ? (v > hi ? hi : v) : lo; }
    static float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

    void setTargets() {
        mixSmooth_.setTarget(params_.mix);
        inSmooth_.setTarget(dbToLin(params_.inputDb));
        outSmooth_.setTarget(dbToLin(params_.outputDb));
        driveSmooth_.setTarget(dbToLin(params_.drive * kDriveDbPerPct));
        intensitySmooth_.setTarget(params_.intensity / 100.0f);
        rectifySmooth_.setTarget(params_.rectify / 100.0f);
    }

    void snapAll() {
        mixSmooth_.snapToTarget();
        inSmooth_.snapToTarget();
        outSmooth_.snapToTarget();
        driveSmooth_.snapToTarget();
        intensitySmooth_.snapToTarget();
        rectifySmooth_.snapToTarget();
    }

    void recomputeStep() {
        // levels = 2^bits over the nominal [-1, 1] range.
        const float levels = std::ldexp(1.0f, params_.bits); // 2^bits
        quantStep_    = 2.0f / levels;
        invQuantStep_ = 1.0f / quantStep_;
    }

    // The Lo-Fi Filter: a Butterworth low-pass whose corner runs, log-linearly
    // in the param, from 0.45 x the rate (0 %) to the held signal's Nyquist,
    // rate / (2 x downsample) (100 %). Redesigned only when its inputs change.
    void recomputeFilter() {
        filterOn_ = params_.filter > 0.0f;
        if (!filterOn_) { lastFilterPct_ = -1.0f; return; }
        if (params_.filter == lastFilterPct_ && params_.downsample == lastFilterDown_) return;
        lastFilterPct_ = params_.filter;
        lastFilterDown_ = params_.downsample;
        const double top = 0.45 * sampleRate_;
        const double held = 0.5 * sampleRate_ / static_cast<double>(params_.downsample);
        const double t = static_cast<double>(params_.filter) / 100.0;
        filterHz_ = std::exp(std::log(top) + t * (std::log(std::min(held, top)) - std::log(top)));
        for (auto& f : lp_) f.setLowpass(sampleRate_, filterHz_, 0.7071067811865476, true);
    }

    // Mid-tread quantization. Bounded: |round(x*invQ)*qstep - x| <= qstep/2, so
    // the output magnitude never exceeds |x| + qstep/2.
    static float quantize(float x, float qstep, float invQ) {
        if (!std::isfinite(x)) return 0.0f;
        const float q = std::floor(x * invQ + 0.5f);
        return q * qstep;
    }

    double sampleRate_ = 48000.0;
    int    maxBlock_   = 0;
    int    numCh_      = 0;

    BitCrusherParams params_{};
    float quantStep_    = 2.0f / 65536.0f;
    float invQuantStep_ = 65536.0f / 2.0f;

    bool   filterOn_ = false;
    double filterHz_ = 0.0;
    float  lastFilterPct_ = -1.0f;
    int    lastFilterDown_ = 0;

    std::vector<float>     held_;   // last captured wet sample, per channel
    std::vector<int>       phase_;  // frames remaining before next capture, per channel
    std::vector<Biquad>    lp_;     // the Lo-Fi filter, per channel
    std::vector<DcBlocker> dc_;     // after the (rectifying) shaper, per channel

    SmoothedValue mixSmooth_{};
    SmoothedValue inSmooth_{};
    SmoothedValue outSmooth_{};
    SmoothedValue driveSmooth_{};
    SmoothedValue intensitySmooth_{};
    SmoothedValue rectifySmooth_{};
};

} // namespace dcdsp
