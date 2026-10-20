// dcdsp_octaver.h — monophonic analog-style sub-octave generator (dcdsp core)
//
// Classic flip-flop octave divider: a Schmitt-triggered zero-cross comparator
// clocks a chain of binary dividers. FF1 = one octave down, FF2 = two octaves
// down. The square-wave dividers are amplitude-modulated by the input envelope
// (so the sub tracks picking dynamics), lowpass-filtered (tone) and summed with
// the dry signal. Pure, header-only, RT-safe: allocation only in prepare();
// process() never allocates, locks, or throws.
//
// Codes against the dcdsp_primitives.h signatures (OnePole, EnvelopeFollower,
// DcBlocker). For the standalone compile-test a minimal test_stubs.h is used
// instead; production builds pull in the real shared header.
#pragma once

#ifdef DCDSP_OCTAVER_TEST_STUBS
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <cstddef>
#include <algorithm>

namespace dcdsp {

struct OctaverParams {
    // Level of the one-octave-down square (flip-flop /2). 0..1
    float sub1 = 0.6f;
    // Level of the two-octaves-down square (flip-flop /4). 0..1
    float sub2 = 0.0f;
    // Dry (input) blend level. 0..1
    float dry = 1.0f;
    // Lowpass cutoff applied to the summed sub signal, Hz. 500..5000
    float toneHz = 2000.0f;
};

class OctaverCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        (void)maxBlockSize;
        int nch = numChannels > 0 ? numChannels : 1;
        ch_.assign(static_cast<size_t>(nch), Channel{});
        for (auto& c : ch_) {
            c.env.prepare(sampleRate_);
            c.env.setTimes(2.0f, 60.0f);      // fast attack, moderate release
            c.track.setLowpass(sampleRate_, 1100.0); // clean fundamental for comparator
            // The DC blocker sits on the SUB-OCTAVE itself, so leaving it at
            // DcBlocker's rate-free default (mR = 0.995) put its corner at
            // 38 Hz at 48 kHz -- and at 76 Hz at 96 kHz, 153 Hz at 192 kHz,
            // where the octaver high-passes away the octave it just generated.
            // 20 Hz is the house default every other core asks for (univibe).
            // A low E's sub-octave is 41 Hz: it was sitting on the old corner.
            c.dcOut.prepare(sampleRate_);
        }
        applyTone();
        // The three levels RAMP (kLevelRampSeconds). As per-block constants a
        // slider drag zippered and `dry` 1 -> 0 muted the guitar in one sample.
        sub1S_.reset(sampleRate_, kLevelRampSeconds);
        sub2S_.reset(sampleRate_, kLevelRampSeconds);
        dryS_.reset(sampleRate_, kLevelRampSeconds);
        sub1S_.setTarget(sub1_);
        sub2S_.setTarget(sub2_);
        dryS_.setTarget(dry_);
        sub1S_.snapToTarget();   // a freshly prepared octaver starts AT its levels
        sub2S_.snapToTarget();
        dryS_.snapToTarget();
        reset();
    }

    void reset() {
        for (auto& c : ch_) {
            c.env.reset();
            c.track.reset();
            c.toneLp.reset();
            c.dcOut.reset();
            c.compHigh = false;
            c.ff1 = false;
            c.ff2 = false;
        }
    }

    void setParams(const OctaverParams& p) {
        sub1_ = clamp01(p.sub1);
        sub2_ = clamp01(p.sub2);
        dry_  = clamp01(p.dry);
        sub1S_.setTarget(sub1_);
        sub2S_.setTarget(sub2_);
        dryS_.setTarget(dry_);
        float t = p.toneHz;
        if (!(t == t)) t = 2000.0f;                 // NaN guard
        t = std::min(std::max(t, 500.0f), 5000.0f);
        if (t != toneHz_) { toneHz_ = t; applyTone(); }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (numChannels <= 0 || numFrames <= 0) return;
        int nch = std::min(numChannels, static_cast<int>(ch_.size()));

        // Channel-OUTER loop: every channel is handed the SAME ramp line and the
        // smoothers advance once, after the loop (SmoothedValue::blockStart).
        float sub1Step = 0.0f, sub2Step = 0.0f, dryStep = 0.0f;
        const float sub1Base = sub1S_.blockStart(numFrames, sub1Step);
        const float sub2Base = sub2S_.blockStart(numFrames, sub2Step);
        const float dryBase  = dryS_.blockStart(numFrames, dryStep);

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (!x) continue;
            Channel& s = ch_[static_cast<size_t>(c)];
            for (int n = 0; n < numFrames; ++n) {
                float in = x[n];
                if (!(in == in)) in = 0.0f;          // scrub NaN in
                in = std::min(std::max(in, -8.0f), 8.0f);

                // Envelope of the (bounded) input drives sub dynamics.
                float amp = s.env.process(in);

                // Comparator on a lightly lowpassed copy with amplitude-relative
                // hysteresis → jitter-free zero-cross clock.
                float tracked = s.track.process(in);
                float h = 0.08f * amp + 1.0e-4f;
                bool rising = false;
                if (!s.compHigh && tracked > h) { s.compHigh = true; rising = true; }
                else if (s.compHigh && tracked < -h) { s.compHigh = false; }

                if (rising) {
                    s.ff1 = !s.ff1;                    // /2
                    if (s.ff1) s.ff2 = !s.ff2;         // /4 (clock on ff1 rising)
                }

                // These are FULL-SCALE squares generated at BASE rate, so
                // their edges are quantised to the sample grid and the
                // broadband content that produces is aliased. That is a
                // DELIBERATE choice, not an oversight: an analogue flip-flop
                // divider has exactly this character, toneLp below (a
                // first-order pole at 500-5000 Hz) is what tames it, and the
                // alternative — an 8x wrap around a pure logic stage — would
                // cost real CPU to remove part of the effect's own sound.
                // ADAA (W4-D66) is not the tool either, and not because of
                // taste: ADAA1 integrates the SHAPER between two consecutive
                // input samples, and there is no shaper here. The
                // discontinuity is a flip-flop TOGGLE — a state change, not a
                // function of x[n] — so it has no antiderivative to take. The
                // technique that does apply is polyBLEP, which
                // dcdsp_subharmonic.h uses for exactly this reason; adopting
                // it here would change the divider's character, which is the
                // decision above.
                float sq1 = s.ff1 ? 1.0f : -1.0f;
                float sq2 = s.ff2 ? 1.0f : -1.0f;

                // Amplitude-follow the squares so the sub breathes with the input.
                const float g1 = sub1Base + sub1Step * static_cast<float>(n);
                const float g2 = sub2Base + sub2Step * static_cast<float>(n);
                float sub = (g1 * sq1 + g2 * sq2) * amp;
                sub = s.toneLp.process(sub);
                sub = s.dcOut.process(sub);            // kill square-wave DC offset

                float out = (dryBase + dryStep * static_cast<float>(n)) * in + sub;
                out = std::min(std::max(out, -16.0f), 16.0f); // hard safety bound
                x[n] = out;
            }
        }

        sub1S_.advanceBlock(numFrames);
        sub2S_.advanceBlock(numFrames);
        dryS_.advanceBlock(numFrames);
    }

    int latencySamples() const { return 0; }

private:
    struct Channel {
        EnvelopeFollower env;
        OnePole track;     // pre-comparator lowpass
        OnePole toneLp;    // sub tone lowpass
        DcBlocker dcOut;
        bool compHigh = false;
        bool ff1 = false;
        bool ff2 = false;
    };

    static float clamp01(float v) {
        if (!(v == v)) return 0.0f;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    void applyTone() {
        for (auto& c : ch_) c.toneLp.setLowpass(sampleRate_, toneHz_);
    }

    static constexpr float kLevelRampSeconds = 0.010f;

    double sampleRate_ = 48000.0;
    float sub1_ = 0.6f, sub2_ = 0.0f, dry_ = 1.0f, toneHz_ = 2000.0f;
    SmoothedValue sub1S_, sub2S_, dryS_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
