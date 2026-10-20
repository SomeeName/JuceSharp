using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

public class JuceMidiInputTests
{
    [Fact]
    public void Message_AccessorsHandleEmptyData()
    {
        var m = new JuceMidiMessage(0.0, []);
        Assert.Equal(0, m.Status);
        Assert.Equal(0, m.Data1);
        Assert.Equal(0, m.Data2);
        Assert.Equal(0, m.Channel);
    }

    [Fact]
    public void Message_DecodesNoteOn()
    {
        // 0x90 = note on, channel 0, note 60 (middle C), velocity 100
        var m = new JuceMidiMessage(1.5, [0x90, 60, 100]);
        Assert.Equal(0x90, m.Status);
        Assert.Equal(60, m.Data1);
        Assert.Equal(100, m.Data2);
        Assert.Equal(0, m.Channel);
        Assert.Equal(0x90, m.Command);
        Assert.Equal(1.5, m.TimestampSeconds);
    }

    [Fact]
    public void Message_DecodesChannelFromStatus()
    {
        // 0x9F = note on, channel 15
        var m = new JuceMidiMessage(0.0, [0x9F, 64, 64]);
        Assert.Equal(15, m.Channel);
        Assert.Equal(0x90, m.Command);
    }

    [NativeAvailableFact]
    public void Enumerate_DoesNotThrow()
    {
        // The list may legitimately be empty in CI.
        var list = JuceMidiInput.Enumerate();
        Assert.NotNull(list);
    }
}

public class JuceParameterInfoTests
{
    [Fact]
    public void IsDiscrete_TrueForSteppedParameter()
    {
        var info = new JuceParameterInfo(0, "Bypass", 0.0f, 2);
        Assert.True(info.IsDiscrete);
    }

    [Fact]
    public void IsDiscrete_FalseForContinuousParameter()
    {
        var info = new JuceParameterInfo(0, "Gain", 0.5f, 0);
        Assert.False(info.IsDiscrete);
    }

    [Fact]
    public void Record_EqualityWorks()
    {
        var a = new JuceParameterInfo(3, "Mix", 0.5f, 0);
        var b = new JuceParameterInfo(3, "Mix", 0.5f, 0);
        Assert.Equal(a, b);
    }
}

public class JuceRenderToWavTests
{
    [NativeAvailableFact]
    public void RenderToWav_ProducesOutputFile()
    {
        var src = TinyWav.Create(seconds: 0.1);
        var dst = Path.Combine(Path.GetDirectoryName(src)!, "out.wav");
        try
        {
            JuceAudioEngine.RenderToWav(src, dst);
            Assert.True(File.Exists(dst), "RenderToWav should produce an output file");
            Assert.True(new FileInfo(dst).Length > 44, "Output WAV should have audio data");
        }
        finally
        {
            TinyWav.TryDelete(src);
        }
    }

    [Fact]
    public void RenderToWav_RejectsEmptyPaths()
    {
        Assert.Throws<ArgumentException>(() => JuceAudioEngine.RenderToWav("", "x.wav"));
        Assert.Throws<ArgumentException>(() => JuceAudioEngine.RenderToWav("a.wav", ""));
    }

    /// <summary>
    /// Stretch and shift were ACCEPTED and silently ignored: the native body
    /// was <c>(void) tempo; (void) semitones;</c> behind a "real DSP TBD"
    /// comment, so asking for −2 semitones returned <c>DCA_OK</c> and a
    /// bit-exact copy at the original pitch. A capability the code does not
    /// have must never look live.
    /// </summary>
    [Theory]
    [InlineData(0.5f, 0.0f)]
    [InlineData(2.0f, 0.0f)]
    [InlineData(1.0f, -2.0f)]
    [InlineData(1.0f, 7.0f)]
    [InlineData(0.8f, 3.0f)]
    public void RenderToWav_RefusesStretchAndShift(float tempo, float semitones)
    {
        Assert.Throws<NotSupportedException>(
            () => JuceAudioEngine.RenderToWav("a.wav", "b.wav", tempo, semitones));
    }

    /// <summary>The refusal above must not have eaten the supported case:
    /// unity tempo, zero semitones, and tempo 0 ("source default") all still
    /// render.</summary>
    [Theory]
    [InlineData(1.0f)]
    [InlineData(0.0f)]
    public void RenderToWav_StillRendersAtUnity(float tempo)
    {
        var src = TinyWav.Create(seconds: 0.1);
        var dst = Path.Combine(Path.GetDirectoryName(src)!, "unity.wav");
        try
        {
            JuceAudioEngine.RenderToWav(src, dst, tempo, 0.0f);
            Assert.True(File.Exists(dst), "the supported path must still render");
            Assert.True(new FileInfo(dst).Length > 44, "Output WAV should have audio data");
        }
        finally
        {
            TinyWav.TryDelete(src);
        }
    }

    /// <summary>
    /// The NATIVE export refuses too — the managed guard is a nicer exception
    /// type, not the defence. Called directly, the way NullHandleRejectionTests
    /// does, because the wrapper can no longer reach this code path.
    /// </summary>
    [NativeAvailableFact]
    public void RenderToWav_TheExportItselfRefusesStretchAndShift()
    {
        var src = TinyWav.Create(seconds: 0.1);
        var dst = Path.Combine(Path.GetDirectoryName(src)!, "native.wav");
        try
        {
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.RenderToWav(src, dst, 0.5f, 0.0f, 0));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.RenderToWav(src, dst, 1.0f, -2.0f, 0));
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.RenderToWav(src, dst, 1.0f, 0.0f, 0));
        }
        finally
        {
            TinyWav.TryDelete(src);
        }
    }
}
