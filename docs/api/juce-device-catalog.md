# JuceDeviceCatalog & JuceAudioHost

One-call catalogue of every audio host and device available to JUCE on this
platform. Built for populating a device-manager UI without spinning up probe
engines yourself.

## `JuceAudioHost`

`sealed record JuceAudioHost(string TypeName, AudioDriver Driver, IReadOnlyList<JuceAudioDevice> OutputDevices, IReadOnlyList<JuceAudioDevice> InputDevices)`

A single `AudioIODeviceType` — "Windows Audio", "ASIO", "CoreAudio" — with its
devices.

| Member | Type | Description |
|---|---|---|
| `TypeName` | `string` | The JUCE host name. This is what you pass to `SetDeviceType` and `JuceEngineConfig.OutputTypeName`. |
| `Driver` | `AudioDriver` | The coarse enum. Every WASAPI mode collapses to `Wasapi`. |
| `OutputDevices` | `IReadOnlyList<JuceAudioDevice>` | |
| `InputDevices` | `IReadOnlyList<JuceAudioDevice>` | |
| `HasOutputs` | `bool` | `OutputDevices.Count > 0`. |
| `HasInputs` | `bool` | `InputDevices.Count > 0`. |

## `JuceDeviceCatalog`

`static class JuceDeviceCatalog`

| Member | Description |
|---|---|
| `static IReadOnlyList<JuceAudioHost> Enumerate()` | Every host with its inputs and outputs. Uses the ABI-v13 host-scoped enumeration, so **no device is opened**. |
| `static string DisplayName(string typeName)` | A friendlier label for a host name, for a dropdown. |
| `static AudioDriver DriverFor(string typeName)` | Map a host name back to the coarse enum. |

## Example

```csharp
foreach (JuceAudioHost host in JuceDeviceCatalog.Enumerate())
{
    if (!host.HasOutputs) continue;

    Console.WriteLine(JuceDeviceCatalog.DisplayName(host.TypeName));
    foreach (var d in host.OutputDevices)
        Console.WriteLine($"    {d.Name}{(d.IsDefault ? "  [default]" : "")}  " +
                          $"{d.MaxOutputChannels}ch @ {d.DefaultSampleRate} Hz");
}
```

## Notes

* Prefer this over `JuceAudioEngine.EnumerateOutputDevices()` when you need to
  know *which host* a device belongs to. The static enumerations flatten that
  away.
* `engine.EnumerateOutputDevicesLive()` is still the right call for "what can
  the engine I already have open see right now".
* Because nothing is opened, an ASIO interface that is powered off still
  appears here. That is deliberate — the user needs to see it in the list —
  but it is also why you should open through
  [`JuceEngineFactory.OpenWithFallback`](juce-engine-factory.md).
