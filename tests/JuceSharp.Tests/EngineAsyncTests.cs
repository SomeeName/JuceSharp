using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     The 0.9 async members (audit F10). Every one of <c>Reconfigure</c>,
///     <c>SetDeviceType</c>, <c>LoadPlugin</c>, <c>TryShowAsioControlPanel</c>,
///     <c>OpenFile</c>, <c>GenerateWaveform*</c> and <c>RenderToWav</c> holds the
///     engine handle on the CALLER's thread for as long as the native call takes
///     — 25 s+ for a cold VST3 scan and unbounded for the modal ASIO panel — so
///     from a UI thread the app is simply frozen.
///     <para>
///         Everything here runs on the NULL device, and nothing asserts a
///         duration: the claims are about ORDER, about a counter, and about a
///         thread being gone.
///     </para>
/// </summary>
[Collection("NullDevice")]
public sealed class EngineAsyncTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();

    public void Dispose() => _scope.Dispose();

    /// <summary>An async open really does open: the source is playable and is
    /// tracked by the engine exactly like a synchronous one.</summary>
    [NativeAvailableFact]
    public async Task OpenFileAsync_ReturnsAPlayableSource()
    {
        using var engine = NullDevice.CreateEngine();
        var wav = TinyWav.Create(seconds: 0.5);
        try
        {
            using var source = await engine.OpenFileAsync(wav);

            Assert.True(source.Length > TimeSpan.Zero, "the source reports no length");
            source.Play();
            source.Pause();
            source.Seek(TimeSpan.FromMilliseconds(100));
            Assert.True(source.Position >= TimeSpan.Zero);
        }
        finally { TinyWav.TryDelete(wav); }
    }

    /// <summary>
    /// Two device changes must never overlap. Asserted as a COUNTER, not a
    /// duration: the worker dispatches one item at a time, so the highest
    /// concurrency it ever observes is 1 — and both calls were really dispatched,
    /// so that 1 is not "nothing ran".
    /// </summary>
    [NativeAvailableFact]
    public async Task ReconfigureAsync_TwiceConcurrently_SerialisesOnTheOneWorker()
    {
        using var engine = NullDevice.CreateEngine(sampleRate: 48000, bufferFrames: 480);

        var a = engine.ReconfigureAsync(44100, 512, null, null);
        var b = engine.ReconfigureAsync(48000, 256, null, null);
        await Task.WhenAll(a, b);

        var worker = engine.WorkerForTests;
        Assert.NotNull(worker);
        Assert.Equal(2, worker!.StartedCount);
        Assert.Equal(1, worker.MaxObservedConcurrency);
        // The LAST request is the one that stuck, which is what serialised means
        // here: they did not interleave into some third configuration.
        Assert.Equal(48000, engine.SampleRate);
        Assert.Equal(256, engine.BufferFrames);
    }

    /// <summary>
    /// Ordering, proven without a clock: the first item is held inside the worker
    /// until the test releases it, and the second records what it saw. If the two
    /// could overlap, <c>secondSawFirstFinished</c> would be false.
    /// </summary>
    [Fact]
    public async Task TheWorkerRunsItemsInOrder_AndNeverTwoAtOnce()
    {
        var worker = new EngineWorker("ordering test");
        using var release = new ManualResetEventSlim(false);
        var firstFinished = false;
        var secondSawFirstFinished = false;
        var firstThread = -1;
        var secondThread = -2;

        var first = worker.Enqueue(() =>
        {
            firstThread = Environment.CurrentManagedThreadId;
            release.Wait();
            Volatile.Write(ref firstFinished, true);
            return 1;
        }, CancellationToken.None);

        var second = worker.Enqueue(() =>
        {
            secondThread = Environment.CurrentManagedThreadId;
            secondSawFirstFinished = Volatile.Read(ref firstFinished);
            return 2;
        }, CancellationToken.None);

        Assert.False(second.IsCompleted, "the second item ran while the first was still inside the worker");
        release.Set();

        Assert.Equal(1, await first);
        Assert.Equal(2, await second);
        Assert.True(secondSawFirstFinished, "the two items overlapped");
        Assert.Equal(firstThread, secondThread);
        Assert.NotEqual(Environment.CurrentManagedThreadId, firstThread);
        Assert.Equal(1, worker.MaxObservedConcurrency);

        worker.Shutdown(5000);
    }

    /// <summary>
    /// A token cancelled BEFORE the call throws and never reaches native.
    /// Asserted with the worker's dispatch counter rather than by inspecting the
    /// engine: an item that was never dispatched cannot have touched the ABI.
    /// </summary>
    [NativeAvailableFact]
    public async Task ACancelledTokenNeverReachesTheEngine()
    {
        using var engine = NullDevice.CreateEngine();
        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => engine.ReconfigureAsync(44100, 512, null, null, cts.Token));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => engine.OpenFileAsync("nowhere.wav", cts.Token));

        // No worker was even created: an already-cancelled token is refused
        // before the queue.
        Assert.Null(engine.WorkerForTests);
        Assert.Equal(48000, engine.SampleRate);   // the device is untouched
    }

    /// <summary>
    /// A token cancelled while the item is still QUEUED is honoured at dispatch —
    /// the item is skipped, and the counter proves nothing ran for it.
    /// </summary>
    [Fact]
    public async Task ATokenCancelledWhileQueued_IsHonouredAtDispatch()
    {
        var worker = new EngineWorker("queued cancel test");
        using var release = new ManualResetEventSlim(false);
        using var cts = new CancellationTokenSource();

        var blocker = worker.Enqueue(() => { release.Wait(); return 0; }, CancellationToken.None);
        var queued = worker.Enqueue(() => 1, cts.Token);

        await cts.CancelAsync();      // still behind the blocker
        release.Set();

        await blocker;
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => queued);
        Assert.Equal(1, worker.StartedCount);   // only the blocker ever ran

        worker.Shutdown(5000);
    }

    /// <summary>
    ///     Disposing the engine with work outstanding must COMPLETE those tasks,
    ///     not leave a host awaiting forever — and must not deadlock against the
    ///     handle drain either. The in-flight item finishes; the one still queued
    ///     is faulted with <see cref="ObjectDisposedException" />.
    /// </summary>
    [NativeAvailableFact]
    public async Task DisposingWithAQueuedCall_CompletesOrFaultsIt_AndNeverDeadlocks()
    {
        var engine = NullDevice.CreateEngine();
        using var inside = new ManualResetEventSlim(false);
        using var release = new ManualResetEventSlim(false);

        // Occupy the worker with something the test controls, then queue a real
        // engine call behind it.
        var first = engine.ReconfigureAsync(44100, 512, null, null);
        await first;
        var worker = engine.WorkerForTests!;

        var blocker = worker.Enqueue(() => { inside.Set(); release.Wait(); return 0; }, CancellationToken.None);
        Assert.True(inside.Wait(TimeSpan.FromSeconds(30)), "the worker never picked the blocker up");
        var queued = engine.ReconfigureAsync(48000, 256, null, null);

        var dispose = Task.Run(engine.Dispose);
        // Wait for Dispose to have CLOSED THE QUEUE before releasing the blocker.
        // The engine's own "shutdown requested" flag is set a moment earlier, and
        // waiting on that one would be racing the test's setup rather than the
        // code: the worker may legitimately still dequeue in that window.
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(30);
        while (!worker.QueueClosed && DateTime.UtcNow < deadline) Thread.Yield();
        Assert.True(worker.QueueClosed, "Dispose never reached the worker shutdown");
        Assert.True(engine.WorkerShutdownRequestedForTests);
        Assert.False(dispose.IsCompleted, "Dispose returned while a native call was still in flight");

        release.Set();
        Assert.True(dispose.Wait(TimeSpan.FromSeconds(60)), "Dispose deadlocked behind its own worker");

        Assert.Equal(0, await blocker);
        // The queued call never reached native — the engine was going away.
        await Assert.ThrowsAsync<ObjectDisposedException>(() => queued);
    }

    /// <summary>The worker thread does not outlive the engine.</summary>
    [NativeAvailableFact]
    public async Task TheWorkerThreadIsGoneAfterDispose()
    {
        var engine = NullDevice.CreateEngine();
        await engine.ReconfigureAsync(44100, 512, null, null);

        var thread = engine.WorkerForTests?.Thread;
        Assert.NotNull(thread);
        Assert.True(thread!.IsAlive);

        engine.Dispose();

        Assert.False(thread.IsAlive, "the engine worker thread outlived the engine");
        // …and a call made afterwards is a clean ObjectDisposedException rather
        // than a resurrected thread.
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => engine.ReconfigureAsync(48000, 256, null, null));
        Assert.False(engine.WorkerForTests?.Thread?.IsAlive ?? false,
            "a post-dispose async call resurrected the worker thread");
    }

    /// <summary>The static offline members run off the caller's thread too, on
    /// the process-wide offline worker.</summary>
    [NativeAvailableFact]
    public async Task GenerateWaveformAsync_RunsOnTheOfflineWorker()
    {
        var wav = TinyWav.Create(seconds: 0.5);
        try
        {
            var before = JuceAudioEngine.OfflineWorkerForTests.StartedCount;
            var peaks = await JuceAudioEngine.GenerateWaveformAsync(wav, 20.0);

            Assert.NotEmpty(peaks);
            Assert.True(JuceAudioEngine.OfflineWorkerForTests.StartedCount > before,
                "the call did not go through the offline worker");
            Assert.NotEqual(Environment.CurrentManagedThreadId,
                            JuceAudioEngine.OfflineWorkerForTests.Thread!.ManagedThreadId);
        }
        finally { TinyWav.TryDelete(wav); }
    }
}
