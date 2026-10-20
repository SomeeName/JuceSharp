namespace JuceSharp.Interop;

/// <summary>
///     ONE dedicated, long-running thread that the seconds-long engine calls are
///     marshalled onto, so an app's UI thread never sits inside one.
///     <para>
///         <b>Never the thread pool.</b> The calls this exists for are not "slow"
///         in the pool's sense — a cold VST3 scan of a large amp sim measures
///         25 s+, <c>dca_engine_show_asio_panel</c> is a MODAL vendor dialog with
///         no upper bound at all, and a 6-stem project's decode + resample is a
///         multi-second block. A <c>Task.Run</c> for any of those parks a pool
///         thread for the duration and the pool answers by INJECTING threads (one
///         every ~0.5 s), which starves every other async workload in the host
///         while the dialog is open. One owned thread is both cheaper and honest.
///     </para>
///     <para>
///         <b>It is deliberately SERIAL.</b> Two device reconfigurations must not
///         overlap; the queue is the mutual exclusion, and it also makes the
///         drain-on-dispose tractable — see <see cref="Shutdown" />.
///     </para>
///     <para>
///         The thread is created on the FIRST enqueue, so an engine whose host
///         never calls an async member never has one.
///     </para>
/// </summary>
internal sealed class EngineWorker
{
    private readonly string _ownerName;
    private readonly object _gate = new();
    private readonly Queue<WorkItem> _queue = new();

    private Thread? _thread;
    private bool _shutdown;
    private long _started;
    private int _concurrent;
    private int _maxConcurrent;

    /// <summary>
    ///     How long <see cref="JuceSharp.JuceAudioEngine.Dispose()" /> waits for
    ///     the item currently IN FLIGHT before it stops waiting and lets the
    ///     handle drain take over.
    ///     <para>
    ///         <b>Deliberately NOT <c>NativeHandleOwner.DefaultDrainTimeoutMs</c>,
    ///         which is what it used to be.</b> Dispose is normally called on the
    ///         host's UI thread, and the calls this worker exists for have no
    ///         upper bound at all —
    ///         <c>JuceAudioEngine.ShowAsioControlPanelAsync</c> is a MODAL vendor
    ///         dialog that returns when the USER closes it. A 60 s join there is
    ///         a minute-long frozen application during shutdown, and if the panel
    ///         is pumping on a message thread the host has made the UI thread it
    ///         is a hard deadlock until expiry. The drain that follows already
    ///         handles a wedged call honestly (it refuses to free the handle,
    ///         logs at Error and leaks it), so the join only has to cover the
    ///         ORDINARY case of an item a few hundred milliseconds from
    ///         returning.
    ///     </para>
    /// </summary>
    internal const int DisposeJoinTimeoutMs = 2_000;

    public EngineWorker(string ownerName) => _ownerName = ownerName;

    /// <summary>How many queued items have actually been DISPATCHED. A test hook:
    /// it is how "a token cancelled before the call started never reached native"
    /// is asserted without a timing argument.</summary>
    internal long StartedCount => Interlocked.Read(ref _started);

    /// <summary>The highest number of items ever running at once. 1 by
    /// construction — asserted, because "serialised" is the contract.</summary>
    internal int MaxObservedConcurrency => Volatile.Read(ref _maxConcurrent);

    /// <summary>The worker thread, or null if nothing was ever enqueued. A test
    /// hook: <c>IsAlive</c> after a dispose is how the teardown is asserted.</summary>
    internal Thread? Thread { get { lock (_gate) return _thread; } }

    /// <summary>Test hook: has <see cref="Shutdown" /> taken the gate and
    /// abandoned the queue? True implies no queued item can ever run — which is
    /// what a dispose-versus-queued-call test has to wait for, and is strictly
    /// later than the engine's own "shutdown requested" flag.</summary>
    internal bool QueueClosed { get { lock (_gate) return _shutdown; } }

    /// <summary>
    ///     Queue one native call. Cancellation is honoured BEFORE the item starts
    ///     — either here (already-cancelled token: the item is never queued) or at
    ///     dispatch. Once native has been entered these exports cannot be
    ///     interrupted, and the task therefore completes normally even if the
    ///     token is cancelled meanwhile; that is documented on every caller.
    /// </summary>
    public Task<T> Enqueue<T>(Func<T> work, CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested) return Task.FromCanceled<T>(cancellationToken);

        // RunContinuationsAsynchronously: a continuation must never run INSIDE
        // this worker thread, or a caller's `await` body would execute where the
        // next native call is supposed to be — and Abandon() completes tasks
        // while holding _gate.
        var tcs = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        var item = new WorkItem(
            Run: () =>
            {
                if (cancellationToken.IsCancellationRequested)
                {
                    tcs.TrySetCanceled(cancellationToken);
                    return;
                }
                Interlocked.Increment(ref _started);
                var live = Interlocked.Increment(ref _concurrent);
                var seen = Volatile.Read(ref _maxConcurrent);
                if (live > seen) Volatile.Write(ref _maxConcurrent, live);
                try { tcs.TrySetResult(work()); }
                catch (OperationCanceledException oce) { tcs.TrySetCanceled(oce.CancellationToken); }
                catch (Exception ex) { tcs.TrySetException(ex); }
                finally { Interlocked.Decrement(ref _concurrent); }
            },
            Abandon: () => tcs.TrySetException(new ObjectDisposedException(
                _ownerName,
                "The engine was disposed before this queued call reached the native engine.")));

        lock (_gate)
        {
            if (_shutdown)
            {
                item.Abandon();
                return tcs.Task;
            }
            _queue.Enqueue(item);
            _thread ??= StartThreadLocked();
            Monitor.Pulse(_gate);
        }
        return tcs.Task;
    }

    private Thread StartThreadLocked()
    {
        var t = new Thread(Loop)
        {
            IsBackground = true,
            Name = $"{_ownerName} worker",
        };
        t.Start();
        return t;
    }

    private void Loop()
    {
        while (true)
        {
            WorkItem item;
            lock (_gate)
            {
                while (_queue.Count == 0 && !_shutdown) Monitor.Wait(_gate);
                if (_queue.Count == 0) return;   // shutdown, nothing left to do
                item = _queue.Dequeue();
            }
            item.Run();                          // never under the lock
        }
    }

    /// <summary>
    ///     Stop accepting work, fault everything still QUEUED, and wait for the
    ///     item currently IN FLIGHT to return.
    ///     <para>
    ///         <b>Order matters and this must run BEFORE the handle drain.</b> An
    ///         in-flight item is inside a native call holding the engine handle
    ///         pinned; if <c>NativeHandleOwner.TakeForDispose</c> ran first it
    ///         would wait its whole 60 s deadline for a call this method is about
    ///         to wait for anyway, and on expiry would LEAK the engine. Joining
    ///         here means the drain that follows finds nothing in flight.
    ///     </para>
    ///     <para>
    ///         A queued-but-unstarted item is faulted with
    ///         <see cref="ObjectDisposedException" /> rather than left pending:
    ///         the alternative is a host awaiting a task that can never complete.
    ///     </para>
    /// </summary>
    /// <param name="joinTimeoutMs">0 on the FINALIZER path — there is one
    /// finalizer thread in the process and it may not wait.</param>
    public void Shutdown(int joinTimeoutMs)
    {
        Thread? t;
        lock (_gate)
        {
            _shutdown = true;
            while (_queue.Count > 0) _queue.Dequeue().Abandon();
            Monitor.PulseAll(_gate);
            t = _thread;
        }

        if (t is null) return;
        // Disposing FROM the worker (a continuation is never scheduled here, but
        // a caller could still contrive it) must not join itself.
        if (ReferenceEquals(t, System.Threading.Thread.CurrentThread)) return;
        if (joinTimeoutMs <= 0)
        {
            if (!t.Join(0))
            {
                JuceLog.Emit(JuceLogLevel.Warn,
                    $"{_ownerName}: an async engine call was still running at finalization. "
                    + "Not waiting — the finalizer thread may not block.");
            }
            return;
        }
        if (!t.Join(joinTimeoutMs))
        {
            JuceLog.Emit(JuceLogLevel.Error,
                $"{_ownerName}: an async engine call did not return within {joinTimeoutMs} ms of dispose. "
                + "Continuing — the handle drain that follows will refuse to free the handle while that "
                + "call is live, and leak it instead.");
        }
    }

    private readonly record struct WorkItem(Action Run, Action Abandon);
}
