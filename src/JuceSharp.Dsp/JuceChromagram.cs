namespace JuceSharp.Dsp;

/// <summary>
/// 12-bin chromagram extractor. Folds an FFT magnitude spectrum into
/// the twelve pitch classes (C, C#, D, …, B) and normalises the result
/// so the sum is 1.
///
/// Useful as input to <see cref="JuceChordDetector"/> or any
/// key/scale analysis.
///
/// <para><b>Not thread-safe per instance.</b> The FFT planes and the chroma
/// accumulator are per-instance scratch reused across calls; construct one
/// per thread.</para>
/// </summary>
public sealed class JuceChromagram
{
    /// <summary>Sample rate in Hz of the audio this instance folds; fixed at
    /// construction and used to map FFT bins to pitch classes.</summary>
    public int   SampleRate { get; }

    /// <summary>Transform size in samples, a power of two. Sets the bin spacing and
    /// therefore how cleanly low notes separate into distinct pitch classes.</summary>
    public int   FftSize    { get; }

    /// <summary>Lowest frequency folded into the chroma vector (default C2).</summary>
    public float MinFreq    { get; init; } = 65f;   // C2

    /// <summary>Highest frequency folded into the chroma vector (default C7).</summary>
    public float MaxFreq    { get; init; } = 2093f; // C7

    /// <summary>
    /// Multiply each frame by a symmetric Hann window before the FFT (default
    /// <c>true</c>). Without it an arbitrary frame boundary sprays a strong
    /// partial across every bin, which reads as energy in pitch classes that
    /// are not being played. Set <c>false</c> only to reproduce the
    /// pre-windowing numbers exactly.
    /// <para><b>Turning this on MOVED a shipped output and the move was
    /// deliberate.</b> <see cref="JuceGuitarAnalyzer"/> builds two of these with
    /// no override, and they feed the <c>Chroma</c> field of every emitted
    /// <c>JuceGuitarEvent</c> — so every analyzer event's chroma vector changed
    /// the day this default became <c>true</c>, with no baseline update and no
    /// test. It shipped that way for a release because the only thing near it
    /// that looked like a claim was a comment about the unrelated Hann
    /// REFACTOR ("moves no analyzer number"), which is true of the refactor and
    /// says nothing about this. Both branches are now pinned by
    /// <c>Wave2ChromagramBaselineTests</c>; if you flip this default again,
    /// re-baseline there in the same change.</para>
    /// </summary>
    public bool  ApplyHannWindow { get; init; } = true;

    // The bin→pitch-class table depends on MinFreq/MaxFreq, which are `init`
    // properties — object-initializer setters run AFTER the constructor, so a
    // table built in the ctor is built from the DEFAULTS and
    // `new JuceChromagram(sr) { MinFreq = 100f }` silently kept the 65 Hz
    // mapping. Build it on first use instead, when the initializer has run.
    private int[]? _binToChroma;

    private readonly JuceFft _fft;
    private readonly float[] _re;
    private readonly float[] _im;
    private readonly float[] _chroma = new float[12];
    private readonly float[]? _hann;

    /// <summary>
    /// Allocate the FFT, the scratch planes and the Hann window for one analysis thread.
    /// The bin-to-pitch-class map is built lazily on first use so the init properties
    /// have run by then.
    /// </summary>
    /// <param name="sampleRate">Sample rate in Hz of the audio to be folded.</param>
    /// <param name="fftSize">Transform size in samples; must be a power of two.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="sampleRate"/> is not positive.</exception>
    /// <exception cref="ArgumentException"><paramref name="fftSize"/> is not a power of two.</exception>
    public JuceChromagram(int sampleRate, int fftSize = 4096)
    {
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        if (fftSize <= 0 || (fftSize & (fftSize - 1)) != 0)
            throw new ArgumentException("FFT size must be power of two.", nameof(fftSize));
        SampleRate = sampleRate;
        FftSize    = fftSize;
        _fft       = new JuceFft(fftSize);
        _re        = new float[fftSize];
        _im        = new float[fftSize];
        // Resolved once rather than looked up per frame. The REASON this comment
        // used to give was wrong — `Windows.Hann` is a ConcurrentDictionary lookup
        // over a value-tuple key and allocates NOTHING — so do not repeat it. The
        // hoist is still right for the ordinary reason: a dictionary probe, a hash
        // and a bounds check per frame on the analyzer's hot path buy nothing when
        // the size cannot change after construction.
        _hann      = Windows.Hann(fftSize);
    }

    private int[] BinToChroma()
    {
        if (_binToChroma is { } cached) return cached;
        if (!(MinFreq > 0f) || !(MaxFreq > MinFreq))
            throw new InvalidOperationException(
                $"0 < {nameof(MinFreq)} < {nameof(MaxFreq)} required (was {MinFreq} .. {MaxFreq}).");

        var table = new int[FftSize / 2];
        for (var k = 1; k < table.Length; k++)
        {
            var f = (float)k * SampleRate / FftSize;
            if (f < MinFreq || f > MaxFreq) { table[k] = -1; continue; }
            table[k] = PitchMath.PitchClass(PitchMath.HzToMidiF(f));
        }
        return _binToChroma = table;
    }

    /// <summary>Compute the 12-bin chroma vector for the given mono
    /// frame (which must equal <see cref="FftSize"/> samples). Allocates the
    /// returned array; use <see cref="Compute(ReadOnlySpan{float}, Span{float})"/>
    /// on a hot path.</summary>
    public float[] Compute(ReadOnlySpan<float> frame)
    {
        var result = new float[12];
        Compute(frame, result);
        return result;
    }

    /// <summary>
    /// Allocation-free overload: writes the 12 chroma values into
    /// <paramref name="chroma"/>.
    /// </summary>
    public void Compute(ReadOnlySpan<float> frame, Span<float> chroma)
    {
        if (frame.Length != FftSize)
            throw new ArgumentException("Frame must equal FFT size.", nameof(frame));
        if (chroma.Length < 12)
            throw new ArgumentException("Chroma output needs 12 elements.", nameof(chroma));

        var table = BinToChroma();
        var re = _re;
        var im = _im;
        if (ApplyHannWindow)
        {
            var w = _hann!;
            for (var i = 0; i < FftSize; i++) { re[i] = frame[i] * w[i]; im[i] = 0f; }
        }
        else
        {
            for (var i = 0; i < FftSize; i++) { re[i] = frame[i]; im[i] = 0f; }
        }
        _fft.Forward(re, im);

        var acc = _chroma;
        Array.Clear(acc);
        for (var k = 1; k < FftSize / 2; k++)
        {
            var c = table[k];
            if (c < 0) continue;
            acc[c] += MathF.Sqrt(re[k] * re[k] + im[k] * im[k]);
        }
        float sum = 0;
        for (var i = 0; i < 12; i++) sum += acc[i];
        if (sum > 0) for (var i = 0; i < 12; i++) acc[i] /= sum;
        acc.AsSpan().CopyTo(chroma);
    }
}
