using System.Globalization;
using System.Runtime.InteropServices;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// The 2026-09 PITCH drawer: <c>detune</c>, <c>pitchcorrect</c>,
/// <c>twelvestring</c> and <c>basssynth</c> (<c>src/dsp/gear/PitchGear.cpp</c>
/// over four new <c>cores/dcdsp_*.h</c> and the shared building blocks in
/// <c>cores/dcdsp_detail_pitchgear.h</c>).
///
/// <para>Everything here renders through the SHIPPING wiring — the catalogue
/// row, the adapter's <c>ParamMailbox</c>, <c>DspChain</c>'s clamp and its node
/// crossfade — on a stopped engine via <c>dca_engine_dsp_process_buffer</c>,
/// exactly like <see cref="DspGainZipperTests"/>. A test that built a core by
/// hand would pass with the row, the adapter or the clamp missing entirely.</para>
///
/// <para>Two measurement notes, because a pitch effect defeats the harness the
/// other gear drawers use. (1) A plain Goertzel cannot separate 440 Hz from
/// 442 Hz without a second of audio AND a window — <see cref="Tone"/> is a
/// Hann-windowed DFT for that reason, and <see cref="PeakNear"/> scans it.
/// (2) Asking WHEN a correction landed is not a spectral question at all: 440
/// and 445 need ~200 ms to tell apart, which is longer than the thing being
/// timed. <see cref="InstantFreq"/> demodulates against a reference instead and
/// reads the phase slope, which resolves a few Hz over 40 ms.</para>
///
/// <para>The allocation gate, the oversized-block guard and the honest-latency
/// check for all four live in the RT scenario
/// <c>RtPitchGearIsAllocationFreeUnderScaleOctaveAndLatencyFlips</c>.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearPitchTests : IDisposable
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

    private const int Ok = 0;
    private const int Sr = 48000;

    /// Every module this file owns. The order is the catalogue's.
    public static readonly string[] PitchModules =
        ["detune", "pitchcorrect", "twelvestring", "basssynth"];

    public static TheoryData<string> EachModule
    {
        get
        {
            var d = new TheoryData<string>();
            foreach (var m in PitchModules) d.Add(m);
            return d;
        }
    }

    private readonly JuceAudioEngine? _engine;
    private readonly ITestOutputHelper _out;

    public GearPitchTests(ITestOutputHelper output)
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

    /// Render one contiguous stretch as <paramref name="blocks"/> consecutive
    /// blocks of a stimulus that is a function of the ABSOLUTE sample index, and
    /// return the WHOLE thing joined — the pitch assertions need to look at
    /// where in the run something happened, not only at the last block.
    private float[] RenderAll(Func<int, float> stim, int frames, int sampleRate, int blocks,
                              bool right = false)
    {
        var all = new float[frames * blocks];
        for (var b = 0; b < blocks; b++)
        {
            var l = new float[frames];
            var r = new float[frames];
            for (var i = 0; i < frames; i++) l[i] = r[i] = stim(b * frames + i);
            Assert.Equal(Ok, DspProcess(H, l, r, frames, sampleRate));
            Array.Copy(right ? r : l, 0, all, b * frames, frames);
        }
        return all;
    }

    private float[] RenderBlocks(Func<int, float> stim, int frames, int sampleRate, int blocks)
    {
        var all = RenderAll(stim, frames, sampleRate, blocks);
        var last = new float[frames];
        Array.Copy(all, all.Length - frames, last, 0, frames);
        return last;
    }

    private static Func<int, float> Sine(double freq, float amp, int sampleRate)
        => i => (float)(amp * Math.Sin(2.0 * Math.PI * freq * i / sampleRate));

    /// Hann-windowed DFT magnitude at an arbitrary frequency. The window is what
    /// makes this usable here: a rectangular Goertzel four bins away from a
    /// loud partial still leaks -13 dB, which is enough to swamp a detuned
    /// neighbour 2 Hz off a fundamental.
    private static double Tone(float[] x, double freq, int sampleRate, int start, int count)
    {
        double re = 0, im = 0, wsum = 0;
        for (var i = 0; i < count; i++)
        {
            var w = 0.5 - 0.5 * Math.Cos(2.0 * Math.PI * i / count);
            var s = x[start + i] * w;
            var a = 2.0 * Math.PI * freq * i / sampleRate;
            re += s * Math.Cos(a);
            im -= s * Math.Sin(a);
            wsum += w;
        }
        return 2.0 * Math.Sqrt(re * re + im * im) / wsum;
    }

    /// The frequency of the strongest partial in [lo, hi], to 0.05 Hz.
    private static double PeakNear(float[] x, double lo, double hi, int sampleRate, int start, int count)
    {
        double best = lo, bestM = -1;
        for (var f = lo; f <= hi; f += 0.05)
        {
            var m = Tone(x, f, sampleRate, start, count);
            if (m > bestM) { bestM = m; best = f; }
        }
        return best;
    }

    /// Instantaneous frequency at <paramref name="atSeconds"/>, by demodulating
    /// against <paramref name="reference"/> and reading the phase slope over
    /// 20 ms. Resolves a couple of Hz with 40 ms of support, where a spectral
    /// peak would need 200 ms — which is the whole point when the question is
    /// "how long did the glide take".
    private static double InstantFreq(float[] x, double reference, double atSeconds, int sampleRate)
    {
        var w = (int)(0.040 * sampleRate);
        double PhaseAt(int s)
        {
            double re = 0, im = 0;
            for (var i = 0; i < w; i++)
            {
                var a = 2.0 * Math.PI * reference * (s + i) / sampleRate;
                re += x[s + i] * Math.Cos(a);
                im -= x[s + i] * Math.Sin(a);
            }
            return Math.Atan2(im, re);
        }
        var s0 = (int)(atSeconds * sampleRate);
        var s1 = s0 + (int)(0.020 * sampleRate);
        var d = PhaseAt(s1) - PhaseAt(s0);
        while (d > Math.PI) d -= 2.0 * Math.PI;
        while (d < -Math.PI) d += 2.0 * Math.PI;
        return reference + d / (2.0 * Math.PI * 0.020);
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

    /// One second at 48 kHz.
    private const int ToneFrames = 48000;
    /// Ratio of a cent interval.
    private static double Cents(double n) => Math.Pow(2.0, n / 1200.0);

    // ======================================================================
    // Catalogue and parameter contracts
    // ======================================================================

    [NativeAvailableFact]
    public void AllFourPitchModules_AreInThePitchDrawer_WithLabelledDiscretes()
    {
        foreach (var id in PitchModules)
        {
            var info = JuceDspModuleCatalog.Find(id);
            Assert.NotNull(info);
            Assert.Equal("pitch", info!.Category);
            Assert.False(string.IsNullOrWhiteSpace(info.DisplayName));
            Assert.NotEmpty(info.Params);

            foreach (var p in info.Params)
            {
                Assert.True(p.Min <= p.Default && p.Default <= p.Max,
                    $"{id}.{p.Id}: default {p.Default} is outside [{p.Min}, {p.Max}]");
            }
        }

        // Every DISCRETE param in the drawer declares itself, and every one of
        // them is a NAMED choice rather than a count — a scale, a key, a
        // waveform, an octave and two footswitches are all pickers.
        AssertNamed("detune", "lowLatency", "Normal", "Low");
        AssertNamed("pitchcorrect", "scale", "Chromatic", "Major", "Minor", "Pentatonic", "Blues");
        AssertNamed("pitchcorrect", "key", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B");
        AssertNamed("pitchcorrect", "formant", "Off", "Preserve");
        AssertNamed("basssynth", "waveform", "Square", "Saw", "Sub-Sine", "PWM");
        AssertNamed("basssynth", "octave", "-2", "-1", "0");

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
            var inside = p.Min + 0.25f * (p.Max - p.Min);
            if (p.IsDiscrete) inside = MathF.Round(inside);
            Assert.Equal(Ok, DspSetParam(H, n, p.Id, inside));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var read));
            Assert.True(Math.Abs(read - inside) < 1e-5f,
                $"{module}.{p.Id}: wrote {inside}, read {read}");

            Assert.Equal(Ok, DspSetParam(H, n, p.Id, p.Max + 1000f));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var high));
            Assert.Equal(p.Max, high, 4);
            Assert.Equal(Ok, DspSetParam(H, n, p.Id, p.Min - 1000f));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var low));
            Assert.Equal(p.Min, low, 4);
        }

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

        // Every core's clamp is NaN-TRANSPARENT (`v < lo` and `v > hi` are both
        // false for a NaN), so the proof that the rejection matters is that the
        // module is still AUDIBLE: a NaN that had reached a coefficient — or,
        // here, a shifter's ratio or an oscillator's increment — would have
        // silenced it for good.
        var y = RenderBlocks(Sine(220.0, 0.25f, Sr), 4096, Sr, 8);
        Assert.True(Rms(y, 0, y.Length) > 1e-4,
            $"{module} went silent after a rejected non-finite parameter");
        foreach (var s in y) Assert.True(float.IsFinite(s));
    }

    /// <summary>
    /// Prepare at 48 kHz, then at 192 kHz, and require the same answer in Hz.
    /// Every module here owns something the RATE decides — the crossfade
    /// window and the pre-delay in samples, the tracker's decimation factor,
    /// its anti-alias corner, its lag bounds and its analysis hop, the
    /// oscillator increment, the LPC frame — and a quantity that is not
    /// re-derived in prepare() moves by a factor of FOUR between these two
    /// rates. This is a BEHAVIOURAL probe rather than a magnitude sweep,
    /// because for a tracker "does it still hear a 110 Hz note" is the question
    /// and a frequency response is not.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(48000)]
    [InlineData(192000)]
    public void PrepareAtAnotherRate_ReDerivesEveryRateOwnedQuantity(int rate)
    {
        var frames = rate;                 // one second, so 1 Hz bins at both
        var scale = rate / (double)Sr;

        // detune: the two voices still land a fixed INTERVAL either side.
        Fresh("detune", "cents=12;spreadMs=0;width=1;mix=1;lowLatency=0");
        var dl = RenderAll(Sine(440, 0.3f, rate), frames, rate, 3);
        var dr = new float[dl.Length];
        Fresh("detune", "cents=12;spreadMs=0;width=1;mix=1;lowLatency=0");
        dr = RenderAll(Sine(440, 0.3f, rate), frames, rate, 3, right: true);
        var up = PeakNear(dl, 430, 450, rate, frames, frames);
        var down = PeakNear(dr, 430, 450, rate, frames, frames);
        _out.WriteLine($"detune @ {rate}: +{up:F2} Hz / -{down:F2} Hz (want {440 * Cents(12):F2} / {440 * Cents(-12):F2})");
        Assert.True(Math.Abs(up - 440 * Cents(12)) < 1.0, $"detune up voice at {up:F2} Hz");
        Assert.True(Math.Abs(down - 440 * Cents(-12)) < 1.0, $"detune down voice at {down:F2} Hz");

        // ...and the latency the chain is told about is the same NUMBER OF
        // MILLISECONDS at both rates, which only holds if the window was
        // re-derived.
        var dn = Fresh("detune", "lowLatency=0");
        Assert.Equal(Ok, DspGetNodeLatency(H, dn, out var latNormal));
        Assert.Equal(Ok, DspSetParam(H, dn, "lowLatency", 1f));
        Assert.Equal(Ok, DspGetNodeLatency(H, dn, out var latLow));
        _out.WriteLine($"detune @ {rate}: latency {latNormal} / {latLow} samples");
        // The node was prepared at the engine's own rate, not `rate`; what this
        // asserts is the RATIO, which is geometry and not rate.
        Assert.True(latLow * 4 < latNormal && latLow * 5 > latNormal,
            $"the low-latency window should be about a fifth of the normal one, got {latLow}/{latNormal}");

        // twelvestring: a low note still grows an octave course.
        Fresh("twelvestring", "octaveLevel=1;unisonLevel=1;detuneCents=8;mix=1");
        var tw = RenderAll(Sine(110, 0.3f, rate), frames, rate, 3);
        var rel = Db(Tone(tw, 220, rate, frames, frames)) - Db(Tone(tw, 110, rate, frames, frames));
        _out.WriteLine($"twelvestring @ {rate}: 220/110 = {rel:F2} dB");
        Assert.True(rel > -12.0, $"the octave course measured {rel:F2} dB under the note");

        // pitchcorrect: a sharp A still snaps to A.
        Fresh("pitchcorrect", "scale=0;key=0;speedMs=0;amount=1;mix=1");
        var pc = RenderAll(Sine(445, 0.3f, rate), frames, rate, 3);
        var corrected = PeakNear(pc, 435, 450, rate, frames, frames);
        _out.WriteLine($"pitchcorrect @ {rate}: 445 -> {corrected:F2} Hz");
        Assert.True(Math.Abs(corrected - 440.0) < 1.0, $"445 Hz corrected to {corrected:F2} Hz");

        // basssynth: a 110 Hz note still drives a 55 Hz voice.
        Fresh("basssynth", "waveform=1;octave=1;cutoffHz=8000;envAmount=0;glideMs=0;dryMix=0;synthMix=1");
        var bs = RenderAll(Sine(110, 0.3f, rate), frames, rate, 3);
        var fund = PeakNear(bs, 50, 60, rate, frames, frames);
        _out.WriteLine($"basssynth @ {rate}: 110 -> {fund:F2} Hz  (scale {scale:F1}x)");
        Assert.True(Math.Abs(fund - 55.0) < 1.0, $"the synth fundamental landed at {fund:F2} Hz");
    }

    /// <summary>
    /// Sample-rate-independent in a second sense: the same audio, cut into
    /// blocks of different sizes, must come out the same. A core that keyed any
    /// per-sample state off the BLOCK (a smoother walked once per block, a
    /// coefficient interval reset at the block top, a tracker hop counter
    /// cleared on entry) fails this and nothing else in the suite sees it.
    /// </summary>
    [NativeAvailableTheory]
    [MemberData(nameof(EachModule))]
    public void BlockSizeDoesNotChangeTheOutput(string module)
    {
        var stim = Sine(196.0, 0.3f, Sr);

        Fresh(module);
        var big = RenderAll(stim, 8192, Sr, 4);
        Fresh(module);
        var small = RenderAll(stim, 512, Sr, 64);

        var n = Math.Min(big.Length, small.Length);
        var diff = RmsDiff(big, small, 0, n);
        var level = Rms(big, 0, n);
        _out.WriteLine($"{module}: 8192-frame vs 512-frame blocks, rms diff {diff:E3} against level {level:E3}");
        Assert.True(level > 1e-4, $"{module} produced nothing to compare");
        Assert.True(diff < 1e-5,
            $"{module} rendered differently at two block sizes (rms diff {diff:E3}) — "
            + "something per-sample is keyed off the block boundary");
    }

    // ======================================================================
    // Ramped levels — the DspGainZipperTests.MixCases shape, one case per
    // module plus the two pitch trajectories.
    //
    // Three renders of the same stimulus: the parameter never moves, the
    // parameter was `after` all along, and the seam we ship. A ramped seam is
    // still close to `before` 128 samples in and has ARRIVED by sample 1,200;
    // a hard step is on `after` from sample 0.
    // ======================================================================

    private const int SeamFrames = 2048;
    private const int EarlyWindow = 128;
    private const int LateStart = 1200;
    private const int SeamPreRoll = 12;

    public static TheoryData<string, string, string, float, float> SeamCases => new()
    {
        { "detune",       "cents=25;spreadMs=8;width=1",           "mix",         0f, 1f },
        { "detune",       "cents=25;spreadMs=8;mix=1",             "width",       0f, 1f },
        { "pitchcorrect", "scale=0;speedMs=50;amount=1",           "mix",         0f, 1f },
        { "twelvestring", "octaveLevel=1;unisonLevel=1",           "mix",         0f, 1f },
        { "twelvestring", "unisonLevel=0;mix=1",                   "octaveLevel", 0f, 1f },
        { "basssynth",    "waveform=1;octave=1;dryMix=0",          "synthMix",    0f, 1f },
        // The DELAY TAPS are NOT in this theory and that is deliberate: this
        // metric measures "how far toward the new setting", and moving a tap
        // half a period through a periodic signal is already maximally
        // different from both endpoints, so a perfectly smooth ramp scores
        // 145 %. They get their own instrument below.
    };

    [NativeAvailableTheory]
    [MemberData(nameof(SeamCases))]
    public void MovingALevelOrADelayTap_WalksAcrossTheSeam_InsteadOfStepping(
        string module, string setup, string param, float before, float after)
    {
        var refBefore = RenderSeam(module, setup, param, before, before);
        var refAfter = RenderSeam(module, setup, param, after, after);
        var seam = RenderSeam(module, setup, param, before, after);

        var earlySpan = RmsDiff(refBefore, refAfter, 0, EarlyWindow);
        var lateSpan = RmsDiff(refBefore, refAfter, LateStart, SeamFrames - LateStart);
        Assert.True(earlySpan > 0.005,
            $"{module}.{param}: the two settings are indistinguishable at the seam "
            + $"(early span {earlySpan:F5}) — the case is vacuous");
        Assert.True(lateSpan > 0.005,
            $"{module}.{param}: the two settings are indistinguishable later in the block "
            + $"(late span {lateSpan:F5}) — the case is vacuous");

        var earlyRatio = RmsDiff(seam, refBefore, 0, EarlyWindow) / earlySpan;
        _out.WriteLine($"{module}.{param}: early {earlyRatio * 100:F0} % of the way over");
        Assert.True(earlyRatio < 0.5,
            $"{module}.{param}: the first {EarlyWindow} samples after the change are "
            + $"{earlyRatio * 100:F0} % of the way to the new setting — a ramp is well under "
            + "half, a hard step is 100 %");
    }

    /// <summary>
    /// A delay tap may not TELEPORT. The right instrument for that is not "how
    /// far toward the new setting" — a tap moved half a period through a sine
    /// is further from both endpoints than either is from the other — but
    /// CONTINUITY: the largest sample-to-sample step just after the write, set
    /// against the largest step a steady render of the same stimulus makes.
    /// A spliced tap shows up as a jump of up to twice the peak amplitude in
    /// one sample; a ramped one is indistinguishable from the waveform's own
    /// slope.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("detune", "cents=25;width=0;mix=1", "spreadMs", 0f, 30f)]
    [InlineData("detune", "cents=25;width=0;mix=1", "lowLatency", 0f, 1f)]
    [InlineData("twelvestring", "octaveLevel=1;unisonLevel=1;mix=1", "delayMs", 0f, 20f)]
    // `formant` substitutes synth(shift(whiten(x))) for shift(x) — the whole wet
    // path, on one sample boundary, and onto an LPC whose rings were STALE
    // because push() was skipped while the switch was off. It appears in no
    // other theory here: SeamCases covers mix/width/octaveLevel, and the RT
    // scenario sets it once at setup and never toggles it.
    [InlineData("pitchcorrect", "scale=0;speedMs=50;amount=1;mix=1", "formant", 0f, 1f)]
    // `brightness` is up to 12 dB of high shelf, written straight into eight
    // biquad designs with no ramp (ensemble smooths `tone` before redesigning
    // and bbddelay ramps `age` for exactly this reason).
    [InlineData("twelvestring", "octaveLevel=1;unisonLevel=1;mix=1", "brightness", 0f, 1f)]
    public void MovingADelayTap_IsContinuous_NotASplice(
        string module, string setup, string param, float before, float after)
    {
        // A CHIRP, not a steady tone. A teleporting tap lands somewhere else in
        // the waveform, and in a periodic one "somewhere else" can be the same
        // value: a 975-sample jump at 196 Hz is 3.98 periods, so a deleted ramp
        // measured as a 4 % step and this test passed with the window written
        // straight through. A sweeping frequency has no such coincidence.
        var chirp = Chirp(80.0, 1500.0, 0.25f, Sr, SeamFrames * (SeamPreRoll + 1));
        // JOINED across the block boundary, and that is the whole measurement:
        // the write lands between two blocks, so the discontinuity is the step
        // from the last pre-roll sample to the first one after. A scan that
        // starts inside the new block misses it completely — a deleted window
        // ramp put the output at +0.03 where it had been at -0.27 and this test
        // passed, because every step WITHIN the new block was ordinary.
        var steady = RenderSeamJoined(module, setup, param, before, before, chirp);
        var seam = RenderSeamJoined(module, setup, param, before, after, chirp);

        var steadyStep = MaxStep(steady, 0, JoinTail + 512);
        var seamStep = MaxStep(seam, 0, JoinTail + 512);
        double peak = 0;
        for (var i = 0; i < JoinTail + 512; i++) peak = Math.Max(peak, Math.Abs(steady[i]));
        _out.WriteLine($"{module}.{param}: largest one-sample step {seamStep:F5} across the write, "
                       + $"{steadyStep:F5} steady, peak {peak:F5}");

        Assert.True(peak > 1e-4, $"{module}.{param}: the steady render is silent — the case is vacuous");
        Assert.True(steadyStep > 1e-6, $"{module}.{param}: the steady render is a DC level — the case is vacuous");
        // Not vacuous in the other direction either: the parameter must have
        // CHANGED something, or a no-op would pass this trivially.
        var refAfter = RenderSeamJoined(module, setup, param, after, after, chirp);
        Assert.True(RmsDiff(steady, refAfter, JoinTail + 1024, 1000) > 0.005,
            $"{module}.{param}: the two settings render the same audio — the case is vacuous");

        // The bound is set against the PEAK, not against the steady slope. A
        // spliced tap lands somewhere else in the waveform, so its jump is up
        // to twice the peak amplitude; a ramped one is bounded by the
        // waveform's own slope times the tap speed, and a tap that is allowed
        // to move up to half a sample per sample legitimately makes a step
        // several times the steady one without ever being discontinuous.
        Assert.True(seamStep < 0.2 * peak,
            $"{module}.{param}: one sample after the write jumped {seamStep:F5} against a peak "
            + $"amplitude of {peak:F5} — the tap teleported rather than glided");

        static double MaxStep(float[] x, int start, int count)
        {
            double m = 0;
            for (var i = start + 1; i < start + count; i++) m = Math.Max(m, Math.Abs(x[i] - x[i - 1]));
            return m;
        }
    }

    /// <summary>
    ///     The synth voice is GATED until there is a note, and the path carries
    ///     no DC once there is.
    ///
    ///     <para>Two faults, one probe each. (1) <c>freq_</c> starts at 0 and
    ///     the tracker needs ~45 ms to become confident, while the input
    ///     envelope's attack is 3 ms — so <c>PolyBlepOsc::value</c> was frozen
    ///     at a CONSTANT (<c>pulse(0, 0, 0.5)</c> is exactly +1) while the
    ///     envelope was already at 1. A chord never becomes confident at all, so
    ///     for a strum that was a DC thump at up to <c>synthMix</c> for as long
    ///     as it rang. (2) the PWM waveform's duty sweeps [0.15, 0.85] at
    ///     0.3 Hz, and a pulse of duty d has a DC of 2d − 1 — up to ±0.7 of
    ///     LFO-modulated offset, on a path with no blocker. None of the six
    ///     existing BassSynth_ cases drives anything but a clean sine the
    ///     tracker locks to.</para>
    /// </summary>
    [NativeAvailableFact]
    public void BassSynth_EmitsNoDc_OnAChordItCannotTrack_NorOnItsPwmWaveform()
    {
        // (1) a three-note chord: no one period, so the tracker holds forever.
        // The chord STARTS at NoteAt, four blocks in, so the chain's own 10 ms
        // add-crossfade (which leaks dry) is long finished before the window
        // that matters opens.
        const int NoteAt = 4 * SeamFrames;
        var raw = Chord([146.83, 185.00, 220.00], 0.2f, Sr);
        Func<int, float> chord = i => i >= NoteAt ? raw(i) : 0f;
        var n = Fresh("basssynth", "waveform=0;octave=1;dryMix=0;synthMix=1;cutoffHz=800");
        var y = RenderAll(chord, SeamFrames, Sr, 24);
        _ = n;

        // The first 45 ms: the tracker needs a full decimated window plus a hop
        // before it answers, and the input envelope's attack is 3 ms. That gap
        // is the whole fault -- the oscillator is frozen at a CONSTANT and the
        // envelope is already at 1.
        // The instrument is the MEAN, not the peak: a frozen oscillator is a
        // constant, and a constant is exactly what a mean sees and a peak does
        // not distinguish from a loud note.
        const int BeforeTheTracker = 2100;
        double mean = 0, early = 0, late = 0;
        for (var i = NoteAt; i < y.Length; i++)
        {
            mean += y[i];
            if (i < NoteAt + BeforeTheTracker) early += y[i];
            late = Math.Max(late, Math.Abs(y[i]));
        }
        mean /= y.Length - NoteAt;
        early /= BeforeTheTracker;
        _out.WriteLine($"basssynth chord: first 45 ms mean {early:E3}, whole-run mean {mean:E3}, peak {late:F4}");
        Assert.True(late > 0.05, "the synth never speaks at all -- the probe is vacuous");
        Assert.True(Math.Abs(early) < 0.1,
            $"the synth put {early:E3} of DC out in the first 45 ms, before the tracker had "
            + "answered -- a frozen oscillator at a constant is a thump, not a note");
        Assert.True(Math.Abs(mean) < 1e-3,
            $"the synth put {mean:E3} of DC out on a chord it cannot track");

        // (2) PWM on a note it CAN track: the block mean has to stay at zero as
        // the duty sweeps, which it cannot do without a blocker.
        Fresh("basssynth", "waveform=3;octave=1;dryMix=0;synthMix=1;cutoffHz=2000;envAmount=0");
        var z = RenderAll(Sine(196.0, 0.3f, Sr), SeamFrames, Sr, 100);

        // ONE SECOND windows, not one block: at 98 Hz a 2,048-frame block holds
        // 4.18 cycles, and the 0.18 of a cycle left over puts ~0.026 of the
        // waveform's own swing into a block mean whatever the DC is. A second
        // is 98 cycles and still only 0.3 of an LFO period, so it averages the
        // AC away and leaves the offset.
        var window = Sr;
        double worst = 0, energy = 0;
        for (var start = 4 * SeamFrames; start + window <= z.Length; start += window / 4)
        {
            double m = 0;
            for (var i = start; i < start + window; i++) m += z[i];
            worst = Math.Max(worst, Math.Abs(m / window));
        }
        for (var i = 0; i < z.Length; i++) energy += Math.Abs(z[i]);
        energy /= z.Length;
        _out.WriteLine($"basssynth PWM over 4.3 s: worst 1 s mean {worst:E3}, mean |x| {energy:F4}");
        Assert.True(energy > 0.01, "the PWM voice is silent — the probe is vacuous");
        Assert.True(worst < 0.05,
            $"the PWM waveform's LFO-modulated DC reached {worst:E3} — there is no blocker");
    }

    /// Three notes at once as a flat array: no single period, so an
    /// autocorrelation tracker never becomes confident about it.
    private static Func<int, float> Chord(double[] hz, float amp, int rate)
        => i =>
        {
            double v = 0;
            foreach (var f in hz) v += Math.Sin(2.0 * Math.PI * f * i / rate);
            return (float)(amp * v / hz.Length);
        };

    /// <summary>A switch written TWICE, the second write landing 3 ms into the
    /// 10 ms crossfade the first one started.
    ///
    /// <para>The instrument is the blend POSITION, not continuity: a saw and a
    /// pulse have full-scale discontinuities of their own, so a step probe on a
    /// synth voice cannot separate a splice from the waveform. Three renders of
    /// the identical stimulus — never moved, moved and left there, and the flip
    /// back — and the question is where the third one SITS. A crossfade that
    /// swaps its two ends and continues from <c>1 - t</c>
    /// (<c>dcdsp_tzflanger.h</c>'s <c>applyShape</c>) is already most of the way
    /// home the sample the flip lands; one that restarts at 0 is emitting the
    /// whole of the shape it was fading TO, i.e. it has jumped to 100 % of the
    /// setting the player just cancelled.</para></summary>
    [NativeAvailableTheory]
    [InlineData("basssynth", "octave=1;dryMix=0;synthMix=1;cutoffHz=600;resonance=0;envAmount=0", "waveform", 0f, 2f)]
    [InlineData("basssynth", "octave=1;dryMix=0;synthMix=1;cutoffHz=600;resonance=0;envAmount=0", "waveform", 1f, 3f)]
    public void FlippingAWaveformBackInsideItsCrossfade_KeepsTheBlendPosition(
        string module, string setup, string param, float a, float b)
    {
        // 3 ms at 48 kHz, inside the 10 ms crossfade. The synth needs a note it
        // is confident about, so the stimulus is the steady 196 Hz sine the rest
        // of this file uses rather than the chirp.
        const int InsideFade = 144;
        const int Window = 256;
        var stim = Sine(196.0, 0.3f, Sr);

        float[] Run(float second, float third)
        {
            var n = Fresh(module, setup);
            Assert.Equal(Ok, DspSetParam(H, n, param, a));
            RenderBlocks(stim, SeamFrames, Sr, SeamPreRoll);
            var at = SeamPreRoll * SeamFrames;

            Assert.Equal(Ok, DspSetParam(H, n, param, second));
            RenderAt(stim, at, InsideFade);
            at += InsideFade;

            Assert.Equal(Ok, DspSetParam(H, n, param, third));
            return RenderAt(stim, at, SeamFrames);
        }

        var stayedAtA = Run(a, a);       // the switch never happened
        var wentToB = Run(b, b);         // the switch happened and stuck
        var flippedBack = Run(b, a);     // …and was cancelled 3 ms in

        var span = RmsDiff(stayedAtA, wentToB, 0, Window);
        Assert.True(span > 0.01,
            $"{module}.{param}: {a} and {b} render the same audio — the case is vacuous");

        var toward = RmsDiff(flippedBack, stayedAtA, 0, Window) / span;
        _out.WriteLine($"{module}.{param}: {a} -> {b} -> {a} 3 ms apart sits {toward * 100:F0} % "
                       + $"away from {a} (span {span:F4})");
        Assert.True(toward < 0.6,
            $"{module}.{param}: after cancelling the switch 3 ms in, the output is still "
            + $"{toward * 100:F0} % of the way to {b} — the second write threw away the blend "
            + "in flight and restarted from the shape the player just cancelled");
    }

    /// One render of `frames` samples of `stim` starting at absolute index
    /// `offset`, returning the left channel.
    private float[] RenderAt(Func<int, float> stim, int offset, int frames)
    {
        var l = new float[frames];
        var r = new float[frames];
        for (var i = 0; i < frames; i++) l[i] = r[i] = stim(offset + i);
        Assert.Equal(Ok, DspProcess(H, l, r, frames, Sr));
        return l;
    }

    /// A logarithmic sweep as a flat array, so two renders of the same case are
    /// sample-aligned and no two instants carry the same waveform value.
    private static Func<int, float> Chirp(double f0, double f1, float amp, int sampleRate, int length)
    {
        var x = new float[length];
        double phase = 0;
        for (var i = 0; i < length; i++)
        {
            var f = f0 * Math.Pow(f1 / f0, i / (double)length);
            phase += 2.0 * Math.PI * f / sampleRate;
            x[i] = (float)(amp * Math.Sin(phase));
        }
        return i => i < x.Length ? x[i] : 0f;
    }

    /// <see cref="RenderSeam"/> with the last <c>JoinTail</c> samples of the
    /// PRE-ROLL block glued on the front, so index <c>JoinTail</c> is the first
    /// sample the parameter write could affect and the step into it is visible.
    private const int JoinTail = 64;

    private float[] RenderSeamJoined(string module, string setup, string param, float first, float second,
                                     Func<int, float>? stimulus = null)
    {
        var n = Fresh(module, setup);
        Assert.Equal(Ok, DspSetParam(H, n, param, first));
        var stim = stimulus ?? Sine(196.0, 0.3f, Sr);
        var preRoll = RenderBlocks(stim, SeamFrames, Sr, SeamPreRoll);

        Assert.Equal(Ok, DspSetParam(H, n, param, second));
        var l = new float[SeamFrames];
        var r = new float[SeamFrames];
        for (var i = 0; i < SeamFrames; i++) l[i] = r[i] = stim(SeamPreRoll * SeamFrames + i);
        Assert.Equal(Ok, DspProcess(H, l, r, SeamFrames, Sr));

        var joined = new float[JoinTail + SeamFrames];
        Array.Copy(preRoll, SeamFrames - JoinTail, joined, 0, JoinTail);
        Array.Copy(l, 0, joined, JoinTail, SeamFrames);
        return joined;
    }

    private float[] RenderSeam(string module, string setup, string param, float first, float second,
                              Func<int, float>? stimulus = null)
    {
        var n = Fresh(module, setup);
        Assert.Equal(Ok, DspSetParam(H, n, param, first));
        var stim = stimulus ?? Sine(196.0, 0.3f, Sr);
        RenderBlocks(stim, SeamFrames, Sr, SeamPreRoll);

        Assert.Equal(Ok, DspSetParam(H, n, param, second));
        var l = new float[SeamFrames];
        var r = new float[SeamFrames];
        for (var i = 0; i < SeamFrames; i++) l[i] = r[i] = stim(SeamPreRoll * SeamFrames + i);
        Assert.Equal(Ok, DspProcess(H, l, r, SeamFrames, Sr));
        return l;
    }

    // ======================================================================
    // detune — "Detune / Micro-Pitch"
    // ======================================================================

    [NativeAvailableFact]
    public void Detune_PutsOneVoiceEitherSideOfTheNote_OneToEachSpeaker()
    {
        Fresh("detune", "cents=12;spreadMs=0;width=1;mix=1;lowLatency=0");
        var left = RenderAll(Sine(440, 0.3f, Sr), ToneFrames, Sr, 3);
        Fresh("detune", "cents=12;spreadMs=0;width=1;mix=1;lowLatency=0");
        var right = RenderAll(Sine(440, 0.3f, Sr), ToneFrames, Sr, 3, right: true);

        var up = PeakNear(left, 430, 450, Sr, ToneFrames, ToneFrames);
        var down = PeakNear(right, 430, 450, Sr, ToneFrames, ToneFrames);
        _out.WriteLine($"detune: L {up:F2} Hz, R {down:F2} Hz (want {440 * Cents(12):F2} / {440 * Cents(-12):F2})");

        Assert.True(Math.Abs(up - 440 * Cents(12)) < 1.0,
            $"the left voice sits at {up:F2} Hz, not {440 * Cents(12):F2}");
        Assert.True(Math.Abs(down - 440 * Cents(-12)) < 1.0,
            $"the right voice sits at {down:F2} Hz, not {440 * Cents(-12):F2}");
        // ...and they are genuinely on opposite sides, which is the whole
        // reason the pedal exists. A width-1 render that put the same voice in
        // both speakers would pass both bounds above if the interval were 0.
        Assert.True(up - down > 4.0, $"both speakers got the same voice ({up:F2} vs {down:F2})");
    }

    [NativeAvailableFact]
    public void Detune_TurnedBackDownToZeroCents_IsFlat_NotACombFilter()
    {
        // THE trap in a two-tap shifter: at ratio 1 the taps STOP MOVING and
        // the pair is left as two fixed taps half a window apart, i.e. a comb.
        //
        // It matters WHERE the crossfade phase was parked when the ratio
        // arrived at 1, and that is why this drives the knob DOWN from 25 cents
        // after half a second rather than starting there. From a fresh node the
        // phase is exactly 0, where one tap's gain is 0 and the other's is 1 —
        // a single tap, accidentally flat — so a version of this test that
        // started at 0 cents passed with the bypass crossfade deleted. Landing
        // mid-window is the case the crossfade exists for.
        foreach (var f in new[] { 220.0, 330.0, 440.0, 660.0, 880.0 })
        {
            var n = Fresh("detune", "cents=25;spreadMs=0;width=1;mix=1");
            var stim = Sine(f, 0.3f, Sr);
            RenderBlocks(stim, ToneFrames / 2, Sr, 1);       // park the phase mid-window
            Assert.Equal(Ok, DspSetParam(H, n, "cents", 0f));
            var y = RenderAll(i => stim(ToneFrames / 2 + i), ToneFrames, Sr, 3);

            var level = Db(Tone(y, f, Sr, ToneFrames, ToneFrames));
            _out.WriteLine($"detune back at 0 cents, {f:F0} Hz: {level:F3} dB against a dry {Db(0.3):F3} dB");
            Assert.True(Math.Abs(level - Db(0.3)) < 0.5,
                $"detune at 0 cents is {level - Db(0.3):F2} dB off flat at {f:F0} Hz — that is a comb");
        }
    }

    [NativeAvailableFact]
    public void Detune_LowLatencyMode_ReportsTheShorterDelay_AndStaysUnderTenMilliseconds()
    {
        var n = Fresh("detune", "lowLatency=0");
        Assert.Equal(Ok, DspGetNodeLatency(H, n, out var normal));
        Assert.Equal(Ok, DspSetParam(H, n, "lowLatency", 1f));
        Assert.Equal(Ok, DspGetNodeLatency(H, n, out var low));
        _out.WriteLine($"detune latency: normal {normal} samples, low {low} samples");

        Assert.True(low < normal, "the low-latency mode reported at least as much delay as the normal one");
        // The pedal exists to be monitored through, so the short mode is
        // budgeted at 10 ms — below what a player hears as a doubled attack.
        Assert.True(low <= 480, $"the low-latency mode reports {low} samples (> 10 ms at 48 kHz)");
        Assert.True(normal > 480, $"the normal mode reports only {normal} samples — the two modes are the same window");
    }

    // ======================================================================
    // pitchcorrect — "Pitch Correction"
    // ======================================================================

    [NativeAvailableFact]
    public void PitchCorrect_SnapsASharpNoteBackToTheSemitone_WithinAHundredMilliseconds()
    {
        Fresh("pitchcorrect", "scale=0;key=0;speedMs=0;amount=1;humanize=0;formant=0;mix=1");
        var y = RenderAll(Sine(445, 0.3f, Sr), 4800, Sr, 40);   // 4 s at 100 ms a block

        // The tracker needs one analysis window (~43 ms) plus a hop before it
        // has an answer at all, so 100 ms is the first honest instant to ask.
        var at100 = InstantFreq(y, 440.0, 0.10, Sr);
        var settled = PeakNear(y, 435, 450, Sr, Sr, Sr);
        _out.WriteLine($"pitchcorrect: 445 Hz -> {at100:F2} Hz at 100 ms, {settled:F2} Hz settled");

        Assert.True(Math.Abs(at100 - 440.0) < 1.0,
            $"100 ms in, the corrected note is at {at100:F2} Hz, not 440");
        Assert.True(Math.Abs(settled - 440.0) < 1.0,
            $"the settled note is at {settled:F2} Hz, not 440");
    }

    [NativeAvailableFact]
    public void PitchCorrect_InCMajor_MovesANoteOffTheScaleOntoAScaleDegree()
    {
        // A#4 (466.16 Hz) is not in C major, and its two neighbours in the
        // scale are A4 (440) and B4 (493.88).
        Fresh("pitchcorrect", "scale=1;key=0;speedMs=0;amount=1;mix=1");
        var y = RenderAll(Sine(466.16, 0.3f, Sr), 4800, Sr, 40);
        var settled = PeakNear(y, 425, 510, Sr, Sr, Sr);
        _out.WriteLine($"pitchcorrect C major: 466.16 Hz -> {settled:F2} Hz");

        var toA = Math.Abs(settled - 440.0);
        var toB = Math.Abs(settled - 493.88);
        Assert.True(Math.Min(toA, toB) < 2.0,
            $"466.16 Hz landed at {settled:F2} Hz, which is neither A4 (440) nor B4 (493.88)");
        // Not vacuous: it MOVED. Chromatic would have left it on A#4.
        Assert.True(Math.Abs(settled - 466.16) > 10.0,
            $"the note did not move off A# at all ({settled:F2} Hz) — the scale mask is not being applied");
    }

    [NativeAvailableFact]
    public void PitchCorrect_SlowSpeed_ArrivesLaterThanTheRobotSnap()
    {
        // The DIFFERENTIAL is the assertion: an absolute "it took N ms" would
        // be a wall-clock number in disguise and would move with the tracker's
        // own lock time. What must be true is that the knob ORDERS the two.
        var fast = TimeToTarget(0f);
        var slow = TimeToTarget(500f);
        _out.WriteLine($"pitchcorrect: speed 0 reached 440 Hz at {fast * 1000:F0} ms, speed 500 at {slow * 1000:F0} ms");

        Assert.True(fast > 0, "the robot snap never reached the target at all");
        Assert.True(slow > 0, "the slow glide never reached the target at all");
        Assert.True(slow > fast + 0.15,
            $"speed 500 reached the note at {slow * 1000:F0} ms and speed 0 at {fast * 1000:F0} ms — "
            + "the knob barely orders them, so it is not a glide");

        double TimeToTarget(float speedMs)
        {
            Fresh("pitchcorrect", $"scale=0;key=0;speedMs={speedMs.ToString(CultureInfo.InvariantCulture)};amount=1;mix=1");
            var y = RenderAll(Sine(445, 0.3f, Sr), 4800, Sr, 40);
            for (var t = 0.05; t < 3.0; t += 0.005)
                if (Math.Abs(InstantFreq(y, 440.0, t, Sr) - 440.0) < 2.0) return t;
            return -1;
        }
    }

    [NativeAvailableFact]
    public void PitchCorrect_AtAmountZero_IsTheDrySignal()
    {
        Fresh("pitchcorrect", "scale=0;key=0;speedMs=0;amount=0;mix=1");
        var y = RenderAll(Sine(445, 0.3f, Sr), ToneFrames, Sr, 3);
        var level = Db(Tone(y, 445, Sr, ToneFrames, ToneFrames));
        var moved = PeakNear(y, 435, 452, Sr, ToneFrames, ToneFrames);
        _out.WriteLine($"pitchcorrect amount 0: {level:F4} dB at {moved:F2} Hz (dry {Db(0.3):F4} dB at 445)");

        Assert.True(Math.Abs(level - Db(0.3)) < 0.1,
            $"amount 0 measured {level:F4} dB where the dry signal is {Db(0.3):F4} dB");
        Assert.True(Math.Abs(moved - 445.0) < 0.5,
            $"amount 0 still moved the note to {moved:F2} Hz");
    }

    [NativeAvailableFact]
    public void PitchCorrect_Humanize_LeavesTheDeviationBehind()
    {
        // humanize is the counterweight to amount, and it is only meaningful if
        // it is measured against the CORRECTED note rather than against a
        // threshold: 1.0 leaves the whole deviation in, so the note stays sharp.
        Fresh("pitchcorrect", "scale=0;key=0;speedMs=0;amount=1;humanize=1;mix=1");
        var y = RenderAll(Sine(445, 0.3f, Sr), ToneFrames, Sr, 3);
        var kept = PeakNear(y, 435, 452, Sr, ToneFrames, ToneFrames);

        Fresh("pitchcorrect", "scale=0;key=0;speedMs=0;amount=1;humanize=0;mix=1");
        var z = RenderAll(Sine(445, 0.3f, Sr), ToneFrames, Sr, 3);
        var corrected = PeakNear(z, 435, 452, Sr, ToneFrames, ToneFrames);
        _out.WriteLine($"pitchcorrect: humanize 1 -> {kept:F2} Hz, humanize 0 -> {corrected:F2} Hz");

        Assert.True(Math.Abs(kept - 445.0) < 1.0, $"humanize 1 still corrected to {kept:F2} Hz");
        Assert.True(Math.Abs(corrected - 440.0) < 1.0, $"humanize 0 corrected to {corrected:F2} Hz");
    }

    // ======================================================================
    // twelvestring — "12-String Simulator"
    // ======================================================================

    [NativeAvailableFact]
    public void TwelveString_GrowsAnOctaveCourseUnderALowNote()
    {
        Fresh("twelvestring", "octaveLevel=1;unisonLevel=1;detuneCents=8;delayMs=6;brightness=0.5;mix=1");
        var y = RenderAll(Sine(110, 0.3f, Sr), ToneFrames, Sr, 3);
        var fund = Db(Tone(y, 110, Sr, ToneFrames, ToneFrames));
        var octave = Db(Tone(y, 220, Sr, ToneFrames, ToneFrames));
        _out.WriteLine($"twelvestring: 110 Hz {fund:F2} dB, 220 Hz course {octave:F2} dB, rel {octave - fund:F2} dB");

        Assert.True(octave - fund > -12.0,
            $"the octave course is {octave - fund:F2} dB under the note — the E A D G courses are missing");

        // Not vacuous: turn the course off and the 220 goes with it.
        Fresh("twelvestring", "octaveLevel=0;unisonLevel=1;detuneCents=8;mix=1");
        var z = RenderAll(Sine(110, 0.3f, Sr), ToneFrames, Sr, 3);
        var silentCourse = Db(Tone(z, 220, Sr, ToneFrames, ToneFrames));
        Assert.True(octave - silentCourse > 20.0,
            $"octaveLevel 0 still produced a 220 Hz course only {octave - silentCourse:F2} dB down");
    }

    [NativeAvailableFact]
    public void TwelveString_GivesAHighNoteADetunedUnison_AndNoOctave()
    {
        // The band split is what this measures. A 12-string's B and E courses
        // are UNISON pairs; a full-spectrum octave-up would put a course above
        // the high E, which no 12-string has — so the 880 must not be there.
        Fresh("twelvestring", "octaveLevel=1;unisonLevel=1;detuneCents=8;delayMs=6;brightness=0.5;mix=1");
        var y = RenderAll(Sine(440, 0.3f, Sr), ToneFrames, Sr, 3);

        var want = 440 * Cents(8);
        var neighbour = PeakNear(y, 441, 444, Sr, ToneFrames, ToneFrames);
        var unison = Db(Tone(y, want, Sr, ToneFrames, ToneFrames));
        var octave = Db(Tone(y, 880, Sr, ToneFrames, ToneFrames));
        _out.WriteLine($"twelvestring: unison peak {neighbour:F2} Hz (want {want:F2}) at {unison:F2} dB, 880 at {octave:F2} dB");

        Assert.True(Math.Abs(neighbour - want) < 1.0,
            $"the unison course sits at {neighbour:F2} Hz, not {want:F2}");
        Assert.True(unison - octave > 18.0,
            $"a 440 Hz note grew an 880 Hz course only {unison - octave:F2} dB under its unison — "
            + "the 250 Hz crossover is leaking into the octave band");
    }

    // ======================================================================
    // basssynth — "Bass Synth Tracker"
    // ======================================================================

    [NativeAvailableFact]
    public void BassSynth_TracksTheNoteAndPlaysItAnOctaveDown()
    {
        Fresh("basssynth", "waveform=1;octave=1;cutoffHz=8000;resonance=0;envAmount=0;glideMs=0;dryMix=0;synthMix=1");
        var y = RenderAll(Sine(110, 0.3f, Sr), ToneFrames, Sr, 3);
        var fund = PeakNear(y, 50, 60, Sr, ToneFrames, ToneFrames);
        _out.WriteLine($"basssynth: 110 Hz in, octave -1 -> {fund:F2} Hz");
        Assert.True(Math.Abs(fund - 55.0) < 1.0, $"the synth voice landed at {fund:F2} Hz, not 55");

        // ...and the octave switch is real.
        Fresh("basssynth", "waveform=1;octave=2;cutoffHz=8000;envAmount=0;glideMs=0;dryMix=0;synthMix=1");
        var z = RenderAll(Sine(110, 0.3f, Sr), ToneFrames, Sr, 3);
        var unison = PeakNear(z, 105, 115, Sr, ToneFrames, ToneFrames);
        Assert.True(Math.Abs(unison - 110.0) < 1.0, $"at octave 0 the voice landed at {unison:F2} Hz, not 110");
    }

    [NativeAvailableFact]
    public void BassSynth_SawIsBandLimited_SoItDoesNotAlias()
    {
        // A naive saw is a step every cycle and a step is flat past Nyquist, so
        // its fold-back is INHARMONIC and lands wherever it likes. PolyBLEP
        // subtracts a band-limited step at every discontinuity. The instrument
        // is the worst NON-HARMONIC partial: harmonics are the waveform and
        // must not be counted against it.
        Fresh("basssynth", "waveform=1;octave=2;cutoffHz=8000;resonance=0;envAmount=0;glideMs=0;dryMix=0;synthMix=1");
        var y = RenderAll(Sine(220, 0.3f, Sr), ToneFrames, Sr, 3);

        var f0 = PeakNear(y, 215, 225, Sr, ToneFrames, ToneFrames);
        var fund = Db(Tone(y, f0, Sr, ToneFrames, ToneFrames));
        double worst = -300, worstF = 0;
        for (var f = 40.0; f < 20000.0; f += 5.0)
        {
            var nearest = Math.Round(f / f0);
            if (Math.Abs(f - nearest * f0) < 12.0) continue;   // a harmonic of the saw
            var m = Db(Tone(y, f, Sr, ToneFrames, ToneFrames)) - fund;
            if (m > worst) { worst = m; worstF = f; }
        }
        _out.WriteLine($"basssynth saw f0 {f0:F2} Hz: worst non-harmonic partial {worst:F2} dB at {worstF:F0} Hz");
        Assert.True(worst < -60.0,
            $"the saw's worst non-harmonic partial is {worst:F2} dB at {worstF:F0} Hz — it is aliasing");
    }

    [NativeAvailableFact]
    public void BassSynth_GoesSilentAfterItsDecay()
    {
        // The level envelope decays LINEARLY to exact zero. An exponential
        // release is still 5 % of full scale after three time constants, which
        // is a synth humming 26 dB down over an empty bar.
        const int decayMs = 300;
        Fresh("basssynth", $"waveform=1;octave=1;cutoffHz=8000;envAmount=0;decayMs={decayMs};glideMs=0;dryMix=0;synthMix=1");
        var y = RenderAll(i => i < Sr ? 0.3f * MathF.Sin(2f * MathF.PI * 110f * i / Sr) : 0f,
                          4800, Sr, 20);

        var during = Rms(y, Sr / 2, Sr / 4);
        var tailStart = Sr + 3 * decayMs * Sr / 1000;
        double tail = 0;
        for (var i = tailStart; i < y.Length; i++) tail = Math.Max(tail, Math.Abs(y[i]));
        _out.WriteLine($"basssynth: {Db(during):F2} dBFS while playing, peak {Db(tail):F2} dBFS from {tailStart / (double)Sr:F2} s");

        Assert.True(during > 0.01, "the synth produced nothing while the note was playing");
        Assert.True(Db(tail) < -60.0,
            $"three decay times after the note stopped the synth is still at {Db(tail):F2} dBFS");
    }

    [NativeAvailableFact]
    public void BassSynth_LongGlide_ArrivesLaterThanNoGlide()
    {
        var fast = TimeToNewNote(0f);
        var slow = TimeToNewNote(300f);
        _out.WriteLine($"basssynth: glide 0 arrived at {fast * 1000:F0} ms, glide 300 at {slow * 1000:F0} ms");

        Assert.True(fast >= 0, "with no glide the voice never reached the second note");
        Assert.True(slow >= 0, "with a 300 ms glide the voice never reached the second note");
        Assert.True(slow > fast + 0.10,
            $"glide 300 arrived at {slow * 1000:F0} ms and glide 0 at {fast * 1000:F0} ms — "
            + "the portamento knob is not doing anything");

        double TimeToNewNote(float glideMs)
        {
            Fresh("basssynth", $"waveform=2;octave=2;cutoffHz=8000;envAmount=0;glideMs={glideMs.ToString(CultureInfo.InvariantCulture)};dryMix=0;synthMix=1");
            // Two notes, phase-continuous across the change so the step is in
            // the PITCH and not in the waveform.
            double phase = 0;
            var stim = new float[Sr * 2];
            for (var i = 0; i < stim.Length; i++)
            {
                phase += 2.0 * Math.PI * (i < Sr ? 110.0 : 220.0) / Sr;
                stim[i] = (float)(0.3 * Math.Sin(phase));
            }
            var y = RenderAll(i => stim[i], 4800, Sr, stim.Length / 4800);
            // Spectral DOMINANCE, not InstantFreq and not a bare peak.
            // Demodulation wraps its phase over the 20 ms it measures, so it
            // can only read +/- 25 Hz off its reference and an octave jump
            // aliases straight through it; and a bare peak over a window that
            // straddles the change picks whichever half is louder, which
            // reports "arrived" before the glide has started. The question
            // "has the voice left the old note for the new one" is exactly
            // 220 dominating 110.
            for (var s = Sr; s + 4800 < y.Length; s += 1200)
            {
                var newNote = Db(Tone(y, 220, Sr, s, 4800));
                var oldNote = Db(Tone(y, 110, Sr, s, 4800));
                if (newNote - oldNote > 12.0) return (s - Sr) / (double)Sr;
            }
            return -1;
        }
    }
    /// <summary>
    ///     No <c>std::lround(v)</c> on a value this drawer has not already
    ///     refused as non-finite — the sibling of
    ///     <c>GearFilterTests.EveryDiscreteParam_RefusesANaNBeforeItReachesLround</c>,
    ///     scoped to the FIVE drawers that file deliberately does not sweep.
    ///
    ///     <para><c>std::lround(NaN)</c> has an unspecified result and raises
    ///     <c>FE_INVALID</c>; every core here clamps NaN-safely afterwards, so
    ///     the SOUND was never at risk, but the adapter stores the result in
    ///     <c>p_</c> and <c>getParam</c> hands that straight back to the UI —
    ///     <c>(float) INT_MIN</c> for an ordinal.</para>
    ///
    ///     <para>The rule is spelled differently from the filter file's, because
    ///     these five guard differently and both shapes are correct: the guard
    ///     may be ON the line (a <c>clampNaN</c>/<c>clampf</c> around the
    ///     argument) or AT THE DOOR of the enclosing <c>setParam</c>, which is
    ///     what four of the five do and what <c>PitchGear.cpp</c> did not until
    ///     2026-09-12 while its header comment said it did. A call on anything
    ///     other than the raw <c>v</c> (<c>lround(p_.state)</c> in a getter, a
    ///     rate conversion of an already-validated field) is not this hazard and
    ///     is not swept.</para>
    /// </summary>
    [Fact]
    public void EveryDiscreteParamInTheseFiveDrawers_RefusesANaNBeforeItReachesLround()
    {
        string[] files = { "Modulation", "Pitch", "Delay", "Reverb", "Utility" };
        var offenders = new List<string>();
        foreach (var f in files)
        {
            var path = $"src/JuceSharp.Native/src/dsp/gear/{f}Gear.cpp";
            var lines = StripBlockComments(WrapperLifetimeAuditTests.ReadRepoFile(path)).Split('\n');
            var guarded = false;
            for (var i = 0; i < lines.Length; i++)
            {
                var line = lines[i];
                // A new member function body resets the door guard.
                if (line.Contains("bool setParam(", StringComparison.Ordinal)
                    || line.Contains("bool getParam(", StringComparison.Ordinal)
                    || line.Contains("void ", StringComparison.Ordinal) && line.Contains("()", StringComparison.Ordinal))
                    guarded = false;
                if (line.Contains("finite(v)", StringComparison.Ordinal) && line.Contains("return false", StringComparison.Ordinal))
                    guarded = true;

                if (!line.Contains("std::lround(v)", StringComparison.Ordinal)) continue;
                if (guarded) continue;
                if (line.Contains("clampNaN", StringComparison.Ordinal)
                    || line.Contains("clampf", StringComparison.Ordinal)
                    || line.Contains("isfinite", StringComparison.Ordinal)) continue;
                offenders.Add($"{f}Gear.cpp:{i + 1}: {line.Trim()}");
            }
        }

        Assert.True(offenders.Count == 0,
            "these adapters hand a NaN straight to std::lround: " + string.Join("; ", offenders));

        // …and the door guard itself, which is the form four of the five use and
        // the one PitchGear.cpp was missing on all four of its adapters while
        // its header comment said "every clamp rejects NaN". The lround sweep
        // above could not see that: PitchGear has no `lround(v)` at all, it
        // lrounds a RATE CONVERSION of an already-stored field, so the NaN it
        // let through went into `p_` and out of `getParam` instead.
        var unguarded = new List<string>();
        foreach (var f in files)
        {
            var path = $"src/JuceSharp.Native/src/dsp/gear/{f}Gear.cpp";
            var lines = StripBlockComments(WrapperLifetimeAuditTests.ReadRepoFile(path)).Split('\n');
            for (var i = 0; i < lines.Length; i++)
            {
                if (!lines[i].Contains("bool setParam(", StringComparison.Ordinal)) continue;
                // The first three STATEMENTS, comments and blanks discounted —
                // stripping comments leaves the lines behind, and half these
                // files explain the guard above it.
                var head = string.Join(" ", lines.Skip(i + 1)
                    .Where(l => l.Trim().Length > 0 && l.Trim() != "{")
                    .Take(3));
                if (head.Contains("finite(v)", StringComparison.Ordinal)
                    && head.Contains("return false", StringComparison.Ordinal)) continue;
                unguarded.Add($"{f}Gear.cpp:{i + 1}");
            }
        }

        Assert.True(unguarded.Count == 0,
            "these setParam bodies do not refuse a non-finite value at the door, so it lands in "
            + "p_ and getParam reports it: " + string.Join("; ", unguarded));
    }

    /// Comments only — a `see cref` or a worked example in a doc comment is
    /// exactly as inert as a deleted line, and both files here carry both.
    private static string StripBlockComments(string source)
    {
        var sb = new System.Text.StringBuilder(source.Length);
        var inBlock = false;
        foreach (var raw in source.Split('\n'))
        {
            var line = raw;
            if (inBlock)
            {
                var close = line.IndexOf("*/", StringComparison.Ordinal);
                if (close < 0) { sb.Append('\n'); continue; }
                line = line[(close + 2)..];
                inBlock = false;
            }
            var open = line.IndexOf("/*", StringComparison.Ordinal);
            if (open >= 0) { line = line[..open]; inBlock = true; }
            var slash = line.IndexOf("//", StringComparison.Ordinal);
            if (slash >= 0) line = line[..slash];
            sb.Append(line).Append('\n');
        }
        return sb.ToString();
    }

}
