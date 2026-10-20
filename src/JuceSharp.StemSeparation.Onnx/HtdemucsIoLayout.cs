using Microsoft.ML.OnnxRuntime;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// The two tensor layouts an htdemucs v4 ONNX export comes in. The model is the
/// same network either way; what differs is where the STFT runs.
/// </summary>
public enum HtdemucsIoLayout
{
    /// <summary>
    /// The host computes the spectrogram. Two inputs — the waveform
    /// <c>[1, 2, 343980]</c> and the CAC spectrogram <c>[1, 4, 2048, 336]</c> — and
    /// two outputs: per-stem spectrograms <c>[1, stems, 4, 2048, 336]</c> (the host
    /// inverts them) and per-stem waveforms <c>[1, stems, 2, 343980]</c>; a stem is
    /// <c>istft(spec) + wave</c>. Shape of <c>arjune123/demucs-onnx</c>'s
    /// <c>htdemucs_6s.onnx</c> (opset 17).
    /// </summary>
    WaveformAndSpectrogram,

    /// <summary>
    /// The STFT, its inverse and the final branch sum are all INSIDE the graph.
    /// One input <c>input [batch, 2, time]</c>, one output
    /// <c>output [batch, stems, 2, time]</c> — the finished stems. Shape of
    /// <c>MrCitron/demucs-v4-onnx</c>'s <c>htdemucs.onnx</c> and
    /// <c>htdemucs_ft.onnx</c> (opset 14, measured 2026-10-05: the stem and
    /// time dimensions are symbolic, the channel dimension resolves to 2 at run
    /// time). The graph reflect-pads its input to the training segment, so the
    /// driver still feeds exactly <see cref="HtdemucsOnnxInference.Segment"/>
    /// samples per chunk — a much shorter input fails inside the graph's Pad.
    /// </summary>
    WaveformOnly,
}

/// <summary>
/// Which tensor plays which role in one loaded htdemucs graph, decided from the
/// graph's declared input/output shapes alone — so the decision is a pure
/// function a test can drive without a model file.
/// </summary>
/// <param name="Layout">Which layout the graph has.</param>
/// <param name="WaveInput">The waveform input's name.</param>
/// <param name="SpecInput">The spectrogram input's name; <c>null</c> for
///   <see cref="HtdemucsIoLayout.WaveformOnly"/>.</param>
/// <param name="WaveOutput">The per-stem waveform output's name.</param>
/// <param name="SpecOutput">The per-stem spectrogram output's name; <c>null</c>
///   for <see cref="HtdemucsIoLayout.WaveformOnly"/>.</param>
/// <param name="DeclaredStems">The stem dimension as the graph declares it, or
///   <c>0</c> when it is symbolic and only the first run can say.</param>
public sealed record HtdemucsIoBinding(
    HtdemucsIoLayout Layout,
    string WaveInput,
    string? SpecInput,
    string WaveOutput,
    string? SpecOutput,
    int DeclaredStems)
{
    /// <summary>
    /// Every layout <see cref="HtdemucsOnnxInference"/> can drive. Spelled out,
    /// not <c>Enum.GetValues</c>: a layout added to the enum is not supported
    /// until the driver can run it, and the catalogue lint reads this list.
    /// </summary>
    public static IReadOnlyList<HtdemucsIoLayout> SupportedLayouts { get; } =
        [HtdemucsIoLayout.WaveformAndSpectrogram, HtdemucsIoLayout.WaveformOnly];

    /// <summary>Classifies a loaded session.</summary>
    /// <exception cref="InvalidOperationException">The graph matches no supported layout.</exception>
    public static HtdemucsIoBinding FromSession(InferenceSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        return Classify(
            session.InputNames.Select(n => (n, session.InputMetadata[n].Dimensions)).ToArray(),
            session.OutputNames.Select(n => (n, session.OutputMetadata[n].Dimensions)).ToArray());
    }

    /// <summary>
    /// Classifies a graph from its declared tensor shapes. A dimension of
    /// <c>-1</c> (or <c>0</c>) is symbolic — ORT reports a named dimension
    /// such as <c>batch</c> or <c>time</c> that way.
    /// </summary>
    /// <exception cref="InvalidOperationException">The graph matches no supported
    ///   layout. The message lists every tensor's shape, so a new export's
    ///   failure says what it actually has.</exception>
    public static HtdemucsIoBinding Classify(
        IReadOnlyList<(string Name, int[] Dims)> inputs,
        IReadOnlyList<(string Name, int[] Dims)> outputs)
    {
        ArgumentNullException.ThrowIfNull(inputs);
        ArgumentNullException.ThrowIfNull(outputs);

        if (inputs.Count == 1 && outputs.Count == 1)
        {
            var (inName, inDims) = inputs[0];
            var (outName, outDims) = outputs[0];
            if (inDims.Length == 3 && Fits(inDims[1], 2) && Fits(inDims[2], HtdemucsOnnxInference.Segment) &&
                outDims.Length == 4 && Fits(outDims[2], 2) && Fits(outDims[3], HtdemucsOnnxInference.Segment))
                return new HtdemucsIoBinding(HtdemucsIoLayout.WaveformOnly, inName, null, outName, null,
                    Math.Max(0, outDims[1]));
        }

        if (inputs.Count == 2 && outputs.Count == 2)
        {
            string? wave = null, spec = null, specOut = null, waveOut = null;
            var declaredStems = 0;
            foreach (var (name, dims) in inputs)
            {
                if (dims.Length == 3 && dims[^1] == HtdemucsOnnxInference.Segment) wave = name;
                else if (dims.Length == 4) spec = name;
            }
            foreach (var (name, dims) in outputs)
            {
                if (dims.Length == 5)
                {
                    specOut = name;
                    declaredStems = Math.Max(0, dims[1]);
                }
                else if (dims.Length == 4)
                {
                    waveOut = name;
                }
            }
            if (wave != null && spec != null && specOut != null && waveOut != null)
                return new HtdemucsIoBinding(HtdemucsIoLayout.WaveformAndSpectrogram, wave, spec, waveOut, specOut,
                    declaredStems);
        }

        throw new InvalidOperationException(
            "htdemucs ONNX has an input/output layout this driver cannot run. Supported: " +
            $"one input [batch, 2, {HtdemucsOnnxInference.Segment}] -> one output [batch, stems, 2, time] " +
            $"(STFT inside the graph), or the waveform [1, 2, {HtdemucsOnnxInference.Segment}] plus the " +
            $"spectrogram [1, 4, {HtdemucsOnnxInference.FreqBins}, {HtdemucsOnnxInference.TimeFrames}] -> " +
            "per-stem spectrogram + waveform outputs. This graph has inputs " + Describe(inputs) +
            " and outputs " + Describe(outputs) + ".");
    }

    private static bool Fits(int declared, int required) => declared <= 0 || declared == required;

    private static string Describe(IReadOnlyList<(string Name, int[] Dims)> tensors) =>
        tensors.Count == 0
            ? "(none)"
            : string.Join(", ", tensors.Select(t =>
                $"{t.Name} [{string.Join(", ", t.Dims.Select(d => d <= 0 ? "?" : d.ToString(System.Globalization.CultureInfo.InvariantCulture)))}]"));
}
