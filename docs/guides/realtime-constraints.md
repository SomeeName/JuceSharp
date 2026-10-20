# Real-time Constraints for Consumers

None of your code runs on the audio thread — the managed wrapper is control-rate
only, and the callback never enters the CLR. What you can still do is starve it,
confuse it, or pay for guarantees you did not ask for. This page is the list.

For what the native side runs on the callback, see
[Threading Model](../native/threading.md); for the managed thread contract, see
[Threading](threading.md).

## Never call the wrapper from an audio callback of your own

Every `JuceDspChain` / `JuceDspNode` call is "safe from any thread and safe
against a running device, **but it is not a real-time API**". The native side
does the structure and parameter work on the *calling* thread and hands the
audio callback an atomic ordering snapshot; that work allocates, takes mutexes
and publishes. If you host JuceSharp inside another engine's callback, marshal
out first.

## A hosted VST3 runs on the callback — its bugs become your dropouts

The VST3 input chain and the MIDI instrument are processed **on the audio
thread**. Whether a third-party plugin allocates, locks, hits the file system or
throws is the plugin's problem; the consequence — `LateCallbacks` climbing, or a
process kill on a native crash — is yours. Two mitigations exist:

* **The sandbox.** `SetRemoteBridge` marshals the plugin across a shared-memory
  bridge into the out-of-process host child (`juce_vst_host.exe`), so a plugin
  crash kills the child instead of your process. See
  [Hosting VST3 Plugins](vst3-hosting.md) — note that the managed API exposes
  three of the four arguments the child requires, so the feature is not
  reachable from the documentation alone today.
* **Measure, don't guess.** Load the plugin, run it, and read
  `DeadlineOverruns` (see [Failure modes](failure-modes.md#the-runtime-health-signal)).
  "Our render overran its block" and "the machine stalled us" are different
  problems and look identical from outside the callback.

## Non-finite samples are scrubbed — but only at the boundaries

A NaN into an IIR biquad is **permanent**: the filter feeds its own output back
and only a reset clears it, so one bad sample out of a user plugin could
otherwise silence a seat for the session, meter included. The engine therefore
scrubs non-finite samples (`dcdsp::scrubNonFinite`) after every untrusted stage
— after the VST3 input chain and the sandbox bridge, at the top of the master
bus, after the instrument mixes, and inside the tone rig's own input and between
stages.

What that buys you and what it does not:

* You do **not** need to scrub audio you hand the engine through `OpenFile`, a
  plugin or the rig.
* You **do** need to reject non-finite *parameter* values: a NaN param is
  rejected at `DspChain::setParam` and reported, because every core's clamp is
  NaN-transparent (`v < lo` and `v > hi` are both false for a NaN). A rejected
  automation event fails the whole render with the event's index rather than
  producing a plausible-and-wrong file.

## Do not smooth gains on top of the engine's ramps

**Every user-facing gain is ramped across the block by the engine** — master
volume, per-seat monitor, input trim, EQ trim, per-source volume and pan, and
the mixes and levels inside the DSP cores. A slider that publishes one value per
UI tick is exactly what that machinery expects.

Adding your own smoothing on top does not make it smoother; it makes the control
lag by your smoothing time constant *plus* the block ramp, and it makes a mute
take audible time to arrive. **Publish the target value; let the engine ramp.**

```csharp
// Right: one write per UI tick, whatever the tick rate.
engine.SetMasterGain(sliderValue);

// Wrong: a managed ramp feeding the engine's ramp.
// for (var g = current; g < target; g += 0.01f) { engine.SetMasterGain(g); Thread.Sleep(5); }
```

## Change a whole preset inside one edit scope

Without a scope, every add / remove / move / bypass is its own published
structure change with its own ~10 ms crossfade: a twelve-pedal preset is twelve
audible changes. `JuceDspChain.BeginEdit()` suppresses publication for the
length of the scope; the commit costs **at most two publications whatever the
preset size** — one transitional, where the old board fades out while the new
nodes fade in in the same 10 ms window, and one final.

```csharp
var chain = engine.GetDspChain(player: 0);

// Dispose-aborts is the whole point of the `using`: a throw half-way through
// leaves the rig exactly as it was, instead of stuck in an edit nobody closes.
using (var edit = chain.BeginEdit())
{
    chain.Clear();                       // the old board keeps playing until the commit

    var gate = chain.AddNode("gate");
    gate.SetParam("thresholdDb", -50f);

    var amp = chain.AddNode("amp");
    amp.SetParam("gain", 0.6f);

    edit.Commit();                       // one change, one fade window
}
```

Three rules that are easy to trip over:

* **A dropped scope freezes the seat.** Forgetting the `using` used to publish
  nothing to that seat's callback for the life of the engine while every edit
  returned success. A finalizer and a stale-scope abort in `BeginEdit` recover
  it now, both logging at `Error` — but a finalizer runs at a GC-dependent
  moment, so the seat is silent-to-edits until then. Use `using`.
* **Abort restores structure only** — membership, order, bypass, branch tags.
  Parameter values are not journalled.
* **Nodes added inside a scope are parked at zero wet** whatever their bypass
  flag. Outside a scope, `AddNodeBypassed` is the way to insert a node, configure
  it, and only then fade it in with `node.Bypass = false`.

## Meters are poll-only and off until asked for

Per-node meters cost nothing until you enable them: while
`chain.MeteringEnabled` is false the callback runs the pre-v30 path byte for
byte. Turn them on when a meter is on screen, off when it is not, and read the
whole chain in **one** call.

```csharp
chain.MeteringEnabled = true;
var meters = new JuceDspNodeMeter[chain.NodeCount];
// ...on your redraw timer:
chain.TryReadMeters(meters, out var count);   // one P/Invoke, in chain order
```

The same discipline applies to `MasterLimiterReductionDb` and the EQ spectrum
tap: nothing is queued and no callback crosses the ABI, so a host that never
reads them pays nothing.

## The master limiter is OFF, and costs 1 ms when on

Since ABI v35 `dca_engine_set_master_limiter` is **off at engine start** and the
host opts in. The stage sits on the live monitor path, so a default-on ceiling
costs every player 1 ms of latency on their own playing for a feature they did
not ask for.

Turn it on when you need a bounded output — the streaming mirror's float path
otherwise carries samples past ±1.0 into an encoder that wraps:

```csharp
engine.SetMasterLimiter(true, ceilingDb: -0.3f, releaseMs: 80f);

// Read back, never echo: the engine CLAMPS, so this is the only honest answer.
var (on, ceiling, release) = engine.GetMasterLimiter();
```

**Its 1 ms look-ahead is folded into `OutputLatencySamples`, and switched off the
stage is not in the path at all** — so `MasterLimiterLatencySamples` is 0 and the
total moves. Anything that aligns audio to a clock (a click scheduler, a
playhead, a chart) must **re-read `OutputLatencySamples` after a toggle** rather
than cache it for the session.

`BufferFrames` is the other half of that arithmetic and is the latency knob in
`JuceEngineConfig`; both it and `SampleRate` are what the device actually gave
you when read back off the engine, not what you asked for.

## Test a rig deterministically with the offline render

`JuceDspChain.ProcessOffline` runs **the same code the live callback runs**,
chunked internally, with no device involved. The automation overload takes a
sorted event list and chunks at each event boundary, driving every event through
the same `setParam` mailbox the live setters use — which is what stops an
automated render and a live performance of the same gesture diverging. That
makes it the right basis for a regression test of your tone.

```csharp
var events = new[]
{
    JuceDspAutomationEvent.Create(frame: 0,      nodeId: reverb.Id, paramId: "mix",    value: 0.05f),
    JuceDspAutomationEvent.Create(frame: 48_000, nodeId: reverb.Id, paramId: "mix",    value: 0.45f),
    JuceDspAutomationEvent.Create(frame: 96_000, nodeId: delay.Id,  paramId: "bypass", value: 1f),
};
chain.ProcessOffline(buffer, channels: 2, sampleRate: 48_000, events);
```

* **`"bypass"` is a reserved param id** routed to the chain's own bypass — a
  footswitch, on any node.
* **The list must be sorted by non-decreasing `Frame`.** An unsorted list is
  rejected, never silently reordered.
* **Offline means offline:** with a device open the call fails with
  `JuceAudioStatus.Device`. Call `engine.Stop()` first, or use a device-less
  engine (the `"Null"` device type under `DCA_ENABLE_NULL_DEVICE=1`).
* **On rejection nothing is touched** — not the buffer and not the rig: the whole
  event list is validated *and resolved* before a sample is rendered. A
  successful render does leave parameters where the last event put them;
  automation is a performance, not a transaction.
* **There is no limiter on the offline path.** The rig has gain and nothing
  bounds it, so a 24-bit PCM write of a hot render clips. Normalise, or trim.

## Related

* [The DSP Tone Rig](tone-rig.md) — modules, branches, sidechains.
* [Offline Processing](offline.md) — the non-rig offline helpers.
* [Engine Health & Dropouts](health.md).
* [Failure modes & runtime health](failure-modes.md).
