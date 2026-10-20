# Clicks & the Render Clock

The engine has a click bus that synthesises a beat, a downbeat or a clap. You
can fire one immediately, or — since ABI v26 — schedule it on the output render
clock so it lands in an exact output sample.

## Firing one now

```csharp
engine.PlayClick(0);   // beat
engine.PlayClick(1);   // downbeat
engine.PlayClick(2);   // clap
```

Good enough for a "test the metronome" button. Not good enough for a metronome
that has to stay with a song: the click lands wherever the next callback puts
it, so it jitters by up to one buffer.

## Scheduling on the render clock

```csharp
if (engine.TryGetRenderClock(out long renderFrames, out long hostTicks))
{
    long target = renderFrames + (long)(0.5 * engine.SampleRate);   // half a second out
    engine.ScheduleClick(0, target);
}
```

`TryGetRenderClock` returns `{frames rendered so far, host clock ticks at that
block boundary}`. To convert a song time `T` into a render frame:

```
frame(T) = renderFrames + (T - audiblePosition) / speed * sampleRate
```

where `audiblePosition` is `JuceAudioSource.AudiblePosition` in seconds and
`speed` is the source's `TempoRatio`.

> ℹ️ **Note**
> **Output latency is deliberately absent from that formula.** The click lands
> in the same output samples as the note's own audio, so both cross the
> identical device buffer and reach the ear together, whatever the latency is.
> Adding `OutputLatencySamples` would push the click *early* by exactly that
> much.

The render clock is published **immediately after the transport pull**, not at
the end of the callback. That placement is what makes it and
`AudiblePosition` describe the *same* output sample; without it the two would
be a block apart and every scheduled click would be systematically off.

## Clearing the queue

```csharp
engine.ClearScheduledClicks();
```

> ⚠️ **Warning**
> **Clear on seek, on pause and on any speed change.** The render clock keeps
> running while the transport does not, so clicks scheduled against the old
> playhead are still in the queue and will fire at the wrong musical position.

## A worked metronome loop

```csharp
void ScheduleBar(JuceAudioSource song, double barStartSeconds, double beatSeconds, int beats)
{
    if (!engine.TryGetRenderClock(out long nowFrames, out _)) return;

    double audible = song.AudiblePosition.TotalSeconds;
    double speed   = song.TempoRatio;
    int    sr      = engine.SampleRate;

    for (int b = 0; b < beats; b++)
    {
        double t     = barStartSeconds + b * beatSeconds;
        long   frame = nowFrames + (long)((t - audible) / speed * sr);
        if (frame > nowFrames)
            engine.ScheduleClick(b == 0 ? 1 : 0, frame);
    }
}
```

Re-run it a bar ahead of time, and call `ClearScheduledClicks()` before you
re-schedule after a seek.
