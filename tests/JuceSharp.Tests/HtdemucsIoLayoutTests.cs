using JuceSharp.StemSeparation.Onnx;
using Microsoft.ML.OnnxRuntime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// DotCharter #254: the catalogue offered MrCitron's <c>htdemucs.onnx</c> /
/// <c>htdemucs_ft.onnx</c>, which download, verify and can be set Active — and
/// then failed to bind, because they run the STFT inside the graph (one input
/// <c>[batch, 2, time]</c>, one output <c>[batch, stems, 2, time]</c>) and
/// <see cref="HtdemucsOnnxInference"/> only knew the two-input export.
/// </summary>
public sealed class HtdemucsIoLayoutTests
{
    private const int Segment = HtdemucsOnnxInference.Segment;

    // ------------------------------------------------------------ classifier

    /// <summary>The MrCitron graphs, as ORT reports them: named dimensions are -1.</summary>
    [Fact]
    public void MrCitronShape_ClassifiesAsWaveformOnly_WithTheStemCountLeftToTheFirstRun()
    {
        var io = HtdemucsIoBinding.Classify(
            [("input", [-1, 2, -1])],
            [("output", [-1, -1, -1, -1])]);

        Assert.Equal(HtdemucsIoLayout.WaveformOnly, io.Layout);
        Assert.Equal("input", io.WaveInput);
        Assert.Equal("output", io.WaveOutput);
        Assert.Null(io.SpecInput);
        Assert.Null(io.SpecOutput);
        Assert.Equal(0, io.DeclaredStems);
    }

    /// <summary>arjune123's <c>htdemucs_6s.onnx</c>, input names and output names as shipped.</summary>
    [Fact]
    public void Arjune6sShape_ClassifiesAsWaveformAndSpectrogram()
    {
        var io = HtdemucsIoBinding.Classify(
            [("input", [1, 2, Segment]), ("onnx::ReduceMean_1", [1, 4, 2048, 336])],
            [("output", [1, 6, 4, 2048, 336]), ("5012", [-1, -1, -1, -1])]);

        Assert.Equal(HtdemucsIoLayout.WaveformAndSpectrogram, io.Layout);
        Assert.Equal("input", io.WaveInput);
        Assert.Equal("onnx::ReduceMean_1", io.SpecInput);
        Assert.Equal("output", io.SpecOutput);
        Assert.Equal("5012", io.WaveOutput);
        Assert.Equal(6, io.DeclaredStems);
    }

    public static TheoryData<string, (string, int[])[], (string, int[])[]> Unsupported => new()
    {
        { "mono input", [("input", [1, 1, -1])], [("output", [1, 4, 2, -1])] },
        { "wrong segment", [("input", [1, 2, 44100])], [("output", [1, 4, 2, 44100])] },
        { "spectrogram output only", [("input", [1, 2, -1])], [("output", [1, 4, 4, 2048, 336])] },
        { "two-input, no waveform output", [("input", [1, 2, Segment]), ("spec", [1, 4, 2048, 336])],
            [("output", [1, 6, 4, 2048, 336]), ("extra", [1, 6, 4, 2048, 336])] },
        { "no inputs", [], [("output", [1, 4, 2, -1])] },
    };

    [Theory]
    [MemberData(nameof(Unsupported))]
    public void AnUnsupportedLayout_IsRefused_WithTheGraphsRealShapesInTheMessage(
        string why, (string, int[])[] inputs, (string, int[])[] outputs)
    {
        var ex = Assert.Throws<InvalidOperationException>(() => HtdemucsIoBinding.Classify(inputs, outputs));
        foreach (var (name, _) in inputs.Concat(outputs))
            Assert.Contains(name, ex.Message, StringComparison.Ordinal);
        Assert.False(string.IsNullOrEmpty(why));
    }

    // ------------------------------------------------------- catalogue lint

    /// <summary>
    /// The guard #254 asked for: an Htdemucs entry is offered only with a
    /// declared, measured layout the driver can run. The declaration is checked
    /// against the real file by <see cref="HtdemucsRealModelTests"/>.
    /// </summary>
    [Fact]
    public void EveryHtdemucsCatalogueEntry_DeclaresALayoutTheDriverSupports()
    {
        var htdemucs = OnnxModelCatalog.ForKind(OnnxStemModelKind.Htdemucs);
        Assert.NotEmpty(htdemucs);
        foreach (var entry in htdemucs)
        {
            Assert.True(entry.HtdemucsLayout is { } layout && HtdemucsIoBinding.SupportedLayouts.Contains(layout),
                $"'{entry.Id}' declares HtdemucsLayout={entry.HtdemucsLayout?.ToString() ?? "null"}; the driver " +
                $"supports {string.Join(", ", HtdemucsIoBinding.SupportedLayouts)}. Measure the file's IO " +
                "(one input or two?) and declare it, or take the entry out of the catalogue.");
        }

        foreach (var entry in OnnxModelCatalog.All.Where(e => e.Kind != OnnxStemModelKind.Htdemucs))
            Assert.Null(entry.HtdemucsLayout);
    }

    // ------------------------------------- the driver on a waveform-only graph

    private static readonly float[,] Weights =
    {
        { 0.10f, 0.15f },
        { 0.20f, 0.25f },
        { 0.30f, 0.35f },
        { 0.40f, 0.45f },
    };

    /// <summary>
    /// The real driver on a real ORT session of the tiny stand-in graph. With
    /// the crossfade off, every output sample must be exactly
    /// <c>w[s, c] * input</c> — across three chunk boundaries, both file ends
    /// (reflection padding is trimmed away with the margin) and both channels.
    /// </summary>
    [Fact]
    public void WaveformOnlyGraph_SeparatesThroughTheDriver_EverySampleInPlace()
    {
        using var session = new InferenceSession(TinyOnnxModel.WaveformOnlyHtdemucs(Weights));
        var engine = new HtdemucsOnnxInference(session, settings: new HtdemucsOnnxInference.Settings(CrossfadeSeconds: 0f));

        Assert.Equal(HtdemucsIoLayout.WaveformOnly, engine.Layout);

        var input = Noise(frames: HtdemucsOnnxInference.SampleRate * 20 + 123, seed: 254);
        var stems = engine.Predict(input, HtdemucsOnnxInference.SampleRate, ct: TestContext.Current.CancellationToken);

        Assert.Equal(4, engine.StemCount);
        Assert.Equal(["drums", "bass", "other", "vocals"], stems.Select(s => s.Name));
        for (var s = 0; s < stems.Count; s++)
        {
            var stem = stems[s].InterleavedStereo;
            Assert.Equal(input.Length, stem.Length);
            for (var i = 0; i < stem.Length; i++)
            {
                var expected = Weights[s, i % 2] * input[i];
                if (Math.Abs(stem[i] - expected) > 1e-6f)
                    Assert.Fail($"stem {s} ({stems[s].Name}) sample {i} (frame {i / 2}, ch {i % 2}) = {stem[i]}, " +
                                $"expected {expected}.");
            }
        }
        Assert.Equal(0, engine.NonFiniteOutputs);
    }

    /// <summary>
    /// The path #254 actually broke: <see cref="OnnxStemSeparator"/> opening a
    /// waveform-only file as <see cref="OnnxStemModelKind.Htdemucs"/>. Default
    /// settings (the constant-power crossfade on), so the check is per-stem
    /// level rather than per sample — and at 48 kHz, through the resample both
    /// ways.
    /// </summary>
    [Fact]
    public async Task WaveformOnlyModelFile_SeparatesThroughOnnxStemSeparator()
    {
        var path = Path.Combine(Path.GetTempPath(), $"juce-tiny-htdemucs-{Guid.NewGuid():N}.onnx");
        await File.WriteAllBytesAsync(path, TinyOnnxModel.WaveformOnlyHtdemucs(Weights), TestContext.Current.CancellationToken);
        try
        {
            using var sep = new OnnxStemSeparator(path, OnnxStemModelKind.Htdemucs, OnnxExecutionProvider.Cpu);
            // Tones, not white noise: noise at 48 kHz has energy above the
            // 22.05 kHz the model rate keeps, and the resample down would eat it.
            var input = Tones(frames: 48_000 * 12, sampleRate: 48_000);
            var stems = await sep.SeparateAsync(input, 48_000, ct: TestContext.Current.CancellationToken);

            Assert.Equal(["drums", "bass", "other", "vocals"], stems.Select(s => s.Name));
            for (var s = 0; s < stems.Count; s++)
            {
                Assert.Equal(48_000, stems[s].SampleRate);
                Assert.Equal(input.Length, stems[s].InterleavedStereo.Length);
                for (var c = 0; c < 2; c++)
                {
                    var ratio = Rms(stems[s].InterleavedStereo, c) / Rms(input, c);
                    Assert.InRange(ratio, Weights[s, c] * 0.97, Weights[s, c] * 1.03);
                }
            }
        }
        finally
        {
            File.Delete(path);
        }
    }

    private static float[] Noise(int frames, int seed)
    {
        var rng = new Random(seed);
        var x = new float[frames * 2];
        for (var i = 0; i < x.Length; i++) x[i] = (float)(rng.NextDouble() * 2 - 1) * (i % 2 == 0 ? 0.5f : 0.3f);
        return x;
    }

    private static float[] Tones(int frames, int sampleRate)
    {
        var x = new float[frames * 2];
        for (var i = 0; i < frames; i++)
        {
            var t = i / (double)sampleRate;
            x[2 * i]     = (float)(0.3 * Math.Sin(2 * Math.PI * 220 * t) + 0.2 * Math.Sin(2 * Math.PI * 3_100 * t));
            x[2 * i + 1] = (float)(0.25 * Math.Sin(2 * Math.PI * 330 * t + 1) + 0.1 * Math.Sin(2 * Math.PI * 7_000 * t));
        }
        return x;
    }

    private static double Rms(float[] interleaved, int channel)
    {
        double sum = 0;
        var n = interleaved.Length / 2;
        for (var i = 0; i < n; i++) sum += interleaved[2 * i + channel] * (double)interleaved[2 * i + channel];
        return Math.Sqrt(sum / n);
    }
}
