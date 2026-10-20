# Filter gear

Modules in the `filter` drawer added in the 2026-09 gear wave. Each section names the
module's type id, its params (id, range, default, unit) and what it is modelled on.
See [The DSP Tone Rig](tone-rig.md) for how modules are hosted.

One module here is **not** in the `filter` drawer: `tilt` is a tone control and
carries the `eq` category. The drawer is a UI fact, and a Tilt EQ filed under
"filter" is a tone control nobody would find.

Everything below is pinned by `tests/JuceSharp.Tests/Dsp/GearFilterTests.cs` and,
on the callback side, by the RT gate's
`RtFilterGearIsAllocationFreeUnderAKeyedBandSwitch`.

---

## `svf` — Multimode Filter (`filter`)

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `mode` | 0..3 | 0 | LowPass / BandPass / HighPass / Notch | which tap of the filter you hear |
| `cutoffHz` | 20..20000 | 1000 | Hz | the corner. **The value IS Hz** — a UI is expected to give the knob a logarithmic taper, but nothing about that taper reaches the DSP |
| `resonance` | 0..1 | 0.3 | — | Q, 0.707 (Butterworth) at 0 to ~20 at 1 |
| `drive` | 0..1 | 0 | — | saturation *inside* the resonance path, so a high-Q setting sings rather than screams |
| `source` | 0..2 | 0 | Manual / LFO / Envelope | what sweeps the cutoff |
| `lfoRateHz` | 0.05..20 | 1 | Hz | sweep rate when `source` = LFO |
| `lfoDepth` | 0..1 | 0.5 | — | octaves of sweep; 1.0 is the full ±4 |
| `envSens` | 0..1 | 0.5 | — | up to +4 octaves at a full-scale input, when `source` = Envelope |
| `envAttackMs` | 1..200 | 10 | ms | follower attack |
| `envReleaseMs` | 10..2000 | 200 | ms | follower release |
| `mix` | 0..1 | 1 | — | dry/wet |

**Modelled on** the analogue multimode filter of the Oberheim SEM / Korg MS-20
family — one section, four simultaneous outputs — built on a Zavalishin/Cytomic
TPT state-variable topology so the cutoff can be modulated at audio rate without
the classic Chamberlin SVF's stability cliff. The `drive` nonlinearity is on the
bandpass integrator *state*, a delayed quantity, so the zero-delay-feedback solve
stays linear: no iteration on the callback, and no setting that blows up.

**Using it.** Park `source` on Envelope with `mode` = BandPass and a low
`cutoffHz` for a funk auto-wah that opens on your picking hand. Turn `resonance`
up and `drive` with it — drive is what keeps the resonant peak musical instead of
a whistle. For a synth sweep, `source` = LFO with `lfoDepth` at 1 covers ±4
octaves, which at the default cutoff is 62 Hz to 16 kHz.

---

## `comb` — Comb Filter (`filter`)

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `freqHz` | 20..5000 | 220 | Hz | the comb's fundamental; the delay is 1/`freqHz` seconds |
| `feedback` | -0.98..0.98 | 0.7 | — | loop gain. **Negative inverts** the returned signal, moving the peaks onto the odd multiples of `freqHz`/2 |
| `damping` | 0..1 | 0.3 | — | a one-pole lowpass in the loop: 0 = 20 kHz corner, 1 = 1 kHz |
| `polarity` | 0..1 | 1 | Feedforward / Feedback | a fixed ripple comb, or a resonant one |
| `mix` | 0..1 | 0.5 | — | dry/wet |

**Modelled on** the resonator at the heart of a flanger, a Karplus-Strong string
and every "metallic" tuned-delay effect. The line is fractional (4-point Lagrange),
so `freqHz` sweeps smoothly, and it is allocated for the **worst case** — 20 Hz at
192 kHz, 9600 samples — rather than for the rate it happens to be prepared at: a
line sized from the prepared rate is correct until a device change asks for a
longer delay than the buffer it got, and every read then clamps, detuning the comb
silently. Pinned at `feedback` 0.9: every peak at 220·k, k = 1..5, is at least 15 dB
above the troughs either side of it.

**Using it.** At `polarity` = Feedback and `freqHz` around a note you are playing,
it is a resonant body — set `damping` high and it is a struck string. Sweep
`freqHz` slowly for a flanger you drive by hand. Negative `feedback` is the
hollow, "phasey" comb; positive is the ringing one.

---

## `vocoder` — Vocoder (`filter`, takes a sidechain key)

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `bands` | 8..24 step 4 | 16 | count | analysis/synthesis bands, log-spaced 80 Hz..8 kHz |
| `carrier` | 0..3 | 1 | Sidechain / Saw / Pulse / Noise | what the envelopes are imposed on |
| `pitchHz` | 30..500 | 110 | Hz | the internal oscillators' pitch. **Fixed — nothing tracks your playing** |
| `attackMs` | 1..100 | 5 | ms | per-band follower attack |
| `releaseMs` | 5..500 | 40 | ms | per-band follower release |
| `formantShift` | -12..12 | 0 | semitones | offset between each band's analysis centre and its synthesis centre: the vocal-tract size control |
| `emphasis` | 0..1 | 0.5 | — | pre-emphasis on the modulator (a 2 kHz high shelf, up to +12 dB), which is what makes consonants legible |
| `mix` | 0..1 | 1 | — | dry/wet |

**Modelled on** the analogue channel vocoder (EMS / Roland VP-330 family). Your
input is the MODULATOR: it is split by the analysis bank, each band's level is
tracked, and those levels are imposed on the same bands of the carrier. Each band
divides by a slow envelope of its own *carrier* content first, so the output
follows the player rather than the oscillator's spectral tilt. The effect is mono
— one analysis, one synthesis, the same wet mixed against each channel's dry.

Set `carrier` to **Sidechain** and the key is the carrier. With no key available
the carrier falls back to the module's own input (the engine's contract — never to
silence), which makes it a near-wire rather than a mute.

Changing `bands` is one of the two structure changes a core in this drawer makes
for itself: two complete banks are held, a count change configures the idle one and
crossfades over 10 ms, and a swap asked for under an in-flight fade is deferred to
the callback that sees that fade land. The incoming bank starts cold, so it takes
its filters' and followers' own settling time (~100 ms) to reach the sound a
settled bank of that size makes — that is inherent to the swap, not a bug.

**`formantShift` is the other, and it goes through the same mechanism.** It
re-designs all of the SYNTHESIS sections — up to 24 bands, Q up to 24, by up to a
full octave — which is the same discontinuity a count change makes, on a knob a
user drags, so it never writes them in place on the LIVE bank. `carrier` is
crossfaded too: Saw and Pulse differ by 1.7 at phase 0, so switching between them
was a splice. Neither costs anything at rest — `setParams` re-designs the bank only
when `bands` or `formantShift` actually moved, where it used to run ~300
transcendentals on the audio thread for *every* param write, `mix` included.

**The bands do not sum flat, and that is deliberate.** The constant-Q is the
standard `sqrt(r)/(r-1)`, which puts ONE 2nd-order RBJ bandpass at −3 dB at the
geometric midpoint between neighbouring centres — but the bank cascades two of
them, so each band is −6 dB there. Measured at 8 bands: **4.2 dB** between a band
centre and the midpoint to its neighbour
(`Vocoder_BandCoverageRipples_ByTheAmountItsFourthOrderBankImplies`). A 4th-order
bank is what makes the bands distinct enough to read as a vocoder rather than as a
filtered mush.

Cost, measured by the RT gate: **181 ns/sample at 24 bands** (4th order on both
sides, 480-frame stereo block) = 0.87 % of one core at 48 kHz.

**Using it.** Key the vocoder from a synth stem and your guitar talks — add a
`vocoder`, set `carrier` = Sidechain and the node's sidechain source to the song
mix. Play chords, not single notes: the modulator has to have energy spread across
the bands for anything to speak. `bands` 8 is the robotic, wide-toothed sound and
24 is the intelligible one.

---

## `talkbox` — Talk Box (`filter`)

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `vowel` | 0..1 | 0.5 | — | a continuous sweep through a–e–i–o–u. **Live in every `sweep` mode** — the LFO and the envelope OFFSET it, they do not replace it |
| `sweep` | 0..2 | 0 | Manual / Envelope / LFO | what moves the vowel |
| `lfoRateHz` | 0.05..10 | 0.5 | Hz | sweep rate when `sweep` = LFO |
| `envSens` | 0..1 | 0.5 | — | how far your picking hand moves the vowel ABOVE where `vowel` sits, when `sweep` = Envelope. At 0 the pedal stays on the knob |
| `resonance` | 0..1 | 0.6 | — | formant Q, 2 to 25 |
| `drive` | 0..1 | 0.3 | — | the driver's saturation, *before* the formants |
| `mix` | 0..1 | 1 | — | dry/wet |

**Modelled on** the tube-and-mouth talk box (Heil / Framptone): a driver crams the
amp signal up a plastic tube into the player's mouth, and the mouth's own resonant
cavity shapes it before the vocal mic picks it up. Here the "mouth" is three
PARALLEL resonant bandpasses — a vocal tract is parallel-resonant, not cascaded —
whose centres slide along the vowel path. The frequency table is the same
Klatt/Csound vocal-tract data every vowel model uses, but the amplitude tilt is
**fixed per formant** (F1 > F2 > F3) rather than per vowel, so the ordering of the
peaks never depends on where the knob is. Measured: at `vowel` 0 the two strongest
peaks of a sawtooth land on 700 Hz and 1220 Hz; at `vowel` 1, on 300 Hz and 870 Hz.

The LFO swings ±½ of the a-to-u path around the knob, so at the default `vowel` 0.5
it still covers the whole of it, so the `vowel` knob stays live in both sweep
modes and `envSens` 0 does not pin the pedal on "a". The modulator is also added
RAW rather than through the vowel smoother: re-targeting a linear ramp once per
sample degenerates it into an ~8 Hz one-pole, which silently low-passed the LFO so
the top third of the rate knob did progressively less than it said.

**Using it.** `sweep` = Envelope turns your picking dynamics into the vowel, which
is the closest a pedal gets to the real thing without a tube in your teeth. Keep
`drive` up — the real driver is a small speaker being abused, and the saturation is
most of the character. High `resonance` with `sweep` = LFO is the chewing "wah-oo"
motion.

---

## `tilt` — Tilt EQ (`eq`)

| id | range | default | unit | what it does |
|---|---|---|---|---|
| `tilt` | -12..12 | 0 | dB | total tilt: **+ lifts the highs by `tilt`/2 and cuts the lows by `tilt`/2** about the pivot |
| `pivotHz` | 200..5000 | 1000 | Hz | the frequency the spectrum rocks about |
| `level` | -12..12 | 0 | dB | output trim |

**Modelled on** the one-knob tilt tone control of the Quad 34 / Baxandall lineage.
It is ONE complementary shelving pair: an RBJ low shelf at −`tilt`/2 dB and a high
shelf at +`tilt`/2 dB, both at Q 0.707 and both centred on the pivot. A shelf sits
at exactly half its dB gain at its own corner, so the two cancel at the pivot by
construction. Pinned at `tilt` +12, +6 and −12: 10 kHz sits `tilt` dB above 100 Hz to within
1.5 dB, and the pivot itself moves by under 0.5 dB.

The shelves are **shelves and not high/low passes** on purpose. An unbounded filter
looks identical over the musical range and then keeps going, and these cascade — a
rig with three of them in it is 20 dB down at the low E with nothing on screen
saying so.

**Using it.** It is the fastest way to take the ice off a bright rig or the mud out
of a dark one without choosing a band. Put it last in the chain and move `tilt` by
2–3 dB at a time; move `pivotHz` down toward 400 Hz to make it a body control and
up toward 3 kHz to make it a presence control.
