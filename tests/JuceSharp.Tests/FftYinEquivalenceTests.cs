using System.Diagnostics;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     Statistical equivalence of <see cref="JuceChromaticTuner.UseFftDifference" />
///     against the direct YIN difference function, plus the same check one level up
///     on <see cref="JuceGuitarAnalyzer" />'s event stream.
///     <para>
///         The FFT form is NOT bit-identical — the FFT's rounding error is relative
///         to the window's total energy while <c>d(τ)</c> near its dip is close to
///         zero — so it cannot be pinned with <c>Assert.Equal</c>. It is pinned the
///         only honest way instead: over a spread of inputs, the detected frequency
///         must agree to well under half a cent and the confidence to 0.01. That is
///         what makes it safe to offer as an option, and stating a TOLERANCE rather
///         than assuming identity is the point.
///     </para>
///     <para>
///         The speed-up is measured and LOGGED, never asserted: a wall-clock
///         assertion flakes the moment two suites run at once.
///     </para>
/// </summary>
public sealed class FftYinEquivalenceTests
{
    private const int Sr = 44100;
    private readonly ITestOutputHelper _out;

    public FftYinEquivalenceTests(ITestOutputHelper output) => _out = output;

    private static float[] Tone(int length, double hz, double phase, double harmonic2, double harmonic3)
    {
        var buf = new float[length];
        for (var i = 0; i < length; i++)
        {
            var t = 2.0 * Math.PI * hz * i / Sr + phase;
            buf[i] = (float)(0.5 * Math.Sin(t)
                           + harmonic2 * Math.Sin(2 * t)
                           + harmonic3 * Math.Sin(3 * t));
        }
        return buf;
    }

    /// <summary>200 random guitar-range tones with random phase and harmonic mix.
    /// Both tuners must find the same note, within half a cent and 0.01 confidence.</summary>
    [Fact]
    public void FftDifference_AgreesWithTheDirectSum_OnTwoHundredRandomTones()
    {
        const int window = 2048;
        var rng    = new Random(20260911);          // fixed seed: a flaky eval is not an eval
        var direct = new JuceChromaticTuner(Sr, window, minFreq: 50f);
        var fast   = new JuceChromaticTuner(Sr, window, minFreq: 50f, useFftDifference: true);

        var compared = 0;
        double worstCents = 0, worstConfidence = 0;
        for (var trial = 0; trial < 200; trial++)
        {
            // 82 Hz (low E) .. 1000 Hz, log-uniform.
            var hz = 82.0 * Math.Pow(1000.0 / 82.0, rng.NextDouble());
            var buf = Tone(window, hz, rng.NextDouble() * Math.PI * 2, rng.NextDouble() * 0.3, rng.NextDouble() * 0.2);

            var a = direct.Detect(buf);
            var b = fast.Detect(buf);
            Assert.Equal(a.HasValue, b.HasValue);
            if (a is not { } x || b is not { } y) continue;

            compared++;
            var cents = Math.Abs(1200.0 * Math.Log2(y.FrequencyHz / x.FrequencyHz));
            worstCents      = Math.Max(worstCents, cents);
            worstConfidence = Math.Max(worstConfidence, Math.Abs(x.Confidence - y.Confidence));
            Assert.Equal(x.NoteName, y.NoteName);
            Assert.Equal(x.Octave, y.Octave);
            Assert.Equal(x.Clamped, y.Clamped);
        }

        _out.WriteLine($"compared {compared}/200 tones; worst |Δ| = {worstCents:F4} cents, " +
                       $"{worstConfidence:F5} confidence");
        Assert.True(compared >= 190, $"only {compared} of 200 tones were detected at all");
        Assert.True(worstCents < 0.5, $"worst disagreement {worstCents:F4} cents");
        Assert.True(worstConfidence < 0.01, $"worst confidence disagreement {worstConfidence:F5}");
    }

    /// <summary>The same over the synthetic shapes the existing tuner tests use —
    /// a pure A4 at 4096, a bass low E (which CLAMPS on a short window), and a
    /// decaying pluck — where the dip geometry is different from a steady tone.</summary>
    [Theory]
    [InlineData(440f, 4096)]
    [InlineData(110f, 2048)]
    [InlineData(41.2f, 2048)]   // below the window's lag ceiling: both must clamp identically
    [InlineData(41.2f, 8192)]
    [InlineData(196f, 4096)]
    public void FftDifference_AgreesOnTheSyntheticCorpus(float hz, int window)
    {
        var direct = new JuceChromaticTuner(Sr, window, minFreq: 25f);
        var fast   = new JuceChromaticTuner(Sr, window, minFreq: 25f, useFftDifference: true);

        var buf = new float[window];
        for (var i = 0; i < window; i++)
            buf[i] = 0.6f * MathF.Exp(-i / (0.6f * Sr)) * MathF.Sin(2f * MathF.PI * hz * i / Sr);

        var a = direct.Detect(buf);
        var b = fast.Detect(buf);
        Assert.Equal(a.HasValue, b.HasValue);
        if (a is null)
            // Never a silent `return`. Both paths agreeing on "nothing here" is a real
            // result, but it leaves the three assertions below UNRUN — so the case has
            // to say so rather than report as a pass that proved an equivalence it
            // never looked at. All five InlineData rows are expected to detect; a skip
            // here means one of them stopped, which is itself the news.
            Assert.Skip($"neither difference path detected {hz} Hz at window {window}, " +
                        "so the equivalence assertions did not run");
        var x = a!.Value;
        var y = b!.Value;

        var cents = Math.Abs(1200.0 * Math.Log2(y.FrequencyHz / x.FrequencyHz));
        _out.WriteLine($"{hz} Hz / {window}: direct {x.FrequencyHz:F4} Hz, fft {y.FrequencyHz:F4} Hz, " +
                       $"Δ {cents:F4} cents, clamped {x.Clamped}");
        Assert.Equal(x.Clamped, y.Clamped);
        Assert.True(cents < 0.5, $"{cents:F4} cents apart");
        Assert.True(Math.Abs(x.Confidence - y.Confidence) < 0.01);
    }

    /// <summary>
    /// Wave-2 audit finding #10: the case the 200-sine corpus structurally cannot reach.
    /// <para>The FFT difference clamps a negative rounding error to EXACTLY zero, and
    /// <c>d̃(τ) == 0</c> is the strongest possible crossing of <c>Threshold</c> —
    /// <c>confidence = 1 - 0 = 1.0</c>. On a DC or near-DC window (a common interface
    /// artefact) every true <c>d(τ)</c> is ≈0 while the energy gate still passes, so the
    /// first τ to contribute positive noise makes <c>runningSum &gt; 0</c> and the next
    /// clamped τ reported a rock-solid bogus period. The direct path returns 1f there
    /// and correctly finds nothing. Every input in the corpus above is a guitar-range
    /// sine, which is precisely where the two paths agree.</para>
    /// </summary>
    [Theory]
    [InlineData(0.5f, 0f)]        // pure DC
    [InlineData(-0.9f, 0f)]       // a negative offset — same shape
    [InlineData(0.5f, 0.5f)]      // a 0.5 Hz drift on top: "near DC"
    [InlineData(0.02f, 2f)]       // a small, slow wander well below MinFrequency
    public void FftDifference_OnADcWindow_AgreesWithTheDirectSum(float offset, float driftHz)
    {
        const int window = 2048;
        var buf = new float[window];
        for (var i = 0; i < window; i++)
            buf[i] = offset + (driftHz > 0f ? 0.02f * MathF.Sin(2f * MathF.PI * driftHz * i / Sr) : 0f);

        var a = new JuceChromaticTuner(Sr, window, minFreq: 50f).Detect(buf);
        var b = new JuceChromaticTuner(Sr, window, minFreq: 50f, useFftDifference: true).Detect(buf);

        _out.WriteLine($"offset {offset}, drift {driftHz} Hz: " +
                       $"direct {(a.HasValue ? $"{a.Value.FrequencyHz:F2} Hz @ {a.Value.Confidence:F3}" : "null")}, " +
                       $"fft {(b.HasValue ? $"{b.Value.FrequencyHz:F2} Hz @ {b.Value.Confidence:F3}" : "null")}");

        Assert.Equal(a.HasValue, b.HasValue);
        Assert.Null(a);
        Assert.Null(b);
    }

    /// <summary>
    ///     What the FFT form actually buys, at the window and hop
    ///     <see cref="JuceGuitarAnalyzer" />'s legato tracker runs. <b>The TIMING
    ///     is logged, never asserted</b> — a wall clock in the gate flakes as soon
    ///     as two suites run at once. What IS asserted is CORRECTNESS: the fast
    ///     form must agree with the direct one on the pitch it reports.
    ///     <para>
    ///         This used to end in <c>Assert.True(fastMs &gt; 0)</c>, which a
    ///         <see cref="Stopwatch" /> elapsed always satisfies — 800 detections
    ///         in the default gate asserting nothing at all.
    ///     </para>
    /// </summary>
    [Fact]
    public void FftDifference_MatchesTheDirectForm_AndItsSpeedupIsReported()
    {
        const int window = 2048, reps = 100;
        var buf = Tone(window, 196.0, 0.0, 0.25, 0.1);
        var direct = new JuceChromaticTuner(Sr, window, minFreq: 30f);
        var fast   = new JuceChromaticTuner(Sr, window, minFreq: 30f, useFftDifference: true);

        for (var i = 0; i < 50; i++) { direct.Detect(buf); fast.Detect(buf); }   // JIT

        var sw = Stopwatch.StartNew();
        for (var i = 0; i < reps; i++) direct.Detect(buf);
        var directMs = sw.Elapsed.TotalMilliseconds;

        sw.Restart();
        for (var i = 0; i < reps; i++) fast.Detect(buf);
        var fastMs = sw.Elapsed.TotalMilliseconds;

        _out.WriteLine($"window {window}, {reps} detections: direct {directMs:F1} ms " +
                       $"({directMs / reps:F3} ms/call), fft {fastMs:F1} ms ({fastMs / reps:F3} ms/call) " +
                       $"=> x{directMs / Math.Max(fastMs, 1e-6):F2}");

        // The assertion is on the ANSWER, not the clock. A 196 Hz tone: both
        // forms must find it, and must agree.
        var a = direct.Detect(buf);
        var b = fast.Detect(buf);
        Assert.NotNull(a);
        Assert.NotNull(b);
        Assert.InRange(Math.Abs(a!.Value.FrequencyHz - b!.Value.FrequencyHz), 0.0, 0.1);
        Assert.InRange((double)a.Value.FrequencyHz, 195.0, 197.0);
    }

    /// <summary>
    /// One level up: the analyzer's whole event stream on a synthetic pluck corpus,
    /// direct against fast. Positions and techniques must match exactly (the onset
    /// detector is untouched by this option) and pitch candidates within half a
    /// cent.
    /// </summary>
    [Fact]
    public void GuitarAnalyzer_EventsMatchWithinTolerance_WithFastYin()
    {
        var audio = PluckCorpus();
        var slow = Collect(new JuceGuitarAnalyzer(Sr));
        var fast = Collect(new JuceGuitarAnalyzer(Sr, useFastYin: true));

        Assert.NotEmpty(slow);
        Assert.Equal(slow.Count, fast.Count);
        double worstCents = 0;
        for (var i = 0; i < slow.Count; i++)
        {
            Assert.Equal(slow[i].Kind, fast[i].Kind);
            Assert.Equal(slow[i].StreamSamplePosition, fast[i].StreamSamplePosition);
            Assert.Equal(slow[i].Techniques, fast[i].Techniques);
            Assert.Equal(slow[i].Pitches.Length, fast[i].Pitches.Length);
            for (var k = 0; k < slow[i].Pitches.Length; k++)
            {
                Assert.Equal(slow[i].Pitches[k].Midi, fast[i].Pitches[k].Midi);
                var cents = Math.Abs(1200.0 * Math.Log2(
                    fast[i].Pitches[k].FrequencyHz / slow[i].Pitches[k].FrequencyHz));
                worstCents = Math.Max(worstCents, cents);
            }
        }
        _out.WriteLine($"{slow.Count} events; worst pitch |Δ| = {worstCents:F4} cents");
        Assert.True(worstCents < 0.5, $"worst pitch disagreement {worstCents:F4} cents");

        // Non-vacuity: the option really did change which tuner ran.
        Assert.False(new JuceGuitarAnalyzer(Sr).UseFastYin);
        Assert.True(new JuceGuitarAnalyzer(Sr, useFastYin: true).UseFastYin);

        static List<JuceGuitarEvent> Collect(JuceGuitarAnalyzer a)
        {
            var audio = PluckCorpus();
            var all = new List<JuceGuitarEvent>();
            for (var i = 0; i + 1024 <= audio.Length; i += 1024) all.AddRange(a.Push(audio.AsSpan(i, 1024)));
            return all;
        }
    }

    private static float[] PluckCorpus()
    {
        float[] notes = [82.41f, 110f, 146.83f, 196f, 246.94f, 329.63f];
        var gap = (int)(0.45 * Sr);
        var audio = new float[8192 + notes.Length * gap + Sr];
        for (var n = 0; n < notes.Length; n++)
        {
            var start = 8192 + n * gap;
            for (var i = 0; i < gap && start + i < audio.Length; i++)
            {
                var env = MathF.Exp(-i / (0.28f * Sr));
                var t = 2f * MathF.PI * notes[n] * i / Sr;
                audio[start + i] = 0.6f * env * (MathF.Sin(t) + 0.35f * MathF.Sin(2 * t) + 0.15f * MathF.Sin(3 * t));
            }
        }
        return audio;
    }
}
