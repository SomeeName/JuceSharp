using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;

namespace JuceSharp.Diagnostics;

/// <summary>
/// Tiny headless front-end for the JuceSharp stem separator. Streams
/// progress lines directly to stdout and prints a final perf summary.
///
/// <para>Usage:</para>
/// <code>
///   juce-stems-diag --input song.wav [--out OUTDIR] [--model htdemucs]
///                   [--device auto|cuda|cpu] [--shifts 1] [--overlap 0.25]
///                   [--stems vocals,drums] [--timeout 240]
///                   [--max-concurrent 1] [--skip-bootstrap] [--quiet]
///   juce-stems-diag --probe
///   juce-stems-diag --version
/// </code>
/// <para>Exit codes: see <see cref="ExitCodes"/>.</para>
/// </summary>
public static class Program
{
    /// <summary>
    /// CLI entry point: parse the arguments, dispatch to <see cref="Runner"/>, and return
    /// one of <see cref="ExitCodes"/>. With no arguments it prints usage. Wires Ctrl+C to
    /// a cancellation token, so an interrupted run exits <see cref="ExitCodes.Cancelled"/>
    /// rather than being killed mid-write.
    /// </summary>
    /// <param name="args">Raw command line.</param>
    /// <returns>The process exit code.</returns>
    public static async Task<int> Main(string[] args)
    {
        if (args.Length == 0)
        {
            PrintUsage(Console.Out);
            return ExitCodes.Ok;
        }

        DiagnosticsOptions opts;
        try { opts = DiagnosticsOptions.Parse(args); }
        catch (ArgumentException ex)
        {
            Console.Error.WriteLine($"argument error: {ex.Message}");
            PrintUsage(Console.Error);
            return ExitCodes.ArgumentError;
        }

        if (opts.HelpRequested)
        {
            PrintUsage(Console.Out);
            return ExitCodes.Ok;
        }
        if (opts.VersionRequested)
        {
            Runner.WriteVersion(Console.Out);
            return ExitCodes.Ok;
        }

        // Ctrl+C must unwind the runner (killing the python child) rather
        // than tear the process down mid-write; without this the documented
        // exit code 130 was unreachable and a cancelled run left an orphan.
        using var cts = new CancellationTokenSource();
        var presses = 0;
        ConsoleCancelEventHandler onCancel = (_, e) => e.Cancel = HandleCancel(ref presses, cts, Console.Error);
        Console.CancelKeyPress += onCancel;
        try
        {
            if (opts.ProbeOnly)
                return await RunProbeAsync(static () => new StemSeparator(),
                                           Console.Out, Console.Error, cts.Token).ConfigureAwait(false);

            return await Runner.RunAsync(opts, Console.Out, Console.Error, ct: cts.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("CANCELLED.");
            return ExitCodes.Cancelled;
        }
        catch (Exception ex) when (ex is InvalidOperationException or IOException
                                      or UnauthorizedAccessException or InvalidDataException
                                      or HttpRequestException)
        {
            // --probe was the ONE arm with no exception handling, and ExitCodes
            // .EnvironmentFailed's own doc described that in the PAST tense: `new
            // StemSeparator()` constructs a PythonEnvManager whose ctor does
            // Directory.CreateDirectory(Root) — UnauthorizedAccessException or IOException
            // on a locked-down or full profile volume — and ProbeAsync then spawns a
            // child. Runner.RunAsync has this catch internally; the probe path does not go
            // through it, so a probe on such a machine exited 0xE0434352 (unhandled
            // exception) rather than 12. Hoisted here so EVERY arm shares it.
            Console.Error.WriteLine($"ENVIRONMENT FAILED ({ex.GetType().Name}): {ex.Message}");
            return ExitCodes.EnvironmentFailed;
        }
        finally
        {
            Console.CancelKeyPress -= onCancel;
        }
    }

    /// <summary>
    /// The <c>--probe</c> arm, with the environment-failure classification the rest of the
    /// CLI already had. Takes a FACTORY rather than a constructed separator because the
    /// throw this exists to catch happens in the CONSTRUCTOR: <c>PythonEnvManager</c>'s
    /// ctor does <c>Directory.CreateDirectory(Root)</c>.
    /// </summary>
    /// <param name="factory">Builds the separator to probe; may throw.</param>
    /// <param name="stdout">Where the probe report goes.</param>
    /// <param name="stderr">Where a failure line goes.</param>
    /// <param name="ct">Cancellation for the probe's own child process.</param>
    /// <returns>An <see cref="ExitCodes"/> value; never throws for an environment fault.</returns>
    internal static async Task<int> RunProbeAsync(Func<StemSeparator> factory,
                                                  TextWriter stdout, TextWriter stderr,
                                                  CancellationToken ct)
    {
        try
        {
            var separator = factory();
            var status = await separator.ProbeAsync(ct).ConfigureAwait(false);
            return Runner.WriteProbe(stdout, status);
        }
        catch (OperationCanceledException)
        {
            stderr.WriteLine("CANCELLED.");
            return ExitCodes.Cancelled;
        }
        catch (Exception ex) when (ex is InvalidOperationException or IOException
                                      or UnauthorizedAccessException or InvalidDataException
                                      or HttpRequestException)
        {
            stderr.WriteLine($"ENVIRONMENT FAILED ({ex.GetType().Name}): {ex.Message}");
            return ExitCodes.EnvironmentFailed;
        }
    }

    /// <summary>
    /// The Ctrl+C handler body, extracted so it can be driven by a test — the console
    /// handler itself cannot be.
    /// <para><b>The second press must actually force.</b> <c>e.Cancel = true</c> was set
    /// unconditionally, so press two was swallowed exactly like press one: it reprinted the
    /// same line and re-cancelled an already-cancelled token. A user watching a wedged
    /// four-hour torch install had been told, by this very message, that an escape hatch
    /// existed; it did not, and Task Manager was the only way out. Press two now leaves
    /// <c>e.Cancel</c> false, so the default handler tears the process down — and
    /// <c>ChildProcessRegistry</c>'s own <c>Console.CancelKeyPress</c> hook has already
    /// killed the child tree by then.</para>
    /// </summary>
    /// <param name="presses">Running count of presses seen; incremented here.</param>
    /// <param name="cts">Token source to cancel on the first press.</param>
    /// <param name="stderr">Where the advisory line goes.</param>
    /// <returns>What to assign to <c>ConsoleCancelEventArgs.Cancel</c>: <c>true</c> to
    /// suppress the default handler (press one), <c>false</c> to let it kill the process
    /// (press two and after). <c>ConsoleCancelEventArgs</c> has no public constructor, which
    /// is why the flag is RETURNED rather than set here — a test can call this, it cannot
    /// build that event.</returns>
    internal static bool HandleCancel(ref int presses, CancellationTokenSource cts, TextWriter stderr)
    {
        var n = ++presses;
        if (n >= 2)
        {
            stderr.WriteLine("forcing.");
            return false;   // let the runtime kill us
        }

        stderr.WriteLine("cancelling… (press Ctrl+C again to force)");
        try { cts.Cancel(); } catch (ObjectDisposedException) { /* already shutting down */ }
        return true;
    }

    internal static void PrintUsage(TextWriter w)
    {
        w.WriteLine("juce-stems-diag — JuceSharp.StemSeparation diagnostic runner");
        w.WriteLine();
        w.WriteLine("Usage:");
        w.WriteLine("  juce-stems-diag --input <path> [options]");
        w.WriteLine("  juce-stems-diag --probe");
        w.WriteLine("  juce-stems-diag --version");
        w.WriteLine();
        w.WriteLine("Options:");
        w.WriteLine("  --input <path>            Input audio file (required unless --probe/--version).");
        w.WriteLine("  --out <dir>               Output directory (default: ./stems-out).");
        w.WriteLine("  --model <name>            Model id (htdemucs, htdemucs_6s, bs_roformer, …).");
        w.WriteLine("  --device <auto|cuda|cpu>  Compute device (default: auto).");
        w.WriteLine("  --shifts <int>            Demucs shift-augment passes (default: 1).");
        w.WriteLine("  --overlap <float>         Chunk overlap 0..1 (default: 0.25).");
        w.WriteLine("  --stems <a,b,…>           Only write these stems (default: all the model produces).");
        w.WriteLine("  --timeout <minutes>       Hard cap on the child process (default: 240).");
        w.WriteLine("  --max-concurrent <int>    Process-wide concurrency cap (default: 1).");
        w.WriteLine("  --model-path <path>       Explicit checkpoint/ONNX path (RoFormer).");
        w.WriteLine("  --config <path>           Optional yaml config (RoFormer).");
        w.WriteLine("  --skip-bootstrap          Skip pip install (assumes env is provisioned).");
        w.WriteLine("  --probe                   Report embedded-Python status and exit (no separation).");
        w.WriteLine("  --quiet                   Suppress per-line progress, show summary only.");
        w.WriteLine("  --version                 Print component versions and exit.");
        w.WriteLine("  --help                    Show this help and exit.");
        w.WriteLine();
        w.WriteLine("Exit codes:");
        w.WriteLine($"  {ExitCodes.Ok,-3} success            {ExitCodes.ArgumentError,-3} bad arguments");
        w.WriteLine($"  {ExitCodes.InputNotFound,-3} input not found    {ExitCodes.OutputUnavailable,-3} output dir unusable");
        w.WriteLine($"  {ExitCodes.VramExhausted,-3} VRAM exhausted     {ExitCodes.SeparationFailed,-3} separation failed");
        w.WriteLine($"  {ExitCodes.EnvironmentFailed,-3} environment/bootstrap failed        {ExitCodes.Cancelled,-3} cancelled");
    }
}

/// <summary>Process exit codes produced by <see cref="Program"/> and <see cref="Runner"/>.</summary>
public static class ExitCodes
{
    /// <summary>The run finished and every requested stem was written.</summary>
    public const int Ok = 0;
    /// <summary>The command line could not be parsed, or a required argument was missing.</summary>
    public const int ArgumentError = 2;
    /// <summary>The input audio file named on the command line does not exist.</summary>
    public const int InputNotFound = 3;
    /// <summary>The output directory could not be created (read-only volume,
    /// a FILE already at that path, a denied ACL). Previously this escaped
    /// <see cref="Runner.RunAsync(DiagnosticsOptions,TextWriter,TextWriter,Runner.SeparationFunc,CancellationToken)"/>
    /// as an unhandled IOException.</summary>
    public const int OutputUnavailable = 4;
    /// <summary>The GPU ran out of video memory; retry on <c>cpu</c>, with less overlap,
    /// with <c>--shifts 0</c>, or with a smaller model.</summary>
    public const int VramExhausted = 10;
    /// <summary>The separation itself failed for any reason other than VRAM — a model that
    /// would not load, undecodable audio, a CUDA mismatch, or an unclassified child error.</summary>
    public const int SeparationFailed = 11;
    /// <summary>The embedded-Python bootstrap failed: pip, the interpreter
    /// download, a checksum mismatch, or the network. These surface as
    /// <see cref="InvalidOperationException"/> / <see cref="HttpRequestException"/>
    /// / <see cref="IOException"/>, none of which derive from
    /// <see cref="StemSeparationException"/> — so before this code existed
    /// they left the process with 0xE0434352 (unhandled exception).</summary>
    public const int EnvironmentFailed = 12;
    /// <summary>The run was cancelled, normally by Ctrl+C. 130 is the shell convention for
    /// "terminated by SIGINT", and a cancellation is deliberately NOT reported as a
    /// separation failure.</summary>
    public const int Cancelled = 130;
}

/// <summary>Parsed command-line input for <see cref="Runner"/>.</summary>
internal sealed record DiagnosticsOptions(
    string InputPath,
    string OutputDirectory,
    string Model,
    string Device,
    int Shifts,
    double Overlap,
    int MaxConcurrent,
    string? ModelPath,
    string? ConfigPath,
    bool SkipBootstrap,
    bool Quiet,
    TimeSpan? Timeout = null,
    IReadOnlyList<string>? Stems = null,
    bool HelpRequested = false,
    bool VersionRequested = false,
    bool ProbeOnly = false)
{
    /// <summary>Default child-process cap, matching
    /// <see cref="StemSeparationOptions.Timeout"/>.</summary>
    public static readonly TimeSpan DefaultTimeout = TimeSpan.FromHours(4);

    /// <summary>
    /// Parse the raw command line into an options record, applying the documented defaults
    /// (<c>htdemucs</c>, device <c>auto</c>, one shift, 0.25 overlap, one concurrent run).
    /// <c>--help</c>, <c>--version</c> and <c>--probe</c> come back as request flags rather
    /// than side effects, so the caller decides what to print.
    /// </summary>
    /// <param name="args">Raw command line.</param>
    /// <returns>The parsed options.</returns>
    /// <exception cref="ArgumentException">An unknown flag, a missing value, or a value
    /// that is not a number.</exception>
    public static DiagnosticsOptions Parse(string[] args)
    {
        string? input = null;
        string? output = null;
        var model = "htdemucs";
        var device = "auto";
        var shifts = 1;
        var overlap = 0.25;
        var maxConcurrent = 1;
        string? modelPath = null;
        string? configPath = null;
        var skipBootstrap = false;
        var quiet = false;
        var help = false;
        var version = false;
        var probe = false;
        TimeSpan? timeout = null;
        IReadOnlyList<string>? stems = null;

        for (var i = 0; i < args.Length; i++)
        {
            var a = args[i];
            string Next()
            {
                if (i + 1 >= args.Length)
                    throw new ArgumentException($"missing value for {a}.");
                return args[++i];
            }

            // int.Parse / double.Parse throw FormatException, which is NOT an
            // ArgumentException — so a typo in --shifts escaped Main's catch
            // and killed the process with 0xE0434352 instead of exiting 2.
            int Int(string v) =>
                int.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n)
                    ? n
                    : throw new ArgumentException($"{a} expects an integer, got '{v}'.");
            double Dbl(string v) =>
                double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out var d)
                    ? d
                    : throw new ArgumentException($"{a} expects a number, got '{v}'.");

            switch (a)
            {
                case "--input": input = Next(); break;
                case "--out": output = Next(); break;
                case "--model": model = Next(); break;
                case "--device": device = Next(); break;
                case "--shifts": shifts = Int(Next()); break;
                case "--overlap": overlap = Dbl(Next()); break;
                case "--max-concurrent": maxConcurrent = Int(Next()); break;
                case "--timeout":
                {
                    var minutes = Dbl(Next());
                    if (!(minutes > 0) || double.IsInfinity(minutes))
                        throw new ArgumentException($"--timeout expects a positive number of minutes, got '{minutes.ToString(CultureInfo.InvariantCulture)}'.");
                    timeout = TimeSpan.FromMinutes(minutes);
                    break;
                }
                case "--stems":
                {
                    var parts = Next()
                        .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
                    if (parts.Length == 0)
                        throw new ArgumentException("--stems expects a comma-separated list of stem names.");
                    stems = parts;
                    break;
                }
                case "--model-path": modelPath = Next(); break;
                case "--config": configPath = Next(); break;
                case "--skip-bootstrap": skipBootstrap = true; break;
                case "--quiet": quiet = true; break;
                case "--probe": probe = true; break;
                case "--version": version = true; break;
                case "--help":
                case "-h": help = true; break;
                default: throw new ArgumentException($"unknown flag '{a}'.");
            }
        }

        // --help / --version / --probe are complete requests on their own;
        // requiring --input alongside them made the switch arm above dead
        // code, because Main short-circuited on the flag before parsing.
        if (help || version || probe)
        {
            return new DiagnosticsOptions(
                input ?? string.Empty, output ?? string.Empty, model, device, shifts, overlap,
                maxConcurrent, modelPath, configPath, skipBootstrap, quiet,
                timeout, stems, help, version && !help, probe && !help && !version);
        }

        if (string.IsNullOrWhiteSpace(input))
            throw new ArgumentException("--input is required.");
        if (shifts < 0)
            throw new ArgumentException($"--shifts must be >= 0, got {shifts}.");
        if (overlap is < 0 or >= 1 || double.IsNaN(overlap))
            throw new ArgumentException($"--overlap must be in [0,1), got {overlap.ToString(CultureInfo.InvariantCulture)}.");
        if (maxConcurrent < 1)
            throw new ArgumentException($"--max-concurrent must be >= 1, got {maxConcurrent}.");
        output ??= Path.Combine(Environment.CurrentDirectory, "stems-out");

        return new DiagnosticsOptions(
            input, output, model, device, shifts, overlap,
            maxConcurrent, modelPath, configPath, skipBootstrap, quiet,
            timeout, stems);
    }

    /// <summary>Project the CLI options onto the backend's own option record — model,
    /// device, shifts, overlap, concurrency, explicit model/config paths, stem selection
    /// and timeout.</summary>
    /// <returns>Options ready to hand to the separator.</returns>
    public StemSeparationOptions ToBackendOptions() => new()
    {
        Model = Model,
        Device = Device,
        Shifts = Shifts,
        Overlap = Overlap,
        MaxConcurrentSeparations = MaxConcurrent,
        ModelPath = ModelPath,
        ConfigPath = ConfigPath,
        SkipPackageBootstrap = SkipBootstrap,
        Stems = Stems,
        Timeout = Timeout ?? DefaultTimeout,
    };
}

/// <summary>
/// Pure-async runner extracted from <see cref="Program.Main"/> so it can
/// be exercised by integration tests. The runner takes explicit
/// <see cref="TextWriter"/> sinks instead of touching
/// <see cref="Console"/> directly.
/// </summary>
internal static class Runner
{
    /// <summary>
    /// Delegate signature the runner dispatches to. Defaults to
    /// <see cref="StemSeparator.SeparateTrackAsync"/>; tests inject a
    /// pure in-memory implementation.
    /// </summary>
    public delegate Task<SeparationResult> SeparationFunc(
        string inputPath, string outputDirectory, StemSeparationOptions options,
        IProgress<StemSeparationProgress>? progress, CancellationToken ct);

    /// <summary>
    /// The PULL-shaped runner signature. This is the one the real CLI uses:
    /// the backend hands back a stream of samples ending in a terminal item
    /// that carries the <see cref="SeparationResult"/>.
    /// </summary>
    public delegate IAsyncEnumerable<StemSeparationProgress> StreamingSeparationFunc(
        string inputPath, string outputDirectory, StemSeparationOptions options, CancellationToken ct);

    /// <summary>
    /// Run a separation against a real <see cref="StemSeparator"/> and render its progress
    /// to the supplied writers — the shape the CLI itself uses. Adapts the separator's
    /// streaming API onto the shared rendering path, so there is one place that decides
    /// what a progress line looks like.
    /// </summary>
    /// <param name="opts">Parsed command-line options.</param>
    /// <param name="stdout">Sink for progress and the summary.</param>
    /// <param name="stderr">Sink for errors.</param>
    /// <param name="separator">Separator to drive; a default-constructed one when <c>null</c>.</param>
    /// <param name="ct">Cancels the run; a cancelled run returns <see cref="ExitCodes.Cancelled"/>.</param>
    /// <returns>One of <see cref="ExitCodes"/>.</returns>
    public static Task<int> RunAsync(
        DiagnosticsOptions opts,
        TextWriter stdout,
        TextWriter stderr,
        StemSeparator? separator = null,
        CancellationToken ct = default)
    {
        var sep = separator ?? new StemSeparator();
        return RunAsync(opts, stdout, stderr,
            (i, o, options, token) => sep.SeparateWithProgressAsync(i, o, options, token), ct);
    }

    /// <summary>
    /// PUSH-shaped overload, kept for hosts (and tests) that already have an
    /// <see cref="IProgress{T}"/>-shaped separation function. It does not
    /// re-implement the rendering: it adapts the callback into the same
    /// channel the pull path uses, so there is exactly one place that decides
    /// what a progress line looks like.
    /// </summary>
    public static Task<int> RunAsync(
        DiagnosticsOptions opts,
        TextWriter stdout,
        TextWriter stderr,
        SeparationFunc runner,
        CancellationToken ct = default)
    {
        return RunAsync(opts, stdout, stderr, Stream, ct);

        IAsyncEnumerable<StemSeparationProgress> Stream(
            string input, string outputDir, StemSeparationOptions options, CancellationToken token) =>
            CoalescingProgressStream.Create<StemSeparationProgress>(
                async (report, t) =>
                {
                    var result = await runner(input, outputDir, options,
                        new InlineProgress<StemSeparationProgress>(report), t).ConfigureAwait(false);
                    report(new StemSeparationProgress("done", 1.0, "Separation complete.",
                        Stem: null, Backend: StemSeparator.BackendId, Result: result));
                },
                ct: token);
    }

    /// <summary>
    /// The core PULL-shaped runner every other overload funnels into: validate the input
    /// and output paths, consume the progress stream, render each sample, and map the
    /// terminal outcome onto an exit code. This is the ONE place a progress line or a
    /// summary is formatted. Classified separation failures become
    /// <see cref="ExitCodes.VramExhausted"/> or <see cref="ExitCodes.SeparationFailed"/>;
    /// bootstrap failures become <see cref="ExitCodes.EnvironmentFailed"/>; a cancellation
    /// becomes <see cref="ExitCodes.Cancelled"/> and is never reported as a failure.
    /// </summary>
    /// <param name="opts">Parsed command-line options.</param>
    /// <param name="stdout">Sink for progress and the summary.</param>
    /// <param name="stderr">Sink for errors.</param>
    /// <param name="runner">Produces the progress stream, ending in a terminal item that
    /// carries the result. Tests inject a pure in-memory implementation here.</param>
    /// <param name="ct">Cancels the run.</param>
    /// <returns>One of <see cref="ExitCodes"/>.</returns>
    public static async Task<int> RunAsync(
        DiagnosticsOptions opts,
        TextWriter stdout,
        TextWriter stderr,
        StreamingSeparationFunc runner,
        CancellationToken ct = default)
    {
        if (!File.Exists(opts.InputPath))
        {
            stderr.WriteLine($"input file not found: {opts.InputPath}");
            return ExitCodes.InputNotFound;
        }
        try
        {
            Directory.CreateDirectory(opts.OutputDirectory);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            stderr.WriteLine($"cannot create output directory '{opts.OutputDirectory}': {ex.Message}");
            return ExitCodes.OutputUnavailable;
        }

        var sw = Stopwatch.StartNew();
        var vramExhausted = false;
        string? deviceUsed = null;

        try
        {
            // The +1 %-movement throttle that used to live here is GONE. The
            // backend publishes into a bounded, drop-oldest channel, so a
            // consumer that cannot keep up already coalesces at the transport
            // — a second throttle here only meant that two consumers of the
            // same backend discarded different samples for different reasons.
            SeparationResult? result = null;
            await foreach (var p in runner(
                               opts.InputPath, opts.OutputDirectory, opts.ToBackendOptions(), ct)
                           .WithCancellation(ct).ConfigureAwait(false))
            {
                // The terminal item carries the result; nothing else does.
                if (p.Result is not null) result = p.Result;
                if (opts.Quiet) continue;

                var frac = p.Fraction >= 0 ? $"{p.Fraction,6:P1}" : "   —  ";
                stdout.WriteLine($"[{Elapsed(sw)}] {p.Phase,-9} {frac} {p.Message}");
            }

            deviceUsed = result?.Device ?? opts.Device;
            sw.Stop();
            PrintSummary(stdout, opts, sw.Elapsed, deviceUsed, result?.Stems.Count ?? 0,
                vramExhausted, success: true);
            return ExitCodes.Ok;
        }
        catch (OperationCanceledException)
        {
            // Must precede the StemSeparationException arm: a cancel is not
            // a separation failure (see StemSeparator's cancel contract).
            sw.Stop();
            stderr.WriteLine("CANCELLED.");
            return ExitCodes.Cancelled;
        }
        catch (StemSeparationVramException ex)
        {
            vramExhausted = true;
            sw.Stop();
            stderr.WriteLine($"VRAM EXHAUSTED: {ex.Message}");
            PrintSummary(stdout, opts, sw.Elapsed, deviceUsed ?? opts.Device, 0,
                vramExhausted, success: false);
            return ExitCodes.VramExhausted;
        }
        catch (StemSeparationException ex)
        {
            sw.Stop();
            stderr.WriteLine($"SEPARATION FAILED ({ex.GetType().Name}): {ex.Message}");
            PrintSummary(stdout, opts, sw.Elapsed, deviceUsed ?? opts.Device, 0,
                vramExhausted, success: false);
            return ExitCodes.SeparationFailed;
        }
        catch (Exception ex) when (ex is InvalidOperationException or HttpRequestException
                                      or InvalidDataException or IOException or UnauthorizedAccessException)
        {
            // The embedded-Python bootstrap throws these, and none of them is
            // a StemSeparationException — so a failed torch install used to
            // escape as an unhandled exception (0xE0434352).
            sw.Stop();
            stderr.WriteLine($"ENVIRONMENT FAILED ({ex.GetType().Name}): {ex.Message}");
            PrintSummary(stdout, opts, sw.Elapsed, deviceUsed ?? opts.Device, 0,
                vramExhausted, success: false);
            return ExitCodes.EnvironmentFailed;
        }
    }

    /// <summary>Prints the embedded-Python status for <c>--probe</c>. Always
    /// returns <see cref="ExitCodes.Ok"/> — "not installed" is a valid
    /// answer to the question, not a failure of the tool.</summary>
    public static int WriteProbe(TextWriter stdout, PythonEnvStatus status)
    {
        stdout.WriteLine("============== Python Environment ===========");
        stdout.WriteLine($"  Interpreter    : {(status.InterpreterPresent ? "present" : "MISSING")}");
        stdout.WriteLine($"  Executable     : {status.Executable ?? "(none)"}");
        stdout.WriteLine($"  Version        : {status.Version ?? "(unknown)"}");
        stdout.WriteLine($"  pip            : {(status.HasPip ? "present" : "missing")}");
        stdout.WriteLine($"  torch          : {(status.HasTorch ? "present" : "missing")}");
        stdout.WriteLine($"  Ready          : {(status.InterpreterPresent && status.HasTorch ? "yes" : "no")}");
        stdout.WriteLine("=============================================");
        return ExitCodes.Ok;
    }

    /// <summary>Prints component versions for <c>--version</c>.</summary>
    public static void WriteVersion(TextWriter stdout)
    {
        stdout.WriteLine($"juce-stems-diag        {AssemblyVersionOf(typeof(Runner).Assembly)}");
        stdout.WriteLine($"JuceSharp.StemSeparation {AssemblyVersionOf(typeof(StemSeparator).Assembly)}");
        stdout.WriteLine($"JuceSharp (wrapper)    {AssemblyVersionOf(typeof(JuceSharp.JuceAudioEngine).Assembly)}");
        stdout.WriteLine($"JuceSharp.Native ABI   {NativeAbiVersion()}");
        stdout.WriteLine($"Embedded Python        {PythonEnvManager.DefaultPythonVersion}");
    }

    private static string AssemblyVersionOf(Assembly asm) =>
        asm.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? asm.GetName().Version?.ToString()
        ?? "(unknown)";

    private static string NativeAbiVersion()
    {
        // The wrapper's expected ABI is an internal const; the tool is a
        // diagnostic, so reflecting it is preferable to widening the
        // wrapper's public surface. Never throw out of --version.
        try
        {
            var t = typeof(JuceSharp.JuceAudioEngine).Assembly
                .GetType("JuceSharp.Interop.NativeMethods", throwOnError: false);
            var f = t?.GetField("ExpectedAbiVersion",
                BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Public);
            var v = f?.GetRawConstantValue();
            return v is null ? "(unknown)" : $"v{v} (expected by wrapper)";
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"juce-stems-diag: ABI probe failed: {ex.Message}");
            return "(unknown)";
        }
    }

    private static void PrintSummary(
        TextWriter stdout, DiagnosticsOptions opts, TimeSpan elapsed,
        string? device, int stemCount, bool vramExhausted, bool success)
    {
        long inputBytes = 0;
        try { inputBytes = new FileInfo(opts.InputPath).Length; }
        catch (Exception ex) { stdout.WriteLine($"  (input size unavailable: {ex.Message})"); }
        var mbPerSec = elapsed.TotalSeconds > 0
            ? inputBytes / 1024.0 / 1024.0 / elapsed.TotalSeconds
            : 0;

        stdout.WriteLine();
        stdout.WriteLine("================ Run Summary ================");
        stdout.WriteLine($"  Status         : {(success ? "OK" : "FAILED")}");
        stdout.WriteLine($"  Input          : {opts.InputPath}");
        stdout.WriteLine($"  Output         : {opts.OutputDirectory}");
        stdout.WriteLine($"  Model          : {opts.Model}");
        stdout.WriteLine($"  Device used    : {device ?? "(unknown)"}");
        stdout.WriteLine($"  Total elapsed  : {elapsed.TotalSeconds:0.00} s");
        stdout.WriteLine($"  Processing rate: {mbPerSec:0.00} MiB/s (wrt input file size)");
        stdout.WriteLine($"  Stems written  : {stemCount}");
        // A single run either hit VRAM exhaustion or it did not; the old
        // "VRAM failures : N" line could only ever read 0 or 1 and invited
        // the reader to believe it was counting retries.
        stdout.WriteLine($"  VRAM exhausted : {(vramExhausted ? "yes" : "no")}");
        stdout.WriteLine("=============================================");
    }

    private static string Elapsed(Stopwatch sw)
    {
        var t = sw.Elapsed;
        return $"{(int)t.TotalMinutes:00}:{t.Seconds:00}.{t.Milliseconds / 100:0}";
    }

    /// <summary>
    /// <see cref="IProgress{T}"/> that invokes the handler on the reporting
    /// thread. See the note at its single use site.
    /// </summary>
    internal sealed class InlineProgress<T>(Action<T> handler) : IProgress<T>
    {
        public void Report(T value) => handler(value);
    }
}
