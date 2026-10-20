using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceGuitarAnalyzerTests
{
    private const int Sr = 48000;

    /// <summary>Synthetic plucked string: 8 harmonics with 1/h amplitude,
    /// per-harmonic exponential decay, 3 ms linear attack, ~0.2 scale.</summary>
    private static float[] Pluck(float f0, int sr, double seconds, double decayPerSecBase = 2.0)
    {
        int n = (int)(seconds * sr);
        var s = new float[n];
        for (int h = 1; h <= 8; h++)
        {
            double amp   = 1.0 / h;
            double decay = decayPerSecBase + 0.7 * h;
            double w     = 2 * Math.PI * f0 * h / sr;
            for (int i = 0; i < n; i++)
            {
                double t      = (double)i / sr;
                double attack = Math.Min(1.0, t / 0.003);
                s[i] += (float)(0.2 * amp * attack * Math.Exp(-t * decay) * Math.Sin(w * i));
            }
        }
        return s;
    }

    private static void Mix(float[] dest, float[] src, int offset)
    {
        int n = Math.Min(src.Length, dest.Length - offset);
        for (int i = 0; i < n; i++) dest[offset + i] += src[i];
    }

    /// <summary>Linear fade over the final <paramref name="seconds"/> so the
    /// synthetic signal does not end in a click.</summary>
    private static void FadeOut(float[] s, int sr, double seconds = 0.02)
    {
        int n = Math.Min(s.Length, (int)(seconds * sr));
        for (int i = 0; i < n; i++)
            s[s.Length - n + i] *= 1f - (float)i / n;
    }

    /// <summary>Push the whole signal plus enough trailing silence to
    /// flush all queued events.</summary>
    private static List<JuceGuitarEvent> Analyze(JuceGuitarAnalyzer analyzer, float[] samples)
    {
        var events = new List<JuceGuitarEvent>();
        events.AddRange(analyzer.Push(samples));
        events.AddRange(analyzer.Push(new float[(int)(0.3 * analyzer.SampleRate)]));
        return events;
    }

    private static int PitchClass(int midi) => ((midi % 12) + 12) % 12;

    // ------------------------------------------------------------- (a)

    [Theory]
    [InlineData(82.41f, 40)]   // E2
    [InlineData(110f, 45)]     // A2
    [InlineData(146.83f, 50)]  // D3
    [InlineData(196f, 55)]     // G3
    [InlineData(246.94f, 59)]  // B3
    [InlineData(329.63f, 64)]  // E4
    public void SingleNote_ExactlyOneOnsetWithCorrectPitch(float f0, int expectedMidi)
    {
        int attackAt = (int)(0.05 * Sr);
        var sig = new float[(int)(0.7 * Sr)];
        Mix(sig, Pluck(f0, Sr, 0.65), attackAt);
        FadeOut(sig, Sr);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);

        Assert.Single(events);
        var ev = events[0];
        Assert.Equal(JuceGuitarEventKind.Onset, ev.Kind);
        Assert.NotEmpty(ev.Pitches);
        Assert.Equal(expectedMidi, ev.Pitches[0].Midi);

        long tolerance = (long)(0.015 * Sr);
        Assert.InRange((long)ev.StreamSamplePosition, attackAt - tolerance, attackAt + tolerance);
    }

    // ------------------------------------------------------------- (b)

    [Fact]
    public void PowerChord_OneOnsetContainingExpectedPitchClasses()
    {
        int attackAt = (int)(0.05 * Sr);
        var sig = new float[(int)(0.8 * Sr)];
        foreach (var f in new[] { 82.41f, 123.47f, 164.81f }) // E2 + B2 + E3
            Mix(sig, Pluck(f, Sr, 0.75), attackAt);
        FadeOut(sig, Sr);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.Single(onsets);
        var pcs = onsets[0].Pitches.Select(p => PitchClass(p.Midi)).ToHashSet();
        Assert.Contains(4, pcs);   // E
        Assert.Contains(11, pcs);  // B
    }

    // ------------------------------------------------------------- (c)

    [Fact]
    public void AmChord_FindsAtLeastTwoOfThreePitchClasses()
    {
        int attackAt = (int)(0.05 * Sr);
        var sig = new float[(int)(0.8 * Sr)];
        foreach (var f in new[] { 110f, 164.81f, 220f, 261.63f }) // A2 + E3 + A3 + C4
            Mix(sig, Pluck(f, Sr, 0.75), attackAt);
        FadeOut(sig, Sr);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.Single(onsets);
        var pcs = onsets[0].Pitches.Select(p => PitchClass(p.Midi)).ToHashSet();
        int hits = new[] { 9, 4, 0 }.Count(pcs.Contains);
        Assert.True(hits >= 2, $"expected >=2 of pcs {{9,4,0}}, got [{string.Join(",", pcs)}]");
    }

    // ------------------------------------------------------------- (d)

    [Fact]
    public void TwoPlucks_TwoOnsets300MsApart()
    {
        int first  = (int)(0.05 * Sr);
        int second = first + (int)(0.30 * Sr);
        var sig = new float[(int)(1.2 * Sr)];
        Mix(sig, Pluck(164.81f, Sr, 1.1), first);
        Mix(sig, Pluck(164.81f, Sr, 0.8), second);
        FadeOut(sig, Sr);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.Equal(2, onsets.Count);
        long gap = (long)onsets[1].StreamSamplePosition - (long)onsets[0].StreamSamplePosition;
        long expected = (long)(0.30 * Sr);
        long tolerance = (long)(0.020 * Sr);
        Assert.InRange(gap, expected - tolerance, expected + tolerance);
    }

    // ------------------------------------------------------------- (e)

    [Fact]
    public void PalmMute_FastDecayIsFlagged()
    {
        int attackAt = (int)(0.05 * Sr);
        var sig = new float[(int)(0.6 * Sr)];
        Mix(sig, Pluck(110f, Sr, 0.5, decayPerSecBase: 28.0), attackAt);
        FadeOut(sig, Sr);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.Single(onsets);
        Assert.True(onsets[0].Techniques.HasFlag(JuceGuitarTechniques.PalmMute),
            $"expected PalmMute, got {onsets[0].Techniques} (decay {onsets[0].DecayRatio}, centroid {onsets[0].SpectralCentroidHz})");
    }

    // ------------------------------------------------------------- (f)

    [Fact]
    public void NoiseBurst_IsFlaggedPercussive()
    {
        int attackAt = (int)(0.05 * Sr);
        var sig = new float[(int)(0.6 * Sr)];
        var rnd = new Random(1234);
        for (int i = 0; i < (int)(0.030 * Sr); i++)
            sig[attackAt + i] = (float)((rnd.NextDouble() * 2 - 1) * 0.15);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.Single(onsets);
        Assert.True(onsets[0].Techniques.HasFlag(JuceGuitarTechniques.Percussive),
            $"expected Percussive, got {onsets[0].Techniques} (flatness {onsets[0].SpectralFlatness})");
    }

    // ------------------------------------------------------------- (g)

    [Fact]
    public void StreamAccounting_RaggedPushesAndRebaseAreConsistent()
    {
        var sig = new float[(int)(1.0 * Sr)];
        Mix(sig, Pluck(164.81f, Sr, 0.95), (int)(0.05 * Sr));
        Mix(sig, Pluck(220f, Sr, 0.55), (int)(0.45 * Sr));
        FadeOut(sig, Sr);

        var single   = new JuceGuitarAnalyzer(Sr);
        var evSingle = single.Push(sig).ToList();
        Assert.Equal((ulong)sig.Length, single.StreamPosition);

        var ragged   = new JuceGuitarAnalyzer(Sr);
        var evRagged = new List<JuceGuitarEvent>();
        for (int i = 0; i < sig.Length; i += 331)
        {
            int n = Math.Min(331, sig.Length - i);
            evRagged.AddRange(ragged.Push(sig.AsSpan(i, n)));
        }
        Assert.Equal((ulong)sig.Length, ragged.StreamPosition);

        Assert.Equal(evSingle.Count, evRagged.Count);
        for (int i = 0; i < evSingle.Count; i++)
        {
            Assert.Equal(evSingle[i].Kind, evRagged[i].Kind);
            long delta = (long)evSingle[i].StreamSamplePosition
                       - (long)evRagged[i].StreamSamplePosition;
            Assert.True(Math.Abs(delta) <= 256,
                $"event {i} moved by {delta} samples between push patterns");
        }

        var rebased = new JuceGuitarAnalyzer(Sr);
        rebased.Reset(1_000_000);
        var evRebased = rebased.Push(sig).ToList();
        Assert.Equal(1_000_000UL + (ulong)sig.Length, rebased.StreamPosition);
        Assert.Equal(evSingle.Count, evRebased.Count);
        for (int i = 0; i < evSingle.Count; i++)
            Assert.Equal(evSingle[i].StreamSamplePosition + 1_000_000UL,
                         evRebased[i].StreamSamplePosition);
    }

    // ------------------------------------------------------------- (h)

    [Fact]
    public void Legato_CrossfadeToNewPitchEmitsLegatoWithoutExtraOnset()
    {
        // 400 ms G3 pluck, then a 10 ms crossfade into a sustained A3
        // "sine-ish" tone at similar amplitude — no fresh attack.
        int    n           = (int)(1.2 * Sr);
        double attackAt    = 0.05;
        double xfadeStart  = attackAt + 0.40;
        double xfadeLength = 0.010;

        var g3 = new float[n];
        Mix(g3, Pluck(196f, Sr, 1.1), (int)(attackAt * Sr));

        var sig = new float[n];
        for (int i = 0; i < n; i++)
        {
            double t = (double)i / Sr;
            double a = t < xfadeStart ? 0.0 : Math.Min(1.0, (t - xfadeStart) / xfadeLength);
            double tone = 0.08 * Math.Sin(2 * Math.PI * 220.0 * t)
                        + 0.02 * Math.Sin(2 * Math.PI * 440.0 * t);
            sig[i] = (float)((1.0 - a) * g3[i] + a * tone);
        }
        FadeOut(sig, Sr);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);

        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();
        Assert.True(onsets.Count == 1,
            $"expected only the initial pluck onset, got {onsets.Count} onsets at " +
            $"[{string.Join(", ", onsets.Select(o => o.StreamSeconds.ToString("F3")))}]");

        var legatos = events.Where(e =>
            e.Kind == JuceGuitarEventKind.Legato &&
            e.Pitches.Length > 0 &&
            PitchClass(e.Pitches[0].Midi) == 9).ToList();
        Assert.True(legatos.Count > 0, "expected a Legato event with pitch class A (9)");
        Assert.True(legatos[0].StreamSeconds > xfadeStart - 0.02,
            $"legato fired too early: {legatos[0].StreamSeconds:F3}s");
        Assert.True(legatos[0].Techniques.HasFlag(JuceGuitarTechniques.Legato));
    }

    // ------------------------------------------------------ noise gate

    [Fact]
    public void Silence_ProducesNoEvents()
    {
        var events = Analyze(new JuceGuitarAnalyzer(Sr), new float[(int)(1.5 * Sr)]);
        Assert.Empty(events);
    }

    [Fact]
    public void HissFloor_ContinuousStatic_ProducesNoEvents()
    {
        // Interface hiss / background static: uniform noise at ±0.008
        // (RMS ≈ 0.0046), well below any real attack but spectrally busy
        // enough to tickle the relative flux detector.
        var rnd = new Random(42);
        var sig = new float[(int)(2.0 * Sr)];
        for (int i = 0; i < sig.Length; i++)
            sig[i] = (float)((rnd.NextDouble() * 2 - 1) * 0.008);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        Assert.Empty(events);
    }

    [Fact]
    public void TinyStaticBursts_BelowAttackFloor_ProduceNoEvents()
    {
        // Electrical crackle: short noise bursts at ±0.012 (RMS ≈ 0.007).
        // Each burst is a textbook onset shape (silence → noise), so only
        // the absolute MinAttackRms floor can reject it.
        var rnd = new Random(7);
        var sig = new float[(int)(1.5 * Sr)];
        for (int burst = 0; burst < 4; burst++)
        {
            int at = (int)((0.1 + burst * 0.3) * Sr);
            for (int i = 0; i < (int)(0.030 * Sr); i++)
                sig[at + i] = (float)((rnd.NextDouble() * 2 - 1) * 0.012);
        }

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        Assert.Empty(events);
    }

    [Fact]
    public void HissFloor_RealPluckStillDetected()
    {
        // The gate must not eat real notes riding on top of the hiss.
        var rnd = new Random(99);
        var sig = new float[(int)(1.0 * Sr)];
        for (int i = 0; i < sig.Length; i++)
            sig[i] = (float)((rnd.NextDouble() * 2 - 1) * 0.004);
        int attackAt = (int)(0.3 * Sr);
        Mix(sig, Pluck(110f, Sr, 0.6), attackAt);
        FadeOut(sig, Sr);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.Single(onsets);
        Assert.NotEmpty(onsets[0].Pitches);
        Assert.Equal(45, onsets[0].Pitches[0].Midi); // A2
    }

    [Fact]
    public void MutedChugTrain_EveryOnsetIsFlaggedPercussive()
    {
        // Constant muted chugging: loud broadband bursts with no harmonic
        // structure. Every detected attack must carry the Percussive flag —
        // that flag is what vetoes chugs against clean chart notes.
        var rnd = new Random(1234);
        var sig = new float[(int)(1.6 * Sr)];
        for (int burst = 0; burst < 5; burst++)
        {
            int at = (int)((0.1 + burst * 0.25) * Sr);
            for (int i = 0; i < (int)(0.030 * Sr); i++)
                sig[at + i] = (float)((rnd.NextDouble() * 2 - 1) * 0.15);
        }

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.Equal(5, onsets.Count);
        Assert.All(onsets, o => Assert.True(
            o.Techniques.HasFlag(JuceGuitarTechniques.Percussive),
            $"chug at {o.StreamSeconds:F3}s not Percussive: {o.Techniques} (flatness {o.SpectralFlatness:F2})"));
    }

    // ------------------------------------------- ring-out suppression

    [Fact]
    public void RingOut_PreviousStringDoesNotDominateNewOnset()
    {
        // E3 plucked first and left ringing; A3 plucked 500 ms later on
        // top. Onset-synchronous spectral differencing must report A3 as
        // the top candidate and suppress the still-ringing E3.
        int first  = (int)(0.05 * Sr);
        int second = first + (int)(0.50 * Sr);
        var sig = new float[(int)(1.4 * Sr)];
        Mix(sig, Pluck(164.81f, Sr, 1.3), first);   // E3, keeps ringing
        Mix(sig, Pluck(220f, Sr, 0.8), second);     // A3 over the ring-out
        FadeOut(sig, Sr);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.Equal(2, onsets.Count);
        var a3Onset = onsets[1];
        Assert.NotEmpty(a3Onset.Pitches);
        Assert.True(a3Onset.Pitches[0].Midi == 57,
            $"expected top candidate A3 (57), got midi {a3Onset.Pitches[0].Midi} " +
            $"(all: [{string.Join(", ", a3Onset.Pitches.Select(p => $"{p.Midi}@{p.Salience:F2}"))}])");
        Assert.All(a3Onset.Pitches.Where(p => p.Midi == 52),
            p => Assert.True(p.Salience < 0.5f,
                $"ringing E3 scored salience {p.Salience:F2}, expected < 0.5"));
    }

    [Fact]
    public void RingOut_ChordOverRingingStringFindsChordPitchClasses()
    {
        // E2 ringing, then an Am-style chord (A2 + E3 + A3) on top: the
        // chord's pitch classes must come through the ring-out.
        int first  = (int)(0.05 * Sr);
        int second = first + (int)(0.50 * Sr);
        var sig = new float[(int)(1.4 * Sr)];
        Mix(sig, Pluck(82.41f, Sr, 1.3), first);    // E2, keeps ringing
        foreach (var f in new[] { 110f, 164.81f, 220f }) // A2 + E3 + A3
            Mix(sig, Pluck(f, Sr, 0.8), second);
        FadeOut(sig, Sr);

        var events = Analyze(new JuceGuitarAnalyzer(Sr), sig);
        var onsets = events.Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.Equal(2, onsets.Count);
        var chord = onsets[1];
        Assert.NotEmpty(chord.Pitches);
        var pcs = chord.Pitches.Select(p => PitchClass(p.Midi)).ToHashSet();
        Assert.Contains(9, pcs); // A must survive the E ring-out
        int topPc = PitchClass(chord.Pitches[0].Midi);
        Assert.True(topPc is 9 or 4,
            $"expected top candidate pc in {{9,4}}, got {topPc} " +
            $"(all: [{string.Join(", ", chord.Pitches.Select(p => $"{p.Midi}@{p.Salience:F2}"))}])");
    }
}
