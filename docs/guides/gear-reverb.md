# Reverb gear

Modules in the `reverb` drawer added in the 2026-09 gear wave. Each section names the
module's type id, its params (id, range, default, unit) and what it is modelled on.
See [The DSP Tone Rig](tone-rig.md) for how modules are hosted.

Three of the four share one late tank — the 8-line feedback delay network in
`src/dsp/cores/dcdsp_hallreverb.h` (`dcdsp::revdetail::Fdn8`), with Hadamard
(unitary) feedback mixing, one damping one-pole and one DC blocker per line.
Unitary mixing is why the decay is predictable: the matrix preserves energy
exactly, so the loop gain is the PRODUCT of exactly three things -- the per-line
feedback gain `g_i = 10^(-3 L_i / (RT60 · fs))`, the damping one-pole, and the
in-loop DC blocker. **It is the product that is below unity, not each factor.**
The DC blocker's magnitude is `2/(1+r)` at Nyquist, about **1.001** for the 15 Hz
corner used here; the tank is still contractive at every setting because the
damping one-pole is far below unity up there for every `dampingKHz` the
catalogue offers. This page and the header both said the only loss was `g_i`, and
called the network strictly contractive for any `g_i < 1`.

Measured on this machine (2026-09-11, `GearReverbTests`): `decayS = 1` gives an
RT60 of **1.07 s**, `decayS = 4` gives **3.50 s**, and `dampingKHz` 2 versus 16
moves the late tail's 8 kHz content by **31.5 dB**.

---

## `hallreverb` — Hall / Room Reverb

A room, not a plate or a spring: a short multitap of **early reflections** (the
discrete first bounces that tell the ear how big the space is) in front of the
late FDN tank. Signal path: pre-delay → four input-diffusion allpasses →
{ early multitap, FDN } → bounded low shelf → width matrix → mix.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `space` | 0 – 3 | 2 (Hall) | named | Room / Chamber / Hall / Cathedral. Sets the whole delay-network geometry (×1.0 / ×1.7 / ×3.0 / ×5.0 on every line length and on the early-reflection pattern). |
| `decayS` | 0.2 – 20 | 2.5 | s | RT60 of the late tail. |
| `predelayMs` | 0 – 200 | 20 | ms | Gap between the dry attack and the start of the reverb. Glides; see below. |
| `dampingKHz` | 1 – 16 | 6 | kHz | In-loop one-pole corner, per line. Low values give a dark, absorbent room. |
| `lowCutHz` | 20 – 500 | 80 | Hz | A **bounded** −15 dB low shelf on the wet path, never an unbounded high-pass. |
| `erLevel` | 0 – 1 | 0.5 | — | Level of the early-reflection multitap under the tail. |
| `diffusion` | 0 – 1 | 0.7 | — | Input-allpass coefficient: how far a transient is smeared before it reaches the tank. |
| `width` | 0 – 1 | 1 | — | Stereo spread of the wet. 0 is mono, 1 is fully decorrelated L/R. |
| `mix` | 0 – 1 | 0.3 | — | Dry/wet. |

**`space` is a picker, not a knob.** It re-derives every delay-line length, so it
is a topology switch: the core keeps **two** tanks and crossfades between them.
The incoming tank is reset, then runs SILENT for 200 ms — longer than the
longest line of the largest geometry — before a 40 ms equal-gain crossfade
begins. Without that warm-up a freshly reset tank emits nothing until its
shortest line first wraps and then switches its eight lines on one after another,
straight into a rising crossfade: measured 0.089 of step against 0.0046 for the
same audio with no switch. With it, the switch is step-for-step identical to not
switching.

Usage: start at Hall, `decayS` 2.5, `mix` 0.25 for a stage sound. For a big
ambient bed take `space` to Cathedral and `decayS` past 8, then pull `dampingKHz`
down to 3 so the tail goes dark instead of hissing. `erLevel` is what makes a
short `decayS` sound like a real room rather than a short tail — turn it up
before you turn the decay down.

---

## `convreverb` — Convolution Reverb

Models nothing: it plays back a real space's impulse response, so the character
is whatever WAV is loaded. Uniform-partitioned overlap-save FFT convolution.
Partition size **P** is the next power of two at or above the block size given at
`prepare()`; the FFT is 2P; the frequency-delay line holds one input spectrum per
partition and is shared by both live IR snapshots, so a swap costs a second
accumulator and not a second history.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `irFile` | string | *(empty)* | path | The IR WAV. Empty reverts to the built-in default. |
| `predelayMs` | 0 – 200 | 0 | ms | Gap before the convolution. Ramped tap. |
| `lengthPct` | 10 – 100 | 100 | % | Truncates the IR, with a 20 ms fade at the cut, so a shortened IR ends rather than stops. |
| `lowCutHz` | 20 – 500 | 40 | Hz | Bounded −15 dB low shelf on the wet. **20 = off**, and off means out of the path. |
| `highCutKHz` | 2 – 20 | 16 | kHz | Low-pass on the wet. **20 = off**, likewise. |
| `mix` | 0 – 1 | 0.3 | — | Dry/wet. |

### IR requirements

- **Format:** RIFF/WAVE, PCM16 / PCM24 / IEEE float32, plain or
  `WAVE_FORMAT_EXTENSIBLE`, 8 – 384 kHz. **Mono** — the first channel of a
  multichannel file is used, and it is convolved into left and right
  independently, which preserves the player's own stereo image in the wet.
- **Maximum length: 10 seconds, and at most 480,000 samples** — ten seconds at
  48 kHz, five at 96 kHz, two and a half at 192 kHz. The tap cap is not a second
  opinion about the duration, it is the memory bound: every tap costs two floats
  in each live snapshot and two more per channel in the frequency-delay line, so
  480,000 taps is already ~16 MB for one instance on one seat. Anything longer is
  truncated, not refused.
- **Rate:** converted to the device rate once, on the owner thread, from the
  ORIGINAL decoded samples — so a device change re-converts rather than
  converting an already-converted copy. The converted taps are CACHED, so a
  `lengthPct` drag pays for the partition and transform only and never for the
  resample (`RtReverbGearIsAllocationFree…`: 19 `lengthPct` writes, 0 source
  conversions).
- **Threads:** the rebuild is owner-thread work end to end and shares NOTHING
  with the callback — its own FFT instance, its own pad and work buffers — and
  hands the result over as one atomically published snapshot whose retired
  predecessor is parked in a `RetirementQueue`. A single shared FFT is why this
  is spelled out: that class stages into a member scratch, so one shared
  instance let a `lengthPct` write or an `irFile` load corrupt the transform the
  callback was midway through (the RT gate's threaded swap probe measured an
  `inf` peak against a 0.23 reference).
- **A device change re-partitions.** `P` follows the block size, so a snapshot
  carries the geometry it was built for and one built at the old `P` is dropped
  rather than crossfaded — a 20 ms blend of a snapshot walked with the wrong
  stride is noise, not a transition.
- **Level:** the IR is energy-normalised. That is deliberate and not cosmetic:
  the shared WAV reader bounds an IR by its L1 norm, which is the right guard for
  a 2,048-tap cabinet and a ~40 dB attenuation for a two-second room. It also
  gives the more useful property that swapping IRs does not move the wet level.
- **A bad file changes nothing.** An unreadable, malformed, silent or
  non-existent path is REFUSED (`DCA_ERR_INVALID_ARG`) and the previous IR keeps
  playing. A reverb that goes silent because a path moved is worse than one that
  ignores the request.
- **With no file loaded** the module still makes a sound: `prepare()` synthesises
  a deterministic 0.5 s decaying noise burst with a direct arrival.

**Latency.** The partition FIFO costs exactly one partition, and the module
reports it (`dca_engine_dsp_get_node_latency`). The dry path is delayed by the
same amount inside the module, so dry and wet stay aligned; the chain's
branch-PDC compensates everything else. A convolution that reported zero would
put every parallel branch one partition out.

**Exactness.** `ConvReverb_ConvolvingADirac_ReproducesTheLoadedIr` drives a Dirac
through a 2 s (96,000-tap) IR loaded from a WAV and compares the output with the
file, peak-normalised, sample for sample: worst error **4.2e-7** at tap 2,108,
with a reported latency of 2,048 samples (measured 2026-09-11). The one degree of
freedom given away is the single scalar, because the core energy-normalises what
it loads; every sample RATIO is pinned. A mis-partitioned overlap-save, an
off-by-one in the frequency-delay line or a wrong-sign twiddle all fail it — the
last of those was tried on purpose and reported 1.95.

**Cost.** Measured by the RT gate
(`RtReverbGearIsAllocationFreeUnderAnIrSwapAndASpaceSwitch`, 2026-09-11): a 2 s
IR (96,000 taps) at a 480-frame block, stereo, is **385 ns/sample — 1.85 % of one
core at 48 kHz**. An IR of `kDirectMax` (256) taps or fewer skips the FFT and
runs as a plain dot product over the same input history.

Usage: load a real room and leave `mix` low — 0.15 – 0.3 is a space, 0.5 is a
special effect. `lengthPct` is the fastest way to fit a cathedral IR into a fast
song; it rebuilds the IR on the owner thread, so sweep it, do not automate it per
block. `predelayMs` up to ~40 ms keeps the attack in front of the room.

---

## `gatedreverb` — Gated Reverb

The 80s drum sound: a dense hall recorded through a noise gate keyed off the
**source**. The tail is enormous while the drum is sounding and is cut dead a
fixed time after it stops, so the reverb reads as part of the hit rather than as
a space. The VCA's control signal comes from the DRY input's envelope — that is
what makes it a gate and not a ducker.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `decayS` | 0.5 – 10 | 3 | s | RT60 of the tank behind the gate. Set it long; the gate, not the decay, ends the tail. |
| `gateMs` | 20 – 1000 | 250 | ms | Hold, measured from when the input falls BELOW the threshold. |
| `thresholdDb` | −60 – 0 | −30 | dBFS | Where the input counts as sounding. −60 is always open, 0 is never open. |
| `releaseMs` | 5 – 200 | 20 | ms | How long the VCA takes to reach EXACTLY zero after the hold expires. |
| `predelayMs` | 0 – 100 | 0 | ms | Gap before the tank. |
| `dampingKHz` | 1 – 16 | 8 | kHz | In-loop damping corner. |
| `mix` | 0 – 1 | 0.5 | — | Dry/wet. |

The release runs to a true zero, not to a small number: a gate that closes to
0.001 leaves the tail audible at −60 dB, which is exactly the artefact the effect
exists to remove and which no level-based test notices. Measured: with the
defaults, `gateMs + releaseMs + 20 ms` after a burst stops the wet is at
digital silence, while 20 ms after the burst — inside the hold — it is still within
20 dB of its level during the burst, which is what says the hold is real and the
cut is not simply a reverb that had no tail.

Usage: long `decayS`, short `gateMs`, and set `thresholdDb` just under the
quietest note you want to trigger it. Shorten `gateMs` until the tail stops
sounding like a room and starts sounding like part of the snare.

---

## `reversereverb` — Reverse Reverb

The tape trick: reverse the tape, print a reverb, reverse it back, and the tail
SWELLS into the note instead of decaying out of it. Path: pre-delay → diffusers →
FDN tank → capture ring → two backwards readers → mix.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `decayS` | 0.5 – 5 | 2 | s | RT60 of the tank whose output is captured. |
| `windowMs` | 100 – 2000 | 800 | ms | The capture window that is played backwards. |
| `predelayMs` | 0 – 200 | 0 | ms | Gap before the tank. |
| `dampingKHz` | 1 – 16 | 8 | kHz | In-loop damping corner. |
| `mix` | 0 – 1 | 0.5 | — | Dry/wet. |
| `swellShape` | 0 – 1 | 0.5 | — | 0 arrives early and sits there (√x), 1 is a long slow swell (x³·⁵). |

The reader is one line: a head walking BACKWARDS through the ring moves away from
the write head at 2×, so the sample to read at window phase `p` is simply
`lookback = 2p + 1`. A decaying tail read that way rises — measured 15.3 dB from
the first quarter of a window to the last at `swellShape` 1.

One reader cannot be continuous: it has to jump from the oldest sample of its
window back to the newest, which is a full-scale step every `windowMs`. So there
are **two** readers, half a window apart, each under a periodic envelope, and
**a `windowMs` change is latched only where a reader's envelope is exactly
zero** — at its own wrap. Latching it anywhere else is the same click by another
name: measured 0.055 of step against 0.0010 for the same audio with no change.

**The envelope is NORMALISED so the pair sums to a constant, and it is the same
constant at every `swellShape`.** The Hann factor alone is COLA at 50 % overlap;
the envelope actually applied is `hann(x) · x^γ`, and that product is not.
Measured with the normalisation removed
(`ReverseReverb_TheTwoReadersSumToAConstant`): the wet envelope dipped **4.5 dB**
below its mean at `swellShape` 0.5 and **7.4 dB** at 1, once per half window, and
the mean level fell about 9 dB from 0.5 to 1 — so the shape knob was also a large
volume control. It is a shape control now.

**And the two readers are kept half a window apart across a `windowMs` change.**
They latch at their OWN wraps, which are half a window apart, so the offset used
to settle wherever it landed, with nothing but a `reset()` to restore it; near
zero both envelopes reach their zero together and the wet path NULLS once per
window. Reader 1 runs ONE transitional window after a change — both of its ends
are at envelope zero, so it costs nothing audible — whose length is chosen to put
its next wrap back on the half. Without it the same probe measured dips of
**27.3 dB** (800 → 2000 ms) and **37.3 dB** (2000 → 500 ms).

The wet is delayed by up to one window, which is the effect, not a fault: a
window can only be played backwards once it has been captured. The DRY path is
untouched, so `latencySamples()` is 0.

Usage: `windowMs` sets the length of the swell — 200–400 ms for a lead-in on
single notes, 1–2 s for a pad. Push `swellShape` up and `mix` down for something
that sits under a part rather than swallowing it; at `mix` 1 it is an effect in
its own right.
