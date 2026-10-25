using System.Runtime.InteropServices;
using System.Text;

namespace JuceSharp.Interop;

/// <summary>
/// P/Invoke surface for <c>juce_sharp.dll</c> (see <see cref="LibName"/> — the
/// header is <c>dotcharter_audio.h</c>, the LIBRARY has never been called that).
/// Mirrors <c>include/dotcharter_audio.h</c> exactly. Do not call these directly
/// from product code — use the high-level <see cref="JuceAudioEngine"/>
/// and friends.
/// </summary>
internal static class NativeMethods
{
    internal const string LibName = "juce_sharp";
    /// <summary>
    ///     Must equal <c>DCA_ABI_VERSION</c> in <c>include/dotcharter_audio.h</c>
    ///     — <see cref="JuceAudioEngine" /> compares them for EQUALITY at open, so
    ///     a header bump the wrapper does not follow refuses every engine.
    ///
    ///     <para>
    ///         v30 is additive (per-node meters, chain latency/PDC, the xrun
    ///         counters); the managed bindings for those live on the ABI surface
    ///         only when something calls them. Bumping this number is what makes
    ///         the rebuilt DLL loadable, not a claim that they are bound.
    ///     </para>
    ///     <para>
    ///         v33 adds five introspection exports: the rig's node type and
    ///         bypass getters, the catalogue category, and the two
    ///         multi-component VST3 entry points.
    ///     </para>
    ///     <para>
    ///         v34 adds the rig's edit scopes (begin / commit / abort a whole
    ///         preset as ONE publication), branch delay compensation at the
    ///         merger, and the per-node sidechain bus plus the module-type flags
    ///         export that advertises which modules accept one.
    ///     </para>
    ///     <para>
    ///         v36 adds two exports and one status code, all additive:
    ///         <c>dca_stems_separate_file2</c> (a progress callback that can
    ///         CANCEL, which the v35 one could not because it returns void),
    ///         <c>dca_midi_output_get_counts3</c> (the driver-threw counter,
    ///         which existed natively since v35 with no way to read it), and
    ///         <see cref="DcaStatus.Cancelled" />. Every v35 entry point keeps
    ///         its exact signature and behaviour.
    ///     </para>
    ///     <para>
    ///         v38 adds the CHART SOURCE (a per-seat note generator rendered into
    ///         the seat's rig input, after its capture ring), the SONG CLOCK it
    ///         follows, and the instrument ROUTE — nine additive exports, three
    ///         structs, everything off at engine start.
    ///     </para>
    ///     <para>
    ///         v39 puts the arrangement's <c>capo</c> into <c>dca_chart_note</c> (the
    ///         first reserved slot; still 80 bytes): harmonic nodes are measured from
    ///         the capo, which <c>pitch - fret</c> cannot recover.
    ///     </para>
    ///     <para>
    ///         v40 adds INSTRUMENT MODELS for the built-in voices (model, pickup,
    ///         coil split; DotCharter #50) and releases a disabled source's voice and
    ///         sample pack (#61): four additive exports and six fields appended to
    ///         <c>dca_chart_source_status</c> (cbSize, so a v38 struct still works).
    ///     </para>
    ///     <para>
    ///         v41 adds <c>dca_engine_create2</c> (DotCharter #85): the headless Null
    ///         device is requested PER ENGINE through create flags
    ///         (<see cref="EngineCreateNullDevice" />, <see cref="EngineCreateNullDeviceManual" />)
    ///         instead of only through the process-wide <c>DCA_ENABLE_NULL_DEVICE</c>
    ///         env var, which leaked into a concurrently built live engine. The env
    ///         vars still work and are OR-ed with the flags.
    ///     </para>
    ///     <para>
    ///         v42 adds native Wwise-Vorbis (.wem) decoding (DotCharter #426): the
    ///         seven <c>dca_wem_*</c> exports behind <see cref="JuceWemDecoder" />, and
    ///         every engine format manager now opens a <c>.wem</c> as a source.
    ///     </para>
    ///     <para>
    ///         v43 does the load-time rate conversion once, into a file (DotCharter
    ///         #435): <c>dca_convert_audio_for_device</c> behind
    ///         <see cref="JuceAudioEngine.ConvertAudioForDevice" />, and the
    ///         <c>dca_source_get_preload_digest</c> test seam that proves an open of
    ///         the converted file plays bit-identical samples.
    ///     </para>
    ///     <para>
    ///         v45 loads the <c>rsamp</c> chain table at RUN TIME (JuceSharp #25):
    ///         <c>dca_rsamp_set_table</c> and <c>dca_rsamp_table_row_count</c>. The
    ///         numbers are Rocksmith's, so the library ships none of them and the
    ///         host builds them from the user's own install
    ///         (<see cref="Dsp.RsAmpChainTable" />, <see cref="Dsp.JuceRsAmpTable" />).
    ///     </para>
    ///     <para>
    ///         v46 exports the audio callback's LOAD (JuceSharp #29):
    ///         <c>dca_engine_get_callback_load</c> behind
    ///         <see cref="JuceAudioEngine.TryGetCallbackLoad" />, so a host can show a
    ///         real CPU meter.
    ///     </para>
    ///     <para>
    ///         v47 adds the EXTERNAL PROCESS INSERT: a seat's dry input exchanged
    ///         with another process through a named shared-memory ring
    ///         (<c>dca_engine_extproc_open</c> / <c>_close</c> / <c>_get_stats</c>, each
    ///         with a <c>_for_player</c> twin), behind
    ///         <see cref="JuceAudioEngine.OpenExternalProcessInsert" />, and two
    ///         appended status codes, <see cref="DcaStatus.SampleRate" /> and
    ///         <see cref="DcaStatus.NotFound" />.
    ///     </para>
    ///     <para>
    ///         v49 exports the open device's channel names:
    ///         <c>dca_engine_get_channel_names</c> behind
    ///         <see cref="JuceAudioEngine.GetChannelNames" />, so a host can label an
    ///         output pair or an input jack the way the driver does. A one-output
    ///         device also folds the stereo bus into its channel natively.
    ///     </para>
    /// </summary>
    internal const uint   ExpectedAbiVersion = 49;

    /// <summary>Mirrors <c>DCA_MAX_PLAYERS</c>: how many independent live-input
    /// rigs (seats) the native engine hosts. Each has its own DSP chain, VST3
    /// input chain, capture ring and hardware input channel.</summary>
    internal const int MaxPlayers = 4;

    /// <summary>Mirrors <c>DCA_PLAYER_NODE_ID_STRIDE</c>. DSP node ids are unique
    /// per ENGINE: seat <c>p</c> hands out <c>p * stride + local</c>, which is why
    /// every node-scoped entry point below takes no player argument. Seat 0's ids
    /// are 0, 1, 2, … exactly as before ABI v28.</summary>
    internal const int PlayerNodeIdStride = 1_000_000;

    /// <summary>Mirrors the <c>event_count</c> cap the header documents on
    /// <c>dca_engine_dsp_process_buffer_automated*</c> (262,144). The cap is
    /// enforced NATIVE-side, after the whole list has been copied into an
    /// unmanaged block — so the wrapper has to know it too, or a caller 10×
    /// past it pays a full O(n) marshal and a multi-hundred-megabyte
    /// AllocHGlobal before being told "no".</summary>
    internal const int MaxAutomationEvents = 262_144;

    internal enum DcaStatus
    {
        Ok                = 0,
        InvalidArg        = 1,
        NotInitialized    = 2,
        Device            = 3,
        Decode            = 4,
        Plugin            = 5,
        /// <summary>v36. The caller ASKED to stop. Not a failure — the wrapper
        /// turns this into <see cref="System.OperationCanceledException" />,
        /// never a <see cref="JuceAudioException" />, because a user's Cancel
        /// must not read to a consumer as a reason to fall through to another
        /// backend.</summary>
        Cancelled         = 6,
        /// <summary>v47. The device does not run at the rate the other process's
        /// ring carries; nothing was opened.</summary>
        SampleRate        = 7,
        /// <summary>v47. The named object (a shared-memory block) does not exist.</summary>
        NotFound          = 8,
        Internal          = 99,
    }

    internal enum DcaDriver
    {
        Default     = 0,
        Wasapi      = 1,
        DirectSound = 2,
        Asio        = 3,
        CoreAudio   = 4,
        Alsa        = 5,
        Jack        = 6,
    }

    internal enum DcaLogLevel
    {
        Trace = 0,
        Debug = 1,
        Info  = 2,
        Warn  = 3,
        Error = 4,
    }

    // --- UTF-8 helpers for the fixed char[] fields of the two by-value structs.
    //
    // The native side treats every string that crosses this boundary as UTF-8
    // (copyTruncated -> juce::String::toRawUTF8 on the way out,
    // juce::String::fromUTF8 on the way in). These structs used to be marshalled
    // CharSet.Ansi/ByValTStr — i.e. the SYSTEM CODEPAGE — so any non-ASCII device
    // name (localized Windows endpoints, branded interfaces on a de-DE/JP box)
    // read back as mojibake AND re-encoded to a non-matching byte sequence on
    // write, silently failing device selection into the fallback walk
    // (2026-07-16 audit finding #3). The structs are now blittable with
    // `fixed byte` buffers and explicit UTF-8 encode/decode; the byte layout is
    // unchanged (128/256/64-byte char arrays, 4-byte ints), so no ABI bump.

    internal static unsafe string Utf8FromFixed(byte* buf, int capacity)
    {
        var span = new ReadOnlySpan<byte>(buf, capacity);
        var nul = span.IndexOf((byte)0);
        return Encoding.UTF8.GetString(nul < 0 ? span : span[..nul]);
    }

    internal static unsafe void Utf8ToFixed(string? value, byte* buf, int capacity)
    {
        new Span<byte>(buf, capacity).Clear();
        if (string.IsNullOrEmpty(value)) return;
        Span<byte> dest = new(buf, capacity - 1); // always NUL-terminated
        if (Encoding.UTF8.GetByteCount(value) <= dest.Length)
        {
            Encoding.UTF8.GetBytes(value, dest);
            return;
        }
        // Truncate WITHOUT splitting a multi-byte sequence: a torn trailing
        // sequence would decode as U+FFFD on the native side.
        var bytes = Encoding.UTF8.GetBytes(value);
        var n = dest.Length;
        while (n > 0 && (bytes[n] & 0xC0) == 0x80) n--; // don't start mid-continuation
        bytes.AsSpan(0, n).CopyTo(dest);
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct DcaDeviceInfo
    {
        internal fixed byte IdUtf8[128];
        internal fixed byte NameUtf8[256];
        public DcaDriver Driver;
        public int       MaxInputChannels;
        public int       MaxOutputChannels;
        public int       DefaultSampleRate;
        public int       IsDefault;
        // ABI v7: AudioIODeviceType::getTypeName() — disambiguates WASAPI
        // Shared / Exclusive / Low-Latency, which all map to DcaDriver.Wasapi.
        internal fixed byte TypeNameUtf8[64];

        public string Id
        {
            get { fixed (byte* p = IdUtf8) return Utf8FromFixed(p, 128); }
            set { fixed (byte* p = IdUtf8) Utf8ToFixed(value, p, 128); }
        }
        public string Name
        {
            get { fixed (byte* p = NameUtf8) return Utf8FromFixed(p, 256); }
            set { fixed (byte* p = NameUtf8) Utf8ToFixed(value, p, 256); }
        }
        public string TypeName
        {
            get { fixed (byte* p = TypeNameUtf8) return Utf8FromFixed(p, 64); }
            set { fixed (byte* p = TypeNameUtf8) Utf8ToFixed(value, p, 64); }
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct DcaEngineConfig
    {
        public int       SampleRate;
        public int       BufferFrames;
        public int       OutputChannels;
        public int       InputChannels;
        public uint      OutputChannelMask;
        public uint      InputChannelMask;
        public DcaDriver PreferredDriver;
        internal fixed byte OutputDeviceIdUtf8[128];
        internal fixed byte InputDeviceIdUtf8[128];
        // ABI v7: explicit device-type name override (e.g.
        // "Windows Audio (Exclusive Mode)"). When non-empty wins over
        // PreferredDriver; required to pick a specific WASAPI mode.
        internal fixed byte OutputTypeNameUtf8[64];
        internal fixed byte InputTypeNameUtf8[64];

        public string OutputDeviceId
        {
            get { fixed (byte* p = OutputDeviceIdUtf8) return Utf8FromFixed(p, 128); }
            set { fixed (byte* p = OutputDeviceIdUtf8) Utf8ToFixed(value, p, 128); }
        }
        public string InputDeviceId
        {
            get { fixed (byte* p = InputDeviceIdUtf8) return Utf8FromFixed(p, 128); }
            set { fixed (byte* p = InputDeviceIdUtf8) Utf8ToFixed(value, p, 128); }
        }
        public string OutputTypeName
        {
            get { fixed (byte* p = OutputTypeNameUtf8) return Utf8FromFixed(p, 64); }
            set { fixed (byte* p = OutputTypeNameUtf8) Utf8ToFixed(value, p, 64); }
        }
        public string InputTypeName
        {
            get { fixed (byte* p = InputTypeNameUtf8) return Utf8FromFixed(p, 64); }
            set { fixed (byte* p = InputTypeNameUtf8) Utf8ToFixed(value, p, 64); }
        }
    }

    // Native log messages come from juce::String::toRawUTF8 / ASCII literals —
    // decode them as UTF-8, not the system codepage.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DcaLogCallback(nint user, DcaLogLevel level,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string msg);

    // --- Library lifecycle -------------------------------------------------

    [DllImport(LibName, EntryPoint = "dca_abi_version",     CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint AbiVersion();

    [DllImport(LibName, EntryPoint = "dca_version_string",  CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint VersionStringRaw();

    [DllImport(LibName, EntryPoint = "dca_set_log_callback", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SetLogCallback(DcaLogCallback? cb, nint user);

    // --- Engine ------------------------------------------------------------

    [DllImport(LibName, EntryPoint = "dca_engine_create",  CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineCreate(in DcaEngineConfig cfg, out nint engine);

    /// <summary>Mirrors <c>DCA_ENGINE_CREATE_NULL_DEVICE</c> (ABI v41): register the
    /// headless "Null" device type with THIS engine's device manager.</summary>
    internal const uint EngineCreateNullDevice       = 1u << 0;
    /// <summary>Mirrors <c>DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL</c> (ABI v41): the
    /// Null device runs in manual-pump mode. Implies <see cref="EngineCreateNullDevice" />.</summary>
    internal const uint EngineCreateNullDeviceManual = 1u << 1;
    /// <summary>Mirrors <c>DCA_ENGINE_CREATE_KNOWN_FLAGS</c>.</summary>
    internal const uint EngineCreateKnownFlags       = EngineCreateNullDevice | EngineCreateNullDeviceManual;

    /// <summary>ABI v41: <c>dca_engine_create</c> with per-engine
    /// <c>DCA_ENGINE_CREATE_*</c> flags. An unknown bit is <see cref="DcaStatus.InvalidArg" />;
    /// <paramref name="engine" /> is zero on every failure.</summary>
    [DllImport(LibName, EntryPoint = "dca_engine_create2", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineCreate2(in DcaEngineConfig cfg, uint flags, out nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_destroy", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDestroy(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_start",   CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineStart(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_stop",    CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineStop(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_get_sample_rate",   CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetSampleRate  (nint engine, out int sr);

    [DllImport(LibName, EntryPoint = "dca_engine_get_buffer_frames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetBufferFrames(nint engine, out int frames);

    [DllImport(LibName, EntryPoint = "dca_engine_get_output_latency_samples", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetOutputLatencySamples(nint engine, out int samples);

    // Native reads these ids with juce::String::fromUTF8 — marshal them UTF-8
    // like every other per-call string param (audit #3: they were LPStr/ANSI).
    [DllImport(LibName, EntryPoint = "dca_engine_reconfigure",      CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineReconfigure(
        nint engine,
        int sampleRate,
        int bufferFrames,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? outputDeviceId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? inputDeviceId);

    [DllImport(LibName, EntryPoint = "dca_engine_set_device_type", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetDeviceType(
        nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? typeName,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? outputDeviceId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? inputDeviceId,
        uint outputChannelMask,
        uint inputChannelMask);

    [DllImport(LibName, EntryPoint = "dca_engine_set_monitor_gain",  CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetMonitorGain(nint engine, float gain);

    [DllImport(LibName, EntryPoint = "dca_engine_set_input_gain",    CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetInputGain(nint engine, float gain);

    [DllImport(LibName, EntryPoint = "dca_engine_set_input_pitch_shift", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetInputPitchShift(nint engine, float semitones);

    [DllImport(LibName, EntryPoint = "dca_engine_set_input_pitch_shift_mode", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetInputPitchShiftMode(nint engine, int mode);

    // ABI v31: the same two knobs per SEAT. The two above are these with
    // player = 0, exactly as EngineSetInputCaptureChannel is
    // EngineSetPlayerInputChannel(0, …).
    [DllImport(LibName, EntryPoint = "dca_engine_set_player_input_pitch_shift", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetPlayerInputPitchShift(nint engine, int player, float semitones);

    [DllImport(LibName, EntryPoint = "dca_engine_set_player_input_pitch_shift_mode", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetPlayerInputPitchShiftMode(nint engine, int player, int mode);

    [DllImport(LibName, EntryPoint = "dca_engine_get_input_peak",    CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetInputPeak(nint engine, out float peak);

    [DllImport(LibName, EntryPoint = "dca_engine_set_input_capture_channel", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetInputCaptureChannel(nint engine, int channel);

    [DllImport(LibName, EntryPoint = "dca_engine_read_input_capture", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineReadInputCapture(nint engine, float* dest, int count, out int written);

    // ABI v15: streaming cursor variant — returns 0 when caught up instead of
    // re-serving the latest window. Safe to drain-poll.
    [DllImport(LibName, EntryPoint = "dca_engine_read_input_capture_stream", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineReadInputCaptureStream(nint engine, float* dest, int count, out int written);

    // ABI v16: position-reporting streaming read + capture/host-clock sync pair.
    [DllImport(LibName, EntryPoint = "dca_engine_read_input_capture_stream2", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineReadInputCaptureStream2(nint engine, float* dest, int count, out int written, out ulong streamPosition);

    [DllImport(LibName, EntryPoint = "dca_engine_get_input_capture_sync", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetInputCaptureSync(nint engine, out ulong capturedSamples, out long hostTicks);

    // ---- Per-player seats (ABI v28) --------------------------------------
    // One live-input rig per seat, all sharing ONE open device — this is
    // channel routing inside a single AudioIODevice, never a second device
    // open. Every entry point above is the same call with player = 0.

    [DllImport(LibName, EntryPoint = "dca_engine_set_player_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetPlayerCount(nint engine, int count);

    [DllImport(LibName, EntryPoint = "dca_engine_get_player_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineGetPlayerCount(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_set_player_input_channel", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetPlayerInputChannel(nint engine, int player, int channel);

    [DllImport(LibName, EntryPoint = "dca_engine_get_player_input_channel", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineGetPlayerInputChannel(nint engine, int player);

    [DllImport(LibName, EntryPoint = "dca_engine_set_player_monitor_gain", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetPlayerMonitorGain(nint engine, int player, float linearGain);

    [DllImport(LibName, EntryPoint = "dca_engine_get_player_input_peak", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetPlayerInputPeak(nint engine, int player, out float peak);

    [DllImport(LibName, EntryPoint = "dca_engine_read_input_capture_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineReadInputCaptureForPlayer(nint engine, int player,
        float* dest, int count, out int written);

    [DllImport(LibName, EntryPoint = "dca_engine_read_input_capture_stream_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineReadInputCaptureStreamForPlayer(nint engine, int player,
        float* dest, int count, out int written);

    [DllImport(LibName, EntryPoint = "dca_engine_read_input_capture_stream2_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineReadInputCaptureStream2ForPlayer(nint engine, int player,
        float* dest, int count, out int written, out ulong streamPosition);

    [DllImport(LibName, EntryPoint = "dca_engine_get_input_capture_sync_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetInputCaptureSyncForPlayer(nint engine, int player,
        out ulong capturedSamples, out long hostTicks);

    // --- ABI v37: positional capture read + input injection (re-amp) ---------

    [DllImport(LibName, EntryPoint = "dca_engine_read_input_capture_from_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineReadInputCaptureFromForPlayer(nint engine, int player,
        ulong from, float* dest, int count, out int written, out ulong first);

    [DllImport(LibName, EntryPoint = "dca_engine_set_input_injection_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineSetInputInjectionForPlayer(nint engine, int player,
        float* mono, long frames, int loop);

    [DllImport(LibName, EntryPoint = "dca_engine_clear_input_injection_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineClearInputInjectionForPlayer(nint engine, int player);

    [DllImport(LibName, EntryPoint = "dca_engine_set_input_injection_playing_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetInputInjectionPlayingForPlayer(nint engine, int player, int playing);

    [DllImport(LibName, EntryPoint = "dca_engine_set_input_injection_loop_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetInputInjectionLoopForPlayer(nint engine, int player, int loop);

    [DllImport(LibName, EntryPoint = "dca_engine_seek_input_injection_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSeekInputInjectionForPlayer(nint engine, int player, long frame);

    [DllImport(LibName, EntryPoint = "dca_engine_get_input_injection_status_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetInputInjectionStatusForPlayer(nint engine, int player,
        out long position, out long length, out int playing, out int loop);

    // --- ABI v38: chart source + song clock + instrument route ----------------

    // dca_chart_note — 80 bytes, 8-aligned, pinned field by field to the header
    // by AbiV38Tests. APPEND-EXTENSIBLE: the upload passes this struct's size as
    // its stride, so a field appended at the end of the native struct is read
    // only by a caller that knows it.
    [StructLayout(LayoutKind.Sequential)]
    internal struct DcaChartNote
    {
        public double StartSeconds;
        public double ReleaseSeconds;
        public double SustainSeconds;
        public float  Pitch;
        public float  Velocity;
        public int    StringIndex;
        public int    Fret;
        public float  SlideToFret;
        public uint   Flags;
        public int    ChordId;
        public int    BendFirst;
        public int    BendCount;
        public int    Strum;
        public int    Capo;
        public float  Reserved1;
        public float  Reserved2;
        public float  Reserved3;
    }

    // dca_chart_bend_point — 16 bytes.
    [StructLayout(LayoutKind.Sequential)]
    internal struct DcaChartBendPoint
    {
        public double TimeSeconds;
        public float  Semitones;
        public int    Reserved;
    }

    // dca_chart_source_status — cbSize-style like DcaXrunStats; 112 bytes (v40
    // appended the last six fields to the v38 80).
    [StructLayout(LayoutKind.Sequential)]
    internal struct DcaChartSourceStatus
    {
        public int    StructBytes;
        public int    Enabled;
        public int    Voice;
        public int    Mode;
        public int    NoteCount;
        public int    ActiveVoices;
        public int    ClockBound;
        public int    ClockPlaying;
        public long   NotesStarted;
        public long   NotesChased;
        public long   NotesDropped;
        public double SongSeconds;
        public float  Gain;
        public float  Reserved0;
        public double OffsetSeconds;
        // ---- ABI v40 ----
        public int    Model;
        public int    Pickup;
        public uint   InstrumentFlags;
        public int    Sampled;
        public long   PackBytes;
        public int    VoiceLoaded;
        public int    Reserved1;
    }

    // dca_chart_model_info (ABI v40) — cbSize-style; 48 bytes.
    [StructLayout(LayoutKind.Sequential)]
    internal struct DcaChartModelInfo
    {
        public int  StructBytes;
        public int  Bass;
        public uint PickupMask;
        public int  DefaultPickup;
        public int  HasCoilSplit;
        public int  Installed;
        public uint RecordedMask;
        public uint RecordedAnyMask;
        public int  Reserved0;
        public int  Reserved1;
        public int  Reserved2;
        public int  Reserved3;
    }

    /// <summary>Mirrors <c>DCA_CHART_INSTRUMENT_COIL_SPLIT</c>.</summary>
    internal const uint ChartInstrumentCoilSplit = 1u;

    [DllImport(LibName, EntryPoint = "dca_engine_chart_source_set_instrument_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineChartSourceSetInstrumentForPlayer(nint engine, int player, int model, int pickup, uint flags);

    [DllImport(LibName, EntryPoint = "dca_engine_chart_source_get_instrument_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineChartSourceGetInstrumentForPlayer(nint engine, int player, int bass,
        out int model, out int pickup, out uint flags);

    [DllImport(LibName, EntryPoint = "dca_chart_model_get_info", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus ChartModelGetInfo(int model, ref DcaChartModelInfo info);

    [DllImport(LibName, EntryPoint = "dca_chart_voice_pack_bytes", CallingConvention = CallingConvention.Cdecl)]
    internal static extern long ChartVoicePackBytes();

    [DllImport(LibName, EntryPoint = "dca_engine_set_song_clock_source", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetSongClockSource(nint engine, nint source);

    [DllImport(LibName, EntryPoint = "dca_engine_chart_source_set_notes_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineChartSourceSetNotesForPlayer(nint engine, int player,
        DcaChartNote* notes, int noteCount, int noteStrideBytes, DcaChartBendPoint* bends, int bendCount);

    /// <summary>ABI v48: the note upload with the byte length of the notes buffer,
    /// so the engine checks count x stride against it.</summary>
    [DllImport(LibName, EntryPoint = "dca_engine_chart_source_set_notes_for_player2", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineChartSourceSetNotesForPlayer2(nint engine, int player,
        DcaChartNote* notes, int noteCount, int noteStrideBytes, long notesByteLength,
        DcaChartBendPoint* bends, int bendCount);

    [DllImport(LibName, EntryPoint = "dca_engine_chart_source_set_enabled_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineChartSourceSetEnabledForPlayer(nint engine, int player, int enabled);

    [DllImport(LibName, EntryPoint = "dca_engine_chart_source_set_voice_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineChartSourceSetVoiceForPlayer(nint engine, int player, int voice);

    [DllImport(LibName, EntryPoint = "dca_engine_chart_source_set_params_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineChartSourceSetParamsForPlayer(nint engine, int player,
        float gainLinear, double offsetSeconds, int mode);

    [DllImport(LibName, EntryPoint = "dca_engine_chart_source_get_status_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineChartSourceGetStatusForPlayer(nint engine, int player,
        ref DcaChartSourceStatus status);

    // Value-returning: 1 = available, 0 = a known kind this build lacks, -1 = not a kind.
    [DllImport(LibName, EntryPoint = "dca_chart_voice_is_available", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ChartVoiceIsAvailable(int voice);

    [DllImport(LibName, EntryPoint = "dca_engine_set_instrument_route", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetInstrumentRoute(nint engine, int player);

    [DllImport(LibName, EntryPoint = "dca_engine_get_instrument_route", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetInstrumentRoute(nint engine, out int player);

    [DllImport(LibName, EntryPoint = "dca_engine_show_asio_panel",   CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineShowAsioPanel(nint engine, nint parent);

    [DllImport(LibName, EntryPoint = "dca_engine_set_master_volume", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetMasterVolume(nint engine, float gain);

    [DllImport(LibName, EntryPoint = "dca_engine_set_high_pass",     CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetHighPass(nint engine, float freq, float q);

    [DllImport(LibName, EntryPoint = "dca_engine_set_low_pass",      CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetLowPass(nint engine, float freq, float q);

    [DllImport(LibName, EntryPoint = "dca_engine_set_band_pass",     CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetBandPass(nint engine, float freq, float q);

    // ---- Parametric EQ (ABI v29) -----------------------------------------

    [DllImport(LibName, EntryPoint = "dca_engine_set_eq_band",       CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetEqBand(nint engine, int index, int type,
        float freqHz, float q, float gainDb, int enabled);

    [DllImport(LibName, EntryPoint = "dca_engine_set_eq_enabled",    CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetEqEnabled(nint engine, int enabled);

    [DllImport(LibName, EntryPoint = "dca_engine_set_eq_solo",       CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetEqSolo(nint engine, int bandIndex);

    [DllImport(LibName, EntryPoint = "dca_engine_set_eq_route",      CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetEqRoute(nint engine, int route);

    [DllImport(LibName, EntryPoint = "dca_engine_set_eq_trim",       CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetEqTrim(nint engine, float trimDb);

    [DllImport(LibName, EntryPoint = "dca_engine_get_eq_active_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetEqActiveCount(nint engine, out int count);

    [DllImport(LibName, EntryPoint = "dca_engine_read_eq_spectrum",  CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineReadEqSpectrum(nint engine, [Out] float[] destDb,
        int binCount, out int written);

    [DllImport(LibName, EntryPoint = "dca_engine_enumerate_output_devices", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineEnumerateOutputs(nint engine,
        [Out] DcaDeviceInfo[]? devices, int capacity, out int count);

    [DllImport(LibName, EntryPoint = "dca_engine_enumerate_input_devices", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineEnumerateInputs(nint engine,
        [Out] DcaDeviceInfo[]? devices, int capacity, out int count);

    /// <summary>Mirrors <c>DCA_CHANNEL_NAME_BYTES</c>: the stride of one name in the
    /// <c>dca_engine_get_channel_names</c> buffer.</summary>
    internal const int ChannelNameBytes = 64;

    /// <summary>ABI v49. The open device's channel names, <see cref="ChannelNameBytes" />
    /// per entry; <paramref name="count" /> is the device's total, possibly more
    /// than <paramref name="capacityEntries" />.</summary>
    [DllImport(LibName, EntryPoint = "dca_engine_get_channel_names", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetChannelNames(nint engine, int isInput,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? deviceName,
        [Out] byte[]? names, int capacityEntries, out int count);

    [DllImport(LibName, EntryPoint = "dca_engine_set_mirror_device", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetMirrorDevice(nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? deviceId);

    [DllImport(LibName, EntryPoint = "dca_engine_play_click", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EnginePlayClick(nint engine, int clickType);

    // ABI v26: sample-accurate click scheduling on the output render clock.
    [DllImport(LibName, EntryPoint = "dca_engine_get_render_clock", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetRenderClock(nint engine, out long renderFrames, out long hostTicks);

    // ABI v30: the engine's own dropout counters, bumped by relaxed atomics on
    // the audio thread (no allocation, no lock, no syscall). Layout is pinned to
    // dotcharter_audio.h by NativeMethodsTests — 4x int32 then 11x int64, so the
    // int64 block is already 8-aligned and the struct carries no padding.
    [StructLayout(LayoutKind.Sequential)]
    internal struct DcaXrunStats
    {
        // IN: sizeof(DcaXrunStats), Win32 cbSize style. OUT: bytes the engine
        // wrote. Appending a field at the END of the native struct is additive
        // and does NOT bump the ABI, which is what this field buys.
        public int  StructBytes;
        public int  DeviceOpen;
        public int  DeviceRunning;
        // Driver-reported since reset. -1 means the driver CANNOT report (WASAPI,
        // CoreAudio aggregate); never surface that as zero.
        public int  DeviceXruns;
        public long Callbacks;
        // The audible one: the gap before this callback exceeded 1.5x the previous
        // block + 0.5 ms, so the device filled part of a buffer without us.
        public long LateCallbacks;
        // WE were the cause — the callback overran its own block budget.
        public long DeadlineOverruns;
        // Over 80% of budget but inside it. A load warning, not a dropout, and
        // strictly disjoint from DeadlineOverruns.
        public long NearMisses;
        public long DroppedFrames;
        // juce::Time::getHighResolutionTicks() at the last late callback / overrun;
        // 0 = none since reset. Same timebase as the render clock and Stopwatch.
        public long LastXrunHostTicks;
        public long LastXrunRenderFrame;
        public long MaxCallbackNs;
        public long LastCallbackNs;
        public long BlockBudgetNs;
        // Input-chain VST3s that threw out of processBlock and were DISABLED
        // (ABI-additive, 2026-09). ONE per plugin, not one per block.
        public long InputPluginFaults;
    }

    [DllImport(LibName, EntryPoint = "dca_engine_get_xrun_stats", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetXrunStats(nint engine, ref DcaXrunStats stats);

    [DllImport(LibName, EntryPoint = "dca_engine_reset_xrun_stats", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineResetXrunStats(nint engine);

    // v46: the callback load (fractions of the buffer period). Either pointer may be null.
    [DllImport(LibName, EntryPoint = "dca_engine_get_callback_load", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetCallbackLoad(nint engine, out float average, out float peak);

    // v47: the external process insert. Laid out exactly as the
    // header's dca_extproc_stats (8x int32 then 5x int64, no padding); pinned by
    // NativeMethodsTests.DcaExtprocStats_LayoutMatchesTheHeader.
    [StructLayout(LayoutKind.Sequential)]
    internal struct DcaExtprocStats
    {
        // IN: sizeof(DcaExtprocStats). OUT: bytes the engine wrote (cbSize style).
        public int  StructBytes;
        public int  State;
        public int  Connected;
        public int  SampleRate;
        public int  BlockFrames;
        public int  RingBlocks;
        public int  LatencySamples;
        public int  Reserved;
        public long BlocksSent;
        public long BlocksReceived;
        public long Underruns;
        public long Overruns;
        public long LateBlocks;
    }

    [DllImport(LibName, EntryPoint = "dca_engine_extproc_open_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineExtprocOpenForPlayer(nint engine, int player,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? mappingName,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? magic);

    [DllImport(LibName, EntryPoint = "dca_engine_extproc_open", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineExtprocOpen(nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? mappingName,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? magic);

    [DllImport(LibName, EntryPoint = "dca_engine_extproc_close_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineExtprocCloseForPlayer(nint engine, int player);

    [DllImport(LibName, EntryPoint = "dca_engine_extproc_close", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineExtprocClose(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_extproc_get_stats_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineExtprocGetStatsForPlayer(nint engine, int player, ref DcaExtprocStats stats);

    [DllImport(LibName, EntryPoint = "dca_engine_extproc_get_stats", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineExtprocGetStats(nint engine, ref DcaExtprocStats stats);

    [DllImport(LibName, EntryPoint = "dca_engine_schedule_click", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineScheduleClick(nint engine, int clickType, long renderFrame);

    [DllImport(LibName, EntryPoint = "dca_engine_clear_scheduled_clicks", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineClearScheduledClicks(nint engine);

    [DllImport(LibName, EntryPoint = "dca_generate_waveform", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus GenerateWaveform(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        double blockMs,
        [Out] float[]? outLevels,
        int capacity,
        out int outCount);

    [DllImport(LibName, EntryPoint = "dca_generate_waveform_from_memory", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus GenerateWaveformFromMemory(
        [In] byte[] data,
        long dataSize,
        double blockMs,
        [Out] float[]? outLevels,
        int capacity,
        out int outCount);

    // --- Offline decode ----------------------------------------------------

    [DllImport(LibName, EntryPoint = "dca_decode_audio_file", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus DecodeAudioFile(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        out nint outSamples,
        out int outSampleRate,
        out int outChannels,
        out long outFrames);

    // SIZING-ONLY view of the SAME entry point, and it exists because the
    // overload above cannot express the header's cheap path.
    //
    // dca_decode_audio_file decodes the whole file and hands the caller
    // OWNERSHIP of a malloc'd interleaved buffer whenever out_samples != NULL;
    // with out_samples == NULL it fills the three metadata outs and returns
    // BEFORE the reader ever runs (dotcharter_audio.cpp: "sizing call only").
    // An `out nint` parameter always passes a real address, so that NULL path
    // was unreachable — every metadata probe therefore paid a full decode AND
    // leaked frames * channels * 4 bytes of native memory, forever.
    // Pass nint.Zero here; never a real pointer.
    [DllImport(LibName, EntryPoint = "dca_decode_audio_file", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus ProbeAudioFile(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        nint outSamplesMustBeZero,
        out int outSampleRate,
        out int outChannels,
        out long outFrames);

    [DllImport(LibName, EntryPoint = "dca_decode_free", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void DecodeFree(nint samples);

    // --- Wwise Vorbis (.wem) decoding (ABI v42) --------------------------------

    /// <summary>Mirrors <c>dca_wem_info</c> (48 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct DcaWemInfo
    {
        public int  Channels;
        public int  SampleRate;
        public long Frames;
        public long HeaderSampleCount;
        public long DecodedFrames;
        public int  Variant;
        public int  ModPackets;
        public int  GranulePresent;
        public int  PacketCount;
    }

    [DllImport(LibName, EntryPoint = "dca_wem_open_file", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus WemOpenFile(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, int variant, out nint wem);

    [DllImport(LibName, EntryPoint = "dca_wem_open_memory", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus WemOpenMemory(byte* data, nuint length, int variant, out nint wem);

    [DllImport(LibName, EntryPoint = "dca_wem_get_info", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus WemGetInfo(nint wem, out DcaWemInfo info);

    [DllImport(LibName, EntryPoint = "dca_wem_read_f32", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus WemReadF32(nint wem, float* interleaved, int capacityFrames, out int frames);

    [DllImport(LibName, EntryPoint = "dca_wem_read_s16", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus WemReadS16(nint wem, short* interleaved, int capacityFrames, out int frames);

    [DllImport(LibName, EntryPoint = "dca_wem_seek", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus WemSeek(nint wem, long frame);

    [DllImport(LibName, EntryPoint = "dca_wem_destroy", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus WemDestroy(nint wem);

    // --- Wwise Vorbis (.wem) encoding (ABI v44) --------------------------------

    [DllImport(LibName, EntryPoint = "dca_wem_encode_vorbis", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus WemEncodeVorbis(
        float* interleaved, long frames, int channels, int sampleRate, out nint data, out long size);

    [DllImport(LibName, EntryPoint = "dca_wem_encode_free", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void WemEncodeFree(nint data);

    // --- Devices -----------------------------------------------------------

    [DllImport(LibName, EntryPoint = "dca_enumerate_output_devices", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EnumerateOutputDevices(
        [Out] DcaDeviceInfo[]? devices, int capacity, out int count);

    [DllImport(LibName, EntryPoint = "dca_enumerate_input_devices", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EnumerateInputDevices(
        [Out] DcaDeviceInfo[]? devices, int capacity, out int count);

    [DllImport(LibName, EntryPoint = "dca_enumerate_host_devices", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EnumerateHostDevices(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? typeName,
        int isInput,
        [Out] DcaDeviceInfo[]? devices, int capacity, out int count);

    [DllImport(LibName, EntryPoint = "dca_enumerate_host_types", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EnumerateHostTypes(
        [Out] byte[]? typeNames, int capacityEntries, out int count);

    // --- Sources -----------------------------------------------------------

    [DllImport(LibName, EntryPoint = "dca_source_open_file", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceOpenFile(nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, out nint source);

    [DllImport(LibName, EntryPoint = "dca_source_open_memory", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus SourceOpenMemory(nint engine,
        byte* bytes, nuint numBytes,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? formatHint,
        out nint source);

    [DllImport(LibName, EntryPoint = "dca_source_destroy", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceDestroy(nint source);

    [DllImport(LibName, EntryPoint = "dca_source_play",  CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourcePlay (nint source);
    [DllImport(LibName, EntryPoint = "dca_source_pause", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourcePause(nint source);
    [DllImport(LibName, EntryPoint = "dca_source_stop",  CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceStop (nint source);

    [DllImport(LibName, EntryPoint = "dca_source_seek_seconds",         CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceSeek       (nint source, double seconds);
    [DllImport(LibName, EntryPoint = "dca_source_get_position_seconds", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceGetPosition(nint source, out double seconds);
    [DllImport(LibName, EntryPoint = "dca_source_get_length_seconds",   CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceGetLength  (nint source, out double seconds);

    // ABI v43 (DotCharter #435). out_bits: 16 for a written file (int16 samples +
    // the preload step in a dcps chunk), 0 when the load would not convert and
    // nothing was written.
    [DllImport(LibName, EntryPoint = "dca_convert_audio_for_device", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus ConvertAudioForDevice(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string srcPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string dstWavPath,
        double deviceRate,
        out int outBits);

    // ABI v43 test seam: a fingerprint of what a preloaded source holds.
    [DllImport(LibName, EntryPoint = "dca_source_get_preload_digest", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceGetPreloadDigest(
        nint source, out ulong hash, out float step, out long frames, out double rate, out int converted);

    [DllImport(LibName, EntryPoint = "dca_source_set_volume",          CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceSetVolume        (nint source, float gain);
    [DllImport(LibName, EntryPoint = "dca_source_set_pan",             CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceSetPan           (nint source, float pan);
    [DllImport(LibName, EntryPoint = "dca_source_set_tempo",           CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceSetTempo         (nint source, float factor);
    [DllImport(LibName, EntryPoint = "dca_source_set_pitch_semitones", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceSetPitchSemitones(nint source, float semitones);

    [DllImport(LibName, EntryPoint = "dca_engine_set_stretch_quality", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetStretchQuality(nint engine, int quality);

    // --- Stem group (shared stretcher, ABI v9) -----------------------------

    [DllImport(LibName, EntryPoint = "dca_source_set_stem_grouped",       CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceSetStemGrouped     (nint source, int grouped);
    [DllImport(LibName, EntryPoint = "dca_engine_set_stem_group_tempo",   CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetStemGroupTempo  (nint engine, float factor);
    [DllImport(LibName, EntryPoint = "dca_engine_set_stem_group_pitch",   CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetStemGroupPitch  (nint engine, float semitones);

    // --- Plugins -----------------------------------------------------------

    [DllImport(LibName, EntryPoint = "dca_plugin_load_vst3", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginLoadVst3(nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, out nint plugin);

    // ABI v33: one .vst3 bundle may declare SEVERAL plugins. Until v33 this ABI
    // took descriptor 0 and nothing else, so every component past the first was
    // unreachable — and unreported, because loading the bundle looked like it
    // worked. PluginLoadVst3 above is exactly component 0.
    [DllImport(LibName, EntryPoint = "dca_plugin_count_vst3_components", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginCountVst3Components(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, out int count);

    [DllImport(LibName, EntryPoint = "dca_plugin_load_vst3_component", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginLoadVst3Component(nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, int componentIndex, out nint plugin);

    [DllImport(LibName, EntryPoint = "dca_plugin_unload",            CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginUnload          (nint plugin);
    [DllImport(LibName, EntryPoint = "dca_plugin_attach_to_input",   CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginAttachToInput   (nint plugin);
    // ABI v28: attach to ONE seat's input chain. Attaching an already-attached
    // plugin to a different seat MOVES it (native detaches first), so one
    // instance is never run twice per block over two different signals.
    [DllImport(LibName, EntryPoint = "dca_plugin_attach_to_input_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginAttachToInputForPlayer(nint plugin, int player);
    [DllImport(LibName, EntryPoint = "dca_plugin_detach_from_input", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginDetachFromInput (nint plugin);
    // ABI v28: the seat holding this plugin, or -1 when it is not attached.
    [DllImport(LibName, EntryPoint = "dca_plugin_get_input_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int PluginGetInputPlayer(nint plugin);
    [DllImport(LibName, EntryPoint = "dca_plugin_show_editor",       CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginShowEditor      (nint plugin, nint parent);
    [DllImport(LibName, EntryPoint = "dca_plugin_hide_editor",       CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginHideEditor      (nint plugin);

    // ABI v14: invoked on the JUCE message thread after the user closed the
    // editor window with its native X button and the window was torn down.
    // Keep the delegate rooted for the plugin's lifetime.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DcaEditorClosedCallback(nint user);
    [DllImport(LibName, EntryPoint = "dca_plugin_set_editor_closed_callback", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginSetEditorClosedCallback(nint plugin, DcaEditorClosedCallback? cb, nint user);

    [DllImport(LibName, EntryPoint = "dca_plugin_attach_to_instrument",   CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginAttachToInstrument  (nint plugin);
    [DllImport(LibName, EntryPoint = "dca_plugin_detach_from_instrument", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginDetachFromInstrument(nint plugin);

    // --- ABI v19: out-of-process VST bridge --------------------------------

    [DllImport(LibName, EntryPoint = "dca_engine_set_remote_bridge", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetRemoteBridge(nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? shmName,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? requestEventName,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? responseEventName,
        int enable);

    [DllImport(LibName, EntryPoint = "dca_engine_get_remote_bridge_stats", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetRemoteBridgeStats(nint engine,
        out uint processed, out uint timeouts, out uint heartbeat);

    [DllImport(LibName, EntryPoint = "dca_engine_send_midi_short",     CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSendMidiShort(nint engine, byte status, byte data1, byte data2);
    [DllImport(LibName, EntryPoint = "dca_engine_set_instrument_gain", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetInstrumentGain(nint engine, float gain);

    [DllImport(LibName, EntryPoint = "dca_source_get_audible_position_seconds", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus SourceGetAudiblePosition(nint source, out double seconds);

    [DllImport(LibName, EntryPoint = "dca_plugin_save_state", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginSaveState(nint plugin,
        [Out] byte[]? buffer, int capacity, out int written);

    [DllImport(LibName, EntryPoint = "dca_plugin_load_state", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginLoadState(nint plugin,
        [In] byte[] data, int length);

    // --- ABI v4: VST3 parameters ------------------------------------------

    [DllImport(LibName, EntryPoint = "dca_plugin_get_parameter_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginGetParameterCount(nint plugin, out int count);

    [DllImport(LibName, EntryPoint = "dca_plugin_get_parameter_info",  CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginGetParameterInfo(nint plugin, int index,
        [Out] byte[]? nameBuffer, int nameCapacity, out int nameLength,
        out float defaultValue, out int numSteps);

    [DllImport(LibName, EntryPoint = "dca_plugin_get_parameter",       CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginGetParameter(nint plugin, int index, out float value);

    [DllImport(LibName, EntryPoint = "dca_plugin_set_parameter",       CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginSetParameter(nint plugin, int index, float value);

    [DllImport(LibName, EntryPoint = "dca_plugin_get_latency_samples",  CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus PluginGetLatencySamples(nint plugin, out int samples);

    // --- ABI v4: MIDI hardware input --------------------------------------

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DcaMidiCallback(nint user, double timestampSeconds, nint data, int length);

    [DllImport(LibName, EntryPoint = "dca_midi_input_enumerate", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus MidiInputEnumerate(
        [Out] byte[]? buffer, int capacity, out int size, out int count);

    [DllImport(LibName, EntryPoint = "dca_midi_input_open",      CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus MidiInputOpen(int deviceIndex, DcaMidiCallback cb, nint user, out nint handle);

    [DllImport(LibName, EntryPoint = "dca_midi_input_close",     CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus MidiInputClose(nint handle);

    // --- ABI v4: Offline render -------------------------------------------

    [DllImport(LibName, EntryPoint = "dca_render_to_wav", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus RenderToWav(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string srcPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string dstPath,
        float tempo, float semitones, int sampleRate);

    // --- ABI v5: AI stem separation (htdemucs / hdemucs_mmi) --------------

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DcaStemsProgressCallback(
        nint user, float fraction, [MarshalAs(UnmanagedType.LPUTF8Str)] string stage);

    [DllImport(LibName, EntryPoint = "dca_stems_is_supported", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int StemsIsSupported();

    [DllImport(LibName, EntryPoint = "dca_stems_describe_backend", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus StemsDescribeBackend([Out] byte[]? buffer, int capacity, out int size);

    [DllImport(LibName, EntryPoint = "dca_stems_separate_file", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus StemsSeparateFile(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string srcPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string outDir,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string modelPath,
        int numThreads,
        DcaStemsProgressCallback? progressCb,
        nint progressUser);

    // ABI v36: the same callback with a cancellation channel. NON-ZERO means
    // CONTINUE and 0 means stop, so a handler that falls off the end of a path
    // without returning does not silently cancel every separation.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int DcaStemsProgressCallback2(
        nint user, float fraction, [MarshalAs(UnmanagedType.LPUTF8Str)] string stage);

    [DllImport(LibName, EntryPoint = "dca_stems_separate_file2", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus StemsSeparateFile2(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string srcPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string outDir,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string modelPath,
        int numThreads,
        DcaStemsProgressCallback2? progressCb,
        nint progressUser);

    // --- ABI v22: DSP tone-rig string parameters ---------------------------
    // String-valued node params (NAM model paths, user IR WAVs, Miku phrase
    // tables). Values are UTF-8; the getter returns the full byte length of
    // the value (excluding NUL) or -1 when the node/param is unknown — call
    // with a null buffer / capacity 0 to size, then again with a buffer.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_node_string", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetNodeString(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? utf8Value);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_node_string", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspGetNodeString(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId,
        [Out] byte[]? outUtf8, int capacity);

    [DllImport(LibName, EntryPoint = "dca_dsp_module_string_param_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DspModuleStringParamCount(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId);

    [DllImport(LibName, EntryPoint = "dca_dsp_module_string_param_info", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus DspModuleStringParamInfo(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId, int index,
        [Out] byte[]? outId, int idCap,
        [Out] byte[]? outName, int nameCap,
        [Out] byte[]? outFileFilter, int filterCap);

    // --- ABI v24: enum value labels ----------------------------------------
    // Named labels for enum-like model params (amp "voicing", cab "model",
    // overdrive/distortion "model"). LabelCount returns -1 for a continuous
    // param (no labels). Label copies the UTF-8 label for a value index and
    // returns the full byte length (excluding NUL), or -1 when the param has
    // no labels / the index is out of range — call with a null buffer to size.

    [DllImport(LibName, EntryPoint = "dca_dsp_module_param_label_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DspModuleParamLabelCount(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId);

    [DllImport(LibName, EntryPoint = "dca_dsp_module_param_label", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DspModuleParamLabel(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId,
        int valueIndex,
        [Out] byte[]? outUtf8, int capacity);

    // --- ABI v27: discrete quantization step --------------------------------
    // > 0 for an ORDINAL discrete param (a count the core lrounds: chorus
    // voices, phaser stages — step 2, the core forces it even —, bitcrusher
    // bits/downsample, …): the UI snaps the knob to it and shows an integer.
    // 0 = continuous; -1 = unknown module/param. NAMED discrete params carry
    // value labels instead (a picker), not a step.

    [DllImport(LibName, EntryPoint = "dca_dsp_module_param_step", CallingConvention = CallingConvention.Cdecl)]
    internal static extern float DspModuleParamStep(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId);

    // ABI v45: the rsamp chain table (src/dsp/cores/dcdsp_rsamp_table.h). The bytes
    // are copied; null with length 0 unloads. Process-wide, control thread.
    [DllImport(LibName, EntryPoint = "dca_rsamp_set_table", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus RsAmpSetTable(byte[]? data, int length);

    [DllImport(LibName, EntryPoint = "dca_rsamp_table_row_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RsAmpTableRowCount();

    // --- ABI v23: DSP split / parallel chains -------------------------------
    // Per-node branch tag for the splitter/merger parallel regions. 0 = main
    // (branch 1 inside a region); 1..3 = explicit branch. Out-of-range ids
    // and unknown nodes return InvalidArg. Full semantics are documented at
    // dca_engine_dsp_set_node_branch in dotcharter_audio.h.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_node_branch", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetNodeBranch(nint engine, int nodeId, int branchId);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_node_branch", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetNodeBranch(nint engine, int nodeId, out int branchId);

    // --- ABI v33: node introspection ---------------------------------------
    // The two things the rig knew and never told anybody. Before these, a node
    // this wrapper did not CREATE had an empty TypeId and read as un-bypassed,
    // because the only source was what the wrapper itself had recorded.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_node_type", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetNodeType(nint engine, int nodeId,
        [Out] byte[]? outUtf8, int capacity, out int outLen);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_bypass", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetBypass(nint engine, int nodeId, out int bypassed);

    // --- ABI v34: edit scopes ----------------------------------------------
    // CHAIN-scoped, so per seat. Between begin and commit the native chain
    // suppresses publication, so a whole preset reaches the audio callback as
    // ONE ordering snapshot instead of one per node. A nested begin and a
    // commit/abort with nothing open are both DCA_ERR_INVALID_ARG.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_begin_edit_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspBeginEditForPlayer(nint engine, int player);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_commit_edit_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspCommitEditForPlayer(nint engine, int player);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_abort_edit_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspAbortEditForPlayer(nint engine, int player);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_editing_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetEditingForPlayer(nint engine, int player, out int editing);

    // --- ABI v34: branch delay compensation --------------------------------
    // ON by default. Summing parallel branches of different latency is a comb
    // filter, not a blend; off is the pre-v34 behaviour byte for byte.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_branch_pdc_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetBranchPdcForPlayer(nint engine, int player, int enabled);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_branch_pdc_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetBranchPdcForPlayer(nint engine, int player, out int enabled);

    // --- ABI v37: spillover --------------------------------------------------
    // OFF at engine start. A removed delay/reverb rings out as a post-chain tail.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_spillover_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetSpilloverForPlayer(nint engine, int player, int enabled);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_spillover_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetSpilloverForPlayer(nint engine, int player, out int enabled);

    // Value-returning: ringing tails, -1 for a dead handle or a bad seat.
    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_spill_tail_count_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspGetSpillTailCountForPlayer(nint engine, int player);

    // --- ABI v34: node sidechain + module capability flags ------------------
    // 0 = none (key off the module's own input), 1 = the seat's dry input,
    // 2 = the song/master mix. REJECTED on a module type whose
    // DspModuleTypeFlags does not carry DspModuleFlagSidechain.

    /// <summary>Mirrors <c>DCA_DSP_MODULE_FLAG_SIDECHAIN</c>.</summary>
    internal const int DspModuleFlagSidechain = 0x1;

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_node_sidechain", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetNodeSidechain(nint engine, int nodeId, int source);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_node_sidechain", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetNodeSidechain(nint engine, int nodeId, out int source);

    [DllImport(LibName, EntryPoint = "dca_dsp_module_type_flags", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus DspModuleTypeFlags(int typeIndex, out int flags);

    // --- ABI v28: per-player tone-rig chains -------------------------------
    // The CHAIN-scoped half of the tone-rig ABI, addressed by seat. Everything
    // node-scoped (remove / move / bypass / params / strings / branches) needs
    // no player argument, because a node id already names its seat
    // (<see cref="PlayerNodeIdStride"/>). Returned node ids are engine-global,
    // so they are safe to hand straight back to the node-scoped calls.

    // LPUTF8Str, not LPStr. Every string across this ABI is UTF-8
    // (juce::String::fromUTF8 on the native side); LPStr marshals with the
    // SYSTEM CODEPAGE, which is the 2026-07-16 audit #3 bug. Today's module
    // type ids are pure ASCII so the two agree byte for byte — which is
    // exactly why it survived here after every sibling was converted, and
    // exactly what NativeMethodsTests.EveryStringParameter_IsMarshalledAsUtf8
    // now stops.
    [DllImport(LibName, EntryPoint = "dca_engine_dsp_add_node_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspAddNodeForPlayer(nint engine, int player,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_add_node_bypassed_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspAddNodeBypassedForPlayer(nint engine, int player,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_node_count_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspNodeCountForPlayer(nint engine, int player);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_node_ids_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspGetNodeIdsForPlayer(nint engine, int player,
        [Out] int[]? outIds, int capacity);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_clear_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspClearForPlayer(nint engine, int player);

    // Offline render of ONE seat's rig with no device open — the seam every
    // per-player tone test drives. float* rather than float[] so a caller can
    // hand it a Span slice without copying twice.
    [DllImport(LibName, EntryPoint = "dca_engine_dsp_process_buffer_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineDspProcessBufferForPlayer(nint engine, int player,
        float* left, float* right, int numFrames, int sampleRate);

    // ======================================================================
    //  DSP tone rig — the SEAT-0 / node-scoped / catalogue half (ABI v24–v32)
    // ======================================================================
    //
    // NONE of the declarations below existed until 0.8.x, and the ones above
    // (the *_for_player half, the string params, the labels, the step and the
    // branch tags) were declared and wrapped by NOTHING. So the whole in-house
    // tone rig — 57 module types, per-node meters, chain latency, the offline
    // render — was unreachable from the JuceSharp package: every consumer that
    // wanted it re-declared the C ABI itself (DotCharter's
    // DspNativeMethods.cs; the ToneRig example's DspInterop.cs, now deleted).
    // Re-declaring an ABI in three places is how one of them ends up with the
    // LPStr that EveryStringParameter_IsMarshalledAsUtf8 exists to stop.
    //
    // Node ids are ENGINE-GLOBAL (player * PlayerNodeIdStride + local), which is
    // why everything node-scoped here takes no player argument.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_add_node", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspAddNode(nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_remove_node", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspRemoveNode(nint engine, int nodeId);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_move_node", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspMoveNode(nint engine, int nodeId, int newIndex);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_bypass", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetBypass(nint engine, int nodeId, int bypassed);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_param", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, float value);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_param", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, out float value);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_node_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspNodeCount(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_node_ids", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspGetNodeIds(nint engine, [Out] int[]? outIds, int capacity);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_clear", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspClear(nint engine);

    // ---- Static module catalogue (no engine required) ---------------------

    [DllImport(LibName, EntryPoint = "dca_dsp_module_type_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DspModuleTypeCount();

    [DllImport(LibName, EntryPoint = "dca_dsp_module_type_info", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus DspModuleTypeInfo(int index,
        [Out] byte[]? outId, int idCap,
        [Out] byte[]? outName, int nameCap);

    // ABI v33. Which drawer of a palette this module belongs in, out of a short
    // FIXED set the engine owns — never a managed guess table, which would be a
    // second source of truth for something only the native factory knows.
    [DllImport(LibName, EntryPoint = "dca_dsp_module_type_category", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus DspModuleTypeCategory(int typeIndex,
        [Out] byte[]? outUtf8, int capacity, out int outLen);

    [DllImport(LibName, EntryPoint = "dca_dsp_module_param_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DspModuleParamCount(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId);

    [DllImport(LibName, EntryPoint = "dca_dsp_module_param_info", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus DspModuleParamInfo(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string moduleTypeId, int index,
        [Out] byte[]? outId, int idCap,
        [Out] byte[]? outName, int nameCap,
        out float outMin, out float outMax, out float outDefault);

    // ---- Per-node meters + latency (ABI v30) ------------------------------
    // These four lived in Dsp/JuceDspMeters.cs as a PRIVATE DllImport site with
    // raw `nint engine` arguments and raw `int` returns — outside the handle
    // drain, and invisible to NativeMethodsTests / NativeCallbackSafetyTests,
    // both of which reflect over typeof(NativeMethods) only. Every P/Invoke in
    // this package belongs here for exactly that reason.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_metering", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetMetering(nint engine, int enabled);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_metering_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetMeteringForPlayer(nint engine, int player, int enabled);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_metering", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspGetMetering(nint engine, int player);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_node_meter", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetNodeMeter(nint engine, int nodeId,
        out float inPeak, out float inRms, out float outPeak, out float outRms);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_read_node_meters", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int EngineDspReadNodeMeters(nint engine, int player,
        float* outValues, int capacity);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_node_latency", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetNodeLatency(nint engine, int nodeId, out int samples);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_chain_latency", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspGetChainLatency(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_chain_latency_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int EngineDspGetChainLatencyForPlayer(nint engine, int player);

    // ---- Offline tooling --------------------------------------------------
    // Seat 0's offline render. Returns DCA_ERR_DEVICE while a device is OPEN,
    // deliberately: the chunk loop takes the chain's process barrier once per
    // 512 frames, so rendering against a running device contends the audio
    // callback thousands of times.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_process_buffer", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus EngineDspProcessBuffer(nint engine,
        float* left, float* right, int numFrames, int sampleRate);

    // Drop-tune (CSVD) offline processor. MONO, engine-less: it runs one
    // DropTune instance over the segments given, so an engage/disengage lands
    // mid-stream against a primed ring exactly like the live pedal.
    [DllImport(LibName, EntryPoint = "dca_droptune_process_offline", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus DropTuneProcessOffline(
        int sampleRate,
        float maxLagMs,
        float* semitonesPerSegment,
        int* segmentLengths,
        int numSegments,
        float* input,
        float* output,
        int totalSamples);

    // ---- v34 (engine) -----------------------------------------------------

    // Master-bus ceiling. `enabled` is an int32 across the ABI (C has no bool
    // of a pinned width); a non-finite ceiling or release is REJECTED native
    // side, never clamped.
    [DllImport(LibName, EntryPoint = "dca_engine_set_master_limiter", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSetMasterLimiter(nint engine, int enabled,
        float ceilingDb, float releaseMs);

    [DllImport(LibName, EntryPoint = "dca_engine_get_master_limiter", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetMasterLimiter(nint engine, out int enabled,
        out float ceilingDb, out float releaseMs);

    [DllImport(LibName, EntryPoint = "dca_engine_get_master_limiter_reduction_db", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetMasterLimiterReductionDb(nint engine, out float db);

    [DllImport(LibName, EntryPoint = "dca_engine_get_master_limiter_latency_samples", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetMasterLimiterLatencySamples(nint engine, out int samples);

    // Sample-accurate MIDI to the instrument bus. `packed` is
    // status | data1<<8 | data2<<16; `renderFrame` is an ABSOLUTE output-timeline
    // frame in the domain dca_engine_get_render_clock publishes, and negative
    // means "as soon as possible" (exactly what EngineSendMidiShort produces).
    [DllImport(LibName, EntryPoint = "dca_engine_send_midi_at", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineSendMidiAt(nint engine, uint packed, long renderFrame);

    [DllImport(LibName, EntryPoint = "dca_engine_get_midi_timing_stats", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineGetMidiTimingStats(nint engine,
        out long scheduled, out long late, out long dropped);

    [DllImport(LibName, EntryPoint = "dca_engine_reset_midi_timing_stats", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineResetMidiTimingStats(nint engine);

    // MIDI hardware OUTPUT. Names are UTF-8, newline-separated; two-call
    // pattern, same as the v4 input side.
    [DllImport(LibName, EntryPoint = "dca_midi_output_enumerate", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus MidiOutputEnumerate(byte[]? buffer, int capacity,
        out int size, out int count);

    [DllImport(LibName, EntryPoint = "dca_midi_output_open", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus MidiOutputOpen(int deviceIndex, out nint handle);

    [DllImport(LibName, EntryPoint = "dca_midi_output_close", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus MidiOutputClose(nint handle);

    [DllImport(LibName, EntryPoint = "dca_midi_output_send", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus MidiOutputSend(nint handle, byte* data, int length);

    [DllImport(LibName, EntryPoint = "dca_midi_output_send_at", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe DcaStatus MidiOutputSendAt(nint handle, nint engine,
        byte* data, int length, long renderFrame);

    [DllImport(LibName, EntryPoint = "dca_midi_output_get_sent_count", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus MidiOutputGetSentCount(nint handle, out long count);

    // Both dispatcher counters in ONE call (v35). `dropped` is what the sent
    // count could never say: messages thrown away because the queue was full,
    // which used to be reported to the caller as a bad ARGUMENT.
    [DllImport(LibName, EntryPoint = "dca_midi_output_get_counts", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus MidiOutputGetCounts(nint handle, out long sent, out long dropped);

    // ABI v36: adds `failed` — messages the DRIVER threw on, which is a
    // different fault from `dropped` (our queue was full and we never handed it
    // over). A third export rather than a third out-param, because the v35
    // signature has to keep working.
    [DllImport(LibName, EntryPoint = "dca_midi_output_get_counts3", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus MidiOutputGetCounts3(
        nint handle, out long sent, out long dropped, out long failed);

    // Offline render with parameter automation. `events` is a raw block of
    // dca_dsp_automation_event (48 bytes each) rather than a managed array: the
    // struct carries a fixed 32-byte id field, which is not blittable, so the
    // caller marshals it once instead of paying a per-call copy the marshaller
    // would make anyway. Returns the raw dca_status as an int so the caller can
    // pair a non-Ok with `badIndex` before it throws.
    [DllImport(LibName, EntryPoint = "dca_engine_dsp_process_buffer_automated_for_player", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int EngineDspProcessBufferAutomatedForPlayer(
        nint engine, int player, float* interleaved, int channels, int frames, int sampleRate,
        nint events, int eventCount, out int badIndex);

    // ---- v35: seat-0 aliases ----------------------------------------------
    //
    // Every pre-v28 export is an alias for seat 0 — the rule
    // native-engine-abi.md states — and the v34 additions broke that shape for
    // seven of them. The wrapper itself always passes a seat explicitly
    // (JuceDspChain.Player), so these exist so the pin below is not vacuous and
    // so a NON-.NET binding reading the header finds the shape it expects.

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_begin_edit", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspBeginEdit(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_commit_edit", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspCommitEdit(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_abort_edit", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspAbortEdit(nint engine);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_editing", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetEditing(nint engine, out int editing);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_set_branch_pdc", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspSetBranchPdc(nint engine, int enabled);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_get_branch_pdc", CallingConvention = CallingConvention.Cdecl)]
    internal static extern DcaStatus EngineDspGetBranchPdc(nint engine, out int enabled);

    [DllImport(LibName, EntryPoint = "dca_engine_dsp_process_buffer_automated", CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int EngineDspProcessBufferAutomated(
        nint engine, float* interleaved, int channels, int frames, int sampleRate,
        nint events, int eventCount, out int badIndex);
}
