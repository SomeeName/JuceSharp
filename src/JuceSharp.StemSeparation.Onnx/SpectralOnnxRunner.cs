using Microsoft.ML.OnnxRuntime;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// The single-input / single-output ONNX call the two spectral drivers
/// (BS-RoFormer, MDX-Net) both make, in the form that neither boxes a tensor
/// per chunk nor ignores cancellation.
/// </summary>
/// <remarks>
/// <para><b>Why not <c>NamedOnnxValue.CreateFromTensor</c>.</b> That overload
/// takes a fresh <c>DenseTensor&lt;float&gt;</c> per chunk — for BS-RoFormer at
/// 4 x 3073 x 256 that is a 12 MB Large Object Heap allocation every chunk, plus
/// an indexer call per element to fill it. Pinning one reusable array as an
/// <see cref="OrtValue"/> costs neither.</para>
/// <para><b>Why <see cref="RunOptions.Terminate"/>.</b> Without it the only
/// cancel point in a separation is the top of the chunk loop, and one chunk is
/// seconds of inference — a user Cancel appeared to do nothing until the current
/// chunk finished. Terminate makes <c>Run</c> abandon the graph; it returns
/// early rather than throwing, so the caller must check the token afterwards.</para>
/// </remarks>
internal sealed class SpectralOnnxRunner
{
    /// <summary>
    /// The ONE cancellation callback either ONNX driver hands a token.
    ///
    /// <para>
    /// Setting <see cref="RunOptions.Terminate"/> is a P/Invoke into ORT and
    /// can throw <c>OnnxRuntimeException</c>. A throw out of a cancellation
    /// callback propagates from <c>CancellationTokenSource.Cancel</c> — on the
    /// CANCELLING thread, wrapped in an <see cref="AggregateException"/> — or,
    /// when the token is already cancelled at registration, straight out of
    /// <c>Register</c> on this one. Either way the consumer sees an ORT type
    /// where <c>IStemSeparator</c> promises
    /// <see cref="OperationCanceledException"/>, and a host with a "retry on
    /// CPU" branch fires it on a user cancel. Cancellation is best-effort by
    /// construction: if ORT will not take the hint, the run finishes the
    /// current chunk and the caller's own
    /// <c>ThrowIfCancellationRequested</c> reports the cancel correctly.
    /// </para>
    /// </summary>
    internal static readonly Action<object?> RequestTerminate = state =>
    {
        try
        {
            if (state is RunOptions options) options.Terminate = true;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(
                $"SpectralOnnxRunner: RunOptions.Terminate threw, ignoring: {ex.Message}");
        }
    };

    private readonly InferenceSession _session;
    private readonly string[] _inputNames;
    private readonly string[] _outputNames;
    private float[] _input = [];

    public SpectralOnnxRunner(InferenceSession session, string inputName, string outputName)
        : this(session, [inputName], [outputName])
    {
    }

    /// <summary>A runner over several named inputs and outputs — the split-complex
    /// BS-RoFormer layout feeds <c>spec_real</c> and <c>spec_imag</c> and reads two
    /// outputs back.</summary>
    public SpectralOnnxRunner(InferenceSession session, IReadOnlyList<string> inputNames, IReadOnlyList<string> outputNames)
    {
        _session = session;
        _inputNames = [.. inputNames];
        _outputNames = [.. outputNames];
    }

    /// <summary>A reusable <c>[1, 4, dimF, dimT]</c> input buffer, zeroed.</summary>
    public float[] InputBuffer(int dimF, int dimT)
    {
        var needed = 4 * dimF * dimT;
        if (_input.Length < needed) _input = new float[needed];
        Array.Clear(_input, 0, needed);
        return _input;
    }

    /// <summary>
    /// Packs two spectrograms into the channel-first <c>[1, 4, dimF, dimT]</c>
    /// layout the models expect: <c>[re_L, im_L, re_R, im_R]</c>.
    /// </summary>
    public static void PackInput(float[] dst, SpectrumPlanes left, SpectrumPlanes right,
                                 int dimF, int dimT, int frames, int bins)
    {
        var plane = dimF * dimT;
        var t = Math.Min(dimT, frames);
        var fMax = Math.Min(dimF, bins);
        for (var i = 0; i < t; i++)
        {
            var reL = left.Re[i];
            var imL = left.Im[i];
            var reR = right.Re[i];
            var imR = right.Im[i];
            for (var f = 0; f < fMax; f++)
            {
                var idx = f * dimT + i;
                dst[0 * plane + idx] = reL[f];
                dst[1 * plane + idx] = imL[f];
                dst[2 * plane + idx] = reR[f];
                dst[3 * plane + idx] = imR[f];
            }
        }
    }

    /// <summary>
    /// Unpacks one stem's four output planes (<c>re_L, im_L, re_R, im_R</c>,
    /// starting at <paramref name="baseChannel"/>) out of a channel-first
    /// <c>[1, C, outF, outT]</c> tensor.
    ///
    /// <para>
    /// <b>The loop bounds come from the OUTPUT shape, not the input's.</b> Both
    /// spectral drivers iterated <c>t &lt; dimT</c> — the INPUT time dimension —
    /// while indexing <c>f * outT + t</c>; BS-RoFormer's shape is probed at
    /// runtime (a dynamic axis falls back to the computed frame count), so
    /// <c>outT &lt; dimT</c> is reachable, and at that point every
    /// <c>t &gt;= outT</c> read the NEXT bin's samples and the last bin read
    /// past the end of the span: cross-bin garbage in the stems, or an
    /// <see cref="IndexOutOfRangeException"/> deep inside inference. The planes
    /// are zero-initialised, so stopping at the real extent leaves silence.
    /// </para>
    /// </summary>
    public static void UnpackOutput(
        ReadOnlySpan<float> output,
        SpectrumPlanes left, SpectrumPlanes right,
        int dimF, int dimT, int bins, int outF, int outT, int baseChannel = 0)
    {
        if (outF <= 0 || outT <= 0) return;
        var plane = outF * outT;
        var needed = (long)(baseChannel + 4) * plane;
        if (needed > output.Length)
            throw new InvalidOperationException(
                $"ONNX output holds {output.Length} floats but stem {baseChannel / 4} needs " +
                $"{needed} at [{outF} x {outT}]. The model's output shape does not match its declared one.");

        var tMax = Math.Min(dimT, outT);
        var fMax = Math.Min(Math.Min(dimF, bins), outF);
        for (var t = 0; t < tMax; t++)
        {
            var reL = left.Re[t];
            var imL = left.Im[t];
            var reR = right.Re[t];
            var imR = right.Im[t];
            for (var f = 0; f < fMax; f++)
            {
                var idx = f * outT + t;
                reL[f] = output[(baseChannel + 0) * plane + idx];
                imL[f] = output[(baseChannel + 1) * plane + idx];
                reR[f] = output[(baseChannel + 2) * plane + idx];
                imR[f] = output[(baseChannel + 3) * plane + idx];
            }
        }
    }

    /// <summary>
    /// Packs two spectrograms into the split-complex layout: <paramref name="re"/> and
    /// <paramref name="im"/> each <c>[1, 2, dimF, dimT]</c>, channel-major then bin then
    /// frame (<c>(c * dimF + f) * dimT + t</c>) — what <c>torch.stft</c> over a
    /// <c>[2, samples]</c> batch produces and what BS-RoFormer SW's export takes.
    /// </summary>
    public static void PackSplit(float[] re, float[] im, SpectrumPlanes left, SpectrumPlanes right,
                                 int dimF, int dimT)
    {
        var plane = dimF * dimT;
        var needed = 2 * plane;
        if (re.Length < needed || im.Length < needed)
            throw new ArgumentException($"Split input buffers need {needed} floats each.", nameof(re));
        Array.Clear(re, 0, needed);
        Array.Clear(im, 0, needed);
        var t = Math.Min(dimT, Math.Min(left.FrameCount, right.FrameCount));
        var fMax = Math.Min(dimF, left.BinCount);
        for (var i = 0; i < t; i++)
        {
            var reL = left.Re[i];
            var imL = left.Im[i];
            var reR = right.Re[i];
            var imR = right.Im[i];
            for (var f = 0; f < fMax; f++)
            {
                var idx = f * dimT + i;
                re[idx] = reL[f];
                im[idx] = imL[f];
                re[plane + idx] = reR[f];
                im[plane + idx] = imR[f];
            }
        }
    }

    /// <summary>
    /// Unpacks one (stem, channel) spectrogram out of the split-complex outputs, each
    /// <c>[1, stems, 2, dimF, dimT]</c>: element <c>[0, s, c, f, t]</c> is at
    /// <c>((s * 2 + c) * dimF + f) * dimT + t</c>. Every bin and frame of
    /// <paramref name="dest"/> within <c>[dimF, dimT]</c> is overwritten, so a reused
    /// destination needs no clearing.
    /// </summary>
    public static void UnpackSplit(ReadOnlySpan<float> re, ReadOnlySpan<float> im, int stem, int channel,
                                   SpectrumPlanes dest, int dimF, int dimT)
    {
        var plane = dimF * dimT;
        var offset = (long)(stem * 2 + channel) * plane;
        if (offset + plane > re.Length || offset + plane > im.Length)
            throw new InvalidOperationException(
                $"ONNX output holds {Math.Min(re.Length, im.Length)} floats but stem {stem} channel {channel} " +
                $"needs {offset + plane} at [{dimF} x {dimT}]. The model's output shape does not match its declared one.");
        var tMax = Math.Min(dimT, dest.FrameCount);
        var fMax = Math.Min(dimF, dest.BinCount);
        var o = (int)offset;
        for (var t = 0; t < tMax; t++)
        {
            var dRe = dest.Re[t];
            var dIm = dest.Im[t];
            for (var f = 0; f < fMax; f++)
            {
                var idx = o + f * dimT + t;
                dRe[f] = re[idx];
                dIm[f] = im[idx];
            }
        }
    }

    /// <summary>
    /// Runs the session over the pinned input. The caller must dispose the
    /// result before the next call, and must not let a span from it escape.
    /// </summary>
    public IDisposableReadOnlyCollection<OrtValue> Run(float[] input, int dimF, int dimT, CancellationToken ct)
        => Run([input], [1, 4, dimF, dimT], ct);

    /// <summary>
    /// Runs the session over several pinned inputs of one shape, one per input name, in
    /// name order. Same disposal rule as the single-input form.
    /// </summary>
    public IDisposableReadOnlyCollection<OrtValue> Run(IReadOnlyList<float[]> inputs, long[] shape, CancellationToken ct)
    {
        if (inputs.Count != _inputNames.Length)
            throw new ArgumentException(
                $"{inputs.Count} input buffer(s) for {_inputNames.Length} input name(s).", nameof(inputs));

        var values = new OrtValue[inputs.Count];
        try
        {
            for (var i = 0; i < values.Length; i++)
                values[i] = OrtValue.CreateTensorValueFromMemory(inputs[i], shape);
            using var runOpts = new RunOptions();
            using var reg = ct.Register(RequestTerminate, runOpts);

            var results = _session.Run(runOpts, _inputNames, values, _outputNames);
            if (ct.IsCancellationRequested)
            {
                results.Dispose();
                ct.ThrowIfCancellationRequested();
            }
            return results;
        }
        finally
        {
            foreach (var value in values) value?.Dispose();
        }
    }
}
