# JuceTunerStream

A UI-grade streaming wrapper over [`JuceChromaticTuner`](juce-tuner.md).

`Detect` is a one-shot over a full window. A live needle needs four more things
— a ring, a fixed emit rate, an octave-slip filter and note hysteresis — and
every caller was writing those four differently. `JuceTunerStream` owns them.

```csharp
var stream = new JuceTunerStream(
    new JuceChromaticTuner(sampleRate, windowSize: 4096, minFreq: 25f),
    hopSize: 512);                       // default: WindowSize / 4

// …on the capture callback / ring drain:
if (stream.Push(block) && stream.Latest is { IsStable: true } f)
    Console.WriteLine($"{f.NoteName}{f.Octave}  {f.CentsOff:+0.0;-0.0;0} cents  {f.FrequencyHz:F2} Hz");
```

`Push` takes arbitrary block sizes and is **allocation-free after
construction** (pinned by `JuceTunerStreamTests.Push_DoesNotAllocate`), so it
is safe on a capture path. Non-finite input is scrubbed at the boundary and
counted in `NonFiniteSamples`.

## The frame

`JuceTunerFrame` is a `readonly record struct`:

| Member | Meaning |
|---|---|
| `NoteName`, `Octave`, `MidiNote` | the **held** note — what the display is showing |
| `CentsOff` | signed cents of the smoothed frequency against the **held** note, not against the nearest semitone. That is the needle deflection: during a note change it grows past ±50 for a few frames instead of snapping to zero on a note the UI is not showing yet |
| `FrequencyHz` | median-smoothed fundamental |
| `Confidence` | the newest reading's YIN confidence (not smoothed) |
| `Clamped` | the reading sat on the window's lag ceiling — a **floor artefact, not a measurement** |
| `IsStable` | the held note has been agreed on by `StableFrames` readings, the reading cleared `MinStableConfidence`, and it is not `Clamped` |
| `TimeSeconds` | the **START** of the analysis window, in seconds since construction / `Reset` — the same convention `JuceLeadFrame.TimeSeconds` and `JuceNoteEvent.TimestampSeconds` use. It advances by exactly `HopSize / SampleRate` per emitted frame whatever block sizes the caller pushes. **Not `StreamPosition`**, which `Push` advances by the whole block *before* processing any of it |

**A clamped frame is never stable.** Below `JuceChromaticTuner.EffectiveMinFrequency`
YIN does not fail — it returns the clamped lag as a *confident* reading (a bass
low E on a 2048-sample window at 44.1 kHz reads ≈43 Hz at 0.93 confidence,
nearly a semitone sharp and indistinguishable from a real F1). `Clamped` is set
when the underlying reading is clamped **or** the frequency is within
`ClampedMargin` (5 %) of `EffectiveMinFrequency`. Widen the window rather than
lowering `minFreq`: the window is the binding limit.

## The three filters

| Filter | Knob | Default | What it removes |
|---|---|---|---|
| 3-tap **median** on frequency | — | always on | single-frame octave slips (a mean would smear one across three frames) |
| note **hysteresis** | `NoteChangeFrames` | 3 | name flicker at a transition. A reading further than `ImmediateChangeCents` (250) from the held note switches immediately — more than two semitones is never a mis-read, but an ordinary whole tone (200 cents) still waits |
| **release** hold | `ReleaseFrames` | 12 | a pick-hand mute blanking the display. After that many unvoiced windows `Latest` goes `null` and tracking resets |

## API

| Member | Description |
|---|---|
| `JuceTunerStream(JuceChromaticTuner tuner, int hopSize = 0)` | Wrap an existing tuner; `hopSize` 0 ⇒ `WindowSize / 4`. |
| `JuceTunerStream(int sampleRate, int windowSize = 2048, float minFreq = 50, float maxFreq = 1500, int hopSize = 0)` | Convenience: builds the tuner too. |
| `bool Push(ReadOnlySpan<float> samples)` | `true` if at least one frame was emitted. |
| `JuceTunerFrame? Latest { get; }` | Newest frame, `null` before the first / after a release. |
| `long StreamPosition`, `long FramesEmitted` | Reset by `Reset()`. |
| `long NonFiniteSamples` | Lifetime health counter — **not** reset by `Reset()`. |
| `void Reset()` | Drop window, median, held note and positions. |
| `NoteChangeFrames`, `ImmediateChangeCents`, `StableFrames`, `MinStableConfidence`, `ReleaseFrames`, `ClampedMargin` | `init`-only knobs; see the table above. |

The tuner is not copied and must not be driven concurrently from elsewhere.
