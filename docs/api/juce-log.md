# JuceLog & JuceConsoleLogger

JuceSharp offers two static logging helpers in addition to the
`ILogger<JuceAudioEngine>` parameter passed to the engine constructor.

## `JuceLog` (static)

Global event-based hook. Useful when you don't own the
`JuceAudioEngine` (e.g. inside a plugin, in test infrastructure, or for
sending warnings to telemetry).

```csharp
public static class JuceLog
{
    public static event Action<JuceLogLevel, string>? MessageReceived;
    public static JuceLogLevel MinimumLevel { get; set; }
}
```

### `MinimumLevel`

Filter applied **before** invoking subscribers (and before the
`ILogger`). Defaults to whatever the `JUCESHARP_LOG_LEVEL` environment
variable says (`trace`, `debug`, `info`, `warn`, `error`), or `Trace`
if the variable is unset.

### `MessageReceived`

Multicast event raised once per native log line. Handlers must be
**fast and non-throwing**. Exceptions are caught, logged to
`System.Diagnostics.Debug`, and silently dropped — the engine itself
never crashes.

### Example

```csharp
JuceLog.MessageReceived += (level, msg) =>
{
    if (level >= JuceLogLevel.Warn)
        Telemetry.Track("juce.warn", msg);
};
```

## `JuceConsoleLogger` (ILogger)

Default sink when no `ILogger` is supplied and
`JuceEngineConfig.EnableConsoleLog` is `true`. Format:

```
JUCE  INFO   JUCE engine started. Driver=Asio, sr=48000, buf=128, ...
JUCE  WARN   Plugin editor closed at shutdown -- caller forgot to dispose.
JUCE  ERROR  Unable to load VST3 plugin: file not found.
```

* `Trace` is filtered out (use `JuceLog.MessageReceived` for `Trace`).
* `Information` and below go to `Console.Out`.
* `Warning` and above go to `Console.Error`.

You can also use it directly from your own logging code:

```csharp
JuceConsoleLogger.Instance.LogWarning("manual line");
```

## `JuceLogLevel` enum

```csharp
public enum JuceLogLevel
{
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
}
```
