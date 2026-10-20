// dcdsp_flanger.h — short modulated-delay flanger with feedback (pure dcdsp core).
//
// A short (~0.5-5.5 ms) InterpolatedDelayLine per channel, swept by one LFO,
// with a feedback path. Feedback is clamped to +/-0.95 (comb pole magnitude
// < 1 => stable) and DC-blocked so long feedback tails cannot accumulate DC.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct FlangerParams {
    float rateHz   = 0.3f; // 0.05 .. 3    — sweep rate
    float depthMs  = 2.0f; // 0.5  .. 5    — sweep span
    float feedback = 0.5f; // -0.95 .. 0.95
    float mix      = 0.5f; // 0    .. 1    — dry/wet
};

class FlangerCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        numCh_ = numChannels < 1 ? 1 : numChannels;

        const double maxDelayMs = kMinDelayMs + kMaxDepthMs + 2.0;
        const int maxSamps = (int)std::ceil(maxDelayMs * 0.001 * sr_) + 4;

        lines_.resize((size_t)numCh_);
        for (auto& l : lines_) l.prepare(maxSamps);
        dc_.resize((size_t)numCh_);
        // The DC blocker sits INSIDE the recirculating loop, so its corner
        // compounds once per repeat. Left unprepared it keeps the class default
        // R = 0.995 — documented as ~38 Hz at 48 kHz, which is ~76 Hz at 96 kHz
        // and ~152 Hz at 192 kHz: the comb gets audibly thinner purely because
        // the user changed device rate. Every sibling core that puts a
        // DcBlocker in a loop (chorus, fuzz, amp, feedbacker, bigmuff) prepares
        // it; these two did not.
        for (auto& d : dc_) d.prepare(sr_);

        lfo_.prepare(sr_);
        lfo_.setShape(Lfo::Sine);
        lfo_.setFrequency(p_.rateHz);

        // ~15 ms parameter smoothing, the same window dcdsp_chorus.h uses.
        // depthMs is a DELAY LENGTH, not a level: a legal 0.5 -> 5 ms jump
        // (a preset switch, an A/B) moved the read tap 216 samples at 48 kHz
        // between two adjacent samples, i.e. spliced the waveform. mix and
        // feedback stepped with it.
        smDepth_.reset(sr_, 0.015f);
        smMix_.reset(sr_, 0.015f);
        smFb_.reset(sr_, 0.015f);
        smDepth_.setTarget(depthSampFor(p_.depthMs)); smDepth_.snapToTarget();
        smMix_.setTarget(p_.mix);                     smMix_.snapToTarget();
        smFb_.setTarget(p_.feedback);                 smFb_.snapToTarget();
        // The first setParams carrying the caller's REAL values must snap, not
        // glide off these defaults — an offline bounce has to be deterministic
        // from sample 0. (Same contract as dcdsp_delay.h's primed_.)
        primed_ = false;
        reset();
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& d : dc_) d.reset();
        lfo_.reset();
    }

    void setParams(const FlangerParams& p) {
        FlangerParams q = p;
        q.rateHz   = clampf(q.rateHz, 0.05f, 3.0f);
        q.depthMs  = clampf(q.depthMs, 0.5f, 5.0f);
        q.feedback = clampf(q.feedback, -0.95f, 0.95f);
        q.mix      = clampf(q.mix, 0.0f, 1.0f);
        if (q.rateHz != p_.rateHz) lfo_.setFrequency(q.rateHz);
        smDepth_.setTarget(depthSampFor(q.depthMs));
        smMix_.setTarget(q.mix);
        smFb_.setTarget(q.feedback);
        if (!primed_) { smDepth_.snapToTarget(); smMix_.snapToTarget(); smFb_.snapToTarget(); primed_ = true; }
        p_ = q;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        const float minSamp   = (float)(kMinDelayMs * 0.001 * sr_);
        const int   ch        = numChannels < numCh_ ? numChannels : numCh_;

        for (int n = 0; n < numFrames; ++n) {
            // Advanced once per FRAME and shared across channels, so the two
            // channels never drift apart mid-ramp.
            const float depthSamp = smDepth_.next();
            const float fb        = smFb_.next();  // clamped [-0.95, 0.95]
            const float wet       = smMix_.next();
            const float dry       = 1.0f - wet;
            const float m = lfo_.process(); // [-1, 1], shared across channels
            for (int c = 0; c < ch; ++c) {
                const float sign = (c & 1) ? -1.0f : 1.0f;
                const float uni  = 0.5f * (1.0f + sign * m); // [0, 1]
                float d = minSamp + depthSamp * uni;
                if (d < 1.0f) d = 1.0f;

                const float delayed = lines_[(size_t)c].readAt(d);
                lines_[(size_t)c].push(channels[c][n] + fb * dc_[(size_t)c].process(delayed));
                channels[c][n] = dry * channels[c][n] + wet * delayed;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    float depthSampFor(float depthMs) const { return (float)(depthMs * 0.001 * sr_); }

    static constexpr double kMinDelayMs = 0.5;
    static constexpr double kMaxDepthMs = 5.0;

    std::vector<InterpolatedDelayLine> lines_;
    std::vector<DcBlocker> dc_;
    Lfo lfo_;
    SmoothedValue smDepth_, smMix_, smFb_;
    bool primed_ = false;
    FlangerParams p_;
    double sr_ = 44100.0;
    int numCh_ = 2;
};

} // namespace dcdsp
