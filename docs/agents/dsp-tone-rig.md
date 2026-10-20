# The DSP tone rig and the drop tuner (`src/dsp/`)

Deep reference for `JuceSharp/AGENTS.md`. The in-house effect chain applied to
the guitar INPUT ahead of the user VST chain, the `dcdsp` cores behind it, and
the live-input pitch shifter. Managed mirror: `DotCharter.Services/Audio/Dsp/`
(`DcRigTargetCatalog`) — see `docs/agents/services/dsp-tone-rig.md` in the
DotCharter repo.

## Drop tune: never put a phase vocoder on the live input

The live-input pitch shifter is **CSVD** (`src/dsp/cores/dcdsp_droptune.h`, ABI v25).
Read its header before touching it — it encodes the reason, not just the code.

- **The phase-vocoder family CANNOT be low-latency.** Resolving a guitar's harmonics
  needs a window of `N > 4*fs/f0` — 48 ms for a low E, 65 ms for a 7-string low B.
  That is window theory. Rubber Band's own docs call LiveShifter "not a low-latency
  effect, with a delay of 50ms or more"; Signalsmith's default block is 120 ms. Both
  are excellent and both are unplayable here. **Do not "upgrade" the drop tuner to
  one of them.** (They are still the right tool for OFFLINE work.)
- **Down-only shifting has no latency floor.** The read head trails the write head and
  jumps FORWARD into already-written audio, so nothing is looked ahead for. Up-shifting
  is the direction that needs a buffered period. Keep the shifter down-only.
- **Latency is felt at the ATTACK, not during sustain.** A HF-envelope onset detector
  snaps the read head forward at every pick (~3-4 ms), which is why palm mutes and
  fast riffs are the BEST case here, not the worst.
- **Never emit dry, unshifted audio at the attack.** A previous "transient duck" did,
  so the pick arrived at the wrong pitch with the shifted sustain behind it — the
  DigiTech Drop's most-complained-about artifact, reimplemented by accident.
- **Crossing UNITY is a splice, in both directions, and is crossfaded since
  2026-09-11.** `semitones` is written per block from the callback, so engaging
  swapped the output from the dry sample to a sinc read `anchorDelay_` (1.3 ms)
  back in ONE sample, and releasing swapped it back the same way — measured at
  0.487 full-scale on a 196 Hz sine, against that signal's own 0.0115 slope.
  Every other head movement in the file was already crossfaded
  (`fadeLen_`/`fadeRho_`), and **`DspChain`'s 10 ms bypass crossfade does not
  cover this**: drop tune is not a catalogue node, the engine drives it
  directly. `unityFade_`/`unityFadeLen_` run one `snapXfade_` (1 ms) fade
  between the shifted read and the LIVE dry input — deliberately separate from
  `fade_`/`rOld_`, which the splice scheduler owns and which fades between two
  READ HEADS. The law is amplitude-complementary because the two sides are the
  same material at most one ceiling apart (rho ≈ 1), which is the case the
  file's own correlation-following law reduces to linear for.
  **This is NOT the attack rule being relaxed** — the transient anchor still
  emits the attack already shifted; the fade covers only a knob crossing unity.
  **"`semitones == 0` is a TRUE BYPASS" is therefore now bit-exact from the
  moment that 1 ms has drained, not from the sample the knob moved.** Both
  halves are pinned, because either alone rots into the other's bug:
  `RtDropTuneOfflineIsAllocationFree` requires every zero segment to carry
  exactly one fade of 1..48 samples (0 means the release is cutting again) and
  nothing but dry past it, and separately renders a tuner that was NEVER
  engaged and requires it bit-exact from sample 0;
  `DropTuneOfflineTests.CrossingUnity_CrossfadesInsteadOfSplicing` measures the
  step at both seams against the settled slope.

## The 2026-09 gear wave: per-category gear files (57 → 100 types; 101 with `rsamp`)

The nine palette drawers each gained modules on 2026-09-11/12, one Opus agent per
drawer, and the layout that made nine agents possible without a merge war is
now the rule for ANY new module:

- **A new module is one core header + one adapter + one row in ITS drawer's
  file** — `src/dsp/gear/<Category>Gear.cpp` (`DynamicsGear`, `DriveGear`,
  `AmpCabGear`, `FilterGear`, `ModulationGear`, `PitchGear`, `DelayGear`,
  `ReverbGear`, `UtilityGear`), appended inside that file's
  `append<Category>Gear(std::vector<Entry>&)`. `DspModuleFactory.cpp` keeps the
  original 57 rows and calls the nine hooks in a fixed order, so a new module is
  never inserted before an existing one. A tenth hook, `RsAmpGear.cpp`
  (`appendRsAmpGear`, DotCharter #88), runs LAST: `rsamp` runs Rocksmith 2014's
  own amp chains from a table the HOST loads at run time (ABI v45,
  `dca_rsamp_set_table`; `cores/dcdsp_rsamp_table.h` documents the blob, the
  managed writer is `JuceSharp.Dsp.RsAmpChainTable`). **No number read out of the
  game's files is compiled in** (#25): DotCharter builds the table from the
  player's own install. Without a table every `rsamp` module passes audio
  through. The model ROWS are public (`cores/dcdsp_rsamp_models.h`: key + name,
  ordinal by key) and append-only, because a saved `model` int names a row; a
  loaded table's row whose key is not that row's is refused whole. The table is
  process-wide, published with one release store and read by the core with one
  acquire load per block; published tables are never freed (at most 32 distinct
  per process), so the audio thread never sees one die. **"Stable within a build" is the exact
  claim and it is weaker than it sounds**: the last original row is `namamp`,
  immediately before the appends and behind `#if DOTCHARTER_DSP_NEURAL`, so a
  `-DDOTCHARTER_DSP_NEURAL=OFF` build shifts EVERY gear index down by one and the
  catalogue is 100 rows (99 before `rsamp`) — and three test executables build exactly that way
  (`tone_regressions`, `dualcab_test`, `delay_sync_trails`). Nothing persists an
  index (the index-keyed exports are read only inside
  `JuceDspModuleCatalog.Load()` and everything downstream keys by type id), but
  never compare a flags or category table across builds by index, and never
  write a test whose floor is a bare count. `src/dsp/GearEntry.h` owns `Entry`, the category constants and
  `kFlagSidechain` — the factory TU no longer defines them.
- **`JUCESHARP_RIG_FACTORY_SOURCES` is the ONE CMake list of factory TUs**, fed
  to the DLL, the RT gate and the three test executables that compile the
  factory by hand (`tone_regressions`, `dualcab_test`, `delay_sync_trails`).
  The first build of the scaffold found those three listing
  `DspModuleFactory.cpp` directly: nine unresolved externals at link time. Add
  a gear file to that list, nowhere else.
- **Each drawer has one RT scenario** (`scenario*Gear*` in `rt_main.cpp`, run
  lines grouped after `RtFullRigAllocatesNothing`'s) that chains every module
  in the drawer under the allocation sentinel for 200 blocks WHILE throwing
  every switch the drawer has, plus one managed test file per drawer
  (`tests/JuceSharp.Tests/Dsp/Gear<Category>Tests.cs`) carrying, per module: a
  behavioural claim that was BROKEN ON PURPOSE and seen to fail, a
  three-render ramp case on every level/mix, NaN rejection, a two-rate
  `prepare()` case and an oversized-block case. `RtFullRigAllocatesNothing`,
  `RtSidechainKeyingIsAllocationFreeAndAdvertisedHonestly` and the managed
  catalogue lints walk the catalogue, so a new row is under them the moment it
  exists. Consumer-facing pages: `docs/guides/gear-<category>.md`.
- **Lessons the wave paid for, now rules:** (1) a flat-sum probe passes with
  a crossover compensator DELETED unless the crossovers interact — pick
  frequencies that do (multiband, 500/1000 Hz showed 3.16 dB where 150/2500
  showed nothing); (2) an alias theory at catalogue DEFAULTS is vacuous —
  `poweramp` with oversampling forced to 1× still measured −126 dB at
  defaults; measure DRIVEN; (3) a seam probe compared against the signal's
  own settled slope is vacuous for anything with a ramped edge per step
  (rhythmdelay) or diffusion after the tap (every reverb) — compare against a
  CONTROL render with the param left alone, and read across the render
  boundary, because the change lands at sample 0 of the next block; (4) the
  `gain` node clamps at −24 dB, so a `pad(−40) → keyed module` idiom really
  pads by 24; (5) a freshly reset FDN tank emits nothing until its shortest
  line wraps and then switches lines on in turn — **a reset tank must not be
  crossfaded in until every line has filled** (`hallreverb` warms up 200 ms
  in silence before its 40 ms space fade); (6) a `*ForTest()` accessor that
  reads `smoother.current()` walks whether or not the core USES the smoother —
  pin the value the DSP consumes.
- **Shared building blocks that came with the wave:** `dcdsp::revdetail`
  (`dcdsp_hallreverb.h`: FDN, predelay, diffuser, early-reflection multitap,
  reused by `gatedreverb`), the uniform-partitioned FFT convolution in
  `convreverb` (partition = block size, worst case 10 s / 480,000 taps, one
  partition of latency, 385 ns/sample at a 2 s IR — the cab's DIRECT FIR stays
  because its zero-latency contract cannot survive a partition, see the cab
  bullet), and `MasterLimiter.h`'s sliding-minimum design re-implemented as the
  `limiter` core. Every tracker-based pitch module (`pitchcorrect`, `basssynth`)
  carries its own native autocorrelation tracker — the managed CQT/YIN are
  NOT reachable from the callback.
- **New param SHAPES the pedalboard UI has never rendered:** a string param
  that is a file path (`convreverb.irFile`, `*.wav`, refusal keeps the old IR),
  a footswitch with a state machine (`looper.state`) and a one-shot trigger
  that reads back 0 (`looper.undo`), a 13-param module with three repeated
  per-band triples (`multiband`, `low*`/`mid*`/`high*` ids for grouping), and
  drawer modules with non-zero, user-controllable latency (`tzflanger`,
  `limiter`, `convreverb`, `micpos`, `tapesat`, `bassod`) that branch PDC
  handles but a per-node latency display will now show.

## Auto-threshold gate and the crusher's drive / lo-fi stages (2026-10-05, DotCharter #155 / #157)

Two original modules gained params, no new catalogue row, no ABI change:

- **`gate.autoFloor`** (Off/On, default Off). On, the threshold FOLLOWS the
  input's own noise floor and `thresholdDb` is its ceiling; the threshold is
  floor + 6 dB (twice the floor's amplitude). `dcdsp_gate.h` measures the floor
  whenever the input sits STILL: one envelope maximum per 50 ms frame (2.5+
  cycles of mains hum), and every 100 ms the last second of frames — p90 − p10 ≤ 4 dB is a floor, a
  note's decay is not. A lower reading lands at once, a higher one at most
  20 dB/s (the first reading is the calibration; at 6 dB/s a rig that had
  measured digital silence took 6 s to shut on a -60 dBFS hiss); the floor never goes under
  -96.3 dB (one 16-bit LSB), and it STARTS there until the first reading (#63),
  so nothing played before the gate has measured is cut. Fixed-size state, two
  `nth_element`s over 20 doubles per 100 ms.
  DotCharter prepends it to every Rocksmith song tone (`RsInputGate`).
- **`bitcrusher`'s FutzBox-style stages** (section order and Lo-Fi controls as
  McDSP's FutzBox manual publishes them; the laws are ours) — `inputDb`, `drive` (0 = no distortion
  stage; else 0.5 dB of gain per %, the same shaper `dcdsp_rsamp.h` runs), `intensity`,
  `rectify`, `filter` (a Butterworth low-pass from 0.45 × rate at 0 % to the
  held signal's Nyquist at 100 %, log-linear; inferred) and `outputDb`, all on
  the CRUSHED path only, in the order input → shaper → DC block → hold →
  filter → quantize → output. At their defaults the path is the classic
  crusher bit for bit (quantize-then-hold equals hold-then-quantize).

Pinned by `GateAutoFloorAndFutzCrusherTests`, two `DspGainZipperTests` rows and
the RT scenario `RtAutoFloorGateAndFutzCrusherAllocateNothing`.

## Branch PDC: when the dirty flag is read (W5-E2 / W5-E3, 2026-09-12)

The re-derive after a param write (`recomputeBranchPdcRt`) must run at the END of
a block, because a module only adopts its `ParamMailbox` at the top of its own
`process()` and the merger has to be programmed from the latency the core
actually ran. That much is W4-D54 and is unchanged.

**The FLAG is a different question, and getting it wrong reopened the same bug
one callback wide.** `pdcDirty_.exchange(false, …)` sat at the bottom of
`process()` alongside the recompute, so a `setParam` landing after a module's
adopt and before that exchange was consumed by a block whose modules had run the
OLD latency: the walk programmed the mergers from the old number, cleared the
flag, and — exactly as before — nothing came back. The region stayed misaligned
"until an unrelated publish". At 5–10 ms blocks that window is the ordinary case
for a UI drag.

It is now **sampled and cleared at the TOP** of `process()` and acted on at the
bottom `if (redoBranchPdc)`. That is also what makes the release/acquire pair
carry the guarantee the code wants: a flag observed at the top means the mailbox
write happened-before, so every module adopts it in THIS block. A write landing
after the sample leaves the flag set for the NEXT block, which is correct.

`branchPdc_` is an `std::atomic<bool>` for the same wave: `recomputeBranchPdcRt`
reads it from the audio thread, which may not take `writeMutex_`, and every other
cross-thread scalar in `DspChain` is an atomic for exactly that reason. Relaxed
on both sides — it orders nothing. Neither change touches `rt_lock_sites.txt`:
neither adds a lock.

Pinned by the mid-block phase of `RtBranchPdcFollowsAParamOnTheBlockItChanges`,
which parks the audio thread inside a block with a gate module and has a writer
thread perform the `setParam` there — deterministic, not raced.

## The rig, the catalogue and the cores

The bullets below sit under `src/JuceSharp.Native/` in the source guide; the
nesting is kept as-is so the text is byte-identical to what it replaced.

  - **In-house DSP tone rig (`src/dsp/`, ABI v24, 0.16 Phase 0):** a
    reorderable in-process effect chain applied to the guitar INPUT before the
    user VST chain (our rig first, VST3 the escape hatch). `DspModule` (thin
    processing-node interface — NOT a `juce::AudioProcessor`) + `DspChain`
    (reorderable, RT-safe via the same atomic-snapshot + wait-one-block barrier
    the input VST chain uses; module STATE is stable across reorders) +
    `DspModuleFactory` (catalogue + `createModule`). The effects are pure
    header-only `dcdsp` cores in `src/dsp/cores/` (`dcdsp_primitives.h` +
    **57-entry catalogue (0.18):** gain(Clean Boost)/gate/compressor/eq/
    overdrive/distortion/amp/cab/tremolo/chorus/flanger/delay/reverb, plus
    fuzz/phaser/wah/octaver/ringmod/bitcrusher/tapedelay/platereverb/rotary/
    formant/graphiceq/harmonizer/vibrato/sustainer (0.15.1), plus
    springreverb/shimmer/univibe/harmtremolo/envfilter/doubler/acousticsim/
    noisereducer/klon/bigmuff (0.15.2), plus **two Miku vocal stomps** (see
    below) and namamp
    (NAM/AIDA-X neural captures via FetchContent'd NeuralAudio, MIT, pinned to
    the release tag **v0.1.3**; NOT a pure core — worker-thread model load,
    `NamAmpModule.cpp` is the only NeuralAudio TU) (0.16, amp face + SRC in
    0.36 — see the namamp block below), plus autowah (envelope-driven
    CryBaby-voiced funk wah, distinct from envfilter), boost (clean/treble
    booster with tilt + character), dimension (Dimension-C dual chorus:
    counter-modulated tap pair = AM widening with zero FM/vibrato) and the
    splitter/merger routing nodes (0.16 W5), plus the **0.18 expressive wave**:
    swell (envelope volume/auto-swell — the only new LEVEL module),
    whammy (swept WSOLA treadle, +/-24 semis, optional LPC formant preserve),
    reversedelay/slapback/freeze/tapestop/glitch (performance time effects),
    subharmonic (phase-locked polyBLEP synth bass — NOT a second octaver),
    filterseq (step-sequenced SVF; mode=Random IS sample-and-hold, which is why
    there is no separate `samplehold`), feedbacker (bounded self-oscillating
    howl), resonator (tuned Karplus-Strong comb bank) and widener (M/S +
    side-only Haas, mono-safe BY CONSTRUCTION). They share
    `cores/dcdsp_detail_pitch.h` (a DETAIL header — no module, no catalogue
    entry). The 0.18 `chorus` gained four params that are each a NO-OP at their
    default (feedback 0 / spread 0 / lfoShape 0=Sine / **wetHpfHz 20 == the
    filter BYPASSED, not "run at 20 Hz"** — that is what keeps the default
    bit-identical to 0.16). `harmonizer` is a WSOLA pitch
    shifter (was granular). **Gear roster (0.16):** `amp` has a 24-model
    `voicing` (RANGE 0..23; 0-6 unchanged so persisted ints keep their sound,
    7-23 append named soundalikes — Tweed/Blackface/Plexi/JMP/AC/Recto/Djent/
    boutique/bass/DI) — **27 since 2026-10 (DotCharter #70): 24 Tweed Bass 45,
    25 Plexi 50 Circuit, 26 Plexi 100 Circuit are CIRCUIT voicings**, a family
    row with its shelf trio replaced by the passive TMB tone stack modelled from
    its component values (`cores/dcdsp_tonestack.h`: Yeh & Smith, DAFx-06,
    eq. (1), bilinear at the 4x rate; Fender 5F6-A and Marshall 1987 values,
    each cited in the header). Their bass/mid/treble are POT ROTATIONS (noon is
    0.5, not "flat" — a passive stack has no flat setting), and the stack is
    divided by its own noon RMS gain so only the level, never the shape, is
    ours. `amp` also gained `bright` (Off/On — Fender's AB763 bright cap, 120 pF
    across a 1 MOhm audio-taper pot, between the first and second stage) and
    `resonance` (0..1 — `PowerAmpResonance` in `dcdsp_poweramp.h`, shared with
    the `poweramp` module); both are SKIPPED at their default, so rows 0-23 and
    every saved preset render bit-identically. `amp_circuits_test` holds the
    closed form to an independent nodal solve of the schematic (<1e-6 dB), the
    discretisation to the analog response (<0.1 dB to 10 kHz) and the stacks to
    Kuehnel's published break frequencies. `cab` a 26-model `model` (RANGE 0..25; 0-3 unchanged,
    4-11 new synthesized min-phase IRs in `dcdsp_cab_irs.h`, 12-25 appended
    2026-10 for DotCharter #74 — built by `tools/cab_irs/gen_cab_irs.py` from
    MEASURED driver responses (Jensen's published 1 W / 1 m IEC-baffle sweeps,
    `tools/cab_irs/driver_curves.json`) through Small's closed-box and Keele's
    QB3 vented-box formulas, piston directivity and a 2.5 kHz LR4 crossover;
    the generator copies rows 0-11 back VERBATIM and the `tone_fidelity_cores`
    ctest pins them by an FNV hash of their float bits), and both
    `overdrive`/`distortion` a 4-model `model` (0 = original sound). All use a
    shared topology + per-model coefficient table so CPU stays flat; model 0/
    voicings 0-6 & cab 0-3 are bit-identical to their predecessors. Model index
    -> display name lives in the **ABI v24 value-label catalogue**
    (`dca_dsp_module_param_label_count`/`dca_dsp_module_param_label`, size-then-
    copy UTF-8, -1 sentinel for continuous params) via `ParamInfo.valueLabels`.

    **Every DISCRETE param must declare itself — a param the adapter lrounds is
    never a plain knob (ABI v27).** Two shapes, and a discrete param is exactly
    one of them: NAMED (`valueLabels` — a picker: tremolo shape, wah mode, a
    footswitch) or ORDINAL (`ParamInfo.step` > 0, read via
    `dca_dsp_module_param_step` — a knob that SNAPS: chorus voices, bitcrusher
    bits, and phaser `stages` which is **step 2** because the core forces the
    allpass count even). Otherwise the UI lets the user park a knob at "6.5
    stages" while the core runs 6. Three params LOOK discrete and are genuinely
    continuous blends — read the header before you "fix" them:
    `compressor.detector` (peak/RMS mix), `univibe.mode` (chorus↔vibrato morph)
    and `envfilter.direction` (up/down sweep blend; the autowah's same-named
    param IS an int switch).
    Cores have ZERO JUCE/framework deps,
    each self-tested with a standalone MSVC harness; the factory wraps each in a
    DspModule adapter mapping string param ids to the core's params struct.

    **The two Miku modules — `miku` and `mikustomp` — are BOTH shipped on
    purpose. Do not "unify" them.**
    - `miku` (`dcdsp_miku.h`, 0.16): our own vocal stomp. Autocorrelation pitch
      tracker + onset-driven lyric sequencer + a 3-formant parallel voice with
      per-vowel amp tables. Deliberately robotic and fun. **Owner wants it kept
      exactly as it is** — treat its sound as frozen.
    - `mikustomp` (`dcdsp_mikustomp.h`, 0.16): a recreation of the real KORG
      MIKU STOMP. The hardware is a guitar front-end on a Yamaha NSX-1 running
      *eVocaloid*, which CONCATENATES recorded diphone samples from Crypton's
      Hatsune Miku voicebank via Articulation Element Modeling (head / body /
      joint / tail fragments). We cannot ship that voicebank, so we synthesise
      the same STRUCTURE with **EpR** (Excitation plus Resonances — the model
      VOCALOID itself is built on): polyBLEP glottal pulse + spectral tilt +
      chest resonance + pitch-synchronous aspiration, into a **4-deep Klatt
      CASCADE** (F1..F4) + nasal antiformant, over a young-female-Japanese vowel
      table. Articulation is a 22-entry Japanese moraic consonant table with
      Delattre F1/F2/F3 **loci**: the formants glide locus -> vowel over the
      NSX-1's fixed 50 ms window, which synthesises a diphone. A re-pick gets
      the full HEAD (closure + burst); a legato note change gets a JOINT (pitch
      + formants glide, no head) — the AEM head/joint split. 11 lyric patterns
      in the pedal's own knob order, 3 user Phrase slots (`phrases`/`phrase2`/
      `phrase3`; `phrases` shares v1's id so one lyric string works in either),
      wet by default (`mix` 1) because the hardware replaces your guitar.
      The header carries the full research summary and an honest list of where
      we differ from the hardware. ~3.0% of one core, flat across block sizes.
      **0.18.1 intelligibility pass** (measured with the standalone
      `src/JuceSharp.Native/tools/miku_measure.cpp` harness — LPC formant tracking + WAV dumps; the
      research notes it cited, `docs/miku-stomp-research.md`, were never tracked
      and do not exist): the formant glide is armed at the
      **RELEASE**, never at the syllable start; the nasal antiformant gained its
      **Klatt nasal POLE**; stops gained **aspiration (VOT)**; frication got a
      second noise pole and Klatt's ramp-in envelope; /h/ is now excited at the
      glottis through the FOLLOWING vowel's formants; and note-off is a dedicated
      fast fade. **No param changes** — the knob table is untouched.
      **0.20 syllable pass** (harness T11-T16; same doc): a phrase starts at its
      FIRST mora (the index used to init at 0, so "nyan" opened on the /N/ hum)
      and the pattern knob restarts it; the output tanh is driven at unity with
      the 2.2 makeup AFTER it (its cubic products used to read as an extra
      formant at ~1.7 kHz on /o/); the onset detector keeps up at 16 picks/s
      (25 ms release / 30 ms lookback / 45 ms refractory); affricates attack in
      3 ms (the /s/-vs-/ts/ rise cue); the nasal normaliser evaluates |H| at the
      pole too, with a real nostril loss; and F1 is pitch-floored at ~1.05*f0
      (soprano formant tuning — /i/ recovered 13.6 dB at a sung 659 Hz).
      Again no param changes.

    Three measured facts from building `mikustomp` that apply to ANY
    pitch-tracking pedal here — do not regress them:
    - **Onset detection must use a HIGH-FREQUENCY envelope, not a broadband
      one.** Re-picking over a still-ringing string only moves the broadband
      envelope ~1.4x, so a 2x threshold misses half the picks (this is why v1
      can feel unresponsive). The >1.2 kHz envelope jumps 2.1-4.0x and peaks
      1-5 ms after the pick instead of 6-20 ms.
    - **A per-hop pitch-ratio test cannot see a hammer-on.** The tracker's own
      analysis window delivers the pitch change as several ~3-6% steps, each
      below any sane per-hop threshold. Compare against the pitch ~40 ms ago;
      that also lets a slow BEND glide without re-triggering a syllable.
    - **Normalise a Klatt cascade before any soft-clipper.** Un-normalised it
      swings 1.2..7.4 peak by vowel/pitch, which pins the clipper in saturation:
      its distortion products sat 10-17 dB *above* the F3 valley and audibly
      veiled the third formant. Divide by F1's peak gain (a pure scalar — it
      moves no formant). Dividing by the product of all four peak gains
      over-normalises by ~10^4 and leaves the voice inaudible.
    - The pitch analysis is **hop-limited** (~4 ms), not once-per-block, so CPU
      does not blow up at a 64-sample buffer. `miku` v1 does tie it to the block
      and costs 3x more at block=64 than at block=256.

    Four more from the 0.18.1 pass. These are about SPEECH, and any future vocal
    module here will hit all four:
    - **A formant transition that happens while the tract is shut is inaudible,
      and the transition IS the consonant.** The locus->vowel glide used to be
      armed at the syllable start, so all 50 ms of it ran during the closure and
      the burst with the voice muted. Measured: F2 at the first VOICED frame sat
      at ~1650 Hz (the vowel's own value) whatever the locus was — b, d and g were
      literally the same sound. **Arm the glide at the RELEASE.**
    - **A Klatt nasal ZERO is not a notch — it is a +12 dB/oct treble ramp with a
      dip in it** (measured on our own recipe: +21 dB @ 2 kHz, +45 dB @ 8 kHz).
      Klatt ALWAYS pairs RNZ with RNP and parks the zero **on** the pole when the
      sound is not nasalised, where they cancel exactly — which only works if both
      keep the **same bandwidth**. Without the pole every murmur came out brighter
      than a plain vowel instead of 15-25 dB darker.
    - **Voiceless stops need aspiration (VOT), not just a burst.** The formant
      sweep has to run *through* the aspiration, because a voiceless stop has no
      voicing to carry its place of articulation.
    - **Stacked envelope releases are a hangover.** A 50 ms gate feeding a 150 ms
      level follower feeding a 90 ms amp release meant the voice kept singing for
      **715 ms after the input hit digital silence** — the owner's "weird blob at
      the end". Note-off must be its OWN fade, not the sum of the followers.
      (`miku` v1 does NOT have this: it cuts with a 15 ms linear ramp.)
    C ABI: `dca_engine_dsp_*` (add/remove/move/bypass/set-param/list) + static
    `dca_dsp_module_*` catalogue for a generic managed UI. **ABI v23 adds
    split/parallel chains** (`RoutingModules.h`): `splitter`/`merger` catalogue
    modules mark a 2-3 branch parallel region the chain executes over COPIES
    of the split input (per-branch level + balance-law pan at the merger;
    mode 0 average = bit-exact null, mode 1 sum); per-node branch tags via
    `dca_engine_dsp_set/get_node_branch` (0 = branch 1). Branch latency
    differences were v1-accepted (no PDC) until **ABI v34, which compensates
    them at the merger by default** — see "Branch delay compensation" below;
    bypassed splitter degrades linear,
    unmatched splitter auto-merges at chain end. **ABI v22 adds
    string-valued params** (`dca_engine_dsp_set/get_node_string` +
    `dca_dsp_module_string_param_*` catalogue with a file-picker filter):
    cab/acousticsim `irPath` (user WAV IRs via the bounded `IrWavLoader.h`
    parser, "" reverts to the bundled bank), namamp `modelPath`, miku
    `phrases`, mikustomp `phrases`/`phrase2`/`phrase3`.
    Heavy string-param work (file parse, model load) must happen
    off the audio thread inside the module. Add a module = one
    core header + a factory adapter + a catalogue entry (param ids/ranges MUST
    mirror the adapter, AND the managed hand-mirror
    `DcRigTargetCatalog` in DotCharter.Services) + **a CATEGORY on the `Entry`
    row (ABI v33)**, one word out of the fixed set `kCategory*` declares above
    `entries()`: dynamics / drive / amp / cab / modulation / delay / reverb /
    pitch / filter / eq / utility / routing / synth. It is a real field on
    `ModuleTypeInfo` and it defaults to `""`, so a module added without one
    compiles perfectly cleanly and silently falls out of every palette drawer —
    `tone_regressions`' `everyCatalogueEntryHasAKnownCategory` is the only thing
    that says so, and it rejects an off-vocabulary word too (a typo like
    "reverbs" would otherwise read as a brand-new drawer holding one pedal).
    **Never mirror the vocabulary managed-side**: it is exported by
    `dca_dsp_module_type_category` precisely so there is one source of truth. **Adapter float params go
    through `ParamMailbox` (DspModule.h): setParam updates the adapter's `p_`
    (getParam read-back keeps its clamp/lround semantics) and `publish()`es;
    `process()` calls `mail_.applyPending(core_)` so `core_.setParams` only
    ever runs on the audio thread. Never call `core_.setParams` from
    `setParam` — that recomputes multi-word coefficients while process() may
    be reading them (2026-07-16 audit #4).** Single scalars may use a plain
    atomic instead (see CleanGainModule / RoutingModules). Biquad coefficients compute in
    `double`, so low-freq filtering is safe at base rate; do NOT run the float
    `Biquad` HPF/shelf at oversampled rates — use `OnePole` (first order) or
    **`BiquadD`** (0.16 W6: the same RBJ sections with double coefficients AND
    double state, for shelves/peaks that must run at 4x/8x). **The `amp` core
    (0.16 W6) runs its ENTIRE path — preamp cascade, cathode follower, tone
    stack, power amp, output transformer — inside one 4x oversampled region.**
    The pre-W6 model oversampled only the preamp waveshapers at 2x and clipped
    the power amp at base rate, measuring -32..-34 dBc of in-band alias hash on
    the hi-gain voicings — that was the "fizz". Two hard-won rules live in
    `dcdsp_amp.h`: (1) an envelope-driven bias shift MUST use a memoryless
    rectifier + a LINEAR RC — the shared peak `EnvelopeFollower`'s
    attack/release switch is a nonlinear recursion and period-doubles into ghost
    partials (it cost 37 dB of inharmonic floor); (2) inter-stage Miller
    low-passes belong at 9-18 kHz, NOT 5-8 kHz — lower corners eat the 2-6 kHz
    presence harmonics the tone actually lives on. Oversampling, not the Miller
    corner, is what kills aliasing.

    **`namamp` — the amp face and the rate converter (0.36).** A NAM/AIDA-X
    capture is one amp at ONE knob setting; it has no controls and its tone
    stack cannot be recovered from it. The module therefore wraps it in a face
    (`cores/dcdsp_ampface.h`, a DETAIL core — no catalogue entry of its own):
    `gain` PRE-capture (the only control that genuinely changes the
    nonlinearity, because the capture's response is a function of input level)
    and `bass`/`mid`/`treble`/`resonance`/`presence`/`master` POST-capture. The
    stack is post BY DECISION: the capture already contains preamp AND power
    amp, so there is no seam to insert a stack into, and a pre-capture stack
    would only re-spectrum the drive feeding a nonlinearity that has its own
    stack baked in. **Every knob defaults to 0.5 and neutral is a STRICT no-op**
    — each band is SKIPPED rather than run at 0 dB, so every pre-0.36 preset
    stays bit-identical. (Measured footnote: an RBJ section at 0 dB is ALREADY a
    bit-exact identity in this TDF-II form — `A == 1` makes `b1 == a1` and
    `b2 == a2` after the `a0` normalisation. The skip is the CPU saving and the
    structural guarantee, not the thing that makes neutral exact.)
    Measured absolute, input -18 dBFS, `tone_response.exe face`: bass +/-11.98
    at 20 Hz, mid +/-11.96 at 630 Hz, treble +/-12.00 at 16 kHz, presence
    +/-8.98 at 16 kHz, resonance +/-8.87 at 100 Hz, master -inf..+12.04, gain
    -18..+18 dB; neutral 0.00 dB at every probe and bit-identical over 2.05 M
    samples (`tone_response.exe face neutral`). **`resonance` does NOT replace
    `dcdsp_cab.h`'s `kResonance`** — that one is always-on inside the cab's wet
    path and restores the driver Fs the IR bank has no trace of; this one is
    player-facing and FLAT by default, so a stock rig gets the bump exactly
    once.
    The module no longer dry-bypasses off 48 kHz. It **resamples around the
    capture** (`cores/dcdsp_detail_src.h`, arbitrary-ratio windowed-sinc,
    Kaiser beta 8.6, 512 phases, 32 taps scaled up for decimation): the
    converter pair is built ON THE WORKER THREAD as part of the loaded model,
    so the audio thread still never allocates, and the capture always runs at
    its own native rate. Round trip measured flat within 0.005 dB from 20 Hz to
    16 kHz at 44.1 / 88.2 / 96 kHz host rates, +1.42 / 1.00 / 0.97 ms latency
    (`tone_response.exe src <hostRate>`). **Do NOT "simplify" this to
    NeuralAudio's `SetExternalSampleRate`** — it only rescales WaveNet dilations
    when the engine rate is an exact INTEGER multiple of the model rate and does
    nothing at all on the RTNeural (`.aidax`) path, i.e. it serves none of the
    44.1 / 88.2 / 176.4 kHz users this exists for. `rateMismatch` survives as a
    poll-only param but now only fires when the converters could not be built
    — **and, since 2026-08-29, while a device-rate / block-size renegotiation
    is IN FLIGHT** (`reloading_`, set by `prepare()` when a snapshot is live,
    cleared when the worker publishes its replacement): in that window
    `process()` refuses the stale snapshot and passes dry, and the poll used to
    read "loaded, no mismatch" over a silent wire (`NativeRtSourceLintTests
    .NamAmpModule_ReportsARenegotiationInFlight_ThroughRateMismatch`);
    `resampling` is the new poll-only diagnostic.
    **The built-in `amp` module's DISPLAY NAME is "Legacy Amp"; its type id is
    still `amp` and must never change** — the id is persisted in every
    `DcRigPreset`, all 83 factory tone presets (70 use `amp`), every saved user
    tone's JSON and DotCharter's `DcRigTargetCatalog` mirror.

    **AN IN-PATH BASS FILTER IS A BOUNDED SHELF, NEVER AN UNBOUNDED HIGH-PASS
    (0.35).** This is the third rule and it cost the whole rig its low end. The
    circuit networks these filters stand in for — a cathode-bypass cap, a Tube
    Screamer's feedback-leg cut, a RAT's input network — are SHELVES of finite
    depth; the real coupling caps sit at 7-20 Hz and are out of band. Spelling
    them as high-passes and cascading them meant the response kept falling at
    6-24 dB/oct through the register the instrument's fundamentals live in.
    Measured (dB re the stage's own 200 Hz-4 kHz log-mean, at the low E,
    E2 = 82 Hz, input -18 dBFS): amp voicings -0.1..-15.0, screamer overdrive
    -20.6, every distortion model -11.5..-19.7, whole factory chains -9..-18.
    Three sites were fixed and each now carries a named depth budget —
    `dcdsp_amp.h` `cplCutDb` (via `OnePole::setHighpassShelf`, floor 0 =
    bit-identical to the old high-pass), `dcdsp_overdrive.h` `preCutDb`,
    `dcdsp_distortion.h` `kTightCutDb`/`kBodyCutDb` (both via
    `Biquad::setLowShelf`). After: amp -0.1..-3.5, chains -1.8..-3.9.
    **`dcdsp_cab.h` `kResonance`** is the other half: the generated IR bank has
    a box rolloff but NO driver Fs resonance (model 0 decays monotonically below
    200 Hz), so a per-model peaking section in the wet path restores it —
    SKIPPED for a user IR, which carries its own. **The cost is measured and
    accepted:** more LF into the clippers raises the power-chord off-grid
    residue by up to +20.8 dB (Modern Djent -88.4 → -67.6 dBc total,
    -99.6 → -85.4 dBc worst product) — still ~35-40 dB inside the pre-W6
    -32..-34 dBc regime this metric exists to condemn. Reproduce any of it with
    `tools/tone_response.cpp` (`sweep` / `cabsweep` / `filters` / `preset` /
    `alias`); the managed gate is DotCharter's `ToneLowEndResponseTests`.
    **The failure mode is that each such filter looks individually reasonable
    and they SUM — so never add a fourth without measuring the cascade.**

    **`DspChain::process`
    scrubs non-finite (NaN/Inf) samples to 0 at the input and after EVERY stage**
    — a single bad sample can otherwise latch a downstream feedback/IIR core
    (tapedelay/platereverb/graphiceq) into permanent NaN; keep that guard.
    Since 2026-08-29 the implementation is `dcdsp::scrubNonFinite`
    (`src/dsp/Scrub.h`), shared with `AudioEngine`, which needs the same pass
    after ITS untrusted stages (the VST3 input chain, the sandbox bridge, the
    instrument mixes) — see `rt-safety-and-tests.md`. Change the rule in one
    place.
    **The scrub cleans the BUFFER, never a module's internal state** — a NaN
    that reaches a recursive core's own memory is permanent until `reset()`,
    which is why a param that can produce one is still a bug the scrub does not
    cover.

    **So a NaN PARAM is rejected at `DspChain::setParam`** (2026-08-28), which
    is the single point every param write funnels through, and the rejection is
    REPORTED — `dca_engine_dsp_set_param` returns `DCA_ERR_INVALID_ARG` rather
    than swallowing it. Every core clamps with `v < lo ? lo : (v > hi ? hi : v)`
    and both comparisons are false for NaN, so a clamp is transparent to it;
    `Inf` IS caught (`Inf > hi`), which is why NaN was the only hole. Four cores
    (`filterseq`, `feedbacker`, `resonator`, `graphiceq`) already carried their
    own `if (!(v == v))` — the intent existed and had reached 4 of 54.
    `RtNaNParamIsRejectedAndDoesNotKillTheModule` drives a fully-wet `delay`
    through the real rig on the null device and asserts both halves; verified
    non-vacuous by removing the gate, where the delay's output peak goes
    0.242846 → **0.000000** and stays there.

    **Since 2026-09-11 the cores no longer depend on that one gate holding.**
    The clamp helpers are still NaN-transparent by construction, but the three
    places where a NaN became UNRECOVERABLE are closed, so the chain-level
    rejection is defence in depth rather than the only thing between a NaN and a
    permanently silent pedal:
    - `dcdsp::clampCutoff` writes its low bound NEGATED (`if (!(fc >= 1.0))`),
      `common()`'s Q guard the same way, and a new `dcdsp::clampGainDb` bounds
      the shelf/peak gain — one change covering every filter in all 47 cores,
      because `Biquad::reset()` and `StateVariableTPT::reset()` clear STATE and
      never the design.
    - `gate` and `noisereducer` RECOVER their persistent smoothed gain to unity
      when it goes non-finite (`clampd(g, 0, 1)` let a NaN straight through, and
      that gain multiplies every subsequent sample for the life of the node).
    - `MikuCore` gained the `hardReset()` `MikuStompCore` already had, invoked
      when its own voice sample is non-finite — the output was scrubbed while
      `svf_[]`, `voiceDc_` and `adaa_` kept feeding the NaN back.
    Pinned by `RtNaNCannotLatchACoreCoefficientOrGain`, which drives the cores
    DIRECTLY (through the rig the NaN never arrives) and checks the primitives
    one by one; verified non-vacuous by reverting each half — the Biquad, the
    TPT filter and the one-pole all read `nan` for ever, and the gate and miku
    both go to peak 0.000000 and stay there.

    **`dcdsp::clampCutoff`'s ceiling is a QUARTER of the sample rate, not
    Nyquist** (`nyq * 0.4999`, unchanged since the rig's first commit; its
    comment guards `tan(pi*fc/sr)`, which needs only `fc < sr/2`). Every core
    is voiced against it, so it was NOT moved GLOBALLY. A design whose PARAM
    advertises a corner past a quarter of the rate OPTS IN to
    `clampCutoffNearNyquist` (0.9 × Nyquist) through the `nearNyquist` flag
    on `OnePole::setLowpass`, `Biquad::setPeaking` / `setLowpass` /
    `setHighShelf` and `StateVariableTPT::set` — off by default, so every other
    corner is coefficient-for-coefficient what it was. The opted-in designs
    (DotCharter #68, #87): the `eq` treble shelf; `svf.cutoffHz` and its
    LFO/envelope sweep; `graphiceq`'s 16 kHz band; the `Fdn8` damping one-pole
    behind `hallreverb` / `gatedreverb` / `reversereverb` (`dampingKHz` 1-16);
    `platereverb`'s damping (250 Hz-16 kHz; its fixed 16 kHz input `bandwidth_`
    keeps the old ceiling so the default plate does not move);
    `convreverb.highCutKHz`; `tapesat.hfRolloffKHz`; and the micpos tube mic's
    top shelf and every mic's off-axis shelf. Raising the tank's damping put the
    damping-times-DC-blocker product above unity (1.0003 worst, 16 kHz at
    44.1 kHz); `g_i` carries the margin — the derivation is at `Fdn8`.
    Pinned per core by the `tone_fidelity_cores` ctest (RBJ / TPT response at
    the corner, and a 30 s ring of the worst-case tank) and JuceSharp's
    `FilterCornersAboveQuarterRateTests` (offline render; each 16-vs-12 kHz pair
    rendered sample-identical before), plus `EqWidenedRangeTests`.

    Note this does NOT undo the "permanent until `reset()`" sentence above.
    `DspModule::reset()` had NO caller at all until 2026-08-29 and now has
    exactly one — `DspChain::setBypass(id, false)` on a PARKED node, see below —
    which is not a recovery path: nothing reaches it while a latched module is
    live and un-bypassed. The ways out of a latched core are still a device
    change, a bypass round-trip, or removing and re-adding the node. No ABI
    export clears a module directly, and adding one has not been argued for.

### `DspModule::reset()` has ONE caller, and where it is NOT called matters more

Fifty-four adapters implemented `reset()` and nothing called it (found in the
0.48 audit). Wired, rather than deleted, and at one place: **`DspChain::setBypass(id,
false)` when the node's fade has already landed at 0.**

Why there:

- **A generic parked node is not decaying, it is FROZEN.** `DspChain::process` skips a
  generic node whose wet weight has reached 0 (`continue`), so its delay lines, reverb
  tanks, envelope followers and LFO phases stop at the instant the fade landed.
  Un-bypassing then fades that stale audio back IN, which is neither thing a
  pedal does: true bypass kills the tail, trails let it decay away. Measured on
  the old code with a 60 ms / 0.9-feedback delay: RMS 0.2146 came back against a
  0.2708 live tail — 79 % of it, from before the bypass.
- **It is safe exactly BECAUSE the fade has landed.** `n.fade->wet` is stored at
  the END of the block in which the module was last processed, so once the
  message thread reads 0 the audio thread is already past that node and every
  later block skips it; the node stays marked bypassed in the live snapshot
  until `publishLocked()`. Calling `reset()` on a node that is still fading — or
  still live — would race the callback.
- **A quick toggle keeps its state.** Bypass and un-bypass inside the 10 ms ramp
  is not a park, so the check simply does not fire. That is also the musical
  answer.

The opt-in `DelayModule` bypass path is an exception: it remains callback-owned
at zero weight so trails can decay. `resetIfParkedLocked` publishes
`requestBypassReset()` for it rather than clearing buffers concurrently. The
callback clears hard-bypass history on re-enable; continuously advancing trails
survive re-enable. Generic modules retain the parked/reset path above.

Why NOT the alternatives, all three of which were considered:

- **`DspChain::prepare()` — redundant.** 51 of the **54 headers in
  `src/dsp/cores/` that declare a `*Core` class** already call their own
  `reset()` at the end of their own `prepare()`; only `graphiceq`, `ringmod` and
  `dualcab` do not (re-measured 2026-09-11 — the denominator is 54 cores, not the
  63 header FILES and not the 57 catalogue entries). A
  chain-level call would be a no-op for almost everything and could not be given
  a test that fails without it.
- **Transport stop — not a chain event.** The rig runs on LIVE INPUT; the
  transport is the backing track. Clearing a reverb because the song stopped is
  wrong, and there is no path from one to the other.
- **The internal move/remove fade — forbidden.** `fadeOutAndWait`/`fadeIn` drive
  the bypass flag directly and never call `setBypass`, which is what keeps the
  promise at the top of DspChain.h that reordering a delay never clears its
  buffer. `DspBypassClearsStateTests.MovingADelay_KeepsItsTail` pins that half
  and would have caught a reset put in the fade instead.

## The sidechain bus (ABI v34)

`DspModule::process` was single-buffer from the start, so the compressor's
"sidechain" was an internal 100 Hz high-pass on its OWN input — a detector
shape, not a sidechain. v34 adds an opt-in second buffer:
`supportsSidechain()` + `processSidechained(ch, nch, n, key, keyCh)`, whose
DEFAULT forwards to `process`, so every catalogue module type that predates it (the
54 cores plus the `splitter`/`merger` routing nodes and `namamp`) are untouched and
`process` stays the pure virtual everything overrides.

Sources: 0 none, 1 the seat's DRY input (the chain's input block), 2 the song /
master mix. Wired today on `compressor` and `gate` — both had a single
`channels[ch][n]` detector line, which is exactly the shape that takes a key.

Six things are load-bearing:

- **A source a module would ignore is REJECTED at `setNodeSidechain`**, not
  stored. A stored-and-ignored source is this repo's dead-feature bug in
  miniature: the UI offers a control, the control does nothing, and everything
  compiles.
- **The capability is advertised by a SECOND export** —
  `dca_dsp_module_type_flags`, bit 0 — because `dca_dsp_module_type_info`
  cannot change signature. That makes the `Entry::flags` table a second source
  of truth for one fact, so the RT gate sweeps the whole catalogue and compares
  each flag with a CONSTRUCTED module's `supportsSidechain()`.
- **An unavailable source falls back to the module's own input, never to
  silence.** Keyed from silence a GATE sits closed: the rig goes quiet and
  nothing on screen says why. That is why the chain calls plain `process` when
  it has no key rather than handing over a zeroed buffer.
- **The dry key is a copy of the input block, allocated in `prepare()` for the
  worst case and filled only when the published order asks for it.** The
  `rtSidechainDry_` flag is decided on the message thread in
  `publishListLocked`, the same "off until asked for" discipline as the meters —
  and the buffer is sized whether or not anything is keyed, because a node can
  be keyed while the device is running and growing it on the callback is the
  audit-#5 bug.
- **The song key is ONE pointer**, handed to `processDspRig` per block by the
  engine callback (`out`, after `processMasterBus`). For seat p it therefore
  also carries the monitor of every seat processed earlier in the same
  callback — the honest cheap answer, documented in the header rather than
  quietly true.
- **Only the DETECTOR moves.** The audio path, the wet/dry crossfade and the
  per-stage scrub are byte-for-byte what they were, and a keyed node is still
  one `process` call from the chain's point of view.

Evidence: `DspSidechainTests` renders `gain(-40 dB) -> compressor` twice with
ONE property changed — unkeyed the compressor sits far below its threshold and
is a wire, keyed off the dry input it is well above it and pulls the same quiet
signal down by more than 40 %. A sidechain that were quietly ignored produces
identical renders and fails. It also pins the unavailable-source fallback (a
gate keyed to the song mix on the offline path must sound exactly unkeyed) and
the rejection on a module without a detector.
`RtSidechainKeyingIsAllocationFreeAndAdvertisedHonestly` covers the callback
(zero allocations with two keyed nodes live) and the flag/instance agreement.

## Branch delay compensation at the merger (ABI v34)

From v23 to v33 a parallel region summed branches that carried different
latencies. That is a COMB FILTER, not a blend — a 2-sample module in one branch
of a two-branch split makes the merger compute x[n] + x[n-2], which notches the
spectrum — and the chain had the numbers all along: `chainLatencySamples()`
walked the regions, computed the per-branch totals, returned the max and threw
the rest away.

`MergerModule` now owns one delay line per branch per channel, and
`DspChain::recomputeBranchPdcLocked()` (the same walk) sets each branch's delay
to `max − its own total`. Default ON; `setBranchPdc(false)` restores the
pre-v34 sum byte for byte, because the comb is a sound some players want.

Five things are load-bearing:

- **The rings are sized in `prepare()` for the WORST case, never from the
  runtime delay** — `kPdcCapacity` = 8,192 samples per branch per channel,
  which is more than the largest single-module latency in the catalogue at any
  supported rate (whammy: 7,680 at 192 kHz). A branch that stacks past it
  CLAMPS. This is the `whammy` lesson from the `prepare()` rules applied
  pre-emptively: every ring access masks with `kPdcCapacity`, so a geometry
  that grew at runtime would index outside the allocation.
- **A delay CHANGE is crossfaded across the block**, old tap to new tap. A
  param can move a latency (whammy's `lowLatency` flips 1,152 ↔ 1,920 samples),
  and moving a delay tap in one sample is a splice — the same rule
  `resonator`'s per-voice tap follows.
- **At delay 0 the ring read returns the sample just written**, so an untouched
  `splitter`→`merger` pair is still a bit-exact passthrough and the v23 null
  test still holds.
- **The recompute runs on the message thread from FOUR places**: every
  `publishListLocked` (structure), every accepted `setParam` and
  `setStringParam` (a param can move a latency), and `prepare()` (most
  latencies are rate-derived, so a device change moves them all). Miss the
  param path and the alignment is correct only until the first knob turn.
- **An unmatched splitter is NOT compensated.** It auto-merges inside
  `DspChain::process` itself rather than through a `MergerModule`, so there is
  no delay line there to set. Documented, not forgotten.

Evidence: `DspBranchPdcTests` reads it off an IMPULSE, which is the only
stimulus where "one arrival" and "two arrivals" are not a matter of opinion —
`tapestop` is the late branch because its header guarantees 2 samples at every
rate and a bit-exact read at that delay. The two cases are each other's A/B on
one binary: compensated gives ONE arrival at +2 of amplitude ~1.0 with nothing
at +0, uncompensated gives TWO of ~0.5 at +0 and +2, and no single behaviour
passes both. `RtBranchPdcFollowsALatencyChangeWithoutAllocating` drives a
whammy inside a live split region while a storm flips the param that moves its
latency: zero callback allocations, zero frees.

## Edit scopes: a preset is ONE publication (ABI v34)

`beginEdit()` / `commitEdit()` / `abortEdit()` on `DspChain`, behind
`dca_engine_dsp_{begin,commit,abort,get_editing}_edit_for_player`.

The problem they solve is not CPU, it is AUDIBILITY. Every mutator publishes an
ordering snapshot and takes the one-block barrier, which is exactly right for one
footswitch and wrong for a preset: a twelve-pedal board was twelve publications
plus ~thirty param writes, each individually visible to the callback, so the user
heard the board assemble itself pedal by pedal. v32's parked insertion parks ONE
node and was never enough on its own.

How it works, and the four things that are load-bearing:

- **`publishLocked()` early-returns while `editing_` is set.** Every mutator
  already funnels through it, so suppression is one line rather than an
  edit-aware copy of each mutator. The mutators that WAIT on a fade
  (`removeNode`, `moveNode`, `setNodeBranch`) check `editing_` first and take a
  pure-bookkeeping path instead — there is no published slot to fade out of.
- **A node removed inside a scope is not destroyed; it is parked in
  `editRemoved_`.** It is still in the LIVE snapshot and still being processed,
  so it has to stay alive — and `prepare()` re-prepares those slots too, or a
  device change mid-scope leaves the audible-but-shadow-removed module running
  at the old rate. A node the SAME scope added is destroyed outright instead:
  it was never published, so nothing can be holding it.
- **Commit is at most TWO publications, and one fade window.** The transitional
  one (only when the scope removed something still audible) publishes the
  pre-edit order with the departing nodes marked bypassed AND the new nodes
  appended parked at 0, so both ramps run in the same 10 ms; `waitForFades`
  then waits ONCE for the whole set, with the same heartbeat give-up as
  `fadeOutAndWait` so a chain nothing is processing costs milliseconds. The
  final publication is the shadow order itself, and `publishListLocked`'s own
  barrier is what makes the departing modules safe to destroy on return.
- **A nested begin ABORTS the stale scope and opens a fresh one** (2026-09-11;
  it returned false until then, and this bullet said "a nested begin is an
  ERROR"). It is still never a reference count — two owners editing one rig
  would have the inner commit publish the outer one's half-built state — but
  refusing did not close the abandoned scope, and `editing_` suppresses EVERY
  publication while only `~DspChain` cleared it. The rigs are value members of
  `AudioEngine`, so that destructor runs once, at engine destruction: a dropped
  `JuceDspEditScope` froze the seat's rig for the whole session, with every
  later edit accumulating silently in the shadow and every call returning
  success. `staleScopeAborts_` counts it and a Warning goes to the managed log
  through `DspChain::setLogSink`. The managed `JuceDspEditScope` finalizer is
  the first net; this is the second. Pinned by the deliberately-flipped
  assertion in `RtPresetCommitPublishesOncePerPresetNotOncePerNode`.
  A commit or abort with nothing open is still rejected, and `~DspChain`
  aborts an open scope so `dca_engine_destroy` under one is defined — it now
  WAITS OUT an in-flight commit first (bounded, `kCommitWaitMs`), because
  `abortEdit` is REFUSED while `committing_` and the destructor was therefore
  ending no transaction at all.
- **A STRUCTURE MUTATOR MAY NOT ACT ON A STALE `editing_`** (2026-09-11). Three
  methods checked the flag, DROPPED `writeMutex_` to wait for a fade, and then
  acted on that answer. `removeNode` was the CRITICAL one: a `beginEdit` landing
  in its window makes the following `publishLocked()` early-return, so the node
  was erased and DESTROYED while its raw pointer was still in the snapshot the
  audio callback reads — and `barrier()` cannot help, because the snapshot was
  never replaced. It re-checks under the final lock and falls into
  `removeNodeEditingLocked`, which keeps the module alive in `editRemoved_`.
  `clear()` always made this decision atomically; that is what showed the shape.
  Likewise `removeNodeEditingLocked`'s "never published, so nothing to barrier
  against" holds only while no commit is in flight — the TRANSITIONAL
  publication contains the scope's added nodes — so while `committing_` it
  parks rather than destroys, and rather than recycling a `NodeFade` slot a live
  `NodeRef` still points at. And `moveNode`/`setNodeBranch` no longer clobber a
  `setBypass` that arrived during their fade: `Slot::bypassSeq` is bumped by
  every `setBypass` and `restoreLiveIfUntouchedLocked` compares it, so the
  footswitch wins. Pinned by `RtRemoveCannotRaceBeginEdit` and
  `RtBypassDuringAMoveIsNotClobbered` (both drive a bare `DspChain` and spin on
  `fadeWaitSleepsForTest()` so the interleaving is deterministic, and both
  report themselves vacuous if they miss the window).
- **`commitEdit` clears `committing_` from an RAII guard**, not on the success
  path only: a throw out of its allocations left the flag set for ever and every
  later `beginEdit` blocked on a predicate that could never become true.
  `beginEdit`'s wait is bounded and logs at Warning if it has to force the valve.
- **One walk, not two.** `chainLatencySamples()` and
  `recomputeBranchPdcLocked()` are both `walkRegionsLocked()` now. They were
  hand-maintained copies that had drifted on BOTH axes — the latency getter
  walked `idOrder_` (the SHADOW inside a scope) and resolved through
  `modules_.find`, against the rule its sibling states in bold — so inside a
  scope a removed node was compensated for and not counted, and an added one was
  counted and not compensated for.
  `Wave4DspChainFixTests.ChainLatency_InsideAnEditScope_DescribesThePublishedChain`.
- **The catalogue's `[min,max]` is enforced in `DspChain::setParam`**
  (2026-09-11), for every catalogue type (100 after the gear wave), so a value read back is one the DSP is running.
  Nothing clamped before: the ABI rejects only non-finite, this function only
  NaN, and 3 of 57 adapters did it themselves — `thresholdDb = +50` on a param
  declared `[-80, 0]` was stored and read back as 50 while the core ran 0, which
  `DspModule.h` asserts cannot happen. A param the catalogue does not declare
  (the poll-only ids, the routing per-branch ids) passes through untouched.
  Swept by `Wave4DspChainFixTests.EveryCatalogueParam_ReadsBackInsideItsDeclaredRange`
  — 548 params read back out of range before the fix.
  **The per-adapter `isfinite` guards are NOT dead** (the wave-4 audit's W4-D55
  says they are): a direct `DspModule::setParam` caller passes through neither
  gate, and `tests/dsp/delay_sync_trails.cpp` is one and asserts on them.
- **`DspChain::hasParam` asks `DspModule::hasSettableParam`, not `getParam`.**
  The two are not one contract: `namamp` answers `getParam` for three poll-only
  status ids its `setParam` rejects, so the automated render's pre-pass accepted
  such an event and failed MID-RENDER — the half-swept rig the pre-pass exists
  to prevent. `hasSettableParam` defaults to `getParam`; a module whose two sets
  differ must override it.

Abort restores STRUCTURE — membership, order, bypass flags, branch tags (all
snapshotted into `preEditOrder_` / `preEditFlags_` at begin). Parameter VALUES
are deliberately not journalled: a scope is a structure transaction, not an undo
stack, and journalling every param write would put an allocation on the path a
preset load hammers.

Evidence: `RtPresetCommitPublishesOncePerPresetNotOncePerNode` runs the SAME
twelve-node preset unscoped and scoped while the callback is pumping and
compares `publishCountForTest()` (>= 12 against <= 2), plus zero callback
allocations, the nested-begin refusal, the stray-commit refusal and an aborted
scope leaving the chain it started with. `DspEditScopeTests` pins the audible
half offline: a preset built inside a scope renders bit-identically to dry until
the commit.

## `DspChain::prepare` runs under BOTH mutexes (2026-08-29)

`prepare()` took `writeMutex_`; `process()` takes `processMutex_`; they shared
nothing, so the two ran **concurrently** — and `prepare()` `assign()`s
`branchData_` and `dryData_`, i.e. FREES the blocks `process()`'s memcpys hold
raw pointers into (`branchPtrs_`/`dryPtrs_`), rewrites the `numChannels_` /
`maxBlock_` that decide whether that scratch is safe to touch at all, and
re-prepares every module underneath the block walking it. Reached from
`AudioEngine::prepareMasterBus` (device change, NAM 48 kHz request, watchdog
recovery) and from `dspProcessOffline`'s lazy re-prepare.

`prepare()` now takes `writeMutex_` **then** `processMutex_` for its whole body
— the same order `publishLocked`, `remove()` and `clear()` already take (each
publishes under `writeMutex_` then calls `barrier()`, which IS `processMutex_`),
and the audio thread never takes `writeMutex_`, so it cannot invert. The cost is
that the callback waits out a rig prepare; on the normal path there is no
callback in flight, and on the paths where there is one, that wait is the point.

Pinned by `RtRigPrepareCannotRaceTheCallback`, which drives a bare `DspChain`
(the shipping entry points can no longer be made to overlap on demand: the ABI's
offline export refuses to run with a device open, and JUCE serialises its own
device restart). Its detector is the GEOMETRY, not luck — `prepare()` alternates
a wide 2 x 1024 scratch with a narrow 1 x 64 one while the render thread keeps
asking for 1024 frames through a real split region, so the unguarded code
memcpys 8 KB into a 256-byte block. Verified non-vacuous by removing the second
lock and rebuilding: the scenario aborts (exit 3 twice, `0xC0000409` once, three
consecutive runs) where the guarded build reports 1,021 blocks against 159
prepares and passes.

## `prepare()` is where cores go wrong — seven rules

Five came out of the 0.57 pass; the last two were found on 2026-08-28 and are
the same mistake seen from its other two sides. Both are pinned by
`RtPrepareKeepsUserStateAndReDerivesFromTheRate`, which measures rather than
asserts structure.

- **`prepare()` must NOT touch USER CONTENT.** `DspChain::prepare` re-prepares
  the EXISTING module objects, so anything prepare() clears is lost on every
  device change, rate change and watchdog recovery — while the adapter's cached
  copy survives and goes on reporting it. `mikustomp`'s `prepare()` zeroed the
  three Phrase slots outright (and reset the buffer selector they had at the
  time): a device change silently deleted the user's lyrics, the pedal reverted
  to "Ahh", and `getStringParam` lied about it. Slots are zero-initialised at
  construction, which is the only time they should be.
- **`DcBlocker`'s default is a COEFFICIENT, not a frequency.** `mR = 0.995` is
  38 Hz at 48 kHz, 76 Hz at 96 kHz and 153 Hz at 192 kHz. `octaver` (on the
  sub-octave itself — a low E's sub is 41 Hz, sitting right on the old corner),
  `rotary` (on the BASS rotor, whose band is < 800 Hz by construction) and
  `phaser` all skipped `dc.prepare(sr)`; `univibe` is the one-line model that
  got it right. Measured on the 60 Hz probe: **−7.3 dB (phaser), −7.3 dB
  (rotary), −6.2 dB (octaver)** lost between 48 kHz and 192 kHz before the fix,
  under 0.02 dB after — and the fix moves the corner to the house 20 Hz at
  48 kHz too, which is about +1 dB of 60 Hz at the shipping rate.

The original five:

Every one of these SHIPPED, and every one is invisible until a device change or
the first knob write. They are all the same underlying mistake: `prepare()`
re-derives coefficients, and nothing in `dcdsp_primitives.h` clears state as a
side effect of being retuned.

- **`prepare()` must CLEAR every stateful member, not just retune it.** Every
  `OnePole::setLowpass` / `Biquad::set*` / `EnvelopeFollower::prepare` /
  `DcBlocker::prepare` / `Lfo::prepare` writes coefficients ONLY. A core that
  does not call `reset()` at the end of `prepare()` (or clear each member by
  hand) runs the OLD rate's filter, envelope and LFO memory through the NEW
  rate's coefficients. `miku` and `mikustomp` both hand-rolled a prepare that
  duplicated most of `reset()` and missed a dozen members between them.
- **Every sample count derived from the rate must be re-derived in
  `prepare()` — including the ones `setParams` owns.** `droptune`'s `maxLag_`
  and `whammy`'s `glideCoef_` were written only by `setParams`, next to a dozen
  siblings that `prepare()` did recompute, so a rate change left them at the old
  rate's value (half the requested milliseconds at twice the rate) until the
  next knob write. Store the requested MILLISECONDS and re-derive from both
  call sites. **Two more of these were found and fixed on 2026-09-11**, so do
  not read the list above as closed: `freeze`'s `lengthTgt_` and `gainStep_`
  (now `FreezeCore::recomputeRateDerived()`, called from both sites — a 48 → 96
  kHz change halved the captured loop length AND the fade time), and the same
  shape one level down in a per-SAMPLE COEFFICIENT: `feedbacker`'s peak hold was
  the bare literal `3.5e-6f` documented as "~6 s", which is true at 48 kHz only
  (1.5 s at 192 kHz), and `mikustomp`'s `shimmer`/`jitter` one-poles were the
  two that the eight-coefficient rate-derivation sweep in its `prepare()`
  missed. Rate-owned state must also be CLEARED, not only recomputed:
  `subharmonic`'s `inc1_`/`inc2_` are periods in SAMPLES that neither `reset()`
  nor `prepare()` touched, so both oscillators ran an octave off after a rate
  change until the tracker regained confidence. And **`prepare()` must apply the
  same CLAMPS as `setParams`** — `autowah`'s handed the envelope follower RAW
  attack/release times, so a device change silently reverted the clamp
  (`attackMs <= 0` makes `EnvelopeFollower::coeff` return 0, i.e. a different
  effect). All five are in
  `RtPrepareKeepsUserStateAndReDerivesFromTheRate`.
- **Never snap a smoother to its CURRENT value at prepare.**
  `setCurrentAndTarget(x.current())` reads like "don't ramp" and is not: on a
  first prepare `current()` is 0 (`SmoothedValue::reset` deliberately leaves
  current/target alone), so it DISCARDS the target `setParams` just computed and
  pins the value at zero for ever. `dcdsp_bigmuff.h` did exactly that, so a Big
  Muff added to a board and left at its defaults produced DIGITAL SILENCE —
  `preGain_` 0 through both clipping stages — until the first knob write
  re-published a target. Use `SmoothedValue::snapToTarget()`, added for this.
  The mirror-image mistake is `dcdsp_vibrato.h`'s "snap": eight `next()` calls
  against a 960-step ramp, i.e. 0.8 % of it, so every prepare/reset opened with
  a ~2.4-semitone downward pitch swoop (~8 at the maximum depth).
  **The milder form — `setTarget` in `prepare()` with no snap at all — was still
  live in EIGHT cores until 2026-09-11** (`amp`, `bitcrusher`, `distortion`,
  `overdrive`, `phaser`, `rotary`, `fuzz`, `wah`), against 26 siblings that
  already snapped. `rotary`'s was the dangerous kind: the comment above the
  `setTarget`s read "Seed smoothers at their targets so the first block is
  clean", which is exactly what `setTarget` alone does NOT do — the horn spun up
  0 → 0.83 Hz over a whole second and the drum over 2.4 s. `wah` is the one that
  bites in ordinary use rather than only at a device change: `reset()` is reached
  on every un-bypass of a PARKED node, so a manual pedal re-swept heel → position
  every time it came back in. Pinned per core in
  `RtPrepareKeepsUserStateAndReDerivesFromTheRate`, against the smoothers
  themselves (`*CurrentForTest()`) rather than a level — **and note the sequence
  trap that made an earlier version of that probe pass vacuously: a `setParams`
  BEFORE the first `prepare()` snaps by itself, because `setTarget` snaps while
  `mSteps <= 1` and `mSteps` is 1 until `reset()` derives it from a rate.**
- **A `prepare()` that REASSIGNS a filter vector must FORCE the redesign.**
  `dcdsp_widener.h` did `sideHp_.assign(pairs, OnePole{})` — and a
  default-constructed `OnePole` is a WIRE, not a high-pass — while its
  `hz != hpHz_` guard, whose cache survives `prepare()`, skipped the redesign
  for an unchanged frequency. Bass-mono was silently, permanently off from the
  first device change onward. Every sibling that caches a coefficient guard
  clears it first (`reversedelay`, `slapback`, `tapedelay` set `firstParams_`;
  `mikustomp` passes `force = true`).
- **An OVERSIZED block is SKIPPED WHOLE, never partly processed.** The rule is
  that a hot path skips a block it cannot serve rather than growing a buffer —
  but `amp`, `bigmuff`, `distortion` and `fuzz` clamped with
  `std::min(numFrames, maxBlock_)` and left the tail UNTOUCHED, i.e. full-level
  clean DI spliced onto the middle of a high-gain patch, which is the one
  outcome worse than either alternative. All four now `return` early
  (2026-09-11). The same rule one level in: a core's OUTPUT loop must walk the
  count `prepare()` gave it, not the caller's. `compressor` iterated
  `numChannels` and dereferenced unconditionally while its own detector loop 50
  lines above carefully skipped a null pointer, and `tremolo` had no channel
  clamp at all because the `numCh_` its `prepare()` wrote was never read.
  `RtOversizedBlockIsSkippedNotPartiallyProcessed` +
  `RtCoreOutputLoopsRespectThePreparedChannelCount`.
- **The cabinet FIR picks its kernel at RUNTIME, and the partitioned FFT is
  measured-and-declined, not forgotten (W4-D67, 2026-09-11).**
  `cores/dcdsp_detail_fir.h` still ships the SSE2 dot product for a
  pre-Haswell machine and now selects an AVX2 + FMA one at LOAD time when
  CPUID (plus the XGETBV check, because a kernel that does not save YMM state
  is a silent corrupter) says it can. Measured end to end: **cab 289.30 ->
  161.62 ns/sample, dualcab 333.17 -> 182.02**, a 44-45 % cut at the shipped
  2,048-tap IR. The two kernels differ by at most **9.537e-07** — FMA and a
  wider reduction tree change the ROUNDING ORDER of the sum, nothing else.
  Uniform-partitioned FFT convolution was measured too and NOT taken: the
  crossover is between **512 and 1,024 taps** at every block size the engine
  uses (below it the FFT is a LOSS — 0.51x at 256 taps and a 512-frame block),
  and at 2,048 taps it would be ~2.9x more on top of the AVX2 kernel. What
  stops it is latency and variable block size, not arithmetic: uniform
  partitioning is zero-latency only while the partition equals the CALL size,
  and `process()` is called with any numFrames <= the prepared maximum. The
  full numbers and the two dead ends are at the site; the answer that has
  neither problem is a non-uniform (Gardner) partitioning, which is a
  subsystem with its own latency contract rather than a change to a dot
  product. Pinned by `RtFirDotPicksTheWideKernelAndAgreesWithIt`, which fails
  if the dispatch silently stops selecting the wide kernel — the failure mode
  that otherwise looks exactly like success.
- **A base-rate memoryless shaper gets ADAA1 unless it is a modulator, a
  safety bound inside a feedback loop, or not a shaper at all (W4-D66).**
  `graphiceq`'s output limiter was the one site in the rig with neither
  oversampling nor ADAA on an audio path; it is `dcdsp::adaaTanh` now, worth a
  measured **-52.852 -> -61.175 dBc** worst alias product at the most the
  `level` knob allows (a full-scale 9 kHz tone, +12 dB trim; the limiter idle
  reads -109.228 as the vacuity reference). The four that stay are documented
  at their sites and the reasons are structural, not taste: `octaver`'s
  flip-flop squares have no shaper to take an antiderivative OF (polyBLEP is
  the technique that applies), `chorus`'s `shapeHyperTri` shapes a sub-audio
  LFO, and `platereverb`'s and `reversedelay`'s `softLimit`s are runaway
  bounds inside recirculating paths, where ADAA1's half-sample group delay
  retunes the loop. `RtGraphicEqLimiterAliasFloorDropped`.
  **…and the limiter adds only its NON-LINEAR part since 2026-10 (DotCharter
  #87).** ADAA1 is the mean of tanh over the step, which in tanh's linear region
  is the two-tap average (x + x1) / 2 — `|cos(pi f/sr)|`, i.e. -3 dB at 12 kHz
  and -6 dB at 16 kHz on EVERY graphic EQ at 48 kHz, whatever its bands said.
  The core now adds `kMaxOut * (adaaTanh(x) - (x + x1)/2)` to the dry sample:
  flat to Nyquist below the knee, the same anti-aliased shaping in it, and a
  hard clamp for the documented bound. Pinned by `tone_fidelity_cores` (a flat
  graphic EQ reads 0.00 dB at 8 / 12 / 16 kHz) and
  `FilterCornersAboveQuarterRateTests`.
- **`bigmuff`'s base-rate output clip is fine — do not "fix" it (checked
  2026-09-11).** Audit W4-D65 read `bassMix + trebMix == 1.15` as meaning
  `|toned| * 1.6` reaches ~1.84 and that the clipper engages at the default
  volume. It does not: the two tone-stack paths are complementary first-order
  filters of the SAME signal, so their sum at any one frequency is ~0.575·|s2|.
  MEASURED peak 0.850 with a full-scale 600 Hz tone at sustain 0.7 / volume
  **1.0**, and 0 of 14,336 samples on the ceiling. `adaaHardClip` would be the
  wrong repair anyway — it is not the identity below the threshold (it returns
  the mean over the step, a one-sample low-pass), so it changes the pedal's
  sound at every level. `RtBigMuffOutputStageStaysUnderTheBaseRateClipper` is a
  regression guard on that measurement, not a fix.
- **A buffer sized from a runtime MODE must be ALLOCATED for the worst case.**
  `dcdsp_whammy.h`'s `lowLatency` param re-runs `buildGeometry()`, which grows
  `inSize_`/`stSize_` 4× (32 ms window vs 12 ms) — against the vectors
  `prepare()` had allocated in whichever mode happened to be current. Every ring
  access masks with those members, so prepare-small-then-switch-large ran the
  writes off the end, past `capacity()` once a rate change had reallocated
  smaller, and `buildHann` reallocated on the audio thread in the same step.
  `prepare()` now sizes for the non-low-latency geometry and PINS the masks to
  the allocation.
  **`dcdsp_harmonizer.h` was the same core with the same param and a verbatim
  copy of the same false comment, and it stayed broken for a release after
  whammy was fixed** — found 2026-08-28. It now carries the identical
  `allocIn_`/`allocSt_` pin, and BOTH cores expose `ringsCoverGeometry()` so the
  gate asserts the invariant instead of trusting the comment:
  `RtPitchCoreGeometrySurvivesALowLatencyFlip` prepares each core with
  `lowLatency` already ON, flips it OFF inside a `ScopedRtThread` (which is
  where a `ParamMailbox` flip actually lands — the top of `process()`), and
  demands both in-bounds masks and zero allocations. Verified non-vacuous by
  reverting the harmonizer pin: the gate does not fail an assertion, it
  **aborts the process** (exit 3, CRT heap check) on the flip.
  **That scenario's two checks are IN-BOUNDS and ZERO-ALLOCATION, and neither
  of them is a cost bound — so the flip went on doing, inside the one callback
  that saw it, exactly what `glitch` was rewritten to stop doing** (found
  2026-09-11): a `buildHann` of up to 8,192 `std::cos`, plus a `std::fill` over
  all EIGHT `kMaxChannels`' rings — ~1.8 MB — whatever `numChannels_` actually
  was. A capacity-retaining `assign` is not an allocation and a `cos` is not
  one either, which is why "the pin holds" and "the burst is gone" were two
  different statements all along. Fixed in both cores together (the repo's own
  history here is that whammy was fixed and harmonizer stayed broken for a
  release): `prepare()` builds BOTH modes' Hann tables (`hannLarge_`,
  `hannSmall_`) and `selectHann()` swaps a `const float*`, and `reprimeSoft()`
  clears `numChannels_` channels rather than `kMaxChannels`.
  `RtPitchCoreFlipDoesNotBurstOneCallback` pins it, and its instrument is
  STRUCTURAL — two counters the cores expose, `hannBuilds()` and
  `reprimeChannelClears()` — not the cost ratio `glitch` uses. That is
  deliberate and measured: a flip block times at ~1.0× its neighbour WITH the
  burst present, because the re-prime also un-primes the core so that block
  produces no WSOLA grains at all, and the saving very nearly cancels the
  burst (ratio 1.025 broken against 0.997 fixed — a timing bound written here
  first was vacuous in both directions). The counters cannot cancel: 40 flips
  read 40 Hann builds / 320 channel clears before the fix and 0 / 80 after.

**A dry/wet mix, a level or a delay TAP applied raw from the params struct is a
click, not a knob.** `setParams` lands on a block boundary, so the value steps
between two adjacent samples: `flanger`'s `depthMs` moved the read tap 216
samples at 48 kHz across a legal 0.5 → 5 ms preset switch (a waveform splice),
and `eq`'s ±24 dB trim stepped the output 251×. Where the per-channel loop is
channel-OUTER, compute the ramp ONCE for the block (`gStart + gStep * i`, the
same shape as `MergerModule::mergeRt`) rather than holding a stateful smoother —
advanced inside the loop it would run a different part of the ramp on each
channel and tilt the stereo image while the knob moved.

**MikuCore's phrase TIMING changed on 2026-09-11, and a saved preset will sound
different because of it.** Two one-liners, both of which `MikuStompCore` had
already fixed and neither of which was visible through the ABI:
- `sylIndex_` rested at **0** while `advanceSyllable()` takes
  `idx = (prev + 1) % n` on BOTH the custom-phrase and preset paths, so the
  first pick sang `table[1]` and **every multi-syllable phrase opened on its
  SECOND syllable**. It rests at −1 now, in the member initialiser, in
  `prepare()` and in `reset()` (a reset restarts the phrase), exactly as
  mikustomp does — mikustomp's own note records the audible version of this:
  "nyan" opened on the /N/ hum, Scat on "bi" instead of "du".
- `startBurst()`'s nasal head ran `vowelSmooth_.setCurrentAndTarget(1.0f)`,
  which overwrites the TARGET `advanceSyllable()` set one line earlier — so a
  nasal mora hummed on the closed U and **never opened into its vowel at all**,
  while the trailing comment said "opens to the vowel". It now snaps the
  CURRENT value and re-arms the ramp toward the syllable's own vowel.
`RtMikuSingsItsPhraseFromTheFirstSyllable` pins both (the core exposes
`vowelPosition()` for the second). **Its stimulus gap is load-bearing and is
not arbitrary**: the onset test is `fe > 2 * feDel` against the fast envelope
20 ms earlier, and that envelope releases over 60 ms, so picks closer than
~128 ms apart leave `feDel` more than half the attack and only the FIRST one
ever fires. Measured at a 32 ms gap the sequencer never leaves syllable 0,
which reads exactly like a broken advance and is not one — do not "fix" a
future Miku probe by shortening that gap.

**A shape/topology crossfade must be RE-ENTRANT (2026-09-11).** `ringmod` and
`vibrato` both ran `setCurrentAndTarget(0)` on a second switch inside the 10 ms
window, which discards the blend in flight: the output goes from
`(1-t)·prev + t·cur` to `1.0·cur` in ONE sample — up to ~0.7 full scale, i.e.
exactly the click the crossfade exists to remove, and a footswitch bounce or a
preset A/B is the ordinary way to reach it. Flipping BACK now swaps the two ends
and keeps the blend where it is (`setCurrentAndTarget(1 - t)`); any other
destination still restarts. `phaser`'s `stages` got the same treatment when it
was given a crossfade at all — the cascade is SEQUENTIAL, so the outputs at the
old and new stage counts come from ONE pass tapped twice, which also means every
section keeps running through the fade and there is no stale state to re-inject
(6 → 2 → 6 used to replay audio from before the first change); the sections above
the landed count are cleared once the fade lands.
`DspParamSeamTests.FlippingBackInsideTheFadeWindow_KeepsTheBlend` is the pin, and
the row that discriminates is `phaser/stages` — 0.0611 against a settled slope of
0.0040 with the re-entrancy removed. The two LFO rows are the same mechanism and
pass either way on a 110 Hz sine; they are coverage, not evidence.
**Two whole CLASSES of gain are invisible to `DspGainZipperTests.MixCases`, and
`BandedMixCases` beside it exists for them (2026-09-11).** The first is a gain
inside a FILTER's band: `MixCases` drives a 110 Hz sine, which carries no energy
where a wah at position 0.8 or a formant filter at 500–3,000 Hz is working, so
those rows trip its own vacuity guard (early spans 0.00098 and 0.00036 against a
0.005 floor) whatever the core does. The second is a gain with a NON-LINEAR
mapping: `MixCases` bounds "how far along the PARAMETER is" at a fixed 128
samples, and a reciprocal gain (`1/Q`, `1/(1+0.25(q−0.7))`) moves most of its
output distance in the first sliver of knob travel — a perfect linear ramp reads
84 % (filterseq) and 67 % (envfilter) of the way at 27 % of the window.
`BandedMixCases` drives fixed-seed BROADBAND noise (a positional hash of the
absolute sample index, so the three renders stay sample-aligned) and each case
carries its OWN early window, bounding where the OUTPUT has got to.
It closed three of the six it was built for: `envfilter.resonance` (102 % of the
way with the switch hard), `formant.brightness` (90 %) and `tremolo.shape`
(63 % at 144 samples — the window has to be a few time constants of that core's
1.5 ms follower or neither side has moved).
**Three are still unpinned and each was retried against it:** `wah.resonance`
passes with the switch hard at 16 and at 48 samples (the peak gain reaches the
output through a resonant SVF whose own state dominates the early window, and
widening it walks into the ramp); `filterseq.resonance` trips the vacuity guard
at both (0.00175 / 0.00170 against a 0.002 floor); and `platereverb.predelayMs`
reports the seam 100 % away from the reference 848 samples on, because the tank
carries the old tap's phase far longer than a block. **wah and filterseq are
therefore NOT FIXED**; platereverb IS fixed and is the one place in this wave
where a fix shipped without a probe. Every one of these notes is repeated at the
harness row, so nobody re-derives them.

**Ramped 2026-09-11 (W4):** `amp`'s preamp drive (`gain`, up to +58 dB / ~790×
into the FIRST `tanh`) and its power-amp drive — both base-rate `SmoothedValue`s
pre-filled into a block buffer indexed `i / kOs` inside the oversampled,
CHANNEL-outer loop, exactly as `masterBuf_` already was; `overdrive`'s `model`
footswitch, which stepped the dry/wet blend 1.00 → 0.60 AND the shaper bias
0.06 → 0.04 (a DC step the 38 Hz `DcBlocker` turns into a thump rather than
removing) and switched in two biquads still holding state from the last time
that model was selected — those two are now `reset()` on the off→ON edge;
`platereverb`'s `predelayMs`, a plain knob with no `step` and no labels that
TELEPORTED a read tap into the TANK INPUT (20 ms `SmoothedValue`, mirroring
`dcdsp_resonator.h`); `envfilter`'s `resonance`, which sets `Q` AND the `1/Q`
output normalisation the wet is divided by — a 20 dB output multiply that sat
one field from the `mix` smoother written to fix less; `formant`'s
`brightness`, the third gain in `g_[f] = amp * bright * (1/Q)` (the other two
were already smoothed), whose CACHED coefficients meant the block-rate recompute
had to be gated on the brightness smoother as well as the vowel one or the knob
would simply stop working; `tremolo`'s `shape`, a 3-way picker whose modulation
target jumps by up to `depth` at one phase (the 1.5 ms one-pole turns that click
into a thump, it does not remove it), now the same `Lfo::peek` crossfade
vibrato and ringmod use — square is derived AFTER the LFO, so it is a third
shape; and `subharmonic`'s `waveform`, a named 3-way picker read
per sample with nothing at all damping it — square and saw differ by up to 2.0
at one phase — now a 10 ms crossfade of two calls to the same stateless
oscillator, re-entrant (flipping BACK inside the window swaps the ends and
keeps the blend rather than discarding a fade in flight).
**`platereverb.predelayMs` has NO row in either managed harness and that is
deliberate, not an oversight** — the four diffusers between the tap and the
module's only observable smear the splice until `DspParamSeamTests`' step probe
measures no contrast at all, and `DspGainZipperTests.MixCases` cannot converge
because the tank carries the old tap's phase for far longer than one
2,048-sample block (measured: the seam sits 102 % from the "always after"
reference at sample 1,200 WITH the fix and 98 % without). The note is in
`DspGainZipperTests` too; do not add a row expecting it to work.

**Ramped 2026-09-11, second pass (W4 items B):** `boost`'s `character` drive,
`slapback` and `tapedelay`'s record-path `drive` (all three normalised by a
`1/drive` make-up, so they are inaudible quiet and a level step once the stage
runs hot — and all three displace an ADAA state's operating point in one sample
either way), and the two delay MODULATION DEPTHS that are ADDED to a read tap:
`tapedelay`'s `wowFlutter` (144 samples, 3 ms at 48 kHz) and `slapback`'s
`wobble` (17). `timeMs` in both cores got a 50 ms glide for exactly that reason
and the depth added to it did not.
**`delay`'s `pingPong` is now a 10 ms crossfade of the two feedback SOURCES**
(the two `reads_`, not two shaped outputs — the tone filter and DC blocker are
per-channel state and running them twice would double their contribution
mid-fade). It was a full-amplitude splice inside a loop running at up to 0.95,
and the repo's loop-gain exemption does not cover it: a routing switch is not a
level. **Its probe is hand-written and two things about it are load-bearing** —
the stimulus must be STEREO (this theory feeds both channels the same signal,
which makes cross-feed and self-feed bit-identical: measured RMS difference
0.00000), and the horizon must be FIVE blocks, because the output reads its own
line whatever `pingPong` says, so the splice is written at the toggle and first
heard one delay period — 150 ms — later. A 600-sample window sees nothing and
passes with the switch hard.

**`acousticsim` rebuilds only what MOVED (W4-D21, 2026-09-11).** `setParams`
ran `updateCoeffs()` unconditionally and it is drained on the audio callback, so
ONE `mix` tick cost two passes over up to 4,096 IR taps plus five biquad designs
per channel, each with a `cos`/`sin`/`pow`. Allocation-free and deterministic,
which is exactly why the RT gate never saw it — `dcdsp_cab.h` already guarded the
equivalent. It is now `updateKernel()` (depends on `body` + the custom IR) and
`updateVoicing()` (depends on `brightness` + `lowCutHz`), each gated on its OWN
inputs. `RtAcousticSimMixDoesNotRebuildTheIr` counts both (64 mix ticks cost 64
of each before the guard, 0 after) **and asserts the guard is not a MUTE** — a
`body` move must still rebuild the kernel and not the voicing, and vice versa,
or "no rebuild" would also be true of a core that had stopped listening.

**`glitch`'s repeat decay ramps across the loop wrap (W4-D8).** The core goes to
real trouble to make the WAVEFORM continuous there — `readSlice`'s
`p + loopLen_` bridge exists for nothing else — and then multiplied both sides
of that splice by a gain that jumped 40 % at the same sample, so there was a
click every lap at `decay > 0`. The gain now walks across the same `xfade_`
region the waveform is bridged over, and `repeatGainNow()` is called ONCE per
frame above the channel loop (per channel it would walk the ramp C times too
fast). Its probe's reference is the SAME loop at `decay = 0`, where only the
bridge is at work: 0.0114 against 0.0036 with the gain stepped.

**`chorus`'s `voices` is a CROSSFADE of the voice COUNT (W4-D3, 2026-09-11),
and the fix has a second half nobody would guess.** 1 → 2 stepped the wet
normalisation 1.0 → 0.5 — a 6 dB cut in one sample — AND introduced a new read
tap at a different base delay at full weight, while every other knob in that
core, `lfoShape` included, was already smoothed. `process()` now runs
`max(prev, cur)` voices, weights the ones only ONE count has by a 30 ms fade,
and derives the normalisation from the FADED count so the wet level never moves.
**The second half: the LFO phases advance for all `kMaxVoices`, not for the
voices currently summed.** A voice that is not summed still has to keep its
phase — the voices are deliberately fanned apart in phase, and that fan is the
whole reason a second voice thickens rather than flanges. Frozen, it comes back
holding whatever value it had when it was last used, which also makes the voice
count HISTORY-DEPENDENT and is what stopped any before/after probe converging
(the row read "121 % away" until the phases were freed). Three adds per sample;
the summed voices' sequence is untouched.

Ramped in 0.57:
`flanger` depth/mix/feedback, `delay` mix/feedback, `eq` trim, `graphiceq`
level. Later passes also ramped sustainer output, octaver/subharmonic levels,
reverb/plate/spring/shimmer/tape/wah/cab/acoustic mixes and harmonic tremolo
depth. `SmoothedValue::blockStart` intentionally stretches the final ramp
segment to the block boundary; it is stereo-consistent but not sample-exact
across block sizes (10 ms becomes 21.33 ms for a 1,024-frame block at 48 kHz).


**A two-deep buffer flip is ABA-unsafe, and both Miku cores shipped one
(fixed 2026-08-28).** `miku` and `mikustomp` published a user phrase table by
writing `1 - sel` and storing the new `sel`. The audio thread resolves `sel` and
then reads out of that buffer LATER — `mikustomp` at the next note, `miku` for a
whole block — so the SECOND `setPhrase` flips straight back to the buffer the
reader is holding and rewrites it under them. `Syllable` is two ints, and both
fields index a coefficient table (`kCons`, `kVowF`), so the torn case is an
out-of-bounds read, not merely a wrong sound. **Two buffers only ever move a
race; they do not close one.** The replacement is the repo's own pattern:
a per-slot SEQLOCK (`ParamMailbox` in `DspModule.h`) over syllables stored as
packed lock-free `atomic<uint32_t>` — `MikuStompCore::nextUserSyllable` /
`MikuCore::nextCustomSyllable` take `(count, syllable)` between two equal EVEN
reads of the sequence and give up after `kPhraseReadTries` rather than spinning
(the audio thread sings the current mora one note longer). Nothing is allocated
and nothing is freed on either side, so there is no retirement to get wrong,
and the phrase table is now resolved AT THE NOTE instead of hoisted to the
block top. Pinned by `RtMikuPhraseSwapIsCoherentUnderEdits`: phase A hammers the
audio thread's own reader against a writer alternating two disjoint phrases
(measured 0.1–10 M coherent reads per run, **33,727 torn** the moment the
packing is removed — the non-vacuous check), phase B drives real plucks through
the core while the same storm runs and demands zero callback allocations AND
zero callback frees.

**IR adoption and ordinary params use atomic snapshots.** `ParamMailbox` now
stores packed atomic words, not a plain struct behind a sequence counter (the
latter was still a C++ data race). Payload and 64-bit sequence operations are
sequentially consistent; the audio thread makes one bounded attempt and keeps
its previous value if publication overlaps. Writers remain serialized by the
chain's owner mutex. No retry spin, allocation, or retirement happens here.

`CabModule`/`AcousticSimModule` parse the WAV on the owner thread and publish a
fixed-capacity `IrMailbox` payload (at most 4,096 taps). Only `process()`
adopts/clears it and rebuilds the core kernel. `prepare()` reserves custom-IR
and resampling storage, so adoption cannot allocate.

**THE RATE CONVERSION HAPPENS IN THE PUBLISHER, NOT AT ADOPTION** (2026-09-10).
Both kernel sources went through `detail::resampleIr` inside the callback that
drained the mailbox — up to 4,096 output taps, six Lanczos taps each, two
`std::sin` per tap, plus three biquad redesigns per channel, in ONE block. It
allocates nothing, so the RT gate could not see it, and the conversion
short-circuits at 48 kHz (`|dRate| < 1 Hz`), so the owner's own machine could
not either; on a 44.1 kHz device every model tick and every preset load paid it.
Now: `IrMailbox::setEngineRate` (called from the module's `prepare()`) converts
a user IR on the publishing thread, and `dcrig::ModelIrMailbox` does the same
for a BUNDLED model, which is the commoner of the two. `CabCore` holds both
sources AT THE ENGINE RATE, so `rebuildIr()` is a bounded reverse copy and
`adoptModelKernel()` is a copy plus one release store. `CabCore::setParams`
keeps an owner-thread conversion fallback for DIRECT callers
(`tools/tone_response.cpp`, the native tone tests) — in the engine it is dead,
because the model mailbox is drained BEFORE the param mailbox, and
`conversionsForTest()` is how the tests prove that. Slot B carries no bundled
bank at all (`CabCore::useBundledBank(false)` from `DualCabCore`'s ctor): it is
audible only with a user IR, and one `CabParams` is forwarded to both slots, so
it used to re-convert a 2,048-tap table for a kernel nothing would ever read.
Pinned by `RtCabKernelSwitchDoesNotBurstOneCallback` (8.33x -> 1.37x) and the
`dualcab` CTest's `modelConversionsHappenOffTheCallback` /
`prepareAtEachRateMatchesTheMailboxKernel`. The core kernel
slots are now audio-owned; they are not a cross-thread lifetime mechanism.
`setCustomIr`/`clearCustomIr` must never be called concurrently with processing.
The standalone `tone_regressions` CTest exercises the real factory adapters,
coherent concurrent parameter/IR snapshots and debug-CRT allocation hooks.

**Rate changes preserve the original 48 kHz DC response.** Overdrive,
distortion, tape delay, spring/plate/shimmer reverb, acoustic sim and slapback
used the shared DC blocker's fixed `.995f` coefficient at every sample rate.
Its corner doubled at 96 kHz; feedback paths compounded the bass loss on each
repeat. They now call `prepareLegacy48k(sr)` in preparation: exactly `.995f` at
48 kHz, the same decay constant at other rates. Do not replace this with the
primitive's default 20 Hz `prepare()` unless deliberately changing the voicing.
`tone_regressions` measures all eight at 60 Hz across 48/96 kHz and back; the
acoustic case uses body=0 to isolate DC response from body-IR resampling.

## A param may not STEP a modulator, a delay tap, or one callback's workload

Three cores did, and none of it was a level — which is why the gain rule and the
allocation gate both looked straight past them (2026-08-29, the 0.48 audit).
Pinned by `DspParamSeamTests` (managed, offline: the largest sample-to-sample
step over the 12.5 ms after the change, against the same signal's own settled
slope) and by the RT gate's `RtGlitchCaptureDoesNotBurstOneCallback`.

- **A WAVEFORM switch is a full-scale discontinuity, so it is crossfaded.**
  `Lfo::setShape` swaps the shape under a running oscillator and the phase
  carries on: at one phase sine and triangle differ by up to 1.0 (phase 0 is
  0 against -1). In `ringmod` that stepped the carrier, i.e. a click in the wet
  at mix 1; in `vibrato` the LFO **is** the delay, so the read head teleported by
  the whole depth — 576 samples, 12 ms, at the maximum — mid-note. Measured
  against the old code: a 0.3139 step where the signal's own slope is 0.0041,
  seventy-six times. `Lfo` gained `shapeAt(shape, phase)` / `peek(shape)` /
  `advance()` (stateless, additive, `process()` is now written in terms of them)
  so a core can read BOTH shapes off one phase and crossfade over 10 ms; the two
  cores keep `shapeCur_`/`shapePrev_` and a `SmoothedValue`, and pay the second
  evaluation only while `isRamping()`. A new LFO core that lets the user change
  shape owes the same three members.
- **The tap of a LOADED delay line ramps.** `resonator` re-solved `delay_[]` in
  `retune()` and the callback read the new value on the next sample. Every one of
  key, chord, octave and spread does that, and `spread` is a knob the user DRAGS
  — but the real damage is the feedback: a Karplus-Strong loop running at up to
  0.998 does not decay a splice, it re-injects it, so one step rang for up to six
  seconds. The taps are now per-channel `SmoothedValue`s over 20 ms (a short
  pitch glide) and a voice the chord just switched ON snaps instead, because its
  line holds nothing but zeros. `voiceCount_`'s own `1/sqrt(n)` normalisation is
  still a step; a chord change is a discrete event and it has not been reported.
- **A capture is drained incrementally, never copied in one callback.**
  `glitch` copied the whole slice — up to 1004 ms per channel, 1.5 MB at 192 kHz
  stereo — inside the callback that saw the trigger. Allocation-free and
  deterministic, which is exactly why nothing caught it: the RT gate's catalogue
  sweep drives every module at its DEFAULTS, where glitch's mode is Manual and
  the trigger is off, so the capture path never ran. `dcdsp_freeze.h` had already
  solved this; glitch now uses the same shape (history ring at 2x the longest
  capture and power-of-two masked, an absolute write counter, `kCopyRate = 2`
  samples copied per output sample, `readCaptured` serving reads out of the
  still-recording ring until the copy catches up). Measured on the debug-CRT
  gate binary: the trigger callback cost **759x** its neighbours before,
  1.13x after.
  **`whammy` and `harmonizer` were the same shape and were not swept** until
  2026-09-11 — a `lowLatency` or `formant` flip rebuilt the Hann window and
  memset every channel's rings on the callback. See the worst-case-allocation
  bullet above for the fix and for why that one is pinned by counters rather
  than by a cost ratio.

## Idle sleep: the live rig skips itself while nothing is heard (2026-09-24)

A restored rig runs on every DEVICE block from engine start, on every screen,
whatever the input. The owner's gate -> two NAM captures -> cab -> eq -> gain
measured ~13.5 % of one core at 128 frames / 48 kHz with a -70 dBFS hum in, the
same as a played note. Idle sleep takes that to ~0 while the guitar rests, and
does not change the sound. Code: `src/dsp/IdleSleep.h` (the decision, the
allow-list, the constants — header-only, no JUCE), `AudioEngine::processDspRigLive`
+ `rigIdleEndBlock` (the device callback), `DspChain::rtEditGeneration` /
`rtIdleHoldSamples` / `rtMayRest` / `noteExternalProcess`.

- **Where.** Only the device callback's seat loop calls `processDspRigLive`.
  The offline renders (`dspProcessOffline`, `dspProcessOfflineAutomated`) call
  `DspChain::process` directly and never sleep: a bounce is exact. `DspChain`
  itself never skips anything.
- **Falls asleep** when what the seat's rig contributes to the device output —
  the post-EQ peak times the louder end of the monitor ramp, never counted
  below unity so the post-chain meter stays honest with monitoring off — has
  stayed below **-90 dBFS** (`idle::kQuietPeak`) for the chain's **hold**, AND
  the rig's input stayed below **-40 dBFS** (`kMaxIdleInputPeak`: louder is a
  performance, whatever the rig outputs), AND the hold's second half was no
  quieter than its first by more than **sqrt(2)** (the input is a STATIONARY
  floor, not the tail of a note the rig muted — otherwise the window slides).
- **The hold** is `max(5 s, the longest settle any published node declares)`,
  in samples at the prepared rate. **Five seconds is measured, not guessed**:
  slow states (a DC blocker, an amp's sag follower) are still decaying at 2 s
  and a high-gain stage resurfaces the residue at the next pick — the in-house
  owner-like chain differed from a never-slept one by -66 dBFS after a 2 s hold,
  -79 dBFS after 3 s, and was bit-exact after 5-6 s of digital silence (FTZ
  makes a decaying state reach exact zero, so a long enough hold makes the
  frozen state the SAME state). Per-type settles: a gate's `hold + 10 x release`
  (its gain heads for a -120 dB floor); a compressor's or limiter's
  `40 x (release [+ attack])` — its gain heads for UNITY, a residue is a
  relative error on the whole next note, and 40 tau is where a double reaches
  1.0 exactly (compressor -> rat measured -60 dBFS at 10 tau).
- **While asleep** the rig is skipped and its output channels are cleared —
  what it was outputting to within -90 dBFS. Downstream stages still run.
- **Wakes on the block whose input peak exceeds sqrt(2) x the idle floor**
  (`kWakeRatio`; the floor never below 2^-24, below any 24-bit converter), and
  that block IS processed, so no attack is lost. sqrt(2), not the 2x first
  proposed: the only input a sleeping rig can miss is one between the floor and
  that ratio, and halving the band costs nothing (a 6.2-sigma noise sample is a
  ~3e-10 event). **Also wakes — and restarts the hold — on any change of**
  `RigIdleContext`: the chain's edit generation (every publication, every
  param / string-param write, `prepare`, the branch-PDC switch, the meters, an
  offline pass), the input-VST generation, the input trim, the seat's monitor
  gain, the input EQ's trim and route, the capture channel, the seat count, the
  sandbox bridge — and on an EQ retune (`forceAwake`). `prepareMasterBus`
  resets every seat to awake.
- **Never sleeps** while: a node's type is not on the allow-list; the per-node
  meters are on (a sleeping rig would freeze them); a bypass / move / insert
  crossfade was ramping in the last block (the ramp only advances in
  `process()`); a node is keyed from the SONG mix (the song can open a keyed
  stage with no guitar); an input VST or the sandbox bridge ran on the rig's
  output (a third-party stage it cannot reason about).
- **The allow-list (`kIdleSleepTypes`) is finite-memory types only**: gain,
  volume, eq, graphiceq, tilt, midside, widener, splitter, merger, cab, micpos,
  acousticsim, the drives / fuzzes / amps (overdrive, distortion, fuzz, boost,
  klon, bigmuff, rat, ds1, hm2, tapesat, bassod, octfuzz, amp, acpre,
  poweramp), **namamp**, gate, compressor, limiter, transient, sustainer. After
  a hold of idle input such a module's whole state is a function of that idle
  input. **Left off, with the measurement that put them there**: long or
  infinite memory — delays, every reverb, resonant filters (svf, comb,
  resonator, wah, envfilter, autowah), feedbacker — each passes ALONE, but its
  frozen tail sits below -90 dBFS at its own output and a stage after it can
  reveal it (hallreverb -> two drives: -57 dBFS; convreverb -> amp: -79 dBFS;
  a delay in front of a closed gate replays its last echoes into the next
  pick). Free-running LFOs and modulated tanks (a sleep freezes the phase),
  the pitch shifters, the capture devices (looper, freeze, glitch, granular,
  the reverse windows, tape stop), and the expander (-67 dBFS through two
  fuzzes even after a 6.6 s settle), optocomp, multiband, ducker,
  noisereducer, swell, the vocoders. **A type added to the list must pass
  `juce_sharp_rig_idle_sweep` alone and in its random chains** — and the list
  failing CLOSED is deliberate: an unmeasured module keeps the seat awake.
- **Why resuming is the same sound.** The chain's state only advances inside
  `process()`, so a skipped block is exactly "the idle stretch was cut out of
  this seat's input", and the waking block continues from where the last awake
  block left every module. After the hold, that state is a function of the idle
  input alone. On digital silence it is the state a never-slept rig is in, bit
  for bit (the rtcheck scenario asserts `==`; the sweep measured it for every
  allow-listed type). On a real
  idle (hum, hiss, a filter's last-bit limit cycle) it is the state a
  never-slept rig would be in had the pick landed a moment later — and a
  never-slept rig already moves the note by that much from pick to pick. A
  RESET would be the wrong answer: it hands the pick a cold state (an empty NAM
  history, a gate at its initial envelope) that no never-slept rig is ever in.
- **Evidence** (all native; nothing crosses the ABI):
  - `juce_sharp_rig_idle_sweep` (`tests/dsp/rig_idle_sweep.cpp`, ctest
    `rig_idle_sweep`, ~15 min): every allow-listed type at defaults / all-max /
    all-min / two random settings, idle on digital silence and on -70 dBFS hum
    + -90 dBFS hiss, plus 48 random chains of allow-listed types and an
    owner-like in-house chain. Three chains per run: A never sleeps, B runs the
    shipping `RigIdleSleep`, C never sleeps and idles 3 blocks longer. Fails
    when B never slept on silence (vacuous), when A rose above -90 dBFS while B
    slept, when the waking block was skipped, or when B differs from A by more
    than `wakeAcceptable` allows (-80 dBFS peak; or 100 dB below the note's
    energy; or no more than C, +6 dB). `--discover` prints the whole
    catalogue's numbers (the allow-list was chosen from them); `--chain
    type:variant,...` and `--min-hold <s>` are debugging aids.
  - `RtRigIdleSleepSkipsSilenceAndWakesOnThePick` (the RT gate): the real
    callback, allocation sentinel ARMED through sleep, skip and wake. Asleep
    after the hold and not before; exact zeros while asleep; the waking block
    processed and the output from it on IDENTICAL (`==`) to a never-slept
    engine; a param write and a monitor move wake it on the next block and
    restart the full hold; a non-allow-listed node (tremolo) keeps it awake;
    an offline render of a sleeping seat equals a never-slept one's and does not
    advance the device path. **The RT gate's `Fixture` switches idle sleep OFF
    for every other scenario** (`AudioEngine::setRigIdleSleepForTest`), because
    many pump silence longer than the hold and would silently stop exercising
    the rig they check.
  - `juce_sharp_rig_idle_nam` (`tests/nam/rig_idle_nam.cpp`, ctest
    `rig_idle_nam` with NeuralAudio's BossLSTM capture): the one test with a
    real neural capture — a shipping-configuration build (release CRT) of the
    engine on the manual null device. Determinism first (two never-slept
    engines must agree bit for bit), then slept vs never-slept vs the
    pick-later yardstick. `--bench` times the callback per block.
  - Proven non-vacuous by breaking each half on purpose: a 2 s hold fails the
    RT scenario's exact-equality (-78 dBFS); removing the param write's
    generation bump fails "a param write did not wake"; removing the offline
    pass's `noteExternalProcess` fails the wake comparison (0.41 of full scale);
    skipping the waking block fails it (0.995).
- **Measured** (2026-09-24, 128 frames at 48 kHz = 2,667 us a block, the owner's
  gate -> precision-drive -> mesa-markiv -> cab -> eq -> gain): the device
  callback idling on -70 dBFS hum + hiss costs **~280-320 us (10.4-12 % of a
  core) awake and 8-11 us (0.3-0.4 %) asleep**; on digital silence 4-6 us —
  the same as the engine with no rig at all (7-10 us). Playing is unchanged
  (the bookkeeping is a peak scan and a POD compare; run-to-run noise between
  two engines in one process is +/-15 %, which is why `--bench` alternates
  OFF / ON twice). The managed probe (`LocalOnly_AudioIdleRigProbe`, paced
  null device, the engine's own callback timer, silence in) read 285 us before
  and 7.8 us after.
- **NeuralAudio is compiled with `/arch:AVX2` since the same day**
  (`JUCESHARP_NEURAL_AVX2`, default ON, MSVC only; NeuralAudio + RTNeural
  PRIVATE, the rest of the DLL stays SSE2). The same rig while PLAYING, offline
  best-of-3: **276 -> 176 us (10.3 % -> 6.6 %)** — mesa-markiv 188 -> 109 us,
  precision-drive 67 -> 41 us; through the callback 290-330 -> 190-225 us.
  **The price is a CPU floor**: NeuralAudio's static registrations run when the
  DLL loads, so a CPU without AVX2 (pre-Haswell Intel, pre-Excavator AMD, the
  Atom-class Pentium / Celeron Silver line) must be assumed to fail at LOAD,
  not only when a capture plays. `-DJUCESHARP_NEURAL_AVX2=OFF` is the escape;
  a runtime-dispatched build (NeuralAudio compiled twice) is the real fix if
  that floor ever matters.

## The offline tooling path is not the device callback

`dca_engine_dsp_process_buffer{,_for_player}` → `AudioEngine::dspProcessOffline`
inherits NONE of the callback's guarantees:

- It **never idle-sleeps** (see the section above) — it calls
  `DspChain::process` directly — and on every way out it calls
  `DspChain::noteExternalProcess`, so a seat asleep on the device path wakes
  instead of resuming from a state it never saw being made.

- It establishes its **own `juce::ScopedNoDenormals`**. Without it a reverb or
  delay tail decaying into subnormals costs ~100× per sample for the rest of the
  bounce; the live path only gets FTZ/DAZ because `EngineCallback` sets it.
  **That applies to the OTHER offline export too** —
  `dca_droptune_process_offline` shipped without one until 0.48, and CSVD's
  ring, detector state and crossfade tails all decay toward subnormals between
  segments. `RtDropTuneOfflineIsAllocationFree` now drives the shipping export
  with a normal burst followed by a subnormal tail and counts the subnormals
  that come back: it is not observable any other way (MXCSR is restored before
  the export returns). Measured **46,525 of 52,096 output samples subnormal**
  with the line removed, 0 with it.
- It **re-prepares the rig when the requested rate differs from the prepared
  one — but only when no device is open.** A rig prepared at 48 kHz and bounced
  at 44.1 kHz ran every corner 8.8 % sharp and every delay 8.8 % short,
  silently, for the whole render. The take re-amp drives a dedicated device-less
  engine, so re-preparing there is free and correct; re-preparing under a LIVE
  callback would instead retune the player's tone mid-song, so when a device is
  open the caller has to match its rate.

### Transparent overdrive dry alignment

Overdrive model 3 blends 40% clean with a 2x-oversampled ADAA wet path. The
clean copy formerly had no delay: its moving cancellation notch measured
-22.92 dB at 4 kHz/44.1 kHz and -17.07 dB at 4 kHz/48 kHz at low drive.
Only this model now reads a fractional dry delay matching the oversampler's
low-frequency group delay plus the ADAA half-sample at the 2x rate. It reports
that rounded compensation latency through owner-thread adapter metadata: cached
preparation geometry selected by `p_.model`, never a read of callback-owned
`blend_`. The latency tests query before callback adoption in both directions. This is
not frequency-independent phase correction of the voicing filters. Models 0-2
keep their original signal path and zero reported latency; the delay history
stays warm during model changes. `tone_regressions` covers the 2-6 kHz sweep at
44.1/48/96 kHz, stereo equality, block partition continuity, adapter latency,
and pre-change output references for the other three models.


## Lane mixer switches (2026-09-05)

`merger` appends `mute1..3` and `solo1..3` (0/1, default off) after its
existing level/pan/mode parameters. These are authored state, persisted through
the generic catalogue/snapshot path; never implement mute by overwriting level
or bypassing branch effects. `MergerModule::mergeRt` combines these switches
with the existing gain ramp. Mute wins over solo; multiple soloed branches mix;
a solo on disabled branch 3 cannot silence a two-branch region. Average-mode
normalization keeps the original enabled-branch count when muting/soloing.
No ABI layout/export change; old engines advertise no switches and consumers
must disable them. The pure `lane_mixer_test.cpp` CTest target verifies audio
output and level/state preservation without opening an engine.


## Dual cabinet IRs (2026-09-05)

The existing `cab` keeps `irPath` as slot A (empty restores its bundled `model`)
and appends `irPathB` (empty means absent). `DualCabCore` composes two existing
`CabCore` FIR/resonance/cut paths through `processWet`, then applies slot gain,
polarity, alignment, linear A/B blend, the shared dry/wet mix and output trim.
An absent B ignores blend and uses full A; clearing B at blend=1 cannot silence
the cab. No microphone-position/quality simulation is advertised.

Appended numeric parameters: `blend` 0..1 (default A=0), `gainADb`/`gainBDb`
-24..24 (0), `invertA`/`invertB` 0/1 (normal=0), `delayAMs`/`delayBMs` 0..20 (0),
`outputDb` -24..24 (0). Existing `model`, `mix`, `lowCutHz`, `highCutHz` keep
identity, range and defaults. Both polarity settings have real catalogue labels.
The existing numeric/string snapshot path persists every setting; no ABI export
or preset schema changes. Older engines expose only their original parameters.

Two independent `IrMailbox` instances keep file I/O on the serialized owner side,
retain the last accepted file on load failure and coalesce pending updates before
audio adoption. Each IR stays capped at 4096 taps; the mailbox resolves an
IR/device sample-rate mismatch on the OWNER thread (see above). Prepare sizes all storage: at most 8 channels,
65536 frames of scratch per channel, and 20 ms alignment rings up to 384 kHz.
Oversized processing blocks are chunked; no callback allocation or file I/O.
The unused B convolution is skipped. Gain/polarity/blend/output/alignment use 10 ms
ramps; manual alignment is a wet-path effect, not fixed host-reported latency.

CTest `dualcab` exercises actual file adapters/latest-wins adoption, independent
clear/failure behavior, stereo isolation, gains/polarity/blend/alignment,
mixed-rate re-prepare, callback allocation and sample-exact legacy compatibility
at 44.1/48/96 kHz. No device or JUCE startup is required.
## Delay tempo sync and bypass trails (2026-09-05)

`delay` appends four persisted float parameters; old five-parameter presets retain
bit-identical sound with both opt-ins off:

- `sync`: 0 Off / 1 On, default 0. Manual `timeMs` remains saved and is used when off.
- `bpm`: 20..400, default 120; this is the node's explicit tempo, not an implicit
  global transport clock. The host must publish tempo changes if it wants follow mode.
- `division`: 0..8, default 2; labels in stable order: 1/1, 1/2, 1/4,
  1/4 Dotted, 1/4 Triplet, 1/8, 1/8 Dotted, 1/8 Triplet, 1/16.
- `trails`: 0 Off / 1 On, default 0. Sync derives time as 60000/BPM times
  quarter-note beats, clamped to 10..2000ms; the existing 50ms tap glide remains.

`DspModule::ownsBypassProcessing` opts the delay into `processBypass`, using the
chain's existing 10ms weight exactly once. Trails ramp new input into the delay
line to zero, return fresh dry input to unity, and retain wet feedback history.
Turning trails off smooths its contribution over 10ms; enabling it after hard
bypass clears frozen history. No new buffers, allocations or locks occur in the
callback; `ParamMailbox` carries parameters and an atomic reset request prevents
message-thread clears of a draining module. Other modules use the original path.
The core remains channel/sample bounded; oversized blocks retain the chain's
scratch-free snap fallback. Reprepare clears all history.

`juce_sharp_delay_sync_trails` / CTest `delay_sync_trails` exercises the actual
factory and chain offline: tempo signal timing, metadata round trips, legacy
bit-exact comparison against frozen e8286ab at three rates, dry/wet transitions,
feedback decay, rapid toggles, hard reset, oversized blocks and debug-CRT
allocation checks. The one-sample read-before-write delay convention is unchanged.

## Spillover tails (ABI v37)

`DspChain` keeps a removed audible `delay`/`reverb` (catalogue CATEGORY, so a
new module in either family spills with no list to update) ringing as a
post-chain tail — see `native-engine-abi.md` § ABI v37 for the mechanism. What
matters when you touch the chain: a spill node is in the PUBLISHED order but not
in `idOrder_`/`publishedOrder_` (`NodeRef::spill`), so every walk over the
snapshot must skip it (`process`, `recomputeBranchPdcRt`) and no message-side
walk sees it; a `spilling` node is still in place and is processed through
`spillHandOverRt`, never `processBypass`. A tail module is never re-prepared:
`prepare()` ends it.
