using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The cancel contract for the python sidecar runner.
///
/// <para>
/// <c>PythonEnvManager.Run</c> used to swallow a cancellation into
/// <c>(ok: false, stderr: "Cancelled.")</c>. Three of its four callers
/// special-cased that string; <c>EnsurePipAsync</c>,
/// <c>EnsurePackagesAsync</c> and <c>CkptToOnnxConverter.ConvertAsync</c>
/// did not, and turned it into
/// <c>InvalidOperationException("pip install failed … stderr: Cancelled.")</c> —
/// so a user pressing Cancel during the multi-GB first-run torch install was
/// told the install had failed.
/// </para>
/// <para>
/// A TIMEOUT keeps the old <c>ok:false</c> shape: the caller's token is fine,
/// it is the command that did not finish.
/// </para>
/// </summary>
public sealed class PythonEnvManagerCancellationTests
{
    /// <summary>A child that runs for ~30 s and prints its first line at
    /// once, so the test can cancel a genuinely in-flight process rather
    /// than racing the spawn.</summary>
    private static (string exe, string[] args)? SlowChild()
    {
        if (OperatingSystem.IsWindows())
        {
            var ping = Path.Combine(Environment.SystemDirectory, "ping.exe");
            return File.Exists(ping) ? (ping, ["-n", "30", "127.0.0.1"]) : null;
        }
        foreach (var sh in new[] { "/bin/sh", "/usr/bin/sh" })
            if (File.Exists(sh)) return (sh, ["-c", "echo START; sleep 30"]);
        return null;
    }

    [Fact]
    public async Task Run_throws_OperationCanceledException_when_the_token_is_cancelled()
    {
        var child = SlowChild();
        if (child is null)
        {
            Assert.Skip("No long-running child process available on this platform.");
            return;
        }
        var (exe, args) = child.Value;

        using var cts = new CancellationTokenSource();
        // Cancel the moment the child proves it is alive; the timer is only
        // a backstop so the test cannot hang if the child prints nothing.
        cts.CancelAfter(TimeSpan.FromSeconds(10));

        var oce = await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await PythonEnvManager.RunStreaming(
                exe, args, timeout: TimeSpan.FromSeconds(30),
                onLine: _ => cts.Cancel(),
                cts.Token));

        Assert.Equal(cts.Token, oce.CancellationToken);
    }

    [Fact]
    public async Task Run_throws_immediately_for_an_already_cancelled_token()
    {
        var child = SlowChild();
        if (child is null)
        {
            Assert.Skip("No long-running child process available on this platform.");
            return;
        }
        var (exe, args) = child.Value;

        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await PythonEnvManager.RunStreaming(
                exe, args, timeout: TimeSpan.FromSeconds(30), onLine: null, cts.Token));
    }

    /// <summary>
    /// The other half of the contract, and the reason the fix is not simply
    /// "throw on any linked-token trip": a timeout is a failure OF THE RUN
    /// and must stay reportable as one.
    /// </summary>
    [Fact]
    public async Task Run_still_reports_a_timeout_as_a_failed_run_not_a_cancellation()
    {
        var child = SlowChild();
        if (child is null)
        {
            Assert.Skip("No long-running child process available on this platform.");
            return;
        }
        var (exe, args) = child.Value;

        var (ok, _, stderr) = await PythonEnvManager.RunStreaming(
            exe, args, timeout: TimeSpan.FromMilliseconds(250), onLine: null,
            TestContext.Current.CancellationToken);

        Assert.False(ok);
        Assert.Contains("Timed out", stderr, StringComparison.Ordinal);
        Assert.DoesNotContain("Cancelled", stderr, StringComparison.Ordinal);
    }
}
