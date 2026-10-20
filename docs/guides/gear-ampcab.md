# Amp & cab gear

Modules in the `amp / cab` drawer added in the 2026-09 gear wave. Each section names the
module's type id, its params (id, range, default, unit) and what it is modelled on.
See [The DSP Tone Rig](tone-rig.md) for how modules are hosted.

Every number on this page is measured by
`tests/JuceSharp.Tests/Dsp/GearAmpCabTests.cs` — if one of them moves, that
suite goes red and this page is what is wrong. Each of those assertions is a
**two-sided** `Assert.InRange` centred on the figure printed here (±1.5 dB);
they were one-sided floors until 2026-09-12, between 1.2 and 6.5 dB looser than
the numbers they were supposed to pin. The allocation gate and the
oversized-block guard for all four live in the RT scenario
`RtAmpCabGearChainAllocatesNothingAcrossAModelSwitch`.

All four are also **silent from digital silence**
(`SilenceIn_StaysBelowTheFloor_FromTheVeryFirstBlock`, floor −60 dBFS RMS).
`bassamp` and `poweramp` each run a grid-biased valve stage, which emits DC into
the blocker behind it, and a blocker starting from zero state answers a DC step
with a full-amplitude decaying pulse — measured at −49.0 and −50.0 dBFS RMS over
the first 500 ms before the fix. Both cores now prime themselves on a quarter
second of silence in `prepare()` and `reset()`, so adding the amp, changing the
device or un-bypassing it does not thump.

---

## `bassamp` — Bass Amp (category `amp`)

The app charts bass and had no bass head. Four voicings over one signal path:
preamp gain → optical compressor → four-band tone stack → oversampled valve
stage → master.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `model` | 0–3 | 0 | named | `Flip-Top` / `Tube 300` / `Solid 800` / `Modern`. Moves the preamp gain curve, the four tone-stack CENTRES, a brightness shelf and the clip character. |
| `gain` | 0–1 | 0.4 | — | Preamp gain, 0 dB to +30…+36 dB depending on the model. |
| `bass` | −20–20 | 0 | dB | Bounded low shelf at the model's bass centre (80 Hz on `Flip-Top`). |
| `lowMid` | −20–20 | 0 | dB | Bell at the model's low-mid centre (250 Hz on `Flip-Top`), Q 2.0. |
| `hiMid` | −20–20 | 0 | dB | Bell at the model's high-mid centre (800 Hz on `Flip-Top`), Q 2.0. |
| `treble` | −20–20 | 0 | dB | Bounded high shelf at 3 kHz. |
| `drive` | 0–1 | 0.2 | — | Post-EQ valve stage, 4× oversampled ADAA tanh with the model's grid bias. Gain-compensated, so it is a drive control and not a second volume. |
| `compress` | 0–1 | 0.3 | — | Optical-style compressor in the preamp (30 ms attack / 300 ms release). |
| `master` | 0–1 | 0.6 | — | Output level. |
| `mid` | −20–20 | 0 | dB | A fifth, SWEEPABLE bell, Q 1 (2026-10). Out of the path at 0 dB. |
| `midHz` | 100–5000 | 800 | Hz | Its centre. |
| `midQ` | 0.3–8 | 1 | — | Its Q. |
| `lowMidOct` | −4–4 | 0 | oct | Moves the `lowMid` bell off the model's centre (Hz would have needed a default per model). |
| `hiMidOct` | −4–4 | 0 | oct | The same for `hiMid`. |
| `lowShift` | 0–1 | 0 | named | `Off` / `On`: Ampeg's Ultra Lo. |
| `highShift` | 0–1 | 0 | named | `Off` / `On`: Ampeg's Ultra Hi. |

**The 2026-10 controls** (DotCharter #72) are the ones Rocksmith's bass heads
carry. Each defaults to the pre-existing sound, and at its default its filter
is SKIPPED, so a saved tone renders sample-identically. The sweepable bell's
default Q 1 is Eden's figure for its World Tour semi-parametric bands ("one
octave fixed bandwidth (Q=1)", WT-500/600/800 operation manual). Ultra Lo /
Ultra Hi are the figures Ampeg PUBLISHES for its SVT-CL (owner's manual: Ultra
Lo "+2 dB at 40 Hz and −10 dB cut at 500 Hz", Ultra Hi "+9 dB at 8 kHz"), each
an RBJ peaking bell at Q 0.7 — Ampeg gives no bandwidth, so the Q is ours. No
value here comes from Rocksmith's files (JuceSharp #25; until then these were
the HW-300B soundbank's bands): a converted Rocksmith head runs the game's own
chain only through `rsamp`, whose table the player's install supplies. A centre
is capped at a quarter of the rate (`clampCutoff`). The band ranges widened to
±20 dB so a converted bass tone's deep cuts and boosts fit (Eden's bands are
±15 dB). `amp_circuits_test` (native) and `GearAmpCabTests` pin every one of
those numbers.

**Modelled on** a small flip-top valve combo, a 300 W all-valve fridge, an
800 W solid-state head and a modern scooped hi-fi voicing. The shelves are RBJ
sections: a shelf is HALF its labelled gain at its corner and reaches the full
amount in its plateau, so `bass` +12 measures +11.8 dB at 20 Hz and the two
bells measure +11.6 dB at their centres, each moving its neighbour by under
0.5 dB.

**Nothing in this module high-passes.** A low B is 31 Hz and a low E 41 Hz; one
"reasonable" 80 Hz high-pass would remove the instrument's fundamentals, so
every voicing filter is a bounded shelf and the only high-pass in the core is a
10 Hz DC blocker. 41 Hz comes out within 2 dB of 100 Hz at every gain setting.

```
bassamp -> cab      # the whole head; put nothing amp-like in front of it
compressor -> bassamp   # a pedal compressor still works; `compress` is the amp's own
bassamp with drive 0    # a clean DI-ish bass channel, tone stack only
```

---

## `acpre` — Acoustic Preamp (category `amp`)

The stage DI that goes IN FRONT of `acousticsim` — that module is a BODY
simulator, this is the preamp strip: anti-feedback notch, polarity, three-band
EQ, air, and a corrector for under-saddle piezo "quack".

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `notchHz` | 60–500 | 120 | Hz | Centre of the anti-feedback notch. |
| `notchDepthDb` | 0–30 | 0 | dB | How much it cuts. 0 is off and is bit transparent. |
| `notchQ` | 2–30 | 10 | — | Width. At Q 10 a 30 dB cut at 120 Hz takes 1.1 dB out of 200 Hz. |
| `phase` | 0–1 | 0 | named | `Normal` / `Inverted`. Crossfades through zero over 10 ms. |
| `bass` | −12–12 | 0 | dB | Bounded low shelf, 100 Hz. |
| `mid` | −12–12 | 0 | dB | Bell, 800 Hz, Q 1.1. |
| `treble` | −12–12 | 0 | dB | Bounded high shelf, 5 kHz. |
| `presence` | 0–1 | 0.3 | — | Blends in a 10 kHz air shelf (+8 dB) with a −3 dB tilt at 350 Hz. |
| `piezoFix` | 0–1 | 0 | — | The quack corrector: a −6 dB bell at 1.8 kHz plus a +2.5 dB shelf at 160 Hz, tapered by the knob. At 1 it measures 6.0 dB of cut at 1.8 kHz. |
| `level` | 0–1 | 0.7 | — | Output level. |

**Modelled on** the preamp section of a stage acoustic DI. The module is
entirely linear — no waveshaper, no DC blocker, no oversampling — and at flat
settings it is BIT transparent, which is why `phase = Inverted` is an exact
negation you can null against the dry signal. Every knob whose neutral is "off"
is a blend of weight zero over a fixed filter, so it ramps AND it is exactly
the identity at zero.

```
acpre -> acousticsim     # preamp first, body sim second
acpre alone              # a piezo DI: piezoFix 1, notch on whatever the room rings at
notchDepthDb 0           # the notch is out of the path entirely
```

---

## `poweramp` — Power Amp & Sag (category `amp`)

The half of an amplifier a drive pedal does not give you. Put it after any
preamp or dirt box and the rig behaves like a real head's output section.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `tubes` | 0–4 | 2 | named | `EL84` / `6V6` / `EL34` / `6L6` / `KT88`. Headroom, knee softness and bias asymmetry; all scalars, so a change is four smoothers walking over 10 ms. |
| `drive` | 0–1 | 0.3 | — | How hard the stage is pushed (0…+20 dB), gain-compensated. |
| `sag` | 0–1 | 0.3 | — | Supply sag: a 200 ms follower droops the clip CEILING under load and recovers over 200 ms. At 1, a sustained loud note is 5.9 dB down at 500 ms; at 0 it is 0.0 dB down. |
| `presence` | 0–1 | 0.5 | — | Negative-feedback high shelf at 3.5 kHz. 0 → 1 moves 5 kHz by 7.2 dB. |
| `resonance` | 0–1 | 0.5 | — | Output-transformer / speaker low shelf at 110 Hz. 0 → 1 moves 80 Hz by 7.1 dB and 4 kHz by 0.00 dB. |
| `damping` | 0–1 | 0.5 | — | The speaker impedance peak: a bell at 95 Hz (Q 2.2) plus a small 2.5 kHz bell. |
| `level` | 0–1 | 0.6 | — | Output level. |

**Modelled on** a push-pull valve output stage with its rail sag, its negative
feedback loop and the speaker's impedance curve. The stage is 4× oversampled
with an ADAA tanh: worst alias product **−126.5 dB** relative to the
fundamental at the catalogue defaults, and **−121.4 dB with `drive` at 1 into
an EL84's headroom**, from a two-tone stimulus (1 kHz + 1.1 kHz) measured by
comparing a 48 kHz render against a 192 kHz one bin for bin. (bassamp's own
driven case measures −65.4 dB on the same method; the defaults case is
−123.2 dB.) The driven cases are the ones that PROVE the oversampler: forced
to 1×, the stage still measured −126 dB at the defaults, because the tanh is
barely bending the signal there.

Sag scales the CEILING, not the input: `y = ceiling · tanh(x / ceiling)` leaves
quiet playing untouched and squashes loud playing, where scaling the input
would make the knob a slow volume control that ducks clean passages too. The
core re-implements sag rather than sharing `dcdsp_amp.h`'s — that core's time
constants belong to its own voicing table.

```
tubescreamer -> poweramp -> cab    # any pedal, now with an output section
poweramp with drive 1, sag 1       # the sagging, blooming, compressed end of the range
poweramp -> micpos                 # power amp, speaker colour, then the mic
```

---

## `rsamp` — Rocksmith Amp (category `amp`)

Rocksmith 2014's OWN amps (DotCharter #88): each of the game's 63 amps (42 guitar
amps, 18 bass heads, 3 DI preamps) is a chain of up to four Wwise effect plug-ins
in its soundbank, and every knob is a set of RTPC curves onto their parameters.
`rsamp` runs that chain from a table the HOST hands in at run time (ABI v45,
`dca_rsamp_set_table`, JuceSharp #25): the numbers are the game's, so this
library ships none of them, and DotCharter builds the table from the player's
own install (`JuceSharp.Dsp.RsAmpChainTable` writes the blob documented in
`cores/dcdsp_rsamp_table.h`). **Without a table, or for a row the loaded table
has no chain for, `rsamp` passes audio through unchanged.** A table loaded or
unloaded while a node plays is picked up by the core at its next block, through
the same 5 ms dip a model change takes. The 63 model rows (key and name) are
public: `cores/dcdsp_rsamp_models.h`. Signal path: the chain's input 18 dB
hotter → each plug-in in order → 18 dB back down.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `model` | 0–62 | 0 | named | The amp, by its in-game name (`AT-120` … `DI. Pre Tube`). A change dips out over 5 ms, swaps the chain at silence (every filter, oversampler and envelope reset) and comes back over 5 ms. |
| `k1`–`k10` | 0–1 | 0.5 | position | The amp's knobs in its manifest's order, each as its POSITION between the game's own minimum and maximum (a switch or selector rounds). What a knob does is the model's: Gain on one amp, a 7.5 kHz graphic band on another. Knobs ramp over 30 ms and re-solve the whole chain every 8 samples while they move. |
| `level` | −24–24 | 0 | dB | Output trim, ramped over 30 ms. 0 dB is the chain at the game's own levels; DotCharter's loudness levelling puts here what its trailing gain's ±24 dB cannot give. |

The plug-in clones, and what in them is inferred (the header of
`cores/dcdsp_rsamp.h` has the detail):

- **Wwise Guitar Distortion** (three pre-EQ bands, a waveshaper, three post-EQ
  bands, output, wet). The band types and the five transfer functions are
  Audiokinetic's documentation; the transfer functions were fitted to its
  published figures (Clip exact, Overdrive a C1 quadratic knee, Heavy
  `1 - e^(-9.98|x|)`, Fuzz asymmetric). The drive law (0–100 % → 0–+25 dB) is
  inferred.
- **iZotope Trash Multiband Distortion**, the Windows build's drive stage, is
  unpublished; `rsamp` runs the Guitar Distortion block the game's own Mac
  build swaps in for it.
- **McDSP FutzBox** (resonant high/low-pass, distortion, one EQ band, levels):
  its block layout is decoded against the Wwise help page; its distortion modes
  are unpublished (amount → 0–+50 dB into a tanh/hard-clip blend set by
  Intensity).
- **Wwise Compressor**: threshold, ratio, attack, release,
  output; peak detector, hard knee, per channel.
- The input level (+18 dB) puts a full strum at the rig's −18 dBFS reference
  near 0 dBFS into the chain — a calibrated, near-full-scale instrument input —
  and the chain's output comes back 18 dB lower, so the module is unity overall.

Measured by `tests/JuceSharp.Tests/Dsp/GearRsAmpTests.cs` on SYNTHETIC tables
(`Dsp/SyntheticRsAmpTables.cs`; no game number is in this repository), so each
expected response follows from the row's own definition: a dB curve moves its
band by the curve's ends (also at 44.1 and 96 kHz), an output curve is a
broadband gain, a log-Hz curve moves the FutzBox corner, a drive curve drives
the waveshaper, `level` is an exact output gain; no knob, level, model change or
table load steps; with no table, and for a row the table lacks, audio passes
through. The parser's refusals and the core's pick-up of a new table are the
native `rsamp_table` test. The allocation gate and the oversized-block guard
are the RT scenarios `RtRsAmpGearAllocatesNothingAcrossEveryModel` (all 63
rows of a synthetic table, armed) and
`RtRsAmpTableLoadedWhilePlayingSwapsWithoutAllocating`. The real install's
table is compared with the header compiled in until #25, by hash, in
DotCharter's owner-only `LocalRsGameTablesTest`.

## `micpos` — Cab Mic Position (category `cab`)

The MIC layer that goes AFTER `cab` or `dualcab`. It loads no impulse
response; it models what MOVING the mic does to one.

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `mic` | 0–4 | 0 | named | `57` / `121` / `421` / `Condenser` / `Tube`, five fixed EQ curves. The 57 and the 121 differ by 10.5 dB at 5 kHz. `Tube` (2026-10) is a large-diaphragm tube condenser fitted to the Neumann U 67's published cardioid response (within 0.55 dB, 50 Hz-18 kHz): -2.2 dB at 100 Hz, +1.5 dB at 9-10 kHz, -4.7 dB at 16 kHz. |
| `position` | 0–1 | 0.3 | — | Dust cap → cone edge: a bounded −12 dB shelf at 3.5 kHz plus a small 220 Hz bump. 0 → 1 drops 6 kHz by 10.6 dB. |
| `distanceCm` | 0–30 | 5 | cm | Proximity effect (a bounded +8 dB shelf at 150 Hz that fades out by 30 cm) and, past 15 cm, a gently blended floor-bounce tap. 0 vs 30 cm is 5.7 dB at 100 Hz. |
| `room` | 0–1 | 0 | — | Five early reflections at 7/13/19/29/41 ms. At 0 it adds exactly nothing. |
| `blend2` | 0–1 | 0 | — | A second mic — the ribbon at 10 cm — summed in on a tap placed RELATIVE to the first capsule, so the closer of the two is the earlier one (56 samples behind the first mic's at 0 cm, 14 ahead of it at 30 cm). |
| `level` | 0–1 | 0.7 | — | Output level. |
| `angleDeg` | 0–60 | 0 | deg | The capsule turned off the speaker axis (2026-10): the MIC'S own published polar response at that angle — a broadband loss plus a bounded low and high shelf, read off the Shure SM57 user guide (57), the Neumann U 67 manual (Tube) and the figure-8 law (121; the 421 and Condenser take the ideal cardioid) — and a proximity effect that thins with the gradient share of the capsule. At 45 degrees the 57 loses 1.2 dB at 1 kHz and 2.4 dB at 8 kHz, the ribbon 3.0 dB everywhere. 0 runs no part of the stage. |

**Modelled on** the microphones every guitar session reaches for and on what a
mic stand actually does: toward the edge loses top, close gains bass, far picks
up the floor — and turning the mic off axis does what the mic's polar chart
says, which is NOT the same as moving it to the edge (`position` moves the
capsule across the cone; `angleDeg` turns it).

**Latency is a constant 42 samples at 48 kHz, it does not follow the distance
knob, and it is the delay the dry path actually has** — measured off a Dirac by
`MicPos_Latency_IsTheDelayTheDryPathActuallyHas`, not read back from the module.
The FIRST capsule owns the 30 cm reference plane and its tap never moves; the
knob moves the SECOND capsule relative to it. That is what stops a knob drag
from re-timing the whole branch under branch PDC, and it is also what keeps the
two mics in the right ORDER — a common far plane would have made the closer
capsule come out later (`MicPos_TheCloserCapsuleIsTheEarlierOne`). With
`blend2 > 0` and `distanceCm > 10` the ribbon leads the reported latency, which
is what a mic 20 cm closer does; the number PDC compensates is the first
capsule's. The line is allocated in `prepare()` for the worst case (the longest
room tap at the highest rate, plus the second capsule's furthest tap), never for
the current knob positions.

```
cab -> micpos            # always after the IR, never before it
micpos blend2 0.4        # a 57 and a ribbon, already time-aligned
micpos room 0.2          # a little air without reaching for a reverb
```
