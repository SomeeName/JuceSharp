using JuceSharp.Diagnostics;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

public sealed class DiagnosticsCliRunnerTests
{
    [Fact]
    public async Task Runner_emits_summary_on_success()
    {
        var input = Path.GetTempFileName();
        File.WriteAllBytes(input, [0, 0, 0, 0]);
        try
        {
            var outDir = Path.Combine(Path.GetTempPath(), "diag-out-" + Guid.NewGuid());
            var opts = new DiagnosticsOptions(
                InputPath: input, OutputDirectory: outDir,
                Model: "htdemucs", Device: "cpu", Shifts: 1, Overlap: 0.25,
                MaxConcurrent: 1, ModelPath: null, ConfigPath: null,
                SkipBootstrap: true, Quiet: false);
            var stdout = new StringWriter();
            var stderr = new StringWriter();

            static Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct)
            {
                p?.Report(new StemSeparationProgress("inference", 0.5, "halfway", null));
                return Task.FromResult(new SeparationResult(i, o, opt.Model, opt.Device,
                    new[] { new StemFile("vocals", Path.Combine(o, "vocals.wav")) }));
            }
            var code = await Runner.RunAsync(opts, stdout, stderr, Fake, TestContext.Current.CancellationToken);

            Assert.Equal(0, code);
            var s = stdout.ToString();
            Assert.Contains("Run Summary", s);
            Assert.Contains("Status         : OK", s);
            Assert.Contains("Stems written  : 1", s);
            Assert.Contains("Device used    : cpu", s);
            Assert.Contains("Total elapsed", s);
        }
        finally { try { File.Delete(input); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Runner_returns_exit_code_10_on_vram_failure_and_summary_shows_failure()
    {
        var input = Path.GetTempFileName();
        File.WriteAllBytes(input, [0, 0, 0, 0]);
        try
        {
            var opts = new DiagnosticsOptions(
                InputPath: input,
                OutputDirectory: Path.Combine(Path.GetTempPath(), "diag-out-" + Guid.NewGuid()),
                Model: "htdemucs", Device: "cuda", Shifts: 1, Overlap: 0.25,
                MaxConcurrent: 1, ModelPath: null, ConfigPath: null,
                SkipBootstrap: true, Quiet: true);
            var stdout = new StringWriter();
            var stderr = new StringWriter();

            static Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct) =>
                throw new StemSeparationVramException(opt.Model, opt.Device, "CUDA out of memory");
            var code = await Runner.RunAsync(opts, stdout, stderr, Fake, TestContext.Current.CancellationToken);

            Assert.Equal(10, code);
            Assert.Contains("VRAM EXHAUSTED", stderr.ToString());
            Assert.Contains("VRAM exhausted : yes", stdout.ToString());
            Assert.Contains("Status         : FAILED", stdout.ToString());
        }
        finally { try { File.Delete(input); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Runner_returns_exit_code_11_on_other_separation_failure()
    {
        var input = Path.GetTempFileName();
        try
        {
            var opts = new DiagnosticsOptions(
                InputPath: input,
                OutputDirectory: Path.Combine(Path.GetTempPath(), "diag-" + Guid.NewGuid()),
                Model: "htdemucs", Device: "cpu", Shifts: 1, Overlap: 0.25,
                MaxConcurrent: 1, ModelPath: null, ConfigPath: null,
                SkipBootstrap: true, Quiet: true);
            var stdout = new StringWriter();
            var stderr = new StringWriter();
            static Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct) =>
                throw new StemSeparationModelLoadException(opt.Model, "checkpoint missing");
            var code = await Runner.RunAsync(opts, stdout, stderr, Fake, TestContext.Current.CancellationToken);
            Assert.Equal(11, code);
            Assert.Contains("SEPARATION FAILED", stderr.ToString());
        }
        finally { try { File.Delete(input); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Runner_returns_exit_code_130_on_cancellation()
    {
        var input = Path.GetTempFileName();
        try
        {
            var opts = new DiagnosticsOptions(
                InputPath: input,
                OutputDirectory: Path.Combine(Path.GetTempPath(), "diag-" + Guid.NewGuid()),
                Model: "htdemucs", Device: "cpu", Shifts: 1, Overlap: 0.25,
                MaxConcurrent: 1, ModelPath: null, ConfigPath: null,
                SkipBootstrap: true, Quiet: true);
            var stdout = new StringWriter();
            var stderr = new StringWriter();
            using var cts = new CancellationTokenSource();
            cts.Cancel();
            static Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct)
            {
                ct.ThrowIfCancellationRequested();
                return Task.FromResult(new SeparationResult(i, o, opt.Model, opt.Device, Array.Empty<StemFile>()));
            }
            var code = await Runner.RunAsync(opts, stdout, stderr, Fake, cts.Token);
            Assert.Equal(130, code);
        }
        finally { try { File.Delete(input); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    // ------------------------------------------------------------------
    // Failure modes that used to escape the runner entirely.
    // ------------------------------------------------------------------

    private static DiagnosticsOptions OptsFor(string input, string? outDir = null) => new(
        InputPath: input,
        OutputDirectory: outDir ?? Path.Combine(Path.GetTempPath(), "diag-" + Guid.NewGuid()),
        Model: "htdemucs", Device: "cpu", Shifts: 1, Overlap: 0.25,
        MaxConcurrent: 1, ModelPath: null, ConfigPath: null,
        SkipBootstrap: true, Quiet: true);

    /// <summary>
    /// The embedded-Python bootstrap throws <see cref="InvalidOperationException"/>
    /// ("pip install failed…", "get-pip.py failed…"), <see cref="HttpRequestException"/>
    /// and <see cref="InvalidDataException"/> (checksum mismatch). None derives
    /// from <see cref="StemSeparationException"/>, so all three used to leave
    /// the process with an unhandled exception (0xE0434352) instead of an
    /// exit code a script can read.
    /// </summary>
    [Theory]
    [InlineData(typeof(InvalidOperationException))]
    [InlineData(typeof(HttpRequestException))]
    [InlineData(typeof(InvalidDataException))]
    [InlineData(typeof(IOException))]
    public async Task Runner_returns_exit_code_12_on_a_bootstrap_failure(Type exceptionType)
    {
        var input = Path.GetTempFileName();
        try
        {
            var stdout = new StringWriter();
            var stderr = new StringWriter();
            Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct) =>
                throw (Exception)Activator.CreateInstance(exceptionType, "boom")!;

            var code = await Runner.RunAsync(OptsFor(input), stdout, stderr, Fake, TestContext.Current.CancellationToken);

            Assert.Equal(ExitCodes.EnvironmentFailed, code);
            Assert.Contains("ENVIRONMENT FAILED", stderr.ToString(), StringComparison.Ordinal);
            Assert.Contains("Status         : FAILED", stdout.ToString(), StringComparison.Ordinal);
        }
        finally { try { File.Delete(input); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    /// <summary>An output path that is an existing FILE cannot be created as a
    /// directory; <c>Directory.CreateDirectory</c> throws <see cref="IOException"/>
    /// straight out of the runner's first statements, before any try block
    /// existed.</summary>
    [Fact]
    public async Task Runner_returns_exit_code_4_when_the_output_directory_cannot_be_created()
    {
        var input = Path.GetTempFileName();
        var blocker = Path.GetTempFileName();      // a FILE where a DIR is wanted
        try
        {
            var stdout = new StringWriter();
            var stderr = new StringWriter();
            static Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct) =>
                throw new InvalidOperationException("the runner must never get this far");

            var code = await Runner.RunAsync(OptsFor(input, blocker), stdout, stderr, Fake, TestContext.Current.CancellationToken);

            Assert.Equal(ExitCodes.OutputUnavailable, code);
            Assert.Contains("cannot create output directory", stderr.ToString(), StringComparison.Ordinal);
        }
        finally
        {
            foreach (var f in new[] { input, blocker })
                try { File.Delete(f); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }

    /// <summary>
    /// A cancellation must be reported as a cancellation even though
    /// <see cref="OperationCanceledException"/> and the separation exceptions
    /// are caught in the same method — the arm order is load-bearing.
    /// </summary>
    [Fact]
    public async Task A_cancel_is_not_reported_as_a_separation_failure()
    {
        var input = Path.GetTempFileName();
        try
        {
            var stdout = new StringWriter();
            var stderr = new StringWriter();
            static Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct) =>
                throw new OperationCanceledException();

            var code = await Runner.RunAsync(OptsFor(input), stdout, stderr, Fake, TestContext.Current.CancellationToken);

            Assert.Equal(ExitCodes.Cancelled, code);
            Assert.Contains("CANCELLED.", stderr.ToString(), StringComparison.Ordinal);
            Assert.DoesNotContain("SEPARATION FAILED", stderr.ToString(), StringComparison.Ordinal);
            Assert.DoesNotContain("Run Summary", stdout.ToString(), StringComparison.Ordinal);
        }
        finally { try { File.Delete(input); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    /// <summary>
    /// <see cref="Progress{T}"/> marshals to the captured context, so on a
    /// console app the progress lines were posted to the thread pool and
    /// routinely landed AFTER the run summary. Reporting is now inline, so
    /// stdout is ordered.
    /// </summary>
    [Fact]
    public async Task Progress_lines_are_written_before_the_summary()
    {
        var input = Path.GetTempFileName();
        File.WriteAllBytes(input, [0, 0, 0, 0]);
        try
        {
            var opts = OptsFor(input) with { Quiet = false };
            var stdout = new StringWriter();
            var stderr = new StringWriter();

            static Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct)
            {
                for (var k = 0; k < 20; k++)
                    p?.Report(new StemSeparationProgress("inference", k / 20.0, $"tick {k}", null));
                return Task.FromResult(new SeparationResult(i, o, opt.Model, opt.Device, Array.Empty<StemFile>()));
            }

            var code = await Runner.RunAsync(opts, stdout, stderr, Fake, TestContext.Current.CancellationToken);
            Assert.Equal(ExitCodes.Ok, code);

            var text = stdout.ToString();
            var summaryAt = text.IndexOf("Run Summary", StringComparison.Ordinal);
            var lastTickAt = text.LastIndexOf("tick ", StringComparison.Ordinal);
            Assert.True(summaryAt >= 0, "the summary must be printed");
            Assert.True(lastTickAt >= 0, "progress lines must be printed");
            Assert.True(lastTickAt < summaryAt,
                "every progress line must precede the summary — Progress<T> used to post them after it");
        }
        finally { try { File.Delete(input); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Quiet_suppresses_progress_but_keeps_the_summary()
    {
        var input = Path.GetTempFileName();
        try
        {
            var stdout = new StringWriter();
            var stderr = new StringWriter();
            static Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct)
            {
                p?.Report(new StemSeparationProgress("inference", 0.5, "tick", null));
                return Task.FromResult(new SeparationResult(i, o, opt.Model, opt.Device, Array.Empty<StemFile>()));
            }

            await Runner.RunAsync(OptsFor(input), stdout, stderr, Fake, TestContext.Current.CancellationToken);

            Assert.DoesNotContain("tick", stdout.ToString(), StringComparison.Ordinal);
            Assert.Contains("Run Summary", stdout.ToString(), StringComparison.Ordinal);
            Assert.Contains("VRAM exhausted : no", stdout.ToString(), StringComparison.Ordinal);
        }
        finally { try { File.Delete(input); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    /// <summary>The runner must forward the CLI's timeout and stem subset into
    /// the backend options, or the flags are decoration.</summary>
    [Fact]
    public async Task Timeout_and_stem_subset_reach_the_backend()
    {
        var input = Path.GetTempFileName();
        try
        {
            var opts = OptsFor(input) with
            {
                Timeout = TimeSpan.FromMinutes(7),
                Stems = new[] { "vocals", "other" },
            };
            StemSeparationOptions? seen = null;
            Task<SeparationResult> Fake(string i, string o, StemSeparationOptions opt, IProgress<StemSeparationProgress>? p, CancellationToken ct)
            {
                seen = opt;
                return Task.FromResult(new SeparationResult(i, o, opt.Model, opt.Device, Array.Empty<StemFile>()));
            }

            await Runner.RunAsync(opts, new StringWriter(), new StringWriter(), Fake, TestContext.Current.CancellationToken);

            Assert.NotNull(seen);
            Assert.Equal(TimeSpan.FromMinutes(7), seen!.Timeout);
            Assert.Equal(new[] { "vocals", "other" }, seen.Stems);
        }
        finally { try { File.Delete(input); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public void WriteProbe_reports_every_field_and_never_fails()
    {
        var w = new StringWriter();
        var code = Runner.WriteProbe(w, new PythonEnvStatus(true, true, false, @"C:\x\python.exe", "Python 3.11.9"));

        Assert.Equal(ExitCodes.Ok, code);
        var s = w.ToString();
        Assert.Contains("Python 3.11.9", s, StringComparison.Ordinal);
        Assert.Contains(@"C:\x\python.exe", s, StringComparison.Ordinal);
        Assert.Contains("torch          : missing", s, StringComparison.Ordinal);
        Assert.Contains("Ready          : no", s, StringComparison.Ordinal);

        var ready = new StringWriter();
        Runner.WriteProbe(ready, new PythonEnvStatus(true, true, true, "py", "3.11.9"));
        Assert.Contains("Ready          : yes", ready.ToString(), StringComparison.Ordinal);

        var missing = new StringWriter();
        Runner.WriteProbe(missing, new PythonEnvStatus(false, false, false, null, null));
        Assert.Contains("Interpreter    : MISSING", missing.ToString(), StringComparison.Ordinal);
        Assert.Contains("(none)", missing.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public void WriteVersion_names_every_component()
    {
        var w = new StringWriter();
        Runner.WriteVersion(w);
        var s = w.ToString();

        Assert.Contains("juce-stems-diag", s, StringComparison.Ordinal);
        Assert.Contains("JuceSharp.StemSeparation", s, StringComparison.Ordinal);
        Assert.Contains("JuceSharp.Native ABI", s, StringComparison.Ordinal);
        Assert.Contains("Embedded Python", s, StringComparison.Ordinal);
        Assert.Contains(PythonEnvManager.DefaultPythonVersion, s, StringComparison.Ordinal);
        // The ABI must be a real number, not the "(unknown)" fallback — the
        // reflection target is an internal const that a rename would break.
        Assert.Matches(@"JuceSharp\.Native ABI\s+v\d+", s);
    }
}
