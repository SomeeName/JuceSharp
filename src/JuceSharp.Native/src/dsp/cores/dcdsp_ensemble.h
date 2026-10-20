// dcdsp_ensemble.h — string-ensemble (Solina / Eminent) chorus core, RT-safe.
//
// The topology that makes a string machine sound like a string machine: ONE
// BBD line per channel with up to THREE read taps, and every tap modulated by a
// SLOW bank and a FAST bank at once, the taps spaced 360/voices degrees apart
// in both banks. The two rates are what separates this from an ordinary chorus
// — the slow bank gives the drift, the fast bank the shimmer.
// Easy to get wrong: summing every tap into every channel. Three taps at 120
// degrees CANCEL each other's first-order sidebands when summed, and the
// ensemble goes flat; each channel therefore takes its OWN subset of the taps
// (L = 0,1 / R = 1,2 at three voices) and the per-tap channel weights RAMP, so
// a voices change fades rather than switching a tap in at full level. There is
// deliberately NO dc blocker: nothing here recirculates, and this rig already
// cascades enough bounded bass filters.
//
// Namespace dcdsp. C++17. Deps: only the shared primitives header.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct EnsembleParams {
    float rateHz     = 0.6f;  // 0.1 .. 2   — the slow bank
    float fastRateHz = 5.5f;  // 3   .. 8   — the fast bank
    float depth      = 0.6f;  // 0 .. 1
    int   voices     = 3;     // 2 .. 3
    float tone       = 0.5f;  // 0 .. 1  — BBD darkness (2 kHz .. 12 kHz)
    float mix        = 0.6f;  // 0 .. 1
};

class EnsembleCore {
public:
    static constexpr int   kMaxVoices  = 3;
    static constexpr int   kSides      = 2;
    static constexpr float kBaseMs     = 8.0f;   // BBD standing delay
    static constexpr float kSlowSpanMs = 3.0f;   // slow bank excursion at depth 1
    static constexpr float kFastSpanMs = 0.6f;   // fast bank excursion at depth 1

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_    = numChannels < 1 ? 1 : numChannels;
        maxBlock_ = maxBlockSize < 1 ? 1 : maxBlockSize;

        // Worst case: base + both banks at full depth + Lagrange headroom.
        const double maxDelayMs = (double) kBaseMs + kSlowSpanMs + kFastSpanMs + 2.0;
        const int maxSamps = (int) std::ceil(maxDelayMs * 0.001 * sr_) + 4;
        lines_.resize((size_t) numCh_);
        for (auto& l : lines_) l.prepare(maxSamps);
        lp_.resize((size_t) numCh_);

        // Rate-owned: both phase increments and every millisecond-to-sample
        // conversion. None of these may be left to setParams.
        slowInc_  = (double) clampNan(p_.rateHz, 0.1f, 2.0f, 0.6f) / sr_;
        fastInc_  = (double) clampNan(p_.fastRateHz, 3.0f, 8.0f, 5.5f) / sr_;
        baseSamp_ = (float) ((double) kBaseMs * 0.001 * sr_);
        slowSamp_ = (float) ((double) kSlowSpanMs * 0.001 * sr_);
        fastSamp_ = (float) ((double) kFastSpanMs * 0.001 * sr_);
        lastFc_   = -1.0;   // force the BBD low-pass to be redesigned for sr_

        smDepth_.reset(sr_, kParamRampSeconds);
        smMix_.reset(sr_, kParamRampSeconds);
        smTone_.reset(sr_, kParamRampSeconds);
        smSpacing_.reset(sr_, kGeometryRampSeconds);
        for (auto& row : smWeight_)
            for (auto& s : row) s.reset(sr_, kParamRampSeconds);
        retarget();
        snapAll();
        primed_ = false;
        reset();
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& f : lp_) f.reset();
        slowPhase_ = 0.0;
        fastPhase_ = 0.0;
    }

    void setParams(const EnsembleParams& p) {
        p_ = p;
        slowInc_ = (double) clampNan(p.rateHz, 0.1f, 2.0f, 0.6f) / sr_;
        fastInc_ = (double) clampNan(p.fastRateHz, 3.0f, 8.0f, 5.5f) / sr_;
        retarget();
        if (!primed_) { snapAll(); primed_ = true; }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;
        const int ch = numChannels < numCh_ ? numChannels : numCh_;
        const float maxTap = baseSamp_ + slowSamp_ + fastSamp_;

        std::array<float, kMaxVoices> tap{};
        std::array<std::array<float, kMaxVoices>, kSides> w{};

        for (int n = 0; n < numFrames; ++n) {
            const float depth = smDepth_.next();
            const float wet   = smMix_.next();
            const float dry   = 1.0f - wet;
            const float tone  = smTone_.next();
            // Every smoother is walked ONCE PER FRAME, outside the channel loop.
            // Walking a shared smoother inside it is the stereo-tilt bug the
            // primitives header documents, and the weight matrix is shared the
            // moment the rig hands this core more than two channels.
            for (int s = 0; s < kSides; ++s)
                for (int v = 0; v < kMaxVoices; ++v)
                    w[(size_t) s][(size_t) v] = smWeight_[(size_t) s][(size_t) v].next();

            // The per-tap LFO phase offset RAMPS with the voice count. The
            // weights already did (`smWeight_`), but `spacing` did not: 3 -> 2
            // voices moves tap 1's offset by 1/6 of a cycle instantly, and tap 1
            // runs at weight 1.0 on the odd channels in BOTH configurations, so
            // the right channel's read tap jumped by up to ~170 samples on one
            // sample. That is a splice in a delay line, not a level change, and
            // no ramped weight can hide it.
            const float spacing = smSpacing_.next();
            for (int v = 0; v < kMaxVoices; ++v) {
                const double off = (double) v * (double) spacing;
                double ps = slowPhase_ + off; ps -= std::floor(ps);
                double pf = fastPhase_ + off; pf -= std::floor(pf);
                const float slow = Lfo::shapeAt(Lfo::Sine, (float) ps);
                const float fast = Lfo::shapeAt(Lfo::Sine, (float) pf);
                float d = baseSamp_ + depth * (slowSamp_ * slow + fastSamp_ * fast);
                if (d < 1.0f)   d = 1.0f;
                if (d > maxTap) d = maxTap;
                tap[(size_t) v] = d;
            }
            slowPhase_ += slowInc_; slowPhase_ -= std::floor(slowPhase_);
            fastPhase_ += fastInc_; fastPhase_ -= std::floor(fastPhase_);

            // The BBD low-pass follows `tone`. Redesigned only when the
            // smoothed value has actually moved (a 15 ms glide, then nothing),
            // so a settled rig pays no transcendental per sample.
            // The GUARD IS ON `tone`, not on `fc`: guarding the redesign left
            // the std::pow running every sample forever, which is precisely what
            // the comment above claims it does not do.
            if (tone != lastTone_) {
                lastTone_ = tone;
                const double fc = 2000.0 * std::pow(6.0, (double) tone);   // 2 k .. 12 k
                if (std::fabs(fc - lastFc_) > 0.5) {
                    for (auto& f : lp_) f.setLowpass(sr_, fc);
                    lastFc_ = fc;
                }
            }

            for (int c = 0; c < ch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                const size_t ci = (size_t) c;
                const size_t side = (size_t) (c & 1);
                const float x = buf[n];

                float sum = 0.0f, norm = 0.0f;
                for (int v = 0; v < kMaxVoices; ++v) {
                    const float wv = w[side][(size_t) v];
                    if (wv <= 0.0f) continue;
                    sum  += wv * lines_[ci].readAt(tap[(size_t) v]);
                    norm += wv;
                }
                lines_[ci].push(x);
                float wetSig = (norm > 1.0e-6f) ? lp_[ci].process(sum / norm) : 0.0f;
                if (!std::isfinite(wetSig)) wetSig = 0.0f;
                float out = dry * x + wet * wetSig;
                if (!std::isfinite(out)) out = 0.0f;
                buf[n] = out;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    static float clampNan(float v, float lo, float hi, float fallback) {
        if (!std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    int activeVoices() const {
        int v = p_.voices;
        if (v < 2) v = 2;
        if (v > kMaxVoices) v = kMaxVoices;
        return v;
    }
    void snapAll() {
        smDepth_.snapToTarget(); smMix_.snapToTarget(); smTone_.snapToTarget();
        smSpacing_.snapToTarget();
        for (auto& row : smWeight_) for (auto& s : row) s.snapToTarget();
    }

    void retarget() {
        smDepth_.setTarget(clampNan(p_.depth, 0.0f, 1.0f, 0.6f));
        smMix_.setTarget(clampNan(p_.mix, 0.0f, 1.0f, 0.6f));
        smTone_.setTarget(clampNan(p_.tone, 0.0f, 1.0f, 0.5f));
        // Side s takes `active - 1` consecutive taps starting at s: at three
        // voices L = {0,1} and R = {1,2}, at two L = {0} and R = {1}. A change
        // retargets the weights, which RAMP — a tap is never switched in at
        // full level.
        const int active = activeVoices();
        const int count  = active - 1 < 1 ? 1 : active - 1;
        smSpacing_.setTarget(1.0f / (float) active);
        for (int s = 0; s < kSides; ++s)
            for (int v = 0; v < kMaxVoices; ++v) {
                bool on = false;
                for (int i = 0; i < count; ++i)
                    if (v == (s + i) % active) on = true;
                smWeight_[(size_t) s][(size_t) v].setTarget(on ? 1.0f : 0.0f);
            }
    }

    static constexpr float kParamRampSeconds = 0.015f;
    /// The voice-count geometry glide. Longer than a level ramp on purpose:
    /// this one moves a READ TAP, and 15 ms of 170 samples is still a chirp.
    static constexpr float kGeometryRampSeconds = 0.050f;

    std::vector<InterpolatedDelayLine> lines_;
    std::vector<OnePole> lp_;
    SmoothedValue smDepth_, smMix_, smTone_, smSpacing_;
    std::array<std::array<SmoothedValue, kMaxVoices>, kSides> smWeight_{};
    double slowPhase_ = 0.0, fastPhase_ = 0.0;
    double slowInc_ = 0.0, fastInc_ = 0.0, sr_ = 48000.0, lastFc_ = -1.0;
    float  lastTone_ = -1.0f;
    float baseSamp_ = 384.0f, slowSamp_ = 144.0f, fastSamp_ = 28.8f;
    bool primed_ = false;
    EnsembleParams p_{};
    int numCh_ = 2;
    int maxBlock_ = 2048;
};

} // namespace dcdsp
