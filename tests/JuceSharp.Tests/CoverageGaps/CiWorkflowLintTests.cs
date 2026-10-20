using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.RegularExpressions;
using Xunit;
using YamlDotNet.Serialization;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// Toolchain and CI-permission invariants, read off the YAML as a PARSED
/// DOCUMENT rather than as text.
///
/// <para>
/// Everything this file pins was floating: the workflow ran on
/// <c>windows-latest</c> / <c>ubuntu-latest</c> and installed
/// <c>dotnet-version: "10.x"</c>, so the Visual Studio, CMake, Windows SDK and
/// .NET SDK versions that compiled a given commit were whatever GitHub happened
/// to ship that morning — and a build that breaks on a day nobody pushed is the
/// most expensive kind of CI break there is. <c>global.json</c> named a test
/// runner and no SDK at all, so the same commit could be built locally on a
/// different SDK with a different analyzer rule set. And there was no
/// <c>permissions:</c> block, so the default <c>GITHUB_TOKEN</c> inherited the
/// repository default — read/write on everything for an older repo — and handed
/// it to every action in every pull-request build.
/// </para>
/// <para>
/// A substring lint over CI YAML is worse than none here: "permissions:" would
/// match a comment saying there is no permissions block, and an indentation
/// slip that nests a key under the wrong job is invisible to a regex and fatal
/// on the runner. These read the deserialised document.
/// </para>
/// </summary>
public sealed class CiWorkflowLintTests
{
    private static string RepoRoot([CallerFilePath] string? thisFile = null)
    {
        var dir = Path.GetDirectoryName(thisFile);
        while (dir != null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        return dir ?? throw new InvalidOperationException("Could not locate the JuceSharp root.");
    }

    private static string WorkflowDir() => Path.Combine(RepoRoot(), ".github", "workflows");

    private static IEnumerable<(string Name, Dictionary<object, object> Doc)> Workflows()
    {
        var files = Directory.EnumerateFiles(WorkflowDir(), "*.yml").ToList();
        Assert.NotEmpty(files);   // a lint over a folder that has emptied is vacuous.

        var deserializer = new DeserializerBuilder().Build();
        foreach (var file in files.OrderBy(f => f, StringComparer.Ordinal))
        {
            var doc = deserializer.Deserialize<Dictionary<object, object>>(File.ReadAllText(file));
            Assert.NotNull(doc);
            yield return (Path.GetFileName(file), doc!);
        }
    }

    private static Dictionary<object, object> Jobs(Dictionary<object, object> workflow)
    {
        Assert.True(workflow.TryGetValue("jobs", out var jobs), "workflow declares no jobs.");
        var map = Assert.IsType<Dictionary<object, object>>(jobs);
        Assert.NotEmpty(map);
        return map;
    }

    private static IEnumerable<Dictionary<object, object>> Steps(Dictionary<object, object> job)
    {
        if (!job.TryGetValue("steps", out var steps)) yield break;
        foreach (var step in (List<object>)steps)
            yield return (Dictionary<object, object>)step;
    }

    /// <summary>
    /// The default <c>GITHUB_TOKEN</c> must be scoped by the workflow, not by
    /// whatever the repository settings happen to say. Declaring it at the
    /// TOP LEVEL is the part that matters: a per-job block leaves any job added
    /// later on the inherited default.
    /// </summary>
    [Fact]
    public void Every_workflow_scopes_the_default_token_at_the_top_level()
    {
        foreach (var (name, doc) in Workflows())
        {
            Assert.True(doc.TryGetValue("permissions", out var permissions),
                $"{name} declares no top-level `permissions:`; the GITHUB_TOKEN inherits the repository default.");

            // `permissions: read-all` is also a legal answer; a MAP has to at
            // least grant contents, and must grant nothing write.
            if (permissions is string scalar)
            {
                Assert.Equal("read-all", scalar);
                continue;
            }

            var map = Assert.IsType<Dictionary<object, object>>(permissions);
            Assert.NotEmpty(map);
            Assert.Equal("read", Assert.IsType<string>(map[(object)"contents"]));

            foreach (var (scope, value) in map)
                Assert.True((string)value is "read" or "none",
                    $"{name} grants '{scope}: {value}' at the top level; nothing in this workflow writes.");
        }
    }

    /// <summary>
    /// A floating runner label moves the whole native toolchain under the build
    /// on GitHub's schedule.
    /// </summary>
    [Fact]
    public void No_job_runs_on_a_floating_runner_image()
    {
        foreach (var (name, doc) in Workflows())
            foreach (var (jobName, job) in Jobs(doc))
            {
                var map = Assert.IsType<Dictionary<object, object>>(job);
                var runsOn = Assert.IsType<string>(map[(object)"runs-on"]);

                Assert.False(runsOn.EndsWith("-latest", StringComparison.Ordinal),
                    $"{name}:{jobName} runs on '{runsOn}'. Pin the image version — " +
                    "the label moves and takes Visual Studio, CMake and the Windows SDK with it.");
                // …and it must still name a version, not a self-hosted alias.
                // windows-2025, ubuntu-24.04, macos-15-large …
                Assert.Matches(new Regex(@"-\d{2,4}(\.\d+)?(-[a-z0-9]+)?$"), runsOn);
            }
    }

    /// <summary>
    /// Every third-party action runs with the repository's token. A ref that is
    /// a BRANCH ("@main") re-points under the workflow without a commit here.
    /// A major tag or a full commit SHA are both acceptable; a branch is not.
    /// </summary>
    [Fact]
    public void Every_action_is_pinned_to_a_tag_or_a_sha()
    {
        var pinned = new Regex(@"^[A-Za-z0-9._-]+/[A-Za-z0-9._/-]+@(v\d+(\.\d+)*|[0-9a-f]{40})$");
        var seen = 0;

        foreach (var (name, doc) in Workflows())
            foreach (var (jobName, job) in Jobs(doc))
                foreach (var step in Steps(Assert.IsType<Dictionary<object, object>>(job)))
                {
                    if (!step.TryGetValue("uses", out var uses)) continue;
                    seen++;
                    var reference = Assert.IsType<string>(uses);
                    Assert.True(pinned.IsMatch(reference),
                        $"{name}:{jobName} uses '{reference}', which is not pinned to a version tag or a SHA.");
                }

        Assert.True(seen >= 4, $"only {seen} `uses:` steps found — has the workflow moved?");
    }

    /// <summary>
    /// <c>global.json</c> is the ONE place the SDK is declared, and CI has to
    /// read it rather than resolve its own. <c>rollForward</c> is the half that
    /// keeps the pin from being a machine-by-machine install chore: it allows a
    /// newer patch/feature band of the same major, never a different major.
    /// </summary>
    [Fact]
    public void The_sdk_is_pinned_in_global_json_and_ci_installs_that_one()
    {
        using var json = JsonDocument.Parse(File.ReadAllText(Path.Combine(RepoRoot(), "global.json")));

        Assert.True(json.RootElement.TryGetProperty("sdk", out var sdk),
            "global.json declares no `sdk` section; the SDK that builds this repo is whatever is installed.");

        var version = sdk.GetProperty("version").GetString();
        Assert.True(Version.TryParse(version, out _), $"sdk.version '{version}' is not a version.");

        var roll = sdk.GetProperty("rollForward").GetString();
        Assert.Contains(roll, new[] { "latestPatch", "latestFeature", "latestMinor" });
        Assert.False(sdk.GetProperty("allowPrerelease").GetBoolean());

        // The runner must install THAT SDK, not resolve "10.x" for itself.
        var setups = 0;
        foreach (var (name, doc) in Workflows())
            foreach (var (jobName, job) in Jobs(doc))
                foreach (var step in Steps(Assert.IsType<Dictionary<object, object>>(job)))
                {
                    if (step.TryGetValue("uses", out var uses) &&
                        ((string)uses).StartsWith("actions/setup-dotnet@", StringComparison.Ordinal))
                    {
                        setups++;
                        var with = Assert.IsType<Dictionary<object, object>>(step[(object)"with"]);
                        Assert.True(with.ContainsKey("global-json-file"),
                            $"{name}:{jobName} pins its own dotnet-version instead of reading global.json.");
                        Assert.False(with.ContainsKey("dotnet-version"),
                            $"{name}:{jobName} sets both global-json-file and dotnet-version.");
                    }
                }

        Assert.True(setups >= 1, "no actions/setup-dotnet step found — has the workflow moved?");
    }
}
