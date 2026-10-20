// dcdsp_hm2.h — "Chainsaw" metal distortion (BOSS HM-2 family).
//
// Models: two cascaded op-amp clipping stages followed by the pedal's famous
// "colour mix" pair — two RESONANT boosts, one at ~80 Hz and one at ~1 kHz,
// each worth up to +12 dB — with a fixed scooped mid between them. All knobs
// full is the buzzsaw.
// Topology: bounded input bass shelf -> [8x oversampled: dist gain ->
// asymmetric ADAA tanh -> second tanh -> ADAA hard clip] -> DC block ->
// low colour peak -> mid scoop -> high colour peak -> level.
//
// Easy to get wrong: the colour boosts belong AFTER the clippers. Put them in
// front and the clipper simply compresses them away, so the knobs measure a
// couple of dB at their own centre frequencies instead of the ten they are
// labelled with — the pedal then has no chainsaw in it at all.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct Hm2Params {
    float dist   = 0.7f;  // 0..1   drive into the clipper cascade, 0 dB .. +46 dB
    float lowDb  = 10.f;  // 0..12  resonant colour boost at 80 Hz
    float highDb = 10.f;  // 0..12  resonant colour boost at 1 kHz
    float level  = 0.5f;  // 0..1   output level
};

class Hm2Core {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOs);
            c.dc.prepare(sr_, 14.0);   // below the 80 Hz colour peak, rate-derived
            c.dc.reset();
            c.inLo.reset(); c.low.reset(); c.scoop.reset(); c.high.reset();
            c.st1 = c.st2 = c.st3 = 0.f;
        }
        preGainBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        outGainBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        preGain_.reset(sr_, kRampSeconds);
        outGain_.reset(sr_, kRampSeconds);
        primeBuf_.assign(ch_.size() * static_cast<size_t>(maxBlock_), 0.f);
        primePtrs_.resize(ch_.size());
        for (size_t c = 0; c < ch_.size(); ++c)
            primePtrs_[c] = primeBuf_.data() + c * static_cast<size_t>(maxBlock_);

        setParams(params_);
        preGain_.snapToTarget();
        outGain_.snapToTarget();
        primeOnSilence();
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset(); c.dc.reset();
            c.inLo.reset(); c.low.reset(); c.scoop.reset(); c.high.reset();
            c.st1 = c.st2 = c.st3 = 0.f;
        }
        primeOnSilence();
    }

    void setParams(const Hm2Params& p) {
        params_ = p;
        const float dist  = clamp01(p.dist);
        const float level = clamp01(p.level);
        const double lowDb  = clampColour(p.lowDb);
        const double highDb = clampColour(p.highDb);
        preGain_.setTarget(std::pow(10.f, dist * 46.f / 20.f));
        outGain_.setTarget(level * 0.6f);
        for (auto& c : ch_) {
            // BOUNDED input shelf, never a high-pass — the HM-2 is not a thin
            // pedal, it is a scooped one, and the scoop lives post-clip below.
            c.inLo.setLowShelf(sr_, 150.0, 0.707, -4.0);
            // The two colour-mix boosts, resonant (Q > 0.707) so each one is a
            // band and not a shelf, and EXACT at its own centre frequency.
            c.low.setPeaking(sr_, kLowHz, 1.2, lowDb);
            c.scoop.setPeaking(sr_, 450.0, 0.8, -9.0);
            c.high.setPeaking(sr_, kHighHz, 1.4, highDb);
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
                float s = preGainBuf_[static_cast<size_t>(k / kOs)] * up[k];
                s = adaaTanh(s + kBias, st.st1);       // stage 1: asymmetric soft
                s = adaaTanh(2.6f * s, st.st2);        // stage 2: squared up
                s = adaaHardClip(2.2f * s, st.st3);    // stage 3: the buzzsaw edge
                up[k] = detail::sanitize(s);
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i) {
                float s = st.dc.process(x[i]);   // strip the stage-1 asymmetry
                s = st.low.process(s);
                s = st.scoop.process(s);
                s = st.high.process(s);
                x[i] = detail::sanitize(s * outGainBuf_[static_cast<size_t>(i)]);
            }
        }
    }

    int latencySamples() const { return 0; }

    float preGainCurrentForTest() const { return preGain_.current(); }
    float outGainCurrentForTest() const { return outGain_.current(); }

    static constexpr double kLowHz  = 80.0;
    static constexpr double kHighHz = 1000.0;

private:
    static constexpr int   kOs          = 8;
    static constexpr float kRampSeconds = 0.010f;
    static constexpr float kBias        = 0.09f;
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
    // NaN lands on the first branch, exactly like clampCutoff's negated form.
    static double clampColour(float v) {
        if (!(v > 0.f)) return 0.0;
        return v > 12.f ? 12.0 : static_cast<double>(v);
    }

    struct Channel {
        detail::OversamplerNx os;
        Biquad    inLo, low, scoop, high;
        DcBlocker dc;
        float     st1 = 0.f, st2 = 0.f, st3 = 0.f;
    };

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    Hm2Params params_{};
    SmoothedValue preGain_, outGain_;
    std::vector<float> preGainBuf_, outGainBuf_;
    /// Scratch for primeOnSilence(); sized in prepare(), never on the callback.
    std::vector<float>  primeBuf_;
    std::vector<float*> primePtrs_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
