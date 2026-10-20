// dcdsp_shimmer.h — Shimmer reverb core for the DotCharter native DSP engine.
//
// A Schroeder/Freeverb-style reverb tank whose recirculating feedback is blended
// with a +12-semitone (octave-up) granular pitch shift, so the tail blooms into an
// ethereal shimmering pad.
//
// Contract:
//  * Pure, header-only, C++17. Only depends on the STL headers below.
//  * RT-safe: the ONLY allocation happens in prepare(). reset()/setParams()/process()
//    never allocate, lock, or throw.
//  * process() is IN-PLACE and its output is ALWAYS finite and bounded (|x| < 32).
//    The feedback tank is strictly contractive (loop gain < 1) EVEN WITH the
//    pitch-shifted path, because the octave-up shifter has magnitude gain <= 1
//    (two triangular windows that sum to unity), the damping one-pole has gain <= 1,
//    and the comb feedback coefficient is capped below 1. A final safety clamp
//    guards the output regardless.
//
// This standalone header re-implements the small primitives it needs (comb / allpass /
// one-pole damper / DC blocker / granular octave shifter) so it compiles with no
// external deps. In the real engine these map conceptually onto the shared
// InterpolatedDelayLine / OnePole / DcBlocker primitives.

#pragma once

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared engine primitives (real DcBlocker). The comb/allpass/granular-octave
// building blocks are effect-specific and kept in a per-core namespace below.
#include "dcdsp_primitives.h"

namespace dcdsp {

// ---------------------------------------------------------------------------
// Parameters. All in [min,max] with the documented default.
// ---------------------------------------------------------------------------
struct ShimmerParams {
    // Tail length. Maps to the comb feedback coefficient (0.70 .. 0.96, always < 1).
    // [0, 1], default 0.7
    float decay = 0.7f;

    // Octave-up amount blended into the tank feedback. 0 = plain reverb,
    // 1 = feedback fully pitch-shifted up an octave (max shimmer bloom).
    // [0, 1], default 0.5
    float shimmer = 0.5f;

    // High-frequency absorption in the tail (in-loop one-pole low-pass).
    // Higher = darker / faster high-freq decay. [0, 1], default 0.5
    float damping = 0.5f;

    // Dry/wet blend. 0 = dry only, 1 = wet only. [0, 1], default 0.35
    float mix = 0.35f;
};

namespace shimmer_detail {

// Granular octave-up (ratio = 2) pitch shifter. Two overlapping taps read a
// circular buffer at 2x the write rate; triangular crossfade windows (which sum
// to exactly 1) hide the wrap discontinuity and guarantee |out| <= recent max|in|.
struct OctaveUp {
    std::vector<float> buf;
    int   size = 0;
    int   w    = 0;
    float phase = 0.0f;   // in [0,1), fractional position through the window
    float win  = 1.0f;    // window length in samples

    void prepare(int windowLen) {
        if (windowLen < 64) windowLen = 64;
        win  = static_cast<float>(windowLen);
        size = windowLen * 2 + 4;
        buf.assign(static_cast<std::size_t>(size), 0.0f);
        reset();
    }

    void reset() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        w = 0;
        phase = 0.0f;
    }

    // Linear-interpolated circular read; |result| <= max of the two neighbours.
    static float fracRead(const std::vector<float>& b, int sz, float pos) {
        while (pos < 0.0f)                        pos += static_cast<float>(sz);
        while (pos >= static_cast<float>(sz))     pos -= static_cast<float>(sz);
        int   i0 = static_cast<int>(pos);
        float f  = pos - static_cast<float>(i0);
        int   i1 = i0 + 1; if (i1 >= sz) i1 -= sz;
        return b[static_cast<std::size_t>(i0)] +
               (b[static_cast<std::size_t>(i1)] - b[static_cast<std::size_t>(i0)]) * f;
    }

    float process(float x) {
        buf[static_cast<std::size_t>(w)] = x;

        // ratio = 2 (octave up) => delay shrinks at 1 sample/sample => phase -1/win.
        phase -= 1.0f / win;
        if (phase < 0.0f) phase += 1.0f;

        float p1 = phase;
        float p2 = phase + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;

        float d1 = p1 * win;
        float d2 = p2 * win;

        float r1 = fracRead(buf, size, static_cast<float>(w) - d1);
        float r2 = fracRead(buf, size, static_cast<float>(w) - d2);

        // Triangular windows: win1 + win2 == 1 for all phase => unity magnitude.
        float win1 = 1.0f - std::fabs(2.0f * p1 - 1.0f);
        float win2 = 1.0f - std::fabs(2.0f * p2 - 1.0f);
        float out  = r1 * win1 + r2 * win2;

        if (++w >= size) w = 0;
        return out;
    }
};

// Freeverb-style comb with an in-loop damping low-pass and, blended into the SAME
// feedback path, an octave-up copy. Bounded by construction:
//   |fb| = |(1-shimmer)*store + shimmer*shifted| <= max(|store|,|shifted|) <= M
//   buf = input + feedback*fb  =>  M <= |input|/(1-feedback), feedback < 1.
struct ShimmerComb {
    std::vector<float> buf;
    int      size  = 0;
    int      idx   = 0;
    float    store = 0.0f;   // damping one-pole state
    OctaveUp oct;

    void prepare(int len, int octWin) {
        if (len < 16) len = 16;
        buf.assign(static_cast<std::size_t>(len), 0.0f);
        size = len;
        idx  = 0;
        store = 0.0f;
        oct.prepare(octWin);
    }

    void reset() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        idx = 0;
        store = 0.0f;
        oct.reset();
    }

    float process(float input, float feedback, float damp, float shimmer) {
        float read = buf[static_cast<std::size_t>(idx)];

        // In-loop low-pass (gain <= 1). Higher damp => darker tail.
        store = read * (1.0f - damp) + store * damp;

        float shifted = oct.process(store);

        // (1-shimmer)*store + shimmer*shifted, written as a lerp.
        float fb = store + shimmer * (shifted - store);

        buf[static_cast<std::size_t>(idx)] = input + feedback * fb;
        if (++idx >= size) idx = 0;
        return read;
    }
};

// Freeverb allpass diffuser (g < 1 => bounded / lossless magnitude).
struct Allpass {
    std::vector<float> buf;
    int size = 0;
    int idx  = 0;

    void prepare(int len) {
        if (len < 16) len = 16;
        buf.assign(static_cast<std::size_t>(len), 0.0f);
        size = len;
        idx  = 0;
    }
    void reset() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        idx = 0;
    }
    float process(float input, float g) {
        float bufout = buf[static_cast<std::size_t>(idx)];
        float output = -input + bufout;
        buf[static_cast<std::size_t>(idx)] = input + bufout * g;
        if (++idx >= size) idx = 0;
        return output;
    }
};

} // namespace shimmer_detail

// ---------------------------------------------------------------------------
// ShimmerCore
// ---------------------------------------------------------------------------
class ShimmerCore {
public:
    void prepare(double sr, int /*maxBlock*/, int nch) {
        sr_    = sr > 0.0 ? sr : 44100.0;
        maxCh_ = nch < 1 ? 1 : nch;

        const int octWin = std::max(256, static_cast<int>(0.030 * sr_)); // ~30 ms grains
        const int combT[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        const int apT[4]   = { 556, 441, 341, 225 };
        const double scale = sr_ / 44100.0;

        chans_.clear();
        chans_.resize(static_cast<std::size_t>(maxCh_));
        for (int c = 0; c < maxCh_; ++c) {
            Channel& ch = chans_[static_cast<std::size_t>(c)];
            ch.dc.prepareLegacy48k(sr_);
            const int spread = (c % 2 == 1) ? 23 : 0; // stereo de-correlation
            for (int i = 0; i < 8; ++i)
                ch.combs[static_cast<std::size_t>(i)].prepare(
                    static_cast<int>(combT[i] * scale) + spread, octWin);
            for (int i = 0; i < 4; ++i)
                ch.aps[static_cast<std::size_t>(i)].prepare(
                    static_cast<int>(apT[i] * scale) + spread);
        }
        // `mix` RAMPS (kMixRampSeconds); as a per-block constant a mix drag
        // stepped a 100 % dry/wet crossfade, and the wet arrives with
        // kWetMakeup on top of it.
        mixS_.reset(sr_, kMixRampSeconds);
        mixS_.setTarget(mix_);
        mixS_.snapToTarget();   // a freshly prepared shimmer starts AT its mix
        reset();
    }

    void reset() {
        for (auto& ch : chans_) {
            for (auto& c : ch.combs) c.reset();
            for (auto& a : ch.aps)   a.reset();
            ch.dc.reset();
        }
    }

    void setParams(const ShimmerParams& p) {
        const float decay   = clamp01(p.decay);
        const float shimmer = clamp01(p.shimmer);
        const float damping = clamp01(p.damping);
        const float mix     = clamp01(p.mix);

        feedback_ = 0.70f + 0.26f * decay;   // 0.70 .. 0.96, strictly < 1
        shimmer_  = shimmer;
        damp_     = 0.05f + 0.55f * damping; // 0.05 .. 0.60, low-pass memory
        mix_      = mix;
        mixS_.setTarget(mix_);
    }

    // In-place processing of `nch` planar channels of `nframes` samples each.
    void process(float* const* ch, int nch, int nframes) {
        const int n = nch < maxCh_ ? nch : maxCh_;

        // Channel-OUTER loop: one ramp line for the block, same for every
        // channel, advanced once after the loop.
        float mixStep = 0.0f;
        const float mixBase = mixS_.blockStart(nframes, mixStep);

        for (int c = 0; c < n; ++c) {
            Channel& t = chans_[static_cast<std::size_t>(c)];
            float*   x = ch[c];
            for (int i = 0; i < nframes; ++i) {
                float in = x[i];
                if (!std::isfinite(in)) in = 0.0f;

                const float input = in * kInputGain;

                float sum = 0.0f;
                for (int k = 0; k < 8; ++k)
                    sum += t.combs[static_cast<std::size_t>(k)]
                               .process(input, feedback_, damp_, shimmer_);
                float wet = sum * 0.125f;

                for (int k = 0; k < 4; ++k)
                    wet = t.aps[static_cast<std::size_t>(k)].process(wet, 0.5f);

                wet = t.dc.process(wet);

                const float mixG = mixBase + mixStep * static_cast<float>(i);
                float out = in * (1.0f - mixG) + wet * mixG * kWetMakeup;

                // Final safety clamp — output is always finite and |out| < 32.
                if (!std::isfinite(out)) out = 0.0f;
                if (out >  31.9f) out =  31.9f;
                if (out < -31.9f) out = -31.9f;
                x[i] = out;
            }
        }

        mixS_.advanceBlock(nframes);
    }

    int latencySamples() const { return 0; }

private:
    struct Channel {
        std::array<shimmer_detail::ShimmerComb, 8> combs;
        std::array<shimmer_detail::Allpass, 4>     aps;
        DcBlocker                                  dc;
    };

    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    static constexpr float kInputGain = 0.10f; // keep the tank well below overload
    static constexpr float kWetMakeup = 1.5f;  // wet-path make-up gain

    double sr_    = 44100.0;
    int    maxCh_ = 2;

    float feedback_ = 0.85f;
    float shimmer_  = 0.5f;
    float damp_     = 0.35f;
    float mix_      = 0.35f;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixS_;

    std::vector<Channel> chans_;
};

} // namespace dcdsp
