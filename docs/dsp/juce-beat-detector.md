# JuceBeatDetector

Offline BPM, beat, tempo-map and downbeat extractor. Pipeline:

1. **Spectral flux** onset envelope per FFT frame.
2. **Moving-average** smoothing (~50 ms span).
3. **Autocorrelation** over 60–200 BPM by default, with parabolic
   interpolation → the global `Bpm`.
4. **Windowed autocorrelation** → a LOCAL period per frame.
5. **Dynamic-programming beat tracker** (Ellis) over that local period → `Beats`.
6. **Segmentation** of the inter-beat intervals → `TempoMap`, and
   beat-synchronous accent autocorrelation → `Downbeats` / `BeatsPerBar`.

```csharp
var det = new JuceBeatDetector(sampleRate);
var r   = det.Analyze(monoSamples);
Console.WriteLine($"{r.Bpm:F1} BPM, {r.Beats.Count} beats, {r.TempoMap.Count} tempo segments");
foreach (var seg in r.TempoMap)
    Console.WriteLine($"  {seg.Time:mm\\:ss\\.fff}  {seg.Bpm:F1} BPM");
```

> ⚠️ Click-track inputs can produce octave-equivalent BPM (60 vs 120,
> 75 vs 150 …). Cross-check with the metronome you expect, or with
> the imported PSARC tempo if available.

## Behaviour change in 0.9

`Beats` used to be a **fixed grid**: one global period stepped from the first
half-beat to the end, each step snapped to the nearest envelope maximum within
±period/4. That cannot represent a song that speeds up — every beat past a
tempo change drifted. It is now the output of the tracker.

Set `Options = new JuceBeatDetector.AnalysisOptions { FixedGrid = true }` to get
the old grid back, bit for bit. `Bpm` is the same global figure in both modes
and did **not** change. In fixed-grid mode `TempoMap` holds the single global
tempo and `Downbeats` is empty — a grid carries no evidence about meter.

## Resolution

A tempo map is only as fine as its envelope. One frame of inter-beat interval
is `bpm² / (60 · fps)` BPM wide, where `fps = SampleRate / HopSize`:

| `HopSize` @ 22.05 kHz | fps | resolution at 120 BPM |
|---|---|---|
| 512 (default) | 43 | 5.6 BPM |
| 128 | 172 | 1.4 BPM |
| 64 | 345 | 0.70 BPM |

`SegmentToleranceBpm` is therefore **floored** by `1.5 ×` that resolution: asking
for 2 BPM segmentation at the default hop would segment on quantisation noise
alone. Halve `HopSize` to buy real precision. A segment's tempo is measured over
its whole span, never from one interval, which is what makes the reported number
finer than the frame grid.

## API

| Member | Description |
|--------|-------------|
| `JuceBeatDetector(int sampleRate)` | |
| `int FftSize { get; init; } = 1024` | |
| `int HopSize { get; init; } = 512` | |
| `float MinBpm / MaxBpm { get; init; }` | Search range. |
| `AnalysisOptions Options { get; init; }` | `FixedGrid` (default `false`), `SegmentToleranceBpm` (default 2). |
| `long NonFiniteSamples` | NaN/Inf samples scrubbed at the input. |
| `Result Analyze(ReadOnlySpan<float> mono)` | Throws if input has < 4 windows. |

`Result` is
`(float Bpm, IReadOnlyList<TimeSpan> Beats, IReadOnlyList<JuceTempoSegment> TempoMap, IReadOnlyList<TimeSpan> Downbeats, int BeatsPerBar, float Confidence)`;
the last **four** are optional constructor parameters, so `new Result(bpm, beats)`
still compiles and yields empty collections.

`Confidence` (0…1, default `0f`) is how sure the tracker is. Audio with no
beat now returns `Bpm` 0, empty beats **and** `Confidence == 0` rather than
throwing; a genuine detection is well above zero. Treat a low value as "this
audio may not have a beat", never as an error.

`JuceTempoSegment` is `readonly record struct (TimeSpan Time, float Bpm)`.
`BeatsPerBar` is 3, 4, or 0 when no meter was judged (fixed-grid mode, or fewer
than eight beats).

## Why the tracker is not greedy

The DP scores every frame as "the best cumulative evidence for a beat here":
onset strength plus the best predecessor in `[t - 2p, t - p/2]`, penalised by
`-100 · ln(interval / p)²`. Backtracking from the best score in the final period
gives a globally optimal sequence, which is what lets it ride through a bar with
no onsets at all. The **local** period is what makes a ramp trackable: against a
single global period the transition cost fights the ramp from both ends.

Two guards keep the ends honest:

* the local autocorrelation is weighted by a log-Gaussian prior around the global
  lag (σ = 0.7 octaves), or a window routinely locks onto half or double the
  tempo and the map then reports an octave error as a tempo change;
* beats at either **end** of the chain that sit on no onset at all are dropped.
  The DP has no predecessor for the first half-period, so every silent lead-in
  frame ties and the backtrack lands its first beat on an arbitrary one. One
  phantom beat is invisible in the beat list and loud in the tempo map — it is a
  whole spurious segment. Interior beats are never trimmed.
