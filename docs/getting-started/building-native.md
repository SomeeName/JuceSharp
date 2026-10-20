# Building the Native DLL

`juce_sharp.dll` is a **build artifact** — it is not tracked in the repository
and no NuGet package ships it today. You build it once with CMake, and the
post-build step drops it where the managed project expects it. An ABI bump
means rebuilding it before any managed code runs.

This page is also the **corresponding-source recipe** the binaries' AGPLv3 /
GPL terms call for (see the repository's `LICENSE` and
`THIRD-PARTY-NOTICES.md`): a checkout of this repository at the commit a
binary was built from, plus the dependencies `FetchContent` downloads at the
tags pinned in `src/JuceSharp.Native/CMakeLists.txt` (table below), rebuilds
`juce_sharp.dll` and `juce_vst_host.exe` with nothing else to fetch by hand.

## Prerequisites

| Tool                | Version              | Notes                                  |
| ------------------- | -------------------- | -------------------------------------- |
| CMake               | ≥ 3.22               | `cmake_minimum_required(VERSION 3.22)`. |
| C++ compiler        | anything with **C++17** | `CMAKE_CXX_STANDARD 17`, extensions off. |
| C compiler          | any                  | The project declares `LANGUAGES C CXX` — JUCE 8.0.15+ hard-errors without one, though no C of our own is compiled. |
| Git                 | any recent           | Needed by `FetchContent`.              |
| (Windows) Visual Studio | **2022 or later**, "Desktop development with C++" | Pick the generator that matches your toolset. |
| (Windows) ASIO SDK  | bundled inside JUCE  | See [ASIO](#asio) — nothing to download. |

> ℹ️ **Note**
> On the maintainer's machine the only installed toolchain is **Visual Studio
> Build Tools 2026**, so the generator there is `"Visual Studio 18 2026"`, and
> neither `cmake` nor the VS tools are on `PATH` (CMake is the copy the Build
> Tools bundle under `Common7\IDE\CommonExtensions\Microsoft\CMake`). See `AGENTS.md` § *Native rebuild* for the exact invocation used here.

## Configure & build (Windows)

**Pass the generator that matches the toolset you actually have** — a generator
for a Visual Studio that is not installed fails at configure time, before
anything is compiled. `"Visual Studio 17 2022"` below is an example, not a
default; with only Build Tools 2026 installed (the maintainer's case) it is
`"Visual Studio 18 2026"`.

```powershell
cd JuceSharp/src/JuceSharp.Native
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # or "Visual Studio 18 2026"
cmake --build build --config Release --target juce_sharp
```

The post-build step deploys `juce_sharp.dll` to
`JuceSharp/src/JuceSharp/runtimes/win-x64/native/`, and the out-of-process VST
host child `juce_vst_host.exe` next to it. Any project that has a
`<ProjectReference>` to `JuceSharp.csproj` will then copy both into its own
output.

## Configure & build (Linux)

```bash
cd JuceSharp/src/JuceSharp.Native
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target juce_sharp
```

Output goes to `JuceSharp/src/JuceSharp/runtimes/linux-x64/native/libjuce_sharp.so`.

## Configure & build (macOS)

```bash
cd JuceSharp/src/JuceSharp.Native
cmake -S . -B build -G Xcode
cmake --build build --config Release --target juce_sharp
```

Output goes to `runtimes/osx-arm64/native/libjuce_sharp.dylib` (or `osx-x64`
on Intel Macs).

> ⚠️ **Warning**
> The deploy path (`JUCESHARP_RID`) is derived from the host system, and the
> Linux/macOS branches exist in `CMakeLists.txt` — but only win-x64 has ever
> been produced and tested. Treat the other two as source-available, not
> supported.

## CMake options

Every switch below is declared in `src/JuceSharp.Native/CMakeLists.txt`.

| Option / cache variable       | Default | Description |
| ----------------------------- | ------- | ----------- |
| `DOTCHARTER_USE_SYSTEM_JUCE`  | `OFF`   | Use a local JUCE checkout instead of fetching. Requires `JUCE_DIR`. |
| `JUCESHARP_USE_SYSTEM_JUCE`   | `OFF`   | Alias of the above for the standalone build. |
| `JUCE_DIR`                    | —       | Path to the JUCE checkout, used when either switch above is `ON`. |
| `DOTCHARTER_DSP_NEURAL`       | `ON`    | Build the `namamp` neural-capture module (NeuralAudio) into `juce_sharp`. |
| `DOTCHARTER_STEMS_DEMUCS_CPP` | `OFF`   | Build native htdemucs (demucs.cpp) inference into `juce_sharp`. |
| `JUCESHARP_BUILD_NATIVE_TESTS`| `ON`    | Build the native test executables (RT gate and friends). |
| `JUCE_TAG`                    | `9.0.3` | JUCE git tag/branch to fetch. Configure FAILS if the JUCE sources found are a different version (a stale `FETCHCONTENT_SOURCE_DIR_JUCE` is not re-checked-out). |
| `RUBBERBAND_TAG`              | `v4.0.0`| Rubber Band git tag. |
| `NEURALAUDIO_TAG`             | `v0.1.3`| NeuralAudio git sha/tag. |
| `DEMUCS_CPP_TAG`              | `f1206e9adeea103aef4a636b9e62297cf1f8e34e` | demucs.cpp sha to fetch. |
| `JUCESHARP_RID`               | host-derived | .NET RID used for the runtime deploy path. |

CMake warns if `JUCE_TAG` or `RUBBERBAND_TAG` is pointed at a mutable ref
(`main`, `HEAD`, …) — builds stop being reproducible.

## ASIO

Nothing to download: ASIO is on by default on Windows and compiles against the
ASIO headers **JUCE itself bundles** (`modules/juce_audio_devices/native/asio/`,
since JUCE 8.0.15; ASIO SDK 2.3 in JUCE 9.0.3), which Steinberg dual-licenses as proprietary or **GPLv3**.
No Steinberg SDK is vendored here; the `third_party/ASIOSDK2.3/` copy this repo
carried from 2026-05-23 (commit `4993879`) had not been read by the build since
the 8.0.15 bump and was deleted on 2026-09-28. `JUCE_ASIO_USE_EXTERNAL_SDK`
stays at JUCE's default of 0. `EngineSurfaceCoverageTests.Windows_build_registers_the_ASIO_host_type`
fails if a Windows build ever loses the ASIO host type.

## Validating the build

```powershell
dotnet test --project JuceSharp/tests/JuceSharp.Tests/JuceSharp.Tests.csproj
```

`global.json` selects the **Microsoft.Testing.Platform** runner (xunit.v3), so
pass the test project explicitly rather than the solution.

Tests decorated with `[NativeAvailableFact]` / `[NativeAvailableTheory]`
exercise the freshly-built DLL and **skip cleanly when it is missing**; the
rest run regardless and validate the managed surface. Set
`DCA_ENABLE_NULL_DEVICE=1` to let device-dependent tests run against the
engine's headless null device instead of real hardware.

The native gates are separate — see
[Building from Source](../native/building.md#running-native-tests).
