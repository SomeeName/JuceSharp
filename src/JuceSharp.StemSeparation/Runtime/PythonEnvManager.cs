using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace JuceSharp.StemSeparation.Runtime;

/// <summary>
/// Manages a self-contained, downloaded-on-demand embedded Python interpreter
/// living under <c>%LOCALAPPDATA%/JuceSharp/python-embed/</c> (Windows) or
/// <c>~/.local/share/JuceSharp/python-embed/</c> (Linux/macOS). The first
/// time anything in the stem-separation pipeline asks for Python:
///
///   1. <see cref="EnsureInterpreterAsync"/> downloads the
///      <c>python-3.11.x-embed-amd64.zip</c> Windows distribution
///      (~10 MB, pinned by SHA-256) and unzips it. There is no other
///      platform: python.org publishes no embeddable build for Linux or
///      macOS, and an interpreter found on PATH is whatever the PATH says,
///      so elsewhere the bootstrap refuses rather than run one.
///   2. The embedded distribution ships with site-packages disabled by
///      default (the trimmed <c>python311._pth</c> excludes them); we
///      patch that file to re-enable them so <c>pip</c>-installed
///      packages are importable.
///   3. <see cref="EnsurePipAsync"/> downloads ONE pinned pip wheel,
///      checks its SHA-256 and installs pip from it with no index.
///   4. <see cref="EnsurePackagesAsync"/> installs the hash-locked package
///      set (<see cref="PythonPackageLock"/>) with <c>--require-hashes</c>.
///
/// All steps are idempotent and report progress so a UI can show
/// "Downloading Python… / Installing torch…" toasts.
///
/// We intentionally keep this purely subprocess-based instead of in-process
/// (Python.NET / pythonnet) because:
///   - the conversion pipeline runs once per model and takes minutes — a
///     few hundred ms of process start-up is irrelevant,
///   - a torch crash inside the converter must not take down the audio
///     application,
///   - subprocess avoids GIL / threading entanglement with the JUCE
///     real-time thread.
/// Adding a pythonnet-based fast path on top of this manager later is a
/// purely additive change.
/// </summary>
public sealed class PythonEnvManager
{
    /// <summary>Default Python version we bundle. 3.11 is the most-tested
    /// target for both <c>onnx</c> and <c>torch 2.x</c>.</summary>
    public const string DefaultPythonVersion = "3.11.9";

    /// <summary>
    /// SHA-256 of <c>python-3.11.9-embed-amd64.zip</c> as published by
    /// python.org. The archive is downloaded over HTTPS and then EXECUTED,
    /// so TLS alone is not the whole trust story: a compromised or
    /// substituted mirror, a corporate MITM proxy or a truncated transfer
    /// would otherwise be unpacked and run.
    /// <para>
    /// Provenance: computed from
    /// <c>https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip</c>
    /// (10,701,472 bytes) on 2026-09-10 and cross-checked against the MD5
    /// <c>6d9aa08531d48fcc261ba667e2df17c4</c> published on the
    /// python.org 3.11.9 release page — python.org publishes MD5 (not
    /// SHA-256) for release artefacts, so the MD5 is the authoritative
    /// upstream value and this SHA-256 is the strong hash derived from the
    /// byte-identical archive.
    /// </para>
    /// <para><b>Bump this whenever <see cref="DefaultPythonVersion"/> changes.</b></para>
    /// </summary>
    public const string DefaultPythonEmbedSha256 =
        "009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b";

    /// <summary>
    /// Expected SHA-256 of the embedded-Python archive. Defaults to
    /// <see cref="DefaultPythonEmbedSha256"/>. Set to <c>null</c> only when
    /// deliberately pointing at an unpinned build — the download is then
    /// unverified and the manager logs a warning.
    /// </summary>
    public string? PythonEmbedSha256 { get; set; } = DefaultPythonEmbedSha256;

    /// <summary>
    /// The pip wheel the bootstrap installs pip from, as published on PyPI.
    /// <para>
    /// This replaced <c>get-pip.py</c>, which had two holes no pin on the
    /// script could close: the only URL for a supported interpreter is the
    /// rolling <c>bootstrap.pypa.io/pip/get-pip.py</c>, and the script installs
    /// the LATEST pip (and, for 3.11, setuptools and wheel) from PyPI with no
    /// hash, so even a pinned copy ran an unpinned download. A wheel is one
    /// immutable file: it is fetched here, checked against
    /// <see cref="DefaultPipWheelSha256"/>, and pip installs itself from it
    /// with <c>--no-index</c>.
    /// </para>
    /// </summary>
    public const string DefaultPipWheelUrl =
        "https://files.pythonhosted.org/packages/f3/6e/1736e5b4ae2b778ef2f81c47d797de9f891d4d8acb047a24ca37a60294dd/pip-26.2.1-py3-none-any.whl";

    /// <summary>
    /// SHA-256 of <see cref="DefaultPipWheelUrl"/>: the
    /// <c>digests.sha256</c> PyPI's JSON API publishes for
    /// <c>pip-26.2.1-py3-none-any.whl</c> (1,816,632 bytes), re-computed from
    /// the downloaded file on 2026-10-20.
    /// </summary>
    public const string DefaultPipWheelSha256 =
        "71138adf1f4ca900cdb7d289c21b7494329f2332b6d85f0e1c42108c0384ed3e";

    /// <summary>URL of the pip wheel. Must be https (see
    /// <see cref="VerifiedDownload.ValidateSourceUrl"/>) and name a
    /// <c>.whl</c> file; pair a mirror with <see cref="PipWheelSha256"/>.</summary>
    public string PipWheelUrl { get; set; } = DefaultPipWheelUrl;

    /// <summary>Expected SHA-256 of <see cref="PipWheelUrl"/>. Required:
    /// <see cref="EnsurePipAsync"/> refuses to run pip from an unpinned file.</summary>
    public string PipWheelSha256 { get; set; } = DefaultPipWheelSha256;

    private readonly ILogger _logger;

    /// <summary>
    /// Point the manager at an embed root, creating the directory if it is not there, and
    /// sweep any temp files a previous interrupted download left behind. Does not provision
    /// anything — the interpreter and packages are installed lazily on first use.
    /// </summary>
    /// <param name="rootDirectory">Where the embedded interpreter lives; the per-user
    /// default when <c>null</c>. Deleting this directory forces a clean re-install.</param>
    /// <param name="logger">Log sink; a null logger when <c>null</c>.</param>
    public PythonEnvManager(string? rootDirectory = null, ILogger<PythonEnvManager>? logger = null)
    {
        Root = rootDirectory ?? DefaultRoot();
        Directory.CreateDirectory(Root);
        _logger = logger ?? NullLogger<PythonEnvManager>.Instance;
        // Sweep at construction so we don't have to remember to do it
        // before every download. Cheap (single Directory.EnumerateFiles).
        try
        {
            var n = CleanStaleTempFiles();
            if (n > 0) _logger.LogInformation("Removed {Count} stale partial-download files from {Root}.", n, Root);
        }
        catch { /* never let a failed cleanup block construction */ }
    }

    /// <summary>Root directory containing the unpacked interpreter.</summary>
    public string Root { get; }

    /// <summary>Path to <c>python.exe</c> (Windows) or <c>python3</c>
    /// (other platforms) once <see cref="EnsureInterpreterAsync"/> has
    /// completed. Empty until then.</summary>
    public string Executable { get; internal set; } = string.Empty;

    /// <summary>Directory where pip writes packages (Lib/site-packages on
    /// the embedded distribution).</summary>
    public string SitePackages =>
        OperatingSystem.IsWindows()
            ? Path.Combine(Root, "Lib", "site-packages")
            : Path.Combine(Root, "lib", "site-packages");

    /// <summary>
    /// Reports the discovered or bootstrapped state of the embedded
    /// interpreter without performing any network IO.
    /// <para>
    /// This is the UI gate behind "Separate stems" (via
    /// <c>StemSeparator.IsReady()</c>) and is therefore called from the UI
    /// thread. Everything except the interpreter VERSION is a file-system
    /// check; the version needs one <c>python --version</c> child process,
    /// which is <b>cached per interpreter identity</b> (full path + last
    /// write time + length) so only the FIRST probe of a given interpreter
    /// ever spawns anything. Replacing or upgrading the interpreter changes
    /// the identity and re-probes by itself.
    /// </para>
    /// <para>Prefer <see cref="ProbeAsync"/> from async code.</para>
    /// </summary>
    public PythonEnvStatus Probe() => ProbeCore(async: false).GetAwaiter().GetResult();

    /// <summary>
    /// Asynchronous <see cref="Probe"/>. Identical result; never blocks the
    /// calling thread on the one-time <c>python --version</c> child process.
    /// </summary>
    public Task<PythonEnvStatus> ProbeAsync(CancellationToken ct = default) => ProbeCore(async: true, ct);

    private async Task<PythonEnvStatus> ProbeCore(bool async, CancellationToken ct = default)
    {
        var exe = ResolveLocalExecutable();
        if (exe is null) return new PythonEnvStatus(false, false, false, null, null);
        var hasPip   = File.Exists(Path.Combine(Root, OperatingSystem.IsWindows() ? "Scripts" : "bin",
                                                OperatingSystem.IsWindows() ? "pip.exe" : "pip"));
        var hasTorch = Directory.Exists(Path.Combine(SitePackages, "torch"));
        var version  = async
            ? await ReadInterpreterVersionCachedAsync(exe, ct).ConfigureAwait(false)
            : ReadInterpreterVersionCached(exe);
        return new PythonEnvStatus(true, hasPip, hasTorch, exe, version);
    }

    /// <summary>
    /// Downloads (when missing) and unpacks the embedded Python
    /// distribution, then patches its <c>pth</c> file so site-packages are
    /// importable. Reports two-phase progress: download then extract.
    /// </summary>
    public async Task EnsureInterpreterAsync(IProgress<PythonInstallProgress>? progress = null, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        if (!string.IsNullOrEmpty(Executable) && File.Exists(Executable))
        {
            progress?.Report(new PythonInstallProgress("ready", 1.0, "Interpreter already configured."));
            return;
        }
        if (ResolveLocalExecutable() is { } existing)
        {
            Executable = existing;
            progress?.Report(new PythonInstallProgress("ready", 1.0, "Embedded interpreter already present."));
            return;
        }

        using var envLock = await AcquireEnvLockAsync(Root, ct, _logger).ConfigureAwait(false);
        // Re-check under the gate: another process may have finished the
        // whole bootstrap while we queued behind it.
        if (ResolveLocalExecutable() is { } arrived)
        {
            Executable = arrived;
            progress?.Report(new PythonInstallProgress("ready", 1.0, "Embedded interpreter already present."));
            return;
        }

        if (!OperatingSystem.IsWindows())
        {
            // python.org publishes no embeddable build for macOS/Linux, and the
            // package lock is win_amd64 wheels. Running whatever `python3` the
            // PATH resolves to would execute an interpreter nobody pinned.
            throw new PlatformNotSupportedException(
                "The embedded Python interpreter and its hash-locked packages exist for Windows only. " +
                "On this platform the Python sidecar is not available.");
        }

        var version = DefaultPythonVersion;
        var url = $"https://www.python.org/ftp/python/{version}/python-{version}-embed-amd64.zip";
        var zipPath = Path.Combine(Root, $"python-{version}-embed.zip");
        progress?.Report(new PythonInstallProgress("download", 0.0, $"Downloading Python {version}…"));

        var expectedZipHash = PythonEmbedSha256;
        if (string.IsNullOrWhiteSpace(expectedZipHash))
            _logger.LogWarning("PythonEmbedSha256 is not set — the Python {Version} archive will be unpacked and executed UNVERIFIED.", version);

        await DownloadWithProgress(url, zipPath, sub => progress?.Report(
            new PythonInstallProgress("download", sub * 0.85, $"Downloading Python {version}… {sub:P0}")), ct,
            expectedZipHash).ConfigureAwait(false);

        progress?.Report(new PythonInstallProgress("extract", 0.85, "Extracting interpreter…"));
        // Staged extract: unzip into a sibling temp directory, then copy into
        // Root file by file (Root may already hold partial state from an
        // earlier failed run, which a directory rename cannot merge).
        //
        // That copy is NOT atomic — this comment claimed it was — so the
        // all-or-nothing property comes from the ".interpreter-ready" stamp
        // written as the very last step instead: until it exists, no process
        // (this one or a peer that never took the gate) considers the
        // interpreter present. See ResolveLocalExecutable.
        // Delete any stamp left by a previous install first: from here until
        // the stamp is rewritten, this root is explicitly NOT ready.
        try { File.Delete(InterpreterReadyStampPath); }
        catch (Exception ex) { _logger.LogDebug(ex, "Could not clear the ready stamp in {Root}", Root); }
        var stageDir = Path.Combine(Path.GetDirectoryName(Root) ?? Path.GetTempPath(),
            $".{Path.GetFileName(Root)}.extract.{Guid.NewGuid():N}");
        try
        {
            Directory.CreateDirectory(stageDir);
            ZipFile.ExtractToDirectory(zipPath, stageDir, overwriteFiles: true);
            // Move files from stage into Root (Root may already contain
            // partial state from a previous failed run; overwrite each file).
            foreach (var src in Directory.EnumerateFileSystemEntries(stageDir, "*", SearchOption.AllDirectories))
            {
                var rel = Path.GetRelativePath(stageDir, src);
                var dst = Path.Combine(Root, rel);
                if (Directory.Exists(src))
                {
                    Directory.CreateDirectory(dst);
                }
                else
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                    File.Copy(src, dst, overwrite: true);
                }
            }
        }
        finally
        {
            try { if (Directory.Exists(stageDir)) Directory.Delete(stageDir, recursive: true); }
            catch (Exception ex) { _logger.LogDebug(ex, "Could not delete stage dir {Path}", stageDir); }
        }
        try { File.Delete(zipPath); }
        catch (Exception ex) { _logger.LogDebug(ex, "Could not delete temp zip {Path}", zipPath); }

        // Re-enable site-packages by un-commenting `import site` in the .pth
        var pthPath = Directory.EnumerateFiles(Root, "python*._pth").FirstOrDefault();
        if (pthPath != null)
        {
            var lines = await File.ReadAllLinesAsync(pthPath, ct).ConfigureAwait(false);
            for (int i = 0; i < lines.Length; i++)
                if (lines[i].TrimStart().Equals("#import site", StringComparison.OrdinalIgnoreCase))
                    lines[i] = "import site";
            // Belt-and-braces: ensure the line exists once.
            if (!lines.Any(l => l.Trim().Equals("import site", StringComparison.OrdinalIgnoreCase)))
                lines = lines.Concat(new[] { "import site" }).ToArray();
            await File.WriteAllLinesAsync(pthPath, lines, ct).ConfigureAwait(false);
        }

        Executable = Path.Combine(Root, "python.exe");
        if (!File.Exists(Executable))
            throw new InvalidOperationException($"Extraction succeeded but '{Executable}' is missing.");
        // LAST: everything the interpreter needs is now on disk and patched.
        await File.WriteAllTextAsync(InterpreterReadyStampPath, version, ct).ConfigureAwait(false);
        progress?.Report(new PythonInstallProgress("ready", 1.0, $"Embedded Python {version} ready."));
        _logger.LogInformation("Embedded Python {Version} installed at {Root}.", version, Root);
    }

    /// <summary>Downloads the pinned pip wheel, verifies it, and installs pip
    /// from it with no package index. Idempotent.</summary>
    /// <exception cref="ArgumentException"><see cref="PipWheelUrl"/> is not https
    ///   or does not name a <c>.whl</c> file.</exception>
    /// <exception cref="InvalidOperationException"><see cref="PipWheelSha256"/>
    ///   is not a SHA-256, or the install failed.</exception>
    /// <exception cref="InvalidDataException">The download did not match
    ///   <see cref="PipWheelSha256"/>; nothing is executed.</exception>
    public async Task EnsurePipAsync(IProgress<PythonInstallProgress>? progress = null, CancellationToken ct = default)
    {
        // Policy first, before any process or transfer: a bad URL or an
        // unpinned wheel is a configuration error whatever state the root is in.
        var wheelName = ValidatePipWheelSource(PipWheelUrl, PipWheelSha256);

        if (string.IsNullOrEmpty(Executable))
            await EnsureInterpreterAsync(progress, ct).ConfigureAwait(false);

        var (pipOk, _, _) = await Run(Executable, "-m pip --version", timeout: TimeSpan.FromSeconds(20), ct).ConfigureAwait(false);
        if (pipOk) { progress?.Report(new PythonInstallProgress("pip-ready", 1.0, "pip already installed.")); return; }

        using var envLock = await AcquireEnvLockAsync(Root, ct, _logger).ConfigureAwait(false);
        // Re-check under the gate — a peer may have bootstrapped pip while
        // we queued.
        var (pipOkNow, _, _) = await Run(Executable, "-m pip --version", timeout: TimeSpan.FromSeconds(20), ct).ConfigureAwait(false);
        if (pipOkNow) { progress?.Report(new PythonInstallProgress("pip-ready", 1.0, "pip already installed.")); return; }

        var wheel = Path.Combine(Root, wheelName);
        progress?.Report(new PythonInstallProgress("pip-download", 0.0, $"Downloading {wheelName}…"));
        await DownloadWithProgress(PipWheelUrl, wheel, _ => { }, ct, PipWheelSha256.Trim()).ConfigureAwait(false);
        progress?.Report(new PythonInstallProgress("pip-install", 0.5, "Bootstrapping pip…"));

        var (ok, stdout, stderr) = await Run(Executable, BuildPipBootstrapArguments(wheel),
            timeout: TimeSpan.FromMinutes(5), ct).ConfigureAwait(false);
        try { File.Delete(wheel); }
        catch (Exception ex) { _logger.LogDebug(ex, "Could not delete the pip wheel at {Path}", wheel); }
        if (!ok)
            throw new InvalidOperationException($"The pip bootstrap failed.\nstdout: {stdout}\nstderr: {stderr}");
        progress?.Report(new PythonInstallProgress("pip-ready", 1.0, "pip installed."));
    }

    /// <summary>
    /// The transport and pin rule for the pip wheel: https (loopback http for
    /// tests, as everywhere), a <c>.whl</c> file name pip will accept, and a
    /// SHA-256. Returns the wheel's file name.
    /// </summary>
    internal static string ValidatePipWheelSource(string? url, string? sha256)
    {
        VerifiedDownload.ValidateSourceUrl(url, "pip wheel", nameof(PipWheelUrl));
        var name = Path.GetFileName(new Uri(url!).AbsolutePath);
        if (string.IsNullOrEmpty(name) || !name.EndsWith(".whl", StringComparison.OrdinalIgnoreCase)
            || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw new ArgumentException($"'{url}' does not name a pip wheel (*.whl).", nameof(PipWheelUrl));
        var hash = sha256?.Trim() ?? string.Empty;
        if (hash.Length != 64 || !hash.All(Uri.IsHexDigit))
            throw new InvalidOperationException(
                "PipWheelSha256 must be the 64-hex-digit SHA-256 of the pip wheel. The bootstrap never " +
                "runs an unpinned pip.");
        return name;
    }

    /// <summary>
    /// The argv that installs pip from its own verified wheel. pip runs from
    /// the wheel on <c>sys.path</c> (the way <c>get-pip.py</c> runs its bundled
    /// copy) because pip refuses to modify itself when started as
    /// <c>&lt;wheel&gt;/pip</c> on Windows. <c>--no-index --no-deps</c>: nothing
    /// but the verified file is installed. The wheel path is its own argument,
    /// never spliced into the code string.
    /// </summary>
    internal static IReadOnlyList<string> BuildPipBootstrapArguments(string wheel) =>
    [
        "-c",
        "import sys; w = sys.argv[1]; sys.path.insert(0, w); " +
        "from pip._internal.cli.main import main; " +
        "sys.exit(main(['install', '--isolated', '--no-index', '--no-deps', " +
        "'--no-warn-script-location', '--disable-pip-version-check', w]))",
        wheel,
    ];

    /// <summary>
    /// Installs the hash-locked package set (<see cref="PythonPackageLock"/>)
    /// that covers <paramref name="packages"/>, via
    /// <c>python -m pip install --require-hashes --no-deps -r &lt;lock&gt;</c>.
    /// <para>
    /// <b>Only pinned packages.</b> Every spec must be a
    /// <see cref="PythonPackagePins"/> value: the lock was generated from those
    /// and holds the hash of every wheel in their dependency closure, so pip
    /// installs exactly those bytes or fails. A spec outside the lock has no
    /// hash to install it by and is refused. The lock is Windows-only; any
    /// other platform gets a <see cref="PlatformNotSupportedException"/>.
    /// </para>
    /// <para>
    /// <b>The whole set is installed at most once per root.</b> Success is
    /// recorded as a stamp file whose name carries the SHA-256 of the exact
    /// package set (see <see cref="PackageSetStampFileName"/>), so a second
    /// call with the same pins spawns NO process at all — not even pip's own
    /// "already satisfied" resolve, which costs seconds and a network round
    /// trip on every separation. Changing any pin changes the hash and
    /// re-installs by itself.
    /// </para>
    /// <para>
    /// (Before this stamp existed, every single separation shelled out to
    /// <c>pip install torch … demucs …</c> unless the caller passed
    /// <c>SkipPackageBootstrap</c> — the doc-comment claimed it "skips ones
    /// already importable", which pip does not do for you.)
    /// </para>
    /// </summary>
    public async Task EnsurePackagesAsync(
        IEnumerable<string> packages,
        IProgress<PythonInstallProgress>? progress = null,
        CancellationToken ct = default)
    {
        var list = packages.Where(p => !string.IsNullOrWhiteSpace(p)).Select(p => p.Trim()).ToArray();
        if (list.Length == 0) return;

        var unlocked = list.Where(p => !PythonPackageLock.CoveredPins.Contains(p)).ToArray();
        if (unlocked.Length > 0)
            throw new InvalidOperationException(
                $"[{string.Join(", ", unlocked)}] is not in the hash-locked package set, so there is no hash " +
                "to install it by. Only PythonPackagePins values can be installed.");
        if (!PythonPackageLock.CoversThisPlatform)
            throw new PlatformNotSupportedException(
                "The hash-locked package set covers Windows x64 only; packages are not installed on this platform.");

        var stamp = PackageSetStampPath(list);
        if (File.Exists(stamp) && StampStillDescribesTheEnvironment(stamp, list))
        {
            progress?.Report(new PythonInstallProgress("pip-installed", 1.0, "Packages already installed."));
            return;
        }
        DiscardStampIfStale(stamp, list);

        // NOTE: EnsurePipAsync takes the bootstrap gate itself, so it must be
        // awaited BEFORE we take it here — the gate is not reentrant.
        await EnsurePipAsync(progress, ct).ConfigureAwait(false);

        using var envLock = await AcquireEnvLockAsync(Root, ct, _logger).ConfigureAwait(false);
        if (File.Exists(stamp) && StampStillDescribesTheEnvironment(stamp, list))
        {
            progress?.Report(new PythonInstallProgress("pip-installed", 1.0, "Packages already installed."));
            return;
        }

        progress?.Report(new PythonInstallProgress("pip-install", 0.0, $"Installing: {string.Join(", ", list)}"));
        // Written under the gate, every time: a lock file a peer or a user left
        // in the root is never what pip reads.
        var lockPath = Path.Combine(Root, $"requirements-{PythonPackageLock.Digest}.lock");
        await File.WriteAllTextAsync(lockPath, PythonPackageLock.Text, new UTF8Encoding(false), ct).ConfigureAwait(false);
        var (ok, stdout, stderr) = await Run(Executable, BuildPipInstallArguments(lockPath), timeout: TimeSpan.FromHours(1), ct).ConfigureAwait(false);
        if (!ok)
            throw new InvalidOperationException(
                $"pip install failed for [{string.Join(", ", list)}].\nstdout: {Truncate(stdout, 800)}\nstderr: {Truncate(stderr, 800)}");
        try
        {
            Directory.CreateDirectory(Root);
            // The interpreter identity goes in FIRST: a stamp that only records
            // the pin set says nothing about the tree those pins were installed
            // into, and a replaced interpreter is a different tree.
            await File.WriteAllTextAsync(stamp,
                string.Join(Environment.NewLine,
                    new[] { InterpreterStampLine + CurrentInterpreterIdentity() }.Concat(list)),
                ct).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            // A missing stamp costs a redundant pip run next time; it is
            // never a correctness problem, so it must not fail the install.
            _logger.LogDebug(ex, "Could not write package stamp {Path}", stamp);
        }
        progress?.Report(new PythonInstallProgress("pip-installed", 1.0, "Packages installed."));
        _logger.LogInformation("pip installed: {Packages}", string.Join(", ", list));
    }

    /// <summary>
    /// The exact argv handed to <c>python -m pip install</c>, one element per
    /// argument, for <see cref="ProcessStartInfo.ArgumentList"/>.
    /// <para>
    /// <c>--require-hashes</c> makes every wheel prove its SHA-256;
    /// <c>--no-deps</c> means nothing outside the lock is resolved;
    /// <c>--only-binary :all:</c> means no sdist build script ever runs. The
    /// lock path is passed VERBATIM — a path with spaces, a quote or a trailing
    /// backslash is escaped by the runtime, never by us.
    /// </para>
    /// </summary>
    internal static IReadOnlyList<string> BuildPipInstallArguments(string lockPath) =>
    [
        "-m", "pip", "install", "--isolated", "--no-warn-script-location", "--disable-pip-version-check",
        "--require-hashes", "--no-deps", "--only-binary", ":all:",
        "-r", lockPath,
    ];

    // --- Bootstrap serialization (cross-process) -----------------------
    //
    // Everything under Root is shared mutable state: one interpreter, one
    // site-packages tree, one pip. Two bootstraps running at once — two
    // StemSeparator instances, a StemSeparator plus a CkptToOnnxConverter
    // (each of which DEFAULT-CONSTRUCTS its own manager on the same default
    // Root), or the juce-stems-diag CLI running beside the host app — mean
    // two `pip install torch` processes unpacking multi-GB wheels into one
    // site-packages directory, which is how you get a half-written torch
    // and an OSError on the next import.
    //
    // The gate is therefore CROSS-PROCESS (an exclusive handle on a lock file
    // in the root, see EnvLockFileName), not just a static lock. A file
    // handle, unlike a Mutex, is not thread-affine and can be held across an
    // await, and unlike a named kernel object it is neither nameable by
    // another user nor left held by a dead owner.
    // Refcounted, like VerifiedDownloader.DestinationGates: an entry lives
    // only while someone holds or waits on it. A static table keyed by a root
    // path and never pruned is a leak for any host that bootstraps into
    // per-project roots (W4-S18, second half).
    private static readonly Dictionary<string, LocalGateEntry> LocalEnvGates =
        new(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);

    private static readonly object LocalGatesLock = new();

    private sealed class LocalGateEntry
    {
        internal SemaphoreSlim Semaphore { get; } = new(1, 1);
        internal int Refs { get; set; }
    }

    /// <summary>Live process-local gate count. Tests assert this returns to zero.</summary>
    internal static int LocalEnvGateCount
    {
        get { lock (LocalGatesLock) return LocalEnvGates.Count; }
    }

    private static LocalGateEntry ReferenceLocalGate(string name)
    {
        lock (LocalGatesLock)
        {
            if (!LocalEnvGates.TryGetValue(name, out var entry))
                LocalEnvGates[name] = entry = new LocalGateEntry();
            entry.Refs++;
            return entry;
        }
    }

    private static void UnreferenceLocalGate(string name)
    {
        lock (LocalGatesLock)
        {
            if (!LocalEnvGates.TryGetValue(name, out var e)) return;
            if (--e.Refs <= 0) LocalEnvGates.Remove(name);
        }
    }

    /// <summary>
    /// Upper bound on the wait for the CROSS-PROCESS half of the gate.
    ///
    /// <para>
    /// The cross-process half is an exclusive handle on a lock file, which the
    /// OS drops when its owner dies — a crashed or killed bootstrap does not
    /// wedge the next one. The bound is for a LIVE owner that is stuck (a
    /// hung <c>pip install</c>): on expiry we log at Warning and proceed with the
    /// process-local gate alone — degraded, loudly, rather than hung.
    /// </para>
    /// <para>Settable for tests only.</para>
    /// </summary>
    internal static TimeSpan CrossProcessEnvLockTimeout { get; set; } = TimeSpan.FromMinutes(30);

    /// <summary>
    /// Name of the lock file, inside <see cref="Root"/>, whose exclusive handle
    /// is the cross-process half of the bootstrap gate.
    ///
    /// <para>
    /// It replaced a named kernel semaphore (<c>Global\JuceSharp.python-embed.&lt;hash&gt;</c>).
    /// That name was derived from the root path alone, so any process of any
    /// user could compute it, create the object first and hold it — every
    /// bootstrap then waited out the timeout — and a semaphore is not released
    /// when its owner dies. A file in the root carries the root's own ACL (by
    /// default the user's profile), is per-root by construction, and its handle
    /// is released by the OS on process exit.
    /// </para>
    /// </summary>
    internal const string EnvLockFileName = ".bootstrap.lock";

    /// <summary>Key of the PROCESS-LOCAL half of the gate for
    /// <paramref name="root"/>: the normalised full path.</summary>
    internal static string EnvLockName(string root)
    {
        var full = Path.GetFullPath(root);
        return OperatingSystem.IsWindows() ? full.ToUpperInvariant() : full;
    }

    /// <summary>
    /// Acquires the cross-process bootstrap gate for <paramref name="root"/>.
    /// Dispose the returned handle to release it. Exposed to tests so the
    /// serialization can be proven without running pip.
    /// </summary>
    internal static async Task<IDisposable> AcquireEnvLockAsync(
        string root, CancellationToken ct, ILogger? logger = null)
    {
        var name = EnvLockName(root);
        var entry = ReferenceLocalGate(name);
        var local = entry.Semaphore;
        try
        {
            await local.WaitAsync(ct).ConfigureAwait(false);
        }
        catch
        {
            // Cancelled while queued: drop the reference, or the entry outlives
            // every user of it.
            UnreferenceLocalGate(name);
            throw;
        }
        FileStream? held = null;
        try
        {
            Directory.CreateDirectory(root);
            var lockPath = Path.Combine(root, EnvLockFileName);
            // Poll rather than block a pool thread: a bootstrap holding this
            // gate can legitimately run for the length of a torch download.
            // Bounded — see CrossProcessEnvLockTimeout.
            var deadline = Environment.TickCount64 + (long)CrossProcessEnvLockTimeout.TotalMilliseconds;
            while ((held = TryOpenLockFile(lockPath)) is null)
            {
                if (Environment.TickCount64 >= deadline)
                {
                    (logger ?? NullLogger.Instance).LogWarning(
                        "The cross-process Python bootstrap gate for {Root} was not released within {Timeout}. " +
                        "Another process holds {LockFile} and appears stuck. Proceeding with process-local " +
                        "serialization only.", root, CrossProcessEnvLockTimeout, EnvLockFileName);
                    break;
                }
                await Task.Delay(250, ct).ConfigureAwait(false);
            }
        }
        catch
        {
            held?.Dispose();
            local.Release();
            UnreferenceLocalGate(name);
            throw;
        }
        return new EnvLockHandle(name, local, held);
    }

    /// <summary>The exclusive handle, or null while another process holds it.
    /// Any other failure (no access to the root) is an exception: proceeding
    /// unserialized must be a logged decision, not a silent one.</summary>
    private static FileStream? TryOpenLockFile(string path)
    {
        try
        {
            return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None,
                bufferSize: 1, FileOptions.None);
        }
        catch (IOException) { return null; }
    }

    private sealed class EnvLockHandle(string name, SemaphoreSlim local, FileStream? crossProcess) : IDisposable
    {
        private int _disposed;
        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            crossProcess?.Dispose();
            local.Release();
            UnreferenceLocalGate(name);
        }
    }

    // --- Package-set stamp --------------------------------------------

    /// <summary>
    /// Name of the marker file recording that this exact pinned package set
    /// has already been installed into <see cref="Root"/>. Keyed on the
    /// SHA-256 of the set, so changing, adding or removing a pin invalidates
    /// it automatically — there is no version number to forget to bump.
    /// </summary>
    internal static string PackageSetStampFileName(IEnumerable<string> packages) =>
        PackageSetStampFileNameForLock(packages, PythonPackageLock.Digest);

    /// <summary>The stamp name for a pin set installed from the lock whose
    /// digest is <paramref name="lockDigest"/>: regenerating the lock (new
    /// versions, new hashes) re-runs pip exactly as changing a pin does.</summary>
    internal static string PackageSetStampFileNameForLock(IEnumerable<string> packages, string lockDigest)
    {
        var canonical = "lock=" + lockDigest + "\n" + string.Join('\n', packages
            .Where(p => !string.IsNullOrWhiteSpace(p))
            .Select(p => p.Trim()));
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(canonical))).ToLowerInvariant();
        return $".packages-{hash[..32]}.stamp";
    }

    private string PackageSetStampPath(IEnumerable<string> packages) =>
        Path.Combine(Root, PackageSetStampFileName(packages));

    /// <summary>First line of a stamp written since 0.9: the interpreter the
    /// packages were installed into.</summary>
    internal const string InterpreterStampLine = "interpreter=";

    private string CurrentInterpreterIdentity()
    {
        var exe = ResolveLocalExecutable();
        return exe is null ? string.Empty : VersionCacheKey(exe);
    }

    /// <summary>
    /// Whether the stamp is still TRUE, not merely present.
    ///
    /// <para>
    /// The stamp records a pin SET; it records nothing about the tree those
    /// pins were installed into. A user who deletes
    /// <c>Lib\site-packages\torch</c>, a failed uninstall or an antivirus
    /// quarantine leaves the stamp behind, so pip is never run again and every
    /// separation fails with a Python <c>ImportError</c> for ever — while
    /// <c>Probe().HasTorch</c> knew the answer the whole time.
    /// </para>
    /// <para>
    /// Two cheap file-system checks, no child process: the interpreter is
    /// present and is the one recorded (older stamps record none, and are
    /// accepted on that point), and every pin whose installed directory this
    /// repo knows — <see cref="PythonPackagePins.InstalledDirectories"/> — has
    /// that directory in site-packages. A pin the map does not know is NOT
    /// checked: guessing a distribution's import name wrong would re-run pip on
    /// every launch, which is the failure the stamp exists to prevent.
    /// </para>
    /// </summary>
    private bool StampStillDescribesTheEnvironment(string stampPath, IReadOnlyList<string> packages)
    {
        try
        {
            var exe = ResolveLocalExecutable();
            if (exe is null) return false;

            var recorded = File.ReadLines(stampPath)
                .FirstOrDefault(l => l.StartsWith(InterpreterStampLine, StringComparison.Ordinal));
            if (recorded is not null)
            {
                var identity = recorded[InterpreterStampLine.Length..];
                if (identity.Length > 0 &&
                    !string.Equals(identity, VersionCacheKey(exe), StringComparison.Ordinal))
                    return false;
            }

            foreach (var spec in packages)
            {
                if (!PythonPackagePins.InstalledDirectories.TryGetValue(spec.Trim(), out var dir)) continue;
                if (!Directory.Exists(Path.Combine(SitePackages, dir))) return false;
            }
            return true;
        }
        catch (IOException) { return true; }
        catch (UnauthorizedAccessException) { return true; }
    }

    /// <summary>A stamp that no longer describes the tree is deleted, so the
    /// reinstall below is not repeated on every launch after it succeeds.</summary>
    private void DiscardStampIfStale(string stampPath, IReadOnlyList<string> packages)
    {
        if (!File.Exists(stampPath) || StampStillDescribesTheEnvironment(stampPath, packages)) return;
        _logger.LogWarning(
            "The package stamp {Stamp} no longer matches the environment (a package directory or the " +
            "interpreter is missing). Reinstalling.", Path.GetFileName(stampPath));
        try { File.Delete(stampPath); }
        catch (Exception ex) { _logger.LogDebug(ex, "Could not delete the stale package stamp {Path}", stampPath); }
    }

    private static string DefaultRoot()
    {
        var baseDir = OperatingSystem.IsWindows()
            ? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData)
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share");
        return Path.Combine(baseDir, "JuceSharp", "python-embed");
    }

    /// <summary>
    /// Marker written as the LAST step of a successful extract. Its absence is
    /// what makes a half-extracted root invisible to
    /// <see cref="ResolveLocalExecutable"/> — see the remarks there.
    /// </summary>
    internal const string InterpreterReadyStampName = ".interpreter-ready";

    private string InterpreterReadyStampPath => Path.Combine(Root, InterpreterReadyStampName);

    /// <summary>
    /// The interpreter this root offers, or null.
    ///
    /// <para>
    /// <b>"python.exe exists" is NOT "the interpreter is usable".</b> The
    /// extract copies a staging directory into <see cref="Root"/> file by file
    /// and then patches <c>python311._pth</c>; the pre-lock fast path in
    /// <see cref="EnsureInterpreterAsync"/> calls this method BEFORE taking the
    /// cross-process gate, so process B could see <c>python.exe</c> after A had
    /// copied it and before <c>python311.zip</c>, <c>python3.dll</c> and the
    /// <c>._pth</c> patch landed — and then run pip against a half-extracted
    /// runtime, which is exactly the failure the gate exists to prevent,
    /// arriving through the one door the gate does not cover.
    /// </para>
    /// <para>
    /// A root installed before the stamp existed is complete but unstamped;
    /// it is recognised by the two things the extract lands last (a patched
    /// <c>._pth</c> and the stdlib zip) and adopted rather than re-extracted.
    /// </para>
    /// </summary>
    private string? ResolveLocalExecutable()
    {
        if (OperatingSystem.IsWindows())
        {
            var p = Path.Combine(Root, "python.exe");
            if (!File.Exists(p)) return null;
            if (File.Exists(InterpreterReadyStampPath)) return p;
            if (!LooksLikeACompleteLegacyExtract()) return null;
            try { File.WriteAllText(InterpreterReadyStampPath, DefaultPythonVersion); }
            catch (Exception ex) { _logger.LogDebug(ex, "Could not stamp legacy interpreter root {Root}", Root); }
            return p;
        }
        // There is no embedded interpreter off Windows, and the PATH is never
        // searched for one: whichever `python3` comes first on it is not an
        // interpreter anybody pinned. Only an Executable assigned explicitly
        // (by a test) counts.
        if (!string.IsNullOrEmpty(Executable) && File.Exists(Executable)) return Executable;
        return null;
    }

    /// <summary>
    /// A root extracted by a build that predates
    /// <see cref="InterpreterReadyStampName"/>: the stdlib zip is present and
    /// the <c>._pth</c> has already been patched, which are the last two things
    /// a successful extract produces.
    /// </summary>
    private bool LooksLikeACompleteLegacyExtract()
    {
        try
        {
            if (!Directory.EnumerateFiles(Root, "python*.zip", SearchOption.TopDirectoryOnly).Any())
                return false;
            var pth = Directory.EnumerateFiles(Root, "python*._pth", SearchOption.TopDirectoryOnly).FirstOrDefault();
            return pth is not null
                   && File.ReadLines(pth).Any(l => l.Trim().Equals("import site", StringComparison.OrdinalIgnoreCase));
        }
        catch (IOException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
    }

    // --- Interpreter version cache ------------------------------------
    //
    // Keyed on interpreter IDENTITY (path + last write time + length) so a
    // replaced/upgraded interpreter re-probes without any explicit
    // invalidation, while repeated Probe() calls — which the UI makes on
    // every menu open — cost zero processes.
    private static readonly ConcurrentDictionary<string, string?> VersionCache = new(StringComparer.Ordinal);

    /// <summary>Test hook: number of <c>python --version</c> child processes
    /// this process has actually launched. A cache hit does not increment it.</summary>
    internal static int VersionProbeSpawnCount;

    /// <summary>Test hook: forget every cached interpreter version.</summary>
    internal static void ClearVersionCacheForTests()
    {
        VersionCache.Clear();
        Interlocked.Exchange(ref VersionProbeSpawnCount, 0);
    }

    private static string VersionCacheKey(string exe)
    {
        try
        {
            var fi = new FileInfo(exe);
            return $"{fi.FullName}|{fi.LastWriteTimeUtc.Ticks}|{fi.Length}";
        }
        catch
        {
            return exe;
        }
    }

    private static string? ReadInterpreterVersionCached(string exe)
    {
        var key = VersionCacheKey(exe);
        if (VersionCache.TryGetValue(key, out var cached)) return cached;
        // Wrap in Task.Run so even if a caller invokes us from a UI thread
        // with a SynchronizationContext we don't deadlock on .GetResult.
        var value = Task.Run(() => TryReadInterpreterVersionAsync(exe, default)).GetAwaiter().GetResult();
        VersionCache[key] = value;
        return value;
    }

    private static async Task<string?> ReadInterpreterVersionCachedAsync(string exe, CancellationToken ct)
    {
        var key = VersionCacheKey(exe);
        if (VersionCache.TryGetValue(key, out var cached)) return cached;
        var value = await TryReadInterpreterVersionAsync(exe, ct).ConfigureAwait(false);
        VersionCache[key] = value;
        return value;
    }

    private static async Task<string?> TryReadInterpreterVersionAsync(string exe, CancellationToken ct)
    {
        try
        {
            Interlocked.Increment(ref VersionProbeSpawnCount);
            var (ok, stdout, _) = await Run(exe, "--version", timeout: TimeSpan.FromSeconds(5), ct).ConfigureAwait(false);
            return ok ? stdout.Trim() : null;
        }
        catch (OperationCanceledException) { throw; }
        catch { return null; }
    }

    /// <summary>
    /// Resilient download with progress: writes to <c>{dest}.tmp.{guid}</c>
    /// first, fsyncs, then atomically renames into place. If the transfer
    /// is cancelled or the connection drops, the temp file is removed so
    /// the next boot does not see a half-formed payload.
    /// <para>
    /// Returns the number of bytes written. Throws on network failure or
    /// cancellation; the destination either exists in full or not at all.
    /// </para>
    /// </summary>
    /// <param name="url">Source URL.</param>
    /// <param name="dest">Destination path; only created on success.</param>
    /// <param name="progress">Fractional progress callback (0..1).</param>
    /// <param name="ct">Cancellation token.</param>
    /// <param name="expectedSha256">
    /// When non-null, the completed transfer is hashed BEFORE it is renamed
    /// into place; a mismatch deletes the temp file and throws
    /// <see cref="InvalidDataException"/>, so <paramref name="dest"/> is
    /// never created from unverified bytes.
    /// </param>
    internal static Task<long> DownloadWithProgress(string url, string dest, Action<double> progress, CancellationToken ct,
        string? expectedSha256 = null)
        => VerifiedDownloader.DownloadAsync(url, dest, p => { if (p.Fraction >= 0) progress(p.Fraction); },
            ct, expectedSha256, allowResume: false);

    /// <summary>
    /// Hashes <paramref name="path"/> and compares it with
    /// <paramref name="expectedSha256"/> (hex, case-insensitive). A mismatch
    /// DELETES the file and throws — a payload that fails its pin must not
    /// be left on disk where a later run could pick it up as a resumed
    /// download. A null expectation is a no-op (the caller logs that).
    /// </summary>
    internal static Task VerifyFileHashAsync(string path, string? expectedSha256, string source, CancellationToken ct)
        => VerifiedDownloader.VerifyFileHashAsync(path, expectedSha256, source, ct);

    /// <summary>
    /// Sweeps the embed root for orphan <c>*.tmp.*</c> partial-download
    /// files left behind by a previous crashed/cancelled boot. Returns
    /// the number of files removed. Always safe to call.
    ///
    /// <para>
    /// <b>This covers the NON-resumable shape only.</b> A resumable download
    /// writes <c>{dest}.{urlhash}.part</c> into the MODEL directory and keeps it
    /// deliberately on cancellation and on network failure;
    /// <see cref="VerifiedDownload.CleanPartials"/> is that sweeper, and it uses
    /// the same liveness probe as this one.
    /// </para>
    ///
    /// <para>
    /// <b>An IN-FLIGHT download is not stale.</b> This runs from the
    /// constructor, and two managers on one root is a configuration this class
    /// documents as normal (a <c>StemSeparator</c> plus a
    /// <c>CkptToOnnxConverter</c>, or the diag CLI beside the host app), so an
    /// unconditional delete of every <c>*.tmp.*</c> was a second manager
    /// deleting the first manager's half-written interpreter archive out from
    /// under it. <c>VerifiedDownloader</c> holds its temp file
    /// <see cref="FileShare.None"/>, so "can I open this exclusively?" is an
    /// exact test for "is somebody writing it?" — no clock, no age heuristic.
    /// </para>
    /// </summary>
    public int CleanStaleTempFiles()
    {
        if (!Directory.Exists(Root)) return 0;
        var removed = 0;
        foreach (var f in Directory.EnumerateFiles(Root, "*.tmp.*", SearchOption.TopDirectoryOnly))
        {
            try
            {
                // Opening it exclusively proves nobody is downloading into it.
                using (new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.None)) { }
            }
            catch (IOException)
            {
                _logger.LogDebug("Leaving {Path} alone — another download is writing it.", f);
                continue;
            }
            catch (UnauthorizedAccessException) { continue; }

            try { File.Delete(f); removed++; } catch { /* best effort */ }
        }
        return removed;
    }

    internal static async Task<(bool ok, string stdout, string stderr)> Run(string exe, string args, TimeSpan timeout, CancellationToken ct)
        => await RunStreaming(exe, args, timeout, onLine: null, ct).ConfigureAwait(false);

    internal static async Task<(bool ok, string stdout, string stderr)> Run(string exe, IEnumerable<string> argumentList, TimeSpan timeout, CancellationToken ct)
        => await RunStreaming(exe, argumentList, timeout, onLine: null, ct).ConfigureAwait(false);

    /// <summary>
    /// Process runner with an optional per-line callback over stdout. Lines
    /// matching the convention <c>PROGRESS &lt;0..1&gt; [text]</c> are still
    /// surfaced as plain text — interpretation is up to the caller. All
    /// stdout/stderr is captured and returned in full.
    /// </summary>
    /// <summary>
    /// Same as <see cref="RunStreaming(string,string,TimeSpan,Action{string}?,CancellationToken)"/>
    /// but uses <see cref="ProcessStartInfo.ArgumentList"/> to avoid quoting
    /// bugs when arguments contain spaces, quotes, backslashes, or other
    /// characters that the legacy <c>Arguments</c> string would mis-parse.
    /// </summary>
    internal static Task<(bool ok, string stdout, string stderr)> RunStreaming(
        string exe, IEnumerable<string> argumentList, TimeSpan timeout, Action<string>? onLine, CancellationToken ct)
        => RunStreamingCore(exe, argumentString: null, argumentList: argumentList, timeout, onLine, ct);

    internal static Task<(bool ok, string stdout, string stderr)> RunStreaming(
        string exe, string args, TimeSpan timeout, Action<string>? onLine, CancellationToken ct)
        => RunStreamingCore(exe, argumentString: args, argumentList: null, timeout, onLine, ct);

    private static async Task<(bool ok, string stdout, string stderr)> RunStreamingCore(
        string exe, string? argumentString, IEnumerable<string>? argumentList,
        TimeSpan timeout, Action<string>? onLine, CancellationToken ct)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                RedirectStandardOutput = true, RedirectStandardError = true,
                UseShellExecute = false, CreateNoWindow = true,
                WorkingDirectory = Path.GetDirectoryName(exe) ?? string.Empty,
                // UTF-8 on BOTH sides of the pipe, and it has to be both.
                // CPython writing to a PIPE on Windows encodes with the locale
                // ANSI codepage and .NET decodes with Console.OutputEncoding,
                // which for a GUI host is also the ANSI codepage and for the diag
                // CLI is whatever the console is set to — neither is UTF-8 and
                // the two rarely agree. `STEM <name> <path>` is parsed straight
                // out of that string, so a library under C:\Users\Müller came
                // back as a path that does not exist, reported as a SUCCESSFUL
                // separation. Same fault as the LPStr-vs-LPUTF8Str one on the
                // native ABI, at the process boundary instead.
                StandardOutputEncoding = new UTF8Encoding(false),
                StandardErrorEncoding = new UTF8Encoding(false),
            };
            if (argumentList is not null)
            {
                foreach (var a in argumentList) psi.ArgumentList.Add(a);
            }
            else if (argumentString is not null)
            {
                psi.Arguments = argumentString;
            }
            // Host-isolation: even though we always invoke the embedded
            // interpreter explicitly (Root/python.exe), guard against
            // accidental contamination from a globally-installed Python on
            // the user's machine. PYTHONNOUSERSITE skips ~/.local /
            // %APPDATA%/Python user-site, PIP_USER=0 forces pip to ignore
            // any `pip.conf` user-install default, and PYTHONDONTWRITEBYTECODE
            // keeps the embed root free of host-process .pyc residue.
            psi.EnvironmentVariables["PYTHONNOUSERSITE"] = "1";
            psi.EnvironmentVariables["PIP_USER"] = "0";
            psi.EnvironmentVariables["PIP_REQUIRE_VIRTUALENV"] = "0";
            psi.EnvironmentVariables["PYTHONDONTWRITEBYTECODE"] = "1";
            // The child half of the encoding contract above: PYTHONIOENCODING
            // fixes the three std streams, PYTHONUTF8 puts the whole interpreter
            // (filesystem encoding included) in UTF-8 mode, so a non-ASCII path
            // survives BOTH the open and the print.
            psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
            psi.EnvironmentVariables["PYTHONUTF8"] = "1";
            using var p = Process.Start(psi);
            if (p is null) return (false, string.Empty, "Process.Start returned null.");
            ChildProcessRegistry.Register(p);

            var sb = new StringBuilder();
            var stdout = p.StandardOutput;
            var soTask = Task.Run(async () =>
            {
                string? line;
                while ((line = await stdout.ReadLineAsync().ConfigureAwait(false)) != null)
                {
                    sb.AppendLine(line);
                    try { onLine?.Invoke(line); }
                    catch (Exception ex) { Debug.WriteLine($"PythonEnvManager.Run: onLine handler threw: {ex.Message}"); }
                }
            }, ct);
            var seTask = p.StandardError.ReadToEndAsync();

            using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            cts.CancelAfter(timeout);
            try
            {
                await p.WaitForExitAsync(cts.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                try { p.Kill(true); }
                catch (Exception ex) { Debug.WriteLine($"PythonEnvManager.Run: Kill failed: {ex.Message}"); }
                try { await soTask.ConfigureAwait(false); }
                catch (Exception ex) { Debug.WriteLine($"PythonEnvManager.Run: stdout task failed after cancellation: {ex.Message}"); }
                // A user CANCEL is not a failure of the thing being run. It
                // used to come back as ok=false with stderr "Cancelled.",
                // which every caller then reported as its own failure:
                // EnsurePipAsync / EnsurePackagesAsync / ConvertAsync all
                // threw InvalidOperationException("… failed … stderr:
                // Cancelled."), so pressing Cancel during the first-run torch
                // install surfaced to the user as a pip error. Only three of
                // the four call sites special-cased it. Throw instead — the
                // child is already killed.
                //
                // A TIMEOUT is still a failure of the run and keeps the
                // ok=false shape, because the caller's token is fine and it
                // is the command that did not finish.
                if (ct.IsCancellationRequested) throw new OperationCanceledException(ct);
                // Drain the stderr the child produced BEFORE it stalled. It used
                // to be abandoned here, which threw away the one piece of
                // evidence a timeout leaves: a torch CUDA init that hangs, an OOM
                // kill, a model that never loads all write to stderr first, and
                // StemSeparationError.Build was then asked to classify a failure
                // from the single sentence below. The child is already killed, so
                // the pipe is closing; the wait is a belt-and-braces bound, not a
                // timing assumption.
                var partialStderr = string.Empty;
                try
                {
                    partialStderr = await seTask.WaitAsync(TimeSpan.FromSeconds(2), CancellationToken.None)
                                                .ConfigureAwait(false);
                }
                catch (Exception ex) { Debug.WriteLine($"PythonEnvManager.Run: stderr drain after timeout failed: {ex.Message}"); }
                var timedOut = $"Timed out after {timeout}.";
                return (false, sb.ToString(),
                        partialStderr.Length == 0 ? timedOut : timedOut + Environment.NewLine + partialStderr);
            }
            try { await soTask.ConfigureAwait(false); }
            catch (Exception ex) { Debug.WriteLine($"PythonEnvManager.Run: stdout task failed: {ex.Message}"); }
            return (p.ExitCode == 0, sb.ToString(), await seTask.ConfigureAwait(false));
        }
        catch (OperationCanceledException)
        {
            // Never fold a cancellation into the (ok:false, stderr) shape —
            // see the note above. This also covers a token already cancelled
            // before Process.Start.
            throw;
        }
        catch (Exception ex)
        {
            return (false, string.Empty, ex.Message);
        }
    }

    private static string Truncate(string s, int max) =>
        string.IsNullOrEmpty(s) ? string.Empty : s.Length <= max ? s : s[..max] + "…";
}

/// <summary>Snapshot of the embedded Python state.</summary>
public readonly record struct PythonEnvStatus(bool InterpreterPresent, bool HasPip, bool HasTorch, string? Executable, string? Version);

/// <summary>Progress report emitted during embedded Python install / package install.</summary>
public readonly record struct PythonInstallProgress(string Phase, double Fraction, string Message);
