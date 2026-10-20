# Architecture

JuceSharp consists of a thin managed wrapper around a self-contained
native engine.

```
┌─────────────────────────────────────────────────────────────────────┐
│                       Your .NET application                         │
│                                                                     │
│  ┌────────────┐  ┌──────────────┐  ┌──────────────┐  ┌───────────┐  │
│  │ JuceAudio  │  │ JuceAudio    │  │ JuceAudio    │  │ JuceLog   │  │
│  │ Engine     │  │ Source       │  │ Plugin       │  │ (static)  │  │
│  └─────┬──────┘  └──────┬───────┘  └──────┬───────┘  └─────┬─────┘  │
│        │                │                 │                │        │
│        └────────────────┴─────────────────┴────────────────┘        │
│                            P/Invoke (C ABI)                         │
└──────────────────────────────────┬──────────────────────────────────┘
                                   │
                       ╔═══════════▼═══════════╗
                       ║     juce_sharp.dll    ║
                       ║                       ║
                       ║  ┌─────────────────┐  ║
                       ║  │ JuceHost        │  ║   ← message thread
                       ║  │ (own MM thread) │  ║
                       ║  └────────┬────────┘  ║
                       ║           │           ║
                       ║  ┌────────▼────────┐  ║
                       ║  │ AudioEngine     │  ║
                       ║  │ (mixer + graph) │  ║
                       ║  └────────┬────────┘  ║
                       ║           │           ║
                       ║  ┌────────▼────────┐  ║   ← audio thread
                       ║  │ AudioDeviceManager│ ║   (real-time, no GC)
                       ║  └────────┬────────┘  ║
                       ╚═══════════│═══════════╝
                                   │
                       ╔═══════════▼═══════════╗
                       ║ OS audio (ASIO/WASAPI ║
                       ║ /CoreAudio/ALSA/JACK) ║
                       ╚═══════════════════════╝
```

**Not in the diagram: a second process.** When the sandbox bridge is enabled
(`dca_engine_set_remote_bridge`, ABI v19), the input VST3 chain and the MIDI
instrument run in **`juce_vst_host.exe`** — a separate executable deployed next
to `juce_sharp.dll` — with audio over a shared-memory block and control over a
named pipe. The host application launches it; the engine only attaches. Its
command line, the JSON pipe protocol and the header the parent must initialise
are in [VST3 Hosting](../guides/vst3-hosting.md), § *Sandboxing a plugin in a
child process*.

## Threading

JuceSharp manages three thread classes inside the native DLL:

1. **Audio thread** — driven by the OS. Pulls blocks from the
   `AudioEngine`'s output mix. Never enters managed code, never
   allocates, never takes locks held by anything else.
2. **Message thread** (a.k.a. JuceHost MM thread) — created on first
   engine load. Hosts plugin editors and any JUCE component that
   requires `MessageManager::callAsync`. Calls from .NET into
   plugin-loading or editor-opening code are proxied onto this thread
   via `callSync`.
3. **Caller threads** — anything that calls into JuceSharp from managed code.
   `SendMidi` is lock-free on the consumer side (the audio thread never takes a
   lock for it), but the MIDI *producer* side is serialised by a message-thread
   mutex, and **not every property setter is lock-free**: `JuceAudioSource.Volume`
   and `.Mute` share a small `lock (_levelGate)` so a mute cannot lose a
   concurrent volume write.

The audio thread **never** calls back into managed code. Four delegate types
do cross native→managed, but none of them from the audio callback:
`dca_log_callback` and the ABI-v14 editor-closed callback fire on the message
thread, the ABI-v4 MIDI callback fires on the JUCE MIDI input thread, and the
ABI-v5 / v36 stem-progress callbacks fire on the thread that called the
separation export. See
[Threading Model](threading.md#plugin-authors-callbacks-from-native-to-managed).

## Memory ownership

Native handles are **not** reference-counted. Each handle-owning wrapper
(`JuceAudioEngine`, `JuceAudioSource`, `JuceAudioPlugin`, `JuceMidiInput` and
`JuceMidiOutput` — **five**, not four; `JuceDspChain` / `JuceDspNode` own none,
being views on the engine)
holds a `NativeHandleOwner` that acquires the handle around every P/Invoke and
releases it afterwards. `Dispose()` **drains**: it waits on the last outstanding
`Release` before destroying the handle, and on expiry it **leaks the handle and
logs at Error** rather than freeing it under a live call — the calls that
legitimately run long are exactly the ones a user reaches
(`TryShowAsioControlPanel` is a modal vendor dialog, `Reconfigure` reopens the
driver, a cold VST3 scan is 25 s+).

The drain is **per handle**, so the engine drains its children too.
`JuceAudioEngine` keeps a weak registry of every source, plugin, MIDI input and
MIDI output it hands out; `Dispose()` claims the engine handle (which closes the
registry — every factory acquires before it tracks), stops the device, disposes
each child, and only then calls `dca_engine_destroy`. A `JuceLoopRegion` is
reached through its owning source. MIDI inputs **and outputs** are additionally
held **strongly** (`_midiInputs` / `_midiOutputs`), because callers wire the
callback — or keep sending — without holding a reference of their own, and a
weak-only registry let the first gen-2 collection close the device silently.

A poll racing a dispose therefore surfaces as a caught
`ObjectDisposedException`, never a native crash. If you forget to dispose a
plugin, the engine still tears it down through the editor-safe sequence during
its own `Dispose()`.

## Plugin registry

`Engine` maintains a `std::vector<Plugin*>` protected by a mutex. Every
`dca_plugin_load_vst3` adds to it; every `dca_plugin_unload` removes
from it. `dca_engine_destroy` iterates the registry and force-tears any
orphaned plugins on the message thread **before** destroying the
engine. This is what prevents the BIAS-FX / Helix shutdown crash when
.NET forgets to unload.

## ABI versioning

Each release of JuceSharp bumps the integer returned by
`dca_abi_version()`. The managed `NativeMethods.ExpectedAbiVersion`
constant matches the wrapper's expectation. A mismatch throws
`JuceAbiMismatchException` (an `InvalidOperationException` subclass) from the
engine's constructor — that's
your "rebuild the DLL" signal.
