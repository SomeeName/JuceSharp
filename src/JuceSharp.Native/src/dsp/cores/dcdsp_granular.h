// dcdsp_granular.h — granular cloud / grain-delay (pure dcdsp core, RT-safe).
//
// A capture ring the input is written into continuously, and up to
// kMaxGrains CONCURRENT grains reading it back: each grain is a Hann-windowed
// read at 2^(semitones/12) of real time, launched from `spray`-scattered
// positions, and the grains overlap by construction (a new one is launched
// every grainLen / density samples). `freeze` stops the WRITE and leaves the
// cloud circling what is already in the ring.
//
// Easy to get wrong two ways. (1) The window: rebuilding a Hann per grain is
// an O(grainLen) burst inside the callback that launched it, so the table is
// built ONCE per prepare and every grain indexes it by its own normalised
// position. (2) The grain's start offset: a grain played at rate r catches up
// to the write head at (r-1) samples per sample, so it must start at least
// grainLen * max(1, r) behind it — start closer and a pitched-up grain reads
// samples that have not been written yet.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe.
struct GranularParams {
    float grainMs        = 80.0f; // 10 .. 500
    int   density        = 8;     // 1 .. 32 — CONCURRENT grains
    int   pitchSemitones = 0;     // -12 .. +12
    float spray          = 0.3f;  // 0 .. 1 — random read-position scatter
    float feedback       = 0.2f;  // 0 .. 0.9
    float mix            = 0.5f;  // 0 .. 1
    bool  freeze         = false; // stop writing the capture ring
};

class GranularCore {
public:
    static constexpr int kMaxGrains = 32;
    /// Below this window value a grain is close enough to its own end that
    /// replacing it is inaudible; above it the slot is left to finish.
    static constexpr float kReplaceFloor = 0.05f;

    /// Fallback block bound when the host passes maxBlockSize <= 0. Zero meant
    /// NO oversized-block guard at all, which is the one case every other core
    /// in the wave substitutes a safe default for.
    static constexpr int kFallbackMaxBlock = 4096;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_    = numChannels < 1 ? 1 : numChannels;
        maxBlock_ = maxBlockSize > 0 ? maxBlockSize : kFallbackMaxBlock;

        maxGrainSamp_ = (int) std::ceil(kMaxGrainMs * 0.001 * sr_) + 8;
        // Worst case: the longest grain, started the furthest back the spray
        // allows, walking away from the head at the slowest rate. 4x covers it
        // with a whole grain to spare.
        ringSize_ = nextPow2(4 * maxGrainSamp_ + 8);
        ringMask_ = ringSize_ - 1;
        ring_.assign((std::size_t) (numCh_ * ringSize_), 0.0f);

        // ONE window table, indexed by normalised position — never per grain.
        window_.resize(kWindowTable + 1);
        for (int i = 0; i <= kWindowTable; ++i) {
            const double t = (double) i / (double) kWindowTable;
            window_[(std::size_t) i] = (float) (0.5 - 0.5 * std::cos(kTwoPi * t));
        }

        dc_.assign((std::size_t) numCh_, DcBlocker{});
        for (auto& d : dc_) d.prepare(sr_);

        smMix_.reset(sr_, kLevelRampSeconds);
        smFb_.reset(sr_, kLevelRampSeconds);
        smGain_.reset(sr_, kLevelRampSeconds);

        applyDerived();
        smMix_.snapToTarget();
        smFb_.snapToTarget();
        smGain_.snapToTarget();
        primed_ = false;
        reset();
    }

    // State only: the ring holds AUDIO, so it is cleared; the params, `freeze`
    // included, are user content and survive a device change untouched.
    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        for (auto& d : dc_) d.reset();
        for (auto& g : grains_) g.active = false;
        head_ = 0;
        hopCounter_ = 0;
        nextSlot_ = 0;
        rng_ = kSeed;
    }

    void setParams(const GranularParams& p) {
        p_.grainMs        = clampf(p.grainMs, 10.0f, 500.0f);
        p_.density        = std::clamp(p.density, 1, kMaxGrains);
        p_.pitchSemitones = std::clamp(p.pitchSemitones, -12, 12);
        p_.spray          = clampf(p.spray, 0.0f, 1.0f);
        p_.feedback       = clampf(p.feedback, 0.0f, 0.9f);
        p_.mix            = clampf(p.mix, 0.0f, 1.0f);
        p_.freeze         = p.freeze;
        applyDerived();
        if (! primed_) {
            smMix_.snapToTarget();
            smFb_.snapToTarget();
            smGain_.snapToTarget();
            primed_ = true;
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        // The two guards every core in the other four drawers opens with, and
        // that this drawer's five dropped. DspChain passes a full set of valid
        // pointers today, so this is consistency and robustness rather than a
        // live crash -- but the RT gate calls process() directly and a null
        // `channels` was an access violation, not a skipped block. Truncating at
        // the first null rather than skipping past it is deliberate: multitap
        // and duckdelay SUM across channels before they write, so a hole in the
        // middle of the set is not a channel this core can render around.
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (maxBlock_ > 0 && numFrames > maxBlock_) return;
        int ch = numChannels < numCh_ ? numChannels : numCh_;
        if (ch > kMaxChannels) ch = kMaxChannels;
        for (int c = 0; c < ch; ++c) if (channels[c] == nullptr) { ch = c; break; }
        if (ch <= 0) return;

        const int   grainLen = grainSamples_;
        const int   hop      = std::max(1, grainLen / std::max(1, p_.density));
        const float rate     = rate_;
        const bool  frozen   = p_.freeze;
        // Frozen, the head does not move, so a grain walks away from it at the
        // full read rate; running, it only gains (rate - 1) per sample.
        const float delayStep = frozen ? -rate : (1.0f - rate);

        for (int n = 0; n < numFrames; ++n) {
            const float wet  = smMix_.next();
            const float dry  = 1.0f - wet;
            const float fb   = smFb_.next();
            const float gain = smGain_.next();

            // --- launch: at most one grain per sample, O(1) ------------------
            if (--hopCounter_ <= 0) {
                hopCounter_ = hop;
                launch(grainLen, rate);
            }

            // --- render every live grain ------------------------------------
            float wetCh[kMaxChannels] = {};
            for (int gi = 0; gi < kMaxGrains; ++gi) {
                Grain& g = grains_[(std::size_t) gi];
                if (! g.active) continue;
                const float w = windowAt((float) g.age * g.invLen);
                for (int c = 0; c < ch; ++c)
                    wetCh[c] += w * readRing(c, g.delay);
                g.delay += delayStep;
                if (++g.age >= g.len || g.delay < 1.0f || g.delay > (float) (ringSize_ - 4))
                    g.active = false;
            }

            for (int c = 0; c < ch; ++c) {
                const auto uc = (std::size_t) c;
                const float x = channels[c][n];
                const float v = gain * wetCh[c];
                if (! frozen) {
                    const float fed = softLimit(dc_[uc].process(fb * v));
                    writeRing(c, x + fed);
                }
                channels[c][n] = dry * x + wet * v;
            }
            if (! frozen) head_ = (head_ + 1) & ringMask_;
        }
    }

    int latencySamples() const { return 0; }

    /// Live grain count, for tests. Not a param.
    int activeGrainsForTest() const {
        int n = 0;
        for (const auto& g : grains_) if (g.active) ++n;
        return n;
    }

private:
    struct Grain {
        bool  active = false;
        int   age    = 0;
        int   len    = 1;
        float invLen = 1.0f;
        float delay  = 0.0f;   // samples behind the write head
    };

    static constexpr int kMaxChannels = 8;

    void applyDerived() {
        smMix_.setTarget(p_.mix);
        smFb_.setTarget(p_.feedback);
        grainSamples_ = std::max(8, (int) std::lround(p_.grainMs * 0.001 * sr_));
        rate_ = std::pow(2.0f, (float) p_.pitchSemitones * (1.0f / 12.0f));
        // With `density` grains overlapping a Hann window the sum is ~density/2.
        // Normalising by that keeps the cloud at the level of what went in
        // whatever the density, so the knob does not double as a volume control.
        smGain_.setTarget(2.0f / std::max(1.0f, (float) p_.density));
    }

    void launch(int grainLen, float rate) {
        Grain& g = grains_[(std::size_t) nextSlot_];
        // A slot still MID-WINDOW is left to finish. The round robin returns to
        // a slot after kMaxGrains * hop == grainLen samples, which is exactly a
        // grain's life and therefore safe while grainLen is STEADY -- but
        // lowering `grainMs` shortens the cycle below the life of the grains
        // already in flight, so the slot being reused is at a non-zero window
        // position: a discontinuity, and a replacement started at window
        // position 0 rather than faded in. Dropping one launch out of a cloud
        // of up to 32 is not audible; cutting a grain in half is.
        if (g.active && windowAt((float) g.age * g.invLen) > kReplaceFloor) return;
        nextSlot_ = (nextSlot_ + 1) % kMaxGrains;
        g.active = true;
        g.age    = 0;
        g.len    = grainLen;
        g.invLen = 1.0f / (float) grainLen;
        // kStartMargin keeps the LAST sample of a frozen, pitched-up grain
        // inside the ring: frozen, the grain walks all the way to the head.
        const float base = (float) grainLen * std::max(1.0f, rate) + kStartMargin;
        const float scatter = p_.spray * (float) grainLen * nextUnit();
        g.delay = std::clamp(base + scatter, 1.0f, (float) (ringSize_ - 8));
    }

    float windowAt(float t) const {
        if (! (t > 0.0f)) return 0.0f;
        if (t >= 1.0f) return 0.0f;
        const float f = t * (float) kWindowTable;
        const int   i = (int) f;
        const float frac = f - (float) i;
        const float a = window_[(std::size_t) i];
        const float b = window_[(std::size_t) (i + 1)];
        return a + frac * (b - a);
    }

    float readRing(int c, float delay) const {
        const int   id = (int) delay;
        const float f  = delay - (float) id;
        const int   i0 = (head_ - id) & ringMask_;
        const int   i1 = (i0 - 1) & ringMask_;
        const float* base = ring_.data() + (std::size_t) (c * ringSize_);
        const float a = base[(std::size_t) i0];
        const float b = base[(std::size_t) i1];
        return a + f * (b - a);
    }

    void writeRing(int c, float v) {
        ring_[(std::size_t) (c * ringSize_ + head_)] = v;
    }

    float nextUnit() {                 // [0, 1), deterministic
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return (float) (rng_ >> 8) * (1.0f / 16777216.0f);
    }

    static int nextPow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float softLimit(float x) { return 0.9f * std::tanh(x * (1.0f / 0.9f)); }

    static constexpr double kMaxGrainMs      = 500.0;
    static constexpr float  kLevelRampSeconds = 0.015f;
    static constexpr int    kWindowTable      = 2048;
    static constexpr float  kStartMargin      = 16.0f;
    static constexpr uint32_t kSeed = 0x6A9E1C3Du;

    std::vector<float> ring_;
    std::vector<float> window_;
    std::vector<DcBlocker> dc_;
    std::array<Grain, (std::size_t) kMaxGrains> grains_{};
    SmoothedValue smMix_, smFb_, smGain_;
    GranularParams p_{};
    double sr_ = 48000.0;
    int    numCh_ = 2;
    int    maxBlock_ = 0;
    int    maxGrainSamp_ = 0;
    int    ringSize_ = 0;
    int    ringMask_ = 0;
    int    head_ = 0;
    int    hopCounter_ = 0;
    int    nextSlot_ = 0;
    int    grainSamples_ = 0;
    float  rate_ = 1.0f;
    uint32_t rng_ = kSeed;
    bool   primed_ = false;
};

} // namespace dcdsp
