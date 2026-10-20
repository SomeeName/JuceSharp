// dcdsp_tzflanger.h — thru-zero (tape) flanger core (header-only, pure, RT-safe).
//
// Models the two-tape-machine flange: TWO delay lines per channel fed the same
// input. The reference ("dry") path sits at a FIXED manualMs; the second path
// sweeps manualMs +/- depthMs around it, so the RELATIVE delay between them
// passes through zero twice per LFO cycle. At that instant the two paths are
// sample-identical and sum coherently (+6 dB over either half); a quarter cycle
// later the comb notches are back. A single modulated line cannot do this — its
// relative delay never reaches zero, which is the one thing that is easy to get
// wrong here. The price of the fixed reference path is REAL latency, so
// latencySamples() reports manualMs and the chain's PDC compensates it.
//
// Namespace dcdsp. C++17. Deps: only the shared primitives header.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct TzFlangerParams {
    float rateHz   = 0.3f;  // 0.02 .. 10   — sweep rate
    float depthMs  = 3.0f;  // 0.1  .. 10   — sweep span either side of manual
    float manualMs = 3.0f;  // 0    .. 10   — the fixed reference delay
    float feedback = 0.3f;  // -0.95 .. 0.95 (regeneration, on the swept path)
    float mix      = 0.5f;  // 0    .. 1    — reference/swept blend
    int   shape    = 0;     // 0 = Sine, 1 = Triangle
};

class TzFlangerCore {
public:
    static constexpr float kMinRate   = 0.02f;
    static constexpr float kMaxRate   = 10.0f;
    static constexpr float kMinDepth  = 0.1f;
    static constexpr float kMaxDepth  = 10.0f;
    static constexpr float kMaxManual = 10.0f;

    /// Latency the FIXED reference path adds, in samples, for a given manual
    /// setting. One definition, read by the core and by the adapter's
    /// latencySamples() (which must answer from the MESSAGE thread's copy of
    /// the params, not from the audio thread's).
    ///
    /// The +1 is not a fudge: both lines are READ before they are pushed, so
    /// readAt(0) is the previous sample. Reading both the same way is what
    /// makes the zero crossing exact.
    static int latencySamplesFor(float manualMs, double sampleRate) {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        const float m = clampNan(manualMs, 0.0f, kMaxManual, 0.0f);
        return (int) std::lround((double) m * 0.001 * sr) + 1;
    }

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_    = numChannels < 1 ? 1 : numChannels;
        maxBlock_ = maxBlockSize < 1 ? 1 : maxBlockSize;

        // WORST CASE, always: the longest tap this core can ever ask for is
        // manual(max) + depth(max) + the Lagrange headroom. Sizing from the
        // CURRENT params would make a later legal manualMs/depthMs write read
        // off the end of a buffer prepare() had allocated for a smaller mode.
        const double maxDelayMs = (double) kMaxManual + (double) kMaxDepth + 2.0;
        const int maxSamps = (int) std::ceil(maxDelayMs * 0.001 * sr_) + 4;
        refLines_.resize((size_t) numCh_);
        modLines_.resize((size_t) numCh_);
        dc_.resize((size_t) numCh_);
        for (auto& l : refLines_) l.prepare(maxSamps);
        for (auto& l : modLines_) l.prepare(maxSamps);
        // The DC blocker is INSIDE the regeneration loop, so its corner
        // compounds once per repeat; rate-derive it or it rises with the device
        // rate (the bug dcdsp_flanger.h's prepare() documents).
        for (auto& d : dc_) d.prepare(sr_);

        lfo_.prepare(sr_);
        lfo_.setFrequency(clampNan(p_.rateHz, kMinRate, kMaxRate, 0.3f));
        shapeCur_ = shapePrev_ = shapeFor(p_.shape);
        lfo_.setShape(shapeCur_);
        shapeFade_.reset(sr_, kShapeFadeSeconds);
        shapeFade_.setCurrentAndTarget(1.0f);

        // ~15 ms on every continuous quantity. depthMs and manualMs are DELAY
        // LENGTHS: a legal 0.1 -> 10 ms jump moves a read tap by 475 samples at
        // 48 kHz, i.e. splices the waveform, and no amount of mix smoothing
        // hides that.
        smDepth_.reset(sr_, kParamRampSeconds);
        smManual_.reset(sr_, kParamRampSeconds);
        smMix_.reset(sr_, kParamRampSeconds);
        smFb_.reset(sr_, kParamRampSeconds);
        retarget();
        smDepth_.snapToTarget(); smManual_.snapToTarget();
        smMix_.snapToTarget();   smFb_.snapToTarget();
        shapeFade_.snapToTarget();
        primed_ = false;   // the first REAL setParams snaps, so a bounce is
                           // deterministic from sample 0 (dcdsp_delay.h's rule)
        reset();
    }

    void reset() {
        for (auto& l : refLines_) l.reset();
        for (auto& l : modLines_) l.reset();
        for (auto& d : dc_) d.reset();
        lfo_.reset();
    }

    void setParams(const TzFlangerParams& p) {
        p_ = p;
        lfo_.setFrequency(clampNan(p.rateHz, kMinRate, kMaxRate, 0.3f));
        applyShape();
        retarget();
        if (!primed_) {
            smDepth_.snapToTarget(); smManual_.snapToTarget();
            smMix_.snapToTarget();   smFb_.snapToTarget();
            shapeFade_.snapToTarget();
            primed_ = true;
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        // An oversized block is skipped WHOLE — never half-processed.
        if (numFrames > maxBlock_) return;
        const int ch = numChannels < numCh_ ? numChannels : numCh_;
        const float maxTap = (float) ((kMaxManual + kMaxDepth) * 0.001 * sr_);

        for (int n = 0; n < numFrames; ++n) {
            // Sample-outer loop, so next() composes: every channel is handed the
            // SAME value for this frame and the smoothers walk once per frame.
            const float depth  = smDepth_.next();
            const float manual = smManual_.next();
            const float fb     = smFb_.next();
            const float wetAmt = smMix_.next();
            const float dryAmt = 1.0f - wetAmt;

            const float t = shapeFade_.next();
            float m = (t >= 1.0f) ? lfo_.peek(shapeCur_)
                                  : (1.0f - t) * lfo_.peek(shapePrev_) + t * lfo_.peek(shapeCur_);
            lfo_.advance();
            if (!std::isfinite(m)) m = 0.0f;

            float modTap = manual + depth * m;
            if (modTap < 0.0f)   modTap = 0.0f;
            if (modTap > maxTap) modTap = maxTap;

            for (int c = 0; c < ch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                const float x = buf[n];
                const size_t ci = (size_t) c;

                // READ then PUSH on BOTH lines. Same ordering on both is what
                // makes modTap == manual a sample-exact coincidence rather than
                // a one-sample comb at 24 kHz.
                const float ref = refLines_[ci].readAt(manual);
                const float mod = modLines_[ci].readAt(modTap);
                refLines_[ci].push(x);
                float fbIn = fb * dc_[ci].process(mod);
                if (!std::isfinite(fbIn)) fbIn = 0.0f;
                fbIn = fbIn < -4.0f ? -4.0f : (fbIn > 4.0f ? 4.0f : fbIn);
                modLines_[ci].push(x + fbIn);

                float out = dryAmt * ref + wetAmt * mod;
                if (!std::isfinite(out)) out = 0.0f;
                buf[n] = out;
            }
        }
    }

    int latencySamples() const { return latencySamplesFor(p_.manualMs, sr_); }

private:
    static float clampNan(float v, float lo, float hi, float fallback) {
        if (!std::isfinite(v)) return fallback;   // a NaN passes every <= gate
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static Lfo::Shape shapeFor(int s) { return s == 1 ? Lfo::Tri : Lfo::Sine; }

    void retarget() {
        const double msToSamp = 0.001 * sr_;
        smDepth_.setTarget((float) (clampNan(p_.depthMs, kMinDepth, kMaxDepth, 3.0f) * msToSamp));
        smManual_.setTarget((float) (clampNan(p_.manualMs, 0.0f, kMaxManual, 3.0f) * msToSamp));
        smMix_.setTarget(clampNan(p_.mix, 0.0f, 1.0f, 0.5f));
        smFb_.setTarget(clampNan(p_.feedback, -0.95f, 0.95f, 0.0f));
    }

    // A shape switch is a full-scale discontinuity in the SWEEP (sine and
    // triangle differ by up to 1.0 at one phase, and the LFO here IS the read
    // tap), so it crossfades. Flipping BACK inside the window swaps the two
    // ends and keeps the blend's position — discarding it is the click the
    // crossfade exists to remove (dcdsp_ringmod.h's applyShape, same shape).
    void applyShape() {
        const Lfo::Shape want = shapeFor(p_.shape);
        lfo_.setShape(want);
        if (want == shapeCur_) return;
        if (want == shapePrev_ && shapeFade_.isRamping()) {
            const float t = shapeFade_.current();
            shapePrev_ = shapeCur_;
            shapeCur_  = want;
            shapeFade_.setCurrentAndTarget(1.0f - t);
        } else {
            shapePrev_ = shapeCur_;
            shapeCur_  = want;
            shapeFade_.setCurrentAndTarget(0.0f);
        }
        shapeFade_.setTarget(1.0f);
    }

    static constexpr float kShapeFadeSeconds = 0.010f;
    static constexpr float kParamRampSeconds = 0.015f;

    std::vector<InterpolatedDelayLine> refLines_, modLines_;
    std::vector<DcBlocker> dc_;
    Lfo lfo_;
    SmoothedValue smDepth_, smManual_, smMix_, smFb_, shapeFade_;
    Lfo::Shape shapeCur_ = Lfo::Sine, shapePrev_ = Lfo::Sine;
    bool primed_ = false;
    TzFlangerParams p_{};
    double sr_ = 48000.0;
    int numCh_ = 2;
    int maxBlock_ = 2048;
};

} // namespace dcdsp
