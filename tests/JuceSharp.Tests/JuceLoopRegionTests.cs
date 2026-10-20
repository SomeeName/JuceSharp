using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceLoopRegionTests
{
    [Fact]
    public void Constructor_ValidatesArgs()
    {
        Assert.Throws<ArgumentException>(() =>
            new JuceLoopRegion(TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(1)));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1), pollIntervalMs: 0));
    }

    [Fact]
    public void Dispose_IsIdempotent()
    {
        var loop = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1));
        loop.Dispose();
        loop.Dispose();
    }
}
