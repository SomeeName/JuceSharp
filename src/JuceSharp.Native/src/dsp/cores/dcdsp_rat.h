// dcdsp_rat.h — "Rodent" op-amp distortion (ProCo RAT family).
//
// Models: a very high gain LM308 op-amp stage whose feedback leg loses bass as
// the gain pot opens, a treble-emphasis cap before the clipper, a pair of
// shunt diodes to ground (silicon / LED / one-of-each "turbo"), and the pedal's
// single passive RC Filter knob — which is a LOW-PASS, so CLOCKWISE IS DARKER.
// Topology: bounded pre-clip bass shelf -> fixed +6 dB treble emphasis ->
// [8x oversampled: op-amp gain -> asymmetric ADAA hard clip -> makeup] ->
// DC block -> one-pole Filter -> volume.
//
// Easy to get wrong: `clip` is a FOOTSWITCH that moves two per-sample
// multipliers (both clip thresholds) and the makeup gain. Stepping them splices
// the waveform, so all three are SmoothedValues crossfaded over 10 ms — the
// same treatment dcdsp_overdrive.h gives its `model`. The second trap is the
// Filter knob: spelling it as a high-pass-complement or inverting the taper
// gives a pedal that gets BRIGHTER clockwise, which is backwards.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct RatParams {
    float distortion = 0.5f; // 0..1  op-amp gain, 0 dB .. +45 dB
    float filter     = 0.4f; // 0..1  passive low-pass, 0 = open (12 kHz), 1 = dark (500 Hz)
    float volume     = 0.6f; // 0..1  output level
    int   clip       = 0;    // 0 silicon / 1 LED / 2 turbo
};

class RatCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOs);
            c.dc.prepare(sr_, 18.0);        // rate-derived corner, not a fixed coefficient
            c.dc.reset();
            c.preLo.reset(); c.preHi.reset(); c.filt.reset();
            c.clipState = 0.f;
        }
        preGainBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        outGainBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        loBuf_.assign(static_cast<size_t>(maxBlock_), -1.f);
        hiBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        mkBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);

        preGain_.reset(sr_, kRampSeconds);
        outGain_.reset(sr_, kRampSeconds);
        loThr_.reset(sr_, kRampSeconds);
        hiThr_.reset(sr_, kRampSeconds);
        makeup_.reset(sr_, kRampSeconds);
        setParams(params_);
        // A freshly prepared pedal starts AT its configured settings; reset()
        // preserves mCurrent (0 on a first prepare), so without this the first
        // 10 ms fades up from silence and an offline bounce is not
        // deterministic from sample 0.
        preGain_.snapToTarget();
        outGain_.snapToTarget();
        loThr_.snapToTarget();
        hiThr_.snapToTarget();
        makeup_.snapToTarget();
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset(); c.dc.reset();
            c.preLo.reset(); c.preHi.reset(); c.filt.reset();
            c.clipState = 0.f;
        }
    }

    void setParams(const RatParams& p) {
        params_ = p;
        const float dist = clamp01(p.distortion);
        const float filt = clamp01(p.filter);
        const float vol  = clamp01(p.volume);
        const int   ci   = p.clip < 0 ? 0 : (p.clip > kNumClips - 1 ? kNumClips - 1 : p.clip);
        const Clip& cl   = kClips[ci];

        preGain_.setTarget(std::pow(10.f, dist * 45.f / 20.f));
        outGain_.setTarget(vol * 1.2f);
        loThr_.setTarget(cl.lo);
        hiThr_.setTarget(cl.hi);
        makeup_.setTarget(cl.makeup);

        // BOUNDED pre-clip bass shelf, never a high-pass: the RAT's LF loss
        // lives in the op-amp FEEDBACK leg, so the unity path still passes the
        // fundamental and the depth is finite (<= 8 dB) however far the gain
        // pot is opened. Cascading unbounded high-passes here is what put the
        // 0.16 overdrive/distortion cores 20 dB below their own midband at the
        // low E — see dcdsp_overdrive.h.
        const double preCutDb = 2.0 + 6.0 * static_cast<double>(dist);
        // Passive RC Filter: 12 kHz fully anticlockwise down to 500 Hz fully
        // clockwise, logarithmic like the pot it models.
        const double filtHz = 500.0 * std::pow(24.0, 1.0 - static_cast<double>(filt));
        for (auto& c : ch_) {
            c.preLo.setLowShelf(sr_, 180.0, 0.707, -preCutDb);
            c.preHi.setHighShelf(sr_, 1600.0, 0.707, 6.0); // the "ruetz" emphasis cap
            c.filt.setLowpass(sr_, filtHz);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        // SKIP an oversized block rather than processing part of it: the tail
        // past maxBlock_ would be left UNTOUCHED, i.e. full-level clean DI
        // spliced into the middle of a distorted block.
        if (numFrames > maxBlock_) return;
        const int nch = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        for (int i = 0; i < frames; ++i) {
            preGainBuf_[static_cast<size_t>(i)] = preGain_.next();
            outGainBuf_[static_cast<size_t>(i)] = outGain_.next();
            loBuf_[static_cast<size_t>(i)]      = loThr_.next();
            hiBuf_[static_cast<size_t>(i)]      = hiThr_.next();
            mkBuf_[static_cast<size_t>(i)]      = makeup_.next();
        }

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (x == nullptr) continue;
            Channel& st = ch_[static_cast<size_t>(c)];

            // Front end at BASE rate. The op-amp gain (up to +45 dB) is NOT
            // applied here: multiplying and clamping at base rate would clip
            // before the oversampler could help, aliasing the result.
            for (int i = 0; i < frames; ++i) {
                float s = detail::sanitize(x[i]);
                s = st.preLo.process(s);
                s = st.preHi.process(s);
                x[i] = detail::sanitize(s);
            }

            st.os.upsample(x, frames);
            float* up = st.os.data();
            const int m = st.os.upSize(frames);
            for (int k = 0; k < m; ++k) {
                const size_t b = static_cast<size_t>(k / kOs);
                const float v = preGainBuf_[b] * up[k];
                up[k] = detail::sanitize(mkBuf_[b] * adaaAsymClip(v, loBuf_[b], hiBuf_[b], st.clipState));
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i) {
                float s = st.dc.process(x[i]);   // strip the asymmetric clip's DC
                s = st.filt.process(s);
                x[i] = detail::sanitize(s * outGainBuf_[static_cast<size_t>(i)]);
            }
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the smoothers' CURRENT values, so a gate case can assert a
    // freshly prepared core starts AT its configured settings.
    float preGainCurrentForTest() const { return preGain_.current(); }
    float outGainCurrentForTest() const { return outGain_.current(); }

private:
    static constexpr int   kOs           = 8;     // hard clipper: 8x, as dcdsp_distortion.h
    static constexpr float kRampSeconds  = 0.010f;

    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.f;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    // First-order antiderivative-antialiased clip to [lo, hi] (the ADAA1 recipe
    // of dcdsp_primitives.h's adaaHardClip, generalised to an ASYMMETRIC pair
    // of thresholds so a "one silicon diode, one LED" leg can be modelled).
    // F(x) = x^2/2 inside, hi*x - hi^2/2 above, lo*x - lo^2/2 below; continuous
    // at both corners, so the divided difference is well behaved.
    static float adaaAsymClip(float x, float lo, float hi, float& state) {
        const double x1 = static_cast<double>(x);
        const double x0 = static_cast<double>(state);
        const double l = static_cast<double>(lo), h = static_cast<double>(hi);
        const double diff = x1 - x0;
        double y;
        if (std::fabs(diff) < 1e-5) {
            const double mdd = 0.5 * (x1 + x0);
            y = (mdd > h) ? h : ((mdd < l) ? l : mdd);
        } else {
            const auto F = [l, h](double v) {
                if (v > h) return h * v - 0.5 * h * h;
                if (v < l) return l * v - 0.5 * l * l;
                return 0.5 * v * v;
            };
            y = (F(x1) - F(x0)) / diff;
        }
        state = x;
        return static_cast<float>(y);
    }

    struct Channel {
        detail::OversamplerNx os;
        Biquad  preLo, preHi;
        DcBlocker dc;
        OnePole filt;                 // the passive RC Filter knob
        float   clipState = 0.f;      // ADAA state
    };

    // Diode table. `makeup` normalises the three legs to roughly equal loudness
    // so the footswitch is a TONE change, not a level jump.
    struct Clip { float lo, hi, makeup; };
    static const int kNumClips = 3;
    static constexpr Clip kClips[kNumClips] = {
        { -0.75f, 0.75f, 1.30f },   // 0 silicon — the stock pair, tight and symmetric
        { -1.80f, 1.80f, 0.62f },   // 1 LED     — more headroom, louder and more open
        { -0.45f, 0.95f, 1.45f },   // 2 turbo   — one leg lifted: asymmetric, spitty
    };

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    RatParams params_{};
    SmoothedValue preGain_, outGain_, loThr_, hiThr_, makeup_;
    std::vector<float> preGainBuf_, outGainBuf_, loBuf_, hiBuf_, mkBuf_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
