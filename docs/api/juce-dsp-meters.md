# JuceDspMeters & JuceDspNodeMeter

`namespace JuceSharp.Dsp` (in the **core `JuceSharp`** package, not
`JuceSharp.Dsp`)

The managed surface over the ABI-v30 per-node DSP meters. It is polling and
marshalling only — no state, no caching, no lifetime management.

> ℹ️ **Note**
> This is the OLDER meter façade; prefer [`JuceDspChain`](juce-dsp-chain.md),
> which pins the engine handle and covers the whole rig. Everything
> else — adding nodes, parameters, the catalogue, latency — goes through the C
> ABI. See [The DSP Tone Rig](../guides/tone-rig.md).

## `JuceDspNodeMeter`

`readonly record struct JuceDspNodeMeter(float InputPeak, float InputRms, float OutputPeak, float OutputRms)`

**Linear sample amplitudes for one node** — not dB, and not the song or master
bus. Convert to dB yourself if you need it.

## `JuceDspMeters`

`static class JuceDspMeters`

| Member | Description |
|---|---|
| `static bool SetEnabled(nint engine, int player, bool enabled)` | Turn metering on or off for one seat. |
| `static bool IsEnabled(nint engine, int player)` | Is it on for this seat? |
| `static bool TryRead(nint engine, int nodeId, out JuceDspNodeMeter meter)` | Poll one node. |
| `static int Read(nint engine, int player, float[] values)` | Poll a whole seat in one call — four floats per node, in chain order. **Returns the NODE COUNT, which may exceed the number of entries written**: the native call writes only complete entries that fit, so clamp with the capacity you passed (`Math.Min(returned * 4, values.Length)`) or you read an uninitialised tail. An empty array is the way to size the result. `0` means both "this seat has no nodes" and "the call failed" — the native `-1` sentinel is folded to `0` so a caller sizing an array never sees a negative length; use `JuceDspChain.TryReadMeters(…, out int)` where the engine's lifetime is tracked and the two are distinguishable. |

## Example

```csharp
using JuceSharp.Dsp;

nint eng = engine.NativeHandle;
JuceDspMeters.SetEnabled(eng, player: 0, enabled: true);

// per node
if (JuceDspMeters.TryRead(eng, cabNodeId, out JuceDspNodeMeter m))
    Console.WriteLine($"in {m.InputPeak:F3} / {m.InputRms:F3}   " +
                      $"out {m.OutputPeak:F3} / {m.OutputRms:F3}");

// whole seat
var values = new float[256];
int n = JuceDspMeters.Read(eng, player: 0, values);

JuceDspMeters.SetEnabled(eng, player: 0, enabled: false);   // stop paying for it
```

## Notes

> ⚠️ **Warning**
> **These take a raw `nint`. The caller must keep the engine handle alive for
> the whole call** — the wrapper does not acquire it. Do not stash
> `engine.NativeHandle` and use it later; read it immediately before the call,
> from an engine you know is not being disposed.

* **Metering is off until you ask for it, and you own the enable's lifetime.**
  Turn it off when the meter UI is hidden; a rig with nothing watching should
  not pay for the measurement.
* It is **poll-only**. There is no callback, no event, no push. Drive it from
  a UI timer.
* `player` is the seat index, `0 … JuceAudioEngine.MaxPlayers - 1`. `nodeId`
  is engine-global (`player * 1_000_000 + local`), so `TryRead` resolves the
  seat from the id and takes no `player`.
