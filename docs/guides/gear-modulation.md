# Modulation gear

Modules in the `modulation` drawer added in the 2026-09 gear wave. Each section names the
module's type id, its params (id, range, default, unit) and what it is modelled on.
See [The DSP Tone Rig](tone-rig.md) for how modules are hosted.

All five are pure `dcdsp` cores under `src/JuceSharp.Native/src/dsp/cores/`, adapted in
`src/dsp/gear/ModulationGear.cpp`, and pinned by
`tests/JuceSharp.Tests/Dsp/GearModulationTests.cs` plus the RT gate's
`RtModulationGearAllocatesNothingUnderStructureWrites`.

**No tempo sync.** `dcdsp_delay.h` derives its synced time from a `bpm` parameter the HOST
writes on that one module; there is no shared tempo source in the chain, so a second copy of
that convention here would be a second thing for the host to keep in step. Every rate below
is in hertz.

---

## `tzflanger` — Thru-Zero Flanger

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `rateHz` | 0.02 – 10 | 0.3 | Hz | sweep rate |
| `depthMs` | 0.1 – 10 | 3 | ms | sweep span either side of `manualMs` |
| `manualMs` | 0 – 10 | 3 | ms | the FIXED reference delay the swept path crosses |
| `feedback` | −0.95 – 0.95 | 0.3 | — | regeneration on the swept path (negative inverts) |
| `mix` | 0 – 1 | 0.5 | — | reference/swept blend |
| `shape` | Sine / Triangle | Sine | — | LFO waveform |

Modelled on the two-tape-machine flange (a reel is dragged so the two copies pass through
each other) rather than on a pedal. Both paths are delayed — the reference by `manualMs`, the
swept one by `manualMs ± depthMs` — so their RELATIVE delay passes through zero twice per LFO
cycle. At that instant the two are sample-identical and the 0.5/0.5 blend reconstructs the
input: a coherent sum, +6 dB over either half. An ordinary flanger delays only the swept path
and never reaches that null.

**`manualMs` is real latency and the module reports it** (`latencySamples`), so the chain's
PDC and the merger's branch alignment compensate for it.

Usage: start at `manualMs` = `depthMs` for the deepest through-zero; raise `feedback` toward
±0.9 for the jet whoosh; take `manualMs` to 0 and it is a plain flanger with no null.

---

## `barberpole` — Barber-Pole Phaser

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `rateHz` | 0.05 – 5 | 0.2 | Hz | how fast one voice crosses the range |
| `stages` | 2 – 12, step 2 | 6 | — | allpass sections per voice (even; notches come in pairs) |
| `depth` | 0 – 1 | 0.8 | — | how much allpass is summed with the dry inside a voice |
| `feedback` | −0.9 – 0.9 | 0.3 | — | per-voice regeneration |
| `direction` | Up / Down | Up | — | sign of the sweep |
| `mix` | 0 – 1 | 0.5 | — | dry/wet |

Modelled on the Shepard-tone phaser (the endless-rise trick): four independent phaser voices
share one sawtooth phase staggered by a quarter cycle each, sweeping 120 Hz to 5 kHz
logarithmically, and each is weighted by a raised cosine that is ZERO at both ends of its own
sweep. A voice is therefore silent when it wraps, and the notch pattern appears to climb for
ever. The four windows sum to exactly 2, so the level never pumps as the voices hand over.

The composite pattern repeats at 4× `rateHz` — that is the illusion working, not a bug. It is
also why there is no "the spectral centroid rises monotonically" test: the composite spectrum
is invariant under a quarter-cycle shift, so its centroid is periodic rather than rising, and a
test that claimed otherwise would be asserting the effect is BROKEN. What is pinned instead is
the property that makes the illusion possible — no step at the wrap
(`BarberPole_SweepsWithoutAStepAtTheWrap`, measured 0.85× the stimulus' own slope with the
window in place and 3.8× without it) — plus that `stages` changes the notch density and that
`direction` and `rateHz` do not click when they move.

Usage: slow (0.05 – 0.3 Hz) is the point; `stages` 12 with `depth` 1 gives the densest
pattern; `feedback` sharpens the notches into something more like a resonant filter.

---

## `autopan` — Auto-Pan

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `rateHz` | 0.05 – 10 | 1 | Hz | modulation rate |
| `depth` | 0 – 1 | 1 | — | how far across the pair the image travels |
| `shape` | Sine / Triangle / Square | Sine | — | modulator waveform |
| `phaseDeg` | 0 – 180 | 90 | ° | the RIGHT channel's LFO offset |
| `law` | Equal Power / Linear | Equal Power | — | pan law |

One oscillator read at two phases: the left gain comes from phase *p*, the right from
*p + phaseDeg*. At **180°** the two are in antiphase and this is a classic auto-pan, with
gL² + gR² exactly 1 under the equal-power law — the image walks across the pair and the sum
never pumps. At **0°** both sides move together, which is a tremolo. The default **90°** is
the quadrature motion of a source rotating around the listener.

The square is **slew-limited to a 10 ms full-scale transit**, re-derived from the device rate:
a raw square is a full-scale step twice a cycle, i.e. a click at any depth, and it is the one
modulator discontinuity that happens with no parameter moving.

**Neither law is unity at rest**, and there is no `mix` to get the dry back: at `depth` 0 both
channels sit at `pos` 0, which is −3.0 dB under Equal Power and −6.0 dB under Linear. Switching
between them is therefore a 3 dB change in a PER-SAMPLE GAIN, so `law` crossfades over 10 ms
exactly as `shape` does — and, like `shape`, flipping back inside that window swaps the two
ends and keeps the blend rather than restarting it.

Usage: 180° + Equal Power for a true pan; 90° for rotation; Linear when the material is going
to be summed to mono downstream.

---

## `ensemble` — String Ensemble

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `rateHz` | 0.1 – 2 | 0.6 | Hz | the SLOW bank |
| `fastRateHz` | 3 – 8 | 5.5 | Hz | the FAST bank |
| `depth` | 0 – 1 | 0.6 | — | excursion of both banks (3 ms slow + 0.6 ms fast at 1) |
| `voices` | 2 – 3, step 1 | 3 | — | delay taps in play |
| `tone` | 0 – 1 | 0.5 | — | BBD low-pass on the wet, 2 kHz – 12 kHz |
| `mix` | 0 – 1 | 0.6 | — | dry/wet |

Modelled on the Solina / Eminent string machine: one BBD line per channel with up to three
read taps, every tap modulated by a slow AND a fast LFO, the taps spaced 360/`voices` degrees
apart in both banks. The two rates together are what separates this from a chorus.

Each channel takes its own SUBSET of the taps (L = 0,1 and R = 1,2 at three voices; L = 0 and
R = 1 at two). Summing all three into both channels would cancel the first-order sidebands of
taps 120° apart and the ensemble would go flat. The per-tap channel weights ramp, so changing
`voices` fades a tap in rather than switching it in at full level, and it GLIDES the tap
spacing with it (50 ms): the per-tap LFO offset is `v / voices`, so 3 → 2 moves tap 1 by a
sixth of a cycle, and tap 1 runs at full weight on the odd channels in both configurations —
a ramped weight cannot hide a read tap that jumped ~170 samples in one sample. There is deliberately no DC
blocker — nothing here recirculates.

Usage: `mix` around 0.6 with `depth` 0.6 is the stock string pad; drop `tone` for the dull
BBD character; two voices is the narrower, more chorus-like setting.

---

## `freqshift` — Frequency Shifter

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `shiftHz` | −1000 – 1000 | 5 | Hz | how far every partial moves |
| `mix` | 0 – 1 | 0.5 | — | dry/wet |
| `feedback` | 0 – 0.9 | 0 | — | the shifted output back into the input (stacks shifts) |
| `direction` | Up / Down / Both | Up | — | which sideband is kept; Both = up on L, down on R |

Modelled on the Bode 735 / Moogerfooger single-sideband shifter. **This is not a pitch
shifter**: every partial moves by the same number of hertz, so 440 Hz with `shiftHz` 100 comes
out at 540 Hz and the harmonic series is destroyed — which is the effect. The topology is a
polyphase IIR Hilbert PAIR (two four-section cascades of second-order z⁻² allpasses, one fed
the input delayed by a sample) multiplied by a quadrature oscillator.

Both branches are ALLPASS, so the magnitude response is unity by construction and measuring it
proves nothing. The quality figure is the **image**: the unwanted sideband the pair's phase
error leaks through, measured at better than −40 dB in the shipping test (440 Hz shifted to
540 Hz, the 340 Hz image checked directly). At `shiftHz` 0 **and `mix` 1** the module is
transparent to within 0.1 dB and ±1 Hz — that qualifier is the whole claim: at the catalogue
default `mix` 0.5 the dry sums with an allpass-rotated wet, which is a comb, not a passthrough.
The carrier's phase is continuous, so a shift jump glides instead of stepping; Up↔Down is a
sign change on the quadrature term and crossfades over 10 ms.

Two notes on the controls. `shiftHz` is SIGNED and `direction` selects the sideband, so
`shiftHz −100` with Up and `shiftHz +100` with Down are the same sound: one degree of freedom
with two controls, and a preset can express the same state two ways. And `direction` Both keys
off the channel index, so **on a mono chain only the Up side is ever heard**.

Usage: ±3 to ±20 Hz is a slow phasing/chorus that never repeats; `direction` Both with
`shiftHz` around 5 is a wide stereo shimmer from a mono source; `feedback` above 0.5 turns a
single note into a rising or falling ladder.
