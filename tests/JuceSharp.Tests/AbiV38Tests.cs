using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using JuceSharp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v38: the CHART SOURCE (a per-seat note generator rendered into the seat's
/// rig input after its capture ring), the SONG CLOCK it follows and the
/// instrument ROUTE. Nine additive exports and three structs; everything off /
/// empty at engine start.
/// </summary>
public sealed class AbiV38Tests
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 38u, "the v38 exports predate the current version");
    }

    [Theory]
    [InlineData("dca_engine_set_song_clock_source")]
    [InlineData("dca_engine_chart_source_set_notes_for_player")]
    [InlineData("dca_engine_chart_source_set_enabled_for_player")]
    [InlineData("dca_engine_chart_source_set_voice_for_player")]
    [InlineData("dca_engine_chart_source_set_params_for_player")]
    [InlineData("dca_engine_chart_source_get_status_for_player")]
    [InlineData("dca_chart_voice_is_available")]
    [InlineData("dca_engine_set_instrument_route")]
    [InlineData("dca_engine_get_instrument_route")]
    public void TheV38Exports_AreInTheDll(string export)
    {
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr),
            $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    [NativeAvailableFact]
    public unsafe void EveryV38Export_RejectsANullEngine()
    {
        var bad = NativeMethods.DcaStatus.InvalidArg;
        Assert.Equal(bad, NativeMethods.EngineSetSongClockSource(nint.Zero, nint.Zero));
        var note = new NativeMethods.DcaChartNote();
        Assert.Equal(bad, NativeMethods.EngineChartSourceSetNotesForPlayer(nint.Zero, 0, &note, 1,
            Unsafe.SizeOf<NativeMethods.DcaChartNote>(), null, 0));
        Assert.Equal(bad, NativeMethods.EngineChartSourceSetEnabledForPlayer(nint.Zero, 0, 1));
        Assert.Equal(bad, NativeMethods.EngineChartSourceSetVoiceForPlayer(nint.Zero, 0, 0));
        Assert.Equal(bad, NativeMethods.EngineChartSourceSetParamsForPlayer(nint.Zero, 0, 1f, 0.0, 0));
        var st = new NativeMethods.DcaChartSourceStatus { StructBytes = Unsafe.SizeOf<NativeMethods.DcaChartSourceStatus>() };
        Assert.Equal(bad, NativeMethods.EngineChartSourceGetStatusForPlayer(nint.Zero, 0, ref st));
        Assert.Equal(bad, NativeMethods.EngineSetInstrumentRoute(nint.Zero, 0));
        Assert.Equal(bad, NativeMethods.EngineGetInstrumentRoute(nint.Zero, out var route));
        Assert.Equal(-1, route);
    }

    [NativeAvailableFact]
    public void VoiceAvailability_FollowsTheBuild_AndRejectsAnUnknownKind()
    {
        Assert.Equal(1, NativeMethods.ChartVoiceIsAvailable((int)JuceChartVoice.Placeholder));
        Assert.True(JuceAudioEngine.IsChartVoiceAvailable(JuceChartVoice.Placeholder));
        Assert.Equal(-1, NativeMethods.ChartVoiceIsAvailable(99));
        Assert.Equal(-1, NativeMethods.ChartVoiceIsAvailable(-1));
    }

    /// <summary>The three structs cross the boundary by value: size and every
    /// offset the header relies on are pinned, and the PUBLIC mirrors are
    /// byte-identical to the internal ones (the wrapper reinterprets the span).</summary>
    [Fact]
    public void TheStructLayouts_MatchTheHeader()
    {
        Assert.Equal(80, Unsafe.SizeOf<NativeMethods.DcaChartNote>());
        Assert.Equal(80, Marshal.SizeOf<NativeMethods.DcaChartNote>());
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeMethods.DcaChartNote>(nameof(NativeMethods.DcaChartNote.Pitch)));
        Assert.Equal(44, (int)Marshal.OffsetOf<NativeMethods.DcaChartNote>(nameof(NativeMethods.DcaChartNote.Flags)));
        Assert.Equal(60, (int)Marshal.OffsetOf<NativeMethods.DcaChartNote>(nameof(NativeMethods.DcaChartNote.Strum)));
        Assert.Equal(16, Unsafe.SizeOf<NativeMethods.DcaChartBendPoint>());
        // v38 shipped 80 bytes; v40 APPENDED six fields (cbSize contract): 112.
        Assert.Equal(112, Unsafe.SizeOf<NativeMethods.DcaChartSourceStatus>());
        Assert.Equal(32, (int)Marshal.OffsetOf<NativeMethods.DcaChartSourceStatus>(nameof(NativeMethods.DcaChartSourceStatus.NotesStarted)));
        Assert.Equal(56, (int)Marshal.OffsetOf<NativeMethods.DcaChartSourceStatus>(nameof(NativeMethods.DcaChartSourceStatus.SongSeconds)));
        Assert.Equal(72, (int)Marshal.OffsetOf<NativeMethods.DcaChartSourceStatus>(nameof(NativeMethods.DcaChartSourceStatus.OffsetSeconds)));

        Assert.Equal(Unsafe.SizeOf<NativeMethods.DcaChartNote>(), Unsafe.SizeOf<JuceChartNote>());
        Assert.Equal(Unsafe.SizeOf<NativeMethods.DcaChartBendPoint>(), Unsafe.SizeOf<JuceChartBendPoint>());
        foreach (var f in typeof(NativeMethods.DcaChartNote).GetFields())
            Assert.Equal(Marshal.OffsetOf<NativeMethods.DcaChartNote>(f.Name), Marshal.OffsetOf<JuceChartNote>(f.Name));

        var v38 = Regex.Match(Header(), @"#define\s+DCA_CHART_NOTE_V38_BYTES\s+(\d+)");
        Assert.True(v38.Success);
        Assert.Equal(80, int.Parse(v38.Groups[1].Value));
    }

    /// <summary>Every <c>DCA_CHART_NOTE_*</c> bit, voice, mode and strum ordinal in
    /// the header has its managed twin under the same name — read off the header,
    /// so a bit added on one side only fails here.</summary>
    [Fact]
    public void TheFlagBitsAndOrdinals_MatchTheHeader()
    {
        var header = Header();
        var bits = Regex.Matches(header, @"#define\s+DCA_CHART_NOTE_([A-Z_]+)\s+\(1u\s*<<\s*(\d+)\)");
        Assert.True(bits.Count >= 19, $"only {bits.Count} DCA_CHART_NOTE_* bits found in the header");
        foreach (Match b in bits)
        {
            var name = Pascal(b.Groups[1].Value);
            Assert.True(Enum.TryParse<JuceChartNoteFlags>(name, out var flag), $"JuceChartNoteFlags has no {name}");
            Assert.Equal(1u << int.Parse(b.Groups[2].Value), (uint)flag);
        }
        Assert.Equal(bits.Count, Enum.GetValues<JuceChartNoteFlags>().Length - 1); // minus None

        AssertOrdinals<JuceChartVoice>(header, "DCA_CHART_VOICE_");
        AssertOrdinals<JuceChartSourceMode>(header, "DCA_CHART_SOURCE_");
        AssertOrdinals<JuceChartStrum>(header, "DCA_CHART_STRUM_");
    }

    private static void AssertOrdinals<T>(string header, string prefix) where T : struct, Enum
    {
        var defs = Regex.Matches(header, @"#define\s+" + prefix + @"([A-Z]+)\s+(\d+)\b");
        Assert.True(defs.Count >= 2, $"no {prefix}* ordinals in the header");
        foreach (Match d in defs)
        {
            var name = Pascal(d.Groups[1].Value);
            Assert.True(Enum.TryParse<T>(name, out var v), $"{typeof(T).Name} has no {name}");
            Assert.Equal(int.Parse(d.Groups[2].Value), Convert.ToInt32(v));
        }
        Assert.Equal(defs.Count, Enum.GetValues<T>().Length);
    }

    private static string Pascal(string upperSnake)
    {
        var sb = new StringBuilder();
        foreach (var part in upperSnake.Split('_', StringSplitOptions.RemoveEmptyEntries))
            sb.Append(part[0]).Append(part[1..].ToLowerInvariant());
        return sb.ToString();
    }
}

/// <summary>
/// The chart source on the NULL device, end to end through the wrapper: a playing
/// song clock drives a few notes into seat 0's rig, the seat's post-rig meter sees
/// them, and the seat's CAPTURE RING — what live detection, scoring and the tuner
/// read — stays exactly silent. Plus the contract edges: a rejected list keeps the
/// previous table, a destroyed clock source unbinds, values out of range throw.
/// </summary>
[Collection("NullDevice")]
public sealed class ChartSourceNullDeviceTests : IDisposable
{
    private const int Rate = 48000;
    private readonly NullDeviceScope _scope = new();
    private readonly JuceAudioEngine? _engine;

    public ChartSourceNullDeviceTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = NullDevice.CreateEngine(inputChannels: 1);
    }

    public void Dispose()
    {
        _engine?.Dispose();
        _scope.Dispose();
    }

    internal static JuceChartNote[] Notes(int count, double first = 0.15, double every = 0.12)
    {
        var notes = new JuceChartNote[count];
        for (var i = 0; i < count; i++)
        {
            notes[i] = new JuceChartNote
            {
                StartSeconds = first + every * i,
                ReleaseSeconds = first + every * i + 0.1,
                Pitch = 45f + i % 7,
                Velocity = 0.9f,
                StringIndex = i % 6,
                Fret = 2,
                SlideToFret = -1f,
                ChordId = -1,
                BendFirst = -1,
            };
        }
        return notes;
    }

    /// <summary>A 16-bit mono WAV of silence: the song's CLOCK is what matters.</summary>
    internal static byte[] SilentWav(double seconds)
    {
        var frames = (int)(seconds * Rate);
        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);
        w.Write("RIFF"u8); w.Write(36 + frames * 2); w.Write("WAVE"u8);
        w.Write("fmt "u8); w.Write(16); w.Write((short)1); w.Write((short)1); w.Write(Rate); w.Write(Rate * 2);
        w.Write((short)2); w.Write((short)16);
        w.Write("data"u8); w.Write(frames * 2); w.Write(new byte[frames * 2]);
        w.Flush();
        return ms.ToArray();
    }

    [NativeAvailableFact]
    public void IsOffAndEmptyAtEngineStart()
    {
        Assert.True(_engine!.TryGetChartSourceStatus(0, out var st));
        Assert.False(st.IsEnabled);
        Assert.Equal(0, st.NoteCount);
        Assert.False(st.IsClockBound);
        Assert.Equal(JuceChartVoice.Placeholder, st.Voice);
        Assert.Equal(1f, st.Gain);
        Assert.Equal(-1, _engine.GetInstrumentRoute());
    }

    [NativeAvailableFact]
    public void PlayingNotes_ReachTheSeatRig_AndNeverTheCaptureRing()
    {
        using var song = _engine!.OpenMemory(SilentWav(6.0), "wav");
        _engine.SetSongClockSource(song);
        _engine.SetChartNotes(0, Notes(30), ReadOnlySpan<JuceChartBendPoint>.Empty);
        _engine.SetChartSourceEnabled(0, true);
        _engine.GetAndResetPlayerInputPeak(0);
        var ring = new float[8192];
        while (_engine.ReadInputCaptureStream(0, ring, out _) > 0) { }   // drain what came before
        song.Play();

        var rigPeak = 0f;
        var ringPeak = 0f;
        long ringSamples = 0;
        var deadline = Environment.TickCount64 + 2500;
        while (Environment.TickCount64 < deadline)
        {
            rigPeak = MathF.Max(rigPeak, _engine.GetAndResetPlayerInputPeak(0));
            int n;
            while ((n = _engine.ReadInputCaptureStream(0, ring, out _)) > 0)
            {
                ringSamples += n;
                for (var i = 0; i < n; i++) ringPeak = MathF.Max(ringPeak, MathF.Abs(ring[i]));
            }
            Thread.Sleep(20);
        }

        Assert.True(_engine.TryGetChartSourceStatus(0, out var st));
        Assert.True(st.IsEnabled && st.IsClockBound, "the source is not enabled / bound");
        Assert.True(st.NotesStarted >= 5, $"only {st.NotesStarted} notes started in 2.5 s of play");
        Assert.True(rigPeak > 0.02f, $"the seat's rig never carried the chart source (post-rig peak {rigPeak})");
        Assert.True(ringSamples > Rate, $"the capture ring delivered only {ringSamples} samples — the isolation check would be vacuous");
        Assert.Equal(0f, ringPeak);

        song.Pause();
        _engine.SetChartSourceEnabled(0, false);
        Assert.True(_engine.TryGetChartSourceStatus(0, out st));
        Assert.False(st.IsEnabled);
    }

    [NativeAvailableFact]
    public void ARejectedList_KeepsThePreviousTable()
    {
        _engine!.SetChartNotes(0, Notes(4), ReadOnlySpan<JuceChartBendPoint>.Empty);
        var unsorted = Notes(3);
        (unsorted[0], unsorted[2]) = (unsorted[2], unsorted[0]);
        Assert.Throws<JuceAudioException>(() => _engine.SetChartNotes(0, unsorted, ReadOnlySpan<JuceChartBendPoint>.Empty));
        var badBend = Notes(1);
        badBend[0].BendFirst = 0;
        badBend[0].BendCount = 2;
        Assert.Throws<JuceAudioException>(() => _engine.SetChartNotes(0, badBend, new JuceChartBendPoint[1]));
        Assert.True(_engine.TryGetChartSourceStatus(0, out var st));
        Assert.Equal(4, st.NoteCount);
        _engine.SetChartNotes(0, ReadOnlySpan<JuceChartNote>.Empty, ReadOnlySpan<JuceChartBendPoint>.Empty);
        Assert.True(_engine.TryGetChartSourceStatus(0, out st));
        Assert.Equal(0, st.NoteCount);
    }

    [NativeAvailableFact]
    public void ADisposedClockSource_UnbindsItself()
    {
        var song = _engine!.OpenMemory(SilentWav(1.0), "wav");
        _engine.SetSongClockSource(song);
        Assert.True(_engine.TryGetChartSourceStatus(0, out var st));
        Assert.True(st.IsClockBound);
        song.Dispose();
        Assert.True(_engine.TryGetChartSourceStatus(0, out st));
        Assert.False(st.IsClockBound, "a disposed source is still the song clock");
    }

    [NativeAvailableFact]
    public void ParamsVoiceAndRoute_AreValidatedAndReadBack()
    {
        _engine!.SetChartSourceParams(1, 0.5f, 0.25, JuceChartSourceMode.Replace);
        Assert.True(_engine.TryGetChartSourceStatus(1, out var st));
        Assert.Equal(0.5f, st.Gain);
        Assert.Equal(0.25, st.OffsetSeconds);
        Assert.Equal(JuceChartSourceMode.Replace, st.Mode);
        Assert.Throws<ArgumentOutOfRangeException>(() => _engine.SetChartSourceParams(1, 5f, 0, JuceChartSourceMode.Mix));
        Assert.Throws<ArgumentOutOfRangeException>(() => _engine.SetChartSourceParams(1, 1f, 3, JuceChartSourceMode.Mix));
        Assert.Throws<ArgumentOutOfRangeException>(() => _engine.SetChartNotes(4, Notes(1), default));

        Assert.True(_engine.TrySetChartVoice(1, JuceChartVoice.Placeholder));
        Assert.Equal(JuceAudioEngine.IsChartVoiceAvailable(JuceChartVoice.Guitar),
            _engine.TrySetChartVoice(1, JuceChartVoice.Guitar));

        _engine.SetInstrumentRoute(1);
        Assert.Equal(1, _engine.GetInstrumentRoute());
        _engine.SetInstrumentRoute(-1);
        Assert.Equal(-1, _engine.GetInstrumentRoute());
        Assert.Throws<ArgumentOutOfRangeException>(() => _engine.SetInstrumentRoute(4));
    }
}
