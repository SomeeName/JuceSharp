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
    /// The guard that keeps the PRIVATE dev repo manual-only. The public repo and
    /// the private dev repo carry the same workflow; public runs on push and
    /// pull_request cost nothing, private ones bill every minute. With this
    /// <c>if:</c> on every job a push or PR on the private repo creates a run
    /// whose jobs are all skipped, and the "Run workflow" button still runs it all.
    /// </summary>
    private const string PrivateRepoGate = "github.event_name == 'workflow_dispatch' || !github.event.repository.private";

    private static List<string> JobsThatCanRunUnattendedOnThePrivateRepo(Dictionary<object, object> workflow)
    {
        Assert.True(workflow.TryGetValue("on", out var on), "workflow declares no triggers.");
        var triggers = on switch
        {
            Dictionary<object, object> map => map.Keys.Select(k => (string)k).ToList(),
            List<object> list => list.Select(t => (string)t).ToList(),
            _ => new List<string> { (string)on! },
        };
        Assert.Contains("workflow_dispatch", triggers);
        if (triggers.All(t => t == "workflow_dispatch")) return [];

        const string suffix = $"&& ({PrivateRepoGate})";
        var ungated = new List<string>();
        foreach (var (id, job) in Jobs(workflow))
        {
            var condition = ((Dictionary<object, object>)job).TryGetValue("if", out var value) ? value as string : null;
            var expression = condition ?? "";
            if (expression.StartsWith("${{", StringComparison.Ordinal) && expression.EndsWith("}}", StringComparison.Ordinal))
                expression = expression[3..^2].Trim();
            var rest = expression == PrivateRepoGate ? ""
                : expression.EndsWith(suffix, StringComparison.Ordinal) ? expression[..^suffix.Length]
                : null;
            if (rest is null || rest.Contains("||", StringComparison.Ordinal))
                ungated.Add($"{id} (if: {condition ?? "<none>"})");
        }
        return ungated;
    }

    /// <summary>
    /// A trigger besides <c>workflow_dispatch</c> is allowed only when EVERY job
    /// is gated on <see cref="PrivateRepoGate"/> (alone, or AND-ed onto a
    /// condition with no <c>||</c> of its own).
    /// </summary>
    [Fact]
    public void Every_job_of_an_unattended_workflow_is_gated_off_the_private_repo()
    {
        foreach (var (name, doc) in Workflows())
        {
            var ungated = JobsThatCanRunUnattendedOnThePrivateRepo(doc);
            Assert.True(ungated.Count == 0,
                $"{name}: every job must carry `if: {PrivateRepoGate}` (or `${{{{ <condition> && ({PrivateRepoGate}) }}}}`) "
                + $"so the private dev repo stays manual-only; ungated: {string.Join(", ", ungated)}");
        }
    }

    /// <summary>The lint above reports an ungated push trigger and an OR-ed gate, and passes the gated shapes.</summary>
    [Theory]
    [InlineData("on:\n  push:\n  workflow_dispatch:\njobs:\n  a:\n    runs-on: x\n  b:\n    if: always()\n    runs-on: x\n", 2)]
    [InlineData("on:\n  pull_request:\n  workflow_dispatch:\njobs:\n  a:\n    if: ${{ always() || (" + PrivateRepoGate + ") }}\n    runs-on: x\n", 1)]
    [InlineData("on:\n  push:\n  workflow_dispatch:\njobs:\n  a:\n    if: " + PrivateRepoGate + "\n    runs-on: x\n  b:\n    if: ${{ always() && (" + PrivateRepoGate + ") }}\n    runs-on: x\n", 0)]
    [InlineData("on:\n  workflow_dispatch:\njobs:\n  a:\n    runs-on: x\n", 0)]
    public void The_private_repo_gate_lint_is_not_vacuous(string yaml, int expected)
    {
        var doc = new DeserializerBuilder().Build().Deserialize<Dictionary<object, object>>(yaml);
        Assert.Equal(expected, JobsThatCanRunUnattendedOnThePrivateRepo(doc).Count);
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
    /// Every third-party action runs with the repository's token. A branch
    /// ("@main") re-points under the workflow without a commit here, and so can a
    /// TAG: whoever controls the action's repo can move "v4". Only a full commit
    /// SHA is immutable, so every <c>uses:</c> names one (with the version in a
    /// comment). And no checkout leaves the token in <c>.git/config</c> for later
    /// steps: <c>persist-credentials: false</c> on every <c>actions/checkout</c>.
    /// </summary>
    [Fact]
    public void Every_action_is_pinned_to_a_commit_sha_and_no_checkout_persists_the_token()
    {
        var pinned = new Regex(@"^[A-Za-z0-9._-]+/[A-Za-z0-9._/-]+@[0-9a-f]{40}$");
        var seen = 0;
        var checkouts = 0;

        foreach (var (name, doc) in Workflows())
            foreach (var (jobName, job) in Jobs(doc))
                foreach (var step in Steps(Assert.IsType<Dictionary<object, object>>(job)))
                {
                    if (!step.TryGetValue("uses", out var uses)) continue;
                    seen++;
                    var reference = Assert.IsType<string>(uses);
                    Assert.True(pinned.IsMatch(reference),
                        $"{name}:{jobName} uses '{reference}', which is not pinned to a full commit SHA.");
                    if (!reference.StartsWith("actions/checkout@", StringComparison.Ordinal)) continue;
                    checkouts++;
                    var with = step.TryGetValue("with", out var w) ? w as Dictionary<object, object> : null;
                    Assert.True(with is not null && with.TryGetValue("persist-credentials", out var persist) && (string)persist == "false",
                        $"{name}:{jobName} checks out without `persist-credentials: false`.");
                }

        Assert.True(seen >= 4, $"only {seen} `uses:` steps found — has the workflow moved?");
        Assert.True(checkouts >= 2, $"only {checkouts} checkouts found — has the workflow moved?");
    }

    /// <summary>
    /// The event is part of the concurrency group, so a push run (all jobs
    /// skipped on the private repo) can never cancel a manual run of the same ref.
    /// </summary>
    [Fact]
    public void The_concurrency_group_is_per_event()
    {
        foreach (var (name, doc) in Workflows())
        {
            if (!doc.TryGetValue("concurrency", out var concurrency)) continue;
            var group = Assert.IsType<string>(Assert.IsType<Dictionary<object, object>>(concurrency)[(object)"group"]);
            Assert.Contains("github.event_name", group, StringComparison.Ordinal);
        }
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
