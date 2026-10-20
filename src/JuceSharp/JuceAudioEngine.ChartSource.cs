using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using JuceSharp.Interop;

namespace JuceSharp;

/// <content>
///     ABI v38: the CHART SOURCE — a per-seat note generator the engine renders
///     into a seat's RIG INPUT (after its capture ring, so detection, scoring and
///     the tuner never hear it) against the SONG CLOCK, plus the instrument
///     ROUTE. All real-time work is native: the notes are uploaded once and the
///     engine schedules them on the audio thread. Every call pins the engine
///     handle for its duration. Nothing here survives an engine REBUILD — the
///     host replays it.
/// </content>
public sealed partial class JuceAudioEngine
{
    /// <summary>
    ///     Binds the song clock every seat's chart source follows: the audible
    ///     position of <paramref name="source" />, read on the audio thread after
    ///     every pull, so the notes follow play, pause, seeks, loops and practice
    ///     speed by construction. <c>null</c> unbinds (every chart source then
    ///     releases its notes). A disposed source unbinds itself
    ///     (<c>dca_engine_set_song_clock_source</c>). Control thread.
    /// </summary>
    /// <exception cref="ArgumentException"><paramref name="source" /> is not a source
    /// this engine opened.</exception>
    /// <exception cref="JuceAudioException">The engine refused the source.</exception>
    public void SetSongClockSource(JuceAudioSource? source)
    {
        var h = Acquire();
        try
        {
            if (source == null)
            {
                Check(NativeMethods.EngineSetSongClockSource(h, nint.Zero), "dca_engine_set_song_clock_source");
                return;
            }
            var s = source.AcquireForEngineCall();
            try { Check(NativeMethods.EngineSetSongClockSource(h, s), "dca_engine_set_song_clock_source"); }
            finally { source.ReleaseForEngineCall(); }
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Uploads <paramref name="player" />'s notes, replacing the previous table;
    ///     an empty span clears it. The notes must be sorted by start, every time
    ///     finite and non-negative, release at or after start, pitch in [0, 127];
    ///     every bend range must lie inside <paramref name="bends" />. A list that
    ///     breaks a rule is rejected WHOLE and the previous table stays. Both spans
    ///     are copied (<c>dca_engine_chart_source_set_notes_for_player2</c>, which is
    ///     also told the byte length of the notes, ABI v48).
    ///     Control thread; validating a long chart is a copy, never audio-thread work.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    /// <exception cref="JuceAudioException">The engine rejected the list (the native
    /// log line says which rule).</exception>
    public void SetChartNotes(int player, ReadOnlySpan<JuceChartNote> notes, ReadOnlySpan<JuceChartBendPoint> bends)
    {
        ValidatePlayer(player, nameof(player));
        var h = Acquire();
        try
        {
            var nativeNotes = MemoryMarshal.Cast<JuceChartNote, NativeMethods.DcaChartNote>(notes);
            var nativeBends = MemoryMarshal.Cast<JuceChartBendPoint, NativeMethods.DcaChartBendPoint>(bends);
            unsafe
            {
                fixed (NativeMethods.DcaChartNote* pn = nativeNotes)
                fixed (NativeMethods.DcaChartBendPoint* pb = nativeBends)
                {
                    var stride = Unsafe.SizeOf<NativeMethods.DcaChartNote>();
                    Check(NativeMethods.EngineChartSourceSetNotesForPlayer2(h, player, pn, nativeNotes.Length,
                            stride, (long)nativeNotes.Length * stride, pb, nativeBends.Length),
                        "dca_engine_chart_source_set_notes_for_player2");
                }
            }
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Switches <paramref name="player" />'s chart source on or off (off at engine
    ///     start). Switching ON builds the selected voice first — its sample pack loads
    ///     ON THIS THREAD (tens to a few hundred ms for a real pack; call it off the UI
    ///     thread). Switching off chokes every note through the voice's fast fade and,
    ///     since ABI v40, then frees the voice and its pack natively, off the audio
    ///     thread (<c>dca_engine_chart_source_set_enabled_for_player</c>). Control thread.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    /// <exception cref="JuceAudioException">The engine could not build the voice.</exception>
    public void SetChartSourceEnabled(int player, bool enabled)
    {
        ValidatePlayer(player, nameof(player));
        var h = Acquire();
        try
        {
            Check(NativeMethods.EngineChartSourceSetEnabledForPlayer(h, player, enabled ? 1 : 0),
                "dca_engine_chart_source_set_enabled_for_player");
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Selects <paramref name="player" />'s voice; the placeholder at engine start.
    ///     While the source is on, the new voice is built here (loading its pack) and
    ///     cross-faded in; while it is off (ABI v40) the choice is only recorded and
    ///     the next enable builds it. Returns false — and leaves the current voice — when this build does not
    ///     contain <paramref name="voice" /> or the engine is gone
    ///     (<c>dca_engine_chart_source_set_voice_for_player</c>). Control thread.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public bool TrySetChartVoice(int player, JuceChartVoice voice)
    {
        ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try { return NativeMethods.EngineChartSourceSetVoiceForPlayer(h, player, (int)voice) == NativeMethods.DcaStatus.Ok; }
        finally { Release(); }
    }

    /// <summary>
    ///     Whether this build of the native engine contains <paramref name="voice" />
    ///     (<c>dca_chart_voice_is_available</c>). A voice it does not contain must be
    ///     shown unavailable, never offered.
    /// </summary>
    public static bool IsChartVoiceAvailable(JuceChartVoice voice) => NativeMethods.ChartVoiceIsAvailable((int)voice) == 1;

    /// <summary>
    ///     Sets <paramref name="player" />'s generator level into the rig
    ///     (<paramref name="gainLinear" /> in [0, 4]; 1 at engine start), the time
    ///     added to every note (<paramref name="offsetSeconds" /> in [-2, 2];
    ///     positive = later) and how it meets the live input
    ///     (<c>dca_engine_chart_source_set_params_for_player</c>). Out-of-range values
    ///     are REJECTED, never clamped. Control thread.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException">A value is outside its range, or
    /// <paramref name="player" /> is not a seat this engine has.</exception>
    public void SetChartSourceParams(int player, float gainLinear, double offsetSeconds, JuceChartSourceMode mode)
    {
        ValidatePlayer(player, nameof(player));
        if (!float.IsFinite(gainLinear) || gainLinear < 0f || gainLinear > 4f)
            throw new ArgumentOutOfRangeException(nameof(gainLinear), gainLinear, "Gain must be in [0, 4].");
        if (!double.IsFinite(offsetSeconds) || offsetSeconds < -2.0 || offsetSeconds > 2.0)
            throw new ArgumentOutOfRangeException(nameof(offsetSeconds), offsetSeconds, "Offset must be in [-2, 2] s.");
        if (mode is not (JuceChartSourceMode.Mix or JuceChartSourceMode.Replace))
            throw new ArgumentOutOfRangeException(nameof(mode), mode, "Unknown chart source mode.");
        var h = Acquire();
        try
        {
            Check(NativeMethods.EngineChartSourceSetParamsForPlayer(h, player, gainLinear, offsetSeconds, (int)mode),
                "dca_engine_chart_source_set_params_for_player");
        }
        finally { Release(); }
    }

    /// <summary>
    ///     A snapshot of <paramref name="player" />'s chart source. False (and a
    ///     default status) when the engine is gone
    ///     (<c>dca_engine_chart_source_get_status_for_player</c>). Any non-audio
    ///     thread; cheap enough to poll per UI frame.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public bool TryGetChartSourceStatus(int player, out JuceChartSourceStatus status)
    {
        ValidatePlayer(player, nameof(player));
        status = default;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            var raw = new NativeMethods.DcaChartSourceStatus
            {
                StructBytes = Unsafe.SizeOf<NativeMethods.DcaChartSourceStatus>(),
            };
            if (NativeMethods.EngineChartSourceGetStatusForPlayer(h, player, ref raw) != NativeMethods.DcaStatus.Ok)
                return false;
            status = new JuceChartSourceStatus
            {
                IsEnabled = raw.Enabled != 0,
                Voice = (JuceChartVoice)raw.Voice,
                Mode = (JuceChartSourceMode)raw.Mode,
                NoteCount = raw.NoteCount,
                ActiveVoices = raw.ActiveVoices,
                IsClockBound = raw.ClockBound != 0,
                IsClockPlaying = raw.ClockPlaying != 0,
                NotesStarted = raw.NotesStarted,
                NotesChased = raw.NotesChased,
                NotesDropped = raw.NotesDropped,
                SongSeconds = raw.SongSeconds,
                Gain = raw.Gain,
                OffsetSeconds = raw.OffsetSeconds,
                Instrument = new JuceChartInstrument((JuceChartInstrumentModel)raw.Model, (JuceChartPickup)raw.Pickup,
                    (raw.InstrumentFlags & NativeMethods.ChartInstrumentCoilSplit) != 0),
                IsSampled = raw.Sampled != 0,
                IsVoiceLoaded = raw.VoiceLoaded != 0,
                PackBytes = raw.PackBytes,
            };
            return true;
        }
        finally { Release(); }
    }

    /// <summary>
    ///     ABI v40 (#50): selects the instrument model, pickup and coil split of the
    ///     voice of <paramref name="instrument" />'s family on <paramref name="player" />
    ///     — a guitar model sets the <see cref="JuceChartVoice.Guitar" /> voice, a bass
    ///     model the <see cref="JuceChartVoice.Bass" /> voice; both are kept. While that
    ///     voice is playing, a new one is built ON THIS THREAD (its sample pack loads
    ///     here — call it off the UI thread) and cross-faded in; the replaced voice and
    ///     its pack are freed natively, off the audio thread. Returns false — and changes
    ///     nothing — when the pickup is not on the model's selector or the engine is
    ///     gone (<c>dca_engine_chart_source_set_instrument_for_player</c>). Control thread.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public bool TrySetChartInstrument(int player, JuceChartInstrument instrument)
    {
        ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            return NativeMethods.EngineChartSourceSetInstrumentForPlayer(h, player, (int)instrument.Model, (int)instrument.Pickup,
                instrument.CoilSplit ? NativeMethods.ChartInstrumentCoilSplit : 0u) == NativeMethods.DcaStatus.Ok;
        }
        finally { Release(); }
    }

    /// <summary>
    ///     ABI v40: the guitar (<paramref name="bass" /> false) or bass selection of
    ///     <paramref name="player" />, or null when the engine is gone
    ///     (<c>dca_engine_chart_source_get_instrument_for_player</c>).
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public JuceChartInstrument? GetChartInstrument(int player, bool bass)
    {
        ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return null;
        try
        {
            if (NativeMethods.EngineChartSourceGetInstrumentForPlayer(h, player, bass ? 1 : 0, out var model, out var pickup,
                    out var flags) != NativeMethods.DcaStatus.Ok)
                return null;
            return new JuceChartInstrument((JuceChartInstrumentModel)model, (JuceChartPickup)pickup,
                (flags & NativeMethods.ChartInstrumentCoilSplit) != 0);
        }
        finally { Release(); }
    }

    /// <summary>
    ///     ABI v40: what <paramref name="model" /> offers — its pickups, its default,
    ///     coil split, whether its packs are installed and which positions are real
    ///     recordings (<c>dca_chart_model_get_info</c>). Null for a model this build
    ///     does not know. Reads the voice directory (file I/O): not per frame.
    /// </summary>
    public static JuceChartModelInfo? GetChartModelInfo(JuceChartInstrumentModel model)
    {
        var raw = new NativeMethods.DcaChartModelInfo { StructBytes = Unsafe.SizeOf<NativeMethods.DcaChartModelInfo>() };
        if (NativeMethods.ChartModelGetInfo((int)model, ref raw) != NativeMethods.DcaStatus.Ok) return null;
        return new JuceChartModelInfo
        {
            Model = model,
            IsBass = raw.Bass != 0,
            Pickups = PickupsIn(raw.PickupMask),
            DefaultPickup = (JuceChartPickup)raw.DefaultPickup,
            HasCoilSplit = raw.HasCoilSplit != 0,
            IsInstalled = raw.Installed != 0,
            RecordedPickups = PickupsIn(raw.RecordedMask),
            PartlyRecordedPickups = PickupsIn(raw.RecordedAnyMask),
        };
    }

    /// <summary>
    ///     ABI v40 (#61): bytes of sample-pack memory alive in this process — every
    ///     engine's chart voices, a shared pack counted once
    ///     (<c>dca_chart_voice_pack_bytes</c>). 0 once every chart source is off and
    ///     its voice released.
    /// </summary>
    public static long ChartVoicePackBytes => NativeMethods.ChartVoicePackBytes();

    /// <summary>The selector order a guitarist reads: neck first, the blends between.</summary>
    private static readonly JuceChartPickup[] SelectorOrder =
    [
        JuceChartPickup.Neck, JuceChartPickup.NeckMiddle, JuceChartPickup.NeckBridge, JuceChartPickup.Middle,
        JuceChartPickup.MiddleBridge, JuceChartPickup.Bridge,
    ];

    private static JuceChartPickup[] PickupsIn(uint mask) =>
        SelectorOrder.Where(p => (mask & (1u << (int)p)) != 0).ToArray();

    /// <summary>
    ///     Routes the IN-PROCESS instrument (<see cref="JuceAudioPlugin.AttachToInstrument" />)
    ///     into <paramref name="player" />'s rig input — summed to mono, after the
    ///     capture ring, and no longer on the master — or back onto the master with
    ///     -1, the engine-start value (<c>dca_engine_set_instrument_route</c>). The
    ///     SANDBOXED instrument cannot follow (its block returns from the child after
    ///     the rig has run) and stays on the master whatever this says. Control thread.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// neither -1 nor a seat this engine has.</exception>
    public void SetInstrumentRoute(int player)
    {
        if (player != -1) ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetInstrumentRoute(h, player), "engine_set_instrument_route"); }
        finally { Release(); }
    }

    /// <summary>
    ///     The seat the in-process instrument is routed into, or -1 for the master
    ///     (also on a disposed engine) (<c>dca_engine_get_instrument_route</c>).
    /// </summary>
    public int GetInstrumentRoute()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return -1;
        try { return NativeMethods.EngineGetInstrumentRoute(h, out var p) == NativeMethods.DcaStatus.Ok ? p : -1; }
        finally { Release(); }
    }
}
