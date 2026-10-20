using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// BS-RoFormer inference for "no-STFT" ONNX exports: the graph takes a
/// pre-computed complex spectrogram and returns masked spectrograms, and the
/// STFT / iSTFT on either side are ours. Two export layouts are driven, told
/// apart from the graph's own metadata by <see cref="BsRoformerIoBinding"/>:
/// <list type="bullet">
///   <item><see cref="BsRoformerIoLayout.PackedChannels"/> — <c>ZFTurbo/MSS_ONNX_TensorRT</c>
///         against viperx's <c>model_bs_roformer_ep_317_sdr_12.9755.ckpt</c>: one
///         <c>[1, 4, F, T]</c> input, the vocal stem out, instrumental recovered by
///         subtraction.</item>
///   <item><see cref="BsRoformerIoLayout.SplitComplex"/> — <c>elicwhite/bs-roformer-web</c>'s
///         BS-RoFormer SW export: <c>spec_real</c> / <c>spec_imag</c> in, six stems
///         (bass, drums, other, vocals, guitar, piano) of <c>[1, 6, 2, F, T]</c> out, DC
///         bin already zeroed. n_fft 2048, hop 512, a FIXED chunk because the rotary
///         embedding was traced at that length: 176,400 samples (T = 345) for that export,
///         588,800 (T = 1151, the training chunk) for <c>tools/bs-roformer-sw-export</c>'s
///         (DotCharter #267). The spectrogram is <c>torch.stft(center=True)</c>'s, i.e.
///         REFLECT-padded at the chunk edges. Its geometry comes from the model's custom
///         metadata and is checked against the declared shapes
///         (<see cref="BsRoformerIoBinding.ResolveSplitGeometry"/>); the song loop is
///         MSST's own (<see cref="FadeWindowChunker"/>, <see cref="ChunkPipeline"/>).</item>
/// </list>
/// </summary>
/// <remarks>
/// <para><b>Packed layout status: experimental.</b> The canonical viperx config uses
/// <c>n_fft = 2048</c>, <c>hop = 441</c>, <c>chunk_size = 352800</c>
/// samples at 44.1 kHz, dim_freqs_in = 1025. We probe the actual ONNX
/// input shape at runtime and adapt; if the shape doesn't match the
/// well-known BS-RoFormer convention we log a clear error rather than
/// silently producing garbage.</para>
/// <para>This implementation deliberately uses 50% overlap and a Hann
/// window — heavier overlap is a future tuning knob.</para>
/// </remarks>
public sealed class BsRoformerInference
{
    /// <summary>Geometry the model was exported with. These are not free knobs: an
    /// exported BS-RoFormer graph has fixed STFT dimensions, and a mismatch produces
    /// garbage rather than an error.</summary>
    /// <param name="NFft">STFT transform size in samples.</param>
    /// <param name="Hop">STFT hop in samples.</param>
    /// <param name="ChunkSize">Samples per inference chunk, per channel.</param>
    /// <param name="Overlap">Overlap factor between chunks; 2 means 50%, which is what
    /// this implementation is tuned for.</param>
    public sealed record Settings(
        int NFft = 2048,
        int Hop = 441,
        int ChunkSize = 352800,
        int Overlap = 2);

    private readonly InferenceSession _session;
    private readonly Settings _cfg;
    private readonly string _inputName;
    private readonly string _outputName;
    private readonly int[] _inputShape;
    private readonly BsRoformerIoBinding _layout;
    private readonly IReadOnlyList<string> _stemNames;
    private readonly Action<string>? _log;

    /// <summary>
    /// Bind to a loaded ONNX session, resolving its input and output tensor names and
    /// reading back the declared input shape. The session is NOT owned — the caller
    /// disposes it.
    /// </summary>
    /// <param name="session">The loaded BS-RoFormer graph.</param>
    /// <param name="log">Receives one line describing the bound tensor names and shape.</param>
    /// <param name="settings">STFT and chunk geometry; must match what the model was
    /// exported with. Defaults when <c>null</c>.</param>
    /// <exception cref="NotSupportedException">The graph is neither BS-RoFormer layout, or
    /// a split-complex model's metadata contradicts its declared shapes.</exception>
    public BsRoformerInference(InferenceSession session, Action<string>? log = null, Settings? settings = null)
    {
        ArgumentNullException.ThrowIfNull(session);
        _session = session;
        _log = log;
        _layout = BsRoformerIoBinding.FromSession(session);
        _inputName = _layout.InputNames[0];
        _outputName = _layout.OutputNames[0];
        _inputShape = session.InputMetadata[_inputName].Dimensions.ToArray();

        if (_layout.Layout == BsRoformerIoLayout.SplitComplex)
        {
            var (resolved, names) = _layout.ResolveSplitGeometry(CustomMetadata(session));
            _cfg = settings ?? resolved;
            _stemNames = names;
            if (_layout.Frames > 0 && BsRoformerIoBinding.FramesFor(_cfg.ChunkSize, _cfg.Hop) != _layout.Frames)
            {
                throw new NotSupportedException(
                    $"BS-RoFormer ONNX: a {_cfg.ChunkSize}-sample chunk at hop {_cfg.Hop} does not give the " +
                    $"{_layout.Frames} frames the model was traced at.");
            }
        }
        else
        {
            _cfg = settings ?? new Settings();
            _stemNames = _layout.Stems == 1 ? ["vocals"] : [];
        }

        log?.Invoke($"BS-RoFormer {_layout.Layout}: inputs [{string.Join(", ", _layout.InputNames)}] " +
            $"shape=[{string.Join(',', _inputShape)}], outputs [{string.Join(", ", _layout.OutputNames)}], " +
            $"{_layout.Stems} stem(s), n_fft={_cfg.NFft}, hop={_cfg.Hop}, chunk={_cfg.ChunkSize}.");
    }

    /// <summary>The tensor layout the graph declares.</summary>
    public BsRoformerIoBinding Binding => _layout;

    /// <summary>The STFT and chunk geometry in use.</summary>
    public Settings Geometry => _cfg;

    /// <summary>Stem names in model output order for the split-complex layout (from the
    /// model's <c>stems</c> metadata, else BS-RoFormer SW's order). For a packed model,
    /// <c>vocals</c> for the single-stem one and empty otherwise (the caller names those).</summary>
    public IReadOnlyList<string> StemNames => _stemNames;

    private static IReadOnlyDictionary<string, string> CustomMetadata(InferenceSession session)
    {
        try
        {
            return new Dictionary<string, string>(session.ModelMetadata.CustomMetadataMap, StringComparer.Ordinal);
        }
        catch (OnnxRuntimeException)
        {
            return new Dictionary<string, string>();
        }
    }

    /// <summary>
    /// Run the model over <paramref name="stereo"/> (interleaved L/R,
    /// 44.1 kHz) and return the predicted vocal-stem signal.
    /// </summary>
    public float[] PredictVocals(float[] stereo, IProgress<double>? progress = null, CancellationToken ct = default)
    {
        return Chunker().Run(stereo, (l, r) => ProcessChunk(l, r, ct), progress, ct);
    }

    /// <summary>
    /// The pad / chunk / overlap-add loop, shared with the multi-stem path and
    /// with MDX-Net. Created once per instance so <see cref="NonFiniteOutputs"/>
    /// counts the whole prediction.
    /// </summary>
    private OverlapAddChunker Chunker()
        => _chunker ??= new OverlapAddChunker(_cfg.ChunkSize, _cfg.Overlap);

    private OverlapAddChunker? _chunker;

    private SpectralOnnxRunner Runner() => _runner ??= new SpectralOnnxRunner(_session, _inputName, _outputName);

    private SpectralOnnxRunner? _runner;

    /// <summary>Count of non-finite samples the MODEL produced, scrubbed on the
    /// way into the overlap-add. See <see cref="OverlapAddChunker"/>.</summary>
    public long NonFiniteOutputs => (_chunker?.NonFiniteOutputs ?? 0) + Interlocked.Read(ref _splitNonFinite);

    private (float[] L, float[] R) ProcessChunk(float[] chunkL, float[] chunkR, CancellationToken ct)
    {
        var specL = ForwardChunk(chunkL);
        var specR = ForwardChunk(chunkR, right: true);
        var frames = specL.FrameCount;
        var bins   = _cfg.NFft / 2 + 1;

        // BS-RoFormer ONNX exports from MSS_ONNX_TensorRT typically use the layout
        // [batch=1, stereo_channels*2_complex=4, freq=bins, time=frames] —
        // channels = [re_L, im_L, re_R, im_R] — the same convention as MDX-Net.
        // If the introspected shape disagrees, we adapt the time dimension only
        // (model is fully convolutional in time for BS-RoFormer).
        var dimT = ShapeOr(_inputShape, 3, frames);
        var dimF = ShapeOr(_inputShape, 2, bins);
        var nCh  = ShapeOr(_inputShape, 1, 4);
        if (nCh != 4)
            throw new InvalidOperationException(
                $"BS-RoFormer ONNX expects 4 input channels (re_L, im_L, re_R, im_R) but model reports {nCh}. " +
                "This variant is not yet supported.");

        var runner = Runner();
        var input = runner.InputBuffer(dimF, dimT);
        SpectralOnnxRunner.PackInput(input, specL, specR, dimF, dimT, frames, bins);

        using var results = runner.Run(input, dimF, dimT, ct);
        var output = results[0].GetTensorDataAsSpan<float>();
        var outShape = results[0].GetTensorTypeAndShape().Shape;
        var outT = (int)outShape[3];
        var outF = (int)outShape[2];

        var outSpecL = new SpectrumPlanes(dimT, _cfg.NFft, _cfg.Hop) { OriginalLength = chunkL.Length };
        var outSpecR = new SpectrumPlanes(dimT, _cfg.NFft, _cfg.Hop) { OriginalLength = chunkR.Length };
        // The OUTPUT shape bounds the loop: this model's time axis is probed at
        // runtime, so outT < dimT is reachable here in particular. See
        // SpectralOnnxRunner.UnpackOutput.
        SpectralOnnxRunner.UnpackOutput(output, outSpecL, outSpecR, dimF, dimT, bins, outF, outT);
        return (InverseChunk(outSpecL), InverseChunk(outSpecR));
    }

    /// <summary>
    /// Multi-stem variant. When the model emits more than 4 spectrogram
    /// channels (i.e. more than one stem), each consecutive 4-channel group
    /// is treated as one stem (re_L, im_L, re_R, im_R). Returns stems in
    /// model output order; the caller supplies the names.
    /// </summary>
    public IReadOnlyList<float[]> PredictAllStems(
        float[] stereo,
        IProgress<double>? progress = null,
        CancellationToken ct = default)
    {
        if (stereo.Length % 2 != 0)
            throw new ArgumentException("Input must be interleaved stereo.", nameof(stereo));
        if (_layout.Layout == BsRoformerIoLayout.SplitComplex)
            return PredictNamedStems(stereo, progress, ct).Select(s => s.Stereo).ToArray();

        // Probe output channel count from the model.
        var outMeta = _session.OutputMetadata[_outputName];
        var outChannels = outMeta.Dimensions.Length >= 2 && outMeta.Dimensions[1] > 0
            ? outMeta.Dimensions[1]
            : 4;
        if (outChannels % 4 != 0)
            throw new InvalidOperationException(
                $"BS-RoFormer ONNX output has {outChannels} channels; expected a multiple of 4 (4 per stem).");
        var numStems = outChannels / 4;

        if (numStems == 1)
            return new[] { PredictVocals(stereo, progress, ct) };

        return Chunker().RunMulti(stereo, numStems,
            (l, r) => ProcessChunkMulti(l, r, numStems, ct), progress, ct);
    }

    private (float[] L, float[] R)[] ProcessChunkMulti(float[] chunkL, float[] chunkR, int numStems, CancellationToken ct)
    {
        var specL = ForwardChunk(chunkL);
        var specR = ForwardChunk(chunkR, right: true);
        var fr   = specL.FrameCount;
        var bins = _cfg.NFft / 2 + 1;

        var dimT = ShapeOr(_inputShape, 3, fr);
        var dimF = ShapeOr(_inputShape, 2, bins);

        var runner = Runner();
        var input = runner.InputBuffer(dimF, dimT);
        SpectralOnnxRunner.PackInput(input, specL, specR, dimF, dimT, fr, bins);

        using var results = runner.Run(input, dimF, dimT, ct);
        var output = results[0].GetTensorDataAsSpan<float>();   // [1, numStems*4, F, T]
        var outShape = results[0].GetTensorTypeAndShape().Shape;
        var outT = (int)outShape[3];
        var outF = (int)outShape[2];

        var perStem = new (float[] L, float[] R)[numStems];
        for (var s = 0; s < numStems; s++)
        {
            var outSpecL = new SpectrumPlanes(dimT, _cfg.NFft, _cfg.Hop) { OriginalLength = chunkL.Length };
            var outSpecR = new SpectrumPlanes(dimT, _cfg.NFft, _cfg.Hop) { OriginalLength = chunkR.Length };
            SpectralOnnxRunner.UnpackOutput(output, outSpecL, outSpecR, dimF, dimT, bins, outF, outT,
                baseChannel: s * 4);
            perStem[s] = (InverseChunk(outSpecL), InverseChunk(outSpecR));
        }
        return perStem;
    }


    /// <summary>
    /// Every stem the model emits, named, in model output order. The split-complex
    /// layout's own path; a packed model goes through <see cref="PredictAllStems"/> and is
    /// named <c>stem_N</c> unless it is the single-stem vocal model.
    /// </summary>
    /// <param name="stereo">Interleaved L/R at 44.1 kHz.</param>
    /// <param name="progress">0..1 progress, one tick per chunk.</param>
    /// <param name="ct">Cancellation; also interrupts a running chunk.</param>
    /// <returns>(name, interleaved stereo) per stem, each as long as the input.</returns>
    public IReadOnlyList<(string Name, float[] Stereo)> PredictNamedStems(
        float[] stereo, IProgress<double>? progress = null, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(stereo);
        if (stereo.Length % 2 != 0)
            throw new ArgumentException("Input must be interleaved stereo.", nameof(stereo));

        if (_layout.Layout != BsRoformerIoLayout.SplitComplex)
        {
            var packed = PredictAllStems(stereo, progress, ct);
            return packed
                .Select((s, i) => (_stemNames.Count == packed.Count ? _stemNames[i] : $"stem_{i + 1}", s))
                .ToArray();
        }

        var stems = PredictSplit(stereo, progress, ct);
        var named = new (string, float[])[stems.Length];
        for (var i = 0; i < stems.Length; i++) named[i] = (_stemNames[i], stems[i]);
        return named;
    }

    // ---- split-complex path (BS-RoFormer SW) ------------------------------
    //
    // The song goes through FadeWindowChunker (MSST's demix, sample for sample) and
    // ChunkPipeline: chunk k's model run on this thread, chunk k-1's unpack + twelve
    // iSTFTs + overlap-add on a pool thread at the same time, and chunk k+1's fill +
    // forward STFT on another (into the other input slot). The model's IO is bound
    // to arrays allocated once per song (SplitComplexIoBinding); the STFT planes,
    // workspaces and per-(stem, channel) audio buffers are allocated on first use and
    // reused for every chunk and every later song.

    private SpectrumPlanes? _splitInL;
    private SpectrumPlanes? _splitInR;
    private SpectrumPlanes[]? _splitOut;
    private StftWorkspace[]? _splitWork;
    private float[][]? _splitAudio;
    private long _splitNonFinite;

    /// <summary>Where the time of the last split-complex song went: chunks, the model stage
    /// (the ONNX run, plus the first chunk's fill and forward STFT; later ones are prefetched
    /// during the previous run), the host stage (unpack, iSTFT, overlap-add; it
    /// overlaps the next chunk's model stage) and the wall time. <c>null</c> before the
    /// first song.</summary>
    internal (int Chunks, TimeSpan Model, TimeSpan Host, TimeSpan Wall)? LastSplitTiming { get; private set; }

    private float[][] PredictSplit(float[] stereo, IProgress<double>? progress, CancellationToken ct)
    {
        var bins = _cfg.NFft / 2 + 1;
        var size = _cfg.ChunkSize;
        var frames = BsRoformerIoBinding.FramesFor(size, _cfg.Hop);
        var pass = new FadeWindowChunker(size, _cfg.Overlap).Begin(stereo, _layout.Stems);
        using var io = new SplitComplexIoBinding(_session, _layout, bins, frames);
        var chunkL = new float[size];
        var chunkR = new float[size];
        long modelTicks = 0, hostTicks = 0;
        var wall = System.Diagnostics.Stopwatch.StartNew();
        Task? prefetch = null;

        void Prepare(int k, int slot)
        {
            pass.Fill(k, chunkL, chunkR);
            ForwardSplit(chunkL, chunkR, io, slot);
        }

        try
        {
            ChunkPipeline.Run(pass.Count,
                (k, slot) =>
                {
                    var t = System.Diagnostics.Stopwatch.GetTimestamp();
                    // Chunk k's input was prepared during chunk k-1's run; chunk k+1's is
                    // prepared, into the other input slot, during this one.
                    if (prefetch is null)
                    {
                        Prepare(k, slot);
                    }
                    else
                    {
                        var ready = prefetch;
                        prefetch = null;
                        ready.GetAwaiter().GetResult();
                    }
                    if (k + 1 < pass.Count)
                    {
                        var next = k + 1;
                        prefetch = Task.Run(() => Prepare(next, next & 1), CancellationToken.None);
                    }
                    io.Run(slot, ct);
                    modelTicks += System.Diagnostics.Stopwatch.GetTimestamp() - t;
                },
                (k, slot) =>
                {
                    var t = System.Diagnostics.Stopwatch.GetTimestamp();
                    pass.Accumulate(k, InverseSplit(io, slot, size));
                    hostTicks += System.Diagnostics.Stopwatch.GetTimestamp() - t;
                    progress?.Report((k + 1.0) / pass.Count);
                },
                ct);
        }
        finally
        {
            // Never leave a prefetch writing into the STFT state or the binding's arrays.
            if (prefetch != null)
            {
                try
                {
                    prefetch.Wait();
                }
                catch (AggregateException)
                {
                    // The exception already leaving is the one the caller sees.
                }
            }
        }

        var stems = pass.Finish();
        wall.Stop();
        Interlocked.Add(ref _splitNonFinite, pass.NonFiniteOutputs);
        LastSplitTiming = (pass.Count, Ticks(modelTicks), Ticks(hostTicks), wall.Elapsed);
        var timing = LastSplitTiming.Value;
        _log?.Invoke($"BS-RoFormer {_layout.Stems}-stem: {timing.Chunks} chunks of {size} samples in " +
            $"{timing.Wall.TotalSeconds:F1} s; model stage {timing.Model.TotalSeconds:F1} s, host stage " +
            $"{timing.Host.TotalSeconds:F1} s (overlapped with the next chunk's model stage); " +
            $"{pass.NonFiniteOutputs} non-finite model samples silenced.");
        return stems;

        static TimeSpan Ticks(long ticks) => TimeSpan.FromSeconds(ticks / (double)System.Diagnostics.Stopwatch.Frequency);
    }

    /// <summary>One split-complex chunk, without the chunker: reflect-padded STFT, the model,
    /// an iSTFT per (stem, channel). Internal so the parity test can compare ONE chunk against
    /// the export's reference. The returned arrays are reused by the next call.</summary>
    internal (float[] L, float[] R)[] ProcessChunkSplit(float[] chunkL, float[] chunkR, CancellationToken ct)
    {
        var bins = _cfg.NFft / 2 + 1;
        var frames = BsRoformerIoBinding.FramesFor(chunkL.Length, _cfg.Hop);
        using var io = new SplitComplexIoBinding(_session, _layout, bins, frames, slots: 1);
        ForwardSplit(chunkL, chunkR, io, 0);
        io.Run(0, ct);
        return InverseSplit(io, 0, chunkL.Length);
    }

    private void ForwardSplit(float[] chunkL, float[] chunkR, SplitComplexIoBinding io, int slot)
    {
        var bins = _cfg.NFft / 2 + 1;
        var frames = BsRoformerIoBinding.FramesFor(chunkL.Length, _cfg.Hop);
        var paddedLen = chunkL.Length + 2 * (_cfg.NFft / 2);
        _workL ??= new StftWorkspace(_cfg.NFft, paddedLen, frames);
        _workR ??= new StftWorkspace(_cfg.NFft, paddedLen, frames);
        _splitInL ??= new SpectrumPlanes(frames, _cfg.NFft, _cfg.Hop);
        _splitInR ??= new SpectrumPlanes(frames, _cfg.NFft, _cfg.Hop);
        Stft.Forward(chunkL, _splitInL, _workL, periodic: true, StftPadding.Reflect);
        Stft.Forward(chunkR, _splitInR, _workR, periodic: true, StftPadding.Reflect);
        SpectralOnnxRunner.PackSplit(io.InputRe[slot], io.InputIm[slot], _splitInL, _splitInR, bins, frames);
    }

    private (float[] L, float[] R)[] InverseSplit(SplitComplexIoBinding io, int slot, int length)
    {
        var bins = _cfg.NFft / 2 + 1;
        var frames = BsRoformerIoBinding.FramesFor(length, _cfg.Hop);
        var paddedLen = length + 2 * (_cfg.NFft / 2);
        var stems = _layout.Stems;
        var jobs = stems * 2;
        var planes = _splitOut ??= NewArray(jobs, () => new SpectrumPlanes(frames, _cfg.NFft, _cfg.Hop));
        var work = _splitWork ??= NewArray(jobs, () => new StftWorkspace(_cfg.NFft, paddedLen, frames));
        var audio = _splitAudio ??= NewArray(jobs, () => new float[length]);
        var outRe = io.OutputRe[slot];
        var outIm = io.OutputIm[slot];

        // Twelve independent (stem, channel) inverses, each with its own planes and workspace.
        Parallel.For(0, jobs, j =>
        {
            SpectralOnnxRunner.UnpackSplit(outRe, outIm, j / 2, j % 2, planes[j], bins, frames);
            planes[j].OriginalLength = length;
            Stft.Inverse(planes[j], audio[j], work[j], periodic: true);
        });

        var perStem = new (float[] L, float[] R)[stems];
        for (var s = 0; s < stems; s++) perStem[s] = (audio[2 * s], audio[2 * s + 1]);
        return perStem;
    }

    private static T[] NewArray<T>(int count, Func<T> create)
    {
        var items = new T[count];
        for (var i = 0; i < count; i++) items[i] = create();
        return items;
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

    private static int ShapeOr(int[] shape, int idx, int fallback)
    {
        if (idx < 0 || idx >= shape.Length) return fallback;
        var v = shape[idx];
        return v > 0 ? v : fallback;
    }

}
