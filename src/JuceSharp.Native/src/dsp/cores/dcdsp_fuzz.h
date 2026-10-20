// dcdsp_fuzz.h — FUZZ pedal DSP core (pure, header-only, RT-safe).
//
// Character: germanium Fuzz-Face-style transistor fuzz. Input coupling
// high-pass, a pickup-loading treble-cut shelf (the famous low-input-impedance
// interaction with a passive guitar), gated "sputter" at low input, an
// envelope-driven bias shift (dying-battery supply sag: the operating point
// collapses while a note rings and recovers on the decay), then a driven
// asymmetric soft->hard clip cascade run inside a REAL 8x OversamplerNx wrap
// with ADAA on both stages, a tilt tone control, and an output level with a
// guaranteed final hard clip (output is ALWAYS bounded).
//
// Aliasing: both clip stages are ADAA1 (tanh stage via the shared adaaTanh;
// the asymmetric hard stage has a closed-form antiderivative below) and run at
// 8x through the shared elliptic oversampler — the previous core used linear-
// interpolation "oversampling" (which band-limits nothing) and a memoryless
// shaper, making it the worst aliaser in the set. 8x (not the usual 4x)
// because the up-to-131x drive keeps meaningful harmonic energy beyond a 4x
// Nyquist even after ADAA (measured: ~6-14 dB better than 4x here).
//
// Deps: dcdsp_primitives.h (OnePole, EnvelopeFollower, DcBlocker,
// SmoothedValue, adaaTanh) + dcdsp_detail_os.h (OversamplerNx, sanitize).
//
// RT-safety: all allocation happens in prepare(); process() never allocates,
// locks, or throws. IIR oversampling => latencySamples() stays 0.
#pragma once

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

namespace dcdsp {

// All params normalized 0..1.
struct FuzzParams {
    float fuzz  = 0.6f; // 0..1  input gain / drive amount (more = thicker, more sputter)
    float tone  = 0.5f; // 0..1  tilt: 0 = dark (bassy), 0.5 = flat, 1 = bright
    float level = 0.7f; // 0..1  output level trim
};

class FuzzCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        nch_ = (numChannels > 0) ? numChannels : 1;

        ch_.assign(static_cast<size_t>(nch_), ChannelState{});
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOversample);
            c.inputHpf.setHighpass(sr_, 90.0);   // input coupling cap ~90 Hz
            c.loadLpf.setLowpass(sr_, 3200.0);   // pickup-loading shelf split
            c.toneLpf.setLowpass(sr_, 720.0);    // tilt split point
            c.env.prepare(sr_);
            c.env.setTimes(2.0f, 60.0f);         // gate follower
            c.sagEnv.prepare(sr_);
            c.sagEnv.setTimes(4.0f, 220.0f);     // battery-sag follower (slower recovery)
            c.dcBlock.prepare(sr_);
        }

        fuzzSm_.reset(sr_, 0.02f);
        toneSm_.reset(sr_, 0.02f);
        levelSm_.reset(sr_, 0.02f);
        fuzzSm_.setTarget(p_.fuzz);
        toneSm_.setTarget(p_.tone);
        levelSm_.setTarget(p_.level);
        // Start AT the configured values: reset() preserves mCurrent, which is 0
        // on a first prepare, so without this the first 20 ms fades up from zero
        // and an offline bounce is not deterministic from sample 0.
        fuzzSm_.snapToTarget();
        toneSm_.snapToTarget();
        levelSm_.snapToTarget();

        fzBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        tnBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        lvBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        biasBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        driveBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);

        reset();
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset();
            c.inputHpf.reset();
            c.loadLpf.reset();
            c.toneLpf.reset();
            c.env.reset();
            c.sagEnv.reset();
            c.dcBlock.reset();
            c.st1 = 0.f;
            c.st2 = 0.f;
        }
    }

    void setParams(const FuzzParams& p) {
        p_.fuzz  = clamp01(p.fuzz);
        p_.tone  = clamp01(p.tone);
        p_.level = clamp01(p.level);
        fuzzSm_.setTarget(p_.fuzz);
        toneSm_.setTarget(p_.tone);
        levelSm_.setTarget(p_.level);
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        // SKIP an oversized block rather than processing part of it: the tail
        // past maxBlock_ was left UNTOUCHED, i.e. full-level clean DI spliced
        // into the middle of a fuzz block.
        if (numFrames > maxBlock_) return;
        const int nc = std::min(numChannels, nch_);
        const int frames = std::min(numFrames, maxBlock_);

        // Smoothed params advance once per frame, shared across channels.
        for (int i = 0; i < frames; ++i) {
            fzBuf_[i] = fuzzSm_.next();
            tnBuf_[i] = toneSm_.next();
            lvBuf_[i] = levelSm_.next();
        }

        for (int ch = 0; ch < nc; ++ch) {
            float* buf = channels[ch];
            if (buf == nullptr) continue;
            ChannelState& c = ch_[static_cast<size_t>(ch)];

            // --- base-rate front end: coupling HPF, pickup-loading shelf,
            // gate; per-frame drive + bias for the oversampled stage ----------
            // The drive gain (up to 131x) is deliberately NOT applied here:
            // multiplying at base rate and clamping would hard-clip at the
            // base rate, aliasing before the oversampler can help. The
            // band-limited ~unity signal goes up; drive is applied per OS
            // sample inside the ADAA stage.
            for (int i = 0; i < frames; ++i) {
                const float fz = fzBuf_[i];
                float x = buf[i];
                if (!std::isfinite(x)) x = 0.f;

                x = c.inputHpf.process(x);

                // Pickup loading: the low input impedance shaves treble going
                // in (~-9 dB above ~3.2 kHz); the clip cascade regenerates it.
                const float lo = c.loadLpf.process(x);
                x = lo + kLoadBright * (x - lo);

                // Gated sputter: attenuate signal below the gate threshold.
                // Threshold rises with fuzz => more cutout on decays.
                const float gateThresh = 0.008f + fz * 0.05f;
                const float e = c.env.process(x);
                float gate = 1.0f;
                if (e < gateThresh) {
                    const float r = (gateThresh > 1e-9f) ? (e / gateThresh) : 0.f;
                    gate = r * r; // quadratic gate => abrupt sputtery cutoff
                }

                // Drive: quadratic map, aggressive high-gain front end.
                const float drive = 1.0f + fz * fz * 130.0f;
                driveBuf_[i] = drive;
                buf[i] = detail::sanitize(x * gate);

                // Dying-battery bias shift: the supply sags while the driven
                // level is high, pushing the stage-1 operating point off
                // center; it recovers as the note decays (sputtery tails at
                // high fuzz). Bias is slow-varying INPUT to the fixed ADAA
                // shapers below, so it does not break their antiderivatives.
                const float sag = c.sagEnv.process(std::min(std::fabs(buf[i]) * drive, 3.0f));
                biasBuf_[i] = kBias0 + kSagAmount * fz * fz * (sag * (1.0f / 3.0f));
            }

            // --- oversampled ADAA clip cascade ------------------------------
            c.os.upsample(buf, frames);
            float* up = c.os.data();
            const int m = c.os.upSize(frames);
            for (int k = 0; k < m; ++k) {
                const int base = k / kOversample;
                // Stage 1: driven asymmetric soft saturation (ADAA tanh).
                float s = adaaTanh(2.2f * (driveBuf_[base] * up[k] + biasBuf_[base]), c.st1);
                // Stage 2: asymmetric hard clip, positive side earlier/harder
                // (classic fuzz asymmetry), closed-form ADAA.
                s = adaaFuzzClip(s, c.st2);
                up[k] = detail::sanitize(s);
            }
            c.os.downsample(buf, frames);

            // --- base-rate tone / level -------------------------------------
            for (int i = 0; i < frames; ++i) {
                float y = buf[i];

                // Tilt tone (constant-gain at tone=0.5).
                const float tn  = tnBuf_[i];
                const float low = c.toneLpf.process(y);
                const float high = y - low;
                y = low * (2.0f * (1.0f - tn)) + high * (2.0f * tn);

                // Remove clip-induced DC, apply level.
                y = c.dcBlock.process(y);
                y *= lvBuf_[i] * 0.9f;

                // Guaranteed bound: final hard clip.
                y = hardClip(y, 1.0f);
                if (!std::isfinite(y)) y = 0.f;
                buf[i] = y;
            }
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the smoother's CURRENT value, so a gate case can assert a
    // freshly prepared core starts AT its configured value rather than
    // ramping up to it from 0 (SmoothedValue::reset() preserves mCurrent).
    float levelCurrentForTest() const { return levelSm_.current(); }
    float fuzzCurrentForTest()  const { return fuzzSm_.current(); }

private:
    static constexpr int   kOversample = 8;
    static constexpr float kBias0      = 0.12f; // static asymmetry (even harmonics)
    static constexpr float kSagAmount  = 0.35f; // max extra bias from battery sag
    static constexpr float kLoadBright = 0.35f; // pickup-loading shelf: ~-9 dB HF

    struct ChannelState {
        detail::OversamplerNx os;
        OnePole inputHpf;
        OnePole loadLpf;
        OnePole toneLpf;
        EnvelopeFollower env;
        EnvelopeFollower sagEnv;
        DcBlocker dcBlock;
        float st1 = 0.f; // ADAA state, stage 1 (tanh)
        float st2 = 0.f; // ADAA state, stage 2 (asymmetric hard clip)
    };

    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.f;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    static float hardClip(float x, float lim) {
        if (x >  lim) return  lim;
        if (x < -lim) return -lim;
        return x;
    }

    // Stage-2 transfer: identity inside (kLoClip, kHiClip), tiny residual
    // slope outside (soft knee). Asymmetric: positive clips earlier.
    static constexpr float kHiClip = 0.80f;
    static constexpr float kLoClip = -0.92f;
    static constexpr float kKnee   = 0.02f;

    static float fuzzClipShape(float x) {
        if (x > kHiClip) return kHiClip + (x - kHiClip) * kKnee;
        if (x < kLoClip) return kLoClip + (x - kLoClip) * kKnee;
        return x;
    }
    // Antiderivative of fuzzClipShape (continuous at the knees).
    static double fuzzClipAd(double x) {
        if (x > kHiClip) {
            const double d = x - kHiClip;
            return 0.5 * kHiClip * kHiClip + kHiClip * d + 0.5 * kKnee * d * d;
        }
        if (x < kLoClip) {
            const double d = x - kLoClip;
            return 0.5 * kLoClip * kLoClip + kLoClip * d + 0.5 * kKnee * d * d;
        }
        return 0.5 * x * x;
    }
    // First-order ADAA of the asymmetric clip (same recipe as adaaTanh).
    static float adaaFuzzClip(float x, float& x1state) {
        const double diff = static_cast<double>(x) - static_cast<double>(x1state);
        double y;
        if (std::fabs(diff) < 1e-5) {
            y = fuzzClipShape(0.5f * (x + x1state));
        } else {
            y = (fuzzClipAd(x) - fuzzClipAd(x1state)) / diff;
        }
        x1state = x;
        return static_cast<float>(y);
    }

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    int nch_ = 1;
    FuzzParams p_{};

    SmoothedValue fuzzSm_, toneSm_, levelSm_;
    std::vector<float> fzBuf_, tnBuf_, lvBuf_, biasBuf_, driveBuf_;
    std::vector<ChannelState> ch_;
};

} // namespace dcdsp
