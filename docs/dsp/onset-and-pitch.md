# Onset, Pitch, Notes & Guitar Events

Four streaming engines from `JuceSharp.Dsp`. All pure managed, none
`IDisposable`, all fed `ReadOnlySpan<float>` mono.

## `JuceOnsetDetector`

`sealed class`

| Member | Description |
|---|---|
| `const int FftSize = 1024` | Fixed. |
| `const int HopSize = 256` | Fixed. |
| `JuceOnsetDetector(int sampleRate)` | |
| `int SampleRate` | |
| `long StreamPosition { get; private set; }` | How far the detector has consumed. |
| `void Reset()` | |
| `IReadOnlyList<JuceOnset> Push(ReadOnlySpan<float> samples)` | Onsets found in this block. |
| `void Push(ReadOnlySpan<float> samples, Span<JuceOnset> outOnsets, out int count)` | Same detection, no allocation: onsets go into your span. |
| `static int RequiredOnsetCapacity(int blockSamples)` | Span length that guarantees nothing is dropped. |
| `long DroppedOnsets` | Lifetime count of onsets a full span could not take. Health counter — `Reset()` does **not** clear it. |
| `const int PositionOffset = 384` | Already baked into the reported position. |
| `const int LatencySamples = 896` | Report delay — see below. |

`JuceOnset` is `readonly record struct (long SamplePosition, float Strength)`.
`SamplePosition` is absolute in the pushed stream, so it survives block
boundaries.

```csharp
var det = new JuceOnsetDetector(44_100);
foreach (var chunk in chunks)
    foreach (var o in det.Push(chunk))
        Console.WriteLine($"{o.SamplePosition / 44100.0:F3}s  {o.Strength:F2}");
```

On a capture path take the span overload instead — it is the same code path
with a different sink (pinned bit-for-bit by
`StreamingSpanPushTests.OnsetSpanOverload_MatchesTheListOverloadExactly`) and it
allocates nothing:

```csharp
var scratch = new JuceOnset[JuceOnsetDetector.RequiredOnsetCapacity(block.Length)];
det.Push(block, scratch, out int n);
for (var i = 0; i < n; i++) { /* scratch[i] */ }
```

An onset that will not fit is **dropped and counted** in `DroppedOnsets`, never
thrown: a throw halfway through a block would leave the stream position past
audio whose onsets were never reported. Size the span with
`RequiredOnsetCapacity` and the counter stays at zero.

### Latency

`LatencySamples` (= `FftSize + HopSize - PositionOffset` = **896**, ~20 ms at
44.1 kHz) is how far past a reported `SamplePosition` the stream must advance
before that onset comes *out* of `Push`. It is exact, not a tuning: the
local-maximum decision is made one hop late so the flux can be compared with its
successor. Pushing in blocks larger than `HopSize` adds the block's own
quantisation on top — the constant is the floor. Use it to align onsets to your
own input timeline instead of guessing from the FFT size.

## `JucePolyphonicPitch`

`sealed class`

| Member | Description |
|---|---|
| `const int WindowSize = 4096` | The window `Detect` expects. |
| `const int FftSize = 8192` | |
| `JucePolyphonicPitch(int sampleRate)` | |
| `int SampleRate` | |
| `JucePitchCandidate[] Detect(ReadOnlySpan<float> window4096)` | |
| `JucePitchCandidate[] Detect(ReadOnlySpan<float> window4096, ReadOnlySpan<float> referenceWindow4096)` | Suppress what was already sounding. |

`JucePitchCandidate` is
`readonly record struct (int Midi, float FrequencyHz, float Salience)`.

The two-window overload is the one to use at an attack: pass the window just
before the onset as the reference, and pitches that were already ringing are
attenuated, so what comes back is what was newly played.

## `JuceNoteStream`

`sealed class` — turns a sample stream into note-on / note-off events.

| Member | Default | Description |
|---|---|---|
| `JuceNoteStream(int sampleRate, int windowSize = 2048, int? hopSamples = null, float minFreq = 50f, float maxFreq = 1500f)` | | Wraps a `JuceChromaticTuner`. |
| `OnsetEnergyThreshold` | `1e-4f` | Energy needed to start a note. |
| `ReleaseEnergyThreshold` | `2e-5f` | Energy below which it ends. |
| `StablePitchCents` | `40f` | How steady the pitch must be to commit. |
| `RetriggerCents` | `80f` | Pitch change that counts as a new note. |
| `SampleRate` / `WindowSize` / `HopSamples` | | Read-only, from the tuner. |
| `void Reset()` | | |
| `IReadOnlyList<JuceNoteEvent> Push(ReadOnlySpan<float> samples)` | | |

`JuceNoteEvent` is `readonly record struct (JuceNoteEventKind Kind, int Midi,
string NoteName, int Octave, float FrequencyHz, float CentsOff, float Velocity,
double TimestampSeconds)`; `JuceNoteEventKind` is `On | Off`.

It is monophonic — it is built on the YIN tuner, not the polyphonic detector.

## `JuceGuitarAnalyzer`

`sealed class` — the richest of the four. One event per attack or legato
transition, with pitch candidates, chroma, spectral descriptors and technique
flags.

| Member | Default | Description |
|---|---|---|
| `JuceGuitarAnalyzer(int sampleRate, bool useFastYin = false)` | | `useFastYin` drives the three internal `JuceChromaticTuner`s through the FFT difference function: a large constant-factor win that agrees with the direct path to well under half a cent, but **not bit-identical**, which is why it is opt-in. |
| `bool UseFastYin { get; }` | `false` | What the constructor was given. |
| `MinAttackRms` | `0.01f` | Floor for what counts as an attack. |
| `int SampleRate` | | |
| `ulong StreamPosition` | | Absolute write cursor. |
| `long NonFiniteSamples` | | How many NaN/Inf samples were scrubbed at the input. **Non-zero means something upstream is broken.** |
| `long LegatoLongWindowReadings` | | Diagnostic counter. |
| `void Reset(ulong streamPosition = 0)` | | |
| `IReadOnlyList<JuceGuitarEvent> Push(ReadOnlySpan<float> samples)` | | |
| `void Push(ReadOnlySpan<float> samples, Span<JuceGuitarEvent> outEvents, out int count)` | | Same detection, caller-owned destination. |
| `IReadOnlyList<JuceGuitarEvent> Push(ReadOnlySpan<float> samples, List<JuceOnsetNotice> notices)` | | Same detection, and each attack also APPENDED to `notices` as a provisional `JuceOnsetNotice` the moment the detector fires (see below). |
| `static int RequiredEventCapacity(int blockSamples)` | | Span length that never drops. |
| `long DroppedEvents` | | Overflow counter; `Reset()` does not clear it. |
| `int LatencySamples` | | `4096 + 80 ms`: an event at P is emitted no earlier than P + this. |

The span overload is **not** allocation-free when events are emitted and cannot
be — `JuceGuitarEvent` carries a `JucePitchCandidate[]` and a 12-bin chroma
array. What it removes is the per-call `List` on the overwhelmingly common path
where a block finalizes nothing. `LatencySamples` is larger than
`JuceOnsetDetector.LatencySamples` and is the binding one here: finalizing an
event needs the whole 4096-sample analysis window plus the 80 ms the decay ratio
measures.

`JuceGuitarEvent` is a `readonly record struct` of
`(JuceGuitarEventKind Kind, double StreamSeconds, ulong StreamSamplePosition,
JucePitchCandidate[] Pitches, float[] Chroma, float SpectralCentroidHz,
float Rms, float CrestFactor, float SpectralFlatness, float DecayRatio,
JuceGuitarTechniques Techniques)`.

It also has an init-only `ulong? DetectorSamplePosition`: the onset detector's
position for the attack the event finalizes (null on a tracker-found legato).

`JuceGuitarEventKind` is `Onset | Legato`.

### Provisional onsets

An event comes out `LatencySamples` (~190 ms) after its attack. A caller that only
needs WHEN — a game timing a hit another detector already confirmed — passes a
`List<JuceOnsetNotice>` and gets each attack ~20 ms after it:

| `JuceOnsetNotice` field | Meaning |
|---|---|
| `Kind` | `Detected` (a new provisional attack) or `Withdrawn` (it will produce no event) |
| `DetectorSamplePosition` | The KEY: the finalized event's `DetectorSamplePosition` equals it |
| `StreamSamplePosition`, `StreamSeconds` | The attack estimate, snapped onto the RMS rise heard so far — within one hop on a clean pluck |
| `Strength` | Detector flux over its threshold |

Every `Detected` notice is resolved exactly once — by the event carrying its key or
by a `Withdrawn` notice — unless `Reset` drops it first. A detector onset with no
fresh attack behind it yet (the flux detector also fires on the ripple of a
ringing low note) is not published, so an event's key may name no notice.

```csharp
var notices = new List<JuceOnsetNotice>();
notices.Clear();
foreach (var e in g.Push(block, notices)) { /* finalized: e.DetectorSamplePosition pairs it */ }
foreach (var n in notices) { /* provisional: n.StreamSeconds, ~20 ms after the attack */ }
```

`JuceGuitarTechniques` is `[Flags]`:

| Flag | Value |
|---|---|
| `None` | 0 |
| `PalmMute` | 1 |
| `Harmonic` | 2 |
| `PinchHarmonic` | 4 |
| `Percussive` | 8 |
| `Slap` | 16 |
| `Pop` | 32 |
| `Legato` | 64 |

```csharp
var g = new JuceGuitarAnalyzer(44_100);
foreach (var e in g.Push(block))
    if (e.Techniques.HasFlag(JuceGuitarTechniques.PalmMute))
        Console.WriteLine($"palm mute at {e.StreamSeconds:F3}s");
```

## NaN scrubbing

The managed detection path scrubs non-finite samples at its input, and it must:
a `NaN` passes every `<=` gate and would then hold the noise gate open for the
whole ten-second horizon. Watch `NonFiniteSamples` in development — a rising
count points at a broken producer, not at the analyzer.
