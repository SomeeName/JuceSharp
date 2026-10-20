// dcdsp_tapesat.h — tape-machine saturation and head colour.
//
// Models: the record chain of an analogue tape deck — a pre-emphasis shelf
// into the magnetic saturation of the oxide, the matching de-emphasis on
// playback (which is where tape's soft HF COMPRESSION comes from, not from a
// low-pass), the low-frequency "head bump" of the playback head's gap, the
// gap-loss HF roll-off, and a switchable noise floor.
// Topology: dry split (delay-compensated) -> pre-emphasis -> [4x oversampled:
// drive -> biased ADAA tanh -> unity-gain normalise] -> DC block ->
// de-emphasis -> 30 Hz head bump -> gap-loss roll-off -> hiss -> dry/wet mix.
//
// Easy to get wrong: the emphasis pair must be EXACTLY complementary, or the
// module stops being unity at drive 0 and starts being a tone control. And the
// dry leg has to be delayed by the oversampler's group delay: mixing an
// undelayed dry against the wet is a comb filter, not a blend.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace dcdsp {

struct TapeSatParams {
    float drive         = 0.3f;  // 0..1   into the oxide, -10 dB .. +18 dB
    float bias          = 0.5f;  // 0..1   record-bias asymmetry; 0.5 = symmetric
    float hfRolloffKHz  = 12.f;  // 4..20  gap-loss corner
    float hiss          = 0.f;   // 0..1   noise floor, -70 dBFS at 1.0
    float mix           = 1.f;   // 0..1   dry/wet
};

class TapeSatCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        const int n = std::max(1, numChannels);
        ch_.assign(static_cast<size_t>(n), Channel{});
        uint32_t seed = 0x9E3779B9u;
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOs);
            dryDelaySamples_ = c.os.groupDelayBaseSamples() + 0.5 / kOs;
            c.dryDelay.prepare(static_cast<int>(std::ceil(dryDelaySamples_)) + 4);
            c.dc.prepare(sr_, 10.0);        // below the 30 Hz head bump
            c.dc.reset();
            c.pre.reset(); c.de.reset(); c.bump.reset();
            c.lp1.reset(); c.lp2.reset();
            c.sat = 0.f;
            c.seed = seed;
            c.rng  = seed;
            seed += 0x85EBCA6Bu;            // one deterministic stream per channel
        }
        driveBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        biasBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        hissBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        mixBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        dryBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);

        drive_.reset(sr_, kRampSeconds);
        bias_.reset(sr_, kRampSeconds);
        hiss_.reset(sr_, kRampSeconds);
        mix_.reset(sr_, kRampSeconds);
        setParams(params_);
        drive_.snapToTarget();
        bias_.snapToTarget();
        hiss_.snapToTarget();
        mix_.snapToTarget();
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset(); c.dryDelay.reset(); c.dc.reset();
            c.pre.reset(); c.de.reset(); c.bump.reset();
            c.lp1.reset(); c.lp2.reset();
            c.sat = 0.f;
            c.rng = c.seed;   // the hiss stream is deterministic from a reset
        }
    }

    void setParams(const TapeSatParams& p) {
        params_ = p;
        // -10 dB .. +18 dB. The knob is the RECORD LEVEL going onto the tape,
        // so its bottom end sits BELOW unity: at drive 0 the saturator has
        // 10 dB of headroom over a full-scale input and the module is a wire
        // to within 0.2 dB, which is what makes "drive 0 is clean" true rather
        // than merely quiet. Starting at unity leaves tanh() audibly
        // compressing a -3 dBFS input with the knob all the way down.
        drive_.setTarget(std::pow(10.f, (clamp01(p.drive) * 28.f - 10.f) / 20.f));
        bias_.setTarget((clamp01(p.bias) - 0.5f) * 0.8f);
        // Uniform noise has RMS 1/sqrt(3), so scale it back to make `hiss` read
        // directly as -70 dBFS RMS at 1.0.
        hiss_.setTarget(clamp01(p.hiss) * kHissFull * 1.7320508f);
        mix_.setTarget(clamp01(p.mix));
        const double rollHz = clampRolloff(p.hfRolloffKHz) * 1000.0;
        for (auto& c : ch_) {
            c.pre.setHighShelf(sr_, kEmphasisHz, 0.707, kEmphasisDb);
            c.de.setHighShelf(sr_, kEmphasisHz, 0.707, -kEmphasisDb);
            c.bump.setPeaking(sr_, 30.0, 1.0, 3.0);
            // nearNyquist (DotCharter #87): `hfRolloffKHz` runs 4-20 kHz;
            // past a quarter of the rate it used to land on 12 kHz.
            c.lp1.setLowpass(sr_, rollHz, 0.707, true);
            c.lp2.setLowpass(sr_, rollHz, 0.707, true);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // skip, never half-process
        const int nch = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        for (int i = 0; i < frames; ++i) {
            driveBuf_[static_cast<size_t>(i)] = drive_.next();
            biasBuf_[static_cast<size_t>(i)]  = bias_.next();
            hissBuf_[static_cast<size_t>(i)]  = hiss_.next();
            mixBuf_[static_cast<size_t>(i)]   = mix_.next();
        }

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (x == nullptr) continue;
            Channel& st = ch_[static_cast<size_t>(c)];

            for (int i = 0; i < frames; ++i) {
                const float s = detail::sanitize(x[i]);
                st.dryDelay.push(s);
                dryBuf_[static_cast<size_t>(i)] =
                    st.dryDelay.readAt(static_cast<float>(dryDelaySamples_));
                x[i] = detail::sanitize(st.pre.process(s));
            }

            st.os.upsample(x, frames);
            float* up = st.os.data();
            const int m = st.os.upSize(frames);
            for (int k = 0; k < m; ++k) {
                const size_t b = static_cast<size_t>(k / kOs);
                const float g = driveBuf_[b];
                // Unity small-signal gain: tanh(g*x)/g -> x as x -> 0, so the
                // drive knob adds saturation without adding level.
                const float s = adaaTanh(g * up[k] + biasBuf_[b], st.sat) / g;
                up[k] = detail::sanitize(s);
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                float s = st.dc.process(x[i]);     // the bias offset leaves here
                s = st.de.process(s);              // exactly undoes the pre-emphasis
                s = st.bump.process(s);
                s = st.lp2.process(st.lp1.process(s));
                s += hissBuf_[b] * nextNoise(st.rng);
                const float mx = mixBuf_[b];
                x[i] = detail::sanitize(mx * s + (1.f - mx) * dryBuf_[b]);
            }
        }
    }

    // The dry leg is ALWAYS delay-compensated, so the reported latency does not
    // move with `mix` — a figure that toggled with a continuous knob would churn
    // the chain's PDC on every drag.
    int latencySamples() const { return static_cast<int>(std::lround(dryDelaySamples_)); }

    float driveCurrentForTest() const { return drive_.current(); }
    float mixCurrentForTest()   const { return mix_.current(); }

private:
    static constexpr int    kOs          = 4;       // soft saturator: 4x
    static constexpr float  kRampSeconds = 0.010f;
    static constexpr double kEmphasisHz  = 3000.0;
    static constexpr double kEmphasisDb  = 9.0;
    static constexpr float  kHissFull    = 3.1622777e-4f;  // -70 dBFS

    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.f;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }
    static double clampRolloff(float v) {
        if (!(v > 4.f)) return 4.0;   // NaN lands here
        return v > 20.f ? 20.0 : static_cast<double>(v);
    }
    // xorshift32: one multiply-free step, no allocation, no table, and the same
    // sequence from the same seed on every platform.
    static float nextNoise(uint32_t& s) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (1.f / 8388608.f) - 1.f;   // uniform [-1, 1)
    }

    struct Channel {
        detail::OversamplerNx os;
        InterpolatedDelayLine dryDelay;
        Biquad    pre, de, bump, lp1, lp2;
        DcBlocker dc;
        float     sat = 0.f;       // ADAA state
        uint32_t  seed = 1u, rng = 1u;
    };

    double sr_ = 48000.0;
    double dryDelaySamples_ = 0.0;
    int maxBlock_ = 512;
    TapeSatParams params_{};
    SmoothedValue drive_, bias_, hiss_, mix_;
    std::vector<float> driveBuf_, biasBuf_, hissBuf_, mixBuf_, dryBuf_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
