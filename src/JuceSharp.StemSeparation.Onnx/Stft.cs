using System.Numerics;
using JuceSharp.Dsp;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Short-time Fourier transform and overlap-add inverse used by the ONNX
/// stem-separation backends.
/// </summary>
/// <remarks>
/// <para>The transforms run on <see cref="JuceFft"/> — the same float radix-2
/// engine the managed detection path uses — over FLOAT planes
/// (<see cref="SpectrumPlanes"/>) with caller-owned scratch
/// (<see cref="StftWorkspace"/>). They previously ran on MathNet's
/// <c>Fourier</c> over <see cref="Complex"/>, which is double: 16 bytes per bin
/// for data that arrives from a float32 model and goes straight back into one.</para>
/// <para><b>Two Hann forms, and the choice is per MODEL, not per taste.</b>
/// <c>periodic: true</c> (divisor <c>N</c>) is what <c>torch.hann_window</c>
/// produces, so it is what MDX-Net, BS-RoFormer and HTDemucs were TRAINED with;
/// <c>periodic: false</c> (divisor <c>N-1</c>, the symmetric analysis form) is
/// the historical default of the centred <c>Forward</c> / <c>Inverse</c> pair
/// and is kept as their default only so the public API stays compatible.
/// HTDemucs always used the periodic form here. Every DRIVER now asks for
/// periodic.</para>
/// <para>The <see cref="Complex"/>-based <see cref="SpectrogramFrames"/> surface
/// is retained for external callers and converts to and from the planes.</para>
/// </remarks>
public static class Stft
{
    /// <summary>A centred STFT as complex frames. Retained for external callers; the
    /// internal drivers use the split <c>SpectrumPlanes</c> form, which this converts to
    /// and from.</summary>
    /// <param name="Frames">The spectrum, <c>[frame][nFft/2 + 1]</c>.</param>
    /// <param name="NFft">Transform size in samples.</param>
    /// <param name="Hop">Hop in samples between frames.</param>
    /// <param name="OriginalLength">Length in samples of the signal this reconstructs, so
    /// the inverse can trim the padding back off.</param>
    public sealed record SpectrogramFrames(Complex[][] Frames, int NFft, int Hop, int OriginalLength)
    {
        /// <summary>Number of analysis frames held.</summary>
        public int FrameCount => Frames.Length;

        /// <summary>Bins per frame: <c>NFft / 2 + 1</c>, DC and Nyquist both included.</summary>
        public int BinCount   => NFft / 2 + 1;
    }

    /// <summary>Symmetric ("analysis") Hann window: divisor <c>n - 1</c>.</summary>
    public static float[] HannWindow(int n) => (float[])CachedWindow(n, periodic: false).Clone();

    /// <summary>
    /// Periodic Hann window used by <c>torch.hann_window</c> / HTDemucs:
    /// divisor <c>n</c>. This is the variant whose squared windows satisfy COLA
    /// for hop = N/4.
    /// </summary>
    public static float[] HannWindowPeriodic(int n) => (float[])CachedWindow(n, periodic: true).Clone();

    /// <summary>
    /// Memoized windows. INTERNAL and read-only by contract — the public
    /// factories above hand out a private copy, because a caller that mutated a
    /// shared window would corrupt every subsequent transform.
    /// <para>HTDemucs' form is <c>sin(pi*i/n)^2</c>, which is algebraically
    /// identical to <c>0.5*(1 - cos(2*pi*i/n))</c>; the shared
    /// <c>Windows.Hann</c> computes the cosine form. Verified equal to within
    /// one float ulp by <c>StftWindowFormTests</c>.</para>
    /// </summary>
    private static float[] CachedWindow(int n, bool periodic) => Windows.Hann(n, periodic);

    // ---------------------------------------------------------------- HTDemucs

    /// <summary>
    /// HTDemucs / hybrid-Demucs STFT contract (reference: facebookresearch
    /// demucs htdemucs.py line 433-439). Differs from the canonical centred
    /// STFT in two ways:
    /// <list type="bullet">
    ///   <item>Padding is <c>hop/2 * 3</c> samples on each side
    ///         (1536 for hop=1024), NOT <c>nFft/2</c>.</item>
    ///   <item>After the forward FFT the first 2 and the trailing frames are
    ///         discarded, leaving exactly <c>ceil(N/hop)</c> frames.</item>
    /// </list>
    /// Bin 0 is DC and bin <c>nFft/2</c> is Nyquist (the caller drops Nyquist
    /// when feeding the network — HTDemucs keeps DC and discards Nyquist to
    /// land on a power-of-two bin count).
    /// </summary>
    public static void ForwardHtdemucs(ReadOnlySpan<float> signal, SpectrumPlanes dest, StftWorkspace work)
    {
        var nFft = dest.NFft;
        var hop = dest.Hop;
        var window = CachedWindow(nFft, periodic: true);
        var padLeft = hop / 2 * 3;
        var expectedFrames = (int)Math.Ceiling((double)signal.Length / hop);
        if (expectedFrames != dest.FrameCount)
            throw new ArgumentException(
                $"Destination holds {dest.FrameCount} frames but this signal needs {expectedFrames}.",
                nameof(dest));

        var paddedLen = padLeft + expectedFrames * hop + padLeft;
        work.EnsureCapacity(paddedLen, expectedFrames);
        var padded = work.Padded;
        Array.Clear(padded, 0, paddedLen);
        signal.CopyTo(padded.AsSpan(padLeft));
        dest.OriginalLength = signal.Length;

        // We compute framesAvailable frames then keep [2 .. 2+expectedFrames).
        work.ResetPool();
        var fft = work.Fft;
        Parallel.For(0, expectedFrames, work.RentPlane, (f, _, plane) =>
        {
            var re = plane.AsSpan(0, nFft);
            var im = plane.AsSpan(nFft, nFft);
            var start = (f + 2) * hop;
            for (var i = 0; i < nFft; i++)
            {
                var idx = start + i;
                re[i] = idx < paddedLen ? padded[idx] * window[i] : 0f;
                im[i] = 0f;
            }
            fft.Forward(re, im);
            var outRe = dest.Re[f];
            var outIm = dest.Im[f];
            for (var b = 0; b < outRe.Length; b++) { outRe[b] = re[b]; outIm[b] = im[b]; }
            return plane;
        }, static _ => { });
    }

    /// <summary>
    /// Inverse of <see cref="ForwardHtdemucs(ReadOnlySpan{float}, SpectrumPlanes, StftWorkspace)"/>.
    /// Pads the spectrogram with 2 silence frames each side (matching the frames
    /// the forward pass discarded) before overlap-add, then strips
    /// <c>hop/2 * 3</c> samples from the front. Normalisation is
    /// <c>sum(window^2)</c>, so adjacent overlapping frames cancel at unity.
    /// Writes <c>spec.OriginalLength</c> samples into <paramref name="dest"/>.
    /// </summary>
    public static void InverseHtdemucs(SpectrumPlanes spec, Span<float> dest, StftWorkspace work)
    {
        var nFft = spec.NFft;
        var hop = spec.Hop;
        var window = CachedWindow(nFft, periodic: true);
        var padLeft = hop / 2 * 3;
        var framesWithPad = spec.FrameCount + 4;
        var paddedLen = framesWithPad * hop + nFft;
        work.EnsureCapacity(paddedLen, spec.FrameCount);
        if (dest.Length < spec.OriginalLength)
            throw new ArgumentException("Destination is shorter than the signal being reconstructed.", nameof(dest));

        OverlapAdd(spec, work, window, paddedLen, frameStartOffset: 2 * hop);

        var output = work.Output;
        var norm = work.Norm;
        for (var i = 0; i < spec.OriginalLength; i++)
        {
            var src = i + padLeft;
            dest[i] = src < paddedLen && norm[src] > 1e-8f ? output[src] / norm[src] : 0f;
        }
    }

    // ------------------------------------------------------- centred transform

    /// <summary>
    /// Centred STFT: the signal is zero-padded by <c>nFft/2</c> on each side so
    /// the first frame is centred on sample 0 (matches <c>torch.stft(center=True)</c>).
    /// </summary>
    /// <param name="signal">Mono input.</param>
    /// <param name="dest">Planes to fill; its frame count must match the signal.</param>
    /// <param name="work">Caller-owned scratch.</param>
    /// <param name="periodic">
    /// <c>true</c> for the <c>torch.hann_window</c> form the models were trained
    /// with. Defaults to <c>false</c> (symmetric) purely to keep the historical
    /// public behaviour; drivers pass <c>true</c>.
    /// </param>
    public static void Forward(ReadOnlySpan<float> signal, SpectrumPlanes dest, StftWorkspace work,
                               bool periodic = false)
        => Forward(signal, dest, work, periodic, StftPadding.Zero);

    /// <summary>
    /// Centred STFT with an explicit edge padding. <see cref="StftPadding.Reflect"/> is
    /// <c>torch.stft(center=True)</c>'s DEFAULT (<c>pad_mode="reflect"</c>), so it is what a
    /// model trained through <c>torch.stft</c> saw at the chunk edges; BS-RoFormer SW is one,
    /// and its exported reference spectrogram only matches with it. The overload without a
    /// padding argument keeps the zero padding it has always had.
    /// </summary>
    /// <param name="signal">Mono input. For <see cref="StftPadding.Reflect"/> it must be
    /// longer than <c>nFft / 2</c>, the same requirement <c>torch.stft</c> has.</param>
    /// <param name="dest">Planes to fill; its frame count must match the signal.</param>
    /// <param name="work">Caller-owned scratch.</param>
    /// <param name="periodic"><c>true</c> for the <c>torch.hann_window</c> form.</param>
    /// <param name="padding">How the <c>nFft / 2</c> samples beyond each edge are filled.</param>
    public static void Forward(ReadOnlySpan<float> signal, SpectrumPlanes dest, StftWorkspace work,
                               bool periodic, StftPadding padding)
    {
        var nFft = dest.NFft;
        var hop = dest.Hop;
        var window = CachedWindow(nFft, periodic);
        var pad = nFft / 2;
        var paddedLen = signal.Length + 2 * pad;
        var frameCount = Math.Max(1, (paddedLen - nFft) / hop + 1);
        if (frameCount != dest.FrameCount)
            throw new ArgumentException(
                $"Destination holds {dest.FrameCount} frames but this signal needs {frameCount}.",
                nameof(dest));

        work.EnsureCapacity(paddedLen, frameCount);
        var padded = work.Padded;
        Array.Clear(padded, 0, paddedLen);
        signal.CopyTo(padded.AsSpan(pad));
        if (padding == StftPadding.Reflect) ReflectEdges(padded, pad, signal.Length);
        dest.OriginalLength = signal.Length;

        work.ResetPool();
        var fft = work.Fft;
        Parallel.For(0, frameCount, work.RentPlane, (f, _, plane) =>
        {
            var re = plane.AsSpan(0, nFft);
            var im = plane.AsSpan(nFft, nFft);
            var start = f * hop;
            for (var i = 0; i < nFft; i++)
            {
                var idx = start + i;
                re[i] = idx < paddedLen ? padded[idx] * window[i] : 0f;
                im[i] = 0f;
            }
            fft.Forward(re, im);
            var outRe = dest.Re[f];
            var outIm = dest.Im[f];
            for (var b = 0; b < outRe.Length; b++) { outRe[b] = re[b]; outIm[b] = im[b]; }
            return plane;
        }, static _ => { });
    }

    /// <summary>
    /// <c>numpy.pad(mode="reflect")</c> / <c>torch</c>'s reflect: the edge sample is NOT
    /// repeated, so <c>x[-k] = x[k]</c> and <c>x[n-1+k] = x[n-1-k]</c>.
    /// </summary>
    private static void ReflectEdges(float[] padded, int pad, int length)
    {
        if (length <= pad)
            throw new ArgumentException(
                $"Reflect padding needs a signal longer than {pad} samples; this one has {length}.");
        for (var k = 1; k <= pad; k++)
        {
            padded[pad - k] = padded[pad + k];
            padded[pad + length - 1 + k] = padded[pad + length - 1 - k];
        }
    }

    /// <summary>
    /// Inverse of <see cref="Forward(ReadOnlySpan{float}, SpectrumPlanes, StftWorkspace, bool)"/>:
    /// overlap-add with COLA-normalised Hann windows, trimmed back to
    /// <see cref="SpectrumPlanes.OriginalLength"/>.
    /// </summary>
    public static void Inverse(SpectrumPlanes spec, Span<float> dest, StftWorkspace work,
                               bool periodic = false)
    {
        var nFft = spec.NFft;
        var pad = nFft / 2;
        var window = CachedWindow(nFft, periodic);
        var paddedLen = spec.OriginalLength + 2 * pad;
        work.EnsureCapacity(paddedLen, spec.FrameCount);
        if (dest.Length < spec.OriginalLength)
            throw new ArgumentException("Destination is shorter than the signal being reconstructed.", nameof(dest));

        OverlapAdd(spec, work, window, paddedLen, frameStartOffset: 0);

        var output = work.Output;
        var norm = work.Norm;
        for (var i = 0; i < spec.OriginalLength; i++)
        {
            var src = i + pad;
            dest[i] = src < paddedLen && norm[src] > 1e-8f ? output[src] / norm[src] : 0f;
        }
    }

    /// <summary>
    /// IFFT each frame and overlap-add it immediately.
    /// <para><b>Deliberately serial, and that is what makes it cheap.</b> The
    /// accumulate cannot be parallel (adjacent frames overlap), so a parallel
    /// phase 1 needs a <c>float[nFft]</c> PER FRAME to hand across — 5.5 MB per
    /// concurrent inverse at the shipping shape. Doing the IFFT inline needs one
    /// scratch plane. The parallelism that matters is one level up: the driver
    /// runs six stems at once, i.e. twelve of these.</para>
    /// </summary>
    private static void OverlapAdd(SpectrumPlanes spec, StftWorkspace work, float[] window,
                                   int paddedLen, int frameStartOffset)
    {
        var nFft = spec.NFft;
        var hop = spec.Hop;
        var output = work.Output;
        var norm = work.Norm;
        Array.Clear(output, 0, paddedLen);
        Array.Clear(norm, 0, paddedLen);

        var fft = work.Fft;
        work.ResetPool();
        var plane = work.RentPlane();
        var re = plane.AsSpan(0, nFft);
        var im = plane.AsSpan(nFft, nFft);
        for (var f = 0; f < spec.FrameCount; f++)
        {
            fft.InverseReal(spec.Re[f], spec.Im[f], re, im);
            var start = frameStartOffset + f * hop;
            var count = Math.Min(nFft, paddedLen - start);
            for (var i = 0; i < count; i++)
            {
                var w = window[i];
                output[start + i] += re[i] * w;
                norm[start + i] += w * w;
            }
        }
    }

    // --------------------------------------------- Complex compatibility layer

    /// <summary>Allocating <see cref="Complex"/> wrapper over
    /// <see cref="ForwardHtdemucs(ReadOnlySpan{float}, SpectrumPlanes, StftWorkspace)"/>.
    /// Kept for external callers; the drivers use the plane form.</summary>
    public static SpectrogramFrames ForwardHtdemucs(float[] signal, int nFft, int hop)
    {
        var frames = (int)Math.Ceiling((double)signal.Length / hop);
        var planes = new SpectrumPlanes(frames, nFft, hop);
        var padLeft = hop / 2 * 3;
        using var work = Lease(nFft, padLeft + frames * hop + padLeft, frames);
        ForwardHtdemucs(signal, planes, work.Value);
        return ToComplex(planes);
    }

    /// <summary>Allocating <see cref="Complex"/> wrapper over
    /// <see cref="InverseHtdemucs(SpectrumPlanes, Span{float}, StftWorkspace)"/>.</summary>
    public static float[] InverseHtdemucs(SpectrogramFrames spec)
    {
        var planes = FromComplex(spec);
        var paddedLen = (spec.FrameCount + 4) * spec.Hop + spec.NFft;
        using var work = Lease(spec.NFft, paddedLen, spec.FrameCount);
        var dest = new float[spec.OriginalLength];
        InverseHtdemucs(planes, dest, work.Value);
        return dest;
    }

    /// <summary>Allocating <see cref="Complex"/> wrapper over the centred forward
    /// transform. Uses the SYMMETRIC window, as it always has.</summary>
    public static SpectrogramFrames Forward(float[] signal, int nFft, int hop, bool periodic = false)
    {
        if (nFft <= 0) throw new ArgumentException("nFft must be > 0.", nameof(nFft));
        if (hop <= 0) throw new ArgumentException("hop must be > 0.", nameof(hop));
        var pad = nFft / 2;
        var paddedLen = signal.Length + 2 * pad;
        var frames = Math.Max(1, (paddedLen - nFft) / hop + 1);
        var planes = new SpectrumPlanes(frames, nFft, hop);
        using var work = Lease(nFft, paddedLen, frames);
        Forward(signal, planes, work.Value, periodic);
        return ToComplex(planes);
    }

    /// <summary>Allocating <see cref="Complex"/> wrapper over the centred inverse.</summary>
    public static float[] Inverse(SpectrogramFrames spec, bool periodic = false)
    {
        var planes = FromComplex(spec);
        var paddedLen = spec.OriginalLength + 2 * (spec.NFft / 2);
        using var work = Lease(spec.NFft, paddedLen, spec.FrameCount);
        var dest = new float[spec.OriginalLength];
        Inverse(planes, dest, work.Value, periodic);
        return dest;
    }

    private static SpectrogramFrames ToComplex(SpectrumPlanes planes)
    {
        var frames = new Complex[planes.FrameCount][];
        for (var f = 0; f < frames.Length; f++)
        {
            var re = planes.Re[f];
            var im = planes.Im[f];
            var row = new Complex[re.Length];
            for (var b = 0; b < row.Length; b++) row[b] = new Complex(re[b], im[b]);
            frames[f] = row;
        }
        return new SpectrogramFrames(frames, planes.NFft, planes.Hop, planes.OriginalLength);
    }

    private static SpectrumPlanes FromComplex(SpectrogramFrames spec)
    {
        var planes = new SpectrumPlanes(spec.FrameCount, spec.NFft, spec.Hop)
        {
            OriginalLength = spec.OriginalLength,
        };
        for (var f = 0; f < spec.FrameCount; f++)
        {
            var row = spec.Frames[f];
            var re = planes.Re[f];
            var im = planes.Im[f];
            for (var b = 0; b < re.Length && b < row.Length; b++)
            {
                re[b] = (float)row[b].Real;
                im[b] = (float)row[b].Imaginary;
            }
        }
        return planes;
    }

    /// <summary>
    /// A one-shot workspace for the allocating wrappers. These are convenience
    /// entry points, not the hot path — the drivers own a long-lived
    /// <see cref="StftWorkspace"/>.
    /// </summary>
    private readonly struct WorkspaceLease : IDisposable
    {
        public WorkspaceLease(StftWorkspace value) => Value = value;
        public StftWorkspace Value { get; }
        public void Dispose() { }
    }

    private static WorkspaceLease Lease(int nFft, int paddedLen, int frames)
        => new(new StftWorkspace(nFft, paddedLen, frames));
}

/// <summary>How a centred STFT fills the <c>nFft / 2</c> samples beyond each edge of the signal.</summary>
public enum StftPadding
{
    /// <summary>Zeros. This package's historical behaviour.</summary>
    Zero,

    /// <summary>A mirror of the signal that does not repeat the edge sample:
    /// <c>torch.stft(center=True)</c>'s default.</summary>
    Reflect,
}
