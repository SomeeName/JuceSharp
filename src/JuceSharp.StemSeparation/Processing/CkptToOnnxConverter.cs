using JuceSharp.StemSeparation.Runtime;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace JuceSharp.StemSeparation.Processing;

/// <summary>
/// Converts a PyTorch <c>.ckpt</c> stem-separation model into a runtime-loadable
/// <c>.onnx</c> file by invoking the bundled <c>PythonScripts/ckpt_to_onnx.py</c>
/// against an embedded Python interpreter (see <see cref="PythonEnvManager"/>).
///
/// Resolution: the interpreter is ALWAYS sourced from the
/// supplied <see cref="PythonEnvManager"/> (defaulting to one rooted at
/// <c>%LOCALAPPDATA%/JuceSharp/python-embed</c>). The host machine's
/// global <c>PATH</c>, <c>PYTHONHOME</c>, user pip caches, and registry
/// entries are NEVER consulted — the user must not be required to
/// install Python, PyTorch, or any pip components on their machine. On
/// first use, <see cref="ConvertAsync"/> bootstraps the embedded
/// distribution (~30 MB Python + ~2 GB torch) into the AppData root and
/// pip-installs the required packages there.
///
/// We need <c>torch</c> + <c>numpy</c> + <c>pyyaml</c> + <c>onnx</c> at runtime
/// for any architecture-aware export; <c>bs_roformer</c> is required for
/// BS-RoFormer / RoFormer family checkpoints. The converter installs these on
/// demand the first time it runs.
/// </summary>
public sealed class CkptToOnnxConverter
{
    /// <summary>Packages auto-installed into the embedded interpreter on
    /// first run. Pinned conservatively; users can override via
    /// <see cref="ConvertAsync(string, string?, string, IProgress{PythonInstallProgress}?, CancellationToken)"/>'s
    /// progress callback to display a spinner during the (multi-GB) torch fetch.</summary>
    /// <remarks>Every pin comes from <see cref="PythonPackagePins"/> — this
    /// converter and <see cref="StemSeparator"/> install into the SAME
    /// interpreter root, so a divergent pin means one of them silently
    /// re-resolves the other's torch.</remarks>
    public static readonly IReadOnlyList<string> RequiredPackages = new[]
    {
        PythonPackagePins.Torch,
        PythonPackagePins.Numpy,
        PythonPackagePins.PyYaml,
        PythonPackagePins.Onnx,
        PythonPackagePins.BsRoformer,
        PythonPackagePins.SafeTensors,
    };

    /// <summary>
    /// The token <c>ckpt_to_onnx.py</c> writes to stderr when a checkpoint
    /// would need a full unpickle and <see cref="AllowUnsafePickleFallback"/>
    /// was not set. <see cref="ConvertAsync"/> maps it to
    /// <see cref="UnsafeCheckpointException"/>.
    /// </summary>
    public const string UnsafePickleRefusedMarker = "UNSAFE_PICKLE_REFUSED";

    /// <summary>
    /// A <c>.ckpt</c> / <c>.pt</c> / <c>.pth</c> is a pickle, and unpickling
    /// runs whatever code the file names. The script always tries
    /// <c>torch.load(weights_only=True)</c> first (a real boundary on the
    /// pinned torch 2.6+) and loads <c>.safetensors</c> without pickle at
    /// all. Only when this is <c>true</c> may it fall back to a FULL unpickle
    /// — set it only after the user has explicitly accepted that the file can
    /// run code on their machine. Default <c>false</c>.
    /// </summary>
    public bool AllowUnsafePickleFallback { get; init; }

    private readonly PythonEnvManager _env;
    private readonly ILogger _logger;
    private readonly string _scriptPath;

    /// <summary>
    /// Bind the converter to a Python environment and locate the bundled conversion script.
    /// Nothing is downloaded or launched here — provisioning happens on the first
    /// <c>ConvertAsync</c>.
    /// </summary>
    /// <param name="env">Interpreter manager to run the script under; a default-rooted
    /// <see cref="PythonEnvManager"/> when <c>null</c>.</param>
    /// <param name="logger">Log sink; a null logger when <c>null</c>.</param>
    /// <param name="scriptPathOverride">Path to a conversion script to use instead of the
    /// one shipped beside the assembly — for tests and for a patched local copy.</param>
    public CkptToOnnxConverter(
        PythonEnvManager? env = null,
        ILogger<CkptToOnnxConverter>? logger = null,
        string? scriptPathOverride = null)
    {
        _env = env ?? new PythonEnvManager();
        _logger = logger ?? NullLogger<CkptToOnnxConverter>.Instance;
        _scriptPath = scriptPathOverride ?? ResolveBundledScript();
    }

    /// <summary>Returns true when an interpreter is reachable and torch is
    /// importable — caller can use this to decide whether to surface a "you
    /// need to install Python (~10MB) and torch (~2GB) to convert .ckpt
    /// models" prompt before showing the import dialog.</summary>
    public bool IsReady()
    {
        var s = _env.Probe();
        return s.InterpreterPresent && s.HasTorch;
    }

    /// <summary>Report what the embedded interpreter currently has installed, without any
    /// network IO. Blocks briefly on a child process the first time; prefer the async
    /// probe on a UI thread.</summary>
    /// <returns>Whether the interpreter, torch and the other pinned packages are present.</returns>
    public PythonEnvStatus Probe() => _env.Probe();

    /// <summary>
    /// Convert <paramref name="ckptPath"/> (with optional <paramref name="yamlPath"/>
    /// next to it) into <paramref name="onnxOutputPath"/>. Bootstraps the
    /// embedded interpreter and required packages if they aren't already
    /// installed.
    /// </summary>
    public async Task ConvertAsync(
        string ckptPath,
        string? yamlPath,
        string onnxOutputPath,
        IProgress<PythonInstallProgress>? progress = null,
        CancellationToken ct = default)
    {
        // Root every path before validating it: the child's working directory is
        // the interpreter's own folder, not ours, and the arguments go over
        // verbatim — see the same note in StemSeparator.SeparateCoreAsync.
        if (string.IsNullOrWhiteSpace(ckptPath))
            throw new ArgumentException("Checkpoint path is required.", nameof(ckptPath));
        if (string.IsNullOrWhiteSpace(onnxOutputPath))
            throw new ArgumentException("Output path is required.", nameof(onnxOutputPath));
        ckptPath = Path.GetFullPath(ckptPath);
        onnxOutputPath = Path.GetFullPath(onnxOutputPath);
        if (!string.IsNullOrWhiteSpace(yamlPath)) yamlPath = Path.GetFullPath(yamlPath);

        if (!File.Exists(ckptPath))
            throw new FileNotFoundException("Checkpoint not found.", ckptPath);
        if (!File.Exists(_scriptPath))
            throw new FileNotFoundException(
                "Bundled converter script not found. Re-install JuceSharp.StemSeparation " +
                "or pass scriptPathOverride.", _scriptPath);

        await _env.EnsureInterpreterAsync(progress, ct).ConfigureAwait(false);
        await _env.EnsurePackagesAsync(RequiredPackages, progress, ct).ConfigureAwait(false);

        await ConvertCoreAsync(ckptPath, yamlPath, onnxOutputPath, progress,
            (args, token) => PythonEnvManager.Run(_env.Executable, args, timeout: TimeSpan.FromHours(1), token),
            ct).ConfigureAwait(false);
    }

    /// <summary>
    /// Everything after provisioning: build the argv, run it, and promote the
    /// result. <paramref name="runner"/> is the seam — the public entry point
    /// passes the real interpreter, a test passes a stub, so the temp-file and
    /// promotion contract is coverable without a 2 GB torch install.
    /// Both paths arrive here with ROOTED paths.
    /// </summary>
    internal async Task ConvertCoreAsync(
        string ckptPath,
        string? yamlPath,
        string onnxOutputPath,
        IProgress<PythonInstallProgress>? progress,
        Func<IReadOnlyList<string>, CancellationToken, Task<(bool ok, string stdout, string stderr)>> runner,
        CancellationToken ct)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(onnxOutputPath)!);
        progress?.Report(new PythonInstallProgress("convert", 0.0,
            $"Converting '{Path.GetFileName(ckptPath)}'… (this can take several minutes)"));

        // Write to a temp file and rename, never straight to the canonical path.
        // This runs for minutes inside a child process we kill on cancel; a
        // cancel, a crash, a full disk or a power cut otherwise leaves a
        // TRUNCATED .onnx exactly where every later load looks for it, and
        // File.Exists says yes to it. That is the failure VerifiedDownload
        // exists to prevent one door over ("a half-written download from a
        // killed process becomes a permanent, mystifying load failure"), and
        // this door did not have it.
        var tmp = onnxOutputPath + ".tmp." + Guid.NewGuid().ToString("N");
        var argumentList = new List<string>
        {
            _scriptPath,
            "--ckpt", ckptPath,
            "--out",  tmp,
        };
        if (!string.IsNullOrWhiteSpace(yamlPath) && File.Exists(yamlPath))
        {
            argumentList.Add("--config");
            argumentList.Add(yamlPath);
        }
        if (AllowUnsafePickleFallback)
            argumentList.Add("--allow-unsafe-pickle");

        bool ok;
        string stdout, stderr;
        try
        {
            (ok, stdout, stderr) = await runner(argumentList, ct).ConfigureAwait(false);
        }
        catch
        {
            // Includes the cancellation path, which must not leave the partial
            // behind either.
            TryDelete(tmp);
            throw;
        }

        if (!ok || !File.Exists(tmp))
        {
            TryDelete(tmp);
            _logger.LogError("ckpt→onnx conversion failed. stdout:\n{Stdout}\nstderr:\n{Stderr}", stdout, stderr);
            if (stderr.Contains(UnsafePickleRefusedMarker, StringComparison.Ordinal))
                throw new UnsafeCheckpointException(
                    $"'{Path.GetFileName(ckptPath)}' cannot be read as plain weights, and loading it fully " +
                    "would run code contained in the file, which was not allowed.");
            throw new InvalidOperationException(
                $"Conversion of '{Path.GetFileName(ckptPath)}' failed. stderr: {Truncate(stderr, 600)}");
        }

        try
        {
            File.Move(tmp, onnxOutputPath, overwrite: true);
        }
        catch
        {
            TryDelete(tmp);
            throw;
        }

        _logger.LogInformation("ckpt→onnx done: '{Out}' ({Size:N0} bytes).",
            Path.GetFileName(onnxOutputPath), new FileInfo(onnxOutputPath).Length);
        progress?.Report(new PythonInstallProgress("convert", 1.0, "Conversion complete."));
    }

    private static string ResolveBundledScript()
    {
        // The .csproj copies PythonScripts/*.py to the output directory.
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "PythonScripts", "ckpt_to_onnx.py"),
            Path.Combine(AppContext.BaseDirectory, "ckpt_to_onnx.py"),
        };
        foreach (var c in candidates)
            if (File.Exists(c)) return c;
        return candidates[0];
    }

    private void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch (Exception ex) { _logger.LogDebug(ex, "Could not remove partial conversion output {Path}", path); }
    }

    private static string Truncate(string s, int max) =>
        string.IsNullOrEmpty(s) ? string.Empty : s.Length <= max ? s : s[..max] + "…";
}
