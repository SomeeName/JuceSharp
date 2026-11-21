using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The "probe the count, then copy" calls (device lists, a plugin's saved state): the
/// native side can report more items on the copy than on the probe, and the wrapper must
/// neither index past its buffer nor hand back a padded blob.
/// </summary>
public sealed class CountedReadTests
{
    private const NativeMethods.DcaStatus Ok = NativeMethods.DcaStatus.Ok;

    [Fact]
    public void Read_ListThatGrewBetweenProbeAndCopy_IsCopiedWhole()
    {
        var calls = 0;
        var (items, count) = CountedReads.Read<int>((int[]? buf, int cap, out int n) =>
        {
            calls++;
            var total = calls == 1 ? 2 : 3;   // two at the probe, three by the time it is copied
            n = total;
            if (buf != null)
                for (var i = 0; i < Math.Min(cap, total); i++) buf[i] = 10 + i;
            return Ok;
        });

        Assert.Equal(3, count);
        Assert.True(items.Length >= count);
        Assert.Equal(new[] { 10, 11, 12 }, items.Take(count));
    }

    [Fact]
    public void Read_ListThatKeepsGrowing_StopsAndNeverReportsMoreThanItHolds()
    {
        var calls = 0;
        var (items, count) = CountedReads.Read<int>((int[]? buf, int cap, out int n) =>
        {
            calls++;
            n = cap + 1;
            if (buf != null)
                for (var i = 0; i < cap; i++) buf[i] = i;
            return Ok;
        });

        Assert.True(count <= items.Length, $"count {count} exceeds the buffer of {items.Length}");
        Assert.True(calls <= 1 + CountedReads.MaxAttempts, $"{calls} native calls");
    }

    [Fact]
    public void Read_ListThatShrank_ReportsWhatWasCopied()
    {
        var (items, count) = CountedReads.Read<int>((int[]? buf, int cap, out int n) =>
        {
            n = buf == null ? 5 : 2;
            return Ok;
        });
        Assert.Equal(2, count);
        Assert.True(items.Length >= count);
    }

    [Fact]
    public void Read_NativeFailureOrEmptyList_IsEmpty()
    {
        var (_, failedCount) = CountedReads.Read<int>(
            (int[]? buf, int cap, out int n) => { n = 3; return NativeMethods.DcaStatus.InvalidArg; });
        var (_, emptyCount) = CountedReads.Read<int>((int[]? buf, int cap, out int n) => { n = 0; return Ok; });
        Assert.Equal(0, failedCount);
        Assert.Equal(0, emptyCount);
    }

    [Fact]
    public void ReadState_StateThatGrewBetweenSizeAndCopy_IsCopiedWhole()
    {
        var state = new byte[] { 1, 2, 3, 4, 5, 6 };
        var calls = 0;
        var got = JuceAudioPlugin.ReadState((byte[]? buf, int cap, out int n) =>
        {
            calls++;
            var size = calls == 1 ? 4 : state.Length;   // four bytes at the size query, six by the copy
            n = size;
            if (buf != null)
                Array.Copy(state, buf, Math.Min(cap, size));
            return Ok;
        });

        Assert.Equal(state, got);
    }

    [Fact]
    public void ReadState_StateThatNeverSettles_ThrowsAfterABoundedNumberOfAttempts()
    {
        var calls = 0;
        Assert.Throws<InvalidOperationException>(() => JuceAudioPlugin.ReadState((byte[]? buf, int cap, out int n) =>
        {
            calls++;
            n = cap + 1;
            return Ok;
        }));
        Assert.True(calls <= 1 + JuceAudioPlugin.SaveStateAttempts, $"{calls} native calls");
    }

    [Fact]
    public void ReadState_StateThatShrank_IsTrimmedNotPadded()
    {
        var got = JuceAudioPlugin.ReadState((byte[]? buf, int cap, out int n) =>
        {
            n = buf == null ? 4 : 2;
            if (buf != null) { buf[0] = 7; buf[1] = 8; }
            return Ok;
        });
        Assert.Equal(new byte[] { 7, 8 }, got);
    }

    [Fact]
    public void ReadState_EmptyState_IsEmpty()
    {
        Assert.Empty(JuceAudioPlugin.ReadState((byte[]? buf, int cap, out int n) => { n = 0; return Ok; }));
    }

    [Fact]
    public void ReadState_NativeFailure_Throws()
    {
        Assert.Throws<JuceAudioException>(() => JuceAudioPlugin.ReadState(
            (byte[]? buf, int cap, out int n) => { n = 0; return NativeMethods.DcaStatus.Plugin; }));
    }
}
