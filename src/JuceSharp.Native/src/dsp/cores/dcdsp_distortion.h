// dcdsp_distortion.h — high-gain distortion.
// Path: tight input SHELF -> (8x oversampled) input gain + cascaded ADAA
// clipping stages -> DC block -> body SHELF -> tone (tilt + optional mid shape
// + sweepable low-pass) -> output level. 8x (not 4x) because the +40 dB drive
// keeps meaningful harmonic energy beyond a 4x Nyquist even after ADAA
// (measured ~4-12 dB better).
// The `model` param (0.16) selects a per-model coefficient table inside the
// SAME OS/ADAA wrap (flat CPU); model 0 is bit-identical to the original:
//   0 = Generic (original), 1 = Gnash (rat-ish), 2 = Metal (scooped/tight),
//   3 = Plus (op-amp soft).
// Pure dcdsp core: no JUCE, no I/O, RT-safe (alloc only in prepare()).
#pragma once

#ifdef DCDSP_TEST_STUBS
  #include "test_stubs.h"
#else
  #include "dcdsp_primitives.h"
#endif
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct DistortionParams {
    float gain    = 0.6f;   // 0..1   input drive (0 dB .. +40 dB)
    float tone    = 0.5f;   // 0..1   spectral tilt + low-pass sweep
    float level   = 0.6f;   // 0..1   output level
    // Input low-shelf corner that tightens the low end. The clamp is applied to
    // tightHz * model.tightScale and lands in [20, 400] Hz, so the effective RAW
    // range depends on `model` (metal is tighter). Nominal knob range 20..300.
    float tightHz = 90.f;
    int   model   = 0;      // 0 generic / 1 gnash / 2 metal / 3 plus
};

class DistortionCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        int n = std::max(1, numChannels);
        ch_.resize(n);
        for (auto& c : ch_) {
            c.dc.prepareLegacy48k(sr_);
            c.os.prepare(sampleRate, maxBlock_, kOs); // true kOs-times oversampling
            c.st1 = c.st2 = c.st3 = 0.f;
            c.tight.reset(); c.body.reset();
            c.dc.reset(); c.tilt.reset(); c.lp.reset(); c.mid.reset();
        }
        preGainBuf_.assign(maxBlock_, 1.f);
        outGainBuf_.assign(maxBlock_, 0.f);
        preGain_.reset(sampleRate, 0.01f);
        outGain_.reset(sampleRate, 0.01f);
        setParams(params_);
        // Start AT the configured drive/level: reset() preserves mCurrent (0 on
        // a first prepare), so without this the first 10 ms fades up from zero.
        preGain_.snapToTarget();
        outGain_.snapToTarget();
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset(); c.st1 = c.st2 = c.st3 = 0.f;
            c.tight.reset(); c.body.reset();
            c.dc.reset(); c.tilt.reset(); c.lp.reset(); c.mid.reset();
        }
    }

    void setParams(const DistortionParams& p) {
        params_ = p;
        const int mi = p.model < 0 ? 0 : (p.model > kNumModels - 1 ? kNumModels - 1 : p.model);
        const Model& md = kModels[mi];
        bias_ = md.bias; s2_ = md.s2; s3_ = md.s3;
        midOn_ = md.midDb != 0.f;
        float gain = clamp01(p.gain), tone = clamp01(p.tone), level = clamp01(p.level);
        // The tight knob multiplies the model's own tightness (metal is tighter).
        float tight = std::min(std::max(p.tightHz * md.tightScale, 20.f), 400.f);
        preGain_.setTarget(std::pow(10.f, gain * 40.f / 20.f) * md.driveScale); // 0 .. +40 dB
        outGain_.setTarget(level * 0.9f);
        float tiltDb = (tone - 0.5f) * 2.f * 9.f + md.tiltOfsDb;  // +/-9 dB high shelf
        float lpHz   = 2000.f + tone * 6000.f;                    // 2k .. 8k
        // Inter-stage bass tightening lives at BASE rate (well-conditioned):
        // running these HPFs at the oversampled rate would place their poles
        // at z~=1, where float biquad coefficients lose precision and eat the
        // harmonics.
        for (auto& c : ch_) {
            // BOUNDED shelves, not high-passes (0.35). These two cascaded
            // Butterworths put EVERY model 11-20 dB below its own 200 Hz-4 kHz
            // midband at the low E (measured at the catalogue defaults) — a real
            // RAT/DS-1 is mid-forward but keeps the fundamental. The corners
            // still voice the pedal; only the depth is now finite. Same class of
            // bug as the amp's coupling network — see dcdsp_amp.h.
            c.tight.setLowShelf(sr_, tight, 0.707, -kTightCutDb);
            c.body.setLowShelf(sr_, md.bodyHz, 0.707, -kBodyCutDb); // post-clip
            c.tilt.setHighShelf(sr_, md.tiltFc, 0.707, tiltDb);
            c.lp.setLowpass(sr_, lpHz, 0.707);
            if (midOn_) c.mid.setPeaking(sr_, md.midFc, md.midQ, md.midDb);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        // SKIP an oversized block rather than processing part of it: the tail
        // past maxBlock_ was left UNTOUCHED, i.e. full-level clean DI spliced
        // into the middle of a distorted block.
        if (numFrames > maxBlock_) return;
        int nch = std::min(numChannels, (int)ch_.size());
        int frames = std::min(numFrames, maxBlock_);
        for (int i = 0; i < frames; ++i) { preGainBuf_[i] = preGain_.next(); outGainBuf_[i] = outGain_.next(); }

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (!x) continue;
            Channel& st = ch_[c];

            // tight input HPF (gain is applied inside the OS domain below:
            // multiplying by up to +40 dB here and clamping would hard-clip
            // at BASE rate, aliasing before the oversampler can help)
            for (int i = 0; i < frames; ++i)
                x[i] = detail::sanitize(st.tight.process(x[i]));

            // --- oversampled cascaded clipper (nonlinearity only) ----------
            st.os.upsample(x, frames);
            float* up = st.os.data();
            int m = st.os.upSize(frames);
            for (int i = 0; i < m; ++i) {
                float s = preGainBuf_[i / kOs] * up[i];
                s = adaaTanh(s + bias_, st.st1);       // stage 1: soft, asymmetric
                s = adaaTanh(s2_ * s, st.st2);         // stage 2: more drive
                s = adaaHardClip(s3_ * s, st.st3);     // stage 3: hard clip (ADAA)
                up[i] = detail::sanitize(s);
            }
            st.os.downsample(x, frames);

            // --- base-rate tightening + tone + level -----------------------
            for (int i = 0; i < frames; ++i) {
                float s = st.dc.process(x[i]);
                s = st.body.process(s); // inter-stage bass tightening (base rate)
                s = st.tilt.process(s);
                if (midOn_) s = st.mid.process(s); // per-model mid shape (rat push / metal scoop)
                s = st.lp.process(s);
                x[i] = detail::sanitize(s * outGainBuf_[i]);
            }
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the smoother's CURRENT value, so a gate case can assert a
    // freshly prepared core starts AT its configured value rather than
    // ramping up to it from 0 (SmoothedValue::reset() preserves mCurrent).
    float preGainCurrentForTest() const { return preGain_.current(); }
    float outGainCurrentForTest() const { return outGain_.current(); }

private:
    static float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    struct Channel {
        detail::OversamplerNx os;
        float st1 = 0.f, st2 = 0.f, st3 = 0.f;   // ADAA states
        Biquad tight, body, tilt, lp, mid;
        DcBlocker dc;
    };

    // Per-model voicing table (0.16). Model 0 is bit-identical to the original
    // generic distortion (bias 0.08, s2 1.8, s3 1.6, body 180, tilt 1200, no
    // mid shape, drive/tight x1).
    struct Model {
        float bias;                 // stage-1 asymmetry
        float s2, s3;               // stage-2 drive, stage-3 hard-clip drive
        float driveScale;           // extra pre-gain multiplier
        float tightScale;           // multiplies the tight-knob HPF freq
        float bodyHz;               // post-clip tightening HPF
        float midFc, midQ, midDb;   // per-model mid shape (0 dB = off)
        float tiltFc, tiltOfsDb;    // tilt shelf centre + fixed offset
    };
    // Bounded depth of the two bass-shaping shelves (dB). Together they are the
    // pedal's whole low-end voicing; keep the sum under ~12 dB or the pedal is
    // thin again, and never turn either back into an unbounded high-pass.
    static constexpr float kTightCutDb = 6.0f;
    static constexpr float kBodyCutDb  = 4.0f;

    static const int kNumModels = 4;
    static constexpr Model kModels[kNumModels] = {
        // bias   s2    s3   drvS  tgtS  body    midFc  midQ  midDb  tiltFc tOfs
        { 0.08f, 1.8f, 1.6f, 1.00f, 1.0f, 180.f,  800.f, 1.0f,  0.0f, 1200.f, 0.0f }, // 0 generic (original)
        { 0.10f, 2.2f, 2.4f, 1.10f, 1.3f, 220.f,  1000.f, 0.9f, +4.0f, 1600.f, +2.0f }, // 1 gnash (rat-ish)
        { 0.06f, 2.0f, 2.0f, 1.20f, 2.0f, 240.f,  650.f, 0.7f, -6.0f, 1400.f, +1.0f }, // 2 metal (scooped/tight)
        { 0.05f, 1.4f, 1.1f, 0.85f, 0.9f, 150.f,  800.f, 1.0f,  0.0f, 1000.f, -1.5f }, // 3 plus (op-amp soft)
    };

    static const int kOs = 8;
    double sr_ = 48000.0;
    int maxBlock_ = 512;
    float bias_ = 0.08f, s2_ = 1.8f, s3_ = 1.6f;
    bool  midOn_ = false;
    DistortionParams params_;
    SmoothedValue preGain_, outGain_;
    std::vector<float> preGainBuf_, outGainBuf_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
