# Offline Processing

Everything on this page runs faster than real time and none of it touches the
audio device.

## Playing from memory

`OpenMemory` is not strictly offline, but it belongs with the rest: it lets you
play bytes you already hold — an archive entry, a download, a decrypted asset —
without a temporary file.

```csharp
byte[] bytes = archive.ReadEntry("song.ogg");
using var src = engine.OpenMemory(bytes, formatHint: ".ogg");
src.Play();
```

The format hint is optional; JUCE's format manager sniffs the container.

## Waveform peaks

Both entry points are static — no engine, no device.

```csharp
float[] peaks = JuceAudioEngine.GenerateWaveform(path, blockMs: 10.0);
float[] same  = JuceAudioEngine.GenerateWaveformFromMemory(bytes, blockMs: 10.0);
```

See [Waveform Generation](waveform.md) for why `blockMs` is a `double`.

## Rendering a file to WAV

```csharp
JuceAudioEngine.RenderToWav(srcPath, dstPath);
JuceAudioEngine.RenderToWav(srcPath, dstPath, sampleRate: 44_100);
```

> ⚠️ **Warning**
> **`RenderToWav` cannot stretch or shift, and says so.** Any `tempo` other
> than `0f` or `1f`, or any non-zero `semitones`, throws
> `NotSupportedException`. This path is a decode plus a 16-bit PCM WAV write —
> it has no Rubber Band transport. For a stretched bounce, play through the
> live engine.

## Decoding to PCM

`JuceSharp.AudioIo` is a **separate package** (`<PackageReference
Include="JuceSharp.AudioIo" />`, or a `ProjectReference` to
`src/JuceSharp.AudioIo/` from a clone). It wraps the native decoder:

```csharp
using JuceSharp.AudioIo;

var (sampleRate, channels, frames) = JuceAudioDecoder.Probe(path);
AudioBuffer buf   = JuceAudioDecoder.Decode(path);
AudioBuffer mono44 = JuceAudioDecoder.DecodeAs(path, 44_100, 1);

float[] m = buf.ToMono();
float[][] perChannel = buf.SplitChannels();

WavIo.WriteFloat32(outPath, buf);
```

See [Audio I/O](audioio.md) for the format matrix and the caveat on
`DecodeAs`'s resampler.

## Re-amping through the tone rig

The rig can render a stereo buffer with no device open — the same code the
audio callback runs:

```csharp
using var engine = new JuceAudioEngine();
engine.Stop();                                   // offline means offline
var rig = engine.GetDspChain();
rig.AddNode("amp").SetParam("gain", 0.65f);
var output = new float[interleaved.Length];
rig.ProcessOffline(interleaved, channels: 2, sampleRate, output);
```

> ⚠️ **Warning**
> **Stop the engine first.** The chunk loop takes the chain's process barrier —
> the callback's own wait-one-block mutex — once per 512 frames, so rendering
> three minutes against a running device contends the callback about 16,000
> times, each for a whole rig pass on a worker thread. An open device returns
> `DCA_ERR_DEVICE`. Use a dedicated device-less engine for re-amping.
>
> The rig is prepared lazily at the sample rate you pass, which must be
> 8,000–192,000 Hz.

### ...with automation (ABI v34)

A static render is the one thing a re-amp is never asked to do: a volume swell,
a wah pass and a footswitch mid-solo are all ordinary. The automation overload
takes a list of `{frame, nodeId, paramId, value}` and chunks the render at each
event boundary, so an event at frame `F` is **applied at** sample `F` — not
necessarily HEARD there, see the last bullet:

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
* A rejected event throws a `JuceAudioException` naming its **index**, and
  NOTHING is touched — not the input buffer, and (since ABI v35) not the RIG
  either. The whole list is validated AND RESOLVED — seat, node, parameter id —
  before a single event is applied, so a half-processed buffer can never be
  mistaken for a good one and a failed render can no longer leave the seat's
  chain at an arbitrary mid-sweep state.
* `nodeId` must name THIS seat. Node ids are engine-global
  (`player * PlayerNodeIdStride + local`), so an id from another seat used to be
  applied to THAT seat's chain; it is rejected by index (v35).
* `events.Length` is capped at **262,144**. Resampling an automation curve per
  sample — the obvious naive way to write a wah sweep — produced a chain of
  one-sample `process` calls, each taking the chain's barrier: a 5-minute render
  became 14 M of them with no cap and no diagnostic, and simply appeared to hang.
* Calling this while a device is OPEN is `DCA_ERR_DEVICE`
  (`JuceAudioStatus.Device`). Offline means offline.
* Both the chain's bypass and the `gain` node's level **ramp over ~10 ms**
  rather than stepping, so measure a level change a few thousand samples clear
  of the boundary.

Run `--offline in.wav out.wav` in `examples/JuceSharp.Examples.ToneRig` to see
it end to end.

## Drop tune, offline

`dca_droptune_process_offline` runs the CSVD shifter over a segment list
against a primed ring, so engage/disengage transitions happen mid-stream
exactly as they do live. Mono, in-place allowed. See
[Drop Tune](drop-tune.md).

## What these paths do *not* inherit

The offline tooling path is not the device callback and inherits none of its
guarantees. It establishes its own denormal handling and re-prepares the rig on
a rate change — but only when no device is open. Its contracts are
tooling-grade: they are real processing, good enough to double as a preview or
a bounce, and not a promise of real-time safety.
