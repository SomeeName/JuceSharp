# JuceAudioSource

`sealed class JuceAudioSource : IDisposable`

Handle to a single decoded media file owned by the native engine. Obtain
one from [`JuceAudioEngine.OpenFile`](juce-audio-engine.md).

## Transport

| Member                     | Description                                                    |
| -------------------------- | -------------------------------------------------------------- |
| `Play()`                   | Start streaming.                                               |
| `Pause()`                  | Freeze position.                                               |
| `Stop()`                   | Freeze and rewind to 0.                                        |
| `Seek(TimeSpan position)`  | Jump to the given offset.                                      |

## Position

| Member              | Description                                                 |
| ------------------- | ----------------------------------------------------------- |
| `Position`          | Last-set position (the seek target).                        |
| `AudiblePosition`   | What the listener is actually hearing right now — accounts for the Rubber Band stretcher's internal read-ahead. |
| `Length`            | Total file length.                                          |
| `GetPreloadDigest()` | Fingerprint of what a preloaded source plays: hash, step, frames, held rate, converted-at-load (ABI v43). Equal digests = bit-identical audio. Throws for a streaming source. Tests and diagnostics only. |

## Audio shaping

All of these are **read/write**. The native side exposes no read API for them,
so the getter returns the wrapper's cached last-set value.

| Member             | Type   | Default | Description                          |
| ------------------ | ------ | ------- | ------------------------------------ |
| `Volume`           | `float`| `1f`    | Linear gain, clamped to `[0, +inf)` — see below. |
| `Mute`             | `bool` | `false` | Gates the native gain to 0 without losing `Volume`. Shares a lock with `Volume` so the two cannot desync. |
| `Pan`              | `float`| `0f`    | Stereo position, clamped to `[-1, +1]` (−1 = hard left). |
| `TempoRatio`       | `float`| `1f`    | Rubber Band time-stretch factor. While `StemGrouped` the write is DROPPED and logged at Warn — the getter keeps the last value the engine actually ran, never the ignored one. |
| `PitchSemitones`   | `float`| `0f`    | Rubber Band pitch shift. Dropped + logged while `StemGrouped`, like `TempoRatio`. |
| `StemGrouped`      | `bool` | `false` | Join the engine's single shared stem-group stretcher (`JuceAudioEngine.StemGroupTempo` / `StemGroupPitchSemitones` then drive tempo and pitch). |
| `Loop`             | `JuceLoopRegion?` | `null` | Assigning ATTACHES the new region first, then swaps it in and disposes the previous one. A region already bound to a different source is rejected with `InvalidOperationException` and the existing loop is left running — the old order disposed the previous region before it found that out, so a rejected assignment killed the loop and armed nothing. See [`JuceLoopRegion`](juce-loop-region.md). |
| `IsDisposed`       | `bool`            | `false` | True once `Dispose()` has claimed the native handle; every other member then throws `ObjectDisposedException`. |

> ⚠️ **`Volume` has a FLOOR and no CEILING, on purpose (ABI v33).**
> A negative gain is clamped to 0, on both sides of the ABI. It is not a
> quieter source — `juce::AudioTransportSource::setGain` accepts it and it
> inverts PHASE, so the source cancels against anything correlated already in
> the mix (the same take on another stem, a doubled guitar, the click). That
> reads as a hollow mix rather than as a wrong level, and nothing reports it.
>
> There is deliberately **no upper bound**: boost above unity is a shipped
> feature (a +6 dB stem fader is linear 2, and a per-stem gain folded into a
> master gain reaches 16 in DotCharter), so a ceiling invented at this boundary
> would silently cap a user’s fader. Values well above 1 CAN clip the master
> bus — that is the caller’s call to make. A non-finite value is REJECTED
> (`JuceAudioException`), never laundered into 0.
>
> `Pan` is clamped to `[-1, +1]` rather than rejected: a caller dragging past
> the end wants the hard left it asked for. The clamp is mirrored managed-side
> so the cached getter cannot report a position the engine is not playing.

## Disposal

`Dispose()` drains any in-flight native call, then detaches the source and
releases its buffers. It implements **`IDisposable` only** — there is no
`DisposeAsync`. Disposing also disposes any attached `Loop`.

You do not have to dispose the source before the engine: the engine keeps a
registry of every child it handed out and disposes them itself, in order,
before `dca_engine_destroy`.

## Threading

All members are safe to call from any thread. Setters are atomic; the
audio thread picks up new values at the next block boundary.

## Errors

Every member throws:

* `ObjectDisposedException` after `Dispose`.
* [`JuceAudioException`](juce-audio-exception.md) if the native call
  returns non-`Ok`. Inspect the `Status` property to branch.
