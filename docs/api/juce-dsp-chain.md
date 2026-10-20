# The DSP tone rig from managed code

`JuceDspChain` is the managed face of the engine's in-house tone rig — a
reorderable effect chain applied to a seat's guitar **input**, before any user
VST3 input chain. 100 module types in a default build (57 originals plus the 43
of the 2026-09 gear wave — see the `gear-*` guides; `namamp` is the one neural
module and sits behind `-DDOTCHARTER_DSP_NEURAL`, which is ON by default, so a
`NEURAL=OFF` build has **99** — which is why you enumerate the catalogue instead
of assuming a count), addressed by string id, with every parameter
described by a catalogue so a generic pedalboard UI needs no per-module code.

Everything here is in the `JuceSharp.Dsp` namespace and ships in the `JuceSharp`
package. Before 0.8.x none of it existed: nineteen of the header's rig exports
had no P/Invoke declaration at all and the rest were declared and wrapped by
nothing, so a consumer that wanted the rig re-declared the C ABI itself.

## The types

| Type | What it is |
|---|---|
| `JuceDspModuleCatalog` | Static palette. `Modules` / `Find(id)`. **No engine required** — build your UI before a device is open. Read once from native and cached for the process. |
| `JuceDspModuleInfo` | One catalogue entry: `Id`, `DisplayName`, `Category`, `Params`, `StringParams`, `SupportsSidechain`. |
| `JuceDspParamInfo` | One float param: `Id`, `Name`, `Min`, `Max`, `Default`, `Step`, `Labels`, plus `IsDiscrete` / `IsNamed` / `IsOrdinal` and `Describe(value)`. |
| `JuceDspStringParamInfo` | One string param: `Id`, `Name`, `FileFilter` (a `;`-separated wildcard list for a file picker). |
| `JuceDspChain` | One seat's rig, from `engine.GetDspChain(player)`. Node ids are engine-global, and the `int nodeId` overloads (`RemoveNode`, `MoveNode`, `TryReadMeter`) reject an id belonging to another seat with `ArgumentOutOfRangeException` — use `GetDspChain(thatSeat)`. `AddNode`, `AddNodeBypassed`, `RemoveNode`, `MoveNode`, `Clear`, `Nodes`, `NodeIds()`, `NodeCount`, `TryGetNode`, `LatencySamples`, `MeteringEnabled`, `TryReadMeter` / `TryReadMeters`, `BeginEdit()` / `IsEditing`, `BranchLatencyCompensation`, and three **instance** `ProcessOffline` overloads — interleaved, stereo-planar, and the automated one taking `JuceDspAutomationEvent`s. None of them is static: an offline render still runs this seat's rig. |
| `JuceDspEditScope` | An open edit scope (ABI v34). `Commit()`, `Abort()`, `IsClosed`, `Player`. **Dispose without `Commit()` aborts.** |
| `JuceDspAutomationEvent` | One `{Frame, NodeId, Param, Value}` change for an automated offline render (ABI v34). Build it with `JuceDspAutomationEvent.Create`. |
| `JuceDspNode` | One pedal. `Id`, `TypeId` (two P/Invokes per read — there is no cached fast path; cache it caller-side if a UI reads it per frame), `Player`, `Bypass`, `GetParam` / `SetParam`, `GetString` / `SetString`, `Branch`, `Sidechain`, `LatencySamples`, `Meter`, `Remove()`, `MoveTo(index)`. |
| `JuceDspSidechainSource` | What a keyed node's detector listens to (ABI v34): `None`, `SeatInput`, `SongMix`. |
| `JuceDspNodeMeter` | `InputPeak`, `InputRms`, `OutputPeak`, `OutputRms` — **linear** amplitudes, never dB. |

`JuceDspMeters` is the older raw-`nint` façade over the v30 meters. It still
works and is not obsolete (DotCharter calls it), but it cannot pin the engine
handle; prefer the chain.

## Twenty lines

```csharp
using JuceSharp;
using JuceSharp.Dsp;

// The palette needs no engine at all.
foreach (var m in JuceDspModuleCatalog.Modules)
    Console.WriteLine($"{m.Id,-14} {m.DisplayName} ({m.Params.Count} params)");

using var engine = new JuceAudioEngine();
var rig = engine.GetDspChain();              // seat 0 == the solo player

var od = rig.AddNode("overdrive");           // appended FULLY WET
od.SetParam("drive", 0.6f);

// ABI v32 parked insertion: zero wet, so nothing is heard until it is ready.
var delay = rig.AddNodeBypassed("delay");
delay.SetParam("sync", 1f);
delay.SetParam("bpm", 120f);
delay.SetParam("division", 5f);              // "1/8" — a NAMED discrete param
delay.MoveTo(0);                             // still inaudible
delay.Bypass = false;                        // 10 ms fade-in, params already in place

rig.MeteringEnabled = true;                  // OFF until asked for
Span<JuceDspNodeMeter> meters = stackalloc JuceDspNodeMeter[rig.NodeCount];
rig.TryReadMeters(meters, out int nodeCount); // ONE P/Invoke, in chain order
Console.WriteLine($"chain latency {rig.LatencySamples} samples");
```

> The single-argument `TryReadMeters(Span<JuceDspNodeMeter>)` is `[Obsolete]`
> — it cannot separate "this seat has no nodes" from "the engine is gone", both
> of which it returns as `0` — and this repo builds with
> `TreatWarningsAsErrors`, so copying it into your own project fails the build.
> Use the `out int nodeCount` form.

## Node ids name their seat

Node ids are **engine-global**: seat `p` hands out
`p * JuceAudioEngine.PlayerNodeIdStride + local`. That is what kept the ABI v28
per-seat delta small — everything node-scoped (remove, move, bypass, params,
strings, branches, latency, meter) resolves the seat from the id and needs no
player argument; only chain-scoped calls are addressed by seat.

You should never have to do that arithmetic: every id this API returns or accepts
is already global, and `JuceDspChain.ToGlobalNodeId(local)` exists only so the
formula is never re-typed. A node id naming a seat the engine does not have is
**rejected, never clamped**.

## Message-thread only

Every call is chain **structure** or **parameter** work. The native side does it
on the calling thread and publishes an atomic ordering snapshot to the audio
thread, so nothing here runs on the audio callback. It is safe from any thread
and safe against a running device — but it is not a real-time API, so do not call
it from an audio callback of your own.

Structure changes are **crossfaded by the chain**, over ~10 ms: bypass fades out,
un-bypass fades in with the module already running, a remove waits for its
fade-out before the module is retired, and a move fades out at the old slot and
in at the new. A call returning therefore does not mean the change is fully
audible yet — by design. Splitter/merger topology switches instantly; that is
routing, not level.

Every call pins the engine handle for its duration, so one racing an engine
`Dispose()` surfaces as a caught `ObjectDisposedException` and never a read
through freed native memory. That is the difference between this and binding
`dca_engine_dsp_*` against `JuceAudioEngine.NativeHandle` yourself, which is
unpinned. Poll-shaped members (`MeteringEnabled` getter, `TryReadMeters`,
`JuceDspNode.LatencySamples`, `JuceDspNode.Meter`) no-op instead of throwing,
like every other `TryAcquire` reader in the wrapper.

## Node introspection is the ENGINE's answer (ABI v33)

`JuceDspNode.TypeId` and `JuceDspNode.Bypass` read
`dca_engine_dsp_get_node_type` / `dca_engine_dsp_get_bypass`, so a node this
wrapper did not create — one built by another binding against the same engine,
or found after re-attaching to a live one — reports its real type and its real
bypass state. Before v33 neither export existed: the wrapper could only remember
what it had itself written, so a chain enumerated from outside came back as bare
integers with an empty `TypeId` and every node reading un-bypassed.

`Bypass` reports the message-thread INTENT — the flag last set, including the
parked state an `AddNodeBypassed` starts a node in — and never the crossfade
position. The ~10 ms ramp belongs to the audio thread; a getter that tracked it
would flicker after every toggle.

`JuceDspModuleInfo.Category` comes from `dca_dsp_module_type_category` and is one
of a short fixed set the ENGINE owns: `dynamics`, `drive`, `amp`, `cab`,
`modulation`, `delay`, `reverb`, `pitch`, `filter`, `eq`, `utility`, `routing`,
`synth`. Every catalogue entry has one. It is read from native rather than
mirrored in a managed table because a managed table would be a second source of
truth for something only the factory knows — and would drift the first time a
module was added.

A node whose type the engine will not answer for reports `string.Empty` rather
than a guess — and a node whose ENGINE is gone falls back to the type id it was
created with. A wrong type id would send a UI looking up the wrong parameter
list, so neither path invents one.

## A preset is ONE change: edit scopes (ABI v34)

`JuceDspChain.BeginEdit()` returns a `JuceDspEditScope`. While it is open every
structure and parameter write on that seat accumulates against a shadow chain
and nothing is published to the audio callback; `Commit()` publishes the lot.

| Member | What it does |
|---|---|
| `JuceDspChain.BeginEdit()` | Opens the scope. If one is already open it is **ABORTED** and a fresh scope opens (the engine logs a Warning, and the superseded `JuceDspEditScope` is marked closed so its late `Dispose` cannot kill the new one). Nesting is still not supported — the inner begin DISCARDS the outer scope's edits rather than stacking. |
| `JuceDspChain.IsEditing` | Whether a scope is open on this seat. Poll-shaped: `false` on a disposed engine. |
| `JuceDspEditScope.Commit()` | Publish everything, as one change. Idempotent. |
| `JuceDspEditScope.Abort()` | Throw it away. Same as disposing without committing. |
| `JuceDspEditScope.IsClosed` / `.Player` | Scope state and the seat it belongs to. |
| `Dispose()` | **Aborts** unless `Commit()` already ran. |

```csharp
using var edit = rig.BeginEdit();
rig.Clear();                                  // the old board keeps playing
foreach (var step in preset) { … }            // parked; nobody hears it yet
edit.Commit();                                // one change, one fade window
```

Reads inside the scope report the SHADOW — `NodeCount`, `NodeIds()`,
`GetParam`, `Bypass`, `Branch`, `LatencySamples` all describe the preset being
built, not the one playing. Nodes added inside a scope are parked at zero wet
whatever their bypass flag and fade in together at commit; nodes removed inside
one keep playing out of the live chain and fade out together, in the same ~10 ms
window. Commit costs at most two ordering publications whatever the preset size,
against one per node without a scope.

**Dispose-aborts is the reason for the `using` shape**: a throw halfway through
building a preset leaves the rig exactly as it was instead of stuck in an edit
nobody will close. Abort restores STRUCTURE — membership, order, bypass flags,
branch tags — and destroys the nodes the scope added. Parameter VALUES are not
journalled and are not rolled back; a scope is a structure transaction, not an
undo stack.

A scope is per seat: one on seat 0 does not suppress seat 1's publications.

**A DROPPED scope used to freeze the seat for the life of the engine.**
`editing_` is cleared by no other managed call and the rigs are value members of
the native engine, so a caller who forgot the `using` left every later add /
remove / move / bypass accumulating in the shadow, returning success, and never
reaching the audio callback again. Two nets close that and both must stay:
`JuceDspEditScope` has a **finalizer** that aborts the scope and logs at Error
(naming it by `ScopeId`), and `BeginEdit()` supersedes a stale scope rather than
refusing. Neither is a reason to drop the `using` — a finalizer runs at a
GC-dependent moment, and the seat is silent-to-the-callback until then.

**An aborted scope also rolls back the wrapper's identity map.** The chain
journals the node ids a scope ADDED and the `JuceDspNode` wrappers its
`RemoveNode` / `Clear` EVICTED, and the abort replays both: the added ids are
purged (so `TryGetNode` stops resolving a node the engine destroyed — a phantom
whose poll-shaped members read to a UI as a real, bypassed, silent pedal) and
the evicted wrappers come back (so `Nodes` / `TryGetNode` keep handing out the
SAME instance the caller already holds). A node the same scope both added and
removed is not resurrected. A COMMIT drops the journal without replaying it.

## Sidechain (ABI v34)

`JuceDspNode.Sidechain` is a `JuceDspSidechainSource`: `None` (the module's own
input, the default), `SeatInput` (the seat's DRY input — the chain's input
block) or `SongMix` (the song as it stands when this seat is processed). Only
the DETECTOR moves; the audio path is untouched.

```csharp
if (JuceDspModuleCatalog.Find("compressor")!.SupportsSidechain)
    comp.Sidechain = JuceDspSidechainSource.SeatInput;
```

`JuceDspModuleInfo.SupportsSidechain` (`dca_dsp_module_type_flags` bit 0) is
what a palette must gate the control on: setting a source on any other module
type throws `JuceAudioException`, deliberately, because a source a module
quietly ignored would read as a live feature that does nothing. `compressor` and
`gate` accept a key today.

An unavailable source — `SongMix` on `ProcessOffline`, which has no song — falls
back to the module's own input, **never to silence**: keyed from silence a gate
sits closed.

## Discrete parameters declare themselves

Every discrete param is exactly one of two shapes, and a UI must render them
differently:

- **NAMED** — the value picks a named thing (amp `voicing`, cab `model`, delay
  `division`, a footswitch). `Labels` is non-null; `value = Min + index`. Render a
  **picker**.
- **ORDINAL** — the value is a count (chorus voices, phaser `stages`, bitcrusher
  bits). `Step > 0`. Render a knob that **snaps** to multiples of the step off
  `Min` and shows an integer; a continuous knob here reads "6.5 stages" while the
  core runs 6. Note `stages` has step **2**, not 1 — the core forces the allpass
  count even.

`JuceDspParamInfo.Describe(value)` does the right thing for both.

**A NaN parameter is refused**, managed-side as well as native-side. Every core's
clamp is NaN-transparent (`v < lo` and `v > hi` are both false for a NaN), so a
NaN that reaches the coefficients silences the module permanently; catching it in
managed code is what names the parameter that did it.

## Offline rendering

`ProcessOffline` runs a buffer through the seat's rig with no audio device
involved — the same code the live callback runs, chunked internally. It is how
per-seat tone is verified in tests, and it doubles as a preview / re-amp hook.

**Offline means offline.** The engine returns `JuceAudioStatus.Device` while a
device is open, and that refusal is deliberate: the chunk loop takes the chain's
process barrier — the audio callback's own wait-one-block mutex — once per 512
frames, so rendering a three-minute take against a running device contends the
callback ~16,000 times. Call `engine.Stop()` first, or use a dedicated
device-less engine.

```csharp
using var engine = new JuceAudioEngine();
engine.Stop();                               // OFFLINE MEANS OFFLINE
var rig = engine.GetDspChain();
rig.AddNode("amp").SetParam("gain", 0.65f);

var output = new float[interleaved.Length];
rig.ProcessOffline(interleaved, channels: 2, sampleRate: 48000, output);
```

> ⚠️ **`channels: 1` folds the rig's output down to mono**, not just its input
> up to stereo. The rig always runs two channels: a mono render feeds
> `L = R = input` and then writes `0.5 * (L + R)`. Through a channel-symmetric
> chain that is a no-op — through `widener`, `rotary`, `chorus`, a ping-pong
> `delay` or the panner it cancels exactly what those modules put out of phase,
> and a mono re-amp comes back thin or partially silent with nothing saying why.
> Duplicate the take into both channels and use the stereo overload, which is
> also zero-copy in place where the mono one allocates two `float[frames]`.

### ...with automation (ABI v34)

A static render is the one thing a re-amp is never asked to do: a volume swell,
a wah pass and a footswitch mid-solo are all ordinary. The automation overload
takes a list of `{frame, nodeId, paramId, value}` and chunks the render at each
event boundary, so an event at frame `F` is heard **by** sample `F`:

```csharp
var events = new[]
{
    JuceDspAutomationEvent.Create(frame: 0,      node.Id, "gainDb", 0f),
    JuceDspAutomationEvent.Create(frame: 48_000, node.Id, "gainDb", 12f),
    JuceDspAutomationEvent.Create(frame: 96_000, node.Id, "bypass", 1f),   // footswitch
};
rig.ProcessOffline(interleaved, channels: 2, sampleRate: 48_000, events);   // IN PLACE
```

* **`"bypass"` is a reserved parameter id** and routes to the chain's bypass
  (non-zero = bypassed). No module declares a parameter by that name, because
  the chain owns bypass and not the module.
* Events go through the **same `setParam` mailbox the live callback's setters
  use**, which is what makes an automated render and a live performance of the
  same gesture unable to diverge — and what makes a NaN rejected here exactly as
  it would be rejected live.
* The list must be **sorted by non-decreasing frame**. An unsorted list is
  rejected rather than silently reordered: a caller that thinks it is automating
  a sweep and is actually automating a scramble gets a render that is plausible
  and wrong.
* A rejected event throws a `JuceAudioException` naming its **index**, and the
  input buffer is left **untouched** — so a half-processed buffer can never be
  mistaken for a good one.
* At most **`JuceDspChain.MaxAutomationEvents`** (262,144) events per call. Past
  that the wrapper throws `ArgumentOutOfRangeException` *before* marshalling
  anything; the engine enforces the same bound, but only after the whole list
  has been copied into unmanaged memory.
* Both the chain's bypass and the `gain` node's level **ramp over ~10 ms**
  rather than stepping, so measure a level change a few thousand samples clear
  of the boundary.

## Latency

`JuceDspChain.LatencySamples` is the whole chain, at the chain's prepared rate.
Inside a split/parallel region it is the **maximum** over the enabled branches,
not their sum — they run in parallel, so the region is as late as its latest
branch. It is **not** a claim that the branches are aligned: the engine does not
delay-compensate between them, and this number deliberately does not pretend
otherwise.

**Since ABI v34 the branches are ALIGNED, so that number is what the region
actually runs at.** `JuceDspChain.BranchLatencyCompensation` (default `true`)
makes the merger delay each branch by (latest − its own); summing branches of
different latency is a comb filter, not a blend, and that was the v23 accepted
limit rather than a choice. Set it `false` for the pre-v34 sum — the comb is a
sound some players want — at which point the paragraph above's old caveat
applies again. The compensation follows parameter changes as well as structure
ones (a param can move a latency), survives a device change, clamps at 8,191
samples per branch, and does not cover an **unmatched** splitter, which
auto-merges inside the chain rather than at a merger node.

`JuceDspNode.LatencySamples` is one node, and a **bypassed** node reports 0 — it
is not in the path, so a caller compensating for the rig must not budget for it.
Most modules are 0; the pitch/neural family (harmonizer, whammy, klon, namamp's
rate converter, the drop tuner) is not.
