// dcdsp_harmtremolo.h
//
// HARMONIC TREMOLO (Fender "brownface" style) — RT-safe, header-only.
//
// The signal is split into a LOW band and a HIGH band by a complementary
// first-order crossover (low = one-pole LP, high = input - low, so the two
// bands always sum back to the input exactly). Each band is then amplitude-
// modulated by the SAME LFO but in OPPOSITE PHASE and the bands are summed.
// As the LFO sweeps, energy pans between the low and high bands, producing the
// woozy, phasey warble that distinguishes harmonic tremolo from an ordinary
// (whole-signal) tremolo.
//
// Namespace: dcdsp. C++17. Deps only: <cmath><vector><array><algorithm>
// <cstddef><cstdint>. RT contract: allocation only in prepare(); process()
// never allocates / locks / throws; output is ALWAYS finite and bounded to
// |x| < 32 (final safety clamp on every sample).
//
// This header includes dcdsp_primitives.h unconditionally and carries NO local
// stubs: the one-pole low-pass is the shared dcdsp::OnePole. The LFO is still
// inlined here as phase_/phaseInc_/std::sin rather than dcdsp::Lfo, which is
// why it has no shape-crossfade facility to reach for — that is the one part of
// the old "standalone stub" note that was still true.

#pragma once

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared engine primitives (real OnePole + kPi). No local stubs.
#include "dcdsp_primitives.h"

namespace dcdsp {

// ---------------------------------------------------------------------------

struct HarmTremoloParams {
    // LFO speed in Hz.                 [0.1 .. 12]   default 4
    float rateHz = 4.0f;
    // Modulation depth (0 = bypass).   [0 .. 1]      default 0.6
    float depth = 0.6f;
    // Band-split crossover freq in Hz. [200 .. 2000] default 800
    float crossoverHz = 800.0f;
};

class HarmTremoloCore {
public:
    void prepare(double sr, int /*maxBlock*/, int nch) {
        sampleRate_ = (sr > 0.0) ? sr : 48000.0;
        maxChannels_ = (nch < 1) ? 1 : nch;
        lpf_.assign(static_cast<std::size_t>(maxChannels_), OnePole{});
        // `depth` RAMPS (kDepthRampSeconds). It is a modulation depth but it IS
        // a per-sample gain: at an LFO extreme, 0 -> 1 drops one band's gain
        // from unity to zero in one sample.
        depthSmooth_.reset(sampleRate_, kDepthRampSeconds);
        depthSmooth_.setTarget(params_.depth);
        depthSmooth_.snapToTarget();   // a freshly prepared core starts AT its depth
        updateCoeffs();
        reset();
    }

    void reset() {
        phase_ = 0.0;
        for (auto& f : lpf_) f.reset();
    }

    void setParams(const HarmTremoloParams& p) {
        params_.rateHz      = clampf(p.rateHz,      0.1f,   12.0f);
        params_.depth       = clampf(p.depth,       0.0f,    1.0f);
        depthSmooth_.setTarget(params_.depth);
        params_.crossoverHz = clampf(p.crossoverHz, 200.0f, 2000.0f);
        updateCoeffs();
    }

    // In-place processing. ch[c] points at nframes float samples for channel c.
    void process(float* const* ch, int nch, int nframes) {
        if (ch == nullptr || nframes <= 0 || nch <= 0) return;
        const int chans = (nch < maxChannels_) ? nch : maxChannels_;

        for (int n = 0; n < nframes; ++n) {
            const double depth = static_cast<double>(depthSmooth_.next());
            // Shared LFO, sampled once per frame so all channels stay in phase.
            const double lfo = std::sin(phase_);           // [-1, 1]
            phase_ += phaseInc_;
            if (phase_ >= 2.0 * kPi) phase_ -= 2.0 * kPi;

            // Opposite-phase band gains. DIP-ONLY: each gain lives in
            // [1 - depth, 1] and never exceeds unity, so the recombined
            // signal cannot exceed the input level (the old 1 +/- depth*lfo
            // form boosted the active band up to 2x and clipped hot inputs).
            // depth == 0 -> both gains 1 -> exact bypass, as before.
            const double lowGain  = 1.0 - depth * 0.5 * (1.0 + lfo);   // [1-depth, 1]
            const double highGain = 1.0 - depth * 0.5 * (1.0 - lfo);   // [1-depth, 1]

            for (int c = 0; c < chans; ++c) {
                float* buf = ch[c];
                if (buf == nullptr) continue;

                double in = static_cast<double>(buf[n]);
                if (!std::isfinite(in)) in = 0.0;          // scrub bad input

                const double low  = lpf_[static_cast<std::size_t>(c)].process(static_cast<float>(in));
                const double high = in - low;              // complementary band

                double out = lowGain * low + highGain * high;

                // Final safety: always finite, always |x| < 32.
                if (!std::isfinite(out)) out = 0.0;
                out = clampd(out, -kOutLimit, kOutLimit);

                buf[n] = static_cast<float>(out);
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    static constexpr double kOutLimit = 31.0; // strictly inside |x| < 32

    static float  clampf(float v, float lo, float hi) {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }
    static double clampd(double v, double lo, double hi) {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    void updateCoeffs() {
        for (auto& f : lpf_) f.setLowpass(sampleRate_, params_.crossoverHz);
        double inc = 2.0 * kPi * static_cast<double>(params_.rateHz) / sampleRate_;
        if (!std::isfinite(inc) || inc < 0.0) inc = 0.0;
        phaseInc_ = inc;
    }

    HarmTremoloParams params_{};
    static constexpr float kDepthRampSeconds = 0.010f;
    SmoothedValue depthSmooth_;
    std::vector<OnePole> lpf_{};
    double sampleRate_ = 48000.0;
    int    maxChannels_ = 1;
    double phase_ = 0.0;
    double phaseInc_ = 0.0;
};

} // namespace dcdsp
