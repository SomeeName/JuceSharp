namespace JuceSharp;

// Typed mirrors of the small integer enums this ABI passes as bare int32. The
// ORDINALS are the contract (dotcharter_audio.h), never the names — every one of
// these is pinned against the header by NativeMethodsTests, the same way
// DcaDriver_OrdinalsMatchManagedEnum pins the driver enum. Append, never
// renumber.
//
// The int overloads on JuceAudioEngine stay: callers that persist the ordinal
// (DotCharter's EQ section and its metronome both do) must keep compiling.

/// <summary>
///     The drop-tune pedal's splice-lag CEILING (<c>dca_engine_set_input_pitch_shift_mode</c>).
///     <para>
///         Not a shifter choice. The shifter is CSVD
///         (<c>src/dsp/cores/dcdsp_droptune.h</c>) and is the same in all three;
///         these pick how long a splice may wait for a good correlation point.
///         Attack latency (~1.5–4 ms) is set by the transient anchor and does not
///         change between them. Before ABI v25 the same argument picked an ENGINE
///         (0 = Rubber Band, 1 = OLA) — both are gone.
///     </para>
/// </summary>
public enum JuceDropTuneMode
{
    /// <summary>10 ms lag ceiling: the most immediate, the most splices.</summary>
    Tight = 0,
    /// <summary>20 ms lag ceiling. The engine default.</summary>
    Balanced = 1,
    /// <summary>35 ms lag ceiling: fewest splices, best on sustained chords.</summary>
    Smooth = 2,
}

/// <summary>Shape of one parametric-EQ band (<c>dca_eq_band_type</c>, ABI v29).
/// <see cref="HighPass" />, <see cref="LowPass" /> and <see cref="Notch" /> ignore
/// the band's gain.</summary>
public enum JuceEqBandType
{
    /// <summary>Second-order high-pass: everything below the corner is rolled off.</summary>
    HighPass  = 0,
    /// <summary>Low shelf: the band's gain is applied to everything below the corner.</summary>
    LowShelf  = 1,
    /// <summary>Peaking bell centred on the band frequency, width set by its Q.</summary>
    Bell      = 2,
    /// <summary>High shelf: the band's gain is applied to everything above the corner.</summary>
    HighShelf = 3,
    /// <summary>Second-order low-pass: everything above the corner is rolled off.</summary>
    LowPass   = 4,
    /// <summary>Band-reject notch that cuts a narrow slice out at the band frequency.</summary>
    Notch     = 5,
}

/// <summary>Which bus the six EQ bands run on (<c>dca_eq_route</c>, ABI v29).</summary>
public enum JuceEqRoute
{
    /// <summary>The mixed playback output only. The engine default.</summary>
    Song  = 0,
    /// <summary>Every seat's post-rig guitar signal only.</summary>
    Input = 1,
    /// <summary>The playback mix and every seat's post-rig guitar signal alike.</summary>
    Both  = 2,
}

/// <summary>Which synthesised click <c>dca_engine_play_click</c> /
/// <c>dca_engine_schedule_click</c> sounds.</summary>
public enum JuceClickType
{
    /// <summary>The ordinary off-beat click.</summary>
    Beat     = 0,
    /// <summary>The accented bar-one click, pitched above <see cref="Beat"/>.</summary>
    Downbeat = 1,
    /// <summary>A noise-burst hand-clap, for count-ins that should not read as a metronome.</summary>
    Clap     = 2,
}

/// <summary>
///     What <see cref="JuceAudioEngine.ConvertAudioForDevice" /> wrote
///     (<c>dca_convert_audio_for_device</c>'s <c>out_bits</c>, ABI v43). The value
///     IS the bit depth of the written WAV.
/// </summary>
/// <remarks>The written file is always 16-bit: the preload's own int16 samples,
/// with its step in a <c>dcps</c> chunk the engine honours on every open, so even
/// a song whose conversion overshot full scale is bit-exact at 16 bits.</remarks>
public enum JuceDeviceRateConversion
{
    /// <summary>Nothing was written: the engine would not convert this file at
    /// that device rate (it is already at or above it), so open the original.</summary>
    NotNeeded = 0,
    /// <summary>A 16-bit PCM WAV at the device rate, plus the preload step in a
    /// <c>dcps</c> chunk.</summary>
    Pcm16 = 16,
}

/// <summary>
///     A fingerprint of the samples a preloaded source plays
///     (<c>dca_source_get_preload_digest</c>, ABI v43). Two sources with equal
///     digests hold the same 16-bit samples at the same step, frame count and
///     rate: they play bit-identical audio with identical alignment.
/// </summary>
/// <param name="Hash">FNV-1a 64 over the held samples (see the header).</param>
/// <param name="Step">The preload's step: a played sample is int16 x step.</param>
/// <param name="Frames">Frames held.</param>
/// <param name="SampleRate">The rate the held buffer is at.</param>
/// <param name="ConvertedAtLoad">Whether this open rate-converted at load.</param>
public readonly record struct JucePreloadDigest(ulong Hash, float Step, long Frames, double SampleRate, bool ConvertedAtLoad);
