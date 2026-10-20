#pragma once

// WHICH ZONES A CHART CAN PLAY (#417). The sampled voice decodes a lazy pack's
// zones only for the chart it is about to play: this planner reads the chart
// (strings, pitches, slides, bends, articulations) and marks, per pack, every
// zone the sampler (dcvoice_sampler.h) can select for one of its notes — the
// WHOLE pitch group (every velocity layer and round robin) at every semitone
// the note can sound at, plus `marginSemis` either side. One semitone is what
// the sampler itself reaches past the note (the one-take packs' neighbouring-
// fret round robin) and covers a fractional pitch between two queries; an
// EDIT needs no margin, because its upload decodes its zones before the engine
// publishes it (AudioEngine::setChartNotes). A zone the sampler asks for that is not marked
// is not a fault: it plays the nearest decoded recording and is decoded on a
// worker (selectZone, serviceZoneRequests), so this planner only has to be
// right for the render to be bit-identical, never for the voice to be safe.
//
// Mirrors the sampler's note-on rules (SampleInstrument::startNow, endLane,
// rezone, tremoloRepick): a change to which articulation or pitch a note
// selects there must be made here too — `testDemandCoversEveryNote` in the
// sample_voice test renders a chart through a lazy pack decoded with this
// plan and a fully decoded one and compares them sample for sample.
//
// CONTROL thread (allocates). Plain C++17 plus the C ABI header, no JUCE.

#include "../../include/dotcharter_audio.h"
#include "../dsp/voice/dcvoice_instrument.h"
#include "../dsp/voice/dcvoice_pack.h"
#include "ChartNoteTable.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dcsynth {

struct ZoneDemand
{
    std::vector<uint8_t> pack;   // per zone of the instrument's pack: 1 = the chart can play it
    std::vector<uint8_t> arts;   // per zone of the articulation pack (empty without one)
};

namespace demanddetail {
inline double hzOf(double midi) noexcept { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

/// Unique (pack, art, variant, lane, semitone) queries, resolved once each.
class QuerySet
{
public:
    static constexpr int kLo = -24, kHi = 160;   // semitone window (MIDI), clamped
    static constexpr int kSemis = kHi - kLo + 1;
    static constexpr int kLanes = dcvoice::IVoiceCore::kMaxStrings + 1;   // lane 0 = "any string"

    QuerySet() : bits_((size_t) 2 * dcvoice::kArtCount * 2 * kLanes * kSemis, 0) {}

    void add(int packId, dcvoice::Art art, int variant, int string, int semi)
    {
        semi = std::clamp(semi, kLo, kHi);
        bits_[index(packId, (int) art, variant & 1, string, semi)] = 1;
    }
    void addRange(int packId, dcvoice::Art art, int variant, int string, int lo, int hi)
    {
        for (int m = lo; m <= hi; ++m)
            add(packId, art, variant, string, m);
    }

    template <typename Fn>
    void forEach(Fn&& fn) const
    {
        for (int p = 0; p < 2; ++p)
            for (int a = 0; a < dcvoice::kArtCount; ++a)
                for (int v = 0; v < 2; ++v)
                    for (int lane = 0; lane < kLanes; ++lane)
                        for (int m = kLo; m <= kHi; ++m)
                            if (bits_[index(p, a, v, lane - 1, m)] != 0)
                                fn(p, (dcvoice::Art) a, v, lane - 1, m);
    }

private:
    static size_t index(int p, int a, int v, int string, int m)
    {
        const int lane = std::clamp(string, -1, dcvoice::IVoiceCore::kMaxStrings - 1) + 1;
        return ((((size_t) p * dcvoice::kArtCount + (size_t) a) * 2 + (size_t) v) * kLanes + (size_t) lane) * kSemis
               + (size_t) (m - kLo);
    }
    std::vector<uint8_t> bits_;
};
} // namespace demanddetail

/// The zones of `pack` (and of the articulation pack `arts`, may be null) that
/// `table` can play on a guitar (`bass` false) or bass voice. Control thread.
inline ZoneDemand planZoneDemand(const ChartNoteTable& table, const dcvoice::SamplePack& pack, const dcvoice::SamplePack* arts,
                                 bool bass, int marginSemis = 1)
{
    using dcvoice::Art;
    constexpr int kPack = 0, kArts = 1;
    demanddetail::QuerySet q;
    bool anyDead = false, anyHarmonic = false, anySlap = false;
    const int margin = std::max(marginSemis, 1);   // >= 1: a pitch between two integer queries is covered
    const bool artsPalm = arts != nullptr && arts->has(Art::Palm, 0);
    const bool artsHarmonic = arts != nullptr && arts->has(Art::NatHarm, 0);
    const bool artsPinch = arts != nullptr && arts->has(Art::Pinch, 0);
    // LINK_NEXT of the last two notes per string: GuitarChartVoice ties a note
    // to the previous one on its string (two, in case the scheduler dropped one).
    bool linkNext[16][2] {};
    for (int32_t i = 0; i < table.count(); ++i)
    {
        const dca_chart_note& n = table.notes[(size_t) i];
        const uint32_t f = n.flags;
        const int lane = std::clamp((int) n.string_index, 0, dcvoice::IVoiceCore::kMaxStrings - 1);
        const int link = std::clamp((int) n.string_index, 0, 15);
        const bool linked = linkNext[link][0] || linkNext[link][1];
        linkNext[link][1] = linkNext[link][0];
        linkNext[link][0] = (f & DCA_CHART_NOTE_LINK_NEXT) != 0;
        // Every pitch the note can sound at: the slide's end, the bend curve.
        double lo = n.pitch, hi = n.pitch;
        if (n.slide_to_fret >= 0.0f)
        {
            const double end = (double) n.pitch + (double) (n.slide_to_fret - (float) n.fret);
            lo = std::min(lo, end);
            hi = std::max(hi, end);
        }
        double bendLo = 0.0, bendHi = 0.0;
        for (int32_t k = 0; k < n.bend_count && n.bend_first >= 0; ++k)
        {
            const double s = table.bends[(size_t) (n.bend_first + k)].semitones;
            bendLo = std::min(bendLo, s);
            bendHi = std::max(bendHi, s);
        }
        lo += bendLo;
        hi += bendHi;
        // Vibrato adds a quarter semitone at most: inside the margin.
        const int mLo = (int) std::floor(lo) - margin, mHi = (int) std::ceil(hi) + margin;
        const int variant = bass && (f & DCA_CHART_NOTE_PICKED) ? 1 : 0;

        // The sustain at the note's pitch (and its release noise) — unless the
        // sampler's note-on (SampleInstrument::startNow, in its order) certainly
        // plays something else: a guitar's dead note, or a palm mute / natural /
        // pinch harmonic the articulation pack recorded. A legato entry on a
        // ringing string, a tie and a tremolo re-pick play the sustain whatever
        // the other flags say.
        bool ownSustain = true;
        if (! (f & (DCA_CHART_NOTE_HAMMER_ON | DCA_CHART_NOTE_PULL_OFF | DCA_CHART_NOTE_TAP | DCA_CHART_NOTE_TREMOLO)) && ! linked)
        {
            if (bass)
                ownSustain = (f & (DCA_CHART_NOTE_SLAP | DCA_CHART_NOTE_POP)) != 0 || ! (f & DCA_CHART_NOTE_FRET_HAND_MUTE);
            else if (f & DCA_CHART_NOTE_FRET_HAND_MUTE)
                ownSustain = false;
            else if (f & DCA_CHART_NOTE_PALM_MUTE)
                ownSustain = ! artsPalm;
            else if (f & DCA_CHART_NOTE_HARMONIC)
                ownSustain = false;   // the recorded harmonic, or the sustain at ITS pitch (below)
            else if (f & DCA_CHART_NOTE_PINCH_HARMONIC)
                ownSustain = ! artsPinch;
        }
        if (ownSustain)
        {
            q.addRange(kPack, Art::Sustain, variant, lane, mLo, mHi);
            q.addRange(kPack, Art::Release, 0, -1, mLo, mHi);
        }
        if (f & (DCA_CHART_NOTE_HAMMER_ON | DCA_CHART_NOTE_TAP))
            q.addRange(kPack, Art::Legato, 0, lane, mLo, mHi);
        if (bass && (f & (DCA_CHART_NOTE_SLAP | DCA_CHART_NOTE_POP)))
        {
            anySlap = true;
            q.addRange(kPack, Art::SlapNote, 0, lane, mLo, mHi);
            q.addRange(kPack, Art::Sustain, 0, lane, mLo, mHi);   // the slap / pop models' variants
            q.addRange(kPack, Art::Sustain, 1, lane, mLo, mHi);
        }
        if (f & DCA_CHART_NOTE_FRET_HAND_MUTE)
        {
            if (bass)
                q.addRange(kPack, Art::Ghost, 0, -1, mLo, mHi);
            else
                anyDead = true;
        }
        if (f & DCA_CHART_NOTE_PALM_MUTE)
        {
            q.addRange(kPack, Art::Palm, variant, lane, mLo, mHi);
            if (arts != nullptr)
                q.addRange(kArts, Art::Palm, 0, lane, mLo, mHi);
        }
        // Its harmonic pitch through the sustain: without a recorded harmonic,
        // and for a harmonic entered legato / tied / re-picked.
        const bool harmonicSustain = ! artsHarmonic
                                     || (f & (DCA_CHART_NOTE_HAMMER_ON | DCA_CHART_NOTE_PULL_OFF | DCA_CHART_NOTE_TAP | DCA_CHART_NOTE_TREMOLO)) != 0
                                     || linked;
        if ((f & DCA_CHART_NOTE_HARMONIC) && ! harmonicSustain)
            anyHarmonic = true;
        else if (f & DCA_CHART_NOTE_HARMONIC)
        {
            anyHarmonic = true;
            // Without a recorded harmonic: the recording AT the harmonic's pitch.
            dcvoice::NoteStart ns;
            ns.fret = n.fret;
            ns.capo = n.capo;
            ns.pitch = n.pitch;
            // Its slide and bends move it like any note; its release noise too.
            const double h = dcvoice::vibratingOpenPitch(ns) + 12.0 * std::log2((double) dcvoice::naturalHarmonicNumber(ns));
            const int hLo = (int) std::floor(h + (lo - n.pitch)) - margin, hHi = (int) std::ceil(h + (hi - n.pitch)) + margin;
            q.addRange(kPack, Art::Sustain, variant, lane, hLo, hHi);
            q.addRange(kPack, Art::Release, 0, -1, hLo, hHi);
        }
        if (f & DCA_CHART_NOTE_PINCH_HARMONIC)
        {
            if (arts != nullptr)
                q.addRange(kArts, Art::Pinch, 0, lane, mLo, mHi);
            // Without a recorded pinch: the recording k * f0 (<= 1200 Hz) over it.
            for (int m = mLo; m <= mHi; ++m)
            {
                const double h = demanddetail::hzOf(m);
                const int kp = std::clamp((int) std::lround(1800.0 / std::max(h, 1.0)), 3, 8);
                const double hk = std::min(h * kp, 1200.0);
                const int mk = (int) std::lround(69.0 + 12.0 * std::log2(hk / 440.0));
                q.addRange(kPack, Art::Sustain, variant, lane, mk - 1, mk + 1);
            }
        }
    }

    ZoneDemand d;
    d.pack.assign(pack.zones.size(), 0);
    if (arts != nullptr)
        d.arts.assign(arts->zones.size(), 0);
    q.forEach([&](int packId, Art art, int variant, int string, int semi) {
        if (packId == kArts && arts == nullptr)
            return;
        const dcvoice::SamplePack& p = packId == kArts ? *arts : pack;
        dcvoice::markGroup(p, art, variant, demanddetail::hzOf(semi), string, packId == kArts ? d.arts : d.pack);
    });
    // The small articulations go whole: a dead note picks by the open string
    // it was muted on, a slap's thumb is one unpitched set, and a recorded
    // harmonic is chosen by harmonic number and string before pitch.
    if (anyDead)
        dcvoice::markArt(pack, Art::Dead, d.pack);
    if (anySlap)
        dcvoice::markArt(pack, Art::Slap, d.pack);
    if (anyHarmonic && arts != nullptr)
        dcvoice::markArt(*arts, Art::NatHarm, d.arts);
    return d;
}

/// Bytes of decoded zones `have` that `want` does not ask for (what a fresh
/// pack would not hold). Control thread.
inline int64_t staleZoneBytes(const dcvoice::SamplePack& pack, const std::vector<uint8_t>& want)
{
    if (pack.lazy == nullptr)
        return 0;
    int64_t stale = 0;
    for (size_t i = 0; i < pack.zones.size(); ++i)
        if (pack.ready(pack.zones[i]) && (i >= want.size() || want[i] == 0))
            stale += (int64_t) pack.zones[i].length * 2;
    return stale;
}

} // namespace dcsynth
