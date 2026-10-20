using System.Globalization;
using System.Text.Json;
using Microsoft.ML.OnnxRuntime;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>How a BS-RoFormer ONNX export lays out its spectrogram tensors.</summary>
public enum BsRoformerIoLayout
{
    /// <summary>One input <c>[1, 4, F, T]</c> holding <c>re_L, im_L, re_R, im_R</c>, and one
    /// output <c>[1, 4 * stems, F, T]</c> in the same per-stem grouping. The
    /// <c>ZFTurbo/MSS_ONNX_TensorRT</c> "no-STFT" exports (the 2-stem vocal model).</summary>
    PackedChannels,

    /// <summary>Two inputs <c>spec_real</c> / <c>spec_imag</c>, each <c>[1, 2, F, T]</c>
    /// (channel, bin, frame), and two outputs <c>out_spec_real</c> / <c>out_spec_imag</c>,
    /// each <c>[1, stems, 2, F, T]</c>, already masked and with the DC bin zeroed. The
    /// <c>elicwhite/bs-roformer-web</c> exports (BS-RoFormer SW, 6 stems).</summary>
    SplitComplex,
}

/// <summary>
/// The tensor layout of a BS-RoFormer ONNX graph, read from its declared input and output
/// metadata rather than assumed.
/// <para>
/// <b>Why classify at all.</b> The driver used to assume the packed 4-channel layout and
/// fell back to "the computed frame count" for anything it did not recognise, so a model
/// with a different contract either threw deep inside a chunk or, worse, ran and produced
/// noise. The two exports this package drives differ in the NUMBER of inputs and outputs,
/// their rank and their channel order; <see cref="Classify"/> tells them apart from the
/// metadata alone and refuses anything else with the shapes in the message.
/// </para>
/// </summary>
/// <param name="Layout">Which of the two layouts.</param>
/// <param name="InputNames">Tensor names to feed: one for <see cref="BsRoformerIoLayout.PackedChannels"/>,
/// <c>[real, imag]</c> for <see cref="BsRoformerIoLayout.SplitComplex"/>.</param>
/// <param name="OutputNames">Tensor names to fetch, in the same order.</param>
/// <param name="Stems">Stems the graph emits.</param>
/// <param name="Bins">Frequency bins (<c>F</c>), or -1 when the axis is dynamic.</param>
/// <param name="Frames">Time frames per chunk (<c>T</c>), or -1 when the axis is dynamic.</param>
public sealed record BsRoformerIoBinding(
    BsRoformerIoLayout Layout,
    IReadOnlyList<string> InputNames,
    IReadOnlyList<string> OutputNames,
    int Stems,
    int Bins,
    int Frames)
{
    /// <summary>The stem order of BS-RoFormer SW, as its training config and the export's
    /// README list it. Used when a <see cref="BsRoformerIoLayout.SplitComplex"/> model carries
    /// no <c>stems</c> metadata of its own and emits six stems.</summary>
    public static IReadOnlyList<string> SwStemOrder { get; } = ["bass", "drums", "other", "vocals", "guitar", "piano"];

    /// <summary>Every layout <see cref="BsRoformerInference"/> can drive. A catalogue entry
    /// declaring anything else must not be offered; the catalogue lint reads this list.</summary>
    public static IReadOnlyList<BsRoformerIoLayout> SupportedLayouts { get; } =
        [BsRoformerIoLayout.PackedChannels, BsRoformerIoLayout.SplitComplex];

    /// <summary>Classify a loaded session's declared inputs and outputs.</summary>
    /// <param name="session">The loaded graph.</param>
    /// <returns>The layout.</returns>
    /// <exception cref="NotSupportedException">The graph matches neither layout.</exception>
    public static BsRoformerIoBinding FromSession(InferenceSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        return Classify(
            session.InputNames.Select(n => (n, session.InputMetadata[n].Dimensions)).ToArray(),
            session.OutputNames.Select(n => (n, session.OutputMetadata[n].Dimensions)).ToArray());
    }

    /// <summary>
    /// Classify a graph from its declared tensors. Pure, so every rule is testable without
    /// a model file. A dimension of 0 or less is a dynamic axis.
    /// </summary>
    /// <param name="inputs">Each input's name and declared dimensions, in graph order.</param>
    /// <param name="outputs">Each output's name and declared dimensions, in graph order.</param>
    /// <returns>The layout.</returns>
    /// <exception cref="NotSupportedException">The graph matches neither layout.</exception>
    public static BsRoformerIoBinding Classify(
        IReadOnlyList<(string Name, int[] Dims)> inputs,
        IReadOnlyList<(string Name, int[] Dims)> outputs)
    {
        ArgumentNullException.ThrowIfNull(inputs);
        ArgumentNullException.ThrowIfNull(outputs);

        if (inputs.Count == 1 && outputs.Count == 1)
            return ClassifyPacked(inputs[0], outputs[0], inputs, outputs);
        if (inputs.Count == 2 && outputs.Count == 2)
            return ClassifySplit(inputs, outputs);
        throw Unsupported($"{inputs.Count} input(s) and {outputs.Count} output(s)", inputs, outputs);
    }

    private static BsRoformerIoBinding ClassifyPacked(
        (string Name, int[] Dims) input, (string Name, int[] Dims) output,
        IReadOnlyList<(string Name, int[] Dims)> inputs, IReadOnlyList<(string Name, int[] Dims)> outputs)
    {
        if (input.Dims.Length != 4 || output.Dims.Length != 4)
            throw Unsupported("a single input and output that are not both rank 4", inputs, outputs);
        if (input.Dims[1] > 0 && input.Dims[1] != 4)
            throw Unsupported($"{input.Dims[1]} input channels where re_L, im_L, re_R, im_R (4) are expected",
                inputs, outputs);

        var outChannels = output.Dims[1] > 0 ? output.Dims[1] : 4;
        if (outChannels % 4 != 0)
            throw Unsupported($"{outChannels} output channels, not a multiple of 4 (4 per stem)", inputs, outputs);

        return new BsRoformerIoBinding(BsRoformerIoLayout.PackedChannels, [input.Name], [output.Name],
            outChannels / 4, Static(input.Dims[2]), Static(input.Dims[3]));
    }

    private static BsRoformerIoBinding ClassifySplit(
        IReadOnlyList<(string Name, int[] Dims)> inputs, IReadOnlyList<(string Name, int[] Dims)> outputs)
    {
        var (inRe, inIm) = RealThenImag(inputs);
        var (outRe, outIm) = RealThenImag(outputs);

        if (inRe.Dims.Length != 4 || inIm.Dims.Length != 4)
            throw Unsupported("two inputs that are not both rank 4 [1, 2, F, T]", inputs, outputs);
        if (outRe.Dims.Length != 5 || outIm.Dims.Length != 5)
            throw Unsupported("two outputs that are not both rank 5 [1, stems, 2, F, T]", inputs, outputs);
        if (!inRe.Dims.SequenceEqual(inIm.Dims) || !outRe.Dims.SequenceEqual(outIm.Dims))
            throw Unsupported("real and imaginary tensors of different shapes", inputs, outputs);
        if (inRe.Dims[1] > 0 && inRe.Dims[1] != 2)
            throw Unsupported($"{inRe.Dims[1]} input channels where stereo (2) is expected", inputs, outputs);
        if (outRe.Dims[2] > 0 && outRe.Dims[2] != 2)
            throw Unsupported($"{outRe.Dims[2]} output channels where stereo (2) is expected", inputs, outputs);
        if (outRe.Dims[1] <= 0)
            throw Unsupported("a dynamic stem axis, so the stem count is unknown", inputs, outputs);
        if (Mismatch(inRe.Dims[2], outRe.Dims[3]) || Mismatch(inRe.Dims[3], outRe.Dims[4]))
            throw Unsupported("outputs whose [F, T] differ from the inputs'", inputs, outputs);

        return new BsRoformerIoBinding(BsRoformerIoLayout.SplitComplex, [inRe.Name, inIm.Name], [outRe.Name, outIm.Name],
            outRe.Dims[1], Static(inRe.Dims[2]), Static(inRe.Dims[3]));
    }

    /// <summary>
    /// Orders a real/imaginary pair. The names decide when they say (<c>spec_real</c>,
    /// <c>out_spec_imag</c>); otherwise graph order does, real first. Feeding the
    /// imaginary plane as the real one would run without error and return noise, so a
    /// pair whose names contradict each other is refused rather than guessed.
    /// </summary>
    private static ((string Name, int[] Dims) Re, (string Name, int[] Dims) Im) RealThenImag(
        IReadOnlyList<(string Name, int[] Dims)> pair)
    {
        var a = pair[0];
        var b = pair[1];
        var aImag = IsImag(a.Name);
        var bImag = IsImag(b.Name);
        if (aImag && !bImag) return (b, a);
        if (aImag && bImag)
            throw new NotSupportedException(
                $"BS-RoFormer ONNX: both '{a.Name}' and '{b.Name}' read as imaginary; cannot tell which is real.");
        return (a, b);
    }

    private static bool IsImag(string name) =>
        name.Contains("imag", StringComparison.OrdinalIgnoreCase);

    private static bool Mismatch(int a, int b) => a > 0 && b > 0 && a != b;

    private static int Static(int dim) => dim > 0 ? dim : -1;

    private static NotSupportedException Unsupported(
        string what, IReadOnlyList<(string Name, int[] Dims)> inputs, IReadOnlyList<(string Name, int[] Dims)> outputs)
        => new($"BS-RoFormer ONNX layout not supported: {what}. Inputs: {Describe(inputs)}; outputs: {Describe(outputs)}.");

    private static string Describe(IReadOnlyList<(string Name, int[] Dims)> tensors) =>
        string.Join(", ", tensors.Select(t =>
            $"'{t.Name}' [{string.Join(',', t.Dims.Select(d => d.ToString(CultureInfo.InvariantCulture)))}]"));

    /// <summary>
    /// The STFT and chunk geometry of a <see cref="BsRoformerIoLayout.SplitComplex"/> model,
    /// and its stem names. The export records them as custom metadata (<c>n_fft</c>,
    /// <c>hop_length</c>, <c>chunk_samples</c>, <c>stems</c>, <c>sample_rate</c>); a key that is
    /// absent falls back to BS-RoFormer SW's values (n_fft 2048, hop 512, 176,400 samples).
    /// <para>
    /// <b>The frame count is CHECKED, not trusted.</b> The export's rotary embedding caches
    /// by sequence length, so the time axis is fixed at trace time (T = 345 for SW) even if
    /// it is declared dynamic. A centred STFT of <c>chunk</c> samples has
    /// <c>chunk / hop + 1</c> frames; a geometry that does not land on the declared T would
    /// run and return noise, so it throws here instead.
    /// </para>
    /// </summary>
    /// <param name="metadata">The model's custom metadata map; may be empty.</param>
    /// <param name="overlap">Chunk overlap factor handed to the chunker (2 = 50 %).</param>
    /// <returns>The settings and the stem names, in output order.</returns>
    /// <exception cref="NotSupportedException">The metadata contradicts the declared shapes,
    /// or the model is not at 44.1 kHz.</exception>
    public (BsRoformerInference.Settings Settings, IReadOnlyList<string> StemNames) ResolveSplitGeometry(
        IReadOnlyDictionary<string, string> metadata, int overlap = 2)
    {
        ArgumentNullException.ThrowIfNull(metadata);
        if (Layout != BsRoformerIoLayout.SplitComplex)
            throw new InvalidOperationException("Geometry metadata is only defined for the split-complex layout.");

        var nFft = Int(metadata, "n_fft") ?? 2048;
        var hop = Int(metadata, "hop_length") ?? 512;
        var rate = Int(metadata, "sample_rate") ?? OnnxStemSeparator.SpectralModelSampleRate;
        if (rate != OnnxStemSeparator.SpectralModelSampleRate)
            throw new NotSupportedException(
                $"BS-RoFormer ONNX: the model is at {rate} Hz; this driver runs spectral models at " +
                $"{OnnxStemSeparator.SpectralModelSampleRate} Hz.");
        if (nFft <= 0 || nFft % 2 != 0 || hop <= 0)
            throw new NotSupportedException($"BS-RoFormer ONNX: n_fft {nFft} / hop {hop} is not a usable STFT.");
        if (Bins > 0 && nFft / 2 + 1 != Bins)
            throw new NotSupportedException(
                $"BS-RoFormer ONNX: n_fft {nFft} gives {nFft / 2 + 1} bins but the model declares {Bins}.");

        var chunk = Int(metadata, "chunk_samples")
                    ?? (Frames > 0 && FramesFor(SwChunkSamples, hop) != Frames ? (Frames - 1) * hop : SwChunkSamples);
        if (chunk <= nFft / 2)
            throw new NotSupportedException($"BS-RoFormer ONNX: chunk of {chunk} samples is too short for n_fft {nFft}.");
        if (Frames > 0 && FramesFor(chunk, hop) != Frames)
            throw new NotSupportedException(
                $"BS-RoFormer ONNX: a {chunk}-sample chunk at hop {hop} gives {FramesFor(chunk, hop)} frames but " +
                $"the model declares {Frames}.");

        return (new BsRoformerInference.Settings(nFft, hop, chunk, overlap), StemNamesFrom(metadata));
    }

    /// <summary>BS-RoFormer SW's trace size: 4 s at 44.1 kHz, T = 345 at hop 512.</summary>
    public const int SwChunkSamples = 176_400;

    /// <summary>Frames a centred (<c>center=True</c>) STFT of <paramref name="samples"/> has.</summary>
    /// <param name="samples">Signal length.</param>
    /// <param name="hop">STFT hop.</param>
    /// <returns><c>samples / hop + 1</c>.</returns>
    public static int FramesFor(int samples, int hop) => samples / hop + 1;

    private IReadOnlyList<string> StemNamesFrom(IReadOnlyDictionary<string, string> metadata)
    {
        if (metadata.TryGetValue("stems", out var raw))
        {
            try
            {
                var names = JsonSerializer.Deserialize<string[]>(raw);
                if (names is { Length: > 0 } && names.Length == Stems && names.All(n => !string.IsNullOrWhiteSpace(n)))
                    return names.Select(n => n.Trim().ToLowerInvariant()).ToArray();
            }
            catch (JsonException)
            {
                // Fall through to the defaults below: a malformed tag is no worse than none.
            }
        }

        return Stems == SwStemOrder.Count
            ? SwStemOrder
            : Enumerable.Range(1, Stems).Select(i => $"stem_{i}").ToArray();
    }

    private static int? Int(IReadOnlyDictionary<string, string> metadata, string key) =>
        metadata.TryGetValue(key, out var raw) &&
        int.TryParse(raw.Trim().Trim('"'), NumberStyles.Integer, CultureInfo.InvariantCulture, out var v)
            ? v
            : null;
}
