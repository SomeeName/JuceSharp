// dcdsp_delay.h — stereo delay with tone-shaped feedback and ping-pong (pure dcdsp core).
//
// Per-channel InterpolatedDelayLine (sized for the 2000 ms max at prepare),
// a one-pole low-pass ("tone") plus a DC blocker in each feedback path. When
// pingPong is set the feedback crosses channels in pairs (0<->1, 2<->3, ...).
// Feedback is clamped to [0, 0.95] and the feedback filters have gain <= 1, so
// the loop gain never reaches 1 => bounded.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct DelayParams {
    float timeMs   = 300.0f;  // 10 .. 2000
    float feedback = 0.4f;    // 0  .. 0.95
    float mix      = 0.35f;   // 0  .. 1     — dry/wet
    bool  pingPong = false;
    bool sync = false;
    float bpm = 120.0f;      // 20 .. 400
    int division = 2;        // Whole, half, quarter, quarter dotted/triplet, eighth, eighth dotted/triplet, sixteenth
    bool trails = false;
    float toneHz   = 4000.0f; // 500 .. 12000 — low-pass in the feedback path
};

// Quarter-note beat counts; the persisted ordinal is the catalogue label index.
inline float delayTimeMs(const DelayParams& p) {
    if (!p.sync) return p.timeMs;
    static constexpr float beats[] = {4.0f, 2.0f, 1.0f, 1.5f, 2.0f/3.0f, 0.5f, 0.75f, 1.0f/3.0f, 0.25f};
    const float bpm = std::isfinite(p.bpm) ? std::clamp(p.bpm, 20.0f, 400.0f) : 120.0f;
    return std::clamp(60000.0f / bpm * beats[std::clamp(p.division, 0, 8)], 10.0f, 2000.0f);
}

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
        targetDelay_ = clampDelaySamp((float)(delayTimeMs(p_) * 0.001 * sr_));
        smoothedDelay_ = targetDelay_;
        // The delay TIME was smoothed; mix and feedback were not, so a preset
        // switch stepped the wet/dry crossfade and the loop gain between two
        // adjacent samples. ~15 ms, the window the chorus/flanger use.
        smMix_.reset(sr_, 0.015f); smMix_.setTarget(p_.mix);      smMix_.snapToTarget();
        smFb_.reset(sr_, 0.015f);  smFb_.setTarget(p_.feedback);  smFb_.snapToTarget();
        // NOT primed: the first setParams() (carrying the caller's real time)
        // must snap instantly instead of gliding from these default params.
        smTrails_.reset(sr_, 0.010f); smTrails_.setTarget(p_.trails ? 1.0f : 0.0f); smTrails_.snapToTarget();
        // pingPong swaps the feedback SOURCE, and L and R line contents are
        // uncorrelated at that instant, so the toggle was a full-amplitude
        // splice INSIDE a loop running at up to 0.95. The loop-gain exemption
        // does not cover it: this is routing, not a level. Same 10 ms window
        // smTrails_ uses one line up.
        smPing_.reset(sr_, 0.010f); smPing_.setTarget(p_.pingPong ? 1.0f : 0.0f); smPing_.snapToTarget();
        primed_ = false;
        reset();
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        for (auto& t : tone_) t.reset();
        for (auto& d : dc_) d.reset();
        smoothedDelay_ = targetDelay_;
        hardParked_ = false;
    }

    void setParams(const DelayParams& p) {
        DelayParams q = p;
        q.timeMs   = clampf(q.timeMs, 10.0f, 2000.0f);
        q.feedback = clampf(q.feedback, 0.0f, 0.95f);
        q.mix      = clampf(q.mix, 0.0f, 1.0f);
        q.toneHz   = clampf(q.toneHz, 500.0f, 12000.0f);

        if (q.toneHz != p_.toneHz) { p_.toneHz = q.toneHz; applyTone(); }
        targetDelay_ = clampDelaySamp((float)(delayTimeMs(q) * 0.001 * sr_));
        smTrails_.setTarget(q.trails ? 1.0f : 0.0f);
        smPing_.setTarget(q.pingPong ? 1.0f : 0.0f);
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
        processImpl(channels, numChannels, numFrames, 1.0f, 1.0f, 0.0f, false);
    }

    // Chain supplies its one bypass ramp. Trails fade INPUT to the feedback line,
    // not existing wet history, and dry moves to unity rather than being doubled.
    void processBypass(float* const* channels, int numChannels, int numFrames,
                       float wetStart, float wetTarget, float wetStep) {
        if (hardParked_ && p_.trails) reset(); // never resurrect a frozen hard-bypass tail
        if (wetStart == 0.0f && wetTarget == 0.0f && !p_.trails && !smTrails_.isRamping()) {
            hardParked_ = true;
            return;
        }
        hardParked_ = false;
        processImpl(channels, numChannels, numFrames, wetStart, wetTarget, wetStep, true);
    }

private:
    void processImpl(float* const* channels, int numChannels, int numFrames,
                     float weight, float target, float step, bool bypassPath) {
        const bool fading = bypassPath && weight != target;
        const int ch = numChannels < numCh_ ? numChannels : numCh_;

        for (int n = 0; n < numFrames; ++n) {
            smoothedDelay_ = targetDelay_ + (smoothedDelay_ - targetDelay_) * smoothCoef_;
            const float d = smoothedDelay_;
            const float fb  = smFb_.next();  // clamped [0, 0.95]
            const float wet = smMix_.next();
            const float dry = 1.0f - wet;
            if (fading) weight = std::clamp(weight + step, 0.0f, 1.0f);
            const float trails = smTrails_.next();
            const float ping   = smPing_.next();

            // Read every channel's delayed sample before writing, so ping-pong
            // cross-feedback sees a consistent snapshot of this frame.
            for (int c = 0; c < ch; ++c) reads_[(size_t)c] = lines_[(size_t)c].readAt(d);

            for (int c = 0; c < ch; ++c) {
                const int partner = c ^ 1;
                const int cross = (partner < ch) ? partner : c; // odd leftover self-feeds
                // Blend the two SOURCES, not two shaped outputs: the tone filter
                // and DC blocker are per-channel state and running them twice
                // would double their contribution mid-fade.
                const float srcSample =
                    (1.0f - ping) * reads_[(size_t)c] + ping * reads_[(size_t)cross];
                const float shaped = dc_[(size_t)c].process(tone_[(size_t)c].process(srcSample));
                const float x = channels[c][n];
                const float feed = bypassPath && trails > 0.0f ? x * (1.0f - trails + trails * weight) : x;
                lines_[(size_t)c].push(feed + fb * shaped);
                float output = dry * x + wet * reads_[(size_t)c];
                if (fading || (bypassPath && weight == 0.0f)) output = x + (output - x) * weight;
                if (bypassPath && trails > 0.0f) {
                    const float tailOutput = (1.0f - wet * weight) * x + wet * reads_[(size_t)c];
                    output += (tailOutput - output) * trails;
                }
                channels[c][n] = output;
            }
        }
    }

public:
    bool needsBypassProcessing() const { return p_.trails || smTrails_.isRamping(); }
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
    SmoothedValue smMix_, smFb_, smTrails_;
    SmoothedValue smPing_;   // 0 = self-feed, 1 = cross-feed
    bool hardParked_ = false;
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
