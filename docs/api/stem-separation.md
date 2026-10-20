# JuceSharp.StemSeparation — API

The Python-sidecar backend. Narrative guide:
[the StemSeparation developer guide](../StemSeparation.md); backend comparison:
[Choosing a Stem-Separation Backend](../guides/stem-backends.md).

Namespaces: `JuceSharp.StemSeparation.Processing` and
`JuceSharp.StemSeparation.Runtime`.

## `IStemSeparator`

| Member | Description |
|---|---|
| `bool IsReady()` | Is the environment usable without an install step? |
| `PythonEnvStatus Probe()` | What is present. |
| `Task<SeparationResult> SeparateTrackAsync(string inputPath, string outputDir, StemSeparationOptions? options = null, IProgress<StemSeparationProgress>? progress = null, CancellationToken ct = default)` | Run a separation, PUSH progress. |
| `IAsyncEnumerable<StemSeparationProgress> SeparateWithProgressAsync(string inputPath, string outputDir, StemSeparationOptions? options = null, CancellationToken ct = default)` | The same run, PULL progress. **Default interface method** — a backend that implements only `SeparateTrackAsync` gets it for free. The LAST item carries the `SeparationResult` in `Result`. See [Streaming progress](../guides/stem-backends.md#streaming-progress-await-foreach). |

## `StemSeparator : IStemSeparator`

`sealed class StemSeparator`

| Member | Description |
|---|---|
| `StemSeparator(PythonEnvManager? env = null, ILogger<StemSeparator>? logger = null, string? scriptPathOverride = null)` | All three arguments are for testing and embedding. |
| `static IReadOnlyList<string> RequiredPackages` | `torch>=2.1,<2.6`, `torchaudio>=2.1,<2.6`, `numpy<2`, `soundfile`, `librosa`, `demucs>=4.0`, `bs_roformer>=1.1.0`, `pyyaml`. |
| `static int GlobalConcurrencyLimit { get; set; }` | The **process-static** gate. **Grow-only** — assigning a lower value throws `InvalidOperationException`. |
| `bool IsReady()` · `PythonEnvStatus Probe()` · `Task<SeparationResult> SeparateTrackAsync(…)` · `IAsyncEnumerable<StemSeparationProgress> SeparateWithProgressAsync(…)` | the four `IStemSeparator` members. `SeparateWithProgressAsync` has a default interface implementation that bridges `SeparateTrackAsync`, so a backend only has to write the other three. |
| `Task<PythonEnvStatus> ProbeAsync(CancellationToken ct = default)` · `Task<bool> IsReadyAsync(CancellationToken ct = default)` | the async forms — prefer them on a UI thread; `Probe()` shells out to the interpreter. |
| `const string BackendId` | `"python-sidecar"` — stamped on every streamed progress sample. |

## `StemSeparationOptions`

`sealed record`

| Property | Default | Notes |
|---|---|---|
| `Model` | `"htdemucs"` | `htdemucs`, `htdemucs_ft`, `htdemucs_6s`, `mdx_extra`, `bs_roformer`, … |
| `Device` | `"auto"` | `auto` / `cuda` / `cuda:0` / `mps` / `cpu`. |
| `Shifts` | `1` | Demucs equivariant shifts. |
| `Overlap` | `0.25` | Chunk overlap. Lower uses less VRAM. |
| `ModelPath` | `null` | A `.pt` / `.ckpt` / `.safetensors` override. |
| `ConfigPath` | `null` | RoFormer YAML for a custom checkpoint. |
| `Stems` | `null` | Restrict the outputs. |
| `Timeout` | **`4 h`** | Hard upper bound on the child process. |
| `SkipPackageBootstrap` | `false` | Skip pip on every call. |
| `MaxConcurrentSeparations` | `1` | Grows the process-global gate. |
| `static Default` | | A ready-made instance. |

## Result and progress types

| Type | Shape |
|---|---|
| `SeparationResult` | `sealed record (string InputPath, string OutputDirectory, string Model, string Device, IReadOnlyList<StemFile> Stems)` with `string? GetStemPath(string name)`. |
| `StemFile` | `readonly record struct (string Name, string Path)`. |
| `StemSeparationProgress` | `readonly record struct (string Phase, double Fraction, string Message, string? Stem, string? Backend = null, SeparationResult? Result = null)`. `Fraction` is `-1` for sub-events with no position. `Backend` is the backend id (`StemSeparator.BackendId` is `"python-sidecar"`). `Result` is non-null on EXACTLY ONE sample per run — the terminal `done` item — which is what lets the progress channel drop interim samples without losing anything. The two trailing parameters have defaults, so four-argument construction still compiles. |

## Exceptions

`StemSeparationErrorKind`: `Unknown`, `VramExhausted`, `CudaDriverMismatch`,
`CudaUnavailable`, `MissingPackage`, `ModelLoadFailure`,
`AudioFormatUnsupported`.

| Type | `Kind` | Extra members |
|---|---|---|
| `StemSeparationException` (base, not sealed) | any | `Kind`, `Stderr` — the captured stderr **TRUNCATED to its last 800 characters** for every classified kind; only the `Unknown` fall-through keeps it whole. The tail is where the traceback's cause is, and a full GPU log is megabytes. |
| `StemSeparationVramException` | `VramExhausted` | `Model`, `Device` |
| `StemSeparationCudaDriverException` | `CudaDriverMismatch` | `Model`, `Device` |
| `StemSeparationCudaUnavailableException` | `CudaUnavailable` | `Model`, `Device` |
| `StemSeparationMissingPackageException` | `MissingPackage` | — |
| `StemSeparationModelLoadException` | `ModelLoadFailure` | `Model` |
| `StemSeparationAudioFormatException` | `AudioFormatUnsupported` | — |

> ⚠️ **Warning**
> **A cancelled run throws `OperationCanceledException`, never
> `StemSeparationException`.** If you are chaining backends, a user Cancel must
> not read as a reason to fall through to the next one.

## `VerifiedDownload`

`static class` — namespace `JuceSharp.StemSeparation.Runtime`

The **kind-less** public face of the package-internal `VerifiedDownloader`:
fetch one file from one URL to one path, verified, resumable, atomically
renamed. Both model catalogues are built on it, so there is exactly one
implementation of "download this safely" in the two stems packages
(`HttpDownloadLintTests` keeps it that way). Guide:
[Downloading anything else](../guides/stem-backends.md#downloading-anything-else-verifieddownload).

| Member | Description |
|---|---|
| `static Task<string> EnsureFileAsync(Uri url, string destinationPath, string? sha256 = null, long? expectedBytes = null, IProgress<DownloadProgress>? progress = null, Action<string>? log = null, CancellationToken ct = default)` | Present-and-correct, or downloaded. An existing file is CHECKED, not trusted: hashed when there is a hash, length-checked when there is only a size, kept with a warning when there is neither. Verifies the COMPLETE file before the atomic rename. Resumes a per-URL `.part` over HTTP `Range`. Returns the destination path. |
| `static Task<bool> VerifyFileAsync(string path, string sha256, long? expectedBytes = null, CancellationToken ct = default)` | Missing file ⇒ `false`. A blank `sha256` THROWS `ArgumentException` — `false` must not mean both "corrupt" and "nobody pinned it". |
| `static Task<string> ComputeSha256Async(string path, CancellationToken ct = default)` | Lower-case hex, for pinning a copy you obtained and validated some other way. |
| `static void ValidateSourceUrl(string? url, string subject, string paramName)` | The transport rule in ONE place: absolute `https`, or plain `http` to a LOOPBACK host. Both catalogues' `ValidateEntry` call it. |
| `static int CleanPartials(string directory, TimeSpan olderThan, Action<string>? log = null)` | Reclaim abandoned `.part` prefixes from `directory`, non-recursively; returns how many went. A file still being written is held `FileShare.None` by the downloader and is skipped. **`TimeSpan.Zero` is not fully safe against a CONCURRENT download**: between HTTP attempts the writer's handle is closed, so a zero sweep can delete a live download's prefix — it costs that download a restart (the size/hash check rejects the stale resume), never a corrupt file. Pass a non-zero `olderThan` unless you know nothing else is fetching into that directory. |

Throws: `ArgumentException` (blank destination, non-https source),
`InvalidDataException` (hash or size mismatch — nothing is left at the
destination), `IOException` (truncated transfer, **timeout**, or an unusable
existing file that could not be replaced), `HttpRequestException`,
`OperationCanceledException` (the CALLER's token only).

> ⚠️ **Warning**
> **A timeout is an `IOException`, never an `OperationCanceledException`.**
> `HttpClient.Timeout` covers the content reads and expires as
> `TaskCanceledException`, which is indistinguishable from a user Cancel — so a
> slow first mirror used to prevent the second from ever being tried, and every
> consumer reported a user cancel. The budget lives on a linked token inside the
> downloader and expiry is translated before it leaves the type.

## `DemucsGgmlModelCatalog`

`static class` — namespace `JuceSharp.StemSeparation.Runtime`

The ggml weights the **native demucs.cpp** backend loads, with pinned SHA-256
hashes, over `VerifiedDownload`. Full table and the provenance of every hash:
[the backend guide](../guides/stem-backends.md#getting-a-ggml-weight-demucsggmlmodelcatalog).

| Member | Description |
|---|---|
| `static IReadOnlyList<DemucsGgmlModelEntry> All` | Every known weight. |
| `static DemucsGgmlModelEntry Default` | The 4-source HTDemucs — the balanced default. |
| `static DemucsGgmlModelEntry? FindById(string? id)` | Case-insensitive; a miss is null. |
| `static Task<string> EnsureAsync(DemucsGgmlModelEntry entry, string targetDir, IProgress<DownloadProgress>? progress = null, bool allowUnverified = false, Action<string>? log = null, CancellationToken ct = default)` | `VerifiedDownload.EnsureFileAsync` per mirror, plus the two things a catalogue owns: an unpinned entry is REFUSED unless the caller opts in, and a failed mirror falls through while a user Cancel does not. |
| `static Task<bool> VerifyAsync(DemucsGgmlModelEntry entry, string path, CancellationToken ct = default)` | Missing file ⇒ `false`. An entry with no pinned hash THROWS. |
| `static void ValidateEntry(DemucsGgmlModelEntry entry)` | Pre-flight: bare file name, at least one URL, every URL https (loopback http excepted). Public, so a host synthesizing entries can check them where it builds them. |

`DemucsGgmlModelEntry`: `sealed record (string Id, string DisplayName, string FileName, IReadOnlyList<string> Urls, string? Sha256, long SizeBytes, string HashSource, int SourceCount, string LicenceNote)`.
No model KIND — that is the point: the field on the ONNX entry names the
inference driver, and a ggml weight has no honest value for it.
`HashSource` is mandatory for a hash AND for the absence of one.

## `DownloadProgress`

`readonly record struct (long BytesReceived, long? TotalBytes, double Fraction, bool Resumed)`
— namespace `JuceSharp.StemSeparation.Runtime`, and the progress type of every
download in BOTH stems packages.

`TotalBytes` is null when the server declared no length (a chunked response),
and `Fraction` is `-1` for that case — show an indeterminate bar, never 0 %.
`BytesReceived` already includes the bytes a resumed `.part` file held, so a
resumed download opens part-way along.

## `PythonEnvManager`

`sealed class` — namespace `JuceSharp.StemSeparation.Runtime`

| Member | Description |
|---|---|
| `PythonEnvManager(string? rootDirectory = null, ILogger<PythonEnvManager>? logger = null)` | `null` root means the default per-user location. |
| `const string DefaultPythonVersion = "3.11.9"` | |
| `string Root` | The embed root. Windows default: `%LOCALAPPDATA%\JuceSharp\python-embed`; otherwise `~/.local/share/JuceSharp/python-embed`. |
| `string Executable` | Public getter, **internal setter** (test seam). |
| `string SitePackages` | |
| `PythonEnvStatus Probe()` | |
| `Task EnsureInterpreterAsync(IProgress<PythonInstallProgress>?, CancellationToken)` | Download + extract. |
| `Task EnsurePipAsync(IProgress<PythonInstallProgress>?, CancellationToken)` | |
| `Task EnsurePackagesAsync(IEnumerable<string> packages, IProgress<PythonInstallProgress>?, CancellationToken)` | |
| `int CleanStaleTempFiles()` | Sweep `*.tmp.*` left by a crashed boot. |

Its downloads go through the package-internal `VerifiedDownloader`, which is
the **only** place either stem-separation package fetches bytes (pinned by
`HttpDownloadLintTests`): temp file → hash the COMPLETE file → atomic rename.
The Python bootstrap uses the non-resumable policy — a half-formed interpreter
archive is deleted rather than kept — while
[`OnnxModelCatalog`](stem-separation-onnx.md) uses the resumable `.part` one.

`PythonEnvStatus` is
`readonly record struct (bool InterpreterPresent, bool HasPip, bool HasTorch, string? Executable, string? Version)`.

`PythonInstallProgress` is
`readonly record struct (string Phase, double Fraction, string Message)`. Its
phases: `system`, `download`, `extract`, `pip-download`, `pip-install`,
`pip-installed`, `pip-ready`, `ready` — plus `convert` from the converter
below.

## `CkptToOnnxConverter`

`sealed class`

| Member | Description |
|---|---|
| `CkptToOnnxConverter(PythonEnvManager? env = null, ILogger<CkptToOnnxConverter>? logger = null, string? scriptPathOverride = null)` | |
| `static IReadOnlyList<string> RequiredPackages` | `torch>=2.1,<2.6`, `numpy<2`, `pyyaml`, `onnx>=1.15`, `bs_roformer>=1.1.0` — a **smaller** set than `StemSeparator`'s. |
| `bool IsReady()` | |
| `PythonEnvStatus Probe()` | |
| `Task ConvertAsync(string ckptPath, string? yamlPath, string onnxOutputPath, IProgress<PythonInstallProgress>? progress = null, CancellationToken ct = default)` | |

Because the package sets differ, `IsReady()` on the converter and on the
separator are different questions — do not use one to gate the other.

## `ChildProcessRegistry`

**Internal**, not public API. Worth knowing about anyway: it kills and
untracks child processes, and it **never owns** the `Process` objects it
tracks — disposal belongs to whoever called `Process.Start`. Disposing from
its own `Exited` handler once made a *successful* python run report as a
separation failure.
