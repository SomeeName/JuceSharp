// dcdsp_graphiceq.h — 10-band ISO graphic EQ (pure dcdsp core, header-only, RT-safe)
//
// Ten cascaded RBJ peaking biquads at the ISO octave centres
//   31 / 63 / 125 / 250 / 500 / 1k / 2k / 4k / 8k / 16k Hz
// followed by a linear output level trim. Fully bounded and unconditionally
// stable: every stage is a fixed-Q peaking filter with clamped gain, so there
// is no feedback/waveshaping path that can blow up at extreme settings.
//
// Deps: dcdsp_primitives.h (Biquad) only, itself limited to
//   <cmath> <vector> <array> <algorithm> <cstddef> <cstdint>.
#pragma once

#ifdef DCDSP_USE_TEST_STUBS
#include "test_stubs.h"    // standalone compile-test only
#else
#include "dcdsp_primitives.h"
#endif

#include <vector>
#include <array>
#include <cstddef>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct GraphicEqParams {
    // Per-band gain in dB. Range: [-12, +12]. Values outside are clamped.
    // Index -> ISO centre frequency:
    //   0:31Hz 1:63Hz 2:125Hz 3:250Hz 4:500Hz 5:1kHz 6:2kHz 7:4kHz 8:8kHz 9:16kHz
    float bands[10] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    // Output level trim in dB. Range: [-12, +12]. Clamped.
    float level = 0.f;
};

class GraphicEqCore {
public:
    static constexpr int kNumBands = 10;

    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 44100.0;
        maxCh_ = (numChannels > 0) ? numChannels : 1;
        filters_.assign(static_cast<std::size_t>(maxCh_), std::array<Biquad, kNumBands>{});
        limitState_.assign(static_cast<std::size_t>(maxCh_), 0.f);
        dirty_ = true;
        applyIfDirty();  // program coefficients + clear state
        // 10^(12/20) = the widest the level can be; full traverse in ~10 ms.
        levelSlew_ = static_cast<float>(3.9810717055349722 / (0.010 * sr_));
        levelCur_ = levelGain_;
    }

    void reset() {
        for (auto& chain : filters_)
            for (auto& bq : chain)
                bq.reset();
        std::fill(limitState_.begin(), limitState_.end(), 0.f);
    }

    void setParams(const GraphicEqParams& p) {
        // Clamp into the documented ranges up front; store canonicalised copy.
        GraphicEqParams c;
        for (int b = 0; b < kNumBands; ++b)
            c.bands[b] = clampDb(p.bands[b]);
        c.level = clampDb(p.level);

        if (!equalParams(c, params_)) {
            params_ = c;
            dirty_ = true;
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0)
            return;
        applyIfDirty();

        const int nch = std::min(numChannels, maxCh_);
        // The +/-12 dB output level was a raw per-sample multiply straight off
        // the params struct, so a -12 -> +12 preset switch stepped the output
        // 16x between two adjacent samples. Ramp it, slew-limited to a
        // full-scale traverse in ~10 ms. Computed once per BLOCK rather than
        // held in a smoother because this loop is channel-outer: a stateful
        // smoother advanced inside it would run a different part of the ramp
        // per channel and tilt the image while the knob moved.
        const float gStart = levelCur_;
        float gEnd = levelGain_;
        const float maxStep = levelSlew_ * static_cast<float>(numFrames);
        if (gEnd > gStart + maxStep) gEnd = gStart + maxStep;
        else if (gEnd < gStart - maxStep) gEnd = gStart - maxStep;
        const float gStep = (gEnd - gStart) / static_cast<float>(numFrames);
        levelCur_ = gEnd;
        for (int ch = 0; ch < nch; ++ch) {
            float* x = channels[ch];
            if (x == nullptr)
                continue;
            auto& chain = filters_[static_cast<std::size_t>(ch)];
            for (int n = 0; n < numFrames; ++n) {
                float s = x[n];
                for (int b = 0; b < kNumBands; ++b)
                    s = chain[b].process(s);
                s *= (gStart + gStep * static_cast<float>(n + 1));
                // Output safety soft-limit, bounding the output to
                // (-kMaxOut, +kMaxOut) and essentially transparent at musical
                // levels (|s| << kMaxOut).
                // ADAA1, not a raw tanh (W4-D66). It stays a SAFETY limiter
                // that only engages far past any musical level — ten cascaded
                // +12 dB bands plus a +12 dB trim is ~+132 dB of linear gain —
                // but "rarely reached" is not "never reached", and when it IS
                // reached a memoryless tanh at base rate folds its whole
                // harmonic series back into the band. Oversampling an EQ to
                // protect one stage would cost every user CPU for nothing;
                // dcdsp::adaaTanh costs one float of state per channel and one
                // extra divide, and it is what every other shaper in the rig
                // already uses.
                //
                // ...but ADAA1 is NOT transparent below the knee, which this
                // comment claimed until 2026-10: it is the MEAN of tanh over
                // the step, and in tanh's linear region the mean of a line over
                // [x1, x] is (x + x1) / 2 — a two-tap average, |cos(pi f/sr)|:
                // -1.2 dB at 8 kHz, -3 dB at 12 kHz and -6 dB at 16 kHz at
                // 48 kHz on EVERY graphic EQ, whatever its bands said (found by
                // the 16 kHz band probe of DotCharter #87). So the limiter adds
                // only its NON-LINEAR part to the dry sample: the ADAA1 mean of
                // tanh MINUS the ADAA1 mean of the identity (the same two-tap
                // average, which is alias-free because it is linear). Below
                // the knee the two cancel and the EQ is flat to Nyquist; in
                // the knee the correction is exactly the anti-aliased shaping
                // ADAA1 produced before, so the alias floor is the one
                // RtGraphicEqLimiterAliasFloorDropped measures. The hard clamp
                // keeps the documented bound when a correction lags a
                // full-scale transient by its half sample.
                float& u1 = limitState_[static_cast<std::size_t>(ch)];
                const float uPrev = u1;
                const float u = s * kInvMaxOut;
                const float shaped = adaaTanh(u, u1);
                s += kMaxOut * (shaped - 0.5f * (u + uPrev));
                if (s > kMaxOut * kBoundFraction) s = kMaxOut * kBoundFraction;
                else if (s < -kMaxOut * kBoundFraction) s = -kMaxOut * kBoundFraction;
                if (!std::isfinite(s)) s = 0.f;
                x[n] = s;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    static float clampDb(float v) {
        if (!(v == v))          // NaN guard
            return 0.f;
        if (v < -12.f) return -12.f;
        if (v >  12.f) return  12.f;
        return v;
    }

    static bool equalParams(const GraphicEqParams& a, const GraphicEqParams& b) {
        if (a.level != b.level) return false;
        for (int i = 0; i < kNumBands; ++i)
            if (a.bands[i] != b.bands[i]) return false;
        return true;
    }

    void applyIfDirty() {
        if (!dirty_)
            return;
        dirty_ = false;

        static constexpr double kCenters[kNumBands] =
            {31.0, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
        // Octave-band constant Q = sqrt(2^N)/(2^N-1) with N=1 octave.
        static constexpr double kQ = 1.4142135623730951;

        // Keep every centre safely below Nyquist so setPeaking is well-formed
        // even at low sample rates (e.g. 22.05 kHz would put 16 kHz above fs/2).
        const double fcMax = sr_ * 0.45;

        for (auto& chain : filters_) {
            for (int b = 0; b < kNumBands; ++b) {
                double fc = kCenters[b];
                if (fc > fcMax) fc = fcMax;
                // nearNyquist: the 16 kHz band is a 16 kHz band (at 48 kHz;
                // it was designed at 12 kHz until DotCharter #87). The nine
                // centres below a quarter of the rate are unchanged.
                chain[b].setPeaking(sr_, fc, kQ, static_cast<double>(params_.bands[b]), true);
            }
        }
        levelGain_ = static_cast<float>(std::pow(10.0, params_.level / 20.0));
    }

    // Output soft-limit ceiling. |output| stays strictly below this bound.
    static constexpr float kMaxOut    = 24.0f;
    static constexpr float kInvMaxOut = 1.0f / kMaxOut;
    // |output| < kMaxOut * kBoundFraction: the "strictly below" of the bound.
    static constexpr float kBoundFraction = 0.99999f;

    double sr_ = 44100.0;
    int maxCh_ = 0;
    std::vector<std::array<Biquad, kNumBands>> filters_;  // [channel][band]
    // ADAA1 history for the output limiter, one per channel (see process()).
    std::vector<float> limitState_;
    GraphicEqParams params_{};
    bool dirty_ = true;
    float levelGain_ = 1.0f;   // target
    float levelCur_ = 1.0f;    // where the ramp has got to
    float levelSlew_ = 1.0f;   // max linear change per sample (set in prepare)
};

} // namespace dcdsp
