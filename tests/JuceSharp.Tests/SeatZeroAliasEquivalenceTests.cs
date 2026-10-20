using System;
using JuceSharp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// W4-N10, from the other end: the four pre-v28 capture exports and their
/// <c>_for_player(…, 0)</c> twins answer identically, for GOOD arguments and for
/// BAD ones.
/// <para>
/// <see cref="AbiSentinelAndAliasTests" /> pins the source shape — that the
/// seat-0 export is a one-line delegation and not a second copy. This pins the
/// BEHAVIOUR, and it is the half that matters: the reason the copies were worth
/// collapsing is that two implementations of one contract drift, and a lint that
/// only reads the source would be satisfied by a delegation to the wrong twin or
/// with the wrong seat.
/// </para>
/// <para>
/// Runs on the null device so it needs no audio hardware and no signal: the
/// capture ring's CONTENTS are irrelevant here, only that both entry points
/// report the same thing about it.
/// </para>
/// </summary>
[Collection("NullDevice")]
public sealed class SeatZeroAliasEquivalenceTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    /// <summary>
    /// An engine whose capture ring has real content and is then FROZEN.
    /// <para>
    /// <b>Two consecutive reads of a LIVE, self-paced ring are not comparable</b>,
    /// and the "good call" half of these tests assumed they were:
    /// <c>ReadInputCaptureStream2_SeatZeroAndForPlayerZero_Agree</c> failed a
    /// full gate on 2026-09-12 with <c>Expected: 256, Actual: 224</c> — the
    /// first read drained the ring and the pacing thread had refilled only 224
    /// frames before the second one, which on a box running two test suites at
    /// once is simply what happened, not a disagreement between the two entry
    /// points. Stopping the device stops the callback that writes the ring, so
    /// every read below describes the same unchanging content and every
    /// assertion is exact at any machine speed.
    /// </para>
    /// </summary>
    private static JuceAudioEngine QuiescentEngineWithACapturedRing()
    {
        var engine = NullDevice.CreateEngine(inputChannels: 2);

        var deadline = Environment.TickCount64 + 2000;
        while (!engine.TryGetInputCaptureSync(out var samples, out _) || samples == 0)
        {
            if (Environment.TickCount64 >= deadline)
            {
                engine.Dispose();
                Assert.Fail("the null device's capture ring never advanced");
            }
            Thread.Sleep(10);
        }

        engine.Stop();
        return engine;
    }

    [NativeAvailableFact]
    public unsafe void ReadInputCapture_SeatZeroAndForPlayerZero_Agree()
    {
        using var engine = QuiescentEngineWithACapturedRing();
        var h = engine.NativeHandle;
        Assert.NotEqual(nint.Zero, h);

        var a = new float[256];
        var b = new float[256];

        fixed (float* pa = a)
        fixed (float* pb = b)
        {
            // --- the good call ------------------------------------------------
            // readInputCapture is WINDOWED: it hands back the most recent `count`
            // samples and consumes nothing. On a frozen ring the two entry points
            // must therefore agree on the count AND return the same samples —
            // which is only assertable because the ring has stopped moving.
            var sa = NativeMethods.EngineReadInputCapture(h, pa, a.Length, out var wa);
            var sb = NativeMethods.EngineReadInputCaptureForPlayer(h, 0, pb, b.Length, out var wb);
            Assert.Equal(sa, sb);
            Assert.Equal(wa, wb);
            Assert.True(wa > 0, "the ring was empty, so the agreement above is vacuous");
            Assert.Equal(a.AsSpan(0, wa).ToArray(), b.AsSpan(0, wb).ToArray());

            // --- and the FAILURE arguments, which is where the two copies
            //     diverged: count <= 0 is rejected, and *out_written must come
            //     back zeroed rather than holding whatever was there.
            wa = 12345; wb = 54321;
            sa = NativeMethods.EngineReadInputCapture(h, pa, 0, out wa);
            sb = NativeMethods.EngineReadInputCaptureForPlayer(h, 0, pb, 0, out wb);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, sa);
            Assert.Equal(sa, sb);
            Assert.Equal(0, wa);
            Assert.Equal(0, wb);
        }
    }

    [NativeAvailableFact]
    public unsafe void ReadInputCaptureStream_SeatZeroAndForPlayerZero_Agree()
    {
        using var engine = QuiescentEngineWithACapturedRing();
        var h = engine.NativeHandle;

        var a = new float[256];
        var b = new float[256];

        fixed (float* pa = a)
        fixed (float* pb = b)
        {
            // The streaming read CONSUMES, against ONE cursor per seat — so the
            // question is not "do two reads return the same count" (they must
            // NOT: the second sees only what the first left) but "do the two
            // entry points share the cursor". Drain through the seat-0 export,
            // then ask the twin: a second cursor would hand the ring back again.
            NativeMethods.DcaStatus sa;
            int wa;
            var drained = 0L;
            while (true)
            {
                sa = NativeMethods.EngineReadInputCaptureStream(h, pa, a.Length, out wa);
                Assert.Equal(NativeMethods.DcaStatus.Ok, sa);
                if (wa == 0) break;
                drained += wa;
            }
            Assert.True(drained > 0, "the capture ring never held a sample — the agreement below is vacuous");

            var sb = NativeMethods.EngineReadInputCaptureStreamForPlayer(h, 0, pb, b.Length, out var wb);
            Assert.Equal(sa, sb);
            Assert.Equal(wa, wb);
            Assert.Equal(0, wb);

            sa = NativeMethods.EngineReadInputCaptureStream(h, pa, -1, out wa);
            sb = NativeMethods.EngineReadInputCaptureStreamForPlayer(h, 0, pb, -1, out wb);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, sa);
            Assert.Equal(sa, sb);
            Assert.Equal(0, wa);
            Assert.Equal(0, wb);
        }
    }

    [NativeAvailableFact]
    public unsafe void ReadInputCaptureStream2_SeatZeroAndForPlayerZero_Agree()
    {
        using var engine = QuiescentEngineWithACapturedRing();
        var h = engine.NativeHandle;

        var a = new float[256];
        var b = new float[256];

        fixed (float* pa = a)
        fixed (float* pb = b)
        {
            // Same shape as the sibling above, plus the thing only this export
            // can show: the reported position is the ABSOLUTE index of dest[0],
            // so a shared cursor is provable arithmetically — every read starts
            // exactly where the previous one ended, drained ring or not.
            NativeMethods.DcaStatus sa;
            int wa;
            ulong posA;
            var drained = 0UL;
            var expectedNext = ulong.MaxValue;
            while (true)
            {
                sa = NativeMethods.EngineReadInputCaptureStream2(h, pa, a.Length, out wa, out posA);
                Assert.Equal(NativeMethods.DcaStatus.Ok, sa);
                if (expectedNext != ulong.MaxValue) Assert.Equal(expectedNext, posA);
                expectedNext = posA + (ulong)wa;
                if (wa == 0) break;
                drained += (ulong)wa;
            }
            Assert.True(drained > 0, "the capture ring never held a sample — the agreement below is vacuous");

            var sb = NativeMethods.EngineReadInputCaptureStream2ForPlayer(h, 0, pb, b.Length, out var wb, out var posB);
            Assert.Equal(sa, sb);
            Assert.Equal(wa, wb);
            Assert.Equal(0, wb);
            // THE alias assertion: the twin resumes at the cursor the seat-0
            // export left behind, rather than at one of its own.
            Assert.Equal(posA, posB);

            sa = NativeMethods.EngineReadInputCaptureStream2(h, pa, 0, out wa, out posA);
            sb = NativeMethods.EngineReadInputCaptureStream2ForPlayer(h, 0, pb, 0, out wb, out posB);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, sa);
            Assert.Equal(sa, sb);
            Assert.Equal(0, wa);
            Assert.Equal(0, wb);
            Assert.Equal(0UL, posA);
            Assert.Equal(0UL, posB);
        }
    }

    [NativeAvailableFact]
    public void GetInputCaptureSync_SeatZeroAndForPlayerZero_Agree()
    {
        using var engine = NullDevice.CreateEngine(inputChannels: 2);
        var h = engine.NativeHandle;

        var sa = NativeMethods.EngineGetInputCaptureSync(h, out var samplesA, out var ticksA);
        var sb = NativeMethods.EngineGetInputCaptureSyncForPlayer(h, 0, out var samplesB, out var ticksB);

        Assert.Equal(sa, sb);
        // The counters run, so the two calls are not required to return the same
        // sample index — only the same ANSWER about whether there is one, and
        // ordered the way the calls were made.
        if (sa == NativeMethods.DcaStatus.Ok)
        {
            Assert.True(samplesB >= samplesA,
                $"the seat-0 alias reported {samplesA} captured samples and the twin, called "
                + $"immediately after, reported {samplesB} — they are not reading the same ring");
            Assert.True(ticksB >= ticksA);
        }
    }

    /// <summary>
    /// Non-vacuity for the three theories above: a seat PAST the last one is
    /// rejected, never clamped, so "the alias forwards seat 0" is a claim with a
    /// distinguishable negative. If <c>_for_player</c> clamped, this would
    /// succeed and the equivalence assertions would be trivially true for every
    /// seat.
    /// </summary>
    [NativeAvailableFact]
    public unsafe void ASeatPastTheLast_IsRejectedRatherThanClamped()
    {
        using var engine = NullDevice.CreateEngine(inputChannels: 2);
        var h = engine.NativeHandle;

        var a = new float[256];
        fixed (float* pa = a)
        {
            var s = NativeMethods.EngineReadInputCaptureForPlayer(
                h, NativeMethods.MaxPlayers, pa, a.Length, out var written);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, s);
            Assert.Equal(0, written);
        }
    }
}
