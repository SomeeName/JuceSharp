// dcdsp_glitch.h — granular beat-repeat / stutter (dcdsp core).
//
// Captures a slice of what you just played and loops it: the stutter, the
// buffer-repeat, the machine-gun chop. Two ways to fire it:
//
//   Manual — the footswitch. Flip `trigger` and the last `sliceMs` loops for as
//            long as you hold it. Release and you drop back into the live signal.
//   Auto   — a beat-repeat. Every `sliceMs` the core rolls the dice against
//            `chance`; when it wins it captures the slice and repeats it
//            `repeats` times, then hands the signal back. Set chance to 1 for a
//            relentless chop, to 0.2 for something that ambushes you.
//
// THE LOOP SEAM (why this does not click). A naive stutter loops [0, L) and
// clicks every wrap, because sample L-1 and sample 0 are strangers. This core
// captures L + X samples, loops the first L, and crossfades the head of each
// iteration against slice[p + L] — the audio that ACTUALLY followed slice[p] in
// real time. At the wrap the output is therefore continuous by construction
// (p = L-1 ends on slice[L-1]; p = 0 begins on slice[L], its true successor) and
// it has morphed back to the loop head X samples later. That is a real
// crossfade, not a window taped over the discontinuity.
//
// Reverse is the exception: played backwards, the seam joins slice[0] to
// slice[L-1] and no captured audio bridges them, so each pass gets a short
// raised-cosine window. The resulting chop IS the sound of every reverse-stutter
// pedal ever made.
//
// Contract: namespace dcdsp, C++17, header-only, zero framework deps. Allocation
// happens ONLY in prepare(); process() never allocates, locks, or throws.
//
// THE CAPTURE IS INCREMENTAL, and that is not cosmetic. It used to copy the
// whole slice — up to 1004 ms per channel — inside the ONE callback that saw
// the trigger. Allocation-free and bounded, yes, but at 192 kHz stereo that is
// 1.5 MB of memcpy in a callback whose budget at a 32-sample buffer is 167 us:
// a guaranteed dropout at the exact instant the player stamps the footswitch,
// which is the worst possible moment for one. dcdsp_freeze.h had already solved
// it and this core now uses the same shape: the history ring is sized at TWICE
// the longest capture and keeps recording straight through, the snapshot is
// copied out at kCopyRate samples per output sample, and playback reads
// whichever side already holds the data (readCaptured). The copy finishes in
// capN/kCopyRate samples while the ring keeps the region alive for at least
// capN more, so the read can never outrun it. Cost is flat: two extra sample
// copies per sample, and only until the snapshot is complete.
//
// Output is a replay of the input at unit gain, so it is bounded by the input;
// a final clamp and non-finite scrub guard the rest.
#pragma once

#include "dcdsp_primitives.h"

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dcdsp {

namespace glitch_detail {

// xorshift32 — deterministic, allocation-free, constant-seeded so a render is
// reproducible.
struct Rng {
    std::uint32_t s = 0x2545F491u;
    void reset() { s = 0x2545F491u; }
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float nextUnit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
};

} // namespace glitch_detail

struct GlitchParams {
    // 0 = Manual (the footswitch), 1 = Auto (beat-repeat).
    int mode = 0;
    // Manual only: 0 = Off, 1 = On (stutter while held).
    int trigger = 0;
    // Length of the captured slice. [20, 1000] ms
    float sliceMs = 250.0f;
    // Auto only: how many times a captured slice repeats. [1, 16]
    int repeats = 4;
    // Auto only: probability that any given slice boundary fires a capture. [0, 1]
    float chance = 0.35f;
    // Level lost per repeat — 0 holds forever, 1 fades the stutter out. [0, 1]
    float decay = 0.0f;
    // 0 = Forward, 1 = Reverse.
    int reverse = 0;
    // Dry/wet blend. [0, 1]
    float mix = 1.0f;
};

class GlitchCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxCh_ = (numChannels > 0) ? numChannels : 1;

        xfade_   = std::max(8, static_cast<int>(kXfadeMs * 0.001 * sr_));
        capMax_  = static_cast<int>(kMaxSliceMs * 0.001 * sr_) + xfade_ + 8;
        // 2x the capture window, power-of-two so the wrap is a mask: the ring
        // keeps recording through the stutter and the incremental snapshot must
        // finish before it laps the captured region.
        ringSize_ = nextPow2(2 * capMax_ + 8);
        ringMask_ = ringSize_ - 1;

        chans_.clear();
        chans_.resize(static_cast<std::size_t>(maxCh_));
        for (auto& ch : chans_) {
            ch.history.assign(static_cast<std::size_t>(ringSize_), 0.f);
            ch.slice.assign(static_cast<std::size_t>(capMax_), 0.f);
        }

        // 8 ms enter/exit ramp — the stutter arrives and leaves without a click.
        wetRamp_.reset(sr_, 0.008f);
        wetRamp_.setCurrentAndTarget(0.f);
        // `mix` RAMPS (kMixRampSeconds). wetRamp_ only covers the trigger's
        // enter/exit; the KNOB rode on top of it as a per-block constant, so a
        // mix drag stepped a full dry/wet crossfade at every block edge.
        mixSmooth_.reset(sr_, kMixRampSeconds);
        mixSmooth_.setTarget(mix_);
        mixSmooth_.snapToTarget();   // a freshly prepared stutter starts AT its mix
        reset();
    }

    void reset() {
        for (auto& ch : chans_) {
            std::fill(ch.history.begin(), ch.history.end(), 0.f);
            std::fill(ch.slice.begin(), ch.slice.end(), 0.f);
        }
        rng_.reset();
        writeAbs_     = 0;
        anchor_       = 0;
        capN_         = 0;
        copyDone_     = 0;
        playing_      = false;
        playPos_      = 0;
        loopLen_      = 0;
        repeatIndex_  = 0;
        repeatGain_   = 1.f;
        repeatGainPrev_ = 1.f;
        gainFade_     = 0;
        autoClock_    = 0;
        lastTrigger_  = 0;
        wetRamp_.setCurrentAndTarget(0.f);
    }

    void setParams(const GlitchParams& p) {
        mode_    = clampi(p.mode, 0, 1);
        sliceMs_ = clamp(p.sliceMs, 20.f, kMaxSliceMs);
        repeats_ = clampi(p.repeats, 1, 16);
        chance_  = clamp(p.chance, 0.f, 1.f);
        decay_   = clamp(p.decay, 0.f, 1.f);
        reverse_ = clampi(p.reverse, 0, 1);
        mix_     = clamp(p.mix, 0.f, 1.f);
        mixSmooth_.setTarget(mix_);

        const int trig = (p.trigger >= 1) ? 1 : 0;
        if (trig != lastTrigger_) {
            pendingTrigger_ = true;   // consumed on the next frame (RT-safe edge)
            lastTrigger_    = trig;
        }
        trigger_ = trig;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int C = std::min(numChannels, static_cast<int>(chans_.size()));
        if (C <= 0 || ringSize_ <= 0) return;

        const int sliceSamples = std::max(16,
            static_cast<int>(static_cast<double>(sliceMs_) * 0.001 * sr_));

        for (int n = 0; n < numFrames; ++n) {
            // ---- write history -------------------------------------------------
            for (int c = 0; c < C; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                float in = buf[n];
                if (!std::isfinite(in)) { in = 0.f; buf[n] = 0.f; }
                chans_[static_cast<std::size_t>(c)]
                    .history[ringIdx(writeAbs_)] = clamp(in, -16.f, 16.f);
            }

            // ---- transport -----------------------------------------------------
            if (mode_ == 0) {
                // Manual: the footswitch owns the loop.
                if (pendingTrigger_) {
                    pendingTrigger_ = false;
                    if (trigger_ == 1) capture(C, sliceSamples);
                    else               release();
                }
            } else {
                // Auto: a free-running slice clock rolls the dice at each boundary.
                if (++autoClock_ >= sliceSamples) {
                    autoClock_ = 0;
                    if (!playing_ && rng_.nextUnit() < chance_)
                        capture(C, sliceSamples);
                }
                pendingTrigger_ = false;
            }

            // ---- incremental snapshot: kCopyRate samples per output sample ----
            if (copyDone_ < capN_) {
                for (int k = 0; k < kCopyRate && copyDone_ < capN_; ++k) {
                    const std::size_t src =
                        ringIdx(anchor_ - static_cast<long long>(capN_)
                                        + static_cast<long long>(copyDone_));
                    for (int c = 0; c < C; ++c) {
                        Channel& ch = chans_[static_cast<std::size_t>(c)];
                        ch.slice[static_cast<std::size_t>(copyDone_)] = ch.history[src];
                    }
                    ++copyDone_;
                }
            }

            const float w = wetRamp_.next() * mixSmooth_.next();
            // ONCE per frame, above the channel loop: repeatGainNow() advances
            // the ramp, and calling it per channel would walk it C times too
            // fast and hand each channel a different gain.
            const float repGain = repeatGainNow();

            // ---- render ---------------------------------------------------------
            for (int c = 0; c < C; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                Channel& ch = chans_[static_cast<std::size_t>(c)];

                const float dry = ch.history[ringIdx(writeAbs_)];
                float wet = dry;

                if (loopLen_ > 0 && w > 0.f) {
                    wet = readSlice(ch, playPos_) * repGain;
                    if (!std::isfinite(wet)) wet = 0.f;
                }

                float out = (1.f - w) * dry + w * wet;
                if (!std::isfinite(out)) out = 0.f;
                buf[n] = clamp(out, -16.f, 16.f);
            }

            // ---- advance the loop ----------------------------------------------
            if (loopLen_ > 0) {
                if (++playPos_ >= loopLen_) {
                    playPos_ = 0;
                    ++repeatIndex_;
                    // The WAVEFORM is continuous across the wrap -- readSlice's
                    // `p + loopLen_` bridge goes to real trouble to make it so --
                    // and then both sides of that splice were multiplied by a
                    // gain that jumped 40 % at the same sample. A click every lap
                    // with decay > 0. Ramp it across the same xfade_ region the
                    // waveform is bridged over.
                    repeatGainPrev_ = repeatGain_;
                    repeatGain_    *= (1.f - 0.4f * decay_);
                    gainFade_       = xfade_;

                    // Auto mode hands the signal back after `repeats` passes;
                    // Manual keeps looping until the switch is released.
                    if (playing_ && mode_ == 1 && repeatIndex_ >= repeats_)
                        release();
                }
            }

            // The loop is torn down only once the wet ramp has actually reached 0
            // — that is what makes the exit click-free.
            if (!playing_ && wetRamp_.current() <= 1.0e-4f && loopLen_ > 0) {
                loopLen_    = 0;
                playPos_    = 0;
                repeatIndex_ = 0;
                repeatGain_ = 1.f;
                repeatGainPrev_ = 1.f;
                gainFade_ = 0;
                capN_       = 0;
                copyDone_   = 0;
            }

            ++writeAbs_;
        }
    }

    int latencySamples() const { return 0; }

private:
    struct Channel {
        std::vector<float> history;   // rolling input ring
        std::vector<float> slice;     // the captured, linearized slice
    };

    static float clamp(float v, float lo, float hi) {
        if (!(v == v)) return lo;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // ARM the snapshot of the last (loopLen + xfade) samples. Copies NOTHING
    // here: it records where the window is and lets the frame loop drain it at
    // kCopyRate samples per sample, while readCaptured serves reads out of the
    // still-recording ring until the copy has caught up. O(1) per trigger.
    void capture(int /*C*/, int sliceSamples) {
        const int need = std::min(capMax_, sliceSamples + xfade_);
        loopLen_ = need - xfade_;
        if (loopLen_ < 16) { loopLen_ = 0; capN_ = 0; copyDone_ = 0; return; }

        // history[writeAbs_] holds the CURRENT sample, so the window ENDS one
        // past it and the newest `need` samples are [anchor_ - need, anchor_).
        capN_     = need;
        anchor_   = writeAbs_ + 1;
        copyDone_ = 0;

        playing_     = true;
        playPos_     = 0;
        repeatIndex_ = 0;
        repeatGain_  = 1.f;
        wetRamp_.setTarget(1.f);
    }

    void release() {
        playing_ = false;
        wetRamp_.setTarget(0.f);
    }

    // Forward: crossfade the head of each iteration against the audio that really
    // followed it (slice[p + loopLen]), so the wrap is continuous.
    // Reverse: window each pass — a backwards seam has no captured bridge.
    /// The repeat gain for THIS sample: a linear ramp from the previous lap's
    /// gain to the current one across the xfade_ region the waveform is
    /// bridged over, so the splice the bridge removes is not re-introduced by
    /// the level multiplying both of its sides.
    float repeatGainNow() {
        if (gainFade_ <= 0) return repeatGain_;
        const float t = 1.f - static_cast<float>(gainFade_) / static_cast<float>(xfade_);
        --gainFade_;
        return repeatGainPrev_ + (repeatGain_ - repeatGainPrev_) * t;
    }

    float readSlice(const Channel& ch, int p) const {
        if (loopLen_ <= 0) return 0.f;

        if (reverse_ != 0) {
            const int idx = loopLen_ - 1 - p;
            const int edge = std::min(p, loopLen_ - 1 - p);
            float g = 1.f;
            if (edge < xfade_) {
                const float t = static_cast<float>(edge) / static_cast<float>(xfade_);
                g = 0.5f * (1.f - std::cos(static_cast<float>(kPi) * t));
            }
            return readCaptured(ch, idx) * g;
        }

        const float head = readCaptured(ch, p);
        if (p >= xfade_) return head;

        const float tail = readCaptured(ch, p + loopLen_);
        const float t    = static_cast<float>(p) / static_cast<float>(xfade_);
        return tail * (1.f - t) + head * t;
    }

    // The snapshot, wherever it currently lives. `i` indexes [0, capN_). Below
    // copyDone_ the incremental copy has landed it in slice_; at or above it the
    // 2x-oversized, still-recording ring is guaranteed to still hold it.
    float readCaptured(const Channel& ch, int i) const {
        if (capN_ <= 0) return 0.f;
        if (i < 0) i = 0;
        if (i >= capN_) i = capN_ - 1;
        if (i < copyDone_) return ch.slice[static_cast<std::size_t>(i)];
        return ch.history[ringIdx(anchor_ - static_cast<long long>(capN_)
                                          + static_cast<long long>(i))];
    }

    std::size_t ringIdx(long long abs) const {
        return static_cast<std::size_t>(abs & static_cast<long long>(ringMask_));
    }

    static int nextPow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    static constexpr float kMaxSliceMs = 1000.0f;
    static constexpr double kXfadeMs   = 4.0;
    static constexpr int   kCopyRate   = 2;   // snapshot samples per output sample

    double sr_    = 48000.0;
    int    maxCh_ = 2;

    int mode_    = 0;
    int trigger_ = 0;
    float sliceMs_ = 250.0f;
    int   repeats_ = 4;
    float chance_  = 0.35f;
    float decay_   = 0.0f;
    int   reverse_ = 0;
    float mix_     = 1.0f;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixSmooth_;

    std::vector<Channel>  chans_;
    glitch_detail::Rng    rng_{};
    SmoothedValue         wetRamp_{};

    int   capMax_      = 0;
    int   ringSize_    = 0;
    int   ringMask_    = 0;
    int   xfade_       = 192;
    long long writeAbs_ = 0;
    long long anchor_   = 0;
    int   capN_        = 0;
    int   copyDone_    = 0;
    bool  playing_     = false;
    int   playPos_     = 0;
    int   loopLen_     = 0;
    int   repeatIndex_ = 0;
    float repeatGain_  = 1.f;
    float repeatGainPrev_ = 1.f;   // the gain the PREVIOUS lap played at
    int   gainFade_    = 0;        // samples of repeat-gain ramp still owed
    int   autoClock_   = 0;
    int   lastTrigger_ = 0;
    bool  pendingTrigger_ = false;
};

} // namespace dcdsp
