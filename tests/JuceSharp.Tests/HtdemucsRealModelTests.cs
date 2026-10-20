using System.Diagnostics;
using JuceSharp.StemSeparation.Onnx;
using Microsoft.ML.OnnxRuntime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ENV-GATED: every <see cref="OnnxStemModelKind.Htdemucs"/> catalogue entry,
/// run for real. Skips cleanly unless <c>JUCESHARP_HTDEMUCS_MODEL_DIR</c> names
/// a directory holding the catalogue files under their
/// <see cref="OnnxModelEntry.FileName"/> (an entry whose file is absent skips on
/// its own). Never downloads — fetch with <c>OnnxModelCatalog.EnsureAsync</c>
/// into a scratch directory first.
/// <para>
/// <c>JUCESHARP_HTDEMUCS_TEST_WAV</c> picks the audio (default: the TestData
/// guitar preview the other ONNX integration tests use);
/// <c>JUCESHARP_HTDEMUCS_PROVIDER</c> picks <c>cpu</c> (default) or
/// <c>directml</c>. Only the first 30 s of the WAV are separated.
/// </para>
/// <para>
/// What it pins, per entry: the file is the pinned one, its measured IO layout
/// is the one the catalogue DECLARES (the lint in
/// <see cref="HtdemucsIoLayoutTests"/> only checks the declaration is
/// supported), and the separation is sane — the declared stem count, finite
/// samples, no stem louder than the mix (DirectML graph fusion once inflated
/// htdemucs output 35–65x, #151) and stems that roughly add back up to it.
/// </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class HtdemucsRealModelTests
{
    private const string ModelDirVariable = "JUCESHARP_HTDEMUCS_MODEL_DIR";

    public static TheoryData<string> HtdemucsEntries =>
        [.. OnnxModelCatalog.ForKind(OnnxStemModelKind.Htdemucs).Select(e => e.Id)];

    [Theory]
    [MemberData(nameof(HtdemucsEntries))]
    public async Task CatalogueEntry_BindsAsDeclared_AndSeparatesSanely(string id)
    {
        var entry = OnnxModelCatalog.FindById(id)!;
        var dir = Environment.GetEnvironmentVariable(ModelDirVariable);
        if (string.IsNullOrWhiteSpace(dir))
        {
            Assert.Skip($"{ModelDirVariable} is not set.");
            return;
        }
        var path = Path.Combine(dir, entry.FileName);
        if (!File.Exists(path))
        {
            Assert.Skip($"{entry.FileName} is not in {dir}.");
            return;
        }
        var wavPath = Environment.GetEnvironmentVariable("JUCESHARP_HTDEMUCS_TEST_WAV") is { Length: > 0 } w
            ? w
            : OnnxStemSeparatorIntegrationTests.TestWavPathResolved;
        if (!File.Exists(wavPath))
        {
            Assert.Skip($"Test WAV missing at {wavPath}.");
            return;
        }
        var ct = TestContext.Current.CancellationToken;
        var output = TestContext.Current.TestOutputHelper;

        Assert.True(await OnnxModelCatalog.VerifyAsync(entry, path, ct), $"{path} is not the pinned {entry.Id}.");

        using (var probe = new InferenceSession(path))
        {
            var io = HtdemucsIoBinding.FromSession(probe);
            output?.WriteLine($"{entry.Id}: inputs " +
                string.Join(", ", probe.InputNames.Select(n => $"{n} [{string.Join(",", probe.InputMetadata[n].Dimensions)}]")) +
                "; outputs " +
                string.Join(", ", probe.OutputNames.Select(n => $"{n} [{string.Join(",", probe.OutputMetadata[n].Dimensions)}]")));
            Assert.Equal(entry.HtdemucsLayout, io.Layout);
        }

        var (all, sampleRate, channels) = OnnxStemSeparatorIntegrationTests.ReadWav(wavPath);
        Assert.Equal(2, channels);
        var mix = all.AsSpan(0, Math.Min(all.Length, sampleRate * 30 * 2)).ToArray();

        var provider = OnnxExecutionProviderParser.Parse(
            Environment.GetEnvironmentVariable("JUCESHARP_HTDEMUCS_PROVIDER") ?? "cpu");
        using var sep = new OnnxStemSeparator(path, OnnxStemModelKind.Htdemucs, provider, s => output?.WriteLine(s));
        var sw = Stopwatch.StartNew();
        var stems = await sep.SeparateAsync(mix, sampleRate, ct: ct);
        sw.Stop();

        var mixRms = Rms(mix);
        var sum = new float[mix.Length];
        output?.WriteLine($"{entry.Id} on {sep.BackendDescription}: {sw.ElapsedMilliseconds} ms, mix RMS {mixRms:F4}");
        Assert.Equal(entry.SourceCount, stems.Count);
        foreach (var stem in stems)
        {
            Assert.Equal(mix.Length, stem.InterleavedStereo.Length);
            Assert.All(stem.InterleavedStereo, v => Assert.True(float.IsFinite(v)));
            var rms = Rms(stem.InterleavedStereo);
            output?.WriteLine($"  {stem.Name,-7} RMS {rms:F4} ({rms / mixRms:P0} of the mix)");
            Assert.True(rms < 1.5 * mixRms, $"{stem.Name} is {rms / mixRms:F1}x the mix — inflated output.");
            for (var i = 0; i < sum.Length; i++) sum[i] += stem.InterleavedStereo[i];
        }

        var sumRms = Rms(sum);
        var corr = Correlation(sum, mix);
        output?.WriteLine($"  sum of stems RMS {sumRms:F4} ({sumRms / mixRms:P0} of the mix), correlation {corr:F3}");
        // htdemucs's sources add back up to the mix: the 6-stem export measured
        // 100 % / 0.999 here. MrCitron's in-graph-STFT exports measured 63–69 % /
        // 0.81–0.84 — their summed stems kept ~30 % of the 300 Hz–8 kHz energy
        // and carried 2.5–5x the mix's energy above 8 kHz, uncorrelated (the
        // graph's own spectral branch; raw Python ORT on one segment matched) —
        // and were taken out of the catalogue for it (DotCharter #254). This bar
        // is what keeps an export like that from coming back.
        Assert.InRange(sumRms / mixRms, 0.85, 1.15);
        Assert.True(corr > 0.95, $"the stems add back up to something that correlates only {corr:F3} with the mix.");
    }

    private static double Rms(float[] x)
    {
        double s = 0;
        foreach (var v in x) s += v * (double)v;
        return Math.Sqrt(s / Math.Max(1, x.Length));
    }

    private static double Correlation(float[] a, float[] b)
    {
        double ab = 0, aa = 0, bb = 0;
        for (var i = 0; i < a.Length; i++)
        {
            ab += a[i] * (double)b[i];
            aa += a[i] * (double)a[i];
            bb += b[i] * (double)b[i];
        }
        return ab / Math.Sqrt(Math.Max(1e-30, aa * bb));
    }
}
