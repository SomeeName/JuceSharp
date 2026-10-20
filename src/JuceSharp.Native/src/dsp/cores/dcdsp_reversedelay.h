// dcdsp_reversedelay.h — reverse / backwards delay (pure dcdsp core, RT-safe).
//
// The tape-flipped echo: everything you play comes back BACKWARDS, one segment
// behind. Swells bloom out of nothing, picks arrive as reverse-attack whooshes.
//
// ---------------------------------------------------------------------------
// How a reverse tap actually works
// ---------------------------------------------------------------------------
// Reading a delay line backwards at speed 1 means the read position moves
// -1 sample per output sample while the write head moves +1, so the TAP DELAY
// grows at 2 samples per sample:
//
//     readAbs(t) = A - (t - t0)   =>   delay(t) = t - readAbs(t) = 2*(t - t0) + d0
//
// A segment of L samples therefore sweeps the tap from ~0 to 2L — which is why
// a reverse delay needs a buffer of TWICE the segment length, not L.
//
// ---------------------------------------------------------------------------
// The seams (this is the part that clicks if you get it wrong)
// ---------------------------------------------------------------------------
// A single reverse head restarts at the top of every segment, splicing the
// END of one chunk of audio onto the START of the next — a hard discontinuity
// once per segment, i.e. a periodic click at 1/L Hz.
//
// So we run TWO reverse heads a half-segment apart, each windowed by
// sin(pi*phase) — the head fades from silence at its wrap-in and back to
// silence at its wrap-out, so no head is ever audible at its own splice point.
// The two windows are power-complementary:
//
//     sin^2(pi*p) + sin^2(pi*(p+0.5)) = sin^2 + cos^2 = 1
//
// so the pair sums to constant POWER (the two heads read audio a full segment
// apart — decorrelated material, where power, not amplitude, is what adds).
//
// ---------------------------------------------------------------------------
// Why `feedback` stops at 0.7 (a real bound, not a taste call)
// ---------------------------------------------------------------------------
// Constant-power gains mean the pair's AMPLITUDE sum peaks at sqrt(2) (both
// heads at 0.707, phase 0.25). A sustained tone whose period divides the
// segment IS coherent across the two heads, so that sqrt(2) is reachable, not
// theoretical. Loop gain <= feedback * sqrt(2), and 0.7 * sqrt(2) = 0.99 < 1 —
// bounded for any input. The recirculation additionally passes a soft limiter
// (4*tanh(v/4): within 0.5 % of unity for |v| <= 1, hard-bounded at +/-4) and a
// DC blocker, so even a pathological transient cannot latch the loop.
//
// Pure processing core — NO JUCE, NO I/O. Allocation only in prepare().
// process() never allocates, locks, or throws. latencySamples() == 0 (the dry
// path is a wire; the wet path is the effect, as with every other delay here).
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe.
struct ReverseDelayParams {
    float timeMs   = 500.0f;  // 100 .. 2000 — length of the reversed segment
    float feedback = 0.30f;   // 0 .. 0.7    — hard cap, see the header note
    float mix      = 0.50f;   // 0 .. 1      — dry .. wet
    float toneHz   = 4000.0f; // 500 .. 12000 — low-pass on the wet + feedback path
};

class ReverseDelayCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numCh_ = (numChannels > 0) ? numChannels : 1;

        // A reverse head sweeps the tap over TWICE the segment length.
        maxDelaySamples_ =
            static_cast<int>(std::ceil(2.0 * kMaxTimeMs * 0.001 * sr_)) + 8;

        lines_.assign(static_cast<std::size_t>(numCh_), InterpolatedDelayLine{});
        tone_.assign(static_cast<std::size_t>(numCh_), OnePole{});
        dc_.assign(static_cast<std::size_t>(numCh_), DcBlocker{});
        for (auto& l : lines_) l.prepare(maxDelaySamples_);
        for (auto& d : dc_)    d.prepare(sr_);

        smSeg_.reset(sr_, 0.080f);   // 80 ms glide on the segment length
        smMix_.reset(sr_, 0.015f);

        firstParams_ = true;
        setParams(p_);
        smSeg_.setCurrentAndTarget(segTgt_);
        smMix_.setCurrentAndTarget(mixTgt_);
        reset();
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& t : tone_)  t.reset();
        for (auto& d : dc_)    d.reset();
        phase_ = 0.0f;
        smSeg_.setCurrentAndTarget(segTgt_);
        smMix_.setCurrentAndTarget(mixTgt_);
    }

    void setParams(const ReverseDelayParams& p) {
        p_ = p;

        segTgt_ = clampf(p.timeMs, kMinTimeMs, kMaxTimeMs) * 0.001f
                  * static_cast<float>(sr_);
        mixTgt_ = clampf(p.mix, 0.0f, 1.0f);
        fb_     = clampf(p.feedback, 0.0f, kMaxFeedback);

        const float tone = clampf(p.toneHz, kMinToneHz, kMaxToneHz);
        if (firstParams_ || tone != toneHz_) {
            toneHz_ = tone;
            for (auto& t : tone_) t.setLowpass(sr_, toneHz_);
        }
        if (firstParams_) {
            smSeg_.setCurrentAndTarget(segTgt_);
            smMix_.setCurrentAndTarget(mixTgt_);
        }
        smSeg_.setTarget(segTgt_);
        smMix_.setTarget(mixTgt_);
        firstParams_ = false;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int nch = (numChannels < numCh_) ? numChannels : numCh_;

        const float minTap = 1.0f;
        const float maxTap = static_cast<float>(maxDelaySamples_) - 2.0f;

        for (int n = 0; n < numFrames; ++n) {
            const float seg = smSeg_.next();          // segment length, samples
            const float mix = smMix_.next();
            const float dry = 1.0f - mix;

            // One shared phase clock: both heads (and every channel) stay on the
            // same reverse grid, so a stereo image is not torn apart.
            phase_ += 1.0f / std::max(seg, 1.0f);
            if (phase_ >= 1.0f) phase_ -= std::floor(phase_);
            if (!(phase_ >= 0.0f && phase_ < 1.0f)) phase_ = 0.0f;   // NaN guard

            float p2 = phase_ + 0.5f;
            if (p2 >= 1.0f) p2 -= 1.0f;

            // Reverse taps: delay sweeps 1 .. 1 + 2*seg across a segment.
            const float d1 = clampf(1.0f + 2.0f * phase_ * seg, minTap, maxTap);
            const float d2 = clampf(1.0f + 2.0f * p2     * seg, minTap, maxTap);

            // Power-complementary grain windows (see the header note).
            // sin(pi * p) == fastSinTurns(p / 2); a grain window is a modulator.
            const float g1 = fastSinTurns(0.5f * phase_);
            const float g2 = fastSinTurns(0.5f * p2);

            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                const std::size_t sc = static_cast<std::size_t>(c);

                float x = buf[n];
                if (!std::isfinite(x)) x = 0.0f;

                // --- reverse read: two windowed backwards heads.
                const float rev = g1 * lines_[sc].readAt(d1)
                                + g2 * lines_[sc].readAt(d2);

                // --- shape the wet/feedback path (tape-ish roll-off, no DC drift).
                const float wet = dc_[sc].process(tone_[sc].process(rev));

                // --- recirculate. Soft-limited so the loop can never run away.
                lines_[sc].push(softLimit(x + fb_ * wet));

                float out = dry * x + mix * wet;
                if (!std::isfinite(out)) out = 0.0f;
                buf[n] = clampf(out, -kOutClamp, kOutClamp);
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    // 4*tanh(v/4): unity to within 0.5 % for |v| <= 1, hard-bounded at +/-4.
    // The repeats stay clean at playing levels; a runaway is impossible.
    // NOT ADAA (W4-D66): same two reasons as dcdsp_platereverb.h's. It is a
    // runaway bound that is unity to within 0.5 % for |v| <= 1, and it is in
    // the FEEDBACK path, where ADAA1's half-sample delay lengthens the loop.
    static float softLimit(float v) {
        if (!std::isfinite(v)) return 0.0f;
        return 4.0f * std::tanh(0.25f * v);
    }
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr float kMinTimeMs   = 100.0f;
    static constexpr float kMaxTimeMs   = 2000.0f;
    // 0.7 * sqrt(2) = 0.99 < 1: the two constant-power heads can sum coherently
    // to sqrt(2) on a periodic tone, so this cap is what bounds the loop.
    static constexpr float kMaxFeedback = 0.7f;
    static constexpr float kMinToneHz   = 500.0f;
    static constexpr float kMaxToneHz   = 12000.0f;
    static constexpr float kOutClamp    = 16.0f;

    ReverseDelayParams p_{};
    bool   firstParams_ = true;
    double sr_ = 48000.0;
    int    numCh_ = 2;
    int    maxDelaySamples_ = 1;

    std::vector<InterpolatedDelayLine> lines_;
    std::vector<OnePole>   tone_;
    std::vector<DcBlocker> dc_;

    SmoothedValue smSeg_, smMix_;
    float segTgt_ = 0.0f, mixTgt_ = 0.5f;
    float fb_ = 0.30f;
    float toneHz_ = 4000.0f;
    float phase_ = 0.0f;   // shared reverse-grid phase in [0,1)
};

} // namespace dcdsp
