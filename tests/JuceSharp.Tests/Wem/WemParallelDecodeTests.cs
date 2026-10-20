using System.Diagnostics;
using Xunit;

namespace JuceSharp.Tests.Wem;

/// <summary>
/// The parallel .wem decode (DotCharter #434): a <c>dca_wem</c> splits its packets
/// into segments decoded on worker threads, each primed with the packet before
/// it. Its output must be BIT-IDENTICAL to the serial decode — float and 16-bit,
/// read in any chunk size, after any seek — on the synthetic fixtures (every
/// layout) and on real Rocksmith files when the decoded-PSARC cache is present.
/// <para>
/// The knobs are the decoder's own environment overrides, read at open:
/// <c>DCA_WEM_DECODE_THREADS</c> (1 = serial) and <c>DCA_WEM_LANE_PACKETS</c>
/// (packets per segment). The fixtures are short, so they run with tiny segments
/// to cut them into many. The variables are process-wide, so the class runs in
/// the serialized <c>NativeEngine</c> collection (nothing else runs beside it).
/// </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class WemParallelDecodeTests
{
    private readonly ITestOutputHelper _out;

    public WemParallelDecodeTests(ITestOutputHelper output) => _out = output;

    public static TheoryData<string, string, int, int> Cuts()
    {
        var data = new TheoryData<string, string, int, int>();
        foreach (var (fixture, layout) in new[]
                 {
                     ("stereo48k.ogg", nameof(WemFixtureWriter.Layout.EmbeddedMod)),
                     ("stereo48k.ogg", nameof(WemFixtureWriter.Layout.EmbeddedStandard)),
                     ("stereo48k.ogg", nameof(WemFixtureWriter.Layout.GranuleChunk)),
                     ("stereo48k.ogg", nameof(WemFixtureWriter.Layout.HeaderTriad)),
                     ("mono44k.ogg", nameof(WemFixtureWriter.Layout.EmbeddedMod)),
                     ("mono44k.ogg", nameof(WemFixtureWriter.Layout.EmbeddedStandard)),
                 })
        {
            data.Add(fixture, layout, 2, 2);
            data.Add(fixture, layout, 3, 5);
            data.Add(fixture, layout, 4, 3);
        }
        return data;
    }

    /// <summary>Every layout, cut three ways: the float and 16-bit reads equal the
    /// serial decode bit for bit, read in a chunk size that straddles segments.</summary>
    [NativeAvailableTheory]
    [MemberData(nameof(Cuts))]
    public void ParallelDecode_IsBitIdenticalToTheSerialDecode(string fixture, string layoutName, int threads, int lanePackets)
    {
        var layout = Enum.Parse<WemFixtureWriter.Layout>(layoutName);
        var wem = WemFixtureWriter.Build(WemFixtureWriter.ReadOgg(WemDecoderTests.Fixture(fixture)), layout, inlineCodebooks: true);

        var serial = WithDecodeEnv(1, lanePackets, () => DecodeF32(wem, 777));
        var parallel = WithDecodeEnv(threads, lanePackets, () => DecodeF32(wem, 777));
        // The cut must actually be parallel: the decoder stays serial below two segments' worth.
        Assert.True(serial.Info.PacketCount >= 2 * lanePackets,
                    $"{fixture} has {serial.Info.PacketCount} packets: too few to cut into {lanePackets}-packet segments");
        AssertBitIdentical(serial.Samples, parallel.Samples, $"{fixture} {layout} f32 threads={threads} lane={lanePackets}");

        var serial16 = WithDecodeEnv(1, lanePackets, () => DecodeS16(wem, 500));
        var parallel16 = WithDecodeEnv(threads, lanePackets, () => DecodeS16(wem, 500));
        Assert.Equal(serial16, parallel16);
        _out.WriteLine($"{fixture} {layout}: {serial.Info.PacketCount} packets, {serial.Info.Frames} frames, identical");
    }

    /// <summary>A seek in parallel mode restarts the batches at the seek point and
    /// still reads exactly what a continuous serial decode produced there — on one
    /// decoder seeking around, and on a fresh decoder per seek (a decoder that ever
    /// fell back to serial stays serial, so the fresh ones keep every seek parallel).</summary>
    [NativeAvailableFact]
    public void Seek_InParallelMode_IsBitExactWithASerialContinuousDecode()
    {
        var wem = WemDecoderTests.RocksmithShapedWem();
        var full = WithDecodeEnv(1, 4, () => DecodeF32(wem, 4096));
        WithDecodeEnv(3, 4, () =>
        {
            using var shared = JuceWemDecoder.OpenMemory(wem);
            var ch = shared.Info.Channels;
            var frames = shared.Info.Frames;
            var rng = new Random(434);
            var positions = new List<long> { 0, 1, 255, 256, 1024, frames - 1, frames };
            for (var i = 0; i < 40; i++) positions.Add(rng.NextInt64(0, frames));

            var buf = new float[5000 * ch];
            foreach (var fresh in new[] { false, true })
            {
                foreach (var pos in positions)
                {
                    using var own = fresh ? JuceWemDecoder.OpenMemory(wem) : null;
                    var d = own ?? shared;
                    d.Seek(pos);
                    var got = d.Read(buf);
                    Assert.Equal((int)Math.Min(5000, frames - pos), got);
                    for (var i = 0; i < got * ch; i++)
                    {
                        Assert.True(BitConverter.SingleToInt32Bits(full.Samples[pos * ch + i]) == BitConverter.SingleToInt32Bits(buf[i]),
                                    $"{(fresh ? "fresh" : "shared")} decoder, seek {pos}: sample {i} differs");
                    }
                }
            }
            return 0;
        });
    }

    /// <summary>Real Rocksmith .wem files (read-only copies of the decoded-PSARC
    /// cache; skipped when absent): the DEFAULT parallel decode against serial.
    /// <c>JUCESHARP_WEM_CORPUS</c> picks how many (default 2).</summary>
    [NativeAvailableFact]
    public void RealRocksmithWems_ParallelDecodeIsBitIdenticalToSerial()
    {
        var files = WemCorpusTests.CorpusFiles(defaultCount: 2);
        if (files.Count == 0)
        {
            Assert.Skip("no PsarcAudioCache .wem files on this machine");
            return;
        }

        foreach (var file in files)
        {
            var wem = WemCorpusTests.ReadCopy(file);
            var serial = WithDecodeEnv(1, null, () => DecodeS16(wem, 8192));
            var parallel = WithDecodeEnv(null, null, () => DecodeS16(wem, 8192));
            Assert.Equal(serial.Length, parallel.Length);
            Assert.True(serial.AsSpan().SequenceEqual(parallel), $"{WemCorpusTests.SongName(file)}: 16-bit output differs");

            var serialF = WithDecodeEnv(1, null, () => DecodeF32(wem, 8192));
            var parallelF = WithDecodeEnv(null, null, () => DecodeF32(wem, 8192));
            AssertBitIdentical(serialF.Samples, parallelF.Samples, WemCorpusTests.SongName(file));
            _out.WriteLine($"{WemCorpusTests.SongName(file)}: {serialF.Info.PacketCount} packets, " +
                           $"{serialF.Info.Frames / (double)serialF.Info.SampleRate:F1} s, identical");
        }
    }

    /// <summary>Serial vs parallel decode time per song, the way DotCharter writes
    /// its WAV (OpenFile + 16-bit reads of 8192 frames). Env-gated
    /// (<c>JUCESHARP_WEM_BENCH=1</c>), logs only. Song count as in
    /// <see cref="WemCorpusTests" /> (default 5 here).</summary>
    [NativeAvailableFact]
    public void Benchmark_SerialVsParallelDecode()
    {
        if (Environment.GetEnvironmentVariable("JUCESHARP_WEM_BENCH") != "1")
        {
            Assert.Skip("set JUCESHARP_WEM_BENCH=1 to run the .wem decode benchmark");
            return;
        }
        var files = WemCorpusTests.CorpusFiles(defaultCount: 5);
        if (files.Count == 0)
        {
            Assert.Skip("no PsarcAudioCache .wem files on this machine");
            return;
        }

        var dir = Directory.CreateTempSubdirectory("jucesharp-wem-bench-").FullName;
        try
        {
            var copies = files.Select(f =>
            {
                var copy = Path.Combine(dir, $"{Guid.NewGuid():N}.wem");
                File.Copy(f, copy);
                return (Song: WemCorpusTests.SongName(f), Path: copy);
            }).ToList();
            WithDecodeEnv(1, null, () => DecodeFileS16(copies[0].Path));
            WithDecodeEnv(null, null, () => DecodeFileS16(copies[0].Path));

            int[] threadCounts = [1, 2, 4];
            _out.WriteLine("| song | length s | rate | " + string.Join(" | ", threadCounts.Select(t => t == 1 ? "serial ms" : $"{t} threads ms")) + " | default ms | speed-up (default) |");
            _out.WriteLine("|---|---|---|" + string.Concat(Enumerable.Repeat("---|", threadCounts.Length + 2)));
            foreach (var (song, path) in copies)
            {
                var times = new List<double>();
                var info = default(JuceWemInfo);
                foreach (var t in threadCounts.Select(t => (int?)t).Append(null))
                {
                    var best = double.MaxValue;
                    for (var rep = 0; rep < 3; rep++)
                    {
                        var sw = Stopwatch.StartNew();
                        info = WithDecodeEnv(t, null, () => DecodeFileS16(path));
                        best = Math.Min(best, sw.Elapsed.TotalMilliseconds);
                    }
                    times.Add(best);
                }
                _out.WriteLine($"| {song} | {info.Frames / (double)info.SampleRate:F0} | {info.SampleRate} | " +
                               string.Join(" | ", times.Select(ms => ms.ToString("F0"))) + $" | {times[0] / times[^1]:F2}x |");
            }
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    internal sealed record F32(float[] Samples, JuceWemInfo Info);

    private static F32 DecodeF32(byte[] wem, int chunkFrames)
    {
        using var d = JuceWemDecoder.OpenMemory(wem);
        var all = new float[d.Info.Frames * d.Info.Channels];
        var at = 0;
        while (at < all.Length)
        {
            var got = d.Read(all.AsSpan(at, Math.Min(all.Length - at, chunkFrames * d.Info.Channels)));
            if (got == 0) break;
            at += got * d.Info.Channels;
        }
        Assert.Equal(all.Length, at);
        Assert.Equal(0, d.Read(new float[64 * d.Info.Channels]));
        return new F32(all, d.Info);
    }

    private static short[] DecodeS16(byte[] wem, int chunkFrames)
    {
        using var d = JuceWemDecoder.OpenMemory(wem);
        var all = new short[d.Info.Frames * d.Info.Channels];
        var at = 0;
        while (at < all.Length)
        {
            var got = d.Read(all.AsSpan(at, Math.Min(all.Length - at, chunkFrames * d.Info.Channels)));
            if (got == 0) break;
            at += got * d.Info.Channels;
        }
        Assert.Equal(all.Length, at);
        return all;
    }

    private static JuceWemInfo DecodeFileS16(string path)
    {
        using var d = JuceWemDecoder.OpenFile(path);
        var pcm = new short[d.Info.Channels * 8192];
        while (d.Read(pcm) > 0) { }
        return d.Info;
    }

    private static void AssertBitIdentical(float[] expected, float[] actual, string what)
    {
        Assert.Equal(expected.Length, actual.Length);
        for (var i = 0; i < expected.Length; i++)
        {
            if (BitConverter.SingleToInt32Bits(expected[i]) != BitConverter.SingleToInt32Bits(actual[i]))
                Assert.Fail($"{what}: sample {i} differs (serial {expected[i]:R}, parallel {actual[i]:R})");
        }
    }

    /// <summary>Runs <paramref name="body" /> with the decoder's overrides set (null =
    /// unset, the default) and puts the previous values back.</summary>
    private static T WithDecodeEnv<T>(int? threads, int? lanePackets, Func<T> body)
    {
        const string threadsVar = "DCA_WEM_DECODE_THREADS";
        const string laneVar = "DCA_WEM_LANE_PACKETS";
        var oldThreads = Environment.GetEnvironmentVariable(threadsVar);
        var oldLane = Environment.GetEnvironmentVariable(laneVar);
        Environment.SetEnvironmentVariable(threadsVar, threads?.ToString(System.Globalization.CultureInfo.InvariantCulture));
        Environment.SetEnvironmentVariable(laneVar, lanePackets?.ToString(System.Globalization.CultureInfo.InvariantCulture));
        try { return body(); }
        finally
        {
            Environment.SetEnvironmentVariable(threadsVar, oldThreads);
            Environment.SetEnvironmentVariable(laneVar, oldLane);
        }
    }
}
