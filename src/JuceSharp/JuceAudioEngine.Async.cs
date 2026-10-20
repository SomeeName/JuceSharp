using JuceSharp.Interop;

namespace JuceSharp;

/// <content>
///     ASYNC variants of the seconds-long engine calls (audit F10).
///     <para>
///         <b>The threading contract, once, for every member here.</b> The work is
///         marshalled onto ONE dedicated long-running thread per engine, created
///         lazily on first use and torn down in <see cref="Dispose()" /> BEFORE
///         the handle drain. It is never the thread pool: a cold VST3 scan is
///         25 s+ and the ASIO control panel is modal with no upper bound, and
///         parking a pool thread for that long makes the pool inject threads
///         while every other async workload in the host starves.
///     </para>
///     <para>
///         <b>The queue is SERIAL</b>, so two device changes cannot overlap — a
///         second <see cref="ReconfigureAsync" /> waits for the first, exactly as
///         two synchronous calls from one thread would.
///     </para>
///     <para>
///         <b>Cancellation is honoured BEFORE the call starts, and not after.</b>
///         A token already cancelled when the method is called never touches
///         native at all, and one cancelled while the item is still queued is
///         honoured at dispatch. None of these native exports is interruptible
///         once entered — there is no <c>dca_*_cancel</c> — so a token cancelled
///         mid-call is ignored and the task completes with the real result. That
///         is stated rather than faked: completing the task early while the
///         engine is still inside the call would hand the caller a disposed-looking
///         engine that is anything but.
///     </para>
///     <para>
///         The synchronous members are unchanged and remain the right call from a
///         background thread of your own. These exist for the UI thread.
///     </para>
/// </content>
public sealed partial class JuceAudioEngine
{
    private readonly object _workerGate = new();
    private EngineWorker? _worker;
    private bool _workerClosed;

    /// <summary>
    ///     The STATIC offline calls (<see cref="GenerateWaveformAsync" />,
    ///     <see cref="GenerateWaveformFromMemoryAsync" />,
    ///     <see cref="RenderToWavAsync" />) have no engine to hang a worker off,
    ///     so they share one process-wide. It is a background thread and idles at
    ///     zero cost; it is deliberately never shut down, because there is no
    ///     lifetime that could own it.
    ///     <para>
    ///         <b>Two consequences no caller could read anywhere until W4-E2, and
    ///         they are the price of the "never shut down" line above.</b> All
    ///         three members are SERIALISED against each other for the life of the
    ///         process — a six-stem project's waveforms are generated strictly one
    ///         at a time, and a <see cref="RenderToWavAsync" /> of a whole song
    ///         blocks every waveform request behind it. And because the worker is
    ///         never shut down, ONE wedged offline decode wedges all three members
    ///         permanently, with no recovery short of a process restart.
    ///     </para>
    ///     <para>
    ///         The serialisation is NOT the argument <see cref="EngineWorker" />
    ///         makes for itself — that one is "two device reconfigurations must not
    ///         overlap", and these three are pure, engine-free and side-effect-free
    ///         per call. It is kept at 1 anyway because these calls are I/O- and
    ///         memory-heavy (a full decode into RAM apiece) and N of them at once
    ///         is a different resource decision than the one this type was built to
    ///         make. A host that wants them concurrent should call the SYNCHRONOUS
    ///         <see cref="GenerateWaveform" /> / <see cref="RenderToWav" /> on its
    ///         own threads. Pinned either way by
    ///         <c>WrapperErgonomicsTests.TheOfflineWorkerIsSerial_AndSaysSo</c>
///         (<c>tests/JuceSharp.Tests/CoverageGaps/WrapperErgonomicsTests.cs</c>).
///         It has never been in <c>EngineAsyncTests</c>; a citation that names
///         the wrong file is how a rule stops being checked.
    ///     </para>
    /// </summary>
    private static readonly EngineWorker s_offlineWorker = new("JuceAudioEngine offline");

    /// <summary>Test hook: the per-engine worker, or null if no async member was
    /// ever called on this engine.</summary>
    internal EngineWorker? WorkerForTests { get { lock (_workerGate) return _worker; } }

    /// <summary>Test hook: the process-wide worker behind the static offline
    /// async members.</summary>
    internal static EngineWorker OfflineWorkerForTests => s_offlineWorker;

    /// <summary>Test hook: has <see cref="ShutdownWorker" /> run? A dispose racing
    /// a queued call is otherwise untestable without a sleep.</summary>
    internal bool WorkerShutdownRequestedForTests { get { lock (_workerGate) return _workerClosed; } }

    // ------------------------------------------------------------- device

    /// <inheritdoc cref="Reconfigure" />
    /// <remarks>Runs on this engine's worker thread; see the type's async
    /// remarks. Reopening a driver is a multi-hundred-millisecond call on a good
    /// day and seconds on a bad one, which is why it is here.</remarks>
    public Task ReconfigureAsync(int sampleRate, int bufferFrames,
                                 string? outputDeviceId, string? inputDeviceId,
                                 CancellationToken cancellationToken = default)
        => RunAsync(() => Reconfigure(sampleRate, bufferFrames, outputDeviceId, inputDeviceId),
                    cancellationToken);

    /// <inheritdoc cref="SetDeviceType" />
    /// <remarks>Runs on this engine's worker thread; see the type's async
    /// remarks. An in-place host swap closes and reopens the device.</remarks>
    public Task SetDeviceTypeAsync(string? typeName,
                                   string? outputDeviceId = null,
                                   string? inputDeviceId = null,
                                   uint outputChannelMask = 0,
                                   uint inputChannelMask = 0,
                                   CancellationToken cancellationToken = default)
        => RunAsync(() => SetDeviceType(typeName, outputDeviceId, inputDeviceId,
                                        outputChannelMask, inputChannelMask),
                    cancellationToken);

    /// <summary>
    ///     <see cref="TryShowAsioControlPanel" /> off the calling thread. THE
    ///     member this whole mechanism exists for: the panel is a MODAL vendor
    ///     dialog, so the synchronous call does not return until the user closes
    ///     it — on a UI thread that is a frozen application, and it is also the
    ///     call that blew through the handle drain's old 2 s ceiling.
    ///     <para>
    ///         <b>DISPOSING THE ENGINE WHILE THE PANEL IS OPEN STILL BLOCKS.</b>
    ///         Nothing can cancel a modal vendor dialog, so a
    ///         <see cref="Dispose()" /> that lands while the user is inside it
    ///         waits <c>EngineWorker.DisposeJoinTimeoutMs</c> on the worker and
    ///         then up to <c>NativeHandleOwner.DefaultDrainTimeoutMs</c> for the
    ///         handle, after which the native engine is LEAKED (logged at Error)
    ///         rather than freed under the live call. Close the panel before
    ///         tearing the engine down.
    ///     </para>
    /// </summary>
    /// <returns>Whether the panel was shown, and the engine's reason if not.</returns>
    public Task<JuceAsioPanelResult> ShowAsioControlPanelAsync(nint parentWindow,
                                                              CancellationToken cancellationToken = default)
        => RunAsync(() =>
        {
            var shown = TryShowAsioControlPanel(parentWindow, out var error);
            return new JuceAsioPanelResult(shown, error);
        }, cancellationToken);

    // ------------------------------------------------------------- content

    /// <inheritdoc cref="OpenFile" />
    /// <remarks>Runs on this engine's worker thread; see the type's async
    /// remarks. A load decodes the whole file and, off a 44.1 kHz source on a
    /// 48 kHz device, resamples it — per stem, which is where a 6-stem project's
    /// several seconds go.</remarks>
    public Task<JuceAudioSource> OpenFileAsync(string path, CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        return RunAsync(() => NotACorpse(OpenFile(path), static s => s.IsDisposed), cancellationToken);
    }

    /// <inheritdoc cref="LoadPlugin(string)" />
    /// <remarks>Runs on this engine's worker thread; see the type's async
    /// remarks. A COLD VST3 scan of a large amp sim measures 25 s+.</remarks>
    public Task<JuceAudioPlugin> LoadPluginAsync(string path, CancellationToken cancellationToken = default)
        => LoadPluginAsync(path, 0, cancellationToken);

    /// <inheritdoc cref="LoadPlugin(string, int)" />
    /// <remarks>Runs on this engine's worker thread; see the type's async
    /// remarks.</remarks>
    public Task<JuceAudioPlugin> LoadPluginAsync(string path, int componentIndex,
                                                 CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentOutOfRangeException.ThrowIfNegative(componentIndex);
        return RunAsync(() => NotACorpse(LoadPlugin(path, componentIndex), static p => p.IsDisposed),
                        cancellationToken);
    }

    /// <summary>
    ///     Post-check for the async factories: FAULT the task rather than hand a
    ///     caller a child wrapper the engine's own teardown has already destroyed.
    ///     <para>
    ///         The ordering that produces one: the work item runs,
    ///         <c>OpenFile</c> / <c>LoadPlugin</c> succeeds and registers the
    ///         child under <c>_childrenGate</c> (so <c>Track</c> itself does not
    ///         throw), <see cref="ShutdownWorker" /> then joins and returns, and
    ///         <c>DisposeChildren</c> disposes that very child. The awaiting
    ///         caller used to receive a live-LOOKING
    ///         <see cref="JuceAudioSource" /> whose every member throws. The
    ///         synchronous path has the same window; the async one widens it to
    ///         the length of a 25 s cold VST3 scan.
    ///     </para>
    ///     <para>
    ///         Two questions, because they close different halves: has a dispose
    ///         STARTED (<c>_workerClosed</c> is set before the join, so an item
    ///         still in flight always observes it), and has this child ALREADY
    ///         been destroyed. Either answer means the caller gets an
    ///         <see cref="ObjectDisposedException" /> naming the engine — which
    ///         is what happened, rather than the child's own, which would read as
    ///         "your source was disposed" by someone unnamed.
    ///     </para>
    /// </summary>
    private T NotACorpse<T>(T child, Func<T, bool> isDisposed) where T : IDisposable
    {
        bool closed;
        lock (_workerGate) closed = _workerClosed;
        if (!closed && !isDisposed(child)) return child;

        // Never leak what we just opened: if the teardown has not reached this
        // child yet, this destroys it; if it has, Dispose is idempotent.
        try { child.Dispose(); } catch { /* best effort — we are already failing */ }
        throw new ObjectDisposedException(nameof(JuceAudioEngine),
            "The engine was disposed while this call was inside the native engine. What it opened has "
            + "been destroyed rather than handed back — every member on it would have thrown.");
    }

    // ------------------------------------------------------------- offline (static)

    /// <inheritdoc cref="GenerateWaveform" />
    /// <remarks>STATIC, so it runs on the process-wide offline worker rather than
    /// any engine's — it needs no engine. SERIALISED with every other static
    /// offline call for the life of the process, and the worker is never shut
    /// down, so one wedged decode wedges this member permanently. See
    /// <c>s_offlineWorker</c>.</remarks>
    public static Task<float[]> GenerateWaveformAsync(string path, double blockMs,
                                                      CancellationToken cancellationToken = default)
        => s_offlineWorker.Enqueue(() => GenerateWaveform(path, blockMs), cancellationToken);

    /// <inheritdoc cref="GenerateWaveformFromMemory" />
    /// <remarks>STATIC; runs on the process-wide offline worker, SERIALISED with
    /// every other static offline call and never recovered from a wedge. The
    /// array is read for the duration of the call — do not mutate it until the
    /// task completes.</remarks>
    public static Task<float[]> GenerateWaveformFromMemoryAsync(byte[] data, double blockMs,
                                                                CancellationToken cancellationToken = default)
        => s_offlineWorker.Enqueue(() => GenerateWaveformFromMemory(data, blockMs), cancellationToken);

    /// <inheritdoc cref="RenderToWav" />
    /// <remarks>STATIC; runs on the process-wide offline worker, SERIALISED with
    /// every other static offline call. A decode + WAV write of a full song is
    /// seconds — and for those seconds every GenerateWaveformAsync in the process
    /// is queued behind it.</remarks>
    public static Task RenderToWavAsync(string srcPath, string dstPath,
                                        float tempo = 1.0f, float semitones = 0.0f, int sampleRate = 0,
                                        CancellationToken cancellationToken = default)
        => s_offlineWorker.Enqueue<object?>(() =>
        {
            RenderToWav(srcPath, dstPath, tempo, semitones, sampleRate);
            return null;
        }, cancellationToken);

    // ------------------------------------------------------------- plumbing

    private Task<T> RunAsync<T>(Func<T> work, CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested) return Task.FromCanceled<T>(cancellationToken);
        EngineWorker worker;
        lock (_workerGate)
        {
            // A worker created after Dispose would be a thread nobody ever joins.
            if (_workerClosed)
                return Task.FromException<T>(new ObjectDisposedException(nameof(JuceAudioEngine)));
            worker = _worker ??= new EngineWorker(nameof(JuceAudioEngine));
        }
        return worker.Enqueue(work, cancellationToken);
    }

    private Task RunAsync(Action work, CancellationToken cancellationToken)
        => RunAsync<object?>(() => { work(); return null; }, cancellationToken);

    /// <summary>
    ///     Stop the worker and wait for whatever it is inside, BEFORE the engine
    ///     handle is claimed and drained. Called from <see cref="Dispose(bool)" />
    ///     and from nowhere else.
    /// </summary>
    /// <param name="disposing">False on the finalizer path, where nothing may
    /// wait: there is one finalizer thread in the process.</param>
    private void ShutdownWorker(bool disposing)
    {
        // A PARTIALLY-CONSTRUCTED instance reaches here: the CLR queues the
        // finalizer the moment the allocation succeeds, and a constructor that
        // threw before the field initializers ran leaves _workerGate null. A
        // throwing finalizer is a process-wide FailFast, so this is read once
        // into a local and null-checked rather than locked blind — the same rule
        // `_owner?.` follows below.
        var gate = _workerGate;
        if (gate is null) return;

        EngineWorker? worker;
        lock (gate)
        {
            _workerClosed = true;
            worker = _worker;
        }
        // EngineWorker.DisposeJoinTimeoutMs, NOT the 60 s handle-drain deadline
        // this used to pass: Dispose runs on the host's UI thread and the item
        // in flight can be a modal ASIO panel with no upper bound. See that
        // constant for the whole argument, and ShowAsioControlPanelAsync for
        // what a caller still has to know.
        worker?.Shutdown(disposing ? EngineWorker.DisposeJoinTimeoutMs : 0);
    }
}

/// <summary>
/// What <see cref="JuceAudioEngine.ShowAsioControlPanelAsync" /> answers: the
/// out-parameter pair of <see cref="JuceAudioEngine.TryShowAsioControlPanel" />,
/// which a <c>Task</c> cannot carry.
/// </summary>
/// <param name="Shown">Whether the driver's panel was opened.</param>
/// <param name="Error">Why not, when it was not. Null on success.</param>
public readonly record struct JuceAsioPanelResult(bool Shown, string? Error);
