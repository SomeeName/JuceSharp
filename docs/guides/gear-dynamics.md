# Dynamics gear

Modules in the `dynamics` drawer added in the 2026-09 gear wave. Each section names the
module's type id, its params (id, range, default, unit) and what it is modelled on.
See [The DSP Tone Rig](tone-rig.md) for how modules are hosted.

Every module here is pinned by `GearDynamicsTests` (managed, offline) and, together,
by the RT gate's `RtDynamicsGearIsAllocationFreeAndReportsItsLatency`. Two of them
accept an external sidechain key (`expander`, `ducker`); the other four have no
external detector and the engine **rejects** a key on them.

---

## `limiter` — Pedal Limiter

A look-ahead brick-wall limiter as a pedal: the ceiling is a guarantee, not a
tendency.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `ceilingDb` | −20 … 0 | −1 | dBFS | the level the output never crosses |
| `releaseMs` | 5 … 500 | 80 | ms | how fast the gain recovers after a peak |
| `lookaheadMs` | 0 … 5 | 1 | ms | how far ahead the detector sees. **Reported as latency** |
| `gainDb` | −12 … +12 | 0 | dB | **input** gain, applied before the detector |

**Modelled on** the brick-wall limiter at the end of a pedalboard (Boss LM-2 /
Keeley territory, but with a real look-ahead rather than a feedback cell). The
topology is the one `src/MasterLimiter.h` runs on the master bus, re-implemented
as a pure `dcdsp` core: peak detector → **sliding minimum** over the look-ahead
window (a monotonic deque, O(1) amortised) → per-sample one-pole → applied to a
look-ahead-**delayed** copy, with a clamp at the ceiling as the backstop.

`gainDb` is an INPUT gain on purpose — put it after the limiter and the ceiling
stops being a guarantee. `lookaheadMs` moves a read tap, so a change is
crossfaded over 10 ms rather than spliced, and the ring is allocated in
`prepare()` for the param's 5 ms maximum and never resized. A change asked for
while a crossfade is still in flight is ADOPTED on the callback that sees the
fade land, not applied at once: restarting the fade at zero was itself a splice,
inside the mechanism that exists to prevent splices. `latencySamples()` reports
the tap the audio path settles ON (never 0: a zero-length look-ahead is a
clipper, so the tap floors at one sample), which is what the chain's branch PDC
budgets for; during the 10 ms fade the path is reading a blend of two taps,
which no single number describes.

That crossfade was unreachable by any test until 2026-09-12 — it could be
deleted outright with the whole suite staying green.
`Limiter_MovingTheLookaheadTap_DoesNotSpliceTheWaveform` measures it the way a
tap has to be measured: the largest sample-to-sample step across the seam,
against the same render with the knob left alone.

> Put it last on the board and set the ceiling where your interface wants it.
> Drive it with `gainDb` rather than with the pedal in front, so what you hear
> getting louder is the limiter working and not the drive changing character.
> A long `releaseMs` on a strummed part breathes; a short one pumps.

---

## `transient` — Transient Shaper

Attack and sustain as two independent knobs, with no threshold to ride.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `attackDb` | −12 … +12 | 0 | dB | gain applied while the note is ATTACKING |
| `sustainDb` | −12 … +12 | 0 | dB | gain applied while it is DECAYING |
| `sensitivity` | 0 … 1 | 0.5 | — | how little envelope difference already counts as a full attack |
| `mix` | 0 … 1 | 1 | — | dry/wet |

**Modelled on** the SPL Transient Designer / Boss TAE: two envelope followers on
one linked detector, one FAST (0.5 ms / 20 ms) and one SLOW (20 ms / 150 ms).
Their difference in dB says where in the note you are — fast above slow is an
attack, fast below slow is a decay — and each knob gains one phase. There is no
threshold control because there is no threshold: the same setting does the same
thing to a quiet note and a loud one.

`sensitivity` sets the dB span that counts as "fully an attack" (18 dB at 0,
5 dB at 1). Because the applied gain is `attackDb · diffDb / span`, that span is
a MULTIPLIER on the gain — 0 → 1 moves it by up to 3.6× — so it is smoothed as
`1/span` over the same 10 ms as the two dB knobs, and it is the drawer's one
"depth" in `RampCases`. It was an unsmoothed `double` until 2026-09-12 and moved
the applied gain from +3 dB to +12 dB in one sample, which this page's own
"every user-facing gain, depth and mix ramps" already forbade.

Both followers are driven from ONE linked detector (max |x| over
the prepared channels); per-channel followers would give each channel its own
attack gain and pan every pick across the stereo image.

> Set `attackDb` to +4 and leave `sustainDb` at 0 to get the pick back after a
> compressor has eaten it. Negative `sustainDb` tightens a boomy room without
> touching the attack — which is the thing a gate cannot do.

---

## `multiband` — Multiband Compressor

Three bands, one set of ballistics, Linkwitz-Riley 4th-order crossovers.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `lowXoverHz` | 60 … 500 | 150 | Hz | low/mid split |
| `highXoverHz` | 1000 … 8000 | 2500 | Hz | mid/high split (pushed up if it would cross the low one) |
| `lowThresholdDb` / `midThresholdDb` / `highThresholdDb` | −60 … 0 | −20 | dBFS | per-band knee centre |
| `lowRatio` / `midRatio` / `highRatio` | 1 … 20 | 3 | :1 | per-band ratio; 1 is a wire |
| `lowGainDb` / `midGainDb` / `highGainDb` | −12 … +12 | 0 | dB | per-band makeup |
| `attackMs` | 0.1 … 100 | 10 | ms | shared by all three bands |
| `releaseMs` | 5 … 1000 | 120 | ms | shared by all three bands |

**Modelled on** a studio multiband (dbx / Waves C3 shape) shrunk to a pedal. Each
crossover leg is two cascaded Butterworth (Q = 1/√2) sections, which is what
makes the two halves of a split sum to an **allpass** — so at ratio 1 everywhere
the module is a wire in level terms, measured flat to better than 0.1 dB from
40 Hz to 16 kHz.

The thing that is easy to get wrong: the LOW band also has to pass through the
SECOND crossover and have both of its outputs summed, or the three legs do not
sum flat. It only shows up once the two splits are close enough to interact —
at 150 / 2500 Hz a low band that skipped the compensator still measures flat, and
at 500 / 1000 Hz it is 3.2 dB down at 700 Hz.

Detectors are linked across channels per band, so compression never shifts the
stereo image, and the three bands share one pair of ballistics so they cannot
argue about timing.

> Ratio 1 in the bands you do not want touched — that costs nothing and keeps
> them provably flat. The usual guitar setting is low-band only: 150 Hz,
> `lowRatio` 4, `lowThresholdDb` around −24, which tames a boomy low E without
> dulling the pick.

---

## `expander` — Expander

The gate's quieter sibling: a curve instead of a door. Sidechain-capable.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `thresholdDb` | −80 … 0 | −40 | dBFS | expansion starts below this |
| `ratio` | 1 … 10 | 2 | :1 | (ratio − 1) dB of attenuation per dB below the threshold |
| `attackMs` | 0.1 … 100 | 10 | ms | how fast the gain comes UP (the expander opening) |
| `releaseMs` | 5 … 1000 | 120 | ms | how fast it goes DOWN |
| `rangeDb` | 0 … 60 | 30 | dB | the most it may ever take |

**Modelled on** the downward expander half of a studio gate. Where
`dcdsp_gate.h` is a hysteresis state machine that opens and shuts, this is a
continuous curve: a noise floor 10 dB under the threshold at 2:1 loses 10 dB and
a note that pokes through loses nothing. `rangeDb` is the floor that keeps it a
musical expander rather than a slow gate.

Its detector releases in 50 ms, not the gate's 10 ms, and the difference is
measurable: this module's contract is a curve, so an envelope that sags between
the peaks of a waveform is an error **multiplied by (ratio − 1)**. A gate only
compares its envelope against a threshold and pays nothing for the same sag.

**Sidechain:** accepts a key (`JuceDspSidechainSource.SeatInput` / `SongMix`).
An unavailable source falls back to the module's own input, never to silence.

> Reach for this instead of a gate when the noise and the note overlap — at 2:1
> a quiet passage gets quieter without a door slamming behind it. Keyed from the
> seat's dry input, an expander at the END of a chain follows your PICK rather
> than the hiss the drive in front of it is making.

---

## `optocomp` — Optical Compressor

One knob, no ratio control, and a release that remembers.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `peakReduction` | 0 … 1 | 0.4 | — | how hard the cell works (moves the threshold, −6 → −42 dBFS) |
| `gainDb` | 0 … 24 | 0 | dB | output makeup |
| `limit` | 0 / 1 | 0 (`Compress`) | named | `Compress` ≈ 3:1, `Limit` ≈ 15:1 |
| `emphasis` | 0 … 1 | 0.3 | — | high-frequency emphasis in the DETECTOR only |

**Modelled on** the LA-2A / Diamond opto cell: a lamp drives a photoresistor, so
the release is not one time constant — the cell recovers quickly from a short hit
and slowly from one it has been holding, and that program dependence is most of
what people mean by "the LA-2A sound". The knee is wide (10 dB) and soft.

The reduction is split into two components that attack together and release
separately: a fast one (60 ms) and a slow one whose time constant is driven by
`mem_`, a one-pole that charges while the cell is working (300 ms when it has
just started, up to 3 s after it has been held). Their SUM is the applied
reduction, so the classic fast-then-slow recovery falls out with no branch and
no "which stage am I in" state to get wrong. Measured: recovery after a 2 s hold
takes several times longer than after a 50 ms one.

`limit` is the one DISCRETE param in this wave — it carries `valueLabels`, so a
palette renders a two-way picker rather than a knob parked at "0.5 modes".

> Set `peakReduction` by ear until the meter moves 3–6 dB on your hardest pick,
> then bring the level back with `gainDb`. Leave it in `Compress` for rhythm and
> switch to `Limit` for a solo boost. `emphasis` only changes what the cell
> LISTENS to, so turning it up makes the compressor duck on pick attack without
> dulling the tone.

---

## `ducker` — Ducker

Sidechain-keyed attenuation with a hold timer. The one module here whose whole
point is the key.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `thresholdDb` | −60 … 0 | −30 | dBFS | key level that triggers the duck |
| `duckDb` | 0 … 40 | 12 | dB | how far down the through signal goes |
| `attackMs` | 1 … 200 | 10 | ms | how fast it dips |
| `holdMs` | 0 … 500 | 50 | ms | stays down this long after the key stops |
| `releaseMs` | 10 … 2000 | 300 | ms | how fast it comes back |

**Modelled on** the broadcast voice-over ducker. It is deliberately NOT a
compressor: the attenuation does not follow how far over the threshold the key
is, which is what makes it sound like a decision rather than pumping. The hold
timer is re-armed on every sample the key is over, so a sustained key never
releases in the middle of itself.

**Sidechain:** `kFlagSidechain`. With no external key it keys off its own input
— the engine's documented fallback for an unavailable source, never silence.
At rest it is a true wire (the gain snaps the last hundredth of a dB to unity
rather than parking at 0.9999 for ever).

> Key it from the **song mix** and put it on the seat's rig: the backing track
> then rides under your solo automatically instead of you reaching for a level.
> Key it from the **seat input** with a long `holdMs` and it ducks a delay or
> reverb send while you are playing and lets the tail back up when you stop.

---

## Shared invariants

All six follow the rig's standing rules, and each is pinned:

- **Every user-facing gain, depth and mix ramps** over 10 ms
  (`SmoothedValue`) — `ChangingAGainOrDepth_WalksAcrossTheSeam_InsteadOfStepping`
  renders the same stimulus three times and requires the seam to still be near
  the OLD value 128 samples in.
- **`prepare()` re-derives everything the rate owns and clears no user content** —
  `PrepareReDerivesEveryRateOwnedQuantity_SoTheTimingIsTheSameAtEveryRate`
  compares both where a module settles and how long it takes to get there, at
  48 kHz and 192 kHz.
- **No core here sizes any buffer by the BLOCK.** Everything is sized by the
  channel count or by a param's maximum, so there is no oversized-block hazard
  at all: `ARender_IsIndependentOfHowItIsCutIntoBlocks` renders the same audio
  in one shot and in 577-frame pieces and requires the samples to match.
  A block wider in CHANNELS than `prepare()` saw is clamped to the prepared
  count, as the compressor already does.
- **`process()` allocates nothing, locks nothing and does no I/O** — the RT gate
  plus a source lint (`NoDynamicsCore_AllocatesOrLocksInsideProcess`) over the
  six core headers.
- **Params clamp NaN-safely and a NaN is rejected at the chain** —
  `ANaNParam_IsRejected_AndTheModuleStillWorks` sweeps every param of every
  module and then proves the module is still making sound.
- **Adapter float params reach their core through `ParamMailbox`**, never a
  synchronous `core_.setParams` from the message thread.
