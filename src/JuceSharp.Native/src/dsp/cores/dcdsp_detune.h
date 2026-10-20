// dcdsp_detune.h — DetuneCore: the micro-pitch doubler (an Eventide H910/H3000
// "micro pitch" patch, a TC Chorus+ in detune mode, the Boss CE/DC family's
// thickening).
//
// TOPOLOGY. Two pitch shifters at +/- the SAME small interval (12 cents by
// default), one panned each way, with a few milliseconds of delay OFFSET
// between them so the pair never phases into one another. There is no LFO —
// that is what makes it a detuner rather than a chorus: the beating comes from
// the constant interval, not from a modulated delay, so it never warbles.
//
// EASY TO GET WRONG: at cents 0 the two taps of each shifter STOP MOVING and
// the pair degenerates into a fixed comb (see dcdsp_detail_pitchgear.h). The
// core crossfades each voice to its own time-aligned dry as the interval
// approaches zero, which is why "cents 0, spread 0, mix 1" measures flat
// instead of notched. The lowLatency flip is a WINDOW change and therefore a
// delay-tap move: it is ramped, and the rings are allocated for the long
// window at prepare() so a ramp can never leave them.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif
#include "dcdsp_detail_pitchgear.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace dcdsp {

struct DetuneParams {
    float cents      = 12.0f;   // symmetric +/- interval   [0 .. 50]
    float spreadMs   = 8.0f;    // delay offset, voice B    [0 .. 30]
    float width      = 1.0f;    // 1 = A hard left, B hard right [0 .. 1]
    float mix        = 0.5f;    // dry/wet                  [0 .. 1]
    float lowLatency = 0.0f;    // 0/1; 1 = the short window
};

class DetuneCore {
public:
    /// Window lengths, in milliseconds. The long one spans a low-E period
    /// (12.1 ms) comfortably; the short one is what keeps the pedal under
    /// 10 ms of latency for a player monitoring themselves.
    static constexpr double kWindowLongMs = 40.0;
    static constexpr double kWindowShortMs = 8.0;
    /// Fixed head start, so the moving taps never run past the write head.
    static constexpr double kPreDelayMs = 1.0;
    static constexpr double kMaxSpreadMs = 30.0;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        nch_ = std::min(std::max(numChannels, 1), kMaxChannels);
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 4096;

        // RATE-owned geometry, both modes. The shifters are allocated for the
        // LONG window plus the longest spread, so neither a mode flip nor a
        // spread sweep can reach an allocation on the callback.
        winLong_ = (float) (sr_ * kWindowLongMs / 1000.0);
        winShort_ = (float) (sr_ * kWindowShortMs / 1000.0);
        preDelay_ = (float) (sr_ * kPreDelayMs / 1000.0);
        for (int c = 0; c < nch_; ++c) {
            a_[(std::size_t) c].prepare(sr_, kWindowLongMs, kPreDelayMs);
            b_[(std::size_t) c].prepare(sr_, kWindowLongMs, kPreDelayMs + kMaxSpreadMs);
        }

        ratioA_.reset(sr_, kRampSeconds);
        ratioB_.reset(sr_, kRampSeconds);
        spread_.reset(sr_, kGeometryRampSeconds);
        window_.reset(sr_, kGeometryRampSeconds);
        blend_.reset(sr_, kRampSeconds);
        width_.reset(sr_, kRampSeconds);
        mix_.reset(sr_, kRampSeconds);

        applyDerived();
        // A freshly prepared node starts AT its settings, never ramps up to
        // them from whatever it happened to hold (the bigmuff lesson).
        ratioA_.snapToTarget();
        ratioB_.snapToTarget();
        spread_.snapToTarget();
        window_.snapToTarget();
        blend_.snapToTarget();
        width_.snapToTarget();
        mix_.snapToTarget();
        reset();
    }

    void reset() {
        for (int c = 0; c < kMaxChannels; ++c) {
            a_[(std::size_t) c].reset();
            b_[(std::size_t) c].reset();
        }
    }

    void setParams(const DetuneParams& p) {
        p_ = p;
        applyDerived();
    }

    /// Message-thread INTENT: the nominal group delay of the voice-A pair at
    /// the geometry the knob now asks for. Voice B sits `spreadMs` later by
    /// design — that offset is the effect, not latency. The ramp after a mode
    /// flip is a 20 ms glide through the two values; the chain's PDC follows
    /// the target, exactly as the limiter's look-ahead does.
    int latencySamples() const {
        const float w = lowLatency_ ? winShort_ : winLong_;
        return (int) std::lround((double) detail::GrainShifter::latencyFor(preDelay_, w));
    }

    /// TEST SURFACE: true while every shifter's ring covers the geometry the
    /// audio path can ask for.
    bool ringsCoverGeometry() const {
        for (int c = 0; c < nch_; ++c)
            if (!a_[(std::size_t) c].ringCoversGeometry() || !b_[(std::size_t) c].ringCoversGeometry())
                return false;
        return true;
    }

    /// TEST SURFACE: crossfade-window tables built since construction. One per
    /// shifter per prepare(); a lowLatency flip may never raise it.
    long long windowBuilds() const {
        long long n = 0;
        for (int c = 0; c < nch_; ++c)
            n += a_[(std::size_t) c].windowBuilds() + b_[(std::size_t) c].windowBuilds();
        return n;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        // An oversized block is skipped WHOLE. Half-processing it would splice
        // clean dry into the middle of a block, which is worse than a gap.
        if (numFrames > maxBlock_) return;
        const int nch = std::min(numChannels, nch_);

        for (int i = 0; i < numFrames; ++i) {
            // One walk per FRAME, above the channel loop: a smoother walked
            // inside a channel-inner loop runs numChannels times too fast and
            // hands each channel a different gain over the same block.
            const float rA = ratioA_.next();
            const float rB = ratioB_.next();
            const float win = window_.next();
            const float spread = spread_.next();
            const float blend = blend_.next();
            const float w = width_.next();
            const float mix = mix_.next();
            const float gSelf = 0.5f + 0.5f * w;
            const float gCross = 0.5f - 0.5f * w;

            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                float x = buf[i];
                if (!std::isfinite(x)) x = 0.0f;

                detail::GrainShifter& sa = a_[(std::size_t) c];
                detail::GrainShifter& sb = b_[(std::size_t) c];
                const float va = detail::GrainShifter::bypassBlend(
                    sa.process(x, rA, win, preDelay_), sa.readNominal(win, preDelay_), blend);
                const float vb = detail::GrainShifter::bypassBlend(
                    sb.process(x, rB, win, preDelay_ + spread),
                    sb.readNominal(win, preDelay_ + spread), blend);

                // Even channels lead with voice A, odd with voice B, so a
                // stereo pair opens outward and a mono node still hears both.
                const bool lead = ((c & 1) == 0);
                const float wet = lead ? (gSelf * va + gCross * vb)
                                       : (gSelf * vb + gCross * va);
                float out = (1.0f - mix) * x + mix * wet;
                if (!std::isfinite(out)) out = 0.0f;
                buf[i] = std::max(-16.0f, std::min(16.0f, out));
            }
        }
    }

private:
    void applyDerived() {
        const float cents = detail::clampf(p_.cents, 0.0f, 50.0f, 12.0f);
        const float ratio = detail::centsToRatio(cents);
        ratioA_.setTarget(ratio);
        ratioB_.setTarget(1.0f / ratio);
        // Hysteresis so a knob parked at the very bottom cannot chatter the
        // bypass crossfade. Below a quarter of a cent there is no interval to
        // hear and the comb is the only thing left.
        if (cents > 0.5f) blend_.setTarget(1.0f);
        else if (cents < 0.25f) blend_.setTarget(0.0f);

        const float spreadMs = detail::clampf(p_.spreadMs, 0.0f, (float) kMaxSpreadMs, 8.0f);
        spread_.setTarget((float) (sr_ * (double) spreadMs / 1000.0));
        width_.setTarget(detail::clampf(p_.width, 0.0f, 1.0f, 1.0f));
        mix_.setTarget(detail::clampf(p_.mix, 0.0f, 1.0f, 0.5f));

        lowLatency_ = (detail::finiteOr(p_.lowLatency, 0.0f) >= 0.5f);
        window_.setTarget(lowLatency_ ? winShort_ : winLong_);
    }

    static constexpr int kMaxChannels = 8;
    static constexpr float kRampSeconds = 0.010f;
    /// The window is a DELAY TAP, so a mode flip glides rather than stepping —
    /// and the ramp has to be long enough that the tap moves LESS THAN ONE
    /// SAMPLE PER SAMPLE. The two windows are 1,536 samples apart at 48 kHz; a
    /// 20 ms ramp moved the tap at 1.6 samples/sample, so the read head raced
    /// backwards through the ring faster than real time and the flip came out
    /// as a 2.6x-speed chirp. 80 ms keeps |d'| under 0.5, which is a short
    /// downward bend — audible as a bend, which is what it is, and not a click.
    static constexpr float kGeometryRampSeconds = 0.080f;

    DetuneParams p_{};
    double sr_ = 48000.0;
    int nch_ = 2;
    int maxBlock_ = 4096;
    bool lowLatency_ = false;
    float winLong_ = 1920.0f, winShort_ = 384.0f, preDelay_ = 48.0f;

    SmoothedValue ratioA_, ratioB_, spread_, window_, blend_, width_, mix_;
    std::array<detail::GrainShifter, kMaxChannels> a_{}, b_{};
};

} // namespace dcdsp
