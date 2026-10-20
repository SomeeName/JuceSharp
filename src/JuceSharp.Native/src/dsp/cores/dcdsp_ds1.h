// dcdsp_ds1.h — "Orange" transistor-plus-op-amp distortion (BOSS DS-1 family).
//
// Models: a discrete transistor booster feeding an op-amp gain stage with a
// pair of asymmetric silicon diodes in its FEEDBACK leg (soft-knee clipping,
// not a shunt hard clip), then the pedal's passive TILT tone stack.
// Topology: bounded input bass shelf -> fixed booster (+6 dB, asymmetric tanh)
// -> [8x oversampled: dist gain -> asymmetric ADAA soft/hard cascade] ->
// DC block -> tilt tone -> level.
//
// Easy to get wrong: the DS-1's tone control is NOT one filter. It is a passive
// TILT — a low-pass leg and its complementary high-pass leg, summed in
// opposite proportions around ~700 Hz — so at noon it is FLAT, fully
// anticlockwise it is a low-pass and fully clockwise a high-pass. Modelling it
// as a single sweeping low-pass loses the top half of the knob's range and the
// flat centre detent.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct Ds1Params {
    float dist  = 0.5f; // 0..1  op-amp drive, 0 dB .. +42 dB
    float tone  = 0.5f; // 0..1  passive tilt around 700 Hz; 0.5 = flat
    float level = 0.6f; // 0..1  output level
};

class Ds1Core {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOs);
            c.dc.prepare(sr_, 18.0);     // rate-derived, not a fixed coefficient
            c.dc.reset();
            c.inLo.reset(); c.tiltLp.reset();
            c.st1 = c.st2 = 0.f;
        }
        preGainBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        outGainBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        toneBuf_.assign(static_cast<size_t>(maxBlock_), 0.5f);

        preGain_.reset(sr_, kRampSeconds);
        outGain_.reset(sr_, kRampSeconds);
        tone_.reset(sr_, kRampSeconds);
        primeBuf_.assign(ch_.size() * static_cast<size_t>(maxBlock_), 0.f);
        primePtrs_.resize(ch_.size());
        for (size_t c = 0; c < ch_.size(); ++c)
            primePtrs_[c] = primeBuf_.data() + c * static_cast<size_t>(maxBlock_);

        setParams(params_);
        preGain_.snapToTarget();
        outGain_.snapToTarget();
        tone_.snapToTarget();
        primeOnSilence();
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset(); c.dc.reset();
            c.inLo.reset(); c.tiltLp.reset();
            c.st1 = c.st2 = 0.f;
        }
        primeOnSilence();
    }

    void setParams(const Ds1Params& p) {
        params_ = p;
        const float dist  = clamp01(p.dist);
        const float tone  = clamp01(p.tone);
        const float level = clamp01(p.level);
        preGain_.setTarget(std::pow(10.f, dist * 42.f / 20.f));
        outGain_.setTarget(level * 0.9f);
        tone_.setTarget(tone);
        for (auto& c : ch_) {
            // BOUNDED input bass shelf (-5 dB floor), never a high-pass: the
            // DS-1 is mid-forward but it does not lose the low E. Depth is
            // fixed, so cascading it with an amp's own network stays finite.
            c.inLo.setLowShelf(sr_, 220.0, 0.707, -5.0);
            // The tilt's split point. The high leg is the COMPLEMENT of this
            // low-pass (x - lp), so the two sum to exactly unity at tone 0.5.
            c.tiltLp.setLowpass(sr_, kTiltHz);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // skip, never half-process
        const int nch = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        for (int i = 0; i < frames; ++i) {
            preGainBuf_[static_cast<size_t>(i)] = preGain_.next();
            outGainBuf_[static_cast<size_t>(i)] = outGain_.next();
            toneBuf_[static_cast<size_t>(i)]    = tone_.next();
        }

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (x == nullptr) continue;
            Channel& st = ch_[static_cast<size_t>(c)];

            for (int i = 0; i < frames; ++i)
                x[i] = detail::sanitize(st.inLo.process(detail::sanitize(x[i])));

            st.os.upsample(x, frames);
            float* up = st.os.data();
            const int m = st.os.upSize(frames);
            for (int k = 0; k < m; ++k) {
                const size_t b = static_cast<size_t>(k / kOs);
                // Transistor booster: a fixed, gently asymmetric stage that
                // runs even at dist 0 — this is what gives the pedal its
                // second-harmonic colour before the op-amp does anything.
                float s = adaaTanh(kBoost * up[k] + kBoostBias, st.st1);
                // Op-amp stage with asymmetric feedback diodes.
                s = adaaAsymClip(preGainBuf_[b] * s, kClipLo, kClipHi, st.st2);
                up[k] = detail::sanitize(s);
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i) {
                float s = st.dc.process(x[i]);
                // Passive tilt: 2*(1-t) of the low leg + 2*t of its complement.
                // t = 0.5 sums to exactly the input, which is the flat detent.
                const float t   = toneBuf_[static_cast<size_t>(i)];
                const float low = st.tiltLp.process(s);
                s = 2.f * (1.f - t) * low + 2.f * t * (s - low);
                x[i] = detail::sanitize(s * outGainBuf_[static_cast<size_t>(i)]);
            }
        }
    }

    int latencySamples() const { return 0; }

    float preGainCurrentForTest() const { return preGain_.current(); }
    float outGainCurrentForTest() const { return outGain_.current(); }

private:
    static constexpr int   kOs          = 8;      // hard-clipping cascade: 8x
    static constexpr float kRampSeconds = 0.010f;
    static constexpr double kTiltHz     = 700.0;  // the tone stack's split point
    static constexpr float kBoost       = 2.0f;   // discrete booster, ~+6 dB
    static constexpr float kBoostBias   = 0.07f;  // its operating-point offset
    static constexpr float kClipLo      = -0.55f; // asymmetric feedback diodes
    static constexpr float kClipHi      = 0.90f;
    /// ~25 time constants of the DC blocker's corner: long enough that the
    /// primed state IS the steady state to well below any audible floor.
    static constexpr double kPrimeSeconds = 0.25;

    /// Leave every stage in the state SILENCE really produces.
    ///
    /// This pedal's first gain stage carries a fixed operating-point OFFSET
    /// (the bias constant above), so with nothing playing it emits constant DC
    /// into the blocker that follows it. A `DcBlocker` reset to zero state
    /// answers a DC step with a FULL-AMPLITUDE decaying pulse - y[0] = x[0],
    /// decaying with the corner's ~9 ms time constant - so every prepare(),
    /// every device change and every un-bypass thumped, out of digital silence,
    /// with nothing on screen to explain it. Measured by the consumer's own
    /// gate at -28.0 dBFS (ds1) and -33.9 dBFS (hm2) against a -35 dBFS floor.
    ///
    /// Running the module's OWN process() over a quarter second of silence is
    /// what primes it, rather than a hand-derived DC constant: the value the
    /// blocker must start on is the output of an 8x oversampled ADAA cascade
    /// behind two filters, and a constant copied out of that arithmetic would
    /// be a second source of truth the next voicing change silently falsifies.
    /// Message thread only (prepare / reset), and it allocates nothing - the
    /// scratch is sized in prepare().
    void primeOnSilence() {
        if (ch_.empty() || maxBlock_ <= 0 || primePtrs_.size() != ch_.size()) return;
        const int nch   = static_cast<int>(ch_.size());
        const int total = static_cast<int>(sr_ * kPrimeSeconds);
        for (int done = 0; done < total; done += maxBlock_) {
            const int n = (maxBlock_ < total - done) ? maxBlock_ : (total - done);
            std::fill(primeBuf_.begin(), primeBuf_.end(), 0.f);
            process(primePtrs_.data(), nch, n);
        }
    }

    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.f;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    // ADAA1 clip to an ASYMMETRIC [lo, hi]; see dcdsp_rat.h for the derivation.
    static float adaaAsymClip(float x, float lo, float hi, float& state) {
        const double x1 = static_cast<double>(x), x0 = static_cast<double>(state);
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
        Biquad    inLo;
        DcBlocker dc;
        OnePole   tiltLp;
        float     st1 = 0.f, st2 = 0.f;   // ADAA states
    };

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    Ds1Params params_{};
    SmoothedValue preGain_, outGain_, tone_;
    std::vector<float> preGainBuf_, outGainBuf_, toneBuf_;
    /// Scratch for primeOnSilence(); sized in prepare(), never on the callback.
    std::vector<float>  primeBuf_;
    std::vector<float*> primePtrs_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
