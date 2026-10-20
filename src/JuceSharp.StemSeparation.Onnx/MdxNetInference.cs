using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// MDX-Net inference loop for UVR-style ONNX models (UVR-MDX-NET variants
/// from <c>TRvlvr/model_repo</c>). Splits a stereo signal into instrumental
/// and vocals using sliding-window chunks of spectrogram, model prediction
/// of the instrumental spec, ISTFT, and overlap-add reassembly.
/// </summary>
/// <remarks>
/// <para>Model spec (Inst_HQ_3 and most UVR-MDX-NET variants):</para>
/// <list type="bullet">
///   <item>Input shape <c>[1, 4, dim_f, dim_t]</c> with channels
///         <c>[re_L, im_L, re_R, im_R]</c>.</item>
///   <item><c>n_fft = 6144</c>, <c>hop = 1024</c>, <c>dim_f = 3072</c>,
///         <c>dim_t = 256</c>.</item>
///   <item><c>chunk_size = hop * (dim_t - 1) = 261120</c> samples.</item>
///   <item>Model output: the predicted instrumental spectrogram with the
///         same shape; ISTFT yields the instrumental signal.</item>
/// </list>
/// <para>Vocals are recovered by subtracting the instrumental from the
/// original signal — perfect cancellation when the network is good.</para>
/// <para>This is a deliberately conservative first-pass implementation:
/// it works at <c>overlap = 2</c> (50%) which trades runtime for an
/// honest seam-free output. Tuning overlap = 4 or 8 is a follow-up.</para>
/// </remarks>
public sealed class MdxNetInference
{
    /// <summary>Geometry the MDX-Net model was exported with. The spectrogram U-Net has a
    /// FIXED input shape, so these must match the file or the run produces garbage.</summary>
    /// <param name="NFft">STFT transform size in samples.</param>
    /// <param name="Hop">STFT hop in samples.</param>
    /// <param name="DimF">Frequency bins the model consumes; the spectrogram is cropped or
    /// padded to this.</param>
    /// <param name="DimT">Time frames per inference, which fixes the chunk length.</param>
    /// <param name="Overlap">Overlap factor between chunks; 2 means 50%.</param>
    public sealed record Settings(int NFft = 6144, int Hop = 1024, int DimF = 3072, int DimT = 256, int Overlap = 2)
    {
        /// <summary>Samples per inference chunk per channel, derived as
        /// <c>Hop * (DimT - 1)</c> — the exact length whose STFT gives
        /// <see cref="DimT"/> frames.</summary>
        public int ChunkSize => Hop * (DimT - 1);
    }

    private readonly InferenceSession _session;
    private readonly Settings _cfg;
    private readonly string _inputName;
    private readonly string _outputName;

    /// <summary>
    /// Bind to a loaded MDX-Net session, resolving its input and output tensor names. The
    /// session is NOT owned — the caller disposes it.
    /// </summary>
    /// <param name="session">The loaded MDX-Net graph.</param>
    /// <param name="settings">STFT and chunk geometry; must match the exported model.
    /// Defaults when <c>null</c>.</param>
    public MdxNetInference(InferenceSession session, Settings? settings = null)
    {
        _session = session;
        _cfg = settings ?? new Settings();
        _inputName  = session.InputNames.Count > 0  ? session.InputNames[0]  : "input";
        _outputName = session.OutputNames.Count > 0 ? session.OutputNames[0] : "output";
    }

    /// <summary>
    /// Run the model over <paramref name="stereo"/> (length = frames * 2,
    /// interleaved L/R, sample rate 44100 expected) and return the
    /// predicted instrumental signal in the same layout.
    /// </summary>
    public float[] PredictInstrumental(float[] stereo, IProgress<double>? progress = null, CancellationToken ct = default)
    {
        return (_chunker ??= new OverlapAddChunker(_cfg.ChunkSize, _cfg.Overlap))
            .Run(stereo, (l, r) => ProcessChunk(l, r, ct), progress, ct);
    }

    private OverlapAddChunker? _chunker;

    private SpectralOnnxRunner Runner() => _runner ??= new SpectralOnnxRunner(_session, _inputName, _outputName);

    private SpectralOnnxRunner? _runner;

    /// <summary>Count of non-finite samples the MODEL produced, scrubbed on the
    /// way into the overlap-add. See <see cref="OverlapAddChunker"/>.</summary>
    public long NonFiniteOutputs => _chunker?.NonFiniteOutputs ?? 0;

    private (float[] L, float[] R) ProcessChunk(float[] chunkL, float[] chunkR, CancellationToken ct)
    {
        var specL = ForwardChunk(chunkL);
        var specR = ForwardChunk(chunkR, right: true);
        var frames = specL.FrameCount;
        var dimT = _cfg.DimT;
        var dimF = _cfg.DimF;

        // The class doc promises the chunk size lines up with the model shape.
        // It used to SILENTLY truncate instead: `if (frames > dimT) frames = dimT`
        // dropped the tail of every chunk, and `f < specL.BinCount` quietly
        // zero-filled any bin the model wanted and the transform did not have.
        // Both are configuration errors and both are inaudible until someone
        // wonders why the stems sound thin.
        if (frames > dimT)
            throw new InvalidOperationException(
                $"MDX-Net chunk produced {frames} STFT frames but the model takes {dimT}. " +
                $"ChunkSize ({_cfg.ChunkSize}) and Hop ({_cfg.Hop}) must agree with DimT.");
        if (dimF > specL.BinCount)
            throw new InvalidOperationException(
                $"MDX-Net model wants {dimF} frequency bins but nFft {_cfg.NFft} yields " +
                $"{specL.BinCount}. DimF must not exceed nFft/2 + 1.");

        // Build [1, 4, dimF, dimT]. PyTorch layout: channel-first.
        var runner = Runner();
        var input = runner.InputBuffer(dimF, dimT);
        SpectralOnnxRunner.PackInput(input, specL, specR, dimF, dimT, frames, specL.BinCount);

        using var results = runner.Run(input, dimF, dimT, ct);
        var output = results[0].GetTensorDataAsSpan<float>();
        var outShape = results[0].GetTensorTypeAndShape().Shape;
        var outT = (int)outShape[3];
        var outF = (int)outShape[2];

        // Decode output spec back to time-domain via ISTFT. The loop bounds
        // are the OUTPUT shape's — see SpectralOnnxRunner.UnpackOutput.
        var outSpecL = new SpectrumPlanes(dimT, _cfg.NFft, _cfg.Hop) { OriginalLength = chunkL.Length };
        var outSpecR = new SpectrumPlanes(dimT, _cfg.NFft, _cfg.Hop) { OriginalLength = chunkR.Length };
        var bins = _cfg.NFft / 2 + 1;
        SpectralOnnxRunner.UnpackOutput(output, outSpecL, outSpecR, dimF, dimT, bins, outF, outT);

        return (InverseChunk(outSpecL), InverseChunk(outSpecR));
    }


    // ---- STFT plumbing shared by the chunk paths -------------------------
    //
    // One workspace per channel, created on first use and reused for the whole
    // prediction: the transforms take caller scratch, so nothing per-FRAME is
    // allocated. Per-CHUNK is a different story and this comment used to read
    // as though it were not: each chunk still builds two SpectrumPlanes plus a
    // float[OriginalLength] per channel (~12 MB per MDX-Net chunk), and
    // OverlapAddChunker.Run allocates a one-element array per chunk. Only the
    // htdemucs driver is allocation-free at chunk granularity.
    // PERIODIC Hann throughout - torch.hann_window is what these models were
    // TRAINED with, and the symmetric (N-1) form this class used before is a
    // different window.

    private StftWorkspace? _workL;
    private StftWorkspace? _workR;
    private StftWorkspace? _workOut;

    private SpectrumPlanes ForwardChunk(float[] chunk, bool right = false)
    {
        var pad = _cfg.NFft / 2;
        var paddedLen = chunk.Length + 2 * pad;
        var frames = Math.Max(1, (paddedLen - _cfg.NFft) / _cfg.Hop + 1);
        var work = right
            ? _workR ??= new StftWorkspace(_cfg.NFft, paddedLen, frames)
            : _workL ??= new StftWorkspace(_cfg.NFft, paddedLen, frames);
        var planes = new SpectrumPlanes(frames, _cfg.NFft, _cfg.Hop);
        Stft.Forward(chunk, planes, work, periodic: true);
        return planes;
    }

    private float[] InverseChunk(SpectrumPlanes planes)
    {
        var needed = Math.Max(planes.OriginalLength + _cfg.NFft,
                              planes.FrameCount * _cfg.Hop + _cfg.NFft);
        if (_workOut is null || _workOut.Mono.Length < needed)
            _workOut = new StftWorkspace(_cfg.NFft, needed, planes.FrameCount);
        var dest = new float[planes.OriginalLength];
        Stft.Inverse(planes, dest, _workOut, periodic: true);
        return dest;
    }

}
