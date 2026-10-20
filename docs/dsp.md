# DSP Toolkit

`JuceSharp.Dsp` is its own **package** and its own project
(`src/JuceSharp.Dsp/`). *This page is about that package.* It has **no native
dependency at all** — no P/Invoke, no `juce_sharp.dll`, nothing here is
`IDisposable` — so it runs anywhere `net10.0` runs. (It targets `net10.0` only,
like every packable project in this repo; "no native dependency" is not "any
target framework".)

> ⚠️ **`JuceSharp.Dsp` is a package name AND a namespace, and they are not the
> same set.** The **core `JuceSharp`** package also declares types in the
> `JuceSharp.Dsp` namespace — `JuceDspChain`, `JuceDspNode`, `JuceDspEditScope`,
> `JuceDspMeters`, `JuceDspModuleCatalog` — and those DO P/Invoke, and
> `JuceDspEditScope` **is `IDisposable`**: its entire contract is
> `using var edit = chain.BeginEdit();`. A `using JuceSharp.Dsp;` can therefore
> reach either. Nothing on this page is one of them; see
> [`juce-dsp-chain.md`](api/juce-dsp-chain.md) for the rig side.

## Why managed?

The hot signal path stays in the native engine. The DSP toolkit is
meant for *analysis*: chord-chart generation, tuner UI, BPM detection
during PSARC import, off-line waveform / spectrogram rendering, etc.
Keeping it in C# means it ships without a native rebuild and is easy
to test deterministically with xUnit.

## Available modules

Eighteen source files declare 33 top-level public types (plus two nested,
`JuceBeatDetector.Result` and `JuceBeatDetector.AnalysisOptions`), measured
2026-09-11. The engines:

| Type | Use case | Page |
|------|----------|------|
| `JuceFft` | Radix-2 Cooley-Tukey FFT | [→](dsp/juce-fft.md) |
| `JuceChromaticTuner` | YIN-based mono pitch detection | [→](dsp/juce-tuner.md) |
| `JuceTunerStream` | The live needle over `JuceChromaticTuner`: ring, emit rate, octave-slip median, note hysteresis (0.9) | [→](dsp/juce-tuner-stream.md) |
| `JuceBeatDetector` | Offline BPM, tracked beats, tempo map + downbeats | [→](dsp/juce-beat-detector.md) |
| `JuceChromagram` | 12-bin pitch-class profile | [→](dsp/juce-chromagram.md) |
| `JuceChordDetector` | Triad recognition from chroma | [→](dsp/juce-chord-detector.md) |
| `JuceOnsetDetector` | Streaming spectral-flux onsets | [→](dsp/onset-and-pitch.md) |
| `JucePolyphonicPitch` | Multiple simultaneous pitches from one window | [→](dsp/onset-and-pitch.md) |
| `JuceGuitarAnalyzer` | Onset/legato events with technique flags | [→](dsp/onset-and-pitch.md) |
| `JuceNoteStream` | Note-on / note-off events from a sample stream | [→](dsp/onset-and-pitch.md) |
| `JuceSpectrogramBuilder` | Hann-windowed STFT bitmap | [→](dsp/spectrogram-and-peaks.md) |
| `JuceWaveformPeaks` | Min/max buckets for waveform drawing | [→](dsp/spectrogram-and-peaks.md) |
| `JuceConstantQ` | Streaming constant-Q transform with a noise floor | [→](dsp/detection-v8.md) |
| `JuceChartVerifier` | "Was this notated note actually played?" | [→](dsp/detection-v8.md) |
| `JuceLeadTracker` | Per-block lead pitch + bend/slide/vibrato | [→](dsp/detection-v8.md) |
| `LiveCqtConfig` | The shipping detection constants | [→](dsp/detection-v8.md) |

The result types they return — `JuceTunerReading`, `JuceChordReading`,
`JuceChordQuality`, `JuceOnset`, `JucePitchCandidate`, `JuceNoteEvent`,
`JuceNoteEventKind`, `JuceGuitarEvent`, `JuceGuitarEventKind`,
`JuceGuitarTechniques`, `JuceLeadFrame`, `JuceLeadTechnique`,
`JuceVerifyResult`, `JuceSpectrogram`, `JuceWaveformPeak`, `JuceTunerFrame`
(the streaming tuner's per-emit frame) and `JuceTempoSegment` (one `(Time, Bpm)`
point of `JuceBeatDetector`'s tempo map) — are all `record` types documented on
the same pages.

> ⚠️ **Warning**
> **`LiveCqtConfig` is the single source of truth for the shipping detection
> configuration.** Never re-type one of its constants into a harness or a
> caller: that is exactly what drifted once, and the evaluation then reported
> a pre-veto number as current.

## Building a chord chart from a song

```csharp
using JuceSharp.Dsp;

float[] mono = LoadMonoSamples("song.wav", out int sr);

var chroma = new JuceChromagram(sr, fftSize: 4096);
var det    = new JuceChordDetector { MinConfidence = 0.6f };

const int hop = 8192;
for (int offset = 0; offset + chroma.FftSize < mono.Length; offset += hop)
{
    var frame = mono.AsSpan(offset, chroma.FftSize);
    var c     = chroma.Compute(frame);
    var hit   = det.Detect(c);
    if (hit is { } r)
        Console.WriteLine($"{TimeSpan.FromSeconds((double)offset / sr):mm\\:ss} → {r.Name}  ({r.Confidence:P0})");
}
```
