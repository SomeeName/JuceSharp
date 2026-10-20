# JuceAudioException

`sealed class JuceAudioException : Exception`

Thrown when a native API call returns a non-`Ok` status.

## Properties

| Property      | Type               | Description                                            |
| ------------- | ------------------ | ------------------------------------------------------ |
| `Status`      | `JuceAudioStatus`  | Typed status enum.                                     |
| `StatusCode`  | `int`              | Raw native status code (compat shim).                  |
| `Operation`   | `string`           | Friendly name of the failed entry point (e.g. `"plugin_load_vst3"`). |

## `JuceAudioStatus` enum

```csharp
public enum JuceAudioStatus
{
    Ok             = 0,   // never seen on an exception
    InvalidArg     = 1,
    NotInitialized = 2,
    Device         = 3,
    Decode         = 4,
    Plugin         = 5,
    Cancelled      = 6,   // ABI v36 — NOT a failure; see below
    Internal       = 99,
}
```

> **`Cancelled` is not an error.** It is the one value that must not be treated
> as one: the wrapper maps it to `OperationCanceledException` rather than to a
> `JuceAudioException`, because a consumer that sees "the stem separation
> failed" falls through to the next backend — which is exactly the wrong thing
> to do after the user pressed Cancel. It is reached only from
> `dca_stems_separate_file2`.

## Constructing one outside the wrapper

```csharp
new JuceAudioException(JuceAudioStatus.Decode, "dca_stems_separate_file");
new JuceAudioException("node 7 has no 'ir' parameter",   // detail FIRST
                       JuceAudioStatus.InvalidArg,
                       "dca_engine_dsp_get_node_string");
```

The status sits **between** the two strings on purpose. The three-argument ctor
was `(status, operation, message = null)` for one release — two adjacent
`string`s, so transposing them compiled cleanly and produced an exception whose
`Operation` was a sentence. Nothing but a reader could have caught that.

## Typical use

```csharp
try
{
    using var amp = engine.LoadPlugin(path);
}
catch (JuceAudioException ex) when (ex.Status == JuceAudioStatus.Plugin)
{
    // plugin failed to scan / instantiate
}
catch (JuceAudioException ex) when (ex.Status == JuceAudioStatus.Device)
{
    // engine lost the device
}
```

## ToString override

`ToString()` adds an `Operation=..., Status=...` line after the standard
exception text. Handy when piping to log aggregators that flatten
exceptions to a single string.
