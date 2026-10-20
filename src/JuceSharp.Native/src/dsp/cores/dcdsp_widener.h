// dcdsp_widener.h — stereo widener (mid/side + side-only Haas). Pure dcdsp core.
//
// A UTILITY, not a modulation effect: it opens the stereo image of whatever
// reaches it (a mono guitar, or the already-stereo output of a doubler /
// dimension / reverb) and is **MONO-SAFE BY CONSTRUCTION** — not "mono-safe if
// you keep the knobs low", but structurally, for every parameter setting.
//
// ---------------------------------------------------------------------------
// THE MONO-SAFETY GUARANTEE (this is the whole point of the module)
// ---------------------------------------------------------------------------
// The core works exclusively in mid/side:
//
//     M = 0.5*(L + R)                       <- NEVER touched
//     S = 0.5*(L - R)                       <- everything happens here
//     S' = width*S + haas * 0.5*(M - M[n-D])
//     S' = sideHp(S')                       (bass-mono: HPF on the SIDE only)
//     L' = M + S'    R' = M - S'
//
// A mono downmix is (L' + R')/2 = M = (L + R)/2 — the ORIGINAL mono sum,
// independent of width, haas, D and the bass-mono corner. Every widening
// contribution lives in S', and S' cancels identically in the sum. There is no
// comb filter to fall out, because the delayed copy NEVER reaches the mid path.
//
// That is the difference between this and a naive Haas widener (L = x,
// R = x[n-D]), whose mono sum IS 0.5*(x + x[n-D]) — a comb filter with nulls
// every 1/D Hz. We keep the Haas *cue* (the inter-channel time difference the
// ear localises on) by taking only the SIDE component of that pair,
// 0.5*(x - x[n-D]), and adding it to S. The L/R outputs still differ by a
// delayed copy of the mid, so the image widens exactly as a Haas pair does;
// the mono sum simply does not contain it.
//
// In exact arithmetic the fold is bit-identical. In float, (M+S') + (M-S')
// differs from 2M by at most 1 ulp of the intermediate sums — measured at
// <= -140 dBFS on the standalone harness, i.e. 20+ dB below a 24-bit LSB.
//
// `bassMonoHz` high-passes the SIDE signal, so everything below the corner
// collapses to the centre (the standard mastering move: solid mono low end,
// wide top). It is applied to S' only, so it cannot break the guarantee above.
//
// Channel handling: stereo pairs (0,1), (2,3), ... are widened independently; a
// MONO chain (numChannels == 1) passes through UNCHANGED — a single channel has
// no stereo image to open, and inventing one by writing a delayed copy into it
// would be exactly the comb filter this module exists to avoid. An odd trailing
// channel likewise passes through.
//
// Pure processing core — NO JUCE, NO I/O. Allocation only in prepare().
// process() never allocates, locks, or throws. latencySamples() == 0: the mid
// path is a straight wire; only the synthesized side is delayed.
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
struct WidenerParams {
    float width      = 1.4f;   // 0 .. 2    — M/S gain on the EXISTING side content
                               //             (0 = mono, 1 = unchanged, 2 = doubled)
    float haas       = 0.35f;  // 0 .. 1    — side synthesized from the mid (this is
                               //             what widens a MONO source)
    float delayMs    = 18.0f;  // 1 .. 40   — the Haas time used to synthesize it
    float bassMonoHz = 120.0f; // 0 .. 500  — side-path high-pass (0 = off): below
                               //             this corner everything stays centred
};

class WidenerCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numCh_ = (numChannels > 0) ? numChannels : 1;

        maxDelaySamples_ = static_cast<int>(std::ceil(kMaxDelayMs * 0.001 * sr_)) + 4;

        // One delay line + one side HPF per PAIR (pairs are widened independently).
        const int pairs = (numCh_ + 1) / 2;
        mid_.assign(static_cast<std::size_t>(pairs), InterpolatedDelayLine{});
        sideHp_.assign(static_cast<std::size_t>(pairs), OnePole{});
        for (auto& l : mid_) l.prepare(maxDelaySamples_);

        smDelay_.reset(sr_, 0.020f);   // 20 ms glide on the Haas time (no zipper)
        smWidth_.reset(sr_, 0.015f);
        smHaas_.reset(sr_, 0.015f);
        // 20 ms: bringing a whole high-pass in or out of the side path is a
        // structural change, not a knob move (W4-D22).
        smHpMix_.reset(sr_, 0.020f);

        // The line above wiped sideHp_ back to DEFAULT-CONSTRUCTED OnePoles,
        // and a default OnePole is a WIRE (mA0 = 1, mB1 = 0, mHighpass =
        // false), not a high-pass. applyParams() only re-designs them when the
        // frequency CHANGED (`hz != hpHz_`), and hpHz_ survives prepare — so
        // every prepare after the first left hpOn_ true over identity filters
        // and the bass-mono feature was silently, permanently off from the
        // first device change onward. Force the redesign.
        hpHz_ = -1.0f;
        applyParams();
        smDelay_.setCurrentAndTarget(delayTgt_);
        smWidth_.setCurrentAndTarget(widthTgt_);
        smHaas_.setCurrentAndTarget(haasTgt_);
        reset();
    }

    void reset() {
        for (auto& l : mid_)    l.reset();
        for (auto& h : sideHp_) h.reset();
        smDelay_.setCurrentAndTarget(delayTgt_);
        smWidth_.setCurrentAndTarget(widthTgt_);
        smHaas_.setCurrentAndTarget(haasTgt_);
        smHpMix_.snapToTarget();   // a freshly prepared widener starts AT its state
    }

    void setParams(const WidenerParams& p) {
        p_ = p;
        applyParams();
        smDelay_.setTarget(delayTgt_);
        smWidth_.setTarget(widthTgt_);
        smHaas_.setTarget(haasTgt_);
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int nch = (numChannels < numCh_) ? numChannels : numCh_;

        const float minTap = 1.0f;
        const float maxTap = static_cast<float>(maxDelaySamples_) - 2.0f;

        for (int n = 0; n < numFrames; ++n) {
            float tap = smDelay_.next();
            tap = clampf(tap, minTap, maxTap);
            const float width = smWidth_.next();
            const float haas  = smHaas_.next();
            const float hpw   = smHpMix_.next();   // 0 = side untouched, 1 = high-passed

            for (int c = 0; c + 1 < nch; c += 2) {
                float* pl = channels[c];
                float* pr = channels[c + 1];
                if (pl == nullptr || pr == nullptr) continue;
                const std::size_t pi = static_cast<std::size_t>(c / 2);

                float l = pl[n];
                float r = pr[n];
                if (!std::isfinite(l)) l = 0.0f;
                if (!std::isfinite(r)) r = 0.0f;

                // --- mid/side. The mid is a wire; everything happens in the side.
                const float m = 0.5f * (l + r);
                const float s = 0.5f * (l - r);

                // --- side synthesized from the mid: the SIDE half of a Haas pair.
                const float md = mid_[pi].readAt(tap);
                mid_[pi].push(m);
                float sOut = width * s + haas * 0.5f * (m - md);

                // --- bass-mono: high-pass the SIDE, so the low end stays centred.
                // W4-D22: the filter now runs UNCONDITIONALLY and its output is
                // CROSSFADED in. Two bugs lived in the old `if (hpOn_)`: crossing
                // kMinBassMonoHz switched a whole high-pass in or out of the side
                // path in one sample (at bassMonoHz 200 that is most of the side's
                // low end appearing or vanishing instantly), and a filter that is
                // skipped keeps whatever state it last held, so switching back ON
                // spliced a stale one-pole memory in behind it.
                const float hpOut = sideHp_[pi].process(sOut);
                sOut += hpw * (hpOut - sOut);

                float outL = m + sOut;
                float outR = m - sOut;
                if (!std::isfinite(outL)) outL = 0.0f;
                if (!std::isfinite(outR)) outR = 0.0f;
                pl[n] = clampf(outL, -kOutClamp, kOutClamp);
                pr[n] = clampf(outR, -kOutClamp, kOutClamp);
            }
            // An odd trailing channel (and a wholly mono chain) passes through:
            // a single channel carries no stereo image, and writing a delayed
            // copy into it is precisely the comb filter this module avoids.
        }
    }

    int latencySamples() const { return 0; }

private:
    void applyParams() {
        widthTgt_ = clampf(p_.width, 0.0f, 2.0f);
        haasTgt_  = clampf(p_.haas, 0.0f, 1.0f);
        delayTgt_ = clampf(p_.delayMs, kMinDelayMs, kMaxDelayMs)
                    * 0.001f * static_cast<float>(sr_);

        const float hz = clampf(p_.bassMonoHz, 0.0f, kMaxBassMonoHz);
        hpOn_ = (hz >= kMinBassMonoHz);
        smHpMix_.setTarget(hpOn_ ? 1.0f : 0.0f);
        // Design at the FLOOR while the control is below it, so the filter the
        // crossfade is about to bring in is always a real one and never a
        // default-constructed OnePole (which is a WIRE — the same trap that made
        // bass-mono silently permanently off before, see prepare()).
        const float designHz = hpOn_ ? hz : kMinBassMonoHz;
        if (designHz != hpHz_) {
            hpHz_ = designHz;
            for (auto& h : sideHp_) h.setHighpass(sr_, hpHz_);
        }
    }

    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr float kMinDelayMs    = 1.0f;
    static constexpr float kMaxDelayMs    = 40.0f;
    static constexpr float kMinBassMonoHz = 20.0f;   // below this the HPF is OFF
    static constexpr float kMaxBassMonoHz = 500.0f;
    static constexpr float kOutClamp      = 16.0f;

    WidenerParams p_{};
    double sr_ = 48000.0;
    int    numCh_ = 2;
    int    maxDelaySamples_ = 1;

    std::vector<InterpolatedDelayLine> mid_;    // one per stereo pair
    std::vector<OnePole>               sideHp_; // one per stereo pair

    SmoothedValue smDelay_, smWidth_, smHaas_;
    float delayTgt_ = 0.0f, widthTgt_ = 1.4f, haasTgt_ = 0.35f;
    float hpHz_ = -1.0f;
    bool  hpOn_ = true;
    SmoothedValue smHpMix_;   // 0 = side untouched, 1 = high-passed (W4-D22)
};

} // namespace dcdsp
