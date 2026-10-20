// dcdsp_compressor.h — CompressorCore: feedforward log-domain soft-knee
// compressor (Giannoulis, Massberg & Reiss, "Digital Dynamic Range Compressor
// Design", JAES 2012), with makeup gain.
//
// Pure dcdsp core. RT-safe: allocation only in prepare(); process() never
// allocates, locks, or throws. The static curve is the smooth quadratic
// soft-knee gain computer and the ballistics are a branching one-pole smoother
// applied in the gain-reduction (log) domain — the canonical feedforward design.
//
// 0.16 detector upgrade:
//  - The level detector is a `detector`-blended mix of instantaneous peak and
//    ~10 ms exponential RMS (0 = peak, 1 = RMS, default RMS). Pure peak tracked
//    the waveform of low notes cycle-by-cycle, turning gain reduction into
//    intermodulation distortion; RMS averages over the cycle.
//  - A ~100 Hz one-pole sidechain highpass feeds the detector (audio path is
//    untouched), so low-E fundamentals stop pumping the gain.
//  - Program-dependent release: the release smoother speeds up (~0.3x the
//    release time) when the current gain reduction spikes above its ~250 ms
//    running average (a transient hit) and stays at the full user release on
//    sustained material — the same dual-time-constant idea as the sustainer's
//    depth-scaled release.
//
// ABI v34 EXTERNAL sidechain: process() takes an optional `key`, and the
// detector (highpass, RMS, ballistics and all) reads THAT instead of the audio.
// The "sidechain highpass" above is a detector SHAPE on the module's own input
// and always was; this is the real thing — duck under the song, or compress off
// the dry pick rather than off whatever the drive ahead of the node produced.
// Null key = the pre-v34 behaviour, byte for byte. The key is clamped to the
// PREPARED channel count because `scHpf_` is sized in prepare().
//
// Multi-channel linking: the detector is max |sidechain(x)| across all
// channels, producing one gain-reduction value applied identically to every
// channel. Max keeps the stereo image stable (both channels always get the
// same gain) and reacts to the loudest channel, which is the conventional
// choice for a bus/stereo compressor.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"   // OnePole (sidechain HPF)
#endif

#include <cmath>
#include <vector>
#include <cstddef>

namespace dcdsp {

struct CompressorParams {
    float thresholdDb = -18.0f; // knee centre, dBFS   [-60 .. 0]
    float ratio       = 4.0f;   //                     [1 .. 20]
    float attackMs    = 10.0f;  //                     [0.1 .. 100]
    float releaseMs   = 120.0f; //                     [5 .. 1000]
    float makeupDb    = 0.0f;   // output makeup gain  [0 .. 24]
    float kneeDb      = 6.0f;   // soft-knee width     [0 .. 24]
    float detector    = 1.0f;   // 0 = peak .. 1 = RMS [0 .. 1]
};

class CompressorCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        nch_ = numChannels > 0 ? numChannels : 1;
        scHpf_.assign(static_cast<size_t>(nch_), OnePole{});
        for (auto& f : scHpf_) f.setHighpass(sr_, kSidechainHpfHz);
        rmsCoef_ = tcoef(kRmsMs);
        avgCoef_ = tcoef(kGrAvgMs);
        makeupSmooth_.reset(sr_, kMakeupRampSeconds);
        recalc();
        makeupSmooth_.setTarget(p_.makeupDb);
        makeupSmooth_.snapToTarget(); // start AT the configured makeup
        reset();
    }

    void reset() {
        yL_ = 0.0;      // smoothed gain reduction, dB
        grAvg_ = 0.0;   // slow-averaged gain reduction, dB
        rmsSq_ = 0.0;   // mean-square detector state
        for (auto& f : scHpf_) f.reset();
    }

    void setParams(const CompressorParams& p) {
        p_ = p;
        clamp();
        // Makeup ramps (kMakeupRampSeconds); as a per-block constant a slider
        // drag zippered and a preset switch stepped by the whole delta.
        makeupSmooth_.setTarget(p_.makeupDb);
        recalc();
    }

    /// `key` (ABI v34) is an EXTERNAL sidechain: the detector reads it instead
    /// of the audio, and the audio path is untouched. Null = key off the input,
    /// which is what every caller before v34 did and is still the default.
    /// The key is clamped to the prepared channel count, because `scHpf_` is
    /// sized in prepare() and this runs on the callback.
    void process(float* const* channels, int numChannels, int numFrames,
                 const float* const* key = nullptr, int keyChannels = 0) {
        if (numChannels <= 0 || numFrames <= 0) return;
        const int nsc = numChannels < nch_ ? numChannels : nch_;
        const float* const* det_src = channels;
        int detCh = nsc;
        if (key != nullptr && keyChannels > 0) {
            det_src = key;
            detCh = keyChannels < nch_ ? keyChannels : nch_;
        }

        const double T = p_.thresholdDb;
        const double W = p_.kneeDb;
        const double invR = 1.0 / p_.ratio;
        const double det = p_.detector;

        for (int n = 0; n < numFrames; ++n) {
            // Linked sidechain: max |HPF(x)| across channels (audio untouched).
            double inst = 0.0;
            for (int ch = 0; ch < detCh; ++ch) {
                const float* src = det_src[ch];
                if (src == nullptr) continue;
                const double a = std::fabs(static_cast<double>(
                    scHpf_[static_cast<size_t>(ch)].process(src[n])));
                if (a > inst) inst = a;
            }
            // ~10 ms exponential RMS of the linked level.
            rmsSq_ = rmsCoef_ * rmsSq_ + (1.0 - rmsCoef_) * inst * inst;
            if (!(rmsSq_ >= 0.0)) rmsSq_ = 0.0;              // NaN guard
            const double rms = std::sqrt(rmsSq_);

            const double level = (1.0 - det) * inst + det * rms;
            const double xdB = 20.0 * std::log10(level + kTiny);

            // Static gain computer -> desired output level yG (dB), soft knee.
            //   below knee : yG = xdB
            //   in knee    : yG = xdB + (1/R - 1)(d + W/2)^2 / (2W)
            //   above knee : yG = T + d/R                     (d = xdB - T)
            const double d = xdB - T;
            double yG;
            if (W > kTiny && 2.0 * d > -W && 2.0 * d < W) {
                const double t = d + W * 0.5;
                yG = xdB + (invR - 1.0) * (t * t) / (2.0 * W);
            } else if (d > 0.0) {
                yG = T + d * invR;          // above knee (also the hard-knee path)
            } else {
                yG = xdB;                   // below knee
            }

            double cdB = xdB - yG;          // required gain reduction, >= 0
            if (cdB < 0.0) cdB = 0.0;

            // Branching one-pole smoother in the gain-reduction domain, with
            // program-dependent release: reduction spiking above its slow
            // average means a transient just hit -> recover fast; on sustained
            // material (yL_ ~ grAvg_) use the full user release.
            if (cdB > yL_) {
                yL_ = attackCoef_ * yL_ + (1.0 - attackCoef_) * cdB;
            } else {
                const double spike = yL_ - grAvg_;                    // dB
                double t = spike * (1.0 / kSpikeRangeDb);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                const double relCoef =
                    releaseCoef_ + t * (fastRelCoef_ - releaseCoef_);
                yL_ = relCoef * yL_ + (1.0 - relCoef) * cdB;
            }
            if (!std::isfinite(yL_)) yL_ = 0.0;
            grAvg_ = avgCoef_ * grAvg_ + (1.0 - avgCoef_) * yL_;
            if (!std::isfinite(grAvg_)) grAvg_ = 0.0;

            const double gainDb = (double) makeupSmooth_.next() - yL_;   // makeup minus reduction
            const float g = static_cast<float>(dbToGain(gainDb));   // exp2 form

            // `nsc`, not the caller's numChannels — and null-checked, exactly as
            // the detector loop above already is. This loop walked past the
            // PREPARED channel count and dereferenced unconditionally.
            for (int ch = 0; ch < nsc; ++ch)
                if (channels[ch] != nullptr) channels[ch][n] *= g;
        }
    }

    int latencySamples() const { return 0; }

private:
    void clamp() {
        p_.thresholdDb = clampf(p_.thresholdDb, -60.0f, 0.0f);
        p_.ratio       = clampf(p_.ratio, 1.0f, 20.0f);
        p_.attackMs    = clampf(p_.attackMs, 0.1f, 100.0f);
        p_.releaseMs   = clampf(p_.releaseMs, 5.0f, 1000.0f);
        p_.makeupDb    = clampf(p_.makeupDb, 0.0f, 24.0f);
        p_.kneeDb      = clampf(p_.kneeDb, 0.0f, 24.0f);
        p_.detector    = clampf(p_.detector, 0.0f, 1.0f);
    }
    void recalc() {
        attackCoef_  = tcoef(p_.attackMs);
        releaseCoef_ = tcoef(p_.releaseMs);
        fastRelCoef_ = tcoef(p_.releaseMs * kFastReleaseFactor);
    }
    double tcoef(double ms) const {
        if (ms <= 0.0) return 0.0;
        return std::exp(-1.0 / (ms * 0.001 * sr_));
    }
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr double kTiny = 1e-9;
    static constexpr double kRmsMs = 10.0;            // RMS window
    static constexpr double kSidechainHpfHz = 100.0;  // detector HPF corner
    static constexpr double kGrAvgMs = 250.0;         // "sustained" averaging
    static constexpr double kSpikeRangeDb = 6.0;      // spike -> full-fast span
    static constexpr double kFastReleaseFactor = 0.3; // fast release = 0.3x user

    CompressorParams p_{};
    double sr_ = 48000.0;
    static constexpr float kMakeupRampSeconds = 0.010f;
    SmoothedValue makeupSmooth_;
    int nch_ = 1;
    double yL_ = 0.0;
    double grAvg_ = 0.0;
    double rmsSq_ = 0.0;
    double attackCoef_ = 0.0, releaseCoef_ = 0.0, fastRelCoef_ = 0.0;
    double rmsCoef_ = 0.0, avgCoef_ = 0.0;
    std::vector<OnePole> scHpf_;
};

} // namespace dcdsp
