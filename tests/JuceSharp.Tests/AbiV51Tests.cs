using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v51: seat routing for the chart sound (DotCharter #751). A chart source and
/// the in-process instrument each enter the seats a MASK names
/// (<c>dca_engine_chart_source_set_route_mask_for_player</c>,
/// <c>dca_engine_set_instrument_route_mask</c> and their getters). What reaches
/// which rig, allocation-free, is native-only and pinned by the RT gate
/// (<c>RtSeatRoutingMasks</c>); this class pins the contract the host sees.
/// </summary>
[Collection("NullDevice")]
public sealed class AbiV51Tests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header does not define DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 51u, "seat routing predates the current version");
    }

    [Theory]
    [InlineData("dca_engine_set_instrument_route_mask")]
    [InlineData("dca_engine_get_instrument_route_mask")]
    [InlineData("dca_engine_chart_source_set_route_mask_for_player")]
    [InlineData("dca_engine_chart_source_get_route_mask_for_player")]
    public void TheV51Export_IsInTheHeaderAndTheDll(string export)
    {
        Assert.Contains($"DCA_CALL {export}(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        try
        {
            Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr), $"{export} is not exported by the built juce_sharp.dll.");
            Assert.NotEqual(nint.Zero, addr);
        }
        finally { NativeLibrary.Free(lib); }
    }

    [NativeAvailableFact]
    public void ANullEngine_IsAnInvalidArgument_AndTheOutMaskIsZeroed()
    {
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineSetInstrumentRouteMask(nint.Zero, 1u));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineGetInstrumentRouteMask(nint.Zero, out var i));
        Assert.Equal(0u, i);
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineChartSourceSetRouteMaskForPlayer(nint.Zero, 0, 1u));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineChartSourceGetRouteMaskForPlayer(nint.Zero, 0, out var c));
        Assert.Equal(0u, c);
    }

    /// <summary>
    /// The engine starts on v50's routing (each source in its own seat, the
    /// instrument after the master), keeps a mask as written, refuses a bit or a
    /// player past the seats, and the v38 one-seat route is a view of the mask.
    /// </summary>
    [NativeAvailableFact]
    public void TheMasks_StartOnTheV50Routing_RoundTrip_AndRefuseSeatsThatDoNotExist()
    {
        using var engine = NullDevice.CreateEngine();
        Assert.Equal(0u, engine.GetInstrumentRouteMask());
        for (var p = 0; p < JuceAudioEngine.MaxPlayers; p++)
            Assert.Equal(1u << p, engine.GetChartRouteMask(p));

        Assert.True(engine.TrySetChartRouteMask(0, 0b11u));
        Assert.Equal(0b11u, engine.GetChartRouteMask(0));
        Assert.True(engine.TrySetChartRouteMask(0, 0u));
        Assert.Equal(0b01u, engine.GetChartRouteMask(0));   // 0 = its own seat

        Assert.True(engine.TrySetInstrumentRouteMask(0b10u));
        Assert.Equal(0b10u, engine.GetInstrumentRouteMask());
        Assert.Equal(1, engine.GetInstrumentRoute());
        engine.SetInstrumentRoute(-1);
        Assert.Equal(0u, engine.GetInstrumentRouteMask());
        engine.SetInstrumentRoute(0);
        Assert.Equal(0b01u, engine.GetInstrumentRouteMask());

        var past = 1u << JuceAudioEngine.MaxPlayers;
        Assert.False(engine.TrySetInstrumentRouteMask(past));
        Assert.False(engine.TrySetChartRouteMask(0, past));
        Assert.False(engine.TrySetChartRouteMask(-1, 1u));
        Assert.False(engine.TrySetChartRouteMask(JuceAudioEngine.MaxPlayers, 1u));
        Assert.Equal(0u, engine.GetChartRouteMask(JuceAudioEngine.MaxPlayers));
        Assert.Equal(0b01u, engine.GetInstrumentRouteMask());   // a refused write changes nothing
    }

    /// <summary>A rebuilt engine starts on the defaults again: the host replays.</summary>
    [NativeAvailableFact]
    public void AFreshEngine_ForgetsTheMasks()
    {
        using (var first = NullDevice.CreateEngine())
        {
            Assert.True(first.TrySetChartRouteMask(0, 0b10u));
            Assert.True(first.TrySetInstrumentRouteMask(0b11u));
        }
        using var second = NullDevice.CreateEngine();
        Assert.Equal(0b01u, second.GetChartRouteMask(0));
        Assert.Equal(0u, second.GetInstrumentRouteMask());
    }
}
