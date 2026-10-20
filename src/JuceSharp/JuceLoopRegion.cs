namespace JuceSharp;

/// <summary>
/// Polling-based A-B loop region for a <see cref="JuceAudioSource"/>.
/// When attached, a low-priority timer watches the source's audible
/// position and seeks back to <see cref="Start"/> as soon as it crosses
/// <see cref="End"/>.
///
/// Precision is the configured <see cref="PollIntervalMs"/> (default
/// 5 ms), which is good enough for practice-loop UX. There is no
/// sample-accurate alternative to reach for: the native ABI exports no loop
/// region at all (this comment used to promise one "once it lands", and
/// nothing has ever been landing).
/// <para>A region is attached to exactly ONE source, by
/// <see cref="JuceAudioSource.Loop"/>, and cannot be moved to a second.</para>
/// </summary>
public sealed class JuceLoopRegion : IDisposable
{
    /// <summary>Position the source is seeked back to each time the loop wraps.</summary>
    public TimeSpan Start { get; }

    /// <summary>Position that triggers the wrap: the poll seeks back to
    /// <see cref="Start"/> as soon as the audible position reaches or passes it.</summary>
    public TimeSpan End   { get; }

    /// <summary>Interval in milliseconds between position polls, and therefore the
    /// worst-case overshoot past <see cref="End"/>. Defaults to 5 ms.</summary>
    public int      PollIntervalMs { get; }

    private Timer?           _timer;
    private JuceAudioSource? _source;
    private int              _disposed;

    /// <summary>
    /// Create a detached A-B region; attach it by assigning it to
    /// <see cref="JuceAudioSource.Loop"/>. Throws <see cref="ArgumentException"/> when
    /// <paramref name="end"/> is not after <paramref name="start"/>, and
    /// <see cref="ArgumentOutOfRangeException"/> when
    /// <paramref name="pollIntervalMs"/> is below 1.
    /// </summary>
    /// <param name="start">Where the loop restarts from.</param>
    /// <param name="end">Where the loop wraps.</param>
    /// <param name="pollIntervalMs">Position poll period in milliseconds; also the loop's precision.</param>
    public JuceLoopRegion(TimeSpan start, TimeSpan end, int pollIntervalMs = 5)
    {
        if (end <= start)
            throw new ArgumentException("Loop end must be after loop start.", nameof(end));
        if (pollIntervalMs < 1)
            throw new ArgumentOutOfRangeException(nameof(pollIntervalMs));
        Start          = start;
        End            = end;
        PollIntervalMs = pollIntervalMs;
    }

    internal void Attach(JuceAudioSource source)
    {
        // A DISPOSED region can never loop again, and saying so is the whole
        // point: Dispose() resets both _source and _timer to null, so a disposed
        // region passed the CAS below, published a timer, hit the race check a
        // few lines down, tore the timer straight back off and returned
        // NORMALLY. JuceAudioSource.Loop then stored it and reported a live
        // loop that never seeked — verbatim the failure that setter's own doc
        // says was fixed, which it was only for `src.Loop = src.Loop`
        // (W4-M5). The throw happens BEFORE the _source CAS, and the setter
        // attaches before it swaps, so a rejected region leaves the existing
        // loop running and untouched.
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);

        // A region belongs to ONE source for life. Re-attaching to a second one
        // used to be a silent no-op that kept the FIRST source — so a host that
        // moved a loop from one stem to another got a loop that still seeked the
        // old stem, with both properties reporting the region it thought it had.
        // The CAS also makes the write visible before the timer is published, so
        // Tick() on a thread-pool thread cannot observe a null _source.
        var existing = Interlocked.CompareExchange(ref _source, source, null);
        if (existing is not null && !ReferenceEquals(existing, source))
        {
            throw new InvalidOperationException(
                "This JuceLoopRegion is already attached to a different JuceAudioSource. "
                + "Create a new region for the second source — a region owns exactly one.");
        }

        // Created STOPPED and published before it is armed (concurrency audit
        // #11): the old `_timer = new Timer(..., period, period)` assigned a
        // running timer AFTER the region was already reachable, so a Dispose()
        // that landed between the source write and that assignment saw a null
        // field, disposed nothing, and the timer then fired every 5 ms forever
        // — rooting the region and the source it points at. A practice-loop UI
        // reassigning Loop on every A/B change accumulated one per change.
        var timer = new Timer(_ => Tick(), null, Timeout.Infinite, Timeout.Infinite);
        if (Interlocked.CompareExchange(ref _timer, timer, null) is not null)
        {
            timer.Dispose(); // attached twice: keep the first
            return;
        }

        if (Volatile.Read(ref _disposed) != 0)
        {
            // Dispose ran between the source write and the publish above and
            // found nothing to dispose; finish its job here.
            Interlocked.Exchange(ref _timer, null)?.Dispose();
            return;
        }

        timer.Change(PollIntervalMs, PollIntervalMs);
    }

    /// <summary>True while a poll timer is published for this region. Test seam
    /// for the attach-vs-dispose race: a disposed region must never hold one.</summary>
    internal bool HasTimerForTests => Volatile.Read(ref _timer) is not null;

    private void Tick()
    {
        if (Volatile.Read(ref _disposed) != 0) return;
        var src = Volatile.Read(ref _source);
        if (src is null) return;
        try
        {
            if (src.AudiblePosition >= End)
                src.Seek(Start);
        }
        catch (ObjectDisposedException)
        {
            Dispose();
        }
        catch (Exception ex)
        {
            JuceLog.Emit(JuceLogLevel.Warn, $"Loop timer threw: {ex.GetType().Name}: {ex.Message}");
        }
    }

    /// <summary>Stop the poll timer and detach from the source. Idempotent and safe to
    /// call from any thread, including from inside the timer callback.</summary>
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        Interlocked.Exchange(ref _timer, null)?.Dispose();
        Volatile.Write(ref _source, null);
    }
}
