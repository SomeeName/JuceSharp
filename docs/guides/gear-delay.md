# Delay gear

Modules in the `delay` drawer added in the 2026-09 gear wave. Each section names the
module's type id, its params (id, range, default, unit) and what it is modelled on.
See [The DSP Tone Rig](tone-rig.md) for how modules are hosted.

Five modules, one core header each under
`src/JuceSharp.Native/src/dsp/cores/`, adapters and catalogue rows in
`src/JuceSharp.Native/src/dsp/gear/DelayGear.cpp`. They share four
properties, and all four are pinned by
`tests/JuceSharp.Tests/Dsp/GearDelayTests.cs` and the RT gate's
`RtDelayGearRigIsAllocationFree`:

- **Delay lines are allocated in `prepare()` for the worst case** — the longest
  time the knob allows, at the rate `prepare` was handed. Nothing on the
  callback grows a buffer, and an oversized block is skipped WHOLE rather than
  processed head-first.
- **A time change glides.** The read position is slewed or one-pole smoothed,
  so moving the knob is a pitch bend and never a splice. `bbddelay` makes the
  glide the point; the other four keep it under a ~50 ms smoother.
- **Every feedback path carries a DC blocker and a soft limiter**, so the top
  of the feedback knob is a bounded, sustaining tail rather than a runaway.
- **Every user-facing gain, mix and level ramps** (`SmoothedValue`), and every
  param clamps to its declared range and REFUSES a NaN.

---

## `multitap` — Multitap Delay

A multi-head tape / analog multitap: one mono delay line written from the
channel sum, read at up to four taps whose offsets are `timeMs` times a fixed
ratio set, each tap panned alternately right/left and levelled by a geometric
falloff. The tap sum — tone-shaped, DC-blocked, soft-limited and normalised by
the weights that produced it — is the single feedback signal.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `taps` | 1 – 4, step 1 | 3 | count | How many taps are live. A count change is a 30 ms **weighted fade** per tap, not a branch. |
| `timeMs` | 10 – 2000 | 375 | ms | The BASE tap interval; every tap is a fixed ratio of it. |
| `spread` | Even / Dotted / Triplet / Golden | Even | — | The ratio set. Even 1·2·3·4, Dotted 0.75·1.5·2.25·3, Triplet ⅔·1⅓·2·2⅔, Golden 0.618·1·1.618·2.618. |
| `decay` | 0 – 1 | 0.6 | — | Per-tap level falloff: tap *k* sits at `(1 − 0.9·decay)^k`. At 0 every tap is equal. |
| `pan` | 0 – 1 | 0.7 | — | Alternating equal-power width. Tap 1 right, tap 2 left, tap 3 right… At 0 every tap is centred. |
| `feedback` | 0 – 0.9 | 0.2 | — | How much of the normalised tap sum is written back. |
| `tone` | 0 – 1 | 0.5 | — | One-pole low-pass in the feedback path, 500 Hz – 12 kHz logarithmic. |
| `mix` | 0 – 1 | 0.4 | — | Dry/wet. |

**Modelled on** a Deluxe Memory Man / multi-head Echoplex arrangement — several
playback heads off one record head, rather than several independent delays.

**Using it.** Set `taps 4`, `spread Golden` and `decay 0.3` for a wide,
non-metrical wash that never lines up into a rhythm. For a dotted-eighth lead
delay use `spread Dotted`, `taps 2`, `pan 1`. `feedback` recirculates the whole
tap pattern, so it thickens rather than simply lengthening: past ~0.6 with four
taps it fills in completely.

---

## `duckdelay` — Ducking Delay

A delay whose repeats get out of the way while you play. A peak detector
(5 ms attack, your `releaseMs`) watches the DRY input; while it sits above the
`sensitivity`-derived threshold the wet READ is attenuated by `duckDb`, and it
swells back as the detector falls.

The attenuation is on the read, never the write: duck what goes INTO the line
and the quiet is baked into the recording and nothing can swell back.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `timeMs` | 10 – 2000 | 400 | ms | Echo time. |
| `feedback` | 0 – 0.9 | 0.35 | — | Repeats. Tone-shaped, DC-blocked and soft-limited in the loop. |
| `duckDb` | 0 – 40 | 12 | dB | How far the repeats drop while you are playing. 0 is a plain delay. |
| `sensitivity` | 0 – 1 | 0.5 | — | Detector threshold, −6 dBFS at 0 to −60 dBFS at 1. There is a 6 dB knee above it. |
| `releaseMs` | 20 – 2000 | 300 | ms | Detector release — how fast the repeats come back. |
| `tone` | 0 – 1 | 0.5 | — | One-pole low-pass in the feedback path, 500 Hz – 12 kHz. |
| `mix` | 0 – 1 | 0.4 | — | Dry/wet. |

**Modelled on** the studio "delay throw" a vocal engineer rides by hand, and on
the ducking delays of the TC 2290 / Strymon Timeline lineage.

**Using it.** `duckDb 12`, `releaseMs 300` is the setting that lets a long
delay sit behind a busy rhythm part. Raise `sensitivity` if quiet picking is
not ducking; lower it if sustained notes keep the tail suppressed. Long
`releaseMs` with high `feedback` gives the classic "the delay arrives when you
stop" effect.

---

## `bbddelay` — Analog BBD Delay

A bucket-brigade analog echo. A BBD is a fixed number of charge buckets clocked
at a variable rate, so the delay line's own sample rate IS the delay time — and
the anti-alias filter before it and the reconstruction filter after it both
track that clock. **A longer setting is therefore a darker echo**, and because
the pair sits inside the feedback loop, every repeat is darker than the one
before. The corner is `480 Hz·s / t`, clamped to 700 Hz – 12 kHz and dragged
further down by `age`.

`feedback` is not a plain loop coefficient. Two low-passes, a DC blocker and
an interpolator each take a little off every lap, so a raw 1.0 is a slow DECAY
rather than the runaway a maxed regen control is supposed to be; the excess is
weighted to the very top of the travel instead, and the soft limiter turns what
is above unity into a bounded oscillation. The limiter sits AFTER the regen
gain: limiting the echo and then multiplying by a gain above 1 puts more than
the ceiling into the line, and the repeat comes back over 0 dBFS.

The time knob **drags**. The read position is slew-limited to 0.25 samples of
delay per sample of audio, so a 100 → 400 ms move is a ~1.2 s downward pitch
dive, and the filter pair is re-derived from the CURRENT gliding delay (once
per 1 % of movement) rather than jumping to the new corner.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `timeMs` | 20 – 600 | 300 | ms | Bucket clock. Moving it is a pitch dive, not a jump. |
| `feedback` | 0 – **1** | 0.4 | — | Regeneration. The loop gain crosses unity at about 0.82, so the top fifth of the travel really oscillates; a soft limiter at 0.6, sitting AFTER the regen gain, is what bounds it. |
| `modRateHz` | 0.1 – 5 | 0.5 | Hz | Clock-modulation LFO (sine). |
| `modDepth` | 0 – 1 | 0.1 | — | Modulation depth, up to 6 ms of delay movement. |
| `age` | 0 – 1 | 0.3 | — | A tired chip: recirculated clock hiss (∝ age²) plus up to 60 % more bandwidth loss on top of the clock tracking. |
| `mix` | 0 – 1 | 0.4 | — | Dry/wet. |

**Modelled on** an MN3005-based analog delay (Deluxe Memory Man, DM-2, Carbon
Copy): the darkening-with-length and darkening-with-repeat behaviour is the
identity of the circuit, not a tone control bolted on.

**Using it.** Leave `age` around 0.3 and `modDepth` 0.1 for a warm slapback
that sits under a clean tone. Push `feedback` past ~0.82 and ride `timeMs` for the
classic oscillating-delay noise; it builds to a steady ~0.55 and stays under
0 dBFS for as long as you leave it there. `modDepth 1` with `modRateHz 0.3` is a chorused analog echo.

---

## `granular` — Granular Cloud

A capture ring the input is written into continuously, and up to 32
CONCURRENT grains reading it back. Each grain is a Hann-windowed read at
`2^(pitchSemitones/12)` of real time, launched from a `spray`-scattered
position; a new grain is launched every `grainMs / density` samples, so the
grains always overlap. `freeze` stops the WRITE and leaves the cloud circling
what is already in the ring.

**A slot still mid-window is left to finish.** The round robin returns to a slot
after 32 hops — exactly one grain's life while `grainMs` is steady, which is why
the scheduler can be O(1). LOWER `grainMs` and the cycle drops below the life of
the grains already in flight, so a slot at a non-zero window position would be
overwritten: a discontinuity, and a replacement started at full window position 0
instead of faded in. That launch is skipped instead (one grain out of a cloud of
up to 32; `Granular_ShorteningTheGrain_DoesNotCutTheOnesInFlight` measured 0.0174
of step against 0.0062 for the same render with the knob left alone).

The Hann table is built once per `prepare` and indexed by each grain's
normalised position — a per-grain window rebuild would be an O(grainLen) burst
inside the callback that launched it. Scheduling is one launch per hop,
O(density) reads per sample, zero allocation. Measured worst case (density 32,
`grainMs` 500, stereo, 48 kHz): **212, 406 and 516 ns/sample on three runs of
one box** (the low figure is an idle box, the high one a box building alongside), against a 20,833 ns/sample block budget. It is a wall-clock difference
against an empty rig, so re-measure rather than quoting either figure —
`RtDelayGearRigIsAllocationFree` prints it on every gate run.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `grainMs` | 10 – 500 | 80 | ms | Grain length. Short is a texture, long is a smear. |
| `density` | 1 – 32, step 1 | 8 | count | CONCURRENT grains. The output is normalised by `2/density`, so the knob is not a volume control. |
| `pitchSemitones` | −12 – +12, step 1 | 0 | semitones | Grain playback rate. |
| `spray` | 0 – 1 | 0.3 | — | Random read-position scatter, up to one grain length. Deterministic (the PRNG is seeded in `prepare`). |
| `feedback` | 0 – 0.9 | 0.2 | — | Writes the cloud back into the ring. Soft-limited and DC-blocked; ignored while frozen. |
| `mix` | 0 – 1 | 0.5 | — | Dry/wet. |
| `freeze` | Off / On | Off | — | Stops writing the ring. The cloud holds what it captured, for ever. |

**Modelled on** the grain-delay family — Mountain of a Thousand Faces,
Microcosm, Clouds — rather than on any one pedal.

**Using it.** `grainMs 80`, `density 8`, `pitch +12` over a held chord is the
shimmer that needs no reverb. `freeze On` with `mix 1` turns the pedal into a
pad you play over; the capture is whatever was in the ring at the moment you
hit it, so hit it on a chord. High `spray` with long grains is the
"where-is-the-beat" texture; `spray 0` keeps every grain on the same offset and
the pitch shift clean.

---

## `rhythmdelay` — Rhythmic Tremolo Delay

A delay whose REPEATS are chopped by an eight-step gate sequencer, so the tail
comes back as a rhythm instead of a smear. The gate is on the wet read ONLY —
outside the feedback loop — so the pattern shapes what you hear without
compounding itself once per lap.

Every step edge is a ramp `gateShape` of a step long (floored at 1 ms), and a
PATTERN change is nothing special: it only moves the gate smoother's target, so
switching patterns mid-step crossfades for free.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `timeMs` | 10 – 2000 | 500 | ms | Echo time. |
| `feedback` | 0 – 0.9 | 0.4 | — | Repeats. DC-blocked and soft-limited in the loop. |
| `pattern` | 8 named | `x.x.x.x.` | — | `x.x.x.x.`, `xx.xx.x.`, `x..x..x.`, `xxx.xxx.`, `x.xx.x.x`, `x...x...`, `xxxx....`, `x.x.xx..`. `x` = heard, `.` = gated. |
| `stepMs` | 50 – 1000 | 125 | ms | One step of the sequencer. A shorter step takes effect on the step in flight. |
| `gateDepth` | 0 – 1 | 1 | — | How far a `.` step drops. **0 is a true no-op** — the multiplier is exactly `1.0f` and the module is bit-for-bit a plain delay. |
| `gateShape` | 0 – 1 | 0.2 | — | Edge slope as a fraction of the step. 0 is the sharpest chop the core allows, never a click. |
| `mix` | 0 – 1 | 0.5 | — | Dry/wet. |

**Modelled on** the "rhythmic delay" / pattern-tremolo pairing you would
otherwise build from two pedals — a Timeline `Trem` machine feeding a delay, or
a tremolo placed after one.

**Using it.** Set `stepMs` to the sixteenth of the song and pick a pattern that
is NOT the one the drummer is playing — the delay is more interesting
answering the part than doubling it. `xxxx....` at a long `timeMs` gives
bursts; `x...x...` at a short one gives a stuttered slapback. `gateShape` under
0.1 is percussive, over 0.5 is a pulsing swell.
