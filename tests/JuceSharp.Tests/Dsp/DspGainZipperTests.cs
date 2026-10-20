using System.Runtime.InteropServices;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// Every user-facing GAIN ramps, never steps (JuceSharp/AGENTS.md). The engine's
/// own faders have obeyed that since <c>AudioEngine::rampOrScale</c>; three
/// cores did not: the clean <c>gain</c> node applied one constant per block,
/// and the compressor's makeup and the sustainer's makeup and level were
/// per-block constants too.
/// A slider drag zippered and a preset switch stepped by the whole delta in
/// one sample.
///
/// <para>Rendered offline on a stopped engine, exactly like
/// <see cref="DspBypassCrossfadeTests"/>: one block at the old value, the
/// parameter changes, one block at the new value, and the seam must be a ramp.
/// Each case computes what the old STEP would have been at the seam and
/// requires it to be large, so a zero crossing cannot pass vacuously.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class DspGainZipperTests : IDisposable
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

    private readonly JuceAudioEngine? _engine;

    public DspGainZipperTests()
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

    private static float[] Sine(int offset)
    {
        var buf = new float[Frames];
        for (var i = 0; i < Frames; i++)
            buf[i] = 0.25f * MathF.Sin(2f * MathF.PI * 110f * (offset + i) / SampleRate);
        return buf;
    }

    private float[] Render(int offset)
    {
        var left = Sine(offset);
        var right = Sine(offset);
        Assert.Equal(Ok, DspProcess(H, left, right, Frames, SampleRate));
        return left;
    }

    private static float MaxStep(float previous, float[] block, int count)
    {
        var max = MathF.Abs(block[0] - previous);
        for (var i = 1; i < count; i++) max = MathF.Max(max, MathF.Abs(block[i] - block[i - 1]));
        return max;
    }

    /// <summary>The sine's own largest sample-to-sample step at unity gain.</summary>
    private const float SineStep = 0.25f * 2f * MathF.PI * 110f / SampleRate; // ~0.0036

    public static TheoryData<string, string, float, float> Cases => new()
    {
        // module, param, before, after — every pair is at least +12 dB of gain.
        { "gain", "gainDb", 0f, 12f },
        { "compressor", "makeupDb", 0f, 12f },
        { "sustainer", "level", 0.5f, 1f }, // output trim 0 → +12 dB ("sustain" is not a
                                            // usable probe: 20:1 compression eats most of
                                            // its +14 dB lift on a steady sine)
        // The bit crusher's FutzBox levels (#157): at 16 bits and no drive the
        // crushed path is a wire, so both are plain gains on it.
        { "bitcrusher", "inputDb", 0f, 12f },
        { "bitcrusher", "outputDb", 0f, 12f },
    };

    [NativeAvailableTheory]
    [MemberData(nameof(Cases))]
    public void ChangingAGain_RampsAcrossTheSeam_InsteadOfStepping(string module, string param, float before, float after)
    {
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        Assert.Equal(Ok, DspSetParam(H, n, param, before));

        Render(0); // let any detector settle on the steady sine
        var a = Render(Frames);
        Assert.Equal(Ok, DspSetParam(H, n, param, after));
        var b = Render(2 * Frames);

        // What the old constant-per-block code did at the seam: the whole gain
        // delta in one sample. Measured from the two blocks' own steady levels
        // so the test does not have to know each module's law.
        var levelA = Rms(a, Frames - 256, 256);
        var levelB = Rms(b, Frames - 256, 256);
        var ratio = levelB / levelA;
        Assert.True(ratio > 2.5f, $"{module}: fixture must change the gain a lot (got x{ratio:F2})");
        var oldStep = MathF.Abs(a[^1]) * (ratio - 1f);

        // The first 600 samples of block B contain the whole 10 ms ramp. No
        // step there may exceed the sine's own slope at the NEW gain plus the
        // ramp's per-sample contribution. The old cut must be well clear of
        // that bound (a compressed sine sits low, so the floor is relative).
        var allowed = SineStep * ratio * 2f + oldStep / 200f;
        Assert.True(oldStep > 3f * allowed,
            $"{module}: seam sits too close to a zero crossing (old step would be {oldStep:F3}, allowed {allowed:F4})");
        var seamStep = MaxStep(a[^1], b, 600);
        Assert.True(seamStep < allowed,
            $"{module}: seam stepped by {seamStep:F4} (allowed {allowed:F4}; a hard step would be {oldStep:F3})");
    }


    // ======================================================================
    // Mixes and output levels - the ~30 MEDIUM zippers of the 0.48 audit.
    //
    // The gain theory above needs the parameter to RAISE the level a lot, which
    // a dry/wet MIX does not do: at mix 0 you hear the dry, at mix 1 the wet,
    // and on a steady tone those often sit at the same RMS. So this theory does
    // not measure level at all - it measures WHERE THE OUTPUT IS between the two
    // settings, sample by sample, against two reference renders of the same
    // stimulus:
    //
    //   before : the parameter never moves            -> "no change" audio
    //   after  : the parameter was `after` all along  -> "instant change" audio
    //   seam   : two blocks at `before`, then `after` -> what we ship
    //
    // Every parameter here feeds ONLY the output blend, never the internal
    // state, so `after`'s third block is exactly what the old per-block constant
    // produced at the seam. A ramped seam must therefore still be close to
    // `before` at the START of the block and have arrived at `after` by the end;
    // the old code was `after` from sample 0, which is what the early-window
    // ratio catches and what no level-based probe could see.
    // ======================================================================

    /// (module, setup, arm, param, before, after). `setup` is applied before any
    /// audio, `arm` between the first and second block - a footswitch/latch that
    /// only means something once the core has heard something.
    public static TheoryData<string, string, string, string, float, float> MixCases => new()
    {
        // --- output levels: a voice or a pad summed under the dry ------------
        { "octaver",      "sub1=0;sub2=0",            "", "dry",   1f, 0f },
        { "octaver",      "dry=0;sub2=0;toneHz=5000", "", "sub1",  0f, 1f },
        { "octaver",      "dry=0;sub1=0;toneHz=5000", "", "sub2",  0f, 1f },
        { "subharmonic",  "sub1=0;sub2=0",            "", "dry",   1f, 0f },
        { "subharmonic",  "dry=0;sub2=0",             "", "sub1",  0f, 1f },
        { "subharmonic",  "dry=0;sub1=0",             "", "sub2",  0f, 1f },
        { "feedbacker",   "riseMs=10;thresholdDb=-60","", "mix",   0f, 1f },
        { "freeze",       "fadeMs=5;lengthMs=120",    "capture=1", "level", 0f, 1f },

        // --- dry/wet mixes ---------------------------------------------------
        { "reverb",       "roomSize=0.9",             "", "mix",   0f, 1f },
        { "reverb",       "roomSize=0.9;mix=1",       "", "width", 1f, 0f },
        { "platereverb",  "decay=0.9",                "", "mix",   0f, 1f },
        { "platereverb",  "decay=0.9;mix=1",          "", "width", 1f, 0f },
        { "springreverb", "decay=0.9",                "", "mix",   0f, 1f },
        { "shimmer",      "decay=0.9",                "", "mix",   0f, 1f },
        { "harmonizer",   "semitones=7",              "", "mix",   0f, 1f },
        { "whammy",       "position=1;glideMs=0",     "", "mix",   0f, 1f },
        { "tapedelay",    "timeMs=200;feedback=0.5",  "", "mix",   0f, 1f },
        { "wah",          "mode=1;position=0.8",      "", "mix",   0f, 1f },
        { "autowah",      "sensitivity=0.9",          "", "mix",   0f, 1f },
        { "cab",          "",                         "", "mix",   0f, 1f },
        { "envfilter",    "sensitivity=0.9",          "", "mix",   0f, 1f },
        { "filterseq",    "rateHz=4",                 "", "mix",   0f, 1f },
        { "formant",      "vowel=1;rateHz=0",         "", "amount",0f, 1f },
        { "acousticsim",  "body=1",                   "", "mix",   0f, 1f },
        { "resonator",    "decay=1;chord=1",          "", "mix",   0f, 1f },
        { "glitch",       "mode=1;chance=1;sliceMs=60", "", "mix", 0f, 1f },
        // `glitch.decay` (W4-D8) is NOT a trajectory: its artefact is a step at
        // every loop WRAP, not at the parameter seam. It has its own fact in
        // DspParamSeamTests.
        { "miku",         "",                         "", "mix",   0f, 1f },
        { "mikustomp",    "",                         "", "mix",   0f, 1f },
        // W4-D10: `character` moves every formant centre AND the pure output
        // scalar at the end of the cascade (cascadeNorm_, which the core's own
        // comment calls "only the level"), while nasalAmt_/bwMul_ beside it
        // were already smoothed.
        { "miku",         "mix=1",                    "", "character", 0f, 1f },
        // W4-D3: `voices` 1 -> 2 stepped the wet normalisation 1.0 -> 0.5 (a
        // 6 dB cut in one sample) AND introduced a new read tap at a different
        // base delay, at full weight, while every other knob in that core --
        // lfoShape included -- was already smoothed.
        { "chorus",       "mix=1;depthMs=12;voices=1", "", "voices", 1f, 2f },
        // The three normalised saturation DRIVES of W4-D7 / W4-D9 are ramped in
        // the cores and have no row, each for a measured reason:
        //   * `boost.character` reads 92 % of the way at 128 samples WITH the
        //     ramp, because that one knob also flips `satActive_` (the whole
        //     saturation branch, tanh included, switches in) and moves the
        //     input high-pass corner 20 Hz -> 700 Hz. The drive is one of three
        //     things it does and no probe here separates them.
        //   * `slapback.drive` and `tapedelay.drive` report the seam 100 % away
        //     from the reference 848 samples on: the saturation is on the
        //     RECORD path, so the reference render's delay line holds audio
        //     that was saturated differently for its whole pre-roll and the two
        //     never converge inside a block. The same wall platereverb's
        //     predelay hit, one stage further in.
        // (mikustomp's `character` has no row -- see BandedMixCases below.)
        { "tapestop",     "stopMs=3000",       "trigger=1", "mix",   0f, 1f },
        { "harmtremolo",  "rateHz=0.5",               "", "depth", 0f, 1f },

        // --- W4 wave ---------------------------------------------------------
        // The amp's PREAMP drive. `gain` spans up to +58 dB (~790x) on the
        // hottest voicing and landed as ONE constant per block into the FIRST
        // tanh; `master_` was the only ramped level in that core. It belongs
        // here and not in `Cases` because the cascade SATURATES: +58 dB of
        // drive into a tanh raises the output level barely at all, which is
        // exactly the shape `Cases` cannot see.
        { "amp",          "voicing=0;master=0.5",     "", "gain",   0f, 1f },
        // The overdrive's `model` footswitch: 2 -> 3 steps the dry/wet blend
        // 1.00 -> 0.60 and the shaper bias 0.06 -> 0.04 in one sample, and
        // switches in two biquads carrying state from the last selection.
        { "overdrive",    "drive=0.8;level=0.8;model=2", "", "model", 2f, 3f },
        // NOTE: `platereverb.predelayMs` is deliberately NOT here, and not in
        // DspParamSeamTests either. It IS a teleporting read tap into the tank
        // input (fixed 2026-09-11, W4-D14, a 20 ms SmoothedValue mirroring
        // dcdsp_resonator.h), but neither harness can see it: the four
        // diffusers between the tap and the module's only observable smear the
        // splice until the step probe measures NO contrast, and the trajectory
        // probe cannot converge because the tank carries the old tap's phase
        // for far longer than one 2,048-sample block (measured: the seam sits
        // 102 % from the "always after" reference at sample 1,200 with the fix
        // in and 98 % without it). Do not add a row here expecting it to work.
        // A named 3-way picker read PER SAMPLE with nothing damping it: square
        // and saw differ by up to 2.0 at one phase. Same reason as above — the
        // 900 Hz SVF after the oscillator makes the switch's step and the
        // shape's OWN slope the same size, so only the trajectory separates them.
        { "subharmonic",  "sub1=1;sub2=0;dry=0",      "", "waveform", 0f, 2f },
        // NOTE: the four RESONANCE-shaped gains W4-D4/W4-D5 name --
        // `wah.resonance`, `envfilter.resonance`, `filterseq.resonance` and
        // `formant.brightness` -- have no row here, and a ramp for them was
        // written, measured and REVERTED rather than shipped untested. Two
        // independent reasons, both measured 2026-09-11:
        //   * The stimulus. A 110 Hz sine carries no energy in the band a wah
        //     at position 0.8 or a formant filter at 500-3,000 Hz is shaping,
        //     so both rows trip this theory's OWN vacuity guard (early span
        //     0.00098 and 0.00036 against a 0.005 floor) whatever the core does.
        //   * The mapping. This theory assumes the output moves LINEARLY with
        //     the parameter, because that is what makes "27 % of the ramp" mean
        //     "under half way there". These gains are RECIPROCAL (1/Q, or
        //     1/(1+0.25(q-0.7))), so a perfect linear ramp is already ~80 % of
        //     the way in output terms at 27 % -- measured 84 % for filterseq
        //     and 67 % for envfilter WITH a correct ramp in place.
        // A probe for them needs a stimulus inside the filter's band and a
        // bound on the output's own trajectory rather than on the parameter's.
    };

    /// Samples of block 3 that must still be close to the OLD value. The ramp is
    /// 10 ms (480 samples at 48 kHz), so 128 samples in it is ~27 % of the way.
    private const int EarlyWindow = 128;
    /// Where the ramp is long finished and the seam must equal the NEW value.
    private const int LateStart = 1200;

    [NativeAvailableTheory]
    [MemberData(nameof(MixCases))]
    public void ChangingAMixOrLevel_WalksAcrossTheSeam_InsteadOfSwitching(
        string module, string setup, string arm, string param, float before, float after)
    {
        var refBefore = RenderSeam(module, setup, arm, param, before, before);
        var refAfter  = RenderSeam(module, setup, arm, param, after,  after);
        var seam      = RenderSeam(module, setup, arm, param, before, after);

        // How far apart the two settings actually are, in each window. If either
        // is tiny the case proves nothing - pick a parameter pair that moves.
        var earlySpan = RmsDiff(refBefore, refAfter, 0, EarlyWindow);
        var lateSpan  = RmsDiff(refBefore, refAfter, LateStart, Frames - LateStart);
        Assert.True(earlySpan > 0.005f,
            $"{module}.{param}: the two settings are indistinguishable at the seam "
            + $"(early span {earlySpan:F5}) - the case is vacuous");
        Assert.True(lateSpan > 0.005f,
            $"{module}.{param}: the two settings are indistinguishable later in the block "
            + $"(late span {lateSpan:F5}) - the case is vacuous");

        // A hard switch puts the seam ON `after` from sample 0, i.e. ratio 1.0.
        var earlyRatio = RmsDiff(seam, refBefore, 0, EarlyWindow) / earlySpan;
        Assert.True(earlyRatio < 0.5f,
            $"{module}.{param}: the first {EarlyWindow} samples after the change are "
            + $"{earlyRatio * 100f:F0} % of the way to the new setting - a ramp is well "
            + "under half, a hard switch is 100 %");

        // ...and it must actually ARRIVE, or "ramping" would just mean "ignored".
        var lateRatio = RmsDiff(seam, refAfter, LateStart, Frames - LateStart) / lateSpan;
        Assert.True(lateRatio < 0.15f,
            $"{module}.{param}: {Frames - LateStart} samples later the seam is still "
            + $"{lateRatio * 100f:F0} % away from the new setting - the ramp never landed");
    }

    /// Pre-roll blocks before the latch is armed. Five, not one: `freeze` snapshots
    /// its loop plus a 40 ms pre-roll out of a ring that starts SILENT, so a shorter
    /// run captured mostly zeros and the level knob had nothing to scale.
    private const int PreRoll = 5;

    /// A fresh node, PreRoll + 2 blocks at `first`, then one at `second`, which is
    /// the block returned. Deterministic - every core seeds its RNG in prepare(),
    /// and the stimulus is a function of the ABSOLUTE sample index, so the three
    /// runs of a case are sample-aligned.
    private float[] RenderSeam(string module, string setup, string arm,
                               string param, float first, float second)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        ApplyAll(n, setup);
        Assert.Equal(Ok, DspSetParam(H, n, param, first));

        // Settle detectors and trackers, fill delay/freeze rings, and finish the
        // chain's own add crossfade.
        for (var b = 0; b < PreRoll; b++) Render(b * Frames);
        ApplyAll(n, arm);
        Render(PreRoll * Frames);
        Assert.Equal(Ok, DspSetParam(H, n, param, second));
        return Render((PreRoll + 1) * Frames);
    }

    private void ApplyAll(int node, string spec)
    {
        if (string.IsNullOrEmpty(spec)) return;
        foreach (var pair in spec.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = pair.Split('=');
            Assert.Equal(Ok, DspSetParam(H, node, kv[0],
                float.Parse(kv[1], System.Globalization.CultureInfo.InvariantCulture)));
        }
    }

    // ======================================================================
    // Band-limited / non-linearly-mapped gains - the W4 wave.
    //
    // `MixCases` above cannot see two whole classes of gain, and both were
    // MEASURED rather than guessed (2026-09-11):
    //
    //   * Its stimulus is a 110 Hz sine, which carries no energy in the band a
    //     wah at position 0.8 or a formant filter at 500-3,000 Hz is shaping.
    //     Those rows trip its own vacuity guard (early spans 0.00098 and
    //     0.00036 against a 0.005 floor) whatever the core does. So this theory
    //     drives fixed-seed BROADBAND NOISE, which has energy everywhere.
    //
    //   * Its early window is 128 samples and its bound is 0.5, which encodes
    //     "a linear ramp is about a quarter of the way there". That is a
    //     statement about the PARAMETER. A gain of 1/Q, or 1/(1+0.25(q-0.7)),
    //     moves most of its output distance in the first sliver of knob travel:
    //     a perfect linear ramp is already ~80 % of the way at 27 % of the
    //     window (measured 84 % filterseq, 67 % envfilter). So each case here
    //     carries its OWN window, and the bound is on where the OUTPUT has got
    //     to by then - which is the thing a listener hears.
    //
    // Picking a window: it has to be early enough that the ramp has not
    // arrived, and late enough that a HARD switch has. For a reciprocal gain
    // 16 samples of a 480-sample ramp is ~3 % of knob travel and still under
    // 30 % of the output distance, while a hard switch is 100 % from sample 0.
    // For a gain behind a one-pole (tremolo's 1.5 ms follower) the window must
    // be a few time constants of THAT, or neither side has moved.
    // ======================================================================

    /// (module, setup, param, before, after, earlyWindow). No `arm` column:
    /// every case here is a knob, not a footswitch.
    public static TheoryData<string, string, string, float, float, int> BandedMixCases => new()
    {
        // Reciprocal gains: 16 samples, ~3 % of the ramp.
        { "envfilter",   "sensitivity=0.9;mix=1",          "resonance",  0f, 1f, 16 },
        { "formant",     "vowel=1;rateHz=0;amount=1",      "brightness", 0f, 1f, 16 },
        // NOT here either: mikustomp's `character` has no probe in EITHER theory.
        // MixCases reads 86 % of the way at 128 samples for a correct 30 ms ramp
        // and 61 % for a 10 ms one -- it gets WORSE as the ramp gets slower,
        // which is the tell that a formant move's intermediate states are not
        // "between" its endpoints in RMS terms at all. A 16-sample window here
        // is vacuous the other way (span 0.00000: the voice is a gated synth and
        // has not started singing that early in the block). The FIX is shipped
        // -- it is the identical smoother miku's row below does pin, in the
        // sibling core, and this repo's history is that fixing one of a pair and
        // leaving the other costs a release.
        // NOT here, and each was retried against THIS theory before being left
        // out (2026-09-11). `wah.resonance` passes with the switch hard at both
        // 16 and 48 samples: a wah's peak gain reaches the output through a
        // resonant SVF whose own state dominates the first samples, so the
        // artefact is not in the window and widening the window walks into the
        // ramp. `filterseq.resonance` trips the vacuity guard at both (span
        // 0.00175 and 0.00170 against a 0.002 floor) -- its resComp only spans
        // 1.0 -> 0.364 and the sequencer is sweeping the cutoff underneath it,
        // so the two settings are genuinely close over a short window. Both
        // remain UNFIXED for want of a probe.
        // Behind a 1.5 ms (72-sample) one-pole: a hard switch is 63 % of the
        // way there by then, a 10 ms crossfade 15 %.
        { "tremolo",     "rateHz=4;depth=0.8",             "shape",      0f, 1f, 144 },
        // `platereverb.predelayMs` is NOT here either, and it is the one case
        // where the fix SHIPPED without a probe (W4-D14, a 20 ms SmoothedValue).
        // Retried against this theory: the late assertion reports the seam 100 %
        // away from the "always after" reference 848 samples on, because the
        // tank carries the old tap's phase far longer than one block -- the same
        // wall the sine theory hit at 102 %. A broadband stimulus does not change
        // it; the observable is simply too far downstream of the tap.
    };

    [NativeAvailableTheory]
    [MemberData(nameof(BandedMixCases))]
    public void ChangingABandedGain_WalksAcrossTheSeam_InsteadOfSwitching(
        string module, string setup, string param, float before, float after, int earlyWindow)
    {
        var refBefore = RenderSeamNoise(module, setup, param, before, before);
        var refAfter  = RenderSeamNoise(module, setup, param, after,  after);
        var seam      = RenderSeamNoise(module, setup, param, before, after);

        var earlySpan = RmsDiff(refBefore, refAfter, 0, earlyWindow);
        var lateSpan  = RmsDiff(refBefore, refAfter, LateStart, Frames - LateStart);
        Assert.True(earlySpan > 0.002f,
            $"{module}.{param}: the two settings are indistinguishable in the first "
            + $"{earlyWindow} samples (span {earlySpan:F5}) - the case is vacuous");
        Assert.True(lateSpan > 0.002f,
            $"{module}.{param}: the two settings are indistinguishable later in the block "
            + $"(span {lateSpan:F5}) - the case is vacuous");

        var earlyRatio = RmsDiff(seam, refBefore, 0, earlyWindow) / earlySpan;
        Assert.True(earlyRatio < 0.5f,
            $"{module}.{param}: the first {earlyWindow} samples after the change are "
            + $"{earlyRatio * 100f:F0} % of the way to the new setting - a ramp is well "
            + "under half, a hard switch is 100 %");

        var lateRatio = RmsDiff(seam, refAfter, LateStart, Frames - LateStart) / lateSpan;
        Assert.True(lateRatio < 0.25f,
            $"{module}.{param}: {Frames - LateStart} samples later the seam is still "
            + $"{lateRatio * 100f:F0} % away from the new setting - the ramp never landed");
    }

    /// As <see cref="RenderSeam"/>, but on broadband noise. The stimulus is a
    /// pure function of the ABSOLUTE sample index (a hash, not a running PRNG),
    /// so the three renders of a case are sample-aligned exactly as the sine
    /// ones are - which is the whole reason the comparison means anything.
    private float[] RenderSeamNoise(string module, string setup, string param,
                                    float first, float second)
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, module);
        Assert.True(n >= 0, $"{module}: add failed");
        ApplyAll(n, setup);
        Assert.Equal(Ok, DspSetParam(H, n, param, first));

        for (var b = 0; b < PreRoll; b++) RenderNoise(b * Frames);
        RenderNoise(PreRoll * Frames);
        Assert.Equal(Ok, DspSetParam(H, n, param, second));
        return RenderNoise((PreRoll + 1) * Frames);
    }

    private float[] RenderNoise(int offset)
    {
        var left = Noise(offset);
        var right = Noise(offset);
        Assert.Equal(Ok, DspProcess(H, left, right, Frames, SampleRate));
        return left;
    }

    private static float[] Noise(int offset)
    {
        var buf = new float[Frames];
        for (var i = 0; i < Frames; i++)
        {
            // splitmix32 over the absolute index: positional, not sequential.
            var x = (uint)(offset + i) * 0x9E3779B9u + 0x85EBCA6Bu;
            x ^= x >> 16; x *= 0x7FEB352Du;
            x ^= x >> 15; x *= 0x846CA68Bu;
            x ^= x >> 16;
            buf[i] = 0.25f * ((x >> 8) * (1f / 8388608f) - 1f);
        }
        return buf;
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
}
