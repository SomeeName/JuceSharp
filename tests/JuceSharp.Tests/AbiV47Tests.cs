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
/// ABI v47: the EXTERNAL PROCESS INSERT — a seat's dry input exchanged with another
/// process through a named shared-memory ring. The contract half (version, ordinals,
/// struct layout, exports in the BUILT dll, null handles, seat-0 aliases) is here;
/// <see cref="ExternalProcessInsertNullDeviceTests" /> plays the other process. The RT
/// half (no allocation, lock or wait on the callback) is
/// <c>RtExternalProcessInsertExchangesWithoutAllocatingAndFallsBackDry</c> in the
/// native gate.
/// </summary>
public sealed class AbiV47Tests
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    public static TheoryData<string> Exports => new()
    {
        "dca_engine_extproc_open_for_player",
        "dca_engine_extproc_open",
        "dca_engine_extproc_close_for_player",
        "dca_engine_extproc_close",
        "dca_engine_extproc_get_stats_for_player",
        "dca_engine_extproc_get_stats",
    };

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 47u, "the external process insert predates the current version");
    }

    [Theory]
    [InlineData("DCA_ERR_SAMPLE_RATE", (int)NativeMethods.DcaStatus.SampleRate)]
    [InlineData("DCA_ERR_NOT_FOUND", (int)NativeMethods.DcaStatus.NotFound)]
    [InlineData("DCA_ERR_CANCELLED", (int)NativeMethods.DcaStatus.Cancelled)]
    public void TheAppendedStatusCodes_MatchTheHeader(string enumerator, int managed)
    {
        var m = Regex.Match(Header(), $@"^\s*{Regex.Escape(enumerator)}\s*=\s*(\d+)", RegexOptions.Multiline);
        Assert.True(m.Success, $"No '{enumerator} = N' in dotcharter_audio.h.");
        Assert.Equal(int.Parse(m.Groups[1].Value), managed);
    }

    [Theory]
    [InlineData("DCA_EXTPROC_STATE_CLOSED", (int)ExternalProcessInsertState.Closed)]
    [InlineData("DCA_EXTPROC_STATE_WAITING", (int)ExternalProcessInsertState.Waiting)]
    [InlineData("DCA_EXTPROC_STATE_ACTIVE", (int)ExternalProcessInsertState.Active)]
    [InlineData("DCA_EXTPROC_STATE_RATE_MISMATCH", (int)ExternalProcessInsertState.RateMismatch)]
    [InlineData("DCA_EXTPROC_STATE_BAD_LAYOUT", (int)ExternalProcessInsertState.BadLayout)]
    public void TheStateOrdinals_MatchTheHeader(string define, int managed)
    {
        var m = Regex.Match(Header(), $@"^\s*#define\s+{Regex.Escape(define)}\s+(\d+)", RegexOptions.Multiline);
        Assert.True(m.Success, $"No '#define {define}' in dotcharter_audio.h.");
        Assert.Equal(int.Parse(m.Groups[1].Value), managed);
    }

    /// <summary>8x int32 then 5x int64: no padding, 72 bytes. The header's field
    /// order is read back so a reorder on either side fails here.</summary>
    [Fact]
    public void DcaExtprocStats_LayoutMatchesTheHeader()
    {
        Assert.Equal(72, Marshal.SizeOf<NativeMethods.DcaExtprocStats>());
        Assert.Equal(0, (int)Marshal.OffsetOf<NativeMethods.DcaExtprocStats>("StructBytes"));
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeMethods.DcaExtprocStats>("LatencySamples"));
        Assert.Equal(32, (int)Marshal.OffsetOf<NativeMethods.DcaExtprocStats>("BlocksSent"));
        Assert.Equal(64, (int)Marshal.OffsetOf<NativeMethods.DcaExtprocStats>("LateBlocks"));

        var body = Regex.Match(Header(), @"typedef struct dca_extproc_stats \{(.*?)\} dca_extproc_stats;",
            RegexOptions.Singleline);
        Assert.True(body.Success, "no dca_extproc_stats in the header");
        var fields = Regex.Matches(body.Groups[1].Value, @"^\s*(int32_t|int64_t)\s+(\w+);", RegexOptions.Multiline);
        string[] managed =
        [
            "StructBytes", "State", "Connected", "SampleRate", "BlockFrames", "RingBlocks", "LatencySamples",
            "Reserved", "BlocksSent", "BlocksReceived", "Underruns", "Overruns", "LateBlocks",
        ];
        Assert.Equal(managed.Length, fields.Count);
        for (var i = 0; i < managed.Length; i++)
        {
            Assert.Equal(managed[i].ToLowerInvariant(), fields[i].Groups[2].Value.Replace("_", "", StringComparison.Ordinal));
            var expected = fields[i].Groups[1].Value == "int64_t" ? typeof(long) : typeof(int);
            Assert.Equal(expected, typeof(NativeMethods.DcaExtprocStats).GetField(managed[i])!.FieldType);
        }
    }

    [Theory]
    [MemberData(nameof(Exports))]
    public void TheV47Export_IsInTheHeaderAndTheDll(string export)
    {
        Assert.Contains($"DCA_CALL {export}(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr), $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    [NativeAvailableFact]
    public void ANullEngine_IsAnInvalidArgument_AndTheStatsAreZeroed()
    {
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineExtprocOpen(nint.Zero, "Local\\x", null));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineExtprocOpenForPlayer(nint.Zero, 0, "Local\\x", null));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineExtprocClose(nint.Zero));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineExtprocCloseForPlayer(nint.Zero, 0));

        var st = Poisoned();
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineExtprocGetStats(nint.Zero, ref st));
        Assert.Equal(0, st.State);
        Assert.Equal(0L, st.Underruns);
        st = Poisoned();
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineExtprocGetStatsForPlayer(nint.Zero, 0, ref st));
        Assert.Equal(0, st.Connected);
        Assert.Equal(0L, st.LateBlocks);

        // A struct_bytes too small to hold even the state is refused, not overrun.
        var tiny = new NativeMethods.DcaExtprocStats { StructBytes = 4 };
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineExtprocGetStats(nint.Zero, ref tiny));
    }

    private static NativeMethods.DcaExtprocStats Poisoned() => new()
    {
        StructBytes = Marshal.SizeOf<NativeMethods.DcaExtprocStats>(),
        State = 77, Connected = 77, Underruns = 77, LateBlocks = 77,
    };

    /// <summary>The seat-0 forms are one-line aliases: the guards live in the twin.</summary>
    [Theory]
    [InlineData("dca_engine_extproc_open")]
    [InlineData("dca_engine_extproc_close")]
    [InlineData("dca_engine_extproc_get_stats")]
    public void ASeatZeroExport_IsAPureAliasForItsForPlayerTwin(string export)
    {
        var cpp = File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/src/dotcharter_audio.cpp"));
        var m = Regex.Match(cpp, $@"DCA_CALL {Regex.Escape(export)}\([^)]*\)\s*\{{(.*?)\n\}}", RegexOptions.Singleline);
        Assert.True(m.Success, $"no definition of {export}");
        var body = m.Groups[1].Value;
        Assert.Contains($"return {export}_for_player(handle, 0", body, StringComparison.Ordinal);
        Assert.Equal(1, body.Split(';').Length - 1);
        Assert.DoesNotContain("e->impl.", body, StringComparison.Ordinal);
    }
}

/// <summary>
/// The shared-memory header's offsets, read from ONE place: the native
/// <c>ExternalProcessInsert.h</c>, which pins each against <c>offsetof</c> of the
/// format's own field-by-field struct. The fake other process below uses only these,
/// so it cannot agree with the engine on a layout the format does not have (a header
/// 8 bytes too long is exactly how two sides agree and are both wrong).
/// </summary>
internal static class SharedRingFormat
{
    private static readonly string Source = File.ReadAllText(
        AbiV37Tests.FindRepoFile("src/JuceSharp.Native/src/ExternalProcessInsert.h"));

    public static int Constant(string name)
    {
        var m = Regex.Match(Source, $@"inline constexpr size_t\s+{Regex.Escape(name)}\s*=\s*(\d+);");
        Assert.True(m.Success, $"no '{name}' in ExternalProcessInsert.h");
        return int.Parse(m.Groups[1].Value);
    }

    public static readonly int HeaderBytes = Constant("kHeaderBytes");
    public static readonly int Version = Constant("kOffVersion");
    public static readonly int SampleRate = Constant("kOffSampleRate");
    public static readonly int BlockFrames = Constant("kOffBlockFrames");
    public static readonly int RingBlocks = Constant("kOffRingBlocks");
    public static readonly int InChannels = Constant("kOffInChannels");
    public static readonly int OutChannels = Constant("kOffOutChannels");
    public static readonly int InWrite = Constant("kOffInWrite");
    public static readonly int InRead = Constant("kOffInRead");
    public static readonly int OutWrite = Constant("kOffOutWrite");
    public static readonly int OutRead = Constant("kOffOutRead");
    public static readonly int HostUnderruns = Constant("kOffHostUnder");
    public static readonly int HostOverruns = Constant("kOffHostOver");

    public static int Align64(int v) => (v + 63) & ~63;

    public static int InOffset => Align64(HeaderBytes);

    public static int OutOffset(int ringBlocks, int blockFrames) => Align64(InOffset + (ringBlocks * blockFrames * sizeof(float)));
}

/// <summary>The format's header as the protocol declares it (8-byte magic, then fourteen
/// uint32 in order). A third, independent statement of the layout.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FormatHeader
{
    public fixed byte Magic[8];
    public uint Version, SampleRate, BlockFrames, RingBlocks, InChannels, OutChannels;
    public uint InWrite, InRead, OutWrite, OutRead;
    public uint OtherUnderruns, HostUnderruns, HostOverruns, OtherOverruns;
}

public sealed class SharedRingFormatTests
{
    [Fact]
    public void TheNativeOffsets_AreTheFormatsOwnLayout()
    {
        Assert.Equal(64, SharedRingFormat.HeaderBytes);
        Assert.Equal(64, Marshal.SizeOf<FormatHeader>());
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("Version"), SharedRingFormat.Version);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("SampleRate"), SharedRingFormat.SampleRate);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("BlockFrames"), SharedRingFormat.BlockFrames);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("RingBlocks"), SharedRingFormat.RingBlocks);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("InChannels"), SharedRingFormat.InChannels);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("OutChannels"), SharedRingFormat.OutChannels);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("InWrite"), SharedRingFormat.InWrite);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("InRead"), SharedRingFormat.InRead);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("OutWrite"), SharedRingFormat.OutWrite);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("OutRead"), SharedRingFormat.OutRead);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("HostUnderruns"), SharedRingFormat.HostUnderruns);
        Assert.Equal((int)Marshal.OffsetOf<FormatHeader>("HostOverruns"), SharedRingFormat.HostOverruns);
        // The rings: in right after the header, out after ringBlocks * blockFrames mono floats, both 64-aligned.
        Assert.Equal(64, SharedRingFormat.InOffset);
        Assert.Equal(64 + (32 * 128 * 4), SharedRingFormat.OutOffset(32, 128));
    }
}

/// <summary>
/// The behaviour, end to end through the C ABI: a paced Null-device engine whose
/// seat 0 plays an injected sine, and a FAKE OTHER PROCESS — this test, creating the
/// named block and echoing every input block back x 0.25 from its own thread. The
/// processed audio must replace the seat's output (its post-chain peak drops to a
/// quarter), the counters must move, and when the other side stops the seat must
/// fall back to its DRY input (the peak comes back) with the underruns counted.
/// </summary>
[Collection("NullDevice")]
public sealed unsafe class ExternalProcessInsertNullDeviceTests : IDisposable
{
    private const int Ring = 32;
    private const int Block = 128;
    private const float Gain = 0.25f;
    private const string Magic = "DCXPTEST";

    private readonly JuceAudioEngine? _engine;
    private readonly string _name = $"Local\\dc-juce-tests-extproc-{Environment.ProcessId}-{Guid.NewGuid():N}";
    private readonly MemoryMappedFile _file;
    private readonly MemoryMappedViewAccessor _view;
    private readonly byte* _base;
    private Thread? _echo;
    private volatile bool _stopEcho;

    public ExternalProcessInsertNullDeviceTests()
    {
        var bytes = SharedRingFormat.HeaderBytes + (Ring * 1024 * 3 * sizeof(float));
        _file = MemoryMappedFile.CreateNew(_name, bytes);
        _view = _file.CreateViewAccessor(0, bytes);
        byte* p = null;
        _view.SafeMemoryMappedViewHandle.AcquirePointer(ref p);
        _base = p + _view.PointerOffset;
        new Span<byte>(_base, bytes).Clear();
        for (var i = 0; i < 8; i++) _base[i] = (byte)Magic[i];
        U32(SharedRingFormat.Version) = 1;
        U32(SharedRingFormat.SampleRate) = 48000;
        U32(SharedRingFormat.BlockFrames) = 0; // not published yet
        U32(SharedRingFormat.RingBlocks) = Ring;
        U32(SharedRingFormat.InChannels) = 1;
        U32(SharedRingFormat.OutChannels) = 2;

        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine(new JuceEngineConfig
        {
            OutputTypeName = NullDevice.TypeName,
            SampleRate = 48000,
            BufferFrames = 480,
            InputChannels = 1,
            EnableNullDevice = true,
            EnableConsoleLog = false,
        });
        var take = new float[4800]; // 100 Hz, ten whole cycles: loops seamlessly
        for (var i = 0; i < take.Length; i++) take[i] = 0.5f * MathF.Sin(2f * MathF.PI * 100f * i / 48000f);
        _engine.SetInputInjection(0, take, loop: true);
        _engine.SetInputInjectionPlaying(0, true);
    }

    public void Dispose()
    {
        StopEcho();
        _engine?.Dispose();
        _view.SafeMemoryMappedViewHandle.ReleasePointer();
        _view.Dispose();
        _file.Dispose();
    }

    private ref uint U32(int offset) => ref *(uint*)(_base + offset);

    private void StartEcho()
    {
        _stopEcho = false;
        _echo = new Thread(EchoLoop) { IsBackground = true, Priority = ThreadPriority.Highest, Name = "fake other process" };
        _echo.Start();
    }

    private void StopEcho()
    {
        _stopEcho = true;
        _echo?.Join();
        _echo = null;
    }

    /// <summary>What the other process does: every unread input block, x <see cref="Gain" />, out.</summary>
    private void EchoLoop()
    {
        var b = (int)Volatile.Read(ref U32(SharedRingFormat.BlockFrames));
        var inRing = (float*)(_base + SharedRingFormat.InOffset);
        var outRing = (float*)(_base + SharedRingFormat.OutOffset(Ring, b));
        while (!_stopEcho)
        {
            var inWrite = Volatile.Read(ref U32(SharedRingFormat.InWrite));
            var inRead = U32(SharedRingFormat.InRead);
            var outWrite = U32(SharedRingFormat.OutWrite);
            var outRead = Volatile.Read(ref U32(SharedRingFormat.OutRead));
            if (inRead == inWrite || outWrite - outRead >= Ring)
            {
                Thread.Yield();
                continue;
            }

            var src = inRing + ((inRead % Ring) * b);
            var dst = outRing + ((outWrite % Ring) * b * 2);
            for (var j = 0; j < b; j++) dst[2 * j] = dst[(2 * j) + 1] = src[j] * Gain;
            Volatile.Write(ref U32(SharedRingFormat.InRead), inRead + 1);
            Volatile.Write(ref U32(SharedRingFormat.OutWrite), outWrite + 1);
        }
    }

    private ExternalProcessInsertStats Stats()
    {
        Assert.True(_engine!.TryGetExternalProcessInsertStats(0, out var st));
        return st;
    }

    private bool WaitFor(Func<ExternalProcessInsertStats, bool> predicate, int ms = 5000)
    {
        var deadline = Environment.TickCount64 + ms;
        while (Environment.TickCount64 < deadline)
        {
            if (predicate(Stats())) return true;
            Thread.Sleep(10);
        }
        return false;
    }

    /// <summary>The seat's post-chain peak over a window, and whether any underrun
    /// fell inside it (a window with an underrun mixes dry blocks in).</summary>
    private (float Peak, bool Clean) PeakWindow(int ms = 150)
    {
        var before = Stats().Underruns;
        _engine!.GetAndResetPlayerInputPeak(0);
        Thread.Sleep(ms);
        var peak = _engine.GetAndResetPlayerInputPeak(0);
        return (peak, Stats().Underruns == before);
    }

    [NativeAvailableFact]
    public void TheOtherProcessesAudio_ReplacesTheSeat_AndAnUnderrunFallsBackDry()
    {
        Thread.Sleep(200);
        var (dry, _) = PeakWindow();
        Assert.InRange(dry, 0.45f, 0.55f);
        Assert.Equal(ExternalProcessInsertState.Closed, Stats().State);

        Assert.Equal(ExternalProcessInsertOpenResult.Opened, _engine!.OpenExternalProcessInsert(0, _name, Magic));
        Assert.True(WaitFor(s => s.State == ExternalProcessInsertState.Waiting), $"a block with no size published is not Waiting ({Stats().State})");
        var (waiting, _) = PeakWindow();
        Assert.InRange(waiting, 0.45f, 0.55f); // waiting = dry

        // The other side publishes its block and starts answering.
        Volatile.Write(ref U32(SharedRingFormat.BlockFrames), Block);
        StartEcho();
        Assert.True(WaitFor(s => s.Connected && s.BlocksReceived > 50), $"never connected: {Stats()}");
        var st = Stats();
        Assert.Equal(ExternalProcessInsertState.Active, st.State);
        Assert.Equal(Block, st.BlockFrames);
        Assert.Equal(Ring, st.RingBlocks);
        Assert.Equal(48000, st.SampleRate);
        Assert.InRange(st.LatencySamples, Block, 16 * Block);

        // The seat now plays the other process's audio (x 0.25), not its own.
        var processed = 0f;
        var clean = false;
        for (var attempt = 0; attempt < 10 && !clean; attempt++) (processed, clean) = PeakWindow();
        Assert.True(clean, "no window without an underrun in ten tries");
        Assert.InRange(processed / dry, 0.2f, 0.3f);

        // The other side stops: the seat plays DRY (never silence) and counts it.
        StopEcho();
        var underrunsBefore = st.Underruns;
        Assert.True(WaitFor(s => !s.Connected && s.Underruns > underrunsBefore + 20), $"no underruns counted: {Stats()}");
        var (fallback, _) = PeakWindow();
        Assert.InRange(fallback / dry, 0.9f, 1.1f);
        Assert.True(Stats().Overruns > 0, "a full input ring was not counted");
        Assert.True(Volatile.Read(ref U32(SharedRingFormat.HostUnderruns)) > 0, "the header does not mirror the host's underruns for the other side");

        Assert.True(_engine.CloseExternalProcessInsert(0));
        Assert.Equal(ExternalProcessInsertState.Closed, Stats().State);
    }

    [NativeAvailableFact]
    public void TheRefusals_SayWhy_AndOpenNothing()
    {
        Assert.Equal(ExternalProcessInsertOpenResult.NotFound,
            _engine!.OpenExternalProcessInsert(0, $"Local\\dc-juce-tests-no-such-block-{Guid.NewGuid():N}"));
        Assert.Equal(ExternalProcessInsertOpenResult.BadHeader, _engine.OpenExternalProcessInsert(0, _name, "OTHERMAG"));

        U32(SharedRingFormat.SampleRate) = 44100;
        Assert.Equal(ExternalProcessInsertOpenResult.SampleRateMismatch, _engine.OpenExternalProcessInsert(0, _name, Magic));
        U32(SharedRingFormat.SampleRate) = 48000;
        U32(SharedRingFormat.Version) = 2;
        Assert.Equal(ExternalProcessInsertOpenResult.BadHeader, _engine.OpenExternalProcessInsert(0, _name, Magic));
        U32(SharedRingFormat.Version) = 1;
        Assert.Equal(ExternalProcessInsertState.Closed, Stats().State);

        // The seat-0 alias opens seat 0; another seat stays closed.
        Assert.Equal(NativeMethods.DcaStatus.Ok, OpenSeatZeroThroughTheAlias());
        Assert.Equal(ExternalProcessInsertState.Waiting, Stats().State);
        Assert.True(_engine.TryGetExternalProcessInsertStats(1, out var seat1));
        Assert.Equal(ExternalProcessInsertState.Closed, seat1.State);
        Assert.Throws<ArgumentOutOfRangeException>(() => _engine.OpenExternalProcessInsert(NativeMethods.MaxPlayers, _name, Magic));
        Assert.True(_engine.CloseExternalProcessInsert(0));
        Assert.Equal(ExternalProcessInsertState.Closed, Stats().State);
    }

    private NativeMethods.DcaStatus OpenSeatZeroThroughTheAlias()
    {
        var h = _engine!.Acquire();
        try { return NativeMethods.EngineExtprocOpen(h, _name, Magic); }
        finally { _engine.Release(); }
    }

    [NativeAvailableFact]
    public void ADisposedEngine_ReportsFailure()
    {
        _engine!.Dispose();
        Assert.Equal(ExternalProcessInsertOpenResult.Failed, _engine.OpenExternalProcessInsert(0, _name, Magic));
        Assert.False(_engine.CloseExternalProcessInsert(0));
        Assert.False(_engine.TryGetExternalProcessInsertStats(0, out var st));
        Assert.Equal(default, st);
    }
}
