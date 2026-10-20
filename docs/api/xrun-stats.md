# XrunStats

`readonly record struct XrunStats`

A snapshot of the engine's dropout counters (ABI v30) — **what the audio
callback observed**, not what a managed poll could infer. Obtained from
`JuceAudioEngine.TryGetXrunStats(out XrunStats)` and zeroed by
`ResetXrunStats()`.

## Members

| Member | Type | Description |
|---|---|---|
| `DeviceOpen` | `bool` | A device is open. |
| `DeviceRunning` | `bool` | It is streaming. |
| `DeviceXruns` | `int?` | The driver's own xrun count. **`null` means the driver cannot report** — never render it as 0. |
| `Callbacks` | `long` | Total callbacks this session. |
| `LateCallbacks` | `long` | The callback **arrived** late. The audible fault. |
| `DeadlineOverruns` | `long` | *Our* processing exceeded the block deadline. |
| `NearMisses` | `long` | Close to the deadline — a load warning, not a dropout. |
| `DroppedFrames` | `long` | Frames lost. |
| `LastXrunHostTicks` | `long` | Host clock at the most recent dropout. |
| `LastXrunRenderFrame` | `long` | Render frame at the most recent dropout. |
| `MaxCallbackNs` | `long` | Worst callback duration seen. |
| `LastCallbackNs` | `long` | Most recent callback duration. |
| `BlockBudgetNs` | `long` | The deadline for one block at the current rate and size. |
| `Load` | `double?` | `LastCallbackNs / BlockBudgetNs`, or `null` when `BlockBudgetNs <= 0`. |
| `HasDropouts` | `bool` | `LateCallbacks > 0 \|\| DeviceXruns > 0`. |

## Example

```csharp
if (engine.TryGetXrunStats(out XrunStats x) && x.DeviceOpen)
{
    status.Text = x.HasDropouts
        ? $"dropouts: {x.LateCallbacks} late, {x.DeviceXruns?.ToString() ?? "n/a"} device"
        : $"clean — load {x.Load:P0}";
}
```

## Notes

> ⚠️ **Warning**
> **Do not sum the three counters.** `LateCallbacks`, `DeadlineOverruns` and
> `NearMisses` are three different faults with three different remediations:
> the system was late, *we* were slow, or we came close. Collapsing them into
> one "dropouts" number destroys the only diagnostic information there is.

* **The first four callbacks of every device session are ignored.** Without
  that, every device change would report a dropout that never happened.
* On WASAPI and CoreAudio, `LateCallbacks` is the *only* dropout signal the
  driver gives at all — which is why `DeviceXruns` is so often `null` there.
* `LastXrunRenderFrame` lines up against `TryGetRenderClock`, so you can
  correlate a dropout with what your app was doing at that exact output sample.

See [Engine Health & Dropouts](../guides/health.md).
