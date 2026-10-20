// dcdsp_optocomp.h — OptoCompCore: an optical compressor with a
// program-dependent release.
//
// Models the LA-2A / Diamond-style opto cell: a lamp drives a photoresistor, so
// the "release" is not one time constant at all — the cell recovers quickly
// from a short hit and slowly from one it has been holding, and that memory is
// most of what people mean by "the LA-2A sound". One knob (peakReduction) moves
// the threshold, there is no ratio control, and the knee is soft.
//
// TOPOLOGY. Detector (peak follower over an emphasis-shaped key) -> soft-knee
// gain computer -> the reduction is SPLIT INTO TWO COMPONENTS, each with its own
// release: a fast one (~60 ms) and a slow one whose time constant is driven by
// `mem_`, a one-pole that charges while the cell is working. Their SUM is the
// applied reduction, which is what produces the classic fast-then-slow recovery
// with no branch anywhere and no "which stage am I in" state to get wrong.
//
// EASY TO GET WRONG: `mem_` must charge and discharge on a time constant of the
// same order as the slow release (~1.5 s). Make it fast and every release is
// the fast one; make it a latch and the pedal never recovers its quick response
// after one loud passage.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"   // OnePole, SmoothedValue
#endif

#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

struct OptoCompParams {
    float peakReduction = 0.4f;  // how hard the cell works   [0 .. 1]
    float gainDb        = 0.0f;  // output makeup             [0 .. 24]
    int   limit         = 0;     // 0 = Compress, 1 = Limit
    float emphasis      = 0.3f;  // detector HF emphasis      [0 .. 1]
};

class OptoCompCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_  = sampleRate > 0.0 ? sampleRate : 48000.0;
        nch_ = numChannels > 0 ? numChannels : 1;

        // Rate-owned, all of it: the emphasis filter per channel and every
        // ballistic coefficient.
        emphHp_.assign((size_t) nch_, OnePole{});
        for (auto& f : emphHp_) f.setHighpass(sr_, kEmphasisHz);

        makeupSmooth_.reset(sr_, kMakeupRampSeconds);
        makeupSmooth_.setTarget(p_.gainDb);
        makeupSmooth_.snapToTarget();     // start AT the configured makeup
        recalc();
        reset();
    }

    void reset() {
        for (auto& f : emphHp_) f.reset();
        env_ = 0.0;
        grFast_ = grSlow_ = 0.0;
        mem_ = 0.0;
    }

    void setParams(const OptoCompParams& p) {
        p_ = p;
        clamp();
        makeupSmooth_.setTarget(p_.gainDb);
        recalc();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        if (emphHp_.empty()) return;
        const int nch = numChannels < nch_ ? numChannels : nch_;

        const double T    = threshold_;
        const double invR = 1.0 / ratio_;

        for (int n = 0; n < numFrames; ++n) {
            // --- detector: linked max over the emphasis-shaped key -----------
            double inst = 0.0;
            for (int ch = 0; ch < nch; ++ch) {
                const float* p = channels[ch];
                if (p == nullptr) continue;
                const float x  = p[n];
                const float hp = emphHp_[(size_t) ch].process(x);
                const double a = std::fabs((double) x + (double) p_.emphasis * (double) hp);
                if (a > inst) inst = a;
            }
            const double c = (inst > env_) ? detAttack_ : detRelease_;
            env_ = c * env_ + (1.0 - c) * inst;
            if (! std::isfinite(env_)) env_ = 0.0;

            const double xdB = 20.0 * std::log10(env_ + kTiny);

            // --- soft-knee gain computer --------------------------------------
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
            if (! (cdB > 0.0)) cdB = 0.0;

            // --- the opto cell's memory ---------------------------------------
            // Charges while the cell is working, discharges when it is not, on a
            // time constant of the same order as the slow release itself.
            const double want = (cdB > kWorkingDb) ? 1.0 : 0.0;
            mem_ = memCoef_ * mem_ + (1.0 - memCoef_) * want;
            if (! std::isfinite(mem_)) mem_ = 0.0;
            const double slowMs  = kSlowMinMs + mem_ * (kSlowMaxMs - kSlowMinMs);
            const double slowRel = tcoef(slowMs);

            // --- two components, one sum --------------------------------------
            // Both attack together; only the RELEASE differs, which is what
            // makes the recovery fast at first and slow afterwards.
            const double half = cdB * 0.5;
            grFast_ = (half > grFast_) ? (attackCoef_ * grFast_ + (1.0 - attackCoef_) * half)
                                       : (fastRel_ * grFast_ + (1.0 - fastRel_) * half);
            grSlow_ = (half > grSlow_) ? (attackCoef_ * grSlow_ + (1.0 - attackCoef_) * half)
                                       : (slowRel * grSlow_ + (1.0 - slowRel) * half);
            if (! std::isfinite(grFast_)) grFast_ = 0.0;
            if (! std::isfinite(grSlow_)) grSlow_ = 0.0;

            const double gainDb = (double) makeupSmooth_.next() - (grFast_ + grSlow_);
            const float g = (float) dbToGain(gainDb);
            for (int ch = 0; ch < nch; ++ch)
                if (channels[ch] != nullptr) channels[ch][n] *= g;
        }
    }

    int latencySamples() const { return 0; }

private:
    void clamp() {
        p_.peakReduction = clampf(p_.peakReduction, 0.0f, 1.0f, 0.4f);
        p_.gainDb        = clampf(p_.gainDb, 0.0f, 24.0f, 0.0f);
        p_.emphasis      = clampf(p_.emphasis, 0.0f, 1.0f, 0.3f);
        p_.limit         = p_.limit != 0 ? 1 : 0;
    }

    /// Everything the RATE owns, plus the two things peakReduction/limit own.
    void recalc() {
        // One knob, no ratio control: more peak reduction = a lower threshold.
        threshold_ = kThresholdTopDb
                   + (double) p_.peakReduction * (kThresholdBottomDb - kThresholdTopDb);
        ratio_ = (p_.limit != 0) ? kLimitRatio : kCompressRatio;
        detAttack_  = tcoef(kDetAttackMs);
        detRelease_ = tcoef(kDetReleaseMs);
        attackCoef_ = tcoef(kAttackMs);
        fastRel_    = tcoef(kFastReleaseMs);
        memCoef_    = tcoef(kMemoryMs);
    }

    double tcoef(double ms) const {
        if (! (ms > 0.0)) return 0.0;
        return std::exp(-1.0 / (ms * 0.001 * sr_));
    }
    static float clampf(float v, float lo, float hi, float fallback) {
        if (! std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr double kTiny = 1e-9;
    static constexpr double kKneeDb = 10.0;            // opto knees are WIDE
    static constexpr double kThresholdTopDb    = -6.0; // peakReduction 0
    static constexpr double kThresholdBottomDb = -42.0;// peakReduction 1
    static constexpr double kCompressRatio = 3.0;
    static constexpr double kLimitRatio    = 15.0;
    static constexpr double kDetAttackMs   = 1.0;
    static constexpr double kDetReleaseMs  = 25.0;
    static constexpr double kAttackMs      = 10.0;
    static constexpr double kFastReleaseMs = 60.0;
    static constexpr double kSlowMinMs     = 300.0;
    static constexpr double kSlowMaxMs     = 3000.0;
    static constexpr double kMemoryMs      = 1500.0;
    static constexpr double kWorkingDb     = 0.5;
    static constexpr double kEmphasisHz    = 1500.0;
    static constexpr float  kMakeupRampSeconds = 0.010f;

    OptoCompParams p_{};
    double sr_  = 48000.0;
    int    nch_ = 1;
    std::vector<OnePole> emphHp_;
    double env_ = 0.0;
    double grFast_ = 0.0, grSlow_ = 0.0;
    double mem_ = 0.0;
    double threshold_ = -20.0, ratio_ = 3.0;
    double detAttack_ = 0.0, detRelease_ = 0.0;
    double attackCoef_ = 0.0, fastRel_ = 0.0, memCoef_ = 0.0;
    SmoothedValue makeupSmooth_;
};

} // namespace dcdsp
