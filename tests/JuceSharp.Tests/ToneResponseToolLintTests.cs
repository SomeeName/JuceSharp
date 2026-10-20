using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// W4-D63. <c>tools/tone_response.cpp</c> is how this repo's own rule — "an
/// in-path bass filter is a BOUNDED SHELF, never an unbounded high-pass; they
/// SUM, so never add a fourth without measuring the cascade" — is actually
/// measured. It had NO CMake target and said so on purpose, with a hand
/// <c>cl</c> line in its header comment, so nothing ever compiled it.
/// <para>
/// That is what let it keep a HAND-TRANSCRIBED copy of <c>AmpCore</c>'s private
/// 24-row voicing table for as long as it did: a copy nobody builds cannot fail
/// to build, and its own comment admitted "a mismatch means this table drifted
/// from the header" with nothing but a printed cross-check to notice. It reads
/// the real table through <c>AmpCore::voicingLfForTest</c> now, and there is an
/// <c>EXCLUDE_FROM_ALL</c> target so the compiler sees the file whenever
/// somebody asks.
/// </para>
/// <para>
/// A source lint rather than a ctest entry, deliberately: the tool PRINTS
/// measurements for a human and asserts nothing, so registering it with ctest
/// would imply a pass/fail it does not have. What must not regress is that it
/// still compiles and still has no second copy of the table — both of which are
/// statements about the source and the build file.
/// </para>
/// </summary>
public sealed class ToneResponseToolLintTests
{
    private const string Tool = "src/JuceSharp.Native/tools/tone_response.cpp";
    private const string NativeCMake = "src/JuceSharp.Native/CMakeLists.txt";
    private const string AmpCore = "src/JuceSharp.Native/src/dsp/cores/dcdsp_amp.h";

    /// <summary>
    /// The tool has a target, it is <c>EXCLUDE_FROM_ALL</c> (so neither a normal
    /// build nor CI pays for it), and it compiles the real file.
    /// </summary>
    [Fact]
    public void TheToneResponseTool_HasAnExcludeFromAllTarget()
    {
        var cmake = File.ReadAllText(FindRepoFile(NativeCMake));

        var m = Regex.Match(cmake,
            @"add_executable\s*\(\s*juce_sharp_tone_response\s+(?<flags>[^)]*?)tools/tone_response\.cpp\s*\)");
        Assert.True(m.Success,
            "there is no add_executable for tools/tone_response.cpp. Without a target nothing ever "
            + "compiles it, which is exactly how it kept a hand-transcribed copy of AmpCore's "
            + "voicing table: a file nobody builds cannot fail to build.");

        Assert.Contains("EXCLUDE_FROM_ALL", m.Groups["flags"].Value, StringComparison.Ordinal);

        // ...and it can see the cores it includes.
        Assert.Matches(
            @"target_include_directories\s*\(\s*juce_sharp_tone_response\s+PRIVATE[^)]*src\s*\)", cmake);
    }

    /// <summary>
    /// No ctest entry, on purpose. The tool prints tables and returns 0
    /// regardless; a <c>add_test</c> would report "passed" for output nobody
    /// read, which is worse than no coverage because it looks like coverage.
    /// </summary>
    [Fact]
    public void TheToneResponseTool_IsNotRegisteredWithCTest()
    {
        var cmake = File.ReadAllText(FindRepoFile(NativeCMake));
        var tests = File.ReadAllText(FindRepoFile("src/JuceSharp.Native/tests/CMakeLists.txt"));

        Assert.DoesNotContain("add_test(NAME tone_response", cmake, StringComparison.Ordinal);
        Assert.DoesNotContain("juce_sharp_tone_response", tests, StringComparison.Ordinal);
    }

    /// <summary>
    /// The tool reads the REAL voicing numbers and carries no second copy of
    /// them. The transcribed table was 24 rows of brace-initialised floats named
    /// <c>kLf</c>; what replaces it is one call per row.
    /// </summary>
    [Fact]
    public void TheToneResponseTool_ReadsTheRealVoicingTable()
    {
        var tool = File.ReadAllText(FindRepoFile(Tool));

        Assert.Contains("voicingLfForTest", tool, StringComparison.Ordinal);
        Assert.DoesNotContain("kLf", tool, StringComparison.Ordinal);

        // The row COUNT comes from the core too, so a 25th voicing cannot leave
        // the tool walking 24.
        Assert.Contains("numVoicingsForTest", tool, StringComparison.Ordinal);
        Assert.DoesNotContain("const VoicingLf kLf[24]", tool, StringComparison.Ordinal);
    }

    /// <summary>
    /// ...and the accessor really returns the table rather than a fresh
    /// transcription of it one level down, which would move the problem instead
    /// of fixing it.
    /// </summary>
    [Fact]
    public void TheVoicingAccessor_ReadsKVoicingsItself()
    {
        var amp = File.ReadAllText(FindRepoFile(AmpCore));
        var at = amp.IndexOf("voicingLfForTest", StringComparison.Ordinal);
        Assert.True(at > 0, "AmpCore::voicingLfForTest is gone — the tool has nothing to read");

        var body = amp[at..Math.Min(amp.Length, at + 800)];
        Assert.Contains("kVoicings[", body, StringComparison.Ordinal);

        // Non-vacuity: kVoicings is still the private table it always was, so
        // "reads kVoicings" means something.
        Assert.Matches(@"static constexpr Voicing kVoicings\[kNumVoicings\]", amp);
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
