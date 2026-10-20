namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// A complex spectrogram stored as two FLOAT planes, <c>[frames][bins]</c>.
/// <para><b>Why not <see cref="System.Numerics.Complex"/>.</b> That type is two
/// <c>double</c>s: 16 bytes per bin against this 8, for data that comes out of
/// a float32 model and goes back into one. On the shipping HTDemucs shape
/// (6 stems x 2 channels x 336 frames x 2049 bins) that difference alone is
/// <b>132 MB -&gt; 66 MB</b> of per-instance scratch, all of it Large Object
/// Heap.</para>
/// <para>The planes are allocated once and OVERWRITTEN in place, which is what
/// makes the driver's chunk loop allocation-free. Per-frame rows are distinct
/// arrays so a <c>Parallel.For</c> over frames can write them concurrently.</para>
/// </summary>
public sealed class SpectrumPlanes
{
    /// <summary>Allocate planes for <paramref name="frames"/> frames of
    /// <c>nFft/2 + 1</c> bins.</summary>
    public SpectrumPlanes(int frames, int nFft, int hop)
    {
        if (frames < 0) throw new ArgumentOutOfRangeException(nameof(frames));
        // NOT a power-of-two check. MDX-Net's exported graphs are built around
        // n_fft = 6144 (3 * 2048, dim_f = 3072), and this assertion — together
        // with the identical one JuceFft used to carry — threw on the first
        // chunk of every MDX-Net model in the catalogue. Even is still required:
        // BinCount is nFft/2 + 1 with a self-conjugate Nyquist bin, which an odd
        // size does not have.
        if (nFft <= 0) throw new ArgumentException("nFft must be > 0.", nameof(nFft));
        if (nFft % 2 != 0) throw new ArgumentException("nFft must be even.", nameof(nFft));
        if (hop <= 0) throw new ArgumentException("hop must be > 0.", nameof(hop));

        NFft = nFft;
        Hop = hop;
        var bins = nFft / 2 + 1;
        Re = new float[frames][];
        Im = new float[frames][];
        for (var f = 0; f < frames; f++)
        {
            Re[f] = new float[bins];
            Im[f] = new float[bins];
        }
    }

    /// <summary>Real parts, <c>[frames][nFft/2 + 1]</c>.</summary>
    public float[][] Re { get; }

    /// <summary>Imaginary parts, <c>[frames][nFft/2 + 1]</c>.</summary>
    public float[][] Im { get; }

    /// <summary>STFT transform size in samples these planes were computed with.</summary>
    public int NFft { get; }

    /// <summary>STFT hop in samples between adjacent frames.</summary>
    public int Hop { get; }

    /// <summary>Length in samples of the signal this spectrogram reconstructs.
    /// Settable because the planes are reused across chunks.</summary>
    public int OriginalLength { get; set; }

    /// <summary>Number of analysis frames held — the first index into <see cref="Re"/>
    /// and <see cref="Im"/>.</summary>
    public int FrameCount => Re.Length;

    /// <summary>Bins per frame: <c>NFft / 2 + 1</c>, DC and Nyquist both included.</summary>
    public int BinCount => NFft / 2 + 1;

    /// <summary>Zero every bin of every frame.</summary>
    public void Clear()
    {
        foreach (var row in Re) Array.Clear(row);
        foreach (var row in Im) Array.Clear(row);
    }
}
