using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceEngineConfigTests
{
    [Fact]
    public void Defaults_AreZeroOrEmpty()
    {
        var cfg = new JuceEngineConfig();
        Assert.Equal(0, cfg.SampleRate);
        Assert.Equal(0, cfg.BufferFrames);
        Assert.Equal(0, cfg.OutputChannels);
        Assert.Equal(0, cfg.InputChannels);
        Assert.Equal(0u, cfg.OutputChannelMask);
        Assert.Equal(0u, cfg.InputChannelMask);
        Assert.Equal(AudioDriver.Default, cfg.PreferredDriver);
        Assert.Equal(string.Empty, cfg.OutputDeviceId);
        Assert.Equal(string.Empty, cfg.InputDeviceId);
    }

    [Fact]
    public void ToNative_CopiesAllFields()
    {
        var cfg = new JuceEngineConfig
        {
            SampleRate = 48000,
            BufferFrames = 128,
            OutputChannels = 2,
            InputChannels = 1,
            OutputChannelMask = 0b11,
            InputChannelMask = 0b1,
            PreferredDriver = AudioDriver.Asio,
            OutputDeviceId = "out-id",
            InputDeviceId = "in-id",
        };

        var native = cfg.ToNative();

        Assert.Equal(48000, native.SampleRate);
        Assert.Equal(128, native.BufferFrames);
        Assert.Equal(2, native.OutputChannels);
        Assert.Equal(1, native.InputChannels);
        Assert.Equal(0b11u, native.OutputChannelMask);
        Assert.Equal(0b1u, native.InputChannelMask);
        Assert.Equal(NativeMethods.DcaDriver.Asio, native.PreferredDriver);
        Assert.Equal("out-id", native.OutputDeviceId);
        Assert.Equal("in-id", native.InputDeviceId);
    }

    [Theory]
    [InlineData(AudioDriver.Default, (int)NativeMethods.DcaDriver.Default)]
    [InlineData(AudioDriver.Wasapi, (int)NativeMethods.DcaDriver.Wasapi)]
    [InlineData(AudioDriver.DirectSound, (int)NativeMethods.DcaDriver.DirectSound)]
    [InlineData(AudioDriver.Asio, (int)NativeMethods.DcaDriver.Asio)]
    [InlineData(AudioDriver.CoreAudio, (int)NativeMethods.DcaDriver.CoreAudio)]
    [InlineData(AudioDriver.Alsa, (int)NativeMethods.DcaDriver.Alsa)]
    [InlineData(AudioDriver.Jack, (int)NativeMethods.DcaDriver.Jack)]
    public void ToNative_MapsAllDrivers(AudioDriver managed, int expectedOrdinal)
    {
        var native = new JuceEngineConfig { PreferredDriver = managed }.ToNative();
        Assert.Equal((NativeMethods.DcaDriver)expectedOrdinal, native.PreferredDriver);
    }

    [Fact]
    public void ToNative_NormalisesNullDeviceIdsToEmpty()
    {
        var cfg = new JuceEngineConfig { OutputDeviceId = null!, InputDeviceId = null! };
        var native = cfg.ToNative();
        Assert.Equal(string.Empty, native.OutputDeviceId);
        Assert.Equal(string.Empty, native.InputDeviceId);
    }

    [Fact]
    public void Validate_AcceptsDefaults()
        => new JuceEngineConfig().Validate();

    [Theory]
    [InlineData(-1)]
    [InlineData(500_000)]
    public void Validate_RejectsBadSampleRate(int sr)
        => Assert.Throws<ArgumentOutOfRangeException>(
            () => new JuceEngineConfig { SampleRate = sr }.Validate());

    [Theory]
    [InlineData(-1)]
    [InlineData(20_000)]
    public void Validate_RejectsBadBufferFrames(int bf)
        => Assert.Throws<ArgumentOutOfRangeException>(
            () => new JuceEngineConfig { BufferFrames = bf }.Validate());

    [Theory]
    [InlineData(-1)]
    [InlineData(128)]
    public void Validate_RejectsBadChannelCounts(int n)
    {
        Assert.Throws<ArgumentOutOfRangeException>(
            () => new JuceEngineConfig { OutputChannels = n }.Validate());
        Assert.Throws<ArgumentOutOfRangeException>(
            () => new JuceEngineConfig { InputChannels = n }.Validate());
    }

    [Fact]
    public void Validate_RejectsUnknownDriver()
        => Assert.Throws<ArgumentOutOfRangeException>(
            () => new JuceEngineConfig { PreferredDriver = (AudioDriver)999 }.Validate());

    [Theory]
    [InlineData(0)]
    [InlineData(44_100)]
    [InlineData(48_000)]
    [InlineData(96_000)]
    [InlineData(384_000)]
    public void Validate_AcceptsTypicalSampleRates(int sr)
        => new JuceEngineConfig { SampleRate = sr }.Validate();

    [Fact]
    public void EnableConsoleLog_DefaultsTrue()
        => Assert.True(new JuceEngineConfig().EnableConsoleLog);

    [Fact]
    public void EnableConsoleLog_CanBeDisabled()
        => Assert.False(new JuceEngineConfig { EnableConsoleLog = false }.EnableConsoleLog);
}
