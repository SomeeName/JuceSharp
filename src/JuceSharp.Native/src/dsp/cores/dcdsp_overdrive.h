// dcdsp_overdrive.h — soft, asymmetric tube-style overdrive.
// Path: (optional BOUNDED pre-clip bass shelf) -> input drive gain -> (2x OS)
// asymmetric ADAA-tanh waveshaper -> DC block -> tilt tone -> (optional mid
// bump) -> post low-pass -> (optional dry blend) -> output level.
// The `model` param (0.16) selects a per-model coefficient table (soundalike
// pedal voicings) inside the SAME OS/ADAA wrap, so CPU stays flat; model 0 is
// bit-identical to the original generic overdrive:
//   0 = Generic (original), 1 = Screamer (mid hump),
//   2 = Blues Breaker (open), 3 = Transparent (klone-lite).
// Pure dcdsp core: no JUCE, no I/O, RT-safe (alloc only in prepare()).
#pragma once

#ifdef DCDSP_TEST_STUBS
  #include "test_stubs.h"        // minimal primitives stand-in for standalone tests
#else
  #include "dcdsp_primitives.h"  // Agent P's shared primitives (production)
#endif
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct OverdriveParams {
    float drive = 0.5f; // 0..1  input gain into the shaper (0 dB .. +30 dB)
    float tone  = 0.5f; // 0..1  spectral tilt (0 = dark, 1 = bright)
    float level = 0.7f; // 0..1  output level
    int   model = 0;    // 0 generic / 1 screamer / 2 bluesbreaker / 3 transparent
};

class OverdriveCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        int n = std::max(1, numChannels);
        ch_.resize(n);
        for (auto& c : ch_) {
            c.dc.prepareLegacy48k(sr_);
            c.os.prepare(sampleRate, maxBlock_, kOs_); // true 2x oversampling
            // Match the wet oversampler delay plus the first-order ADAA
            // half-sample at the 2x rate. Only transparent model3 blends dry.
            dryDelaySamples_ = c.os.groupDelayBaseSamples() + 0.5 / kOs_;
            c.dryDelay.prepare(static_cast<int>(std::ceil(dryDelaySamples_)) + 4);
            c.shaperState = 0.f;
            c.dc.reset(); c.tilt.reset(); c.post.reset(); c.preHp.reset(); c.mid.reset();
        }
        preGainBuf_.assign(maxBlock_, 1.f);
        outGainBuf_.assign(maxBlock_, 0.f);
        dryBuf_.assign(maxBlock_, 0.f);
        biasBuf_.assign(maxBlock_, 0.f);
        blendBuf_.assign(maxBlock_, 1.f);
        preGain_.reset(sampleRate, 0.01f);
        outGain_.reset(sampleRate, 0.01f);
        // `model` is a NAMED discrete param — a footswitch — and it moved TWO
        // per-sample multipliers in one sample: blend 1.00 -> 0.60 (a 40 % dry
        // splice) and the shaper bias 0.06 -> 0.04, which is a DC step through
        // the tanh that the 38 Hz DcBlocker turns into a thump rather than
        // removing. Same class as ringmod's `waveform`, which was fixed.
        biasS_.reset(sampleRate, kModelFadeSeconds);
        blendS_.reset(sampleRate, kModelFadeSeconds);
        setParams(params_);
        // Start AT the configured drive/level: reset() preserves mCurrent (0 on
        // a first prepare), so without this the first 10 ms fades up from zero.
        preGain_.snapToTarget();
        outGain_.snapToTarget();
        biasS_.snapToTarget();    // a freshly prepared pedal starts AT its model
        blendS_.snapToTarget();
    }

    void reset() {
        for (auto& c : ch_) { c.os.reset(); c.dryDelay.reset(); c.shaperState = 0.f; c.dc.reset();
            c.tilt.reset(); c.post.reset(); c.preHp.reset(); c.mid.reset(); }
    }

    void setParams(const OverdriveParams& p) {
        params_ = p;
        const int mi = p.model < 0 ? 0 : (p.model > kNumModels - 1 ? kNumModels - 1 : p.model);
        const Model& md = kModels[mi];
        bias_       = md.bias;
        driveScale_ = md.driveScale;
        // A filter switched OFF keeps its state, so switching it back ON
        // re-injects audio from whenever the model was last selected. Clear on
        // the off->ON edge; a model that keeps the filter on keeps its state.
        const bool wantPreHp = md.preHpHz > 0.f;
        const bool wantMid   = md.midDb != 0.f;
        const bool preHpEdge = wantPreHp && !preHpOn_;
        const bool midEdge   = wantMid   && !midOn_;
        preHpOn_    = wantPreHp;
        midOn_      = wantMid;
        blend_      = md.blend;
        biasS_.setTarget(bias_);
        blendS_.setTarget(blend_);
        float drive = clamp01(p.drive), tone = clamp01(p.tone), level = clamp01(p.level);
        preGain_.setTarget(std::pow(10.f, drive * 30.f / 20.f) * driveScale_); // 0 .. +30 dB
        outGain_.setTarget(level * 1.2f);
        float tiltDb = (tone - 0.5f) * 2.f * 12.f;               // +/-12 dB high shelf
        float postHz = 4500.f + tone * 4000.f;                   // 4.5k .. 8.5k
        for (auto& c : ch_) {
            c.tilt.setHighShelf(sr_, md.tiltFc, 0.707, tiltDb);
            c.post.setLowpass(sr_, postHz, 0.707);
            // BOUNDED bass cut, not a high-pass (0.35). A Tube Screamer's bass
            // cut lives in the op-amp's FEEDBACK leg, so the unity path still
            // passes the fundamental and the pedal's LF loss is bounded at
            // roughly the depth of its mid hump. Spelling it as a full
            // 2nd-order Butterworth put the screamer 20.6 dB below its own
            // 200 Hz-4 kHz midband at the low E (measured; model 0, which has
            // no pre-HPF at all, sat at -2.9). See dcdsp_amp.h for the same
            // class of bug in the amp's coupling network.
            if (preHpEdge) c.preHp.reset();
            if (midEdge)   c.mid.reset();
            if (preHpOn_) c.preHp.setLowShelf(sr_, md.preHpHz, 0.707, -md.preCutDb);
            if (midOn_)   c.mid.setPeaking(sr_, md.midFc, md.midQ, md.midDb);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        int nch = std::min(numChannels, (int)ch_.size());
        int frames = std::min(numFrames, maxBlock_);
        // Per-block gain ramps computed once, shared by every channel.
        for (int i = 0; i < frames; ++i) {
            preGainBuf_[i] = preGain_.next();
            outGainBuf_[i] = outGain_.next();
            biasBuf_[i]    = biasS_.next();
            blendBuf_[i]   = blendS_.next();
        }
        // The ramp is monotonic, so the two endpoints decide whether the dry
        // path is needed at all this block. At rest on models 0-2 both are 1.0
        // and the signal path below stays sample-identical to the pre-ramp one.
        const bool blendActive = frames > 0
            && (blendBuf_[0] < 1.f || blendBuf_[frames - 1] < 1.f);

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (!x) continue;
            Channel& st = ch_[c];
            // Guard only — the +30 dB drive is applied per OS sample inside
            // the oversampled stage (multiplying here and clamping would
            // hard-clip at BASE rate, aliasing before the oversampler).
            for (int i = 0; i < frames; ++i) x[i] = detail::sanitize(x[i]);
            // Transparent models keep a clean dry copy for the output blend.
            // Keep history warm across model switches. Models0-2 never read
            // it, so their signal path remains sample-identical.
            for (int i = 0; i < frames; ++i) {
                st.dryDelay.push(x[i]);
                if (blendActive) dryBuf_[i] = st.dryDelay.readAt(static_cast<float>(dryDelaySamples_));
            }
            // Screamer-style pre-clip bass cut -> mid-forward drive (base rate).
            if (preHpOn_) for (int i = 0; i < frames; ++i) x[i] = detail::sanitize(st.preHp.process(x[i]));

            // --- 2x oversampled asymmetric soft clip -----------------------
            st.os.upsample(x, frames);
            float* up = st.os.data();
            int m = st.os.upSize(frames);
            for (int i = 0; i < m; ++i) {
                // Asymmetric bias -> even harmonics (tube-like). ADAA band-limits.
                const float v = preGainBuf_[i / kOs_] * up[i];
                up[i] = detail::sanitize(adaaTanh(v + biasBuf_[i / kOs_], st.shaperState));
            }
            st.os.downsample(x, frames);

            // --- post shaping ----------------------------------------------
            for (int i = 0; i < frames; ++i) {
                float s = st.dc.process(x[i]);   // strip the asymmetry DC
                s = st.tilt.process(s);
                if (midOn_) s = st.mid.process(s);
                s = st.post.process(s);
                if (blendActive) { const float b = blendBuf_[i]; s = b * s + (1.f - b) * dryBuf_[i]; }
                x[i] = detail::sanitize(s * outGainBuf_[i]);
            }
        }
    }

    int latencySamples() const { return blend_ < 1.f ? dryCompensationLatencySamples() : 0; }

    // Test-only: the smoother's CURRENT value, so a gate case can assert a
    // freshly prepared core starts AT its configured value rather than
    // ramping up to it from 0 (SmoothedValue::reset() preserves mCurrent).
    float preGainCurrentForTest() const { return preGain_.current(); }
    float outGainCurrentForTest() const { return outGain_.current(); }
    /// Does this model mix DRY back in, i.e. does it carry the compensation
    /// delay? The MODEL TABLE is the only source of truth for that. Adapters
    /// must call this rather than re-deriving it from the model index: a
    /// hard-coded `model >= 3` is true today only because row 3 happens to be
    /// the single blend below 1.0, and a second one added anywhere in the table
    /// would make the node's reported latency wrong with nothing failing.
    static bool modelBlendsDry(int model) {
        const int m = model < 0 ? 0 : (model > kNumModels - 1 ? kNumModels - 1 : model);
        return kModels[m].blend < 1.f;
    }
    // Preparation-only geometry. Adapters cache this on the owner thread and
    // select it using their own model, never querying callback-owned blend_.
    int dryCompensationLatencySamples() const { return static_cast<int>(std::lround(dryDelaySamples_)); }

private:
    static float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    struct Channel {
        detail::OversamplerNx os;
        InterpolatedDelayLine dryDelay;
        float shaperState = 0.f;
        DcBlocker dc;
        Biquad tilt, post, preHp, mid;
    };

    // Per-model voicing table (0.16). Model 0 is bit-identical to the original
    // generic overdrive (bias 0.12, tilt 720, no pre-HPF/mid/blend).
    struct Model {
        float bias;                 // shaper asymmetry
        float driveScale;           // extra pre-gain multiplier
        float preHpHz;              // pre-clip bass cut corner (0 = off)
        float preCutDb;             // ...and its BOUNDED depth (see setParams)
        float midFc, midQ, midDb;   // post mid bump (0 dB = off)
        float tiltFc;               // tilt shelf centre
        float blend;                // wet fraction (1 = full wet)
    };
    static const int kNumModels = 4;
    static constexpr Model kModels[kNumModels] = {
        // bias  drvS  preHp  preCut  midFc   midQ  midDb  tiltFc  blend
        { 0.12f, 1.00f,   0.f,  0.0f,  720.f, 1.0f,  0.0f,  720.f, 1.00f }, // 0 generic (original, no pre-cut)
        { 0.10f, 1.15f, 220.f,  9.0f,  720.f, 1.1f, +3.5f,  720.f, 1.00f }, // 1 screamer (mid hump)
        { 0.06f, 0.90f, 110.f,  4.0f,  500.f, 0.8f,  0.0f, 1000.f, 1.00f }, // 2 blues breaker (open)
        { 0.04f, 0.70f,  70.f,  3.0f,  800.f, 0.7f,  0.0f, 1600.f, 0.60f }, // 3 transparent (klone-lite)
    };

    static const int kOs_ = 2;
    double sr_ = 48000.0;
    double dryDelaySamples_ = 0.0;
    int maxBlock_ = 512;
    float bias_ = 0.12f;
    float driveScale_ = 1.f;
    float blend_ = 1.f;
    bool  preHpOn_ = false, midOn_ = false;
    OverdriveParams params_;
    /// The `model` footswitch crossfade — blend and shaper bias. 10 ms, the
    /// same window DspChain gives a bypass and ringmod gives a waveform.
    static constexpr float kModelFadeSeconds = 0.010f;
    SmoothedValue preGain_, outGain_;
    SmoothedValue biasS_, blendS_;   // bias_/blend_ stay the TARGETS
    std::vector<float> preGainBuf_, outGainBuf_, dryBuf_, biasBuf_, blendBuf_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
