// dcdsp_filterseq.h — step-sequenced resonant filter (dcdsp core).
//
// A step sequencer drives the cutoff of a resonant TPT state-variable filter.
// Each step jumps (or glides) the cutoff to a new value inside [rangeLo, rangeHi];
// the pattern walks Up / Down / Up-Down, or picks a NEW RANDOM value every step —
// which IS the classic sample-and-hold filter modulation, so this one core covers
// both the sequenced-filter and the S&H idiom (see the note below).
//
// The step clock has two sources:
//   trigger = Clock — a free-running rate in steps/second (the pedal idiom).
//   trigger = Pick  — the sequencer advances ONE STEP PER PICK. The onset detector
//                     runs on a HIGH-FREQUENCY envelope, never a broadband one:
//                     re-picking a still-ringing string only moves the broadband
//                     envelope ~1.4x (a 2x threshold misses half the picks), while
//                     the >1.2 kHz envelope jumps 2-4x and peaks 1-5 ms after the
//                     pick. That fact is measured, and it is why "Pick" mode feels
//                     locked to the player instead of laggy.
//
// WHY THERE IS NO SEPARATE `samplehold` MODULE: a random S&H filter is exactly
// this core with mode = Random, and a pick-triggered S&H is this core with
// mode = Random + trigger = Pick. Shipping a second module whose entire
// difference is "one param is pinned" would be catalogue noise; the mode picker
// says the same thing in the UI.
//
// Contract: namespace dcdsp, C++17, header-only, zero framework deps. Allocation
// happens ONLY in prepare(); process() never allocates, locks, or throws. The
// TPT SVF is unconditionally stable under audio-rate cutoff/Q modulation (which
// a stepped, glided cutoff is), and the output is always finite (|x| < 24).
#pragma once

#include "dcdsp_primitives.h"

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dcdsp {

namespace filterseq_detail {

// xorshift32 — deterministic, allocation-free, and seeded from a constant so a
// render is reproducible (the repo cares about that).
struct Rng {
    std::uint32_t s = 0x9E3779B9u;
    void reset() { s = 0x9E3779B9u; }
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    // Uniform in [0, 1).
    float nextUnit() {
        return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f);
    }
};

} // namespace filterseq_detail

struct FilterSeqParams {
    // Step clock speed, steps per second (Clock trigger only). [0.1, 16]
    float rateHz = 4.0f;
    // Sequence length. [2, 16]
    int   steps = 8;
    // Pattern: 0 = Up, 1 = Down, 2 = Up-Down, 3 = Random (sample & hold).
    int   mode = 0;
    // Step source: 0 = Clock (free-running rate), 1 = Pick (one step per pick).
    int   trigger = 0;
    // Bottom of the cutoff range, Hz. [100, 1000]
    float rangeLoHz = 200.0f;
    // Top of the cutoff range, Hz. [500, 6000]
    float rangeHiHz = 3000.0f;
    // Filter resonance. [0, 1] -> Q 0.7 .. 7.7
    float resonance = 0.6f;
    // Step smoothing. 0 = hard S&H edges, 1 = a ~250 ms glide between steps. [0, 1]
    float glide = 0.15f;
    // Filter type: 0 = Low-pass, 1 = Band-pass, 2 = High-pass.
    int   filterType = 0;
    // Dry/wet blend. [0, 1]
    float mix = 1.0f;
};

class FilterSeqCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxCh_ = (numChannels > 0) ? numChannels : 1;

        filters_.assign(static_cast<std::size_t>(maxCh_), StateVariableTPT{});

        onsetHp_.setHighpass(sr_, 1200.0);   // the HF band the pick lives in
        fastEnv_.prepare(sr_);
        fastEnv_.setTimes(1.0f, 40.0f);
        slowEnv_.prepare(sr_);
        slowEnv_.setTimes(60.0f, 220.0f);

        refractory_ = static_cast<int>(0.040 * sr_);   // 40 ms between picks
        // `mix` RAMPS (kMixRampSeconds); as a per-block constant a mix drag
        // stepped a full dry/wet crossfade at every block edge.
        mixSmooth_.reset(sr_, kMixRampSeconds);
        resS_.reset(sr_, kResRampSeconds);
        resS_.setTarget(resonance_);
        resS_.snapToTarget();   // a freshly prepared sequencer starts AT its resonance
        mixSmooth_.setTarget(mix_);
        mixSmooth_.snapToTarget();   // a freshly prepared sequencer starts AT its mix
        recompute();
        reset();
    }

    void reset() {
        for (auto& f : filters_) f.reset();
        onsetHp_.reset();
        fastEnv_.reset();
        slowEnv_.reset();
        rng_.reset();
        clockPhase_ = 0.f;
        stepIndex_  = 0;
        stepDir_    = 1;
        sinceOnset_ = 0;
        logCutoff_  = std::log2(static_cast<float>(std::max(20.f, lo_)));
        targetLog_  = logCutoff_;
        newStep();   // seed step 0's target
    }

    void setParams(const FilterSeqParams& p) {
        rateHz_     = clamp(p.rateHz, 0.1f, 16.f);
        steps_      = clampi(p.steps, 2, 16);
        mode_       = clampi(p.mode, 0, 3);
        trigger_    = clampi(p.trigger, 0, 1);
        lo_         = clamp(p.rangeLoHz, 100.f, 1000.f);
        hi_         = clamp(p.rangeHiHz, 500.f, 6000.f);
        if (hi_ < lo_) std::swap(hi_, lo_);
        resonance_  = clamp(p.resonance, 0.f, 1.f);
        glide_      = clamp(p.glide, 0.f, 1.f);
        filterType_ = clampi(p.filterType, 0, 2);
        mix_        = clamp(p.mix, 0.f, 1.f);
        mixSmooth_.setTarget(mix_);
        if (stepIndex_ >= steps_) stepIndex_ = 0;
        recompute();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int C = std::min(numChannels, static_cast<int>(filters_.size()));
        if (C <= 0) return;

        const float invC       = 1.f / static_cast<float>(C);
        const float clockInc   = static_cast<float>(rateHz_ / sr_);
        const bool  pickDriven = (trigger_ == 1);

        for (int n = 0; n < numFrames; ++n) {
            const float mix     = mixSmooth_.next();
            const float dryGain = 1.f - mix;
            // ---- mono detector (all channels sweep coherently) ---------------
            float mono = 0.f;
            for (int c = 0; c < C; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                float in = buf[n];
                if (!std::isfinite(in)) { in = 0.f; buf[n] = 0.f; }
                mono += in;
            }
            mono *= invC;

            // ---- step clock ---------------------------------------------------
            if (sinceOnset_ < refractory_) ++sinceOnset_;

            if (pickDriven) {
                const float hf   = onsetHp_.process(mono);
                const float fast = fastEnv_.process(hf);
                const float slow = slowEnv_.process(hf);
                if (sinceOnset_ >= refractory_ &&
                    fast > 2.0f * slow + kOnsetFloor) {
                    sinceOnset_ = 0;
                    advanceStep();
                }
            } else {
                clockPhase_ += clockInc;
                if (clockPhase_ >= 1.f) {
                    clockPhase_ -= std::floor(clockPhase_);
                    advanceStep();
                }
            }

            // ---- glide toward the step's cutoff (in log2 Hz: musical) ---------
            logCutoff_ += glideCoef_ * (targetLog_ - logCutoff_);
            if (!std::isfinite(logCutoff_)) logCutoff_ = targetLog_;
            const double cutoff = clampCutoff(sr_, std::exp2(static_cast<double>(logCutoff_)));

            // ---- resonance ----------------------------------------------------
            // W4-D5: `resonance` moves the filter's Q from 0.7 to 7.7 and the
            // compensation gain from 1.0 to 0.364, and both were per-block
            // constants — one knob step retuned the resonant peak and dropped
            // the body by 8.8 dB in a single sample. Smoothed per sample; at
            // rest resS_ IS the knob, so a settled board is unchanged.
            const double res     = static_cast<double>(resS_.next());
            appliedRes_ = (float) res;   // test surface; see resonanceCurrentForTest
            const double q       = 0.7 + res * 7.0;               // 0.7 .. 7.7
            const float  resComp = static_cast<float>(1.0 / (1.0 + 0.25 * (q - 0.7)));

            // ---- filter -------------------------------------------------------
            // `cutoff` and `q` do not depend on the channel, so the DESIGN (a
            // std::tan plus a divide) is done once and copied; each channel
            // keeps its own STATE. Bit-identical to calling set() per channel.
            for (int c = 0; c < C; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;

                StateVariableTPT& f = filters_[static_cast<std::size_t>(c)];
                const float dry = buf[n];
                if (c == 0) f.set(sr_, cutoff, q);
                else        f.copyCoeffsFrom(filters_[0]);
                f.process(dry);

                float wet;
                switch (filterType_) {
                    case 1:  wet = static_cast<float>(f.bp() / q); break;   // normalized BP
                    case 2:  wet = f.hp() * resComp;               break;
                    default: wet = f.lp() * resComp;               break;
                }
                if (!std::isfinite(wet)) { wet = 0.f; f.reset(); }

                float out = dryGain * dry + mix * wet;
                if (!std::isfinite(out)) out = 0.f;
                buf[n] = clamp(out, -24.f, 24.f);
            }
        }
    }

    int latencySamples() const { return 0; }

    /// Test-only: the SMOOTHED resonance the filter is running at right now,
    /// in the knob's own 0..1 units. Between the two endpoints while the ramp
    /// is in flight — which is the whole claim W4-D5's fix makes, and which no
    /// output-domain probe could separate from the sequencer's own sweep.
    ///
    /// It records what process() ACTUALLY APPLIED on its last sample, not
    /// what the smoother holds: a probe reading the smoother would pass even
    /// if the core went on using the UN-smoothed member, which is exactly the
    /// mistake this accessor exists to avoid (measured — the first version of
    /// it made that mistake and the gate case passed with the fix reverted).
    float resonanceCurrentForTest() const { return appliedRes_; }

private:
    static float clamp(float v, float lo, float hi) {
        if (!(v == v)) return lo;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    void advanceStep() {
        switch (mode_) {
            case 1:  // Down
                if (--stepIndex_ < 0) stepIndex_ = steps_ - 1;
                break;
            case 2:  // Up-Down (bounce, no repeated endpoint)
                if (steps_ <= 1) { stepIndex_ = 0; break; }
                stepIndex_ += stepDir_;
                if (stepIndex_ >= steps_ - 1) { stepIndex_ = steps_ - 1; stepDir_ = -1; }
                else if (stepIndex_ <= 0)     { stepIndex_ = 0;          stepDir_ =  1; }
                break;
            case 3:  // Random (sample & hold) — index is unused
                break;
            default: // Up
                if (++stepIndex_ >= steps_) stepIndex_ = 0;
                break;
        }
        newStep();
    }

    // Turn the current step into a target cutoff (log2 Hz).
    void newStep() {
        float n01;
        if (mode_ == 3) {
            n01 = rng_.nextUnit();
        } else {
            const float denom = static_cast<float>(std::max(1, steps_ - 1));
            n01 = static_cast<float>(stepIndex_) / denom;
            if (mode_ == 1) n01 = 1.f - n01;     // Down walks the ramp backwards
        }
        n01 = clamp(n01, 0.f, 1.f);

        const float loLog = std::log2(std::max(20.f, lo_));
        const float hiLog = std::log2(std::max(20.f, hi_));
        targetLog_ = loLog + n01 * (hiLog - loLog);
    }

    void recompute() {
        resS_.setTarget(resonance_);
        q_ = 0.7 + static_cast<double>(resonance_) * 7.0;   // 0.7 .. 7.7
        // Resonance eats the body: without this a maxed LP/HP resonance would put
        // a ~+18 dB spike on the sweep. Normalized so resonance = 0 is unity.
        resComp_ = static_cast<float>(1.0 / (1.0 + 0.25 * (q_ - 0.7)));

        // One-pole glide, 0 -> instant (hard S&H), 1 -> ~250 ms.
        const double tau = static_cast<double>(glide_) * 0.25;
        glideCoef_ = (tau <= 1.0e-6)
                     ? 1.f
                     : static_cast<float>(1.0 - std::exp(-1.0 / (tau * sr_)));
    }

    static constexpr float kOnsetFloor = 1.0e-4f;

    double sr_    = 48000.0;
    int    maxCh_ = 2;

    float rateHz_     = 4.0f;
    int   steps_      = 8;
    int   mode_       = 0;
    int   trigger_    = 0;
    float lo_         = 200.0f;
    float hi_         = 3000.0f;
    float resonance_  = 0.6f;
    float glide_      = 0.15f;
    int   filterType_ = 0;
    float mix_        = 1.0f;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixSmooth_;
    static constexpr float kResRampSeconds = 0.020f;
    SmoothedValue resS_;   // resonance, in knob units (W4-D5)
    float appliedRes_ = 0.6f;   // test surface only

    double q_          = 4.9;
    float  resComp_    = 0.49f;
    float  glideCoef_  = 1.0f;

    std::vector<StateVariableTPT> filters_;
    OnePole          onsetHp_{};
    EnvelopeFollower fastEnv_{};
    EnvelopeFollower slowEnv_{};
    filterseq_detail::Rng rng_{};

    float clockPhase_ = 0.f;
    int   stepIndex_  = 0;
    int   stepDir_    = 1;
    int   sinceOnset_ = 0;
    int   refractory_ = 1920;
    float logCutoff_  = 7.64f;
    float targetLog_  = 7.64f;
};

} // namespace dcdsp
