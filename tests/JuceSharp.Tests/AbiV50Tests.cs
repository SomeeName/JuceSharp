using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v50: one sandbox child per SEAT (DotCharter #696). Every seat owns its own
/// bridge (mapping, events, child), armed and read through
/// <c>dca_engine_set_remote_bridge_for_player</c> and
/// <c>dca_engine_get_remote_bridge_stats_for_player</c>; the v19 pair is seat 0's.
/// Which seat's requests reach which child, and the per-seat scheduling, are
/// native-only and pinned by the RT gate (<c>RtSandboxEachSeatOwnsItsBridge</c> and
/// the <c>RtSandbox*</c> overload scenarios).
/// </summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows")] // named shm/events — like the bridge itself
[Collection("NullDevice")]
public sealed class AbiV50Tests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    // dcvb layout (bridge version 4): the mapping size and the child-published seat mask.
    private const int TotalBytes = 197712;
    private const int OffSeatChainMask = 197708;

    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header does not define DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 50u, "the per-seat sandbox bridge predates the current version");
    }

    [Theory]
    [InlineData("dca_engine_set_remote_bridge_for_player")]
    [InlineData("dca_engine_get_remote_bridge_stats_for_player")]
    public void TheV50Export_IsInTheHeaderAndTheDll(string export)
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
    public void ANullEngineOrAPlayerOutOfRange_IsAnInvalidArgument_AndTheCountersAreZeroed()
    {
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineSetRemoteBridgeForPlayer(nint.Zero, 0, "a", "b", "c", 1));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineGetRemoteBridgeStatsForPlayer(nint.Zero, 0, out var p, out var t, out var hb));
        Assert.Equal((0u, 0u, 0u), (p, t, hb));

        using var engine = NullDevice.CreateEngine();
        foreach (var player in new[] { -1, JuceAudioEngine.MaxPlayers })
        {
            Assert.False(engine.SetRemoteBridge(player, "dcvb_v50_bad_shm", "dcvb_v50_bad_req", "dcvb_v50_bad_resp", enable: true),
                $"player {player} was accepted");
            Assert.False(engine.TryGetRemoteBridgeStats(player, out p, out t, out hb), $"player {player} read counters");
            Assert.Equal((0u, 0u, 0u), (p, t, hb));
        }
    }

    /// <summary>
    /// The v19 pair is seat 0's bridge and nobody else's. No child answers here,
    /// so every round trip an armed seat makes is counted as a timeout on THAT
    /// seat's bridge: seat 0 armed through the v19 export counts on player 0 and
    /// reads the same through both getters while player 1 stays at zero; seat 1,
    /// armed through its own export (its mapping published a seat-1 chain the
    /// way its child would), counts on player 1; and the v19 disarm stops seat 0
    /// only.
    /// </summary>
    [NativeAvailableFact]
    public void TheV19Pair_IsSeatZerosBridge_AndEachSeatCountsItsOwnTrips()
    {
        using var engine = NullDevice.CreateEngine(inputChannels: 2);
        engine.SetPlayerCount(2);
        var suffix = Guid.NewGuid().ToString("N");
        string Name(int seat, string what) => $"dcvb_v50_{what}{seat}_{suffix}";

        using var seat1Mapping = MemoryMappedFile.CreateOrOpen(Name(1, "shm"), TotalBytes);
        using var seat1View = seat1Mapping.CreateViewAccessor(0, TotalBytes);
        try
        {
            Assert.True(engine.SetRemoteBridge(Name(0, "shm"), Name(0, "req"), Name(0, "resp"), enable: true),
                "arming seat 0 through the v19 export failed");
            Assert.True(SpinUntil(() => Timeouts(engine, 0) >= 3), "seat 0's bridge never counted a trip");

            Assert.True(engine.TryGetRemoteBridgeStats(out var p19, out var t19, out _));
            Assert.True(engine.TryGetRemoteBridgeStats(0, out var p0, out var t0, out _));
            Assert.True(t19 > 0 && t0 > 0 && p19 == p0, $"v19 {p19}/{t19} vs player 0 {p0}/{t0}");
            Assert.Equal(0u, Timeouts(engine, 1));

            Assert.True(engine.SetRemoteBridge(1, Name(1, "shm"), Name(1, "req"), Name(1, "resp"), enable: true),
                "arming seat 1 through its own export failed");
            seat1View.Write(OffSeatChainMask, 0b10u);
            Assert.True(SpinUntil(() => Timeouts(engine, 1) >= 3), "seat 1's bridge never counted a trip of its own");

            Assert.True(engine.SetRemoteBridge(null, null, null, enable: false), "the v19 disarm failed");
            var seat0AtDisarm = Timeouts(engine, 0);
            var seat1AtDisarm = Timeouts(engine, 1);
            // Seat 1 is in poll-only by now (no child), and poll-only still posts
            // and counts every callback it is not answered.
            Assert.True(SpinUntil(() => Timeouts(engine, 1) >= seat1AtDisarm + 5),
                "the v19 disarm stopped seat 1's bridge");
            Assert.True(Timeouts(engine, 0) <= seat0AtDisarm + 1, "seat 0 kept counting trips after the v19 disarm");
        }
        finally
        {
            engine.SetRemoteBridge(0, null, null, null, enable: false);
            engine.SetRemoteBridge(1, null, null, null, enable: false);
        }
    }

    private static uint Timeouts(JuceAudioEngine engine, int player)
    {
        engine.TryGetRemoteBridgeStats(player, out _, out var timeouts, out _);
        return timeouts;
    }

    private static bool SpinUntil(Func<bool> condition)
    {
        var deadline = Environment.TickCount64 + 5000;
        while (Environment.TickCount64 < deadline)
        {
            if (condition()) return true;
            Thread.Sleep(20);
        }
        return condition();
    }
}
