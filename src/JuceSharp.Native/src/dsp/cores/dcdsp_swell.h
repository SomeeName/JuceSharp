// dcdsp_swell.h — VOLUME / AUTO-SWELL PEDAL (pure, header-only, RT-safe)
//
// The "violin" / slow-gear effect: an envelope-driven volume pedal that DUCKS
// the pick attack and then fades the note in over the swell time, so a guitar
// speaks like a bowed string. In `Volume` mode the same module is a plain
// volume pedal (a static level with a smoothed knob).
//
// It is a LEVEL module — the only one in the catalogue besides gain/boost that
// changes the chain's static output — and its static gain is exactly
// `position` (linear, 0..1). It can only ever ATTENUATE: the applied gain lives
// in [0, position] and settles at `position` during sustain, so |out| <= |in|
// at position = 1 and the module can never blow up a downstream stage.
// DcRigLoudnessNormalizer models it as a linear multiply by `position`.
//
// ---------------------------------------------------------------------------
// The one thing that makes this work: the onset detector runs on a HIGH-
// FREQUENCY envelope, not a broadband one.
//
// A swell pedal is worthless if it cannot re-trigger on a note picked over a
// still-ringing string — which is most of what a guitarist plays. That is the
// measured finding from the mikustomp work (JuceSharp AGENTS.md): re-picking
// over a ringing string only moves the BROADBAND envelope ~1.4x, so a sane
// threshold misses half the picks, while the >1.2 kHz envelope jumps 2.1-4.0x
// and peaks 1-5 ms after the pick instead of 6-20 ms. So:
//   - the detector highpasses at 1.2 kHz,
//   - it fires on a FAST envelope out-running a SLOW one (a ratio test, which
//     is level-independent and therefore sees a re-pick during sustain), gated
//     by an absolute threshold from `sensitivity` (which sets how hard you have
//     to pick to arm it at all),
//   - and it holds a short refractory lock so one pick is one trigger.
// The 1-5 ms detection latency is also why this core adds ZERO latency: it does
// not need to look ahead to duck before the transient has finished arriving.
// (A few hundred microseconds of the pick always survive. Every real slow-gear
// has that; it is what stops the effect sounding like a gate.)
//
// The envelope NEVER multiplies the audio directly — it only triggers a state
// machine whose gain is a ramp. That is deliberate: an envelope follower's
// attack/release switch is a nonlinear recursion, and feeding it straight into
// a gain would fold its own ripple back into the signal.
//
// Click-free by construction:
//   - the duck is a constant-slope linear ramp (3 ms full-scale), not a step;
//   - the swell is a smoothstep, whose derivative is ZERO at both ends, so it
//     lands on unity without a corner;
//   - a re-trigger mid-swell re-enters the duck from WHEREVER the gain is, so
//     the gain trace stays continuous no matter how fast you pick;
//   - `position`, the swell floor and the auto/volume mode are all smoothed, so
//     no knob (or mode switch) can step the gain.
//
// Contract shape (see the sibling cores):
//   struct SwellParams { ... };
//   class  SwellCore   { prepare / reset / setParams / process / latencySamples };
//
// Real-time safety: the only allocation happens in prepare(); process() never
// allocates, locks, or throws.

#pragma once

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstddef>

#ifdef DCDSP_TEST_STUBS
#include "test_stubs.h"
#else
#include "dcdsp_primitives.h"
#endif

namespace dcdsp {

// ---------------------------------------------------------------------------
// Parameters. All values are clamped internally, so out-of-range input is safe.
// ---------------------------------------------------------------------------
struct SwellParams
{
    // 0 = Auto Swell (envelope-driven violin swell), 1 = Volume (static pedal).
    // In Volume mode attackMs / sensitivity / depth do nothing.
    int   mode        = 0;
    // The swell time: how long the note takes to fade in after a pick.
    // [5..2000 ms]. Under ~50 ms this is a transient softener rather than a
    // swell; 200-600 ms is the classic bowed/violin range; past ~1.2 s a picked
    // note has already decayed most of the way before the swell finishes, which
    // is the deliberate "pad" abuse of the effect.
    float attackMs    = 350.0f;
    // How hard you have to pick to arm a swell: the absolute floor the HF onset
    // envelope must clear. 0 = only a hard pick triggers (-20 dBFS in the HF
    // band), 1 = a light fingered note triggers (-55 dBFS). [0..1]
    float sensitivity = 0.5f;
    // How far the pick attack is ducked. 1 = all the way to silence (the classic
    // slow-gear: the pick is gone entirely), 0 = no ducking at all (the module
    // becomes transparent apart from `position`). The swell floor is 1 - depth,
    // so this doubles as the "how much of the attack survives" blend. [0..1]
    float depth       = 1.0f;
    // The pedal position — the level the swell rises TO, and the whole gain in
    // Volume mode. This is the module's static gain (see the loudness note
    // above). [0..1], 1 = unity.
    float position    = 1.0f;
};

// ---------------------------------------------------------------------------
// SwellCore
// ---------------------------------------------------------------------------
class SwellCore
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels)
    {
        (void)maxBlockSize;
        sr_  = (sampleRate > 0.0) ? sampleRate : 44100.0;
        nch_ = (numChannels > 0) ? numChannels : 1;

        // Onset detector: one HF-highpassed mono sum drives both channels, so a
        // stereo signal never swells lopsidedly.
        hpf_.setHighpass(sr_, kOnsetHpfHz);
        envFast_.prepare(sr_);
        envSlow_.prepare(sr_);
        envFast_.setTimes(kFastAttackMs, kFastReleaseMs);
        envSlow_.setTimes(kSlowAttackMs, kSlowReleaseMs);

        smPos_.reset(sr_, 0.020f);
        smFloor_.reset(sr_, 0.020f);
        smMode_.reset(sr_, 0.020f);

        // A full-scale (1 -> 0) duck takes kDuckMs; the slope is constant, so a
        // duck from a partly-swelled gain is proportionally shorter.
        duckRate_ = 1.0f / (float)std::max(1.0, kDuckMs * 0.001 * sr_);
        lockSamples_ = (int)std::max(1.0, kRetriggerLockMs * 0.001 * sr_);

        applyDerived();
        smPos_.setCurrentAndTarget(clamp01(params_.position));
        smFloor_.setCurrentAndTarget(1.0f - clamp01(params_.depth));
        smMode_.setCurrentAndTarget(mode_ == 0 ? 1.0f : 0.0f);
        // NOT primed: the first setParams() carries the caller's REAL values and
        // must snap to them, not glide 20 ms from these placeholder defaults —
        // otherwise a preset that adds this pedal at position 0.3 slides the
        // level in, and one at depth 0 (transparent) still ducks the first pick.
        // Same rule, same reason as DelayCore's `primed_`.
        primed_ = false;

        reset();
    }

    void reset()
    {
        hpf_.reset();
        envFast_.reset();
        envSlow_.reset();
        // Start OPEN: audio that is already ringing when the pedal is inserted
        // (or when playback resumes) must pass, not be muted until the next pick.
        state_ = State::Sustain;
        gain_  = 1.0f;
        prog_  = 1.0f;
        lock_  = 0;
    }

    void setParams(const SwellParams& p)
    {
        params_ = p;
        applyDerived();

        const float pos   = clamp01(p.position);
        const float floor = 1.0f - clamp01(p.depth);
        const float autoW = (mode_ == 0) ? 1.0f : 0.0f;   // 1 = auto swell, 0 = volume

        if (!primed_)
        {
            smPos_.setCurrentAndTarget(pos);
            smFloor_.setCurrentAndTarget(floor);
            smMode_.setCurrentAndTarget(autoW);
            primed_ = true;
        }
        else
        {
            smPos_.setTarget(pos);
            smFloor_.setTarget(floor);
            smMode_.setTarget(autoW);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames)
    {
        if (channels == nullptr || numChannels <= 0 || numFrames <= 0)
            return;

        const int nc = (numChannels < nch_) ? numChannels : nch_;
        if (nc <= 0)
            return;
        const float invNc = 1.0f / (float)nc;

        for (int i = 0; i < numFrames; ++i)
        {
            const float pos   = smPos_.next();     // static gain (the pedal position)
            const float floor = smFloor_.next();   // 1 - depth
            const float autoW = smMode_.next();    // 1 = auto swell, 0 = plain volume

            // ---- onset detection (HF envelope, ratio test, refractory) ------
            float mono = 0.0f;
            for (int c = 0; c < nc; ++c)
            {
                const float* src = channels[c];
                if (src != nullptr) mono += src[i];
            }
            mono *= invNc;

            const float hf   = hpf_.process(mono);
            const float fast = envFast_.process(hf);
            const float slow = envSlow_.process(hf);

            const bool onset = (lock_ == 0)
                            && (fast > thresh_)               // hard enough to arm
                            && (fast > kOnsetRatio * slow);   // and it OUT-RAN the sustain
            if (lock_ > 0) --lock_;

            if (onset)
            {
                lock_ = lockSamples_;
                // Duck from wherever the gain currently is — that is what makes a
                // re-pick mid-swell continuous instead of a step.
                if (gain_ > floor) state_ = State::Ducking;
                else             { state_ = State::Swelling; prog_ = 0.0f; }
            }

            // ---- swell state machine (always runs, so a mode switch is clean) -
            switch (state_)
            {
                case State::Ducking:
                    gain_ -= duckRate_;
                    if (gain_ <= floor)
                    {
                        gain_  = floor;
                        prog_  = 0.0f;
                        state_ = State::Swelling;
                    }
                    break;

                case State::Swelling:
                    prog_ += progInc_;
                    if (prog_ >= 1.0f)
                    {
                        prog_  = 1.0f;
                        gain_  = 1.0f;
                        state_ = State::Sustain;
                    }
                    else
                    {
                        // Smoothstep: zero derivative at BOTH ends, so the swell
                        // leaves the floor and lands on unity without a corner.
                        const float t = prog_ * prog_ * (3.0f - 2.0f * prog_);
                        gain_ = floor + (1.0f - floor) * t;
                    }
                    break;

                case State::Sustain:
                default:
                    gain_ = 1.0f;
                    break;
            }

            // Blend the swell factor against unity by the (smoothed) mode, so
            // flipping Auto <-> Volume mid-note glides instead of stepping.
            const float swell = 1.0f + autoW * (gain_ - 1.0f);   // [floor .. 1]
            const float g     = pos * swell;                     // [0 .. position]

            for (int c = 0; c < nc; ++c)
            {
                float* dst = channels[c];
                if (dst == nullptr) continue;
                float out = dst[i] * g;
                if (!std::isfinite(out)) out = 0.0f;
                dst[i] = out;
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    enum class State { Sustain, Ducking, Swelling };

    // Everything derivable from params_ that is NOT a smoothed control.
    void applyDerived()
    {
        mode_ = (params_.mode != 0) ? 1 : 0;

        // Swell progress advances 1/attackSamples per sample. Changing the swell
        // time MID-swell keeps `prog_` and only re-rates it, so the gain trace
        // stays continuous (the knob speeds the fade up or slows it down; it
        // never restarts or jumps it).
        const float ms = clampf(params_.attackMs, kMinAttackMs, kMaxAttackMs);
        const double samples = std::max(1.0, (double)ms * 0.001 * sr_);
        progInc_ = (float)(1.0 / samples);

        // sensitivity 0 -> -20 dBFS (hard pick only), 1 -> -55 dBFS (light touch).
        const float s = clamp01(params_.sensitivity);
        const float thrDb = kThreshHardDb + s * (kThreshSoftDb - kThreshHardDb);
        thresh_ = std::pow(10.0f, thrDb * (1.0f / 20.0f));
    }

    // ---- onset detector ----------------------------------------------------
    // The HF band the pick lives in (see the header note). A one-pole is enough:
    // we only need the pick's energy to dominate the fundamental's, not a brick
    // wall.
    static constexpr double kOnsetHpfHz     = 1200.0;
    static constexpr float  kFastAttackMs   = 0.5f;   // sees the pick in ~1-2 ms
    static constexpr float  kFastReleaseMs  = 40.0f;
    static constexpr float  kSlowAttackMs   = 25.0f;  // lags a new pick on purpose
    static constexpr float  kSlowReleaseMs  = 300.0f;
    // Fire when the fast envelope out-runs the slow one by this much. The
    // measured HF jump for a re-pick over a ringing string is 2.1-4.0x, so 1.8
    // sits under the worst measured case with margin, while a sustaining note
    // (where fast ~= slow) never reaches it.
    static constexpr float  kOnsetRatio     = 1.8f;
    static constexpr float  kThreshHardDb   = -20.0f; // sensitivity 0
    static constexpr float  kThreshSoftDb   = -55.0f; // sensitivity 1
    static constexpr double kRetriggerLockMs = 25.0;  // one pick = one trigger

    // ---- swell -------------------------------------------------------------
    static constexpr double kDuckMs      = 3.0;    // full-scale duck; a fade, not a step
    static constexpr float  kMinAttackMs = 5.0f;
    static constexpr float  kMaxAttackMs = 2000.0f;

    static float clampf(float v, float lo, float hi)
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }
    static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

    double sr_  = 44100.0;
    int    nch_ = 1;
    SwellParams params_{};

    // detector
    OnePole          hpf_{};
    EnvelopeFollower envFast_{};
    EnvelopeFollower envSlow_{};
    float            thresh_      = 0.0133f;   // sensitivity 0.5 -> -37.5 dBFS
    int              lock_        = 0;
    int              lockSamples_ = 1;

    // swell state
    State state_    = State::Sustain;
    float gain_     = 1.0f;    // the swell factor alone, in [floor .. 1]
    float prog_     = 1.0f;    // swell progress [0..1]
    float progInc_  = 0.0f;
    float duckRate_ = 1.0f;
    int   mode_     = 0;
    bool  primed_   = false;   // see prepare(): the first setParams() SNAPS

    // smoothed controls
    SmoothedValue smPos_{}, smFloor_{}, smMode_{};
};

} // namespace dcdsp
