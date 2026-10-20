using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceChromagramTests
{
    [Fact]
    public void Compute_HighlightsCMajorTriadPitchClasses()
    {
        const int sr = 44100;
        const int size = 4096;
        var frame = new float[size];
        float[] freqs = [261.63f, 329.63f, 392.0f]; // C4, E4, G4
        foreach (var f in freqs)
            for (int i = 0; i < size; i++)
                frame[i] += MathF.Sin(2 * MathF.PI * f * i / sr) / freqs.Length;

        var chroma = new JuceChromagram(sr).Compute(frame);

        int c = 0, e = 4, g = 7;
        for (int i = 0; i < 12; i++)
        {
            if (i == c || i == e || i == g) continue;
            Assert.True(chroma[c] > chroma[i]);
            Assert.True(chroma[e] > chroma[i]);
            Assert.True(chroma[g] > chroma[i]);
        }
    }
}
