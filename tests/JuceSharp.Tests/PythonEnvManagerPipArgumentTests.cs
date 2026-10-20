using System.Runtime.CompilerServices;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// <c>EnsurePackagesAsync</c> used to build ONE string —
/// <c>-m pip install … "pkg1" "pkg2"</c> — and hand it to
/// <c>ProcessStartInfo.Arguments</c>. A local wheel path with an
/// embedded quote, or one ending in a backslash (which escapes the closing
/// quote under the MSVCRT rules), was mis-parsed by the child. The argv is now
/// a list for <c>ProcessStartInfo.ArgumentList</c>, and each spec is
/// passed verbatim; the runtime does the escaping.
/// </summary>
public sealed class PythonEnvManagerPipArgumentTests
{
    [Fact]
    public void A_wheel_path_with_spaces_a_quote_and_a_trailing_backslash_is_one_verbatim_argument()
    {
        var wheel = @"C:\my wheels\weird ""name"" pkg\";
        var args = PythonEnvManager.BuildPipInstallArguments(new[] { "torch==2.3.0", wheel });

        Assert.Equal(new[]
        {
            "-m", "pip", "install", "--isolated", "--no-warn-script-location", "--disable-pip-version-check",
            "torch==2.3.0", wheel,
        }, args);

        // Nothing was quoted or escaped by us — that is the runtime's job.
        Assert.DoesNotContain(args, a => a.StartsWith('"'));
        Assert.Same(wheel, args[^1]);
    }

    [Fact]
    public void Blank_specs_are_dropped_before_they_reach_pip()
    {
        var args = PythonEnvManager.BuildPipInstallArguments(new[] { "", "  ", "numpy" });
        Assert.Equal("numpy", args[^1]);
        Assert.Equal(7, args.Count);
    }

    [Fact]
    public void EnsurePackagesAsync_no_longer_concatenates_a_quoted_string()
    {
        var source = File.ReadAllText(ManagerSourcePath());
        var start = source.IndexOf("public async Task EnsurePackagesAsync(", StringComparison.Ordinal);
        Assert.True(start >= 0);
        var end = source.IndexOf("internal static IReadOnlyList<string> BuildPipInstallArguments", start, StringComparison.Ordinal);
        Assert.True(end > start);
        var body = source.Substring(start, end - start);

        Assert.DoesNotContain("Append('\"')", body);
        Assert.DoesNotContain("new StringBuilder(", body);
        Assert.Contains("Run(Executable, BuildPipInstallArguments(list)", body);

        // And the get-pip bootstrap is on the list overload too — its script
        // path lives under the user's config root, which can contain spaces.
        Assert.Contains("Run(Executable, new[] { getPip, \"--no-warn-script-location\" }", source);
    }

    private static string ManagerSourcePath([CallerFilePath] string? thisFile = null)
    {
        var testsDir = Path.GetDirectoryName(thisFile!)!;
        var path = Path.GetFullPath(Path.Combine(testsDir, "..", "..", "src", "JuceSharp.StemSeparation", "Runtime", "PythonEnvManager.cs"));
        Assert.True(File.Exists(path), path);
        return path;
    }
}
