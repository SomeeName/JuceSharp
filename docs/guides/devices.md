# Devices, Hosts & Fallback

Picking an audio device correctly is the single most common source of "it
opened but there is no sound". This page covers the four pieces JuceSharp
gives you: the catalogue, the fallback opener, host swapping and channel
masks.

## Hosts vs drivers vs devices

JUCE calls a backend an `AudioIODeviceType`; JuceSharp calls it a **host** and
exposes its name as `TypeName`. `AudioDriver` is a coarser enum that
**collapses every WASAPI variant into `Wasapi`**, so it cannot tell you whether
you are on shared, exclusive or low-latency mode.

| `TypeName` | `Driver` |
|---|---|
| `"Windows Audio"` | `Wasapi` |
| `"Windows Audio (Exclusive Mode)"` | `Wasapi` |
| `"Windows Audio (Low Latency Mode)"` | `Wasapi` |
| `"DirectSound"` | `DirectSound` |
| `"ASIO"` | `Asio` |
| `"CoreAudio"` | `CoreAudio` |

**Use `TypeName` whenever the mode matters.**

## The catalogue

`JuceDeviceCatalog.Enumerate()` returns every host with its inputs and outputs
in one call, and — since ABI v13 — **without opening a device**, so it is cheap
enough to call while building a settings dialog.

```csharp
foreach (JuceAudioHost host in JuceDeviceCatalog.Enumerate())
{
    Console.WriteLine($"{JuceDeviceCatalog.DisplayName(host.TypeName)}  ({host.Driver})");
    foreach (var d in host.OutputDevices)
        Console.WriteLine($"    out  {d.Name}{(d.IsDefault ? "  [default]" : "")}");
    foreach (var d in host.InputDevices)
        Console.WriteLine($"    in   {d.Name}");
}
```

`host.HasOutputs` / `HasInputs` let you drop empty hosts from a dropdown.
`JuceDeviceCatalog.DriverFor(typeName)` maps back to the enum.

The narrower static enumerations remain: `JuceAudioEngine.EnumerateOutputDevices()`
and `EnumerateInputDevices()` (no engine, every driver), and
`engine.EnumerateOutputDevicesLive()` / `EnumerateInputDevicesLive()` (what the
running engine's current host can see).

## Opening with fallback

> ⚠️ **Warning**
> **An empty device id is not "the default device".** JUCE picks the host's
> *first* device, which on a machine with an interface plugged in is very often
> an ASIO device that is powered off — so `Start()` fails, or succeeds into
> silence.

```csharp
var result = JuceEngineFactory.OpenWithFallback(new JuceEngineConfig
{
    OutputTypeName = "Windows Audio (Low Latency Mode)",
    SampleRate     = 48_000,
    BufferFrames   = 128,
});

if (result.Engine is null)
{
    foreach (string attempt in result.Attempts)
        Console.Error.WriteLine(attempt);
    return;
}

using var engine = result.Engine;
Console.WriteLine($"opened with {result.ConfigUsed.OutputDeviceId}");
```

There is no `Start()` in that sample and none is needed: **constructing a
`JuceAudioEngine` already opens and starts the device** (`dca_engine_create`
takes the config and starts it), so `OpenWithFallback` hands you a running
engine and a later `Start()` returns immediately. The corollary matters on
exclusive-mode ASIO — the sound card is taken the moment the constructor
returns, not at some later call.

`OpenResult` is `(JuceAudioEngine? Engine, JuceEngineConfig ConfigUsed,
IReadOnlyList<string> Attempts)`. `Attempts` is the audit trail — log it, it is
what tells a user *why* their interface was skipped.

## Changing device without recreating the engine

Two levels, both in-place:

```csharp
// Same host, different device / rate / buffer (ABI v8).
engine.Reconfigure(48_000, 256, outputDeviceId: newId, inputDeviceId: null);

// Different host entirely (ABI v12) - e.g. WASAPI shared -> ASIO.
engine.SetDeviceType("ASIO", outputDeviceId: asioId, inputDeviceId: asioId,
                     outputChannelMask: ChannelMask.Stereo,
                     inputChannelMask:  ChannelMask.Single(0));
```

> ⚠️ **Warning**
> `Stop()` followed by `Start()` does **not** restore playback — sources are
> detached from the mixer for good. Reconfigure, or build a new engine.
>
> A device rebuild only replays state that `prepareMasterBus` knows about. The
> mirror device, an attached VST3 and the EQ are replayed; anything you set
> outside that path is not. Re-apply your own per-seat state after a device
> change.

## The ASIO control panel

Buffer size, clock source and channel layout on an ASIO interface belong to the
vendor's own panel, not to `JuceEngineConfig`. Open it with:

```csharp
if (!engine.TryShowAsioControlPanel(parentWindow: nint.Zero, out string? error))
    Console.Error.WriteLine($"no ASIO panel: {error}");
```

* **It never throws.** Every failure — not an ASIO driver, no device open, the
  engine disposed — comes back as `error`, so there is no `catch` to write.
* **It blocks for as long as the vendor's MODAL dialog is up**, which can be
  minutes. Call it from a UI thread, never from anything with a deadline. (This
  call is the reason `Dispose()` waits on the last in-flight P/Invoke instead of
  freeing under it.)
* `parentWindow` is a native window handle for modality; `nint.Zero` is fine and
  means "no parent".
* Changing the buffer size in that panel is a **device rebuild** — re-read the
  warning above about what is and is not replayed.

## Channel masks

`OutputChannelMask` / `InputChannelMask` are raw `uint` bitmasks, LSB = channel
0. `ChannelMask` builds and reads them:

| Member | Result |
|---|---|
| `ChannelMask.Mono` | `1u` |
| `ChannelMask.Stereo` | `0b11u` |
| `ChannelMask.Single(int channel)` | one bit; `0u` for a channel outside 0…31 |
| `ChannelMask.First(int count)` | the low `count` bits |
| `ChannelMask.FromChannels(params int[])` | an arbitrary set |
| `ChannelMask.Enumerate(uint mask)` | `IEnumerable<int>` of the set bits |

Leave a mask at `0` to let JUCE pick the first *N* channels in order.

## The null device

For tests and CI on a box with no sound card, set `DCA_ENABLE_NULL_DEVICE=1`
before the engine is created. A silent device type appears in the catalogue and
can be opened like any other. `JuceSharp.Examples.Playback --null` (or `--device null`) runs
the whole playback example on it.
