# JuceSharp

Reusable managed (.NET) wrapper over `JuceSharp.Native` — a JUCE-based C++
audio engine DLL exporting a flat C ABI. Originally extracted from
[DotCharter](https://github.com/SomeeName/DotCharter); now usable as a
standalone audio library by any .NET application that needs:

> **Windows x64 only today.** `juce_sharp.dll` has never been built or tested on
> macOS or Linux; treat those platforms as unbuilt, not supported. The
> `../../docs/...` links below are repository-relative — on a package page they
> will not resolve; read them in the repo.

* low-latency device I/O (WASAPI / DirectSound / ASIO; the CoreAudio / ALSA /
  JACK backends are JUCE's and unexercised here)
* file decoding + transport (with Rubber Band time-stretch / pitch-shift)
* VST3 hosting (effects on input bus + internal MIDI VSTi)
* a streaming mirror output (parallel WASAPI for OBS / Discord capture)
* peak-meter waveform pre-render

## Public surface

| Type                | Purpose                                            |
| ------------------- | -------------------------------------------------- |
| `JuceAudioEngine`   | Owns the device + master graph. One per app. Sync surface plus an `*Async` half (`ReconfigureAsync`, `SetDeviceTypeAsync`, `ShowAsioControlPanelAsync`, `OpenFileAsync`, `LoadPluginAsync`) that runs on ONE dedicated worker thread per engine, never the pool. |
| `JuceAudioSource`   | A single song/stem source bound to the engine.    |
| `JuceAudioPlugin`   | A hosted VST3: attach to the input or instrument graph, editor, state, parameters. |
| `JuceMidiInput`     | A hardware MIDI input; `Action<JuceMidiMessage>` or the allocation-free `JuceMidiSpanHandler`. |
| `JuceMidiOutput`    | A hardware MIDI output (ABI v34). One high-priority dispatcher thread per device, never the caller's; `SendAt(data, renderFrame)` fires when that frame reaches the EAR. See [`docs/api/juce-midi-output.md`](../../docs/api/juce-midi-output.md). |
| `JuceLoopRegion`    | A loop bound to one source. Owns no handle of its own. |
| `JuceChartNote` / `JuceChartBendPoint` | The chart source's upload (ABI v38): one note / one bend point, laid out as `dca_chart_note` / `dca_chart_bend_point`. Upload with `engine.SetChartNotes(seat, notes, bends)`; the engine plays them through that seat's rig against the song clock (`engine.SetSongClockSource(source)`), after its capture ring. `JuceChartNoteFlags`, `JuceChartVoice`, `JuceChartSourceMode`, `JuceChartSourceStatus` go with it. |
| `JuceEngineConfig`  | Device, driver, sample rate, buffer config.        |
| `JuceAudioDevice`   | One row from `EnumerateInput/OutputDevices`.       |
| `JuceAudioException`| Thrown on native error; carries the status code.   |
| `JuceAbiMismatchException` | The engine refused a `juce_sharp.dll` whose `DCA_ABI_VERSION` is not EXACTLY `NativeMethods.ExpectedAbiVersion`. Rebuild the native side. |
| `AudioDriver`       | Enum: Default / Wasapi / DirectSound / Asio / ...  |

The tone rig's managed face (`JuceSharp.Dsp` namespace, same package):

| Type | Purpose |
| ---- | ------- |
| `JuceDspChain` | One seat's reorderable pedalboard — `engine.GetDspChain(seat)`. Add / remove / move / bypass nodes, read meters and chain latency, render offline, and `BranchLatencyCompensation`. |
| `JuceDspNode` | One pedal: `TypeId`, `Bypass`, `Branch`, float and string params, `LatencySamples`, `Meter`, `Sidechain`. |
| `JuceDspEditScope` | `using var edit = chain.BeginEdit();` — a whole preset publishes as ONE change instead of one per node. Dispose without `Commit()` aborts. |
| `JuceDspModuleCatalog` | The engine-owned module catalogue: id, display name, category, capability flags. |
| `JuceDspMeters` | The raw-handle poll façade over the ABI-v30 per-node meters. Not obsolete — DotCharter still calls it. |

And the seven test seams, so host code can be unit-tested without
`juce_sharp.dll` or a sound card — every wrapper here is `sealed` with an
`internal` constructor, which is what made that impossible before:
`IJuceAudioEngine`, `IJuceAudioSource`, `IJuceAudioPlugin`, `IJuceMidiInput`,
`IJuceMidiOutput`, `IJuceDspChain`, `IJuceDspNode`. See
[`docs/guides/testing-without-native.md`](../../docs/guides/testing-without-native.md).
Every member on them is `<inheritdoc>` onto its concrete counterpart, and
`Wave2DocsInterfaceDocTests` keeps it that way.

Sibling packages carry the rest: `JuceSharp.Dsp` (analysis — `JuceTunerStream`,
`JuceBeatDetector` + `TempoMap`, `JuceGuitarAnalyzer`, the Detection v8 CQT),
`JuceSharp.AudioIo` (`WavIo`, the streaming `WavWriter`),
`JuceSharp.StemSeparation` and `.StemSeparation.Onnx`.
`CoalescingProgressStream` lives here and turns a `report`-callback API into an
`IAsyncEnumerable<T>`.

## Lifetime contract

**SIX types own a `NativeHandleOwner`: `JuceAudioEngine`, `JuceAudioSource`,
`JuceAudioPlugin`, `JuceMidiInput`, `JuceMidiOutput` and (ABI v42)
`JuceWemDecoder`.** Each pins its native handle for the duration of every
P/Invoke, and `Dispose` **drains** the in-flight calls before destroying the
handle. `JuceWemDecoder` belongs to no engine: it is a standalone `.wem` decoder
(`docs/api/juce-wem-decoder.md`).

* `JuceDspChain` and `JuceDspNode` own **no handle at all** — they are views on
  the engine, and every call goes through the engine's own
  `Acquire()` / `TryAcquire()`, so the engine's drain already covers them.
* The two MIDI wrappers held a raw `nint` closed with one `Interlocked.Exchange`
  and had **no drain at all** until the wave-3 fix wave. The OUTPUT one was the
  urgent half: it makes live native calls and `dca_midi_output_close` is a bare
  `delete` that joins the dispatcher thread and destroys the `juce::MidiOutput`,
  so a sequencer flushing note-offs while the UI tore the engine down — the
  ORDINARY shutdown path — was a native use-after-free. Leaving the INPUT one is
  what let the output one be written that way, so both were fixed together.

Completion of the native destroy is a **separate** mechanism from claiming a
handle, and it is the one the 0.9 wave introduced:
`Interop/NativeDestruction` + `INativeChild`. Each child carries a destruction
token (unclaimed → destroying → destroyed, or LEAKED when the native destroy
failed); `Run` executes the destroy exactly once under a gate and `Wait` blocks
until it completes. **The engine keeps the token even after the child wrapper
has been collected**, which is what lets teardown wait on a destruction whose
wrapper no longer exists — a `WeakReference` alone cannot. A failed destroy
retains its callback owner rather than freeing it, because a leaked native
object may still invoke its registered delegate. `JuceAudioSource`,
`JuceAudioPlugin`, `JuceMidiInput` and `JuceMidiOutput` all implement
`INativeChild`; pinned by
`NativeDestructionTests`.

Two properties make the drain a real barrier rather than a hope:

* **The drain never frees under a live call.** It waits on the last `Release`
  (no spin), and if a native call outlives the drain deadline the handle is
  **leaked and logged at Error**, never destroyed. A leak beats a
  use-after-free — and the calls that legitimately run long are the ones a user
  reaches: `TryShowAsioControlPanel` is a modal vendor dialog with no bound at
  all, `Reconfigure` reopens the driver, and a cold VST3 scan of a large amp sim
  measures 25 s+. The previous ceiling was 2 s, after which it returned the
  handle anyway.
* **`JuceAudioEngine.Dispose` drains its CHILDREN first.** A drain is
  PER-HANDLE, so the engine's own counter says nothing about a `dca_source_*`
  call in flight. The engine keeps a weak registry of every
  source, plugin and MIDI input it hands out, claims the engine handle (which
  closes the registry — no new child can be opened past that point), stops the
  device, disposes every child, and only then calls `dca_engine_destroy`.
  A `JuceLoopRegion` is reached through its owning `JuceAudioSource`.
  Before this, a 4 ms position poll inside
  `dca_source_get_audible_position_seconds` could be running while
  `dca_engine_destroy` reset the transport under it.

So a poll racing a dispose surfaces as `ObjectDisposedException` on the *next*
call, never a native crash. Pinned by `NativeHandleDrainTests` and
`EngineChildDrainTests`.

**Any managed delegate handed to native needs a catch-all body.** A reverse
P/Invoke that throws unwinds through C++ frames — a process kill, not a
catchable error. `NativeCallbackSafetyTests.EveryDelegateHandedToNative_HasACatchAllBody`
discovers the `[UnmanagedFunctionPointer]` types from `NativeMethods.cs` and
lints every assignment and factory for one.

## Native DLL deployment

The native build (`JuceSharp.Native`) drops `juce_sharp.dll` into
`runtimes/win-x64/native/`. This csproj packs that as a content file,
so any project referencing `JuceSharp` gets the DLL copied to its
output automatically.

If you see `DllNotFoundException: juce_sharp`, build the native project
first (see `../JuceSharp.Native/README.md`).

### `juce_vst_host.exe` lands in TWO different places

The sandboxed VST3 host child ships in the same item as the DLL and stays
**co-located with it** on both delivery routes — but the two routes put the pair
in different folders, and no code in this package resolves the path for you
(`dca_engine_set_remote_bridge` takes shared-memory and event names only, so
launching the child is the consumer's job):

| Route | Where both binaries land |
|---|---|
| `PackageReference` (NuGet) | `<output>/runtimes/win-x64/native/` — the RID-asset folder; `juce_sharp.dll` itself is resolved from there automatically through `deps.json`, the `.exe` is not |
| `ProjectReference` (this repo, and DotCharter) | flattened **beside your executable**, in `AppContext.BaseDirectory` |

So the obvious `Path.Combine(AppContext.BaseDirectory, "juce_vst_host.exe")` —
which is what working in a ProjectReference tree teaches — silently finds
nothing on the NuGet route and degrades to in-process hosting. Probe both:

```csharp
static string? FindVstHost()
{
    var b = AppContext.BaseDirectory;
    foreach (var p in new[] { Path.Combine(b, "juce_vst_host.exe"),
                              Path.Combine(b, "runtimes", "win-x64", "native", "juce_vst_host.exe") })
        if (File.Exists(p)) return p;
    return null;
}
```

## Tests

See [`tests/JuceSharp.Tests/`](../../tests/JuceSharp.Tests/) for the
xUnit test suite covering the managed wrapper, P/Invoke bindings,
config marshalling, exception types, device enumeration, engine
lifecycle, sources, plugins and waveform generation.

## Licence — read this before you redistribute

**The MIT grant covers this wrapper's C# source only. The package also ships
`juce_sharp.dll` and `juce_vst_host.exe`, and those binaries are not MIT: every
build of them, and this package because it carries them, is distributed under
the AGPLv3 (JUCE 9) together with the GPL terms of Rubber Band and the ASIO SDK.**

`juce_sharp.dll` statically links (`juce_vst_host.exe` links JUCE alone):

* **JUCE 9** (tag `9.0.3`) — **AGPLv3 or a commercial JUCE licence** (JUCE 9 EULA).
* **Rubber Band Library** (tag `v4.0.0`) — **GPLv2 or later, or a commercial
  licence.** Linking it puts the resulting binary under the GPL unless you hold one.
* **Steinberg VST 3 SDK 3.8.0** — bundled inside JUCE, for VST3 hosting. **MIT.**
* **Steinberg ASIO SDK 2.3** headers — bundled inside JUCE, Windows-only. GPLv3 or
  Steinberg's proprietary ASIO licence.
* **NeuralAudio** (MIT) + **NAM Core** (MIT) + **RTNeural** (BSD-3-Clause) +
  **Eigen** (MPL-2.0), by default (`-DDOTCHARTER_DSP_NEURAL=ON`).
* **demucs.cpp** (MIT, vendoring Eigen) when
  `-DDOTCHARTER_STEMS_DEMUCS_CPP=ON`; off by default.

`JuceSharp.StemSeparation.Onnx` additionally depends on
`Microsoft.ML.OnnxRuntime.DirectML` (MIT) and redistributes `DirectML.dll` from
`Microsoft.AI.DirectML`, which is under **Microsoft's own redistributable licence**.

Practically: **distributing a product built on this package requires either
complying with the AGPLv3 (with Rubber Band under GPLv2 or later, taken as
v3) for your whole application, or holding commercial JUCE, Rubber Band and
ASIO licences.** `JuceSharp.Dsp` is the one package here that
carries no native code and is genuinely MIT on its own.

The statement and the component list are in the `LICENSE` file shipped inside
this package; the copyright lines, licence texts and the runtime downloads that
are *not* redistributed are in `THIRD-PARTY-NOTICES.md`, shipped beside it. It is informational and **not legal advice** — verify the current
terms with each vendor.
