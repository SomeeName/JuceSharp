#pragma once

// The built-in guitar and bass as chart-source VOICES (issue #39 step 1 + the
// bass step): a field-for-field adapter from dcsynth::ChartVoice (ABI v38 —
// issue #39 step 0) onto a dcvoice core (src/dsp/voice/). All sound is made by
// the core; this file only maps.
//
// THE CORE: real recordings when the model's sample pack is found (`voices/`
// beside juce_sharp.dll — see dcvoice_pack_store.h), loaded once in prepare()
// on the control thread and shared; the waveguide string model when it is not
// (a dev tree without the pack, a broken install), so the source never goes
// silent.
//
// THE INSTRUMENT MODEL (#50): a voice is built for ONE InstrumentConfig —
// model, pickup selector, coil split (dcvoice_models.h). prepare() loads ONLY
// the pack that selection is converted from (a Strat position, the Gretsch
// set, the bass set) and gives the core a ConversionPlan per pack variant; a
// model whose Strat pack is missing falls back to the Gretsch set converted to
// the same target. Changing the selection is a NEW voice: the engine swaps it
// in and cross-fades (fadeOutAll on the old one), and the old voice — with
// the last reference to its pack — is destroyed on the control thread.
//
// WHAT THE VOICE DECIDES HERE (the scheduler hands it over):
//   * STRUM SPREAD: chordIndex/chordSize/strum -> a per-string attack delay,
//     8 ms down / 6 ms up / 3 ms palm-muted. DCA_CHART_STRUM_UNKNOWN falls back
//     to: palm-muted or accented -> down; else alternate with the previous
//     chord when it was under 0.3 s (wall) ago; else down. The beat-grid
//     heuristic (dcvoice_plan.h) is better and belongs to the host that fills
//     `strum`.
//   * SLIDE SHAPE: the scheduler's slide is a straight line over the sustain;
//     the finger here moves from 35 % to 95 % of it (45 % for an unpitched
//     slide out) and the core quantises it to frets.
//   * VIBRATO depth: dca_chart_note carries the flag, not VibratoStrength, so
//     depth is the strength-80 default (0.25 semitone guitar, 0.2 bass; the
//     held-out players measured 18-25 cents peak to peak, #52).
//   * TREMOLO: re-picked by the core at 14 Hz x practice speed.

#include "ChartVoice.h"
#include "ChartZoneDemand.h"
#include "../../include/dotcharter_audio.h"
#include "../dsp/voice/dcvoice_excitation_data.h"
#include "../dsp/voice/dcvoice_instrument.h"
#include "../dsp/voice/dcvoice_models.h"
#include "../dsp/voice/dcvoice_pack_store.h"
#include "../dsp/voice/dcvoice_sampler.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace dcsynth {

class GuitarChartVoice final : public ChartVoice
{
public:
    explicit GuitarChartVoice(bool bass) : GuitarChartVoice(bass, dcvoice::legacyConfig(bass)) {}

    GuitarChartVoice(bool bass, const dcvoice::InstrumentConfig& config) : bass_(bass), config_(config)
    {
        if (! dcvoice::validConfig(config_, bass_))
            config_ = dcvoice::legacyConfig(bass_);
        spec_ = bass ? dcvoice::InstrumentSpec::bassGuitar(5) : dcvoice::InstrumentSpec::guitar();
        // A chart may use up to 8 strings (string_index is validated 0..15;
        // lanes past the last real string reuse its geometry). Pitch arrives
        // explicitly, so the spec's tuning only seeds the waveguide's tables.
        for (int s = spec_.numStrings; s < dcvoice::IVoiceCore::kMaxStrings; ++s)
        {
            spec_.openHz[(size_t) s] = spec_.openHz[(size_t) spec_.numStrings - 1];
            spec_.stiffnessB[(size_t) s] = spec_.stiffnessB[(size_t) spec_.numStrings - 1];
        }
        spec_.numStrings = dcvoice::IVoiceCore::kMaxStrings;
    }

    const char* name() const noexcept override
    {
        if (pack_ != nullptr)
            return bass_ ? "sampled-bass" : "sampled-guitar";
        return bass_ ? "waveguide-bass" : "waveguide-guitar";
    }

    /// The selection this voice was built for.
    const dcvoice::InstrumentConfig& config() const noexcept { return config_; }
    /// The pack it plays ("" = the waveguide fallback).
    const std::string& packName() const noexcept { return packName_; }
    /// Bytes of the pack it holds (0 for the waveguide).
    int64_t packBytes() const noexcept override
    {
        return (pack_ != nullptr ? (int64_t) pack_->bytes() : 0) + (arts_ != nullptr ? (int64_t) arts_->bytes() : 0);
    }
    /// True when the shared articulation pack (real palm mutes, harmonics) plays.
    bool hasArticulations() const noexcept { return arts_ != nullptr; }

    void useLazyZones(bool fresh) override
    {
        if (core_ == nullptr)
            load_ = fresh ? dcvoice::PackLoad::LazyFresh : dcvoice::PackLoad::Lazy;
    }

    void preloadZones(const ChartNoteTable& table) override
    {
        if (pack_ == nullptr || pack_->lazy == nullptr)
            return;
        const ZoneDemand d = planZoneDemand(table, *pack_, arts_.get(), bass_);
        dcvoice::ensureZones(*pack_, d.pack);
        if (arts_ != nullptr)
            dcvoice::ensureZones(*arts_, d.arts);
    }

    int64_t staleZoneBytes(const ChartNoteTable& table) const override
    {
        if (pack_ == nullptr || pack_->lazy == nullptr)
            return 0;
        const ZoneDemand d = planZoneDemand(table, *pack_, arts_.get(), bass_);
        return dcsynth::staleZoneBytes(*pack_, d.pack) + (arts_ != nullptr ? dcsynth::staleZoneBytes(*arts_, d.arts) : 0);
    }

    void lazyPacks(std::vector<std::shared_ptr<const dcvoice::SamplePack>>& out) const override
    {
        if (pack_ != nullptr && pack_->lazy != nullptr)
            out.push_back(pack_);
        if (arts_ != nullptr && arts_->lazy != nullptr)
            out.push_back(arts_);
    }

    /// CONTROL thread: loads (or shares) the selection's sample pack the first
    /// time, then (re)prepares the core.
    void prepare(double sampleRate, int maxBlockSize) override
    {
        if (core_ == nullptr)
            build();
        core_->prepare(sampleRate, maxBlockSize);
        lastChordSample_ = -1000000000;
        lastChordUp_ = true;
        lastChordId_ = -1;
    }

    /// True when real recordings are playing (false: the waveguide fallback).
    bool sampled() const noexcept override { return pack_ != nullptr; }

    /// True when every pack variant (and the recorded slaps, #109) plays its
    /// recordings unconverted.
    bool recordedAsIs() const noexcept
    {
        return sampler_ != nullptr && sampler_->conversion(0).identity && sampler_->conversion(1).identity
               && sampler_->slapConversion().identity;
    }

    /// The sample player when recordings are playing (nullptr: the waveguide).
    /// Read-only, for tests and tools.
    const dcvoice::SampleInstrument* sampler() const noexcept { return sampler_; }

    void fadeOutAll(int samples) noexcept override
    {
        if (sampler_ != nullptr)
            sampler_->fadeOutAll(samples);
        else if (core_ != nullptr)
            core_->allNotesOff(true);
        for (bool& b : linkedNext_)
            b = false;
    }

    void reset() noexcept override { if (core_) core_->reset(); }

    void noteOn(const ChartNoteOn& n) noexcept override
    {
        if (core_ == nullptr)
            return;   // never prepared: step 0 always prepares before publishing
        const uint32_t f = n.flags;
        uint32_t flags = 0;
        if (f & DCA_CHART_NOTE_PALM_MUTE)       flags |= dcvoice::kFlagPalmMute;
        if (f & DCA_CHART_NOTE_FRET_HAND_MUTE)  flags |= dcvoice::kFlagFullMute;
        if (f & DCA_CHART_NOTE_HARMONIC)        flags |= dcvoice::kFlagHarmonic;
        if (f & DCA_CHART_NOTE_PINCH_HARMONIC)  flags |= dcvoice::kFlagPinchHarmonic;
        if (f & DCA_CHART_NOTE_HAMMER_ON)       flags |= dcvoice::kFlagHammerOn;
        if (f & DCA_CHART_NOTE_PULL_OFF)        flags |= dcvoice::kFlagPullOff;
        if (f & DCA_CHART_NOTE_TAP)             flags |= dcvoice::kFlagTap;
        if (f & DCA_CHART_NOTE_TREMOLO)         flags |= dcvoice::kFlagTremolo;
        if (f & DCA_CHART_NOTE_ACCENT)          flags |= dcvoice::kFlagAccent;
        if (f & DCA_CHART_NOTE_UNPITCHED_SLIDE) flags |= dcvoice::kFlagUnpitchedSlide;
        if (f & DCA_CHART_NOTE_SLAP)            flags |= dcvoice::kFlagSlap;
        if (f & DCA_CHART_NOTE_POP)             flags |= dcvoice::kFlagPop;
        if (f & DCA_CHART_NOTE_PICKED)          flags |= dcvoice::kFlagPicked;
        if (f & DCA_CHART_NOTE_GHOST)           flags |= dcvoice::kFlagGhost;
        if (f & DCA_CHART_NOTE_LET_RING)        flags |= dcvoice::kFlagLetRing;
        if (f & DCA_CHART_NOTE_END_DAMPED)      flags |= dcvoice::kFlagEndDamped;
        // LINK_NEXT on the PREVIOUS note means this one has no attack; the
        // ABI flags the earlier note, so a linked continuation arrives as a
        // plain note on a still-ringing string — treated as legato below.
        if (linkedNext_[(size_t) std::clamp(n.stringIndex, 0, 15)])
            flags |= dcvoice::kFlagLinked;
        linkedNext_[(size_t) std::clamp(n.stringIndex, 0, 15)] = (f & DCA_CHART_NOTE_LINK_NEXT) != 0;

        dcvoice::NoteStart s;
        s.id = n.noteId;
        s.string = std::clamp(n.stringIndex, 0, dcvoice::IVoiceCore::kMaxStrings - 1);
        s.fret = n.fret;
        s.capo = n.capo;   // ABI v39: harmonic nodes count from the capo (#51)
        s.pitch = n.pitch;
        s.velocity = n.velocity;
        s.vibratoSemis = (f & DCA_CHART_NOTE_VIBRATO) ? (bass_ ? 0.2f : 0.25f) : 0.0f;
        s.slideToSemis = n.slideToPitch - n.pitch;
        s.initialBend = n.initialBendSemitones;
        s.initialSlide = shapeSlide(n.initialSlideSemitones, s.slideToSemis, (f & DCA_CHART_NOTE_UNPITCHED_SLIDE) != 0);
        s.speed = n.speed;
        s.chaseSeconds = n.chaseSeconds;
        slideTotal_[(size_t) s.string] = s.slideToSemis;
        unpitched_[(size_t) s.string] = (f & DCA_CHART_NOTE_UNPITCHED_SLIDE) != 0;

        if (n.chordSize > 1 && n.chaseSeconds <= 0.0)
        {
            bool up;
            if (n.chordId != lastChordId_)
            {
                if (n.strum == DCA_CHART_STRUM_UP)
                    up = true;
                else if (n.strum == DCA_CHART_STRUM_DOWN || (f & (DCA_CHART_NOTE_PALM_MUTE | DCA_CHART_NOTE_ACCENT)))
                    up = false;
                else
                    up = (core_->samplesRendered() - lastChordSample_) < (int64_t) (0.3 * core_->sampleRate()) ? ! lastChordUp_ : false;
                lastChordId_ = n.chordId;
                lastChordUp_ = up;
                lastChordSample_ = core_->samplesRendered();
            }
            up = lastChordUp_;
            const int rank = up ? (n.chordSize - 1 - n.chordIndex) : n.chordIndex;
            const double ms = (f & DCA_CHART_NOTE_PALM_MUTE) ? 3.0 : (up ? 6.0 : 8.0);
            s.delaySamples = (int) std::lround(strumDelayMs(rank, ms, n.chordId, n.stringIndex) * 1.0e-3 * core_->sampleRate());
            s.velocity *= strumVelocityScale(n.chordId, n.stringIndex);
            if (up)
            {
                s.flags |= dcvoice::kFlagUpStroke;
                s.velocity *= 0.85f;
            }
        }
        s.flags |= flags;
        core_->noteOn(s);
    }

    void noteOff(int32_t noteId, NoteOffKind kind) noexcept override
    {
        if (core_ != nullptr)
            core_->noteOff(noteId, kind == NoteOffKind::Choke);
    }

    void allNotesOff(NoteOffKind kind) noexcept override
    {
        if (core_ != nullptr)
            core_->allNotesOff(kind == NoteOffKind::Choke);
        for (bool& b : linkedNext_)
            b = false;
    }

    void setNoteControls(int32_t noteId, const ChartNoteControls& c) noexcept override
    {
        if (core_ == nullptr)
            return;
        int string = -1;
        for (int s = 0; s < dcvoice::IVoiceCore::kMaxStrings; ++s)
            if (core_->stringNoteId(s) == noteId)
                string = s;
        const float slide = string >= 0 ? shapeSlide(c.slideSemitones, slideTotal_[(size_t) string], unpitched_[(size_t) string])
                                        : c.slideSemitones;
        core_->setControls(noteId, c.bendSemitones, slide, 32);
    }

    void render(float* monoOut, int numSamples) noexcept override
    {
        if (core_ != nullptr)
            core_->render(monoOut, numSamples);
    }

    int activeNotes() const noexcept override { return core_ != nullptr ? core_->activeNotes() : 0; }

    /// A strummed string's attack delay (ms): rank x the per-string spread, and
    /// (#52) a hand, not a sequencer — every string after the first lands up to
    /// +-1.5 ms off the even spread. Deterministic per chord and string.
    static double strumDelayMs(int rank, double msPerString, int32_t chordId, int32_t stringIndex) noexcept
    {
        const double jitter = rank > 0 ? kStrumJitterMs * dcvoice::hashUnit((uint32_t) chordId, (uint32_t) stringIndex + 7u) : 0.0;
        return std::max(0.0, rank * msPerString + jitter);
    }
    /// ...and each string is hit up to +-8 % harder or softer.
    static float strumVelocityScale(int32_t chordId, int32_t stringIndex) noexcept
    {
        return (float) (1.0 + kStrumVelocitySpread * dcvoice::hashUnit((uint32_t) chordId + 991u, (uint32_t) stringIndex));
    }
    static constexpr double kStrumJitterMs = 1.5, kStrumVelocitySpread = 0.08;

private:
    /// CONTROL thread: the pack for the selection and a plan per variant.
    void build()
    {
        const dcvoice::ModelSpec* model = dcvoice::findModel(config_.model);
        const dcvoice::PositionSpec* pos = model != nullptr ? dcvoice::findPosition(*model, config_.pickup) : nullptr;
        std::string error;
        if (model != nullptr && pos != nullptr)
        {
            packName_ = pos->sourcePack;
            pack_ = dcvoice::acquirePack(packName_.c_str(), error, load_);
            if (pack_ == nullptr && ! bass_ && packName_ != "guitar")
            {
                // A Strat position not installed: the Gretsch set, converted.
                packName_ = "guitar";
                pack_ = dcvoice::acquirePack("guitar", error, load_);
            }
        }
        if (pack_ != nullptr)
        {
            // The guitar's real palm mutes and harmonics: one pack every guitar
            // model shares, converted to its pickup. Optional — without it the
            // core approximates them from the model's own recordings.
            if (! bass_)
            {
                std::string artsError;
                arts_ = dcvoice::acquirePack("guitar-arts", artsError, load_);
            }
            auto sampler = std::make_unique<dcvoice::SampleInstrument>(spec_, pack_.get(), arts_.get());
            const dcvoice::PickupSound& target = config_.coilSplit ? pos->split : pos->full;
            for (int variant = 0; variant < 2; ++variant)
                sampler->setConversion(variant, dcvoice::makeConversionPlan(*model, target, packName_.c_str(), variant));
            if (arts_ != nullptr)
                sampler->setArtsConversion(dcvoice::makeConversionPlan(*model, target, "guitar-arts", 0));
            // The recorded slaps (#109) were made on a third bass: their own plan.
            if (bass_ && pack_->has(dcvoice::Art::SlapNote, 0))
                sampler->setSlapConversion(dcvoice::makeConversionPlan(*model, target, "bass-slap", 0));
            sampler_ = sampler.get();
            core_ = std::move(sampler);
        }
        else
        {
            packName_.clear();
            core_ = std::make_unique<dcvoice::StringInstrument>(
                spec_, bass_ ? dcvoice::bassExcitations() : dcvoice::guitarExcitations());
        }
    }

    /// The scheduler's straight line over the sustain -> the finger's move.
    static float shapeSlide(float linear, float total, bool unpitched) noexcept
    {
        if (total == 0.0f)
            return 0.0f;
        const double p = std::clamp((double) linear / total, 0.0, 1.0);
        const double from = unpitched ? 0.45 : 0.35;
        return (float) (total * std::clamp((p - from) / (0.95 - from), 0.0, 1.0));
    }

    bool bass_;
    dcvoice::InstrumentConfig config_;
    std::string packName_;
    dcvoice::InstrumentSpec spec_;
    std::shared_ptr<const dcvoice::SamplePack> pack_;
    std::shared_ptr<const dcvoice::SamplePack> arts_;   // the shared articulation pack (guitar only, optional)
    dcvoice::PackLoad load_ = dcvoice::PackLoad::Eager;  // the engine asks for Lazy (#417); tools and tests stay eager
    std::unique_ptr<dcvoice::IVoiceCore> core_;
    dcvoice::SampleInstrument* sampler_ = nullptr;   // core_, when it plays recordings
    float slideTotal_[dcvoice::IVoiceCore::kMaxStrings] {};
    bool unpitched_[dcvoice::IVoiceCore::kMaxStrings] {};
    bool linkedNext_[16] {};
    int64_t lastChordSample_ = -1000000000;
    bool lastChordUp_ = true;
    int32_t lastChordId_ = -1;
};

} // namespace dcsynth
