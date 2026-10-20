// dcdsp_formant.h — FORMANT / "MIKU" vocal pedal.
//
// Pure, RT-safe DSP core (namespace dcdsp, C++17). A parallel bank of resonant
// bandpass filters tuned to the F1/F2/F3 formants of the five cardinal vowels
// (A E I O U). Morphing the `vowel` parameter interpolates the formant
// frequencies, giving a talking / "singing" talkbox-style vocal character. An
// optional slow LFO auto-morphs the vowel for a chewing/"aeiou" motion. Output
// is soft-limited so even extreme settings stay bounded.
//
// 0.16:
//  - Real per-vowel amplitude AND bandwidth tables (Klatt/Csound-style vocal
//    tract data): each vowel has its own F2/F3 level (e.g. "i" carries F2 at
//    -15 dB, not equal-gain) and per-formant bandwidth (Q = f/bw), instead of
//    one fixed gain/Q set for all vowels. Amplitudes interpolate in dB,
//    frequencies/bandwidths linearly.
//  - Filter coefficients (the tan() in StateVariableTPT::set) are recomputed
//    only every kCoeffInterval samples and only when the morph actually moved
//    (>1e-4), not per sample per formant per channel. At a 0.33 ms cadence
//    under a 30 ms vowel smoother the stepping produces no measurable
//    artifacts (8-16 kHz splatter < -100 dB during a worst-case 8 Hz
//    auto-morph; the -29 dB RMS trajectory difference vs per-sample updates
//    is phase drift of the high-Q resonators, not clicks).
//
// Allocation happens only in prepare(); process() never allocates, locks, or
// throws. Codes against the dcdsp_primitives.h signatures (StateVariableTPT,
// Lfo, SmoothedValue, adaaTanh). For the standalone compile-test a minimal
// test_stubs.h stands in for the real primitives; production includes the real
// header.
#pragma once

#ifdef DCDSP_FORMANT_TEST_STUBS
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dcdsp {

struct FormantParams {
    // Vowel position, 0..1, morphs continuously across A - E - I - O - U.
    // 0.00 = A, 0.25 = E, 0.50 = I, 0.75 = O, 1.00 = U.
    float vowel = 0.0f;
    // Wet mix, 0..1. 0 = fully dry (bypass), 1 = fully formant-filtered.
    float amount = 0.5f;
    // Brightness, 0..1. Raises the relative level of the upper formants (F2/F3)
    // for a more nasal / present vocal; 0 = dark/rounded, 1 = bright/edgy.
    float brightness = 0.5f;
    // Auto-morph LFO rate in Hz, 0..8. 0 = manual (vowel parameter is used
    // directly). > 0 sweeps the vowel across the full A..U range at this rate.
    float rateHz = 0.0f;
};

class FormantCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        (void)maxBlockSize;
        int nch = (numChannels > 0) ? numChannels : 1;
        channels_.assign((size_t)nch, ChannelState{});   // sole allocation point
        lfo_.prepare(sampleRate_);
        lfo_.setShape(Lfo::Sine);
        vowelSmooth_.reset(sampleRate_, 0.03f);            // ~30 ms de-zipper
        vowelSmooth_.setTarget(clamp01(params_.vowel));
        brightSmooth_.reset(sampleRate_, 0.03f);           // same window, same path
        brightSmooth_.setTarget(clamp01(params_.brightness));
        brightSmooth_.snapToTarget();   // a freshly prepared core starts AT it
        // `amount` is the WET MIX despite the name, and RAMPS (kMixRampSeconds).
        // The vowel was already smoothed; the mix was one constant per block.
        amountSmooth_.reset(sampleRate_, kMixRampSeconds);
        amountSmooth_.setTarget(clamp01(params_.amount));
        amountSmooth_.snapToTarget();   // a freshly prepared core starts AT its mix
        lastCoeffVpos_ = -1.0f;                            // force first update
        coeffCountdown_ = 0;
        reset();
    }

    void reset() {
        for (auto& c : channels_) {
            for (auto& f : c.svf) f.reset();
            c.adaaState = 0.0f;
        }
        lfo_.reset();
        vowelSmooth_.setTarget(clamp01(params_.vowel));
        lastCoeffVpos_ = -1.0f;
        coeffCountdown_ = 0;
    }

    void setParams(const FormantParams& p) {
        params_.vowel      = clamp01(p.vowel);
        params_.amount     = clamp01(p.amount);
        amountSmooth_.setTarget(params_.amount);
        params_.brightness = clamp01(p.brightness);
        brightSmooth_.setTarget(params_.brightness);
        params_.rateHz     = clampf(p.rateHz, 0.0f, 8.0f);
        lfo_.setFrequency(params_.rateHz);
        lastCoeffVpos_ = -1.0f;    // brightness feeds the cached gains
        coeffCountdown_ = 0;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (numFrames <= 0 || numChannels <= 0 || channels_.empty()) return;
        const int nch = std::min(numChannels, (int)channels_.size());

        const bool  autoMorph = params_.rateHz > 0.0f;

        for (int n = 0; n < numFrames; ++n) {
            const float wet  = amountSmooth_.next();
            const float dryG = 1.0f - wet;
            // --- vowel position for this sample ---
            float target;
            if (autoMorph) {
                // LFO sweeps the full A..U range: [-1,1] -> [0,1].
                target = 0.5f * (lfo_.process() + 1.0f);
            } else {
                target = params_.vowel;
            }
            vowelSmooth_.setTarget(target);
            const float vpos = clamp01(vowelSmooth_.next());
            const float bpos = brightSmooth_.next();

            // --- block-rate coefficient update (hoists the tan() recompute) ---
            if (--coeffCountdown_ <= 0) {
                coeffCountdown_ = kCoeffInterval;
                // The `bpos` half is load-bearing: the gains are CACHED, so
                // gating the recompute on the vowel alone would leave a
                // brightness ramp reading whatever value it held when setParams
                // forced its one update -- i.e. the knob would stop working.
                if (std::fabs(vpos - lastCoeffVpos_) > 1e-4f
                 || std::fabs(bpos - lastCoeffBright_) > 1e-4f) {
                    lastCoeffVpos_   = vpos;
                    lastCoeffBright_ = bpos;
                    updateCoeffs(vpos);
                }
            }

            for (int c = 0; c < nch; ++c) {
                ChannelState& cs = channels_[(size_t)c];

                float x = channels[c][n];
                if (!std::isfinite(x)) x = 0.0f;

                cs.svf[0].process(x);
                cs.svf[1].process(x);
                cs.svf[2].process(x);

                // Bandpass output of the TPT SVF peaks at ~Q at resonance;
                // 1/Q (folded into the cached gains) normalises each band to
                // its table amplitude.
                float band = g_[0] * cs.svf[0].bp()
                           + g_[1] * cs.svf[1].bp()
                           + g_[2] * cs.svf[2].bp();

                // Makeup then soft-limit: guarantees a bounded, stable output
                // even at extreme resonance / brightness.
                float wetOut = adaaTanh(kMakeup * band, cs.adaaState);

                float y = dryG * x + wet * wetOut;
                if (!std::isfinite(y)) y = 0.0f;
                channels[c][n] = y;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    struct ChannelState {
        std::array<StateVariableTPT, 3> svf{};
        float adaaState = 0.0f;
    };

    static float clampf(float v, float lo, float hi) {
        return (v < lo) ? lo : (v > hi ? hi : v);
    }
    static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

    // Interpolate the vowel tables at pos in [0,1] and load the filters (ALL
    // allocated channels, so none is left with stale coefficients).
    // Frequencies/bandwidths interpolate linearly, amplitudes in dB.
    void updateCoeffs(float pos) {
        float seg = pos * 4.0f;                 // 0..4 across A E I O U
        int i = (int)seg;
        if (i < 0) i = 0;
        if (i > 3) i = 3;
        float t = seg - (float)i;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        // Brightness scales the upper-formant gains from 0.5x..1.5x.
        // Read the SMOOTHER, not the struct: this multiplies g_[1] and g_[2],
        // the per-sample upper-formant gains, and runs 0.5x -> 1.5x (3x) across
        // the knob. vowelSmooth_ one field over exists for exactly this.
        const float bright = 0.5f + brightSmooth_.current();

        for (int f = 0; f < 3; ++f) {
            const float fc = kFreq[f][i] + (kFreq[f][i + 1] - kFreq[f][i]) * t;
            const float bw = kBw[f][i]   + (kBw[f][i + 1]   - kBw[f][i])   * t;
            const float db = kAmpDb[f][i] + (kAmpDb[f][i + 1] - kAmpDb[f][i]) * t;
            double Q = (double)fc / (double)((bw < 20.0f) ? 20.0f : bw);
            if (Q < 1.0) Q = 1.0;
            if (Q > 40.0) Q = 40.0;

            float amp = std::pow(10.0f, db * (1.0f / 20.0f));
            if (f > 0) amp *= bright;
            // Fold the 1/Q bp normalisation into the cached gain.
            g_[f] = amp * (float)(1.0 / Q);

            for (auto& cs : channels_)
                cs.svf[(size_t)f].set(sampleRate_, fc, Q);
        }
    }

    // Cardinal vowel formant tables, A E I O U (neutral tenor-ish voice,
    // Klatt/Csound-style vocal-tract data): centre frequency (Hz), amplitude
    // relative to F1 (dB), bandwidth (Hz).
    static constexpr float kFreq[3][5] = {
        {  700.f,  530.f,  270.f,  570.f,  300.f },   // F1
        { 1220.f, 1840.f, 2290.f,  840.f,  870.f },   // F2
        { 2600.f, 2480.f, 3010.f, 2410.f, 2240.f },   // F3
    };
    static constexpr float kAmpDb[3][5] = {
        {   0.f,   0.f,   0.f,   0.f,   0.f },        // F1 reference
        {  -6.f, -14.f, -15.f, -10.f, -20.f },        // F2
        {  -7.f, -12.f, -18.f, -12.f, -17.f },        // F3
    };
    static constexpr float kBw[3][5] = {
        {  80.f,  70.f,  40.f,  40.f,  40.f },        // B1
        {  90.f,  80.f,  90.f,  80.f,  60.f },        // B2
        { 120.f, 100.f, 100.f, 100.f, 100.f },        // B3
    };

    // Coefficient update cadence in samples (~0.33 ms @ 48 kHz). The vowel
    // smoother's 30 ms ramp makes the stepping inaudible (see file header for
    // the measured numbers). The macro hook exists so the standalone harness
    // can compile an interval-1 reference build for that comparison.
#ifndef DCDSP_FORMANT_COEFF_INTERVAL
#define DCDSP_FORMANT_COEFF_INTERVAL 16
#endif
    static constexpr int kCoeffInterval = DCDSP_FORMANT_COEFF_INTERVAL;
    static constexpr float kMakeup = 2.2f; // pre-limiter gain for presence

    FormantParams params_{};
    double sampleRate_ = 48000.0;
    std::vector<ChannelState> channels_;
    Lfo lfo_;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue vowelSmooth_;
    SmoothedValue brightSmooth_;   // params_.brightness stays the TARGET
    SmoothedValue amountSmooth_;
    float g_[3] = { 0.f, 0.f, 0.f };   // cached amp * (1/Q) per formant
    float lastCoeffVpos_ = -1.0f;
    float lastCoeffBright_ = -1.0f;
    int   coeffCountdown_ = 0;
};

// Out-of-line constexpr definitions (C++17: needed for ODR-use of the arrays).
constexpr float FormantCore::kFreq[3][5];
constexpr float FormantCore::kAmpDb[3][5];
constexpr float FormantCore::kBw[3][5];

} // namespace dcdsp
