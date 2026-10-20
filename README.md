# JuceSharp

📖 **Docs:** the GitBook/HonKit source lives under [`docs/`](./docs/) — start at
[`docs/README.md`](./docs/README.md) and [`docs/SUMMARY.md`](./docs/SUMMARY.md).
No site is published today; render locally with HonKit if you want the browsable
version (`cd docs && npm install && npx honkit serve`).

A reusable .NET wrapper around a JUCE-based native audio engine
(`juce_sharp.dll`). Extracted from the
[DotCharter](https://github.com/SomeeName/DotCharter) project so any
.NET application can have:

> **Windows x64 only today.** `juce_sharp.dll` has never been built or tested on
> macOS or Linux — CMake carries `APPLE`/`UNIX` branches and JUCE itself is
> portable, but nothing above it has been exercised there, and no package is
> published to any feed. Treat those platforms as unbuilt, not "supported".

* Low-latency device I/O: WASAPI · DirectSound · ASIO (the CoreAudio / ALSA /
  JACK backends are JUCE's and are reachable in principle — see the caveat above)
* File decoding & transport with [Rubber Band](https://breakfastquay.com/rubberband/)
  time-stretch and pitch-shift
* VST3 plug-in hosting on both an input bus (live guitar / mic) and an
  internal MIDI VSTi — in-process, or sandboxed in a child process
* An in-house reorderable DSP tone rig (107 header-only `dcdsp_*` files, 97 of
  which declare a `*Core`; 100 catalogue module types in a default build) ahead of the
  input VST chain, with up to four independent per-player "seats"
* A 6-band master parametric EQ, sample-accurate click scheduling and native
  dropout/xrun counters
* Streaming-mirror output (a second parallel WASAPI sink for OBS / Discord)
* One-shot peak-meter waveform pre-render, offline decode and offline render
* Managed analysis DSP (tuner, onset, polyphonic pitch, CQT, chord/beat) and
  three stem-separation backends

## Layout

```
JuceSharp/
├── JuceSharp.sln              standalone solution
├── docs/                      GitBook/HonKit documentation source
├── examples/                  six runnable sample projects
├── src/
│   ├── JuceSharp/                    managed C# wrapper (PackageId: JuceSharp)
│   ├── JuceSharp.Native/             native C++ DLL (CMake, JUCE-based)
│   ├── JuceSharp.Dsp/                managed analysis DSP + Detection v8
│   ├── JuceSharp.AudioIo/            decode / WAV / AudioBuffer helpers
│   ├── JuceSharp.StemSeparation/     Python-sidecar stem separation
│   ├── JuceSharp.StemSeparation.Onnx/in-process ONNX inference
│   └── JuceSharp.Diagnostics/        `juce-stems-diag` CLI
└── tests/
    └── JuceSharp.Tests/       xunit.v3 suite (Microsoft.Testing.Platform)
```

## Quick start

```csharp
using JuceSharp;

using var engine = new JuceAudioEngine(new JuceEngineConfig
{
    PreferredDriver = AudioDriver.Wasapi,
    SampleRate      = 48000,
    BufferFrames    = 256,
});
engine.Start();

using var song = engine.OpenFile(@"C:\Music\track.wav");
song.Volume     = 1.0f;
song.TempoRatio = 0.9f;
song.Play();
```

## Building the native DLL

Everything the binaries are built from is either in this repository or
fetched by CMake `FetchContent` at a pinned tag (JUCE `9.0.3`, Rubber Band
`v4.0.0`, NeuralAudio `v0.1.3`, and demucs.cpp at a pinned commit when that
option is on), so a clean checkout plus the toolchain below rebuilds them.

You need **Visual Studio 2022 or later** (Build Tools are enough) with the
"Desktop development with C++" workload, **CMake ≥ 3.22**, and **Git** (for
FetchContent; the first configure downloads the dependencies). The project is
built and released with **Visual Studio Build Tools 2026**, generator
`"Visual Studio 18 2026"`; the CMake those Build Tools bundle
(`Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) works
when `cmake` is not on `PATH`.

```powershell
cd src/JuceSharp.Native
# Pass the generator for the toolset you HAVE; one for a VS you do not have
# fails at configure time. "Visual Studio 17 2022" for VS 2022.
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --target juce_sharp juce_vst_host
```

The full option list, the Linux/macOS commands (unbuilt so far) and the native
test gates are in
[`docs/getting-started/building-native.md`](./docs/getting-started/building-native.md);
`AGENTS.md` § *Native rebuild* has the maintainer's exact invocation.

The post-build step deploys `juce_sharp.dll` (and `juce_vst_host.exe`) into
`src/JuceSharp/runtimes/win-x64/native/`, which the managed csproj packs
as a content file and into the nupkg under `runtimes/win-x64/native/`. Any project
referencing `JuceSharp` gets them copied to its output automatically. **The DLL
is a build artifact and is not tracked** — build it before `dotnet pack` —
rebuild it after any ABI bump or the engine constructor throws.

### ASIO

Nothing to download: ASIO is on by default on Windows and compiles against the
ASIO headers **JUCE itself bundles** (`modules/juce_audio_devices/native/asio/`,
since JUCE 8.0.15; ASIO SDK 2.3 in JUCE 9.0.3), which Steinberg dual-licenses as proprietary or **GPLv3**.
No Steinberg SDK is vendored here. `JUCE_ASIO_USE_EXTERNAL_SDK`
stays at JUCE's default of 0. `EngineSurfaceCoverageTests.Windows_build_registers_the_ASIO_host_type`
fails if a Windows build ever loses the ASIO host type.

### Optional: use a local JUCE checkout

```powershell
cmake -S . -B build -DJUCESHARP_USE_SYSTEM_JUCE=ON -DJUCE_DIR=C:/path/to/JUCE
```

## Building & testing

```powershell
dotnet build JuceSharp.sln -c Release
dotnet test --project tests/JuceSharp.Tests/JuceSharp.Tests.csproj
```

`global.json` selects the **Microsoft.Testing.Platform** runner (xunit.v3
4.0.0), so point `dotnet test` at the test project rather than the solution.

`JuceSharp.Tests` holds roughly a thousand test methods (count them with `[Fact]`, `[Theory]`, `[NativeAvailableFact]`,
`[NativeAvailableTheory]` and `[OnnxResourceFact]` — the `NativeAvailable*` pair
is ~a quarter of the suite and is skipped without the native DLL), covering,
broadly:

* the managed surface of every wrapper type (engine, source, plugin, mixer,
  loop region, MIDI input, config, exception, log);
* **interop pins** — `NativeMethodsTests` (P/Invoke shape, enum ordinals,
  struct layout, UTF-8 marshalling), `Utf8InteropTests`, `Utf8TruncationTests`,
  `NativeCallbackSafetyTests`, `FinalizerSafetyTests`,
  `NativeHandleDrainTests`;
* **source lints** over the native tree — `NativeRtSourceLintTests`,
  `NativeCrashRaceSourceLintTests`, `WrapperLevelGateLintTests`;
* the DSP rig driven offline through the ABI (`Dsp/Dsp*Tests`, gain-zipper,
  bypass crossfade, split chains, string params, soak);
* the null device (`NullDeviceTests`, `DCA_ENABLE_NULL_DEVICE=1`);
* AudioIo, ONNX stem separation, the Python sidecar, the `juce-stems-diag` CLI;
* the CQT / Detection v8 evaluations (opt-in — they need `GND_IDMT_ROOT`).

Tests requiring the native DLL are decorated with
`[NativeAvailableFact]` / `[NativeAvailableTheory]` and skip cleanly
when the DLL is missing (e.g. a box that hasn't built the C++ side).

Native gates are separate and run through CTest — see
[`docs/native/building.md`](./docs/native/building.md#running-native-tests).

Code coverage is collected via Coverlet — see the
`coverlet.msbuild` & `coverlet.collector` packages in
`JuceSharp.Tests.csproj`.

## ABI policy

* `dca_abi_version()` is bumped on every ABI change, additive ones included.
* The managed wrapper checks the ABI version on `JuceAudioEngine`
  construction and throws `JuceAbiMismatchException` (an `InvalidOperationException`
  subclass) on mismatch. Exact
  equality is required — there is no back-version shim.
* All exports are `extern "C"`, non-throwing, POD/handle types only.

## License

**The source is MIT; the native binaries are AGPLv3.**

* The C# wrapper and everything else in this repository's source — the
  managed projects, the native C++ we wrote, examples, tools and tests — is
  under the MIT licence ([`LICENSE`](./LICENSE)).
* Every build of the native library — `juce_sharp.dll` and the sandboxed VST3
  host `juce_vst_host.exe` — statically links **JUCE 9**, which is licensed
  under the AGPLv3 or a commercial JUCE licence (JUCE 9.0.3's `LICENSE.md`:
  *"The JUCE Framework modules are dual-licensed under the AGPLv3 and the
  commercial JUCE licence"*); `juce_sharp.dll` also links **Rubber Band**
  (GPLv2 or later, or commercial) and the **ASIO SDK** headers JUCE bundles
  (GPLv3 or Steinberg's proprietary licence). So each such build, and every
  NuGet package or application that carries one, is distributed under the
  **AGPLv3** together with those GPL terms. JuceSharp holds no commercial
  licence for any of them.
* If you distribute the binaries, offer their corresponding source: this
  repository at the commit you built plus the dependencies CMake fetches at
  the tags pinned in `src/JuceSharp.Native/CMakeLists.txt`.
  [Building the native DLL](#building-the-native-dll) is the whole recipe.
* `JuceSharp.Dsp` carries no native code and is plain MIT.

The statement is at the end of [`LICENSE`](./LICENSE) (after the unchanged MIT
text); every component, with its version, copyright line and licence, is in
[`THIRD-PARTY-NOTICES.md`](./THIRD-PARTY-NOTICES.md). This is not legal advice.
