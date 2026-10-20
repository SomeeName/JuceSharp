using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// <b>Every dependency CMake downloads into the native binary has an entry in
/// <c>THIRD-PARTY-NOTICES.md</c> naming its repository and the tag it is pinned
/// to.</b>
///
/// <para>
/// <c>juce_sharp.dll</c> is distributed under the licences of what it links, so
/// a <c>FetchContent_Declare</c> added without a notice ships a binary whose
/// notices are incomplete, and a tag bump without one leaves the notice naming
/// a version the binary no longer contains. The tag is resolved through the
/// <c>set(X_TAG "…")</c> line the declaration reads, so the check follows the
/// pin rather than a copy of it.
/// </para>
/// </summary>
public sealed class ThirdPartyNoticeCoverageTests
{
    private static readonly Regex Declare = new(
        @"FetchContent_Declare\(\s*(?<name>[A-Za-z0-9_]+)(?<body>[^)]*)\)",
        RegexOptions.Compiled);

    private static readonly Regex Repository = new(
        @"GIT_REPOSITORY\s+(?<url>\S+)", RegexOptions.Compiled);

    private static readonly Regex Tag = new(
        @"GIT_TAG\s+(?<tag>\S+)", RegexOptions.Compiled);

    private static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "JuceSharp.sln")))
            dir = dir.Parent;
        Assert.NotNull(dir);
        return dir!.FullName;
    }

    private static IEnumerable<string> CMakeFiles(string root)
    {
        static bool IsBuildOutput(string f)
        {
            var rel = f.Replace('\\', '/');
            return rel.Contains("/build/", StringComparison.Ordinal)
                || rel.Contains("/_deps/", StringComparison.Ordinal)
                || rel.Contains("/out/", StringComparison.Ordinal)
                || rel.Contains("/bin/", StringComparison.Ordinal)
                || rel.Contains("/obj/", StringComparison.Ordinal);
        }

        return Directory.EnumerateFiles(Path.Combine(root, "src"), "CMakeLists.txt", SearchOption.AllDirectories)
                        .Where(f => !IsBuildOutput(f))
                        .OrderBy(f => f, StringComparer.Ordinal);
    }

    private static string ResolveVariables(string value, string cmake) =>
        Regex.Replace(value, @"\$\{(?<var>[A-Za-z0-9_]+)\}", m =>
        {
            var set = Regex.Match(cmake,
                @"set\(\s*" + Regex.Escape(m.Groups["var"].Value) + @"\s+""(?<v>[^""]+)""");
            Assert.True(set.Success, $"no set({m.Groups["var"].Value} \"…\") for {value}");
            return set.Groups["v"].Value;
        });

    internal static IReadOnlyList<(string Name, string Url, string Tag)> FetchedDependencies(string root)
    {
        var found = new List<(string, string, string)>();
        foreach (var file in CMakeFiles(root))
        {
            var cmake = File.ReadAllText(file);
            foreach (Match d in Declare.Matches(cmake))
            {
                var body = d.Groups["body"].Value;
                var url = Repository.Match(body);
                var tag = Tag.Match(body);
                Assert.True(url.Success, $"{d.Groups["name"].Value} in {file} has no GIT_REPOSITORY");
                Assert.True(tag.Success, $"{d.Groups["name"].Value} in {file} has no GIT_TAG");
                found.Add((d.Groups["name"].Value,
                           ResolveVariables(url.Groups["url"].Value, cmake),
                           ResolveVariables(tag.Groups["tag"].Value, cmake)));
            }
        }

        return found;
    }

    [Fact]
    public void Every_FetchContent_dependency_has_a_notice_naming_its_repository_and_pinned_tag()
    {
        var root = RepoRoot();
        var deps = FetchedDependencies(root);

        // Not vacuous: the scan must see the two dependencies every build links.
        Assert.Contains(deps, d => d.Name == "JUCE");
        Assert.Contains(deps, d => d.Name == "rubberband");
        Assert.True(deps.Count >= 4, $"expected at least 4 FetchContent dependencies, found {deps.Count}");

        var notices = File.ReadAllText(Path.Combine(root, "THIRD-PARTY-NOTICES.md"));
        foreach (var (name, url, tag) in deps)
        {
            var repo = url.EndsWith(".git", StringComparison.Ordinal) ? url[..^4] : url;
            Assert.True(notices.Contains(repo, StringComparison.Ordinal),
                $"THIRD-PARTY-NOTICES.md does not name {name}'s repository {repo}");
            Assert.True(notices.Contains(tag, StringComparison.Ordinal),
                $"THIRD-PARTY-NOTICES.md does not name {name}'s pinned tag {tag}");
        }
    }

    /// <summary>
    /// The MIT grant stays the plain MIT text, and the statement after it says
    /// the native binaries are AGPLv3 and points at the notices; the README
    /// says the same.
    /// </summary>
    [Fact]
    public void The_licence_states_the_binary_terms_without_altering_the_MIT_grant()
    {
        var root = RepoRoot();
        var licence = File.ReadAllText(Path.Combine(root, "LICENSE")).ReplaceLineEndings("\n");

        Assert.StartsWith("MIT License\n", licence, StringComparison.Ordinal);
        var statement = licence.IndexOf("LICENCE OF THE NATIVE BINARIES", StringComparison.Ordinal);
        Assert.True(statement > licence.IndexOf("OTHER DEALINGS IN THE\nSOFTWARE.", StringComparison.Ordinal),
            "the binary-licence statement must follow the complete MIT text");
        var tail = licence[statement..];
        foreach (var needle in new[] { "juce_sharp.dll", "juce_vst_host.exe", "AGPLv3", "THIRD-PARTY-NOTICES.md" })
            Assert.Contains(needle, tail, StringComparison.Ordinal);

        var readme = File.ReadAllText(Path.Combine(root, "README.md"));
        foreach (var needle in new[] { "AGPLv3", "THIRD-PARTY-NOTICES.md", "juce_vst_host.exe" })
            Assert.Contains(needle, readme, StringComparison.Ordinal);
    }
}
