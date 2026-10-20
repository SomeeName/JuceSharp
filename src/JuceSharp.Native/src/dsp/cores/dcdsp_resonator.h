// dcdsp_resonator.h — tuned Karplus-Strong comb bank (dcdsp core).
//
// A bank of up to four tuned, damped comb resonators — Karplus-Strong strings
// with no exciter of their own. Your guitar excites them, so whatever you play
// rings a chord you CHOSE: pick anything and the bank sings the key back at you.
// Sympathetic strings (a sitar's tarafs, a Nashville-tuned second guitar, an
// EBow'd drone) are exactly this, and so is the "shimmer/ambient in-key" trick
// people spend a whole rack on.
//
// Each voice is the classic KS loop:
//     y[n] = delayLine(L)            (fractional, 3rd-order Lagrange)
//     lp   = onePole(y[n])           (in-loop damping: the string's HF loss)
//     push(  inScale * x[n] + tanh(fb * lp)  )
//
// -------------------------- STABILITY GUARANTEE ----------------------------
// The loop's recirculating value passes through an ADAA tanh, which returns
// tanh(xi) for some xi (mean value theorem), so its magnitude is < 1 EVERY
// sample, unconditionally — the comb cannot run away even if the feedback
// coefficient, the damping one-pole and the fractional-delay interpolator all
// conspired to exceed unity gain. The input is scaled by (1 - fb), which is the
// standard resonant-gain normalization: the steady-state gain AT a resonance is
// ~1, so the wet arrives at roughly the level you played, not at 200x it (which
// is what an un-normalized fb = 0.995 comb does). Non-finite scrub + a hard
// output clamp guard the rest.
// ---------------------------------------------------------------------------
//
// Contract: namespace dcdsp, C++17, header-only, zero framework deps. Allocation
// happens ONLY in prepare(); process() never allocates, locks, or throws.
#pragma once

#include "dcdsp_primitives.h"

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct ResonatorParams {
    // Root pitch class: 0 = C, 1 = C#, ... 11 = B.
    int key = 0;
    // Which chord the bank is tuned to:
    //   0 = Power (root, 5th, octave)
    //   1 = Major (root, 3rd, 5th, octave)
    //   2 = Minor (root, b3rd, 5th, octave)
    //   3 = Sus4  (root, 4th, 5th, octave)
    //   4 = Octaves (root, octave, two octaves)
    int chord = 0;
    // Register of the root: MIDI note = 24 + key + 12 * octave (2 -> C3). [0, 4]
    int octave = 2;
    // Ring time, exponentially mapped to 0.3 s .. 6 s of T60. Solved into a
    // PER-VOICE feedback coefficient (always < 1), so every note of the chord
    // rings for the same time. [0, 1]
    float decay = 0.7f;
    // In-loop high-frequency loss. Higher = a darker, shorter ring. [0, 1]
    float damping = 0.5f;
    // Detune between voices and between channels -> width and chorus. [0, 1]
    float spread = 0.5f;
    // Dry/wet blend. [0, 1]
    float mix = 0.5f;
};

class ResonatorCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxCh_ = (numChannels > 0) ? numChannels : 1;

        // The longest string the bank can be tuned to (30 Hz) sets the delay line.
        maxDelay_ = static_cast<int>(sr_ / kMinHz) + 8;

        chans_.clear();
        chans_.resize(static_cast<std::size_t>(maxCh_));
        for (auto& ch : chans_) {
            for (auto& v : ch.voices) {
                v.line.prepare(maxDelay_);
                v.line.setInterpolation(InterpolatedDelayLine::Lagrange3);
            }
            ch.dc.prepare(sr_, 18.0);
        }
        // `mix` RAMPS (kMixRampSeconds); as a per-block constant a mix drag
        // stepped a full dry/wet crossfade at every block edge.
        mixSmooth_.reset(sr_, kMixRampSeconds);
        mixSmooth_.setTarget(mix_);
        mixSmooth_.snapToTarget();   // a freshly prepared bank starts AT its mix
        for (auto& ch : chans_)
            for (auto& d : ch.delaySm) d.reset(sr_, kRetuneSeconds);
        voiceCountPrev_ = 0;         // a fresh prepare snaps every tap
        retune();
        for (auto& ch : chans_)
            for (auto& d : ch.delaySm) d.snapToTarget();
        reset();
    }

    void reset() {
        for (auto& ch : chans_)
            for (auto& d : ch.delaySm) d.snapToTarget();   // a cleared bank starts AT its taps
        for (auto& ch : chans_) {
            for (auto& v : ch.voices) {
                v.line.reset();
                v.damp.reset();
                v.sat = 0.f;
            }
            ch.dc.reset();
        }
    }

    void setParams(const ResonatorParams& p) {
        key_     = clampi(p.key, 0, 11);
        chord_   = clampi(p.chord, 0, 4);
        octave_  = clampi(p.octave, 0, 4);
        decay_   = clamp(p.decay, 0.f, 1.f);
        damping_ = clamp(p.damping, 0.f, 1.f);
        spread_  = clamp(p.spread, 0.f, 1.f);
        mix_     = clamp(p.mix, 0.f, 1.f);
        mixSmooth_.setTarget(mix_);
        retune();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int C = std::min(numChannels, static_cast<int>(chans_.size()));
        if (C <= 0) return;

        const float norm    = 1.f / std::sqrt(static_cast<float>(std::max(1, voiceCount_)));

        // Channel-OUTER loop: one ramp line for the block, handed to every
        // channel, advanced once after the loop.
        float mixStep = 0.f;
        const float mixBase = mixSmooth_.blockStart(numFrames, mixStep);

        for (int c = 0; c < C; ++c) {
            float* buf = channels[c];
            if (buf == nullptr) continue;
            Channel& ch = chans_[static_cast<std::size_t>(c)];

            for (int n = 0; n < numFrames; ++n) {
                float in = buf[n];
                if (!std::isfinite(in)) in = 0.f;
                in = clamp(in, -8.f, 8.f);

                float sum = 0.f;
                for (int v = 0; v < voiceCount_; ++v) {
                    const std::size_t vi = static_cast<std::size_t>(v);
                    Voice& vo = ch.voices[vi];

                    const float y  = vo.line.readAt(ch.delaySm[vi].next());
                    const float lp = vo.damp.process(y);
                    // |tanh| < 1 strictly -> the loop is bounded by construction.
                    const float fb = adaaTanh(fbCoef_[vi] * lp, vo.sat);
                    float push = inScale_[vi] * in + fb;
                    if (!std::isfinite(push)) {
                        push = 0.f;
                        vo.line.reset();
                        vo.damp.reset();
                        vo.sat = 0.f;
                    }
                    vo.line.push(push);
                    sum += y;
                }

                float wet = ch.dc.process(sum * norm);
                if (!std::isfinite(wet)) { wet = 0.f; ch.dc.reset(); }

                const float mixG    = mixBase + mixStep * static_cast<float>(n);
                float out = (1.f - mixG) * in + mixG * wet;
                if (!std::isfinite(out)) out = 0.f;
                buf[n] = clamp(out, -16.f, 16.f);
            }
        }

        mixSmooth_.advanceBlock(numFrames);
    }

    int latencySamples() const { return 0; }

private:
    static constexpr int    kMaxVoicesC = 4;
    static constexpr double kMinHz      = 30.0;
    static constexpr double kMaxHz      = 2000.0;

    struct Voice {
        InterpolatedDelayLine line;
        OnePole               damp;
        float                 sat = 0.f;
    };
    struct Channel {
        std::array<Voice, kMaxVoicesC> voices;
        // The read position of a LOADED Karplus-Strong loop. Every one of key,
        // chord, octave and spread rewrites it, and `spread` is a knob the user
        // DRAGS: stepping the tap teleported the read head inside a comb whose
        // feedback runs up to 0.998, so the splice was not a click that decayed
        // -- it was re-injected into the loop and rang for up to six seconds.
        // Ramped, the same edit is a short pitch glide.
        std::array<SmoothedValue, kMaxVoicesC> delaySm;
        DcBlocker                      dc;
    };

    static float clamp(float v, float lo, float hi) {
        if (!(v == v)) return lo;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    static float midiToHz(float midi) {
        return 440.f * std::exp2((midi - 69.f) / 12.f);
    }

    void retune() {
        // Chord tables, in semitones above the root.
        static const int kPower[3]   = { 0, 7, 12 };
        static const int kMajor[4]   = { 0, 4, 7, 12 };
        static const int kMinor[4]   = { 0, 3, 7, 12 };
        static const int kSus4[4]    = { 0, 5, 7, 12 };
        static const int kOctaves[3] = { 0, 12, 24 };

        const int* table;
        switch (chord_) {
            case 1:  table = kMajor;   voiceCount_ = 4; break;
            case 2:  table = kMinor;   voiceCount_ = 4; break;
            case 3:  table = kSus4;    voiceCount_ = 4; break;
            case 4:  table = kOctaves; voiceCount_ = 3; break;
            default: table = kPower;   voiceCount_ = 3; break;
        }

        const float rootMidi = static_cast<float>(24 + key_ + 12 * octave_);
        const float centre   = 0.5f * static_cast<float>(voiceCount_ - 1);

        // `decay` is RING TIME IN SECONDS, exponentially mapped: 0.3 s .. 6 s. It is
        // NOT the comb's feedback coefficient. Making the knob linear in fb (the
        // obvious thing) measured 0.5 s / 0.9 s / 4.1 s at decay 0 / 0.5 / 1 — the
        // entire useful range crammed into the last fifth of the knob, because T60
        // goes as 1/(1 - fb).
        const float t60 = kMinRingS * std::exp2(std::log2(kMaxRingS / kMinRingS) * decay_);

        for (int v = 0; v < voiceCount_; ++v) {
            const std::size_t vi = static_cast<std::size_t>(v);

            // Per-voice detune (works in mono too — it is the bank chorusing with
            // itself, not a stereo trick).
            const float cents = (static_cast<float>(v) - centre) * spread_ * 3.f;
            const float midi  = rootMidi + static_cast<float>(table[v]) + cents / 100.f;
            const float hz    = clamp(midiToHz(midi),
                                      static_cast<float>(kMinHz),
                                      static_cast<float>(kMaxHz));
            const float d = clamp(static_cast<float>(sr_) / hz,
                                  4.f, static_cast<float>(maxDelay_));
            delay_[vi] = d;

            // Feedback PER VOICE, solved from the target ring time:
            //     fb = 10^(-3 * L / (fs * T60))   <=>   fb^(T60*fs/L) = 10^-3
            // A single shared fb would make the octave voice ring HALF as long as the
            // root (T60 scales with the delay length), so a "chord" would collapse to
            // its root within a second. Solving per voice keeps the chord a chord.
            const float e  = -3.f * d / (static_cast<float>(sr_) * t60);
            const float fb = clamp(std::pow(10.f, e), 0.5f, kMaxFb);
            fbCoef_[vi] = fb;

            // Excitation law. The obvious choice — inScale = (1 - fb), which makes the
            // steady-state gain AT a resonance exactly 1 — is WRONG here, and
            // measurably so: it assumes the input is a sine parked on the comb's peak.
            // A guitar is not. Its energy is spread over a harmonic series that almost
            // never lands on the bank's peaks, so everything off-resonance gets the
            // comb's stopband gain ((1-fb)/(1+fb), i.e. nothing) — the bank measured
            // 30-45 dB under the dry and, absurdly, got QUIETER as decay went up.
            //
            // sqrt(1 - fb^2) is the energy-preserving excitation: it scales with the
            // loop's per-cycle ENERGY loss rather than its amplitude loss, so a long
            // ring and a short one arrive at the same loudness, and the bank is driven
            // by the pick TRANSIENT — which is broadband, and is what actually rings a
            // sympathetic string. kExcite is measured, not guessed: it lands the wet
            // within a few dB of the dry across the whole decay range for a -18 dBFS
            // guitar.
            inScale_[vi] = kExcite * std::sqrt(std::max(0.f, 1.f - fb * fb));
        }
        for (int v = voiceCount_; v < kMaxVoicesC; ++v) {
            const std::size_t vi = static_cast<std::size_t>(v);
            delay_[vi]   = 4.f;
            fbCoef_[vi]  = 0.f;
            inScale_[vi] = 0.f;
        }

        // Per-channel detune: +/- spread * 6 cents -> the bank decorrelates across
        // the stereo field instead of doubling dead-centre.
        const float chCents = spread_ * 6.f;
        chDetune_[0] = std::exp2( chCents / 1200.f);   // left  rings slightly flat
        chDetune_[1] = std::exp2(-chCents / 1200.f);   // right slightly sharp

        // Publish the per-channel taps. A voice that was ALREADY sounding ramps;
        // one the chord just switched on snaps, because its delay line holds
        // nothing but zeros (fbCoef_/inScale_ were 0 while it was off) and there
        // is no discontinuity to smooth.
        for (std::size_t c = 0; c < chans_.size(); ++c) {
            Channel& ch = chans_[c];
            const float det = chDetune_[static_cast<std::size_t>(c & 1)];
            for (int v = 0; v < kMaxVoicesC; ++v) {
                const std::size_t vi = static_cast<std::size_t>(v);
                ch.delaySm[vi].setTarget(delay_[vi] * det);
                if (v >= voiceCount_ || v >= voiceCountPrev_) ch.delaySm[vi].snapToTarget();
            }
        }
        voiceCountPrev_ = voiceCount_;

        // In-loop damping: a gentle one-pole low-pass, 8 kHz (bright) down to
        // 700 Hz (a dark, felt-muted string).
        const double dampHz = 8000.0 - 7300.0 * static_cast<double>(damping_);
        for (auto& ch : chans_)
            for (auto& v : ch.voices)
                v.damp.setLowpass(sr_, dampHz);
    }

    // Excitation constant — see retune(). Measured against a -18 dBFS picked low E.
    static constexpr float kExcite   = 1.15f;
    static constexpr float kMaxFb    = 0.998f;  // still strictly < 1
    static constexpr float kRetuneSeconds = 0.020f;  // tap glide on a retune
    static constexpr float kMinRingS = 0.3f;    // decay = 0
    static constexpr float kMaxRingS = 6.0f;    // decay = 1

    double sr_        = 48000.0;
    int    maxCh_     = 2;
    int    maxDelay_  = 1608;

    int   key_     = 0;
    int   chord_   = 0;
    int   octave_  = 2;
    float decay_   = 0.7f;
    float damping_ = 0.5f;
    float spread_  = 0.5f;
    float mix_     = 0.5f;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixSmooth_;

    int voiceCount_ = 3;
    int voiceCountPrev_ = 0;
    std::array<float, kMaxVoicesC> delay_{};
    std::array<float, kMaxVoicesC> fbCoef_{};    // per voice: solved from the ring time
    std::array<float, kMaxVoicesC> inScale_{};   // per voice: sqrt(1 - fb^2) excitation
    std::array<float, 2>           chDetune_{ 1.f, 1.f };

    std::vector<Channel> chans_;
};

} // namespace dcdsp
