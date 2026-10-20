// dcdsp_looper.h — phrase looper (pure dcdsp core, RT-safe).
//
// A footswitch looper: Record a phrase, Play it back under your live playing,
// Overdub layers onto it, Undo the last layer, and read the loop backwards or at
// half speed. The live input is ALWAYS a wire at unity — the loop is added to
// it, never mixed against it, because a looper that ducks your own playing is
// unplayable.
//
// Topology: one interleaved loop buffer + one interleaved UNDO buffer, both
// sized in prepare() for the WORST CASE (kMaxLoopSeconds at the prepared rate,
// see the memory note below). Record writes forward; Play/Overdub read at a
// fractional position that wraps modulo the loop length; Overdub sums the input
// into the loop, scaled by `overdubDecay`. Every state change crossfades the
// loop's own output over 10 ms; the dry path never moves.
//
// THE THREE THINGS THAT ARE EASY TO GET WRONG:
//  1. The UNDO snapshot is NOT a memcpy inside the callback that saw the
//     footswitch — up to 92 MB of it. It is drained INCREMENTALLY at 2 frames
//     per output sample, starting AHEAD of the overdub write head (which moves
//     at 1 frame per sample), so the copy can never be overtaken and the whole
//     snapshot lands half a loop after the overdub begins. Same shape as
//     dcdsp_freeze.h's capture. The undo itself is a std::swap of the two
//     vectors: a pointer exchange, no allocation, no copy.
//  2. The loop is USER CONTENT. prepare() must not clear it — a device change
//     mid-set would otherwise delete the take. What prepare() cannot preserve is
//     the RATE: a loop recorded at 48 kHz and re-prepared at 192 kHz is read at
//     the new rate and therefore plays four times fast, two octaves up. That is
//     a deliberate, documented trade against resampling megabytes on a device
//     change; keeping the content is the lesser surprise.
//  3. Overdub is REFUSED while `reverse` or `halfSpeed` is engaged. The write
//     head would have to follow a fractional, backwards read position, which
//     both smears the write and breaks (1)'s "the copy outruns the writer"
//     invariant. The loop still plays; it just takes no new material.
//
// MEMORY. Both buffers are allocated for kMaxLoopSeconds (60 s) at the prepared
// rate, for RT safety: `maxLoopS` is a musical CAP on how long a record pass may
// run, never a geometry change, so no runtime parameter can resize a buffer that
// process() indexes (dcdsp_whammy's `lowLatency` shipped exactly that bug). The
// cost, per node, is 60 * rate * channels * 4 bytes * 2:
//     48 kHz stereo   23.04 MB loop + 23.04 MB undo =  46.08 MB
//    192 kHz stereo   92.16 MB loop + 92.16 MB undo = 184.32 MB
// That is large and deliberate. Do not instantiate one per seat casually.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <atomic>
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <utility>

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe.
struct LooperParams {
    float state        = 0.0f;   // 0 Stop, 1 Record, 2 Play, 3 Overdub
    float maxLoopS     = 30.0f;  // 1 .. 60 (step 1) — record-length cap
    float loopLevel    = 1.0f;   // 0 .. 1
    float overdubDecay = 1.0f;   // 0 .. 1 — the existing loop's feedback
    float reverse      = 0.0f;   // 0 Off, 1 On
    float halfSpeed    = 0.0f;   // 0 Off, 1 On
    float undoSeq      = 0.0f;   // TRIGGER: any CHANGE performs one undo
    float mix          = 1.0f;   // 0 .. 1 — the loop's blend into the output
};

class LooperCore {
public:
    enum State { Stop = 0, Record = 1, Play = 2, Overdub = 3 };

    static constexpr float kMaxLoopSeconds = 60.0f;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        const int nch = (numChannels > 0) ? numChannels : 1;
        // The buffers are frame-major (frame * numCh + ch), so a resize keeps
        // the recorded frames only while the channel count is unchanged.
        if (nch != numCh_) { loopLen_ = 0; recLen_ = 0; snapValid_ = false; }

        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numCh_    = nch;
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 1;

        capacity_ = static_cast<int>(kMaxLoopSeconds * sr_);
        if (capacity_ < 1) capacity_ = 1;

        const std::size_t want = static_cast<std::size_t>(capacity_) * static_cast<std::size_t>(numCh_);
        loop_.resize(want, 0.0f);   // resize, not assign: the take survives
        undo_.resize(want, 0.0f);

        // A rate DROP shrinks the buffer under the take; the tail is gone.
        if (loopLen_ > capacity_) { loopLen_ = capacity_; snapValid_ = false; }
        if (recLen_  > capacity_) recLen_  = capacity_;

        smPlay_.reset(sr_, kStateFadeSeconds);
        smLevel_.reset(sr_, kLevelRampSeconds);
        smMix_.reset(sr_, kLevelRampSeconds);
        applyParams();
        smPlay_.snapToTarget();
        smLevel_.snapToTarget();
        smMix_.snapToTarget();

        if (loopLen_ > 0) { if (pos_ >= loopLen_) pos_ = 0.0; }
        else              pos_ = 0.0;
    }

    // Clears PLAYBACK state only. The recorded loop is user content: a bypass
    // that erased the take a player just built would be the worst footgun in the
    // catalogue, so reset() deliberately leaves loop_ and loopLen_ alone.
    void reset() {
        pos_ = 0.0;
        snapPending_ = false;
        snapCopied_  = 0;
        smPlay_.snapToTarget();
        smLevel_.snapToTarget();
        smMix_.snapToTarget();
    }

    void setParams(const LooperParams& p) {
        p_ = p;
        applyParams();

        // Compared against the last REQUESTED transport, never against the live
        // one: a record pass that closed itself on `maxLoopS` moves state_ to
        // Play on the audio thread while the param still reads Record, and
        // comparing against state_ would make the very next parameter write of
        // ANY kind look like a fresh Record press and wipe the take.
        const int want = clampi(static_cast<int>(std::lround(p.state)), 0, 3);
        // Published UNCONDITIONALLY, including when it compares equal: it is how
        // the message thread knows its write has crossed the mailbox at all.
        pubSeen_.store(want, std::memory_order_relaxed);
        if (want != reqState_) { reqState_ = want; transitionTo(want); }

        // The undo TRIGGER: the adapter bumps a sequence rather than sending a
        // 1 the audio thread would have to see fall back to 0. A coalesced pair
        // of presses is one undo, which is the right answer for a footswitch.
        if (p.undoSeq != undoSeq_) { undoSeq_ = p.undoSeq; performUndo(); }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;   // oversized: skipped WHOLE

        const int nch = (numChannels < numCh_) ? numChannels : numCh_;
        const int cap = recordCap();
        const bool overdubbing = (state_ == Overdub) && loopLen_ > 0 && canOverdub();
        const double inc = (halfSpeed_ ? 0.5 : 1.0) * (reverse_ ? -1.0 : 1.0);

        for (int n = 0; n < numFrames; ++n) {
            const float play = smPlay_.next();
            const float lvl  = smLevel_.next();
            const float mix  = smMix_.next();
            const float wet  = play * lvl * mix;

            // (1) the incremental UNDO snapshot, ALWAYS ahead of the writer.
            if (snapPending_) advanceSnapshot();

            // The cap is re-read every block because `maxLoopS` is a LIVE
            // knob, so it can drop BELOW recLen_ mid-pass -- and then the record
            // branch below is never entered again, which used to take the
            // auto-close with it because the close lived INSIDE it. The result
            // was a wedge: recording stopped, loopLen_ stayed 0, the `continue`
            // below took every sample, and the take so far sat in loop_
            // unreachable until the player changed `state` by hand.
            if (state_ == Record && recLen_ >= cap) closeRecording(Play);

            if (state_ == Record && recLen_ < cap) {
                for (int c = 0; c < nch; ++c) {
                    float* p = channels[c];
                    if (p == nullptr) continue;
                    float v = p[n];
                    if (!std::isfinite(v)) v = 0.0f;
                    loop_[frameIndex(recLen_, c)] = v;
                }
                ++recLen_;
                if (recLen_ >= cap) closeRecording(Play);   // full: auto-play
                continue;   // the loop is silent while it is being recorded
            }

            if (loopLen_ <= 0) continue;   // nothing to play: dry is already out

            const int i0 = static_cast<int>(pos_);
            const int i1 = (i0 + 1 < loopLen_) ? (i0 + 1) : 0;
            const float frac = static_cast<float>(pos_ - static_cast<double>(i0));

            for (int c = 0; c < nch; ++c) {
                float* p = channels[c];
                if (p == nullptr) continue;
                float in = p[n];
                if (!std::isfinite(in)) in = 0.0f;

                const float a = loop_[frameIndex(i0, c)];
                const float b = loop_[frameIndex(i1, c)];
                const float s = a + frac * (b - a);

                if (overdubbing)
                    loop_[frameIndex(i0, c)] = a * decay_ + in;

                // The live input is a WIRE; the loop is ADDED to it.
                p[n] = in + wet * s;
            }

            pos_ += inc;
            if (pos_ >= static_cast<double>(loopLen_)) pos_ -= static_cast<double>(loopLen_);
            if (pos_ < 0.0)                            pos_ += static_cast<double>(loopLen_);
        }
    }

    int latencySamples() const { return 0; }

    // ---- introspection for the native tools/tests --------------------------
    int   loopLengthFrames() const { return loopLen_; }
    /// The transport the ENGINE is running, which is not always the one that was
    /// last requested: a record pass that fills `maxLoopS` closes itself.
    /// Message-thread safe.
    int   transportState() const { return pubState_.load(std::memory_order_relaxed); }
    /// The last `state` value the audio thread has SEEN out of the mailbox --
    /// the player's request, never the engine's own override of it. A getter
    /// compares this with the value it wrote: equal means the core has caught
    /// up and `transportState()` is the honest answer; unequal means the write
    /// is still in flight and the requested value is.
    int   lastSeenRequest() const { return pubSeen_.load(std::memory_order_relaxed); }
    int   capacityFrames()   const { return capacity_; }
    bool  undoAvailable()    const { return snapValid_; }
    // Bytes both rings cost at the prepared rate/channel count.
    std::size_t bufferBytes() const {
        return (loop_.size() + undo_.size()) * sizeof(float);
    }

private:
    std::size_t frameIndex(int frame, int ch) const {
        return static_cast<std::size_t>(frame) * static_cast<std::size_t>(numCh_)
             + static_cast<std::size_t>(ch);
    }

    int recordCap() const {
        const int byKnob = static_cast<int>(clampf(p_.maxLoopS, 1.0f, kMaxLoopSeconds) * sr_);
        return (byKnob < capacity_) ? byKnob : capacity_;
    }

    bool canOverdub() const { return !reverse_ && !halfSpeed_; }

    void applyParams() {
        decay_     = clampf(p_.overdubDecay, 0.0f, 1.0f);
        reverse_   = p_.reverse   >= 0.5f;
        halfSpeed_ = p_.halfSpeed >= 0.5f;
        smLevel_.setTarget(clampf(p_.loopLevel, 0.0f, 1.0f));
        smMix_.setTarget(clampf(p_.mix, 0.0f, 1.0f));
    }

    void transitionTo(int want) {
        const int from = state_;
        state_ = want;

        if (want == Record) {
            // Record always starts a NEW loop. The alternative (record over the
            // existing one) is what Overdub is for.
            loopLen_ = 0;
            recLen_  = 0;
            pos_     = 0.0;
            snapPending_ = false;
            snapValid_   = false;
            smPlay_.setTarget(0.0f);
            publishTransport();
            return;
        }

        if (from == Record) closeRecording(want);

        if (want == Overdub && loopLen_ > 0) beginSnapshot();
        smPlay_.setTarget((want == Stop || loopLen_ <= 0) ? 0.0f : 1.0f);
        publishTransport();
    }

    // Record -> anything: the recorded length BECOMES the loop length, bounded
    // by the CURRENT cap (`maxLoopS` may have been turned down mid-pass, and a
    // 5 s loop under a 2 s knob is not what the knob says).
    //
    // It also republishes `reqState_`. This close can be ENGINE-driven -- a pass
    // that filled the cap moves the transport to Play on the audio thread with
    // no param write behind it -- and leaving reqState_ on Record made the
    // player's next Record press compare equal and do NOTHING: they had to go
    // Stop -> Record to start a new take.
    void closeRecording(int want) {
        state_    = want;
        reqState_ = want;
        const int cap = recordCap();
        loopLen_  = (recLen_ < cap) ? recLen_ : cap;
        pos_      = 0.0;
        if (want == Overdub && loopLen_ > 0) beginSnapshot();
        smPlay_.setTarget((want == Stop || loopLen_ <= 0) ? 0.0f : 1.0f);
        publishTransport();
    }

    /// Hand the message thread the LIVE transport and the request the audio
    /// thread has actually APPLIED. Two values, not one: the adapter reports the
    /// player's own pending request until the core has seen it, and the live
    /// transport afterwards -- which is the only way a getter can be honest
    /// about an engine-driven change without lying about a fresh write that has
    /// not crossed the mailbox yet.
    void publishTransport() {
        pubState_.store(state_, std::memory_order_relaxed);
    }

    void beginSnapshot() {
        snapStart_   = static_cast<int>(pos_);
        if (snapStart_ < 0 || snapStart_ >= loopLen_) snapStart_ = 0;
        snapCopied_  = 0;
        snapPending_ = true;
        snapValid_   = false;
    }

    // 2 frames per output sample, from the write head forward. The overdub
    // writer moves 1 frame per sample from the same start, so the copy leads it
    // by exactly as much as it has run and finishes after loopLen_/2 samples.
    void advanceSnapshot() {
        for (int k = 0; k < kSnapFramesPerSample && snapCopied_ < loopLen_; ++k) {
            int f = snapStart_ + snapCopied_;
            if (f >= loopLen_) f -= loopLen_;
            const std::size_t base = frameIndex(f, 0);
            for (int c = 0; c < numCh_; ++c)
                undo_[base + static_cast<std::size_t>(c)] = loop_[base + static_cast<std::size_t>(c)];
            ++snapCopied_;
        }
        if (snapCopied_ >= loopLen_) {
            snapPending_ = false;
            snapValid_   = true;
        }
    }

    // A pointer exchange, not a copy: std::vector::swap moves three pointers.
    // Refused while a snapshot is still draining — half of undo_ would be the
    // pre-overdub take and half whatever the buffer held before it.
    void performUndo() {
        if (!snapValid_ || loopLen_ <= 0) return;
        loop_.swap(undo_);
        // snapValid_ stays true, so a second press swaps back (redo). If you
        // kept overdubbing after the undo, that redo returns the PRE-UNDO take,
        // not what you have played since.
    }

    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static int clampi(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr float kStateFadeSeconds = 0.010f;
    static constexpr float kLevelRampSeconds = 0.015f;
    static constexpr int   kSnapFramesPerSample = 2;

    LooperParams p_{};
    double sr_       = 48000.0;
    int    numCh_    = 2;
    int    maxBlock_ = 1;
    int    capacity_ = 0;

    std::vector<float> loop_, undo_;
    int    loopLen_ = 0;
    int    recLen_  = 0;
    double pos_     = 0.0;
    int    state_    = Stop;   // what the transport IS
    int    reqState_ = Stop;   // what was last ASKED for (see setParams)
    // Published for the MESSAGE thread by publishTransport(); see the getters.
    std::atomic<int> pubState_{ Stop };
    std::atomic<int> pubSeen_{ Stop };

    bool  reverse_   = false;
    bool  halfSpeed_ = false;
    float decay_     = 1.0f;
    float undoSeq_   = 0.0f;

    bool snapPending_ = false;
    bool snapValid_   = false;
    int  snapStart_   = 0;
    int  snapCopied_  = 0;

    SmoothedValue smPlay_, smLevel_, smMix_;
};

} // namespace dcdsp
