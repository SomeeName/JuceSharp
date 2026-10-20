// dcvoice_instrument.h — StringInstrument: a guitar (6/7 strings) or bass
// (4/5 strings) built from StringModel lanes, driven by NOTE EVENTS. The
// timeline player (dcvoice_timeline.h) and the engine's chart source (issue
// #39 step 0, through dcvoice_chartvoice.h) are two front ends over this one
// core; neither does any DSP of its own.
#pragma once

#include "dcvoice_excitation.h"
#include "dcvoice_string.h"
#include "dcvoice_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dcvoice {

/// The physical instrument. Values are seconds / Hz unless stated.
struct InstrumentSpec
{
    int numStrings = 6;
    std::array<double, 8> openHz {};          // index 0 = lowest string
    std::array<double, 8> stiffnessB {};      // inharmonicity at the open string
    int dispersionStages = 2;
    // Decay profile for a note with no recorded clip (synthetic excitation).
    // Fitted to the guitar library's own measurements (extract.log medians):
    // T60(f0) = 14.7 s * (82.4 / f0)^0.52, ~1 s plateau above ~4 f0.
    double t60Low = 14.7;         // decay of the fundamental at t60RefHz
    double t60RefHz = 82.4;
    double t60Exponent = 0.52;    // T60(f0) = t60Low * (t60RefHz / f0)^exp
    double t60High = 1.0;         // plateau
    double cornerMul = 4.0;       // shelf corner = max(cornerMul * f0, cornerMinHz)
    double cornerMinHz = 400.0;
    double palmT60Low = 0.22;
    double palmT60High = 0.03;
    double deadT60 = 0.025;
    double releaseT60 = 0.07;
    double outputGain = 0.45;   // a picked mf note peaks near -12 dBFS DI
    bool bass = false;
    ExcKind defaultKind = ExcKind::Pick;
    // Synthetic pluck geometry (fractions of the string from the bridge).
    double pickPos = 0.16;
    double pickupPos = 0.11;
    double resonanceHz = 4200.0;
    int maxFret = 30;
    int capo = 0;                 // capo fret (0 = none); fret 0 sounds here, harmonic nodes count from it

    static InstrumentSpec guitar()
    {
        InstrumentSpec s;
        s.numStrings = 6;
        const double midi[6] = { 40, 45, 50, 55, 59, 64 };
        for (int i = 0; i < 6; ++i)
            s.openHz[(size_t) i] = 440.0 * std::pow(2.0, (midi[i] - 69.0) / 12.0);
        // Plain-steel B from the textbook formula for a 10-46 set at 25.5";
        // the wound strings' cores are thinner than their outer diameter.
        const double b[6] = { 5.0e-5, 4.0e-5, 3.0e-5, 8.0e-5, 4.0e-5, 1.3e-5 };
        for (int i = 0; i < 6; ++i)
            s.stiffnessB[(size_t) i] = b[i];
        return s;
    }

    static InstrumentSpec bassGuitar(int strings = 4)
    {
        InstrumentSpec s;
        s.bass = true;
        s.numStrings = strings == 5 ? 5 : 4;
        const double midi4[4] = { 28, 33, 38, 43 };
        const double midi5[5] = { 23, 28, 33, 38, 43 };
        const double b4[4] = { 1.6e-4, 1.3e-4, 1.1e-4, 9.0e-5 };
        const double b5[5] = { 2.4e-4, 1.6e-4, 1.3e-4, 1.1e-4, 9.0e-5 };
        for (int i = 0; i < s.numStrings; ++i)
        {
            const double m = s.numStrings == 5 ? midi5[i] : midi4[i];
            s.openHz[(size_t) i] = 440.0 * std::pow(2.0, (m - 69.0) / 12.0);
            s.stiffnessB[(size_t) i] = s.numStrings == 5 ? b5[i] : b4[i];
        }
        s.dispersionStages = 4;
        s.t60Low = 18.0;
        s.t60RefHz = 41.2;
        s.t60Exponent = 0.35;
        s.t60High = 2.2;
        s.cornerMul = 5.0;
        s.cornerMinHz = 250.0;
        s.palmT60Low = 0.30;
        s.palmT60High = 0.04;
        s.deadT60 = 0.04;
        s.releaseT60 = 0.09;
        s.defaultKind = ExcKind::Finger;
        s.pickPos = 0.22;
        s.pickupPos = 0.14;
        s.resonanceHz = 3200.0;
        s.maxFret = 24;
        return s;
    }

    /// Re-tunes the strings by semitone offsets from the spec's standard
    /// tuning (the chart's TuningPitches), with an optional A4 reference.
    void applyTuningOffsets(const int* offsets, int count, double a4Hz = 440.0)
    {
        for (int i = 0; i < numStrings && i < count; ++i)
            openHz[(size_t) i] *= std::pow(2.0, offsets[i] / 12.0);
        if (a4Hz > 0.0 && std::fabs(a4Hz - 440.0) > 1.0e-6)
            for (int i = 0; i < numStrings; ++i)
                openHz[(size_t) i] *= a4Hz / 440.0;
    }
};

/// Natural-harmonic number for a touch at `fret` (12 -> 2, 7 -> 3, 5 -> 4,
/// 4/9/16 -> 5, 3 -> 6, ...): the smallest k with a node j/k within 1.2 % of
/// the string length of the touch point, else the nearest node up to k = 10.
inline int harmonicNumberForFret(double fret)
{
    const double x = 1.0 - std::pow(2.0, -fret / 12.0);
    for (int k = 2; k <= StringModel::kMaxNodeTaps; ++k)
        for (int j = 1; j < k; ++j)
            if (std::fabs(x - (double) j / k) < 0.012)
                return k;
    int bestK = 2;
    double best = 1.0;
    for (int k = 2; k <= StringModel::kMaxNodeTaps; ++k)
        for (int j = 1; j < k; ++j)
            if (std::fabs(x - (double) j / k) < best)
            {
                best = std::fabs(x - (double) j / k);
                bestK = k;
            }
    return bestK;
}

/// Deterministic per-event jitter in [-1, 1).
inline double hashUnit(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (double) (h >> 8) * (2.0 / 16777216.0) - 1.0;
}

/// One note as the core starts it. Pitch is a MIDI note number (A4 = 69),
/// fractional allowed, of the UNBENT fretted string — tuning and capo are in
/// it. `fret` is the charted fret (it picks the natural-harmonic node and the
/// hammer position); the open string is therefore pitch - fret.
struct NoteStart
{
    int32_t id = -1;
    int32_t string = 0;
    int32_t fret = 0;
    int32_t capo = 0;            // the capo fret (0 = none): the vibrating string starts THERE
    double pitch = 40.0;
    uint32_t flags = 0;          // VoiceNoteFlags
    float velocity = 0.7f;
    float vibratoSemis = 0.0f;   // 0 = none
    float slideToSemis = 0.0f;   // where a slide ends, relative (0 = no slide)
    float slideInSemis = 0.0f;   // slide INTO the note from this offset (0 = none)
    float initialBend = 0.0f;    // bend at the attack (pre-bend, or mid-bend when chased)
    float initialSlide = 0.0f;   // slide progress at the attack (chase only)
    int32_t delaySamples = 0;    // strum offset: the attack happens this many samples later
    float speed = 1.0f;          // song s per wall s (tremolo rate follows practice speed)
    double chaseSeconds = 0.0;   // > 0: enter mid-sustain, no pick attack
};

/// The open pitch of the VIBRATING string: with a capo the string vibrates from
/// the capo, not the nut. `pitch` is the charted fret's pitch counted from the
/// nut, so fret f > 0 sounds at open + f and fret 0 sounds at the capo.
inline double vibratingOpenPitch(const NoteStart& n) noexcept
{
    return n.fret > 0 ? n.pitch - n.fret + n.capo : n.pitch;
}

/// The natural-harmonic number of a touch at the charted fret: its node is
/// counted from the CAPO (capo 2 + charted fret 14 = the octave, k = 2).
inline int naturalHarmonicNumber(const NoteStart& n) noexcept
{
    const int fromCapo = n.fret - std::max(n.capo, 0);
    return fromCapo > 0 ? harmonicNumberForFret(fromCapo) : 2;
}

/// The instrument CORE a front end drives: the waveguide StringInstrument and
/// the sample-based SampleInstrument (dcvoice_sampler.h) both implement it, so
/// the timeline player and the engine adapter never know which one plays.
///
///   prepare()            control thread, allocates; leaves the voice silent
///   everything else      audio thread: no allocation, locks, I/O or throws
class IVoiceCore
{
public:
    static constexpr int kMaxStrings = 8;

    virtual ~IVoiceCore() = default;
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void reset() noexcept = 0;
    virtual void noteOn(const NoteStart& n) noexcept = 0;
    /// Release: the fret hand lets go (tens of ms, natural). Choke: <= 5 ms.
    /// A note the core already replaced is ignored.
    virtual void noteOff(int32_t id, bool choke) noexcept = 0;
    virtual void allNotesOff(bool choke) noexcept = 0;
    /// Bend and slide (semitones, relative to the note's pitch) the note should
    /// reach `rampSamples` from now; the core glides there.
    virtual void setControls(int32_t id, float bendSemis, float slideSemis, int rampSamples) noexcept = 0;
    /// ADDS numSamples of DI into out. Any block size.
    virtual void render(float* out, int numSamples) noexcept = 0;
    virtual int activeNotes() const noexcept = 0;
    virtual bool stringActive(int s) const noexcept = 0;
    virtual int32_t stringNoteId(int s) const noexcept = 0;
    virtual int64_t samplesRendered() const noexcept = 0;
    virtual double sampleRate() const noexcept = 0;
    virtual const InstrumentSpec& spec() const noexcept = 0;

    /// MIDI pitch of string s at the charted fret (counted from the nut) in the
    /// spec's tuning; fret 0 sounds at the spec's capo.
    double pitchOf(int s, double fret) const noexcept
    {
        return 69.0 + 12.0 * std::log2(spec().openHz[(size_t) std::clamp(s, 0, kMaxStrings - 1)] / 440.0)
               + (fret <= 0.0 ? (double) spec().capo : fret);
    }
};

/// The instrument CORE: one StringModel lane per string, driven by events.
/// This is the shape of issue #39 step 0's dcsynth::ChartVoice — noteOn /
/// noteOff(release|choke) / per-note bend+slide controls / render(add) — so
/// the engine adapter is a field-for-field mapping (see dcvoice_chartvoice.h).
///
///   prepare()            control thread, allocates; leaves the voice silent
///   everything else      audio thread: no allocation, locks, I/O or throws
///
/// One lane per string, like the instrument: a note on a string that is still
/// sounding REPLACES it (the pick stops the string, then plucks it), and a
/// hammer-on / pull-off / tap / linked note moves the pitch of the vibration
/// that is already there instead of starting a new one.
///
/// ARTICULATIONS (flags -> what the string does):
///   picked note        pickStop + recorded residual (synthetic pluck fallback);
///                      an up-stroke inverts the residual
///   palm mute          T60 ~0.2 s, top end ~30 ms, residual low-passed
///   full mute / dead   T60 ~25 ms + the recorded dead-string "chk" (guitar) or
///                      ghost pluck (bass)
///   hammer-on / tap    pitch glide ~1 ms, no pick stop, a small synthetic thump
///                      at the fret; onto a silent string it makes the sound
///   pull-off           pitch glide + a soft finger pluck
///   linked             pitch only, no attack
///   natural harmonic   the loop stays the OPEN string; a node damper at 1/k
///                      (12->2, 7/19->3, 5/24->4, ...) held for the excitation
///   pinch harmonic     partial node touch, k chosen so k*f0 ~ 1.4 kHz
///   bend / slide       per-note controls (semitones); slides are QUANTISED to
///                      the fret under the finger — a real slide is a fast
///                      chromatic glissando — with a fret click per fret
///   unpitched slide    the level fades with slide progress
///   slide in           glide in from slideInSemis over ~60 ms
///   vibrato            5.5 Hz; guitar bends UP (0..depth), bass symmetric
///   tremolo            kFlagTremolo: the core re-picks at 14 Hz x speed
///   accent / ghost     level and velocity layer
///   slap / pop (bass)  synthetic thumb / finger snap + string-fret collision
class StringInstrument final : public IVoiceCore
{
public:
    static constexpr int kControlInterval = 16;
    StringInstrument() = default;
    explicit StringInstrument(const InstrumentSpec& spec, const ExcitationBank& bank = {}) { configure(spec, bank); }

    /// Control thread, never concurrent with render(). Call prepare() after.
    void configure(const InstrumentSpec& spec, const ExcitationBank& bank)
    {
        spec_ = spec;
        spec_.numStrings = std::clamp(spec_.numStrings, 1, kMaxStrings);
        bank_ = bank;
        if (fs_ > 0.0)
            buildTables();
    }

    const InstrumentSpec& spec() const noexcept override { return spec_; }
    const ExcitationBank& bank() const noexcept { return bank_; }
    double sampleRate() const noexcept override { return fs_; }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        (void) maxBlockSize;
        // Delay lines for the lowest note anyone can ask for: a bass low B
        // tuned down a fourth and bent/slid down a few semitones (18 Hz).
        const int maxExc = (int) (0.16 * fs_) + 64;
        for (int s = 0; s < kMaxStrings; ++s)
            lanes_[(size_t) s].model.prepare(fs_, 18.0, spec_.dispersionStages, maxExc);
        scratch_.assign((size_t) maxExc, 0.0f);
        buildTables();
        dcCoef_ = std::exp(-kTwoPi * 12.0 / fs_);
        reset();
    }

    /// Everything to zero, immediately, and every note forgotten.
    void reset() noexcept override
    {
        for (auto& l : lanes_)
        {
            l.model.reset();
            l.state = LaneState::Idle;
            l.id = -1;
            l.quietSamples = 0;
            l.pending = false;
            l.choke = 0;
            l.choking = false;
            l.rr = 0;   // round robin restarts: the same song renders the same samples
        }
        dcX_ = dcY_ = 0.0;
        ctrlPhase_ = 0;
        eventCounter_ = 0;
        samples_ = 0;
    }

    void noteOn(const NoteStart& n) noexcept override
    {
        if (n.string < 0 || n.string >= spec_.numStrings || ! std::isfinite(n.pitch) || fs_ <= 0.0)
            return;
        Lane& l = lanes_[(size_t) n.string];
        if (n.delaySamples > 0)
        {
            // A strummed string waits for the pick to reach it; whatever it
            // was doing goes on until then.
            l.pendingNote = n;
            l.pendingNote.delaySamples = 0;
            l.pendingLeft = n.delaySamples;
            l.pending = true;
            return;
        }
        startNow(l, n.string, n);
    }

    /// Release: the fret hand lets go (tens of ms). Choke: <= 5 ms, no click.
    /// A note the core already replaced is ignored.
    void noteOff(int32_t id, bool choke) noexcept override
    {
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            Lane& l = lanes_[(size_t) s];
            if (l.pending && l.pendingNote.id == id)
            {
                l.pending = false;   // never started: nothing to end
                continue;
            }
            if (l.id != id || l.state == LaneState::Idle)
                continue;
            endLane(l, choke || (l.note.flags & kFlagEndDamped) != 0);
        }
    }

    void allNotesOff(bool choke) noexcept override
    {
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            Lane& l = lanes_[(size_t) s];
            l.pending = false;
            if (l.state != LaneState::Idle)
                endLane(l, choke);
        }
    }

    /// Bend and slide (semitones, relative to the note's pitch) the note
    /// should reach `rampSamples` from now; the core glides there.
    void setControls(int32_t id, float bendSemis, float slideSemis, int rampSamples) noexcept override
    {
        if (! std::isfinite(bendSemis) || ! std::isfinite(slideSemis))
            return;
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            Lane& l = lanes_[(size_t) s];
            if (l.pending && l.pendingNote.id == id)
            {
                l.pendingNote.initialBend = bendSemis;
                l.pendingNote.initialSlide = slideSemis;
            }
            if (l.id != id || l.state == LaneState::Idle)
                continue;
            const int steps = std::max(1, (rampSamples + kControlInterval - 1) / kControlInterval);
            l.bendStep = (bendSemis - l.bend) / steps;
            l.slideStep = (slideSemis - l.slide) / steps;
            l.bendTarget = bendSemis;
            l.slideTarget = slideSemis;
            l.ctlSteps = steps;
        }
    }

    /// ADDS numSamples of DI into out. Any block size.
    void render(float* out, int numSamples) noexcept override
    {
        if (out == nullptr || numSamples <= 0 || fs_ <= 0.0)
            return;
        int i = 0;
        while (i < numSamples)
        {
            if (ctrlPhase_ == 0)
                controlStep();
            // Strummed attacks land on their exact sample: start what is due,
            // and cut the segment at the next one.
            int end = std::min(numSamples, i + (kControlInterval - ctrlPhase_));
            for (int s = 0; s < spec_.numStrings; ++s)
            {
                Lane& l = lanes_[(size_t) s];
                if (! l.pending)
                    continue;
                if (l.pendingLeft <= 0)
                {
                    l.pending = false;
                    startNow(l, s, l.pendingNote);
                }
                else
                    end = std::min(end, i + l.pendingLeft);
            }
            const int n = end - i;
            float* dst = scratchOut_;
            std::fill(dst, dst + n, 0.0f);
            for (int s = 0; s < spec_.numStrings; ++s)
            {
                Lane& l = lanes_[(size_t) s];
                if (l.state == LaneState::Idle)
                    continue;
                float g = l.level;
                if (l.choking)
                {
                    g *= (float) l.choke / (float) chokeSamples();
                    l.choke = std::max(0, l.choke - n);
                }
                l.model.render(dst, n, g);
                l.samplesSinceOn += n;
            }
            for (int s = 0; s < spec_.numStrings; ++s)
                if (lanes_[(size_t) s].pending)
                    lanes_[(size_t) s].pendingLeft -= n;
            for (int k = 0; k < n; ++k)
            {
                const double x = dst[k] * spec_.outputGain;
                dcY_ = x - dcX_ + dcCoef_ * dcY_;
                dcX_ = x;
                float y = (float) dcY_;
                if (! std::isfinite(y))
                {
                    y = 0.0f;
                    nonFinite_ = true;
                }
                out[i + k] += y;
            }
            ctrlPhase_ = (ctrlPhase_ + n) % kControlInterval;
            samples_ += n;
            if (ctrlPhase_ == 0)
                retireQuietLanes();
            i = end;
        }
        if (std::fabs(dcY_) < 1.0e-20)
            dcY_ = 0.0;
        if (nonFinite_)
        {
            for (auto& l : lanes_)
            {
                l.model.reset();
                l.state = LaneState::Idle;
                l.id = -1;
            }
            dcX_ = dcY_ = 0.0;
            nonFinite_ = false;
        }
    }

    int activeNotes() const noexcept override
    {
        int n = 0;
        for (int s = 0; s < spec_.numStrings; ++s)
            n += (lanes_[(size_t) s].state != LaneState::Idle || lanes_[(size_t) s].pending) ? 1 : 0;
        return n;
    }
    bool stringActive(int s) const noexcept override { return s >= 0 && s < kMaxStrings && lanes_[(size_t) s].state != LaneState::Idle; }
    int32_t stringNoteId(int s) const noexcept override { return stringActive(s) ? lanes_[(size_t) s].id : -1; }
    double stringFrequency(int s) const noexcept { return lanes_[(size_t) s].freq; }
    int64_t samplesRendered() const noexcept override { return samples_; }


private:
    enum class LaneState : uint8_t { Idle, Ringing, Released };

    struct Lane
    {
        StringModel model;
        LaneState state = LaneState::Idle;
        int32_t id = -1;
        NoteStart note {};
        // pending strummed attack
        bool pending = false;
        NoteStart pendingNote {};
        int pendingLeft = 0;
        // controls
        double base = 40.0;          // smoothed fretted pitch (glides on legato)
        double baseTarget = 40.0;
        double baseGlide = 0.0015;   // s
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;
        float slide = 0.0f, slideTarget = 0.0f, slideStep = 0.0f;
        int ctlSteps = 0;
        double lastQuant = 0.0;
        double freq = 100.0;
        int64_t samplesSinceOn = 0;
        int harmonicK = 1;
        int quietSamples = 0;
        bool choking = false;
        int choke = 0;
        float level = 1.0f;
        uint32_t rr = 0;
        int tremoloLeft = 0;
        int tremoloCount = 0;
        // decay profile (see lossFor)
        double refHz = 100.0;
        double t60Low = 5.0;
        double t60High = 1.0;
        double cornerHz = 400.0;
        double apA = 0.0;
    };

    int chokeSamples() const noexcept { return std::max(8, (int) (0.004 * fs_)); }

    void endLane(Lane& l, bool choke) noexcept
    {
        if (choke && ! l.choking)
        {
            l.choking = true;
            l.choke = chokeSamples();
        }
        l.state = LaneState::Released;
    }

    void buildTables()
    {
        // Dispersion coefficient per string and fret, fitted once (control thread).
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            for (int f = 0; f <= kTableFrets; ++f)
            {
                const double f0 = spec_.openHz[(size_t) s] * std::pow(2.0, f / 12.0);
                // B scales with 1/L^2 at constant tension.
                const double B = spec_.stiffnessB[(size_t) s] * std::pow(2.0, f / 6.0);
                const LossCoeffs loss = designLoss(f0, fs_, t60For(f0), spec_.t60High, cornerFor(f0));
                const double period = fs_ / f0;
                apTable_[(size_t) s][(size_t) f] =
                    (float) fitDispersion(f0, fs_, B, spec_.dispersionStages, loss, 0.45 * period - 2.0);
            }
        }
        // ...and per recorded clip, at the clip's own pitch and stiffness.
        clipAp_.assign((size_t) std::max(bank_.count, 0), 0.0f);
        for (int i = 0; i < bank_.count; ++i)
        {
            const ExcitationClip& c = bank_.clips[i];
            const double f0 = c.f0Hz;
            const LossCoeffs loss = designLoss(f0, fs_, c.t60Low, c.t60High, c.cornerHz);
            clipAp_[(size_t) i] = (float) fitDispersion(f0, fs_, c.stiffness, spec_.dispersionStages, loss,
                                                        0.45 * fs_ / f0 - 2.0);
        }
    }

    double t60For(double f0) const noexcept
    {
        return spec_.t60Low * std::pow(spec_.t60RefHz / std::max(f0, 1.0), spec_.t60Exponent);
    }

    double cornerFor(double f0) const noexcept { return std::max(spec_.cornerMul * f0, spec_.cornerMinHz); }

    float apFor(int s, double f0) const noexcept
    {
        const double fp = std::clamp(12.0 * std::log2(std::max(f0, 1.0) / spec_.openHz[(size_t) s]), 0.0, (double) kTableFrets);
        const int i = std::min((int) fp, kTableFrets - 1);
        const double fr = fp - i;
        const auto& t = apTable_[(size_t) s];
        return (float) (t[(size_t) i] + fr * (t[(size_t) i + 1] - t[(size_t) i]));
    }

    void specProfile(Lane& l, int s, double f0) const noexcept
    {
        l.refHz = f0;
        l.t60Low = t60For(f0);
        l.t60High = spec_.t60High;
        l.cornerHz = cornerFor(f0);
        l.apA = apFor(s, f0);
    }

    void clipProfile(Lane& l, const ExcitationClip& c, double f0) const noexcept
    {
        const double r = f0 / c.f0Hz;
        l.refHz = f0;
        l.t60Low = c.t60Low * std::pow(1.0 / r, spec_.t60Exponent);
        l.t60High = c.t60High;
        l.cornerHz = c.cornerHz * r;
        l.apA = clipAp_[(size_t) (&c - bank_.clips)];
    }

    int layerFor(double v) const noexcept
    {
        const int layers = std::max(bank_.layers, 1);
        return std::clamp((int) (v * layers), 0, layers - 1);
    }

    static double hz(double midi) noexcept { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

    static double smoothstep(double x) noexcept
    {
        x = std::clamp(x, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }

    /// The slide, quantised to the fret under the finger (bridge side).
    static double quantisedSlide(double slide) noexcept { return std::ceil(slide - 1.0e-4); }

    // ---- note on ---------------------------------------------------------

    void startNow(Lane& l, int s, const NoteStart& ev) noexcept
    {
        const uint32_t flags = ev.flags;
        const bool wasSounding = l.state != LaneState::Idle && ! l.choking && l.quietSamples < (int) (0.05 * fs_);
        const bool legatoGesture = (flags & (kFlagHammerOn | kFlagPullOff | kFlagTap | kFlagLinked)) != 0;
        const bool legato = legatoGesture && wasSounding && ev.chaseSeconds <= 0.0;
        const bool natural = (flags & kFlagHarmonic) != 0;
        const double open = vibratingOpenPitch(ev);

        l.note = ev;
        l.id = ev.id;
        l.samplesSinceOn = 0;
        l.quietSamples = 0;
        l.choke = 0;
        l.choking = false;
        l.state = LaneState::Ringing;
        l.harmonicK = 1;
        l.level = 1.0f;
        l.bend = l.bendTarget = ev.initialBend;
        l.slide = l.slideTarget = ev.initialSlide;
        l.bendStep = l.slideStep = 0.0f;
        l.ctlSteps = 0;
        l.tremoloCount = 0;
        l.tremoloLeft = (flags & kFlagTremolo) ? tremoloPeriod(ev.speed) : 0;
        ++eventCounter_;

        // Natural harmonics keep the OPEN string vibrating (its loop length).
        l.baseTarget = natural ? open : ev.pitch;
        if (legato)
            l.baseGlide = (flags & kFlagLinked) ? 0.004 : 0.0008;
        else
        {
            l.base = l.baseTarget + ev.slideInSemis;
            l.baseGlide = 0.0015;
        }
        l.lastQuant = quantisedSlide(l.slide);

        const double f0 = hz(currentPitch(l));
        StringModel& m = l.model;

        if ((flags & kFlagLinked) && legato)
        {
            control(s, kControlInterval);
            return;   // a linked continuation has no attack at all
        }

        const double v = std::clamp((double) ev.velocity * (1.0 + 0.05 * hashUnit(eventCounter_, (uint32_t) s)), 0.02, 1.0);
        const bool up = (flags & kFlagUpStroke) != 0;
        const float sign = up ? -1.0f : 1.0f;
        const double velGain = 0.55 + 0.6 * v;
        const int maxLen = m.maxExcitation();

        if (legato)
        {
            // Same vibrating string: its decay profile carries on.
            control(s, kControlInterval);
            if (flags & kFlagPullOff)
                addRecorded(l, defaultKind(flags), f0, 0, (float) (0.45 * velGain) * sign, 0.0, maxLen);
            else
                addThump(l, f0, (flags & kFlagTap) ? 0.55 * velGain : 0.22 * velGain, maxLen, ev.fret);
            return;
        }

        // A new attack: the spec's profile unless a recorded clip brings its own.
        specProfile(l, s, natural ? hz(open) : f0);

        if (ev.chaseSeconds > 0.0)
        {
            // Entering mid-sustain after a seek: no pick, the string is
            // already ringing at roughly the level it would have decayed to.
            const double wall = ev.chaseSeconds / std::max((double) ev.speed, 0.05);
            const double t60 = (flags & kFlagPalmMute) ? spec_.palmT60Low : l.t60Low;
            const double left = std::max(std::pow(10.0, -3.0 * wall / std::max(t60, 0.01)), 0.03);
            if (wasSounding)
                m.pickStop(0.0);
            control(s, 1);
            addSynth(l, f0, velGain * left, 4.0, 0.0, maxLen);
            return;
        }

        double gain = velGain;
        if (flags & kFlagAccent)
            gain *= 1.12;
        if (flags & kFlagGhost)
            gain *= 0.45;
        if (legatoGesture)   // hammer / pull / tap onto a silent string makes the sound itself
            gain *= (flags & kFlagTap) ? 0.85 : 0.6;
        if (natural)
            gain *= 1.3;
        if (flags & kFlagPinchHarmonic)
            gain *= 1.5;

        if ((flags & (kFlagSlap | kFlagPop)) != 0)
        {
            addSlapPop(l, f0, (flags & kFlagPop) != 0, gain, maxLen);
        }
        else if (flags & kFlagFullMute)
        {
            const ExcKind k = spec_.bass ? ExcKind::Ghost : ExcKind::Dead;
            if (! addRecorded(l, k, f0, layerFor(v), (float) (0.9 * gain) * sign, 0.0, maxLen))
                addSynth(l, f0, 0.6 * gain * sign, 0.4, 1.2, maxLen);
        }
        else
        {
            const int layer = layerFor(std::min(1.0, v + ((flags & kFlagAccent) ? 0.2 : 0.0)));
            const double lp = (flags & kFlagPalmMute) ? (spec_.bass ? 900.0 : 1600.0) : 0.0;
            const double pm = (flags & kFlagPalmMute) ? 0.85 : 1.0;
            const ExcitationClip* c = addRecorded(l, defaultKind(flags), f0, layer, (float) (gain * pm) * sign, lp, maxLen);
            if (c != nullptr)
                clipProfile(l, *c, natural ? hz(open) : f0);
            else
                addSynth(l, f0, gain * pm * sign, (flags & kFlagPalmMute) ? 1.2 : 0.5, 0.15, maxLen);
        }

        control(s, 1);
        if (wasSounding)
            m.pickStop(0.12);

        if (natural)
        {
            l.harmonicK = naturalHarmonicNumber(ev);
            const double period = fs_ / hz(open);
            m.touchNode(l.harmonicK, 1.0, std::max((int) (3.0 * period), m.excitationRemaining()), (int) (0.012 * fs_));
        }
        else if (flags & kFlagPinchHarmonic)
        {
            // The thumb stays on for the length of the pick's residual (or the
            // excitation re-feeds every mode as it lifts) but only partly
            // damps: the fundamental is still heard under the squeal.
            l.harmonicK = std::clamp((int) std::lround(1400.0 / f0), 3, 6);
            const double period = fs_ / f0;
            m.touchNode(l.harmonicK, 0.9, std::max((int) (2.5 * period), m.excitationRemaining()), (int) (0.010 * fs_));
        }
    }

    ExcKind defaultKind(uint32_t flags) const noexcept
    {
        if (spec_.bass && (flags & kFlagPicked))
            return ExcKind::Pick;
        return spec_.defaultKind;
    }

    int tremoloPeriod(float speed) const noexcept
    {
        return std::max(16, (int) (fs_ / (14.0 * std::clamp((double) speed, 0.25, 2.0))));
    }

    void tremoloRepick(Lane& l, int s) noexcept
    {
        const uint32_t up = (++l.tremoloCount % 2) ? kFlagUpStroke : 0u;
        const double f0 = l.freq;
        const double v = std::clamp((double) l.note.velocity * (up ? 0.88 : 1.0)
                                        * (1.0 + 0.05 * hashUnit(eventCounter_++, (uint32_t) s)), 0.02, 1.0);
        const float sign = up ? -1.0f : 1.0f;
        const double pm = (l.note.flags & kFlagPalmMute) ? 0.85 : 1.0;
        const double lp = (l.note.flags & kFlagPalmMute) ? (spec_.bass ? 900.0 : 1600.0) : 0.0;
        l.model.pickStop(0.25);
        if (! addRecorded(l, defaultKind(l.note.flags), f0, layerFor(v), (float) ((0.55 + 0.6 * v) * pm) * sign, lp,
                          l.model.maxExcitation()))
            addSynth(l, f0, (0.55 + 0.6 * v) * pm * sign, 0.5, 0.15, l.model.maxExcitation());
    }

    const ExcitationClip* addRecorded(Lane& l, ExcKind kind, double f0, int layer, float gain, double lowpassHz,
                                      int maxLen) noexcept
    {
        const ExcitationClip* c = selectClip(bank_, kind, f0, layer, l.rr++);
        if (c == nullptr)
            return nullptr;
        const int len = std::min(maxLen, (int) (c->length * fs_ / c->sampleRate) + 4);
        float* dst = l.model.beginExcitation(len);
        addClip(*c, fs_, f0 / c->f0Hz, gain, dst, len, lowpassHz);
        return c;
    }

    void addSynth(Lane& l, double f0, double gain, double pulseMs, double noise, int maxLen) noexcept
    {
        SynthPluck sp;
        sp.f0 = f0;
        sp.pickPos = spec_.pickPos;
        sp.pickupPos = spec_.pickupPos;
        sp.pulseMs = pulseMs;
        sp.noise = noise;
        sp.resonanceHz = spec_.resonanceHz;
        sp.gain = (float) (gain * kSynthLevel);
        sp.seed = eventCounter_;
        const int len = std::min(maxLen, (int) (0.05 * fs_));
        float* dst = l.model.beginExcitation(len);
        addSynthPluck(sp, fs_, scratch_.data(), dst, len);
    }

    void addThump(Lane& l, double f0, double gain, int maxLen, int fret) noexcept
    {
        // The finger hits the string at the fret: a soft wide pulse whose
        // "pick position" is that fret's distance from the bridge.
        SynthPluck sp;
        sp.f0 = f0;
        sp.pickPos = std::clamp(std::pow(2.0, -fret / 12.0) * 0.98, 0.05, 0.5);
        sp.pickupPos = spec_.pickupPos;
        sp.pulseMs = 1.4;
        sp.noise = 0.25;
        sp.noiseMs = 0.8;
        sp.resonanceHz = spec_.resonanceHz;
        sp.gain = (float) (gain * kSynthLevel);
        sp.seed = eventCounter_ * 7u + 3u;
        const int len = std::min(maxLen, (int) (0.03 * fs_));
        float* dst = l.model.beginExcitation(len);
        addSynthPluck(sp, fs_, scratch_.data(), dst, len);
    }

    void addSlapPop(Lane& l, double f0, bool pop, double gain, int maxLen) noexcept
    {
        // Slap: the thumb strikes the string over the end of the neck and
        // drives it into the frets. Pop: a finger pulls it up and lets go so
        // it snaps back onto them. Both are bright, both collide.
        SynthPluck sp;
        sp.f0 = f0;
        sp.pickPos = pop ? 0.24 : 0.33;
        sp.pickupPos = spec_.pickupPos;
        sp.pulseMs = pop ? 0.18 : 0.30;
        sp.noise = pop ? 0.35 : 0.5;
        sp.noiseMs = pop ? 1.0 : 2.0;
        sp.resonanceHz = 5200.0;
        sp.resonanceQ = 1.8;
        sp.gain = (float) (gain * kSynthLevel * (pop ? 8.0 : 14.0));
        sp.seed = eventCounter_ * 13u + 5u;
        const int len = std::min(maxLen, (int) (0.04 * fs_));
        float* dst = l.model.beginExcitation(len);
        addSynthPluck(sp, fs_, scratch_.data(), dst, len);
        // Recorded finger residual underneath for body.
        addRecorded(l, spec_.defaultKind, f0, std::max(bank_.layers - 1, 0), (float) (0.5 * gain), 0.0, maxLen);
        // The frets stop the string's first, largest excursions only: the limit
        // sits at a fraction of this attack's own peak, so the collision adds
        // the metallic edge without eating the note's sustain.
        const float excPeak = l.model.excitationPeak();
        l.model.collide(pop ? 0.6 : 0.5, (pop ? 0.8 : 0.9) * excPeak, (int) ((pop ? 0.04 : 0.06) * fs_));
    }

    // ---- control ---------------------------------------------------------

    double currentPitch(const Lane& l) const noexcept
    {
        double p = l.base + quantisedSlide(l.slide) + l.bend;
        const NoteStart& n = l.note;
        if (n.vibratoSemis > 0.0f)
        {
            const double realT = (double) l.samplesSinceOn / fs_;
            const double env = smoothstep((realT - 0.08) / 0.15);
            const double ph = kTwoPi * 5.5 * realT;
            p += spec_.bass ? n.vibratoSemis * env * 0.5 * std::sin(ph) : n.vibratoSemis * env * (0.5 - 0.5 * std::cos(ph));
        }
        return p;
    }

    void controlStep() noexcept
    {
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            Lane& l = lanes_[(size_t) s];
            if (l.state == LaneState::Idle)
                continue;
            if (l.ctlSteps > 0)
            {
                l.bend += l.bendStep;
                l.slide += l.slideStep;
                if (--l.ctlSteps == 0)
                {
                    l.bend = l.bendTarget;
                    l.slide = l.slideTarget;
                }
            }
            if (l.tremoloLeft > 0 && l.state == LaneState::Ringing)
            {
                l.tremoloLeft -= kControlInterval;
                if (l.tremoloLeft <= 0)
                {
                    l.tremoloLeft += tremoloPeriod(l.note.speed);
                    tremoloRepick(l, s);
                }
            }
            control(s, kControlInterval);
        }
    }

    void control(int s, int ramp) noexcept
    {
        Lane& l = lanes_[(size_t) s];
        const NoteStart& n = l.note;
        const double dtCtl = kControlInterval / fs_;
        const double a = l.baseGlide > 0.0 ? 1.0 - std::exp(-dtCtl / l.baseGlide) : 1.0;
        if (ramp <= 1 && l.samplesSinceOn == 0 && n.slideInSemis == 0.0f)
            l.base = l.baseTarget;
        else if (n.slideInSemis != 0.0f && l.samplesSinceOn < (int64_t) (0.06 * fs_))
            l.base = l.baseTarget + n.slideInSemis * (1.0 - smoothstep((double) l.samplesSinceOn / (0.06 * fs_)));
        else
            l.base += (l.baseTarget - l.base) * a;

        const double q = quantisedSlide(l.slide);
        const bool sliding = l.ctlSteps > 0 && std::fabs(l.slideStep) > 1.0e-6;
        if (q != l.lastQuant && l.state == LaneState::Ringing && l.samplesSinceOn > 0)
            addThump(l, l.freq, 0.03, l.model.maxExcitation(), std::max(0, (int) (n.fret + q)));
        l.lastQuant = q;

        double fade = 1.0;
        if ((n.flags & kFlagUnpitchedSlide) && n.slideToSemis != 0.0f)
            fade = 1.0 - 0.85 * std::clamp((double) l.slide / n.slideToSemis, 0.0, 1.0);

        const double f = hz(currentPitch(l));
        l.freq = f;

        // Loss: what is touching the string right now. The open-string decay
        // profile follows the pitch: T60 ~ f^-exp, the shelf corner ~ f.
        const double r = f / l.refHz;
        double tl, th, corner = l.cornerHz * r;
        if (l.state == LaneState::Released)
        {
            tl = spec_.releaseT60;
            th = tl * 0.4;
            corner = std::max(2.0 * f, 200.0);
        }
        else if (n.flags & kFlagFullMute)
        {
            tl = spec_.deadT60;
            th = spec_.deadT60 * 0.5;
            corner = std::max(2.0 * f, 200.0);
        }
        else if (n.flags & kFlagPalmMute)
        {
            tl = spec_.palmT60Low;
            th = spec_.palmT60High;
            corner = std::max(2.5 * f, 250.0);
        }
        else
        {
            tl = l.t60Low * std::pow(1.0 / r, spec_.t60Exponent);
            th = l.t60High;
            if (n.flags & kFlagHarmonic)
                tl *= 1.5;
            if (sliding)
            {
                tl *= 0.7;
                th *= 0.7;
            }
            if (fade < 1.0)
            {
                const double fadeT = 0.08 + 0.6 * fade;
                tl = std::min(tl, fadeT);
                th = std::min(th, fadeT * 0.5);
            }
        }
        const LossCoeffs loss = designLoss(f, fs_, tl, th, corner);
        l.model.setTargets(f, l.apA, loss, ramp);
    }

    void retireQuietLanes() noexcept
    {
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            Lane& l = lanes_[(size_t) s];
            if (l.state == LaneState::Idle)
                continue;
            const float peak = l.model.takePeak();
            const bool choked = l.choking && l.choke == 0;
            if ((peak < 2.0e-6f && ! l.model.excitationPending()) || choked)
                l.quietSamples += kControlInterval;
            else
                l.quietSamples = 0;
            if (l.quietSamples > (int) (0.08 * fs_) || choked)
            {
                l.model.clearState();
                l.state = LaneState::Idle;
                l.id = -1;
                l.choke = 0;
                l.choking = false;
            }
        }
    }

    static constexpr int kTableFrets = 36;
    static constexpr double kSynthLevel = 0.35;
    static constexpr int kMaxRender = kControlInterval;

    InstrumentSpec spec_ = InstrumentSpec::guitar();
    ExcitationBank bank_ {};
    double fs_ = 0.0;
    std::array<Lane, kMaxStrings> lanes_ {};
    std::array<std::array<float, kTableFrets + 1>, kMaxStrings> apTable_ {};
    std::vector<float> scratch_;
    std::vector<float> clipAp_;
    float scratchOut_[kMaxRender] {};
    int ctrlPhase_ = 0;
    uint32_t eventCounter_ = 0;
    int64_t samples_ = 0;
    double dcCoef_ = 0.998;
    double dcX_ = 0.0;
    double dcY_ = 0.0;
    bool nonFinite_ = false;
};

} // namespace dcvoice
