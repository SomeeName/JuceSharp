# Live Seats (Multiple Players)

ABI v28 turned the engine's single live-input path into up to four independent
**seats**. Each one owns its own DSP tone chain, its own VST3 input chain, its
own mono capture ring and its own hardware input channel — and they all share
**one** open device.

That last point is the whole design constraint: this is channel routing inside
a single `AudioIODevice`, never a second device open. An ASIO driver is
typically exclusive, so two engines cannot split one interface's two inputs.

```csharp
Console.WriteLine(JuceAudioEngine.MaxPlayers);          // 4  (DCA_MAX_PLAYERS)
Console.WriteLine(JuceAudioEngine.PlayerNodeIdStride);  // 1_000_000
```

## Arming seats

```csharp
engine.SetPlayerCount(2);
Console.WriteLine(engine.GetPlayerCount());   // 2
```

> ℹ️ **Note**
> **The count defaults to 1, and at 1 the audio callback runs the pre-v28 path
> byte-for-byte.** A solo player must not pay for — or hear any change from — a
> feature they are not using. Raise the count only when a second player is
> genuinely present.
>
> A second seat is an explicit choice, never inferred from a second input
> channel being open. Two open channels are far more often a stereo pickup.

## Routing each seat

```csharp
engine.SetPlayerInputChannel(0, 0);        // seat 0 <- hardware channel 1
engine.SetPlayerInputChannel(1, 1);        // seat 1 <- hardware channel 2
int ch = engine.GetPlayerInputChannel(1);

engine.SetPlayerMonitorGain(0, 1.0f);
engine.SetPlayerMonitorGain(1, 0.7f);      // linear, ramped across the block
```

The channel index is clamped to the device's actual input count on the audio
thread, so an out-of-range value degrades rather than faults.

## Metering each seat

```csharp
for (int p = 0; p < engine.GetPlayerCount(); p++)
{
    float peak = engine.GetAndResetPlayerInputPeak(p);   // LINEAR, read-and-reset
    DrawMeter(p, peak);
}
```

Read-and-reset means exactly one consumer: two pollers each see part of the
signal.

## Capture per seat

Each seat has its own dry, pre-gain, pre-chain mono ring:

```csharp
Span<float> buf = stackalloc float[2048];

int n = engine.ReadInputCapture(player: 1, buf);                    // latest window
int m = engine.ReadInputCaptureStream(player: 1, buf);              // only new samples
int k = engine.ReadInputCaptureStream(player: 1, buf, out ulong pos);

if (engine.TryGetInputCaptureSync(player: 1, out ulong captured, out long hostTicks))
    { /* align this seat's audio against the host clock */ }
```

Use the **streaming** read for a continuous consumer such as live note
detection; the windowed read always returns the full latest window and must not
be drain-polled.

## A tone rig per seat

Chain-scoped tone-rig operations take the seat index; node-scoped ones resolve
it from the node id, because ids are engine-global:

```
node id = player * 1_000_000 + local
```

Seat 0's ids are `0, 1, 2, …` exactly as before v28, so every pre-v28 export is
an alias for "seat 0".

> ⚠️ **Warning**
> A node id naming a seat ≥ 4 is **rejected**, never clamped onto another
> seat's rig. Do not synthesise ids yourself — keep what the add call returned.

See [The DSP Tone Rig](tone-rig.md). Plugins attach per seat too:

```csharp
amp.AttachToInput(player: 1);
Console.WriteLine(amp.InputPlayer);   // 1, or -1 when detached
```

## Drop tune per seat

Until ABI v31 the engine held **one** shifter and applied it to the raw device
input *before* the per-player split — so on a couch with two guitars in
different tunings, dropping one dropped both, and the seat that did not ask for
it was detuned with nothing on screen saying so. Use the per-seat overloads:

```csharp
engine.SetInputPitchShift(player: 1, semitones: -2f);
```

See [Drop Tune](drop-tune.md).

## Replay state after a device rebuild

A device change, a NAM 48 kHz request or a watchdog recovery rebuilds the
engine's master bus, and **only what `prepareMasterBus` knows about is
replayed**. Re-apply the seat count, the per-seat channels and the per-seat
monitor gains yourself after any reconfigure.

## Offline verification

`dca_engine_dsp_process_buffer_for_player` renders any seat's rig with **no
device open at all** — build seat 0 and seat 1 differently, push the same input
through both, compare. `player` outside `0…3` returns `DCA_ERR_INVALID_ARG`;
an open device returns `DCA_ERR_DEVICE`.
