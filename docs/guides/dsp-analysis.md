# Audio Analysis

`JuceSharp.Dsp` is a pure-managed analysis toolkit: no P/Invoke, no
`juce_sharp.dll`, nothing `IDisposable`. It is deterministic enough to
unit-test.

**It is a separate package**, and the samples on this page reach a second one
(`JuceAudioDecoder` lives in `JuceSharp.AudioIo`), so reference both — the core
`JuceSharp` package pulls in neither:

```xml
<PackageReference Include="JuceSharp.Dsp" />
<PackageReference Include="JuceSharp.AudioIo" />
```

(No package is published to a feed yet — see
[Installation](../getting-started/installation.md); from a clone these are
`ProjectReference`s to `src/JuceSharp.Dsp/` and `src/JuceSharp.AudioIo/`.)

The engine stays in native code; this package is for *understanding* audio —
chord charts, tuner UI, BPM detection during import, offline spectrograms, and
DotCharter's live note verification.

## Getting samples in

Everything here takes `ReadOnlySpan<float>` mono at a known sample rate. Two
sources:

```csharp
// Offline, from a file.
float[] mono = JuceAudioDecoder.DecodeAs(path, 44_100, 1).Interleaved;

// Live, from the engine's dry capture ring.
Span<float> block = stackalloc float[1024];
int n = engine.ReadInputCaptureStream(block);
```

## Onsets

`JuceOnsetDetector` is streaming: push blocks, get onsets back with an absolute
sample position.

```csharp
var det = new JuceOnsetDetector(sampleRate);          // FftSize 1024, HopSize 256
foreach (var chunk in chunks)
    foreach (JuceOnset o in det.Push(chunk))
        Console.WriteLine($"{o.SamplePosition / (double)sampleRate:F3}s  str={o.Strength:F2}");
det.Reset();
```

`StreamPosition` tracks how far the detector has consumed. On a capture path use
the allocation-free span overload — same detection, your buffer:

```csharp
var scratch = new JuceOnset[JuceOnsetDetector.RequiredOnsetCapacity(block.Length)];
det.Push(block, scratch, out int n);
```

`JuceOnsetDetector.LatencySamples` (896) and `JuceGuitarAnalyzer.LatencySamples`
(4096 + 80 ms) are the two report delays: how far past a reported position the
stream must advance before the detection comes out. Subtract them to line
detections up with your own input timeline. Overflow of the destination span is
dropped and counted (`DroppedOnsets` / `DroppedEvents`), never thrown. When only the
attack TIME matters, `JuceGuitarAnalyzer.Push(block, notices)` also reports each
attack as a provisional `JuceOnsetNotice` at the onset detector's delay rather than
the analyzer's ([onset-and-pitch.md](../dsp/onset-and-pitch.md#provisional-onsets)).

## Pitch

**Monophonic**, YIN-based:

```csharp
var tuner = new JuceChromaticTuner(sampleRate, windowSize: 2048,
                                   minFreq: 50f, maxFreq: 1500f) { Threshold = 0.15f };

if (tuner.Detect(window) is { } r)
    Console.WriteLine($"{r.NoteName}{r.Octave}  {r.FrequencyHz:F1} Hz  {r.CentsOff:F1} cents  ({r.Confidence:P0})");
```

For a **live tuner** do not drive `Detect` yourself — `JuceTunerStream` owns the
ring, the emit rate, a 3-tap median against octave slips and the note
hysteresis, and it refuses to call a reading below the window's lag ceiling
stable:

```csharp
var stream = new JuceTunerStream(new JuceChromaticTuner(sampleRate, 4096, minFreq: 25f), hopSize: 512);

if (stream.Push(block) && stream.Latest is { IsStable: true } f)
    Console.WriteLine($"{f.NoteName}{f.Octave}  {f.CentsOff:+0.0;-0.0;0} cents");
```

`Push` is allocation-free after construction. See
[`JuceTunerStream`](../dsp/juce-tuner-stream.md).

The YIN difference function is O(W*tau_max) and is the dominant cost of the
offline pass. `useFftDifference: true` computes it through an FFT
autocorrelation instead - ~9x faster at window 2048, agreeing with the direct
sum to 0.0025 cents. It is OPT-IN and `JuceGuitarAnalyzer` keeps the direct path
by default, because that analyzer's output is a bit-identity contract; pass
`new JuceGuitarAnalyzer(sampleRate, useFastYin: true)` to switch it over.

**Polyphonic**, from a fixed 4096-sample window:

```csharp
var poly = new JucePolyphonicPitch(sampleRate);        // WindowSize 4096, FftSize 8192
foreach (JucePitchCandidate c in poly.Detect(window4096))
    Console.WriteLine($"MIDI {c.Midi}  {c.FrequencyHz:F1} Hz  salience {c.Salience:F2}");
```

The second `Detect` overload takes a reference window, which lets you suppress
what was already sounding before the attack.

## Notes

`JuceNoteStream` turns a sample stream into note-on / note-off events:

```csharp
var notes = new JuceNoteStream(sampleRate);
foreach (JuceNoteEvent e in notes.Push(block))
    Console.WriteLine($"{e.Kind} {e.NoteName}{e.Octave} vel={e.Velocity:F2} @{e.TimestampSeconds:F3}");
```

Tune it with `OnsetEnergyThreshold`, `ReleaseEnergyThreshold`,
`StablePitchCents` and `RetriggerCents`.

## Guitar events

`JuceGuitarAnalyzer` is the richest of them: it emits an event per attack
(`Onset`) or per legato transition (`Legato`), carrying the pitch candidates,
the chroma, spectral descriptors and a technique flag set.

```csharp
var g = new JuceGuitarAnalyzer(sampleRate) { MinAttackRms = 0.01f };

foreach (JuceGuitarEvent e in g.Push(block))
{
    if (e.Techniques.HasFlag(JuceGuitarTechniques.PalmMute)) { }
    if (e.Techniques.HasFlag(JuceGuitarTechniques.PinchHarmonic)) { }
    Console.WriteLine($"{e.Kind} @{e.StreamSeconds:F3}  rms={e.Rms:F3} " +
                      $"centroid={e.SpectralCentroidHz:F0} crest={e.CrestFactor:F1}");
}
```

`JuceGuitarTechniques` is a `[Flags]` enum: `PalmMute`, `Harmonic`,
`PinchHarmonic`, `Percussive`, `Slap`, `Pop`, `Legato`.

## Chords and beats

```csharp
var chroma = new JuceChromagram(sampleRate, fftSize: 4096);
var chords = new JuceChordDetector { MinConfidence = 0.6f };

if (chords.Detect(chroma.Compute(frame)) is { } hit)
    Console.WriteLine($"{hit.Name}  ({hit.Quality}, {hit.Confidence:P0})");

var beats = new JuceBeatDetector(sampleRate);
JuceBeatDetector.Result bpm = beats.Analyze(mono);
Console.WriteLine($"{bpm.Bpm:F1} BPM, {bpm.Beats.Count} beats");

foreach (JuceTempoSegment seg in bpm.TempoMap)          // one entry for a steady song
    Console.WriteLine($"  {seg.Time:mm\\:ss}  {seg.Bpm:F1} BPM");
Console.WriteLine($"{bpm.BeatsPerBar}/4, {bpm.Downbeats.Count} downbeats");
```

**`Beats` is tracked, not gridded, as of 0.9.** The old fixed grid stepped one
global period from the first half-beat, so every beat past a tempo change
drifted; the beats now come from a dynamic-programming tracker over a local
tempo estimate, and `TempoMap` / `Downbeats` / `BeatsPerBar` come with them.
`Options = new JuceBeatDetector.AnalysisOptions { FixedGrid = true }` restores
the old grid bit for bit. `Bpm` is unchanged in both modes.

A tempo map is only as fine as its envelope: one frame of inter-beat interval is
`bpm^2 / (60 * fps)` BPM, which is 5.6 BPM at 120 BPM with the DEFAULT 512-sample
hop. Halve `HopSize` before asking for 2 BPM segments — see
[`JuceBeatDetector`](../dsp/juce-beat-detector.md).

`JuceChordQuality` is `Major | Minor | Diminished | Augmented`.

## Spectrogram and waveform buckets

```csharp
var b = new JuceSpectrogramBuilder(fftSize: 2048, hopSize: 512);
JuceSpectrogram spec = b.Build(mono, sampleRate, decibelScale: true);
Console.WriteLine($"{spec.FrameCount} x {spec.BinCount}, " +
                  $"frame 10 at {spec.FrameTimeSeconds(10):F3}s, bin 40 = {spec.BinHz(40):F0} Hz");

JuceWaveformPeak[] peaks =
    JuceWaveformPeaks.Build(interleaved, channels: 2, sampleRate, bucketMilliseconds: 10.0);
```

`JuceWaveformPeaks` gives min/max per bucket, which draws better at low zoom
than the single absolute peak `JuceAudioEngine.GenerateWaveform` returns.

## Detection v8

The chart-verification stack: a streaming constant-Q transform with a live
noise floor, a verifier that answers "was this notated note actually played?",
and a lead tracker for bends, slides and vibrato.

```csharp
var cqt = new JuceConstantQ(
    sampleRate,
    hop:                LiveCqtConfig.Hop,
    fMinMidi:           LiveCqtConfig.MinMidi,
    binCount:           LiveCqtConfig.BinCount,
    maxKernelSamples:   LiveCqtConfig.MaxKernelSamples,
    floorPercentile:    LiveCqtConfig.FloorPercentile,
    noiseHistoryFrames: LiveCqtConfig.NoiseHistoryFrames(sampleRate),
    subOctaveBins:      LiveCqtConfig.SubOctaveBins);

var verifier = new JuceChartVerifier(cqt)
{
    NoiseMargin   = LiveCqtConfig.NoiseMargin,
    SubOctaveVeto = LiveCqtConfig.SubOctaveVeto,
};

cqt.Push(block, streamPosition);
JuceVerifyResult v = verifier.Verify(soundingMidi: 52, tSeconds: t);
if (v.Hit) { }
```

> ⚠️ **Warning**
> **`LiveCqtConfig` is the single source of truth for the shipping detection
> configuration.** Never re-type one of its constants into a caller or a
> harness. That is exactly what drifted once, and the evaluation then reported
> a pre-veto number as current. If a value needs to change, change
> `LiveCqtConfig` and let every caller follow.

`JuceLeadTracker` classifies each block as `Silent`, `Steady`, `Bend`, `Slide`,
`Vibrato` or `None`:

```csharp
var lead = new JuceLeadTracker(sampleRate);
JuceLeadFrame f = lead.ProcessBlock(block);
if (f.Midi is { } midi)
    Console.WriteLine($"{midi:F2}  {f.Technique}");
```

## NaN safety

The managed detection path scrubs non-finite samples at its input, and it has
to: a `NaN` passes every `<=` gate and would then hold the noise gate open for
the whole 10-second horizon. `JuceConstantQ.NonFiniteSamples` and
`JuceGuitarAnalyzer.NonFiniteSamples` count what was scrubbed — a non-zero
value means something upstream is broken.

## Per-type pages

* [`JuceFft`](../dsp/juce-fft.md)
* [`JuceChromaticTuner`](../dsp/juce-tuner.md)
* [`JuceTunerStream`](../dsp/juce-tuner-stream.md)
* [`JuceBeatDetector`](../dsp/juce-beat-detector.md)
* [`JuceChromagram`](../dsp/juce-chromagram.md)
* [`JuceChordDetector`](../dsp/juce-chord-detector.md)
* [Onset & pitch](../dsp/onset-and-pitch.md)
* [Spectrogram & peaks](../dsp/spectrogram-and-peaks.md)
* [Detection v8](../dsp/detection-v8.md)
