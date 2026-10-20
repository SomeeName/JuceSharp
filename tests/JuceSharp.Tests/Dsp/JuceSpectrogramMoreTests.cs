using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

public sealed class JuceSpectrogramMoreTests
{
    [Fact]
    public void Construct_HopMustBePositiveAndAtMostFft()
    {
        Assert.Throws<ArgumentException>(() => new JuceSpectrogramBuilder(1024, 0));
        Assert.Throws<ArgumentException>(() => new JuceSpectrogramBuilder(1024, -1));
        Assert.Throws<ArgumentException>(() => new JuceSpectrogramBuilder(1024, 1025));
    }

    [Fact]
    public void Build_TooShortInput_ReturnsEmptyFrames()
    {
        var sgram = new JuceSpectrogramBuilder(1024, 256).Build(new float[100], 22050);
        Assert.Equal(0, sgram.FrameCount);
        Assert.Equal(512, sgram.BinCount);
        Assert.False(sgram.DecibelScale);
    }

    [Fact]
    public void Build_ExactlyOneFftWindow_ProducesOneFrame()
    {
        var sgram = new JuceSpectrogramBuilder(512, 128).Build(new float[512], 22050);
        Assert.Equal(1, sgram.FrameCount);
    }

    [Fact]
    public void BinHz_ScalesWithSampleRate()
    {
        var sgram = new JuceSpectrogramBuilder(1024, 256).Build(new float[2048], 44100);
        Assert.Equal(0.0, sgram.BinHz(0));
        Assert.Equal(43.06, sgram.BinHz(1), 1);
        // Nyquist-adjacent: bin (FftSize/2 - 1) = 511
        Assert.Equal(44100 * 511.0 / 1024.0, sgram.BinHz(511), 3);
    }

    [Fact]
    public void FrameTimeSeconds_ScalesWithHop()
    {
        var sgram = new JuceSpectrogramBuilder(512, 128).Build(new float[1024], 16000);
        Assert.Equal(0.0, sgram.FrameTimeSeconds(0), 6);
        Assert.Equal(128 / 16000.0, sgram.FrameTimeSeconds(1), 6);
        Assert.Equal(2 * 128 / 16000.0, sgram.FrameTimeSeconds(2), 6);
    }

    [Fact]
    public void DecibelScale_FlooredAtMinus120()
    {
        // All-zero input => magnitude exactly 0 => dB clamp to -120.
        var sgram = new JuceSpectrogramBuilder(512, 128).Build(new float[2048], 22050, decibelScale: true);
        Assert.True(sgram.DecibelScale);
        Assert.All(sgram.Frames, frame => Assert.All(frame, v => Assert.Equal(-120f, v)));
    }

    [Fact]
    public void DecibelScale_FullScaleSineNearZeroDb()
    {
        const int sr = 22050;
        const int fft = 2048;
        var mono = new float[fft * 4];
        // Sine that doesn't perfectly bin-align — magnitude per bin ≈ 0.5,
        // dB ≈ -6. We just assert it's well above the -120 floor.
        for (var i = 0; i < mono.Length; i++)
            mono[i] = MathF.Sin(2f * MathF.PI * 800f * i / sr);

        var sgram = new JuceSpectrogramBuilder().Build(mono, sr, decibelScale: true);
        var max = float.NegativeInfinity;
        foreach (var frame in sgram.Frames)
            foreach (var v in frame)
                if (v > max) max = v;

        Assert.True(max > -20f, $"expected peak well above -20 dB, got {max}");
    }
}
