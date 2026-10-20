using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The non-power-of-two (Bluestein) path on <see cref="JuceFft"/>, and the
/// promise that adding it left the radix-2 path alone.
/// <para>W4-S1: <c>JuceFft</c> rejected every size that was not a power of two,
/// which made MDX-Net's <c>n_fft = 6144</c> — the geometry its ONNX graphs were
/// exported with — throw on the first chunk of every one of the five catalogue
/// entries that route there.</para>
/// </summary>
public sealed class JuceFftBluesteinTests
{
    /// <summary>
    /// The radix-2 butterfly EXACTLY as it stood before the Bluestein path was
    /// added, copied into the test so "unchanged" is asserted against code and
    /// not against a remembered number. Bit-for-bit, not within a tolerance.
    /// </summary>
    private static void LegacyRadix2Forward(float[] re, float[] im)
    {
        int size = re.Length;
        int log2 = (int)Math.Log2(size);
        var bitRev = new int[size];
        for (int i = 0; i < size; i++)
        {
            int r = 0, x = i;
            for (int b = 0; b < log2; b++) { r = (r << 1) | (x & 1); x >>= 1; }
            bitRev[i] = r;
        }
        var cos = new float[size / 2];
        var sin = new float[size / 2];
        for (int i = 0; i < size / 2; i++)
        {
            var a = -2.0 * Math.PI * i / size;
            cos[i] = (float)Math.Cos(a);
            sin[i] = (float)Math.Sin(a);
        }
        for (int i = 0; i < size; i++)
        {
            var j = bitRev[i];
            if (j > i)
            {
                (re[i], re[j]) = (re[j], re[i]);
                (im[i], im[j]) = (im[j], im[i]);
            }
        }
        for (int stage = 1; stage <= log2; stage++)
        {
            int half = 1 << (stage - 1);
            int full = half << 1;
            int step = size / full;
            for (int k = 0; k < size; k += full)
            {
                for (int j = 0; j < half; j++)
                {
                    var c = cos[j * step];
                    var s = sin[j * step];
                    int a = k + j;
                    int b = a + half;
                    var tre = re[b] * c - im[b] * s;
                    var tim = re[b] * s + im[b] * c;
                    re[b] = re[a] - tre;
                    im[b] = im[a] - tim;
                    re[a] += tre;
                    im[a] += tim;
                }
            }
        }
    }

    private static (double[] Re, double[] Im) NaiveDft(float[] re, float[] im)
    {
        int n = re.Length;
        var or_ = new double[n];
        var oi = new double[n];
        for (int k = 0; k < n; k++)
        {
            double sr = 0, si = 0;
            for (int j = 0; j < n; j++)
            {
                var ang = -2.0 * Math.PI * ((long)j * k % n) / n;
                double c = Math.Cos(ang), s = Math.Sin(ang);
                sr += re[j] * c - im[j] * s;
                si += re[j] * s + im[j] * c;
            }
            or_[k] = sr;
            oi[k] = si;
        }
        return (or_, oi);
    }

    private static (float[] Re, float[] Im) Noise(int n, int seed)
    {
        var rng = new Random(seed);
        var re = new float[n];
        var im = new float[n];
        for (int i = 0; i < n; i++)
        {
            re[i] = (float)(rng.NextDouble() * 2 - 1);
            im[i] = (float)(rng.NextDouble() * 2 - 1);
        }
        return (re, im);
    }

    [Theory]
    [InlineData(2)]
    [InlineData(64)]
    [InlineData(1024)]
    [InlineData(4096)]
    public void A_power_of_two_still_produces_the_pre_bluestein_bits(int size)
    {
        var (re, im) = Noise(size, seed: size);
        var legacyRe = (float[])re.Clone();
        var legacyIm = (float[])im.Clone();
        LegacyRadix2Forward(legacyRe, legacyIm);

        var fft = new JuceFft(size);
        Assert.True(fft.IsRadix2);
        fft.Forward(re, im);

        for (int i = 0; i < size; i++)
        {
            Assert.Equal(BitConverter.SingleToInt32Bits(legacyRe[i]), BitConverter.SingleToInt32Bits(re[i]));
            Assert.Equal(BitConverter.SingleToInt32Bits(legacyIm[i]), BitConverter.SingleToInt32Bits(im[i]));
        }
    }

    [Theory]
    [InlineData(3)]
    [InlineData(5)]
    [InlineData(6)]
    [InlineData(9)]
    [InlineData(12)]
    [InlineData(100)]
    [InlineData(384)]   // 3 * 128 - the radix-3 path
    [InlineData(1000)]  // 8 * 125 - the general Bluestein path
    public void A_non_power_of_two_matches_a_naive_dft(int size)
    {
        var (re, im) = Noise(size, seed: 7 + size);
        var (refRe, refIm) = NaiveDft(re, im);

        var fft = new JuceFft(size);
        Assert.False(fft.IsRadix2);
        fft.Forward(re, im);

        double scale = 0;
        for (int i = 0; i < size; i++) scale = Math.Max(scale, Math.Abs(refRe[i]) + Math.Abs(refIm[i]));
        for (int i = 0; i < size; i++)
        {
            Assert.True(Math.Abs(re[i] - refRe[i]) < 1e-4 * scale, $"re[{i}] {re[i]} vs {refRe[i]}");
            Assert.True(Math.Abs(im[i] - refIm[i]) < 1e-4 * scale, $"im[{i}] {im[i]} vs {refIm[i]}");
        }
    }

    /// <summary>The size that made W4-S1 critical: MDX-Net's own n_fft.</summary>
    [Fact]
    public void The_mdx_net_size_6144_transforms_a_pure_tone_into_one_bin()
    {
        const int size = 6144;
        const int bin = 384;
        var samples = new float[size];
        for (int i = 0; i < size; i++) samples[i] = MathF.Sin(2 * MathF.PI * bin * i / size);

        var fft = new JuceFft(size);
        Assert.False(fft.IsRadix2);
        var mag = fft.Magnitude(samples);

        int peak = 0;
        for (int i = 1; i < mag.Length; i++) if (mag[i] > mag[peak]) peak = i;
        Assert.Equal(bin, peak);
        // Everything away from the tone is numerical noise, not a second lobe.
        for (int i = 0; i < mag.Length; i++)
            if (Math.Abs(i - bin) > 2) Assert.True(mag[i] < mag[peak] * 1e-3f, $"bin {i} = {mag[i]}");
    }

    [Theory]
    [InlineData(6144)]
    [InlineData(1000)]
    [InlineData(12)]
    public void Inverse_undoes_forward_at_a_non_power_of_two_size(int size)
    {
        var (re, im) = Noise(size, seed: 99);
        var origRe = (float[])re.Clone();
        var origIm = (float[])im.Clone();

        var fft = new JuceFft(size);
        fft.Forward(re, im);
        fft.Inverse(re, im);

        for (int i = 0; i < size; i++)
        {
            Assert.True(Math.Abs(re[i] - origRe[i]) < 1e-4, $"re[{i}] {re[i]} vs {origRe[i]}");
            Assert.True(Math.Abs(im[i] - origIm[i]) < 1e-4, $"im[{i}] {im[i]} vs {origIm[i]}");
        }
    }

    /// <summary>
    /// The scratch pool is rented per call, so the documented "safe from several
    /// threads with per-thread buffers" contract has to survive the new path —
    /// <c>Stft.Forward</c> drives it from a <c>Parallel.For</c>.
    /// </summary>
    [Fact]
    public void Bluestein_is_safe_from_many_threads_at_once()
    {
        const int size = 1536;
        var fft = new JuceFft(size);
        var (seedRe, seedIm) = Noise(size, seed: 5);
        var expectRe = (float[])seedRe.Clone();
        var expectIm = (float[])seedIm.Clone();
        fft.Forward(expectRe, expectIm);

        Parallel.For(0, 64, _ =>
        {
            var re = (float[])seedRe.Clone();
            var im = (float[])seedIm.Clone();
            fft.Forward(re, im);
            for (int i = 0; i < size; i++)
            {
                Assert.Equal(expectRe[i], re[i]);
                Assert.Equal(expectIm[i], im[i]);
            }
        });
    }

    [Fact]
    public void Sizes_below_two_are_still_rejected()
    {
        Assert.Throws<ArgumentException>(() => new JuceFft(0));
        Assert.Throws<ArgumentException>(() => new JuceFft(1));
        Assert.Throws<ArgumentException>(() => new JuceFft(-4));
    }
}
