using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using JuceSharp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// A channel mask decides which physical input a seat listens to. Getting one
/// silently wrong does not throw and does not log — it routes a different
/// guitar, which is the failure this repo has already paid for twice.
/// </summary>
public sealed class ChannelMaskTests
{
    [Theory]
    [InlineData(0, 0b1u)]
    [InlineData(1, 0b10u)]
    [InlineData(31, 0x8000_0000u)]
    public void Single_SetsExactlyThatBit(int channel, uint expected)
        => Assert.Equal(expected, ChannelMask.Single(channel));

    /// <summary>
    /// C# masks a shift count to the operand width, so <c>1u &lt;&lt; 32</c> is
    /// <c>1u</c> — channel 0. An out-of-range request has to come back EMPTY,
    /// because "no channel" is audible as silence while "channel 0" is audible
    /// as somebody else's guitar.
    /// </summary>
    [Theory]
    [InlineData(-1)]
    [InlineData(32)]
    [InlineData(33)]
    [InlineData(64)]
    [InlineData(int.MaxValue)]
    public void Single_OutOfRange_IsEmpty_NotChannelZero(int channel)
    {
        var mask = ChannelMask.Single(channel);
        Assert.Equal(0u, mask);
        Assert.False((mask & 1u) != 0, $"channel {channel} wrapped onto channel 0");
    }

    [Theory]
    [InlineData(0, 0u)]
    [InlineData(-4, 0u)]
    [InlineData(1, 0b1u)]
    [InlineData(2, 0b11u)]
    [InlineData(31, 0x7FFF_FFFFu)]
    public void First_TakesTheLowChannels(int count, uint expected)
        => Assert.Equal(expected, ChannelMask.First(count));

    /// <summary><c>1L &lt;&lt; 64</c> is <c>1L</c> for the same reason, so an
    /// unguarded count of 64 returned 0 — no channels at all.</summary>
    [Theory]
    [InlineData(32)]
    [InlineData(64)]
    [InlineData(1000)]
    public void First_AboveThirtyTwo_SaturatesInsteadOfWrappingToNothing(int count)
        => Assert.Equal(uint.MaxValue, ChannelMask.First(count));

    [Fact]
    public void Enumerate_RoundTripsFromChannels()
    {
        var mask = ChannelMask.FromChannels(0, 5, 31);
        Assert.Equal(new[] { 0, 5, 31 }, ChannelMask.Enumerate(mask).ToArray());
    }

    /// <summary>
    /// Source lint. A wrapper that hands native a delegate keeps it alive
    /// through a field, but <c>GC.SuppressFinalize(this)</c> is the last read of
    /// <c>this</c> the JIT can see — after it, the object is collectible while
    /// native still holds the raw function pointer. Every such Dispose must end
    /// with a <c>GC.KeepAlive</c>. <c>JuceMidiInput</c> always had one;
    /// <c>JuceAudioPlugin</c> did not, and un-registered its editor callback in
    /// exactly that window.
    /// </summary>
    [Fact]
    public void EveryWrapperThatRegistersACallback_KeepsItAliveAcrossDispose()
    {
        var dir = FindWrapperSourceDir();
        var offenders = Directory.GetFiles(dir, "*.cs")
            .Select(p => (Path: p, Text: File.ReadAllText(p)))
            // A field whose type is one of the ABI's callback delegates is the
            // only way a managed function pointer reaches native from here.
            .Where(f => Regex.IsMatch(f.Text, @"NativeMethods\.Dca\w*Callback\s+_\w+;"))
            .Where(f => f.Text.Contains("GC.SuppressFinalize"))
            .Where(f => !f.Text.Contains("GC.KeepAlive"))
            .Select(f => Path.GetFileName(f.Path))
            .ToArray();

        Assert.True(offenders.Length == 0,
            "these wrappers hand native a delegate and dispose without a GC.KeepAlive, so "
            + "the thunk can be collected between GC.SuppressFinalize and the last P/Invoke: "
            + string.Join(", ", offenders));
    }

    /// <summary>Proves the lint above looks at something: the two files it is
    /// written for must actually be in scope, or it passes over an empty set.</summary>
    [Fact]
    public void TheCallbackLint_IsNotScanningAnEmptySet()
    {
        var dir = FindWrapperSourceDir();
        var withCallbackFields = Directory.GetFiles(dir, "*.cs")
            .Where(p => Regex.IsMatch(File.ReadAllText(p), @"NativeMethods\.Dca\w*Callback\s+_\w+;"))
            .Select(Path.GetFileName)
            .ToArray();

        Assert.Contains("JuceAudioPlugin.cs", withCallbackFields);
        Assert.Contains("JuceMidiInput.cs", withCallbackFields);
    }

    private static string FindWrapperSourceDir()
    {
        const string relative = "src/JuceSharp";
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative);
            if (File.Exists(Path.Combine(candidate, "ChannelMask.cs"))) return candidate;
        }

        throw new InvalidOperationException(
            $"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
