// dcdsp_limiter.h — LimiterCore: a look-ahead peak limiter as a PEDAL.
//
// Models the brick-wall limiter at the end of a pedalboard (a Boss LM-2 / Keeley
// with a real look-ahead rather than a feedback cell): input gain, a hard
// ceiling the output never crosses, and a program release. The topology is the
// one src/MasterLimiter.h uses on the master bus — peak detector -> SLIDING
// MINIMUM over the look-ahead window (monotonic deque) -> per-sample one-pole ->
// applied to a look-ahead-DELAYED copy, with a clamp at the ceiling as the
// backstop — re-implemented here as a pure dcdsp core, because a DspModule may
// not reach into the engine's own bus stages.
//
// EASY TO GET WRONG, three ways:
//  * The gain computer MUST be the window minimum, not the instantaneous
//    target. With the instantaneous one the smoother only starts moving when
//    the peak is already at the output, every attack punches through, the
//    backstop clamp does all the work and the "limiter" is a clipper.
//  * `gainDb` is an INPUT gain, applied BEFORE the detector and the delay. Put
//    it after and the ceiling stops being a guarantee.
//  * `lookaheadMs` moves a read TAP. The ring is allocated for the WORST case
//    (the param's 5 ms maximum) in prepare() and never resized, and a change is
//    CROSSFADED between the old and new tap over kTapFadeSeconds — a teleporting
//    delay tap is a splice, which is the one thing a limiter must not add. A
//    change requested while a fade is STILL RUNNING is ADOPTED on the callback
//    that sees the fade land, not applied immediately: restarting the fade at
//    zero made `w` jump from its mid-fade value back to 0 and recorded the
//    already-new tap as the old one, i.e. a splice inside the mechanism that
//    exists to prevent splices. It is the deferral dcdsp_vocoder.h uses for its
//    band count, for the same reason — one weight cannot describe a three-way
//    blend.
//    Pinned by Limiter_MovingTheLookaheadTap_DoesNotSpliceTheWaveform. Delete
//    the crossfade and that test sees a 240-sample tap jump; before it existed
//    the whole mechanism could be deleted with the suite staying green.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"   // SmoothedValue
#endif

#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

struct LimiterParams {
    float ceilingDb   = -1.0f;  // output ceiling, dBFS   [-20 .. 0]
    float releaseMs   = 80.0f;  // gain recovery time     [5 .. 500]
    float lookaheadMs = 1.0f;   // detector advance       [0 .. 5]
    float gainDb      = 0.0f;   // INPUT gain             [-12 .. 12]
};

class LimiterCore {
public:
    /// The param maximum, and therefore the size prepare() allocates for. A
    /// runtime lookaheadMs only ever selects a SHORTER tap into this ring.
    static constexpr float kMaxLookaheadMs = 5.0f;

    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_  = sampleRate > 0.0 ? sampleRate : 48000.0;
        nch_ = numChannels > 0 ? numChannels : 1;

        // Worst case, once. `lookahead_` below is only ever an index into this.
        maxDelay_ = (int) std::lround(sr_ * (double) kMaxLookaheadMs / 1000.0) + 2;
        if (maxDelay_ < 2) maxDelay_ = 2;
        ring_.assign((size_t) nch_ * (size_t) maxDelay_, 0.0f);
        dqVal_.assign((size_t) maxDelay_ + 1, 0.0f);
        dqIdx_.assign((size_t) maxDelay_ + 1, 0LL);

        gainSmooth_.reset(sr_, kGainRampSeconds);
        tapFade_.reset(sr_, kTapFadeSeconds);

        recalc();                       // rate-owned: tap length, coefficients
        gainSmooth_.setTarget(inputLin());
        gainSmooth_.snapToTarget();     // start AT the configured input gain
        tapFade_.setCurrentAndTarget(1.0f);
        lookahead_ = oldLookahead_ = lookaheadTarget_;
        reset();
    }

    void reset() {
        for (auto& s : ring_) s = 0.0f;
        writePos_  = 0;
        sampleIdx_ = 0;
        dqHead_ = dqCount_ = 0;
        gain_ = 1.0f;
    }

    void setParams(const LimiterParams& p) {
        p_ = p;
        clamp();
        gainSmooth_.setTarget(inputLin());
        // recalc() only moves the TARGET tap. The crossfade onto it is started
        // by the callback, once any fade already in flight has landed.
        recalc();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        if (ring_.empty()) return;
        const int nch = numChannels < nch_ ? numChannels : nch_;

        // A pending tap move is adopted HERE, once the previous crossfade has
        // landed, so two fades can never overlap. Both taps read the same ring,
        // so there is nothing to allocate and nothing to clear.
        if (! tapFade_.isRamping() && lookahead_ != lookaheadTarget_) {
            oldLookahead_ = lookahead_;
            lookahead_    = lookaheadTarget_;
            tapFade_.setCurrentAndTarget(0.0f);
            tapFade_.setTarget(1.0f);
        }

        const float ceilLin = (float) dbToGain((double) p_.ceilingDb);
        const int   L       = lookahead_;
        const int   oldL    = oldLookahead_;

        for (int n = 0; n < numFrames; ++n) {
            const float inGain = gainSmooth_.next();

            // --- input gain, then the peak of the INCOMING frame ------------
            float peak = 0.0f;
            for (int ch = 0; ch < nch; ++ch) {
                float* p = channels[ch];
                if (p == nullptr) continue;
                const float v = p[n] * inGain;
                p[n] = v;
                const float a = std::fabs(v);
                if (a > peak) peak = a;
            }

            float target = 1.0f;
            if (std::isfinite(peak) && peak > ceilLin) target = ceilLin / peak;

            // --- sliding minimum over the look-ahead window -----------------
            pushTarget(target, L);
            const float windowMin = dqVal_[(size_t) dqHead_];

            const float coef = (windowMin < gain_) ? attackCoef_ : releaseCoef_;
            gain_ += (windowMin - gain_) * coef;
            if (! std::isfinite(gain_)) gain_ = 1.0f;
            if (gain_ > 1.0f) gain_ = 1.0f;
            if (gain_ < 0.0f) gain_ = 0.0f;
            // A one-pole approaches unity asymptotically and never arrives, so
            // without this snap a limiter that has once worked multiplies every
            // later sample by 0.9999 for the life of the node — and the "a sine
            // under the ceiling passes unchanged" contract would be false by a
            // hundredth of a dB for ever.
            if (windowMin >= 1.0f && gain_ > 0.9995f) gain_ = 1.0f;

            // --- the delayed read, the gain, then the backstop clamp --------
            const float w = tapFade_.next();
            const size_t slot = (size_t) writePos_;
            for (int ch = 0; ch < nch; ++ch) {
                float* p = channels[ch];
                if (p == nullptr) continue;
                float* line = ring_.data() + (size_t) ch * (size_t) maxDelay_;
                const float dNew = line[readSlot(L)];
                const float dOld = (w >= 1.0f) ? dNew : line[readSlot(oldL)];
                line[slot] = p[n];
                float v = (dOld + (dNew - dOld) * w) * gain_;
                // The smoother never reaches its target exactly, so a fast
                // transient can still leave tenths of a dB of overshoot. The
                // clamp is what makes the ceiling a GUARANTEE, and by
                // construction it only touches samples the smoother nearly
                // caught.
                if (v >  ceilLin) v =  ceilLin;
                if (v < -ceilLin) v = -ceilLin;
                p[n] = v;
            }
            if (++writePos_ >= maxDelay_) writePos_ = 0;
        }
        if (! tapFade_.isRamping()) oldLookahead_ = lookahead_;
    }

    /// The tap the audio path settles on, in samples, so the chain's PDC
    /// budgets for what is there. During the 10 ms crossfade onto a new tap the
    /// path is reading a BLEND of two, which no single number describes; this
    /// reports the DESTINATION, because that is where PDC should already be
    /// pointing when the fade lands. Never zero — a zero-length look-ahead is a
    /// clipper, so the tap floors at one sample.
    int latencySamples() const { return lookaheadTarget_; }

private:
    int readSlot(int delaySamples) const {
        int s = writePos_ - delaySamples;
        while (s < 0) s += maxDelay_;
        return s;
    }

    /// Monotonic-deque push for the sliding window minimum. Amortised O(1); a
    /// naive rescan would be up to 960 ops per sample at 192 kHz and 5 ms.
    void pushTarget(float t, int L) {
        const int cap = (int) dqVal_.size();
        while (dqCount_ > 0) {
            const int back = (dqHead_ + dqCount_ - 1) % cap;
            if (dqVal_[(size_t) back] < t) break;
            --dqCount_;
        }
        const int slot = (dqHead_ + dqCount_) % cap;
        dqVal_[(size_t) slot] = t;
        dqIdx_[(size_t) slot] = sampleIdx_;
        ++dqCount_;
        while (dqCount_ > 0 && dqIdx_[(size_t) dqHead_] <= sampleIdx_ - (long long) L) {
            dqHead_ = (dqHead_ + 1) % cap;
            --dqCount_;
        }
        ++sampleIdx_;
    }

    void clamp() {
        p_.ceilingDb   = clampf(p_.ceilingDb, -20.0f, 0.0f, 0.0f);
        p_.releaseMs   = clampf(p_.releaseMs, 5.0f, 500.0f);
        p_.lookaheadMs = clampf(p_.lookaheadMs, 0.0f, kMaxLookaheadMs);
        // A NaN lands on the NEUTRAL value, never on `lo`: for these two `lo`
        // IS the most extreme setting the knob has (-20 dBFS of ceiling, -12 dB
        // of input trim), so a rejected NaN would be the loudest possible
        // answer to "this value is garbage".
        p_.gainDb      = clampf(p_.gainDb, -12.0f, 12.0f, 0.0f);
    }

    /// Everything the RATE owns. prepare() and setParams both come through
    /// here, which is what keeps a device change from leaving a 48 kHz tap
    /// length running at 192 kHz.
    void recalc() {
        lookaheadTarget_ = (int) std::lround(sr_ * (double) p_.lookaheadMs / 1000.0);
        if (lookaheadTarget_ < 1) lookaheadTarget_ = 1;            // never 0: see latencySamples()
        if (lookaheadTarget_ > maxDelay_ - 1) lookaheadTarget_ = maxDelay_ - 1;
        // tau = L/4 samples: the one-pole is within ~2 % of the window minimum
        // after a full window and the clamp covers the remainder. dt/tau, so a
        // release change costs a divide and not an exp() on the callback.
        attackCoef_  = std::fmin(1.0f, 4.0f / (float) lookaheadTarget_);
        const float tau = std::fmax(1.0f, p_.releaseMs * (float) sr_ * 0.001f);
        releaseCoef_ = std::fmin(1.0f, 1.0f / tau);
    }

    float inputLin() const { return (float) dbToGain((double) p_.gainDb); }

    /// NaN lands on `fallback`, which defaults to `lo` for the params whose
    /// neutral IS the bottom of their range. `ceilingDb` and `gainDb` pass
    /// their own neutral instead: for those, `lo` is the most extreme setting
    /// the knob has.
    static float clampf(float v, float lo, float hi) { return clampf(v, lo, hi, lo); }
    static float clampf(float v, float lo, float hi, float fallback) {
        if (v != v) return fallback;          // NaN
        if (v < lo) return lo;
        return v > hi ? hi : v;
    }

    static constexpr float kGainRampSeconds = 0.010f;
    static constexpr float kTapFadeSeconds  = 0.010f;

    LimiterParams p_{};
    double sr_  = 48000.0;
    int    nch_ = 1;

    std::vector<float>     ring_;
    int                    maxDelay_ = 0;
    int                    writePos_ = 0;
    std::vector<float>     dqVal_;
    std::vector<long long> dqIdx_;
    int                    dqHead_  = 0;
    int                    dqCount_ = 0;
    long long              sampleIdx_ = 0;

    int   lookaheadTarget_ = 1;   // what the knob asks for
    int   lookahead_    = 1;      // the tap currently faded IN
    int   oldLookahead_ = 1;      // the tap fading OUT
    float gain_        = 1.0f;
    float attackCoef_  = 0.1f;
    float releaseCoef_ = 0.01f;
    SmoothedValue gainSmooth_;
    SmoothedValue tapFade_;
};

} // namespace dcdsp
