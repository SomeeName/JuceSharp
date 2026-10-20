using Xunit;

namespace JuceSharp.Tests;

public class JuceLoopRegionConstructorTests
{
    [Fact]
    public void Construct_StoresFields()
    {
        var loop = new JuceLoopRegion(TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(2), 7);
        Assert.Equal(TimeSpan.FromSeconds(1), loop.Start);
        Assert.Equal(TimeSpan.FromSeconds(2), loop.End);
        Assert.Equal(7, loop.PollIntervalMs);
        loop.Dispose();
    }

    [Fact]
    public void Construct_EndBeforeStart_Throws()
    {
        Assert.Throws<ArgumentException>(() =>
            new JuceLoopRegion(TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public void Construct_EndEqualsStart_Throws()
    {
        Assert.Throws<ArgumentException>(() =>
            new JuceLoopRegion(TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public void Construct_PollIntervalLessThanOne_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1), 0));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1), -5));
    }

    [Fact]
    public void Dispose_IsIdempotent()
    {
        var loop = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1));
        loop.Dispose();
        loop.Dispose();
    }
}
