using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// <b>Reads how many bytes this thread allocated across a region, and knows
/// that the reading is not the same thing as the fact.</b>
///
/// <para>
/// <c>GC.GetAllocatedBytesForCurrentThread()</c> is
/// <c>alloc_bytes + alloc_bytes_uoh - (alloc_limit - alloc_ptr)</c>. A
/// collection RETIRES the calling thread's allocation context, so the
/// unconsumed remainder of its current quantum stops being subtracted and is
/// charged to the thread permanently: <b>two reads straddling a GC differ even
/// when nothing allocated between them</b>, by anything from a few bytes up to
/// the ~8 KB quantum. Every probe in this suite asserts zero, so a single GC
/// landing in the window is a failure with no defect behind it. Measured on
/// this box on 2026-09-12:
/// <c>StreamingSpanPushTests.GuitarAnalyzerSpanPush_AllocatesNothingWhenNoEventsAreEmitted</c>
/// reported 360 bytes over a region whose subject allocates none.
/// </para>
/// <para>
/// Two things answer it and both are needed. <c>[Collection("AllocationProbes")]</c>
/// (see <c>TestCollections.cs</c>) stops the rest of the suite making garbage
/// alongside — that is most of the exposure, and it is not all of it, because
/// the runtime's own threads still allocate. This helper is the other half: it
/// settles the heap so the window starts on a FRESH quantum, and if the
/// reading is still non-zero it takes it again. <b>A real per-call allocation
/// is deterministic and survives every retry</b>, so this cannot hide a
/// regression; the probes it serves measure regions that allocate tens of KB
/// the moment their subject regresses.
/// </para>
/// <para>
/// What this deliberately is NOT: a wall clock (the repo rule is absolute), and
/// not <c>GC.TryStartNoGCRegion</c> — that THROWS when the budget cannot be
/// granted, which on a loaded box is the same flake wearing a different
/// exception.
/// </para>
/// <para>
/// The caller still warms its own path up before calling here. The first-ever
/// execution of the code under test drags tier-0 JIT and first-touch cctors
/// into the window, widening it by orders of magnitude and multiplying the odds
/// of a collection landing inside it.
/// </para>
/// </summary>
internal static class AllocationProbe
{
    /// <summary>
    /// Bytes this thread allocated while running <paramref name="work"/>.
    /// <paramref name="work"/> may be run more than once and must therefore be
    /// repeatable; its closure is allocated by the CALLER, before the window
    /// opens, and invoking a delegate allocates nothing.
    /// </summary>
    public static long Measure(Action work)
    {
        Settle();
        var delta = MeasureOnce(work);
        if (delta == 0) return 0;

        Settle();
        return MeasureOnce(work);
    }

    private static long MeasureOnce(Action work)
    {
        var before = GC.GetAllocatedBytesForCurrentThread();
        work();
        return GC.GetAllocatedBytesForCurrentThread() - before;
    }

    /// <summary>Collect twice around the finalizer queue, so the window opens on a
    /// fresh allocation context with a full gen-0 budget in front of it.</summary>
    private static void Settle()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }
}

/// <summary>
/// Non-vacuity for <see cref="AllocationProbe"/> itself. A helper that always
/// answered zero — because the delegate was never invoked, or because the
/// retry laundered the reading — would make every probe in this suite pass for
/// the wrong reason, and nothing else would notice.
/// </summary>
[Collection("AllocationProbes")]
public sealed class AllocationProbeTests
{
    [Fact]
    public void Measure_ReadsZeroForAnEmptyRegion_AndTheRealSizeForAnAllocatingOne()
    {
        var sink = new byte[1][];
        var empty = Array.Empty<byte>();

        Assert.Equal(0, AllocationProbe.Measure(() => sink[0] = empty));

        // 64 KB is far outside anything a retired allocation context can account
        // for, and it is DETERMINISTIC — which is exactly why settling and
        // retrying cannot launder a real allocation, only a GC artefact.
        var delta = AllocationProbe.Measure(() => sink[0] = new byte[64 * 1024]);
        Assert.InRange(delta, 64 * 1024, 96 * 1024);
    }
}
