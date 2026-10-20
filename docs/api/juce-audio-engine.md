# JuceAudioEngine

`sealed class JuceAudioEngine : IDisposable`

The main entry point. Owns the native audio device, the output graph, and
every source, plugin and MIDI input it hands out.

## Construction

```csharp
new JuceAudioEngine(JuceEngineConfig? config = null, ILogger<JuceAudioEngine>? logger = null)
```

* `config` — see [`JuceEngineConfig`](juce-engine-config.md). `null` uses
  driver defaults.
* `logger` — optional `ILogger`. If `null` and
  `config.EnableConsoleLog` is `true` (default), JuceSharp wires
  [`JuceConsoleLogger`](juce-log.md#juceconsolelogger).

Throws:

* `ArgumentOutOfRangeException` — invalid `config`.
* `JuceAbiMismatchException` (subclass of `InvalidOperationException`) — managed/native ABI version mismatch. Exact
  equality is required; the message tells you to rebuild the DLL.
* `JuceAudioException` — native engine creation failed.

For a device that may be offline or absent, prefer
[`JuceEngineFactory.OpenWithFallback`](juce-engine-factory.md).

---

## Static members

| Member | Description |
| --- | --- |
| `string NativeVersion` | The native engine's version string. For logs; it lags the package version. |
| `int MaxPlayers` | `DCA_MAX_PLAYERS` (4). |
| `int PlayerNodeIdStride` | `DCA_PLAYER_NODE_ID_STRIDE` (1,000,000). |
| `const int EqBandCount = 6` | Bands in the master EQ bank. |
| `IReadOnlyList<JuceAudioDevice> EnumerateOutputDevices()` | Output devices across every driver, no engine needed. |
| `IReadOnlyList<JuceAudioDevice> EnumerateInputDevices()` | Input devices, same. |
| `IReadOnlyList<string> EnumerateMidiInputs()` | MIDI input device names (forwards to `JuceMidiInput.Enumerate`). |
| `float[] GenerateWaveform(string path, double blockMs)` | Peak-per-block waveform from a file. |
| `float[] GenerateWaveformFromMemory(byte[] data, double blockMs)` | The same from an in-memory blob (ABI v20). |
| `JuceDeviceRateConversion ConvertAudioForDevice(string sourcePath, string destinationWavPath, int deviceSampleRate)` | Write the song at a device rate with the preload's own upsampler and quantiser, so opening it plays bit-identical samples without the per-open conversion (ABI v43). `NotNeeded` writes nothing; `Pcm16` is the 16-bit WAV written, with the preload step in its `dcps` chunk. Slow: never on a UI or audio thread. |
| `void RenderToWav(string srcPath, string dstPath, float tempo = 1f, float semitones = 0f, int sampleRate = 0)` | Faster-than-realtime decode to a 16-bit PCM WAV (ABI v4). |
| `void ProcessDropTuneOffline(ReadOnlySpan<float> input, Span<float> output, int sampleRate, ReadOnlySpan<float> semitonesPerSegment, ReadOnlySpan<int> segmentLengths, float maxLagMs)` | MONO offline CSVD drop tuner (`dca_droptune_process_offline`) — engine-less, in-place allowed, DOWN-ONLY. The segments run through ONE DropTune instance, so an engage/disengage lands mid-stream against a primed ring exactly as under a footswitch. |
| `void ProcessDropTuneOffline(…, JuceDropTuneMode mode = Balanced)` | The same with the lag ceiling by name: `Tight` 10 ms, `Balanced` 20 ms, `Smooth` 35 ms. |

> ⚠️ **Warning**
> `RenderToWav` throws **`NotSupportedException`** for any `tempo` other than
> `0f`/`1f` or any non-zero `semitones`. That path is a decode plus a WAV
> write; it has no Rubber Band transport.

## Instance — lifecycle & device

| Member | Description |
| --- | --- |
| `nint NativeHandle` | Raw engine handle, or `nint.Zero` once disposed. |
| `void Start()` | Open the device and start streaming. |
| `void Stop()` | Stop the device. **Does not** make a later `Start()` resume playback — sources are detached from the mixer for good. |
| `void Dispose()` | Claim the handle, stop, dispose every child, destroy the engine. `IDisposable` only. |
| `int SampleRate` | Active rate in Hz. |
| `int BufferFrames` | Active block size in frames. |
| `int OutputLatencySamples` | Device + driver output latency, **plus the master limiter's look-ahead** (ABI v7, v34); 0 with no device open. |
| `void Reconfigure(int sampleRate, int bufferFrames, string? outputDeviceId, string? inputDeviceId)` | In-place device reconfigure (ABI v8). |
| `void SetDeviceType(string? typeName, string? outputDeviceId = null, string? inputDeviceId = null, uint outputChannelMask = 0, uint inputChannelMask = 0)` | In-place host swap (ABI v12). |
| `IReadOnlyList<JuceAudioDevice> EnumerateOutputDevicesLive()` | Devices visible to the live engine. |
| `IReadOnlyList<JuceAudioDevice> EnumerateInputDevicesLive()` | Same, inputs. |
| `bool TryShowAsioControlPanel(nint parentWindow, out string? error)` | Pop the vendor's ASIO panel. **Modal and unbounded** — do not call it from a thread you need back. |

## Instance — master bus & routing

| Member | Description |
| --- | --- |
| `void SetMasterGain(float gain)` | Master output gain (linear, ramped). |
| `void SetMonitorGain(float gain)` | Dry input monitor gain (linear, ramped). |
| `void SetInputGain(float gain)` | Pre-chain input trim (linear, ramped). |
| `void SetHighPass(float freqHz, float q)` | Master high-pass; `freq <= 0` disables. |
| `void SetLowPass(float freqHz, float q)` | Master low-pass. |
| `void SetBandPass(float freqHz, float q)` | Master band-pass. |
| `void SetInstrumentGain(float gain)` | Instrument-bus gain. |
| `void SetMasterLimiter(bool enabled, float ceilingDb = -0.3f, float releaseMs = 80f)` | Master-bus ceiling (ABI v34). **OFF at engine start since ABI v35 — the host opts in** (v34 had it ON, which cost every player 1 ms of latency on their own monitored playing). Ranges are clamped on both sides; a non-finite value throws. A no-op on a disposed engine, like every other master-bus setter. |
| `(bool, float, float) GetMasterLimiter()` | What the engine holds, post-clamp. |
| `float MasterLimiterReductionDb` | Poll-only decaying hold of the deepest gain reduction, in POSITIVE dB. 0 when idle or off. Returns 0 rather than throwing on a disposed engine. |
| `int MasterLimiterLatencySamples` | The limiter's own share of `OutputLatencySamples`. **0 while it is off** — switched off it is not in the path. |
| `bool SetMirrorDevice(string? deviceId)` | Open / close the streaming mirror. `null` or `""` disables. |
| `bool SetMirrorOutput(string? deviceId)` | **`[Obsolete]`** alias of `SetMirrorDevice`. Under `TreatWarningsAsErrors` (this repo's default, and a common one) calling it fails the build — use `SetMirrorDevice`. |
| `float GetAndResetInputPeak()` | Read-and-reset the input peak. **Linear**, not dB. |

## Instance — parametric EQ (ABI v29)

Six bands, all off at engine start.

| Member | Description |
| --- | --- |
| `void SetEqBand(int index, JuceEqBandType type, float freqHz, float q, float gainDb, bool enabled)` | **Prefer this one.** Configure one band. `JuceEqBandType` is `HighPass` / `LowShelf` / `Bell` / `HighShelf` / `LowPass` / `Notch` (0–5). |
| `void SetEqBand(int index, int type, float freqHz, float q, float gainDb, bool enabled)` | The raw-ordinal form of the same call. Both overloads throw `ArgumentOutOfRangeException` for an `index` outside `[0, EqBandCount)` — it used to be a log line only. |
| `void SetEqEnabled(bool enabled)` | Master enable for the bank. |
| `void SetEqSolo(int bandIndex)` | Solo one band. |
| `void SetEqRoute(JuceEqRoute route)` | **Prefer this one.** Which bus the bank sits on: `Song` (default) / `Input` / `Both`. |
| `void SetEqRoute(int route)` | The raw-ordinal form. |
| `void SetEqTrim(float trimDb)` | Post-EQ make-up trim. |
| `int GetEqActiveCount()` | How many bands are actually **audible**. |
| `int ReadEqSpectrum(float[] destDb)` | Sampled magnitude response in dB, for a UI curve. `destDb.Length` must be in `[MinEqSpectrumBins, MaxEqSpectrumBins]` (8…512) or it throws; 0 now means only "the engine is gone". |

## Instance — clicks & render clock (ABI v26)

| Member | Description |
| --- | --- |
| `void PlayClick(JuceClickType clickType)` | **Prefer this one.** Fire a click now: `Beat` / `Downbeat` / `Clap` (0–2). |
| `void PlayClick(int clickType)` | The raw-ordinal form. |
| `bool TryGetRenderClock(out long renderFrames, out long hostTicks)` | The output render clock at a block boundary. |
| `void ScheduleClick(JuceClickType clickType, long renderFrame)` | **Prefer this one.** Queue a click at an absolute render frame. |
| `void ScheduleClick(int clickType, long renderFrame)` | The raw-ordinal form. |
| `void ClearScheduledClicks()` | Drop the queue — do this on seek, pause and speed change. |

## Instance — health (ABI v30)

| Member | Description |
| --- | --- |
| `bool TryGetXrunStats(out XrunStats stats)` | Snapshot the callback's own dropout counters. |
| `bool ResetXrunStats()` | Zero them. |
| `bool TryGetCallbackLoad(out CallbackLoad load)` | The callback's load as a fraction of the buffer period (ABI v46): an average (0.5 s EMA) and a peak decaying over 1.5 s that reading does not reset. Both 0 while no device runs; not touched by `ResetXrunStats`. |

## Instance — live input capture (ABI v11 / v15 / v16)

| Member | Description |
| --- | --- |
| `void SetInputCaptureChannel(int channel)` | Which channel feeds the mono ring; `-1` mixes all. |
| `int ReadInputCapture(Span<float> dest)` | Latest window. Must **not** be drain-polled. |
| `int ReadInputCaptureStream(Span<float> dest)` | Only samples not yet consumed (own cursor). |
| `int ReadInputCaptureStream(Span<float> dest, out ulong streamPosition)` | …plus the absolute stream position. |
| `bool TryGetInputCaptureSync(out ulong capturedSamples, out long hostTicks)` | Atomic {captured, host clock} sample. |

## Instance — seats (ABI v28)

| Member | Description |
| --- | --- |
| `void SetPlayerCount(int count)` | How many seats are live. Defaults to 1; at 1 the callback runs the pre-v28 path byte-for-byte. |
| `int GetPlayerCount()` | Current seat count. |
| `void SetPlayerInputChannel(int player, int channel)` | Hardware input channel for a seat. |
| `int GetPlayerInputChannel(int player)` | Read it back. |
| `void SetPlayerMonitorGain(int player, float linearGain)` | Per-seat monitor gain. |
| `float GetAndResetPlayerInputPeak(int player)` | Per-seat input peak (linear). |
| `int ReadInputCapture(int player, Span<float> dest)` | Per-seat windowed capture read. |
| `int ReadInputCaptureStream(int player, Span<float> dest)` | Per-seat streaming read. |
| `int ReadInputCaptureStream(int player, Span<float> dest, out ulong streamPosition)` | …with position. |
| `bool TryGetInputCaptureSync(int player, out ulong capturedSamples, out long hostTicks)` | Per-seat clock sync. |

## Instance — drop tune (ABI v17 / v18 / v25 / v31)

| Member | Description |
| --- | --- |
| `void SetInputPitchShift(float semitones)` | Seat 0's drop tune. **Down-only.** |
| `void SetInputPitchShiftMode(JuceDropTuneMode mode)` | **Prefer this one.** Seat 0's splice-lag ceiling: `Tight` (10 ms) / `Balanced` (20 ms, the engine default) / `Smooth` (35 ms). |
| `void SetInputPitchShiftMode(int mode)` | The raw-ordinal form. |
| `void SetInputPitchShift(int player, float semitones)` | Per seat (ABI v31). |
| `void SetInputPitchShiftMode(int player, JuceDropTuneMode mode)` | Per seat, typed (ABI v31). |
| `void SetInputPitchShiftMode(int player, int mode)` | Per seat, raw ordinal (ABI v31). |

## Instance — sources

| Member | Description |
| --- | --- |
| `JuceAudioSource OpenFile(string path)` | Open a decodable file. |
| `unsafe JuceAudioSource OpenMemory(ReadOnlySpan<byte> bytes, string? formatHint = null)` | Open an in-memory blob. |
| `void SetStretchQuality(bool faster)` | `false` = R3 (Finer), `true` = R2 (Faster). Only affects sources opened **after** the call. |
| `float StemGroupTempo { get; set; }` | Tempo on the shared stem-group stretcher. |
| `float StemGroupPitchSemitones { get; set; }` | Pitch on the shared stem-group stretcher. |

## Instance — plugins & MIDI

| Member | Description |
| --- | --- |
| `JuceAudioPlugin LoadPlugin(string path)` | The safe wrapper. Throws on failure. |
| `JuceAudioPlugin LoadPlugin(string path, int componentIndex)` | The same, reaching one component of a multi-component VST3 bundle (ABI v33). An index the bundle does not have is rejected, never clamped. |
| `static int CountVst3Components(string path)` | How many components that bundle declares (ABI v33) — the index domain for the overload above. |
| `void SendMidi(byte status, byte data1, byte data2)` | Lock-free short message to the instrument bus. Stamped at sample 0 of the block that drains it. |
| `void SendMidi(byte status, byte data1, byte data2, long renderFrame)` | The same, at an EXACT output frame (ABI v34). Frames are the `TryGetRenderClock` domain; negative = ASAP. |
| `bool TryGetMidiTimingStats(out MidiTimingStats)` | Poll-only `Scheduled` / `Late` / `Dropped`, shaped like `TryGetXrunStats`: `false` means the engine is gone, which a zeroed tuple could not say. |
| `(long, long, long) GetMidiTimingStats()` | The tuple form. Reports `(0,0,0)` for BOTH "disposed" and "nothing scheduled" — prefer the `Try` overload. |
| `bool ResetMidiTimingStats()` | Zero them; `false` when the engine is gone. |
| `JuceMidiInput OpenMidiInput(int deviceIndex, Action<JuceMidiMessage> handler)` | Open a hardware MIDI input (ABI v4). The handler runs on the JUCE MIDI thread. |
| `JuceMidiInput OpenMidiInput(int deviceIndex, JuceMidiSpanHandler handler)` | The allocation-free form. `JuceMidiSpanHandler` is `(double timestampSeconds, ReadOnlySpan<byte> data)`; the span is valid only for the call. |
| `IReadOnlyList<string> EnumerateMidiOutputs()` | MIDI OUTPUT device names (ABI v34). |
| `JuceMidiOutput OpenMidiOutput(int deviceIndex)` | Open a hardware MIDI output (ABI v34). Rooted by the engine, same contract as the input. The concrete member returns `JuceMidiOutput`; `IJuceAudioEngine.OpenMidiOutput` returns it as `IJuceMidiOutput`, which is what makes it fakeable in a test. See [`juce-midi-output.md`](juce-midi-output.md). |
| `bool SetRemoteBridge(string? shmName, string? requestEventName, string? responseEventName, bool enable)` | Attach / detach the sandboxed VST host bridge (ABI v19). |
| `bool TryGetRemoteBridgeStats(out uint processed, out uint timeouts, out uint heartbeat)` | Bridge counters. |

## The raw-`nint` plugin members are GONE (0.8.x)

There is no raw-handle plugin surface on the engine any more. The nine members
that used to be here — `LoadVst3`, `UnloadPlugin`, `AttachPluginToInput`,
`DetachPluginFromInput`, `ShowPluginEditor`, `HidePluginEditor`,
`AttachPluginToInstrument`, `DetachPluginFromInstrument` and
`GetPluginLatencySamples` — were **deleted**, not deprecated, and their absence
is pinned by `WrapperLifetimeAuditTests.TheRawNintPluginSurface_IsGone`. They
skipped `JuceAudioPlugin`'s drain entirely, so a poll racing a dispose was a
native crash rather than a caught `ObjectDisposedException`.

`LoadPlugin(string)` hands you a tracked [`JuceAudioPlugin`](juce-audio-plugin.md);
everything the old members did is an instance member on it.

| Was | Now |
| --- | --- |
| `LoadVst3(string)` | `JuceAudioEngine.LoadPlugin(string)` |
| `UnloadPlugin(nint)` | `JuceAudioPlugin.Dispose()` |
| `AttachPluginToInput(nint)` | `JuceAudioPlugin.AttachToInput(int player = 0)` |
| `DetachPluginFromInput(nint)` | `JuceAudioPlugin.DetachFromInput()` |
| `ShowPluginEditor(nint)` | `JuceAudioPlugin.ShowEditor()` |
| `HidePluginEditor(nint)` | `JuceAudioPlugin.HideEditor()` |
| `AttachPluginToInstrument(nint)` | `JuceAudioPlugin.AttachToInstrument()` |
| `DetachPluginFromInstrument(nint)` | `JuceAudioPlugin.DetachFromInstrument()` |
| `GetPluginLatencySamples(nint)` | `JuceAudioPlugin.LatencySamples` |

## The DSP tone rig

`GetDspChain(int player = 0)` returns the seat's `JuceDspChain` — the managed
face of the reorderable in-house rig (ABI v21 onwards): catalogue, add / parked
add / remove / move / bypass / params / string params / branches, per-node meters
and chain latency, and an offline render. See
[JuceDspChain](juce-dsp-chain.md) and [The DSP Tone Rig](../guides/tone-rig.md).
`JuceSharp.Dsp.JuceDspMeters` remains as the older raw-`nint` meter façade.

## Instance — async variants (0.9)

The calls below are the ones that take **seconds**, and every one of them holds
the engine handle on the *calling* thread for the duration. From a UI thread
that is a frozen application, so each has an async twin:

| Async member | Wraps | Why it is on this list |
|---|---|---|
| `ReconfigureAsync(sampleRate, bufferFrames, outputDeviceId, inputDeviceId, ct)` | `Reconfigure` | reopens the driver |
| `SetDeviceTypeAsync(typeName, outputDeviceId, inputDeviceId, outputChannelMask, inputChannelMask, ct)` | `SetDeviceType` | in-place host swap: close + reopen |
| `ShowAsioControlPanelAsync(parentWindow, ct)` → `JuceAsioPanelResult` | `TryShowAsioControlPanel` | a **modal vendor dialog**, unbounded |
| `OpenFileAsync(path, ct)` | `OpenFile` | decode + resample, per stem |
| `LoadPluginAsync(path, ct)` / `LoadPluginAsync(path, componentIndex, ct)` | `LoadPlugin` | a cold VST3 scan measures 25 s+ |
| `static GenerateWaveformAsync(path, blockMs, ct)` | `GenerateWaveform` | full decode |
| `static GenerateWaveformFromMemoryAsync(data, blockMs, ct)` | `GenerateWaveformFromMemory` | full decode |
| `static RenderToWavAsync(src, dst, tempo, semitones, sampleRate, ct)` | `RenderToWav` | decode + WAV write |

`JuceAsioPanelResult` is `(bool Shown, string? Error)` — the out-parameter pair a
`Task` cannot carry.

> ⚠️ **The three `static` rows above share ONE process-wide worker.** They are
> serialised against each other for the life of the process — a six-stem
> project's waveforms are generated strictly one at a time, and a
> `RenderToWavAsync` of a whole song queues every waveform request behind it —
> and that worker is deliberately never shut down, so one wedged offline decode
> wedges all three permanently. Call the synchronous `GenerateWaveform` /
> `RenderToWav` on your own threads if you need them concurrent.

**The threading contract** (identical for every member above):

* Work runs on **one dedicated long-running thread per engine**, created on first
  use and torn down in `Dispose` *before* the handle drain. The static members
  share one process-wide offline worker instead, having no engine to own them.
* **Never the thread pool.** Parking a pool thread for a 25 s scan or an open
  modal dialog makes the pool inject threads while the rest of the host's async
  work starves.
* The queue is **serial**: a second `ReconfigureAsync` waits for the first. Two
  device changes cannot overlap.
* **Cancellation is honoured before the call starts, and not after.** An
  already-cancelled token never touches native; one cancelled while the item is
  queued is honoured at dispatch. No `dca_*` export is interruptible once
  entered, so a token cancelled mid-call is ignored and the task completes with
  the real result.
* Disposing the engine **faults every queued-but-unstarted call** with
  `ObjectDisposedException` and waits for the one in flight. A host never awaits
  a task that cannot complete.

The synchronous members are unchanged and remain the right call from a background
thread you already own.

## Interfaces (0.9)

`JuceAudioEngine` implements **`IJuceAudioEngine`**, which covers its public
*instance* surface (statics cannot be made substitutable by an interface).
`JuceAudioSource`, `JuceAudioPlugin`, `JuceMidiInput`, `JuceDspChain` and
`JuceDspNode` have the same treatment. The factory members
(`GetDspChain`, `OpenFile`, `OpenMemory`, `LoadPlugin`, `OpenMidiInput`,
`OpenFileAsync`, `LoadPluginAsync`) answer in interface terms *on the interface*
— as explicit implementations, so a caller holding the concrete type still gets
the concrete type back and no existing signature changed.

This exists so a consumer can unit-test the code that *drives* the engine without
`juce_sharp.dll` or a sound card. See
[Testing without the native DLL](../guides/testing-without-native.md).

## Thread-safety

* `Start` / `Stop` / `Dispose` must be called from one thread at a time.
* Setters (`SetMonitorGain`, `SetMasterGain`, …) are safe from any
  thread — they publish atomically and the audio thread ramps to the new value.
* `SendMidi` is lock-free and safe from any thread including UI handlers.
* `GetAndResetInputPeak` and the streaming capture reads have **consumer-side
  state**: two callers will each see part of the signal. Poll from one place.

## Error handling

Every property and method that touches the native engine throws
[`JuceAudioException`](juce-audio-exception.md) on native failure, or
`ObjectDisposedException` after disposal. See
[Error Handling Patterns](../guides/error-handling.md).
