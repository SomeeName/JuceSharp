using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// Source lint: <b>the stem-separation packages fetch bytes in exactly ONE
/// place</b>, <c>Runtime/VerifiedDownloader.cs</c>.
///
/// <para>
/// Everything these packages download is subsequently EXECUTED or loaded as a
/// model — the Python embed archive is unzipped and run, an ONNX graph goes
/// straight into ONNX Runtime — so each download has to be written to a temp
/// file, hashed WHOLE, and only then renamed into place. That sequence is easy
/// to write in the wrong order, and the wrong order is invisible: the file is
/// there, the app works, right up until a truncated transfer or a substituted
/// mirror. A second <see cref="HttpClient"/> in these projects is a second
/// chance to get it wrong, and the way it arrives is never a rewrite — it is
/// one innocent <c>_http.GetAsync</c> in a new feature.
/// </para>
/// <para>
/// This is also the shape the DotCharter side had before 0.9: its
/// <c>StemModelManager</c> carries its own client, its own <c>.partial</c>
/// dance and a hash for two of its eleven entries.
/// </para>
/// </summary>
public sealed class HttpDownloadLintTests
{
    /// <summary>
    /// ALL SIX packable projects, not the two that happened to have a download
    /// in them when this lint was written.
    ///
    /// <para>
    /// "Only VerifiedDownloader reaches the network" was asserted over
    /// <c>JuceSharp.StemSeparation</c> and <c>.Onnx</c> alone, which makes the
    /// claim true of a third of the shipped surface and silent about the rest —
    /// and the way a second HttpClient arrives is never a rewrite, it is one
    /// innocent <c>GetAsync</c> in a new feature, in whichever project that
    /// feature lands in. <c>JuceSharp</c> itself already downloads nothing and
    /// must keep not doing so: it is the wrapper, and a fetch on its side would
    /// sit under the audio engine's lifetime rather than the sidecar's.
    /// Measured when this was widened: the four added projects contain zero
    /// matches, so the widening cost nothing and closed the gap.
    /// </para>
    /// </summary>
    private static readonly string[] LintedProjects =
    [
        "JuceSharp",
        "JuceSharp.AudioIo",
        "JuceSharp.Diagnostics",
        "JuceSharp.Dsp",
        "JuceSharp.StemSeparation",
        "JuceSharp.StemSeparation.Onnx",
    ];

    /// <summary>The one sanctioned site.</summary>
    private const string Downloader = "VerifiedDownloader.cs";

    /// <summary>
    /// Reading bytes off the wire, in every spelling that reaches a socket.
    /// <para>
    /// The bare type name <c>HttpClient</c> is an alternative on purpose, not
    /// laziness: the sanctioned field is TARGET-TYPED
    /// (<c>HttpClient Http = new()</c>), so a pattern hunting for
    /// <c>new HttpClient</c> would walk straight past the very shape it is
    /// meant to catch. Any mention of the type outside the downloader — in
    /// code; comments are stripped first — is the thing to look at.
    /// </para>
    /// </summary>
    private static readonly Regex Fetch = new(
        @"\b(HttpClient|WebClient|\.GetAsync\s*\(|\.GetStreamAsync\s*\(|\.GetByteArrayAsync\s*\(|" +
        @"\.SendAsync\s*\(|\.DownloadFile)",
        RegexOptions.Compiled);

    private static IEnumerable<(string File, string Text)> SourceFiles()
    {
        foreach (var project in LintedProjects)
        {
            var root = Path.Combine(RepoRoot(), "src", project);
            Assert.True(Directory.Exists(root), $"project folder '{root}' is missing — has it moved?");
            foreach (var file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
            {
                // bin/ and obj/ hold generated copies of nothing we wrote.
                var rel = Path.GetRelativePath(root, file).Replace('\\', '/');
                if (rel.StartsWith("bin/", StringComparison.Ordinal) ||
                    rel.StartsWith("obj/", StringComparison.Ordinal)) continue;
                yield return (file, File.ReadAllText(file));
            }
        }
    }

    /// <summary>
    /// THE COVERAGE half, and the reason the finding existed: the lint above is
    /// only as good as its project list, and a list written by hand goes stale
    /// the day a seventh package appears. This derives the expected set from
    /// the project files — every csproj under <c>src/</c> that declares a
    /// <c>PackageId</c> — and fails if the list does not name all of them.
    /// </summary>
    [Fact]
    public void The_lint_covers_every_packable_project()
    {
        var src = Path.Combine(RepoRoot(), "src");
        var packable = Directory.EnumerateFiles(src, "*.csproj", SearchOption.AllDirectories)
            .Where(f => !f.Replace('\\', '/').Contains("/build/", StringComparison.Ordinal))
            .Where(f => !f.Replace('\\', '/').Contains("/_deps/", StringComparison.Ordinal))
            .Where(f => !f.Replace('\\', '/').Contains("/third_party/", StringComparison.Ordinal))
            .Where(f => File.ReadAllText(f).Contains("<PackageId>", StringComparison.Ordinal))
            .Select(f => Path.GetFileNameWithoutExtension(f))
            .OrderBy(n => n, StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(6, packable.Length);
        Assert.Equal(packable, LintedProjects.OrderBy(n => n, StringComparer.Ordinal).ToArray());
    }

    [Fact]
    public void Only_VerifiedDownloader_reaches_the_network()
    {
        var offenders = new List<string>();

        foreach (var (file, text) in SourceFiles())
        {
            if (string.Equals(Path.GetFileName(file), Downloader, StringComparison.Ordinal)) continue;

            // Comments are exactly as inert as a `see cref` — strip them first,
            // or this file's own prose would fail its own lint.
            var code = StripComments(text);
            foreach (Match m in Fetch.Matches(code))
                offenders.Add($"{Path.GetFileName(file)}: {m.Value.Trim()}");
        }

        Assert.True(offenders.Count == 0,
            "These stem-separation sources fetch bytes themselves instead of going through " +
            $"{Downloader} (temp file → hash the COMPLETE file → atomic rename):\n  " +
            string.Join("\n  ", offenders));
    }

    /// <summary>
    /// Non-vacuity: the pattern the lint hunts for must actually be FOUND in
    /// the sanctioned file. A regex that matches nothing anywhere would pass
    /// the test above for the wrong reason.
    /// </summary>
    [Fact]
    public void The_lint_pattern_matches_the_one_sanctioned_site()
    {
        var downloader = SourceFiles().Single(f => Path.GetFileName(f.File) == Downloader);
        var code = StripComments(downloader.Text);

        Assert.Matches(Fetch, code);
        Assert.Contains("SendAsync", code, StringComparison.Ordinal);
        // …and it holds the only HttpClient in the two projects. Matched on the
        // DECLARATION, not on "new HttpClient": the field is target-typed
        // (`HttpClient Http = new()`), which is exactly the spelling a lint
        // looking for the constructor name would walk straight past — and it is
        // why the Fetch pattern above matches `HttpClient(` too.
        Assert.Contains("HttpClient Http = new", code, StringComparison.Ordinal);
    }

    /// <summary>
    /// The atomic-rename ORDER is the invariant, and it is one method, so pin
    /// it: hashing must precede the move. A future edit that moves first and
    /// verifies after would leave an unverified payload at the destination for
    /// a window — and a crash in that window makes it permanent.
    /// </summary>
    [Fact]
    public void The_downloader_verifies_before_it_renames()
    {
        var code = StripComments(SourceFiles().Single(f => Path.GetFileName(f.File) == Downloader).Text);

        var verify = code.IndexOf("VerifyFileHashAsync(tmp", StringComparison.Ordinal);
        var move = code.IndexOf("File.Move(tmp, dest", StringComparison.Ordinal);
        Assert.True(verify >= 0, "the hash check moved or was renamed");
        Assert.True(move >= 0, "the atomic rename moved or was renamed");
        Assert.True(verify < move, "the COMPLETE file must be hashed BEFORE it is renamed into place");
    }

    private static string StripComments(string source)
    {
        source = Regex.Replace(source, @"/\*.*?\*/", " ", RegexOptions.Singleline);
        source = Regex.Replace(source, @"//[^\n]*", " ");
        return source;
    }

    private static string RepoRoot([CallerFilePath] string? thisFile = null)
    {
        var dir = Path.GetDirectoryName(thisFile);
        while (dir != null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        return dir ?? throw new InvalidOperationException("Could not locate the JuceSharp root.");
    }
}
