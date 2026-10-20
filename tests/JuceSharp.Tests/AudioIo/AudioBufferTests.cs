using JuceSharp.AudioIo;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

public class AudioBufferTests
{
    [Fact]
    public void FrameCount_DerivedFromInterleavedLength()
    {
        var buf = new AudioBuffer(new float[12], 48000, 2);
        Assert.Equal(6, buf.FrameCount);
    }

    [Fact]
    public void FrameCount_ZeroChannels_ReturnsZero()
    {
        var buf = new AudioBuffer(Array.Empty<float>(), 48000, 0);
        Assert.Equal(0, buf.FrameCount);
    }

    [Fact]
    public void DurationSeconds_MatchesFramesOverSampleRate()
    {
        var buf = new AudioBuffer(new float[48000 * 2], 48000, 2);
        Assert.Equal(1.0, buf.DurationSeconds, 6);
    }

    [Fact]
    public void DurationSeconds_ZeroSampleRate_ReturnsZero()
    {
        var buf = new AudioBuffer(new float[4], 0, 2);
        Assert.Equal(0.0, buf.DurationSeconds);
    }

    [Fact]
    public void SplitChannels_DeinterleavesCorrectly()
    {
        // 3 frames, 2 channels: L0 R0 L1 R1 L2 R2
        var data = new[] { 1f, -1f, 2f, -2f, 3f, -3f };
        var buf  = new AudioBuffer(data, 8000, 2);

        var planes = buf.SplitChannels();

        Assert.Equal(2, planes.Length);
        Assert.Equal(new[] { 1f, 2f, 3f },    planes[0]);
        Assert.Equal(new[] { -1f, -2f, -3f }, planes[1]);
    }

    [Fact]
    public void ToMono_AveragesAcrossChannels()
    {
        // 2 frames, 2 channels
        var data = new[] { 1f, 3f, 2f, 4f };
        var buf  = new AudioBuffer(data, 8000, 2);

        var mono = buf.ToMono();

        Assert.Equal(new[] { 2f, 3f }, mono);
    }

    [Fact]
    public void ToMono_ZeroChannels_ReturnsEmpty()
    {
        var buf  = new AudioBuffer(Array.Empty<float>(), 8000, 0);
        var mono = buf.ToMono();
        Assert.Empty(mono);
    }

    [Fact]
    public void ToMono_MonoInput_PassesThrough()
    {
        var data = new[] { 0.1f, -0.2f, 0.3f };
        var buf  = new AudioBuffer(data, 8000, 1);

        var mono = buf.ToMono();

        Assert.Equal(data, mono);
    }
}
