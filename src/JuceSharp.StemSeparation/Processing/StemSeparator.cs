using System.Globalization;
using System.Runtime.CompilerServices;
using JuceSharp.StemSeparation.Runtime;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace JuceSharp.StemSeparation.Processing;

/// <summary>
/// Production audio stem separator. Drives a bundled Python sidecar
/// (<c>PythonScripts/separate_track.py</c>) that loads a Demucs or
/// BS-RoFormer model, runs inference on an input audio file, and writes
/// the resulting per-stem wavs to <c>outputDir</c>.
///
/// <para>
/// <b>Why subprocess.</b> A torch crash during inference (OOM on a 16-min
/// track, CUDA driver mismatch, malformed checkpoint) must not bring the
/// audio host down. Spawning a child process keeps the JUCE real-time
/// thread isolated from Python's GIL and from the multi-GB VRAM
/// allocation. Live progress is forwarded through stdout — the caller
/// sees a continuous <see cref="IProgress{T}"/> stream exactly as if the
/// model were embedded.
/// </para>
/// <para>
/// The bundled script speaks a tiny line protocol on stdout:
/// <list type="bullet">
///   <item><c>PROGRESS &lt;0..1&gt; [message]</c></item>
///   <item><c>STEM &lt;name&gt; &lt;path&gt;</c> for each written file</item>
///   <item><c>DEVICE &lt;cuda|mps|cpu&gt;</c> after device selection</item>
///   <item><c>READY</c> when the model is loaded</item>
/// </list>
/// Anything else is forwarded verbatim into the logger at info level.
/// </para>
/// </summary>
public sealed class StemSeparator : IStemSeparator
{
    /// <summary>Packages installed into the embedded interpreter on first
    /// run. <c>demucs</c> covers the htdemucs / mdx_extra families;
    /// <c>bs_roformer</c> covers the RoFormer family; <c>soundfile</c>
    /// handles wav/flac IO; <c>librosa</c> provides resampling.</summary>
    /// <remarks>Every pin comes from <see cref="PythonPackagePins"/> — see
    /// that type for why they may not be re-typed here.</remarks>
    public static readonly IReadOnlyList<string> RequiredPackages = new[]
    {
        PythonPackagePins.Torch,
        PythonPackagePins.TorchAudio,
        PythonPackagePins.Numpy,
        PythonPackagePins.SoundFile,
        PythonPackagePins.Librosa,
        PythonPackagePins.Demucs,
        PythonPackagePins.BsRoformer,
        PythonPackagePins.PyYaml,
        PythonPackagePins.SafeTensors,
    };

    // --- Global concurrency gate --------------------------------------
    //
    // Stem separation is GPU/VRAM-bound. Letting two callers fire
    // simultaneously on a single CUDA context essentially guarantees an
    // OOM crash on consumer hardware, and the slowdown from contention
    // outweighs the parallel speedup even on big cards. We default to
    // serial execution and let power users opt in to more via
    // StemSeparationOptions.MaxConcurrentSeparations.
    //
    // The gate is **process-global** because GPU memory is a
    // process-global resource — separate StemSeparator instances must
    // not be able to dodge the limit by being constructed independently.
    //
    // TWO gates, because a per-call request must not be permanent. The
    // CAPACITY gate is the physical ceiling and can only grow — a semaphore's
    // count cannot be lowered without racing whoever holds it. The POLICY gate
    // is GlobalConcurrencyLimit, and only the host's own startup call moves it.
    // A caller that asks for more than the standing policy is admitted by the
    // capacity gate alone (it opted in, explicitly, for that call); every other
    // caller ALSO holds the policy gate, which is what makes the request
    // transient. Before this split, one SeparateTrackAsync carrying
    // MaxConcurrentSeparations = 8 raised the ceiling irreversibly for every
    // future default-options caller — i.e. for exactly the callers the VRAM
    // gate exists for.
    //
    // Acquisition order is always capacity → policy, so two gates cannot
    // deadlock against each other.
    private static readonly object SGateLock = new();
    private static SemaphoreSlim _sGate = new(initialCount: 1, maxCount: int.MaxValue);
    private static SemaphoreSlim _sPolicyGate = new(initialCount: 1, maxCount: int.MaxValue);
    private static int _sCapacity = 1;
    private static int _sCurrentLimit = 1;

    /// <summary>
    /// Current process-wide ceiling on concurrent
    /// <see cref="SeparateTrackAsync"/> calls. Defaults to 1. Setting
    /// this value can only grow the limit (shrinking would race with
    /// in-flight callers); use it once at startup to size the gate for
    /// the host machine.
    /// <para>
    /// A per-call <see cref="StemSeparationOptions.MaxConcurrentSeparations"/>
    /// does NOT move this value — see the remarks on that property.
    /// </para>
    /// </summary>
    public static int GlobalConcurrencyLimit
    {
        get { lock (SGateLock) return _sCurrentLimit; }
        set
        {
            lock (SGateLock)
            {
                if (value < _sCurrentLimit)
                    throw new InvalidOperationException(
                        $"GlobalConcurrencyLimit can only grow ({value} < current {_sCurrentLimit}). " +
                        "Shrinking would race with in-flight separations.");
                if (value > _sCurrentLimit)
                {
                    _sPolicyGate.Release(value - _sCurrentLimit);
                    _sCurrentLimit = value;
                }
            }
            EnsureCapacity(value);
        }
    }

    /// <summary>
    /// Test hook: swap in a fresh size-1 gate.
    /// <para>
    /// The old instance is deliberately <b>NOT</b> disposed. An in-flight
    /// <see cref="SeparateTrackAsync"/> captured it into a local and will run
    /// <c>finally { gate.Release(); }</c> against it — disposing here made
    /// that release throw <see cref="ObjectDisposedException"/> out of a
    /// finally block, which is a failure in an unrelated test (or, outside
    /// tests, a lost separation). A <see cref="SemaphoreSlim"/> that has
    /// never had <c>AvailableWaitHandle</c> read holds no kernel handle, so
    /// letting the GC take it leaks nothing.
    /// </para>
    /// </summary>
    internal static void ResetGlobalGateForTests()
    {
        lock (SGateLock)
        {
            _sGate = new SemaphoreSlim(1, int.MaxValue);
            _sPolicyGate = new SemaphoreSlim(1, int.MaxValue);
            _sCapacity = 1;
            _sCurrentLimit = 1;
        }
    }

    /// <summary>Test hook: the gate instance a separation starting now would
    /// take. Used to prove <see cref="ResetGlobalGateForTests"/> leaves an
    /// already-captured gate usable.</summary>
    internal static SemaphoreSlim CurrentGateForTests => CurrentGate;

    /// <summary>
    /// Grows the PHYSICAL ceiling. Never touches
    /// <see cref="GlobalConcurrencyLimit"/>: raising the policy is the host's
    /// decision and belongs to that setter alone.
    /// </summary>
    private static void EnsureCapacity(int desired)
    {
        if (desired < 1) desired = 1;
        lock (SGateLock)
        {
            if (desired > _sCapacity)
            {
                _sGate.Release(desired - _sCapacity);
                _sCapacity = desired;
            }
        }
    }

    /// <summary>
    /// Takes one separation slot, honouring the standing policy unless this
    /// call explicitly asked for more.
    /// <para>
    /// Dispose the handle to release. Always release — even on cancellation,
    /// exception or timeout — or a crashed call blocks the queue for ever.
    /// </para>
    /// </summary>
    internal static async Task<IDisposable> AcquireSeparationSlotAsync(int requested, CancellationToken ct)
    {
        if (requested < 1) requested = 1;
        EnsureCapacity(requested);

        SemaphoreSlim capacity, policy;
        bool honoursPolicy;
        lock (SGateLock)
        {
            capacity = _sGate;
            policy = _sPolicyGate;
            // Asking for MORE than the standing policy is the opt-in; asking
            // for that much or less is an ordinary caller and queues behind it.
            honoursPolicy = requested <= _sCurrentLimit;
        }

        await capacity.WaitAsync(ct).ConfigureAwait(false);
        if (!honoursPolicy) return new SeparationSlot(capacity, null);
        try
        {
            await policy.WaitAsync(ct).ConfigureAwait(false);
        }
        catch
        {
            capacity.Release();
            throw;
        }
        return new SeparationSlot(capacity, policy);
    }

    private sealed class SeparationSlot(SemaphoreSlim capacity, SemaphoreSlim? policy) : IDisposable
    {
        private int _disposed;
        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            policy?.Release();
            capacity.Release();
        }
    }

    private static SemaphoreSlim CurrentGate
    {
        get { lock (SGateLock) return _sGate; }
    }

    private readonly PythonEnvManager _env;
    private readonly ILogger _logger;
    private readonly string _scriptPath;

    /// <summary>
    /// Bind the separator to a Python environment and locate the bundled demucs script.
    /// Nothing is downloaded or launched here — provisioning happens on the first
    /// separation.
    /// </summary>
    /// <param name="env">Interpreter manager to run the script under; a default-rooted
    /// <see cref="PythonEnvManager"/> when <c>null</c>.</param>
    /// <param name="logger">Log sink; a null logger when <c>null</c>.</param>
    /// <param name="scriptPathOverride">Path to a separation script to use instead of the
    /// one shipped beside the assembly — for tests and for a patched local copy.</param>
    public StemSeparator(
        PythonEnvManager? env = null,
        ILogger<StemSeparator>? logger = null,
        string? scriptPathOverride = null)
    {
        _env = env ?? new PythonEnvManager();
        _logger = logger ?? NullLogger<StemSeparator>.Instance;
        _scriptPath = scriptPathOverride ?? ResolveBundledScript();
    }

    /// <summary>Without doing any network IO, returns whether the embedded
    /// interpreter and torch are installed. Useful for gating UI
    /// "Separate stems" buttons behind a one-time install prompt.</summary>
    public bool IsReady()
    {
        var s = _env.Probe();
        return s.InterpreterPresent && s.HasTorch;
    }

    /// <summary>Report what the embedded interpreter currently has installed, without any
    /// network IO — the gate behind a "Separate stems" button. Blocks briefly on a child
    /// process the first time; use <see cref="ProbeAsync"/> from a UI thread.</summary>
    /// <returns>Whether the interpreter, torch and the other pinned packages are present.</returns>
    public PythonEnvStatus Probe() => _env.Probe();

    /// <summary>Asynchronous <see cref="Probe"/>; never blocks the caller on
    /// the one-time interpreter version child process.</summary>
    public Task<PythonEnvStatus> ProbeAsync(CancellationToken ct = default) => _env.ProbeAsync(ct);

    /// <summary>Asynchronous <see cref="IsReady"/>.</summary>
    public async Task<bool> IsReadyAsync(CancellationToken ct = default)
    {
        var s = await _env.ProbeAsync(ct).ConfigureAwait(false);
        return s.InterpreterPresent && s.HasTorch;
    }

    /// <summary>
    /// Run the separation pipeline. Bootstraps the embedded interpreter +
    /// required packages on first call. Honours
    /// <paramref name="ct"/> by killing the child process. Returns a
    /// <see cref="SeparationResult"/> listing the written stems.
    /// </summary>
    public async Task<SeparationResult> SeparateTrackAsync(
        string inputPath,
        string outputDir,
        StemSeparationOptions? options = null,
        IProgress<StemSeparationProgress>? progress = null,
        CancellationToken ct = default)
    {
        var result = await SeparateCoreAsync(
            inputPath, outputDir, options,
            progress is null ? null : progress.Report, ct).ConfigureAwait(false);
        // The terminal sample is emitted HERE rather than inside the core, so
        // the push path and the pull path publish byte-identical terminal
        // items (same phase, same fraction, same Result payload).
        progress?.Report(TerminalSample(result));
        return result;
    }

    /// <summary>
    /// <see cref="SeparateTrackAsync"/> as a PULL stream. Every sample the run
    /// produces — install, load, queue, inference, per-stem, done — arrives as
    /// one <see cref="StemSeparationProgress"/>, and the LAST one carries the
    /// whole <see cref="SeparationResult"/> in
    /// <see cref="StemSeparationProgress.Result"/>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <b>Not a wrapper over <see cref="IProgress{T}"/>.</b> The channel writer
    /// IS the sink the stdout line parser reports into — there is no
    /// <see cref="IProgress{T}"/>, no <see cref="Progress{T}"/> post and no
    /// second buffering layer between the sidecar's <c>PROGRESS</c> line and
    /// the consumer. Both public surfaces sit on
    /// <see cref="SeparateCoreAsync"/> as peers.
    /// </para>
    /// <para>
    /// Cancellation, coalescing under a slow consumer, and the
    /// "faults keep their original type" rule are all
    /// <see cref="CoalescingProgressStream"/>'s — including the guarantee that
    /// a cancelled run surfaces
    /// <see cref="OperationCanceledException"/> from <c>MoveNextAsync</c> and
    /// never a <see cref="StemSeparationException"/>.
    /// </para>
    /// </remarks>
    public async IAsyncEnumerable<StemSeparationProgress> SeparateWithProgressAsync(
        string inputPath,
        string outputDir,
        StemSeparationOptions? options = null,
        [EnumeratorCancellation] CancellationToken ct = default)
    {
        var stream = CoalescingProgressStream.Create<StemSeparationProgress>(
            async (report, token) =>
            {
                var result = await SeparateCoreAsync(inputPath, outputDir, options, report, token)
                    .ConfigureAwait(false);
                report(TerminalSample(result));
            },
            ct: ct);

        await foreach (var item in stream.ConfigureAwait(false))
            yield return item;
    }

    /// <summary>Backend identifier stamped on every sample this separator
    /// publishes, so a host merging several backends' streams can tell them
    /// apart without tracking which task produced which sample.</summary>
    public const string BackendId = "python-sidecar";

    private static StemSeparationProgress TerminalSample(SeparationResult result) =>
        new("done", 1.0, "Separation complete.", Stem: null, Backend: BackendId, Result: result);

    private async Task<SeparationResult> SeparateCoreAsync(
        string inputPath,
        string outputDir,
        StemSeparationOptions? options,
        Action<StemSeparationProgress>? publish,
        CancellationToken ct)
    {
        // One stamping point: every sample leaving this run carries BackendId,
        // so no individual report site can forget it.
        var report = publish is null
            ? (Action<StemSeparationProgress>?)null
            : p => publish(p with { Backend = BackendId });

        if (string.IsNullOrWhiteSpace(inputPath))
            throw new ArgumentException("Input path is required.", nameof(inputPath));
        if (string.IsNullOrWhiteSpace(outputDir))
            throw new ArgumentException("Output directory is required.", nameof(outputDir));

        // Root every path BEFORE validating it, because the parent and the child
        // do not share a working directory: RunStreamingCore forces the child's
        // CWD to the interpreter's own folder (%LOCALAPPDATA%\JuceSharp\
        // python-embed) and BuildArgs passes --input/--out verbatim. A caller
        // who passed "song.wav" therefore passed File.Exists here and then had
        // the sidecar miss the file, or write the stems into the embed root.
        // The result record echoes the rooted paths for the same reason.
        inputPath = Path.GetFullPath(inputPath);
        outputDir = Path.GetFullPath(outputDir);
        if (!File.Exists(inputPath))
            throw new FileNotFoundException("Input audio not found.", inputPath);
        if (!File.Exists(_scriptPath))
            throw new FileNotFoundException(
                "Bundled separator script not found. Re-install JuceSharp.StemSeparation " +
                "or pass scriptPathOverride.", _scriptPath);

        Directory.CreateDirectory(outputDir);
        options ??= StemSeparationOptions.Default;

        // A Demucs bag is only ever loaded from pinned, hash-verified files
        // (DemucsWeightPins); a model with no pin is refused here, before
        // anything is downloaded or installed.
        var weightBag = ResolveWeightBag(options);
        ct.ThrowIfCancellationRequested();

        // Install progress is folded into the front 5% of the bar so the
        // perceived progress curve stays monotonic across phases.
        var installScale = 0.05;
        // ImmediateProgress, never Progress<T>: Progress<T> posts to the
        // captured SynchronizationContext, so an install sample would reach
        // the channel AFTER the terminal item on any host with one.
        var installProgress = report is null
            ? null
            : new ImmediateProgress<PythonInstallProgress>(p =>
                report(new StemSeparationProgress(
                    p.Phase, p.Fraction * installScale, p.Message, Stem: null)));

        await _env.EnsureInterpreterAsync(installProgress, ct).ConfigureAwait(false);
        if (!options.SkipPackageBootstrap)
            await _env.EnsurePackagesAsync(RequiredPackages, installProgress, ct).ConfigureAwait(false);

        string? weightsDir = null;
        if (weightBag is not null)
        {
            report?.Invoke(new StemSeparationProgress("weights", installScale,
                $"Fetching the pinned {weightBag.Model} weights…", Stem: null));
            weightsDir = await DemucsWeightPins.EnsureAsync(weightBag,
                options.DemucsWeightsDirectory ?? Path.Combine(_env.Root, "demucs-weights", weightBag.Model),
                line => _logger.LogInformation("[weights] {Line}", line), ct).ConfigureAwait(false);
        }

        report?.Invoke(new StemSeparationProgress("loading", installScale,
            $"Loading {options.Model} on {options.Device}…", Stem: null));

        var args = BuildArgs(_scriptPath, inputPath, outputDir, options, weightsDir);
        var stems = new List<StemFile>();
        string? deviceUsed = null;

        // Throttle: serialize GPU-bound inference unless the caller has
        // explicitly opted into more concurrency. The wait happens
        // *after* install so a queued caller doesn't block the embed
        // bootstrap. (The bootstrap has its own, separate, CROSS-PROCESS
        // gate — PythonEnvManager.AcquireEnvLockAsync. This comment used to
        // claim that serialization existed before it did.)
        report?.Invoke(new StemSeparationProgress("queued", installScale,
            "Waiting for a free GPU slot…", Stem: null));
        var slot = await AcquireSeparationSlotAsync(options.MaxConcurrentSeparations, ct).ConfigureAwait(false);
        bool ok;
        string stdout, stderr;
        try
        {
            (ok, stdout, stderr) = await PythonEnvManager.RunStreaming(
                _env.Executable, args, options.Timeout,
                onLine: line =>
                {
                    if (line.StartsWith("PROGRESS ", StringComparison.Ordinal))
                    {
                        var rest = line.AsSpan("PROGRESS ".Length);
                        var sp = rest.IndexOf(' ');
                        var fracTok = sp < 0 ? rest : rest[..sp];
                        var msg = sp < 0 ? string.Empty : rest[(sp + 1)..].ToString();
                        if (double.TryParse(fracTok, NumberStyles.Float, CultureInfo.InvariantCulture, out var frac))
                        {
                            var scaled = installScale + Math.Clamp(frac, 0, 1) * (1 - installScale);
                            report?.Invoke(new StemSeparationProgress("inference", scaled, msg, Stem: null));
                        }
                    }
                    else if (line.StartsWith("STEM ", StringComparison.Ordinal))
                    {
                        var rest = line["STEM ".Length..];
                        var sp = rest.IndexOf(' ');
                        if (sp > 0)
                        {
                            var name = rest[..sp];
                            var path = rest[(sp + 1)..];
                            stems.Add(new StemFile(name, path));
                            report?.Invoke(new StemSeparationProgress("stem", -1, $"Wrote stem '{name}'.", Stem: name));
                        }
                    }
                    else if (line.StartsWith("DEVICE ", StringComparison.Ordinal))
                    {
                        deviceUsed = line["DEVICE ".Length..].Trim();
                    }
                    else if (line.StartsWith("READY", StringComparison.Ordinal))
                    {
                        report?.Invoke(new StemSeparationProgress("ready", -1, "Model loaded.", Stem: null));
                    }
                    else if (line.Length > 0)
                    {
                        _logger.LogInformation("[separator] {Line}", line);
                    }
                },
                ct).ConfigureAwait(false);
        }
        finally
        {
            // Always release — even on cancellation, exception, or timeout —
            // so a crashed call doesn't permanently block the queue.
            slot.Dispose();
        }

        // A cancel is not a separation FAILURE. RunStreaming now kills the
        // child (Kill(entireProcessTree: true)) and THROWS
        // OperationCanceledException itself, so this line is a backstop for
        // the window between the child exiting normally and us observing the
        // token — keep it: without it a cancel that lands after the last
        // block would still be reported as StemSeparationException(Unknown),
        // i.e. to any consumer that does not special-case it, a reason to
        // fall through to the next backend.
        ct.ThrowIfCancellationRequested();

        if (!ok)
        {
            _logger.LogError("Stem separation failed. stderr:\n{Stderr}", stderr);
            throw StemSeparationError.Build(stderr, stdout, options.Model,
                deviceUsed ?? options.Device);
        }

        // NO terminal sample here: both public surfaces emit TerminalSample
        // themselves, so the "done" item always carries the Result and there
        // is exactly one of it.
        return new SeparationResult(inputPath, outputDir, options.Model,
            deviceUsed ?? options.Device, stems);
    }

    /// <summary>Internal so the path contract can be asserted without a
    /// sidecar: every path in this argv has to be ROOTED, because the child's
    /// working directory is the interpreter's and not ours.</summary>
    internal static List<string> BuildArgs(string script, string input, string outDir, StemSeparationOptions opt,
        string? weightsDir = null)
    {
        var args = new List<string>
        {
            script,
            // Rooted HERE as well as at the validation site, so the guarantee
            // belongs to the argv builder and cannot be lost by a second caller.
            // Path.GetFullPath is idempotent on an already-rooted path.
            "--input",  Path.GetFullPath(input),
            "--out",    Path.GetFullPath(outDir),
            "--model",  opt.Model,
            "--device", opt.Device,
            "--shifts", opt.Shifts.ToString(CultureInfo.InvariantCulture),
            "--overlap", opt.Overlap.ToString("0.0##", CultureInfo.InvariantCulture),
        };
        // Same reason as --input/--out: the child's CWD is the interpreter's.
        if (!string.IsNullOrWhiteSpace(opt.ModelPath))
        {
            args.Add("--model-path");
            args.Add(Path.GetFullPath(opt.ModelPath));
        }
        if (!string.IsNullOrWhiteSpace(opt.ConfigPath))
        {
            args.Add("--config");
            args.Add(Path.GetFullPath(opt.ConfigPath));
        }
        if (!string.IsNullOrWhiteSpace(weightsDir))
        {
            args.Add("--weights-dir");
            args.Add(Path.GetFullPath(weightsDir));
        }
        if (opt.Stems is { Count: > 0 })
        {
            args.Add("--stems");
            args.Add(string.Join(',', opt.Stems));
        }
        return args;
    }

    /// <summary>
    /// The pinned bag a Demucs separation will load, or null when this call
    /// fetches no weights: a RoFormer model (it reads
    /// <see cref="StemSeparationOptions.ModelPath"/>), or a caller that skipped
    /// the bootstrap and named no <see cref="StemSeparationOptions.DemucsWeightsDirectory"/>
    /// (the script then refuses to load a Demucs bag at all).
    /// </summary>
    /// <exception cref="ArgumentException">The model has no pinned weights.</exception>
    internal static DemucsWeightBag? ResolveWeightBag(StemSeparationOptions options)
    {
        if (string.Equals(options.Model, "bs_roformer", StringComparison.OrdinalIgnoreCase)) return null;
        if (options.SkipPackageBootstrap && options.DemucsWeightsDirectory is null) return null;
        return DemucsWeightPins.Find(options.Model) ?? throw new ArgumentException(
            $"'{options.Model}' has no pinned weights. Pinned Demucs models: " +
            string.Join(", ", DemucsWeightPins.All.Select(b => b.Model)) + ".", nameof(options));
    }

    private static string ResolveBundledScript()
    {
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "PythonScripts", "separate_track.py"),
            Path.Combine(AppContext.BaseDirectory, "separate_track.py"),
        };
        foreach (var c in candidates) if (File.Exists(c)) return c;
        return candidates[0];
    }
}

/// <summary>Configuration for a single <see cref="StemSeparator.SeparateTrackAsync"/> call.</summary>
public sealed record StemSeparationOptions
{
    /// <summary>Model identifier passed to the script. For Demucs models
    /// these are the canonical names with pinned weights (<c>htdemucs</c>,
    /// <c>htdemucs_ft</c>, <c>htdemucs_6s</c>, <c>hdemucs_mmi</c>, <c>mdx</c>,
    /// <c>mdx_extra</c>); any other name is refused. For RoFormer models pass
    /// <c>bs_roformer</c> and supply <see cref="ModelPath"/> +
    /// <see cref="ConfigPath"/>.</summary>
    public string Model { get; init; } = "htdemucs";

    /// <summary><c>cuda</c>, <c>mps</c>, <c>cpu</c>, or <c>auto</c>.</summary>
    public string Device { get; init; } = "auto";

    /// <summary>Number of random-shift augmentation passes. Demucs default
    /// is 1. Higher values trade compute for SNR.</summary>
    public int Shifts { get; init; } = 1;

    /// <summary>Chunk overlap fraction (0..1). Demucs default is 0.25.</summary>
    public double Overlap { get; init; } = 0.25;

    /// <summary>Optional explicit checkpoint or ONNX path. Required for
    /// custom RoFormer models.</summary>
    public string? ModelPath { get; init; }

    /// <summary>Optional yaml config (RoFormer family).</summary>
    public string? ConfigPath { get; init; }

    /// <summary>Restrict output to a subset of stems (e.g.
    /// <c>["vocals", "other"]</c>). Null means write every stem the model
    /// produces.</summary>
    public IReadOnlyList<string>? Stems { get; init; }

    /// <summary>Hard upper bound for the child process. Default 4 hours
    /// (covers full albums on CPU).</summary>
    public TimeSpan Timeout { get; init; } = TimeSpan.FromHours(4);

    /// <summary>When true, <see cref="StemSeparator.SeparateTrackAsync"/>
    /// skips the <c>pip install torch demucs …</c> step and assumes the
    /// caller has provisioned the interpreter themselves. Useful for
    /// advanced users with system-managed envs and for offline tests.
    /// <para>It also skips the Demucs weight download unless
    /// <see cref="DemucsWeightsDirectory"/> is set; the bundled script then
    /// refuses a Demucs model, because it never fetches weights itself.</para></summary>
    public bool SkipPackageBootstrap { get; init; }

    /// <summary>
    /// Where the pinned Demucs weights are kept (one directory per model).
    /// Null means <c>&lt;interpreter root&gt;/demucs-weights/&lt;model&gt;</c>.
    /// Whatever is there is checked against its pinned SHA-256 before use, and
    /// a missing or wrong file is fetched again from its pinned revision.
    /// </summary>
    public string? DemucsWeightsDirectory { get; init; }

    /// <summary>
    /// Process-global ceiling on concurrent separations. Defaults to 1
    /// because Demucs/RoFormer routinely consume 6–14 GiB of VRAM —
    /// firing two on a 12 GiB card guarantees a
    /// <see cref="StemSeparationVramException"/>.
    /// <para>
    /// Power users with high-VRAM cards (24 GiB+) can raise this to
    /// allow N tracks to process in parallel.
    /// </para>
    /// <para>
    /// <b>The value applies to THIS call only.</b> It admits this separation
    /// past the standing policy; it does not move
    /// <see cref="StemSeparator.GlobalConcurrencyLimit"/>, so the next
    /// default-options caller is serialized again exactly as before. (It used
    /// to raise the process-wide ceiling permanently, which silently removed
    /// the VRAM gate for every later caller.) To raise the standing policy for
    /// the whole process, set <see cref="StemSeparator.GlobalConcurrencyLimit"/>
    /// once at startup.
    /// </para>
    /// </summary>
    public int MaxConcurrentSeparations { get; init; } = 1;

    /// <summary>The all-defaults option set, shared and immutable: the standing model,
    /// device and overlap, and a per-call concurrency of one. Use <c>with</c> to vary a
    /// single knob rather than constructing from scratch.</summary>
    public static StemSeparationOptions Default { get; } = new();
}

/// <summary>Single progress sample emitted during a separation run.</summary>
/// <param name="Phase">Coarse phase identifier: <c>install</c>, <c>loading</c>,
/// <c>ready</c>, <c>inference</c>, <c>stem</c>, <c>done</c>.</param>
/// <param name="Fraction">0..1 overall progress, or -1 for sub-events that
/// don't carry a fractional position (e.g. a stem being written).</param>
/// <param name="Message">Human-readable status.</param>
/// <param name="Stem">Stem name if the event is a STEM message.</param>
/// <param name="Backend">Which backend produced the sample —
/// <see cref="StemSeparator.BackendId"/> for the python sidecar, the
/// implementation's type name for a third-party
/// <see cref="IStemSeparator"/>. Null on a sample built by hand.</param>
/// <param name="Result">
/// Non-null on EXACTLY ONE sample per run: the terminal <c>done</c> item.
/// <para>
/// This is what makes coalescing safe. A bounded, drop-oldest progress
/// channel may discard interim samples when the consumer is behind — but the
/// terminal item is by construction the newest write, so it always survives,
/// and it carries the complete <see cref="SeparationResult"/> including every
/// stem. A consumer can therefore miss any number of <c>stem</c> samples and
/// still end up with the full list.
/// </para>
/// <para>
/// The two trailing parameters have defaults so the four-argument shape every
/// existing caller writes still compiles.
/// </para>
/// </param>
public readonly record struct StemSeparationProgress(
    string Phase,
    double Fraction,
    string Message,
    string? Stem,
    string? Backend = null,
    SeparationResult? Result = null);

/// <summary>One stem produced by a separation run.</summary>
public readonly record struct StemFile(string Name, string Path);

/// <summary>Outcome of a successful <see cref="StemSeparator.SeparateTrackAsync"/> call.</summary>
public sealed record SeparationResult(
    string InputPath,
    string OutputDirectory,
    string Model,
    string Device,
    IReadOnlyList<StemFile> Stems)
{
    /// <summary>Convenience accessor for a stem by name (case-insensitive).</summary>
    public string? GetStemPath(string name) =>
        Stems.FirstOrDefault(s => string.Equals(s.Name, name, StringComparison.OrdinalIgnoreCase)).Path;
}
