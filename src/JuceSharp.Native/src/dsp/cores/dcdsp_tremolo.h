// dcdsp_tremolo.h — amplitude-modulation tremolo (pure dcdsp core).
//
// Codes against the shared dcdsp primitives (Lfo). In production the guarded
// include below pulls in the real dcdsp_primitives.h; the standalone test
// defines DCDSP_PRIMITIVES_H first and supplies test_stubs.h instead.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>

namespace dcdsp {

struct TremoloParams {
    float rateHz = 5.0f; // 0.1 .. 12  — modulation rate
    float depth  = 0.5f; // 0  .. 1    — 0 = no effect, 1 = full amplitude dip
    int   shape  = 0;    // 0 = sine, 1 = triangle, 2 = square
};

class TremoloCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        numCh_ = numChannels < 1 ? 1 : numChannels;
        lfo_.prepare(sr_);
        lfo_.setFrequency(p_.rateHz);
        applyShape();
        // ~1.5 ms one-pole smoothing on the modulation gain — keeps square-wave
        // edges from clicking without audibly softening sine/triangle shapes.
        smoothCoef_ = (float)std::exp(-1.0 / (0.0015 * sr_));
        shapeCur_ = shapePrev_ = p_.shape;
        shapeFade_.reset(sr_, kShapeFadeSeconds);
        shapeFade_.setCurrentAndTarget(1.0f);
        reset();
    }

    void reset() {
        lfo_.reset();
        gainZ_ = 1.0f;
        shapePrev_ = shapeCur_;
        shapeFade_.setCurrentAndTarget(1.0f);   // a reset lands the waveform too
    }

    void setParams(const TremoloParams& p) {
        TremoloParams q = p;
        q.rateHz = clampf(q.rateHz, 0.1f, 12.0f);
        q.depth  = clampf(q.depth, 0.0f, 1.0f);
        q.shape  = q.shape < 0 ? 0 : (q.shape > 2 ? 2 : q.shape);
        if (q.rateHz != p_.rateHz) lfo_.setFrequency(q.rateHz);
        if (q.shape != p_.shape) { p_.shape = q.shape; applyShape(); setShapeFade(q.shape); }
        p_ = q;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        // numCh_ was written by prepare() and never read, which is exactly why
        // this loop had no channel clamp at all: it walked the CALLER's count
        // and dereferenced unconditionally.
        const int nc = numChannels < numCh_ ? numChannels : numCh_;
        const float depth = p_.depth;
        for (int n = 0; n < numFrames; ++n) {
            // A shape switch jumps the modulation target by up to `depth` at one
            // phase, and the 1.5 ms one-pole below does not remove that -- it
            // turns the click into a THUMP. So the shapes CROSSFADE, exactly as
            // in dcdsp_vibrato.h and dcdsp_ringmod.h. Measured: with the switch
            // hard the output is 63 % of the way to the new shape 144 samples
            // in (DspGainZipperTests.BandedMixCases); with the fade, 15 %.
            const float t = shapeFade_.next();
            float mod = (t >= 1.0f)
                            ? modFor(shapeCur_)
                            : (1.0f - t) * modFor(shapePrev_) + t * modFor(shapeCur_);
            lfo_.advance();
            const float uni = 0.5f * (mod + 1.0f);    // [0, 1]
            const float target = 1.0f - depth * (1.0f - uni); // [1-depth, 1]
            gainZ_ = target + (gainZ_ - target) * smoothCoef_;
            for (int c = 0; c < nc; ++c)
                if (channels[c] != nullptr) channels[c][n] *= gainZ_;
        }
    }

    int latencySamples() const { return 0; }

private:
    /// The LFO value for one shape at the CURRENT phase. Stateless given the
    /// phase, which is what makes the crossfade one extra call rather than a
    /// second oscillator. `Lfo` exposes Sine/Tri/Saw only; square is the sign of
    /// the sine, so it is a third shape here even though the oscillator never
    /// sees it.
    float modFor(int shape) const {
        float m = lfo_.peek(shape == 1 ? Lfo::Tri : Lfo::Sine);
        if (shape == 2) m = (m >= 0.0f ? 1.0f : -1.0f);
        return m;
    }

    /// Re-entrant, for the reason spelled out in dcdsp_ringmod.h's applyShape().
    void setShapeFade(int want) {
        if (want == shapeCur_) return;
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

    void applyShape() {
        // Lfo exposes Sine/Tri/Saw only; square is derived from the sine sign in
        // process(), so a "square" request drives the sine oscillator here.
        lfo_.setShape(p_.shape == 1 ? Lfo::Tri : Lfo::Sine);
    }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    Lfo lfo_;
    TremoloParams p_;
    double sr_ = 44100.0;
    int numCh_ = 2;
    float gainZ_ = 1.0f;
    float smoothCoef_ = 0.0f;
    static constexpr float kShapeFadeSeconds = 0.010f;
    int   shapeCur_  = 0;   // p_.shape stays the request; these two drive the fade
    int   shapePrev_ = 0;
    SmoothedValue shapeFade_;
};

} // namespace dcdsp
