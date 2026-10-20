# Waveform Generation

JuceSharp can pre-render the per-block peak amplitudes of a file for
display in waveform UI widgets, without you having to decode the file
yourself.

## One-shot pre-render

Both entry points are **static** on `JuceAudioEngine` — no engine instance and
no open audio device are needed.

```csharp
float[] peaks = JuceAudioEngine.GenerateWaveform(@"C:\Music\track.wav",
                                                 blockMs: 10.0);

byte[] blob  = File.ReadAllBytes(@"C:\Music\track.wav");
float[] same = JuceAudioEngine.GenerateWaveformFromMemory(blob, blockMs: 10.0);
```

Each entry is the peak absolute amplitude (`0..1`) over its block. The block
width is given in **milliseconds**, not samples, and there is no channel
selector: the result is a mixdown.

`GenerateWaveformFromMemory` (ABI v20) exists so callers that already hold the
file bytes — a PSARC or an archive entry — can skip a second end-to-end disk
read. The blob is only read for the duration of the call.

> ℹ️ **Note**
> **`blockMs` is a `double` on purpose, and must stay one.** An integer
> samples-per-block is exact at 48 kHz and drifts **+408 ms over three
> minutes** at 44.1 kHz, because the block width no longer divides the rate.
> Fractional milliseconds keep the peak grid aligned to wall time at any rate.

## Picking a block width

| Display target              | Suggested `blockMs`                      |
| --------------------------- | ---------------------------------------- |
| Full-song minimap (200 px)  | `durationSeconds * 1000 / 200`           |
| Editor view (2000 px)       | `durationSeconds * 1000 / 2000`          |
| Fine detail (zoom-in)       | `5` – `20`                               |

For a 3-minute file at 10 ms you get 18,000 floats (~70 KB) — small enough to
keep entirely in memory and feed to any rendering pipeline. Generation is
roughly linear with file size and runs on the calling thread — wrap with
`Task.Run` if you don't want to block the UI thread.

## Peaks from a buffer you already have

If the samples are already in memory (you decoded them, or they came out of a
stem separator), use the managed builder instead — it gives min/max buckets
rather than a single absolute peak, which reads better at low zoom:

`JuceWaveformPeaks` is in the **separate `JuceSharp.Dsp` package** — the core
`JuceSharp` package does not reference it, so add it first or this will not
compile:

```xml
<PackageReference Include="JuceSharp.Dsp" />
```

```csharp
using JuceSharp.Dsp;

JuceWaveformPeak[] buckets = JuceWaveformPeaks.Build(
    samples, channels: 2, sampleRate: 48_000, bucketMilliseconds: 10.0);
```

## In-place updates

Re-call `GenerateWaveform` whenever the source file changes. The native
side does not cache — it's the caller's job to memoize if needed.
