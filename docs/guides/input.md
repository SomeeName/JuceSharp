# Live Input & Monitoring

JuceSharp can capture the engine's input device and route it through the
output mix — useful for guitar processing, voice monitoring, podcasting,
etc.

## Selecting input device

```csharp
using var engine = new JuceAudioEngine(new JuceEngineConfig
{
    PreferredDriver  = AudioDriver.Asio,    // lowest input latency on Windows
    InputDeviceId    = "Focusrite USB ASIO",
    InputChannels    = 2,
    InputChannelMask = ChannelMask.Single(0),   // only Ch.1
    BufferFrames     = 128,
});
engine.Start();
```

`InputChannelMask` is a per-channel bitmask (LSB = channel 0). Leave it `0` to
use the first `InputChannels` channels in order. `ChannelMask.Single`,
`ChannelMask.First`, `ChannelMask.FromChannels` and `ChannelMask.Enumerate`
build and read them.

## Monitoring and trim

Two separate linear gains, both ramped across the block on the native side (a
constant per block would zipper):

```csharp
engine.SetMonitorGain(0.8f);   // how much dry input goes to the output mix
engine.SetInputGain(1.0f);     // pre-chain input trim
```

There is no `SetInputMonitoring(enabled, gain)` — pass `0f` to
`SetMonitorGain` to mute the monitor path.

The monitoring path is **dry** unless you build a DSP rig or load a plugin and
call `AttachToInput()` on it — see [Hosting VST3 Plugins](vst3-hosting.md) and
[The DSP Tone Rig](tone-rig.md).

> ⚠️ **Warning**
> **Feedback warning.** If your output speakers are picking up your input
> mic, monitoring will feed back. Use headphones or a directional mic.

## Querying the live input level

```csharp
float peak = engine.GetAndResetInputPeak();       // LINEAR, 0..1 (not dB)
float db   = peak > 0 ? 20f * MathF.Log10(peak) : float.NegativeInfinity;
```

`GetAndResetInputPeak` returns the peak **linear** amplitude observed since the
last call and resets the accumulator — so poll it on a fixed cadence
(every ~16 ms on a UI timer is fine) and do not call it from two places, or
each will see only part of the signal. There is no `GetInputLevelDb()`;
convert yourself as above.

## Multiple players (seats)

With more than one seat armed, everything above has a per-seat form:

```csharp
engine.SetPlayerCount(2);
engine.SetPlayerInputChannel(0, 0);
engine.SetPlayerInputChannel(1, 1);
engine.SetPlayerMonitorGain(1, 0.6f);

float p1 = engine.GetAndResetPlayerInputPeak(1);
```

See [Live Seats](seats.md) for the whole ABI-v28 surface. At a seat count of
1 the audio callback runs the pre-v28 path byte-for-byte.

## Reading the raw input

A dry, pre-gain, pre-chain mono ring is available for analysis (tuner, note
detection):

```csharp
Span<float> buf = stackalloc float[2048];
int n = engine.ReadInputCapture(buf);                       // latest window
int m = engine.ReadInputCaptureStream(buf, out ulong pos);  // only new samples
```

`ReadInputCapture` always returns the **latest window** and must not be
drain-polled; `ReadInputCaptureStream` keeps its own cursor and is the one to
use for a continuous consumer. `SetInputCaptureChannel(-1)` mixes every input
channel down to mono.
