namespace JuceSharp.Dsp;

/// <summary>
/// Magnitude spectrogram with optional Hann windowing, log-magnitude (dB)
/// scaling, and overlap. Stored as <c>[frame][bin]</c>; <c>bin</c> goes
/// up to <c>FftSize / 2</c> (Nyquist excluded).
/// </summary>
public sealed record JuceSpectrogram(float[][] Frames, int FftSize, int HopSize, int SampleRate, bool DecibelScale)
{
    /// <summary>How many analysis frames the spectrogram holds — the first index into
    /// <c>Frames</c>.</summary>
    public int FrameCount => Frames.Length;

    /// <summary>Bins per frame: <c>FftSize / 2</c>, Nyquist excluded.</summary>
    public int BinCount   => FftSize / 2;

    /// <summary>Start time in seconds of a frame, measured from the beginning of the
    /// analysed signal.</summary>
    /// <param name="frameIndex">Frame index, 0 to <see cref="FrameCount"/> − 1.</param>
    public double FrameTimeSeconds(int frameIndex) => (frameIndex * (double)HopSize) / SampleRate;

    /// <summary>Centre frequency in Hz of a bin.</summary>
    /// <param name="bin">Bin index, 0 to <see cref="BinCount"/> − 1.</param>
    public double BinHz(int bin) => bin * SampleRate / (double)FftSize;
}

/// <summary>
/// Builds a magnitude (or dB) spectrogram from a mono signal using the
/// existing <see cref="JuceFft"/>.
/// <para>The FFT planes and the analysis window are per-instance scratch,
/// reused across frames and across calls. The per-frame magnitude row is NOT
/// scratch — it is the returned data, one array per frame of the result — so
/// "allocation-free per frame", which this doc used to claim outright, was only
/// ever true of the scratch.</para>
/// </summary>
/// <remarks>
/// <para><b>Thread-safety:</b> instances are <b>not</b> thread-safe. The
/// builder holds reusable scratch buffers (<c>_re</c>, <c>_im</c>) and a
/// <see cref="JuceFft"/> that is also single-threaded. Concurrent calls
/// to <see cref="Build"/> on the same instance will corrupt those buffers
/// and produce undefined output. Create one builder per thread, or guard
/// access with a lock.</para>
/// </remarks>
public sealed class JuceSpectrogramBuilder
{
    private readonly JuceFft _fft;
    private readonly float[] _window;
    private readonly float[] _re;
    private readonly float[] _im;

    /// <summary>FFT length in samples. A power of two is the FASTEST case, not a
    /// requirement of this type — the constraint is whatever <see cref="JuceFft"/>
    /// accepts, and it is that transform, not this builder, that decides.</summary>
    public int FftSize { get; }
    /// <summary>Hop in samples (typically <c>FftSize / 4</c>).</summary>
    public int HopSize { get; }

    /// <summary>
    /// Lifetime count of non-finite (NaN / ±Inf) input samples replaced with silence at
    /// <see cref="Build"/>, across every call on this instance.
    /// <para>Counted once per WINDOW a bad sample appears in — windows overlap — so it is a
    /// "how bad" signal rather than an exact sample census, the same convention
    /// <c>JuceBeatDetector.NonFiniteSamples</c> uses.</para>
    /// </summary>
    public long NonFiniteSamples { get; private set; }

    /// <summary>Allocate the FFT, the analysis window and the reusable scratch planes for
    /// one builder. One builder per thread — the scratch is shared across every
    /// <c>Build</c> call on the instance.</summary>
    /// <param name="fftSize">Transform length in samples. A power of two is the fastest
    /// case; the accepted set is whatever <see cref="JuceFft"/> supports.</param>
    /// <param name="hopSize">Advance between frames in samples; must be in (0, fftSize].</param>
    /// <exception cref="ArgumentException"><paramref name="hopSize"/> is outside (0, fftSize].</exception>
    public JuceSpectrogramBuilder(int fftSize = 2048, int hopSize = 512)
    {
        if (hopSize <= 0 || hopSize > fftSize)
            throw new ArgumentException("Hop must be in (0, fftSize].", nameof(hopSize));
        FftSize = fftSize;
        HopSize = hopSize;
        _fft = new JuceFft(fftSize);
        _window = Windows.Hann(fftSize);
        _re = new float[fftSize];
        _im = new float[fftSize];
    }

    /// <summary>
    /// Compute the spectrogram for <paramref name="mono"/>.
    /// </summary>
    /// <param name="mono">Mono float samples.</param>
    /// <param name="sampleRate">Sample rate of <paramref name="mono"/>.</param>
    /// <param name="decibelScale">If true, convert magnitudes to dBFS (20 log10, floor at -120 dB).</param>
    /// <remarks>Non-finite input samples are replaced with silence and counted in
    /// <see cref="NonFiniteSamples"/> — the same rule every other entry point in this
    /// assembly follows. Unscrubbed, a NaN survived the documented −120 dB floor outright:
    /// <c>MathF.Max(1e-12f, NaN)</c> is NaN and so is <c>MathF.Max(-120f, NaN)</c>, so it
    /// left through the one line whose entire purpose is that floor.</remarks>
    public JuceSpectrogram Build(ReadOnlySpan<float> mono, int sampleRate, bool decibelScale = false)
    {
        if (mono.Length < FftSize) return new JuceSpectrogram([], FftSize, HopSize, sampleRate, decibelScale);
        var frameCount = 1 + (mono.Length - FftSize) / HopSize;
        var frames = new float[frameCount][];
        var bins = FftSize / 2;
        var floor = decibelScale ? -120f : 0f;
        for (var f = 0; f < frameCount; f++)
        {
            var start = f * HopSize;
            for (var i = 0; i < FftSize; i++)
            {
                // Scrub at the input (see NonFiniteSamples).
                var s = mono[start + i];
                if (!float.IsFinite(s)) { s = 0f; NonFiniteSamples++; }
                _re[i] = s * _window[i];
                _im[i] = 0f;
            }
            _fft.Forward(_re, _im);
            var mag = new float[bins];
            for (var i = 0; i < bins; i++)
            {
                var m = MathF.Sqrt(_re[i] * _re[i] + _im[i] * _im[i]) / (FftSize * 0.5f);
                // …and guard the OUTPUT too: a scrubbed input still leaves a dot product
                // that can overflow to +Inf on a pathological signal, which is the same
                // belt-and-braces split JuceConstantQ.Finite applies per kernel.
                mag[i] = !float.IsFinite(m)
                    ? floor
                    : decibelScale ? MathF.Max(-120f, 20f * MathF.Log10(MathF.Max(m, 1e-12f))) : m;
            }
            frames[f] = mag;
        }
        return new JuceSpectrogram(frames, FftSize, HopSize, sampleRate, decibelScale);
    }
}
