// dcdsp_feedbacker.h — controlled amp-feedback / infinite-sustain howl (dcdsp core).
//
// What real amp feedback is: the speaker drives the string, the string drives the
// pickup, and the loop closes around a resonance until a partial self-oscillates.
// We do not have a speaker or a string, so we build the SAME loop in the box: the
// note you are holding sets the resonant frequency of a high-Q filter, and that
// filter is placed inside a saturating feedback loop that is allowed to break into
// oscillation. Hold a note and it blooms into a sustained howl — at the
// fundamental, or (the classic) at the octave or the fifth above it.
//
// -------------------------- STABILITY GUARANTEE ----------------------------
// The recirculating state CANNOT grow without bound. Per sample:
//
//     s   = in + fb                                (fb is last sample's loop state)
//     r   = normalizedBandpass(s)                  (TPT SVF: unconditionally stable
//                                                   under audio-rate fc/Q modulation)
//     fb  = g * adaaTanh(kLoopDrive * r)           (0 <= g <= 0.98)
//
// `adaaTanh` returns tanh(xi) for some xi between the two input samples (mean
// value theorem on log(cosh)), so |adaaTanh(.)| < 1 ALWAYS. Therefore
//
//     |fb| < g <= 0.98    every sample, unconditionally
//
// — independent of Q, of the tracked frequency, of the sample rate and of how
// hard you hit it. The loop state is bounded BY CONSTRUCTION, not by a limiter
// that could be outrun. Self-oscillation happens because the small-signal loop
// gain (g * kLoopDrive) exceeds 1; the tanh's compressive gain then falls until
// the loop gain is exactly 1, which is a stable limit cycle — the howl. The wet
// signal is separately regulated by an AGC to the note's OWN peak level (clamped
// to [0, 8]), so the howl arrives at the level you played at, not at full scale.
// A non-finite scrub and a final clamp guard the output regardless.
// ---------------------------------------------------------------------------
//
// Contract: namespace dcdsp, C++17, header-only, zero framework deps. Allocation
// happens ONLY in prepare(); process() never allocates, locks, or throws.
//
// The wet is ADDED to the dry, not crossfaded with it — a sustainer does not take
// your guitar away, it blooms underneath it. `mix` is therefore the howl's level.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_pitch.h"

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

struct FeedbackerParams {
    // How hard the loop is driven -> how fast and how violently it howls. [0, 1]
    float amount = 0.5f;
    // Bloom time: how long a held note takes to reach full feedback. [10, 3000] ms
    float riseMs = 400.0f;
    // Which partial howls: 0 = Fundamental, 1 = Octave Up, 2 = Fifth Up,
    // 3 = Two Octaves Up.
    int   harmonic = 1;
    // Q of the resonator: low = a fat, breathy howl; high = a pure, screaming
    // sine. [0, 1] -> Q 2 .. 42
    float resonance = 0.7f;
    // Playing level required to ARM the feedback. Below it the loop bleeds away,
    // so the pedal is silent when you are. [-60, -10] dBFS
    float thresholdDb = -40.0f;
    // Level of the howl, relative to the note that summoned it. [0, 1]
    float mix = 0.5f;
};

class FeedbackerCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxCh_ = (numChannels > 0) ? numChannels : 1;

        tracker_.prepare(sr_);
        chans_.clear();
        chans_.resize(static_cast<std::size_t>(maxCh_));
        for (auto& ch : chans_) {
            ch.loopEnv.prepare(sr_);
            ch.loopEnv.setTimes(20.0f, 200.0f);
            ch.dc.prepare(sr_, 18.0);
        }
        recompute();
        // The howl's LEVEL ramps (kLevelRampSeconds): it is added on top of an
        // untouched dry, so as a per-block constant a drag stepped a steady,
        // self-oscillating tone by its whole amplitude at every block edge.
        mixS_.reset(sr_, kLevelRampSeconds);
        mixS_.setTarget(mix_);
        mixS_.snapToTarget();   // a freshly prepared core starts AT its level
        reset();
    }

    void reset() {
        tracker_.reset();
        for (auto& ch : chans_) {
            ch.reso.reset();
            ch.loopEnv.reset();
            ch.dc.reset();
            ch.fb  = 0.f;
            ch.sat = 0.f;
        }
        coeffCountdown_ = 0;
        lastCoeffHz_ = -1.f;
        lastCoeffQ_ = -1.0;
        bloom_     = 0.f;
        targetAmp_ = 0.f;
        freqHz_    = 220.f;
        armed_     = false;
    }

    void setParams(const FeedbackerParams& p) {
        amount_      = clamp(p.amount, 0.f, 1.f);
        riseMs_      = clamp(p.riseMs, 10.f, 3000.f);
        harmonic_    = clampi(p.harmonic, 0, 3);
        resonance_   = clamp(p.resonance, 0.f, 1.f);
        thresholdDb_ = clamp(p.thresholdDb, -60.f, -10.f);
        mix_         = clamp(p.mix, 0.f, 1.f);
        mixS_.setTarget(mix_);
        recompute();
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int C = std::min(numChannels, static_cast<int>(chans_.size()));
        if (C <= 0) return;

        const float invC     = 1.f / static_cast<float>(C);
        const float riseInc  = static_cast<float>(1000.0 / (static_cast<double>(riseMs_) * sr_));
        const float fallInc  = static_cast<float>(1000.0 / (kFallMs * sr_));
        const float loopGain = kMaxLoop * amount_;

        for (int n = 0; n < numFrames; ++n) {
            // ---- mono detector: the note you are holding ---------------------
            float mono = 0.f;
            for (int c = 0; c < C; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                float in = buf[n];
                if (!std::isfinite(in)) { in = 0.f; buf[n] = 0.f; }
                mono += in;
            }
            mono *= invC;

            tracker_.process(mono);
            const float amp = tracker_.amplitude();

            // ARM / HOLD hysteresis. A single threshold makes the howl die WITH the
            // string (it disarms the moment the note decays past it), which is the
            // opposite of what a sustainer is for. So: ARM on a real pitched note
            // above the threshold, and then HOLD the loop up for as long as the
            // string is alive at all — the loop is self-oscillating and does not
            // need the string to keep going, only permission. MUTE the strings
            // (input under -60 dBFS) and the bloom bleeds out in 150 ms.
            if (amp > threshLin_ && tracker_.confident()) armed_ = true;
            if (amp <= kMuteFloor)                        armed_ = false;

            bloom_ += armed_ ? riseInc : -fallInc;
            bloom_ = clamp(bloom_, 0.f, 1.f);

            // The howl is regulated to the note's own peak, held with a very slow
            // decay (~6 s) — so a feedback note SUSTAINS at the level you played it
            // instead of sagging away with the string underneath it.
            if (amp > targetAmp_) targetAmp_ = amp;
            else                  targetAmp_ += holdDecay_ * (amp - targetAmp_);

            if (tracker_.confident()) {
                const float f = tracker_.frequencyHz() * ratio_;
                if (std::isfinite(f)) freqHz_ = clamp(f, 40.f, 5000.f);
            }

            const float g    = loopGain * bloom_;
            const float mixG = mixS_.next();

            for (int c = 0; c < C; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                Channel& s = chans_[static_cast<std::size_t>(c)];

                const float dry = buf[n];

                // The loop. The SVF DESIGN is hoisted: freqHz_ only moves when
                // the pitch tracker publishes a new note (and q_ only on a knob
                // write), so re-running set()'s std::tan and divide every sample
                // per channel was redesigning an unchanged filter. Same shape as
                // dcdsp_formant.h's kCoeffInterval guard, and EXACT: when the
                // inputs have not changed the coefficients are the ones set()
                // would have produced. The design is also shared across channels
                // — identical arguments, different STATE.
                if (c == 0) {
                    if (--coeffCountdown_ <= 0 ||
                        std::fabs(freqHz_ - lastCoeffHz_) > 1e-4f ||
                        std::fabs(q_ - lastCoeffQ_) > 1e-9) {
                        coeffCountdown_ = kCoeffInterval;
                        lastCoeffHz_ = freqHz_;
                        lastCoeffQ_  = q_;
                        s.reso.set(sr_, static_cast<double>(freqHz_), q_);
                    }
                } else {
                    s.reso.copyCoeffsFrom(chans_[0].reso);
                }
                s.reso.process(dry + s.fb);
                float r = static_cast<float>(s.reso.bp() / q_);   // unity peak gain
                if (!std::isfinite(r)) { r = 0.f; s.reso.reset(); s.fb = 0.f; s.sat = 0.f; }

                // |fb| < g <= 0.98 by construction. See the stability note above.
                s.fb = g * adaaTanh(kLoopDrive * r, s.sat);
                if (!std::isfinite(s.fb)) { s.fb = 0.f; s.sat = 0.f; }

                // AGC the howl to the note's level (never to full scale).
                const float lenv = s.loopEnv.process(r);
                float agc = targetAmp_ / (lenv + kEps);
                if (!std::isfinite(agc)) agc = 0.f;
                agc = clamp(agc, 0.f, kMaxAgc);

                float wet = s.dc.process(r * agc * bloom_);
                if (!std::isfinite(wet)) { wet = 0.f; s.dc.reset(); }

                float out = dry + mixG * wet;
                if (!std::isfinite(out)) out = 0.f;
                buf[n] = clamp(out, -16.f, 16.f);
            }
        }
    }

    int latencySamples() const { return 0; }

    // Test-only: the rate-derived peak-hold coefficient. Exposed so a gate case
    // can assert it MOVED across a prepare at a different rate — a per-sample
    // coefficient that survives a device change is invisible from the output
    // over any window a test can afford.
    float holdDecayForTest() const { return holdDecay_; }

private:
    struct Channel {
        StateVariableTPT reso;
        EnvelopeFollower loopEnv;
        DcBlocker        dc;
        float fb  = 0.f;   // the recirculating loop state
        float sat = 0.f;   // adaaTanh history
    };

    static float clamp(float v, float lo, float hi) {
        if (!(v == v)) return lo;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    void recompute() {
        // The peak-hold decay is a per-SAMPLE coefficient, so it belongs to the
        // RATE. As the bare literal it used to be (3.5e-6f) it meant ~6 s at
        // 48 kHz only: 6.5 s at 44.1 kHz and 1.5 s at 192 kHz — the same class
        // as the bare 0.995 DcBlocker coefficient this repo chased down across
        // six cores. One-pole time constant: 1 - exp(-1 / (tau * sr)).
        holdDecay_ = static_cast<float>(
            1.0 - std::exp(-1.0 / (kHoldSeconds * (sr_ > 0.0 ? sr_ : 48000.0))));
        q_ = 2.0 + static_cast<double>(resonance_) * 40.0;      // 2 .. 42: a howl needs Q
        threshLin_ = std::pow(10.f, thresholdDb_ / 20.f);
        switch (harmonic_) {
            case 1:  ratio_ = 2.0f;  break;   // octave up — the classic
            case 2:  ratio_ = 1.5f;  break;   // fifth up
            case 3:  ratio_ = 4.0f;  break;   // two octaves up
            default: ratio_ = 1.0f;  break;   // fundamental
        }
    }

    static constexpr float  kMaxLoop   = 0.98f;   // hard cap on the loop coefficient
    static constexpr float  kLevelRampSeconds = 0.010f;
    static constexpr float  kLoopDrive = 3.0f;    // small-signal gain into the tanh
    static constexpr float  kMaxAgc    = 8.0f;
    static constexpr float  kEps       = 1.0e-5f;
    static constexpr double kFallMs    = 150.0;   // how fast the bloom bleeds out
    static constexpr double kHoldSeconds = 6.0;   // peak-hold time constant: the howl holds
    static constexpr float  kMuteFloor = 1.0e-3f; // -60 dBFS: the strings are muted

    double sr_    = 48000.0;
    int    maxCh_ = 2;

    float amount_      = 0.5f;
    float riseMs_      = 400.0f;
    int   harmonic_    = 1;
    float resonance_   = 0.7f;
    float thresholdDb_ = -40.0f;
    float mix_         = 0.5f;
    SmoothedValue mixS_;

    double q_         = 30.0;
    float  ratio_     = 2.0f;
    float  threshLin_ = 0.01f;
    float  holdDecay_ = 3.5e-6f;   // re-derived from the rate in recompute()
    // Hoisted SVF design (see process()). 32 samples is 0.67 ms at 48 kHz —
    // far finer than the tracker's own hop, so the sweep is unchanged.
    static constexpr int kCoeffInterval = 32;
    int    coeffCountdown_ = 0;
    float  lastCoeffHz_ = -1.f;
    double lastCoeffQ_   = -1.0;

    detail::PeriodTracker tracker_{};
    std::vector<Channel>  chans_;

    float bloom_     = 0.f;
    float targetAmp_ = 0.f;
    float freqHz_    = 220.f;
    bool  armed_     = false;
};

} // namespace dcdsp
