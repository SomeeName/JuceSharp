// dcdsp_basssynth.h — BassSynthCore: the guitar/bass synth tracker (a Boss
// SYB, an Electro-Harmonix Bass Micro Synth, the synth voice of a Roland GR).
//
// TOPOLOGY, and it is a whole small synth voice:
//
//   input --> pitch tracker --> portamento --> band-limited oscillator
//                \                                     |
//                 \--> level envelope --> filter env -->+--> SVF --> VCA --> mix
//
// The input is never the SOURCE of the synth tone, only its CONTROLLER: the
// oscillator is generated, which is why this tracks a clean neck pickup far
// better than it tracks a distorted one.
//
// EASY TO GET WRONG, three ways:
//  * A NAIVE saw or square aliases audibly at these fundamentals — a step is
//    flat past Nyquist — and the fold-back is INHARMONIC, which is the "cheap
//    synth" sound nobody wants. PolyBLEP is not optional here.
//  * The oscillator increment must GLIDE. Splicing it on a new note is a click
//    in a waveform that has no zero crossings to hide behind.
//  * The level envelope DECAYS LINEARLY to exact zero, not exponentially. An
//    exponential release is still 5 % of full scale after three time constants,
//    so a "silent" synth sat 26 dB down over an empty bar.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif
#include "dcdsp_detail_pitchgear.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace dcdsp {

struct BassSynthParams {
    float waveform  = 0.0f;    // 0 Square, 1 Saw, 2 Sub-Sine, 3 PWM
    float octave    = 1.0f;    // 0 = -2, 1 = -1, 2 = 0
    float cutoffHz  = 800.0f;  // filter corner          [40 .. 8000]
    float resonance = 0.4f;    // filter Q               [0 .. 1]
    float envAmount = 0.6f;    // envelope -> cutoff     [0 .. 1]
    float decayMs   = 300.0f;  // level decay            [10 .. 2000]
    float glideMs   = 20.0f;   // portamento             [0 .. 300]
    float dryMix    = 0.3f;    // the original signal    [0 .. 1]
    float synthMix  = 0.7f;    // the synth voice        [0 .. 1]
};

class BassSynthCore {
public:
    /// How far the envelope may open the filter above its knob setting.
    static constexpr double kEnvSweepHz = 6000.0;
    /// Control rate for the filter coefficients: one design every 16 samples
    /// (3 kHz at 48 kHz). A std::tan per sample would dominate the whole core,
    /// and 3 kHz is far above anything a cutoff envelope contains.
    static constexpr int kCoeffInterval = 16;
    /// PWM sweep, the classic slow duty-cycle drift.
    static constexpr double kPwmHz = 0.3;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        nch_ = std::min(std::max(numChannels, 1), kMaxChannels);
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 4096;

        tracker_.prepare(sr_);
        osc_.prepare(sr_);
        pwm_.prepare(sr_);
        pwm_.setFrequency((float) kPwmHz);
        pwm_.setShape(Lfo::Sine);
        inEnv_.prepare(sr_);
        // Rate-derived, like every other in-loop corner in the wave. The voice
        // is SYNTHESISED, so it has no reason to be DC-free: a pulse of duty d
        // carries 2d - 1 of DC all by itself.
        dc_.prepare(sr_, kDcCornerHz);
        inEnv_.setTimes(3.0f, 8.0f);
        svf_.reset();

        freq_.reset(sr_, 0.020f);
        shapeBlend_.reset(sr_, kRampSeconds);
        dry_.reset(sr_, kRampSeconds);
        wet_.reset(sr_, kRampSeconds);

        applyDerived();
        freq_.setCurrentAndTarget(0.0f);
        shapeBlend_.snapToTarget();
        dry_.snapToTarget();
        wet_.snapToTarget();
        reset();
    }

    void reset() {
        tracker_.reset();
        osc_.reset();
        pwm_.reset();
        inEnv_.reset();
        svf_.reset();
        dc_.reset();
        level_ = 0.0f;
        coeffCount_ = 0;
        lastTrackedHz_ = -1.0f;
    }

    void setParams(const BassSynthParams& p) {
        p_ = p;
        applyDerived();
    }

    /// ZERO: the dry passes through untouched and the synth voice is
    /// GENERATED, not delayed. The tracker's analysis lag means a new note
    /// takes a few tens of milliseconds to be recognised, which is a tracking
    /// property and not something branch PDC can or should compensate.
    int latencySamples() const { return 0; }

    // ---- test surfaces ----------------------------------------------------
    float trackedHz() const { return tracker_.frequencyHz(); }
    float oscHz() const { return freq_.current(); }
    float levelEnvelope() const { return level_; }
    int trackerDecimation() const { return tracker_.decimation(); }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // oversized: skipped WHOLE
        const int nch = std::min(numChannels, nch_);
        const float invN = 1.0f / (float) nch;

        for (int i = 0; i < numFrames; ++i) {
            // ---- 1) controller: track and follow the input on the MONO SUM.
            float mono = 0.0f;
            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                float x = buf[i];
                if (!std::isfinite(x)) { x = 0.0f; buf[i] = 0.0f; }
                mono += x;
            }
            mono *= invN;
            tracker_.process(mono);
            retarget();

            const float drive = inEnv_.process(mono);
            // Instant attack, LINEAR decay to exact zero (see the header).
            if (drive > level_) level_ = drive;
            else level_ = std::max(0.0f, level_ - decayStep_);
            const float env = (level_ > kGateFloor) ? std::min(1.0f, level_ * kEnvScale) : 0.0f;

            // ---- 2) the voice.
            const float hz = freq_.next();
            const float blend = shapeBlend_.next();
            osc_.advance(hz);
            const float pw = 0.5f + 0.35f * pwm_.process();
            float raw = osc_.value(toShape_, pw);
            if (blend < 0.999f)
                raw = blend * raw + (1.0f - blend) * osc_.value(fromShape_, pw);

            if (++coeffCount_ >= kCoeffInterval) {
                coeffCount_ = 0;
                const double fc = (double) cutoff_ + (double) envAmount_ * (double) env * kEnvSweepHz;
                svf_.set(sr_, fc, q_);
            }
            svf_.process(raw);
            float voice = svf_.lp() * env;
            // THE VOICE IS GATED UNTIL THERE IS A NOTE. `freq_` starts at 0 and
            // AutocorrTracker needs a full decimated window plus a hop (~45 ms)
            // before it is confident, while inEnv_'s attack is 3 ms -- so `env`
            // is already at 1 while PolyBlepOsc::value is frozen at a CONSTANT
            // (pulse(0, 0, 0.5) is exactly +1, saw(0, 0) exactly -1). For the
            // first note, and for the whole of a strum that never becomes
            // confident, the synth emitted a DC thump at up to `synthMix`.
            if (!(hz >= kMinVoiceHz)) voice = 0.0f;
            // …and even with a note, `pw` sweeps [0.15, 0.85] at 0.3 Hz, so the
            // PWM waveform carries up to +/-0.7 of LFO-MODULATED DC. Nothing
            // else in this core removes it, and every other core in the wave
            // that synthesises or recirculates rate-derives a blocker.
            voice = dc_.process(voice);
            if (!std::isfinite(voice)) { voice = 0.0f; svf_.reset(); }
            voice = std::max(-4.0f, std::min(4.0f, voice));

            // ---- 3) mix. One walk per FRAME, above the channel loop.
            const float gd = dry_.next();
            const float gw = wet_.next();
            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                float out = gd * buf[i] + gw * voice;
                if (!std::isfinite(out)) out = 0.0f;
                buf[i] = std::max(-16.0f, std::min(16.0f, out));
            }
        }
    }

private:
    /// Aim the oscillator at the tracked note, transposed. Holds the last note
    /// while the tracker is unsure — a chord has no one period, and chasing its
    /// noise is worse than staying put.
    void retarget() {
        const float f = tracker_.frequencyHz();
        if (!tracker_.confident() || !(f > 1.0f)) return;
        if (f == lastTrackedHz_) return;
        lastTrackedHz_ = f;
        const float target = f * octaveFactor_;
        freq_.setTarget(detail::clampf(target, 0.0f, (float) (sr_ * 0.45), 0.0f));
    }

    void applyDerived() {
        const int shape = (int) std::lround(detail::clampf(p_.waveform, 0.0f, 3.0f, 0.0f));
        const auto want = static_cast<detail::PolyBlepOsc::Shape>(shape);
        if (want != toShape_) {
            // A waveform switch is a FULL-SCALE discontinuity (square and sine
            // differ by 1.0 at phase 0), so the two shapes are crossfaded off
            // ONE phase accumulator. Flipping BACK inside the window swaps the
            // two ends and continues from 1 - t (dcdsp_tzflanger.h's applyShape)
            // -- restarting at 0 discards the in-flight blend, which jumps the
            // output by up to the full difference between the two shapes.
            if (want == fromShape_ && shapeBlend_.isRamping()) {
                const float t = shapeBlend_.current();
                fromShape_ = toShape_;
                toShape_   = want;
                shapeBlend_.setCurrentAndTarget(1.0f - t);
            } else {
                fromShape_ = toShape_;
                toShape_   = want;
                shapeBlend_.setCurrentAndTarget(0.0f);
            }
            shapeBlend_.setTarget(1.0f);
        }

        const int oct = (int) std::lround(detail::clampf(p_.octave, 0.0f, 2.0f, 1.0f));
        octaveFactor_ = (oct == 0) ? 0.25f : ((oct == 1) ? 0.5f : 1.0f);
        // An octave switch is a pitch change, so it takes the portamento rather
        // than stepping the increment.
        if (lastTrackedHz_ > 0.0f)
            freq_.setTarget(detail::clampf(lastTrackedHz_ * octaveFactor_,
                                           0.0f, (float) (sr_ * 0.45), 0.0f));

        cutoff_ = detail::clampf(p_.cutoffHz, 40.0f, 8000.0f, 800.0f);
        const float res = detail::clampf(p_.resonance, 0.0f, 1.0f, 0.4f);
        q_ = 0.707 + 8.0 * (double) res;
        envAmount_ = detail::clampf(p_.envAmount, 0.0f, 1.0f, 0.6f);
        coeffCount_ = kCoeffInterval;    // re-design on the next sample

        // RATE-owned: a decay in milliseconds is a per-sample step.
        const float decayMs = detail::clampf(p_.decayMs, 10.0f, 2000.0f, 300.0f);
        decayStep_ = (float) (1.0 / (sr_ * (double) decayMs / 1000.0));

        const float glideMs = detail::clampf(p_.glideMs, 0.0f, 300.0f, 20.0f);
        const float pending = freq_.target();
        freq_.reset(sr_, std::max(kMinGlideMs, glideMs) * 0.001f);
        freq_.setTarget(pending);

        dry_.setTarget(detail::clampf(p_.dryMix, 0.0f, 1.0f, 0.3f));
        wet_.setTarget(detail::clampf(p_.synthMix, 0.0f, 1.0f, 0.7f));
    }

    static constexpr int kMaxChannels = 8;
    static constexpr float kRampSeconds = 0.010f;
    /// Below this the tracker has not answered yet and the oscillator is frozen
    /// at a constant -- the low B of a 5-string is 30.9 Hz, so nothing musical
    /// is inside this gate.
    static constexpr float kMinVoiceHz = 20.0f;
    /// The synth path's DC corner. Below the lowest note the octave switch can
    /// produce (a low E transposed two octaves down is 20.6 Hz), so it removes
    /// the offset without touching the fundamental.
    static constexpr double kDcCornerHz = 15.0;
    /// Even "no glide" keeps the increment continuous over one millisecond.
    static constexpr float kMinGlideMs = 1.0f;
    static constexpr float kGateFloor = 3.0e-4f;   // ~-70 dBFS
    static constexpr float kEnvScale = 4.0f;       // a picked note reaches full

    BassSynthParams p_{};
    double sr_ = 48000.0;
    int nch_ = 2;
    int maxBlock_ = 4096;

    detail::AutocorrTracker tracker_;
    detail::PolyBlepOsc osc_;
    Lfo pwm_;
    DcBlocker dc_;
    EnvelopeFollower inEnv_;
    StateVariableTPT svf_;

    SmoothedValue freq_, shapeBlend_, dry_, wet_;
    detail::PolyBlepOsc::Shape fromShape_ = detail::PolyBlepOsc::Square;
    detail::PolyBlepOsc::Shape toShape_ = detail::PolyBlepOsc::Square;
    float octaveFactor_ = 0.5f;
    float cutoff_ = 800.0f;
    double q_ = 3.9;
    float envAmount_ = 0.6f;
    float decayStep_ = 0.0f;
    float level_ = 0.0f;
    float lastTrackedHz_ = -1.0f;
    int coeffCount_ = 0;
};

} // namespace dcdsp
