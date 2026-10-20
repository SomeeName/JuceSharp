# Audio Playback

The simplest possible use of JuceSharp: open a file, set transport
parameters, hit play.

## Opening a file

```csharp
using var engine = new JuceAudioEngine();   // opens AND starts the device

using var src = engine.OpenFile(@"C:\Music\track.flac");
```

> ℹ️ **The constructor takes the sound card.** `dca_engine_create` is given the
> config and opens *and starts* the device, so `Start()` immediately afterwards
> is a no-op (it early-returns on an already-started engine). That matters on
> exclusive-mode ASIO: the card is yours from the moment the constructor
> returns. `Start()` is for after an explicit `Stop()` — and note that `Stop()`
> is not a pause: it detaches every source from the mixer permanently, so
> restarting gives you a live device playing silence. `Reconfigure`, or a new
> engine, is the way back.

`OpenFile` returns a [`JuceAudioSource`](../api/juce-audio-source.md), already
attached to the engine's mixer — the engine streams it on demand.

`OpenMemory` does the same from bytes you already hold:

```csharp
using var src = engine.OpenMemory(File.ReadAllBytes(path), formatHint: ".ogg");
```

### Supported formats

| Format       | Notes                                  |
| ------------ | -------------------------------------- |
| WAV / BWF    | PCM 8/16/24/32-bit, float 32/64        |
| AIFF / AIFC  | PCM                                    |
| FLAC         | Native JUCE decoder                    |
| OGG Vorbis   | Native JUCE decoder                    |
| MP3          | Native JUCE decoder (CBR + VBR) — **Windows only**, like [Audio I/O](audioio.md) says |

The file's sample rate is converted to the device rate **once, at load**, by a
rational polyphase FIR — not per block. Group delay is zero by construction,
so the song does not sit late against a chart or a click.

## Transport

```csharp
src.Play();        // start streaming
src.Pause();       // freeze position
src.Stop();        // freeze and rewind to 0
src.Seek(TimeSpan.FromSeconds(30));

var pos     = src.Position;        // transport position
var audible = src.AudiblePosition; // what the listener is hearing now
var length  = src.Length;
```

`AudiblePosition` is what UIs should display — it accounts for the Rubber Band
stretcher's internal read-ahead, and it is the value the render clock is
published against, so it and `TryGetRenderClock` describe the same output
sample.

## Volume / pan / mute

```csharp
src.Volume = 0.5f;     // linear gain
src.Mute   = true;     // gates to 0 without losing the user value
src.Pan    = -1f;      // -1 = left, 0 = centre, +1 = right
```

> ⚠️ **Warning**
> **`Volume` is clamped at 0 and unbounded above.** A negative value folds to
> `0f` in the wrapper and again in the engine (ABI v33 added the native
> `[0, +inf)` clamp) — a negative gain would be a phase inversion, not a quieter
> source. There is deliberately **no upper bound**: boost past unity is a
> shipping feature. Above 1.0 you can clip the master bus, so clamp the top
> yourself if the value comes from a user-facing slider — or switch the master
> limiter on (`engine.SetMasterLimiter(...)`; it is **off** at engine start).
> A `NaN` is passed through on purpose, so it surfaces as a `JuceAudioException`
> naming the call rather than being laundered into a silent source.

`Volume`, `TempoRatio`, `PitchSemitones` and `Pan` are all readable: the
wrapper caches the last value you set, because the native side exposes no read
API for them. `Mute` and `Volume` share a lock so they cannot desync.

## Time-stretch & pitch (Rubber Band)

```csharp
src.TempoRatio     = 0.85f;   // 0.5 = half speed, 2.0 = double speed
src.PitchSemitones = +3f;     // semitones
```

These are independent: tempo doesn't change pitch and vice versa.

`engine.SetStretchQuality(faster: true)` swaps the whole engine from Rubber
Band's R3 (Finer) engine to R2 (Faster) — worth doing when five or more
stretchers have to fit in the audio thread's budget. It only affects sources
opened **after** the call.

For a stem mix, join the sources to the shared stem group instead of giving
each its own stretcher:

```csharp
foreach (var stem in stems) stem.StemGrouped = true;
engine.StemGroupTempo           = 0.85f;
engine.StemGroupPitchSemitones  = 0f;
```

Grouped sources bypass their own stretcher and are summed into one bus driven
by a single one — which is what avoids the comb-filter-style cancellation you
get when independent per-stem stretchers drift out of window alignment.
Per-source `TempoRatio` / `PitchSemitones` are no-ops while grouped.

> ℹ️ **Note**
> The Rubber Band engine adds inherent latency to the stretched source.
> JuceSharp accounts for this in `AudiblePosition`.

## Looping

`Loop` is a **`JuceLoopRegion?`**, not a `bool`:

```csharp
src.Loop = new JuceLoopRegion(TimeSpan.FromSeconds(8), TimeSpan.FromSeconds(16));
src.Play();

src.Loop = null;   // stop looping; the old region is disposed for you
```

It is a **polling** loop — a timer watches `AudiblePosition` and seeks back to
`Start` once it crosses `End`, at `PollIntervalMs` (default 5 ms) precision.
There is no sample-accurate native loop in the ABI today.

## Disposal

Every wrapper is `IDisposable` only — there is no `IAsyncDisposable`.

You do **not** have to dispose the source before the engine.
`JuceAudioEngine.Dispose()` claims its own handle, stops the device, and
disposes every source, plugin and MIDI input it handed out before destroying
the engine. Disposing the source yourself is still good hygiene — it frees the
native source immediately instead of at engine teardown.
