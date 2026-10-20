using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v46 (JuceSharp #29): <c>dca_engine_get_callback_load</c>, the audio callback's
/// load as a fraction of the buffer period, so a host's CPU meter shows a number the
/// engine measured. The RT side (no allocation, the reset at a device session) is
/// <c>RtCallbackLoadIsMeasuredWithoutAllocating</c> in the native gate.
/// </summary>
[Collection("NullDevice")]
public sealed class AbiV46Tests
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    private static JuceAudioEngine NullEngine(bool manual = false) => new(new JuceEngineConfig
    {
        OutputTypeName = NullDevice.TypeName,
        SampleRate = 48000,
        BufferFrames = 480,
        EnableNullDevice = true,
        NullDeviceManualPump = manual,
        EnableConsoleLog = false,
    });

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 46u, "dca_engine_get_callback_load predates the current version");
    }

    [Theory]
    [InlineData("dca_engine_get_callback_load")]
    public void TheV46Export_IsInTheHeaderAndTheDll(string export)
    {
        Assert.Contains($"DCA_CALL {export}(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr), $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    [NativeAvailableFact]
    public void ANullEngine_IsAnInvalidArgument_AndTheOutParamsAreZeroed()
    {
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineGetCallbackLoad(nint.Zero, out var average, out var peak));
        Assert.Equal(0f, average);
        Assert.Equal(0f, peak);
    }

    /// <summary>The paced Null device renders real callbacks: the meter must read a
    /// load above zero, a peak at least the average, and the same numbers to a second
    /// reader (the peak is not reset by reading).</summary>
    [NativeAvailableFact]
    public void APacedEngine_ReportsALoad_AndReadingDoesNotResetThePeak()
    {
        using var engine = NullEngine();
        var load = default(CallbackLoad);
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (!(engine.TryGetCallbackLoad(out load) && load.Average > 0f) && DateTime.UtcNow < deadline)
            Thread.Sleep(20);

        Assert.True(load.Average > 0f, $"the paced Null device rendered for up to 5 s and the average load stayed {load.Average}");
        Assert.True(load.Peak >= load.Average * 0.999f, $"peak {load.Peak} is below the average {load.Average}");
        Assert.True(load.Peak < 50f, $"a peak of {load.Peak} buffer periods is not a load, it is a unit error");
    }

    /// <summary>A manual-pump device never calls back, so the meter reads exactly 0:
    /// it reports what the callback measured, never a guess.</summary>
    [NativeAvailableFact]
    public void AnEngineWhoseDeviceNeverCallsBack_ReadsZero()
    {
        using var engine = NullEngine(manual: true);
        Thread.Sleep(100);
        Assert.True(engine.TryGetCallbackLoad(out var load));
        Assert.Equal(default, load);
    }

    [NativeAvailableFact]
    public void ADisposedEngine_ReportsFailure_NotAStaleLoad()
    {
        var engine = NullEngine();
        engine.Dispose();
        Assert.False(engine.TryGetCallbackLoad(out var load));
        Assert.Equal(default, load);
    }
}
