# Hosting VST3 Plugins

JuceSharp can host VST3 effects on the input bus, and VST3 instruments on
an internal MIDI-driven bus. Both share the same lifecycle: load, attach,
optionally show the editor, dispose.

## Finding plugins

```csharp
foreach (var entry in JuceVst3Scanner.Scan())
    Console.WriteLine(entry.Display);      // "Helix Native  (74.3 MB)"
```

`JuceVst3Scanner.Scan(roots: null, maxDepth: 4, limit: 0)` walks
`JuceVst3Scanner.DefaultPaths()` plus any roots you supply and returns
`Vst3Entry(Name, Path, SizeBytes, LastWriteUtc)`. It is a **file-system walk,
not an instantiation** — it does not load anything, so it is cheap and safe.

## Loading a plugin

```csharp
using var amp = engine.LoadPlugin(@"C:\VST3\Helix Native.vst3");
```

`LoadPlugin` returns a [`JuceAudioPlugin`](../api/juce-audio-plugin.md). It
runs through JUCE's `AudioPluginFormatManager` on the engine's message
thread, so concurrent loads are serialised safely. A cold scan of a large amp
sim can take **25 seconds or more** — do it off your UI thread.

If the path doesn't exist or the plugin fails to scan, a
[`JuceAudioException`](../api/juce-audio-exception.md) is thrown with
`Status = JuceAudioStatus.Plugin`.

## Attaching to the input bus (effects)

```csharp
amp.AttachToInput();           // seat 0
amp.AttachToInput(player: 1);  // ABI v28: one seat's input chain
amp.Bypass = false;            // Bypass is a PROPERTY, not SetBypass(bool)
Console.WriteLine(amp.InputPlayer);   // which seat, or -1 when detached
```

Multiple effects can be attached — they form a serial chain in the order
they were attached, and they sit **after** the in-house DSP tone rig.

```csharp
using var amp  = engine.LoadPlugin(ampPath);    amp.AttachToInput();
using var rev  = engine.LoadPlugin(reverbPath); rev.AttachToInput();
// signal flow: input → drop tune → DSP rig → amp → rev → master
```

`Bypass`'s setter detaches and re-attaches the plugin; `DetachFromInput()` is
idempotent.

## Attaching to the instrument bus (MIDI synths)

```csharp
using var guitar = engine.LoadPlugin(@"C:\VST3\Ample Guitar SC.vst3");
guitar.AttachToInstrument();

engine.SendMidi(0x90, 60, 100);    // note-on
```

Only **one** instrument can be attached at a time. Attaching a second
instrument replaces the first (the old one is detached, not unloaded).

## Showing the editor

```csharp
amp.ShowEditor();     // no arguments — the title is the plugin's own
amp.HideEditor();

amp.EditorClosedByUser += () => Console.WriteLine("user closed the window");
```

JuceSharp creates a native top-level window on its own message thread, so
this works from console apps, services, MAUI apps — anything, with or
without a UI framework. `EditorClosedByUser` (ABI v14) fires on the message
thread after the user closes the window, so your UI's "editor open" toggle
does not go stale.

Disposing the plugin automatically closes the editor.

> ✅ **Tip**
> **Editor shutdown is crash-proof.** Many plugins (BIAS FX, Helix Native,
> Neural DSP, Ample Guitar, …) crash on DLL detach if their editor is alive
> when their plugin instance dies. JuceSharp tracks every loaded plugin and
> tears down editors **before** the engine — even if you forget to dispose
> the plugin. A warning is logged when this safety net kicks in.

## Parameters

```csharp
foreach (JuceParameterInfo p in amp.EnumerateParameters())
    Console.WriteLine($"{p.Name} = {amp.GetParameter(p.Index):F2}" +
                      (p.IsDiscrete ? $"  ({p.NumSteps} steps)" : ""));

JuceParameterInfo one = amp.GetParameterInfo(0);
amp.SetParameter(0, 0.75f);    // value is normalised [0, 1]
```

There is no `GetParameterName(int)`: names come from
`GetParameterInfo(i).Name`, or in bulk from `EnumerateParameters()`.
`JuceParameterInfo` is a `readonly record struct` of
`(Index, Name, DefaultValue, NumSteps)` with `IsDiscrete => NumSteps > 0`.

## Latency

```csharp
int samples = amp.LatencySamples;   // ABI v10
```

The plugin's own reported processing latency, in samples at the device rate.
Nothing compensates for it automatically — subtract it yourself if you are
aligning against a chart or a click.

## State persistence

```csharp
byte[] state = amp.SaveState();
File.WriteAllBytes("amp.state", state);

// later…
amp.LoadState(File.ReadAllBytes("amp.state"));
```

The methods are `SaveState()` / `LoadState(byte[])` — not `GetState`/`SetState`.
State is the plugin's own opaque blob; pass it back verbatim. The format
is plugin-specific (often a raw VST3 stream snapshot).

## Sandboxing a plugin in a child process (ABI v19)

A plugin that segfaults inside the audio callback takes your process with it.
The engine can instead run the input chain in `juce_vst_host.exe`, reached over
a shared-memory bridge:

```csharp
bool ok = engine.SetRemoteBridge(shmName, requestEventName, responseEventName,
                                 enable: true);

if (engine.TryGetRemoteBridgeStats(out uint processed, out uint timeouts, out uint heartbeat))
    Console.WriteLine($"{processed} blocks, {timeouts} timeouts, hb={heartbeat}");
```

You are responsible for launching the child and for naming the shared-memory
section and the two events consistently on both sides. `timeouts` climbing is
the signal that the child is not keeping up with the callback's deadline.

### Launching `juce_vst_host.exe` — what the managed API does not give you

The child ships next to `juce_sharp.dll` in `runtimes/win-x64/native/` (CMake's
POST_BUILD deploys it). It is **not** started for you, and the three names
`SetRemoteBridge` takes are not its whole command line:

```text
juce_vst_host.exe shm=<mapping> req=<event> resp=<event> pipe=<pipe-name>
                  [sr=<sample-rate>] [block=<frames>] [parent=<pid>]
```

All seven tokens are `key=value` and order-free. **The first four are required**
— missing any of them makes the child log
`missing required arguments (shm/req/resp/pipe)` and exit with code **2** —
and `pipe=` has no counterpart in the managed API at all: it is the control
channel, a **named pipe you create before spawning the child**, carrying
newline-delimited UTF-8 JSON operations (`ping`, `prepare`, `load` — which
takes an optional `component` index for a multi-component bundle since ABI v33 —
`unload`, `bypass`, `reorder`, `loadInstrument`, `unloadInstrument`,
`showEditor` / `hideEditor`, `getState`, `quit`). Audio never goes over it; audio is the shared-memory block.
`sr` and `block` default to 44100 / 512 when omitted.

The parent also has to **create the mapping and initialise its header first**:
`dcvb::SharedHeader` (`src/JuceSharp.Native/include/dotcharter_vst_bridge.h`)
begins with `magic` `'DCVB'` (`0x44435642`) and `version`
(`dcvb::kBridgeVersion`, **3** today — v3 appended the per-event MIDI sample
offsets after the instrument region, so the mapping is `dcvb::kTotalBytes`
= 197,704 bytes), and both sides refuse a version they do
not recognise rather than running on a half-understood layout. The child
publishes `childState = kChildReady` once it is up, bumps `heartbeat` on every
worker wakeup — a stalled heartbeat is how the parent notices a crash — and
`prepareBlockFrames` / `prepareGeneration` are how a device reconfigure reaches
a plugin that has already been prepared.

Editor windows belong to the child process, and a plugin that crashes takes
**only** the child down: the parent passes audio through dry and can respawn it.
That is the entire point of the feature.
