# Threading Model

JuceSharp manages four thread classes of its own — the audio thread, the
message thread, caller threads and the per-device MIDI-output dispatchers, one
section each below. One more thread delivers callbacks into your code without
being managed here: the **JUCE MIDI input thread**. (The stem-progress callback
is *not* a third — it runs on whichever caller thread invoked
`dca_stems_separate_file[2]`.) Both are tabulated under
[callbacks from native to managed](#plugin-authors-callbacks-from-native-to-managed).
Understanding the boundaries is essential for plugin / extension authors.

## 1. The audio thread

* Driven by the OS audio driver (ASIO, WASAPI, CoreAudio, …).
* Pulls fixed-size blocks (typically 64 — 512 frames) from
  `AudioEngine::process()`.
* **Real-time**. Anything that allocates, takes a non-RT lock, calls into
  the OS file system, or enters the CLR is a bug. JUCE will warn at
  runtime via its own real-time checker in debug builds.

What runs on it, in order: the per-seat input split and monitor/trim gains,
the **CSVD drop-tune shifter** (per seat since ABI v31), the **in-house DSP
tone rig** (one `DspChain` per seat, up to `DCA_MAX_PLAYERS`), the VST3 input
chain — in-process, or marshalled across the **shared-memory sandbox bridge**
to `juce_vst_host.exe` — the capture ring writes, the transports with Rubber
Band stretching, the shared stem-group stretcher, the **6-band master EQ
bank**, the legacy HP/LP/BP filters, the click bus, and the streaming-mirror
sink copy.

File→device sample-rate conversion is **not** on this thread: it happens once
at load, on the loading thread (`src/LoadResampler.h`).

## 2. The message thread (`JuceHost`)

* Created the first time `dca_engine_create` is called.
* Owns a `juce::ScopedJuceInitialiser_GUI` so any JUCE component
  (top-level windows, timers, async updates) Just Works.
* `JuceHost::callSync(fn)` proxies an arbitrary lambda from any thread
  onto this thread and blocks until it returns.

What runs on it: plugin loading, plugin destruction, editor open/close,
parameter changes that need to fire JUCE listeners, every line of
`emitLog`.

## 3. Caller threads

* Any thread of the host application that calls into JuceSharp.
* All managed setters (`SetMasterGain`, `SetMonitorGain`, …) write
  atomically to engine state — the audio thread picks the new value up
  on the next block.
* `SendMidi` writes to a ring drained on the audio thread. **Producers can be
  of arbitrary count, and since ABI v35 they SERIALISE on a small message-side
  mutex** (`AudioEngine::midiProducerMutex_`). The ring is a
  `juce::AbstractFifo`, which is SINGLE-producer / single-consumer: two
  concurrent `prepareToWrite`s compute from the same `validStart`, so both write
  the same slot and `finishedWrite` double-advances — one event lost, one slot
  of stale garbage forwarded to somebody's sampler, and no counter moves. The
  CONSUMER side stays entirely lock-free; the audio thread never takes that
  mutex, which is why it is not in `rt_lock_sites.txt`.
  Pinned by `RtConcurrentMidiProducersLoseNoEvents`.

## 4. MIDI-output dispatcher threads (ABI v34)

* **ONE per open `dca_midi_output_*` device**, started at
  `juce::Thread::Priority::highest`, and there is no upper bound on how many a
  host may open.
* It sleeps in ≤ 20 ms steps while its earliest deadline is far away, then
  **busy-YIELDS the last ~1.5 ms** (`MidiOutputDevice::kSpinAheadSeconds`)
  before every deadline. That spin is what buys the accuracy — a
  `Thread::wait(1)` is 1 ms only while some process on the box holds
  `timeBeginPeriod(1)` and ~15.6 ms otherwise — but **the cost is real and was
  undocumented**: for a stream of events more than ~660/s apart the spin is
  effectively continuous, and three open devices means three cores yielding at
  `THREAD_PRIORITY_HIGHEST` next to the audio callback.
* It calls `sendMessageNow` OUTSIDE the queue mutex, deliberately: the OS MIDI
  API is not bounded on a wedged interface, and holding the lock across it would
  make `send` — a UI-thread call — wait on the driver.
* **Closing it is a bounded join, and on expiry the device is LEAKED**, never
  `stopThread`ed (which would `TerminateThread` a thread parked inside the
  driver and then destroy the `juce::MidiOutput` under it).
* Nothing here is ever reached from the audio callback, and it is not meant to
  be.

## Calls that must block on the message thread

Internally, the native side wraps these in `JuceHost::callSync`:

* `dca_plugin_load_vst3`
* `dca_plugin_unload`
* `dca_plugin_show_editor`
* `dca_plugin_hide_editor`
* `dca_engine_destroy` (when forced to unload orphaned plugins)

The managed API never exposes the "async" alternative — the cost of a
context switch is acceptable here because these are control-rate calls
made at most a few times per session.

## The shutdown ordering rule

VST3 plugins almost always crash on DLL detach if their editor
`AudioProcessorEditor` is destroyed **after** the
`AudioPluginInstance`. The required order is:

1. Detach from any audio / instrument graph.
2. `editor.reset()` on the message thread.
3. `releaseResources()`.
4. Destroy the plugin instance.

`teardownPluginAndDelete()` enforces all four steps in that order, and
`Engine::~Engine` calls it for every plugin still in its registry. The
managed `JuceAudioPlugin.Dispose()` calls the same path through
`dca_plugin_unload`.

## Plugin authors: callbacks from native to managed

There are **four** delegate types handed from managed code to native, declared
`[UnmanagedFunctionPointer(CallingConvention.Cdecl)]` in `NativeMethods.cs`:

| Delegate | Since | Invoked on |
|---|---|---|
| `DcaLogCallback` | v1 | the message thread (`dca_set_log_callback`) |
| `DcaMidiCallback` | v4 | the **JUCE MIDI input thread** (`dca_midi_input_open`) |
| `DcaEditorClosedCallback` | v14 | the message thread, after the user closes a plugin editor |
| `DcaStemsProgressCallback` | v5 | the **thread that called `dca_stems_separate_file`** |
| `DcaStemsProgressCallback2` | v36 | the **thread that called `dca_stems_separate_file2`**. Returns `int`: non-zero = continue, `0` = cancel |

> ⚠️ **Warning**
> **Every one of these needs a catch-all body.** A reverse-P/Invoke that throws
> unwinds through C++ frames: that is a process kill, not a catchable error.
> `NativeCallbackSafetyTests.EveryDelegateHandedToNative_HasACatchAllBody`
> discovers the delegate types by reflection and lints both shapes — a variable
> assigned a lambda or method group, and a factory method returning one.

Three of the five are **not** on the message thread, which is what makes them
easy to get wrong: the MIDI callback runs on JUCE's MIDI input thread (do not
block, allocate or lock inside — copy the bytes and return), and both stem
progress callbacks run on whichever thread invoked the separation export — for
the managed wrapper, the dedicated background thread behind `SeparateAsync`.

If your application needs to do significant work in a handler, post
to a queue and process elsewhere. The native side does not block waiting for
the callback to return, but you should still be courteous to the thread you
are on.
