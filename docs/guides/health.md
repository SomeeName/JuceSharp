# Engine Health & Dropouts

ABI v30 added dropout counters measured **inside the audio callback**, which is
the only place the truth is. A managed poll can observe that audio sounded
wrong; it cannot tell you whether the callback arrived late, ran long, or the
driver itself glitched.

```csharp
if (engine.TryGetXrunStats(out XrunStats x))
{
    Console.WriteLine($"callbacks={x.Callbacks}  late={x.LateCallbacks}  " +
                      $"overruns={x.DeadlineOverruns}  near={x.NearMisses}  " +
                      $"device={(x.DeviceXruns?.ToString() ?? "n/a")}  " +
                      $"load={x.Load:P0}");
}

engine.ResetXrunStats();
```

## The three counters are three different faults

| Counter | Meaning | Who is at fault |
|---|---|---|
| `LateCallbacks` | The callback **arrived** late. | The system. This is the **audible** one, and on WASAPI / CoreAudio it is the only dropout signal the driver gives at all. |
| `DeadlineOverruns` | Our own processing exceeded the block deadline. | **Us.** Too much DSP, or a plugin that is too slow. |
| `NearMisses` | We came close to the deadline. | A load warning, not a dropout. |

Do not sum them, and do not surface them as one "dropouts" number: the
remediation is different for each. `LateCallbacks` rising means raise the
buffer size or blame the OS; `DeadlineOverruns` rising means take work out of
the chain.

> ⚠️ **Warning**
> **`DeviceXruns` is `int?` and `null` means "this driver cannot report".**
> Never render that as `0` — you would be claiming a clean run you have no
> evidence for. The native side returns −1 for it and the wrapper maps that to
> `null`.

> ℹ️ **Note**
> **The first four callbacks of every device session are ignored.** Without
> that, every device change would report a dropout that never happened.

## The rest of `XrunStats`

| Member | Type | Meaning |
|---|---|---|
| `DeviceOpen` | `bool` | A device is open. |
| `DeviceRunning` | `bool` | It is streaming. |
| `Callbacks` | `long` | Total callbacks counted this session. |
| `DroppedFrames` | `long` | Frames lost. |
| `LastXrunHostTicks` | `long` | Host clock at the most recent dropout. |
| `LastXrunRenderFrame` | `long` | Render frame at the most recent dropout — line this up against your own timeline to find *what* was happening. |
| `MaxCallbackNs` | `long` | Worst callback duration seen. |
| `LastCallbackNs` | `long` | Most recent callback duration. |
| `BlockBudgetNs` | `long` | The deadline for one block at the current rate and size. |
| `Load` | `double?` | `LastCallbackNs / BlockBudgetNs`, or `null` when the budget is unknown. |
| `HasDropouts` | `bool` | `LateCallbacks > 0 || DeviceXruns > 0`. |

`Load` is the number to put on a status bar. Sustained values above ~0.7 mean
you are one plugin away from `DeadlineOverruns`.

## Output latency

```csharp
int samples = engine.OutputLatencySamples;              // ABI v7
double ms   = 1000.0 * samples / engine.SampleRate;
```

Includes the device buffer plus whatever extra driver/hardware delay the OS
layer reports. Returns **0** when no device is open — check `DeviceOpen`
before you display it.

Note that a hosted VST3 adds its own latency on top, reported separately by
`JuceAudioPlugin.LatencySamples`, and nothing compensates for it
automatically.

Since ABI v34 this figure **also includes the master limiter's look-ahead**
(1 ms), because that stage is genuinely in the path between the samples you hand
the engine and the ear. `engine.MasterLimiterLatencySamples` reports the
limiter's share if you need to decompose it.

It **follows** the limiter's `enabled` flag — switched off, the limiter is not
in the path at all and contributes nothing. So if you align scheduled clicks
against `OutputLatencySamples`, **re-read it after a toggle** rather than
caching it for the session. (Keeping the delay line running while the limiter is
off so this number never moved was tried and rejected: it makes every user pay
1 ms on their own monitored playing for a feature they switched off, and it
re-times the whole engine — the RT gate caught exactly that.)

## Master-bus ceiling

```csharp
engine.SetMasterLimiter(enabled: true, ceilingDb: -0.3f, releaseMs: 80f);
float reduction = engine.MasterLimiterReductionDb;   // POSITIVE dB, 0 when idle
```

**OFF at engine start since ABI v35 — the host opts in** (v34 shipped it ON,
which cost every player 1 ms of latency on their own monitored playing for a
feature they had not asked for). −0.3 dBFS and 80 ms are the defaults once it is
enabled, and it is a **true bypass** when off: the output is bit-for-bit the
pre-v34 engine. The cost of that is a one-block discontinuity
when a user toggles it mid-song, which is a rare gesture and the price of not
charging everyone 1 ms for a stage they disabled.

The engine's output is the sum of the song bus,
every seat's monitor mix, the click bus and both instrument legs, and nothing
bounded that sum before v34 — a hardware DAC clips what comes out past ±1.0,
but the streaming mirror carries float, so over-range samples survived into
OBS / Discord's encoder and wrapped there.

`MasterLimiterReductionDb` is **poll-only**, exactly like the ABI v30 node
meters: nothing is queued, no callback crosses the ABI, and a caller that never
reads it pays nothing. It is a decaying hold with a 0.25 s time constant, so a
30 Hz poll still catches a single transient — put it on a gain-reduction meter,
not in a control loop.

## Testing without a sound card

Set `DCA_ENABLE_NULL_DEVICE=1` before creating the engine. A silent device type
appears in the catalogue and behaves like any other, so device-dependent logic
can be exercised on a CI box or a machine whose interface is in use.

```powershell
$env:DCA_ENABLE_NULL_DEVICE = '1'
dotnet run --project examples\JuceSharp.Examples.Playback -- song.wav 5 --null        # --device null is an accepted alias
```

The suite's `NullDeviceTests` cover it, and the native RT gate
(`juce_sharp_rtcheck`) drives the real callback under an allocation hook and a
lock allow-list — so "no allocation on the audio thread" is enforced rather
than intended.
