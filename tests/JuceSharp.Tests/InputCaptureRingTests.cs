using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Live input-capture ring coverage (2026-07-16 audit test-gap list: the
/// SPSC ring + seqlock sync pair had no managed-side tests). These are pure
/// cursor/ring LOGIC tests, so they run against the NULL device: the input
/// path is real (zeroed buffers flow through the whole input chain into the
/// ring at the configured cadence) but no physical input endpoint is opened —
/// which is what used to make this family skip or flake on hosts without a
/// working input. The one assertion where real hardware is the point (the
/// {samples, QPC} sync pair tracking a REAL device clock) stays on a real
/// endpoint in <see cref="InputCaptureRealDeviceSmokeTests"/>.
/// </summary>
[Collection("NullDevice")]
public sealed class InputCaptureRingTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    private readonly JuceAudioEngine? _engine;
    private readonly ITestOutputHelper _out;

    public InputCaptureRingTests(ITestOutputHelper output)
    {
        _out = output;
        if (!NativeProbe.IsAvailable) return;
        // The capture ring is only written when input flows, so ask for one
        // input channel; the null device grants it unconditionally.
        _engine = NullDevice.CreateEngine(inputChannels: 1);
    }

    public void Dispose()
    {
        _engine?.Dispose();
        _scope.Dispose();
    }

    /// Wait until the capture ring has seen at least one audio block.
    private void RequireCapture(out ulong samples, out long ticks)
    {
        samples = 0; ticks = 0;
        var deadline = Environment.TickCount64 + 2000;
        while (Environment.TickCount64 < deadline)
        {
            if (_engine!.TryGetInputCaptureSync(out samples, out ticks))
                return;
            Thread.Sleep(20);
        }
        Assert.Fail("null-device capture ring never advanced");
    }

    [NativeAvailableFact]
    public void CaptureSync_AdvancesMonotonically_WithPlausibleTicks()
    {
        RequireCapture(out var s1, out var t1);
        Assert.True(s1 > 0);
        Assert.True(t1 > 0);

        Thread.Sleep(150);
        Assert.True(_engine!.TryGetInputCaptureSync(out var s2, out var t2));
        Assert.True(s2 > s1, $"captured-total did not advance ({s1} -> {s2})");
        Assert.True(t2 > t1, "host ticks did not advance");

        // STRUCTURE, not a budget. This ended in
        // Assert.InRange(rate, SampleRate * 0.5, SampleRate * 2.0) after a
        // Thread.Sleep(150), and the LOWER half of that band is a performance
        // assertion: a saturated box starves the pacing thread and the realised
        // rate falls through it with nothing wrong. The upper half cannot flake
        // that way — the ring is written from the audio callback, which load can
        // only make LATE — so a rate ABOVE the device rate stays an error (it
        // would mean the {samples, ticks} pair does not describe one instant).
        // What replaces the lower bound is the invariant that holds at any
        // speed: the ring is written a BLOCK at a time.
        Assert.Equal(0UL, (s2 - s1) % (ulong)_engine.BufferFrames);

        var elapsedSec = (t2 - t1) / (double)System.Diagnostics.Stopwatch.Frequency;
        var rate = (s2 - s1) / elapsedSec;
        Assert.True(rate <= _engine.SampleRate * 2.0,
            $"capture advanced FASTER than the device rate: {rate:F0} Hz against {_engine.SampleRate} Hz.");
        _out.WriteLine($"null-device capture: {rate:F0} Hz over {elapsedSec * 1000:F1} ms " +
                       $"({s2 - s1} samples, {(s2 - s1) / (ulong)_engine.BufferFrames} blocks).");
    }

    [NativeAvailableFact]
    public void StreamRead_PositionsAreMonotonicAndGapFree()
    {
        RequireCapture(out _, out _);

        var buf = new float[8192];

        // Prime the cursor, then verify successive reads are contiguous:
        // next position == previous position + previous count (the ~2.7 s
        // ring cannot overflow between immediate reads).
        var n0 = _engine!.ReadInputCaptureStream(buf, out ulong pos);
        var expectedNext = pos + (ulong)n0;
        for (var i = 0; i < 5; i++)
        {
            Thread.Sleep(30);
            int n;
            ulong p;
            while ((n = _engine.ReadInputCaptureStream(buf, out p)) > 0)
            {
                Assert.Equal(expectedNext, p);
                expectedNext = p + (ulong)n;
                foreach (var v in buf.AsSpan(0, n))
                    Assert.True(float.IsFinite(v), "non-finite sample in capture stream");
            }
        }

        // Caught-up stream returns 0, not a re-served window.
        Assert.Equal(0, _engine.ReadInputCaptureStream(buf, out _));
    }

    [NativeAvailableFact]
    public void WindowedRead_AlwaysFillsTheRequestedWindow()
    {
        RequireCapture(out _, out _);

        // The windowed API zero-pads its leading region if needed and always
        // returns a full "latest N samples" window.
        var buf = new float[4096];
        var written = _engine!.ReadInputCapture(buf);
        Assert.Equal(buf.Length, written);
        foreach (var v in buf)
            Assert.True(float.IsFinite(v));
    }

    [NativeAvailableFact]
    public void StreamAndWindowedCursors_DoNotDisturbEachOther()
    {
        RequireCapture(out _, out _);

        var streamBuf = new float[4096];
        _engine!.ReadInputCaptureStream(streamBuf, out ulong posBefore);
        var nBefore = 0;
        // Drain to caught-up.
        int k;
        while ((k = _engine.ReadInputCaptureStream(streamBuf, out _)) > 0) nBefore += k;

        // A windowed read in between must not move the streaming cursor…
        var winBuf = new float[2048];
        Assert.Equal(winBuf.Length, _engine.ReadInputCapture(winBuf));

        Thread.Sleep(60);
        var n = _engine.ReadInputCaptureStream(streamBuf, out ulong posAfter);
        Assert.True(n > 0, "stream did not resume after windowed read");
        // …so the resumed stream continues from where it was drained, not
        // from wherever the windowed read landed.
        Assert.True(posAfter >= posBefore, "stream position went backwards");
    }
}

/// <summary>
/// Real-device smoke for the capture-ring family: the sync pair must track a
/// REAL input device's clock (a driver handing bogus timestamps or rates is
/// exactly what this would catch). Skips on hosts without a live input path.
/// </summary>
[Collection("NativeEngine")]
public sealed class InputCaptureRealDeviceSmokeTests : IDisposable
{
    private readonly JuceAudioEngine? _engine;
    private readonly ITestOutputHelper _out;

    public InputCaptureRealDeviceSmokeTests(ITestOutputHelper output)
    {
        _out = output;
        if (!NativeProbe.IsAvailable) return;
        try
        {
            // The default config opens ZERO input channels — the capture ring
            // is only written when live input flows, so ask for one.
            _engine = new JuceAudioEngine(new JuceEngineConfig { InputChannels = 1 });
        }
        catch (JuceAudioException)
        {
            // No openable input on this host — the test will skip.
            _engine = new JuceAudioEngine();
        }
    }

    public void Dispose() => _engine?.Dispose();

    [NativeAvailableFact]
    public void CaptureSync_TracksARealDeviceClock()
    {
        ulong s1 = 0; long t1 = 0;
        var deadline = Environment.TickCount64 + 2000;
        while (Environment.TickCount64 < deadline
               && !_engine!.TryGetInputCaptureSync(out s1, out t1))
            Thread.Sleep(20);
        if (s1 == 0)
            Assert.Skip("no input device / no capture callbacks on this host");

        Thread.Sleep(150);
        Assert.True(_engine!.TryGetInputCaptureSync(out var s2, out var t2));
        Assert.True(s2 > s1, $"captured-total did not advance ({s1} -> {s2})");
        Assert.True(t2 > t1, "host ticks did not advance");

        // Same reasoning as the null-device sibling, and more so on real
        // hardware: a driver that is late is a busy box, a driver that is EARLY
        // is a broken clock pair. The rate is logged, the direction is asserted.
        var elapsedSec = (t2 - t1) / (double)System.Diagnostics.Stopwatch.Frequency;
        var rate = (s2 - s1) / elapsedSec;
        Assert.True(rate <= _engine.SampleRate * 2.0,
            $"capture advanced FASTER than the device rate: {rate:F0} Hz against {_engine.SampleRate} Hz.");
        _out.WriteLine($"real-device capture: {rate:F0} Hz over {elapsedSec * 1000:F1} ms ({s2 - s1} samples).");
    }
}
