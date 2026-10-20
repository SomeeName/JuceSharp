using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

public sealed class JuceSpectrogramTests
{
    [Fact]
    public void Builds_expected_frame_count()
    {
        var sr = 22_050;
        var mono = new float[sr];
        var rng = new Random(7);
        for (var i = 0; i < mono.Length; i++) mono[i] = (float)(rng.NextDouble() - 0.5);

        var sgram = new JuceSpectrogramBuilder(fftSize: 1024, hopSize: 256).Build(mono, sr);
        Assert.Equal(1 + (sr - 1024) / 256, sgram.FrameCount);
        Assert.Equal(512, sgram.BinCount);
        Assert.All(sgram.Frames, f => Assert.Equal(512, f.Length));
    }

    [Fact]
    public void Sine_lights_up_expected_bin()
    {
        const int sr = 44_100;
        const int fft = 4096;
        var freq = 1000.0;
        var mono = new float[fft * 4];
        for (var i = 0; i < mono.Length; i++)
            mono[i] = (float)Math.Sin(2 * Math.PI * freq * i / sr);

        var sgram = new JuceSpectrogramBuilder(fft, fft / 4).Build(mono, sr, decibelScale: false);
        Assert.True(sgram.FrameCount > 0);
        var frame = sgram.Frames[sgram.FrameCount / 2];
        var expectedBin = (int)Math.Round(freq * fft / sr);
        var argmax = 0;
        var max = 0f;
        for (var b = 0; b < frame.Length; b++)
            if (frame[b] > max) { max = frame[b]; argmax = b; }
        Assert.InRange(argmax, expectedBin - 2, expectedBin + 2);
    }
}
