# JuceEngineFactory

`static class JuceEngineFactory`

Helpers to open a [`JuceAudioEngine`](juce-audio-engine.md) safely.

> ⚠️ **Warning**
> JUCE's default behaviour when an empty device id is supplied is to pick the
> host's **first** device — which is very often an ASIO interface that is
> powered off. The result is either a failed `Start()` or, worse, a successful
> open into silence. Prefer this factory over `new JuceAudioEngine(config)`
> anywhere a user's hardware is involved.

## `OpenResult`

`sealed record OpenResult(JuceAudioEngine? Engine, JuceEngineConfig ConfigUsed, IReadOnlyList<string> Attempts)`

| Member | Type | Description |
|---|---|---|
| `Engine` | `JuceAudioEngine?` | The opened engine, or `null` if every attempt failed. |
| `EngineInterface` | `IJuceAudioEngine?` | The same instance typed as the abstraction seam, for a host that holds its engine through the interface. `Engine` stays concrete because three of the six shipped examples pass it to a method taking the concrete type. |
| `ConfigUsed` | `JuceEngineConfig` | The config that actually worked — not necessarily the one you passed. |
| `Attempts` | `IReadOnlyList<string>` | One line per attempt, in order. |

## Members

| Member | Description |
|---|---|
| `static OpenResult OpenWithFallback(JuceEngineConfig config)` | Try the requested configuration, then walk to something that works. |

## Example

```csharp
var result = JuceEngineFactory.OpenWithFallback(new JuceEngineConfig
{
    OutputTypeName = "Windows Audio (Low Latency Mode)",
    SampleRate     = 48_000,
    BufferFrames   = 128,
});

if (result.Engine is null)
{
    foreach (string line in result.Attempts)
        log.LogWarning("audio open attempt: {Line}", line);
    throw new InvalidOperationException("No usable audio device.");
}

using JuceAudioEngine engine = result.Engine;
engine.Start();

log.LogInformation("opened {Device} on {Host} @ {Rate} Hz / {Frames} frames",
                   result.ConfigUsed.OutputDeviceId,
                   result.ConfigUsed.OutputTypeName,
                   engine.SampleRate, engine.BufferFrames);
```

## Notes

* **Log `Attempts`.** It is the only artefact that explains *why* the user's
  interface was skipped, and it is the first thing to ask for in a bug report.
* **`ConfigUsed` is the truth, not your input.** Persist that, not the config
  you asked for, if you are remembering the user's device between runs.
* You still own the returned engine — dispose it. `Engine` being `null` means
  there is nothing to dispose.
