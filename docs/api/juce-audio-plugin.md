# JuceAudioPlugin

`sealed class JuceAudioPlugin : IDisposable, INativeChild, IJuceAudioPlugin`

`INativeChild` is what lets the engine wait on this plugin's native destruction
during its own teardown even after the wrapper has been collected;
`IJuceAudioPlugin` is the test seam.

Safe RAII wrapper around a loaded VST3 plugin. Obtain one from
[`JuceAudioEngine.LoadPlugin`](juce-audio-engine.md).

```csharp
using var amp = engine.LoadPlugin(@"C:\VST3\Helix Native.vst3");
amp.AttachToInput();
amp.ShowEditor();
```

## Properties

| Member       | Description                                                  |
| ------------ | ------------------------------------------------------------ |
| `Path`           | `string` — the path the plugin was loaded from.              |
| `IsDisposed`     | `bool` — `true` after `Dispose()`.                           |
| `Handle`         | `nint` — raw native handle for interop with legacy `nint`-based APIs. `[EditorBrowsable(Never)]`, and **unpinned**: it is not kept alive for you, so a call racing `Dispose` is a use-after-free. Prefer the typed members. |
| `Bypass`         | `bool { get; set; }` — detaches / re-attaches the plugin from its input chain. It is a **property**, not `SetBypass(bool)`. The getter asks the engine (`InputPlayer < 0`), so `DetachFromInput()` and `AttachToInput(seat)` move it too; a plugin never attached to an input chain reads `true`. |
| `InputPlayer`    | `int` — which seat the plugin is attached to, or `-1` when detached (ABI v28). |
| `ParameterCount` | `int` — number of automatable parameters (ABI v4).           |
| `LatencySamples` | `int` — the plugin's own reported processing latency (ABI v10). Nothing compensates for it automatically. |

## Events

| Member | Description |
|---|---|
| `event Action? EditorClosedByUser` | Fired on the JUCE message thread after the **user** closes the editor window (ABI v14), so a host toggle does not go stale. |

## Methods

| Member                   | Description                                              |
| ------------------------ | -------------------------------------------------------- |
| `AttachToInput(int player = 0)` | Insert into a seat's input (effects) chain.       |
| `DetachFromInput()`      | Remove from the input chain. Idempotent.                 |
| `AttachToInstrument()`   | Wire up as the engine's MIDI instrument.                 |
| `DetachFromInstrument()` | Remove from the instrument bus.                          |
| `ShowEditor()`           | Open the plugin's editor window on the JUCE message thread. Takes **no arguments** — the title is the plugin's own. |
| `HideEditor()`           | Close the editor.                                        |
| `GetParameter(int index)` → `float` | Read a normalised 0…1 value.                  |
| `SetParameter(int index, float value)` | Write a normalised 0…1 value.              |
| `GetParameterInfo(int index)` → `JuceParameterInfo` | Name, default and step count. There is no `GetParameterName`. |
| `EnumerateParameters()` → `JuceParameterInfo[]` | Every parameter in one call.  |
| `SaveState()` → `byte[]` | The plugin's opaque state blob. Not `GetState`. |
| `LoadState(byte[] data)` | Push a saved blob back. Not `SetState`.                  |
| `Dispose()`              | Detach + unload + destroy editor. Safe to call twice.    |

`JuceParameterInfo` is
`readonly record struct (int Index, string Name, float DefaultValue, int NumSteps)`
with `IsDiscrete => NumSteps > 0`.

## Disposal semantics

`Dispose()` performs the following sequence on the JUCE message thread:

1. Detach from input and instrument graphs.
2. Reset the editor pointer (so it dies before the processor instance —
   the order most plugins require).
3. Release the audio resources.
4. Destroy the `AudioPluginInstance`.
5. Remove from the engine's plugin registry.

If you forget to dispose, the engine catches it in
`JuceAudioEngine.Dispose()` and runs the same sequence — logging a
`WARN` line so you know.

## Errors

* `ArgumentException` — passing a zero handle to the constructor.
* `ObjectDisposedException` — calling any method after `Dispose()`.
* [`JuceAudioException`](juce-audio-exception.md) — `AttachToInput`
  or `AttachToInstrument` failed.

Editor failures (e.g. plugin lacks an editor) emit a WARN log but do not
throw.
