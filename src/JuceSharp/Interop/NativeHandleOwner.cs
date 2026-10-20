namespace JuceSharp.Interop;

/// <summary>How a <see cref="NativeHandleOwner.TakeForDispose(string)" /> ended.</summary>
internal enum HandleDrain
{
    /// <summary>Another Dispose (or the finalizer) already claimed the handle.
    /// Nothing to destroy, and nothing went wrong.</summary>
    AlreadyClaimed,
    /// <summary>Every in-flight P/Invoke returned. The handle is safe to destroy.</summary>
    Drained,
    /// <summary>A native call was STILL running when the drain deadline expired.
    /// The handle is deliberately NOT returned: the native object is leaked
    /// rather than freed under a live call. A leak beats a use-after-free.</summary>
    TimedOut,
}

/// <summary>
/// Tracks a native handle plus the number of interop calls currently using
/// it, so Dispose can drain in-flight calls before destroying the handle.
/// Prevents the use-after-free window where one thread polls a wrapper
/// (e.g. a 4 ms position timer reading <c>AudiblePosition</c>) while another
/// thread disposes it: previously the handle could be destroyed between the
/// alive-check and the P/Invoke.
/// </summary>
/// <remarks>
/// <para>
/// <b>The drain never frees under a live call.</b> Until 0.61 the wait was a
/// bounded <c>SpinWait</c> that gave up after 2 s and <i>returned the handle
/// anyway</i>, so every caller destroyed it while native was still inside the
/// call. That is not a theoretical window: the calls routinely held under
/// <see cref="Acquire" /> that exceed 2 s are the ones a user reaches —
/// <c>dca_engine_show_asio_panel</c> is a MODAL vendor dialog with no upper
/// bound at all, a device <c>Reconfigure</c> reopens the driver, and a
/// first-run VST3 scan of a large amp sim measures 25 s+. Now the wait is
/// driven by the last <see cref="Release" /> (a Monitor pulse, not a spin, so
/// a slow drain does not burn a core), and on expiry the handle is reported as
/// <see cref="HandleDrain.TimedOut" /> and NOT handed back — the caller logs
/// and leaks it.
/// </para>
/// <para>
/// The wait is re-polled on a short bounded <c>Monitor.Wait</c> rather than
/// relying on the pulse alone, so a <see cref="Release" /> that pulses before
/// the waiter is inside the lock cannot be missed. Every path that decrements
/// <c>_inFlight</c> goes through <see cref="Release" /> for the same reason —
/// a failed <see cref="Acquire" /> that decremented directly would leave a
/// waiter parked until the deadline.
/// </para>
/// </remarks>
internal sealed class NativeHandleOwner
{
    /// <summary>
    /// How long a dispose waits for in-flight native calls before giving up and
    /// leaking the handle. Generous on purpose — it has to clear a modal ASIO
    /// control panel and a cold VST3 scan, both of which are legitimate and
    /// both of which blew straight through the old 2 s ceiling. It is a
    /// backstop against a wedged driver, not a routine deadline.
    /// </summary>
    internal const int DefaultDrainTimeoutMs = 60_000;

    private readonly object _drainGate = new();

    private nint _handle;
    private int _inFlight;
    private volatile bool _disposeRequested;

    public NativeHandleOwner(nint handle) => _handle = handle;

    public bool IsAlive => Volatile.Read(ref _handle) != nint.Zero;

    /// <summary>
    /// Pin the handle for the duration of one native call. Every successful
    /// Acquire must be paired with <see cref="Release"/> (use try/finally).
    /// Throws <see cref="ObjectDisposedException"/> when already disposed.
    /// </summary>
    public nint Acquire(string ownerName)
    {
        Interlocked.Increment(ref _inFlight);
        var h = Volatile.Read(ref _handle);
        if (h == nint.Zero)
        {
            // Release, not a bare Decrement: a concurrent TakeForDispose may
            // already be waiting on this count reaching zero.
            Release();
            throw new ObjectDisposedException(ownerName);
        }
        return h;
    }

    /// <summary>
    /// Like <see cref="Acquire"/> but returns <c>nint.Zero</c> instead of
    /// throwing. Callers must call <see cref="Release"/> ONLY when the
    /// returned handle is non-zero.
    /// </summary>
    public nint TryAcquire()
    {
        Interlocked.Increment(ref _inFlight);
        var h = Volatile.Read(ref _handle);
        if (h == nint.Zero) Release();
        return h;
    }

    public void Release()
    {
        if (Interlocked.Decrement(ref _inFlight) != 0) return;
        if (!_disposeRequested) return;
        lock (_drainGate) Monitor.PulseAll(_drainGate);
    }

    /// <summary>Read the current handle without pinning it. For raw-handle
    /// interop only — the caller must guarantee the owner outlives any use
    /// of the returned value.</summary>
    public nint TryPeek() => Volatile.Read(ref _handle);

    /// <summary>
    /// Take ownership of the handle for destruction. No new acquires can
    /// succeed afterwards, and the call blocks until every in-flight native
    /// call has returned. Returns the handle to destroy, or
    /// <c>nint.Zero</c> when there is nothing to destroy — either the owner
    /// was already disposed, or the drain timed out, in which case the native
    /// object is leaked rather than freed under a live call.
    /// </summary>
    public nint TakeForDispose(string ownerName)
        => TakeForDispose(ownerName, DefaultDrainTimeoutMs, out _);

    /// <summary>
    /// <see cref="TakeForDispose(string)"/> with the outcome reported, so a
    /// caller can tell "already disposed" (do nothing) from "timed out"
    /// (log, and skip the destroy). The timeout is a parameter only so tests
    /// can drive the expiry path deterministically.
    /// </summary>
    internal nint TakeForDispose(string ownerName, int timeoutMs, out HandleDrain outcome)
    {
        var h = Interlocked.Exchange(ref _handle, nint.Zero);
        if (h == nint.Zero)
        {
            outcome = HandleDrain.AlreadyClaimed;
            return nint.Zero;
        }

        // Announce the dispose BEFORE the first read of _inFlight: a Release
        // that runs after this write always pulses, and one that ran before it
        // has already decremented the count the loop below re-reads.
        _disposeRequested = true;

        lock (_drainGate)
        {
            var deadline = timeoutMs < 0 ? long.MaxValue : Environment.TickCount64 + timeoutMs;
            while (Volatile.Read(ref _inFlight) > 0)
            {
                var remaining = deadline - Environment.TickCount64;
                if (remaining <= 0)
                {
                    outcome = HandleDrain.TimedOut;
                    JuceLog.Emit(JuceLogLevel.Error,
                        $"{ownerName}: a native call was still running after {timeoutMs} ms of " +
                        "drain-on-dispose. LEAKING the native handle instead of destroying it — " +
                        "freeing it here would be a use-after-free. Something is wedged in the " +
                        "native engine (a modal driver panel, or a stalled device).");
                    return nint.Zero;
                }
                // Bounded re-poll: the pulse is the fast path, the timeout is
                // what makes a pulse delivered before we were inside the lock
                // impossible to miss.
                Monitor.Wait(_drainGate, (int)Math.Min(remaining, 25));
            }
        }

        outcome = HandleDrain.Drained;
        return h;
    }
}
