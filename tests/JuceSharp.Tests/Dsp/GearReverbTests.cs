using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using JuceSharp.Dsp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// The four `reverb`-drawer modules added in the 2026-09 gear wave:
/// <c>hallreverb</c>, <c>convreverb</c>, <c>gatedreverb</c> and
/// <c>reversereverb</c>.
///
/// <para>Everything here renders OFFLINE on a stopped engine through
/// <c>dca_engine_dsp_process_buffer</c>, the same harness
/// <see cref="DspGainZipperTests"/> and <see cref="DspBypassCrossfadeTests"/>
/// use. Each module gets, at minimum: the behaviour it exists for (RT60,
/// exactness of a convolution, the gate closing, the swell rising), a ramped
/// mix, a param that must GLIDE rather than splice, a NaN/clamp case, and a
/// rate change that has to re-derive everything the rate owns while leaving
/// user content (a loaded IR) alone.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearReverbTests : IDisposable
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
    private const int Rate = 48000;
    /// Silence rendered before every measurement, so the chain's own 10 ms
    /// add-crossfade is finished and the node is at full wet.
    private const int PreRoll = 4800;
    /// Pre-roll for the reverse reverb's probes: long enough that both of its
    /// backwards readers are inside REAL captured audio (2 x the longest window
    /// under test) rather than the ring's initial zeros.
    private const int Prime = 120000;

    private static readonly string[] Modules =
        { "hallreverb", "convreverb", "gatedreverb", "reversereverb" };

    private readonly JuceAudioEngine? _engine;
    private readonly ITestOutputHelper _out;

    public GearReverbTests(ITestOutputHelper output)
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
        _engine.Dispose();
    }

    private nint H => _engine!.NativeHandle;

    // ======================================================================
    // harness
    // ======================================================================

    /// A fresh single-node chain with `setup` applied, returning the node id.
    private int Fresh(string module, string setup = "")
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        ApplyAll(n, setup);
        return n;
    }

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

    /// Renders `signal` (mono, copied to both channels) through the chain in ONE
    /// call and returns the left channel. The offline export chunks internally,
    /// so this is exactly what the callback would see.
    private float[] Render(float[] signal, int rate = Rate)
    {
        var l = (float[])signal.Clone();
        var r = (float[])signal.Clone();
        Assert.Equal(Ok, DspProcess(H, l, r, signal.Length, rate));
        return l;
    }

    private static float[] Impulse(int length, int at = PreRoll, float amp = 1f)
    {
        var x = new float[length];
        x[at] = amp;
        return x;
    }

    private static float[] Noise(int length, float amp = 0.25f, int offset = 0)
    {
        var x = new float[length];
        for (var i = 0; i < length; i++)
        {
            var v = (uint)(offset + i) * 0x9E3779B9u + 0x85EBCA6Bu;
            v ^= v >> 16; v *= 0x7FEB352Du;
            v ^= v >> 15; v *= 0x846CA68Bu;
            v ^= v >> 16;
            x[i] = amp * ((v >> 8) * (1f / 8388608f) - 1f);
        }
        return x;
    }

    /// A 110 Hz sine — the stimulus for every STEP probe here. Broadband noise
    /// cannot see a splice in a reverb at all: its own sample-to-sample slope is
    /// ~0.6 at 0.25 amplitude, which is larger than anything a spliced tank, a
    /// teleported read tap or a re-latched window can produce, so all three
    /// probes passed with the fault deliberately in (measured 2026-09-11, the
    /// same wall DspGainZipperTests records for platereverb.predelayMs). A sine's
    /// own step is 0.0036, so a discontinuity has nowhere to hide.
    private static float[] Sine(int length, float amp = 0.25f, int offset = 0, double hz = 110.0)
    {
        var x = new float[length];
        for (var i = 0; i < length; i++)
            x[i] = amp * MathF.Sin((float)(2.0 * Math.PI * hz * (offset + i) / Rate));
        return x;
    }

    private static double Rms(float[] x, int start, int count)
    {
        if (start < 0) start = 0;
        if (start + count > x.Length) count = x.Length - start;
        if (count <= 0) return 0;
        double a = 0;
        for (var i = start; i < start + count; i++) a += (double)x[i] * x[i];
        return Math.Sqrt(a / count);
    }

    /// `previous` followed by `block` — so a step ACROSS a render boundary is
    /// inside the array a step probe walks.
    private static float[] Join(float previous, float[] block)
    {
        var y = new float[block.Length + 1];
        y[0] = previous;
        Array.Copy(block, 0, y, 1, block.Length);
        return y;
    }

    private static float MaxStep(float[] x, int start, int count)
    {
        var m = 0f;
        for (var i = Math.Max(1, start); i < Math.Min(x.Length, start + count); i++)
            m = MathF.Max(m, MathF.Abs(x[i] - x[i - 1]));
        return m;
    }

    private static double Db(double a, double b) => 20.0 * Math.Log10(Math.Max(a, 1e-12) / Math.Max(b, 1e-12));

    /// RBJ band-pass, used to ask what a tail is doing at ONE frequency.
    private static float[] BandPass(float[] x, double fc, double q, int rate = Rate)
    {
        var w = 2.0 * Math.PI * fc / rate;
        var alpha = Math.Sin(w) / (2.0 * q);
        double b0 = alpha, b1 = 0, b2 = -alpha;
        double a0 = 1 + alpha, a1 = -2 * Math.Cos(w), a2 = 1 - alpha;
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
        var y = new float[x.Length];
        double z1 = 0, z2 = 0;
        for (var i = 0; i < x.Length; i++)
        {
            double v = x[i];
            var o = b0 * v + z1;
            z1 = b1 * v - a1 * o + z2;
            z2 = b2 * v - a2 * o;
            y[i] = (float)o;
        }
        return y;
    }

    /// T30, extrapolated: the time the short-window level takes to fall from
    /// -5 dB to -35 dB below its peak, doubled. Standard, and far more robust on
    /// a real tail than waiting for a literal 60 dB. The windows OVERLAP (50 ms
    /// long, 10 ms apart) and both crossings are interpolated inside the window
    /// they land in — a non-overlapping hop quantises the answer to twice the
    /// hop, which put a correct 1.0 s tail at 0.8 s.
    private static double Rt60(float[] x, int from, int rate = Rate)
    {
        var win = rate / 20;
        var hop = rate / 100;
        var levels = new List<double>();
        for (var i = from; i + win <= x.Length; i += hop) levels.Add(Rms(x, i, win));
        if (levels.Count < 8) return -1;
        var peak = levels.Max();
        var dbs = levels.Select(v => Db(v, peak)).ToList();
        var t5 = Crossing(dbs, -5, hop / (double)rate);
        var t35 = Crossing(dbs, -35, hop / (double)rate, t5 < 0 ? 0 : (int)(t5 * rate / hop));
        if (t5 < 0 || t35 < 0) return -1;
        return (t35 - t5) * 2.0;
    }

    /// First time the level curve crosses `target` dB, linearly interpolated.
    private static double Crossing(List<double> dbs, double target, double dt, int fromIndex = 0)
    {
        for (var i = Math.Max(1, fromIndex); i < dbs.Count; i++)
        {
            if (dbs[i] > target) continue;
            var span = dbs[i - 1] - dbs[i];
            var f = span > 1e-9 ? (dbs[i - 1] - target) / span : 0.0;
            return (i - 1 + f) * dt;
        }
        return -1;
    }

    // ======================================================================
    // catalogue + parameter surface (all four)
    // ======================================================================

    [NativeAvailableFact]
    public void EveryReverbModule_IsInTheCatalogue_InTheReverbDrawer()
    {
        var byId = JuceDspModuleCatalog.Modules.ToDictionary(m => m.Id);
        foreach (var id in Modules)
        {
            Assert.True(byId.ContainsKey(id), $"'{id}' is not in the module catalogue");
            Assert.Equal("reverb", byId[id].Category);
            Assert.NotEmpty(byId[id].Params);
        }
        // `space` is a NAMED discrete choice and must render as a picker.
        var space = byId["hallreverb"].Params.Single(p => p.Id == "space");
        Assert.True(space.IsNamed);
        Assert.Equal(4, space.Labels!.Count);
        Assert.Equal("Cathedral", space.Labels[3]);
        // …and the convolution's IR path is a FILE string param.
        var s = byId["convreverb"].StringParams.Single();
        Assert.Equal("irFile", s.Id);
        Assert.Equal("*.wav", s.FileFilter);
    }

    [NativeAvailableTheory]
    [InlineData("hallreverb")]
    [InlineData("convreverb")]
    [InlineData("gatedreverb")]
    [InlineData("reversereverb")]
    public void EveryReverbModule_GetParamReadsBackWhatSetParamStored(string module)
    {
        var info = JuceDspModuleCatalog.Modules.Single(m => m.Id == module);
        var n = Fresh(module);
        foreach (var p in info.Params)
        {
            // A value that is not the default, inside the range.
            var v = p.IsNamed
                ? MathF.Round(p.Min)
                : p.Min + 0.37f * (p.Max - p.Min);
            if (Math.Abs(v - p.Default) < 1e-6f) v = p.Max;
            Assert.Equal(Ok, DspSetParam(H, n, p.Id, v));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var back));
            Assert.True(Math.Abs(back - v) < 1e-3f,
                $"{module}.{p.Id}: set {v}, read back {back}");
        }
    }

    [NativeAvailableTheory]
    [InlineData("hallreverb")]
    [InlineData("convreverb")]
    [InlineData("gatedreverb")]
    [InlineData("reversereverb")]
    public void EveryReverbModule_ClampsOutOfRange_AndRejectsNaN(string module)
    {
        var info = JuceDspModuleCatalog.Modules.Single(m => m.Id == module);
        var n = Fresh(module);
        foreach (var p in info.Params)
        {
            Assert.Equal(Ok, DspSetParam(H, n, p.Id, p.Max * 1000f + 1f));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var hi));
            Assert.True(hi <= p.Max + 1e-3f, $"{module}.{p.Id} did not clamp high: {hi}");

            // A NaN must be REFUSED at the chain, and must not have moved the
            // value it was refused for. A NaN inside an FDN is permanent.
            var before = hi;
            Assert.NotEqual(Ok, DspSetParam(H, n, p.Id, float.NaN));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var after));
            Assert.Equal(before, after);
            Assert.False(float.IsNaN(after));
        }
        // …and the module is still alive and finite afterwards. Back to the
        // DEFAULTS first: the sweep above parks every knob at its maximum, and
        // for two of these modules that is a legitimately silent setting
        // (gatedreverb at thresholdDb 0 never opens; reversereverb at a 2 s
        // window is still inside its first swell).
        foreach (var p in info.Params)
            Assert.Equal(Ok, DspSetParam(H, n, p.Id, p.Default));
        Assert.Equal(Ok, DspSetParam(H, n, "mix", 1f));
        var y = Render(Noise(Rate * 2));
        Assert.All(y, v => Assert.True(float.IsFinite(v)));
        Assert.True(Rms(y, Rate, Rate / 2) > 1e-5, $"{module} went silent after the NaN sweep");
    }

    [NativeAvailableTheory]
    [InlineData("hallreverb")]
    [InlineData("convreverb")]
    [InlineData("gatedreverb")]
    [InlineData("reversereverb")]
    public void EveryReverbModule_ReDerivesEverythingTheRateOwns(string module)
    {
        // 48 kHz, then 96 kHz: the offline export re-prepares when the rate
        // moves, which is exactly the device change that has broken four cores
        // in this repo's history (tank lengths, damping corners, window sizes).
        var n = Fresh(module, "mix=1");
        var a = Render(Noise(Rate), Rate);
        var b = Render(Noise(2 * Rate), 2 * Rate);
        Assert.All(b, v => Assert.True(float.IsFinite(v)));
        var ra = Rms(a, PreRoll, Rate / 2);
        var rb = Rms(b, 2 * PreRoll, Rate);
        Assert.True(rb > 1e-4, $"{module}: silent after the rate change");
        // Same stimulus level in, so the wet level must not have moved by more
        // than a few dB — a tank whose lengths did not follow the rate is a
        // different room, and a damping corner that did not is a different tone.
        Assert.True(Math.Abs(Db(rb, ra)) < 6.0,
            $"{module}: the wet level moved {Db(rb, ra):F1} dB across a 48 -> 96 kHz change "
            + "— something the RATE owns was not re-derived");
        _ = n;
    }

    /// The mix/level ramp every module owes (DspGainZipperTests' MixCases shape,
    /// kept here so the reverb wave does not have to edit a shared file): three
    /// renders of the same stimulus — never moved, moved from the start, moved
    /// at the seam — and the seam must still be near the OLD value 128 samples
    /// in and have ARRIVED much later.
    [NativeAvailableTheory]
    [InlineData("hallreverb", "decayS=6", "mix")]
    [InlineData("convreverb", "predelayMs=0", "mix")]
    [InlineData("gatedreverb", "thresholdDb=-60;decayS=6", "mix")]
    [InlineData("reversereverb", "windowMs=200", "mix")]
    public void ChangingTheMix_WalksAcrossTheSeam_InsteadOfSwitching(
        string module, string setup, string param)
    {
        const int block = 2048;
        const int early = 128;
        const int lateFrom = 1200;

        float[] Seam(float first, float second)
        {
            var n = Fresh(module, setup);
            Assert.Equal(Ok, DspSetParam(H, n, param, first));
            Render(Noise(6 * block));                       // settle: rings, tanks, FIFOs
            var a = Render(Noise(block, 0.25f, 6 * block));
            Assert.Equal(Ok, DspSetParam(H, n, param, second));
            _ = a;
            return Render(Noise(block, 0.25f, 7 * block));
        }

        var refBefore = Seam(0f, 0f);
        var refAfter = Seam(1f, 1f);
        var seam = Seam(0f, 1f);

        var earlySpan = RmsDiff(refBefore, refAfter, 0, early);
        var lateSpan = RmsDiff(refBefore, refAfter, lateFrom, block - lateFrom);
        Assert.True(earlySpan > 0.005, $"{module}.{param}: vacuous (early span {earlySpan:F5})");
        Assert.True(lateSpan > 0.005, $"{module}.{param}: vacuous (late span {lateSpan:F5})");

        var earlyRatio = RmsDiff(seam, refBefore, 0, early) / earlySpan;
        Assert.True(earlyRatio < 0.5,
            $"{module}.{param}: {earlyRatio * 100:F0} % of the way to the new setting after "
            + $"{early} samples — a ramp is well under half, a hard switch is 100 %");
        var lateRatio = RmsDiff(seam, refAfter, lateFrom, block - lateFrom) / lateSpan;
        Assert.True(lateRatio < 0.2,
            $"{module}.{param}: still {lateRatio * 100:F0} % away from the new setting "
            + $"{block - lateFrom} samples later — the ramp never landed");
    }

    private static double RmsDiff(float[] a, float[] b, int start, int count)
    {
        double acc = 0;
        for (var i = start; i < start + count; i++)
        {
            double d = a[i] - b[i];
            acc += d * d;
        }
        return Math.Sqrt(acc / count);
    }

    // ======================================================================
    // hallreverb
    // ======================================================================

    [NativeAvailableTheory]
    [InlineData(1f, 0.8, 1.2)]
    [InlineData(4f, 3.2, 4.8)]
    public void HallReverb_Rt60FollowsDecaySeconds(float decayS, double lo, double hi)
    {
        // Damping wide open and the low shelf off, so what is measured is the
        // FDN's own decay law (g = 10^(-3 L / (RT60 fs)) per line) and not a
        // filter in the loop.
        Fresh("hallreverb", $"mix=1;erLevel=0;predelayMs=0;dampingKHz=16;lowCutHz=20;decayS={decayS.ToString(CultureInfo.InvariantCulture)}");
        var y = Render(Impulse(Rate * 12));
        var rt = Rt60(y, PreRoll);
        _out.WriteLine($"hallreverb decayS={decayS} -> measured RT60 {rt:F2} s");
        Assert.True(rt >= lo && rt <= hi,
            $"decayS {decayS} measured RT60 {rt:F2} s, expected {lo}..{hi}");
    }

    [NativeAvailableFact]
    public void HallReverb_DampingTiltsTheLateTail()
    {
        double At8k(float dampKHz)
        {
            Fresh("hallreverb", $"mix=1;erLevel=0;decayS=4;lowCutHz=20;dampingKHz={dampKHz.ToString(CultureInfo.InvariantCulture)}");
            var y = Render(Impulse(Rate * 4));
            return Rms(BandPass(y, 8000, 4), PreRoll + Rate / 2, Rate);
        }

        var dark = At8k(2f);
        var bright = At8k(16f);
        var delta = Db(bright, dark);
        _out.WriteLine($"hallreverb 8 kHz late-tail tilt: damping 2 kHz {dark:E3}, 16 kHz {bright:E3}, {delta:F1} dB");
        Assert.True(delta > 10.0,
            $"damping 2 vs 16 kHz moved the 8 kHz late tail by only {delta:F1} dB");
    }

    [NativeAvailableFact]
    public void HallReverb_IsStableAtTheLongestDecay()
    {
        // 20 s of RT60 is where a non-unitary mixing matrix rings UP instead of
        // down, and it does so slowly enough to look fine for a second.
        Fresh("hallreverb", "mix=1;erLevel=0;decayS=20;space=3;dampingKHz=16;lowCutHz=20");
        var y = Render(Impulse(Rate * 10));
        Assert.All(y, v => Assert.True(float.IsFinite(v)));

        var win = Rate / 10;
        var levels = new List<double>();
        for (var i = PreRoll + win; i + win <= y.Length; i += win) levels.Add(Rms(y, i, win));
        var peak = levels.Max();
        for (var i = 1; i < levels.Count; i++)
            Assert.True(levels[i] <= levels[i - 1] * 1.10,
                $"100 ms window {i} rose to {levels[i]:E3} from {levels[i - 1]:E3} — the tank is ringing up");
        Assert.True(levels[^1] < peak, "the tail never decayed at all");
        _out.WriteLine($"hallreverb decayS=20: peak {peak:E3}, after 9 s {levels[^1]:E3} ({Db(levels[^1], peak):F1} dB)");
    }

    [NativeAvailableFact]
    public void HallReverb_SpaceSwitchCrossfades_InsteadOfSplicingEightDelayLines()
    {
        const int len = Rate * 2;
        const int at = Rate;

        float[] Run(bool switchIt)
        {
            var n = Fresh("hallreverb", "mix=1;erLevel=0.5;decayS=4;space=3");
            var x = Sine(len);
            var head = new float[at];
            Array.Copy(x, head, at);
            var a = Render(head);
            if (switchIt) Assert.Equal(Ok, DspSetParam(H, n, "space", 0f));
            var tail = new float[len - at];
            Array.Copy(x, at, tail, 0, len - at);
            return Join(a[^1], Render(tail));
        }

        var steady = Run(false);
        var switched = Run(true);
        // The crossfade is 40 ms; a splice would land inside the first few
        // hundred samples of it.
        var quiet = MaxStep(steady, 0, 4000);
        var moved = MaxStep(switched, 0, 4000);
        _out.WriteLine($"hallreverb space switch: max step {moved:F5} against a steady {quiet:F5}");
        Assert.True(quiet > 1e-4, "the reference render is silent — the case is vacuous");
        Assert.True(moved < quiet * 2.0f,
            $"the space switch stepped by {moved:F5}, against {quiet:F5} for the same audio with "
            + "no switch — the geometry was written into the LIVE tank");
        // …and it really did change room.
        Assert.True(RmsDiff(steady, switched, 8000, 20000) > 1e-4,
            "the space switch changed nothing — the case is vacuous");
    }

    [NativeAvailableFact]
    public void HallReverb_PredelayGlides_InsteadOfTeleportingTheReadTap()
    {
        const int len = Rate;
        float[] Run(bool move)
        {
            var n = Fresh("hallreverb", "mix=1;erLevel=0;decayS=3;predelayMs=10;diffusion=0");
            var head = Render(Sine(Rate / 2));
            if (move) Assert.Equal(Ok, DspSetParam(H, n, "predelayMs", 60f));
            return Join(head[^1], Render(Sine(len, 0.25f, Rate / 2)));
        }

        var steady = Run(false);
        var moved = Run(true);
        var quiet = MaxStep(steady, 0, 3000);
        var jump = MaxStep(moved, 0, 3000);
        _out.WriteLine($"hallreverb predelay 10 -> 60 ms: max step {jump:F5} against {quiet:F5}");
        Assert.True(quiet > 1e-4, "vacuous: the reference render is silent");
        // A 50 ms glide over the 20 ms ramp walks the read head ~2.5 source
        // samples per output sample, which is a pitch bend, not a step; a
        // teleport is an arbitrary phase jump and lands near the wet's peak.
        Assert.True(jump < quiet * 2f,
            $"a 50 ms predelay move stepped by {jump:F5} against {quiet:F5} — the tap teleported");
    }

    // ======================================================================
    // convreverb
    // ======================================================================

    /// The convolution has to BE a convolution. A Dirac in, and what comes out
    /// must be the loaded IR — shape for shape, to a tolerance that no
    /// mis-partitioned overlap-save, no off-by-one in the frequency-delay line
    /// and no wrong-sign twiddle could survive.
    ///
    /// It is compared PEAK-NORMALISED, because the core energy-normalises what
    /// it loads on purpose (see dcdsp_convreverb.h: the shared WAV reader's L1
    /// bound is a 40 dB cut on a two-second room). The scalar is the one degree
    /// of freedom given away; every one of the 96,000 sample RATIOS is pinned.
    [NativeAvailableFact]
    public void ConvReverb_ConvolvingADirac_ReproducesTheLoadedIr()
    {
        var ir = SyntheticIr(2 * Rate, seed: 0xBEEF);
        var wav = WriteFloatWav(ir, Rate);
        try
        {
            var n = Fresh("convreverb", "mix=1;predelayMs=0;lengthPct=100;lowCutHz=20;highCutKHz=20");
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.EngineDspSetNodeString(H, n, "irFile", wav));

            var latency = NodeLatency(n);
            Assert.True(latency > 0, "a uniform-partition convolution reports zero latency");

            var y = Render(Impulse(3 * Rate));
            var start = PreRoll + latency;

            // Peak-normalise both and compare sample for sample.
            var yPeak = 0f; for (var i = 0; i < ir.Length; i++) yPeak = MathF.Max(yPeak, MathF.Abs(y[start + i]));
            var hPeak = 0f; foreach (var v in ir) hPeak = MathF.Max(hPeak, MathF.Abs(v));
            Assert.True(yPeak > 1e-6f && hPeak > 1e-6f, "vacuous: nothing came out");

            var worst = 0.0;
            var worstAt = -1;
            for (var i = 0; i < ir.Length; i++)
            {
                var d = Math.Abs(y[start + i] / yPeak - ir[i] / hPeak);
                if (d > worst) { worst = d; worstAt = i; }
            }
            _out.WriteLine($"convreverb Dirac vs IR: worst peak-normalised error {worst:E3} at tap {worstAt} "
                           + $"over {ir.Length} taps (latency {latency})");
            Assert.True(worst < 1e-4,
                $"the convolution differs from the IR by {worst:E3} at tap {worstAt}");
        }
        finally { TryDelete(wav); }
    }

    [NativeAvailableFact]
    public void ConvReverb_LengthPctShortensTheTail()
    {
        var ir = SyntheticIr(2 * Rate, seed: 0x5151);
        var wav = WriteFloatWav(ir, Rate);
        try
        {
            double Tail(float pct)
            {
                var n = Fresh("convreverb", "mix=1;predelayMs=0;lowCutHz=20;highCutKHz=20");
                Assert.Equal(NativeMethods.DcaStatus.Ok,
                    NativeMethods.EngineDspSetNodeString(H, n, "irFile", wav));
                Assert.Equal(Ok, DspSetParam(H, n, "lengthPct", pct));
                var y = Render(Impulse(3 * Rate));
                // The last quarter-second of the FULL IR: present at 100 %, gone at 50 %.
                return Rms(y, PreRoll + NodeLatency(n) + Rate * 7 / 4, Rate / 4);
            }

            var full = Tail(100f);
            var half = Tail(50f);
            _out.WriteLine($"convreverb lengthPct 100 -> 50: last-quarter tail {full:E3} -> {half:E3} ({Db(half, full):F1} dB)");
            Assert.True(full > 1e-6, "vacuous: the full IR has no tail there");
            Assert.True(Db(half, full) < -30.0,
                $"lengthPct 50 left {Db(half, full):F1} dB of the truncated region — it did not truncate");
        }
        finally { TryDelete(wav); }
    }

    /// <summary>
    ///     The two backwards readers SUM to a constant, at every
    ///     <c>swellShape</c> — and stay half a window apart across a
    ///     <c>windowMs</c> change.
    ///
    ///     <para>The header claimed COLA of the Hann factor and applied
    ///     <c>hann(x) * x^gamma</c>, which is not: the pair's sum walked 0.088
    ///     to 0.187 at <c>swellShape</c> 1, a 6.6 dB pump at <c>2 / windowMs</c>
    ///     — 2.5 Hz for the default window, squarely audible. And the two
    ///     readers latch a new window at their OWN wraps, half a window apart,
    ///     so a change left the offset wherever it happened to land; near zero
    ///     both envelopes reach their zero together and the wet path NULLS once
    ///     per window, with nothing to restore the relationship short of a
    ///     reset.</para>
    ///
    ///     <para>The instrument is the ENVELOPE, measured in 10 ms frames on a
    ///     steady noise input: a constant-sum pair with a steady input has a
    ///     flat one, whatever the shape.</para>
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(0f, 800f, 800f)]
    [InlineData(0.5f, 800f, 800f)]
    [InlineData(1f, 800f, 800f)]
    [InlineData(0.5f, 800f, 2000f)]   // …and across a window change
    [InlineData(1f, 2000f, 500f)]
    public void ReverseReverb_TheTwoReadersSumToAConstant(float shape, float fromMs, float toMs)
    {
        var n = Fresh("reversereverb",
            $"mix=1;decayS=2;predelayMs=0;dampingKHz=16;"
            + $"swellShape={shape.ToString(CultureInfo.InvariantCulture)};"
            + $"windowMs={fromMs.ToString(CultureInfo.InvariantCulture)}");

        // Prime: both readers have to be inside REAL captured audio, not the
        // ring's initial zeros, at the LONGER of the two windows.
        // 6 s, not `Prime`: a 2,000 ms window looks back up to 2 x 2,000 ms, so
        // a shorter prime leaves the readers inside the ring's initial zeros and
        // the dip it measures is emptiness, not phase.
        const int Warm = 6 * Rate;
        var noise = Noise(Warm + 8 * Rate);
        Render(noise[..Warm]);
        if (toMs != fromMs) Assert.Equal(Ok, DspSetParam(H, n, "windowMs", toMs));

        // Four seconds after the change: long enough for the transitional
        // window to have run and for several laps of the new one.
        var y = Render(noise[Warm..]);
        var skip = 4 * Rate;

        const int Frame = Rate / 100;                 // 10 ms
        var frames = new List<double>();
        for (var i = skip; i + Frame <= y.Length; i += Frame) frames.Add(Rms(y, i, Frame));

        var mean = frames.Average();
        var min = frames.Min();
        var max = frames.Max();
        var ripple = (max - min) / mean;
        _out.WriteLine($"reversereverb shape {shape} {fromMs}->{toMs} ms: envelope min {min:E3}, "
            + $"mean {mean:E3}, max {max:E3}, ripple {ripple * 100:F0} %, "
            + $"{Db(min, mean):F1} dB below the mean at the dip");

        Assert.True(mean > 1e-4, "the wet is silent — the case is vacuous");
        Assert.True(Db(min, mean) > -3.0,
            $"the wet envelope dips {Db(min, mean):F1} dB below its mean — the two readers do "
            + "not sum to a constant, or they are no longer half a window apart");
    }

    [NativeAvailableFact]
    public void ConvReverb_ASweepOfLengthPct_StaysFinite_AndReturnsToWhereItStarted()
    {
        // `lengthPct` is a continuous knob, so a UI drag issues one full IR
        // rebuild per tick on ONE node — every step re-partitioning and
        // re-transforming a two-second IR while the callback plays the previous
        // snapshot. This walks it down and back up on a single node.
        //
        // The return leg is the load-bearing half: the rate conversion feeding
        // the rebuild is CACHED (it cannot change when only the kept fraction
        // moves, and re-running it per tick was the expensive part), and a cache
        // that held post-truncation taps would make a shortened IR permanent.
        var ir = SyntheticIr(2 * Rate, seed: 0x4A17);
        var wav = WriteFloatWav(ir, Rate);
        try
        {
            var n = Fresh("convreverb", "mix=1;predelayMs=0;lowCutHz=20;highCutKHz=20");
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.EngineDspSetNodeString(H, n, "irFile", wav));
            var skip = PreRoll + NodeLatency(n);

            (double Total, double Late) At(float pct)
            {
                Assert.Equal(Ok, DspSetParam(H, n, "lengthPct", pct));
                var y = Render(Impulse(3 * Rate));
                Assert.All(y, v => Assert.True(float.IsFinite(v),
                    $"convreverb emitted a non-finite sample at lengthPct {pct}"));
                // Total energy is what must fall MONOTONELY; the last quarter of
                // the full IR is where a truncation is unmissable. A decaying
                // IR keeps most of its energy in the first tenth, so the total
                // alone moves only ~2 dB across the whole sweep.
                return (Rms(y, skip, 2 * Rate), Rms(y, skip + Rate * 7 / 4, Rate / 4));
            }

            var down = new List<(double Total, double Late)>();
            for (var pct = 100f; pct >= 10f; pct -= 10f) down.Add(At(pct));
            var backAt100 = At(100f);

            _out.WriteLine("convreverb lengthPct 100 -> 10, total energy: "
                + string.Join(", ", down.Select(v => v.Total.ToString("E2", CultureInfo.InvariantCulture)))
                + $"; back at 100: {backAt100.Total:E2}. Late tail 100 % {down[0].Late:E3} -> 10 % {down[^1].Late:E3}");

            Assert.True(down[0].Total > 1e-5, "vacuous: the full IR renders nothing");
            Assert.True(down[0].Late > 1e-6, "vacuous: the full IR has no late tail to truncate");
            for (var i = 1; i < down.Count; i++)
                Assert.True(down[i].Total <= down[i - 1].Total * 1.001,
                    $"step {i} of the lengthPct sweep RAISED the wet energy "
                    + $"({down[i - 1].Total:E3} -> {down[i].Total:E3})");
            Assert.True(Db(down[^1].Late, down[0].Late) < -30.0,
                $"lengthPct 10 left {Db(down[^1].Late, down[0].Late):F1} dB of the truncated region");
            Assert.True(Math.Abs(Db(backAt100.Total, down[0].Total)) < 0.1,
                $"returning to lengthPct 100 landed {Db(backAt100.Total, down[0].Total):F2} dB from where the sweep started");
            Assert.True(Math.Abs(Db(backAt100.Late, down[0].Late)) < 0.1,
                $"the late tail did not come back: {Db(backAt100.Late, down[0].Late):F2} dB");
        }
        finally { TryDelete(wav); }
    }

    [NativeAvailableFact]
    public void ConvReverb_ABadIrFile_IsRefused_AndThePreviousIrKeepsPlaying()
    {
        var ir = SyntheticIr(Rate / 2, seed: 0x0C0C);
        var wav = WriteFloatWav(ir, Rate);
        try
        {
            var n = Fresh("convreverb", "mix=1;lowCutHz=20;highCutKHz=20");
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.EngineDspSetNodeString(H, n, "irFile", wav));
            var good = Render(Impulse(Rate));

            var missing = Path.Combine(Path.GetTempPath(), "no-such-ir-" + Guid.NewGuid().ToString("N") + ".wav");
            Assert.NotEqual(NativeMethods.DcaStatus.Ok,
                NativeMethods.EngineDspSetNodeString(H, n, "irFile", missing));
            // The path did not change…
            Assert.Equal(wav, GetNodeString(n, "irFile"));
            // …and neither did the sound. A reverb that goes silent because a
            // path moved is worse than one that ignores the request.
            var after = Render(Impulse(Rate));
            Assert.True(Rms(after, PreRoll, Rate / 2) > 1e-5, "the module went silent on a bad file");
            Assert.True(RmsDiff(good, after, PreRoll, Rate / 2) < 1e-6,
                "a refused IR load still changed the audio");
        }
        finally { TryDelete(wav); }
    }

    [NativeAvailableFact]
    public void ConvReverb_TheLoadedIrSurvivesADeviceChange()
    {
        // prepare() re-derives everything the rate owns and clears TANKS, but a
        // loaded IR is USER CONTENT: it is re-converted from the original
        // decoded samples, never dropped (mikustomp lost its lyrics this way).
        var ir = SyntheticIr(Rate, seed: 0xD00D);
        var wav = WriteFloatWav(ir, Rate);
        try
        {
            var n = Fresh("convreverb", "mix=1;lowCutHz=20;highCutKHz=20");
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.EngineDspSetNodeString(H, n, "irFile", wav));
            var at48 = Render(Impulse(Rate * 2), Rate);

            var at96 = Render(Impulse(Rate * 4, PreRoll * 2), 2 * Rate);
            Assert.Equal(wav, GetNodeString(n, "irFile"));
            Assert.All(at96, v => Assert.True(float.IsFinite(v)));

            var a = Rms(at48, PreRoll, Rate);
            var b = Rms(at96, 2 * PreRoll, 2 * Rate);
            _out.WriteLine($"convreverb across 48 -> 96 kHz: {a:E3} -> {b:E3} ({Db(b, a):F1} dB)");
            Assert.True(b > 1e-5, "the IR did not survive the rate change — the module is silent");
            Assert.True(Math.Abs(Db(b, a)) < 6.0,
                $"the wet level moved {Db(b, a):F1} dB across the rate change");
        }
        finally { TryDelete(wav); }
    }

    [NativeAvailableFact]
    public void ConvReverb_MakesSoundWithNoFileLoaded()
    {
        var n = Fresh("convreverb", "mix=1");
        Assert.Equal(string.Empty, GetNodeString(n, "irFile"));
        var y = Render(Impulse(Rate));
        Assert.True(Rms(y, PreRoll, Rate / 2) > 1e-4,
            "a convolution reverb with no IR loaded produced silence — the built-in default is missing");
    }

    // ======================================================================
    // gatedreverb
    // ======================================================================

    [NativeAvailableFact]
    public void GatedReverb_CutsTheTail_WithinTheHoldPlusRelease()
    {
        const float gateMs = 250f, releaseMs = 20f;
        var n = Fresh("gatedreverb",
            $"mix=1;decayS=8;thresholdDb=-40;gateMs={gateMs.ToString(CultureInfo.InvariantCulture)};releaseMs={releaseMs.ToString(CultureInfo.InvariantCulture)}");

        // A half-second burst, then silence.
        var burstEnd = PreRoll + Rate / 2;
        var x = Noise(Rate * 3);
        for (var i = 0; i < x.Length; i++) if (i < PreRoll || i >= burstEnd) x[i] = 0f;
        var y = Render(x);

        var during = Rms(y, burstEnd - Rate / 4, Rate / 8);
        Assert.True(during > 1e-3, "the wet is not present during the burst — the case is vacuous");

        var cutAt = burstEnd + (int)((gateMs + releaseMs + 20f) * Rate / 1000f);
        var afterGate = Rms(y, cutAt, Rate / 10);
        _out.WriteLine($"gatedreverb: during {during:E3}, {gateMs + releaseMs + 20f:F0} ms after the burst {afterGate:E3} ({Db(afterGate, during):F1} dB)");
        Assert.True(Db(afterGate, during) < -30.0,
            $"the tail was only {Db(afterGate, during):F1} dB down {gateMs + releaseMs + 20f:F0} ms after the input stopped");

        // …and the gate was still holding it OPEN before that, or the number
        // above would be a reverb that simply had no tail.
        var inHold = Rms(y, burstEnd + Rate / 50, Rate / 50);
        Assert.True(Db(inHold, during) > -20.0,
            $"the gate closed during its own {gateMs} ms hold ({Db(inHold, during):F1} dB)");
        _ = n;
    }

    [NativeAvailableTheory]
    [InlineData(-60f, true)]
    [InlineData(0f, false)]
    public void GatedReverb_ThresholdFlipsGatingOnAndOff(float thresholdDb, bool expectOpen)
    {
        // The stimulus sits at -12 dBFS, so -60 dB is always open and 0 dB is
        // never open.
        Fresh("gatedreverb",
            $"mix=1;decayS=4;gateMs=20;releaseMs=5;thresholdDb={thresholdDb.ToString(CultureInfo.InvariantCulture)}");
        var y = Render(Noise(Rate));
        var level = Rms(y, PreRoll + Rate / 4, Rate / 4);
        _out.WriteLine($"gatedreverb threshold {thresholdDb} dB -> wet {level:E3}");
        if (expectOpen) Assert.True(level > 1e-3, $"the gate never opened at {thresholdDb} dB");
        else Assert.True(level < 1e-5, $"the gate never closed at {thresholdDb} dB (wet {level:E3})");
    }

    // ======================================================================
    // reversereverb
    // ======================================================================

    [NativeAvailableFact]
    public void ReverseReverb_EnvelopeRisesAcrossTheWindow()
    {
        const int windowMs = 800;
        var win = windowMs * Rate / 1000;
        Fresh("reversereverb", $"mix=1;decayS=4;swellShape=1;windowMs={windowMs}");

        var y = Render(Impulse(Rate * 4));
        // One whole playback window, starting a window after the impulse — by
        // then the ring holds the tail and a backwards head is inside it.
        var from = PreRoll + win;
        var q = win / 4;
        var first = Rms(y, from, q);
        var last = Rms(y, from + 3 * q, q);
        _out.WriteLine($"reversereverb: first quarter {first:E3}, last quarter {last:E3} ({Db(last, first):F1} dB)");
        Assert.True(last > 1e-6, "vacuous: nothing came out");
        Assert.True(Db(last, first) > 10.0,
            $"the window envelope rose by only {Db(last, first):F1} dB — a reverse reverb SWELLS");
    }

    [NativeAvailableFact]
    public void ReverseReverb_WindowChangeIsLatchedAtAZeroCrossingOfItsOwnEnvelope()
    {
        const int len = Rate * 2;
        float[] Run(bool move)
        {
            var n = Fresh("reversereverb", "mix=1;decayS=5;dampingKHz=16;windowMs=800;swellShape=0.5");
            // 2.5 s of pre-roll, not one: a backwards head reaches 2 x windowMs
            // behind the write head, so with an 800 ms window the SECOND reader
            // is still reading the ring's initial zeros a second in — killing a
            // reader that is playing silence costs nothing, and that is why this
            // probe read no contrast at all until the pre-roll was lengthened.
            var head = Render(Sine(Prime));
            if (move) Assert.Equal(Ok, DspSetParam(H, n, "windowMs", 100f));
            // The change is applied at sample 0 of the NEXT block, so the step
            // it makes is the one ACROSS the block boundary — measuring from
            // sample 1 of the new block cannot see it at all, and this probe
            // passed with the fault in for exactly that reason.
            return Join(head[^1], Render(Sine(len, 0.25f, Prime)));
        }

        var steady = Run(false);
        var moved = Run(true);
        var quiet = MaxStep(steady, 0, Rate / 2);
        var jump = MaxStep(moved, 0, Rate / 2);
        _out.WriteLine($"reversereverb windowMs 800 -> 100: max step {jump:F5} against {quiet:F5}");
        Assert.True(quiet > 1e-4, "vacuous: the reference render is silent");
        Assert.True(jump < quiet * 3.0f,
            $"a window change stepped by {jump:F5} against {quiet:F5} — it was latched mid-window");
        Assert.True(RmsDiff(steady, moved, Rate / 2, Rate) > 1e-4,
            "the window change did nothing — the case is vacuous");
    }

    // ======================================================================
    // helpers: a float32 WAV with EXACTLY the samples we want
    // ======================================================================

    /// A decaying noise burst with a direct arrival — the shape of a real room
    /// IR, deterministic, and broadband enough that a wrong FFT bin shows up.
    private static float[] SyntheticIr(int taps, uint seed)
    {
        var h = new float[taps];
        var rng = seed;
        for (var i = 0; i < taps; i++)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            var w = ((rng >> 8) * (1f / 8388608f)) - 1f;
            h[i] = w * MathF.Exp(-5f * i / taps);
        }
        h[0] = 1f;
        return h;
    }

    private static string WriteFloatWav(float[] samples, int rate)
    {
        var dir = Path.Combine(Path.GetTempPath(), "JuceSharpTests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "ir.wav");
        using var fs = File.Create(path);
        using var bw = new BinaryWriter(fs);
        var dataSize = samples.Length * 4;
        bw.Write(Encoding.ASCII.GetBytes("RIFF"));
        bw.Write(36 + dataSize);
        bw.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
        bw.Write(16);
        bw.Write((short)3);            // IEEE float
        bw.Write((short)1);            // mono
        bw.Write(rate);
        bw.Write(rate * 4);
        bw.Write((short)4);
        bw.Write((short)32);
        bw.Write(Encoding.ASCII.GetBytes("data"));
        bw.Write(dataSize);
        foreach (var s in samples) bw.Write(s);
        return path;
    }

    private static void TryDelete(string path)
    {
        try
        {
            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir)) Directory.Delete(dir, true);
        }
        catch { /* best-effort */ }
    }

    private string GetNodeString(int node, string param)
    {
        var len = NativeMethods.EngineDspGetNodeString(H, node, param, null, 0);
        if (len < 0) return string.Empty;
        var buf = new byte[len + 1];
        NativeMethods.EngineDspGetNodeString(H, node, param, buf, buf.Length);
        return Encoding.UTF8.GetString(buf, 0, len);
    }

    private int NodeLatency(int node)
    {
        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.EngineDspGetNodeLatency(H, node, out var samples));
        return samples;
    }
}
