#pragma once

// The chart source's uploaded notes (ABI v38), in the form the audio thread
// reads: immutable once built. Built and validated on the CONTROL thread by
// `ChartNoteTable::build`; published to the callback by an atomic pointer swap
// in AudioEngine and freed on the control thread after a Dekker handshake,
// exactly like a re-amp take. Nothing here may be called from the audio thread
// except the const readers (`lowerBound`, `bendAt`, `slideAt`).
//
// Plain C++17 plus the C ABI header — no JUCE — so the scheduler tests link it
// without the engine.

#include "../../include/dotcharter_audio.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dcsynth {

/// Upload caps. Generous for a chart (a long, dense one is ~5,000 notes) and
/// small enough that one copy cannot exhaust memory.
constexpr int32_t kMaxChartNotes      = 262144;
constexpr int32_t kMaxChartBendPoints = 1048576;
/// A slide with no authored sustain is drawn over this long.
constexpr double  kDefaultSlideSeconds = 0.1;

static_assert(sizeof(dca_chart_note) == DCA_CHART_NOTE_V38_BYTES,
              "dca_chart_note changed size: it is append-only, and the V38 constant is permanent");
static_assert(offsetof(dca_chart_note, pitch) == 24 && offsetof(dca_chart_note, flags) == 44
              && offsetof(dca_chart_note, strum) == 60 && offsetof(dca_chart_note, capo) == 64
              && offsetof(dca_chart_note, reserved) == 68,
              "dca_chart_note layout moved — the managed mirror pins these offsets");
static_assert(sizeof(dca_chart_bend_point) == 16, "dca_chart_bend_point is 16 bytes");

struct ChartNoteTable
{
    std::vector<dca_chart_note>       notes;      // sorted by start_seconds
    std::vector<dca_chart_bend_point> bends;
    std::vector<int32_t>              chordSize;  // per note; 1 for a single note
    std::vector<int32_t>              chordIndex; // per note; 0 = lowest string of its chord
    double   maxDurationSeconds = 0.0;            // max(release - start): bounds the chase scan
    uint64_t serial             = 0;              // set by the publisher; "is this a new table"

    int32_t count() const noexcept { return (int32_t) notes.size(); }

    /// First index whose start is >= t (count() when none). O(log n), no allocation.
    int32_t lowerBound(double t) const noexcept
    {
        int32_t lo = 0, hi = count();
        while (lo < hi)
        {
            const int32_t mid = lo + (hi - lo) / 2;
            if (notes[(size_t) mid].start_seconds < t) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    /// The bend (semitones) of note `i` at chart time `t`. Linear between
    /// points, held after the last; from the attack to the first point it ramps
    /// up from 0 — unless that first point sits ON the attack (within 1 ms),
    /// which is a pre-bend and holds from the start.
    float bendAt(int32_t i, double t) const noexcept
    {
        const auto& n = notes[(size_t) i];
        if (n.bend_count <= 0 || n.bend_first < 0) return 0.0f;
        const dca_chart_bend_point* p = bends.data() + n.bend_first;
        const int32_t cnt = n.bend_count;
        if (t <= p[0].time_seconds)
        {
            const double span = p[0].time_seconds - n.start_seconds;
            if (span <= 1.0e-3) return p[0].semitones;           // pre-bend
            const double f = (t - n.start_seconds) / span;
            return (float) (std::clamp(f, 0.0, 1.0) * (double) p[0].semitones);
        }
        for (int32_t k = 1; k < cnt; ++k)
        {
            if (t <= p[k].time_seconds)
            {
                const double span = p[k].time_seconds - p[k - 1].time_seconds;
                const double f = span > 0.0 ? (t - p[k - 1].time_seconds) / span : 1.0;
                return (float) ((double) p[k - 1].semitones
                                + std::clamp(f, 0.0, 1.0) * (double) (p[k].semitones - p[k - 1].semitones));
            }
        }
        return p[cnt - 1].semitones;
    }

    /// The slide (semitones) of note `i` at chart time `t`: a straight line from
    /// the fret to `slide_to_fret` over the authored sustain (kDefaultSlideSeconds
    /// when it has none). 0 for a note without a slide.
    float slideAt(int32_t i, double t) const noexcept
    {
        const auto& n = notes[(size_t) i];
        if (n.slide_to_fret < 0.0f) return 0.0f;
        const double len = n.sustain_seconds > 0.0 ? n.sustain_seconds : kDefaultSlideSeconds;
        const double f = std::clamp((t - n.start_seconds) / len, 0.0, 1.0);
        return (float) (f * (double) (n.slide_to_fret - (float) n.fret));
    }

    /// Validate and copy an upload. Returns null and fills `error` when ANY rule
    /// is broken — a list is accepted whole or not at all. `stride` is the
    /// caller's element size (append-extensible, see dotcharter_audio.h).
    static std::unique_ptr<ChartNoteTable> build(const void* noteBytes, int32_t count, int32_t stride,
                                                 const dca_chart_bend_point* bendPts, int32_t bendCount,
                                                 std::string& error)
    {
        error.clear();
        if (count < 0 || count > kMaxChartNotes) { error = "note_count out of range"; return nullptr; }
        if (bendCount < 0 || bendCount > kMaxChartBendPoints) { error = "bend_count out of range"; return nullptr; }
        if (count > 0 && noteBytes == nullptr) { error = "notes is null"; return nullptr; }
        if (bendCount > 0 && bendPts == nullptr) { error = "bends is null"; return nullptr; }
        if (stride < DCA_CHART_NOTE_V38_BYTES) { error = "note_stride_bytes below DCA_CHART_NOTE_V38_BYTES"; return nullptr; }

        auto t = std::make_unique<ChartNoteTable>();
        t->notes.resize((size_t) count);
        const size_t copyBytes = std::min((size_t) stride, sizeof(dca_chart_note));
        const auto* src = static_cast<const unsigned char*>(noteBytes);
        for (int32_t i = 0; i < count; ++i)
        {
            dca_chart_note n{};
            std::memcpy(&n, src + (size_t) i * (size_t) stride, copyBytes);
            t->notes[(size_t) i] = n;
        }
        if (bendCount > 0) t->bends.assign(bendPts, bendPts + bendCount);

        for (int32_t b = 0; b < bendCount; ++b)
        {
            const auto& p = t->bends[(size_t) b];
            if (! std::isfinite(p.time_seconds) || ! std::isfinite(p.semitones)
                || std::abs(p.semitones) > 24.0f)
            { error = "bend point " + std::to_string(b) + " is not finite or exceeds 24 semitones"; return nullptr; }
        }

        double prevStart = 0.0;
        for (int32_t i = 0; i < count; ++i)
        {
            const auto& n = t->notes[(size_t) i];
            const std::string at = "note " + std::to_string(i);
            if (! std::isfinite(n.start_seconds) || ! std::isfinite(n.release_seconds)
                || ! std::isfinite(n.sustain_seconds) || n.start_seconds < 0.0
                || n.sustain_seconds < 0.0)
            { error = at + ": a time is not finite or is negative"; return nullptr; }
            if (n.release_seconds < n.start_seconds) { error = at + ": release before start"; return nullptr; }
            if (i > 0 && n.start_seconds < prevStart) { error = at + ": notes are not sorted by start"; return nullptr; }
            prevStart = n.start_seconds;
            if (! std::isfinite(n.pitch) || n.pitch < 0.0f || n.pitch > 127.0f)
            { error = at + ": pitch outside 0..127"; return nullptr; }
            if (! std::isfinite(n.velocity) || n.velocity < 0.0f || n.velocity > 1.0f)
            { error = at + ": velocity outside 0..1"; return nullptr; }
            if (n.string_index < 0 || n.string_index > 15) { error = at + ": string_index outside 0..15"; return nullptr; }
            if (n.fret < 0 || n.fret > 36) { error = at + ": fret outside 0..36"; return nullptr; }
            if (n.capo < 0 || n.capo > 24) { error = at + ": capo outside 0..24"; return nullptr; }
            if (! std::isfinite(n.slide_to_fret) || n.slide_to_fret > 36.0f)
            { error = at + ": slide_to_fret not finite or past fret 36"; return nullptr; }
            if (n.bend_count < 0 || (n.bend_count > 0 && (n.bend_first < 0
                || (int64_t) n.bend_first + n.bend_count > (int64_t) bendCount)))
            { error = at + ": bend range outside the bend array"; return nullptr; }
            for (int32_t k = 1; k < n.bend_count; ++k)
                if (t->bends[(size_t) (n.bend_first + k)].time_seconds
                    < t->bends[(size_t) (n.bend_first + k - 1)].time_seconds)
                { error = at + ": bend points are not in time order"; return nullptr; }
            t->maxDurationSeconds = std::max(t->maxDurationSeconds, n.release_seconds - n.start_seconds);
        }

        // Chord membership: size and the position of each note by string, so a
        // voice can spread a strum without the host inventing a direction.
        t->chordSize .assign((size_t) count, 1);
        t->chordIndex.assign((size_t) count, 0);
        std::unordered_map<int32_t, std::vector<int32_t>> chords;
        for (int32_t i = 0; i < count; ++i)
            if (t->notes[(size_t) i].chord_id >= 0) chords[t->notes[(size_t) i].chord_id].push_back(i);
        for (auto& kv : chords)
        {
            auto& members = kv.second;
            std::stable_sort(members.begin(), members.end(), [&](int32_t a, int32_t b)
            { return t->notes[(size_t) a].string_index < t->notes[(size_t) b].string_index; });
            for (size_t k = 0; k < members.size(); ++k)
            {
                t->chordSize [(size_t) members[k]] = (int32_t) members.size();
                t->chordIndex[(size_t) members[k]] = (int32_t) k;
            }
        }
        return t;
    }
};

} // namespace dcsynth
