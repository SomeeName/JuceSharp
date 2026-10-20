# JuceChromaticTuner

Monophonic pitch detector based on YIN
(de Cheveigné & Kawahara, 2002) with parabolic interpolation for
sub-sample accuracy. Designed for guitar / bass at 50 Hz–1.5 kHz.

```csharp
var tuner = new JuceChromaticTuner(sampleRate: 44100, windowSize: 2048);

if (tuner.Detect(monoBuffer) is { } r)
    Console.WriteLine($"{r.NoteName}{r.Octave}  {r.CentsOff:+0;-0;0} cents");
```

Returns `null` if the buffer is too quiet (RMS < 1e-6) or no periodic
signal is found below the threshold.

## API

| Member | Description |
|--------|-------------|
| `JuceChromaticTuner(int sampleRate, int windowSize = 2048, float minFreq = 50, float maxFreq = 1500, bool useFftDifference = false)` | Construct. `windowSize` ≥ 256. |
| `float Threshold { get; init; } = 0.15f` | YIN periodicity threshold; lower = stricter. |
| `bool UseFftDifference { get; }` | Opt-in fast difference function; see below. |
| `JuceTunerReading? Detect(ReadOnlySpan<float> samples)` | Returns reading or `null`. |

`JuceTunerReading` exposes `FrequencyHz`, `NoteName`, `Octave`,
`CentsOff`, `Confidence` (0..1) and `Clamped` (default `false`) — the same
`Clamped` flag [`JuceTunerFrame`](juce-tuner-stream.md) carries: the reading sat
on the analysis window's lag ceiling, so it is a floor artefact rather than a
measurement. Widen the window instead of trusting it.

For a live needle use [`JuceTunerStream`](juce-tuner-stream.md), which owns the
ring, the emit rate, the octave-slip median and the note hysteresis.

## `useFftDifference` - the same answer, ~9x faster

The YIN difference function is O(W*tau_max): 2M multiply-adds per 256-sample
legato hop in `JuceGuitarAnalyzer`, which is the dominant cost of the whole
offline audio-to-chart pass. Expanding the square turns it into an
autocorrelation,

```
d(tau) = sum_{i<W-tau} x[i]^2  +  sum_{tau<=i<W} x[i]^2  -  2 * r_x(tau)
```

where the two energy terms are differences of a cumulative-energy prefix and the
cross term is the linear autocorrelation - one zero-padded real FFT, one power
spectrum, one inverse, every lag at once. The cumulative-mean normalisation on
top is byte-for-byte the loop the direct path runs.

**It is opt-in because it is not bit-identical.** The FFT's rounding error is
relative to the window's total energy while `d(tau)` near its dip is close to
zero, so the last few float digits differ. Measured (`FftYinEquivalenceTests`):

| | direct | FFT |
|---|---|---|
| 200 random 82-1000 Hz tones, worst disagreement | - | **0.0025 cents**, 0.00000 confidence |
| synthetic corpus (A4, A2, bass low E clamped and unclamped, G3) | - | <= 0.0018 cents |
| 400 detections at window 2048, Release | 1.893 ms/call | **0.203 ms/call (x9.3)** |
| the same in a Debug build | 7.030 ms/call | 1.389 ms/call (x5.1) |

`JuceGuitarAnalyzer` keeps the DIRECT path by default - its event stream is a
bit-identity contract - and takes `new JuceGuitarAnalyzer(sampleRate,
useFastYin: true)` to switch all three of its internal tuners over. On the
synthetic pluck corpus the two analyzers produce the same events, the same
positions, the same technique flags and the same MIDI numbers, with pitch
frequencies 0.001 cents apart.

Both are constructor parameters rather than `init` properties on purpose: the
flag decides what the constructor allocates, and an `init` property would have to
allocate lazily on the first call - exactly the per-call garbage these types
exist to avoid.
