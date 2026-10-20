using JuceSharp.AudioIo;
using JuceSharp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

/// <summary>
/// D14. Two of the decoder's error paths threw away the only information the
/// caller could act on. Neither branch needs the native engine — both are
/// reached through the internal helpers on purpose, because the alternative
/// fixtures are "a broken driver" and "a 62-minute 6-channel master".
/// </summary>
public sealed class JuceAudioDecoderErrorTests
{
    /// <summary>
    /// Every unmapped native status was rewritten to <c>Internal</c> on the way
    /// out, so a NotFound, an Unsupported and a real engine fault all arrived
    /// looking identical to a caller switching on <c>JuceAudioException.Status</c>.
    /// </summary>
    [Theory]
    [InlineData((int)NativeMethods.DcaStatus.NotInitialized)]
    [InlineData((int)NativeMethods.DcaStatus.Device)]
    [InlineData((int)NativeMethods.DcaStatus.Plugin)]
    public void ThrowIfError_PassesTheRealNativeStatusThrough(int raw)
    {
        var status = (NativeMethods.DcaStatus)raw;
        var ex = Assert.Throws<JuceAudioException>(
            () => JuceAudioDecoder.ThrowIfError(status, "song.flac", "decode"));

        Assert.Equal((int)status, ex.StatusCode);
        Assert.NotEqual((int)NativeMethods.DcaStatus.Internal, ex.StatusCode);
        Assert.Contains("song.flac", ex.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ThrowIfError_KeepsItsTwoSpecificMappings()
    {
        Assert.Throws<ArgumentException>(
            () => JuceAudioDecoder.ThrowIfError(NativeMethods.DcaStatus.InvalidArg, "x.wav", "probe"));
        Assert.Throws<InvalidDataException>(
            () => JuceAudioDecoder.ThrowIfError(NativeMethods.DcaStatus.Decode, "x.wav", "probe"));
        JuceAudioDecoder.ThrowIfError(NativeMethods.DcaStatus.Ok, "x.wav", "probe"); // must not throw
    }

    /// <summary>
    /// A file past 2^31 interleaved samples used to surface as a bare
    /// <c>OverflowException</c> from a <c>checked</c> cast — no file name, no
    /// size, nothing saying the file was merely too long.
    /// </summary>
    [Fact]
    public void TooManySamples_ThrowsNotSupportedWithTheFileAndTheSize()
    {
        var ex = Assert.Throws<NotSupportedException>(
            () => JuceAudioDecoder.CheckedTotalSamples(frames: 4_000_000_000L, channels: 2, path: "huge.wav"));

        Assert.Contains("huge.wav", ex.Message, StringComparison.Ordinal);
        Assert.Contains("8000000000", ex.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("Arithmetic operation resulted", ex.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ASizeThatFits_IsReturnedUnchanged()
    {
        Assert.Equal(2_000_000, JuceAudioDecoder.CheckedTotalSamples(1_000_000, 2, "ok.wav"));
    }
}
