# MIDI Instruments

JuceSharp hosts a single VST3i instrument on a dedicated MIDI bus, and
exposes a lock-free pump for short MIDI messages.

## Sending short messages

```csharp
engine.SendMidi(status: 0x90, data1: 60, data2: 100);   // note-on C4 vel 100
engine.SendMidi(0xB0, 64, 127);                         // CC#64 (sustain) on
engine.SendMidi(0xE0, 0x00, 0x40);                      // pitch-bend centre
```

`SendMidi` is non-blocking and safe to call from any thread (including a
UI input handler). Internally it pushes onto a lock-free ring of
**`kMidiQueueDepth` = 512 entries** — entries, not bytes: that is the number to
reason about when you are asking whether a burst can overflow. The audio thread
drains it; the producer side is serialised by a message-thread mutex the audio
thread never takes.

## Routing to an instrument plugin

```csharp
using var guitar = engine.LoadPlugin(@"C:\VST3\Ample Guitar.vst3");
guitar.AttachToInstrument();
```

From that point onwards every `SendMidi` is delivered to the plugin during
the next audio block.

## Multiple instruments

Only one instrument is wired up at a time. To swap, just attach the new
one — the old plugin is detached but not disposed (so its editor stays
open, parameters persist, etc.).

## Sample-accurate scheduling (ABI v34)

`SendMidi(status, data1, data2)` stamps the event at **sample 0 of whichever
block happens to drain it** — up to one whole buffer of skew (5–10 ms) on every
note. The four-argument overload names the sample instead:

```csharp
engine.TryGetRenderClock(out long frames, out _);
engine.SendMidi(0x90, 60, 100, frames + 137);   // note-on, 137 samples in
```

Frames are absolute output-timeline frames in the same domain
`TryGetRenderClock` publishes. The event is **parked** on the audio thread until
the block that contains its frame and then placed at its exact offset inside it
— the same mechanism scheduled clicks use — so:

* scheduling **ahead** is legal (up to 5 s; past that an event is dropped rather
  than held, because the only way to be that far ahead is a render clock that
  rewound under a device restart);
* an out-of-order send cannot hold back the events behind it, because dispatch
  is by frame and not by arrival;
* a **negative** frame means "as soon as possible" and is bit-for-bit the
  three-argument overload.

```csharp
var (scheduled, late, dropped) = engine.GetMidiTimingStats();
```

`late` is the number worth watching: it counts events whose frame had already
gone past when their block ran (they are placed at sample 0 instead), and it is
the only signal that your scheduling horizon is shorter than the engine's own
latency.

The **out-of-process VST sandbox** is scheduled the same way: since bridge v3
the engine parks the event until its block and the shared mapping carries its
sample offset to `juce_vst_host.exe`, so a sandboxed instrument plays it on the
same sample an in-process one would.

## Latency

Without scheduling, MIDI dispatch latency = `BufferFrames / SampleRate` (e.g.
256 / 48000 = ~5.3 ms). The reported value is logged on engine start:

```
[Audio] Audio engine ready. … midiDelay=5ms.
```

## MIDI input (hardware)

```csharp
IReadOnlyList<string> names = JuceAudioEngine.EnumerateMidiInputs();

using JuceMidiInput midi = engine.OpenMidiInput(0, msg =>
{
    // msg is a JuceMidiMessage: TimestampSeconds, Data,
    // plus decoded Status / Data1 / Data2 / Channel / Command.
    if (msg.Command == 0x90 && msg.Data2 > 0)
        engine.SendMidi(msg.Status, msg.Data1, msg.Data2);   // forward to the VSTi
});
```

`JuceAudioEngine.EnumerateMidiInputs()` is static and forwards to
`JuceMidiInput.Enumerate()`; the index you pass to `OpenMidiInput` is the index
into that list.

> ⚠️ **Warning**
> **Your handler runs on the JUCE MIDI input thread, not the message thread and
> not your UI thread.** Do not block, allocate or take locks in it — copy what
> you need and return. It must also not throw: a reverse-P/Invoke that unwinds
> into C++ frames kills the process. The wrapper installs a catch-all around
> your delegate, but keep it cheap regardless.

Dispose the `JuceMidiInput` to stop and release the device. The engine holds
each open input **strongly** until you or it disposes, so a device you wired a
callback to and kept no reference for will not be closed by a GC.

## MIDI output (hardware, ABI v34)

```csharp
IReadOnlyList<string> names = JuceAudioEngine.EnumerateMidiOutputs();
using IJuceMidiOutput outp  = engine.OpenMidiOutput(0);   // the v35 test seam

outp.Send(0x90, 60, 100);                       // as soon as the dispatcher can

engine.TryGetRenderClock(out long frames, out _);
outp.SendAt(0x90, 60, 100, frames + 480);       // when that frame reaches the EAR
```

`SendAt`'s deadline carries the engine's **output latency** (master limiter
included), so a note sent to a hardware synth lines up with what the user hears
rather than with what the callback has just rendered.

Both calls are **asynchronous**. Every message goes through one dedicated
high-priority dispatcher thread inside the native device, never yours, so a
wedged MIDI interface can never block a UI thread — use `SentCount` if you need
to know something actually reached the wire, and `DroppedCount` (ABI v35) to know
something did NOT: a full 4,096-entry queue throws `JuceAudioException` carrying
`JuceAudioStatus.Device` and moves that counter. Before v35 a drop was reported
as a bad ARGUMENT, which is what the caller would then believe, while the
note-off it lost became a stuck note.

Messages that fall due in one dispatcher wake go on the wire in DEADLINE order
(the queue itself is unsorted; the due batch is sorted just before it is sent).
See [`../api/juce-midi-output.md`](../api/juce-midi-output.md) and § 4 of
[`../native/threading.md`](../native/threading.md) — one high-priority spinner
per open device is a real cost and it is now written down.

> **Why not `juce::MidiOutput::sendBlockOfMessages`?**
> It is the obvious answer and it is not usable here. It takes the output's
> `CriticalSection` on the *calling* thread to splice into its pending list, and
> it takes its deadlines in `Time::getMillisecondCounter()` **milliseconds** —
> which throws away the whole point of a sample-accurate schedule. The
> dispatcher here sleeps while its earliest deadline is far away and busy-spins
> the last ~1.5 ms on the high-resolution counter, because a `Thread::wait(1)`
> is 1 ms only while some process on the box holds `timeBeginPeriod(1)` and
> ~15.6 ms otherwise.

Lifetime is the same contract as the input: dispose the `JuceMidiOutput`, or the
engine, to close the device. The engine holds each open output **strongly**, so
one you wired a scheduler to and kept no reference for will not be closed by a
GC.
