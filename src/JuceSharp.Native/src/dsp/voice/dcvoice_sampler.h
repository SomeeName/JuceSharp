// dcvoice_sampler.h — SampleInstrument: the guitar / bass voice played from
// REAL RECORDINGS (a SamplePack, dcvoice_pack.h), behind the same event API as
// the waveguide core (IVoiceCore), so the timeline player and the engine
// adapter drive either one.
//
// WHAT IS RECORDED, WHAT IS PROCESSED
//   sustain        the recorded note, nearest velocity layer, round robin; the
//                  pack is chromatic for guitar, so most notes play at ratio 1
//   palm mute      the recorded staccato / muted note, darkened by a gentle
//                  low-pass and shortened
//   hammer-on/tap  the recorded hammer-on (guitar; bass: the soft layer entered
//                  past its pick), cross-faded from the ringing note
//   pull-off       the soft layer, cross-faded (a pull-off is a finger pluck)
//   dead note      the recorded fret-hand-muted string (guitar), nothing pitched
//   ghost (bass)   the recorded ghost note, shortened
//   note-off       the release recording (fret hand lifting) under a natural
//                  ~70 ms damping of the note
//   CONTINUOUS GESTURES are resampling of the playing recording: bends,
//   pre-bends, releases and vibrato move its playback rate; a slide re-zones at
//   every fret it crosses (the new fret's recording, time-aligned, cross-faded
//   in 6 ms), so a long slide never chipmunks the timbre.
//   THE ARTICULATION PACK (guitar-arts.dcvp, #52): real palm mutes, natural
//   harmonics and pinch harmonics, shared by every guitar model and converted
//   to its pickup like any recording. When it is installed they play from it.
//   APPROXIMATED without it: natural harmonics (the recorded note at the
//   harmonic's pitch through a low-pass that leaves its fundamental alone),
//   pinch harmonics (the picked note, quieter, plus the recorded note k * f0
//   through a narrow band-pass, ringing), palm mutes (the staccato or sustain
//   recording, darkened and shortened).
//   slap (bass)    the RECORDED thumb slap of the note (Art::SlapNote, #109:
//                  a solid-body roundwound bass, every natural note E1..D4,
//                  two layers x two takes; the packs DotCharter builds no
//                  longer carry any, but a pack that does is played), nearest
//                  pitch retuned; converted to the selected bass like any
//                  recording (its own ConversionPlan)
//   MODELLED: pop (no pop recording exists to us) — the finger recording with
//   a transient brightness and the string snapping onto the frets; and slap
//   too when the pack has no recorded slaps (the #52 model: brightness, frets,
//   the recorded thumb on muted strings); see SlapPopShape.
//
// One lane per string: a new note on a sounding string damps the old one in
// 12 ms (the pick stops it). Every note-off is honoured.
//
// INSTRUMENT MODELS (#50): setConversion() gives each pack variant a
// ConversionPlan (dcvoice_pickup.h) that turns its recordings into another
// pickup / instrument, per playing recording: the source pickup's comb out,
// the target's in (both from the note's string and fret), a resonance swap and
// a body EQ. An identity plan (the model and position the pack recorded) costs
// nothing. A pack that recorded every string (the Strat) is played from the
// recording made on the charted string.
//
// REAL-TIME: prepare() allocates nothing but bookkeeping (and, once, the comb
// history when a plan converts); the pack is loaded elsewhere on the control
// thread and must outlive the instrument. render() and every event are
// allocation-, lock- and I/O-free.
#pragma once

#include "dcvoice_instrument.h"
#include "dcvoice_pack.h"
#include "dcvoice_pickup.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace dcvoice {

/// The slap / pop model's shape (#42), one per kind. Calibrated with
/// tools/voice/realism_eval.py against real slap phrases.
struct SlapPopShape
{
    int variant = 0;            // the pack variant the note plays (0 fingers, 1 pick)
    double level = 1.0;         // the note against an ordinary note's level
    double brightHz = 700.0;    // transient brightness: high-pass corner
    double brightDb = 9.0;      //   added at the attack (dB above the recording)
    double floorDb = 0.0;       //   where it settles (negative = darker than recorded)
    double brightTau = 0.1;     //   its time constant, s
    double clankDrive = 6.0;    // the frets: clip drive (x the recording's peak)
    double clankLevel = 0.3;    //   level against the note
    double clankHz = 1200.0;    //   high-pass corner
    double clankTau = 0.012;    //   decay time constant, s
    double thumbLevel = 0.0;    // the recorded thumb on muted strings (slap)
    int recorded = 0;           // 1: the note is the pack's RECORDED slap (Art::SlapNote) when it has one
};
struct SlapPopShapes
{
    // Fitted 2026-10-04 (realism_eval.py: attack-vs-body brightness, band
    // levels over 0-30 / 30-120 / 120-400 ms, transient): a slap is the finger
    // recording made bright for ~0.15 s (+15 dB above 400 Hz at the attack,
    // settling at +4 dB) under the recorded thumb; a pop is brighter higher up
    // and its string snaps onto the frets (a hard one-sided clip, ~35 ms).
    // `slap` is the MODEL, played only by a pack without recorded slaps.
    SlapPopShape slap { 0, 1.1, 400.0, 15.0, 4.0, 0.15, 6.0, 0.0, 1200.0, 0.012, 0.3, 0 };
    SlapPopShape pop { 0, 1.0, 1500.0, 12.0, 4.0, 0.08, 16.0, 0.8, 1200.0, 0.035, 0.0, 0 };
    // #109: a slap is the RECORDING (Art::SlapNote) as it is - no added
    // brightness, frets or thumb: the recording has all three.
    SlapPopShape slapRecorded { 0, 1.0, 400.0, 0.0, 0.0, 0.15, 6.0, 0.0, 1200.0, 0.012, 0.0, 1 };
};
/// Read when a slap / pop STARTS (audio thread, read-only). Writable only by
/// the offline calibration tool before anything renders.
inline SlapPopShapes& slapPopShapes() noexcept
{
    static SlapPopShapes s;
    return s;
}

class SampleInstrument final : public IVoiceCore
{
public:
    static constexpr int kControlInterval = 16;
    static constexpr int kPlayers = 6;

    /// `arts` (optional): the shared articulation pack (palm mutes, harmonics).
    /// Both packs must outlive the instrument.
    SampleInstrument(const InstrumentSpec& spec, const SamplePack* pack, const SamplePack* arts = nullptr)
        : spec_(spec), pack_(pack), arts_(arts)
    {
        spec_.numStrings = std::clamp(spec_.numStrings, 1, kMaxStrings);
        for (int s = 0; s < kMaxStrings; ++s)
        {
            lanes_[(size_t) s].index = s;
            for (int k = 0; k < kPlayers; ++k)
                lanes_[(size_t) s].players[(size_t) k].slot = (uint16_t) (s * kPlayers + k);
        }
    }

    const InstrumentSpec& spec() const noexcept override { return spec_; }
    double sampleRate() const noexcept override { return fs_; }
    const SamplePack* pack() const noexcept { return pack_; }

    /// CONTROL thread, before prepare(): convert the recordings of pack variant
    /// `variant` (-1 = every variant) with `plan`. Identity by default.
    void setConversion(int variant, const ConversionPlan& plan)
    {
        for (int v = 0; v < 2; ++v)
            if (variant < 0 || variant == v)
                plans_[v] = plan;
        if (! plan.identity && rings_ == nullptr)
            rings_ = std::make_unique<std::array<CombRing, (size_t) kMaxStrings * kPlayers>>();
    }
    const ConversionPlan& conversion(int variant) const noexcept { return plans_[variant & 1]; }

    /// CONTROL thread, before prepare(): convert the ARTICULATION pack's
    /// recordings with `plan` (they were made on another guitar).
    void setArtsConversion(const ConversionPlan& plan)
    {
        artsPlan_ = plan;
        if (! plan.identity && rings_ == nullptr)
            rings_ = std::make_unique<std::array<CombRing, (size_t) kMaxStrings * kPlayers>>();
    }
    const ConversionPlan& artsConversion() const noexcept { return artsPlan_; }

    /// CONTROL thread, before prepare(): convert the pack's RECORDED SLAPS
    /// (Art::SlapNote, #109) with `plan` - they were made on another bass than
    /// the pack's finger and pick notes.
    void setSlapConversion(const ConversionPlan& plan)
    {
        slapPlan_ = plan;
        if (! plan.identity && rings_ == nullptr)
            rings_ = std::make_unique<std::array<CombRing, (size_t) kMaxStrings * kPlayers>>();
    }
    const ConversionPlan& slapConversion() const noexcept { return slapPlan_; }
    const SamplePack* artsPack() const noexcept { return arts_; }

    void prepare(double sampleRate, int) override
    {
        fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        releaseMul_ = (float) std::exp(-6.9 / (kReleaseT60 * fs_));
        for (auto& plan : plans_)
            plan.prepare(fs_);
        artsPlan_.prepare(fs_);
        slapPlan_.prepare(fs_);
        reset();
    }

    /// Fades every sounding recording out over `samples` and forgets the notes —
    /// the voice being replaced by another (a model or pickup change), so the
    /// two cross-fade instead of the old one stopping dead.
    void fadeOutAll(int samples) noexcept
    {
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            Lane& l = lanes_[(size_t) s];
            l.pending = false;
            l.sounding = false;
            l.tremoloLeft = 0;
            for (auto& p : l.players)
                if (p.active)
                    fadeOutPlayer(p, samples);
        }
    }

    void reset() noexcept override
    {
        for (auto& l : lanes_)
        {
            for (auto& p : l.players)
                p.active = false;
            l.id = -1;
            l.sounding = false;
            l.pending = false;
            l.rr = 0;
            l.relRr = 0;
        }
        ctrlPhase_ = 0;
        samples_ = 0;
        counter_ = 0;
    }

    void noteOn(const NoteStart& n) noexcept override
    {
        if (pack_ == nullptr || fs_ <= 0.0 || n.string < 0 || n.string >= spec_.numStrings || ! std::isfinite(n.pitch))
            return;
        Lane& l = lanes_[(size_t) n.string];
        if (n.delaySamples > 0)
        {
            l.pendingNote = n;
            l.pendingNote.delaySamples = 0;
            l.pendingLeft = n.delaySamples;
            l.pending = true;
            return;
        }
        startNow(l, n);
    }

    void noteOff(int32_t id, bool choke) noexcept override
    {
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            Lane& l = lanes_[(size_t) s];
            if (l.pending && l.pendingNote.id == id)
                l.pending = false;
            if (l.sounding && l.id == id)
            {
                if (! choke && (l.note.flags & kFlagEndDamped) != 0)
                {
                    // Damped by the next note on its string: gone in ~15 ms, quietly.
                    for (auto& p : l.players)
                        if (p.active)
                            fadeOutPlayer(p, (int) (0.015 * fs_));
                    l.sounding = false;
                    l.tremoloLeft = 0;
                    continue;
                }
                endLane(l, choke, ! choke);
            }
        }
    }

    void allNotesOff(bool choke) noexcept override
    {
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            Lane& l = lanes_[(size_t) s];
            l.pending = false;
            if (l.sounding)
                endLane(l, choke, false);
        }
    }

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
            if (! l.sounding || l.id != id)
                continue;
            const int steps = std::max(1, (rampSamples + kControlInterval - 1) / kControlInterval);
            l.bendStep = (bendSemis - l.bend) / steps;
            l.slideStep = (slideSemis - l.slide) / steps;
            l.bendTarget = bendSemis;
            l.slideTarget = slideSemis;
            l.ctlSteps = steps;
        }
    }

    void render(float* out, int numSamples) noexcept override
    {
        if (out == nullptr || numSamples <= 0 || fs_ <= 0.0 || pack_ == nullptr)
            return;
        int i = 0;
        while (i < numSamples)
        {
            if (ctrlPhase_ == 0)
                controlStep();
            int end = std::min(numSamples, i + (kControlInterval - ctrlPhase_));
            for (auto& l : lanes_)
            {
                if (! l.pending)
                    continue;
                if (l.pendingLeft <= 0)
                {
                    l.pending = false;
                    startNow(l, l.pendingNote);
                }
                else
                    end = std::min(end, i + l.pendingLeft);
            }
            const int n = end - i;
            for (int s = 0; s < spec_.numStrings; ++s)
            {
                Lane& l = lanes_[(size_t) s];
                if (l.pending)
                    l.pendingLeft -= n;
                for (auto& p : l.players)
                    if (p.active)
                        renderPlayer(p, out + i, n);
            }
            ctrlPhase_ = (ctrlPhase_ + n) % kControlInterval;
            samples_ += n;
            i = end;
        }
        // Scrub: a recording cannot produce a NaN, but a corrupt pack could.
        for (int k = 0; k < numSamples; ++k)
            if (! std::isfinite(out[k]))
                out[k] = 0.0f;
    }

    int activeNotes() const noexcept override
    {
        int n = 0;
        for (int s = 0; s < spec_.numStrings; ++s)
            n += (laneAudible(lanes_[(size_t) s]) || lanes_[(size_t) s].pending) ? 1 : 0;
        return n;
    }
    bool stringActive(int s) const noexcept override
    {
        return s >= 0 && s < kMaxStrings && laneAudible(lanes_[(size_t) s]);
    }
    int32_t stringNoteId(int s) const noexcept override
    {
        return (s >= 0 && s < kMaxStrings && lanes_[(size_t) s].sounding) ? lanes_[(size_t) s].id : -1;
    }
    int64_t samplesRendered() const noexcept override { return samples_; }

private:
    static constexpr double kReleaseT60 = 0.07;
    static constexpr int kSameStringFadeMs = 12;

    enum class Filter : uint8_t { None, LowPass, BandPass, HighPass, HighShelf };

    struct Player
    {
        bool active = false;
        const PackZone* zone = nullptr;
        const int16_t* data = nullptr;
        double pos = 1.0;
        double rate = 1.0;       // source samples per output sample at ratio 1
        double rootHz = 100.0;
        double pitchMul = 1.0;   // played pitch = lane pitch * pitchMul (pinch overlay: k)
        bool pitched = true;
        double step = 1.0, stepInc = 0.0;
        float gain = 0.0f;
        float env = 1.0f, envMul = 1.0f;
        bool released = false;
        int fadeIn = 0, fadeInLen = 1;
        int fadeOut = 0, fadeOutLen = 1;
        bool fadingOut = false;
        Filter filter = Filter::None;
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        // TRANSIENT BRIGHTNESS (slap / pop): a parallel high-pass whose gain
        // starts at `brightGain` and decays to `brightFloor` — the partials a
        // hard impact adds are the first to die, so a slap is bright for a
        // moment, not for ever (a static shelf is what made it a synth).
        bool bright = false;
        float hb0 = 0, hb1 = 0, hb2 = 0, ha1 = 0, ha2 = 0, hz1 = 0, hz2 = 0;
        float brightGain = 0.0f, brightFloor = 0.0f, brightMul = 1.0f;
        // STRING ON THE FRETS (slap / pop): the recording through a one-sided
        // soft clip (the frets stop the string on one side only), 0 = off.
        float clankIn = 0.0f, clankOut = 0.0f;
        bool main = false;       // follows the lane's note (re-zoned on slides)
        // The pickup / instrument conversion (#50); off = the recording as is.
        uint16_t slot = 0;       // this player's CombRing (kept across resets)
        bool conv = false;
        const ConversionPlan* plan = nullptr;
        NoteComb comb {};
        float lead[4] {}, lag[4] {};   // target taps, samples behind now
        float dSin = 0.0f, dCos = 0.0f; // source inverse feedback delays (0 = stage off)
        int widx = 0, written = 0;
        float rz1 = 0, rz2 = 0;
        float bz1[4] {}, bz2[4] {};
    };

    struct Lane
    {
        std::array<Player, kPlayers> players {};
        bool sounding = false;   // a note is held (not yet released / choked)
        int32_t id = -1;
        NoteStart note {};
        bool pending = false;
        NoteStart pendingNote {};
        int pendingLeft = 0;
        double base = 40.0, baseTarget = 40.0, baseGlide = 0.0;
        float bend = 0, bendTarget = 0, bendStep = 0;
        float slide = 0, slideTarget = 0, slideStep = 0;
        int ctlSteps = 0;
        double lastQuant = 0.0;
        int64_t onAt = 0;
        uint32_t rr = 0, relRr = 0;
        int layer = 1;
        int variant = 0;
        Art art = Art::Sustain;
        float level = 1.0f;
        int tremoloLeft = 0, tremoloCount = 0;
        int index = 0;           // the string this lane plays (0 = lowest)
    };

    static double hz(double midi) noexcept { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

    static double smoothstep(double x) noexcept
    {
        x = std::clamp(x, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }

    static bool laneAudible(const Lane& l) noexcept
    {
        for (const auto& p : l.players)
            if (p.active)
                return true;
        return false;
    }

    int chokeSamples() const noexcept { return std::max(8, (int) (0.004 * fs_)); }

    int layerFor(double v, Art art, int variant, bool accent) const noexcept
    {
        const int layers = std::max(pack_->layerCount[(int) art][pack_->has(art, variant) ? variant : 0], 1);
        if (accent)
            return layers - 1;
        // Charts carry one velocity (0.72) for nearly everything: that is the
        // middle layer, not the softest.
        const int l = v < 0.5 ? 0 : (v < 0.85 ? layers / 2 : layers - 1);
        return std::clamp(l, 0, layers - 1);
    }

    Player* freePlayer(Lane& l) noexcept
    {
        for (auto& p : l.players)
            if (! p.active)
                return &p;
        // All busy: take the quietest that is not the main note.
        Player* best = nullptr;
        float bestEnv = 2.0f;
        for (auto& p : l.players)
            if (! p.main && p.env * (p.fadingOut ? 0.5f : 1.0f) < bestEnv)
            {
                bestEnv = p.env;
                best = &p;
            }
        return best != nullptr ? best : &l.players[kPlayers - 1];
    }

    void fadeOutPlayer(Player& p, int samples) noexcept
    {
        if (! p.active)
            return;
        const int len = std::max(samples, 1);
        if (p.fadingOut && p.fadeOut <= len)
            return;
        p.fadingOut = true;
        p.fadeOut = len;
        p.fadeOutLen = len;
        p.main = false;
        // A recording on its way out keeps the pitch it had: the string's NEW
        // pitch belongs to the note replacing it, never to the one fading.
        p.pitched = false;
        p.stepInc = 0.0;
    }

    static void setBiquad(Player& p, Filter type, double f, double q, double fs, double gainDb = 0.0) noexcept
    {
        p.filter = type;
        p.z1 = p.z2 = 0.0f;
        if (type == Filter::None)
            return;
        f = std::clamp(f, 20.0, 0.45 * fs);
        const double w = kTwoPi * f / fs, cw = std::cos(w), sw = std::sin(w), alpha = sw / (2.0 * q);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (type)
        {
            case Filter::LowPass:
                b0 = (1 - cw) * 0.5; b1 = 1 - cw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Filter::HighPass:
                b0 = (1 + cw) * 0.5; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Filter::BandPass:
                b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Filter::HighShelf:
            {
                const double A = std::pow(10.0, gainDb / 40.0), sq = 2.0 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cw + sq);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - sq);
                a0 = (A + 1) - (A - 1) * cw + sq;
                a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - sq;
                break;
            }
            default:
                break;
        }
        p.b0 = (float) (b0 / a0); p.b1 = (float) (b1 / a0); p.b2 = (float) (b2 / a0);
        p.a1 = (float) (a1 / a0); p.a2 = (float) (a2 / a0);
    }

    /// Arms a player's transient brightness: a 2nd-order high-pass at `hz` added
    /// back on top of the recording, `startDb` above it at the attack, decaying
    /// with time constant `tau` to `floorDb`.
    void setBright(Player& p, double hzc, double startDb, double floorDb, double tau) const noexcept
    {
        Player tmp;
        setBiquad(tmp, Filter::HighPass, hzc, 0.7, fs_);
        p.hb0 = tmp.b0; p.hb1 = tmp.b1; p.hb2 = tmp.b2; p.ha1 = tmp.a1; p.ha2 = tmp.a2;
        p.hz1 = p.hz2 = 0.0f;
        p.brightGain = (float) (std::pow(10.0, startDb / 20.0) - 1.0);
        p.brightFloor = (float) (std::pow(10.0, floorDb / 20.0) - 1.0);
        p.brightMul = (float) std::exp(-1.0 / (std::max(tau, 1.0e-3) * fs_));
        p.bright = true;
    }

    /// Arms a player as the string-on-fret collision: its recording driven into
    /// a one-sided soft clip `drive` times past its own peak level.
    static void setClank(Player& p, double drive) noexcept
    {
        const double g = std::max((double) p.zone->gain, 1.0e-9);
        p.clankIn = (float) (g * drive);
        p.clankOut = (float) (1.0 / (g * drive));
    }

    /// Starts a recording on a free player of the lane. `startSeconds` skips
    /// into it (chase, legato without a pick). Returns the player or nullptr.
    Player* play(Lane& l, const PackZone* z, double targetHz, bool pitched, float gain, double startSeconds,
                 int fadeInSamples) noexcept
    {
        if (z == nullptr)
            return nullptr;
        Player* p = freePlayer(l);
        const uint16_t slot = p->slot;
        *p = Player {};
        p->slot = slot;
        p->active = true;
        p->zone = z;
        const SamplePack& src = packOf(z);
        p->data = src.samples(*z);
        p->rate = src.sampleRate / fs_;
        p->rootHz = z->rootHz;
        p->pitched = pitched;
        const double ratio = pitched ? std::clamp(targetHz / z->rootHz, 0.25, 4.0) : 1.0;
        p->step = p->rate * ratio;
        p->pos = 1.0 + std::max(0.0, startSeconds) * src.sampleRate;
        if (p->pos > (double) z->length - 64.0)
            p->pos = std::max(1.0, (double) z->length - 64.0);
        p->gain = gain * z->gain * zoneLevel(*z);
        p->fadeIn = p->fadeInLen = std::max(fadeInSamples, 0);
        if (p->fadeInLen == 0)
            p->fadeInLen = 1;
        const Art art = (Art) z->art;
        if (pitched && rings_ != nullptr && ! planOf(*z).identity
            && (art == Art::Sustain || art == Art::Palm || art == Art::Legato || art == Art::Ghost || art == Art::NatHarm
                || art == Art::Pinch || art == Art::SlapNote))
            armConversion(l, *p, targetHz);
        return p;
    }

    /// The note's comb geometry for this recording: the TARGET from the lane's
    /// charted string / fret (slides move the fret, a natural harmonic vibrates
    /// the whole open string in mode k), the SOURCE from where the recording was
    /// made. Audio thread, no allocation.
    void armConversion(const Lane& l, Player& p, double hz) noexcept
    {
        const ConversionPlan& plan = planOf(*p.zone);
        const NoteStart& n = l.note;
        const int capo = std::max(n.capo, 0);
        const bool natural = (n.flags & kFlagHarmonic) != 0;
        const double k = natural ? (double) naturalHarmonicNumber(n) : 1.0;
        double fret = natural ? (double) capo : (n.fret > 0 ? (double) n.fret : (double) capo);
        if (! natural)
            fret += l.lastQuant;
        fret = std::clamp(fret, 0.0, 36.0);
        const double openT = n.fret > 0 ? n.pitch - n.fret : n.pitch - capo;
        const double lengthT = plan.targetScaleMm * std::pow(2.0, -fret / 12.0);
        // A recorded natural harmonic sounds mode kS of its whole open string.
        const int kS = p.zone->art == (uint8_t) Art::NatHarm ? std::max(harmonicOfNode(p.zone->fret), 1) : 1;
        const double fretS = kS > 1 ? 0.0 : (p.zone->fret >= 0 ? (double) p.zone->fret : 0.0);
        const double openS = 69.0 + 12.0 * std::log2((double) p.zone->rootHz / 440.0) - fretS - 12.0 * std::log2((double) kS);
        const double lengthS = plan.source.scaleMm * std::pow(2.0, -fretS / 12.0);
        p.comb.set(plan, openT, lengthT, openS, lengthS, k, (double) kS);
        p.plan = &plan;
        p.conv = true;
        p.widx = 0;
        p.written = 0;
        p.rz1 = p.rz2 = 0.0f;
        for (int i = 0; i < 4; ++i)
            p.bz1[i] = p.bz2[i] = 0.0f;
        setCombDelays(p, hz);
    }

    /// Delays in samples for the pitch the recording is playing now: rho * fs / f.
    void setCombDelays(Player& p, double hz) noexcept
    {
        const double period = fs_ / std::max(hz, 10.0);
        const auto clampD = [](double d) { return (float) std::clamp(d, 0.0, (double) pickup::kMaxDelay); };
        double centre = 0.0;
        for (int i = 0; i < p.comb.targetCount; ++i)
            centre = std::max(centre, 0.5 * p.comb.targetRho[i] * period);
        for (int i = 0; i < p.comb.targetCount; ++i)
        {
            const double a = 0.5 * p.comb.targetRho[i] * period;
            p.lead[i] = clampD(centre - a);
            p.lag[i] = clampD(centre + a);
        }
        const double dSin = p.comb.sourceSinRho * period, dCos = p.comb.sourceCosRho * period;
        p.dSin = dSin >= 1.0 ? clampD(dSin) : 0.0f;
        p.dCos = dCos >= 2.0 ? clampD(dCos) : 0.0f;
    }

    static float ringRead(const float* ring, int widx, int written, float d) noexcept
    {
        if (d > (float) written - 1.0f)
            return 0.0f;
        const int di = (int) d;
        const float fr = d - (float) di;
        constexpr int mask = pickup::kRing - 1;
        const float a = ring[(widx - di) & mask], b = ring[(widx - di - 1) & mask];
        return a + fr * (b - a);
    }

    static float biquad(const BiquadCoeffs& c, float x, float& z1, float& z2) noexcept
    {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    /// One sample through the player's conversion (see dcvoice_pickup.h).
    float convert(Player& p, float x) noexcept
    {
        CombRing& r = (*rings_)[p.slot];
        constexpr int mask = pickup::kRing - 1;
        const float g = pickup::kRegularise, gs = p.plan->source.inverseG;
        // The source's comb out: its sine factor, then its cosine factor.
        float u = p.dSin > 0.0f ? 2.0f * x + gs * ringRead(r.u, (p.widx - 1) & mask, p.written, p.dSin - 1.0f) : x;
        r.u[p.widx] = u;
        float v = p.dCos > 0.0f ? 2.0f * u - gs * ringRead(r.v, (p.widx - 1) & mask, p.written, p.dCos - 1.0f) : u;
        r.v[p.widx] = v;
        if (p.written < pickup::kRing)
            ++p.written;
        // The target's comb in.
        float y = 0.0f;
        for (int i = 0; i < p.comb.targetCount; ++i)
            y += 0.5f * p.comb.targetWeight[i] * (ringRead(r.v, p.widx, p.written, p.lead[i]) - g * ringRead(r.v, p.widx, p.written, p.lag[i]));
        p.widx = (p.widx + 1) & mask;
        const ConversionPlan& plan = *p.plan;
        if (! plan.resonance.unity())
            y = biquad(plan.resonance, y, p.rz1, p.rz2);
        for (int i = 0; i < plan.bodyCount; ++i)
            y = biquad(plan.body[i], y, p.bz1[i], p.bz2[i]);
        return y * p.comb.gain * plan.levelGain;
    }

    /// Level of each kind of recording against a sustained note (every zone is
    /// peak-normalised by the pack builder, so these set the balance): a
    /// default-velocity guitar note peaks near -16 dBFS DI (a full strum then
    /// keeps its headroom), a bass note near -11 dBFS.
    float artLevel(Art a) const noexcept
    {
        if (spec_.bass)
        {
            switch (a)
            {
                case Art::Sustain: return 1.2f;
                case Art::Palm: return 0.8f;
                // A muted pluck PEAKS above an ordinary note (it is all attack):
                // +5 dB on the held-out Karoryfer basses (#52). It was 0.6.
                case Art::Ghost: return 2.2f;
                case Art::Release: return 0.12f;
                case Art::Slap: return 1.0f;
                case Art::SlapNote: return kSlapNoteLevel;
                default: return 1.0f;
            }
        }
        switch (a)
        {
            case Art::Sustain: return 1.0f;
            case Art::Palm: return 0.5f;
            case Art::Legato: return 0.65f;
            case Art::Release: return 0.12f;
            // +3 dB over an mf note on the held-out Emilyguitar (#52); at 0.35
            // a dead note was a faint tick 9 dB UNDER the notes around it.
            case Art::Dead: return 1.25f;
            default: return 1.0f;
        }
    }

    bool isArts(const PackZone* z) const noexcept
    {
        return arts_ != nullptr && z >= arts_->zones.data() && z < arts_->zones.data() + arts_->zones.size();
    }
    const SamplePack& packOf(const PackZone* z) const noexcept { return isArts(z) ? *arts_ : *pack_; }
    const ConversionPlan& planOf(const PackZone& z) const noexcept
    {
        if (isArts(&z))
            return artsPlan_;
        return z.art == (uint8_t) Art::SlapNote ? slapPlan_ : plans_[z.variant & 1];
    }

    /// The level of a recording against a sustained note. The articulation
    /// pack's recordings are each peak-normalised like a sustain; their real
    /// balance (measured on the held-out player of the same dataset against
    /// that guitar's own open notes) is set here.
    float zoneLevel(const PackZone& z) const noexcept
    {
        if (isArts(&z))
        {
            switch ((Art) z.art)
            {
                case Art::Palm: return kArtsPalmLevel;
                case Art::NatHarm: return kArtsHarmonicLevel;
                case Art::Pinch: return kArtsPinchLevel;
                default: return 1.0f;
            }
        }
        return artLevel((Art) z.art);
    }

    bool artsHas(Art a) const noexcept { return arts_ != nullptr && arts_->has(a, 0); }

    /// The recorded natural harmonic closest to (string, k, pitch): the same
    /// harmonic number first, then the same string, then the pitch. O(zones),
    /// allocation-free.
    const PackZone* selectHarmonic(int string, int k, double hzWanted, uint32_t rr) const noexcept
    {
        if (! artsHas(Art::NatHarm) || hzWanted <= 0.0)
            return nullptr;
        // A LAZY articulation pack (#417) chooses among the DECODED harmonics
        // and asks for the rest (the demand planner decodes them all when a
        // chart has any harmonic, so this is the fully decoded choice then).
        bool ready = true;
        for (const PackZone& z : arts_->zones)
            if (z.art == (uint8_t) Art::NatHarm && ! arts_->ready(z))
            {
                ready = false;
                arts_->request(z);
            }
        const double lf = std::log2(hzWanted);
        double best = 1.0e9;
        for (const PackZone& z : arts_->zones)
        {
            if (z.art != (uint8_t) Art::NatHarm || (! ready && ! arts_->ready(z)))
                continue;
            const double score = (harmonicOfNode(z.fret) == k ? 0.0 : 100.0) + (z.string == string ? 0.0 : 10.0)
                                 + std::fabs(std::log2((double) z.rootHz) - lf) * 12.0;
            best = std::min(best, score);
        }
        int count = 0;
        for (const PackZone& z : arts_->zones)
            if (z.art == (uint8_t) Art::NatHarm && (ready || arts_->ready(z))
                && (harmonicOfNode(z.fret) == k ? 0.0 : 100.0) + (z.string == string ? 0.0 : 10.0)
                           + std::fabs(std::log2((double) z.rootHz) - lf) * 12.0 < best + 0.5)
                ++count;
        int want = count > 0 ? (int) (rr % (uint32_t) count) : 0;
        for (const PackZone& z : arts_->zones)
            if (z.art == (uint8_t) Art::NatHarm && (ready || arts_->ready(z))
                && (harmonicOfNode(z.fret) == k ? 0.0 : 100.0) + (z.string == string ? 0.0 : 10.0)
                           + std::fabs(std::log2((double) z.rootHz) - lf) * 12.0 < best + 0.5 && want-- == 0)
                return &z;
        return nullptr;
    }

    Player* mainPlayer(Lane& l) noexcept
    {
        for (auto& p : l.players)
            if (p.active && p.main)
                return &p;
        return nullptr;
    }

    void endLane(Lane& l, bool choke, bool releaseNoise) noexcept
    {
        l.sounding = false;
        l.tremoloLeft = 0;
        const int sounded = (int) (samples_ - l.onAt);
        for (auto& p : l.players)
        {
            if (! p.active)
                continue;
            if (choke)
                fadeOutPlayer(p, chokeSamples());
            else
                p.released = true;
            p.main = false;
        }
        // The fret hand letting go is a sound of its own; not after a
        // fast note, where it would only be clutter.
        if (releaseNoise && sounded > (int) (0.15 * fs_) && (l.art == Art::Sustain || l.art == Art::SlapNote))
        {
            const double h = hz(l.base);
            const PackZone* z = selectZone(*pack_, Art::Release, 0, h, 0, l.relRr++);
            if (Player* p = play(l, z, h, false, l.level, 0.0, 0))
                p->main = false;
        }
    }

    double lanePitch(const Lane& l) const noexcept
    {
        double p = l.base + std::ceil(l.slide - 1.0e-4) + l.bend;
        const NoteStart& n = l.note;
        if (n.vibratoSemis > 0.0f)
            p += vibrato(n.vibratoSemis, (double) (samples_ - l.onAt) / fs_, (uint32_t) n.id, spec_.bass);
        return p;
    }

public:
    /// The vibrato offset (semitones) `t` s into a note. A HAND, not an LFO
    /// (#52, measured on Guitar-TECHS' two players: 3.3-3.9 Hz, 18-25 cents
    /// peak to peak, starting ~0.3 s in, periodicity 0.3-0.85): ~4.3 Hz whose
    /// rate wanders +-10 % and depth +-15 % on slow, per-note phases; it grows
    /// in over 0.12-0.37 s. Guitar bends UP and back (one-sided), a bass rocks
    /// the string both ways. Closed form in t (the phase of a rate that varies
    /// sinusoidally), so it is stateless: block size never changes it.
    static double vibrato(double depth, double t, uint32_t seed, bool bass) noexcept
    {
        if (t <= 0.0)
            return 0.0;
        const double phiR = kTwoPi * 0.5 * (1.0 + hashUnit(seed, 101u));
        const double phiD = kTwoPi * 0.5 * (1.0 + hashUnit(seed, 202u));
        const double phase = kTwoPi * kVibratoHz * t
                             + (kVibratoHz * kVibratoRateWander / kVibratoWanderHz)
                                   * (std::cos(phiR) - std::cos(kTwoPi * kVibratoWanderHz * t + phiR));
        const double d = depth * (1.0 + kVibratoDepthWander * std::sin(kTwoPi * 0.53 * t + phiD));
        const double env = smoothstep((t - 0.12) / 0.25);
        return bass ? d * env * 0.5 * std::sin(phase) : d * env * (0.5 - 0.5 * std::cos(phase));
    }
    static constexpr double kVibratoHz = 4.3, kVibratoRateWander = 0.10, kVibratoWanderHz = 0.37;
    static constexpr double kVibratoDepthWander = 0.15;

private:
    // ---- note on -----------------------------------------------------------

    void startNow(Lane& l, const NoteStart& ev) noexcept
    {
        const uint32_t f = ev.flags;
        const bool wasSounding = l.sounding && laneAudible(l);
        const bool legatoGesture = (f & (kFlagHammerOn | kFlagPullOff | kFlagTap | kFlagLinked)) != 0;
        const bool legato = legatoGesture && wasSounding && ev.chaseSeconds <= 0.0;
        const bool natural = (f & kFlagHarmonic) != 0;
        const double open = vibratingOpenPitch(ev);   // the capo, not the nut, ends the string
        const int k = natural ? naturalHarmonicNumber(ev) : 1;
        ++counter_;

        l.note = ev;
        l.id = ev.id;
        l.sounding = true;
        l.onAt = samples_;
        l.bend = l.bendTarget = ev.initialBend;
        l.slide = l.slideTarget = ev.initialSlide;
        l.bendStep = l.slideStep = 0.0f;
        l.ctlSteps = 0;
        l.baseTarget = natural ? open + 12.0 * std::log2((double) k) : ev.pitch;
        l.tremoloCount = 0;
        l.tremoloLeft = (f & kFlagTremolo) ? tremoloPeriod(ev.speed) : 0;
        l.variant = (spec_.bass && (f & kFlagPicked)) ? 1 : 0;
        const double v = std::clamp((double) ev.velocity * (1.0 + 0.04 * hashUnit(counter_, (uint32_t) ev.string)), 0.02, 1.0);
        double gain = 0.7 + 0.45 * v;
        // A player never picks twice alike: +-0.6 dB per attack (deterministic).
        gain *= std::pow(10.0, kAttackLevelJitterDb * hashUnit(counter_, 0x51u + (uint32_t) ev.string) / 20.0);
        if (f & kFlagAccent)
            gain *= 1.12;
        if (f & kFlagGhost)
            gain *= 0.45;
        if ((f & kFlagUpStroke) != 0)
            gain *= 0.95;
        l.level = (float) gain;

        if (legato && (f & kFlagLinked))
        {
            // A tie: the string keeps sounding, only the pitch moves.
            l.baseGlide = 0.004;
            l.lastQuant = std::ceil(l.slide - 1.0e-4);
            return;
        }

        if (legato)
        {
            // Hammer-on / tap / pull-off on a ringing string: the new fret's
            // recording takes over from the old one in a short cross-fade.
            l.base = l.baseTarget;
            l.baseGlide = 0.0;
            l.lastQuant = std::ceil(l.slide - 1.0e-4);
            const double h = hz(lanePitch(l));
            Player* old = mainPlayer(l);
            Player* p = nullptr;
            l.art = Art::Sustain;
            if ((f & (kFlagHammerOn | kFlagTap)) && pack_->has(Art::Legato, 0))
                p = play(l, selectZone(*pack_, Art::Legato, 0, h, 0, l.rr++, l.index), h, true,
                         (float) (gain * ((f & kFlagTap) ? 1.0 : 0.85)), 0.0, (int) (0.003 * fs_));
            else
                // A pull-off is the finger plucking as it leaves: the string's
                // own attack, but no PICK — the recording is entered past its
                // pick click (5 ms, #52). A hammer-on skips further (6 ms).
                p = play(l, selectZone(*pack_, Art::Sustain, l.variant, h, 0, l.rr++, l.index), h, true,
                         (float) (gain * ((f & kFlagPullOff) ? 0.75 : 0.6)), (f & kFlagPullOff) ? 0.005 : 0.006,
                         (int) (0.003 * fs_));
            if (old != nullptr)
                fadeOutPlayer(*old, (int) (0.010 * fs_));
            if (p != nullptr)
                p->main = true;
            return;
        }

        // A new attack: whatever the string was doing stops (the pick, the
        // palm or the fret hand).
        for (auto& p : l.players)
            if (p.active)
                fadeOutPlayer(p, (int) (kSameStringFadeMs * 1.0e-3 * fs_));
        l.base = l.baseTarget + ev.slideInSemis;
        l.baseGlide = 0.0;
        l.lastQuant = std::ceil(l.slide - 1.0e-4);
        const double h = hz(lanePitch(l));
        const uint32_t rr = l.rr++;

        if ((f & (kFlagSlap | kFlagPop)) != 0 && spec_.bass)
        {
            // SLAP / POP. A SLAP is RECORDED when the pack has slaps (#109,
            // Art::SlapNote): the nearest recorded slap, its velocity layer,
            // round robin, retuned - played as it is (SlapPopShapes::slapRecorded).
            // Otherwise, and for every POP (no pop recording exists to us), the
            // MODEL fitted in #52 against CC0 slap phrases (realism_eval.py):
            //   the note   the hardest recorded layer (a pop: the pick set, the
            //              harder attack), with a TRANSIENT brightness that dies
            //              in ~0.1 s — the impact's extra partials go first;
            //   the frets  the same recording through a one-sided soft clip,
            //              high-passed, gone in tens of milliseconds — the
            //              string slapping onto the frets (the "clank");
            //   the thumb  a slap: the recorded thumb on muted strings.
            const bool pop = (f & kFlagPop) != 0;
            const bool haveSlaps = pack_->has(Art::SlapNote, 0);
            const SlapPopShapes& shapes = slapPopShapes();
            const SlapPopShape& sh = pop ? shapes.pop : (haveSlaps ? shapes.slapRecorded : shapes.slap);
            const bool recorded = sh.recorded != 0 && haveSlaps;
            l.art = recorded ? Art::SlapNote : Art::Sustain;
            const int slapLayer = layerFor(v, Art::SlapNote, 0, (f & kFlagAccent) != 0);
            const PackZone* z = recorded ? selectZone(*pack_, Art::SlapNote, 0, h, slapLayer, rr, l.index)
                                         : selectZone(*pack_, Art::Sustain, sh.variant, h, 99, rr, l.index);
            if (Player* p = play(l, z, h, true, (float) (gain * sh.level), 0.0, 0))
            {
                p->main = true;
                if (sh.brightDb != 0.0 || sh.floorDb != 0.0)
                    setBright(*p, sh.brightHz, sh.brightDb, sh.floorDb, sh.brightTau);
            }
            if (sh.clankLevel > 0.0)
                if (Player* c = play(l, z, h, true, (float) (gain * sh.level * sh.clankLevel), 0.0, 0))
                {
                    c->main = false;
                    setClank(*c, sh.clankDrive);
                    setBiquad(*c, Filter::HighPass, sh.clankHz, 0.7, fs_);
                    c->envMul = (float) std::exp(-1.0 / (sh.clankTau * fs_));
                }
            if (sh.thumbLevel > 0.0)
                play(l, selectZone(*pack_, Art::Slap, 0, 100.0, 0, rr), 0.0, false, (float) (gain * sh.thumbLevel), 0.0, 0);
            return;
        }
        if (f & kFlagFullMute)
        {
            l.art = spec_.bass ? Art::Ghost : Art::Dead;
            if (spec_.bass)
            {
                if (Player* p = play(l, selectZone(*pack_, Art::Ghost, 0, h, 0, rr), h, true, (float) gain, 0.0, 0))
                {
                    p->main = true;
                    p->envMul = (float) std::exp(-1.0 / (0.08 * fs_));
                    // The finger's click on the muted string: the recorded ghosts
                    // are ~14 dB darker in their first 30 ms than the held-out
                    // basses' muted plucks (#52).
                    setBright(*p, 800.0, 12.0, 0.0, 0.03);
                }
            }
            else
            {
                // The open string it was muted on picks the recording; nothing pitched.
                play(l, selectZone(*pack_, Art::Dead, 0, hz(open), 0, rr), 0.0, false, (float) gain, 0.0, 0);
            }
            return;
        }
        if ((f & kFlagPalmMute) && artsHas(Art::Palm))
        {
            // A REAL palm mute (the articulation pack): no filter, no added decay.
            l.art = Art::Palm;
            if (Player* p = play(l, selectZone(*arts_, Art::Palm, 0, h, 0, rr, l.index), h, true, (float) gain, 0.0, 0))
            {
                p->main = true;
                quietArtsHiss(*p);
            }
            return;
        }
        if (f & kFlagPalmMute)
        {
            // A pack without palm-mute recordings (the Strat's) mutes its own
            // sustain the same way: the same low-pass and short decay, so the
            // palm keeps the model's pickup and string.
            l.art = Art::Palm;
            const Art palm = pack_->has(Art::Palm, l.variant) ? Art::Palm : Art::Sustain;
            if (Player* p = play(l, selectZone(*pack_, palm, l.variant, h, 0, rr, l.index), h, true,
                                 (float) (gain * (palm == Art::Palm ? 1.0 : 0.5)), 0.0, 0))
            {
                p->main = true;
                setBiquad(*p, Filter::LowPass, spec_.bass ? 900.0 : 1800.0, 0.6, fs_);
                p->envMul = (float) std::exp(-1.0 / ((spec_.bass ? 0.20 : 0.16) * fs_));
            }
            return;
        }

        l.art = Art::Sustain;
        const int layer = layerFor(v, Art::Sustain, l.variant, (f & kFlagAccent) != 0);
        const double chase = ev.chaseSeconds > 0.0 ? ev.chaseSeconds / std::max((double) ev.speed, 0.05) : 0.0;
        if (natural && artsHas(Art::NatHarm))
        {
            // A RECORDED natural harmonic: the nearest of the same harmonic
            // number (same string first), retuned to this one's exact pitch.
            l.art = Art::NatHarm;
            if (Player* p = play(l, selectHarmonic(l.index, k, h, rr), h, true, (float) gain, chase,
                                 chase > 0 ? (int) (0.005 * fs_) : 0))
            {
                p->main = true;
                quietArtsHiss(*p);
            }
            return;
        }
        if (natural)
        {
            // APPROXIMATION: the recorded note AT the harmonic's pitch, through
            // a low-pass just above it — round and bell-like, and the open
            // string's fundamental is not in it at all.
            if (Player* p = play(l, selectZone(*pack_, Art::Sustain, l.variant, h, 0, rr, l.index), h, true, (float) (gain * 1.1),
                                 chase, chase > 0 ? (int) (0.005 * fs_) : (int) (0.002 * fs_)))
            {
                p->main = true;
                setBiquad(*p, Filter::LowPass, 1.35 * h, 0.8, fs_);
            }
            return;
        }
        if ((f & kFlagPinchHarmonic) && artsHas(Art::Pinch))
        {
            // A RECORDED pinch harmonic (the same string, the nearest fret).
            l.art = Art::Pinch;
            if (Player* p = play(l, selectZone(*arts_, Art::Pinch, 0, h, 0, rr, l.index), h, true, (float) gain, chase,
                                 chase > 0 ? (int) (0.005 * fs_) : 0))
            {
                p->main = true;
                quietArtsHiss(*p);
            }
            return;
        }
        if (legatoGesture && (f & kFlagLinked) == 0 && chase <= 0.0)
        {
            // A hammer-on, pull-off or tap onto a SILENT string (a legato run
            // moving to a new string, tapping from silence; DotCharter #100):
            // the fretting hand sounds it, so no pick. The same entries as on a
            // ringing string above, at the string model's softer level for a
            // note that has nothing to continue.
            const double soft = (f & kFlagTap) ? 0.85 : ((f & kFlagPullOff) ? 0.75 : 0.6);
            Player* q = nullptr;
            if ((f & (kFlagHammerOn | kFlagTap)) && pack_->has(Art::Legato, 0))
                q = play(l, selectZone(*pack_, Art::Legato, 0, h, 0, rr, l.index), h, true,
                         (float) (gain * soft), 0.0, (int) (0.003 * fs_));
            else
                q = play(l, selectZone(*pack_, Art::Sustain, l.variant, h, layer, rr, l.index), h, true,
                         (float) (gain * soft), (f & kFlagPullOff) ? 0.005 : 0.006, (int) (0.003 * fs_));
            if (q != nullptr)
                q->main = true;
            return;
        }

        // A pack with ONE take per string and fret (the Strat's): a repeated
        // note would be the same recording every time (a machine gun). Two
        // attacks in three play the NEIGHBOURING fret's recording on the same
        // string, retuned a semitone — another pluck of the same string.
        double zoneHz = h;
        if (pack_->multiString && chase <= 0.0 && pack_->layerCount[(int) Art::Sustain][l.variant & 1] <= 1)
        {
            const uint32_t pick = rr % 3;
            if (pick == 1 && ev.fret > 0)
                zoneHz = h * std::pow(2.0, -1.0 / 12.0);
            else if (pick == 2)
                zoneHz = h * std::pow(2.0, 1.0 / 12.0);
        }
        Player* p = play(l, selectZone(*pack_, Art::Sustain, l.variant, zoneHz, layer, rr, l.index), h, true,
                         (float) (gain * ((f & kFlagPinchHarmonic) ? 0.55 : 1.0)), chase,
                         chase > 0 ? (int) (0.005 * fs_) : 0);
        if (p != nullptr)
            p->main = true;
        if ((f & kFlagPinchHarmonic) && chase <= 0.0)
        {
            // APPROXIMATION: the squeal is the recorded note k * f0 through a
            // narrow band-pass, louder than the note under it, ringing with it.
            const int kp = std::clamp((int) std::lround(1800.0 / std::max(h, 1.0)), 3, 8);
            const double hk = h * kp;
            if (Player* o = play(l, selectZone(*pack_, Art::Sustain, l.variant, std::min(hk, 1200.0), layer, rr, l.index), hk,
                                 true, (float) (gain * 1.6), 0.004, (int) (0.004 * fs_)))
            {
                o->pitchMul = kp;
                o->main = false;
                o->conv = false;   // a band-passed partial: its pickup is the note's
                setBiquad(*o, Filter::BandPass, hk, 3.0, fs_);
                o->envMul = 1.0f;
            }
        }
    }

    int tremoloPeriod(float speed) const noexcept
    {
        return std::max(16, (int) (fs_ / (14.0 * std::clamp((double) speed, 0.25, 2.0))));
    }

    // ---- control -------------------------------------------------------------

    void controlStep() noexcept
    {
        for (int s = 0; s < spec_.numStrings; ++s)
        {
            Lane& l = lanes_[(size_t) s];
            if (! laneAudible(l))
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
            if (l.baseGlide > 0.0)
                l.base += (l.baseTarget - l.base) * (1.0 - std::exp(-kControlInterval / (fs_ * l.baseGlide)));
            else if (l.note.slideInSemis != 0.0f && samples_ - l.onAt < (int64_t) (0.06 * fs_))
                l.base = l.baseTarget + l.note.slideInSemis * (1.0 - smoothstep((double) (samples_ - l.onAt) / (0.06 * fs_)));
            else
                l.base = l.baseTarget;

            if (l.sounding && l.tremoloLeft > 0)
            {
                l.tremoloLeft -= kControlInterval;
                if (l.tremoloLeft <= 0)
                {
                    l.tremoloLeft += tremoloPeriod(l.note.speed);
                    tremoloRepick(l);
                }
            }

            // A slide re-zones at every fret it crosses.
            const double q = std::ceil(l.slide - 1.0e-4);
            if (l.sounding && q != l.lastQuant)
            {
                l.lastQuant = q;
                rezone(l);
            }

            const double h = hz(lanePitch(l));
            double fade = 1.0;
            if ((l.note.flags & kFlagUnpitchedSlide) && l.note.slideToSemis != 0.0f)
                fade = 1.0 - 0.9 * std::clamp((double) l.slide / l.note.slideToSemis, 0.0, 1.0);
            for (auto& p : l.players)
            {
                if (! p.active || ! p.pitched)
                    continue;
                const double target = p.rate * std::clamp(h * p.pitchMul / p.rootHz, 0.25, 4.0);
                p.stepInc = (target - p.step) / kControlInterval;
                if (p.conv)
                    setCombDelays(p, h * p.pitchMul);
                if (fade < 1.0 && p.main)
                    p.gain = (float) (p.zone->gain * zoneLevel(*p.zone) * l.level * fade);
            }
        }
    }

    void rezone(Lane& l) noexcept
    {
        Player* old = mainPlayer(l);
        if (old == nullptr || (l.art != Art::Sustain && l.art != Art::SlapNote))
            return;
        const double h = hz(lanePitch(l));
        // A slide from a recorded slap moves through the recorded slaps.
        const PackZone* z = selectZone(*pack_, l.art, l.art == Art::SlapNote ? 0 : l.variant, h, old->zone->layer, l.rr++,
                                       l.index);
        if (z == nullptr || z == old->zone)
            return;
        // Time-aligned: the new fret's recording at the same age as the old one.
        const double age = (old->pos - 1.0) / pack_->sampleRate;
        const float g = old->gain / (old->zone->gain * zoneLevel(*old->zone));
        Player* p = play(l, z, h, true, g, age, (int) (0.006 * fs_));
        fadeOutPlayer(*old, (int) (0.006 * fs_));
        if (p != nullptr)
        {
            p->main = true;
            p->env = old->env;
            p->envMul = old->envMul;
            p->filter = Filter::None;
        }
    }

    void tremoloRepick(Lane& l) noexcept
    {
        const bool up = (++l.tremoloCount % 2) != 0;
        const double h = hz(lanePitch(l));
        Player* old = mainPlayer(l);
        const int layer = old != nullptr ? old->zone->layer : 1;
        Player* p = play(l, selectZone(*pack_, Art::Sustain, l.variant, h, layer, l.rr++, l.index), h, true,
                         (float) (l.level * (up ? 0.9 : 1.0)), 0.0, 0);
        if (old != nullptr)
            fadeOutPlayer(*old, (int) (0.006 * fs_));
        if (p != nullptr)
            p->main = true;
    }

    // ---- render ----------------------------------------------------------------

    void renderPlayer(Player& p, float* out, int n) noexcept
    {
        const int16_t* x = p.data;
        const double last = (double) p.zone->length - 3.0;
        const float rel = p.released ? releaseMul_ : 1.0f;
        for (int i = 0; i < n; ++i)
        {
            if (p.pos >= last)
            {
                p.active = false;
                return;
            }
            const int idx = (int) p.pos;
            const float t = (float) (p.pos - idx);
            const float xm1 = x[idx - 1], x0 = x[idx], x1 = x[idx + 1], x2 = x[idx + 2];
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            float v = ((c3 * t + c2) * t + c1) * t + x0;
            if (p.clankIn > 0.0f)
            {
                const float u = v * p.clankIn;
                v = (u > 0.0f ? u / (1.0f + u) : u) * p.clankOut;
            }
            if (p.filter != Filter::None)
            {
                const float y = p.b0 * v + p.z1;
                p.z1 = p.b1 * v - p.a1 * y + p.z2;
                p.z2 = p.b2 * v - p.a2 * y;
                v = y;
            }
            if (p.conv)
                v = convert(p, v);
            if (p.bright)
            {
                const float h = p.hb0 * v + p.hz1;
                p.hz1 = p.hb1 * v - p.ha1 * h + p.hz2;
                p.hz2 = p.hb2 * v - p.ha2 * h;
                v += p.brightGain * h;
                p.brightGain = p.brightFloor + (p.brightGain - p.brightFloor) * p.brightMul;
            }
            float g = p.gain * p.env;
            if (p.fadeIn > 0)
            {
                g *= 1.0f - (float) p.fadeIn / (float) p.fadeInLen;
                --p.fadeIn;
            }
            if (p.fadingOut)
            {
                g *= (float) p.fadeOut / (float) p.fadeOutLen;
                if (--p.fadeOut <= 0)
                {
                    p.active = false;
                    return;
                }
            }
            out[i] += (float) (spec_.outputGain * kLevel) * g * v;
            p.env *= p.envMul * rel;
            p.step += p.stepInc;
            p.pos += p.step;
        }
        if (p.env < 1.0e-4f)
            p.active = false;
        if (std::fabs(p.z1) < 1.0e-20f) p.z1 = 0.0f;
        if (std::fabs(p.z2) < 1.0e-20f) p.z2 = 0.0f;
        if (std::fabs(p.hz1) < 1.0e-20f) p.hz1 = 0.0f;
        if (std::fabs(p.hz2) < 1.0e-20f) p.hz2 = 0.0f;
        if (p.conv)
        {
            if (std::fabs(p.rz1) < 1.0e-20f) p.rz1 = 0.0f;
            if (std::fabs(p.rz2) < 1.0e-20f) p.rz2 = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                if (std::fabs(p.bz1[i]) < 1.0e-20f) p.bz1[i] = 0.0f;
                if (std::fabs(p.bz2[i]) < 1.0e-20f) p.bz2[i] = 0.0f;
            }
        }
    }

    static constexpr double kLevel = 1.0;

    // The articulation pack's levels against a sustained note (fitted with
    // realism_eval.py on the held-out player: level_vs_open).
    static constexpr float kArtsPalmLevel = 1.0f, kArtsHarmonicLevel = 1.3f, kArtsPinchLevel = 1.8f;
    // A recorded slap against the bass's finger note (both peak-normalised by
    // the pack builder). The recorded slaps have a low crest factor (a
    // dark body under the impact): at 1.0 a slap's first 300 ms sit +3 dB RMS
    // over a finger note, where the #52 model's slap sat (measured with
    // realism_eval.py's renders, #109); its peak is then near a finger note's.
    static constexpr float kSlapNoteLevel = 1.0f;
    // Per-attack level spread (dB, uniform): the held-out round robins of a real
    // guitar spread 0.57 dB (sd) and 0.06 octave in brightness.
    static constexpr double kAttackLevelJitterDb = 0.6;
    // The articulation recordings' noise floor (a passive humbucker into an
    // interface) sits 10-20 dB over the Strat sets' above 5 kHz once the note
    // has decayed (measured, #52), and a high-gain rig would make it a hiss.
    // Their NOTES have little up there after the attack (flatwounds), so the
    // top closes as the note settles — the way a string's highest partials die
    // first — and a fixed low-pass far above the pick takes the rest.
    static constexpr double kArtsHissCutHz = 9000.0, kArtsTopHz = 4500.0, kArtsTopDb = -10.0, kArtsTopTau = 0.2;
    void quietArtsHiss(Player& p) const noexcept
    {
        setBiquad(p, Filter::LowPass, kArtsHissCutHz, 0.6, fs_);
        setBright(p, kArtsTopHz, 0.0, kArtsTopDb, kArtsTopTau);
    }

    InstrumentSpec spec_;
    const SamplePack* pack_ = nullptr;
    const SamplePack* arts_ = nullptr;
    ConversionPlan artsPlan_ {};
    ConversionPlan slapPlan_ {};   // the recorded slaps' (#109): another bass than the pack's notes
    double fs_ = 0.0;
    float releaseMul_ = 1.0f;
    std::array<Lane, kMaxStrings> lanes_ {};
    ConversionPlan plans_[2] {};   // per pack variant (#50); identity by default
    std::unique_ptr<std::array<CombRing, (size_t) kMaxStrings * kPlayers>> rings_;   // only when a plan converts
    int ctrlPhase_ = 0;
    int64_t samples_ = 0;
    uint32_t counter_ = 0;
};

} // namespace dcvoice
