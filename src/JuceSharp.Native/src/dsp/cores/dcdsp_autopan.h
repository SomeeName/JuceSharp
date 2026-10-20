// dcdsp_autopan.h — stereo auto-panner / rotary placement core (header-only, RT-safe).
//
// One LFO read at TWO phases: the left channel's gain comes from phase p, the
// right from p + phaseDeg. At 180 degrees the two are in antiphase and this is
// a classic auto-pan (the signal walks across the pair); at 0 it degenerates to
// a tremolo; 90 (the default) is the quadrature motion of a rotating source.
// Both channels use the SAME law, g(pos) = cos(pi/4 * (1 + pos)) for equal
// power, so at 180 degrees gL^2 + gR^2 is exactly 1 and the pair never pumps.
// Easy to get wrong: the SQUARE shape. A raw square is a full-scale step twice
// per cycle, i.e. a click at any depth — so the modulator is slew-limited to a
// 10 ms full-scale transit, re-derived from the sample rate in prepare().
//
// Namespace dcdsp. C++17. Deps: only the shared primitives header (Lfo::shapeAt
// is used STATICALLY — this core owns its own phase accumulator because it has
// to read one oscillator at two phases, which Lfo's stateful peek() cannot do).
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct AutoPanParams {
    float rateHz   = 1.0f;   // 0.05 .. 10
    float depth    = 1.0f;   // 0 .. 1
    int   shape    = 0;      // 0 = Sine, 1 = Triangle, 2 = Square
    float phaseDeg = 90.0f;  // 0 .. 180 — the right channel's LFO offset
    int   law      = 0;      // 0 = EqualPower, 1 = Linear
};

class AutoPanCore {
public:
    // Shape slots: 0 = the live shape, 1 = the one being faded out.
    static constexpr int kSlots = 2;
    static constexpr int kMaxCh = 2;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_    = numChannels < 1 ? 1 : numChannels;
        maxBlock_ = maxBlockSize < 1 ? 1 : maxBlockSize;

        // Everything the RATE owns, re-derived here: the phase increment and
        // the square's slew step (a full -1 -> +1 transit in kSquareSlewSeconds
        // whatever the device rate).
        phaseInc_  = (double) clampNan(p_.rateHz, 0.05f, 10.0f, 1.0f) / sr_;
        slewStep_  = (float) (2.0 / (kSquareSlewSeconds * sr_));

        smDepth_.reset(sr_, kParamRampSeconds);
        smPhase_.reset(sr_, kParamRampSeconds);
        shapeFade_.reset(sr_, kShapeFadeSeconds);
        shapeFade_.setCurrentAndTarget(1.0f);
        shapeCur_ = shapePrev_ = p_.shape;
        lawFade_.reset(sr_, kShapeFadeSeconds);
        lawFade_.setCurrentAndTarget(1.0f);
        lawCur_ = lawPrev_ = (p_.law == 1) ? 1 : 0;
        retarget();
        smDepth_.snapToTarget(); smPhase_.snapToTarget();
        shapeFade_.snapToTarget(); lawFade_.snapToTarget();
        primed_ = false;
        reset();
    }

    void reset() {
        phase_ = 0.0;
        for (auto& row : slew_) for (auto& v : row) v = 0.0f;
    }

    void setParams(const AutoPanParams& p) {
        p_ = p;
        phaseInc_ = (double) clampNan(p.rateHz, 0.05f, 10.0f, 1.0f) / sr_;
        applyShape();
        applyLaw();
        retarget();
        if (!primed_) {
            smDepth_.snapToTarget(); smPhase_.snapToTarget();
            shapeFade_.snapToTarget(); lawFade_.snapToTarget();
            primed_ = true;
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;
        const int ch = numChannels < numCh_ ? numChannels : numCh_;

        for (int n = 0; n < numFrames; ++n) {
            const float depth  = smDepth_.next();
            // phaseDeg RAMPS. Writing it straight would teleport the right
            // channel's oscillator, which is a step in a GAIN however smooth
            // the gain law is.
            const float offset = smPhase_.next() * (1.0f / 360.0f);
            const float t      = shapeFade_.next();
            // …and `law` is that same step with the oscillator left alone: the
            // two laws differ by 3.0 dB at pos 0 (0.7071 against 0.5), so
            // reading p_.law raw at the block top put a -3 dB gain change on
            // one sample boundary. Both are cheap; blend them.
            const float lf     = lawFade_.next();

            for (int c = 0; c < ch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                const double ph = phase_ + ((c & 1) ? (double) offset : 0.0);
                float m = modulator(c, ph, t);
                if (!std::isfinite(m)) m = 0.0f;
                float pos = depth * m;
                pos = pos < -1.0f ? -1.0f : (pos > 1.0f ? 1.0f : pos);
                float g = lawGain(lawCur_, pos);
                if (lf < 1.0f) g = (1.0f - lf) * lawGain(lawPrev_, pos) + lf * g;
                if (!std::isfinite(g)) g = 1.0f;
                float out = buf[n] * g;
                if (!std::isfinite(out)) out = 0.0f;
                buf[n] = out;
            }

            phase_ += phaseInc_;
            phase_ -= std::floor(phase_);
        }
    }

    int latencySamples() const { return 0; }

private:
    static float clampNan(float v, float lo, float hi, float fallback) {
        if (!std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    void retarget() {
        smDepth_.setTarget(clampNan(p_.depth, 0.0f, 1.0f, 1.0f));
        smPhase_.setTarget(clampNan(p_.phaseDeg, 0.0f, 180.0f, 90.0f));
    }

    // A shape switch is a full-scale discontinuity in the modulator, so the two
    // shapes are evaluated at the SAME phase and crossfaded — the ringmod /
    // vibrato shape, with one addition: the square's slew state is per SLOT, so
    // the outgoing square keeps walking while it fades rather than freezing.
    void applyShape() {
        int want = p_.shape;
        if (want < 0 || want > 2) want = 0;
        if (want == shapeCur_) return;
        if (want == shapePrev_ && shapeFade_.isRamping()) {
            const float t = shapeFade_.current();
            shapePrev_ = shapeCur_;
            shapeCur_  = want;
            // The two ENDS swapped, so the per-slot square slew has to swap with
            // them: slot 0 is whatever shapeCur_ is. Leaving it behind handed the
            // outgoing square slot 1's stale walker and stepped the modulator by
            // the distance between the two — measured 0.0196 against a 0.0054
            // settled slope on Sine -> Square -> Sine, 3 ms apart.
            for (int c = 0; c < kMaxCh; ++c)
                std::swap(slew_[0][(size_t) c], slew_[1][(size_t) c]);
            shapeFade_.setCurrentAndTarget(1.0f - t);
        } else {
            shapePrev_ = shapeCur_;
            shapeCur_  = want;
            shapeFade_.setCurrentAndTarget(0.0f);
        }
        shapeFade_.setTarget(1.0f);
    }

    /// The two pan laws, both evaluated so a switch can be crossfaded.
    /// Neither is unity at rest: Equal Power is -3 dB at pos 0 and Linear -6 dB.
    static float lawGain(int law, float pos) {
        return (law != 1) ? (float) std::cos(kPi * 0.25 * (1.0 + (double) pos))
                          : 0.5f * (1.0f - pos);
    }

    // `law` substitutes one per-sample gain curve for another, which is the same
    // class of write as `shape` and gets the same treatment — including the
    // flip-back: back inside the window swaps the two ends and keeps the blend's
    // position, because discarding it is the click the crossfade exists to remove.
    void applyLaw() {
        const int want = (p_.law == 1) ? 1 : 0;
        if (want == lawCur_) return;
        if (want == lawPrev_ && lawFade_.isRamping()) {
            const float t = lawFade_.current();
            lawPrev_ = lawCur_;
            lawCur_  = want;
            lawFade_.setCurrentAndTarget(1.0f - t);
        } else {
            lawPrev_ = lawCur_;
            lawCur_  = want;
            lawFade_.setCurrentAndTarget(0.0f);
        }
        lawFade_.setTarget(1.0f);
    }

    float shapeValue(int slot, int c, int shape, double phase) {
        float ph = (float) (phase - std::floor(phase));
        if (!(ph >= 0.f && ph < 1.f)) ph = 0.f;
        if (shape == 2) {
            // Square, slew-limited. The raw +/-1 edge is the click; the slew is
            // what the crossfade rule asks for and it is rate-derived.
            const float raw = (Lfo::shapeAt(Lfo::Sine, ph) >= 0.f) ? 1.0f : -1.0f;
            float& s = slew_[(size_t) slot][(size_t) (c & 1)];
            if (raw > s)      s = std::min(raw, s + slewStep_);
            else if (raw < s) s = std::max(raw, s - slewStep_);
            return s;
        }
        return Lfo::shapeAt(shape == 1 ? Lfo::Tri : Lfo::Sine, ph);
    }

    float modulator(int c, double phase, float fade) {
        if (fade >= 1.0f) {
            // Keep the retired slot's square walking so a switch BACK does not
            // resume from a stale edge.
            if (shapePrev_ != shapeCur_) (void) shapeValue(1, c, shapePrev_, phase);
            return shapeValue(0, c, shapeCur_, phase);
        }
        const float a = shapeValue(1, c, shapePrev_, phase);
        const float b = shapeValue(0, c, shapeCur_, phase);
        return (1.0f - fade) * a + fade * b;
    }

    static constexpr float kParamRampSeconds   = 0.015f;
    static constexpr float kShapeFadeSeconds   = 0.010f;
    static constexpr double kSquareSlewSeconds = 0.010;

    SmoothedValue smDepth_, smPhase_, shapeFade_, lawFade_;
    float slew_[kSlots][kMaxCh]{};
    int shapeCur_ = 0, shapePrev_ = 0;
    int lawCur_ = 0, lawPrev_ = 0;
    double phase_ = 0.0, phaseInc_ = 0.0, sr_ = 48000.0;
    float slewStep_ = 0.004f;
    bool primed_ = false;
    AutoPanParams p_{};
    int numCh_ = 2;
    int maxBlock_ = 2048;
};

} // namespace dcdsp
