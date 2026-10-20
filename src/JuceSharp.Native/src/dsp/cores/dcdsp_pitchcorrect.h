// dcdsp_pitchcorrect.h — PitchCorrectCore: monophonic pitch correction (an
// Auto-Tune / Melodyne "snap to scale" box as a PEDAL, the way a Boss VE or a
// TC Mic Mechanic does it live).
//
// TOPOLOGY. A native autocorrelation tracker (dcdsp_detail_pitchgear.h) reports
// the fundamental; the note is snapped to the nearest member of the chosen
// scale; the deviation in cents becomes a shift RATIO that a crossfaded
// delay-line shifter applies. `speedMs` is the glide onto that ratio — 0 is the
// hard "robot" snap, 500 ms is a singer's own portamento. `formant` runs the
// whole thing through an LPC whiten / re-dress pair so the body resonances stay
// put instead of moving with the note.
//
// EASY TO GET WRONG: it is MONOPHONIC — a chord has no one period, so the
// tracker's normalised peak collapses and the core HOLDS its last ratio rather
// than chasing noise. Without that hold, a strum makes the pedal sweep wildly;
// with it, a strum simply passes through at whatever correction the last single
// note asked for. The second trap is `amount` 0: it must be bit-for-bit the
// dry path, which means crossfading to the shifter's own TIME-ALIGNED dry read
// rather than to the undelayed input (a splice) or to the frozen two-tap pair
// (a comb).
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif
#include "dcdsp_detail_pitchgear.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace dcdsp {

struct PitchCorrectParams {
    float scale    = 0.0f;    // 0 Chromatic, 1 Major, 2 Minor, 3 Pentatonic, 4 Blues
    float key      = 0.0f;    // 0 = C .. 11 = B
    float speedMs  = 50.0f;   // glide onto the corrected pitch   [0 .. 500]
    float amount   = 1.0f;    // how much of the deviation to remove [0 .. 1]
    float humanize = 0.0f;    // fraction of the deviation LEFT IN   [0 .. 1]
    float formant  = 0.0f;    // 0 = Off, 1 = Preserve
    float mix      = 1.0f;    // dry/wet                          [0 .. 1]
};

class PitchCorrectCore {
public:
    /// Shifter geometry. 40 ms spans a low-E period four times over, which is
    /// what keeps a dropped-tuning guitar from warbling; the head start keeps
    /// the moving taps behind the write head.
    static constexpr double kWindowMs = 40.0;
    static constexpr double kPreDelayMs = 1.0;
    /// Splice-alignment search span. A correction of up to two semitones
    /// wraps a grain every few hundred milliseconds, and an UNALIGNED wrap
    /// at that size is an audible phase reversal; 8 ms covers a full period
    /// down to 125 Hz. It is part of the reported latency, exactly as
    /// dcdsp_whammy.h's search span is part of its.
    static constexpr double kAlignMs = 8.0;
    /// A correction wider than this is a TRACKING ERROR (an octave slip, a
    /// chord), not a performance, so it is refused rather than applied.
    static constexpr float kMaxCorrectionCents = 200.0f;
    /// Even the "robot" setting keeps the tap RATE continuous. 1.5 ms is under
    /// a fiftieth of the shortest musical note and reads as instant.
    static constexpr float kMinGlideMs = 1.5f;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        nch_ = std::min(std::max(numChannels, 1), kMaxChannels);
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 4096;

        window_ = (float) (sr_ * kWindowMs / 1000.0);
        preDelay_ = (float) (sr_ * kPreDelayMs / 1000.0);
        align_ = (int) std::lround(sr_ * kAlignMs / 1000.0);
        latency_ = (int) std::lround(
            (double) detail::GrainShifter::latencyFor(preDelay_, window_, align_));

        tracker_.prepare(sr_);
        for (int c = 0; c < nch_; ++c) {
            shift_[(std::size_t) c].prepare(sr_, kWindowMs, kPreDelayMs, kAlignMs);
            lpc_[(std::size_t) c].prepare(sr_, latency_);
        }

        ratio_.reset(sr_, kRatioRampSeconds);
        blend_.reset(sr_, kRampSeconds);
        mix_.reset(sr_, kRampSeconds);
        formantFade_.reset(sr_, kFormantFadeSeconds);
        applyDerived();
        ratio_.snapToTarget();
        blend_.snapToTarget();
        mix_.snapToTarget();
        formantFade_.snapToTarget();
        reset();
    }

    void reset() {
        tracker_.reset();
        for (int c = 0; c < kMaxChannels; ++c) {
            shift_[(std::size_t) c].reset();
            lpc_[(std::size_t) c].reset();
        }
        shiftCents_ = 0.0f;
    }

    void setParams(const PitchCorrectParams& p) {
        p_ = p;
        applyDerived();
    }

    /// Constant: the shifter's nominal group delay. The TRACKER's own analysis
    /// lag is not in this figure and must not be — it delays no audio; it only
    /// means the correction arrives a few tens of milliseconds after a new
    /// note, which is a tracking property, not latency the chain can
    /// compensate.
    int latencySamples() const { return latency_; }

    // ---- test surfaces ----------------------------------------------------
    float trackedHz() const { return tracker_.frequencyHz(); }
    bool tracking() const { return tracker_.confident(); }
    /// Current correction in cents, after amount/humanize and the glide.
    float shiftCents() const { return shiftCents_; }
    int trackerDecimation() const { return tracker_.decimation(); }
    int trackerHopSamples() const { return tracker_.hopSamples(); }
    bool ringsCoverGeometry() const {
        for (int c = 0; c < nch_; ++c)
            if (!shift_[(std::size_t) c].ringCoversGeometry()) return false;
        return true;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // oversized: skipped WHOLE
        const int nch = std::min(numChannels, nch_);
        const float invN = 1.0f / (float) nch;

        for (int i = 0; i < numFrames; ++i) {
            // ---- 1) track on the MONO SUM, so a stereo node does not run two
            //         trackers that can disagree about what note is playing.
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
            updateTarget();

            // ---- 2) one walk per FRAME, above the channel loop.
            const float r = ratio_.next();
            const float blend = blend_.next();
            const float mix = mix_.next();
            // `formant` substitutes synth(shift(whiten(x))) for shift(x): the
            // whole wet path, and it landed on ONE sample. It now walks the
            // LPC pair's BANDWIDTH EXPANSION from 0 to 1 instead (see
            // LpcFormant::whiten) — s = 0 is exactly the plain path, s = 1 is
            // exactly the formant path, and every s between is a genuine
            // inverse pair, which no blend of the two end states is.
            //
            // The glide is LONGER than the shifter's pipeline on purpose. The
            // front end whitens sample n at today's s while the back end
            // re-dresses audio that was whitened `latency_` samples ago, so a
            // 10 ms ramp over a 21 ms pipeline would apply two quite different
            // filters to the same sample. At kFormantFadeSeconds the two ends
            // differ by under a tenth of the sweep and the mismatch is a slow
            // spectral tilt rather than anything discontinuous.
            const float ff = formantFade_.next();

            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                const float x = buf[i];
                detail::GrainShifter& sh = shift_[(std::size_t) c];
                detail::LpcFormant& lp = lpc_[(std::size_t) c];

                // The LPC runs UNCONDITIONALLY. It used to be skipped while
                // formant was off, so the raw ring and the coefficient-set ring
                // were stale by however long the pedal had been off and
                // `whiten()` re-dressed the first sample after a switch with a
                // set derived from audio from before it. A ring write plus one
                // hop-rate solve every 12 ms is the price of the sets being warm
                // the moment the switch lands; with ff at 0 the result is
                // discarded and the output is bit-identical to not running it.
                lp.push(x);
                const float in = lp.whiten(x, ff);
                float wet = detail::GrainShifter::bypassBlend(
                    sh.process(in, r, window_, preDelay_),
                    sh.readNominal(window_, preDelay_), blend);
                wet = lp.synth(wet, ff);

                if (!std::isfinite(wet)) wet = 0.0f;
                wet = std::max(-8.0f, std::min(8.0f, wet));
                float out = (1.0f - mix) * x + mix * wet;
                if (!std::isfinite(out)) out = 0.0f;
                buf[i] = std::max(-16.0f, std::min(16.0f, out));
            }
        }
    }

private:
    /// Re-aim the shift once per tracker answer. Runs per sample but early-outs
    /// between hops, because the tracker only moves on a hop boundary.
    void updateTarget() {
        const float f = tracker_.frequencyHz();
        if (!tracker_.confident() || !(f > 1.0f)) return;   // HOLD the last ratio
        if (f == lastTrackedHz_) return;                    // nothing new this frame
        lastTrackedHz_ = f;

        const float midi = detail::freqToMidi(f);
        const float snapped = detail::snapToScale(midi, scale_, key_);
        // Deviation the player is off by, in cents. Positive = sharp.
        const float devCents = (midi - snapped) * 100.0f;
        // amount scales the correction; humanize LEAVES a fraction of the
        // original deviation in, so 1.0 is no correction at all whatever
        // `amount` says — which is what "humanize" means.
        float cents = -devCents * amount_ * (1.0f - humanize_);
        cents = detail::clampf(cents, -kMaxCorrectionCents, kMaxCorrectionCents, 0.0f);
        shiftCents_ = cents;
        ratio_.setTarget(detail::centsToRatio(cents));
        if (std::fabs(cents) > 1.0f) blend_.setTarget(1.0f);
        else if (std::fabs(cents) < 0.5f) blend_.setTarget(0.0f);
    }

    void applyDerived() {
        const int s = (int) std::lround(detail::clampf(p_.scale, 0.0f, 4.0f, 0.0f));
        scale_ = static_cast<detail::Scale>(s);
        key_ = (int) std::lround(detail::clampf(p_.key, 0.0f, 11.0f, 0.0f));
        amount_ = detail::clampf(p_.amount, 0.0f, 1.0f, 1.0f);
        humanize_ = detail::clampf(p_.humanize, 0.0f, 1.0f, 0.0f);
        mix_.setTarget(detail::clampf(p_.mix, 0.0f, 1.0f, 1.0f));
        formant_ = (detail::finiteOr(p_.formant, 0.0f) >= 0.5f);
        formantFade_.setTarget(formant_ ? 1.0f : 0.0f);

        const float ms = std::max(kMinGlideMs,
                                  detail::clampf(p_.speedMs, 0.0f, 500.0f, 50.0f));
        // The glide is the RATIO smoother's ramp length, re-derived here so it
        // survives a sample-rate change as well as a knob move. reset() cancels
        // the ramp in flight, so the pending target is re-issued: without that,
        // a knob write during a glide FREEZES the pitch half-way to the note.
        const float pending = ratio_.target();
        ratio_.reset(sr_, ms * 0.001f);
        ratio_.setTarget(pending);

        // amount 0 (or humanize 1) is the dry path: aim the shift at unity NOW
        // rather than waiting for the next tracker hop, so the knob answers
        // immediately.
        if (amount_ <= 0.0f || humanize_ >= 1.0f) {
            shiftCents_ = 0.0f;
            ratio_.setTarget(1.0f);
            blend_.setTarget(0.0f);
        }
        lastTrackedHz_ = -1.0f;   // force a re-aim on the next confident answer
    }

    static constexpr int kMaxChannels = 8;
    static constexpr float kRampSeconds = 0.010f;
    static constexpr float kRatioRampSeconds = 0.050f;   // replaced by speedMs
    /// The `formant` glide. Deliberately several times the shifter's pipeline
    /// latency — see the note in process().
    static constexpr float kFormantFadeSeconds = 0.150f;

    PitchCorrectParams p_{};
    double sr_ = 48000.0;
    int nch_ = 2;
    int maxBlock_ = 4096;
    int latency_ = 1008;
    int align_ = 384;
    float window_ = 1920.0f, preDelay_ = 48.0f;

    detail::Scale scale_ = detail::Scale::Chromatic;
    int key_ = 0;
    float amount_ = 1.0f, humanize_ = 0.0f;
    bool formant_ = false;
    float shiftCents_ = 0.0f;
    float lastTrackedHz_ = -1.0f;

    detail::AutocorrTracker tracker_;
    SmoothedValue ratio_, blend_, mix_, formantFade_;
    std::array<detail::GrainShifter, kMaxChannels> shift_{};
    std::array<detail::LpcFormant, kMaxChannels> lpc_{};
};

} // namespace dcdsp
