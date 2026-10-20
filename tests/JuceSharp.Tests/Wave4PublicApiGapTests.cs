using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The two API gaps the examples wave hit: <see cref="Windows"/> was internal, so an
/// application feeding <see cref="JuceFft"/> directly had to hand-roll a seventh copy of
/// the Hann loops this assembly exists to own; and <see cref="JuceTunerFrame"/> was the one
/// frame type with no timestamp, while <see cref="JuceLeadFrame.TimeSeconds"/> and
/// <see cref="JuceNoteEvent.TimestampSeconds"/> both carry one.
/// </summary>
public sealed class Wave4PublicApiGapTests
{
    /// <summary>
    /// <see cref="Windows.Hann"/> must be reachable from outside the assembly — which is
    /// what this file calling it proves — and must still be the cached, shared,
    /// symmetric-by-default window its own doc describes.
    /// </summary>
    [Fact]
    public void Windows_Hann_IsPublic_CachedAndCorrect()
    {
        Assert.True(typeof(Windows).IsPublic, "Windows must be part of the public surface");

        var w = Windows.Hann(8);
        Assert.Same(w, Windows.Hann(8));                       // cached, shared
        Assert.NotSame(w, Windows.Hann(8, periodic: true));    // keyed on BOTH

        // Symmetric (N-1): zero at both ends, 1.0 at the centre of an odd window.
        Assert.Equal(0f, w[0], 6);
        Assert.Equal(0f, w[^1], 6);
        Assert.Equal(1f, Windows.Hann(9)[4], 6);

        // Periodic (N): zero at the start only — the form that overlap-adds to a constant.
        var p = Windows.Hann(8, periodic: true);
        Assert.Equal(0f, p[0], 6);
        Assert.True(p[^1] > 0f, "the periodic form must not be zero at its last sample");

        Assert.Throws<ArgumentOutOfRangeException>(() => Windows.Hann(0));
    }

    /// <summary>
    /// <see cref="JuceTunerFrame.TimeSeconds"/> is the START of the analysis window, derived
    /// from the stream position — so consecutive frames are exactly
    /// <c>HopSize / SampleRate</c> apart, the first is 0, and none of it depends on the
    /// block sizes the caller happens to push. (<c>StreamPosition</c> cannot answer this:
    /// <c>Push</c> advances it by the whole block BEFORE processing any of it.)
    /// </summary>
    [Fact]
    public void TunerFrame_TimeSeconds_TracksTheWindowStart_NotTheCallersBlocks()
    {
        const int sr = 44100, window = 2048, hop = 512;
        var stream = new JuceTunerStream(sr, window, hopSize: hop);

        var tone = new float[sr];
        for (var i = 0; i < tone.Length; i++)
            tone[i] = 0.5f * MathF.Sin(2f * MathF.PI * 220f * i / sr);

        // Push in awkward, uneven blocks — the timestamps must not notice.
        var times = new List<double>();
        var offset = 0;
        var sizes = new[] { 777, 1234, 512, 4096, 333 };
        var s = 0;
        while (offset < tone.Length)
        {
            var n = Math.Min(sizes[s++ % sizes.Length], tone.Length - offset);
            if (stream.Push(tone.AsSpan(offset, n)) && stream.Latest is { } f) times.Add(f.TimeSeconds);
            offset += n;
        }

        Assert.NotEmpty(times);
        Assert.Equal(0.0, times[0], 9);
        // Every recorded time is a whole number of hops from the start.
        var hopSeconds = hop / (double)sr;
        foreach (var t in times)
            Assert.Equal(0.0, Math.IEEERemainder(t, hopSeconds), 9);
        // …and it advances monotonically, never past the audio that produced it.
        for (var i = 1; i < times.Count; i++) Assert.True(times[i] > times[i - 1]);
        Assert.True(times[^1] <= (tone.Length - window) / (double)sr + 1e-9,
            "a frame cannot be timestamped past the last full window in the stream");

        // The very first full window is timestamped 0, and Reset re-anchors the axis
        // exactly as it does StreamPosition.
        var fresh = new JuceTunerStream(sr, window, hopSize: hop);
        Assert.True(fresh.Push(tone.AsSpan(0, window)));
        Assert.Equal(0.0, fresh.Latest!.Value.TimeSeconds, 9);

        fresh.Push(tone.AsSpan(window, hop * 4));
        Assert.True(fresh.Latest!.Value.TimeSeconds > 0.0);
        fresh.Reset();
        Assert.True(fresh.Push(tone.AsSpan(0, window)));
        Assert.Equal(0.0, fresh.Latest!.Value.TimeSeconds, 9);
    }
}
