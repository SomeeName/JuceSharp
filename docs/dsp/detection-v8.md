# Detection v8 — CQT, Chart Verifier & Lead Tracker

The chart-verification stack: a streaming constant-Q transform with a live
noise floor, a verifier that answers *"was this notated note actually
played?"*, a lead tracker for bends and slides, and the one type that holds the
shipping constants.

> ⚠️ **Warning**
> **`LiveCqtConfig` is the single source of truth for the shipping
> configuration.** Never re-type one of its constants into a caller or a
> harness. That is exactly what drifted once, and the evaluation then reported
> a pre-veto number as current. Change `LiveCqtConfig` and let every caller
> follow.

## `LiveCqtConfig`

`static class` — every value verified against the source.

| Member | Value | Meaning |
|---|---|---|
| `MinMidi` | `28` | Lowest MIDI note the transform covers. |
| `BinCount` | `144` | Bins above `MinMidi`. |
| `Hop` | `256` | Samples between frames. |
| `MaxKernelSamples` | `4096` | Kernel cap — bounds streaming latency. |
| `NoiseMargin` | `3f` | How far above the floor a bin must sit to count. |
| `SubOctaveVeto` | `0.5` | Veto strength for the octave below. |
| `SubOctaveBins` | `26` | Bins allocated *below* `MinMidi` so the veto has somewhere to look. |
| `NoiseHistorySeconds` | `10.0` | Noise-floor horizon. |
| `FloorPercentile` | `1.0f` | Percentile used for the floor. |
| `HarmonicPeakBelowHz` | `0.0` | Off. |
| `HarmonicComb` | `0` | Off. |
| `NoiseHistoryFrames(int sampleRate)` | method | `NoiseHistorySeconds * sampleRate / Hop`. |
| `RelFloor` | `0.10f` | Relative floor below the frame peak. |
| `RingFrames` | `384` | Streaming ring depth — the default `JuceConstantQ.Reset` uses. |
| `NoisePercentile` | `0.05f` | Percentile of the history used as the noise estimate. |

## `JuceConstantQ`

`sealed class` — a streaming CQT with a rotating frame ring.

```csharp
var cqt = new JuceConstantQ(
    sampleRate:         44_100,
    hop:                LiveCqtConfig.Hop,
    fMinMidi:           LiveCqtConfig.MinMidi,
    binsPerOctave:      24,
    binCount:           LiveCqtConfig.BinCount,
    relFloor:           LiveCqtConfig.RelFloor,
    maxKernelSamples:   LiveCqtConfig.MaxKernelSamples,
    floorPercentile:    LiveCqtConfig.FloorPercentile,
    noiseHistoryFrames: LiveCqtConfig.NoiseHistoryFrames(44_100),
    subOctaveBins:      LiveCqtConfig.SubOctaveBins);
```

| Member | Description |
|---|---|
| `SampleRate`, `Hop`, `BinsPerOctave`, `BinCount`, `FMinMidi`, `FMinHz`, `RelFloor`, `Step`, `MaxKernelSamples`, `FloorPercentile`, `NoiseHistoryFrames`, `SubOctaveBins` | The configuration, read back. |
| `LowestBin` | `-SubOctaveBins` — bin indices go **negative** below `FMinMidi`. |
| `int BinForMidi(double midi)` | |
| `long FrameOfSeconds(double seconds)` | |
| `long OldestFrame` / `NewestFrame` | The ring's live window (streaming mode). |
| `void AnalyzeWhole(ReadOnlySpan<float> y)` | Offline: analyse a whole buffer. |
| `void Reset(long streamPosition = 0, int ringFrames = LiveCqtConfig.RingFrames)` | Enter streaming mode. The default is 384 — take it from `LiveCqtConfig.RingFrames`, never by re-typing the number. |
| `void Push(ReadOnlySpan<float> samples, long streamPosition)` | Feed the stream. |
| `float Magnitude(int bin, long frame)` | |
| `float Floor(int bin)` | The learned noise floor for a bin. |
| `float FrameEnergy(long frame)` | |
| `float NoiseFloorEnergy()` | |
| `long DiscontinuityResets` | Stream-position jumps that forced a reset. |
| `long NonFiniteSamples` | NaN/Inf scrubbed at the input. **Non-zero means a broken producer.** |

## `JuceChartVerifier`

`sealed class` — constructed over a `JuceConstantQ` and asked about one
notated note at one time.

| Member | Default | Description |
|---|---|---|
| `JuceChartVerifier(JuceConstantQ cqt)` | | |
| `PreRatio` | `0.0` | |
| `PostWindow` | `(0.04, 0.14)` | Seconds before/after the note for the post window. |
| `PreWindow` | `(0.18, 0.05)` | Seconds before/after for the pre window. |
| `HarmonicSupport` | `false` | |
| `NoiseMargin` | `3f` | Set from `LiveCqtConfig.NoiseMargin`. |
| `HarmonicComb` | `0` | |
| `HarmonicPeakBelowHz` | `0.0` | |
| `SubOctaveVeto` | `0.0` | Set from `LiveCqtConfig.SubOctaveVeto`. |
| `JuceVerifyResult Verify(double soundingMidi, double tSeconds, bool allowHarmonicPeak = true, bool allowSubOctaveVeto = true)` | | |

`JuceVerifyResult` is
`readonly record struct (bool Hit, float Confidence, float Level)`.

```csharp
var verifier = new JuceChartVerifier(cqt)
{
    NoiseMargin   = LiveCqtConfig.NoiseMargin,
    SubOctaveVeto = LiveCqtConfig.SubOctaveVeto,
};

cqt.Push(block, streamPosition);
JuceVerifyResult v = verifier.Verify(soundingMidi: 52, tSeconds: noteTime);
if (v.Hit) award(v.Confidence);
```

The **sub-octave veto** is why `SubOctaveBins` exists: without bins below
`MinMidi`, a note an octave down has nowhere to be seen, and it scores as a
hit on the note above it.

## `JuceLeadTracker`

`sealed class` — per-block lead pitch with a technique classification.

| Member | Default | Description |
|---|---|---|
| `JuceLeadTracker(int sampleRate = 44100, int blockSize = 1024, int windowSize = 2048, float fMin = 65f, float fMax = 1320f, double trajSeconds = 0.35)` | | |
| `SampleRate`, `BlockSize`, `WindowSize` | | |
| `SilenceRms` | `1e-3f` | |
| `BendSemitones` | `0.7f` | |
| `SlideSemitones` | `1.8f` | |
| `VibratoDepth` | `0.15f` | |
| `SteadySemitones` | `0.2f` | |
| `void Reset()` | | |
| `JuceLeadFrame ProcessBlock(ReadOnlySpan<float> block)` | | |

`JuceLeadFrame` is
`readonly record struct (double TimeSeconds, float? Midi, JuceLeadTechnique Technique)`;
`JuceLeadTechnique` is `Silent | Steady | Bend | Slide | Vibrato | None`.
`Midi` is `null` when there is no pitch.

## Measuring it

Use `CqtStreamingEvalTests` — it measures the **streaming** path that actually
ships. `CqtDatasetEvalTests` measures the offline path and will give you a
different, non-shipping number.

Both are **opt-in**: they find the IDMT-SMT-GUITAR_V2 corpus only through the
`GND_IDMT_ROOT` environment variable. A hard-coded fallback path once made them
gate-resident on exactly one machine, for about ten minutes per run, ending in
a failure or a dead test host.
