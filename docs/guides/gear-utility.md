# Utility & stereo gear

Modules in the `utility` drawer added in the 2026-09 gear wave. Each section names the
module's type id, its params (id, range, default, unit) and what it is modelled on.
See [The DSP Tone Rig](tone-rig.md) for how modules are hosted.

---

## `volume` — Volume / Expression Pedal

A volume pedal with a selectable pot taper. `position` is the treadle, `curve` is
the pot under it, and `minDb`/`maxDb` are what the two ends of the sweep mean.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `position` | 0 – 1 | 1 | — | the treadle. 0 = heel down (`minDb`), 1 = toe down (`maxDb`) |
| `curve` | Linear / Log / Audio / S-Curve | Linear | — | the taper applied to `position` before the dB interpolation |
| `minDb` | -90 – 0 | -90 | dB | what heel-down means. -90 is a mute; -12 makes it a "gain pedal" that never goes silent |
| `maxDb` | -12 – 12 | 0 | dB | what toe-down means. Above 0 the pedal is also a boost |
| `smoothMs` | 1 – 200 | 20 | ms | how long a position JUMP takes to arrive |

**`t` is a position on the dB SWEEP, not a fraction of the output voltage**, and
that distinction is the whole reading of this table: the pedal interpolates
`minDb .. maxDb` at `t`, so at the default `minDb = -90` the Audio curve at half
travel gives `t = 0.0995`, `-81.0 dB`, i.e. 0.009 % of full scale — not the 10 %
of OUTPUT LEVEL that the classic audio-pot definition means. By the same token
`Linear` here is a constant-dB-per-travel law, which is what a player would
recognise as an audio taper. The four names describe the SHAPE of `t(p)`; they do
not promise the pot convention's numbers.

The tapers are exactly these, and the managed tests assert the formula rather
than a golden number:

| curve | t(p) | at half travel, as a position on the dB sweep |
|---|---|---|
| Linear | `p` | the midpoint of the dB range |
| Log | `p²` | 25 % of the way up the dB range |
| Audio | `p^3.3219` | 9.95 % of the way up the dB range (−81.0 dB at the default heel) |
| S-Curve | `p²(3 - 2p)` | smoothstep: slow at both ends, the swell-pedal feel |

**Modelled on** a passive volume pedal (Ernie Ball VP Jr. and friends), where the
*only* thing that differs between models a player can hear is the taper of the pot
and how far down the heel goes.

**Using it.** Map `volume.position` to your expression pedal's MIDI CC and leave
everything else alone. `smoothMs` exists because a 7-bit CC is 128 steps across the
whole sweep — the smoother is what stops that being 128 zipper edges; 20 ms is
inaudible on a pedal sweep and short enough that a stomp still reads as a stomp.
Put it *after* your drive for a clean fade-out, *before* it to use the pedal as a
gain control.

---

## `midside` — Mid/Side Processor

Encode L/R to mid and side, treat the two independently, decode back:

```
M = 0.5(L + R)                S = 0.5(L - R)
S' = width * S, then the mono-below shelf
M' = midGain  * midPolarity  * M
S' = sideGain * sidePolarity * S'
L' = M' + S'   R' = M' - S'   (then the swap crossfade)
```

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `midDb` | -24 – 12 | 0 | dB | level of the mid (centre) |
| `sideDb` | -24 – 12 | 0 | dB | level of the side (the difference between the channels) |
| `width` | 0 – 2 | 1 | — | 0 = mono fold, 1 = unchanged, 2 = double the existing side |
| `midPhase` | Normal / Inverted | Normal | — | polarity of the mid |
| `sidePhase` | Normal / Inverted | Normal | — | polarity of the side |
| `monoBelowHz` | 0 – 500 | 0 | Hz | 0 = off. Otherwise the side is shelved **-26 dB** below this corner, so the low end collapses to the centre |
| `swap` | Off / On | Off | — | exchange L and R after the decode |

**Modelled on** the mastering desk's M/S matrix (Brainworx bx_control, the
Neutrik/Elektron-era hardware M/S boxes), not on a stereo widener — there is no
Haas delay here and nothing is invented. `dcdsp_widener.h` is the module that
synthesises width from a mono source; this one only redistributes what arrives.

`monoBelowHz` is a **bounded shelf**, deliberately, not a high-pass: the rig's
rule is that in-path bass filters SUM, and three modules each doing "just a gentle
high-pass on the sides" is how a mix loses its low end entirely. At the floor the
side is 26 dB down — inaudible as width, and still not a null.

**Using it.** Put `midside` LAST, after the cab and any reverb, so it sees the
finished stereo image. `width 0` is a hard, bit-exact mono fold — the fastest way
to check a wide patch survives a phone speaker. `monoBelowHz 120` with `width 1.4`
is the standard "solid low end, wide top" move.

---

## `looper` — Looper

A footswitch phrase looper. The live input is **always a wire at unity**; the loop
is added to it.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `state` | Stop / Record / Play / Overdub | Stop | — | the footswitch. Every transition crossfades the loop's own output over 10 ms |
| `maxLoopS` | 1 – 60, step 1 | 30 | s | how long a record pass may run before it closes itself into Play |
| `loopLevel` | 0 – 1 | 1 | — | the loop's own level |
| `overdubDecay` | 0 – 1 | 1 | — | how much of the existing loop survives each overdub lap. 1 keeps everything; below 1 the old layers fade as you build |
| `reverse` | Off / On | Off | — | read the loop backwards |
| `halfSpeed` | Off / On | Off | — | read at half rate with linear interpolation — an octave down, twice as long |
| `undo` | TRIGGER (`-` / `Undo`) | — | — | undo the last overdub. **Writing `Undo` performs it and the param reads back 0** |
| *(read-back)* | `state` | — | — | **reports the transport the ENGINE is running, not the last value written** — a record pass that fills `maxLoopS` closes itself |
| `mix` | 0 – 1 | 1 | — | the loop's blend into the output |

`loopLevel` and `mix` multiply. `loopLevel` is the level you set once for the song;
`mix` is the blend a preset or a second pedal can ride. Both ramp.

**Modelled on** a single-switch stompbox looper (Ditto / RC-1 class): one loop, one
undo, no sync, no tempo, no multi-track.

### Honest limits

- **The transport is the one control that matters, and Record always starts a NEW
  loop.** Recording over an existing one is what Overdub is for.
- **The transport CHANGES ITSELF, and the getter says so.** A record pass that
  fills `maxLoopS` closes into Play on the audio thread with no param write
  behind it, so `dca_engine_dsp_get_param(node, "state")` reports the LIVE
  transport once the core has seen your write, and your own pending value until
  then, so a footswitch UI never shows "Recording" for a loop that is already
  playing.
- **`maxLoopS` is live, and lowering it below what is already recorded closes the
  pass immediately**, at the new cap. It used to WEDGE: the record branch was
  never entered again, the auto-close lived inside it, and the module neither
  recorded nor played until you changed `state` by hand.
- **Overdub is REFUSED while `reverse` or `halfSpeed` is engaged.** The write head
  would have to follow a fractional, backwards read position. The loop still plays;
  it just takes no new material.
- **`undo` is a one-level swap, and it becomes available half a loop after the
  overdub begins** — that is how long the incremental pre-overdub snapshot takes to
  drain (2 frames per output sample, so it can never be overtaken by the overdub's
  own 1-frame-per-sample write head; copying it in one go would be up to 92 MB
  inside the callback that saw the footswitch). Pressing Undo again swaps back
  (redo); if you kept overdubbing in between, that redo returns the pre-undo take,
  not what you have played since.
- **60 s cap, and the memory is paid up front.** Both rings are allocated in
  `prepare()` for 60 s at the prepared rate, because a runtime parameter that
  resized a buffer `process()` indexes is a class of bug this rig has shipped
  before. Measured by the RT gate, stereo: **43 MiB at 48 kHz, 175 MiB at
  192 kHz** (loop + undo). `maxLoopS` caps the record LENGTH, never the geometry.
  Do not put a looper on every seat casually.
- **A device-rate change keeps the take and shifts its pitch.** `prepare()` must not
  touch user content, so the samples survive — but they are then read at the new
  rate, so a loop recorded at 48 kHz plays four times fast and two octaves up on a
  192 kHz device. Resampling tens of megabytes on the device-change thread was the
  alternative; keeping the audio is the lesser surprise, and it is asserted in
  `GearUtilityTests` so nobody "fixes" it silently. A rate DROP truncates a take
  longer than the new capacity, and a CHANNEL-COUNT change discards it (the buffer
  is frame-major).
- **No wrap crossfade.** The lap boundary is exactly where you closed the take. A
  loop closed mid-note clicks once per lap — that is the instrument.
- **`reset()` does not erase the loop.** Bypassing the node and un-bypassing it
  keeps the take; only a new Record replaces it.

**Using it.** Drive `looper.state` from a four-way footswitch (or three buttons:
Rec/Play/Stop with Overdub as a hold) and `looper.undo` from a MOMENTARY control —
it is a trigger, not a latch, and a UI that renders it as a toggle is wrong about
what it is. **The catalogue cannot express that today**, and a generic pedalboard
UI has to be told: `undo` is declared with two value labels, so
`JuceDspModuleCatalog`'s `IsNamed` is true and the documented rendering is a
PICKER — a latch whose second state the engine immediately reports as the first.
Expressing it properly needs a capability bit beside the existing step/label pair
(a `kParamFlagMomentary`), which is an ABI change and is NOT in this wave; until
then this is the one place in the palette where a UI has to special-case a param
by name. Put the looper LAST in the chain so it records your finished tone;
put it before the amp only if you want to re-amp the loop live.
