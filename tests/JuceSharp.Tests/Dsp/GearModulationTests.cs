using System.Globalization;
using System.Runtime.InteropServices;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// The five modules of the 2026-09 <c>modulation</c> gear wave — <c>tzflanger</c>,
/// <c>barberpole</c>, <c>autopan</c>, <c>ensemble</c> and <c>freqshift</c> — held
/// to the rig's standing rules plus the one claim each of them makes that nothing
/// else in the catalogue makes.
///
/// <para>Everything here renders OFFLINE on a stopped engine through
/// <c>dca_engine_dsp_process_buffer</c>, exactly like
/// <see cref="DspGainZipperTests" /> and <see cref="DspParamSeamTests" />: one
/// node, a stimulus that is a pure function of the absolute sample index, and
/// assertions on the samples. The offline export re-prepares the rig when the
/// requested rate differs from the prepared one, which is what makes the
/// 48 kHz/192 kHz theory a real test of <c>prepare()</c> rather than of
/// arithmetic.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearModulationTests : IDisposable
{
    private const string Lib = "juce_sharp";

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_add_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAddNode(nint engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_clear_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspClearFor(nint engine, int player);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, float value);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_get_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspGetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, out float value);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_get_node_latency", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspGetNodeLatency(nint engine, int nodeId, out int samples);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_process_buffer", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspProcess(nint engine, float[] left, float[] right, int frames, int sampleRate);

    [DllImport(Lib, EntryPoint = "dca_dsp_module_type_count", CallingConvention = CallingConvention.Cdecl)]
    private static extern int ModuleTypeCount();

    [DllImport(Lib, EntryPoint = "dca_dsp_module_type_category", CallingConvention = CallingConvention.Cdecl)]
    private static extern int ModuleTypeCategory(int index, byte[] outCategory, int cap, out int len);

    [DllImport(Lib, EntryPoint = "dca_dsp_module_type_info", CallingConvention = CallingConvention.Cdecl)]
    private static extern int ModuleTypeInfo(int index, byte[] outId, int idCap, byte[] outName, int nameCap);

    [DllImport(Lib, EntryPoint = "dca_dsp_module_param_count", CallingConvention = CallingConvention.Cdecl)]
    private static extern int ModuleParamCount([MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    private const int Ok = 0;
    private const int Frames = 2048;
    private const int SampleRate = 48000;

    /// <summary>Blocks of audio before a seam: settles smoothers, fills lines.</summary>
    private const int PreRoll = 4;
    /// <summary>Samples of the seam block that must still be near the OLD value.</summary>
    private const int EarlyWindow = 128;
    /// <summary>Where a 15 ms ramp is long finished.</summary>
    private const int LateStart = 1200;
    /// <summary>The window a sample-to-sample step is hunted in.</summary>
    private const int SeamWindow = 900;

    /// <summary>Every type id this file owns.</summary>
    public static readonly string[] Modules =
        ["tzflanger", "barberpole", "autopan", "ensemble", "freqshift"];

    private readonly JuceAudioEngine? _engine;

    public GearModulationTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose()
    {
        if (_engine is null) return;
        DspClearFor(_engine.NativeHandle, 0);
        _engine.Dispose();
    }

    private nint H => _engine!.NativeHandle;

    // ======================================================================
    // Catalogue
    // ======================================================================

    /// <summary>
    /// A module nobody can find is a dead feature. Every one of the five is in
    /// the native catalogue, under the <c>modulation</c> category — which is a
    /// FIXED vocabulary the engine owns (ABI v33), so a typo here would make the
    /// pedalboard drawer lose the pedal rather than fail a build.
    /// </summary>
    [NativeAvailableTheory]
    [MemberData(nameof(ModuleRows))]
    public void EveryModule_IsInTheCatalogueUnderModulation(string module)
    {
        var found = false;
        for (var i = 0; i < ModuleTypeCount(); i++)
        {
            var id = new byte[128];
            var name = new byte[256];
            Assert.Equal(Ok, ModuleTypeInfo(i, id, id.Length, name, name.Length));
            if (Cstr(id) != module) continue;
            found = true;
            Assert.True(ModuleParamCount(module) >= 4, $"{module}: only {ModuleParamCount(module)} params");
            var cat = new byte[64];
            Assert.Equal(Ok, ModuleTypeCategory(i, cat, cat.Length, out _));
            Assert.Equal("modulation", Cstr(cat));
            Assert.False(string.IsNullOrWhiteSpace(Cstr(name)), $"{module}: no display name");
        }

        Assert.True(found, $"{module} is not in the native catalogue");
    }

    public static TheoryData<string> ModuleRows
    {
        get
        {
            var d = new TheoryData<string>();
            foreach (var m in Modules) d.Add(m);
            return d;
        }
    }

    // ======================================================================
    // Param contract: read-back, clamping, NaN
    // ======================================================================

    /// <summary>(module, param, value) — <c>getParam</c> answers what
    /// <c>setParam</c> stored, for every id each module declares.</summary>
    public static TheoryData<string, string, float> ReadBackCases => new()
    {
        { "tzflanger", "rateHz", 4.5f }, { "tzflanger", "depthMs", 7.25f },
        { "tzflanger", "manualMs", 2.5f }, { "tzflanger", "feedback", -0.4f },
        { "tzflanger", "mix", 0.75f }, { "tzflanger", "shape", 1f },
        { "barberpole", "rateHz", 3.5f }, { "barberpole", "stages", 10f },
        { "barberpole", "depth", 0.25f }, { "barberpole", "feedback", -0.6f },
        { "barberpole", "direction", 1f }, { "barberpole", "mix", 0.9f },
        { "autopan", "rateHz", 7f }, { "autopan", "depth", 0.3f },
        { "autopan", "shape", 2f }, { "autopan", "phaseDeg", 45f }, { "autopan", "law", 1f },
        { "ensemble", "rateHz", 1.5f }, { "ensemble", "fastRateHz", 7f },
        { "ensemble", "depth", 0.9f }, { "ensemble", "voices", 2f },
        { "ensemble", "tone", 0.2f }, { "ensemble", "mix", 0.4f },
        { "freqshift", "shiftHz", -250f }, { "freqshift", "mix", 0.8f },
        { "freqshift", "feedback", 0.5f }, { "freqshift", "direction", 2f },
    };

    [NativeAvailableTheory]
    [MemberData(nameof(ReadBackCases))]
    public void GetParam_ReadsBackWhatSetParamStored(string module, string param, float value)
    {
        var n = Fresh(module);
        Assert.Equal(Ok, DspSetParam(H, n, param, value));
        Assert.Equal(Ok, DspGetParam(H, n, param, out var read));
        Assert.Equal(value, read, 4);
    }

    /// <summary>
    /// A NaN is REJECTED and changes nothing. The chain rejects one before the
    /// module sees it, and every adapter here rejects it again — the second
    /// guard is the one that matters for a direct <c>DspModule</c> caller, and
    /// the assertion that survives both is that the audio is bit-identical
    /// across the attempt.
    /// </summary>
    [NativeAvailableTheory]
    [MemberData(nameof(ReadBackCases))]
    public void ANaNParam_IsRejected_AndTheModuleGoesOnRendering(string module, string param, float value)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh(module);
        Assert.Equal(Ok, DspSetParam(H, n, param, value));
        var before = RenderSine(Blocks(3), SampleRate);

        Assert.NotEqual(Ok, DspSetParam(H, n, param, float.NaN));
        Assert.Equal(Ok, DspGetParam(H, n, param, out var read));
        Assert.Equal(value, read, 4);

        // A NaN that reached a coefficient is PERMANENT, so the tell is the
        // NEXT render and not the return code.
        DspClearFor(H, 0);
        var n2 = Fresh(module);
        Assert.Equal(Ok, DspSetParam(H, n2, param, value));
        Assert.NotEqual(Ok, DspSetParam(H, n2, param, float.NaN));
        var after = RenderSine(Blocks(3), SampleRate);
        for (var i = 0; i < after.L.Length; i++)
            Assert.True(float.IsFinite(after.L[i]) && float.IsFinite(after.R[i]),
                $"{module}.{param}: a rejected NaN still poisoned sample {i}");
        Assert.True(RmsDiff(before.L, after.L, 0, before.L.Length) < 1e-6f,
            $"{module}.{param}: a rejected NaN changed the audio");
    }

    /// <summary>(module, param, absurd value) — a value far outside the declared
    /// range is CLAMPED, and the module stays bounded and finite. A clamp written
    /// as <c>v &lt; lo ? lo : …</c> is NaN-transparent, which is why every core
    /// here tests <c>isfinite</c> first.</summary>
    public static TheoryData<string, string, float> ClampCases => new()
    {
        { "tzflanger", "rateHz", 1e6f }, { "tzflanger", "depthMs", 1e6f },
        { "tzflanger", "manualMs", -1e6f }, { "tzflanger", "feedback", 40f },
        { "tzflanger", "mix", -5f }, { "tzflanger", "shape", 99f },
        { "barberpole", "rateHz", -1e6f }, { "barberpole", "stages", 99f },
        { "barberpole", "depth", 1e6f }, { "barberpole", "feedback", -40f },
        { "barberpole", "direction", 99f },
        { "autopan", "rateHz", 1e6f }, { "autopan", "depth", -7f },
        { "autopan", "shape", -3f }, { "autopan", "phaseDeg", 1e5f }, { "autopan", "law", 42f },
        { "ensemble", "rateHz", 1e6f }, { "ensemble", "fastRateHz", -1e6f },
        { "ensemble", "depth", 9f }, { "ensemble", "voices", -9f }, { "ensemble", "tone", 9f },
        { "freqshift", "shiftHz", 1e6f }, { "freqshift", "feedback", 9f },
        { "freqshift", "direction", -9f },
    };

    [NativeAvailableTheory]
    [MemberData(nameof(ClampCases))]
    public void AnOutOfRangeParam_IsClamped_AndTheOutputStaysBounded(string module, string param, float value)
    {
        var n = Fresh(module);
        Assert.Equal(Ok, DspSetParam(H, n, param, value));
        var r = RenderSine(Blocks(4), SampleRate);
        for (var i = 0; i < r.L.Length; i++)
        {
            Assert.True(float.IsFinite(r.L[i]) && float.IsFinite(r.R[i]),
                $"{module}.{param}={value}: non-finite sample at {i}");
            Assert.True(MathF.Abs(r.L[i]) < 4f && MathF.Abs(r.R[i]) < 4f,
                $"{module}.{param}={value}: |sample| {MathF.Abs(r.L[i]):F2} at {i} - a clamp let the core run away");
        }
    }

    // ======================================================================
    // Ramps and seams
    // ======================================================================

    /// <summary>(module, setup, param, before, after) — a mix or level that
    /// moves must WALK across the seam. Three renders of the same stimulus:
    /// never moved, moved from the start, and moved at the seam. See
    /// <see cref="DspGainZipperTests" />'s <c>MixCases</c>, whose shape and
    /// windows this follows exactly.</summary>
    public static TheoryData<string, string, string, float, float> MixCases => new()
    {
        { "tzflanger", "rateHz=1;depthMs=5;manualMs=5;feedback=0",       "mix",   0f, 1f },
        { "barberpole", "rateHz=1;depth=1;feedback=0;stages=12",         "mix",   0f, 1f },
        { "autopan",   "rateHz=1;phaseDeg=180;law=0;shape=0",            "depth", 0f, 1f },
        { "ensemble",  "rateHz=2;fastRateHz=8;depth=1;voices=3;tone=0.5","mix",   0f, 1f },
        { "freqshift", "shiftHz=200;feedback=0;direction=0",             "mix",   0f, 1f },
    };

    [NativeAvailableTheory]
    [MemberData(nameof(MixCases))]
    public void ChangingAMixOrLevel_WalksAcrossTheSeam_InsteadOfSwitching(
        string module, string setup, string param, float before, float after)
    {
        var refBefore = RenderSeam(module, setup, param, before, before);
        var refAfter = RenderSeam(module, setup, param, after, after);
        var seam = RenderSeam(module, setup, param, before, after);

        // Every metric is taken on BOTH channels: a case is vacuous only when
        // NEITHER channel can tell the two settings apart, and the ratios are
        // asserted on every channel that can.
        var spanSeen = false;
        foreach (var ch in new[] { 0, 1 })
        {
            var rb = ch == 0 ? refBefore.L : refBefore.R;
            var ra = ch == 0 ? refAfter.L : refAfter.R;
            var sm = ch == 0 ? seam.L : seam.R;
            var earlySpan = RmsDiff(rb, ra, 0, EarlyWindow);
            var lateSpan = RmsDiff(rb, ra, LateStart, Frames - LateStart);
            if (earlySpan <= 0.005f || lateSpan <= 0.005f) continue;
            spanSeen = true;

            var earlyRatio = RmsDiff(sm, rb, 0, EarlyWindow) / earlySpan;
            Assert.True(earlyRatio < 0.5f,
                $"{module}.{param} (channel {ch}): the first {EarlyWindow} samples after the "
                + $"change are {earlyRatio * 100f:F0} % of the way to the new setting - a ramp "
                + "is well under half, a hard switch is 100 %");

            var lateRatio = RmsDiff(sm, ra, LateStart, Frames - LateStart) / lateSpan;
            Assert.True(lateRatio < 0.15f,
                $"{module}.{param} (channel {ch}): {Frames - LateStart} samples later the seam "
                + $"is still {lateRatio * 100f:F0} % away from the new setting - the ramp never landed");
        }
        Assert.True(spanSeen,
            $"{module}.{param}: neither channel can tell the two settings apart - the case is vacuous");
    }

    /// <summary>(module, setup, param, before, after) — a parameter that moves a
    /// MODULATOR, a read tap, a stage count or a sideband sign may not put a STEP
    /// into the output. The probe is the artefact itself: the largest
    /// sample-to-sample step in the window after the change, against the same
    /// signal's own largest step once settled (<see cref="DspParamSeamTests" />).</summary>
    public static TheoryData<string, string, string, float, float> SeamCases => new()
    {
        // A depth is a DELAY LENGTH here: 0.1 -> 10 ms is 475 samples at 48 kHz.
        { "tzflanger", "rateHz=1;manualMs=5;mix=1;feedback=0",  "depthMs",  0.1f, 10f },
        { "tzflanger", "rateHz=1;manualMs=5;mix=1;feedback=0",  "manualMs", 1f,   9f },
        // Sine vs triangle differ by up to 1.0 at one phase, and the LFO IS the tap.
        { "tzflanger", "rateHz=2;depthMs=8;manualMs=8;mix=1",   "shape",    0f,   1f },
        { "barberpole", "rateHz=1;depth=1;mix=1;feedback=0.3",  "stages",   2f,   12f },
        { "barberpole", "depth=1;mix=1;feedback=0",             "rateHz",   0.05f, 5f },
        { "barberpole", "rateHz=2;depth=1;mix=1",               "direction", 0f,  1f },
        // A raw square is a full-scale step twice a cycle; the slew is the fix.
        { "autopan",   "rateHz=2;depth=1;phaseDeg=180",         "shape",    0f,   2f },
        { "autopan",   "rateHz=2;depth=1;shape=0",              "phaseDeg", 0f,   180f },
        { "autopan",   "depth=1;shape=0;phaseDeg=180",          "rateHz",   0.05f, 10f },
        // Equal Power and Linear differ by 3.0 dB at pos 0 (0.7071 against 0.5).
        // That is a PER-SAMPLE GAIN, so switching it raw is a step however
        // smooth the curve is - and until 2026-09-12 the Linear law was never
        // RENDERED by any test in the repo, only read back and clamped.
        { "autopan",   "rateHz=2;depth=1;phaseDeg=180;shape=0",  "law",      0f,   1f },
        // A voice arriving at full level is a new delay tap at full weight.
        { "ensemble",  "rateHz=2;fastRateHz=8;depth=1;mix=1",   "voices",   2f,   3f },
        { "ensemble",  "fastRateHz=8;depth=1;mix=1;voices=3",   "rateHz",   0.1f, 2f },
        // The carrier's PHASE is continuous, so a shift jump is a glide.
        { "freqshift", "mix=1;feedback=0;direction=0",          "shiftHz",  0f,   200f },
        // Up -> Down is a SIGN CHANGE on the quadrature term.
        { "freqshift", "mix=1;feedback=0;shiftHz=300",          "direction", 0f,  1f },
    };

    [NativeAvailableTheory]
    [MemberData(nameof(SeamCases))]
    public void ChangingAModulator_DoesNotStepTheOutput(
        string module, string setup, string param, float before, float after)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh(module);
        ApplyAll(n, setup);
        Assert.Equal(Ok, DspSetParam(H, n, param, before));

        var pre = RenderSine(Blocks(PreRoll + 1), SampleRate);

        Assert.Equal(Ok, DspSetParam(H, n, param, after));
        var post = ContinueSine(pre.L.Length, Blocks(2), SampleRate);

        // The settled slope is measured on BOTH sides of the change, because
        // several of these parameters legitimately change how fast the output
        // itself moves - freqshift's direction flips a 300 Hz shift from 140 Hz
        // to 740 Hz, whose own slope is 8x the one before it. Comparing a seam
        // against only the OLD state's slope reports that as a splice.
        // BOTH channels. Reading .L alone made autopan.phaseDeg and
        // ensemble.voices vacuous - see RenderSeam's remarks.
        var channelSeen = false;
        foreach (var ch in new[] { 0, 1 })
        {
            var p0 = ch == 0 ? pre.L : pre.R;
            var p1 = ch == 0 ? post.L : post.R;
            var natural = MathF.Max(MaxStep(p0, Frames, p0.Length - Frames),
                                    MaxStep(p1, Frames + 1, Frames - 1));
            if (natural <= 1e-4f) continue;     // silent here; the other channel must not be
            channelSeen = true;

            var step = MathF.Abs(p1[0] - p0[^1]);
            for (var i = 1; i < SeamWindow; i++)
                step = MathF.Max(step, MathF.Abs(p1[i] - p1[i - 1]));

            var allowed = 3f * natural;
            Assert.True(step < allowed,
                $"{module}.{param} (channel {ch}): {before} -> {after} stepped the output by "
                + $"{step:F4} against a settled slope of {natural:F4} (allowed {allowed:F4}) "
                + "- that is a splice, not a fade");
        }
        Assert.True(channelSeen, $"{module}.{param}: the probe signal is silent on both channels");
    }

    /// <summary>(module, setup, param, a, b) — a switch written TWICE, the
    /// second write landing 3 ms into the 10 ms crossfade the first one started.
    ///
    /// <para>A crossfade that restarts at 0 on the second write discards the
    /// blend position it is currently emitting: at t = 0.3 the output goes from
    /// <c>0.7*a + 0.3*b</c> to the whole of <c>a</c> in one sample, which is the
    /// click the crossfade exists to remove. <c>tzflanger.shape</c> is the
    /// reference implementation (it swaps the two ends and continues from
    /// 1 - t) and is here as a CONTROL; <c>barberpole.stages</c> and
    /// <c>autopan.law</c> did not, and <c>barberpole</c> had a second half —
    /// with <c>stagesPrev_ == stagesCur_</c> after the flip-back its retirement
    /// block never fired, so sections above the live count kept pre-flip state
    /// for a later grow to re-inject.</para></summary>
    public static TheoryData<string, string, string, float, float> FlipBackCases => new()
    {
        { "tzflanger", "rateHz=2;depthMs=8;manualMs=8;mix=1",     "shape",  0f, 1f },
        { "barberpole", "rateHz=5;depth=1;mix=1;feedback=0.9",    "stages", 2f, 12f },
        { "autopan",   "rateHz=2;depth=1;phaseDeg=180;shape=0",   "law",    0f, 1f },
        { "autopan",   "rateHz=2;depth=1;phaseDeg=180;law=0",     "shape",  0f, 2f },
    };

    /// <summary>3 ms at 48 kHz — inside every 10 ms crossfade in the drawer.</summary>
    private const int InsideFade = 144;

    [NativeAvailableTheory]
    [MemberData(nameof(FlipBackCases))]
    public void FlippingASwitchBackInsideItsCrossfade_KeepsTheBlendPosition(
        string module, string setup, string param, float a, float b)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh(module);
        ApplyAll(n, setup);
        Assert.Equal(Ok, DspSetParam(H, n, param, a));

        var settled = RenderSine(Blocks(PreRoll + 1), SampleRate);
        var at = settled.L.Length;

        Assert.Equal(Ok, DspSetParam(H, n, param, b));
        var mid = ContinueSine(at, InsideFade, SampleRate);
        at += InsideFade;

        Assert.Equal(Ok, DspSetParam(H, n, param, a));           // back, mid-fade
        var post = ContinueSine(at, Frames, SampleRate);

        var channelSeen = false;
        foreach (var ch in new[] { 0, 1 })
        {
            var s0 = ch == 0 ? settled.L : settled.R;
            var m = ch == 0 ? mid.L : mid.R;
            var q = ch == 0 ? post.L : post.R;
            var natural = MathF.Max(MaxStep(s0, Frames, s0.Length - Frames),
                                    MaxStep(q, Frames / 2, Frames / 2));
            if (natural <= 1e-4f) continue;
            channelSeen = true;

            var step = MathF.Abs(q[0] - m[^1]);
            for (var i = 1; i < SeamWindow; i++)
                step = MathF.Max(step, MathF.Abs(q[i] - q[i - 1]));

            var allowed = 3f * natural;
            Assert.True(step < allowed,
                $"{module}.{param}: {a} -> {b} -> {a} 3 ms apart stepped the output by "
                + $"{step:F4} on channel {ch} against a settled slope of {natural:F4} "
                + $"(allowed {allowed:F4}) - the second write threw away the blend in flight");
        }
        Assert.True(channelSeen, $"{module}.{param}: the probe signal is silent on both channels");
    }

    /// <summary>Under the Linear pan law the two channel gains SUM to one at
    /// every position, which is what "linear" means and what distinguishes it
    /// from Equal Power (whose SQUARES sum to one). Nothing rendered
    /// <c>law = 1</c> at all before 2026-09-12 — it appeared only in a
    /// <c>getParam</c> read-back and a clamp case — so neither its arithmetic
    /// nor its seam was covered.</summary>
    [NativeAvailableFact]
    public void AutoPan_TheLinearLaw_SumsToUnityAcrossTheSweep()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh("autopan");
        ApplyAll(n, "rateHz=4;depth=1;phaseDeg=180;shape=0;law=1");
        RenderDc(Blocks(PreRoll + 1), SampleRate);              // settle every ramp
        var y = ContinueDc(Blocks(PreRoll + 1), Blocks(4), SampleRate);

        var worst = 0f;
        var spread = 0f;
        for (var i = 0; i < y.L.Length; i++)
        {
            worst = MathF.Max(worst, MathF.Abs(y.L[i] + y.R[i] - DcLevel));
            spread = MathF.Max(spread, MathF.Abs(y.L[i] - y.R[i]));
        }
        Assert.True(spread > 0.5f * DcLevel,
            $"the panner barely moved (spread {spread:F4}) - the case is vacuous");
        Assert.True(worst < 1e-4f * DcLevel + 1e-5f,
            $"the Linear law's two gains summed to {worst:E3} away from unity");
    }

    // ======================================================================
    // prepare() re-derives everything the RATE owns
    // ======================================================================

    /// <summary>(module, setup) — the same rig at 48 kHz and at 192 kHz must
    /// modulate at the same rate IN SECONDS. An LFO increment, a delay length in
    /// samples or an allpass coefficient left un-re-derived by <c>prepare()</c>
    /// makes the 192 kHz render four times slower, which this measures directly:
    /// the output ENVELOPE's spectrum, on a hop grid defined in milliseconds, has
    /// to be the same shape at both rates.</summary>
    public static TheoryData<string, string> RateCases => new()
    {
        { "tzflanger", "rateHz=4;depthMs=5;manualMs=5;feedback=0.6;mix=1" },
        { "barberpole", "rateHz=3;depth=1;feedback=0;mix=1;stages=8" },
        { "autopan",   "rateHz=3;depth=1;phaseDeg=0;law=0;shape=0" },
        { "ensemble",  "rateHz=2;fastRateHz=8;depth=1;voices=3;tone=0.5;mix=0.5" },
        { "freqshift", "shiftHz=5;mix=0.5;feedback=0;direction=0" },
    };

    [NativeAvailableTheory]
    [MemberData(nameof(RateCases))]
    public void PrepareReDerivesEverythingTheRateOwns(string module, string setup)
    {
        var a = EnvelopeSpectrum(module, setup, 48000);
        var b = EnvelopeSpectrum(module, setup, 192000);

        // Non-vacuous: there has to BE a modulation to compare.
        Assert.True(Peak(a) > 3f * Median(a),
            $"{module}: nothing is modulating at 48 kHz (peak {Peak(a):F4}, median {Median(a):F4})");
        Assert.True(Peak(b) > 3f * Median(b),
            $"{module}: nothing is modulating at 192 kHz (peak {Peak(b):F4}, median {Median(b):F4})");

        var dist = Distance(a, b);
        Assert.True(dist < 0.35,
            $"{module}: the modulation spectrum at 192 kHz is a different shape from the one at "
            + $"48 kHz (normalised distance {dist:F3}) - something prepare() owns was not re-derived");
    }

    /// <summary>The 0.5 .. 20 Hz grid the envelope spectrum is sampled on.</summary>
    private const int SpecBins = 79;
    private const double SpecLoHz = 0.5;
    private const double SpecStepHz = 0.25;

    /// <summary>Euclidean distance between two unit-norm envelope spectra: 0 is
    /// the same modulation, sqrt(2) is nothing in common.</summary>
    private static double Distance(float[] a, float[] b)
    {
        var acc = 0.0;
        for (var i = 0; i < a.Length; i++) acc += (a[i] - b[i]) * (double)(a[i] - b[i]);
        return Math.Sqrt(acc);
    }

    /// <summary>One bin of an <see cref="EnvelopeSpectrum" /> by frequency.</summary>
    private static float At(float[] spec, double hz)
        => spec[Math.Clamp((int)Math.Round((hz - SpecLoHz) / SpecStepHz), 0, SpecBins - 1)];

    /// <summary>Normalised magnitude of the output envelope at 0.5 .. 20 Hz, on a
    /// 5 ms hop grid, so the vector is directly comparable between sample rates.</summary>
    private float[] EnvelopeSpectrum(string module, string setup, int rate, bool right = false)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh(module);
        ApplyAll(n, setup);

        const double seconds = 3.0;
        var frames = (int)(seconds * rate);
        var r = RenderSine(frames, rate);

        var buf = right ? r.R : r.L;
        var hop = rate / 200;                      // 5 ms, whatever the rate
        var env = new float[buf.Length / hop];
        for (var h = 0; h < env.Length; h++) env[h] = Rms(buf, h * hop, hop);
        var mean = 0f;
        foreach (var v in env) mean += v;
        mean /= env.Length;
        for (var h = 0; h < env.Length; h++) env[h] -= mean;

        var spec = new float[SpecBins];
        for (var i = 0; i < spec.Length; i++)
            spec[i] = (float)Goertzel(env, 0, env.Length, SpecLoHz + SpecStepHz * i, 200.0);
        var norm = 0.0;
        foreach (var v in spec) norm += v * (double)v;
        norm = Math.Sqrt(norm);
        if (norm > 1e-12) for (var i = 0; i < spec.Length; i++) spec[i] = (float)(spec[i] / norm);
        return spec;
    }

    // ======================================================================
    // tzflanger
    // ======================================================================

    /// <summary>
    /// The claim the whole module exists for. Both paths are delayed — the
    /// reference by <c>manualMs</c>, the swept one by <c>manualMs +
    /// depthMs*lfo</c> — so at the LFO's zero the two are SAMPLE-IDENTICAL and
    /// the 0.5/0.5 blend reconstructs the input exactly: the coherent sum of two
    /// half-weight copies, i.e. +6 dB over either one of them. Away from the
    /// zero the relative delay is a real comb with real notches.
    ///
    /// <para>A flanger that delays only the SWEPT path passes no part of this:
    /// its relative delay never reaches zero, so the crossing render would show
    /// a comb notch instead of unity.</para>
    /// </summary>
    [NativeAvailableFact]
    public void TzFlanger_SumsCoherentlyAtTheZeroCrossing_AndCombsAwayFromIt()
    {
        // --- at the crossing: the LFO starts at phase 0 (sine = 0) and the
        // slowest legal rate keeps it there for the whole render.
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh("tzflanger");
        ApplyAll(n, "rateHz=0.02;depthMs=0.1;manualMs=3;feedback=0;mix=0.5;shape=0");
        RenderNoise(Blocks(2), SampleRate);
        var at = ContinueNoise(Blocks(2), Blocks(2), SampleRate);
        var inRms = Rms(NoiseRecord(Blocks(2), Blocks(2)), 0, Blocks(2));
        var outRms = Rms(at.L, 0, at.L.Length);
        var db = 20f * MathF.Log10(outRms / inRms);
        Assert.True(MathF.Abs(db) < 0.5f,
            $"at the zero crossing the two half-weight paths must sum to unity (+6 dB over one "
            + $"of them); measured {db:F2} dB");

        // --- a quarter cycle later: a real comb. 2.5 s at 0.1 Hz puts the sine
        // at its peak, so the relative delay is the full 3 ms and the notches
        // sit at (k+0.5)/3 ms => 167 Hz, 500 Hz, 833 Hz…
        Assert.Equal(Ok, DspClearFor(H, 0));
        var m = Fresh("tzflanger");
        ApplyAll(m, "rateHz=0.1;depthMs=3;manualMs=3;feedback=0;mix=0.5;shape=0");
        const int quarter = 120_000;               // 2.5 s at 48 kHz
        RenderNoise(quarter, SampleRate);
        var swept = ContinueNoise(quarter, 16384, SampleRate);
        var dry = NoiseRecord(quarter, 16384);

        var deepest = 0f;
        var deepestHz = 0f;
        for (var hz = 120f; hz < 5000f; hz *= 1.01f)
        {
            var num = Goertzel(swept.L, 0, swept.L.Length, hz, SampleRate);
            var den = Goertzel(dry, 0, dry.Length, hz, SampleRate);
            if (den < 1e-9) continue;
            var g = 20f * MathF.Log10((float)(num / den));
            if (g < deepest) { deepest = g; deepestHz = hz; }
        }

        Assert.True(deepest < -20f,
            $"away from the crossing the swept path must comb against the reference; the deepest "
            + $"notch below 5 kHz was only {deepest:F1} dB (at {deepestHz:F0} Hz)");
    }

    /// <summary>
    /// The reference path is a FIXED delay, so the module is genuinely late and
    /// has to say so — a module that swallows its own latency makes both
    /// <c>dca_engine_dsp_get_chain_latency</c> and the merger's branch alignment
    /// lie (JuceSharp/AGENTS.md).
    /// </summary>
    [NativeAvailableFact]
    public void TzFlanger_ReportsItsManualDelayAsLatency()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh("tzflanger");
        RenderSine(Frames, SampleRate);       // establish the prepared rate

        Assert.Equal(Ok, DspSetParam(H, n, "manualMs", 0f));
        Assert.Equal(Ok, DspGetNodeLatency(H, n, out var zero));
        Assert.InRange(zero, 0, 2);

        Assert.Equal(Ok, DspSetParam(H, n, "manualMs", 5f));
        Assert.Equal(Ok, DspGetNodeLatency(H, n, out var five));
        Assert.InRange(five, 238, 243);       // 5 ms at 48 kHz = 240 samples

        Assert.Equal(Ok, DspSetParam(H, n, "manualMs", 10f));
        Assert.Equal(Ok, DspGetNodeLatency(H, n, out var ten));
        Assert.InRange(ten, 478, 483);
    }

    // ======================================================================
    // barberpole
    // ======================================================================

    /// <summary>
    /// The endless rise. Each voice is windowed to ZERO at both ends of its own
    /// sweep, so the sawtooth wrap it performs there is inaudible — that window
    /// is the entire trick, and without it the wrap is a full-weight voice
    /// jumping from the top of the range to the bottom four times a cycle.
    /// </summary>
    [NativeAvailableFact]
    public void BarberPole_SweepsWithoutAStepAtTheWrap()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh("barberpole");
        ApplyAll(n, "rateHz=5;depth=1;feedback=0.3;mix=1;stages=12");
        RenderSine(Blocks(2), SampleRate);
        var r = ContinueSine(Blocks(2), 96_000, SampleRate);      // 2 s = 40 wraps
        var dry = SineRecord(Blocks(2), 96_000);

        // Non-vacuous: the phaser has to be doing something to the signal.
        Assert.True(RmsDiff(r.L, dry, 0, dry.Length) > 0.02f,
            "the barber-pole is not changing the signal - the case is vacuous");

        var step = MaxStep(r.L, 1, r.L.Length - 1);
        var natural = MaxStep(dry, 1, dry.Length - 1);
        // 2x, and the factor is MEASURED, not chosen: with the window in place
        // the largest step over 40 wraps is 0.85x the stimulus' own slope, and
        // with the window replaced by a constant (the same four voices, still
        // summing to a constant level, just not silent when they wrap) it is
        // 3.8x. A looser bound passes the broken version.
        Assert.True(step < 2f * natural,
            $"a voice handover stepped the output by {step:F4} against the stimulus' own slope "
            + $"of {natural:F4} - the crossfade window is not reaching zero at the wrap");
    }

    /// <summary>
    /// Stage count is a real parameter, not decoration: a 12-section cascade
    /// notches six times per voice where a 4-section one notches twice, so the
    /// response is measurably rougher.
    /// </summary>
    [NativeAvailableFact]
    public void BarberPole_StageCountChangesTheNotchPattern()
    {
        var few = Roughness("barberpole", "rateHz=0.05;depth=1;feedback=0;mix=1;stages=4");
        var many = Roughness("barberpole", "rateHz=0.05;depth=1;feedback=0;mix=1;stages=12");
        Assert.True(many > 1.3f * few,
            $"12 stages must notch more than 4 (total variation {many:F1} dB vs {few:F1} dB)");
    }

    /// <summary>Total variation of the magnitude response over 150 Hz .. 6 kHz,
    /// in dB: more notches means more variation, and it needs no peak picking.</summary>
    private float Roughness(string module, string setup)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh(module);
        ApplyAll(n, setup);
        RenderNoise(Blocks(2), SampleRate);
        var wet = ContinueNoise(Blocks(2), 16384, SampleRate);
        var dry = NoiseRecord(Blocks(2), 16384);

        var tv = 0f;
        var prev = float.NaN;
        for (var hz = 150f; hz < 6000f; hz *= 1.01f)
        {
            var num = Goertzel(wet.L, 0, wet.L.Length, hz, SampleRate);
            var den = Goertzel(dry, 0, dry.Length, hz, SampleRate);
            if (den < 1e-9) continue;
            var g = 20f * MathF.Log10((float)(num / den));
            if (!float.IsNaN(prev)) tv += MathF.Abs(g - prev);
            prev = g;
        }

        return tv;
    }

    // ======================================================================
    // autopan
    // ======================================================================

    /// <summary>
    /// At 180 degrees of L/R phase the two sides are in antiphase and the equal-
    /// power law holds: gL² + gR² is 1 at every instant, so the pair never pumps
    /// while the image walks across it. The stimulus is DC, which makes the
    /// output literally the two gain curves.
    /// </summary>
    [NativeAvailableFact]
    public void AutoPan_AtOppositePhase_TheSidesAreAntiphaseAndEqualPowerIsConstant()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh("autopan");
        ApplyAll(n, "rateHz=2;depth=1;shape=0;phaseDeg=180;law=0");
        RenderDc(Blocks(2), SampleRate);                    // settle the 15 ms ramps
        var r = ContinueDc(Blocks(2), 48_000, SampleRate);  // one second, four sweeps

        float sumL = 0, sumR = 0;
        for (var i = 0; i < r.L.Length; i++) { sumL += r.L[i]; sumR += r.R[i]; }
        var meanL = sumL / r.L.Length;
        var meanR = sumR / r.R.Length;

        double cov = 0, varL = 0, varR = 0;
        var worstDb = 0f;
        for (var i = 0; i < r.L.Length; i++)
        {
            double dl = r.L[i] - meanL, dr = r.R[i] - meanR;
            cov += dl * dr; varL += dl * dl; varR += dr * dr;
            var power = MathF.Sqrt(r.L[i] * r.L[i] + r.R[i] * r.R[i]) / DcLevel;
            worstDb = MathF.Max(worstDb, MathF.Abs(20f * MathF.Log10(power)));
        }

        var corr = (float)(cov / Math.Sqrt(varL * varR));
        Assert.True(varL > 1e-4 && varR > 1e-4, "neither side is moving - the case is vacuous");
        // -0.9 and not -1: the two equal-power curves are cos and sin of the
        // same swept angle, which move in opposite directions but are not
        // LINEARLY related, so a perfect implementation measures about -0.92.
        Assert.True(corr < -0.9f,
            $"at 180 degrees the two sides must move in antiphase (correlation {corr:F3})");
        Assert.True(worstDb < 0.2f,
            $"the equal-power sum must stay constant across the sweep; it moved by {worstDb:F3} dB");
    }

    /// <summary>
    /// A raw square modulator is a full-scale step twice a cycle — a click at any
    /// depth, and the one shape the rig's "a param may not STEP a modulator" rule
    /// does not otherwise reach, because nothing is changing while it happens.
    /// The square is slew-limited to a 10 ms full-scale transit instead.
    /// </summary>
    [NativeAvailableFact]
    public void AutoPan_TheSquareShapeIsSlewLimited()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh("autopan");
        ApplyAll(n, "rateHz=2;depth=1;shape=2;phaseDeg=180;law=0");
        RenderDc(Blocks(2), SampleRate);
        var r = ContinueDc(Blocks(2), 48_000, SampleRate);

        // It has to REACH both ends, or "no step" would just mean "not moving".
        float lo = float.MaxValue, hi = float.MinValue;
        foreach (var v in r.L) { lo = MathF.Min(lo, v); hi = MathF.Max(hi, v); }
        Assert.True(hi - lo > 0.6f * DcLevel,
            $"the square must swing the pan from end to end (span {(hi - lo) / DcLevel:F2})");

        // A 10 ms transit of the full [-1, 1] modulator, through a law whose
        // slope never exceeds pi/4, is at most 0.0033 per sample of gain.
        var step = MaxStep(r.L, 1, r.L.Length - 1);
        Assert.True(step < 0.003f,
            $"the square modulator stepped the gain by {step:F4} - an un-slewed square would "
            + $"step by about {DcLevel:F2}");
    }

    // ======================================================================
    // ensemble
    // ======================================================================

    /// <summary>
    /// The one thing that makes this a string ensemble rather than a chorus: the
    /// taps are modulated by a SLOW bank and a FAST bank at once, and the two are
    /// INDEPENDENT. The probe is differential rather than absolute, and that is
    /// not fussiness — a deep slow sweep crosses many comb nulls per cycle, so
    /// its envelope carries a dense harmonic series and there is energy at very
    /// nearly any "fast" rate you care to name whether the fast bank exists or
    /// not. Measured: with <c>fastSpanMs</c> forced to zero, an absolute probe at
    /// 8 Hz against a noise floor still passed. Moving each bank's rate and
    /// watching the energy FOLLOW it does not.
    /// </summary>
    [NativeAvailableFact]
    public void Ensemble_BothBanksAreLiveAndIndependent()
    {
        const string common = "depth=1;voices=3;tone=0.5;mix=0.5";

        foreach (var right in new[] { false, true })
        {
            var side = right ? "right" : "left";

            // The FAST bank, at a fixed slow rate.
            var slowFast = EnvelopeSpectrum("ensemble", common + ";rateHz=2;fastRateHz=3", SampleRate, right);
            var fastFast = EnvelopeSpectrum("ensemble", common + ";rateHz=2;fastRateHz=8", SampleRate, right);
            Assert.True(At(slowFast, 3.0) > 2f * At(fastFast, 3.0),
                $"{side}: moving the fast bank off 3 Hz must take the 3 Hz energy with it "
                + $"({At(slowFast, 3.0):F4} vs {At(fastFast, 3.0):F4})");
            Assert.True(At(fastFast, 8.0) > 2f * At(slowFast, 8.0),
                $"{side}: moving the fast bank to 8 Hz must put energy there "
                + $"({At(fastFast, 8.0):F4} vs {At(slowFast, 8.0):F4})");

            // …and the SLOW bank, at a fixed fast rate. Only the 1 Hz direction
            // is asserted positively: a deep sweep at 1 Hz puts plenty at 2 Hz
            // (its own second harmonic), so "2 Hz energy means the slow bank is
            // at 2 Hz" is not a claim the physics supports. 1 Hz is not a
            // harmonic of 2 Hz, so that direction is clean.
            var slowLow = EnvelopeSpectrum("ensemble", common + ";rateHz=1;fastRateHz=5.5", SampleRate, right);
            var slowHigh = EnvelopeSpectrum("ensemble", common + ";rateHz=2;fastRateHz=5.5", SampleRate, right);
            Assert.True(At(slowLow, 1.0) > 2f * At(slowHigh, 1.0),
                $"{side}: the slow bank at 1 Hz must put energy at 1 Hz "
                + $"({At(slowLow, 1.0):F4} vs {At(slowHigh, 1.0):F4})");
            Assert.True(Distance(slowLow, slowHigh) > 0.2,
                $"{side}: the slow bank's rate must change the modulation spectrum "
                + $"(distance {Distance(slowLow, slowHigh):F3})");
        }
    }

    /// <summary>
    /// <c>voices</c> is a real parameter: at three voices each side takes TWO
    /// taps (L = 0,1 / R = 1,2) and at two it takes one, so the modulation the
    /// two settings produce is a different shape, not merely a different level.
    /// </summary>
    [NativeAvailableFact]
    public void Ensemble_VoiceCountChangesTheModulation()
    {
        const string setup = "rateHz=2;fastRateHz=8;depth=1;tone=0.5;mix=0.5";
        var two = EnvelopeSpectrum("ensemble", setup + ";voices=2", SampleRate);
        var three = EnvelopeSpectrum("ensemble", setup + ";voices=3", SampleRate);

        var dist = Distance(two, three);
        Assert.True(dist > 0.2,
            $"two voices and three must not modulate the same way (normalised distance {dist:F3})");
    }

    // ======================================================================
    // freqshift
    // ======================================================================

    /// <summary>
    /// A frequency shifter is NOT a pitch shifter: 440 Hz shifted by 100 Hz lands
    /// at 540 Hz, not at an octave-ish ratio. The single-sideband arithmetic is
    /// what makes that true, and its quality is measured by the IMAGE — the
    /// unwanted 340 Hz sideband the Hilbert pair's phase error leaks through.
    /// Both branches are ALLPASS, so magnitude proves nothing and the image is
    /// the only real evidence.
    /// </summary>
    [NativeAvailableFact]
    public void FreqShift_MovesEveryPartialByHertz_AndSuppressesTheImage()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh("freqshift");
        ApplyAll(n, "shiftHz=100;mix=1;feedback=0;direction=0");
        RenderTone(Blocks(4), SampleRate, 440f);
        var r = ContinueTone(Blocks(4), 32768, SampleRate, 440f);

        var best = 0.0;
        var bestHz = 0.0;
        for (var hz = 500.0; hz <= 580.0; hz += 0.25)
        {
            var m = Goertzel(r.L, 0, r.L.Length, hz, SampleRate);
            if (m > best) { best = m; bestHz = hz; }
        }

        Assert.True(Math.Abs(bestHz - 540.0) <= 2.0,
            $"440 Hz shifted up by 100 Hz must land at 540 Hz; the peak was at {bestHz:F1} Hz");

        var image = Goertzel(r.L, 0, r.L.Length, 340.0, SampleRate);
        var rejection = 20.0 * Math.Log10(image / best);
        Assert.True(rejection < -40.0,
            $"the 340 Hz image must be at least 40 dB down; it is {rejection:F1} dB");
    }

    /// <summary>
    /// At zero shift the quadrature term is multiplied by sin(0) and the module
    /// is a pure allpass: the tone comes out at the same frequency and the same
    /// level. Anything else means the Hilbert branches are not unit magnitude,
    /// which is the one thing about them that is exact.
    /// </summary>
    [NativeAvailableFact]
    public void FreqShift_AtZeroShift_IsTransparent()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh("freqshift");
        ApplyAll(n, "shiftHz=0;mix=1;feedback=0;direction=0");
        RenderTone(Blocks(4), SampleRate, 440f);
        var r = ContinueTone(Blocks(4), 32768, SampleRate, 440f);
        var dry = ToneRecord(Blocks(4), 32768, 440f);

        var best = 0.0;
        var bestHz = 0.0;
        for (var hz = 420.0; hz <= 460.0; hz += 0.25)
        {
            var m = Goertzel(r.L, 0, r.L.Length, hz, SampleRate);
            if (m > best) { best = m; bestHz = hz; }
        }

        Assert.True(Math.Abs(bestHz - 440.0) <= 1.0,
            $"a zero shift must not move the tone; the peak was at {bestHz:F2} Hz");
        var db = 20f * MathF.Log10(Rms(r.L, 0, r.L.Length) / Rms(dry, 0, dry.Length));
        Assert.True(MathF.Abs(db) < 0.1f,
            $"a zero shift must be unity gain (allpass); measured {db:F3} dB");
    }

    /// <summary>
    /// <c>direction = Both</c> is the reason the direction weights are per-SIDE:
    /// the left channel keeps the upper sideband and the right the lower, which
    /// is the widening trick the effect is usually bought for.
    /// </summary>
    [NativeAvailableFact]
    public void FreqShift_DirectionBoth_ShiftsUpOnTheLeftAndDownOnTheRight()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh("freqshift");
        ApplyAll(n, "shiftHz=100;mix=1;feedback=0;direction=2");
        RenderTone(Blocks(4), SampleRate, 440f);
        var r = ContinueTone(Blocks(4), 32768, SampleRate, 440f);

        var upL = Goertzel(r.L, 0, r.L.Length, 540.0, SampleRate);
        var downL = Goertzel(r.L, 0, r.L.Length, 340.0, SampleRate);
        var upR = Goertzel(r.R, 0, r.R.Length, 540.0, SampleRate);
        var downR = Goertzel(r.R, 0, r.R.Length, 340.0, SampleRate);

        Assert.True(20.0 * Math.Log10(downL / upL) < -40.0,
            $"the left side must keep the UPPER sideband ({20.0 * Math.Log10(downL / upL):F1} dB)");
        Assert.True(20.0 * Math.Log10(upR / downR) < -40.0,
            $"the right side must keep the LOWER sideband ({20.0 * Math.Log10(upR / downR):F1} dB)");
    }

    // ======================================================================
    // Fixture helpers
    // ======================================================================

    private const float DcLevel = 0.5f;

    private int Fresh(string module)
    {
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        return n;
    }

    private static int Blocks(int n) => n * Frames;

    private void ApplyAll(int node, string spec)
    {
        if (string.IsNullOrEmpty(spec)) return;
        foreach (var pair in spec.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = pair.Split('=');
            Assert.Equal(Ok, DspSetParam(H, node, kv[0],
                float.Parse(kv[1], CultureInfo.InvariantCulture)));
        }
    }

    /// <summary>One node, PreRoll + 1 blocks at <paramref name="first" />, then
    /// one block at <paramref name="second" /> — the block returned.</summary>
    /// <remarks>BOTH channels, and that is load-bearing. This returned
    /// <c>.L</c> only until 2026-09-12, which made every case for a parameter
    /// that moves the RIGHT channel vacuous: <c>autopan.phaseDeg</c> is applied
    /// to odd channels alone (<c>dcdsp_autopan.h</c>: the offset is added only
    /// when <c>(c &amp; 1)</c>), so it cannot change channel 0 at all and its
    /// case asserted that a parameter with no effect on the measured signal did
    /// not step it; and <c>ensemble.voices</c> leaves tap 0's phase offset at
    /// <c>0 * spacing == 0</c> in both configurations, so the left channel is
    /// continuous by construction while the right one moved a read tap by ~170
    /// samples in one sample.</remarks>
    private (float[] L, float[] R) RenderSeam(string module, string setup, string param, float first, float second)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = Fresh(module);
        ApplyAll(n, setup);
        Assert.Equal(Ok, DspSetParam(H, n, param, first));
        RenderSine(Blocks(PreRoll + 1), SampleRate);
        Assert.Equal(Ok, DspSetParam(H, n, param, second));
        return ContinueSine(Blocks(PreRoll + 1), Frames, SampleRate);
    }

    // --- stimulus: every generator is a pure function of the ABSOLUTE sample
    // index, so two renders of the same case are sample-aligned exactly.

    private static float Sine(int i, float hz, int rate) => 0.25f * MathF.Sin(2f * MathF.PI * hz * i / rate);

    private static float Noise(int i)
    {
        var x = (uint)i * 0x9E3779B9u + 0x85EBCA6Bu;
        x ^= x >> 16; x *= 0x7FEB352Du;
        x ^= x >> 15; x *= 0x846CA68Bu;
        x ^= x >> 16;
        return 0.25f * ((x >> 8) * (1f / 8388608f) - 1f);
    }

    private static float[] SineRecord(int offset, int frames, int rate = SampleRate)
        => ToneRecord(offset, frames, 220f, rate);

    private static float[] ToneRecord(int offset, int frames, float hz, int rate = SampleRate)
    {
        var b = new float[frames];
        for (var i = 0; i < frames; i++) b[i] = Sine(offset + i, hz, rate);
        return b;
    }

    private static float[] NoiseRecord(int offset, int frames)
    {
        var b = new float[frames];
        for (var i = 0; i < frames; i++) b[i] = Noise(offset + i);
        return b;
    }

    private (float[] L, float[] R) Run(float[] l, float[] r, int rate)
    {
        Assert.Equal(Ok, DspProcess(H, l, r, l.Length, rate));
        return (l, r);
    }

    private (float[] L, float[] R) RenderSine(int frames, int rate) => ContinueSine(0, frames, rate);
    private (float[] L, float[] R) ContinueSine(int offset, int frames, int rate)
        => Run(SineRecord(offset, frames, rate), SineRecord(offset, frames, rate), rate);

    private (float[] L, float[] R) RenderTone(int frames, int rate, float hz) => ContinueTone(0, frames, rate, hz);
    private (float[] L, float[] R) ContinueTone(int offset, int frames, int rate, float hz)
        => Run(ToneRecord(offset, frames, hz, rate), ToneRecord(offset, frames, hz, rate), rate);

    private (float[] L, float[] R) RenderNoise(int frames, int rate) => ContinueNoise(0, frames, rate);
    private (float[] L, float[] R) ContinueNoise(int offset, int frames, int rate)
        => Run(NoiseRecord(offset, frames), NoiseRecord(offset, frames), rate);

    private (float[] L, float[] R) RenderDc(int frames, int rate) => ContinueDc(0, frames, rate);
    private (float[] L, float[] R) ContinueDc(int offset, int frames, int rate)
    {
        _ = offset;
        var l = new float[frames];
        var r = new float[frames];
        Array.Fill(l, DcLevel);
        Array.Fill(r, DcLevel);
        return Run(l, r, rate);
    }

    // --- measurement

    private static float MaxStep(float[] x, int start, int count)
    {
        var max = 0f;
        for (var i = start; i < start + count; i++) max = MathF.Max(max, MathF.Abs(x[i] - x[i - 1]));
        return max;
    }

    private static float[] Envelope(float[] x, int hop)
    {
        var env = new float[x.Length / hop];
        for (var h = 0; h < env.Length; h++) env[h] = Rms(x, h * hop, hop);
        var mean = 0f;
        foreach (var v in env) mean += v;
        mean /= env.Length;
        for (var h = 0; h < env.Length; h++) env[h] -= mean;
        return env;
    }

    /// <summary>Hann-windowed magnitude at one frequency. A direct DFT bin, not a
    /// full FFT: the frequencies of interest here (2 Hz on an envelope, 340 Hz on
    /// audio) are not on any convenient FFT grid.</summary>
    private static double Goertzel(float[] x, int start, int count, double hz, double rate)
    {
        double re = 0, im = 0;
        for (var i = 0; i < count; i++)
        {
            var w = 0.5 - 0.5 * Math.Cos(2 * Math.PI * i / (count - 1));
            var a = 2 * Math.PI * hz * i / rate;
            re += x[start + i] * w * Math.Cos(a);
            im += x[start + i] * w * Math.Sin(a);
        }

        return Math.Sqrt(re * re + im * im) / count;
    }

    private static float Peak(float[] x)
    {
        var m = 0f;
        foreach (var v in x) m = MathF.Max(m, v);
        return m;
    }

    private static float Median(float[] x)
    {
        var copy = (float[])x.Clone();
        Array.Sort(copy);
        return copy[copy.Length / 2];
    }

    private static float RmsDiff(float[] a, float[] b, int start, int count)
    {
        double acc = 0;
        for (var i = start; i < start + count; i++)
        {
            double d = a[i] - b[i];
            acc += d * d;
        }

        return (float)Math.Sqrt(acc / count);
    }

    private static float Rms(float[] x, int start, int count)
    {
        double acc = 0;
        for (var i = start; i < start + count; i++) acc += (double)x[i] * x[i];
        return (float)Math.Sqrt(acc / count);
    }

    private static string Cstr(byte[] b)
    {
        var len = Array.IndexOf(b, (byte)0);
        return System.Text.Encoding.UTF8.GetString(b, 0, len < 0 ? b.Length : len);
    }
}
