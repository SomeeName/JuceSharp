using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The BS-RoFormer SW support with no model file: the IO-layout classification, the chunk
/// and hop geometry, the stem order, the split-complex tensor packing, the reflect-padded
/// STFT the export was traced with, and the catalogue row. The real model runs in
/// <see cref="BsRoformerSwModelTests"/>, env-gated.
/// </summary>
public sealed class BsRoformerSwLayoutTests
{
    // The SW export's declared tensors, as ONNX Runtime reports them (read 2026-10-05).
    private static readonly (string, int[])[] SwInputs =
    [
        ("spec_real", [1, 2, 1025, 345]),
        ("spec_imag", [1, 2, 1025, 345]),
    ];

    private static readonly (string, int[])[] SwOutputs =
    [
        ("out_spec_real", [1, 6, 2, 1025, 345]),
        ("out_spec_imag", [1, 6, 2, 1025, 345]),
    ];

    private static readonly Dictionary<string, string> NoMetadata = [];

    // ------------------------------------------------------------ classification

    [Fact]
    public void TheSwExport_IsSplitComplex_WithSixStemsAtTheTracedShape()
    {
        var layout = BsRoformerIoBinding.Classify(SwInputs, SwOutputs);

        Assert.Equal(BsRoformerIoLayout.SplitComplex, layout.Layout);
        Assert.Equal(["spec_real", "spec_imag"], layout.InputNames);
        Assert.Equal(["out_spec_real", "out_spec_imag"], layout.OutputNames);
        Assert.Equal(6, layout.Stems);
        Assert.Equal(1025, layout.Bins);
        Assert.Equal(345, layout.Frames);
    }

    /// <summary>Feeding the imaginary plane as the real one runs without an error and
    /// returns noise, so the names decide the pair's order, not the graph's.</summary>
    [Fact]
    public void TheRealAndImaginaryTensors_AreOrderedByName_NotByGraphPosition()
    {
        var layout = BsRoformerIoBinding.Classify([SwInputs[1], SwInputs[0]], [SwOutputs[1], SwOutputs[0]]);

        Assert.Equal(["spec_real", "spec_imag"], layout.InputNames);
        Assert.Equal(["out_spec_real", "out_spec_imag"], layout.OutputNames);
    }

    [Fact]
    public void ThePackedVocalExport_IsStillPacked()
    {
        var layout = BsRoformerIoBinding.Classify([("input", [1, 4, 1025, 801])], [("output", [1, 4, 1025, 801])]);

        Assert.Equal(BsRoformerIoLayout.PackedChannels, layout.Layout);
        Assert.Equal(1, layout.Stems);
        Assert.Equal(["input"], layout.InputNames);
    }

    [Fact]
    public void APackedMultiStemExport_CountsFourChannelsPerStem_AndADynamicAxisIsMinusOne()
    {
        var layout = BsRoformerIoBinding.Classify([("x", [1, 4, -1, -1])], [("y", [1, 24, -1, -1])]);

        Assert.Equal(6, layout.Stems);
        Assert.Equal(-1, layout.Bins);
        Assert.Equal(-1, layout.Frames);
    }

    public static TheoryData<string, (string, int[])[], (string, int[])[]> Unsupported() => new()
    {
        { "three inputs", [.. SwInputs, ("extra", [1])], SwOutputs },
        { "rank-4 outputs", SwInputs, [("out_spec_real", [1, 12, 1025, 345]), ("out_spec_imag", [1, 12, 1025, 345])] },
        { "mono input", [("spec_real", [1, 1, 1025, 345]), ("spec_imag", [1, 1, 1025, 345])], SwOutputs },
        { "three output channels", SwInputs, [("out_spec_real", [1, 6, 3, 1025, 345]), ("out_spec_imag", [1, 6, 3, 1025, 345])] },
        { "bins differ", SwInputs, [("out_spec_real", [1, 6, 2, 1024, 345]), ("out_spec_imag", [1, 6, 2, 1024, 345])] },
        { "dynamic stems", SwInputs, [("out_spec_real", [1, -1, 2, 1025, 345]), ("out_spec_imag", [1, -1, 2, 1025, 345])] },
        { "both imaginary", [("a_imag", [1, 2, 1025, 345]), ("b_imag", [1, 2, 1025, 345])], SwOutputs },
        { "packed with 6 channels", [("x", [1, 6, 1025, 345])], [("y", [1, 6, 1025, 345])] },
        { "packed with 6 out channels", [("x", [1, 4, 1025, 345])], [("y", [1, 6, 1025, 345])] },
    };

    [Theory]
    [MemberData(nameof(Unsupported))]
    public void AnythingElse_IsRefused_WithTheShapesInTheMessage(
        string why, (string, int[])[] inputs, (string, int[])[] outputs)
    {
        var ex = Assert.Throws<NotSupportedException>(() => BsRoformerIoBinding.Classify(inputs, outputs));
        Assert.Contains(inputs[0].Item1, ex.Message, StringComparison.Ordinal);
        Assert.False(string.IsNullOrEmpty(why));
    }

    // ------------------------------------------------------------ geometry

    [Fact]
    public void ACentredStftOfTheSwChunk_HasTheTracedFrameCount()
    {
        Assert.Equal(176_400, BsRoformerIoBinding.SwChunkSamples);
        Assert.Equal(345, BsRoformerIoBinding.FramesFor(176_400, 512));
    }

    [Fact]
    public void WithoutMetadata_TheSwGeometryIsAssumed_AndMatchesTheDeclaredShape()
    {
        var (settings, names) = BsRoformerIoBinding.Classify(SwInputs, SwOutputs).ResolveSplitGeometry(NoMetadata);

        Assert.Equal(2048, settings.NFft);
        Assert.Equal(512, settings.Hop);
        Assert.Equal(176_400, settings.ChunkSize);
        Assert.Equal(2, settings.Overlap);
        Assert.Equal(["bass", "drums", "other", "vocals", "guitar", "piano"], names);
    }

    [Fact]
    public void TheModelsOwnMetadata_Wins()
    {
        var meta = new Dictionary<string, string>
        {
            ["n_fft"] = "2048",
            ["hop_length"] = "512",
            ["chunk_samples"] = "176400",
            ["sample_rate"] = "44100",
            ["stems"] = "[\"Bass\", \"drums\", \"other\", \"vocals\", \"guitar\", \"keys\"]",
        };

        var (settings, names) = BsRoformerIoBinding.Classify(SwInputs, SwOutputs).ResolveSplitGeometry(meta);

        Assert.Equal(176_400, settings.ChunkSize);
        Assert.Equal(["bass", "drums", "other", "vocals", "guitar", "keys"], names);
    }

    [Fact]
    public void AStemsTagOfTheWrongLength_FallsBackToTheSwOrder()
    {
        var meta = new Dictionary<string, string> { ["stems"] = "[\"vocals\", \"other\"]" };
        var (_, names) = BsRoformerIoBinding.Classify(SwInputs, SwOutputs).ResolveSplitGeometry(meta);
        Assert.Equal(BsRoformerIoBinding.SwStemOrder, names);
    }

    [Theory]
    [InlineData("chunk_samples", "352800")] // T would be 690, the model is traced at 345
    [InlineData("hop_length", "441")]       // 176,400 / 441 + 1 = 401 frames
    [InlineData("n_fft", "4096")]           // 2049 bins, the model declares 1025
    [InlineData("sample_rate", "48000")]    // the driver runs spectral models at 44.1 kHz
    public void MetadataThatContradictsTheDeclaredShape_IsRefused(string key, string value)
    {
        var layout = BsRoformerIoBinding.Classify(SwInputs, SwOutputs);
        var meta = new Dictionary<string, string> { ["chunk_samples"] = "176400", [key] = value };
        Assert.Throws<NotSupportedException>(() => layout.ResolveSplitGeometry(meta));
    }

    [Fact]
    public void AnUnknownTraceLength_WithNoChunkTag_TakesTheLongestChunkThatFitsIt()
    {
        var layout = BsRoformerIoBinding.Classify(
            [("spec_real", [1, 2, 1025, 300]), ("spec_imag", [1, 2, 1025, 300])],
            [("out_spec_real", [1, 4, 2, 1025, 300]), ("out_spec_imag", [1, 4, 2, 1025, 300])]);

        var (settings, names) = layout.ResolveSplitGeometry(NoMetadata);

        Assert.Equal(299 * 512, settings.ChunkSize);
        Assert.Equal(300, BsRoformerIoBinding.FramesFor(settings.ChunkSize, settings.Hop));
        Assert.Equal(["stem_1", "stem_2", "stem_3", "stem_4"], names);
    }

    // ------------------------------------------------------------ stem order

    /// <summary>htdemucs is drums, bass, …; SW is bass, drums, …. Naming SW's output with
    /// the htdemucs order would swap the two stems this model is chosen for.</summary>
    [Fact]
    public void TheSwStemOrder_IsBassDrumsOtherVocalsGuitarPiano()
    {
        Assert.Equal(["bass", "drums", "other", "vocals", "guitar", "piano"], BsRoformerIoBinding.SwStemOrder);
    }

    // ------------------------------------------------------------ packing

    [Fact]
    public void PackSplit_IsChannelThenBinThenFrame()
    {
        const int bins = 5;
        const int frames = 4;
        var left = Coded(frames, channel: 0);
        var right = Coded(frames, channel: 1);
        var re = new float[2 * bins * frames];
        var im = new float[2 * bins * frames];

        SpectralOnnxRunner.PackSplit(re, im, left, right, bins, frames);

        for (var c = 0; c < 2; c++)
        for (var f = 0; f < bins; f++)
        for (var t = 0; t < frames; t++)
        {
            var idx = (c * bins + f) * frames + t;
            Assert.Equal(Code(0, c, f, t), re[idx]);
            Assert.Equal(-Code(0, c, f, t), im[idx]);
        }
    }

    [Fact]
    public void UnpackSplit_PicksTheRequestedStemAndChannel()
    {
        const int stems = 6;
        const int bins = 5;
        const int frames = 4;
        var re = new float[stems * 2 * bins * frames];
        var im = new float[re.Length];
        for (var s = 0; s < stems; s++)
        for (var c = 0; c < 2; c++)
        for (var f = 0; f < bins; f++)
        for (var t = 0; t < frames; t++)
        {
            var idx = ((s * 2 + c) * bins + f) * frames + t;
            re[idx] = Code(s, c, f, t);
            im[idx] = -Code(s, c, f, t);
        }

        for (var s = 0; s < stems; s++)
        for (var c = 0; c < 2; c++)
        {
            var dest = new SpectrumPlanes(frames, 8, 2); // 8-point FFT -> 5 bins
            SpectralOnnxRunner.UnpackSplit(re, im, s, c, dest, bins, frames);
            for (var f = 0; f < bins; f++)
            for (var t = 0; t < frames; t++)
            {
                Assert.Equal(Code(s, c, f, t), dest.Re[t][f]);
                Assert.Equal(-Code(s, c, f, t), dest.Im[t][f]);
            }
        }
    }

    [Fact]
    public void UnpackSplit_RefusesAnOutputTooSmallForTheStem()
    {
        var re = new float[2 * 5 * 4]; // one stem's worth
        var dest = new SpectrumPlanes(4, 8, 2);
        Assert.Throws<InvalidOperationException>(() =>
            SpectralOnnxRunner.UnpackSplit(re, re, stem: 1, channel: 0, dest, 5, 4));
    }

    private static float Code(int s, int c, int f, int t) => s * 1000 + c * 100 + f * 10 + t + 1;

    private static SpectrumPlanes Coded(int frames, int channel)
    {
        var planes = new SpectrumPlanes(frames, 8, 2);
        for (var t = 0; t < frames; t++)
        for (var f = 0; f < planes.BinCount; f++)
        {
            planes.Re[t][f] = Code(0, channel, f, t);
            planes.Im[t][f] = -Code(0, channel, f, t);
        }
        return planes;
    }

    // ------------------------------------------------------------ reflect STFT

    /// <summary>
    /// The export's spectrogram is <c>torch.stft(center=True)</c>, whose default pad is
    /// REFLECT. Checked bin for bin against a direct DFT of a reflect-padded frame —
    /// including frame 0, the one zero padding gets wrong.
    /// </summary>
    [Fact]
    public void ReflectPadding_MatchesADirectDftOfTheMirroredSignal()
    {
        const int nFft = 16;
        const int hop = 4;
        var random = new Random(258);
        var signal = Enumerable.Range(0, 37).Select(_ => (float)(random.NextDouble() * 2 - 1)).ToArray();
        var frames = BsRoformerIoBinding.FramesFor(signal.Length, hop);
        var planes = new SpectrumPlanes(frames, nFft, hop);
        var work = new StftWorkspace(nFft, signal.Length + nFft, frames);

        Stft.Forward(signal, planes, work, periodic: true, StftPadding.Reflect);

        var window = Stft.HannWindowPeriodic(nFft);
        for (var t = 0; t < frames; t++)
        for (var k = 0; k <= nFft / 2; k++)
        {
            double re = 0, im = 0;
            for (var n = 0; n < nFft; n++)
            {
                var x = Reflect(signal, t * hop - nFft / 2 + n) * window[n];
                var phase = -2 * Math.PI * k * n / nFft;
                re += x * Math.Cos(phase);
                im += x * Math.Sin(phase);
            }
            Assert.Equal(re, planes.Re[t][k], tolerance: 1e-4);
            Assert.Equal(im, planes.Im[t][k], tolerance: 1e-4);
        }
    }

    [Fact]
    public void ZeroPadding_IsStillTheDefault_AndDiffersFromReflectAtTheEdges()
    {
        const int nFft = 16;
        const int hop = 4;
        var signal = Enumerable.Range(0, 40).Select(i => (float)Math.Sin(i * 0.7) + 0.3f).ToArray();
        var frames = BsRoformerIoBinding.FramesFor(signal.Length, hop);
        var zero = new SpectrumPlanes(frames, nFft, hop);
        var explicitZero = new SpectrumPlanes(frames, nFft, hop);
        var reflect = new SpectrumPlanes(frames, nFft, hop);
        var work = new StftWorkspace(nFft, signal.Length + nFft, frames);

        Stft.Forward(signal, zero, work, periodic: true);
        Stft.Forward(signal, explicitZero, work, periodic: true, StftPadding.Zero);
        Stft.Forward(signal, reflect, work, periodic: true, StftPadding.Reflect);

        Assert.Equal(zero.Re[0], explicitZero.Re[0]);
        Assert.NotEqual(zero.Re[0][0], reflect.Re[0][0]);
        // A frame that never reaches the padding is identical either way.
        Assert.Equal(zero.Re[frames / 2], reflect.Re[frames / 2]);
    }

    [Fact]
    public void ReflectPadding_RoundTripsThroughTheInverse()
    {
        const int nFft = 2048;
        const int hop = 512;
        var random = new Random(345);
        var signal = Enumerable.Range(0, 20_000).Select(_ => (float)(random.NextDouble() - 0.5)).ToArray();
        var frames = BsRoformerIoBinding.FramesFor(signal.Length, hop);
        var planes = new SpectrumPlanes(frames, nFft, hop);
        var work = new StftWorkspace(nFft, signal.Length + nFft, frames);

        Stft.Forward(signal, planes, work, periodic: true, StftPadding.Reflect);
        var back = new float[signal.Length];
        Stft.Inverse(planes, back, work, periodic: true);

        for (var i = 0; i < signal.Length; i++) Assert.Equal(signal[i], back[i], tolerance: 1e-5);
    }

    [Fact]
    public void ReflectPadding_RefusesASignalNoLongerThanHalfTheTransform()
    {
        var planes = new SpectrumPlanes(BsRoformerIoBinding.FramesFor(8, 4), 16, 4);
        var work = new StftWorkspace(16, 8 + 16, planes.FrameCount);
        Assert.Throws<ArgumentException>(() =>
            Stft.Forward(new float[8], planes, work, periodic: true, StftPadding.Reflect));
    }

    private static float Reflect(float[] x, int i)
    {
        var n = x.Length;
        if (i < 0) i = -i;
        if (i >= n) i = 2 * (n - 1) - i;
        return x[i];
    }

    // ------------------------------------------------------------ catalogue

    /// <summary>A BS-RoFormer row whose declared layout the driver cannot drive is a
    /// download that can never separate (the MrCitron shape, #254).</summary>
    [Fact]
    public void EveryBsRoformerRow_DeclaresALayoutTheDriverSupports()
    {
        var rows = OnnxModelCatalog.All.Where(e => e.Kind == OnnxStemModelKind.BsRoformer).ToArray();
        Assert.NotEmpty(rows);
        Assert.All(rows, e =>
        {
            Assert.NotNull(e.BsRoformerLayout);
            Assert.Contains(e.BsRoformerLayout.Value, BsRoformerIoBinding.SupportedLayouts);
        });
        Assert.All(OnnxModelCatalog.All.Where(e => e.Kind != OnnxStemModelKind.BsRoformer),
            e => Assert.Null(e.BsRoformerLayout));
    }

    /// <summary>
    /// The SW row: the driver kind that can load its layout, a pinned hash and exact size
    /// (the file is a single third-party re-host, so the pin is the only thing that notices
    /// a swap), six stems, and a licence note that says the weights' licence is unknown.
    /// </summary>
    [Fact]
    public void TheSwCatalogueRow_IsPinned_SixStem_DrivenByBsRoformer_AndSaysItsLicenceIsUnknown()
    {
        var entry = OnnxModelCatalog.FindById("bs-roformer-sw-6s");

        Assert.NotNull(entry);
        OnnxModelCatalog.ValidateEntry(entry);
        Assert.Equal(OnnxStemModelKind.BsRoformer, entry.Kind);
        Assert.Equal(BsRoformerIoLayout.SplitComplex, entry.BsRoformerLayout);
        Assert.Equal("d3d2bac77a7023282cb5f35a5807179e34076b60589867b572275f1a8ec36444", entry.Sha256);
        Assert.Equal(352_778_874, entry.SizeBytes);
        Assert.Equal(6, entry.SourceCount);
        Assert.Equal(BsRoformerIoBinding.SwStemOrder.Count, entry.SourceCount);
        // The row's driver must accept the layout the file declares (the shapes above).
        Assert.Equal(entry.SourceCount, BsRoformerIoBinding.Classify(SwInputs, SwOutputs).Stems);
        Assert.Contains("lfs.oid", entry.HashSource, StringComparison.Ordinal);
        Assert.Contains("UNKNOWN", entry.LicenceNote, StringComparison.Ordinal);
        Assert.Contains("enerjazzer/BS-ROFO-SW-Fixed", entry.LicenceNote, StringComparison.Ordinal);
        Assert.All(entry.Urls, u => Assert.StartsWith("https://huggingface.co/elicwhite/", u, StringComparison.Ordinal));
    }
}
