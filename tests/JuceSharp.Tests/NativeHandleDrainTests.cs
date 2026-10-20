using System;
using System.Collections.Concurrent;
using System.Threading;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The drain-on-dispose contract, which the repo's guides state as a SAFETY
/// claim ("a position poll racing a dispose surfaces as ObjectDisposedException,
/// never a native crash") and which was false on two counts until 0.61:
/// <list type="number">
///   <item><see cref="NativeHandleOwner.TakeForDispose(string)" /> spun for 2 s
///     and then <b>returned the handle anyway</b>, so every caller destroyed it
///     under a live native call;</item>
///   <item><see cref="JuceAudioEngine" /> drained only its OWN counter, and each
///     child wrapper owns a SEPARATE one — so a source call in flight was
///     invisible to the engine's dispose.</item>
/// </list>
/// The pure half lives here; the engine half is
/// <see cref="EngineChildDrainTests" /> below, which needs a real engine.
/// </summary>
public sealed class NativeHandleDrainTests
{
    /// <summary>
    ///     A ZERO deadline is what the finalizer path passes, and the two outcomes
    ///     have to stay distinct: with nothing in flight it drains instantly and
    ///     hands the handle back; with a call in flight it reports
    ///     <see cref="HandleDrain.TimedOut" /> and does NOT hand it back, so the
    ///     native object is leaked rather than freed under a live call.
    ///     <para>
    ///         Why 0 there at all: one finalizer thread serves the process, so the
    ///         60 s default does not stall this wrapper — it stalls every finalizer
    ///         queued behind it, including the child wrappers the engine's own
    ///         finalizer is about to check.
    ///     </para>
    /// </summary>
    [Fact]
    public void AZeroDeadline_DrainsWhenIdle_AndLeaksRatherThanFreeingUnderALiveCall()
    {
        var idle = new NativeHandleOwner(0x1234);
        Assert.Equal(0x1234, idle.TakeForDispose("idle", 0, out var idleOutcome));
        Assert.Equal(HandleDrain.Drained, idleOutcome);

        var busy = new NativeHandleOwner(0x5678);
        Assert.NotEqual(nint.Zero, busy.TryAcquire()); // a call is "in flight"
        try
        {
            Assert.Equal(nint.Zero, busy.TakeForDispose("busy", 0, out var busyOutcome));
            Assert.Equal(HandleDrain.TimedOut, busyOutcome);
        }
        finally { busy.Release(); }
    }

    /// <summary>
    /// ORDERING, not wall clock: the handle is not handed back for destruction
    /// until the last <c>Release</c> has run. Two ManualResetEvents drive the
    /// interleaving, and the assertion is the recorded sequence.
    /// </summary>
    [Fact]
    public void TakeForDispose_HandsBackTheHandleOnlyAfterTheLastReleaseHasRun()
    {
        var owner = new NativeHandleOwner(0x1234);
        var order = new ConcurrentQueue<string>();
        var acquired = new ManualResetEventSlim(false);
        var letGo = new ManualResetEventSlim(false);
        var disposerParked = new ManualResetEventSlim(false);

        var caller = new Thread(() =>
        {
            _ = owner.Acquire("caller");
            order.Enqueue("acquired");
            acquired.Set();
            // Hold the pin until the disposer is demonstrably inside the drain.
            disposerParked.Wait(TimeSpan.FromSeconds(10));
            letGo.Wait(TimeSpan.FromSeconds(10));
            order.Enqueue("released");
            owner.Release();
        }) { IsBackground = true, Name = "drain-caller" };

        nint taken = -1;
        var disposer = new Thread(() =>
        {
            disposerParked.Set();
            taken = owner.TakeForDispose("owner");
            order.Enqueue("taken");
        }) { IsBackground = true, Name = "drain-disposer" };

        caller.Start();
        Assert.True(acquired.Wait(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken));
        disposer.Start();
        Assert.True(disposerParked.Wait(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken));

        letGo.Set();
        Assert.True(caller.Join(TimeSpan.FromSeconds(10)));
        Assert.True(disposer.Join(TimeSpan.FromSeconds(10)));

        Assert.Equal(new[] { "acquired", "released", "taken" }, order.ToArray());
        Assert.Equal((nint)0x1234, taken);
    }

    /// <summary>
    /// THE fix for the 2 s ceiling. A call that outlives the drain deadline —
    /// a modal ASIO control panel, a cold VST3 scan, a wedged device — must
    /// leave the handle UNFREED. Returning it (what shipped) means the caller
    /// destroys the native object while native is still inside the call.
    /// A leak beats a use-after-free.
    /// <para>The timeout is injected, so this is deterministic rather than a
    /// wall-clock race: the drain cannot complete because the pin is held for
    /// the whole test.</para>
    /// </summary>
    [Fact]
    public void TakeForDispose_OnExpiry_ReportsTimedOut_AndRefusesToHandBackTheHandle()
    {
        var owner = new NativeHandleOwner(0x4242);
        var acquired = new ManualResetEventSlim(false);
        var letGo = new ManualResetEventSlim(false);

        var caller = new Thread(() =>
        {
            _ = owner.Acquire("caller");
            acquired.Set();
            letGo.Wait(TimeSpan.FromSeconds(10));
            owner.Release();
        }) { IsBackground = true, Name = "wedged-caller" };

        caller.Start();
        Assert.True(acquired.Wait(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken));

        var h = owner.TakeForDispose("owner", timeoutMs: 50, out var outcome);

        Assert.Equal(HandleDrain.TimedOut, outcome);
        Assert.Equal(nint.Zero, h);

        letGo.Set();
        Assert.True(caller.Join(TimeSpan.FromSeconds(10)));
    }

    /// <summary>A drained take reports Drained and a second one reports
    /// AlreadyClaimed — two Disposes must not both destroy.</summary>
    [Fact]
    public void TakeForDispose_IsClaimedExactlyOnce()
    {
        var owner = new NativeHandleOwner(0x99);

        var first = owner.TakeForDispose("owner", timeoutMs: 1_000, out var firstOutcome);
        var second = owner.TakeForDispose("owner", timeoutMs: 1_000, out var secondOutcome);

        Assert.Equal(HandleDrain.Drained, firstOutcome);
        Assert.Equal((nint)0x99, first);
        Assert.Equal(HandleDrain.AlreadyClaimed, secondOutcome);
        Assert.Equal(nint.Zero, second);
        Assert.False(owner.IsAlive);
    }

    /// <summary>
    /// The same ordering through the <c>TryAcquire</c> / <c>Release</c> pair —
    /// the fire-and-forget path most of the wrapper's setters use. Its Release
    /// has to wake the drain too; if only <c>Acquire</c>'s did, every no-op
    /// setter in flight would park a dispose until its deadline.
    /// </summary>
    [Fact]
    public void ATryAcquiredCallAlsoHoldsOffTheDrainUntilItReleases()
    {
        var owner = new NativeHandleOwner(0x55);
        var order = new ConcurrentQueue<string>();
        var acquired = new ManualResetEventSlim(false);
        var disposerParked = new ManualResetEventSlim(false);

        nint acquiredHandle = -1;
        var caller = new Thread(() =>
        {
            // Never Assert on a background thread — a failure there is an
            // unhandled exception, which takes the whole test host with it.
            acquiredHandle = owner.TryAcquire();
            order.Enqueue("acquired");
            acquired.Set();
            disposerParked.Wait(TimeSpan.FromSeconds(10));
            order.Enqueue("released");
            owner.Release();
        }) { IsBackground = true, Name = "tryacquire-caller" };

        var outcome = HandleDrain.AlreadyClaimed;
        var disposer = new Thread(() =>
        {
            disposerParked.Set();
            _ = owner.TakeForDispose("owner", timeoutMs: 10_000, out outcome);
            order.Enqueue("taken");
        }) { IsBackground = true, Name = "tryacquire-disposer" };

        caller.Start();
        Assert.True(acquired.Wait(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken));
        disposer.Start();

        Assert.True(caller.Join(TimeSpan.FromSeconds(10)));
        Assert.True(disposer.Join(TimeSpan.FromSeconds(15)));

        Assert.Equal((nint)0x55, acquiredHandle);
        Assert.Equal(HandleDrain.Drained, outcome);
        Assert.Equal(new[] { "acquired", "released", "taken" }, order.ToArray());
    }
}

/// <summary>
/// P0-1: the engine's drain is PER-HANDLE, so it said nothing about a child.
/// The 4 ms position poll in <c>JuceAudioPlaybackService</c> lives inside
/// <c>JuceAudioSource.AudiblePosition</c>; a device change disposing the engine
/// ran <c>dca_engine_destroy</c> (which resets the source's transport) straight
/// through it.
/// </summary>
[Collection("NullDevice")]
public sealed class EngineChildDrainTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    private readonly string? _wavPath;

    public EngineChildDrainTests()
    {
        if (NativeProbe.IsAvailable) _wavPath = TinyWav.Create(seconds: 2.0);
    }

    public void Dispose()
    {
        _scope.Dispose();
        if (_wavPath is not null) TinyWav.TryDelete(_wavPath);
    }

    /// <summary>
    /// The structural pin, and the cheapest one that fails on the old code:
    /// after the engine is disposed its sources must report themselves DISPOSED.
    /// Before the child registry the source's own handle was untouched, so the
    /// next poll dereferenced a transport <c>dca_engine_destroy</c> had already
    /// reset — an access violation, not this exception.
    /// </summary>
    [NativeAvailableFact]
    public void DisposingTheEngine_DrainsAndDestroysItsSources()
    {
        var engine = NullDevice.CreateEngine();
        var src = engine.OpenFile(_wavPath!);

        // Alive before.
        Assert.True(src.Length.TotalSeconds > 0);

        engine.Dispose();

        Assert.Throws<ObjectDisposedException>(() => _ = src.AudiblePosition);
        Assert.Throws<ObjectDisposedException>(() => _ = src.Position);
        // …and disposing the source afterwards is still a clean no-op.
        src.Dispose();
    }

    /// <summary>A disposed engine can no longer mint children — the registry is
    /// claimed AFTER the engine handle, so there is no window in which a source
    /// is opened onto a destroyed engine.</summary>
    [NativeAvailableFact]
    public void AfterDispose_TheEngineOpensNoNewChildren()
    {
        var engine = NullDevice.CreateEngine();
        engine.Dispose();

        Assert.Throws<ObjectDisposedException>(() => engine.OpenFile(_wavPath!));
    }

    /// <summary>
    /// The live interleaving the audit describes: a position poll running while
    /// the engine is disposed from another thread. The caller must observe
    /// <see cref="ObjectDisposedException" /> and the process must survive —
    /// which it only does because the source's own drain now runs before
    /// <c>dca_engine_destroy</c>.
    /// </summary>
    [NativeAvailableFact]
    public void APositionPollRacingEngineDispose_ObservesObjectDisposed_NeverACrash()
    {
        var engine = NullDevice.CreateEngine();
        var src = engine.OpenFile(_wavPath!);
        src.Play();

        var polling = new ManualResetEventSlim(false);
        var stop = new ManualResetEventSlim(false);
        var unexpected = new ConcurrentQueue<Exception>();
        var polls = 0;
        var sawDisposed = false;

        var poller = new Thread(() =>
        {
            while (!stop.IsSet)
            {
                try
                {
                    _ = src.AudiblePosition;
                    Interlocked.Increment(ref polls);
                    polling.Set();
                }
                catch (ObjectDisposedException)
                {
                    sawDisposed = true;
                    return;
                }
                catch (Exception ex)
                {
                    unexpected.Enqueue(ex);
                    return;
                }
            }
        }) { IsBackground = true, Name = "position-poll" };

        poller.Start();
        Assert.True(polling.Wait(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken),
            "the poll thread never read a position");

        engine.Dispose();

        stop.Set();
        Assert.True(poller.Join(TimeSpan.FromSeconds(10)));
        Assert.Empty(unexpected);
        Assert.True(sawDisposed || polls > 0);
        // Whatever the poller saw, the source is gone now.
        Assert.Throws<ObjectDisposedException>(() => _ = src.Position);
    }
}

/// <summary>
///     W4-M1. <see cref="JuceMidiInput" /> was given a
///     <see cref="NativeHandleOwner" /> by wave-2 #25 and never the ACQUIRE that
///     makes one mean anything: no path on the type incremented the in-flight
///     count after construction, so <c>TakeForDispose</c> always observed 0,
///     always reported <see cref="HandleDrain.Drained" />, and always handed the
///     handle to <c>dca_midi_input_close</c> — <c>device-&gt;stop();
///     b-&gt;device.reset(); delete b;</c> — whether or not the JUCE driver
///     thread was at that instant inside <c>OnMidi</c> holding a
///     <see cref="ReadOnlySpan{T}" /> over the message that object owns.
///     <para>
///         The guard written for it (<c>WrapperLifetimeAuditTests</c>) was
///         <c>Assert.Contains</c> over source text and went green on the no-op,
///         which is why these two are BEHAVIOURAL. No MIDI device is needed:
///         <c>OnMidi</c> is an ordinary instance method, so the test builds a
///         detached instance and invokes it directly — the point is the
///         delegate's own body, exactly as
///         <c>NativeCallbackSafetyTests</c> does for the catch-all.
///     </para>
/// </summary>
public sealed class MidiInputDispatchDrainTests
{
    /// <summary>
    /// An instance with no native device behind it: the ctor is skipped
    /// (it would need a real MIDI input), and the two fields OnMidi reads are
    /// planted directly. The finalizer is suppressed — on an uninitialized
    /// instance <c>_destruction</c> is null, which the <c>?.</c> in
    /// <c>DisposeNativeHandle</c> already tolerates, but there is nothing to
    /// finalize either way.
    /// </summary>
    private static (JuceMidiInput Input, NativeHandleOwner Owner, Action<byte[]> Dispatch) Detached(
        JuceMidiSpanHandler handler)
    {
        var input = (JuceMidiInput)System.Runtime.CompilerServices.RuntimeHelpers
            .GetUninitializedObject(typeof(JuceMidiInput));
        GC.SuppressFinalize(input);

        var owner = new NativeHandleOwner(0x4D1D1);
        Field("_owner").SetValue(input, owner);
        Field("_spanCallback").SetValue(input, handler);

        var onMidi = typeof(JuceMidiInput).GetMethod(
            "OnMidi",
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!;

        void Dispatch(byte[] bytes)
        {
            var block = System.Runtime.InteropServices.Marshal.AllocHGlobal(bytes.Length);
            try
            {
                System.Runtime.InteropServices.Marshal.Copy(bytes, 0, block, bytes.Length);
                onMidi.Invoke(input, new object[] { nint.Zero, 0.0, block, bytes.Length });
            }
            finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(block); }
        }

        return (input, owner, Dispatch);

        static System.Reflection.FieldInfo Field(string name)
            => typeof(JuceMidiInput).GetField(
                   name,
                   System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)
               ?? throw new InvalidOperationException($"JuceMidiInput.{name} is gone — repoint this test.");
    }

    /// <summary>
    ///     THE finding. While a dispatch is inside the user handler the handle
    ///     must NOT be handed back for destruction: the drain reports
    ///     <see cref="HandleDrain.TimedOut" /> and leaks rather than letting
    ///     <c>dca_midi_input_close</c> delete the device the driver thread is
    ///     dispatching through.
    ///     <para>Deterministic: two events drive the interleaving and the drain
    ///     deadline is injected, so nothing here is a wall-clock race.</para>
    /// </summary>
    [Fact]
    public void ADispatchInFlight_HoldsOffTheCloseInsteadOfBeingInvisibleToIt()
    {
        var inHandler = new ManualResetEventSlim(false);
        var letGo = new ManualResetEventSlim(false);

        var (_, owner, dispatch) = Detached((_, _) =>
        {
            inHandler.Set();
            letGo.Wait(TimeSpan.FromSeconds(10));
        });

        var driver = new Thread(() => dispatch(new byte[] { 0x90, 0x40, 0x7F }))
        { IsBackground = true, Name = "fake-midi-driver" };
        driver.Start();

        Assert.True(inHandler.Wait(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken),
            "the fake driver thread never reached the user handler");

        var h = owner.TakeForDispose(nameof(JuceMidiInput), timeoutMs: 100, out var outcome);

        Assert.Equal(HandleDrain.TimedOut, outcome);
        Assert.Equal(nint.Zero, h);

        letGo.Set();
        Assert.True(driver.Join(TimeSpan.FromSeconds(10)));
    }

    /// <summary>
    /// The second half of the same pin, and the half a user notices: once
    /// Dispose has CLAIMED the handle the device is being deleted, so a
    /// callback the driver had already queued must return without invoking the
    /// host's handler. Before the acquire it ran it — over native memory the
    /// close had freed.
    /// </summary>
    [Fact]
    public void ADispatchAfterTheHandleIsClaimed_NeverReachesTheUserHandler()
    {
        var seen = 0;
        var (_, owner, dispatch) = Detached((_, _) => Interlocked.Increment(ref seen));

        dispatch(new byte[] { 0x90, 0x40, 0x7F });
        Assert.Equal(1, seen); // the handler is wired and normally runs

        Assert.NotEqual(nint.Zero, owner.TakeForDispose(nameof(JuceMidiInput), 0, out var outcome));
        Assert.Equal(HandleDrain.Drained, outcome);

        dispatch(new byte[] { 0x80, 0x40, 0x00 });
        Assert.Equal(1, seen);
    }
}
