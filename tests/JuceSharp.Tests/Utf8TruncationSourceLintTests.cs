using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Every fixed-size string out-buffer in this ABI must be cut at a UTF-8 CODE
/// POINT, through <c>dcabi::utf8TruncatedLength</c> (src/Utf8Truncate.h) or one
/// of the two helpers built on it (<c>copyTruncated</c> over
/// <c>juce::String</c>, <c>copyStdTruncated</c> over <c>std::string</c>). A
/// torn sequence decodes to U+FFFD, so a short buffer CORRUPTS the tail rather
/// than clipping it — and the managed side then cannot match the string it was
/// handed back, which is the same failure the <c>LPStr</c> mojibake bug
/// produced from the other end.
/// <para>
/// <see cref="Dsp.Utf8TruncationTests"/> drives every cut position through a
/// real export, but it can only do that for
/// <c>dca_engine_dsp_get_node_string</c>, where the CALLER chooses the string.
/// The three sites fixed on 2026-09-10 have no such lever: a VST3 parameter
/// name needs a real plugin, a MIDI device name needs real hardware, and a
/// driver type name is whatever the host's audio stack is called. So they get a
/// lint instead — which is also the only thing that stops the next one being
/// hand-rolled. The comment in <c>dca_stems_describe_backend</c> claimed to be
/// "the last hand-rolled memcpy" while all three of these were still live.
/// </para>
/// </summary>
public sealed class Utf8TruncationSourceLintTests
{
    private const string AbiCpp = "src/JuceSharp.Native/src/dotcharter_audio.cpp";
    private const string EngineCpp = "src/JuceSharp.Native/src/AudioEngine.cpp";

    public static TheoryData<string, string> Sites => new()
    {
        { AbiCpp, "dca_plugin_get_parameter_info" },
        { AbiCpp, "dca_midi_input_enumerate" },
        { AbiCpp, "dca_stems_describe_backend" },
        { EngineCpp, "AudioEngine::enumerateHostTypes" },
    };

    [Theory]
    [MemberData(nameof(Sites))]
    public void EveryFixedOutBuffer_IsCutAtACodePoint(string file, string function)
    {
        var body = StripComments(BodyOf(File.ReadAllText(FindRepoFile(file)), function));

        Assert.True(
            body.Contains("utf8TruncatedLength", StringComparison.Ordinal)
            || body.Contains("copyTruncated(", StringComparison.Ordinal)
            || body.Contains("copyStdTruncated(", StringComparison.Ordinal),
            $"{function} writes a fixed-size string buffer without going through "
            + "dcabi::utf8TruncatedLength (or copyTruncated / copyStdTruncated) — a byte-wise "
            + "cut tears a UTF-8 sequence and the managed side reads U+FFFD");
    }

    /// <summary>
    /// The generic half: nobody may hand-roll a bounded string copy anywhere in
    /// the two native translation units that own the ABI's out buffers. The
    /// pattern is the loop those three sites used —
    /// <c>for (; i &lt; N &amp;&amp; s[i] != '\0'; ++i)</c>.
    /// </summary>
    [Fact]
    public void NoHandRolledByteWiseStringCopy_SurvivesInTheAbiOrTheEngine()
    {
        foreach (var file in new[] { AbiCpp, EngineCpp })
        {
            var src = StripComments(File.ReadAllText(FindRepoFile(file)));
            var hits = Regex.Matches(src, @"!=\s*'\\0'\s*;\s*\+\+\w+\s*\)\s*\w+\s*\[").Count;
            Assert.True(hits == 0,
                $"{file} carries {hits} hand-rolled byte-wise string copy loop(s); route them "
                + "through dcabi::utf8TruncatedLength");
        }
    }

    /// <summary>Non-vacuity: the body extractor must really be finding bodies,
    /// or every assertion above passes against an empty string.</summary>
    [Fact]
    public void TheBodyExtractor_FindsRealBodies()
    {
        var body = BodyOf(File.ReadAllText(FindRepoFile(AbiCpp)), "dca_midi_input_enumerate");
        Assert.Contains("getAvailableDevices", body, StringComparison.Ordinal);
        Assert.True(body.Length > 200, $"extracted body is only {body.Length} chars");

        // …and a function that genuinely does NOT truncate must fail the
        // predicate the theory uses, so the predicate is not a tautology.
        var other = StripComments(BodyOf(File.ReadAllText(FindRepoFile(AbiCpp)),
                                         "dca_engine_set_master_volume"));
        Assert.DoesNotContain("utf8TruncatedLength", other, StringComparison.Ordinal);
    }

    /// <summary>Everything from the function's opening brace to its matching
    /// close, by brace depth. Crude, but these are plain C functions.</summary>
    private static string BodyOf(string src, string function)
    {
        var at = src.IndexOf(function + "(", StringComparison.Ordinal);
        Assert.True(at >= 0, $"{function} not found");
        var open = src.IndexOf('{', at);
        Assert.True(open >= 0, $"{function} has no body");

        var depth = 0;
        for (var i = open; i < src.Length; i++)
        {
            if (src[i] == '{') depth++;
            else if (src[i] == '}' && --depth == 0)
                return src.Substring(open, i - open + 1);
        }
        throw new InvalidDataException($"unbalanced braces after {function}");
    }

    private static string StripComments(string src)
    {
        src = Regex.Replace(src, @"/\*.*?\*/", " ", RegexOptions.Singleline);
        return Regex.Replace(src, @"//[^\n]*", " ");
    }

    private static string FindRepoFile(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException(
            $"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
