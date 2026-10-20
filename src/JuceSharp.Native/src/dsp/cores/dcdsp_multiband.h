// dcdsp_multiband.h — MultibandCore: a three-band compressor over Linkwitz-Riley
// 4th-order crossovers.
//
// Models a studio multiband (dbx/Waves C3 shape) as a pedal: two crossover
// frequencies, an independent threshold / ratio / makeup per band, and ONE pair
// of ballistics shared by all three so the bands cannot argue about timing.
// Each LR4 crossover is two cascaded Butterworth (Q = 1/sqrt(2)) sections, which
// is what makes the two halves sum to an ALLPASS — flat magnitude, so at ratio 1
// everywhere the whole module is a wire in level terms.
//
// EASY TO GET WRONG: the LOW band must also pass through the SECOND crossover,
// or the sum is not flat. A three-band tree that splits low/high at fa and then
// splits the high half at fb leaves the low band phase-UNMATCHED against the two
// bands that went through fb, and the sum dips several dB around fa. The fix
// here is the standard one: run the low band through the fb crossover as well
// and SUM both of its outputs, which is exactly the allpass fb contributes to
// the other two. `lowApLp_`/`lowApHp_` are that compensator and nothing else.
//
// The detectors are LINKED across channels per band (max |x|), one gain per
// band applied to every channel, so compression never shifts the stereo image.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"   // BiquadD, SmoothedValue, clampCutoff
#endif

#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

/// Butterworth Q. Two cascaded sections at this Q are one Linkwitz-Riley 4th
/// order leg, and LP4 + HP4 at the same corner sum to an ALLPASS — which is the
/// property the whole three-band tree below is built on.
inline constexpr double kMultibandButterQ = 0.70710678118654752440;

struct MultibandParams {
    float lowXoverHz  = 150.0f;   // low/mid split, Hz     [60 .. 500]
    float highXoverHz = 2500.0f;  // mid/high split, Hz    [1000 .. 8000]
    float lowThresholdDb  = -20.0f, lowRatio  = 3.0f, lowGainDb  = 0.0f;
    float midThresholdDb  = -20.0f, midRatio  = 3.0f, midGainDb  = 0.0f;
    float highThresholdDb = -20.0f, highRatio = 3.0f, highGainDb = 0.0f;
    float attackMs  = 10.0f;   // [0.1 .. 100]
    float releaseMs = 120.0f;  // [5 .. 1000]
};

class MultibandCore {
public:
    static constexpr int kBands = 3;

    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_  = sampleRate > 0.0 ? sampleRate : 48000.0;
        nch_ = numChannels > 0 ? numChannels : 1;

        // Channel-indexed scratch. Sized by the CHANNEL count, never by the
        // block, so there is no oversized-block hazard in this core at all.
        split_.assign((size_t) nch_ * (size_t) kBands, 0.0f);
        xover_.assign((size_t) nch_, ChannelFilters{});

        for (int b = 0; b < kBands; ++b) {
            gainSmooth_[b].reset(sr_, kMakeupRampSeconds);
            gainSmooth_[b].setTarget(makeupOf(b));
            gainSmooth_[b].snapToTarget();   // start AT the configured makeup
        }
        recalc();                             // rate-owned: coefficients, ballistics
        reset();
    }

    /// STATE only. Re-assigning the ChannelFilters would throw away the
    /// coefficients recalc() designed — and a Biquad's reset() clears state
    /// alone for exactly that reason (dcdsp_primitives.h).
    void reset() {
        for (auto& c : xover_) {
            c.lpA.reset(); c.hpA.reset(); c.lpB.reset(); c.hpB.reset();
            c.lowApLp.reset(); c.lowApHp.reset();
        }
        for (int b = 0; b < kBands; ++b) yL_[b] = 0.0;
    }

    void setParams(const MultibandParams& p) {
        p_ = p;
        clamp();
        for (int b = 0; b < kBands; ++b) gainSmooth_[b].setTarget(makeupOf(b));
        recalc();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        if (xover_.empty()) return;
        const int nch = numChannels < nch_ ? numChannels : nch_;

        for (int n = 0; n < numFrames; ++n) {
            // --- split, and link one detector per band across channels -------
            double det[kBands] = { 0.0, 0.0, 0.0 };
            for (int ch = 0; ch < nch; ++ch) {
                const float* p = channels[ch];
                const double x = (p != nullptr) ? (double) p[n] : 0.0;
                auto& f = xover_[(size_t) ch];

                const double lowRaw = f.lpA.process(x);
                const double upper  = f.hpA.process(x);
                // The allpass compensator: the low band goes through the SECOND
                // crossover too and both halves are summed (see the header).
                const double low  = f.lowApLp.process(lowRaw) + f.lowApHp.process(lowRaw);
                const double mid  = f.lpB.process(upper);
                const double high = f.hpB.process(upper);

                const size_t base = (size_t) ch * (size_t) kBands;
                split_[base + 0] = (float) low;
                split_[base + 1] = (float) mid;
                split_[base + 2] = (float) high;

                if (std::fabs(low)  > det[0]) det[0] = std::fabs(low);
                if (std::fabs(mid)  > det[1]) det[1] = std::fabs(mid);
                if (std::fabs(high) > det[2]) det[2] = std::fabs(high);
            }

            // --- one gain per band -------------------------------------------
            float g[kBands];
            for (int b = 0; b < kBands; ++b) {
                const double xdB = 20.0 * std::log10(det[b] + kTiny);
                const double T = thresholdOf(b);
                const double invR = 1.0 / ratioOf(b);
                const double d = xdB - T;
                double yG;
                if (2.0 * d > -kKneeDb && 2.0 * d < kKneeDb) {
                    const double t = d + kKneeDb * 0.5;
                    yG = xdB + (invR - 1.0) * (t * t) / (2.0 * kKneeDb);
                } else if (d > 0.0) {
                    yG = T + d * invR;
                } else {
                    yG = xdB;
                }
                double cdB = xdB - yG;
                if (! (cdB > 0.0)) cdB = 0.0;                 // NaN lands here too

                const double coef = (cdB > yL_[b]) ? attackCoef_ : releaseCoef_;
                yL_[b] = coef * yL_[b] + (1.0 - coef) * cdB;
                if (! std::isfinite(yL_[b])) yL_[b] = 0.0;

                const double gainDb = (double) gainSmooth_[b].next() - yL_[b];
                g[b] = (float) dbToGain(gainDb);
            }

            // --- recombine -----------------------------------------------------
            for (int ch = 0; ch < nch; ++ch) {
                float* p = channels[ch];
                if (p == nullptr) continue;
                const size_t base = (size_t) ch * (size_t) kBands;
                p[n] = split_[base + 0] * g[0]
                     + split_[base + 1] * g[1]
                     + split_[base + 2] * g[2];
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    /// One LR4 leg: TWO cascaded Butterworth (Q = 1/sqrt(2)) sections. It has
    /// to be two distinct objects — BiquadD is TDF-II and a cascade needs its
    /// own state per section; running one object twice per sample is a
    /// different (and wrong) filter.
    struct Lr4 {
        BiquadD a, b;
        void setLowpass(double sr, double fc)  { a.setLowpass(sr, fc, kMultibandButterQ);  b.setLowpass(sr, fc, kMultibandButterQ); }
        void setHighpass(double sr, double fc) { a.setHighpass(sr, fc, kMultibandButterQ); b.setHighpass(sr, fc, kMultibandButterQ); }
        double process(double x) { return b.processD(a.processD(x)); }
        void reset() { a.reset(); b.reset(); }
    };

    struct ChannelFilters {
        Lr4 lpA, hpA, lpB, hpB, lowApLp, lowApHp;
    };

    double thresholdOf(int b) const {
        return b == 0 ? p_.lowThresholdDb : (b == 1 ? p_.midThresholdDb : p_.highThresholdDb);
    }
    double ratioOf(int b) const {
        return b == 0 ? p_.lowRatio : (b == 1 ? p_.midRatio : p_.highRatio);
    }
    float makeupOf(int b) const {
        return b == 0 ? p_.lowGainDb : (b == 1 ? p_.midGainDb : p_.highGainDb);
    }

    void clamp() {
        p_.lowXoverHz  = clampf(p_.lowXoverHz, 60.0f, 500.0f, 150.0f);
        p_.highXoverHz = clampf(p_.highXoverHz, 1000.0f, 8000.0f, 2500.0f);
        p_.lowThresholdDb  = clampf(p_.lowThresholdDb, -60.0f, 0.0f, -20.0f);
        p_.midThresholdDb  = clampf(p_.midThresholdDb, -60.0f, 0.0f, -20.0f);
        p_.highThresholdDb = clampf(p_.highThresholdDb, -60.0f, 0.0f, -20.0f);
        p_.lowRatio  = clampf(p_.lowRatio, 1.0f, 20.0f, 1.0f);
        p_.midRatio  = clampf(p_.midRatio, 1.0f, 20.0f, 1.0f);
        p_.highRatio = clampf(p_.highRatio, 1.0f, 20.0f, 1.0f);
        p_.lowGainDb  = clampf(p_.lowGainDb, -12.0f, 12.0f, 0.0f);
        p_.midGainDb  = clampf(p_.midGainDb, -12.0f, 12.0f, 0.0f);
        p_.highGainDb = clampf(p_.highGainDb, -12.0f, 12.0f, 0.0f);
        p_.attackMs  = clampf(p_.attackMs, 0.1f, 100.0f, 10.0f);
        p_.releaseMs = clampf(p_.releaseMs, 5.0f, 1000.0f, 120.0f);
        // A crossover pair that has crossed over is not a band split. The high
        // split is pushed above the low one rather than both being ignored.
        if (p_.highXoverHz < p_.lowXoverHz * 2.0f) p_.highXoverHz = p_.lowXoverHz * 2.0f;
    }

    /// Everything the RATE owns: six biquads per channel and both ballistics.
    /// setParams and prepare both come through here, so a device change cannot
    /// leave a 48 kHz crossover running at 192 kHz.
    void recalc() {
        const double fa = clampCutoff(sr_, (double) p_.lowXoverHz);
        const double fb = clampCutoff(sr_, (double) p_.highXoverHz);
        for (auto& f : xover_) {
            f.lpA.setLowpass(sr_, fa);
            f.hpA.setHighpass(sr_, fa);
            f.lpB.setLowpass(sr_, fb);
            f.hpB.setHighpass(sr_, fb);
            // The low band's allpass compensator at the SECOND crossover.
            f.lowApLp.setLowpass(sr_, fb);
            f.lowApHp.setHighpass(sr_, fb);
        }
        attackCoef_  = tcoef(p_.attackMs);
        releaseCoef_ = tcoef(p_.releaseMs);
    }

    double tcoef(double ms) const {
        if (! (ms > 0.0)) return 0.0;
        return std::exp(-1.0 / (ms * 0.001 * sr_));
    }

    static float clampf(float v, float lo, float hi, float fallback) {
        if (! std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr double kTiny   = 1e-9;
    static constexpr double kKneeDb = 6.0;
    static constexpr float  kMakeupRampSeconds = 0.010f;

    MultibandParams p_{};
    double sr_  = 48000.0;
    int    nch_ = 1;
    std::vector<ChannelFilters> xover_;
    std::vector<float>          split_;
    double yL_[kBands] = { 0.0, 0.0, 0.0 };
    double attackCoef_ = 0.0, releaseCoef_ = 0.0;
    SmoothedValue gainSmooth_[kBands];
};

} // namespace dcdsp
