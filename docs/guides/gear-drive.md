# Drive gear

Modules in the `drive` drawer added in the 2026-09 gear wave. Each section names the
module's type id, its params (id, range, default, unit) and what it is modelled on.
See [The DSP Tone Rig](tone-rig.md) for how modules are hosted.

Every module here follows the drawer's house rules: the clipping stage runs inside a
true oversampled wrap (8× for the hard clippers and the rectifier, 4× for the soft
saturators), every asymmetric stage is followed by a DC blocker, every tone control is
an RC-derived biquad rather than "a low-pass", every in-path bass filter is a
**bounded shelf** and never an unbounded high-pass, and every user-facing level, mix,
blend and footswitch is crossfaded over 10 ms instead of stepped.

The numbers below are the ones `tests/JuceSharp.Tests/Dsp/GearDriveTests.cs` measures
on every run — if one of them moves, that test fails and this page is wrong. Each of
those assertions is a **two-sided** `Assert.InRange` centred on the figure printed
here (±1.5 dB for a level, ±3 points for a THD percentage). They were one-sided
floors until 2026-09-12, 1.2 to 16 dB looser than the numbers they were supposed to
pin, so the page could have rotted in silence.

Every module here is also **silent from digital silence**. That is not free: five of
the seven run a gain stage with a fixed operating-point bias, which emits DC into the
DC blocker behind it, and a blocker starting from zero state answers a DC step with a
full-amplitude decaying pulse. Each core therefore primes itself on a quarter second
of silence in `prepare()` and `reset()`, so adding the pedal, changing the device or
un-bypassing it does not thump (`SilenceIn_StaysBelowTheFloor_FromTheVeryFirstBlock`,
floor −60 dBFS RMS; before it, ds1 measured −28.0 and tubepre peaked at −21.2 dBFS).

---

## `rat` — Rodent Distortion

An op-amp distortion of the ProCo RAT family: a very high gain LM308-style stage
whose feedback leg sheds bass as the gain opens, a treble-emphasis cap before the
clipper, shunt diodes to ground, and the pedal's single passive RC **Filter**
control.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `distortion` | 0 – 1 | 0.5 | — | op-amp gain into the diodes, 0 dB … +45 dB |
| `filter` | 0 – 1 | 0.4 | — | the passive low-pass, 12 kHz fully anticlockwise down to 500 Hz fully clockwise |
| `volume` | 0 – 1 | 0.6 | — | output level |
| `clip` | 0 – 2 | 0 (Silicon) | named | diode pair: `Silicon` (stock, tight), `LED` (more headroom, open and loud), `Turbo` (one leg lifted: asymmetric and spitty) |

**Clockwise on `filter` is DARKER.** That is the real pedal and it is not a bug; the
measured 5 kHz-against-500 Hz balance falls 16.3 dB from `filter` 0 to `filter` 1.
At `distortion` 1 a 200 Hz sine measures 46.5 % THD. The three `clip` legs are gain
matched, so the switch is a change of texture rather than of level, and it
crossfades — both thresholds and the makeup gain are smoothed.

---

## `ds1` — Orange Distortion

A BOSS DS-1 style pedal: a discrete transistor booster that runs even with the dist
knob down, an op-amp stage with asymmetric silicon diodes in its **feedback** leg
(soft knee, not a shunt hard clip), and the passive tilt tone stack.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `dist` | 0 – 1 | 0.5 | — | op-amp drive, 0 dB … +42 dB |
| `tone` | 0 – 1 | 0.5 | — | passive tilt around 700 Hz; 0 is the low-pass leg, 1 the high-pass leg, 0.5 is exactly flat |
| `level` | 0 – 1 | 0.6 | — | output level |

The tone control is a **tilt**, not one sweeping filter: a low-pass leg and its exact
complement, summed in opposite proportions, so noon is flat and the two ends are a
real low-pass and a real high-pass. The measured 2 kHz-against-200 Hz balance moves
20.0 dB from end to end. THD rises monotonically with `dist` (2.5 % → 48.9 % on a
200 Hz sine at −9 dBFS).

---

## `hm2` — Chainsaw Distortion

A BOSS HM-2 style pedal: two cascaded clipping stages into the famous "colour mix"
pair of **resonant** boosts, with a fixed scooped mid between them. Everything full
is the Swedish buzzsaw.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `dist` | 0 – 1 | 0.7 | — | drive into the cascade, 0 dB … +46 dB |
| `lowDb` | 0 – 12 | 10 | dB | resonant colour boost centred on 80 Hz |
| `highDb` | 0 – 12 | 10 | dB | resonant colour boost centred on 1 kHz |
| `level` | 0 – 1 | 0.5 | — | output level |

Both colour knobs sit **after** the clippers, which is why they measure exactly what
they are labelled with (10.00 dB at 80 Hz and at 1 kHz for a 0 → 10 move) instead of
being compressed away. `dist` 1 measures 45.1 % THD. Start with both colours at
10 and pull `highDb` back if the mid gets lost.

---

## `tapesat` — Tape Saturation

The record chain of an analogue tape deck: pre-emphasis into the oxide's saturation,
the matching de-emphasis on playback — which is where tape's soft HF *compression*
comes from — the playback head's low-frequency bump, the gap-loss roll-off, and a
switchable noise floor.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `drive` | 0 – 1 | 0.3 | — | record level onto the tape, −10 dB … +18 dB |
| `bias` | 0 – 1 | 0.5 | — | record-bias asymmetry; 0.5 is symmetric, either end adds even harmonics |
| `hfRolloffKHz` | 4 – 20 | 12 | kHz | gap-loss corner (two cascaded 2nd-order sections) |
| `hiss` | 0 – 1 | 0 | — | tape noise; 1.0 is a −70 dBFS RMS floor |
| `mix` | 0 – 1 | 1 | — | dry/wet, with the dry leg delay-compensated |

The knob's bottom end sits **below** unity on purpose, so `drive` 0 really is a wire
(the saturator has 10 dB of headroom over a full-scale input) rather than merely
quiet; at `drive` 1 a −3 dBFS input measures 12.4 dB of compression. `hfRolloffKHz`
6 against 20 moves 10 kHz by 22.1 dB (19.5 dB until 2026-10, when a roll-off past a
quarter of the rate was still held at 12 kHz at 48 kHz — DotCharter #87). `hiss` 0 adds **nothing at all** — silence in
is bit-identical silence out — and the generator is seeded per channel in `prepare`,
so a bounce is reproducible sample for sample.

This module reports a few samples of latency (the dry leg's compensation delay), and
it reports the same number whatever `mix` is set to, so dragging that knob never
churns the chain's delay compensation.

---

## `bassod` — Bass Overdrive

The "drive plus clean" topology every usable bass dirt box uses: the distorted leg is
allowed to thin out, and the fundamental is put back by an untouched clean leg summed
alongside it, with a two-band shelving tone stack after the sum.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `drive` | 0 – 1 | 0.4 | — | into the clipper, 0 dB … +32 dB |
| `blend` | 0 – 1 | 0.5 | — | 0 is the clean leg alone, 1 the driven leg alone |
| `bass` | −12 – +12 | 0 | dB | low shelf at 100 Hz |
| `treble` | −12 – +12 | 0 | dB | high shelf at 3 kHz |
| `level` | 0 – 1 | 0.6 | — | output level |

The clean leg is delayed to match the driven leg's oversampler, so the blend is a sum
and not a comb: at `blend` 0 the 41 Hz fundamental of a low E sits within 0.01 dB of
where it does in the input, against 7.1 dB down through the driven leg alone. Both
tone controls are shelves, so neither can run away when this is cascaded with an
amp's own network. The module reports a constant few samples of latency.

---

## `tubepre` — Tube Preamp / DI

Two asymmetric triode-style gain stages with a coupling network between them, a
bright cap across the gain pot, a passive tilt tone stack, a **supply-sag** model, and
a switchable speaker-emulating DI curve.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `gain` | 0 – 1 | 0.4 | — | first-stage drive, 0 dB … +36 dB |
| `sag` | 0 – 1 | 0.3 | — | how far the B+ rail droops under load, lowering the second stage's clipping threshold |
| `bright` | 0 / 1 | Off | named | the bright cap: a +7 dB shelf above 2 kHz |
| `tone` | 0 – 1 | 0.5 | — | passive tilt around 800 Hz; 0.5 is flat |
| `level` | 0 – 1 | 0.6 | — | output level |
| `cabSim` | 0 / 1 | Off | named | the DI cab curve: a 4th-order 5 kHz roll-off, a beam-blocker dip above it, and a bounded (−18 dB) low end |

`sag` is the reason to reach for this over a plain overdrive: with it at 1 a
sustained loud note measures 16.9 dB lower 500 ms in than it does over its first
20 ms, and blooms back as the note decays; with it at 0 the level does not move at
all (−0.01 dB). `cabSim` drops 8 kHz by 27.0 dB while leaving 800 Hz essentially
alone, so it is a cab and not a mute — switch it on when going straight to a desk,
off when there is a real cab module after this one. Both switches crossfade rather
than switching a filter in, so neither clicks.

---

## `octfuzz` — Octave Fuzz

A Tycobrahe Octavia / Foxx Tone Machine style octave-up fuzz: a driven germanium fuzz
stage feeding a **full-wave rectifier**, which folds the negative half of the wave
onto the positive one. A rectified sine contains only even harmonics, so the
fundamental disappears and the octave above it becomes the loudest thing there.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `fuzz` | 0 – 1 | 0.7 | — | input drive, 0 dB … +40 dB |
| `octave` | 0 – 1 | 0.6 | — | crossfade into the rectified octave-up leg |
| `tone` | 0 – 1 | 0.5 | — | bounded ±10 dB shelf at 1.2 kHz |
| `volume` | 0 – 1 | 0.5 | — | output level |

At `octave` 1 a 200 Hz sine comes out with its 400 Hz component 148 dB above what is
left of the fundamental; at `octave` 0 the pedal is a plain fuzz and the fundamental
is 37 dB above the octave. It is loudest and cleanest above the 12th fret on the top
two strings — that is a property of the circuit, not of this model. The rectifier is
ADAA'd as well as oversampled 8×, because `|x|` has a corner at zero and is a far
worse aliaser than any smooth clipper.

---

## Aliasing

Measured on a two-tone (999 Hz + 1100 Hz) at 48 kHz against the same render at
192 kHz, expressed against the 1 kHz fundamental
(`GearDriveTests.AliasProducts_StayBelowMinusSixtyDb_AtDefaultsAndDriven`). Both
columns are measured: an alias theory at catalogue DEFAULTS alone can be vacuous, so
each module is also driven with its dirt knob at maximum.

| module | at the defaults | driven | driven, and with `kOs` forced to 1 |
|---|---|---|---|
| `rat` | −94.7 dB | −81.2 dB | **−45.6 dB** |
| `ds1` | −88.1 dB | −75.9 dB | **−41.4 dB** |
| `hm2` | −81.5 dB | −78.2 dB | **−49.0 dB** |
| `tapesat` | −133.8 dB | −147.5 dB | −131.7 dB |
| `bassod` | −106.8 dB | −107.3 dB | **−46.4 dB** |
| `tubepre` | −91.7 dB | −63.1 dB | **−39.6 dB** |
| `octfuzz` | −66.0 dB | −72.0 dB | **−55.5 dB** |

The third column is what makes the second one worth having, and it was measured
(2026-09-12) rather than assumed. At 1× the DEFAULTS rows already go red for four of
the seven — ds1 −45.1, hm2 −49.3, tubepre −49.6, octfuzz −56.8 — so a defaults-only
theory was not vacuous for those. It was for `rat` (−67.6 at 1×), `bassod` (−137.8)
and `tapesat` (−153.8), which pass with no oversampling in them at all; driving them
closes two of the three. **`tapesat` stays green at 1× even at `drive` 1**, so no
setting in its range makes its own alias theory bite: its knob maps to a gentle tanh
under a 12 kHz roll-off, and the 4× wrap is insurance rather than a requirement at
these settings.

`octfuzz` is driven on `fuzz` alone. At `octave` 1 the rectifier all but annihilates
the probe tone, and this measurement normalises every bin by the fundamental, so the
denominator collapses and every legitimate product reads as a +104 dB "alias" — the
metric breaking down, not the pedal aliasing.
