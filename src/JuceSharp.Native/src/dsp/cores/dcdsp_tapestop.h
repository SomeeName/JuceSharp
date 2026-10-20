// dcdsp_tapestop.h — the DJ tape / turntable stop (dcdsp core).
//
// Hit the switch and the tape brakes: playback speed AND pitch decay together to
// zero, and the sound sags into silence. Release it and the reel spins back up
// through the same pitch ramp.
//
// Mechanically this is a delay line read at a VARIABLE RATE. The write head
// always advances one sample per sample; the read head advances by `rate`, so
// the read delay grows by (1 - rate) every sample and the playback pitch is
// exactly `rate`. Braking is therefore a single scalar ramp — no pitch shifter,
// no windowing, no latency floor. (This is also the reason a tape stop can be
// done HONESTLY in real time on a live guitar while a spectral freeze cannot:
// down-rate reading only ever looks BACKWARD into audio that is already written.)
//
// THE RESYNC — the one place we deliberately part company with a real turntable.
// A physical deck that stops for two seconds and spins back up leaves you two
// seconds behind, forever. A guitar rig must not: you would be playing into a
// two-second-late monitor for the rest of the song. So when the spin-up reaches
// full speed, the core performs a 30 ms EQUAL-POWER CROSSFADE from the delayed
// read back to the live signal and snaps the read head home. On a guitar that is
// inaudible; it is what makes the pedal usable more than once per song.
//
// Contract: namespace dcdsp, C++17, header-only, zero framework deps. Allocation
// happens ONLY in prepare(); process() never allocates, locks, or throws. The
// output is a bounded rescaling of buffered input — no feedback path exists, so
// there is nothing that can diverge; non-finite input is scrubbed on write.
//
// LATENCY: 2 samples, always (the read head parks at kBaseDelay so the Lagrange
// interpolator always has its full 4-tap window; at delay 2.0 the fractional part
// is 0 and the read is BIT-EXACT). The dry path is taken from the same delay line
// at the same 2 samples, so dry and wet never comb against each other.
#pragma once

#include "dcdsp_primitives.h"

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct TapeStopParams {
    // 0 = Run (tape at speed), 1 = Stop (brake engaged). The pedal's footswitch.
    int trigger = 0;
    // Brake time: how long the tape takes to reach a standstill. [50, 3000] ms
    float stopMs = 600.0f;
    // Spin-up time: how long it takes to come back to speed. [50, 3000] ms
    float startMs = 400.0f;
    // Brake shape: 0 = linear deceleration, 1 = cubic (holds speed, then dives).
    // [0, 1]
    float curve = 0.5f;
    // Dry/wet blend. 1 = the tape IS the signal (what you almost always want).
    float mix = 1.0f;
};

class TapeStopCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxCh_ = (numChannels > 0) ? numChannels : 1;

        // A full brake + a full spin-up can each accumulate up to half their
        // duration of delay (the integral of 1 - rate over the ramp), so a 3 s
        // brake plus a 3 s spin-up needs ~3 s. Four gives headroom.
        maxDelay_ = static_cast<int>(kMaxDelaySeconds * sr_) + 8;

        lines_.clear();
        lines_.resize(static_cast<std::size_t>(maxCh_));
        for (auto& l : lines_) {
            l.prepare(maxDelay_);
            l.setInterpolation(InterpolatedDelayLine::Lagrange3);
        }
        resyncSamples_ = std::max(8, static_cast<int>(kResyncMs * 0.001 * sr_));
        // `mix` RAMPS (kMixRampSeconds); as a per-block constant a mix drag
        // stepped the whole balance between the dry and a pitch-varying wet.
        mixSmooth_.reset(sr_, kMixRampSeconds);
        mixSmooth_.setTarget(mix_);
        mixSmooth_.snapToTarget();   // a freshly prepared reel starts AT its mix
        curveSmooth_.reset(sr_, kCurveRamp);
        curveSmooth_.setTarget(curve_);
        curveSmooth_.snapToTarget();   // ...and AT its brake shape
        reset();
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        state_      = Run;
        rate_       = 1.f;
        readDelay_  = kBaseDelay;
        phase_      = 0.f;
        resyncPos_  = 0;
        lastTrigger_ = 0;
    }

    void setParams(const TapeStopParams& p) {
        const int trig = (p.trigger >= 1) ? 1 : 0;
        stopMs_  = clamp(p.stopMs, 50.f, 3000.f);
        startMs_ = clamp(p.startMs, 50.f, 3000.f);
        curve_   = clamp(p.curve, 0.f, 1.f);
        curveSmooth_.setTarget(curve_);
        mix_     = clamp(p.mix, 0.f, 1.f);
        mixSmooth_.setTarget(mix_);

        if (trig != lastTrigger_) {
            if (trig == 1) {
                // Engage the brake from wherever the reel currently is.
                phase_ = 1.f - rate_;
                state_ = Stopping;
            } else {
                // Release: spin back up from wherever it currently is.
                phase_ = rate_;
                state_ = Starting;
            }
            lastTrigger_ = trig;
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int C = std::min(numChannels, static_cast<int>(lines_.size()));
        if (C <= 0) return;

        const float stopInc  = static_cast<float>(1000.0 / (static_cast<double>(stopMs_) * sr_));
        const float startInc = static_cast<float>(1000.0 / (static_cast<double>(startMs_) * sr_));
        const float maxD     = static_cast<float>(maxDelay_);

        for (int n = 0; n < numFrames; ++n) {
            // W4-D22: `curve` is read PER SAMPLE and reshapes the brake, so
            // turning it mid-stop stepped the playback RATE — i.e. the pitch —
            // in one sample. Worst case at phase 0.5: linear gives 0.5 and cubic
            // 0.125, a jump of nearly two octaves. Smoothed over kCurveRamp; at
            // rest the value is the knob exactly, so a stop started after the
            // ramp has landed is bit-identical to the old path.
            const float curveG  = curveSmooth_.next();
            const float mixG    = mixSmooth_.next();
            const float dryGain = 1.f - mixG;
            // ---- transport ----------------------------------------------------
            switch (state_) {
                case Run:
                    rate_      = 1.f;
                    readDelay_ = kBaseDelay;
                    break;

                case Stopping:
                    phase_ += stopInc;
                    if (phase_ >= 1.f) { phase_ = 1.f; state_ = Stopped; }
                    rate_ = shape(1.f - phase_, curveG);
                    break;

                case Stopped:
                    rate_ = 0.f;
                    break;

                case Starting:
                    phase_ += startInc;
                    if (phase_ >= 1.f) {
                        phase_     = 1.f;
                        state_     = Resync;
                        resyncPos_ = 0;
                    }
                    rate_ = shape(phase_, curveG);
                    break;

                case Resync:
                    rate_ = 1.f;
                    break;
            }

            // The read head trails the write head by (1 - rate) more each sample:
            // that IS the speed/pitch drop.
            if (state_ != Run && state_ != Resync) {
                readDelay_ += static_cast<double>(1.f - rate_);
                if (readDelay_ > static_cast<double>(maxD)) readDelay_ = static_cast<double>(maxD);
                if (readDelay_ < static_cast<double>(kBaseDelay)) readDelay_ = kBaseDelay;
            }

            // Fade the last sliver of speed to silence — at rate 0 a tape emits
            // nothing, and a frozen read head would otherwise hold a DC-ish tone.
            const float speedFade = std::min(1.f, rate_ / kFadeRate);

            for (int c = 0; c < C; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                InterpolatedDelayLine& line = lines_[static_cast<std::size_t>(c)];

                float in = buf[n];
                if (!std::isfinite(in)) in = 0.f;
                line.push(clamp(in, -16.f, 16.f));

                // The dry reference is read from the SAME line at the same base
                // delay, so mixing dry with wet never combs.
                const float dry = line.readAt(kBaseDelay);
                float wet = line.readAt(static_cast<float>(readDelay_)) * speedFade;

                if (state_ == Resync) {
                    // 30 ms equal-power crossfade back to live. See the header note.
                    const float t  = static_cast<float>(resyncPos_) /
                                     static_cast<float>(resyncSamples_);
                    const float a  = static_cast<float>(std::cos(0.5 * kPi * t));
                    const float b  = static_cast<float>(std::sin(0.5 * kPi * t));
                    wet = wet * a + dry * b;
                }

                if (!std::isfinite(wet)) wet = 0.f;
                float out = dryGain * dry + mixG * wet;
                if (!std::isfinite(out)) out = 0.f;
                buf[n] = clamp(out, -16.f, 16.f);
            }

            if (state_ == Resync && ++resyncPos_ >= resyncSamples_) {
                state_     = Run;
                rate_      = 1.f;
                readDelay_ = kBaseDelay;
            }
        }
    }

    // The read head parks 2 samples back so the Lagrange window is always whole.
    int latencySamples() const { return static_cast<int>(kBaseDelay); }

    /// Test-only: the transport rate the brake is currently running at (1 =
    /// play, 0 = stopped). It is the PITCH, so a step here is a pitch jump that
    /// NO sample-domain probe can see — the read POSITION stays continuous
    /// while its derivative does not, which is why W4-D22's `curve` seam is
    /// pinned in the RT gate against this value and not in DspParamSeamTests.
    float transportRateForTest() const { return rate_; }

private:
    enum State { Run, Stopping, Stopped, Starting, Resync };

    static float clamp(float v, float lo, float hi) {
        if (!(v == v)) return lo;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Brake/spin shape: a blend of linear and cubic. curve 0 = constant
    // deceleration (a motor coasting); curve 1 = holds speed then dives (a hand
    // slamming the platter).
    float shape(float u, float curve) const {
        u = clamp(u, 0.f, 1.f);
        return u * (1.f - curve) + u * u * u * curve;
    }

    static constexpr float  kBaseDelay       = 2.0f;
    static constexpr float  kFadeRate        = 0.08f;   // fade out below 8% speed
    static constexpr double kResyncMs        = 30.0;
    static constexpr double kMaxDelaySeconds = 4.0;

    double sr_        = 48000.0;
    int    maxCh_     = 2;
    int    maxDelay_  = 192008;

    float stopMs_  = 600.0f;
    float startMs_ = 400.0f;
    float curve_   = 0.5f;
    static constexpr float kCurveRamp = 0.030f;   // brake-shape crossfade (W4-D22)
    SmoothedValue curveSmooth_;
    float mix_     = 1.0f;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixSmooth_;

    std::vector<InterpolatedDelayLine> lines_;

    State  state_        = Run;
    float  rate_         = 1.f;
    double readDelay_    = kBaseDelay;
    float  phase_        = 0.f;
    int    resyncPos_    = 0;
    int    resyncSamples_ = 1440;
    int    lastTrigger_  = 0;
};

} // namespace dcdsp
