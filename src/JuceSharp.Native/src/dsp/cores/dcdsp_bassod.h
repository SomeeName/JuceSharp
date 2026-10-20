// dcdsp_bassod.h — bass overdrive with a parallel clean blend.
//
// Models: the "drive plus clean" topology every usable bass dirt box uses —
// the distorted leg is allowed to lose low end (that is what stops a driven
// bass turning to mud), and the fundamental is put back by an untouched clean
// leg summed alongside it, with a two-band shelving tone stack after the sum.
// Topology: dry split (delay-compensated) | bounded pre-clip bass shelf ->
// [4x oversampled: drive -> biased ADAA tanh] -> DC block; sum by `blend` ->
// 100 Hz bass shelf -> 3 kHz treble shelf -> level.
//
// Easy to get wrong: the clean leg MUST be delayed by the wet leg's
// oversampler group delay. Undelayed, the blend is a comb filter whose first
// notch sits in the bass guitar's own range, so the knob that exists to
// RESTORE the fundamental cancels it instead — see dcdsp_overdrive.h, whose
// transparent model carries the same compensation.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct BassOdParams {
    float drive  = 0.4f;  // 0..1      into the clipper, 0 dB .. +32 dB
    float blend  = 0.5f;  // 0..1      0 = the clean leg alone, 1 = the driven leg alone
    float bass   = 0.f;   // -12..+12  low shelf at 100 Hz
    float treble = 0.f;   // -12..+12  high shelf at 3 kHz
    float level  = 0.6f;  // 0..1      output level
};

class BassOdCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOs);
            dryDelaySamples_ = c.os.groupDelayBaseSamples() + 0.5 / kOs;
            c.dryDelay.prepare(static_cast<int>(std::ceil(dryDelaySamples_)) + 4);
            c.dc.prepare(sr_, 12.0);   // below a 5-string's low B, rate-derived
            c.dc.reset();
            c.preLo.reset(); c.bass.reset(); c.treble.reset();
            c.sat = 0.f;
        }
        driveBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        blendBuf_.assign(static_cast<size_t>(maxBlock_), 0.5f);
        outGainBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        dryBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);

        drive_.reset(sr_, kRampSeconds);
        blend_.reset(sr_, kRampSeconds);
        outGain_.reset(sr_, kRampSeconds);
        setParams(params_);
        drive_.snapToTarget();
        blend_.snapToTarget();
        primeBuf_.assign(ch_.size() * static_cast<size_t>(maxBlock_), 0.f);
        primePtrs_.resize(ch_.size());
        for (size_t c = 0; c < ch_.size(); ++c)
            primePtrs_[c] = primeBuf_.data() + c * static_cast<size_t>(maxBlock_);
        outGain_.snapToTarget();
        primeOnSilence();
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset(); c.dryDelay.reset(); c.dc.reset();
            c.preLo.reset(); c.bass.reset(); c.treble.reset();
            c.sat = 0.f;
        }
    primeOnSilence();
    }

    void setParams(const BassOdParams& p) {
        params_ = p;
        drive_.setTarget(std::pow(10.f, clamp01(p.drive) * 32.f / 20.f));
        blend_.setTarget(clamp01(p.blend));
        outGain_.setTarget(clamp01(p.level) * 1.4f);
        const double bassDb   = clampShelf(p.bass);
        const double trebleDb = clampShelf(p.treble);
        for (auto& c : ch_) {
            // BOUNDED pre-clip bass shelf (-7 dB floor), never a high-pass: the
            // driven leg is meant to thin out, not to disappear, and the clean
            // leg is what carries the fundamental anyway.
            c.preLo.setLowShelf(sr_, 160.0, 0.707, -7.0);
            // Tone stack: two SHELVES, so neither can run away with cascading.
            c.bass.setLowShelf(sr_, 100.0, 0.707, bassDb);
            c.treble.setHighShelf(sr_, 3000.0, 0.707, trebleDb);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // skip, never half-process
        const int nch = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        for (int i = 0; i < frames; ++i) {
            driveBuf_[static_cast<size_t>(i)]   = drive_.next();
            blendBuf_[static_cast<size_t>(i)]   = blend_.next();
            outGainBuf_[static_cast<size_t>(i)] = outGain_.next();
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
                x[i] = detail::sanitize(st.preLo.process(s));
            }

            st.os.upsample(x, frames);
            float* up = st.os.data();
            const int m = st.os.upSize(frames);
            for (int k = 0; k < m; ++k) {
                const float g = driveBuf_[static_cast<size_t>(k / kOs)];
                up[k] = detail::sanitize(adaaTanh(g * up[k] + kBias, st.sat));
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                float wet = st.dc.process(x[i]);       // strip the bias DC
                const float bl = blendBuf_[b];
                float s = bl * wet + (1.f - bl) * dryBuf_[b];
                s = st.bass.process(s);
                s = st.treble.process(s);
                x[i] = detail::sanitize(s * outGainBuf_[b]);
            }
        }
    }

    // Constant, not blend-dependent: both legs are always time-aligned, so a
    // continuous knob never churns the chain's PDC.
    int latencySamples() const { return static_cast<int>(std::lround(dryDelaySamples_)); }

    float blendCurrentForTest()   const { return blend_.current(); }
    float outGainCurrentForTest() const { return outGain_.current(); }

private:
    static constexpr int   kOs          = 4;       // soft clipper: 4x
    static constexpr float kRampSeconds = 0.010f;
    static constexpr float kBias        = 0.08f;   // asymmetry -> even harmonics
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
    static double clampShelf(float v) {
        if (!(v > -12.f)) return (v < 0.f) ? -12.0 : 0.0;   // NaN -> 0 dB
        return v > 12.f ? 12.0 : static_cast<double>(v);
    }

    struct Channel {
        detail::OversamplerNx os;
        InterpolatedDelayLine dryDelay;
        Biquad    preLo, bass, treble;
        DcBlocker dc;
        float     sat = 0.f;
    };

    double sr_ = 48000.0;
    double dryDelaySamples_ = 0.0;
    int maxBlock_ = 512;
    BassOdParams params_{};
    SmoothedValue drive_, blend_, outGain_;
    std::vector<float> driveBuf_, blendBuf_, outGainBuf_, dryBuf_;
    /// Scratch for primeOnSilence(); sized in prepare(), never on the callback.
    std::vector<float>  primeBuf_;
    std::vector<float*> primePtrs_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
