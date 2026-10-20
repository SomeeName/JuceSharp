using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceChromaticTunerTests
{
    [Fact]
    public void Detect_RecognisesA4()
    {
        const int sr = 44100;
        const int size = 4096;
        const float f = 440f;
        var samples = new float[size];
        for (int i = 0; i < size; i++)
            samples[i] = 0.5f * MathF.Sin(2 * MathF.PI * f * i / sr);

        var tuner = new JuceChromaticTuner(sr, size);
        var r = tuner.Detect(samples);

        Assert.NotNull(r);
        Assert.Equal("A", r.Value.NoteName);
        Assert.Equal(4, r.Value.Octave);
        Assert.InRange(r.Value.CentsOff, -5f, 5f);
        Assert.InRange(r.Value.FrequencyHz, 438f, 442f);
    }

    [Fact]
    public void Detect_ReturnsNullForSilence()
    {
        var tuner = new JuceChromaticTuner(44100);
        Assert.Null(tuner.Detect(new float[2048]));
    }

    [Fact]
    public void Detect_ReturnsNullForUndersizedBuffer()
    {
        var tuner = new JuceChromaticTuner(44100);
        Assert.Null(tuner.Detect(new float[100]));
    }
}
