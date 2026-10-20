// dcdsp_ampface.h — the amp FACE wrapped around a neural capture.
//
// A DETAIL core: it has no catalogue entry of its own and is only instantiated
// by `namamp` (NamAmpModule.cpp). Pure dcdsp — zero framework deps — so
// `tools/tone_response.cpp` measures it with the same describing-function
// instrument as every other stage.
//
// WHAT PROBLEM THIS SOLVES. A NAM/AIDA-X capture is a snapshot of ONE amp at ONE
// knob setting. It has no controls, and there is no way to recover the captured
// amp's tone stack from it. `namamp` therefore exposed exactly two params
// (inputDb / outputDb) and the player got a voice they could not shape. This is
// the honest amp face wrapped AROUND that snapshot:
//
//   Gain      pre-capture drive. The ONE control that genuinely changes the
//             nonlinearity, because a capture's response is a function of the
//             level presented to it — driving it harder is exactly what turning
//             the modelled amp up did.
//   Bass/Mid/Treble   the tone stack (see below).
//   Resonance/Presence  power-amp feel: an LF bump at the driver-resonance
//             region and an HF shelf, the two things a real amp's negative-
//             feedback loop lets you move.
//   Master    post level.
//
// WHERE THE TONE STACK SITS, AND WHY. In a real amp the stack is between the
// preamp and the power amp. A capture is a black box that already contains BOTH
// — there is no seam to insert into, and pretending otherwise would be a lie
// about what the model is. The stack therefore runs POST-capture, where it
// behaves like the amp's own post-EQ / an EQ in the effects loop. Putting it
// pre-capture was considered and rejected: it would not reproduce a stack that
// sits inside the black box, it would just re-spectrum the drive feeding a
// nonlinearity that already has a stack of its own baked in — a bass boost would
// come back as flub, not as low end. Gain is pre-capture for the opposite
// reason: level into the model is the one thing the capture is genuinely a
// function of.
//
// NEUTRAL IS A STRICT NO-OP. Every control's default is 0.5 and every filter is
// SKIPPED (not run at 0 dB) at its neutral value, so existing presets and
// captures stay bit-identical. `isNeutral()` reports the whole face bypassed.
// Same rule as `dcdsp_chorus.h`'s `wetHpfHz 20 == the filter BYPASSED`.
// Honest footnote, because it was measured rather than assumed: an RBJ section
// at 0 dB IS already a bit-exact identity in this TDF-II form — A == 1 makes
// b1 == a1 and b2 == a2 after the a0 normalisation, so the state variables stay
// exactly zero and y == x in float. The skip is therefore not what MAKES neutral
// bit-identical; it is what makes it free (five biquads per sample for nothing)
// and what keeps the guarantee structural instead of contingent on that
// cancellation surviving a future coefficient change.
//
// DO NOT DOUBLE UP WITH THE CAB. `dcdsp_cab.h`'s `kResonance` (0.35 low-end
// pass) restores the driver Fs bump the generated IR bank has no trace of, and
// it is ALWAYS on inside the cab's wet path. This module's Resonance is a
// separate, player-facing control that is FLAT at its default, so a stock rig
// gets the cab's bump exactly once. Turning it up stacks with the cab on
// purpose; that is what the knob is for.
#ifndef DCDSP_AMPFACE_H
#define DCDSP_AMPFACE_H

#include "dcdsp_primitives.h"

#include <algorithm>
#include <cmath>

namespace dcdsp {

struct AmpFaceParams {
    float gain      = 0.5f;  // pre-capture drive; 0.5 = 0 dB
    float bass      = 0.5f;  // low shelf;    0.5 = flat
    float mid       = 0.5f;  // mid peak;     0.5 = flat
    float treble    = 0.5f;  // high shelf;   0.5 = flat
    float presence  = 0.5f;  // top shelf;    0.5 = flat
    float resonance = 0.5f;  // LF peak;      0.5 = flat
    float master    = 0.5f;  // post level;   0.5 = unity
};

class AmpFace {
public:
    // Filter geometry. Chosen so the four EQ bands overlap the way a real amp's
    // controls do without any two of them being the same filter:
    //   bass       low shelf  @ 120 Hz  Q 0.70  +/-12 dB
    //   mid        peak       @ 650 Hz  Q 0.80  +/-12 dB
    //   treble     high shelf @ 2.2 kHz Q 0.70  +/-12 dB
    //   presence   high shelf @ 5.0 kHz Q 0.70  +/- 9 dB
    //   resonance  peak       @  95 Hz  Q 1.10  +/- 9 dB
    static constexpr double kBassHz      = 120.0,  kBassQ      = 0.70;
    static constexpr double kMidHz       = 650.0,  kMidQ       = 0.80;
    static constexpr double kTrebleHz    = 2200.0, kTrebleQ    = 0.70;
    static constexpr double kPresenceHz  = 5000.0, kPresenceQ  = 0.70;
    static constexpr double kResonanceHz =   95.0, kResonanceQ = 1.10;

    static constexpr float kEqRangeDb       = 12.0f; // bass / mid / treble
    static constexpr float kShelfRangeDb    =  9.0f; // presence / resonance
    static constexpr float kGainRangeDb     = 18.0f; // pre-capture drive

    void prepare(double sampleRate, int /*maxBlock*/, int /*numChannels*/)
    {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        preSmooth_.reset(sr_, kGainRampSeconds);
        postSmooth_.reset(sr_, kGainRampSeconds);
        setParams(p_);
        // A freshly prepared face starts AT its knobs, not ramping to them.
        preSmooth_.snapToTarget();
        postSmooth_.snapToTarget();
        reset();
    }

    void reset()
    {
        bass_.reset(); mid_.reset(); treble_.reset(); presence_.reset(); resonance_.reset();
    }

    /// AUDIO THREAD ONLY (via the adapter's ParamMailbox) — it recomputes
    /// multi-word biquad coefficients in place.
    void setParams(const AmpFaceParams& p)
    {
        p_ = p;

        preGain_ = knobToLin(p.gain, kGainRangeDb);
        // Audio taper: lin = (2*m)^2. Exactly 1.0 at the 0.5 default, silence at
        // 0, +12.04 dB at 1.0 — a volume control that can actually reach zero,
        // which a symmetric dB knob cannot.
        const float m = clamp01(p.master) * 2.0f;
        postGain_ = m * m;
        // Both gains RAMP to their new value (kGainRampSeconds): the master
        // was a per-block constant, so a slider drag zippered and a preset
        // switch stepped. preGain() hands the ramp to the adapter per sample.
        preSmooth_.setTarget(preGain_);
        postSmooth_.setTarget(postGain_);

        bassDb_      = knobToDb(p.bass,      kEqRangeDb);
        midDb_       = knobToDb(p.mid,       kEqRangeDb);
        trebleDb_    = knobToDb(p.treble,    kEqRangeDb);
        presenceDb_  = knobToDb(p.presence,  kShelfRangeDb);
        resonanceDb_ = knobToDb(p.resonance, kShelfRangeDb);

        bassOn_      = arm(bass_,      bassDb_,      kBassHz,      kBassQ,      Shape::LowShelf);
        midOn_       = arm(mid_,       midDb_,       kMidHz,       kMidQ,       Shape::Peak);
        trebleOn_    = arm(treble_,    trebleDb_,    kTrebleHz,    kTrebleQ,    Shape::HighShelf);
        presenceOn_  = arm(presence_,  presenceDb_,  kPresenceHz,  kPresenceQ,  Shape::HighShelf);
        resonanceOn_ = arm(resonance_, resonanceDb_, kResonanceHz, kResonanceQ, Shape::Peak);

        neutral_ = !bassOn_ && !midOn_ && !trebleOn_ && !presenceOn_ && !resonanceOn_
                   && preGain_ == 1.0f && postGain_ == 1.0f;
    }

    /// Linear pre-capture drive TARGET. Exactly 1.0f at the neutral default.
    float preGain() const { return preGain_; }

    /// AUDIO THREAD. The pre-capture drive for the NEXT sample, walking the
    /// ramp; call once per sample in order.
    float nextPreGain() { return preSmooth_.next(); }

    /// True when nothing in the face would change a single sample — which
    /// includes "no gain ramp still in flight".
    bool isNeutral() const { return neutral_ && ! preSmooth_.isRamping() && ! postSmooth_.isRamping(); }

    /// AUDIO THREAD. Post-capture half of the face, in place on one mono run:
    /// tone stack -> resonance -> presence -> master. Alloc-free. A band at its
    /// neutral value is SKIPPED, not run at 0 dB.
    void processPost(float* x, int n)
    {
        if (x == nullptr || n <= 0) return;
        if (bassOn_)      for (int i = 0; i < n; ++i) x[i] = bass_.process(x[i]);
        if (midOn_)       for (int i = 0; i < n; ++i) x[i] = mid_.process(x[i]);
        if (trebleOn_)    for (int i = 0; i < n; ++i) x[i] = treble_.process(x[i]);
        if (resonanceOn_) for (int i = 0; i < n; ++i) x[i] = resonance_.process(x[i]);
        if (presenceOn_)  for (int i = 0; i < n; ++i) x[i] = presence_.process(x[i]);
        if (postSmooth_.isRamping())
        {
            for (int i = 0; i < n; ++i) x[i] *= postSmooth_.next();
        }
        else if (postGain_ != 1.0f)
        {
            for (int i = 0; i < n; ++i) x[i] *= postGain_;
        }
    }

    int latencySamples() const { return 0; }

private:
    enum class Shape { LowShelf, Peak, HighShelf };

    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    /// Knob 0..1 -> +/-rangeDb, exactly 0 at 0.5.
    static float knobToDb(float v, float rangeDb) { return (clamp01(v) - 0.5f) * 2.0f * rangeDb; }

    static float knobToLin(float v, float rangeDb)
    {
        const float db = knobToDb(v, rangeDb);
        return db == 0.0f ? 1.0f : std::pow(10.0f, db / 20.0f);
    }

    /// Design the section and report whether it is worth running at all. Under
    /// kDeadZoneDb the section is skipped, which is what keeps the neutral face
    /// bit-identical to the pre-face signal path.
    bool arm(Biquad& f, float gainDb, double hz, double q, Shape shape)
    {
        if (std::fabs(gainDb) < kDeadZoneDb) return false;
        switch (shape)
        {
            case Shape::LowShelf:  f.setLowShelf (sr_, hz, q, gainDb); break;
            case Shape::Peak:      f.setPeaking  (sr_, hz, q, gainDb); break;
            case Shape::HighShelf: f.setHighShelf(sr_, hz, q, gainDb); break;
        }
        return true;
    }

    static constexpr float kDeadZoneDb = 1.0e-4f;

    AmpFaceParams p_{};
    double sr_ = 48000.0;

    Biquad bass_, mid_, treble_, presence_, resonance_;
    bool   bassOn_ = false, midOn_ = false, trebleOn_ = false;
    bool   presenceOn_ = false, resonanceOn_ = false;
    bool   neutral_ = true;

    float preGain_  = 1.0f;
    float postGain_ = 1.0f;
    static constexpr float kGainRampSeconds = 0.010f;
    SmoothedValue preSmooth_;
    SmoothedValue postSmooth_;
    float bassDb_ = 0.0f, midDb_ = 0.0f, trebleDb_ = 0.0f;
    float presenceDb_ = 0.0f, resonanceDb_ = 0.0f;
};

} // namespace dcdsp

#endif // DCDSP_AMPFACE_H
