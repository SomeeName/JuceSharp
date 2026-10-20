# The chart voice (built-in guitar and bass)

Deep reference for `JuceSharp/AGENTS.md`. The guitar and bass that play a
chart's notes as a DI signal through a seat's rig (DotCharter issue #39 step 1
+ bass). Plain C++17, no JUCE: `src/JuceSharp.Native/src/dsp/voice/`, reached
from the chart source through `src/synth/GuitarChartVoice.h`
(`DCA_CHART_VOICE_GUITAR` / `DCA_CHART_VOICE_BASS`).

**It plays REAL RECORDINGS.** Eight sample packs sit in `voices/` beside
`juce_sharp.dll` (218 MB on disk, 594 MB of int16 audio once loaded; measured
2026-10-07): `guitar.dcvp` (CC0 Karoryfer, a Gretsch recorded on its bridge
pickup, 73 MB / 143 MB of audio), `bass.dcvp` (CC0
Karoryfer, fingers + pick), ONE PACK PER STRATOCASTER PICKUP POSITION
(`strat-neck`, `-neck-middle`, `-middle`, `-middle-bridge`, `-bridge`: EGFxSet,
CC BY 4.0, every string x frets 0..22, 18-21 / ~57 MB each) and `guitar-arts.dcvp` (#52:
Guitar-TECHS player 1, CC BY 4.0, 9 / 33 MB — real palm mutes, natural and pinch
harmonics that EVERY guitar model loads beside its own pack and converts to its
pickup). They are NOT in git:
`tools/voice/build_voice_pack.py` builds them from a committed manifest
(`tools/voice/voice_pack_manifest.json`: pinned commit or archive, URL and SHA-256
per source file). Without a pack the voice falls back (a Strat position to the
Gretsch set converted, anything else to a waveguide string model), so the source
is never silent.

**INSTRUMENT MODELS (ABI v40, DotCharter #50).** A voice is built for one
`InstrumentConfig` — model, pickup selector, coil split (`dcvoice_models.h`) —
and loads ONLY the pack that selection is converted from. Recorded as is: the
Strat's five positions (also the HSS's neck / neck+middle / middle), the Gretsch
on its bridge, the Jazz bass's pick on both pickups. Everything else (Tele, Les
Paul, the HSS humbucker and every split, the other Gretsch and bass positions)
is the closest recording CONVERTED per note: the source pickup's comb divided
out, the target's applied, the resonance swapped, a body EQ (`dcvoice_pickup.h`).
`tools/voice/calibrate_pickups.py` fits and checks that maths against EGFxSet
(8.18 → 5.37 dB per harmonic predicting one position from another) and measured
where the Karoryfer sets were recorded.

## Files

| File | What |
|---|---|
| `dcvoice_pack.h` | the `.dcvp` format (version 2 = coded zones, version 1 = raw int16, anything else REJECTED), `parsePack` (validates every offset — it is a file on a user's disk — and decodes a v2 pack's zones on a few worker threads it joins), `loadPackFile`; the LAZY pack (#417): `loadPackLazy` / `parsePackLazy` (the zone table only), `ZoneStore` (per-zone atomic sample pointers), `ensureZones` (decode a want-list, control thread), `serviceZoneRequests` (what the audio thread asked for), `markGroup` / `markArt`; `selectZone` (nearest pitch group, then layer, then round robin; allocation-free; on a lazy pack it asks for a missing group and plays the nearest decoded one) |
| `dcvoice_codec.h` | the LOSSLESS zone codec of a v2 pack (#408): per 4096-sample block an LPC predictor, per 256-sample partition a Rice parameter; `decodeZone` is bounds-checked, rejects any stream that does not end in its own last byte, and reproduces the builder's int16 samples exactly. Stream layout: the builder's "ZONE CODEC" notes |
| `dcvoice_pack_store.h` | where packs live (`voices/` next to the module, or `DOTCHARTER_VOICE_DIR`) and `acquirePack` — loaded once per process on the CONTROL thread, shared by `shared_ptr`, freed with the last voice; `PackLoad::Eager` (tools, tests) / `Lazy` / `LazyFresh` (the engine, #417); `livePackBytes` (the RAM held — a lazy pack's DECODED zones — `dca_chart_voice_pack_bytes`) |
| `dcvoice_models.h` | the INSTRUMENT MODEL table (#50): models, pickup selectors, coil split, where each pack was recorded (`sourceFor`), the fitted Strat geometry, defaults (`defaultConfig` = HSS bridge / Jazz both; `legacyConfig` = the Gretsch as recorded) |
| `dcvoice_pickup.h` | the per-note CONVERSION: `ConversionPlan` (per pack variant, coefficients at prepare), `NoteComb` (the note's comb geometry + loudness normalisation), `makeConversionPlan` (IDENTITY when the selection is what the pack recorded) |
| `dcvoice_sampler.h` | `SampleInstrument`: the sample player. Six players per string lane (note, cross-fades, release noise, slap thumb / fret clank), Hermite resampling, per-player biquad, transient brightness and one-sided clip (slap / pop), envelopes; the optional ARTICULATION pack (`arts_`, its own `ConversionPlan`); the RECORDED SLAPS (`Art::SlapNote`, their own `slapPlan_`, #109); `SlapPopShapes` (`slapRecorded`, the pop model and the slap model a pack without slaps falls back to); `vibrato()` (closed form, humanised) |
| `dcvoice_instrument.h` | `NoteStart`, `IVoiceCore` (the event API both cores implement), `InstrumentSpec`, and `StringInstrument` — the waveguide fallback |
| `dcvoice_string.h`, `dcvoice_excitation*.h` | the waveguide string (`StringModel`) and its small compiled-in excitation bank (`dcvoice_excitation_data.h`, GENERATED by `tools/voice/extract_excitations.py`) |
| `dcvoice_types.h` | `VoiceNote` / `VoiceTimeline` / `VoiceClock` / `IChartVoice` and the `kFlag*` bits |
| `dcvoice_plan.h` | `planChart`: chart notes -> `VoiceNote`s (release policy, strum direction from the beat grid, tremolo). CONTROL THREAD. Used by the tool and tests; the engine's host builds its own events |
| `dcvoice_timeline.h` | `ChartTimelineVoice`: plays a planned timeline through either core (tool, tests, RT gate) |
| `src/synth/GuitarChartVoice.h` | the `dcsynth::ChartVoice` adapter: DCA flags -> `kFlag*`, strum delay from `chordIndex`/`strum`, slide reshaping, pack-or-fallback core; `useLazyZones` / `preloadZones` / `staleZoneBytes` / `lazyPacks` (#417) |
| `src/synth/ChartZoneDemand.h` | `planZoneDemand` (#417): a chart table -> the zones the sampler can select for it (whole pitch groups, a 1-semitone margin), MIRRORING `SampleInstrument::startNow` / `endLane` / `rezone` / `tremoloRepick`; `staleZoneBytes` |
| `CREDITS.md` | provenance and licences of every recording |

## What is recorded, what is processed

- **Recorded**: every sustained note (guitar chromatic, 3 velocity layers x 2
  round robins; bass fingerstyle 2 layers x 2 rr, pick 2 x 2 every 3
  semitones), hammer-ons (Gretsch), release noises, dead notes (guitar, per
  string), bass ghost notes, the slap thumb (bass, muted slap), bass SLAPS
  (#109: `Art::SlapNote`, a slap bass's thumb slaps, every natural note
  E1..D4, 2 layers x 2 takes; a sharp is the nearest natural retuned a
  semitone; string and fret MEASURED from each recording's inharmonicity; converted
  to the selected bass with its own plan, `sourceFor("bass-slap")`), and — from the
  ARTICULATION pack, for every guitar model — palm mutes (every string x fret),
  natural harmonics (each string at its 12th / 7th / 5th / 4th-fret nodes; a
  number a string lacks is borrowed from another string and retuned) and pinch
  harmonics (every string, ~a dozen frets; the nearest fret, retuned).
- **Resampled**: bends, pre-bends, releases and vibrato move the playback rate
  of the playing recording. A slide RE-ZONES at every fret it crosses (the new
  fret's recording, time-aligned, cross-faded in 6 ms), so it is a chromatic
  glissando of real notes rather than one note pitched up seven semitones.
  A one-take pack (the Strat's) also re-zones REPEATED attacks: two in three
  play the neighbouring fret's recording retuned a semitone (no machine gun).
- **Modelled**: the POP (no pop recording exists to us: the slap set the
  loader was built against calls every file a slap and nothing in the audio singles one out) — the finger
  recording with a TRANSIENT brightness (a parallel high-pass whose gain decays:
  +12 dB above 1.5 kHz in ~0.08 s) and its string snapping onto the frets (the
  same recording through a one-sided clip, high-passed, ~35 ms). The recorded slap
  played as a pop measured WORSE (realism distance 2.40 against the pop model's
  0.86, #109), so it is not used. The SLAP model (+15 dB above 400 Hz settling at
  +4 dB in ~0.15 s, the recorded thumb on muted strings) still plays when
  `bass.dcvp` has no recorded slaps (`SlapPopShapes::slap`). Both fitted with
  `realism_eval.py` against CC0 / CC BY slap phrases (`voice_render --tune`
  overrides them for calibration ONLY; `slapRec.<field>` is the recorded slap's).
- **Recorded slap vs the references (#109, measured 2026-10-07)**: the slap
  distance went 0.75 (model) -> 7.46 (recording); 7.98 vs 1.69 on reference
  events with no next onset inside 450 ms. That set's E / A strings are
  dark after the impact (-61 dB above 2 kHz at 120-400 ms against the phrases'
  -31; its own FINGERED notes are as dark, -62) and its impact is brighter
  (-15 vs -29 dB at 0-30 ms). A plain finger note with no slap at all scores 2.03
  against the same slap references: with three phrases of three other basses,
  this metric measures those basses' tone more than the technique. The level is
  set by RMS, not peak (`kSlapNoteLevel`): +3 dB over a finger note in the first
  300 ms, where the model's slap sat.
- **Approximated** only when `guitar-arts.dcvp` is missing: natural harmonics
  (the recorded note at the harmonic's pitch, low-passed just above it), pinch
  harmonics (the picked note quieter + the recorded note k*f0 through a narrow
  band-pass), palm mutes (staccato / sustain, low-passed and shortened).
- **Humanised** (#52, deterministic per note): every attack +-0.6 dB; vibrato
  ~4.3 Hz whose rate (+-10 %) and depth (+-15 %) wander, one-sided on a guitar,
  starting 0.12-0.37 s in (`SampleInstrument::vibrato`); a strum's strings land
  +-1.5 ms off the even spread and +-8 % in force (`GuitarChartVoice::strumDelayMs`
  / `strumVelocityScale`). The HOST picks a chord's strum direction from the beat
  grid (`ChartNoteEventBuilder.StrumFor`, managed): on the beat and the even
  eighth / sixteenth subdivisions down, between them up, palm / accent down.

## Rules that bite

- **ONLY THE ZONES THE CHART PLAYS ARE DECODED (#417).** The engine's voices load
  their packs LAZILY (`PackLoad::Lazy`: the zone table, a few KB) and decode, on
  the CONTROL thread, the zones `planZoneDemand` finds in the chart: when the
  voice is built (`AudioEngine::buildChartVoice`, with the published table) and
  BEFORE a new table is published (`AudioEngine::setChartNotes`, under the seat's
  `controlMutex`, which keeps the voice alive). An edit therefore never misses —
  its zones are decoded before its notes can play. A new chart that would leave
  8 MiB or more decoded for nothing (`kChartStaleZoneBytes`: another song) gets a
  voice on FRESH packs (`LazyFresh`), cross-faded in like a model swap; the old
  voice and its zones go with the reaper. A zone the plan missed is not a fault:
  `selectZone` plays the nearest DECODED recording (retuned; nothing when none is
  decoded), flags the group, and the CHART REAPER (`serviceChartZonesLocked`,
  polling every 20 ms while a lazy voice is published — the callback cannot
  signal) decodes it. The audio thread never decodes, allocates, locks or waits
  for a zone. **`planZoneDemand` mirrors the sampler's note-on**: change which
  articulation or pitch a note selects there and change the plan, or
  `testDemandCoversEveryNote` fails (the lazy render must stay BIT-IDENTICAL and
  ask for nothing). Decoded zones stay until the pack goes; two seats on the same
  selection share one lazy pack (the union of their charts). A zone whose bytes no
  longer decode (a damaged stream, a pack file replaced on disk — `ensureZones`
  checks the size) stays absent and is never retried. The live input does not use
  these voices. Measured with real charts (DotCharter's
  `ChartVoiceZoneRamHarnessTests`, 2026-10-08): a Strat selection 86.5 MB ->
  16-49 MB, the Gretsch 168 MB -> 67-120 MB (a chromatic set: every string shares
  its pitches), the bass 124 MB -> 40-50 MB; the decode is 50-500 ms on the
  control thread.
- **ONE PACK PER SELECTION, AND ONLY WHILE ON.** The engine builds the voice when
  the source is enabled (or its selection changes while it is), and RELEASES it —
  and with the last reference its pack — on the chart REAPER thread once a
  disabled source is silent (#61 part 2). A model / pickup change is a NEW voice:
  the engine publishes the replaced one as `fading`, the audio thread calls its
  `fadeOutAll` (10 ms) and renders it until silent, the reaper frees it. Never
  load a pack on the audio thread and never free one there. Measured on the real
  packs (2026-10-07, coded v2 packs, low priority on 8 cores): 230-250 ms to load
  a Strat position (+ the articulation pack), ~560 ms the Gretsch set, ~330 ms the
  bass — 2.5-3x the raw v1 load, the price of decoding (an EAGER load; the engine
  loads lazily since #417 and decodes only the chart's zones); private memory back
  below its pre-enable level after a disable (`ChartVoiceRealPackRamTests`).
- **The packs are CODED, LOSSLESSLY (#408).** A v2 pack is 36 % of its PCM on
  disk; `parsePack` decodes every zone into the same int16 buffer a v1 pack holds
  (an eager pack: ~87 MB a Strat selection, ~168 MB the Gretsch one, ~124 MB the
  bass), a lazy pack each zone into its own buffer (#417), and every render is
  byte-identical to the v1 pack's. Decoding never runs on the audio thread: the
  sampler only ever sees decoded int16. A lossy codec (Vorbis 160 kb/s: 19 % of
  PCM) was measured and NOT taken — it moved realism distances (b_pop 0.86 ->
  1.01, the bass sustain's t20 1272 -> 990 ms) for 100 MB more saving. A pack
  format change is a VERSION bump: the reader rejects versions it does not know,
  and `VoicePackPackagingTests` keeps the builder's `PACK_VERSION, PACK_CODEC` and
  the reader's `kPackVersionCoded` / `kPackCodecLossless` equal.
- **A conversion is only as good as the source's placement.** `sourceFor` records
  where each pack was recorded; the Karoryfer placements were MEASURED from the
  recordings' own comb (`calibrate_pickups.py` section 3), not read off a readme. A
  weakly known placement gets a gentler inverse (`SourceSound::inverseG`).

- **`render()` and every event are allocation-, lock- and I/O-free; pack loading
  is not, and happens only in `prepare()` (control thread) via `acquirePack`.**
  The sampler reads the pack through a raw pointer its owner keeps alive.
- **Every note-off is honoured.** Release: ~70 ms natural damping plus the
  recorded release noise (only after notes longer than 150 ms). `END_DAMPED`
  (`kFlagEndDamped`, cut by the next note on the string): 15 ms, no noise.
  Choke: 4 ms. Only notes the host keeps sounding (hand shapes / let ring) ring
  on — the voice never extends a note on its own.
- **Determinism**: `reset()` restarts every round robin; the same events render
  the same samples, and the block size never changes the output (the timeline
  front end aims controls at the end of the control STEP and releases with half
  a sample of tolerance, like starts).
- **Levels are set in two places, deliberately**: the pack builder normalises
  every pitch's loudest layer to 0.5 and compresses the softer layers' relation
  to the 0.35 power (the recordings span ~23 dB p..f); `SampleInstrument::artLevel`
  (and `zoneLevel` for the articulation pack) balances the kinds of recording
  against a sustained note (a default guitar note peaks near -16 dBFS, a bass
  note near -11 dBFS, a full strum keeps headroom). Those balances are MEASURED
  (#52): a muted pluck (dead note) peaks ~3 dB (guitar) / ~5 dB (bass) above an
  ordinary note on the held-out Karoryfer sets — a dead note at the old 0.35 was
  a faint tick 9 dB under the notes; a palm mute peaks like an open note, a pinch
  ~7 dB over it (Guitar-TECHS player 1's own sessions).
- **The held-out references never ship.** Guitar-TECHS player 2, Emilyguitar's
  muted strings, Swagbass / Fashionbass and the Freesound slap phrases are what
  `realism_eval.py` measures against; adding any of them to the manifest makes
  the evaluation circular. (Swagbass's muted-slap NOISES are packed as the slap
  thumb; its notes are the reference.)
- **No non-free source ships.** The recorded thumb slaps (`Art::SlapNote`) are
  no longer built: the packs hold CC0 / CC BY recordings only, so the voice plays
  the slap MODEL (`pack_->has(Art::SlapNote, 0)` is false). The loader still
  plays slaps from any `.dcvp` that carries them.
- **`Art::SlapNote` (9) is new in #109**: a DLL from before it REJECTS the whole
  bass pack (`parsePack`: unknown articulation), so the pack and the DLL ship
  together. `build` writes `voice-pack.stamp` (SHA-256 of the manifest and the
  builder) and `New-Release.ps1` rebuilds packs whose stamp does not match.
- **The pack builder must stay deterministic**: same manifest, same bytes. It
  measures every pitched recording's f0 by harmonic sum (a single-peak search
  misreads low, short bass notes); ghost notes take their sustain's pitch.
- **The waveguide's excitation bank is extracted WITH the synth's own loop
  maths** (`extract_excitations.py` mirrors `designLoss` / `fitDispersion`):
  change one and the other in the same change, then regenerate.
- **The bank is stored small and its size is a TEST** (#61): each clip
  band-limited to 22.05 kHz (picks, dead notes) or 11.025 kHz (bass fingers,
  ghosts — under -57 dB above 5 kHz), 12-bit with an exactly-zero sum, two base-64
  digits per sample in one string literal per clip (`clipSample` decodes inside
  `addClip`). `testBankIsShipped` caps it at 290,000 samples and a 640 KiB header;
  measured against the 3.6 MB int16 table it replaced (`realism_eval.py` with an
  empty `--packs` dir), every distance moved by at most 0.01 and every attack
  nulls >= 35 dB.
  The empty-bank pluck (the "drop it" option) measured worse on 8 of 13 sets.

## Tests and tools

- `juce_sharp_sample_voice_test` (ctest `sample_voice`): builds packs IN MEMORY
  from synthesised tones and round-trips the binary format through the real
  parser (as version 2, coded by a fixed-predictor encoder in the test, and a v1
  twin that must load bit-identical), decodes the PYTHON encoder's golden stream
  (`build_voice_pack.py codec-golden`) bit-exact, rejects unknown versions /
  codecs, truncated, over-long and damaged streams; pitch (0.000 cents), bends, slides fret by fret, note-offs honoured in
  a sweep vs let-ring inside a shape, same-string damping, palm / dead / harmonic
  / pinch / ghost / slap, determinism, block-size independence, zero
  allocations on the render path. Malformed packs are rejected.
- `juce_sharp_guitar_voice_test` (ctest `guitar_voice`): the waveguide fallback,
  incl. every clip's decode, a RECORDED attack per string and articulation
  (it must differ from the empty-bank pluck) and the bank's size budget.
- `RtChartVoiceRendersEveryArticulationWithoutAllocating` in the RT gate:
  waveguide guitar, waveguide bass and the sample player (on a pack synthesised
  in the scenario).
- `juce_sharp_sample_voice_test` also pins the realism pass (#52): the
  articulation pack's palm / natural / pinch recordings play (a 7 kHz marker
  only those fixture zones carry) and in tune, an unknown articulation code is
  rejected, slap and pop brightness DECAYS (a static shelf fails it) and a pop
  clanks, dead notes peak near a note, the vibrato is late / one-sided / ~4.3 Hz
  with unequal cycles, repeated notes on a one-take pack differ, a strum is
  jittered, a pull-off has no pick click — every guard seen failing with its
  change reverted.
- A hammer-on, pull-off or tap onto a SILENT string (a legato run moving to a
  new string, tapping from silence) is fretting-hand too (DotCharter #100):
  `SampleInstrument::startNow` enters it like the ringing-string legato path:
  the pack's hammer-on recording, else the sustain past its pick click, at the
  string model's softer level (hammer-on 0.6, pull-off 0.75, tap 0.85), never
  the picked attack. Pinned by `testLegatoOntoASilentStringHasNoPick`. The
  CHART side keeps the string ringing INTO a legato note on it
  (`ChartNoteEventBuilder.ReleaseFor`), so this path is only for a string that
  really is silent.
- `RtChartVoiceRendersEveryArticulationWithoutAllocating` covers six voices:
  both waveguides, the sampled guitar, a one-take guitar pack WITH the
  articulation pack (palm / harmonic / pinch, neighbour round robin), the
  sampled bass (ghost, modelled slap, pop) and the sampled bass with RECORDED
  slaps through a non-identity slap conversion (#109; an allocation put in the
  recorded-slap branch fails only that voice).
- `testRecordedSlaps` in `juce_sharp_sample_voice_test` (#109): a slap plays the
  recorded slap (a per-layer marker), retuned, its layer by velocity; a POP does
  not; the slaps convert with their own plan and never the finger notes'; the
  adapter gives them the slap source bass's (`GuitarChartVoice::sampler()`). Each
  guard seen failing with its change reverted.
- `tools/voice/realism_eval.py refs|render|measure|report` (#52): renders the
  notes the held-out recordings play through the ENGINE path
  (`voice_render --events`, the default model) and compares per articulation
  (attack, band levels over time, decay, harmonic structure, vibrato rate /
  depth / regularity, levels against an open note); a z-score per metric and an
  RMS distance per articulation. References: see "held-out" above.
- `juce_sharp_voice_render` (EXCLUDE_FROM_ALL): `--events <file> --out <wav>
  [--bass] [--model <key> --pickup <sel>]` renders through `GuitarChartVoice`
  (the engine adapter, flags, strum, models); `--rig <wav>` / `--rig-bass <wav>`
  pass a DI through a fixed amp + cab (for A/B sets); `--pack-dir <dir>` plays the
  real packs; `--listening-set <dir> [--suffix _v3]`, `--notes <file> --out <wav>`,
  `--bench`, and `--models <dir> --pack-dir <dir>` (#50): one riff through every
  model x pickup (x coil split) into the output directory, with a spectra table
  as CSV and as Markdown (centroid and band levels; bass split into its fingers
  and pick halves). Those files are tool output, not repo files.
- `juce_sharp_instrument_models_test` (ctest `instrument_models`): the model
  table, which selections are identity, the rendered comb against its analytic
  transfer (within 1.5 dB), pitch through every model, the spectral DIRECTION
  (neck darker than bridge on every model, a humbucker darker than its split, P
  darker than a J bridge) on packs coloured with their recorded comb, one pack per
  selection and its release, the fade-out, zero allocations while converting.
- LAZY ZONES (#417), in `juce_sharp_sample_voice_test`: `testLazyPackDecodesOnDemand`
  (the table only until asked; exactly the asked zones, bit-identical, counted;
  a damaged zone or a changed file decodes nothing), `testLazySelectionMatchesEager`
  (a decoded group is the eager choice for every art / pitch / layer / rr /
  string; a miss plays the nearest decoded zone, allocates nothing and asks),
  `testDemandCoversEveryNote` (dense charts with every articulation through
  `GuitarChartVoice` + `ChartScheduler`: the lazy voice on its plan renders
  BIT-IDENTICAL to the eager one and asks for nothing — Gretsch + articulations,
  a one-take Strat, the bass with recorded slaps; a tie into a palm mute; a note
  outside the plan sounds, asks, and once serviced is the eager note) and
  `testLazyMissesNeverAllocate`. In the RT gate,
  `RtChartVoiceZoneMissFallsBackWithoutBlocking` (a chart published without its
  zones — `AudioEngine::setChartZonePreloadForTesting` — plays its first note
  from the fallback in the block that asks, the reaper decodes the zones, nothing
  allocated or freed on the callback) and `RtChartVoiceNewChartReleasesTheOldZones`
  (another song swaps in fresh packs: the held bytes fall to the new chart's, an
  edit only adds its zones). Every guard seen failing with its change reverted.
- `RtChartModelSwapCrossfadesAndReleasesThePack` in the RT gate: a mid-note model
  swap through the ENGINE (no gap, no click, nothing allocated or freed on the
  callback), then disable (pack released) and re-enable.
- `tools/voice/calibrate_pickups.py --zip <Clean.zip> --packs <dir>`: the
  calibration the model table cites.
- `tools/voice/build_voice_pack.py build --out <repo>/artifacts/voice-pack`
  (numpy, network on first run; sources cached). `DotCharter.UI.csproj` copies
  `artifacts/voice-pack/*` into `voices/`; `tools/release/New-Release.ps1` builds
  the packs when they are missing and refuses a payload without them.
