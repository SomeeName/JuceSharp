# Native engine ABI — the versioned surfaces

Deep reference for `JuceSharp/AGENTS.md`. `src/JuceSharp.Native/` exports a
versioned C ABI (`include/dotcharter_audio.h`, `DCA_ABI_VERSION`); an
incompatible change bumps that constant and rebuilds the DLL before any managed
code runs. This file carries the per-surface rules for the ABI revisions that
each cost a bug or would have.

## The 2026-09-12 engine pass — three faults, no version bump

Nothing here moved `DCA_ABI_VERSION` (36) or the export count (186). They are
listed first because each is a guarantee that read as delivered and was not.

**`dca_xrun_stats` grew `input_plugin_faults`, and that is ADDITIVE.** The struct
is a Win32-style cbSize contract — the CALLER writes `sizeof` into
`struct_bytes`, the engine fills a local of ITS size and `memcpy`s the smaller of
the two, then reports how much it wrote. Appending at the END is therefore not an
ABI break and needs no new export: an older caller never sees the tail and a
newer caller against an older DLL is told so by the returned `struct_bytes`.
Reordering or resizing an existing field still is a break. `NativeMethodsTests`
pins the offsets and the (now 104-byte) size.

**A THROWING INPUT PLUGIN IS NOW LATCHED, NOT JUST CAUGHT.**
`AudioEngine::processInputChain` wrapped the third-party `processBlock` in
`catch (...) { break; }` and stopped there. Nothing recorded that the plugin had
thrown, so a deterministic fault — an expired trial's licence check, a plugin
prepared for the wrong layout, which is the overwhelmingly common shape — threw
ONCE PER CALLBACK for ever. That is not a survivable cost: `__cxa_allocate_exception`
mallocs, the unwinder takes a global lock over the `.pdata` lookup on Windows, and
the round trip is tens of microseconds against a 2.6 ms budget at 128 frames /
48 kHz — a permanent glitch, with the monitor path dry from the `break` and
nothing anywhere saying why.

* The faulted set is a `uint32_t` bitset per seat, bit *i* = entry *i*, and it is
  **audio-thread-only state**: read and written solely inside `processInputChain`,
  under that seat's `inputChainProcessMutex_`. No atomic, no new lock,
  `rt_lock_sites.txt` unchanged.
* It is re-armed by a **GENERATION COUNTER** (`inputChainGeneration_`), bumped by
  `attachInputPlugin`, `detachInputPlugin` and `reprepareAttachedPlugins`. A
  prepare is a legitimate second chance, because "prepared for the wrong layout"
  is one of the two ordinary causes.
  **Comparing the snapshot POINTER instead is ABA-unsafe and was tried first.**
  Attach and detach each publish a fresh vector and drop the old one, and the
  allocator routinely returns the same address — so a detach+attach compared
  EQUAL, the bitset survived a chain whose entries had all moved, and bit 0 (the
  plugin that had faulted) disabled whatever now sat at index 0. The RT gate
  caught it in one run as "the stage behind it ran 0 blocks". A counter that only
  increases cannot alias.
* A latched entry is **skipped**, not `break`ed: the rest of the chain keeps
  running, so the user loses one pedal rather than everything behind it.
* Reported as `dca_xrun_stats::input_plugin_faults`, **ONE per plugin and not one
  per block**, so a host can tell one broken pedal from a load problem. Managed:
  `XrunStats.InputPluginFaults`. Nothing is logged from the catch body — logging
  on the audio thread is the worse fault, and the counter is the report.
* A chain longer than 32 entries stops disabling past bit 31 rather than
  mis-indexing; those entries keep the pre-fix behaviour and nothing else changes.

Pinned by the storm phase of `RtAThrowingInputPluginCannotKillTheCallback`: 200
blocks with a permanently faulting stage, asserting the plugin is entered at most
twice, that the counter moved by exactly 1, and that the stage BEHIND it kept
running.

**THE DRIVER-CALLBACK BACKSTOP NOW MAKES THE SILENCE IT CLAIMED.** Its comment
said "the block of silence this produces" while the `catch (...)` body was empty
and `renderBlock` never clears `out` — the first thing it does is hand the buffer
to `juce::AudioSourcePlayer`, which is what fills or zeroes it. So a throw from
inside `player_` left the DRIVER'S buffer, which several WASAPI/ASIO backends
make the PREVIOUS block repeated at full level, and a throw after it left the
song without the monitor. The catch now clears every output channel
(`FloatVectorOperations::clear` — no allocation, no lock). It must still stay a
bare `catch (...)`: logging, allocating or rethrowing there are each worse than a
silent block.

**`dca_engine_show_asio_panel` BLOCKS THE MESSAGE THREAD, AND THAT IS THE
DOCUMENTED TRADE.** It is the one export in the file that can occupy the JuceHost
message thread indefinitely: `showControlPanel()` is a modal vendor dialog, and
while it is up every other `callSync` export queues behind it — `_start`, `_stop`,
`_reconfigure`, `_set_device_type`, both device enumerations and
`dca_engine_destroy`. Before the W4-N5 marshalling only the calling thread
blocked. The managed consequence is stated rather than fixed: closing the app
with the panel open makes `NativeHandleOwner`'s drain time out at 60 s, log at
Error and LEAK the engine — a leak beats a use-after-free. It is NOT a
self-deadlock (`callSync` short-circuits on
`MessageManager::existsAndIsCurrentThread()`).

*Why it was not "fixed":* `callAsync` unblocks this caller and nothing else — the
panel still owns the message thread — and the export would lose the three error
answers it returns today ("no device", "no control panel", "the device
declined"), which `TryShowAsioControlPanel` surfaces. Running the panel on its own
thread hands the raw device pointer to a thread that does not own the driver's COM
apartment, i.e. it reopens the exact fault the marshalling exists to close. If
this is revisited, the shape is a host-owned modal-loop escape in `JuceHost`. It
is not automatable against a real driver, and no test pretends to be.

**Timer resolution is released in `~MidiOutputDevice` too**, not only in
`shutdown()`. A device dropped between `open()` and a shutdown left the whole
PROCESS at 1 ms resolution for good; `timeBeginPeriod` is refcounted per process,
so the release is idempotent and two devices are fine. A device whose shutdown
timed out is leaked along with its request — the same trade the handle drain
makes.

**Two comments that lied and now do not.** `MidiOutputDevice::failedCount()`'s
doc said the counter was "not yet surfaced through the ABI" for a release AFTER
`dca_midi_output_get_counts3` shipped — and prescribed the design v36 deliberately
rejected (a third out-param on the existing export, which has callers passing two
pointers). And the bridge MIDI carry's overflow branch is dead by construction:
the drain is capped at the remaining room, so `lost` is always 0. The carry is
bounded and lossless; sustained back-pressure surfaces as `room == 0`, and the
drop is accounted on the PRODUCER side (`dca_engine_get_midi_timing_stats`'
`dropped`), which is where a diagnostician should look.

## ABI v47 — the external process insert

- **Six additive exports, one struct, two appended statuses.**
  `dca_engine_extproc_open_for_player(engine, player, mapping_name, magic)`,
  `dca_engine_extproc_close_for_player`, `dca_engine_extproc_get_stats_for_player`
  (`dca_extproc_stats`, struct_bytes contract like `dca_xrun_stats`), each with a
  one-line seat-0 alias (`dca_engine_extproc_open` / `_close` / `_get_stats`);
  `DCA_EXTPROC_STATE_*` (CLOSED, WAITING, ACTIVE, RATE_MISMATCH, BAD_LAYOUT);
  `DCA_ERR_SAMPLE_RATE = 7` and `DCA_ERR_NOT_FOUND = 8`. Managed:
  `JuceAudioEngine.OpenExternalProcessInsert` (→ `ExternalProcessInsertOpenResult`,
  never throws for a refusal), `CloseExternalProcessInsert`,
  `TryGetExternalProcessInsertStats` (→ `ExternalProcessInsertStats`), all on
  `IJuceAudioEngine`.
- **What it does.** Per seat, while open, the seat's dry chain buffer (after the
  input trim and the chart source, before the rig) is written block by block into
  a named shared-memory ring ANOTHER process created, and that process's processed
  block REPLACES the rig, the input VST3 chain and the sandbox for that seat. The
  input EQ, the meter and the monitor mix follow unchanged. Nothing open: one
  seq_cst load per seat per block and today's path.
- **The ring** (`src/ExternalProcessInsert.h`): a 64-byte header (8-byte magic the
  CALLER names, version 1, rate, block frames, ring blocks, in/out channels, four
  free-running uint32 block counters, four fault counters), a mono float input
  ring at 64, a 1- or 2-channel INTERLEAVED output ring at
  `64 + align64(ring * block * 4)`. Producer writes then release-stores its
  counter; consumer acquire-loads. Every offset is pinned by `static_assert`
  against `offsetof` of the format's own field-by-field struct, and the managed
  fake reads the offsets out of that header (`SharedRingFormat`) — a second
  literal is how two sides agree and are both wrong.
- **Real time.** No allocation, lock, wait or system call on the callback; the
  callback never waits on the other process. Host blocks of any size are
  re-blocked into ring blocks (one ring block of fixed delay). The reader keeps a
  cushion of `ceil(largest callback / block) + 1` blocks before it plays; a block
  not back in time is an UNDERRUN and the seat plays its DRY input for it (never
  silence; 64-frame ramp at each processed/dry switch), and re-primes. A full
  input ring is an OVERRUN (the block is dropped, unread input is never
  overwritten). Blocks piling up beyond cushion + 2 are dropped as LATE. The
  host's underrun/overrun counts are mirrored into the header for the other
  side's log. No event is signalled.
- **Lifetime.** `open` maps (`OpenFileMappingW` + `MapViewOfFile`) and validates
  on the caller's thread and publishes the session with a seq_cst store; the
  audio thread takes it at the silent bottom of a 5 ms dip (the
  `dcdsp_rsamp_table.h` pattern) and announces it in a hazard slot before using
  it. `close` unpublishes, waits (bounded ~200 ms, caller's thread) for the audio
  thread to let go, then unmaps; a session still held is unmapped by the next
  control call or the destructor. `audioDeviceStopped` releases the hazard.
- **Rate.** No resampler: `open` refuses with `DCA_ERR_SAMPLE_RATE` (nothing
  opened) when the device does not run at the ring's rate; DotCharter requests
  that rate in place and opens again. A later reconfigure to another rate keeps
  the session but bypasses it (RATE_MISMATCH: the seat runs its own rig) until
  the rate matches again.
- **Engine rebuild:** a reconfigure keeps the session (per-engine state, the
  engine object survives); a destroyed engine loses it and the host re-opens it
  (DotCharter: `ExternalProcessInserts.ReplayTo` from
  `AttachPerPlayerStateToEngine`).
- `AbiV47Tests` (version, ordinals, struct layout against the header, exports in
  the built DLL, null handles, seat-0 aliases), `SharedRingFormatTests`,
  `ExternalProcessInsertNullDeviceTests` (a paced Null engine and a fake other
  process echoing x 0.25: the seat's peak follows it; stopping the echo falls back
  dry and counts underruns and overruns; refusals), and the RT scenario
  `RtExternalProcessInsertExchangesWithoutAllocatingAndFallsBackDry` (waiting,
  connected, disconnected and the close dip, all armed: zero allocations).

## ABI v46 — the audio callback's load (JuceSharp #29)

- **One additive export.** `dca_engine_get_callback_load(engine, out_average,
  out_peak)`: the callback's cost as a FRACTION of the buffer period (1.0 = the
  whole block's wall-clock budget; above it is a deadline overrun). Either
  out-pointer may be NULL; both NULL or a NULL engine is `DCA_ERR_INVALID_ARG`,
  with the non-NULL out-params zeroed first. Managed:
  `JuceAudioEngine.TryGetCallbackLoad` → `CallbackLoad(Average, Peak)`, on
  `IJuceAudioEngine`.
- **Shape.** The average is an EMA with a 0.5 s time constant whose per-block
  coefficient comes from the block's own duration (a variable-block driver
  averages TIME, not callbacks). The peak is the largest block load decaying with
  a 1.5 s time constant. Both are computed in `AudioEngine::recordCallbackTiming`
  (the same QPC delta the xrun counters use) and published as two relaxed
  `std::atomic<float>` (static-asserted lock-free); a read is two loads, so any
  number of pollers see the same numbers. **The peak is NOT reset on read** — a
  reset-on-read peak would let the health logger steal the UI meter's.
- **Why not a field on `dca_xrun_stats`** (which is append-extensible): every field
  there is a "since `dca_engine_reset_xrun_stats`" counter, and a decaying meter
  that a reset must NOT touch would be the one field breaking that rule.
- **Engine rebuild:** nothing to replay. Both values restart from 0 in
  `resetXrunSession` (device session start) and are zeroed in
  `audioDeviceStopped`; the session's 4 warm-up blocks are not counted, so a
  rebuild paints no phantom spike.
- `AbiV46Tests` (paced Null device: a load above 0; manual pump: exactly 0;
  disposed: false) and the RT scenario `RtCallbackLoadIsMeasuredWithoutAllocating`
  (zero allocations, the meter moves, the peak is bounded by `max_callback_ns`,
  0 across a reopen and its warm-up).

## ABI v45 — the `rsamp` chain table loaded at run time (JuceSharp #25)

- **Two additive exports.** `dca_rsamp_set_table(data, length)` loads (or, with
  `NULL, 0`, unloads) the chain table every `rsamp` module runs;
  `dca_rsamp_table_row_count()` says how many rows it carries a chain for (0 = none).
  Managed: `JuceSharp.Dsp.RsAmpChainTable` (the writer) and `JuceRsAmpTable`
  (`TryLoad`, `Unload`, `LoadedRowCount`). No handle: the table is PROCESS-wide.
- **Why.** The table is Rocksmith 2014's own data (each amp's soundbank chain and
  RTPC curves, the `init.bnk` defaults baked in). It was compiled in from a
  generated header (dcdsp_rsamp_chains.h, deleted), which kept the repository from going
  public. It is gone; DotCharter builds the table from the player's own install.
- **The blob** (`src/dsp/cores/dcdsp_rsamp_table.h`) is validated WHOLE before
  anything changes — magic, format, counts, every index, every float finite, the
  row's key against the public row list (`dcdsp_rsamp_models.h`), consumed exactly
  — and a refusal is `DCA_ERR_INVALID_ARG` with the reason logged; the table in
  force stays.
- **Real-time publication.** Parse, validate and retention run on the caller's
  thread under the registry's mutex; the audio thread reads the table with ONE
  acquire load of `gCurrentTable` at the top of a block, and the core swaps at the
  silent bottom of its 5 ms model-change dip. Tables are never freed (a core may
  still run one); an identical reload republishes the kept copy, and at most 32
  distinct tables may be loaded per process.
- **Engine rebuild:** nothing to replay natively (the table is process-wide and
  outlives every engine); DotCharter still hands it over on every engine build,
  because the tables may be published before any engine exists.
- `AbiV45Tests`, `Dsp/GearRsAmpTests` (synthetic tables), the native `rsamp_table`
  test and the RT scenario `RtRsAmpTableLoadedWhilePlayingSwapsWithoutAllocating`.

## ABI v44 — Wwise-free Wwise-Vorbis .wem encoding (DotCharter #317)

- **Two additive exports.** `dca_wem_encode_vorbis(interleaved, frames, channels,
  sample_rate, *out_data, *out_size)` returns a natively allocated `.wem`;
  `dca_wem_encode_free` releases it. No engine. Managed: `JuceWemEncoder.EncodeVorbis`.
- **Why.** Rocksmith 2014 does not play IMA-ADPCM `.wem` audio (the timeline never
  advances; owner-confirmed 2026-10-11, DotCharter #717), so a package built
  without Wwise needs Vorbis, and only Wwise wrote that.
- **What makes it safe.** The encoder (`src/wem/WemVorbisEncoder.cpp`) is the exact
  inverse of the decoder's rebuild and pins its own setup header against the one
  Wwise writes (FNV-1a-64); constants copied from Wwise files are valid only for
  that header. 48 kHz stereo only. The decoder's bit I/O and codebook library moved
  VERBATIM into `src/wem/WemVorbisCommon.h`, shared by both.
- **Field rules and their evidence:** [`wem-encoder.md`](wem-encoder.md).
  `AbiV44Tests`, `Wem/WemEncoderTests`.

## ABI v43 — the load-time rate conversion, once, into a file (DotCharter #435)

- **Two additive exports.** `dca_convert_audio_for_device(src, dst, device_rate,
  *out_bits)` needs no engine. `dca_source_get_preload_digest(source, *hash,
  *step, *frames, *rate, *converted)` is a test seam: an FNV-1a 64 of a
  preloaded source's int16 samples, plus its step, frame count, held rate and
  whether the open converted. Managed: `JuceAudioEngine.ConvertAudioForDevice` →
  `JuceDeviceRateConversion`, and `JuceAudioSource.GetPreloadDigest` →
  `JucePreloadDigest`.
- **Why.** A preload UPsamples a file below the device rate on EVERY open
  (`TransportSource::tryPreloadIntoMemory`, `src/LoadResampler.h`). The owner's
  device runs at 48 kHz and Rocksmith songs are 44.1 kHz, so every repeat open
  paid ~0.3-0.9 s for it. A host now writes the converted song once and opens that.
- **Bit-identical by construction, not by tolerance.** `dotcharter::convertFileForDevice`
  (`AudioEngine.cpp`) runs the preload's own steps: the same formats (basic +
  `.wem`), the shared `decodeWholeReader`, the shared `loadWillUpsample` gate,
  the same `upsampleDecodedBuffer` call and the same `Int16PreloadSource`
  quantiser. **Change one of these and the other changes with it, or the cached
  file stops matching.** What it writes (`src/DeviceRateWav.h`):
  - **Exactly what the preload holds, at 16 bits:** its int16 samples as PCM, and
    its step in a `dcps` chunk between `fmt ` and `data` (`u32 version 1 | f32 step |
    u64 frames | u32 source rate | u32 0`). JUCE's 16-bit reader decodes `v` as
    `v/32768`, which re-quantises to `v` at 1/32768, and the open then ADOPTS the
    stored step (`Int16PreloadSource::adoptStoredStep`). So a loud master whose
    upsample overshot full scale (step 1.06-1.16 x 1/32768 on all six real songs
    measured) is bit-exact too, at ~1.09x the 44.1 kHz original's bytes (the rate
    ratio) instead of the 2.2x a 32-bit float file cost.
  - **Nothing** (`*out_bits` 0, `DCA_OK`) when the load would not convert, i.e. the
    file is at or above the device rate. The engine never DOWNsamples at load.
- **Every reader honours the step.** `TransportSource::openFile/openMemory` probe
  the chunk: the preload adopts it, or -- when the device is at a HIGHER rate than
  the file -- scales the decode by `step x 32768` before converting again
  (`applyStoredStepBeforeConversion`, shared with the converter); the STREAMING
  path (budget exhausted) wraps the reader in `devwav::StoredStepGainSource`. The
  waveform, `dca_decode_audio_file`, the stem separator and managed `WavIo.Read`
  scale too. Any other reader hears an ordinary 16-bit WAV up to ~1.3 dB quiet.
- **A damaged device-rate file is refused, never played wrong.** A `dcps` chunk
  that does not add up -- short chunk, unknown version, step outside
  [1/32768, 1), fmt not 16-bit PCM, data not `frames x channels x 2` bytes, data
  truncated, or a reader that disagrees -- fails `dca_source_open_*` with
  `DCA_ERR_DECODE` before the previous source is torn down, and fails the
  converter. A host re-derives the song (DotCharter re-decodes the `.wem`).
- **Alignment.** The file is at the device rate, so its open converts nothing
  (`playbackSampleRate_` = file rate = device rate) and holds the same frame
  count. Length in seconds and every seek are unchanged. `device_rate` must be a
  whole number of Hz, because the WAV header stores an integer and a fractional
  mismatch would convert again.
- **Failure.** On a failure `dst` is deleted, never left half written. Missing or
  undecodable source, or a conversion failure, returns `DCA_ERR_DECODE`. An
  unwritable destination returns `DCA_ERR_INTERNAL`. Bad args, a bad rate or
  src == dst return `DCA_ERR_INVALID_ARG`. The digest returns
  `DCA_ERR_INVALID_ARG` for a NULL/orphaned or STREAMING source and zeroes every
  out first.
- **Not on the callback.** Both exports run on load/worker threads. The digest
  reads `Int16PreloadSource::channelSamples`, a const accessor the callback
  never calls; the chunk probe runs on the open's thread, and
  `StoredStepGainSource` on the read-ahead thread. The RT gate is unaffected.
- **Measured (2026-10-09).** Release DLL, Null device at 48 kHz, CPUs 8-15 below
  normal, OpenMemory as DotCharter opens a song, read + open, median of 5. Six
  cached 336-446 s songs went from 488-669 ms to 204-261 ms, digest equal on all
  six; each file is 1.088x its original (e.g. 78.7 MB -> 85.7 MB). Converting
  once cost 0.57-0.72 s.
- Pinned by `AbiV43Tests`: version, both exports in the header AND the built
  DLL, the enum, identity at the 1/32768 step AND at an overshooting step (same
  hash, step, frames, rate and length as the load-time conversion on a 48 kHz
  Null device, 76 header bytes + 4 per frame), the level kept on a 44.1 and a
  96 kHz device, damaged files refused, the offline decode and `WavIo` honouring
  the step, the digest seeing a one-sample change, no file when no conversion is
  needed, argument rejection, and null-source zeroing. The native
  `device_rate_wav` ctest pins the writer, the probe's damage cases and the
  STREAMING gain source, which the managed suite cannot force.
  `JUCESHARP_DEVICE_RATE_BENCH=1` (+ `_SONGS=N`, `JUCESHARP_WEM_CORPUS_DIR`) runs
  the table above over song WAVs copied to a temp folder.

## ABI v42 — native Wwise-Vorbis (.wem) decoding (DotCharter #426)

- **Seven additive exports** (`dca_wem_open_file`, `_open_memory`, `_get_info`,
  `_read_f32`, `_read_s16`, `_seek`, `_destroy`), the 48-byte `dca_wem_info` and
  `DCA_WEM_VARIANT_*` (0 AUTO … 6 HEADER_TRIAD). A `dca_wem_t` is a plain heap
  object with no engine, device or callback. It is single-threaded and may never
  be called from an audio callback. `out` is NULL on every failure. NULL args and
  unknown variants return `DCA_ERR_INVALID_ARG`, and anything undecodable returns
  `DCA_ERR_DECODE` with the reason sent to the log callback.
- **Behaviour change:** `AudioEngine`'s format manager (and
  `dca_decode_audio_file`, `dca_generate_waveform*`) also registers
  `dcwem::WemAudioFormat`. `dca_source_open_file` on a `.wem`, and
  `dca_source_open_memory` with hint `"wem"` or no hint, now open it through the
  ordinary transport. It is decoded on the load thread by `tryPreloadIntoMemory`
  into the 16-bit preload. `render_to_wav` and the stem separator still use the
  basic format set only.
- **Where it lives:** `src/wem/WemVorbisDecoder.{h,cpp}` is the decoder: a port of
  ww2ogg's `wwriff.cpp`/`codebook.cpp` that rebuilds the identification, comment
  and setup header PACKETS and feeds every audio packet to JUCE's bundled
  libvorbis (`vorbis_synthesis`). There is no Ogg container. libvorbis is reached
  by declaring its `extern "C"` API inside `juce::OggVorbisNamespace`, exactly as
  `juce_OggVorbisAudioFormat.cpp` includes it, so the DLL has ONE libvorbis.
  `src/wem/WemAudioFormat.{h,cpp}` is the `juce::AudioFormat`.
  `src/wem/wem_codebooks.h` is GENERATED by `tools/gen_wem_codebooks.py` from
  ww2ogg's two packed codebook libraries. It records their SHA-256, origin and
  licence. Never hand-edit it.
- **The output contract is the managed path's** (Ww2Ogg.Core + NVorbis, which
  DotCharter used before):
  - The variant order is DotCharter's (aoTuV first).
  - Mod packets are rebuilt exactly as ww2ogg does it, including the peek at the
    next packet.
  - The LENGTH is the decoded total clipped to the end granule. For granule-less
    files (vorb `0x2A`/embedded, every Rocksmith `.wem`) that is the vorb sample
    count. Otherwise it is the last packet's granule. That clip is where
    Ww2Ogg.Core's page granules make NVorbis stop.
  - Float output is NOT clipped (NVorbis clips to ±1). `dca_wem_read_s16` applies
    DotCharter's WAV formula, so it clamps.
- **libvorbis ignores the VALUES of a long block's previous/next window flags.**
  Its overlap-add uses the real neighbouring block sizes, so only the presence of
  the two bits matters, because it shifts the rest of the packet. A wrong flag
  value therefore cannot be caught by any output comparison. A missing pair of
  bits fails every mod-packet test.
- **Seek is exact**: `vorbis_synthesis_restart`, feed packet k-1 and discard its
  output (libvorbis returns none for the first block after a restart), then drop
  the leading frames of packet k. Per-packet output counts come from a header-only
  pre-scan (`prevBlock/4 + curBlock/4`).
- Managed: `JuceWemDecoder` / `JuceWemInfo` / `JuceWemVariant`
  (`docs/api/juce-wem-decoder.md`). Pinned by `AbiV42Tests` (version, every
  export in the DLL, variant ordinals and struct layout against the header,
  engine registration) and `Wem/WemDecoderTests`. The latter decodes `.wem` files
  that `Wem/WemFixtureWriter` builds from the committed synthetic Ogg fixtures,
  in every layout (inline, aoTuV and default library ids, mod and standard
  packets, granule headers, header triad), against Ww2Ogg.Core + NVorbis. It
  also covers seek, s16, the engine source, truncation and corrupt packets.
  `Wem/WemCorpusTests` does the same on the owner's PSARC cache (skips without
  it; `JUCESHARP_WEM_CORPUS=all`), and `Wem/WemBenchmarkTests` times all three
  paths (`JUCESHARP_WEM_BENCH=1`).
- **A `dca_wem_t` decodes on worker threads (DotCharter #434, no ABI change).**
  `openWem` opts every `dca_wem_*` handle into
  `WemVorbisDecoder::enableParallelDecode`; the engine's `WemAudioFormat` reader
  does NOT (it seeks per block). A read then decodes BATCHES of
  `threads x 256` packets: each batch is cut at packet boundaries into one
  segment per worker, each worker restarts its OWN libvorbis state over the
  decoder's shared `vorbis_info` (read-only once the decoder's own
  `vorbis_synthesis_init` built `fullbooks`; lanes are created on the caller's
  thread) and primes with the packet before its first one, output discarded —
  the seek fact above, so the joined output is BIT-IDENTICAL to the serial
  decode. The next batch decodes while the caller drains the current one. Workers
  copy the caller's MXCSR. A segment whose packet fails, or decodes to a frame
  count the pre-scan did not predict, ends parallel mode at that packet and the
  rest is decoded serially (primed as seek primes), so even the failure is the
  serial one. Threads: `min(4, hardware threads / 2)`; a file under two
  segments' worth of packets stays serial. `DCA_WEM_DECODE_THREADS` (1 = serial,
  clamped to 16) and `DCA_WEM_LANE_PACKETS` override, read through
  `juce::SystemStats::getEnvironmentVariable` at open (tests and measurements
  only). Memory: two batches of planar float, ~10 MB for a 48 kHz stereo song.
  Measured on 8 cached Rocksmith WEMs (Release DLL, Idle priority, CPUs 8-15,
  16-bit reads of 8192 frames as DotCharter's WAV writer does): 211-691 s songs,
  serial 416-1756 ms, 4 threads 126-548 ms, 2.7-3.2x. Pinned by
  `Wem/WemParallelDecodeTests` (float and 16-bit identity on every synthetic
  layout cut three ways, seek on a shared and on fresh decoders, real WEMs when
  the cache is present; `JUCESHARP_WEM_BENCH=1` times serial vs 2 vs 4 threads).
  The decoder logs `wem: decoded N packets; parallel batches=B max segments=S`
  at DEBUG when a handle reaches its end.

## ABI v41 — the Null device is a per-engine create flag (DotCharter #85)

- **One additive export, `dca_engine_create2(cfg, flags, out)`.**
  `dca_engine_create` is it with flags 0. `DCA_ENGINE_CREATE_NULL_DEVICE` (bit 0)
  registers the "Null" type with THIS engine's device manager and lets its mirror
  open the Null device; `DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL` (bit 1) also puts
  the devices that type creates in manual-pump mode and implies bit 0. An unknown
  bit is `DCA_ERR_INVALID_ARG` (rejected before any engine exists, `*out` NULL).
- **The flags live on the engine** (`AudioEngine::setNullDeviceOptions`, set before
  the first `start()` and before the handle is published), so a later
  `dca_engine_start` on a device-less engine registers the type the same way.
- **Precedence: flag OR env.** `DCA_ENABLE_NULL_DEVICE=1` / `DCA_NULL_DEVICE_MANUAL=1`
  still enable it for every engine in the process (the RT gate, the env-path tests
  and the examples rely on that); a 0 flag never hides it.
- **Why:** the env var is process-wide. DotCharter's offline rig host set it around
  its engine's construction during ordinary play, and a live-engine rebuild on
  another thread in that window listed the Null type to the user.
- Managed: `JuceEngineConfig.EnableNullDevice` / `NullDeviceManualPump` →
  `CreateFlags()`; the `JuceAudioEngine` constructor always calls
  `dca_engine_create2`. Pinned by `AbiV41Tests` (version, export in the DLL, flag
  values against the header, unknown-bit rejection, flag on/off with the env
  var cleared, and a flag-built engine leaving a concurrent engine without the type).

## ABI v40 — instrument models for the built-in voices, and packs released when off (#50, #61)

- **Four additive exports.** `dca_engine_chart_source_set_instrument_for_player`
  (model, pickup, flags: a guitar model sets the GUITAR voice's selection, a bass
  model the BASS voice's, both kept per seat), `_get_instrument_for_player`
  (out-params zeroed before the first failure), `dca_chart_model_get_info`
  (cbSize struct: pickups, default, coil split, packs installed, which positions
  are RECORDED unconverted — `recorded_mask` every articulation set,
  `recorded_any_mask` at least one) and `dca_chart_voice_pack_bytes` (the sample
  memory alive in the process — since #417, with no ABI change, the DECODED zones
  of the engine's lazy packs, so it grows with the chart; the status' `pack_bytes`
  is read live too). Values `DCA_CHART_MODEL_*`, `DCA_CHART_PICKUP_*`,
  `DCA_CHART_INSTRUMENT_COIL_SPLIT` are `static_assert`ed against
  `dcvoice_models.h` — ONE table. An unknown model, a pickup not on the model's
  selector or an unknown flag bit is REJECTED, never clamped.
- **`dca_chart_source_status` grew six APPENDED fields** (model, pickup, flags,
  sampled, pack_bytes, voice_loaded; 112 bytes) under the cbSize contract, so a
  v38 caller's 80-byte struct is still filled.
- **Behaviour changes, deliberate.** `_set_voice_` while the source is DISABLED
  only records the kind (v38 built the voice at once). Disabling RELEASES the
  voice — and with the last reference its pack — once it is silent, on the
  CHART REAPER thread (`AudioEngine::chartReaperLoop`, started on first use,
  joined first in `~AudioEngine`); a disabled source holds no sample memory.
  Enabling builds the selected voice before it publishes the flag. A selection
  change while ENABLED builds a new voice OUTSIDE `chartMutex_` (the seat's
  `controlMutex` serialises the seat's control calls instead, so a pack load
  never blocks a status poll or a device start) and CROSS-FADES: the old slot is
  published as `fading` BEFORE the new `voice` (a block that sees the new voice
  must see the old one fading), the callback calls `fadeOutAll` once and renders
  it until silent, then raises `fadeDone`; the reaper frees it (400 ms deadline
  for a device with no callbacks). A device restart drops a fade in flight.
- **Defaults at engine start:** guitar SEMI_HOLLOW / BRIDGE (the v38/39 sound —
  the Gretsch as recorded), bass JAZZ / NECK_BRIDGE. **A rebuild starts there
  again; the host replays its selection** (DotCharter: `PushChartSynthTo` writes
  both selections first).
- Pinned by `AbiV40Tests` (version, exports in the DLL, null handles, layouts,
  values against the header, the model table's shape), `ChartInstrumentNullDeviceTests`
  (packs held only while on, only the selection's own; switch, release, re-enable),
  `ChartVoiceRealPackRamTests` (opt-in, the real packs: lazy load, a whole-neck chart, release),
  `RtChartModelSwapCrossfadesAndReleasesThePack` and the `instrument_models` ctest.

## ABI v39 — the chart note carries the capo (#51)

`dca_chart_note.capo` (int32, offset 64) takes the first of the four reserved
floats, so the struct stays 80 bytes, `DCA_CHART_NOTE_V38_BYTES` is unchanged and
a v38 caller's zeroed slot reads as capo 0. `pitch` is exactly what it was — the
capo was always folded into it — but natural-harmonic NODES are measured from the
capo (capo 2 + charted fret 14 is the octave node of the capo'd string), and
`pitch - fret` cannot recover that. The scheduler copies it into
`dcsynth::ChartNoteOn::capo`; the table rejects a capo outside 0..24. No export
was added or resignatured. Pinned by `AbiV39Tests` (version, offset, the header
declaring the field), the `chart_scheduler` ctest's `CapoReachesTheVoice` and
`ChartNoteEventBuilderTests` on the DotCharter side.

## ABI v38 — the chart source, the song clock and the instrument route

NINE additive exports and three structs, nothing reordered or resignatured,
everything OFF / empty at engine start (a host that never calls them runs the
v37 path byte for byte: the song clock is only sampled while a seat's chart
source is enabled, and the seat loop only grows a generator branch for such a
seat). No seat-0 aliases: they are new, not pre-v28.

- **What it is.** A per-seat NOTE GENERATOR (`src/synth/`) that renders a
  chart's notes as a mono DI signal into the seat's RIG INPUT — after the input
  fill and trim, before the in-house rig, the VST chain and the sandbox — so the
  whole rig processes it like the guitar (#39). It is mixed in AFTER
  `writeInputCapture`: **the capture ring never hears it**, so detection,
  scoring, the tuner and the DI recorder are blind to it by construction. It
  never reaches the post-master instrument bus. `DCA_CHART_SOURCE_MIX` (default)
  sums it with the guitar; `_REPLACE` gives the rig only the generator while the
  ring still hears the guitar. A seat whose device has no input still runs its
  rig while a generator is enabled (`AudioEngine::mixSeatGenerators`).
- **The song clock.** `dca_engine_set_song_clock_source` binds ONE transport
  source; every block, right after the transport pull, the callback reads its
  audible position, play flag, seek generation and tempo
  (`TransportSource::readSongClockRt` — plain reads and atomics, no lock) into
  `songClock_`. `TransportSource` bumps `seekGeneration_` on every seek and
  stop, which is what makes a seek smaller than the jump tolerance still a seek.
  The binding is read under a seq_cst Dekker handshake (`songClockBusy_`), and
  `TransportSource::releaseWiring` calls `forgetSongClockSource` FIRST, so a
  destroyed or re-opened source is never read after its transport is freed (a
  re-open rebinds itself: `wasSongClock_`).
- **The scheduler** (`ChartScheduler.h`, audio thread only, pure C++17) turns
  consecutive clock samples into half-open windows of song time that TILE the
  timeline — continuous play takes `[previous end, new position)`, so no note
  is skipped or fired twice, and a stretcher's backward wobble yields an empty
  window rather than a re-fire. A DISCONTINUITY (new seek generation, resume
  after pause, a new table / voice / offset, or an unannounced jump beyond one
  block + 250 ms) chokes the voice and re-seats the window at
  `[pos - block*tempo, pos)`; notes held across the new start are CHASED
  (started at sample 0 with `chaseSeconds`). Events land at
  `floor((t - start) / width * n)` (+1e-6 so a time ON a sample boundary is on
  that sample). The voice renders every DEVICE sample whatever the tempo, so
  practice speed moves notes in time, never in pitch. Bounded: 64 tracked notes
  (the oldest is stolen and counted), 64 note-ons per block (the rest dropped
  and counted), a 512-note chase scan, bend/slide controls every 32 samples.
- **Publication** is the re-amp take's: `ChartNoteTable::build` validates and
  copies on the caller's thread (a list is accepted WHOLE or rejected and the
  previous table stays), the voice is built and prepared there too, both are
  published by an atomic swap with a SERIAL (never compare pointers — a freed
  table and its replacement can share an address), and a replaced one is freed
  on the control thread after a seq_cst Dekker handshake on `chartBusy_`
  (bounded 2 s, then LEAKED and logged). `audioDeviceAboutToStart` re-prepares
  every published voice and sizes the per-seat scratch with 2x headroom; a
  bigger block skips the generator for that block rather than growing.
- **The upload** (`dca_chart_note`, 80 bytes, `dca_chart_bend_point`, 16) is
  APPEND-EXTENSIBLE through its stride argument (`DCA_CHART_NOTE_V38_BYTES` is
  the floor; the engine reads the smaller of the caller's stride and its own
  sizeof). Times are CHART seconds (the axis `dca_source_get_audible_position_seconds`
  reports). `pitch` is what the string SOUNDS (tuning, capo, cent offset, bass
  octave all in), fractional allowed. `release_seconds` is the HOST's decision
  (ring, legato, hand shape). `fret` is as charted, from the NUT, and `capo`
  carries the arrangement's capo, because node positions (natural harmonics) are
  measured from the capo and cannot be recovered from `pitch - fret`. Articulations are `DCA_CHART_NOTE_*` bits —
  palm / fret-hand mute, natural / pinch harmonic, hammer-on / pull-off / tap,
  vibrato, tremolo, accent, unpitched slide, slap / pop / picked, let-ring,
  link-next, staccato, ghost, bass, and HOW it ends (`END_DAMPED` = cut by the next
  note on its string, `END_SHAPE` = rang to its hand shape's end) — plus `slide_to_fret`, a bend curve, a chord
  id (the engine derives chord size and each note's index by string) and a strum
  field (`UNKNOWN` today: the chart model has none). **A new articulation is a
  new bit**, no bump.
- **Voices** (`ChartVoice.h`) are the step-1 seam: `prepare` on a control
  thread (allocate there), then `reset` / `noteOn(ChartNoteOn)` /
  `noteOff(id, Release|Choke)` / `allNotesOff` / `setNoteControls(bend, slide)`
  / `render(add mono)` / `activeNotes` on the audio thread, all `noexcept`. The
  catalogue is `ChartVoiceFactory.cpp` — the one place a kind becomes an object,
  and what `dca_chart_voice_is_available` answers from. `PLACEHOLDER` (a
  Karplus-Strong pluck, `PlaceholderPluckVoice.h`) ships now; `GUITAR` and
  `BASS` are reserved ordinals that answer 0 until their voices are registered.
  The voice's output is NaN-scrubbed and gain-ramped before it enters a rig.
- **Instrument route.** `dca_engine_set_instrument_route(engine, seat)` sums the
  IN-PROCESS instrument (rendered before the seat loop by
  `renderInstrumentWork`) to mono into that seat's rig input, after its capture
  ring, and takes it OFF the master; `-1` (engine start) is the old after-the-master
  mix. A route naming a seat that is not live this block falls back to the
  master. **The SANDBOXED instrument cannot follow**: its block comes back from
  the child in the same round trip that runs seat 0's input chain, i.e. after
  the rig has run, so routing it would cost a whole block of latency — it stays
  on the master and a host must show the option unavailable there.
- **Rebuild:** a new engine starts with every chart source off and empty, no
  clock, route -1. The host replays (DotCharter: `ReplayChartSynthTo` from
  `AttachPerPlayerStateToEngine`, and `BindSongClock` wherever the song source is
  published).

Pinned by `AbiV38Tests` (version, presence of all nine exports in the built
DLL, null-handle rejection, struct layouts, every flag / ordinal read off the
header), `ChartSourceNullDeviceTests` (notes reach the seat's post-rig meter,
the capture ring stays exactly silent, a rejected list keeps the previous
table, a disposed clock source unbinds), the binding theory in
`NativeMethodsTests`, the `chart_scheduler` ctest (`tests/synth/`) and the RT
gate's `RtChartSourceFeedsTheSeatRigNotTheCaptureRing`.

## ABI v37 — spillover, re-amp injection and a cursorless capture read

TEN additive `_for_player` exports, nothing reordered or resignatured; both
features are OFF / empty at engine start, so a host that never calls them runs
the v36 path byte for byte. No seat-0 aliases: they are new, not pre-v28.

- **Spillover** — `dca_engine_dsp_{set,get}_spillover_for_player`,
  `dca_engine_dsp_get_spill_tail_count_for_player` (value-returning, `-1`
  sentinel, and it RETIRES finished tails first, so it is `guardValue`d).
  `DspChain` (`src/dsp/DspChain.h`, `SpillTail`): a REMOVE of an audible node
  whose catalogue category is `delay` or `reverb` marks the slot `spilling`,
  and the remove's usual fade becomes an INPUT ramp in place
  (`spillHandOverRt`: out = module(in·w) + in·(1−w)), so neither the tail nor
  the dry path steps; the SAME publication that drops it from `idOrder_`
  re-publishes it as a post-chain tail (`NodeRef::spill`), processed over
  silence after the whole chain and summed into the rig output
  (`processSpillTailsRt`). Done after 8 s (with a 10 ms fade) or 200 ms under
  -90 dBFS; Done tails are reaped by the next `publishListLocked` AFTER its
  barrier (never freed on the callback). Pool: grow-only `tailPool_`, at most
  4 ringing — a fifth sets `fastFade` on the oldest. Edit-scope commits spill
  the same way; `clear()` ends every tail (a reset); `prepare()` ends them (no
  callback in flight). A node the preset diff keeps was never removed and rings
  through regardless.
- **Re-amp** — `dca_engine_set_input_injection_for_player` (copies a mono
  engine-rate take, PAUSED at 0), `_clear_`, `_set_playing_`, `_set_loop_`,
  `_seek_`, `dca_engine_get_input_injection_status_for_player`
  (`DCA_ERR_INVALID_ARG` = no take). `AudioEngine::renderInputInjection` runs
  per seat AFTER the drop tune and BEFORE `writeInputCapture`, so the capture
  ring (tuner, detection), the rig and the meter all hear the take and the
  guitar is muted while one is loaded; with no input device at all the seat
  loop runs for an injected seat only. The take is immutable once published
  (`injection_[p]`, raw pointer); a replaced take is freed on the CONTROL
  thread after a seq_cst Dekker handshake on `injectionBusy_` (bounded 2 s,
  then LEAKED and logged — a leak beats a free under a live read). 5 ms
  play/pause ramp; a NEW take (serial) starts from gain 0. An engine rebuild
  starts empty — the host replays (DotCharter: `ReplayReampTakesTo`).
- **`dca_engine_read_input_capture_from_for_player`** — the caller owns the
  cursor: reads from an absolute position, reports where the copy really began
  (later = a gap, earlier = the stream restarted and is read from its start).
  Any number of these coexist with the ONE streaming cursor live detection
  drains, which a DI recorder must never steal from.

Pinned by `AbiV37Tests` (version, presence of all ten in the built DLL, null
handle rejection), `SpilloverOfflineTests`, `InputInjectionNullDeviceTests`,
the binding theory in `NativeMethodsTests`, and the RT gate's
`RtSpilloverTailRingsAfterRemovalWithoutAllocating` +
`RtInputInjectionReplacesTheSeatInputWithoutAllocating`.

## ABI v36 — a separation that can be cancelled, and the counter with no way out

Two additive exports, one additive status, ONE new callback typedef
(`dca_stems_progress_cb2`; this line, the header comment and the commit message
all said "two" until 2026-09-12). **Nothing
was reordered and no existing signature changed**, which is the whole shape of
this revision: both additions are SECOND entry points beside their v35 twins.

### `dca_stems_separate_file2` + `dca_stems_progress_cb2` (W4-N19)

`dca_stems_progress_cb` returns `void`. There is no channel to answer on, so a
multi-minute separation could not be stopped — and the managed side honoured
this repo's own rule (*"a cancelled separation throws
`OperationCanceledException`, not `StemSeparationException`"*) by abandoning the
wrapper task while the native run went on to completion, writing stems nobody
wanted. `JuceStemSeparation.SeparateAsync`'s doc said so in as many words.

- **The new callback returns `int`, and NON-ZERO means CONTINUE.** That
  direction is deliberate: a binding that falls off the end of some path
  without returning a value fails SAFE (keeps going) rather than silently
  cancelling every separation.
- **The checked points are between the coarse phases AND between the per-target
  WAV writes.** That second one is the point of the exercise — the 0.95 → 1.0
  stretch writes four or six files, is pure I/O after the model has already
  run, and was uninterruptible.
- **Inference itself cannot be interrupted.** `demucscppthreaded::threaded_inference`
  takes no callback, so a cancel arriving during it is honoured the moment it
  returns. That is a real bound on how fast a cancel lands and it is stated
  rather than pretended away.
- **A cancelled run LEAVES the stems already written on disk.** The caller asked
  to stop; deleting a partial set is a decision for whoever owns `out_dir`.
- **ONE implementation.** `separate_file` (v35) forwards to
  `separate_file_cancellable` with a callback that always answers "keep going",
  so the two entry points can never drift — the same rule that collapsed the
  four copied seat-0 capture exports in the wave before this one.
- **A THROWING progress handler does not cancel.** It is a broken observer;
  tearing down a multi-minute run because somebody's progress bar faulted is the
  wrong blast radius. The ABI trampoline catches and returns "continue", exactly
  as the v35 callback swallows-and-logs. Only the token stops the run.

**Why not change the existing typedef.** `void` has no room for an answer, so
the change would be a signature change: every binding compiled against it breaks
— silently, in the case of a C caller — and three managed tests invoke that
delegate directly. **Why not `dca_stems_cancel(handle)`.** There is no handle:
`dca_stems_separate_file` is a free function over paths, so a cancel export
would first have to invent a session object the ABI does not have.

### `dca_midi_output_get_counts3` (W4-N15)

`MidiOutputDevice::failedCount()` — messages the DRIVER threw on inside
`sendMessageNow` — has existed since v35 with no export to read it, and its own
doc said so. It is a different fault from `dropped`: dropped means *we never
handed it over* (our queue was full), failed means *it reached the driver and
did not come out the other side*. Before the two were split, `sent` moved on a
throw as well, so a wedged interface read as working; `sent + failed` is what
`sent` alone used to report.

A third export rather than a third out-param on `dca_midi_output_get_counts`,
because an existing caller passes exactly two pointers and has to keep
compiling. Managed: `JuceMidiOutput.FailedCount` / `.ReadAllCounters()`, both on
`IJuceMidiOutput` and on the fake.

### `DCA_ERR_CANCELLED = 6`

The first new status since the enum was written, and **it is not a failure**.
A host that lumps it in with the errors turns a user's Cancel into an error
dialog and, in this repo's own words, into "a reason to fall through to the next
backend". `JuceStemSeparation` raises `OperationCanceledException` for it and
`JuceAudioException` for everything else. Mirrored in BOTH managed enums
(`NativeMethods.DcaStatus`, `JuceAudioStatus`) because the wrapper casts
straight between them; `AbiV36Tests` pins the ordinal on all three sides.

## Where an export GOES: `src/dotcharter_audio.cpp`, the one ABI translation unit

New engine exports go in **`src/dotcharter_audio.cpp`** (grep for the
neighbouring `dca_engine_*` and insert next to it). `JUCESHARP_CORE_SOURCES` in
`CMakeLists.txt` is the list of what is compiled; nothing else is.

Until 2026-08-28 the tree also carried `src/abi/{abi_common,abi_devices,
abi_engine,abi_source}.cpp` + `abi_internal.h` — a per-domain ABI split that
was never wired into `JUCESHARP_CORE_SOURCES`, ~1,100 lines, every function a
duplicate of one still live in `dotcharter_audio.cpp` and 18 ABI versions
stale (three of its four divergences were bugs already fixed in the live
file). An entry point added there built perfectly cleanly and did not exist in
the DLL: no warning, no link error, just an `EntryPointNotFoundException` on
the managed side's first call. **It is deleted; do not restore it.** The
lesson survives it: after every ABI addition, `dumpbin /EXPORTS` is the only
thing that proves the export is in the DLL, and it is worth running:

```powershell
& "$env:LOCALAPPDATA\..\dumpbin.exe" /EXPORTS build\Release\juce_sharp.dll | Select-String dca_engine_your_new_export
```

## Per-player live-input rigs (ABI v28) — couch co-op

  - **PER-PLAYER live-input rigs (ABI v28) — couch co-op.** The engine hosts
    `DCA_MAX_PLAYERS` (4) INDEPENDENT rigs ("seats"). Each owns its own
    `dcrig::DspChain`, its own VST3 input chain, its own mono capture ring and
    its own hardware INPUT CHANNEL. **This is channel routing inside ONE open
    device, never a second device open** — an ASIO driver is typically
    exclusive, so the two inputs of one interface (exactly the co-op case) can
    only be split from inside a single `AudioIODevice`.
    - **`playerCount_` (default 1) is the switch, and at 1 the callback runs the
      pre-v28 path byte-for-byte.** That is deliberate: a solo player must not
      pay for, or hear any change from, a feature they are not using. The one
      behaviour that differs at >1 is the chain FILL — at 1 **on a 1-2 in
      interface** a seat takes channels 0/1 of the effective input (a stereo
      pickup stays stereo), at >1 every seat is MONO from its own channel
      duplicated L/R, because with two guitars on inputs 0 and 1 the stereo fill
      would put BOTH players into seat 0's amp. **The CHANNEL COUNT is part of
      that condition and was not until 2026-09-10**: on a 4-in interface a solo
      player's rig was filled from channels 0/1 whatever
      `inputCaptureChannel_[0]` said, while `writeInputCapture` used the
      requested channel — so the amp and the note detection listened to
      DIFFERENT guitars, and engaging the drop tuner (which publishes a mono
      copy of the CAPTURE channel) moved the rig onto a different input
      mid-song. The fill condition is now exactly the split the shifter above it
      makes, `players == 1 && numInputChannels <= 2`, which is what makes the
      "SAME rule" claim below true. Pinned by
      `RtSoloSeatOnAMultiInputDeviceFollowsItsCaptureChannel` (four DC inputs at
      four levels: monitor 0.1 against a ring of 0.3 before the fix).
    - **The node-id contract is what kept the ABI delta small.** DSP node ids
      are unique per ENGINE: seat `p` hands out
      `p * DCA_PLAYER_NODE_ID_STRIDE + local`. So every NODE-scoped export
      (remove / move / bypass / set_param / get_param / node string / node
      branch) keeps its pre-v28 signature and resolves the seat from the id, and
      only the CHAIN-scoped ones gained `*_for_player` variants (add / count /
      list / clear / process_buffer). **Seat 0's ids are 0, 1, 2, … exactly as
      before, so every pre-v28 export is an alias for seat 0.** A node id naming
      a seat past `DCA_MAX_PLAYERS` is REJECTED, never clamped — clamping would
      silently apply one player's edit to another player's rig.
    - Routing/capture exports: `dca_engine_set/get_player_count`,
      `dca_engine_set/get_player_input_channel` (seat `p` defaults to input `p`;
      `dca_engine_set_input_capture_channel` IS seat 0's),
      `dca_engine_set_player_monitor_gain` (a trim MULTIPLIED into the global
      monitor gain, unity by default — how two guitars get balanced in one mix),
      `dca_engine_get_player_input_peak`, and
      `dca_engine_read_input_capture{,_stream,_stream2}_for_player` +
      `dca_engine_get_input_capture_sync_for_player`. **The rig and the capture
      ring resolve a seat's channel with the SAME rule**, so a player's tone and
      a player's judging can never listen to different guitars.
    - VST: `dca_plugin_attach_to_input_for_player` (+ `dca_plugin_get_input_player`).
      Attaching an already-attached plugin to a DIFFERENT seat MOVES it — native
      detaches first — because one instance in two chains would run twice per
      block over two signals with one lot of internal state. `detachInputPlugin`
      removes from EVERY seat and takes each seat's wait-one-block barrier in
      turn (never nested — it is a barrier, not a critical section).
    - **ONE thing stays seat 0 only, on purpose:** the out-of-process VST sandbox
      bridge — the shared mapping carries one stereo stream per block, so a
      second seat needs a second child process. Seat 2's in-process rig and VST
      chain are unaffected by it.
      **The drop-tune pedal used to be the second entry here and is not any
      more — see ABI v31 below.** It was one CSVD engine ahead of the per-player
      split, which made the pedal unusable on a couch: dropping one guitar
      dropped both.
    - **RT note that the alloc gate cannot see:** the callback's
      `std::atomic_*(shared_ptr*)` sites scale with the number of LIVE seats
      (`inputChain_[p]` and each `DspChain::order_`). At `playerCount_ == 1` the
      count is unchanged from v27. See the `_Shared_ptr_spin_lock` bullet under
      the RT gate below before adding another such site.
    - Offline evidence: `tests/JuceSharp.Tests/Dsp/DspPerPlayerChainTests.cs`
      drives `dca_engine_dsp_process_buffer_for_player` with **no device open**
      and proves two seats produce different output from the same input and that
      editing one seat leaves the other's buffer bit-identical.

## Parametric EQ over the master bus (ABI v29)

  - **Parametric EQ over the master bus (ABI v29).** Six typed bands
    (`dca_eq_band_type`: high pass / low shelf / bell / high shelf / low pass /
    notch) with per-band frequency, Q, gain and enable, plus a master power, a
    single-index solo, a routing target (`dca_eq_route`: song bus / every seat's
    post-rig guitar / both), an output trim, an active-band COUNT and a
    magnitude spectrum for an analyser. Exports:
    `dca_engine_set_eq_band` / `_enabled` / `_solo` / `_route` / `_trim`,
    `dca_engine_get_eq_active_count`, `dca_engine_read_eq_spectrum`.
    Everything is OFF at engine start, so an app that never calls them runs the
    pre-v29 callback byte for byte. The three fixed filters
    (`dca_engine_set_high_pass` &c) are unchanged and still run AHEAD of the six
    bands. Five rules, each of which cost a bug or would have:
    - **The bands ride in the SAME triple-buffered coefficient snapshot as the three
      fixed sections**, published by the same `publishMasterFilterCoeffs` on the
      message thread under `filterPublishMutex_`. `LatestSnapshot` retains an
      exclusive callback slot through every bus/seat in the block; a publisher
      burst cannot reuse it. No callback locks, allocations or ownership frees.
      Two publish paths would need two slot indices and the
      callback could then read a band set from one tick and a filter set from
      another. `IIR::Coefficients` factories heap-allocate, which is exactly why
      none of this may move onto the callback — dragging a band handle produces
      setter traffic at UI tick rate.
    - **A band is published only when it is AUDIBLE** (power on, and either
      soloed or enabled-with-nothing-soloed). Deciding that on the message
      thread is what makes `eqActiveCount()` and the audio path agree BY
      CONSTRUCTION rather than by two copies of one predicate — the managed
      "n/6" badge reads that export, so the number on screen is a statement
      about the DSP.
    - **The guitar banks need their OWN dirty flag** (`eqInputDirty_`).
      `processMasterBus` consumes `filterDirty_` and runs earlier in the same
      callback, so a shared flag leaves the guitar EQ permanently one publish
      behind. It is consumed ONCE before the seat loop, never inside it: per
      seat, the first seat clears it and every later seat never retunes.
    - **One bank per BUS and per SEAT.** Two seats carry different signals and
      cannot share biquad memory. Retuning a RUNNING band keeps its z^-1 state
      (only off->on resets), which is what stops a drag clicking; routing AWAY
      from a bus forgets that bus's engaged flags, so a stale tail cannot replay
      when the user routes back. Re-engagement copies the current coefficients
      even if their dirty notification arrived while that bank was unrouted.
      Inactive seats also forget engagement. EQ trim ramps per bus/seat from
      its previous block value, with identical endpoints across channels.
    - **The analyser is a lock-free mono ring the callback appends to**, snapshot
      + windowed + FFT'd on the CALLING thread by `readEqSpectrum` (its mutex is
      message-thread only, so it is not a new RT lock site). The tap is PRE-EQ,
      so the spectrum does not chase the curve being dragged over it, and it
      follows the route. A torn read costs one wrong bin in one frame of a 15 Hz
      display — which is why it needs no synchronisation, and why it must never
      be used for anything that is not a picture.
    The managed ranges are mirrored in `DotCharter.Services/Audio/Equalizer/EqBand.cs`
    (`EqLimits`) and clamped on BOTH sides deliberately: the ABI is a boundary,
    and a band the engine silently moved is a band whose drawn curve and audible
    curve disagree. Change either side and change both.

## Master-bus ceiling (ABI v34)

`dca_engine_set_master_limiter` / `dca_engine_get_master_limiter` /
`dca_engine_get_master_limiter_reduction_db` /
`dca_engine_get_master_limiter_latency_samples`, over
`src/MasterLimiter.h`. Four additive exports plus ONE changed meaning
(`dca_engine_get_output_latency_samples`, below). **OFF at engine start since
ABI v35** — `masterLimiterEnabled_ { false }` (`src/AudioEngine.h:1368`), and the
HOST opts in by calling `dca_engine_set_master_limiter`. When enabled the
defaults are -0.3 dBFS / 80 ms release. (v34 shipped it default-ON; that cost
every player 1 ms of look-ahead latency on their own monitor path for a feature
they had not asked for, which is why v35 reversed it.)

**What was wrong.** The final device buffer was NaN-SCRUBBED and nothing else.
`dcdsp::scrubNonFinite` repairs NaN/Inf and says nothing at all about
MAGNITUDE, so the sum of the song bus + every seat's monitor (a rig ending in a
boost pedal legitimately leaves 1.0 behind) + the click bus + both instrument
legs reached the driver unbounded. A hardware DAC clips that; the STREAMING
MIRROR carries FLOAT, so the over-range samples survived into OBS / Discord's
encoder and WRAPPED there. That is the reported artefact, and it is why the
stage sits after the last contributor and BEFORE `mirror_.pushBlock` rather
than merely before the device.

Six rules, each of which cost a bug or would have:

- **SWITCHED OFF IT IS NOT IN THE PATH AT ALL** — not a delay line at unity,
  not a memcpy. The output is bit-for-bit the pre-v34 engine and the reported
  latency is 0, which is the same rule `playerCount_ == 1` follows for the v28
  seats. **The first version did the opposite** and kept the delay running when
  disabled, so that the latency stayed constant across a toggle and the v26
  click anchor could never move. The RT gate settled it: 48 samples of
  unconditional delay shifted every output block and broke
  `RtEqTrimRampsAndSparseInputIsSilent`, which measures an EQ trim ramp sample
  by sample. A stage that silently re-times the whole engine while switched OFF
  is worse than a click scheduler having to re-read
  `dca_engine_get_output_latency_samples` after a toggle. That export now
  reports **device + limiter-when-enabled**, because it answers "how late does
  audio handed to the engine reach the ear";
  `_get_master_limiter_latency_samples` decomposes it. The consequence to know
  about: toggling mid-song inserts or removes 1 ms in one block, i.e. a click.
  The delay line is cleared on the OFF edge so the stage cannot replay audio
  from whenever it was last on.
- **The reported latency describes the LAST BLOCK, not the session** (W4-N13,
  2026-09-11). `MasterLimiter::process` SKIPS a block wider than `prepare()`
  sized — the "skip, never grow" rule — and `latencySamples()` correctly answers
  0 for such a block, because it really did reach the driver undelayed. But it
  gated on `skipped_`, which is the CUMULATIVE health counter and is cleared
  only by `prepare()`, so **one skipped block made the export report 0 for ever**
  — including for every later block the stage did delay. A skip is per-block
  (`numChannels > channels_`) and the very next block at the prepared width is
  limited normally, so that is the easy state to reach: the same 1 ms error the
  branch was added to prevent, sign flipped, on the one number click alignment,
  the render-clock anchor and the MIDI-out deadline all consume. An
  audio-thread-written `lastAppliedLatency_` answers it now, seeded to
  `lookahead_` by `prepare()` so a freshly prepared stage that has not run a
  block does not claim zero. `skipped_` stays cumulative, which is what it is
  documented as. The enabled/disabled gate stays on the MESSAGE thread and is
  deliberately NOT mirrored into that atomic — doing so would make a
  just-enabled limiter report 0 until its first callback.
  `RtMasterLimiterSkipIsCountedAndReportsNoLatency` now drives a third block
  after the skip; reverted it reads `latency 48 -> 0 -> 0` and fails.
- **The gain computer is a SLIDING MINIMUM over the look-ahead window, never
  the instantaneous target.** With the instantaneous target the smoother only
  starts moving once the peak is already at the output, so every attack punches
  through into the backstop clamp and the stage degrades to a CLIPPER. Measured
  in the gate: 0.78 % of samples sitting on the ceiling with the window
  minimum, **13.28 % with the instantaneous target**. The window is a monotonic
  deque over pre-sized arrays (O(1) amortised); a naive rescan would be up to
  288 operations per sample at 192 kHz.
- **The applied gain is PER-SAMPLE smoothed and both smoothers SNAP to their
  rest value.** A one-pole approaches its target asymptotically, and in FLOAT
  it stalls: `g += (1-g)*2.6e-4` at `g = 0.999886` rounds to the same float for
  ever. Without the snap the limiter multiplies every later sample by 0.99989
  for the life of the session, the reduction meter parks at 0.001 dB (a
  permanently lit meter over silence — the v30 `setMetering(false)` fault, one
  decimal place down), and the OFF state is never the bit-exact pass-through it
  is documented to be. Found by the gate, not by reasoning.
- **No transcendental on the callback.** Both coefficients are the `k = dt/tau`
  first-order form the v30 meters use, and the `release_ms` conversion is
  cached against the last value seen.
- **It is sized from `masterBusWidth_`, like every other per-channel
  master-bus stage (JuceSharp #31).** That is the device's ACTIVE output count,
  because JUCE's Windows devices PACK the enabled outputs: the callback gets one
  non-null pointer per ENABLED channel, in order, whatever their physical
  positions (WASAPI `run()`: `numOutputBuffers = getActiveOutputChannels().countNumberOfSetBits()`;
  ASIO: `outBuffers[n]` for the n-th enabled channel, callback handed
  `numActiveOutputChans`; DirectSound: `outputBuffers.setSize(enabledOutputs.countNumberOfSetBits(), …)`),
  and `AudioDeviceManager` passes the count through. Until #31 this bullet said
  the opposite ("one pointer per PHYSICAL channel, inactive ones null" — the
  first paragraph of `juce_AudioIODevice.h`'s callback doc, which no Windows
  device implements) and the limiter was over-sized from
  `getOutputChannelNames()` while the HP/LP/BP and song-EQ banks used the active
  count with a `jmin(ch, size - 1)` fallback. A block wider than the width is
  SKIPPED and COUNTED by every stage (`masterBusSkippedBlocks()`,
  `masterLimiterSkippedBlocks()`), never folded onto a shared state. Gated by
  `RtSparseOutputMaskIsPackedAndTheMasterBusKeepsLAndRApart` (the callback's
  pointer count and null mask on outputs 3-4 of 8, one width for every stage,
  L/R independence and continuity with HP + an EQ band, the over-wide skip, a
  rebuild re-deriving the widths) and managed
  `MasterLimiterNullDeviceTests.ASparseOutputMask_StillReachesTheLimiterOnEveryBlock`.
- **A non-finite ceiling or release is REJECTED, never clamped.** Both halves
  of a clamp (`v < lo`, `v > hi`) are false for a NaN, so a clamped NaN reaches
  the ceiling and the bus is silent for the session. Same rule as
  `DspChain::setParam`. The finite ranges ARE clamped, on both sides of the
  ABI, for the v29 EQ's reason: a cached managed getter must not be able to
  disagree with the engine.

Gated by `RtMasterLimiterHoldsTheCeilingWithoutAllocating` (native: OFF passes
a +12 dB stimulus through at 4.0, ON holds 0.966051 with 12.34 dB of reduction,
0.78 % clamped, 0 allocations, the meter returns to EXACTLY 0) and
`MasterLimiterAbiTests` / `MasterLimiterNullDeviceTests` (managed: the two-sided
clamp, the NaN rejection, and that the stage is reached from a real audio
callback at all).

## Output render clock + scheduled clicks (ABI v26)

  - **Output render clock + scheduled clicks (ABI v26).** `renderFrames_` counts
    every frame the device callback has rendered and is published seqlock-style
    (`dca_engine_get_render_clock` → {frames, QPC ticks}) **immediately after the
    transport pull** — not at the end of the callback. That placement is
    load-bearing: it makes the render clock and
    `dca_source_get_audible_position_seconds` describe the SAME output sample, so a
    managed caller can read them back-to-back, re-read the frame counter to prove no
    block landed in between, and get a torn-free {frame ↔ song time} anchor. Move the
    publish and you silently reintroduce a one-block error. `dca_engine_schedule_click`
    starts a voice at a sample OFFSET inside its block (`mixClickBus` takes the block's
    start frame); `dca_engine_clear_scheduled_clicks` travels through the SAME FIFO as
    an ordinary entry (a sentinel, not a side-channel flag) so "schedule then clear"
    reliably cancels. The immediate `triggerClick` path is untouched and still fires at
    the next block. **Why anyone cares:** output latency cancels out of
    `frame(T) = renderFrames + (T - rawTransportMs)/speed*sr`, so a scheduled click sits
    in the very samples that carry the note's audio and reaches the ear WITH it —
    whereas the trigger-now path is always one output latency late.

## Offline waveform peaks (`dca_generate_waveform*`)

  - **Offline waveform peaks (`dca_generate_waveform*`): the block width is
    FRACTIONAL and must stay that way.** The caller's only way back from a block
    index to a time is `index * blockMs`, so block `k` has to span samples
    `[llround(k*w), llround((k+1)*w))` with `w = sr*blockMs/1000` a real number.
    `generateWaveformFromReader` used to round `w` to an integer
    samples-per-block, which is exact at 48 kHz (96 samples for 2 ms) and NOT at
    44.1 kHz (88.2 → 88): every block came out 1.99546 ms, so DotCharter's
    timeline waveform ran 0.227 % fast and drifted to the right of its own audio
    — +408 ms three minutes into a song. Never reintroduce an integer
    samples-per-block here, and never let the boundary accumulate (recompute it
    from `written + 1` each time). Owner-reported, fixed 2026-07.

## Per-seat drop tune (ABI v31)

`dca_engine_set_player_input_pitch_shift{,_mode}`. Two additive exports; nothing
was reordered and no signature changed, and the two pre-v31 exports are now
exactly their per-player form with `player = 0` — the same relationship
`dca_engine_set_input_capture_channel` has with
`dca_engine_set_player_input_channel`.

- **What was wrong.** The engine held ONE `dcdsp::DropTune`, one
  `inputPitchShiftSemis_` atomic and one lag atomic, applied to the RAW device
  input **above** the per-player loop. So on a couch with two guitars in
  different tunings the pedal could not be used at all: dropping one dropped
  BOTH, and the seat that did not ask for it was detuned with nothing on screen
  saying so. `AudioEngine` now holds `kMaxPlayers` shifters, targets, lag
  ceilings and scratch pairs, and the shift runs INSIDE the seat loop.
- **The channel rule survived the rewrite and is still the load-bearing part.**
  A seat shifts exactly the signal ITS capture ring consumes, as MONO. Taking
  ch0/ch1 blindly on a bigger interface shifted the wrong inputs *and* handed
  the ring a 2-channel buffer whose channel index clamped down — so note
  detection silently listened to a different physical input than the one being
  shifted. The one exception is the pre-v28 case, `players == 1 && numInputChannels <= 2`,
  where channels 0/1 are taken as they always were so a genuine STEREO pickup
  stays stereo. That is the same split the chain fill makes, and it is what keeps
  a solo player's signal path byte-for-byte what it was.
- **Every seat's shifter is prepared in `audioDeviceAboutToStart`**, not lazily.
  `playerCount_` is raised from the MESSAGE thread (a co-op run starting), so a
  shifter first prepared on the audio thread is an allocation on the audio
  thread. Same reasoning as `chainBuffer_`, which is sized for `kMaxPlayers`
  whatever the count is. The 2× scratch headroom and the SKIP-don't-resize rule
  are unchanged, per seat.
- A player index past `DCA_MAX_PLAYERS` is REJECTED, never clamped — the node-id
  rule, and for the same reason: a clamp would silently detune somebody else.
- Managed side: `IPlayerInputRouting.SetPlayerInputPitchShift{,Mode}`, recorded
  in `PlayerInputRoutingState` and replayed by `AttachPerPlayerStateToEngine`
  (the count, the channels and the trims each shipped MISSING from that replay —
  a dropped drop tune is the loudest of the four). Guarded from the DotCharter
  side by `Services/Audio/DropTuneNativeMirrorTests`, which reads THIS repo's
  header and `dotcharter_audio.cpp` and fails if the shifter moves back above
  the split.

## Per-node meters + latency (ABI v30)

`dca_engine_dsp_set_metering{,_for_player}` / `dca_engine_dsp_get_metering`,
`dca_engine_dsp_get_node_meter`, `dca_engine_dsp_read_node_meters`,
`dca_engine_dsp_get_node_latency`, `dca_engine_dsp_get_chain_latency{,_for_player}`.
All additive and appended to the `dca_engine_dsp_*` block; nothing was
reordered. Five rules:

- **Poll-only, and OFF until asked for.** The callback folds each block into a
  decaying envelope held in the node's own atomics; nothing is queued and no
  callback crosses the ABI — the same contract as v28's `playerCount_` and
  v29's EQ. While metering is off, `DspChain::process` is byte-for-byte the
  pre-v30 path, so a caller that never asks pays nothing. Switching it OFF also
  ZEROES the meters: otherwise a UI that stops polling leaves a lit rig frozen
  on screen, which reads as live audio that is not there.
- **Levels are LINEAR amplitudes, never dB, and legitimately exceed 1.0** on a
  node driving a clipper. The ABI reports what the samples did and the caller
  picks the scale. Peak falls ~34 dB/s (`tau` 0.25 s) so a 30 Hz poll still
  catches a palm mute; RMS integrates over ~300 ms. Both are stored as the MEAN
  SQUARE with the `sqrt` done once per POLL, not twice per node per block on the
  audio thread. The per-block one-pole coefficient is `k = dt/tau` clamped —
  the first-order form of `1-exp(-dt/tau)` — because a transcendental has no
  business on the callback for a number that only has to look right on a meter.
- **The meters live in a grow-only pool, indexed from the `Slot` and resolved to
  a raw pointer at publish time.** A `NodeMeter` is never destroyed while the
  chain lives, and its slot is only recycled AFTER `removeNode`'s
  wait-one-block barrier — which is what makes a stale snapshot's meter pointer
  harmless (at worst two nodes' levels mixed) rather than a use-after-free.
- **`dca_engine_dsp_get_chain_latency` is the MAX over a split region's enabled
  branches, not the sum** — the branches run in parallel, so the region is as
  late as its latest branch. **Since ABI v34 it IS also a statement that the
  branches are aligned** (it was explicitly not one from v23 to v33, when branch
  PDC was the accepted limit): the merger delays every branch by
  (max − its own total), so the region really runs at this number.
  `dca_engine_dsp_set_branch_pdc_for_player(engine, player, 0)` turns the
  compensation off, at which point the old caveat applies again — the branches
  disagree by up to this much. A BYPASSED node reports 0, because it is not in
  the path and a delay-compensating caller must not budget for it.
- **BRANCH PDC IS RE-DERIVED ON THE CALLBACK, AFTER THE CORES ADOPT** (W4-D54,
  2026-09-11). `DspChain::setParam` publishes into the module's `ParamMailbox`
  and the CORE adopts at the top of its next `process()` — but setParam also
  called `recomputeBranchPdcLocked` immediately, so the mergers were programmed
  from the latency the core had **not yet replaced**. Every adapter that
  delegates `latencySamples()` to its core is affected (`whammy`, `vibrato`,
  `harmonizer`, `klon`); `overdrive` is not, because it answers from its own
  `p_` copy, which setParam updates synchronously.
  **And it was never "one block early"** — nothing re-ran the recompute
  afterwards, so the region stayed misaligned until an unrelated publish or
  prepare happened along. Measured: a whammy flipped to `lowLatency` moves
  1,920 → 648 samples and the merger stayed at 1,920, i.e. **1,272 samples
  (26 ms) of comb across the whole split region** for as long as the player left
  the knob alone.
  The message thread cannot fix this by WAITING: the one-block barrier proves no
  block is in flight, not that one has RUN. So `setParam`/`setStringParam` set
  `pdcDirty_` and the callback re-derives once, after the adoption, over the
  ordering SNAPSHOT it already holds (`recomputeBranchPdcRt` — no lock, no map
  lookup, no allocation, just virtual calls and relaxed stores). Gated on the
  flag, so an ordinary block is byte-for-byte unchanged.
  **That is deliberately a second implementation of one rule**, which this repo
  normally forbids: the two run on different threads with different things in
  hand, and the alternative — taking `writeMutex_` on the callback — is the one
  thing never allowed. They are pinned against each other by
  `RtBranchPdcFollowsAParamOnTheBlockItChanges`, which also asserts the
  re-derive is GATED (a later block with no param write must not move the delay).
  → `dsp-tone-rig.md`
- **An adapter that swallows `core_.latencySamples()` makes the export lie.**
  `harmonizer` and `vibrato` both did while their cores reported real numbers;
  only `klon`, `whammy`, `tapestop` and `namamp` forwarded it. If a core reports
  latency, its adapter must override too — otherwise the export calls the one
  genuinely late module in a rig zero-latency.

`dca_engine_dsp_read_node_meters` is the one a UI should actually drive: 4
floats per node (`in_peak`, `in_rms`, `out_peak`, `out_rms`) in CHAIN ORDER,
index-aligned with `dca_engine_dsp_get_node_ids_for_player`, one P/Invoke per
frame instead of one per pedal. Capacity is in FLOATS; call with a null buffer
to size. Evidence lives in the RT gate
(`RtNodeMeteringIsAllocationFreeAndNotVacuous`) — see `rt-safety-and-tests.md`.

## Rig + plugin introspection (ABI v33)

Five additive exports, nothing reordered, no signature changed.

`dca_engine_dsp_get_node_type` / `dca_engine_dsp_get_bypass` expose two things
`DspChain` has always held and never published. Before v33 the only source for
either was whatever the CALLER had recorded when it created the node, so a chain
enumerated through `dca_engine_dsp_get_node_ids*` was a list of bare integers: a
second binding could not look a node's parameter list up at all, and every node
read as un-bypassed. Both are node-scoped, so they resolve the seat from the id
through `rigOfNode` and REJECT one past `DCA_MAX_PLAYERS` rather than clamping —
clamping would report a DIFFERENT pedal's type.

- **The bypass getter is the message-thread INTENT, not the fade position**, and
  that is deliberate. `DspChain::Slot::bypassed` is the flag `setBypass` stores;
  the ~10 ms wet/dry ramp belongs to the audio thread. A getter that tracked the
  ramp would flicker for 10 ms after every toggle, and a UI mirroring it would
  fight its own footswitch. (One consequence to know about: `fadeOutAndWait`
  sets the same flag while a MOVE or a RETAG is in flight and clears it after,
  so a getter racing a reorder can observe a transient `true`.)
- **`dca_dsp_module_type_category`** is the catalogue's own word out of a SHORT
  FIXED SET — dynamics / drive / amp / cab / modulation / delay / reverb / pitch
  / filter / eq / utility / synth / routing. It lives on `ModuleTypeInfo` (last
  member, so every brace-initialiser in the 57-entry table kept its meaning) and
  is carried by `Entry::category` in `src/dsp/DspModuleFactory.cpp`. **Never
  mirror this vocabulary in managed code** — a guess table is a second source of
  truth for something only the factory knows, and drifts the first time a module
  is added. A missing or off-vocabulary category fails
  `tone_regressions`' `everyCatalogueEntryHasAKnownCategory`, which is the only
  thing that would notice: the field defaults to `""` and a new module without
  one compiles perfectly cleanly.

`dca_plugin_count_vst3_components` / `dca_plugin_load_vst3_component` reach the
components of a MULTI-COMPONENT bundle. One `.vst3` may declare several plugins
(an amp and its cab, a vendor suite in one file); `dca_plugin_load_vst3` took
`descs[0]` and always had, so everything past the first was unreachable — and
unreported, because loading the bundle looked like it had worked.
`dca_plugin_load_vst3` is now exactly `_component(..., 0)`. **An index the
bundle does not have is REJECTED, never clamped**: a silently substituted plugin
is a saved rig that reloads as a different effect. The COUNT is a real scan
(seconds on a cold bundle), which is why `JuceVst3Scanner` deliberately does not
carry one — its contract is a directory walk that touches no plugin code.

The sandbox child honours the index too: `VstHostMain`'s `load` and
`loadInstrument` pipe ops take an optional `component` property. **That is the
JSON control pipe, not the shared-memory header, so `kBridgeVersion` did NOT
move** for it (it was 2 then; v3 is the MIDI-timing block, below). An absent property parses to a void `var` whose int
conversion is 0, so a pre-v33 parent keeps the old behaviour byte for byte.

## Source volume and pan are clamped at the boundary (ABI v33)

`dca_source_set_volume` clamps to `[0, +inf)` and `dca_source_set_pan` to
`[-1, +1]`. Neither was clamped at the ABI before, and they are two different
faults:

- **A negative gain reached `juce::AudioTransportSource::setGain`, which accepts
  it.** That is a PHASE INVERSION, not a quieter source: it cancels against
  anything correlated already in the mix — the same take on another stem, a
  doubled guitar, the click — so it reads as a hollow MIX rather than as a wrong
  level, with nothing logged.
- **Pan was clamped deep inside `PanningAudioSource::setPan` but not at the
  boundary**, so a wrapper caching what it wrote could report pan 4.0 while the
  engine played hard right. Same "drawn curve disagrees with the audible curve"
  shape the v29 EQ clamps on both sides to avoid.

**There is deliberately NO upper bound on volume.** Boost above unity is a
shipped feature: DotCharter's stem faders reach +6 dB (linear 2), its per-stem
gain contract is documented 0..4, and `JuceAudioPlaybackService.Mixer`'s
stem × master product can legitimately arrive at 16. A ceiling invented here
would silently cap a user's fader. A non-finite value is still REJECTED, not
clamped. The managed side mirrors both clamps so the cached getters agree with
the engine; guarded by `SourceLevelRangeTests` (behaviour managed-side, source
lint native-side — this ABI exports no volume or pan getter).

## Dropout / xrun counters + device health (ABI v30)

`dca_engine_get_xrun_stats` / `dca_engine_reset_xrun_stats`, over a
`dca_xrun_stats` struct. Dropout detection used to be a MANAGED heuristic, and a
heuristic running in the polling thread cannot tell an audio-callback overrun
from a hiccup in the polling thread that was watching for one. The engine counts
it where it happens now, in `AudioEngine::recordCallbackTiming`.

**Three conditions, and they are three different faults — do not collapse them
into one "glitches" number:**

- **`late_callbacks`** — the callback ARRIVED late: the gap from the previous
  callback's start exceeded 1.5× the previous block's own duration + 0.5 ms. The
  device went round without us and the user heard it. **This is the one to show
  a user**, and on WASAPI / CoreAudio it is the only signal that exists at all.
  (The gap is measured against the PREVIOUS block's size, not this one's,
  because a variable-block driver renegotiates.)
- **`deadline_overruns`** — OUR callback outlasted the block it was rendering.
  We are the cause. It normally makes the NEXT callback late, so the two
  correlate; `late_callbacks` climbing while this stays 0 means the machine, not
  the rig.
- **`near_misses`** — over 80% of budget, INSIDE it. A load warning. This is the
  pre-existing `overrunCount_`, which used to *include* the real overruns and
  therefore read as a problem when it was a warning and vice versa. It is now
  strictly disjoint: a callback is counted as one or the other, never both.

**`device_xruns` is the DRIVER's own count** (`juce::AudioIODevice::
getXRunCount`). ASIO and DirectSound implement it; **WASAPI and CoreAudio's
aggregate path return −1** — which is precisely why the callback-side counters
above have to exist. Never surface −1 as zero.

Also carried: `last_xrun_host_ticks` (QPC — the same timebase as
`dca_engine_get_render_clock`, `dca_engine_get_input_capture_sync` and .NET's
`Stopwatch`), `last_xrun_render_frame`, `max_callback_ns`, `last_callback_ns`,
`block_budget_ns` (load = last ÷ budget), and `device_open` / `device_running`.

Rules, each of which cost a bug or would have:

- **Everything is relaxed atomics bumped on the audio thread: no allocation, no
  lock, no logging, no syscall.** The tick source is the cached `perTick` QPC
  conversion that was already there — never call
  `getHighResolutionTicksPerSecond()` per callback (that is a
  `QueryPerformanceFrequency` syscall).
- **The first `kXrunWarmupBlocks` (4) callbacks of every device session are
  IGNORED, and that is load-bearing.** A cold device thread is always late, so
  without it every device change, sample-rate change and watchdog recovery would
  report a dropout that never happened — the exact false positive that made a
  managed heuristic worthless. `AudioEngine::resetXrunSession()` is called from
  `audioDeviceAboutToStart` next to `resetRenderClock()`; **keep them together.**
- **`struct_bytes` is a Win32-style `cbSize`.** The CALLER sets it to
  `sizeof(dca_xrun_stats)`; native fills a local of its own size and copies
  across the smaller of the two, then reports how many bytes it wrote.
  **Appending a field at the END is therefore additive and needs no ABI bump**;
  reordering or resizing an existing field is still a break. A `struct_bytes`
  below the four leading `int32`s is rejected with `DCA_ERR_INVALID_ARG`.
- **`device_open` / `device_running` are how the managed side tells a live engine
  from a dead handle.** A `dca_engine_reconfigure` whose rollback ALSO failed
  leaves a valid engine handle with no device behind it; this reports that
  honestly as 0 instead of letting the app go on believing it is playing.

Gated by `RtXrunCounterIsRealAndQuiet` and `RtXrunStatsAbiHonoursStructBytes` —
see `rt-safety-and-tests.md`.

## The sandbox bridge carries MIDI sample offsets (bridge v3, JuceSharp #28)

`dotcharter_vst_bridge.h`'s `kBridgeVersion` is **3**. A `dcvb::MidiTiming`
block is APPENDED after the instrument region at `kMidiTimingOffset` (197184):
`int64 blockStartFrame` (the render-clock frame of the block's sample 0) and
`uint32 midiOffset[128]`, the sample offset of `midi[i]` inside the block. The
header and every v2 offset are unchanged; `kTotalBytes` grew 197184 → 197704.

- **Parent:** `AudioEngine::collectBridgeMidi` drains the fifo into the bridge
  leg's OWN parked set (`bridgeMidiPending_`, under `remoteBridgeMutex_` — never
  `midiPending_`, which is the in-process leg's behind `instrumentMutex_`) and
  forwards only the events due in this block, with the same frame arithmetic
  as `renderInstrumentWork`: late ⇒ sample 0 and counted `late`, > 5 s ahead ⇒
  dropped, future ⇒ stays parked. A timed-out round trip carries its events to
  the next block at sample 0. `discardQueuedMidi` and a bridge DISABLE clear the
  parked set (`clearBridgeMidiLocked`).
- **Child:** `juce_vst_host.exe` places `midi[i]` at
  `dcvb::midiSampleOffset(timing, i, blockFrames)` (clamped into the block), and
  its MidiBuffer is pre-sized so `addEvent` never grows on the worker.
- **Mismatch:** a named mapping keeps the size it was created with, so a v2
  mapping cannot be viewed at v3's size. The parent's `enable` fails (DotCharter
  falls back to in-process hosting); the child logs `bridge layout mismatch:
  header v2 … this build v3` and exits 2 before touching the control pipe; the
  managed host kills a child whose `hello.bridgeVersion` differs.
- Gated by `RtSandboxMidiLandsOnItsSampleWithoutAllocating` (exact offsets,
  parked-not-early, late at 0, allocation-free) and
  `RemoteBridgeRoundTripTests.ScheduledMidi_ReachesTheSandboxChildOnItsExactFrame`.

## The sandbox bridge replays the device configuration (bridge v2, 2026-09-10)

Bridge v2 (still in v3): `reserved0` and
`reserved1` became `prepareBlockFrames` (offset 44) and `prepareGeneration`
(offset 572) — same offsets, same 576-byte header, so every `static_assert` is
unchanged, but the SEMANTICS changed and a v1 child must be refused. **The
managed mirror `VstSandboxLayout` in `DotCharter.Services` has to follow.**

What it fixes: the child was told its rate and block ONCE, as launch arguments,
and read `sampleRate` exactly once, in `openBridge`. `audioDeviceAboutToStart`
published only the rate, into a field nobody re-read. So after a
`dca_engine_reconfigure` every SANDBOXED VST3 went on running at the rate the
child process was launched with, and one prepared for 256 frames could be handed
1024 — which entitles it to allocate, on the very worker thread the parent's
audio callback is blocked waiting for. That is exactly the bug
`reprepareAttachedPlugins` closed for the IN-PROCESS plugins, one process
boundary away.

- `RemoteVstBridge::setDevicePrepare(sr, blockFrames)` replaces `setSampleRate`:
  it writes both fields relaxed and then `fetch_add`s the generation with
  RELEASE, so the child can never see a torn pair. `enable()` stamps the pair
  once and the child LATCHES that generation in `openBridge`, so arming is not
  itself mistaken for a device change.
- The child's `audioWorkerLoop` compares the generation on every wake-up and
  posts `prepareAll(sr, blk)` to its MESSAGE thread — never `prepareToPlay` from
  the worker, which holds `graphMutex_` while processing and would block the
  parent for however long a big amp sim takes. Until it lands the worker answers
  every block with nothing active, which the parent already treats as "pass the
  audio through dry".
- Evidence: `RemoteBridgeRoundTripTests.ADeviceChange_RepublishesRateAndBlockToTheChild`
  (the PARENT half — the managed fake child cannot re-prepare anything, so the
  child half is code review plus the bridge version gate).

## Offline render with parameter automation (ABI v34)

`dca_engine_dsp_process_buffer_automated_for_player` +
`dca_dsp_automation_event` (48 bytes; `static_assert`ed in
`dotcharter_audio.cpp` and mirrored by `JuceDspAutomationLayoutTests`), over
`AudioEngine::dspProcessOfflineAutomated`.

`dspProcessOffline` renders a whole take at ONE static setting, which is the one
thing a re-amp is never asked to do. The obvious alternative — the caller
chunking the buffer and calling `setParam` between calls — is the same loop with
a per-chunk re-prepare check and no way to say WHEN, because the caller cannot
see the chunk boundary the engine picks.

- **Every event goes through `DspChain::setParam`, the SAME mailbox the live
  callback's setters use.** That is the load-bearing choice, not an
  implementation detail: it is what makes an automated render and a live
  performance of the same gesture unable to diverge, and it is what makes a NaN
  rejected here exactly as it would be rejected live.
- **The whole list is validated BEFORE a single sample is rendered.** Rejecting
  half way through would leave the caller holding a partially processed buffer
  it cannot tell apart from a good one — and the failure it most wants to
  catch, a NaN, is precisely the one that would already have reached the
  coefficients by then. The input is left byte-identical on rejection.
- **An UNSORTED list is REJECTED, never reordered.** A caller that thinks it is
  automating a sweep and is actually automating a scramble gets a render that is
  plausible and wrong.
- **The chunk boundary is the EVENT boundary** (capped at the same 512 frames
  `dspProcessOffline` uses), and events due at the chunk's first frame are
  applied BEFORE it renders — so an event at frame F is heard by sample F and
  not by F+1. Applying the list up front instead makes the gate's level-split
  and bypass assertions go red, which is how it was verified.
- **`"bypass"` is a RESERVED param id** routed to `setBypass`. No module
  declares a parameter by that name (the chain owns bypass, not the module), so
  it costs nothing and gives automation the footswitch. `value != 0` = bypassed.
- **The id field is a fixed 32 bytes and a caller may legally fill all of them
  without a terminator**, so the engine copies into a 33-byte local before it
  compares. The managed `Create` helper REJECTS an id that would not fit rather
  than truncating: a silently cut id resolves to nothing, and "nothing happened"
  is the hardest automation bug there is to see.
- Same `DCA_ERR_DEVICE`-when-a-device-is-open rule as the static export, and if
  anything a stronger case for it: this variant takes the chain's process
  barrier once per EVENT boundary as well as once per 512 frames.

Gated by `DspOfflineAutomationTests` (11 cases — nine `[NativeAvailableFact]`s
plus a two-case non-finite `[Theory]`: a +12 dB change at 1 s splits the
render into two measurable levels; a no-event render matches the static path
sample for sample; unsorted / non-finite / out-of-range / unknown-param are each
rejected by index with the buffer untouched; `"bypass"` removes 18 dB from the
second half) and `JuceDspAutomationLayoutTests` (4), with the device-refusal case
in `DspOfflineAutomationDeviceRefusalTests`.

**The case that pins "heard BY sample F" is
`TheEventTakesEffectOnExactlyItsSample_NotAtTheNextChunkBoundary`, and none of
the others can** — an RMS over half a second cannot tell frame F from frame
F + 128, which is exactly the error a FIXED-stride render makes. It renders the
same rig twice, clean and with one event at frame 12,345, and asserts the FIRST
frame at which the two differ is 12,345. Verified non-vacuous by deleting the
`until` shortening in `dspProcessOfflineAutomated` so the loop chunks at a flat
512: the first difference moved to 12,800 — the next chunk boundary — and
nothing else in the class went red.

## Sample-accurate MIDI + MIDI OUTPUT (ABI v34)

`dca_engine_send_midi_at`, `dca_engine_get_midi_timing_stats` /
`_reset_midi_timing_stats`, and the six `dca_midi_output_*` exports over
`src/MidiOutputDevice.h`.

**What was wrong.** `renderInstrumentBus` stamped EVERY event at sample 0 of
whichever block drained it — up to one whole buffer, 5-10 ms, on every note of a
charted performance. The comment that justified it (`p3-51`) said the queue
"would need to carry (event, sampleOffset) pairs ... and we'd need a
free-running sample counter on the audio thread to convert wall-clock to
per-block offset". **ABI v26 shipped exactly that counter** (`renderFrames_`)
and nobody came back here. That is the shape worth remembering: a deliberate
simplification whose stated blocker was removed by an unrelated feature two
years later, with nothing linking the two.

- **The mechanism is the v26 SCHEDULED CLICK mechanism, deliberately.** Drain
  the lock-free fifo into an audio-thread-only PENDING set (`midiPending_`),
  then fire everything whose frame lands in THIS block at its exact offset.
  Clamping into whichever block happened to drain the event — the obvious
  shortcut — is the pre-v34 error under a new name, and it is what the gate's
  phase B exists to catch. Parking is also what makes an out-of-order send
  harmless: dispatch is by FRAME, so a far-future event cannot hold back the
  ones behind it.
- **A late event goes at sample 0 and is COUNTED.** `midiLateEvents_` is the
  only signal a caller has that its scheduling horizon is shorter than the
  engine's own latency; silently placing it at 0 is exactly the old behaviour
  and would have been indistinguishable from the fix working.
- **A status byte below 0x80 is REJECTED at both the ABI and the engine.**
  `packed == 0` is the queue's EMPTY-SLOT marker, so accepting one would make
  the parked set unreadable: every event after it reads as a free slot.
- **`discardQueuedMidi` clears the PARKED set too.** An event that has already
  left the fifo and is waiting for its frame is exactly as stale as one still
  in it, and a scheduled note-on surviving an instrument swap is a note nothing
  will ever turn off. It takes `instrumentMutex_` SEQUENTIALLY (never nested
  inside `remoteBridgeMutex_`); neither caller holds it, because attach
  discards before publishing the plugin and detach after its own barrier.
- **The out-of-process VST sandbox leg is sample-accurate too** since bridge
  v3 (JuceSharp #28): it parks events in its own set and sends each one's
  offset across the mapping — see "The sandbox bridge carries MIDI sample
  offsets" above.

**MIDI OUTPUT dispatch: one dedicated high-priority thread per device, and
`juce::MidiOutput::sendBlockOfMessages` was rejected for two reasons.** It takes
the output's `CriticalSection` on the CALLING thread to splice into its pending
list (so it can never be reached from anything near the audio thread), and it
takes its deadlines in `Time::getMillisecondCounter()` MILLISECONDS, which
throws away the whole point of a sample-accurate schedule. `MidiOutputDevice`'s
dispatcher sleeps in <=20 ms steps while its earliest deadline is far away and
BUSY-SPINS the last ~1.5 ms on `getHighResolutionTicks` — a `Thread::wait(1)` is
1 ms only while some process on the box holds `timeBeginPeriod(1)` and ~15.6 ms
otherwise, which is the same trap that made `RtBridgeWaitIsBounded` phase C
flake. `sendMessageNow` is called OUTSIDE the queue mutex, because it reaches
the OS MIDI API and is not bounded on a wedged interface.

- **`dca_midi_output_send_at` takes the ENGINE as a parameter and the handle
  does NOT cache one.** The frame -> QPC-deadline conversion runs on the
  CALLING thread against the render clock; an output that held an
  `AudioEngine*` would outlive it the moment a caller destroyed the engine
  first. The deadline also adds the engine's OUTPUT LATENCY (master limiter
  included), because a hardware synth's note has to line up with what the user
  HEARS, not with the sample the callback has just rendered.
- **Nothing in `MidiOutputDevice.h` is audio-thread safe and it is not meant to
  be**, which is why its queue is a plain mutex-guarded vector. A lock-free
  structure there would be ceremony that invited someone to call it from the
  callback.

Gated by `RtScheduledMidiLandsOnItsSampleWithoutAllocating` (four offsets inside
one block land on samples 0/37/128/255; an event named for the NEXT block is not
dispatched early; a late event goes at 0 and bumps the counter; 256 scheduled
events over 32 blocks allocate nothing), verified non-vacuous by forcing the
offset back to 0 and watching three assertions go red. Managed:
`MidiTimingTests` + `JuceMidiOutputTests`.

## The MIDI fifo is drained, never `reset()` (2026-08-29)

`midiFifo_` is a `juce::AbstractFifo`: **single producer, single consumer**, and
`reset()` rewrites BOTH index atomics, so it is legal only when nothing else is
touching it. `attachInstrumentPlugin` and `detachInstrumentPlugin` both called
it, and each had two racers:

- the **consumer** — `collectBridgeMidi` runs on the audio thread whenever
  `instrumentPlugin_ == nullptr` (the sandbox-bridge leg, under
  `remoteBridgeMutex_`). That is *exactly* the window an attach resets in;
  `detach`'s own `instrumentMutex_` barrier holds out the in-process reader and
  nothing else.
- the **producer** — `sendMidiShort` is called from the JUCE MIDI input thread,
  not the message thread, and holds no lock at all.

`AudioEngine::discardQueuedMidi()` replaces both call sites: a consumer-side
`prepareToRead` / `finishedRead` (legal against a live producer) under
`remoteBridgeMutex_` (the barrier the audio-thread consumer already uses). A
plain `std::lock_guard`, not `DCA_RT_LOCK_GUARD` — it is a message-thread
acquire, and the macro would publish a new name into the RT gate's observed
lock-site set.

Two halves, because the race itself is not reproducible on demand (the
audio-thread consumer only runs with the sandbox bridge enabled, and a torn
`AbstractFifo` loses or duplicates events rather than corrupting memory):
`RtInstrumentAttachDiscardsStaleMidi` proves the discard still discards — 400
stale events dropped, then 24 live ones delivered — and
`NativeRtSourceLintTests.MidiFifo_IsNeverResetWhileAConsumerCanBeRunning` proves
nobody puts `reset()` back.

## A NULL handle is `DCA_ERR_INVALID_ARG`, never `DCA_OK`

TEN exports used to answer a null handle with `DCA_OK` and a plausible zero.

Eight were fixed 0.48: `dca_engine_get_input_peak`, the six
`dca_engine_read_input_capture{,_stream,_stream2}{,_for_player}` and
`dca_plugin_save_state`. **Every one of the capture reads is called with its
status discarded** — the wrapper uses `written` — so a dead engine read as "the
ring was empty", which is silence that looks like a quiet guitar, forever, with
nothing logged. `dca_plugin_save_state`'s caller DOES check, and would have
written an empty state into a saved rig.

Argument validation still comes FIRST (a null `dest`, a non-positive `count`, a
seat past `DCA_MAX_PLAYERS`), so the handle check cannot shadow one of those.
The one deliberate `DCA_OK` that remains is a LIVE plugin with no instance yet:
it genuinely has no state, so zero bytes is a correct answer. Pinned by
`NullHandleRejectionTests`, which P/Invokes each export with `nint.Zero`
directly — the wrapper cannot produce a zero handle (`NativeHandleOwner` throws
`ObjectDisposedException` first), which is why its own tests never saw this.

**The last two went 2026-08-29: `dca_source_get_position_seconds` and
`dca_source_get_length_seconds`.** Their own family already disagreed with them
— `play` / `pause` / `stop` / `seek` / `set_volume` and
`dca_source_get_audible_position_seconds` have always rejected — and the zero
they wrote is the dangerous kind: position 0.0 reads as "the song is at the
start", length 0.0 as "an empty file". Both now reject a NULL source AND an
**orphaned** one (the shell `dca_engine_destroy` leaves when the caller frees
the engine first), which is a real state, not just a caller bug:
`AuditRegressionTests.audit9_SourceOutlivingEngine_IsOrphanedNotUaf` was
asserting the old `DCA_OK` + 0.0 and now asserts the rejection. `*out` is still
zeroed first, so a caller that ignores the status is no worse off. **The managed
`JuceAudioSource.Position` / `.Length` go through `JuceAudioEngine.Check`, so an
orphaned source now THROWS there instead of silently reporting 0** — dispose
sources before the engine.

`dca_engine_get_player_count` is deliberately NOT in this set. It returns
`int32_t` with no status channel and 0 is its documented sentinel (the wrapper
returns 0 for a disposed engine of its own accord); a live engine clamps to
1..`DCA_MAX_PLAYERS` and can never report 0, so unlike the getters above it is
not a plausible answer. Pinned by `NullHandleRejectionTests` so it cannot drift.

## The failure sentinel of a VALUE-returning export is `-1` (W4-N8, 2026-09-11)

The section above is about exports that return `dca_status`. The ~21 that return
a VALUE instead had **two conventions for one concept, with no rule saying which
applied where**: `dca_engine_dsp_get_chain_latency*`, `_add_node`,
`_get_node_string`, `dca_plugin_get_input_player` and the module-label pair used
`-1` for "bad handle / bad seat / unknown id"; `_node_count*`, `_get_node_ids*`
and `_read_node_meters` used **`0`, which is also the perfectly normal answer for
an EMPTY chain**. So a managed caller polling a rig could not distinguish "you
passed a dead engine" from "there are no pedals" — which is how a
handle-lifetime bug reads as an empty board, and it is the same shape as the
`position 0.0` fault above.

**`-1` everywhere now; `0` stays a legal ANSWER everywhere.** Source-compatible
for any caller that only ever passed live handles or tested `<= 0`, which all
three wrapper call sites do (`JuceDspChain.NodeIds`' `count <= 0`, and
`Math.Max(0, …)` in `JuceDspChain.ReadNodeMeters` / `JuceDspMeters.Read`) — **so
this needed no `DCA_ABI_VERSION` bump.** It IS a behaviour change a third-party
binding testing `== 0` for failure would see; the rule is stated once, at the
top of the rig block in `dotcharter_audio.h`.

**`dca_engine_dsp_get_metering` is the one deliberate exception and keeps `0`.**
It is a PREDICATE with two legal answers, every caller reads it as `!= 0`, and
`-1` is non-zero — a dead handle would report "metering is ON". "Could not ask"
and "off" are the same actionable answer there. Both halves are pinned by
`AbiSentinelAndAliasTests`, which asserts `-1` across four exports AND `0` for
the predicate, so "fix" the exception and the gate goes red.

## A pre-v28 export is a ONE-LINE ALIAS for seat 0 (W4-N10)

Stated in as many words at the v35 aliases and honoured everywhere except the
four capture exports, which were full COPIES of their `_for_player` twins —
eight near-identical bodies, two independent drift surfaces for one behaviour,
and the copies additionally left `*out_written` at whatever the caller's stack
held on every failure path. They are `return …_for_player(handle, 0, …);` now,
and the twins zero their out-params before the first possible failure, which is
the rule `dca_engine_dsp_get_param` states verbatim.

`AbiSentinelAndAliasTests` pins the SHAPE (one statement, no `reinterpret_cast`
of its own); `SeatZeroAliasEquivalenceTests` pins the BEHAVIOUR on the null
device — same status, same written count, same stream position, for good
arguments and for failure arguments — plus a seat past the last being REJECTED
rather than clamped, without which the equivalence would be trivially true for
every seat.

## Engine rebuild: what survives a device change, and what does not

`dca_engine_reconfigure` and `dca_engine_set_device_type` both go through
`AudioDeviceManager::setAudioDeviceSetup`, which CLOSES and REOPENS the device.
The audio callback registration survives that (the manager owns the callback
list and re-issues `audioDeviceAboutToStart`), so **`prepareMasterBus` is the one
place that learns the new rate / block size / channel count** — anything that has
to follow the device belongs there, and everything currently in it does follow:
the three fixed filters, the six v29 EQ bands per bus and per seat, the mirror's
producer rate (`mirror_.setEngineSampleRate`), every seat's chain buffer, every
seat's `dcrig` rig, the instrument buffer and the MIDI scratch.
`audioDeviceAboutToStart` adds the render clock, the drop-tune scratch, the
sandbox-bridge silence buffer and the xrun session reset.

Three things did NOT follow it. Each was found by asking the question rather than
by a bug report:

1. **An attached VST3 was prepared exactly once, at attach time.**
   `attachInputPlugin` / `attachInstrumentPlugin` call
   `prepareToPlay(deviceSr_, deviceBlock_)` and nothing re-ran it, so after a
   reconfigure to a different rate every loaded plugin's time constants were
   detuned — and a plugin prepared for 256 frames that is handed 1024 is
   entitled to ALLOCATE, on the audio thread. `prepareMasterBus` now ends with
   `reprepareAttachedPlugins()`. It needs no wait-one-block barrier (unlike
   `detachInputPlugin`) because `audioDeviceAboutToStart` runs with no callback
   in flight, and it is the same place `juce::AudioProcessorPlayer` prepares the
   processor IT hosts — so a VST3 is called here on the thread every JUCE host
   calls it on.
2. **`stop()` closed the streaming mirror and nothing remembered the device
   name**, so a `stop()` + `start()` rebuild ended the OBS / Discord stream
   permanently and silently. `requestedMirrorDevice_` remembers the REQUEST and
   `restoreMirrorAfterStart()` reopens it after `addAudioCallback` (which is
   what makes `deviceSr_` current). A mirror device that has gone away is logged
   and skipped — it must never fail the primary start.
3. **`start()` had a failure exit that did not clear `started_`.** The
   "Requested audio driver not available" return left `started_ == true` with no
   device open and no callback added, so the NEXT `start()` hit the idempotence
   guard at the top and returned SUCCESS: a permanently dead engine reporting
   itself as running, and a `stop()` that then tore down nothing. **Every failure
   exit in `start()` must clear `started_`** — the other two already did, which
   is exactly why this one was invisible.

**`stop()` is final; `start()` after it is REFUSED (#27).**

- `stop()` does `mixer_.removeAllInputs()` + `sources_.clear()`. Every open
  `TransportSource` survives as an object but is detached from the mixer
  forever, so a stop/start cycle used to play silence while `dca_engine_start`
  returned `DCA_OK`. Blind re-attachment is NOT the fix: a stem-grouped
  source's `sourceForMixer()` belongs to `stemSubMixer_`, not `mixer_`, so
  re-adding the saved set would double-route exactly the grouped sources. So
  `start()` now returns an error once a `stop()` has stopped the engine
  (`stoppedAfterStart_`): `dca_engine_start` answers `DCA_ERR_DEVICE` with the
  reason logged, and `JuceAudioEngine.Start()` throws `InvalidOperationException`.
  `stop()` also removes the audio callback, which `dca_engine_reconfigure` does
  NOT re-add, so **the only way back is to recreate the engine**
  (`dca_engine_destroy` + `dca_engine_create`); to change device WITHOUT stopping,
  reconfigure. No signature changed, so no ABI bump (abi-policy: a bug fix
  is a PATCH). `EngineRestartRefusalTests`.
- `stop()` also leaves `stemGroupStretcher_` / `stemSources_` populated while
  `removeAllInputs()` has already unhooked the stretcher, so the stem group
  believes it is wired when it is not.

## Input VST attachment and NAM publication

`dca_plugin_attach_to_input_for_player` serializes the complete check, detach,
prepare/append and attachment-state update on the JUCE message thread. Detach
uses the same transaction boundary. Keeping only prepare/append inside
`callSync` allows simultaneous callers to insert an instance twice.

NAM clear invalidates queued/in-flight generations under `workMutex_`.
After model/resampler preparation, the worker checks the generation and publishes
active model plus status under that same mutex. Retirement remains outside it;
no loader work or locking is added to `process`.

Managed node meter polling is exposed by `src/JuceSharp/Dsp/JuceDspMeters.cs`
(in the native wrapper assembly, namespace `JuceSharp.Dsp`). It marshals the
existing ABI-v30 enable/read/bulk-read surface; callers protect engine lifetime
and coordinate enable ownership. The separate `JuceSharp.Dsp` project remains
pure managed and does not load the native engine.
