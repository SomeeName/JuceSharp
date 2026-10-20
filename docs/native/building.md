# Building from Source

See [Getting Started › Building the Native DLL](../getting-started/building-native.md)
for the user-facing toolchain instructions and the full CMake option table.

This page covers contributor-relevant detail.

## Repository layout

```
JuceSharp/src/JuceSharp.Native/
├── CMakeLists.txt              top-level build
├── include/
│   ├── dotcharter_audio.h      public C ABI header (DCA_ABI_VERSION)
│   └── dotcharter_vst_bridge.h shared-memory layout for the VST host child
├── src/
│   ├── dotcharter_audio.cpp    the ONE ABI translation unit; also holds JuceHost
│   ├── AudioEngine.cpp/.h      device manager, mixer, transports, master bus
│   ├── LoadResampler.h         rational polyphase FIR, applied once at file load
│   ├── MirrorOutput.cpp/.h     parallel streaming-mirror sink
│   ├── MirrorClockController.h drift control for the mirror
│   ├── RemoteVstBridge.cpp/.h  shared-memory bridge to juce_vst_host.exe
│   ├── RubberBandAudioSource.cpp/.h  stretch / pitch transport source
│   ├── PanningAudioSource.cpp/.h
│   ├── StemSeparator.cpp/.h    native demucs.cpp entry point (opt-in)
│   ├── NullAudioIODevice.h     headless device (DCA_ENABLE_NULL_DEVICE=1)
│   ├── Scrub.h, RtDiag.h, Utf8Truncate.h, LatestSnapshot.h
│   ├── dsp/                    the in-house reorderable tone rig
│   │   ├── DspChain.h  DspModule.h  DspModuleFactory.cpp/.h
│   │   ├── NamAmpModule.cpp/.h  RoutingModules.h  Retirement.h
│   │   ├── IrMailbox.h  IrWavLoader.h  Scrub.h
│   │   └── cores/              63 `dcdsp_*` headers (54 declare a `*Core`)
│   └── host/
│       └── VstHostMain.cpp     the juce_vst_host.exe sandbox child
├── tests/
│   ├── CMakeLists.txt
│   ├── rt/                     rt_main.cpp, rt_sentinel.*, rt_baseline.txt,
│   │                           rt_lock_sites.txt — the allocation/lock gate
│   ├── dsp/                    tone_regressions.cpp, delay_sync_trails.cpp, fixtures/
│   ├── nam/                    nam_lifetime.cpp
│   ├── dualcab_test.cpp
│   └── lane_mixer_test.cpp
├── tools/                      offline measurement programs (tone_response.cpp,
│                               droptune_measure.cpp, droptune_poly.cpp, miku_measure.cpp)
```

There is no `JuceHost.cpp/.h`: `class JuceHost` — the embedded message thread
and GUI initialiser — is declared inside `src/dotcharter_audio.cpp`.

## CMake targets

* `juce_sharp` — `SHARED` library, ABI-versioned via `DCA_ABI_VERSION`.
* `juce_vst_host` — Windows-only executable, the out-of-process VST3 host.

Post-build deploys `juce_sharp.dll` (or `.so` / `.dylib`) and, on Windows,
`juce_vst_host.exe` to `JuceSharp/src/JuceSharp/runtimes/${JUCESHARP_RID}/native/`.
The managed csproj packs that directory so referencing projects pick the files
up automatically.

## Dependency acquisition

All four are fetched with `FetchContent` at configure time and pinned:

| Dependency  | Pin | Notes |
| ----------- | --- | ----- |
| JUCE        | `9.0.3` (`JUCE_TAG`) | A pure port from 8.0.15 (2026-09-28): `JUCE_USE_OPUS=0` and `JUCE_USE_MP3AUDIOFORMAT=0` keep 8.x's format set, and `MidiOutputDevice` restores the Win32 MIDI-out retries 9.x dropped — the full list is in `CMakeLists.txt` above `JUCE_TAG` and in the changelog. Configure fails if the JUCE sources are not the tagged version. Source-only, so `DCA_ABI_VERSION` does not move; the DLL still must be rebuilt. |
| Rubber Band | `v4.0.0` (`RUBBERBAND_TAG`) | GPLv2 or commercial. Linking it makes `juce_sharp.dll` GPLv2 unless you hold a commercial licence. |
| NeuralAudio | `v0.1.3` (`NEURALAUDIO_TAG`) | Only when `DOTCHARTER_DSP_NEURAL=ON` (the default). Pulls NAM Core, RTNeural, Eigen. |
| demucs.cpp  | sha `f1206e9adeea103aef4a636b9e62297cf1f8e34e` (`DEMUCS_CPP_TAG`) | Only when `DOTCHARTER_STEMS_DEMUCS_CPP=ON` (off by default). OpenBLAS / AOCL / MKL are all forced off. |

Override JUCE with `-DDOTCHARTER_USE_SYSTEM_JUCE=ON -DJUCE_DIR=path/to/JUCE`
(`JUCESHARP_USE_SYSTEM_JUCE` is an alias) if you maintain a local checkout.

## ASIO

No SDK is redistributed in this repository — see
[Building the Native DLL › ASIO](../getting-started/building-native.md#asio).
There is no wiring beyond `JUCE_ASIO=1` in `JUCESHARP_CORE_DEFINITIONS`: JUCE
(8.0.15+, 9.0.3 today) bundles the ASIO 2.3 headers (GPLv3 / proprietary dual
licence) and uses them while `JUCE_ASIO_USE_EXTERNAL_SDK` is at its default of
0. The `ASIO_SDK_DIR` probe and the vendored `third_party/ASIOSDK2.3/` were
removed on 2026-09-28, so nothing can downgrade the build to no-ASIO any more:
`JUCE_ASIO=1` is unconditional and `EngineSurfaceCoverageTests` checks the
built DLL registers the host type.

## Adding a new ABI export

1. Add the function to `include/dotcharter_audio.h` with `DCA_API` linkage.
2. Implement it in `src/dotcharter_audio.cpp` — **the one ABI translation
   unit**. Wrap the body in `guard("dca_foo_bar", [&] { … })` so a thrown C++
   exception becomes `DCA_ERR_INTERNAL` plus a logged line.
3. Increment `DCA_ABI_VERSION` in the header. Do this even for a purely
   additive export: it is how a managed caller knows the symbol is there to
   resolve at all.
4. Add the matching P/Invoke to
   `JuceSharp/src/JuceSharp/Interop/NativeMethods.cs` and bump
   `ExpectedAbiVersion`.
5. Expose it via a managed method on `JuceAudioEngine` / `JuceAudioSource` /
   `JuceAudioPlugin`.
6. Add tests in `JuceSharp.Tests`.
7. Rebuild and redeploy the DLL, then confirm the symbol is really in it:
   `dumpbin /EXPORTS juce_sharp.dll | findstr dca_foo_bar`.

## Running native tests

`JUCESHARP_BUILD_NATIVE_TESTS` defaults to **ON**, so a normal configure
already produces the gates. There is no `juce_sharp_tests` target.

| Target | CTest name | Gates |
|---|---|---|
| `juce_sharp_rtcheck` | `rt_alloc_gate` | allocations and locks on the real device callback |
| `juce_sharp_tone_regressions` | `tone_regressions` | tone-rig core regressions |
| `juce_sharp_delay_sync_trails` | `delay_sync_trails` | delay tempo-sync and bypass trails |
| `juce_sharp_dualcab_test` | `dualcab` | dual cabinet IR slots |
| `juce_sharp_lane_mixer_test` | `lane_mixer` | split/merge lane mixing |
| `juce_sharp_amp_circuits_test` | `amp_circuits` | the `amp` circuit voicings' tone stack against its sources, Bright / Resonance, `bassamp`'s mid sweep and Ultra Lo / Ultra Hi (2026-10) |
| `juce_sharp_nam_lifetime` | `juce_sharp_nam_lifetime` | namamp model retirement (needs `DOTCHARTER_DSP_NEURAL=ON`) |

```powershell
cmake --build build --config Release --target juce_sharp_rtcheck
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

The DSP soak inside the RT gate defaults to ~2 s and honours
`JUCESHARP_SOAK_SECONDS`.

## Logging knobs (environment)

| Variable                       | Effect                                                |
| ------------------------------ | ----------------------------------------------------- |
| `JUCESHARP_LOG_LEVEL`          | Managed-side filter (`trace` … `error`).              |
| `JUCESHARP_CONSOLE_LOG`        | Native stderr fallback (`trace` … `error`, or `1`).   |
| `JUCESHARP_FORCE_CONSOLE_LOG`  | Force stderr fallback even with a managed callback.   |
| `DCA_ENABLE_NULL_DEVICE=1`     | Expose the headless null audio device, for tests on a box with no sound card. |
| `JUCESHARP_SOAK_SECONDS`       | Length of the DSP soak inside `juce_sharp_rtcheck`.   |
