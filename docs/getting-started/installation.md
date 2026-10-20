# Installation

JuceSharp targets **.NET 10** and is **win-x64 only today**. No pre-built binary
is published anywhere: `juce_sharp.dll` is a build artifact you produce with
CMake, and the repository tracks nothing under `src/JuceSharp/runtimes/` except
a `.gitignore` and a `README.txt`. macOS and Linux have **never been built or
tested** — CMake carries some `APPLE`/`UNIX` branches and CI has a
`continue-on-error` Linux job that only measures the distance to a Linux build,
so treat those platforms as unbuilt rather than supported. The JUCE layer
underneath is portable in principle, which is why the branches exist; nothing
above it has been exercised there.

## Option 1 — ProjectReference (recommended while iterating)

**Step 1: build the native DLL.** There is no route that skips this. Follow
[Building the Native DLL](building-native.md); CMake's POST_BUILD step deploys
`juce_sharp.dll` and `juce_vst_host.exe` into `src/JuceSharp/runtimes/win-x64/native/`.
Skip it and your app compiles perfectly cleanly and throws
`DllNotFoundException` at the first engine call.

**Step 2: reference the managed project.**

```xml
<ItemGroup>
  <ProjectReference Include="..\JuceSharp\src\JuceSharp\JuceSharp.csproj" />
</ItemGroup>
```

Once step 1 has run, MSBuild copies the contents of `runtimes/win-x64/native/`
into your output directory automatically.

**The siblings are separate packages.** `JuceSharp.Dsp` (offline analysis DSP),
`JuceSharp.AudioIo` (`JuceAudioDecoder`, `WavIo`, `WavWriter`, `AudioBuffer`),
`JuceSharp.StemSeparation` and `JuceSharp.StemSeparation.Onnx` are NOT pulled in
by the core package — `JuceSharp.csproj` references none of them. Add a
`ProjectReference` (or, later, a `PackageReference`) per sibling you use, or the
samples in the guides will not compile.

## Option 2 — NuGet (planned)

Once the ABI is stable, JuceSharp will publish to nuget.org:

```bash
dotnet add package JuceSharp
```

> ℹ️ **Note**
> The **packaging** is done and linted, and CI packs the six packages to
> `artifacts/` on every build. The **publishing** is not: there is no
> `dotnet nuget push` anywhere, so `dotnet add package JuceSharp` cannot resolve
> today. Option 1 is the only working route.

## Runtime requirements

| Requirement | Notes |
| --- | --- |
| Windows x64, Windows 10 1809 or later | The only platform that has ever been built. Drivers: WASAPI (default), DirectSound, ASIO. |
| **.NET 10 Runtime** | On the target machine — or publish self-contained. |
| **Visual C++ 2015–2022 x64 Redistributable** | `juce_sharp.dll` and `juce_vst_host.exe` link the DYNAMIC CRT (`MSVCP140.dll`, `VCRUNTIME140*.dll`, `api-ms-win-crt-*`). Without it the DLL fails to load and you get `DllNotFoundException` — which looks exactly like a missing DLL, so check this before you go hunting for the file. |

macOS and Linux are **not** in this table on purpose; see the note at the top of
this page.

## Verifying the install

```csharp
using JuceSharp;

Console.WriteLine($"Native version: {JuceAudioEngine.NativeVersion}");
```

If you see `JuceSharp.Native 1.0.0 (ABI 47, JUCE 9.0.3)` you're good to go. The
string is generated from the CMake project version and `DCA_ABI_VERSION`, so the
numbers here move with the tree — treat the SHAPE as the contract and check the
ABI number against `include/dotcharter_audio.h`, not against this line. Use
`dca_abi_version()` (checked for you in `JuceAudioEngine`'s constructor) if you
need to reason about compatibility; the version string is for logs only.

## Troubleshooting the first run

| Symptom | Cause |
| --- | --- |
| `DllNotFoundException` | Either the native DLL is not next to your executable (you skipped step 1, or the copy did not happen — check your build output and [Building the Native DLL](building-native.md)), **or** the DLL is there and the VC++ redistributable above is missing. Both surface as the same exception. |
| `JuceAbiMismatchException` at `new JuceAudioEngine(...)` | A **stale** `juce_sharp.dll`: the managed wrapper's expected `DCA_ABI_VERSION` and the DLL's disagree. This is the likeliest first-run failure for a source consumer — rebuild the native side, do not chase the managed code. The check runs in the constructor, before anything opens a device. |

## Where to go next

[First sound](first-sound.md) walks the whole path from this reference to audio
coming out of a device — the native build, where the DLL has to sit, the ABI
check, device selection and fallback, the Null device for CI, and disposal
order — and then hands off to [Threading](../guides/threading.md),
[Failure modes & runtime health](../guides/failure-modes.md) and
[Real-time constraints](../guides/realtime-constraints.md).
