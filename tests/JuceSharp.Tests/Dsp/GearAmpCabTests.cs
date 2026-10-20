using System.Globalization;
using System.Runtime.InteropServices;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// The 2026-09 AMP / CAB drawer: <c>bassamp</c>, <c>acpre</c>,
/// <c>poweramp</c> and <c>micpos</c> (<c>src/dsp/gear/AmpCabGear.cpp</c> over
/// four new <c>cores/dcdsp_*.h</c>).
///
/// <para>Everything here renders through the SHIPPING wiring — the catalogue
/// row, the adapter's <c>ParamMailbox</c>, <c>DspChain</c>'s clamp and its
/// node crossfade — on a stopped engine via
/// <c>dca_engine_dsp_process_buffer</c>, exactly like
/// <see cref="DspGainZipperTests"/> and <see cref="GearDriveTests"/>. A test
/// that built a core by hand would pass with the row, the adapter or the clamp
/// missing entirely.</para>
///
/// <para>Three groups: the catalogue/param contracts every module owes
/// (read-back, clamping, NaN rejection, labelled discretes, rate
/// re-derivation, ramped levels and crossfaded switches), the per-module
/// VOICING claims the doc page makes (<c>docs/guides/gear-ampcab.md</c> — if a
/// number here moves, that page is wrong), and one aliasing measurement over
/// the two modules that contain a non-linearity. The allocation gate and the
/// oversized-block guard live in the RT scenario
/// <c>RtAmpCabGearChainAllocatesNothingAcrossAModelSwitch</c>.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearAmpCabTests : IDisposable
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

    /// Every module this file owns. The order is the catalogue's.
    public static readonly string[] AmpCabModules = ["bassamp", "acpre", "poweramp", "micpos"];

    public static TheoryData<string> EachModule
    {
        get
        {
            var d = new TheoryData<string>();
            foreach (var m in AmpCabModules) d.Add(m);
            return d;
        }
    }

    private readonly JuceAudioEngine? _engine;
    private readonly ITestOutputHelper _out;

    public GearAmpCabTests(ITestOutputHelper output)
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

    // ---------------------------------------------------------------- helpers

    /// A chain holding exactly one fresh node of <paramref name="module"/>,
    /// with `a=1;b=2`-style parameters applied before any audio.
    private int Fresh(string module, string setup = "")
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        Apply(n, setup);
        return n;
    }

    private void Apply(int node, string spec)
    {
        if (string.IsNullOrEmpty(spec)) return;
        foreach (var pair in spec.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = pair.Split('=');
            Assert.Equal(Ok, DspSetParam(H, node, kv[0],
                float.Parse(kv[1], CultureInfo.InvariantCulture)));
        }
    }

    /// Render <paramref name="blocks"/> consecutive blocks of a stimulus that is
    /// a function of the ABSOLUTE sample index (so the blocks are phase
    /// continuous and two runs of the same case are sample-aligned) and return
    /// the LAST one. The earlier blocks are the pre-roll that lets the chain's
    /// own 10 ms node crossfade, the mic delay line and every follower settle.
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

    /// The same, but every block CONCATENATED — for the two tests that watch a
    /// level develop over time (the optical compressor and the supply sag).
    private float[] RenderConcat(Func<int, float> stim, int frames, int sampleRate, int blocks)
    {
        var all = new float[frames * blocks];
        for (var b = 0; b < blocks; b++)
        {
            var l = new float[frames];
            var r = new float[frames];
            for (var i = 0; i < frames; i++) l[i] = r[i] = stim(b * frames + i);
            Assert.Equal(Ok, DspProcess(H, l, r, frames, sampleRate));
            Array.Copy(l, 0, all, b * frames, frames);
        }
        return all;
    }

    /// The seam stimulus: low-end content for the level knobs and a 6 kHz
    /// partial for the shelves that live above the guitar's own spectrum.
    private static Func<int, float> TwoTone(int sampleRate)
        => i => (float)(0.22 * Math.Sin(2.0 * Math.PI * 110.0 * i / sampleRate)
                      + 0.10 * Math.Sin(2.0 * Math.PI * 6000.0 * i / sampleRate));

    private static Func<int, float> Sine(double freq, float amp, int sampleRate)
        => i => (float)(amp * Math.Sin(2.0 * Math.PI * freq * i / sampleRate));

    /// Amplitude of one frequency by Goertzel. Exact (no leakage) when
    /// freq * count / sampleRate is a whole number, which every call here
    /// arranges.
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

    private static double Rms(float[] x, int start, int count)
    {
        double acc = 0;
        for (var i = start; i < start + count; i++) acc += (double)x[i] * x[i];
        return Math.Sqrt(acc / count);
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

    /// One second at 48 kHz: every whole frequency lands exactly on a bin, so
    /// <see cref="Amplitude"/> is leakage-free.
    private const int ToneFrames = 48000;

    /// Output amplitude at <paramref name="freq"/> for a single-tone render.
    private double ToneLevel(string module, string setup, double freq, float amp,
                            int sampleRate = Sr, int frames = ToneFrames)
    {
        Fresh(module, setup);
        var y = RenderBlocks(Sine(freq, amp, sampleRate), frames, sampleRate, 2);
        return Amplitude(y, freq, sampleRate, 0, frames);
    }

    // ======================================================================
    // Catalogue and parameter contracts
    // ======================================================================

    [NativeAvailableFact]
    public void EveryAmpCabModule_IsInItsOwnDrawer_WithLabelledDiscretes()
    {
        var expectedCategory = new Dictionary<string, string>
        {
            ["bassamp"] = "amp",
            ["acpre"] = "amp",
            ["poweramp"] = "amp",
            // micpos is the MIC layer that follows a cab, so it lives in the
            // cab drawer next to cab / dualcab rather than with the heads.
            ["micpos"] = "cab",
        };

        foreach (var id in AmpCabModules)
        {
            var info = JuceDspModuleCatalog.Find(id);
            Assert.NotNull(info);
            Assert.Equal(expectedCategory[id], info!.Category);
            Assert.False(string.IsNullOrWhiteSpace(info.DisplayName));
            Assert.NotEmpty(info.Params);

            foreach (var p in info.Params)
            {
                Assert.True(p.Min <= p.Default && p.Default <= p.Max,
                    $"{id}.{p.Id}: default {p.Default} is outside [{p.Min}, {p.Max}]");
            }
        }

        // Every DISCRETE param declares itself. These six are the only ones in
        // the drawer, and each is a NAMED choice, so each carries labels (the
        // two shift switches since 2026-10, DotCharter #72).
        AssertNamed("bassamp", "model", "Flip-Top", "Tube 300", "Solid 800", "Modern");
        AssertNamed("bassamp", "lowShift", "Off", "On");
        AssertNamed("bassamp", "highShift", "Off", "On");
        AssertNamed("acpre", "phase", "Normal", "Inverted");
        AssertNamed("poweramp", "tubes", "EL84", "6V6", "EL34", "6L6", "KT88");
        AssertNamed("micpos", "mic", "57", "121", "421", "Condenser", "Tube");

        void AssertNamed(string module, string param, params string[] labels)
        {
            var p = JuceDspModuleCatalog.Find(module)!.Params.Single(x => x.Id == param);
            Assert.True(p.IsNamed, $"{module}.{param} must carry value labels");
            Assert.Equal(labels, p.Labels!);
            Assert.Equal(labels.Length - 1, (int)(p.Max - p.Min));
        }
    }

    [NativeAvailableTheory]
    [MemberData(nameof(EachModule))]
    public void GetParam_ReadsBackWhatSetParamStored_AndTheChainClampsTheRange(string module)
    {
        var info = JuceDspModuleCatalog.Find(module)!;
        var n = Fresh(module);

        foreach (var p in info.Params)
        {
            // A value inside the range comes back unchanged.
            var inside = p.Min + 0.25f * (p.Max - p.Min);
            if (p.IsDiscrete) inside = MathF.Round(inside);
            Assert.Equal(Ok, DspSetParam(H, n, p.Id, inside));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var read));
            Assert.True(Math.Abs(read - inside) < 1e-5f,
                $"{module}.{p.Id}: wrote {inside}, read {read}");

            // ...and one outside is CLAMPED to the catalogue bound, not stored.
            Assert.Equal(Ok, DspSetParam(H, n, p.Id, p.Max + 1000f));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var high));
            Assert.Equal(p.Max, high, 4);
            Assert.Equal(Ok, DspSetParam(H, n, p.Id, p.Min - 1000f));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var low));
            Assert.Equal(p.Min, low, 4);
        }

        // An id nobody owns is refused rather than silently swallowed.
        Assert.NotEqual(Ok, DspSetParam(H, n, "noSuchParam", 0.5f));
        Assert.NotEqual(Ok, DspGetParam(H, n, "noSuchParam", out _));
    }

    [NativeAvailableTheory]
    [MemberData(nameof(EachModule))]
    public void NonFiniteParam_IsRejected_AndTheModuleKeepsWorking(string module)
    {
        var info = JuceDspModuleCatalog.Find(module)!;
        var n = Fresh(module);

        foreach (var p in info.Params)
        {
            foreach (var bad in new[] { float.NaN, float.PositiveInfinity, float.NegativeInfinity })
            {
                Assert.NotEqual(Ok, DspSetParam(H, n, p.Id, bad));
                Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var still));
                Assert.True(float.IsFinite(still),
                    $"{module}.{p.Id}: a rejected {bad} still reached the stored value");
            }
        }

        // Every core's clamp is NaN-transparent, so the proof that the rejection
        // matters is that the module is still AUDIBLE afterwards: a NaN that had
        // reached the coefficients would have silenced it for good.
        var y = RenderBlocks(Sine(220.0, 0.25f, Sr), 4096, Sr, 4);
        Assert.True(Rms(y, 0, y.Length) > 1e-4,
            $"{module} went silent after a rejected non-finite parameter");
        foreach (var s in y) Assert.True(float.IsFinite(s));
    }

    /// <summary>
    /// Prepare at 48 kHz, then at 192 kHz, and require the module's frequency
    /// response to be the SAME in Hz. Everything the RATE owns moves if it is
    /// not re-derived in prepare(): the oversampler and the DC blockers
    /// (bassamp, poweramp), every shelf and bell (all four), the sag follower's
    /// time constants (poweramp) and — the one only this drawer has — the mic
    /// delay line's samples-per-centimetre, the floor-bounce tap and the five
    /// room taps (micpos), which are all in SAMPLES and quadruple if they are
    /// not re-derived.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("bassamp", "model=0;gain=0.05;drive=0.02;compress=0;master=0.6")]
    [InlineData("bassamp", "model=1;gain=0.05;drive=0.02;compress=0;master=0.6;lowShift=1;highShift=1;mid=6;midHz=1600;lowMidOct=-1;lowMid=6")]
    [InlineData("acpre", "notchDepthDb=0;phase=0;presence=0.3;piezoFix=0.5;level=0.7")]
    [InlineData("poweramp", "tubes=2;drive=0.02;sag=0;presence=0.5;resonance=0.5;damping=0.5;level=0.6")]
    [InlineData("micpos", "mic=0;position=0.3;distanceCm=5;room=0;blend2=0;level=0.7")]
    public void PrepareAtAnotherRate_ReDerivesEveryRateOwnedQuantity(string module, string setup)
    {
        // 0.25 s at each rate, so 4 Hz bins and every probe lands on one.
        const int frames48 = 12000;
        const int frames192 = 48000;
        // Below fs/8 at the LOWER rate: an RBJ biquad is a BILINEAR design, so
        // its response at f/fs = 1/6 genuinely differs between the two rates by
        // a couple of dB (frequency warping) — that is the filter being
        // correct, not a missing re-derivation.
        double[] probes = [40.0, 100.0, 400.0, 1000.0, 3000.0];
        const float amp = 0.02f;   // small: keep any non-linearity out of it

        foreach (var f in probes)
        {
            Fresh(module, setup);
            var a = Amplitude(RenderBlocks(Sine(f, amp, Sr), frames48, Sr, 2), f, Sr, 0, frames48);
            Fresh(module, setup);
            var b = Amplitude(RenderBlocks(Sine(f, amp, 192000), frames192, 192000, 2),
                              f, 192000, 0, frames192);

            var delta = Db(b) - Db(a);
            _out.WriteLine($"{module} @ {f:F0} Hz: 48k {Db(a):F2} dB, 192k {Db(b):F2} dB, delta {delta:F2} dB");
            Assert.True(Math.Abs(delta) < 1.5,
                $"{module} at {f:F0} Hz moved {delta:F2} dB between 48 kHz and 192 kHz — "
                + "something the RATE owns was not re-derived in prepare()");
        }
    }

    /// <summary>
    /// micpos is the one module in this drawer that carries latency, and it is
    /// a PREPARE-time geometry (the FIRST capsule owns a 30 cm reference plane)
    /// rather than a function of the distance knob. A latency that moved with a
    /// knob would re-time the whole branch under branch PDC every time the user
    /// dragged it.
    ///
    /// <para>The number is measured off a DIRAC, never read back from the
    /// adapter. Reading <c>dca_engine_dsp_get_chain_latency</c> twice and
    /// comparing it with itself is what the first version of this test did, and
    /// it could not disagree with itself: the core reported a constant 42 while
    /// the dry tap really sat at <c>42 - distanceCm*1.3994</c> — 35 samples at
    /// the 5 cm default, ~0 at 30 cm — so branch PDC over-compensated by up to
    /// 42 samples. Here the reported number and the measured arrival have to
    /// AGREE, which is the only form of this assertion that can fail.</para>
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(0f)]
    [InlineData(5f)]
    [InlineData(30f)]
    public void MicPos_Latency_IsTheDelayTheDryPathActuallyHas(float distanceCm)
    {
        var y = RenderImpulse($"mic=0;position=0.3;room=0;blend2=0;level=0.8"
                              + $";distanceCm={distanceCm.ToString(CultureInfo.InvariantCulture)}");
        var reported = ChainLatency();
        var arrival = FirstArrival(y);

        _out.WriteLine($"micpos @ {distanceCm:F0} cm: reported {reported} samples, "
                       + $"Dirac arrived at {arrival}");
        // 30 cm of air at 48 kHz is 42 samples; a zero here would mean the
        // aligned taps were never wired up.
        Assert.InRange(reported, 40, 44);
        Assert.Equal(reported, arrival);
    }

    /// <summary>
    /// The two capsules' RELATIVE order. The second mic is fixed at 10 cm, so
    /// with the first mic closer than that the first one has to arrive FIRST,
    /// and with it further away the second one does. Referring both taps to a
    /// common 30 cm plane inverted exactly this: the dry tap moved with the
    /// knob and the second one did not, so the CLOSER capsule came out LATER
    /// (by 7 samples at the 5 cm default) — and branch PDC cannot see it,
    /// because both mics live inside one node that reports one number.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(0f, 55.99)]
    [InlineData(5f, 48.99)]
    [InlineData(30f, 14.01)]
    public void MicPos_TheCloserCapsuleIsTheEarlierOne(float distanceCm, double expectedTap)
    {
        var d = distanceCm.ToString(CultureInfo.InvariantCulture);
        var withSecond = RenderImpulse($"mic=0;position=0.3;room=0;blend2=1;level=0.8;distanceCm={d}");
        var withoutSecond = RenderImpulse($"mic=0;position=0.3;room=0;blend2=0;level=0.8;distanceCm={d}");

        // The blend2 weight is the ONLY difference, so the difference render is
        // the second capsule alone and its peak is that capsule's tap.
        var diff = new float[withSecond.Length];
        for (var i = 0; i < diff.Length; i++) diff[i] = withSecond[i] - withoutSecond[i];
        var tap = PeakIndex(diff);
        var firstTap = FirstArrival(withoutSecond);

        _out.WriteLine($"micpos @ {distanceCm:F0} cm: first capsule at {firstTap}, "
                       + $"second at {tap} (expected ~{expectedTap:F1})");
        Assert.InRange(tap, expectedTap - 1.5, expectedTap + 1.5);
        if (distanceCm < 10f)
            Assert.True(tap > firstTap,
                $"micpos at {distanceCm:F0} cm: the 10 cm capsule arrived at {tap}, "
                + $"not after the closer one at {firstTap}");
        else
            Assert.True(tap < firstTap,
                $"micpos at {distanceCm:F0} cm: the 10 cm capsule arrived at {tap}, "
                + $"not before the further one at {firstTap}");
    }

    /// One Dirac through a fresh node, after enough silence for the chain's own
    /// 10 ms node crossfade and every smoother in the core to have landed.
    private float[] RenderImpulse(string setup, int frames = 512)
    {
        Fresh("micpos", setup);
        RenderBlocks(_ => 0f, frames, Sr, 4);
        var l = new float[frames];
        var r = new float[frames];
        l[0] = r[0] = 1f;
        Assert.Equal(Ok, DspProcess(H, l, r, frames, Sr));
        return l;
    }

    /// The first sample that is not exactly the silence that precedes a causal
    /// tap. Everything before the tap is a bit-zero read out of a cleared delay
    /// line, so this is exact rather than a threshold guess.
    private static int FirstArrival(float[] y, double threshold = 1e-5)
    {
        for (var i = 0; i < y.Length; i++)
            if (Math.Abs(y[i]) > threshold) return i;
        return -1;
    }

    /// The index of the largest magnitude — used for a FRACTIONAL tap, where
    /// the delay line's 4-point Lagrange kernel spreads the Dirac over four
    /// samples and a first-crossing would report the kernel's leading tap.
    private static int PeakIndex(float[] y)
    {
        var best = 0;
        for (var i = 1; i < y.Length; i++)
            if (Math.Abs(y[i]) > Math.Abs(y[best])) best = i;
        return best;
    }

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_get_chain_latency", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspChainLatency(nint engine);

    private int ChainLatency() => DspChainLatency(H);

    // ======================================================================
    // Ramped levels and crossfaded switches - the DspGainZipperTests.MixCases
    // shape, one case per module plus every switch in the drawer.
    //
    // Three renders of the same stimulus: the parameter never moves, the
    // parameter was `after` all along, and the seam we ship. A ramped seam is
    // still close to `before` 128 samples in and has ARRIVED by sample 1,200;
    // a hard step is on `after` from sample 0.
    // ======================================================================

    private const int SeamFrames = 2048;
    private const int EarlyWindow = 128;
    private const int LateStart = 1200;
    private const int SeamPreRoll = 6;

    public static TheoryData<string, string, string, float, float> SeamCases => new()
    {
        { "bassamp",  "model=0;gain=0.5;drive=0.4;compress=0",        "master",   0.15f, 1f },
        { "acpre",    "presence=0.3;piezoFix=0",                      "level",    0.15f, 1f },
        { "poweramp", "tubes=2;drive=0.5;sag=0",                      "level",    0.15f, 1f },
        { "micpos",   "mic=0;position=0.3;distanceCm=5",              "level",    0.15f, 1f },
        // The four SWITCHES. Each moves a whole filter bank, a set of clip
        // scalars or the signal's polarity, so each is a 10 ms crossfade in the
        // core rather than a switch between two samples.
        { "bassamp",  "gain=0.5;drive=0.4;compress=0;master=0.8",     "model",    0f, 2f },
        { "acpre",    "presence=0.3;piezoFix=0;level=0.8",            "phase",    0f, 1f },
        { "poweramp", "drive=0.6;sag=0;level=0.8",                    "tubes",    0f, 4f },
        { "micpos",   "position=0.3;distanceCm=5;level=0.8",          "mic",      0f, 1f },
        { "micpos",   "position=0.3;distanceCm=5;level=0.8",          "mic",      0f, 4f },
        // ...and every remaining continuous knob whose neutral is "off": each
        // one is a blend weight over a fixed filter or tap, which is what makes
        // a filter knob ramp at all.
        { "bassamp",  "gain=0.6;drive=0.4;master=0.8",                "compress", 0f, 1f },
        { "acpre",    "presence=0;piezoFix=0;level=0.8",              "presence", 0f, 1f },
        { "acpre",    "presence=0;piezoFix=0;level=0.8",              "piezoFix", 0f, 1f },
        { "poweramp", "drive=0.4;sag=0;resonance=0;damping=0;level=0.8", "presence", 0f, 1f },
        { "poweramp", "drive=0.4;sag=0;presence=0;damping=0;level=0.8", "resonance", 0f, 1f },
        { "poweramp", "drive=0.4;sag=0;presence=0;resonance=0;level=0.8", "damping", 0f, 1f },
        { "micpos",   "mic=0;position=0;distanceCm=5;level=0.8",      "position", 0f, 1f },
        { "micpos",   "mic=0;position=0.3;distanceCm=5;level=0.8",    "blend2",   0f, 1f },
        { "micpos",   "mic=0;position=0.3;distanceCm=5;level=0.8",    "room",     0f, 1f },
        // The off-axis ANGLE (DotCharter #75): a broadband gain that ramps per
        // sample plus two shelves redesigned every 32 frames as the angle ramps.
        { "micpos",   "mic=0;position=0;distanceCm=5;level=0.8",      "angleDeg", 0f, 60f },
        { "micpos",   "mic=4;position=0;distanceCm=5;level=0.8",      "angleDeg", 0f, 60f },
        // SAG, which is why every poweramp row above pins `sag=0`. It is a
        // per-sample multiplier on the clip CEILING, so with the follower
        // charged a step in the knob is a step in the waveform. It was a bare
        // float until 2026-09-12 and the drawer's seam theory pinned it to 0
        // everywhere, so the hole could not be seen; this row is what sees it.
        { "poweramp", "tubes=2;drive=1;presence=0;resonance=0;damping=0;level=0.8", "sag", 0f, 1f },
        // distanceCm is deliberately NOT here: it moves the second capsule's
        // delay TAP, and a tap
        // sweeping 28 samples over 10 ms Doppler-shifts the 6 kHz partial, so
        // the seam decorrelates from BOTH references and the RmsDiff proxy
        // grades a perfectly smooth ramp as a step (measured: 52 %). It gets
        // its own discontinuity test below.
    };

    [NativeAvailableTheory]
    [MemberData(nameof(SeamCases))]
    public void MovingAKnobOrASwitch_WalksAcrossTheSeam_InsteadOfStepping(
        string module, string setup, string param, float before, float after)
    {
        var refBefore = RenderSeam(module, setup, param, before, before);
        var refAfter = RenderSeam(module, setup, param, after, after);
        var seam = RenderSeam(module, setup, param, before, after);

        var earlySpan = RmsDiff(refBefore, refAfter, 0, EarlyWindow);
        var lateSpan = RmsDiff(refBefore, refAfter, LateStart, SeamFrames - LateStart);
        Assert.True(earlySpan > 0.005,
            $"{module}.{param}: the two settings are indistinguishable at the seam "
            + $"(early span {earlySpan:F5}) - the case is vacuous");
        Assert.True(lateSpan > 0.005,
            $"{module}.{param}: the two settings are indistinguishable later in the block "
            + $"(late span {lateSpan:F5}) - the case is vacuous");

        var earlyRatio = RmsDiff(seam, refBefore, 0, EarlyWindow) / earlySpan;
        Assert.True(earlyRatio < 0.5,
            $"{module}.{param}: the first {EarlyWindow} samples after the change are "
            + $"{earlyRatio * 100:F0} % of the way to the new setting - a ramp is well under "
            + "half, a hard step is 100 %");

        var lateRatio = RmsDiff(seam, refAfter, LateStart, SeamFrames - LateStart) / lateSpan;
        Assert.True(lateRatio < 0.15,
            $"{module}.{param}: {SeamFrames - LateStart} samples later the seam is still "
            + $"{lateRatio * 100:F0} % away from the new setting - the ramp never landed");
    }

    private float[] RenderSeamAt(string setup, string param, float first, float second, double freq)
        => RenderSeam("micpos", setup, param, first, second, Sine(freq, 0.25f, Sr));

    private float[] RenderSeam(string module, string setup, string param, float first, float second)
        => RenderSeam(module, setup, param, first, second, TwoTone(Sr));

    private float[] RenderSeam(string module, string setup, string param, float first, float second,
                               Func<int, float> stim)
    {
        var n = Fresh(module, setup);
        Assert.Equal(Ok, DspSetParam(H, n, param, first));
        RenderBlocks(stim, SeamFrames, Sr, SeamPreRoll);

        // Continue the SAME phase: the pre-roll consumed SeamPreRoll blocks.
        Assert.Equal(Ok, DspSetParam(H, n, param, second));
        var l = new float[SeamFrames];
        var r = new float[SeamFrames];
        for (var i = 0; i < SeamFrames; i++) l[i] = r[i] = stim(SeamPreRoll * SeamFrames + i);
        Assert.Equal(Ok, DspProcess(H, l, r, SeamFrames, Sr));
        return l;
    }

    /// <summary>
    /// The distance knob moves a delay TAP — the SECOND capsule's, since the
    /// first one owns the fixed reference plane, which is why this case has to
    /// run with <c>blend2=1</c>. At <c>blend2=0</c> no tap moves at all and the
    /// case is vacuous. It cannot be graded by the seam
    /// theory (see the note in <see cref="SeamCases"/>), so it is graded the
    /// way a tap must be: the biggest sample-to-sample step across the seam
    /// block has to stay in the same league as the stimulus's own slope. A tap
    /// that jumped 25 cm — 35 samples at 48 kHz — would splice a 0.5 rad phase
    /// discontinuity into a 110 Hz sine, which is ~30x that slope and audible
    /// as a click.
    /// </summary>
    [NativeAvailableFact]
    public void MicPos_Distance_MovesItsDelayTapWithoutAClick()
    {
        const string setup = "mic=0;position=0.3;room=0;blend2=1;level=0.8";
        var steady = MaxStep(RenderSeamAt(setup, "distanceCm", 5f, 5f, 110.0));
        var swept = MaxStep(RenderSeamAt(setup, "distanceCm", 5f, 30f, 110.0));

        _out.WriteLine($"micpos distanceCm 5 -> 30 cm: max sample step {swept:E3} "
                       + $"against {steady:E3} at rest (x{swept / steady:F2})");
        Assert.True(swept < 3.0 * steady,
            $"micpos's distance tap stepped {swept:E3} against a resting {steady:E3} — "
            + "the tap jumped instead of sweeping");

        static double MaxStep(float[] y)
        {
            double m = 0;
            for (var i = 1; i < y.Length; i++) m = Math.Max(m, Math.Abs(y[i] - y[i - 1]));
            return m;
        }
    }

    // ======================================================================
    // bassamp - "Bass Amp"
    // ======================================================================

    /// <summary>
    /// The reason this module exists at all: the app charts BASS, a low B is
    /// 31 Hz and a low E 41 Hz, and one "reasonable" 80 Hz high-pass anywhere
    /// in an amp model removes the instrument's fundamentals. With the tone
    /// stack flat, 41 Hz must come out within 2 dB of 100 Hz at EVERY gain
    /// setting — including the ones where the valve stage is clipping, because
    /// a memoryless non-linearity treats the two tones identically and any
    /// difference is a filter that should not be there.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(0.0f)]
    [InlineData(0.5f)]
    [InlineData(1.0f)]
    public void BassAmp_TheLowE_SurvivesAtEveryGain(float gain)
    {
        var setup = string.Create(CultureInfo.InvariantCulture,
            $"model=0;bass=0;lowMid=0;hiMid=0;treble=0;drive=0.5;compress=0;master=0.6;gain={gain}");
        var low = Db(ToneLevel("bassamp", setup, 41.0, 0.2f));
        var mid = Db(ToneLevel("bassamp", setup, 100.0, 0.2f));

        _out.WriteLine($"bassamp gain {gain:F1}: 41 Hz {low:F2} dB, 100 Hz {mid:F2} dB, "
                       + $"delta {low - mid:F2} dB");
        Assert.True(Math.Abs(low - mid) < 2.0,
            $"bassamp at gain {gain:F1} put {low - mid:F2} dB between the low E and 100 Hz — "
            + "something in the path is high-passing the instrument away");
    }

    /// <summary>
    /// Each band boosts ITS OWN band by the decibels it is labelled with and
    /// leaves its neighbour alone. The two shelves are probed in their PLATEAU
    /// (20 Hz and 12 kHz) rather than at their corner, because an RBJ shelf is
    /// half its gain at the corner by construction; the two bells are probed at
    /// their centre, where a peaking section is exactly its gain.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("bass", 20.0, 250.0)]
    [InlineData("lowMid", 250.0, 800.0)]
    [InlineData("hiMid", 800.0, 250.0)]
    [InlineData("treble", 12000.0, 800.0)]
    public void BassAmp_EachBand_BoostsItsOwnCentre_AndLeavesItsNeighbourAlone(
        string param, double own, double neighbour)
    {
        const string flat = "model=0;gain=0.2;drive=0;compress=0;master=0.6;"
                          + "bass=0;lowMid=0;hiMid=0;treble=0";
        var boosted = flat + ";" + param + "=12";

        var ownBoost = Db(ToneLevel("bassamp", boosted, own, 0.05f))
                     - Db(ToneLevel("bassamp", flat, own, 0.05f));
        var neighbourMove = Db(ToneLevel("bassamp", boosted, neighbour, 0.05f))
                          - Db(ToneLevel("bassamp", flat, neighbour, 0.05f));

        _out.WriteLine($"bassamp {param} +12: {ownBoost:F2} dB at {own:F0} Hz, "
                       + $"{neighbourMove:F2} dB at {neighbour:F0} Hz");
        Assert.InRange(ownBoost, 10.0, 12.5);
        Assert.True(Math.Abs(neighbourMove) < 3.0,
            $"bassamp's {param} moved its neighbour at {neighbour:F0} Hz by {neighbourMove:F2} dB");
    }

    /// <summary>
    /// The optical compressor: a +12 dB input step must arrive at the output
    /// MORE than 3 dB smaller once the compressor has had 100 ms to react —
    /// and the comparison is against the same render at compress 0, so the
    /// valve stage's own compression cannot be mistaken for it.
    /// </summary>
    [NativeAvailableFact]
    public void BassAmp_Compress_SquashesAPlusTwelveDbStep()
    {
        const int block = 4800;          // 100 ms
        const int stepAt = 10 * block;   // 1 s in
        Func<int, float> step = i =>
        {
            var amp = i < stepAt ? 0.04f : 0.16f;   // +12.04 dB
            return (float)(amp * Math.Sin(2.0 * Math.PI * 220.0 * i / Sr));
        };

        var squashed = Measure(1f);
        var open = Measure(0f);
        _out.WriteLine($"bassamp step at the output: compress 0 {open:F2} dB, "
                       + $"compress 1 {squashed:F2} dB (input step +12.04 dB)");
        Assert.True(open - squashed > 3.0,
            $"bassamp's compressor only took {open - squashed:F2} dB off a +12 dB step");
        // ...and at compress 0 the step is essentially intact, which is what
        // makes the knob real rather than a second master volume.
        Assert.True(open > 9.0,
            $"bassamp at compress 0 already swallowed the step ({open:F2} dB)");

        double Measure(float compress)
        {
            var setup = string.Create(CultureInfo.InvariantCulture,
                $"model=0;gain=0.3;drive=0;master=0.6;compress={compress}");
            Fresh("bassamp", setup);
            var y = RenderConcat(step, block, Sr, 13);
            var before = Rms(y, stepAt - block, block);
            var after = Rms(y, stepAt + block, block);   // 100-200 ms after
            return Db(after) - Db(before);
        }
    }

    // ----------------------------------------------------------------------
    // bassamp, 2026-10 (DotCharter #72): the controls Rocksmith's heads carry
    // ----------------------------------------------------------------------

    private const string CleanHead = "model=1;gain=0.2;drive=0;compress=0;master=0.6;bass=0;lowMid=0;hiMid=0;treble=0";

    /// <summary>
    /// Ultra Hi and Ultra Lo at the figures Ampeg publishes for its SVT-CL ("+9 dB at
    /// 8 kHz"; "+2 dB at 40 Hz and -10 dB cut at 500 Hz"), each an RBJ bell at Q 0.7,
    /// whose sums are +9.0 dB at 8 kHz, +1.84 dB at 40 Hz and -9.97 dB at 500 Hz.
    /// Through the shipping catalogue row, adapter and chain.
    /// </summary>
    [NativeAvailableFact]
    public void BassAmp_UltraHiAndUltraLo_AreAmpegsPublishedFigures()
    {
        double Lift(string sw, double hz) => Db(ToneLevel("bassamp", CleanHead + ";" + sw + "=1", hz, 0.02f))
                                           - Db(ToneLevel("bassamp", CleanHead, hz, 0.02f));
        var hi8k = Lift("highShift", 8000.0);
        var lo40 = Lift("lowShift", 40.0);
        var lo500 = Lift("lowShift", 500.0);
        _out.WriteLine($"Ultra Hi at 8 kHz {hi8k:F2} dB; Ultra Lo at 40 Hz {lo40:F2}, at 500 Hz {lo500:F2} dB");
        Assert.InRange(hi8k, 8.8, 9.2);
        Assert.InRange(lo40, 1.64, 2.04);
        Assert.InRange(lo500, -10.17, -9.77);
    }

    /// <summary>The sweepable mid boosts its own frequency by its label, wherever
    /// it is put, and leaves a frequency two octaves away nearly alone.</summary>
    [NativeAvailableTheory]
    [InlineData(220.0, 1760.0)]
    [InlineData(800.0, 200.0)]
    [InlineData(3000.0, 750.0)]
    public void BassAmp_TheSweepableMid_BoostsWhereItIsPut(double hz, double away)
    {
        var setup = string.Create(CultureInfo.InvariantCulture, $"{CleanHead};mid=10;midHz={hz}");
        var own = Db(ToneLevel("bassamp", setup, hz, 0.02f)) - Db(ToneLevel("bassamp", CleanHead, hz, 0.02f));
        var far = Db(ToneLevel("bassamp", setup, away, 0.02f)) - Db(ToneLevel("bassamp", CleanHead, away, 0.02f));
        _out.WriteLine($"mid +10 at {hz} Hz: {own:F2} dB there, {far:F2} dB at {away} Hz");
        Assert.InRange(own, 9.5, 10.5);
        Assert.True(Math.Abs(far) < 1.5, $"the mid at {hz} Hz moved {away} Hz by {far:F2} dB");
    }

    /// <summary>An octave offset moves a fixed bell by that many octaves: the
    /// Tube 300's 250 Hz low-mid bell at -1 octave boosts 125 Hz by its label.</summary>
    [NativeAvailableFact]
    public void BassAmp_AnOctaveOffset_MovesTheBell()
    {
        var shifted = CleanHead + ";lowMid=12;lowMidOct=-1";
        var at125 = Db(ToneLevel("bassamp", shifted, 125.0, 0.02f)) - Db(ToneLevel("bassamp", CleanHead, 125.0, 0.02f));
        var at250 = Db(ToneLevel("bassamp", shifted, 250.0, 0.02f)) - Db(ToneLevel("bassamp", CleanHead, 250.0, 0.02f));
        _out.WriteLine($"lowMid +12 one octave down: {at125:F2} dB at 125 Hz, {at250:F2} dB at 250 Hz");
        Assert.InRange(at125, 11.0, 12.5);
        Assert.True(at250 < at125 - 4.0, "the bell did not move off its 250 Hz centre");
    }

    /// <summary>The new controls at their defaults are OUT of the path: a parked
    /// mid band at any frequency and two shift switches at Off render the head
    /// sample-identically to one that never heard of them.</summary>
    [NativeAvailableFact]
    public void BassAmp_TheNewControlsAtTheirDefaults_AreBitTransparent()
    {
        Fresh("bassamp", CleanHead);
        var reference = RenderBlocks(TwoTone(Sr), 4096, Sr, 3);
        Fresh("bassamp", CleanHead + ";midHz=3000;lowShift=0;highShift=0;mid=0;lowMidOct=0;hiMidOct=0");
        var parked = RenderBlocks(TwoTone(Sr), 4096, Sr, 3);
        Assert.Equal(reference, parked);
    }

    // ----------------------------------------------------------------------
    // amp, 2026-10 (DotCharter #70): circuit voicings, Bright, Resonance
    // ----------------------------------------------------------------------

    /// <summary>
    /// A circuit voicing (24 Tweed Bass 45, 25 Plexi 50 Circuit) runs the passive
    /// TMB stack: its noon setting scoops the mids against the shelf-stack row
    /// it is built on, which a shelf trio at noon does not. Through the shipping
    /// row (the native amp_circuits test holds the stack to Yeh &amp; Smith).
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(24, 8)]
    [InlineData(25, 1)]
    public void Amp_ACircuitVoicing_ScoopsTheMidsAtNoon(int circuit, int family)
    {
        double Level(int v, double hz) => Db(ToneLevel("amp",
            string.Create(CultureInfo.InvariantCulture, $"voicing={v};gain=0;bass=0.5;mid=0.5;treble=0.5;presence=0;master=0.5"), hz, 0.001f));
        double D(double hz) => Level(circuit, hz) - Level(family, hz);
        var lows = D(100.0);
        var mids = D(700.0);
        var highs = D(3000.0);
        _out.WriteLine($"voicing {circuit} minus {family}: 100 Hz {lows:F2}, 700 Hz {mids:F2}, 3 kHz {highs:F2} dB");
        Assert.True(mids < lows - 3.0 && mids < highs - 3.0,
            $"voicing {circuit} has no noon scoop against row {family}");
    }

    /// <summary>Bright (Fender AB763: 120 pF across a 1 MOhm volume pot) lifts the
    /// treble at a low gain setting and is shorted out at full gain.</summary>
    [NativeAvailableFact]
    public void Amp_Bright_LiftsTheTreble_AndIsShortedAtFullGain()
    {
        double Lift(float gain, double hz)
        {
            var off = string.Create(CultureInfo.InvariantCulture, $"voicing=24;gain={gain};presence=0;master=0.5");
            return Db(ToneLevel("amp", off + ";bright=1", hz, 0.001f)) - Db(ToneLevel("amp", off, hz, 0.001f));
        }

        var trebleLow = Lift(0.3f, 6000.0);
        var bassLow = Lift(0.3f, 200.0);
        var trebleFull = Lift(1.0f, 6000.0);
        _out.WriteLine($"bright at gain 0.3: 6 kHz {trebleLow:F2}, 200 Hz {bassLow:F2}; at gain 1: 6 kHz {trebleFull:F2} dB");
        Assert.True(trebleLow > 6.0, $"bright only lifted 6 kHz by {trebleLow:F2} dB at gain 0.3");
        Assert.True(Math.Abs(bassLow) < 1.5);
        Assert.True(Math.Abs(trebleFull) < 0.05, $"bright still moved 6 kHz by {trebleFull:F2} dB at full gain");
    }

    /// <summary>Resonance lifts the low end through the rig's shared poweramp
    /// resonance shelf, and at 0 the amp is sample-identical to one without it.</summary>
    [NativeAvailableFact]
    public void Amp_Resonance_LiftsTheLows_AndIsTransparentAtZero()
    {
        const string setup = "voicing=11;gain=0;presence=0;master=0.5";
        var lift80 = Db(ToneLevel("amp", setup + ";resonance=1", 80.0, 0.001f)) - Db(ToneLevel("amp", setup, 80.0, 0.001f));
        var lift1k = Db(ToneLevel("amp", setup + ";resonance=1", 1000.0, 0.001f)) - Db(ToneLevel("amp", setup, 1000.0, 0.001f));
        _out.WriteLine($"resonance 1: 80 Hz {lift80:F2} dB, 1 kHz {lift1k:F2} dB");
        Assert.True(lift80 - lift1k > 4.0);

        Fresh("amp", setup);
        var reference = RenderBlocks(TwoTone(Sr), 4096, Sr, 3);
        Fresh("amp", setup + ";resonance=0;bright=0");
        Assert.Equal(reference, RenderBlocks(TwoTone(Sr), 4096, Sr, 3));
    }

    // ======================================================================
    // acpre - "Acoustic Preamp"
    // ======================================================================

    [NativeAvailableFact]
    public void AcPre_TheFeedbackNotch_CutsItsOwnFrequencyAndLeavesTwoHundredAlone()
    {
        const string flat = "notchHz=120;notchDepthDb=0;notchQ=10;presence=0;piezoFix=0;level=0.7";
        const string deep = "notchHz=120;notchDepthDb=30;notchQ=10;presence=0;piezoFix=0;level=0.7";

        var at120 = Db(ToneLevel("acpre", deep, 120.0, 0.1f)) - Db(ToneLevel("acpre", flat, 120.0, 0.1f));
        var at200 = Db(ToneLevel("acpre", deep, 200.0, 0.1f)) - Db(ToneLevel("acpre", flat, 200.0, 0.1f));

        _out.WriteLine($"acpre notch 120 Hz / depth 30 / Q 10: {at120:F2} dB at 120 Hz, "
                       + $"{at200:F2} dB at 200 Hz");
        Assert.True(at120 <= -25.0, $"acpre's notch only cut 120 Hz by {-at120:F2} dB");
        Assert.True(Math.Abs(at200) < 3.0,
            $"acpre's notch took {-at200:F2} dB out of 200 Hz — it is far too wide to kill "
            + "one room mode with");
    }

    /// <summary>
    /// At flat settings the preamp is BIT transparent, so Phase = Inverted is
    /// an exact negation and a player can null the box against the dry signal.
    /// Every RBJ section at 0 dB normalises to numerator == denominator and
    /// cancels exactly; every "off" knob is a blend of weight zero. A DC
    /// blocker or a makeup gain added "for safety" would break this, and the
    /// only way to notice is a test that asserts EXACT equality.
    /// </summary>
    [NativeAvailableFact]
    public void AcPre_PhaseInverted_IsAnExactNegation()
    {
        const string flat = "notchDepthDb=0;bass=0;mid=0;treble=0;presence=0;piezoFix=0;level=0.7";
        Fresh("acpre", flat + ";phase=0");
        var normal = RenderBlocks(Sine(220.0, 0.3f, Sr), 4096, Sr, 4);
        Fresh("acpre", flat + ";phase=1");
        var inverted = RenderBlocks(Sine(220.0, 0.3f, Sr), 4096, Sr, 4);

        Assert.True(Rms(normal, 0, normal.Length) > 0.05, "acpre produced nothing to invert");
        for (var i = 0; i < normal.Length; i++)
        {
            Assert.True(normal[i] == -inverted[i],
                $"acpre at sample {i}: normal {normal[i]:G9}, inverted {inverted[i]:G9} — "
                + "the flat path is not bit transparent");
        }
    }

    [NativeAvailableFact]
    public void AcPre_PiezoFix_CutsTheQuackBand()
    {
        const string off = "notchDepthDb=0;presence=0;piezoFix=0;level=0.7";
        const string on = "notchDepthDb=0;presence=0;piezoFix=1;level=0.7";
        var cut = Db(ToneLevel("acpre", off, 1800.0, 0.1f)) - Db(ToneLevel("acpre", on, 1800.0, 0.1f));
        _out.WriteLine($"acpre piezoFix 1 at 1.8 kHz: {cut:F2} dB of cut");
        Assert.InRange(cut, 4.0, 8.0);
    }

    // ======================================================================
    // poweramp - "Power Amp & Sag"
    // ======================================================================

    /// <summary>
    /// Supply sag: under a sustained loud note the rail droops, the clip
    /// ceiling comes down with it, and the note is more than 1 dB quieter half
    /// a second in than it was in its first 20 ms. At sag 0 it is not — the
    /// stage is memoryless there, so a drop would mean something else in the
    /// path is drifting.
    /// </summary>
    [NativeAvailableFact]
    public void PowerAmp_Sag_DroopsASustainedNote_AndAtZeroDoesNot()
    {
        var withSag = Droop(1f);
        var without = Droop(0f);
        _out.WriteLine($"poweramp sustained note: sag 1 droops {withSag:F2} dB, sag 0 {without:F2} dB");
        Assert.True(withSag > 1.0,
            $"poweramp at sag 1 only drooped {withSag:F2} dB over half a second");
        Assert.True(without < 0.25,
            $"poweramp at sag 0 drooped {without:F2} dB — the stage must be memoryless there");

        /// dB by which the note's level at 500-520 ms sits BELOW its level at
        /// 100-120 ms, measured on the FUNDAMENTAL.
        double Droop(float sag)
        {
            var setup = string.Create(CultureInfo.InvariantCulture,
                $"tubes=2;drive=0.8;presence=0.5;resonance=0.5;damping=0.5;level=0.6;sag={sag}");
            Fresh("poweramp", setup);
            const int block = 960;   // 20 ms
            var y = RenderConcat(Sine(150.0, 0.7f, Sr), block, Sr, 30);
            // The reference window is 100-120 ms, not the first 20 ms: the
            // valve stage's grid bias puts a DC STEP on the opening blocks and
            // the 12 Hz DC blocker takes ~7 time constants to clear it, whose
            // leakage into the 150 Hz bin reads as 2.7 dB of sag at sag 0. By
            // 100 ms that is gone and the 200 ms sag attack has barely begun.
            // The FUNDAMENTAL, not the RMS: the valve stage's grid bias puts a
            // DC step on the first block that the 12 Hz DC blocker needs ~40 ms
            // to remove, and an RMS window would read that as 3.5 dB of "sag"
            // at sag 0. 150 Hz over 960 samples is exactly three cycles, so the
            // Goertzel is leakage free.
            var first = Amplitude(y, 150.0, Sr, 5 * block, block);   // 100-120 ms
            var later = Amplitude(y, 150.0, Sr, 25 * block, block);   // 500-520 ms
            return Db(first) - Db(later);
        }
    }

    /// <remarks>
    /// The bound is TWO-SIDED and centred on the figure
    /// <c>docs/guides/gear-ampcab.md</c> quotes. That page promises the suite
    /// measures every number on it "on every run"; a one-sided floor cannot
    /// keep that promise, and this one was 1.2 to 6.5 dB looser than the figure
    /// it was supposed to pin (audit W5-A7). +/-1.5 dB is wide enough that a
    /// compiler or a rate change does not flake it and narrow enough that a
    /// VOICING change forces a doc edit.
    /// </remarks>
    [NativeAvailableFact]
    public void PowerAmp_Presence_MovesFiveKilohertz()
    {
        const string dull = "tubes=2;drive=0.2;sag=0;resonance=0;damping=0;level=0.6;presence=0";
        const string bright = "tubes=2;drive=0.2;sag=0;resonance=0;damping=0;level=0.6;presence=1";
        var delta = Db(ToneLevel("poweramp", bright, 5000.0, 0.05f))
                  - Db(ToneLevel("poweramp", dull, 5000.0, 0.05f));
        _out.WriteLine($"poweramp presence 0 -> 1 at 5 kHz: {delta:F2} dB");
        Assert.InRange(delta, 5.7, 8.7);             // gear-ampcab.md: 7.2 dB
    }

    /// <remarks>
    /// The bound is TWO-SIDED and centred on the figure
    /// <c>docs/guides/gear-ampcab.md</c> quotes. That page promises the suite
    /// measures every number on it "on every run"; a one-sided floor cannot
    /// keep that promise, and this one was 1.2 to 6.5 dB looser than the figure
    /// it was supposed to pin (audit W5-A7). +/-1.5 dB is wide enough that a
    /// compiler or a rate change does not flake it and narrow enough that a
    /// VOICING change forces a doc edit.
    /// </remarks>
    [NativeAvailableFact]
    public void PowerAmp_Resonance_MovesTheLowEnd_WithoutTouchingTheTop()
    {
        const string flat = "tubes=2;drive=0.2;sag=0;presence=0;damping=0;level=0.6;resonance=0";
        const string full = "tubes=2;drive=0.2;sag=0;presence=0;damping=0;level=0.6;resonance=1";
        var low = Db(ToneLevel("poweramp", full, 80.0, 0.05f))
                - Db(ToneLevel("poweramp", flat, 80.0, 0.05f));
        var top = Db(ToneLevel("poweramp", full, 4000.0, 0.05f))
                - Db(ToneLevel("poweramp", flat, 4000.0, 0.05f));
        _out.WriteLine($"poweramp resonance 0 -> 1: {low:F2} dB at 80 Hz, {top:F2} dB at 4 kHz");
        Assert.InRange(low, 5.6, 8.6);               // gear-ampcab.md: 7.1 dB
        Assert.True(Math.Abs(top) < 1.0,
            $"poweramp's resonance moved 4 kHz by {top:F2} dB — it is a low shelf, not a tilt");
    }

    // ======================================================================
    // micpos - "Cab Mic Position"
    // ======================================================================

    /// <remarks>
    /// The bound is TWO-SIDED and centred on the figure
    /// <c>docs/guides/gear-ampcab.md</c> quotes. That page promises the suite
    /// measures every number on it "on every run"; a one-sided floor cannot
    /// keep that promise, and this one was 1.2 to 6.5 dB looser than the figure
    /// it was supposed to pin (audit W5-A7). +/-1.5 dB is wide enough that a
    /// compiler or a rate change does not flake it and narrow enough that a
    /// VOICING change forces a doc edit.
    /// </remarks>
    [NativeAvailableFact]
    public void MicPos_TheDynamicAndTheRibbon_DifferWhereTheyActuallyDiffer()
    {
        const string baseline = "position=0;distanceCm=5;room=0;blend2=0;level=0.7";
        var dyn = Db(ToneLevel("micpos", baseline + ";mic=0", 5000.0, 0.1f));
        var ribbon = Db(ToneLevel("micpos", baseline + ";mic=1", 5000.0, 0.1f));
        _out.WriteLine($"micpos at 5 kHz: 57 {dyn:F2} dB, 121 {ribbon:F2} dB, delta {dyn - ribbon:F2} dB");
        Assert.InRange(dyn - ribbon, 9.0, 12.0);     // gear-ampcab.md: 10.5 dB
    }

    [NativeAvailableFact]
    public void MicPos_Distance_MovesTheProximityEffect()
    {
        const string baseline = "mic=0;position=0;room=0;blend2=0;level=0.7";
        var close = Db(ToneLevel("micpos", baseline + ";distanceCm=0", 100.0, 0.1f));
        var far = Db(ToneLevel("micpos", baseline + ";distanceCm=30", 100.0, 0.1f));
        _out.WriteLine($"micpos at 100 Hz: 0 cm {close:F2} dB, 30 cm {far:F2} dB, "
                       + $"delta {close - far:F2} dB");
        Assert.True(close - far > 4.0,
            $"micpos: the proximity effect is only {close - far:F2} dB across the whole "
            + "distance range");
    }

    [NativeAvailableFact]
    public void MicPos_Position_TakesTheTopOffAsItMovesOffAxis()
    {
        const string baseline = "mic=0;distanceCm=5;room=0;blend2=0;level=0.7";
        var cap = Db(ToneLevel("micpos", baseline + ";position=0", 6000.0, 0.1f));
        var edge = Db(ToneLevel("micpos", baseline + ";position=1", 6000.0, 0.1f));
        _out.WriteLine($"micpos at 6 kHz: cap {cap:F2} dB, edge {edge:F2} dB, delta {cap - edge:F2} dB");
        Assert.True(cap - edge > 4.0,
            $"micpos: moving from the dust cap to the cone edge only lost {cap - edge:F2} dB "
            + "at 6 kHz");
    }

    /// <summary>
    /// The fifth mic, the TUBE condenser (DotCharter #75), through the real
    /// module: its response re 1 kHz holds the Neumann U 67's published
    /// cardioid chart (U 67 operating manual, "Specifications U 67") from 2 to
    /// 16 kHz. At 15 cm the proximity shelf (150 Hz) is half in, so only the
    /// band above 1 kHz is the capsule's own. The 16 kHz point needs the near-
    /// Nyquist shelf (#87): at the quarter-rate ceiling it reads ~5 dB high.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(4000.0, 0.07)]
    [InlineData(8000.0, 1.03)]
    [InlineData(10000.0, 1.41)]
    [InlineData(14000.0, -1.65)]
    [InlineData(16000.0, -4.70)]
    public void MicPos_TheTubeMic_FollowsTheU67Chart(double hz, double chartDb)
    {
        const string setup = "mic=4;position=0;distanceCm=15;room=0;blend2=0;level=0.7";
        var at = Db(ToneLevel("micpos", setup, hz, 0.05f)) - Db(ToneLevel("micpos", setup, 1000.0, 0.05f));
        _out.WriteLine($"micpos tube at {hz:F0} Hz: {at:F2} dB re 1 kHz (U 67 chart {chartDb:F2})");
        Assert.InRange(at - chartDb, -0.6, 0.6);
    }

    /// <summary>
    /// The off-axis ANGLE (DotCharter #75), through the real module: a 57
    /// turned 45 degrees loses what the Shure SM57 user guide's polar chart
    /// says it loses at 45 degrees (-1.2 / -1.8 / -0.6 / -2.4 dB at 1 / 2 / 4 /
    /// 8 kHz), and the figure-8 ribbon loses cos 45 = -3.01 dB at any frequency.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(0, 1000.0, -1.2)]
    [InlineData(0, 2000.0, -1.8)]
    [InlineData(0, 4000.0, -0.6)]
    [InlineData(0, 8000.0, -2.4)]
    [InlineData(1, 1000.0, -3.01)]
    [InlineData(1, 6000.0, -3.01)]
    public void MicPos_Angle_FollowsThePublishedPolarChart(int mic, double hz, double chartDb)
    {
        var baseline = $"mic={mic};position=0;distanceCm=15;room=0;blend2=0;level=0.7";
        var loss = Db(ToneLevel("micpos", baseline + ";angleDeg=45", hz, 0.05f))
                   - Db(ToneLevel("micpos", baseline + ";angleDeg=0", hz, 0.05f));
        _out.WriteLine($"micpos mic {mic} at 45 deg, {hz:F0} Hz: {loss:F2} dB (chart {chartDb:F2})");
        Assert.InRange(loss - chartDb, -0.8, 0.8);
    }

    /// <summary>
    /// At <c>room = 0</c> the five early-reflection taps contribute exactly
    /// nothing: the weight is zero and `s += 0 * er` is the identity, so no
    /// reflection can leak into a dry mic.
    ///
    /// <para>What is left at room 0 is NOT a reflection — it is the mic
    /// curve's own IIR ring from the direct impulse, which at the 7 ms tap is
    /// still ~3e-4 and is why this asserts a RATIO and a SHAPE rather than an
    /// absolute floor. A reflection is a local SPIKE: at room 1 the 7 ms tap
    /// stands 58 dB above the ring 3 ms ahead of it. At room 0 the window is
    /// allowed to be up to 2x that earlier one — three cascaded shelves ringing
    /// out are not monotone sample to sample, and the measured worst case is
    /// 1.04x — but nothing like a tap, which is what stops this from passing on
    /// a module whose taps were never wired up.
    /// </para>
    /// </summary>
    [NativeAvailableFact]
    public void MicPos_Room_AtZero_AddsNoEarlyReflectionsAtAll()
    {
        double[] tapMs = [7.0, 13.0, 19.0, 29.0, 41.0];
        const string baseline = "mic=0;position=0.3;distanceCm=5;blend2=0;level=0.7";

        var dry = Impulse(baseline + ";room=0");
        var wet = Impulse(baseline + ";room=1");

        foreach (var ms in tapMs)
        {
            // The direct tap sits ON the reference plane (42 samples at
            // 48 kHz, whatever the distance knob says — see
            // MicPos_Latency_IsTheDelayTheDryPathActuallyHas); a reflection
            // lands that many samples after it. A generous window either side
            // covers the interpolator's four-point kernel. The "before" window
            // sits 3 ms earlier, where no tap lands for any of the five.
            var centre = (int)Math.Round(ms * 0.001 * Sr) + 42;
            var before = centre - (int)Math.Round(0.003 * Sr);
            var dryPeak = Peak(dry, centre - 8, 17);
            var wetPeak = Peak(wet, centre - 8, 17);
            var dryBefore = Peak(dry, before - 8, 17);
            var wetBefore = Peak(wet, before - 8, 17);
            _out.WriteLine($"micpos tap {ms:F0} ms: room 0 {dryPeak:E2} (ring 3 ms earlier "
                           + $"{dryBefore:E2}), room 1 {wetPeak:E2} (earlier {wetBefore:E2}), "
                           + $"ratio {Db(wetPeak) - Db(dryPeak):F1} dB");
            Assert.True(dryPeak <= 2.0 * dryBefore,
                $"micpos at room 0 carried {dryPeak:E3} at the {ms:F0} ms tap against "
                + $"{dryBefore:E3} 3 ms earlier — that is a reflection leaking into a dry mic, "
                + "not a decaying ring");
            Assert.True(Db(wetPeak) - Db(dryPeak) > 40.0,
                $"micpos at room 0 carried {dryPeak:E3} at the {ms:F0} ms tap, only "
                + $"{Db(wetPeak) - Db(dryPeak):F1} dB below room 1");
            Assert.True(wetPeak > 5.0 * wetBefore && wetPeak > 0.01,
                $"micpos at room 1 carried only {wetPeak:E3} at the {ms:F0} ms tap — the "
                + "reflections are not there at all, so the room 0 half is vacuous");
        }

        float[] Impulse(string setup) => RenderImpulse(setup, 4096);

        static double Peak(float[] x, int start, int count)
        {
            double m = 0;
            for (var i = Math.Max(0, start); i < Math.Min(x.Length, start + count); i++)
                m = Math.Max(m, Math.Abs(x[i]));
            return m;
        }
    }


    // ======================================================================
    // Silence in, silence out
    // ======================================================================

    /// <summary>
    /// Every module in this drawer must be QUIET from digital silence. Five
    /// cores in the DRIVE drawer were not — a gain stage with a fixed
    /// operating-point bias emits DC into a DC blocker that starts from zero
    /// state, and a blocker answers a DC step with a full-amplitude decaying
    /// pulse, i.e. a THUMP on every prepare, device change and un-bypass. This
    /// drawer contains the same shape (<c>poweramp</c>'s and <c>bassamp</c>'s
    /// bias into their own blockers), so it carries the same guarantee.
    /// </summary>
    [NativeAvailableTheory]
    [MemberData(nameof(EachModule))]
    public void SilenceIn_StaysBelowTheFloor_FromTheVeryFirstBlock(string module)
    {
        Fresh(module);                      // the catalogue defaults
        const int half = Sr / 2;
        var l = new float[half];
        var r = new float[half];
        Assert.Equal(Ok, DspProcess(H, l, r, half, Sr));

        var rms = Rms(l, 0, half);
        var peak = 0.0;
        foreach (var v in l) peak = Math.Max(peak, Math.Abs(v));
        _out.WriteLine($"{module} on silence: {Db(rms):F1} dBFS RMS, {Db(peak):F1} dBFS peak "
                       + "over the first 500 ms");
        Assert.True(Db(rms) < -60.0,
            $"{module} emitted {Db(rms):F1} dBFS RMS out of digital silence");
    }

    // ======================================================================
    // Aliasing - the two modules in this drawer that contain a non-linearity.
    //
    // A two-tone (1 kHz + 1.1 kHz) through a non-linearity produces IMD
    // products on a 100 Hz grid - and so do its ALIASES, because 48 kHz is a
    // whole multiple of 100 Hz. Frequency alone therefore cannot tell the two
    // apart, so this renders the SAME stimulus at 48 kHz and at 192 kHz (where
    // the same oversampling factor puts the fold 4x further out) and compares
    // the two spectra bin for bin: a legitimate IMD product appears in both and
    // cancels, an alias appears only in the 48 kHz one. The method is
    // GearDriveTests'.
    // ======================================================================

    private const int AliasN48 = 32768;
    private const int AliasN192 = 131072;
    private const double AliasBinHz = 48000.0 / AliasN48;
    private const int AliasBinF1 = 682;   // 999.02 Hz
    private const int AliasBinF2 = 751;   // 1100.10 Hz

    /// <remarks>
    /// The two DRIVEN cases are the ones that prove the oversampler: measured
    /// with <c>kOs</c> forced to 1, the catalogue defaults still came out at
    /// -126 dB, because at those settings the ADAA tanh is barely bending the
    /// signal and ADAA alone suppresses what little it makes. A defaults-only
    /// theory would therefore have passed on a core with no oversampling in it
    /// at all.
    /// </remarks>
    [NativeAvailableTheory]
    [InlineData("bassamp", "model=0;gain=0.4;drive=0.2;compress=0.3;master=0.6")]
    [InlineData("poweramp", "tubes=2;drive=0.3;sag=0.3;presence=0.5;resonance=0.5;damping=0.5;level=0.6")]
    [InlineData("bassamp", "model=3;gain=0.8;drive=1;compress=0;master=0.6")]
    [InlineData("poweramp", "tubes=0;drive=1;sag=0;presence=0.5;resonance=0.5;damping=0.5;level=0.6")]
    public void AliasProducts_StayBelowMinusSixtyDb_AtDefaultsAndDriven(string module, string setup)
    {
        var m48 = Spectrum(module, setup, Sr, AliasN48);
        var m192 = Spectrum(module, setup, 192000, AliasN192);

        var ref48 = m48[AliasBinF1];
        var ref192 = m192[AliasBinF1];
        Assert.True(ref48 > 0 && ref192 > 0, $"{module}: no output at the probe tone");

        var worst = 0.0;
        var worstHz = 0.0;
        var lo = (int)(150.0 / AliasBinHz);
        var hi = (int)(18000.0 / AliasBinHz);
        for (var k = lo; k <= hi; k++)
        {
            if (Math.Abs(k - AliasBinF1) <= 2 || Math.Abs(k - AliasBinF2) <= 2) continue;
            var got = m48[k] / ref48;
            var legit = m192[k] / ref192;
            if (got < 4.0 * legit) continue;      // a legitimate product, not an alias
            var excess = got - legit;
            if (excess > worst) { worst = excess; worstHz = k * AliasBinHz; }
        }

        var worstDb = Db(worst);
        _out.WriteLine($"{module}: worst alias product {worstDb:F1} dB (relative to the "
                       + $"1 kHz fundamental) at {worstHz:F0} Hz");
        Assert.True(worstDb < -60.0,
            $"{module} aliased to {worstDb:F1} dB at {worstHz:F0} Hz - oversample more");
    }

    private float[] Spectrum(string module, string setup, int sampleRate, int frames)
    {
        var f1 = AliasBinF1 * AliasBinHz;
        var f2 = AliasBinF2 * AliasBinHz;
        Fresh(module, setup);
        var y = RenderBlocks(
            i => (float)(0.3 * (Math.Sin(2.0 * Math.PI * f1 * i / sampleRate)
                              + Math.Sin(2.0 * Math.PI * f2 * i / sampleRate))),
            frames, sampleRate, 2);

        var win = new float[frames];
        for (var i = 0; i < frames; i++)
            win[i] = y[i] * (float)(0.5 - 0.5 * Math.Cos(2.0 * Math.PI * i / frames));

        var fft = new JuceFft(frames);
        var re = new float[frames];
        var im = new float[frames];
        fft.ForwardReal(win, re, im);

        var half = frames / 2 + 1;
        var mag = new float[half];
        for (var k = 0; k < half; k++)
            mag[k] = MathF.Sqrt(re[k] * re[k] + im[k] * im[k]) / frames;
        return mag;
    }
}
