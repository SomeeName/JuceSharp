using System.Diagnostics;
using System.Globalization;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

[CollectionDefinition("StemSeparatorConcurrency", DisableParallelization = true)]
public class StemSeparatorConcurrencyCollection { }

/// <summary>
/// Tests for the process-global concurrency throttler in
/// <see cref="StemSeparator"/> and for the <see cref="ChildProcessRegistry"/>
/// teardown contract. All tests run offline against a system-installed
/// Python so they finish in well under 10 s on CI machines that have
/// python on PATH and are silently skipped otherwise (mirroring the
/// existing pattern in <c>StemSeparatorTests</c>).
/// </summary>
[Collection("StemSeparatorConcurrency")]
public class StemSeparatorConcurrencyTests : IDisposable
{
    // TempRoot() is called from inside the parallel Run() closures below, and
    // List<T>.Add is not thread-safe — it tore _size / lost entries / threw
    // IndexOutOfRangeException straight out of Add.
    private readonly System.Collections.Concurrent.ConcurrentBag<string> _scratch = [];

    public void Dispose()
    {
        // Reset the gate so a later test in the same run isn't poisoned
        // by a Release that this fixture made.
        StemSeparator.ResetGlobalGateForTests();
        foreach (var d in _scratch) { try { Directory.Delete(d, true); } catch (Exception ex) { Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    private string TempRoot()
    {
        var p = Path.Combine(Path.GetTempPath(), "jucesharp-conc-" + Guid.NewGuid());
        Directory.CreateDirectory(p);
        _scratch.Add(p);
        return p;
    }

    private string TempFile(string ext, byte[] body)
    {
        var p = Path.Combine(TempRoot(), "x" + ext);
        File.WriteAllBytes(p, body);
        return p;
    }

    private static string? ResolveSystemPython()
    {
        var path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrEmpty(path)) return null;
        foreach (var dir in path.Split(Path.PathSeparator))
            foreach (var name in new[] { "python3", "python" })
            {
                var ext = OperatingSystem.IsWindows() ? ".exe" : string.Empty;
                var full = Path.Combine(dir, name + ext);
                if (File.Exists(full)) return full;
            }
        return null;
    }

    /// <summary>
    /// A python script that measures OVERLAP directly instead of leaving the
    /// test to infer it from a stopwatch.
    /// <list type="number">
    ///   <item>Drops a liveness marker <c>start_&lt;PID&gt;</c> into the SHARED
    ///     <paramref name="markerDir"/> the moment it boots.</item>
    ///   <item>Sleeps, so concurrent peers have time to boot.</item>
    ///   <item>Counts the markers still present, writes that count to
    ///     <c>peers_&lt;PID&gt;</c>, and REMOVES its own marker.</item>
    ///   <item>Emits a STEM line so <c>StemSeparator</c> considers it successful.</item>
    /// </list>
    /// <para>Because each child removes its marker before exiting, the peak
    /// count IS the concurrency the gate permitted. The tests used to assert on
    /// total wall-clock instead ("&gt;= 2700 ms" / "&lt; 4200 ms"), and the upper
    /// bound was the live flake: three cold python starts on a loaded box blow
    /// past 4.2 s with the gate working perfectly. A count cannot be slow.</para>
    /// </summary>
    private static async Task<string> WriteOverlapScriptAsync(string dir, string markerDir, double sleepSeconds = 2.0)
    {
        var script = Path.Combine(dir, "overlap.py");
        var py = $$"""
            import argparse, os, sys, time
            ap = argparse.ArgumentParser()
            ap.add_argument("--input"); ap.add_argument("--out")
            ap.add_argument("--model"); ap.add_argument("--device")
            ap.add_argument("--shifts", type=int); ap.add_argument("--overlap", type=float)
            ap.add_argument("--model-path", dest="model_path", default=None)
            ap.add_argument("--config", default=None)
            ap.add_argument("--stems", default=None)
            a = ap.parse_args()
            markers = r"{{markerDir}}"
            os.makedirs(markers, exist_ok=True)
            os.makedirs(a.out, exist_ok=True)
            barrier = os.path.join(markers, f"start_{os.getpid()}")
            open(barrier, "w").close()
            sys.stdout.flush()
            print("DEVICE cpu", flush=True)
            print("READY", flush=True)
            time.sleep({{sleepSeconds.ToString("0.###", CultureInfo.InvariantCulture)}})
            peers = len([f for f in os.listdir(markers) if f.startswith("start_")])
            open(os.path.join(markers, f"peers_{os.getpid()}"), "w").write(str(peers))
            os.remove(barrier)
            stem_path = os.path.join(a.out, "vocals.wav")
            open(stem_path, "wb").write(b"\x00" * 8)
            print(f"STEM vocals {stem_path}", flush=True)
            """;
        await File.WriteAllTextAsync(script, py);
        return script;
    }

    /// <summary>Peer counts observed by each child that ran, one per process.</summary>
    private static int[] ObservedConcurrency(string markerDir) =>
        Directory.GetFiles(markerDir, "peers_*")
                 .Select(f => int.Parse(File.ReadAllText(f), CultureInfo.InvariantCulture))
                 .ToArray();

    [Fact]
    public void GlobalConcurrencyLimit_defaults_to_one_and_can_only_grow()
    {
        StemSeparator.ResetGlobalGateForTests();
        Assert.Equal(1, StemSeparator.GlobalConcurrencyLimit);
        StemSeparator.GlobalConcurrencyLimit = 4;
        Assert.Equal(4, StemSeparator.GlobalConcurrencyLimit);
        // Shrinking would race with in-flight separations, so the setter REFUSES
        // it. (It used to be a silent no-op; this test still asserted the no-op
        // long after the throw shipped, so it was failing on every run.)
        Assert.Throws<InvalidOperationException>(() => StemSeparator.GlobalConcurrencyLimit = 2);
        Assert.Equal(4, StemSeparator.GlobalConcurrencyLimit);
        Assert.Throws<InvalidOperationException>(() => StemSeparator.GlobalConcurrencyLimit = 0);
        Assert.Equal(4, StemSeparator.GlobalConcurrencyLimit);
    }

    [Fact]
    public async Task Concurrent_calls_are_serialised_by_default()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");
        StemSeparator.ResetGlobalGateForTests();

        var dir = TempRoot();
        var markers = TempRoot();
        var script = await WriteOverlapScriptAsync(dir, markers, sleepSeconds: 1.5);
        var input = TempFile(".wav", [1, 2, 3]);

        async Task Run()
        {
            var env = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };
            var sep = new StemSeparator(env, scriptPathOverride: script);
            await sep.SeparateTrackAsync(input, TempRoot(),
                options: new StemSeparationOptions { SkipPackageBootstrap = true }, ct: TestContext.Current.CancellationToken);
        }

        await Task.WhenAll(Run(), Run());

        // The default gate has ONE slot, so it is structurally impossible for
        // two children to be alive at the same instant — every child must have
        // seen only itself. Unlike the elapsed-time bound this replaced, a slow
        // machine cannot break it.
        var observed = ObservedConcurrency(markers);
        Assert.Equal(2, observed.Length);
        Assert.All(observed, n => Assert.Equal(1, n));
    }

    [Fact]
    public async Task MaxConcurrentSeparations_allows_parallel_execution_when_raised()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");
        StemSeparator.ResetGlobalGateForTests();

        var dir = TempRoot();
        var markers = TempRoot();
        var script = await WriteOverlapScriptAsync(dir, markers, sleepSeconds: 1.5);
        var input = TempFile(".wav", [1, 2, 3]);

        async Task Run()
        {
            var env = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };
            var sep = new StemSeparator(env, scriptPathOverride: script);
            await sep.SeparateTrackAsync(input, TempRoot(),
                options: new StemSeparationOptions
                {
                    SkipPackageBootstrap = true,
                    MaxConcurrentSeparations = 4,
                }, ct: TestContext.Current.CancellationToken);
        }

        await Task.WhenAll(Run(), Run(), Run());

        // A 4-slot gate must let at least two children be alive at once. This
        // replaced an "elapsed < 4.2 s" bound that a loaded box broke while the
        // gate was working perfectly — the flake AGENTS.md warns about.
        var observed = ObservedConcurrency(markers);
        Assert.Equal(3, observed.Length);
        Assert.Contains(observed, n => n >= 2);
    }

    [Fact]
    public async Task Gate_is_released_after_a_failed_run()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");
        StemSeparator.ResetGlobalGateForTests();

        // Script that immediately exits with code 7 — separator throws.
        var dir = TempRoot();
        var fail = Path.Combine(dir, "fail.py");
        await File.WriteAllTextAsync(fail, """
            import sys
            sys.stderr.write("boom\n")
            sys.exit(7)
            """, TestContext.Current.CancellationToken);
        var ok = Path.Combine(dir, "ok.py");
        await File.WriteAllTextAsync(ok, """
            import argparse, os
            ap = argparse.ArgumentParser()
            ap.add_argument("--input"); ap.add_argument("--out")
            ap.add_argument("--model"); ap.add_argument("--device")
            ap.add_argument("--shifts", type=int); ap.add_argument("--overlap", type=float)
            ap.add_argument("--model-path", dest="model_path", default=None)
            ap.add_argument("--config", default=None)
            ap.add_argument("--stems", default=None)
            a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
            p = os.path.join(a.out, "v.wav"); open(p,"wb").write(b"\x00"*8)
            print(f"STEM vocals {p}", flush=True)
            """, TestContext.Current.CancellationToken);

        var input = TempFile(".wav", [1, 2, 3]);
        var env = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };

        await Assert.ThrowsAsync<StemSeparationException>(() =>
            new StemSeparator(env, scriptPathOverride: fail).SeparateTrackAsync(
                input, TempRoot(),
                options: new StemSeparationOptions { SkipPackageBootstrap = true }, ct: TestContext.Current.CancellationToken));

        // If the gate had been leaked, this second call would deadlock.
        // Use a hard wall-clock to fail fast instead.
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(
            TestContext.Current.CancellationToken);
        cts.CancelAfter(TimeSpan.FromSeconds(30));
        var result = await new StemSeparator(env, scriptPathOverride: ok).SeparateTrackAsync(
            input, TempRoot(),
            options: new StemSeparationOptions { SkipPackageBootstrap = true },
            ct: cts.Token);
        Assert.NotEmpty(result.Stems);
    }

    [Fact]
    public async Task Gate_is_released_after_cancellation()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");
        StemSeparator.ResetGlobalGateForTests();

        var dir = TempRoot();
        var spin = Path.Combine(dir, "spin.py");
        await File.WriteAllTextAsync(spin, """
            import argparse, time
            ap = argparse.ArgumentParser()
            ap.add_argument("--input"); ap.add_argument("--out")
            ap.add_argument("--model"); ap.add_argument("--device")
            ap.add_argument("--shifts", type=int); ap.add_argument("--overlap", type=float)
            ap.add_argument("--model-path", dest="model_path", default=None)
            ap.add_argument("--config", default=None)
            ap.add_argument("--stems", default=None)
            ap.parse_args()
            for _ in range(60): time.sleep(1)
            """, TestContext.Current.CancellationToken);

        var input = TempFile(".wav", [1, 2, 3]);
        var env = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };

        using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(1)))
        {
            await Assert.ThrowsAnyAsync<Exception>(() =>
                new StemSeparator(env, scriptPathOverride: spin).SeparateTrackAsync(
                    input, TempRoot(),
                    options: new StemSeparationOptions { SkipPackageBootstrap = true },
                    ct: cts.Token));
        }

        // Subsequent call must acquire instantly (gate released).
        var liveDir = TempRoot();
        var liveScript = Path.Combine(liveDir, "ok.py");
        await File.WriteAllTextAsync(liveScript, """
            import argparse, os
            ap = argparse.ArgumentParser()
            ap.add_argument("--input"); ap.add_argument("--out")
            ap.add_argument("--model"); ap.add_argument("--device")
            ap.add_argument("--shifts", type=int); ap.add_argument("--overlap", type=float)
            ap.add_argument("--model-path", dest="model_path", default=None)
            ap.add_argument("--config", default=None)
            ap.add_argument("--stems", default=None)
            a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
            p = os.path.join(a.out, "v.wav"); open(p,"wb").write(b"\x00"*8)
            print(f"STEM vocals {p}", flush=True)
            """, TestContext.Current.CancellationToken);

        using var cts2 = CancellationTokenSource.CreateLinkedTokenSource(
            TestContext.Current.CancellationToken);
        cts2.CancelAfter(TimeSpan.FromSeconds(15));
        var result = await new StemSeparator(env, scriptPathOverride: liveScript).SeparateTrackAsync(
            input, TempRoot(),
            options: new StemSeparationOptions { SkipPackageBootstrap = true },
            ct: cts2.Token);
        Assert.NotEmpty(result.Stems);
    }

    [Fact]
    public async Task Concurrent_calls_emit_queued_progress_phase_on_waiters()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");
        StemSeparator.ResetGlobalGateForTests();

        var dir = TempRoot();
        var script = await WriteOverlapScriptAsync(dir, TempRoot(), sleepSeconds: 1.2);
        var input = TempFile(".wav", [1, 2, 3]);

        var firstStarted = new TaskCompletionSource();
        var phasesA = new List<string>();
        var phasesB = new List<string>();

        async Task RunWithLog(List<string> bucket, Action onFirstProgress)
        {
            var env = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };
            var sep = new StemSeparator(env, scriptPathOverride: script);
            var first = true;
            var prog = new Progress<StemSeparationProgress>(p =>
            {
                lock (bucket) bucket.Add(p.Phase);
                if (first) { first = false; onFirstProgress(); }
            });
            await sep.SeparateTrackAsync(input, TempRoot(),
                new StemSeparationOptions { SkipPackageBootstrap = true }, prog, TestContext.Current.CancellationToken);
        }

        var t1 = RunWithLog(phasesA, () => firstStarted.TrySetResult());
        // Bounded: an unbounded await here hung the whole run forever whenever
        // the first runner died before its first report.
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(60), TestContext.Current.CancellationToken);
        var t2 = RunWithLog(phasesB, () => { });

        await Task.WhenAll(t1, t2);

        // The second runner had to wait → must have emitted a "queued" phase.
        //
        // NOTE this assertion is currently WEAK on purpose-of-record:
        // StemSeparator reports "queued" UNCONDITIONALLY before awaiting the
        // gate, so a caller that never waited also emits it. Tightening it
        // means changing production (report only when `gate.Wait(0)` fails),
        // which is a behaviour change to the progress contract — left for the
        // owner rather than slipped into an audit. See the JuceSharp AGENTS.md
        // validation section.
        Assert.Contains("queued", phasesB);
    }
}

/// <summary>
/// Tests for <see cref="ChildProcessRegistry"/>.
/// <para>These used to shell out to the system python, which made every one of
/// them silently PASS on a machine without it. The registry does not care what
/// the child is, so they now use the OS shell — always present, starts in
/// milliseconds, and lets the tests assert rather than skip.</para>
/// <para>No sleeps: exit is observed through a SECOND <c>Exited</c> handler
/// subscribed after <c>Register</c>'s. Multicast delegates fire in subscription
/// order, so when ours runs the registry's has already completed. The real
/// kill-on-parent-death is enforced by the OS kernel and cannot be tested
/// in-process; that is what the job object is for.</para>
/// </summary>
[Collection("StemSeparatorConcurrency")]
public class ChildProcessRegistryTests
{
    /// <summary>A child that exits immediately with <paramref name="exitCode"/>.</summary>
    private static Process StartExitingChild(int exitCode) => StartShell(
        windows: $"/c exit {exitCode}",
        posix:   $"-c \"exit {exitCode}\"");

    /// <summary>A child that stays alive until it is killed.</summary>
    private static Process StartLongLivedChild() => StartShell(
        windows: "/c ping -n 600 127.0.0.1 >nul",
        posix:   "-c \"sleep 600\"");

    private static Process StartShell(string windows, string posix)
    {
        var psi = OperatingSystem.IsWindows()
            ? new ProcessStartInfo(Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe", windows)
            : new ProcessStartInfo("/bin/sh", posix);
        psi.UseShellExecute = false;
        psi.CreateNoWindow  = true;
        return Process.Start(psi)!;
    }

    /// <summary>Completes once every <c>Exited</c> handler registered BEFORE
    /// this one has run.</summary>
    private static Task ExitObservedAfterRegistry(Process p)
    {
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        p.Exited += (_, _) => tcs.TrySetResult();
        return tcs.Task;
    }

    /// <summary>
    /// THE regression test for the registry's worst bug: it used to
    /// <c>Dispose()</c> the <see cref="Process"/> from its own <c>Exited</c>
    /// handler. That handler is subscribed inside <c>Register</c>, i.e. BEFORE
    /// the one <c>WaitForExitAsync</c> installs, so it always won — and the
    /// owner's <c>p.ExitCode</c> read then threw
    /// <c>InvalidOperationException("No process is associated with this
    /// object")</c>. This is exactly the shape of
    /// <c>PythonEnvManager.RunStreamingCore</c>, where the effect was that a
    /// SUCCESSFUL python run was reported as a separation failure.
    /// </summary>
    [Fact]
    public async Task Register_does_not_dispose_the_process_its_caller_still_owns()
    {
        using var p = StartExitingChild(7);
        ChildProcessRegistry.Register(p);

        await p.WaitForExitAsync(TestContext.Current.CancellationToken).WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

        Assert.Equal(7, p.ExitCode);
    }

    [Fact]
    public async Task Register_tracks_then_untracks_an_exiting_process()
    {
        using var p = StartLongLivedChild();
        ChildProcessRegistry.Register(p);
        Assert.True(ChildProcessRegistry.IsTracked(p.Id), "A live child must be tracked.");

        // Subscribed after Register's handler, so it observes the registry's
        // bookkeeping as already done — no polling, no sleep.
        var exitObserved = ExitObservedAfterRegistry(p);
        p.Kill(entireProcessTree: true);
        await exitObserved.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

        Assert.False(ChildProcessRegistry.IsTracked(p.Id), "An exited child must be untracked.");
    }

    [Fact]
    public void KillAll_terminates_a_long_running_child()
    {
        using var p = StartLongLivedChild();
        ChildProcessRegistry.Register(p);
        Assert.False(p.HasExited);

        ChildProcessRegistry.KillAll();
        Assert.True(p.WaitForExit(30_000), "KillAll should have terminated the child.");
        Assert.False(ChildProcessRegistry.IsTracked(p.Id), "KillAll must untrack what it killed.");
    }

    [Fact]
    public async Task Register_does_not_track_an_already_exited_process()
    {
        using var p = StartExitingChild(0);
        await p.WaitForExitAsync(TestContext.Current.CancellationToken).WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

        ChildProcessRegistry.Register(p);

        Assert.False(ChildProcessRegistry.IsTracked(p.Id), "A dead process must not enter the live set.");
    }

    /// <summary>
    /// <c>ResetGlobalGateForTests</c> used to DISPOSE the live gate. An
    /// in-flight <c>SeparateTrackAsync</c> has already captured that instance
    /// into a local and runs <c>finally { gate.Release(); }</c> against it —
    /// so the reset made a release throw <see cref="ObjectDisposedException"/>
    /// out of a finally block, failing whichever unrelated separation happened
    /// to be running (this fixture's own <c>Dispose</c> calls the reset, so
    /// the blast radius was any test running beside it).
    /// </summary>
    [Fact]
    public void Resetting_the_global_gate_leaves_an_already_captured_gate_usable()
    {
        var captured = StemSeparator.CurrentGateForTests;
        Assert.True(captured.Wait(0, TestContext.Current.CancellationToken), "The gate should start with one free slot.");

        // A separation is now 'in flight' holding `captured`.
        StemSeparator.ResetGlobalGateForTests();
        Assert.NotSame(captured, StemSeparator.CurrentGateForTests);

        // The in-flight separation's finally block. This threw before the fix.
        captured.Release();
        Assert.Equal(1, captured.CurrentCount);

        // …and the fresh gate is independent and usable.
        var fresh = StemSeparator.CurrentGateForTests;
        Assert.True(fresh.Wait(0, TestContext.Current.CancellationToken));
        fresh.Release();
    }

    [Fact]
    public void Resetting_the_global_gate_restores_the_limit_to_one()
    {
        StemSeparator.GlobalConcurrencyLimit = 4;
        Assert.Equal(4, StemSeparator.GlobalConcurrencyLimit);

        StemSeparator.ResetGlobalGateForTests();

        Assert.Equal(1, StemSeparator.GlobalConcurrencyLimit);
        Assert.Equal(1, StemSeparator.CurrentGateForTests.CurrentCount);
    }
}
