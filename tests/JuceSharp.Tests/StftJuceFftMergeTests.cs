using JuceSharp.Dsp;
using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Cleanup 18/19. The ONNX STFT ran on MathNet's <c>Fourier</c> over
/// <see cref="System.Numerics.Complex"/> — two doubles, 16 bytes a bin, for data
/// that arrives from a float32 model and goes straight back into one — while the
/// repo already owned a float radix-2 FFT (<see cref="JuceFft"/>) that the whole
/// detection path runs on. Two FFT implementations, and the expensive one stored
/// its results at 2x the width.
///
/// <para>The reference here is a NAIVE O(N^2) DFT in <c>double</c>, computed in
/// this file. That is a stronger check than golden vectors captured from MathNet
/// would have been: it is an independent implementation of the definition, so it
/// catches a shared bug in both FFTs, and it cannot rot.</para>
/// </summary>
[Collection("AllocationProbes")]
public sealed class StftJuceFftMergeTests
{
    // ------------------------------------------------------- JuceFft additions

    /// <summary>Textbook DFT, double precision: X[k] = sum_n x[n] e^(-2i.pi.kn/N).</summary>
    private static (double[] Re, double[] Im) NaiveDft(ReadOnlySpan<float> x)
    {
        var n = x.Length;
        var re = new double[n];
        var im = new double[n];
        for (var k = 0; k < n; k++)
        {
            double sr = 0, si = 0;
            for (var t = 0; t < n; t++)
            {
                var a = -2.0 * Math.PI * k * t / n;
                sr += x[t] * Math.Cos(a);
                si += x[t] * Math.Sin(a);
            }
            re[k] = sr;
            im[k] = si;
        }
        return (re, im);
    }

    private static float[] RandomFrame(int n, int seed)
    {
        var rng = new Random(seed);
        var x = new float[n];
        for (var i = 0; i < n; i++) x[i] = (float)(rng.NextDouble() * 2 - 1);
        return x;
    }

    [Fact]
    public void JuceFft_ForwardMatchesADoubleReferenceDft()
    {
        const int n = 256;
        var x = RandomFrame(n, 1234);
        var (refRe, refIm) = NaiveDft(x);

        var fft = new JuceFft(n);
        var re = new float[n];
        var im = new float[n];
        fft.ForwardReal(x, re, im);

        var peak = 0.0;
        for (var k = 0; k < n; k++) peak = Math.Max(peak, Math.Sqrt(refRe[k] * refRe[k] + refIm[k] * refIm[k]));
        Assert.True(peak > 1.0, "the fixture must have a real spectrum");

        for (var k = 0; k < n; k++)
        {
            Assert.True(Math.Abs(re[k] - refRe[k]) / peak < 1e-4, $"bin {k} real: {re[k]} vs {refRe[k]}");
            Assert.True(Math.Abs(im[k] - refIm[k]) / peak < 1e-4, $"bin {k} imag: {im[k]} vs {refIm[k]}");
        }
    }

    /// <summary>The inverse must undo the forward — same "Matlab" convention
    /// MathNet's <c>FourierOptions.Matlab</c> used (no forward scaling, 1/N on
    /// the inverse), or every reconstructed stem comes back N times too loud.</summary>
    [Fact]
    public void JuceFft_InverseUndoesForward()
    {
        const int n = 512;
        var x = RandomFrame(n, 99);
        var fft = new JuceFft(n);
        var re = new float[n];
        var im = new float[n];
        fft.ForwardReal(x, re, im);
        fft.Inverse(re, im);

        for (var i = 0; i < n; i++)
        {
            Assert.Equal(x[i], re[i], 4);
            Assert.True(Math.Abs(im[i]) < 1e-4f, $"imaginary residue at {i}: {im[i]}");
        }
    }

    /// <summary>The half-spectrum inverse rebuilds the upper half by conjugate
    /// symmetry; the result must be the same real signal.</summary>
    [Fact]
    public void JuceFft_InverseRealRoundTripsThroughAHalfSpectrum()
    {
        const int n = 512;
        var x = RandomFrame(n, 7);
        var fft = new JuceFft(n);
        var re = new float[n];
        var im = new float[n];
        fft.ForwardReal(x, re, im);

        var bins = n / 2 + 1;
        var binRe = re[..bins].ToArray();
        var binIm = im[..bins].ToArray();

        fft.InverseReal(binRe, binIm, re, im);
        for (var i = 0; i < n; i++)
            Assert.True(MathF.Abs(x[i] - re[i]) < 1e-5f, $"sample {i}: {re[i]} vs {x[i]}");
    }

    /// <summary>The pre-existing forward API must be BIT-identical — every CQT
    /// and analyzer number in the repo comes out of it.</summary>
    [Fact]
    public void JuceFft_ExistingForwardIsUnchangedByTheNewOverloads()
    {
        const int n = 1024;
        var x = RandomFrame(n, 4242);

        var fft = new JuceFft(n);
        var reA = new float[n];
        var imA = new float[n];
        x.CopyTo(reA, 0);
        fft.Forward(reA, imA);

        var reB = new float[n];
        var imB = new float[n];
        fft.ForwardReal(x, reB, imB);

        for (var i = 0; i < n; i++)
        {
            Assert.Equal(reA[i], reB[i]);   // exact, not approximate
            Assert.Equal(imA[i], imB[i]);
        }
    }

    // ----------------------------------------------------------- Stft on JuceFft

    /// <summary>
    /// Frame 3 of a real STFT against the naive double DFT of the same windowed
    /// frame. This is the equivalence the MathNet removal turns on.
    /// </summary>
    [Fact]
    public void Stft_ForwardFrameMatchesADoubleReferenceDft()
    {
        const int nFft = 256;
        const int hop = 64;
        var signal = RandomFrame(4096, 31337);

        var pad = nFft / 2;
        var paddedLen = signal.Length + 2 * pad;
        var frames = (paddedLen - nFft) / hop + 1;
        var planes = new SpectrumPlanes(frames, nFft, hop);
        var work = new StftWorkspace(nFft, paddedLen, frames);
        Stft.Forward(signal, planes, work);

        // Rebuild frame 3 by hand: the padded signal, times the symmetric Hann.
        const int f = 3;
        var window = Stft.HannWindow(nFft);
        var frame = new float[nFft];
        for (var i = 0; i < nFft; i++)
        {
            var idx = f * hop + i - pad;
            frame[i] = (idx >= 0 && idx < signal.Length ? signal[idx] : 0f) * window[i];
        }
        var (refRe, refIm) = NaiveDft(frame);

        var peak = 0.0;
        for (var k = 0; k <= nFft / 2; k++) peak = Math.Max(peak, Math.Sqrt(refRe[k] * refRe[k] + refIm[k] * refIm[k]));

        for (var k = 0; k <= nFft / 2; k++)
        {
            Assert.True(Math.Abs(planes.Re[f][k] - refRe[k]) / peak < 1e-4,
                $"bin {k} real: {planes.Re[f][k]} vs {refRe[k]} (peak {peak})");
            Assert.True(Math.Abs(planes.Im[f][k] - refIm[k]) / peak < 1e-4,
                $"bin {k} imag: {planes.Im[f][k]} vs {refIm[k]} (peak {peak})");
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Stft_CentredRoundTripReconstructsTheSignal(bool periodic)
    {
        const int nFft = 512;
        const int hop = 128;
        var signal = RandomFrame(8192, 5);

        var pad = nFft / 2;
        var paddedLen = signal.Length + 2 * pad;
        var frames = (paddedLen - nFft) / hop + 1;
        var planes = new SpectrumPlanes(frames, nFft, hop);
        var work = new StftWorkspace(nFft, paddedLen, frames);
        Stft.Forward(signal, planes, work, periodic);

        var back = new float[signal.Length];
        Stft.Inverse(planes, back, work, periodic);

        // Skip the first and last frame: COLA normalisation is exact only where
        // the window sum is complete.
        for (var i = nFft; i < signal.Length - nFft; i++)
            Assert.True(MathF.Abs(signal[i] - back[i]) < 1e-4f, $"sample {i}: {back[i]} vs {signal[i]}");
    }

    [Fact]
    public void Stft_HtdemucsRoundTripReconstructsTheSignal()
    {
        const int nFft = 512;
        const int hop = 128;
        var signal = RandomFrame(4096, 11);

        var frames = (int)Math.Ceiling((double)signal.Length / hop);
        var planes = new SpectrumPlanes(frames, nFft, hop);
        var padded = Math.Max(hop / 2 * 3 * 2 + frames * hop, (frames + 4) * hop + nFft);
        var work = new StftWorkspace(nFft, padded, frames);

        Stft.ForwardHtdemucs(signal, planes, work);
        var back = new float[signal.Length];
        Stft.InverseHtdemucs(planes, back, work);

        for (var i = nFft; i < signal.Length - nFft; i++)
            Assert.True(MathF.Abs(signal[i] - back[i]) < 1e-4f, $"sample {i}: {back[i]} vs {signal[i]}");
    }

    /// <summary>
    /// The <see cref="System.Numerics.Complex"/> wrappers are what external
    /// callers (and the parent app's tests) still use; they must agree with the
    /// plane form they now delegate to.
    /// </summary>
    [Fact]
    public void Stft_ComplexWrappersAgreeWithThePlaneForm()
    {
        const int nFft = 256;
        const int hop = 64;
        var signal = RandomFrame(2048, 17);

        var legacy = Stft.Forward(signal, nFft, hop);

        var pad = nFft / 2;
        var paddedLen = signal.Length + 2 * pad;
        var frames = (paddedLen - nFft) / hop + 1;
        var planes = new SpectrumPlanes(frames, nFft, hop);
        Stft.Forward(signal, planes, new StftWorkspace(nFft, paddedLen, frames));

        Assert.Equal(frames, legacy.FrameCount);
        for (var f = 0; f < frames; f++)
        for (var b = 0; b < legacy.BinCount; b++)
        {
            Assert.Equal(planes.Re[f][b], (float)legacy.Frames[f][b].Real);
            Assert.Equal(planes.Im[f][b], (float)legacy.Frames[f][b].Imaginary);
        }
    }

    // -------------------------------------------------------------- allocation

    /// <summary>
    /// THE POINT of the workspace. The old transforms allocated a
    /// <c>Complex[nFft]</c> per worker, a <c>Complex[bins]</c> PER FRAME, and in
    /// the inverse an output, a norm, a <c>float[nFft]</c> per frame and the
    /// trimmed result — roughly 140 MB per chunk on the shipping shape.
    /// </summary>
    [Fact]
    public void Stft_ASecondForwardAndInverseAllocateAlmostNothing()
    {
        const int nFft = 1024;
        const int hop = 256;
        var signal = RandomFrame(32768, 3);

        var frames = (int)Math.Ceiling((double)signal.Length / hop);
        var planes = new SpectrumPlanes(frames, nFft, hop);
        var padded = Math.Max(hop / 2 * 3 * 2 + frames * hop, (frames + 4) * hop + nFft);
        var work = new StftWorkspace(nFft, padded, frames);
        var back = new float[signal.Length];

        // Warm: first call touches every pooled plane and JITs both paths.
        Stft.ForwardHtdemucs(signal, planes, work);
        Stft.InverseHtdemucs(planes, back, work);

        var before = GC.GetAllocatedBytesForCurrentThread();
        Stft.ForwardHtdemucs(signal, planes, work);
        Stft.InverseHtdemucs(planes, back, work);
        var delta = GC.GetAllocatedBytesForCurrentThread() - before;

        // The only remaining allocation is Parallel.For's own bookkeeping in the
        // forward. One frame of the OLD code allocated more than this on its own
        // (a Complex[513] row is 8,232 bytes; there are 128 frames).
        Assert.True(delta < 16 * 1024,
            $"a warm forward+inverse allocated {delta} bytes; the transforms must run on the workspace");
    }

    // ------------------------------------------------------------ window forms

    /// <summary>
    /// HTDemucs' reference form is <c>sin(pi.i/n)^2</c>; the shared
    /// <c>Windows.Hann(n, periodic: true)</c> computes
    /// <c>0.5(1 - cos(2.pi.i/n))</c>. They are algebraically identical, and this
    /// pins that they are also identical in float to well under a bit.
    /// </summary>
    [Theory]
    [InlineData(256)]
    [InlineData(4096)]
    public void PeriodicHann_MatchesTheSinSquaredFormHtdemucsUses(int n)
    {
        var w = Stft.HannWindowPeriodic(n);
        for (var i = 0; i < n; i++)
        {
            var s = Math.Sin(Math.PI * i / n);
            Assert.True(Math.Abs(w[i] - s * s) < 1e-6, $"tap {i}: {w[i]} vs {s * s}");
        }
    }

    /// <summary>The two forms are NOT the same window, which is why the choice
    /// has to be explicit per model.</summary>
    [Fact]
    public void SymmetricAndPeriodicHann_AreDifferentWindows()
    {
        const int n = 256;
        var sym = Stft.HannWindow(n);
        var per = Stft.HannWindowPeriodic(n);
        Assert.NotEqual(sym[1], per[1]);
        Assert.Equal(0f, sym[n - 1], 6);          // symmetric closes on zero
        Assert.True(per[n - 1] > 1e-4f);          // periodic does not
    }

    /// <summary>A window handed out must be a private copy — a caller that
    /// mutated the shared cache would corrupt every later transform.</summary>
    [Fact]
    public void PublicWindowFactoriesHandOutPrivateCopies()
    {
        var a = Stft.HannWindowPeriodic(64);
        var b = Stft.HannWindowPeriodic(64);
        Assert.NotSame(a, b);
        a[10] = -999f;
        Assert.NotEqual(-999f, Stft.HannWindowPeriodic(64)[10]);
    }
}
