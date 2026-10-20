# Pitch gear

Modules in the `pitch` drawer added in the 2026-09 gear wave. Each section names the
module's type id, its params (id, range, default, unit) and what it is modelled on.
See [The DSP Tone Rig](tone-rig.md) for how modules are hosted.

All four are built from four shared blocks in
`src/dsp/cores/dcdsp_detail_pitchgear.h`:

* **`GrainShifter`** — a two-tap crossfaded delay-line pitch shifter. Reading a
  delay line whose delay ramps at `1 - ratio` shifts the pitch; the ramp is
  wrapped over a window and two taps half a window apart are crossfaded with
  sin/cos gains, so each tap's gain is zero at the instant it wraps. Optionally
  the wrap point is chosen by cross-correlation instead of by the clock, which
  is what an octave shift needs (see `twelvestring` below).
* **`AutocorrTracker`** — a monophonic pitch tracker: normalised autocorrelation
  over a ×4-decimated copy (a 43 ms window at a 12 kHz working rate), then nine
  full-rate lags around the coarse answer plus a parabolic fit, which is what
  gets it from ~40 cents of lag quantisation down to under half a cent.
* **`LpcFormant`** — a 12th-order LPC whiten / re-dress pair, so a shifted note
  keeps its body resonances instead of moving them (`pitchcorrect`'s Formant
  switch). Time domain, never a phase vocoder — that is banned on the live input
  everywhere in this repo.
* **`PolyBlepOsc`** — a band-limited saw / square / PWM / sine oscillator.

Everything is allocated in `prepare()`; nothing on `process()` allocates, locks
or resizes. The allocation gate, the oversized-block guard and the latency
contract are pinned by the RT scenario
`RtPitchGearIsAllocationFreeUnderScaleOctaveAndLatencyFlips`; the audible
behaviour by `tests/JuceSharp.Tests/Dsp/GearPitchTests.cs`.

---

## `detune` — Detune / Micro-Pitch

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `cents` | 0 … 50 | 12 | cents | The symmetric interval. Voice A is this far sharp, voice B this far flat. |
| `spreadMs` | 0 … 30 | 8 | ms | Extra delay on voice B, so the two voices never phase into one another. |
| `width` | 0 … 1 | 1 | — | 1 puts voice A hard left and voice B hard right; 0 sums both into every channel. |
| `mix` | 0 … 1 | 0.5 | — | Dry/wet. |
| `lowLatency` | Normal / Low | Normal | — | Picks the crossfade window: 40 ms (21 ms of delay) or 8 ms (5 ms of delay). |

**Modelled on** an Eventide H910/H3000 "micro pitch" patch, a TC Chorus+ in
detune mode, and the thickening in the Boss CE/DC family. There is no LFO, and
that is what makes it a detuner rather than a chorus: the beating comes from a
constant interval, so it never warbles.

**Using it.** Start at `mix` 0.5, `cents` 10–15 and `width` 1 for a wide,
still-mono-compatible double; push `cents` past 30 for a chorus-like shimmer.
`spreadMs` is the difference between "one thick note" (0–4 ms) and "two players"
(15–30 ms). Switch `lowLatency` to Low if you monitor yourself through the rig —
5 ms is under what reads as a doubled attack; leave it Normal for recorded or
low-tuned parts, where the longer window is smoother on the bottom strings.

**Latency:** 21 ms in Normal, 5 ms in Low, reported through `latencySamples()`
so branch PDC compensates for it. Voice B's `spreadMs` sits on top of that by
design — that offset is the effect, not latency. A latency-mode switch is a
delay-tap move and glides over 80 ms, which is a short downward bend rather than
a click.

---

## `pitchcorrect` — Pitch Correction

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `scale` | Chromatic / Major / Minor / Pentatonic / Blues | Chromatic | — | Which notes the pitch may be snapped to. |
| `key` | C … B | C | — | The root the scale is rooted at. Ignored for Chromatic. |
| `speedMs` | 0 … 500 | 50 | ms | How long the glide onto the corrected pitch takes. 0 is the hard "robot" snap. |
| `amount` | 0 … 1 | 1 | — | How much of the measured deviation is removed. 0 is the dry signal. |
| `humanize` | 0 … 1 | 0 | — | The fraction of the original deviation LEFT IN. 1 is no correction at all, whatever `amount` says. |
| `formant` | Off / Preserve | Off | — | Preserve runs the shift through an LPC whiten/re-dress pair so the body resonances stay put. It GLIDES over 150 ms (below), not on one sample. |
| `mix` | 0 … 1 | 1 | — | Dry/wet. |

**Modelled on** a live scale-snapping box — an Auto-Tune "retune speed" patch as
a pedal does it, closest to a Boss VE or a TC Mic Mechanic.

**Using it.** For a corrective setting, pick the song's key and scale and leave
`speedMs` at 40–80 ms: fast enough that the note starts in tune, slow enough that
vibrato survives. `speedMs` 0 with Chromatic is the hard T-Pain effect.
`humanize` around 0.2 keeps the performance's own intonation while removing the
`formant` substitutes `synth(shift(whiten(x)))` for `shift(x)` — the whole wet path — so it
cannot be switched on a sample boundary. It walks the LPC pair's BANDWIDTH EXPANSION from 0 to
1 instead: every pole of the prediction filter is scaled toward the origin, so 0 is exactly the
plain path, 1 is exactly the formant path, and every setting between is a genuine inverse pair.
Blending the two END STATES is not the same thing and does not work — an all-pole synthesis
filter fed unwhitened audio rings, measured as a 2.64 output peak against a 0.26 input. The
glide is 150 ms, several times the shifter's own pipeline, because the front end whitens sample
*n* at today's setting while the back end re-dresses audio whitened ~21 ms ago. The LPC also
runs while `formant` is OFF, so its rings and coefficient sets are warm the moment the switch
lands; the result is discarded and the audio is bit-identical to not running it.

Turn `formant` on for anything over about a semitone of correction,
or the tone goes thin as the resonances move with the note.

**Honest limits.** It is **monophonic only**. The tracker resolves one period,
so a chord has no answer it can use: the normalised correlation collapses below
the confidence floor and the module HOLDS its last ratio rather than chasing
noise — a strum passes through at whatever correction the last single note asked
for. Tracking range is **50 Hz to 1.4 kHz**, which covers a bass low B up past
the 24th fret of a guitar's high E. A correction wider than **200 cents** is
refused, because at that size it is an octave slip or a chord, not a
performance. The first answer arrives about **45–55 ms** into a new note (one
analysis window plus a hop); that is a tracking delay, not latency, and is
deliberately not folded into the reported figure.

**Latency:** 29 ms — 1 ms of head start, 20 ms of half-window, and 8 ms for the
splice-alignment search, which moves the instantaneous tap by up to ±8 ms around
the nominal delay. Constant at every setting.

**Cost:** measured on the owner's box at 48 kHz, stereo, 256-frame blocks —
**94 ns/sample/channel** with `formant` Off, **150 ns/sample/channel** with it
On (Release, `/O2`, the core driven directly). Under the RT gate's instrumented
build the same case reads 370 ns/sample/channel, about 3.6 % of one core.

---

## `twelvestring` — 12-String Simulator

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `octaveLevel` | 0 … 1 | 0.6 | — | Level of the octave-up courses, under ~250 Hz (the E A D G pairs). |
| `unisonLevel` | 0 … 1 | 0.6 | — | Level of the detuned unison courses, above ~250 Hz (the B and E pairs). |
| `detuneCents` | 0 … 25 | 8 | cents | How far out of tune the unison course sits. |
| `delayMs` | 0 … 20 | 6 | ms | Course-strike offset — one pick, two strings. |
| `brightness` | 0 … 1 | 0.5 | — | A bounded ±6 dB high shelf at 2.5 kHz on the added courses only. Ramped over 10 ms and re-designed from the SMOOTHED value, only when it has moved 0.4 % (0.048 dB). |
| `mix` | 0 … 1 | 0.6 | — | How much course is summed with the original. The note itself is always there. |

**Modelled on** a Boss OC/CE-style "12 string" mode, an Electro-Harmonix Nano POG
patch and the twelve-string voice of a Roland GR.

**Why it is a two-band effect.** A real 12-string is not uniform: the four LOW
courses (E A D G) are octave pairs — a thin string tuned an octave up beside the
wound one — while the two HIGH courses (B E) are unison pairs that are never
quite in tune with each other. So the split is at 250 Hz (open G is 196 Hz, open
B is 247 Hz): below it, an octave-up voice; above it, a slightly detuned copy. A
full-spectrum octave-up would put a course above the high E, which no 12-string
has, so the crossover is 6th-order Butterworth and the test asserts the 880 Hz
is not there for a 440 Hz note.

**Using it.** Leave `mix` around 0.6 — the courses should sit under the note, not
replace it. `detuneCents` 6–10 is a well-kept instrument; past 15 it is a
twelve-string that needs tuning, which is sometimes the point. `delayMs` 4–8 ms
gives the loose strum; at 0 the courses sound like a synth doubling rather than a
second string.

**Latency:** ZERO, and deliberately. The dry path through this module is not
delayed at all; the added courses are, by `delayMs`, and that offset IS the
effect. Reporting the course delay would make branch PDC push every other branch
27 ms late to align against a signal that is never alone.

**Cost:** **176 ns/sample/channel** at 48 kHz stereo — the highest in the drawer,
because it runs a steep crossover and two aligned shifters per channel.

---

## `basssynth` — Bass Synth Tracker

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `waveform` | Square / Saw / Sub-Sine / PWM | Square | — | The oscillator. All four are band-limited (PolyBLEP); PWM sweeps its duty cycle at 0.3 Hz. |
| `octave` | -2 / -1 / 0 | -1 | — | Transposition of the tracked note. |
| `cutoffHz` | 40 … 8000 | 800 | Hz | Low-pass corner of the state-variable filter. |
| `resonance` | 0 … 1 | 0.4 | — | Filter Q, 0.7 to 8.7. |
| `envAmount` | 0 … 1 | 0.6 | — | How far the input envelope opens the filter, up to +6 kHz over `cutoffHz`. |
| `decayMs` | 10 … 2000 | 300 | ms | Level decay after the note stops. |
| `glideMs` | 0 … 300 | 20 | ms | Portamento between tracked notes. |
| `dryMix` | 0 … 1 | 0.3 | — | The original signal. |
| `synthMix` | 0 … 1 | 0.7 | — | The synth voice. |

**Modelled on** a Boss SYB, an Electro-Harmonix Bass Micro Synth and the synth
voice of a Roland GR. The input is never the SOURCE of the tone, only its
CONTROLLER: tracker → portamento → band-limited oscillator → SVF → VCA.

**Using it.** Keep some `dryMix` — the synth has no attack transient of its own
and the dry pick is what makes it sound played rather than triggered. `cutoffHz`
500–1200 with `envAmount` 0.5–0.8 is the classic filter-sweep-per-note; drop
`cutoffHz` to 100–200 with `octave` -2 for a sub that sits under a riff.
`glideMs` past about 150 ms turns a line into a lead, because each note slides
into the next rather than arriving.

**The voice is GATED until there is a note, and DC-blocked once there is.** Two
things made that necessary. The tracker needs ~45 ms before it is confident and
`freq_` starts at 0, while the input envelope's attack is 3 ms — so a frozen
oscillator (`pulse(0, 0, 0.5)` is exactly +1) came out at full envelope as a DC
thump for the first note, and for the whole of a strum that never becomes
confident at all. And the PWM waveform's duty sweeps [0.15, 0.85], so it carries
up to ±0.7 of LFO-modulated DC by construction. The voice is silenced below 20 Hz
(the low B of a 5-string is 30.9 Hz) and runs through a rate-derived 15 Hz
blocker. `BassSynth_EmitsNoDc_OnAChordItCannotTrack_NorOnItsPwmWaveform` pins
both halves.

**Honest limits.** Same tracker as `pitchcorrect`, so the same rules: it is
**monophonic only**, its range is **50 Hz to 1.4 kHz**, and a chord makes it hold
the last note it was sure of rather than chase the strum. It tracks a clean neck
pickup far better than a distorted signal — put it early in the chain, before any
drive. A new note is recognised about **45–55 ms** in, on top of whatever
`glideMs` asks for.

**Aliasing.** A naive saw or square is a step every cycle and a step is flat past
Nyquist, so its fold-back is inharmonic and lands anywhere. PolyBLEP subtracts a
band-limited step at each discontinuity: measured on a 220 Hz saw at 48 kHz, the
worst NON-harmonic partial is **−75 dB** relative to the fundamental (it is
−46 dB with the correction removed).

**Latency:** ZERO — the dry passes through untouched and the synth voice is
generated, not delayed.

**Cost:** **67 ns/sample/channel** at 48 kHz stereo (202 ns under the RT gate's
instrumented build, about 2 % of one core).
