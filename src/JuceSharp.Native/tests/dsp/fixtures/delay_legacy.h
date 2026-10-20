// Frozen e8286ab implementation: bit-exact legacy compatibility oracle.
// dcdsp_delay.h — stereo delay with tone-shaped feedback and ping-pong (pure dcdsp core).
//
// Per-channel InterpolatedDelayLine (sized for the 2000 ms max at prepare),
// a one-pole low-pass ("tone") plus a DC blocker in each feedback path. When
// pingPong is set the feedback crosses channels in pairs (0<->1, 2<->3, ...).
// Feedback is clamped to [0, 0.95] and the feedback filters have gain <= 1, so
// the loop gain never reaches 1 => bounded.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dsp/cores/dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace legacy_delay {
using namespace dcdsp;

struct DelayParams {
    float timeMs   = 300.0f;  // 10 .. 2000
    float feedback = 0.4f;    // 0  .. 0.95
    float mix      = 0.35f;   // 0  .. 1     — dry/wet
    bool  pingPong = false;
    float toneHz   = 4000.0f; // 500 .. 12000 — low-pass in the feedback path
};

class DelayCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        numCh_ = numChannels < 1 ? 1 : numChannels;

        maxDelaySamp_ = (int)std::ceil(kMaxTimeMs * 0.001 * sr_) + 4;
        lines_.resize((size_t)numCh_);
        for (auto& l : lines_) l.prepare(maxDelaySamp_);
        tone_.resize((size_t)numCh_);
        dc_.resize((size_t)numCh_);
        // Inside the recirculating loop, so its corner compounds once per
        // repeat. Unprepared it keeps R = 0.995 — ~38 Hz at 48 kHz but ~76 Hz
        // at 96 kHz — and a delay tail loses its low end twice as fast purely
        // because the device rate changed.
        for (auto& d : dc_) d.prepare(sr_);
        reads_.assign((size_t)numCh_, 0.0f);

        smoothCoef_ = (float)std::exp(-1.0 / (0.05 * sr_)); // ~50 ms glide
        applyTone();
        targetDelay_ = clampDelaySamp((float)(p_.timeMs * 0.001 * sr_));
        smoothedDelay_ = targetDelay_;
        // The delay TIME was smoothed; mix and feedback were not, so a preset
        // switch stepped the wet/dry crossfade and the loop gain between two
        // adjacent samples. ~15 ms, the window the chorus/flanger use.
        smMix_.reset(sr_, 0.015f); smMix_.setTarget(p_.mix);      smMix_.snapToTarget();
        smFb_.reset(sr_, 0.015f);  smFb_.setTarget(p_.feedback);  smFb_.snapToTarget();
        // NOT primed: the first setParams() (carrying the caller's real time)
        // must snap instantly instead of gliding from these default params.
        primed_ = false;
        reset();
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& t : tone_) t.reset();
        for (auto& d : dc_) d.reset();
        smoothedDelay_ = targetDelay_;
    }

    void setParams(const DelayParams& p) {
        DelayParams q = p;
        q.timeMs   = clampf(q.timeMs, 10.0f, 2000.0f);
        q.feedback = clampf(q.feedback, 0.0f, 0.95f);
        q.mix      = clampf(q.mix, 0.0f, 1.0f);
        q.toneHz   = clampf(q.toneHz, 500.0f, 12000.0f);

        if (q.toneHz != p_.toneHz) { p_.toneHz = q.toneHz; applyTone(); }
        targetDelay_ = clampDelaySamp((float)(q.timeMs * 0.001 * sr_));
        smMix_.setTarget(q.mix);
        smFb_.setTarget(q.feedback);
        if (!primed_) {
            smoothedDelay_ = targetDelay_;
            smMix_.snapToTarget();
            smFb_.snapToTarget();
            primed_ = true;
        }
        p_ = q;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        const int ch = numChannels < numCh_ ? numChannels : numCh_;

        for (int n = 0; n < numFrames; ++n) {
            smoothedDelay_ = targetDelay_ + (smoothedDelay_ - targetDelay_) * smoothCoef_;
            const float d = smoothedDelay_;
            const float fb  = smFb_.next();  // clamped [0, 0.95]
            const float wet = smMix_.next();
            const float dry = 1.0f - wet;

            // Read every channel's delayed sample before writing, so ping-pong
            // cross-feedback sees a consistent snapshot of this frame.
            for (int c = 0; c < ch; ++c) reads_[(size_t)c] = lines_[(size_t)c].readAt(d);

            for (int c = 0; c < ch; ++c) {
                int src = c;
                if (p_.pingPong) {
                    const int partner = c ^ 1;
                    src = (partner < ch) ? partner : c; // odd leftover self-feeds
                }
                const float shaped = dc_[(size_t)c].process(tone_[(size_t)c].process(reads_[(size_t)src]));
                const float x = channels[c][n];
                lines_[(size_t)c].push(x + fb * shaped);
                channels[c][n] = dry * x + wet * reads_[(size_t)c];
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    void applyTone() {
        for (auto& t : tone_) t.setLowpass(sr_, p_.toneHz);
    }
    float clampDelaySamp(float s) const {
        const float maxd = (float)(maxDelaySamp_ - 2);
        if (s < 1.0f) s = 1.0f;
        if (s > maxd) s = maxd;
        return s;
    }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    static constexpr double kMaxTimeMs = 2000.0;

    std::vector<InterpolatedDelayLine> lines_;
    std::vector<OnePole>   tone_;
    std::vector<DcBlocker> dc_;
    std::vector<float>     reads_;
    SmoothedValue smMix_, smFb_;
    DelayParams p_;
    double sr_ = 44100.0;
    int    numCh_ = 2;
    int    maxDelaySamp_ = 0;
    float  smoothCoef_ = 0.0f;
    float  targetDelay_ = 0.0f;
    float  smoothedDelay_ = 0.0f;
    bool   primed_ = false;
};

} // namespace dcdsp
