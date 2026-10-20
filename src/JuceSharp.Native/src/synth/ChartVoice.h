#pragma once

// The VOICE of the chart source (ABI v38): what turns a chart's note events into
// a mono DI signal for a seat's rig. This is the interface the built-in guitar
// and bass voices (#39 step 1 and the bass step) implement. It is plain C++17 —
// no JUCE — so a voice and its tests build without the engine.
//
// WHO CALLS WHAT, ON WHICH THREAD
//
//   * `prepare` runs on a CONTROL thread, before the voice is published to the
//     audio thread and again on every device (re)start, never while a callback
//     can be inside this instance. It may allocate: size every buffer for the
//     worst case here (the lowest note at the highest rate, `maxBlockSize`).
//   * Everything else runs on the AUDIO thread, from ChartScheduler only:
//     no allocation, no locks, no I/O, no logging, and nothing may throw (the
//     methods are noexcept; an escaping exception is std::terminate).
//
// TIME
//
//   The scheduler owns time. It splits each device block at every note event
//   and at least every kControlStepSamples, applies the events due at the
//   start of a segment, updates continuous controls, then calls `render` for
//   that segment. A voice therefore never sees a sample offset: an event
//   applies at the first sample of the next `render`. Durations in ChartNoteOn
//   are SONG seconds; `speed` is song seconds per wall second (practice speed),
//   so a voice that wants a wall-clock duration divides by it. The voice
//   renders at the DEVICE rate whatever the speed — slow practice never changes
//   pitch.
//
// NOTES
//
//   HOW a note ends is in its flags at note-on: DCA_CHART_NOTE_END_DAMPED means
//   the Release that will arrive is the next note on the same string damping
//   it (cut it, a hand lifting off), DCA_CHART_NOTE_END_SHAPE that it rang to its
//   hand shape's end, neither that it ends at its own sustain. The scheduler
//   sends the noteOff at release_seconds either way.
//
//   A note is identified by `noteId` (its index in the uploaded table), which is
//   unique for as long as that table lives. The scheduler sends exactly one
//   noteOff per noteOn, except after `allNotesOff` / `reset`, which end every
//   note at once. A voice is free to choke a sounding note itself when a new
//   one starts on the same string (a real string sounds one pitch at a time);
//   a later noteOff for a note the voice already dropped must be ignored.

#include <cstdint>
#include <memory>
#include <vector>

namespace dcvoice { struct SamplePack; }

namespace dcsynth {

struct ChartNoteTable;

/// How a note ends.
enum class NoteOffKind : int32_t
{
    /// The note's authored or host-computed end: let it decay naturally (a
    /// fretting hand lifting off — tens of milliseconds, not a click).
    Release = 0,
    /// Cut it NOW without a click (<= 5 ms): a seek, a table swap, a voice
    /// swap, the source being disabled, or the note pool stealing a slot.
    Choke = 1,
};

/// Everything a voice needs to start a note. The articulation is carried
/// verbatim from the uploaded dca_chart_note so a voice can render any
/// DCA_CHART_NOTE_* flag without an ABI change.
struct ChartNoteOn
{
    int32_t  noteId      = -1;    // index in the uploaded table
    int32_t  stringIndex = 0;     // 0 = lowest-pitched string
    int32_t  fret        = 0;     // as charted, from the NUT (0 = open; the capo is already in `pitch`)
    int32_t  capo        = 0;     // the capo fret (0 = none): node positions (harmonics) count from it
    float    pitch       = 0.0f;  // MIDI note the string sounds unbent, fractional allowed
    float    velocity    = 0.0f;  // 0..1
    uint32_t flags       = 0;     // DCA_CHART_NOTE_* (dotcharter_audio.h)
    int32_t  chordId     = -1;    // -1 = single note
    int32_t  chordSize   = 1;     // notes in this chord (1 for a single note)
    int32_t  chordIndex  = 0;     // position in the chord, 0 = lowest string
    int32_t  strum       = 0;     // DCA_CHART_STRUM_*
    float    slideToPitch = 0.0f; // where a slide ends; == pitch when there is none
    float    initialBendSemitones  = 0.0f; // bend AT the attack: a pre-bend, or mid-bend when chased
    float    initialSlideSemitones = 0.0f; // slide progress at the attack (non-zero only when chased)
    float    speed       = 1.0f;  // song seconds per wall second at the attack
    double   durationSeconds = 0.0; // release - start, SONG seconds
    double   sustainSeconds  = 0.0; // authored sustain, SONG seconds (0 = none)
    double   chaseSeconds    = 0.0; // > 0: started this far INTO the note after a seek or
                                    // resume — skip the pick attack, enter mid-sustain
};

/// Continuous pitch controls, both in semitones relative to `pitch`. The
/// scheduler evaluates the chart's bend curve and slide at the END of the
/// segment it is about to render; the voice glides to them across that render
/// call (so a 32-sample control step is a smooth line, never a staircase).
struct ChartNoteControls
{
    float bendSemitones  = 0.0f;
    float slideSemitones = 0.0f;
};

class ChartVoice
{
public:
    virtual ~ChartVoice() = default;

    /// Short ASCII name for logs ("placeholder-pluck"). Never null.
    virtual const char* name() const noexcept = 0;

    /// CONTROL thread. See the file comment. Must leave the voice silent.
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;

    /// AUDIO thread. Silence everything immediately (no fade) and forget every note.
    virtual void reset() noexcept = 0;

    virtual void noteOn(const ChartNoteOn& note) noexcept = 0;
    virtual void noteOff(int32_t noteId, NoteOffKind kind) noexcept = 0;
    virtual void allNotesOff(NoteOffKind kind) noexcept = 0;
    virtual void setNoteControls(int32_t noteId, const ChartNoteControls& controls) noexcept = 0;

    /// ADD `numSamples` of mono DI into `monoOut` (never overwrite). DI level:
    /// a hard pick of a single note peaks around 0.5, like a passive pickup
    /// into an instrument input. `numSamples` <= the prepared maxBlockSize.
    virtual void render(float* monoOut, int numSamples) noexcept = 0;

    /// Notes still producing sound (including releasing tails).
    virtual int activeNotes() const noexcept = 0;

    /// AUDIO thread. This voice is being REPLACED (a model or pickup change):
    /// fade everything out over about `samples` and forget every note, so the
    /// old voice and its successor cross-fade rather than the old one stopping
    /// dead. The default is a choke (<= 5 ms).
    virtual void fadeOutAll(int samples) noexcept
    {
        (void) samples;
        allNotesOff(NoteOffKind::Choke);
    }

    /// True when the voice plays real recordings (a sample pack). Any thread
    /// after prepare().
    virtual bool sampled() const noexcept { return false; }
    /// Bytes of sample pack this voice holds (shared packs counted in full; a
    /// lazy pack's DECODED zones only, so it grows as zones are decoded). Any
    /// thread after prepare().
    virtual int64_t packBytes() const noexcept { return 0; }

    // ---- LAZY ZONES (#417). The engine's voices load their packs' zone tables
    // only and decode the zones a chart can play. All CONTROL thread; the
    // defaults are a voice without packs.

    /// Before prepare(): load lazily; `fresh` = new pack instances (not the
    /// shared ones), so the zones an older chart decoded are not carried over.
    virtual void useLazyZones(bool fresh) { (void) fresh; }
    /// Decodes (now, on this thread) every zone `table` can play. May take
    /// hundreds of milliseconds; never touches what the audio thread reads
    /// except by publishing decoded zones. Safe concurrently with rendering.
    virtual void preloadZones(const ChartNoteTable& table) { (void) table; }
    /// Bytes decoded that `table` would not ask for (what a fresh voice saves).
    virtual int64_t staleZoneBytes(const ChartNoteTable& table) const { (void) table; return 0; }
    /// The lazy packs this voice plays, for a worker that decodes the zones the
    /// audio thread asked for (dcvoice::serviceZoneRequests).
    virtual void lazyPacks(std::vector<std::shared_ptr<const dcvoice::SamplePack>>& out) const { (void) out; }
};

} // namespace dcsynth
