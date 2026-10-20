// dcdsp_chorus.h — multi-voice modulated-delay chorus (pure dcdsp core).
//
// Modulated InterpolatedDelayLine taps (base delay ~12-20 ms) summed with dry.
// Voices are decorrelated by staggering each voice LFO's start phase and by a
// small per-voice base-delay offset; L/R are decorrelated by inverting the
// modulation sign on odd channels (for every shape here that inversion is
// EXACTLY a half-cycle phase shift, so it is a phase relationship, not a
// polarity trick).
//
// 0.16 fixes:
//  - Depth is clamped per voice so the modulated tap can never underflow the
//    front of the delay line (max depth 15 ms vs voice-0 base 12 ms used to
//    pin the tap at ~0 for part of the cycle — a hard waveform fold).
//  - Depth and mix changes are ramped (~15 ms SmoothedValue) so parameter
//    moves no longer zipper/click.
//
// 0.18 BBD pass — four new params, and EVERY ONE OF THEM IS A NO-OP AT ITS
// DEFAULT. The pre-0.18 core was a textbook modulated delay: sine-only LFOs, no
// regeneration, no tone, and a stereo image that was just the modulation sign
// flipped. That is why it read narrow and "seasick" — a full-band wet path with
// a pure sine sweep wobbles the low strings' fundamentals, which is the one
// thing a real bucket-brigade chorus never does (its wet path is bandwidth-
// limited and its LFO is not a sine).
//
//   feedback  0..0.9,   default 0    — the BBD regeneration path this core
//                                      never had. At 0 the pushed sample is
//                                      literally `x`, so the line, the taps and
//                                      the output are bit-identical to 0.16.
//   spread    0..1,     default 0    — TRUE stereo: rotates each channel's
//                                      per-voice LFO phase instead of only
//                                      flipping the sign, so L and R get
//                                      genuinely different comb trajectories
//                                      rather than mirror images. Channel 0 is
//                                      never rotated, so mono is untouched and
//                                      spread=0 is the 0.16 image exactly.
//   lfoShape  0/1/2,    default 0    — Sine (0.16), Triangle, Hyper-triangle.
//                                      A CE-2's sweep is a linear (triangle)
//                                      ramp, not a sine.
//   wetHpfHz  20..400,  default 20   — highpass on the WET path only. The
//                                      single most important knob here: a BBD's
//                                      wet path rolls the bass off, which is
//                                      what stops the detuned voices from
//                                      wobbling the fundamental of a low E and
//                                      making the whole thing sound seasick.
//                                      At the range MINIMUM (20 Hz) the filter
//                                      is BYPASSED, not run at 20 Hz — that is
//                                      what makes the default bit-exact. The
//                                      discontinuity is inaudible: a 20 Hz
//                                      one-pole is within 0.01 dB of unity over
//                                      the guitar's range anyway.
//
// Invariants worth keeping:
//  - |lfo| <= 1 for EVERY shape, which is what lets the per-voice depth clamp
//    (`base - depth >= minTap`) stay a proof rather than a hope.
//  - latencySamples() == 0.
//  - The feedback path is tone-shaped (one-pole LP) + DC-blocked and its loop
//    gain is <= 0.9, and the sample pushed into the line is finite-guarded, so
//    a single NaN can never latch the recursion.
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

struct ChorusParams {
    float rateHz   = 0.8f;  // 0.1 .. 5    — LFO rate
    float depthMs  = 6.0f;  // 1  .. 15    — peak delay deviation
    float mix      = 0.5f;  // 0  .. 1     — dry/wet
    int   voices   = 2;     // 1  .. 3
    // ---- 0.18 (all default to the pre-0.18 behaviour) ----------------------
    float feedback = 0.0f;  // 0  .. 0.9   — BBD regeneration (0 = none)
    float spread   = 0.0f;  // 0  .. 1     — true-stereo LFO phase rotation
    int   lfoShape = 0;     // 0 sine (0.16) / 1 triangle / 2 hyper-triangle
    float wetHpfHz = 20.0f; // 20 .. 400   — wet-only HPF; 20 == OFF (bypassed)
};

class ChorusCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        numCh_ = numChannels < 1 ? 1 : numChannels;

        const double maxDelayMs =
            kBaseMs + (kMaxVoices - 1) * kVoiceSpreadMs + kMaxDepthMs + 5.0;
        const int maxSamps = (int)std::ceil(maxDelayMs * 0.001 * sr_) + 4;

        lines_.resize((size_t)numCh_);
        for (auto& l : lines_) l.prepare(maxSamps);

        // Wet-path highpass + the feedback path's tone/DC conditioning: one set
        // per channel (the voices are summed before either is applied).
        wetHp_.assign((size_t)numCh_, OnePole{});
        fbLp_.assign((size_t)numCh_, OnePole{});
        fbDc_.assign((size_t)numCh_, DcBlocker{});
        for (auto& f : fbLp_) f.setLowpass(sr_, kFbToneHz);
        for (auto& d : fbDc_) d.prepare(sr_);
        applyWetHpf();

        // Voice base delays, precomputed with the SAME expression the 0.16 hot
        // loop evaluated per sample (identical rounding, so identical taps).
        for (int v = 0; v < kMaxVoices; ++v)
            baseSamp_[(size_t)v] = (float)((kBaseMs + v * kVoiceSpreadMs) * 0.001 * sr_);

        buildSpreadTable();

        smDepth_.reset(sr_, 0.015f);
        // 30 ms: a new voice is a new read TAP as well as a level change,
        // and the same window `lfoShape` already crossfades over.
        smVoices_.reset(sr_, 0.030f);
        smVoices_.setCurrentAndTarget(1.0f);
        voicesPrev_ = p_.voices;
        smMix_.reset(sr_, 0.015f);
        smFeedback_.reset(sr_, 0.015f);
        smSpread_.reset(sr_, 0.050f);   // slower: a spread move GLIDES the taps
        smShape_.reset(sr_, 0.030f);    // shape crossfade (see evalLfo)
        smDepth_.setCurrentAndTarget((float)(p_.depthMs * 0.001 * sr_));
        smMix_.setCurrentAndTarget(p_.mix);
        smFeedback_.setCurrentAndTarget(p_.feedback);
        smSpread_.setCurrentAndTarget(p_.spread);
        smShape_.setCurrentAndTarget(1.0f);

        setRate(p_.rateHz);
        shapeFrom_ = shapeTo_ = clampShape(p_.lfoShape);

        reset();
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& f : wetHp_) f.reset();
        for (auto& f : fbLp_) f.reset();
        for (auto& d : fbDc_) d.reset();
        staggerPhases();
        smDepth_.setCurrentAndTarget((float)(p_.depthMs * 0.001 * sr_));
        smMix_.setCurrentAndTarget(p_.mix);
        smFeedback_.setCurrentAndTarget(p_.feedback);
        smSpread_.setCurrentAndTarget(p_.spread);
        shapeFrom_ = shapeTo_ = clampShape(p_.lfoShape);
        smShape_.setCurrentAndTarget(1.0f);
    }

    void setParams(const ChorusParams& p) {
        ChorusParams q = p;
        q.rateHz   = clampf(q.rateHz, 0.1f, 5.0f);
        q.depthMs  = clampf(q.depthMs, 1.0f, 15.0f);
        q.mix      = clampf(q.mix, 0.0f, 1.0f);
        q.voices   = q.voices < 1 ? 1 : (q.voices > kMaxVoices ? kMaxVoices : q.voices);
        setVoiceCount(q.voices);
        q.feedback = clampf(q.feedback, 0.0f, kMaxFeedback);
        q.spread   = clampf(q.spread, 0.0f, 1.0f);
        q.lfoShape = clampShape(q.lfoShape);
        q.wetHpfHz = clampf(q.wetHpfHz, kHpfOffHz, 400.0f);

        if (q.rateHz != p_.rateHz) setRate(q.rateHz);
        if (q.wetHpfHz != p_.wetHpfHz) { p_.wetHpfHz = q.wetHpfHz; applyWetHpf(); }
        // A shape switch would step every tap (sine and triangle disagree by up
        // to a full unit at the same phase). Crossfade the two shapes over
        // ~30 ms instead; once the fade lands, only the new shape is evaluated,
        // so the steady state costs exactly what a single shape costs.
        if (q.lfoShape != shapeTo_) {
            shapeFrom_ = currentShapeForFade();
            shapeTo_   = q.lfoShape;
            smShape_.setCurrentAndTarget(0.0f);
            smShape_.setTarget(1.0f);
        }

        p_ = q;
        smDepth_.setTarget((float)(q.depthMs * 0.001 * sr_));
        smMix_.setTarget(q.mix);
        smFeedback_.setTarget(q.feedback);
        smSpread_.setTarget(q.spread);
    }

    /// Crossfade the voice COUNT. Re-entrant the same way the shape switches
    /// in ringmod/vibrato/tremolo are: going back inside the window swaps the
    /// two ends and keeps the blend rather than discarding one in flight.
    void setVoiceCount(int want) {
        if (want == p_.voices) return;
        if (want == voicesPrev_ && smVoices_.isRamping()) {
            const float t = smVoices_.current();
            voicesPrev_ = p_.voices;
            smVoices_.setCurrentAndTarget(1.0f - t);
        } else {
            voicesPrev_ = p_.voices;
            smVoices_.setCurrentAndTarget(0.0f);
        }
        smVoices_.setTarget(1.0f);
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;

        // `voices` used to step TWO things at once: the wet normalisation
        // (1 -> 2 voices is 1.0 -> 0.5, a 6 dB cut in one sample) and the set of
        // read TAPS, since a new voice arrives at its own base delay at full
        // weight. Every other parameter in this core is smoothed, `lfoShape`
        // included. So the count crossfades: run max(prev, cur) voices, weight
        // the ones only ONE of the two counts has by the fade, and derive the
        // normalisation from the FADED count so the wet level stays put.
        const int   vLo  = voicesPrev_ < p_.voices ? voicesPrev_ : p_.voices;
        const int   V    = voicesPrev_ > p_.voices ? voicesPrev_ : p_.voices;
        const int   ch   = numChannels < numCh_ ? numChannels : numCh_;
        // Smallest modulated tap must stay >= this (headroom for the
        // Lagrange interpolator + never touching the just-written sample).
        const float minTap  = (float)(kMinTapMs * 0.001 * sr_);
        const bool  hpfOn   = hpfActive_;

        for (int n = 0; n < numFrames; ++n) {
            const float depthSamp = smDepth_.next();
            const float wet       = smMix_.next();
            const float dry       = 1.0f - wet;
            const float fb        = smFeedback_.next();
            const float sp        = smSpread_.next();
            const bool  fbOn      = (fb > 0.0f);

            // Shape crossfade weight (1 = fully on shapeTo_ — the steady state).
            // 0 -> 1 toward the NEW count. `edgeW` is the weight of the
            // voices that only one of the two counts has; at rest it is 1 and
            // vLo == V, so the expression below is the pre-fade one exactly.
            const float vf    = smVoices_.next();
            const float edgeW = (p_.voices >= voicesPrev_) ? vf : (1.0f - vf);
            const float faded = (float)vLo + edgeW * (float)(V - vLo);
            const float norm  = 1.0f / (faded > 1e-6f ? faded : 1.0f);

            float xf = 1.0f;
            if (shapeFrom_ != shapeTo_) {
                xf = smShape_.next();
                if (xf >= 1.0f) shapeFrom_ = shapeTo_;
            }

            // At spread == 0 (the default) every channel evaluates the LFO at the
            // SAME phase, so evaluate it once per voice and share it — that is
            // literally the 0.16 hot loop, cost included. Only a non-zero spread
            // pays for a per-channel evaluation.
            const bool spreadOn = (sp != 0.0f);
            float lvShared[kMaxVoices] = { 0.0f, 0.0f, 0.0f };
            if (!spreadOn)
                for (int v = 0; v < V; ++v)
                    lvShared[v] = evalLfo(phase_[(size_t)v], xf);

            for (int c = 0; c < ch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;

                const float x    = buf[n];
                const float sign = (c & 1) ? -1.0f : 1.0f; // decorrelate odd channels
                const std::array<float, kMaxVoices>& off = spreadOff_[(size_t)c];

                float wetSum = 0.0f;
                for (int v = 0; v < V; ++v) {
                    // Per-channel LFO phase rotation (spread). At spread == 0 the
                    // offset would be exactly 0.0f, so the shared value above IS
                    // the 0.16 sequence, unchanged.
                    float lv;
                    if (!spreadOn) {
                        lv = lvShared[v];
                    } else {
                        float p = phase_[(size_t)v] + sp * off[(size_t)v];
                        p -= std::floor(p);
                        lv = evalLfo(p, xf);       // |lv| <= 1, all shapes
                    }

                    const float baseSamp = baseSamp_[(size_t)v];
                    // Clamp the depth per voice so base - depth can never dip
                    // below minTap: keeps the sweep a pure (fold-free) FM.
                    float dep = depthSamp;
                    const float maxDep = baseSamp - minTap;
                    if (dep > maxDep) dep = maxDep;
                    float d = baseSamp + sign * dep * lv;
                    if (d < minTap) d = minTap;   // belt and suspenders
                    wetSum += (v < vLo ? 1.0f : edgeW) * lines_[(size_t)c].readAt(d);
                }

                if (voicesPrev_ != p_.voices && !smVoices_.isRamping())
                voicesPrev_ = p_.voices;   // the fade landed; retire the old count

            float wetOut = wetSum * norm;
                if (hpfOn) wetOut = wetHp_[(size_t)c].process(wetOut);

                // BBD regeneration: the wet sum, tone-shaped (the bucket line
                // loses highs on every pass) and DC-blocked, back into the line.
                // Loop gain <= 0.9 * |LP| * |DC| < 1 => bounded.
                float in = x;
                if (fbOn) {
                    const float shaped =
                        fbDc_[(size_t)c].process(fbLp_[(size_t)c].process(wetOut));
                    in = x + fb * shaped;
                    if (!std::isfinite(in)) in = 0.0f;   // never latch the loop
                }
                lines_[(size_t)c].push(in);

                buf[n] = dry * x + wet * wetOut;
            }

            // Advance the shared voice phases (same accumulator arithmetic the
            // 0.16 Lfo used, so the sine sequence is unchanged sample for sample).
            // kMaxVoices, not V: a voice that is not currently summed still has
            // to keep its PHASE, or switching it on hands it a value frozen at
            // whenever it was last used. The voices are deliberately fanned
            // apart in phase (see reset()), and that fan is the whole reason a
            // second voice thickens rather than flanges -- a frozen phase
            // destroys it, and makes the voice count history-dependent, which is
            // also what stops any before/after probe from ever converging. The
            // extra work is three adds per sample and the summed voices'
            // sequence is untouched.
            for (int v = 0; v < kMaxVoices; ++v) {
                float p = phase_[(size_t)v] + inc_;
                if (p >= 1.0f) p -= std::floor(p);
                if (p <  0.0f) p -= std::floor(p);
                phase_[(size_t)v] = p;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    // ---- LFO ---------------------------------------------------------------
    // Own phase accumulators rather than the shared Lfo class: `spread` needs to
    // evaluate the SAME accumulator at a per-channel phase offset (so the knob
    // can glide without ever jumping a phase), and the hyper-triangle is not one
    // of Lfo's shapes. The arithmetic below is a transcription of Lfo's, so at
    // the defaults (Sine, spread 0) the modulation is bit-identical to 0.16.

    static float shapeSine(float p) {
        return std::sin(static_cast<float>(kTwoPi) * p);
    }
    static float shapeTri(float p) {
        return 1.0f - 4.0f * std::fabs(p - 0.5f);         // -1 at 0, +1 at 0.5
    }
    // Hyper-triangle: the triangle pushed outward with a normalised tanh, so the
    // sweep CROSSES the centre faster (slope ~1.9x) and DWELLS at the turning
    // points (slope ~0.2x). Same peak excursion, but more of the cycle is spent
    // at full detune — the "more effect for the same depth" the hyper-triangular
    // LFO is chosen for, and much closer to a comparator-driven BBD sweep than a
    // sine. Smooth everywhere (C-infinity), bounded derivative, and |out| <= 1
    // by construction (kHyperNorm is a hair under 1/tanh(kHyperK)), so the
    // per-voice depth clamp stays a proof.
    // NOT ADAA (W4-D66), and it needs none: `p` is an LFO PHASE, so this tanh
    // shapes a sub-audio modulator, not audio. Its harmonics are at a few Hz
    // and there is nothing to fold.
    static float shapeHyperTri(float p) {
        const float t = shapeTri(p);
        return std::tanh(kHyperK * t) * kHyperNorm;
    }
    static float shapeAt(int shape, float p) {
        switch (shape) {
            case 1:  return shapeTri(p);
            case 2:  return shapeHyperTri(p);
            default: return shapeSine(p);
        }
    }
    // Steady state (shapeFrom_ == shapeTo_) evaluates ONE shape — no extra cost.
    float evalLfo(float p, float xf) const {
        if (shapeFrom_ == shapeTo_) return shapeAt(shapeTo_, p);
        const float a = shapeAt(shapeFrom_, p);
        const float b = shapeAt(shapeTo_, p);
        return a + (b - a) * xf;
    }
    // The shape a fade should start FROM when a second switch lands mid-fade:
    // just the shape we were heading to (restarting from the visible endpoint is
    // continuous enough at 30 ms and keeps this branch trivial).
    int currentShapeForFade() const {
        return (smShape_.current() >= 0.5f) ? shapeTo_ : shapeFrom_;
    }

    void setRate(float hz) {
        inc_ = static_cast<float>(static_cast<double>(hz) / sr_);
    }
    // Give each voice a distinct starting phase (0, 1/3, 2/3).
    void staggerPhases() {
        for (int v = 0; v < kMaxVoices; ++v)
            phase_[(size_t)v] = (float)v / (float)kMaxVoices;
    }

    // ---- spread ------------------------------------------------------------
    // Per-(channel, voice) phase rotation, in CYCLES, at spread == 1. Channel 0
    // is always 0, so mono can never be "widened" and the left channel is always
    // the reference. Each further channel is rotated a quarter cycle, and its
    // voices are additionally fanned apart — that fan is what makes the wet
    // signals genuinely different per channel instead of mirror images, which is
    // all the old sign flip could do.
    void buildSpreadTable() {
        spreadOff_.assign((size_t)numCh_, std::array<float, kMaxVoices>{});
        for (int c = 0; c < numCh_; ++c)
            for (int v = 0; v < kMaxVoices; ++v)
                spreadOff_[(size_t)c][(size_t)v] =
                    kChanRotCycles * (float)c + kVoiceFanCycles * (float)(v * c);
    }

    void applyWetHpf() {
        hpfActive_ = (p_.wetHpfHz > kHpfOffHz);
        if (!hpfActive_) return;                       // bypassed at the minimum
        for (auto& f : wetHp_) f.setHighpass(sr_, (double)p_.wetHpfHz);
    }

    static int clampShape(int s) { return s < 0 ? 0 : (s > 2 ? 2 : s); }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    static constexpr int    kMaxVoices     = 3;
    static constexpr double kBaseMs        = 12.0; // centre of the 7-25 ms window
    static constexpr double kVoiceSpreadMs = 4.0;  // per-voice base-delay offset
    static constexpr double kMaxDepthMs    = 15.0;
    static constexpr double kMinTapMs      = 1.0;  // shortest allowed read tap

    static constexpr float  kMaxFeedback   = 0.9f;
    static constexpr double kFbToneHz      = 6000.0; // BBD loses highs per pass
    static constexpr float  kHpfOffHz      = 20.0f;  // == the range min == OFF

    // tanh(1.8) = 0.9468060. kHyperNorm is a hair BELOW 1/tanh(1.8) = 1.0561756
    // on purpose: it pins |hyper-tri| to 0.9999997 at the triangle's peaks, so
    // every shape shares the same peak excursion and none can exceed 1.
    static constexpr float  kHyperK        = 1.8f;
    static constexpr float  kHyperNorm     = 1.05617f;

    static constexpr float  kChanRotCycles = 0.25f;        // 90 deg per channel
    static constexpr float  kVoiceFanCycles = 1.0f / 12.0f; // 30 deg per voice step

    std::vector<InterpolatedDelayLine> lines_;
    std::vector<OnePole>   wetHp_;   // wet-only highpass (off at the range min)
    std::vector<OnePole>   fbLp_;    // feedback tone
    std::vector<DcBlocker> fbDc_;    // feedback DC guard
    std::vector<std::array<float, kMaxVoices>> spreadOff_;

    ChorusParams p_;
    SmoothedValue smVoices_;   // 0 = voicesPrev_, 1 = p_.voices
    int voicesPrev_ = 2;
    SmoothedValue smDepth_, smMix_, smFeedback_, smSpread_, smShape_;
    std::array<float, kMaxVoices> phase_{};
    std::array<float, kMaxVoices> baseSamp_{};
    float inc_ = 0.0f;
    int   shapeFrom_ = 0, shapeTo_ = 0;
    bool  hpfActive_ = false;
    double sr_ = 44100.0;
    int numCh_ = 2;
};

} // namespace dcdsp
