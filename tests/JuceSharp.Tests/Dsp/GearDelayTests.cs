using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.InteropServices;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// The five modules of the 2026-09 <c>delay</c> gear wave — <c>multitap</c>,
/// <c>duckdelay</c>, <c>bbddelay</c>, <c>granular</c> and <c>rhythmdelay</c>
/// (<c>src/dsp/gear/DelayGear.cpp</c>, cores under <c>src/dsp/cores/</c>).
///
/// <para>Everything here renders OFFLINE on a stopped engine through
/// <c>dca_engine_dsp_process_buffer</c>, exactly like
/// <see cref="DspGainZipperTests"/> and <see cref="DspParamSeamTests"/>: that
/// path is deterministic (every core seeds its own PRNG in <c>prepare</c>, and
/// the stimulus is a pure function of the ABSOLUTE sample index), so two runs
/// of the same case are sample-aligned and a difference between them means
/// something.</para>
///
/// <para>Each module gets, at minimum: what it is FOR (an echo where it should
/// be, a duck of the size asked for, a darker long repeat, an octave, a
/// rhythm), a seam probe for the parameter that used to splice, a ramp probe
/// for its <c>mix</c>, a clamp/NaN probe, and a 48 k -> 192 k probe that the
/// rate-owned quantities were re-derived.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearDelayTests : IDisposable
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
    private const int Frames = 2048;
    private const int Rate48 = 48000;
    private const int Rate192 = 192000;

    /// Every module id this file covers, in catalogue order.
    public static TheoryData<string> Modules => new() { "multitap", "duckdelay", "bbddelay", "granular", "rhythmdelay" };

    private readonly JuceAudioEngine? _engine;
    private readonly ITestOutputHelper _out;

    public GearDelayTests(ITestOutputHelper output)
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
    // Rendering harness
    // ======================================================================

    /// One rendered run: interleaved-free left/right, `blocks` * Frames long.
    private sealed record Run(float[] L, float[] R);

    /// <summary>
    ///     A fresh single-node chain, then <paramref name="blocks"/> blocks of
    ///     <paramref name="stimulus"/>. `edits[b]` is applied BEFORE block b.
    /// </summary>
    private Run Render(string module, string setup, Func<int, float> stimulus, int blocks,
                       (int Block, string Spec)[]? edits = null, int rate = Rate48)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        Apply(n, setup);

        var l = new float[blocks * Frames];
        var r = new float[blocks * Frames];
        var bl = new float[Frames];
        var br = new float[Frames];
        for (var b = 0; b < blocks; b++)
        {
            if (edits is not null)
                foreach (var e in edits)
                    if (e.Block == b)
                        Apply(n, e.Spec);

            for (var i = 0; i < Frames; i++) bl[i] = br[i] = stimulus(b * Frames + i);
            Assert.Equal(Ok, DspProcess(H, bl, br, Frames, rate));
            Array.Copy(bl, 0, l, b * Frames, Frames);
            Array.Copy(br, 0, r, b * Frames, Frames);
        }
        return new Run(l, r);
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

    /// <summary>One sample of 1.0 at <paramref name="at"/>, silence elsewhere.</summary>
    private static Func<int, float> Impulse(int at) => i => i == at ? 1f : 0f;

    /// <summary>A steady sine at the absolute index — phase-continuous across blocks.</summary>
    private static Func<int, float> Sine(float hz, int rate = Rate48, float amp = 0.25f)
        => i => amp * MathF.Sin(2f * MathF.PI * hz * i / rate);

    /// <summary>A sine that stops dead at <paramref name="stopAt"/>.</summary>
    private static Func<int, float> SineUntil(float hz, int stopAt, int rate = Rate48, float amp = 0.25f)
        => i => i < stopAt ? amp * MathF.Sin(2f * MathF.PI * hz * i / rate) : 0f;

    /// <summary>Positional broadband noise (splitmix32 over the absolute index).</summary>
    private static float NoiseAt(int i)
    {
        var x = (uint)i * 0x9E3779B9u + 0x85EBCA6Bu;
        x ^= x >> 16; x *= 0x7FEB352Du;
        x ^= x >> 15; x *= 0x846CA68Bu;
        x ^= x >> 16;
        return 0.25f * ((x >> 8) * (1f / 8388608f) - 1f);
    }

    private static float Rms(float[] x, int start, int count)
    {
        double acc = 0;
        for (var i = start; i < start + count; i++) acc += (double)x[i] * x[i];
        return (float)Math.Sqrt(acc / count);
    }

    private static float RmsDiff(float[] a, float[] b, int start, int count)
    {
        double acc = 0;
        for (var i = start; i < start + count; i++) { double d = a[i] - b[i]; acc += d * d; }
        return (float)Math.Sqrt(acc / count);
    }

    private static float MaxStep(float[] x, int start, int count)
    {
        var max = 0f;
        for (var i = start + 1; i < start + count; i++) max = MathF.Max(max, MathF.Abs(x[i] - x[i - 1]));
        return max;
    }

    private static float Peak(float[] x, int start, int count)
    {
        var max = 0f;
        for (var i = start; i < start + count; i++) max = MathF.Max(max, MathF.Abs(x[i]));
        return max;
    }

    private static int ArgPeak(float[] x, int start, int count)
    {
        var best = start;
        for (var i = start; i < start + count; i++)
            if (MathF.Abs(x[i]) > MathF.Abs(x[best])) best = i;
        return best;
    }

    /// <summary>Goertzel magnitude of one bin, over a Hann-windowed segment.</summary>
    private static double Goertzel(float[] x, int start, int count, double hz, int rate)
    {
        var w = 2.0 * Math.PI * hz / rate;
        var coeff = 2.0 * Math.Cos(w);
        double s1 = 0, s2 = 0;
        for (var i = 0; i < count; i++)
        {
            var win = 0.5 - 0.5 * Math.Cos(2.0 * Math.PI * i / count);
            var s = x[start + i] * win + coeff * s1 - s2;
            s2 = s1; s1 = s;
        }
        return Math.Sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
    }

    /// <summary>Summed Goertzel energy over a band, sampled every 100 Hz.</summary>
    private static double BandEnergy(float[] x, int start, int count, double lo, double hi, int rate)
    {
        double acc = 0;
        for (var f = lo; f <= hi; f += 100.0)
        {
            var m = Goertzel(x, start, count, f, rate);
            acc += m * m;
        }
        return acc;
    }

    /// <summary>Frequency with the largest magnitude in [lo, hi], 1 Hz resolution.</summary>
    private static double DominantHz(float[] x, int start, int count, double lo, double hi, int rate)
    {
        double bestHz = lo, best = -1;
        for (var f = lo; f <= hi; f += 1.0)
        {
            var m = Goertzel(x, start, count, f, rate);
            if (m > best) { best = m; bestHz = f; }
        }
        return bestHz;
    }

    // ======================================================================
    // multitap
    // ======================================================================

    /// <summary>
    ///     Three taps at 100 ms with the Even spread land at 100 / 200 / 300 ms,
    ///     each quieter than the last. This is what the module IS; a tap table
    ///     that drifted, or a read position that was not settled when the first
    ///     <c>setParams</c> arrived, shows up here as a displaced echo.
    /// </summary>
    [NativeAvailableFact]
    public void Multitap_ThreeEvenTaps_LandOnTheirOffsets_WithDecayingLevels()
    {
        const int at = Frames; // one block of silence first: the chain's own add fade is 10 ms
        var run = Render("multitap",
            "taps=3;timeMs=100;spread=0;decay=0.6;pan=0;feedback=0;tone=1;mix=1",
            Impulse(at), 10);

        var expect = new[] { 4800, 9600, 14400 };
        var levels = new float[3];
        for (var k = 0; k < 3; k++)
        {
            var centre = at + expect[k];
            var found = ArgPeak(run.L, centre - 400, 800);
            Assert.True(Math.Abs(found - centre) <= 1,
                $"tap {k + 1} landed at sample {found}, expected {centre} (+/-1)");
            levels[k] = MathF.Abs(run.L[found]);
        }

        Assert.True(levels[0] > 0.5f, $"the first tap is inaudible ({levels[0]:F4})");
        Assert.True(levels[1] < levels[0] * 0.8f && levels[1] > 0.05f,
            $"tap 2 ({levels[1]:F4}) does not sit below tap 1 ({levels[0]:F4})");
        Assert.True(levels[2] < levels[1] * 0.8f && levels[2] > 0.01f,
            $"tap 3 ({levels[2]:F4}) does not sit below tap 2 ({levels[1]:F4})");
    }

    /// <summary>
    ///     The Dotted / Triplet / Golden spreads move the taps and nothing else.
    ///     A ratio table is exactly the kind of thing that is written once and
    ///     never checked, and a UI showing "Triplet" over even echoes is a lie
    ///     no allocation gate can see.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(1, 0.75f, 1.5f)]   // Dotted
    [InlineData(2, 2f / 3f, 4f / 3f)] // Triplet
    [InlineData(3, 0.618034f, 1.0f)]  // Golden
    public void Multitap_EachSpread_PutsItsTapsWhereItsNameSays(int spread, float r0, float r1)
    {
        const int at = Frames;
        var run = Render("multitap",
            $"taps=2;timeMs=200;spread={spread.ToString(CultureInfo.InvariantCulture)};"
            + "decay=0.3;pan=0;feedback=0;tone=1;mix=1",
            Impulse(at), 10);

        foreach (var (ratio, index) in new[] { (r0, 1), (r1, 2) })
        {
            var centre = at + (int)MathF.Round(ratio * 0.2f * Rate48);
            var found = ArgPeak(run.L, centre - 400, 800);
            Assert.True(Math.Abs(found - centre) <= 2,
                $"spread {spread} tap {index} landed at {found}, expected {centre} (+/-2)");
        }
    }

    /// <summary>
    ///     <c>taps</c> 1 -> 3 is a WEIGHTED FADE, not a branch. Two new taps
    ///     switched in at full level are a full-amplitude splice in whatever
    ///     those taps happen to be playing — the shape
    ///     <c>chorus.voices</c> shipped broken and this core copies the fix of.
    /// </summary>
    [NativeAvailableFact]
    public void Multitap_TapCountChange_FadesTheNewTapsIn_InsteadOfSplicing()
    {
        var run = Render("multitap",
            "taps=1;timeMs=37;spread=0;decay=0;pan=0;feedback=0;tone=1;mix=1",
            Sine(187.5f), 8, [(5, "taps=3")]);

        // The settled slope of the same signal, measured well before the change.
        var natural = MaxStep(run.L, 3 * Frames, Frames);
        Assert.True(natural > 1e-4f, "the probe signal is silent");

        // The change lands at the top of block 5; the 30 ms fade is 1,440 samples.
        var seam = MaxStep(run.L, 5 * Frames - 1, 2400);
        Assert.True(seam < 3f * natural,
            $"taps 1 -> 3 stepped the output by {seam:F4} against a settled slope of "
            + $"{natural:F4} — the extra taps switched in rather than fading in");

        // ...and it must actually have DONE something, or a core that ignored
        // the parameter would pass.
        var before = Rms(run.L, 4 * Frames, Frames);
        var after = Rms(run.L, 7 * Frames, Frames);
        Assert.True(MathF.Abs(after - before) > 0.01f,
            $"taps 1 -> 3 changed nothing (RMS {before:F4} -> {after:F4})");
    }

    /// <summary>
    ///     <c>pan</c> places the taps alternately: tap 1 right, tap 2 left. With
    ///     the width at 1 the first echo must be essentially absent from the
    ///     other side, or the knob is a label rather than a placement.
    /// </summary>
    [NativeAvailableFact]
    public void Multitap_Pan_PutsSuccessiveTapsOnAlternatingSides()
    {
        const int at = Frames;
        var run = Render("multitap",
            "taps=2;timeMs=100;spread=0;decay=0;pan=1;feedback=0;tone=1;mix=1",
            Impulse(at), 8);

        var firstL = Peak(run.L, at + 4700, 200);
        var firstR = Peak(run.R, at + 4700, 200);
        var secondL = Peak(run.L, at + 9500, 200);
        var secondR = Peak(run.R, at + 9500, 200);

        Assert.True(firstR > 0.5f, $"the first tap is inaudible on the right ({firstR:F4})");
        Assert.True(firstL < 0.1f * firstR,
            $"tap 1 at pan 1 leaked to the left (L {firstL:F4} vs R {firstR:F4})");
        Assert.True(secondL > 0.5f, $"the second tap is inaudible on the left ({secondL:F4})");
        Assert.True(secondR < 0.1f * secondL,
            $"tap 2 at pan 1 leaked to the right (R {secondR:F4} vs L {secondL:F4})");
    }

    // ======================================================================
    // duckdelay
    // ======================================================================

    /// <summary>
    ///     The repeats sit <c>duckDb</c> down while the dry input is above the
    ///     threshold, and swell back within 3x <c>releaseMs</c> of it stopping.
    ///
    ///     <para>Measured against the SAME render at <c>duckDb = 0</c> rather
    ///     than against an absolute level: the tail is decaying under both, so
    ///     only the ratio between the two isolates the duck.</para>
    /// </summary>
    [NativeAvailableFact]
    public void DuckDelay_AttenuatesTheRepeatsWhilePlaying_AndLetsThemSwellBack()
    {
        const int stop = 8 * Frames;
        const string setup = "timeMs=250;feedback=0.85;sensitivity=0.5;releaseMs=200;tone=1;mix=1";
        var flat = Render("duckdelay", setup + ";duckDb=0", SineUntil(187.5f, stop), 30);
        var duck = Render("duckdelay", setup + ";duckDb=12", SineUntil(187.5f, stop), 30);

        // While playing, and well past the first echo (250 ms = ~6 blocks).
        var playingFlat = Rms(flat.L, 7 * Frames, Frames);
        var playingDuck = Rms(duck.L, 7 * Frames, Frames);
        Assert.True(playingFlat > 0.01f, $"no repeats to duck (RMS {playingFlat:F5})");
        var duckedDb = 20f * MathF.Log10(playingDuck / playingFlat);
        Assert.True(MathF.Abs(duckedDb + 12f) < 1f,
            $"the repeats were pulled down by {-duckedDb:F2} dB, not the 12 dB asked for");

        // 3 x releaseMs = 600 ms = 28,800 samples at 48 kHz.
        const int recovered = stop + 28800;
        var restFlat = Rms(flat.L, recovered, Frames);
        var restDuck = Rms(duck.L, recovered, Frames);
        Assert.True(restFlat > 0.001f, $"the tail died before the probe window (RMS {restFlat:F5})");
        var restDb = 20f * MathF.Log10(restDuck / restFlat);
        Assert.True(MathF.Abs(restDb) < 1f,
            $"3x releaseMs after the input stopped the repeats are still {-restDb:F2} dB down — "
            + "they never swelled back");
    }

    // ======================================================================
    // bbddelay
    // ======================================================================

    /// <summary>
    ///     THE BBD IDENTITY: the anti-alias and reconstruction filters track the
    ///     bucket clock, so a longer setting is a darker echo. 600 ms must lose
    ///     more than 6 dB above 3 kHz against 100 ms — a delay whose filters are
    ///     fixed shows no difference at all here.
    /// </summary>
    [NativeAvailableFact]
    public void BbdDelay_LongerTimesAreDarker_BecauseTheFiltersTrackTheClock()
    {
        const string setup = "feedback=0;modDepth=0;age=0;mix=1";
        var shortRun = Render("bbddelay", setup + ";timeMs=100", NoiseAt, 26);
        var longRun = Render("bbddelay", setup + ";timeMs=600", NoiseAt, 26);

        // A window both settings have filled: 600 ms is 28,800 samples, so the
        // long line does not produce its first sample until block 15.
        const int start = 20 * Frames;
        var hiShort = BandEnergy(shortRun.L, start, 4096, 3000, 8000, Rate48);
        var hiLong = BandEnergy(longRun.L, start, 4096, 3000, 8000, Rate48);
        var loShort = BandEnergy(shortRun.L, start, 4096, 200, 1000, Rate48);
        var loLong = BandEnergy(longRun.L, start, 4096, 200, 1000, Rate48);

        Assert.True(loShort > 0 && loLong > 0, "one of the two renders is silent");
        // Normalised by each run's own low band, so this measures TILT and not
        // a level difference the feedback setting could produce.
        var tiltShort = hiShort / loShort;
        var tiltLong = hiLong / loLong;
        var dropDb = 10.0 * Math.Log10(tiltLong / tiltShort);
        Assert.True(dropDb < -6.0,
            $"600 ms is only {-dropDb:F1} dB darker than 100 ms above 3 kHz — the filters "
            + "are not following the clock");
    }

    /// <summary>
    ///     Feedback 1.0 is allowed — self-oscillation is the sound — and the
    ///     soft limiter in the loop is what makes it BOUNDED. Ten seconds at
    ///     the top of the knob must never reach 0 dBFS.
    /// </summary>
    [NativeAvailableFact]
    public void BbdDelay_FeedbackAtOne_SelfOscillatesWithoutReachingFullScale()
    {
        const int blocks = 10 * Rate48 / Frames; // ~10 s
        // At the top of the knob the loop gain is ABOVE unity by construction
        // (kRegenScale), so the line grows every lap and only the soft limiter
        // stops it. That is what makes this probe discriminating: with the
        // ceiling lifted the same 10 s run climbs past full scale, and with the
        // loop merely at unity (no regen scale) it would decay and the bound
        // would be met by a core that does nothing.
        var run = Render("bbddelay", "timeMs=120;feedback=1;modDepth=0.3;age=0.5;mix=1",
            SineUntil(187.5f, 4 * Frames), blocks);

        var peak = Peak(run.L, 0, run.L.Length);
        Assert.True(peak < 1f, $"feedback 1.0 reached {peak:F3} — the loop is not bounded");
        // ...and it must have actually GROWN past what went in, or "bounded"
        // is being satisfied by a loop that never regenerated.
        Assert.True(peak > 0.4f,
            $"feedback 1.0 never built past the input (peak {peak:F3}) — the top of the "
            + "regen knob is a decay, not an oscillation");
        foreach (var s in run.L) Assert.True(float.IsFinite(s), "the loop went non-finite");

        // Not vacuous: it has to still be ringing at the end, or "bounded" just
        // means the core muted itself.
        var tail = Rms(run.L, run.L.Length - Frames, Frames);
        Assert.True(tail > 0.01f, $"the oscillation died out (tail RMS {tail:F5}) — "
            + "feedback 1.0 should sustain");
    }

    /// <summary>
    ///     A time change DRAGS the read head: the echo's instantaneous pitch
    ///     dips while the clock slows and recovers when it lands. That is what
    ///     an analog time knob does, and the alternative — teleporting the read
    ///     position — is a splice inside a running feedback loop.
    /// </summary>
    [NativeAvailableFact]
    public void BbdDelay_TimeChange_GlidesThePitchDown_InsteadOfSplicing()
    {
        var run = Render("bbddelay", "timeMs=100;feedback=0;modDepth=0;age=0;mix=1",
            Sine(440f), 64, [(10, "timeMs=400")]);

        // Settled before the move, mid-dive, and settled long after.
        var before = DominantHz(run.L, 8 * Frames, 4096, 380, 470, Rate48);
        var during = DominantHz(run.L, 14 * Frames, 4096, 280, 470, Rate48);
        // 300 ms of delay change at kMaxSlew (0.25 samples/sample) is ~57,600
        // samples of dive, so "settled" is not reached until block ~38.
        var after = DominantHz(run.L, 58 * Frames, 4096, 380, 470, Rate48);

        Assert.True(Math.Abs(before - 440) < 5, $"the settled echo is at {before} Hz, not 440");
        Assert.True(during < before - 40,
            $"the echo stayed at {during} Hz while the time knob moved — the read position "
            + "jumped instead of gliding");
        Assert.True(Math.Abs(after - 440) < 5,
            $"the echo settled at {after} Hz instead of coming back to 440");

        // ...and the dive contains no splice.
        var natural = MaxStep(run.L, 8 * Frames, Frames);
        var glide = MaxStep(run.L, 10 * Frames - 1, 4 * Frames);
        Assert.True(glide < 3f * natural,
            $"the glide stepped by {glide:F4} against a settled slope of {natural:F4}");
    }

    // ======================================================================
    // granular
    // ======================================================================

    /// <summary>
    ///     +12 semitones reads every grain at twice real time, so a 220 Hz input
    ///     comes back as a 440 Hz cloud. A grain engine whose rate never reaches
    ///     the read position leaves the peak at 220 and looks perfectly healthy.
    /// </summary>
    [NativeAvailableFact]
    public void Granular_OctaveUp_PutsTheCloudAnOctaveAboveTheInput()
    {
        var run = Render("granular",
            "grainMs=80;density=8;pitchSemitones=12;spray=0;feedback=0;mix=1;freeze=0",
            Sine(200f), 16);

        const int start = 12 * Frames;
        Assert.True(Rms(run.L, start, 4096) > 0.01f, "the cloud is silent");
        var hz = DominantHz(run.L, start, 4096, 120, 600, Rate48);
        Assert.True(Math.Abs(hz - 400) <= 5,
            $"the dominant peak is at {hz} Hz, not 400 — the grain read rate is not 2x");
        // ...and the input's own pitch is GONE, not merely quieter than the octave.
        var octave = Goertzel(run.L, start, 4096, 400, Rate48);
        var root = Goertzel(run.L, start, 4096, 200, Rate48);
        Assert.True(octave > 8.0 * root,
            $"the 200 Hz input is still there under the octave (400 Hz {octave:F1} vs "
            + $"200 Hz {root:F1})");
    }

    /// <summary>
    ///     <c>freeze</c> stops the WRITE, so the cloud goes on circling what is
    ///     already in the ring after the input stops. Without it the ring fills
    ///     with silence and the cloud dies with the input.
    /// </summary>
    [NativeAvailableFact]
    public void Granular_Freeze_HoldsTheCloudAfterTheInputGoesSilent()
    {
        const int stop = 6 * Frames;
        const string setup = "grainMs=120;density=8;pitchSemitones=0;spray=0.2;feedback=0;mix=1";
        var running = Render("granular", setup + ";freeze=0", SineUntil(200f, stop), 60);
        var frozen = Render("granular", setup + ";freeze=0", SineUntil(200f, stop), 60,
            [(5, "freeze=1")]);

        // Two seconds after the input stops: 2 s = 96,000 samples = ~47 blocks.
        const int late = 50 * Frames;
        var liveLevel = Rms(running.L, late, 4096);
        var heldLevel = Rms(frozen.L, late, 4096);
        Assert.True(heldLevel > 0.02f, $"the frozen cloud faded out (RMS {heldLevel:F5})");
        Assert.True(liveLevel < 0.2f * heldLevel,
            $"the UNfrozen cloud is still going (RMS {liveLevel:F5} against {heldLevel:F5}) — "
            + "freeze cannot be shown to do anything");

        // ...and what it holds is the SPECTRUM it captured, not noise.
        var hz = DominantHz(frozen.L, late, 4096, 120, 400, Rate48);
        Assert.True(Math.Abs(hz - 200) <= 5, $"the frozen cloud is at {hz} Hz, not 200");
    }

    /// <summary>
    ///     The worst case the knobs allow — 32 concurrent grains of 500 ms —
    ///     renders finite, audible audio. The COST of it is measured by the RT
    ///     gate's <c>RtDelayGearRigIsAllocationFree</c>, which reports ns/sample;
    ///     here the point is only that the configuration is reachable and the
    ///     ring is big enough for it (a grain that walked out of the ring would
    ///     go silent, and one that walked past the head would read unwritten
    ///     samples).
    /// </summary>
    /// <summary>
    ///     Lowering <c>grainMs</c> at high density must not CUT the grains
    ///     already in flight.
    ///
    ///     <para>The round robin returns to a slot after
    ///     <c>kMaxGrains * hop == grainLen</c> samples — exactly a grain's life,
    ///     which is safe while <c>grainLen</c> is steady. Shorten it and the
    ///     cycle time drops below the life of the grains already playing, so a
    ///     slot at a non-zero window position is overwritten: a discontinuity,
    ///     and a replacement started at full window position 0 rather than faded
    ///     in. The RT scenario changes <c>density</c> mid-run but not
    ///     <c>grainMs</c>, and nothing asserted continuity.</para>
    ///
    ///     <para>The CONTROL is the same render with <c>grainMs</c> left alone:
    ///     a grain cloud has plenty of slope of its own, so this signal's own
    ///     settled step is not a reference the seam can be judged against.</para>
    /// </summary>
    [NativeAvailableFact]
    public void Granular_ShorteningTheGrain_DoesNotCutTheOnesInFlight()
    {
        const string setup = "grainMs=500;density=32;pitchSemitones=0;spray=0;feedback=0;mix=1";
        // 40 blocks with the change at 24: a 500 ms grain reads 24,000 samples back,
        // so the cloud is SILENT until the ring has that much real audio in it.
        var run = Render("granular", setup, Sine(187.5f), 40, [(24, "grainMs=25")]);
        var control = Render("granular", setup, Sine(187.5f), 40);

        var natural = MaxStep(control.L, 24 * Frames - 1, 2 * Frames);
        Assert.True(natural > 1e-4f, "the probe signal is silent");
        Assert.True(RmsDiff(run.L, control.L, 28 * Frames, Frames) > 0.005f,
            "shortening the grain produced the same audio — the case is vacuous");

        // Read ACROSS the render boundary: the write lands at sample 0 of the
        // next block, so a scan that starts inside it misses a sample-0 step.
        var seam = MaxStep(run.L, 24 * Frames - 1, 2 * Frames);
        _out.WriteLine($"granular grainMs 500 -> 25 at density 32: largest step {seam:F5}, "
            + $"{natural:F5} with the knob left alone");
        // 1.5x: measured 0.00720 with the guard in and 0.01737 without it,
        // against 0.00615 for the same render with the knob left alone.
        Assert.True(seam < 1.5f * natural,
            $"shortening the grain stepped the output by {seam:F5} against {natural:F5} for the "
            + "same render with the knob left alone — a slot still mid-window was overwritten");
    }

    [NativeAvailableFact]
    public void Granular_MaximumDensityAndGrainLength_StillRendersAudio()
    {
        var run = Render("granular",
            "grainMs=500;density=32;pitchSemitones=12;spray=1;feedback=0.5;mix=1;freeze=0",
            NoiseAt, 30);

        foreach (var s in run.L) Assert.True(float.IsFinite(s), "the cloud went non-finite");
        Assert.True(Peak(run.L, 0, run.L.Length) < 2f, "the cloud ran away");
        Assert.True(Rms(run.L, 24 * Frames, 4096) > 0.01f, "the cloud is silent at full density");
    }

    // ======================================================================
    // rhythmdelay
    // ======================================================================

    /// <summary>
    ///     The repeats come back through the step pattern: at <c>gateDepth 1</c>
    ///     an 'x' step is audible and a '.' step is not. Pattern 5
    ///     ("x...x...") is the clearest of the eight — one step on, three off.
    /// </summary>
    [NativeAvailableFact]
    public void RhythmDelay_TheRepeats_FollowTheChosenStepPattern()
    {
        const int step = 2400; // 50 ms at 48 kHz
        var run = Render("rhythmdelay",
            "timeMs=100;feedback=0.7;pattern=5;stepMs=50;gateDepth=1;gateShape=0.2;mix=1",
            Sine(187.5f), 30);

        // Well past the first echo and any start-up transient; the sequencer
        // free-runs from sample 0, so step k covers [k*6000, (k+1)*6000).
        var on = new List<float>();
        var off = new List<float>();
        for (var s = 8; s < 24; s++)
        {
            // Measure the MIDDLE of each step, clear of both edge ramps.
            var start = s * step + step / 4;
            var rms = Rms(run.L, start, step / 2);
            if (s % 4 == 0) on.Add(rms); else off.Add(rms);
        }

        var onAvg = on.Average();
        var offAvg = off.Average();
        Assert.True(onAvg > 0.01f, $"the gated repeats are inaudible even on an 'x' step ({onAvg:F5})");
        var db = 20f * MathF.Log10(offAvg / onAvg);
        Assert.True(db < -20f,
            $"a '.' step is only {-db:F1} dB below an 'x' step — the pattern is not gating");
    }

    /// <summary>
    ///     A pattern change only re-targets the gate SMOOTHER, so it crossfades
    ///     over the <c>gateShape</c> slope wherever in the step it lands. A gate
    ///     written straight to the output is a click eight times a bar, and a
    ///     pattern change is where it is loudest.
    /// </summary>
    [NativeAvailableFact]
    public void RhythmDelay_APatternChange_CrossfadesTheGate_InsteadOfClicking()
    {
        const string setup = "timeMs=100;feedback=0.6;pattern=6;stepMs=125;gateDepth=1;gateShape=0.2;mix=1";
        var run = Render("rhythmdelay", setup, Sine(187.5f), 16, [(8, "pattern=5")]);
        // The CONTROL is the same render with the pattern left alone. Measuring
        // against this signal's OWN settled slope is not enough: the sequencer
        // puts a (ramped) edge at every step boundary, so a module that wrote
        // every edge hard would raise the reference along with the seam and
        // pass — measured, it does. The control has the step edges and not the
        // change.
        var control = Render("rhythmdelay", setup, Sine(187.5f), 16);

        // gateShape 0.2 of a 125 ms step is 25 ms: a full 0 -> 1 edge can move
        // at most 1/1200 of full scale per sample at 48 kHz.
        var natural = MaxStep(control.L, 8 * Frames - 1, 2 * Frames);
        Assert.True(natural > 1e-4f, "the probe signal is silent");
        Assert.True(RmsDiff(run.L, control.L, 10 * Frames, Frames) > 0.01f,
            "the pattern change produced the same audio — the case is vacuous");

        var seam = MaxStep(run.L, 8 * Frames - 1, 2 * Frames);
        Assert.True(seam < 1.5f * natural,
            $"the pattern change stepped the output by {seam:F4} against {natural:F4} for the "
            + "same render with the pattern left alone — it wrote the gate instead of "
            + "re-targeting the smoother");
    }

    /// <summary>
    ///     <c>gateDepth 0</c> is a TRUE no-op: the gate multiplier is exactly
    ///     1.0f, so the pattern and the step length cannot reach the audio at
    ///     all and the module is a plain delay. Asserted bit for bit, because
    ///     "almost identical" would hide a smoother that is quietly running.
    /// </summary>
    [NativeAvailableFact]
    public void RhythmDelay_GateDepthZero_IsBitIdenticalWhateverThePattern()
    {
        const string common = "timeMs=100;feedback=0.6;gateDepth=0;gateShape=0.2;mix=1";
        var a = Render("rhythmdelay", common + ";pattern=0;stepMs=125", Sine(187.5f), 10);
        var b = Render("rhythmdelay", common + ";pattern=5;stepMs=500", Sine(187.5f), 10);
        var gated = Render("rhythmdelay",
            "timeMs=100;feedback=0.6;gateDepth=1;gateShape=0.2;mix=1;pattern=5;stepMs=500",
            Sine(187.5f), 10);

        for (var i = 0; i < a.L.Length; i++)
            Assert.True(a.L[i] == b.L[i],
                $"gateDepth 0 is not a no-op: sample {i} is {a.L[i]} with pattern 0 and "
                + $"{b.L[i]} with pattern 5");

        // Not vacuous: the same two settings at gateDepth 1 must differ a lot.
        Assert.True(RmsDiff(a.L, gated.L, 6 * Frames, Frames) > 0.01f,
            "the gate does nothing at depth 1 either — the case proves nothing");
    }

    // ======================================================================
    // Cross-module: mix ramps, clamps, rate derivation
    // ======================================================================

    /// <summary>
    ///     Every module's <c>mix</c> WALKS across a change instead of switching
    ///     (the <see cref="DspGainZipperTests"/> MixCases shape, repeated here
    ///     because that file's theory data is owned by another change). Three
    ///     renders: never moved, moved from the start, moved at the seam. A
    ///     hard switch puts the seam ON the new setting from sample 0.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("multitap", "taps=3;timeMs=90;feedback=0.4;tone=1;pan=0")]
    [InlineData("duckdelay", "timeMs=90;feedback=0.4;duckDb=0;tone=1")]
    [InlineData("bbddelay", "timeMs=90;feedback=0.4;age=0;modDepth=0")]
    [InlineData("granular", "grainMs=60;density=8;pitchSemitones=7;spray=0;feedback=0")]
    [InlineData("rhythmdelay", "timeMs=90;feedback=0.4;gateDepth=0")]
    public void EveryDelayGearModule_RampsItsMix_InsteadOfSwitching(string module, string setup)
    {
        const int seamBlock = 6;
        const int earlyWindow = 128;
        const int lateStart = 1200;

        var stim = Sine(187.5f);
        var refBefore = Render(module, setup + ";mix=0", stim, seamBlock + 1);
        var refAfter = Render(module, setup + ";mix=1", stim, seamBlock + 1);
        var seam = Render(module, setup + ";mix=0", stim, seamBlock + 1, [(seamBlock, "mix=1")]);

        var b0 = seamBlock * Frames;
        var earlySpan = RmsDiff(refBefore.L, refAfter.L, b0, earlyWindow);
        var lateSpan = RmsDiff(refBefore.L, refAfter.L, b0 + lateStart, Frames - lateStart);
        Assert.True(earlySpan > 0.005f,
            $"{module}.mix: the two settings are indistinguishable at the seam "
            + $"(span {earlySpan:F5}) — the case is vacuous");
        Assert.True(lateSpan > 0.005f,
            $"{module}.mix: the two settings are indistinguishable later in the block "
            + $"(span {lateSpan:F5}) — the case is vacuous");

        var earlyRatio = RmsDiff(seam.L, refBefore.L, b0, earlyWindow) / earlySpan;
        Assert.True(earlyRatio < 0.5f,
            $"{module}.mix: the first {earlyWindow} samples after the change are "
            + $"{earlyRatio * 100f:F0} % of the way to the new setting — a ramp is well under "
            + "half, a hard switch is 100 %");

        var lateRatio = RmsDiff(seam.L, refAfter.L, b0 + lateStart, Frames - lateStart) / lateSpan;
        Assert.True(lateRatio < 0.15f,
            $"{module}.mix: {Frames - lateStart} samples later the seam is still "
            + $"{lateRatio * 100f:F0} % away from the new setting — the ramp never landed");
    }

    /// <summary>
    ///     Every declared param of every module in this wave reads back inside
    ///     its declared range after an out-of-range write, and a NaN is REFUSED
    ///     at the door rather than stored. A NaN that reaches a core is
    ///     permanent: every <c>v &lt; lo</c> / <c>v &gt; hi</c> is false for one,
    ///     so it lands in the coefficients and the module is silent for good.
    /// </summary>
    [NativeAvailableTheory]
    [MemberData(nameof(Modules))]
    public void EveryDelayGearParam_ClampsToItsRange_AndRefusesNaN(string module)
    {
        var info = JuceDspModuleCatalog.Find(module);
        Assert.NotNull(info);
        Assert.Equal("delay", info!.Category);

        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");

        var probed = 0;
        foreach (var p in info.Params)
        {
            var span = MathF.Max(1f, MathF.Abs(p.Max - p.Min));
            foreach (var probe in new[] { p.Max + span, p.Min - span })
            {
                Assert.Equal(Ok, DspSetParam(H, n, p.Id, probe));
                Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var read));
                var slack = MathF.Max(p.Step, 1e-4f * span);
                Assert.True(read >= p.Min - slack && read <= p.Max + slack,
                    $"{module}.{p.Id}: set {probe}, read back {read}, declared [{p.Min}, {p.Max}]");
                probed++;
            }

            // A NaN must be refused, and must not have moved the stored value.
            Assert.Equal(Ok, DspSetParam(H, n, p.Id, p.Default));
            Assert.NotEqual(Ok, DspSetParam(H, n, p.Id, float.NaN));
            Assert.Equal(Ok, DspGetParam(H, n, p.Id, out var kept));
            Assert.Equal(p.Default, kept, 3);
        }

        Assert.True(probed >= 12, $"{module}: only {probed} params probed — the sweep is vacuous");
    }

    /// <summary>
    ///     Re-preparing at 192 kHz re-derives everything the RATE owns: ring
    ///     sizes, read positions, LFO increments, filter corners, step counters.
    ///     The probe is the same measurement in MILLISECONDS (or Hz) at both
    ///     rates — a quantity that was computed once at 48 kHz and cached lands
    ///     four times early, or four times low, at 192 kHz.
    ///
    ///     <para>One assertion per module, each on the quantity that module's
    ///     rate arithmetic is MOST likely to get wrong.</para>
    /// </summary>
    [NativeAvailableTheory]
    [MemberData(nameof(Modules))]
    public void EveryDelayGearModule_ReDerivesItsRateOwnedState_At192k(string module)
    {
        switch (module)
        {
            case "multitap":
            {
                // The tap OFFSET, in milliseconds. The ring, the read position
                // and the glide coefficient all feed this one number.
                foreach (var rate in new[] { Rate48, Rate192 })
                {
                    var at = Frames;
                    var run = Render("multitap",
                        "taps=1;timeMs=20;spread=0;decay=0;pan=0;feedback=0;tone=1;mix=1",
                        Impulse(at), 8, rate: rate);
                    var expected = at + (int)MathF.Round(0.02f * rate);
                    var found = ArgPeak(run.L, expected - 300, 600);
                    Assert.True(Math.Abs(found - expected) <= 1,
                        $"multitap at {rate} Hz put its tap at {found}, expected {expected}");
                }
                break;
            }
            case "duckdelay":
            case "rhythmdelay":
            {
                // The echo time, in milliseconds — the delay line's own
                // rate-derived length and read position.
                foreach (var rate in new[] { Rate48, Rate192 })
                {
                    var at = Frames;
                    var setup = module == "duckdelay"
                        ? "timeMs=20;feedback=0;duckDb=0;tone=1;mix=1"
                        : "timeMs=20;feedback=0;gateDepth=0;mix=1";
                    var run = Render(module, setup, Impulse(at), 8, rate: rate);
                    var expected = at + (int)MathF.Round(0.02f * rate);
                    var found = ArgPeak(run.L, expected - 300, 600);
                    Assert.True(Math.Abs(found - expected) <= 1,
                        $"{module} at {rate} Hz put its echo at {found}, expected {expected}");
                }
                break;
            }
            case "bbddelay":
            {
                // The clock-tracking filter corner: a Biquad designed at 48 kHz
                // and reused at 192 kHz sits four times too high, so the same
                // setting would come back four times brighter.
                var tilts = new double[2];
                var rates = new[] { Rate48, Rate192 };
                for (var i = 0; i < 2; i++)
                {
                    var run = Render("bbddelay", "timeMs=100;feedback=0;modDepth=0;age=0;mix=1",
                        NoiseAt, 24, rate: rates[i]);
                    const int start = 20 * Frames;
                    var hi = BandEnergy(run.L, start, 4096, 6000, 12000, rates[i]);
                    var lo = BandEnergy(run.L, start, 4096, 200, 1000, rates[i]);
                    Assert.True(lo > 0, $"bbddelay at {rates[i]} Hz is silent");
                    tilts[i] = hi / lo;
                }
                var deltaDb = Math.Abs(10.0 * Math.Log10(tilts[1] / tilts[0]));
                Assert.True(deltaDb < 6.0,
                    $"bbddelay's spectral tilt moved {deltaDb:F1} dB between 48 kHz and 192 kHz — "
                    + "the clock-tracking filters were not re-derived from the rate");
                break;
            }
            case "granular":
            {
                // The grain LENGTH and the ring are both in samples; the pitch
                // ratio is not. If the grain geometry did not follow the rate,
                // the cloud at 192 kHz is a quarter as long per grain and the
                // pitch probe is the cleanest thing that still holds.
                foreach (var rate in new[] { Rate48, Rate192 })
                {
                    var run = Render("granular",
                        "grainMs=80;density=8;pitchSemitones=12;spray=0;feedback=0;mix=1",
                        Sine(200f, rate), 16, rate: rate);
                    var hz = DominantHz(run.L, 12 * Frames, 4096, 120, 600, rate);
                    Assert.True(Math.Abs(hz - 400) <= 5,
                        $"granular at {rate} Hz put the octave at {hz} Hz, not 400");
                }
                break;
            }
            default:
                Assert.Fail($"no rate probe for {module}");
                break;
        }
    }
}
