// dcdsp_freqshift.h — Bode-style single-sideband frequency shifter, RT-safe.
//
// NOT a pitch shifter. Every partial moves by the SAME number of hertz, so a
// 440 Hz tone with shiftHz 100 lands at 540 Hz and the harmonic series is
// destroyed — which is the whole point of the effect.
// Topology: a polyphase IIR Hilbert PAIR (two four-section cascades of
// second-order z^-2 allpasses, one branch fed the input delayed by a sample)
// whose outputs are ~90 degrees apart at every audio frequency, multiplied by a
// quadrature oscillator. With branch A on the delayed input, I*cos + Q*sin keeps
// the upper sideband and I*cos - Q*sin the lower.
// Easy to get wrong: both branches are ALLPASS, so the magnitude response is
// unity by construction and "it is not attenuating" proves nothing — the thing
// to measure is the phase error, which shows up as the unwanted sideband (the
// IMAGE) and nowhere else. Flipping the direction is also a full-scale sign
// change on the Q term, so the two sidebands crossfade rather than switch.
//
// Namespace dcdsp. C++17. Deps: only the shared primitives header.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct FreqShiftParams {
    float shiftHz  = 5.0f;   // -1000 .. 1000
    float mix      = 0.5f;   // 0 .. 1
    float feedback = 0.0f;   // 0 .. 0.9
    int   direction = 0;     // 0 = Up, 1 = Down, 2 = Both (up on L, down on R)
};

/// Two four-section allpass cascades whose outputs differ by ~90 degrees over
/// roughly 20 Hz .. 20 kHz at 48 kHz. Coefficients are the published
/// polyphase-IIR Hilbert pair (Niemitalo); they are squared pole radii, so the
/// sections are unconditionally stable and EXACTLY unit magnitude.
class HilbertPair {
public:
    static constexpr int kSections = 4;

    void reset() {
        for (auto& b : branch_)
            for (auto& s : b) { s.x1 = s.x2 = s.y1 = s.y2 = 0.0f; }
        delayed_ = 0.0f;
    }

    /// One sample in, the in-phase and quadrature parts out.
    void process(float x, float& inPhase, float& quadrature) {
        // Branch A sees the input delayed by one sample; that delay is what
        // turns "two allpass cascades" into a quadrature PAIR.
        const float xa = delayed_;
        delayed_ = x;
        inPhase    = run(0, xa);
        quadrature = run(1, x);
    }

private:
    struct Section { float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f; };

    float run(int b, float x) {
        static constexpr float kA[2][kSections] = {
            { 0.6923877778065f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f },
            { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f }
        };
        for (int i = 0; i < kSections; ++i) {
            Section& s = branch_[(size_t) b][(size_t) i];
            const float a = kA[b][i];
            // H(z) = (a - z^-2) / (1 - a z^-2), a = pole radius squared.
            const float y = a * (x + s.y2) - s.x2;
            s.x2 = s.x1; s.x1 = x;
            s.y2 = s.y1; s.y1 = y;
            x = y;
        }
        return std::isfinite(x) ? x : 0.0f;
    }

    std::array<std::array<Section, kSections>, 2> branch_{};
    float delayed_ = 0.0f;
};

class FreqShiftCore {
public:
    static constexpr int   kSides    = 2;
    static constexpr float kMaxShift = 1000.0f;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_    = numChannels < 1 ? 1 : numChannels;
        maxBlock_ = maxBlockSize < 1 ? 1 : maxBlockSize;

        hilbert_.resize((size_t) numCh_);
        dc_.resize((size_t) numCh_);
        fbState_.assign((size_t) numCh_, 0.0f);
        for (auto& d : dc_) d.prepare(sr_);   // inside the feedback loop

        smShift_.reset(sr_, kParamRampSeconds);
        smMix_.reset(sr_, kParamRampSeconds);
        smFb_.reset(sr_, kParamRampSeconds);
        for (auto& row : smDir_) for (auto& s : row) s.reset(sr_, kDirFadeSeconds);
        retarget();
        snapAll();
        primed_ = false;
        reset();
    }

    void reset() {
        for (auto& h : hilbert_) h.reset();
        for (auto& d : dc_) d.reset();
        std::fill(fbState_.begin(), fbState_.end(), 0.0f);
        phase_ = 0.0;
    }

    void setParams(const FreqShiftParams& p) {
        p_ = p;
        retarget();
        if (!primed_) { snapAll(); primed_ = true; }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;
        const int ch = numChannels < numCh_ ? numChannels : numCh_;

        std::array<std::array<float, 2>, kSides> dirW{};

        for (int n = 0; n < numFrames; ++n) {
            const float shift = smShift_.next();
            const float wet   = smMix_.next();
            const float dry   = 1.0f - wet;
            const float fb    = smFb_.next();
            for (int s = 0; s < kSides; ++s)
                for (int k = 0; k < 2; ++k)
                    dirW[(size_t) s][(size_t) k] = smDir_[(size_t) s][(size_t) k].next();

            // The carrier's PHASE is continuous across a shift change, which is
            // what makes a 0 -> 200 Hz jump a glide rather than a step: nothing
            // in the output depends on the frequency directly.
            const float c = (float) std::cos(kTwoPi * phase_);
            const float s = (float) std::sin(kTwoPi * phase_);
            phase_ += (double) shift / sr_;
            phase_ -= std::floor(phase_);

            for (int cc = 0; cc < ch; ++cc) {
                float* buf = channels[cc];
                if (buf == nullptr) continue;
                const size_t ci   = (size_t) cc;
                const size_t side = (size_t) (cc & 1);
                const float x = buf[n];

                float in = x + fb * fbState_[ci];
                if (!std::isfinite(in)) in = 0.0f;
                in = in < -4.0f ? -4.0f : (in > 4.0f ? 4.0f : in);

                float ip = 0.0f, qp = 0.0f;
                hilbert_[ci].process(in, ip, qp);
                // The SIGN convention is not a free choice and was measured,
                // not derived: with branch A taking the one-sample-delayed
                // input, A LAGS B by 90 degrees, so it is `+ qp * s` that keeps
                // the UPPER sideband. Getting it the other way round is
                // invisible in every magnitude measurement — both sidebands are
                // the same size — and shows up only as a 440 Hz tone landing at
                // 340 Hz instead of 540.
                const float up   = ip * c + qp * s;   // upper sideband
                const float down = ip * c - qp * s;   // lower sideband
                float shifted = dirW[side][0] * up + dirW[side][1] * down;
                if (!std::isfinite(shifted)) shifted = 0.0f;

                float fbIn = dc_[ci].process(shifted);
                if (!std::isfinite(fbIn)) fbIn = 0.0f;
                fbState_[ci] = fbIn < -4.0f ? -4.0f : (fbIn > 4.0f ? 4.0f : fbIn);

                float out = dry * x + wet * shifted;
                if (!std::isfinite(out)) out = 0.0f;
                buf[n] = out;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    static float clampNan(float v, float lo, float hi, float fallback) {
        if (!std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    void snapAll() {
        smShift_.snapToTarget(); smMix_.snapToTarget(); smFb_.snapToTarget();
        for (auto& row : smDir_) for (auto& s : row) s.snapToTarget();
    }
    void retarget() {
        smShift_.setTarget(clampNan(p_.shiftHz, -kMaxShift, kMaxShift, 0.0f));
        smMix_.setTarget(clampNan(p_.mix, 0.0f, 1.0f, 0.5f));
        smFb_.setTarget(clampNan(p_.feedback, 0.0f, 0.9f, 0.0f));
        // Up / Down is a SIGN CHANGE on the quadrature term — up to twice the
        // wet amplitude in one sample. It crossfades, per side, over 10 ms.
        int d = p_.direction;
        if (d < 0 || d > 2) d = 0;
        for (int s = 0; s < kSides; ++s) {
            const bool wantUp = (d == 0) || (d == 2 && s == 0);
            smDir_[(size_t) s][0].setTarget(wantUp ? 1.0f : 0.0f);
            smDir_[(size_t) s][1].setTarget(wantUp ? 0.0f : 1.0f);
        }
    }

    static constexpr float kParamRampSeconds = 0.015f;
    static constexpr float kDirFadeSeconds   = 0.010f;

    std::vector<HilbertPair> hilbert_;
    std::vector<DcBlocker> dc_;
    std::vector<float> fbState_;
    SmoothedValue smShift_, smMix_, smFb_;
    std::array<std::array<SmoothedValue, 2>, kSides> smDir_{};
    double phase_ = 0.0, sr_ = 48000.0;
    bool primed_ = false;
    FreqShiftParams p_{};
    int numCh_ = 2;
    int maxBlock_ = 2048;
};

} // namespace dcdsp
