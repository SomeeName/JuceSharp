# JuceSharp.Native

Native C++ audio engine behind [JuceSharp](../JuceSharp/README.md), built on
[JUCE](https://juce.com). Exports a flat, versioned C ABI
(`include/dotcharter_audio.h`, `DCA_ABI_VERSION`) consumed by the managed
`JuceSharp` wrapper via P/Invoke.

The header name is historical — this engine was extracted from DotCharter, and
the ABI symbols keep their `dca_` prefix so existing callers did not have to be
rewritten. The build product is **`juce_sharp.dll`**.

## Why JUCE

* Mature, battle-tested audio device abstraction (WASAPI / DirectSound /
  ASIO / CoreAudio / ALSA / JACK) in one tree.
* First-class VST3 hosting (`juce_audio_processors`) including editor
  windowing on Win32 / macOS / X11.
* Built-in audio formats and DSP primitives — no need to glue PortAudio +
  libsndfile + Rubber Band + the Steinberg SDK by hand.

## Building (Windows, MSVC)

```powershell
cd src/JuceSharp.Native
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Use the generator that matches the toolset you actually have — **VS 2022 or
later**. On the maintainer's machine the only installed toolchain is Visual
Studio Build Tools 2026, so the generator there is `"Visual Studio 18 2026"`,
and neither `cmake` nor the VS tools are on `PATH` (CMake is CLion's bundled
copy). See `AGENTS.md` § *Native rebuild* for the exact invocation used here.

The build requires **C++17** (`CMAKE_CXX_STANDARD 17`) and CMake ≥ 3.22. The
project declares `LANGUAGES C CXX`: JUCE 8.0.15+ hard-errors without a C
compiler even though no C of our own is compiled.

The post-build step deploys `juce_sharp.dll` — and, on Windows, the
out-of-process VST host child `juce_vst_host.exe` — into
`../JuceSharp/runtimes/<rid>/native/` (`win-x64` here), which the managed
csproj packs as content so `dotnet build` picks it up.

**The DLL is a build artifact and is not tracked.** An ABI bump means
rebuilding it before any managed code runs, or engine construction throws on
the version check.

## Layout

```
src/JuceSharp.Native/
├── CMakeLists.txt              top-level build
├── include/
│   ├── dotcharter_audio.h      public C ABI header (DCA_ABI_VERSION)
│   └── dotcharter_vst_bridge.h shared-memory layout for the VST host child
├── src/
│   ├── dotcharter_audio.cpp    the ONE ABI translation unit (also holds JuceHost)
│   ├── AudioEngine.cpp/.h      device manager, mixer, transports, master bus
│   ├── LoadResampler.h         rational polyphase FIR used once at file load
│   ├── MirrorOutput.cpp/.h     parallel streaming-mirror sink
│   ├── RemoteVstBridge.cpp/.h  shared-memory bridge to juce_vst_host.exe
│   ├── RubberBandAudioSource.cpp/.h  stretch/pitch transport source
│   ├── StemSeparator.cpp/.h    native demucs.cpp entry (opt-in)
│   ├── NullAudioIODevice.h     headless device for tests
│   ├── PanningAudioSource.*, MirrorClockController.h, LatestSnapshot.h,
│   ├── RtDiag.h, Scrub.h helpers, Utf8Truncate.h
│   ├── dsp/                    the in-house reorderable tone rig
│   │   ├── DspChain.h, DspModule.h, DspModuleFactory.cpp/.h
│   │   ├── NamAmpModule.cpp/.h, RoutingModules.h, Retirement.h,
│   │   ├── IrMailbox.h, IrWavLoader.h, Scrub.h
│   │   └── cores/              63 `dcdsp_*` headers (54 declare a `*Core`)
│   └── host/
│       └── VstHostMain.cpp     the juce_vst_host.exe sandbox child
├── tests/                      native gates (see below)
└── tools/                      offline measurement programs
```

## ASIO

Nothing to download: ASIO is on by default on Windows and compiles against the
ASIO headers **JUCE itself bundles** (`modules/juce_audio_devices/native/asio/`,
since JUCE 8.0.15; ASIO SDK 2.3 in JUCE 9.0.3), which Steinberg dual-licenses as proprietary or **GPLv3**.
No Steinberg SDK is vendored here; the `third_party/ASIOSDK2.3/` copy this repo
carried from 2026-05-23 (commit `4993879`) had not been read by the build since
the 8.0.15 bump and was deleted on 2026-09-28. `JUCE_ASIO_USE_EXTERNAL_SDK`
stays at JUCE's default of 0. `EngineSurfaceCoverageTests.Windows_build_registers_the_ASIO_host_type`
fails if a Windows build ever loses the ASIO host type.

## Optional: use a local JUCE checkout

```powershell
cmake -S . -B build -DDOTCHARTER_USE_SYSTEM_JUCE=ON -DJUCE_DIR=C:/path/to/JUCE
```

`JUCESHARP_USE_SYSTEM_JUCE` is an alias of the same switch for the standalone
build.

## Native tests

`JUCESHARP_BUILD_NATIVE_TESTS` is **ON** by default and adds six executables,
each registered with CTest:

| Target | CTest name | What it gates |
|---|---|---|
| `juce_sharp_rtcheck` | `rt_alloc_gate` | allocations and locks on the real device callback |
| `juce_sharp_tone_regressions` | `tone_regressions` | tone-rig core regressions |
| `juce_sharp_delay_sync_trails` | `delay_sync_trails` | delay tempo-sync + bypass trails |
| `juce_sharp_dualcab_test` | `dualcab` | dual cabinet IR slots |
| `juce_sharp_lane_mixer_test` | `lane_mixer` | split/merge lane mixing |
| `juce_sharp_nam_lifetime` | `juce_sharp_nam_lifetime` | namamp model retirement (only when `DOTCHARTER_DSP_NEURAL` is ON) |

```powershell
cmake --build build --config Release --target juce_sharp_rtcheck
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

## ABI policy

* `dca_abi_version()` is bumped on every backwards-incompatible change, and in
  practice on additive ones too — a managed caller needs to know the new
  symbols are there to resolve at all.
* The managed wrapper checks the ABI version in `JuceAudioEngine`'s
  constructor and throws `InvalidOperationException` on mismatch.
* All exports are `extern "C"`, non-throwing, POD/handle types only.
* There is exactly **one** ABI translation unit, `src/dotcharter_audio.cpp`.
  After adding an export, `dumpbin /EXPORTS` is the only proof it is in the
  DLL.
