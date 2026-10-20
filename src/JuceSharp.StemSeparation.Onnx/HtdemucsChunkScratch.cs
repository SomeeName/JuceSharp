namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Every constant-shape buffer <see cref="HtdemucsOnnxInference"/> needs per
/// chunk, allocated ONCE for a whole prediction.
/// <para>
/// The driver used to allocate all of these inside the chunk loop: ~375 MB per
/// chunk, essentially all Large Object Heap. With a ≈6.75 s stride a 3:30 song
/// is ~32 chunks — ~12 GB of LOH churn per separation, forcing repeated gen-2
/// compactions during a run that is already competing with the audio callback.
/// (The class doc claimed the buffers were rented from
/// <c>ArrayPool&lt;T&gt;.Shared</c>; nothing in the project ever called it.)
/// </para>
/// <para>
/// <b>Fields, not <c>ArrayPool</c> rentals</b>, on purpose: every one of these is
/// far larger than the shared pool's 1 MB bucket ceiling, so a rental of that
/// size is a plain <c>new</c> plus bookkeeping, and their shapes are fixed by the
/// model — one live set is already the minimum possible footprint.
/// </para>
/// <para>
/// <b>Spectra are FLOAT PLANES, not <c>Complex</c>.</b> <c>System.Numerics.Complex</c>
/// is two doubles — 16 bytes a bin for data that comes out of a float32 model and
/// goes straight back into one. On the shipping shape (6 stems x 2 channels x 336
/// frames x 2049 bins) that is <b>132 MB against 66 MB</b>. See
/// <see cref="SpectrumPlanes"/>.
/// </para>
/// <para>
/// <b>Reuse safety.</b> <c>Left</c>/<c>Right</c>, the ORT copies and each
/// <c>Interleaved[s]</c> are fully overwritten every chunk. The spectrogram rows
/// are not: the driver writes bins <c>0 .. freqBins-1</c> and deliberately leaves
/// the Nyquist bin at index <c>freqBins</c> zero, so it must never be written by
/// anyone — allocation gives it to us as zero and nothing may change that.
/// </para>
/// <para>
/// Per-stem slots are independent, so the driver's <c>Parallel.For</c> over stems
/// can write <c>SpecL[s]</c>/<c>SpecR[s]</c>/<c>Interleaved[s]</c> concurrently —
/// and each stem owns <c>Work[s]</c>, which is what lets the transforms run with
/// no allocation at all. <c>Work[0]</c>/<c>Work[1]</c> are reused by the FORWARD
/// pass, which finishes before any stem is built.
/// </para>
/// </summary>
internal sealed class HtdemucsChunkScratch
{
    private float[] _specOut;
    private float[] _waveOut;

    /// <summary>
    /// DEAD PARAMETER, kept only so an existing caller still compiles:
    /// <paramref name="bins" /> decided nothing. Every plane sizes itself
    /// <c>nFft / 2 + 1</c> from the TRANSFORM (see <see cref="SpectrumPlanes" />),
    /// so a caller passing a <c>bins</c> that disagreed with <paramref name="nFft" />
    /// got the transform's geometry regardless — a silent disagreement between
    /// what the call site says the shape is and what it is.
    /// <para>
    /// Not <c>[Obsolete]</c> yet, and the reason has SHRUNK since this was
    /// written (W4-S29): re-checked on 2026-09-11, the only caller left in the
    /// whole DotCharter tree is
    /// <c>DotCharter.Tests/Services/Audio/HtdemucsChunkScratchTests.cs</c>. No
    /// PRODUCT code calls it. So the stated reason — "DotCharter still calls it
    /// and builds with warnings as errors" — is now one TEST, which can be
    /// updated in the same change that adds the attribute; it is not the
    /// standing constraint it reads as. Removing it here alone would break that
    /// test's build, so the two have to move together. Call the 6-argument
    /// constructor; this one forwards to it and ignores <paramref name="bins" />
    /// exactly as it always did.
    /// </para>
    /// </summary>
    public HtdemucsChunkScratch(int stems, int segment, int timeFrames, int freqBins, int bins,
                                int nFft, int hop)
        : this(stems, segment, timeFrames, freqBins, nFft, hop)
    {
        _ = bins;
    }

    public HtdemucsChunkScratch(int stems, int segment, int timeFrames, int freqBins,
                                int nFft, int hop)
    {
        Stems = stems;
        Bins = nFft / 2 + 1;
        Left = new float[segment];
        Right = new float[segment];

        Interleaved = new float[stems][];
        SpecL = new SpectrumPlanes[stems];
        SpecR = new SpectrumPlanes[stems];
        for (var s = 0; s < stems; s++)
        {
            Interleaved[s] = new float[segment * 2];
            SpecL[s] = new SpectrumPlanes(timeFrames, nFft, hop) { OriginalLength = segment };
            SpecR[s] = new SpectrumPlanes(timeFrames, nFft, hop) { OriginalLength = segment };
        }

        // The analysis planes the forward pass fills, before the model runs.
        var forwardFrames = (int)Math.Ceiling((double)segment / hop);
        InL = new SpectrumPlanes(forwardFrames, nFft, hop);
        InR = new SpectrumPlanes(forwardFrames, nFft, hop);

        // One workspace per stem is the cheap arrangement: the inverse is serial
        // per frame, so a workspace holds three padded-length buffers and one FFT
        // plane rather than a float[nFft] per frame.
        var forwardPadded = hop / 2 * 3 * 2 + forwardFrames * hop;
        var inversePadded = (timeFrames + 4) * hop + nFft;
        var padded = Math.Max(forwardPadded, inversePadded);
        Work = new StftWorkspace[Math.Max(2, stems)];
        for (var i = 0; i < Work.Length; i++)
            Work[i] = new StftWorkspace(nFft, padded, Math.Max(forwardFrames, timeFrames));

        _specOut = new float[stems * 4 * freqBins * timeFrames];
        _waveOut = new float[stems * 2 * segment];
    }

    public int Stems { get; }

    /// <summary>
    /// The bin count every plane here actually has: <c>nFft / 2 + 1</c>, derived
    /// from the transform and from nothing else. Exposed so a caller can assert
    /// the geometry instead of passing its own idea of it in.
    /// </summary>
    public int Bins { get; }

    /// <summary>Left/right channel extracts of the current chunk, <c>[segment]</c>.</summary>
    public float[] Left { get; }

    public float[] Right { get; }

    /// <summary>Per-stem interleaved-stereo chunk output, <c>[stems][segment*2]</c>.</summary>
    public float[][] Interleaved { get; }

    /// <summary>Per-stem reconstruction spectrograms.</summary>
    public SpectrumPlanes[] SpecL { get; }

    public SpectrumPlanes[] SpecR { get; }

    /// <summary>Analysis spectrograms of the current chunk's two channels.</summary>
    public SpectrumPlanes InL { get; }

    public SpectrumPlanes InR { get; }

    /// <summary>One transform workspace per stem (index 0/1 double as the
    /// forward pass's two).</summary>
    public StftWorkspace[] Work { get; }

    /// <summary>
    /// The buffer the model's spectrogram output is copied into. Grows only if
    /// the real tensor is larger than the shape we sized from — normally a
    /// no-op that hands back the same array for the life of the instance.
    /// </summary>
    public float[] SpecOut(int length) => Ensure(ref _specOut, length);

    /// <summary>Counterpart for the waveform branch.</summary>
    public float[] WaveOut(int length) => Ensure(ref _waveOut, length);

    /// <summary>Total bytes held. Reported so the cost of the one live set is
    /// visible rather than inferred.</summary>
    public long EstimatedBytes
    {
        get
        {
            long total = 4L * (Left.Length + Right.Length + _specOut.Length + _waveOut.Length);
            foreach (var buf in Interleaved) total += 4L * buf.Length;
            foreach (var p in SpecL) total += PlaneBytes(p);
            foreach (var p in SpecR) total += PlaneBytes(p);
            total += PlaneBytes(InL) + PlaneBytes(InR);
            foreach (var w in Work) total += w.EstimatedBytes;
            return total;
        }
    }

    private static long PlaneBytes(SpectrumPlanes p) => 2L * p.FrameCount * p.BinCount * sizeof(float);

    private static float[] Ensure(ref float[] buffer, int length)
    {
        if (buffer.Length < length) buffer = new float[length];
        return buffer;
    }
}
