# JuceAudioDevice

`sealed record JuceAudioDevice`

Information about a single audio device, returned by
[`JuceAudioEngine.EnumerateOutputDevices`](juce-audio-engine.md) and
friends.

## Fields

| Field                 | Type          | Description                                |
| --------------------- | ------------- | ------------------------------------------ |
| `Id`                  | `string`      | Stable device identifier. Driver-specific. |
| `Name`                | `string`      | Human-readable name.                       |
| `Driver`              | `AudioDriver` | Which driver enumerated this device.       |
| `MaxInputChannels`    | `int`         | Max simultaneous input channels.           |
| `MaxOutputChannels`   | `int`         | Max simultaneous output channels.          |
| `DefaultSampleRate`   | `int`         | Driver-reported default rate in Hz.        |
| `IsDefault`           | `bool`        | True if this is the OS / driver default.   |
| `TypeName`            | `string`      | JUCE's `AudioIODeviceType` name — `"Windows Audio"`, `"Windows Audio (Exclusive Mode)"`, `"Windows Audio (Low Latency Mode)"`, `"ASIO"`, `"CoreAudio"`. Defaults to `""` for the no-engine enumeration path. |

> ⚠️ **Warning**
> **`Driver` and `TypeName` are not the same thing.** The `AudioDriver` enum
> collapses every WASAPI variant into `Wasapi`, so `TypeName` is the only way to
> distinguish shared, exclusive and low-latency mode. Pass `TypeName` to
> `JuceEngineConfig.OutputTypeName` / `InputTypeName` or to
> `JuceAudioEngine.SetDeviceType` when you care which one you get.

## Example

```csharp
foreach (var d in JuceAudioEngine.EnumerateOutputDevices())
{
    Console.WriteLine($"{d.Driver,-12} {d.Name}");
    Console.WriteLine($"             default rate = {d.DefaultSampleRate}, " +
                      $"out = {d.MaxOutputChannels} ch");
}
```

## Notes

* `Id` is what you pass to `JuceEngineConfig.OutputDeviceId` /
  `InputDeviceId` to select this device explicitly.
* If multiple drivers can address the same physical interface (e.g.
  Focusrite shows up under both WASAPI and ASIO), you'll see two
  entries.
