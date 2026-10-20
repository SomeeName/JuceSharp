using System.Diagnostics;
using JuceSharp.AudioIo;
using Xunit;

namespace JuceSharp.Tests.Wem;

/// <summary>
/// The native .wem decoder against the managed path on REAL Rocksmith files: the
/// decoded-PSARC cache DotCharter keeps (<c>%LOCALAPPDATA%\DotCharter\PsarcAudioCache\**\*.wem</c>,
/// overridable with <c>JUCESHARP_WEM_CORPUS_DIR</c>). Read-only: each file is copied
/// to a temp folder first. SKIPS when the cache is absent (CI has none).
/// <para>
/// <c>JUCESHARP_WEM_CORPUS</c> picks how many songs: unset = 2 (a few seconds),
/// a number = that many, <c>all</c> = every one. The benchmark is
/// <see cref="WemBenchmarkTests" />.
/// </para>
/// </summary>
public sealed class WemCorpusTests
{
    private readonly ITestOutputHelper _out;

    public WemCorpusTests(ITestOutputHelper output) => _out = output;

    internal static IReadOnlyList<string> CorpusFiles(int defaultCount)
    {
        var dir = Environment.GetEnvironmentVariable("JUCESHARP_WEM_CORPUS_DIR") is { Length: > 0 } d
            ? d
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                           "DotCharter", "PsarcAudioCache");
        if (!Directory.Exists(dir)) return [];
        var files = Directory.EnumerateFiles(dir, "*.wem", SearchOption.AllDirectories)
                             .OrderBy(f => f, StringComparer.OrdinalIgnoreCase)
                             .ToList();
        var want = Environment.GetEnvironmentVariable("JUCESHARP_WEM_CORPUS");
        if (string.Equals(want, "all", StringComparison.OrdinalIgnoreCase)) return files;
        var count = int.TryParse(want, out var n) && n > 0 ? n : defaultCount;
        return files.Take(count).ToList();
    }

    /// <summary>A read-only copy of a cache file, so nothing here can touch the cache.</summary>
    internal static byte[] ReadCopy(string path)
    {
        var temp = Path.Combine(Path.GetTempPath(), $"jucesharp-wem-corpus-{Guid.NewGuid():N}.wem");
        File.Copy(path, temp);
        try { return File.ReadAllBytes(temp); }
        finally { File.Delete(temp); }
    }

    internal static string SongName(string path) => Path.GetFileName(Path.GetDirectoryName(path)) ?? path;

    [NativeAvailableFact]
    public void RealRocksmithWems_DecodeLikeTheManagedPath()
    {
        var files = CorpusFiles(defaultCount: 2);
        if (files.Count == 0)
        {
            Assert.Skip("no PsarcAudioCache .wem files on this machine");
            return;
        }

        _out.WriteLine("| song | variant | frames (native) | frames (managed) | header samples | max abs diff | 16-bit mismatches |");
        _out.WriteLine("|---|---|---|---|---|---|---|");
        double worst = 0;
        var worstPcm = 0;
        foreach (var file in files)
        {
            var wem = ReadCopy(file);
            using var d = JuceWemDecoder.OpenMemory(wem);
            var native = WemReference.DecodeNative(wem);
            var managed = WemReference.DecodeManaged(wem);
            var cmp = WemReference.Compare(native, managed);
            _out.WriteLine($"| {SongName(file)} | {d.Info.Variant} | {cmp.NativeFrames} | {cmp.ManagedFrames} | " +
                           $"{d.Info.HeaderSampleCount} | {cmp.MaxAbsDiff:E2} | {cmp.Pcm16Mismatches} (max {cmp.MaxPcm16Diff} LSB) |");

            Assert.Equal(managed.Channels, native.Channels);
            Assert.Equal(managed.SampleRate, native.SampleRate);
            Assert.Equal(cmp.ManagedFrames, cmp.NativeFrames);
            worst = Math.Max(worst, cmp.MaxAbsDiff);
            worstPcm = Math.Max(worstPcm, cmp.MaxPcm16Diff);
        }
        _out.WriteLine($"{files.Count} songs, worst max abs diff {worst:E3}, worst 16-bit diff {worstPcm} LSB");
        Assert.True(worst <= 1e-5, $"max |native - managed| = {worst:E3}");
        Assert.True(worstPcm <= 1, $"16-bit samples differ by up to {worstPcm} LSB");
    }
}

/// <summary>
/// Per-song timing of the three ways to turn a .wem into PCM. Env-gated
/// (<c>JUCESHARP_WEM_BENCH=1</c>), logs only — no wall-clock assertion. Song count
/// as in <see cref="WemCorpusTests" /> (default 5 here).
/// </summary>
public sealed class WemBenchmarkTests
{
    private readonly ITestOutputHelper _out;

    public WemBenchmarkTests(ITestOutputHelper output) => _out = output;

    [NativeAvailableFact]
    public void Benchmark_NativeVsWw2OggNVorbisVsWw2OggJuce()
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

        // Warm every path once so the first song does not pay JIT / first-touch.
        var warm = WemCorpusTests.ReadCopy(files[0]);
        WemReference.DecodeNative(warm);
        WemReference.DecodeOgg(WemReference.ToOgg(warm));

        _out.WriteLine("| song | length s | native WEM->PCM ms | ww2ogg ms | + NVorbis ms | ww2ogg+NVorbis ms | ww2ogg + JUCE Ogg ms | speed-up vs ww2ogg+NVorbis |");
        _out.WriteLine("|---|---|---|---|---|---|---|---|");
        double sumNative = 0, sumManaged = 0, sumJuce = 0;
        foreach (var file in files)
        {
            var wem = WemCorpusTests.ReadCopy(file);

            var sw = Stopwatch.StartNew();
            var native = WemReference.DecodeNative(wem);
            var nativeMs = sw.Elapsed.TotalMilliseconds;

            sw.Restart();
            var ogg = WemReference.ToOgg(wem);
            var ww2oggMs = sw.Elapsed.TotalMilliseconds;
            sw.Restart();
            WemReference.DecodeOgg(ogg);
            var nvorbisMs = sw.Elapsed.TotalMilliseconds;

            var temp = Path.Combine(Path.GetTempPath(), $"jucesharp-wem-bench-{Guid.NewGuid():N}.ogg");
            double juceMs;
            try
            {
                sw.Restart();
                var ogg2 = WemReference.ToOgg(wem);
                File.WriteAllBytes(temp, ogg2);
                JuceAudioDecoder.Decode(temp);
                juceMs = sw.Elapsed.TotalMilliseconds;
            }
            finally { File.Delete(temp); }

            var managedMs = ww2oggMs + nvorbisMs;
            sumNative += nativeMs; sumManaged += managedMs; sumJuce += juceMs;
            _out.WriteLine($"| {WemCorpusTests.SongName(file)} | {native.Frames / (double)native.SampleRate:F1} | {nativeMs:F0} | " +
                           $"{ww2oggMs:F0} | {nvorbisMs:F0} | {managedMs:F0} | {juceMs:F0} | {managedMs / nativeMs:F1}x |");
        }
        _out.WriteLine($"| total ({files.Count}) | | {sumNative:F0} | | | {sumManaged:F0} | {sumJuce:F0} | {sumManaged / sumNative:F1}x |");
    }
}
