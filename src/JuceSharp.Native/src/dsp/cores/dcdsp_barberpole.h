// dcdsp_barberpole.h — barber-pole (Shepard) phaser core (header-only, RT-safe).
//
// Four INDEPENDENT phaser voices share one sawtooth phase, staggered by a
// quarter cycle each. A voice sweeps its allpass cascade across the whole
// log-frequency range in one cycle and is weighted by a raised-cosine window
// that is ZERO at both ends of its sweep — so a voice fades in at the bottom,
// crosses the band and fades out at the top, and the saw wrap it then performs
// is inaudible. The result is a notch pattern that appears to rise (or fall)
// for ever without a reset.
// Easy to get wrong: any window whose four staggered copies do not SUM to a
// constant. 0.5*(1-cos) at a quarter-cycle stagger sums to exactly 2, which is
// why the wet is normalised by 2 and the level never pumps. Note also that the
// composite pattern is periodic at 4x the LFO rate — that is the illusion
// working, not a bug.
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

struct BarberPoleParams {
    float rateHz    = 0.2f;  // 0.05 .. 5
    int   stages    = 6;     // 2 .. 12, even
    float depth     = 0.8f;  // 0 .. 1  — how much allpass is summed with the dry
    float feedback  = 0.3f;  // -0.9 .. 0.9
    int   direction = 0;     // 0 = Up, 1 = Down
    float mix       = 0.5f;  // 0 .. 1
};

class BarberPoleCore {
public:
    static constexpr int   kVoices    = 4;
    static constexpr int   kMaxStages = 12;
    static constexpr float kFMinHz    = 120.0f;
    static constexpr float kFMaxHz    = 5000.0f;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_    = numChannels < 1 ? 1 : numChannels;
        maxBlock_ = maxBlockSize < 1 ? 1 : maxBlockSize;

        chans_.resize((size_t) numCh_);
        for (auto& c : chans_) c.dc.prepare(sr_);   // rate-derived, not defaulted

        smDepth_.reset(sr_, kParamRampSeconds);
        smFb_.reset(sr_, kParamRampSeconds);
        smMix_.reset(sr_, kParamRampSeconds);
        stageFade_.reset(sr_, kStageFadeSeconds);
        stageFade_.setCurrentAndTarget(1.0f);
        stagesCur_ = stagesPrev_ = stagesRetired_ = evenStages(p_.stages);
        retarget();
        smDepth_.snapToTarget(); smFb_.snapToTarget(); smMix_.snapToTarget();
        stageFade_.snapToTarget();
        // Everything the RATE owns, re-derived here and nowhere else.
        phaseInc_ = (double) clampNan(p_.rateHz, 0.05f, 5.0f, 0.2f) / sr_;
        primed_ = false;
        reset();
    }

    void reset() {
        for (auto& c : chans_) {
            for (auto& v : c.voices) { v.ap.fill(0.0f); v.fb = 0.0f; }
            c.dc.reset();
        }
        phase_ = 0.0;
    }

    void setParams(const BarberPoleParams& p) {
        p_ = p;
        phaseInc_ = (double) clampNan(p.rateHz, 0.05f, 5.0f, 0.2f) / sr_;
        const int want = evenStages(p.stages);
        if (want != stagesCur_) {
            // The cascade is tapped at BOTH counts in one pass (dcdsp_phaser.h's
            // trick), so a stage change crossfades instead of switching the
            // notch pattern in one sample. Flipping BACK inside the window swaps
            // the two ends and continues from 1 - t (dcdsp_tzflanger.h's
            // applyShape): restarting the blend at 0 would jump the output from
            // the half-and-half it is currently emitting to the whole of the
            // shape it is flipping back to, which is the click the crossfade
            // exists to remove.
            if (want == stagesPrev_ && stageFade_.isRamping()) {
                const float t = stageFade_.current();
                stagesPrev_ = stagesCur_;
                stagesCur_  = want;
                stageFade_.setCurrentAndTarget(1.0f - t);
            } else {
                stagesPrev_ = stagesCur_;
                stagesCur_  = want;
                stageFade_.setCurrentAndTarget(0.0f);
            }
            stageFade_.setTarget(1.0f);
        }
        retarget();
        if (!primed_) {
            smDepth_.snapToTarget(); smFb_.snapToTarget(); smMix_.snapToTarget();
            stageFade_.snapToTarget();
            primed_ = true;
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;
        const int ch    = numChannels < numCh_ ? numChannels : numCh_;
        const int nRun  = stagesCur_ > stagesPrev_ ? stagesCur_ : stagesPrev_;
        // Direction is the SIGN of the sweep, nothing else. The phase
        // accumulator keeps running from where it is, so flipping it is
        // continuous by construction and needs no crossfade.
        const double dir = (p_.direction == 1) ? -1.0 : 1.0;
        const double piOverSr = kPi / sr_;
        const float  ratio    = kFMaxHz / kFMinHz;

        std::array<float, kVoices> coef{}, win{};

        for (int n = 0; n < numFrames; ++n) {
            const float depth = smDepth_.next();
            const float fb    = smFb_.next();
            const float wet   = smMix_.next();
            const float dry   = 1.0f - wet;
            const float sFade = stageFade_.next();

            // Per-FRAME, shared by every channel: four staggered sweep
            // positions, their allpass coefficients and their window weights.
            for (int v = 0; v < kVoices; ++v) {
                double pv = phase_ + (double) v / (double) kVoices;
                pv -= std::floor(pv);
                float fc = kFMinHz * std::pow(ratio, (float) pv);
                fc = clampNan(fc, 20.0f, (float) (sr_ * 0.45), 1000.0f);
                const float t = (float) std::tan(piOverSr * (double) fc);
                float a = (t - 1.0f) / (t + 1.0f);
                coef[(size_t) v] = clampNan(a, -0.999f, 0.999f, 0.0f);
                // Raised cosine: 0 at both ends of the voice's own sweep, so it
                // is silent when it wraps. The four of them sum to exactly 2.
                win[(size_t) v] = 0.5f * (1.0f - (float) std::cos(kTwoPi * pv));
            }

            phase_ += dir * phaseInc_;
            phase_ -= std::floor(phase_);

            for (int c = 0; c < ch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                const float x = buf[n];
                ChannelState& cs = chans_[(size_t) c];

                float sum = 0.0f;
                for (int v = 0; v < kVoices; ++v) {
                    VoiceState& vs = cs.voices[(size_t) v];
                    const float a = coef[(size_t) v];
                    float s = x + fb * vs.fb;
                    if (!std::isfinite(s)) s = 0.0f;
                    float sPrev = s, sCur = s;
                    for (int k = 0; k < nRun; ++k) {
                        const float st = vs.ap[(size_t) k];
                        const float u  = s - a * st;      // |a| < 1 => stable
                        const float y  = a * u + st;
                        vs.ap[(size_t) k] = u;
                        s = y;
                        if (k + 1 == stagesPrev_) sPrev = s;
                        if (k + 1 == stagesCur_)  sCur  = s;
                    }
                    s = (sFade >= 1.0f) ? sCur : (1.0f - sFade) * sPrev + sFade * sCur;
                    if (!std::isfinite(s)) s = 0.0f;
                    vs.fb = s < -4.0f ? -4.0f : (s > 4.0f ? 4.0f : s);
                    // One phaser voice: dry + depth * allpass, halved so a
                    // fully-summing voice stays at unity.
                    sum += win[(size_t) v] * 0.5f * (x + depth * s);
                }
                // The four windows sum to 2, so this is a weighted AVERAGE of
                // the voices: no level pumping as they hand over.
                float wetSig = cs.dc.process(0.5f * sum);
                if (!std::isfinite(wetSig)) wetSig = 0.0f;
                float out = dry * x + wet * wetSig;
                if (!std::isfinite(out)) out = 0.0f;
                buf[n] = out;
            }
        }

        // The fade has landed: retire the sections above the live count so a
        // later GROW cannot re-inject audio from before this change.
        // …and it is driven off "the fade landed AND a section is now above the
        // live count", not off `stagesPrev_ != stagesCur_`: a flip-back leaves
        // those two EQUAL while sections 6..11 still hold pre-flip state, so the
        // old condition skipped exactly the case the retirement exists for.
        if (!stageFade_.isRamping() && stagesRetired_ != stagesCur_) {
            for (auto& c : chans_)
                for (auto& v : c.voices)
                    for (int k = stagesCur_; k < kMaxStages; ++k) v.ap[(size_t) k] = 0.0f;
            stagesPrev_    = stagesCur_;
            stagesRetired_ = stagesCur_;
        }
    }

    int latencySamples() const { return 0; }

private:
    static float clampNan(float v, float lo, float hi, float fallback) {
        if (!std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static int evenStages(int s) {
        if (s < 2) s = 2;
        if (s > kMaxStages) s = kMaxStages;
        return s & ~1;   // the cascade only makes notches in pairs
    }
    void retarget() {
        smDepth_.setTarget(clampNan(p_.depth, 0.0f, 1.0f, 0.8f));
        smFb_.setTarget(clampNan(p_.feedback, -0.9f, 0.9f, 0.0f));
        smMix_.setTarget(clampNan(p_.mix, 0.0f, 1.0f, 0.5f));
    }

    struct VoiceState {
        std::array<float, kMaxStages> ap{};
        float fb = 0.0f;
    };
    struct ChannelState {
        std::array<VoiceState, kVoices> voices{};
        DcBlocker dc;
    };

    static constexpr float kParamRampSeconds = 0.015f;
    static constexpr float kStageFadeSeconds = 0.010f;

    std::vector<ChannelState> chans_;
    SmoothedValue smDepth_, smFb_, smMix_, stageFade_;
    int stagesCur_ = 6, stagesPrev_ = 6, stagesRetired_ = 6;
    double phase_ = 0.0, phaseInc_ = 0.0, sr_ = 48000.0;
    bool primed_ = false;
    BarberPoleParams p_{};
    int numCh_ = 2;
    int maxBlock_ = 2048;
};

} // namespace dcdsp
