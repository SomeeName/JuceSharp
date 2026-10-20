# Master EQ Bank

ABI v29 added a six-band parametric EQ on the master bus, alongside the older
single high-pass / low-pass / band-pass filters.

```csharp
Console.WriteLine(JuceAudioEngine.EqBandCount);   // 6

engine.SetEqBand(index: 0, type: 0, freqHz: 80f,   q: 0.7f, gainDb: -3f, enabled: true);
engine.SetEqBand(index: 1, type: 1, freqHz: 2500f, q: 1.2f, gainDb: +4f, enabled: true);
engine.SetEqTrim(-2f);
engine.SetEqEnabled(true);
```

> ℹ️ **Note**
> **Everything is OFF at engine start** — the whole bank, and every band inside
> it. An engine that nobody configures runs the same signal path it did before
> v29.

## Members

| Member | Purpose |
|---|---|
| `SetEqBand(int index, int type, float freqHz, float q, float gainDb, bool enabled)` | Configure one band. `index` is `0 … EqBandCount-1`. |
| `SetEqEnabled(bool enabled)` | Master enable for the whole bank. |
| `SetEqSolo(int bandIndex)` | Audition one band on its own. |
| `SetEqRoute(int route)` | Which bus the bank sits on. |
| `SetEqTrim(float trimDb)` | Post-EQ make-up trim. |
| `GetEqActiveCount()` | How many bands are actually **audible**. |
| `ReadEqSpectrum(float[] destDb)` | Sampled magnitude response in dB for a UI curve; returns how many points were written. |

`type` and `route` have typed overloads (`JuceEqBandType`, `JuceEqRoute`) as
well as the raw `int` ones; the ordinals are pinned to the header by test.

## "Audible", not "enabled"

`GetEqActiveCount` does not count the bands you switched on; it counts the ones
that actually change the signal. A band enabled at 0 dB gain with a bell shape
is not audible, and is not published.

That predicate is evaluated **once, on the message thread**, and the audio
callback consumes the result — so the export and the callback are one predicate
rather than two copies of one that can drift apart. Treat `GetEqActiveCount` as
authoritative for "is the EQ doing anything", including in a UI badge.

## Drawing the response

```csharp
var db = new float[256];
int n = engine.ReadEqSpectrum(db);
for (int i = 0; i < n; i++)
    plot.AddPoint(i, db[i]);
```

The spectrum reflects the same coefficients the callback is running, including
the trim and any solo.

## Replay it after a device rebuild

> ⚠️ **Warning**
> A device change, a sample-rate renegotiation or a watchdog recovery rebuilds
> the master bus, and **only what `prepareMasterBus` knows about is replayed**.
> The EQ shipped a bug of exactly this shape once. Re-apply your bands after
> any `Reconfigure` or `SetDeviceType`.

## The older filters

`SetHighPass(freqHz, q)`, `SetLowPass(freqHz, q)` and `SetBandPass(freqHz, q)`
predate the bank and are independent of it. `freqHz <= 0` disables a filter.
They retune without zeroing their state, so sweeping one does not click.

## NaN safety

Every untrusted stage in the signal path is followed by a scrub before the EQ
sees it — the VST3 input chain and the sandbox bridge in particular. This
matters because **a NaN into a biquad is permanent**: the filter feeds its own
output back, and `reset()` is only reached on an off→on transition, so one bad
sample out of a user plugin used to silence a seat for the rest of the session,
meter included.
