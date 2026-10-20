namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Acquire/release gate between <see cref="OnnxStemSeparator.SeparateAsync"/>
/// and <see cref="OnnxStemSeparator.Dispose"/> — the managed twin of
/// <c>JuceSharp.Interop.NativeHandleOwner</c>.
/// </summary>
/// <remarks>
/// <para>
/// <c>SeparateAsync</c> used to check a disposed flag on the CALLING thread and
/// then queue the inference onto the pool, while <c>Dispose()</c> freed the
/// <c>InferenceSession</c> with nothing that waited for a pool thread already
/// inside <c>session.Run</c>. That is a use-after-free of a native ONNX Runtime
/// session for any caller that disposes before awaiting. DotCharter's own call
/// site never hit it (it awaits under a <c>using</c>), which is why it stayed
/// latent; the API is public and application-agnostic, so the ordering is
/// enforced here rather than trusted to callers.
/// </para>
/// <para>
/// The wait re-polls on a short bounded <c>Monitor.Wait</c> rather than relying
/// on the pulse alone, so an <see cref="Exit"/> that pulses before the disposer
/// is inside the lock cannot be missed. Every path that decrements
/// <c>_inFlight</c> goes through <see cref="Exit"/> for the same reason.
/// </para>
/// </remarks>
internal sealed class InferenceGuard
{
    /// <summary>How long a dispose waits for an in-flight inference before
    /// giving up and LEAKING the session. A 6-stem htdemucs pass on a CPU
    /// provider is minutes, not seconds, and the caller who disposes mid-run
    /// has already cancelled — the token is checked between chunks — so this
    /// is a backstop against a wedged provider, not a routine deadline.</summary>
    internal const int DefaultDrainTimeoutMs = 120_000;

    public enum DrainOutcome { Drained, TimedOut, AlreadyClaimed }

    private readonly object _drainGate = new();
    private int _inFlight;
    private int _disposeClaimed;
    private volatile bool _disposeRequested;

    public bool IsDisposeRequested => _disposeRequested;

    /// <summary>
    /// Pin the session for the duration of one inference. Every successful
    /// Enter must be paired with <see cref="Exit"/> (use try/finally). Throws
    /// <see cref="ObjectDisposedException"/> once a dispose has been requested,
    /// so an inference queued behind a Dispose never touches the freed session.
    /// </summary>
    public void Enter(string ownerName)
    {
        Interlocked.Increment(ref _inFlight);
        if (_disposeRequested)
        {
            // Exit, not a bare Decrement: a concurrent TakeForDispose may
            // already be waiting on this count reaching zero.
            Exit();
            throw new ObjectDisposedException(ownerName);
        }
    }

    public void Exit()
    {
        if (Interlocked.Decrement(ref _inFlight) != 0) return;
        if (!_disposeRequested) return;
        lock (_drainGate) Monitor.PulseAll(_drainGate);
    }

    /// <summary>
    /// Claim the session for destruction. No new <see cref="Enter"/> succeeds
    /// afterwards, and the call blocks until every in-flight inference has
    /// exited. Returns <see cref="DrainOutcome.Drained"/> when the caller may
    /// free the session, <see cref="DrainOutcome.TimedOut"/> when it must NOT
    /// (an inference is still running — freeing under it is the crash this
    /// type exists to prevent), and <see cref="DrainOutcome.AlreadyClaimed"/>
    /// on a second dispose.
    /// </summary>
    public DrainOutcome TakeForDispose(int timeoutMs = DefaultDrainTimeoutMs)
    {
        if (Interlocked.Exchange(ref _disposeClaimed, 1) != 0) return DrainOutcome.AlreadyClaimed;

        // Announce the dispose BEFORE the first read of _inFlight: an Exit that
        // runs after this write always pulses, and one that ran before it has
        // already decremented the count the loop below re-reads.
        _disposeRequested = true;

        lock (_drainGate)
        {
            var deadline = timeoutMs < 0 ? long.MaxValue : Environment.TickCount64 + timeoutMs;
            while (Volatile.Read(ref _inFlight) > 0)
            {
                var remaining = deadline - Environment.TickCount64;
                if (remaining <= 0) return DrainOutcome.TimedOut;
                Monitor.Wait(_drainGate, (int)Math.Min(remaining, 25));
            }
        }

        return DrainOutcome.Drained;
    }
}
