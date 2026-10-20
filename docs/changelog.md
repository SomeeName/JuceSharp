# Changelog

Reconstructed from the git history. **v1.0.0**, **v0.11.0**, **v0.10.0**, **v0.9.0** and the releases through
**v0.7.0** carry a git tag (0.8.0 was never tagged).
**The version lives in `Directory.Build.props` at the repository root** — one
`<Version>` for all five packable projects, with `AssemblyVersion` derived from
it by the SDK; no csproj under `src/` carries a `<Version>` at all, and
`PackagingLintTests` asserts that. `src/JuceSharp.Native/CMakeLists.txt` carries
the same number a second time because it feeds `dca_version_string()`; the same
lint asserts the two agree. **0.8.0** was set by the ABI-v32 commit; **0.9.0** was
released 2026-10-12 at ABI v44, **0.10.0** on 2026-10-13, still ABI v44; **0.11.0** on 2026-10-14 at ABI v46; **1.0.0** on 2026-10-20 at ABI v47, the
first public snapshot. Bump both
numbers before the next release.

Every entry names the ABI version it required, because the managed wrapper
demands exact equality and will refuse a stale `juce_sharp.dll`.

The 0.9.0 release spans the three sections below the `## 0.10.0` one (ABI
v33 → v44): they were three stacked `## Unreleased` headings until 2026-09-11,
which read as three pending releases rather than one.

---

## 1.0.0 — 2026-10-20 (ABI v47)

**ABI v47** — rebuild `juce_sharp.dll`; the managed wrapper refuses a v46 (or older) one.
The first public snapshot. The version moves 0.11.0 → 1.0.0 in both places that
carry it (`Directory.Build.props` and `src/JuceSharp.Native/CMakeLists.txt`, which
feeds `dca_version_string()`); the ABI itself did not change for the release.

- **External process insert (ABI v47).** A seat's dry input is written into a
  named shared-memory ring another process created, and the processed block it
  writes back replaces the seat's rig. No allocation, lock, wait or syscall on the
  callback; an underrun plays the dry input; overruns, underruns and late blocks
  are counted separately. Map/unmap happen on the caller's thread, and sessions
  are taken at the bottom of a 5 ms dip behind a hazard slot. Six additive exports
  (`open` / `close` / `get_stats` plus their `_for_player` twins), the
  `dca_extproc_stats` struct, and two new statuses: `DCA_ERR_SAMPLE_RATE = 7`,
  `DCA_ERR_NOT_FOUND = 8`. Managed face: `JuceAudioEngine.OpenExternalProcessInsert` /
  `CloseExternalProcessInsert` / `TryGetExternalProcessInsertStats`. `AbiV47Tests`, `SharedRingFormatTests`,
  `ExternalProcessInsertNullDeviceTests`, RT scenario
  `RtExternalProcessInsertExchangesWithoutAllocatingAndFallsBackDry`.

## 0.11.0 — 2026-10-14 (ABI v46)

**ABI v46** — rebuild `juce_sharp.dll`; the managed wrapper refuses a v45 (or v44) one.

- **MIDI to a SANDBOXED VST instrument is sample-accurate (#28, bridge v3, ABI
  unchanged).** `dca_engine_send_midi_at` placed an event on its exact sample
  for the in-process instrument only; the out-of-process bridge drained the
  fifo with no offset, so the child played every event at sample 0 of whichever
  block carried it and hosts had to keep sandboxed MIDI immediate. The bridge
  leg now PARKS scheduled events in its own set (`collectBridgeMidi`, same frame
  arithmetic and `late`/`dropped` counting as the in-process leg) and the shared
  mapping gained a `dcvb::MidiTiming` block AFTER the audio regions — the
  block's render-clock frame and one sample offset per `midi[]` slot — which
  `juce_vst_host.exe` applies. `kBridgeVersion` 2 → 3, `kTotalBytes` 197184 →
  197704, every v2 offset unchanged. **`juce_sharp.dll` and `juce_vst_host.exe`
  must come from the same build**: a v2 mapping is refused by the parent
  (`dca_engine_set_remote_bridge` fails) and by the child (logs `bridge layout
  mismatch`, exits 2), and the managed mirror (DotCharter's `VstSandboxLayout`)
  must say 3. The child also writes `hello` before it starts reading its control
  pipe, which removes the startup deadlock a parent that waited for `hello` hit
  in ~4 launches of 70, and pre-sizes its MidiBuffer so the worker never grows it.

- **The audio callback's load is exported (#29, ABI v46)**, so a host can show a
  real CPU meter. `dca_engine_get_callback_load` (managed
  `JuceAudioEngine.TryGetCallbackLoad` → `CallbackLoad(Average, Peak)`) reports the
  callback's cost as a fraction of the buffer period: an exponentially weighted
  average (0.5 s) and a decaying peak (1.5 s) that reading does not reset. Both are
  computed on the audio thread from relaxed atomics, read 0 while no device runs,
  restart at every device session and skip its warm-up blocks. RT scenario
  `RtCallbackLoadIsMeasuredWithoutAllocating`; `AbiV46Tests`.

- **The `rsamp` chain table is loaded at run time (#25).** No number read out of
  Rocksmith 2014's files is compiled in any more: `dcdsp_rsamp_chains.h` (the 63
  amps' soundbank chains, RTPC curves and baked `init.bnk` defaults) is deleted.
  `dca_rsamp_set_table` / `dca_rsamp_table_row_count` (managed
  `RsAmpChainTable` + `JuceRsAmpTable`) take the table from the host, which
  builds it from the user's own install; it is process-wide, validated whole,
  published to the audio thread with one atomic store and never freed while a
  core may run it. Without a table every `rsamp` module passes audio through.
  The 63 public amp keys and names stay, in `dcdsp_rsamp_models.h`.
- **`bassamp` Ultra Lo / Ultra Hi now follow Ampeg's published SVT-CL figures**
  (+2 dB at 40 Hz and −10 dB at 500 Hz; +9 dB at 8 kHz; Q 0.7 each, ours) instead
  of the HW-300B soundbank's bands. A saved tone with a shift switched on sounds
  slightly different.
- `GearRsAmpTests`, the RT gate and the new native `rsamp_table` test run on
  SYNTHETIC tables.
- **`gate.autoFloor` assumes digital silence until its first reading (#63).**
  Before the first stationary second the floor is −96.3 dBFS (one 16-bit LSB,
  the floor's own lower bound), so the auto threshold starts at −90.3 dBFS
  instead of −84 dBFS: a quiet note played from the first sample is no longer
  expanded away while the gate has not measured anything yet. Pinned by
  `GateAutoFloorAndFutzCrusherTests.AutoFloor_BeforeItsFirstReading_AssumesDigitalSilence`.
  The gate, `bitcrusher`, `eq` and `rsamp` comments now describe what the DSP
  does on its own terms.

---

## 0.10.0 — 2026-10-13 (ABI v44)

Shipped with DotCharter v0.69.0. No ABI change (still v44).

* **Package updates (DotCharter #733):** the Avalonia example on 12.1.4,
  Microsoft.Extensions.Logging.Abstractions 10.0.12, xunit.v3 4.0.2,
  Microsoft.NET.Test.Sdk 18.10.1, coverlet 10.1.0.

* **48 kHz files on a 44.1 kHz device, and every STREAMING source, are now
  converted by the polyphase kernel instead of JUCE's linear resampler
  (JuceSharp #33).** The load-time kernel (`LoadResampler.h`) now downsamples:
  its cutoff follows the OUTPUT Nyquist (`0.99 × min(1, L/M)`), 48 → 44.1 kHz is
  147:160 with 219 taps per phase, and the group delay is still zero by
  construction. A source that missed the preload budget goes through the new
  `PolyphaseStreamSource` — the same kernel per block, allocation- and
  lock-free on the callback — in both directions. Measured on the null device
  (old → new): 48 kHz file on 44.1 kHz, 15 kHz −2.89 → +0.0003 dB; a 21 kHz
  tone's fold-back at 17.1 kHz −10.9 → −102 dB; a 23 kHz tone's alias at
  21.1 kHz −19.1 → −106 dB; streaming 44.1 → 48 kHz, 15 kHz −3.45 → −0.0003 dB.
  Only a ratio with no rational form (a non-integer rate, more than 1024 phases
  or 8:1 down) still uses JUCE's converter. The open log names the converter
  every source used (`ResampleReport::converter`).
  `dca_convert_audio_for_device` is unchanged: still upsample-only.
  Tests: `RtPreloadedSourceDownsamplesAtLoadWithoutAliasing`,
  `RtStreamingSourceResamplesWithThePolyphaseKernel`; the 96 → 48 kHz case of
  `RtPreloadedSourceResamplesAtLoadWithoutShiftingTime` now expects a load-time
  conversion; `AbiV43Tests.ADeviceRateFile_OnAnotherDeviceRate_KeepsItsLevel`
  now expects the 48 kHz device-rate file to be downsampled at level on a
  44.1 kHz device.

---

## 0.9.0 — 2026-10-12 (ABI v44)

Shipped with DotCharter v0.68.0. Everything from the ABI v36 work up to the
Wwise-free .wem encoder (ABI v44); the two `0.9.0 cycle` sections further down
hold its ABI v32 → v33 start.

### Provisional onsets from `JuceGuitarAnalyzer` (2026-10-12, no ABI change, DotCharter #643)

- `JuceGuitarAnalyzer.Push(samples, List<JuceOnsetNotice>)` reports each attack as a
  provisional `JuceOnsetNotice` (`Detected`: detector position as the key, an attack
  estimate, strength) the moment the onset detector fires, ~170 ms before the analyzed
  event; a `Withdrawn` notice when the onset will produce no event.
- `JuceGuitarEvent.DetectorSamplePosition` (init-only) pairs an event with its notice.
- Only onsets whose audio already shows a fresh attack are published: the flux
  detector also fires on the ripple of a ringing low note.
- Detection is unchanged; the other `Push` overloads publish nothing.
  `JuceGuitarAnalyzerProvisionalOnsetTests`.

### One master-bus width (2026-10-12, no ABI change, JuceSharp #31)

- Settled which master-bus comment was true: JUCE's WASAPI, ASIO and DirectSound devices
  hand the callback the ENABLED outputs PACKED (one non-null pointer per enabled channel),
  not one pointer per physical channel with the inactive ones null. The limiter's comment
  (and `native-engine-abi.md`) said the opposite, and the limiter was sized from the
  device's full output list (8 on a sparse mask of 2) while the HP/LP/BP and song-EQ banks
  used the active count with a `jmin(ch, size - 1)` fallback.
- Every per-channel master-bus stage is now sized from ONE `masterBusWidth_` (the active
  output count) set in `prepareMasterBus`, filter state is indexed by the channel, and a
  block wider than the width is skipped and COUNTED (`masterBusSkippedBlocks()`) instead
  of folding two channels onto one biquad state. `MasterLimiter::preparedChannels()` and
  `AudioEngine::masterBusStageWidths()` expose the widths.
- RT gate `RtSparseOutputMaskIsPackedAndTheMasterBusKeepsLAndRApart` (outputs 3-4 of 8 on
  the null device) and managed
  `MasterLimiterNullDeviceTests.ASparseOutputMask_StillReachesTheLimiterOnEveryBlock`.

### Song speed / pitch crossing 100 % mid-song is seamless (2026-10-12, no ABI change, #32)

- Slowed -> 100 % no longer skips the stretcher's read-ahead (+68..81 ms of song), and
  100 % -> slowed no longer replays the stretcher's stale audio: once a
  `RubberBandAudioSource` has stretched it stays on, at ratio 1.0 too, until the next
  seek hands it back to the pass-through path.
- A seek or stop now resets the song's (and a grouped stem's stem-group) stretcher in
  one step with the transport move. A seek while slowed used to play out the pre-seek
  read-ahead first.
- A stretcher's first block is padded with Rubber Band's preferred start pad and its
  start delay dropped: no more ~27 ms of silence and fade-in on R3.
- R2 (Faster) is prepared for the 0.25x speed floor, so the first slow ratio no longer
  allocates 2 x 320 KB on the audio callback.
- RT gate: `RtSpeedCrossingUnityIsSeamless{Finer,Faster,StemGroup}`.

### A stopped engine refuses to restart (2026-10-12, no ABI change, #27)

- `dca_engine_start` on an engine `dca_engine_stop` stopped returns `DCA_ERR_DEVICE`
  with the reason logged; `JuceAudioEngine.Start()` throws `InvalidOperationException`.
  `stop()` detaches every source and the audio callback for good, so a restart used to
  open a live device with silence on it and report success. Recreate the engine instead
  (`dca_engine_reconfigure` changes device without stopping).
- `DspPerPlayerChainTests` opens its own engine for the open-device half rather than
  restarting the fixture's, and `EngineDiagnosticsLogTests` provokes the native log
  sink with the refused restart.

### Wwise-free Wwise-Vorbis .wem encoding (2026-10-12, ABI v44, DotCharter #317)

- `dca_wem_encode_vorbis` / `dca_wem_encode_free`, managed `JuceWemEncoder.EncodeVorbis`:
  48 kHz stereo float in, a `.wem` written the way Wwise 2021 writes it for Rocksmith out,
  no Wwise installed. libvorbis 1.3.7 at quality 0.4 with an 18 kHz lowpass gives a setup
  header byte-identical to Wwise's; every other field follows rules measured on 26 Wwise
  files (`docs/agents/wem-encoder.md`).
- The decoder's bit I/O and codebook library moved verbatim into `src/wem/WemVorbisCommon.h`.

### BS-RoFormer SW at its training chunk, 2x faster on DirectML (2026-10-06, no ABI change, DotCharter #267)

- `tools/bs-roformer-sw-export`: rebuilds BS-RoFormer SW from the PyTorch checkpoint
  (`enerjazzer/BS-ROFO-SW-Fixed`) at its 13.35 s TRAINING chunk (588,800 samples,
  T = 1151), with the same split-complex IO, byte for byte reproducible. The 4 s export's
  stems agree with the training chunk's to only 13-27 dB per stem.
- The split-complex song loop is MSST's `demix`, sample for sample (`FadeWindowChunker`).
  Whole-song stems now agree with PyTorch's at the same chunk to 83-102 dB per stem (fp32).
- `ChunkPipeline` + `SplitComplexIoBinding`: the model's IO is bound to two slots of arrays
  allocated once per song; while chunk k runs, chunk k-1's iSTFTs and overlap-add and chunk
  k+1's fill and forward STFT run on pool threads. Each song logs its chunk count, stage
  times and the non-finite samples the scrub silenced.
- DirectML graph fusion per MODEL: on for a file whose metadata says
  `dml_graph_fusion=verified` (`OnnxSessionFactory.DirectMlGraphFusionKey`, read without a
  session by `OnnxModelMetadata`), off for every other one as before.
  `ApplyDirectMlOptions(options, allowGraphFusion)`, `Create(..., directMlGraphFusion)`.
- `SampleRateConverter` uses a per-phase weight table for a rational rate pair: the six
  post-resamples of a 224 s song went from 3.3 s to 0.66 s, same output to 1e-5.
- Measured (RTX 5070 Ti, 224 s song from a 48 kHz WAV, Release, median of 3 interleaved):
  the 4 s export 39.8 s -> 32.5 s; the training-chunk fp16 export 20.1 s at +5.2 GB of video
  memory (PyTorch fp16 on CUDA in the same session: 16.1 s, +3.1 GB).
- New tests: `BsRoformerSwPipelineTests` (default gate: the chunker against values from
  MSST's own demix, the pipeline's slot and drain rules, the metadata read, the fusion
  opt-in, the phase table, the whole driver on a tiny model). `BsRoformerSwModelTests` /
  `BsRoformerSwDirectMlTests` read the geometry from the reference, check a silent chunk,
  and require fusion-ON parity from a model that declares it.

### BS-RoFormer SW, the split-complex BS-RoFormer layout (2026-10-05, no ABI change, DotCharter #258)

- New catalogue entry `bs-roformer-sw-6s` (`bs_roformer_sw_6stem_fp16.onnx`,
  352,778,874 bytes, pinned from the Hugging Face LFS oid): six stems, bass,
  drums, other, vocals, guitar, piano. Weights licence unknown; see its
  `LicenceNote`.
- `BsRoformerIoBinding` classifies a BS-RoFormer graph from its declared tensors:
  `PackedChannels` (the existing 4-channel export) or `SplitComplex`
  (`spec_real` / `spec_imag` in, `[1, stems, 2, F, T]` out). A split-complex
  model's n_fft, hop, chunk and stem names come from its metadata and are
  checked against the traced frame count.
- `BsRoformerInference` drives the split-complex layout (`PredictNamedStems`):
  a reflect-padded STFT (`Stft.Forward(..., StftPadding.Reflect)`, new), the
  two-input / two-output run (`SpectralOnnxRunner`), one iSTFT per stem and
  channel in parallel, every buffer reused across chunks. `OnnxStemSeparator`
  names those stems in the MODEL's order instead of htdemucs'.
- Pinned by `BsRoformerSwLayoutTests` (default gate) and the env-gated
  `BsRoformerSwModelTests` (CPU parity against the export's `reference.npz`,
  stems-sum check) and `BsRoformerSwDirectMlTests` (opt-in: DirectML parity at
  three session settings, and a whole song through the shipped path). DirectML
  graph fusion breaks SW exactly as it broke htdemucs; the global
  `ep.dml.disable_graph_fusion=1` covers it.

### ABI v42 — native Wwise-Vorbis (.wem) decoding (2026-10-08, DotCharter #426)

- Seven new exports, `dca_wem_open_file` / `_open_memory` / `_get_info` /
  `_read_f32` / `_read_s16` / `_seek` / `_destroy`, behind the managed
  `JuceWemDecoder`. A Rocksmith `.wem` decodes to PCM with no ww2ogg step and no
  temporary file. The Vorbis headers are rebuilt natively (a ww2ogg port with the
  aoTuV and default codebook libraries embedded) and the packets go straight to
  JUCE's libvorbis.
- Every engine's format manager opens `.wem`, so `OpenFile("x.wem")` /
  `OpenMemory(bytes, "wem")` play one like any other file (16-bit preload).
- Output matches Ww2Ogg.Core + NVorbis on all 94 songs of the owner's library:
  the same length and alignment, float within 1.0e-6, 16-bit within 1 LSB. It is
  about 2.2x faster than that path.
- Pinned by `AbiV42Tests`, `Wem/WemDecoderTests` (synthetic fixtures in every
  layout) and `Wem/WemCorpusTests`.

### ABI v41 — the Null device is a per-engine create flag (2026-10-04, DotCharter #85)

- New export `dca_engine_create2(cfg, flags, out)`; `dca_engine_create` is it with
  flags 0. `DCA_ENGINE_CREATE_NULL_DEVICE` registers the "Null" device type with
  THAT engine (and its mirror); `DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL` also selects
  manual-pump mode. An unknown bit is `DCA_ERR_INVALID_ARG`.
- Managed: `JuceEngineConfig.EnableNullDevice` / `NullDeviceManualPump`; the
  `JuceAudioEngine` constructor always calls `dca_engine_create2`.
- Why: the env var `DCA_ENABLE_NULL_DEVICE` is process-wide. A host that set it
  around one offline engine's construction registered the Null type in a LIVE
  engine another thread rebuilt meanwhile. The env vars still work (tests, tools)
  and are OR-ed with the flags; neither source can switch the other off.
- Pinned by `AbiV41Tests`.

### Filter corners past a quarter of the rate, a tube mic, an off-axis angle, 14 cab IRs (2026-10-04, no ABI change)

* **Corners past a quarter of the rate are designed where they say** (DotCharter
  #87). `dcdsp::clampCutoff` holds every corner under a quarter of the rate (12 kHz
  at 48 kHz); the cores whose params advertise more now opt into
  `clampCutoffNearNyquist` (0.9 x Nyquist): `svf` (a 16 kHz low-pass was -10 dB at
  16 kHz; it is -3.01), `graphiceq`'s 16 kHz band, the hall / gated / reverse /
  plate reverb damping, `convreverb`'s high cut and `tapesat`'s roll-off. Corners
  below a quarter of the rate do not move. `graphiceq`'s safety limiter no longer
  low-passes a flat EQ (-3 dB at 12 kHz before; see dsp-tone-rig.md).
* **`micpos` gains a fifth mic, `Tube`, and an `angleDeg` param** (DotCharter #75),
  both appended (persisted `mic` 0-3 and every preset that never sets the angle
  sound the same). Calibrated against the Neumann U 67 and Shure SM57 published
  charts; see `docs/guides/gear-ampcab.md`.
* **The `cab` bank has 26 models** (DotCharter #74): 12-25 add the 4x10 / 1x10 /
  2x15 guitar, PA and hi-fi boxes, and 2x10 / 3x10 / 6x10 / 1x12 / 2x12 / 2x15 /
  4x15 / 15+10 bass layouts, generated from Jensen's measured driver curves by
  `src/JuceSharp.Native/tools/cab_irs/gen_cab_irs.py`. Models 0-11 are untouched,
  bit for bit.
* Pinned by the new `tone_fidelity_cores` ctest, `FilterCornersAboveQuarterRateTests`
  and the new `GearAmpCabTests` cases; the RT gate sweeps every cab model, the
  tube mic and the angle with the sentinel armed.

### `eq` covers a four-band studio parametric (2026-10-04, no ABI change)

* **The `eq` module's frequency ranges are widened**: low shelf 20-500 Hz (was
  40-400), bell 100-8000 Hz (was 200-4000), high shelf 2-20 kHz (was 2-12 kHz).
  A four-band studio parametric reaches all four ends, and DotCharter's converter
  clamped them (DotCharter #68). The params are Hz and the defaults did not move.
* **The treble shelf can actually reach 20 kHz.** `dcdsp::clampCutoff`, which every
  biquad designs through, holds a corner under `0.4999 × Nyquist` — a QUARTER of
  the rate, 12 kHz at 48 kHz and 11 kHz at 44.1 kHz — which is where this band's
  old 12 kHz top came from. Every core is voiced against that ceiling, so it
  stays; `Biquad::setHighShelf(..., nearNyquist = true)` opts one design into
  `clampCutoffNearNyquist` (0.9 × Nyquist), and only the `eq`'s treble shelf
  uses it. The guard sits on the rate-derived corner, never the stored param: a
  20 kHz shelf runs at 19.845 kHz on a 44.1 kHz device and gets its 20 kHz back
  on a 48 kHz one. One audible change for an existing preset: a treble corner
  between 11 and 12 kHz on a 44.1 kHz device, which the ceiling used to hold at
  11.0 kHz. `EqWidenedRangeTests` (RBJ response at the new ends, the guard) and
  `RtEqSweepsItsWidenedRangeAt44kWithoutAllocating`.

### `amp` voicing 5 (Jazz Solid 120) re-staged — no ABI change (2026-10-04)

* **Jazz Solid 120 no longer attenuates.** Its row in `dcdsp_amp.h`'s voicing table
  had a preamp of -6..+2 dB and a power stage of 0.25..0.60, so at equal knobs it
  rendered ~15 LU under the Blackface Twin (its fundamental 16 dB under the input
  at gain 0.2). It now takes the Twin's gain staging (preamp 0..+10 dB, power
  0.60..1.40) and keeps its own single clean stage, heavy NFB, no CF / OT / sag:
  measured through a 1x12 and a 4x12 at gain 0.2 / 0.5 / 0.8, it sits within
  0.8 LU of the Twin. A saved rig on this voicing gets ~15 dB louder. DotCharter
  #71; the consumer's per-voicing trim and its pin were re-measured with it.

### ABI v40 — instrument models for the chart voices, packs released when off (2026-10-04)

* **The built-in guitar and bass sound like a chosen instrument and pickup.**
  `JuceAudioEngine.TrySetChartInstrument` (`JuceChartInstrument`: a
  `JuceChartInstrumentModel` — Stratocaster SSS / HSS, Telecaster, Les Paul,
  semi-hollow; Jazz, Precision, PJ, hollow-body bass — a `JuceChartPickup` and
  coil split), `GetChartInstrument`, `GetChartModelInfo` (the selector, coil
  split, installed packs, which positions are real recordings) and
  `ChartVoicePackBytes`. The Strat's five positions play EGFxSet recordings
  (CC BY 4.0, one pack per position); the other models are the closest recording
  converted per note (pickup-position comb, resonance, body EQ), calibrated on
  EGFxSet. Only the selection's own pack is loaded; a change while playing
  cross-fades.
* **A disabled chart source holds no sample memory**: its voice and pack are
  released off the audio thread once silent, and rebuilt by the next enable.
  `TrySetChartVoice` on a disabled source now only records the voice.
* `JuceChartSourceStatus` gains `Instrument`, `IsSampled`, `IsVoiceLoaded` and
  `PackBytes` (six fields appended to the native struct). Requires
  `juce_sharp.dll` at ABI **40**.

### ABI v39 — the chart note carries the capo (2026-10-04)

* `JuceChartNote.Capo` (int, replacing `Reserved0`; the struct is still 80 bytes).
  `Pitch` is unchanged; a voice needs the capo to place natural-harmonic nodes,
  which are measured from it. Requires `juce_sharp.dll` at ABI **39**.

### ABI v38 — the chart source (2026-10-04)

* **A per-seat note generator that plays a chart through the seat's rig.**
  `JuceAudioEngine.SetChartNotes` uploads a chart once (`JuceChartNote` /
  `JuceChartBendPoint`, stride-extensible); `SetSongClockSource` binds the
  transport it follows; `SetChartSourceEnabled`, `TrySetChartVoice`,
  `SetChartSourceParams` and `TryGetChartSourceStatus` drive it. The engine
  schedules every note on the audio thread against the song's audible
  position — sample-accurate, following seeks, loops, pause and tempo — and
  mixes the voice into the seat's rig input AFTER its capture ring, so input
  capture never carries it. Ships a plucked-string PLACEHOLDER voice;
  `IsChartVoiceAvailable` says which voices a build has.
* **`SetInstrumentRoute`** sums the in-process VSTi into a seat's rig input
  instead of mixing it after the master (`-1` keeps the old behaviour). The
  sandboxed instrument cannot be routed.
* Requires `juce_sharp.dll` at ABI **38**. See `docs/agents/native-engine-abi.md`.

### JUCE 8.0.15 -> 9.0.3, a pure port (2026-09-28; no ABI change)

Same behaviour, no new feature: `DCA_ABI_VERSION` does not move (the C ABI
exposes no JUCE type), the DLL must be rebuilt. Read off `git diff 8.0.15 9.0.3`
over every module this engine compiles; the one-paragraph summary is the comment
above `JUCE_TAG` in `src/JuceSharp.Native/CMakeLists.txt`.

* **The shipping build was not on 8.0.15 at all.** The owner's build tree had
  `FETCHCONTENT_SOURCE_DIR_JUCE` pointing at its own `_deps/juce-src`, which
  FetchContent uses as-is and never checks out, so it went on compiling
  **8.0.13** after the 8.0.15 bump — the DLL carried both `JUCE v8.0.13`
  (JUCE's own string) and `JUCE 8.0.15` (our `JUCESHARP_JUCE_TAG`). Configure
  now reads the version out of `juce_StandardHeader.h` and FAILS when it is not
  `JUCE_TAG`. After merging this, that build tree must drop the cache entry (or
  be deleted) so 9.0.3 is fetched.
* **Formats: `JUCE_USE_OPUS=0` and `JUCE_USE_MP3AUDIOFORMAT=0`.** 9.0.2 turned
  the software `MP3AudioFormat` on by default and 9.0.3 added
  `OpusAudioFormat`; `registerBasicFormats()` registers both AHEAD of the
  Windows Media reader that decoded every `.mp3` on 8.x. Left on, every MP3
  would change decoder — measured on two corpus songs and a LAME VBR file, JUCE's
  software decoder puts the audio **624 samples EARLIER** than Windows Media
  (13.0 ms at 48 kHz, 14.1 ms at 44.1 kHz) and reports a length 1,112-1,152
  frames shorter, which moves every chart timed against an MP3. The
  header-sniffing memory probe (`dca_source_open_memory`'s fallback,
  `dca_generate_waveform_from_memory`) would also try two more readers, and
  `.opus` would start decoding natively (the same probe, built on 9.0.3's
  defaults, picks `Opus file` for it). Pinned by
  `JuceFormatSetTests` (an Ogg Opus file is refused; the DLL carries the Ogg,
  FLAC and Windows Media reader names and neither `MP3 file` nor `Opus file`).
  Native Opus is its own later change.
* **Decoding A/B, 8.0.15 vs 9.0.3, same format flags** (a scratch tool outside
  the repo, `registerBasicFormats` + read the whole file): 16/24/32-bit WAV,
  44.1 kHz WAV, FLAC, AIFF and MP3-through-Windows-Media decode **bit-identical**.
  Ogg Vorbis keeps its length and first sample exactly but its samples move by
  at most 6.3e-7 (-124 dBFS, float rounding in the re-vendored libogg/libvorbis
  build). **One real behaviour change, and it is JUCE's hardening (9.0.1
  "protection against malformed AudioFormat input"), not ours to undo: a WAV
  whose chunk claims more bytes than the file holds now opens with ZERO
  samples.** 8.x opened a truncated WAV at its header's length and read zeros
  past the end, and opened a streamed one (`0xFFFFFFFF` sizes, e.g. ffmpeg to a
  pipe) as 1,073,741,823 frames — a 8.6 GB preload. Every WAV DotCharter
  writes itself goes through a seekable file and is unaffected; a user's
  crash-truncated recording is what would notice.
* **MIDI output (Win32): the two retries JUCE 9 dropped are back, in
  `MidiOutputDevice`.** 8.x retried `midiOutShortMsg` on `MIDIERR_NOTREADY`
  50 x 1 ms and `midiOutOpen` on `MMSYSERR_ALLOCATED` 4 x 100 ms inside
  `juce::MidiOutput`; 9.x does neither, and `sendMessageNow` now returns `bool`.
  `sendWithRetry` / `openWithRetry` restore the same bounds (every refusal is
  retried — 9.x no longer says why), and a message still refused after 50 tries
  counts as `failed` rather than `sent`, which is what `failed` was documented to
  mean. 9.x also refuses on a device that is GONE (8.x returned at once there),
  so once a message has used up its retries the next get a single try until the
  driver takes one again, and the retries stop when the dispatcher is asked to
  exit — an unplugged interface costs 50 ms once and never delays a close. `RtMidiOutputRetriesARefusedSendAsJuce8Did` pins the bounds and the
  accounting without a device. Windows MIDI Services and WinRT MIDI stay 0
  (JUCE's defaults); the Win32 input path now re-queues sysex buffers from a
  per-device thread instead of the driver callback.
* **Audio devices — no code change, behaviour to know.**
  `setAudioDeviceSetup` picks the NEAREST listed rate / buffer size where 8.x
  took the device default (only a request the device does not list lands
  differently). ASIO's open probe now disposes its dummy buffers; still the
  BUNDLED ASIO 2.3 headers with `JUCE_ASIO_USE_EXTERNAL_SDK` 0
  (`Windows_build_registers_the_ASIO_host_type` green). WASAPI streams join the
  process's default audio session (8.x passed an uninitialised session GUID) and
  a >32-channel mask no longer overflows. DirectSound: licence header only.
* **VST3 hosting — no code change.** `performEdit` from a plugin updates the
  host parameter without echoing it back into the plugin; a shell plugin's
  factory is given its host context before its classes are enumerated. The
  bundled VST3 SDK is still 3.8.0 (MIT).
* **GUI, out-of-process host only.** `juce_graphics` was rewritten (Drawable is
  no longer a Component, SVG through lunasvg) and Windows multi-touch is OFF by
  default. `juce_vst_host.exe`'s `DocumentWindow` only frames each plugin's own
  HWND, so neither reaches the plugin's drawing; it needs a hands-on look at a
  few real editors (HiDPI included) all the same. `juce_vst_host.exe` grew
  5.7 -> 7.3 MB, `juce_sharp.dll` 10.1 -> 11.8 MB.
* **Licence:** JUCE 9 modules are AGPLv3 or the JUCE 9 commercial EULA, the
  same pair as JUCE 8. VST3 SDK 3.8.0 (MIT) and ASIO SDK 2.3 (GPLv3 or
  Steinberg proprietary) unchanged. `LICENSE`, `Directory.Build.props`, both
  READMEs and `docs/` updated.
* **Found on the way, NOT changed (pure port):** `juce_vst_host.exe` can
  deadlock its own `hello` — a synchronous pipe handle serialises the reader
  thread's `ReadFile` against `initialise`'s `WriteFile`. The product pings
  first and is immune; `VstHostChildLifecycleTests`, which waits for `hello`,
  is not. Same on 8.0.15 (8 of 70 launches) as on 9.0.3 (4 of 70); written up
  in `docs/agents/rt-safety-and-tests.md`.
* No source change was needed to COMPILE: the shipping DLL builds warning-free
  under `/WX` on the first try. RT gate: lock-site set and baseline unchanged.

### The vendored ASIO SDK is gone (2026-09-28; no ABI change)

* **`third_party/ASIOSDK2.3/` (ASIO SDK 2.3.3, Steinberg's proprietary licence)
  is deleted, not upgraded.** JUCE 8.0.15 bundles the ASIO headers itself under
  Steinberg's 2025 dual licence (proprietary or GPLv3) and uses them while
  `JUCE_ASIO_USE_EXTERNAL_SDK` is 0, its default, so the vendored copy had not
  been read by the build since the 8.0.15 bump. Its `ASIO_SDK_DIR` probe, the
  ASIO-absent `JUCE_ASIO=0` branch and the test targets' include hooks went with
  it. The rebuilt DLL is functionally unchanged: the 27 source files of the
  public ASIO SDK 2.3.4 are code-identical to 2.3.3, which differed only in its
  licence text.
* `EngineSurfaceCoverageTests.Windows_build_registers_the_ASIO_host_type` is new:
  it asks the built DLL for its host types and fails if a Windows build ever
  loses ASIO. `NoMacroIsDefinedTwiceForTheShippingDll` now forbids any list edit
  before the single apply, and any `JUCE_ASIO=0`, instead of pinning the deleted
  branch.
* `LICENSE` and every package's licence note now say **MIT** for the VST 3 SDK
  (3.8.0, bundled by JUCE 8.0.15) and **GPLv3 or proprietary** for the ASIO
  headers.

### Wave-5 audit fixes (2026-09-12; no ABI change, `dca_xrun_stats` grew additively)

A fifth read-only audit, this time of the gear wave itself plus a merge-survival
check of wave 4 (five reports, never committed;
75 of 76 sampled wave-4 fixes survived the nineteen merges — the one loss is the
dual-cab decision recorded above). Every fix below was broken on purpose and seen
to fail before it shipped.

> **⚠ HOSTS:** `dca_xrun_stats` gained `input_plugin_faults` (appended; `cbSize`
> keeps it additive). A hosted VST3 that THROWS on the callback is now disabled
> for that seat until the chain is re-attached, and counted there — it used to be
> re-entered, and re-throw, on every block. On a cancel, `dca_stems_separate_file2`
> can leave partial stem WAVs in the output directory: a cache must not treat
> "the directory has WAVs in it" as "separated".

* **The ABI v36 cancellation was dark end to end (Critical).** `SeparateAsync` and
  `SeparateWithProgressAsync` called the five-argument `Separate`, which hard-coded
  `CancellationToken.None`; the token never reached `dca_stems_separate_file2`.
  Both now pass six arguments and there is one route into the core
  (`AbiV36Tests.BothAsyncSeparationPaths_HandTheirTokenToTheCancellableOverload`).
* **Branch PDC after a mid-block param write.** The W4-D54 fix consumed its dirty
  flag at the END of the block, so a write landing mid-block was cleared before
  the modules adopted it; sampled at the top now, and the flag is an atomic
  (`RtBranchPdcFollowsAParamOnTheBlockItChanges`, mid-block phase).
* **A throwing input plugin is latched off per seat** by a generation-keyed bitset
  — the first version keyed on snapshot pointer identity and the allocator's
  address reuse made a detach+attach disable whatever moved into that index; the
  RT gate caught it in one run. `timeEndPeriod` is paired on every exit path of
  the MIDI-out dispatcher; the modal ASIO panel's blocking of every `callSync`
  export is documented at the export with the two rejected alternatives.
* **`AnalysisLevel` is pinned to `10.0`** — `latest` plus `TreatWarningsAsErrors`
  plus `rollForward: latestFeature` made the next SDK's new rules break every
  contributor's build, and `PackagingLintTests` pinned the float; the tool package
  no longer tries to ship a symbol package; the nested `Directory.Build.props`
  files re-import the root (removing the import gave 452 errors: `ImplicitUsings`
  went with it).
* **New gear, first pass (dynamics / drive / amp-cab / filter):** `micpos`
  reported a constant 42-sample latency while its dry tap moved with `distanceCm`
  and the closer capsule came out LATER — the first capsule owns the plane now and
  the test reads a Dirac's actual group delay; `sag` in `tubepre`/`poweramp` was
  the one unsmoothed per-sample multiplier of the wave; **seven cores thumped out
  of digital silence after every prepare** (ds1, hm2, tubepre −21 dBFS, bassod,
  octfuzz, bassamp, poweramp: a pre-clip bias behind a DC blocker reset to zero
  state — the blocker is primed to the stage's steady-state DC); `boost`'s output
  depended on the host block size (a per-block `sat` decision against per-frame
  smoothers, plus a duplicated prepare edit — a regression from W4-D7); the drive
  drawer's alias theory measured at DEFAULTS only (forced 1× still read −126 dB)
  and now carries seven DRIVEN rows; `comb.polarity`, `vocoder.carrier`,
  `svf.source` and `vocoder.formantShift` crossfade; `comb` cleared 38 KB inside
  its sample loop; seven unguarded `std::lround` calls reject NaN first; nine
  one-sided assertions 1.2–16 dB looser than the figures the pages quote are
  two-sided now.
* **New gear, second pass (modulation / pitch / delay / reverb / utility):** the
  convolution reverb shared ONE FFT scratch between the owner thread's IR rebuild
  and the callback (an `irFile` load or `lengthPct` drag raced the callback: `raced
  peak inf`) — the owner has its own FFT now and the RT scenario swaps from a second
  thread; a `lengthPct` sweep re-converted the decoded IR 19 times and is cached; the
  partition geometry survives a block-size change. `pitchcorrect.formant` glides its
  bandwidth expansion instead of switching onto a stale LPC; `barberpole.stages`,
  `basssynth.waveform`, `autopan.shape`/`law` and `ensemble.voices` keep their blend
  on a flip-back; the modulation seam probe reads BOTH channels (it read `.L`, which
  made `autopan.phaseDeg` vacuous); `basssynth` emits no DC before its first confident
  note and none on PWM; the looper's engine-driven auto-close is visible to
  `getParam` and lowering `maxLoopS` mid-record closes the loop instead of wedging;
  the reverse reverb's two readers sum to a constant (COLA: dips of up to −37 dB
  before) and a `windowMs` change re-seats the reader phase; `midside` re-engages its
  side shelf from cleared state; granular no longer cuts in-flight grains when
  `grainMs` drops; the five delay cores' null/degenerate guards are back; eight more
  adapters reject NaN before `lround` (the pitch ones clamped a NaN to the LOW end).
  `GearEntry.h`'s index comment now says a `DOTCHARTER_DSP_NEURAL=OFF` build shifts
  every gear index by one (99 rows).
* **Test-suite hygiene.** The test project had NO runner configuration, so 42
  device / child-process / ONNX classes ran concurrently — that, not the code, was
  every flake of the previous day. `xunit.runner.json` ships next to the assembly
  (verified honoured), five collections with `DisableParallelization` cover 49
  engine-creating classes, an `AllocationProbe` helper settles and re-reads the
  GC counter once (a 360-byte artefact from the runtime's own threads), the five
  wall-clock assertions are structural (kill markers in the python scripts; a
  "whole blocks only" invariant instead of a rate floor), the seat-zero alias
  capture tests compare positions on a STOPPED ring, and the RT gate refuses a
  scenario registered after `RtBridgeWaitIsBounded` (`RtRunListOrderIsPinned`).
  `TestCollectionLintTests` keeps the collection map true.
* Docs: the false "has no public managed wrapper yet" over `ProcessDropTuneOffline`,
  the "cannot be cancelled" warning, an `[Obsolete]` call in the flagship
  `juce-dsp-chain` sample, the `juce-dsp-meters` return contract, `Cancelled = 6`
  in three error tables, two "ABI is still 35" sentences, the NEURAL=OFF
  catalogue count (99 without `namamp`; `JuceDspChainWrapperTests`' floor follows
  the flag), and the `docs/miku-stomp-research.md` note that never existed.

### Gear wave — 43 new tone-rig modules, catalogue 57 → 100 (no ABI change)

Nine palette drawers, one agent each, on 2026-09-11/12. No export changed: every
module is a catalogue row, so `JuceDspModuleCatalog`, `JuceDspNode` and a
catalogue-driven pedalboard pick them up with no wrapper code. The layout that
made it possible is now the rule for new gear — `src/dsp/gear/<Category>Gear.cpp`
per drawer, `GearEntry.h` owning the row shape, `JUCESHARP_RIG_FACTORY_SOURCES`
the one CMake list (docs/agents/dsp-tone-rig.md § "The 2026-09 gear wave"). Every
module ships with a behavioural test that was broken on purpose and seen to
fail, a ramp case on every level/mix, NaN rejection, a two-rate `prepare()`
case, an oversized-block case, and one RT-gate scenario per drawer; consumer
pages are `docs/guides/gear-<drawer>.md`.

* **Dynamics (6)** — `limiter` (look-ahead, sliding-minimum, reports its latency),
  `transient` (attack/sustain), `multiband` (3-band, Linkwitz-Riley), `expander`
  (sidechain), `optocomp` (program-dependent release), `ducker` (sidechain).
* **Drive (7)** — `rat`, `ds1`, `hm2`, `tapesat`, `bassod` (clean blend), `tubepre`
  (sag + DI cab), `octfuzz`. Two-tone alias products all below −60 dB at defaults,
  measured 48 k against 192 k.
* **Amp / cab (4)** — `bassamp` (four voicings, built-in optical compressor),
  `acpre` (feedback notch, phase, piezo fix), `poweramp` (five tube types, sag,
  presence/resonance/damping), `micpos` (four mics, position, distance, room,
  second-mic blend; 42 samples of latency).
* **Filter (5)** — `svf` (LP/BP/HP/notch, manual/LFO/envelope), `comb`, `vocoder`
  (8–24 bands, sidechain carrier, 181 ns/sample at 24 bands), `talkbox`, `tilt`
  (in the `eq` drawer).
* **Modulation (5)** — `tzflanger` (thru-zero, reports its manual delay),
  `barberpole`, `autopan`, `ensemble` (three-line Solina topology), `freqshift`
  (Bode, Hilbert pair, image ≥ 40 dB down).
* **Pitch (4)** — `detune`, `pitchcorrect` (native tracker, scale/key, speed,
  humanize, formant preserve), `twelvestring`, `basssynth` (PolyBLEP, tracker,
  octave down). The two tracker modules are monophonic, 50 Hz–1.4 kHz, ~50 ms
  recognition delay — stated on the page.
* **Delay (5)** — `multitap`, `duckdelay`, `bbddelay` (darker at longer times,
  bounded self-oscillation, pitch glide on a time move), `granular` (freeze,
  212–516 ns/sample), `rhythmdelay` (eight step patterns).
* **Reverb (4)** — `hallreverb` (8-line FDN, four spaces crossfaded after a 200 ms
  warm-up), `convreverb` (user IR via the v22 string-param/IrMailbox path,
  uniform-partitioned FFT, one partition of latency, 385 ns/sample at a 2 s IR),
  `gatedreverb`, `reversereverb`.
* **Utility (3)** — `volume` (expression pedal, four tapers, `smoothMs`),
  `midside` (width, per-side gain/phase, bounded bass-mono), `looper`
  (record/play/overdub/undo/reverse/half-speed, 60 s worst case allocated at
  prepare: 43 MiB at 48 k, 175 MiB at 192 k; loop content survives a device
  change with a documented pitch shift).

**Param shapes a pedalboard UI has never rendered before:** `convreverb.irFile` is
a file path (needs a picker; a refused file keeps the old IR), `looper.state` is a
transport the engine can change on its own (poll `getParam`), `looper.undo` is a
momentary trigger that reads back 0, `multiband` has 13 params in three per-band
triples, and `tzflanger`/`limiter`/`convreverb`/`micpos`/`tapesat`/`bassod` carry
non-zero latency that branch PDC compensates but a per-node display will show.
`svf.cutoffHz` and `comb.freqHz` take plain hertz and want a log taper on the knob.

**Also in this wave (DSP core follow-ups):** four of the five remaining W4-D22 seams
(noisereducer `humDepth`, widener `bassMonoHz`, slapback `repeats`, tapestop
`curve`) crossfade — the fifth, the dual cab's slot-B blend, is the open owner
decision below; five W4-D35 transcendental rows replaced by bounded fast paths
(phaser −25.6 %, compressor −25.9 %, feedbacker −23.1 %; error bounds asserted by
the gate); the graphic EQ's limiter is ADAA1 (−52.9 → −61.2 dBc alias floor); the
cab / dual-cab FIR dispatches to AVX2+FMA at runtime (289 → 162 ns/sample, −44 %);
wah and filterseq resonance now ramp and all three previously unprobed controls
are pinned through core-level accessors. Partitioned-FFT cab convolution was
measured (crossover 512–1024 taps, ~2.9× at 2,048) and declined because the cab's
zero-latency contract does not survive a partition. One item stays open by
design: the dual cab's slot-B blend is still a one-sample switch because
`tests/dualcab_test.cpp` asserts an *immediate* restore — an owner decision.

### ABI v36 — cancellable native stem separation, MIDI-out failed count

Two new exports (184 → 186), nothing reordered or resignatured; both v35 twins still
resolve. Second entry points rather than changed ones, because `void` has no room for an
answer and `dca_stems_separate_file` is a free function over paths, so a `dca_stems_cancel`
would have had to invent a session object the ABI does not have.

* **`dca_stems_separate_file2`** with `dca_stems_progress_cb2` returning `int` — non-zero
  = continue, so a binding that falls off a path fails safe — and a checked point between
  the per-target WAV writes in `StemSeparator.cpp`. New status `DCA_ERR_CANCELLED = 6`. The
  managed native-backend `IAsyncEnumerable` progress path routes its `CancellationToken`
  through it and throws `OperationCanceledException` per the stems rule. A missing model
  file now surfaces as `FileNotFoundException` naming `DemucsGgmlModelCatalog` (native side:
  `DCA_ERR_NOT_INITIALIZED`, was `INVALID_ARG`). The runtime cancellation test skips when
  `DOTCHARTER_STEMS_DEMUCS_CPP=OFF` (this box); the export's presence is pinned by the
  `TryGetExport` sweep regardless.
* **`dca_midi_output_get_counts3`** adds the driver-throw count; `JuceMidiOutput.FailedCount`
  / `ReadAllCounters`, mirrored on `IJuceMidiOutput` and `FakeJuceMidiOutput`.
* Branch PDC re-derives on the block a latency-changing param actually lands (W4-D54 was
  worse than reported: nothing re-ran the recompute at all, so a whammy flipped to
  `lowLatency` left the merger 1,272 samples out for as long as the knob sat still;
  `RtBranchPdcFollowsAParamOnTheBlockItChanges`).
* `namamp` logs the `RetirementQueue` overflow warning once per excursion (the queue's
  `takeOverflowWarning` is NOT edge-triggered and its doc now says so); `tools/tone_response`
  compiles again as an `EXCLUDE_FROM_ALL` target reading the real voicing table; the offline
  "no device open" check is lock-ordered (W4-D73 closed).

### Wave-4 audit fixes (ABI v35; the v36 items above came from the same wave's second pass)

A fourth read-only audit of `main` at 55f3808 (five reports, 216 findings, never
committed) and a ten-branch
fix wave. Two Criticals, both verified by hand before the wave started.

> **⚠ HOSTS: behaviour changes a consumer can observe.**
> * `JuceAudioEngine.SetEqBand` / `ReadEqSpectrum` now THROW `ArgumentOutOfRangeException`
>   on a bad band index or spectrum length; both used to log and return.
> * `JuceAudioPlugin.Bypass` is no longer a cached flag: it asks the engine, so a plugin
>   that was never attached to an input chain reports `true`.
> * Assigning a disposed `JuceLoopRegion` to a source throws `ObjectDisposedException`.
> * `JuceGuitarAnalyzer.LatencySamples` moved from 7,624 to 8,392 at 44.1 kHz (8,704 at
>   48 kHz): it was short of what the post-refinement reads actually consume.
> * `JuceTunerFrame` gained a trailing positional `TimeSeconds`; `WavIo.WriteFloat32(Stream, …)`
>   now requires a seekable stream because it is a front end over `WavWriter`.
> * Three value-returning exports answer `-1` for a fault where their neighbours already
>   did; `dca_engine_dsp_get_metering` deliberately still answers `0`. Every in-repo caller
>   clamps. `dca_stems_separate_file` returns `DCA_ERR_NOT_INITIALIZED` for a missing model
>   file (was `INVALID_ARG`).
> * The nupkgs that carry native code now declare their licence as a FILE (MIT wrapper +
>   the AGPLv3 JUCE / GPLv2 Rubber Band / ASIO SDK / NeuralAudio terms) instead of the
>   `MIT` expression; `JuceSharp.Dsp` stays `MIT`. Three unsigned `InternalsVisibleTo`
>   grants to the DotCharter repo were removed (nothing there used them).
> * .NET analyzers are on with `TreatWarningsAsErrors` across the whole tree.

#### Critical

* **The MDX-Net ONNX drivers threw on their first chunk.** `MdxNetInference.Settings`
  defaults `NFft = 6144` (3 × 2048); the MathNet → `JuceFft` migration made every STFT
  reject a non-power-of-two size, and no test constructed the driver, so five of nine
  catalogue entries including `DefaultIds[MdxNet]` were dead. `JuceFft` now has a radix-3
  decimation for 3·2^k sizes (6144: 110 µs, against 55 µs for a 4096 radix-2) and a
  Bluestein path for any size (526 µs); the radix-2 path is byte-identical.
  `JuceFftBluesteinTests`, `MdxNetGeometryTests` (every catalogue entry's chunk round trip).
* **`DspChain::removeNode` could destroy a module the callback was still reading.** The
  `editing_` check ran under `writeMutex_`, the lock was dropped for the fade wait, and a
  `beginEdit()` in that window made the re-publication a no-op — so the module was erased
  and freed while still in the live order. The edit state is re-checked after the lock is
  re-taken and a stale-scope remove routes to the shadow. `RtRemoveCannotRaceBeginEdit`.

#### Rig (native DSP)

* Edit scopes: a node added inside a scope and removed during its own commit no longer
  dies before the transitional publication is retired; `committing_` is RAII-guarded and
  the wait is bounded; `~DspChain` waits for an in-flight commit; a bypass toggled during a
  move is no longer clobbered (`Slot::bypassSeq`); `chainLatencySamples` walks the PUBLISHED
  order, not the shadow, and the two region walks are one `walkRegionsLocked`.
* Every param write is clamped centrally in `DspChain::setParam` to the catalogue's declared
  range (548 params read back outside it before); the automated-render pre-pass asks
  `DspModule::hasSettableParam` instead of `getParam`, so a poll-only param is rejected.
* `namamp`: the neural stage's scrub moved AHEAD of the amp face's five recursive biquads
  (one NaN latched them for good); `~RetirementQueue` leaks-and-counts instead of handing a
  free to the callback; `ModelIrMailbox::publish` no longer allocates inside `noexcept`.
* Gain zipper sweep, second pass: the amp's preamp and power-amp drives, overdrive's
  `model` (blend + bias crossfade), subharmonic's `waveform`, phaser's `stages`, and
  ringmod/vibrato's shape crossfade re-entrancy are ramped or crossfaded. `whammy` and
  `harmonizer` no longer rebuild an 8,192-point window and clear ~1.8 MB inside the
  callback that saw a `lowLatency`/`formant` flip (`RtPitchCoreFlipDoesNotBurstOneCallback`);
  droptune engage/disengage crossfades instead of splicing (`CrossingUnity_CrossfadesInsteadOfSplicing`).
* `prepare()` discipline: eight cores now `snapToTarget()` after a first prepare (they faded
  up from silence for 10–20 ms after every device change); freeze, feedbacker, mikustomp,
  subharmonic and autowah re-derive their rate-owned counts; `clampCutoff` and the Q/gain
  clamps in `dcdsp_primitives.h` reject NaN instead of passing it to the coefficients; gate,
  noisereducer and miku recover after a scrubbed block; an oversized block is skipped, not
  half-processed; compressor and tremolo respect the prepared channel count.
* Overdrive's adapter asks `OverdriveCore::modelBlendsDry` for its latency instead of
  re-deriving `model >= 3`. `Wave4DspSourceLintTests` fails on a comment citing a test that
  does not exist (two such citations were found).
* Measured and NOT changed: bigmuff's output stage peaks at 0.85 at full volume (the audit's
  clip claim was wrong); the four "dead" `isfinite` guards in the adapter are reached by a
  CTest and stay.

#### Engine (native, non-DSP)

* A hosted VST3's `processBlock` on the input chain runs under a `try`/`catch` like its two
  sibling sites, and the callback has a catch-all backstop (`RtAThrowingInputPluginCannotKillTheCallback`;
  `NativeCallbackGuardLintTests`).
* `dca_engine_show_asio_panel` no longer dereferences the raw device pointer off the message
  thread across a modal dialog — it is marshalled and guarded.
* The instrument gain ramps through `rampOrScaleAdd` on both legs; its `g <= 0` early-out
  was a one-sample cut (`RtGainSlidersRampInsteadOfStepping`).
* The master limiter reports the latency of the block it LAST processed, so one skipped
  block no longer reads as 0 ms for ever; its getter zeroes its out-params.
* MIDI handed to the sandbox child is carried across a bridge timeout instead of dropped
  (counted); `emptyMidi_` is pre-sized; the MIDI-out dispatcher uses `timeBeginPeriod(1)` so
  its 1 ms wait does not defeat its own 1.5 ms spin; `sentCount()` counts messages on the
  wire and `failedCount()` the driver throws.
* The four seat-0 capture exports are real aliases of their `_for_player` twins. The
  polyphase load-resampler kernel is cached per ratio. `MirrorClockController` with
  `targetFrames == 0` falls back to the default depth instead of muting the mirror.
  `PanningAudioSource`'s centre-unity shortcut compared against `0x3F7FFFFF` and never fired;
  it uses an epsilon now.
* CMake: the "ASIO SDK absent" branch edited a definitions list that had already been
  applied, so `juce_sharp` got both `JUCE_ASIO=1` and `=0`.
* Doc rot fixed at the source: a downsample gets NO offline rate conversion (both guards
  refuse it, and the guide said the opposite); the stretch-quality flag is per engine.

#### Wrapper

* `JuceMidiInput`'s wave-2 drain was a no-op — `OnMidi` never acquired the handle, so
  `Dispose` could close the bridge under a live driver callback, and the lint that pinned it
  was a substring match that passed on the no-op. Fixed and pinned behaviourally
  (`MidiInputDispatchDrainTests`).
* `JuceAudioEngine.DisposeChildren` waited up to 60 s per child whose wrapper had been
  collected but not finalized; it grants 2 s to an unreachable wrapper.
* `JuceMixer.Add` folds the new channel into an active solo; `TempoRatio`/`PitchSemitones`
  on a stem-grouped source are dropped with a warning instead of silently ignored;
  seat-scoped chains reject another seat's node id; automation blocks are sized in `long`
  with `JuceDspChain.MaxAutomationEvents` mirrored from the header.
* New surface: `JuceAudioEngine.ProcessDropTuneOffline` (×2, the one ABI export with no
  wrapper), `JuceVst3Scanner.Scan(…, CancellationToken)` + `ScanAsync`,
  `JuceEngineFactory.OpenResult.EngineInterface`, `Min/MaxEqSpectrumBins`; the offline
  worker runs on a dedicated thread per call (`RunOffThePool`) rather than the pool.
* `NativeMethodsTests.EveryDeclaredEntryPoint_IsActuallyExportedByTheDll` sweeps all 186 (184 at the time; two v36 exports since)
  declarations against the DLL with `NativeLibrary.TryGetExport`.

#### Dsp / AudioIo / Diagnostics

* `JuceGuitarAnalyzer`: `_finalizeLag` covers the post-refinement reads; a retriggered note's
  note-on is emitted in the same window as its note-off, not a hop late; the legato short
  floor asks the tuner instead of re-deriving its geometry. `JuceLeadTracker` reports `None`
  for loud unpitched audio and `Silent` only for silence. `JuceChartVerifier.NoiseMargin`,
  the hop and the floor percentile READ `LiveCqtConfig` instead of re-typing it, and
  `CqtEvalConfigTests` lints the source for re-typed literals. The beat detector keeps the
  last complete spectral-flux frame. The spectrogram scrubs non-finite input and never
  returns a value below its documented floor.
* `Windows.Hann` (and siblings) are public; `JuceTunerFrame.TimeSeconds` identifies the
  window a frame belongs to (`JuceLeadFrame` already had it).
* `SampleRateConverter` no longer truncates its anti-alias kernel when downsampling (96 →
  44.1 kHz out-of-band tone: −28.6 → −51.2 dBFS); `JuceAudioDecoder.DecodeAs` validates its
  targets; `WavIo.WriteFloat32` is a front end over `WavWriter`.
* `juce-stems-diag`: `--probe` classifies environment failures instead of throwing, and the
  "press Ctrl+C again to force" line is gone because nothing forced.

#### Stems

* Python sidecar: stdout is decoded as UTF-8 (a non-ASCII stem path came back as mojibake
  and was reported as success); relative `--input`/`--out` are rooted before the child's
  CWD is forced; a timed-out run still reports what the child said on stderr; `Probe()`
  finds a system `python3` on POSIX.
* Downloads: a `206` without a valid `Content-Range` is re-requested whole instead of
  appended; the existence check runs INSIDE the destination gate; `.part` files are swept
  (`VerifiedDownload.CleanPartials`); the destination-gate and the Python env-gate tables
  are refcounted instead of growing for ever; `EnsureFileAsync(…, requireHash: true)` gives
  the kind-less door the catalogue's refusal on demand. The two catalogues share one
  `CatalogueDownload` policy (the types stay separate on purpose).
* `CkptToOnnxConverter` writes to a temp file and renames; the ONNX provider probe is one
  `Lazy`; `ChildProcessRegistry` assigns the job object without a race; the htdemucs path
  applies the same `FitToFrames` correction as its sibling.

#### Packaging / CI / docs

* `PythonScripts/*.py` are packed `copyToOutput="true"` so a `PackageReference` consumer
  gets them (they never did). `global.json` pins the SDK, runner images and action tags
  are pinned and linted, every workflow scopes its token (`CiWorkflowLintTests`),
  `HttpDownloadLintTests` covers all six packable projects, `TestCollectionLintTests`
  checks the `StemSeparatorConcurrency` membership structurally.
* Docs: 63 truth findings fixed across 41 pages (nine deleted engine members documented as
  the migration path, the limiter documented default-ON, a pre-built-binaries claim and a
  macOS/Linux support matrix, an unusable `SetRemoteBridge` page, samples that could not
  compile); four new consumer pages — `getting-started/first-sound.md`,
  `guides/threading.md`, `guides/failure-modes.md`, `guides/realtime-constraints.md` — with
  every sample compiled; `tools/check-doc-links.py` walks the packed readmes too; the
  agent guide names the CMake that actually exists on the owner's box.
* Examples: all six rebuilt for the 0.9 API (edit scopes, sidechain, branch PDC, automated
  offline render, `WavWriter`, `JuceTunerStream`, tempo map, limiter opt-in, `--help`, exit
  codes, `--null` with `--device null` kept as an alias) and published under
  `examples/publish/`.

### Wave-3 audit fixes (ABI v35)

> **⚠ HOSTS MUST ACT: the master limiter is now OFF at engine start.** It was ON
> by default in v34. That contradicted the engine's own "everything is OFF at
> engine start; a solo player pays for nothing they did not ask for" rule in the
> one place it is most expensive — the stage sits on the LIVE MONITOR path, so
> every guitarist gained 1 ms of latency on their own playing simply by
> upgrading, and every consumer's click-alignment arithmetic shifted by 48
> samples at 48 kHz. The rationale for ON (a bounded streaming mirror) is real
> and unchanged; it is now something the host asks for. Call
> `engine.SetMasterLimiter(true)` — or `dca_engine_set_master_limiter` — during
> setup. The −0.3 dBFS / 80 ms defaults are unchanged.

* **`JuceMidiOutput` and `JuceMidiInput` now drain on dispose.** Both held a raw
  `nint` handle closed with one `Interlocked.Exchange`, the only handle-owning
  wrappers in the package with no `NativeHandleOwner`. The OUTPUT one was the
  urgent half: `Send` / `SendAt` / the counters each read the handle into a local
  and then P/Invoke, while `dca_midi_output_close` is a bare `delete` that joins
  the dispatcher thread and destroys the `juce::MidiOutput` — so a sequencer
  thread flushing note-offs while the UI tears the engine down, which IS the
  ordinary shutdown path, was a native use-after-free rather than a caught
  `ObjectDisposedException`.
* **MIDI producers are serialised.** `midiFifo_` is a `juce::AbstractFifo`
  (single-producer) with at least two producers — the JUCE MIDI input thread and
  whatever thread calls `dca_engine_send_midi_at`, which the ABI does not
  constrain. Two concurrent `prepareToWrite`s write the same slot and
  `finishedWrite` double-advances: one event lost, one slot of stale garbage
  forwarded, no counter moving. The lock is message-side only; the consumer stays
  lock-free and `rt_lock_sites.txt` is unchanged.
  `RtConcurrentMidiProducersLoseNoEvents`.
* **`DspChain::commitEdit` owns its scope for the whole commit.** It must drop
  `writeMutex_` for the 10 ms fade wait, and did so with `editing_` still true —
  so a concurrent `beginEdit` / `abortEdit` aborted the in-flight commit and the
  resumed commit then clobbered the new scope. A `committing_` state makes
  `beginEdit` WAIT and `abortEdit` REFUSE. Reachable with no deliberate
  threading: the managed scope finalizer runs on the GC thread.
* **`JuceDspEditScope` can no longer abort the scope that REPLACED it.** The
  chain's `_openScope` is a short `WeakReference`, cleared before the finalizer
  runs, so a dropped scope's late `Dispose`/finalizer aborted the scope opened in
  that window and its whole preset was silently discarded. The open scope is now
  identified by ID (`JuceDspChain.ClaimScope`).
* **`BeginEdit` rolls the stale journal back BEFORE the native call that can
  fail** — it used to roll back after, so a failed begin orphaned the journal and
  the next successful begin replayed a scope two generations old.
* **The automated offline render resolves every event up front.** A rejection
  used to leave the rig half-swept (a wah parked at 0.37, a pedal bypassed) with
  no way for the caller to know or undo it, while the export's comment reasoned
  only about half-processed buffers. An event naming ANOTHER SEAT's node is now
  rejected (node ids are engine-global; `player` read as a scope and was not
  one), `event_count` is capped at 262,144, and the render de-interleaves into a
  512-frame scratch instead of allocating two whole-take buffers (~115 MB for a
  5-minute stereo take).
* **MIDI-out close is a bounded join, then a LEAK.** It was
  `juce::Thread::stopThread(2000)`, which `TerminateThread`s on expiry — i.e. the
  one scenario the class exists to survive (a wedged interface) terminated the
  dispatcher inside the OS MIDI API and then destroyed the device under it.
* **A dropped MIDI-out message is counted and is not a bad argument.**
  `dca_midi_output_get_counts` (v35) reports it and `_send` / `_send_at` answer
  `DCA_ERR_DEVICE` for a full queue; the managed side gains
  `DroppedCount` / `ReadCounters()`. A batch of messages falling due in one
  dispatcher wake is now sorted by DEADLINE before it goes on the wire.
* **The limiter's skip branch is counted and honest.** When it fires the block
  reaches the driver and the streaming mirror unbounded; `skippedBlocks()` says
  so and `latencySamples()` drops to 0 rather than reporting 1 ms of a stage that
  is not in the path. `RtMasterLimiterSkipIsCountedAndReportsNoLatency`.
* **Seat-0 aliases** for the seven `_for_player`-only v34 exports, and
  **`IJuceMidiOutput`** — the seventh test seam, and the only member on any
  interface that no external implementer could satisfy.
* Smaller: `SetMasterLimiter` no-ops on a disposed engine like every sibling bus
  setter; `TryGetMidiTimingStats(out MidiTimingStats)` / `ResetMidiTimingStats()
  -> bool` mirror `TryGetXrunStats`; the automation factory accepts a 32-byte
  param id (the ABI always did); `ProcessOffline` with an empty buffer throws
  instead of silently doing nothing; `AudioEngine::setMasterLimiter` REJECTS a
  non-finite pair instead of substituting defaults, matching the export;
  `RollBackEditJournal` is O(n) rather than O(n²); the stale-scope warning is
  emitted outside `writeMutex_`.

### Rig

* **Edit scopes: a preset is ONE change.**
  `dca_engine_dsp_{begin,commit,abort,get_editing}_edit_for_player`, and
  `JuceDspChain.BeginEdit()` returning a `using`-shaped `JuceDspEditScope`.
  Every structure call used to publish its own ordering snapshot and take the
  engine's one-block barrier, so a twelve-pedal preset was twelve publications
  plus ~thirty parameter writes, each individually audible — the board
  assembled itself pedal by pedal, and v32's parked insertion only ever covered
  one node. Inside a scope the writes accumulate against a shadow chain (reads
  report the shadow, so a caller can build a preset and read it back), added
  nodes are parked and fade in together, removed nodes keep playing and fade
  out together, and the commit costs **at most two publications whatever the
  preset size** with every ramp in one ~10 ms window. Disposing the scope
  without `Commit()` aborts it — membership, order, bypass flags and branch tags
  come back; parameter VALUES are not journalled. A scope is per seat, and
  `dca_engine_destroy` ends an open one.
  `RtPresetCommitPublishesOncePerPresetNotOncePerNode` counts the publications
  against the unscoped baseline while the callback runs; `DspEditScopeTests`
  pins the audible half offline.

* **A DROPPED edit scope no longer freezes the seat's rig.** `editing_` was
  cleared by no other managed call and the rigs are value members of
  `AudioEngine`, so a caller who forgot the `using` left every later add /
  remove / move / bypass piling up in the shadow, returning success, and never
  reaching the audio callback again — for the life of the ENGINE. Three
  changes, all of which must stay: a nested `begin` now **aborts** the stale
  scope and logs a Warning instead of returning `DCA_ERR_INVALID_ARG` (the
  refusal was correct for a transaction and unrecoverable in practice);
  `JuceDspEditScope` has a **finalizer** that aborts the scope, logs at Error
  and names it by `ScopeId`; and `JuceDspChain` marks a superseded scope closed
  so its late `Dispose` cannot abort the scope that replaced it.
  `Wave2EditScopeLeakTests` drives the leak through a real GC.

* **An aborted scope now rolls back the wrapper's identity map too.**
  `abortEdit` destroys the nodes a scope added and restores the ones it removed;
  managed-side, `JuceDspChain._known` did neither. So `TryGetNode` kept
  resolving a destroyed node — a phantom whose poll-shaped members answer
  `false` / `""` / `default` / `-1`, which reads to a UI as a real pedal that is
  bypassed and silent — while a node removed inside the scope came back as a
  FRESH `JuceDspNode`, breaking the reference identity `Nodes` documents. The
  chain journals both halves for the length of a scope and replays them on
  abort; a node the same scope both added and removed is not resurrected, and a
  commit drops the journal without replaying it.

* **Branch delay compensation at the merger.**
  `dca_engine_dsp_{set,get}_branch_pdc_for_player` and
  `JuceDspChain.BranchLatencyCompensation`, **on by default**. A parallel region
  whose branches carry different latencies is a comb filter, not a blend: the
  merger summed `x[n]` from an empty branch with `x[n-L]` from one holding a
  pitch shifter or an oversampled drive, and v23 accepted that while the chain
  was already computing the per-branch totals for
  `dca_engine_dsp_get_chain_latency` and throwing them away. `MergerModule` now
  carries one delay line per branch per channel — sized in `prepare()` for the
  worst case (8,192 samples), crossfaded when a delay moves, bit-exact at zero —
  and the delays are re-derived on every publish, every accepted param/string
  write and every prepare, so they follow a knob and a device change. The
  latency export therefore describes an ALIGNED region now. Off restores the
  pre-v34 sum byte for byte. An unmatched splitter auto-merges inside the chain
  and is not compensated. `DspBranchPdcTests` reads both cases off an impulse
  (one arrival at ~1.0 compensated, two at ~0.5 not);
  `RtBranchPdcFollowsALatencyChangeWithoutAllocating` flips a whammy's latency
  inside a live split region with the allocation hook armed.

* **Sidechain input on the rig.** `dca_engine_dsp_{set,get}_node_sidechain`,
  `dca_dsp_module_type_flags`, `JuceDspNode.Sidechain` /
  `JuceDspSidechainSource` and `JuceDspModuleInfo.SupportsSidechain`. Every
  module was single-buffer, so the compressor's "sidechain" was an internal
  100 Hz high-pass on its own input — a detector shape, not a sidechain. A
  module can now opt in (`DspModule::supportsSidechain` +
  `processSidechained`, whose default forwards to `process`, so all 57 existing
  cores are untouched) and be keyed from the seat's DRY input or the song mix:
  a compressor after a drive can follow the pick instead of the drive, and the
  rig can duck under the backing track. `compressor` and `gate` are wired.
  A source a module would ignore is REJECTED rather than stored, because a
  stored-and-ignored source is a control that does nothing; the capability is a
  separate flags export since `dca_dsp_module_type_info` cannot change
  signature, and the RT gate compares that table against constructed modules.
  An unavailable source falls back to the module's own input and never to
  silence — keyed from silence a gate sits closed. `DspSidechainTests` renders
  the same chain with one property changed;
  `RtSidechainKeyingIsAllocationFreeAndAdvertisedHonestly` covers the callback.
### Wrapper

* **CS1591 is enforced; every public member is documented.** The
  `<NoWarn>$(NoWarn);CS1591</NoWarn>` in `Directory.Build.props` is gone, which
  surfaced **252 undocumented public members across 42 files** in the six
  packable projects — all now carrying a real sentence (units, threading
  contract, what it throws, the `dca_*` export it maps to) rather than a
  restatement of the signature. `PublicApiDocsLintTests` keeps it that way with
  three checks: every public type/member found by reflection has a `<member>`
  entry in that assembly's **shipped** `.xml`; no summary is a placeholder
  (`Gets or sets the X.`, `The X.`, empty); and neither the props nor any
  packable csproj suppresses CS1591 again. Tests and examples set
  `GenerateDocumentationFile=false`, so the warning never reaches them.

* **BREAKING (within this unreleased cycle): `JuceAudioException`'s public
  three-argument ctor is now `(string? message, JuceAudioStatus status, string
  operation)`.** It shipped this cycle as `(status, operation, message = null)`
  — two ADJACENT strings, so transposing them compiled cleanly and produced an
  exception whose `Operation` was a sentence. The status now sits between them,
  matching the internal `Check` ctor, and a two-argument
  `(JuceAudioStatus, string operation)` covers the message-less case unchanged.

* **`JuceAudioSource` gained `IsDisposed`, and its ctor rejects a zero handle
  with `GC.SuppressFinalize(this)` before the throw** — the one handle wrapper
  that did not have that guard, safe only because nothing in it threw yet. A
  finalizer that throws is a process-wide FailFast, not a catchable error.

* **`JuceAudioSource.Loop` attaches BEFORE it swaps.** `JuceLoopRegion.Attach`
  throws when the region already belongs to a different source, and the setter
  had already stored the new region and disposed the old one by then: on that
  throw the property reported a loop, the old loop was dead, the new one was
  never armed, and nothing was looping. A rejected region now leaves the
  existing loop running.

* **`JuceAudioEngine.OpenFileAsync` / `LoadPluginAsync` no longer hand back a
  corpse.** The work item could complete and TRACK its child, `ShutdownWorker`
  join and return, and `DisposeChildren` then dispose that very child — so the
  awaiting caller got a live-looking wrapper whose every member throws. Both
  now fault the task with `ObjectDisposedException(nameof(JuceAudioEngine))`.

* **`Dispose()` no longer joins the async worker for 60 s.** It passed the
  handle-drain deadline, and the call in flight can be
  `ShowAsioControlPanelAsync` — a MODAL vendor dialog that returns when the USER
  closes it — so disposing on the UI thread while it was open froze the app for
  a minute. The join is now `EngineWorker.DisposeJoinTimeoutMs` (2 s); the drain
  behind it already handles a wedged call honestly (refuse to free, log at
  Error, leak). `ShowAsioControlPanelAsync` documents the residual block.

* **`CoalescingProgressStream` no longer disposes its CTS under a running
  worker.** Abandoning the enumeration (a `break`, which disposes the
  enumerator) cancelled and then disposed the linked source without awaiting the
  worker, leaving the backend on a token whose source was gone —
  `CancellationToken.WaitHandle` throws outright on one of those, and both
  `CreateLinkedTokenSource(ct)` and `Task.Delay(250, ct)` are on exactly this
  token in the python path. It now cancels with `CancelAsync` (so the backend's
  callbacks do not run synchronously on the consumer's — often UI — thread) and
  disposes the source from a continuation on the worker. The class doc now says
  out loud that abandonment is fire-and-forget: `DisposeAsync` returns while the
  child process and the GPU gate slot may still be held.

* **`JuceDspChain.TryReadMeters(dest, out int nodeCount)`** — a real `Try`. The
  `int`-returning overload is named `Try*` and cannot report failure: a `0`
  means either "this seat has no nodes" or "the engine is gone", which is
  precisely the distinction its sibling `TryReadMeter` exists to make. The old
  overload is kept (it is what `IJuceDspChain` declares) and documented as
  unable to answer. `TryGetNode` also gained `[MaybeNullWhen(false)]`, which it
  needed from the day it was written, and `Nodes` now says loudly that it is a
  snapshot costing two P/Invokes and an allocation per get.

* **Rig gaps the app found when it adopted `JuceDspChain`.**
  `JuceDspChain.TryGetNode(id, out node)` — callers address the rig by ID (a
  preset, a UI row, an undo entry all store the id), so every one of them kept a
  second id → node map beside this type's; O(1), with one `get_node_ids` probe
  for a node this chain did not create. A **planar**
  `ProcessOffline(left, right, sampleRate, outLeft, outRight)` — the export is
  planar and in place, so the interleaved overload de-interleaves and
  re-interleaves, and the output spans may alias the inputs for a zero-copy
  render. `TryReadMeter(id, out meter)`, which distinguishes **no reading** from
  **silence**: `JuceDspNode.Meter` returns four zeros for both, and that ambiguity
  is why callers bound the raw-handle `JuceDspMeters` façade alongside the chain
  to ask `IsEnabled`. Both questions are now answered under one pin of the engine
  handle. `JuceDspMeters` is *not* `[Obsolete]` — DotCharter still calls it and
  builds with warnings as errors.
* **`HtdemucsChunkScratch`'s dead `bins` argument is gone.** Every plane sizes
  itself `nFft / 2 + 1` from the transform, so a `bins` that disagreed with
  `nFft` was silently ignored. The 7-argument constructor remains as a forwarder
  (likewise not `[Obsolete]`, same reason) and the geometry is readable as
  `HtdemucsChunkScratch.Bins`.
* **Test seams: `IJuceAudioEngine`, `IJuceAudioSource`, `IJuceAudioPlugin`,
  `IJuceMidiInput`, `IJuceDspChain`, `IJuceDspNode`.** Every wrapper type is
  sealed with an internal ctor, which left a consumer unable to unit-test the
  code that *drives* the engine without `juce_sharp.dll` and a sound card. The
  interfaces cover the public instance surface (statics cannot be made
  substitutable); the factory members answer in interface terms as *explicit*
  implementations, so no existing signature changed. A reflection lint walks both
  directions, so a member added to a class and forgotten on its interface fails.
  See [Testing without the native DLL](guides/testing-without-native.md).
* **Async variants of the seconds-long calls.** `ReconfigureAsync`,
  `SetDeviceTypeAsync`, `ShowAsioControlPanelAsync`, `OpenFileAsync`,
  `LoadPluginAsync`, and the static `GenerateWaveformAsync`,
  `GenerateWaveformFromMemoryAsync`, `RenderToWavAsync`. Work runs on ONE
  dedicated long-running thread per engine — never the thread pool, where a 25 s
  VST3 scan or an open modal ASIO panel makes the pool inject threads while
  everything else starves — and the queue is serial, so two device changes cannot
  overlap. Cancellation is honoured before the call starts and documented as
  impossible after it. `Dispose` stops the worker *before* the handle drain and
  faults every queued-but-unstarted call with `ObjectDisposedException`.

---
### Dsp / AudioIo

* **FFT-based YIN: `JuceChromaticTuner(..., useFftDifference: true)`.** The
  difference function is O(W x tau_max) - 2M multiply-adds per 256-sample legato
  hop in `JuceGuitarAnalyzer`, the dominant cost of the offline audio-to-chart
  pass. Expanding the square turns it into a cumulative-energy prefix plus one
  linear autocorrelation, computed with one zero-padded real FFT, a power
  spectrum and one inverse. **Measured x9.3 in Release** (1.893 -> 0.203 ms per
  detection at window 2048; x5.1 in a Debug build).
  **Opt-in, because it is NOT bit-identical** - the FFT's rounding error is
  relative to the window's total energy while `d(tau)` near its dip is near zero.
  Equivalence is pinned statistically instead: over 200 random 82-1000 Hz tones
  the worst disagreement is **0.0025 cents and 0.00000 confidence**, and over the
  synthetic corpus (including a bass low E both clamped and unclamped) 0.0018
  cents. `JuceGuitarAnalyzer` keeps the direct path by default - its event stream
  is a bit-identity contract - and `new JuceGuitarAnalyzer(sr, useFastYin: true)`
  switches all three internal tuners, producing identical events, positions,
  techniques and MIDI numbers with pitches 0.001 cents apart
  (`FftYinEquivalenceTests`). Both flags are constructor parameters, not `init`
  properties: they decide what the constructor allocates, and an init property
  would have to allocate lazily on the first call.

* **`WavWriter` — a streaming WAV writer with 16- and 24-bit PCM output.**
  `WavIo.WriteFloat32` takes the whole buffer at once and writes float32 only,
  so the package could READ 8/16/24/32-bit PCM and 32/64-bit float while writing
  exactly one of them. `WavWriter` opens a file (or any seekable stream) with a
  provisional header, appends `Write(ReadOnlySpan<float>)` / `WritePlanar` blocks
  allocation-free, and patches the RIFF / `data` / `fact` sizes plus the pad byte
  on `Dispose`. `WavSampleFormat` is `Float32 | Pcm16 | Pcm24`.
  * PCM scales by `2^(bits-1)` — the exact INVERSE of the reader's divisor — so
    the only round-trip error is a half LSB of rounding. Scaling by
    `2^(bits-1) - 1` "to avoid clipping at +1.0" adds a systematic `v / 32768`
    gain error on top: measured 4.4e-5 against a 3.05e-5 quantisation step, a
    correction larger than what it corrected. No dither; a non-finite sample is
    written as silence, because `(int)NaN` is 0 on x64 and `INT_MIN` elsewhere
    and an unguarded clamp passes a NaN through (`NaN > max` is false).
  * **An unfinished file is readable, not corrupt.** The provisional sizes are
    the RIFF unknown-length placeholder `0xFFFFFFFF`, not zero, so a process that
    dies before `Dispose` leaves a file `WavIo.Read` still decodes IN FULL.
    Zeros would have produced a valid-looking WAV declaring no audio — silent
    data loss with nothing to notice. `WavWriter.IsUnfinished` reports the state.
  * **RF64 is not written.** A write that would push the data chunk past 4 GB
    throws `NotSupportedException` BEFORE writing anything, so the file on disk
    stays consistent; discovering it at `Dispose` means the overflow is already
    written.

* **`JuceBeatDetector` gains a tempo map, downbeats and a real beat tracker.**
  `Result.TempoMap` is a list of `JuceTempoSegment(TimeSpan Time, float Bpm)`,
  `Result.Downbeats` + `Result.BeatsPerBar` (3 or 4) come from a
  beat-synchronous accent autocorrelation, and the three new `Result` members
  are optional constructor parameters so `new Result(bpm, beats)` still
  compiles.
* **BEHAVIOUR CHANGE: `Result.Beats` is now tracked, not a fixed grid.** The old
  grid stepped ONE global period from the first half-beat and snapped each step
  to a local envelope maximum, so it could not represent a song that speeds up
  and every beat past a tempo change drifted. It is now an Ellis-style dynamic
  program over a windowed LOCAL tempo estimate (log-Gaussian prior around the
  global lag, so a window that locks onto half tempo does not become a fake
  tempo change; chain ends with no onset evidence are trimmed, because the DP
  has no predecessor across the lead-in silence and its first beat would
  otherwise be an arbitrary silent frame — invisible in the beat list, a whole
  spurious segment in the tempo map).
  `Options = new JuceBeatDetector.AnalysisOptions { FixedGrid = true }` restores
  the old grid bit-for-bit; the default is `false`. **The parent DotCharter app
  does not reference `JuceBeatDetector` at all** (grepped across Analysis /
  Services / UI / Core / Tests), so nothing in-tree depends on the grid and
  defaulting the new behaviour on costs no consumer. `Result.Bpm` is the same
  global autocorrelation figure in both modes and did not change.
* `SegmentToleranceBpm` (default 2) is FLOORED by `1.5 x bpm^2 / (60 x fps)`,
  one envelope frame of inter-beat interval — 5.6 BPM at 120 BPM with the
  default 512-sample hop at 22 kHz. Asking for finer segments than that is
  segmenting on quantisation noise; halve `HopSize` to buy real resolution.

* **Streaming onset / event API without the per-call list.**
  `JuceOnsetDetector.Push(block, Span<JuceOnset>, out int)` and
  `JuceGuitarAnalyzer.Push(block, Span<JuceGuitarEvent>, out int)` write into a
  caller-owned span. Both are the SAME code path with a different sink — pinned
  bit-for-bit against the list overload by `StreamingSpanPushTests` — and
  overflow is DROPPED and counted (`DroppedOnsets` / `DroppedEvents`) rather
  than thrown, because a throw halfway through a block would leave the stream
  position past audio whose detections were never reported. Size with
  `RequiredOnsetCapacity` / `RequiredEventCapacity` and the counters stay at
  zero. `JuceGuitarAnalyzer` now drives the onset detector through the span form
  internally: boxing the `IReadOnlyList` enumerator allocated once per
  4096-sample chunk even when the list was `Array.Empty`.
* **The two report delays are published.** `JuceOnsetDetector.LatencySamples`
  (= `FftSize + HopSize - PositionOffset` = 896) and
  `JuceGuitarAnalyzer.LatencySamples` (= 4096 + 80 ms) say how far past a
  reported position the stream must advance before the detection comes out, so a
  caller can align to its own input timeline instead of guessing from the FFT
  size. `JuceOnsetDetector.PositionOffset` is public (was internal).

* **`JuceTunerStream` — a UI-grade tuner over `JuceChromaticTuner`.** `Detect`
  is a one-shot over a full window; a live needle also needs a ring, a fixed
  emit rate, an octave-slip filter and note hysteresis, and every caller was
  writing those four differently. `Push(ReadOnlySpan<float>)` accepts any block
  size and emits a `JuceTunerFrame` every `HopSize` samples with a 3-tap median
  on the frequency, a note that only changes after `NoteChangeFrames` agreeing
  readings (or immediately past `ImmediateChangeCents`), and a release hold so a
  pick-hand mute does not blank the display. **A clamped reading is never
  reported stable**: below `EffectiveMinFrequency` YIN returns the clamped lag
  as a *confident* reading, so `Clamped` is set from the reading's own flag OR
  from the frequency sitting within 5 % of the ceiling. Allocation-free after
  construction (`JuceTunerStreamTests.Push_DoesNotAllocate`); non-finite input
  is scrubbed and counted in `NonFiniteSamples`.

### Stems

* **A KIND-LESS verified download.** `VerifiedDownload` (public,
  `JuceSharp.StemSeparation.Runtime`) — `EnsureFileAsync(Uri, dest, sha256?,
  expectedBytes?, progress?, log?, ct)`, `VerifyFileAsync`,
  `ComputeSha256Async`, `ValidateSourceUrl`. Until now the only public route
  into the verified download path was `OnnxModelCatalog.EnsureAsync`, which
  demands an `OnnxModelEntry` carrying an `OnnxStemModelKind` — the inference
  DRIVER that consumes the file. A host with a ggml weight or a user `.ckpt`
  has no honest value for that field, so consumers labelled them "the nearest
  ONNX kind" purely to reach the downloader; nothing downstream reads `Kind`,
  so it downloaded fine and left a field that lies to the next reader. **Not a
  second implementation:** every method forwards to the package-internal
  `VerifiedDownloader`, and `OnnxModelCatalog.EnsureAsync` now calls
  `EnsureFileAsync` once per mirror, so the existing-file check, the per-URL
  `.part` resume, the length check and the whole-file hash before the atomic
  rename are literally the same code down both doors. One behaviour is new
  rather than moved: with no hash but a known size, an existing file of the
  WRONG length is now discarded instead of trusted.
* **`DemucsGgmlModelCatalog`** (`JuceSharp.StemSeparation.Runtime`) — the
  weights the NATIVE demucs.cpp backend loads finally have a catalogue:
  `All` / `FindById` / `Default`, `EnsureAsync`, `VerifyAsync`,
  `ValidateEntry`. Three entries, each pinned and each recording where its pin
  came from: `ggml-htdemucs-4s` (83,994,361 B), `ggml-htdemucs-6s`
  (54,855,129 B, 6 sources) and `ggml-hdemucs-mmi-v3` (167,294,211 B). Every
  hash is the `lfs.oid` published by
  `huggingface.co/api/datasets/Retrobear/demucs.cpp/tree/main`, read
  2026-09-11; the 6-source one additionally agrees byte for byte with a hash
  DotCharter computed independently on 2026-09-04 from an installed copy.
  A hand-written table elsewhere recorded that this repository publishes no
  oid **because it is a dataset repo** — dataset repos publish it under
  `api/datasets/` rather than `api/models/`, and that note left two of three
  weights unpinned.
* **`OnnxModelCatalog.ValidateEntry` is PUBLIC.** It decides where bytes land
  and where they come from, and it was reachable only through an
  `InternalsVisibleTo`. A host that synthesizes entries wants to reject a bad
  one where it builds it, not as an `ArgumentException` out of a download the
  user was already told had started. The transport rule itself moved into
  `VerifiedDownload.ValidateSourceUrl`, so the ONNX and ggml catalogues cannot
  drift apart on it.

* **Progress is now a PULL stream on every backend.**
  `IStemSeparator.SeparateWithProgressAsync` (a default interface method, so a
  third-party backend gets it for free),
  `OnnxStemSeparator.SeparateWithProgressAsync` and
  `JuceStemSeparation.SeparateWithProgressAsync` return an
  `IAsyncEnumerable<…>` a UI can `await foreach`, ending in a TERMINAL item
  that carries the run's result (`SeparationResult` / the ONNX stem buffers;
  the native façade writes files, so it has none). Built on the new
  `JuceSharp.CoalescingProgressStream`: a bounded, drop-oldest channel, so a
  slow consumer never stalls inference and never loses anything that matters
  — the terminal item is the newest write and cannot be evicted. Cancelling
  the token or abandoning the enumeration cancels the separation
  (`OperationCanceledException` out of `MoveNextAsync`, never a
  `StemSeparationException`), and a backend fault surfaces with its ORIGINAL
  exception type, so a `StemSeparationVramException` still reaches a
  "retry on CPU" branch. The `IProgress<T>` overloads are unchanged.
* `StemSeparationProgress` gained two optional trailing members, `Backend` and
  `Result`. Four-argument construction still compiles.
* `juce-stems-diag` consumes the stream; its +1 %-movement progress throttle is
  gone (the transport coalesces). Exit codes and output format are unchanged.
* `JuceSharp.StemSeparation.Onnx` now declares ProjectReferences on
  `JuceSharp` and `JuceSharp.StemSeparation` — both were arriving transitively
  through `JuceSharp.AudioIo` while this package used types from each.
* **`OnnxModelCatalog`: known ONNX models, pinned and resumable.** Nine entries
  (HTDemucs 4s/6s/ft, BS-RoFormer fp16, five UVR MDX-Net), each with a SHA-256
  read from the Hugging Face repository API on 2026-09-11 and a mandatory
  `HashSource` recording where it came from.
  `EnsureAsync(kind, targetDir, progress, allowUnverified, log, ct)` downloads
  if needed, VERIFIES the complete file before the atomic rename, re-downloads
  an existing file that fails its hash instead of trusting it, resumes an
  interrupted transfer from a `.part` over HTTP `Range` (falling back to a full
  download when the server answers 200 rather than 206), and keeps the partial
  on cancellation. Plus `VerifyAsync` and `ComputeSha256Async`. An entry with
  no pinned hash is refused unless the caller passes `allowUnverified: true`.
* **One downloader.** `PythonEnvManager.DownloadWithProgress` and
  `VerifyFileHashAsync` are now forwarders to the package-internal
  `VerifiedDownloader`, the only place either stems package touches HTTP —
  pinned by `HttpDownloadLintTests`, which also asserts the
  verify-before-rename order. Behaviour of the Python bootstrap is unchanged
  (non-resumable temp file, deleted on any failure).
### Engine
* **`GetMasterLimiter()` is documented as the POST-CLAMP read-back.**
  `SetMasterLimiter` said the ceiling is clamped to [−24, 0] dBFS and stopped
  there. The engine clamps on the message thread and stores the CLAMPED pair
  (the ceiling as a linear value), and the getter derives its dB from that — so
  a request of +5 dBFS reads back as 0, and a Settings row must render from
  `GetMasterLimiter()` rather than from the number it wrote to its own config,
  or the UI shows a ceiling the audio path is not using. No behaviour change;
  `MasterLimiterAbiTests` gained the +5 dB case, which is a number a user
  actually types where the existing +12/100,000 ms case reads as a boundary
  curiosity.
* **Master-bus ceiling.** A look-ahead soft limiter over the FINAL device
  buffer, on at a −0.3 dBFS ceiling once the host enables it (it was ON by
  default in v34; **v35 made it OFF at engine start** — see the wave-3 entry at
  the top), placed after the last contributor and
  *before* `mirror_.pushBlock`. Until now that sum — song bus + every seat's
  monitor + click + both instrument legs — was only NaN-scrubbed and never
  bounded: a hardware DAC clips the over-range samples, but the streaming
  mirror carries FLOAT, so they survived into OBS / Discord's encoder and
  wrapped there. `dca_engine_set_master_limiter` /
  `dca_engine_get_master_limiter` /
  `dca_engine_get_master_limiter_reduction_db` (poll-only, like the v30 node
  meters) / `dca_engine_get_master_limiter_latency_samples`. Managed:
  `SetMasterLimiter` / `GetMasterLimiter` / `MasterLimiterReductionDb` /
  `MasterLimiterLatencySamples`.
* **Offline render with PARAMETER AUTOMATION.**
  `dca_engine_dsp_process_buffer_automated_for_player` takes an interleaved
  buffer (processed in place) and a sorted `dca_dsp_automation_event` list
  (`{frame, node_id, param[32], value}`, 48 bytes, offsets pinned by
  `static_assert` and a managed layout test). The render is chunked at each
  event boundary, so an event at frame F is heard BY sample F, and every event
  goes through the same `DspChain::setParam` mailbox the live callback's
  setters use. `"bypass"` is a reserved param id and routes to the chain's
  bypass. An unsorted list, a non-finite value, an out-of-range frame or an
  unresolvable node/param is rejected BY INDEX with the buffer left untouched.
  Managed: `JuceDspChain.ProcessOffline(buffer, channels, sampleRate, events)`
  and `JuceDspAutomationEvent`.
* **Sample-accurate MIDI to the instrument bus.** Every event used to be
  stamped at sample 0 of whichever block drained it — up to one whole buffer of
  skew (5–10 ms) on every note — and the comment that justified it asked for a
  free-running sample counter on the audio thread that ABI v26 then shipped.
  `dca_engine_send_midi_at(engine, packed, render_frame)` parks the event until
  the block containing its frame and places it on that exact sample, the same
  mechanism v26's scheduled clicks use. `dca_engine_get_midi_timing_stats` /
  `_reset_midi_timing_stats` report `scheduled` / `late` / `dropped`. Managed:
  `SendMidi(status, d1, d2, renderFrame)`, `GetMidiTimingStats()`,
  `ResetMidiTimingStats()`. *Known gap:* the out-of-process VST sandbox leg is
  still flat — its shared mapping has no room for an offset.
* **MIDI hardware OUTPUT.** `dca_midi_output_enumerate` / `_open` / `_send` /
  `_send_at` / `_get_sent_count` / `_close`, and managed `JuceMidiOutput` +
  `JuceAudioEngine.EnumerateMidiOutputs()` / `OpenMidiOutput(index)`. Dispatch
  is one dedicated high-priority thread per device that sleeps far from its
  deadline and busy-spins the last ~1.5 ms; `juce::MidiOutput::sendBlockOfMessages`
  was rejected because it locks on the CALLER's thread and takes millisecond
  deadlines. `_send_at` takes the ENGINE as a parameter rather than caching one,
  so an output can never outlive an engine it holds a pointer to, and its
  deadline carries the output latency so a note lines up with what is HEARD.
* **`dca_engine_get_output_latency_samples` now includes the limiter's
  look-ahead.** It answers "how late does audio handed to the engine reach the
  ear", and the limiter is a real stage in that path. Switched OFF the limiter
  is not in the path at all, so the figure goes back to the device's own — a
  caller that aligns scheduled clicks (ABI v26) against it must RE-READ it after
  a toggle rather than cache it for the session.

---

## 0.9.0 cycle — the ABI v33 increment

### Added
* **Rig node introspection.** `dca_engine_dsp_get_node_type` and
  `dca_engine_dsp_get_bypass` expose two things the chain has always known and
  never told anybody. `JuceDspNode.TypeId` and `JuceDspNode.Bypass` now read
  them, so a node built by another binding against the same engine reports its
  real type and bypass state instead of an empty string and a hard `false`.
  `Bypass` is the message-thread INTENT, never the crossfade position. A node id
  naming a seat past `DCA_MAX_PLAYERS` is rejected, never clamped.
* **Module categories.** `dca_dsp_module_type_category` gives every catalogue
  entry one word out of a fixed set the ENGINE owns (`dynamics`, `drive`, `amp`,
  `cab`, `modulation`, `delay`, `reverb`, `pitch`, `filter`, `eq`, `utility`,
  `routing`, `synth`). `JuceDspModuleInfo.Category` was permanently empty before
  this and is populated now. Assigned per module in
  `src/dsp/DspModuleFactory.cpp`; a missing or off-vocabulary one fails the
  native `tone_regressions` gate.
* **Multi-component VST3 bundles.** `dca_plugin_count_vst3_components` and
  `dca_plugin_load_vst3_component`, plus
  `JuceAudioEngine.CountVst3Components(path)` and
  `LoadPlugin(path, componentIndex)`. One `.vst3` may declare several plugins;
  this ABI took descriptor 0 and nothing else, so every component past the first
  was unreachable and unreported. `dca_plugin_load_vst3` is now exactly
  component 0, and the sandboxed child's `load` / `loadInstrument` pipe ops take
  an optional `component` property (JSON, not the shared-memory header, so
  `kBridgeVersion` is unchanged at 2). An index the bundle does not have is
  REJECTED, never clamped onto component 0.

### Changed
* **`dca_source_set_volume` clamps to `[0, +inf)` and `dca_source_set_pan` to
  `[-1, +1]`**, on both sides of the ABI, with the managed cached getters
  mirroring the clamp. Neither was clamped at the boundary before. A negative
  gain is a phase inversion rather than a quieter source; volume keeps NO upper
  bound because boost above unity is a shipped feature. A non-finite value is
  still rejected.
* `JuceConstantQ` no longer allocates in the streaming steady state: the noise
  floor's percentile sort uses persistent scratch instead of a fresh
  `float[count]` per emitted frame, and a `Reset` that does not change the ring
  size reuses (and clears) its buffers instead of rebuilding ~390 arrays. Output
  is bit-identical; pinned by `JuceConstantQAllocationTests`.
* The 384-frame ring, the 0.05 noise percentile and the 0.10 relative floor are
  named constants in `LiveCqtConfig` with the same values, so no shipping number
  is re-typed inside `JuceConstantQ`.

## 0.9.0 cycle — the ABI v32 increment (after 0.8.0)

### Added
* `JuceSharp.Dsp.JuceDspMeters` / `JuceDspNodeMeter` — the first managed
  surface over the ABI-v30 per-node meter polls (`SetEnabled`, `IsEnabled`,
  `TryRead`, `Read`). Static and `nint`-taking; the caller owns the engine
  handle's lifetime and the metering enable.
* **Dual cabinet IR slots** — two independently controlled impulse responses
  in the cab module, with `juce_sharp_dualcab_test` as its native gate.
* **Delay tempo sync and real bypass trails** — both persisted, both default
  **off**, and the legacy delay output stays bit-exact. `DelayModule` is the
  only module that owns its own bypass processing: it drains history at zero
  chain weight while fresh dry passes through. Gated by
  `juce_sharp_delay_sync_trails`.
* Branch mute and solo are persisted in the merger's parameters.

### Changed
* NuGet packages updated and the test project moved to the **xunit.v3 4.0.0**
  runner under Microsoft.Testing.Platform (`global.json`). `dotnet test` now
  needs `--project <test csproj>` rather than the solution.
* All six example projects brought up to wrapper 0.8.0 / ABI v32, and a new
  `JuceSharp.Examples.ToneRig` added.

### Fixed
* Tone-engine snapshot publication and child-teardown completion.
* Overdrive latency metadata is kept owner-thread local.

---

## 0.8.0 — 2026-09-05 (ABI v32)

### Added
* **Parked DSP node insertion.**
  `dca_engine_dsp_add_node_bypassed_for_player` creates a regular node at zero
  wet so a live preset can be built up node by node: configure it, then
  `set_bypass(false)` fades it in over 10 ms. The existing add exports stay
  immediately wet for offline compatibility. Splitter/merger topology still
  switches instantly — this does not make a whole preset change atomic.

---

## 0.7.x — ABI v21 … v31

The long DSP release. 0.7.0 was tagged at ABI v21 on 2026-07-10; everything
below it landed on `main` afterwards and shipped as part of 0.8.0.

### ABI v31 — per-seat drop tune (2026-08-08)
* `dca_engine_set_player_input_pitch_shift{,_mode}` /
  `JuceAudioEngine.SetInputPitchShift(int player, …)`. Until v31 the engine
  held **one** shifter applied to the raw device input *before* the per-player
  split, so on a couch with two guitars in different tunings, dropping one
  dropped both. The two pre-v31 exports are now exactly their per-player form
  with `player = 0`.

### ABI v30 — meters and dropout counters (2026-08-08)
* Per-node DSP meters and latency: `dca_engine_dsp_set_metering[_for_player]`,
  `get_metering`, `get_node_meter`, `read_node_meters`, `get_node_latency`,
  `get_chain_latency[_for_player]`. **Poll-only and off until asked for.**
  Chain latency is the *max* over a split region's branches, not the sum.
* A real dropout counter, measured in the audio callback:
  `dca_engine_get_xrun_stats` / `reset_xrun_stats`, surfaced as
  `JuceAudioEngine.TryGetXrunStats(out XrunStats)`. `LateCallbacks`,
  `DeadlineOverruns` and `NearMisses` are three different faults;
  `DeviceXruns` is `null` on a driver that cannot report.
* Eight `prepare()`-time faults in DSP cores fixed alongside.

### ABI v29 — master parametric EQ (2026-08-03)
* Six typed bands, a route, a trim and a spectrum tap:
  `SetEqBand`, `SetEqEnabled`, `SetEqSolo`, `SetEqRoute`, `SetEqTrim`,
  `GetEqActiveCount`, `ReadEqSpectrum`, plus `EqBandCount = 6`. Everything is
  off at engine start, and a band is published only when it is *audible* —
  decided once on the message thread, so the active-count export and the audio
  callback are one predicate rather than two copies of one.

### ABI v28 — per-player live-input rigs (2026-08-01)
* Up to `DCA_MAX_PLAYERS` (4) independent "seats" sharing **one** open device:
  each with its own DSP chain, VST3 input chain, mono capture ring and
  hardware input channel. `SetPlayerCount`, `GetPlayerCount`,
  `Set/GetPlayerInputChannel`, `SetPlayerMonitorGain`,
  `GetAndResetPlayerInputPeak`, the four per-seat capture reads,
  `dsp_*_for_player`, `dca_plugin_attach_to_input_for_player`,
  `dca_plugin_get_input_player`, `dsp_process_buffer_for_player`.
* Node ids became engine-global — seat `p` hands out
  `p * DCA_PLAYER_NODE_ID_STRIDE + local` — which is what kept the ABI delta
  small: node-scoped exports kept their signatures and resolve the seat from
  the id.
* `playerCount_` defaults to 1, and at 1 the callback runs the pre-v28 path
  byte-for-byte.

### ABI v27 — declared discrete params (2026-07-14)
* `dca_dsp_module_param_step`, and value labels for every enum param, so a UI
  can never park a knob at "6.5 stages" while the core runs 6.

### ABI v26 — sample-accurate clicks (2026-07-14)
* `dca_engine_get_render_clock`, `schedule_click`, `clear_scheduled_clicks` →
  `TryGetRenderClock`, `ScheduleClick`, `ClearScheduledClicks`. The render
  clock is published immediately after the transport pull, so it and
  `AudiblePosition` describe the same output sample.

### ABI v25 — CSVD drop tune (2026-07-12)
* The drop-tune shifter was replaced with **CSVD** (`dcdsp_droptune.h`),
  down-only. `set_input_pitch_shift_mode`'s meaning changed here. Follow-up
  work took 32–88 dB of warble out and fixed chord behaviour
  (41.8 dB → 4.8 dB from native).

### ABI v24 — enum value labels (2026-07-11)
* `dca_dsp_module_param_label_count` / `_param_label`, feeding the model-picker
  params. Landed with a 24-model amp roster and a 12-model cab bank.

### ABI v23 — split / parallel chains (2026-07-11)
* Splitter + merger modules and per-node branch tags:
  `dca_engine_dsp_set_node_branch` / `get_node_branch`.

### ABI v22 — string node parameters (2026-07-11)
* `dca_engine_dsp_set_node_string` / `get_node_string` plus the string-param
  catalogue exports, which is how user IR paths reach `cab` and `acousticsim`
  and how NAM/AIDA-X model paths reach `namamp`.

### ABI v21 — the in-house DSP tone rig (2026-07-10, tagged 0.7.0)
* `DspModule` / `DspChain` / `DspModuleFactory` and the whole
  `dca_engine_dsp_*` surface — a reorderable effect chain ahead of the input
  VST chain, with a module catalogue over header-only `dcdsp` cores. It grew
  from 12 modules at first commit to the current catalogue over the following
  days, and gained `namamp` (NAM / AIDA-X neural amp capture over
  `mikeoliphant/NeuralAudio`) and both Miku vocal stomps.
* Structure changes are **crossfaded by the chain, never by a core**: bypass,
  un-bypass, move and remove all ramp over 10 ms.

---

## 0.7.0 groundwork — ABI v14 … v20

### ABI v20 — `dca_generate_waveform_from_memory` (2026-07-10)
* `JuceAudioEngine.GenerateWaveformFromMemory(byte[], double blockMs)` — peaks
  from a blob you already hold, skipping a second end-to-end disk read.

### ABI v19 — out-of-process VST bridge (2026-07-05)
* `dca_engine_set_remote_bridge` / `get_remote_bridge_stats` →
  `SetRemoteBridge`, `TryGetRemoteBridgeStats`, plus the sandboxed
  `juce_vst_host.exe` child and its shared-memory bridge, so a plugin that
  crashes cannot take the host process down.

### ABI v17 / v18 — the drop-tune pedal (2026-07-05)
* `dca_engine_set_input_pitch_shift` and then a time-domain shifter with
  transient bypass, on a dedicated mono path.

### ABI v15 / v16 — capture ring cursor and clock sync (2026-07-04)
* `read_input_capture_stream` (its own cursor, only unconsumed samples),
  `read_input_capture_stream2` (with the absolute stream position) and
  `get_input_capture_sync` (atomic {captured samples, host ticks}).
* The ring was deepened to ~2.7 s so live detection survives a stem load.

### ABI v14 — editor-closed notification (2026-07-03)
* `dca_plugin_set_editor_closed_callback` →
  `JuceAudioPlugin.EditorClosedByUser`, plus an input-chain unload barrier.

### Also in this window
* **Detection v8** — `JuceConstantQ`, `JuceChartVerifier`, `JuceLeadTracker`
  and `LiveCqtConfig` in `JuceSharp.Dsp`, with an IDMT-SMT-GUITAR_V2 accuracy
  evaluation. The evals are opt-in via `GND_IDMT_ROOT`.
* Handle drain: in-flight P/Invokes are drained before a source, plugin or
  engine handle is destroyed.

---

## 0.6.x — 2026-06-01 (ABI v13)

### Added
* **Host-scoped enumeration** — `dca_enumerate_host_devices` and
  `dca_enumerate_host_types` list every `AudioIODeviceType` and its devices
  **without opening a device**, which is what makes `JuceDeviceCatalog` a
  single cheap call and host swapping instant.

### Fixed
* 0.6.1–0.6.3: always walk the device list on an open failure, in `start()`
  and `set_device_type` alike.

## 0.5.x — 2026-06-01 (ABI v12)

### Added
* `JuceDeviceCatalog` + `JuceAudioHost` — every host with its inputs and
  outputs in one call.
* `JuceEngineFactory.OpenWithFallback` + `OpenResult` — JUCE otherwise picks
  the host's first device, which is frequently an offline ASIO interface.
* `JuceVst3Scanner` + `Vst3Entry` — a file-system scan for VST3 bundles.
* `ChannelMask` helpers, and `Input/OutputChannelMask` on `JuceEngineConfig`.
* `JuceSharp.Dsp.JuceNoteStream`.
* **In-place host swap** — `dca_engine_set_device_type` /
  `JuceAudioEngine.SetDeviceType`.

## 0.4.x — 2026-05-23 … 2026-06-01

### Added
* **ABI v10** — `dca_plugin_get_latency_samples` /
  `JuceAudioPlugin.LatencySamples` (v0.4.0, 2026-05-23).
* **ABI v11** — the live input **capture ring**:
  `dca_engine_set_input_capture_channel` and `read_input_capture`, dry and
  pre-gain and pre-chain, so analysis sees the guitar rather than the
  processed monitor (v0.4.5).
* The Avalonia 12 example, which exercises the full managed surface.

## 0.3.x — 2026-05-20 … 2026-05-23

### Added
* **ABI v4** — VST3 parameters, hardware MIDI input and an offline render:
  * `JuceAudioPlugin.ParameterCount` / `GetParameter` / `SetParameter` /
    `GetParameterInfo` / `EnumerateParameters`, and `JuceParameterInfo`
    (`Index`, `Name`, `DefaultValue`, `NumSteps`, `IsDiscrete`).
  * `JuceMidiInput` + `JuceMidiMessage`, `JuceAudioEngine.EnumerateMidiInputs()`
    and `OpenMidiInput(int, Action<JuceMidiMessage>)`. **Input only** — no MIDI
    output was added, then or since.
  * `JuceAudioEngine.RenderToWav(src, dst, tempo, semitones, sampleRate)` — a
    decode and a 16-bit PCM WAV write. It throws `NotSupportedException` if
    asked to stretch or shift, which it never performed.
* **ABI v5** — native AI stem separation (`dca_stems_*`), surfaced as
  `JuceStemSeparation` in the core package. Compiled in only with
  `-DDOTCHARTER_STEMS_DEMUCS_CPP=ON`, which is off by default.
* **ABI v6** — `dca_decode_audio_file` / `dca_decode_free`, and the
  `JuceSharp.Dsp` and `JuceSharp.AudioIo` projects split out.
* **ABI v7** — `dca_device_info.type_name` and
  `dca_engine_get_output_latency_samples`, which is what makes WASAPI Shared /
  Exclusive / Low-Latency distinguishable at all.
* **ABI v8** — `dca_engine_reconfigure` / `JuceAudioEngine.Reconfigure`.
* **ABI v9** — the shared **stem-group** stretcher:
  `dca_source_set_stem_grouped`, `dca_engine_set_stem_group_tempo` / `_pitch`.
* v0.2.5 (2026-05-23) vendored the Steinberg ASIO SDK 2.3.3 into
  `third_party/ASIOSDK2.3/` under Steinberg's open-source licensing agreement.

## 0.2 — 2026-05-19 …

### Added
* `JuceAudioSource.Mute` — gates volume without losing the user value.
* `JuceAudioSource.Loop` — assignable `JuceLoopRegion`; a **polling** A-B loop
  driven by audible position (~5 ms precision). There is still no
  sample-accurate native loop.
* `JuceLoopRegion` (new) — `IDisposable` polling loop region.
* `JuceMixer` + `JuceMixerChannel` (new) — a managed multi-source facade with
  mute / solo / volume / pan. It owns no native handle.
* `JuceAudioPlugin.Bypass` — detach / re-attach from the input chain.
* `JuceAudioPlugin.SaveState()` / `LoadState(byte[])` — VST3 preset blobs.
* `JuceSharp.Dsp` as its own project: `JuceFft`, `JuceChromaticTuner`,
  `JuceBeatDetector`, `JuceChromagram`, `JuceChordDetector`.
* Docs: DSP overview + per-type pages, loop & mixer API pages.

### Added (0.1.x)
* GitBook documentation under `docs/`.
* `JuceAudioPlugin` — safe RAII wrapper around a loaded VST3 plugin.
* `JuceLog` static event for global log capture.
* `JuceConsoleLogger` — default sink used when no `ILogger` is supplied.
* `JuceAudioStatus` public enum.
* `JuceAudioException.Status` and `JuceAudioException.Operation`.
* `JuceEngineConfig.Validate()` with friendly `ArgumentOutOfRangeException`s.
* `JuceEngineConfig.EnableConsoleLog`.
* `JuceAudioEngine.LoadPlugin(string)` returning the new wrapper.
* `JuceAudioEngine.SetMirrorOutput(string?)` (alias of `SetMirrorDevice`).
* Native: `emitLog` forwards to stdout/stderr when no managed callback is
  registered, gated by `JUCESHARP_CONSOLE_LOG`.
* Native: `guard("op", lambda)` turns a stray C++ exception at an ABI entry
  point into `DCA_ERR_INTERNAL` plus a logged line.

### Changed
* The log trampoline is exception-safe — a managed handler's exception is
  caught and never propagates back into native code.

### Fixed
* Previously silenced native failures in `LoadVst3`, `SetMirrorDevice` and the
  other fire-and-forget setters are logged.

## 0.1.0 — 2026-05-19

Initial JUCE-based release. JuceSharp was extracted from DotCharter as part of
it (`9ed93e6`, *"audio: extract DotCharter.Audio + .Native into standalone
JuceSharp library"*).
