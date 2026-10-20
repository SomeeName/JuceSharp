using System.Diagnostics;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

// MUST share the collection that owns StemSeparator's process-global gate and
// ChildProcessRegistry's process-global live set. Without this these tests ran
// in PARALLEL with StemSeparatorConcurrencyTests / ChildProcessRegistryTests,
// which (a) call ResetGlobalGateForTests() — disposing the very SemaphoreSlim a
// separation in flight here is about to Release(), throwing ObjectDisposedException
// out of a finally — and (b) call ChildProcessRegistry.KillAll(), which killed
// this file's python children mid-run. That cross-fire is what "some
// StemSeparator/ChildProcess tests are environment-flaky" was describing.
[Collection("StemSeparatorConcurrency")]
public class StemSeparatorTests
{
    private readonly ITestOutputHelper _out;

    public StemSeparatorTests(ITestOutputHelper output) => _out = output;

    private static string TempRoot()
    {
        var p = Path.Combine(Path.GetTempPath(), "jucesharp-sep-" + Guid.NewGuid());
        Directory.CreateDirectory(p);
        return p;
    }

    /// <summary>A Windows path as a python string literal. Forward slashes, which
    /// the Windows CRT accepts, so no escaping question ever arises.</summary>
    private static string Quote(string path) => "\"" + path.Replace(Path.DirectorySeparatorChar, '/') + "\"";

    private static string TempFile(string ext, byte[]? bytes = null)
    {
        var p = Path.Combine(Path.GetTempPath(), "jucesharp-sep-" + Guid.NewGuid() + ext);
        File.WriteAllBytes(p, bytes ?? []);
        return p;
    }

    /// <summary>
    /// Construction resolves the bundled sidecar script and touches nothing
    /// else: no interpreter, no network, no process. (This asserted only that
    /// the constructor did not throw, which the very next test already
    /// implies.)
    /// </summary>
    [Fact]
    public void Default_construction_yields_a_usable_not_yet_ready_separator()
    {
        var root = TempRoot();
        var sep = new StemSeparator(new PythonEnvManager(rootDirectory: root));

        Assert.IsAssignableFrom<IStemSeparator>(sep);
        var probe = sep.Probe();
        Assert.False(probe.InterpreterPresent);
        Assert.Null(probe.Executable);
        Assert.Null(probe.Version);
        // A pristine root is left pristine — construction installs nothing.
        Assert.Empty(Directory.GetFiles(root));
    }

    [Fact]
    public void IsReady_is_false_for_pristine_root()
    {
        var sep = new StemSeparator(new PythonEnvManager(rootDirectory: TempRoot()));
        Assert.False(sep.IsReady());
    }

    [Fact]
    public void Probe_does_not_throw_on_pristine_root()
    {
        var sep = new StemSeparator(new PythonEnvManager(rootDirectory: TempRoot()));
        var s = sep.Probe();
        Assert.False(s.InterpreterPresent);
    }

    [Fact]
    public void Options_SkipPackageBootstrap_defaults_false()
    {
        Assert.False(StemSeparationOptions.Default.SkipPackageBootstrap);
        var o = new StemSeparationOptions { SkipPackageBootstrap = true };
        Assert.True(o.SkipPackageBootstrap);
    }

    [Fact]
    public async Task SeparateTrackAsync_throws_for_null_input()
    {
        var sep = new StemSeparator(new PythonEnvManager(rootDirectory: TempRoot()));
        await Assert.ThrowsAsync<ArgumentException>(
            () => sep.SeparateTrackAsync(null!, TempRoot(), ct: TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task SeparateTrackAsync_throws_for_empty_input()
    {
        var sep = new StemSeparator(new PythonEnvManager(rootDirectory: TempRoot()));
        await Assert.ThrowsAsync<ArgumentException>(
            () => sep.SeparateTrackAsync("   ", TempRoot(), ct: TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task SeparateTrackAsync_throws_when_input_missing()
    {
        var sep = new StemSeparator(new PythonEnvManager(rootDirectory: TempRoot()));
        var bogus = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".wav");
        await Assert.ThrowsAsync<FileNotFoundException>(
            () => sep.SeparateTrackAsync(bogus, TempRoot(), ct: TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task SeparateTrackAsync_throws_for_empty_output_dir()
    {
        var input = TempFile(".wav", [1, 2, 3]);
        var sep = new StemSeparator(new PythonEnvManager(rootDirectory: TempRoot()));
        await Assert.ThrowsAsync<ArgumentException>(
            () => sep.SeparateTrackAsync(input, "", ct: TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task SeparateTrackAsync_throws_when_script_missing()
    {
        var input = TempFile(".wav", [1, 2, 3]);
        var sep = new StemSeparator(
            new PythonEnvManager(rootDirectory: TempRoot()),
            scriptPathOverride: Path.Combine(TempRoot(), "no_such_script.py"));
        await Assert.ThrowsAsync<FileNotFoundException>(
            () => sep.SeparateTrackAsync(input, TempRoot(), ct: TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task SeparateTrackAsync_respects_precancelled_token()
    {
        var input = TempFile(".wav", [1, 2, 3]);
        var sep = new StemSeparator(new PythonEnvManager(rootDirectory: TempRoot()));
        using var cts = new CancellationTokenSource();
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => sep.SeparateTrackAsync(input, TempRoot(), ct: cts.Token));
    }

    [Fact]
    public void RequiredPackages_covers_demucs_and_roformer()
    {
        Assert.Contains(StemSeparator.RequiredPackages, pkg =>
        {
            _ = pkg;
            return pkg.StartsWith("torch", StringComparison.OrdinalIgnoreCase);
        });
        Assert.Contains(StemSeparator.RequiredPackages, pkg =>
        {
            _ = pkg;
            return pkg.StartsWith("demucs", StringComparison.OrdinalIgnoreCase);
        });
        Assert.Contains(StemSeparator.RequiredPackages, pkg => pkg.Contains("bs_roformer", StringComparison.OrdinalIgnoreCase));
        Assert.Contains(StemSeparator.RequiredPackages, pkg => pkg.StartsWith("soundfile", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void Options_defaults_are_sane()
    {
        var opt = StemSeparationOptions.Default;
        Assert.Equal("htdemucs", opt.Model);
        Assert.Equal("auto", opt.Device);
        Assert.True(opt.Shifts >= 1);
        Assert.InRange(opt.Overlap, 0.0, 1.0);
        Assert.True(opt.Timeout > TimeSpan.Zero);
    }

    [Fact]
    public void Options_with_clones_state()
    {
        var a = new StemSeparationOptions { Model = "htdemucs_6s", Device = "cuda" };
        var b = a with { Device = "cpu" };
        Assert.Equal("htdemucs_6s", a.Model);
        Assert.Equal("cuda", a.Device);
        Assert.Equal("htdemucs_6s", b.Model);
        Assert.Equal("cpu", b.Device);
    }

    [Fact]
    public void Result_GetStemPath_is_case_insensitive()
    {
        var res = new SeparationResult("in.wav", "out", "htdemucs", "cuda",
            new[] { new StemFile("Vocals", "/x/vocals.wav") });
        Assert.Equal("/x/vocals.wav", res.GetStemPath("vocals"));
        Assert.Equal("/x/vocals.wav", res.GetStemPath("VOCALS"));
        Assert.Null(res.GetStemPath("drums"));
    }

    [Fact]
    public void Bundled_separate_track_script_exists_in_output()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "PythonScripts", "separate_track.py");
        Assert.True(File.Exists(path), $"Expected bundled script at {path}");
    }

    [Fact]
    public async Task Selftest_smoke_runs_against_system_python_when_available()
    {
        // This test validates the full wire protocol (PROGRESS / DEVICE /
        // READY / STEM) end-to-end without requiring torch or any model
        // weights. Skips when no usable python interpreter is on PATH.
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        var input = TempFile(".wav", [1, 2, 3]);
        var outDir = TempRoot();
        var script = Path.Combine(AppContext.BaseDirectory, "PythonScripts", "separate_track.py");
        Assert.True(File.Exists(script));

        // Inject --selftest by hijacking the Stems list and Model name —
        // simpler: invoke the script directly via PythonEnvManager.RunStreaming
        // because the StemSeparator API doesn't expose a selftest knob.
        var lines = new List<string>();
        var (ok, _, stderr) = await PythonEnvManager.RunStreaming(
            python,
            $"\"{script}\" --selftest --input \"{input}\" --out \"{outDir}\" --model demo",
            TimeSpan.FromMinutes(1),
            l => lines.Add(l),
            TestContext.Current.CancellationToken);

        Assert.True(ok, $"selftest failed. stderr: {stderr}");
        Assert.Contains(lines, l => l.StartsWith("PROGRESS ", StringComparison.Ordinal));
        Assert.Contains(lines, l => l.StartsWith("STEM selftest", StringComparison.Ordinal));
        Assert.Contains(lines, l => l == "READY");
        Assert.True(File.Exists(Path.Combine(outDir, "selftest.wav")));
    }

    [Fact]
    public async Task SeparateTrackAsync_streams_progress_and_returns_result_with_selftest_script()
    {
        // End-to-end test of the C# StemSeparator front door using a
        // tailored standalone script that mimics the real wire protocol.
        // Skips when no python is available.
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        var scriptDir = TempRoot();
        var script = Path.Combine(scriptDir, "fake_separator.py");
        await File.WriteAllTextAsync(script, """
            import argparse, os, sys, time
            ap = argparse.ArgumentParser()
            ap.add_argument("--input", required=True)
            ap.add_argument("--out", required=True)
            ap.add_argument("--model", required=True)
            ap.add_argument("--device", default="auto")
            ap.add_argument("--shifts", type=int, default=1)
            ap.add_argument("--overlap", type=float, default=0.25)
            ap.add_argument("--model-path", dest="model_path", default=None)
            ap.add_argument("--config", default=None)
            ap.add_argument("--stems", default=None)
            a = ap.parse_args()
            os.makedirs(a.out, exist_ok=True)
            print("DEVICE cpu", flush=True)
            for i, f in enumerate([0.1, 0.5, 1.0]):
                print(f"PROGRESS {f:.4f} step{i}", flush=True)
            print("READY", flush=True)
            for s in ["vocals", "other"]:
                p = os.path.join(a.out, f"{s}.wav")
                with open(p, "wb") as fh: fh.write(b"\x00" * 4)
                print(f"STEM {s} {p}", flush=True)
            sys.exit(0)
            """, TestContext.Current.CancellationToken);

        var input = TempFile(".wav", [1, 2, 3]);
        var outDir = TempRoot();
        var env = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };
        var sep = new StemSeparator(env, scriptPathOverride: script);

        var phases = new List<string>();
        var fractions = new List<double>();
        var prog = new SyncProgress<StemSeparationProgress>(p =>
        {
            phases.Add(p.Phase);
            if (p.Fraction >= 0) fractions.Add(p.Fraction);
        });

        var result = await sep.SeparateTrackAsync(input, outDir,
            options: new StemSeparationOptions { SkipPackageBootstrap = true },
            progress: prog, ct: TestContext.Current.CancellationToken);

        Assert.NotNull(result);
        Assert.Equal(input, result.InputPath);
        Assert.Equal(outDir, result.OutputDirectory);
        Assert.Equal("cpu", result.Device);
        Assert.Equal(2, result.Stems.Count);
        Assert.NotNull(result.GetStemPath("vocals"));
        Assert.NotNull(result.GetStemPath("other"));
        Assert.Contains("done", phases);
        Assert.Contains("inference", phases);
        Assert.Contains("stem", phases);
        // Final fraction should be 1.0 (the "done" event).
        Assert.Equal(1.0, fractions[^1], 3);
        // Fractions must be monotonically non-decreasing.
        for (int i = 1; i < fractions.Count; i++)
            Assert.True(fractions[i] >= fractions[i - 1] - 0.001,
                $"Progress regressed at index {i}: {fractions[i - 1]} -> {fractions[i]}");
    }

    [Fact]
    public async Task SeparateTrackAsync_throws_on_nonzero_exit_with_stderr_in_message()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        var scriptDir = TempRoot();
        var script = Path.Combine(scriptDir, "fail.py");
        await File.WriteAllTextAsync(script, """
            import sys
            sys.stderr.write("simulated demucs OOM\n")
            sys.exit(7)
            """, TestContext.Current.CancellationToken);

        var input = TempFile(".wav", [1, 2, 3]);
        var env = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };
        var sep = new StemSeparator(env, scriptPathOverride: script);
        var ex = await Assert.ThrowsAsync<StemSeparationException>(
            () => sep.SeparateTrackAsync(input, TempRoot(),
                options: new StemSeparationOptions { SkipPackageBootstrap = true }, ct: TestContext.Current.CancellationToken));
        Assert.Contains("simulated demucs OOM", ex.Message);
        Assert.Equal(StemSeparationErrorKind.Unknown, ex.Kind);
    }

    [Fact]
    public async Task SeparateTrackAsync_cancellation_kills_long_running_child()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        var scriptDir = TempRoot();
        var script = Path.Combine(scriptDir, "spin.py");

        // The two markers ARE the assertion below. "started" proves the child
        // really ran, so the absence of "completed" means something; "completed"
        // is written only if the 60 s spin reached its end, i.e. only if the
        // child was WAITED OUT instead of killed. Neither can move under load.
        var startedMarker = Path.Combine(scriptDir, "started.marker");
        var completedMarker = Path.Combine(scriptDir, "completed.marker");

        await File.WriteAllTextAsync(script, """
            import argparse, pathlib, time
            ap = argparse.ArgumentParser()
            ap.add_argument("--input"); ap.add_argument("--out")
            ap.add_argument("--model"); ap.add_argument("--device")
            ap.add_argument("--shifts", type=int); ap.add_argument("--overlap", type=float)
            ap.add_argument("--model-path", dest="model_path", default=None)
            ap.add_argument("--config", default=None)
            ap.add_argument("--stems", default=None)
            ap.parse_args()
            pathlib.Path(STARTED).write_text("1")
            for _ in range(60): time.sleep(1)
            pathlib.Path(COMPLETED).write_text("1")
            """.Replace("STARTED", Quote(startedMarker))
               .Replace("COMPLETED", Quote(completedMarker)), TestContext.Current.CancellationToken);

        var input = TempFile(".wav", [1, 2, 3]);
        var env = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };
        var sep = new StemSeparator(env, scriptPathOverride: script);
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(2));
        var sw = Stopwatch.StartNew();
        // A CANCEL IS NOT A FAILURE. RunStreaming kills the child and returns
        // ok=false with stderr "Cancelled.", so SeparateTrackAsync
        // ThrowIfCancellationRequested()s before its `if (!ok)` — otherwise the
        // user pressing Cancel is logged as "Stem separation failed" and reads
        // to every consumer as a reason to fall through to the next backend.
        // This test asserted the pre-fix StemSeparationException for releases
        // after the fix landed: it Assert.Skips on a box with no python and
        // fails deterministically on one with it, which is why it read as
        // environment-flaky rather than as stale.
        var cancelled = await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => sep.SeparateTrackAsync(input, TempRoot(),
                options: new StemSeparationOptions { SkipPackageBootstrap = true },
                ct: cts.Token));
        sw.Stop();

        // …and it is OUR token, i.e. it came from the ct.ThrowIfCancellationRequested()
        // that guards the `if (!ok)`, not from some unrelated cancellation on the way.
        Assert.Equal(cts.Token, cancelled.CancellationToken);

        // The name's other half: the child was KILLED, not waited out — the spin
        // script sleeps 60 s. This was `sw.Elapsed < TimeSpan.FromSeconds(30)`,
        // a wall-clock budget that says nothing about the kill and fails on a
        // busy box; the comment above it already recorded that
        // ChildProcessRegistry.LiveCount was rejected for being process-global.
        // The markers answer the same question structurally.
        Assert.True(File.Exists(startedMarker),
            "the child never started, so 'it did not finish' proves nothing about a kill.");
        Assert.False(File.Exists(completedMarker),
            "the spin script ran all 60 s to completion: the child was WAITED OUT, not killed.");

        _out.WriteLine($"cancellation returned after {sw.Elapsed} (child spins for 60 s).");
    }

    private static string? ResolveSystemPython()
    {
        var path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrEmpty(path)) return null;
        foreach (var dir in path.Split(Path.PathSeparator))
        {
            foreach (var name in new[] { "python3", "python" })
            {
                var ext = OperatingSystem.IsWindows() ? ".exe" : string.Empty;
                var full = Path.Combine(dir, name + ext);
                if (File.Exists(full)) return full;
            }
        }
        return null;
    }
}

// Same reason as StemSeparatorTests above: RunStreaming registers its children
// with the process-global ChildProcessRegistry, so KillAll() from another
// collection would terminate them mid-test.
[Collection("StemSeparatorConcurrency")]
public class PythonEnvManagerStreamingTests
{
    private readonly ITestOutputHelper _out;

    public PythonEnvManagerStreamingTests(ITestOutputHelper output) => _out = output;

    private static string TempRoot()
    {
        var p = Path.Combine(Path.GetTempPath(), "jucesharp-py-" + Guid.NewGuid());
        Directory.CreateDirectory(p);
        return p;
    }

    /// <summary>A path as a python string literal: forward slashes, which the
    /// Windows CRT accepts, so no escaping question ever arises.</summary>
    private static string Quote(string path) => "\"" + path.Replace(Path.DirectorySeparatorChar, '/') + "\"";

    [Fact]
    public async Task RunStreaming_returns_failure_for_missing_executable()
    {
        var (ok, stdout, stderr) = await PythonEnvManager.RunStreaming(
            Path.Combine(Path.GetTempPath(), "no-such-binary-xyz.exe"),
            "--version", TimeSpan.FromSeconds(2), null, TestContext.Current.CancellationToken);
        Assert.False(ok);
        Assert.Empty(stdout);
        Assert.NotEmpty(stderr);
    }

    private static readonly string[] Expected = new[] { "a", "b", "c" };

    [Fact]
    public async Task RunStreaming_invokes_onLine_per_line()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        var lines = new List<string>();
        var (ok, _, _) = await PythonEnvManager.RunStreaming(
            python,
            "-c \"print('a'); print('b'); print('c')\"",
            TimeSpan.FromSeconds(10),
            l => lines.Add(l),
            TestContext.Current.CancellationToken);
        Assert.True(ok);
        Assert.Equal(Expected, lines);
    }

    [Fact]
    public async Task RunStreaming_handler_exception_does_not_kill_process()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        var (ok, stdout, _) = await PythonEnvManager.RunStreaming(
            python,
            "-c \"print('first'); print('second')\"",
            TimeSpan.FromSeconds(10),
            _ => throw new InvalidOperationException("handler explosion"),
            TestContext.Current.CancellationToken);
        Assert.True(ok);
        // stdout should still contain both lines.
        Assert.Contains("first", stdout);
        Assert.Contains("second", stdout);
    }

    /// <summary>
    /// The timeout fires AND the child dies. This ended in
    /// <c>Assert.True(sw.Elapsed &lt; TimeSpan.FromSeconds(15), …)</c>, which is
    /// a budget: it says nothing about whether the child was killed and fails on
    /// a loaded box for a reason that has nothing to do with the code. The
    /// structural discriminator is a marker the script writes only if its 60 s
    /// sleep RETURNED — present means waited out, absent means killed — paired
    /// with a start marker so "absent" cannot be satisfied by a child that never
    /// ran.
    /// </summary>
    [Fact]
    public async Task RunStreaming_times_out_and_kills_child()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        var dir = TempRoot();
        var script = Path.Combine(dir, "sleep.py");
        var startedMarker = Path.Combine(dir, "started.marker");
        var completedMarker = Path.Combine(dir, "completed.marker");
        await File.WriteAllTextAsync(script, """
            import pathlib, time
            pathlib.Path(STARTED).write_text("1")
            time.sleep(60)
            pathlib.Path(COMPLETED).write_text("1")
            """.Replace("STARTED", Quote(startedMarker))
               .Replace("COMPLETED", Quote(completedMarker)), TestContext.Current.CancellationToken);

        var sw = Stopwatch.StartNew();
        var (ok, _, stderr) = await PythonEnvManager.RunStreaming(
            python,
            Quote(script),
            TimeSpan.FromSeconds(2),
            null,
            TestContext.Current.CancellationToken);
        sw.Stop();

        Assert.False(ok);
        Assert.Contains("Timed out", stderr);
        Assert.True(File.Exists(startedMarker),
            "the child never started, so 'it did not finish' proves nothing about a kill.");
        Assert.False(File.Exists(completedMarker),
            "the script slept its full 60 s: the child was WAITED OUT, not killed.");

        _out.WriteLine($"RunStreaming timeout returned after {sw.Elapsed} (child sleeps for 60 s).");
    }

    /// <summary>
    /// A cancel THROWS. It used to come back as
    /// <c>(ok: false, stderr: "Cancelled.")</c>, and only one of the four call
    /// sites special-cased that string — <c>EnsurePipAsync</c>,
    /// <c>EnsurePackagesAsync</c> and <c>CkptToOnnxConverter.ConvertAsync</c>
    /// all turned it into <c>InvalidOperationException("… failed … stderr:
    /// Cancelled.")</c>, so pressing Cancel during the first-run torch install
    /// was reported to the user as a pip failure. The timeout case above keeps
    /// the ok:false shape deliberately: there the token is fine and it is the
    /// command that did not finish.
    /// </summary>
    [Fact]
    public async Task RunStreaming_external_cancellation_propagates()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(1));
        var ex = await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await PythonEnvManager.RunStreaming(
                python!,
                "-c \"import time; time.sleep(60)\"",
                TimeSpan.FromMinutes(5),
                null,
                cts.Token));
        Assert.Equal(cts.Token, ex.CancellationToken);
    }

    [Fact]
    public async Task EnsureInterpreterAsync_short_circuits_when_executable_preset()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        var mgr = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };
        // Should NOT attempt download even though the embed root is empty.
        await mgr.EnsureInterpreterAsync(progress: null, ct: TestContext.Current.CancellationToken);
        Assert.Equal(python, mgr.Executable);
    }

    [Fact]
    public async Task EnsureInterpreterAsync_pre_cancelled_token_throws()
    {
        var mgr = new PythonEnvManager(rootDirectory: TempRoot());
        using var cts = new CancellationTokenSource();
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => mgr.EnsureInterpreterAsync(ct: cts.Token));
    }

    [Fact]
    public void Probe_reports_executable_when_preset()
    {
        var python = ResolveSystemPython();
        if (python is null) Assert.Skip("No python interpreter on PATH — this test exercises the real child-process pipeline.");

        // Pre-set Executable so Probe surfaces it without bootstrap.
        var mgr = new PythonEnvManager(rootDirectory: TempRoot()) { Executable = python };
        // Probe currently relies on ResolveLocalExecutable which checks
        // Root\python.exe on Windows. If the embed root is empty, Probe
        // returns "not present" — confirm that documented behaviour.
        var s = mgr.Probe();
        // On non-Windows, ResolveLocalExecutable considers a preset
        // Executable; on Windows it does not. Either outcome is valid;
        // we only assert the call doesn't throw and is observable.
        _ = s.Executable;
    }

    private static string? ResolveSystemPython()
    {
        var path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrEmpty(path)) return null;
        foreach (var dir in path.Split(Path.PathSeparator))
        {
            foreach (var name in new[] { "python3", "python" })
            {
                var ext = OperatingSystem.IsWindows() ? ".exe" : string.Empty;
                var full = Path.Combine(dir, name + ext);
                if (File.Exists(full)) return full;
            }
        }
        return null;
    }
}

/// <summary>Synchronous IProgress used by ordering-sensitive tests.</summary>
internal sealed class SyncProgress<T>(Action<T> h) : IProgress<T>
{
    private readonly Action<T> _h = h;

    public void Report(T value) => _h(value);
}
