# ChannelMask

`static class ChannelMask`

Helpers to build the channel bitmasks consumed by
[`JuceEngineConfig`](juce-engine-config.md) and
`JuceAudioEngine.SetDeviceType`. **LSB = channel 0.**

The masks themselves are plain `uint`; this class is just arithmetic, so there
is nothing to dispose and nothing to construct.

## Members

| Member | Signature | Notes |
|---|---|---|
| `Mono` | `static uint Mono` | `1u` — channel 0 only. |
| `Stereo` | `static uint Stereo` | `0b11u` — channels 0 and 1. |
| `Single` | `static uint Single(int channel)` | One bit. Returns **`0u`** for a channel below 0 or at/above 32 — it does not throw. |
| `First` | `static uint First(int count)` | The low `count` bits. `0u` for `count <= 0`; `uint.MaxValue` for `count >= 32`. |
| `FromChannels` | `static uint FromChannels(params int[] channels)` | An arbitrary set. |
| `Enumerate` | `static IEnumerable<int> Enumerate(uint mask)` | The set bits, ascending. |

## Example

```csharp
var cfg = new JuceEngineConfig
{
    InputChannels     = 2,
    InputChannelMask  = ChannelMask.FromChannels(0, 3),   // channels 1 and 4
    OutputChannels    = 2,
    OutputChannelMask = ChannelMask.Stereo,
};

foreach (int ch in ChannelMask.Enumerate(cfg.InputChannelMask))
    Console.WriteLine($"input channel {ch + 1} is active");
```

## Notes

* A mask of `0` means **"let JUCE pick"** — the first `InputChannels` /
  `OutputChannels` channels in order. That is the default.
* `Single` and `First` clamp silently rather than throwing, because they are
  frequently fed a value straight out of a UI. If you need to know that a
  channel index was out of range, check it before you call.
* There is no property named `ChannelMask` on `JuceEngineConfig` — the
  properties are `InputChannelMask` and `OutputChannelMask`.
