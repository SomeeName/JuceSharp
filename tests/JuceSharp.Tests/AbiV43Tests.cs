using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v43 (DotCharter #435): the load-time rate conversion, done once into a file.
/// <c>dca_convert_audio_for_device</c> writes a song at the device rate EXACTLY as
/// the preload holds it -- its int16 samples plus its step in a <c>dcps</c> chunk --
/// and
/// <c>dca_source_get_preload_digest</c> fingerprints what a preloaded source holds.
/// The behavioural tests open both files on a 48 kHz Null device and require the
/// SAME digest: the converted file plays bit-identical samples with identical
/// alignment, which is what lets a host cache it without moving sync.
/// </summary>
[Collection("NativeEngine")]
public sealed class AbiV43Tests
{
    private const int DeviceRate = 48_000;

    private readonly ITestOutputHelper _out;

    public AbiV43Tests(ITestOutputHelper output) => _out = output;

    /// <summary>An engine on the native Null device at exactly <paramref name="rate"/>:
    /// never the machine's real output. <c>EnableNullDevice</c> alone only REGISTERS
    /// the type, and the engine would open the default device at its own rate.</summary>
    private static JuceAudioEngine NullEngine(int rate)
    {
        var engine = new JuceAudioEngine(new JuceEngineConfig
        {
            EnableNullDevice = true, OutputTypeName = "Null", SampleRate = rate, BufferFrames = 480,
        });
        Assert.Equal(rate, engine.SampleRate);
        return engine;
    }

    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    public static TheoryData<string> Exports() => new()
    {
        "dca_convert_audio_for_device", "dca_source_get_preload_digest",
    };

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 43u, "the v43 exports predate the current version");
    }

    [Theory]
    [MemberData(nameof(Exports))]
    public void EveryV43Export_IsInTheHeaderAndTheDll(string export)
    {
        Assert.Contains($"DCA_CALL {export}(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr), $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    /// <summary>The managed enum's values are the header's <c>out_bits</c> answers,
    /// and 16 is the only depth ever written.</summary>
    [Fact]
    public void TheConversionEnum_IsTheBitDepth()
    {
        Assert.Equal(0, (int)JuceDeviceRateConversion.NotNeeded);
        Assert.Equal(16, (int)JuceDeviceRateConversion.Pcm16);
        Assert.Equal(2, Enum.GetValues<JuceDeviceRateConversion>().Length);
        Assert.Contains("16  the device-rate file described above", Header(), StringComparison.Ordinal);
        Assert.Contains("\"dcps\" 24 bytes: u32 version (1) | f32 step | u64 frames | u32 source rate | u32 0",
            Header(), StringComparison.Ordinal);
    }

    /// <summary>
    /// A 44.1 kHz song well inside full scale: the load converts it, the converted
    /// file is 16-bit, and opening that file on the same 48 kHz device holds the
    /// same samples, step, frame count, rate and length as the load-time conversion.
    /// </summary>
    [NativeAvailableFact]
    public void A16BitConversion_PlaysBitIdentically_ToTheLoadTimeConversion()
        => AssertConvertedFilePlaysIdentically(amplitude: 0.45, expected: JuceDeviceRateConversion.Pcm16);

    /// <summary>
    /// A full-scale square wave: the upsampler overshoots ±1 and the preload picks a
    /// step coarser than 1/32768, as it does for real loud masters. The file is
    /// STILL 16-bit -- the preload's int16 samples with the step in the dcps chunk --
    /// and the open is still bit-identical.
    /// </summary>
    [NativeAvailableFact]
    public void AnOvershootingConversion_IsStill16Bit_AndPlaysBitIdentically()
        => AssertConvertedFilePlaysIdentically(amplitude: -1.0, expected: JuceDeviceRateConversion.Pcm16);

    /// <summary>Header, fmt, the 24-byte dcps chunk, data: 76 bytes plus the samples.</summary>
    private const int DeviceRateHeaderBytes = 76;

    private void AssertConvertedFilePlaysIdentically(double amplitude, JuceDeviceRateConversion expected)
    {
        var dir = Directory.CreateTempSubdirectory("jucesharp-abi43-").FullName;
        try
        {
            var src = Path.Combine(dir, "song.wav");
            var dst = Path.Combine(dir, "song.48000.wav");
            File.WriteAllBytes(src, amplitude < 0 ? SquareWav(44_100, 7) : MusicWav(44_100, 7, amplitude));

            Assert.Equal(expected, JuceAudioEngine.ConvertAudioForDevice(src, dst, DeviceRate));
            Assert.True(File.Exists(dst));

            using var engine = NullEngine(DeviceRate);
            Assert.Equal(DeviceRate, engine.SampleRate);

            // OpenMemory with the "wav" hint is how DotCharter opens a library song.
            JucePreloadDigest atLoad, preconverted;
            TimeSpan atLoadLength, preconvertedLength;
            using (var a = engine.OpenMemory(File.ReadAllBytes(src), "wav"))
            {
                atLoad = a.GetPreloadDigest();
                atLoadLength = a.Length;
            }
            using (var b = engine.OpenMemory(File.ReadAllBytes(dst), "wav"))
            {
                preconverted = b.GetPreloadDigest();
                preconvertedLength = b.Length;
            }
            _out.WriteLine($"at load:      {atLoad}");
            _out.WriteLine($"preconverted: {preconverted}");

            Assert.True(atLoad.ConvertedAtLoad, "the 44.1 kHz original was not rate-converted at load -- the test proves nothing");
            Assert.False(preconverted.ConvertedAtLoad, "the converted file was converted AGAIN at load");
            Assert.Equal(DeviceRate, atLoad.SampleRate);
            Assert.Equal(atLoad.SampleRate, preconverted.SampleRate);
            Assert.Equal(atLoad.Frames, preconverted.Frames);
            Assert.Equal(atLoad.Step, preconverted.Step);
            Assert.Equal(atLoad.Hash, preconverted.Hash);
            Assert.Equal(atLoadLength, preconvertedLength);
            if (amplitude > 0) Assert.Equal(1f / 32768f, atLoad.Step);
            else Assert.True(atLoad.Step > 1f / 32768f, $"a full-scale square should overshoot; step {atLoad.Step}");

            // 1x the bytes of a 16-bit file at the device rate, whatever the step.
            Assert.Equal(DeviceRateHeaderBytes + atLoad.Frames * 2 * 2, new FileInfo(dst).Length);
            var stored = ReadStoredStep(dst);
            Assert.Equal(atLoad.Step, stored.Step);
            Assert.Equal(atLoad.Frames, stored.Frames);

            // Opened from the file rather than from memory: the same samples.
            using (var c = engine.OpenFile(dst))
                Assert.Equal(preconverted, c.GetPreloadDigest());
        }
        finally { TryDelete(dir); }
    }

    /// <summary>
    /// A device-rate file opened on a device at ANOTHER rate keeps its level: it is
    /// scaled to its real level before the load converts it again -- DOWN at a lower
    /// rate (JuceSharp #33; before that the open adopted the stored step and held the
    /// very same 48 kHz samples), UP at a higher one, where the step tracks what the
    /// ORIGINAL converts to there.
    /// </summary>
    [NativeAvailableFact]
    public void ADeviceRateFile_OnAnotherDeviceRate_KeepsItsLevel()
    {
        var dir = Directory.CreateTempSubdirectory("jucesharp-abi43-").FullName;
        try
        {
            var src = Path.Combine(dir, "song.wav");
            var dst = Path.Combine(dir, "song.48000.wav");
            File.WriteAllBytes(src, SquareWav(44_100, 3));
            Assert.Equal(JuceDeviceRateConversion.Pcm16, JuceAudioEngine.ConvertAudioForDevice(src, dst, DeviceRate));
            var stored = ReadStoredStep(dst);
            Assert.True(stored.Step > 1f / 32768f);

            JucePreloadDigest at48;
            using (var engine = NullEngine(DeviceRate))
            using (var s = engine.OpenFile(dst))
                at48 = s.GetPreloadDigest();

            using (var engine = NullEngine(44_100))
            {
                Assert.Equal(44_100, engine.SampleRate);
                using var s = engine.OpenFile(dst);
                var at441 = s.GetPreloadDigest();
                _out.WriteLine($"44.1 kHz: device-rate file {at441}, stored step {stored.Step}");
                Assert.True(at441.ConvertedAtLoad, "the 48 kHz device-rate file was not downsampled at load");
                Assert.Equal(44_100.0, at441.SampleRate);
                Assert.NotEqual(at48.Hash, at441.Hash);
                // Scaled to its real level first. Read unscaled, the stored samples decode
                // at 1/32768 per LSB -- about 0.78x the song here -- so the downsampled peak
                // stays under full scale and the step falls back to exactly 1/32768. Scaled,
                // the square's own overshoot survives the trip back to 44.1 kHz (measured
                // 1.02x full scale; it was 1.28x at 48 kHz, where the band-limited square is
                // sampled at other instants, so the two steps are not comparable).
                Assert.True(at441.Step > 1.01f / 32768f,
                    $"the 48 kHz device-rate file was downsampled WITHOUT its stored step (step {at441.Step})");
            }

            using (var engine = NullEngine(96_000))
            {
                Assert.Equal(96_000, engine.SampleRate);
                JucePreloadDigest original, devRate;
                using (var a = engine.OpenFile(src)) original = a.GetPreloadDigest();
                using (var b = engine.OpenFile(dst)) devRate = b.GetPreloadDigest();
                _out.WriteLine($"96 kHz: original {original}, device-rate file {devRate}");
                Assert.True(original.ConvertedAtLoad && devRate.ConvertedAtLoad);
                Assert.True(devRate.Step > 1f / 32768f, "the 48 kHz file was not scaled to its level before the upsample");
                Assert.InRange(devRate.Step / original.Step, 0.97f, 1.03f);
            }
        }
        finally { TryDelete(dir); }
    }

    /// <summary>
    /// A device-rate file whose chunk no longer adds up is REFUSED, never played at
    /// the wrong level: a host then re-derives the song (DotCharter falls back to the
    /// load-time conversion). A plain WAV is untouched by the probe.
    /// </summary>
    [NativeAvailableFact]
    public void ADamagedDeviceRateFile_IsRefused()
    {
        var dir = Directory.CreateTempSubdirectory("jucesharp-abi43-").FullName;
        try
        {
            var src = Path.Combine(dir, "song.wav");
            var dst = Path.Combine(dir, "song.48000.wav");
            File.WriteAllBytes(src, MusicWav(44_100, 2, 0.3));
            Assert.Equal(JuceDeviceRateConversion.Pcm16, JuceAudioEngine.ConvertAudioForDevice(src, dst, DeviceRate));
            var good = File.ReadAllBytes(dst);

            using var engine = NullEngine(DeviceRate);
            using (engine.OpenMemory(good, "wav")) { }

            var truncated = good[..^1000];
            var badStep = (byte[])good.Clone();
            BitConverter.TryWriteBytes(badStep.AsSpan(48), 0f);
            foreach (var damaged in new[] { truncated, badStep })
            {
                var ex = Assert.Throws<JuceAudioException>(() => engine.OpenMemory(damaged, "wav"));
                Assert.Equal(JuceAudioStatus.Decode, ex.Status);
                var path = Path.Combine(dir, $"damaged-{damaged.Length}.wav");
                File.WriteAllBytes(path, damaged);
                Assert.Throws<JuceAudioException>(() => engine.OpenFile(path));
                Assert.Throws<JuceAudioException>(() => JuceAudioEngine.ConvertAudioForDevice(path, dst + ".x", 96_000));
            }

            var plain = (byte[])good.Clone();
            plain[36] = (byte)'x';   // rename the chunk: an ordinary 16-bit WAV again
            using (var s = engine.OpenMemory(plain, "wav"))
                Assert.Equal(1f / 32768f, s.GetPreloadDigest().Step);
        }
        finally { TryDelete(dir); }
    }

    /// <summary>The engine's other readers -- and the managed <c>WavIo</c> -- honour the
    /// stored step too: the decoded song keeps the over-full-scale peak a plain
    /// 16-bit read would lose.</summary>
    [NativeAvailableFact]
    public void TheOfflineDecode_HonoursTheStoredStep()
    {
        var dir = Directory.CreateTempSubdirectory("jucesharp-abi43-").FullName;
        try
        {
            var src = Path.Combine(dir, "song.wav");
            var dst = Path.Combine(dir, "song.48000.wav");
            File.WriteAllBytes(src, SquareWav(44_100, 2));
            Assert.Equal(JuceDeviceRateConversion.Pcm16, JuceAudioEngine.ConvertAudioForDevice(src, dst, DeviceRate));
            var stored = ReadStoredStep(dst);
            var decoded = JuceSharp.AudioIo.JuceAudioDecoder.Decode(dst);
            var peak = decoded.Interleaved.Max(MathF.Abs);
            Assert.True(peak > 1f, $"a 16-bit read tops out at 1.0; the stored step should restore the overshoot, got {peak}");
            Assert.InRange(peak, 32767f * stored.Step * 0.999f, 32768f * stored.Step * 1.001f);

            // The managed reader (DotCharter's stem separation input) agrees.
            var managed = JuceSharp.AudioIo.WavIo.Read(dst);
            Assert.Equal(48_000, managed.SampleRate);
            Assert.Equal(peak, managed.Interleaved.Max(MathF.Abs), 5);
        }
        finally { TryDelete(dir); }
    }

    /// <summary>Reads the dcps chunk the writer places after fmt (offset 36).</summary>
    internal static (float Step, long Frames, int SourceRate) ReadStoredStep(string path)
    {
        var head = new byte[DeviceRateHeaderBytes];
        using (var f = File.OpenRead(path)) f.ReadExactly(head);
        Assert.Equal("dcps"u8.ToArray(), head[36..40]);
        Assert.Equal(24, BitConverter.ToInt32(head, 40));
        Assert.Equal(1, BitConverter.ToInt32(head, 44));
        Assert.Equal("data"u8.ToArray(), head[68..72]);
        return (BitConverter.ToSingle(head, 48), BitConverter.ToInt64(head, 52), BitConverter.ToInt32(head, 60));
    }

    /// <summary>The digest is not a constant: a one-sample change moves it.</summary>
    [NativeAvailableFact]
    public void TheDigest_SeesASingleSampleChange()
    {
        using var engine = NullEngine(DeviceRate);
        var wav = MusicWav(DeviceRate, 2, 0.3);
        ulong first, second;
        using (var a = engine.OpenMemory(wav, "wav")) first = a.GetPreloadDigest().Hash;
        wav[^2] ^= 1;   // the low byte of the last sample of the last channel
        using (var b = engine.OpenMemory(wav, "wav")) second = b.GetPreloadDigest().Hash;
        Assert.NotEqual(first, second);
    }

    /// <summary>
    /// Nothing to convert, nothing written: the engine only UPsamples at load, so a
    /// file at or above the device rate is opened as it is.
    /// </summary>
    [NativeAvailableFact]
    public void AFileAtOrAboveTheDeviceRate_IsNotConverted_AndNothingIsWritten()
    {
        var dir = Directory.CreateTempSubdirectory("jucesharp-abi43-").FullName;
        try
        {
            var at48 = Path.Combine(dir, "a.wav");
            var at441 = Path.Combine(dir, "b.wav");
            File.WriteAllBytes(at48, MusicWav(48_000, 1, 0.3));
            File.WriteAllBytes(at441, MusicWav(44_100, 1, 0.3));
            var dst = Path.Combine(dir, "out.wav");

            Assert.Equal(JuceDeviceRateConversion.NotNeeded, JuceAudioEngine.ConvertAudioForDevice(at48, dst, 48_000));
            Assert.Equal(JuceDeviceRateConversion.NotNeeded, JuceAudioEngine.ConvertAudioForDevice(at48, dst, 44_100));
            Assert.Equal(JuceDeviceRateConversion.NotNeeded, JuceAudioEngine.ConvertAudioForDevice(at441, dst, 44_100));
            Assert.False(File.Exists(dst));
        }
        finally { TryDelete(dir); }
    }

    [NativeAvailableFact]
    public void TheConverter_RejectsBadArguments_AndAMissingSource()
    {
        var dir = Directory.CreateTempSubdirectory("jucesharp-abi43-").FullName;
        try
        {
            var src = Path.Combine(dir, "a.wav");
            File.WriteAllBytes(src, MusicWav(44_100, 1, 0.3));
            var dst = Path.Combine(dir, "out.wav");

            var bits = 7;
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.ConvertAudioForDevice(src, dst, 0, out bits));
            Assert.Equal(0, bits);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.ConvertAudioForDevice(src, dst, double.NaN, out _));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.ConvertAudioForDevice(src, dst, 48_000.5, out _));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.ConvertAudioForDevice(src, src, 48_000, out _));
            Assert.Equal(NativeMethods.DcaStatus.Decode,
                NativeMethods.ConvertAudioForDevice(Path.Combine(dir, "missing.wav"), dst, 48_000, out _));
            Assert.False(File.Exists(dst));
        }
        finally { TryDelete(dir); }
    }

    /// <summary>Every out is zeroed before the handle is looked at.</summary>
    [NativeAvailableFact]
    public void TheDigest_RejectsANullSource_AndZeroesItsOuts()
    {
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.SourceGetPreloadDigest(nint.Zero, out var hash, out var step, out var frames, out var rate,
                out var converted));
        Assert.Equal(0ul, hash);
        Assert.Equal(0f, step);
        Assert.Equal(0L, frames);
        Assert.Equal(0.0, rate);
        Assert.Equal(0, converted);
    }

    /// <summary>
    /// Repeat-open time of real cached songs, original (converted at every open)
    /// against the device-rate file, on a 48 kHz Null device, through OpenMemory as
    /// DotCharter opens them. Env-gated (<c>JUCESHARP_DEVICE_RATE_BENCH=1</c>), logs
    /// only. Reads the song WAVs (<c>&lt;wem id&gt;.wav</c>) under <c>%LOCALAPPDATA%\DotCharter\PsarcAudioCache</c> (or
    /// <c>JUCESHARP_WEM_CORPUS_DIR</c>) READ-ONLY: each song is copied into a temp
    /// folder, and the converted file is written there.
    /// </summary>
    [NativeAvailableFact]
    public void Benchmark_RepeatOpen_OriginalVsDeviceRateFile()
    {
        if (Environment.GetEnvironmentVariable("JUCESHARP_DEVICE_RATE_BENCH") != "1")
        {
            Assert.Skip("set JUCESHARP_DEVICE_RATE_BENCH=1 to run the device-rate open benchmark");
            return;
        }
        var root = Environment.GetEnvironmentVariable("JUCESHARP_WEM_CORPUS_DIR") is { Length: > 0 } d
            ? d
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DotCharter", "PsarcAudioCache");
        var songs = Directory.Exists(root)
            ? Directory.EnumerateFiles(root, "*.wav", SearchOption.AllDirectories)
                .Where(f => Path.GetFileNameWithoutExtension(f).All(char.IsAsciiDigit) && WavRate(f) == 44_100)
                .OrderByDescending(f => new FileInfo(f).Length)
                .Take(int.TryParse(Environment.GetEnvironmentVariable("JUCESHARP_DEVICE_RATE_BENCH_SONGS"), out var n) ? n : 3)
                .ToList()
            : [];
        if (songs.Count == 0) { Assert.Skip("no 44.1 kHz cached WAVs on this machine"); return; }

        var dir = Directory.CreateTempSubdirectory("jucesharp-devrate-bench-").FullName;
        try
        {
            using var engine = NullEngine(DeviceRate);
            _out.WriteLine("| song | length s | convert once ms | step x 32768 | bytes original -> device-rate | original read+open ms (median of 5) | device-rate read+open ms (median of 5) | digest equal |");
            _out.WriteLine("|---|---|---|---|---|---|---|---|");
            foreach (var song in songs)
            {
                var src = Path.Combine(dir, Guid.NewGuid().ToString("N") + ".wav");
                File.Copy(song, src);
                var dst = Path.ChangeExtension(src, ".48000.wav");
                var sw = Stopwatch.StartNew();
                var written = JuceAudioEngine.ConvertAudioForDevice(src, dst, DeviceRate);
                var convertMs = sw.Elapsed.TotalMilliseconds;

                var (origMs, origDigest, length) = MedianOpen(engine, src);
                var (fastMs, fastDigest, _) = MedianOpen(engine, dst);
                Assert.Equal(JuceDeviceRateConversion.Pcm16, written);
                var equal = origDigest == fastDigest with { ConvertedAtLoad = true };
                _out.WriteLine($"| {Path.GetFileName(Path.GetDirectoryName(song))} | {length.TotalSeconds:0} | {convertMs:0} | {origDigest.Step * 32768:0.0000} | {new FileInfo(src).Length:N0} -> {new FileInfo(dst).Length:N0} | {origMs:0} | {fastMs:0} | {equal} |");
                Assert.True(equal, $"{song}: {origDigest} vs {fastDigest}");
                File.Delete(src);
                File.Delete(dst);
            }
        }
        finally { TryDelete(dir); }
    }

    private static (double Ms, JucePreloadDigest Digest, TimeSpan Length) MedianOpen(JuceAudioEngine engine, string path)
    {
        var times = new List<double>();
        JucePreloadDigest digest = default;
        var length = TimeSpan.Zero;
        for (var i = 0; i < 5; i++)
        {
            // The read is timed too: the app reads the whole file before every open
            // (NativeFileBuffer).
            var sw = Stopwatch.StartNew();
            var bytes = File.ReadAllBytes(path);
            using var s = engine.OpenMemory(bytes, "wav");
            times.Add(sw.Elapsed.TotalMilliseconds);
            if (i == 0) { digest = s.GetPreloadDigest(); length = s.Length; }
        }
        times.Sort();
        return (times[2], digest, length);
    }

    private static int WavRate(string path)
    {
        try
        {
            using var f = File.OpenRead(path);
            var head = new byte[64];
            if (f.Read(head, 0, head.Length) < 28) return 0;
            return BitConverter.ToInt32(head, 24);
        }
        catch (IOException) { return 0; }
    }

    /// <summary>A stereo 16-bit WAV of a few partials and a little noise, peaking at <paramref name="amplitude"/>.</summary>
    internal static byte[] MusicWav(int rate, int seconds, double amplitude)
    {
        var rng = new Random(435);
        return Wav(rate, rate * seconds, i =>
        {
            var t = (double)i / rate;
            var x = 0.5 * Math.Sin(2 * Math.PI * 220 * t) + 0.3 * Math.Sin(2 * Math.PI * 1_337 * t)
                    + 0.15 * Math.Sin(2 * Math.PI * 9_100 * t) + 0.05 * (rng.NextDouble() * 2 - 1);
            var l = (short)Math.Round(amplitude * x * 32767);
            var r = (short)Math.Round(amplitude * x * 0.8 * 32767);
            return (l, r);
        });
    }

    /// <summary>A full-scale 441 Hz square, both rails (+32767 and -32768) hit.</summary>
    private static byte[] SquareWav(int rate, int seconds)
        => Wav(rate, rate * seconds, i => (i / 50 % 2 == 0 ? (short)32767 : short.MinValue, i / 50 % 2 == 0 ? short.MinValue : (short)32767));

    private static byte[] Wav(int rate, int frames, Func<int, (short L, short R)> sample)
    {
        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);
        w.Write("RIFF"u8); w.Write(36 + frames * 4); w.Write("WAVE"u8);
        w.Write("fmt "u8); w.Write(16); w.Write((short)1); w.Write((short)2); w.Write(rate); w.Write(rate * 4);
        w.Write((short)4); w.Write((short)16);
        w.Write("data"u8); w.Write(frames * 4);
        for (var i = 0; i < frames; i++)
        {
            var (l, r) = sample(i);
            w.Write(l); w.Write(r);
        }
        w.Flush();
        return ms.ToArray();
    }

    private static void TryDelete(string dir)
    {
        try { Directory.Delete(dir, recursive: true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
