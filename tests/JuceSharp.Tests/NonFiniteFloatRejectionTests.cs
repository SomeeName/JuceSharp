using System.Reflection;
using System.Runtime.InteropServices;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// A NaN or an infinity crossing this ABI is a caller bug, and every one of them
/// used to be UNRECOVERABLE:
/// <list type="bullet">
/// <item>a non-finite master / monitor / input gain is stored as the ramp's
/// "current" value, so <c>rampOrScale</c> multiplies the block by NaN, the
/// output scrub zeroes the master bus, and the NEXT block ramps FROM the stored
/// NaN — silence for the rest of the session;</item>
/// <item>a non-finite EQ or filter frequency / Q / gain produces NaN biquad
/// coefficients, and a <c>juce::dsp::IIR</c> feeds its own output back, so the
/// band stays dead until an off→on transition resets it;</item>
/// <item>a non-finite pitch reaches Rubber Band through <c>jmax(0.05, x)</c>,
/// which is NaN-transparent in exactly the wrong direction.</item>
/// </list>
/// <para>
/// <c>DspChain::setParam</c> has rejected NaN since the rig shipped, for
/// precisely this reason (every core's clamp is NaN-transparent: <c>v &lt; lo</c>
/// and <c>v &gt; hi</c> are both false). The ENGINE setters did not. They do now,
/// through <c>dcabi::finite</c> (src/AbiArgs.h).
/// </para>
/// <para>
/// The discovery half matters as much as the assertions: a float-taking export
/// added later shows up in <see cref="EveryFloatTakingExport_IsAccountedFor"/>
/// and fails until somebody decides which bucket it belongs in. The recovery
/// half — that the master bus still makes sound after a NaN was published PAST
/// the ABI — lives in the native gate as
/// <c>RtNonFiniteGainCannotLatchTheMasterBus</c>, because managed code cannot
/// see the null device's output buffers.
/// </para>
/// </summary>
[Collection("NullDevice")]
public sealed class NonFiniteFloatRejectionTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    private readonly string? _wavPath;

    public NonFiniteFloatRejectionTests()
    {
        if (NativeProbe.IsAvailable) _wavPath = TinyWav.Create(seconds: 2.0);
    }

    public void Dispose()
    {
        _scope.Dispose();
        if (_wavPath is not null) TinyWav.TryDelete(_wavPath);
    }

    private static readonly float[] BadFloats =
        [float.NaN, float.PositiveInfinity, float.NegativeInfinity];

    private static readonly double[] BadDoubles =
        [double.NaN, double.PositiveInfinity, double.NegativeInfinity];

    /// <summary>
    /// Engine-scoped setters, driven against a LIVE engine — a null handle
    /// rejects anyway, which would make the whole test vacuous. Each is also
    /// driven with a legal value first, so the handle is proven good.
    /// </summary>
    [NativeAvailableFact]
    public void EveryEngineFloatSetter_RejectsNonFinite_AndStillAcceptsFinite()
    {
        using var engine = NullDevice.CreateEngine(inputChannels: 2);
        var h = engine.NativeHandle;
        Assert.NotEqual(nint.Zero, h);

        AssertFloat1(x => NativeMethods.EngineSetMasterVolume(h, x), 0.5f, "set_master_volume");
        AssertFloat1(x => NativeMethods.EngineSetMonitorGain(h, x), 0.5f, "set_monitor_gain");
        AssertFloat1(x => NativeMethods.EngineSetInputGain(h, x), 0.5f, "set_input_gain");
        AssertFloat1(x => NativeMethods.EngineSetInstrumentGain(h, x), 0.5f, "set_instrument_gain");
        AssertFloat1(x => NativeMethods.EngineSetInputPitchShift(h, x), -2.0f, "set_input_pitch_shift");
        AssertFloat1(x => NativeMethods.EngineSetPlayerInputPitchShift(h, 0, x), -2.0f,
                     "set_player_input_pitch_shift");
        AssertFloat1(x => NativeMethods.EngineSetPlayerMonitorGain(h, 0, x), 0.5f,
                     "set_player_monitor_gain");
        AssertFloat1(x => NativeMethods.EngineSetEqTrim(h, x), -3.0f, "set_eq_trim");

        // ABI v34 master-bus ceiling: poison each of its two floats in turn, so
        // a guard on only the first is caught. A NaN CEILING would be the worst
        // of the family — every sample is compared against it and every
        // comparison is false, so the limiter goes transparent while the meter
        // claims otherwise; a NaN RELEASE poisons the smoothing coefficient.
        foreach (var bad in BadFloats)
        {
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineSetMasterLimiter(h, 1, bad, 80f));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineSetMasterLimiter(h, 1, -0.3f, bad));
        }
        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.EngineSetMasterLimiter(h, 1, -0.3f, 80f));
        AssertFloat1(x => NativeMethods.EngineSetStemGroupTempo(h, x), 1.0f, "set_stem_group_tempo");
        AssertFloat1(x => NativeMethods.EngineSetStemGroupPitch(h, x), 0.0f, "set_stem_group_pitch");

        // Two-float filters: poison each argument in turn, so a guard on only
        // the first one is caught.
        AssertFloat2((a, b) => NativeMethods.EngineSetHighPass(h, a, b), 80f, 0.707f, "set_high_pass");
        AssertFloat2((a, b) => NativeMethods.EngineSetLowPass(h, a, b), 8000f, 0.707f, "set_low_pass");
        AssertFloat2((a, b) => NativeMethods.EngineSetBandPass(h, a, b), 1000f, 0.707f, "set_band_pass");

        // The v29 EQ band takes three.
        foreach (var bad in BadFloats)
        {
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineSetEqBand(h, 0, 2, bad, 0.707f, 3.0f, 1));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineSetEqBand(h, 0, 2, 1000f, bad, 3.0f, 1));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineSetEqBand(h, 0, 2, 1000f, 0.707f, bad, 1));
        }
        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.EngineSetEqBand(h, 0, 2, 1000f, 0.707f, 3.0f, 1));

        // ABI v38 chart source: a float gain AND a double offset — poison each in
        // turn. A NaN gain would ride the ramp into a seat's rig input.
        foreach (var bad in BadFloats)
        {
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineChartSourceSetParamsForPlayer(h, 0, bad, 0.0, 0));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineChartSourceSetParamsForPlayer(h, 0, 1.0f, bad, 0));
        }
        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.EngineChartSourceSetParamsForPlayer(h, 0, 1.0f, 0.0, 0));
    }

    /// <summary>
    /// The source-scoped half. Same shape, on a real transport: a NULL source
    /// rejects on its own, so this needs a live one to mean anything.
    /// </summary>
    [NativeAvailableFact]
    public void EverySourceFloatSetter_RejectsNonFinite_AndStillAcceptsFinite()
    {
        using var engine = NullDevice.CreateEngine();
        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.SourceOpenFile(engine.NativeHandle, _wavPath!, out var s));
        Assert.NotEqual(nint.Zero, s);
        try
        {
            AssertFloat1(x => NativeMethods.SourceSetVolume(s, x), 0.5f, "source_set_volume");
            AssertFloat1(x => NativeMethods.SourceSetPan(s, x), 0.25f, "source_set_pan");
            AssertFloat1(x => NativeMethods.SourceSetTempo(s, x), 1.0f, "source_set_tempo");
            AssertFloat1(x => NativeMethods.SourceSetPitchSemitones(s, x), 0.0f,
                         "source_set_pitch_semitones");

            foreach (var bad in BadDoubles)
                Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.SourceSeek(s, bad));
            Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.SourceSeek(s, 0.5));
        }
        finally
        {
            NativeMethods.SourceDestroy(s);
        }
    }

    /// <summary>
    /// <c>dca_engine_dsp_set_param</c> already rejected NaN inside
    /// <c>DspChain::setParam</c>; the ABI guard adds the infinities so the whole
    /// surface answers alike. Driven with no device — the offline rig path.
    /// </summary>
    [NativeAvailableFact]
    public void DspSetParam_RejectsEveryNonFiniteValue()
    {
        using var engine = NullDevice.CreateEngine();
        var h = engine.NativeHandle;
        var node = DspAddNode(h, "gain");
        Assert.True(node >= 0, "could not add a 'gain' node to seat 0");
        try
        {
            foreach (var bad in BadFloats)
                Assert.Equal((int) NativeMethods.DcaStatus.InvalidArg, DspSetParam(h, node, "gainDb", bad));
            Assert.Equal((int) NativeMethods.DcaStatus.Ok, DspSetParam(h, node, "gainDb", 3.0f));
        }
        finally
        {
            DspRemoveNode(h, node);
        }
    }

    /// <summary>The two offline waveform exports take a <c>block_ms</c> that is
    /// only guarded by <c>&lt;= 0</c>, which NaN passes.</summary>
    [NativeAvailableFact]
    public void GenerateWaveform_RejectsANonFiniteBlockMs()
    {
        var levels = new float[64];
        foreach (var bad in BadDoubles)
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.GenerateWaveform(_wavPath!, bad, levels, levels.Length, out _));

        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.GenerateWaveform(_wavPath!, 20.0, levels, levels.Length, out var count));
        Assert.True(count > 0, "the finite control call produced no blocks — the rejections above prove nothing");
    }

    /// <summary>ABI v43: <c>dca_convert_audio_for_device</c>'s <c>device_rate</c>. A NaN
    /// passes a bare <c>&lt;= 0</c> check and would reach the WAV header.</summary>
    [NativeAvailableFact]
    public void ConvertAudioForDevice_RejectsANonFiniteDeviceRate()
    {
        var dst = Path.Combine(Path.GetTempPath(), $"jucesharp-nonfinite-{Guid.NewGuid():N}.wav");
        try
        {
            foreach (var bad in BadDoubles)
                Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                    NativeMethods.ConvertAudioForDevice(_wavPath!, dst, bad, out _));

            // Finite control: a rate at or below the file's converts nothing -- DCA_OK, no file.
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.ConvertAudioForDevice(_wavPath!, dst, 8_000, out var bits));
            Assert.Equal(0, bits);
            Assert.False(File.Exists(dst));
        }
        finally { File.Delete(dst); }
    }

    /// <summary>
    /// Discovery: every by-value <c>float</c>/<c>double</c> parameter declared in
    /// <see cref="NativeMethods"/> is either exercised above or listed here with
    /// a reason. A new float-taking export therefore cannot ship unguarded and
    /// unnoticed — this test names it.
    /// </summary>
    [Fact]
    public void EveryFloatTakingExport_IsAccountedFor()
    {
        // Exercised by the tests above (managed method names, since that is what
        // reflection sees).
        var covered = new HashSet<string>(StringComparer.Ordinal)
        {
            "EngineSetMasterVolume", "EngineSetMonitorGain", "EngineSetInputGain",
            "EngineSetInstrumentGain", "EngineSetInputPitchShift",
            "EngineSetPlayerInputPitchShift", "EngineSetPlayerMonitorGain",
            "EngineSetEqTrim", "EngineSetStemGroupTempo", "EngineSetStemGroupPitch",
            "EngineSetHighPass", "EngineSetLowPass", "EngineSetBandPass",
            "EngineSetEqBand", "EngineSetMasterLimiter",
            "EngineChartSourceSetParamsForPlayer",
            "SourceSetVolume", "SourceSetPan", "SourceSetTempo", "SourceSetPitchSemitones",
            "SourceSeek",
            "GenerateWaveform",
            "ConvertAudioForDevice",
        };

        // Deliberately NOT driven from managed code, each for a stated reason.
        // They are still guarded natively — this list is about the TEST, not the
        // code.
        var excused = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["GenerateWaveformFromMemory"] =
                "same guard and same line as GenerateWaveform, on a memory blob instead of a path",
            ["PluginSetParameter"] =
                "needs a real VST3 instance; guarded natively because std::clamp(NaN,0,1) returns NaN",
            ["RenderToWav"] =
                "already validated non-finite tempo/semitones before this pass (p2-18)",
            ["EngineDspSetParam"] =
                "dca_engine_dsp_set_param checks dcabi::finite(value) and DspChain::setParam "
                + "rejects NaN of its own accord; the managed side is pinned by "
                + "JuceDspChainWrapperTests (NaN param rejected) which needs a live node",
            ["DropTuneProcessOffline"] =
                "offline tooling export: dca_droptune_process_offline checks dcabi::finite(max_lag_ms) "
                + "and the semitone segments are clamped by the core; not a live setter, needs a primed ring",
        };

        var offenders = new List<string>();
        foreach (var m in typeof(NativeMethods).GetMethods(BindingFlags.NonPublic | BindingFlags.Public
                                                           | BindingFlags.Static))
        {
            if (m.GetCustomAttribute<DllImportAttribute>() is null) continue;
            var takesFloat = m.GetParameters().Any(p =>
                !p.IsOut && !p.ParameterType.IsByRef && !p.ParameterType.IsPointer
                && !p.ParameterType.IsArray
                && (p.ParameterType == typeof(float) || p.ParameterType == typeof(double)));
            if (!takesFloat) continue;
            if (covered.Contains(m.Name) || excused.ContainsKey(m.Name)) continue;
            offenders.Add(m.Name);
        }

        Assert.True(offenders.Count == 0,
            "these exports take a by-value float/double and are neither exercised by "
            + "NonFiniteFloatRejectionTests nor excused in it — add a case or a reason: "
            + string.Join(", ", offenders.Order()));

        // Non-vacuity: the discovery walk must actually find the ones we know
        // about, or a change to the reflection filter would silently pass.
        Assert.Contains("EngineSetMasterVolume", DiscoverFloatTakers());
        Assert.True(DiscoverFloatTakers().Count >= 20,
            $"only {DiscoverFloatTakers().Count} float-taking exports were discovered — "
            + "the reflection filter has stopped matching");
    }

    private static List<string> DiscoverFloatTakers()
        => typeof(NativeMethods)
            .GetMethods(BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Static)
            .Where(m => m.GetCustomAttribute<DllImportAttribute>() is not null)
            .Where(m => m.GetParameters().Any(p =>
                !p.IsOut && !p.ParameterType.IsByRef && !p.ParameterType.IsPointer
                && !p.ParameterType.IsArray
                && (p.ParameterType == typeof(float) || p.ParameterType == typeof(double))))
            .Select(m => m.Name)
            .ToList();

    private static void AssertFloat1(Func<float, NativeMethods.DcaStatus> call, float good, string what)
    {
        foreach (var bad in BadFloats)
            Assert.True(call(bad) == NativeMethods.DcaStatus.InvalidArg,
                $"dca_engine_{what} accepted {bad}");
        Assert.True(call(good) == NativeMethods.DcaStatus.Ok,
            $"dca_engine_{what} rejected the legal value {good} — the handle is dead and the "
            + "rejections above prove nothing");
    }

    private static void AssertFloat2(Func<float, float, NativeMethods.DcaStatus> call,
                                     float goodA, float goodB, string what)
    {
        foreach (var bad in BadFloats)
        {
            Assert.True(call(bad, goodB) == NativeMethods.DcaStatus.InvalidArg,
                $"dca_engine_{what} accepted {bad} as its first argument");
            Assert.True(call(goodA, bad) == NativeMethods.DcaStatus.InvalidArg,
                $"dca_engine_{what} accepted {bad} as its second argument");
        }
        Assert.True(call(goodA, goodB) == NativeMethods.DcaStatus.Ok,
            $"dca_engine_{what} rejected a legal pair — the rejections above prove nothing");
    }

    [DllImport("juce_sharp", EntryPoint = "dca_engine_dsp_add_node",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAddNode(nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport("juce_sharp", EntryPoint = "dca_engine_dsp_set_param",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, float value);

    [DllImport("juce_sharp", EntryPoint = "dca_engine_dsp_remove_node",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspRemoveNode(nint engine, int nodeId);
}
