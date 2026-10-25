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

## Sub-projects (`src/`) — full table with contents: [docs/agents/layout.md](docs/agents/layout.md)

`JuceSharp/` thin C# P/Invoke wrapper (+ managed rig face `Dsp/JuceDspChain`) · `JuceSharp.Native/` C++ DLL with the versioned C ABI (`include/dotcharter_audio.h`, `DCA_ABI_VERSION`; ONE ABI translation unit `src/dotcharter_audio.cpp`) · `JuceSharp.Native/src/dsp/` tone rig (`dcdsp` cores) · `JuceSharp.Dsp/` managed detection (CQT, analyzer) · `JuceSharp.StemSeparation*`, `AudioIo`, `Diagnostics` · `JuceSharp.Native/tests/rt/` + `tests/JuceSharp.Tests/` RT gate and xunit.

## Task routing — "touching X -> read this file first"

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
| a hard rule / ABI-wave invariant you are not sure about (every `ABI vNN` entry, sentinels, scrubs, crossfades, lock rules) | `hard-rule-index.md` (grep it for the export or type name) |
| wrapper lifetime (`NativeHandleOwner`, `INativeChild`), callbacks, UTF-8 strings, XML docs, licensing, packaging, CI | `wrapper-rules.md` |
| the native build environment, FetchContent, JUCE tag bumps | `toolchain.md` |
| a sub-project's contents | `layout.md` |
| `.wem` Vorbis encode / decode | `wem-encoder.md`, `native-engine-abi.md` (v42-v44) |

(Bare names above are files in [docs/agents/](docs/agents/); plain links, not `@import`s, on purpose.)

## Hard rules (always; full text in `hard-rule-index.md` and `wrapper-rules.md`)

- All real-time audio lives in the native engine; the managed wrapper is a thin shell (marshalling + lifetime). No managed per-sample work on the hot path. The audio callback never allocates, locks outside the allow-list, or throws (`rt-safety-and-tests.md`); never add `setSize`/`resize` on it.
- **Incompatible ABI change => bump `DCA_ABI_VERSION` and rebuild the DLL** before running managed code. Shared-memory bridge offsets are `static_assert`ed both sides: layout change => bump `kBridgeVersion` and both mirrors. Value-returning export failure sentinel is `-1`, never `0`.
- Handle-owning wrappers use `NativeHandleOwner`; every use goes through `Acquire`/`TryAcquire`/`Release`. Drain never frees under a live call (it logs and LEAKS on timeout). The engine disposes its children before `dca_engine_destroy`. Do not re-open this as a TODO.
- Every managed delegate handed to native needs a catch-all body (a throw through C++ frames kills the process).
- Every string crossing the ABI is UTF-8 (`LPUTF8Str`, never `LPStr`); fixed out buffers truncate at a code point (`dcabi::utf8TruncatedLength`).
- Every new PUBLIC member needs a real XML doc comment; `CS1591` is an error. Never restore the `NoWarn` to green a build.
- Packages are NOT MIT: `juce_sharp.dll` is AGPLv3/GPL-linked. Only `JuceSharp.Dsp` may keep `PackageLicenseExpression=MIT`; a package declares an expression OR a file, never both.
- Packaging/analyzer/CI settings live in one place each (`Directory.Build.props/.targets`, `global.json`, `.github/workflows/build.yml`) and are linted; never grep CI YAML. A new `InternalsVisibleTo` outside this repo needs a current user.
- Never put a phase vocoder on the live input (drop tune is CSVD; read `dcdsp_droptune.h`). Never re-type a detection constant: change `LiveCqtConfig`.
- The shipped native pair (`juce_sharp.dll`, `juce_vst_host.exe`) links the MSVC runtime STATICALLY (`JUCESHARP_STATIC_CRT`, default ON, #176) and imports only Windows system DLLs; memory the DLL allocates is freed by its own `dca_*` export. Details: `hard-rule-index.md`.
- Submodule workflow: commit here first, then bump the pointer in the parent DotCharter repo. Don't paper over a native bug in the wrapper: fix it native-side and rebuild.

## Native rebuild + toolchain that actually exists on the owner's machine

```powershell
cd src/JuceSharp.Native
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

**Neither `cmake` nor `ctest` is on PATH.** Only **Visual Studio Build Tools 2026** is installed (`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`); use its bundled CMake: `...\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` (`ctest.exe` beside it). Generator is `"Visual Studio 18 2026"` (`17 2022` fails). CLion is not installed. In a fresh agent worktree point FetchContent at the main checkout (`-DFETCHCONTENT_SOURCE_DIR_JUCE=<main>/build/_deps/juce-src`, same for `RUBBERBAND`, `NEURALAUDIO`); that dir is used AS-IS, so after a `JUCE_TAG` bump clear the cache entry or delete `build/` (configure fails when the sources are not the pinned version). Details: `toolchain.md`. POST_BUILD deploys `juce_sharp.dll` to `runtimes/win-x64/native/` and `juce_vst_host.exe` next to it.

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
