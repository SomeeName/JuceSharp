# `JuceSharp.Dsp` — the managed DSP + note-detection surface

Deep reference for `JuceSharp/AGENTS.md`. The managed DSP types the app's
audio-to-chart analysis and live note verification run on. Consumed by
`DotCharter.Analysis` (offline) and `DotCharter.Services/Audio/LivePlay`
(streaming) — neither of which may reference the other, which is why
`LiveCqtConfig` exists.

- `JuceSharp.Dsp/` — managed DSP surface used by the app's audio-to-chart
  analysis (`JuceGuitarAnalyzer`, `JuceFft`, event-kind enums). **Detection v8**
  adds a chart-informed live-verification path: `JuceConstantQ` (streaming
  quarter-tone CQT, offline matrix + rolling buffer), `JuceChartVerifier`
  (Binary Match Mask: confirm each *expected* note against the CQT, no blind
  transcription) and `JuceLeadTracker` (causal YIN + bend/slide/vibrato). Ported
  from the NoteDetection testbed; golden-vector tests use the `Synth` helper in
  `JuceSharp.Tests` (no datasets).
  **`JuceConstantQ` has TWO entry points that compute DIFFERENT statistics, and only
  one ships.** Offline `AnalyzeWhole` gates on a whole-clip 95th-percentile floor and a
  whole-clip noise floor; streaming `Reset`+`Push` (what live play runs) gates on a
  rolling max and a rolling noise floor. **Measure detection with
  `CqtStreamingEvalTests`, never `CqtDatasetEvalTests`** — the latter drives the offline
  path, so its numbers describe code we do not run. Both are opt-in on IDMT
  (`GND_IDMT_ROOT`, `GND_EVAL_CAP`); results are sample-dependent, so compare only at the
  SAME cap and always state it.
  **`GND_IDMT_ROOT` is the ONLY way `IdmtDataset.ResolveRoot` finds the dataset —
  never re-add a fallback path.** It used to fall back to one developer's absolute
  checkout, which made "opt-in" false on the one machine whose gate anybody watches:
  both harnesses ran on every `dotnet test`, took ~10 minutes at the default cap of
  60 tracks, and ended in an assertion failure or a dead test host. Pinned by
  `IdmtDatasetTests`.
  **The streaming path does not ALLOCATE in steady state, and that is now pinned**
  (2026-09-11, `JuceConstantQAllocationTests`). Two sites did and neither had to:
  `NoiseFloorEnergy` allocated a `float[count]` and sorted it once per EMITTED
  FRAME (1,722 floats at the shipping 10 s horizon, ~172 frames a second), and
  `Reset` — which `Push` calls for ANY stream discontinuity, so a resume or a seek
  reaches it mid-song — rebuilt the whole ring, ~390 arrays and ~340 KB. Both now
  use persistent buffers. **Every reused buffer is CLEARED**, which is what makes
  it indistinguishable from the freshly allocated one it replaces and the change a
  pure optimisation; the percentile is the same `Array.Sort` introsort over the
  same values, so the selected number is bit-identical. Assert the ALLOCATION
  COUNT (`GC.GetAllocatedBytesForCurrentThread`), never a wall clock.
  **`LiveCqtConfig` is the single source of truth for the shipping configuration** —
  DotCharter's `LiveCqtVerifier`, its offline `ChartAudioLintAnalyzer` and the streaming
  eval harness all read it (none of the three can reference the others). An un-set
  `CqtStreamingEvalTests` run therefore measures exactly what ships, and the in-gate
  `CqtEvalConfigTests` (no dataset needed) keeps it that way. It did NOT used to: the
  harness hand-copied the numbers and kept defaulting `SubOctaveVeto = 0` and the bare
  2.23 s noise ring long after both were adopted, so it reported the pre-veto ~89.7%
  octave-down decoy rate as current. Never re-type a shipping constant — change
  `LiveCqtConfig`.
  **…and `CqtEvalConfigTests` compared the harness to the config and never the config to
  the PRODUCT DEFAULTS, which is how three re-typed literals survived in the shipping
  path itself** (found 2026-09-11): `JuceChartVerifier.NoiseMargin = 3f`,
  `JuceConstantQ`'s `hop = 256` and `floorPercentile = 1.0f` — in a file that already
  imported `RelFloor`, `NoisePercentile` and `RingFrames` BY NAME, so it read as
  migrated. All three now name the constant, and the guard is a SOURCE lint, not a value
  comparison: while the two numbers still agree a value test passes either way, which is
  exactly the window drift lives in. A fourth of the same shape was
  `JuceGuitarAnalyzer._legatoShortFloorHz`, which re-DERIVED
  `JuceChromaticTuner.EffectiveMinFrequency` from the window size and was wrong below
  30,720 Hz (at 22,050 Hz it computed 22.6 Hz against the tuner's real 30.0 Hz clamp, so
  a clamped reading was accepted as a MEASUREMENT — the artefact that floor exists to
  reject). Ask the object that owns the number.
  **Three 256s in this assembly are NOT `LiveCqtConfig.Hop`** — `JuceOnsetDetector.HopSize`,
  `JuceGuitarAnalyzer.LegatoHop` and `JuceNoteStream`'s `max(256, windowSize/2)` default
  are unrelated subsystems that share the value; folding any of them into the shared
  constant would change the onset detector's frame rate. Only `JuceConstantQ.Hop` follows
  it. Two hard-won settings, with the numbers in their XML docs:
  `NoiseHistoryFrames` (the noise floor needs a **10 s** horizon — over 2.23 s it measures
  "the quietest moment while playing", and the presence gate then rejects real notes) and
  `SubOctaveVeto` (without it, playing the charted note an octave DOWN scores a hit ~90%
  of the time — the 2nd harmonic lands on the charted bin and is a genuine peak; SINGLE
  notes only, chords contain real octaves).
  **The veto needs `JuceConstantQ.SubOctaveBins` to have anywhere to look**: it reads
  `bin - 12*Step`, which is NEGATIVE for the bottom octave of the charted range, so
  before those veto-only kernels existed it silently could not fire across the whole bass
  low E string — exactly the register `LiveMinMidi` was lowered to E1 to cover. (Since
  0.57.4 the range starts at A0, `MinMidi = 21` / `BinCount = 158`, so a 5-string's low B
  and a B-standard bass are in range; the sub-octave bins now reach MIDI 8.)
  **The streaming noise floor has a third estimator since 0.57.4, the IDLE floor**
  (`JuceConstantQ.IdleSteadyFrames` + `IdleSteadyRatio` + `IdleFloorRiseDbPerSecond`, all
  from `LiveCqtConfig`; `0` frames = off, the default for every other caller; offline
  ignores it). The 10 s percentile is still "the quietest moment while playing" during a
  phrase that does not pause, and the gate refused 38.6 % of perfectly played notes on
  real DI songs. Live play gates at `StreamingNoiseMargin = 2` over it; `NoiseMargin = 3`
  stays the offline lint's. Numbers and the eval: `tools/detection-eval/README.md`. The same
  hole made `IsPeak` half-vacuous at bins 0/1 (a missing lower neighbour reads as 0, so
  `c >= 0` was always true). `LiveCqtConfig.SubOctaveBins = 26` (13 semitones at
  quarter-tone) ships. They are excluded from `BinCount` AND from the broadband energy
  sum, so `FrameEnergy`/`NoiseFloorEnergy` and every measured presence-gate number are
  untouched; `subOctaveBins: 0` stays a strict no-op. Cost ~+18% of the per-frame taps. `FloorPercentile`, `HarmonicPeakBelowHz` and
  `HarmonicComb` are **measured and rejected**, kept default-off and documented so they
  are not rediscovered.
  **The managed detection path scrubs non-finite (NaN/Inf) input too** — the same rule
  `DspChain::process` follows natively, for a sharper reason: every verifier gate reads
  "reject when level <= threshold", and `NaN <= x` is FALSE, so an unscrubbed NaN *passes*
  the per-bin floor gate AND the anti-phantom presence gate, then persists in the noise
  history for the whole 10 s horizon and holds that gate open. `JuceConstantQ.Push` and
  `JuceGuitarAnalyzer.Push` scrub at their input and count it (`NonFiniteSamples` on both —
  the condition used to be completely silent); the kernel magnitudes also guard their own
  output, which covers the offline `AnalyzeWhole` path (it reads a caller-owned whole-song
  span it cannot afford to copy) and dot-product overflow to Inf. Keep both halves.
  **…and `JuceChromaticTuner.Detect` was the one detection entry point that did
  NOT scrub, with both its gates NaN-transparent** (`energy/WindowSize < 1e-6f`
  and `freq < MinFrequency || freq > MaxFrequency` are all false for NaN), so a
  non-finite window passed both and returned a reading of `NoteName "C"`,
  `Octave -1`, `CentsOff NaN`. The reachable input is not a NaN sample — those
  make every `d(tau)` NaN and it already returned null — but an **energy that
  overflows to +Inf from finite samples**. Fixed 2026-09-11; the three streaming
  wrappers scrubbed at their boundary already.
  **`JuceSpectrogramBuilder.Build` was the LAST unscrubbed entry point in the
  assembly** and its documented −120 dB floor did not hold for a non-finite
  magnitude at all: `MathF.Max(1e-12f, NaN)` is NaN and so is
  `MathF.Max(-120f, NaN)`, so a NaN left through the one line whose entire
  purpose is that floor. It now scrubs at the input with a `NonFiniteSamples`
  counter (per WINDOW, since windows overlap — the `JuceBeatDetector`
  convention) AND guards the output magnitude, because a scrubbed input still
  leaves a dot product that can overflow to +Inf. Unlike its siblings it never
  LATCHED, which is why it survived: the per-frame scratch is fully overwritten.

**Three smaller wave-4 corrections worth not rediscovering** (2026-09-11):
`JuceConstantQ.Reset`'s `ringFrames` was the one unvalidated public argument on
the type — `Reset(0, 0)` surfaced as a `DivideByZeroException` from the NEXT
`Push` (`Mod(frame, 0)`), which names nothing, and a negative value as an
`OverflowException` from the ring allocation; it is range-checked now.
`JuceConstantQ` and `JuceChartVerifier` carry an explicit **"not thread-safe,
one thread"** statement, which their three siblings (`JuceChromagram`,
`JucePolyphonicPitch`, `JuceSpectrogramBuilder`) always had and these two did
not — they mutate instance scratch from getter-shaped members (the percentile
buffer, the floor memoisation), so the natural live-play split of audio-thread
`Push` / game-thread `Verify` tears a percentile. Making them concurrent is
impractical given the memoisation, so the DOC is the fix and a lint keeps it
there. And `JuceBeatDetector.ComputeSpectralFlux` dropped the last complete
frame (`(n - Fft)/Hop` instead of `+ 1`): 6 frames instead of 7 at the
documented minimum input, which is precisely where `maxLag = env.Length / 2`
already binds and throws "audio is too short".

**`JuceFft` is no longer radix-2 only, and `Windows` is no longer internal.**
The transform now has three paths: **radix-2** for a power of two, **radix-3** for
`3·2^k`, and **Bluestein** (chirp-z) for any other size — which is what makes
MDX-Net's shipping `nFft = 6144` constructible at all; it threw on its first chunk
from the MathNet → `JuceFft` migration until that landed, because MathNet had
Bluestein and this did not. Measured per forward transform: **4096 radix-2 = 55 µs,
6144 radix-3 = 110 µs, 6144 Bluestein = 526 µs** — so a power of two is still the
size to pick when you are free to pick one, and the doc on any `fftSize` parameter
should say "a power of two is the FASTEST case", never "must be a power of two".
`JuceBeatDetector.FftSize` and `JuceSpectrogramBuilder` were both re-worded on that
basis; the beat detector's own check is now EVEN-and-≥2 (its window, its `FftSize/2`
bin loop and its hop are all size-agnostic — only `FftSize/2` needs evenness), and
the radix constraint is left to the transform that owns it.
`JuceSharp.Dsp.Windows` is PUBLIC (2026-09-11) for the same reason the type exists:
an application driving `JuceFft` itself had to hand-roll a seventh copy of the two
Hann loops. The returned array is CACHED AND SHARED — window by multiplying into
your own scratch, never write into it.

**Three more wave-4 corrections, each a doc that was wrong rather than a number
that moved** (2026-09-11): `JuceLeadTechnique.Silent` was returned for LOUD
unvoiced audio as well as for silence, so `None` ("audio was present but no pitch
could be resolved") was unreachable from the unvoiced branch and a consumer could
not tell "the player stopped" from "the player is making noise I cannot track";
`JuceNoteStream`'s retrigger promised a note-on "immediately" after the note-off
and delivered it one hop later, because `EmitOff` clears the stability state the
fall-through onset evaluation then requires; and `JuceChartVerifier.HarmonicSupport`
carried a comment about an octave partial "that also rose" while the shipping
`PreRatio = 0` reduced that test to `> 0`. At `PreRatio == 0` the octave now has to
clear its own bin floor. `HarmonicSupport` is off by default, so no shipping number
moved. Also: `JuceGuitarAnalyzer.LegatoLongWindowReadings` counts long-window
SUCCESSES, not fall-backs — the summary said the opposite, and two fall-back paths
never reach the increment — so `LegatoLongWindowFallbacks` now counts what the old
name claimed.

**`_finalizeLag` must cover what `FinalizeOnset` reads AFTER refinement, not
before it.** The pending queue releases on the UNREFINED onset position, but
`RefineAttackPosition` moves it forward by up to `AnalysisWindow - PreOnsetLookback`
(3,840 samples) and the decay RMS is then read `DecayDelay + ShortRmsWindow`
(4,552 at 44.1 kHz) past THAT. At the old `AnalysisWindow + _decayDelaySamples`
(7,624) the worst case ran 768 samples past `_writePos`, where `ReadRing` clamps and
ZERO-FILLS: `decayRatio` collapsed and the note was silently classified `PalmMute`,
with no counter moving. It is now 8,392 at 44.1 kHz, and `LatencySamples` moved with
it. Pinned structurally (`Wave4DspAuditTests`) rather than by a stimulus, because
reproducing the worst case needs the detector to fire >3,072 samples early on one
particular attack shape — a stimulus that could silently stop reproducing it.

**Provisional onsets (DotCharter #643): `Push(samples, List<JuceOnsetNotice>)`
reports an attack ~20 ms after it, not `LatencySamples` (~190 ms) after it.** A
`Detected` notice goes out when the detector fires; the event that finalizes it
carries the same `DetectorSamplePosition` (the KEY), or a `Withdrawn` notice says
none will. Detection is unchanged — the other overloads publish nothing and every
event is bit-identical apart from the new `DetectorSamplePosition`. Three things a
reader would get wrong:
- **The detector's own position is not an attack estimate** — it is a fixed offset
  into the triggering FFT window, measured ~300 samples early at 48 kHz. The
  notice's `StreamSamplePosition` is the finalized path's `RefineAttackPosition`
  cut at `_writePos`, which lands within one hop on a clean pluck.
- **Not every detector onset is published.** The flux detector also fires every
  ~50 ms on the ripple of a ringing low note (~10 per 0.5 s E2 pluck, all withdrawn
  by the novelty gate later). `ShowsFreshAttack` publishes only when the 1024-sample
  RMS after the estimate over the 1024 before it reaches `AttackRatioMin` — a
  256-sample frame cannot judge it, it is under half a period of E2 — and onsets
  snapping onto an attack already published are not published again. So an event's
  key may name an onset that was never published; a `Detected` notice is still
  resolved exactly once.
- **`Reset` drops pending onsets WITHOUT a notice** — the caller forgets its own
  (DotCharter's `GuitarInputAnalyzer` withdraws them itself).
Pinned by `JuceGuitarAnalyzerProvisionalOnsetTests`.

**Chromagram windowing is PINNED, and it did move a shipping number.**
`JuceChromagram.ApplyHannWindow` defaults to `true` and `JuceGuitarAnalyzer`
constructs both of its chromagrams without overriding it, so the D5 windowing
change moved the `Chroma` field of EVERY emitted `JuceGuitarEvent` — silently,
with no baseline and no test (`ApplyHannWindow` had zero references under
`tests/`). A comment two commits later said "this migration does not move a
single analyzer or CQT number", which is true of the Hann REFACTOR alone
(`0.5f - 0.5f*c` and `0.5f*(1f-c)` are bit-identical, since halving is exact)
and false of the analyzer's chroma; the two sat adjacent and read as one global
no-change claim. The windowing is deliberate and KEPT. Both branches are now
pinned by `Wave2ChromagramBaselineTests`, whose baseline is **derived, not
captured**: the stimulus sits exactly on an FFT bin, so unwindowed it is a
single DFT spike and windowed it is the 0.25/0.5/0.25 main lobe split across two
pitch classes — a captured golden vector would have re-pinned whatever the code
happened to do.

**Three more invariants the streaming types now carry** (2026-09-11):
`JuceTunerStream.Latest` is published through a SEQLOCK (the `ParamMailbox`
pattern, still allocation-free) — it is ~40 bytes documented "bind a UI to
this", written from the analysis thread, so an unsynchronised read could show a
`NoteName` from one frame with the `CentsOff` of another, which reads as a
detection bug rather than a torn read; an exact-OCTAVE jump now goes through
`NoteChangeFrames` instead of `ImmediateChangeCents`, because a YIN octave slip
is 1200 cents and sailed past the 250-cent bypass that exists to filter exactly
that (the 3-tap median only removes a SINGLE-frame slip); and
`JuceBeatDetector` returns `Bpm 0` with a new `Result.Confidence` for an
envelope with no peak, instead of a fabricated ~`MaxBpm` grid indistinguishable
from a real detection. `Confidence` is peak PROMINENCE, not energy-normalised —
the latter reads ~1.0 for a flat non-zero envelope, i.e. a pad with no beat.
`JuceNoteStream` timestamps an event at the window START (up to `WindowSize`
early), which is now stated on `JuceNoteEvent.TimestampSeconds` rather than left
to be discovered.
