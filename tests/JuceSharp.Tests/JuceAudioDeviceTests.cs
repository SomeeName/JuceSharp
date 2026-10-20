using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceAudioDeviceTests
{
    [Fact]
    public void Record_StoresAllFields()
    {
        var d = new JuceAudioDevice("id", "name", AudioDriver.Wasapi, 2, 8, 48000, true);
        Assert.Equal("id", d.Id);
        Assert.Equal("name", d.Name);
        Assert.Equal(AudioDriver.Wasapi, d.Driver);
        Assert.Equal(2, d.MaxInputChannels);
        Assert.Equal(8, d.MaxOutputChannels);
        Assert.Equal(48000, d.DefaultSampleRate);
        Assert.True(d.IsDefault);
    }

    [Fact]
    public void Record_Equality_ByValue()
    {
        var a = new JuceAudioDevice("x", "n", AudioDriver.Asio, 0, 2, 44100, false);
        var b = new JuceAudioDevice("x", "n", AudioDriver.Asio, 0, 2, 44100, false);
        Assert.Equal(a, b);
        Assert.Equal(a.GetHashCode(), b.GetHashCode());
    }

    [Fact]
    public void Record_Inequality_DifferentField()
    {
        var a = new JuceAudioDevice("x", "n", AudioDriver.Asio, 0, 2, 44100, false);
        var b = a with { IsDefault = true };
        Assert.NotEqual(a, b);
    }
}
