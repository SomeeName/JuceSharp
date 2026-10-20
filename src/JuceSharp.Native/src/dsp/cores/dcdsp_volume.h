// dcdsp_volume.h — volume / expression pedal (pure dcdsp core, RT-safe).
//
// A pedal, not a knob: `position` is the treadle (0 = heel down, 1 = toe down),
// `curve` is the TAPER of the pot under it, and `minDb`/`maxDb` are what the two
// ends of the sweep mean — heel-down is a mute at -90 dB and a "gain pedal" at
// -12, toe-down can push the chain up to +12.
//
// Topology: position -> taper(curve) -> dB interpolation between minDb and maxDb
// -> linear gain -> ONE smoother -> a per-sample multiply. Nothing else.
//
// THE ONE THING THAT IS EASY TO GET WRONG: the smoother must run on the LINEAR
// gain and `smoothMs` must be re-derived from the rate. A MIDI-CC pedal sends
// 7-bit steps, so an unsmoothed position is a zipper at 128 steps a sweep; and a
// smoother in the dB domain reaches -60 dB two thirds of the way through a
// "smooth" mute, which is a mute that arrives early and audibly. The ramp time
// is the only rate-owned quantity here, and it is re-derived in prepare().
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <algorithm>

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe.
struct VolumeParams {
    float position = 1.0f;    // 0 .. 1   — the treadle (1 = toe down = maxDb)
    float curve    = 0.0f;    // 0 Linear, 1 Log, 2 Audio, 3 S-Curve
    float minDb    = -90.0f;  // -90 .. 0 — what heel-down means (-90 = mute)
    float maxDb    = 0.0f;    // -12 .. 12 — what toe-down means
    float smoothMs = 20.0f;   // 1 .. 200 — ramp time for a position jump
};

class VolumeCore {
public:
    // The taper applied to `position` before the dB interpolation. Static and
    // pure so the managed tests can assert the SHIPPING formula rather than a
    // re-typed copy of it.
    //   Linear  t = p             a linear pot
    //   Log     t = p^2           the square-law "log" taper of a pedal pot
    //   Audio   t = p^3.3219      the classic audio taper: 10 % at half travel
    //   S-Curve t = p^2(3 - 2p)   smoothstep: slow at both ends, quick in the
    //                             middle — the swell-pedal feel
    static float taper(int curve, float p) {
        if (p <= 0.0f) return 0.0f;
        if (p >= 1.0f) return 1.0f;
        switch (curve) {
            case 1:  return p * p;
            case 2:  return std::pow(p, 3.321928f);
            case 3:  return p * p * (3.0f - 2.0f * p);
            default: return p;
        }
    }

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numCh_    = (numChannels > 0) ? numChannels : 1;
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 1;

        // Force the ramp to be re-derived from the NEW rate: smoothS_ is a
        // cache and a cached "unchanged" would leave 20 ms at 48 kHz running as
        // 5 ms at 192 kHz (widener's bass-mono shipped exactly that shape).
        smoothS_ = -1.0f;
        applyParams();
        smGain_.snapToTarget();   // a freshly prepared pedal starts AT its gain
    }

    // State only — the pedal has none beyond its ramp, and a reset must not
    // teleport the gain: land it on the target rather than on 0.
    void reset() { smGain_.snapToTarget(); }

    void setParams(const VolumeParams& p) {
        p_ = p;
        applyParams();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        // An oversized block is SKIPPED WHOLE. Processing the prepared prefix
        // and leaving the tail dry is a discontinuity in the middle of a buffer,
        // which is worse than one silent block.
        if (numFrames > maxBlock_) return;

        const int nch = (numChannels < numCh_) ? numChannels : numCh_;

        // CHANNEL-outer loop: blockStart/advanceBlock, never next(). next()
        // inside the inner sample loop walks the same smoother once per channel,
        // so the ramp would run nch times too fast AND each channel would get a
        // different gain over the same block.
        float step = 0.0f;
        const float g0 = smGain_.blockStart(numFrames, step);

        for (int c = 0; c < nch; ++c) {
            float* p = channels[c];
            if (p == nullptr) continue;
            for (int n = 0; n < numFrames; ++n)
                p[n] *= g0 + step * static_cast<float>(n);
        }
        smGain_.advanceBlock(numFrames);
    }

    int latencySamples() const { return 0; }

    // The steady-state gain the current settings ask for (linear). Exposed for
    // the native regression tools; the audio path reads the smoother.
    float targetGain() const { return smGain_.target(); }

private:
    void applyParams() {
        const float ms = clampf(p_.smoothMs, kMinSmoothMs, kMaxSmoothMs);
        if (ms != smoothS_) {
            smoothS_ = ms;
            smGain_.reset(sr_, ms * 0.001f);   // rate-owned: re-derived here
        }

        const int   curve = clampi(static_cast<int>(std::lround(p_.curve)), 0, 3);
        const float pos   = clampf(p_.position, 0.0f, 1.0f);
        const float lo    = clampf(p_.minDb, kMinDbFloor, 0.0f);
        const float hi    = clampf(p_.maxDb, -12.0f, 12.0f);

        const float t  = taper(curve, pos);
        const float db = lo + t * (hi - lo);
        smGain_.setTarget(static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0)));
    }

    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static int clampi(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr float kMinSmoothMs = 1.0f;
    static constexpr float kMaxSmoothMs = 200.0f;
    static constexpr float kMinDbFloor  = -90.0f;

    VolumeParams p_{};
    double sr_       = 48000.0;
    int    numCh_    = 2;
    int    maxBlock_ = 1;

    SmoothedValue smGain_;
    float smoothS_ = -1.0f;   // the ramp length currently designed, in ms
};

} // namespace dcdsp
