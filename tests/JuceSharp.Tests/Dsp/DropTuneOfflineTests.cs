using System.Runtime.InteropServices;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// Drop-tune (CSVD, dcdsp_droptune.h) coverage via the offline tooling export
/// <c>dca_droptune_process_offline</c> — the 2026-07-16 audit named the drop
/// tuner as having no xUnit coverage at all (test-gap list). These pin:
/// bypass identity, engage/disengage transitions mid-stream (including the
/// return to bit-exact dry), down-shift frequency correctness, and argument
/// validation. The deeper perceptual metrics stay in the standalone
/// tools/droptune_poly.cpp harness; this is the regression net in the gate.
/// </summary>
public sealed class DropTuneOfflineTests
{
    private const string Lib = "juce_sharp";
    private const int Sr = 48000;

    [DllImport(Lib, EntryPoint = "dca_droptune_process_offline", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DropTuneProcess(
        int sampleRate, float maxLagMs,
        float[] semitonesPerSegment, int[] segmentLengths, int numSegments,
        float[] input, float[] output, int totalSamples);

    private static float[] Sine(double freqHz, int samples, float amplitude = 0.5f)
    {
        var buf = new float[samples];
        for (var i = 0; i < samples; i++)
            buf[i] = amplitude * (float)Math.Sin(2.0 * Math.PI * freqHz * i / Sr);
        return buf;
    }

    /// Dominant frequency estimate via zero-crossing rate over a window.
    private static double ZeroCrossingFrequency(ReadOnlySpan<float> x)
    {
        var crossings = 0;
        for (var i = 1; i < x.Length; i++)
            if ((x[i - 1] < 0f) != (x[i] < 0f))
                crossings++;
        return crossings * (double)Sr / (2.0 * x.Length);
    }

    private static void AssertAllFinite(float[] x)
    {
        foreach (var v in x)
            Assert.True(float.IsFinite(v), "non-finite sample in drop-tune output");
    }

    [NativeAvailableFact]
    public void Bypass_IsBitExactIdentity()
    {
        var input = Sine(220.0, Sr / 2);
        var output = new float[input.Length];
        var status = DropTuneProcess(Sr, 20f,
            new[] { 0f }, new[] { input.Length }, 1, input, output, input.Length);
        Assert.Equal(0, status);
        // The bypass path never writes the buffer — 0 semitones must be a
        // true bypass (no processing, no added latency), per the header rules.
        Assert.Equal(input, output);
    }

    [NativeAvailableTheory]
    [InlineData(-12.0f)] // full octave — the worst case for the interpolator
    [InlineData(-5.0f)]
    public void DownShift_MovesTheFrequencyByTheRequestedRatio(float semitones)
    {
        const double f0 = 220.0;
        var input = Sine(f0, 2 * Sr);
        var output = new float[input.Length];
        var status = DropTuneProcess(Sr, 20f,
            new[] { semitones }, new[] { input.Length }, 1, input, output, input.Length);
        Assert.Equal(0, status);
        AssertAllFinite(output);

        // Measure the sustained tail (skip 0.5 s of ring priming + first
        // splices). CSVD sustain is a resampled copy of the input, so the
        // dominant frequency must sit at f0 * 2^(semitones/12).
        var expected = f0 * Math.Pow(2.0, semitones / 12.0);
        var measured = ZeroCrossingFrequency(output.AsSpan(Sr / 2, Sr));
        Assert.InRange(measured, expected * 0.94, expected * 1.06);

        // And it must still be signal, not a faded-out artifact: the
        // correlation-following crossfade law holds output power near unity.
        double sumSq = 0;
        for (var i = Sr / 2; i < Sr / 2 + Sr; i++) sumSq += output[i] * output[i];
        var rms = Math.Sqrt(sumSq / Sr);
        Assert.InRange(rms, 0.20, 0.55); // input RMS is 0.5/sqrt(2) ~= 0.354
    }

    [NativeAvailableFact]
    public void EngageAndDisengage_MidStream_AreCleanAndReturnToDry()
    {
        const double f0 = 196.0;
        var segLengths = new[] { Sr / 2, Sr, Sr / 2 };
        var total = segLengths[0] + segLengths[1] + segLengths[2];
        var input = Sine(f0, total);
        var output = new float[total];

        var status = DropTuneProcess(Sr, 20f,
            new[] { 0f, -2f, 0f }, segLengths, 3, input, output, total);
        Assert.Equal(0, status);
        AssertAllFinite(output);

        // Segment 1 (bypassed): identity.
        Assert.Equal(input.AsSpan(0, segLengths[0]).ToArray(),
                     output.AsSpan(0, segLengths[0]).ToArray());

        // Segment 2 (engaged, -2 semitones): shifted, sustained signal.
        // Engaging mid-note splices against the ring the bypass kept primed.
        var engagedTail = output.AsSpan(segLengths[0] + Sr / 4, Sr / 2);
        var measured = ZeroCrossingFrequency(engagedTail);
        var expected = f0 * Math.Pow(2.0, -2.0 / 12.0);
        Assert.InRange(measured, expected * 0.94, expected * 1.06);

        // Segment 3 (disengaged): 0 semitones is a TRUE bypass again — but
        // RELEASING the shift is a splice, and the core crossfades it over
        // snapXfade_ (1 ms) rather than cutting at one sample, so "bit-exact
        // dry" starts once that fade has drained. Both halves are asserted:
        // the fade RAN (or the release is cutting again), and nothing past it
        // is touched (or the bypass leaks).
        var seg3 = segLengths[0] + segLengths[1];
        var faded = 0;
        for (var i = 0; i < UnityFadeSamples; i++)
            if (output[seg3 + i] != input[seg3 + i]) faded++;
        Assert.True(faded > 0,
            "releasing the shift produced no crossfaded sample at all — the release is "
            + "cutting at one sample again");
        Assert.Equal(input.AsSpan(seg3 + UnityFadeSamples).ToArray(),
                     output.AsSpan(seg3 + UnityFadeSamples).ToArray());
    }

    /// <summary>DropTune::snapXfade_ — 1 ms at 48 kHz.</summary>
    private const int UnityFadeSamples = 48;

    /// <summary>
    /// Crossing unity in EITHER direction used to be an uncrossfaded splice.
    /// Engaging swapped the output from the dry sample to a sinc read
    /// <c>anchorDelay_</c> (1.3 ms) back in ONE sample; releasing swapped it
    /// back the same way. Every other head movement in that file is crossfaded
    /// (<c>fadeLen_</c>/<c>fadeRho_</c>), and <c>DspChain</c>'s 10 ms bypass
    /// crossfade does not cover it — drop tune is not a catalogue node, the
    /// engine drives it directly.
    ///
    /// <para>The probe is the artefact: the largest sample-to-sample step
    /// across each seam, against the same signal's own largest step once
    /// settled. Deliberately NOT a "is it bit-exact" check — that is the test
    /// above, and it is what made this one necessary.</para>
    /// </summary>
    [NativeAvailableFact]
    public void CrossingUnity_CrossfadesInsteadOfSplicing()
    {
        const double f0 = 196.0;
        var segLengths = new[] { Sr / 2, Sr, Sr / 2 };
        var total = segLengths[0] + segLengths[1] + segLengths[2];
        var input = Sine(f0, total);
        var output = new float[total];

        Assert.Equal(0, DropTuneProcess(Sr, 20f,
            new[] { 0f, -2f, 0f }, segLengths, 3, input, output, total));
        AssertAllFinite(output);

        // The signal's own worst slope, measured well away from both seams and
        // from the priming that follows an engage.
        var natural = MaxStep(output, segLengths[0] + Sr / 2, segLengths[0] + Sr / 2 + Sr / 4);
        Assert.True(natural > 1e-4f, "the probe signal is silent");

        // A full-amplitude splice is ~2x the peak; the fade must keep every
        // step inside a small multiple of what the material itself does.
        var allowed = 4f * natural;

        var engage = MaxStep(output, segLengths[0] - 1, segLengths[0] + 4 * UnityFadeSamples);
        Assert.True(engage < allowed,
            $"engaging stepped the output by {engage:F4} against a settled slope of "
            + $"{natural:F4} (allowed {allowed:F4}) — that is a splice, not a fade");

        var seg3 = segLengths[0] + segLengths[1];
        var release = MaxStep(output, seg3 - 1, seg3 + 4 * UnityFadeSamples);
        Assert.True(release < allowed,
            $"releasing stepped the output by {release:F4} against a settled slope of "
            + $"{natural:F4} (allowed {allowed:F4}) — that is a splice, not a fade");
    }

    private static float MaxStep(float[] x, int start, int end)
    {
        var max = 0f;
        for (var i = Math.Max(1, start) + 1; i < Math.Min(end, x.Length); i++)
            max = MathF.Max(max, MathF.Abs(x[i] - x[i - 1]));
        return max;
    }

    [NativeAvailableFact]
    public void SegmentLengths_MustSumToTotal()
    {
        var input = new float[1000];
        var output = new float[1000];
        var status = DropTuneProcess(Sr, 20f,
            new[] { -2f }, new[] { 999 }, 1, input, output, 1000);
        Assert.NotEqual(0, status); // DCA_ERR_INVALID_ARG
    }

    [NativeAvailableFact]
    public void LongRun_AtTheOctave_StaysFiniteAndPitched()
    {
        // 10 s at -12: dozens of splices + periodicity-probe cycles. This is
        // the bounded in-gate cousin of the w_ overflow concern (#6): the
        // counters are exercised far past several ring wraps (48k * 10 s
        // against a ~262k ring) and the pitch must not drift.
        const double f0 = 110.0;
        var input = Sine(f0, 10 * Sr);
        var output = new float[input.Length];
        var status = DropTuneProcess(Sr, 20f,
            new[] { -12f }, new[] { input.Length }, 1, input, output, input.Length);
        Assert.Equal(0, status);
        AssertAllFinite(output);

        var early = ZeroCrossingFrequency(output.AsSpan(1 * Sr, Sr));
        var late  = ZeroCrossingFrequency(output.AsSpan(8 * Sr, Sr));
        Assert.InRange(early, 55.0 * 0.93, 55.0 * 1.07);
        Assert.InRange(late,  55.0 * 0.93, 55.0 * 1.07);
    }
}

/// <summary>
///     The ABI-coverage gap the wave-4 audit found: <c>dca_droptune_process_offline</c>
///     was declared in <c>NativeMethods</c> and exposed by NO public wrapper
///     member anywhere, while <c>dsp-tone-rig.md</c> routes agents to the drop
///     tuner as a first-class feature. A NuGet consumer could build a rig and
///     could not render the drop tuner offline; the ONLY managed caller was the
///     test-local <c>[DllImport]</c> above.
///     <para>
///         <see cref="JuceAudioEngine.ProcessDropTuneOffline(ReadOnlySpan{float},Span{float},int,ReadOnlySpan{float},ReadOnlySpan{int},float)" />
///         closes it. Every case here compares the wrapper against the RAW
///         export, so "the wrapper is a wrapper" is measured rather than assumed
///         — the audit's own worked example of what a thin binding has to prove.
///     </para>
/// </summary>
public sealed class DropTuneOfflineWrapperTests
{
    private const string Lib = "juce_sharp";
    private const int Sr = 48000;

    [DllImport(Lib, EntryPoint = "dca_droptune_process_offline", CallingConvention = CallingConvention.Cdecl)]
    private static extern int RawDropTune(
        int sampleRate, float maxLagMs,
        float[] semitonesPerSegment, int[] segmentLengths, int numSegments,
        float[] input, float[] output, int totalSamples);

    private static float[] Sine(double freqHz, int samples, float amplitude = 0.5f)
    {
        var buf = new float[samples];
        for (var i = 0; i < samples; i++)
            buf[i] = amplitude * (float)Math.Sin(2.0 * Math.PI * freqHz * i / Sr);
        return buf;
    }

    /// <summary>Bit-for-bit against the raw export, over a two-segment
    /// engage/disengage — the case the segment list exists for.</summary>
    [NativeAvailableFact]
    public void TheWrapper_RendersExactlyWhatTheRawExportRenders()
    {
        var input = Sine(220.0, Sr / 2);
        var semis = new[] { 0f, -5f };
        var lens = new[] { input.Length / 2, input.Length - input.Length / 2 };

        var expected = new float[input.Length];
        Assert.Equal(0, RawDropTune(Sr, 20f, semis, lens, semis.Length, input, expected, input.Length));

        var actual = new float[input.Length];
        JuceAudioEngine.ProcessDropTuneOffline(input, actual, Sr, semis, lens, 20f);

        Assert.Equal(expected, actual);
    }

    /// <summary>The mode overload is the lag ceiling by name: 10 / 20 / 35 ms.
    /// Compared against the raw export at each, so a re-typed number here shows
    /// up as different AUDIO rather than as a comment nobody reads.</summary>
    [NativeAvailableTheory]
    [InlineData(JuceDropTuneMode.Tight, 10f)]
    [InlineData(JuceDropTuneMode.Balanced, 20f)]
    [InlineData(JuceDropTuneMode.Smooth, 35f)]
    public void TheModeOverload_MapsToTheShippedLagCeilings(JuceDropTuneMode mode, float expectedLagMs)
    {
        var input = Sine(110.0, Sr / 4);
        var semis = new[] { -7f };
        var lens = new[] { input.Length };

        var expected = new float[input.Length];
        Assert.Equal(0, RawDropTune(Sr, expectedLagMs, semis, lens, 1, input, expected, input.Length));

        var actual = new float[input.Length];
        JuceAudioEngine.ProcessDropTuneOffline(input, actual, Sr, semis, lens, mode);

        Assert.Equal(expected, actual);
    }

    /// <summary>In place is allowed by the header and has to stay allowed by the
    /// wrapper — a bounce that has to double the buffer is a different
    /// feature.</summary>
    [NativeAvailableFact]
    public void TheWrapper_ProcessesInPlace()
    {
        var input = Sine(220.0, Sr / 8);
        var expected = new float[input.Length];
        JuceAudioEngine.ProcessDropTuneOffline(input, expected, Sr, new[] { -5f }, new[] { input.Length }, 20f);

        var inPlace = (float[])input.Clone();
        JuceAudioEngine.ProcessDropTuneOffline(inPlace, inPlace, Sr, new[] { -5f }, new[] { inPlace.Length }, 20f);

        Assert.Equal(expected, inPlace);
    }

    /// <summary>
    /// The segment sum is checked managed-side because the failure it prevents
    /// is SILENT: a short sum leaves the tail of the output untouched, which on
    /// an in-place call is the original audio at the original pitch — a render
    /// that is plausible and wrong.
    /// </summary>
    [Fact]
    public void TheWrapper_RejectsArgumentsTheEngineWouldAcceptQuietlyOrNotAtAll()
    {
        var buf = new float[128];

        Assert.Throws<ArgumentException>(() =>
            JuceAudioEngine.ProcessDropTuneOffline(
                ReadOnlySpan<float>.Empty, Span<float>.Empty, Sr, new[] { 0f }, new[] { 0 }, 20f));

        Assert.Throws<ArgumentException>(() =>
            JuceAudioEngine.ProcessDropTuneOffline(buf, new float[64], Sr, new[] { 0f }, new[] { 128 }, 20f));

        Assert.Throws<ArgumentException>(() =>
            JuceAudioEngine.ProcessDropTuneOffline(buf, new float[128], Sr, new[] { 0f, -2f }, new[] { 128 }, 20f));

        // 64 + 32 != 128 — the silent-tail case.
        Assert.Throws<ArgumentException>(() =>
            JuceAudioEngine.ProcessDropTuneOffline(buf, new float[128], Sr, new[] { 0f, -2f }, new[] { 64, 32 }, 20f));

        Assert.Throws<ArgumentException>(() =>
            JuceAudioEngine.ProcessDropTuneOffline(buf, new float[128], Sr, new[] { 0f, -2f }, new[] { 192, -64 }, 20f));

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            JuceAudioEngine.ProcessDropTuneOffline(buf, new float[128], Sr, new[] { 0f }, new[] { 128 },
                (JuceDropTuneMode)99));
    }
}
