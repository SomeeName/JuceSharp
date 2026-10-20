using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

public sealed class JuceWaveformPeaksMoreTests
{
    [Fact]
    public void InvalidChannels_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            JuceWaveformPeaks.Build(new float[10], 0, 44100));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            JuceWaveformPeaks.Build(new float[10], -1, 44100));
    }

    [Fact]
    public void InvalidSampleRate_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            JuceWaveformPeaks.Build(new float[10], 1, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            JuceWaveformPeaks.Build(new float[10], 1, -1));
    }

    [Fact]
    public void InvalidBucketMs_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            JuceWaveformPeaks.Build(new float[10], 1, 44100, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            JuceWaveformPeaks.Build(new float[10], 1, 44100, -1));
    }

    [Fact]
    public void Build_ProducesCorrectMinMaxForRamp()
    {
        // 200 mono samples 0..199, sample rate 1000 Hz → 50 ms buckets = 50 samples/bucket.
        var mono = new float[200];
        for (var i = 0; i < mono.Length; i++) mono[i] = i;

        var peaks = JuceWaveformPeaks.Build(mono, channels: 1, sampleRate: 1000, bucketMilliseconds: 50);

        Assert.Equal(4, peaks.Length);
        Assert.Equal(0f, peaks[0].Min);
        Assert.Equal(49f, peaks[0].Max);
        Assert.Equal(50f, peaks[1].Min);
        Assert.Equal(99f, peaks[1].Max);
        Assert.Equal(150f, peaks[3].Min);
        Assert.Equal(199f, peaks[3].Max);
    }

    [Fact]
    public void Build_PartialLastBucket_OnlyCoversAvailableFrames()
    {
        // 75 samples → 50 ms buckets at 1 kHz = 50/bucket → 2 buckets,
        // second bucket only has 25 samples (50..74).
        var mono = new float[75];
        for (var i = 0; i < mono.Length; i++) mono[i] = i;

        var peaks = JuceWaveformPeaks.Build(mono, 1, 1000, 50);

        Assert.Equal(2, peaks.Length);
        Assert.Equal(50f, peaks[1].Min);
        Assert.Equal(74f, peaks[1].Max);
    }

    [Fact]
    public void Build_BucketSmallerThanOneFrame_ClampsToSingleSample()
    {
        // bucketMs * sr / 1000 < 1 → framesPerBucket should clamp to 1.
        var mono = new[] { 1f, 2f, 3f };
        var peaks = JuceWaveformPeaks.Build(mono, 1, sampleRate: 100, bucketMilliseconds: 0.01);
        Assert.Equal(3, peaks.Length);
        for (var i = 0; i < 3; i++)
        {
            Assert.Equal(mono[i], peaks[i].Min);
            Assert.Equal(mono[i], peaks[i].Max);
        }
    }
}
