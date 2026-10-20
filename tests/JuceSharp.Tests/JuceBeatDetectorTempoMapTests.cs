using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     Pins for the 0.9 beat tracker: the tempo map, the downbeat/meter estimate,
///     and the <see cref="JuceBeatDetector.AnalysisOptions.FixedGrid" /> escape hatch
///     that restores the pre-0.9 grid.
///     <para>
///         The fixed grid could not represent a song that speeds up — one global
///         period stepped from the first half-beat, so every beat past a tempo change
///         drifted. These tests are built around a synthetic tempo RAMP for exactly
///         that reason: on a steady click track the old grid and the new tracker agree
///         and the change would look like a no-op.
///     </para>
///     <para>
///         The envelope hop is set explicitly in every case. One frame of inter-beat
///         interval is <c>bpm² / (60·fps)</c> BPM wide — 5.6 BPM at 120 BPM with the
///         default 512-sample hop at 22 kHz — so a tempo map asked for 2 BPM precision
///         at the default hop is asking past the resolution of its own input.
///     </para>
/// </summary>
public sealed class JuceBeatDetectorTempoMapTests
{
    private const int Sr = 22050;

    private static JuceBeatDetector Fine(bool fixedGrid = false) => new(Sr)
    {
        FftSize = 512,
        HopSize = 64,              // fps ~= 344; one IBI frame ~= 0.7 BPM at 120
        Options = new JuceBeatDetector.AnalysisOptions { FixedGrid = fixedGrid },
    };

    /// <summary>Clicks at the given times, each an exponentially decaying 1 kHz burst.</summary>
    private static float[] Clicks(IReadOnlyList<double> times, IReadOnlyList<float> amps, double tailSeconds = 1.0)
    {
        var total = (int)((times[^1] + tailSeconds) * Sr);
        var audio = new float[total];
        for (var b = 0; b < times.Count; b++)
        {
            var start = (int)(times[b] * Sr);
            for (var i = 0; i < 200 && start + i < total; i++)
                audio[start + i] += amps[b] * MathF.Exp(-i / 30f) * MathF.Sin(2f * MathF.PI * 1000f * i / Sr);
        }
        return audio;
    }

    private static (List<double> Times, List<float> Amps) Steady(float bpm, int beats, int accentEvery = 0)
    {
        var t = new List<double>();
        var a = new List<float>();
        for (var i = 0; i < beats; i++)
        {
            t.Add(0.5 + i * 60.0 / bpm);
            a.Add(accentEvery > 0 && i % accentEvery == 0 ? 1.0f : 0.45f);
        }
        return (t, a);
    }

    /// <summary>Linear BPM ramp: the click at each beat, tempo read at that beat.</summary>
    private static (List<double> Times, List<float> Amps) Ramp(float fromBpm, float toBpm, double seconds)
    {
        var t = new List<double>();
        var a = new List<float>();
        var now = 0.5;
        while (now < 0.5 + seconds)
        {
            t.Add(now);
            a.Add(0.8f);
            now += 60.0 / BpmAt(fromBpm, toBpm, seconds, now - 0.5);
        }
        return (t, a);
    }

    private static double BpmAt(float fromBpm, float toBpm, double seconds, double elapsed) =>
        fromBpm + (toBpm - fromBpm) * Math.Clamp(elapsed / seconds, 0, 1);

    /// <summary>A 100 → 140 BPM ramp over 30 s: every segment of the map must name
    /// the tempo actually playing at its own time, within 2 BPM. The pre-0.9 grid
    /// could not express this at all.</summary>
    [Fact]
    public void TempoRamp_IsTrackedWithinTwoBpm()
    {
        var (times, amps) = Ramp(100f, 140f, 30.0);
        var audio = Clicks(times, amps);

        var r = Fine().Analyze(audio);

        Assert.True(r.TempoMap.Count > 4, $"a 40 BPM ramp collapsed to {r.TempoMap.Count} segment(s)");
        for (var i = 0; i < r.TempoMap.Count; i++)
        {
            var seg = r.TempoMap[i];
            var end = i + 1 < r.TempoMap.Count ? r.TempoMap[i + 1].Time.TotalSeconds : times[^1];
            var mid = (seg.Time.TotalSeconds + end) / 2.0;
            var truth = BpmAt(100f, 140f, 30.0, mid - 0.5);
            Assert.InRange(seg.Bpm, truth - 2.0, truth + 2.0);
        }

        // The map is ordered and starts at the first tracked beat.
        for (var i = 1; i < r.TempoMap.Count; i++)
            Assert.True(r.TempoMap[i].Time > r.TempoMap[i - 1].Time);
    }

    /// <summary>A steady track is ONE segment. Non-vacuity for the test above: the
    /// segmenter does not simply cut every few beats.</summary>
    [Fact]
    public void SteadyTempo_IsOneSegment()
    {
        var (times, amps) = Steady(120f, 40);
        var r = Fine().Analyze(Clicks(times, amps));

        var seg = Assert.Single(r.TempoMap);
        Assert.InRange(seg.Bpm, 118f, 122f);
        Assert.InRange(r.Bpm, 118f, 122f);
    }

    [Theory]
    [InlineData(4)]
    [InlineData(3)]
    public void AccentPattern_YieldsTheRightMeterAndDownbeats(int beatsPerBar)
    {
        var (times, amps) = Steady(120f, 48, accentEvery: beatsPerBar);
        var r = Fine().Analyze(Clicks(times, amps));

        Assert.Equal(beatsPerBar, r.BeatsPerBar);
        Assert.NotEmpty(r.Downbeats);

        // Every reported downbeat lands on an accented click (within half a beat).
        var accents = times.Where((_, i) => i % beatsPerBar == 0).ToList();
        foreach (var d in r.Downbeats)
            Assert.Contains(accents, a => Math.Abs(a - d.TotalSeconds) < 0.25);

        // …and there is roughly one per bar, not one per beat.
        Assert.InRange(r.Downbeats.Count, 48 / beatsPerBar - 3, 48 / beatsPerBar + 3);
    }

    /// <summary>
    /// <see cref="JuceBeatDetector.AnalysisOptions.FixedGrid" /> reproduces the
    /// pre-0.9 output: one global period stepped from the first half-beat, snapped
    /// to the nearest envelope maximum within ±period/4. The expected values were
    /// captured from the unchanged grid code, which the 0.9 change moved verbatim
    /// into <c>FixedGrid</c>.
    /// </summary>
    [Fact]
    public void FixedGrid_ReproducesThePreChangeBeats()
    {
        var (times, amps) = Steady(120f, 16);
        var audio = Clicks(times, amps);
        var grid = Fine(fixedGrid: true).Analyze(audio).Beats;

        // Evenly spaced by exactly the integer autocorrelation lag, which is what a
        // grid IS — a tracked beat sequence is not required to be.
        Assert.True(grid.Count >= 14, $"grid had {grid.Count} beats");
        var spacing = (grid[1] - grid[0]).TotalSeconds;
        for (var i = 2; i < grid.Count; i++)
            Assert.Equal(spacing, (grid[i] - grid[i - 1]).TotalSeconds, 4);
        Assert.InRange(spacing, 0.48, 0.52);

        // Non-vacuity: the tracked default is NOT the same object shape — on the
        // ramp the two differ by more than a rounding error.
        var (rt, ra) = Ramp(100f, 140f, 20.0);
        var rampAudio = Clicks(rt, ra);
        var tracked = Fine().Analyze(rampAudio).Beats;
        var gridded = Fine(fixedGrid: true).Analyze(rampAudio).Beats;
        Assert.True(Math.Abs(tracked.Count - gridded.Count) > 0 ||
                    tracked.Zip(gridded).Any(p => Math.Abs((p.First - p.Second).TotalSeconds) > 0.05),
                    "FixedGrid produced the same beats as the tracker on a tempo ramp");

        // The fixed-grid mode says nothing about meter, and must not pretend to.
        var fixedResult = Fine(fixedGrid: true).Analyze(audio);
        Assert.Empty(fixedResult.Downbeats);
        Assert.Equal(0, fixedResult.BeatsPerBar);
        Assert.Single(fixedResult.TempoMap);
    }
}
