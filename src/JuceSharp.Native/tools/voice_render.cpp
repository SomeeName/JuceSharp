#define _CRT_SECURE_NO_WARNINGS
// voice_render — offline renderer for the chart voice (src/dsp/voice/).
//
// Header-only, no JUCE, no device, no DLL: it plans and renders exactly what
// the engine will, block by block at 128 frames, and writes 16-bit mono WAVs.
//
//   juce_sharp_voice_render --listening-set <dir> [--rate 48000]
//       renders the owner's listening set (guitar g01..g11, bass b01..b08)
//   add --pack-dir <dir> to play the real recordings (guitar.dcvp, bass.dcvp;
//   tools/voice/build_voice_pack.py) instead of the waveguide, and
//   --suffix <s> to append <s> to every listening-set file name
//   juce_sharp_voice_render --models <dir> --pack-dir <dir>
//       the INSTRUMENT MODELS (#50): one riff (guitar) and one line (bass)
//       through every model x pickup position (x coil split where it exists),
//       one WAV each, plus spectra.csv / spectra.md — the long-term spectrum's
//       centroid and band levels, to show each choice moves the tone the way it
//       should (neck darker than bridge, a humbucker darker than its split).
//   juce_sharp_voice_render --bench
//       real-time factor: six strings ringing (re-struck chords every second,
//       half of them with vibrato) for 60 s at 48 kHz in 128-frame blocks,
//       plus the worst single block against its 2.67 ms budget
//   juce_sharp_voice_render --notes <file> --out <wav> [--bass|--bass5] [--pick] [--rate R] [--bpm B] [--pack-dir D] [--suffix S]
//       renders a note list: one note per line,
//         <time s> <sustain s> <string> <fret> [token ...]
//       tokens: slide=<fret> unpitched slidein=<±frets> palm dead ho po tap pick
//               harm pinch slap pop vib[=40|80|120] trem accent link ring
//               chord=<id> bend=<t>:<step>[,<t>:<step>...]   (t relative to the note)
//       '#' starts a comment. --bpm gives the strum/tremolo heuristics a grid.
//
// The output is a DRY DI signal: no amp, no cab. It is meant to go through
// the song's rig; on its own it sounds thin, exactly as a DI guitar does.
#include "dsp/voice/dcvoice_excitation_data.h"
#include "dsp/voice/dcvoice_pack.h"
#include "dsp/voice/dcvoice_plan.h"
#include "dsp/voice/dcvoice_timeline.h"
#include "synth/GuitarChartVoice.h"
#include "dsp/cores/dcdsp_amp.h"
#include "dsp/cores/dcdsp_cab.h"

#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dcvoice;

namespace {

// --pack-dir: real recordings (the sample packs); without it, the waveguide.
SamplePack gGuitarPack, gBassPack;
bool gHavePacks = false;
std::string gSuffix;

ChartTimelineVoice makeVoice(bool bass, int bassStrings = 4)
{
    const InstrumentSpec spec = bass ? InstrumentSpec::bassGuitar(bassStrings) : InstrumentSpec::guitar();
    if (gHavePacks)
        return ChartTimelineVoice(spec, bass ? &gBassPack : &gGuitarPack);
    return ChartTimelineVoice(spec, bass ? bassExcitations() : guitarExcitations());
}

struct Song
{
    std::vector<ChartNote> notes;
    std::vector<ChartBend> bends;
    std::vector<double> beats;
    double length = 0.0;

    ChartNote& add(double t, double sus, int string, int fret)
    {
        ChartNote n;
        n.time = t;
        n.sustain = sus;
        n.string = string;
        n.fret = fret;
        notes.push_back(n);
        length = std::max(length, t + sus + 1.5);
        return notes.back();
    }

    void bend(ChartNote& n, std::initializer_list<std::pair<double, double>> pts)
    {
        n.bendFirst = (int) bends.size();
        for (const auto& p : pts)
            bends.push_back(ChartBend { n.time + p.first, (float) p.second });
        n.bendCount = (int) pts.size();
    }

    int chordId = 0;
    void chord(double t, double sus, std::initializer_list<std::pair<int, int>> sf, uint8_t mute = 0, bool accent = false)
    {
        const int id = chordId++;
        for (const auto& p : sf)
        {
            ChartNote& n = add(t, sus, p.first, p.second);
            n.chordId = id;
            n.mute = mute;
            n.accent = accent;
        }
    }

    void grid(double bpm, double seconds)
    {
        const double b = 60.0 / bpm;
        for (double t = 0.0; t < seconds + 2.0; t += b)
            beats.push_back(t);
    }
};

bool writeWav(const std::string& path, const std::vector<float>& x, int rate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (! f)
        return false;
    const uint32_t dataBytes = (uint32_t) (x.size() * 2);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1); u32((uint32_t) rate); u32((uint32_t) rate * 2); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    for (float v : x)
    {
        const float c = std::max(-1.0f, std::min(1.0f, v));
        const int16_t s = (int16_t) std::lround(c * 32767.0f);
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

std::vector<float> render(const Song& song, ChartTimelineVoice& voice, double rate)
{
    PlanOptions opt;
    opt.bass = voice.spec().bass;
    PlannedTimeline plan;
    planChart(song.notes.data(), (int) song.notes.size(), song.bends.data(), (int) song.bends.size(),
              song.beats.empty() ? nullptr : song.beats.data(), (int) song.beats.size(), opt, plan);
    const VoiceTimeline tl = plan.view(1);
    voice.prepare(rate, 128);
    std::vector<float> out((size_t) (song.length * rate));
    VoiceClock clock;
    clock.rolling = true;
    clock.songSecondsPerSample = 1.0 / rate;
    for (size_t i = 0; i < out.size(); i += 128)
    {
        clock.songTime = (double) i / rate;
        voice.render(out.data() + i, (int) std::min<size_t>(128, out.size() - i), clock, tl);
    }
    return out;
}

bool emit(const std::string& dir, const char* name, const Song& song, ChartTimelineVoice& voice, double rate)
{
    const std::vector<float> x = render(song, voice, rate);
    float peak = 0.0f;
    double sum = 0.0;
    for (float v : x)
    {
        peak = std::max(peak, std::fabs(v));
        sum += (double) v * v;
    }
    const std::string path = dir + "/" + name + gSuffix + ".wav";
    const bool ok = writeWav(path, x, (int) rate);
    std::printf("%-34s %6.2f s  peak %6.1f dBFS  rms %6.1f dBFS%s\n", name, x.size() / rate,
                20.0 * std::log10(peak + 1e-12), 10.0 * std::log10(sum / std::max<size_t>(x.size(), 1) + 1e-20),
                ok ? "" : "  (WRITE FAILED)");
    return ok && peak < 1.0f;
}

// ---- the listening set -------------------------------------------------------

bool guitarSet(const std::string& dir, double rate)
{
    ChartTimelineVoice g = makeVoice(false);
    bool ok = true;
    {
        Song s;   // every string: open, 5th, 12th fret
        double t = 0.2;
        for (int str = 0; str < 6; ++str)
            for (int fret : { 0, 5, 12 })
            {
                s.add(t, 1.0, str, fret);
                t += 1.25;
            }
        ok &= emit(dir, "g01_single_notes_per_string", s, g, rate);
    }
    {
        Song s;   // strummed chords, D D U U D U at 100 bpm, last chord let ring
        const double b = 0.6;
        s.grid(100.0, 20.0);
        const std::initializer_list<std::pair<int, int>> chords[4] = {
            { { 0, 0 }, { 1, 2 }, { 2, 2 }, { 3, 1 }, { 4, 0 }, { 5, 0 } },     // E
            { { 0, 3 }, { 1, 2 }, { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 3 } },     // G
            { { 1, 0 }, { 2, 2 }, { 3, 2 }, { 4, 1 }, { 5, 0 } },               // Am
            { { 1, 3 }, { 2, 2 }, { 3, 0 }, { 4, 1 }, { 5, 0 } },               // C
        };
        double bar = 0.2;
        for (int c = 0; c < 4; ++c, bar += 4 * b)
            for (double pos : { 0.0, 1.0, 1.5, 2.5, 3.0, 3.5 })
                s.chord(bar + pos * b, 0.0, chords[c]);
        s.chord(bar, 2.5, chords[0]);
        for (auto& n : s.notes)
            if (n.time >= bar)
                n.letRing = true;
        ok &= emit(dir, "g02_chord_strums", s, g, rate);
    }
    {
        Song s;   // bends: full bend + release, prebend + release, half bend with vibrato
        ChartNote& a = s.add(0.2, 1.8, 3, 7);
        s.bend(a, { { 0.25, 2.0 }, { 0.9, 2.0 }, { 1.4, 0.0 } });
        ChartNote& b = s.add(2.4, 1.6, 4, 8);
        s.bend(b, { { 0.0, 2.0 }, { 0.6, 2.0 }, { 1.1, 0.0 } });
        ChartNote& c = s.add(4.4, 1.8, 5, 12);
        s.bend(c, { { 0.15, 1.0 } });
        c.vibrato = true;
        ChartNote& d = s.add(6.6, 1.2, 4, 15);
        s.bend(d, { { 0.12, 3.0 } });
        ok &= emit(dir, "g03_bends_and_releases", s, g, rate);
    }
    {
        Song s;   // pitched slides up/down, unpitched slide out, slide in
        ChartNote& a = s.add(0.2, 0.6, 1, 5); a.slideTo = 7;
        s.add(0.85, 0.5, 1, 7);
        ChartNote& b = s.add(1.8, 0.9, 2, 7); b.slideTo = 12;
        ChartNote& c = s.add(3.2, 0.9, 2, 12); c.slideTo = 5;
        ChartNote& d = s.add(4.6, 0.9, 0, 12); d.unpitchedSlide = true; d.slideTo = 3;
        ChartNote& e = s.add(6.0, 1.0, 3, 9); e.slideInFrets = -3;
        ok &= emit(dir, "g04_slides", s, g, rate);
    }
    {
        Song s;   // vibrato 40 / 80 / 120
        double t = 0.2;
        for (int v : { 40, 80, 120 })
        {
            ChartNote& n = s.add(t, 1.8, 4, 10);
            n.vibrato = true;
            n.vibratoStrength = v;
            t += 2.3;
        }
        ok &= emit(dir, "g05_vibrato_40_80_120", s, g, rate);
    }
    {
        Song s;   // hammer-on / pull-off run, then a tapping lick
        const double st = 60.0 / 110.0 / 4.0;
        double t = 0.2;
        const int run[][3] = { { 3, 5, 0 }, { 3, 7, 1 }, { 3, 9, 1 }, { 3, 7, 2 }, { 3, 5, 2 }, { 2, 7, 0 }, { 2, 5, 2 },
                               { 2, 7, 1 }, { 2, 9, 1 }, { 2, 7, 2 }, { 2, 5, 2 }, { 1, 7, 0 }, { 1, 5, 2 }, { 1, 7, 1 } };
        for (int rep = 0; rep < 2; ++rep)
            for (const auto& r : run)
            {
                ChartNote& n = s.add(t, 0.0, r[0], r[1]);
                n.hopo = (uint8_t) r[2];
                t += st;
            }
        t += 0.5;
        for (int rep = 0; rep < 4; ++rep)
        {
            ChartNote& a = s.add(t, 0.0, 4, 12); a.hopo = 3; t += st;
            ChartNote& b = s.add(t, 0.0, 4, 5);  b.hopo = 2; t += st;
            ChartNote& c = s.add(t, 0.0, 4, 8);  c.hopo = 1; t += st;
        }
        ok &= emit(dir, "g06_hammer_pull_tap", s, g, rate);
    }
    {
        Song s;   // palm-muted 16th chugs with open power-chord accents
        s.grid(120.0, 10.0);
        const double st = 0.125;
        double t = 0.2;
        for (int bar = 0; bar < 2; ++bar)
        {
            for (int k = 0; k < 16; ++k, t += st)
            {
                if (k == 6 || k == 14)
                {
                    s.chord(t, 0.2, { { 0, 3 }, { 1, 5 }, { 2, 5 } }, 0, true);
                    continue;
                }
                ChartNote& n = s.add(t, 0.0, 0, 0);
                n.mute = 2;
            }
        }
        s.chord(t, 1.5, { { 0, 0 }, { 1, 2 }, { 2, 2 } }, 0, true);
        ok &= emit(dir, "g07_palm_mute_chugs", s, g, rate);
    }
    {
        Song s;   // natural harmonics at 12 / 7 / 5, then pinch harmonics
        double t = 0.2;
        for (int fret : { 12, 7, 5 })
            for (int str : { 0, 2, 4 })
            {
                ChartNote& n = s.add(t, 1.0, str, fret);
                n.harmonic = 1;
                t += 1.1;
            }
        for (int fret : { 3, 5 })
        {
            ChartNote& n = s.add(t, 1.6, 1, fret);
            n.harmonic = 2;
            n.vibrato = true;
            t += 2.0;
        }
        ok &= emit(dir, "g08_natural_and_pinch_harmonics", s, g, rate);
    }
    {
        Song s;   // funk 16ths: chords and dead strums
        s.grid(100.0, 10.0);
        const double st = 0.15;
        double t = 0.2;
        const std::initializer_list<std::pair<int, int>> e9 = { { 1, 7 }, { 2, 6 }, { 3, 7 }, { 4, 7 } };
        for (int k = 0; k < 32; ++k, t += st)
        {
            const bool hit = (k % 8 == 0) || (k % 8 == 3) || (k % 8 == 6);
            s.chord(t, 0.0, e9, hit ? 0 : 1, hit);
        }
        ok &= emit(dir, "g09_dead_notes_funk", s, g, rate);
    }
    {
        Song s;   // tremolo picking
        s.grid(140.0, 8.0);
        ChartNote& a = s.add(0.2, 2.0, 5, 12);
        a.tremolo = true;
        ChartNote& b = s.add(2.4, 1.5, 4, 13);
        b.tremolo = true;
        b.slideTo = 8;
        ok &= emit(dir, "g10_tremolo_picking", s, g, rate);
    }
    {
        Song s;   // a short rock riff mixing it all
        s.grid(120.0, 12.0);
        const double e = 0.25;
        double t = 0.2;
        for (int rep = 0; rep < 2; ++rep)
        {
            for (int k = 0; k < 3; ++k) { ChartNote& n = s.add(t, 0.0, 0, 0); n.mute = 2; t += e / 2; }
            s.chord(t, 0.3, { { 0, 3 }, { 1, 5 }, { 2, 5 } }); t += e;
            for (int k = 0; k < 3; ++k) { ChartNote& n = s.add(t, 0.0, 0, 0); n.mute = 2; t += e / 2; }
            s.chord(t, 0.25, { { 0, 5 }, { 1, 7 }, { 2, 7 } });
            for (auto& n : s.notes) if (n.time == t) n.slideTo = n.fret + 2;
            t += 2 * e;
        }
        ChartNote& bnd = s.add(t, 1.2, 3, 9);
        s.bend(bnd, { { 0.1, 2.0 }, { 0.8, 2.0 } });
        bnd.vibrato = true;
        t += 1.4;
        s.add(t, 0.0, 4, 10); t += e / 2;
        ChartNote& p2 = s.add(t, 0.0, 4, 8); p2.hopo = 2; t += e / 2;
        s.add(t, 0.0, 3, 9); t += e / 2;
        ChartNote& p4 = s.add(t, 0.0, 3, 7); p4.hopo = 2; t += e / 2;
        s.chord(t, 2.0, { { 0, 0 }, { 1, 2 }, { 2, 2 }, { 3, 1 }, { 4, 0 }, { 5, 0 } }, 0, true);
        ok &= emit(dir, "g11_riff", s, g, rate);
    }
    {
        // A sweep-picked arpeggio, single notes across the strings: OUTSIDE a
        // hand shape each note is damped when the next one starts...
        Song s;
        const int frets[6] = { 5, 7, 7, 6, 5, 5 };   // A minor shape at the 5th fret
        double t = 0.2;
        for (int rep = 0; rep < 3; ++rep)
        {
            for (int str = 0; str < 6; ++str, t += 0.11)
                s.add(t, 0.0, str, frets[str]);
            for (int str = 4; str >= 1; --str, t += 0.11)
                s.add(t, 0.0, str, frets[str]);
        }
        s.add(t, 1.5, 0, 5);
        ok &= emit(dir, "g13_sweep_no_hand_shape", s, g, rate);
        // ...and INSIDE one (an arpeggio hand shape: let ring) they ring together.
        for (auto& n : s.notes)
            n.letRing = true;
        ok &= emit(dir, "g14_sweep_inside_hand_shape", s, g, rate);
    }
    return ok;
}

bool bassSet(const std::string& dir, double rate)
{
    ChartTimelineVoice finger = makeVoice(true);
    // The pick clips mark their notes picked (the sampler's pick recordings);
    // the waveguide fallback switches its excitation kind instead.
    InstrumentSpec pickSpec = InstrumentSpec::bassGuitar(4);
    pickSpec.defaultKind = ExcKind::Pick;
    ChartTimelineVoice pick = gHavePacks ? ChartTimelineVoice(pickSpec, &gBassPack) : ChartTimelineVoice(pickSpec, bassExcitations());
    bool ok = true;
    {
        Song s;
        double t = 0.2;
        for (int str = 0; str < 4; ++str)
            for (int fret : { 0, 5, 12 })
            {
                s.add(t, 1.2, str, fret);
                t += 1.45;
            }
        ok &= emit(dir, "b01_single_notes_finger", s, finger, rate);
        for (auto& n : s.notes)
            n.picked = true;
        ok &= emit(dir, "b02_single_notes_pick", s, pick, rate);
    }
    {
        Song s;   // a walking line, fingerstyle, quarter notes at 120 bpm
        const int line[][2] = { { 1, 0 }, { 1, 4 }, { 2, 2 }, { 2, 4 }, { 3, 2 }, { 3, 4 }, { 2, 4 }, { 2, 2 },
                                { 0, 5 }, { 0, 9 }, { 1, 7 }, { 1, 9 }, { 2, 7 }, { 2, 9 }, { 1, 9 }, { 1, 7 } };
        double t = 0.2;
        for (const auto& n : line)
        {
            s.add(t, 0.45, n[0], n[1]);
            t += 0.5;
        }
        s.add(t, 1.5, 1, 0);
        ok &= emit(dir, "b03_walking_line_finger", s, finger, rate);
    }
    {
        Song s;   // ghost notes in a 16th groove
        const double st = 0.14;
        double t = 0.2;
        const int pat[16] = { 1, 0, 2, 0, 1, 2, 0, 1, 1, 0, 2, 0, 1, 2, 1, 0 };   // 1 note, 2 ghost
        for (int bar = 0; bar < 2; ++bar)
            for (int k = 0; k < 16; ++k, t += st)
            {
                if (pat[k] == 0)
                    continue;
                ChartNote& n = s.add(t, 0.0, k % 4 == 3 ? 2 : 0, k % 4 == 3 ? 2 : 7);
                if (pat[k] == 2)
                    n.mute = 1;
            }
        ok &= emit(dir, "b04_ghost_notes", s, finger, rate);
    }
    {
        Song s;   // slides
        ChartNote& a = s.add(0.2, 0.7, 1, 5); a.slideTo = 7;
        s.add(0.95, 0.6, 1, 7);
        ChartNote& b = s.add(2.0, 1.0, 2, 5); b.slideTo = 12;
        ChartNote& c = s.add(3.6, 1.0, 0, 12); c.unpitchedSlide = true; c.slideTo = 2;
        ChartNote& d = s.add(5.2, 1.0, 0, 7); d.slideInFrets = -4;
        ok &= emit(dir, "b05_slides", s, finger, rate);
    }
    {
        Song s;   // slap / pop octaves with a hammer-on, 16ths at 100 bpm
        const double st = 0.15;
        double t = 0.2;
        for (int bar = 0; bar < 2; ++bar)
        {
            const int root = bar == 0 ? 0 : 3;
            const int seq[8][3] = { { 0, root, 2 }, { 0, root, 0 }, { 2, root + 2, 1 }, { 0, root, 2 },
                                    { 1, root + 2, 2 }, { 1, root + 4, 3 }, { 2, root + 2, 1 }, { 0, root, 2 } };
            for (int rep = 0; rep < 2; ++rep)
                for (const auto& q : seq)
                {
                    ChartNote& n = s.add(t, 0.0, q[0], q[1]);
                    if (q[2] == 2) n.bassPicking = 2;            // slap
                    else if (q[2] == 1) n.bassPicking = 1;       // pop
                    else if (q[2] == 3) n.hopo = 1;              // hammer-on
                    else n.mute = 1;                              // thumb ghost
                    t += st;
                }
        }
        ChartNote& last = s.add(t, 1.2, 0, 0);
        last.bassPicking = 2;
        ok &= emit(dir, "b06_slap_pop", s, finger, rate);
    }
    {
        Song s;   // vibrato and a bend
        ChartNote& a = s.add(0.2, 2.0, 2, 9); a.vibrato = true;
        ChartNote& b = s.add(2.6, 1.6, 3, 7);
        s.bend(b, { { 0.3, 2.0 }, { 1.0, 2.0 }, { 1.4, 0.0 } });
        ok &= emit(dir, "b07_vibrato_and_bend", s, finger, rate);
    }
    {
        Song s;   // picked eighths, palm muted, then open
        double t = 0.2;
        for (int k = 0; k < 16; ++k, t += 0.2)
        {
            ChartNote& n = s.add(t, 0.0, 0, k < 8 ? 0 : 3);
            n.mute = k < 8 ? 2 : 0;
            n.picked = true;
        }
        ok &= emit(dir, "b08_pick_palm_mute_then_open", s, pick, rate);
    }
    return ok;
}

// ---- bench ---------------------------------------------------------------------

int bench()
{
    Song s;
    for (int rep = 0; rep < 60; ++rep)
    {
        s.chordId = rep;
        for (int str = 0; str < 6; ++str)
        {
            ChartNote& n = s.add(rep * 1.0, 0.98, str, (rep * 3 + str) % 12);
            n.chordId = rep;
            n.vibrato = rep % 2 == 1;
        }
    }
    const double rate = 48000.0;
    const int block = 128;
    ChartTimelineVoice v = makeVoice(false);
    PlanOptions opt;
    PlannedTimeline plan;
    planChart(s.notes.data(), (int) s.notes.size(), nullptr, 0, nullptr, 0, opt, plan);
    const VoiceTimeline tl = plan.view(1);
    v.prepare(rate, block);
    std::vector<float> buf(block);
    VoiceClock c;
    c.rolling = true;
    c.songSecondsPerSample = 1.0 / rate;
    const long blocks = (long) (60.0 * rate / block);
    double worst = 0.0, total = 0.0;
    int ringing = 0;
    for (long b = 0; b < blocks; ++b)
    {
        c.songTime = (double) b * block / rate;
        const auto t0 = std::chrono::steady_clock::now();
        v.render(buf.data(), block, c, tl);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        total += dt;
        if (b > 100)
            worst = std::max(worst, dt);
        ringing = std::max(ringing, v.activeStrings());
    }
    const double budget = block / rate;
    std::printf("bench: %d strings ringing, 60 s at 48 kHz / %d frames: %.3f s CPU, real-time factor %.5f "
                "(%.2f %% of one core); worst block %.1f us of a %.0f us budget\n",
                ringing, block, total, total / 60.0, 100.0 * total / 60.0, worst * 1e6, budget * 1e6);
    return 0;
}

// ---- note-list mode ----------------------------------------------------------

bool parseNotes(const char* path, Song& s)
{
    FILE* f = std::fopen(path, "rb");
    if (! f)
        return false;
    char line[1024];
    while (std::fgets(line, sizeof line, f))
    {
        if (char* hash = std::strchr(line, '#'))
            *hash = 0;
        double t = 0.0, sus = 0.0;
        int str = 0, fret = 0, used = 0;
        if (std::sscanf(line, "%lf %lf %d %d%n", &t, &sus, &str, &fret, &used) < 4)
            continue;
        ChartNote& n = s.add(t, sus, str, fret);
        char* p = line + used;
        char tok[256];
        int adv = 0;
        while (std::sscanf(p, "%255s%n", tok, &adv) == 1)
        {
            p += adv;
            std::string k = tok, v;
            if (const size_t eq = k.find('='); eq != std::string::npos)
            {
                v = k.substr(eq + 1);
                k = k.substr(0, eq);
            }
            if (k == "slide") n.slideTo = std::atoi(v.c_str());
            else if (k == "unpitched") n.unpitchedSlide = true;
            else if (k == "slidein") n.slideInFrets = std::atoi(v.c_str());
            else if (k == "palm") n.mute = 2;
            else if (k == "dead") n.mute = 1;
            else if (k == "ho") n.hopo = 1;
            else if (k == "po") n.hopo = 2;
            else if (k == "tap") n.hopo = 3;
            else if (k == "harm") n.harmonic = 1;
            else if (k == "pinch") n.harmonic = 2;
            else if (k == "pop") n.bassPicking = 1;
            else if (k == "slap") n.bassPicking = 2;
            else if (k == "vib") { n.vibrato = true; n.vibratoStrength = v.empty() ? 80 : std::atoi(v.c_str()); }
            else if (k == "trem") n.tremolo = true;
            else if (k == "accent") n.accent = true;
            else if (k == "link") n.linkNext = true;
            else if (k == "ring") n.letRing = true;
            else if (k == "pick") n.picked = true;
            else if (k == "chord") n.chordId = std::atoi(v.c_str());
            else if (k == "bend")
            {
                n.bendFirst = (int) s.bends.size();
                const char* q = v.c_str();
                double bt = 0.0, bs = 0.0;
                int a2 = 0;
                while (std::sscanf(q, "%lf:%lf%n", &bt, &bs, &a2) == 2)
                {
                    s.bends.push_back(ChartBend { t + bt, (float) bs });
                    ++n.bendCount;
                    q += a2;
                    if (*q == ',') ++q;
                }
            }
        }
    }
    std::fclose(f);
    return true;
}

// ---- the instrument models (#50) ---------------------------------------------

struct RiffNote
{
    double t, dur;
    int string, fret;
    uint32_t flags;
    int chord = -1, chordSize = 1, chordIndex = 0;
    float bendTo = 0.0f;   // semitones reached at the end of the note
};

std::vector<RiffNote> guitarRiff()
{
    std::vector<RiffNote> r;
    // An E-major arpeggio across all six strings, ringing.
    const int arp[6][2] = { { 0, 0 }, { 1, 2 }, { 2, 2 }, { 3, 1 }, { 4, 0 }, { 5, 0 } };
    for (int i = 0; i < 6; ++i)
        r.push_back({ 0.10 + 0.28 * i, 1.6, arp[i][0], arp[i][1], DCA_CHART_NOTE_LET_RING });
    // A strummed A power chord.
    for (int k = 0; k < 3; ++k)
        r.push_back({ 2.0, 0.9, k + 1, k == 0 ? 0 : 2, 0, 1, 3, k });
    // A melodic line on the G and B strings, then a bend.
    const int line[5][2] = { { 3, 4 }, { 3, 6 }, { 4, 5 }, { 4, 7 }, { 4, 5 } };
    for (int i = 0; i < 5; ++i)
        r.push_back({ 3.1 + 0.24 * i, 0.22, line[i][0], line[i][1], 0 });
    r.push_back({ 4.4, 0.9, 4, 8, DCA_CHART_NOTE_VIBRATO, -1, 1, 0, 2.0f });
    // Palm-muted chugs on the low E, and a high note to end.
    for (int i = 0; i < 4; ++i)
        r.push_back({ 5.5 + 0.18 * i, 0.15, 0, 0, DCA_CHART_NOTE_PALM_MUTE });
    r.push_back({ 6.3, 1.2, 5, 12, DCA_CHART_NOTE_LET_RING });
    return r;
}

std::vector<RiffNote> bassRiff()
{
    std::vector<RiffNote> r;
    const int line[10][2] = { { 0, 0 }, { 0, 0 }, { 1, 2 }, { 1, 0 }, { 0, 3 }, { 0, 5 }, { 2, 2 }, { 2, 0 }, { 1, 5 }, { 1, 3 } };
    for (int i = 0; i < 10; ++i)
        r.push_back({ 0.10 + 0.32 * i, 0.30, line[i][0], line[i][1], 0 });
    for (int i = 0; i < 6; ++i)   // the same line with a pick
        r.push_back({ 3.5 + 0.32 * i, 0.30, line[i][0], line[i][1], DCA_CHART_NOTE_PICKED });
    r.push_back({ 5.6, 1.6, 0, 0, 0 });
    return r;
}

std::vector<float> renderRiff(dcsynth::ChartVoice& v, const std::vector<RiffNote>& riff, bool bass, double rate, double seconds)
{
    static const double gOpen[6] = { 40, 45, 50, 55, 59, 64 }, bOpen[4] = { 28, 33, 38, 43 };
    v.prepare(rate, 64);
    std::vector<float> out((size_t) (seconds * rate), 0.0f);
    std::vector<bool> on(riff.size(), false), off(riff.size(), false);
    for (size_t i = 0; i < out.size(); i += 64)
    {
        const double t = (double) i / rate;
        for (size_t k = 0; k < riff.size(); ++k)
        {
            const RiffNote& n = riff[k];
            if (! on[k] && t >= n.t)
            {
                on[k] = true;
                dcsynth::ChartNoteOn e;
                e.noteId = (int32_t) k; e.stringIndex = n.string; e.fret = n.fret;
                e.pitch = (float) ((bass ? bOpen[n.string] : gOpen[n.string]) + n.fret);
                e.velocity = 0.72f; e.flags = n.flags; e.chordId = n.chord; e.chordSize = n.chordSize; e.chordIndex = n.chordIndex;
                e.slideToPitch = e.pitch; e.durationSeconds = n.dur; e.sustainSeconds = n.dur;
                v.noteOn(e);
            }
            if (on[k] && ! off[k] && n.bendTo != 0.0f)
            {
                dcsynth::ChartNoteControls c;
                c.bendSemitones = (float) (n.bendTo * std::clamp((t - n.t) / (0.6 * n.dur), 0.0, 1.0));
                v.setNoteControls((int32_t) k, c);
            }
            if (on[k] && ! off[k] && t >= n.t + n.dur)
            {
                off[k] = true;
                v.noteOff((int32_t) k, dcsynth::NoteOffKind::Release);
            }
        }
        v.render(out.data() + i, (int) std::min<size_t>(64, out.size() - i));
    }
    return out;
}

/// Long-term average power spectrum (Hann, 8192, hop 4096) -> centroid and
/// band levels relative to the total, dB.
struct Spectrum { double centroid = 0, low = 0, mid = 0, presence = 0, air = 0; };
Spectrum analyse(const std::vector<float>& x, double rate)
{
    const int N = 8192;
    std::vector<double> power(N / 2 + 1, 0.0);
    std::vector<std::complex<double>> buf(N);
    for (size_t start = 0; start + N <= x.size(); start += N / 2)
    {
        for (int i = 0; i < N; ++i)
            buf[(size_t) i] = x[start + (size_t) i] * (0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / N));
        // iterative radix-2 FFT
        for (int i = 1, j = 0; i < N; ++i)
        {
            int bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(buf[(size_t) i], buf[(size_t) j]);
        }
        for (int len = 2; len <= N; len <<= 1)
        {
            const std::complex<double> w = std::polar(1.0, -2.0 * 3.141592653589793 / len);
            for (int i = 0; i < N; i += len)
            {
                std::complex<double> wn = 1.0;
                for (int k = 0; k < len / 2; ++k, wn *= w)
                {
                    const auto a = buf[(size_t) (i + k)], b = buf[(size_t) (i + k + len / 2)] * wn;
                    buf[(size_t) (i + k)] = a + b;
                    buf[(size_t) (i + k + len / 2)] = a - b;
                }
            }
        }
        for (int k = 0; k <= N / 2; ++k)
            power[(size_t) k] += std::norm(buf[(size_t) k]);
    }
    Spectrum s;
    double total = 0, num = 0, lo = 0, mi = 0, pr = 0, ai = 0;
    for (int k = 1; k <= N / 2; ++k)
    {
        const double f = k * rate / N, p = power[(size_t) k];
        if (f < 40.0 || f > 16000.0) continue;
        total += p; num += p * f;
        if (f < 300.0) lo += p; else if (f < 2000.0) mi += p; else if (f < 5000.0) pr += p; else ai += p;
    }
    auto db = [&](double v) { return 10.0 * std::log10((v + 1e-30) / (total + 1e-30)); };
    s.centroid = total > 0 ? num / total : 0;
    s.low = db(lo); s.mid = db(mi); s.presence = db(pr); s.air = db(ai);
    return s;
}

const char* pickupName(int32_t p)
{
    switch (p)
    {
        case kPickNeck: return "neck";
        case kPickNeckMiddle: return "neck-middle";
        case kPickMiddle: return "middle";
        case kPickMiddleBridge: return "middle-bridge";
        case kPickBridge: return "bridge";
        case kPickNeckBridge: return "neck-bridge";
        default: return "?";
    }
}

int modelsSet(const std::string& dir, double rate)
{
    FILE* csv = std::fopen((dir + "/spectra.csv").c_str(), "wb");
    FILE* md = std::fopen((dir + "/spectra.md").c_str(), "wb");
    if (! csv || ! md)
    {
        std::fprintf(stderr, "cannot write into %s\n", dir.c_str());
        return 2;
    }
    std::fprintf(csv, "file,model,pickup,coil_split,source_pack,recorded_as_is,centroid_hz,low_db,mid_db,presence_db,air_db,peak_dbfs\n");
    std::fprintf(md, "| file | pack | as recorded | centroid Hz | <300 Hz dB | 0.3-2 kHz dB | 2-5 kHz dB | >5 kHz dB |\n|---|---|---|---:|---:|---:|---:|---:|\n");
    int bad = 0;
    for (int32_t id : kAllModels)
    {
        const ModelSpec& m = *findModel(id);
        for (int i = 0; i < m.positionCount; ++i)
            for (int split = 0; split < (m.pos[i].splitDiffers ? 2 : 1); ++split)
            {
                InstrumentConfig c;
                c.model = id; c.pickup = m.pos[i].sel; c.coilSplit = split != 0;
                dcsynth::GuitarChartVoice v(m.bass, c);
                const auto riff = m.bass ? bassRiff() : guitarRiff();
                const std::vector<float> x = renderRiff(v, riff, m.bass, rate, 8.0);
                float peak = 0.0f;
                for (float s : x) peak = std::max(peak, std::fabs(s));
                const std::string name = std::string(m.bass ? "bass_" : "guitar_") + m.key + "_" + pickupName(c.pickup)
                                         + (c.coilSplit ? "_split" : "");
                writeWav(dir + "/" + name + ".wav", x, (int) rate);
                const Spectrum s = analyse(x, rate);
                std::fprintf(csv, "%s.wav,%s,%s,%d,%s,%d,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f\n", name.c_str(), m.key, pickupName(c.pickup),
                             split, v.packName().c_str(), v.recordedAsIs() ? 1 : 0, s.centroid, s.low, s.mid, s.presence, s.air,
                             20.0 * std::log10(peak + 1e-12));
                std::fprintf(md, "| %s | %s | %s | %.0f | %.1f | %.1f | %.1f | %.1f |\n", name.c_str(), v.packName().c_str(),
                             v.recordedAsIs() ? "yes" : "no", s.centroid, s.low, s.mid, s.presence, s.air);
                std::printf("%-42s %-20s %-3s centroid %7.1f Hz  low %6.1f  mid %6.1f  pres %6.1f  air %6.1f  peak %6.1f dBFS\n",
                            name.c_str(), v.packName().c_str(), v.recordedAsIs() ? "rec" : "sim", s.centroid, s.low, s.mid,
                            s.presence, s.air, 20.0 * std::log10(peak + 1e-12));
                if (m.bass)
                {
                    // The bass riff is fingers (0..3.4 s) then pick (3.5..5.5 s):
                    // two recorded basses, so each half is reported on its own.
                    const auto part = [&](double a, double b) {
                        return analyse(std::vector<float>(x.begin() + (long) (a * rate), x.begin() + (long) (b * rate)), rate);
                    };
                    const Spectrum f = part(0.0, 3.4), pk = part(3.5, 5.5);
                    std::printf("%-42s fingers centroid %7.1f Hz  pick centroid %7.1f Hz\n", "", f.centroid, pk.centroid);
                    std::fprintf(md, "| %s (fingers / pick) | | | %.0f / %.0f | | | | |\n", name.c_str(), f.centroid, pk.centroid);
                }
                if (! v.sampled() || peak >= 1.0f || peak < 1.0e-3f)
                    ++bad;
            }
    }
    std::fclose(csv);
    std::fclose(md);
    return bad == 0 ? 0 : 1;
}


// ---- engine-path events (#52) --------------------------------------------------
// The realism pass renders reference phrases through the SAME adapter the
// engine drives (dcsynth::GuitarChartVoice: DCA flags, strum spread, slide
// shaping, the selected instrument model), not through planChart. One event
// per line:
//   <time s> <duration s> <string> <fret> [token ...]
// tokens: palm dead harm pinch ho po tap trem accent vib unpitched slap pop
//         pick ghost ring link damped staccato slide=<fret> vel=<0..1>
//         chord=<id>/<size>/<index> strum=up|down bend=<t>:<semis>[,...]
//         (bend t relative to the note, semitones; linear between points)

struct EventNote
{
    double t = 0.0, dur = 0.0;
    int string = 0, fret = 0;
    uint32_t flags = 0;
    float velocity = 0.8f;
    int chord = -1, chordSize = 1, chordIndex = 0, strum = DCA_CHART_STRUM_UNKNOWN;
    int slideTo = -1;
    std::vector<std::pair<double, float>> bend;
};

bool parseEvents(const char* path, std::vector<EventNote>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (! f)
        return false;
    char line[1024];
    while (std::fgets(line, sizeof line, f))
    {
        if (char* hash = std::strchr(line, '#'))
            *hash = 0;
        EventNote n;
        int used = 0;
        if (std::sscanf(line, "%lf %lf %d %d%n", &n.t, &n.dur, &n.string, &n.fret, &used) < 4)
            continue;
        char* p = line + used;
        char tok[256];
        int adv = 0;
        while (std::sscanf(p, "%255s%n", tok, &adv) == 1)
        {
            p += adv;
            std::string k = tok, v;
            if (const size_t eq = k.find('='); eq != std::string::npos)
            {
                v = k.substr(eq + 1);
                k = k.substr(0, eq);
            }
            if (k == "palm") n.flags |= DCA_CHART_NOTE_PALM_MUTE;
            else if (k == "dead") n.flags |= DCA_CHART_NOTE_FRET_HAND_MUTE;
            else if (k == "harm") n.flags |= DCA_CHART_NOTE_HARMONIC;
            else if (k == "pinch") n.flags |= DCA_CHART_NOTE_PINCH_HARMONIC;
            else if (k == "ho") n.flags |= DCA_CHART_NOTE_HAMMER_ON;
            else if (k == "po") n.flags |= DCA_CHART_NOTE_PULL_OFF;
            else if (k == "tap") n.flags |= DCA_CHART_NOTE_TAP;
            else if (k == "trem") n.flags |= DCA_CHART_NOTE_TREMOLO;
            else if (k == "accent") n.flags |= DCA_CHART_NOTE_ACCENT;
            else if (k == "vib") n.flags |= DCA_CHART_NOTE_VIBRATO;
            else if (k == "unpitched") n.flags |= DCA_CHART_NOTE_UNPITCHED_SLIDE;
            else if (k == "slap") n.flags |= DCA_CHART_NOTE_SLAP;
            else if (k == "pop") n.flags |= DCA_CHART_NOTE_POP;
            else if (k == "pick") n.flags |= DCA_CHART_NOTE_PICKED;
            else if (k == "ghost") n.flags |= DCA_CHART_NOTE_GHOST;
            else if (k == "ring") n.flags |= DCA_CHART_NOTE_LET_RING;
            else if (k == "link") n.flags |= DCA_CHART_NOTE_LINK_NEXT;
            else if (k == "damped") n.flags |= DCA_CHART_NOTE_END_DAMPED;
            else if (k == "staccato") n.flags |= DCA_CHART_NOTE_STACCATO;
            else if (k == "slide") n.slideTo = std::atoi(v.c_str());
            else if (k == "vel") n.velocity = (float) std::atof(v.c_str());
            else if (k == "strum") n.strum = v == "up" ? DCA_CHART_STRUM_UP : DCA_CHART_STRUM_DOWN;
            else if (k == "chord") std::sscanf(v.c_str(), "%d/%d/%d", &n.chord, &n.chordSize, &n.chordIndex);
            else if (k == "bend")
            {
                const char* q = v.c_str();
                double bt = 0.0, bs = 0.0;
                int a2 = 0;
                while (std::sscanf(q, "%lf:%lf%n", &bt, &bs, &a2) == 2)
                {
                    n.bend.push_back({ bt, (float) bs });
                    q += a2;
                    if (*q == ',') ++q;
                }
            }
        }
        out.push_back(n);
    }
    std::fclose(f);
    std::stable_sort(out.begin(), out.end(), [](const EventNote& a, const EventNote& b) { return a.t < b.t; });
    return true;
}

float bendAt(const EventNote& n, double rel)
{
    if (n.bend.empty())
        return 0.0f;
    if (rel <= n.bend.front().first)
        return n.bend.front().first <= 0.0 ? n.bend.front().second
                                           : (float) (n.bend.front().second * std::max(0.0, rel) / n.bend.front().first);
    for (size_t i = 1; i < n.bend.size(); ++i)
        if (rel <= n.bend[i].first)
        {
            const double a = n.bend[i - 1].first, b = n.bend[i].first;
            const double u = b > a ? (rel - a) / (b - a) : 1.0;
            return (float) (n.bend[i - 1].second + u * (n.bend[i].second - n.bend[i - 1].second));
        }
    return n.bend.back().second;
}

std::vector<float> renderEvents(dcsynth::ChartVoice& v, const std::vector<EventNote>& ev, bool bass, int bassStrings, double rate)
{
    static const double gOpen[8] = { 40, 45, 50, 55, 59, 64, 64, 64 };
    static const double b4[8] = { 28, 33, 38, 43, 43, 43, 43, 43 }, b5[8] = { 23, 28, 33, 38, 43, 43, 43, 43 };
    const double* open = bass ? (bassStrings == 5 ? b5 : b4) : gOpen;
    constexpr int kBlock = 64;
    v.prepare(rate, kBlock);
    double seconds = 1.0;
    for (const auto& n : ev)
        seconds = std::max(seconds, n.t + n.dur + 1.5);
    std::vector<float> out((size_t) (seconds * rate), 0.0f);
    std::vector<bool> on(ev.size(), false), off(ev.size(), false);
    for (size_t i = 0; i < out.size(); i += kBlock)
    {
        const double t = (double) i / rate;
        for (size_t k = 0; k < ev.size(); ++k)
        {
            const EventNote& n = ev[k];
            if (! on[k] && t >= n.t)
            {
                on[k] = true;
                dcsynth::ChartNoteOn e;
                e.noteId = (int32_t) k; e.stringIndex = n.string; e.fret = n.fret;
                e.pitch = (float) (open[std::clamp(n.string, 0, 7)] + n.fret);
                e.velocity = n.velocity; e.flags = n.flags | (bass ? DCA_CHART_NOTE_BASS : 0u);
                e.chordId = n.chord; e.chordSize = n.chordSize; e.chordIndex = n.chordIndex; e.strum = n.strum;
                e.slideToPitch = n.slideTo >= 0 ? (float) (open[std::clamp(n.string, 0, 7)] + n.slideTo) : e.pitch;
                e.initialBendSemitones = bendAt(n, 0.0);
                e.durationSeconds = n.dur; e.sustainSeconds = n.dur;
                v.noteOn(e);
            }
            if (on[k] && ! off[k] && (! n.bend.empty() || n.slideTo >= 0))
            {
                dcsynth::ChartNoteControls c;
                c.bendSemitones = bendAt(n, t - n.t);
                if (n.slideTo >= 0)
                    c.slideSemitones = (float) ((n.slideTo - n.fret) * std::clamp((t - n.t) / std::max(n.dur, 1e-3), 0.0, 1.0));
                v.setNoteControls((int32_t) k, c);
            }
            if (on[k] && ! off[k] && t >= n.t + n.dur)
            {
                off[k] = true;
                v.noteOff((int32_t) k, dcsynth::NoteOffKind::Release);
            }
        }
        v.render(out.data() + i, (int) std::min<size_t>(kBlock, out.size() - i));
    }
    return out;
}

bool findModelByKey(const std::string& key, const std::string& pickup, bool split, InstrumentConfig& c)
{
    for (int32_t id : kAllModels)
    {
        const ModelSpec& m = *findModel(id);
        if (key != m.key)
            continue;
        c.model = id;
        c.pickup = m.defaultSel;
        c.coilSplit = split;
        if (! pickup.empty())
            for (int i = 0; i < m.positionCount; ++i)
                if (pickup == pickupName(m.pos[i].sel))
                    c.pickup = m.pos[i].sel;
        return true;
    }
    return false;
}

// ---- a typical rig (for the A/B set) --------------------------------------------
// The DI through header-only rig cores: guitar = a crunch amp (Plexi, gain
// 0.55) into its cab; bass = the SVT voicing, gain 0.35, into a bass cab. The
// same binary processes a "before" and an "after" DI, so the rig is identical.

bool readWav16(const std::string& path, std::vector<float>& x, int& rate)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (! f)
        return false;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0)
        b.insert(b.end(), buf, buf + got);
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0)
        return false;
    size_t p = 12;
    int ch = 1, bits = 16;
    while (p + 8 <= b.size())
    {
        uint32_t len;
        std::memcpy(&len, b.data() + p + 4, 4);
        if (std::memcmp(b.data() + p, "fmt ", 4) == 0)
        {
            uint16_t c, bt;
            uint32_t r;
            std::memcpy(&c, b.data() + p + 10, 2);
            std::memcpy(&r, b.data() + p + 12, 4);
            std::memcpy(&bt, b.data() + p + 22, 2);
            ch = c; rate = (int) r; bits = bt;
        }
        else if (std::memcmp(b.data() + p, "data", 4) == 0)
        {
            if (bits != 16)
                return false;
            const size_t n = std::min<size_t>(len, b.size() - p - 8) / (2 * (size_t) ch);
            x.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                int16_t s;
                std::memcpy(&s, b.data() + p + 8 + i * 2 * (size_t) ch, 2);
                x[i] = s / 32768.0f;
            }
            return true;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

int rigMode(const std::string& in, const std::string& outPath, bool bass)
{
    std::vector<float> x;
    int rate = 48000;
    if (! readWav16(in, x, rate))
    {
        std::fprintf(stderr, "cannot read %s\n", in.c_str());
        return 2;
    }
    dcdsp::AmpCore amp;
    dcdsp::CabCore cab;
    dcdsp::AmpParams ap;
    ap.voicing = bass ? 7 : 1;
    ap.gain = bass ? 0.35f : 0.55f;
    dcdsp::CabParams cp;
    cp.model = bass ? 3 : 0;
    cp.highCutHz = bass ? 5000.0f : 8000.0f;
    constexpr int kBlock = 128;
    amp.prepare(rate, kBlock, 1);
    amp.setParams(ap);
    cab.prepare(rate, kBlock, 1);
    cab.setParams(cp);
    for (size_t i = 0; i < x.size(); i += kBlock)
    {
        float* ch[1] = { x.data() + i };
        const int n = (int) std::min<size_t>(kBlock, x.size() - i);
        amp.process(ch, 1, n);
        cab.process(ch, 1, n);
    }
    float peak = 0.0f;
    for (float v : x) peak = std::max(peak, std::fabs(v));
    if (peak > 0.89f)
        for (float& v : x) v *= 0.89f / peak;
    return writeWav(outPath, x, rate) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    std::string set, notes, out, modelsDir, packDir, events, modelKey, pickupKey, rigIn;
    bool coilSplit = false, rigBass = false;
    double rate = 48000.0, bpm = 0.0;
    int bassStrings = 0;
    bool pickStyle = false;
    bool benchMode = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--listening-set") set = next();
        else if (a == "--notes") notes = next();
        else if (a == "--out") out = next();
        else if (a == "--rate") rate = std::atof(next().c_str());
        else if (a == "--bpm") bpm = std::atof(next().c_str());
        else if (a == "--bass") bassStrings = 4;
        else if (a == "--bass5") bassStrings = 5;
        else if (a == "--pick") pickStyle = true;
        else if (a == "--suffix") gSuffix = next();
        else if (a == "--models") modelsDir = next();
        else if (a == "--pack-dir")
        {
            const std::string dir = next();
            packDir = dir;
            std::string err;
            gHavePacks = loadPackFile(dir + "/guitar.dcvp", gGuitarPack, err) && loadPackFile(dir + "/bass.dcvp", gBassPack, err);
            if (! gHavePacks)
                std::fprintf(stderr, "note: guitar/bass packs not loaded (%s); the planChart modes use the waveguide\n", err.c_str());
        }
        else if (a == "--bench") benchMode = true;
        else if (a == "--events") events = next();
        else if (a == "--tune")
        {
            // Calibration only: slap.<field>=v,pop.<field>=v,slapRec.<field>=v,...
            // (SlapPopShape; slapRec = the recorded slap's, #109).
            const std::string spec = next();
            size_t at = 0;
            while (at < spec.size())
            {
                const size_t end = std::min(spec.find(',', at), spec.size());
                const std::string kv = spec.substr(at, end - at);
                at = end + 1;
                const size_t dot = kv.find('.'), eq = kv.find('=');
                if (dot == std::string::npos || eq == std::string::npos)
                    continue;
                const std::string kind = kv.substr(0, dot);
                SlapPopShape& sh = kind == "pop" ? slapPopShapes().pop
                                   : kind == "slapRec" ? slapPopShapes().slapRecorded : slapPopShapes().slap;
                const std::string f = kv.substr(dot + 1, eq - dot - 1);
                const double v = std::atof(kv.substr(eq + 1).c_str());
                if (f == "variant") sh.variant = (int) v;
                else if (f == "level") sh.level = v;
                else if (f == "brightHz") sh.brightHz = v;
                else if (f == "brightDb") sh.brightDb = v;
                else if (f == "floorDb") sh.floorDb = v;
                else if (f == "brightTau") sh.brightTau = v;
                else if (f == "clankDrive") sh.clankDrive = v;
                else if (f == "clankLevel") sh.clankLevel = v;
                else if (f == "clankHz") sh.clankHz = v;
                else if (f == "clankTau") sh.clankTau = v;
                else if (f == "thumbLevel") sh.thumbLevel = v;
                else if (f == "recorded") sh.recorded = (int) v;
            }
        }
        else if (a == "--model") modelKey = next();
        else if (a == "--pickup") pickupKey = next();
        else if (a == "--split") coilSplit = true;
        else if (a == "--rig") rigIn = next();
        else if (a == "--rig-bass") { rigIn = next(); rigBass = true; }
    }
    if (rate < 8000.0 || rate > 384000.0)
        rate = 48000.0;
    if (benchMode)
        return bench();
    if (! rigIn.empty())
        return rigMode(rigIn, out, rigBass);
    if (! events.empty())
    {
        // The ENGINE path: GuitarChartVoice with an instrument model, packs from
        // --pack-dir (or DOTCHARTER_VOICE_DIR).
        if (! packDir.empty())
        {
#ifdef _WIN32
            _putenv_s("DOTCHARTER_VOICE_DIR", packDir.c_str());
#else
            setenv("DOTCHARTER_VOICE_DIR", packDir.c_str(), 1);
#endif
        }
        std::vector<EventNote> ev;
        if (! parseEvents(events.c_str(), ev) || out.empty())
        {
            std::fprintf(stderr, "cannot read %s (or no --out)\n", events.c_str());
            return 2;
        }
        const bool bass = bassStrings > 0;
        InstrumentConfig c = defaultConfig(bass);
        if (! modelKey.empty() && ! findModelByKey(modelKey, pickupKey, coilSplit, c))
        {
            std::fprintf(stderr, "unknown model %s\n", modelKey.c_str());
            return 2;
        }
        dcsynth::GuitarChartVoice v(bass, c);
        const std::vector<float> x = renderEvents(v, ev, bass, bassStrings, rate);
        float peak = 0.0f;
        for (float s2 : x) peak = std::max(peak, std::fabs(s2));
        if (! writeWav(out, x, (int) rate))
            return 1;
        std::printf("wrote %s (%.2f s, %s, pack %s, peak %.1f dBFS)\n", out.c_str(), x.size() / rate, v.name(), v.packName().c_str(),
                    20.0 * std::log10(peak + 1e-12));
        return 0;
    }
    if (! modelsDir.empty())
    {
        if (packDir.empty())
        {
            std::fprintf(stderr, "--models needs --pack-dir\n");
            return 2;
        }
#ifdef _WIN32
        _putenv_s("DOTCHARTER_VOICE_DIR", packDir.c_str());
#else
        setenv("DOTCHARTER_VOICE_DIR", packDir.c_str(), 1);
#endif
        return modelsSet(modelsDir, rate);
    }
    if (! set.empty())
    {
        const bool ok = guitarSet(set, rate) & bassSet(set, rate);
        return ok ? 0 : 1;
    }
    if (notes.empty() || out.empty())
    {
        std::fprintf(stderr, "usage: %s --listening-set <dir> | --notes <file> --out <wav> [--bass|--bass5] [--pick] [--rate R] [--bpm B] [--pack-dir D] [--suffix S]\n", argv[0]);
        return 2;
    }
    Song s;
    if (! parseNotes(notes.c_str(), s))
    {
        std::fprintf(stderr, "cannot read %s\n", notes.c_str());
        return 2;
    }
    if (bpm > 0.0)
        s.grid(bpm, s.length);
    InstrumentSpec spec = bassStrings ? InstrumentSpec::bassGuitar(bassStrings) : InstrumentSpec::guitar();
    if (pickStyle)
        spec.defaultKind = ExcKind::Pick;
    ChartTimelineVoice voice = gHavePacks ? ChartTimelineVoice(spec, bassStrings ? &gBassPack : &gGuitarPack)
                                          : ChartTimelineVoice(spec, bassStrings ? bassExcitations() : guitarExcitations());
    const std::vector<float> x = render(s, voice, rate);
    if (! writeWav(out, x, (int) rate))
        return 1;
    std::printf("wrote %s (%.2f s)\n", out.c_str(), x.size() / rate);
    return 0;
}
