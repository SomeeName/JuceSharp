// dcdsp_platereverb.h — Dattorro-style plate reverb core (pure, header-only, RT-safe)
//
// Topology: predelay -> input bandwidth lowpass -> 4 input-diffusion allpasses
// -> figure-8 tank (two cross-coupled halves; each half: modulated allpass,
// long delay, damping lowpass, decay attenuation, allpass, long delay) -> 7-tap
// stereo output accumulators. Strictly contractive: the only gain elements in
// the loop are unity-gain allpasses, a <=1 damping lowpass and a per-branch decay
// multiply (decay < 1), so full-loop gain (decay^2) is strictly < 1. Denormals
// are flushed and feedback state is finite-guarded, so the tail always decays to
// zero and every sample stays finite and bounded.
//
// RT-safe: all allocation happens in prepare(); process() never allocates, locks
// or throws.
#pragma once

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Production builds pull in the shared primitives header; the standalone
// compile-test defines DCDSP_USE_TEST_STUBS to substitute a minimal local copy
// of ONLY the primitives this core uses (OnePole, Lfo, InterpolatedDelayLine,
// DcBlocker). The stub file is never shipped.
#if defined(DCDSP_USE_TEST_STUBS)
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

namespace dcdsp {

// Parameter block. All values are clamped internally, so out-of-range input is
// safe.
struct PlateReverbParams {
    float decay      = 0.5f; // 0..1  tail length / feedback (0 = short, 1 = long)
    float damping    = 0.3f; // 0..1  high-frequency absorption in the tank
    float predelayMs = 0.0f; // 0..120 ms pre-delay before the reverb
    float mix        = 0.3f; // 0..1  dry -> wet blend (0 = dry, 1 = wet)
    float width      = 1.0f; // 0..1  stereo spread of the wet signal (0 = mono)
};

class PlateReverbCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        (void)maxBlockSize;
        (void)numChannels;
        sr_ = (sampleRate > 0.0) ? sampleRate : 44100.0;

        // Dattorro's tables are specified at 29761 Hz; scale every delay/tap to
        // the running sample rate.
        scale_ = sr_ / 29761.0;

        // --- input section ---
        int maxPre = scaleN(int(0.120 * 29761.0)) + 8; // 120 ms head-room
        predelay_.prepare(maxPre);
        bandwidth_.setLowpass(sr_, std::min(16000.0, 0.45 * sr_));

        diffLen_[0] = scaleN(142); diffG_[0] = 0.75f;
        diffLen_[1] = scaleN(107); diffG_[1] = 0.75f;
        diffLen_[2] = scaleN(379); diffG_[2] = 0.625f;
        diffLen_[3] = scaleN(277); diffG_[3] = 0.625f;
        for (int i = 0; i < 4; ++i) diff_[i].prepare(diffLen_[i] + 4, diffG_[i]);

        // --- tank ---
        excursion_ = float(std::max(1.0, 12.0 * scale_)); // modulation depth

        modLenL_ = scaleN(672);
        modLenR_ = scaleN(908);
        modL_.prepare(modLenL_ + int(excursion_) + 8, 0.70f);
        modR_.prepare(modLenR_ + int(excursion_) + 8, 0.70f);

        d1L_.prepare(scaleN(4453));
        d1R_.prepare(scaleN(4217));
        ap2L_.prepare(scaleN(1800) + 4, 0.50f);
        ap2R_.prepare(scaleN(2656) + 4, 0.50f);
        d2L_.prepare(scaleN(3720));
        d2R_.prepare(scaleN(3163));

        dampL_.setLowpass(sr_, 8000.0);
        dampR_.setLowpass(sr_, 8000.0);

        lfoL_.prepare(sr_); lfoL_.setShape(Lfo::Sine); lfoL_.setFrequency(0.70f);
        lfoR_.prepare(sr_); lfoR_.setShape(Lfo::Sine); lfoR_.setFrequency(0.53f);

        // Output tap positions (Dattorro accumulators), scaled.
        tapL_[0] = float(scaleN(266));  // d1R
        tapL_[1] = float(scaleN(2974)); // d1R
        tapL_[2] = float(scaleN(1913)); // ap2R  (subtracted)
        tapL_[3] = float(scaleN(1996)); // d2R
        tapL_[4] = float(scaleN(1990)); // d1L   (subtracted)
        tapL_[5] = float(scaleN(187));  // ap2L  (subtracted)
        tapL_[6] = float(scaleN(1066)); // d2L   (subtracted)

        tapR_[0] = float(scaleN(353));  // d1L
        tapR_[1] = float(scaleN(3627)); // d1L
        tapR_[2] = float(scaleN(1228)); // ap2L  (subtracted)
        tapR_[3] = float(scaleN(2673)); // d2L
        tapR_[4] = float(scaleN(2111)); // d1R   (subtracted)
        tapR_[5] = float(scaleN(335));  // ap2R  (subtracted)
        tapR_[6] = float(scaleN(121));  // d2R   (subtracted)

        maxPreSamples_ = float(maxPre - 8);

        prepared_ = true;
        // `mix` and `width` RAMP (kMixRampSeconds); as per-block constants a mix
        // drag stepped the whole dry/wet balance and a width drag collapsed the
        // stereo image in one sample.
        mixS_.reset(sr_, kMixRampSeconds);
        widthS_.reset(sr_, kMixRampSeconds);
        // …and so does `predelayMs`, which is a plain knob (no step, no labels)
        // and is a READ TAP into the TANK INPUT: teleporting it splices
        // unrelated audio into a loop that then recirculates it at decayCoef_^2
        // for the whole tail. Exactly the resonator argument, one core over.
        predelayS_.reset(sr_, kPredelayRampSeconds);
        setParams(params_);
        mixS_.snapToTarget();   // a freshly prepared plate starts AT its mix
        widthS_.snapToTarget();
        predelayS_.snapToTarget();
        dcL_.prepareLegacy48k(sr_); dcR_.prepareLegacy48k(sr_);
        reset();
    }

    void reset() {
        predelay_.reset();
        bandwidth_.reset();
        for (auto& a : diff_) a.reset();
        modL_.reset(); modR_.reset();
        d1L_.reset();  d1R_.reset();
        ap2L_.reset(); ap2R_.reset();
        d2L_.reset();  d2R_.reset();
        dampL_.reset(); dampR_.reset();
        lfoL_.reset(); lfoR_.reset();
        dcL_.reset(); dcR_.reset();
        tailL_ = 0.0f;
        tailR_ = 0.0f;
    }

    void setParams(const PlateReverbParams& p) {
        params_ = p;
        float decay   = clamp01(p.decay);
        float damping = clamp01(p.damping);
        mix_   = clamp01(p.mix);
        width_ = clamp01(p.width);
        mixS_.setTarget(mix_);
        widthS_.setTarget(width_);

        // Map user decay 0..1 -> internal per-branch multiply, capped strictly
        // below 1 so the figure-8 loop (gain ~= decay^2) always contracts.
        decayCoef_ = 0.30f + 0.60f * decay; // 0.30 .. 0.90

        // Predelay in samples.
        float pre = p.predelayMs * float(sr_) / 1000.0f;
        predelaySamples_ = std::min(std::max(pre, 0.0f), maxPreSamples_);
        predelayS_.setTarget(predelaySamples_);

        if (prepared_) {
            // Damping lowpass cutoff: damping 0 -> bright, damping 1 -> dark.
            double fc = 250.0 + double(1.0f - damping) * (16000.0 - 250.0);
            // nearNyquist (DotCharter #87): damping below ~0.24 asks for a
            // corner past 12 kHz, which the quarter-rate ceiling flattened
            // onto 12 kHz. The input `bandwidth_` one-pole is a FIXED voicing,
            // not a knob, and keeps the old ceiling so the default plate does
            // not move.
            fc = std::min(fc, 0.45 * sr_);
            dampL_.setLowpass(sr_, fc, true);
            dampR_.setLowpass(sr_, fc, true);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (!prepared_ || numChannels <= 0 || numFrames <= 0) return;

        const bool stereo = numChannels >= 2;
        float* L = channels[0];
        float* R = stereo ? channels[1] : channels[0];

        const float tankIn = 0.5f; // attenuate into the tank for head-room

        for (int n = 0; n < numFrames; ++n) {
            const float inL = L[n];
            const float inR = R[n];
            const float mono = 0.5f * (inL + inR);

            // --- input conditioning ---
            predelay_.push(mono);
            const float preTap = predelayS_.next();
            appliedPre_ = preTap;   // test surface; see predelayTapForTest
            float x = predelay_.readAt(preTap);
            x = bandwidth_.process(x);
            for (int i = 0; i < 4; ++i) x = diff_[i].process(x, float(diffLen_[i]));
            const float in = x * tankIn;

            // --- figure-8 tank (one-sample cross coupling breaks the loop) ---
            const float fbFromR = tailR_;
            const float fbFromL = tailL_;

            // left half
            float lmod = float(modLenL_) + excursion_ * lfoL_.process();
            float l = modL_.process(in + fbFromR, lmod);
            l = d1L_.process(l);
            l = dampL_.process(l);
            l *= decayCoef_;
            l = ap2L_.process(l, float(ap2L_.length()));
            float curL = d2L_.process(l);

            // right half
            float rmod = float(modLenR_) + excursion_ * lfoR_.process();
            float r = modR_.process(in + fbFromL, rmod);
            r = d1R_.process(r);
            r = dampR_.process(r);
            r *= decayCoef_;
            r = ap2R_.process(r, float(ap2R_.length()));
            float curR = d2R_.process(r);

            // finite-guard + denormal flush on the feedback state
            if (!std::isfinite(curL)) curL = 0.0f;
            if (!std::isfinite(curR)) curR = 0.0f;
            tailL_ = flush(curL);
            tailR_ = flush(curR);

            // --- output taps (7-tap Dattorro accumulators) ---
            const float k = 0.5f; // tap coefficient
            float yl = k * ( d1R_.tap(tapL_[0]) + d1R_.tap(tapL_[1])
                           - ap2R_.tap(tapL_[2]) + d2R_.tap(tapL_[3])
                           - d1L_.tap(tapL_[4]) - ap2L_.tap(tapL_[5])
                           - d2L_.tap(tapL_[6]) );
            float yr = k * ( d1L_.tap(tapR_[0]) + d1L_.tap(tapR_[1])
                           - ap2L_.tap(tapR_[2]) + d2L_.tap(tapR_[3])
                           - d1R_.tap(tapR_[4]) - ap2R_.tap(tapR_[5])
                           - d2R_.tap(tapR_[6]) );

            yl = dcL_.process(yl);
            yr = dcR_.process(yr);

            // Guard: an unconditional soft-limit on the WET signal caps the
            // theoretical worst-case tap sum well below the |x|<32 contract.
            // At musical levels (|wet| << 8) it is transparent (16*tanh(x/16)
            // ~= x). It never touches the feedback path, so it cannot affect
            // tank stability.
            yl = softLimit(yl);
            yr = softLimit(yr);

            // stereo width (mid/side on the wet signal)
            const float wetG = mixS_.next();
            const float dryG = 1.0f - wetG;
            float midW  = 0.5f * (yl + yr);
            float sideW = 0.5f * (yl - yr) * widthS_.next();
            float wetL  = midW + sideW;
            float wetR  = midW - sideW;

            L[n] = dryG * inL + wetG * wetL;
            if (stereo) R[n] = dryG * inR + wetG * wetR;
        }
    }

    int latencySamples() const { return 0; }

    /// Test-only: the SMOOTHED pre-delay read tap, in SAMPLES. W4-D14's fix (a
    /// 20 ms SmoothedValue mirroring dcdsp_resonator.h) shipped without a probe
    /// because neither managed seam theory can see it — the four diffusers
    /// between this tap and the module's only observable smear the splice until
    /// the step probe measures no contrast, and the tank carries the old tap's
    /// phase for far longer than one block, so the trajectory probe cannot
    /// converge either (both retried and recorded in DspGainZipperTests). This
    /// is the quantity the fix is about.
    ///
    /// It records what process() ACTUALLY APPLIED on its last sample, not
    /// what the smoother holds: a probe reading the smoother would pass even
    /// if the core went on using the UN-smoothed member, which is exactly the
    /// mistake this accessor exists to avoid (measured — the first version of
    /// it made that mistake and the gate case passed with the fix reverted).
    float predelayTapForTest() const { return appliedPre_; }

private:
    // ---- small helpers ----
    static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    static inline float flush(float x) { return (std::fabs(x) < 1e-18f) ? 0.f : x; }
    // NOT ADAA (W4-D66). Two reasons, both structural. It is a safety bound at
    // |x| = 16 — unity to within 0.2 % at any level the tank reaches in use, so
    // there is nothing to alias; and it sits INSIDE the recirculating tank,
    // where ADAA1's half-sample group delay is not a quality improvement but a
    // retune of every delay line's effective length.
    static inline float softLimit(float x) { return 16.0f * std::tanh(x * (1.0f / 16.0f)); }
    int scaleN(int n) const { return std::max(1, int(std::lround(double(n) * scale_))); }

    // Schroeder allpass over an interpolated delay line (unity magnitude gain).
    struct Allpass {
        InterpolatedDelayLine dl;
        float g = 0.5f;
        int   len = 1;
        void prepare(int maxLen, float gain) { dl.prepare(maxLen); g = gain; len = std::max(1, maxLen - 4); }
        void reset() { dl.reset(); }
        int  length() const { return len; }
        float process(float x, float delay) {
            float wD = dl.readAt(delay);
            float w  = x + g * wD;
            float y  = -g * w + wD;
            dl.push(w);
            return y;
        }
        float tap(float pos) { return dl.readAt(pos); }
    };

    // Pure fractional/integer delay.
    struct Delay {
        InterpolatedDelayLine dl;
        int len = 1;
        void prepare(int length) { len = std::max(1, length); dl.prepare(len + 4); }
        void reset() { dl.reset(); }
        float process(float x) { dl.push(x); return dl.readAt(float(len)); }
        float tap(float pos) { return dl.readAt(pos); }
    };

    // ---- state ----
    double sr_ = 44100.0;
    double scale_ = 1.0;
    bool   prepared_ = false;

    PlateReverbParams params_{};
    float mix_ = 0.3f, width_ = 1.0f, decayCoef_ = 0.6f;
    static constexpr float kMixRampSeconds = 0.010f;
    /// Longer than the mix ramp: this one GLIDES a read head, and a 10 ms glide
    /// over 120 ms of predelay is still a 12x-speed sweep.
    static constexpr float kPredelayRampSeconds = 0.020f;
    SmoothedValue mixS_, widthS_;
    SmoothedValue predelayS_;
    float predelaySamples_ = 0.0f, maxPreSamples_ = 0.0f;
    float appliedPre_ = 0.0f;   // test surface only

    InterpolatedDelayLine predelay_;
    OnePole  bandwidth_;
    Allpass  diff_[4];
    int      diffLen_[4] = {1,1,1,1};
    float    diffG_[4]   = {0.75f,0.75f,0.625f,0.625f};

    Allpass  modL_, modR_;
    int      modLenL_ = 1, modLenR_ = 1;
    float    excursion_ = 1.0f;
    Delay    d1L_, d1R_, d2L_, d2R_;
    Allpass  ap2L_, ap2R_;
    OnePole  dampL_, dampR_;
    Lfo      lfoL_, lfoR_;
    DcBlocker dcL_, dcR_;

    float tailL_ = 0.0f, tailR_ = 0.0f;

    std::array<float,7> tapL_{};
    std::array<float,7> tapR_{};
};

} // namespace dcdsp
