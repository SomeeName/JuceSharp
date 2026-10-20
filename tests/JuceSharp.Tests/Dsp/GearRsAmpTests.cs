using System.Globalization;
using System.Runtime.InteropServices;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// <c>rsamp</c>: Rocksmith 2014's own amp chains (<c>src/dsp/gear/RsAmpGear.cpp</c>
/// over <c>cores/dcdsp_rsamp.h</c>). The chain table is the GAME'S data, so the
/// library ships none of it: the host loads one at run time (ABI v45,
/// <see cref="JuceRsAmpTable" />). Every table here is SYNTHETIC
/// (<see cref="SyntheticRsAmpTables" />), so each expected response follows from
/// the row's own definition.
///
/// <para>Rendered through the shipping wiring (catalogue row, adapter mailbox,
/// the chain's clamp and crossfade) on a stopped engine, like
/// <see cref="GearAmpCabTests"/>. Each voicing claim is a DIFFERENCE of two renders
/// at one frequency, small-signal, with one knob moved, so every other stage of the
/// chain cancels. The table is process-wide, so the class sits in the
/// <c>NativeEngine</c> collection and unloads it when it is done.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearRsAmpTests : IDisposable
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

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_process_buffer", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspProcess(nint engine, float[] left, float[] right, int frames, int sampleRate);

    private const int Ok = 0;
    private const int Sr = 48000;

    /// Small-signal level: 1e-3 (-60 dBFS) is +18 dB at the chain's own input
    /// (dcdsp_rsamp.h), far under every waveshaper's knee.
    private const float Small = 1e-3f;

    private readonly JuceAudioEngine? _engine;
    private readonly ITestOutputHelper _out;

    public GearRsAmpTests(ITestOutputHelper output)
    {
        _out = output;
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose()
    {
        if (_engine is null) return;
        DspClearFor(_engine.NativeHandle, 0);
        JuceRsAmpTable.Unload();
        _engine.Dispose();
    }

    private nint H => _engine!.NativeHandle;

    // ---------------------------------------------------------------- helpers

    private static IReadOnlyList<string> ModelNames
        => JuceDspModuleCatalog.Find("rsamp")!.Params.Single(p => p.Id == "model").Labels!;

    private static void Load(RsAmpChainTable table) => Assert.True(JuceRsAmpTable.TryLoad(table), "the synthetic table was refused");

    private int Fresh(string spec)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, "rsamp");
        Assert.True(n >= 0, "rsamp: add failed");
        Apply(n, spec);
        return n;
    }

    private void Apply(int node, string spec)
    {
        foreach (var pair in spec.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = pair.Split('=');
            Assert.Equal(Ok, DspSetParam(H, node, kv[0], float.Parse(kv[1], CultureInfo.InvariantCulture)));
        }
    }

    private float[] RenderBlocks(Func<int, float> stim, int frames, int sampleRate, int blocks)
    {
        float[] last = [];
        for (var b = 0; b < blocks; b++)
        {
            var l = new float[frames];
            var r = new float[frames];
            for (var i = 0; i < frames; i++) l[i] = r[i] = stim(b * frames + i);
            Assert.Equal(Ok, DspProcess(H, l, r, frames, sampleRate));
            last = l;
        }
        return last;
    }

    private static Func<int, float> Sine(double freq, float amp, int sampleRate)
        => i => (float)(amp * Math.Sin(2.0 * Math.PI * freq * i / sampleRate));

    private static double Amplitude(float[] x, double freq, int sampleRate, int start, int count)
    {
        var w = 2.0 * Math.PI * freq / sampleRate;
        double cw = Math.Cos(w), sw = Math.Sin(w), coeff = 2.0 * cw;
        double s1 = 0, s2 = 0;
        for (var i = 0; i < count; i++)
        {
            var s0 = x[start + i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        var re = s1 - s2 * cw;
        var im = s2 * sw;
        return 2.0 * Math.Sqrt(re * re + im * im) / count;
    }

    private static double Db(double v) => 20.0 * Math.Log10(Math.Max(v, 1e-12));

    /// dB level at a whole frequency (1 s at the rate: leakage-free), after a
    /// one-second pre-roll that lets the 30 ms knob ramps and the chain's node
    /// crossfade settle.
    private double LevelDb(string spec, double freq, float amp = Small, int sampleRate = Sr)
    {
        Fresh(spec);
        var y = RenderBlocks(Sine(freq, amp, sampleRate), sampleRate, sampleRate, 2);
        return Db(Amplitude(y, freq, sampleRate, 0, sampleRate));
    }

    /// The level change at one frequency when one knob moves from a to b.
    private double MoveDb(string setup, string knob, float a, float b, double freq, int sampleRate = Sr)
        => LevelDb($"{setup};{knob}={b.ToString(CultureInfo.InvariantCulture)}", freq, Small, sampleRate)
           - LevelDb($"{setup};{knob}={a.ToString(CultureInfo.InvariantCulture)}", freq, Small, sampleRate);

    // ======================================================================
    // Catalogue and parameter contracts
    // ======================================================================

    [NativeAvailableFact]
    public void TheRow_IsAnAmp_NamesEveryRocksmithAmp_AndCarriesTenKnobPositions()
    {
        var info = JuceDspModuleCatalog.Find("rsamp");
        Assert.NotNull(info);
        Assert.Equal("amp", info.Category);
        var model = info.Params.Single(p => p.Id == "model");
        Assert.True(model.IsNamed);
        // 42 guitar amps, 18 bass heads and 3 DI preamps: every amp the game
        // ships, by its public catalogue name, in ordinal key order
        // (dcdsp_rsamp_models.h) — the rows exist whether a table is loaded or not.
        Assert.Equal(63, model.Labels!.Count);
        Assert.Equal(62f, model.Max - model.Min);
        Assert.Equal("AT-120", model.Labels[0]);
        Assert.Equal("GBC-1H", model.Labels[17]);
        Assert.Equal("HW-300B", model.Labels[55]);
        Assert.Equal("DI. Pre Tube", model.Labels[62]);
        Assert.Equal(model.Labels.Count, model.Labels.Distinct().Count());
        for (var k = 1; k <= 10; k++)
        {
            var p = info.Params.Single(x => x.Id == "k" + k);
            Assert.Equal((0f, 1f, 0.5f), (p.Min, p.Max, p.Default));
        }

        var level = info.Params.Single(x => x.Id == "level");
        Assert.Equal((-24f, 24f, 0f), (level.Min, level.Max, level.Default));
    }

    [NativeAvailableFact]
    public void GetParam_ReadsBack_AndANonFiniteValueIsRefused_AndTheAmpKeepsPlaying()
    {
        Load(SyntheticRsAmpTables.Full());
        var n = Fresh("model=17;k3=0.25");
        Assert.Equal(Ok, DspGetParam(H, n, "k3", out var k3));
        Assert.Equal(0.25f, k3);
        Assert.Equal(Ok, DspGetParam(H, n, "model", out var m));
        Assert.Equal(17f, m);
        Assert.NotEqual(Ok, DspSetParam(H, n, "k1", float.NaN));
        Assert.NotEqual(Ok, DspSetParam(H, n, "model", float.PositiveInfinity));
        Assert.NotEqual(Ok, DspSetParam(H, n, "k11", 0.5f));
        var y = RenderBlocks(Sine(220, 0.1f, Sr), 4800, Sr, 4);
        Assert.All(y, v => Assert.True(float.IsFinite(v)));
        Assert.True(y.Max(MathF.Abs) > 1e-3f, "the amp went silent");
    }

    // ======================================================================
    // No table: nothing pretends to be the game's amp
    // ======================================================================

    /// Without a table (the library ships none) every row passes audio through
    /// UNCHANGED: not a guessed amp dressed up as the game's.
    [NativeAvailableFact]
    public void WithNoTableLoaded_EveryModelPassesAudioThroughUnchanged()
    {
        JuceRsAmpTable.Unload();
        Assert.Equal(0, JuceRsAmpTable.LoadedRowCount);
        foreach (var m in new[] { 0, 17, 55, 62 })
        {
            Fresh($"model={m};k1=1;k2=0");
            var stim = Sine(220, 0.2f, Sr);
            var y = RenderBlocks(stim, 4800, Sr, 3);
            for (var i = 0; i < y.Length; i++)
                Assert.Equal(stim(2 * 4800 + i), y[i], 6);
        }
    }

    /// A row the loaded table has no chain for passes through too.
    [NativeAvailableFact]
    public void ARowTheLoadedTableLacks_PassesAudioThrough()
    {
        Load(SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(17)));
        Assert.Equal(1, JuceRsAmpTable.LoadedRowCount);
        Fresh("model=5");
        var stim = Sine(220, 0.2f, Sr);
        var y = RenderBlocks(stim, 4800, Sr, 3);
        for (var i = 0; i < y.Length; i++)
            Assert.Equal(stim(2 * 4800 + i), y[i], 6);
    }

    [NativeAvailableFact]
    public void EveryModel_PlaysAGuitarLevelSine_FiniteAndAudible_AndSilenceStaysSilent()
    {
        Load(SyntheticRsAmpTables.Full());
        Assert.Equal(63, JuceRsAmpTable.LoadedRowCount);
        for (var m = 0; m < ModelNames.Count; m++)
        {
            var db = LevelDb($"model={m}", 220.0, 0.125f);
            _out.WriteLine($"{m,2} {ModelNames[m],-30} {db,7:0.0} dB");
            Assert.True(db is > -70.0 and < 12.0, $"{ModelNames[m]}: 220 Hz at -18 dBFS came out at {db:0.0} dB");

            Fresh($"model={m}");
            var silent = RenderBlocks(_ => 0f, 4800, Sr, 3);
            Assert.True(silent.Max(MathF.Abs) < 1e-6f, $"{ModelNames[m]}: silence in, {silent.Max(MathF.Abs)} out");
        }
    }

    // ======================================================================
    // The table's own numbers reach the sound
    // ======================================================================

    /// k2 is the 1 kHz bell's gain on a dB-scaled curve from −12 dB to +12 dB, so its
    /// whole travel moves 1 kHz by 24 dB: the curve's ends, through an RBJ peaking
    /// section whose centre gain IS its gain.
    [NativeAvailableFact]
    public void ADbCurve_MovesItsBand_ByTheCurvesEnds()
    {
        Load(SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(17)));
        var moved = MoveDb("model=17", "k2", 0f, 1f, 1000.0);
        _out.WriteLine($"k2 at 1 kHz: {moved:0.00} dB");
        Assert.InRange(moved, 24.0 - 0.3, 24.0 + 0.3);
    }

    /// k3 is the block's output on a dB curve −6 .. +6: a 12 dB move at any frequency.
    [NativeAvailableFact]
    public void AnOutputCurve_IsABroadbandGain()
    {
        Load(SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(17)));
        Assert.InRange(MoveDb("model=17", "k3", 0f, 1f, 300.0), 12.0 - 0.1, 12.0 + 0.1);
    }

    /// A log-Hz curve: the FutzBox low-pass corner, k1 200 Hz .. 20 kHz. At the top
    /// 5 kHz is passed; at the bottom it is far down the slope.
    [NativeAvailableFact]
    public void ALogHzCurve_MovesTheFutzBoxCorner()
    {
        Load(SyntheticRsAmpTables.Of(SyntheticRsAmpTables.FutzRow(55)));
        var moved = MoveDb("model=55", "k1", 0f, 1f, 5000.0);
        _out.WriteLine($"k1 at 5 kHz: {moved:0.0} dB");
        Assert.True(moved > 30.0, $"opening the low-pass from 200 Hz to 20 kHz should lift 5 kHz a lot: {moved:0.0} dB");
    }

    /// The drive knob drives the waveshaper: the third harmonic of a quiet 220 Hz
    /// sine (well under the heavy curve's knee at drive 0) rises with it.
    [NativeAvailableFact]
    public void ADriveCurve_DrivesTheDistortion()
    {
        Load(SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(17, distortion: 2)));

        double Thd(float drive)
        {
            Fresh($"model=17;k1={drive.ToString(CultureInfo.InvariantCulture)}");
            var y = RenderBlocks(Sine(220.0, 0.004f, Sr), Sr, Sr, 2);
            return Db(Amplitude(y, 660.0, Sr, 0, Sr)) - Db(Amplitude(y, 220.0, Sr, 0, Sr));
        }

        var low = Thd(0f);
        var high = Thd(1f);
        _out.WriteLine($"H3/H1: drive 0 {low:0.0} dB, drive 1 {high:0.0} dB");
        Assert.True(high > low + 6.0, $"H3 should rise with drive: {low:0.0} -> {high:0.0} dB");
    }

    // ======================================================================
    // Ramps, switches, rates, and the table swap
    // ======================================================================

    private static double MaxStep(float[] y, int from, int to)
    {
        double m = 0;
        for (var i = Math.Max(1, from); i < to; i++) m = Math.Max(m, Math.Abs(y[i] - y[i - 1]));
        return m;
    }

    private float[] RenderAcross(Action atSeam)
    {
        var stim = Sine(110.0, 0.05f, Sr);
        const int frames = 960;
        var all = new List<float>();
        for (var b = 0; b < 40; b++)
        {
            if (b == 20) atSeam();
            var l = new float[frames];
            var r = new float[frames];
            for (var i = 0; i < frames; i++) l[i] = r[i] = stim(b * frames + i);
            Assert.Equal(Ok, DspProcess(H, l, r, frames, Sr));
            all.AddRange(l);
        }

        return [.. all];
    }

    private void AssertNoStep(float[] y, string what)
    {
        const int frames = 960;
        var before = MaxStep(y, 10 * frames, 20 * frames);
        var after = MaxStep(y, 34 * frames, 40 * frames);
        var across = MaxStep(y, 20 * frames, 24 * frames);
        _out.WriteLine($"{what}: max step before {before:0.0000}, across {across:0.0000}, after {after:0.0000}");
        Assert.True(across <= 1.25 * Math.Max(before, after) + 1e-4,
            $"{what} stepped: {across:0.0000} against {before:0.0000} / {after:0.0000}");
    }

    [NativeAvailableTheory]
    [InlineData("model=17;k1=50", "k2", 0f, 1f)]      // the bell's gain
    [InlineData("model=17;k1=20", "k1", 0.2f, 1f)]    // the drive
    [InlineData("model=55", "model", 55f, 17f)]       // a model change dips, never steps
    [InlineData("model=17;k1=50", "level", -12f, 12f)] // the output trim ramps
    public void MovingAKnobOrTheModel_NeverSteps(string setup, string param, float first, float second)
    {
        Load(SyntheticRsAmpTables.Full());
        var n = Fresh(setup + ";" + param + "=" + first.ToString(CultureInfo.InvariantCulture));
        var y = RenderAcross(() => Assert.Equal(Ok, DspSetParam(H, n, param, second)));
        AssertNoStep(y, $"{param} {first} -> {second}");
    }

    /// A table loaded while the module plays is picked up by the module itself,
    /// through the same dip a model change takes: never a step from passthrough to
    /// the chain.
    [NativeAvailableFact]
    public void ATableLoadedWhilePlaying_IsPickedUpThroughTheDip()
    {
        JuceRsAmpTable.Unload();
        Fresh("model=17;k3=1");
        var y = RenderAcross(() => Load(SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(17))));
        AssertNoStep(y, "table load");
        // ...and it is in the path afterwards: k3 = 1 is the block's +6 dB output,
        // so the signal is no longer the dry stimulus.
        var stim = Sine(110.0, 0.05f, Sr);
        var diff = 0.0;
        for (var i = 39 * 960; i < 40 * 960; i++) diff = Math.Max(diff, Math.Abs(y[i] - stim(i)));
        Assert.True(diff > 1e-3, $"the loaded chain is not in the path: max |y - x| {diff:0.0000}");
    }

    /// The output trim is an exact gain at the chain's output: the whole chain,
    /// distortion included, sees the same input either way.
    [NativeAvailableFact]
    public void Level_IsAnExactOutputGain()
    {
        Load(SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(17, distortion: 2)));
        var at0 = LevelDb("model=17;k1=0.8;level=0", 220.0, 0.125f);
        var at9 = LevelDb("model=17;k1=0.8;level=9", 220.0, 0.125f);
        Assert.InRange(at9 - at0, 9.0 - 0.05, 9.0 + 0.05);
    }

    [NativeAvailableTheory]
    [InlineData(44100)]
    [InlineData(96000)]
    public void AnotherRate_KeepsTheTablesNumbers(int sampleRate)
    {
        Load(SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(17)));
        var moved = MoveDb("model=17", "k2", 0f, 1f, 1000.0, sampleRate);
        Assert.InRange(moved, 24.0 - 0.3, 24.0 + 0.3);
    }
}
