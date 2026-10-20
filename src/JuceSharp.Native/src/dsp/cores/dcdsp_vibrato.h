// dcdsp_vibrato.h — pure header-only vibrato core.
//
// Vibrato = pitch modulation only. A single InterpolatedDelayLine tap whose
// read position is swept by an LFO produces frequency modulation; the output is
// 100% wet (the delayed copy only, no dry blend). That "no dry" is exactly what
// separates vibrato from chorus/flanger (which mix dry + modulated wet and thus
// comb-filter). Amplitude is preserved because a sine read at a time-varying
// phase is still a sine of the same amplitude.
//
// RT-safe: allocation only in prepare(); process() never allocates/locks/throws.
// C++17, deps only <cmath> <vector> <array> <algorithm> <cstddef> <cstdint>
// (transitively via the primitives header).

#pragma once

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

// Production builds include the real shared primitives; the standalone
// compile-test defines DCDSP_USE_TEST_STUBS to pull in a minimal local subset.
#if defined(DCDSP_USE_TEST_STUBS)
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

namespace dcdsp {

// All ranges are clamped inside setParams() so out-of-range host values are safe.
struct VibratoParams {
    float rateHz   = 5.0f;  // LFO speed,           range 0.1 .. 10 Hz
    float depthMs  = 3.0f;  // peak pitch sweep,     range 0.5 .. 12 ms
    int   waveform = 0;     // 0 = sine, 1 = triangle
};

class VibratoCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        (void)maxBlockSize;
        sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numChannels_ = std::max(1, numChannels);

        // Worst-case delay = guard + centre + swept depth. Centre already
        // includes one depth so the modulated read never underflows the guard.
        // Size for the maximum documented depth (12 ms) plus generous headroom
        // so setParams() can never ask for a read past the buffer.
        const double maxDepthSamples = kMaxDepthMs * 0.001 * sampleRate_;
        const int maxDelay = static_cast<int>(
            std::ceil(kGuardSamples + 2.0 * maxDepthSamples)) + 4;

        lines_.resize(static_cast<std::size_t>(numChannels_));
        for (auto& l : lines_)
            l.prepare(maxDelay);
        maxDelaySamples_ = static_cast<float>(maxDelay);

        lfo_.prepare(sampleRate_);
        depthSmooth_.reset(sampleRate_, 0.02f); // 20 ms glide, no zipper on depth
        // Waveform switches CROSSFADE (kShapeFadeSeconds). setShape() swapped the
        // waveform under the running LFO, and at a given phase sine and triangle
        // differ by up to 1.0 -- so `delay` jumped by the WHOLE depth in one
        // sample (576 samples, 12 ms, at the maximum). The read head teleporting
        // 12 ms mid-note is the loudest discontinuity this core can make.
        shapeCur_ = shapePrev_ = Lfo::Sine;
        shapeFade_.reset(sampleRate_, kShapeFadeSeconds);
        shapeFade_.setCurrentAndTarget(1.0f);

        setParams(params_);
        shapeFade_.snapToTarget();   // prepare starts AT its waveform
        reset();
    }

    void reset() {
        for (auto& l : lines_)
            l.reset();
        lfo_.reset();
        // SNAP, not eight steps of a 20 ms ramp. `reset()` re-arms the ramp
        // (setTarget sets mRemaining = 0.02*sr = 960 steps at 48 kHz), so eight
        // next() calls covered 0.8 % of it and the delay then swept from the
        // 4-sample guard up to the full depth over 20 ms — a downward pitch
        // swoop of ~2.4 semitones at the default depth, ~8 at the maximum, on
        // every prepare/reset (device change, chain rebuild).
        depthSmooth_.setCurrentAndTarget(currentDepthSamples());
        shapeFade_.snapToTarget();   // a reset lands the waveform too
    }

    void setParams(const VibratoParams& p) {
        params_.rateHz   = clampf(p.rateHz, 0.1f, 10.0f);
        params_.depthMs  = clampf(p.depthMs, 0.5f, kMaxDepthMs);
        params_.waveform = (p.waveform == 1) ? 1 : 0;

        lfo_.setFrequency(params_.rateHz);
        const Lfo::Shape want = (params_.waveform == 1) ? Lfo::Tri : Lfo::Sine;
        lfo_.setShape(want);              // keep the oscillator's own state honest
        if (want != shapeCur_) {
            // Re-entrant, for the reason spelled out in dcdsp_ringmod.h's
            // applyShape(): discarding a blend in flight is itself a step of up
            // to the full shape difference, and here that step is a DELAY TAP.
            if (want == shapePrev_ && shapeFade_.isRamping()) {
                const float t = shapeFade_.current();
                shapePrev_ = shapeCur_;
                shapeCur_  = want;
                shapeFade_.setCurrentAndTarget(1.0f - t);
            } else {
                shapePrev_ = shapeCur_;
                shapeCur_  = want;
                shapeFade_.setCurrentAndTarget(0.0f);
            }
            shapeFade_.setTarget(1.0f);
        }
        depthSmooth_.setTarget(currentDepthSamples());
    }

    // In-place, 100% wet.
    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        const int nch = std::min(numChannels, numChannels_);

        for (int n = 0; n < numFrames; ++n) {
            // One shared LFO -> identical pitch sweep on every channel (mono mod).
            const float t   = shapeFade_.next();
            const float raw  = (t >= 1.0f)
                                 ? lfo_.peek(shapeCur_)
                                 : (1.0f - t) * lfo_.peek(shapePrev_)
                                       + t * lfo_.peek(shapeCur_);
            lfo_.advance();
            const float lfo = clampf(raw, -1.0f, 1.0f);
            const float depth = depthSmooth_.next();       // samples
            const float centre = kGuardSamples + depth;    // >= guard always
            // delay in [guard, guard + 2*depth]; clamp for absolute safety.
            float delay = centre + depth * lfo;
            delay = clampf(delay, 1.0f, maxDelaySamples_ - 2.0f);

            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                const float x = buf[n];
                auto& line = lines_[static_cast<std::size_t>(c)];
                line.push(x);
                float y = line.readAt(delay);
                if (!std::isfinite(y)) y = 0.0f; // hard guard on the wet tap
                buf[n] = y;
            }
        }
    }

    int latencySamples() const {
        // Report the nominal (centre) tap so downstream latency comp is honest.
        return static_cast<int>(std::lround(kGuardSamples + currentDepthSamples()));
    }

private:
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    float currentDepthSamples() const {
        return params_.depthMs * 0.001f * static_cast<float>(sampleRate_);
    }

    static constexpr float kMaxDepthMs   = 12.0f;
    static constexpr float kGuardSamples = 4.0f; // keeps the interpolated read off the buffer edge

    double sampleRate_ = 48000.0;
    int    numChannels_ = 1;
    float  maxDelaySamples_ = 0.0f;

    VibratoParams params_{};
    std::vector<InterpolatedDelayLine> lines_;
    Lfo lfo_;
    SmoothedValue depthSmooth_;
    static constexpr float kShapeFadeSeconds = 0.010f;
    Lfo::Shape    shapeCur_  = Lfo::Sine;
    Lfo::Shape    shapePrev_ = Lfo::Sine;
    SmoothedValue shapeFade_;
};

} // namespace dcdsp
