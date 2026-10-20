# Examples

Six runnable projects live in `examples/`, all registered in `JuceSharp.sln`
under an **examples** solution folder, so an IDE can Run / Debug them
directly. Each console example is a single `Program.cs` written to be read top
to bottom.

`examples/README.md` in the repository carries the full command lines,
including publishing; this page is the map.

| Project | What it demonstrates |
|---|---|
| `JuceSharp.Examples.Decode` | `JuceSharp.AudioIo` with **no engine**: `JuceAudioDecoder.Probe` / `Decode` / `DecodeAs`, `AudioBuffer.SplitChannels` / `ToMono`, and a float32 WAV round trip through `WavIo`. |
| `JuceSharp.Examples.Playback` | End-to-end engine playback: `JuceDeviceCatalog.Enumerate`, `JuceEngineFactory.OpenWithFallback`, `OpenFile` + transport, `Position` vs `AudiblePosition`, `TempoRatio` / `PitchSemitones` / `Seek` while playing, `TryGetXrunStats`, and a clean `Stop` + `Dispose`. |
| `JuceSharp.Examples.ToneRig` | The DSP tone rig (ABI v21 onwards; v35 today): the module catalogue with named and ordinal params, a gate → overdrive → amp → cab → reverb chain on live input, a delay inserted **parked** and faded in, per-node meters, node and chain latency, bypass crossfades, remove and clear. |
| `JuceSharp.Examples.AnalyzeChords` | Decode → `JuceBeatDetector` → `JuceChromagram` + `JuceChordDetector` over one-second hops → `JuceChromaticTuner` on the loudest window. |
| `JuceSharp.Examples.Spectrogram` | Decode → `JuceSpectrogramBuilder` + `JuceWaveformPeaks` + streaming `JuceOnsetDetector`, with optional CSV export. |
| `JuceSharp.Examples.Avalonia` | An Avalonia 12 desktop UI over the whole managed surface: device picking with in-place host swap, transport, mixer, biquads, A-B loop, live spectrum and tuner, VST3 hosting, a Tone Rig tab, and a dropout readout in the status bar. |

## Running them

```powershell
dotnet run --project examples\JuceSharp.Examples.Decode -- song.flac out.wav
dotnet run --project examples\JuceSharp.Examples.Playback -- song.wav 10 --device "Focusrite"
dotnet run --project examples\JuceSharp.Examples.AnalyzeChords -- song.wav
dotnet run --project examples\JuceSharp.Examples.Spectrogram -- song.wav out.csv
dotnet run --project examples\JuceSharp.Examples.ToneRig  -- --catalog
dotnet run --project examples\JuceSharp.Examples.Avalonia
```

Two of them run **without a sound card**:

```powershell
dotnet run --project examples\JuceSharp.Examples.Playback -- song.wav 5 --null
dotnet run --project examples\JuceSharp.Examples.ToneRig  -- --offline di.wav reamped.wav
```

`--null` (and the equivalent `--device null`) uses the engine's headless null device, and the ToneRig
example's `--offline` mode renders a file through the rig with no device open
at all. `--catalog` prints every module and its parameters.

## Where to start

* **New to the library?** `Playback`. It is the shortest complete path from
  "no engine" to "audio is coming out", and it uses the fallback opener, which
  is what you want in a real app.
* **Wiring JuceSharp into a desktop app?** `Avalonia`. It is the most complete
  end-to-end demo of the managed surface.
* **Only need offline analysis?** `Decode`, then `AnalyzeChords` or
  `Spectrogram`. Neither `Decode` nor the analysis pair opens an audio device.

## The tone rig in the examples

`ToneRig` and the Avalonia Tone Rig tab drive the chain through the managed
`JuceDspChain` / `JuceDspModuleCatalog` API (`engine.GetDspChain(seat)`); no
P/Invoke shim is needed any more. See [The DSP Tone Rig](tone-rig.md) and
[JuceDspChain](../api/juce-dsp-chain.md).

## Publishing

`examples/publish/` is git-ignored. Each published folder gets the example's
`.exe`, the managed assemblies, and `juce_sharp.dll` + `juce_vst_host.exe`
from `runtimes/win-x64/native/` — which CMake fills in, so build the native
side first. The Avalonia project's `EnsureNativeJuceSharp` MSBuild target will
rebuild the DLL for you when a native source is newer than the deployed one;
skip it with `-p:SkipNativeJuceSharp=true`.
