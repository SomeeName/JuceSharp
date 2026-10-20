# JuceSharp Examples

Six sample projects covering the JuceSharp public surface (wrapper **0.9**,
native **ABI v35**). They double as smoke tests we can run locally against real
audio. All six are in `JuceSharp.sln` under the **examples** solution folder, so
opening the solution in Rider / Visual Studio lets you Run / Debug them
directly. Each console example is a single `Program.cs` written to be read top
to bottom, and every one of them answers `--help`.

Argument conventions are the same everywhere: a missing or malformed argument
prints the help and exits **2**, a missing/unreadable native DLL or an ABI
mismatch exits **3**, and "no audio device could be opened" exits **4**.

## Running headless: `DCA_ENABLE_NULL_DEVICE=1`

Three of the six need an *engine*. The engine registers a silent, self-paced
**"Null"** `AudioIODeviceType` — no sound card, nothing audible, the callback
paced by a timer — and it registers it **only when the environment variable
`DCA_ENABLE_NULL_DEVICE` is set to `1`**. The variable is read once at engine
start, so setting it in the process before the first engine call is enough.

* `Playback --null` and `ToneRig --null` set it for you and then ask for the
  `"Null"` output type by name.
* `ToneRig --offline` sets it unconditionally: an offline render needs an
  engine but must **not** have a device open (`ProcessOffline` refuses one —
  its chunk loop would contend the live callback thousands of times).
* `Decode`, `Spectrogram` and `AnalyzeChords` never open a device at all, so
  they need nothing.

```powershell
$env:DCA_ENABLE_NULL_DEVICE = '1'   # or just pass --null
```

---

## `JuceSharp.Examples.Decode`

`JuceSharp.AudioIo` with no engine, no device and no DSP chain. It probes the
header without decoding, decodes to interleaved float32, prints a per-channel
peak/RMS, resamples-and-downmixes in one native call (`DecodeAs`), and then
writes the first second three times through **`WavWriter`** — float32, 24-bit
and 16-bit PCM — reading each one back and measuring the round-trip error
against the half-LSB the quantiser is allowed. It also shows `IsUnfinished`,
which answers "did anything ever patch this header?".

```powershell
dotnet run --project examples\JuceSharp.Examples.Decode -- song.flac out.wav
```

Expected output: a `Probe:` line, a `Decode:` line with a realtime multiple in
the hundreds or thousands, one line per channel, `DecodeAs(22050 Hz, mono)`,
then a three-row table of formats with byte counts, round-trip errors and
`unfinished=False`. A round-trip error of exactly `0` at a given depth means the
source was already on that grid — a 16-bit WAV written back as PCM16 is
lossless, which is the point of the closing note.

## `JuceSharp.Examples.Playback`

End-to-end engine playback, held through the **`IJuceAudioEngine`** interface so
it reads the way testable host code should. It enumerates every host and device
without opening anything (`JuceDeviceCatalog`), opens the engine with the
device-walk fallback, reports that the **master limiter is OFF at engine start**
(ABI v35 — the host opts in; `--limiter` opts in and reads the setting back
*post-clamp*), prints `OutputLatencySamples` next to the limiter's share of it,
plays the file with an A-B **`JuceLoopRegion`**, moves `TempoRatio` and
`PitchSemitones` mid-playback, and finishes with the callback's three dropout
counters. A `JuceAbiMismatchException` is caught and explained rather than
thrown at the user.

```powershell
dotnet run --project examples\JuceSharp.Examples.Playback -- song.wav 10 --device "Focusrite"
dotnet run --project examples\JuceSharp.Examples.Playback -- song.wav 4 --null --limiter   # headless
```

Expected output: the device catalogue, `Engine: 48000 Hz, 480 frames, …`, the
limiter lines, `Output latency: N samples total, of which M are the limiter's
look-ahead`, a `t=…s position=… audible=… tempo=… pitch=… limiterGR=…` line
every 500 ms, then `Dropouts: callbacks=… late=… …`. On the Null device
`late` is routinely non-zero — it is a timer-paced fake device, not a driver.

## `JuceSharp.Examples.ToneRig`

The engine's in-house reorderable tone rig — a seat's guitar-**input** chain —
reached with `engine.GetDspChain(seat)`. It builds a six-pedal preset inside
**one `JuceDspEditScope`** (so the board changes as a single crossfade instead
of once per pedal), inserts a delay **parked** at zero wet and fades it in with
every parameter already in place, keys the compressor's detector off the seat's
dry input (**sidechain**, gated on the catalogue's own capability flag —
setting a key on a module that would ignore it throws, deliberately), and prints
the chain using the **engine's** node-type and bypass getters rather than a
managed memory of what this process created.

`--offline` additionally renders the file twice: a static render written as
24-bit PCM, and the **automated** render (four `JuceDspAutomationEvent`s,
including the reserved `"bypass"` param id used as a footswitch) written as
float32 — both through `WavWriter` — followed by a branch-latency read-back
across a parallel `splitter ∥ merger` region with `BranchLatencyCompensation`
on and off.

```powershell
dotnet run --project examples\JuceSharp.Examples.ToneRig -- --catalog                  # every module, by category
dotnet run --project examples\JuceSharp.Examples.ToneRig -- --offline di.wav reamp.wav # no sound card
dotnet run --project examples\JuceSharp.Examples.ToneRig -- --seconds 8                # live input
dotnet run --project examples\JuceSharp.Examples.ToneRig -- --seconds 2 --null         # headless, silent
```

Expected output: `57 module types`, `Preset built inside one edit scope
(IsEditing=True, 6 nodes in the shadow)` followed by `Committed
(IsEditing=False)`, the parked-delay line, a seven-row chain table with
`key=SeatInput` on the compressor, and then either a meter line every 250 ms
(live) or `Static render: …` / `Automated render: 4 events …` / `Parallel region
(amp ∥ harmonizer +7): …` (offline). `--offline` writes `out.wav`,
`out.auto.wav` and warns when the rendered peak exceeds 0 dBFS — the offline
path has no master limiter, so that file clips.

## `JuceSharp.Examples.Spectrogram`

Offline time/frequency analysis, pure managed. Decode → `JuceSpectrogramBuilder`
(the Hann-windowed STFT over **`JuceFft`**, which replaced MathNet in the 0.9
wave) → a **PGM image** you can open in anything. One frame is also computed
with `JuceFft.Magnitude` directly, so it is visible what the builder is doing,
and `JuceWaveformPeaks` builds the min/max buckets a waveform view needs. The
image is a PGM on purpose: eight lines and no dependency, where a PNG would mean
pulling an imaging package into an example whose subject is the DSP.

```powershell
dotnet run --project examples\JuceSharp.Examples.Spectrogram -- song.wav
dotnet run --project examples\JuceSharp.Examples.Spectrogram -- song.wav --fft 2048 --range 60 --image spec.pgm --csv spec.csv
```

Expected output: `STFT: 1204 frames × 512 bins (dB), 21.5 Hz/bin, 23.2
ms/frame`, the direct-`JuceFft` line, a `peak … dB at … s, … Hz`, the waveform
bucket count, and `Wrote …pgm: 1204×512 8-bit PGM`. Low frequencies are at the
bottom of the image.

## `JuceSharp.Examples.AnalyzeChords`

Musical analysis, pure managed, and the example that exercises the most of the
0.9 additions. `JuceBeatDetector` now returns a **tempo map** and **downbeats**
alongside the BPM and the tracked beats (before 0.9 `Beats` was a fixed grid and
every beat past a tempo change drifted), plus a **confidence** — which exists
because the detector used to fabricate a whole plausible analysis for silence
with no way to tell, so a low value means "this audio may not have a beat" and
never "error". Onsets go through `JuceOnsetDetector`'s allocation-free **span**
overload with `RequiredOnsetCapacity` sizing the buffer. Chords come from
`JuceChromagram` + `JuceChordDetector` over one-second hops. Pitch goes through
**`JuceTunerStream`** — the streaming, hysteresis-filtered wrapper a live tuner
needs — pushed with the decoded file, which is the same code path a live input
takes.

```powershell
dotnet run --project examples\JuceSharp.Examples.AnalyzeChords -- song.wav
```

Expected output: `Beats: 149.6 BPM global, confidence 0.04, 59 tracked beats`,
a tempo map with one segment for a steady song, `meter: 4/4, 15 downbeats`, an
`Onsets:` block, one chord line per second, then a `Tuner:` block listing only
the note *changes*.

## `JuceSharp.Examples.Avalonia`

An Avalonia 12 desktop UI exercising the engine, transport, mixer, biquad
filters, A-B loop, live spectrum + tuner, offline analyses, VST3 hosting, a
**Tone Rig** tab (catalogue dropdown, add / add-parked / bypass / move / remove,
per-node meters, per-param sliders that honour value labels and steps, chain
latency) and a dropout-counter readout in the status bar. It is the most
complete end-to-end demo of the managed surface and a good starting point for
wiring JuceSharp into your own desktop app.

Two things here are worth copying rather than the rest: file open and plugin
load go through **`OpenFileAsync` / `LoadPluginAsync`** (0.9), because both take
seconds — a cold VST3 scan measures 25 s and more — and the synchronous calls
hold the engine handle on the *calling* thread for all of it, which from a click
handler is a window that stops repainting. The async twins run on one dedicated
long-running thread per engine, never the thread pool. And the click handlers
are plain `void` handing their `Task` to a `Forget()` helper, not `async void`:
an `async void` that throws is raised on the synchronisation context and kills
the process.

```powershell
dotnet run --project examples\JuceSharp.Examples.Avalonia
```

Expected output: a window. It does **not** open a device until you press
*Start engine*, so it launches fine on a machine with no sound card.

---

## The tone rig goes through the wrapper

Both rig examples used to bind `dca_engine_dsp_*` themselves, through a shared
`examples/JuceSharp.Examples.ToneRig/DspInterop.cs`, because JuceSharp exposed
none of the rig. That file is **gone**: the package now ships
`JuceDspModuleCatalog` (the static 57-module palette, no engine required),
`JuceAudioEngine.GetDspChain(seat)` → `JuceDspChain`, and `JuceDspNode` for one
pedal. See [docs/api/juce-dsp-chain.md](../docs/api/juce-dsp-chain.md).

Two things changed for the examples beyond the shorter code. Every rig call now
pins the engine handle, so the Avalonia demo's 100 ms meter timer landing during
an engine swap is a caught `ObjectDisposedException` instead of a read through a
raw `NativeHandle` nobody was holding alive. And `PrintChain` no longer has to
identify a pedal by probing for a parameter only that module carries.

Since **ABI v33** that last part is the ENGINE's answer rather than a managed
memory of what this process created: `JuceDspNode.TypeId` and
`JuceDspNode.Bypass` read `dca_engine_dsp_get_node_type` /
`dca_engine_dsp_get_bypass`, and `JuceDspModuleInfo.Category` — which was
permanently empty before, because nothing exported one — carries one word out of
a fixed set the engine owns. Both examples print it: the console `--catalog`
groups by it, and the Avalonia tab's palette dropdown and node list show it.

## Release builds — `examples/publish/`

`examples/publish/` is **git-ignored**. Publish each example
(framework-dependent, win-x64, so the folders stay small) with:

```powershell
foreach ($p in 'Decode','Playback','ToneRig','AnalyzeChords','Spectrogram','Avalonia') {
    $out = "examples\publish\JuceSharp.Examples.$p"
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }   # so you test a clean set
    dotnet publish "examples\JuceSharp.Examples.$p\JuceSharp.Examples.$p.csproj" `
        -c Release -r win-x64 --self-contained false -o $out
}
```

Layout of one published folder:

```
examples/publish/JuceSharp.Examples.ToneRig/
├── JuceSharp.Examples.ToneRig.exe        the apphost you run
├── JuceSharp.Examples.ToneRig.dll        …and its managed entry point
├── JuceSharp.dll, JuceSharp.AudioIo.dll, JuceSharp.Dsp.dll, …
├── juce_sharp.dll                        the native engine, AT THE ROOT
└── juce_vst_host.exe                     the out-of-process VST3 host child
```

Both native artifacts land **at the root of the publish folder**, not under
`runtimes/win-x64/native/` — a RID-specific publish flattens the runtime assets.
`juce_sharp.dll` is a **build artifact and is not tracked**: it is deployed to
`src/JuceSharp/runtimes/win-x64/native/` by CMake's POST_BUILD step, and the
`JuceSharp.csproj` copies it from there. If it is missing, build the native
target (`JuceSharp/AGENTS.md` § Native rebuild) and republish.

```powershell
examples\publish\JuceSharp.Examples.Decode\JuceSharp.Examples.Decode.exe song.flac out.wav
examples\publish\JuceSharp.Examples.Playback\JuceSharp.Examples.Playback.exe song.wav 10
examples\publish\JuceSharp.Examples.Playback\JuceSharp.Examples.Playback.exe song.wav 4 --null --limiter
examples\publish\JuceSharp.Examples.ToneRig\JuceSharp.Examples.ToneRig.exe --catalog
examples\publish\JuceSharp.Examples.ToneRig\JuceSharp.Examples.ToneRig.exe --offline di.wav reamp.wav
examples\publish\JuceSharp.Examples.AnalyzeChords\JuceSharp.Examples.AnalyzeChords.exe song.wav
examples\publish\JuceSharp.Examples.Spectrogram\JuceSharp.Examples.Spectrogram.exe song.wav --image spec.pgm
examples\publish\JuceSharp.Examples.Avalonia\JuceSharp.Examples.Avalonia.exe
```

The Avalonia project's `EnsureNativeJuceSharp` target rebuilds `juce_sharp.dll`
before a build when a native source is newer than the deployed DLL. It looks for
CMake in the Visual Studio 2026 Build Tools, then CLion's bundled copy, then
`PATH` (none of them is on `PATH` by default on Windows), and uses the
`"Visual Studio 18 2026"` generator; override with `-p:CMakeExe=...` /
`-p:CMakeGenerator=...`, or skip it with `-p:SkipNativeJuceSharp=true`.
