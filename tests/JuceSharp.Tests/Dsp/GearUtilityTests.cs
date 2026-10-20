using System;
using System.Globalization;
using System.Runtime.InteropServices;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// The three modules of the 2026-09 <c>utility</c> gear wave — <c>volume</c>,
/// <c>midside</c> and <c>looper</c> (<c>src/dsp/gear/UtilityGear.cpp</c>, cores
/// under <c>src/dsp/cores/</c>).
///
/// <para>Everything here renders OFFLINE on a stopped engine through
/// <c>dca_engine_dsp_process_buffer</c>, exactly like <see cref="GearDelayTests"/>
/// and <see cref="DspGainZipperTests"/>: that path is deterministic and the
/// stimulus is a pure function of the ABSOLUTE sample index, so two runs of the
/// same case are sample-aligned and a difference between them means
/// something.</para>
///
/// <para>A seam probe compared against the signal's OWN settled slope is
/// vacuous for a module that puts edges in the signal anyway; where that
/// applies, the reference is a CONTROL render with the parameter left
/// alone.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearUtilityTests : IDisposable
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
    public static TheoryData<string> Modules => new() { "volume", "midside", "looper" };

    private readonly JuceAudioEngine? _engine;
    private readonly ITestOutputHelper _out;

    public GearUtilityTests(ITestOutputHelper output)
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

    private sealed record Run(float[] L, float[] R);

    /// <summary>
    ///     A fresh single-node chain, then <paramref name="blocks"/> blocks of
    ///     the stimulus. <c>edits[b]</c> is applied BEFORE block b.
    ///     <paramref name="stimR"/> defaults to <paramref name="stimL"/>, so the
    ///     default input is dead centre; the M/S cases hand in an anti-phase or
    ///     one-sided right channel.
    ///     <paramref name="rateFrom"/>/<paramref name="rate2"/> switch the render
    ///     rate mid-run, which is what makes the engine RE-PREPARE the rig — the
    ///     only way to reach a core's device-change path from managed code.
    /// </summary>
    private Run Render(string module, string setup, Func<int, float> stimL, int blocks,
                       (int Block, string Spec)[]? edits = null, int rate = Rate48,
                       Func<int, float>? stimR = null, int rateFrom = int.MaxValue, int rate2 = Rate192)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        Apply(n, setup);
        stimR ??= stimL;

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

            for (var i = 0; i < Frames; i++)
            {
                bl[i] = stimL(b * Frames + i);
                br[i] = stimR(b * Frames + i);
            }
            Assert.Equal(Ok, DspProcess(H, bl, br, Frames, b >= rateFrom ? rate2 : rate));
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

    /// <summary>A steady sine at the absolute index — phase-continuous across blocks.</summary>
    private static Func<int, float> Sine(float hz, int rate = Rate48, float amp = 0.25f)
        => i => amp * MathF.Sin(2f * MathF.PI * hz * i / rate);

    /// <summary>A sine that only sounds inside [from, to) — everywhere else silence.</summary>
    private static Func<int, float> SineBetween(float hz, int from, int to, int rate = Rate48, float amp = 0.25f)
        => i => i >= from && i < to ? amp * MathF.Sin(2f * MathF.PI * hz * i / rate) : 0f;

    /// <summary>Constant DC: the cleanest way to read a gain straight off a sample.</summary>
    private static Func<int, float> Dc(float v) => _ => v;

    /// <summary>A linear 0 -&gt; 1 ramp across [from, from + length) — a signal whose
    /// direction of travel is visible in one subtraction.</summary>
    private static Func<int, float> Ramp(int from, int length)
        => i => i >= from && i < from + length ? (i - from) / (float)length : 0f;

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

    /// <summary>The SHIPPING taper of <c>dcdsp_volume.h</c>, mirrored here so the
    /// curve cases assert a formula rather than a golden number.</summary>
    private static float Taper(int curve, float p) => curve switch
    {
        1 => p * p,
        2 => MathF.Pow(p, 3.321928f),
        3 => p * p * (3f - 2f * p),
        _ => p,
    };

    // ======================================================================
    // volume
    // ======================================================================

    /// <summary>
    ///     A position jump WALKS to the new gain over <c>smoothMs</c>. The probe
    ///     is a DC input, so the output sample IS the gain: a 1 -&gt; 0 jump with
    ///     the heel at -90 dB must not reach -60 dB before 0.8x the ramp (a
    ///     switch, or a ramp derived from the wrong rate) and must have reached
    ///     it by 3x (a ramp that never lands).
    ///
    ///     <para>A MIDI-CC pedal sends 7-bit steps; unsmoothed, that is 128
    ///     zipper edges a sweep, and this is the only knob in the wave whose
    ///     whole job is to move continuously.</para>
    /// </summary>
    [NativeAvailableFact]
    public void Volume_APositionJump_WalksToTheNewGain_OverSmoothMs()
    {
        const int seamBlock = 6;
        const float ms = 100f;
        var run = Render("volume", "position=1;curve=0;minDb=-90;maxDb=0;smoothMs=100",
            Dc(0.5f), 20, [(seamBlock, "position=0")]);

        var seam = seamBlock * Frames;
        var expected = (int)(ms * 0.001f * Rate48);     // 4,800 samples

        // -60 dB of a 0.5 DC input is 5e-4.
        var landed = -1;
        for (var i = seam; i < run.L.Length; i++)
            if (MathF.Abs(run.L[i]) < 5e-4f) { landed = i - seam; break; }

        Assert.True(landed >= 0, "the pedal never reached -60 dB at all");
        Assert.True(landed >= (int)(0.8f * expected),
            $"the mute landed after {landed} samples, sooner than 0.8 x the {expected}-sample "
            + "ramp — the position was switched, not walked");
        Assert.True(landed <= 3 * expected,
            $"the mute took {landed} samples against a {expected}-sample ramp — it never landed");

        // ...and it is a RAMP, not a staircase: every step is at most one
        // ramp increment of the 0.5 input.
        var step = MaxStep(run.L, seam, 2 * expected);
        Assert.True(step < 4f * 0.5f / expected,
            $"the sweep stepped by {step:F6}, against a ramp increment of {0.5f / expected:F6}");
    }

    /// <summary>
    ///     Each taper puts the MID of its travel where its formula says. The
    ///     four are only distinguishable away from the ends — Linear and S-Curve
    ///     agree at exactly 0.5 — so each is probed at two positions.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(0, 0.5f)]
    [InlineData(0, 0.25f)]
    [InlineData(1, 0.5f)]
    [InlineData(1, 0.25f)]
    [InlineData(2, 0.5f)]
    [InlineData(2, 0.25f)]
    [InlineData(3, 0.5f)]
    [InlineData(3, 0.25f)]
    public void Volume_EachCurve_PutsItsMidTravelWhereItsFormulaSays(int curve, float position)
    {
        const float minDb = -60f;
        var pos = position.ToString(CultureInfo.InvariantCulture);
        var run = Render("volume",
            $"position={pos};curve={curve.ToString(CultureInfo.InvariantCulture)};"
            + "minDb=-60;maxDb=0;smoothMs=5", Dc(0.5f), 6);

        // Settled: well past the 5 ms ramp and the chain's own 10 ms add fade.
        var gain = Rms(run.L, 4 * Frames, Frames) / 0.5f;
        var db = 20f * MathF.Log10(MathF.Max(gain, 1e-9f));
        var expected = minDb + Taper(curve, position) * -minDb;

        Assert.True(MathF.Abs(db - expected) < 0.5f,
            $"curve {curve} at position {position} gave {db:F2} dB, the taper says {expected:F2} dB");
    }

    /// <summary>
    ///     …and what that MEANS at the shipping defaults, which the formula test
    ///     above cannot say. `t` is a position on the dB SWEEP, not a fraction
    ///     of the output voltage, so the "Audio" curve at half travel is not the
    ///     10 % of OUTPUT LEVEL the classic audio-pot definition means: at the
    ///     default heel of −90 dB it is −81.0 dB, i.e. 0.009 % of full scale.
    ///     The doc page's taper table says exactly this number, so something has
    ///     to hold it true.
    /// </summary>
    [NativeAvailableFact]
    public void Volume_TheAudioTaper_IsAPositionOnTheDbSweep_NotTenPercentOfLevel()
    {
        var run = Render("volume", "position=0.5;curve=2;minDb=-90;maxDb=0;smoothMs=5", Dc(0.5f), 6);
        var gain = Rms(run.L, 4 * Frames, Frames) / 0.5f;
        var db = 20f * MathF.Log10(MathF.Max(gain, 1e-9f));
        _out.WriteLine($"volume Audio taper, half travel, heel -90 dB: {db:F2} dB ({gain * 100f:F4} % of full scale)");

        Assert.True(MathF.Abs(db - (-81.0f)) < 0.5f,
            $"the Audio curve at half travel gave {db:F2} dB, not the -81.0 dB the doc table states");
        // …and the thing it is NOT: a tenth of the output level.
        Assert.True(gain < 0.01f,
            $"the Audio curve at half travel passed {gain * 100f:F2} % of the signal - if that is "
            + "ever ~10 %, the taper moved into the voltage domain and the doc table is wrong");
    }

    /// <summary>
    ///     Heel-down at the bottom of the <c>minDb</c> range is a MUTE: -90 dB
    ///     of a -6 dBFS input is under -95 dBFS, and the point of the parameter
    ///     is that a player can choose between that and a "gain pedal" floor.
    /// </summary>
    [NativeAvailableFact]
    public void Volume_HeelDownAtMinusNinety_IsSilent()
    {
        var muted = Render("volume", "position=0;curve=0;minDb=-90;maxDb=0;smoothMs=5", Dc(0.5f), 8);
        var gainPedal = Render("volume", "position=0;curve=0;minDb=-12;maxDb=0;smoothMs=5", Dc(0.5f), 8);

        var mutedDb = 20f * MathF.Log10(MathF.Max(Rms(muted.L, 6 * Frames, Frames), 1e-12f));
        Assert.True(mutedDb < -85f, $"heel-down at -90 dB left {mutedDb:F1} dBFS on the output");

        // Not vacuous: the SAME position at a -12 dB floor is plainly audible,
        // so "silent" is the setting talking and not a dead module.
        var floorDb = 20f * MathF.Log10(MathF.Max(Rms(gainPedal.L, 6 * Frames, Frames), 1e-12f));
        Assert.True(MathF.Abs(floorDb - (-6f - 12f)) < 0.5f,
            $"heel-down at a -12 dB floor gave {floorDb:F1} dBFS, expected -18");
    }

    // ======================================================================
    // midside
    // ======================================================================

    /// <summary>
    ///     <c>width</c> 0 is a MONO FOLD, and it is exact: the side is
    ///     multiplied by a smoother that LANDS on zero, so L and R are the same
    ///     float. "Almost equal" would hide a width that merely got small.
    /// </summary>
    [NativeAvailableFact]
    public void MidSide_WidthZero_MakesTheTwoChannelsBitIdentical()
    {
        // A genuinely wide input: the right channel is a different tone.
        var run = Render("midside", "midDb=0;sideDb=0;width=0;monoBelowHz=0", Sine(187.5f), 8,
            stimR: Sine(375f));

        // Settled: past the 15 ms width ramp and the chain's 10 ms add fade.
        for (var i = 4 * Frames; i < 8 * Frames; i++)
            Assert.True(run.L[i] == run.R[i],
                $"width 0 left the channels different at sample {i}: {run.L[i]} vs {run.R[i]}");

        // Not vacuous: at width 1 the same input is plainly NOT mono.
        var wide = Render("midside", "midDb=0;sideDb=0;width=1;monoBelowHz=0", Sine(187.5f), 8,
            stimR: Sine(375f));
        Assert.True(RmsDiff(wide.L, wide.R, 4 * Frames, Frames) > 0.05f,
            "the probe input is already mono — the case proves nothing");
    }

    /// <summary>
    ///     Width cannot INVENT a side. At width 2 a centred input has no side
    ///     content to double, so the output is still dead centre — the property
    ///     that makes this module safe to leave on a mono guitar rig.
    /// </summary>
    [NativeAvailableFact]
    public void MidSide_WidthTwo_OnAMonoInput_IsStillMono()
    {
        var run = Render("midside", "midDb=0;sideDb=0;width=2;monoBelowHz=0", Sine(187.5f), 8);
        for (var i = 4 * Frames; i < 8 * Frames; i++)
            Assert.True(run.L[i] == run.R[i],
                $"width 2 invented a side at sample {i}: {run.L[i]} vs {run.R[i]}");
        Assert.True(Rms(run.L, 4 * Frames, Frames) > 0.05f, "the probe render is silent");
    }

    /// <summary>
    ///     <c>midPhase</c> Inverted negates a centred input EXACTLY — the
    ///     polarity is a smoothed multiply that lands on -1, not an
    ///     approximation of one — and it ramps there rather than stepping.
    /// </summary>
    [NativeAvailableFact]
    public void MidSide_MidPolarityInverted_NegatesACentredInput_Exactly()
    {
        // 200 Hz, NOT the 187.5 the other cases use: 187.5 is a whole number of
        // cycles per 2,048-frame block, so an edit applied at a block boundary
        // lands exactly on a ZERO CROSSING and a hard flip produces no step at
        // all. Measured — with the crossfade removed the 187.5 version of this
        // case still passed.
        var stim = Sine(200f);
        var run = Render("midside", "midDb=0;sideDb=0;width=1;midPhase=1;monoBelowHz=0", stim, 8);

        for (var i = 4 * Frames; i < 8 * Frames; i++)
            Assert.Equal(-stim(i), run.L[i], 6);

        // ...and the flip is a CROSSFADE. The control is the same render with
        // the polarity left alone: measuring against this signal's own slope
        // would pass for a module that wrote the flip straight out, because a
        // sine's slope is not small.
        var seam = Render("midside", "midDb=0;sideDb=0;width=1;midPhase=0;monoBelowHz=0", stim, 8,
            [(4, "midPhase=1")]);
        var control = Render("midside", "midDb=0;sideDb=0;width=1;midPhase=0;monoBelowHz=0", stim, 8);
        var natural = MaxStep(control.L, 4 * Frames - 1, 2 * Frames);
        var flip = MaxStep(seam.L, 4 * Frames - 1, 2 * Frames);
        Assert.True(natural > 1e-4f, "the probe signal is silent");
        Assert.True(flip < 2f * natural,
            $"the polarity flip stepped the output by {flip:F5} against {natural:F5} for the same "
            + "render with the switch left alone — it was written, not crossfaded");
    }

    /// <summary>
    ///     <c>sideDb</c> is a real level on the side path: -24 dB on an
    ///     anti-phase input (which is ALL side and no mid) attenuates by 24 dB.
    /// </summary>
    [NativeAvailableFact]
    public void MidSide_SideLevel_AttenuatesAPureSideSignal_ByWhatItSays()
    {
        var l = Sine(187.5f);
        Func<int, float> r = i => -l(i);
        var flat = Render("midside", "midDb=0;sideDb=0;width=1;monoBelowHz=0", l, 8, stimR: r);
        var cut = Render("midside", "midDb=0;sideDb=-24;width=1;monoBelowHz=0", l, 8, stimR: r);

        var flatRms = Rms(flat.L, 6 * Frames, Frames);
        var cutRms = Rms(cut.L, 6 * Frames, Frames);
        Assert.True(flatRms > 0.05f, $"the anti-phase probe is silent ({flatRms:F5})");
        var db = 20f * MathF.Log10(cutRms / flatRms);
        Assert.True(MathF.Abs(db + 24f) < 0.5f,
            $"sideDb -24 pulled the side down by {-db:F2} dB, not 24");
    }

    /// <summary>
    ///     <c>monoBelowHz</c> folds the LOW end to the centre and leaves the top
    ///     alone. 120 Hz must take more than 12 dB off a 60 Hz side signal and
    ///     less than 1 dB off a 1 kHz one — a filter that reached the MID path,
    ///     or an unbounded high-pass sitting where a shelf belongs, fails one
    ///     side or the other.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(60f, true)]
    [InlineData(1000f, false)]
    public void MidSide_MonoBelow_RemovesTheLowSide_AndLeavesTheTopAlone(float hz, bool shouldCut)
    {
        Func<int, float> l = Sine(hz);
        Func<int, float> r = i => -l(i);
        var off = Render("midside", "midDb=0;sideDb=0;width=1;monoBelowHz=0", l, 16, stimR: r);
        var on = Render("midside", "midDb=0;sideDb=0;width=1;monoBelowHz=120", l, 16, stimR: r);

        // Four blocks: 60 Hz needs a window of whole cycles, or the RMS of the
        // shelf's phase-shifted output is compared against a different slice of
        // the same wave.
        var offRms = Rms(off.L, 10 * Frames, 4 * Frames);
        var onRms = Rms(on.L, 10 * Frames, 4 * Frames);
        Assert.True(offRms > 0.05f, $"the {hz} Hz probe is silent ({offRms:F5})");
        var db = 20f * MathF.Log10(MathF.Max(onRms, 1e-9f) / offRms);

        if (shouldCut)
            Assert.True(db < -12f, $"at {hz} Hz the side only lost {-db:F1} dB — the shelf is not folding");
        else
            Assert.True(MathF.Abs(db) < 1f, $"at {hz} Hz the side moved {db:F2} dB — the shelf reaches too high");
    }

    /// <summary>
    ///     <c>swap</c> exchanges the sides, and it CROSSFADES: flipping a live
    ///     stereo image in one sample is two full-scale steps, the loudest click
    ///     a utility can make.
    /// </summary>
    [NativeAvailableFact]
    public void MidSide_Swap_ExchangesTheSides_ByCrossfading()
    {
        // 200 Hz for the same reason as the polarity case: a block boundary is
        // a zero crossing at 187.5 Hz, where no switch can produce a step.
        Func<int, float> l = Sine(200f);
        Func<int, float> r = _ => 0f;   // everything on the left
        var swapped = Render("midside", "midDb=0;sideDb=0;width=1;swap=1;monoBelowHz=0", l, 8, stimR: r);

        Assert.True(Rms(swapped.R, 6 * Frames, Frames) > 0.05f, "the swap did not move the signal right");
        Assert.True(Rms(swapped.L, 6 * Frames, Frames) < 0.1f * Rms(swapped.R, 6 * Frames, Frames),
            "the swapped channel is still carrying the signal");

        var seam = Render("midside", "midDb=0;sideDb=0;width=1;swap=0;monoBelowHz=0", l, 8,
            [(4, "swap=1")], stimR: r);
        var control = Render("midside", "midDb=0;sideDb=0;width=1;swap=0;monoBelowHz=0", l, 8, stimR: r);
        var natural = MaxStep(control.L, 4 * Frames - 1, 2 * Frames);
        var step = MaxStep(seam.L, 4 * Frames - 1, 2 * Frames);
        Assert.True(step < 2f * natural,
            $"the swap stepped the output by {step:F5} against {natural:F5} with the switch left alone");
    }

    // ======================================================================
    // looper
    // ======================================================================

    // One loop = 24 blocks = 49,152 frames = 1.024 s at 48 kHz. 187.5 Hz is
    // EXACTLY 192 cycles in that, so the lap boundary is waveform-continuous and
    // a step measured near it is the module's and not the loop's.
    private const int RecFrom = 1;
    private const int RecTo = 25;
    private const int LoopLen = (RecTo - RecFrom) * Frames;

    /// <summary>
    ///     THE LOOPER IDENTITY: record a phrase, hit Play with nothing going in,
    ///     and the phrase comes back — at its own pitch, on the beat, every
    ///     <c>LoopLen</c> samples. The probe records a BURST so the repeat is a
    ///     position and not a correlation guess; the spacing must be the
    ///     recorded length to the sample.
    /// </summary>
    [NativeAvailableFact]
    public void Looper_RecordThenPlay_RepeatsThePhrase_AtTheRecordedLength()
    {
        var start = RecFrom * Frames;
        var run = Render("looper", "maxLoopS=30;loopLevel=1;mix=1",
            SineBetween(220f, start, start + 4800), 100,
            [(RecFrom, "state=1"), (RecTo, "state=2")]);

        var play = RecTo * Frames;
        var first = ArgPeak(run.L, play, LoopLen);
        var second = ArgPeak(run.L, play + LoopLen, LoopLen);
        var third = ArgPeak(run.L, play + 2 * LoopLen, LoopLen);

        Assert.True(MathF.Abs(run.L[first]) > 0.1f, $"the loop is inaudible (peak {run.L[first]:F4})");
        Assert.True(Math.Abs(second - first - LoopLen) <= 1,
            $"the second lap came {second - first} samples after the first, not {LoopLen}");
        Assert.True(Math.Abs(third - second - LoopLen) <= 1,
            $"the third lap came {third - second} samples after the second, not {LoopLen}");

        // ...and what repeats is the TONE that was played, not a click.
        var hz = DominantHz(run.L, first, 4096, 150, 320, Rate48);
        Assert.True(Math.Abs(hz - 220) <= 2, $"the loop plays back at {hz} Hz, not 220");
    }

    /// <summary>
    ///     Overdub LAYERS: a 440 Hz pass over a 220 Hz loop leaves both in the
    ///     loop, and Undo takes the layer back off. Undo is a std::swap of two
    ///     buffers whose contents were copied INCREMENTALLY while the overdub
    ///     ran — if that copy were a memcpy in the callback that saw the
    ///     footswitch it would still pass here, which is why the RT gate carries
    ///     the other half.
    /// </summary>
    [NativeAvailableFact]
    public void Looper_Overdub_LayersASecondPass_AndUndoTakesItBack()
    {
        const int odFrom = 27, odTo = 51;
        var recStart = RecFrom * Frames;
        Func<int, float> stim = i =>
            i >= recStart && i < RecTo * Frames ? 0.25f * MathF.Sin(2f * MathF.PI * 220f * i / Rate48)
            : i >= odFrom * Frames && i < odTo * Frames ? 0.25f * MathF.Sin(2f * MathF.PI * 440f * i / Rate48)
            : 0f;

        var run = Render("looper", "maxLoopS=30;loopLevel=1;mix=1;overdubDecay=1", stim, 80,
            [(RecFrom, "state=1"), (RecTo, "state=2"), (odFrom, "state=3"), (odTo, "state=2"),
             (62, "undo=1")]);

        // After the overdub, with silence going in: both tones are in the loop.
        const int layered = 55 * Frames;
        var l220 = Goertzel(run.L, layered, 8192, 220, Rate48);
        var l440 = Goertzel(run.L, layered, 8192, 440, Rate48);
        Assert.True(l220 > 1.0, $"the original 220 Hz pass is gone after the overdub ({l220:F1})");
        Assert.True(l440 > 0.5 * l220,
            $"the 440 Hz overdub is not in the loop (440 {l440:F1} vs 220 {l220:F1})");

        // After the undo: the 220 survives, the 440 does not.
        const int undone = 70 * Frames;
        var u220 = Goertzel(run.L, undone, 8192, 220, Rate48);
        var u440 = Goertzel(run.L, undone, 8192, 440, Rate48);
        Assert.True(u220 > 0.5 * l220,
            $"undo took the ORIGINAL pass with it (220 Hz {u220:F1}, was {l220:F1})");
        Assert.True(u440 < 0.1 * l440,
            $"undo left the overdub behind (440 Hz {u440:F1}, was {l440:F1})");
    }

    /// <summary>
    ///     <c>reverse</c> reads the loop backwards. Recorded as a rising ramp,
    ///     the playback slope is positive forwards and negative reversed, and
    ///     its magnitude is the same — the read step did not change, only its
    ///     sign.
    /// </summary>
    [NativeAvailableFact]
    public void Looper_Reverse_PlaysTheRecordedRampBackwards()
    {
        const int recTo = 3;                       // a short loop: 2 blocks
        const int len = (recTo - RecFrom) * Frames;
        var start = RecFrom * Frames;

        var fwd = Render("looper", "maxLoopS=30;loopLevel=1;mix=1", Ramp(start, len), 30,
            [(RecFrom, "state=1"), (recTo, "state=2")]);
        var rev = Render("looper", "maxLoopS=30;loopLevel=1;mix=1", Ramp(start, len), 30,
            [(RecFrom, "state=1"), (recTo, "state=2"), (20, "reverse=1")]);

        // The mean per-sample slope over a whole lap, ignoring the one wrap.
        static float Slope(float[] x, int start, int count)
        {
            double acc = 0; var n = 0;
            for (var i = start + 1; i < start + count; i++)
            {
                var d = x[i] - x[i - 1];
                if (MathF.Abs(d) > 0.5f) continue;   // the lap wrap
                acc += d; n++;
            }
            return n == 0 ? 0f : (float)(acc / n);
        }

        var expected = 1f / len;
        var forward = Slope(fwd.L, 25 * Frames, Frames);
        var backward = Slope(rev.L, 25 * Frames, Frames);

        Assert.True(MathF.Abs(forward - expected) < 0.25f * expected,
            $"the forward ramp climbs at {forward:E3} per sample, expected {expected:E3}");
        Assert.True(MathF.Abs(backward + expected) < 0.25f * expected,
            $"reverse plays at {backward:E3} per sample, expected {-expected:E3} — it is not "
            + "reading the loop backwards");
    }

    /// <summary>
    ///     <c>halfSpeed</c> reads at half the rate with linear interpolation, so
    ///     the loop comes back an octave down. A read increment that stayed at
    ///     1.0 leaves the pitch where it was and looks entirely healthy.
    /// </summary>
    [NativeAvailableFact]
    public void Looper_HalfSpeed_DropsTheLoopAnOctave()
    {
        // 468.75 Hz is EXACTLY 480 cycles in the 49,152-frame loop, so the lap
        // wraps without a step and the only thing in the spectrum is the tone.
        const float hz = 468.75f;
        var start = RecFrom * Frames;
        var full = Render("looper", "maxLoopS=30;loopLevel=1;mix=1",
            SineBetween(hz, start, RecTo * Frames), 60, [(RecFrom, "state=1"), (RecTo, "state=2")]);
        var half = Render("looper", "maxLoopS=30;loopLevel=1;mix=1",
            SineBetween(hz, start, RecTo * Frames), 60,
            [(RecFrom, "state=1"), (RecTo, "state=2"), (30, "halfSpeed=1")]);

        const int at = 40 * Frames;
        var fullHz = DominantHz(full.L, at, 8192, 150, 700, Rate48);
        var halfHz = DominantHz(half.L, at, 8192, 150, 700, Rate48);
        Assert.True(Math.Abs(fullHz - hz) <= 2, $"the loop plays at {fullHz} Hz, not {hz}");
        Assert.True(Math.Abs(halfHz - hz / 2f) <= 2,
            $"half speed plays at {halfHz} Hz, not {hz / 2f}");
    }

    /// <summary>
    ///     A transport change CROSSFADES the loop's own output over 10 ms. The
    ///     control is the same render that never leaves Record, whose output is
    ///     the dry input alone: the loop switched in at full level would be a
    ///     step of most of its amplitude.
    /// </summary>
    [NativeAvailableFact]
    public void Looper_TheTransport_CrossfadesTheLoopIn_InsteadOfSplicing()
    {
        // Three details, and every one of them is what makes this probe
        // discriminating rather than decorative — each was measured passing with
        // the crossfade REMOVED before it was fixed:
        //  - a COSINE, so the loop's first sample is its PEAK. A sine's is zero,
        //    and switching in a zero is not a step.
        //  - 187.5 Hz, which is 192 whole cycles in the 49,152-frame loop, so
        //    the lap wrap is continuous and cannot become the reference slope.
        //  - the probed transition is STOP -> PLAY, out of silence. Record ->
        //    Play hands the loop over from a dry signal it is phase-identical
        //    to, so even a hard switch-in produces no discontinuity there.
        const int stopAt = 26, playAt = 30;
        var recStart = RecFrom * Frames;
        Func<int, float> stim = i => i >= recStart && i < RecTo * Frames
            ? 0.25f * MathF.Cos(2f * MathF.PI * 187.5f * i / Rate48)
            : 0f;
        var run = Render("looper", "maxLoopS=30;loopLevel=1;mix=1", stim, 45,
            [(RecFrom, "state=1"), (RecTo, "state=2"), (stopAt, "state=0"), (playAt, "state=2")]);

        var natural = MaxStep(run.L, 40 * Frames, Frames);   // settled playback
        Assert.True(natural > 1e-4f, "the loop playback is silent");
        // Not vacuous in either direction: it is silent while stopped and
        // audible once playing, so there IS a switch to smooth.
        Assert.True(Rms(run.L, 29 * Frames, Frames) < 1e-3f,
            "the transport did not stop — nothing is being switched back in");
        Assert.True(Rms(run.L, 40 * Frames, Frames) > 0.05f,
            "nothing is playing after the switch — the seam probe measures nothing");

        var seam = MaxStep(run.L, playAt * Frames - 1, 1440);   // 30 ms around it
        Assert.True(seam < 3f * natural,
            $"Stop -> Play stepped the output by {seam:F5} against a settled slope of "
            + $"{natural:F5} — the loop was switched in rather than faded in");
    }

    /// <summary>
    ///     <c>mix</c> WALKS. Three renders of the same recorded loop: mix left
    ///     at 0, mix at 1 throughout, and mix moved at the seam — 128 samples in,
    ///     the seam is still near the OLD setting (the
    ///     <see cref="DspGainZipperTests"/> MixCases shape).
    /// </summary>
    [NativeAvailableFact]
    public void Looper_Mix_RampsInsteadOfSwitching()
    {
        const int seamBlock = 30;
        var stim = SineBetween(187.5f, RecFrom * Frames, RecTo * Frames);
        (int, string)[] transport = [(RecFrom, "state=1"), (RecTo, "state=2")];

        var refBefore = Render("looper", "maxLoopS=30;loopLevel=1;mix=0", stim, seamBlock + 1, transport);
        var refAfter = Render("looper", "maxLoopS=30;loopLevel=1;mix=1", stim, seamBlock + 1, transport);
        var seam = Render("looper", "maxLoopS=30;loopLevel=1;mix=0", stim, seamBlock + 1,
            [.. transport, (seamBlock, "mix=1")]);

        var b0 = seamBlock * Frames;
        const int earlyWindow = 128;
        const int lateStart = 1200;
        var earlySpan = RmsDiff(refBefore.L, refAfter.L, b0, earlyWindow);
        var lateSpan = RmsDiff(refBefore.L, refAfter.L, b0 + lateStart, Frames - lateStart);
        Assert.True(earlySpan > 0.005f, $"the two mixes are indistinguishable at the seam ({earlySpan:F5})");
        Assert.True(lateSpan > 0.005f, $"the two mixes are indistinguishable later ({lateSpan:F5})");

        var earlyRatio = RmsDiff(seam.L, refBefore.L, b0, earlyWindow) / earlySpan;
        Assert.True(earlyRatio < 0.5f,
            $"the first {earlyWindow} samples after the change are {earlyRatio * 100f:F0} % of the "
            + "way to the new mix — a ramp is well under half, a hard switch is 100 %");
        var lateRatio = RmsDiff(seam.L, refAfter.L, b0 + lateStart, Frames - lateStart) / lateSpan;
        Assert.True(lateRatio < 0.15f,
            $"{Frames - lateStart} samples later the seam is still {lateRatio * 100f:F0} % away — "
            + "the ramp never landed");
    }

    /// <summary>
    ///     The recorded loop is USER CONTENT and survives a device change. The
    ///     render rate moves under a playing loop, which re-prepares the whole
    ///     rig: the take must still be audible afterwards, and — the documented
    ///     half of the trade — it plays at the NEW rate, so a loop recorded at
    ///     48 kHz comes back four times up at 192 kHz rather than being
    ///     resampled on the device-change thread.
    /// </summary>
    [NativeAvailableFact]
    public void Looper_ARePrepareAtANewRate_KeepsTheTake_AndPlaysItAtTheNewRate()
    {
        const int switchBlock = 30;
        var stim = SineBetween(187.5f, RecFrom * Frames, RecTo * Frames);
        var run = Render("looper", "maxLoopS=30;loopLevel=1;mix=1", stim, 45,
            [(RecFrom, "state=1"), (RecTo, "state=2")], rateFrom: switchBlock);

        var before = Rms(run.L, 28 * Frames, Frames);
        var after = Rms(run.L, 40 * Frames, Frames);
        Assert.True(before > 0.05f, $"the loop was not playing before the rate change ({before:F5})");
        Assert.True(after > 0.5f * before,
            $"the loop went quiet across the re-prepare (RMS {before:F4} -> {after:F4}) — prepare() "
            + "cleared user content");

        // 187.5 Hz of content read at 4x the rate is 750 Hz. This is the
        // DOCUMENTED consequence of keeping the samples instead of resampling
        // 23 MB on a device change; it is asserted so nobody "fixes" it silently.
        var hz = DominantHz(run.L, 40 * Frames, 8192, 400, 1200, Rate192);
        Assert.True(Math.Abs(hz - 750) <= 8,
            $"after the re-prepare the loop plays at {hz} Hz, not the 750 the rate ratio gives");
    }

    // ======================================================================
    // Cross-module: ramps, clamps, read-back, rate derivation
    // ======================================================================

    /// <summary>
    ///     The pedal's position and the M/S box's mid level WALK across a change
    ///     instead of switching (the <see cref="DspGainZipperTests"/> MixCases
    ///     shape; the looper's own <c>mix</c> has its own case above because it
    ///     needs a recorded loop first).
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("volume", "curve=0;minDb=-90;maxDb=0;smoothMs=20", "position", 0f, 1f)]
    [InlineData("midside", "sideDb=0;width=1;monoBelowHz=0", "midDb", -24f, 0f)]
    public void EveryUtilityGearLevel_Ramps_InsteadOfSwitching(
        string module, string setup, string param, float from, float to)
    {
        const int seamBlock = 6;
        const int earlyWindow = 128;
        const int lateStart = 1200;
        var f = from.ToString(CultureInfo.InvariantCulture);
        var t = to.ToString(CultureInfo.InvariantCulture);

        var stim = Sine(187.5f);
        var refBefore = Render(module, $"{setup};{param}={f}", stim, seamBlock + 1);
        var refAfter = Render(module, $"{setup};{param}={t}", stim, seamBlock + 1);
        var seam = Render(module, $"{setup};{param}={f}", stim, seamBlock + 1,
            [(seamBlock, $"{param}={t}")]);

        var b0 = seamBlock * Frames;
        var earlySpan = RmsDiff(refBefore.L, refAfter.L, b0, earlyWindow);
        var lateSpan = RmsDiff(refBefore.L, refAfter.L, b0 + lateStart, Frames - lateStart);
        Assert.True(earlySpan > 0.005f,
            $"{module}.{param}: the two settings are indistinguishable at the seam ({earlySpan:F5})");
        Assert.True(lateSpan > 0.005f,
            $"{module}.{param}: the two settings are indistinguishable later ({lateSpan:F5})");

        var earlyRatio = RmsDiff(seam.L, refBefore.L, b0, earlyWindow) / earlySpan;
        Assert.True(earlyRatio < 0.5f,
            $"{module}.{param}: the first {earlyWindow} samples after the change are "
            + $"{earlyRatio * 100f:F0} % of the way to the new setting — a ramp is well under half, "
            + "a hard switch is 100 %");
        var lateRatio = RmsDiff(seam.L, refAfter.L, b0 + lateStart, Frames - lateStart) / lateSpan;
        Assert.True(lateRatio < 0.15f,
            $"{module}.{param}: {Frames - lateStart} samples later the seam is still "
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
    public void EveryUtilityGearParam_ClampsToItsRange_AndRefusesNaN(string module)
    {
        var info = JuceDspModuleCatalog.Find(module);
        Assert.NotNull(info);
        Assert.Equal("utility", info!.Category);

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

        Assert.True(probed >= 10, $"{module}: only {probed} params probed — the sweep is vacuous");
    }

    /// <summary>
    ///     getParam reads back what setParam stored — with ONE deliberate
    ///     exception. The looper's <c>undo</c> is a momentary TRIGGER: writing
    ///     Undo performs it and the param reads 0 again, because a latch that
    ///     stayed at 1 could never be pressed twice. A UI that renders it as a
    ///     toggle is wrong about what it is.
    /// </summary>
    /// <summary>
    ///     Silence in, silence out — across a DISENGAGE while the side channel
    ///     is loud, and a re-engage later.
    ///
    ///     <para>While <c>monoBelowHz</c> is off the side shelf is not clocked
    ///     at all, so its biquad state freezes at whatever it held when the
    ///     crossfade last reached zero. Re-engaging resumed the filter from that
    ///     — seconds-old audio released into a silent passage, INSIDE the faded
    ///     signal where an amplitude crossfade cannot hide it. The core gets the
    ///     other half of this class right (<c>shelfHz_ = -1</c> is forced in
    ///     <c>prepare()</c>, with a comment naming the widener bug), which is
    ///     what makes the omission worth closing.</para>
    /// </summary>
    [NativeAvailableFact]
    public void MidSide_ReEngagingTheSideShelf_DoesNotReleaseTheStateItFrozeWith()
    {
        // Left-only, so the SIDE channel carries the whole signal and the shelf
        // is in the path of something loud.
        var burst = SineBetween(60f, 1 * Frames, 10 * Frames, amp: 0.9f);

        var run = Render("midside", "monoBelowHz=200;sideDb=0;width=1", burst, 30,
            [(5, "monoBelowHz=0"),      // OFF while the burst is still loud
             (20, "monoBelowHz=200")],  // …and back on, into silence
            stimR: _ => 0f);

        // Not vacuous: the shelf really was in the path of something.
        var loud = Rms(run.L, 2 * Frames, 2 * Frames);
        Assert.True(loud > 0.05f, $"the burst never reached the module (RMS {loud:E3})");

        var quiet = Rms(run.L, 11 * Frames, 8 * Frames);
        Assert.True(quiet < 1e-6f,
            $"the module was not silent before the re-engage (RMS {quiet:E3}) — the probe is vacuous");

        var released = Rms(run.L, 20 * Frames, 8 * Frames);
        _out.WriteLine($"midside re-engage: burst {loud:E3}, before {quiet:E3}, after {released:E3}");
        Assert.True(released < 1e-6f,
            $"re-engaging the side shelf released {released:E3} of frozen state into a silent passage");
    }

    /// <summary>
    ///     A record pass that fills <c>maxLoopS</c> closes ITSELF — the engine
    ///     moves the transport to Play on the audio thread with no param write
    ///     behind it. Three things follow from that and none of them worked:
    ///     the loop has to start playing at the cap, <c>getParam("state")</c>
    ///     has to say Play (it reported the last value WRITTEN, so a footswitch
    ///     UI showed "Recording" forever for a loop that was already playing),
    ///     and the next Record press has to start a new take (it compared equal
    ///     to the stale request and did nothing — the player had to go
    ///     Stop → Record by hand).
    /// </summary>
    [NativeAvailableFact]
    public void Looper_AFullRecordPass_AutoPlays_ReportsIt_AndTheNextRecordStartsANewTake()
    {
        const int cap = Rate48;                     // maxLoopS = 1
        const int burst = 4800;

        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, "looper");
        Assert.True(n >= 0);
        Apply(n, "maxLoopS=1;loopLevel=1;mix=1");

        // Take one: Record at block 1, a burst right at the top of it, and then
        // NOTHING — no Play press at all.
        var firstStart = 1 * Frames;
        var takeOne = RunLooper(n, 100, SineBetween(220f, firstStart, firstStart + burst),
            [(1, "state=1")]);

        // The first lap starts where the recording closed: firstStart + cap.
        var lapOne = firstStart + cap;
        var a = ArgPeak(takeOne.L, lapOne, cap);
        var b = ArgPeak(takeOne.L, lapOne + cap, cap);
        Assert.True(MathF.Abs(takeOne.L[a]) > 0.05f,
            $"the auto-closed loop is inaudible (peak {takeOne.L[a]:F4})");
        Assert.True(Math.Abs(b - a - cap) <= 1,
            $"the auto-closed loop repeats every {b - a} samples, not {cap}");

        // …and the getter says so. It reported the last value WRITTEN (Record)
        // before 2026-09-12, which is what left a footswitch UI showing
        // "Recording" forever for a loop that was already playing.
        Assert.Equal(Ok, DspGetParam(H, n, "state", out var reported));
        Assert.Equal(2f, reported, 3);

        // Take two: press Record AGAIN, with the burst somewhere else entirely.
        var secondStart = 2 * Frames + 900;
        var takeTwo = RunLooper(n, 100, SineBetween(330f, secondStart, secondStart + burst),
            [(1, "state=1")]);
        var lapTwo = secondStart + cap;
        var c = ArgPeak(takeTwo.L, lapTwo, cap);
        Assert.True(MathF.Abs(takeTwo.L[c]) > 0.05f,
            "the second Record press produced no new take — it compared equal to the stale request");
        Assert.True(Math.Abs(c - secondStart - cap) < 2000,
            $"the second take's peak is {c - secondStart - cap} samples from where the NEW burst "
            + "was recorded — the first take is still playing");
    }

    /// <summary>
    ///     <c>maxLoopS</c> is a LIVE knob. Turning it down below what has
    ///     already been recorded used to WEDGE the looper: the record branch was
    ///     never entered again, the auto-close lived inside it so it never
    ///     fired, <c>loopLen_</c> stayed 0 and the module neither recorded nor
    ///     played until the player changed <c>state</c> by hand. The take so far
    ///     was still in the buffer and unreachable.
    /// </summary>
    [NativeAvailableFact]
    public void Looper_LoweringMaxLoopSMidRecord_ClosesTheLoop_InsteadOfWedging()
    {
        const int burst = 440;      // two cycles: a sharp, unambiguous argmax
        var start = 1 * Frames;

        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, "looper");
        Assert.True(n >= 0);
        Apply(n, "maxLoopS=30;loopLevel=1;mix=1");

        // Record from block 1; at block 25 (49,152 frames in) drop the cap to
        // one second — BELOW what has already been recorded.
        var run = RunLooper(n, 120, SineBetween(220f, start, start + burst),
            [(1, "state=1"), (25, "maxLoopS=1")]);
        _out.WriteLine($"looper wedge probe: burst {burst}, cap {Rate48}");

        var from = 26 * Frames;
        var heard = Rms(run.L, from, 40 * Frames);
        Assert.True(heard > 1e-3,
            $"the looper went silent after the cap was lowered mid-pass (RMS {heard:E3}) — it wedged");

        // …and it plays at the NEW cap, not at the length that had been recorded.
        var a = ArgPeak(run.L, from, Rate48);
        // Centred on where the next lap SHOULD put it, so the search cannot
        // land on the same lap or the one after.
        var b = ArgPeak(run.L, a + 3 * Rate48 / 4, Rate48 / 2);
        _out.WriteLine($"looper: first peak {a}, next {b}, spacing {b - a}");
        Assert.True(Math.Abs(b - a - Rate48) <= 1,
            $"the closed loop repeats every {b - a} samples, not the {Rate48} the knob now asks for");

        Assert.Equal(Ok, DspGetParam(H, n, "state", out var reported));
        Assert.Equal(2f, reported, 3);
    }

    /// <see cref="Render"/> against an EXISTING node, so a test can read the
    /// transport back and press the footswitch again afterwards.
    private Run RunLooper(int node, int blocks, Func<int, float> stim, (int Block, string Spec)[] edits)
    {
        var l = new float[blocks * Frames];
        var r = new float[blocks * Frames];
        var bl = new float[Frames];
        var br = new float[Frames];
        for (var b = 0; b < blocks; b++)
        {
            foreach (var e in edits)
                if (e.Block == b)
                    Apply(node, e.Spec);
            for (var i = 0; i < Frames; i++) bl[i] = br[i] = stim(b * Frames + i);
            Assert.Equal(Ok, DspProcess(H, bl, br, Frames, Rate48));
            Array.Copy(bl, 0, l, b * Frames, Frames);
            Array.Copy(br, 0, r, b * Frames, Frames);
        }
        return new Run(l, r);
    }

    [NativeAvailableFact]
    public void Looper_Undo_IsATrigger_AndReadsBackZero()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, "looper");
        Assert.True(n >= 0);

        Assert.Equal(Ok, DspSetParam(H, n, "state", 2f));
        Assert.Equal(Ok, DspGetParam(H, n, "state", out var state));
        Assert.Equal(2f, state, 3);

        Assert.Equal(Ok, DspSetParam(H, n, "undo", 1f));
        Assert.Equal(Ok, DspGetParam(H, n, "undo", out var undo));
        Assert.Equal(0f, undo, 6);

        // ...and a second press is still accepted, which is the whole reason the
        // value crossing the mailbox is a sequence rather than the 1 itself.
        Assert.Equal(Ok, DspSetParam(H, n, "undo", 1f));
        Assert.Equal(Ok, DspGetParam(H, n, "undo", out undo));
        Assert.Equal(0f, undo, 6);
    }

    /// <summary>
    ///     Re-preparing at 192 kHz re-derives everything the RATE owns. One
    ///     assertion per module, each on the quantity that module's rate
    ///     arithmetic is most likely to get wrong: the pedal's ramp LENGTH in
    ///     milliseconds, the M/S shelf's corner in Hz, and the looper's ring
    ///     capacity in seconds.
    /// </summary>
    [NativeAvailableTheory]
    [MemberData(nameof(Modules))]
    public void EveryUtilityGearModule_ReDerivesItsRateOwnedState_At192k(string module)
    {
        switch (module)
        {
            case "volume":
            {
                // 100 ms of ramp is 4,800 samples at 48 kHz and 19,200 at
                // 192 kHz. A ramp length computed once and cached lands four
                // times early at the higher rate.
                foreach (var rate in new[] { Rate48, Rate192 })
                {
                    const int seamBlock = 6;
                    var run = Render("volume", "position=1;curve=0;minDb=-90;maxDb=0;smoothMs=100",
                        Dc(0.5f), 40, [(seamBlock, "position=0")], rate: rate);
                    var seam = seamBlock * Frames;
                    var expected = (int)(0.1f * rate);
                    var landed = -1;
                    for (var i = seam; i < run.L.Length; i++)
                        if (MathF.Abs(run.L[i]) < 5e-4f) { landed = i - seam; break; }
                    Assert.True(landed >= (int)(0.8f * expected) && landed <= 3 * expected,
                        $"volume at {rate} Hz muted after {landed} samples, expected ~{expected}");
                }
                break;
            }
            case "midside":
            {
                // The shelf corner in Hz. A Biquad designed at 48 kHz and
                // reused at 192 kHz sits four times too high, which would fold
                // 1 kHz to the centre as well as 60.
                foreach (var rate in new[] { Rate48, Rate192 })
                {
                    Func<int, float> l = Sine(60f, rate);
                    Func<int, float> r = i => -l(i);
                    var off = Render("midside", "midDb=0;sideDb=0;width=1;monoBelowHz=0", l, 16,
                        rate: rate, stimR: r);
                    var on = Render("midside", "midDb=0;sideDb=0;width=1;monoBelowHz=120", l, 16,
                        rate: rate, stimR: r);
                    var db = 20f * MathF.Log10(
                        MathF.Max(Rms(on.L, 10 * Frames, 4 * Frames), 1e-9f)
                        / Rms(off.L, 10 * Frames, 4 * Frames));
                    Assert.True(db < -12f,
                        $"midside at {rate} Hz only took {-db:F1} dB off the 60 Hz side — the shelf "
                        + "corner did not follow the rate");
                }
                break;
            }
            case "looper":
            {
                // The record CAP is in seconds, so it is a different number of
                // frames at each rate. maxLoopS=1 at 192 kHz must hold 4x what
                // it holds at 48 kHz — and the loop that closes on the cap is
                // the one whose length proves it.
                foreach (var rate in new[] { Rate48, Rate192 })
                {
                    var blocks = 2 * rate / Frames + 20;
                    // The input STOPS the moment the 1 s cap is reached, so
                    // anything on the output after that is the loop and only
                    // the loop. A cap computed once at 48 kHz would close the
                    // pass four times early at 192 kHz, leaving a quarter-length
                    // loop that runs out under this window.
                    var run = Render("looper", "maxLoopS=1;loopLevel=1;mix=1",
                        SineBetween(187.5f, Frames, Frames + rate, rate), blocks,
                        [(1, "state=1")], rate: rate);
                    var at = Frames + (int)(1.4f * rate);
                    Assert.True(Rms(run.L, at, 4 * Frames) > 0.05f,
                        $"looper at {rate} Hz produced no loop after its 1 s cap closed the pass");
                }
                break;
            }
            default:
                Assert.Fail($"no rate probe for {module}");
                break;
        }
    }
}
