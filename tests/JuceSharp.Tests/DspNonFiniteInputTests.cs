using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The managed detection path must scrub non-finite input, exactly as the native
/// <c>DspChain::process</c> does at its input and after every stage.
///
/// <para><b>Why a NaN is worse than it looks.</b> Every gate in
/// <see cref="JuceChartVerifier"/> is written as "reject when the level is at or below the
/// threshold" — and <c>NaN &lt;= x</c> is <b>false</b>, so a NaN does not fail the per-bin
/// floor gate or the anti-phantom presence gate, it <b>passes</b> both. It then persists: in
/// the CQT's raw ring for the widest kernel, in the frame ring for 384 frames, and in the
/// noise history for the whole 10 s horizon, where a NaN percentile leaves the presence gate
/// — "the thing that stops confirms while nothing is played" — wide open for ten seconds.
/// Silently: no log, no exception, no counter. Hence <c>NonFiniteSamples</c>.</para>
///
/// <para>One glitched sample out of a decoder, a corrupt WAV, or a divide-by-zero in an
/// upstream stem separator is all it takes. All synthetic; no dataset.</para>
/// </summary>
public sealed class DspNonFiniteInputTests
{
    private const int Sr = 44100;

    private static JuceConstantQ NewLiveCqt() => new(
        Sr, hop: LiveCqtConfig.Hop, fMinMidi: LiveCqtConfig.MinMidi, binCount: LiveCqtConfig.BinCount,
        maxKernelSamples: LiveCqtConfig.MaxKernelSamples,
        noiseHistoryFrames: LiveCqtConfig.NoiseHistoryFrames(Sr));

    /// <summary>Quiet input noise with a burst of non-finite samples scattered through it —
    /// the shape of a decoder glitch.</summary>
    private static float[] NoiseWithNonFinite(double seconds, out int injected)
    {
        var rng = new Random(4242);
        var y = new float[(int)(seconds * Sr)];
        for (var i = 0; i < y.Length; i++) y[i] = (float)(rng.NextDouble() * 2 - 1) * 0.01f;

        float[] poison = { float.NaN, float.PositiveInfinity, float.NegativeInfinity };
        injected = 0;
        for (var i = 6_000; i < y.Length; i += 9_000) y[i] = poison[injected++ % poison.Length];
        return y;
    }

    [Fact]
    public void Cqt_Push_ScrubsNonFiniteInput_AndCountsIt()
    {
        var y = NoiseWithNonFinite(2.0, out var injected);
        Assert.True(injected > 8, "the fixture must inject a real burst");

        var cqt = NewLiveCqt();
        cqt.Reset(0);
        for (var off = 0; off < y.Length; off += 512)
            cqt.Push(y.AsSpan(off, Math.Min(512, y.Length - off)), off);

        Assert.Equal(injected, cqt.NonFiniteSamples);

        // Nothing non-finite may reach ANY read of the transform.
        for (var f = cqt.OldestFrame; f <= cqt.NewestFrame; f++)
        {
            Assert.True(float.IsFinite(cqt.FrameEnergy(f)), $"frame energy not finite at {f}");
            for (var b = 0; b < cqt.BinCount; b++)
                Assert.True(float.IsFinite(cqt.Magnitude(b, f)), $"magnitude not finite at bin {b}, frame {f}");
        }
        for (var b = 0; b < cqt.BinCount; b++)
            Assert.True(float.IsFinite(cqt.Floor(b)), $"floor not finite at bin {b}");

        // The one that matters most: a NaN in the noise history makes every later
        // `presence <= NoiseMargin * NoiseFloorEnergy()` false — the gate stops gating.
        Assert.True(float.IsFinite(cqt.NoiseFloorEnergy()), "noise floor not finite");
        Assert.True(cqt.NoiseFloorEnergy() >= 0f);
    }

    [Fact]
    public void Verifier_OverNonFiniteInput_StillRejectsPhantomsAndReportsFiniteConfidence()
    {
        // Nothing is ever played — only quiet noise with a glitch burst in it.
        var y = NoiseWithNonFinite(2.0, out _);

        var cqt = NewLiveCqt();
        cqt.Reset(0);
        for (var off = 0; off < y.Length; off += 512)
            cqt.Push(y.AsSpan(off, Math.Min(512, y.Length - off)), off);

        var v = new JuceChartVerifier(cqt)
            { NoiseMargin = LiveCqtConfig.NoiseMargin, SubOctaveVeto = LiveCqtConfig.SubOctaveVeto };

        // Probe right across the resident ring, so the glitched frames are covered rather
        // than hoped-over.
        var phantom = 0;
        var probes = 0;
        for (var f = cqt.OldestFrame + 20; f < cqt.NewestFrame - 20; f += 7)
        {
            var t = (f * LiveCqtConfig.Hop) / (double)Sr;
            for (var midi = LiveCqtConfig.MinMidi; midi <= 76; midi += 6)
            {
                var r = v.Verify(midi, t);
                probes++;
                if (r.Hit) phantom++;
                // Math.Clamp(NaN, 0, 1) is NaN — a NaN level propagates straight into the score.
                Assert.True(float.IsFinite(r.Confidence), $"confidence not finite for MIDI {midi} at frame {f}");
                Assert.True(float.IsFinite(r.Level), $"level not finite for MIDI {midi} at frame {f}");
            }
        }

        Assert.True(probes > 200, $"probed too little: {probes}");
        Assert.Equal(0, phantom);
    }

    [Fact]
    public void GuitarAnalyzer_Push_ScrubsNonFiniteInput_AndKeepsEventFeaturesFinite()
    {
        var (clean, _) = Synth.RenderNotes(new[] { 45, 52, 59 }, Sr, noteDur: 0.5, gap: 0.12);

        var baseline = Collect(clean, out var cleanNonFinite);
        Assert.Equal(0, cleanNonFinite);
        Assert.NotEmpty(baseline);

        // Poison a handful of samples right where the analysis windows live.
        var glitched = (float[])clean.Clone();
        var injected = 0;
        for (var i = 5_000; i < glitched.Length; i += 12_000)
        {
            glitched[i] = injected % 2 == 0 ? float.NaN : float.PositiveInfinity;
            injected++;
        }

        var events = Collect(glitched, out var nonFinite);
        Assert.Equal(injected, nonFinite);

        // The damage stays local to the zeroed samples: the same notes are still found …
        Assert.Equal(baseline.Count, events.Count);
        // … and nothing non-finite escapes into an event's features or pitches.
        foreach (var e in events)
        {
            Assert.True(float.IsFinite(e.Rms));
            Assert.True(float.IsFinite(e.CrestFactor));
            Assert.True(float.IsFinite(e.SpectralCentroidHz));
            Assert.True(float.IsFinite(e.SpectralFlatness));
            Assert.True(float.IsFinite(e.DecayRatio));
            Assert.All(e.Chroma, c => Assert.True(float.IsFinite(c)));
            foreach (var p in e.Pitches)
            {
                Assert.True(float.IsFinite(p.FrequencyHz));
                Assert.True(float.IsFinite(p.Salience));
                Assert.InRange(p.Midi, 0, 127);   // (int)MathF.Round(NaN) is int.MinValue
            }
        }
    }

    private static List<JuceGuitarEvent> Collect(float[] audio, out long nonFinite)
    {
        var analyzer = new JuceGuitarAnalyzer(Sr);
        var events = new List<JuceGuitarEvent>();
        for (var off = 0; off < audio.Length; off += 1024)
            events.AddRange(analyzer.Push(audio.AsSpan(off, Math.Min(1024, audio.Length - off))));
        nonFinite = analyzer.NonFiniteSamples;
        return events;
    }
}
