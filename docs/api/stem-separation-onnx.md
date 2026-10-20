# JuceSharp.StemSeparation.Onnx — API

In-process ONNX stem separation. No Python, no child process, no CMake flag.
Namespace `JuceSharp.StemSeparation.Onnx`.

Package references: `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 (the native
`onnxruntime.dll`, DirectML + CPU providers, no CUDA) and `Microsoft.AI.DirectML`
(`DirectML.dll`, deployed beside the consumer and packed under
`runtimes/win-x64/native`).

## `OnnxStemSeparator`

`sealed class OnnxStemSeparator : IDisposable`

| Member | Description |
|---|---|
| `OnnxStemSeparator(string modelPath, OnnxStemModelKind kind, OnnxExecutionProvider preferredExecutionProvider = OnnxExecutionProvider.Auto, Action<string>? log = null, int intraOpNumThreads = 0, int deviceId = 0)` | `intraOpNumThreads = 0` lets the runtime choose. `deviceId` is the DXGI adapter index for DirectML (`OnnxGpuAdapters.Enumerate`), the CUDA ordinal for CUDA. A GPU session that cannot load the model is rebuilt on the CPU. |
| `string BackendDescription` | Human-readable, for logs and UI. |
| `OnnxExecutionProvider ActiveExecutionProvider` | **What `Auto` actually resolved to.** Log it. |
| `string? FallbackReason` | Why the requested provider is not the one running (it would not attach, or it could not build this model's session); `null` when it is. Show it. |
| `Task<IReadOnlyList<OnnxStemBuffer>> SeparateAsync(float[] interleavedStereo, int sampleRate, IProgress<double>? progress = null, CancellationToken ct = default)` | PUSH progress. |
| `IAsyncEnumerable<OnnxStemProgress> SeparateWithProgressAsync(float[] interleavedStereo, int sampleRate, CancellationToken ct = default)` | The same run, PULL progress. Phases `loading` → `inference`… → one `stem` per buffer → `done`; the `done` item carries every buffer in `Result`. See [Streaming progress](../guides/stem-backends.md#streaming-progress-await-foreach). |
| `const string BackendId` | `"onnx"` — stamped on every streamed sample. |
| `void Dispose()` | **Drains an in-flight inference** before freeing the session. |

## `OnnxModelCatalog`

Known ONNX models with pinned SHA-256 hashes, plus a verifying, resumable
downloader. Full table and the provenance of every hash:
[the backend guide](../guides/stem-backends.md#getting-a-model-onnxmodelcatalog).

| Member | Description |
|---|---|
| `static IReadOnlyList<OnnxModelEntry> All` | Every known model. |
| `static OnnxModelEntry? FindById(string? id)` | Case-insensitive; a miss is null. |
| `static IReadOnlyList<OnnxModelEntry> ForKind(OnnxStemModelKind kind)` | Every entry for one driver. |
| `static OnnxModelEntry DefaultFor(OnnxStemModelKind kind)` | The entry a bare kind means. |
| `static Task<string> EnsureAsync(OnnxStemModelKind kind, string targetDir, IProgress<DownloadProgress>? progress = null, bool allowUnverified = false, Action<string>? log = null, CancellationToken ct = default)` | Download-if-needed, **verify**, return the path. An existing file is hashed, not trusted; a mismatch is re-downloaded. Resumes a `.part` over HTTP Range. Cancelling throws `OperationCanceledException` and KEEPS the partial. |
| `static Task<string> EnsureAsync(OnnxModelEntry entry, …)` | The same for a specific entry. |
| `static Task<bool> VerifyAsync(OnnxStemModelKind kind, string path, CancellationToken ct = default)` | Missing file ⇒ `false`. An entry with **no** pinned hash throws `InvalidOperationException` — "I cannot answer" must not be the same value as "the file is corrupt". |
| `static Task<string> ComputeSha256Async(string path, CancellationToken ct = default)` | Lower-case hex, for pinning a model you obtained some other way. |
| `static void ValidateEntry(OnnxModelEntry entry)` | Pre-flight an entry: `FileName` must be a BARE file name (`..\..\startup.exe` was an arbitrary-path writer into a file ONNX Runtime then loads) and every URL absolute https, loopback http excepted. **Public since 0.9** — a host that synthesizes entries from a settings file or a user-typed URL wants to reject a bad one where it builds it. |

`EnsureAsync` is `VerifiedDownload.EnsureFileAsync` once per mirror; the
catalogue's own contribution is the `allowUnverified` decision and the mirror
walk. For a file that belongs to no model kind — a ggml weight, a user `.ckpt` —
call that kind-less entry point directly rather than inventing a `Kind` for it:
see [`VerifiedDownload`](stem-separation.md#verifieddownload).

`OnnxModelEntry`: `sealed record (string Id, OnnxStemModelKind Kind, string DisplayName, string FileName, IReadOnlyList<string> Urls, string? Sha256, long SizeBytes, string HashSource, int SourceCount, string LicenceNote)`.
`Urls` is a MIRROR list — every URL must serve the same bytes. `Sha256 = null`
means "unverified", not "skip the check": `EnsureAsync` refuses it without an
explicit `allowUnverified: true`. `HashSource` is mandatory either way.

`DownloadProgress` (in `JuceSharp.StemSeparation.Runtime`):
`readonly record struct (long BytesReceived, long? TotalBytes, double Fraction, bool Resumed)`.
`Fraction` is `-1` when the server declared no length (a chunked response);
`BytesReceived` includes the bytes a resumed `.part` already held.

## Enums

`OnnxExecutionProvider`: `Auto`, `DirectML`, `Cuda`, `Cpu`. `Auto` tries DirectML, then
CUDA, then the CPU (`OnnxSessionFactory.FallbackOrder`); this package's runtime has
no CUDA provider.

`OnnxGpuAdapters.Enumerate()` → `IReadOnlyList<OnnxGpuAdapter>`: every DXGI adapter in
`deviceId` order (`DeviceId`, `Name`, `VendorId`, `DedicatedVideoMemoryBytes`,
`SharedSystemMemoryBytes`, `IsSoftware`). Index 0 is the display adapter, not the
fastest — pick on purpose. Empty off Windows; never throws.

`OnnxSessionFactory.ApplyDirectMlOptions(SessionOptions, bool allowGraphFusion = false)` /
`DirectMlSessionConfig`: memory pattern off, sequential execution, and
`ep.dml.disable_graph_fusion=1` — **a correctness setting**: with DirectML's graph
fusion on, htdemucs separates into garbage (stems 35-65x too loud, deterministic,
every adapter). Every DirectML session the factory builds takes it UNLESS the model
declares itself verified with fusion: `OnnxSessionFactory.DirectMlGraphFusionKey`
(`dml_graph_fusion`) = `DirectMlGraphFusionVerified` (`verified`) in its metadata,
checked by `AllowsDirectMlGraphFusion(metadata)`. `OnnxStemSeparator` reads the key from
the file before building the session and passes it to
`OnnxSessionFactory.Create(..., directMlGraphFusion)`.

`OnnxStemProgress`: `readonly record struct (string Phase, double Fraction, string Message, string? Stem, string Backend, IReadOnlyList<OnnxStemBuffer>? Result)`.

`OnnxStemModelKind`: `Htdemucs`, `BsRoformer`, `MdxNet` — selects the inference
driver.

`OnnxExecutionProviderParser`:

| Member | Description |
|---|---|
| `static OnnxExecutionProvider Parse(string? value)` | For config files and CLI flags. |
| `static string ToConfigString(OnnxExecutionProvider ep)` | Round-trips it back. |

## `OnnxSessionFactory`

`sealed class`

| Member | Description |
|---|---|
| `OnnxSessionFactory(Action<string>? log = null)` | |
| `static IReadOnlyCollection<string> AvailableNativeProviders` | Straight from `OrtEnv.Instance().GetAvailableProviders()` — what this build of ONNX Runtime actually has. |
| `SessionBundle Create(OnnxExecutionProvider preferred, int intraOpNumThreads = 0)` | |
| `sealed record SessionBundle(SessionOptions Options, OnnxExecutionProvider Active, string Description)` | |

## `OnnxStemBuffer`

`sealed record OnnxStemBuffer(string Name, float[] InterleavedStereo, int SampleRate)`

## Inference drivers

Public, and usable directly if you already hold an `InferenceSession`.

| Type | Settings record | Entry points |
|---|---|---|
| `HtdemucsOnnxInference` | `Settings(float MarginSeconds = 0.5f, float CrossfadeSeconds = 0.05f)` | `int StemCount`, `IReadOnlyList<StemResult> Predict(float[] interleavedStereo, int sampleRate, IProgress<double>?, CancellationToken)`; `StemResult(string Name, float[] InterleavedStereo)`. |
| `BsRoformerInference` | `Settings(int NFft = 2048, int Hop = 441, int ChunkSize = 352800, int Overlap = 2)` — the packed layout's defaults; a split-complex model's comes from its metadata | `BsRoformerIoBinding Binding`, `Settings Geometry`, `IReadOnlyList<string> StemNames`, `float[] PredictVocals(...)`, `IReadOnlyList<float[]> PredictAllStems(...)`, `IReadOnlyList<(string Name, float[] Stereo)> PredictNamedStems(...)`. |
| `MdxNetInference` | `Settings(int NFft = 6144, int Hop = 1024, int DimF = 3072, int DimT = 256, int Overlap = 2)` with a computed `ChunkSize` | `float[] PredictInstrumental(...)`. |

> ℹ️ **Note**
> HTDemucs runs at **44.1 kHz** natively. BS-RoFormer and MDX-Net resample the
> input to 44.1 kHz internally — so feeding a 48 kHz buffer is correct, not a
> mistake, but it does cost a resample.

## Helpers

| Type | Description |
|---|---|
| `Stft` | `HannWindow`, `HannWindowPeriodic`, `Forward` / `Inverse`, and `ForwardHtdemucs` / `InverseHtdemucs` (the HTDemucs-specific framing). The allocating overloads take and return `SpectrogramFrames(Complex[][] Frames, int NFft, int Hop, int OriginalLength)`; the **allocation-free** overloads take a `SpectrumPlanes` destination and an `StftWorkspace` scratch, which is what makes the driver's chunk loop allocation-free. |
| `SpectrumPlanes` | Pre-allocated `Re` / `Im` plane pair (`float[frames][bins]`, `bins = nFft/2 + 1`) plus `NFft`, `Hop`, `OriginalLength`, `FrameCount`, `BinCount`, `Clear()`. Allocated once and overwritten in place; per-frame rows are distinct arrays so a `Parallel.For` over frames can write them concurrently. |
| `StftWorkspace` | The scratch buffers those overloads need — `new StftWorkspace(nFft, maxPaddedLength, maxFrames)`, with `EstimatedBytes` for budgeting. Reuse one per worker, never one per chunk. |
| `SampleRateConverter` | `static float[] Resample(float[] interleaved, int channels, int srcRate, int dstRate)` — Lanczos-8 windowed sinc. A rational rate pair (every real one) uses a per-phase weight table (160 phases for 44.1 <-> 48 kHz) instead of two `Math.Sin` per tap; same output to 1e-5. |

`InferenceGuard` and `HtdemucsChunkScratch` are `internal`.

## Example

```csharp
using JuceSharp.StemSeparation.Onnx;

Console.WriteLine(string.Join(", ", OnnxSessionFactory.AvailableNativeProviders));

using var sep = new OnnxStemSeparator(@"models\htdemucs.onnx",
                                      OnnxStemModelKind.Htdemucs,
                                      OnnxExecutionProvider.Auto,
                                      log: Console.WriteLine);

Console.WriteLine($"{sep.BackendDescription} -> {sep.ActiveExecutionProvider}");

var stems = await sep.SeparateAsync(interleavedStereo, sampleRate,
                                    new Progress<double>(p => bar.Value = p * 100),
                                    token);
```

## Notes

* **Log `ActiveExecutionProvider`.** A silent fall back from DirectML or CUDA
  to CPU is the usual explanation for a separation that suddenly takes twenty
  minutes.
* DirectML (`DirectML.dll`) is a Microsoft proprietary redistributable; ONNX
  Runtime itself is MIT. See the repository `LICENSE`.
* This runs **in your process** — a fault inside the model or the runtime is
  not contained. Use the Python sidecar if you need isolation.
