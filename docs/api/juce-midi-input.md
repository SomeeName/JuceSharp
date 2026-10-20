# JuceMidiInput & JuceMidiMessage

Hardware MIDI **input** (ABI v4). For the OUTPUT side, added in ABI v34, see
[`juce-midi-output.md`](juce-midi-output.md) — this page said "there is no MIDI
output anywhere in this ABI" until v35.

## `JuceMidiMessage`

`readonly record struct JuceMidiMessage(double TimestampSeconds, byte[] Data)`

| Member | Type | Description |
|---|---|---|
| `TimestampSeconds` | `double` | In the **driver's** clock domain. Useful for relative ordering, not for wall time. |
| `Data` | `byte[]` | The raw message bytes. |
| `Status` | `byte` | `Data[0]`, or `0` for an empty message. |
| `Data1` | `byte` | `Data[1]`, or `0`. |
| `Data2` | `byte` | `Data[2]`, or `0`. |
| `Channel` | `int` | `Status & 0x0F`. |
| `Command` | `int` | `Status & 0xF0`. |

The decoded accessors are bounds-safe, so a running-status or SysEx message
will not throw — it will just report zeros for the bytes it does not have.

## `JuceMidiInput`

`sealed class JuceMidiInput : IDisposable`

Created by `JuceAudioEngine.OpenMidiInput(int, Action<JuceMidiMessage>)`; the
constructor is internal. Dispose to stop and release the device.

| Member | Description |
|---|---|
| `int DeviceIndex` | The index this input was opened with. |
| `bool IsDisposed` | `true` once the handle is gone. |
| `static IReadOnlyList<string> Enumerate()` | Device names. `JuceAudioEngine.EnumerateMidiInputs()` forwards here. |
| `void Dispose()` | Stop and release. Waits for a dispatch already inside your handler before `dca_midi_input_close` deletes the device — but the wait is **bounded at 60 s** (`NativeHandleOwner`). On expiry the handle is **leaked and logged at `Error`** and the close is skipped: a leak beats a use-after-free. So a handler that never returns does not hold the close open for ever; it costs you the device and a log line. |

## Example

```csharp
IReadOnlyList<string> names = JuceMidiInput.Enumerate();
for (int i = 0; i < names.Count; i++)
    Console.WriteLine($"[{i}] {names[i]}");

using JuceMidiInput midi = engine.OpenMidiInput(0, msg =>
{
    if (msg.Command == 0x90 && msg.Data2 > 0)
        engine.SendMidi(msg.Status, msg.Data1, msg.Data2);   // forward to the VSTi
});
```

## Notes

> ⚠️ **Warning**
> **Your handler runs on the JUCE MIDI input thread** — not the message
> thread, not your UI thread. Do not block, allocate or take locks in it: copy
> what you need and return. It must also not throw; a reverse-P/Invoke that
> unwinds into C++ frames is a process kill, not a catchable error. The wrapper
> installs a catch-all around your delegate, but keep it cheap regardless.
> A dispatch that arrives after `Dispose()` has claimed the handle returns
> without calling your delegate at all.

* **The engine roots every input it opens.** Applications typically wire the
  callback and keep no reference; a weak-only registry let the first gen-2
  collection close the device silently. The engine now holds each open input
  strongly until you dispose it or the engine is disposed.
* The index passed to `OpenMidiInput` is an index into `Enumerate()`'s list.
  Device order can change when hardware is plugged in — re-enumerate rather
  than persisting the index.
