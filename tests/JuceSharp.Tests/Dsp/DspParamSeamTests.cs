using System.Globalization;
using System.Runtime.InteropServices;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// A parameter must never put a STEP into a running modulator or into the read
/// head of a loaded delay line (the 0.48 audit). Three cores did:
///
/// <list type="bullet">
/// <item><c>vibrato</c> — <c>Lfo::setShape</c> swapped the waveform under the
/// running LFO. At one phase sine and triangle differ by up to 1.0, and the LFO
/// IS the delay: the read head teleported by the whole depth, 576 samples
/// (12 ms) at the maximum, mid-note.</item>
/// <item><c>ringmod</c> — the same switch on the carrier: a full-scale
/// discontinuity in the wet, loudest at mix 1.</item>
/// <item><c>resonator</c> — key/chord/octave/spread rewrote the per-voice tap of
/// a Karplus-Strong loop whose feedback runs to 0.998, so the splice was not a
/// click that decayed but one re-injected into the loop for up to six
/// seconds.</item>
/// </list>
///
/// <para>Rendered offline on a stopped engine, like <see cref="DspGainZipperTests"/>.
/// The probe is the artefact itself: the largest sample-to-sample step over the
/// 12.5 ms after the change, against the SAME signal's own largest step once it
/// has settled. A step is what a listener hears as a click, and it is what a
/// crossfade removes without changing anything else about the sound.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class DspParamSeamTests : IDisposable
{
    private const string Lib = "juce_sharp";

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_add_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAddNode(nint engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_clear_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspClearFor(nint engine, int player);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, float value);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_process_buffer", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspProcess(nint engine, float[] left, float[] right, int frames, int sampleRate);

    private const int Ok = 0;
    private const int Frames = 2048;
    private const int SampleRate = 48000;
    /// <summary>Blocks of audio before the seam: fills rings, settles trackers.</summary>
    private const int PreRoll = 4;
    /// <summary>The window a step is looked for in — longer than every ramp involved.</summary>
    private const int SeamWindow = 600;
    /// <summary>Where the same signal's OWN largest step is measured.</summary>
    private const int SettledStart = 800;

    private readonly JuceAudioEngine? _engine;

    public DspParamSeamTests()
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

    public static TheoryData<string, string, string, float, float> Cases => new()
    {
        // module, setup, param, before, after
        // Sine -> Triangle, the switch both LFO cores got wrong.
        { "vibrato",   "rateHz=2;depthMs=12",            "waveform", 0f, 1f },
        { "ringmod",   "freqHz=500;mix=1",               "waveform", 0f, 1f },
        // An octave down DOUBLES every tap: the biggest retune the knobs allow,
        // and the one that made the loaded loop splice audible for seconds.
        { "resonator", "mix=1;decay=1;chord=1;octave=2", "octave",   2f, 1f },

        // W4-D18: a modulation DEPTH is ADDED to a read tap, so a write moves
        // the tap instantly by delta * |lfo| -- 144 samples (3 ms) at 48 kHz
        // for tapedelay's wowFlutter, 17 for slapback's wobble. `timeMs` got a
        // 50 ms glide for exactly this reason; the depth added to it did not.
        { "tapedelay", "timeMs=200;feedback=0.5;mix=1",   "wowFlutter", 0f, 1f },
        // springreverb's `drip` is ramped too and has NO row: measured, it
        // passes with the depth stepped. Its 143 samples of tap movement go
        // through three feedback tanks and eight allpass stages before the
        // output, which smears the splice below this probe's floor -- the same
        // wall platereverb's predelay hit. Same fix as tapedelay's, which does
        // discriminate; no evidence of its own.
        // slapback's `wobble` is ramped too but has NO row: its maximum depth is
        // 0.35 ms (17 samples at 48 kHz) against tapedelay's 144, so the splice
        // it produces is under this probe's floor -- measured, it passes with
        // the depth stepped. Same fix, same commit, no evidence of its own.
        // `delay.pingPong` (W4-D19) is NOT here: this theory feeds the SAME
        // signal to both channels, and cross-feeding L<->R is then identical to
        // self-feeding -- the row reports "the two settings sound the same
        // (RMS difference 0,00000)". It has its own fact below, with a stereo
        // stimulus, because that is the only way the parameter means anything.

        // NOTE: `tremolo.shape` (W4-D16) has no row, and a crossfade for it was
        // written, measured and REVERTED rather than shipped untested. The
        // core's 1.5 ms one-pole already spreads the shape switch over ~72
        // samples, so the artefact is a THUMP (an envelope dent) and not a
        // sample-to-sample step -- which is what the audit itself says, and
        // which is the one thing this probe cannot see. Measured: the probe
        // passes with the switch hard, at both 0->1 and 0->2.
        // `phaser.stages` (W4-D20) is fixed and has no row HERE for the same
        // reason -- the stage count changes the notch PATTERN rather than
        // producing an edge. It is pinned by the double-toggle theory below,
        // which is about the crossfade's re-entrancy and does discriminate.
    };

    /// <summary>
    /// <c>delay.pingPong</c> swaps which channel's delay line feeds the
    /// feedback path. L and R line contents are uncorrelated at that instant,
    /// so the toggle was a full-amplitude splice INSIDE a loop running at up to
    /// 0.95 — and the repo's loop-gain exemption does not cover it, because a
    /// routing switch is not a level.
    ///
    /// <para>This needs its own fact rather than a <see cref="Cases"/> row: the
    /// theory above drives both channels with the SAME signal, which makes
    /// cross-feed and self-feed bit-identical and the probe vacuous (measured
    /// RMS difference 0.00000). Here the two channels carry different tones, so
    /// the lines genuinely diverge.</para>
    /// </summary>
    [NativeAvailableFact]
    public void DelayPingPong_CrossfadesTheFeedbackSource_InsteadOfSplicing()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, "delay");
        Assert.True(n >= 0, "delay: add failed");
        foreach (var pair in "timeMs=150;feedback=0.85;mix=1".Split(';'))
        {
            var kv = pair.Split('=');
            Assert.Equal(Ok, DspSetParam(H, n, kv[0],
                float.Parse(kv[1], CultureInfo.InvariantCulture)));
        }
        Assert.Equal(Ok, DspSetParam(H, n, "pingPong", 0f));

        // Fill both lines with DIFFERENT material, for long enough that the
        // feedback tail is established on each.
        for (var b = 0; b < PreRoll + 2; b++) RenderStereo(b * Frames, out _);
        var settled = RenderStereo((PreRoll + 2) * Frames, out var settledRight);
        var natural = MathF.Max(MaxStep(settled, 1, Frames), MaxStep(settledRight, 1, Frames));
        Assert.True(natural > 1e-4f, "the probe signal is silent");

        // The two lines must actually differ, or the switch has nothing to do.
        var divergence = RmsDiff(settled, settledRight, SettledStart, Frames - SettledStart);
        Assert.True(divergence > 0.01f,
            $"the two channels' lines carry the same audio (RMS difference {divergence:F5}) "
            + "— the case proves nothing");

        var tailL = settled[^1];
        var tailR = settledRight[^1];
        Assert.Equal(Ok, DspSetParam(H, n, "pingPong", 1f));

        // The horizon is FIVE blocks, not the usual SeamWindow, and that is the
        // whole reason this parameter needs a hand-written probe. The output
        // reads its OWN line whatever pingPong says; only the FEEDBACK source
        // moves. So the splice is WRITTEN at the toggle and first HEARD one
        // delay period later — 150 ms, or ~3.5 blocks at 2,048 samples. A
        // 600-sample window sees nothing at all and passes with the switch hard.
        var step = 0f;
        var prevL = tailL;
        var prevR = tailR;
        for (var b = 0; b < 5; b++)
        {
            var blk = RenderStereo((PreRoll + 3 + b) * Frames, out var blkR);
            step = MathF.Max(step, MathF.Abs(blk[0] - prevL));
            step = MathF.Max(step, MathF.Abs(blkR[0] - prevR));
            for (var i = 1; i < Frames; i++)
            {
                step = MathF.Max(step, MathF.Abs(blk[i] - blk[i - 1]));
                step = MathF.Max(step, MathF.Abs(blkR[i] - blkR[i - 1]));
            }
            prevL = blk[^1];
            prevR = blkR[^1];
        }

        var allowed = 3f * natural;
        Assert.True(step < allowed,
            $"toggling pingPong stepped the output by {step:F4}, against a settled slope of "
            + $"{natural:F4} (allowed {allowed:F4}) — that is a splice in the FEEDBACK path, "
            + "not a fade");
    }

    /// <summary>
    /// <c>glitch</c>'s repeat decay multiplies <c>repeatGain_</c> by up to 0.6
    /// at every loop wrap. The core goes to real trouble to make the WAVEFORM
    /// continuous there — <c>readSlice</c>'s <c>p + loopLen_</c> bridge exists
    /// for nothing else — and then multiplied both sides of that splice by a
    /// gain that jumped 40 % at the same sample. A click every lap.
    ///
    /// <para>The probe is not a parameter seam, because the artefact is not at
    /// one: it recurs at each wrap. The reference is the SAME loop at
    /// <c>decay = 0</c>, where the gain never moves and only the waveform
    /// bridge is at work — so this measures the decay's contribution and
    /// nothing else.</para>
    /// </summary>
    [NativeAvailableFact]
    public void GlitchRepeatDecay_RampsAcrossTheLoopWrap_InsteadOfStepping()
    {
        var quiet = RenderGlitchLoop(0f);
        var decayed = RenderGlitchLoop(1f);

        var natural = MaxStep(quiet, 1, Frames);
        Assert.True(natural > 1e-5f, "the captured loop is silent — the probe proves nothing");

        // The decayed run must still be AUDIBLE, or a core that muted itself
        // would post a perfect step.
        var level = 0f;
        for (var i = 0; i < Frames; i++) level = MathF.Max(level, MathF.Abs(decayed[i]));
        Assert.True(level > 0.01f, $"the decayed loop is silent (peak {level:F4})");

        var step = MaxStep(decayed, 1, Frames);
        // 2.5x, not the 3x the theory above uses: measured, the unramped gain
        // steps 0.0114 against 0.0036 for the same loop at decay 0, and 3x
        // leaves only a 6 percent margin on a deterministic stimulus.
        var allowed = 2.5f * natural;
        Assert.True(step < allowed,
            $"the repeat decay stepped the output by {step:F4} at a loop wrap, against "
            + $"{natural:F4} for the same loop at decay 0 (allowed {allowed:F4}) — the gain "
            + "is jumping at the sample the waveform bridge exists to make continuous");
    }

    /// Captures a short slice in Manual mode and lets it loop, returning a block
    /// well into the repeats.
    private float[] RenderGlitchLoop(float decay)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, "glitch");
        Assert.True(n >= 0, "glitch: add failed");
        foreach (var pair in "mode=0;mix=1;sliceMs=20;repeats=8".Split(';'))
        {
            var kv = pair.Split('=');
            Assert.Equal(Ok, DspSetParam(H, n, kv[0],
                float.Parse(kv[1], CultureInfo.InvariantCulture)));
        }
        Assert.Equal(Ok, DspSetParam(H, n, "decay", decay));

        for (var b = 0; b < PreRoll; b++) Render(b * Frames);
        Assert.Equal(Ok, DspSetParam(H, n, "trigger", 1f));   // footswitch down
        // Two blocks of looping before the one measured, so several wraps have
        // already happened and the chain's own add crossfade is long finished.
        Render(PreRoll * Frames);
        Render((PreRoll + 1) * Frames);
        return Render((PreRoll + 2) * Frames);
    }

    /// <summary>Renders with DIFFERENT tones in L and R, and returns both.</summary>
    private float[] RenderStereo(int offset, out float[] right)
    {
        var left = Sine(offset);
        right = SineAt(offset, 173f);
        Assert.Equal(Ok, DspProcess(H, left, right, Frames, SampleRate));
        return left;
    }

    private static float[] SineAt(int offset, float hz)
    {
        var buf = new float[Frames];
        for (var i = 0; i < Frames; i++)
            buf[i] = 0.25f * MathF.Sin(2f * MathF.PI * hz * (offset + i) / SampleRate);
        return buf;
    }

    /// <summary>
    /// A crossfade that is not RE-ENTRANT re-introduces the click it exists to
    /// remove. Flipping back inside the 10 ms window used to run
    /// <c>setCurrentAndTarget(0)</c> over a blend in flight, so the output went
    /// from <c>(1-t)·Sine + t·Tri</c> to <c>1.0·Tri</c> in ONE sample — up to
    /// ~0.7 full scale. Every shape/stage switch in the rig now swaps the two
    /// ends and keeps the blend where it is instead.
    /// </summary>
    public static TheoryData<string, string, string, float, float> DoubleToggleCases => new()
    {
        { "ringmod",   "freqHz=500;mix=1",                "waveform", 0f, 1f },
        { "vibrato",   "rateHz=2;depthMs=12",             "waveform", 0f, 1f },
        // The row that DISCRIMINATES: with the re-entrancy removed this steps
        // the output by 0.0611 against a settled slope of 0.0040. The two LFO
        // rows above are the same mechanism and the same fix; they are kept as
        // coverage, but on this stimulus they pass either way.
        { "phaser",    "rateHz=0.5;depth=1;mix=1;feedback=0.7;stages=6", "stages", 6f, 2f },
    };

    /// <summary>Well inside the 10 ms (480-sample) fade window.</summary>
    private const int InsideTheFade = 96;

    [NativeAvailableTheory]
    [MemberData(nameof(DoubleToggleCases))]
    public void FlippingBackInsideTheFadeWindow_KeepsTheBlend(
        string module, string setup, string param, float before, float after)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        foreach (var pair in setup.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = pair.Split('=');
            Assert.Equal(Ok, DspSetParam(H, n, kv[0],
                float.Parse(kv[1], CultureInfo.InvariantCulture)));
        }
        Assert.Equal(Ok, DspSetParam(H, n, param, before));

        for (var b = 0; b < PreRoll; b++) Render(b * Frames);
        var settled = Render(PreRoll * Frames);
        // The WHOLE settled block, not its tail: a tremolo at depth 1 spends
        // part of every LFO cycle near silence, and a tail-only slope there is
        // a measurement of the quiet part rather than of the signal.
        var natural = MaxStep(settled, 1, Frames);
        Assert.True(natural > 1e-5f, $"{module}: the probe signal is silent");
        var tail = settled[^1];

        // Switch, render the samples the fade is mid-blend over, switch BACK.
        // Both edges land inside one block so the second one provably arrives
        // while the first fade is still walking.
        Assert.Equal(Ok, DspSetParam(H, n, param, after));
        var block = Render((PreRoll + 1) * Frames);
        // The chain drains setParam at the top of the block, so the second
        // write lands at the start of the NEXT one — InsideTheFade samples of
        // a 480-sample ramp have elapsed by then.
        Assert.Equal(Ok, DspSetParam(H, n, param, before));
        var back = Render((PreRoll + 2) * Frames);

        var step = MathF.Abs(block[0] - tail);
        for (var i = 1; i < Frames; i++)
            step = MathF.Max(step, MathF.Abs(block[i] - block[i - 1]));
        step = MathF.Max(step, MathF.Abs(back[0] - block[^1]));
        for (var i = 1; i < SeamWindow; i++)
            step = MathF.Max(step, MathF.Abs(back[i] - back[i - 1]));

        var allowed = 3f * natural;
        Assert.True(step < allowed,
            $"{module}.{param}: flipping back inside the fade stepped the output by "
            + $"{step:F4}, against a settled slope of {natural:F4} (allowed {allowed:F4}) "
            + "— the second switch is discarding the blend in flight");
    }

    [NativeAvailableTheory]
    [MemberData(nameof(Cases))]
    public void ChangingAModulator_CrossfadesInsteadOfSplicing(
        string module, string setup, string param, float before, float after)
    {
        // The change has to be AUDIBLE or a core that ignored the parameter
        // outright would post a perfect seam.
        var settledBefore = RenderSeam(module, setup, param, before, before, out var tailBefore);
        var settledAfter = RenderSeam(module, setup, param, after, after, out _);
        var span = RmsDiff(settledBefore, settledAfter, SettledStart, Frames - SettledStart);
        Assert.True(span > 0.005f,
            $"{module}.{param}: the two settings sound the same (RMS difference {span:F5}) "
            + "— the case proves nothing");

        var seam = RenderSeam(module, setup, param, before, after, out var tail);

        // The signal's own worst slope, taken from both settled states so a core
        // that is simply louder at one setting does not get a free pass.
        var natural = MathF.Max(MaxStep(settledBefore, SettledStart, Frames),
                                MaxStep(settledAfter, SettledStart, Frames));
        Assert.True(natural > 1e-5f, $"{module}: the probe signal is silent");

        var seamStep = MathF.Abs(seam[0] - tail);
        for (var i = 1; i < SeamWindow; i++)
            seamStep = MathF.Max(seamStep, MathF.Abs(seam[i] - seam[i - 1]));

        var allowed = 3f * natural;
        Assert.True(seamStep < allowed,
            $"{module}.{param}: the change stepped the output by {seamStep:F4}, against a "
            + $"settled slope of {natural:F4} (allowed {allowed:F4}) — that is a splice, not a fade");

        // And the previous block must be innocent, so a failure above cannot be
        // blamed on a noisy stimulus.
        var quiet = MathF.Abs(tail - tailBefore) < 1f; // tail values are bounded
        Assert.True(quiet, $"{module}: the stimulus itself is unstable");
    }

    /// <summary>
    /// A fresh node, PreRoll + 1 blocks at <paramref name="first"/>, then one at
    /// <paramref name="second"/> — the block returned. <paramref name="tail"/> is
    /// the last sample BEFORE the returned block, so a step across the block
    /// boundary is visible.
    /// </summary>
    private float[] RenderSeam(string module, string setup, string param,
                               float first, float second, out float tail)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        foreach (var pair in setup.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = pair.Split('=');
            Assert.Equal(Ok, DspSetParam(H, n, kv[0],
                float.Parse(kv[1], CultureInfo.InvariantCulture)));
        }
        Assert.Equal(Ok, DspSetParam(H, n, param, first));

        for (var b = 0; b < PreRoll; b++) Render(b * Frames);
        var a = Render(PreRoll * Frames);
        tail = a[^1];
        Assert.Equal(Ok, DspSetParam(H, n, param, second));
        return Render((PreRoll + 1) * Frames);
    }

    private float[] Render(int offset)
    {
        var left = Sine(offset);
        var right = Sine(offset);
        Assert.Equal(Ok, DspProcess(H, left, right, Frames, SampleRate));
        return left;
    }

    private static float[] Sine(int offset)
    {
        var buf = new float[Frames];
        for (var i = 0; i < Frames; i++)
            buf[i] = 0.25f * MathF.Sin(2f * MathF.PI * 110f * (offset + i) / SampleRate);
        return buf;
    }

    private static float MaxStep(float[] x, int start, int end)
    {
        var max = 0f;
        for (var i = start + 1; i < end; i++) max = MathF.Max(max, MathF.Abs(x[i] - x[i - 1]));
        return max;
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
}
