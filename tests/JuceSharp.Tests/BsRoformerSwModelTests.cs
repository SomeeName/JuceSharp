using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;
using JuceSharp.AudioIo;
using JuceSharp.StemSeparation.Onnx;
using Microsoft.ML.OnnxRuntime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// BS-RoFormer SW on the REAL model, against a <c>reference.npz</c> of PyTorch fp32 output
/// for one chunk (input audio, its spectrogram, the six masked output spectrograms and their
/// iSTFT). Every test SKIPS unless <c>JUCESHARP_BSROFORMER_SW_DIR</c> names a folder holding
/// the model and <c>reference.npz</c> — neither is in the repo (350 MB + 50-160 MB), and a
/// default gate must not download them. Either export works: the 4 s third-party file with
/// the <c>reference.npz</c> from its repository, or the 13.35 s training-chunk export (#267)
/// with the one <c>tools/bs-roformer-sw-export/make_ref.py</c> writes. The chunk geometry is
/// read from the npz, not assumed.
/// <list type="bullet">
///   <item><c>JUCESHARP_BSROFORMER_SW_DIR</c> — the folder.</item>
///   <item><c>JUCESHARP_BSROFORMER_SW_MODEL</c> — the file name in it (default
///         <c>bs_roformer_sw_6stem_fp16.onnx</c>; the fp32 file is the alternative).</item>
///   <item><c>JUCESHARP_BSROFORMER_SW_THREADS</c> — CPU intra-op threads (default 4).</item>
///   <item><c>JUCESHARP_BSROFORMER_SW_WAV</c> — a stereo WAV for the stems-sum check; its
///         first 20 s are used (from <c>JUCESHARP_BSROFORMER_SW_WAV_OFFSET</c> seconds in).
///         Without it a synthetic mix is.</item>
///   <item><c>JUCESHARP_BSROFORMER_SW_GPU=1</c> — also run the DirectML parity theory.
///         Off by default: it loads the GPU.</item>
/// </list>
/// Each test appends its numbers to <c>parity-report.txt</c> in the folder.
/// </summary>
public sealed class BsRoformerSwModelTests
{
    private const string DirVar = "JUCESHARP_BSROFORMER_SW_DIR";

    internal static readonly string? Dir = Environment.GetEnvironmentVariable(DirVar);

    internal static string ModelPath => Path.Combine(Dir!,
        Environment.GetEnvironmentVariable("JUCESHARP_BSROFORMER_SW_MODEL") is { Length: > 0 } name
            ? name
            : "bs_roformer_sw_6stem_fp16.onnx");

    internal static string ReferencePath => Path.Combine(Dir!, "reference.npz");

    internal static int Threads =>
        int.TryParse(Environment.GetEnvironmentVariable("JUCESHARP_BSROFORMER_SW_THREADS"), out var n) && n > 0 ? n : 4;

    internal static void SkipUnlessPresent()
    {
        if (string.IsNullOrWhiteSpace(Dir))
            Assert.Skip($"{DirVar} is not set; the BS-RoFormer SW model tests need the model and reference.npz.");
        if (!File.Exists(ModelPath)) Assert.Skip($"No model at {ModelPath}.");
        if (!File.Exists(ReferencePath)) Assert.Skip($"No reference.npz at {ReferencePath}.");
    }

    private static SessionOptions CpuOptions() => new()
    {
        GraphOptimizationLevel = GraphOptimizationLevel.ORT_ENABLE_ALL,
        IntraOpNumThreads = Threads,
        InterOpNumThreads = 1,
        LogSeverityLevel = OrtLoggingLevel.ORT_LOGGING_LEVEL_ERROR,
    };

    // ------------------------------------------------------------ the IO contract

    [Fact]
    public void TheModel_DeclaresTheSplitComplexLayout_AndItsMetadataAgrees()
    {
        SkipUnlessPresent();
        using var options = CpuOptions();
        using var session = new InferenceSession(ModelPath, options);

        var layout = BsRoformerIoBinding.FromSession(session);
        var meta = session.ModelMetadata.CustomMetadataMap;
        Report("io", $"inputs: {Describe(session.InputMetadata)}; outputs: {Describe(session.OutputMetadata)}; " +
                     $"metadata: {string.Join(", ", meta.Select(kv => $"{kv.Key}={kv.Value}"))}");

        Assert.Equal(BsRoformerIoLayout.SplitComplex, layout.Layout);
        Assert.Equal(OnnxModelCatalog.FindById("bs-roformer-sw-6s")!.BsRoformerLayout, layout.Layout);
        Assert.Equal(["spec_real", "spec_imag"], layout.InputNames);
        Assert.Equal(["out_spec_real", "out_spec_imag"], layout.OutputNames);
        Assert.Equal(6, layout.Stems);
        Assert.Equal(1025, layout.Bins);
        var npz = Npz.Load(ReferencePath);
        Assert.Equal(npz["spec_real"].Shape[3], layout.Frames);

        var engine = new BsRoformerInference(session);
        Assert.Equal(2048, engine.Geometry.NFft);
        Assert.Equal(512, engine.Geometry.Hop);
        Assert.Equal(layout.Frames, BsRoformerIoBinding.FramesFor(engine.Geometry.ChunkSize, 512));
        Assert.Equal(BsRoformerIoBinding.SwStemOrder, engine.StemNames);
    }

    // ------------------------------------------------------------ CPU parity

    /// <summary>Our reflect-padded STFT of the reference audio against torch's.</summary>
    [Fact]
    public void OurStft_OfTheReferenceAudio_MatchesTorchs()
    {
        SkipUnlessPresent();
        var npz = Npz.Load(ReferencePath);
        var audio = npz["input_audio"].Data; // [1, 2, 176400]
        var specRe = npz["spec_real"].Data;  // [1, 2, 1025, T]
        var specIm = npz["spec_imag"].Data;
        var samples = npz["input_audio"].Shape[2];
        var frames = npz["spec_real"].Shape[3];
        const int bins = 1025;

        var work = new StftWorkspace(2048, samples + 2048, frames);
        var re = new float[2 * bins * frames];
        var im = new float[2 * bins * frames];
        var left = new SpectrumPlanes(frames, 2048, 512);
        var right = new SpectrumPlanes(frames, 2048, 512);
        Stft.Forward(audio.AsSpan(0, samples), left, work, periodic: true, StftPadding.Reflect);
        Stft.Forward(audio.AsSpan(samples, samples), right, work, periodic: true, StftPadding.Reflect);
        SpectralOnnxRunner.PackSplit(re, im, left, right, bins, frames);

        var errRe = RelativeError(specRe, re);
        var errIm = RelativeError(specIm, im);
        Report("stft", $"relative L2 real={errRe:E3} imag={errIm:E3}; max abs real={MaxAbsDiff(specRe, re):E3}");
        Assert.True(errRe < 1e-4 && errIm < 1e-4, $"STFT off torch's: real {errRe:E3}, imag {errIm:E3}");
    }

    /// <summary>The graph itself on the CPU provider, fed the reference spectrogram.</summary>
    [Fact]
    public void TheGraph_OnTheCpu_MatchesTheReferenceSpectrogramsPerStem()
    {
        SkipUnlessPresent();
        var npz = Npz.Load(ReferencePath);
        using var options = CpuOptions();
        using var session = new InferenceSession(ModelPath, options);

        var (outRe, outIm) = RunGraph(session, npz);
        var (worst, vsMix) = CompareStems(npz, outRe, outIm, "cpu-graph");

        AssertParity(worst, vsMix, "cpu-graph");
    }

    /// <summary>One whole chunk through OUR pipeline — reflect STFT, the graph, our iSTFT —
    /// against the reference's own iSTFT output (<c>ref_audio</c>).</summary>
    [Fact]
    public void OneChunk_ThroughOurStftAndIstft_MatchesTheReferenceAudioPerStem()
    {
        SkipUnlessPresent();
        var npz = Npz.Load(ReferencePath);
        var audio = npz["input_audio"].Data;
        var refAudio = npz["ref_audio"].Data; // [1, 6, 2, 176400]
        var samples = npz["input_audio"].Shape[2];
        using var options = CpuOptions();
        using var session = new InferenceSession(ModelPath, options);
        var engine = new BsRoformerInference(session);

        var stems = engine.ProcessChunkSplit(audio[..samples], audio[samples..(2 * samples)], CancellationToken.None);

        var worst = 0.0;
        var vsMix = 0.0;
        var mixEnergy = Energy(audio.AsSpan(0, 2 * samples));
        var lines = new List<string>();
        for (var s = 0; s < stems.Length; s++)
        {
            var expectedL = refAudio.AsSpan((s * 2 + 0) * samples, samples).ToArray();
            var expectedR = refAudio.AsSpan((s * 2 + 1) * samples, samples).ToArray();
            var err = Math.Max(RelativeError(expectedL, stems[s].L), RelativeError(expectedR, stems[s].R));
            var stemShare = (Energy(expectedL) + Energy(expectedR)) / mixEnergy;
            if (stemShare >= AudibleStemShare) worst = Math.Max(worst, err);
            vsMix = Math.Max(vsMix,
                Math.Sqrt((ErrorEnergy(expectedL, stems[s].L) + ErrorEnergy(expectedR, stems[s].R)) / mixEnergy));
            lines.Add($"{engine.StemNames[s]} {err:E3} ({Snr(expectedL, stems[s].L):F1} dB)");
        }
        Report("cpu-chunk-audio", "relative L2 (SNR) per stem: " + string.Join(", ", lines) +
            $"; worst error {-20 * Math.Log10(Math.Max(vsMix, 1e-12)):F1} dB below the mix");
        AssertParity(worst, vsMix, "cpu-chunk-audio");
    }

    /// <summary>
    /// The six stems sum back to the mix. BS-RoFormer's masks are not constrained to sum to
    /// one, so this is a bound, not an identity: it catches a dropped stem, a swapped
    /// channel, a chunk misplaced in the overlap-add or a scale error, any of which costs
    /// tens of dB.
    /// </summary>
    [Fact]
    public async Task TheSixStems_SumBackToTheMix()
    {
        SkipUnlessPresent();
        var (mix, rate, source) = Mix(seconds: 20);
        using var separator = new OnnxStemSeparator(ModelPath, OnnxStemModelKind.BsRoformer,
            OnnxExecutionProvider.Cpu, intraOpNumThreads: Threads);
        var started = System.Diagnostics.Stopwatch.StartNew();
        var stems = await separator.SeparateAsync(mix, rate, ct: TestContext.Current.CancellationToken);
        started.Stop();

        Assert.Equal(BsRoformerIoBinding.SwStemOrder, stems.Select(s => s.Name));
        Assert.All(stems, s => Assert.Equal(mix.Length, s.InterleavedStereo.Length));
        Assert.All(stems, s => Assert.Equal(rate, s.SampleRate));

        var sum = new float[mix.Length];
        foreach (var stem in stems)
        for (var i = 0; i < sum.Length; i++) sum[i] += stem.InterleavedStereo[i];
        var snr = Snr(mix, sum);
        var energies = string.Join(", ", stems.Select(s => $"{s.Name} {Rms(s.InterleavedStereo):E2}"));
        Report("sum", $"{source}, {mix.Length / 2 / (double)rate:F1} s at {rate} Hz: sum-vs-mix SNR {snr:F1} dB; " +
                      $"CPU ({Threads} threads) {started.Elapsed.TotalSeconds:F1} s; stem RMS: {energies}");
        Assert.True(snr > 25, $"the stems sum to the mix at only {snr:F1} dB SNR");
    }

    /// <summary>
    /// A chunk of digital silence gives six silent, FINITE stems. The first fp16 build of the
    /// training-chunk export did not: its norm floor (1e-12) overflowed fp16 as a reciprocal,
    /// 0 * inf made every output of an all-zero chunk NaN, and the driver's scrub turned six of
    /// the test song's 36 chunks into silence - which the single-chunk parity check, on music,
    /// never saw. That was on DirectML; the CPU provider happened to stay finite on the same
    /// file, so <see cref="BsRoformerSwDirectMlTests"/> checks a silent chunk too, and that is
    /// the check that fails on the broken build.
    /// </summary>
    [Fact]
    public void ASilentChunk_GivesSilence_NotNaN()
    {
        SkipUnlessPresent();
        var npz = Npz.Load(ReferencePath);
        using var options = CpuOptions();
        using var session = new InferenceSession(ModelPath, options);

        var (outRe, outIm) = RunSilence(session, npz);

        Assert.All(outRe, v => Assert.Equal(0f, v));
        Assert.All(outIm, v => Assert.Equal(0f, v));
    }

    // ------------------------------------------------------------ helpers

    internal static (float[] Re, float[] Im) RunGraph(InferenceSession session, Npz npz)
    {
        var shape = npz["spec_real"].Shape.Select(d => (long)d).ToArray();
        using var re = OrtValue.CreateTensorValueFromMemory(npz["spec_real"].Data, shape);
        using var im = OrtValue.CreateTensorValueFromMemory(npz["spec_imag"].Data, shape);
        using var runOptions = new RunOptions();
        using var results = session.Run(runOptions, ["spec_real", "spec_imag"], [re, im],
            ["out_spec_real", "out_spec_imag"]);
        return (results[0].GetTensorDataAsSpan<float>().ToArray(), results[1].GetTensorDataAsSpan<float>().ToArray());
    }

    /// <summary>A stem is judged by its relative error only when it carries at least this
    /// share of the mix's energy (-30 dB). A near-silent stem - the piano of a song without
    /// one - is all rounding, and its relative error says nothing; the error-against-the-mix
    /// bound covers it.</summary>
    internal const double AudibleStemShare = 1e-3;

    /// <summary>The parity bar: every audible stem within 2e-2 relative L2 (34 dB) of PyTorch
    /// fp32, and no stem's error above 1e-2 of the MIX (40 dB below it). The fp16
    /// training-chunk export measures 50-61 dB per audible stem and 62-66 dB below the mix over
    /// a whole song on DirectML; PyTorch's own autocast is 56-69 dB.</summary>
    internal static void AssertParity(double worstAudible, double worstVsMix, string label)
    {
        Assert.True(worstAudible < 2e-2, $"{label}: worst audible-stem relative L2 {worstAudible:E3} against PyTorch");
        Assert.True(worstVsMix < 1e-2, $"{label}: worst stem error {worstVsMix:E3} of the mix against PyTorch");
    }

    internal static (double WorstAudible, double WorstVsMix) CompareStems(Npz npz, float[] outRe, float[] outIm, string label)
    {
        var refRe = npz["out_spec_real"].Data;
        var refIm = npz["out_spec_imag"].Data;
        Assert.Equal(refRe.Length, outRe.Length);
        var mixEnergy = Energy(npz["spec_real"].Data) + Energy(npz["spec_imag"].Data);
        var perStem = refRe.Length / 6;
        var worst = 0.0;
        var vsMix = 0.0;
        var lines = new List<string>();
        for (var s = 0; s < 6; s++)
        {
            var rr = refRe.AsSpan(s * perStem, perStem);
            var ri = refIm.AsSpan(s * perStem, perStem);
            var oRe = outRe.AsSpan(s * perStem, perStem);
            var oIm = outIm.AsSpan(s * perStem, perStem);
            var rel = Math.Max(RelativeError(rr, oRe), RelativeError(ri, oIm));
            var maxAbs = Math.Max(MaxAbsDiff(rr, oRe), MaxAbsDiff(ri, oIm));
            var share = (Energy(rr) + Energy(ri)) / mixEnergy;
            var err = Math.Sqrt((ErrorEnergy(rr, oRe) + ErrorEnergy(ri, oIm)) / mixEnergy);
            if (share >= AudibleStemShare) worst = Math.Max(worst, rel);
            vsMix = Math.Max(vsMix, err);
            lines.Add($"{BsRoformerIoBinding.SwStemOrder[s]} relL2 {rel:E3} ({-20 * Math.Log10(Math.Max(rel, 1e-12)):F1} dB), " +
                      $"{-20 * Math.Log10(Math.Max(err, 1e-12)):F1} dB below the mix, maxAbs {maxAbs:E2}");
        }
        Report(label, string.Join("; ", lines) + $" (reference max |x| {MaxAbs(refRe):F3})");
        return (worst, vsMix);
    }

    /// <summary>The graph fed an all-zero spectrogram of the reference's shape.</summary>
    internal static (float[] Re, float[] Im) RunSilence(InferenceSession session, Npz npz)
    {
        var shape = npz["spec_real"].Shape.Select(d => (long)d).ToArray();
        var zeros = new float[npz["spec_real"].Data.Length];
        using var re = OrtValue.CreateTensorValueFromMemory(zeros, shape);
        using var im = OrtValue.CreateTensorValueFromMemory((float[])zeros.Clone(), shape);
        using var runOptions = new RunOptions();
        using var results = session.Run(runOptions, ["spec_real", "spec_imag"], [re, im],
            ["out_spec_real", "out_spec_imag"]);
        return (results[0].GetTensorDataAsSpan<float>().ToArray(), results[1].GetTensorDataAsSpan<float>().ToArray());
    }

    internal static double Energy(ReadOnlySpan<float> a)
    {
        double e = 0;
        foreach (var v in a) e += (double)v * v;
        return e;
    }

    internal static double ErrorEnergy(ReadOnlySpan<float> expected, ReadOnlySpan<float> actual)
    {
        double e = 0;
        for (var i = 0; i < expected.Length; i++)
        {
            var d = (double)expected[i] - actual[i];
            e += d * d;
        }
        return e;
    }

    internal static (float[] Mix, int Rate, string Source) Mix(int seconds)
    {
        var wav = Environment.GetEnvironmentVariable("JUCESHARP_BSROFORMER_SW_WAV");
        if (!string.IsNullOrWhiteSpace(wav) && File.Exists(wav))
        {
            var buffer = WavIo.Read(wav);
            Assert.Equal(2, buffer.Channels);
            var offsetSeconds = int.TryParse(Environment.GetEnvironmentVariable("JUCESHARP_BSROFORMER_SW_WAV_OFFSET"),
                out var o) && o > 0 ? o : 0;
            var start = Math.Min(buffer.Interleaved.Length, offsetSeconds * buffer.SampleRate * 2);
            var take = Math.Min(buffer.Interleaved.Length - start, seconds * buffer.SampleRate * 2);
            return (buffer.Interleaved[start..(start + take)], buffer.SampleRate,
                $"{Path.GetFileName(wav)} from {offsetSeconds} s");
        }

        const int rate = 44_100;
        var random = new Random(258);
        var mix = new float[seconds * rate * 2];
        for (var n = 0; n < mix.Length / 2; n++)
        {
            var t = n / (double)rate;
            var kick = n % (rate / 2) < 3000 ? Math.Sin(2 * Math.PI * 55 * t) * Math.Exp(-(n % (rate / 2)) / 600.0) : 0;
            var hat = n % (rate / 4) < 400 ? (random.NextDouble() - 0.5) * 0.3 : 0;
            var bass = 0.25 * Math.Sin(2 * Math.PI * 82.4 * t);
            var guitar = 0.12 * Math.Sign(Math.Sin(2 * Math.PI * 196 * t)) * (0.6 + 0.4 * Math.Sin(t));
            mix[2 * n] = (float)(0.5 * kick + hat + bass + guitar);
            mix[2 * n + 1] = (float)(0.5 * kick + hat + bass + 0.7 * guitar);
        }
        return (mix, rate, "synthetic mix");
    }

    internal static double RelativeError(ReadOnlySpan<float> expected, ReadOnlySpan<float> actual)
    {
        double num = 0, den = 0;
        for (var i = 0; i < expected.Length; i++)
        {
            var d = (double)expected[i] - actual[i];
            num += d * d;
            den += (double)expected[i] * expected[i];
        }
        return Math.Sqrt(num / Math.Max(den, 1e-30));
    }

    internal static double Snr(ReadOnlySpan<float> expected, ReadOnlySpan<float> actual)
        => -20 * Math.Log10(Math.Max(RelativeError(expected, actual), 1e-12));

    internal static double MaxAbsDiff(ReadOnlySpan<float> a, ReadOnlySpan<float> b)
    {
        var m = 0.0;
        for (var i = 0; i < a.Length; i++) m = Math.Max(m, Math.Abs((double)a[i] - b[i]));
        return m;
    }

    internal static double MaxAbs(ReadOnlySpan<float> a)
    {
        var m = 0.0;
        foreach (var v in a) m = Math.Max(m, Math.Abs(v));
        return m;
    }

    internal static double Rms(float[] a) => Math.Sqrt(a.Sum(v => (double)v * v) / Math.Max(1, a.Length));

    internal static string Describe(IReadOnlyDictionary<string, NodeMetadata> tensors) =>
        string.Join(", ", tensors.Select(kv => $"{kv.Key} [{string.Join(',', kv.Value.Dimensions)}] {kv.Value.ElementDataType}"));

    internal static void Report(string label, string line)
    {
        var text = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} [{Path.GetFileName(ModelPath)}] {label}: {line}";
        TestContext.Current.TestOutputHelper?.WriteLine(text);
        File.AppendAllText(Path.Combine(Dir!, "parity-report.txt"), text + Environment.NewLine);
    }

    /// <summary>A NumPy <c>.npz</c> of little-endian float32 arrays (the export's reference
    /// format), read without a NumPy dependency. Integer arrays are skipped.</summary>
    internal sealed class Npz
    {
        private readonly Dictionary<string, (float[] Data, int[] Shape)> _arrays;

        private Npz(Dictionary<string, (float[] Data, int[] Shape)> arrays) => _arrays = arrays;

        public (float[] Data, int[] Shape) this[string name] => _arrays[name];

        public static Npz Load(string path)
        {
            var arrays = new Dictionary<string, (float[], int[])>(StringComparer.Ordinal);
            using var zip = ZipFile.OpenRead(path);
            foreach (var entry in zip.Entries)
            {
                using var stream = entry.Open();
                using var memory = new MemoryStream();
                stream.CopyTo(memory);
                var bytes = memory.ToArray();
                if (bytes.Length < 10 || bytes[0] != 0x93 || Encoding.ASCII.GetString(bytes, 1, 5) != "NUMPY")
                    throw new InvalidDataException($"{entry.Name} is not a .npy array.");
                var major = bytes[6];
                var headerLength = major == 1 ? BitConverter.ToUInt16(bytes, 8) : (int)BitConverter.ToUInt32(bytes, 8);
                var headerStart = major == 1 ? 10 : 12;
                var header = Encoding.ASCII.GetString(bytes, headerStart, headerLength);
                if (!header.Contains("'<f4'", StringComparison.Ordinal)) continue;
                Assert.Contains("'fortran_order': False", header, StringComparison.Ordinal);
                var shape = Regex.Match(header, @"'shape': \(([^)]*)\)").Groups[1].Value
                    .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                    .Select(d => int.Parse(d, CultureInfo.InvariantCulture)).ToArray();
                var data = new float[(bytes.Length - headerStart - headerLength) / 4];
                Buffer.BlockCopy(bytes, headerStart + headerLength, data, 0, data.Length * 4);
                Assert.Equal(shape.Aggregate(1, (a, b) => a * b), data.Length);
                arrays[Path.GetFileNameWithoutExtension(entry.Name)] = (data, shape);
            }
            return new Npz(arrays);
        }
    }
}

/// <summary>
/// BS-RoFormer SW on DirectML. Opt-in on top of <c>JUCESHARP_BSROFORMER_SW_DIR</c>:
/// <c>JUCESHARP_BSROFORMER_SW_GPU=1</c>, because it loads the GPU. Runs on the hardware
/// adapter with the most dedicated video memory, the one the app's picker ranks first.
/// <c>JUCESHARP_BSROFORMER_SW_SONG</c> names a whole stereo WAV for the full-song run.
/// </summary>
public sealed class BsRoformerSwDirectMlTests
{
    private const string GpuVar = "JUCESHARP_BSROFORMER_SW_GPU";

    private static OnnxGpuAdapter Strongest()
    {
        BsRoformerSwModelTests.SkipUnlessPresent();
        if (Environment.GetEnvironmentVariable(GpuVar) != "1")
            Assert.Skip($"{GpuVar}=1 is not set; the DirectML runs load the GPU and are opt-in.");
        var adapter = OnnxGpuAdapters.Enumerate()
            .Where(a => !a.IsSoftware)
            .OrderByDescending(a => a.DedicatedVideoMemoryBytes)
            .FirstOrDefault();
        if (adapter is null) Assert.Skip("No hardware DirectX 12 adapter.");
        return adapter;
    }

    /// <summary>
    /// The graph on DirectML against the PyTorch reference, with graph fusion OFF, ON, and as
    /// SHIPPED (<see cref="OnnxSessionFactory.ApplyDirectMlOptions"/> with the model's own
    /// <see cref="OnnxSessionFactory.DirectMlGraphFusionKey"/>, read the way
    /// <see cref="OnnxStemSeparator"/> reads it), at ORT_ENABLE_ALL and ORT_ENABLE_BASIC.
    /// The shipped rows must pass, and so must the fusion-ON rows for a model that DECLARES
    /// itself verified with fusion: that declaration is exactly what this test checks. Fusion
    /// on a model without it is measured and reported (it broke htdemucs, #151, and the 4 s
    /// SW export). A silent chunk must come back finite. Opt-in
    /// (<c>JUCESHARP_BSROFORMER_SW_GPU=1</c>): it loads the GPU.
    /// </summary>
    [Theory]
    [InlineData("shipped", "ORT_ENABLE_ALL")]
    [InlineData("shipped", "ORT_ENABLE_BASIC")]
    [InlineData("off", "ORT_ENABLE_ALL")]
    [InlineData("on", "ORT_ENABLE_ALL")]
    [Trait("Category", "Gpu")]
    public void TheGraph_OnDirectMl_MatchesTheReference(string fusion, string level)
    {
        var adapter = Strongest();
        var npz = BsRoformerSwModelTests.Npz.Load(BsRoformerSwModelTests.ReferencePath);
        var verified = OnnxSessionFactory.AllowsDirectMlGraphFusion(OnnxModelMetadata.Read(BsRoformerSwModelTests.ModelPath));
        using var options = new SessionOptions
        {
            GraphOptimizationLevel = Enum.Parse<GraphOptimizationLevel>(level),
            LogSeverityLevel = OrtLoggingLevel.ORT_LOGGING_LEVEL_ERROR,
        };
        var fusionOn = fusion switch
        {
            "on" => true,
            "off" => false,
            _ => verified,
        };
        OnnxSessionFactory.ApplyDirectMlOptions(options, allowGraphFusion: fusionOn);
        options.AppendExecutionProvider_DML(adapter.DeviceId);
        using var session = new InferenceSession(BsRoformerSwModelTests.ModelPath, options);

        var started = System.Diagnostics.Stopwatch.StartNew();
        var (outRe, outIm) = BsRoformerSwModelTests.RunGraph(session, npz);
        var first = started.Elapsed;
        started.Restart();
        _ = BsRoformerSwModelTests.RunGraph(session, npz);
        var second = started.Elapsed;
        var label = $"dml {fusion} (fusion {(fusionOn ? "on" : "off")}) {level} on {adapter.Name}";
        var (worst, vsMix) = BsRoformerSwModelTests.CompareStems(npz, outRe, outIm, label);
        var (silentRe, silentIm) = BsRoformerSwModelTests.RunSilence(session, npz);
        var silentNonFinite = silentRe.Count(v => !float.IsFinite(v)) + silentIm.Count(v => !float.IsFinite(v));
        BsRoformerSwModelTests.Report(label, $"first chunk {first.TotalMilliseconds:F0} ms, second " +
            $"{second.TotalMilliseconds:F0} ms; silent chunk: {silentNonFinite} non-finite outputs");

        if (fusion == "shipped" || fusion == "off" || verified)
        {
            BsRoformerSwModelTests.AssertParity(worst, vsMix, label);
            Assert.Equal(0, silentNonFinite);
        }
    }

    /// <summary>
    /// A whole song through the SHIPPED path (<see cref="OnnxStemSeparator"/> on DirectML,
    /// the factory's session options): it must stay on DirectML, the six stems must sum
    /// back to the mix, and the wall time is reported. Peak video memory is sampled
    /// outside the test (nvidia-smi), since DirectML exposes none.
    /// </summary>
    [Fact]
    [Trait("Category", "Gpu")]
    public async Task AWholeSong_OnDirectMl_StaysOnTheGpu_AndItsStemsSumToTheMix()
    {
        var adapter = Strongest();
        var song = Environment.GetEnvironmentVariable("JUCESHARP_BSROFORMER_SW_SONG");
        if (string.IsNullOrWhiteSpace(song) || !File.Exists(song))
            Assert.Skip("JUCESHARP_BSROFORMER_SW_SONG does not name a WAV.");
        var buffer = WavIo.Read(song);
        Assert.Equal(2, buffer.Channels);

        var log = new List<string>();
        var load = System.Diagnostics.Stopwatch.StartNew();
        using var separator = new OnnxStemSeparator(BsRoformerSwModelTests.ModelPath, OnnxStemModelKind.BsRoformer,
            OnnxExecutionProvider.DirectML, line => { lock (log) log.Add(line); }, deviceId: adapter.DeviceId);
        load.Stop();
        Assert.Equal(OnnxExecutionProvider.DirectML, separator.ActiveExecutionProvider);
        Assert.Null(separator.FallbackReason);

        var run = System.Diagnostics.Stopwatch.StartNew();
        var stems = await separator.SeparateAsync(buffer.Interleaved, buffer.SampleRate,
            ct: TestContext.Current.CancellationToken);
        run.Stop();

        var mix = buffer.Interleaved;
        var sum = new float[mix.Length];
        foreach (var stem in stems)
        for (var i = 0; i < sum.Length; i++) sum[i] += stem.InterleavedStereo[i];
        var snr = BsRoformerSwModelTests.Snr(mix, sum);
        var seconds = mix.Length / 2.0 / buffer.SampleRate;
        BsRoformerSwModelTests.Report($"dml full song on {adapter.Name}",
            $"{Path.GetFileName(song)}, {seconds:F1} s at {buffer.SampleRate} Hz: session load {load.Elapsed.TotalSeconds:F1} s, " +
            $"separation {run.Elapsed.TotalSeconds:F1} s ({seconds / run.Elapsed.TotalSeconds:F1}x real time), " +
            $"sum-vs-mix SNR {snr:F1} dB; stem RMS: " +
            string.Join(", ", stems.Select(s => $"{s.Name} {BsRoformerSwModelTests.Rms(s.InterleavedStereo):E2}")) +
            "; " + string.Join(" | ", log.Where(l => l.Contains("chunks of", StringComparison.Ordinal) ||
                                                    l.Contains("graph fusion", StringComparison.Ordinal))));

        Assert.Equal(BsRoformerIoBinding.SwStemOrder, stems.Select(s => s.Name));
        // PyTorch's own demix sums back at 32.8 dB (13.35 s chunks) / 33.3 dB (4 s) on the test
        // song; a silenced or NaN chunk, a wrong channel or a missing stem costs far more.
        Assert.True(snr > 30, $"the stems sum to the mix at only {snr:F1} dB SNR");
        Assert.Contains(log, l => l.Contains(" 0 non-finite model samples", StringComparison.Ordinal));
    }
}
