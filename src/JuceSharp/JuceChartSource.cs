using System.Runtime.InteropServices;

namespace JuceSharp;

// ABI v38: the managed face of the chart source — a per-seat note generator the
// engine renders into a seat's RIG INPUT, after its capture ring, against the
// song clock (see JuceAudioEngine.ChartSource.cs). The ORDINALS and bit values
// below are the contract (dotcharter_audio.h) and are pinned by AbiV38Tests.

/// <summary>
///     Articulation bits of one <see cref="JuceChartNote" /> (<c>DCA_CHART_NOTE_*</c>).
///     A voice that does not render a bit ignores it. Values are permanent; new bits
///     are appended.
/// </summary>
[Flags]
public enum JuceChartNoteFlags : uint
{
    /// <summary>No articulation: a plain picked note.</summary>
    None = 0,
    /// <summary>Palm mute: short, dark, still pitched.</summary>
    PalmMute = 1u << 0,
    /// <summary>Fret-hand ("full") mute: a percussive, pitchless hit.</summary>
    FretHandMute = 1u << 1,
    /// <summary>Natural harmonic.</summary>
    Harmonic = 1u << 2,
    /// <summary>Pinch (artificial) harmonic.</summary>
    PinchHarmonic = 1u << 3,
    /// <summary>Hammer-on: sounded by the fretting hand, no pick attack.</summary>
    HammerOn = 1u << 4,
    /// <summary>Pull-off: sounded by the fretting hand, no pick attack.</summary>
    PullOff = 1u << 5,
    /// <summary>Tapped note.</summary>
    Tap = 1u << 6,
    /// <summary>Vibrato over the sustain.</summary>
    Vibrato = 1u << 7,
    /// <summary>Tremolo picking: the note is re-picked rapidly for its length.</summary>
    Tremolo = 1u << 8,
    /// <summary>Accented (harder) attack.</summary>
    Accent = 1u << 9,
    /// <summary>The slide target is an UNPITCHED slide (a gesture off the note, not a destination pitch).</summary>
    UnpitchedSlide = 1u << 10,
    /// <summary>Bass slap.</summary>
    Slap = 1u << 11,
    /// <summary>Bass pop.</summary>
    Pop = 1u << 12,
    /// <summary>Bass played with a pick (unset = fingers).</summary>
    Picked = 1u << 13,
    /// <summary>The host held the note past its authored sustain (a let-ring hand shape).</summary>
    LetRing = 1u << 14,
    /// <summary>Tied into the next note on its string.</summary>
    LinkNext = 1u << 15,
    /// <summary>Cut short on purpose.</summary>
    Staccato = 1u << 16,
    /// <summary>A very quiet (ghost) note.</summary>
    Ghost = 1u << 17,
    /// <summary>The arrangement is a bass.</summary>
    Bass = 1u << 18,
    /// <summary>The note's release is the NEXT note on its string — a fretting-hand
    /// damp, so a voice should cut it rather than let it decay.</summary>
    EndDamped = 1u << 19,
    /// <summary>The note rang on to its hand shape's end and is released there.</summary>
    EndShape = 1u << 20,
}

/// <summary>Strum direction of a chord note (<c>DCA_CHART_STRUM_*</c>).</summary>
public enum JuceChartStrum
{
    /// <summary>Not charted; the voice decides.</summary>
    Unknown = 0,
    /// <summary>A down-strum (low string first).</summary>
    Down = 1,
    /// <summary>An up-strum (high string first).</summary>
    Up = 2,
}

/// <summary>A chart source voice (<c>DCA_CHART_VOICE_*</c>).</summary>
public enum JuceChartVoice
{
    /// <summary>A plucked-string PLACEHOLDER (Karplus-Strong), not the real guitar voice.</summary>
    Placeholder = 0,
    /// <summary>The waveguide guitar. Reserved: check <see cref="JuceAudioEngine.IsChartVoiceAvailable" />.</summary>
    Guitar = 1,
    /// <summary>The waveguide bass. Reserved: check <see cref="JuceAudioEngine.IsChartVoiceAvailable" />.</summary>
    Bass = 2,
}

/// <summary>How the chart source meets the seat's live input in its rig (<c>DCA_CHART_SOURCE_*</c>).</summary>
public enum JuceChartSourceMode
{
    /// <summary>Summed with the guitar (the default).</summary>
    Mix = 0,
    /// <summary>The rig hears only the generator; the capture ring still hears the guitar.</summary>
    Replace = 1,
}

/// <summary>
///     One note of a chart source upload, laid out exactly as <c>dca_chart_note</c>
///     (80 bytes). Times are CHART seconds — the song's own timeline, the axis
///     <see cref="JuceAudioSource.AudiblePosition" /> reports. Notes are uploaded
///     sorted by <see cref="StartSeconds" />.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct JuceChartNote
{
    /// <summary>The attack, in chart seconds.</summary>
    public double StartSeconds;

    /// <summary>When the note is released, in chart seconds (at least <see cref="StartSeconds" />).
    /// The host owns ring, legato and hand-shape holds and puts the result here.</summary>
    public double ReleaseSeconds;

    /// <summary>The AUTHORED sustain length in seconds (0 = none); slides are drawn over it.</summary>
    public double SustainSeconds;

    /// <summary>MIDI note number the string sounds at the attack — tuning, capo, cent
    /// offset and the bass octave included; fractional is legal (A4 = 69 = 440 Hz).</summary>
    public float Pitch;

    /// <summary>Attack strength, 0 to 1.</summary>
    public float Velocity;

    /// <summary>String, 0 = the lowest-pitched one.</summary>
    public int StringIndex;

    /// <summary>The fret as charted, counted from the NUT (0 = open; the capo is already in <see cref="Pitch" />).</summary>
    public int Fret;

    /// <summary>The fret a slide ends on, or a negative value for no slide.</summary>
    public float SlideToFret;

    /// <summary>Articulation bits.</summary>
    public JuceChartNoteFlags Flags;

    /// <summary>-1 for a single note; notes that share an id are one chord.</summary>
    public int ChordId;

    /// <summary>Index of this note's first bend point in the bend array, or -1 for none.</summary>
    public int BendFirst;

    /// <summary>How many bend points belong to this note (0 = none).</summary>
    public int BendCount;

    /// <summary>Strum direction of a chord note.</summary>
    public JuceChartStrum Strum;

    /// <summary>The arrangement's capo fret (0 = none). <see cref="Pitch" /> already includes it;
    /// a voice reads it for what is measured from the capo — natural-harmonic nodes sit at
    /// <c>Fret - Capo</c>.</summary>
    public int Capo;

    /// <summary>Reserved; keep zero.</summary>
    public float Reserved1;

    /// <summary>Reserved; keep zero.</summary>
    public float Reserved2;

    /// <summary>Reserved; keep zero.</summary>
    public float Reserved3;
}

/// <summary>
///     One point of a note's bend curve (<c>dca_chart_bend_point</c>, 16 bytes): at
///     <see cref="TimeSeconds" /> (chart seconds, absolute) the string is bent
///     <see cref="Semitones" /> above its fretted pitch. Linear between points, held
///     after the last; a first point ON the attack is a pre-bend.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct JuceChartBendPoint
{
    /// <summary>When, in chart seconds.</summary>
    public double TimeSeconds;

    /// <summary>How far the string is bent, in semitones (1 = a half step).</summary>
    public float Semitones;

    /// <summary>Reserved; keep zero.</summary>
    public int Reserved;
}

/// <summary>
///     A seat's chart source as <see cref="JuceAudioEngine.TryGetChartSourceStatus" />
///     reads it (<c>dca_chart_source_status</c>).
/// </summary>
public readonly record struct JuceChartSourceStatus
{
    /// <summary>Whether the source renders into the seat's rig.</summary>
    public bool IsEnabled { get; init; }

    /// <summary>The selected voice.</summary>
    public JuceChartVoice Voice { get; init; }

    /// <summary>How the generator meets the live input.</summary>
    public JuceChartSourceMode Mode { get; init; }

    /// <summary>Notes in the uploaded table.</summary>
    public int NoteCount { get; init; }

    /// <summary>Notes the voice is sounding now, release tails included.</summary>
    public int ActiveVoices { get; init; }

    /// <summary>Whether a song clock source is bound.</summary>
    public bool IsClockBound { get; init; }

    /// <summary>Whether the song clock advanced in the last rendered block.</summary>
    public bool IsClockPlaying { get; init; }

    /// <summary>Note-ons since the table was uploaded, chased ones included.</summary>
    public long NotesStarted { get; init; }

    /// <summary>Of <see cref="NotesStarted" />, the notes started mid-note after a seek or resume.</summary>
    public long NotesChased { get; init; }

    /// <summary>Note-ons the per-block cap or the note pool refused.</summary>
    public long NotesDropped { get; init; }

    /// <summary>Song time at the end of the last rendered block, in seconds.</summary>
    public double SongSeconds { get; init; }

    /// <summary>Linear gain into the rig.</summary>
    public float Gain { get; init; }

    /// <summary>Seconds added to every note time (positive = later).</summary>
    public double OffsetSeconds { get; init; }

    /// <summary>ABI v40: the instrument selection of the selected voice's family
    /// (the guitar's for <see cref="JuceChartVoice.Guitar" />, the bass's for
    /// <see cref="JuceChartVoice.Bass" />).</summary>
    public JuceChartInstrument Instrument { get; init; }

    /// <summary>ABI v40: the voice plays real recordings (a sample pack is loaded),
    /// false for the waveguide fallback or when no voice is loaded.</summary>
    public bool IsSampled { get; init; }

    /// <summary>ABI v40: a voice exists. False while the source is off and its voice
    /// has been released (#61): a disabled source holds no sample memory.</summary>
    public bool IsVoiceLoaded { get; init; }

    /// <summary>ABI v40: bytes of sample pack the seat's voice holds (0 once released).</summary>
    public long PackBytes { get; init; }
}

/// <summary>
///     ABI v40 (<c>DCA_CHART_MODEL_*</c>, DotCharter #50): which guitar or bass the
///     built-in voices sound like. Values below 16 are guitars (they set the
///     <see cref="JuceChartVoice.Guitar" /> voice), 16 and up basses. The Strat's
///     five positions are real recordings; Telecaster, Les Paul and the HSS bridge
///     humbucker are the closest Strat position CONVERTED (pickup-position comb,
///     pickup resonance, body EQ) — <see cref="JuceChartModelInfo.RecordedPickups" />
///     says which is which.
/// </summary>
public enum JuceChartInstrumentModel
{
    /// <summary>Gretsch Anniversary semi-hollow; recorded on its bridge pickup (the v38/39 guitar).</summary>
    SemiHollow = 0,

    /// <summary>Stratocaster, three single coils, 5-way.</summary>
    StratSss = 1,

    /// <summary>Stratocaster with a bridge humbucker (HSS), 5-way, coil split.</summary>
    StratHss = 2,

    /// <summary>Telecaster, 3-way.</summary>
    Telecaster = 3,

    /// <summary>Les Paul, two humbuckers, 3-way, coil split.</summary>
    LesPaul = 4,

    /// <summary>Jazz bass: neck, both, bridge.</summary>
    JazzBass = 16,

    /// <summary>Precision bass: one split-coil pickup (<see cref="JuceChartPickup.Middle" />).</summary>
    PrecisionBass = 17,

    /// <summary>PJ bass: P (<see cref="JuceChartPickup.Middle" />), P + J, J (<see cref="JuceChartPickup.Bridge" />).</summary>
    PjBass = 18,

    /// <summary>Washburn AB-95 hollowbody bass, neck pickup (recorded).</summary>
    HollowBass = 19,
}

/// <summary>ABI v40 pickup-selector positions (<c>DCA_CHART_PICKUP_*</c>). A model
/// offers the subset its <see cref="JuceChartModelInfo.Pickups" /> lists.</summary>
public enum JuceChartPickup
{
    /// <summary>The neck pickup.</summary>
    Neck = 0,

    /// <summary>Neck and middle together.</summary>
    NeckMiddle = 1,

    /// <summary>The middle pickup (a P bass's split coil).</summary>
    Middle = 2,

    /// <summary>Middle and bridge together.</summary>
    MiddleBridge = 3,

    /// <summary>The bridge pickup.</summary>
    Bridge = 4,

    /// <summary>Neck and bridge together (the middle of a 3-way).</summary>
    NeckBridge = 5,
}

/// <summary>An instrument selection: model, pickup selector position and coil split.</summary>
/// <param name="Model">The instrument model.</param>
/// <param name="Pickup">The selector position; must be one the model offers.</param>
/// <param name="CoilSplit">Humbuckers as one coil; no effect on a model without one.</param>
public readonly record struct JuceChartInstrument(JuceChartInstrumentModel Model, JuceChartPickup Pickup, bool CoilSplit)
{
    /// <summary>True for a bass model.</summary>
    public bool IsBass => (int)Model >= 16;
}

/// <summary>What <see cref="JuceAudioEngine.GetChartModelInfo" /> reports about a model
/// (<c>dca_chart_model_info</c>).</summary>
public sealed record JuceChartModelInfo
{
    /// <summary>Which instrument model this describes (the value asked about).</summary>
    public required JuceChartInstrumentModel Model { get; init; }

    /// <summary>A bass model.</summary>
    public required bool IsBass { get; init; }

    /// <summary>The selector positions, in selector order (neck first).</summary>
    public required IReadOnlyList<JuceChartPickup> Pickups { get; init; }

    /// <summary>The position a fresh selection of this model uses.</summary>
    public required JuceChartPickup DefaultPickup { get; init; }

    /// <summary>The coil-split switch changes something (the model has a humbucker).</summary>
    public required bool HasCoilSplit { get; init; }

    /// <summary>Every sample pack its positions need is installed beside the native
    /// library. False = it plays the fallback (another pack converted, or the
    /// waveguide string model).</summary>
    public required bool IsInstalled { get; init; }

    /// <summary>Positions (coil split off) whose recordings play UNCONVERTED.</summary>
    public required IReadOnlyList<JuceChartPickup> RecordedPickups { get; init; }

    /// <summary>Positions where at least one articulation set is recorded as is
    /// (bass: the fingers and the pick come from different basses).</summary>
    public required IReadOnlyList<JuceChartPickup> PartlyRecordedPickups { get; init; }
}
