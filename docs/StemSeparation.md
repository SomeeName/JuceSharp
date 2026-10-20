# JuceSharp.StemSeparation — Developer Guide

> A first-class, embedded ML stem-separation pipeline for .NET 10+.
> Talks to a bundled Python sidecar so torch crashes can't take the
> audio host down, and surfaces a clean async API with strongly-typed
> exceptions for every well-known failure mode.

This guide walks third-party consumers (DotCharter, your own DAW
plugin, a CLI tool, …) through a complete production integration of
`StemSeparator`. It assumes you've already referenced the project or package:

```xml
<ItemGroup>
  <PackageReference Include="JuceSharp.StemSeparation" Version="*" />
</ItemGroup>
```

The csproj declares no `<Version>`, so a locally packed build carries the
MSBuild default — reference the project directly while iterating.

## 0. Which backend?

There are **three** stem-separation backends in this repository, and this page
documents only the third. Pick before you read on — see
[Choosing a Stem-Separation Backend](guides/stem-backends.md) for the full
comparison.

| Backend | Package | Shape |
|---|---|---|
| `OnnxStemSeparator` | `JuceSharp.StemSeparation.Onnx` | In-process ONNX (HTDemucs / MDX-Net / BS-RoFormer) over DirectML (any DX12 GPU) or the CPU. No Python, no child process. |
| `JuceStemSeparation` | **core `JuceSharp`** | The native demucs.cpp path over the `dca_stems_*` ABI (v5). Compiled in only with `-DDOTCHARTER_STEMS_DEMUCS_CPP=ON`, which is **off by default** — `IsSupported` is `false` in a stock build. |
| `StemSeparator` | `JuceSharp.StemSeparation` | **This page.** An embedded CPython child process running torch + demucs + bs_roformer. |

`JuceSharp.StemSeparation` also ships `CkptToOnnxConverter`, which converts a
`.ckpt` checkpoint into the ONNX model the first backend consumes.

---

## 1. The 30-second example

```csharp
using JuceSharp.StemSeparation.Processing;

var separator = new StemSeparator();
var result = await separator.SeparateTrackAsync(
    inputPath: @"C:\music\song.wav",
    outputDir: @"C:\music\song-stems",
    options:   new StemSeparationOptions { Model = "htdemucs", Device = "auto" });

foreach (var stem in result.Stems)
    Console.WriteLine($"{stem.Name,-8} → {stem.Path}");
```

That's it. The first run downloads the embedded Python distribution
(~25 MB on Windows) and pip-installs the model dependencies (the heavy
one is torch). Subsequent runs reuse the cached interpreter, so they
start instantly.

---

## 2. Lifecycle: what actually happens

Install-time phases come from `PythonInstallProgress` and are forwarded
verbatim; run-time phases are emitted by `StemSeparator` itself.

| Phase | Emitted by | What happens | Where the time goes (first run) |
| ----- | ---------- | ------------ | ------------------------------- |
| `system` | install | An existing system interpreter was accepted instead of downloading one. | — |
| `download` | install | `python-3.11.9-embed-amd64.zip` fetched from python.org. | 5–15 s |
| `extract` | install | The archive is unpacked and `._pth` patched. | 1–5 s |
| `pip-download` | install | `get-pip.py` is fetched. | seconds |
| `pip-install` | install | `pip install "torch>=2.1,<2.6" "torchaudio>=2.1,<2.6" "numpy<2" soundfile librosa "demucs>=4.0" "bs_roformer>=1.1.0" pyyaml` — **the bounds are load-bearing**, and `PythonPackagePins` is where they live. torch 2.6 flips `torch.load`'s default to `weights_only=True`, which breaks the demucs / RoFormer checkpoints as published; torchaudio must be co-pinned or pip resolves a build whose native DLLs are ABI-incompatible with the pinned torch (`OSError: [WinError 127]`); numpy 2 breaks the ABI every torch < 2.3 wheel was built against. If you pre-populate an environment by hand, use these strings verbatim. | 60–600 s (mostly torch) |
| `pip-installed` | install | The pip run finished. | — |
| `pip-ready` | install | pip is present and usable. | — |
| `ready` | install / run | The environment is usable; also emitted once the model is loaded. | — |
| `queued` | run | Waiting on the process-global concurrency gate. | — |
| `loading` | run | Sidecar process spawned, model + checkpoint loaded onto the chosen device. | 2–6 s |
| `inference` | run | Audio streamed through the model in chunks, progress reported continuously. | depends on duration / GPU |
| `stem` | run | An output wav appeared on disk; `Stem` names it. | <1 s per stem |
| `done` | run | Final 100 % progress tick, results returned. | — |

There is no `packages` phase — that string never appears in the code.
`CkptToOnnxConverter` reuses the same `PythonInstallProgress` record and adds
one more phase of its own, `convert`.

The install phases only happen once per machine. The run phases happen on
every separation. The
`StemSeparator` folds install progress into the **leading 5 %** of the
overall progress bar so the perceived progress curve stays smooth — no
"stuck at 0 % for two minutes then jumps to 100 %" UX.

---

## 3. Wiring a progress bar correctly

There are two ways to get progress, and **the stream is the one to reach for
first**:

```csharp
await foreach (var p in separator.SeparateWithProgressAsync(inputPath, outDir, opts, ct))
{
    if (p.Fraction >= 0) bar.Value = p.Fraction * 100;
    status.Text = p.Message;
    if (p.Result is not null) Commit(p.Result);   // the terminal item, once
}
```

`SeparateWithProgressAsync` pulls: your `await foreach` sets the pace, so you
do not need a throttle of your own, and the last item hands you the
`SeparationResult` — there is no second `await` to forget. The samples are
delivered in order, a fault keeps its original exception type, and cancelling
(or `break`ing out) cancels the separation with an
`OperationCanceledException`. The whole contract, including what a slow
consumer loses and what it cannot lose, is in
[Streaming progress](guides/stem-backends.md#streaming-progress-await-foreach).

`SeparateTrackAsync` pushes, through `IProgress<StemSeparationProgress>`, and
is unchanged. Either way, every report carries:

```csharp
public readonly record struct StemSeparationProgress(
    string  Phase,      // install: "system" | "download" | "extract"
                        //   "pip-download" | "pip-install" | "pip-installed" | "pip-ready"
                        // run: "queued" | "loading" | "ready" | "inference" | "stem" | "done"
    double  Fraction,   // 0..1 overall, OR -1 for sub-events with no fraction
    string  Message,    // Human-readable, fine for a status line
    string? Stem,       // Stem name if Phase == "stem", else null
    string? Backend = null,          // "python-sidecar" for this backend
    SeparationResult? Result = null);// non-null on the terminal "done" item ONLY
```

### The rules

1. **Drive the progress bar from `Fraction` whenever it is ≥ 0.** It is
   guaranteed to be **monotonic non-decreasing** for the whole run,
   spanning install + inference, scaled into 0..1.
2. **Ignore `Fraction == -1`.** Those are sub-events (e.g. a stem just
   finished being written) that intentionally have no fractional
   position. Use them to update the status label, not the bar.
3. **The `Phase` string is stable — branch on it for UI text.**
4. **`Result` is non-null on exactly one report per run** — the terminal
   `done` one — and it carries every stem. That is deliberate: the progress
   channel behind `SeparateWithProgressAsync` drops the OLDEST queued sample
   when a consumer falls behind, so an interim `stem` report is expendable,
   while the terminal item is by construction the newest write and can never
   be the one dropped.
5. **Reports may arrive on a thread pool thread.** Marshal to the UI
   thread yourself. If you use `IProgress<T>` constructed on the UI
   thread, it does this for you, **but be aware** the standard
   `Progress<T>` implementation can re-order rapid back-to-back
   reports. If exact ordering matters (e.g. you're computing
   monotonicity yourself), use a synchronous wrapper.

### A complete WinForms/WPF/Avalonia-friendly example

```csharp
var progress = new Progress<StemSeparationProgress>(p =>
{
    // Always on the UI thread because we constructed Progress<T> there.
    ProgressBar.Value = p.Fraction >= 0 ? p.Fraction * 100 : ProgressBar.Value;
    StatusLabel.Text  = p.Phase switch
    {
        "download"  => $"Downloading embedded Python… {p.Fraction:P0}",
        "extract"     => "Unpacking Python…",
        "pip-install" => $"Installing model dependencies… {p.Message}",
        "queued"      => "Waiting for a free separation slot…",
        "loading"   => $"Loading {options.Model} on {options.Device}…",
        "ready"     => "Model loaded.",
        "inference" => $"Separating… {p.Fraction:P0}",
        "stem"      => $"Wrote stem: {p.Stem}",
        "done"      => "Done.",
        _           => p.Message,
    };
});
```

### Cancellation

Pass a `CancellationToken`. When you cancel it, the underlying Python
process is **killed (entire process tree)**, all `*.tmp.*` download files are
swept, and the call throws **`OperationCanceledException`** — wherever the
cancellation landed.

> ⚠️ **Warning**
> A cancelled separation must never surface as `StemSeparationException`.
> `SeparateTrackAsync` calls `ct.ThrowIfCancellationRequested()` before it
> inspects the child's exit status, precisely so that a user pressing Cancel
> does not read to every consumer as a reason to fall through to the next
> backend.

```csharp
using var cts = new CancellationTokenSource();
CancelButton.Click += (_, _) => cts.Cancel();

try
{
    var result = await separator.SeparateTrackAsync(
        input, outDir, options, progress, cts.Token);
}
catch (OperationCanceledException) when (cts.IsCancellationRequested)
{
    // User pressed Cancel. Output directory may contain partial wavs.
}
```

---

### Getting the model file

This backend downloads its own models through `demucs` / `bs_roformer`. The
in-process ONNX backend does not, and `OnnxModelCatalog.EnsureAsync` is where
its model comes from: a table of known models with pinned SHA-256 hashes and a
downloader that verifies the COMPLETE file before the atomic rename, resumes an
interrupted transfer over HTTP `Range`, and re-downloads an existing file that
fails its hash rather than trusting it. See
[Getting a model](guides/stem-backends.md#getting-a-model-onnxmodelcatalog).

## 4. Strongly-typed exceptions

Every well-known Python / torch failure is mapped to a concrete subtype
of `StemSeparationException`. **Catch the specific one you can
remediate, then fall through to the base type** for everything else.

| Subtype                                  | `Kind`                  | Trigger signatures (case-insensitive substrings)                                                                                                                                                |
| ---------------------------------------- | ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `StemSeparationVramException`            | `VramExhausted`         | `CUDA out of memory`, `torch.cuda.OutOfMemoryError`, `HIP out of memory`, `DML_OUT_OF_MEMORY`, `DirectML allocator`, `MPS backend out of memory`, `RuntimeError: out of memory`, `CUBLAS_STATUS_ALLOC_FAILED` |
| `StemSeparationCudaDriverException`      | `CudaDriverMismatch`    | `CUDA driver version is insufficient`, `cuDNN error`, `CUDA error: invalid device`, `forward compatibility was attempted`                                                                       |
| `StemSeparationCudaUnavailableException` | `CudaUnavailable`       | `no CUDA GPUs are available`, `Torch not compiled with CUDA`                                                                                                                                    |
| `StemSeparationMissingPackageException`  | `MissingPackage`        | `ModuleNotFoundError`, `ImportError:`, `No module named`                                                                                                                                        |
| `StemSeparationModelLoadException`       | `ModelLoadFailure`      | `checkpoint … not found / missing / corrupt`, `safetensors … invalid`, `state_dict … unexpected`                                                                                                |
| `StemSeparationAudioFormatException`     | `AudioFormatUnsupported`| `soundfile … (format not recognised \| could not open)`, `LibsndfileError`, `Error opening`                                                                                                     |
| `StemSeparationException` (base)         | `Unknown`               | Anything else (including `"Cancelled."` and `"Timed out…"`).                                                                                                                                    |

### Idiomatic remediation loop

```csharp
StemSeparationOptions opts = new() { Model = "htdemucs_ft", Device = "cuda" };

for (int attempt = 0; attempt < 3; attempt++)
{
    try
    {
        return await separator.SeparateTrackAsync(input, outDir, opts, progress, ct);
    }
    catch (StemSeparationVramException vram) when (attempt == 0)
    {
        log.LogWarning("VRAM exhausted on {Device} running {Model}. " +
                       "Dropping overlap and retrying.", vram.Device, vram.Model);
        opts = opts with { Overlap = 0.10, Shifts = 0 };
    }
    catch (StemSeparationVramException vram) when (attempt == 1)
    {
        log.LogWarning("Still out of VRAM. Falling back to CPU.");
        opts = opts with { Device = "cpu" };
    }
    catch (StemSeparationCudaUnavailableException)
    {
        opts = opts with { Device = "cpu" };
    }
    catch (StemSeparationException ex)
    {
        // Unrecoverable — surface to user.
        log.LogError(ex, "Separation failed: {Kind}. {Stderr}", ex.Kind, ex.Stderr);
        throw;
    }
}
```

Every subtype exposes `Stderr` (the **full** captured stderr) so a
"Copy diagnostics to clipboard" button is one line of code.

---

## 5. Configuration knobs (`StemSeparationOptions`)

| Property              | Default      | Notes                                                                          |
| --------------------- | ------------ | ------------------------------------------------------------------------------ |
| `Model`               | `"htdemucs"` | Demucs: `htdemucs`, `htdemucs_ft`, `htdemucs_6s`, `mdx_extra`. RoFormer: `bs_roformer`, `bs_roformer_sw`, custom path via `ModelPath`. |
| `Device`              | `"auto"`     | `"auto" \| "cuda" \| "cuda:0" \| "mps" \| "cpu"`.                              |
| `Shifts`              | `1`          | Demucs equivariant shifts. 0 = fastest, 5 = highest quality.                   |
| `Overlap`             | `0.25`       | Chunk overlap fraction. Lower → less VRAM, slightly lower quality.             |
| `ModelPath`           | `null`       | Override path to a `.pt` / `.ckpt` / `.safetensors` checkpoint.                |
| `ConfigPath`          | `null`       | RoFormer YAML config when using a custom checkpoint.                           |
| `Stems`               | `null`       | Filter outputs to a subset, e.g. `new[] { "vocals", "drums" }`.                |
| `Timeout`             | `4 h`        | Hard upper bound on the child process. Covers full albums on CPU.              |
| `SkipPackageBootstrap`| `false`      | Skip pip on every call. Set after you've confirmed `IsReady()` returns true.   |
| `MaxConcurrentSeparations` | `1`     | **Process-global** ceiling on concurrent separations — see below.              |

`StemSeparationOptions.Default` is a ready-made instance carrying every default
above.

### Concurrency

The gate is a **process-static** `SemaphoreSlim`, not a per-instance one.
`MaxConcurrentSeparations` grows it just before the child launches, and
`StemSeparator.GlobalConcurrencyLimit` exposes the same knob directly:

```csharp
StemSeparator.GlobalConcurrencyLimit = 2;
```

> ⚠️ **Warning**
> **The limit can only grow.** Assigning a value lower than the current one
> throws `InvalidOperationException` — a semaphore's count cannot be taken back
> while callers may be holding it.

While the gate is full, callers sit in the `queued` phase.

### A "is everything installed?" cheap gate

```csharp
if (!separator.IsReady())
{
    var notice = MessageBox.Show("First-run setup will download ~2 GB. Continue?",
                                 "Stem Separation", MessageBoxButtons.OKCancel);
    if (notice != DialogResult.OK) return;
}

// Now it's safe to call SeparateTrackAsync — the install will run inline,
// or you can prime it by awaiting EnsureInterpreterAsync + EnsurePackagesAsync
// from a startup task.
```

---

## 6. Threading model

* `SeparateTrackAsync` is fully async. It never blocks the calling thread.
* The Python child process runs **in its own OS process** — even a hard
  segfault inside torch cannot tear down your app.
* Progress reports honour the synchronization context captured when you
  constructed `Progress<T>` (standard .NET semantics).
* Multiple concurrent calls are safe, but they are throttled by the
  **process-global** gate described in §5 — not per instance. With the default
  `MaxConcurrentSeparations = 1`, the second caller waits in the `queued`
  phase until the first finishes, however many `StemSeparator` objects you
  created.

---

## 7. Where data lives

```
%LOCALAPPDATA%\JuceSharp\python-embed\   # Windows
~/.local/share/JuceSharp/python-embed/   # Linux / macOS
```

That is **LocalApplicationData**, not roaming `%APPDATA%`. Override it with the
`rootDirectory` argument to `PythonEnvManager`.

* `python.exe` (or `python3`) plus its standard library — CPython **3.11.9**
  (`PythonEnvManager.DefaultPythonVersion`).
* `Lib/site-packages/` with torch, demucs, bs_roformer, …
* Any `*.tmp.*` files in this directory are **partial downloads** from
  a crashed boot. They are swept automatically on the next
  `PythonEnvManager` construction.

Model weights are downloaded by demucs / huggingface into their own
caches (`~/.cache/torch/hub/`, `~/.cache/huggingface/`), **not** into
the embed root.

---

## 8. Testing your integration

Two reusable patterns ship with JuceSharp's own test project:

1. **Inline-script tests.** Construct a `PythonEnvManager` with a
   system-installed python via the `internal` `Executable` setter,
   point `StemSeparator` at a tiny script you write to a temp file,
   and assert against the line protocol. No network, no torch — runs
   in <100 ms.
2. **Loopback-listener download tests.** Spin up an `HttpListener` on
   127.0.0.1 and call `PythonEnvManager.DownloadWithProgress`
   directly. Lets you verify atomic-swap, cancellation, and corruption
   handling without ever touching the public internet.

See `JuceSharp.Tests/StemSeparatorTests.cs` and
`DownloadResilienceTests.cs` for working examples.

For an end-to-end smoke test against real torch + demucs, set
`JUCESHARP_RUN_INTEGRATION=1` and the gated tests in
`StemSeparationIntegrationTests.cs` will run.

---

## 9. FAQ

**Q: Why a subprocess instead of pythonnet?**
A: A torch crash on a 16-minute track at 3 AM must not crash the DAW.
Process isolation is the only honest answer. The line-protocol pipe is
< 1 µs per update — it is transparent to the managed wrapper.

**Q: Can I bring my own Python?**
A: Yes. `PythonEnvManager.Executable` has an `internal` setter
exposed via `InternalsVisibleTo("JuceSharp.Tests")`. For external
consumers, simply pre-populate the embed root with a working
interpreter and skip `EnsureInterpreterAsync`.

**Q: Where do model weights download from?**
A: Demucs models come from `dl.fbaipublicfiles.com` via demucs'
built-in downloader. RoFormer models come from the Hugging Face Hub.
Both have their own atomic-swap and resume logic.

**Q: My GPU has 6 GB and `htdemucs_ft` OOMs. What now?**
A: Catch `StemSeparationVramException`, retry with
`Overlap = 0.10, Shifts = 0`, and if that still OOMs fall back to
`Device = "cpu"`. The "Idiomatic remediation loop" in §4 does exactly
this.
