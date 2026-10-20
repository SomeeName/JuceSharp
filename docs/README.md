---
description: A low-latency .NET wrapper around a JUCE-based native audio engine.
---

# Welcome

**JuceSharp** is a .NET 10+ library that wraps a JUCE-based native audio engine
(`juce_sharp.dll`) and exposes it to managed code through a small, stable,
strongly-typed API. It was extracted from
[DotCharter](https://github.com/SomeeName/DotCharter) so any .NET application —
desktop, console, MAUI, server — can stream audio at whatever latency the
driver allows (ASIO buffers of 64–128 frames are routine), host VST3 plugins,
and process MIDI without ever touching the .NET GC on the real-time path.
Nothing here publishes a measured round-trip figure: it is the device's, not
the library's.

> ✅ **Tip**
> **Why a native engine?** Real-time audio cannot tolerate GC pauses. JuceSharp
> pushes all sample-rate work into unmanaged C++ behind a stable C ABI, and
> restricts the managed side to control-rate calls (load, start, set gain,
> seek, …). The audio thread never enters the CLR.

## Feature highlights

| Feature                          | Status | Notes                                                |
| -------------------------------- | :----: | ---------------------------------------------------- |
| Cross-platform device I/O        |   ✅   | WASAPI · DirectSound · ASIO · CoreAudio · ALSA · JACK |
| File playback + transport        |   ✅   | WAV, AIFF, FLAC, OGG, MP3 (via JUCE codecs), from disk or memory |
| Time-stretch & pitch-shift       |   ✅   | [Rubber Band 4.0](https://breakfastquay.com/rubberband/) baked in, plus a shared stem-group stretcher |
| In-house DSP tone rig            |   ✅   | Reorderable chain of header-only cores ahead of the input VST chain, with parallel branches and crossfaded structure changes |
| Per-player seats                 |   ✅   | Up to four independent live-input rigs on one open device |
| VST3 plugin hosting              |   ✅   | Input bus (FX) **and** internal instrument routing; optionally sandboxed in a child process |
| Plugin editor windows            |   ✅   | Hosted on JuceSharp's own message thread (no UI requirement) |
| MIDI **input** and **output** + short-message send |  ✅   | Lock-free dispatch to the instrument plugin, sample-accurate since ABI v34; hardware MIDI OUT through one high-priority dispatcher thread per device |
| Master parametric EQ             |   ✅   | Six typed bands, a route, a trim and a spectrum tap  |
| Streaming mirror output          |   ✅   | Second parallel WASAPI sink (OBS, Discord, …)        |
| One-shot waveform pre-render     |   ✅   | Per-block peak amplitudes, from a path or a byte blob |
| Click track                      |   ✅   | Synthesised beat / downbeat / clap, schedulable sample-accurately on the render clock |
| Dropout / xrun counters          |   ✅   | Measured in the audio callback, not inferred from a poll |
| Analysis DSP                     |   ✅   | Tuner, onset, polyphonic pitch, CQT, chord, beat, spectrogram |
| Stem separation                  |   ✅   | Three backends: in-process ONNX, native demucs.cpp, Python sidecar |
| Logger sink (ILogger)            |   ✅   | Plus a console fallback for "zero-config" use        |
| Cross-platform build (CMake)     |   🟡   | Only win-x64 has ever been built or tested; the Linux/macOS branches are source-available |

## Layout

```
JuceSharp/
├── JuceSharp.sln              standalone solution
├── docs/                      ← you are here (GitBook/HonKit source)
├── examples/                  six runnable sample projects
├── src/
│   ├── JuceSharp/                     managed wrapper (PackageId: JuceSharp)
│   ├── JuceSharp.Native/              native C++ DLL (CMake + JUCE)
│   ├── JuceSharp.Dsp/                 managed analysis DSP + Detection v8
│   ├── JuceSharp.AudioIo/             decode / WAV / AudioBuffer
│   ├── JuceSharp.StemSeparation/      Python-sidecar separation + ckpt→ONNX
│   ├── JuceSharp.StemSeparation.Onnx/ in-process ONNX inference
│   └── JuceSharp.Diagnostics/         the juce-stems-diag CLI
└── tests/
    └── JuceSharp.Tests/       xunit.v3 (Microsoft.Testing.Platform) + coverlet
```

## When to use JuceSharp

✅ You need **deterministic, low-latency audio** from .NET.
✅ You want to **host VST3 plugins** from a managed application.
✅ You want **transport, time-stretch & pitch-shift** out of the box.
✅ You're building **music tooling**, charting, looping, transcription, etc.

❌ You only need to play a notification sound — use System.Media instead.
❌ You need bit-perfect mastering DAW guarantees — use JUCE directly.

## Next steps

* [Install the NuGet / project reference](getting-started/installation.md)
* [Try the 30-second Quick Start](getting-started/quick-start.md)
* [Read the architecture overview](native/architecture.md)

New to the library and starting from an empty project?
[First sound](getting-started/first-sound.md) is the shortest true path from a
clone to audible playback, and it hands off to
[Threading](guides/threading.md),
[Failure modes & runtime health](guides/failure-modes.md) and
[Real-time constraints](guides/realtime-constraints.md) — the three contracts a
consumer has to honour once sound is coming out.

> ⚠️ **Warning**
> **License notice.** The C# source is MIT; the native binaries are not.
> `juce_sharp.dll` statically links **JUCE 9 (AGPLv3 or
> commercial)**, Rubber Band (GPLv2 or later, or commercial), and the Steinberg VST 3 SDK
> (MIT) and ASIO SDK headers (GPLv3 or proprietary) that ship inside JUCE. Optional
> CMake switches pull in more. Open-source distribution under the matching
> copyleft terms is free; **closed-source commercial distribution requires
> paid licences from the respective vendors.** Every build of `juce_sharp.dll`
> and `juce_vst_host.exe` is distributed under the AGPLv3 with those GPL terms;
> see the statement at the end of the repository's `LICENSE` and the full list
> in `THIRD-PARTY-NOTICES.md` — read them before you redistribute, and take
> your own legal advice.
