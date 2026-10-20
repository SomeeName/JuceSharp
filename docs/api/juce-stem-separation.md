# JuceStemSeparation

`static class JuceStemSeparation` — in the **core `JuceSharp`** package.

The managed entry point for the **native** demucs.cpp stem separation exposed
by `juce_sharp.dll`, wrapping the `dca_stems_*` C ABI (added at v5).

> ⚠️ **Warning**
> **`IsSupported` is `false` in a stock build.** The inference is compiled in
> only with `-DDOTCHARTER_STEMS_DEMUCS_CPP=ON`, and that option defaults to
> **OFF**. `Separate` throws `NotSupportedException` when the backend is not
> present — a CAPABILITY answer, deliberately a different type from the
> `JuceAudioException` a failed native call throws, so a composite service can
> tell "try the next backend" from "this backend broke". Always branch on
> `IsSupported`.

## Members

| Member | Description |
|---|---|
| `static bool IsSupported` | Whether a native backend was compiled in. Cached. |
| `static string DescribeBackend()` | Human-readable backend description. Reports that it could not identify the backend rather than falling through to a hard-coded `"demucs.cpp"` when the native query fails. |
| `static void Separate(string inputAudioPath, string outputDirectory, string modelPath, int numThreads = 0, Action<float, string>? onProgress = null)` | Blocking separation. `numThreads = 0` lets the backend choose. All three paths are checked for null/whitespace first — before the `NotSupportedException` capability check, so a typo is never reported as "no backend". Forwards to the overload below with `default`. |
| `static void Separate(string inputAudioPath, string outputDirectory, string modelPath, int numThreads, Action<float, string>? onProgress, CancellationToken cancellationToken)` | The same, **cancellable inside the native call** (ABI v36). Every other entry point resolves here; it is the only route to `dca_stems_separate_file2`. Throws `OperationCanceledException` when the run was stopped. |
| `static Task SeparateAsync(string inputAudioPath, string outputDirectory, string modelPath, int numThreads = 0, Action<float, string>? onProgress = null, CancellationToken cancellationToken = default)` | The same, on a DEDICATED background thread — never the thread pool. A separation is minutes, and a parked pool thread makes the pool inject one every ~0.5 s. |
| `static IAsyncEnumerable<JuceStemProgress> SeparateWithProgressAsync(string inputAudioPath, string outputDirectory, string modelPath, int numThreads = 0, CancellationToken cancellationToken = default)` | The same again, PULL progress: phases `loading` → `inference`… → `done`. The native callback writes straight into the (thread-safe, non-blocking) progress channel, so a slow consumer cannot back-pressure the demucs worker thread. See [Streaming progress](../guides/stem-backends.md#streaming-progress-await-foreach). |
| `const string BackendId` | `"demucs.cpp"` — stamped on every streamed sample. |

`JuceStemProgress`: `readonly record struct (string Phase, double Fraction, string Message, string Backend)`. It carries no result object on purpose — `dca_stems_separate_file` writes FILES and returns a status, so there is nothing for a terminal item to hand back.

## Example

```csharp
using JuceSharp;

if (!JuceStemSeparation.IsSupported)
{
    Console.Error.WriteLine("Native stem separation was not compiled into this DLL.");
    return;
}

Console.WriteLine(JuceStemSeparation.DescribeBackend());

await JuceStemSeparation.SeparateAsync(
    inputAudioPath:  @"C:\music\song.wav",
    outputDirectory: @"C:\music\song-stems",
    modelPath:       @"C:\models\htdemucs",
    numThreads:      0,
    onProgress:      (fraction, stage) => Console.WriteLine($"{stage}  {fraction:P0}"),
    cancellationToken: token);
```

## Errors

* **`NotSupportedException`** when `IsSupported` is `false` — a CAPABILITY
  answer, deliberately its own type so a composite service can fall through to
  the ONNX or Python backend.
* **`JuceAudioException`** when the native call returns a non-`Ok` status; its
  `Status` carries the typed `JuceAudioStatus` (`Decode` = a file this build
  cannot read, `Internal` = the inference failed), so you never have to parse a
  message string.
* **`FileNotFoundException`** when `modelPath` does not exist. Deliberately
  distinct from `NotSupportedException`: native has one status for both "the
  weights are not on disk" and "no backend was compiled in", and the two answers
  are a DOWNLOAD (`DemucsGgmlModelCatalog.EnsureAsync`) and a rebuild.
* `OperationCanceledException` from `Separate(…, CancellationToken)`,
  `SeparateAsync` and `SeparateWithProgressAsync` — both when the token was
  already cancelled and when the native run was stopped by it.

> **Cancellation reaches the native run** (ABI v36).
> The path taken is `dca_stems_separate_file2`, whose progress callback answers
> `0` to stop; the separator checks between the coarse phases and between the
> per-target WAV writes. Inference itself is demucs.cpp's own threaded call and
> takes no callback, so a cancel arriving during it lands when inference
> returns — a real bound on latency, not an instant stop.
>
> ⚠️ **Stems already written are LEFT on disk.** Deleting a partial set is the
> caller's decision, so any cache that treats "the output directory has WAVs in
> it" as "this song is separated" must not be keyed on that alone.

## Notes

> ⚠️ **Warning**
> **`onProgress` is invoked on the thread that called
> `dca_stems_separate_file2`** — the dedicated background thread behind
> `SeparateAsync`, never the message thread and never your UI thread. (It is
> *not* a demucs worker: every checked point is in the separator's own code, and
> demucs.cpp's threaded inference takes no progress hook.)
> Do not block in it, and do not let it throw — a
> reverse-P/Invoke that unwinds into C++ frames kills the process. Marshal to
> your UI thread yourself.

* This runs **in your process**. Unlike the Python sidecar there is no crash
  isolation: a fault inside the inference takes the host down with it.
* It is one of three stem-separation backends. See
  [Choosing a Stem-Separation Backend](../guides/stem-backends.md) before
  committing to it.
