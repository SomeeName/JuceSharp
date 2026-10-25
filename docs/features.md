# Features & Roadmap

> Status legend: ✅ shipped &nbsp;·&nbsp; 🔨 in progress &nbsp;·&nbsp; 🟢 next &nbsp;·&nbsp; 🟡 later &nbsp;·&nbsp; ⛔ out of scope

## Package SKUs

**Only win-x64 actually ships.** `src/JuceSharp/JuceSharp.csproj` declares no
`RuntimeIdentifiers` and packs only `runtimes/win-x64/native/*`; CMake has some
`APPLE`/`UNIX` branches; `.github/workflows/build.yml` builds and gates win-x64
and carries a `continue-on-error` Linux job that only measures the distance to a
Linux build. No Linux/macOS build has been produced or tested. Treat those
platforms as unbuilt, not "supported".

| Package                        | Platforms | Includes                                                                    |
| ------------------------------- | --------- | ---------------------------------------------------------------------------- |
| `JuceSharp`                     | win-x64   | Managed wrapper + core engine + DSP rig + VST3 host + MIDI + `JuceStemSeparation`. Packs `runtimes/win-x64/native/` (`juce_sharp.dll` + `juce_vst_host.exe`) into the nupkg — the DLL is a **build artifact you produce with CMake before `dotnet pack`**, not something tracked or downloaded. |
| `JuceSharp.Dsp`                 | any       | Pure-managed analysis DSP. No native dependency.                            |
| `JuceSharp.AudioIo`             | win-x64   | `JuceAudioDecoder` (native decode), `WavIo`, `WavWriter` (streaming, 0.9), `AudioBuffer`. |
| `JuceSharp.StemSeparation`      | any       | The **Python sidecar** pipeline (`StemSeparator`, `PythonEnvManager`) plus `CkptToOnnxConverter`. |
| `JuceSharp.StemSeparation.Onnx` | win-x64   | In-process ONNX HTDemucs / MDX-Net / BS-RoFormer inference (`Microsoft.ML.OnnxRuntime.DirectML`: any DirectX 12 GPU, or the CPU). |

There is no `JuceSharp.Native` NuGet package and no `JuceSharp.AI` package.
`JuceSharp.Diagnostics` **is** the sixth packable project and it packs as a
`dotnet tool` (`PackAsTool=true`, `ToolCommandName=juce-stems-diag`) — install
it globally and run `juce-stems-diag`, which is the point: QA and on-site
debugging need it without a checkout.

**No csproj declares a `<Version>` at all.** All six packages take it from the
one `<Version>` in `Directory.Build.props` at the repository root (**1.3.0**),
with `AssemblyVersion` derived from it by the SDK, so every package always
carries the same version. `PackagingLintTests` asserts both halves, and that
`src/JuceSharp.Native/CMakeLists.txt` (which feeds `dca_version_string`) agrees.

## Core engine

| Feature                                  | Status | Notes                                                  |
| ---------------------------------------- | ------ | ------------------------------------------------------ |
| ASIO / WASAPI / CoreAudio / ALSA / JACK  | ✅     | Driver picked by `JuceEngineConfig.PreferredDriver`, or by `OutputTypeName` for a specific WASAPI mode. |
| Device catalogue, one call               | ✅     | `JuceDeviceCatalog.Enumerate()` (ABI v13 host-scoped enumeration). |
| Open-with-fallback                       | ✅     | `JuceEngineFactory.OpenWithFallback` — JUCE otherwise picks the host's first device, often an offline ASIO interface. |
| Live device hot-swap                     | ✅     | `EnumerateOutputDevicesLive()`, `SetDeviceType` (ABI v12). |
| In-place reconfigure                     | ✅     | `Reconfigure(sr, frames, outId, inId)` (ABI v8).       |
| Channel masks                            | ✅     | `ChannelMask` helpers + `Input/OutputChannelMask`.     |
| Mirror output to a second device         | ✅     | `SetMirrorOutput` / `SetMirrorDevice`; survives a device rebuild. |
| Master EQ (HP / LP / BP)                 | ✅     | Per-filter freq + Q.                                   |
| Master parametric EQ bank                | ✅     | **Six** typed bands + route + trim + spectrum tap (ABI v29). All off at engine start; a band is published only when it is audible. |
| Master gain                              | ✅     | Linear, ramped across the block (never a per-block constant). |
| Click track (beat / downbeat / clap)     | ✅     | `PlayClick(type)`.                                     |
| Sample-accurate click scheduling         | ✅     | `TryGetRenderClock` + `ScheduleClick(type, frame)` + `ClearScheduledClicks` (ABI v26). |
| Callback load (CPU meter)                | ✅     | `TryGetCallbackLoad` → `CallbackLoad(Average, Peak)` (ABI v46), fractions of the buffer period measured inside the callback. |
| Dropout / xrun counters                  | ✅     | `TryGetXrunStats` → `XrunStats` (ABI v30). Three counters, three different faults; `DeviceXruns` is `null` when the driver cannot report. |
| Output latency read-back                 | ✅     | `OutputLatencySamples` (ABI v7).                       |
| Headless null device                     | ✅     | `DCA_ENABLE_NULL_DEVICE=1` — device-dependent tests on a box with no sound card. |
| Lock-free MIDI short send                | ✅     | `SendMidi(status, d1, d2)`, plus the sample-accurate `SendMidi(…, renderFrame)` overload. |
| Per-source mute / solo                   | ✅     | `JuceMixer` + `JuceMixerChannel` (managed).            |
| Master limiter / brick-wall              | ✅     | `SetMasterLimiter(enabled, ceilingDb, releaseMs)` (ABI v34). Look-ahead soft limiter over the FINAL device buffer, before the streaming-mirror pickup. **OFF by default since v35 — the host opts in.** |

## Sources & playback

| Feature                              | Status | Notes                                                     |
| ------------------------------------ | ------ | --------------------------------------------------------- |
| `OpenFile(string)`                   | ✅     | WAV, AIFF, FLAC, Ogg, MP3 via the JUCE format manager.    |
| `OpenMemory(ReadOnlySpan<byte>, hint)` | ✅   | Play from bytes you already hold — no temp file.          |
| Multi-source playback (single clock) | ✅     | Every `JuceAudioSource` mixes into the master.            |
| Per-source `Volume` / `Mute` / `Pan` | ✅     | `Volume`/`Mute` share a small lock (`_levelGate`) so mute cannot lose a concurrent volume write; `Pan` is atomic. `Volume` is **clamped at 0 on both sides** — the wrapper folds a negative to 0 and ABI v33 added a native `[0, +inf)` clamp, because a negative gain is a phase inversion, not a quieter source — and is **not bounded above**: boost past unity ships. A `NaN` is deliberately passed through so the native rejection surfaces as a `JuceAudioException` instead of being laundered into a muted source. |
| Real-time tempo (Rubber Band)        | ✅     | `TempoRatio`; `SetStretchQuality(faster)` swaps R3 ↔ R2 for sources opened after the call. |
| Real-time pitch (semitones)          | ✅     | `PitchSemitones`.                                         |
| Shared stem-group stretcher          | ✅     | `StemGrouped` + `StemGroupTempo` / `StemGroupPitchSemitones` — one stretcher for a whole stem set, so the windows stay aligned and the mix does not comb-filter. |
| High-quality load-time resampling    | ✅     | `LoadResampler` — a rational polyphase FIR applied **once at load**, zero group delay by construction. |
| Audible-position read-back           | ✅     | `AudiblePosition` accounts for stretcher latency, and is the same output sample the render clock reports. |
| A-B loop region                      | ✅     | `JuceAudioSource.Loop = new JuceLoopRegion(a, b)` — **polling**, ~5 ms. |
| Sample-accurate native loop          | ⛔     | Not implemented. The ABI exports none; the polling region above is the only looping there is. |
| Stem-aware multitrack mixer          | ✅     | `JuceMixer` with mute / solo / volume / pan per channel.   |
| Crossfade between sources            | 🟡     | Useful for setlist-style apps.                            |
| Offline render to file               | ✅     | `RenderToWav(src, dst, tempo, semitones, sr)` (ABI v4) — decode + WAV write. Throws `NotSupportedException` if asked to stretch or shift. |
| Offline decode to PCM                | ✅     | `JuceSharp.AudioIo.JuceAudioDecoder.Decode/DecodeAs`.     |

## Input & monitoring

| Feature                          | Status | Notes                                                  |
| -------------------------------- | ------ | ------------------------------------------------------ |
| Direct-input monitoring          | ✅     | `SetMonitorGain` (ramped).                             |
| Pre-FX input gain                | ✅     | `SetInputGain` (ramped).                               |
| Peak meter readout               | ✅     | `GetAndResetInputPeak` — **linear**, read-and-reset.   |
| Dry capture ring for analysis    | ✅     | `ReadInputCapture` (windowed, v11), `ReadInputCaptureStream` (cursored, v15), `TryGetInputCaptureSync` (host-clock sync, v16). |
| Per-player seats                 | ✅     | Up to four independent live rigs on one open device — own channel, monitor, peak, capture ring, DSP chain and VST chain (ABI v28). At a count of 1 the callback is byte-for-byte the pre-v28 path. |
| Plugin chain on input            | ✅     | `JuceAudioPlugin.AttachToInput(player)`.               |
| Recording to file                | ✅     | `JuceSharp.AudioIo.WavWriter` (0.9) — a **streaming** 16/24-bit PCM or float32 writer: construct over a path or a `Stream`, `Write`/`WritePlanar` as the capture ring drains, and the RIFF header is patched on `Dispose`. `WavWriter.IsUnfinished` reports a file whose writer never closed. There is still no `RecordInput` *export* — the engine does not own a recorder — so the loop is yours: poll `ReadInputCaptureStream`, push to the writer. See [`docs/api/wav-writer.md`](api/wav-writer.md). Whole-buffer alternative: `WavIo.WriteFloat32`. |
| Chromatic tuner                  | ✅     | `JuceSharp.Dsp.JuceChromaticTuner` (YIN, with an FFT path), and `JuceSharp.Dsp.JuceTunerStream` (0.9) for a live needle: push samples, read `Latest`, with median filtering + hysteresis over the raw readings. |

## Plugin hosting (VST3)

| Feature                              | Status | Notes                                              |
| ------------------------------------ | ------ | -------------------------------------------------- |
| Scan the disk for bundles            | ✅     | `JuceVst3Scanner.Scan()` → `Vst3Entry` — a file walk, no instantiation. |
| Load by path                         | ✅     | `LoadPlugin(string) → JuceAudioPlugin`.            |
| Editor window with parent HWND       | ✅     | `ShowEditor()` / `HideEditor()`.                   |
| Editor-closed notification           | ✅     | `EditorClosedByUser` (ABI v14).                    |
| Attach to input / instrument graph   | ✅     | `AttachToInput(player)` / `AttachToInstrument`.    |
| Reported plugin latency              | ✅     | `LatencySamples` (ABI v10). Not compensated automatically. |
| Out-of-process sandbox               | ✅     | `SetRemoteBridge` + `juce_vst_host.exe` over shared memory (ABI v19), with `TryGetRemoteBridgeStats`. |
| Shutdown-safe teardown               | ✅     | Engine catches orphaned plugins and tears editors down first. |
| Parameter enumeration & automation   | ✅     | `EnumerateParameters()`, `GetParameter`/`SetParameter` (ABI v4). |
| State save / restore                 | ✅     | `SaveState()` / `LoadState(byte[])`.               |
| Bypass toggle                        | ✅     | `Bypass { get; set; }`.                            |
| VST3 instrument MIDI routing         | ✅     | `AttachToInstrument()` + `SendMidi`.               |
| AU / LV2 hosting                     | ⛔     | JUCE supports it, but out of scope today.          |

## DSP tone rig (input)

| Feature                              | Status | Notes                                              |
| ------------------------------------ | ------ | -------------------------------------------------- |
| In-house reorderable effect chain    | ✅     | `DspModule` / `DspChain` / `DspModuleFactory` (ABI v21), ahead of the input VST chain. |
| Built-in module catalogue            | ✅     | Header-only `dcdsp` cores in `src/JuceSharp.Native/src/dsp/cores/` (63 headers). Enumerate it at runtime with `dca_dsp_module_type_count` / `_type_info`. |
| Per-seat chains                      | ✅     | One chain per seat; node ids are engine-global (`player * 1_000_000 + local`) (ABI v28). |
| Split / parallel branches            | ✅     | Splitter + merger + per-node branch tags (ABI v23). |
| String parameters (IR / model paths) | ✅     | `set_node_string` / `get_node_string` (ABI v22).   |
| Declared discrete params             | ✅     | Named value labels (v24) or an ordinal step (v27). |
| Crossfaded structure changes         | ✅     | Bypass / un-bypass / move / remove all ramp over 10 ms **in the chain**, never in a core. A parked node's state is cleared on the way back in. |
| Parked node insertion                | ✅     | `add_node_bypassed_for_player` — insert at zero wet, configure, then fade in (ABI v32). Split/merger topology is still immediate. |
| Per-node meters + latency            | ✅     | Poll-only, off until asked for (ABI v30). `JuceSharp.Dsp.JuceDspMeters` is the managed poll. Chain latency is the **max** over a split region's branches. |
| Dual cabinet IR slots                | ✅     | Two independently controlled IR slots in the cab module. |
| Delay tempo sync + bypass trails     | ✅     | Opt-in, both default off; the legacy delay output stays bit-exact. |
| Neural amp capture (NAM/.aidax)      | ✅     | `namamp` over `mikeoliphant/NeuralAudio` (`DOTCHARTER_DSP_NEURAL`, ON). |
| Live-input drop tune                 | ✅     | CSVD shifter, **down-only** (`dcdsp_droptune.h`, ABI v25); per seat since ABI v31. Never a phase vocoder — see `AGENTS.md`. |
| Managed wrapper for the rig          | ✅     | `JuceDspChain` / `JuceDspNode` / `JuceDspModuleCatalog` (`engine.GetDspChain(seat)`); see `examples/JuceSharp.Examples.ToneRig` and [`docs/api/juce-dsp-chain.md`](api/juce-dsp-chain.md). |
| Edit scopes — a preset is ONE change | ✅     | `dca_engine_dsp_{begin,commit,abort,get_editing}_edit_for_player` (ABI v34), managed as `using var edit = chain.BeginEdit();` → `JuceDspEditScope`. Writes accumulate against a shadow; the commit costs **at most two** publications whatever the preset size, not one per node. Abort restores STRUCTURE only — membership, order, bypass, branch tags — never parameter values. A `begin` on a seat that already has an open scope ABORTS the stale one and logs a Warning. |
| Branch delay compensation            | ✅     | ABI v34, **on by default**: the merger carries one delay line per branch set to (max − branch), so summing branches of different latency is no longer a comb. `chain.BranchLatencyCompensation = false` restores the pre-v34 path byte for byte (some players want the comb). An UNMATCHED splitter auto-merges inside `DspChain::process` and is **not** compensated. |
| Node sidechain / keying              | ✅     | ABI v34. `node.Sidechain = JuceDspSidechainSource.SeatInput` or `.SongMix` on a module that advertises the capability (`compressor`, `gate` today); anything else is REJECTED, never silently stored. An unavailable key falls back to the module's own input, **never to silence** — a gate keyed from silence sits closed. Capability is advertised by `dca_dsp_module_type_flags` bit 0. |

## MIDI

| Feature                              | Status | Notes                                              |
| ------------------------------------ | ------ | -------------------------------------------------- |
| MIDI short-message send              | ✅     | Lock-free ring; `SendMidi(status, d1, d2)`, and `SendMidi(status, d1, d2, renderFrame)` for SAMPLE-ACCURATE placement (ABI v34) with `TryGetMidiTimingStats` reporting scheduled / late / dropped. |
| MIDI input (hardware)                | ✅     | `EnumerateMidiInputs()` + `OpenMidiInput(index, handler)` (ABI v4). |
| MIDI **output** (hardware)           | ✅     | `EnumerateMidiOutputs()` + `OpenMidiOutput(index)` -> `IJuceMidiOutput` (ABI v34). One high-priority dispatcher thread per device, never the caller's; `SendAt(data, renderFrame)` fires when that frame reaches the EAR. |
| Standard MIDI File (SMF) read / write| 🟡     | Rocksmith2014.NET handles this for DotCharter today. |
| 24 PPQN MIDI clock send              | 🟡     | Niche; trivial to add when needed.                 |

## Analysis

| Feature                              | Status | Notes                                              |
| ------------------------------------ | ------ | -------------------------------------------------- |
| Waveform peaks (offline, native)     | ✅     | `JuceAudioEngine.GenerateWaveform(path, blockMs)` and `GenerateWaveformFromMemory` (ABI v20). |
| Waveform peaks (managed, min/max)    | ✅     | `JuceSharp.Dsp.JuceWaveformPeaks.Build` — buckets for any in-memory buffer. |
| BPM / beat detection                 | ✅     | `JuceBeatDetector` (spectral flux + autocorrelation, DP beat tracker, downbeats). `Result.TempoMap` is a piecewise-constant `IReadOnlyList<JuceTempoSegment>` — one `(Time, Bpm)` segment per stretch of steady tempo, opt-in via the local-period pass, with `SegmentToleranceBpm` deciding when a drift becomes a new segment. |
| Chord detection (chromagram)         | ✅     | `JuceChromagram` + `JuceChordDetector` (48 triad templates). |
| FFT (radix-2)                        | ✅     | `JuceFft`.                                         |
| Spectrogram (FFT bitmap)             | ✅     | `JuceSpectrogramBuilder` → `JuceSpectrogram`.      |
| Onset detection                      | ✅     | `JuceOnsetDetector` (streaming).                   |
| Polyphonic pitch                     | ✅     | `JucePolyphonicPitch` → `JucePitchCandidate[]`.    |
| Guitar event analysis                | ✅     | `JuceGuitarAnalyzer` → `JuceGuitarEvent` with technique flags. |
| Note on/off stream                   | ✅     | `JuceNoteStream` → `JuceNoteEvent`.                |
| Detection v8 (CQT chart verification)| ✅     | `JuceConstantQ` + `JuceChartVerifier` + `JuceLeadTracker`, configured by `LiveCqtConfig` — **the single source of truth; never re-type a shipping constant**. |
| Stem separation                      | ✅     | Three backends — see below.                        |

## Stem separation

| Backend | Where | Notes |
|---|---|---|
| `JuceSharp.StemSeparation.Onnx` | its own package | In-process ONNX: HTDemucs, MDX-Net, BS-RoFormer, over DirectML (any DX12 GPU) or the CPU. No Python. |
| `JuceStemSeparation` | **core `JuceSharp`** | The native demucs.cpp path over the `dca_stems_*` ABI (v5). Compiled in only with `-DDOTCHARTER_STEMS_DEMUCS_CPP=ON`, which is **off by default** — so `IsSupported` is `false` in a stock build. |
| `StemSeparator` | `JuceSharp.StemSeparation` | The Python sidecar: an embedded CPython 3.11.9, torch + demucs + bs_roformer, run as a child process so a torch crash cannot take the host down. |

`JuceSharp.StemSeparation` also carries `CkptToOnnxConverter`, which turns a
`.ckpt` checkpoint into the ONNX model the in-process backend consumes.

Added in 0.9:

| Feature | Status | Notes |
|---|---|---|
| Model acquisition | ✅ | `OnnxModelCatalog` — `All` / `FindById` / `ForKind` / `DefaultFor`, plus `EnsureAsync` (download to a per-kind cache directory, mirror failover, optional SHA-256 pin) and `VerifyAsync` / `ComputeSha256Async`. `DemucsGgmlModelCatalog` is the same shape for the native demucs.cpp backend's ggml weights. The public kind-less door is `VerifiedDownload` (`EnsureFileAsync` / `VerifyFileAsync` / `ComputeSha256Async` / `ValidateSourceUrl`); the `VerifiedDownloader` underneath it — resumable range requests, verify-before-rename, progress reporting — is `internal` and cannot be called from your code. |
| Streaming progress | ✅ | `CoalescingProgressStream.Create(...)` turns a `report`-callback API into an `IAsyncEnumerable<T>`, dropping the oldest un-read sample past `DefaultCapacity` (32) so a consumer that stalls loses FRACTIONS, never phase transitions. Every `StemSeparationProgress` carries `Phase`, `Fraction`, `Message`, `Stem`, and since 0.9 `Backend` and a terminal `Result`. Exactly one `("done", 1.0)` per run, published by the caller-facing layer rather than the backend. |

> **Abandoning the enumeration does not stop the backend.** Breaking out of the
> `await foreach` cancels and returns, but the python child may still be running
> and `StemSeparator`'s process-global GPU slot may still be held. If you need to
> wait, drive the backend directly and keep its `Task`.

## Testing & safety nets

| Feature | Status | Notes |
|---|---|---|
| RT allocation / lock gate | ✅ | `juce_sharp_rtcheck` runs the real device callback under an allocation hook and a lock allow-list. |
| Native DSP regression gates | ✅ | `tone_regressions`, `delay_sync_trails`, `dualcab`, `lane_mixer`, `nam_lifetime` under CTest. |
| Offline rig rendering | ✅ | `dsp_process_buffer[_for_player]` and `dsp_process_buffer_automated[_for_player]` (ABI v34: a sorted automation-event list, chunked at each event boundary) — the same code the callback runs, with no device open. |
| Interop pins | ✅ | P/Invoke shape, enum ordinals, struct layout, UTF-8 marshalling, delegate catch-alls, finalizer safety, handle drain. Every ABI v33, v34 and v35 export is pinned by name against the header (`NativeMethodsTests`), and `AbiExceptionGuardLintTests` asserts the export COUNT it states, and `ExpectedAbiVersion` is asserted EQUAL to `DCA_ABI_VERSION`. |
| Test seams without native | ✅ | The seven `IJuce*` interfaces — `IJuceAudioEngine`, `IJuceAudioSource`, `IJuceAudioPlugin`, `IJuceMidiInput`, `IJuceMidiOutput`, `IJuceDspChain`, `IJuceDspNode` — so host code can be unit-tested without `juce_sharp.dll` or a sound card. `InterfaceSeamTests` lints both directions; `Wave2DocsInterfaceDocTests` keeps every member documented. See [`docs/guides/testing-without-native.md`](guides/testing-without-native.md). |
| "The native DLL is really loaded" | ✅ | `NativeAvailableFact`/`Theory` guards roughly a quarter of the suite by try/catching a native call, so an unloadable DLL used to read as a green run full of skips. `JUCESHARP_REQUIRE_NATIVE=1` (set by CI) turns that into a FAILURE and the probe now reports the exception instead of swallowing it — `Wave2NativeProbeTests`. |

## Out of scope (intentional)

| Feature                              | Why skipped                                           |
| ------------------------------------ | ----------------------------------------------------- |
| SmartMaster room calibration         | Not a mastering DAW.                                  |
| NetworkSync multi-device playback    | DotCharter is single-machine.                         |
| Audio matchering                     | Mastering workflow, not charting.                     |

## Releases

See the [Changelog](changelog.md).
