# JuceMidiOutput

Hardware MIDI **output** (ABI v34). The mirror of
[`JuceMidiInput`](juce-midi-input.md). Created by
`JuceAudioEngine.OpenMidiOutput(deviceIndex)`; dispose to close it.

Program against `IJuceMidiOutput` (ABI v35) rather than the concrete type — it
is the seventh test seam, and it exists because `IJuceAudioEngine.OpenMidiOutput`
used to return the sealed, internally-constructed `JuceMidiOutput`, which no fake
could satisfy.

```csharp
foreach (var (name, i) in JuceMidiOutput.Enumerate().Select((n, i) => (n, i)))
    Console.WriteLine($"{i}: {name}");

using var engine = new JuceAudioEngine();
using IJuceMidiOutput midi = engine.OpenMidiOutput(0);

// Now, asynchronously.
midi.Send(0x90, 60, 100);

// …or when output frame F reaches the EAR.
engine.TryGetRenderClock(out var frames, out _);
midi.SendAt(0x90, 62, 100, frames + engine.SampleRate / 2);
```

## Members

| Member | Notes |
|---|---|
| `static Enumerate()` | Device names in index order; the index is what `OpenMidiOutput` takes. Empty on a machine with no MIDI ports — never a one-entry list containing `""`. |
| `DeviceIndex` | What it was opened with. |
| `IsDisposed` | True once the native device has been claimed for destruction. |
| `MaxMessageBytes` | 3, and it is `MidiOutputDevice::kMaxMessageBytes` native-side. Short messages only. |
| `Send(status, data1, data2)` / `Send(ReadOnlySpan<byte>)` | Queue for the dispatcher. ASYNCHRONOUS — see the counters. |
| `SendAt(…, renderFrame)` ×2 | Queue with a deadline derived from the render clock PLUS the engine's output latency, so the note lines up with what the user HEARS and not with what the callback has just rendered. A negative frame, or an engine with no device open, means "as soon as possible". |
| `SentCount` | Messages the dispatcher actually put on the wire. |
| `DroppedCount` | Messages it THREW AWAY because its 4,096-entry queue was full (ABI v35). |
| `FailedCount` | Messages the DRIVER threw on (ABI v36). Distinct from `DroppedCount`: these reached `sendMessageNow` and did not come out the other side. `SentCount + FailedCount` is what `SentCount` alone reported before v36. |
| `ReadCounters()` | Sent + dropped, in one native call, so a caller comparing them cannot read them a queue-full apart. |
| `ReadAllCounters()` | All **three** (`Sent`, `Dropped`, `Failed`) in one native call (ABI v36, `dca_midi_output_get_counts3` — a second export rather than a third out-param, so every v35 caller keeps its signature). `FailedCount` delegates to this. `(0, 0, 0)` on a disposed output. |

## Five things worth knowing

1. **Nothing runs on your thread.** `juce::MidiOutput::sendBlockOfMessages`
   takes the output's `CriticalSection` on the CALLING thread and takes its
   deadlines in milliseconds, so it is unusable here. Instead each open device
   owns ONE dedicated dispatcher thread which sleeps while its earliest deadline
   is far away and BUSY-SPINS the last ~1.5 ms on the high-resolution counter —
   a `Thread::wait(1)` is 1 ms only while some process on the box holds
   `timeBeginPeriod(1)` and ~15.6 ms otherwise. See § 4 of
   [`../native/threading.md`](../native/threading.md) for what that costs.

2. **A batch that falls due in one wake goes out in DEADLINE order.** The queue
   itself is unsorted (insertion is O(1)); the due batch is sorted immediately
   before it is sent. Until ABI v35 it went out in ENQUEUE order, which is
   routine to hit because the dispatcher's sleep is capped at 20 ms.

3. **A full queue is a DROP, not a bad argument.** `Send` throws
   `JuceAudioException` carrying `JuceAudioStatus.Device` and `DroppedCount`
   moves. Before v35 both a saturated queue and a malformed message surfaced as
   `DCA_ERR_INVALID_ARG` — so the caller was told its arguments were wrong while
   the note-off it lost became a stuck note.

4. **SysEx is not supported.** `Send` throws `ArgumentOutOfRangeException` past
   `MaxMessageBytes` and `ArgumentException` for an empty message, rather than
   letting the native dispatcher discover the limit.

5. **Closing is bounded, and on expiry it LEAKS.** `Dispose` drains in-flight
   sends through `NativeHandleOwner` first — the native close joins the
   dispatcher and destroys the `juce::MidiOutput`, so closing under a live send
   is a use-after-free — and the native side joins the dispatcher with a bounded
   wait rather than `stopThread`, which would `TerminateThread` a thread parked
   inside the OS MIDI API. A leak beats a use-after-free.

The engine ALSO holds a strong reference to every output it hands out, until the
output or the engine is disposed: an app that wires a scheduler to an output and
keeps no handle had its device closed by the first gen-2 collection.
