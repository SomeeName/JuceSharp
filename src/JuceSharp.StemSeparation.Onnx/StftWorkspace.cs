using JuceSharp.Dsp;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Every per-call buffer <see cref="Stft"/> needs, allocated ONCE for a whole
/// prediction.
/// <para>The transforms used to allocate inside the chunk loop: a
/// <c>padded</c> signal, a <c>Complex[nFft]</c> per parallel worker, a
/// <c>Complex[bins]</c> PER FRAME, and in the inverse an <c>output</c>, a
/// <c>norm</c>, a <c>float[nFft]</c> per frame and the trimmed result — roughly
/// 140 MB per chunk, or ~4.5 GB of Large Object Heap churn over a 3:30 song.</para>
/// <para><b>Not thread-safe across calls</b>: one workspace drives one
/// transform at a time. It is safe INSIDE a transform because each parallel
/// worker rents its own scratch plane and each frame owns its own output row.</para>
/// </summary>
public sealed class StftWorkspace
{
    private readonly float[]?[] _pool;
    private int _cursor;

    /// <param name="nFft">Transform size. A power of two runs <see cref="JuceFft"/>'s
    /// radix-2 path; any other even size runs its Bluestein path, which MDX-Net's
    /// <c>n_fft = 6144</c> needs.</param>
    /// <param name="maxPaddedLength">Largest padded signal length any call will use.</param>
    /// <param name="maxFrames">Largest frame count any call will use.</param>
    public StftWorkspace(int nFft, int maxPaddedLength, int maxFrames)
    {
        NFft = nFft;
        MaxFrames = maxFrames;
        Fft = new JuceFft(nFft);
        Padded = new float[maxPaddedLength];
        Output = new float[maxPaddedLength];
        Norm = new float[maxPaddedLength];
        Mono = new float[maxPaddedLength];
        // One scratch plane per potential worker. Renting past the end still
        // works (it just allocates), so an unusually wide Parallel.For is a
        // slowdown rather than a crash.
        _pool = new float[Environment.ProcessorCount + 2][];
    }

    /// <summary>Total bytes this workspace holds — reported so a caller can see
    /// what a per-stem set costs before it allocates one.</summary>
    public long EstimatedBytes =>
        4L * (Padded.Length + Output.Length + Norm.Length + Mono.Length) + 8L * NFft * _pool.Length;

    internal int NFft { get; }

    internal JuceFft Fft { get; }

    internal float[] Padded { get; }

    internal float[] Output { get; }

    internal float[] Norm { get; }

    internal int MaxFrames { get; }

    /// <summary>A reusable single-channel output buffer for the caller's
    /// reconstructed signal — so a driver never allocates one per iSTFT.</summary>
    public float[] Mono { get; }

    /// <summary>Hand every worker back its plane slot. Called once per transform,
    /// before the parallel loop starts.</summary>
    internal void ResetPool() => Volatile.Write(ref _cursor, 0);

    /// <summary>A <c>2 * nFft</c> plane: real half first, imaginary half second.
    /// Allocation-free once every slot has been touched.</summary>
    internal float[] RentPlane()
    {
        var i = Interlocked.Increment(ref _cursor) - 1;
        if (i >= _pool.Length) return new float[2 * NFft];
        return _pool[i] ??= new float[2 * NFft];
    }

    internal void EnsureCapacity(int paddedLength, int frames)
    {
        if (paddedLength > Padded.Length || frames > MaxFrames)
            throw new ArgumentException(
                $"StftWorkspace was sized for {Padded.Length} padded samples / {MaxFrames} " +
                $"frames, but this call needs {paddedLength} / {frames}. Size it for the largest chunk.");
    }
}
