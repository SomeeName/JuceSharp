# Logging & Diagnostics

JuceSharp exposes three independent log paths:

1. **`ILogger` injection** — preferred for apps that already use
   `Microsoft.Extensions.Logging`.
2. **Console fallback** — automatic when no `ILogger` is supplied.
3. **Static `JuceLog.MessageReceived` event** — for code that wants
   to subscribe globally (analytics, test capture, …).

All three are wired off the same native callback. Subscribing to one
does **not** silence the others, so you can layer e.g. console + global
event without conflict.

## ILogger injection

```csharp
using Microsoft.Extensions.Logging;

using var lf  = LoggerFactory.Create(b => b.AddConsole().SetMinimumLevel(LogLevel.Debug));
var       log = lf.CreateLogger<JuceAudioEngine>();

using var engine = new JuceAudioEngine(config: null, logger: log);
```

Native levels are mapped as follows:

| Native level   | `LogLevel`      |
| -------------- | --------------- |
| `Trace`        | `Trace`         |
| `Debug`        | `Debug`         |
| `Info`         | `Information`   |
| `Warn`         | `Warning`       |
| `Error`        | `Error`         |

Messages are prefixed `[native]` and emitted under the category
`JuceSharp.JuceAudioEngine`.

## Console fallback

If `logger` is `null`, JuceSharp wires `JuceConsoleLogger` automatically.
Messages go to `Console.Out` (`Info` and below) or `Console.Error`
(`Warn`/`Error`). Format:

```
JUCE  INFO  JUCE engine started. Driver=Asio, sr=48000, buf=128, …
JUCE  WARN  Plugin editor closed at shutdown — caller forgot to dispose.
JUCE  ERROR Unable to load VST3 plugin: file not found.
```

Disable by setting `JuceEngineConfig.EnableConsoleLog = false` — useful
when you're piping logs elsewhere and don't want duplicate console output.

## Environment-variable override

Set `JUCESHARP_LOG_LEVEL=trace|debug|info|warn|error` to change the
**minimum** level emitted by the native side, regardless of which managed
sinks are connected. Useful for debugging in production builds without
recompiling.

```powershell
$env:JUCESHARP_LOG_LEVEL = 'debug'
dotnet run
```

## Static event

```csharp
JuceLog.MessageReceived += (level, message) =>
{
    if (level >= JuceLogLevel.Warn)
        Telemetry.Track("juce.warn", message);
};
```

`MessageReceived` is a multicast event raised on the native callback
thread. Handlers must be **fast and non-throwing** — exceptions are
caught and reported via a debug log line, but the engine itself never
crashes.

## Native version & ABI

```csharp
Console.WriteLine(JuceAudioEngine.NativeVersion);
// → "JuceSharp.Native 0.9.0 (ABI 34, JUCE 9.0.3)"   ← numbers move with the tree
```

The version comes from the CMake project version, the ABI number from
`DCA_ABI_VERSION` and the JUCE tag from the build — it is for logs; compatibility
is decided by the ABI check below, not by parsing this string.

JuceSharp asserts on construction that the native ABI version matches
`NativeMethods.ExpectedAbiVersion` exactly. A mismatch throws
**`JuceAbiMismatchException`** (an `InvalidOperationException` subclass, *not* a
`JuceAudioException`) with the
message `"JuceSharp.Native ABI mismatch: wrapper expects N, native is M.
Rebuild the native DLL."` See [ABI & Versioning Policy](../abi-policy.md).

## Engine readiness

There is no `GetReadyInfo()`. Read the three properties directly after
`Start()`:

```csharp
Console.WriteLine($"{engine.SampleRate} Hz, {engine.BufferFrames} frames, " +
                  $"{engine.OutputLatencySamples} samples out-latency");
```

All three are cheap and safe to poll for a UI status bar.
`OutputLatencySamples` returns 0 when no device is open.

## Dropout counters

`TryGetXrunStats` reports what the audio **callback** observed, which is not
something a managed poll could infer:

```csharp
if (engine.TryGetXrunStats(out var x))
    Console.WriteLine($"late={x.LateCallbacks} overruns={x.DeadlineOverruns} " +
                      $"near={x.NearMisses} device={x.DeviceXruns?.ToString() ?? "n/a"}");
```

See [Engine Health & Dropouts](health.md) — the three counters mean three
different faults, and `DeviceXruns` is `null` on any driver that cannot report.
