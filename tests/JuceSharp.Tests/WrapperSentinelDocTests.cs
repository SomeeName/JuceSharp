using System;
using System.IO;
using System.Text.RegularExpressions;
using JuceSharp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The wrapper's own follow-ups from the v35 sentinel wave: four XML docs that
/// described the native failure answer as <c>0</c> after it became <c>-1</c>,
/// and the one status that means two different things.
/// <para>
/// Doc lints are unusual and this one earns its place: these four methods CLAMP
/// the sentinel away (<c>Math.Max(0, …)</c>, <c>count &lt;= 0</c>), so no
/// behavioural test can see the difference — the doc is the only place a caller
/// learns that the native call distinguishes "no pedals" from "dead engine",
/// and a doc nothing checks is exactly what rotted here in the first place.
/// </para>
/// </summary>
public sealed class WrapperSentinelDocTests
{
    /// <summary>
    /// Every wrapper method that laundered the <c>-1</c> sentinel says so near
    /// its clamp. Matched within the member's own doc block, not file-wide, so
    /// one correct mention cannot cover for three stale neighbours.
    /// </summary>
    [Theory]
    [InlineData("src/JuceSharp/Dsp/JuceDspChain.cs", "public int NodeCount")]
    [InlineData("src/JuceSharp/Dsp/JuceDspChain.cs", "public int[] NodeIds()")]
    [InlineData("src/JuceSharp/Dsp/JuceDspMeters.cs", "public static int Read(")]
    public void AMethodThatClampsTheSentinel_DocumentsIt(string file, string signature)
    {
        var src = File.ReadAllText(FindRepoFile(file));
        var at = src.IndexOf(signature, StringComparison.Ordinal);
        Assert.True(at > 0, $"{signature} not found in {file} — this lint is pointed at a name "
                            + "that moved, which makes it vacuous");

        // The doc block is whatever /// lines immediately precede the member.
        var head = src[..at];
        var lastBlank = head.LastIndexOf("\n\n", StringComparison.Ordinal);
        var doc = lastBlank >= 0 ? head[lastBlank..] : head;

        Assert.True(doc.Contains("-1", StringComparison.Ordinal),
            $"{signature} in {file} clamps the native -1 sentinel to 0 and its own doc never "
            + "mentions it. Since the v35 wave the native export answers -1 for 'dead engine / "
            + "seat out of range' and 0 only for an EMPTY chain; a caller reading this doc has no "
            + "way to learn that the distinction exists and is being thrown away here.");
    }

    /// <summary>
    /// ...and the one that was already right stays right:
    /// <c>JuceDspChain.LatencySamples</c> surfaces the <c>-1</c> instead of
    /// clamping it, which is what the docs above point callers at.
    /// Non-vacuity for the theory: it proves the extractor finds a doc that
    /// really does say this, rather than passing on any file containing "-1".
    /// </summary>
    [Fact]
    public void TheOneMethodThatSurfacesTheSentinel_StillDoesNotClampIt()
    {
        var src = File.ReadAllText(FindRepoFile("src/JuceSharp/Dsp/JuceDspChain.cs"));
        var at = src.IndexOf("public int LatencySamples", StringComparison.Ordinal);
        Assert.True(at > 0, "JuceDspChain.LatencySamples not found");

        var body = src[at..Math.Min(src.Length, at + 900)];
        Assert.DoesNotContain("Math.Max(0", body, StringComparison.Ordinal);

        var head = src[..at];
        var lastBlank = head.LastIndexOf("\n\n", StringComparison.Ordinal);
        Assert.Contains("−1", head[lastBlank..], StringComparison.Ordinal);
    }

    /// <summary>
    /// W4-N19 follow-up. <c>DCA_ERR_NOT_INITIALIZED</c> answers TWO different
    /// questions — "the backend is not compiled in" and "the weights are not on
    /// disk" — and the ABI has no third code, so a host could not tell a
    /// REBUILD from a DOWNLOAD. The wrapper resolves it before calling native:
    /// a missing model file is a <see cref="FileNotFoundException"/> that names
    /// the path and the catalogue that would fetch it.
    /// </summary>
    [Fact]
    public void AMissingModelFile_IsItsOwnExceptionNamingTheDownload()
    {
        if (!JuceStemSeparation.IsSupported)
        {
            // Without a backend the capability check wins first, by design: a
            // caller branching on NotSupportedException must not be told "your
            // model is missing" about a build that could never separate anyway.
            Assert.Throws<NotSupportedException>(
                () => JuceStemSeparation.Separate("in.wav", "out", "definitely-not-here.bin",
                    0, null, TestContext.Current.CancellationToken));
            return;
        }

        var dir = Directory.CreateTempSubdirectory("juce-stems-missing-model").FullName;
        try
        {
            var audio = Path.Combine(dir, "in.wav");
            File.WriteAllBytes(audio, Array.Empty<byte>());
            var missing = Path.Combine(dir, "definitely-not-here.bin");

            var ex = Assert.Throws<FileNotFoundException>(
                () => JuceStemSeparation.Separate(audio, dir, missing,
                    0, null, TestContext.Current.CancellationToken));

            Assert.Equal(missing, ex.FileName);
            Assert.Contains("DemucsGgmlModelCatalog", ex.Message, StringComparison.Ordinal);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch (IOException) { }
        }
    }

    /// <summary>
    /// The wrapper check exists BECAUSE the ABI cannot make this distinction —
    /// so pin that it still cannot, and that the managed side has not quietly
    /// started relying on a code native does not send. If a future ABI grows a
    /// "model missing" status, this fails and the wrapper check can go.
    /// </summary>
    [Fact]
    public void TheAbi_StillHasNoSeparateCodeForAMissingModel()
    {
        var abi = File.ReadAllText(FindRepoFile("src/JuceSharp.Native/src/dotcharter_audio.cpp"));

        // rc == 2 is "model file missing" in StemSeparator; both stem exports map
        // it onto NOT_INITIALIZED, which is also what "no backend" returns.
        Assert.Equal(2, Regex.Matches(abi, @"rc == 2\)\s*return DCA_ERR_NOT_INITIALIZED;").Count);
        Assert.Equal(2, Regex.Matches(abi, @"!dotcharter::stems::is_supported\(\)\) return DCA_ERR_NOT_INITIALIZED;").Count);
    }

    private static string FindRepoFile(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
