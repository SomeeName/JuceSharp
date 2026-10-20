// dcdsp_octfuzz.h — octave-up fuzz (Tycobrahe Octavia / Foxx Tone Machine family).
//
// Models: a driven germanium fuzz stage whose output is fed to a FULL-WAVE
// RECTIFIER — the transformer trick that folds the negative half of the wave
// onto the positive one. A full-wave rectified sine contains only EVEN
// harmonics, so the fundamental disappears and the first octave up becomes the
// loudest thing in the signal; the `octave` knob crossfades between the plain
// fuzz and that rectified leg.
// Topology: bounded input shelf -> [8x oversampled: fuzz gain -> biased ADAA
// tanh | ADAA full-wave rectifier -> shaping tanh; crossfade by `octave`] ->
// DC block -> bounded tone shelf -> post roll-off -> volume.
//
// Easy to get wrong: |x| has a corner at zero, which is a far worse aliaser
// than any smooth clipper — so the rectifier is ADAA1'd (the antiderivative of
// |x| is x|x|/2) as well as oversampled 8x. And it must be followed by a DC
// blocker: a rectified signal is entirely positive, so without one the whole
// pedal rides on a large offset.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct OctFuzzParams {
    float fuzz   = 0.7f; // 0..1  input drive, 0 dB .. +40 dB
    float octave = 0.6f; // 0..1  crossfade into the rectified (octave-up) leg
    float tone   = 0.5f; // 0..1  bounded +/-10 dB shelf at 1.2 kHz
    float volume = 0.5f; // 0..1  output level
};

class OctFuzzCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOs);
            c.dc.prepare(sr_, 16.0);
            c.dc.reset();
            c.inLo.reset(); c.shelf.reset(); c.post.reset();
            c.stFuzz = c.stAbs = c.stOct = 0.f;
        }
        fuzzBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        octBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        outGainBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);

        fuzzGain_.reset(sr_, kRampSeconds);
        octave_.reset(sr_, kRampSeconds);
        outGain_.reset(sr_, kRampSeconds);
        setParams(params_);
        fuzzGain_.snapToTarget();
        octave_.snapToTarget();
        primeBuf_.assign(ch_.size() * static_cast<size_t>(maxBlock_), 0.f);
        primePtrs_.resize(ch_.size());
        for (size_t c = 0; c < ch_.size(); ++c)
            primePtrs_[c] = primeBuf_.data() + c * static_cast<size_t>(maxBlock_);
        outGain_.snapToTarget();
        primeOnSilence();
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset(); c.dc.reset();
            c.inLo.reset(); c.shelf.reset(); c.post.reset();
            c.stFuzz = c.stAbs = c.stOct = 0.f;
        }
    primeOnSilence();
    }

    void setParams(const OctFuzzParams& p) {
        params_ = p;
        fuzzGain_.setTarget(std::pow(10.f, clamp01(p.fuzz) * 40.f / 20.f));
        octave_.setTarget(clamp01(p.octave));
        outGain_.setTarget(clamp01(p.volume) * 0.9f);
        const double toneDb = (static_cast<double>(clamp01(p.tone)) - 0.5) * 2.0 * 10.0;
        for (auto& c : ch_) {
            // BOUNDED input shelf, never a high-pass.
            c.inLo.setLowShelf(sr_, 180.0, 0.707, -5.0);
            // BOUNDED tone shelf: +/-10 dB, not a sweeping filter.
            c.shelf.setHighShelf(sr_, 1200.0, 0.707, toneDb);
            c.post.setLowpass(sr_, 6000.0, 0.707);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // skip, never half-process
        const int nch = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        for (int i = 0; i < frames; ++i) {
            const size_t b = static_cast<size_t>(i);
            fuzzBuf_[b]    = fuzzGain_.next();
            octBuf_[b]     = octave_.next();
            outGainBuf_[b] = outGain_.next();
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
                const float driven = fuzzBuf_[b] * up[k];
                const float fuzzLeg = adaaTanh(driven + kBias, st.stFuzz);
                // Full-wave rectifier, band-limited: ADAA1 over F(x) = x|x|/2.
                const float rect = adaaAbs(driven, st.stAbs);
                const float octLeg = kOctMakeup * adaaTanh(kOctDrive * rect, st.stOct);
                const float o = octBuf_[b];
                up[k] = detail::sanitize((1.f - o) * fuzzLeg + o * octLeg);
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                float s = st.dc.process(x[i]);   // the rectified leg's offset leaves here
                s = st.shelf.process(s);
                s = st.post.process(s);
                x[i] = detail::sanitize(s * outGainBuf_[b]);
            }
        }
    }

    int latencySamples() const { return 0; }

    float octaveCurrentForTest()  const { return octave_.current(); }
    float outGainCurrentForTest() const { return outGain_.current(); }

private:
    static constexpr int   kOs          = 8;      // a rectifier corner needs 8x
    static constexpr float kRampSeconds = 0.010f;
    static constexpr float kBias        = 0.06f;
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
    static constexpr float kOctDrive    = 2.0f;
    static constexpr float kOctMakeup   = 1.4f;   // match the two legs' loudness

    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.f;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    // First-order antiderivative-antialiased |x| (same ADAA1 recipe as
    // adaaHardClip). F(x) = x*|x|/2 is the closed-form antiderivative of |x|
    // and is C1 at the origin, so the divided difference behaves.
    static float adaaAbs(float x, float& state) {
        const double x1 = static_cast<double>(x), x0 = static_cast<double>(state);
        const double diff = x1 - x0;
        double y;
        if (std::fabs(diff) < 1e-5) {
            y = std::fabs(0.5 * (x1 + x0));
        } else {
            const auto F = [](double v) { return 0.5 * v * std::fabs(v); };
            y = (F(x1) - F(x0)) / diff;
        }
        state = x;
        return static_cast<float>(y);
    }

    struct Channel {
        detail::OversamplerNx os;
        Biquad    inLo, shelf, post;
        DcBlocker dc;
        float     stFuzz = 0.f, stAbs = 0.f, stOct = 0.f;
    };

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    OctFuzzParams params_{};
    SmoothedValue fuzzGain_, octave_, outGain_;
    std::vector<float> fuzzBuf_, octBuf_, outGainBuf_;
    /// Scratch for primeOnSilence(); sized in prepare(), never on the callback.
    std::vector<float>  primeBuf_;
    std::vector<float*> primePtrs_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
