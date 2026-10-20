using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using JuceSharp;
using JuceSharp.Dsp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v37: SPILLOVER (a removed delay/reverb rings out as a post-chain tail),
/// RE-AMP (a mono take replaces one seat's live input) and the cursorless
/// positional capture read a DI recorder follows the input with. All additive;
/// both features are OFF / empty at engine start.
/// </summary>
public sealed class AbiV37Tests
{
    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var header = File.ReadAllText(FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));
        var m = System.Text.RegularExpressions.Regex.Match(header, @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 37u, "the v37 exports predate the current version");
    }

    /// <summary>Every v37 entry point is really IN the built DLL — a declaration
    /// compiles whether or not the definition reaches the binary.</summary>
    [Theory]
    [InlineData("dca_engine_dsp_set_spillover_for_player")]
    [InlineData("dca_engine_dsp_get_spillover_for_player")]
    [InlineData("dca_engine_dsp_get_spill_tail_count_for_player")]
    [InlineData("dca_engine_read_input_capture_from_for_player")]
    [InlineData("dca_engine_set_input_injection_for_player")]
    [InlineData("dca_engine_clear_input_injection_for_player")]
    [InlineData("dca_engine_set_input_injection_playing_for_player")]
    [InlineData("dca_engine_set_input_injection_loop_for_player")]
    [InlineData("dca_engine_seek_input_injection_for_player")]
    [InlineData("dca_engine_get_input_injection_status_for_player")]
    public void TheV37Exports_AreInTheDll(string export)
    {
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }

        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr),
            $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    /// <summary>A NULL handle is DCA_ERR_INVALID_ARG (or -1 for the one
    /// value-returning export), never DCA_OK with a plausible zero.</summary>
    [NativeAvailableFact]
    public unsafe void EveryV37Export_RejectsANullEngine()
    {
        var bad = NativeMethods.DcaStatus.InvalidArg;
        Assert.Equal(bad, NativeMethods.EngineDspSetSpilloverForPlayer(nint.Zero, 0, 1));
        Assert.Equal(bad, NativeMethods.EngineDspGetSpilloverForPlayer(nint.Zero, 0, out _));
        Assert.Equal(-1, NativeMethods.EngineDspGetSpillTailCountForPlayer(nint.Zero, 0));
        var buf = stackalloc float[16];
        Assert.Equal(bad, NativeMethods.EngineReadInputCaptureFromForPlayer(nint.Zero, 0, 0, buf, 16, out var w, out var first));
        Assert.Equal(0, w);
        Assert.Equal(0UL, first);
        Assert.Equal(bad, NativeMethods.EngineSetInputInjectionForPlayer(nint.Zero, 0, buf, 16, 1));
        Assert.Equal(bad, NativeMethods.EngineClearInputInjectionForPlayer(nint.Zero, 0));
        Assert.Equal(bad, NativeMethods.EngineSetInputInjectionPlayingForPlayer(nint.Zero, 0, 1));
        Assert.Equal(bad, NativeMethods.EngineSetInputInjectionLoopForPlayer(nint.Zero, 0, 1));
        Assert.Equal(bad, NativeMethods.EngineSeekInputInjectionForPlayer(nint.Zero, 0, 0));
        Assert.Equal(bad, NativeMethods.EngineGetInputInjectionStatusForPlayer(nint.Zero, 0,
            out var pos, out var len, out var playing, out var loop));
        Assert.Equal(0L, pos); Assert.Equal(0L, len); Assert.Equal(0, playing); Assert.Equal(0, loop);
    }

    internal static string FindRepoFile(string relative)
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            var candidate = Path.Combine(dir.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
            dir = dir.Parent;
        }
        throw new FileNotFoundException(relative);
    }
}

/// <summary>
/// Spillover rendered OFFLINE through the live chain on a stopped engine, so the
/// tail can be read off the samples: a delay with heavy feedback is fed a burst
/// and REMOVED, then silence is rendered. With spillover ON the silence carries
/// the delay's tail; OFF it is exactly silent. Also: off at engine start, and
/// the tail count retires a finished tail.
/// </summary>
[Collection("NativeEngine")]
public sealed class SpilloverOfflineTests : IDisposable
{
    private const int Rate = 48000;
    private readonly JuceAudioEngine? _engine;

    public SpilloverOfflineTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    private (float BurstPeak, float TailPeak, int Tails) RemoveARingingDelayAndListen(bool spillover)
    {
        var chain = _engine!.GetDspChain(0);
        chain.Clear();
        chain.Spillover = spillover;
        var delay = chain.AddNode("delay");
        delay.SetParam("timeMs", 120f);
        delay.SetParam("feedback", 0.85f);
        delay.SetParam("mix", 0.5f);

        // 200 ms of a 220 Hz burst, then silence: the delay line is full.
        var n = Rate / 2;
        var burst = new float[n];
        for (var i = 0; i < Rate / 5; i++) burst[i] = 0.5f * MathF.Sin(2f * MathF.PI * 220f * i / Rate);
        var outL = new float[n];
        var outR = new float[n];
        chain.ProcessOffline(burst, burst, Rate, outL, outR);
        var burstPeak = Peak(outL);

        chain.RemoveNode(delay);
        var tails = chain.SpillTailCount;

        var silence = new float[n];
        chain.ProcessOffline(silence, silence, Rate, outL, outR);
        return (burstPeak, Peak(outL), tails);
    }

    private static float Peak(float[] x)
    {
        var p = 0f;
        foreach (var s in x) p = MathF.Max(p, MathF.Abs(s));
        return p;
    }

    [NativeAvailableFact]
    public void Spillover_IsOffAtEngineStart()
    {
        Assert.False(_engine!.GetDspChain(0).Spillover);
        Assert.Equal(0, _engine.GetDspChain(0).SpillTailCount);
    }

    [NativeAvailableFact]
    public void ARemovedDelay_RingsOn_WithSpilloverOn_AndIsSilent_WithItOff()
    {
        var on = RemoveARingingDelayAndListen(spillover: true);
        var off = RemoveARingingDelayAndListen(spillover: false);

        Assert.True(on.BurstPeak > 0.1f, $"the burst never reached the output ({on.BurstPeak})");
        Assert.Equal(1, on.Tails);
        Assert.True(on.TailPeak > 1e-3f, $"spillover ON: the removed delay was cut (tail peak {on.TailPeak})");
        Assert.Equal(0, off.Tails);
        Assert.Equal(0f, off.TailPeak);
    }

    [NativeAvailableFact]
    public void AFinishedTail_IsRetired()
    {
        var ringing = RemoveARingingDelayAndListen(spillover: true);
        Assert.Equal(1, ringing.Tails); // there IS a tail to retire
        var chain = _engine!.GetDspChain(0);
        var silence = new float[Rate];
        var outL = new float[Rate];
        var outR = new float[Rate];
        // The 8 s cap bounds it; 10 s of silence is past it whatever the decay.
        for (var s = 0; s < 10 && chain.SpillTailCount > 0; s++)
            chain.ProcessOffline(silence, silence, Rate, outL, outR);
        Assert.Equal(0, chain.SpillTailCount);
        chain.ProcessOffline(silence, silence, Rate, outL, outR);
        Assert.All(outL, v => Assert.Equal(0f, v));
    }
}

/// <summary>
/// Re-amp on the NULL device: an injected take replaces the seat's input, so the
/// capture ring the tuner reads carries it; clearing it restores the live input
/// (the null device's silence). The positional capture read follows the stream
/// gap-free without a cursor of its own.
/// </summary>
[Collection("NullDevice")]
public sealed class InputInjectionNullDeviceTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    private readonly JuceAudioEngine? _engine;

    public InputInjectionNullDeviceTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = NullDevice.CreateEngine(inputChannels: 1);
    }

    public void Dispose()
    {
        _engine?.Dispose();
        _scope.Dispose();
    }

    private bool WaitForWindow(Func<float[], bool> predicate)
    {
        var win = new float[512];
        var deadline = Environment.TickCount64 + 3000;
        while (Environment.TickCount64 < deadline)
        {
            if (_engine!.ReadInputCapture(0, win) == win.Length && predicate(win)) return true;
            Thread.Sleep(10);
        }
        return false;
    }

    [NativeAvailableFact]
    public void AnInjectedTake_ReachesTheCaptureRing_AndClearingRestoresTheInput()
    {
        var take = new float[4800];
        Array.Fill(take, 0.5f);
        _engine!.SetInputInjection(0, take, loop: true);

        Assert.True(_engine.TryGetInputInjectionStatus(0, out var st));
        Assert.Equal(4800, st.LengthFrames);
        Assert.False(st.IsPlaying);
        Assert.True(st.IsLooping);

        Assert.True(_engine.SetInputInjectionPlaying(0, true));
        Assert.True(WaitForWindow(w => Array.TrueForAll(w, s => MathF.Abs(s - 0.5f) < 1e-5f)),
            "the capture ring never carried the injected take");

        // Loop: after more than a take's length the take is still playing.
        Thread.Sleep(250);
        Assert.True(_engine.TryGetInputInjectionStatus(0, out st));
        Assert.True(st.IsPlaying, "a looping take stopped");
        Assert.InRange(st.PositionFrames, 0, st.LengthFrames);

        Assert.True(_engine.SeekInputInjection(0, 100));
        Assert.True(_engine.SetInputInjectionLoop(0, false));

        _engine.ClearInputInjection(0);
        Assert.False(_engine.TryGetInputInjectionStatus(0, out _));
        Assert.True(WaitForWindow(w => Array.TrueForAll(w, s => s == 0f)),
            "clearing the take did not give the seat its (silent) live input back");
        Assert.False(_engine.SetInputInjectionPlaying(0, true), "play on a seat with no take succeeded");
    }

    [NativeAvailableFact]
    public void TakeOps_AreRejectedOnASeatWithNoTake()
    {
        Assert.False(_engine!.TryGetInputInjectionStatus(1, out var st));
        Assert.Equal(0, st.LengthFrames);
        Assert.False(_engine.SeekInputInjection(1, 10));
        Assert.False(_engine.SetInputInjectionLoop(1, true));
        Assert.Throws<ArgumentOutOfRangeException>(() => _engine.SetInputInjection(DspTestSeats.OutOfRange, new float[4], true));
    }

    /// <summary>A ramp take makes the stream self-describing: following it with the
    /// positional read from a caller-held cursor must yield consecutive samples,
    /// with the reported first position equal to the cursor every time.</summary>
    [NativeAvailableFact]
    public void ThePositionalRead_FollowsTheStreamWithoutGapsOrACursor()
    {
        const int len = 480 * 50;
        var take = new float[len];
        for (var i = 0; i < len; i++) take[i] = i / (float)len;
        _engine!.SetInputInjection(0, take, loop: true);
        Assert.True(_engine.SetInputInjectionPlaying(0, true));
        Assert.True(WaitForWindow(w => w[^1] > 0.01f), "the ramp never reached the ring");

        var buf = new float[4096];
        Assert.True(_engine.TryGetInputCaptureSync(0, out var cursor, out _));
        var last = float.NaN;
        var checkedSamples = 0;
        var deadline = Environment.TickCount64 + 3000;
        while (checkedSamples < 20000 && Environment.TickCount64 < deadline)
        {
            var n = _engine.ReadInputCaptureFrom(0, cursor, buf, out var first);
            Assert.Equal(cursor, first);
            for (var i = 0; i < n; i++)
            {
                if (!float.IsNaN(last))
                {
                    var step = buf[i] - last;
                    // one ramp step, or the loop wrapping back to 0
                    Assert.True(MathF.Abs(step - 1f / len) < 1e-5f || buf[i] < 1e-5f,
                        $"gap at stream position {cursor + (ulong)i}: {last} -> {buf[i]}");
                }
                last = buf[i];
            }
            cursor += (ulong)n;
            checkedSamples += n;
            Thread.Sleep(15);
        }
        Assert.True(checkedSamples >= 20000, $"only {checkedSamples} samples followed");
        _engine.ClearInputInjection(0);
    }
}

internal static class DspTestSeats
{
    public const int OutOfRange = 4;
}
