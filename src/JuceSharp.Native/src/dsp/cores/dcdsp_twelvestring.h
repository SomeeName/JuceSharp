// dcdsp_twelvestring.h — TwelveStringCore: the 12-string simulator (a Boss
// OC/CE-style "12 string" mode, an Electro-Harmonix Nano POG patch, Roland's
// GR twelve-string voice).
//
// WHAT A 12-STRING ACTUALLY IS, and why this is a two-band effect: the four
// LOW courses (E A D G) are an octave PAIR — a thin string tuned an octave up
// beside the wound one — while the two HIGH courses (B E) are a UNISON pair,
// two identical strings that are never quite in tune with each other. So the
// low band gets an octave-up voice and the high band gets a slightly detuned
// copy, split at ~250 Hz (open G is 196 Hz, open B is 247 Hz). One
// full-spectrum octave-up would put a course an octave above the high E, which
// no 12-string has.
//
// The courses are struck a few milliseconds apart (one pick, two strings), and
// the octave strings on a 12-string are BRIGHTER than the wound ones — hence
// `delayMs` and `brightness`.
//
// EASY TO GET WRONG: the band split has to be STEEP. At a gentle 12 dB/oct a
// 440 Hz note still leaks enough into the low band to grow an audible 880 Hz
// course, which is the one thing a 12-string does not do — so the crossover is
// 6th-order Butterworth, and the test asserts the 880 is not there. Second
// trap: `brightness` is a BOUNDED SHELF, never a high-pass. Three in-path
// bass filters in this repo each looked reasonable and SUMMED to a rig that was
// 20 dB down at the low E.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif
#include "dcdsp_detail_pitchgear.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace dcdsp {

struct TwelveStringParams {
    float octaveLevel  = 0.6f;   // the E A D G octave courses   [0 .. 1]
    float unisonLevel  = 0.6f;   // the B E unison courses       [0 .. 1]
    float detuneCents  = 8.0f;   // how far out the unison is    [0 .. 25]
    float delayMs      = 6.0f;   // course-strike offset         [0 .. 20]
    float brightness   = 0.5f;   // shelf on the added courses   [0 .. 1]
    float mix          = 0.6f;   // how much course is summed in [0 .. 1]
};

class TwelveStringCore {
public:
    /// The course split. Open G is 196 Hz and open B is 247 Hz, so everything
    /// below this is an octave course and everything above is a unison.
    static constexpr double kSplitHz = 250.0;
    static constexpr double kWindowMs = 40.0;
    static constexpr double kPreDelayMs = 1.0;
    static constexpr double kMaxDelayMs = 20.0;
    /// Splice alignment for the OCTAVE course only. At ratio 2 the two taps
    /// of an unaligned shifter invert every wrap and the octave CANCELS
    /// (measured: -141 dB at 220 Hz for a 110 Hz note). The unison course is
    /// a few cents and wraps once a second, so it needs none and the search
    /// would only wander its delay.
    static constexpr double kOctaveAlignMs = 8.0;
    /// The brightness shelf: bounded, +/- 6 dB around flat at 0.5.
    static constexpr double kShelfHz = 2500.0;
    static constexpr double kShelfRangeDb = 6.0;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        nch_ = std::min(std::max(numChannels, 1), kMaxChannels);
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 4096;

        window_ = (float) (sr_ * kWindowMs / 1000.0);
        preDelay_ = (float) (sr_ * kPreDelayMs / 1000.0);

        for (int c = 0; c < nch_; ++c) {
            oct_[(std::size_t) c].prepare(sr_, kWindowMs, kPreDelayMs + kMaxDelayMs,
                                         kOctaveAlignMs);
            uni_[(std::size_t) c].prepare(sr_, kWindowMs, kPreDelayMs + kMaxDelayMs);
        }

        octLevel_.reset(sr_, kRampSeconds);
        uniLevel_.reset(sr_, kRampSeconds);
        mix_.reset(sr_, kRampSeconds);
        ratio_.reset(sr_, kRampSeconds);
        delay_.reset(sr_, kGeometryRampSeconds);
        uniBlend_.reset(sr_, kRampSeconds);
        bright_.reset(sr_, kRampSeconds);

        designFilters();       // RATE-owned: the crossover and the shelf
        applyDerived();
        octLevel_.snapToTarget();
        uniLevel_.snapToTarget();
        mix_.snapToTarget();
        ratio_.snapToTarget();
        delay_.snapToTarget();
        uniBlend_.snapToTarget();
        bright_.snapToTarget();
        lastBright_ = -1.0f;     // force the first design
        reset();
    }

    void reset() {
        for (int c = 0; c < kMaxChannels; ++c) {
            oct_[(std::size_t) c].reset();
            uni_[(std::size_t) c].reset();
            for (int s = 0; s < kOrderSections; ++s) {
                lp_[(std::size_t) c][(std::size_t) s].reset();
                hp_[(std::size_t) c][(std::size_t) s].reset();
            }
            shelf_[(std::size_t) c].reset();
        }
    }

    void setParams(const TwelveStringParams& p) {
        p_ = p;
        applyDerived();
    }

    /// ZERO, and deliberately. The DRY path through this module is not delayed
    /// at all — the added courses are, by `delayMs`, and that offset IS the
    /// effect (one pick, two strings). Reporting the course delay here would
    /// make branch PDC push every other branch 27 ms late to align against a
    /// signal that is never alone.
    int latencySamples() const { return 0; }

    // ---- test surfaces ----------------------------------------------------
    bool ringsCoverGeometry() const {
        for (int c = 0; c < nch_; ++c)
            if (!oct_[(std::size_t) c].ringCoversGeometry()
             || !uni_[(std::size_t) c].ringCoversGeometry()) return false;
        return true;
    }
    double splitHz() const { return kSplitHz; }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // oversized: skipped WHOLE
        const int nch = std::min(numChannels, nch_);

        for (int i = 0; i < numFrames; ++i) {
            // One walk per FRAME, above the channel loop.
            const float gOct = octLevel_.next();
            const float gUni = uniLevel_.next();
            const float mix = mix_.next();
            const float r = ratio_.next();
            const float extra = delay_.next();
            const float uniBlend = uniBlend_.next();
            const float pre = preDelay_ + extra;

            // `brightness` RAMPS, and the shelf is re-designed from the smoothed
            // value only when it has actually moved past a tolerance. It used to
            // be written straight into the design from applyDerived(): up to
            // 12 dB of shelf on one sample (measured a 0.229 step against a
            // 0.531 peak), AND eight channels re-designed on every param write
            // including `mix` and `delayMs`, whether or not brightness had
            // moved. ensemble smooths `tone` before redesigning and bbddelay
            // ramps `age` for exactly this reason.
            const float br = bright_.next();
            if (std::fabs(br - lastBright_) > kBrightTolerance) {
                lastBright_ = br;
                const double db = -kShelfRangeDb + 2.0 * kShelfRangeDb * (double) br;
                for (int c = 0; c < nch; ++c)
                    shelf_[(std::size_t) c].setHighShelf(sr_, kShelfHz, 0.707, db);
            }

            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                float x = buf[i];
                if (!std::isfinite(x)) x = 0.0f;

                // ---- steep two-band split -----------------------------------
                float low = x, high = x;
                for (int s = 0; s < kOrderSections; ++s) {
                    low = lp_[(std::size_t) c][(std::size_t) s].process(low);
                    high = hp_[(std::size_t) c][(std::size_t) s].process(high);
                }

                // ---- the two courses ----------------------------------------
                const float octave = oct_[(std::size_t) c].process(low, 2.0f, window_, pre);
                // At detuneCents 0 the unison shifter's taps stop moving and
                // the pair is a fixed comb, so it crossfades to its own
                // time-aligned dry — a second string in perfect tune IS the
                // same string, and that is what it should sound like.
                detail::GrainShifter& su = uni_[(std::size_t) c];
                const float unison = detail::GrainShifter::bypassBlend(
                    su.process(high, r, window_, pre), su.readNominal(window_, pre), uniBlend);

                float course = gOct * octave + gUni * unison;
                course = shelf_[(std::size_t) c].process(course);
                if (!std::isfinite(course)) course = 0.0f;

                // The original string is ALWAYS there — a 12-string adds a
                // course, it does not replace the note. `mix` is how much
                // course, not a dry/wet.
                float out = x + mix * course;
                if (!std::isfinite(out)) out = 0.0f;
                buf[i] = std::max(-16.0f, std::min(16.0f, out));
            }
        }
    }

private:
    /// 6th-order Butterworth split + the brightness shelf. Everything here is
    /// owned by the RATE and must be re-derived on every prepare().
    void designFilters() {
        // Butterworth 6th-order section Qs.
        static const double kQ[kOrderSections] = { 0.5176, 0.7071, 1.9319 };
        const double db = -kShelfRangeDb + 2.0 * kShelfRangeDb
                        * (double) detail::clampf(p_.brightness, 0.0f, 1.0f, 0.5f);
        for (int c = 0; c < kMaxChannels; ++c) {
            for (int s = 0; s < kOrderSections; ++s) {
                lp_[(std::size_t) c][(std::size_t) s].setLowpass(sr_, kSplitHz, kQ[s]);
                hp_[(std::size_t) c][(std::size_t) s].setHighpass(sr_, kSplitHz, kQ[s]);
            }
            shelf_[(std::size_t) c].setHighShelf(sr_, kShelfHz, 0.707, db);
        }
    }

    void applyDerived() {
        octLevel_.setTarget(detail::clampf(p_.octaveLevel, 0.0f, 1.0f, 0.6f));
        uniLevel_.setTarget(detail::clampf(p_.unisonLevel, 0.0f, 1.0f, 0.6f));
        mix_.setTarget(detail::clampf(p_.mix, 0.0f, 1.0f, 0.6f));
        const float cents = detail::clampf(p_.detuneCents, 0.0f, 25.0f, 8.0f);
        ratio_.setTarget(detail::centsToRatio(cents));
        if (cents > 0.5f) uniBlend_.setTarget(1.0f);
        else if (cents < 0.25f) uniBlend_.setTarget(0.0f);
        const float ms = detail::clampf(p_.delayMs, 0.0f, (float) kMaxDelayMs, 6.0f);
        delay_.setTarget((float) (sr_ * (double) ms / 1000.0));
        // Only the shelf depends on a knob, and it is not DESIGNED here at
        // all: this sets the smoother's target and process() re-designs from
        // the smoothed value when it has moved. Designing here meant eight
        // shelves per param write -- for `mix`, for `delayMs`, and even when
        // brightness had not moved -- on the AUDIO thread, since applyDerived
        // runs out of ParamMailbox::applyPending.
        bright_.setTarget(detail::clampf(p_.brightness, 0.0f, 1.0f, 0.5f));
    }

    static constexpr int kMaxChannels = 8;
    static constexpr int kOrderSections = 3;      // 3 biquads = 6th order
    static constexpr float kRampSeconds = 0.010f;
    static constexpr float kGeometryRampSeconds = 0.020f;
    /// 0.4 % of the brightness range = 0.048 dB of shelf: below audibility, and
    /// it bounds the redesign rate to ~250 over the whole 10 ms glide.
    static constexpr float kBrightTolerance = 0.004f;

    TwelveStringParams p_{};
    double sr_ = 48000.0;
    int nch_ = 2;
    int maxBlock_ = 4096;
    float window_ = 1920.0f, preDelay_ = 48.0f;

    SmoothedValue octLevel_, uniLevel_, mix_, ratio_, delay_, uniBlend_, bright_;
    float lastBright_ = -1.0f;
    std::array<detail::GrainShifter, kMaxChannels> oct_{}, uni_{};
    std::array<std::array<Biquad, kOrderSections>, kMaxChannels> lp_{}, hp_{};
    std::array<Biquad, kMaxChannels> shelf_{};
};

} // namespace dcdsp
