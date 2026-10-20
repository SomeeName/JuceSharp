# Threading (managed API)

What a consumer may call from where, which of your delegates runs on which
thread, and what the wrapper guarantees when a poll races a dispose. The native
side's own thread classes — the audio callback, the JUCE message thread, the
MIDI-output dispatchers — are in
[Threading Model](../native/threading.md); this page is the managed contract on
top of them.

Everything below is taken from the code and its XML docs. Where the code does
not state a rule, this page says **unspecified** rather than inventing one.

## The short version

| You are calling | From |
|---|---|
| `JuceDeviceCatalog.Enumerate`, `JuceDspModuleCatalog.Modules` | any thread; no engine, no device |
| engine setters (`SetMasterGain`, `SetMonitorGain`, `SetInputGain`, `SetEqBand`, …) | any thread — they write engine state the audio thread picks up on the next block |
| `JuceDspChain` / `JuceDspNode` (add, remove, move, bypass, params) | any thread. "Safe from any thread and safe against a running device, but not a real-time API" |
| `JuceAudioSource` transport (`Play`, `Stop`, `Seek`, `Volume`, `TempoRatio`) | unspecified in the code beyond the disposal guarantee below; in practice one owner thread |
| `OpenFile`, `LoadPlugin`, `Reconfigure`, `SetDeviceType`, `TryShowAsioControlPanel` | **not** the UI thread — seconds-long. Use the `…Async` twins |
| `JuceDspEditScope` | "not thread-safe and not meant to be: it represents one caller's transaction on one seat" |
| anything at all | never from an audio callback of your own |

## The seconds-long calls, and the worker behind `…Async`

`OpenFileAsync`, `LoadPluginAsync`, `ReconfigureAsync`, `SetDeviceTypeAsync` and
`ShowAsioControlPanelAsync` marshal onto **one dedicated long-running thread per
engine**, created lazily on first use and shut down in `Dispose()` before the
handle drain. It is deliberately **not** the thread pool: a cold VST3 scan
measures 25 s+ and the ASIO panel is modal with no upper bound, and parking a
pool thread for that long makes the pool inject threads while every other async
workload in the host starves.

Two consequences that are contract, not implementation detail:

* **The queue is serial.** A second `ReconfigureAsync` waits for the first —
  two device changes cannot overlap.
* **Cancellation is honoured before the call starts, and not after.** A token
  already cancelled never touches native; one cancelled while the item is still
  queued is honoured at dispatch. None of these native exports is interruptible
  once entered, so a token cancelled mid-call is ignored and the task completes
  with the real result.

The static offline helpers (`GenerateWaveformAsync`, `RenderToWavAsync`) share
one process-wide worker; it has no lifetime that could own it and is never shut
down.

The synchronous members remain the right call from a background thread of your
own. The async ones exist for the UI thread.

## Callbacks: who calls you, and on what

The wrapper hands exactly five delegate types to native, plus two managed-side
events. (Four until ABI v36 added `DcaStemsProgressCallback2`, whose contract is
the odd one out — see its row.)

| Your code | Runs on | You may |
|---|---|---|
| `JuceLog.MessageReceived` | the thread that produced the line — usually the JUCE message thread | not block. Exceptions are caught per-subscriber and reported via `Debug.WriteLine`; one throwing handler no longer stops the others |
| `JuceMidiSpanHandler` / `Action<JuceMidiMessage>` from `OpenMidiInput` | the **JUCE MIDI input thread** | copy the bytes and return. Do not block, allocate or lock |
| `JuceAudioPlugin.EditorClosedByUser` | the **JUCE message thread**, after the user closed the editor | marshal to your UI thread; do not block |
| stem-separation progress (`Action<float,string>`), v5 `DcaStemsProgressCallback` | the **thread that called `dca_stems_separate_file`** — the dedicated background thread behind `SeparateAsync` | post to a queue; see below |
| stem-separation progress, v36 `DcaStemsProgressCallback2` | the same thread | the same — **but this one returns a value**: non-zero = continue, `0` = cancel. The wrapper answers `0` only when your `CancellationToken` is signalled; a handler that THROWS does *not* cancel (the wrapper logs it and returns "continue"), because a broken observer must not tear down a multi-minute run |
| `JuceLoopRegion`'s seek-back | a **thread-pool thread** (`System.Threading.Timer`) | nothing of your own runs here — listed so you know the source seek is not on your thread |

**An exception escaping a delegate handed to native unwinds through C++ frames:
that is a process kill, not a catchable error.** The wrapper wraps each of the
five in a catch-all for you (pinned by
`NativeCallbackSafetyTests.EveryDelegateHandedToNative_HasACatchAllBody`), but
that safety net ends where your own handler's `catch` should have been: a
swallowed exception is a lost MIDI event or a lost progress tick.

The span overload of `OpenMidiInput` borrows the native buffer **in place** —
the `ReadOnlySpan<byte>` is valid only for the duration of the call. Copy
anything you keep. In exchange nothing is allocated per message, which matters
because a swept CC or a held sustain pedal produces hundreds of events a second
straight into gen0 from a driver thread.

```csharp
using var midi = engine.OpenMidiInput(0, (timestampSeconds, data) =>
{
    // Copy — the span points at native memory the driver owns.
    var copy = data.ToArray();
    _queue.Enqueue((timestampSeconds, copy));   // and do the real work elsewhere
});
```

`JuceStemSeparation.SeparateWithProgressAsync` exists for exactly the same reason on the
stems side: the native progress callback writes into a bounded channel, so a
slow consumer pays in stale fractions instead of back-pressuring inference.

## Polling, not events

**There is no position, level or meter event anywhere in the wrapper.** The one
managed event on the whole surface is `JuceAudioPlugin.EditorClosedByUser`;
`JuceLog.MessageReceived` is the one static hook. Everything else is a property
you read when you want it:

* `JuceAudioSource.Position` / `AudiblePosition` — the transport read head, and
  the head compensated for the stretcher's read-ahead. Draw playheads from
  `AudiblePosition`.
* `engine.GetAndResetInputPeak()`, `engine.MasterLimiterReductionDb`,
  `JuceDspChain.TryReadMeters` — level data.
* `engine.TryGetXrunStats`, `engine.TryGetMidiTimingStats` — health counters.

That is a deliberate design, not a gap: nothing is queued and no callback
crosses the ABI, so a host that never reads a meter pays nothing for it. **The
coalescing is therefore yours to do.** Poll on one timer at your redraw rate
(4–10 Hz for text, up to your frame rate for a meter), read each value once, and
push one update — do not start a timer per widget, and do not poll from inside a
render pass.

`JuceDspChain.TryReadMeters(Span<JuceDspNodeMeter>, out int)` is one P/Invoke for
the whole chain in chain order, which is the shape to copy: one call per frame,
not one per node.

Node meters are **poll-only and OFF until asked for** (`chain.MeteringEnabled`);
see [Real-time constraints](realtime-constraints.md#meters-are-poll-only-and-off-until-asked-for).

## Dispose while a call is in flight

Every handle-owning wrapper — `JuceAudioEngine`, `JuceAudioSource`,
`JuceAudioPlugin`, `JuceMidiInput`, `JuceMidiOutput` — routes each P/Invoke
through a `NativeHandleOwner` that pins the handle for the duration of the call.
`Dispose` claims the handle (so no new call can start) and then **waits for
every in-flight call to return** before destroying it.

The guarantee you can rely on:

* A poll racing a dispose fails with **`ObjectDisposedException`**, never a
  native crash. Some members instead return a neutral value rather than throw —
  see [Failure modes](failure-modes.md#value-returning-getters-and-their-sentinels).
* If a call outlives the drain deadline (60 s), the handle is **leaked and
  logged at `Error`** rather than freed under the live call. A leak beats a
  use-after-free. The calls that legitimately reach that ceiling are the modal
  ASIO panel, a device reopen and a cold VST3 scan.
* `JuceAudioEngine.Dispose()` claims its own handle first, stops the device,
  disposes every child it handed out, and only then destroys the engine — so
  you do not have to dispose children first.
* Disposing the engine while the ASIO panel is open **still blocks**: nothing
  can cancel a modal vendor dialog. Close it first.

`JuceDspChain` and `JuceDspNode` own no handle of their own — they are views on
the engine and every call pins the *engine's* handle, so the engine's drain
already covers them.

## The finalizer thread

Two types do real work in a finalizer, and both are recovery paths, not the
intended one:

* **`JuceDspEditScope`** aborts an open scope and logs at `Error`. Until it
  runs, that seat's rig publishes nothing to the audio callback and every edit
  piles up in the shadow chain while returning success. A finalizer runs at a
  GC-dependent moment — use `using`.
* **`JuceMidiInput`**'s finalizer closes the device with a **zero** wait
  deadline: one finalizer thread serves the whole process and may not block on
  a drain.

A throwing finalizer is a process-wide `FailFast`, so every one of them is a
catch-all over a possibly partially-constructed instance
(`FinalizerSafetyTests`). Nothing of yours runs on that thread — but a
`JuceLog.MessageReceived` handler can be invoked from it, via the Error line
above.

## What is unspecified

* Whether two threads may drive the **same `JuceAudioSource`** transport
  concurrently. The code guarantees only that neither will crash against a
  dispose; there is no documented ordering. Own a source from one thread.
* Whether two threads may mutate the **same `JuceDspChain`** concurrently.
  Individual calls are safe from any thread; an interleaving of two callers'
  structure edits is not described anywhere, and `BeginEdit` is explicitly one
  caller's transaction.
* The sandbox-bridge leg of `SendMidi` carries no per-event sample offset — a
  documented gap, not an oversight.
