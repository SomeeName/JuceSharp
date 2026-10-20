using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

public sealed class NativeDestructionTests
{
    [Fact]
    public async Task ClaimedChildDoesNotPermitParentDestroyUntilNativeDestroyReturns()
    {
        var completion = new NativeDestruction();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var destroys = 0;
        var child = Task.Run(() => completion.Run(() =>
        {
            Interlocked.Increment(ref destroys);
            entered.Set();
            Assert.True(release.Wait(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken));
            return true;
        }), TestContext.Current.CancellationToken);
        try
        {
            Assert.True(entered.Wait(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken));
            completion.Run(() => throw new InvalidOperationException("duplicate destroy"));
            Assert.False(completion.Wait(0));
            Assert.False(completion.IsComplete);
        }
        finally { release.Set(); }
        await child;
        Assert.True(completion.Wait(0));
        Assert.Equal(1, destroys);
    }

    [Fact]
    public void TimedOutChildPinPermanentlyPreventsParentDestroy()
    {
        var owner = new NativeHandleOwner(123);
        var completion = new NativeDestruction();
        owner.Acquire("child");
        completion.Run(() => owner.TakeForDispose("child", 0, out _) != 0);
        Assert.False(completion.Wait(0));
        owner.Release();
        completion.Run(() => true);
        Assert.False(completion.Wait(0));
    }

    [Fact]
    public void NativeDestroyFailurePermanentlyPreventsParentDestroy()
    {
        var completion = new NativeDestruction();
        Assert.Throws<InvalidOperationException>(() => completion.Run(() => throw new InvalidOperationException()));
        Assert.False(completion.Wait(0));
        completion.Run(() => true);
        Assert.False(completion.IsComplete);
    }
}

/// <summary>
///     W4-M4. <c>JuceAudioEngine.DisposeChildren</c> waited
///     <see cref="NativeHandleOwner.DefaultDrainTimeoutMs" /> — 60 s — on EVERY
///     tracked child's destruction token, including the ones whose wrapper had
///     been COLLECTED and whose finalizer had not run yet. Nothing on that path
///     calls <c>Destruction.Run</c>, so the token stays unclaimed and the
///     disposing thread (normally the host's UI thread) parks for the full
///     minute, per such child, in series. <c>Track</c>'s compaction cannot save
///     it either: it only drops entries that are already complete, so exactly
///     these children are guaranteed to still be in the list.
///     <para>
///         Wave-2 #22 closed the worker-join half of "Dispose freezes the UI"
///         and reduced that join to 2 s; this second 60 s wait on the same
///         method was never looked at.
///     </para>
/// </summary>
[Collection("NullDevice")]
public sealed class EngineChildDestructionDeadlineTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    /// <summary>The premise, deterministically: a token nobody ever runs never
    /// completes, so the WAIT is the only thing bounding it. No clock in the
    /// assertion — the deadline is the input.</summary>
    [Fact]
    public void ATokenNobodyRuns_NeverCompletes_SoOnlyTheDeadlineEndsTheWait()
    {
        var never = new NativeDestruction();
        Assert.False(never.IsComplete);
        Assert.False(never.Wait(50));
        Assert.False(never.IsComplete);
    }

    /// <summary>
    /// The choice itself, as a value rather than a duration: a reachable
    /// wrapper is one whose Dispose we just called, so the 60 s backstop is
    /// right (a modal ASIO panel, a cold VST3 scan); a collected one has nobody
    /// running its destroy on our behalf, so it gets the short grace and then
    /// the existing "incomplete ⇒ LEAK the engine" branch.
    /// </summary>
    [Fact]
    public void ACollectedChildGetsTheShortGrace_ALiveOneKeepsTheFullBackstop()
    {
        Assert.Equal(NativeHandleOwner.DefaultDrainTimeoutMs,
            JuceAudioEngine.ChildDestructionDeadlineMs(wrapperReachable: true));
        Assert.Equal(EngineWorker.DisposeJoinTimeoutMs,
            JuceAudioEngine.ChildDestructionDeadlineMs(wrapperReachable: false));
        Assert.True(EngineWorker.DisposeJoinTimeoutMs < NativeHandleOwner.DefaultDrainTimeoutMs);
    }

    /// <summary>
    ///     End to end, and a LIVENESS assertion rather than a performance one:
    ///     the only thing that changes between the bug and the fix is how long
    ///     the disposing thread is parked, so the test disposes on its own thread
    ///     and joins with a margin an order of magnitude under the 60 s bug and
    ///     an order of magnitude over the 2 s fix.
    ///     <para>
    ///         The "collected but not finalized" child is produced
    ///         deterministically — a <see cref="WeakReference{T}" /> whose target
    ///         is cleared by hand — rather than by hoping for a GC.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void DisposingAnEngineWhoseChildWrapperWasCollected_DoesNotParkTheDisposingThreadForAMinute()
    {
        var engine = NullDevice.CreateEngine();

        var childrenField = typeof(JuceAudioEngine).GetField(
            "_children",
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!;
        var gate = typeof(JuceAudioEngine).GetField(
            "_childrenGate",
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.GetValue(engine)!;

        var collected = new WeakReference<IDisposable>(new DummyDisposable(), trackResurrection: true);
        collected.SetTarget(null!);
        Assert.False(collected.TryGetTarget(out _));

        var children =
            (List<(WeakReference<IDisposable> Child, NativeDestruction Destruction)>)childrenField.GetValue(engine)!;
        lock (gate) children.Add((collected, new NativeDestruction()));

        var disposer = new Thread(engine.Dispose) { IsBackground = true, Name = "engine-dispose" };
        disposer.Start();

        Assert.True(disposer.Join(TimeSpan.FromSeconds(20)),
            "JuceAudioEngine.Dispose() is still parked on a collected child's destruction token. "
            + "That wait was 60 s per such child, on the host's UI thread, in series.");
    }

    private sealed class DummyDisposable : IDisposable
    {
        public void Dispose() { }
    }
}
