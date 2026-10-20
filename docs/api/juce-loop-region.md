# JuceLoopRegion

Polling-based A-B loop for a `JuceAudioSource`. When attached, a
low-priority timer watches the source's `AudiblePosition` and seeks
back to `Start` as soon as it crosses `End`.

```csharp
src.Loop = new JuceLoopRegion(
    start: TimeSpan.FromSeconds(8),
    end:   TimeSpan.FromSeconds(16));
src.Play();

// stop looping
src.Loop = null;
```

Precision is the configured `PollIntervalMs` (default 5 ms) — good
enough for charting / practice.

> ⚠️ **Warning**
> **There is no sample-accurate native loop.** The ABI exports none, and this
> polling region is the only looping JuceSharp offers. Its private timer tick reads
> `AudiblePosition` on a `System.Threading.Timer` and calls `Seek(Start)` once
> it crosses `End`, so the overshoot is bounded by `PollIntervalMs`, not by a
> sample count.

The timer is published **stopped** and only armed after re-checking that the
region was not disposed meanwhile, so attaching and disposing from different
threads cannot leave a timer running against a dead source.

## API

| Member | Description |
|--------|-------------|
| `JuceLoopRegion(TimeSpan start, TimeSpan end, int pollIntervalMs = 5)` | Throws on `end ≤ start` or `pollIntervalMs < 1`. |
| `TimeSpan Start / End` | |
| `int PollIntervalMs` | |
| `Dispose()` | Stops polling; idempotent. A disposed region cannot be re-armed: assigning one to `JuceAudioSource.Loop` throws `ObjectDisposedException` and leaves the existing loop running. |

Setting `JuceAudioSource.Loop` automatically attaches / detaches /
disposes the previous region.
