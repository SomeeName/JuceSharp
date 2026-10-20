using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     Pins for the span-taking <c>Push</c> overloads on
///     <see cref="JuceOnsetDetector" /> and <see cref="JuceGuitarAnalyzer" />, and for
///     the two latency constants a caller needs to align detections to its own input
///     timeline.
///     <para>
///         The equivalence half is the load-bearing one: the span overload is the SAME
///         code path with a different sink, so it must return exactly what the list
///         overload returns — position for position, strength bit for bit. If those
///         two ever diverge, one of them is the shipped detection path and the other
///         is a silent second implementation.
///     </para>
/// </summary>
[Collection("AllocationProbes")]
public sealed class StreamingSpanPushTests
{
    private const int Sr = 44100;

    /// <summary>Exponentially decaying 1 kHz clicks every <paramref name="gap"/> samples.</summary>
    private static float[] ClickTrack(int clicks, int gap, int lead = 4096)
    {
        var audio = new float[lead + clicks * gap];
        for (var c = 0; c < clicks; c++)
        {
            var start = lead + c * gap;
            for (var i = 0; i < 400 && start + i < audio.Length; i++)
                audio[start + i] = 0.8f * MathF.Exp(-i / 60f) * MathF.Sin(2f * MathF.PI * 1000f * i / Sr);
        }
        return audio;
    }

    private static List<JuceOnset> ViaList(ReadOnlySpan<float> audio, int block)
    {
        var det = new JuceOnsetDetector(Sr);
        var all = new List<JuceOnset>();
        for (var i = 0; i + block <= audio.Length; i += block) all.AddRange(det.Push(audio.Slice(i, block)));
        return all;
    }

    private static List<JuceOnset> ViaSpan(ReadOnlySpan<float> audio, int block, out long dropped)
    {
        var det = new JuceOnsetDetector(Sr);
        var scratch = new JuceOnset[JuceOnsetDetector.RequiredOnsetCapacity(block)];
        var all = new List<JuceOnset>();
        for (var i = 0; i + block <= audio.Length; i += block)
        {
            det.Push(audio.Slice(i, block), scratch, out var n);
            for (var k = 0; k < n; k++) all.Add(scratch[k]);
        }
        dropped = det.DroppedOnsets;
        return all;
    }

    [Theory]
    [InlineData(256)]
    [InlineData(1024)]
    [InlineData(777)]
    public void OnsetSpanOverload_MatchesTheListOverloadExactly(int block)
    {
        var audio = ClickTrack(12, (int)(0.35 * Sr));

        var list = ViaList(audio, block);
        var span = ViaSpan(audio, block, out var dropped);

        Assert.NotEmpty(list);
        Assert.Equal(0, dropped);
        Assert.Equal(list.Count, span.Count);
        for (var i = 0; i < list.Count; i++)
        {
            Assert.Equal(list[i].SamplePosition, span[i].SamplePosition);
            Assert.Equal(list[i].Strength, span[i].Strength);   // bit-for-bit, not approximately
        }
    }

    /// <summary>The reported delay is exact at hop-sized pushes: an impulse's onset
    /// comes out of <c>Push</c> exactly <see cref="JuceOnsetDetector.LatencySamples"/>
    /// samples past the position it reports.</summary>
    [Fact]
    public void OnsetLatencySamples_EqualsTheMeasuredReportDelay()
    {
        var audio = ClickTrack(1, Sr, lead: 8192);
        var det = new JuceOnsetDetector(Sr);
        var scratch = new JuceOnset[8];

        long reportedAt = -1, position = -1;
        for (var i = 0; i + JuceOnsetDetector.HopSize <= audio.Length; i += JuceOnsetDetector.HopSize)
        {
            det.Push(audio.AsSpan(i, JuceOnsetDetector.HopSize), scratch, out var n);
            if (n == 0 || reportedAt >= 0) continue;
            reportedAt = det.StreamPosition;     // samples consumed at the moment it came out
            position   = scratch[0].SamplePosition;
        }

        Assert.True(reportedAt > 0, "no onset was reported");
        Assert.Equal(JuceOnsetDetector.LatencySamples, reportedAt - position);
        Assert.Equal(896, JuceOnsetDetector.LatencySamples);
    }

    /// <summary>A span too small drops rather than throws, and says so. Non-vacuity for
    /// the capacity helper: at <see cref="JuceOnsetDetector.RequiredOnsetCapacity"/>
    /// nothing is dropped, at zero everything is.</summary>
    [Fact]
    public void OnsetOverflow_IsDroppedAndCounted()
    {
        var audio = ClickTrack(12, (int)(0.35 * Sr));
        var det = new JuceOnsetDetector(Sr);
        var tiny = new JuceOnset[0];
        var total = 0;
        for (var i = 0; i + 1024 <= audio.Length; i += 1024)
        {
            det.Push(audio.AsSpan(i, 1024), tiny, out var n);
            total += n;
        }

        Assert.Equal(0, total);
        Assert.Equal(ViaList(audio, 1024).Count, det.DroppedOnsets);
    }

    [Fact]
    public void OnsetSpanPush_DoesNotAllocate()
    {
        var audio = ClickTrack(40, (int)(0.20 * Sr));
        var det = new JuceOnsetDetector(Sr);
        var scratch = new JuceOnset[JuceOnsetDetector.RequiredOnsetCapacity(1024)];
        var seen = 0;

        det.Push(audio.AsSpan(0, 8192), scratch, out _);   // warm up

        var delta = AllocationProbe.Measure(() =>
        {
            for (var i = 8192; i + 1024 <= audio.Length; i += 1024)
            {
                det.Push(audio.AsSpan(i, 1024), scratch, out var n);
                seen += n;
            }
        });

        Assert.True(seen > 10, $"only {seen} onsets — the measured window carried no work");
        Assert.Equal(0, delta);
    }

    // ------------------------------------------------------------ analyzer

    private static float[] Pluck(int count, int gap, int lead = 8192)
    {
        var audio = new float[lead + count * gap + 2 * Sr];
        for (var p = 0; p < count; p++)
        {
            var start = lead + p * gap;
            for (var i = 0; i < gap && start + i < audio.Length; i++)
            {
                var env = MathF.Exp(-i / (0.25f * Sr));
                var t = (start + i) / (float)Sr;
                audio[start + i] = 0.6f * env *
                    (MathF.Sin(2f * MathF.PI * 196f * t) + 0.4f * MathF.Sin(2f * MathF.PI * 392f * t));
            }
        }
        return audio;
    }

    [Fact]
    public void GuitarAnalyzerSpanOverload_MatchesTheListOverloadExactly()
    {
        var audio = Pluck(6, (int)(0.5 * Sr));
        const int block = 1024;

        var a = new JuceGuitarAnalyzer(Sr);
        var list = new List<JuceGuitarEvent>();
        for (var i = 0; i + block <= audio.Length; i += block) list.AddRange(a.Push(audio.AsSpan(i, block)));

        var b = new JuceGuitarAnalyzer(Sr);
        var scratch = new JuceGuitarEvent[JuceGuitarAnalyzer.RequiredEventCapacity(block)];
        var span = new List<JuceGuitarEvent>();
        for (var i = 0; i + block <= audio.Length; i += block)
        {
            b.Push(audio.AsSpan(i, block), scratch, out var n);
            for (var k = 0; k < n; k++) span.Add(scratch[k]);
        }

        Assert.NotEmpty(list);
        Assert.Equal(0, b.DroppedEvents);
        Assert.Equal(list.Count, span.Count);
        for (var i = 0; i < list.Count; i++)
        {
            Assert.Equal(list[i].Kind, span[i].Kind);
            Assert.Equal(list[i].StreamSamplePosition, span[i].StreamSamplePosition);
            Assert.Equal(list[i].Rms, span[i].Rms);
            Assert.Equal(list[i].SpectralCentroidHz, span[i].SpectralCentroidHz);
            Assert.Equal(list[i].Techniques, span[i].Techniques);
            Assert.Equal(list[i].Chroma, span[i].Chroma);
        }
    }

    /// <summary>An event is emitted no later than one push block past
    /// <c>LatencySamples</c> from its own position — and no earlier than the refinement
    /// reach before it, because the delay is measured against the event's REFINED position
    /// while the pending queue releases on the raw detector position.</summary>
    [Fact]
    public void GuitarAnalyzerLatencySamples_BoundsTheMeasuredReportDelay()
    {
        var audio = Pluck(1, (int)(1.0 * Sr));
        const int block = 1024;
        var a = new JuceGuitarAnalyzer(Sr);
        var scratch = new JuceGuitarEvent[JuceGuitarAnalyzer.RequiredEventCapacity(block)];

        long reportedAt = -1, position = -1;
        for (var i = 0; i + block <= audio.Length; i += block)
        {
            a.Push(audio.AsSpan(i, block), scratch, out var n);
            if (n == 0 || reportedAt >= 0) continue;
            reportedAt = (long)a.StreamPosition;
            position   = (long)scratch[0].StreamSamplePosition;
        }

        Assert.True(reportedAt > 0, "no event was emitted");
        var measured = reportedAt - position;
        // RefineAttackPosition can move the reported position FORWARD by up to
        // `AnalysisWindow - PreOnsetLookback` from the one the queue released on, so the
        // measured delay is that much SHORTER at worst. This used to assert
        // `>= LatencySamples`, which only held while refinement happened not to move.
        const int refinementReach = 4096 - 256;
        Assert.InRange(measured, a.LatencySamples - refinementReach, a.LatencySamples + block);
        // The lag covers the refinement reach PLUS the furthest read past the refined
        // position (80 ms of decay delay + the 1024-sample short-RMS window) — not merely
        // the analysis window plus the decay delay, which finalized 768 samples early in
        // the worst case and read a zero-filled decay window. See W4-D2.
        Assert.Equal(refinementReach + (int)(0.080 * Sr) + 1024, a.LatencySamples);
    }

    /// <summary>The common case is a block that finalizes nothing; that path must not
    /// allocate, which it did before — the list overload's <c>IReadOnlyList</c>
    /// enumerator was BOXED once per 4096-sample chunk even when it was
    /// <c>Array.Empty</c>.</summary>
    [Fact]
    public void GuitarAnalyzerSpanPush_AllocatesNothingWhenNoEventsAreEmitted()
    {
        var a = new JuceGuitarAnalyzer(Sr);
        var quiet = new float[4096];
        for (var i = 0; i < quiet.Length; i++) quiet[i] = 1e-4f * MathF.Sin(2f * MathF.PI * 196f * i / Sr);
        var scratch = new JuceGuitarEvent[JuceGuitarAnalyzer.RequiredEventCapacity(4096)];

        for (var i = 0; i < 8; i++) a.Push(quiet, scratch, out _);   // warm up

        var events = 0;
        var delta = AllocationProbe.Measure(
            () => { for (var i = 0; i < 40; i++) { a.Push(quiet, scratch, out var n); events += n; } });

        Assert.Equal(0, events);
        Assert.Equal(0, delta);
    }
}
