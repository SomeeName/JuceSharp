using System.Globalization;
using System.Runtime.InteropServices;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// The 2026-09 DRIVE drawer: <c>rat</c>, <c>ds1</c>, <c>hm2</c>,
/// <c>tapesat</c>, <c>bassod</c>, <c>tubepre</c> and <c>octfuzz</c>
/// (<c>src/dsp/gear/DriveGear.cpp</c> over seven new <c>cores/dcdsp_*.h</c>).
///
/// <para>Everything here renders through the SHIPPING wiring — the catalogue
/// row, the adapter's <c>ParamMailbox</c>, <c>DspChain</c>'s clamp and its
/// node crossfade — on a stopped engine via
/// <c>dca_engine_dsp_process_buffer</c>, exactly like
/// <see cref="DspGainZipperTests"/>. A test that built a core by hand would
/// pass with the row, the adapter or the clamp missing entirely.</para>
///
/// <para>Three groups: the catalogue/param contracts every module owes
/// (read-back, clamping, NaN rejection, labelled discretes, rate
/// re-derivation, ramped levels), the per-module VOICING claims the doc page
/// makes (<c>docs/guides/gear-drive.md</c> — if a number here moves, that page
/// is wrong), and one aliasing measurement. The allocation gate and the
/// oversized-block guard live in the RT scenario
/// <c>RtDriveGearChainAllocatesNothingAndSkipsOversizedBlocks</c>.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearDriveTests : IDisposable
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
    public static readonly string[] DriveModules =
        ["rat", "ds1", "hm2", "tapesat", "bassod", "tubepre", "octfuzz"];

    public static TheoryData<string> EachModule
    {
        get
        {
            var d = new TheoryData<string>();
            foreach (var m in DriveModules) d.Add(m);
            return d;
        }
    }

    private readonly JuceAudioEngine? _engine;
    private readonly ITestOutputHelper _out;

    public GearDriveTests(ITestOutputHelper output)
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
    /// own 10 ms node crossfade and every follower settle.
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
    /// <see cref="Amplitude"/> is leakage-free for the fundamental AND for every
    /// harmonic of it.
    private const int ToneFrames = 48000;

    /// Total harmonic distortion of a single tone: the root-sum-square of
    /// harmonics 2..<paramref name="maxHarmonic"/> over the fundamental.
    private double Thd(string module, string setup, double f0, float amp, int maxHarmonic = 40)
    {
        Fresh(module, setup);
        var y = RenderBlocks(Sine(f0, amp, Sr), ToneFrames, Sr, 2);
        var fund = Amplitude(y, f0, Sr, 0, ToneFrames);
        double acc = 0;
        for (var h = 2; h <= maxHarmonic; h++)
        {
            var f = f0 * h;
            if (f >= Sr * 0.45) break;
            var a = Amplitude(y, f, Sr, 0, ToneFrames);
            acc += a * a;
        }
        return Math.Sqrt(acc) / Math.Max(fund, 1e-9);
    }

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
    public void EverySevenDriveModules_AreInTheDriveDrawer_WithLabelledDiscretes()
    {
        foreach (var id in DriveModules)
        {
            var info = JuceDspModuleCatalog.Find(id);
            Assert.NotNull(info);
            Assert.Equal("drive", info!.Category);
            Assert.False(string.IsNullOrWhiteSpace(info.DisplayName));
            Assert.NotEmpty(info.Params);

            foreach (var p in info.Params)
            {
                Assert.True(p.Min <= p.Default && p.Default <= p.Max,
                    $"{id}.{p.Id}: default {p.Default} is outside [{p.Min}, {p.Max}]");
            }
        }

        // Every DISCRETE param declares itself. These three are the only ones
        // in the drawer, and each is a NAMED choice, so each carries labels.
        AssertNamed("rat", "clip", "Silicon", "LED", "Turbo");
        AssertNamed("tubepre", "bright", "Off", "On");
        AssertNamed("tubepre", "cabSim", "Off", "On");

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
        var y = RenderBlocks(Sine(220.0, 0.25f, Sr), 4096, Sr, 3);
        Assert.True(Rms(y, 0, y.Length) > 1e-4,
            $"{module} went silent after a rejected non-finite parameter");
        foreach (var s in y) Assert.True(float.IsFinite(s));
    }

    /// <summary>
    /// Prepare at 48 kHz, then at 192 kHz, and require the module's frequency
    /// response to be the SAME in Hz. Everything the RATE owns — the
    /// oversampler, every shelf and low-pass, and above all the DC blocker,
    /// whose corner rose from 38 Hz to 153 Hz in three shipped cores because
    /// they never re-derived it — moves if it is not re-derived in prepare().
    /// The drive settings are deliberately LOW so this measures filters and not
    /// the clipper's own amplitude law.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("rat", "distortion=0.05;filter=0.4;volume=0.6")]
    [InlineData("ds1", "dist=0.05;tone=0.5;level=0.6")]
    [InlineData("hm2", "dist=0.05;lowDb=10;highDb=10;level=0.5")]
    [InlineData("tapesat", "drive=0.05;mix=1;hiss=0")]
    [InlineData("bassod", "drive=0.05;blend=1;level=0.6")]
    [InlineData("tubepre", "gain=0.05;sag=0;tone=0.5;level=0.6;cabSim=0")]
    [InlineData("octfuzz", "fuzz=0.05;octave=0;tone=0.5;volume=0.5")]
    public void PrepareAtAnotherRate_ReDerivesEveryRateOwnedQuantity(string module, string setup)
    {
        // 0.25 s at each rate, so 4 Hz bins and every probe lands on one.
        const int frames48 = 12000;
        const int frames192 = 48000;
        // Below fs/8 at the LOWER rate. An RBJ biquad is a BILINEAR design, so
        // its response at f/fs = 1/6 genuinely differs between 48 kHz and
        // 192 kHz by a couple of dB (frequency warping) — that is the filter
        // being correct, not a missing re-derivation, and probing up there
        // would make this test fail for a reason it is not about. The
        // quantities it exists to catch (the DC blocker's corner, the shelves,
        // the oversampler's geometry) all show at the bottom.
        double[] probes = [40.0, 100.0, 400.0, 1000.0, 3000.0];
        const float amp = 0.02f;   // small: keep the non-linearity out of it

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

    // ======================================================================
    // Ramped levels - the DspGainZipperTests.MixCases shape, one case per
    // module plus the three discrete footswitches.
    //
    // Three renders of the same stimulus: the parameter never moves, the
    // parameter was `after` all along, and the seam we ship. A ramped seam is
    // still close to `before` 128 samples in and has ARRIVED by sample 1,200;
    // a hard step is on `after` from sample 0.
    // ======================================================================

    private const int SeamFrames = 2048;
    private const int EarlyWindow = 128;
    private const int LateStart = 1200;
    private const int SeamPreRoll = 4;

    public static TheoryData<string, string, string, float, float> SeamCases => new()
    {
        { "rat",     "distortion=0.5;filter=0.4;clip=0", "volume", 0.15f, 1f },
        { "ds1",     "dist=0.5;tone=0.5",                "level",  0.15f, 1f },
        { "hm2",     "dist=0.6;lowDb=6;highDb=6",        "level",  0.15f, 1f },
        { "tapesat", "drive=1;bias=0.5;hiss=0",          "mix",    0f,    1f },
        { "bassod",  "drive=1;level=0.6",                "blend",  0f,    1f },
        { "tubepre", "gain=0.5;sag=0;tone=0.5",          "level",  0.15f, 1f },
        { "octfuzz", "fuzz=0.7;octave=0.6;tone=0.5",     "volume", 0.15f, 1f },
        // The three FOOTSWITCHES. Each moves per-sample multipliers - two clip
        // thresholds and a makeup gain, or a whole filter branch - so each is a
        // 10 ms crossfade in the core rather than a switch.
        { "rat",     "distortion=0.6;filter=0.3;volume=0.8", "clip",   0f, 1f },
        { "tubepre", "gain=0.5;sag=0;tone=0.5;level=0.8",    "cabSim", 0f, 1f },
        { "tubepre", "gain=0.5;sag=0;tone=0.5;level=0.8",    "bright", 0f, 1f },
        // ...and the octave blend, which is a continuous crossfade between two
        // completely different waveforms.
        { "octfuzz", "fuzz=0.7;tone=0.5;volume=0.8",         "octave", 0f, 1f },
        // SAG, which is why every tubepre row above pins `sag=0`. It is a
        // per-sample multiplier on BOTH of the second stage's clip thresholds,
        // so with the follower charged - which is any time the player is
        // playing - a step in the knob is a step in the waveform. It was a bare
        // float until 2026-09-12 and the whole drawer's seam theory pinned it
        // to 0, so the hole was invisible: this row is the one that would have
        // seen it (measured 96 % of the way at sample 0 before the fix).
        { "tubepre", "gain=0.8;tone=0.5;level=0.8",           "sag",    0f, 1f },
    };

    [NativeAvailableTheory]
    [MemberData(nameof(SeamCases))]
    public void MovingALevelOrASwitch_WalksAcrossTheSeam_InsteadOfStepping(
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

    private float[] RenderSeam(string module, string setup, string param, float first, float second)
    {
        var n = Fresh(module, setup);
        Assert.Equal(Ok, DspSetParam(H, n, param, first));
        var stim = Sine(110.0, 0.25f, Sr);
        RenderBlocks(stim, SeamFrames, Sr, SeamPreRoll);

        // Continue the SAME phase: the pre-roll consumed SeamPreRoll blocks.
        Assert.Equal(Ok, DspSetParam(H, n, param, second));
        var l = new float[SeamFrames];
        var r = new float[SeamFrames];
        for (var i = 0; i < SeamFrames; i++) l[i] = r[i] = stim(SeamPreRoll * SeamFrames + i);
        Assert.Equal(Ok, DspProcess(H, l, r, SeamFrames, Sr));
        return l;
    }

    // ======================================================================
    // rat - "Rodent Distortion"
    // ======================================================================

    /// <remarks>
    /// The bound is TWO-SIDED and centred on the figure the page quotes. The
    /// page promises "if one of these numbers moves, that test fails and this
    /// page is wrong"; a one-sided `> floor` cannot keep that promise, and
    /// every one of these was between 1.2 and 16 dB looser than the number it
    /// was supposed to pin (audit W5-A7). The tolerance is +/-1.5 dB for a
    /// level and +/-3 points for a THD percentage - wide enough that a
    /// compiler or a rate change does not flake it, narrow enough that a
    /// VOICING change forces a doc edit, which is the point.
    /// </remarks>
    [NativeAvailableFact]
    public void Rat_AtFullDistortion_DistortsATwoHundredHertzSineByTheAmountThePageQuotes()
    {
        var thd = Thd("rat", "distortion=1;filter=0;volume=0.6;clip=0", 200.0, 0.2f);
        _out.WriteLine($"rat THD @ distortion 1: {thd * 100:F1} %");
        Assert.InRange(thd * 100.0, 43.5, 49.5);     // gear-drive.md: 46.5 %

        var clean = Thd("rat", "distortion=0;filter=0;volume=0.6;clip=0", 200.0, 0.02f);
        Assert.True(clean < thd / 4.0,
            $"rat at distortion 0 already measured {clean * 100:F1} % THD - the knob does nothing");
    }

    [NativeAvailableFact]
    public void Rat_FilterKnob_IsALowPass_SoClockwiseIsDarker()
    {
        const string open = "distortion=0.3;volume=0.6;clip=0;filter=0";
        const string dark = "distortion=0.3;volume=0.6;clip=0;filter=1";

        var openRatio = Db(ToneLevel("rat", open, 5000.0, 0.1f))
                      - Db(ToneLevel("rat", open, 500.0, 0.1f));
        var darkRatio = Db(ToneLevel("rat", dark, 5000.0, 0.1f))
                      - Db(ToneLevel("rat", dark, 500.0, 0.1f));

        var drop = openRatio - darkRatio;
        _out.WriteLine($"rat 5k/500 balance: filter 0 {openRatio:F2} dB, filter 1 {darkRatio:F2} dB, drop {drop:F2} dB");
        Assert.True(drop > 12.0,
            $"rat's Filter knob only darkened the 5 kHz/500 Hz balance by {drop:F2} dB");
    }

    // ======================================================================
    // ds1 - "Orange Distortion"
    // ======================================================================

    [NativeAvailableFact]
    public void Ds1_ToneStack_IsATiltAroundSevenHundredHertz()
    {
        const string darkS = "dist=0.4;level=0.6;tone=0";
        const string brightS = "dist=0.4;level=0.6;tone=1";

        var dark = Db(ToneLevel("ds1", darkS, 2000.0, 0.1f)) - Db(ToneLevel("ds1", darkS, 200.0, 0.1f));
        var bright = Db(ToneLevel("ds1", brightS, 2000.0, 0.1f)) - Db(ToneLevel("ds1", brightS, 200.0, 0.1f));

        var tilt = bright - dark;
        _out.WriteLine($"ds1 2k/200 balance: tone 0 {dark:F2} dB, tone 1 {bright:F2} dB, tilt {tilt:F2} dB");
        Assert.True(tilt > 15.0, $"ds1's tone control only tilted the balance by {tilt:F2} dB");
    }

    [NativeAvailableFact]
    public void Ds1_Thd_RisesMonotonicallyWithDist()
    {
        double previous = -1;
        foreach (var dist in new[] { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f })
        {
            var setup = string.Create(CultureInfo.InvariantCulture, $"tone=0.5;level=0.6;dist={dist}");
            // 0.35, not a whisper: below ~0.25 the fixed input booster is the only
            // non-linearity in the path at low `dist` settings, so the first two
            // steps of the sweep measure the SAME 0.8 % and the theory reads as a
            // regression when nothing is wrong.
            var thd = Thd("ds1", setup, 200.0, 0.35f);
            _out.WriteLine($"ds1 THD @ dist {dist:F1}: {thd * 100:F2} %");
            Assert.True(thd > previous,
                $"ds1 THD did not rise at dist {dist:F1} ({thd * 100:F2} % vs {previous * 100:F2} %)");
            previous = thd;
        }
        Assert.True(previous > 0.20, $"ds1 at full dist measured only {previous * 100:F1} % THD");
    }

    // ======================================================================
    // hm2 - "Chainsaw Distortion"
    // ======================================================================

    [NativeAvailableTheory]
    [InlineData("lowDb", 80.0)]
    [InlineData("highDb", 1000.0)]
    public void Hm2_EachColourKnob_BoostsItsOwnBandByTheDecibelsItIsLabelledWith(
        string param, double centreHz)
    {
        var flat = string.Create(CultureInfo.InvariantCulture,
            $"dist=0.5;level=0.5;lowDb=0;highDb=0");
        var boosted = string.Create(CultureInfo.InvariantCulture,
            $"dist=0.5;level=0.5;lowDb=0;highDb=0;{param}=10");

        var before = Db(ToneLevel("hm2", flat, centreHz, 0.1f));
        var after = Db(ToneLevel("hm2", boosted, centreHz, 0.1f));
        var boost = after - before;
        _out.WriteLine($"hm2 {param} 0 -> 10 at {centreHz:F0} Hz: {boost:F2} dB");
        Assert.InRange(boost, 8.0, 12.0);
    }

    /// <remarks>
    /// The bound is TWO-SIDED and centred on the figure the page quotes. The
    /// page promises "if one of these numbers moves, that test fails and this
    /// page is wrong"; a one-sided `> floor` cannot keep that promise, and
    /// every one of these was between 1.2 and 16 dB looser than the number it
    /// was supposed to pin (audit W5-A7). The tolerance is +/-1.5 dB for a
    /// level and +/-3 points for a THD percentage - wide enough that a
    /// compiler or a rate change does not flake it, narrow enough that a
    /// VOICING change forces a doc edit, which is the point.
    /// </remarks>
    [NativeAvailableFact]
    public void Hm2_AtFullDist_DistortsByTheAmountThePageQuotes()
    {
        var thd = Thd("hm2", "dist=1;lowDb=0;highDb=0;level=0.5", 200.0, 0.2f);
        _out.WriteLine($"hm2 THD @ dist 1: {thd * 100:F1} %");
        Assert.InRange(thd * 100.0, 42.1, 48.1);     // gear-drive.md: 45.1 %
    }

    // ======================================================================
    // tapesat - "Tape Saturation"
    // ======================================================================

    /// <remarks>
    /// The bound is TWO-SIDED and centred on the figure the page quotes. The
    /// page promises "if one of these numbers moves, that test fails and this
    /// page is wrong"; a one-sided `> floor` cannot keep that promise, and
    /// every one of these was between 1.2 and 16 dB looser than the number it
    /// was supposed to pin (audit W5-A7). The tolerance is +/-1.5 dB for a
    /// level and +/-3 points for a THD percentage - wide enough that a
    /// compiler or a rate change does not flake it, narrow enough that a
    /// VOICING change forces a doc edit, which is the point.
    /// </remarks>
    [NativeAvailableFact]
    public void TapeSat_AtFullDrive_CompressesALoudInputByTheAmountThePageQuotes()
    {
        const string setup = "drive=1;bias=0.5;hfRolloffKHz=20;hiss=0;mix=1";
        var loud = Db(ToneLevel("tapesat", setup, 200.0, 0.708f));   // -3 dBFS
        var quiet = Db(ToneLevel("tapesat", setup, 200.0, 0.0708f)); // 20 dB below it

        var compression = 20.0 - (loud - quiet);
        _out.WriteLine($"tapesat: -3 dBFS in {loud:F2} dB out, -23 dBFS in {quiet:F2} dB out, "
                       + $"compression {compression:F2} dB");
        Assert.InRange(compression, 10.9, 13.9);     // gear-drive.md: 12.4 dB

        // ...and at drive 0 it does not, which is what makes the knob real.
        const string clean = "drive=0;bias=0.5;hfRolloffKHz=20;hiss=0;mix=1";
        var loudC = Db(ToneLevel("tapesat", clean, 200.0, 0.708f));
        var quietC = Db(ToneLevel("tapesat", clean, 200.0, 0.0708f));
        Assert.True(Math.Abs(20.0 - (loudC - quietC)) < 0.5,
            "tapesat at drive 0 is meant to be linear");
    }

    [NativeAvailableFact]
    public void TapeSat_HfRolloff_MovesTenKilohertzByTheAmountThePageQuotes()
    {
        var dull = Db(ToneLevel("tapesat", "drive=0.2;hiss=0;mix=1;hfRolloffKHz=6", 10000.0, 0.1f));
        var open = Db(ToneLevel("tapesat", "drive=0.2;hiss=0;mix=1;hfRolloffKHz=20", 10000.0, 0.1f));
        var delta = open - dull;
        _out.WriteLine($"tapesat 10 kHz: roll-off 6 kHz {dull:F2} dB, 20 kHz {open:F2} dB, delta {delta:F2} dB");
        // gear-drive.md: 22.1 dB. It read 19.5 until the roll-off past a quarter
        // of the rate stopped being held at 12 kHz (DotCharter #87).
        Assert.InRange(delta, 20.6, 23.6);
    }

    [NativeAvailableFact]
    public void TapeSat_HissAtZero_AddsNothing_AndAtOneSitsAtMinusSeventyDbfs()
    {
        const int frames = 48000;

        // Silence in: with hiss 0 the module's whole path is odd-symmetric about
        // zero and every filter is at rest, so the output must be EXACTLY the
        // dry silence - bit for bit, not "quiet".
        Fresh("tapesat", "drive=1;bias=0.5;hiss=0;mix=1");
        var quiet = RenderBlocks(_ => 0f, frames, Sr, 3);
        foreach (var s in quiet)
            Assert.Equal(0f, s);

        // ...and hiss 1 is a -70 dBFS noise floor, which is the number the knob
        // is documented with.
        Fresh("tapesat", "drive=1;bias=0.5;hiss=1;mix=1");
        var noisy = RenderBlocks(_ => 0f, frames, Sr, 3);
        var rms = Rms(noisy, 0, frames);
        _out.WriteLine($"tapesat hiss 1 noise floor: {Db(rms):F2} dBFS");
        Assert.InRange(Db(rms), -73.0, -67.0);

        // The generator is DETERMINISTIC: a second identical run is bit-identical.
        Fresh("tapesat", "drive=1;bias=0.5;hiss=1;mix=1");
        var again = RenderBlocks(_ => 0f, frames, Sr, 3);
        Assert.Equal(noisy, again);
    }

    // ======================================================================
    // bassod - "Bass Overdrive"
    // ======================================================================

    [NativeAvailableFact]
    public void BassOd_AtBlendZero_TheCleanLegKeepsTheFortyOneHertzFundamental()
    {
        // Spectral BALANCE, not level: 41 Hz (a low-E bass fundamental) against
        // 1 kHz, which makes the claim independent of the output trim.
        const string cleanS = "drive=1;blend=0;bass=0;treble=0;level=0.6";
        var cleanBal = Db(ToneLevel("bassod", cleanS, 41.0, 0.3f))
                     - Db(ToneLevel("bassod", cleanS, 1000.0, 0.3f));

        // The driven leg on its own, kept just below saturation. It has to be
        // measured NEARLY CLEAN to be measured at all: at drive 1 the clipper
        // compresses both tones to about the same output level whatever went in,
        // so the pre-clip bass shelf stops showing and the two legs read alike
        // (measured: 0.00 dB against -0.49 dB). That is the clipper hiding the
        // difference, not the difference being absent.
        const string driveS = "drive=0.1;blend=1;bass=0;treble=0;level=0.6";
        var driveBal = Db(ToneLevel("bassod", driveS, 41.0, 0.3f))
                     - Db(ToneLevel("bassod", driveS, 1000.0, 0.3f));

        _out.WriteLine($"bassod 41 Hz/1 kHz balance: clean leg {cleanBal:F2} dB, driven leg {driveBal:F2} dB");
        Assert.True(Math.Abs(cleanBal) < 1.0,
            $"bassod at blend 0 moved the 41 Hz fundamental {cleanBal:F2} dB against 1 kHz - "
            + "the clean leg is meant to be a wire");
        Assert.True(driveBal < -2.0,
            $"the case is vacuous: the driven leg measured {driveBal:F2} dB, i.e. the same "
            + "balance as the clean one, so 'the clean leg keeps the fundamental' proves nothing");
    }

    [NativeAvailableFact]
    public void BassOd_Thd_RisesWithDrive()
    {
        double previous = -1;
        foreach (var drive in new[] { 0.0f, 0.3f, 0.6f, 1.0f })
        {
            var setup = string.Create(CultureInfo.InvariantCulture,
                $"blend=1;bass=0;treble=0;level=0.6;drive={drive}");
            var thd = Thd("bassod", setup, 82.0, 0.2f);
            _out.WriteLine($"bassod THD @ drive {drive:F1}: {thd * 100:F2} %");
            Assert.True(thd > previous,
                $"bassod THD did not rise at drive {drive:F1} ({thd * 100:F2} %)");
            previous = thd;
        }
    }

    // ======================================================================
    // tubepre - "Tube Preamp / DI"
    // ======================================================================

    /// <remarks>
    /// The bound is TWO-SIDED and centred on the figure the page quotes. The
    /// page promises "if one of these numbers moves, that test fails and this
    /// page is wrong"; a one-sided `> floor` cannot keep that promise, and
    /// every one of these was between 1.2 and 16 dB looser than the number it
    /// was supposed to pin (audit W5-A7). The tolerance is +/-1.5 dB for a
    /// level and +/-3 points for a THD percentage - wide enough that a
    /// compiler or a rate change does not flake it, narrow enough that a
    /// VOICING change forces a doc edit, which is the point.
    /// </remarks>
    [NativeAvailableFact]
    public void TubePre_Sag_PullsASustainedNoteDown_AndSagZeroDoesNot()
    {
        Assert.InRange(SustainDrop("sag=1"), 15.4, 18.4);   // gear-drive.md: 16.9 dB
        Assert.True(Math.Abs(SustainDrop("sag=0")) < 0.3, "sag 0 drooped anyway");

        // dB the level has fallen 500 ms into a sustained loud note, measured
        // against the first 20 ms of it.
        double SustainDrop(string sag)
        {
            Fresh("tubepre", $"gain=0.8;tone=0.5;level=0.8;bright=0;cabSim=0;{sag}");
            const int frames = 48000; // one second

            // Two blocks of silence first, so the chain's own node crossfade and
            // the sag follower are both at rest when the note starts.
            RenderBlocks(_ => 0f, frames, Sr, 2);

            var l = new float[frames];
            var r = new float[frames];
            for (var i = 0; i < frames; i++)
                l[i] = r[i] = (float)(0.5 * Math.Sin(2.0 * Math.PI * 110.0 * i / Sr));
            Assert.Equal(Ok, DspProcess(H, l, r, frames, Sr));

            var early = Rms(l, 240, 720);        // 5 ms .. 20 ms
            var late = Rms(l, 23760, 960);       // 495 ms .. 515 ms
            var drop = Db(early) - Db(late);
            _out.WriteLine($"tubepre {sag}: 20 ms {Db(early):F2} dB, 500 ms {Db(late):F2} dB, drop {drop:F2} dB");
            return drop;
        }
    }

    /// <remarks>
    /// The bound is TWO-SIDED and centred on the figure the page quotes. The
    /// page promises "if one of these numbers moves, that test fails and this
    /// page is wrong"; a one-sided `> floor` cannot keep that promise, and
    /// every one of these was between 1.2 and 16 dB looser than the number it
    /// was supposed to pin (audit W5-A7). The tolerance is +/-1.5 dB for a
    /// level and +/-3 points for a THD percentage - wide enough that a
    /// compiler or a rate change does not flake it, narrow enough that a
    /// VOICING change forces a doc edit, which is the point.
    /// </remarks>
    [NativeAvailableFact]
    public void TubePre_CabSim_DropsEightKilohertzByTheAmountThePageQuotes()
    {
        const string off = "gain=0.3;sag=0;bright=0;tone=0.5;level=0.6;cabSim=0";
        const string on = "gain=0.3;sag=0;bright=0;tone=0.5;level=0.6;cabSim=1";
        var dry = Db(ToneLevel("tubepre", off, 8000.0, 0.1f));
        var cab = Db(ToneLevel("tubepre", on, 8000.0, 0.1f));
        var drop = dry - cab;
        _out.WriteLine($"tubepre 8 kHz: cab off {dry:F2} dB, cab on {cab:F2} dB, drop {drop:F2} dB");
        Assert.InRange(drop, 25.5, 28.5);            // gear-drive.md: 27.0 dB

        // ...and it is a CAB, not a mute: the midrange survives.
        var midDry = Db(ToneLevel("tubepre", off, 800.0, 0.1f));
        var midCab = Db(ToneLevel("tubepre", on, 800.0, 0.1f));
        Assert.True(midDry - midCab < 4.0,
            $"tubepre's cab sim cut 800 Hz by {midDry - midCab:F2} dB - that is a mute, not a cab");
    }

    // ======================================================================
    // octfuzz - "Octave Fuzz"
    // ======================================================================

    [NativeAvailableFact]
    public void OctFuzz_AtFullOctave_TheUpperOctaveOutweighsTheFundamental()
    {
        const string full = "fuzz=0.7;tone=0.5;volume=0.5;octave=1";
        const string none = "fuzz=0.7;tone=0.5;volume=0.5;octave=0";

        Fresh("octfuzz", full);
        var yUp = RenderBlocks(Sine(200.0, 0.2f, Sr), ToneFrames, Sr, 2);
        var upF = Db(Amplitude(yUp, 200.0, Sr, 0, ToneFrames));
        var upO = Db(Amplitude(yUp, 400.0, Sr, 0, ToneFrames));

        Fresh("octfuzz", none);
        var yDry = RenderBlocks(Sine(200.0, 0.2f, Sr), ToneFrames, Sr, 2);
        var dryF = Db(Amplitude(yDry, 200.0, Sr, 0, ToneFrames));
        var dryO = Db(Amplitude(yDry, 400.0, Sr, 0, ToneFrames));

        _out.WriteLine($"octfuzz octave 1: 200 Hz {upF:F2} dB, 400 Hz {upO:F2} dB");
        _out.WriteLine($"octfuzz octave 0: 200 Hz {dryF:F2} dB, 400 Hz {dryO:F2} dB");

        Assert.True(upO > upF,
            $"octfuzz at octave 1 left the fundamental on top ({upF:F2} dB vs {upO:F2} dB at 400 Hz)");
        Assert.True(dryO < dryF,
            $"octfuzz at octave 0 is a plain fuzz and must keep its fundamental "
            + $"({dryF:F2} dB vs {dryO:F2} dB)");
    }

    // ======================================================================
    // Silence in, silence out
    // ======================================================================

    /// <summary>
    /// Every module in this drawer must be QUIET from digital silence. Two of
    /// them were not: <c>ds1</c> and <c>hm2</c> each run a first gain stage with
    /// a fixed operating-point bias, so with nothing playing they emit constant
    /// DC into the DC blocker behind them — and a blocker reset to zero state
    /// answers a DC step with a full-amplitude decaying pulse. Measured by the
    /// consumer's gate at −28.0 dBFS (ds1) and −33.9 dBFS (hm2) over the first
    /// half second after a prepare, against its −35 dBFS floor. That is a THUMP
    /// every time the pedal is added, the device changes, or the node is
    /// un-bypassed, and nothing on screen says why.
    ///
    /// <para>The floor here is −60 dBFS, not −35: the fault is a step, and a
    /// step either happens or it does not. The cores prime themselves on
    /// silence in <c>prepare()</c> and <c>reset()</c>, so the first audible
    /// sample is the steady state. (Note this reads the FIRST render after the
    /// node is added — the point is the transient, so there is deliberately no
    /// pre-roll to hide it behind.)</para>
    /// </summary>
    [NativeAvailableTheory]
    [MemberData(nameof(EachModule))]
    public void SilenceIn_StaysBelowTheFloor_FromTheVeryFirstBlock(string module)
    {
        // The catalogue defaults are the settings a freshly added pedal has.
        Fresh(module);
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
            $"{module} emitted {Db(rms):F1} dBFS RMS out of digital silence — a bias stage's DC "
            + "step through a DC blocker that starts from zero state");
    }

    // ======================================================================
    // Host block-size independence
    // ======================================================================

    /// <summary>
    /// <c>boost</c> is not new gear — it is one of the original 57 — but it
    /// lives in this drawer's family and its output depended on the host's
    /// BUFFER SIZE, which no other test here would have caught. The cause was a
    /// single per-BLOCK decision (<c>satActive_ || satSmooth_.isRamping()</c>)
    /// gating a per-FRAME signal path: the ramp landed at the same absolute
    /// sample in every chunking, but the branch only noticed at the next block
    /// boundary. Chunk 1 against chunk 1024 nulled to only −45.9 dBFS.
    ///
    /// <para>The <c>character</c> move mid-render is the whole point: with the
    /// knob left alone no ramp is ever in flight, so the case would be
    /// vacuous — which is why it asserts the reference render actually
    /// CHANGED.</para>
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(1)]
    [InlineData(64)]
    [InlineData(256)]
    public void Boost_ProducesTheSameOutput_WhateverChunkTheHostHandsIt(int chunk)
    {
        var reference = RenderChunkedWithAMove(1024);
        var got = RenderChunkedWithAMove(chunk);

        // Non-vacuity: there is audio to compare at all.
        var level = Rms(reference, 0, reference.Length);
        Assert.True(Db(level) > -30.0, $"the reference render is {Db(level):F1} dBFS — too quiet "
                                       + "for the null to say anything");

        var nulled = Db(RmsDiff(reference, got, 0, reference.Length));
        _out.WriteLine($"boost chunk {chunk} against 1024: null {nulled:F1} dBFS");
        Assert.True(nulled < -60.0,
            $"boost rendered in {chunk}-frame chunks differs from the same render in 1024-frame "
            + $"chunks by {nulled:F1} dBFS RMS — something in it is decided per BLOCK");

        float[] RenderChunkedWithAMove(int size)
        {
            var n = Fresh("boost", "gainDb=18;tilt=0.3;character=0.15");
            const int total = 8192;
            // Pre-roll, in the same chunk size, so the chain's own add
            // crossfade and both smoothers have landed before the comparison.
            RenderChunks(size, total, 0, null, 0f);
            return RenderChunks(size, total, total, n, 0.9f);
        }

        float Stim(int i) => (float)(0.3 * Math.Sin(2.0 * Math.PI * 110.0 * i / Sr));

        float[] RenderChunks(int size, int total, int offset, int? node, float moveTo)
        {
            var outp = new float[total];
            for (var done = 0; done < total; done += size)
            {
                var frames = Math.Min(size, total - done);
                // The move lands at ONE absolute sample, whatever `size` is: it
                // is written at the chunk boundary that reaches the halfway
                // mark, and every chunk size here divides it.
                if (node is { } id && done == total / 2)
                    Assert.Equal(Ok, DspSetParam(H, id, "character", moveTo));
                var l = new float[frames];
                var rr = new float[frames];
                for (var i = 0; i < frames; i++) l[i] = rr[i] = Stim(offset + done + i);

                Assert.Equal(Ok, DspProcess(H, l, rr, frames, Sr));
                Array.Copy(l, 0, outp, done, frames);
            }

            return outp;
        }
    }

    // ======================================================================
    // Aliasing
    //
    // A two-tone (1 kHz + 1.1 kHz) through a non-linearity produces IMD
    // products on a 100 Hz grid - and so do its ALIASES, because 48 kHz is a
    // whole multiple of 100 Hz. Frequency alone therefore cannot tell the two
    // apart, so this renders the SAME stimulus at 48 kHz and at 192 kHz
    // (where the same oversampling factor puts the fold 4x further out) and
    // compares the two spectra bin for bin. The transform sizes are chosen so
    // both have IDENTICAL bin spacing, i.e. bin k is the same frequency in Hz
    // in both; a legitimate IMD product then appears in both with the same
    // magnitude and cancels, and an alias appears only in the 48 kHz one.
    // ======================================================================

    private const int AliasN48 = 32768;
    private const int AliasN192 = 131072;
    /// Bin spacing is 48000/32768 == 192000/131072 == 1.46484375 Hz. The two
    /// tones sit ON bins 682 and 751 so neither leaks.
    private const double AliasBinHz = 48000.0 / AliasN48;
    private const int AliasBinF1 = 682;   // 999.02 Hz
    private const int AliasBinF2 = 751;   // 1100.10 Hz

    /// <remarks>
    /// Why the seven DRIVEN rows exist, measured rather than assumed. With
    /// <c>kOs</c> forced to 1 in all seven cores (2026-09-12), 4 of the 7
    /// DEFAULTS rows go red — ds1 -45.1 dB, hm2 -49.3, tubepre -49.6,
    /// octfuzz -56.8 — so a defaults-only theory was NOT vacuous for those
    /// four. It was for the other three: <c>rat</c> (-67.6), <c>bassod</c>
    /// (-137.8) and <c>tapesat</c> (-153.8) all pass at 1x with no
    /// oversampling in them at all. Driving them closes two of those three —
    /// rat -45.6 and bassod -46.4 both go red at 1x — and every driven row is
    /// green at the shipping factor.
    /// <para><c>tapesat</c> is the honest exception: it stays green at 1x even
    /// at <c>drive=1</c> (-131.7 dB), so no setting in its range makes its
    /// alias theory bite. Its knob maps to a gentle tanh under a 12 kHz
    /// roll-off; the 4x wrap is insurance, not a requirement at these
    /// settings, and saying so beats pretending the row proves something it
    /// does not.</para>
    /// This is lesson (2) of the 2026-09 gear wave,
    /// <c>docs/agents/dsp-tone-rig.md</c>.
    /// </remarks>
    [NativeAvailableTheory]
    [InlineData("rat", "distortion=0.5;filter=0.4;volume=0.6;clip=0")]
    [InlineData("ds1", "dist=0.5;tone=0.5;level=0.6")]
    [InlineData("hm2", "dist=0.7;lowDb=10;highDb=10;level=0.5")]
    [InlineData("tapesat", "drive=0.3;bias=0.5;hfRolloffKHz=12;hiss=0;mix=1")]
    [InlineData("bassod", "drive=0.4;blend=0.5;bass=0;treble=0;level=0.6")]
    [InlineData("tubepre", "gain=0.4;sag=0.3;bright=0;tone=0.5;level=0.6;cabSim=0")]
    [InlineData("octfuzz", "fuzz=0.7;octave=0.6;tone=0.5;volume=0.5")]
    // ...and the same seven DRIVEN, one knob at a time at its maximum.
    [InlineData("rat", "distortion=1;filter=0.4;volume=0.6;clip=0")]
    [InlineData("ds1", "dist=1;tone=0.5;level=0.6")]
    [InlineData("hm2", "dist=1;lowDb=10;highDb=10;level=0.5")]
    [InlineData("tapesat", "drive=1;bias=0.5;hfRolloffKHz=12;hiss=0;mix=1")]
    [InlineData("bassod", "drive=1;blend=0.5;bass=0;treble=0;level=0.6")]
    [InlineData("tubepre", "gain=1;sag=0.3;bright=0;tone=0.5;level=0.6;cabSim=0")]
    // octfuzz is driven on `fuzz` ALONE. At `octave=1` the rectifier all but
    // annihilates the probe tone, and this measurement normalises every bin by
    // the FUNDAMENTAL — so the denominator collapses and every legitimate
    // product reads as a +104 dB "alias" at 2,099 Hz. That is the metric
    // breaking down, not the pedal aliasing; the octave leg is still driven
    // hard at 0.5, where the fundamental survives to normalise against.
    [InlineData("octfuzz", "fuzz=1;octave=0.5;tone=0.5;volume=0.5")]
    public void AliasProducts_StayBelowMinusSixtyDb_AtDefaultsAndDriven(string module, string setup)
    {
        var m48 = Spectrum(module, setup, Sr, AliasN48);
        var m192 = Spectrum(module, setup, 192000, AliasN192);

        // Normalise each render by its OWN fundamental, so the comparison is of
        // spectral shape and a difference in output level cannot show up as an
        // alias.
        var ref48 = m48[AliasBinF1];
        var ref192 = m192[AliasBinF1];
        Assert.True(ref48 > 0 && ref192 > 0, $"{module}: no output at the probe tone");

        var worst = 0.0;
        var worstHz = 0.0;
        // From 150 Hz (above the DC blockers) to 18 kHz (below the 48 kHz
        // oversampler's 0.40*fs passband edge, where the two designs genuinely
        // differ).
        //
        // A bin counts as ALIAS-DOMINATED only when the 48 kHz render carries at
        // least 12 dB MORE there than the reference does. That threshold is the
        // whole measurement, and it is not arbitrary: ADAA1 is a first-order
        // approximation whose residual error shrinks with the oversampled step
        // size, so every strong legitimate IMD product is a fraction of a dB
        // BIGGER at 48 kHz than at 192 kHz. Subtracting raw magnitudes therefore
        // reported the 2*f2+f1 product at 3,199 Hz — the largest legitimate
        // product in the whole spectrum — as a -42 dB "alias" for six of the
        // seven modules. A real alias lands where the reference has essentially
        // nothing, which is exactly what the ratio test asks for.
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

    /// Hann-windowed magnitude spectrum of one rendered block. The stimulus is a
    /// function of TIME in seconds, so the 48 kHz and 192 kHz renders are the
    /// same signal.
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
