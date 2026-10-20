using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceAudioExceptionTests
{
    [Fact]
    public void CarriesMessageAndStatusCode()
    {
        var ex = new JuceAudioException("boom", NativeMethods.DcaStatus.Device, "engine_create");
        Assert.Equal("boom", ex.Message);
        Assert.Equal((int)NativeMethods.DcaStatus.Device, ex.StatusCode);
        Assert.Equal(JuceAudioStatus.Device, ex.Status);
        Assert.Equal("engine_create", ex.Operation);
    }

    [Theory]
    [InlineData(0, 0)]
    [InlineData(1, 1)]
    [InlineData(2, 2)]
    [InlineData(3, 3)]
    [InlineData(4, 4)]
    [InlineData(5, 5)]
    [InlineData(99, 99)]
    public void StatusCodeMirrorsEnum(int statusOrdinal, int expected)
    {
        var s = (NativeMethods.DcaStatus)statusOrdinal;
        Assert.Equal(expected, new JuceAudioException("x", s, "op").StatusCode);
        Assert.Equal((JuceAudioStatus)statusOrdinal, new JuceAudioException("x", s, "op").Status);
    }

    [Fact]
    public void IsException()
    {
        Assert.IsAssignableFrom<Exception>(
            new JuceAudioException("m", NativeMethods.DcaStatus.Decode, "op"));
    }

    [Fact]
    public void ToString_IncludesOperationAndStatus()
    {
        var ex = new JuceAudioException("oops", NativeMethods.DcaStatus.Plugin, "plugin_load_vst3");
        var s = ex.ToString();
        Assert.Contains("plugin_load_vst3", s);
        Assert.Contains("Plugin", s);
    }
}
