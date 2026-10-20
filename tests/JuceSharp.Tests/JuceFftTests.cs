using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceFftTests
{
    /// <summary>
    /// Only sizes below 2 are rejected now. 3 and 1000 used to throw here and
    /// take MDX-Net's n_fft = 6144 with them (W4-S1); they run the Bluestein
    /// path, which <c>JuceFftBluesteinTests</c> covers.
    /// </summary>
    [Fact]
    public void Constructor_RejectsSizesBelowTwo()
    {
        Assert.Throws<ArgumentException>(() => new JuceFft(0));
        Assert.Throws<ArgumentException>(() => new JuceFft(1));
        Assert.Equal(3, new JuceFft(3).Size);
        Assert.Equal(1000, new JuceFft(1000).Size);
    }

    [Fact]
    public void Forward_DetectsBinOfSineWave()
    {
        const int size = 1024;
        const int targetBin = 64;
        var samples = new float[size];
        for (int i = 0; i < size; i++)
            samples[i] = MathF.Sin(2 * MathF.PI * targetBin * i / size);

        var fft = new JuceFft(size);
        var mag = fft.Magnitude(samples);

        int peak = 0;
        for (int i = 1; i < mag.Length; i++) if (mag[i] > mag[peak]) peak = i;
        Assert.Equal(targetBin, peak);
    }

    [Fact]
    public void Magnitude_RejectsWrongLength()
    {
        var fft = new JuceFft(64);
        Assert.Throws<ArgumentException>(() => fft.Magnitude(new float[32]));
    }
}
