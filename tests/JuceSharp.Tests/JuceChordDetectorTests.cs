using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceChordDetectorTests
{
    [Fact]
    public void Detect_RecognisesCMajor()
    {
        var chroma = new float[12];
        chroma[0] = 0.4f; chroma[4] = 0.3f; chroma[7] = 0.3f;
        var det = new JuceChordDetector();
        var r = det.Detect(chroma);
        Assert.NotNull(r);
        Assert.Equal("C", r.Value.Name);
        Assert.Equal(JuceChordQuality.Major, r.Value.Quality);
    }

    [Fact]
    public void Detect_RecognisesAMinor()
    {
        var chroma = new float[12];
        chroma[9] = 0.4f; chroma[0] = 0.3f; chroma[4] = 0.3f;
        var r = new JuceChordDetector().Detect(chroma);
        Assert.NotNull(r);
        Assert.Equal("Am", r.Value.Name);
        Assert.Equal(JuceChordQuality.Minor, r.Value.Quality);
    }

    [Fact]
    public void Detect_RejectsBadLength()
    {
        Assert.Throws<ArgumentException>(() => new JuceChordDetector().Detect(new float[11]));
    }
}
