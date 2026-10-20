using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

public sealed class JuceWaveformPeaksTests
{
    [Fact]
    public void Captures_sine_envelope()
    {
        const int sr = 48_000;
        var mono = new float[sr];
        for (var i = 0; i < mono.Length; i++)
            mono[i] = (float)Math.Sin(2 * Math.PI * 440 * i / sr) * 0.8f;

        var peaks = JuceWaveformPeaks.Build(mono, channels: 1, sampleRate: sr, bucketMilliseconds: 10);
        Assert.Equal(100, peaks.Length);
        foreach (var peak in peaks)
        {
            Assert.InRange(peak.Max, 0.7f, 0.85f);
            Assert.InRange(peak.Min, -0.85f, -0.7f);
        }
    }

    [Fact]
    public void Stereo_input_collapses_to_mono_avg()
    {
        var sr = 1000;
        var stereo = new float[sr * 2];
        for (var f = 0; f < sr; f++) { stereo[2 * f] = 0.5f; stereo[2 * f + 1] = -0.5f; }
        var peaks = JuceWaveformPeaks.Build(stereo, channels: 2, sampleRate: sr, bucketMilliseconds: 100);
        Assert.Equal(10, peaks.Length);
        Assert.All(peaks, p =>
        {
            Assert.Equal(0f, p.Max, 5);
            Assert.Equal(0f, p.Min, 5);
        });
    }

    [Fact]
    public void Empty_input_returns_empty()
    {
        var peaks = JuceWaveformPeaks.Build([], 1, 48_000);
        Assert.Empty(peaks);
    }
}
