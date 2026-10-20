using System.Runtime.CompilerServices;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// <c>EnsurePackagesAsync</c> used to build ONE string —
/// <c>-m pip install … "pkg1" "pkg2"</c> — and hand it to
/// <c>ProcessStartInfo.Arguments</c>. A path with an embedded quote, or one
/// ending in a backslash (which escapes the closing quote under the MSVCRT
/// rules), was mis-parsed by the child. The argv is a list for
/// <c>ProcessStartInfo.ArgumentList</c>, and every path in it is passed
/// verbatim; the runtime does the escaping.
///
/// <para>
/// Since the package set became hash-locked, the argv names no package at all:
/// pip reads the lock with <c>--require-hashes</c>, and these tests pin that
/// too — an install without it accepts whatever bytes the index serves.
/// </para>
/// </summary>
public sealed class PythonEnvManagerPipArgumentTests
{
    [Fact]
    public void A_lock_path_with_spaces_a_quote_and_a_trailing_backslash_is_one_verbatim_argument()
    {
        var lockPath = @"C:\my roots\weird ""name"" root\";
        var args = PythonEnvManager.BuildPipInstallArguments(lockPath);

        Assert.Equal(new[]
        {
            "-m", "pip", "install", "--isolated", "--no-warn-script-location", "--disable-pip-version-check",
            "--require-hashes", "--no-deps", "--only-binary", ":all:",
            "-r", lockPath,
        }, args);

        // Nothing was quoted or escaped by us — that is the runtime's job.
        Assert.DoesNotContain(args, a => a.StartsWith('"'));
        Assert.Same(lockPath, args[^1]);
    }

    /// <summary>The three flags that make the install hash-locked, each on its
    /// own: <c>--require-hashes</c> (every wheel proves its SHA-256),
    /// <c>--no-deps</c> (nothing outside the lock is resolved) and
    /// <c>--only-binary :all:</c> (no sdist build script ever runs).</summary>
    [Fact]
    public void The_pip_install_is_hash_locked()
    {
        var args = PythonEnvManager.BuildPipInstallArguments("lock.txt");
        Assert.Contains("--require-hashes", args);
        Assert.Contains("--no-deps", args);
        var ob = args.ToList().IndexOf("--only-binary");
        Assert.True(ob >= 0 && args[ob + 1] == ":all:");
        var r = args.ToList().IndexOf("-r");
        Assert.True(r >= 0 && args[r + 1] == "lock.txt");
    }

    [Fact]
    public void The_pip_bootstrap_installs_only_the_verified_wheel_with_no_index()
    {
        var wheel = @"C:\my roots\pip-26.2.1-py3-none-any.whl";
        var args = PythonEnvManager.BuildPipBootstrapArguments(wheel);

        Assert.Equal("-c", args[0]);
        Assert.Contains("'--no-index'", args[1]);
        Assert.Contains("'--no-deps'", args[1]);
        // The path is its own argv element, never spliced into the code.
        Assert.DoesNotContain(wheel, args[1]);
        Assert.Same(wheel, args[^1]);
    }

    [Fact]
    public void EnsurePackagesAsync_runs_pip_on_the_lock_through_the_argument_list()
    {
        var source = File.ReadAllText(ManagerSourcePath());
        var start = source.IndexOf("public async Task EnsurePackagesAsync(", StringComparison.Ordinal);
        Assert.True(start >= 0);
        var end = source.IndexOf("internal static IReadOnlyList<string> BuildPipInstallArguments", start, StringComparison.Ordinal);
        Assert.True(end > start);
        var body = source.Substring(start, end - start);

        Assert.DoesNotContain("Append('\"')", body);
        Assert.DoesNotContain("new StringBuilder(", body);
        Assert.Contains("Run(Executable, BuildPipInstallArguments(lockPath)", body);

        // And the pip bootstrap is on the list overload too — its wheel path
        // lives under the user's config root, which can contain spaces.
        Assert.Contains("Run(Executable, BuildPipBootstrapArguments(wheel)", source);
    }

    private static string ManagerSourcePath([CallerFilePath] string? thisFile = null)
    {
        var testsDir = Path.GetDirectoryName(thisFile!)!;
        var path = Path.GetFullPath(Path.Combine(testsDir, "..", "..", "src", "JuceSharp.StemSeparation", "Runtime", "PythonEnvManager.cs"));
        Assert.True(File.Exists(path), path);
        return path;
    }
}
