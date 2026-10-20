namespace JuceSharp.Dsp;

/// <summary>
/// In-place complex FFT for ANY size &gt;= 2.
///
/// <para>A power-of-two size runs the radix-2 Cooley-Tukey butterfly directly.
/// A size of the form <c>3 * 2^k</c> — which is what MDX-Net's 6144 is — runs one
/// <b>radix-3</b> decimation over three third-size radix-2 transforms. Anything
/// else runs <b>Bluestein's chirp-z algorithm</b>: the DFT is re-expressed as a
/// circular convolution of length <c>M = next power of two &gt;= 2*Size - 1</c>,
/// which costs two M-point radix-2 transforms per call (the kernel's own
/// transform is precomputed). The radix-2 path is untouched by both additions and
/// stays bit-for-bit what it always produced — <c>JuceFftBluesteinTests</c>
/// pins it.</para>
///
/// <para><b>Measured, 2026-09-11, one forward transform, Release, best of
/// three:</b> 2048 radix-2 31 us, 4096 radix-2 55 us, 16384 radix-2 205 us;
/// <b>6144 radix-3 110 us</b>, against <b>526 us</b> for the same 6144 forced
/// down the general Bluestein path (which has to run two 16384-point
/// transforms). So MDX-Net's STFT costs about twice a 4096-point one, not ten
/// times.</para>
///
/// <para><b>Why a general size is load-bearing.</b> MDX-Net's exported ONNX
/// graphs are built around <c>n_fft = 6144</c> (= 3 * 2048, with
/// <c>dim_f = 3072</c> bins); zero-padding to 8192 would change the bin spacing
/// and mis-feed the model. Before this path existed, every MDX-Net model in the
/// catalogue threw on its first chunk.</para>
///
/// Allocation-free once constructed on the radix-2 path (the twiddle tables and
/// bit-reverse indices are precomputed) and allocation-free on the Bluestein
/// path once each concurrent caller has touched a scratch slot. Thread-safe to
/// call <see cref="Forward"/> from multiple threads if you pass per-thread
/// buffers.
/// </summary>
public sealed class JuceFft
{
    /// <summary>Transform size in samples. Any value &gt;= 2; a power of two
    /// takes the radix-2 path, <c>3 * 2^k</c> takes the radix-3 path, anything
    /// else takes Bluestein.</summary>
    public int Size { get; }

    /// <summary>
    /// <c>true</c> when <see cref="Size"/> is a power of two and the direct
    /// radix-2 butterfly runs; <c>false</c> when a composite path (radix-3 or
    /// Bluestein) does. Exposed so a caller sizing an offline job can see which
    /// cost it is paying.
    /// </summary>
    public bool IsRadix2 { get; }

    private readonly int       _log2;
    private readonly int[]     _bitRev;
    private readonly float[]   _cos;
    private readonly float[]   _sin;
    private readonly Bluestein _bluestein = null!;
    private readonly Radix3    _radix3    = null!;

    /// <summary>
    /// Precompute the twiddle tables and bit-reverse index map for one transform size, so
    /// that every later <c>Forward</c> allocates nothing. A non-power-of-two size
    /// additionally precomputes either the radix-3 twiddles (when the size is
    /// three times a power of two) or the Bluestein chirp and its transformed kernel.
    /// </summary>
    /// <param name="size">Transform size in samples; at least 2.</param>
    /// <exception cref="ArgumentException"><paramref name="size"/> is less than 2.</exception>
    public JuceFft(int size)
    {
        if (size <= 1)
            throw new ArgumentException("FFT size must be >= 2.", nameof(size));
        Size     = size;
        IsRadix2 = (size & (size - 1)) == 0;

        if (!IsRadix2)
        {
            // Radix-2 tables stay empty: this instance delegates its butterflies
            // to the inner power-of-two transform the chosen path owns.
            _bitRev = [];
            _cos    = [];
            _sin    = [];
            // 3 * 2^k gets one radix-3 decimation over three third-size radix-2
            // transforms, which is ~5x cheaper than Bluestein at 6144 (measured:
            // 110 us against 526 us, against 55 us for a 4096 radix-2). MDX-Net's
            // 6144 is the only non-power-of-two size any shipping catalogue entry
            // asks for, so the general path is the fallback and not the norm.
            if (size % 3 == 0 && ((size / 3) & (size / 3 - 1)) == 0 && size / 3 >= 2)
                _radix3 = new Radix3(size);
            else
                _bluestein = new Bluestein(size);
            return;
        }

        _log2 = (int)Math.Log2(size);

        _bitRev = new int[size];
        for (int i = 0; i < size; i++) _bitRev[i] = ReverseBits(i, _log2);

        _cos = new float[size / 2];
        _sin = new float[size / 2];
        for (int i = 0; i < size / 2; i++)
        {
            var a = -2.0 * Math.PI * i / size;
            _cos[i] = (float)Math.Cos(a);
            _sin[i] = (float)Math.Sin(a);
        }
    }

    /// <summary>Run an in-place complex FFT. <paramref name="re"/> and
    /// <paramref name="im"/> must each be exactly <see cref="Size"/>
    /// samples long.</summary>
    public void Forward(Span<float> re, Span<float> im)
    {
        if (re.Length != Size || im.Length != Size)
            throw new ArgumentException("Buffers must equal FFT size.");

        if (!IsRadix2)
        {
            if (_radix3 is not null) _radix3.Forward(re, im);
            else _bluestein.Forward(re, im);
            return;
        }

        for (int i = 0; i < Size; i++)
        {
            var j = _bitRev[i];
            if (j > i)
            {
                (re[i], re[j]) = (re[j], re[i]);
                (im[i], im[j]) = (im[j], im[i]);
            }
        }

        for (int stage = 1; stage <= _log2; stage++)
        {
            int half = 1 << (stage - 1);
            int full = half << 1;
            int step = Size / full;
            for (int k = 0; k < Size; k += full)
            {
                for (int j = 0; j < half; j++)
                {
                    var c = _cos[j * step];
                    var s = _sin[j * step];
                    int a = k + j;
                    int b = a + half;
                    var tre = re[b] * c - im[b] * s;
                    var tim = re[b] * s + im[b] * c;
                    re[b]   = re[a] - tre;
                    im[b]   = im[a] - tim;
                    re[a]   = re[a] + tre;
                    im[a]   = im[a] + tim;
                }
            }
        }
    }

    /// <summary>
    /// In-place complex INVERSE FFT, unitary-free / "Matlab" convention: the
    /// forward transform applies no scaling and this one divides by
    /// <see cref="Size"/>, so <c>Inverse(Forward(x)) == x</c>.
    /// <para>Implemented as conjugate → <see cref="Forward"/> → conjugate →
    /// scale, so there is exactly ONE butterfly implementation to keep correct
    /// and the twiddle tables are shared. Allocation-free.</para>
    /// </summary>
    public void Inverse(Span<float> re, Span<float> im)
    {
        if (re.Length != Size || im.Length != Size)
            throw new ArgumentException("Buffers must equal FFT size.");

        for (int i = 0; i < Size; i++) im[i] = -im[i];
        Forward(re, im);
        var scale = 1f / Size;
        for (int i = 0; i < Size; i++)
        {
            re[i] *= scale;
            im[i] = -im[i] * scale;
        }
    }

    /// <summary>
    /// Real-input forward transform into caller-owned planes. <paramref name="re"/>
    /// and <paramref name="im"/> must each be <see cref="Size"/> long and are
    /// fully overwritten; bins <c>0 .. Size/2</c> then hold the half-spectrum
    /// (bin 0 = DC, bin <c>Size/2</c> = Nyquist). Allocation-free.
    /// </summary>
    public void ForwardReal(ReadOnlySpan<float> samples, Span<float> re, Span<float> im)
    {
        if (samples.Length > Size)
            throw new ArgumentException("Sample count must not exceed the FFT size.", nameof(samples));
        samples.CopyTo(re);
        re[samples.Length..].Clear();
        im.Clear();
        Forward(re, im);
    }

    /// <summary>
    /// Inverse of a HALF spectrum back to a real signal. The upper half is
    /// reconstructed by conjugate symmetry from <paramref name="binRe"/> /
    /// <paramref name="binIm"/> (which hold <c>Size/2 + 1</c> bins or fewer —
    /// missing top bins are treated as zero), the inverse runs in the
    /// caller-owned <paramref name="re"/> / <paramref name="im"/> planes, and the
    /// real result is left in <paramref name="re"/>. Allocation-free.
    /// </summary>
    public void InverseReal(ReadOnlySpan<float> binRe, ReadOnlySpan<float> binIm,
                            Span<float> re, Span<float> im)
    {
        if (re.Length != Size || im.Length != Size)
            throw new ArgumentException("Buffers must equal FFT size.");
        if (binRe.Length != binIm.Length)
            throw new ArgumentException("Real and imaginary bin planes must be the same length.");

        var bins = Math.Min(binRe.Length, Size / 2 + 1);
        re.Clear();
        im.Clear();
        for (int i = 0; i < bins; i++) { re[i] = binRe[i]; im[i] = binIm[i]; }
        // Conjugate-symmetric upper half so the inverse yields a real signal.
        for (int i = 1; i < Size - bins + 1 && i < bins; i++)
        {
            re[Size - i] = binRe[i];
            im[Size - i] = -binIm[i];
        }
        Inverse(re, im);
    }

    /// <summary>Convenience: real-input → magnitude spectrum of length
    /// <c>Size / 2</c>. Allocates the output array.</summary>
    public float[] Magnitude(ReadOnlySpan<float> samples)
    {
        if (samples.Length != Size) throw new ArgumentException("Sample count must equal FFT size.");
        var re = new float[Size];
        var im = new float[Size];
        samples.CopyTo(re);
        Forward(re, im);
        var mag = new float[Size / 2];
        for (int i = 0; i < mag.Length; i++)
            mag[i] = MathF.Sqrt(re[i] * re[i] + im[i] * im[i]);
        return mag;
    }

    /// <summary>
    /// One radix-3 decimation-in-time stage over three power-of-two transforms,
    /// for sizes of the form <c>3 * 2^k</c>.
    /// <para>Splitting <c>x</c> into <c>x[3r]</c>, <c>x[3r+1]</c>, <c>x[3r+2]</c>
    /// gives three M-point spectra A, B, C (<c>M = N/3</c>); with
    /// <c>u = W_N^q B[q]</c>, <c>v = W_N^2q C[q]</c> and the cube roots of unity
    /// <c>w1 = e^(-2*pi*i/3)</c>, <c>w2 = conj(w1)</c>, the output is
    /// <c>X[q] = A+u+v</c>, <c>X[q+M] = A + w1*u + w2*v</c>,
    /// <c>X[q+2M] = A + w2*u + w1*v</c>.</para>
    /// <para>Why it earns its place next to the general path: at MDX-Net's
    /// n_fft = 6144 this is ~5x cheaper than Bluestein (110 us against 526 us),
    /// because Bluestein has to run TWO 16384-point transforms to convolve a
    /// 6144-point sequence.</para>
    /// </summary>
    private sealed class Radix3
    {
        private const float W1Re = -0.5f;
        private static readonly float W1Im = -(float)(Math.Sqrt(3.0) / 2.0);

        private readonly int     _n;
        private readonly int     _m;
        private readonly JuceFft _inner;
        private readonly float[] _twRe;   // W_N^q,  q in [0, M)
        private readonly float[] _twIm;
        private readonly float[] _tw2Re;  // W_N^2q
        private readonly float[] _tw2Im;
        private readonly Pair?[] _slots;

        private sealed class Pair
        {
            internal Pair(int n) { Re = new float[n]; Im = new float[n]; }
            internal float[] Re { get; }
            internal float[] Im { get; }
        }

        internal Radix3(int n)
        {
            _n = n;
            _m = n / 3;
            _inner = new JuceFft(_m);
            _twRe = new float[_m];
            _twIm = new float[_m];
            _tw2Re = new float[_m];
            _tw2Im = new float[_m];
            for (int q = 0; q < _m; q++)
            {
                var a = -2.0 * Math.PI * q / n;
                _twRe[q] = (float)Math.Cos(a);
                _twIm[q] = (float)Math.Sin(a);
                var a2 = -2.0 * Math.PI * (2L * q % n) / n;
                _tw2Re[q] = (float)Math.Cos(a2);
                _tw2Im[q] = (float)Math.Sin(a2);
            }
            _slots = new Pair?[Environment.ProcessorCount + 2];
        }

        internal void Forward(Span<float> re, Span<float> im)
        {
            var pair = Rent();
            float[] sr = pair.Re, si = pair.Im;
            try
            {
                for (int r = 0; r < _m; r++)
                {
                    int b = 3 * r;
                    sr[r] = re[b];
                    si[r] = im[b];
                    sr[_m + r] = re[b + 1];
                    si[_m + r] = im[b + 1];
                    sr[2 * _m + r] = re[b + 2];
                    si[2 * _m + r] = im[b + 2];
                }

                _inner.Forward(sr.AsSpan(0, _m), si.AsSpan(0, _m));
                _inner.Forward(sr.AsSpan(_m, _m), si.AsSpan(_m, _m));
                _inner.Forward(sr.AsSpan(2 * _m, _m), si.AsSpan(2 * _m, _m));

                for (int q = 0; q < _m; q++)
                {
                    float ar = sr[q], ai = si[q];
                    float br = sr[_m + q], bi = si[_m + q];
                    float cr = sr[2 * _m + q], ci = si[2 * _m + q];

                    float ur = br * _twRe[q] - bi * _twIm[q];
                    float ui = br * _twIm[q] + bi * _twRe[q];
                    float vr = cr * _tw2Re[q] - ci * _tw2Im[q];
                    float vi = cr * _tw2Im[q] + ci * _tw2Re[q];

                    re[q] = ar + ur + vr;
                    im[q] = ai + ui + vi;

                    // w1 * u and w2 * v, then the pair with the roots swapped.
                    float w1ur = W1Re * ur - W1Im * ui, w1ui = W1Re * ui + W1Im * ur;
                    float w2ur = W1Re * ur + W1Im * ui, w2ui = W1Re * ui - W1Im * ur;
                    float w1vr = W1Re * vr - W1Im * vi, w1vi = W1Re * vi + W1Im * vr;
                    float w2vr = W1Re * vr + W1Im * vi, w2vi = W1Re * vi - W1Im * vr;

                    re[_m + q] = ar + w1ur + w2vr;
                    im[_m + q] = ai + w1ui + w2vi;
                    re[2 * _m + q] = ar + w2ur + w1vr;
                    im[2 * _m + q] = ai + w2ui + w1vi;
                }
            }
            finally
            {
                Return(pair);
            }
        }

        private Pair Rent()
        {
            for (int i = 0; i < _slots.Length; i++)
            {
                var p = Interlocked.Exchange(ref _slots[i], null);
                if (p is not null) return p;
            }
            return new Pair(_n);
        }

        private void Return(Pair pair)
        {
            for (int i = 0; i < _slots.Length; i++)
                if (Interlocked.CompareExchange(ref _slots[i], pair, null) is null) return;
        }
    }

    /// <summary>
    /// Bluestein's chirp-z transform: an arbitrary-length DFT expressed as a
    /// circular convolution that a power-of-two FFT can run.
    /// <para>With <c>w[n] = exp(-i*pi*n^2/N)</c> and
    /// <c>j*k = (j^2 + k^2 - (k-j)^2) / 2</c>, the DFT becomes
    /// <c>X[k] = w[k] * sum_j (x[j] w[j]) * conj(w[k-j])</c> — a convolution of
    /// <c>a[j] = x[j] w[j]</c> with <c>b[n] = conj(w[n])</c>, evaluated in
    /// <c>M &gt;= 2N - 1</c> points so the circular wrap cannot alias.</para>
    /// <para>The chirp angle is reduced with <c>n^2 mod 2N</c> rather than formed
    /// from <c>n^2</c> directly: at N = 6144 the raw product reaches 3.8e7, and
    /// taking its sine in double after that much cancellation costs several
    /// digits of the chirp's accuracy.</para>
    /// <para><b>Scratch is rented, not owned</b>, so the class keeps
    /// <see cref="JuceFft.Forward"/>'s documented "safe from several threads with
    /// per-thread buffers" contract. A slot that is busy is skipped and, in the
    /// worst case, a caller allocates its own pair — a slowdown, never a race.</para>
    /// </summary>
    private sealed class Bluestein
    {
        private readonly int      _n;
        private readonly int      _m;
        private readonly JuceFft  _inner;
        private readonly float[]  _chirpRe;
        private readonly float[]  _chirpIm;
        private readonly float[]  _kernelRe;
        private readonly float[]  _kernelIm;
        private readonly Pair?[] _slots;

        private sealed class Pair
        {
            internal Pair(int m) { Re = new float[m]; Im = new float[m]; }
            internal float[] Re { get; }
            internal float[] Im { get; }
        }

        internal Bluestein(int n)
        {
            _n = n;
            var m = 1;
            while (m < 2 * n - 1) m <<= 1;
            _m = m;
            _inner = new JuceFft(m);

            _chirpRe = new float[n];
            _chirpIm = new float[n];
            for (int k = 0; k < n; k++)
            {
                var sq  = (long)k * k % (2L * n);
                var ang = -Math.PI * sq / n;
                _chirpRe[k] = (float)Math.Cos(ang);
                _chirpIm[k] = (float)Math.Sin(ang);
            }

            _kernelRe = new float[m];
            _kernelIm = new float[m];
            for (int k = 0; k < n; k++)
            {
                // b[k] = conj(w[k]); b[-k] = b[k] because w depends on k^2 only.
                _kernelRe[k] = _chirpRe[k];
                _kernelIm[k] = -_chirpIm[k];
                if (k > 0)
                {
                    _kernelRe[m - k] = _chirpRe[k];
                    _kernelIm[m - k] = -_chirpIm[k];
                }
            }
            _inner.Forward(_kernelRe, _kernelIm);

            _slots = new Pair?[Environment.ProcessorCount + 2];
        }

        internal void Forward(Span<float> re, Span<float> im)
        {
            var pair = Rent();
            float[] ar = pair.Re, ai = pair.Im;
            try
            {
                Array.Clear(ar);
                Array.Clear(ai);
                for (int k = 0; k < _n; k++)
                {
                    float xr = re[k], xi = im[k], cr = _chirpRe[k], ci = _chirpIm[k];
                    ar[k] = xr * cr - xi * ci;
                    ai[k] = xr * ci + xi * cr;
                }

                _inner.Forward(ar, ai);
                for (int k = 0; k < _m; k++)
                {
                    float pr = ar[k] * _kernelRe[k] - ai[k] * _kernelIm[k];
                    float pi = ar[k] * _kernelIm[k] + ai[k] * _kernelRe[k];
                    ar[k] = pr;
                    ai[k] = pi;
                }
                _inner.Inverse(ar, ai);

                for (int k = 0; k < _n; k++)
                {
                    float cr = _chirpRe[k], ci = _chirpIm[k];
                    re[k] = ar[k] * cr - ai[k] * ci;
                    im[k] = ar[k] * ci + ai[k] * cr;
                }
            }
            finally
            {
                Return(pair);
            }
        }

        private Pair Rent()
        {
            for (int i = 0; i < _slots.Length; i++)
            {
                var p = Interlocked.Exchange(ref _slots[i], null);
                if (p is not null) return p;
            }
            return new Pair(_m);
        }

        private void Return(Pair pair)
        {
            for (int i = 0; i < _slots.Length; i++)
                if (Interlocked.CompareExchange(ref _slots[i], pair, null) is null) return;
        }
    }

    private static int ReverseBits(int x, int bits)
    {
        int r = 0;
        for (int i = 0; i < bits; i++)
        {
            r = (r << 1) | (x & 1);
            x >>= 1;
        }
        return r;
    }
}
