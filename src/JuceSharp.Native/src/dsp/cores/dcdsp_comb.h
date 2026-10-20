// dcdsp_comb.h — COMB FILTER ("comb").
//
// Models the resonator at the heart of a flanger, a Karplus-Strong string and
// every "metallic" tuned-delay effect: one delay line of 1/freqHz seconds,
// summed with the dry signal either FEEDFORWARD (a fixed +/- 6 dB ripple comb)
// or FEEDBACK (a resonant comb that rings, with a one-pole damping filter in
// the loop so the high partials die first, like a real string).
//
// Topology: dcdsp::InterpolatedDelayLine with 4-point Lagrange interpolation, so
// the tuning is CONTINUOUS and freqHz can be swept without zipper steps; the
// damping one-pole sits inside the feedback path; the mix is a ramped crossfade.
//
// Easy to get wrong: the line is sized for the WORST CASE — 20 Hz at 192 kHz,
// 9600 samples — and NOT for the rate it happens to be prepared at. A line sized
// from the prepared rate is correct until a device change asks for a longer
// delay than the buffer that was allocated for it, and every read then clamps to
// the buffer end, which detunes the comb silently instead of failing. The second
// trap is the delay GLIDE: it is smoothed in SAMPLES per frame, so the peak
// frequency moves monotonically; smoothing freqHz and dividing per sample makes
// the sweep audibly lopsided and, at the ends of the range, discontinuous.
// The third: `polarity` is a SWITCH between two full-scale signals - the
// recirculated tap and the dry-plus-tap sum - and at feedback 0.9 they are
// comparable in level, so flipping it between two samples splices the waveform.
// Both forms therefore run every sample (the damper already did) and both the
// output AND what is pushed into the line crossfade over 10 ms. The line's
// CONTENT means something different in the two modes, which is why the push is
// part of the fade and not only the output.
// The fourth: a non-finite tap must NOT clear the 9,604-float line from inside
// the sample loop. That is 38 KB of `std::fill` on the callback keyed on audio
// content, which no allocation gate can see - it does not allocate. The tap is
// zeroed where it is read and the clear is deferred to the top of the next
// block.
#pragma once

#include "dcdsp_primitives.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

struct CombParams {
    // Comb fundamental in Hz, 20 .. 5000. The delay is 1/freqHz seconds, so the
    // response peaks (feedback) or notches (feedforward, negative) at multiples
    // of this.
    float freqHz = 220.0f;
    // -0.98 .. 0.98. Negative inverts the returned signal: an INVERTED comb,
    // whose peaks land on the odd multiples of freqHz/2.
    float feedback = 0.7f;
    // 0 .. 1. A one-pole lowpass in the loop; 0 = no damping (20 kHz corner),
    // 1 = heavily damped (1 kHz corner).
    float damping = 0.3f;
    // 0 = Feedforward, 1 = Feedback. NAMED discrete.
    int   polarity = 1;
    float mix = 0.5f;
};

class CombCore {
public:
    static constexpr int kPolarityFeedforward = 0;
    static constexpr int kPolarityFeedback    = 1;
    // 20 Hz at 192 kHz. The line is allocated for this whatever the prepared
    // rate is, so a device change to a higher rate can never outgrow it.
    static constexpr int kMaxDelaySamples = 9600;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 1;
        const int nch = (numChannels > 0) ? numChannels : 1;
        lines_.resize(static_cast<std::size_t>(nch));          // sole allocation
        dampers_.assign(static_cast<std::size_t>(nch), OnePole{});
        clearPending_.assign(static_cast<std::size_t>(nch), 0);
        for (auto& l : lines_) l.prepare(kMaxDelaySamples);

        // Rate-owned: the delay in SAMPLES, the damping corner and both ramps.
        delaySmooth_.reset(sr_, kSweepRampSeconds);
        delaySmooth_.setTarget(delayFor(params_.freqHz));
        delaySmooth_.snapToTarget();
        fbSmooth_.reset(sr_, kRampSeconds);
        fbSmooth_.setTarget(params_.feedback);
        fbSmooth_.snapToTarget();
        mixSmooth_.reset(sr_, kRampSeconds);
        mixSmooth_.setTarget(params_.mix);
        mixSmooth_.snapToTarget();
        polSmooth_.reset(sr_, kRampSeconds);
        polSmooth_.setTarget(params_.polarity == kPolarityFeedback ? 1.0f : 0.0f);
        polSmooth_.snapToTarget();
        applyDamping();
        reset();
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& d : dampers_) d.reset();
        for (auto& f : clearPending_) f = 0;
    }

    void setParams(const CombParams& p) {
        params_.freqHz   = clampf(p.freqHz, 20.0f, 5000.0f, 220.0f);
        // A NaN lands on the NEUTRAL value, never on `lo`: for a symmetric
        // range `lo` is the most extreme setting there is, and a NaN feedback
        // becoming a near-maximal INVERTED resonator is not a safe default.
        params_.feedback = clampf(p.feedback, -0.98f, 0.98f, 0.0f);
        params_.damping  = clampf(p.damping, 0.0f, 1.0f, 0.3f);
        params_.polarity = (p.polarity <= 0) ? 0 : 1;
        params_.mix      = clampf(p.mix, 0.0f, 1.0f, 0.5f);

        delaySmooth_.setTarget(delayFor(params_.freqHz));
        fbSmooth_.setTarget(params_.feedback);
        mixSmooth_.setTarget(params_.mix);
        polSmooth_.setTarget(params_.polarity == kPolarityFeedback ? 1.0f : 0.0f);
        applyDamping();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;    // skipped WHOLE, never half-done
        const int nch = std::min(numChannels, static_cast<int>(lines_.size()));
        if (nch <= 0) return;

        // Deferred recovery: a non-finite tap in the PREVIOUS block asked for
        // the line to be cleared, and that 38 KB fill happens here, once,
        // outside the sample loop.
        for (int c = 0; c < nch; ++c) {
            auto& pending = clearPending_[static_cast<std::size_t>(c)];
            if (pending == 0) continue;
            lines_[static_cast<std::size_t>(c)].reset();
            dampers_[static_cast<std::size_t>(c)].reset();
            pending = 0;
        }

        for (int n = 0; n < numFrames; ++n) {
            const float mix   = mixSmooth_.next();
            const float dryG  = 1.0f - mix;
            const float fb    = fbSmooth_.next();
            const float delay = delaySmooth_.next();
            const float pol   = polSmooth_.next();

            for (int c = 0; c < nch; ++c) {
                auto& line = lines_[static_cast<std::size_t>(c)];
                auto& damp = dampers_[static_cast<std::size_t>(c)];

                float x = channels[c][n];
                if (!std::isfinite(x)) x = 0.0f;

                float tap = line.readAt(delay);
                if (!std::isfinite(tap)) {
                    tap = 0.0f;
                    clearPending_[static_cast<std::size_t>(c)] = 1;
                }

                // Both forms, every sample, crossfaded by `pol`.
                //   pol = 1  FEEDBACK: the damping one-pole is INSIDE the loop
                //            (|fb| <= 0.98 and a lowpass has |H| <= 1, so the
                //            loop gain is bounded well under unity and the comb
                //            can never run away) and the output is the tap.
                //   pol = 0  FEEDFORWARD: the line carries the dry signal only,
                //            the ripple is fixed at +/- (1 + |fb|) and nothing
                //            accumulates.
                // Each endpoint is bit-identical to the branch it replaces.
                const float shaped = damp.process(tap);
                line.push(x + pol * fb * shaped);
                const float wet = pol * tap + (1.0f - pol) * (x + fb * shaped);

                float y = dryG * x + mix * wet;
                if (!std::isfinite(y)) y = 0.0f;
                channels[c][n] = clampf(y, -24.0f, 24.0f);
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    void applyDamping() {
        // 0 -> 20 kHz (effectively open), 1 -> 1 kHz.
        const double fc = 20000.0 * std::pow(0.05, static_cast<double>(params_.damping));
        for (auto& d : dampers_) d.setLowpass(sr_, fc);
    }

    float delayFor(float hz) const {
        const double d = sr_ / static_cast<double>(hz > 1.0f ? hz : 1.0f);
        return static_cast<float>(std::min<double>(d, kMaxDelaySamples));
    }

    // `fallback` is where a NaN lands. For a param whose neutral is the MIDDLE
    // of its range, `lo` is the most extreme setting available, so a rejected
    // NaN must not land there: every symmetric param passes its own neutral.
    static float clampf(float v, float lo, float hi) { return clampf(v, lo, hi, lo); }
    static float clampf(float v, float lo, float hi, float fallback) {
        if (v != v) return fallback;          // NaN
        if (v < lo) return lo;
        return (v > hi) ? hi : v;
    }

    static constexpr float kRampSeconds      = 0.010f;
    static constexpr float kSweepRampSeconds = 0.030f;   // the tuning glide

    CombParams params_{};
    double sr_ = 48000.0;
    int    maxBlock_ = 1;
    std::vector<InterpolatedDelayLine> lines_;
    std::vector<OnePole> dampers_;
    SmoothedValue delaySmooth_;   // SAMPLES, not Hz
    SmoothedValue fbSmooth_;
    SmoothedValue mixSmooth_;
    SmoothedValue polSmooth_;     // 0 feedforward .. 1 feedback
    std::vector<char> clearPending_;
};

}  // namespace dcdsp
