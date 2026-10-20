// Chart source (ABI v38): the scheduler's clock mapping, the note table's
// validation and the placeholder voice — deterministic, no device, no JUCE.
//
// The scheduler is driven exactly the way AudioEngine drives it: one
// SongClockSample per block, taken AFTER the pull, carrying the audible
// position at the END of the block. A recording voice notes the GLOBAL sample
// index (blocks * n + offset) at which every event took effect, which is what
// "sample-accurate" has to mean.

#include "synth/ChartNoteTable.h"
#include "synth/ChartScheduler.h"
#include "synth/ChartVoiceFactory.h"
#include "synth/PlaceholderPluckVoice.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace dcsynth;

namespace {

int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, std::string(msg).c_str()); } } while (0)

struct Event
{
    enum Kind { On, Off, AllOff, Controls } kind;
    int32_t     noteId;
    int64_t     sample;     // global sample the event took effect at
    NoteOffKind offKind = NoteOffKind::Release;
    double      chase   = 0.0;
    float       bend    = 0.0f;
    float       slide   = 0.0f;
    float       initialBend = 0.0f;
    int32_t     capo = 0;
};

class RecordingVoice final : public ChartVoice
{
public:
    std::vector<Event> events;
    int64_t rendered = 0;   // samples rendered so far = the sample the next event lands on
    int active = 0;

    const char* name() const noexcept override { return "recording"; }
    void prepare(double, int) override {}
    void reset() noexcept override { active = 0; }
    void noteOn(const ChartNoteOn& n) noexcept override
    {
        Event e{ Event::On, n.noteId, rendered };
        e.chase = n.chaseSeconds;
        e.initialBend = n.initialBendSemitones;
        e.capo = n.capo;
        events.push_back(e);
        ++active;
    }
    void noteOff(int32_t id, NoteOffKind k) noexcept override
    {
        Event e{ Event::Off, id, rendered }; e.offKind = k; events.push_back(e);
        if (active > 0) --active;
    }
    void allNotesOff(NoteOffKind k) noexcept override
    {
        Event e{ Event::AllOff, -1, rendered }; e.offKind = k; events.push_back(e);
        active = 0;
    }
    void setNoteControls(int32_t id, const ChartNoteControls& c) noexcept override
    {
        Event e{ Event::Controls, id, rendered }; e.bend = c.bendSemitones; e.slide = c.slideSemitones;
        events.push_back(e);
    }
    void render(float* out, int n) noexcept override
    {
        for (int i = 0; i < n; ++i) out[i] += 0.0f;
        rendered += n;
    }
    int activeNotes() const noexcept override { return active; }

    const Event* firstOn(int32_t id) const
    {
        for (const auto& e : events) if (e.kind == Event::On && e.noteId == id) return &e;
        return nullptr;
    }
    const Event* firstOff(int32_t id) const
    {
        for (const auto& e : events) if (e.kind == Event::Off && e.noteId == id) return &e;
        return nullptr;
    }
    int countOn(int32_t id) const
    {
        int c = 0; for (const auto& e : events) if (e.kind == Event::On && e.noteId == id) ++c; return c;
    }
};

/// The sample a chart time lands on (the scheduler's own rounding rule: a time
/// exactly on a sample boundary is ON that sample).
int64_t sampleOf(double t, double sr) { return (int64_t) std::floor(t * sr + 1.0e-6); }

dca_chart_note makeNote(double start, double release, float pitch = 52.0f, int string = 1, int fret = 2)
{
    dca_chart_note n{};
    n.start_seconds   = start;
    n.release_seconds = release;
    n.sustain_seconds = 0.0;
    n.pitch           = pitch;
    n.velocity        = 0.8f;
    n.string_index    = string;
    n.fret            = fret;
    n.slide_to_fret   = -1.0f;
    n.chord_id        = -1;
    n.bend_first      = -1;
    return n;
}

std::unique_ptr<ChartNoteTable> table(const std::vector<dca_chart_note>& notes,
                                      const std::vector<dca_chart_bend_point>& bends = {})
{
    std::string err;
    auto t = ChartNoteTable::build(notes.data(), (int32_t) notes.size(), (int32_t) sizeof(dca_chart_note),
                                   bends.empty() ? nullptr : bends.data(), (int32_t) bends.size(), err);
    if (t == nullptr) std::printf("  (table rejected: %s)\n", err.c_str());
    if (t) t->serial = 1;
    return t;
}

/// Drives a scheduler block by block over an ideal transport. Like a real
/// transport the position is derived from an INTEGER count (base + k blocks),
/// never accumulated, so a note on a sample boundary is on it exactly.
struct Rig
{
    double sr = 48000.0;
    int    n  = 256;
    double tempo = 1.0;
    double base = 0.0;         // song position the current run of blocks started at
    int64_t k = 0;             // blocks played since `base`
    double pos = 0.0;          // song position at the end of the last block
    uint32_t gen = 0;
    bool playing = true;
    double offset = 0.0;
    ChartScheduler sched;
    RecordingVoice voice;
    std::vector<float> buf;

    Rig() { buf.assign(4096, 0.0f); }

    void block(const ChartNoteTable* t)
    {
        if (playing) ++k;
        pos = base + (double) (k * n) * tempo / sr;
        SongClockSample c;
        c.valid = true; c.playing = playing; c.position = pos; c.seekGeneration = gen; c.tempo = tempo;
        sched.process(t, t ? t->serial : 0, &voice, 1, c, offset, sr, buf.data(), n);
    }
    void runUntil(const ChartNoteTable* t, double songSeconds)
    {
        while (pos < songSeconds) block(t);
    }
    /// The next block renders [to, to + block). `announce` = a seek (the source
    /// bumps its generation); false = a jump nobody announced.
    void jumpTo(double to, bool announce = true)
    {
        base = to; k = 0; pos = to;
        if (announce) ++gen;
    }
};

// ---------------------------------------------------------------------------

void testNoteOnLandsOnItsSample()
{
    std::printf("[ RUN ] NoteOnLandsOnItsSample\n");
    Rig r;
    // At 48 kHz / 256 a block is 5.333 ms. Note 0 sits mid-block, note 1 exactly
    // on a block boundary (block 200 starts at sample 51,200), note 2 on the
    // block's LAST sample.
    const double b200 = 51200.0 / r.sr;
    auto t = table({ makeNote(1.0, 1.2), makeNote(b200, b200 + 0.1, 55, 2),
                     makeNote((51200.0 + 511.0) / r.sr, 1.3, 57, 3) });
    r.runUntil(t.get(), 2.0);
    const auto* e0 = r.voice.firstOn(0);
    const auto* e1 = r.voice.firstOn(1);
    const auto* e2 = r.voice.firstOn(2);
    CHECK(e0 && e0->sample == 48000, "note at 1.000 s started at sample "
          + std::to_string(e0 ? e0->sample : -1) + ", expected 48000");
    CHECK(e1 && e1->sample == 51200, "note on a block boundary started at sample "
          + std::to_string(e1 ? e1->sample : -1) + ", expected 51200 (the NEXT block's first sample)");
    CHECK(e2 && e2->sample == 51200 + 511, "note on a block's last sample started at "
          + std::to_string(e2 ? e2->sample : -1) + ", expected 51711");
    const auto* off0 = r.voice.firstOff(0);
    CHECK(off0 && off0->sample == sampleOf(1.2, r.sr) && off0->offKind == NoteOffKind::Release,
          "note-off at its release sample (got " + std::to_string(off0 ? off0->sample : -1) + ")");
}

void testDenseRunNeitherSkipsNorRepeats()
{
    std::printf("[ RUN ] DenseRunNeitherSkipsNorRepeats\n");
    Rig r;
    std::vector<dca_chart_note> notes;
    for (int i = 0; i < 1000; ++i)
        notes.push_back(makeNote(0.5 + i * 0.007, 0.5 + i * 0.007 + 0.005, 50.0f + (float) (i % 12), i % 6));
    auto t = table(notes);
    r.runUntil(t.get(), 8.0);
    int bad = 0;
    for (int i = 0; i < 1000; ++i)
    {
        if (r.voice.countOn(i) != 1) ++bad;
        const auto* e = r.voice.firstOn(i);
        if (e && e->sample != sampleOf(0.5 + i * 0.007, r.sr)) ++bad;
    }
    CHECK(bad == 0, std::to_string(bad) + " of 1000 notes were skipped, repeated or mistimed");
}

void testSeekInsideANoteChasesIt()
{
    std::printf("[ RUN ] SeekInsideANoteChasesIt\n");
    Rig r;
    auto t = table({ makeNote(1.0, 3.0), makeNote(2.5, 2.7, 60, 3) });
    r.runUntil(t.get(), 1.5);
    CHECK(r.voice.countOn(0) == 1, "note 0 started before the seek");
    const size_t before = r.voice.events.size();
    const int64_t seekSample = r.voice.rendered;
    r.jumpTo(2.0);   // the next block covers [2.0, 2.0053)
    r.block(t.get());
    bool choked = false;
    for (size_t i = before; i < r.voice.events.size(); ++i)
        if (r.voice.events[i].kind == Event::AllOff && r.voice.events[i].offKind == NoteOffKind::Choke) choked = true;
    CHECK(choked, "a seek did not choke what was sounding");
    CHECK(r.voice.countOn(0) == 2, "the note under the seek point was not re-started");
    const Event* chased = nullptr;
    for (size_t i = before; i < r.voice.events.size(); ++i)
        if (r.voice.events[i].kind == Event::On && r.voice.events[i].noteId == 0) chased = &r.voice.events[i];
    CHECK(chased && chased->sample == seekSample, "the chased note did not start on the block's first sample");
    CHECK(chased && std::abs(chased->chase - (2.0 + 256.0 / r.sr - 256.0 / r.sr - 1.0)) < 1.0e-9,
          "chaseSeconds " + std::to_string(chased ? chased->chase : -1) + " is not the time into the note (1.0)");
    r.runUntil(t.get(), 3.2);
    const auto* e1 = r.voice.firstOn(1);
    CHECK(e1 && e1->sample == seekSample + sampleOf(2.5 - 2.0, r.sr),
          "the note after the seek point is not sample-accurate (got "
          + std::to_string(e1 ? e1->sample - seekSample : -1) + " samples after the seek)");
}

/// A seek SMALLER than the jump tolerance is only visible through the source's
/// seek generation — the position alone looks like ordinary play.
void testSmallSeekIsStillASeek()
{
    std::printf("[ RUN ] SmallSeekIsStillASeek\n");
    Rig r;
    auto t = table({ makeNote(1.0, 3.0) });
    r.runUntil(t.get(), 1.5);
    r.jumpTo(r.pos + 0.1);
    r.block(t.get());
    CHECK(r.voice.countOn(0) == 2, "a 100 ms seek inside a held note did not re-start it");
}

void testLoopWrapReplaysTheRegion()
{
    std::printf("[ RUN ] LoopWrapReplaysTheRegion\n");
    for (int announced = 0; announced < 2; ++announced)
    {
        Rig r;
        auto t = table({ makeNote(1.0, 1.1), makeNote(1.6, 1.7, 55, 2) });
        r.runUntil(t.get(), 2.0);
        // Wrap to 0.9. Announced = a seek generation bump (DotCharter's loop is a
        // SeekTo). Unannounced = a backward jump the scheduler must see on its own.
        r.jumpTo(0.9, announced != 0);
        r.runUntil(t.get(), 2.0);
        CHECK(r.voice.countOn(0) == 2 && r.voice.countOn(1) == 2,
              std::string(announced ? "announced" : "unannounced") + " loop wrap did not replay both notes once more ("
              + std::to_string(r.voice.countOn(0)) + ", " + std::to_string(r.voice.countOn(1)) + ")");
    }
}

void testHalfSpeedMovesTimeNotPitch()
{
    std::printf("[ RUN ] HalfSpeedMovesTimeNotPitch\n");
    Rig r;
    r.tempo = 0.5;
    auto t = table({ makeNote(1.0, 1.5) });
    r.runUntil(t.get(), 2.0);
    const auto* on  = r.voice.firstOn(0);
    const auto* off = r.voice.firstOff(0);
    CHECK(on && on->sample == 96000, "at half speed a note at 1.0 s started at wall sample "
          + std::to_string(on ? on->sample : -1) + ", expected 96000");
    CHECK(off && off->sample == 144000, "at half speed a 0.5 s note lasted "
          + std::to_string(off && on ? off->sample - on->sample : -1) + " samples, expected 48000");
    // The voice is asked for EVERY device sample: it never renders faster or
    // slower than the device, which is what keeps the pitch.
    CHECK(r.voice.rendered == r.k * r.n, "the voice rendered "
          + std::to_string(r.voice.rendered) + " samples for " + std::to_string(r.pos / 0.5) + " s of wall time");
}

void testPauseReleasesAndResumeChases()
{
    std::printf("[ RUN ] PauseReleasesAndResumeChases\n");
    Rig r;
    auto t = table({ makeNote(1.0, 4.0), makeNote(2.0, 2.1, 60, 3) });
    r.runUntil(t.get(), 1.5);
    r.playing = false;
    const size_t before = r.voice.events.size();
    for (int i = 0; i < 200; ++i) r.block(t.get());   // ~1 s paused
    bool released = false; int ons = 0;
    for (size_t i = before; i < r.voice.events.size(); ++i)
    {
        if (r.voice.events[i].kind == Event::AllOff && r.voice.events[i].offKind == NoteOffKind::Release) released = true;
        if (r.voice.events[i].kind == Event::On) ++ons;
    }
    CHECK(released, "pausing did not release the sounding note");
    CHECK(ons == 0, "a note started while paused");
    CHECK(r.sched.stats.clockPlaying.load() == 0, "status still says the clock is playing");
    r.playing = true;
    r.block(t.get());
    CHECK(r.voice.countOn(0) == 2, "resuming inside a held note did not chase it");
    r.runUntil(t.get(), 2.5);
    CHECK(r.voice.countOn(1) == 1, "the note after the pause point did not play");
}

void testBackwardWobbleDoesNotRefire()
{
    std::printf("[ RUN ] BackwardWobbleDoesNotRefire\n");
    Rig r;
    auto t = table({ makeNote(1.0, 1.05) });
    r.runUntil(t.get(), 1.002);
    r.jumpTo(r.pos - 10.0 / r.sr - 256.0 / r.sr, false);   // the next block reports 10 samples BEHIND the last
    r.block(t.get());
    r.runUntil(t.get(), 1.2);
    CHECK(r.voice.countOn(0) == 1, "a stretcher wobble re-fired a note ("
          + std::to_string(r.voice.countOn(0)) + " note-ons)");
}

void testOffsetShiftsEveryNote()
{
    std::printf("[ RUN ] OffsetShiftsEveryNote\n");
    Rig r;
    r.offset = 0.1;
    auto t = table({ makeNote(1.0, 1.1) });
    r.runUntil(t.get(), 1.5);
    const auto* on = r.voice.firstOn(0);
    CHECK(on && on->sample == sampleOf(1.1, r.sr), "a +100 ms offset put the note at sample "
          + std::to_string(on ? on->sample : -1));
}

void testBendCurveDrivesControls()
{
    std::printf("[ RUN ] BendCurveDrivesControls\n");
    Rig r;
    std::vector<dca_chart_note> notes = { makeNote(1.0, 2.0), makeNote(3.0, 4.0, 55, 2) };
    notes[0].bend_first = 0; notes[0].bend_count = 2;   // ramp to 2 st at 1.5, release to 0 at 1.8
    notes[1].bend_first = 2; notes[1].bend_count = 1;   // pre-bend: 1 st AT the attack
    std::vector<dca_chart_bend_point> bends = { { 1.5, 2.0f, 0 }, { 1.8, 0.0f, 0 }, { 3.0, 1.0f, 0 } };
    auto t = table(notes, bends);
    r.runUntil(t.get(), 3.5);
    float peak = 0.0f; float at125 = -1.0f;
    for (const auto& e : r.voice.events)
        if (e.kind == Event::Controls && e.noteId == 0)
        {
            peak = std::max(peak, e.bend);
            // Controls aim at the END of the segment they precede; pick the one
            // whose segment ends at 1.25 s (sample 60,000).
            if (e.sample + 32 == 60000) at125 = e.bend;
        }
    CHECK(std::abs(peak - 2.0f) < 0.01f, "the bend never reached its 2-semitone point (peak "
          + std::to_string(peak) + ")");
    CHECK(std::abs(at125 - 1.0f) < 0.01f, "half-way up the ramp the bend is " + std::to_string(at125) + ", expected 1.0");
    const auto* pre = r.voice.firstOn(1);
    CHECK(pre && std::abs(pre->initialBend - 1.0f) < 1.0e-6f, "a pre-bend did not start bent");
}

/// The capo travels from the uploaded note to the voice untouched, beside the
/// fret as charted and the pitch that already includes it (natural harmonics are
/// measured from the capo, so a voice cannot derive it from pitch - fret).
void testCapoReachesTheVoice()
{
    std::printf("[ RUN ] CapoReachesTheVoice\n");
    Rig r;
    auto n = makeNote(1.0, 1.2, 40.0f + 14.0f, 0, 14);
    n.capo = 2;
    n.flags = DCA_CHART_NOTE_HARMONIC;
    auto t = table({ n });
    r.runUntil(t.get(), 1.5);
    const auto* on = r.voice.firstOn(0);
    CHECK(on && on->capo == 2, "the voice saw capo " + std::to_string(on ? on->capo : -1) + ", expected 2");
    std::string err;
    auto bad = makeNote(1.0, 1.2); bad.capo = 30;
    CHECK(! ChartNoteTable::build(&bad, 1, sizeof(dca_chart_note), nullptr, 0, err), "a capo past fret 24 was accepted");
}

void testTableValidation()
{
    std::printf("[ RUN ] TableValidation\n");
    std::string err;
    auto ok = makeNote(1.0, 1.5);
    std::vector<dca_chart_note> unsorted = { makeNote(2.0, 2.1), makeNote(1.0, 1.1) };
    CHECK(! ChartNoteTable::build(unsorted.data(), 2, sizeof(dca_chart_note), nullptr, 0, err),
          "an unsorted list was accepted");
    auto backwards = makeNote(1.0, 0.5);
    CHECK(! ChartNoteTable::build(&backwards, 1, sizeof(dca_chart_note), nullptr, 0, err),
          "release before start was accepted");
    auto badBend = ok; badBend.bend_first = 0; badBend.bend_count = 3;
    dca_chart_bend_point one{ 1.2, 1.0f, 0 };
    CHECK(! ChartNoteTable::build(&badBend, 1, sizeof(dca_chart_note), &one, 1, err),
          "a bend range past the bend array was accepted");
    auto nanPitch = ok; nanPitch.pitch = std::nanf("");
    CHECK(! ChartNoteTable::build(&nanPitch, 1, sizeof(dca_chart_note), nullptr, 0, err), "a NaN pitch was accepted");
    CHECK(! ChartNoteTable::build(&ok, 1, 64, nullptr, 0, err), "a stride below V38 was accepted");
    // A NEWER caller's bigger element: the prefix is read, the tail ignored.
    struct Wider { dca_chart_note n; double extra; } wide[2] = { { makeNote(1.0, 1.1), 7.0 }, { makeNote(2.0, 2.1), 9.0 } };
    auto w = ChartNoteTable::build(wide, 2, (int32_t) sizeof(Wider), nullptr, 0, err);
    CHECK(w && w->count() == 2 && w->notes[1].start_seconds == 2.0, "a wider stride was not read element by element");
    // Chord membership by string.
    auto c0 = makeNote(1.0, 1.5, 52, 3); c0.chord_id = 7;
    auto c1 = makeNote(1.0, 1.5, 47, 1); c1.chord_id = 7;
    auto c2 = makeNote(1.0, 1.5, 40, 0); c2.chord_id = 7;
    std::vector<dca_chart_note> chord = { c0, c1, c2 };
    auto ct = ChartNoteTable::build(chord.data(), 3, sizeof(dca_chart_note), nullptr, 0, err);
    CHECK(ct && ct->chordSize[0] == 3 && ct->chordIndex[0] == 2 && ct->chordIndex[2] == 0,
          "chord size / index by string is wrong");
}

/// Fundamental by normalised autocorrelation over [40 Hz, 1 kHz] — a plucked
/// string is rich in harmonics, so counting zero crossings measures those.
double pitchHz(const std::vector<float>& x, double sr, size_t from)
{
    const size_t win = 4096;
    if (x.size() < from + win + (size_t) (sr / 40.0)) return 0.0;
    const int minLag = (int) (sr / 1000.0), maxLag = (int) (sr / 40.0);
    double best = -1.0; int bestLag = 0;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double num = 0.0, e0 = 0.0, e1 = 0.0;
        for (size_t i = from; i < from + win; ++i)
        {
            num += (double) x[i] * x[i + (size_t) lag];
            e0  += (double) x[i] * x[i];
            e1  += (double) x[i + (size_t) lag] * x[i + (size_t) lag];
        }
        const double r = num / std::sqrt(e0 * e1 + 1.0e-20);
        // The first lag within 2 % of the best is the period, not a multiple of it.
        if (r > best * 1.02) { best = r; bestLag = lag; }
    }
    return bestLag > 0 ? sr / (double) bestLag : 0.0;
}

void testPlaceholderVoiceSoundsAndStops()
{
    std::printf("[ RUN ] PlaceholderVoiceSoundsAndStops\n");
    const double sr = 48000.0;
    CHECK(voiceAvailability(DCA_CHART_VOICE_PLACEHOLDER) == 1, "the placeholder is not available");
    CHECK(voiceAvailability(DCA_CHART_VOICE_GUITAR) == 1 && voiceAvailability(DCA_CHART_VOICE_BASS) == 1,
          "the built-in guitar and bass voices are not advertised");
    {
        auto g = makeVoice(DCA_CHART_VOICE_GUITAR);
        auto b = makeVoice(DCA_CHART_VOICE_BASS);
        CHECK(g != nullptr && b != nullptr, "the built-in guitar / bass voice cannot be constructed");
    }
    CHECK(voiceAvailability(99) == -1, "an unknown voice kind is not -1");
    auto v = makeVoice(DCA_CHART_VOICE_PLACEHOLDER);
    v->prepare(sr, 512);
    ChartNoteOn on; on.noteId = 0; on.pitch = 45.0f; on.velocity = 0.9f; on.slideToPitch = 45.0f;
    v->noteOn(on);
    std::vector<float> out(256 * 94, 0.0f);   // a whole number of 256-sample renders
    for (size_t i = 0; i < out.size(); i += 256) v->render(out.data() + i, 256);
    float peak = 0.0f; bool finite = true;
    for (float s : out) { peak = std::max(peak, std::abs(s)); if (! std::isfinite(s)) finite = false; }
    CHECK(finite, "the voice produced a NaN/Inf");
    CHECK(peak > 0.05f && peak < 1.0f, "a hard pluck peaked at " + std::to_string(peak));
    const double hz = pitchHz(out, sr, 4800);
    CHECK(std::abs(hz - 110.0) < 3.0, "A2 (110 Hz) measured " + std::to_string(hz) + " Hz");

    // A bend control raises the pitch by two semitones.
    ChartNoteControls c; c.bendSemitones = 2.0f;
    v->setNoteControls(0, c);
    std::vector<float> bent((size_t) sr / 4, 0.0f);
    for (size_t i = 0; i < bent.size(); i += 32) { v->setNoteControls(0, c); v->render(bent.data() + i, 32); }
    const double bentHz = pitchHz(bent, sr, 2400);
    CHECK(std::abs(bentHz - 123.47) < 4.0, "a +2 st bend measured " + std::to_string(bentHz) + " Hz, expected ~123.5");

    v->noteOff(0, NoteOffKind::Choke);
    std::vector<float> tail(4800, 0.0f);
    for (size_t i = 0; i < tail.size(); i += 240) v->render(tail.data() + i, 240);
    float late = 0.0f;
    for (size_t i = 480; i < tail.size(); ++i) late = std::max(late, std::abs(tail[i]));
    CHECK(late == 0.0f, "10 ms after a choke the voice is still at " + std::to_string(late));
    CHECK(v->activeNotes() == 0, "a choked note is still active");
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not eat the log
    testNoteOnLandsOnItsSample();
    testDenseRunNeitherSkipsNorRepeats();
    testSeekInsideANoteChasesIt();
    testSmallSeekIsStillASeek();
    testLoopWrapReplaysTheRegion();
    testHalfSpeedMovesTimeNotPitch();
    testPauseReleasesAndResumeChases();
    testBackwardWobbleDoesNotRefire();
    testOffsetShiftsEveryNote();
    testBendCurveDrivesControls();
    testCapoReachesTheVoice();
    testTableValidation();
    testPlaceholderVoiceSoundsAndStops();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
