using System.Globalization;
using System.Runtime.InteropServices;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// Two module extensions DotCharter's converted song tones use (#155, #157),
/// rendered through the SHIPPING wiring (catalogue row, adapter mailbox,
/// <c>DspChain</c>) on a stopped engine via <c>dca_engine_dsp_process_buffer</c>:
/// <list type="bullet">
///   <item><c>gate.autoFloor</c> — the threshold follows the input's measured noise
///   floor (<c>dcdsp_gate.h</c>);</item>
///   <item><c>bitcrusher</c>'s FutzBox-style stages — input / drive / intensity / rectify /
///   filter / output on the crushed path (<c>dcdsp_bitcrusher.h</c>), which at
///   their defaults leave the classic crusher bit for bit.</item>
/// </list>
/// The allocation gate is the RT scenario
/// <c>RtAutoFloorGateAndFutzCrusherAllocateNothing</c>; the new levels' ramps are
/// rows in <see cref="DspGainZipperTests"/>.
/// </summary>
[Collection("NativeEngine")]
public sealed class GateAutoFloorAndFutzCrusherTests : IDisposable
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
    private const int Block = 512;

    private readonly JuceAudioEngine? _engine;

    public GateAutoFloorAndFutzCrusherTests()
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

    private int Fresh(string module, string setup)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        foreach (var pair in setup.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = pair.Split('=');
            Assert.Equal(Ok, DspSetParam(H, n, kv[0], float.Parse(kv[1], CultureInfo.InvariantCulture)));
        }

        return n;
    }

    /// Mono in, left out, in blocks; the chain's 10 ms node fade-in is the first
    /// half block of whatever is rendered first.
    private float[] Render(float[] input, int rate = Sr)
    {
        var output = new float[input.Length];
        var l = new float[Block];
        var r = new float[Block];
        for (var off = 0; off < input.Length; off += Block)
        {
            var n = Math.Min(Block, input.Length - off);
            Array.Copy(input, off, l, 0, n);
            Array.Copy(input, off, r, 0, n);
            Assert.Equal(Ok, DspProcess(H, l, r, n, rate));
            Array.Copy(l, 0, output, off, n);
        }

        return output;
    }

    /// Near-Gaussian hiss at <paramref name="rmsDb"/> RMS, fixed seed.
    private static float[] Hiss(int samples, double rmsDb, uint seed = 0x1D1E)
    {
        var x = new double[samples];
        var s = seed;
        for (var i = 0; i < samples; i++)
        {
            double v = 0;
            for (var k = 0; k < 4; k++)
            {
                s = unchecked(s * 1664525u + 1013904223u);
                v += (s >> 8) / (double)(1 << 24) * 2.0 - 1.0;
            }

            x[i] = v;
        }

        var rms = Math.Sqrt(x.Sum(v => v * v) / samples);
        var g = Math.Pow(10, rmsDb / 20) / rms;
        return x.Select(v => (float)(v * g)).ToArray();
    }

    private static double RmsDb(float[] x, int from, int to)
    {
        double s = 0;
        for (var i = from; i < to; i++) s += (double)x[i] * x[i];
        return 20 * Math.Log10(Math.Sqrt(s / Math.Max(1, to - from)) + 1e-20);
    }

    // ================================================================ gate

    private const string AutoGate = "autoFloor=1;thresholdDb=0;ratio=50;attackMs=1;holdMs=0;releaseMs=10";

    [NativeAvailableFact]
    public void AutoFloor_ReadsBack_AndDefaultsOff()
    {
        var n = Fresh("gate", "");
        Assert.Equal(Ok, DspGetParam(H, n, "autoFloor", out var off));
        Assert.Equal(0f, off);
        Assert.Equal(Ok, DspSetParam(H, n, "autoFloor", 1f));
        Assert.Equal(Ok, DspGetParam(H, n, "autoFloor", out var on));
        Assert.Equal(1f, on);
    }

    /// The claim: whatever the interface's noise, the gate measures it and shuts
    /// on it. Hiss at -70 and at -55 dBFS RMS, neither of which the FIXED
    /// threshold (here 0 dB = the ceiling, never reached) knows about, both come
    /// out at least 40 dB down once a second has been measured.
    [NativeAvailableTheory]
    [InlineData(-70.0)]
    [InlineData(-55.0)]
    public void AutoFloor_ShutsOnAStationaryNoiseFloor_WhateverItsLevel(double hissDb)
    {
        Fresh("gate", AutoGate);
        var x = Hiss(4 * Sr, hissDb);
        var y = Render(x);
        var drop = RmsDb(x, 3 * Sr, 4 * Sr) - RmsDb(y, 3 * Sr, 4 * Sr);
        Assert.True(drop > 40, $"hiss at {hissDb} dBFS came out only {drop:F1} dB down");
    }

    /// A single-coil's idle: hiss plus 50 Hz mains hum rich in harmonics (to
    /// 750 Hz at 1/n), a periodic floor rather than a random one. Shut at least
    /// 40 dB, like the hiss.
    [NativeAvailableFact]
    public void AutoFloor_ShutsOnHissAndHarmonicRichHum()
    {
        Fresh("gate", AutoGate);
        var x = Hiss(4 * Sr, -78);
        var hum = new double[x.Length];
        for (var i = 0; i < x.Length; i++)
            for (var n = 1; n <= 15; n++) hum[i] += Math.Sin(2 * Math.PI * 50 * n * i / Sr + n * 0.7) / n;
        var g = Math.Pow(10, -72 / 20.0) / Math.Sqrt(hum.Sum(v => v * v) / hum.Length);
        for (var i = 0; i < x.Length; i++) x[i] += (float)(hum[i] * g);
        var y = Render(x);
        var drop = RmsDb(x, 3 * Sr, 4 * Sr) - RmsDb(y, 3 * Sr, 4 * Sr);
        Assert.True(drop > 40, $"hiss + hum came out only {drop:F1} dB down");
    }

    /// A floor measured on digital silence (a rig that started with nothing
    /// plugged in, or fed by the chart voice alone) still follows a louder one
    /// UP: two seconds of silence, then -60 dBFS hiss, shut again within 3 s.
    [NativeAvailableFact]
    public void AutoFloor_FollowsALouderFloor_AfterSilence()
    {
        Fresh("gate", AutoGate);
        var x = new float[7 * Sr];
        Array.Copy(Hiss(5 * Sr, -60), 0, x, 2 * Sr, 5 * Sr);
        var y = Render(x);
        var drop = RmsDb(x, 6 * Sr, 7 * Sr) - RmsDb(y, 6 * Sr, 7 * Sr);
        Assert.True(drop > 40, $"hiss arriving after silence came out only {drop:F1} dB down 4 s later");
    }

    /// Off, the same gate at the same settings passes that hiss (a 0 dB
    /// threshold would SHUT it; so the fixed case is set at -80, under the hiss).
    [NativeAvailableFact]
    public void FixedThreshold_UnderTheHiss_PassesIt()
    {
        Fresh("gate", "autoFloor=0;thresholdDb=-80;ratio=50;attackMs=1;holdMs=0;releaseMs=10");
        var x = Hiss(4 * Sr, -70);
        var y = Render(x);
        var drop = RmsDb(x, 3 * Sr, 4 * Sr) - RmsDb(y, 3 * Sr, 4 * Sr);
        Assert.True(drop < 1, $"a fixed -80 dB gate cut -70 dBFS hiss by {drop:F1} dB");
    }

    /// The other half: it never shuts on PLAYING. After two seconds of -80 dBFS
    /// hiss (the floor is measured), a pluck decaying 20 dB/s from -20 dBFS peak
    /// passes untouched until it is within a few dB of the floor: at the point it
    /// has decayed to -50 dBFS (30 dB above the hiss) the output still equals the
    /// input, and the decay is NOT taken for a floor (it is not stationary).
    [NativeAvailableFact]
    public void AutoFloor_PassesADecayingNote_DownToNearTheFloor()
    {
        Fresh("gate", AutoGate);
        var hiss = Hiss(6 * Sr, -80);
        var x = (float[])hiss.Clone();
        var start = 2 * Sr;
        for (var i = start; i < x.Length; i++)
        {
            var t = (i - start) / (double)Sr;
            x[i] += (float)(0.1 * Math.Pow(10, -20 * t / 20) * Math.Sin(2 * Math.PI * 110 * t));
        }

        var y = Render(x);
        // 1.5 s in, the pluck has decayed 30 dB: -50 dBFS peak, 30 dB over the hiss.
        var at = start + (int)(1.5 * Sr);
        var inDb = RmsDb(x, at, at + 2400);
        var outDb = RmsDb(y, at, at + 2400);
        Assert.True(Math.Abs(inDb - outDb) < 0.5, $"the decay at {inDb:F1} dBFS came out at {outDb:F1}");
        // ...and before the note the hiss WAS gated, so the gate was really measuring.
        Assert.True(RmsDb(x, Sr + Sr / 2, start) - RmsDb(y, Sr + Sr / 2, start) > 40, "the hiss before the note was not gated");
    }

    /// Before its first reading the gate assumes digital silence (one 16-bit LSB,
    /// -96.3 dBFS), the most open its auto threshold gets: a -87 dBFS tone played
    /// from the first sample — never stationary, so never a reading — passes
    /// untouched. (A higher assumed floor, e.g. -90 dB with its -84 dB threshold,
    /// expands it to nothing.)
    [NativeAvailableFact]
    public void AutoFloor_BeforeItsFirstReading_AssumesDigitalSilence()
    {
        Fresh("gate", AutoGate);
        var x = new float[Sr * 3 / 4];
        var amp = Math.Pow(10, -87.0 / 20);
        for (var i = 0; i < x.Length; i++) x[i] = (float)(amp * Math.Sin(2 * Math.PI * 440 * i / Sr));
        var y = Render(x);
        var drop = RmsDb(x, Sr / 4, x.Length) - RmsDb(y, Sr / 4, x.Length);
        Assert.True(Math.Abs(drop) < 1, $"a -87 dBFS tone before the first reading came out {drop:F1} dB down");
    }

    /// The ceiling holds: auto mode never sets a threshold above thresholdDb, so
    /// hiss louder than the ceiling passes.
    [NativeAvailableFact]
    public void AutoFloor_NeverRisesAboveItsCeiling()
    {
        Fresh("gate", "autoFloor=1;thresholdDb=-60;ratio=50;attackMs=1;holdMs=0;releaseMs=10");
        var x = Hiss(4 * Sr, -55);
        var y = Render(x);
        var drop = RmsDb(x, 3 * Sr, 4 * Sr) - RmsDb(y, 3 * Sr, 4 * Sr);
        Assert.True(drop < 1, $"hiss above the -60 dB ceiling was cut by {drop:F1} dB");
    }

    // =========================================================== bitcrusher

    [NativeAvailableFact]
    public void Crusher_NewParams_ReadBack_AndClamp()
    {
        var n = Fresh("bitcrusher", "");
        foreach (var (id, v, expect) in new[]
                 {
                     ("inputDb", 60f, 48f), ("drive", 150f, 100f), ("intensity", -5f, 0f),
                     ("rectify", 120f, 100f), ("filter", 50f, 50f), ("outputDb", -60f, -48f)
                 })
        {
            Assert.Equal(Ok, DspSetParam(H, n, id, v));
            Assert.Equal(Ok, DspGetParam(H, n, id, out var got));
            Assert.True(Math.Abs(got - expect) < 1e-4 || Math.Abs(got - v) < 1e-4,
                $"{id}: wrote {v}, read back {got}");
        }
    }

    /// At their defaults the FutzBox stages are not there: the output is the
    /// classic quantize-and-hold crusher, computed here by hand, sample for sample.
    [NativeAvailableFact]
    public void Crusher_AtDefaults_IsTheClassicCrusher()
    {
        Fresh("bitcrusher", "bits=4;downsample=5;mix=0.6");
        var x = new float[Sr / 2];
        for (var i = 0; i < x.Length; i++) x[i] = (float)(0.3 * Math.Sin(2 * Math.PI * 97 * i / Sr));
        var y = Render(x);

        var step = 2f / 16f;
        var held = 0f;
        var worst = 0f;
        for (var i = Block; i < x.Length; i++)   // past the chain's node fade-in
        {
            if (i % 5 == 0) held = MathF.Floor(x[i] / step + 0.5f) * step;
            var expected = 0.4f * x[i] + 0.6f * held;
            worst = MathF.Max(worst, MathF.Abs(expected - y[i]));
        }

        Assert.True(worst < 1e-5f, $"the default crusher differs from quantize-and-hold by {worst}");
    }

    /// The drive stage's claim (#157): the distortion SATURATES the crushed path
    /// before the quantizer, so a QUIET input is crushed as a full-scale wave and
    /// comes out at the output level, instead of sputtering between zero and one
    /// step. A -40 dBFS sine through 4 bits: plain, it rounds to silence; with
    /// +24 dB in and Drive 60 it is a saturated wave at the -32 dB output level.
    [NativeAvailableFact]
    public void Crusher_Drive_CrushesAQuietInputAsASaturatedWave()
    {
        var x = new float[Sr];
        for (var i = 0; i < x.Length; i++) x[i] = (float)(0.01 * Math.Sin(2 * Math.PI * 82.4 * i / Sr));

        Fresh("bitcrusher", "bits=4;downsample=1;mix=1");
        var plain = RmsDb(Render(x), Sr / 2, Sr);
        Fresh("bitcrusher", "bits=4;downsample=1;mix=1;inputDb=24;drive=60;intensity=0;outputDb=-32");
        var futz = RmsDb(Render(x), Sr / 2, Sr);

        Assert.True(plain < -100, $"a -40 dBFS sine through a plain 4-bit crusher read {plain:F1} dBFS, not silence");
        Assert.InRange(futz, -35.0, -30.0);
    }

    /// The Lo-Fi Filter takes the staircase's images out: with a factor-15 hold,
    /// the energy above 6 kHz falls by at least 10 dB at Filter 60.
    [NativeAvailableFact]
    public void Crusher_Filter_RemovesTheHoldsImages()
    {
        var x = new float[Sr];
        for (var i = 0; i < x.Length; i++) x[i] = (float)(0.3 * Math.Sin(2 * Math.PI * 220 * i / Sr));

        Fresh("bitcrusher", "bits=16;downsample=15;mix=1");
        var raw = HighBandDb(Render(x));
        Fresh("bitcrusher", "bits=16;downsample=15;mix=1;filter=60");
        var filtered = HighBandDb(Render(x));
        Assert.True(raw - filtered > 10, $"Filter 60 took only {raw - filtered:F1} dB off the band above 6 kHz");
    }

    /// Energy above 6 kHz, as the RMS of a crude one-zero-cascade high-pass.
    private static double HighBandDb(float[] y)
    {
        var hp = new float[y.Length];
        for (var i = 3; i < y.Length; i++) hp[i] = (y[i] - 3 * y[i - 1] + 3 * y[i - 2] - y[i - 3]) / 8f;
        return RmsDb(hp, Sr / 2, y.Length);
    }

    /// Rectify scales the negative half before the shaper, so the two half-waves'
    /// peaks part (the DC blocker after the shaper re-centres the wave, which is
    /// why 90 % reads ~1.7:1 rather than 10:1); unrectified they are equal.
    [NativeAvailableFact]
    public void Crusher_Rectify_ShrinksTheNegativeHalf()
    {
        var x = new float[Sr / 4];
        for (var i = 0; i < x.Length; i++) x[i] = (float)(0.2 * Math.Sin(2 * Math.PI * 200 * i / Sr));

        Fresh("bitcrusher", "bits=16;mix=1;drive=20;rectify=0");
        var sym = Render(x);
        Fresh("bitcrusher", "bits=16;mix=1;drive=20;rectify=90");
        var rect = Render(x);
        Assert.True(Asymmetry(sym) < 1.1 && Asymmetry(rect) > 1.5,
            $"rectify 90 left the half-waves {Asymmetry(rect):F2}:1 (unrectified {Asymmetry(sym):F2}:1)");
    }

    private static double Asymmetry(float[] y)
    {
        double pos = 0, neg = 0;
        for (var i = Sr / 8; i < y.Length; i++)
        {
            pos = Math.Max(pos, y[i]);
            neg = Math.Max(neg, -y[i]);
        }

        return pos / Math.Max(1e-9, neg);
    }

    /// Two rates: the filter corner is re-derived from the device rate at prepare,
    /// so a 44.1 kHz render of the same settings is as quiet above 6 kHz.
    [NativeAvailableFact]
    public void Crusher_Filter_FollowsTheRate()
    {
        foreach (var rate in new[] { 44100, 96000 })
        {
            var x = new float[rate];
            for (var i = 0; i < x.Length; i++) x[i] = (float)(0.3 * Math.Sin(2 * Math.PI * 220 * i / rate));
            Fresh("bitcrusher", "bits=16;downsample=15;mix=1");
            var raw = Render(x, rate);
            Fresh("bitcrusher", "bits=16;downsample=15;mix=1;filter=60");
            var filtered = Render(x, rate);
            Assert.All(filtered, v => Assert.True(float.IsFinite(v)));
            Assert.True(RmsDb(filtered, rate / 2, rate) < RmsDb(raw, rate / 2, rate) + 0.5,
                $"at {rate} Hz the filtered crusher came out louder than the raw one");
        }
    }
}
