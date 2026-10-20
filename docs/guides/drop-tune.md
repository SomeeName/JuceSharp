# Drop Tune (Live Input Pitch Shift)

A digital drop-tune pedal on the live input, so a guitar in standard tuning can
play a song written in D standard without retuning.

```csharp
engine.SetInputPitchShift(-2f);        // seat 0: two semitones down
engine.SetInputPitchShiftMode(1);      // quality / splice-lag mode

engine.SetInputPitchShift(player: 1, semitones: -1f);       // ABI v31
engine.SetInputPitchShiftMode(player: 1, mode: 0);
```

## It is down-only, and that is not a limitation to work around

The shifter is **CSVD** (`src/dsp/cores/dcdsp_droptune.h`, ABI v25), and the
native core clamps the amount to `[-24, 0]`.

> ⚠️ **Warning**
> **Never put a phase vocoder on the live input.** Down-shifting has no
> latency floor, while the phase-vocoder family cannot be low-latency at all: a
> low E needs roughly a 48 ms analysis window, which is unplayable. Rubber
> Band's `LiveShifter` and Signalsmith are excellent algorithms and both are
> the wrong tool here.
>
> Rubber Band *is* used for playback stretch and shift on `JuceAudioSource` —
> that path is not real-time-interactive and can afford the latency. Do not
> reach for it on the input.

A related rule the implementation holds to: **never emit dry, unshifted audio
at the attack.** A shifter that falls back to dry while it primes turns every
pick attack into a wrong note.

## Modes

`SetInputPitchShiftMode` selects the splice-lag floor. The shipped modes
correspond to 10, 20 and 35 ms of maximum lag — lower lag is more responsive,
higher lag is cleaner on chords.

> ℹ️ **Note**
> **This parameter's meaning changed at ABI v25.** Before the CSVD rewrite it
> selected between a Rubber Band and a time-domain path. Any stored user
> setting from a pre-v25 build should be reset, not migrated.

## Per seat, since ABI v31

Until v31 the engine held one shifter and applied it to the raw device input
**before** the per-player split. On a couch with two guitars in different
tunings, dropping one dropped both — and the seat that had not asked for it was
detuned with nothing on screen saying so.

The two pre-v31 exports are now exactly their per-player form with
`player = 0`, so existing single-player code is unaffected.

## Rendering offline

`dca_droptune_process_offline` runs the same core over a segment list, against
a primed ring, so engage and disengage transitions happen mid-stream exactly as
they do live:

```c
dca_status dca_droptune_process_offline(
    int32_t        sample_rate,
    float          max_lag_ms,
    const float*   semitones_per_segment,
    const int32_t* segment_lengths,
    int32_t        num_segments,
    const float*   input,
    float*         output,
    int32_t        total_samples);
```

Mono. Segment lengths must sum to `total_samples`. Semitones are clamped to the
core's down-only range. In-place (`input == output`) is allowed.

The public managed wrapper is **`JuceAudioEngine.ProcessDropTuneOffline`**, a
static with two overloads — one taking a transition length in milliseconds, one
taking a `JuceDropTuneMode` (Tight/Balanced/Smooth → 10/20/35 ms). It validates
empty input, length equality, segment-array agreement, non-empty segments,
negative lengths and the **sum** managed-side, because a short sum silently
leaves original-pitch audio in the tail of an in-place render. The LIVE per-seat
shifter is a different surface: `SetInputPitchShift` /
`SetInputPitchShiftMode` (same enum) on `JuceAudioEngine`.
