// dcvoice_plan.h — chart notes -> VoiceTimeline events. CONTROL THREAD ONLY
// (it allocates); run it once when the arrangement is uploaded.
//
// The chart says WHAT is played; a player also decides HOW, and three of
// those decisions are not in the data. Each one is a documented heuristic:
//
// 1. WHEN A NOTE STOPS (let ring vs damped):
//    - a note with a real sustain (> 50 ms) is held to its sustain end;
//    - a short note rings until the next note or chord on ANY string (plus
//      30 ms of overlap) — a player mutes the strings they are not playing —
//      capped at 2.5 s, and never shorter than 90 ms;
//    - letRing (an arpeggio handshape / let-ring chord) rings until the same
//      string is played again, capped at 8 s;
//    - a dead note stops after 60 ms; LinkNext holds to the next note on the
//      string, which then has no attack of its own.
//    A new note on a string always replaces what that string was doing.
//
// 2. STRUM DIRECTION (chords): down on the beat grid, up between. The grid
//    is the 16th-note grid when the neighbouring chord is at most ~0.3 beat
//    away, else the 8th-note grid; triplet positions alternate the same way.
//    Palm-muted and accented chords are always down-strokes. A down-stroke
//    reaches the strings low to high, an up-stroke high to low, 8 ms (down) /
//    6 ms (up) apart, 3 ms for palm mutes; up-strokes are a little softer.
//
// 3. TREMOLO picking: 32nd notes at the local tempo (8 per beat), clamped to
//    8..18 picks per second, alternating down/up. Without a beat grid: 14/s.
#pragma once

#include "dcvoice_instrument.h"
#include "dcvoice_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dcvoice {

/// A note as the chart has it (DotCharter.Core IGuitarSound + its time).
struct ChartNote
{
    double time = 0.0;       // song s
    double sustain = 0.0;    // song s
    int string = 0;          // 0 = lowest
    int fret = 0;
    int slideTo = -1;
    bool unpitchedSlide = false;
    uint8_t mute = 0;        // 0 none, 1 full (dead), 2 palm
    uint8_t hopo = 0;        // 0 none, 1 hammer-on, 2 pull-off, 3 tap
    uint8_t harmonic = 0;    // 0 none, 1 natural, 2 pinch
    uint8_t bassPicking = 0; // 0 none, 1 pop, 2 slap
    bool vibrato = false;
    int vibratoStrength = 0; // the chart's 40 / 80 / 120; 0 with vibrato = 80
    bool tremolo = false;
    bool accent = false;
    bool linkNext = false;
    bool letRing = false;
    bool picked = false;     // bass played with a pick (DCA_CHART_NOTE_PICKED)
    int chordId = -1;        // notes of one chord share an id >= 0
    int bendFirst = 0;       // into the ChartBend array
    int bendCount = 0;
    int slideInFrets = 0;    // signed; 0 = none (Guitar Pro slide-in)
};

/// The chart's BendValue: absolute song time, Step (1.0 = a half step).
struct ChartBend
{
    double time = 0.0;
    float step = 0.0f;
};

struct PlanOptions
{
    bool bass = false;
    double strumDownMs = 8.0;
    double strumUpMs = 6.0;
    double strumPalmMs = 3.0;
    double maxRingSeconds = 2.5;
    double minRingSeconds = 0.09;
    double letRingSeconds = 8.0;
    double overlapSeconds = 0.03;
    double defaultTremoloHz = 14.0;
    float velocity = 0.72f;
};

struct PlannedTimeline
{
    std::vector<VoiceNote> notes;
    std::vector<BendPoint> bends;

    VoiceTimeline view(uint32_t generation) const
    {
        VoiceTimeline t;
        t.notes = notes.data();
        t.count = (int) notes.size();
        t.bends = bends.data();
        t.bendCount = (int) bends.size();
        t.generation = generation;
        return t;
    }
};

namespace detail {

/// True = up-stroke. See heuristic 2 above.
inline bool strumIsUp(double t, double prevGap, double nextGap, const double* beats, int beatCount)
{
    if (beats == nullptr || beatCount < 2)
        return false;
    int lo = 0, hi = beatCount - 1;
    if (t < beats[0] || t >= beats[beatCount - 1])
        return false;
    while (hi - lo > 1)
    {
        const int mid = (lo + hi) / 2;
        if (beats[mid] <= t)
            lo = mid;
        else
            hi = mid;
    }
    const double len = beats[lo + 1] - beats[lo];
    if (len <= 1.0e-6)
        return false;
    const double phase = (t - beats[lo]) / len;
    const double neighbour = std::min(prevGap, nextGap) / len;
    const bool sixteenths = neighbour <= 0.30;
    const double tol = std::min(0.025 / len, 0.06);
    const int grids[3] = { sixteenths ? 4 : 2, 3, 6 };
    for (int g : grids)
    {
        const double x = phase * g;
        const double r = std::round(x);
        if (std::fabs(x - r) <= tol * g)
            return (((int) r) % g) % 2 == 1;
    }
    // Off every grid: alternate by which half of the nearest 8th it is in.
    return std::fmod(phase * 2.0, 1.0) >= 0.5;
}

inline double localBeatLength(double t, const double* beats, int beatCount)
{
    if (beats == nullptr || beatCount < 2)
        return 0.0;
    int i = (int) (std::upper_bound(beats, beats + beatCount, t) - beats) - 1;
    i = std::clamp(i, 0, beatCount - 2);
    return beats[i + 1] - beats[i];
}

} // namespace detail

/// Plans the whole arrangement. `beats` (song s, ascending) may be null.
inline void planChart(const ChartNote* in, int count, const ChartBend* bends, int bendCount,
                      const double* beats, int beatCount, const PlanOptions& opt, PlannedTimeline& out)
{
    out.notes.clear();
    out.bends.clear();
    if (in == nullptr || count <= 0)
        return;

    // Non-finite times/sustains never reach the audio thread.
    std::vector<int> order;
    order.reserve((size_t) count);
    for (int i = 0; i < count; ++i)
        if (std::isfinite(in[i].time) && std::isfinite(in[i].sustain))
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (in[a].time != in[b].time)
            return in[a].time < in[b].time;
        return in[a].string < in[b].string;
    });

    // Groups: a chord id, or notes on different strings within 1 ms.
    std::vector<int> group((size_t) count, -1);
    std::vector<double> groupTime;
    for (size_t k = 0; k < order.size(); ++k)
    {
        const ChartNote& n = in[order[k]];
        int g = -1;
        if (k > 0)
        {
            const ChartNote& p = in[order[k - 1]];
            const bool sameChord = n.chordId >= 0 && n.chordId == p.chordId;
            const bool together = std::fabs(n.time - p.time) < 0.001 && n.string != p.string;
            if (sameChord || together)
                g = group[(size_t) order[k - 1]];
        }
        if (g < 0)
        {
            g = (int) groupTime.size();
            groupTime.push_back(n.time);
        }
        group[(size_t) order[k]] = g;
    }
    const int groups = (int) groupTime.size();
    std::vector<int> groupSize((size_t) groups, 0);
    for (const int i : order)
        ++groupSize[(size_t) group[(size_t) i]];

    // Next note on the same string (by sorted order).
    std::vector<int> nextSame((size_t) count, -1);
    {
        int last[16];
        std::fill(last, last + 16, -1);
        for (size_t k = 0; k < order.size(); ++k)
        {
            const int i = order[k];
            const int s = std::clamp(in[i].string, 0, 15);
            if (last[s] >= 0)
                nextSame[(size_t) last[s]] = i;
            last[s] = i;
        }
    }
    std::vector<char> linkedFromPrev((size_t) count, 0);
    for (const int i : order)
    {
        const int nx = nextSame[(size_t) i];
        if (in[i].linkNext && nx >= 0 && in[nx].time <= in[i].time + std::max(in[i].sustain, 0.0) + 0.06)
            linkedFromPrev[(size_t) nx] = 1;
    }

    // Per-group strum direction.
    std::vector<char> groupUp((size_t) groups, 0);
    for (int g = 0; g < groups; ++g)
    {
        if (groupSize[(size_t) g] < 2)
            continue;
        const double prevGap = g > 0 ? groupTime[(size_t) g] - groupTime[(size_t) g - 1] : 1.0e9;
        const double nextGap = g + 1 < groups ? groupTime[(size_t) g + 1] - groupTime[(size_t) g] : 1.0e9;
        groupUp[(size_t) g] = detail::strumIsUp(groupTime[(size_t) g], prevGap, nextGap, beats, beatCount) ? 1 : 0;
    }
    // Palm-muted / accented chords are down-strokes.
    for (const int i : order)
        if (in[i].mute == 2 || in[i].accent)
            groupUp[(size_t) group[(size_t) i]] = 0;

    auto addNote = [&](const ChartNote& n, int idx, double start, double release, uint32_t extraFlags,
                       float velocity, int bendFirst, int bendCountOut) {
        VoiceNote v {};
        v.startTime = start;
        v.anchorTime = n.time;
        v.releaseTime = std::max(release, start + 0.01);
        v.sustainEnd = n.time + std::max(n.sustain, 0.0);
        v.string = (int16_t) n.string;
        v.fret = (int16_t) std::max(n.fret, 0);
        v.slideToFret = (int16_t) n.slideTo;
        v.slideInFrets = (int16_t) n.slideInFrets;
        uint32_t f = extraFlags;
        if (n.mute == 1) f |= kFlagFullMute;
        if (n.mute == 2) f |= kFlagPalmMute;
        if (n.hopo == 1) f |= kFlagHammerOn;
        if (n.hopo == 2) f |= kFlagPullOff;
        if (n.hopo == 3) f |= kFlagTap;
        if (n.harmonic == 1) f |= kFlagHarmonic;
        if (n.harmonic == 2) f |= kFlagPinchHarmonic;
        if (n.bassPicking == 1) f |= kFlagPop;
        if (n.bassPicking == 2) f |= kFlagSlap;
        if (n.accent) f |= kFlagAccent;
        if (n.unpitchedSlide) f |= kFlagUnpitchedSlide;
        if (n.letRing) f |= kFlagLetRing;
        if (n.picked) f |= kFlagPicked;
        if (linkedFromPrev[(size_t) idx]) f |= kFlagLinked;
        v.flags = f;
        v.velocity = velocity;
        if (n.vibrato)
        {
            const double strength = n.vibratoStrength > 0 ? n.vibratoStrength : 80.0;
            v.vibratoSemis = (float) std::min((opt.bass ? 0.20 : 0.25) * strength / 80.0, 0.9);
        }
        v.bendFirst = bendFirst;
        v.bendCount = bendCountOut;
        out.notes.push_back(v);
    };

    for (size_t k = 0; k < order.size(); ++k)
    {
        const int i = order[k];
        const ChartNote& n = in[i];
        const int g = group[(size_t) i];

        // Velocity: accents up, ghosts and up-strokes down, a little humanising.
        double vel = opt.velocity;
        if (n.accent) vel = 0.95;
        if (n.mute == 2) vel = 0.8;
        if (n.mute == 1) vel = opt.bass ? 0.45 : 0.6;
        const bool up = groupUp[(size_t) g] != 0;
        if (up) vel *= 0.85;
        vel *= 1.0 + 0.04 * hashUnit((uint32_t) i, 77u);

        // Strum offset.
        double start = n.time;
        if (groupSize[(size_t) g] > 1)
        {
            // A group is contiguous in `order`, so only its neighbours are scanned.
            int rank = 0;
            size_t j0 = k;
            while (j0 > 0 && group[(size_t) order[j0 - 1]] == g)
                --j0;
            for (size_t j = j0; j < order.size() && group[(size_t) order[j]] == g; ++j)
            {
                const int o = order[j];
                if (o != i && (up ? in[o].string > n.string : in[o].string < n.string))
                    ++rank;
            }
            const double per = (n.mute == 2 ? opt.strumPalmMs : (up ? opt.strumUpMs : opt.strumDownMs)) * 1.0e-3;
            start += rank * per * (1.0 + 0.15 * hashUnit((uint32_t) g, (uint32_t) n.string));
        }

        // Release.
        const double sus = std::max(n.sustain, 0.0);
        double release;
        const int nx = nextSame[(size_t) i];
        if (n.mute == 1)
            release = n.time + 0.06;
        else if (n.letRing)
            release = nx >= 0 ? std::min(in[nx].time, n.time + opt.letRingSeconds) : n.time + opt.letRingSeconds;
        else if (n.linkNext && nx >= 0)
            release = in[nx].time + 0.01;
        else if (sus > 0.05)
            release = n.time + sus + 0.02;
        else
        {
            double nextAny = n.time + opt.maxRingSeconds;
            if (g + 1 < groups)
                nextAny = std::min(nextAny, groupTime[(size_t) g + 1] + opt.overlapSeconds);
            release = std::clamp(nextAny, n.time + opt.minRingSeconds, n.time + opt.maxRingSeconds);
        }

        // Bends: copy, step -> semitones.
        const int bf = (int) out.bends.size();
        int bc = 0;
        if (bends != nullptr && n.bendCount > 0)
        {
            for (int b = 0; b < n.bendCount; ++b)
            {
                const int bi = n.bendFirst + b;
                if (bi < 0 || bi >= bendCount)
                    continue;
                out.bends.push_back(BendPoint { bends[bi].time, bends[bi].step });
                ++bc;
            }
            std::sort(out.bends.begin() + bf, out.bends.end(),
                      [](const BendPoint& a, const BendPoint& b) { return a.time < b.time; });
        }

        const uint32_t dirFlag = up ? kFlagUpStroke : 0u;
        if (n.tremolo && sus > 0.05)
        {
            const double beat = detail::localBeatLength(n.time, beats, beatCount);
            const double rate = beat > 0.0 ? std::clamp(8.0 / beat, 8.0, 18.0) : opt.defaultTremoloHz;
            const double step = 1.0 / rate;
            const double end = n.time + sus;
            int r = 0;
            for (double t = start; t < end - 0.5 * step; t += step, ++r)
            {
                const double nextT = t + step;
                const bool last = nextT >= end - 0.5 * step;
                const uint32_t fl = (r > 0 ? kFlagTremoloRepeat : 0u) | ((r % 2) ? kFlagUpStroke : 0u);
                const float v = (float) std::clamp(vel * (r % 2 ? 0.88 : 1.0) * (1.0 + 0.05 * hashUnit((uint32_t) i, (uint32_t) r)), 0.05, 1.0);
                ChartNote copy = n;
                if (r > 0)
                {
                    copy.hopo = 0;
                    copy.linkNext = false;
                }
                addNote(copy, i, t, last ? release : nextT + 0.004, fl, v, bf, bc);
                if (r > 0)
                    out.notes.back().flags &= ~(uint32_t) kFlagLinked;
            }
        }
        else
        {
            addNote(n, i, start, release, dirFlag, (float) std::clamp(vel, 0.05, 1.0), bf, bc);
        }
    }

    std::stable_sort(out.notes.begin(), out.notes.end(),
                     [](const VoiceNote& a, const VoiceNote& b) { return a.startTime < b.startTime; });
}

} // namespace dcvoice
