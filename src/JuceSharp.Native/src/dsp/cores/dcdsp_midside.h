// dcdsp_midside.h — mid/side processor (pure dcdsp core, RT-safe).
//
// The mastering-desk M/S box, as a pedal: encode L/R to mid and side, treat the
// two independently (level, polarity, width, a mono-below corner), decode back,
// and optionally swap the sides.
//
//     M = 0.5*(L + R)                 S = 0.5*(L - R)
//     S' = width * S,  then the bass-mono shelf
//     M' = midGain  * midPhase  * M
//     S' = sideGain * sidePhase * S'
//     L' = M' + S'   R' = M' - S'     (then the swap crossfade)
//
// THE ONE THING THAT IS EASY TO GET WRONG: `monoBelowHz` is a BOUNDED SHELF
// (-26 dB below the corner), never a high-pass — a true HPF on the side is how
// three "reasonable" mono-makers in a row leave a mix with no low end at all,
// and the corner has to be re-designed on EVERY prepare. dcdsp_widener.h cached
// "the frequency did not change" across a prepare that had default-constructed
// its filters, so its bass-mono was a WIRE from the first device change onward;
// this core clears that cache in prepare() for exactly that reason. Every gain
// AND both polarity switches ramp: a polarity flip written straight to the
// output is a full-scale step, which is the loudest click a utility can make.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe.
struct MidSideParams {
    float midDb       = 0.0f;   // -24 .. 12
    float sideDb      = 0.0f;   // -24 .. 12
    float width       = 1.0f;   // 0 .. 2   (0 = mono, 1 = unchanged, 2 = double)
    float midPhase    = 0.0f;   // 0 Normal, 1 Inverted
    float sidePhase   = 0.0f;   // 0 Normal, 1 Inverted
    float monoBelowHz = 0.0f;   // 0 .. 500 (0 = off)
    float swap        = 0.0f;   // 0 Off, 1 On — swap L/R after the decode
};

class MidSideCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numCh_    = (numChannels > 0) ? numChannels : 1;
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 1;

        const int pairs = (numCh_ + 1) / 2;
        sideShelf_.assign(static_cast<std::size_t>(pairs), Biquad{});

        // The line above wiped the shelves back to DEFAULT-CONSTRUCTED Biquads,
        // which are WIRES. applyParams() only re-designs them when the corner
        // CHANGED, and shelfHz_ survives prepare — so without this the feature
        // would be silently, permanently off from the first device change on.
        shelfHz_ = -1.0f;

        smMid_.reset(sr_, kGainRampSeconds);
        smSide_.reset(sr_, kGainRampSeconds);
        smWidth_.reset(sr_, kGainRampSeconds);
        smMidPol_.reset(sr_, kSwitchRampSeconds);
        smSidePol_.reset(sr_, kSwitchRampSeconds);
        smSwap_.reset(sr_, kSwitchRampSeconds);
        smShelf_.reset(sr_, kSwitchRampSeconds);

        applyParams();
        // A freshly prepared box starts AT its settings; snapToTarget, never
        // setCurrentAndTarget(current()) — current() is 0 before the first ramp.
        smMid_.snapToTarget();
        smSide_.snapToTarget();
        smWidth_.snapToTarget();
        smMidPol_.snapToTarget();
        smSidePol_.snapToTarget();
        smSwap_.snapToTarget();
        smShelf_.snapToTarget();
        reset();
    }

    void reset() {
        for (auto& f : sideShelf_) f.reset();
    }

    void setParams(const MidSideParams& p) {
        p_ = p;
        applyParams();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;   // oversized: skipped WHOLE

        const int nch = (numChannels < numCh_) ? numChannels : numCh_;

        // Frame-outer: the M/S matrix couples the two channels of a pair, so
        // this loop cannot be channel-outer and next() is the right walk.
        for (int n = 0; n < numFrames; ++n) {
            const float mg    = smMid_.next();
            const float sg    = smSide_.next();
            const float w     = smWidth_.next();
            const float mpol  = smMidPol_.next();
            const float spol  = smSidePol_.next();
            const float sw    = smSwap_.next();
            const float shelf = smShelf_.next();
            const bool  runShelf = shelfOn_ || shelf > 0.0f;

            int c = 0;
            for (; c + 1 < nch; c += 2) {
                float* pl = channels[c];
                float* pr = channels[c + 1];
                if (pl == nullptr || pr == nullptr) continue;
                const std::size_t pi = static_cast<std::size_t>(c / 2);

                float l = pl[n];
                float r = pr[n];
                if (!std::isfinite(l)) l = 0.0f;
                if (!std::isfinite(r)) r = 0.0f;

                float m = 0.5f * (l + r);
                float s = 0.5f * (l - r);

                s *= w;
                if (runShelf) {
                    // Crossfaded in and out, so engaging the corner is not a
                    // step from a cold filter into a live one.
                    const float f = sideShelf_[pi].process(s);
                    s += shelf * (f - s);
                }

                m *= mg * mpol;
                s *= sg * spol;

                const float outL = m + s;
                const float outR = m - s;

                // The swap is a CROSSFADE, not an exchange: flipping the sides
                // of a live stereo image in one sample is two full-scale steps.
                pl[n] = outL + sw * (outR - outL);
                pr[n] = outR + sw * (outL - outR);
            }
            // An odd trailing channel (and a wholly mono chain) has no side, so
            // it IS the mid: it gets the mid gain and polarity and nothing else.
            for (; c < nch; ++c) {
                float* p = channels[c];
                if (p == nullptr) continue;
                float v = p[n];
                if (!std::isfinite(v)) v = 0.0f;
                p[n] = v * mg * mpol;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    void applyParams() {
        smMid_.setTarget(dbToGain(clampf(p_.midDb, -24.0f, 12.0f)));
        smSide_.setTarget(dbToGain(clampf(p_.sideDb, -24.0f, 12.0f)));
        smWidth_.setTarget(clampf(p_.width, 0.0f, 2.0f));
        smMidPol_.setTarget(p_.midPhase >= 0.5f ? -1.0f : 1.0f);
        smSidePol_.setTarget(p_.sidePhase >= 0.5f ? -1.0f : 1.0f);
        smSwap_.setTarget(p_.swap >= 0.5f ? 1.0f : 0.0f);

        const float hz = clampf(p_.monoBelowHz, 0.0f, kMaxMonoHz);
        const bool wasOn = shelfOn_;
        shelfOn_ = (hz >= kMinMonoHz);
        // Engaging from FULLY OFF starts the biquads from zero. While `shelf`
        // is 0 the filter is not clocked at all, so its state freezes at
        // whatever it held when it was last disengaged — possibly seconds of
        // audio ago — and resuming from that under a 10 ms fade-in puts a stale
        // transient INSIDE the crossfaded signal, where the amplitude fade
        // cannot hide it. Only when the fade has fully landed: a toggle inside
        // the window is a live filter and must keep its state, which is the
        // same rule DspChain::setBypass follows for a parked node.
        if (shelfOn_ && !wasOn && !smShelf_.isRamping() && smShelf_.current() <= 0.0f)
            for (auto& f : sideShelf_) f.reset();
        smShelf_.setTarget(shelfOn_ ? 1.0f : 0.0f);
        if (shelfOn_ && hz != shelfHz_) {
            shelfHz_ = hz;
            for (auto& f : sideShelf_)
                f.setLowShelf(sr_, static_cast<double>(hz), kShelfQ, kShelfDepthDb);
        }
    }

    static float dbToGain(float db) {
        return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
    }
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr float  kGainRampSeconds   = 0.015f;
    static constexpr float  kSwitchRampSeconds = 0.010f;
    static constexpr float  kMinMonoHz         = 20.0f;   // below this it is OFF
    static constexpr float  kMaxMonoHz         = 500.0f;
    static constexpr double kShelfQ            = 0.7071067811865476;
    // BOUNDED, and this number is the whole rule: 26 dB of side removal is
    // inaudible as width and is still not a null, so a second mono-maker later
    // in the chain cannot cascade this into a hole.
    static constexpr double kShelfDepthDb      = -26.0;

    MidSideParams p_{};
    double sr_       = 48000.0;
    int    numCh_    = 2;
    int    maxBlock_ = 1;

    std::vector<Biquad> sideShelf_;   // one per stereo pair
    SmoothedValue smMid_, smSide_, smWidth_;
    SmoothedValue smMidPol_, smSidePol_, smSwap_, smShelf_;
    float shelfHz_ = -1.0f;
    bool  shelfOn_ = false;
};

} // namespace dcdsp
