using System.Buffers;
using System.Diagnostics;
using Microsoft.ML.OnnxRuntime;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Hybrid Transformer Demucs (htdemucs v4) inference for the standalone
/// ONNX exports that ship the time-domain branch and the spectrogram
/// branch in a single graph (e.g. <c>arjune123/demucs-onnx</c>,
/// <c>MrCitron/demucs-v4-onnx</c>, <c>rysertio/Demucs-onnx</c>).
/// Supports both the 4-stem and 6-stem variants — the stem count is read
/// from the output tensor shape at session bind (or, when the graph leaves it
/// symbolic, from the first chunk's output).
/// </summary>
/// <remarks>
/// <para>
/// <b>Two IO layouts</b> (<see cref="HtdemucsIoLayout"/>, classified by
/// <see cref="HtdemucsIoBinding.Classify"/>). <c>arjune123</c>'s
/// <c>htdemucs_6s.onnx</c> wants the host to compute the spectrogram (the
/// contract below). <c>MrCitron</c>'s <c>htdemucs.onnx</c> /
/// <c>htdemucs_ft.onnx</c> run the STFT inside the graph: one input
/// <c>input [batch, 2, time]</c>, one output <c>output [batch, stems, 2, time]</c>
/// that is already the finished stems — those chunks skip the forward STFT and
/// the per-stem iSTFT entirely, and go through the same margin/crossfade loop.
/// Until 2026-10-05 this class only knew the first layout, so the MrCitron
/// entries downloaded fine and then failed on bind (DotCharter #254).
/// </para>
/// <para>
/// <b>Two-input model contract</b> (verified against the arjune123 export):
/// </para>
/// <list type="bullet">
///   <item><c>input</c> — <c>[1, 2, 343980]</c> float32 (segment is
///         exactly <c>7.8s @ 44.1 kHz</c>; this is the canonical demucs
///         training segment length).</item>
///   <item>second input (name varies) — <c>[1, 4, 2048, 336]</c> float32:
///         the pre-computed complex spectrogram with the 4 channels
///         packed as <c>[Re_L, Im_L, Re_R, Im_R]</c>. The STFT keeps
///         <b>bin 0 (DC)</b> and drops bin 2048 (Nyquist), giving
///         <c>nFft/2 = 2048</c> bins. <c>Stft.ForwardHtdemucs</c>
///         implements the matching padding (<c>hop/2 * 3</c>) and frame
///         trimming.</item>
/// </list>
/// <para>
/// Outputs:
/// </para>
/// <list type="bullet">
///   <item>rank-5 — per-stem complex spectrograms <c>[1, stems, 4, 2048, 336]</c>.</item>
///   <item>rank-4 — per-stem time-domain waveforms <c>[1, stems, 2, 343980]</c>.
///         Final stem = <c>istft(spec) + time</c> (plain addition).</item>
/// </list>
/// <para>
/// <b>Chunking</b> follows the HTDemucs reference algorithm
/// (margin-trim + cosine constant-power crossfade), not the legacy
/// "shifts × overlap" loop. For each chunk we:
/// </para>
/// <list type="number">
///   <item>Extract <c>segment</c> samples around the current target
///         position, reflection-padding at file boundaries.</item>
///   <item>Run the model exactly once on the full segment.</item>
///   <item>Trim <c>marginSamples</c> from both ends of the model output
///         (those regions are warm-up artefacts).</item>
///   <item>Sin/cos crossfade the leading <c>crossfadeSamples</c> of the
///         trimmed result into the accumulation buffer, then copy the
///         remainder straight.</item>
/// </list>
/// <para>
/// <b>Performance:</b> inputs are bound via <c>OrtValue.CreateTensorValueFromMemory</c>
/// pinning the managed <c>float[]</c> directly — this avoids the
/// reflection-based DenseTensor boxing that <c>NamedOnnxValue.CreateFromTensor</c>
/// uses internally.
/// </para>
/// <para>
/// <b>Every buffer whose shape is constant across chunks is allocated ONCE</b>
/// (in <see cref="EnsureChunkScratch"/>) and reused for the whole prediction:
/// the two channel extracts, the two ORT output copies, the per-stem
/// spectrogram scratch and the per-stem interleaved chunk. They are fields
/// rather than <see cref="ArrayPool{T}"/> rentals because every one of them is
/// larger than the pool's 1 MB bucket ceiling — a rental of that size is just a
/// <c>new</c> plus bookkeeping — and because their sizes are fixed by the model,
/// so a single live set is the minimum possible footprint.
/// </para>
/// <para>
/// This used to allocate them per chunk: ~375 MB per chunk, essentially all
/// Large Object Heap, and with a ≈6.75 s stride a 3:30 song is ~32 chunks —
/// ~12 GB of LOH churn per separation, forcing repeated gen-2 compactions during
/// a run that is already competing with the audio callback. The class doc
/// asserted the buffers were pooled; nothing in the project ever called
/// <c>ArrayPool</c>. <b>Do not re-introduce a per-chunk allocation here.</b> The
/// remaining per-chunk allocations are inside <see cref="Stft"/>'s
/// forward/inverse, which is shared with the MDX-Net backend and takes no
/// caller-supplied scratch.
/// </para>
/// </remarks>
public sealed class HtdemucsOnnxInference
{
    internal const int SampleRate = 44_100;
    internal const int Segment    = 343_980;     // 7.8 s
    internal const int NFft       = 4096;
    internal const int Hop        = 1024;
    internal const int FreqBins   = 2048;        // model drops Nyquist; keeps DC
    internal const int TimeFrames = 336;

    /// <summary>
    /// Chunking / overlap settings. The defaults match the OwnAudioSharp /
    /// HTDemucs reference algorithm. <see cref="MarginSeconds"/> sets how
    /// much of each chunk's output is discarded as "warm-up" — increasing
    /// it reduces seam artefacts at the cost of more chunks per file.
    /// <see cref="CrossfadeSeconds"/> is the cosine-blend region between
    /// adjacent chunks.
    /// </summary>
    public sealed record Settings(
        float MarginSeconds = 0.5f,
        float CrossfadeSeconds = 0.05f);

    /// <summary>One separated stem returned by <see cref="Predict"/>.</summary>
    /// <param name="Name">Stem name — <c>drums</c>, <c>bass</c>, <c>other</c>,
    /// <c>vocals</c>, and for a six-stem model also <c>guitar</c> and <c>piano</c>.</param>
    /// <param name="InterleavedStereo">The stem's samples, interleaved L/R, at
    /// <b><see cref="SampleRate"/> (44 100 Hz) regardless of the rate the caller
    /// passed</b>, and as long as the input was AFTER conversion to that rate.
    /// <para><see cref="Predict"/> replaces its own input when the rates differ,
    /// so a 48 kHz caller using this class DIRECTLY — which it is public and
    /// documented for — gets 44.1 kHz stems, and writes a pitched-up WAV if it
    /// believes otherwise. This said "at the input's sample rate and the same
    /// length as the input", which was wrong on both counts (W4-S12).
    /// <c>OnnxStemSeparator</c> is the type that converts back, and it fits the
    /// result to the mix's exact frame count.</para></param>
    public sealed record StemResult(string Name, float[] InterleavedStereo);

    private static readonly string[] Stems4 = { "drums", "bass", "other", "vocals" };
    private static readonly string[] Stems6 = { "drums", "bass", "other", "vocals", "guitar", "piano" };

    private readonly InferenceSession _session;
    private readonly Settings _cfg;
    private readonly Action<string>? _log;
    private readonly HtdemucsIoBinding _io;
    private int _stems;

    // Every constant-shape per-chunk buffer, allocated once. See
    // HtdemucsChunkScratch and the class remarks above. The waveform-only
    // layout needs none of the spectral planes, so it keeps its two buffers
    // apart — allocated on the first chunk, once the stem count is known.
    private HtdemucsChunkScratch? _scratch;
    private float[] _waveOnlyOut = [];
    private float[][] _waveOnlyInterleaved = [];

    /// <summary>
    /// Bind to a loaded HTDemucs session: classify its IO layout (see
    /// <see cref="HtdemucsIoBinding.Classify"/>) and read the stem count off the
    /// output shape. The session is NOT owned.
    /// </summary>
    /// <param name="session">The loaded HTDemucs graph.</param>
    /// <param name="log">Receives one line naming the bound tensors and the stem count.</param>
    /// <param name="settings">Chunk margin and crossfade; defaults when <c>null</c>.</param>
    /// <exception cref="InvalidOperationException">The graph has neither supported layout.</exception>
    public HtdemucsOnnxInference(InferenceSession session, Action<string>? log = null, Settings? settings = null)
    {
        _session = session;
        _cfg = settings ?? new Settings();
        _log = log;
        _io = HtdemucsIoBinding.FromSession(session);

        // The two-input export sizes its scratch from the stem count before
        // the first chunk, so a symbolic stem dimension keeps the historical
        // fallback of 6. The waveform-only export (whose stem dimension IS
        // symbolic) learns it from its first output instead — guessing 6 for
        // a 4-stem graph would mislabel and mis-stride every stem.
        _stems = _io.DeclaredStems > 0 ? _io.DeclaredStems
               : _io.Layout == HtdemucsIoLayout.WaveformAndSpectrogram ? 6 : 0;
        _log?.Invoke($"htdemucs ONNX bound. layout={_io.Layout}, waveIn='{_io.WaveInput}', " +
            $"specIn='{_io.SpecInput ?? "-"}', specOut='{_io.SpecOutput ?? "-"}', waveOut='{_io.WaveOutput}', " +
            $"stems={(_stems > 0 ? _stems.ToString(System.Globalization.CultureInfo.InvariantCulture) : "from first output")}, " +
            $"margin={_cfg.MarginSeconds:F3}s, crossfade={_cfg.CrossfadeSeconds:F3}s.");
    }

    /// <summary>Which IO layout the bound graph has.</summary>
    public HtdemucsIoLayout Layout => _io.Layout;

    /// <summary>How many stems this model produces, read from the graph's output shape —
    /// 4 for the standard export, 6 for the guitar/piano one. A two-input graph that
    /// does not declare the dimension falls back to 6; a waveform-only graph that does
    /// not declare it reports <c>0</c> until the first <see cref="Predict"/> has read
    /// it off a real output.</summary>
    public int StemCount => _stems;

    /// <summary>
    /// Separate a whole track: split it into overlapping chunks, run the model on each,
    /// and cosine-crossfade the results back together so the seams are inaudible.
    /// Synchronous and CPU/GPU-bound; run it off the UI thread.
    /// </summary>
    /// <param name="interleavedStereo">Input samples, interleaved L/R; the length must be even.</param>
    /// <param name="sampleRate">Sample rate in Hz of the input.</param>
    /// <param name="progress">Reports 0..1 as chunks complete.</param>
    /// <param name="ct">Cancels between chunks; a cancelled run throws
    /// <see cref="OperationCanceledException"/>, never a separation failure.</param>
    /// <returns>One <see cref="StemResult"/> per stem, each the same length as the input.</returns>
    /// <exception cref="ArgumentException">The input length is not even, i.e. not interleaved stereo.</exception>
    public IReadOnlyList<StemResult> Predict(
        float[] interleavedStereo,
        int sampleRate,
        IProgress<double>? progress = null,
        CancellationToken ct = default)
    {
        if (interleavedStereo.Length % 2 != 0)
            throw new ArgumentException("Input must be interleaved stereo.", nameof(interleavedStereo));
        if (sampleRate != SampleRate)
        {
            _log?.Invoke($"htdemucs ONNX: resampling input {sampleRate} → {SampleRate} Hz (Lanczos-8).");
            interleavedStereo = SampleRateConverter.Resample(interleavedStereo, 2, sampleRate, SampleRate);
        }

        var frames = interleavedStereo.Length / 2;
        var hostSpectrogram = _io.Layout == HtdemucsIoLayout.WaveformAndSpectrogram;

        var marginSamples = Math.Max(0, (int)(_cfg.MarginSeconds * SampleRate));
        var crossfadeSamples = Math.Max(0, (int)(_cfg.CrossfadeSeconds * SampleRate));
        var validSize = Math.Max(1, Segment - 2 * marginSamples);
        var stride = Math.Max(1, validSize - crossfadeSamples);

        // Output accumulator: one interleaved-stereo float[] per stem. Built on
        // the first chunk, because a waveform-only graph only says how many
        // stems it has when it has produced some.
        float[][]? stemsOut = null;

        // Pre-allocated input buffers (reused across chunks via OrtValue pinning).
        var waveBuf = new float[1 * 2 * Segment];
        var specBuf = hostSpectrogram ? new float[1 * 4 * FreqBins * TimeFrames] : [];
        var scratch = hostSpectrogram ? EnsureChunkScratch() : null;

        var totalChunks = Math.Max(1, (frames + stride - 1) / stride);
        var chunkIndex = 0;
        var swExtract = new Stopwatch();
        var swSpec    = new Stopwatch();
        var swRun     = new Stopwatch();
        var swBuild   = new Stopwatch();
        var swBlend   = new Stopwatch();
        for (var targetPos = 0; targetPos < frames; targetPos += stride)
        {
            ct.ThrowIfCancellationRequested();
            chunkIndex++;

            var windowStart = targetPos - marginSamples;
            swExtract.Start();
            ExtractStereoChunk(interleavedStereo, frames, windowStart, Segment, waveBuf);
            swExtract.Stop();

            float[][] builtChunks;
            if (hostSpectrogram)
            {
                swSpec.Start();
                ComputeAndPackSpec(waveBuf, specBuf);
                swSpec.Stop();

                swRun.Start();
                var (specOut, specShape, waveOut, waveShape) = RunModel(waveBuf, specBuf, ct);
                swRun.Stop();

                // Build all stem chunks in parallel (each does 2 independent iSTFTs;
                // 6 stems × 2 channels = 12 iSTFTs per audio chunk — embarrassingly parallel).
                swBuild.Start();
                builtChunks = scratch!.Interleaved;
                Parallel.For(0, _stems, s =>
                {
                    BuildStemChunk(s, specOut, specShape, waveOut, waveShape);
                });
                swBuild.Stop();
            }
            else
            {
                swRun.Start();
                var (waveOut, waveShape) = RunWaveformOnlyModel(waveBuf, ct);
                swRun.Stop();

                swBuild.Start();
                builtChunks = EnsureWaveOnlyChunks(waveShape);
                for (var s = 0; s < _stems; s++)
                    BuildWaveOnlyStemChunk(s, waveOut, waveShape);
                swBuild.Stop();
            }

            stemsOut ??= AllocateStemOutputs(_stems, frames);

            // Blend sequentially — cheap, and it writes one shared output.
            swBlend.Start();
            for (var s = 0; s < _stems; s++)
            {
                BlendChunkIntoOutput(builtChunks[s], marginSamples, crossfadeSamples,
                    stemsOut[s], frames, targetPos);
            }
            swBlend.Stop();

            progress?.Report(Math.Min(1.0, chunkIndex / (double)totalChunks));
        }

        _log?.Invoke($"htdemucs timings ({_io.Layout}): extract={swExtract.ElapsedMilliseconds}ms, " +
            $"specPack={swSpec.ElapsedMilliseconds}ms, run={swRun.ElapsedMilliseconds}ms, " +
            $"build={swBuild.ElapsedMilliseconds}ms, blend={swBlend.ElapsedMilliseconds}ms " +
            $"({chunkIndex} chunks).");

        // An empty input never ran the model: a graph that declares its stem
        // count still answers with that many empty stems; one that does not
        // has nothing honest to say but "none".
        stemsOut ??= AllocateStemOutputs(_stems, frames);
        var stemNames = StemNames(_stems);
        var results = new List<StemResult>(_stems);
        for (var s = 0; s < _stems; s++)
            results.Add(new StemResult(stemNames[s], stemsOut[s]));
        return results;
    }

    /// <summary>The stem names htdemucs uses for a given stem count, in output order.</summary>
    internal static string[] StemNames(int stems) =>
        stems == 4 ? Stems4 : stems == 6 ? Stems6 : DefaultNames(stems);

    private static float[][] AllocateStemOutputs(int stems, int frames)
    {
        var outputs = new float[stems][];
        for (var s = 0; s < stems; s++) outputs[s] = new float[frames * 2];
        return outputs;
    }

    /// <summary>
    /// The waveform-only layout's per-stem chunk buffers, sized on the FIRST
    /// chunk from the real output shape <c>[1, stems, 2, Segment]</c> and reused
    /// for every chunk after it. Also where that layout learns its stem count
    /// when the graph leaves the dimension symbolic, and where a graph that
    /// answers with a shape it did not declare is stopped.
    /// </summary>
    private float[][] EnsureWaveOnlyChunks(int[] waveShape)
    {
        if (waveShape.Length != 4 || waveShape[0] != 1 || waveShape[1] <= 0 || waveShape[2] != 2 ||
            waveShape[3] != Segment)
            throw new InvalidOperationException(
                $"htdemucs ONNX ({_io.Layout}) returned output [{string.Join(", ", waveShape)}]; " +
                $"expected [1, stems, 2, {Segment}].");
        if (_stems == 0) _stems = waveShape[1];
        else if (waveShape[1] != _stems)
            throw new InvalidOperationException(
                $"htdemucs ONNX returned {waveShape[1]} stems; the graph declared {_stems}.");

        if (_waveOnlyInterleaved.Length != _stems)
        {
            _waveOnlyInterleaved = new float[_stems][];
            for (var s = 0; s < _stems; s++) _waveOnlyInterleaved[s] = new float[Segment * 2];
        }
        return _waveOnlyInterleaved;
    }

    /// <summary>
    /// Copies <c>length</c> samples of stereo audio starting at
    /// <paramref name="windowStart"/> from <paramref name="src"/> into the
    /// pre-allocated <paramref name="dst"/> buffer (shape <c>[1, 2, length]</c>
    /// flattened). Out-of-range indices are reflection-padded
    /// (<c>src[-i] = src[i]</c> on the left, <c>src[N+i] = src[N-i-2]</c>
    /// on the right).
    /// </summary>
    private static void ExtractStereoChunk(float[] src, int srcFrames, int windowStart, int length, float[] dst)
    {
        for (var i = 0; i < length; i++)
        {
            var srcIdx = windowStart + i;
            if (srcIdx < 0) srcIdx = -srcIdx;
            else if (srcIdx >= srcFrames) srcIdx = Math.Max(0, 2 * srcFrames - srcIdx - 2);
            if (srcIdx < 0) srcIdx = 0;
            if (srcIdx >= srcFrames) srcIdx = srcFrames - 1;

            dst[0 * length + i]              = src[2 * srcIdx];      // left channel
            dst[1 * length + i]              = src[2 * srcIdx + 1];  // right channel
        }
    }

    /// <summary>
    /// Count of non-finite samples the MODEL produced (either branch), replaced
    /// with silence in <see cref="BuildStemChunk"/>. Non-zero means the network
    /// or its execution provider is misbehaving; nothing else would say so.
    /// </summary>
    public long NonFiniteOutputs => Interlocked.Read(ref _nonFiniteOutputs);

    private long _nonFiniteOutputs;

    /// <summary>
    /// Allocates every constant-shape chunk buffer once. Idempotent: a second
    /// <see cref="Predict"/> on the same instance reuses what the first built.
    /// </summary>
    private HtdemucsChunkScratch EnsureChunkScratch() =>
        _scratch ??= new HtdemucsChunkScratch(_stems, Segment, TimeFrames, FreqBins, NFft, Hop);

    private void ComputeAndPackSpec(float[] waveBuf, float[] specBuf)
    {
        // waveBuf is [1, 2, Segment] flattened (L block then R block).
        var leftSpan = _scratch!.Left;
        var rightSpan = _scratch.Right;
        Array.Copy(waveBuf, 0,       leftSpan,  0, Segment);
        Array.Copy(waveBuf, Segment, rightSpan, 0, Segment);

        var specL = _scratch.InL;
        var specR = _scratch.InR;
        Parallel.Invoke(
            () => Stft.ForwardHtdemucs(leftSpan,  specL, _scratch.Work[0]),
            () => Stft.ForwardHtdemucs(rightSpan, specR, _scratch.Work[1]));

        // Pack as [1, 4, FreqBins, TimeFrames] flattened. The 4 channels are
        // [Re_L, Im_L, Re_R, Im_R]. Keep bin 0 (DC), drop the Nyquist bin
        // at index FreqBins (= NFft/2). Frame count caps at TimeFrames.
        var t = Math.Min(TimeFrames, specL.FrameCount);
        var planeStride = FreqBins * TimeFrames;
        Array.Clear(specBuf, 0, specBuf.Length);
        for (var f = 0; f < t; f++)
        {
            var reL = specL.Re[f];
            var imL = specL.Im[f];
            var reR = specR.Re[f];
            var imR = specR.Im[f];
            for (var b = 0; b < FreqBins; b++)
            {
                var idx = b * TimeFrames + f;
                specBuf[0 * planeStride + idx] = reL[b];
                specBuf[1 * planeStride + idx] = imL[b];
                specBuf[2 * planeStride + idx] = reR[b];
                specBuf[3 * planeStride + idx] = imR[b];
            }
        }
    }

    private (float[] SpecOut, int[] SpecShape, float[] WaveOut, int[] WaveShape) RunModel(
        float[] waveBuf, float[] specBuf, CancellationToken ct)
    {
        // Pin managed arrays directly to ORT — no DenseTensor boxing.
        using var waveInput = OrtValue.CreateTensorValueFromMemory(
            waveBuf, new long[] { 1, 2, Segment });
        using var specInput = OrtValue.CreateTensorValueFromMemory(
            specBuf, new long[] { 1, 4, FreqBins, TimeFrames });
        using var runOpts = new RunOptions();
        // CANCELLATION. The only other cancel point is the top of the chunk
        // loop, and one chunk is seconds of inference: a user Cancel appeared to
        // do nothing until it finished. Terminate asks ORT to abandon the graph
        // it is executing; Run then returns EARLY rather than throwing, so the
        // token has to be re-checked below.
        // Shared with the two spectral drivers, and a catch-all body: a throw
        // out of a cancellation callback would surface an ORT exception type
        // where IStemSeparator promises OperationCanceledException. See
        // SpectralOnnxRunner.RequestTerminate.
        using var cancelReg = ct.Register(SpectralOnnxRunner.RequestTerminate, runOpts);

        var inputNames  = new[] { _io.WaveInput, _io.SpecInput! };
        var inputValues = new[] { waveInput, specInput };
        var outputNames = new[] { _io.SpecOutput!, _io.WaveOutput };

        using var results = _session.Run(runOpts, inputNames, inputValues, outputNames);
        ct.ThrowIfCancellationRequested();
        var specOrt = results[0];
        var waveOrt = results[1];

        var specShape = Array.ConvertAll(specOrt.GetTensorTypeAndShape().Shape, x => (int)x);
        var waveShape = Array.ConvertAll(waveOrt.GetTensorTypeAndShape().Shape, x => (int)x);
        // COPY, never .ToArray(): the tensors are only READ downstream, but the
        // OrtValues die with `results` at the end of this method, so a span
        // cannot escape. Copying into the pre-sized scratch keeps the read cheap
        // and costs no allocation — .ToArray() was 82 MB of fresh LOH per chunk.
        var specSrc = specOrt.GetTensorDataAsSpan<float>();
        var waveSrc = waveOrt.GetTensorDataAsSpan<float>();
        var specOut = _scratch!.SpecOut(specSrc.Length);
        var waveOut = _scratch.WaveOut(waveSrc.Length);
        specSrc.CopyTo(specOut);
        waveSrc.CopyTo(waveOut);
        return (specOut, specShape, waveOut, waveShape);
    }

    /// <summary>
    /// <see cref="RunModel"/> for <see cref="HtdemucsIoLayout.WaveformOnly"/>: the
    /// waveform goes in, the finished stems <c>[1, stems, 2, Segment]</c> come
    /// out. Same pinning, same cancellation, same copy-into-reused-scratch rule.
    /// </summary>
    private (float[] WaveOut, int[] WaveShape) RunWaveformOnlyModel(float[] waveBuf, CancellationToken ct)
    {
        using var waveInput = OrtValue.CreateTensorValueFromMemory(
            waveBuf, new long[] { 1, 2, Segment });
        using var runOpts = new RunOptions();
        // See RunModel: Terminate makes Run return early, so re-check after.
        using var cancelReg = ct.Register(SpectralOnnxRunner.RequestTerminate, runOpts);

        var inputNames  = new[] { _io.WaveInput };
        var inputValues = new[] { waveInput };
        var outputNames = new[] { _io.WaveOutput };

        using var results = _session.Run(runOpts, inputNames, inputValues, outputNames);
        ct.ThrowIfCancellationRequested();
        var waveOrt = results[0];
        var waveShape = Array.ConvertAll(waveOrt.GetTensorTypeAndShape().Shape, x => (int)x);
        var waveSrc = waveOrt.GetTensorDataAsSpan<float>();
        if (_waveOnlyOut.Length < waveSrc.Length) _waveOnlyOut = GC.AllocateUninitializedArray<float>(waveSrc.Length);
        waveSrc.CopyTo(_waveOnlyOut);
        return (_waveOnlyOut, waveShape);
    }

    /// <summary>
    /// One stem's interleaved chunk for <see cref="HtdemucsIoLayout.WaveformOnly"/>:
    /// the graph already summed its two branches, so this is a de-planarise
    /// (<c>[stems, 2, Segment]</c> → L/R interleaved) plus the same non-finite
    /// scrub <see cref="BuildStemChunk"/> applies.
    /// </summary>
    private void BuildWaveOnlyStemChunk(int s, float[] waveOut, int[] waveShape)
    {
        var interleaved = _waveOnlyInterleaved[s];
        var waveBase = s * waveShape[2] * waveShape[3];   // [1, S, 2, Segment]
        var bad = 0L;
        for (var i = 0; i < Segment; i++)
        {
            interleaved[2 * i]     = Scrub(waveOut[waveBase + 0 * Segment + i], ref bad);
            interleaved[2 * i + 1] = Scrub(waveOut[waveBase + 1 * Segment + i], ref bad);
        }
        if (bad != 0) Interlocked.Add(ref _nonFiniteOutputs, bad);
    }

    /// <summary>
    /// Reconstructs one stem's interleaved-stereo time-domain waveform from
    /// the dual-branch model outputs: <c>stem[i] = istft(spec[s]) + time[s][i]</c>
    /// into this stem's reusable interleaved buffer (length <c>Segment*2</c>,
    /// fully overwritten every call).
    /// <para>
    /// Instance rather than static because it writes into the per-stem scratch.
    /// Parallel-safe: each <paramref name="s"/> owns its own slot and nothing
    /// crosses between them.
    /// </para>
    /// </summary>
    private void BuildStemChunk(int s, float[] specOut, int[] specShape,
        float[] waveOut, int[] waveShape)
    {
        // specOut shape: [1, S, 4, FreqBins, TimeFrames]
        // waveOut shape: [1, S, 2, Segment]
        var specStrideStem    = specShape[2] * specShape[3] * specShape[4];     // per stem
        var specStrideChan    = specShape[3] * specShape[4];                    // per packed channel
        var specStrideBin     = specShape[4];                                   // per freq bin
        var stemBase = s * specStrideStem;

        var outSpecL = _scratch!.SpecL[s];
        var outSpecR = _scratch.SpecR[s];
        for (var f = 0; f < TimeFrames; f++)
        {
            // Keep DC (bin 0), drop the unfilled Nyquist (left at zero — never
            // written here, so the reused rows keep it zero for good).
            var reL = outSpecL.Re[f];
            var imL = outSpecL.Im[f];
            var reR = outSpecR.Re[f];
            var imR = outSpecR.Im[f];
            for (var b = 0; b < FreqBins; b++)
            {
                var binIdx = b * specStrideBin + f;
                reL[b] = specOut[stemBase + 0 * specStrideChan + binIdx];
                imL[b] = specOut[stemBase + 1 * specStrideChan + binIdx];
                reR[b] = specOut[stemBase + 2 * specStrideChan + binIdx];
                imR[b] = specOut[stemBase + 3 * specStrideChan + binIdx];
            }
        }

        // Both iSTFTs go through THIS stem's workspace, one after the other:
        // the parallelism is one level up (six stems at once), and a serial
        // inverse needs one scratch plane instead of a float[nFft] per frame.
        var work = _scratch.Work[s];
        var interleaved = _scratch.Interleaved[s];
        var waveBase = s * waveShape[2] * waveShape[3];   // [1, S, 2, Segment]

        // SCRUB: the model output is an UNTRUSTED stage, the same rule the
        // native DspChain::process follows. A non-finite sample here does not
        // stay in its chunk — BlendChunkIntoOutput crossfades chunk N into the
        // tail of chunk N-1, so one NaN poisons audio the model got right, and
        // then survives the post-resample and the WAV writer because every
        // downstream gate is written as "reject when <= threshold" and
        // `NaN <= x` is false. See NonFiniteOutputs.
        var bad = 0L;
        var mono = work.Mono;
        Stft.InverseHtdemucs(outSpecL, mono, work);
        for (var i = 0; i < Segment; i++)
            interleaved[2 * i] = Scrub(mono[i] + waveOut[waveBase + 0 * Segment + i], ref bad);

        Stft.InverseHtdemucs(outSpecR, mono, work);
        for (var i = 0; i < Segment; i++)
            interleaved[2 * i + 1] = Scrub(mono[i] + waveOut[waveBase + 1 * Segment + i], ref bad);

        if (bad != 0) Interlocked.Add(ref _nonFiniteOutputs, bad);
    }

    private static float Scrub(float v, ref long counter)
    {
        if (float.IsFinite(v)) return v;
        counter++;
        return 0f;
    }

    /// <summary>
    /// Trims <paramref name="marginSamples"/> from both ends of
    /// <paramref name="stemChunk"/> and writes the result into
    /// <paramref name="output"/> starting at <paramref name="targetPos"/>.
    /// The first <paramref name="crossfadeSamples"/> are blended with the
    /// existing buffer using a constant-power (cos/sin) curve.
    /// </summary>
    /// <remarks>
    /// There used to be a <c>filledForStem</c> cursor threaded through here by
    /// reference and an <c>isFirstStem</c> flag to guard it. Nothing ever READ
    /// the cursor — it was written on the last line of this method and that was
    /// its only use — so both are gone.
    /// </remarks>
    private static void BlendChunkIntoOutput(float[] stemChunk, int marginSamples, int crossfadeSamples,
        float[] output, int outFrames, int targetPos)
    {
        // Effective region of the chunk (in samples): [marginSamples .. Segment - marginSamples).
        var validLen = Segment - 2 * marginSamples;
        // Where this chunk's valid region starts in the global output, in frames.
        var dstStart = targetPos;
        // Crossfade region: blend output[dstStart .. dstStart+crossfade) with
        // chunk[margin .. margin+crossfade).
        var xfade = Math.Min(crossfadeSamples,
            Math.Min(validLen, outFrames - dstStart));
        if (xfade < 0) xfade = 0;

        // For the very first chunk (targetPos == 0), there's nothing to blend
        // against — copy the whole valid region straight.
        var isFirstChunk = targetPos == 0;

        for (var i = 0; i < validLen; i++)
        {
            var dstFrame = dstStart + i;
            if (dstFrame < 0 || dstFrame >= outFrames) continue;
            var srcSampleIdx = marginSamples + i;
            var srcL = stemChunk[2 * srcSampleIdx];
            var srcR = stemChunk[2 * srcSampleIdx + 1];

            if (!isFirstChunk && i < xfade)
            {
                var t = (float)i / xfade;
                var angle = t * (float)Math.PI / 2f;
                var fadeOut = (float)Math.Cos(angle);
                var fadeIn  = (float)Math.Sin(angle);
                output[2 * dstFrame]     = output[2 * dstFrame]     * fadeOut + srcL * fadeIn;
                output[2 * dstFrame + 1] = output[2 * dstFrame + 1] * fadeOut + srcR * fadeIn;
            }
            else
            {
                output[2 * dstFrame]     = srcL;
                output[2 * dstFrame + 1] = srcR;
            }
        }

    }

    private static string[] DefaultNames(int n)
    {
        var a = new string[n];
        for (var i = 0; i < n; i++) a[i] = $"stem{i}";
        return a;
    }
}
