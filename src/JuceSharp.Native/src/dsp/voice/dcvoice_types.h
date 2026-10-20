// dcvoice_types.h — the chart voice's event model and the interface a chart
// source drives it through. This is the ONLY header the engine side (issue #39
// step 0) needs to know besides the factory in dcvoice_instrument.h.
//
// TIME IS SONG TIME, in seconds. The voice renders against the transport
// clock it is handed per block (VoiceClock), so practice speed only changes
// how fast song time advances per sample — note pitch, decay and strum spread
// stay physical (real time), while bends and slides land on their chart
// positions.
//
// OWNERSHIP: a VoiceTimeline is a read-only VIEW. The caller owns the arrays,
// keeps them alive and unchanged while any render() can see them, and swaps a
// whole new timeline in by handing a different one (generation bumped) to the
// next render(). The voice never allocates, locks or frees on render().
#pragma once

#include <cstdint>

namespace dcvoice {

/// One point of a bend curve: absolute song time, pitch offset in semitones
/// (the chart's BendValue.Step: 1.0 = a half step).
struct BendPoint
{
    double time;
    float semitones;
};

enum VoiceNoteFlags : uint32_t
{
    kFlagPalmMute       = 1u << 0,
    kFlagFullMute       = 1u << 1,   // dead note / ghost on bass
    kFlagHammerOn       = 1u << 2,
    kFlagPullOff        = 1u << 3,
    kFlagTap            = 1u << 4,
    kFlagHarmonic       = 1u << 5,   // natural harmonic at `fret`
    kFlagPinchHarmonic  = 1u << 6,
    kFlagAccent         = 1u << 7,
    kFlagUnpitchedSlide = 1u << 8,   // slide out to slideToFret (or ~a fifth down) and fade
    kFlagSlap           = 1u << 9,   // bass
    kFlagPop            = 1u << 10,  // bass
    kFlagLinked         = 1u << 11,  // continues the previous note on this string: no attack
    kFlagUpStroke       = 1u << 12,  // pick direction (strum heuristic / tremolo)
    kFlagTremoloRepeat  = 1u << 13,  // a re-pick the planner generated inside a tremolo note
    kFlagLetRing        = 1u << 14,  // informational: the planner already extended releaseTime
    kFlagTremolo        = 1u << 15,  // event mode only: the VOICE re-picks (the planner expands it instead)
    kFlagPicked         = 1u << 16,  // bass: plectrum (else fingers)
    kFlagGhost          = 1u << 17,  // very soft (bass ghost note when combined with a full mute)
    kFlagEndDamped      = 1u << 18,  // its note-off is a fretting-hand DAMP by the next note: cut, no release noise
};

/// One voice event. Produced by dcvoice::planChart() (control thread) from
/// chart-shaped notes; sorted by startTime.
struct VoiceNote
{
    double startTime;     // song s, strum offset already applied
    double anchorTime;    // song s: the chart note's own time (slide curves run from here;
                          // a tremolo re-pick keeps its parent note's anchor)
    double releaseTime;   // song s: the fret hand lets go / the palm damps
    double sustainEnd;    // song s: the chart sustain end (slides end here)
    int16_t string;       // 0 = lowest-pitched string
    int16_t fret;         // 0 = open
    int16_t slideToFret;  // -1 = none
    int16_t slideInFrets; // signed, 0 = none: slide INTO the note from this many frets away
    uint32_t flags;       // VoiceNoteFlags
    float velocity;       // 0..1
    float vibratoSemis;   // vibrato depth, 0 = none
    int32_t bendFirst;    // index into VoiceTimeline::bends
    int32_t bendCount;    // 0 = no bend
};

struct VoiceTimeline
{
    const VoiceNote* notes = nullptr;
    int count = 0;
    const BendPoint* bends = nullptr;
    int bendCount = 0;
    /// Bump when the arrays change; the voice re-seeks its cursor.
    uint32_t generation = 0;
};

/// The transport, per block.
struct VoiceClock
{
    double songTime = 0.0;                // song s at the block's first sample
    double songSecondsPerSample = 0.0;    // 1/fs at 1x, 0.5/fs at half speed
    bool rolling = false;                 // false: everything is damped, nothing starts
};

/// What a chart source drives. Implemented by StringInstrument.
///
///   prepare()  control thread; allocates; never concurrent with render()
///   render()   audio thread; OVERWRITES `mono` with numSamples of DI signal
///   silence()  audio thread; damps every string (seek, stop, loop wrap)
///
/// A jump in VoiceClock::songTime (seek, loop) is detected inside render()
/// and handled as silence() + re-seek, so a caller need not call it.
class IChartVoice
{
public:
    virtual ~IChartVoice() = default;
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void render(float* mono, int numSamples, const VoiceClock& clock,
                        const VoiceTimeline& timeline) noexcept = 0;
    virtual void silence() noexcept = 0;
};

} // namespace dcvoice
