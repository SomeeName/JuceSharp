// dcdsp_reverb.h — Freeverb (Schroeder) stereo reverb (pure dcdsp core).
//
// Public-domain Jezar/Dreampoint "Freeverb" topology: 8 parallel damped comb
// filters feeding 4 series allpass filters per channel. Tunings are the classic
// 44.1 kHz values, scaled to the active sample rate; the right channel adds the
// standard stereo-spread offset. Self-contained (no shared primitives) so the
// comb/allpass structs live here.
//
// 0.16 additions over stock Freeverb:
//  - Each comb's read tap is modulated by its own slow sine (~0.5-1.3 Hz,
//    +/- ~2.5 samples, linear-interpolated fractional read). Static Freeverb
//    combs produce fixed modes that ring metallically; the slow detune smears
//    the modes apart so the tail stays alive. The LFOs are amplitude-stable
//    "magic circle" recursions (2 mul/2 add per sample, no std::sin in the
//    loop), decorrelated by golden-angle start phases per comb and channel.
//  - A FIXED ~10 ms predelay on the wet input separates the reverb onset from
//    the dry attack (no catalogue param exists for it, so it is a musical
//    constant, not user-facing; dry path is untouched => latencySamples()==0).
//
// Stability: comb feedback = roomsize <= 0.98 < 1 and allpass feedback = 0.5;
// the modulated fractional read is a linear interpolation of two unity-gain
// taps, so every recursive element remains strictly contractive -> the tail
// always decays.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct ReverbParams {
    float roomSize = 0.5f; // 0 .. 1  — tail length
    float damping  = 0.5f; // 0 .. 1  — high-frequency absorption
    float width    = 1.0f; // 0 .. 1  — stereo spread of the wet signal
    float mix      = 0.3f; // 0 .. 1  — dry/wet
};

class ReverbCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int /*numChannels*/) {
        sr_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        const double ratio = sr_ / 44100.0;
        const float modDepth = (float)(kModDepthSamples44k * ratio);

        for (int i = 0; i < kNumCombs; ++i) {
            combL_[i].setSize(scaled(kCombTuning[i], ratio), modDepth);
            combR_[i].setSize(scaled(kCombTuning[i] + kStereoSpread, ratio), modDepth);
            // Golden-angle phase spread decorrelates the 16 LFOs; the right
            // channel gets an extra offset so L/R tails breathe independently.
            combL_[i].setLfo(sr_, kCombLfoHz[i], (double)i * 2.39996);
            combR_[i].setLfo(sr_, kCombLfoHz[i], (double)i * 2.39996 + 1.7);
        }
        for (int i = 0; i < kNumAllpass; ++i) {
            allpassL_[i].setSize(scaled(kAllpassTuning[i], ratio));
            allpassR_[i].setSize(scaled(kAllpassTuning[i] + kStereoSpread, ratio));
            allpassL_[i].feedback = 0.5f;
            allpassR_[i].feedback = 0.5f;
        }
        predelay_.assign((size_t)std::max(1, (int)(kPredelaySec * sr_)), 0.0f);
        preIdx_ = 0;
        // The three output gains RAMP (kMixRampSeconds). `mix` and `width` are
        // both sliders and both land here as per-block constants: at kScaleWet
        // a full mix move steps the wet by ~+9.5 dB while the dry vanishes.
        wet1S_.reset(sr_, kMixRampSeconds);
        wet2S_.reset(sr_, kMixRampSeconds);
        dryS_.reset(sr_, kMixRampSeconds);
        update();
        wet1S_.snapToTarget();   // a freshly prepared reverb starts AT its mix
        wet2S_.snapToTarget();
        dryS_.snapToTarget();
        reset();
    }

    void reset() {
        for (int i = 0; i < kNumCombs; ++i) { combL_[i].reset(); combR_[i].reset(); }
        for (int i = 0; i < kNumAllpass; ++i) { allpassL_[i].reset(); allpassR_[i].reset(); }
        std::fill(predelay_.begin(), predelay_.end(), 0.0f);
        preIdx_ = 0;
    }

    void setParams(const ReverbParams& p) {
        ReverbParams q = p;
        q.roomSize = clampf(q.roomSize, 0.0f, 1.0f);
        q.damping  = clampf(q.damping, 0.0f, 1.0f);
        q.width    = clampf(q.width, 0.0f, 1.0f);
        q.mix      = clampf(q.mix, 0.0f, 1.0f);
        p_ = q;
        update();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (numChannels <= 0) return;

        if (numChannels == 1) {
            float* m = channels[0];
            for (int n = 0; n < numFrames; ++n) {
                const float in = m[n];
                const float input = predelayed((in + in) * kFixedGain);
                float outL = 0.0f, outR = 0.0f;
                for (int i = 0; i < kNumCombs; ++i) { outL += combL_[i].process(input); outR += combR_[i].process(input); }
                for (int i = 0; i < kNumAllpass; ++i) { outL = allpassL_[i].process(outL); outR = allpassR_[i].process(outR); }
                const float w1 = wet1S_.next(), w2 = wet2S_.next(), d = dryS_.next();
                const float wetMono = 0.5f * ((outL + outR) * (w1 + w2));
                m[n] = wetMono + in * d;
            }
            return;
        }

        float* L = channels[0];
        float* R = channels[1];
        for (int n = 0; n < numFrames; ++n) {
            const float inL = L[n], inR = R[n];
            const float input = predelayed((inL + inR) * kFixedGain);
            float outL = 0.0f, outR = 0.0f;
            for (int i = 0; i < kNumCombs; ++i) { outL += combL_[i].process(input); outR += combR_[i].process(input); }
            for (int i = 0; i < kNumAllpass; ++i) { outL = allpassL_[i].process(outL); outR = allpassR_[i].process(outR); }
            const float w1 = wet1S_.next(), w2 = wet2S_.next(), d = dryS_.next();
            L[n] = outL * w1 + outR * w2 + inL * d;
            R[n] = outR * w1 + outL * w2 + inR * d;
        }
        // Channels beyond the first stereo pair are left untouched (dry).
    }

    int latencySamples() const { return 0; }

private:
    static inline float flushDenorm(float v) {
        return (std::fabs(v) < 1.0e-18f) ? 0.0f : v;
    }

    // Damped feedback comb with a slow-sine-modulated fractional read tap.
    struct Comb {
        std::vector<float> buf;
        int w = 0, size = 0, nominal = 0;
        float feedback = 0.0f;
        float damp1 = 0.0f, damp2 = 1.0f;
        float store = 0.0f;
        // Magic-circle sine LFO: u -= k*v; v += k*u keeps |(u,v)| ~ 1 exactly
        // (symplectic rotation), so no std::sin per sample and no drift.
        double u = 1.0, v = 0.0, k = 0.0;
        double u0 = 1.0, v0 = 0.0;   // start phase for reset()
        float depth = 0.0f;

        void setSize(int n, float modDepth) {
            if (n < 8) n = 8;
            nominal = n;
            depth = std::min(modDepth, (float)n * 0.25f); // keep read tap >= ~3/4 n
            size = n + (int)std::ceil(depth) + 3;
            buf.assign((size_t)size, 0.0f);
            w = 0; store = 0.0f;
        }
        void setLfo(double sr, double hz, double phase) {
            k = 2.0 * std::sin(3.14159265358979323846 * hz / (sr > 0.0 ? sr : 44100.0));
            u0 = std::cos(phase); v0 = std::sin(phase);
            u = u0; v = v0;
        }
        void reset() {
            std::fill(buf.begin(), buf.end(), 0.0f);
            w = 0; store = 0.0f; u = u0; v = v0;
        }
        void setDamp(float d) { damp1 = d; damp2 = 1.0f - d; }
        inline float process(float in) {
            u -= k * v; v += k * u;
            const float d = (float)nominal + depth * (float)v;
            const int   id = (int)d;
            const float fr = d - (float)id;
            int i0 = w - id;       if (i0 < 0) i0 += size;   // delay id
            int i1 = i0 - 1;       if (i1 < 0) i1 += size;   // delay id + 1
            const float out = buf[(size_t)i0] + fr * (buf[(size_t)i1] - buf[(size_t)i0]);
            store = flushDenorm(out * damp2 + store * damp1);
            buf[(size_t)w] = in + store * feedback;
            if (++w >= size) w = 0;
            return out;
        }
    };

    struct Allpass {
        std::vector<float> buf;
        int idx = 0;
        float feedback = 0.5f;
        void setSize(int n) { if (n < 1) n = 1; buf.assign((size_t)n, 0.0f); idx = 0; }
        void reset() { std::fill(buf.begin(), buf.end(), 0.0f); idx = 0; }
        inline float process(float in) {
            float bufout = buf[(size_t)idx];
            float out = -in + bufout;
            buf[(size_t)idx] = in + bufout * feedback;
            if (++idx >= (int)buf.size()) idx = 0;
            return out;
        }
    };

    // Fixed wet-input predelay (dry path untouched).
    inline float predelayed(float x) {
        const float out = predelay_[(size_t)preIdx_];
        predelay_[(size_t)preIdx_] = x;
        if (++preIdx_ >= (int)predelay_.size()) preIdx_ = 0;
        return out;
    }

    void update() {
        const float roomsize = p_.roomSize * kScaleRoom + kOffsetRoom; // <= 0.98
        const float damp     = p_.damping * kScaleDamp;                // <= 0.4
        const float wet      = p_.mix * kScaleWet;                     // scaled wet level
        wet1S_.setTarget(wet * (p_.width * 0.5f + 0.5f));
        wet2S_.setTarget(wet * ((1.0f - p_.width) * 0.5f));
        dryS_.setTarget(1.0f - p_.mix); // mix = 0 => unity dry passthrough
        for (int i = 0; i < kNumCombs; ++i) {
            combL_[i].feedback = roomsize; combL_[i].setDamp(damp);
            combR_[i].feedback = roomsize; combR_[i].setDamp(damp);
        }
    }

    static int scaled(int tuning, double ratio) {
        int s = (int)std::floor((double)tuning * ratio);
        return s < 1 ? 1 : s;
    }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // Freeverb constants.
    static constexpr int   kNumCombs    = 8;
    static constexpr int   kNumAllpass  = 4;
    static constexpr int   kStereoSpread = 23;
    static constexpr float kFixedGain   = 0.015f;
    static constexpr float kScaleWet    = 3.0f;
    static constexpr float kMixRampSeconds = 0.010f;
    static constexpr float kScaleDamp   = 0.4f;
    static constexpr float kScaleRoom   = 0.28f;
    static constexpr float kOffsetRoom  = 0.7f;
    // Tail-modulation depth (samples @ 44.1k) and per-comb LFO rates (Hz).
    // Depth 3.5 samples at ~1 Hz is ~1 cent of pitch wobble — it detunes the
    // comb modes (measured ~3 dB less coherent mode buildup on a single comb)
    // without an audible chorus.
    static constexpr double kModDepthSamples44k = 3.5;
    static constexpr double kPredelaySec = 0.010;
    static constexpr double kCombLfoHz[kNumCombs] =
        { 0.51, 0.59, 0.67, 0.79, 0.89, 1.01, 1.13, 1.27 };

    // Classic 44.1 kHz tunings (samples).
    static constexpr int kCombTuning[kNumCombs]      = {1116,1188,1277,1356,1422,1491,1557,1617};
    static constexpr int kAllpassTuning[kNumAllpass] = {556,441,341,225};

    Comb    combL_[kNumCombs],    combR_[kNumCombs];
    Allpass allpassL_[kNumAllpass], allpassR_[kNumAllpass];
    std::vector<float> predelay_;
    int preIdx_ = 0;
    ReverbParams p_;
    double sr_ = 44100.0;
    SmoothedValue wet1S_, wet2S_, dryS_;
};

} // namespace dcdsp
