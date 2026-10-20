namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// The chunk / fade / overlap-add loop of ZFTurbo's Music-Source-Separation-Training
/// (<c>utils/model_utils.py</c>, <c>demix</c>, "generic" mode), sample for sample, for the
/// BS-RoFormer split-complex models.
/// <para>
/// <b>Why not <see cref="OverlapAddChunker"/>.</b> That loop pads a whole chunk of silence
/// on each side and weights every chunk with a full Hann window. MSST, which these
/// checkpoints were trained and evaluated with, reflect-pads <c>chunk - step</c> samples,
/// weights each chunk with a window that is flat apart from a linear fade over the first
/// and last tenth (none at the very start of the song and none at its end), reflect-pads a
/// short last chunk longer than half a chunk, and zero-pads a shorter one. Running the
/// model through the SAME loop is what makes a whole-song comparison against the PyTorch
/// reference meaningful: any difference left is the model's arithmetic, not the windowing.
/// </para>
/// <para>
/// <b>Split into steps</b> (<see cref="Pass.Fill"/>, <see cref="Pass.Accumulate"/>,
/// <see cref="Pass.Finish"/>) so a driver can overlap the host work of one chunk with the
/// inference of the next (<see cref="ChunkPipeline"/>). <see cref="RunMulti"/> is the plain
/// sequential loop over the same steps.
/// </para>
/// <para>
/// A model output is untrusted: a non-finite sample is replaced with silence on its way
/// into the sum and counted (<see cref="Pass.NonFiniteOutputs"/>), as in
/// <see cref="OverlapAddChunker"/>.
/// </para>
/// </summary>
internal sealed class FadeWindowChunker
{
    private readonly float[] _window;

    /// <param name="chunkSize">Samples per chunk, per channel.</param>
    /// <param name="numOverlap">MSST's <c>num_overlap</c>: the step is <c>chunkSize / numOverlap</c>.</param>
    public FadeWindowChunker(int chunkSize, int numOverlap)
    {
        if (chunkSize < 20) throw new ArgumentOutOfRangeException(nameof(chunkSize), "A chunk needs at least 20 samples.");
        if (numOverlap <= 0) throw new ArgumentOutOfRangeException(nameof(numOverlap));
        ChunkSize = chunkSize;
        Step = Math.Max(1, chunkSize / numOverlap);
        Border = chunkSize - Step;
        FadeSize = chunkSize / 10;
        _window = Window(chunkSize, FadeSize);
    }

    /// <summary>Samples per chunk.</summary>
    public int ChunkSize { get; }

    /// <summary>Samples between chunk starts.</summary>
    public int Step { get; }

    /// <summary>Samples reflected onto each end of a song longer than two borders.</summary>
    public int Border { get; }

    /// <summary>Length of each linear fade.</summary>
    public int FadeSize { get; }

    /// <summary>MSST's <c>_getWindowingArray</c>: ones, a <c>linspace(0, 1)</c> fade-in over the
    /// first <paramref name="fade"/> samples and a <c>linspace(1, 0)</c> fade-out over the last.</summary>
    internal static float[] Window(int size, int fade)
    {
        var w = new float[size];
        Array.Fill(w, 1f);
        for (var i = 0; i < fade; i++)
        {
            var up = fade == 1 ? 0f : (float)i / (fade - 1);
            w[size - fade + i] = 1f - up;
            w[i] = up;
        }
        return w;
    }

    /// <summary>Starts one song.</summary>
    /// <param name="interleavedStereo">L/R interleaved.</param>
    /// <param name="stems">Stems the model returns per chunk.</param>
    /// <returns>The pass; not thread-safe except as <see cref="ChunkPipeline"/> uses it.</returns>
    public Pass Begin(float[] interleavedStereo, int stems)
    {
        ArgumentNullException.ThrowIfNull(interleavedStereo);
        if (interleavedStereo.Length % 2 != 0)
            throw new ArgumentException("Input must be interleaved stereo.", nameof(interleavedStereo));
        if (stems <= 0) throw new ArgumentOutOfRangeException(nameof(stems));
        return new Pass(this, interleavedStereo, stems);
    }

    /// <summary>The sequential loop: fill, process, accumulate, for every chunk.</summary>
    public float[][] RunMulti(float[] interleavedStereo, int stems,
                              Func<float[], float[], (float[] L, float[] R)[]> process,
                              IProgress<double>? progress, CancellationToken ct)
    {
        ArgumentNullException.ThrowIfNull(process);
        var pass = Begin(interleavedStereo, stems);
        var chunkL = new float[ChunkSize];
        var chunkR = new float[ChunkSize];
        for (var k = 0; k < pass.Count; k++)
        {
            ct.ThrowIfCancellationRequested();
            pass.Fill(k, chunkL, chunkR);
            pass.Accumulate(k, process(chunkL, chunkR));
            progress?.Report((k + 1.0) / pass.Count);
        }
        return pass.Finish();
    }

    /// <summary>One song through the chunker.</summary>
    internal sealed class Pass
    {
        private readonly FadeWindowChunker _owner;
        private readonly int _frames;
        private readonly int _offset;
        private readonly float[] _padL;
        private readonly float[] _padR;
        private readonly float[][] _accL;
        private readonly float[][] _accR;
        private readonly float[] _weight;
        private long _nonFinite;

        internal Pass(FadeWindowChunker owner, float[] stereo, int stems)
        {
            _owner = owner;
            _frames = stereo.Length / 2;
            // MSST reflects `border` samples onto each end, but only when the song is longer
            // than two borders (torch's reflect pad needs the pad shorter than the input).
            _offset = _frames > 2 * owner.Border && owner.Border > 0 ? owner.Border : 0;
            var length = _frames + 2 * _offset;
            _padL = new float[length];
            _padR = new float[length];
            for (var i = 0; i < _frames; i++)
            {
                _padL[_offset + i] = stereo[2 * i];
                _padR[_offset + i] = stereo[2 * i + 1];
            }
            for (var j = 0; j < _offset; j++)
            {
                // torch.nn.functional.pad(mode="reflect"): the edge sample is not repeated.
                _padL[j] = stereo[2 * (_offset - j)];
                _padR[j] = stereo[2 * (_offset - j) + 1];
                _padL[_offset + _frames + j] = stereo[2 * (_frames - 2 - j)];
                _padR[_offset + _frames + j] = stereo[2 * (_frames - 2 - j) + 1];
            }

            Count = length == 0 ? 0 : (length + owner.Step - 1) / owner.Step;
            _accL = new float[stems][];
            _accR = new float[stems][];
            for (var s = 0; s < stems; s++)
            {
                _accL[s] = new float[length];
                _accR[s] = new float[length];
            }
            _weight = new float[length];
        }

        /// <summary>Chunks in this song.</summary>
        public int Count { get; }

        /// <summary>Non-finite model samples replaced with silence so far.</summary>
        public long NonFiniteOutputs => Interlocked.Read(ref _nonFinite);

        /// <summary>Where chunk <paramref name="k"/> starts in the padded song.</summary>
        public int Start(int k) => k * _owner.Step;

        /// <summary>How many of chunk <paramref name="k"/>'s samples are song (the rest is padding).</summary>
        public int SegmentLength(int k) => Math.Min(_owner.ChunkSize, _padL.Length - Start(k));

        /// <summary>Copies chunk <paramref name="k"/> into the two buffers, padding a short
        /// last chunk: reflected when it holds more than half a chunk, zeros otherwise.</summary>
        public void Fill(int k, float[] chunkL, float[] chunkR)
        {
            var size = _owner.ChunkSize;
            var start = Start(k);
            var seg = SegmentLength(k);
            Array.Copy(_padL, start, chunkL, 0, seg);
            Array.Copy(_padR, start, chunkR, 0, seg);
            if (seg == size) return;
            if (seg > size / 2)
            {
                for (var j = 0; j < size - seg; j++)
                {
                    chunkL[seg + j] = chunkL[seg - 2 - j];
                    chunkR[seg + j] = chunkR[seg - 2 - j];
                }
            }
            else
            {
                Array.Clear(chunkL, seg, size - seg);
                Array.Clear(chunkR, seg, size - seg);
            }
        }

        /// <summary>Adds chunk <paramref name="k"/>'s stems, faded, into the song. Chunks may be
        /// accumulated from another thread than the one that fills, one at a time.</summary>
        public void Accumulate(int k, (float[] L, float[] R)[] perStem)
        {
            ArgumentNullException.ThrowIfNull(perStem);
            if (perStem.Length != _accL.Length)
                throw new InvalidOperationException(
                    $"Chunk processor returned {perStem.Length} stems, expected {_accL.Length}.");

            var w = _owner._window;
            var fade = _owner.FadeSize;
            var start = Start(k);
            var seg = SegmentLength(k);
            // MSST: the first chunk has no fade-in, the last no fade-out ("elif": a song that
            // is one chunk keeps its fade-out).
            var flatHead = k == 0 ? fade : 0;
            var flatTail = k != 0 && k == Count - 1 ? _owner.ChunkSize - fade : int.MaxValue;
            float Weight(int i) => i < flatHead || i >= flatTail ? 1f : w[i];

            long bad = 0;
            for (var s = 0; s < perStem.Length; s++)
            {
                var (sl, sr) = perStem[s];
                var accL = _accL[s];
                var accR = _accR[s];
                for (var i = 0; i < seg; i++)
                {
                    var wi = Weight(i);
                    accL[start + i] += Scrub(sl[i], ref bad) * wi;
                    accR[start + i] += Scrub(sr[i], ref bad) * wi;
                }
            }
            for (var i = 0; i < seg; i++) _weight[start + i] += Weight(i);
            if (bad != 0) Interlocked.Add(ref _nonFinite, bad);
        }

        /// <summary>Divides by the summed weights, strips the reflected border and
        /// interleaves: one array per stem, as long as the input.</summary>
        public float[][] Finish()
        {
            var results = new float[_accL.Length][];
            for (var s = 0; s < _accL.Length; s++)
            {
                var stem = new float[_frames * 2];
                var accL = _accL[s];
                var accR = _accR[s];
                for (var i = 0; i < _frames; i++)
                {
                    var w = _weight[_offset + i];
                    stem[2 * i] = w > 0f ? accL[_offset + i] / w : 0f;
                    stem[2 * i + 1] = w > 0f ? accR[_offset + i] / w : 0f;
                }
                results[s] = stem;
            }
            return results;
        }

        private static float Scrub(float v, ref long bad)
        {
            if (float.IsFinite(v)) return v;
            bad++;
            return 0f;
        }
    }
}
