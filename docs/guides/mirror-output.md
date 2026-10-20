# Mirror Output

The mirror sends a parallel copy of the engine's final master mix — including
the post-DSP, post-VST guitar signal — to a second device. Useful for
streaming, recording, or sending audio to OBS / Discord while ASIO has
exclusive control of the primary output.

## Enabling

```csharp
bool ok = engine.SetMirrorOutput("CABLE Input (VB-Audio Virtual Cable)");
```

The signature is `bool SetMirrorOutput(string? deviceId)`; there is no
`enabled` parameter. `SetMirrorDevice(string?)` is the same method under its
native name — `SetMirrorOutput` simply forwards to it.

Pass `null` or `""` to disable:

```csharp
engine.SetMirrorOutput(null);
```

Both return `false` rather than throwing when the native call fails (a bad
device id, a device that is gone), and log a warning.

## Discovering devices

There is no `EnumerateDevices(DeviceDirection)`. Three options, cheapest first:

```csharp
// Static — every driver, no engine needed.
foreach (var d in JuceAudioEngine.EnumerateOutputDevices())
    Console.WriteLine($"{d.Name}  ({d.TypeName}, {d.MaxOutputChannels}ch)");

// Live — what the running engine's current host can see.
foreach (var d in engine.EnumerateOutputDevicesLive())
    Console.WriteLine(d.Name);

// Whole catalog — every host with its inputs and outputs, one call.
foreach (var host in JuceDeviceCatalog.Enumerate())
    Console.WriteLine($"{host.TypeName}: {host.OutputDevices.Count} out");
```

Pass `JuceAudioDevice.Id` to `SetMirrorOutput`. Prefer `TypeName` over
`Driver` when you care about the WASAPI sub-mode — the `AudioDriver` enum
collapses shared, exclusive and low-latency into `Wasapi`.

## Behaviour

The mirror runs on its own block-pump thread with drift control
(`MirrorClockController`) and is non-blocking: if the mirror device chokes, the
primary output is unaffected. The mirror device is remembered across a device
rebuild, so a driver change or watchdog recovery does not silently drop it.

## Common usage: OBS desktop capture

```csharp
engine.SetMirrorOutput("CABLE Input (VB-Audio Virtual Cable)");
```

Now route VB-Cable into OBS as an audio source. The primary output keeps
its dedicated ASIO path with full low-latency behaviour.
