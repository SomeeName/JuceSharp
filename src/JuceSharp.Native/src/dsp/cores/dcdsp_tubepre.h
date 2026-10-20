// dcdsp_tubepre.h — two-stage valve preamp / DI.
//
// Models: a pair of asymmetric triode gain stages with a coupling network
// between them, a bright cap across the gain pot, a passive tilt tone stack,
// a SUPPLY-SAG model (the B+ rail droops under a sustained load, which lowers
// the second stage's clipping threshold and makes a held note bloom then
// compress), and a switchable speaker-emulating DI curve.
// Topology: bounded input shelf -> bright blend -> [4x oversampled: gain ->
// biased ADAA tanh -> coupling shelf (double-precision, at the OS rate) ->
// sag-scaled asymmetric ADAA clip] -> DC block -> tilt tone -> cab blend ->
// level.
//
// Easy to get wrong: `bright` and `cabSim` are SWITCHES, and switching a
// biquad in and out re-injects whatever its state held when it was last used.
// Both filters therefore run ALL THE TIME and the switch is a 10 ms crossfade
// between their output and the unfiltered signal — the same shape
// dcdsp_overdrive.h gives its model footswitch.
//
// The second trap, and the one this core shipped with: `sag` is not a knob you
// can step. It scales BOTH of the second stage's clip thresholds per sample, so
// moving it with the follower charged splices the waveform exactly the way
// dcdsp_rat.h's `clip` footswitch would — 0 -> 1 at env 0.5 moves both
// thresholds by a factor 0.4 between two samples. It ramps like every other
// per-sample multiplier here.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct TubePreParams {
    float gain   = 0.4f; // 0..1  first-stage drive, 0 dB .. +36 dB
    float sag    = 0.3f; // 0..1  supply-sag depth
    int   bright = 0;    // 0 off / 1 on — the bright cap across the gain pot
    float tone   = 0.5f; // 0..1  passive tilt around 800 Hz; 0.5 = flat
    float level  = 0.6f; // 0..1  output level
    int   cabSim = 0;    // 0 off / 1 on — the DI speaker-emulation curve
};

class TubePreCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOs);
            c.dc.prepare(sr_, 12.0);
            c.dc.reset();
            c.sagEnv.prepare(sr_);
            c.sagEnv.setTimes(kSagAttackMs, kSagReleaseMs);
            c.inLo.reset(); c.brightSh.reset(); c.tiltLp.reset();
            c.couple.reset();
            c.cabLp1.reset(); c.cabLp2.reset(); c.cabTop.reset();
            c.cabLo1.reset(); c.cabLo2.reset();
            c.st1 = c.st2 = 0.f;
        }
        gainBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        outGainBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        toneBuf_.assign(static_cast<size_t>(maxBlock_), 0.5f);
        brightBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        cabBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        sagBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        sagKBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);

        gain_.reset(sr_, kRampSeconds);
        outGain_.reset(sr_, kRampSeconds);
        tone_.reset(sr_, kRampSeconds);
        bright_.reset(sr_, kRampSeconds);
        cab_.reset(sr_, kRampSeconds);
        sagK_.reset(sr_, kRampSeconds);
        setParams(params_);
        gain_.snapToTarget();
        outGain_.snapToTarget();
        tone_.snapToTarget();
        bright_.snapToTarget();
        cab_.snapToTarget();
        primeBuf_.assign(ch_.size() * static_cast<size_t>(maxBlock_), 0.f);
        primePtrs_.resize(ch_.size());
        for (size_t c = 0; c < ch_.size(); ++c)
            primePtrs_[c] = primeBuf_.data() + c * static_cast<size_t>(maxBlock_);
        sagK_.snapToTarget();
        primeOnSilence();
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset(); c.dc.reset(); c.sagEnv.reset();
            c.inLo.reset(); c.brightSh.reset(); c.tiltLp.reset();
            c.couple.reset();
            c.cabLp1.reset(); c.cabLp2.reset(); c.cabTop.reset();
            c.cabLo1.reset(); c.cabLo2.reset();
            c.st1 = c.st2 = 0.f;
        }
    primeOnSilence();
    }

    void setParams(const TubePreParams& p) {
        params_ = p;
        gain_.setTarget(std::pow(10.f, clamp01(p.gain) * 36.f / 20.f));
        outGain_.setTarget(clamp01(p.level) * 1.1f);
        tone_.setTarget(clamp01(p.tone));
        bright_.setTarget(p.bright > 0 ? 1.f : 0.f);
        cab_.setTarget(p.cabSim > 0 ? 1.f : 0.f);
        sagK_.setTarget(clamp01(p.sag) * 3.f);
        const double osRate = sr_ * kOs;
        for (auto& c : ch_) {
            // BOUNDED input shelf, never a high-pass.
            c.inLo.setLowShelf(sr_, 120.0, 0.707, -4.0);
            c.brightSh.setHighShelf(sr_, 2000.0, 0.707, 7.0);
            // Inter-stage coupling network, run at the OVERSAMPLED rate: a
            // low-frequency biquad there sits with its poles at z ~= 1, which
            // is exactly where float coefficients lose precision, so this one
            // is the double-precision BiquadD the primitives header provides.
            c.couple.setLowShelf(osRate, 200.0, 0.707, -4.0);
            c.tiltLp.setLowpass(sr_, kTiltHz);
            // The DI cab curve: a 4th-order roll-off, the beam-blocker dip a
            // real 12" has above it, and a BOUNDED (-18 dB, two shelves) low
            // end — a cab genuinely has no output under ~80 Hz, but spelling
            // that as an unbounded high-pass is how a rig ends up thin once it
            // is cascaded with everything else's bass network.
            c.cabLp1.setLowpass(sr_, 5000.0, 0.707);
            c.cabLp2.setLowpass(sr_, 5000.0, 0.707);
            c.cabTop.setHighShelf(sr_, 7000.0, 0.707, -12.0);
            c.cabLo1.setLowShelf(sr_, 80.0, 0.707, -9.0);
            c.cabLo2.setLowShelf(sr_, 80.0, 0.707, -9.0);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // skip, never half-process
        const int nch = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        for (int i = 0; i < frames; ++i) {
            const size_t b = static_cast<size_t>(i);
            gainBuf_[b]    = gain_.next();
            outGainBuf_[b] = outGain_.next();
            toneBuf_[b]    = tone_.next();
            brightBuf_[b]  = bright_.next();
            cabBuf_[b]     = cab_.next();
            sagKBuf_[b]    = sagK_.next();
        }

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (x == nullptr) continue;
            Channel& st = ch_[static_cast<size_t>(c)];

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                float s = detail::sanitize(x[i]);
                s = st.inLo.process(s);
                const float br = brightBuf_[b];
                const float lift = st.brightSh.process(s);   // always running
                s = (1.f - br) * s + br * lift;
                // Supply sag: a SLOW follower on how hard the first stage is
                // being driven. 1 / (1 + k*env) is the rail droop, and it
                // scales the SECOND stage's clipping threshold below.
                const float env = st.sagEnv.process(std::min(std::fabs(s) * gainBuf_[b], 4.f));
                const float k = sagKBuf_[b];
                sagBuf_[b] = (k > 0.f) ? (1.f / (1.f + k * env)) : 1.f;
                x[i] = detail::sanitize(s);
            }

            st.os.upsample(x, frames);
            float* up = st.os.data();
            const int m = st.os.upSize(frames);
            for (int k = 0; k < m; ++k) {
                const size_t b = static_cast<size_t>(k / kOs);
                float s = adaaTanh(gainBuf_[b] * up[k] + kBias1, st.st1);
                s = st.couple.process(s);
                const float droop = sagBuf_[b];
                s = adaaAsymClip(kStage2Drive * s, kStage2Lo * droop, kStage2Hi * droop, st.st2);
                up[k] = detail::sanitize(s);
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                float s = st.dc.process(x[i]);
                // Passive tilt: a low-pass leg and its exact complement.
                const float t   = toneBuf_[b];
                const float low = st.tiltLp.process(s);
                s = 2.f * (1.f - t) * low + 2.f * t * (s - low);
                // The cab curve runs unconditionally; the switch crossfades it.
                float cab = st.cabLp2.process(st.cabLp1.process(s));
                cab = st.cabTop.process(cab);
                cab = st.cabLo2.process(st.cabLo1.process(cab));
                const float cm = cabBuf_[b];
                s = (1.f - cm) * s + cm * cab;
                x[i] = detail::sanitize(s * outGainBuf_[b]);
            }
        }
    }

    int latencySamples() const { return 0; }

    float gainCurrentForTest()    const { return gain_.current(); }
    float outGainCurrentForTest() const { return outGain_.current(); }

private:
    static constexpr int    kOs            = 4;
    static constexpr float  kRampSeconds   = 0.010f;
    static constexpr double kTiltHz        = 800.0;
    static constexpr float  kBias1         = 0.07f;
    static constexpr float  kStage2Drive   = 1.6f;
    static constexpr float  kStage2Lo      = -0.60f;
    static constexpr float  kStage2Hi      = 0.90f;
    static constexpr float  kSagAttackMs   = 250.f;  // the rail droops slowly...
    static constexpr float  kSagReleaseMs  = 400.f;  // ...and recovers slowly
    /// Long enough that the primed state IS the steady state, to well below
    /// any audible floor, at every DC-blocker corner this drawer uses.
    static constexpr double kPrimeSeconds = 0.25;

    /// Leave every stage in the state SILENCE really produces.
    ///
    /// This pedal's gain stage carries a fixed operating-point OFFSET (the bias
    /// constant above), so with nothing playing it emits constant DC into the
    /// blocker that follows it. A `DcBlocker` reset to zero state answers a DC
    /// step with a FULL-AMPLITUDE decaying pulse - y[0] = x[0], decaying with
    /// the corner's time constant - so every prepare(), every device change and
    /// every un-bypass thumped, out of digital silence, with nothing on screen
    /// to explain it. Five of the seven cores in this drawer shipped it;
    /// measured peaks over the first 500 ms were -21.2 dBFS (tubepre),
    /// -27.8 (bassod) and -38.8 (octfuzz), and the consumer's own gate read
    /// -28.0 dBFS RMS (ds1) and -33.9 (hm2).
    ///
    /// Running the module's OWN process() over a quarter second of silence is
    /// what primes it, rather than a hand-derived DC constant: the value the
    /// blocker must start on is the output of an oversampled ADAA cascade
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
        Biquad          inLo, brightSh, cabLp1, cabLp2, cabTop, cabLo1, cabLo2;
        BiquadD         couple;          // runs at the oversampled rate
        OnePole         tiltLp;
        DcBlocker       dc;
        EnvelopeFollower sagEnv;
        float           st1 = 0.f, st2 = 0.f;
    };

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    TubePreParams params_{};
    SmoothedValue gain_, outGain_, tone_, bright_, cab_, sagK_;
    std::vector<float> gainBuf_, outGainBuf_, toneBuf_, brightBuf_, cabBuf_,
                       sagBuf_, sagKBuf_;
    /// Scratch for primeOnSilence(); sized in prepare(), never on the callback.
    std::vector<float>  primeBuf_;
    std::vector<float*> primePtrs_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
