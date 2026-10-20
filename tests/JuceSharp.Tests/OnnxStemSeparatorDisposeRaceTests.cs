using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The dispose-vs-inference ordering of <see cref="OnnxStemSeparator"/>.
/// <para>
/// THE LATENT DEFECT: <c>SeparateAsync</c> checked a disposed flag on the
/// calling thread and queued the inference onto the pool; <c>Dispose()</c>
/// freed the native <c>InferenceSession</c> with nothing waiting for a pool
/// thread already inside <c>session.Run</c>. No model is needed to prove the
/// ordering — the internal constructor injects the inference runner and the
/// native release, so "the session was freed while an inference was inside
/// it" is observable as plain ordering between two events.
/// </para>
/// <para>
/// Nothing here asserts a duration: every wait is on an event with a generous
/// upper bound, and the assertions are about ORDER (release-after-exit) and
/// OUTCOME (throw vs. run), not time.
/// </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class OnnxStemSeparatorDisposeRaceTests
{
    private static readonly TimeSpan Bound = TimeSpan.FromSeconds(30);

    private static readonly float[] Stereo = new float[4];

    private static IReadOnlyList<OnnxStemBuffer> Stems(int sampleRate) =>
        new[] { new OnnxStemBuffer("vocals", new float[4], sampleRate) };

    [Fact]
    public async Task Dispose_WaitsForAnInFlightInference_BeforeReleasingTheSession()
    {
        using var inferenceStarted = new ManualResetEventSlim();
        using var letInferenceFinish = new ManualResetEventSlim();
        var events = new List<string>();
        var eventsGate = new object();
        void Log(string e) { lock (eventsGate) events.Add(e); }

        var separator = new OnnxStemSeparator(
            runner: (_, rate, _, _) =>
            {
                Log("inference-entered");
                inferenceStarted.Set();
                Assert.True(letInferenceFinish.Wait(Bound, TestContext.Current.CancellationToken), "test never released the inference");
                Log("inference-exited");
                return Stems(rate);
            },
            releaseNative: () => Log("session-released"));

        var inference = separator.SeparateAsync(Stereo, 44100, ct: TestContext.Current.CancellationToken);
        Assert.True(inferenceStarted.Wait(Bound, TestContext.Current.CancellationToken), "inference never started");

        // Dispose from another thread while the inference is inside the session.
        // A DEDICATED THREAD, NOT Task.Run. This test already parks two: its own
        // xunit slot, and the pool item SeparateAsync queued and that is now
        // sitting in the injected runner waiting to be released. The .NET pool
        // injects roughly ONE thread per half second above the core count, so a
        // third pooled item can genuinely fail to get a thread inside the 30 s
        // bound — and the failure it produces ("inference never started") names
        // the symptom and hides the cause. A thread we create ourselves waits
        // behind nothing.
        var dispose = RunOnItsOwnThread(separator.Dispose, "onnx-dispose-race");

        // The dispose is REQUESTED (no new inference may start)...
        Assert.True(SpinWait.SpinUntil(() => separator.IsDisposeRequested, Bound));
        // ...but has not returned, and the session is still alive under the
        // running inference.
        Assert.False(dispose.IsCompleted, "Dispose returned while an inference was still inside the session");
        lock (eventsGate) Assert.DoesNotContain("session-released", events);

        letInferenceFinish.Set();
        var stems = await inference.WaitAsync(Bound, TestContext.Current.CancellationToken);
        await dispose.WaitAsync(Bound, TestContext.Current.CancellationToken);

        Assert.Single(stems);
        lock (eventsGate)
            Assert.Equal(new[] { "inference-entered", "inference-exited", "session-released" }, events);
    }

    [Fact]
    public async Task AnInferenceQueuedBehindADispose_ObservesObjectDisposed_AndNeverTouchesTheSession()
    {
        var ran = 0;
        var separator = new OnnxStemSeparator(
            runner: (_, rate, _, _) =>
            {
                Interlocked.Increment(ref ran);
                return Stems(rate);
            },
            releaseNative: () => { });

        separator.Dispose();

        // The caller-thread check catches the plain case...
        await Assert.ThrowsAsync<ObjectDisposedException>(() => separator.SeparateAsync(Stereo, 44100, ct: TestContext.Current.CancellationToken));
        Assert.Equal(0, ran);
    }

    [Fact]
    public async Task ADisposeThatLandsAfterTheCallerCheck_IsStillObservedOnThePoolThread()
    {
        // The window the old code left open: the caller-thread check passed,
        // then Dispose ran before the pool thread reached the session. The pin
        // on the pool thread must see the dispose and refuse.
        var guard = new InferenceGuard();
        Assert.Equal(InferenceGuard.DrainOutcome.Drained, guard.TakeForDispose());
        Assert.Throws<ObjectDisposedException>(() => guard.Enter("x"));

        // And at the separator level, with the runner itself as the witness.
        var ran = 0;
        using var runnerReached = new ManualResetEventSlim();
        var separator = new OnnxStemSeparator(
            runner: (_, rate, _, _) =>
            {
                Interlocked.Increment(ref ran);
                runnerReached.Set();
                return Stems(rate);
            },
            releaseNative: () => { });

        // A completed inference followed by a dispose then a second call: the
        // second call must throw on the pool thread's pin even though it is
        // queued, and the runner must not run again.
        await separator.SeparateAsync(Stereo, 44100, ct: TestContext.Current.CancellationToken).WaitAsync(Bound, TestContext.Current.CancellationToken);
        Assert.True(runnerReached.Wait(Bound, TestContext.Current.CancellationToken));
        separator.Dispose();
        await Assert.ThrowsAsync<ObjectDisposedException>(() => separator.SeparateAsync(Stereo, 44100, ct: TestContext.Current.CancellationToken));
        Assert.Equal(1, ran);
    }

    [Fact]
    public async Task ADrainThatTimesOut_LeaksTheSessionInsteadOfFreeingItUnderTheInference()
    {
        using var inferenceStarted = new ManualResetEventSlim();
        using var letInferenceFinish = new ManualResetEventSlim();
        var released = 0;
        var logged = new List<string>();

        var separator = new OnnxStemSeparator(
            runner: (_, rate, _, _) =>
            {
                inferenceStarted.Set();
                letInferenceFinish.Wait(Bound, TestContext.Current.CancellationToken);
                return Stems(rate);
            },
            releaseNative: () => Interlocked.Increment(ref released),
            drainTimeoutMs: 1,
            log: logged.Add);

        var inference = separator.SeparateAsync(Stereo, 44100, ct: TestContext.Current.CancellationToken);
        Assert.True(inferenceStarted.Wait(Bound, TestContext.Current.CancellationToken));

        separator.Dispose(); // 1 ms budget, inference is wedged → gives up
        Assert.Equal(0, released);
        Assert.Contains(logged, l => l.Contains("LEAKING", StringComparison.Ordinal));

        letInferenceFinish.Set();
        await inference.WaitAsync(Bound, TestContext.Current.CancellationToken);
        // A second dispose is a no-op: the claim was taken by the first one.
        separator.Dispose();
        Assert.Equal(0, released);
    }

    [Fact]
    public void Guard_SecondTakeForDispose_IsAlreadyClaimed()
    {
        var guard = new InferenceGuard();
        Assert.Equal(InferenceGuard.DrainOutcome.Drained, guard.TakeForDispose());
        Assert.Equal(InferenceGuard.DrainOutcome.AlreadyClaimed, guard.TakeForDispose());
    }

    /// <summary>
    /// Runs <paramref name="work"/> on a thread of its own and hands back a
    /// Task for it. Deliberately NOT <see cref="Task.Run(Action)"/>: every wait
    /// in this class is bounded, and a bound that can expire waiting for the
    /// thread pool to GROW is not measuring what it claims to.
    /// </summary>
    private static Task RunOnItsOwnThread(Action work, string name)
    {
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try { work(); done.TrySetResult(); }
            catch (Exception ex) { done.TrySetException(ex); }
        })
        { IsBackground = true, Name = name };
        thread.Start();
        return done.Task;
    }

}
