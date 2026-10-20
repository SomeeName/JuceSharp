using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JucePolyphonicPitchTests
{
    private const int Sr = 48000;

    private static float[] Harmonics(float f0, int sr, int n, int count = 8, float scale = 0.2f)
    {
        var s = new float[n];
        for (int h = 1; h <= count; h++)
        {
            double w = 2 * Math.PI * f0 * h / sr;
            for (int i = 0; i < n; i++)
                s[i] += (float)(scale / h * Math.Sin(w * i));
        }
        return s;
    }

    [Theory]
    [InlineData(82.41f, 40)]   // E2
    [InlineData(110f, 45)]     // A2
    [InlineData(196f, 55)]     // G3
    [InlineData(329.63f, 64)]  // E4
    public void Detect_SingleNote_TopCandidateMatches(float f0, int midi)
    {
        var poly = new JucePolyphonicPitch(Sr);
        var candidates = poly.Detect(Harmonics(f0, Sr, JucePolyphonicPitch.WindowSize));

        Assert.NotEmpty(candidates);
        Assert.Equal(midi, candidates[0].Midi);
        Assert.Equal(1f, candidates[0].Salience);
        Assert.InRange(candidates[0].FrequencyHz, f0 * 0.99f, f0 * 1.01f);
    }

    [Fact]
    public void Detect_PowerChord_FindsBothPitchClasses()
    {
        var window = new float[JucePolyphonicPitch.WindowSize];
        foreach (var f in new[] { 82.41f, 123.47f, 164.81f })
        {
            var part = Harmonics(f, Sr, window.Length);
            for (int i = 0; i < window.Length; i++) window[i] += part[i];
        }

        var poly = new JucePolyphonicPitch(Sr);
        var candidates = poly.Detect(window);

        var pcs = candidates.Select(c => ((c.Midi % 12) + 12) % 12).ToHashSet();
        Assert.Contains(4, pcs);   // E
        Assert.Contains(11, pcs);  // B
    }

    [Fact]
    public void Detect_Silence_ReturnsEmpty()
    {
        var poly = new JucePolyphonicPitch(Sr);
        Assert.Empty(poly.Detect(new float[JucePolyphonicPitch.WindowSize]));
    }

    [Fact]
    public void Detect_RejectsWrongWindowSize()
    {
        var poly = new JucePolyphonicPitch(Sr);
        Assert.Throws<ArgumentException>(() => poly.Detect(new float[1024]));
    }
}
