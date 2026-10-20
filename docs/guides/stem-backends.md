# Choosing a Stem-Separation Backend

There are **three** separate stem-separation implementations in this
repository. They do not share an interface, they live in different packages,
and the right one depends on whether you can ship Python, whether you have a
GPU, and whether you control the CMake build.

| | `OnnxStemSeparator` | `JuceStemSeparation` | `StemSeparator` |
|---|---|---|---|
| Package | `JuceSharp.StemSeparation.Onnx` | **core `JuceSharp`** | `JuceSharp.StemSeparation` |
| Runs in | your process | your process (native) | a child process |
| Needs Python | no | no | **yes** (downloads its own) |
| Needs a CMake flag | no | **yes** — `DOTCHARTER_STEMS_DEMUCS_CPP` (default OFF) | no |
| Models | HTDemucs, MDX-Net, BS-RoFormer | htdemucs (demucs.cpp) | htdemucs family, BS-RoFormer, custom checkpoints |
| Acceleration | DirectML (any DX12 GPU) / CPU | CPU | CUDA / MPS / CPU (whatever torch finds) |
| Crash isolation | none — a bad model takes the process | none | **full** — a torch segfault cannot reach you |
| First-run cost | none beyond the model file | none | ~25 MB Python + a torch install |
| Reference | `<PackageReference Include="JuceSharp.StemSeparation.Onnx" />` | nothing extra — it is in the core package | `<PackageReference Include="JuceSharp.StemSeparation" />` |

Every sample below needs the package in its column's last row; the core
`JuceSharp` package references none of the siblings, and nothing is on a feed
yet (see [Installation](../getting-started/installation.md)).

## In-process ONNX — the default choice

No Python, no child process, no build flag. You supply the `.onnx` model file.

```csharp
using JuceSharp.StemSeparation.Onnx;

using var sep = new OnnxStemSeparator(
    modelPath: @"models\htdemucs.onnx",
    kind: OnnxStemModelKind.Htdemucs,
    preferredExecutionProvider: OnnxExecutionProvider.Auto,
    log: Console.WriteLine);

Console.WriteLine(sep.BackendDescription);
Console.WriteLine(sep.ActiveExecutionProvider);     // what Auto actually picked

IReadOnlyList<OnnxStemBuffer> stems = await sep.SeparateAsync(
    interleavedStereo, sampleRate,
    progress: new Progress<double>(p => bar.Value = p * 100),
    ct: token);

foreach (var s in stems)
    Console.WriteLine($"{s.Name}: {s.InterleavedStereo.Length / 2} frames @ {s.SampleRate}");
```

`OnnxExecutionProvider` is `Auto | DirectML | Cuda | Cpu`; the shipped runtime is
the DirectML build, so `Cuda` falls through to DirectML and then the CPU.
`OnnxSessionFactory.AvailableNativeProviders` tells you what the loaded ONNX
Runtime actually has, `ActiveExecutionProvider` tells you what `Auto`
resolved to and `FallbackReason` why it is not what you asked for — log them,
because a silent fall back to CPU is the usual explanation for "why is this
taking twenty minutes". Pass `deviceId` (from `OnnxGpuAdapters.Enumerate()`) to
choose the GPU; adapter 0 is merely the one driving the main display.

`OnnxStemModelKind` is `Htdemucs | BsRoformer | MdxNet` and selects the
inference driver. BS-RoFormer and MDX-Net **resample the input to 44.1 kHz**
internally; HTDemucs runs at 44.1 kHz natively.

`BsRoformer` covers two export layouts, which `BsRoformerIoBinding.Classify`
tells apart from the graph's declared tensors (anything else is refused with
the shapes in the message):

| Layout | Tensors | Model |
|---|---|---|
| `PackedChannels` | one input `[1, 4, F, T]` (`re_L, im_L, re_R, im_R`), one output `[1, 4 x stems, F, T]` | `bs-roformer-fp16` (2-stem vocals) |
| `SplitComplex` | `spec_real` / `spec_imag` `[1, 2, 1025, T]` in, `out_spec_real` / `out_spec_imag` `[1, 6, 2, 1025, T]` out, DC bin zeroed in the graph; T = 345 (4 s) or 1151 (13.35 s) | `bs-roformer-sw-6s` (BS-RoFormer SW); the training-chunk export from `tools/bs-roformer-sw-export` |

The split-complex geometry is read from the model's custom metadata
(`n_fft`, `hop_length`, `chunk_samples`, `stems`, `sample_rate`) and CHECKED
against the declared shape: the time axis is fixed at trace time (the rotary
embedding caches by length), so the chunk must be exactly the traced one —
176,400 samples (T = 345 at hop 512) for the 4 s third-party file, 588,800
(T = 1151, the model's TRAINING chunk) for the export `tools/bs-roformer-sw-export`
builds (DotCharter #267). Its STFT is `torch.stft(center=True)`'s, which pads by
REFLECTION (`StftPadding.Reflect`); zero padding drops one chunk's per-stem SNR
against the export's reference from 78-129 dB to 8-21 dB. Stems come back in
the model's own order — bass, drums, other, vocals, guitar, piano — not
htdemucs'. Measured 2026-10-05 against the export's `reference.npz` on the CPU
provider: the fp16 file's six output spectrograms are within 2.3e-4 relative L2
of PyTorch's, and one chunk through our STFT / graph / iSTFT is 78-129 dB SNR
against the reference audio. On the CPU it is slow: 20 s of audio took 137-159 s
on six threads at low priority (eight cores). On DirectML (RTX 5070 Ti,
`BsRoformerSwDirectMlTests`, opt-in with `JUCESHARP_BSROFORMER_SW_GPU=1`) the
graph is within 6.6e-6 relative L2 of PyTorch per stem with DirectML graph
fusion OFF, at ORT_ENABLE_ALL and ORT_ENABLE_BASIC alike, and GARBAGE with
fusion on (relative L2 up to 2.4e3) — the same failure htdemucs had, so
`OnnxSessionFactory.DirectMlSessionConfig` (fusion off for every DirectML
session) is what makes SW correct. A whole 224 s song separated in 39–46 s
(4.9–5.7x real time; 32.5 s with #267's driver, Release) and peaked at ~4.2 GB of video memory above idle; its
stems sum to the mix at 33.0 dB, and at -5.1 dB with the fusion setting
removed.

**The split-complex song loop is MSST's `demix`, sample for sample**
(`FadeWindowChunker`): reflect-pad `chunk - step` samples, step = chunk / 2, a
window flat except a linear fade over each tenth (none at the song's start or end),
a short last chunk reflect-padded when it holds more than half a chunk and
zero-padded otherwise. Run through it, the 4 s export's whole-song stems agree with
PyTorch's own 4 s demix at 83-101 dB per stem, and the training-chunk export's fp32
file with PyTorch's 13.35 s demix at 85-102 dB (the fp16 file: 50-61 dB, PyTorch's own
autocast 56-69 dB). The packed layout and MDX-Net keep `OverlapAddChunker`.

**Three stages overlap** (`ChunkPipeline`, `SplitComplexIoBinding`): while the model
runs chunk k (inputs and outputs bound to two slots of arrays allocated once per
song), a pool thread unpacks chunk k-1 and runs its twelve iSTFTs and the
overlap-add, and another fills chunk k+1 and runs its forward STFT. Each song logs
its chunk count, model-stage and host-stage time and the non-finite samples the
scrub silenced (`BsRoformerInference.LastSplitTiming`).

**DirectML graph fusion is per model.** A file whose metadata carries
`dml_graph_fusion=verified` keeps it on (`OnnxSessionFactory.DirectMlGraphFusionKey`);
every other DirectML session has it off, as #151 requires. The training-chunk export
carries the key because its band split and mask estimators run as 7 batched groups
instead of 62 per-band networks: fusion miscomputes the per-band 62-way `Concat`,
which is what broke the 4 s export.

`Dispose()` drains an in-flight inference before freeing the session, so
disposing while `SeparateAsync` is running is safe.

## Native demucs.cpp — only if you build it in

Its weights have a catalogue of their own since 0.9 — see
[`DemucsGgmlModelCatalog`](#getting-a-ggml-weight-demucsggmlmodelcatalog).

```csharp
using JuceSharp;

if (JuceStemSeparation.IsSupported)
{
    Console.WriteLine(JuceStemSeparation.DescribeBackend());
    await JuceStemSeparation.SeparateAsync(
        inputAudioPath, outputDirectory, modelPath,
        numThreads: 0,
        onProgress: (fraction, stage) => Console.WriteLine($"{stage} {fraction:P0}"),
        cancellationToken: token);
}
```

> ⚠️ **Warning**
> **`IsSupported` is `false` in a stock build.** The demucs.cpp inference is
> compiled in only with `-DDOTCHARTER_STEMS_DEMUCS_CPP=ON`, which is **off by
> default**. `Separate` throws `NotSupportedException` when it is not compiled
> in — a CAPABILITY answer a caller can branch on to try the next backend,
> deliberately a different type from the `JuceAudioException` a failed native
> call throws. Always branch on `IsSupported` — never assume.
>
> The progress callback is invoked **on the separation thread** — the thread
> that called `dca_stems_separate_file2`, i.e. the dedicated background thread
> `SeparateAsync` starts. Not the message thread and not yours. Keep it
> non-blocking, and it must not throw.
>
> **The `cancellationToken` above DOES abort a separation that has started**,
> since ABI v36. The native path is `dca_stems_separate_file2`, whose progress
> callback answers `0` to stop; the separator checks between the coarse phases
> and between the per-target WAV writes. Inference itself takes no callback, so
> a cancel arriving during it lands when inference returns — a bound on latency,
> not an instant stop. Stems already written are LEFT on disk.

## Python sidecar — the widest model support

An embedded CPython 3.11.9 child process running torch, demucs and
bs_roformer. It is the only backend that can load an arbitrary demucs model by
name and the only one where a crash inside torch is contained.

```csharp
using JuceSharp.StemSeparation.Processing;

IStemSeparator sep = new StemSeparator();
if (!sep.IsReady())
{
    var status = sep.Probe();      // PythonEnvStatus: interpreter? pip? torch?
    // First run downloads ~25 MB of Python and then a torch install.
}

SeparationResult result = await sep.SeparateTrackAsync(
    inputPath, outputDir,
    new StemSeparationOptions { Model = "htdemucs_ft", Device = "auto" },
    progress, ct);
```

Full detail — phases, the exception hierarchy, options, concurrency, where the
files land — is in
[the StemSeparation developer guide](../StemSeparation.md).

> ⚠️ **Warning**
> **A cancelled separation throws `OperationCanceledException`, never
> `StemSeparationException`.** If you are chaining backends, do not treat a
> user Cancel as a reason to fall through to the next one.

## Streaming progress (`await foreach`)

Every backend also publishes its progress as an `IAsyncEnumerable<T>` you pull
instead of a callback you are pushed into. **Prefer it.** `IProgress<T>` lets
the producer set the rate and gives the consumer no way to say "I am behind",
which is why every consumer grew its own throttle; the stream moves that
decision into the transport, once.

```csharp
IStemSeparator sep = new StemSeparator();

await foreach (var p in sep.SeparateWithProgressAsync(inputPath, outputDir, options, ct))
{
    bar.Value = p.Fraction >= 0 ? p.Fraction * 100 : bar.Value;
    status.Text = p.Message;

    if (p.Result is not null)          // the terminal item, exactly once
        Commit(p.Result);              // SeparationResult, with every stem
}
```

The shape is the same for all three, and so is the contract:

| Backend | Call | Item | Terminal payload |
|---|---|---|---|
| `StemSeparator` (and any `IStemSeparator`) | `SeparateWithProgressAsync(input, outDir, options, ct)` | `StemSeparationProgress` | `Result` → `SeparationResult` |
| `OnnxStemSeparator` | `SeparateWithProgressAsync(pcm, sampleRate, ct)` | `OnnxStemProgress` | `Result` → `IReadOnlyList<OnnxStemBuffer>` |
| `JuceStemSeparation` | `SeparateWithProgressAsync(input, outDir, modelPath, threads, ct)` | `JuceStemProgress` | none — the native call writes FILES and returns a status, so there is nothing to hand back |

* **Items arrive in order**, each stamped with a `Backend` id
  (`"python-sidecar"`, `"onnx"`, `"demucs.cpp"`) so a host that merges several
  backends' streams can tell them apart.
* **The last item carries the result.** That is the shape a UI binds to with
  the least ceremony — one sequence, no second `await` to forget — and it is
  also what makes the coalescing below lossless.
* **A slow consumer never stalls inference.** Publication is a non-blocking
  write into a bounded, drop-OLDEST channel
  (`CoalescingProgressStream`, default depth 32), so falling behind costs you
  stale interim samples and nothing else. The terminal item is by construction
  the newest write, so it can never be the one dropped — and since it carries
  the complete result, no information is lost even if every interim sample is.
  If you bridge an `IProgress<T>` callback into that stream yourself, use
  `JuceSharp.ImmediateProgress<T>`, which invokes the handler on the reporting
  thread: `Progress<T>` posts to the captured `SynchronizationContext` and
  would let a sample arrive after the terminal item.
* **Cancelling the token, or abandoning the enumeration** (`break` out of the
  `await foreach`), cancels the separation and surfaces
  `OperationCanceledException` from `MoveNextAsync` — never a
  `StemSeparationException`.
* **A backend fault keeps its own exception type.** A
  `StemSeparationVramException` arrives as a `StemSeparationVramException`, so
  a "retry on CPU" branch still fires.
* Each sequence is **single-shot**, and the work does not start until you
  enumerate.

The `IProgress<T>` overloads are unchanged and are not going away; a backend
that only implements `SeparateTrackAsync` still gets a stream, because
`IStemSeparator.SeparateWithProgressAsync` has a default implementation that
bridges it.

## Getting a model: `OnnxModelCatalog`

`OnnxStemSeparator` takes a path to an `.onnx` file and does not care where it
came from. `OnnxModelCatalog` is the "where it came from" half: a table of
known models with **pinned SHA-256 hashes**, and a downloader that verifies
before it installs.

```csharp
using JuceSharp.StemSeparation.Onnx;

var path = await OnnxModelCatalog.EnsureAsync(
    OnnxStemModelKind.Htdemucs,
    targetDir: modelsDir,
    progress: new Progress<DownloadProgress>(p =>
        bar.Value = p.Fraction >= 0 ? p.Fraction * 100 : bar.Value),
    ct: token);

using var sep = new OnnxStemSeparator(path, OnnxStemModelKind.Htdemucs);
```

What `EnsureAsync` guarantees:

* **The file is checked, never trusted.** Already present and hashing
  correctly ⇒ nothing is downloaded. Already present and hashing WRONG ⇒ it is
  discarded and fetched again. A half-written model from a killed process is
  the common case, and treating "the file exists" as "the model is installed"
  is how that turns into a permanent, mystifying load failure.
* **The hash is checked before the rename, and on the COMPLETE file.** The
  destination path never comes into existence holding bytes nobody
  authenticated — the next thing that happens to them is ONNX Runtime loading
  them.
* **Interrupted downloads resume.** The transfer goes to `<model>.part`; a
  cancel keeps it and the next call continues with an HTTP `Range` request. A
  server that ignores `Range` answers `200` with the whole body, and that is
  detected from the STATUS CODE (`206` vs `200`), not from the advertised
  `Accept-Ranges` header — appending a whole body to a prefix would corrupt the
  file silently. A resumed download is still verified whole, because the bytes
  it kept came from an earlier unverified attempt.
* **Cancellation is `OperationCanceledException`,** and it never falls through
  to the next mirror — a user Cancel is not a broken mirror.

`VerifyAsync(kind, path)` answers the same question on demand, and
`ComputeSha256Async(path)` is there for pinning a model you obtained some other
way. An entry with **no** pinned hash (`Sha256 = null`) is REFUSED unless you
pass `allowUnverified: true`; the warning is logged either way.

### The catalogue

Every hash below was read from the Hugging Face repository API on 2026-09-11,
where an LFS object's `oid` **is** its SHA-256 — the same value the CDN serves
and the same value `git lfs` verifies on checkout. Each entry records its own
provenance in `HashSource`, and `OnnxModelCatalogTests` fails the build if a
pin ever appears without one.

| Id | Kind | File | Size | Stems | Hash source |
|---|---|---|---|---|---|
| `htdemucs-6s-onnx` (default for `Htdemucs`) | HTDemucs | `htdemucs_6s.onnx` | 114,559,139 | 6 | `arjune123/demucs-onnx` LFS oid — **and** identical to the hash DotCharter pinned independently from an installed copy on 2026-09-04 |
| `htdemucs-4s-onnx` | HTDemucs | `htdemucs.onnx` | 302,899,058 | 4 | `MrCitron/demucs-v4-onnx` LFS oid |
| `htdemucs-ft-onnx` | HTDemucs | `htdemucs_ft.onnx` | 302,899,058 | 4 | `MrCitron/demucs-v4-onnx` LFS oid |
| `bs-roformer-fp16` (default for `BsRoformer`) | BS-RoFormer | `bs_roformer_fp16.onnx` | 324,761,627 | 2 | `safescribeai/bs-roformer-onnx-fp16` LFS oid |
| `uvr-mdx-net-inst-hq-3` (default for `MdxNet`) | MDX-Net | `UVR-MDX-NET-Inst_HQ_3.onnx` | 66,759,214 | 2 | LFS oid from `seanghay/uvr_models` **and** `Politrees/UVR_resources`; the two agree |
| `uvr-mdx-net-voc-ft` | MDX-Net | `UVR-MDX-NET-Voc_FT.onnx` | 66,762,490 | 2 | `Politrees/UVR_resources` LFS oid |
| `uvr-mdx-net-kim-vocal-2` | MDX-Net | `Kim_Vocal_2.onnx` | 66,759,214 | 2 | LFS oid from both re-hosts; the two agree |
| `uvr-mdx-net-kim-inst` | MDX-Net | `Kim_Inst.onnx` | 66,759,214 | 2 | LFS oid from both re-hosts; the two agree |
| `uvr-mdx-net-inst-main` | MDX-Net | `UVR-MDX-NET-Inst_Main.onnx` | 52,786,726 | 2 | `seanghay/uvr_models` LFS oid |
| `bs-roformer-sw-6s` | BS-RoFormer (split-complex) | `bs_roformer_sw_6stem_fp16.onnx` | 352,778,874 | 6 | `elicwhite/bs-roformer-sw-6stem-onnx` LFS oid, read 2026-10-05, and a download that day hashed the same |

> ⚠️ **Warning**
> **Read `LicenceNote` before you bundle one of these.** The Demucs v4 weights
> are MIT (facebookresearch/demucs), but every entry here is a THIRD-PARTY
> ONNX re-host, and the UVR and BS-RoFormer weights are community models with
> no explicit redistribution licence. Downloading one on a user's behalf at
> runtime is not the same question as shipping it in an installer.
> **`bs-roformer-sw-6s`'s weights have NO known licence at all**: the trainer is
> unknown, the original re-host (`jarredou/BS-ROFO-SW-Fixed`) is deleted and
> stated none, and the MIT licence on `elicwhite/bs-roformer-sw-6stem-onnx`
> covers its export code only. That ONNX file is a single re-host, which is why
> it is pinned; if it disappears, the PyTorch checkpoint survives at
> `enerjazzer/BS-ROFO-SW-Fixed` and `elicwhite/bs-roformer-web`'s
> `python/export_onnx.py` rebuilds the graph from it (with different bytes, so a
> new pin).

## Getting a ggml weight: `DemucsGgmlModelCatalog`

`OnnxModelCatalog` is ONNX-only, so the weights the **native demucs.cpp
backend** loads had no catalogue at all until 0.9 — a host running that backend
carried its own table of file names, URLs and sizes.
`DemucsGgmlModelCatalog` (namespace `JuceSharp.StemSeparation.Runtime`) is that
table, with the same guarantees as the ONNX one because it is the same
downloader underneath.

> **Note the package boundary.** It fetches weights for the *native* backend but
> **ships in `JuceSharp.StemSeparation`** — the Python-sidecar package
> (`src/JuceSharp.StemSeparation/Runtime/`), which is where the verified
> downloader lives. So does `VerifiedDownload` below. Reference that package
> even if the only backend you use is demucs.cpp; it is `any`-platform and pulls
> in no Python by itself.

```csharp
using JuceSharp.StemSeparation.Runtime;

var entry = DemucsGgmlModelCatalog.FindById("ggml-htdemucs-6s")
            ?? DemucsGgmlModelCatalog.Default;

var path = await DemucsGgmlModelCatalog.EnsureAsync(
    entry, targetDir: modelsDir,
    progress: new Progress<DownloadProgress>(p => bar.Value = p.Fraction * 100),
    ct: token);
```

Every hash below is the `lfs.oid` published by
`huggingface.co/api/datasets/Retrobear/demucs.cpp/tree/main`, read on
2026-09-11. An LFS object's oid **is** its SHA-256 — the same value the CDN
serves and the same value `git lfs` verifies on checkout — which is what makes
it a pin rather than a second download agreeing with the first.

| Id | File | Size | Sources | SHA-256 |
|---|---|---|---|---|
| `ggml-htdemucs-4s` (`Default`) | `ggml-model-htdemucs-4s-f16.bin` | 83,994,361 | 4 | `72b17c42…f6df0064` |
| `ggml-htdemucs-6s` | `ggml-model-htdemucs-6s-f16.bin` | 54,855,129 | 6 | `09704f4c…abcee856` |
| `ggml-hdemucs-mmi-v3` | `ggml-model-hdemucs_mmi-v3-f16.bin` | 167,294,211 | 4 | `f72a3b8e…cb6b35a4` |

The 6-source pin is corroborated: it agrees byte for byte with a hash DotCharter
computed independently, on 2026-09-04, from a copy it had been separating with —
a file validated by USE rather than by a second fetch over the same channel.

> ℹ️ **Note**
> If you have seen a note claiming these files carry no published oid because
> the repository is a Hugging Face **dataset**, it was reading the wrong
> endpoint: dataset repos publish `lfs.oid` under `api/datasets/`, not
> `api/models/`. The same note led to two of the three weights shipping
> unpinned, and to the 4-source row being described as "~80 MB" when only an
> EXACT size is an integrity check.

The repository also holds the four `ggml-model-htdemucs_ft_*-4s-f16.bin`
fine-tuned weights (83,994,361 bytes each, oids published the same way). They
are deliberately not in the table: demucs.cpp uses that family as a SET of four,
and a catalogue entry a host could install alone would invite using one alone.

## Downloading anything else: `VerifiedDownload`

Neither catalogue fits a `.ckpt` a user typed the URL for, a model from a table
you maintain yourself, or a weight format JuceSharp has never heard of. That
does not mean writing a second downloader — the thing being fetched is about to
be executed or loaded as a model either way.

`VerifiedDownload` (namespace `JuceSharp.StemSeparation.Runtime`) is the
**kind-less** door: one URL, one destination, no model kind and no catalogue
policy.

```csharp
var path = await VerifiedDownload.EnsureFileAsync(
    new Uri("https://example.org/weights.ckpt"),
    destinationPath: Path.Combine(modelsDir, "weights.ckpt"),
    sha256: knownHash,          // null = unverified; it warns through `log`
    expectedBytes: knownSize,   // null = unknown
    progress: new Progress<DownloadProgress>(Report),
    log: Console.WriteLine,
    ct: token);

if (await VerifiedDownload.VerifyFileAsync(path, knownHash, knownSize, token)) { … }
```

It is the same code the catalogues run — `OnnxModelCatalog.EnsureAsync` calls
`EnsureFileAsync` once per mirror — so the resume policy, the
status-code-not-header resume decision, the length check and the whole-file hash
before the atomic rename are identical down every path. What the CATALOGUES add
on top is exactly two things: the `allowUnverified` decision, and walking a
mirror list.

> ⚠️ **Warning**
> **Do not describe a file by a model kind it is not** in order to reach a
> downloader. `OnnxModelEntry.Kind` names the inference driver that will consume
> the file, and nothing downstream of `EnsureAsync` reads it — so a ggml weight
> given "the nearest ONNX label" downloads perfectly and leaves a field that
> lies to the next reader. That is what `EnsureFileAsync` exists to stop.

With no hash, a pinned **length** is the whole of the evidence, and
`EnsureFileAsync` uses it on an EXISTING file too: right length ⇒ kept with a
warning naming what was and was not checked; wrong length ⇒ discarded and
fetched again. With neither a hash nor a size there is nothing to check, so the
file is kept and the warning says so.

## Converting a checkpoint to ONNX

`JuceSharp.StemSeparation` also ships `CkptToOnnxConverter`, which uses the
same Python environment to turn a `.ckpt` (plus, for RoFormer, its YAML
config) into an `.onnx` the in-process backend can load:

```csharp
var conv = new CkptToOnnxConverter();
await conv.ConvertAsync(ckptPath, yamlPath, onnxOutputPath, progress, ct);
```

It reports through `PythonInstallProgress` and adds one phase of its own,
`convert`. Its own `RequiredPackages` list is smaller than `StemSeparator`'s —
torch, numpy, pyyaml, onnx, bs_roformer — so `IsReady()` on the converter and
on the separator are different questions.

## A pragmatic recommendation

1. Ship `OnnxStemSeparator` with a bundled HTDemucs ONNX model. It has no
   install step and it is the only one that will work on an end user's machine
   out of the box.
2. Fall back to `StemSeparator` only if you need a model that has no ONNX
   export, and put the first-run download behind an explicit user prompt —
   `IsReady()` is the cheap gate for that.
3. Reach for `JuceStemSeparation` only if you already control the CMake build
   and want the separation inside the audio process.

## Command-line

`juce-stems-diag` (the `JuceSharp.Diagnostics` project) drives the Python
sidecar from a shell — useful for reproducing a failure without your app in
the way. It consumes the progress STREAM above, which is why it no longer
carries a progress throttle of its own:

```
juce-stems-diag --input song.wav --out stems --model htdemucs --device auto
```

| Flag | Default |
|---|---|
| `--input <path>` | required |
| `--out <dir>` | `./stems-out` |
| `--model <name>` | `htdemucs` |
| `--device <auto\|cuda\|cpu>` | `auto` |
| `--shifts <int>` | `1` |
| `--overlap <float>` | `0.25` |
| `--max-concurrent <int>` | `1` |
| `--model-path <path>` | — |
| `--config <path>` | — |
| `--stems <a,b,…>` | all the model produces |
| `--timeout <minutes>` | `240` |
| `--skip-bootstrap` | off |
| `--quiet` | off |
| `--probe` | report the Python environment status and exit |
| `--version` | print the package and native versions and exit |
| `--help`, `-h` | — |

Exit codes:

| Code | Meaning |
|---|---|
| `0` | success, or help shown |
| `2` | bad arguments |
| `3` | input file not found |
| `4` | output directory could not be created |
| `10` | VRAM exhausted (`StemSeparationVramException`) |
| `11` | any other separation failure |
| `12` | environment bootstrap failed (download, pip, interpreter) |
| `130` | cancelled (Ctrl+C is wired) |

The codes are the public `ExitCodes` constants in `JuceSharp.Diagnostics`.

It also packs as a `dotnet tool` (`JuceSharp.Diagnostics`, command `juce-stems-diag`); or run it from the build
output or `dotnet run --project src/JuceSharp.Diagnostics`.
