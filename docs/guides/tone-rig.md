# The DSP Tone Rig

JuceSharp ships an in-house, reorderable effect chain that sits on the **live
input**, ahead of the VST3 input chain. It is built from header-only `dcdsp`
cores compiled into `juce_sharp.dll`, so there is no plugin scan, no editor
window and no third-party code on the audio thread.

> ℹ️ **Note**
> The managed face of the chain is `JuceDspChain` (`engine.GetDspChain(seat)`),
> with `JuceDspNode` and the static `JuceDspModuleCatalog` — see
> [JuceDspChain](../api/juce-dsp-chain.md). The C ABI names below are what those
> calls resolve to; `examples/JuceSharp.Examples.ToneRig` uses the wrapper.

## Concepts

**One chain per seat.** The engine holds `DCA_MAX_PLAYERS` (4) chains, one per
live-input seat. Chain-scoped operations take a `player` index; node-scoped
ones do not.

**Node ids are engine-global.** Seat `p` hands out
`p * DCA_PLAYER_NODE_ID_STRIDE + local`, i.e. `player * 1_000_000 + local`.
That is why remove / move / bypass / set_param / get_param / node-string /
node-branch all kept their pre-v28 signatures: they resolve the seat from the
id. A node id naming a seat ≥ `DCA_MAX_PLAYERS` is **rejected**, never clamped
onto another seat's rig.

**Structure changes are crossfaded by the chain, never by a core.** Every
regular node carries a wet/dry weight that ramps over 10 ms on the audio
callback: bypass fades out, un-bypass fades in *with the module running*
during the ramp, remove waits for the fade-out before retiring the module, and
move/retag fades out at the old slot and in at the new. A node whose fade-out
has fully parked is `reset()` on the way back in — otherwise its delay lines
and LFO phases are frozen, not decayed, and fading them in replays audio from
before the bypass. A toggle *inside* the 10 ms ramp is not a park and keeps its
state. **A move or a remove never clears a delay's buffer.**

**Splitter and merger switch instantly** — that is topology, not level.

`JuceDspNode.TypeId` and `JuceDspNode.Bypass` are read back from the ENGINE
(`dca_engine_dsp_get_node_type` / `dca_engine_dsp_get_bypass`, ABI v33), so a
node another binding created against the same engine is not a second-class
citizen. `Bypass` reports the message-thread INTENT, not the position of the
10 ms crossfade.

## The catalogue

Enumerate at runtime; do not hard-code a module list.

Everything on this page is in the **core `JuceSharp` package**, in the
`JuceSharp.Dsp` *namespace* — the namespace is shared with the separate
`JuceSharp.Dsp` analysis package, so do not add a package reference looking for
these types.

```csharp
using JuceSharp.Dsp;

foreach (var m in JuceDspModuleCatalog.Modules)     // dca_dsp_module_type_count/_info
{
    // m.Category (ABI v33) groups the palette: dynamics / drive / amp / cab /
    // modulation / delay / reverb / pitch / filter / eq / utility / routing /
    // synth. The ENGINE owns that vocabulary — never mirror it in a managed
    // table, which would be a second source of truth that drifts on the next
    // module added.
    Console.WriteLine($"{m.Id}  {m.DisplayName}  [{m.Category}]");
    foreach (var p in m.Params)                       // dca_dsp_module_param_count/_info
        Console.WriteLine($"    {p.Id} [{p.Min}..{p.Max}] def={p.Default}  {p.Describe(p.Default)}");
}
```

Every **discrete** parameter declares itself, one of two ways:

* **Named** — `JuceDspParamInfo.Labels` is non-null (`dca_dsp_module_param_label_count`
  / `_param_label`, ABI v24), e.g. an amp model picker.
* **Ordinal** — `JuceDspParamInfo.Step > 0` (`dca_dsp_module_param_step`, ABI v27).

A parameter the adapter rounds is never a plain knob. Honour the labels and
the step or your UI will park a control at "6.5 stages" while the core runs 6.

**String parameters** (ABI v22) carry paths — a user IR for `cab` and
`acousticsim`, a NAM/AIDA-X capture for `namamp`, the lyric slots for the Miku
stomps. They go through `JuceDspNode.SetString` / `GetString`
(`dca_engine_dsp_set_node_string` / `get_node_string`), not `SetParam`.

> ⚠️ **Warning**
> **A NaN parameter is rejected at the chain boundary and reported.** Every
> core's clamp is NaN-transparent (`v < lo` and `v > hi` are both false for
> NaN), so a NaN that got through would reach the coefficients and silence the
> module permanently. Do not rely on the clamp; do not send NaN.

## Building a chain

```csharp
var rig = engine.GetDspChain();            // seat 0

var gate = rig.AddNode("gate");
var od   = rig.AddNode("overdrive");
var amp  = rig.AddNode("amp");
var cab  = rig.AddNode("cab");

od.SetParam("drive", 0.6f);
amp.SetParam("gain",  0.7f);

gate.MoveTo(0);                            // reorder — buffers survive
od.Bypass = true;                          // 10 ms fade-out
```

## Parked insertion (ABI v32)

A node added the normal way is **immediately wet**. To assemble a preset while
audio is running, insert it parked, configure it, then fade it in:

```csharp
var delay = rig.AddNodeBypassed("delay");   // zero wet
delay.SetParam("sync",   1f);
delay.SetParam("trails", 1f);
delay.SetParam("mix",    0.35f);
delay.Bypass = false;                       // now fade it in
```

The older add exports stay immediately wet, deliberately, so offline renders
are unchanged. Parked insertion covers ONE node; for a whole preset use an edit
scope.

## Edit scopes: a preset is one change (ABI v34)

Every structure call publishes an ordering snapshot to the audio callback, so
building a twelve-pedal board pedal by pedal is exactly what the listener
hears. `BeginEdit()` suppresses publication until you commit:

```csharp
using (var edit = rig.BeginEdit())
{
    rig.Clear();                            // the OLD board keeps playing
    foreach (var step in preset)
    {
        var node = rig.AddNode(step.TypeId);   // parked; nobody hears it yet
        foreach (var (id, value) in step.Params) node.SetParam(id, value);
        node.Branch = step.Branch;
    }
    edit.Commit();                          // one change, one fade window
}                                           // dispose without Commit = abort
```

* Inside the scope, reads report the **shadow**: `rig.NodeCount` is the preset
  you are building, not the one that is playing.
* The old board fades out while the new one fades in, in the **same** ~10 ms
  window; commit costs at most two ordering publications whatever the preset
  size.
* **Dispose without `Commit()` aborts** — membership, order, bypass flags and
  branch tags come back and the nodes the scope added are destroyed. Parameter
  VALUES are not rolled back: a scope is a structure transaction, not an undo
  stack.
* A scope is per seat. A nested `BeginEdit()` no longer throws — it **aborts**
  the open scope (Warning logged) and opens a fresh one, because the state a
  refusal left behind could not be cleared by any later call: one dropped scope
  used to freeze that seat's rig for the life of the engine. Nesting is still
  not supported; the inner begin discards the outer scope's edits. Splitter and
  merger topology inside a committed scope switches with the rest of the preset.
* **Still use the `using`.** A dropped scope is recovered by
  `JuceDspEditScope`'s finalizer (it aborts and logs at Error), but that runs
  whenever the GC gets to it — until then the seat publishes nothing to the
  audio callback while every call returns success.

## Sidechain: key a pedal off something else (ABI v34)

A compressor's detector normally listens to its own input, which means a
compressor after a drive is compressing the drive's output, not your picking.
Since v34 a module that advertises the capability can be keyed:

```csharp
var comp = rig.AddNode("compressor");
comp.Sidechain = JuceDspSidechainSource.SeatInput;   // follow the DRY pick
// …or duck the whole rig under the backing track:
comp.Sidechain = JuceDspSidechainSource.SongMix;
```

Only the **detector** moves — the audio path, the crossfade and the NaN scrub
are exactly what they were. `compressor` and `gate` accept a key today; check
`JuceDspModuleInfo.SupportsSidechain` before offering the control, because every
other module type REJECTS a source rather than ignoring it.

If the source is unavailable when the block runs — `SongMix` on the offline
render path — the module keys off its own input. Never off silence: a gate keyed
from silence would sit closed and the rig would go quiet with nothing on screen
saying why.

## Meters and latency (ABI v30)

Metering is **poll-only and off until you ask for it**, so a rig with no UI
attached pays nothing.

```csharp
rig.MeteringEnabled = true;

// JuceDspNode.Meter is a non-nullable struct: `is { } m` is ALWAYS true, and
// with metering off (or the node gone) it hands you a zeroed reading. Use the
// Try form when you need to tell "silent" from "no answer".
if (rig.TryReadMeter(cab.Id, out var m))
    Console.WriteLine($"in {m.InputPeak:F3}/{m.InputRms:F3}  out {m.OutputPeak:F3}/{m.OutputRms:F3}");

Span<JuceDspNodeMeter> all = stackalloc JuceDspNodeMeter[rig.NodeCount];
rig.TryReadMeters(all);                      // whole seat in one call

int chain = rig.LatencySamples;
```

> ⚠️ **Warning**
> `dca_engine_dsp_get_chain_latency` is the **maximum** over a split region's
> branches, not the sum. Since ABI v34 it is also what the region runs at, because
> the branches are delay-aligned (below). The amplitudes are **linear** sample
> values for that node, not the master bus.

## Parallel branches are delay-aligned (ABI v34)

Put a pitch shifter in one branch of a split and leave the other empty, and
before v34 the merger added the two arrivals as they fell — a comb filter, not
a blend. The merger now delays each branch by (latest − its own):

```csharp
rig.BranchLatencyCompensation = false;   // back to the v23 sum; the comb IS a sound
```

It is **on by default**, follows parameter changes as well as structure ones (a
param can move a module's latency) and survives a device change. The ceiling is
8,191 samples per branch; a branch stacking past it is clamped, and an
**unmatched** splitter auto-merges inside the chain rather than at a merger node
and is not compensated.

## Rendering offline

The same code the callback runs, with no sound card:

```csharp
engine.Stop();                               // offline means offline
var output = new float[interleaved.Length];
rig.ProcessOffline(interleaved, channels: 2, sampleRate, output);
```

> ⚠️ **Warning**
> **Stop the engine first.** The chunk loop takes the chain's process barrier —
> the audio callback's own wait-one-block mutex — once per 512 frames, so
> rendering three minutes against a running device contends the callback about
> 16,000 times, each for a whole rig pass on a worker thread. An open device
> returns `DCA_ERR_DEVICE`.

## Running example

```powershell
dotnet run --project examples\JuceSharp.Examples.ToneRig -- --catalog
dotnet run --project examples\JuceSharp.Examples.ToneRig -- --seconds 8
dotnet run --project examples\JuceSharp.Examples.ToneRig -- --offline di.wav reamped.wav
```

See [Examples](examples.md).
