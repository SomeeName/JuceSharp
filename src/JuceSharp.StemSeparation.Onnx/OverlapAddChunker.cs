using JuceSharp.Dsp;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// The pad / chunk / windowed-overlap-add loop the spectral drivers all run.
/// <para>It existed three times — <c>BsRoformerInference.PredictVocals</c>,
/// <c>BsRoformerInference.PredictAllStems</c> and
/// <c>MdxNetInference.PredictInstrumental</c> — as near-identical copies, which
/// is exactly how a fix lands in one of them and not the others: the model-output
/// NaN scrub below is one such fix.</para>
/// <para><b>The scrub is the load-bearing part.</b> A model output is an
/// UNTRUSTED stage — the same rule the native <c>DspChain::process</c> follows.
/// One non-finite sample out of the network does not stay in its own chunk: the
/// accumulate is <c>out[idx+i] += res[i] * window[i]</c> over OVERLAPPING chunks,
/// so it poisons the tail of the previous chunk too, and then survives
/// normalisation, the resampler and the WAV writer. <c>NaN &lt;= x</c> is false,
/// so nothing downstream rejects it.</para>
/// </summary>
internal sealed class OverlapAddChunker
{
    private readonly int _chunkSize;
    private readonly int _step;
    private readonly float[] _window;

    public OverlapAddChunker(int chunkSize, int overlap)
    {
        if (chunkSize <= 0) throw new ArgumentOutOfRangeException(nameof(chunkSize));
        if (overlap <= 0) throw new ArgumentOutOfRangeException(nameof(overlap));
        _chunkSize = chunkSize;
        _step = Math.Max(1, chunkSize / overlap);
        _window = Windows.Hann(chunkSize);
    }

    /// <summary>
    /// Lifetime count of non-finite samples the model produced and this class
    /// replaced with silence. Non-zero means the model or its provider is
    /// misbehaving; it is otherwise completely silent.
    /// </summary>
    public long NonFiniteOutputs { get; private set; }

    /// <summary>Single-stem run. <paramref name="process"/> receives one chunk
    /// per channel and returns the processed pair.</summary>
    public float[] Run(float[] interleavedStereo,
                       Func<float[], float[], (float[] L, float[] R)> process,
                       IProgress<double>? progress, CancellationToken ct)
        => RunMulti(interleavedStereo, 1,
                    (l, r) => [process(l, r)], progress, ct)[0];

    /// <summary>
    /// Multi-stem run: <paramref name="process"/> returns one (L, R) pair per
    /// stem, all sharing the single overlap weight curve.
    /// </summary>
    public float[][] RunMulti(float[] interleavedStereo, int stems,
                              Func<float[], float[], (float[] L, float[] R)[]> process,
                              IProgress<double>? progress, CancellationToken ct)
    {
        if (interleavedStereo.Length % 2 != 0)
            throw new ArgumentException("Input must be interleaved stereo.", nameof(interleavedStereo));

        var frames = interleavedStereo.Length / 2;
        var pad = _chunkSize;
        var padded = frames + 2 * pad;

        var padL = new float[padded];
        var padR = new float[padded];
        for (var i = 0; i < frames; i++)
        {
            padL[pad + i] = interleavedStereo[2 * i];
            padR[pad + i] = interleavedStereo[2 * i + 1];
        }

        var outL = new float[stems][];
        var outR = new float[stems][];
        for (var s = 0; s < stems; s++)
        {
            outL[s] = new float[padded];
            outR[s] = new float[padded];
        }
        var weight = new float[padded];

        var chunkL = new float[_chunkSize];
        var chunkR = new float[_chunkSize];
        var totalChunks = Math.Max(1, (padded - _chunkSize) / _step + 1);

        for (int c = 0, idx = 0; idx + _chunkSize <= padded; idx += _step, c++)
        {
            ct.ThrowIfCancellationRequested();
            Array.Copy(padL, idx, chunkL, 0, _chunkSize);
            Array.Copy(padR, idx, chunkR, 0, _chunkSize);

            var perStem = process(chunkL, chunkR);
            if (perStem.Length != stems)
                throw new InvalidOperationException(
                    $"Chunk processor returned {perStem.Length} stems, expected {stems}.");

            for (var s = 0; s < stems; s++)
            {
                var (sl, sr) = perStem[s];
                var accL = outL[s];
                var accR = outR[s];
                for (var i = 0; i < _chunkSize; i++)
                {
                    var w = _window[i];
                    accL[idx + i] += Scrub(sl[i]) * w;
                    accR[idx + i] += Scrub(sr[i]) * w;
                }
            }
            for (var i = 0; i < _chunkSize; i++) weight[idx + i] += _window[i];
            progress?.Report(Math.Min(1.0, (c + 1.0) / totalChunks));
        }

        var results = new float[stems][];
        for (var s = 0; s < stems; s++)
        {
            var stem = new float[frames * 2];
            for (var i = 0; i < frames; i++)
            {
                var w = weight[i + pad];
                stem[2 * i]     = w > 1e-8f ? outL[s][i + pad] / w : 0f;
                stem[2 * i + 1] = w > 1e-8f ? outR[s][i + pad] / w : 0f;
            }
            results[s] = stem;
        }
        return results;
    }

    private float Scrub(float v)
    {
        if (float.IsFinite(v)) return v;
        NonFiniteOutputs++;
        return 0f;
    }
}
