using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Compile-time / reflection checks on the P/Invoke surface. These don't
/// require the native DLL — they just verify the binding shape stays stable.
/// </summary>
public sealed class NativeMethodsTests
{
    [Fact]
    public void LibName_IsJuceSharp()
        => Assert.Equal("juce_sharp", NativeMethods.LibName);

    [Fact]
    public void ExpectedAbiVersion_IsPositive()
        => Assert.True(NativeMethods.ExpectedAbiVersion >= 1u);

    [Fact]
    public void ExpectedAbiVersion_Tracks_v10_PluginLatencySamples()
    {
        // Bumped to 10 when dca_plugin_get_latency_samples was added.
        // Guards against forgetting to bump ExpectedAbiVersion alongside a
        // header bump (mismatch would mean the managed wrapper happily talks
        // to an old DLL that doesn't export the new entry point).
        Assert.True(NativeMethods.ExpectedAbiVersion >= 10u,
            $"ExpectedAbiVersion={NativeMethods.ExpectedAbiVersion}, expected >= 10 after plugin latency export.");
    }

    [Fact]
    public void PluginGetLatencySamples_Binding_Exists()
    {
        // Reflection check: confirms the P/Invoke for the new entry point
        // is wired (compile-time enforced by NameOf'ing the member).
        var m = typeof(NativeMethods).GetMethod(
            nameof(NativeMethods.PluginGetLatencySamples),
            BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(m);
        var attr = (DllImportAttribute?)
            Attribute.GetCustomAttribute(m, typeof(DllImportAttribute));
        Assert.NotNull(attr);
        Assert.Equal("dca_plugin_get_latency_samples", attr.EntryPoint);
    }

    /// <summary>
    ///     ABI v33's five exports, pinned by NAME. A managed declaration whose
    ///     <c>EntryPoint</c> typo'd is not a compile error anywhere — it is an
    ///     <c>EntryPointNotFoundException</c> on the first call, in whatever
    ///     feature happened to reach it first.
    /// </summary>
    [Theory]
    [InlineData(nameof(NativeMethods.EngineDspGetNodeType), "dca_engine_dsp_get_node_type")]
    [InlineData(nameof(NativeMethods.EngineDspGetBypass), "dca_engine_dsp_get_bypass")]
    [InlineData(nameof(NativeMethods.DspModuleTypeCategory), "dca_dsp_module_type_category")]
    [InlineData(nameof(NativeMethods.PluginCountVst3Components), "dca_plugin_count_vst3_components")]
    [InlineData(nameof(NativeMethods.PluginLoadVst3Component), "dca_plugin_load_vst3_component")]
    public void EveryAbiV33Export_HasABindingNamedAfterIt(string member, string entryPoint)
    {
        var m = typeof(NativeMethods).GetMethod(member, BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(m);
        var attr = (DllImportAttribute?)Attribute.GetCustomAttribute(m!, typeof(DllImportAttribute));
        Assert.NotNull(attr);
        Assert.Equal(entryPoint, attr!.EntryPoint);

        // …and the header really declares it, so the pin cannot outlive the ABI.
        Assert.Contains($"DCA_CALL {entryPoint}(", File.ReadAllText(FindHeader()), StringComparison.Ordinal);
    }

    /// <summary>
    ///     ABI v34's nine exports — the edit scope, branch PDC, node sidechain and
    ///     the module capability flags — pinned by NAME the same way v33's five
    ///     are. They were unlinted until the wave-2 audit (docs-drift item 7).
    /// </summary>
    [Theory]
    [InlineData(nameof(NativeMethods.EngineDspBeginEditForPlayer), "dca_engine_dsp_begin_edit_for_player")]
    [InlineData(nameof(NativeMethods.EngineDspCommitEditForPlayer), "dca_engine_dsp_commit_edit_for_player")]
    [InlineData(nameof(NativeMethods.EngineDspAbortEditForPlayer), "dca_engine_dsp_abort_edit_for_player")]
    [InlineData(nameof(NativeMethods.EngineDspGetEditingForPlayer), "dca_engine_dsp_get_editing_for_player")]
    [InlineData(nameof(NativeMethods.EngineDspSetBranchPdcForPlayer), "dca_engine_dsp_set_branch_pdc_for_player")]
    [InlineData(nameof(NativeMethods.EngineDspGetBranchPdcForPlayer), "dca_engine_dsp_get_branch_pdc_for_player")]
    [InlineData(nameof(NativeMethods.EngineDspSetNodeSidechain), "dca_engine_dsp_set_node_sidechain")]
    [InlineData(nameof(NativeMethods.EngineDspGetNodeSidechain), "dca_engine_dsp_get_node_sidechain")]
    [InlineData(nameof(NativeMethods.DspModuleTypeFlags), "dca_dsp_module_type_flags")]
    public void EveryAbiV34Export_HasABindingNamedAfterIt(string member, string entryPoint)
    {
        var m = typeof(NativeMethods).GetMethod(member, BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(m);
        var attr = (DllImportAttribute?)Attribute.GetCustomAttribute(m!, typeof(DllImportAttribute));
        Assert.NotNull(attr);
        Assert.Equal(entryPoint, attr!.EntryPoint);

        Assert.Contains($"DCA_CALL {entryPoint}(", File.ReadAllText(FindHeader()), StringComparison.Ordinal);
    }

    /// <summary>
    ///     The v34 ENGINE wave's fourteen exports — the master limiter, the
    ///     sample-accurate instrument MIDI, hardware MIDI OUTPUT and the automated
    ///     offline render — plus the eight v35 additions. <b>None of the fourteen
    ///     was pinned by anything</b>: the theory above covers the rig half of v34
    ///     only, and the wave-2 fix branch that wrote it was merged UNDER this
    ///     range. A typo'd <c>EntryPoint</c> is not a compile error anywhere; it is
    ///     an <c>EntryPointNotFoundException</c> at runtime, in whatever feature
    ///     happened to reach it first.
    /// </summary>
    [Theory]
    // v34 (engine): master-bus ceiling
    [InlineData(nameof(NativeMethods.EngineSetMasterLimiter), "dca_engine_set_master_limiter")]
    [InlineData(nameof(NativeMethods.EngineGetMasterLimiter), "dca_engine_get_master_limiter")]
    [InlineData(nameof(NativeMethods.EngineGetMasterLimiterReductionDb), "dca_engine_get_master_limiter_reduction_db")]
    [InlineData(nameof(NativeMethods.EngineGetMasterLimiterLatencySamples), "dca_engine_get_master_limiter_latency_samples")]
    // v34 (engine): sample-accurate instrument MIDI
    [InlineData(nameof(NativeMethods.EngineSendMidiAt), "dca_engine_send_midi_at")]
    [InlineData(nameof(NativeMethods.EngineGetMidiTimingStats), "dca_engine_get_midi_timing_stats")]
    [InlineData(nameof(NativeMethods.EngineResetMidiTimingStats), "dca_engine_reset_midi_timing_stats")]
    // v34 (engine): hardware MIDI output
    [InlineData(nameof(NativeMethods.MidiOutputEnumerate), "dca_midi_output_enumerate")]
    [InlineData(nameof(NativeMethods.MidiOutputOpen), "dca_midi_output_open")]
    [InlineData(nameof(NativeMethods.MidiOutputClose), "dca_midi_output_close")]
    [InlineData(nameof(NativeMethods.MidiOutputSend), "dca_midi_output_send")]
    [InlineData(nameof(NativeMethods.MidiOutputSendAt), "dca_midi_output_send_at")]
    [InlineData(nameof(NativeMethods.MidiOutputGetSentCount), "dca_midi_output_get_sent_count")]
    // v34 (engine): the automated offline render
    [InlineData(nameof(NativeMethods.EngineDspProcessBufferAutomatedForPlayer), "dca_engine_dsp_process_buffer_automated_for_player")]
    // v35: the dropped-message counter and the seven seat-0 aliases
    [InlineData(nameof(NativeMethods.MidiOutputGetCounts), "dca_midi_output_get_counts")]
    [InlineData(nameof(NativeMethods.EngineDspBeginEdit), "dca_engine_dsp_begin_edit")]
    [InlineData(nameof(NativeMethods.EngineDspCommitEdit), "dca_engine_dsp_commit_edit")]
    [InlineData(nameof(NativeMethods.EngineDspAbortEdit), "dca_engine_dsp_abort_edit")]
    [InlineData(nameof(NativeMethods.EngineDspGetEditing), "dca_engine_dsp_get_editing")]
    [InlineData(nameof(NativeMethods.EngineDspSetBranchPdc), "dca_engine_dsp_set_branch_pdc")]
    [InlineData(nameof(NativeMethods.EngineDspGetBranchPdc), "dca_engine_dsp_get_branch_pdc")]
    [InlineData(nameof(NativeMethods.EngineDspProcessBufferAutomated), "dca_engine_dsp_process_buffer_automated")]
    [InlineData(nameof(NativeMethods.EngineDspSetSpilloverForPlayer), "dca_engine_dsp_set_spillover_for_player")]
    [InlineData(nameof(NativeMethods.EngineDspGetSpilloverForPlayer), "dca_engine_dsp_get_spillover_for_player")]
    [InlineData(nameof(NativeMethods.EngineDspGetSpillTailCountForPlayer), "dca_engine_dsp_get_spill_tail_count_for_player")]
    [InlineData(nameof(NativeMethods.EngineReadInputCaptureFromForPlayer), "dca_engine_read_input_capture_from_for_player")]
    [InlineData(nameof(NativeMethods.EngineSetInputInjectionForPlayer), "dca_engine_set_input_injection_for_player")]
    [InlineData(nameof(NativeMethods.EngineClearInputInjectionForPlayer), "dca_engine_clear_input_injection_for_player")]
    [InlineData(nameof(NativeMethods.EngineSetInputInjectionPlayingForPlayer), "dca_engine_set_input_injection_playing_for_player")]
    [InlineData(nameof(NativeMethods.EngineSetInputInjectionLoopForPlayer), "dca_engine_set_input_injection_loop_for_player")]
    [InlineData(nameof(NativeMethods.EngineSeekInputInjectionForPlayer), "dca_engine_seek_input_injection_for_player")]
    [InlineData(nameof(NativeMethods.EngineGetInputInjectionStatusForPlayer), "dca_engine_get_input_injection_status_for_player")]
    [InlineData(nameof(NativeMethods.EngineSetSongClockSource), "dca_engine_set_song_clock_source")]
    [InlineData(nameof(NativeMethods.EngineChartSourceSetNotesForPlayer), "dca_engine_chart_source_set_notes_for_player")]
    [InlineData(nameof(NativeMethods.EngineChartSourceSetNotesForPlayer2), "dca_engine_chart_source_set_notes_for_player2")]
    [InlineData(nameof(NativeMethods.EngineChartSourceSetEnabledForPlayer), "dca_engine_chart_source_set_enabled_for_player")]
    [InlineData(nameof(NativeMethods.EngineChartSourceSetVoiceForPlayer), "dca_engine_chart_source_set_voice_for_player")]
    [InlineData(nameof(NativeMethods.EngineChartSourceSetParamsForPlayer), "dca_engine_chart_source_set_params_for_player")]
    [InlineData(nameof(NativeMethods.EngineChartSourceGetStatusForPlayer), "dca_engine_chart_source_get_status_for_player")]
    [InlineData(nameof(NativeMethods.ChartVoiceIsAvailable), "dca_chart_voice_is_available")]
    [InlineData(nameof(NativeMethods.EngineChartSourceSetInstrumentForPlayer), "dca_engine_chart_source_set_instrument_for_player")]
    [InlineData(nameof(NativeMethods.EngineChartSourceGetInstrumentForPlayer), "dca_engine_chart_source_get_instrument_for_player")]
    [InlineData(nameof(NativeMethods.ChartModelGetInfo), "dca_chart_model_get_info")]
    [InlineData(nameof(NativeMethods.ChartVoicePackBytes), "dca_chart_voice_pack_bytes")]
    [InlineData(nameof(NativeMethods.EngineSetInstrumentRoute), "dca_engine_set_instrument_route")]
    [InlineData(nameof(NativeMethods.EngineGetInstrumentRoute), "dca_engine_get_instrument_route")]
    [InlineData(nameof(NativeMethods.WemOpenFile), "dca_wem_open_file")]
    [InlineData(nameof(NativeMethods.WemOpenMemory), "dca_wem_open_memory")]
    [InlineData(nameof(NativeMethods.WemGetInfo), "dca_wem_get_info")]
    [InlineData(nameof(NativeMethods.WemReadF32), "dca_wem_read_f32")]
    [InlineData(nameof(NativeMethods.WemReadS16), "dca_wem_read_s16")]
    [InlineData(nameof(NativeMethods.WemSeek), "dca_wem_seek")]
    [InlineData(nameof(NativeMethods.WemDestroy), "dca_wem_destroy")]
    [InlineData(nameof(NativeMethods.ConvertAudioForDevice), "dca_convert_audio_for_device")]
    [InlineData(nameof(NativeMethods.SourceGetPreloadDigest), "dca_source_get_preload_digest")]
    [InlineData(nameof(NativeMethods.WemEncodeVorbis), "dca_wem_encode_vorbis")]
    [InlineData(nameof(NativeMethods.WemEncodeFree), "dca_wem_encode_free")]
    // v45: the rsamp chain table, loaded at run time
    [InlineData(nameof(NativeMethods.RsAmpSetTable), "dca_rsamp_set_table")]
    [InlineData(nameof(NativeMethods.RsAmpTableRowCount), "dca_rsamp_table_row_count")]
    // v46: the callback load behind a host's CPU meter
    [InlineData(nameof(NativeMethods.EngineGetCallbackLoad), "dca_engine_get_callback_load")]
    // v47: the external process insert
    [InlineData(nameof(NativeMethods.EngineExtprocOpenForPlayer), "dca_engine_extproc_open_for_player")]
    [InlineData(nameof(NativeMethods.EngineExtprocOpen), "dca_engine_extproc_open")]
    [InlineData(nameof(NativeMethods.EngineExtprocCloseForPlayer), "dca_engine_extproc_close_for_player")]
    [InlineData(nameof(NativeMethods.EngineExtprocClose), "dca_engine_extproc_close")]
    [InlineData(nameof(NativeMethods.EngineExtprocGetStatsForPlayer), "dca_engine_extproc_get_stats_for_player")]
    [InlineData(nameof(NativeMethods.EngineExtprocGetStats), "dca_engine_extproc_get_stats")]
    public void EveryEngineWaveAndV35Export_HasABindingNamedAfterIt(string member, string entryPoint)
    {
        var m = typeof(NativeMethods).GetMethod(member, BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(m);
        var attr = (DllImportAttribute?)Attribute.GetCustomAttribute(m!, typeof(DllImportAttribute));
        Assert.NotNull(attr);
        Assert.Equal(entryPoint, attr!.EntryPoint);

        Assert.Contains($"DCA_CALL {entryPoint}(", File.ReadAllText(FindHeader()), StringComparison.Ordinal);
    }

    /// <summary>
    ///     EQUALITY, not <c>&gt;=</c>. This pin used to read <c>&gt;= 33</c>, which
    ///     kept passing after v34 landed and therefore pinned nothing about it: the
    ///     managed constant and <c>DCA_ABI_VERSION</c> must agree exactly, because
    ///     the engine rejects a mismatch outright (<c>JuceAbiMismatchException</c>).
    ///     Bump both together, or not at all.
    /// </summary>
    [Fact]
    public void ExpectedAbiVersion_EqualsTheHeadersAbiVersion()
    {
        var header = File.ReadAllText(FindHeader());
        var m = Regex.Match(header, @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "no #define DCA_ABI_VERSION in dotcharter_audio.h");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
    }

    /// <summary>
    /// Every string that crosses this ABI is UTF-8 — the native side reads them
    /// with <c>juce::String::fromUTF8</c> and writes them with
    /// <c>toRawUTF8</c>. <c>LPStr</c> marshals with the SYSTEM CODEPAGE instead,
    /// which is audit #3: a device name with a non-ASCII character read back as
    /// mojibake AND re-encoded to a byte sequence the native side could not
    /// match, so device selection silently fell through to the fallback walk.
    ///
    /// <para>The whole surface was converted then — except
    /// <c>dca_engine_dsp_add_node_for_player</c>, which kept <c>LPStr</c> for
    /// another six ABI versions. Nothing noticed, because every module type id
    /// is ASCII and ASCII is identical in both encodings. That is precisely the
    /// class of defect a lint catches and a test never will, so this asserts on
    /// the DECLARATIONS rather than on behaviour.</para>
    /// </summary>
    [Fact]
    public void EveryStringParameter_IsMarshalledAsUtf8()
    {
        var offenders = new List<string>();
        foreach (var m in typeof(NativeMethods).GetMethods(BindingFlags.NonPublic | BindingFlags.Static))
        {
            if (Attribute.GetCustomAttribute(m, typeof(DllImportAttribute)) is null) continue;
            foreach (var p in m.GetParameters())
            {
                if (p.ParameterType != typeof(string)) continue;
                var marshal = (MarshalAsAttribute?)Attribute.GetCustomAttribute(p, typeof(MarshalAsAttribute));
                if (marshal?.Value != UnmanagedType.LPUTF8Str)
                    offenders.Add($"{m.Name}({p.Name}) is {marshal?.Value.ToString() ?? "unattributed"}");
            }
        }

        Assert.Empty(offenders);
    }

    /// <summary>The same rule for the callback delegates: a native log line or a
    /// stem-progress stage is <c>toRawUTF8</c> on the way out.</summary>
    [Fact]
    public void EveryCallbackStringParameter_IsMarshalledAsUtf8()
    {
        var offenders = new List<string>();
        foreach (var t in typeof(NativeMethods).GetNestedTypes(BindingFlags.NonPublic | BindingFlags.Public))
        {
            if (!typeof(Delegate).IsAssignableFrom(t)) continue;
            var invoke = t.GetMethod("Invoke");
            if (invoke is null) continue;
            foreach (var p in invoke.GetParameters())
            {
                if (p.ParameterType != typeof(string)) continue;
                var marshal = (MarshalAsAttribute?)Attribute.GetCustomAttribute(p, typeof(MarshalAsAttribute));
                if (marshal?.Value != UnmanagedType.LPUTF8Str)
                    offenders.Add($"{t.Name}({p.Name}) is {marshal?.Value.ToString() ?? "unattributed"}");
            }
        }

        Assert.Empty(offenders);
    }

    /// <summary>
    /// The two by-value structs are the only place this ABI has a LAYOUT rather
    /// than a signature, and a layout can drift silently: adding a managed field,
    /// resizing a <c>fixed byte</c> buffer or reordering two ints all compile
    /// clean and then read the wrong bytes off a native struct. These numbers
    /// come from <c>dotcharter_audio.h</c> — change the header and change them.
    /// </summary>
    [Fact]
    public void DcaDeviceInfo_LayoutMatchesTheHeader()
    {
        // char id[128] + char name[256] + dca_driver + 4×int32 + char type_name[64]
        Assert.Equal(468, Marshal.SizeOf<NativeMethods.DcaDeviceInfo>());
        Assert.Equal(128 + 256, (int)Marshal.OffsetOf<NativeMethods.DcaDeviceInfo>("Driver"));
        Assert.Equal(128 + 256 + 4, (int)Marshal.OffsetOf<NativeMethods.DcaDeviceInfo>("MaxInputChannels"));
        Assert.Equal(128 + 256 + 16, (int)Marshal.OffsetOf<NativeMethods.DcaDeviceInfo>("IsDefault"));
        Assert.Equal(128 + 256 + 20, (int)Marshal.OffsetOf<NativeMethods.DcaDeviceInfo>("TypeNameUtf8"));
    }

    [Fact]
    public void DcaEngineConfig_LayoutMatchesTheHeader()
    {
        // 4×int32 + 2×uint32 + dca_driver + 2×char[128] + 2×char[64]
        Assert.Equal(412, Marshal.SizeOf<NativeMethods.DcaEngineConfig>());
        Assert.Equal(16, (int)Marshal.OffsetOf<NativeMethods.DcaEngineConfig>("OutputChannelMask"));
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeMethods.DcaEngineConfig>("PreferredDriver"));
        Assert.Equal(28, (int)Marshal.OffsetOf<NativeMethods.DcaEngineConfig>("OutputDeviceIdUtf8"));
        Assert.Equal(28 + 128, (int)Marshal.OffsetOf<NativeMethods.DcaEngineConfig>("InputDeviceIdUtf8"));
        Assert.Equal(28 + 256, (int)Marshal.OffsetOf<NativeMethods.DcaEngineConfig>("OutputTypeNameUtf8"));
        Assert.Equal(28 + 256 + 64, (int)Marshal.OffsetOf<NativeMethods.DcaEngineConfig>("InputTypeNameUtf8"));
    }

    /// <summary>
    ///     <c>ExpectedAbiVersion</c> must equal <c>DCA_ABI_VERSION</c> in the
    ///     HEADER — read out of the source text, not out of the DLL.
    ///
    ///     <para>
    ///         <c>AuditRegressionTests</c> already compares the wrapper against
    ///         <c>NativeMethods.AbiVersion()</c>, but that asks the deployed
    ///         <c>juce_sharp.dll</c>, which is a BUILD ARTIFACT and is routinely
    ///         one or more versions stale. So a header bump landing without the
    ///         wrapper bump passes every managed test on the old DLL and then
    ///         refuses to open the engine the moment somebody rebuilds — which is
    ///         exactly how v30 (per-node meters, chain latency, xrun counters)
    ///         arrived: eleven new exports, header at 30, wrapper still on 29.
    ///     </para>
    ///     <para>
    ///         Reading the header keeps the guard honest with no DLL present at
    ///         all, so it fails in CI, in a worktree, and on the machine that has
    ///         not rebuilt yet.
    ///     </para>
    /// </summary>
    [Fact]
    public void ExpectedAbiVersion_EqualsTheHeadersDcaAbiVersion()
    {
        var header = FindHeader();
        var match = Regex.Match(
            File.ReadAllText(header),
            @"^\s*#define\s+DCA_ABI_VERSION\s+(\d+)",
            RegexOptions.Multiline);

        Assert.True(match.Success, $"No '#define DCA_ABI_VERSION' in {header}.");

        var declared = uint.Parse(match.Groups[1].Value);
        Assert.True(declared == NativeMethods.ExpectedAbiVersion,
            $"dotcharter_audio.h declares DCA_ABI_VERSION {declared} but the wrapper "
            + $"pins ExpectedAbiVersion {NativeMethods.ExpectedAbiVersion}. The engine "
            + "compares these for equality at open, so this mismatch refuses every "
            + "engine once the DLL is rebuilt. Bump the wrapper in the same change.");
    }

    /// <summary>
    ///     <c>DCA_ABI_VERSION</c> was the ONLY <c>#define</c> read out of the
    ///     header. Three more are hand-mirrored in managed code and each one is a
    ///     silent-corruption bug if it drifts: <c>DCA_MAX_PLAYERS</c> gates every
    ///     seat argument (too low refuses a seat the engine has, too high lets one
    ///     through to be rejected native-side with no context),
    ///     <c>DCA_PLAYER_NODE_ID_STRIDE</c> is the arithmetic that turns a local
    ///     node id into an engine-global one — get it wrong and seat 1's nodes
    ///     address seat 0's chain — and <c>DCA_EQ_BAND_COUNT</c> sizes the band
    ///     array a UI builds. None of them bumps the ABI when it changes, so
    ///     nothing else would notice.
    /// </summary>
    [Theory]
    [InlineData("DCA_MAX_PLAYERS", NativeMethods.MaxPlayers)]
    [InlineData("DCA_PLAYER_NODE_ID_STRIDE", NativeMethods.PlayerNodeIdStride)]
    [InlineData("DCA_EQ_BAND_COUNT", JuceAudioEngine.EqBandCount)]
    public void EveryMirroredHeaderConstant_EqualsTheHeader(string define, int managed)
    {
        var match = Regex.Match(File.ReadAllText(FindHeader()),
            $@"^\s*#define\s+{Regex.Escape(define)}\s+(\d+)", RegexOptions.Multiline);
        Assert.True(match.Success, $"No '#define {define}' in dotcharter_audio.h.");
        Assert.True(int.Parse(match.Groups[1].Value) == managed,
            $"dotcharter_audio.h declares {define} {match.Groups[1].Value} but managed code mirrors {managed}.");
    }

    /// <summary>
    ///     The typed enums are ORDINAL mirrors of header enums; the NAMES are ours,
    ///     the numbers are the ABI. Read out of the header text so a renumbering
    ///     fails here rather than mis-shaping an EQ band or sounding the wrong
    ///     click. Same guarantee <see cref="DcaDriver_OrdinalsMatchManagedEnum" />
    ///     gives the driver enum, for the three that had no managed enum at all.
    /// </summary>
    [Theory]
    [InlineData("DCA_EQ_HIGH_PASS",   (int)JuceEqBandType.HighPass)]
    [InlineData("DCA_EQ_LOW_SHELF",   (int)JuceEqBandType.LowShelf)]
    [InlineData("DCA_EQ_BELL",        (int)JuceEqBandType.Bell)]
    [InlineData("DCA_EQ_HIGH_SHELF",  (int)JuceEqBandType.HighShelf)]
    [InlineData("DCA_EQ_LOW_PASS",    (int)JuceEqBandType.LowPass)]
    [InlineData("DCA_EQ_NOTCH",       (int)JuceEqBandType.Notch)]
    [InlineData("DCA_EQ_ROUTE_SONG",  (int)JuceEqRoute.Song)]
    [InlineData("DCA_EQ_ROUTE_INPUT", (int)JuceEqRoute.Input)]
    [InlineData("DCA_EQ_ROUTE_BOTH",  (int)JuceEqRoute.Both)]
    public void EveryTypedAbiEnumMember_MatchesTheHeadersOrdinal(string enumerator, int managed)
    {
        var match = Regex.Match(File.ReadAllText(FindHeader()),
            $@"^\s*{Regex.Escape(enumerator)}\s*=\s*(\d+)", RegexOptions.Multiline);
        Assert.True(match.Success, $"No '{enumerator} = N' in dotcharter_audio.h.");
        Assert.Equal(int.Parse(match.Groups[1].Value), managed);
    }

    /// <summary>
    ///     The click type and the drop-tune mode are documented in PROSE in the
    ///     header rather than declared as C enums, so they are pinned against that
    ///     prose. <see cref="JuceDropTuneMode" /> matters most: the meaning of the
    ///     same three integers CHANGED at ABI v25 (it used to select a shifter
    ///     engine, Rubber Band vs OLA) and the wrapper's XML docs still described
    ///     the pre-v25 contract until this audit.
    /// </summary>
    [Fact]
    public void DropTuneModeAndClickType_MatchTheHeadersDocumentedOrdinals()
    {
        var header = File.ReadAllText(FindHeader());

        Assert.Matches(new Regex(@"0\s*=\s*Tight",     RegexOptions.IgnoreCase), header);
        Assert.Matches(new Regex(@"1\s*=\s*Balanced",  RegexOptions.IgnoreCase), header);
        Assert.Matches(new Regex(@"2\s*=\s*Smooth",    RegexOptions.IgnoreCase), header);
        Assert.Equal(0, (int)JuceDropTuneMode.Tight);
        Assert.Equal(1, (int)JuceDropTuneMode.Balanced);
        Assert.Equal(2, (int)JuceDropTuneMode.Smooth);

        Assert.Matches(new Regex(@"0\s*=\s*beat", RegexOptions.IgnoreCase), header);
        Assert.Matches(new Regex(@"1\s*=\s*downbeat", RegexOptions.IgnoreCase), header);
        Assert.Matches(new Regex(@"2\s*=\s*clap", RegexOptions.IgnoreCase), header);
        Assert.Equal(0, (int)JuceClickType.Beat);
        Assert.Equal(1, (int)JuceClickType.Downbeat);
        Assert.Equal(2, (int)JuceClickType.Clap);
    }

    /// <summary>Walk up from the test binaries to the sub-repo root — the test
    /// runs from <c>bin/&lt;cfg&gt;/&lt;tfm&gt;</c> in a normal build and from a
    /// different depth in a worktree, so depth is counted, never assumed.</summary>
    /// <summary>
    ///     The automation cap is the FOURTH hand-mirrored native constant, and the
    ///     awkward one: it is not a <c>#define</c> at all. Native it is
    ///     <c>static constexpr int32_t kMaxAutomationEvents</c> in the one ABI
    ///     translation unit, and the header states it only in the prose on
    ///     <c>dca_engine_dsp_process_buffer_automated*</c> — so a change to it
    ///     bumps nothing and notifies nobody. Both sources are read here, because
    ///     the header comment is what a consumer believes and the constexpr is
    ///     what runs; if those two ever disagree the wrapper cannot match both.
    ///     <para>W4-M7: the wrapper needs the number because the engine only
    ///     enforces it AFTER the whole event list has been marshalled into
    ///     unmanaged memory.</para>
    /// </summary>
    [Fact]
    public void TheAutomationEventCap_MatchesTheHeaderProseAndTheEngineConstant()
    {
        var prose = Regex.Match(File.ReadAllText(FindHeader()),
            @"`event_count`\s+is\s+capped\s+at\s+([\d,]+)");
        Assert.True(prose.Success, "dotcharter_audio.h no longer states the event_count cap.");
        Assert.Equal(JuceSharp.Dsp.JuceDspChain.MaxAutomationEvents,
                     int.Parse(prose.Groups[1].Value.Replace(",", "")));

        var native = Regex.Match(File.ReadAllText(FindAbiTranslationUnit()),
            @"kMaxAutomationEvents\s*=\s*(\d+)\s*;");
        Assert.True(native.Success, "dotcharter_audio.cpp no longer defines kMaxAutomationEvents.");
        Assert.Equal(JuceSharp.Dsp.JuceDspChain.MaxAutomationEvents, int.Parse(native.Groups[1].Value));
    }

    /// <summary>
    ///     <b>Every <c>EntryPoint</c> this assembly declares really is in the
    ///     DLL.</b> The wrapper declares 184 entry points and CALLS 163 of them;
    ///     the other 21 — fourteen seat-0 aliases, the superseded MIDI sent-count,
    ///     the two non-player plugin twins, the decode pair and the offline drop
    ///     tuner — are reached by no managed code, so a typo in one of those
    ///     strings compiled perfectly cleanly and would surface as an
    ///     <see cref="EntryPointNotFoundException" /> in a CONSUMER's process, not
    ///     here. Seven of the aliases were added to the header in ABI v35
    ///     specifically because the "every pre-v28 export is an alias for seat 0"
    ///     rule demanded them, and nothing proved they reached the build.
    ///     <para>
    ///         <c>AbiExceptionGuardLintTests</c> proves exports exist in the
    ///         SOURCE; this proves they exist in the BINARY, which is the half
    ///         that an unbuilt translation unit (the five <c>src/abi/abi_*.cpp</c>
    ///         files that sat stale from ~v13 to 2026-08-28) silently breaks.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void EveryDeclaredEntryPoint_IsActuallyExportedByTheDll()
    {
        var declarations = Regex.Matches(
                File.ReadAllText(FindWrapperFile("src/JuceSharp/Interop/NativeMethods.cs")),
                @"EntryPoint\s*=\s*""(dca_[a-z0-9_]+)""")
            .Select(m => m.Groups[1].Value)
            .Distinct(StringComparer.Ordinal)
            .ToArray();

        Assert.True(declarations.Length > 150,
            $"only {declarations.Length} EntryPoint literals found — the regex has stopped matching.");

        var handle = NativeLibrary.Load("juce_sharp", typeof(JuceAudioEngine).Assembly, null);
        try
        {
            var missing = declarations
                .Where(name => !NativeLibrary.TryGetExport(handle, name, out _))
                .ToArray();

            Assert.True(missing.Length == 0,
                "NativeMethods declares entry points the DLL does not export: "
                + string.Join(", ", missing)
                + ". Either the name is a typo or the export was added to a translation unit "
                + "that is not built (there is exactly ONE ABI translation unit).");
        }
        finally { NativeLibrary.Free(handle); }
    }

    /// <summary>Non-vacuity for the sweep above: the probe really can say NO.
    /// Without this, a <c>TryGetExport</c> that always succeeded would make the
    /// whole assertion a tautology.</summary>
    [NativeAvailableFact]
    public void TheExportProbe_RejectsANameTheDllDoesNotHave()
    {
        var handle = NativeLibrary.Load("juce_sharp", typeof(JuceAudioEngine).Assembly, null);
        try
        {
            Assert.True(NativeLibrary.TryGetExport(handle, "dca_version_string", out _));
            Assert.False(NativeLibrary.TryGetExport(handle, "dca_definitely_not_an_export", out _));
        }
        finally { NativeLibrary.Free(handle); }
    }

    private static string FindWrapperFile(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
        }

        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }

    private static string FindAbiTranslationUnit()
    {
        const string relative = "src/JuceSharp.Native/src/dotcharter_audio.cpp";
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, relative);
            if (File.Exists(candidate)) return candidate;
        }

        throw new InvalidOperationException(
            $"Could not find {relative} above {AppContext.BaseDirectory}.");
    }

    private static string FindHeader()
    {
        const string relative = "src/JuceSharp.Native/include/dotcharter_audio.h";
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, relative);
            if (File.Exists(candidate)) return candidate;
        }

        throw new InvalidOperationException(
            $"Could not find {relative} above {AppContext.BaseDirectory}.");
    }

    /// <summary>
    ///     ABI v30's counter struct is the third by-value LAYOUT on this boundary,
    ///     and it is the most fragile of the three: fourteen fields of two widths,
    ///     where inserting an <c>int32_t</c> anywhere above the <c>int64</c> block
    ///     would silently shift ten counters by four bytes and read garbage.
    ///
    ///     <para>
    ///         The size is 96 = 4x4 + 10x8 with NO padding — which is only true
    ///         because the four int32s come first and 16 is already 8-aligned.
    ///         That is a property of the field ORDER, not an accident, so the
    ///         offsets below pin the boundary between the two blocks explicitly.
    ///     </para>
    /// </summary>
    [Fact]
    public void DcaXrunStats_LayoutMatchesTheHeader()
    {
        // 104 = 4x int32 + 11x int64. It was 96 until `input_plugin_faults` was
        // APPENDED (W5-E7): appending at the END is additive by the header's own
        // cbSize contract and needs no ABI bump — an older caller sends a
        // smaller struct_bytes and never sees the tail — which is exactly why
        // this number is allowed to move while the OFFSETS below are not.
        Assert.Equal(104, Marshal.SizeOf<NativeMethods.DcaXrunStats>());

        // The four int32s, in header order.
        Assert.Equal(0,  (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("StructBytes"));
        Assert.Equal(4,  (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("DeviceOpen"));
        Assert.Equal(8,  (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("DeviceRunning"));
        Assert.Equal(12, (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("DeviceXruns"));

        // …then the int64 block, starting immediately with no padding.
        Assert.Equal(16, (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("Callbacks"));
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("LateCallbacks"));
        Assert.Equal(32, (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("DeadlineOverruns"));
        Assert.Equal(40, (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("NearMisses"));
        Assert.Equal(88, (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("BlockBudgetNs"));
        Assert.Equal(96, (int)Marshal.OffsetOf<NativeMethods.DcaXrunStats>("InputPluginFaults"));
    }

    /// <summary>
    ///     The engine reads <c>struct_bytes</c> as a Win32-style cbSize and
    ///     rejects the call when it is too small, so the managed caller MUST send
    ///     the real size. Sending a hard-coded number is how this drifts.
    /// </summary>
    [Fact]
    public void XrunStats_CannotReportIsNullNotZero()
    {
        // -1 is the native sentinel for "this driver cannot report xruns" (WASAPI,
        // CoreAudio aggregate). Folding it to 0 would state the opposite: that the
        // driver looked and saw none.
        var cannotReport = XrunStats.From(new NativeMethods.DcaXrunStats { DeviceXruns = -1 });
        Assert.Null(cannotReport.DeviceXruns);

        var reportedNone = XrunStats.From(new NativeMethods.DcaXrunStats { DeviceXruns = 0 });
        Assert.Equal(0, reportedNone.DeviceXruns);
    }

    /// <summary>Load is undefined before the first block — a budget of 0 must not
    /// divide into an "infinite" load on a device that simply has not started.
    /// </summary>
    [Fact]
    public void XrunStats_LoadIsNullUntilABlockHasBeenRendered()
    {
        Assert.Null(XrunStats.From(new NativeMethods.DcaXrunStats { LastCallbackNs = 500_000 }).Load);

        var running = XrunStats.From(new NativeMethods.DcaXrunStats
        {
            LastCallbackNs = 2_000_000, BlockBudgetNs = 5_333_000
        });
        Assert.NotNull(running.Load);
        Assert.InRange(running.Load!.Value, 0.37, 0.38);
    }

    [Fact]
    public void DcaStatus_HasExpectedMembers()
    {
        Assert.Equal(0, (int)NativeMethods.DcaStatus.Ok);
        Assert.Equal(99, (int)NativeMethods.DcaStatus.Internal);
    }

    [Fact]
    public void DcaDriver_OrdinalsMatchManagedEnum()
    {
        foreach (AudioDriver d in Enum.GetValues<AudioDriver>())
        {
            Assert.Equal((int)d, (int)(NativeMethods.DcaDriver)(int)d);
        }
    }

    [Theory]
    [InlineData(0, 0)]
    [InlineData(1, 1)]
    [InlineData(2, 2)]
    [InlineData(3, 3)]
    [InlineData(4, 4)]
    public void DcaLogLevel_OrdinalsAreStable(int level, int expected)
        => Assert.Equal(expected, (int)(NativeMethods.DcaLogLevel)level);

    [Fact]
    public void DcaDeviceInfo_HasExpectedShape()
    {
        var info = default(NativeMethods.DcaDeviceInfo);
        info.Id = "x"; info.Name = "y"; info.Driver = NativeMethods.DcaDriver.Wasapi;
        info.MaxInputChannels = 1; info.MaxOutputChannels = 2;
        info.DefaultSampleRate = 48000; info.IsDefault = 1;
        Assert.Equal("x", info.Id);
        Assert.Equal("y", info.Name);
        Assert.Equal(NativeMethods.DcaDriver.Wasapi, info.Driver);
        Assert.Equal(1, info.MaxInputChannels);
        Assert.Equal(2, info.MaxOutputChannels);
        Assert.Equal(48000, info.DefaultSampleRate);
        Assert.Equal(1, info.IsDefault);
    }

    [Fact]
    public void DcaEngineConfig_RoundTripsThroughManagedToNative()
    {
        var cfg = new JuceEngineConfig
        {
            SampleRate = 44100,
            BufferFrames = 256,
            OutputChannels = 2,
            InputChannels = 1,
            OutputChannelMask = 3,
            InputChannelMask = 1,
            PreferredDriver = AudioDriver.Wasapi,
            OutputDeviceId = "out",
            InputDeviceId = "in",
        };
        var n = cfg.ToNative();
        Assert.Equal(cfg.SampleRate, n.SampleRate);
        Assert.Equal(cfg.BufferFrames, n.BufferFrames);
        Assert.Equal(cfg.OutputChannels, n.OutputChannels);
        Assert.Equal(cfg.InputChannels, n.InputChannels);
        Assert.Equal(cfg.OutputChannelMask, n.OutputChannelMask);
        Assert.Equal(cfg.InputChannelMask, n.InputChannelMask);
        Assert.Equal((int)cfg.PreferredDriver, (int)n.PreferredDriver);
        Assert.Equal(cfg.OutputDeviceId, n.OutputDeviceId);
        Assert.Equal(cfg.InputDeviceId, n.InputDeviceId);
    }
}
