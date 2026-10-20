using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading;
using JuceSharp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v40 (DotCharter #50, #61 part 2): INSTRUMENT MODELS for the built-in
/// guitar / bass voices (model, pickup selector, coil split) and the release of a
/// disabled chart source's voice and sample pack. Four additive exports, six
/// fields appended to <c>dca_chart_source_status</c>, one new struct.
/// </summary>
public sealed class AbiV40Tests
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 40u, "the instrument models predate the current version");
    }

    [Theory]
    [InlineData("dca_engine_chart_source_set_instrument_for_player")]
    [InlineData("dca_engine_chart_source_get_instrument_for_player")]
    [InlineData("dca_chart_model_get_info")]
    [InlineData("dca_chart_voice_pack_bytes")]
    public void TheV40Exports_AreInTheDll(string export)
    {
        Assert.Contains("DCA_CALL " + export + "(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr), $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    [NativeAvailableFact]
    public void TheV40Exports_RejectBadArguments()
    {
        var bad = NativeMethods.DcaStatus.InvalidArg;
        Assert.Equal(bad, NativeMethods.EngineChartSourceSetInstrumentForPlayer(nint.Zero, 0, 1, 4, 0));
        Assert.Equal(bad, NativeMethods.EngineChartSourceGetInstrumentForPlayer(nint.Zero, 0, 0, out var m, out var p, out var f));
        Assert.Equal((0, 0, 0u), (m, p, f));   // zeroed before the failure path
        var info = new NativeMethods.DcaChartModelInfo { StructBytes = Unsafe.SizeOf<NativeMethods.DcaChartModelInfo>() };
        Assert.Equal(bad, NativeMethods.ChartModelGetInfo(7, ref info));
        Assert.Equal(bad, NativeMethods.ChartModelGetInfo(-1, ref info));
        info.StructBytes = 4;
        Assert.Equal(bad, NativeMethods.ChartModelGetInfo(1, ref info));
        Assert.True(NativeMethods.ChartVoicePackBytes() >= 0);
        Assert.Null(JuceAudioEngine.GetChartModelInfo((JuceChartInstrumentModel)7));
    }

    [Fact]
    public void TheStructLayouts_MatchTheHeader()
    {
        Assert.Equal(112, Unsafe.SizeOf<NativeMethods.DcaChartSourceStatus>());
        Assert.Equal(80, (int)Marshal.OffsetOf<NativeMethods.DcaChartSourceStatus>(nameof(NativeMethods.DcaChartSourceStatus.Model)));
        Assert.Equal(92, (int)Marshal.OffsetOf<NativeMethods.DcaChartSourceStatus>(nameof(NativeMethods.DcaChartSourceStatus.Sampled)));
        Assert.Equal(96, (int)Marshal.OffsetOf<NativeMethods.DcaChartSourceStatus>(nameof(NativeMethods.DcaChartSourceStatus.PackBytes)));
        Assert.Equal(104, (int)Marshal.OffsetOf<NativeMethods.DcaChartSourceStatus>(nameof(NativeMethods.DcaChartSourceStatus.VoiceLoaded)));
        Assert.Equal(48, Unsafe.SizeOf<NativeMethods.DcaChartModelInfo>());
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeMethods.DcaChartModelInfo>(nameof(NativeMethods.DcaChartModelInfo.RecordedMask)));
        // The native side static_asserts the same two sizes.
        var cpp = File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/src/dotcharter_audio.cpp"));
        Assert.Contains("sizeof(dca_chart_source_status) == 112", cpp, StringComparison.Ordinal);
        Assert.Contains("sizeof(dca_chart_model_info) == 48", cpp, StringComparison.Ordinal);
    }

    /// <summary>Every model / pickup / flag value of the header has its managed
    /// twin with the same number (read off the header).</summary>
    [Fact]
    public void TheModelPickupAndFlagValues_MatchTheHeader()
    {
        var header = Header();
        var models = new Dictionary<string, JuceChartInstrumentModel>
        {
            ["SEMI_HOLLOW"] = JuceChartInstrumentModel.SemiHollow, ["STRAT_SSS"] = JuceChartInstrumentModel.StratSss,
            ["STRAT_HSS"] = JuceChartInstrumentModel.StratHss, ["TELE"] = JuceChartInstrumentModel.Telecaster,
            ["LES_PAUL"] = JuceChartInstrumentModel.LesPaul, ["JAZZ_BASS"] = JuceChartInstrumentModel.JazzBass,
            ["P_BASS"] = JuceChartInstrumentModel.PrecisionBass, ["PJ_BASS"] = JuceChartInstrumentModel.PjBass,
            ["HOLLOW_BASS"] = JuceChartInstrumentModel.HollowBass,
        };
        var defs = Regex.Matches(header, @"#define\s+DCA_CHART_MODEL_([A-Z_]+)\s+(\d+)\b");
        Assert.Equal(models.Count, defs.Count);
        Assert.Equal(models.Count, Enum.GetValues<JuceChartInstrumentModel>().Length);
        foreach (Match d in defs)
            Assert.Equal(int.Parse(d.Groups[2].Value), (int)models[d.Groups[1].Value]);

        var pickups = Regex.Matches(header, @"#define\s+DCA_CHART_PICKUP_([A-Z_]+)\s+(\d+)\b");
        Assert.Equal(Enum.GetValues<JuceChartPickup>().Length, pickups.Count);
        foreach (Match d in pickups)
        {
            var name = string.Concat(d.Groups[1].Value.Split('_').Select(w => w[0] + w[1..].ToLowerInvariant()));
            Assert.True(Enum.TryParse<JuceChartPickup>(name, out var v), $"JuceChartPickup has no {name}");
            Assert.Equal(int.Parse(d.Groups[2].Value), (int)v);
        }
        var split = Regex.Match(header, @"#define\s+DCA_CHART_INSTRUMENT_COIL_SPLIT\s+\(1u\s*<<\s*(\d+)\)");
        Assert.True(split.Success);
        Assert.Equal(NativeMethods.ChartInstrumentCoilSplit, 1u << int.Parse(split.Groups[1].Value));
    }

    /// <summary>What each model offers, from the native table: the selector,
    /// coil split, and which positions are REAL recordings.</summary>
    [NativeAvailableFact]
    public void EveryModel_DescribesItsSelector_AndWhatIsRecorded()
    {
        static JuceChartModelInfo Info(JuceChartInstrumentModel m) => JuceAudioEngine.GetChartModelInfo(m)!;
        JuceChartPickup[] fiveWay = [JuceChartPickup.Neck, JuceChartPickup.NeckMiddle, JuceChartPickup.Middle, JuceChartPickup.MiddleBridge, JuceChartPickup.Bridge];
        JuceChartPickup[] threeWay = [JuceChartPickup.Neck, JuceChartPickup.NeckBridge, JuceChartPickup.Bridge];

        foreach (var m in Enum.GetValues<JuceChartInstrumentModel>())
        {
            var i = Info(m);
            Assert.Equal((int)m >= 16, i.IsBass);
            Assert.Contains(i.DefaultPickup, i.Pickups);
            Assert.All(i.RecordedPickups, p => Assert.Contains(p, i.PartlyRecordedPickups));
        }
        Assert.Equal(fiveWay, Info(JuceChartInstrumentModel.StratSss).Pickups);
        Assert.Equal(fiveWay, Info(JuceChartInstrumentModel.StratSss).RecordedPickups);   // EGFxSet: every position
        Assert.Equal(fiveWay, Info(JuceChartInstrumentModel.StratHss).Pickups);
        Assert.Equal([JuceChartPickup.Neck, JuceChartPickup.NeckMiddle, JuceChartPickup.Middle], Info(JuceChartInstrumentModel.StratHss).RecordedPickups);
        Assert.Equal(threeWay, Info(JuceChartInstrumentModel.Telecaster).Pickups);
        Assert.Empty(Info(JuceChartInstrumentModel.Telecaster).RecordedPickups);
        Assert.Equal(threeWay, Info(JuceChartInstrumentModel.LesPaul).Pickups);
        Assert.Equal([JuceChartPickup.Bridge], Info(JuceChartInstrumentModel.SemiHollow).RecordedPickups);
        Assert.Equal(threeWay, Info(JuceChartInstrumentModel.JazzBass).Pickups);
        Assert.Empty(Info(JuceChartInstrumentModel.JazzBass).RecordedPickups);
        Assert.Equal([JuceChartPickup.NeckBridge], Info(JuceChartInstrumentModel.JazzBass).PartlyRecordedPickups);   // the pick
        Assert.Equal([JuceChartPickup.Middle], Info(JuceChartInstrumentModel.PrecisionBass).Pickups);
        Assert.Equal([JuceChartPickup.Middle, JuceChartPickup.MiddleBridge, JuceChartPickup.Bridge], Info(JuceChartInstrumentModel.PjBass).Pickups);

        Assert.True(Info(JuceChartInstrumentModel.StratHss).HasCoilSplit);
        Assert.True(Info(JuceChartInstrumentModel.LesPaul).HasCoilSplit);
        Assert.False(Info(JuceChartInstrumentModel.StratSss).HasCoilSplit);
        Assert.False(Info(JuceChartInstrumentModel.Telecaster).HasCoilSplit);
        Assert.False(Info(JuceChartInstrumentModel.JazzBass).HasCoilSplit);
    }
}

/// <summary>
/// Synthetic voice packs (the .dcvp format byte for byte, as
/// tools/voice/build_voice_pack.py writes it) in a temp voice directory, so the
/// engine path can be driven without the real packs (built outside git).
/// </summary>
internal static class VoicePackFixture
{
    public const string EnvVar = "DOTCHARTER_VOICE_DIR";

    public static readonly string[] Packs =
        ["strat-neck", "strat-neck-middle", "strat-middle", "strat-middle-bridge", "strat-bridge", "guitar", "bass"];

    /// <summary>Writes every pack the model table names into a fresh directory.</summary>
    public static string WriteAll(string name)
    {
        var dir = Path.Combine(Path.GetTempPath(), name);
        Directory.CreateDirectory(dir);
        foreach (var p in Packs)
            File.WriteAllBytes(Path.Combine(dir, p + ".dcvp"), Pack(p == "bass"));
        return dir;
    }

    private static byte[] Pack(bool bass)
    {
        var zones = new List<(int Variant, double Root, int String, int Fret, short[] Data)>();
        int[] open = bass ? [28, 33, 38, 43] : [40, 45, 50, 55, 59, 64];
        for (var variant = 0; variant < (bass ? 2 : 1); variant++)
            for (var s = 0; s < open.Length; s++)
                for (var f = 0; f <= 5; f++)
                {
                    var hz = 440.0 * Math.Pow(2.0, (open[s] + f - 69) / 12.0);
                    var y = new short[(int)(1.2 * 44100)];
                    for (var i = 0; i < y.Length; i++)
                    {
                        var t = i / 44100.0;
                        double v = 0;
                        for (var n = 1; n <= 8; n++) v += Math.Sin(2 * Math.PI * n * hz * t) / n;
                        y[i] = (short)Math.Round(14000 * v * Math.Exp(-t / 1.5));
                    }
                    zones.Add((variant, hz, s, f, y));
                }
        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);
        var dataOff = (64 + 40 * zones.Count + 15) & ~15;
        long total = zones.Sum(z => (long)z.Data.Length);
        long textOff = dataOff + 2 * total;
        w.Write("DCVP"u8); w.Write(1u); w.Write(bass ? 1u : 0u); w.Write((uint)zones.Count); w.Write(44100f); w.Write(0u);
        w.Write(64L); w.Write((long)dataOff); w.Write(total); w.Write(textOff); w.Write(7u); w.Write(0u);
        long start = 0;
        foreach (var z in zones)
        {
            w.Write((byte)0); w.Write((byte)z.Variant); w.Write((byte)0); w.Write((byte)0);
            w.Write((float)z.Root); w.Write(1f / 32767f); w.Write((uint)z.Data.Length); w.Write(start); w.Write(0f);
            w.Write((byte)(z.String + 1)); w.Write((byte)(z.Fret + 1)); w.Write(new byte[10]);
            start += z.Data.Length;
        }
        w.Write(new byte[dataOff - (64 + 40 * zones.Count)]);
        foreach (var z in zones) foreach (var v in z.Data) w.Write(v);
        w.Write("fixture"u8);
        w.Flush();
        return ms.ToArray();
    }
}

/// <summary>
/// The instrument models on the NULL device through the wrapper: the selection is
/// recorded while off and costs no memory, enabling loads exactly one pack,
/// switching while it plays cross-fades to the new selection's pack and frees the
/// old one, disabling RELEASES the voice and its pack (#61 part 2), and re-enabling
/// brings the same selection back. Synthetic packs in a temp voice directory.
/// </summary>
[Collection("NullDevice")]
public sealed class ChartInstrumentNullDeviceTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    private readonly JuceAudioEngine? _engine;

    public ChartInstrumentNullDeviceTests()
    {
        Environment.SetEnvironmentVariable(VoicePackFixture.EnvVar, VoicePackFixture.WriteAll("juce_sharp_chart_instrument_tests"));
        if (!NativeProbe.IsAvailable) return;
        _engine = NullDevice.CreateEngine(inputChannels: 1);
    }

    public void Dispose()
    {
        _engine?.Dispose();
        Environment.SetEnvironmentVariable(VoicePackFixture.EnvVar, null);
        _scope.Dispose();
    }

    private static bool WaitFor(Func<bool> condition, int ms = 2000)
    {
        var deadline = Environment.TickCount64 + ms;
        while (Environment.TickCount64 < deadline)
        {
            if (condition()) return true;
            Thread.Sleep(10);
        }
        return condition();
    }

    private JuceChartSourceStatus Status()
    {
        Assert.True(_engine!.TryGetChartSourceStatus(0, out var st));
        return st;
    }

    [NativeAvailableFact]
    public void TheSelection_DefaultsToTheLegacyVoices_AndIsValidated()
    {
        Assert.Equal(new JuceChartInstrument(JuceChartInstrumentModel.SemiHollow, JuceChartPickup.Bridge, false), _engine!.GetChartInstrument(0, bass: false));
        Assert.Equal(new JuceChartInstrument(JuceChartInstrumentModel.JazzBass, JuceChartPickup.NeckBridge, false), _engine.GetChartInstrument(0, bass: true));
        // A pickup the model does not have, and an unknown model, are refused and change nothing.
        Assert.False(_engine.TrySetChartInstrument(0, new JuceChartInstrument(JuceChartInstrumentModel.StratSss, JuceChartPickup.NeckBridge, false)));
        Assert.False(_engine.TrySetChartInstrument(0, new JuceChartInstrument((JuceChartInstrumentModel)9, JuceChartPickup.Neck, false)));
        Assert.Equal(JuceChartInstrumentModel.SemiHollow, _engine.GetChartInstrument(0, false)!.Value.Model);
        // A bass model sets the BASS selection only, per seat.
        Assert.True(_engine.TrySetChartInstrument(1, new JuceChartInstrument(JuceChartInstrumentModel.PrecisionBass, JuceChartPickup.Middle, false)));
        Assert.Equal(JuceChartInstrumentModel.PrecisionBass, _engine.GetChartInstrument(1, true)!.Value.Model);
        Assert.Equal(JuceChartInstrumentModel.SemiHollow, _engine.GetChartInstrument(1, false)!.Value.Model);
        Assert.Equal(JuceChartInstrumentModel.JazzBass, _engine.GetChartInstrument(0, true)!.Value.Model);
        Assert.Throws<ArgumentOutOfRangeException>(() => _engine.TrySetChartInstrument(4, new JuceChartInstrument(JuceChartInstrumentModel.StratSss, JuceChartPickup.Neck, false)));
    }

    [NativeAvailableFact]
    public void PacksAreHeldOnlyWhileTheSynthIsOn_AndOnlyTheSelectionsOwn()
    {
        var e = _engine!;
        using var song = e.OpenMemory(ChartSourceNullDeviceTests.SilentWav(6.0), "wav");
        e.SetSongClockSource(song);
        e.SetChartNotes(0, ChartSourceNullDeviceTests.Notes(30, 0.1, 0.1), default);
        var hss = new JuceChartInstrument(JuceChartInstrumentModel.StratHss, JuceChartPickup.Bridge, false);
        Assert.True(e.TrySetChartVoice(0, JuceChartVoice.Guitar));
        Assert.True(e.TrySetChartInstrument(0, hss));

        // Off: the choice is recorded, nothing is loaded.
        var st = Status();
        Assert.False(st.IsVoiceLoaded);
        Assert.Equal(0, st.PackBytes);
        Assert.Equal(hss, st.Instrument);
        Assert.Equal(0, JuceAudioEngine.ChartVoicePackBytes);

        // On: exactly the selection's pack (strat-bridge for the HSS bridge humbucker).
        e.SetChartSourceEnabled(0, true);
        song.Play();
        st = Status();
        Assert.True(st.IsVoiceLoaded && st.IsSampled, "enabling did not load a sampled voice");
        var onePack = st.PackBytes;
        Assert.True(onePack > 0);
        Assert.Equal(onePack, JuceAudioEngine.ChartVoicePackBytes);

        // Switch while it plays: the new selection's pack, and the old one freed after the cross-fade.
        var neck = new JuceChartInstrument(JuceChartInstrumentModel.StratHss, JuceChartPickup.Neck, false);
        Assert.True(e.TrySetChartInstrument(0, neck));
        Assert.Equal(neck, Status().Instrument);
        Assert.True(WaitFor(() => JuceAudioEngine.ChartVoicePackBytes == onePack),
            $"after the switch {JuceAudioEngine.ChartVoicePackBytes} pack bytes are held, expected one pack ({onePack})");
        // Coil split toggles while it plays, too.
        Assert.True(e.TrySetChartInstrument(0, hss with { CoilSplit = true }));
        Assert.True(Status().Instrument.CoilSplit);

        // Off: the voice AND its pack go (#61 part 2).
        e.SetChartSourceEnabled(0, false);
        Assert.True(WaitFor(() => JuceAudioEngine.ChartVoicePackBytes == 0 && !Status().IsVoiceLoaded),
            $"disabling kept {JuceAudioEngine.ChartVoicePackBytes} pack bytes alive");
        Assert.Equal(0, Status().PackBytes);
        Assert.Equal(hss with { CoilSplit = true }, Status().Instrument);   // the selection survives

        // On again: the same selection, reloaded, playing.
        e.SetChartSourceEnabled(0, true);
        st = Status();
        Assert.True(st.IsVoiceLoaded && st.PackBytes == onePack);
        var started = st.NotesStarted;
        Assert.True(WaitFor(() => Status().NotesStarted > started), "the re-enabled voice started no note");
        e.SetChartSourceEnabled(0, false);
        song.Pause();
    }

    [NativeAvailableFact]
    public void ABassChart_PlaysTheBassSelection()
    {
        var e = _engine!;
        Assert.True(e.TrySetChartInstrument(0, new JuceChartInstrument(JuceChartInstrumentModel.PjBass, JuceChartPickup.Bridge, false)));
        Assert.True(e.TrySetChartVoice(0, JuceChartVoice.Bass));
        e.SetChartSourceEnabled(0, true);
        var st = Status();
        Assert.Equal(JuceChartVoice.Bass, st.Voice);
        Assert.Equal(JuceChartInstrumentModel.PjBass, st.Instrument.Model);
        Assert.True(st.IsSampled && st.PackBytes > 0);
        e.SetChartSourceEnabled(0, false);
        Assert.True(WaitFor(() => JuceAudioEngine.ChartVoicePackBytes == 0));
    }
}

/// <summary>
/// OPT-IN measurement on the REAL packs (set JUCESHARP_REAL_VOICE_DIR to the
/// directory holding them, e.g. DotCharter's artifacts/voice-pack): the process's
/// private memory before enabling, while each model plays, and after disabling.
/// Skipped in CI and on a machine without the packs. Prints the numbers; asserts
/// that the packs load LAZILY (#417: the zone tables only, until a chart asks for
/// zones — here every string and fret), and that the RAM falls back within 32 MB
/// of where it started. A real chart's RAM is DotCharter's
/// ChartVoiceZoneRamHarnessTests.
/// </summary>
[Collection("NullDevice")]
public sealed class ChartVoiceRealPackRamTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    private readonly ITestOutputHelper _out;

    public ChartVoiceRealPackRamTests(ITestOutputHelper output) => _out = output;

    /// <summary>Also written to %TEMP%/juce_sharp_voice_ram.txt — the runner does not keep test output.</summary>
    private void Report(string line)
    {
        _out.WriteLine(line);
        File.AppendAllText(Path.Combine(Path.GetTempPath(), "juce_sharp_voice_ram.txt"), line + Environment.NewLine);
    }

    public void Dispose()
    {
        Environment.SetEnvironmentVariable(VoicePackFixture.EnvVar, null);
        _scope.Dispose();
    }

    private static long Private()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        using var p = Process.GetCurrentProcess();
        p.Refresh();
        return p.PrivateMemorySize64;
    }

    [NativeAvailableFact]
    public void RealPacks_AreReleasedWhenTheSynthIsSwitchedOff()
    {
        var dir = Environment.GetEnvironmentVariable("JUCESHARP_REAL_VOICE_DIR");
        if (string.IsNullOrEmpty(dir) || !File.Exists(Path.Combine(dir, "strat-bridge.dcvp")))
        {
            Assert.Skip("JUCESHARP_REAL_VOICE_DIR does not name a directory with the real packs");
            return;
        }
        Environment.SetEnvironmentVariable(VoicePackFixture.EnvVar, dir);
        using var e = NullDevice.CreateEngine(inputChannels: 1);
        var before = Private();
        Report($"before enable: private {before / 1048576.0:F1} MB, packs {JuceAudioEngine.ChartVoicePackBytes / 1048576.0:F1} MB");
        (JuceChartVoice Voice, JuceChartInstrument Inst)[] cases =
        [
            (JuceChartVoice.Guitar, new(JuceChartInstrumentModel.StratHss, JuceChartPickup.Bridge, false)),
            (JuceChartVoice.Guitar, new(JuceChartInstrumentModel.StratSss, JuceChartPickup.Neck, false)),
            (JuceChartVoice.Guitar, new(JuceChartInstrumentModel.SemiHollow, JuceChartPickup.NeckBridge, false)),
            (JuceChartVoice.Bass, new(JuceChartInstrumentModel.JazzBass, JuceChartPickup.NeckBridge, false)),
        ];
        foreach (var (voice, inst) in cases)
        {
            Assert.True(e.TrySetChartVoice(0, voice));
            Assert.True(e.TrySetChartInstrument(0, inst));
            e.SetChartNotes(0, [], []);
            var sw = Stopwatch.StartNew();
            e.SetChartSourceEnabled(0, true);
            var loadMs = sw.ElapsedMilliseconds;
            var on = Private();
            Assert.True(e.TryGetChartSourceStatus(0, out var st));
            // #417: the packs load LAZILY — without a chart only the zone tables
            // are held; a chart over the whole neck decodes every sustain.
            sw.Restart();
            e.SetChartNotes(0, WholeNeck(voice == JuceChartVoice.Bass), []);
            var chartMs = sw.ElapsedMilliseconds;
            Assert.True(e.TryGetChartSourceStatus(0, out var full));
            var onFull = Private();
            e.SetChartSourceEnabled(0, false);
            var deadline = Environment.TickCount64 + 3000;
            while (JuceAudioEngine.ChartVoicePackBytes != 0 && Environment.TickCount64 < deadline) Thread.Sleep(10);
            var off = Private();
            Report($"{inst.Model}/{inst.Pickup}: load {loadMs} ms, pack {st.PackBytes / 1048576.0:F1} MB without a chart, "
                           + $"{full.PackBytes / 1048576.0:F1} MB with the whole neck ({chartMs} ms); "
                           + $"private on {on / 1048576.0:F1} / {onFull / 1048576.0:F1} MB, off {off / 1048576.0:F1} MB (before {before / 1048576.0:F1})");
            Assert.True(st.IsSampled, $"{inst.Model}: the real pack did not load");
            Assert.True(st.PackBytes < 2L * 1048576, $"{inst.Model}: {st.PackBytes / 1048576.0:F1} MB held without a chart (the zone tables are a few KB)");
            Assert.True(full.PackBytes > 20L * 1048576, $"{inst.Model}: a whole-neck chart decoded only {full.PackBytes / 1048576.0:F1} MB");
            Assert.Equal(0, JuceAudioEngine.ChartVoicePackBytes);
            Assert.True(off - before < 32L * 1048576, $"{inst.Model}: RAM after disabling is {(off - before) / 1048576.0:F1} MB above the start");
        }
        e.SetChartNotes(0, [], []);
    }

    /// <summary>Every string, every fret 0..22, once each — every sustain recording.</summary>
    private static JuceChartNote[] WholeNeck(bool bass)
    {
        int[] open = bass ? [28, 33, 38, 43] : [40, 45, 50, 55, 59, 64];
        var notes = new List<JuceChartNote>();
        var t = 0.1;
        for (var s = 0; s < open.Length; s++)
        {
            for (var fret = 0; fret <= 22; fret++, t += 0.05)
            {
                notes.Add(new JuceChartNote
                {
                    StartSeconds = t, ReleaseSeconds = t + 0.3, Pitch = open[s] + fret, Velocity = 0.8f,
                    StringIndex = s, Fret = fret, SlideToFret = -1f, ChordId = -1, BendFirst = -1,
                });
            }
        }

        return notes.ToArray();
    }
}
