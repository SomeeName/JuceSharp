#pragma once

#include "DspModule.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

// Split / parallel-chain routing modules (ABI v23, 0.16 W5).
//
// AmpliTube-style multi-row routing for the tone rig. Two catalogue module
// types — `splitter` and `merger` — mark a parallel region inside the
// otherwise-linear DspChain; the chain executes the [splitter .. merger] span
// as 2-3 parallel branches (see DspChain::process). These classes live in
// their own header (not the factory's anonymous namespace) because DspChain
// needs the CONCRETE types on the audio thread: the splitter's enabled branch
// count and the merger's level/pan/mode are read per block via the lock-free
// accessors below (identified at publish time by typeId(), so no RTTI).
//
// Audio semantics (v1, documented in dotcharter_audio.h):
//   * every enabled branch starts from a COPY of the splitter's input;
//   * the merger OVERWRITES the main buffer with the per-branch level/pan mix;
//   * mode 0 ("average") scales the sum by 1/branchCount so an untouched
//     splitter->merger pair is a bit-exact passthrough; mode 1 is a raw sum;
//   * pan is a balance-law stereo pan (unity at centre — keeps the null test
//     exact); it only applies when the chain runs 2 channels;
//   * branch latency differences ARE compensated since ABI v34 (see below);
//     they were the v23 accepted limit until then.
//
// RT contract: both modules' process() is a no-op wire (the chain performs
// the actual copy/mix); mergeRt() never allocates, locks, or throws. Params
// are plain atomics — setParam runs on the message thread under the chain's
// write mutex while the audio thread reads relaxed snapshots, same tolerance
// as every other module's param handoff.
namespace dcrig
{

// Shared bounds for the split engine (DspChain sizes its scratch to these).
constexpr int kDspMaxBranches = 3;

// ---- Splitter ---------------------------------------------------------------
// Opens a parallel region. Params: enabledBranches (2..3, default 2).
class SplitterModule : public DspModule
{
public:
    void prepare(double, int, int) override {}
    void reset() override {}

    bool setParam(const char* id, float v) override
    {
        if (std::strcmp(id, "enabledBranches") != 0) return false;
        int n = (int) std::lround(v);
        if (n < 2) n = 2;
        if (n > kDspMaxBranches) n = kDspMaxBranches;
        branches_.store(n, std::memory_order_relaxed);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (std::strcmp(id, "enabledBranches") != 0) return false;
        out = (float) branches_.load(std::memory_order_relaxed);
        return true;
    }

    // The chain performs the branch copy; the node itself is a wire.
    void process(float* const*, int, int) override {}
    const char* typeId() const override { return "splitter"; }

    // Audio thread: enabled branch count, clamped to [2, kDspMaxBranches].
    int branchCountRt() const noexcept
    {
        int n = branches_.load(std::memory_order_relaxed);
        return n < 2 ? 2 : (n > kDspMaxBranches ? kDspMaxBranches : n);
    }

private:
    std::atomic<int> branches_{ 2 };
};

// ---- Merger -----------------------------------------------------------------
// Closes a parallel region and mixes the branches back into the main buffer.
// Params: level1..3 (0..2, default 1), pan1..3 (-1..1, default 0),
//         mode (0 = average [default], 1 = sum), mute1..3 / solo1..3 (0/1).
// Mute wins over solo. Solo considers enabled branches only; level values never change.
//
// BRANCH DELAY COMPENSATION (ABI v34). A parallel region whose branches carry
// different latencies is a COMB FILTER, not a blend: put a 2-sample module in
// one branch of a two-branch split and the merger sums x[n] with x[n-2], which
// notches the spectrum. v23 accepted that ("branch latency differences are NOT
// compensated"); the chain computed the per-branch totals for
// chainLatencySamples() and then threw them away. The merger now delays each
// branch by (max - branch) so every branch arrives aligned, and the region's
// reported latency is the max it actually runs at.
//
// Three things are load-bearing:
//   * the rings are sized for the WORST case in prepare() and never from a
//     runtime value — the delays are published ints the message thread may
//     change at any time, and every ring access masks with kPdcCapacity;
//   * a delay CHANGE is crossfaded across the block from the old tap to the
//     new one, because moving a delay tap in one sample is a splice;
//   * at zero delay the ring read returns the sample just written, so an
//     untouched splitter->merger pair is still a bit-exact passthrough.
class MergerModule : public DspModule
{
public:
    // Ring capacity per branch per channel. A delay is at most capacity-1, so
    // the sample read back is always one write ahead of being overwritten.
    // 8192 covers the largest single-module latency in the catalogue at every
    // supported rate (whammy: 7,680 samples at 192 kHz) with room to spare; a
    // branch that stacks more than that CLAMPS, and the excess is not
    // compensated. Documented rather than grown: the ring is
    // kDspMaxBranches * channels * capacity floats, allocated per merger.
    static constexpr int kPdcCapacity   = 8192;
    static constexpr int kPdcMaxSamples = kPdcCapacity - 1;

    void prepare(double, int, int numChannels) override
    {
        lastValid_ = false;
        for (int b = 0; b < kDspMaxBranches; ++b) lastValidB_[b] = false;
        nch_ = numChannels < 1 ? 1 : (numChannels > kMaxCh ? kMaxCh : numChannels);
        pdcData_.assign((size_t) kDspMaxBranches * (size_t) nch_ * (size_t) kPdcCapacity, 0.0f);
        pdcWrite_ = 0;
        for (int b = 0; b < kDspMaxBranches; ++b) lastDelay_[b] = -1; // -1 = snap, do not crossfade
    }
    void reset() override
    {
        lastValid_ = false;
        std::fill(pdcData_.begin(), pdcData_.end(), 0.0f);
        for (int b = 0; b < kDspMaxBranches; ++b) { lastDelay_[b] = -1; lastValidB_[b] = false; }
    }

    /// Message thread (the chain, under its write mutex): how far this branch
    /// must be delayed to line up with the latest one. Clamped, never rejected
    /// — a region deeper than the ring is still better aligned than not.
    void setBranchDelay(int branch, int samples) noexcept
    {
        if (branch < 0 || branch >= kDspMaxBranches) return;
        if (samples < 0) samples = 0;
        if (samples > kPdcMaxSamples) samples = kPdcMaxSamples;
        pdcDelay_[branch].store(samples, std::memory_order_relaxed);
    }
    int branchDelay(int branch) const noexcept
    {
        if (branch < 0 || branch >= kDspMaxBranches) return 0;
        return pdcDelay_[branch].load(std::memory_order_relaxed);
    }

    bool setParam(const char* id, float v) override
    {
        const int b = branchIndex(id, "level");
        if (b >= 0) { level_[b].store(clampf(v, 0.f, 2.f), std::memory_order_relaxed); return true; }
        const int p = branchIndex(id, "pan");
        if (p >= 0) { pan_[p].store(clampf(v, -1.f, 1.f), std::memory_order_relaxed); return true; }
        const int m = branchIndex(id, "mute");
        if (m >= 0) { mute_[m].store(v >= 0.5f, std::memory_order_relaxed); return true; }
        const int s = branchIndex(id, "solo");
        if (s >= 0) { solo_[s].store(v >= 0.5f, std::memory_order_relaxed); return true; }
        if (std::strcmp(id, "mode") == 0)
        {
            mode_.store(v >= 0.5f ? 1 : 0, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    bool getParam(const char* id, float& out) const override
    {
        const int b = branchIndex(id, "level");
        if (b >= 0) { out = level_[b].load(std::memory_order_relaxed); return true; }
        const int p = branchIndex(id, "pan");
        if (p >= 0) { out = pan_[p].load(std::memory_order_relaxed); return true; }
        const int m = branchIndex(id, "mute");
        if (m >= 0) { out = mute_[m].load(std::memory_order_relaxed) ? 1.f : 0.f; return true; }
        const int s = branchIndex(id, "solo");
        if (s >= 0) { out = solo_[s].load(std::memory_order_relaxed) ? 1.f : 0.f; return true; }
        if (std::strcmp(id, "mode") == 0)
        {
            out = (float) mode_.load(std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // The chain calls mergeRt instead; standalone (no open region) it's a wire.
    void process(float* const*, int, int) override {}
    const char* typeId() const override { return "merger"; }

    // Audio thread. Overwrites dest[0..numChannels) with the branch mix.
    // branches[b][c] are the branch channel buffers (only [0..numBranches)
    // are read). Per-branch gains ramp linearly across the block from the
    // previous block's landed gains so level/pan moves don't click.
    //
    // ABOVE kMaxCh (8) CHANNELS THIS IS A DOCUMENTED CLAMP, not a full mix:
    // the per-branch gain bookkeeping is sized kMaxCh and the PDC rings nch_
    // (itself clamped to kMaxCh in prepare), so channels 8+ are summed with
    // channel 7's gain and WITHOUT delay compensation. The rig runs at 1-2
    // channels, so this is unreachable in the shipping path; it is stated
    // because an undocumented cliff is worse than a stated one.
    void mergeRt(float* const* dest,
                 const std::vector<std::vector<float*>>& branches,
                 int numBranches, int numChannels, int numFrames) noexcept
    {
        if (dest == nullptr || numBranches <= 0 || numChannels <= 0 || numFrames <= 0)
            return;
        if (numBranches > kDspMaxBranches) numBranches = kDspMaxBranches;

        const float scale = mode_.load(std::memory_order_relaxed) == 0
                                ? 1.0f / (float) numBranches : 1.0f;
        const bool  stereoPan = (numChannels == 2);
        const int   nch = numChannels < kMaxCh ? numChannels : kMaxCh;

        // Snapshot switches once per block. A solo on a disabled third branch
        // must not silence the two live branches after a routing-width change.
        bool solos[kDspMaxBranches] = {};
        bool anySolo = false;
        for (int b = 0; b < numBranches; ++b)
            anySolo |= (solos[b] = solo_[b].load(std::memory_order_relaxed));

        // Per-branch, per-channel target gains.
        float target[kDspMaxBranches][kMaxCh];
        for (int b = 0; b < numBranches; ++b)
        {
            const bool audible = !mute_[b].load(std::memory_order_relaxed) && (!anySolo || solos[b]);
            const float lvl = audible ? level_[b].load(std::memory_order_relaxed) * scale : 0.f;
            float gL = lvl, gR = lvl;
            if (stereoPan)
            {
                // Balance law: unity at centre (null-test exact), attenuate
                // the far side only.
                const float p = pan_[b].load(std::memory_order_relaxed);
                if (p > 0.f) gL = lvl * (1.f - p);
                else if (p < 0.f) gR = lvl * (1.f + p);
            }
            for (int c = 0; c < nch; ++c)
                target[b][c] = (c == 1) ? gR : gL;
        }

        const float invFrames = 1.0f / (float) numFrames;
        // A branch the splitter has NARROWED away (3 -> 2) stops being written
        // here, so its ring froze holding pre-narrowing audio and its
        // lastDelay_/last_ froze holding pre-narrowing state. Widening back
        // (2 -> 3) then crossfaded from a stale tap INTO stale ring content at a
        // stale gain — a burst of audio from before the change. Keep the
        // disabled branches' rings filled with silence (bounded: at most
        // kDspMaxBranches-1 branches x nch_ channels x numFrames zero writes,
        // and only while a region is actually narrowed), and mark them cold so
        // the re-enable SNAPS its tap and its gain instead of crossfading out of
        // something that never played.
        // Delay compensation is available only once prepare() has sized the
        // rings; an unprepared merger (offline caller that never prepared the
        // chain) falls back to the pre-v34 straight sum.
        const bool pdcReady = ! pdcData_.empty();
        for (int c = 0; c < numChannels; ++c)
        {
            float* out = dest[c];
            if (out == nullptr) continue;
            const int gc = c < kMaxCh ? c : kMaxCh - 1;
            const bool ringOk = pdcReady && c < nch_;
            // First branch overwrites, the rest accumulate.
            for (int b = 0; b < numBranches; ++b)
            {
                const float* in = branches[(size_t) b][(size_t) c];
                const float gt  = target[b][gc];
                const float g0  = (lastValid_ && lastValidB_[b]) ? last_[b][gc] : gt;
                const float step = (gt - g0) * invFrames;
                if (! ringOk)
                {
                    if (b == 0)
                        for (int i = 0; i < numFrames; ++i) out[i] = in[i] * (g0 + step * (float) i);
                    else
                        for (int i = 0; i < numFrames; ++i) out[i] += in[i] * (g0 + step * (float) i);
                    continue;
                }
                float* ring = pdcData_.data()
                              + ((size_t) b * (size_t) nch_ + (size_t) c) * (size_t) kPdcCapacity;
                const int dNew = pdcDelay_[b].load(std::memory_order_relaxed);
                // -1 means "nothing played yet": snap rather than crossfade out
                // of a tap that never existed.
                const int dOld = lastDelay_[b] < 0 ? dNew : lastDelay_[b];
                for (int i = 0; i < numFrames; ++i)
                {
                    const int w = (pdcWrite_ + i) & kPdcMask;
                    ring[w] = in[i];
                    float s = ring[(w + kPdcCapacity - dNew) & kPdcMask];
                    if (dOld != dNew)
                    {
                        // A tap that moves in one sample is a splice; walk from
                        // the old read point to the new one across the block.
                        const float t = (float) i * invFrames;
                        const float sOld = ring[(w + kPdcCapacity - dOld) & kPdcMask];
                        s = sOld + (s - sOld) * t;
                    }
                    const float g = g0 + step * (float) i;
                    if (b == 0) out[i] = s * g;
                    else        out[i] += s * g;
                }
            }
        }
        if (pdcReady)
        {
            // Disabled branches: keep their rings advancing with SILENCE so a
            // re-enable reads silence rather than pre-narrowing audio.
            for (int b = numBranches; b < kDspMaxBranches; ++b)
            {
                for (int c = 0; c < nch_; ++c)
                {
                    float* ring = pdcData_.data()
                                  + ((size_t) b * (size_t) nch_ + (size_t) c) * (size_t) kPdcCapacity;
                    for (int i = 0; i < numFrames; ++i) ring[(pdcWrite_ + i) & kPdcMask] = 0.0f;
                }
            }
            // One cursor for every branch and channel — they are all written
            // the same block — advanced once, after the whole mix.
            pdcWrite_ = (pdcWrite_ + numFrames) & kPdcMask;
            for (int b = 0; b < numBranches; ++b)
                lastDelay_[b] = pdcDelay_[b].load(std::memory_order_relaxed);
            for (int b = numBranches; b < kDspMaxBranches; ++b)
                lastDelay_[b] = -1; // cold: snap the tap when it comes back
        }
        for (int b = 0; b < numBranches; ++b)
        {
            for (int c = 0; c < kMaxCh; ++c)
                last_[b][c] = target[b][c < nch ? c : (nch - 1)];
            lastValidB_[b] = true;
        }
        for (int b = numBranches; b < kDspMaxBranches; ++b)
            lastValidB_[b] = false; // cold: snap the gain when it comes back
        lastValid_ = true;
    }

private:
    static constexpr int kMaxCh = 8; // ramp bookkeeping bound (pan uses 2)
    static constexpr int kPdcMask = kPdcCapacity - 1;

    static float clampf(float v, float lo, float hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    // Matches "level1".."level3" / "pan1".."pan3" -> 0-based branch index.
    static int branchIndex(const char* id, const char* prefix)
    {
        const size_t n = std::strlen(prefix);
        if (std::strncmp(id, prefix, n) != 0) return -1;
        const char d = id[n];
        if (d < '1' || d > '0' + kDspMaxBranches || id[n + 1] != '\0') return -1;
        return d - '1';
    }

    std::atomic<float> level_[kDspMaxBranches] = { 1.f, 1.f, 1.f };
    std::atomic<float> pan_[kDspMaxBranches]   = { 0.f, 0.f, 0.f };
    std::atomic<int>   mode_{ 0 };
    std::atomic<bool> mute_[kDspMaxBranches] = { false, false, false };
    std::atomic<bool> solo_[kDspMaxBranches] = { false, false, false };

    // Branch delay compensation (v34). pdcDelay_ is published by the message
    // thread; everything else below is audio-thread-only, and pdcData_ is
    // allocated ONLY in prepare().
    std::atomic<int>   pdcDelay_[kDspMaxBranches] = { 0, 0, 0 };
    std::vector<float> pdcData_;                 // [branch][channel][kPdcCapacity]
    int                pdcWrite_ = 0;
    int                lastDelay_[kDspMaxBranches] = { -1, -1, -1 };
    int                nch_ = 0;

    // Audio-thread-only ramp state. lastValidB_ is PER BRANCH: the global flag
    // alone could not tell "this merger has never run" from "this branch was
    // switched off and is coming back", and the second one must snap.
    float last_[kDspMaxBranches][kMaxCh] = {};
    bool  lastValid_ = false;
    bool  lastValidB_[kDspMaxBranches] = { false, false, false };
};

} // namespace dcrig
