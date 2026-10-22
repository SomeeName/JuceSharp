# JuceSharp Agent Guide

Reusable .NET wrapper around a JUCE-based native audio engine (`juce_sharp.dll`),
extracted from DotCharter so any .NET app gets low-latency device I/O, transport
with Rubber Band stretch/pitch-shift (PLAYBACK only — see the drop-tune rule
below), VST3 hosting (input bus + MIDI VSTi), streaming-mirror output, DSP and stem
separation. **This is a git submodule — a separate repo with its own release
cadence.** Full usage docs: `README.md` and `docs/` (also `src/JuceSharp/README.md`).

**This guide is a ROUTER, not a reference.** It auto-loads for every agent that
opens a file in this repo, so it carries only what everyone needs: the sub-project
map, the ABI/wrapper rules that bite from anywhere, the rebuild command, and the
ONE extra file to open for the area you are touching. Every ABI history, module
catalogue, measured number and hard-won invariant that used to sit inline is in
[docs/agents/](docs/agents/), moved VERBATIM — nothing was summarised away.
**Read the routed file before you touch that area**, and update it in the same
change when you change its subsystem. Every bare `*.md` name below is a file in
that one folder (`docs/agents/`); they are plain links rather than `@import`s on
purpose — an import would re-load the bytes and defeat the point.

## Layout + routing — one row per sub-project (`src/`)

| Sub-project | What lives there | Read this first |
|---|---|---|
| `JuceSharp/` | the thin, pure-C# P/Invoke wrapper (PackageId `JuceSharp`): `JuceAudioEngine`, `JuceAudioSource`, `JuceAudioPlugin`, and since 2026-09-11 the rig's managed face `Dsp/JuceDspChain` + `JuceDspNode` + `JuceDspModuleCatalog` (`engine.GetDspChain(seat)`; every rig export is declared in `NativeMethods` and every call pins the engine handle — `JuceDspChainWrapperTests`). Marshalling + lifetime only (`NativeHandleOwner`), no DSP | the wrapper rules below; `docs/api/juce-dsp-chain.md` |
| `JuceSharp.Native/` | the native C++ DLL (CMake, JUCE via FetchContent). Owns `AudioDeviceManager`, mixer, transports, the VST3 chain, and exports a **versioned C ABI** (`include/dotcharter_audio.h`, `DCA_ABI_VERSION`). ASIO compiles against the headers JUCE bundles — nothing vendored (2026-09-28). The out-of-process VST host child (`juce_vst_host.exe`) and its shared-memory bridge live here too | `native-engine-abi.md` for the versioned surfaces; `rt-safety-and-tests.md` before anything on the callback |
| `JuceSharp.Native/src/dsp/` | the in-house reorderable tone rig (`DspModule`/`DspChain`/`DspModuleFactory`, ABI v21; value labels v24) over the header-only `dcdsp` cores in `src/dsp/cores/`, the **101-entry module catalogue in a default build (re-measured 2026-10-04: 57 original rows in `DspModuleFactory.cpp` + 43 in `src/dsp/gear/<Category>Gear.cpp` + `rsamp` in `src/dsp/gear/RsAmpGear.cpp`, Rocksmith's own amp chains, DotCharter #88; 111 `dcdsp_*.h` headers of which 98 declare a `*Core` — `dcdsp_tonestack.h` is a helper, and `dcdsp_rsamp_types.h` / `dcdsp_rsamp_models.h` / `dcdsp_rsamp_table.h` are `rsamp`'s row list and run-time table, ABI v45 — no game data is compiled in, #25)** (each entry carrying a fixed-set `category` since ABI v33; `namamp` is the one neural module and sits behind `#if DOTCHARTER_DSP_NEURAL`, so a `-DDOTCHARTER_DSP_NEURAL=OFF` build has **100** — this row said 56 until 2026-09-12, a number left over from the pre-gear-wave catalogue, and `JuceDspChainWrapperTests`' floor had inherited it as a flat `>= 100` that FAILED the NEURAL=OFF build this line calls supported; the floor is now `100 + (namamp present ? 1 : 0)`), `namamp`, the two Miku stomps, and the CSVD drop tuner | `dsp-tone-rig.md` |
| `JuceSharp.Dsp/` | the managed DSP surface the app's audio-to-chart analysis and live note verification run on: `JuceGuitarAnalyzer`, `JuceFft`, and Detection v8's `JuceConstantQ` / `JuceChartVerifier` / `JuceLeadTracker` + `LiveCqtConfig` | `managed-dsp-detection.md` |
| `JuceSharp.StemSeparation/` + `.Onnx/`, `JuceSharp.AudioIo/`, `JuceSharp.Diagnostics/` | stem-separation backends (native demucs.cpp + ONNX), the child-process registry, device I/O and diagnostics | `stem-separation-processes.md` |
| `JuceSharp.Native/tests/rt/`, `tests/JuceSharp.Tests/` | the allocation/lock gate over the real device callback (`juce_sharp_rtcheck`), the null device, and the xunit suite | `rt-safety-and-tests.md` |

**Task routing** (the same answer, keyed by what you are doing):

| Doing this | Read this |
|---|---|
| adding or changing an ABI export | `native-engine-abi.md` (+ bump `DCA_ABI_VERSION`, rebuild) — it goes in `src/dotcharter_audio.cpp`, the one ABI translation unit |
| adding a DSP module, a param, a branch tag or a gear model | `dsp-tone-rig.md` |
| anything on the live-input pitch shifter | `dsp-tone-rig.md` § drop tune — read `dcdsp_droptune.h` before the code |
| touching note detection, the CQT or its shipping constants | `managed-dsp-detection.md` (never re-type a constant — change `LiveCqtConfig`) |
| anything that runs on the audio callback | `rt-safety-and-tests.md` (allocation hook + lock allow-list) |
| a python/child-process path, or a "flaky StemSeparator test" | `stem-separation-processes.md` |
| couch co-op seats, input channels or per-player capture | `native-engine-abi.md` § ABI v28 |
| the master-bus ceiling / limiter | `native-engine-abi.md`; `src/MasterLimiter.h` rule 1 first — it is OFF at engine start and the HOST opts in |
| hardware MIDI output, or the MIDI-out dispatcher thread | `docs/api/juce-midi-output.md` + `docs/native/threading.md` § 4 (one high-priority spinner per open device) |
| the automated offline render (`*_process_buffer_automated*`) | `native-engine-abi.md`; the whole event list is validated AND RESOLVED before a sample is rendered |
| the built-in guitar / bass voice (`src/dsp/voice/`, chart source voices) | `chart-voice.md` — the excitation bank is GENERATED with the synth's own loop maths; change both or neither |

## Hard-rule index — one line each, full text one file away

- **A Rubber Band stretcher that has run stays on until a seek (#32):**
  `RubberBandAudioSource` takes its unity fast path only from a CLEAN stretcher;
  dropping to it mid-song skipped the read-ahead, coming back replayed stale
  audio. Resets happen on the control thread with the move
  (`repositionInner`), never on the callback — R3's `reset()` allocates. R2 is
  prepared at the 0.25x floor. `RtSpeedCrossingUnityIsSeamless*`.
  → `rt-safety-and-tests.md`

- **The shipped native pair links the MSVC runtime STATICALLY (#176):**
  `juce_sharp.dll` and `juce_vst_host.exe` import only Windows system DLLs; a
  dynamic CRT made v1.0.0 fail to load on a Windows without the VC++
  redistributable. `CMAKE_MSVC_RUNTIME_LIBRARY` is set once, before any target
  (`JUCESHARP_STATIC_CRT`, default ON), so JUCE, Rubber Band and NeuralAudio
  follow it; a test that links the shipping NeuralAudio uses
  `JUCESHARP_SHIPPING_CRT`, never a literal `MultiThreadedDLL`. The RT gate
  keeps its own debug CRT. Memory the DLL allocates is freed by its own `dca_*`
  export, never by the host (separate CRT heap). The guard lives in DotCharter
  (`NativeImportAuditTests`, the release preflight).
  → `docs/getting-started/installation.md`

- **ABI v43 does the load-time UPSAMPLE once, into a file (DotCharter #435):**
  `dca_convert_audio_for_device` writes a song at the device rate with the
  preload's OWN decode, `upsampleDecodedBuffer` and `Int16PreloadSource`
  quantiser, and stores EXACTLY what the preload holds: int16 samples plus the
  preload step in a `dcps` WAV chunk (`src/DeviceRateWav.h`) -- always 16-bit,
  nothing when the load would not convert. Every open honours the step (preload
  via `Int16PreloadSource::adoptStoredStep`, streaming via
  `StoredStepGainSource`) and REFUSES a damaged device-rate file. Opening the
  result is BIT-IDENTICAL and aligned. Change the preload's steps and
  `convertFileForDevice` together. `dca_source_get_preload_digest` is the test
  seam. `AbiV43Tests`, native `device_rate_wav`. → `native-engine-abi.md`

- **ABI v44 ENCODES Wwise-Vorbis `.wem` without Wwise (DotCharter #317):**
  `dca_wem_encode_vorbis` / `_free`, `JuceWemEncoder`. 48 kHz stereo only; the
  encoder REFUSES a setup header that is not the pinned Wwise one, because the
  copied Wwise constants are only valid for it. Do not change its quality or
  lowpass. Shares `src/wem/WemVorbisCommon.h` with the decoder. `AbiV44Tests`,
  `Wem/WemEncoderTests`. → `docs/agents/wem-encoder.md`

- **ABI v48: the note upload names its byte length, and untrusted inputs are
  capped:** `dca_engine_chart_source_set_notes_for_player2` checks count x stride
  against the buffer; a `.wem` is capped at 256 MiB and is NEVER streamed (a
  WEM that does not fit the preload budget is refused); `namamp` dry-bypasses a
  capture rate outside [8, 384] kHz instead of terminating on its load thread;
  `dca_plugin_save_state` refuses a state over INT32_MAX. `AbiV48Tests`,
  `RtWemSourceIsPreloadedNeverStreamed`. → `native-engine-abi.md`

- **ABI v42 decodes Wwise-Vorbis `.wem` NATIVELY (#426):** seven additive
  `dca_wem_*` exports, and every engine format manager opens a `.wem` as a source
  (decoded on the load thread, 16-bit preload). The decoder (`src/wem/`) is a
  ww2ogg header-rebuild port over JUCE's own libvorbis. Its output contract is the
  managed Ww2Ogg.Core + NVorbis path (same length and alignment, float within
  ~1e-6, 16-bit within 1 LSB). `wem_codebooks.h` is GENERATED
  (`tools/gen_wem_codebooks.py`). A `dca_wem_t` decodes on up to 4 worker
  threads, BIT-IDENTICAL to the serial decode (DotCharter #434, no ABI change).
  `AbiV42Tests`, `Wem/WemDecoderTests`, `Wem/WemCorpusTests`,
  `Wem/WemParallelDecodeTests`.
  → `native-engine-abi.md`

- **ABI v40 adds INSTRUMENT MODELS to the chart voices and RELEASES a disabled
  source's voice + sample pack (#50, #61).** Four additive exports and six fields
  appended to `dca_chart_source_status`. A model / pickup change is a NEW voice,
  built off every lock the callback or a status poll takes and CROSS-FADED in
  (`fading` published before `voice`); replaced and released voices are freed by
  the chart REAPER thread, never by the callback. Only the selection's own pack is
  loaded. `AbiV40Tests`, `ChartInstrumentNullDeviceTests`,
  `RtChartModelSwapCrossfadesAndReleasesThePack`, the `instrument_models` ctest.
  → `native-engine-abi.md` · `chart-voice.md`

- **ABI v39 puts the CAPO into `dca_chart_note`** (offset 64, the first reserved
  slot; still 80 bytes; no export changed). `pitch` still includes it; a voice
  measures natural-harmonic nodes from it. `AbiV39Tests`.
  → `native-engine-abi.md`

- **ABI v38 adds the CHART SOURCE — a per-seat note generator mixed into a
  seat's RIG INPUT after its capture ring — the SONG CLOCK it follows and the
  instrument ROUTE; nine additive exports, everything OFF at engine start.**
  Detection never hears the generator because it enters AFTER
  `writeInputCapture`; keep it there. The clock is ONE bound transport source
  read on the audio thread after the pull, guarded by a Dekker handshake that
  `TransportSource::releaseWiring` waits out FIRST. Tables and voices are
  published by swap + SERIAL and freed on the control thread (leaked if the
  callback wedges). A voice (`src/synth/ChartVoice.h`) is registered ONLY in
  `ChartVoiceFactory.cpp`; a new articulation is a new `DCA_CHART_NOTE_*` bit
  (the note struct is stride-extensible), never an ABI bump. The SANDBOXED
  instrument cannot be routed into a rig. Nothing survives a rebuild — the host
  replays. `AbiV38Tests`, `ChartSourceNullDeviceTests`, the `chart_scheduler`
  ctest, `RtChartSourceFeedsTheSeatRigNotTheCaptureRing`.
  → `native-engine-abi.md`

- **ABI v37 adds SPILLOVER, RE-AMP INJECTION and a CURSORLESS capture read —
  ten additive `_for_player` exports, everything OFF / empty at engine start.**
  A removed audible `delay`/`reverb` node hands over IN PLACE (its INPUT ramps
  to silence, its output keeps ringing) and then rings as a post-chain tail
  until 8 s or -90 dBFS; tails are reaped after a publish's barrier, never on
  the callback, at most four per seat. An injected take REPLACES a seat's input
  before its capture ring, is published by an atomic swap and freed on the
  control thread after a seq_cst Dekker handshake (leaked, not freed, if the
  callback wedges). Neither survives a rebuild on its own — the host replays
  both. `AbiV37Tests`, `RtSpilloverTailRingsAfterRemovalWithoutAllocating`,
  `RtInputInjectionReplacesTheSeatInputWithoutAllocating`.
  → `native-engine-abi.md`

- **ABI v36 adds TWO exports and one status, and both are SECOND entry points
  rather than changed ones.** (1) `dca_stems_separate_file2` +
  `dca_stems_progress_cb2` — the v35 progress callback returns `void`, so a
  multi-minute separation could not be CANCELLED at all, and the managed side
  honoured "a cancelled separation throws `OperationCanceledException`" only by
  abandoning a thread that went on writing stems nobody wanted. The new callback
  returns `int` and **NON-ZERO means CONTINUE**, deliberately: a binding that
  forgets to return a value on some path then fails safe instead of silently
  cancelling everything. Checked between the coarse phases AND **between the
  per-target WAV writes** — four or six files of pure I/O after the model has
  run, which is the stretch that was uninterruptible. Inference itself is
  demucs.cpp's own threaded call and takes no callback, so a cancel arriving
  during it lands when inference returns; that bound is documented, not
  pretended away. A cancelled run LEAVES the stems already written on disk.
  (2) `dca_midi_output_get_counts3` — `failedCount()` (messages the DRIVER threw
  on) existed natively since v35 with no way to read it, and it is a different
  fault from `dropped` ("our queue was full, we never handed it over").
  (3) `DCA_ERR_CANCELLED = 6`, the first new status since the enum was written —
  **not a failure**, and the wrapper raises `OperationCanceledException` for it
  so a user's Cancel never reads as a reason to fall through to another backend.
  **Changing the existing typedef was the alternative and is worse**: `void` has
  no room for an answer, and every binding compiled against it would break
  silently. `AbiV36Tests` pins the mirrored version, the status ordinal on both
  sides, and that all four entry points resolve out of the built DLL.
  → `native-engine-abi.md`

- **ABI v35 adds eight exports and REVERSES two v34 decisions.**
  (1) The seven SEAT-0 ALIASES the v34 additions were missing
  (`dca_engine_dsp_{begin,commit,abort}_edit`, `_get_editing`,
  `_{set,get}_branch_pdc`, `_process_buffer_automated`) — "every pre-v28 export
  is an alias for seat 0" is a rule this repo states, and the automated render
  broke it while its non-automated sibling did not. (2)
  `dca_midi_output_get_counts`: a message the dispatcher DROPPED because its
  queue was full was neither counted nor distinguishable from a bad argument, so
  the caller was told its ARGUMENTS were wrong while the note-off it lost became
  a stuck note; `_send`/`_send_at` now answer `DCA_ERR_DEVICE` for a full queue.
  (3) **THE MASTER LIMITER IS OFF AT ENGINE START** — v34 shipped it ON, which
  made this engine's own "everything is OFF at engine start" rule true only for
  a user who had switched it off: the stage sits on the LIVE MONITOR path, so it
  cost every player 1 ms of latency on their own playing and moved every
  consumer's click-alignment arithmetic by 48 samples at 48 kHz. **A host that
  wants a bounded streaming mirror must now call
  `dca_engine_set_master_limiter`.** (4) The automated offline render RESOLVES
  every event (seat, node, param id) in its pre-pass, so a rejected list leaves
  the RIG untouched and not only the buffer, an event naming ANOTHER SEAT's node
  is rejected, and `event_count` is capped at 262,144.
  → `native-engine-abi.md`

- **NOTHING ESCAPES THE DRIVER CALLBACK, and that is a property of the CLASS,
  not of a list of call sites.** An exception out of the audio callback unwinds
  through JUCE's ASIO/WASAPI frame with nothing to catch it — `std::terminate`,
  a process kill, not a dropped block (`NullAudioIODevice::renderOneBlock` is
  `noexcept`, so the RT gate itself dies on one). Everything here is this
  project's own code and cannot throw EXCEPT the third-party plugins, of which
  there are three call sites; the engine wrapped two and left
  `processInputChain` — the path every amp-sim user is on — calling
  `processBlock` bare, with no outer backstop either. Both halves exist now: a
  per-stage `try`/`catch` that BREAKS (stage N+1 must not run over the audio
  stage N left half-written) and a bare `catch (...)` with an EMPTY body around
  the whole callback, which fires ZERO times across the gate so it masks
  nothing. **The DEVICE POINTER rule is the same shape**:
  `getCurrentAudioDevice()` may appear only at four sanctioned sites and
  everything else takes `deviceSnapshot()` — `dca_engine_show_asio_panel` held
  that raw pointer across a MODAL vendor dialog from the caller's thread, and is
  now marshalled through `callSync` and `guard()`ed with the catch INSIDE the
  marshalled body. Pinned by `RtAThrowingInputPluginCannotKillTheCallback` (the
  gate process DIES without the fix) and `NativeCallbackGuardLintTests`, which
  sweeps `AudioEngine.{h,cpp}` and `dotcharter_audio.cpp` — **not every
  `->processBlock(` site in the repo**, as this bullet claimed until 2026-09-12:
  the out-of-process host child (`src/host/VstHostMain.cpp`) is outside its
  sweep, deliberately, because a throw there kills the SANDBOX and not the host.
  Its "three call sites" are two `processBlock` plus a `prepareToPlay`.
  → `rt-safety-and-tests.md`

- **The failure sentinel of a VALUE-returning export is `-1`, never `0`.** Five
  exports answered `0` for "dead engine / bad seat" AND for "this seat's rig is
  empty", while their neighbours used `-1` for the same fault — two conventions
  for one concept, and a caller could not tell a disposed handle from an empty
  board. `0` stays a legal ANSWER everywhere. **`dca_engine_dsp_get_metering` is
  the one deliberate exception and keeps `0`**: it is a PREDICATE read as
  `!= 0`, so `-1` would report "metering is ON" for a dead handle. And a
  pre-v28 export is a ONE-LINE ALIAS for seat 0 — the four capture exports were
  full copies of their `_for_player` twins, which is two drift surfaces for one
  behaviour. `AbiSentinelAndAliasTests` + `SeatZeroAliasEquivalenceTests`.
  **No `DCA_ABI_VERSION` bump of its own** — no export was added, removed
  or resignatured, and every in-repo caller clamps with `<= 0` / `Math.Max(0, …)`.
  (The header is at **36** today; this line said "it is still 35" until
  2026-09-12, contradicting the v36 bullet eighty lines above it.)
  → `native-engine-abi.md`

- **The MIDI producer side is SERIALISED; the consumer stays lock-free.**
  `midiFifo_` is a `juce::AbstractFifo` (SPSC) with at least two producers — the
  JUCE MIDI input thread and whatever thread calls `dca_engine_send_midi_at` —
  and two concurrent `prepareToWrite`s compute from the same `validStart`, so
  both write the same slot and `finishedWrite` double-advances: one event LOST,
  one slot of stale garbage forwarded, no counter moving. `midiProducerMutex_`
  is a message-side lock the AUDIO THREAD NEVER TAKES, which is why
  `rt_lock_sites.txt` does not move. Pinned by
  `RtConcurrentMidiProducersLoseNoEvents`.
  → `native-engine-abi.md` · `rt-safety-and-tests.md`

- **A `commitEdit` OWNS its scope for the whole commit, fades included.**
  `DspChain::commitEdit` must drop `writeMutex_` for `waitForFades` (holding it
  there blocks every other message-thread mutator for the 10 ms crossfade), and
  it drops it with `editing_` still true — so `beginEdit`'s stale-scope valve
  used to fire into the middle of it and the resumed commit then clobbered the
  state the NEW scope had just established. A `committing_` flag closes that:
  `beginEdit` WAITS on `editCv_`, `abortEdit` is REFUSED. No deliberate
  threading is needed to reach it — the managed `JuceDspEditScope` finalizer
  runs on the GC thread. Managed-side the open scope is identified by an **ID,
  not an object reference**: `_openScope` is a SHORT `WeakReference`, cleared
  BEFORE the finalizer runs, so a dropped scope could abort the scope that
  REPLACED it (`JuceDspChain.ClaimScope`). `Wave3EditScopeRaceTests`.
  → `dsp-tone-rig.md`

- **ABI v34 gives the OFFLINE rig render parameter automation, and the events
  go through the LIVE setter path.**
  `dca_engine_dsp_process_buffer_automated_for_player` chunks at each event
  boundary so an event at frame F is heard BY sample F, and every event goes
  through `DspChain::setParam` — the same mailbox the callback's setters use,
  which is what stops an automated render and a live performance of the same
  gesture diverging. The WHOLE list is validated before a sample is rendered
  (a half-processed buffer is indistinguishable from a good one); an unsorted
  list is REJECTED, never reordered; `"bypass"` is a reserved param id.
  → `native-engine-abi.md`

- **ABI v34 makes instrument MIDI sample-accurate and adds MIDI OUTPUT.**
  Events were stamped at sample 0 of whichever block drained them; the comment
  that justified it asked for a free-running audio-thread sample counter that
  ABI v26 then shipped, and nobody came back. `dca_engine_send_midi_at` PARKS
  the event until the block containing its frame — the v26 scheduled-click
  mechanism, because clamping into the draining block is the same error under a
  new name. A late event goes at sample 0 and is COUNTED; a status byte below
  0x80 is REJECTED (packed 0 is the queue's empty-slot marker). The SANDBOX
  BRIDGE leg is still flat and that is a documented gap, not an oversight: its
  shared `uint32` has no room for an offset. MIDI out dispatches from ONE
  high-priority thread per device that spins the last 1.5 ms — JUCE's own
  `sendBlockOfMessages` locks on the CALLER's thread and takes millisecond
  deadlines — and `_send_at` takes the engine as a PARAMETER so an output can
  never outlive one it cached.
  → `native-engine-abi.md`

- **ABI v34 puts a CEILING on the master bus, and it is a TRUE BYPASS when it
  is switched off.** The final device buffer was NaN-scrubbed and never
  BOUNDED — a scrub says nothing about magnitude — so song + every seat's
  monitor + click + instrument reached the driver and the streaming mirror past
  ±1.0, and the mirror's float path carried that into OBS's encoder, which
  wraps. `dca_engine_set_master_limiter` is **OFF by default since ABI v35** (it was ON
  in v34 — the stage sits on the LIVE MONITOR path, so default-ON cost every
  player 1 ms of latency on their own playing for a feature they had not asked
  for; the HOST opts in) at a −0.3 dBFS ceiling. Its
  1 ms look-ahead is folded into `dca_engine_get_output_latency_samples`, and
  **switched OFF the stage is not in the path at all** — keeping the delay
  running so that figure never moved re-timed the whole engine and broke
  `RtEqTrimRampsAndSparseInputIsSilent`, so a click scheduler must RE-READ the
  latency after a toggle. The gain computer is a SLIDING MINIMUM over the
  look-ahead window — with the instantaneous target it degrades to a clipper
  (0.78 % → 13.28 % of samples on the ceiling, measured).
  → `native-engine-abi.md`

- **ABI v33 adds five INTROSPECTION exports and clamps two source setters.**
  `dca_engine_dsp_get_node_type` / `dca_engine_dsp_get_bypass` publish what
  `DspChain` always knew, so a node another binding created is no longer a bare
  integer; the bypass getter is the message-thread INTENT, never the 10 ms fade
  position. `dca_dsp_module_type_category` gives every catalogue entry one word
  from a FIXED set the engine owns — never mirror that vocabulary managed-side.
  `dca_plugin_count_vst3_components` / `dca_plugin_load_vst3_component` reach a
  multi-component bundle (an index it does not have is REJECTED, never clamped);
  the child's `load` pipe op gained an optional `component` property, which is
  JSON and NOT the shared-memory header, so `kBridgeVersion` did not move. And
  `dca_source_set_volume` now clamps to `[0, +inf)` — a negative gain is a phase
  INVERSION, not a quieter source — with **no upper bound**, because boost above
  unity ships; `dca_source_set_pan` clamps to `[-1, +1]` at the boundary as well
  as inside the panner, so a cached managed getter cannot disagree with the
  engine.
  → `native-engine-abi.md`

- **ABI v32 / wrapper 0.8.0 adds parked DSP insertion.**
  `dca_engine_dsp_add_node_bypassed_for_player` creates a regular node at zero
  wet; configure it before `set_bypass(false)` fades it in. Existing add exports
  remain immediately wet for offline compatibility. Split/merger topology is
  still immediate; this does not make an entire preset change atomic.

- **A LIVE seat's rig SLEEPS while nothing is heard, and only on the device
  callback** (2026-09-24, no ABI change). `AudioEngine::processDspRigLive` skips
  `DspChain::process` and clears the rig's output once what the seat puts on the
  device output has stayed under -90 dBFS for the hold (>= 5 s, longer for a
  node that declares a settle) over a stationary input under -40 dBFS, and wakes
  ON the block whose input clears sqrt(2) x the idle floor — that block is
  processed. Any edit, param write, trim / monitor / input-EQ / VST / bridge
  change wakes it. Only ALLOW-LISTED module types may sleep
  (`src/dsp/IdleSleep.h`; the list fails closed), the offline renders never
  do, and **a new type goes on the list only with `juce_sharp_rig_idle_sweep`
  green**. The RT gate's `Fixture` turns it OFF for every older scenario.
  NeuralAudio + RTNeural are `/arch:AVX2` since the same day
  (`JUCESHARP_NEURAL_AVX2`, -36 % on the owner's two captures) — which makes
  AVX2 a CPU requirement for loading the DLL.
  → `dsp-tone-rig.md` § Idle sleep

- **A SIDECHAIN source a module ignores is a dead feature, so it is REJECTED**
  (ABI v34). `DspModule::supportsSidechain()` + `processSidechained()` are an
  opt-in second buffer — FIVE modules today: `compressor` and `gate`
  (`DspModuleFactory.cpp`), plus `expander`, `ducker` (`gear/DynamicsGear.cpp`)
  and `vocoder` (`gear/FilterGear.cpp`) from the gear wave; grep
  `supportsSidechain` rather than trusting this list — and
  `DspChain::setNodeSidechain` refuses a non-zero source on anything else
  rather than storing it. The capability is advertised by
  `dca_dsp_module_type_flags` (bit 0) because `dca_dsp_module_type_info` cannot
  change signature, which makes the flag table a SECOND source of truth for one
  capability: `RtSidechainKeyingIsAllocationFreeAndAdvertisedHonestly` sweeps
  the catalogue and fails if a row disagrees with a constructed module.
  **An UNAVAILABLE source falls back to the module's own input, never to
  silence** — keyed from silence a gate sits CLOSED, which is a rig that goes
  quiet with nothing on screen saying why. The seat-input key is a copy of the
  chain's input block, sized in `prepare()` for the worst case and filled only
  when the published order actually asks for it (one `rtSidechainDry_` flag,
  the same "off until asked for" discipline as the meters). The song-mix key is
  ONE read-only pointer the engine hands `processDspRig` per block; it is the
  output block after `processMasterBus`, so for seat p it also carries the
  monitor of every earlier seat — documented, not accidental.
  → `dsp-tone-rig.md`
- **Never put a phase vocoder on the live input.** The shifter is **CSVD**
  (`src/dsp/cores/dcdsp_droptune.h`, ABI v25) and it is DOWN-ONLY, because
  down-shifting has no latency floor while the phase-vocoder family cannot be
  low-latency at all (a low E needs a ~48 ms window; Rubber Band's LiveShifter
  and Signalsmith are excellent and unplayable here). Never emit dry, unshifted
  audio at the attack.
  → `dsp-tone-rig.md`
- **Node ids are engine-global: seat `p` hands out
  `p * DCA_PLAYER_NODE_ID_STRIDE + local`.** That is what kept the v28 ABI delta
  small — NODE-scoped exports keep their pre-v28 signature and resolve the seat
  from the id; only CHAIN-scoped ones gained `*_for_player`. A node id naming a
  seat past `DCA_MAX_PLAYERS` is REJECTED, never clamped.
  → `native-engine-abi.md`
- **`playerCount_` defaults to 1 and at 1 the callback runs the pre-v28 path
  byte-for-byte** — a solo player must not pay for, or hear any change from, a
  feature they are not using. Same contract for the v29 EQ: everything is OFF at
  engine start.
  → `native-engine-abi.md`
- **A v29 EQ band is published only when it is AUDIBLE**, decided once on the
  message thread, so `dca_engine_get_eq_active_count` and the audio callback are
  ONE predicate rather than two copies of one. Managed ranges are mirrored in
  `DotCharter.Services/Audio/Equalizer/EqBand.cs` and clamped on BOTH sides —
  change either side and change both.
  → `native-engine-abi.md`
- **There is ONE ABI translation unit: `src/dotcharter_audio.cpp`.** A
  per-domain split (`src/abi/abi_*.cpp`, five files) sat unbuilt in the tree
  from ~ABI v13 to 2026-08-28 — every function a stale duplicate of a live one,
  and an export added there compiled perfectly cleanly and did not exist in the
  DLL. It is deleted; do not restore it. After every ABI addition, `dumpbin
  /EXPORTS` is still the only proof the entry point is in the DLL.
  → `native-engine-abi.md`
- **The render clock is published immediately after the transport pull**, not at
  the end of the callback — that placement is what makes it and
  `dca_source_get_audible_position_seconds` describe the SAME output sample.
  → `native-engine-abi.md`
- **Dropout detection is NATIVE, and its three counters are three different
  faults** — `late_callbacks` (the callback arrived late: the audible one, and
  the only signal WASAPI/CoreAudio give at all), `deadline_overruns` (we were
  the cause) and `near_misses` (a load warning). `device_xruns` is −1 on any
  driver that cannot report; never surface that as zero. The first 4 callbacks
  of every device session are ignored, or every device change reports a
  dropout that never happened. **`dca_xrun_stats` is APPEND-EXTENSIBLE** — the
  caller declares `struct_bytes` Win32-cbSize style and the engine copies the
  smaller of the two — so a new counter goes on the END of it and needs NO ABI
  bump and no new export. `input_plugin_faults` (2026-09-12) is the worked
  example. The callback LOAD is the exception, with its own export
  (`dca_engine_get_callback_load`, ABI v46): it is a decaying meter that a
  reset must not touch, not a "since reset" counter.
  → `native-engine-abi.md`

- **The external process insert (ABI v47) never waits on the other process.**
  `src/ExternalProcessInsert.{h,cpp}`: a seat's dry input goes into a named
  shared-memory ring another process created and its processed block comes back
  in place of the seat's rig. Map/unmap on the CALLER's thread; the audio thread
  takes a session at the bottom of a 5 ms dip and announces it in a hazard slot
  (`inUse_`) before an unmap may happen. An underrun plays the DRY input, never
  silence. Generic names only (no program on the other side is named in this
  repo); the header offsets are pinned with `offsetof` against the format's own
  struct. No resampler: the device must run at the ring's rate
  (`DCA_ERR_SAMPLE_RATE`). `AbiV47Tests`,
  `RtExternalProcessInsertExchangesWithoutAllocatingAndFallsBackDry`.
  → `native-engine-abi.md`

- **A THIRD-PARTY plugin that throws on the callback is CAUGHT AND THEN
  LATCHED — catching alone is only half of it.** `processInputChain`'s
  `catch (...) { break; }` shipped without the second half every host does:
  disable the offender. A plugin that faults DETERMINISTICALLY — an expired
  trial's licence check, a layout it was not prepared for, which is the
  overwhelmingly common case — therefore threw once per callback for ever, and
  a C++ throw is not RT-safe at any price (`__cxa_allocate_exception` mallocs,
  the unwinder takes a global lock over the `.pdata` lookup: tens of
  microseconds against a 2.6 ms budget at 128 frames / 48 kHz). The engine now
  keeps a per-seat bitset of faulted entries, AUDIO-THREAD-ONLY and re-armed by a
  GENERATION COUNTER that attach / detach / prepare bump (**never by snapshot
  POINTER identity — the allocator reuses the address, so a detach+attach
  compares equal and the stale bits disable whatever moved into that index**),
  skips a latched entry, and counts it ONCE per plugin in
  `dca_xrun_stats::input_plugin_faults`. No log from the catch body — the
  counter is the report. Pinned by the storm phase of
  `RtAThrowingInputPluginCannotKillTheCallback`.
  **The driver-callback BACKSTOP now clears the block, too.** Its comment said
  "the block of silence this produces" while its body was empty and `renderBlock`
  never zeroes `out` — so a throw from inside `player_` left whatever the driver
  handed us, which several WASAPI/ASIO backends make the PREVIOUS block at full
  level.
  → `native-engine-abi.md`

- **`dca_engine_show_asio_panel` is the ONE export that can occupy the JuceHost
  message thread indefinitely, and that is a documented trade, not a bug.**
  `showControlPanel()` is a modal vendor dialog; while it is up every other
  `callSync` export queues behind it, `dca_engine_destroy` included, so closing
  the app with the panel open makes the managed drain time out at 60 s, log at
  Error and LEAK the engine. The alternatives are worse: `callAsync` unblocks
  only this caller (the panel still owns the thread) and loses the three error
  answers the export returns today, and running the panel off the message thread
  hands the raw device pointer back to a thread that does not own the driver's
  COM apartment — which is the W4-N5 fault this marshalling exists to fix.
  → `native-engine-abi.md`
- **A device REBUILD only replays what `prepareMasterBus` knows about**, so
  anything that must follow the rate / block size belongs there — that is why an
  attached VST3 is now re-prepared and the streaming mirror is remembered. And
  **`stop()` + `start()` on one engine does not restore playback** (sources are
  detached from the mixer forever): recreate the engine, or reconfigure it.
  → `native-engine-abi.md`
- **The offline waveform block width is FRACTIONAL and must stay that way** — an
  integer samples-per-block is exact at 48 kHz and drifts +408 ms over three
  minutes at 44.1 kHz.
  → `native-engine-abi.md`
- **Every DISCRETE DSP param must declare itself** — NAMED (`valueLabels`) or
  ORDINAL (`ParamInfo.step` > 0). A param the adapter `lround`s is never a plain
  knob, or the UI parks a knob at "6.5 stages" while the core runs 6.
  → `dsp-tone-rig.md`
- **Adapter float params go through `ParamMailbox`; never call `core_.setParams`
  from `setParam`** — that recomputes multi-word coefficients while `process()`
  may be reading them.
  → `dsp-tone-rig.md`
- **An in-path bass filter is a BOUNDED SHELF, never an unbounded high-pass.**
  Each such filter looks individually reasonable and they SUM — never add a
  fourth without measuring the cascade (`tools/tone_response.cpp`).
  → `dsp-tone-rig.md`
- **`prepare()` re-derives coefficients; it does NOT clear state, and neither
  does any `set*` in `dcdsp_primitives.h`.** Four shapes of this shipped and
  each is invisible until a device change: state left uncleared, a rate-derived
  sample count only `setParams` owned, a smoother snapped to `current()`
  (which is 0 before it has ever ramped — `bigmuff` was SILENT on a fresh
  board), and a reassigned filter vector whose "has it changed?" cache survived
  prepare and skipped the redesign (`widener`'s bass-mono became a WIRE).
  → `dsp-tone-rig.md`
- **A buffer sized from a runtime MODE must be ALLOCATED for the worst case.**
  `whammy`'s `lowLatency` param re-ran its geometry, growing the ring sizes 4×
  against vectors `prepare()` had allocated in the small mode — and every ring
  access masks with those members.
  → `dsp-tone-rig.md`
- **Per-node meters are POLL-ONLY and OFF until asked for (ABI v30);
  `dca_engine_dsp_get_chain_latency` is the MAX over a split region's branches,
  not the sum.** Since ABI v34 that number also describes a region that is
  ALIGNED: the merger carries one delay line per branch, set to (max − branch)
  and re-derived on every publish, every accepted param/string write and every
  prepare, so summing branches of different latency is no longer the comb it
  was from v23 to v33. It is ON by default and
  `dca_engine_dsp_set_branch_pdc_for_player` turns it off (the comb is a sound
  some players want, and off is the pre-v34 path byte for byte). The rings are
  sized for the WORST case in `MergerModule::prepare` (8,192 samples per branch
  per channel) and a deeper branch CLAMPS; an UNMATCHED splitter auto-merges
  inside `DspChain::process` rather than at a merger node and is not
  compensated. An adapter that swallows `core_.latencySamples()` makes both the
  export and the alignment lie.
  → `native-engine-abi.md` · `dsp-tone-rig.md`
- **The offline tooling path is not the device callback and inherits none of its
  guarantees** — it establishes its own `ScopedNoDenormals` and must re-prepare
  the rig on a rate change, but only when no device is open.
  → `dsp-tone-rig.md`
- **EVERY UNTRUSTED STAGE IS FOLLOWED BY A SCRUB** — `dcdsp::scrubNonFinite`
  (`src/dsp/Scrub.h`) is the one implementation. `DspChain::process` scrubs its
  input and after EVERY stage; the ENGINE scrubs after the VST3 input chain +
  sandbox bridge (before `applyEqBank`), at the top of `processMasterBus`, and
  after the instrument mixes (before the streaming mirror). A NaN into a
  `juce::dsp::IIR` biquad is PERMANENT — the filter feeds its own output back
  and `reset()` is only reached on an OFF→ON transition — so one bad sample out
  of a user plugin used to silence a seat for the session, meter included. The
  managed detection path scrubs its input too (a `NaN` passes every `<=` gate
  and then holds the noise gate open for the whole 10 s horizon). Keep every
  half. Pinned by `RtNaNFromAnInputPluginCannotLatchTheEq`.
  → `dsp-tone-rig.md` · `rt-safety-and-tests.md` · `managed-dsp-detection.md`
- **`LiveCqtConfig` is the single source of truth for the shipping detection
  configuration** — never re-type a shipping constant into a harness; that is
  exactly what drifted, and the eval then reported a pre-veto number as current.
  Measure with `CqtStreamingEvalTests`, never `CqtDatasetEvalTests`.
  → `managed-dsp-detection.md`
- **`ChildProcessRegistry` NEVER owns the `Process` objects it tracks** — it kills
  and untracks; disposal belongs to whoever called `Process.Start`. Disposing from
  its own `Exited` handler made a SUCCESSFUL python run report as a separation
  failure.
  → `stem-separation-processes.md`
- **A cancelled separation throws `OperationCanceledException`, not
  `StemSeparationException`** — otherwise a user Cancel reads to every consumer as
  a reason to fall through to the next backend.
  → `stem-separation-processes.md`
- **Every user-facing GAIN is ramped across the block, never applied as a
  constant.** `AudioEngine::rampOrScale` / `rampOrScaleAdd` is the route for the
  MASTER-BUS levels — master volume, the per-seat monitor, the input trim — and
  it is **not the only ramping site in the engine**: a per-SOURCE level ramps
  where the source lives. `PanningAudioSource::getNextAudioBlock`
  (`src/JuceSharp.Native/src/PanningAudioSource.h`) ramps the two pan gains with
  `juce::AudioBuffer::applyGainRamp` from the previous block's applied value,
  falling back to a flat `applyGain` only when the gain did not move; its own
  comment records that it "was the last user-facing level that still stepped".
  Pinned by `RtSourcePanRampsInsteadOfStepping`. **Reach for the mechanism the
  surrounding code already uses — do not route a source-level gain through
  `rampOrScale`.** (This bullet said `rampOrScale` was "the ONLY route" until
  2026-09-11, which would have sent the next agent adding a source gain to the
  wrong place entirely.) A slider
  publishes per UI tick; one constant per block is a zipper, and the old
  `if (pmon > 0)` monitor guard made a mute a full-amplitude cut in ONE sample.
  The same shape bit pan twice: an early `return` for |pan| < 1e-4 turned a drag
  back to centre into a one-sample CUT, and removing it was part of the fix.
  **The same rule holds INSIDE a core**: `SmoothedValue` (dcdsp_primitives.h,
  `snapToTarget()` in `prepare()`, `isRamping()` for any "unity, skip the
  loop" shortcut) or a block ramp from the previous block's value. **A core
  whose `process()` loop is CHANNEL-outer must use `SmoothedValue::blockStart`
  + `advanceBlock`, never `next()`** — `next()` inside the inner sample loop
  walks the same smoother once per channel, so the ramp runs numChannels times
  too fast AND each channel gets a different gain over the same block, which
  tilts the stereo image while the knob moves. `dcdsp_eq.h` and
  `dcdsp_graphiceq.h` hand-rolled that line before it was shared; leave those
  two alone. The clean `gain` node, the compressor's makeup and the sustainer's
  makeup + level were per-block constants until 2026-08-28, and **30 more
  mixes / output levels across 24 cores until 2026-08-29**.
  `DspGainZipperTests` (managed, offline) is where a new gain gets its case,
  and it has TWO theories: `Cases` for a gain that raises the level a lot, and
  `MixCases` for a mix or level that does not — the latter renders the same
  stimulus three times (never moved / moved from the start / moved at the seam)
  and asserts the seam is still near the OLD value 128 samples in. Feedback
  coefficients are deliberately NOT in it: a loop gain is not a per-sample
  output multiply, so stepping it changes a tail's steady level without ever
  producing a sample-to-sample discontinuity.
  → `rt-safety-and-tests.md`
- **A param may not STEP a modulator, a delay tap, or one callback's workload.**
  A waveform switch is a full-scale discontinuity (sine vs triangle differ by up
  to 1.0 at one phase), so `ringmod` and `vibrato` crossfade it over 10 ms using
  `Lfo::peek`/`advance`; `resonator` ramps its per-voice tap, because a splice in
  a 0.998-feedback Karplus-Strong loop is re-injected rather than decayed; and
  `glitch` drains its capture incrementally (`dcdsp_freeze.h`'s shape) instead of
  copying up to 1.5 MB inside the callback that saw the footswitch. None of the
  three is a level, which is why the gain rule and the allocation gate both
  looked past them. `DspParamSeamTests` +
  `RtGlitchCaptureDoesNotBurstOneCallback`.
  → `dsp-tone-rig.md`
- **Delay trails are an opt-in bypass-processing capability.** Only `DelayModule`
  owns its bypass processing at zero chain weight; it drains history while fresh
  dry passes through. Its parked reset is an atomic request consumed on the
  callback, never a concurrent message-thread buffer clear. `sync` and `trails`
  default off; the legacy delay output is bit-exact.
  See `dsp-tone-rig.md`.
- **Structure changes to the rig are CROSSFADED by the CHAIN, never by a core**
  (2026-08-28). `DspChain` gives every regular node a wet/dry weight that ramps
  over `kFadeSeconds` (10 ms) on the callback: bypass fades out, un-bypass
  fades in with the module RUNNING during the ramp (that is what keeps a
  `SwellCore` follower warm), remove waits for the fade-out before it retires
  the module, move/retag fade out at the old slot and in at the new. Splitter
  and merger still switch instantly — topology, not level. A core must not
  add its own bypass ramp on top. Pinned by `DspBypassCrossfadeTests`
  (managed, offline, reads the ramp off the samples) and the gate's
  `RtStructureStormCrossfadesWithoutAllocating`.
  **…and since ABI v34 a whole PRESET is one such change, not N of them.**
  `DspChain::beginEdit/commitEdit/abortEdit` suppress publication for the
  length of a scope: writes accumulate against a SHADOW (`editAdded_`,
  `editRemoved_`, which keeps a removed node alive because it is still in the
  live snapshot), reads report the shadow, and the commit costs AT MOST TWO
  publications whatever the preset size — one transitional, where the old board
  fades out while the new nodes fade in in the SAME 10 ms window, and one final.
  Nodes added inside a scope are parked at 0 wet whatever their bypass flag.
  Abort restores STRUCTURE only (membership, order, bypass, branch tags);
  parameter VALUES are not journalled. A nested begin is still never a NESTING
  LEVEL — but **since 2026-09-11 it ABORTS the stale scope and logs a Warning
  instead of returning false** (`staleScopeAborts_` counts it), and `~DspChain`
  still ends an open scope. **The change is ABI-neutral — no export signature or
  version moved for it (the header is at 36 today; this line said 35 until
  2026-09-12).** The reversal is not a softening — returning false left the dead
  scope OPEN, and an open scope publishes nothing, so a caller that dropped a
  `JuceDspEditScope` (forgot the `using`, stored it on a field) froze that
  seat's rig for the life of the engine while every `AddNode`/`RemoveNode`/
  `Bypass` returned success. Failing the NEW edit to protect a scope nobody owns
  is the wrong trade; the log line names the cause. Pinned by
  `RtPresetCommitPublishesOncePerPresetNotOncePerNode` (counts publications
  against the unscoped baseline while the callback runs) and
  `DspEditScopeTests`.
- **A publisher that cannot take the callback's mutex still may not GIVE UP.**
  `NamAmpModule` spun 500 ms on `use_count() > 1` and then released anyway, so
  a slow block was handed a `free()` and a NeuralAudio model destructor — the
  bound's own failure case. `src/dsp/Retirement.h`'s `dcdsp::RetirementQueue`
  parks the retired object and the worker sweeps it once nothing else holds it.
  namamp is NOT in the RT gate binary (verify with the "namamp" string, present
  in the DLL and absent from `juce_sharp_rtcheck.exe`), so the queue is pinned
  by `RtRetiredSnapshotsDieOnTheOwnerThread` and the module's use of it by
  `NativeRtSourceLintTests`.
  → `rt-safety-and-tests.md`
- **The retired ordering snapshot dies on the MESSAGE thread.** `publishLocked`
  exchanges `order_`, takes the one-block barrier, and only then drops the old
  `shared_ptr`. Drop it before the barrier and the callback's copy is the last
  owner — 435 frees on the audio thread in one storm, latent since the chain
  existed, invisible until a scenario toggled structure while ARMED.
  → `rt-safety-and-tests.md`
- **The file→device rate conversion happens ONCE at LOAD, not per block, and
  since 2026-08-29 it is a RATIONAL POLYPHASE FIR (`src/LoadResampler.h`).**
  JUCE's `ResamplingAudioSource` is linear interpolation plus one 2nd-order
  Butterworth (−3.14 dB at 15 kHz for a 44.1 kHz track on a 48 kHz device), so
  `tryPreloadIntoMemory` converts the decoded buffer itself. Doing that with a
  per-sample `juce::WindowedSincInterpolator` was the right QUALITY at the
  wrong PRICE: **~6.4 s per 183 s stereo file, once per stem, sequentially —
  ~39 s of a 6-stem project's load** (owner's log, v0.48.5). 44.1→48 kHz is
  EXACTLY 160:147, so the same 100-crossing kernel is precomputed once into 160
  phases of 201 taps and each output sample becomes one SSE dot product, split
  over up to 4 threads on the LOAD thread. Measured on that same case:
  **6,129 ms → 104 ms (×59; 340 ms on one thread)**, 15 kHz −0.000319 dB
  against a native 48 kHz file (was −0.088), burst offset 0 samples.
  **Group delay is now zero by CONSTRUCTION** — the prototype's centre sits on
  a whole multiple of L — where the fallback still trims by hand; uncompensated,
  the whole song sits 2.3 ms late against the chart. A ratio the reduction
  refuses (a non-integer rate, or more than 1024 phases) falls back to the
  windowed sinc SILENTLY, which is why the gate asserts the fast path was TAKEN
  and not merely that it still sounds right. Nothing in that header may ever be
  reached from the callback.
  **BOTH DIRECTIONS, AND STREAMING TOO, since JuceSharp #33.** The kernel's
  cutoff is `0.99 × min(1, L/M)` × the input Nyquist, so a DOWNSAMPLE cuts at
  the OUTPUT Nyquist (48→44.1 kHz is 147:160, 219 taps per phase: the window
  widens by M/L to keep ~100 output-rate crossings) and `tryPreloadIntoMemory`
  converts it at load (`loadWillConvert` → `dcload::convertDecodedBuffer`). A
  source that MISSED the preload budget gets `PolyphaseStreamSource`
  (`src/PolyphaseStreamSource.h`): the same cached kernel run per block on the
  audio thread, over a `BufferingAudioSource` the engine builds itself, so the
  transport's own resampler sits at ratio 1.0. Its history buffer is sized at
  create() and output is made in ≤512-frame chunks, so it never allocates;
  seeks/prepare arrive under `stretcherMutex_` like every other seek. Only a
  ratio with NO rational form (non-integer rate, >1024 phases, >8:1 down) is
  still JUCE's linear converter, and the open log says so:
  `ResampleReport::converter` is `polyphase` / `windowed-sinc` /
  `streaming-polyphase` / `transport-linear` / `none`. `dca_convert_audio_for_device`
  (ABI v43) stays UPSAMPLE-only on purpose (`loadWillUpsample`): its host
  replaces the original with the result. Pinned by
  `RtPreloadedSourceDownsamplesAtLoadWithoutAliasing` and
  `RtStreamingSourceResamplesWithThePolyphaseKernel`.
  → `rt-safety-and-tests.md`
- **A NaN param is rejected at `DspChain::setParam` and REPORTED.** Every core's
  clamp is NaN-transparent (`v < lo`/`v > hi` are both false), so a NaN reaches
  the coefficients and the module is silent for good.
  → `dsp-tone-rig.md`
- **`DspModule::reset()` has exactly ONE caller and that is deliberate:
  `DspChain::setBypass(id, false)` on a node whose fade-out has fully PARKED**
  (2026-08-29; it had none at all before). A parked node is `continue`d by
  `process()`, so its delay lines, tanks, followers and LFO phases freeze at the
  instant the fade landed — not decaying, frozen — and fading that back in
  replayed audio from before the bypass (measured: 79 % of the tail came back).
  Clearing on the way in is the true-bypass answer, and it is safe precisely
  because the fade has landed: `wet` is stored AFTER `module->process()`, so once
  the message thread sees 0 the audio thread is already past the node. A toggle
  inside the 10 ms ramp is not a park and keeps its state. **Move and remove must
  NOT come through this path** — they drive the flag via `fadeOutAndWait`/
  `fadeIn`, which is what keeps DspChain.h's promise that a reorder never clears
  a delay's buffer. `DspBypassClearsStateTests` pins both halves.
  → `dsp-tone-rig.md`
- **A two-deep buffer flip is ABA-unsafe — never publish shared state that way.**
  The reader resolves the index and reads out of that buffer LATER, so the
  SECOND write flips back to the buffer it is holding and rewrites it under it.
  Both Miku cores shipped that for their user phrase tables and both now use a
  per-slot SEQLOCK (the `ParamMailbox` pattern) over packed lock-free atomics,
  resolved AT THE NOTE rather than hoisted to the block top; the audio thread
  gives up after `kPhraseReadTries` instead of spinning, and neither side
  allocates or frees. Pinned by `RtMikuPhraseSwapIsCoherentUnderEdits`
  (33,727 torn reads the moment the packing is removed).
  → `dsp-tone-rig.md`
- **The branch-PDC dirty flag is SAMPLED AT THE TOP of `process()` and acted on
  at the BOTTOM.** The re-derive has to run after the modules adopt their
  mailboxes (that is the whole of W4-D54), but consuming the flag at the bottom
  too left a one-callback hole: a `setParam` landing between a module's adopt
  and the `exchange` is swallowed by a block whose modules ran the OLD latency,
  the mergers are programmed from it, the flag is cleared and nothing comes back
  — the original bug, one block wide, which at 5–10 ms blocks is what an
  ordinary UI drag hits. Sampling at the top is also what makes the
  release/acquire pair mean anything: a flag observed there means the mailbox
  write happened-before. And `branchPdc_` itself is an `std::atomic<bool>` since
  2026-09-12 — `recomputeBranchPdcRt` reads it from the callback, which may not
  take `writeMutex_`. `rt_lock_sites.txt` does not move for either: neither adds
  a lock. Pinned by the mid-block phase of
  `RtBranchPdcFollowsAParamOnTheBlockItChanges`.
  → `dsp-tone-rig.md`
- **`DspChain::prepare` holds `writeMutex_` as well as `processMutex_`, in that
  order.** The two methods shared no mutex until 2026-08-29, so a device change /
  NAM 48 kHz request / watchdog recovery (`prepareMasterBus`) — or
  `dspProcessOffline`'s lazy re-prepare — reallocated `branchData_`/`dryData_`
  and re-prepared every module while `process()` was memcpying through the raw
  pointers into them. A missing lock is the one class of RT bug
  `rt_lock_sites.txt` can never catch. Pinned by
  `RtRigPrepareCannotRaceTheCallback`.
  → `dsp-tone-rig.md` · `rt-safety-and-tests.md`
- **`prepare()` must not touch USER CONTENT, and must re-derive everything the
  RATE owns.** `mikustomp`'s three lyric slots were wiped by every device
  change; `octaver`/`rotary`/`phaser` never rate-derived their `DcBlocker`, so
  its corner rose with the device rate (38 Hz at 48 k → 153 Hz at 192 k).
  → `dsp-tone-rig.md`
- **`midiFifo_` is drained, never `reset()`.** `juce::AbstractFifo` is SPSC and
  `reset()` rewrites both index atomics; the instrument attach/detach path
  raced the audio thread's `collectBridgeMidi` (the sandbox-bridge leg, which
  runs precisely while no in-process instrument is attached) AND `sendMidiShort`
  on the JUCE MIDI input thread. `AudioEngine::discardQueuedMidi()` is a
  consumer-side drain under `remoteBridgeMutex_`.
  → `native-engine-abi.md`
- **Never add a `setSize`/`resize` on the callback.** Every audio-thread buffer is
  pre-sized with 2× block headroom at prepare/attach time and the hot paths SKIP
  the block instead of growing. Pinned by `juce_sharp_rtcheck` — and the gate
  cannot see a lock taken inside the standard library, of which there is one
  (`std::atomic_*(shared_ptr*)`, taken three times per block).
  → `rt-safety-and-tests.md`
- **Everything that touches `StemSeparator`'s process-global gate or
  `ChildProcessRegistry`'s live set belongs to
  `[Collection("StemSeparatorConcurrency")]` — ELEVEN declarations across nine
  files today, 2026-09-12 (grep the attribute rather than trusting this count;
  it read "eight" for two waves).** That cross-fire,
  not the environment, was most of what "the StemSeparator tests are flaky" meant.
  A missing dependency must `Assert.Skip`, never `return`.
  → `rt-safety-and-tests.md`
- **A wrapper finalizer must tolerate a PARTIALLY-CONSTRUCTED instance** — reach the
  handle owner as `_owner?.`, never `_owner.`, and `GC.SuppressFinalize(this)` before
  a constructor throws. A throwing finalizer is a process-wide FailFast, so
  `JuceAudioPlugin`'s null deref made a PASSING unit test kill the whole suite
  hundreds of tests later. Pinned by `FinalizerSafetyTests`, which discovers the
  types by reflection.
  → `rt-safety-and-tests.md`
- **A research harness is opt-in or it is part of the gate.** `GND_IDMT_ROOT` is the
  ONLY way the CQT evals find IDMT — the hard-coded fallback path made them run on
  every `dotnet test` on exactly one machine, for ~10 minutes, ending in a failure or
  a dead test host.
  → `rt-safety-and-tests.md` · `managed-dsp-detection.md`

## Rules (mirror the DotCharter root guide)

- All real-time audio lives in the native engine; the managed wrapper stays a
  thin shell (marshalling + lifetime). No managed per-sample processing on the
  hot path.
- **Incompatible ABI change ⇒ bump `DCA_ABI_VERSION` and rebuild the DLL** before
  running managed code. Every shared-memory bridge offset is `static_assert`ed on
  both sides — change the layout ⇒ bump `kBridgeVersion` and both mirrors.
- Handle-owning wrappers use `NativeHandleOwner` (acquire/release around every
  P/Invoke, drain-on-dispose) so a poll racing a dispose surfaces as a caught
  `ObjectDisposedException`, never a native crash. **Its field is nullable and
  every use goes through the wrapper's own `Acquire`/`TryAcquire`/`Release`
  helpers**, because the finalizer sees a constructor that threw.
  **That claim was FALSE on two counts until 0.61 and both fixes must stay**
  (`NativeHandleDrainTests`, `EngineChildDrainTests`):
  - **The drain must never free under a live call.** It waited 2 s on a
    `SpinWait` and then returned the handle *anyway*, so every caller destroyed
    it mid-call — and the calls that routinely exceed 2 s are the ones a user
    reaches (`TryShowAsioControlPanel` is a MODAL vendor dialog, `Reconfigure`
    reopens the driver, a cold VST3 scan is 25 s+). It now waits on the last
    `Release` and, on expiry, LOGS at Error and **leaks** the handle. A leak
    beats a use-after-free.
  - **The drain is PER-HANDLE, so the engine has to drain its CHILDREN.** The
    engine's counter says nothing about a `dca_source_*` call in flight.
    **FIVE types own a `NativeHandleOwner`** — `JuceAudioEngine`,
    `JuceAudioPlugin`, `JuceAudioSource`, and since the wave-3 fix wave
    `JuceMidiInput` and `JuceMidiOutput` too. Both MIDI wrappers held a raw
    `nint _handle` closed with one `Interlocked.Exchange` and had **no drain at
    all**; the OUTPUT one was the urgent half, because it makes live native
    calls (`Send`, `SendAt`, the counters) while `dca_midi_output_close` is a
    bare `delete` that joins the dispatcher thread and destroys the
    `juce::MidiOutput` — so a sequencer flushing note-offs while the UI tore the
    engine down, which is the ORDINARY shutdown path, was a native
    use-after-free rather than a caught `ObjectDisposedException`. Leaving the
    INPUT one is what let the output one be written that way, so both were fixed
    together; pinned by
    `WrapperLifetimeAuditTests.EveryHandleOwningWrapper_DrainsThroughNativeHandleOwner`.
    `JuceDspChain` and
    `JuceDspNode` own **no handle at all**: they are views on the engine and
    every call goes through `_engine.Acquire()` / `TryAcquire()`, so the
    engine's own drain already covers them.
    **The mechanism the 0.9 wave actually introduced, and that no document
    named until now, is `Interop/NativeDestruction` + `INativeChild`.** It is
    the COMPLETION half, deliberately distinct from claiming a handle: an
    internal `NativeDestruction` token per child (`_state`: unclaimed →
    destroying → destroyed, or LEAKED when the native destroy failed), `Run`
    executing the destroy exactly once under a gate, and `Wait` blocking until
    it is complete. The engine keeps the TOKEN even after the child wrapper has
    been collected, so teardown can wait on a destruction whose wrapper no
    longer exists — which a `WeakReference` alone cannot do. A destroy that
    fails parks the callback owner in a static bag rather than freeing it,
    because a leaked native object may still invoke its registered delegate.
    All FOUR of `JuceAudioSource`, `JuceAudioPlugin`, `JuceMidiInput` and
    `JuceMidiOutput` implement `INativeChild`.
    `JuceAudioEngine` keeps a
    WEAK registry of every source/plugin/MIDI input **and MIDI output** it
    hands out (`_midiOutputs`, tracked since the wave-3 drain fix — this line
    omitted it until 2026-09-12; a
    `JuceLoopRegion` is reached through its source) and `Dispose` claims the
    engine handle → stops → disposes every child → *then*
    `dca_engine_destroy`. Claiming the engine handle first is what closes the
    registry: every factory `Acquire`s before it tracks. **The registry is for
    teardown ORDER and roots nothing** — a MIDI input is ALSO held strongly in
    `_midiInputs` until it or the engine is disposed (2026-08-29), because the
    app wires the callback and keeps no handle, and a weak-only registry let
    the first gen-2 collection close the device silently
    (`JuceMidiInputRootingTests`). A `JuceLoopRegion` publishes its poll timer
    STOPPED and arms it only after checking it was not disposed meanwhile
    (`JuceLoopRegionAttachRaceTests`).
  The native side states the same contract at `dotcharter_audio.cpp`'s
  `dca_engine_destroy` (contract D-06), and it was rewritten in the same wave to
  match — it now says the wrapper drains and disposes every child handle before
  the destroy and LEAKS rather than frees a handle whose drain times out. **Do
  not re-open this as a TODO**: this bullet pointed an agent at
  `dotcharter_audio.cpp:~198` as work still to do for a release after it was
  done, which is the kind of note that makes the next reader redo it.
- **Every managed delegate handed to native needs a catch-all body.** A
  reverse-P/Invoke that throws unwinds through C++ frames: a process kill, not
  a catchable error. Three of the four callbacks always had it; the stem
  progress callback did not, and it is invoked from a demucs WORKER thread.
  `NativeCallbackSafetyTests.EveryDelegateHandedToNative_HasACatchAllBody`
  discovers the `[UnmanagedFunctionPointer]` types from `NativeMethods.cs` and
  lints both shapes — a variable assigned a lambda or method group, and a
  factory method whose return type is one of them.
- **Every string crossing this ABI is UTF-8: `[MarshalAs(UnmanagedType.LPUTF8Str)]`,
  never `LPStr`.** `LPStr` is the system codepage (audit #3: localized device names
  read back as mojibake and re-encoded to a byte sequence native could not match,
  so device selection silently fell through to the fallback walk). ASCII is
  identical in both, which is why one `LPStr` survived six ABI versions;
  `NativeMethodsTests.EveryStringParameter_IsMarshalledAsUtf8` reads the
  DECLARATIONS now. The two by-value structs' size and field offsets are pinned to
  the header the same way.
  **And a fixed out buffer truncates at a CODE POINT, never a byte** —
  `dcabi::utf8TruncatedLength` (`src/Utf8Truncate.h`) is the one implementation
  behind both `copyTruncated` and `copyStdTruncated`. A torn sequence decodes to
  U+FFFD, so a short buffer CORRUPTS the tail rather than clipping it; and
  `dca_device_info` writes the same name into `id[128]` and `name[256]`, so a
  byte-wise cut tore the id of any device whose 127th byte landed mid-sequence
  while leaving the name whole — the two then disagree and selection by id falls
  through, which is the `LPStr` mojibake failure from the other end. Pinned by
  `Utf8TruncationTests`, which drives every capacity through a real export and
  decodes with `throwOnInvalidBytes`.
- **A new PUBLIC member ships with a real XML doc comment — the compiler
  enforces it.** `CS1591` is no longer suppressed in `Directory.Build.props`
  (0.9; removing that one line surfaced 252 undocumented members across 42
  files), so an undocumented public type or member is a diagnostic at the site
  that introduced it — **an ERROR since 2026-09-11**, because
  `Directory.Build.props` now sets `TreatWarningsAsErrors` for the whole tree
  (the solution measured 0 warnings at the time, tests and examples included).
  **Never put the `NoWarn` back to make a build green** —
  `PublicApiDocsLintTests` fails if it returns, and also covers the two things
  the warning cannot see: a member missing from the assembly's SHIPPED `.xml`,
  and a summary that is a placeholder restatement of the signature
  ("Gets or sets the Volume.", "The Volume.", empty). Write what it does, its
  units, its threading contract, what it throws and the `dca_*` export it maps
  to. `tests/` and `examples/` set `GenerateDocumentationFile=false` in their own
  `Directory.Build.props`, so the rule reaches the six packable projects only.
- **These packages are NOT MIT, and five of the six say so.** The MIT grant
  covers this wrapper's C# source. `juce_sharp.dll` statically links JUCE 9
  (**AGPLv3** or commercial), Rubber Band v4 (**GPLv2** or commercial), the
  VST 3 SDK and ASIO SDK headers inside JUCE, and NeuralAudio/NAM
  Core/RTNeural/Eigen — so every package that carries or transitively pulls that
  DLL declares `PackageLicenseFile` and ships the repository `LICENSE` (the MIT
  text, then a LICENCE OF THE NATIVE BINARIES statement — every build of
  `juce_sharp.dll` / `juce_vst_host.exe` is AGPLv3 with the GPL terms of Rubber
  Band and ASIO — naming each component) and `THIRD-PARTY-NOTICES.md` (versions,
  copyright lines, licence texts). `ThirdPartyNoticeCoverageTests` fails when a
  `FetchContent_Declare` has no notice naming its repository and pinned tag, or
  when LICENSE's MIT text stops coming first. **`JuceSharp.Dsp` is the ONLY
  one that may keep `PackageLicenseExpression=MIT`**, and only because it has no
  ProjectReference, no PackageReference and no P/Invoke; a package may declare an
  expression or a file, never both (NU5035). Adding a native dependency to a
  package means moving it off the expression. `PackagingLintTests` pins the form,
  the packed LICENSE, that the file still names each component, and that exactly
  one project opts out.
- **Packaging, analyzer and CI settings live in ONE place each and are linted.**
  `Directory.Build.props` owns `<Version>`, the package metadata, and the
  analyzer level (`AnalysisLevel=latest`, `AnalysisMode=Default` plus
  Documentation/SingleFile at `All` — the highest combination measured clean;
  Minimum is DIRTIER than Default, so re-measure rather than assume).
  `Directory.Build.targets` packs the LICENSE. `global.json` pins the SDK with
  `rollForward`, and `.github/workflows/build.yml` installs THAT one
  (`global-json-file:`), runs on pinned runner images, and scopes the default
  token with a top-level `permissions: contents: read`. It runs on push and
  pull_request only on the PUBLIC repo: every job carries
  `if: github.event_name == 'workflow_dispatch' || !github.event.repository.private`,
  so on the private dev repo those events give a run of skipped jobs (0 paid
  minutes) and only the "Run workflow" button runs it. A hosted runner has no
  audio endpoint, so the windows job installs the VB-CABLE virtual device
  before the managed tests (the default-device engine tests need one), and the
  test project sets `DeterministicSourcePaths=false` so the `[CallerFilePath]`
  source lints still find the repo under `ContinuousIntegrationBuild`.
  Every action is pinned by full commit SHA, every checkout sets
  `persist-credentials: false`, the concurrency group includes the event, and
  JUCE / Rubber Band / NeuralAudio are fetched by commit (`*_COMMIT`, the
  commit their `*_TAG` named).
  `CiWorkflowLintTests`
  parses the YAML — never grep CI YAML, a substring matches the comment that
  explains why a block is absent.
- **A shipped package is UNSIGNED, so an `InternalsVisibleTo` naming an assembly
  outside this repo is a coupling anyone can claim, never a protection.** Grant
  one only when a named assembly uses an internal member TODAY. Three of the
  four such grants were dead in 0.9; the one that survives is
  `JuceSharp.StemSeparation.Onnx -> DotCharter.Tests`
  (`HtdemucsChunkScratchTests` constructs the internal `HtdemucsChunkScratch`),
  and `PackagingLintTests.Internals_are_granted_to_the_consumer_repo_exactly_once`
  LISTS the set so a new one is a deliberate edit.
- **Submodule workflow:** commit changes here first (this repo), then bump the
  submodule pointer in the parent DotCharter repo. Don't edit the wrapper to
  paper over a native bug — fix it native-side and rebuild.

## Native rebuild

```powershell
cd src/JuceSharp.Native
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

**On the owner's machine neither of those commands is on PATH.** The only
toolchain installed is **Visual Studio Build Tools 2026** at
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`, and the
working CMake is the copy it bundles:
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
(`ctest.exe` beside it, v4.3.1-msvc1 on 2026-09-11). This file pointed at a
CLion-bundled CMake under `%LOCALAPPDATA%\Programs\CLion` until 2026-09-11;
CLion is not installed on this box and that path does not exist. The
generator is `"Visual Studio 18 2026"`; `"Visual Studio 17 2022"` (what this
file said until 0.56) fails to configure. In a fresh agent worktree the
FetchContent deps are re-cloned from scratch unless you point them at the main
checkout's copies:
`-DFETCHCONTENT_SOURCE_DIR_JUCE=<main>/build/_deps/juce-src` and the same for
`RUBBERBAND` and `NEURALAUDIO` (~33 s to configure instead of a full clone).
**A `FETCHCONTENT_SOURCE_DIR_*` directory is used AS-IS — FetchContent never
checks it out at the pinned tag.** The main checkout's own cache pointed JUCE
at its own `_deps/juce-src` and so compiled 8.0.13 for the whole 8.0.15
release; configure now FAILS when the JUCE sources are not the `JUCE_TAG`
version (the check follows `FetchContent_MakeAvailable(JUCE)`). After a
`JUCE_TAG` bump, clear that cache entry (or delete `build/`) and let JUCE be
fetched fresh; only point it at a copy that is already the new tag.

CMake POST_BUILD deploys `juce_sharp.dll` to `runtimes/win-x64/native/` and the
`juce_vst_host.exe` child next to it.

## Validation

```powershell
dotnet test --project tests/JuceSharp.Tests/JuceSharp.Tests.csproj
cd src/JuceSharp.Native
cmake --build build --config Release --target juce_sharp_rtcheck
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

The DSP soak defaults to ~2 s and honours `JUCESHARP_SOAK_SECONDS`. The
collection rules, the null device (`JuceEngineConfig.EnableNullDevice` per engine,
or the process-wide `DCA_ENABLE_NULL_DEVICE=1`), the two offline
DSP tooling exports and the whole RT gate are in
[docs/agents/rt-safety-and-tests.md](docs/agents/rt-safety-and-tests.md) — read
it before adding a device-dependent test or anything on the callback.

## Keeping this current

**When you change a subsystem in a way that would make its guide misleading — a
new/removed sub-project, a changed invariant, a new gotcha, a moved entry point —
update the routed `docs/agents/*.md` file in the same change**, and add a row here
only if a whole SUB-PROJECT appeared or moved. Terse, accurate bullets with real
file/type names beat prose. This file is auto-loaded for every agent in the repo:
anything that is not routing or a cross-cutting rule belongs in the topic file,
not here.
