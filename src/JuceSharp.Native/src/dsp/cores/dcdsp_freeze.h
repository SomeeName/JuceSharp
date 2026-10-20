// dcdsp_freeze.h — infinite sustain / sustain-pad (pure dcdsp core, RT-safe).
//
// Stomp it and the note you are holding hangs there FOREVER, as a pad, while
// your guitar keeps playing over the top of it (the dry path is a wire — this
// is a sustain pedal, not a wet effect). Release it and the pad fades out.
//
// ---------------------------------------------------------------------------
// The capture (footswitch-style latch)
// ---------------------------------------------------------------------------
// `capture` is a LATCH, not a trigger: 1 = held, 0 = released. The RISING EDGE
// (0 -> 1) snapshots the last `lengthMs` of input; the value staying at 1 does
// nothing further, so a preset that re-sends capture = 1 never re-captures.
//
// (A preset should never SHIP capture = 1: at apply time the buffer holds
// whatever was in the room, or silence. Capture is a performance gesture.)
//
// ---------------------------------------------------------------------------
// Why the loop does not click, and why the capture does not spike
// ---------------------------------------------------------------------------
// SEAM: the loop wraps from its end straight back to its start, which is a hard
// waveform discontinuity — one click per lap, forever. We crossfade it out over
// the last `kXfadeMs` of the lap against the PRE-ROLL: the real recorded audio
// immediately BEFORE the loop start, which the ring still holds. Equal-power
// (cos/sin) crossfade, and at the wrap instant the fade-in tap has arrived
// exactly at the loop's first sample — so the wrap is continuous, not merely
// masked. Nothing is invented; both sides of the seam are audio the player
// actually played.
//
// SPIKE: the naive capture is a memcpy of up to 2 s * sr * channels inside one
// audio callback (~380 us for stereo at 192 kHz — a guaranteed dropout at a
// 32-sample buffer). Instead the ring is sized at 2x the capture window and
// keeps recording straight through the freeze; the snapshot is copied out
// INCREMENTALLY at 2 samples per output sample, while playback reads whichever
// side already has the data (`readCaptured`). The copy therefore finishes in
// loopLen/2 samples, and the ring's oldest still-needed sample survives for
// ringSize - (loopLen + xfade) >= loopLen samples — twice the grace it needs.
// Cost is flat: 2 extra sample copies per sample, and only until the snapshot
// is complete. No allocation, no lock, no burst.
//
// Because the ring never stops recording, a release followed immediately by a
// re-capture gets a FULL, continuous `lengthMs` — there is no refill window.
//
// Pure processing core — NO JUCE, NO I/O. Allocation only in prepare().
// process() never allocates, locks, or throws. latencySamples() == 0.
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
struct FreezeParams {
    float capture  = 0.0f;    // 0/1 — LATCH. Rising edge captures; 0 releases.
    float lengthMs = 500.0f;  // 120 .. 2000 — captured loop length
    float level    = 0.7f;    // 0 .. 1   — the pad's level under your playing
    float fadeMs   = 80.0f;   // 5 .. 2000 — pad fade in on capture / out on release
    float toneHz   = 6000.0f; // 500 .. 12000 — pad low-pass (keeps it under the guitar)
};

class FreezeCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numCh_ = (numChannels > 0) ? numChannels : 1;

        maxLoop_  = static_cast<int>(std::ceil(kMaxLengthMs * 0.001 * sr_));
        minLoop_  = static_cast<int>(std::ceil(kMinLengthMs * 0.001 * sr_));
        xfade_    = static_cast<int>(std::ceil(kXfadeMs * 0.001 * sr_));
        capMax_   = maxLoop_ + xfade_;               // loop + pre-roll
        // 2x the capture window: the ring keeps recording through the freeze,
        // and the incremental snapshot must finish before it laps the region.
        ringSize_ = nextPow2(2 * capMax_ + 8);
        ringMask_ = ringSize_ - 1;

        ring_.assign(static_cast<std::size_t>(numCh_ * ringSize_), 0.0f);
        cap_.assign(static_cast<std::size_t>(numCh_ * capMax_), 0.0f);
        padLp_.assign(static_cast<std::size_t>(numCh_), OnePole{});

        applyTone(true);
        recomputeRateDerived();
        // The pad LEVEL ramps (kLevelRampSeconds). `gain_` is the capture/release
        // fade and never covered the knob: as a per-block constant a level drag
        // stepped a steady, sustained pad — the most exposed signal there is.
        levelS_.reset(sr_, kLevelRampSeconds);
        levelS_.setTarget(level_);
        levelS_.snapToTarget();   // a freshly prepared pad starts AT its level
        reset();
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        std::fill(cap_.begin(), cap_.end(), 0.0f);
        for (auto& f : padLp_) f.reset();
        writeAbs_ = 0;
        frozen_   = false;
        lastCapture_ = false;
        loopLen_  = 0;
        capN_     = 0;
        copyDone_ = 0;
        anchor_   = 0;
        phase_    = 0;
        gain_     = 0.0f;
        gainTarget_ = 0.0f;
    }

    void setParams(const FreezeParams& p) {
        p_ = p;
        recomputeRateDerived();
        level_ = clampf(p.level, 0.0f, 1.0f);
        levelS_.setTarget(level_);

        applyTone(false);

        // ---- the latch. Only the 0 -> 1 EDGE captures.
        const bool want = (p.capture >= 0.5f);
        if (want && !lastCapture_) beginCapture();
        else if (!want && lastCapture_) gainTarget_ = 0.0f;   // release: fade out
        lastCapture_ = want;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int nch = (numChannels < numCh_) ? numChannels : numCh_;

        for (int n = 0; n < numFrames; ++n) {
            // --- the ring records ALWAYS, freeze or not (see the header note).
            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                float x = (buf != nullptr) ? buf[n] : 0.0f;
                if (!std::isfinite(x)) x = 0.0f;
                if (buf != nullptr) buf[n] = x;            // scrubbed dry
                ring_[ringIdx(c, writeAbs_)] = x;
            }
            ++writeAbs_;

            // --- incremental snapshot: 2 samples per output sample.
            if (frozen_ && copyDone_ < capN_) {
                for (int k = 0; k < kCopyRate && copyDone_ < capN_; ++k) {
                    const long long src = anchor_ - static_cast<long long>(capN_)
                                        + static_cast<long long>(copyDone_);
                    for (int c = 0; c < nch; ++c)
                        cap_[capIdx(c, copyDone_)] = ring_[ringIdx(c, src)];
                    ++copyDone_;
                }
            }

            // --- pad gain ramp (linear, fadeMs).
            if (gain_ < gainTarget_) gain_ = std::min(gainTarget_, gain_ + gainStep_);
            else if (gain_ > gainTarget_) gain_ = std::max(gainTarget_, gain_ - gainStep_);

            // Walked BEFORE the release early-out below, so the level ramp keeps
            // wall-clock time whether or not the pad is currently sounding.
            const float lvl = levelS_.next();

            if (!frozen_ || gain_ <= 0.0f) {
                // Fully released: stop reading, so a re-capture starts clean.
                if (frozen_ && gain_ <= 0.0f && gainTarget_ <= 0.0f) frozen_ = false;
                continue;
            }

            // --- loop read with the pre-roll crossfade at the seam.
            const int p = phase_;
            const int idx = p + xfade_;                  // pre-roll occupies [0, xfade_)
            const int tailStart = loopLen_ - xfade_;     // seam region

            float w1 = 1.0f, w2 = 0.0f;
            bool blend = false;
            if (xfade_ > 0 && p >= tailStart) {
                const float t = static_cast<float>(p - tailStart)
                              / static_cast<float>(xfade_);   // 0 .. 1
                const float th = t * 0.5f * static_cast<float>(kPi);
                w1 = std::cos(th);                       // fade out the lap's end
                w2 = std::sin(th);                       // fade in the pre-roll
                blend = true;
            }

            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;

                float v = w1 * readCaptured(c, idx);
                if (blend) v += w2 * readCaptured(c, idx - loopLen_);

                v = padLp_[static_cast<std::size_t>(c)].process(v);
                if (!std::isfinite(v)) v = 0.0f;

                float out = buf[n] + v * gain_ * lvl;
                if (!std::isfinite(out)) out = 0.0f;
                buf[n] = clampf(out, -kOutClamp, kOutClamp);
            }

            if (++phase_ >= loopLen_) phase_ = 0;
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the two rate-derived counts recomputeRateDerived() owns, so a
    // gate case can assert both MOVED across a prepare at a different rate.
    int   loopLengthTargetForTest() const { return lengthTgt_; }
    float gainStepForTest()         const { return gainStep_; }

private:
    void beginCapture() {
        loopLen_  = lengthTgt_;
        capN_     = loopLen_ + xfade_;          // pre-roll + loop
        anchor_   = writeAbs_;                  // one past the newest sample
        copyDone_ = 0;
        phase_    = 0;
        frozen_   = true;
        gain_     = 0.0f;                       // fade in from silence
        gainTarget_ = 1.0f;
        for (auto& f : padLp_) f.reset();
    }

    // The snapshot, wherever it currently lives. `i` indexes the captured
    // window [0, capN_): [0, xfade_) is the pre-roll, [xfade_, capN_) the loop.
    // Below copyDone_ the incremental copy has already landed it in cap_; above
    // it the (still-recording, 2x-oversized) ring is guaranteed to still hold it.
    float readCaptured(int c, int i) const {
        if (i < 0) i = 0;
        if (i >= capN_) i = capN_ - 1;
        if (i < copyDone_) return cap_[capIdx(c, i)];
        const long long src = anchor_ - static_cast<long long>(capN_)
                            + static_cast<long long>(i);
        return ring_[ringIdx(c, src)];
    }

    std::size_t ringIdx(int c, long long abs) const {
        const long long i = abs & static_cast<long long>(ringMask_);
        return static_cast<std::size_t>(c) * static_cast<std::size_t>(ringSize_)
             + static_cast<std::size_t>(i);
    }
    std::size_t capIdx(int c, int i) const {
        return static_cast<std::size_t>(c) * static_cast<std::size_t>(capMax_)
             + static_cast<std::size_t>(i);
    }

    // Both of these are counts in SAMPLES, so both belong to the RATE and must
    // be re-derived by prepare() as well as by setParams() — the guide's named
    // "a rate-derived sample count only setParams owned" shape. Before this was
    // factored out, a 48 -> 96 kHz device change (or a NAM 48 kHz request, or a
    // watchdog rebuild) left the next capture at HALF the set length and the
    // fade at HALF the set time until an unrelated param write happened to fix
    // it. Must run after minLoop_/maxLoop_ have been derived.
    void recomputeRateDerived() {
        lengthTgt_ = clampi(
            static_cast<int>(clampf(p_.lengthMs, kMinLengthMs, kMaxLengthMs) * 0.001f
                             * static_cast<float>(sr_)),
            minLoop_, maxLoop_);

        const float fadeMs = clampf(p_.fadeMs, kMinFadeMs, kMaxFadeMs);
        gainStep_ = 1.0f / std::max(1.0f, fadeMs * 0.001f * static_cast<float>(sr_));
    }

    void applyTone(bool force) {
        const float hz = clampf(p_.toneHz, kMinToneHz, kMaxToneHz);
        if (!force && hz == toneHz_) return;
        toneHz_ = hz;
        for (auto& f : padLp_) f.setLowpass(sr_, toneHz_);
    }

    static int nextPow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static int clampi(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr float kMinLengthMs = 120.0f;
    static constexpr float kMaxLengthMs = 2000.0f;
    static constexpr float kMinFadeMs   = 5.0f;
    static constexpr float kMaxFadeMs   = 2000.0f;
    static constexpr float kMinToneHz   = 500.0f;
    static constexpr float kMaxToneHz   = 12000.0f;
    static constexpr float kXfadeMs     = 40.0f;   // loop-seam crossfade
    static constexpr int   kCopyRate    = 2;       // snapshot samples per output sample
    static constexpr float kLevelRampSeconds = 0.010f;
    static constexpr float kOutClamp    = 16.0f;

    FreezeParams p_{};
    double sr_ = 48000.0;
    int    numCh_ = 1;

    std::vector<float>   ring_;    // [ch][ringSize_] — always recording
    std::vector<float>   cap_;     // [ch][capMax_]   — the frozen snapshot
    std::vector<OnePole> padLp_;

    int  ringSize_ = 2, ringMask_ = 1;
    int  maxLoop_ = 1, minLoop_ = 1, xfade_ = 0, capMax_ = 1;
    long long writeAbs_ = 0;

    bool frozen_ = false, lastCapture_ = false;
    long long anchor_ = 0;
    int  loopLen_ = 0, capN_ = 0, copyDone_ = 0, phase_ = 0;

    int   lengthTgt_ = 0;
    float level_ = 0.7f;
    float gain_ = 0.0f, gainTarget_ = 0.0f, gainStep_ = 0.001f;
    SmoothedValue levelS_;
    float toneHz_ = -1.0f;
};

} // namespace dcdsp
