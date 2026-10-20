// dcdsp_subharmonic.h — monophonic SUB-OCTAVE SYNTH (dcdsp core).
//
// ---------------------------------------------------------------------------
// This is NOT a second octaver. Read dcdsp_octaver.h first; the two are
// different instruments and both ship on purpose:
//
//   octaver     = the analogue flip-flop divider (a Boss OC-2). A comparator
//                 clocks a binary divider; the raw square's amplitude is the
//                 input envelope; a one-pole "tone" knob is the entire voice.
//                 It sounds like a divider because it IS one.
//
//   subharmonic = a small monophonic SYNTHESIZER the guitar plays. A period
//                 tracker phase-LOCKS a continuous oscillator to the string, so
//                 the sub is a band-limited (polyBLEP) square / saw / sine, not a
//                 divider's state bit; and it has a voice of its own — a resonant
//                 TPT low-pass with a FILTER ENVELOPE (picking opens it), its own
//                 ATTACK (0 ms = pluck, 300 ms = the bowed auto-swell sub) and a
//                 soft-clipping output stage.
//
// Musically: octaver = analogue sub under the guitar. subharmonic = SYNTH BASS
// under the guitar. The parameter sets barely overlap.
// ---------------------------------------------------------------------------
//
// Contract: namespace dcdsp, C++17, header-only, zero framework deps. Allocation
// happens ONLY in prepare(); process() never allocates, locks, or throws.
// Output is ALWAYS finite and bounded: the synth voice passes through an ADAA
// tanh (|y| < 1 by the mean value theorem) before the output VCA, so the sub can
// never exceed the tracked note's own amplitude, whatever the filter resonance
// does; a final clamp guards the sum.
//
// The oscillator, filter and envelope are MONO (one voice, driven by a mono
// detector) and the result is added to every channel: a sub-bass belongs in the
// middle, and one voice is also the cheapest thing that can be phase-coherent.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_pitch.h"

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

namespace subharmonic_detail {

// polyBLEP residual for a discontinuity at phase 0 (Valimaki/Huovilainen).
// Removes the first-order step aliasing a naive saw/square sprays — the whole
// reason this core does not simply reuse the divider's square.
inline float polyBlep(float t, float dt) {
    if (dt <= 0.f) return 0.f;
    if (t < dt) {
        const float u = t / dt;
        return u + u - u * u - 1.f;
    }
    if (t > 1.f - dt) {
        const float u = (t - 1.f) / dt;
        return u * u + u + u + 1.f;
    }
    return 0.f;
}

inline float blepSaw(float phase, float dt) {
    return 2.f * phase - 1.f - polyBlep(phase, dt);
}

inline float blepSquare(float phase, float dt) {
    float y = (phase < 0.5f) ? 1.f : -1.f;
    y += polyBlep(phase, dt);
    float p2 = phase + 0.5f;
    if (p2 >= 1.f) p2 -= 1.f;
    y -= polyBlep(p2, dt);
    return y;
}

inline float sineWave(float phase) {
    return std::sin(static_cast<float>(kTwoPi) * phase);
}

} // namespace subharmonic_detail

struct SubharmonicParams {
    // Level of the -1 octave synth voice. [0, 1]
    float sub1 = 0.7f;
    // Level of the -2 octave synth voice. [0, 1]
    float sub2 = 0.0f;
    // Oscillator waveform: 0 = square, 1 = saw, 2 = sine. Anything else = square.
    int   waveform = 0;
    // Base cutoff of the synth's resonant low-pass, Hz. [100, 4000]
    float cutoffHz = 900.0f;
    // Filter resonance. [0, 1] -> Q 0.7 .. 7.7
    float resonance = 0.25f;
    // How far picking dynamics open the filter above the base cutoff. [0, 1]
    float envAmount = 0.4f;
    // Synth attack. 0 ms = pluck; 300 ms = the bowed auto-swell sub. [0, 300]
    float attackMs = 5.0f;
    // Dry (guitar) blend. [0, 1]
    float dry = 1.0f;
};

class SubharmonicCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxCh_ = (numChannels > 0) ? numChannels : 1;

        tracker_.prepare(sr_);
        voiceEnv_.prepare(sr_);
        voiceEnv_.setTimes(attackMs_, kVoiceReleaseMs);
        dc_.prepare(sr_, 18.0);
        applyFilter();
        // The three levels RAMP (kLevelRampSeconds). As per-block constants a
        // slider drag zippered and `dry` 1 -> 0 muted the guitar in one sample.
        sub1S_.reset(sr_, kLevelRampSeconds);
        sub2S_.reset(sr_, kLevelRampSeconds);
        dryS_.reset(sr_, kLevelRampSeconds);
        sub1S_.setTarget(sub1_);
        sub2S_.setTarget(sub2_);
        dryS_.setTarget(dry_);
        sub1S_.snapToTarget();   // a freshly prepared core starts AT its levels
        sub2S_.snapToTarget();
        dryS_.snapToTarget();
        waveFade_.reset(sr_, kWaveFadeSeconds);
        waveFade_.setTarget(1.0f);
        waveFade_.snapToTarget();   // ...and AT its shape, not fading into it
        wavePrev_ = waveform_;
        reset();
    }

    void reset() {
        tracker_.reset();
        voiceEnv_.reset();
        svf_.reset();
        dc_.reset();
        phase1_ = 0.f;
        phase2_ = 0.f;
        // The increments are periods in SAMPLES, so they belong to the rate. The
        // tracker only rewrites them once it is confident again, so carrying
        // them across a 48 -> 96 kHz prepare ran both oscillators an octave off
        // (and gave polyBlep the wrong dt) until then.
        inc1_ = 0.f;
        inc2_ = 0.f;
        satState_ = 0.f;
    }

    void setParams(const SubharmonicParams& p) {
        sub1_      = clamp(p.sub1, 0.f, 1.f);
        sub2_      = clamp(p.sub2, 0.f, 1.f);
        setWaveform((p.waveform >= 0 && p.waveform <= 2) ? p.waveform : 0);
        cutoffHz_  = clamp(p.cutoffHz, 100.f, 4000.f);
        resonance_ = clamp(p.resonance, 0.f, 1.f);
        envAmount_ = clamp(p.envAmount, 0.f, 1.f);
        attackMs_  = clamp(p.attackMs, 0.f, 300.f);
        dry_       = clamp(p.dry, 0.f, 1.f);

        sub1S_.setTarget(sub1_);
        sub2S_.setTarget(sub2_);
        dryS_.setTarget(dry_);

        voiceEnv_.setTimes(attackMs_, kVoiceReleaseMs);
        applyFilter();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int C = std::min(numChannels, maxCh_);
        if (C <= 0) return;

        const float invC = 1.f / static_cast<float>(C);

        for (int n = 0; n < numFrames; ++n) {
            // ---- mono detector: one string, one sub voice --------------------
            float mono = 0.f;
            for (int c = 0; c < C; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                float in = buf[n];
                if (!std::isfinite(in)) { in = 0.f; buf[n] = 0.f; }
                mono += in;
            }
            mono *= invC;

            const bool rise = tracker_.process(mono);
            const bool have = tracker_.confident();

            // ---- oscillator: phase-locked to the string ----------------------
            if (have) {
                const float period = tracker_.periodSamples();
                inc1_ = 1.f / (2.f * period);   // -1 octave: one cycle per 2 periods
                inc2_ = 1.f / (4.f * period);   // -2 octaves
            }

            if (rise && have) {
                // A string rising edge lands on phase 0 or 0.5 of the -1 octave
                // voice (0, .25, .5, .75 of the -2). Nudge toward the nearer grid
                // point instead of snapping — a snap is a click, a nudge is a PLL.
                lockPhase(phase1_, 0.5f);
                lockPhase(phase2_, 0.25f);
            }

            // A waveform switch is a FULL-SCALE step: square and saw differ by
            // up to 2.0 at one phase and that difference is multiplied by
            // sub1S_/sub2S_ straight into every channel, with nothing damping
            // it. The oscillators are stateless given the phase, so the fix is
            // one extra call each — the same crossfade ringmod and vibrato use.
            const float wt = waveFade_.next();
            float osc1, osc2;
            if (wt >= 1.0f) {
                osc1 = waveAt(waveform_, phase1_, inc1_);
                osc2 = waveAt(waveform_, phase2_, inc2_);
            } else {
                osc1 = (1.f - wt) * waveAt(wavePrev_, phase1_, inc1_)
                     +        wt  * waveAt(waveform_, phase1_, inc1_);
                osc2 = (1.f - wt) * waveAt(wavePrev_, phase2_, inc2_)
                     +        wt  * waveAt(waveform_, phase2_, inc2_);
            }

            phase1_ += inc1_;
            phase1_ -= std::floor(phase1_);
            phase2_ += inc2_;
            phase2_ -= std::floor(phase2_);

            // ---- envelope: the note's level, shaped by the synth attack ------
            const float target   = have ? tracker_.amplitude() : 0.f;
            const float voiceAmp = voiceEnv_.process(target);

            // ---- filter: base cutoff + picking envelope ----------------------
            const float envNorm = std::min(1.f, voiceAmp * 6.f);
            double cutoff = static_cast<double>(cutoffHz_) *
                            (1.0 + static_cast<double>(envAmount_) * 6.0 *
                                   static_cast<double>(envNorm));
            cutoff = clampCutoff(sr_, cutoff);
            svf_.set(sr_, cutoff, q_);

            float voice = sub1S_.next() * osc1 + sub2S_.next() * osc2;   // |voice| <= 2
            svf_.process(voice);
            float filtered = svf_.lp() * resComp_;
            if (!std::isfinite(filtered)) { filtered = 0.f; svf_.reset(); }

            // Soft-clipping synth output stage. |adaaTanh| < 1 strictly (mean
            // value theorem on log(cosh)), so the sub can never be louder than
            // the note that produced it, whatever the resonance did upstream.
            float shaped = adaaTanh(filtered, satState_);
            float sub    = dc_.process(shaped * voiceAmp * kSubGain);
            if (!std::isfinite(sub)) { sub = 0.f; dc_.reset(); satState_ = 0.f; }

            // ---- sum into every channel --------------------------------------
            const float dryG = dryS_.next();
            for (int c = 0; c < C; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                float out = dryG * buf[n] + sub;
                if (!std::isfinite(out)) out = 0.f;
                buf[n] = clamp(out, -16.f, 16.f);
            }
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the -1 octave oscillator increment, a period in SAMPLES, so
    // a gate case can assert a prepare() at a new rate does not carry it over.
    float osc1IncForTest() const { return inc1_; }

private:
    static float clamp(float v, float lo, float hi) {
        if (!(v == v)) return lo;                 // NaN -> low bound
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Pull `phase` toward the nearest multiple of `grid`, gently.
    static void lockPhase(float& phase, float grid) {
        const float nearest = std::floor(phase / grid + 0.5f) * grid;
        phase -= kLockGain * (phase - nearest);
        phase -= std::floor(phase);
        if (!std::isfinite(phase)) phase = 0.f;
    }

    static float waveAt(int w, float phase, float inc) {
        switch (w) {
            case 1:  return subharmonic_detail::blepSaw(phase, inc);
            case 2:  return subharmonic_detail::sineWave(phase);
            default: return subharmonic_detail::blepSquare(phase, inc);
        }
    }

    /// Switch shape over kWaveFadeSeconds instead of at one sample. Re-entrant:
    /// flipping BACK inside the window swaps the two ends and keeps the blend
    /// where it is, rather than discarding a fade in flight (which would itself
    /// be a step of up to the full shape difference).
    void setWaveform(int want) {
        if (want == waveform_) return;
        if (waveFade_.isRamping() && want == wavePrev_) {
            const float t = waveFade_.current();
            wavePrev_ = waveform_;
            waveform_ = want;
            waveFade_.setCurrentAndTarget(1.0f - t);
        } else {
            wavePrev_ = waveform_;
            waveform_ = want;
            waveFade_.setCurrentAndTarget(0.0f);
        }
        waveFade_.setTarget(1.0f);
    }

    void applyFilter() {
        q_ = 0.7 + static_cast<double>(resonance_) * 7.0;   // 0.7 .. 7.7
        // Resonance eats the body (the physical law every ladder filter obeys):
        // compensating the passband keeps a maxed resonance a ~+9 dB peak instead
        // of a ~+18 dB one, so the output stage colours rather than pins.
        resComp_ = static_cast<float>(1.0 / (1.0 + 0.25 * (q_ - 0.7)));
    }

    static constexpr float kSubGain        = 1.0f;
    static constexpr float kLockGain       = 0.12f;   // PLL correction per edge
    static constexpr float kVoiceReleaseMs = 180.0f;  // sub sustains past the string
    static constexpr float kLevelRampSeconds = 0.010f;
    /// The shape crossfade, the same 10 ms ringmod and vibrato use.
    static constexpr float kWaveFadeSeconds  = 0.010f;

    double sr_    = 48000.0;
    int    maxCh_ = 2;

    float sub1_      = 0.7f;
    float sub2_      = 0.0f;
    int   waveform_  = 0;   // the shape in force
    int   wavePrev_  = 0;   // the one being faded OUT while waveFade_ ramps
    float cutoffHz_  = 900.0f;
    float resonance_ = 0.25f;
    float envAmount_ = 0.4f;
    float attackMs_  = 5.0f;
    float dry_       = 1.0f;

    SmoothedValue sub1S_, sub2S_, dryS_;
    SmoothedValue waveFade_;   // 0 = wavePrev_, 1 = waveform_

    double q_       = 2.45;
    float  resComp_ = 0.7f;

    detail::PeriodTracker tracker_{};
    EnvelopeFollower      voiceEnv_{};
    StateVariableTPT      svf_{};
    DcBlocker             dc_{};

    float phase1_   = 0.f, phase2_ = 0.f;
    float inc1_     = 0.f, inc2_   = 0.f;
    float satState_ = 0.f;
};

} // namespace dcdsp
