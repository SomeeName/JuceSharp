using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// A loop region belongs to exactly ONE source, and re-assigning the SAME region
/// must not tear it down. Both halves were broken and both were invisible: the
/// property kept reporting a region that had stopped looping.
/// </summary>
[Collection("NullDevice")]
public sealed class JuceLoopRegionOwnershipTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    /// <summary>
    ///     <c>src.Loop = src.Loop</c> — which any "refresh the A/B loop" UI does by
    ///     accident — used to dispose the region and then re-attach it, and
    ///     <c>Attach</c> on a disposed region tears its own timer straight back
    ///     down. The loop silently stopped looping while <c>Loop</c> still returned
    ///     a region.
    /// </summary>
    [NativeAvailableFact]
    public void AssigningTheSameRegionBack_IsANoOp_AndTheLoopKeepsRunning()
    {
        using var engine = NullDevice.CreateEngine();
        using var src = engine.OpenFile(TinyWav.Create());

        var region = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1));
        src.Loop = region;
        Assert.True(region.HasTimerForTests);

        src.Loop = src.Loop; // the self-assignment
        Assert.Same(region, src.Loop);
        Assert.True(region.HasTimerForTests,
            "self-assigning Loop disposed the region: the A/B loop is dead but the property still reports one");
    }

    /// <summary>Replacing a region with a DIFFERENT one still disposes the old.</summary>
    [NativeAvailableFact]
    public void ReplacingTheRegion_DisposesTheOldOne()
    {
        using var engine = NullDevice.CreateEngine();
        using var src = engine.OpenFile(TinyWav.Create());

        var first = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1));
        var second = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(2));
        src.Loop = first;
        src.Loop = second;

        Assert.False(first.HasTimerForTests);
        Assert.True(second.HasTimerForTests);
        Assert.Same(second, src.Loop);
    }

    /// <summary>
    ///     A region moved to a second source used to be a silent no-op that kept
    ///     the FIRST source — so the loop seeked the stem it had been taken off,
    ///     with both properties reporting the region the host thought it had set.
    /// </summary>
    [NativeAvailableFact]
    public void ARegionCannotBeAttachedToASecondSource()
    {
        using var engine = NullDevice.CreateEngine();
        var wav = TinyWav.Create();
        using var a = engine.OpenFile(wav);
        using var b = engine.OpenFile(wav);

        var region = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1));
        a.Loop = region;

        Assert.Throws<InvalidOperationException>(() => b.Loop = region);
    }
}

/// <summary>
///     W4-M5. The OTHER path into "the property reports a loop that never
///     loops". <c>Dispose()</c> resets both <c>_source</c> and <c>_timer</c> to
///     null, so a DISPOSED region passed both CAS gates in <c>Attach</c>,
///     published a timer, hit the race check a few lines down, tore the timer
///     straight back off and returned NORMALLY —
///     <see cref="JuceAudioSource.Loop" />'s setter saw no throw, stored it, and
///     the property reported a live loop. The <c>ReferenceEquals</c> early-out
///     that shipped covers only <c>src.Loop = src.Loop</c>;
///     <c>src.Loop = aRegionSomebodyDisposed</c> was still silent, while the
///     setter's own doc said the failure was fixed.
/// </summary>
[Collection("NullDevice")]
public sealed class DisposedLoopRegionRejectionTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    /// <summary>The throw, and — because the setter attaches BEFORE it swaps —
    /// the existing loop surviving it, which is the reason the order is
    /// load-bearing.</summary>
    [NativeAvailableFact]
    public void AssigningADisposedRegion_Throws_AndLeavesTheRunningLoopAlone()
    {
        using var engine = NullDevice.CreateEngine();
        using var src = engine.OpenFile(TinyWav.Create());

        var live = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1));
        src.Loop = live;
        Assert.True(live.HasTimerForTests);

        var corpse = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(2));
        corpse.Dispose();

        Assert.Throws<ObjectDisposedException>(() => src.Loop = corpse);

        Assert.Same(live, src.Loop);
        Assert.True(live.HasTimerForTests,
            "the rejected region took the running loop down with it");
        Assert.False(corpse.HasTimerForTests);
    }

    /// <summary>A region disposed AFTER it was attached is the same corpse: the
    /// source keeps reporting it (that is Dispose's contract, not a bug), but
    /// re-arming it on a second source must not read as success either.</summary>
    [NativeAvailableFact]
    public void ARegionDisposedAfterAttach_CannotBeReArmed()
    {
        using var engine = NullDevice.CreateEngine();
        var wav = TinyWav.Create();
        using var a = engine.OpenFile(wav);
        using var b = engine.OpenFile(wav);

        var region = new JuceLoopRegion(TimeSpan.Zero, TimeSpan.FromSeconds(1));
        a.Loop = region;
        region.Dispose();

        Assert.Throws<ObjectDisposedException>(() => b.Loop = region);
        Assert.Null(b.Loop);
    }
}
