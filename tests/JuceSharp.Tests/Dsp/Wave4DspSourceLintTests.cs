using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     Source lints over <c>src/JuceSharp.Native/src/dsp/</c> for wave-4 audit
///     findings whose failure mode leaves nothing to assert at runtime.
/// </summary>
public sealed class Wave4DspSourceLintTests
{
    private const string DspDir = "src/JuceSharp.Native/src/dsp";

    /// <summary>
    ///     No <c>.assign(</c> / <c>.resize(</c> inside <c>IrMailbox.h</c>.
    ///     <para>
    ///         <c>IrMailbox::republish</c> says why, in a comment
    ///         <c>ModelIrMailbox::publish</c> sat 40 lines under and did not take:
    ///         both are <c>noexcept</c>, and a <c>bad_alloc</c> inside a
    ///         <c>noexcept</c> function is <c>std::terminate</c>, not an error.
    ///         The sibling that did it is the MORE reached of the two — every
    ///         model knob tick and every preset change goes through it.
    ///     </para>
    ///     <para>
    ///         A lint because the failure is an out-of-memory abort: there is no
    ///         in-process way to make the allocation fail and still have a
    ///         process left to assert in.
    ///     </para>
    /// </summary>
    [Fact]
    public void IrMailbox_AllocatesNowhere()
    {
        var text = StripComments(File.ReadAllText(FindRepoFile(DspDir + "/IrMailbox.h")));
        var offenders = Regex.Matches(text, @"\.\s*(assign|resize|push_back|emplace_back)\s*\(")
                             .Select(m => m.Value).ToList();
        Assert.True(offenders.Count == 0,
            "IrMailbox.h grows a container inside a noexcept publisher: "
            + string.Join(", ", offenders));
    }

    /// <summary>
    ///     Every <c>loadIrWav</c> call passes <c>IrMailbox::kMaxTaps</c>, never a
    ///     literal. <c>IrMailbox::publish</c> silently truncates with
    ///     <c>std::min(count, kMaxTaps)</c>, so raising the loader's cap without
    ///     the constant drops the excess taps with no error anywhere.
    /// </summary>
    [Fact]
    public void TheIrLoaderCap_IsTheMailboxConstant_NotALiteral()
    {
        var text = StripComments(File.ReadAllText(FindRepoFile(DspDir + "/DspModuleFactory.cpp")));
        var calls = Regex.Matches(text, @"loadIrWav\s*\(([^)]*)\)").Select(m => m.Groups[1].Value).ToList();
        Assert.True(calls.Count >= 2, $"expected at least two loadIrWav call sites, found {calls.Count}");
        foreach (var args in calls)
            Assert.True(args.Contains("kMaxTaps", StringComparison.Ordinal),
                $"loadIrWav({args}) passes a literal cap instead of IrMailbox::kMaxTaps");
    }

    /// <summary>
    ///     A test name CITED in a comment under <c>src/dsp/</c> has to exist.
    ///     <para>
    ///         <c>DspModuleFactory.cpp</c> told a reader that a
    ///         second-source-of-truth hazard (the ABI v34 capability-flag table
    ///         against what a constructed module reports) was covered by
    ///         <c>tone_regressions' everyCatalogueFlagMatchesTheModule</c>. That
    ///         function has never existed anywhere in the repo. The sweep is real,
    ///         but it lives in the RT gate — a separate CTest binary
    ///         <c>dotnet test</c> does not run — so the citation sent a reader
    ///         somewhere they would look, not find it, and be unable to tell
    ///         whether the hazard was covered at all. The root guide calls this
    ///         the dangerous kind of rot, and
    ///         <c>Infrastructure/DocReferenceLintTests</c> in the parent repo
    ///         lints markdown for exactly this and does not look at C++ comments.
    ///     </para>
    /// </summary>
    /// <summary>
    ///     Citations this lint has FOUND to be dead and cannot fix here, with the
    ///     reason. This is not a blessing: each entry is a live finding, and an
    ///     entry added merely to turn a red build green defeats the whole test.
    ///     <list type="bullet">
    ///         <item>
    ///             <c>cores/dcdsp_cab.h:158</c> cites
    ///             <c>IrKernelIsPreConvertedTests</c> and
    ///             <c>RtCabModelSwitchDoesNotResampleOnTheCallback</c>; neither
    ///             has ever existed. The real scenario is
    ///             <c>RtCabKernelSwitchDoesNotBurstOneCallback</c>. The cores are
    ///             owned by another change in this wave, so the correction goes
    ///             with that one — remove this entry when it lands.
    ///         </item>
    ///     </list>
    /// </summary>
    // Empty on purpose: a name goes here only while the comment that cites it
    // is being fixed in another branch, and comes out again at merge.
    private static readonly string[] KnownDeadCitations = [];

    [Fact]
    public void EveryTestNameCitedInADspComment_Exists()
    {
        var root = FindRepoDir(DspDir);
        var repo = new DirectoryInfo(root).Parent!.Parent!.Parent!.Parent!.FullName; // dsp -> src -> JuceSharp.Native -> src -> repo
        var haystack = string.Join("\n", CandidateFiles(repo).Select(SafeRead));

        var dead = new List<string>();
        foreach (var file in Directory.EnumerateFiles(root, "*.*", SearchOption.AllDirectories)
                                      .Where(f => f.EndsWith(".h", StringComparison.Ordinal)
                                               || f.EndsWith(".cpp", StringComparison.Ordinal)))
        {
            foreach (var name in CitedTestNames(CommentsOnly(File.ReadAllText(file))))
            {
                if (KnownDeadCitations.Contains(name, StringComparer.Ordinal)) continue;
                // One mention is the citation itself; it has to appear somewhere
                // that is not a comment under src/dsp/.
                if (!haystack.Contains(name, StringComparison.Ordinal))
                    dead.Add($"{Path.GetFileName(file)} cites '{name}', which exists nowhere in the repo");
            }
        }

        Assert.True(dead.Count == 0, string.Join("\n", dead));
    }

    // ---- helpers ------------------------------------------------------------

    /// <summary>
    ///     Names a comment offers as a TEST: an <c>Rt*</c> gate scenario, a
    ///     <c>*Tests</c> class, or a lowerCamel <c>every*</c> sweep function —
    ///     the three shapes this tree actually cites.
    /// </summary>
    private static IEnumerable<string> CitedTestNames(string comments)
        => Regex.Matches(comments, @"\b(Rt[A-Z][A-Za-z0-9]{6,}|[A-Z][A-Za-z0-9]*Tests|every[A-Z][A-Za-z0-9]{6,})\b")
                .Select(m => m.Value)
                .Distinct(StringComparer.Ordinal);

    /// <summary>Everything a cited name could legitimately live in.</summary>
    private static IEnumerable<string> CandidateFiles(string repo)
    {
        foreach (var rel in new[] { "src", "tests", "docs" })
        {
            var dir = Path.Combine(repo, rel);
            if (!Directory.Exists(dir)) continue;
            foreach (var f in Directory.EnumerateFiles(dir, "*.*", SearchOption.AllDirectories))
            {
                var ext = Path.GetExtension(f);
                if (ext is ".cpp" or ".cs" or ".md" or ".txt") yield return f;
                // ...but NOT the src/dsp headers themselves: a name that appears
                // only in the comment citing it is exactly what this catches.
                if (ext == ".h" && !f.Replace('\\', '/').Contains("/src/dsp/", StringComparison.Ordinal))
                    yield return f;
            }
        }
    }

    /// <summary>
    ///     Source counts only for its CODE. A citation must resolve to something
    ///     REAL — a scenario, a class, a function — not to another comment about
    ///     it, and least of all to this lint's own doc comments, which quote the
    ///     dead citation they were written about. That self-reference made the
    ///     sweep pass with the fix reverted, i.e. exactly the vacuity it exists
    ///     to prevent. Markdown is prose all the way down and is read as-is.
    /// </summary>
    private static string SafeRead(string path)
    {
        try
        {
            var text = File.ReadAllText(path);
            var ext = Path.GetExtension(path);
            return ext is ".cs" or ".cpp" or ".h" ? StripComments(text) : text;
        }
        catch (IOException) { return string.Empty; }
    }

    private static string CommentsOnly(string source)
    {
        var sb = new System.Text.StringBuilder();
        foreach (Match m in Regex.Matches(source, @"//[^\n]*|/\*.*?\*/", RegexOptions.Singleline))
            sb.AppendLine(m.Value);
        return sb.ToString();
    }

    private static string StripComments(string source)
        => Regex.Replace(source, @"//[^\n]*|/\*.*?\*/", " ", RegexOptions.Singleline);

    private static string FindRepoDir(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (Directory.Exists(candidate)) return candidate;
        }

        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
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
