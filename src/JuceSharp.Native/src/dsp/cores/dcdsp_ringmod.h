// dcdsp_ringmod.h — Ring modulator DSP core (header-only, pure, RT-safe).
//
// Multiplies the input by an internally generated carrier oscillator and mixes
// the modulated (wet) signal against the dry input. Inherently bounded: the
// carrier is in [-1,1] and mix in [0,1], so |out| <= |in|. Every path is still
// guarded (params clamped, values smoothed) so extreme/abrupt settings stay
// stable and click-free.
//
// Namespace dcdsp. C++17. Deps: only the shared primitives header.
//
// Codes against dcdsp_primitives.h (Lfo, SmoothedValue). For the standalone
// compile-test a minimal local stub is substituted via the guard below; the
// production build always pulls in the real shared header.
#pragma once

#if defined(DCDSP_RINGMOD_TEST_STUBS)
    #include "test_stubs.h"
#else
    #include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dcdsp {

struct RingModParams {
    // Carrier frequency in Hz. Range 20 .. 3000 (clamped internally).
    float freqHz = 500.0f;
    // Dry/wet blend. 0 = fully dry (bypass-ish), 1 = fully modulated. Range 0 .. 1.
    float mix = 1.0f;
    // Carrier waveform: 0 = sine, 1 = triangle. Any other value treated as sine.
    int waveform = 0;
};

class RingModCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int /*numChannels*/) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        carrier_.prepare(sr_);
        carrier_.setShape(Lfo::Sine);
        // Waveform switches CROSSFADE (kShapeFadeSeconds). setShape() swaps the
        // waveform under a running oscillator: at a given phase sine and
        // triangle differ by up to 1.0, so a switch put a full-scale step into
        // the carrier -- i.e. a click in the wet, loudest at mix = 1.
        shapeCur_ = shapePrev_ = Lfo::Sine;
        shapeFade_.reset(sr_, kShapeFadeSeconds);
        shapeFade_.setCurrentAndTarget(1.0f);
        // ~5 ms parameter ramps: fast enough to feel instant, slow enough to
        // suppress zipper noise on freq/mix changes.
        freqSmooth_.reset(sr_, 0.005f);
        mixSmooth_.reset(sr_, 0.005f);
        // Snap smoothers to the current targets so prepare() starts settled.
        freqSmooth_.setTarget(clampFreq(params_.freqHz));
        mixSmooth_.setTarget(clampMix(params_.mix));
        snapSmoothers();
        applyShape();
        shapeFade_.snapToTarget();   // prepare starts AT its waveform, never fading in
        prepared_ = true;
        firstParams_ = true;
    }

    void reset() {
        carrier_.reset();
        // Re-snap so a reset does not introduce a ramp from a stale value, and
        // treat the next setParams as an initial (non-ramped) load.
        if (prepared_) {
            freqSmooth_.setTarget(clampFreq(params_.freqHz));
            mixSmooth_.setTarget(clampMix(params_.mix));
            snapSmoothers();
            firstParams_ = true;
        }
    }

    void setParams(const RingModParams& p) {
        params_ = p;
        const float f = clampFreq(p.freqHz);
        const float m = clampMix(p.mix);
        freqSmooth_.setTarget(f);
        mixSmooth_.setTarget(m);
        applyShape();
        // The first param set after prepare/reset snaps (no audible ramp from a
        // stale default); later changes ramp smoothly to avoid zipper noise.
        if (firstParams_) {
            snapSmoothers();
            firstParams_ = false;
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (numChannels <= 0 || numFrames <= 0 || channels == nullptr)
            return;

        for (int n = 0; n < numFrames; ++n) {
            // One carrier sample per frame, shared across all channels so
            // multi-channel material stays phase-coherent.
            const float f = freqSmooth_.next();
            carrier_.setFrequency(f);
            const float t = shapeFade_.next();
            float c = (t >= 1.0f)
                        ? carrier_.peek(shapeCur_)
                        : (1.0f - t) * carrier_.peek(shapePrev_)
                              + t * carrier_.peek(shapeCur_);
            carrier_.advance();
            // Guard the carrier hard to [-1,1] regardless of primitive shape.
            if (!(std::isfinite(c))) c = 0.0f;
            c = std::min(1.0f, std::max(-1.0f, c));

            const float mix = mixSmooth_.next();      // already in [0,1]
            const float dryGain = 1.0f - mix;

            for (int ch = 0; ch < numChannels; ++ch) {
                float* buf = channels[ch];
                if (buf == nullptr) continue;
                const float in = buf[n];
                const float wet = in * c;             // ring modulation
                float out = dryGain * in + mix * wet;
                // Final safety net: never emit NaN/Inf. Output is naturally
                // bounded by |in| but clamp anyway for absolute stability.
                if (!(std::isfinite(out))) out = 0.0f;
                buf[n] = out;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    static float clampFreq(float f) {
        if (!(std::isfinite(f))) return 20.0f;
        return std::min(3000.0f, std::max(20.0f, f));
    }
    static float clampMix(float m) {
        if (!(std::isfinite(m))) return 0.0f;
        return std::min(1.0f, std::max(0.0f, m));
    }

    void applyShape() {
        const Lfo::Shape want = (params_.waveform == 1) ? Lfo::Tri : Lfo::Sine;
        carrier_.setShape(want);          // keep the oscillator's own state honest
        if (want == shapeCur_) return;
        // A SECOND switch inside the 10 ms window used to discard the blend in
        // flight: the output went from (1-t)*prev + t*cur to 1.0*cur in one
        // sample -- up to ~0.7 full scale, which is the click this crossfade
        // exists to remove. Flipping BACK is the common case (a footswitch
        // bounce, a preset A/B), and there the two ends simply swap and the
        // blend keeps its position.
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

    void snapSmoothers() {
        // Drive each smoother to its target immediately (no audible ramp).
        // Iterate past the full ramp length so a linear ramp lands exactly.
        const int steps = static_cast<int>(sr_ * 0.02) + 4;
        for (int i = 0; i < steps; ++i) { freqSmooth_.next(); mixSmooth_.next(); }
        shapeFade_.snapToTarget();   // a snap must land the shape too
    }

    double sr_ = 48000.0;
    bool prepared_ = false;
    bool firstParams_ = true;
    RingModParams params_{};
    Lfo carrier_{};
    SmoothedValue freqSmooth_{};
    SmoothedValue mixSmooth_{};
    static constexpr float kShapeFadeSeconds = 0.010f;
    Lfo::Shape    shapeCur_  = Lfo::Sine;
    Lfo::Shape    shapePrev_ = Lfo::Sine;
    SmoothedValue shapeFade_{};
};

} // namespace dcdsp
