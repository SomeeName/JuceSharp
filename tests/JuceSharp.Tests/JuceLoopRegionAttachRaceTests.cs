using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     Concurrency audit #11: <c>JuceLoopRegion.Attach</c> assigned a RUNNING
///     timer after the region was already reachable, so a <c>Dispose()</c> that
///     landed between the source write and that assignment saw a null field,
///     disposed nothing, and the timer then fired every 5 ms forever — rooting
///     the region and the source it pointed at. Driven without a native source:
///     <c>Attach(null!)</c> only publishes the timer, and a tick against a null
///     source is a no-op, so the race is purely managed.
/// </summary>
public sealed class JuceLoopRegionAttachRaceTests
{
    [Fact]
    public void ARegionDisposedDuringAttach_NeverKeepsATimer()
    {
        // Many rounds, two threads, no timing assumption: the outcome we assert
        // must hold for EVERY interleaving, so any interleaving the scheduler
        // produces is a valid trial.
        for (var round = 0; round < 500; round++)
        {
            var region = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1));
            using var go = new ManualResetEventSlim(false);
            var attach = new Thread(() =>
            {
                go.Wait();
                // Losing the race to Dispose is now an ObjectDisposedException
                // rather than a silent no-op (W4-M5), and it has to be caught
                // HERE: an escaping exception on a background thread is an
                // unhandled exception, which takes the whole test host with it.
                // Never Assert on this thread for the same reason.
                try { region.Attach(null!); }
                catch (ObjectDisposedException) { /* Dispose won — the documented answer */ }
            });
            var dispose = new Thread(() =>
            {
                go.Wait();
                region.Dispose();
            });
            attach.Start();
            dispose.Start();
            go.Set();
            attach.Join();
            dispose.Join();

            Assert.False(region.HasTimerForTests,
                $"round {round}: a disposed loop region still holds a poll timer");
        }
    }

    [Fact]
    public void AttachThenDispose_ReleasesTheTimer_AndASecondAttachIsIgnored()
    {
        var region = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1));
        region.Attach(null!);
        Assert.True(region.HasTimerForTests);
        region.Attach(null!); // attached twice: the first timer is kept, the second dropped
        Assert.True(region.HasTimerForTests);

        region.Dispose();
        Assert.False(region.HasTimerForTests);

        // Attach after Dispose used to publish a timer, tear it back down and
        // return NORMALLY — which is what let JuceAudioSource.Loop store a
        // corpse and report a live loop (W4-M5). It is a throw now, and it
        // still leaves no timer behind.
        Assert.Throws<ObjectDisposedException>(() => region.Attach(null!));
        Assert.False(region.HasTimerForTests);
    }
}
