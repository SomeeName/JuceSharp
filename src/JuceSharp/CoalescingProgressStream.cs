using System.Runtime.CompilerServices;
using System.Threading.Channels;

namespace JuceSharp;

/// <summary>
/// Turns a push-shaped long-running operation (one that hands its caller a
/// <c>report</c> callback) into a pull-shaped
/// <see cref="IAsyncEnumerable{T}"/> that a UI can <c>await foreach</c>.
///
/// <para>
/// <b>Why this exists.</b> Every stem-separation backend used to publish
/// through <see cref="IProgress{T}"/>, which pushes: the producer decides how
/// often the consumer is called, and the consumer has no way to say "I am
/// behind". Every consumer therefore grew its own throttle (the diagnostics
/// CLI had a +1 % filter, the DotCharter view-model a timer), and those
/// throttles are what a progress stream removes.
/// </para>
///
/// <para>
/// <b>The contract, in four lines.</b>
/// <list type="number">
///   <item>The producer NEVER blocks on the consumer. Publication is a
///     <see cref="ChannelWriter{T}.TryWrite"/> into a BOUNDED channel with
///     <see cref="BoundedChannelFullMode.DropOldest"/>, so a slow UI costs
///     stale samples, never inference throughput. That also makes publication
///     callable from any thread — which matters, because demucs.cpp reports
///     from a WORKER thread and the python sidecar from its stdout reader.</item>
///   <item>The TERMINAL item is never coalesced away. DropOldest evicts the
///     OLDEST queued item, and the terminal item is by construction the last
///     thing written before the writer completes, so it always survives.
///     Backends put the whole result on it precisely so that dropping interim
///     samples can never lose information.</item>
///   <item>A backend fault surfaces from <c>MoveNextAsync</c> with its
///     ORIGINAL exception type, AFTER every sample published before it. The
///     writer is completed WITHOUT an exception and the fault is re-thrown by
///     awaiting the worker's <see cref="Task"/> once the channel has drained,
///     so the type, the message and the stack are the backend's own and do not
///     depend on channel plumbing. Handing the exception to
///     <c>Complete(error)</c> instead would make the observed type depend on
///     how the reader is written: <see cref="ChannelReader{T}.ReadAllAsync"/>
///     re-throws it as-is, but <see cref="ChannelReader{T}.ReadAsync"/> wraps
///     it in a <see cref="ChannelClosedException"/> — and a consumer that
///     catches <c>StemSeparationVramException</c> to offer "retry on CPU"
///     must not be at the mercy of that difference.</item>
///   <item>Cancelling the enumeration cancels the WORK. The token is linked,
///     so both an explicit token and an abandoned enumeration (a
///     <c>break</c> out of the <c>await foreach</c>, which disposes the
///     enumerator) stop the backend. A cancelled run surfaces as
///     <see cref="OperationCanceledException"/> from <c>MoveNextAsync</c>,
///     never as a silent early end of stream.
///     <b>Read the ABANDONMENT paragraph below before relying on this</b>: it
///     requests cancellation and returns, and does not WAIT for the backend to
///     notice.</item>
/// </list>
/// </para>
///
/// <para>
/// <b>ABANDONMENT IS FIRE-AND-FORGET, and that is the one thing item 4 does
/// NOT promise.</b> Breaking out of the <c>await foreach</c> REQUESTS
/// cancellation and returns; it does not wait for the backend to notice. So
/// when <c>DisposeAsync</c> returns, the python child may still be running and
/// <c>StemSeparator</c>'s process-global GPU slot may still be held — a caller
/// that breaks out and immediately starts a second separation queues behind the
/// run it believes it stopped. There is no handle to wait on here by design (a
/// bounded wait inside an enumerator's disposal is a stall with no owner); if
/// you need one, drive the backend directly and keep its <c>Task</c>. The
/// linked <see cref="CancellationTokenSource"/> is deliberately NOT disposed on
/// that path until the worker completes, because the worker is still using its
/// token.
/// </para>
/// </summary>
public static class CoalescingProgressStream
{
    /// <summary>
    ///     Queue depth before the oldest un-read sample is dropped. 32 is far more
    ///     than any real consumer is behind by; it exists so a consumer that
    ///     stalls briefly (a UI doing a layout pass) keeps the LAST 32 samples
    ///     rather than only the newest.
    ///     <para>
    ///         <b>It does not protect phase transitions, and this doc claimed it
    ///         did until W4-C2.</b>
    ///         <see cref="BoundedChannelFullMode.DropOldest" /> drops strictly by
    ///         AGE and knows nothing about an item's content, so a
    ///         <c>loading → inference</c> transition is evicted exactly as readily
    ///         as a fraction once a demucs run — which reports many samples a
    ///         second — has pushed 32 items past a stalled reader. The only item
    ///         the mode genuinely protects is the TERMINAL one (contract item 2),
    ///         and that is correct because it is the NEWEST. A consumer that must
    ///         see every transition needs a separate lane or a coalescing key, not
    ///         a bigger DropOldest queue.
    ///     </para>
    /// </summary>
    public const int DefaultCapacity = 32;

    /// <summary>
    /// Runs <paramref name="work"/> and streams everything it reports.
    /// </summary>
    /// <typeparam name="T">The progress item type.</typeparam>
    /// <param name="work">
    /// The operation. It is handed a <c>report</c> callback — thread-safe,
    /// non-blocking, never throwing — and a cancellation token linked to the
    /// enumerator's. It must publish its own terminal item before returning;
    /// this helper adds nothing to the stream.
    /// <para>
    /// The work is NOT started until the returned sequence is first enumerated,
    /// and is started at most once (the sequence is single-shot: enumerate it
    /// twice and the second enumeration runs the work again).
    /// </para>
    /// </param>
    /// <param name="capacity">Queue depth; see <see cref="DefaultCapacity"/>.</param>
    /// <param name="ct">Cancellation for both the enumeration and the work.</param>
    public static async IAsyncEnumerable<T> Create<T>(
        Func<Action<T>, CancellationToken, Task> work,
        int capacity = DefaultCapacity,
        [EnumeratorCancellation] CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(work);
        ArgumentOutOfRangeException.ThrowIfLessThan(capacity, 1);

        var channel = Channel.CreateBounded<T>(new BoundedChannelOptions(capacity)
        {
            FullMode = BoundedChannelFullMode.DropOldest,
            SingleReader = true,
            // The demucs.cpp progress callback arrives on a worker thread and
            // the python sidecar's on its stdout reader, so the writer is NOT
            // single — TryWrite has to stay the thread-safe overload.
            SingleWriter = false,
            AllowSynchronousContinuations = false,
        });

        var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var worker = RunWorkerAsync(channel.Writer, work, cts.Token);
        try
        {
            // CancellationToken.None: the reader must DRAIN. Cancelling the
            // read would end the stream before the terminal item and before
            // the worker's exception could be observed; the token's job is to
            // stop the WORK, which it does through the linked source.
            await foreach (var item in channel.Reader.ReadAllAsync(CancellationToken.None).ConfigureAwait(false))
                yield return item;

            // Drained. Re-throw the backend's own exception, with its own type.
            await worker.ConfigureAwait(false);
        }
        finally
        {
            if (!worker.IsCompleted)
            {
                // The consumer walked away mid-stream (a `break`, which disposes
                // the enumerator). Stop the backend.
                //
                // CancelAsync, not Cancel: Cancel runs the backend's registered
                // cancellation callbacks SYNCHRONOUSLY on whichever thread broke
                // out of the loop, which on a UI host is the UI thread.
                await cts.CancelAsync().ConfigureAwait(false);

                // …and the CTS OUTLIVES this scope. The worker is still running
                // and still using cts.Token — `CancellationToken.WaitHandle`
                // throws ObjectDisposedException outright, and both
                // `CreateLinkedTokenSource(ct)` and `Task.Delay(250, ct)` are on
                // that token in the python path — so disposing it here (which is
                // what this used to do) pulled it out from under the backend.
                // The continuation both observes the fault, so an abandoned
                // separation cannot raise TaskScheduler.UnobservedTaskException,
                // and disposes the source once nobody can be using it.
                _ = worker.ContinueWith(
                    static (t, state) =>
                    {
                        _ = t.Exception;
                        ((CancellationTokenSource)state!).Dispose();
                    },
                    cts,
                    CancellationToken.None,
                    TaskContinuationOptions.ExecuteSynchronously,
                    TaskScheduler.Default);
            }
            else
            {
                cts.Dispose();
            }
        }
    }

    /// <summary>
    /// Runs the work and ALWAYS completes the writer, so a reader can never
    /// hang on a producer that threw before publishing anything. The writer is
    /// completed without an exception on purpose — see the class remarks.
    /// </summary>
    private static async Task RunWorkerAsync<T>(
        ChannelWriter<T> writer, Func<Action<T>, CancellationToken, Task> work, CancellationToken ct)
    {
        try
        {
            // TryWrite on a DropOldest bounded channel always succeeds, so the
            // report callback is total: it cannot block, fail, or throw, which
            // is what makes it safe to call from a native worker thread.
            await work(item => writer.TryWrite(item), ct).ConfigureAwait(false);
        }
        finally
        {
            writer.TryComplete();
        }
    }
}

/// <summary>
/// <see cref="IProgress{T}"/> that invokes the handler ON THE REPORTING
/// THREAD. <see cref="Progress{T}"/> posts to the captured
/// <see cref="SynchronizationContext"/>, which would re-introduce exactly the
/// unordered-delivery problem a progress stream exists to remove: the sample
/// would arrive at the channel after the terminal item.
/// </summary>
public sealed class ImmediateProgress<T>(Action<T> handler) : IProgress<T>
{
    private readonly Action<T> _handler = handler ?? throw new ArgumentNullException(nameof(handler));

    /// <inheritdoc />
    public void Report(T value) => _handler(value);
}
