# JuceEngineConfig

`sealed class JuceEngineConfig`

Engine creation parameters. All numeric fields use `0` as a sentinel
meaning "device default". Validated by `JuceAudioEngine`'s constructor.

```csharp
var cfg = new JuceEngineConfig
{
    PreferredDriver   = AudioDriver.Asio,
    SampleRate        = 48_000,
    BufferFrames      = 128,
    OutputChannels    = 2,
    InputChannels     = 1,
    OutputDeviceId    = "Focusrite USB ASIO",
    InputDeviceId     = "Focusrite USB ASIO",
    EnableConsoleLog  = true,
};
```

## Fields

| Field               | Type          | Default        | Notes                                |
| ------------------- | ------------- | -------------- | ------------------------------------ |
| `SampleRate`        | `int`         | `0`            | Hz. 0 ≤ N ≤ 384 000.                 |
| `BufferFrames`      | `int`         | `0`            | 0 ≤ N ≤ 16 384.                      |
| `OutputChannels`    | `int`         | `0`            | 0 ≤ N ≤ 64.                          |
| `InputChannels`     | `int`         | `0`            | 0 ≤ N ≤ 64.                          |
| `OutputChannelMask` | `uint`        | `0`            | Per-channel bitmask. 0 = let JUCE pick. |
| `InputChannelMask`  | `uint`        | `0`            | Per-channel bitmask. 0 = let JUCE pick. |
| `PreferredDriver`   | `AudioDriver` | `Default`      | See enum below.                      |
| `OutputDeviceId`    | `string`      | `""`           | Driver-specific. Empty = default.    |
| `InputDeviceId`     | `string`      | `""`           | Driver-specific. Empty = default.    |
| `OutputTypeName`    | `string`      | `""`           | JUCE `AudioIODeviceType` name. **Overrides `PreferredDriver` when non-empty** — this is how you pick WASAPI Exclusive or Low-Latency mode. |
| `InputTypeName`     | `string`      | `""`           | The same for the input host.         |
| `EnableConsoleLog`  | `bool`        | `true`         | Wire `JuceConsoleLogger` if no `ILogger` given. |
| `EnableNullDevice`  | `bool`        | `false`        | Register the headless "Null" device type for THIS engine (ABI v41, `dca_engine_create2`). Prefer it to the process-wide `DCA_ENABLE_NULL_DEVICE=1`, which every engine in the process reads; the env var still works and is OR-ed in. |
| `NullDeviceManualPump` | `bool`     | `false`        | Manual-pump mode for that device (native harnesses); implies `EnableNullDevice`. OR-ed with `DCA_NULL_DEVICE_MANUAL=1`. |

Every property is `init`-only. The four string properties are `[AllowNull]`
and null-coalesce to `""` in their init accessor, so you can assign `null`
freely.

## Building channel masks

`OutputChannelMask` / `InputChannelMask` are raw `uint` bitmasks with LSB =
channel 0. The static [`ChannelMask`](channel-mask.md) helpers build and read
them:

```csharp
new JuceEngineConfig
{
    InputChannels    = 2,
    InputChannelMask = ChannelMask.FromChannels(0, 3),  // channels 1 and 4
    OutputChannelMask = ChannelMask.Stereo,             // 0b11
};
```

There is no property called `ChannelMask` on this type — that is the name of
the helper class.

## `AudioDriver` enum

```csharp
public enum AudioDriver
{
    Default     = 0,
    Wasapi      = 1,
    DirectSound = 2,
    Asio        = 3,
    CoreAudio   = 4,
    Alsa        = 5,
    Jack        = 6,
}
```

## `Validate()`

Throws `ArgumentOutOfRangeException` on any out-of-range numeric field
or unknown `AudioDriver` value. Called automatically by
`JuceAudioEngine`'s constructor.
