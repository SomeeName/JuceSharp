# Failure Modes & Runtime Health

Which exception you can actually catch, what only reaches the log, what the
engine keeps running in a degraded state, and how to tell a healthy engine from
a struggling one. The `try`/`catch` shapes and the `JuceAudioStatus` table are in
[Error Handling Patterns](error-handling.md); this page is the layer around
them — the exception types a consumer meets across the *whole* surface, the
values that are sentinels rather than throws, and the counters.

## The exception map

| Type | Thrown by | Means |
|---|---|---|
| `DllNotFoundException` | the first native call in the process | `juce_sharp.dll` is not on the search path. It is a **build artifact and is not in git** — see [First sound](../getting-started/first-sound.md) |
| `JuceAbiMismatchException` : `InvalidOperationException` | `JuceAudioEngine`'s constructor, and the first touch of `JuceDspModuleCatalog.Modules` | the DLL's `DCA_ABI_VERSION` is not **exactly** the wrapper's. Carries `ExpectedAbiVersion` / `NativeAbiVersion`. Rebuild the DLL; never widen the check |
| `JuceAudioException` | any wrapper call whose `dca_*` export returned non-`Ok` | carries `Status` (`JuceAudioStatus`), `StatusCode` and `Operation` (the entry-point name). Branch on `Status`, log `Operation` |
| `ArgumentOutOfRangeException` | `JuceEngineConfig.Validate()`, run by the engine constructor | a config field is negative or absurd (`SampleRate` > 384,000, `BufferFrames` > 16,384, channels > 64, an undefined `AudioDriver`). Managed-side, before any P/Invoke |
| `ObjectDisposedException` | any member of a disposed wrapper | including "disposed while your call was queued on the engine's async worker", which says so in the message |
| `NotSupportedException` | `JuceStemSeparation.Separate` when `IsSupported` is false; `RenderToWav` asked to stretch or shift | a **capability** answer, deliberately a different type from a failure — it is what a composite service branches on to try the next backend |
| `StemSeparationException` (+ six derived) | `JuceSharp.StemSeparation`'s sidecar backend | a classified failure: `Kind`, `Stderr`, and derived types for VRAM, CUDA driver mismatch, CUDA unavailable, a missing package, model load, unsupported audio format |
| `OperationCanceledException` | the stems backends, on a cancelled run | **never** a `StemSeparationException` — a cancel is not a separation failure, and conflating them makes a user's Cancel read as a reason to fall through to the next backend |

`JuceAudioException` is public and constructible (`(message, status, operation)`
— the status sits *between* the two strings on purpose, so transposing them
cannot compile into a nonsense exception), which is how a backend outside the
wrapper reports a real native status instead of flattening it into a string.

### What the wrapper does with a `dca_*` status

There is one funnel: `JuceAudioEngine.Check(status, op)` throws
`JuceAudioException($"{op} failed with {status}.", status, op)` for anything that
is not `Ok`. The eight `DcaStatus` values map ordinal-for-ordinal onto the public
`JuceAudioStatus` (`Ok` 0, `InvalidArg` 1, `NotInitialized` 2, `Device` 3,
`Decode` 4, `Plugin` 5, `Cancelled` 6, `Internal` 99).

`Cancelled` (ABI v36) never reaches that funnel: it is **not a failure**, and
`JuceStemSeparation` turns it into `OperationCanceledException` before any
`Check`. Omitting it from an error table is the costly direction — a host that
classifies it as one falls through to the next backend after a user Cancel.

The second funnel is the one to know about: **`WarnIfFailed(status, op)`**, used
by fire-and-forget setters that are documented never to throw — volume, gains,
filters, clicks. A failure there is logged (`ILogger` at Warning **and**
`JuceLog` at `Warn`) and the call returns normally. So:

> **A gain that did not take does not throw.** If a setter appears to have no
> effect, read the log before reading the code.

## Logged, not thrown

`JuceLog` is the process-wide sink for every native line, independent of which
`ILogger` any one engine was given:

```csharp
JuceLog.MinimumLevel = JuceLogLevel.Info;   // or JUCESHARP_LOG_LEVEL=info
JuceLog.MessageReceived += (level, message) => Console.WriteLine($"[{level}] {message}");
```

Handlers run on the thread that produced the line and must not block. Each
subscriber is invoked inside its own `try`, so one throwing handler no longer
silently stops delivery to the rest.

These reach you **only** through the log:

* a discarded setter status (`WarnIfFailed`, above) — `Warn`;
* a handle drain that expired, leaking the native handle instead of freeing it
  under a live call — `Error`;
* a `JuceDspEditScope` that was garbage-collected while still open — `Error`,
  naming the seat, because until then that seat's rig published nothing to the
  audio callback while every edit returned success;
* a host progress handler that threw inside the stem-separation callback —
  `Warn`.

Treat `Error` from `JuceLog` as a defect report, not as telemetry: every one of
the lines above exists because the alternative was a crash or silence.

## Degraded but running

The engine is designed to keep going and say so, rather than fail the call.

* **A capability the build does not have reports so.** `JuceStemSeparation.IsSupported`
  is false on a DLL built without the native demucs backend, and `Separate`
  throws `NotSupportedException` rather than pretending. Probe the capability and
  branch; do not catch your way through it.
* **`JuceEngineFactory.OpenWithFallback` returns a null `Engine` instead of
  throwing** when no device on the host could be opened, with
  `OpenResult.Attempts` carrying one line per failed device. That is the normal
  "no sound card" path, and it is a state your UI has to render — it is not an
  error.
* **Clamping is silent, so read back rather than echo.** `SetMasterLimiter`
  clamps its ceiling and release; `GetMasterLimiter()` is the only honest answer
  about what is running. The same rule applies to anything else you set and then
  display.
* **A structure change is crossfaded over ~10 ms**, so a `JuceDspChain` call
  returning does not mean the change is fully audible yet. That is by design,
  not latency to wait out.

### Value-returning getters and their sentinels

Some members deliberately return a neutral value on a disposed engine instead of
throwing, because they are polled from timers:

| Member | On a disposed engine / failed read |
|---|---|
| `MasterLimiterReductionDb` | `0f` (also `0f` while idle or disabled) |
| `TryGetXrunStats` / `TryGetMidiTimingStats` | `false`, `stats` zeroed |
| `GetMidiTimingStats()` (the tuple overload) | `(0, 0, 0)` — which cannot distinguish "disposed" from "nothing scheduled yet". Prefer the `Try…` form |
| `ResetXrunStats` / `ResetMidiTimingStats` | `false` |
| `OutputLatencySamples`, `MasterLimiterLatencySamples` | **throw** `ObjectDisposedException`; they return `0` only when no device is open |

`MasterLimiterLatencySamples` is also `0` while the limiter is **off** — off, it
is not in the signal path at all. Anything aligning audio to a clock must
re-read `OutputLatencySamples` after a limiter toggle rather than cache it for
the session.

## The runtime health signal

`engine.TryGetXrunStats(out var x)` is the one honest read on how the audio
callback is actually doing — measured *in* the callback, not inferred from a
managed poll. Call `engine.ResetXrunStats()` once after opening so the counters
describe your run rather than the device open.

```csharp
if (engine.TryGetXrunStats(out var x))
{
    Console.WriteLine($"callbacks={x.Callbacks} late={x.LateCallbacks} " +
                      $"overruns={x.DeadlineOverruns} nearMisses={x.NearMisses} " +
                      $"driver={(x.DeviceXruns?.ToString() ?? "n/a")} load={x.Load:P0}");
}
```

**The three callback counters answer different questions and must never be added
together:**

| Counter | Question it answers | What you do |
|---|---|---|
| `LateCallbacks` | *Was there an audible dropout?* The gap before the callback exceeded 1.5× the previous block plus 0.5 ms, so the device rendered without us — whoever's fault | this is the number to show a user |
| `DeadlineOverruns` | *Were we the cause?* The callback overran its own block budget | lighten the rig, or raise `BufferFrames` |
| `NearMisses` | *Are we close?* Inside budget but near it; strictly disjoint from the overruns | a load warning, no action yet |

`DeviceXruns` is `int?` — what the **driver** reports, or `null` when it cannot
report at all. ASIO and DirectSound implement it; WASAPI and CoreAudio's
aggregate path do not. **Never render an absent count as zero**: "the driver
reported no dropouts" and "the driver cannot tell us" are opposite claims.

On the Null device `LateCallbacks` is routinely non-zero — it is a timer-paced
fake, not a driver. Do not treat that as a regression.

For scheduled MIDI the equivalent is `TryGetMidiTimingStats`: `Late` counts
events whose frame had already passed when their block ran (placed at sample 0),
and it is the only signal that your scheduling horizon is shorter than the
engine's own latency. `Dropped` counts events the queue could not hold — since
ABI v35 a full queue answers `DCA_ERR_DEVICE` rather than "bad argument", so a
lost note-off is distinguishable from a coding error.

## Related

* [Error Handling Patterns](error-handling.md) — the `JuceAudioStatus` table and
  the catch shapes.
* [Engine Health & Dropouts](health.md) — the counters in depth.
* [Logging & Diagnostics](logging.md) — sinks and levels.
* [Threading](threading.md) — the dispose-while-in-flight guarantee.
