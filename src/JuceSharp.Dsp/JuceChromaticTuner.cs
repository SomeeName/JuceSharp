namespace JuceSharp.Dsp;

/// <summary>Result of a single tuner detection.</summary>
/// <param name="FrequencyHz">Detected fundamental in Hz.</param>
/// <param name="NoteName">Sharp note name of the nearest semitone (C, C#, …).</param>
/// <param name="Octave">Octave of the nearest semitone in scientific pitch notation (MIDI 60 = C4).</param>
/// <param name="CentsOff">Signed cents from the nearest semitone.</param>
/// <param name="Confidence">1 - the YIN difference value at the chosen lag; higher is more periodic.</param>
/// <param name="Clamped">
/// <b>The reading sits on the tuner's lag ceiling and is therefore a FLOOR
/// ARTEFACT, not a measurement.</b> Below its lowest resolvable frequency YIN
/// does not fail — it returns the clamped lag as a confident reading. Measured
/// on a synthetic bass low E (MIDI 28, 41.20 Hz) at 44.1 kHz, a 2048-sample
/// window reports <b>43.05 Hz at 0.93 confidence</b>: nearly a semitone sharp
/// and indistinguishable from a real F1. The reading is still returned — the
/// analyzer's long-window fallback keys off exactly this condition — but any
/// caller that would display or trust it should check this flag against
/// <see cref="JuceChromaticTuner.EffectiveMinFrequency"/> first.
/// </param>
public readonly record struct JuceTunerReading(
    float  FrequencyHz,
    string NoteName,
    int    Octave,
    float  CentsOff,
    float  Confidence,
    bool   Clamped = false);

/// <summary>
/// Monophonic chromatic tuner based on the YIN pitch-detection
/// algorithm (de Cheveigné &amp; Kawahara, 2002). Designed for guitar /
/// bass: detects 50 Hz – 1.5 kHz with cent-level accuracy <b>at the default
/// <c>minFreq</c></b>.
///
/// <para><b>The floor is TWO limits, and the window is usually the binding one.</b>
/// <see cref="Detect"/> rejects anything below <see cref="MinFrequency"/> outright, and it
/// searches lags only up to <c>WindowSize / 2</c> — so a 2048-sample window cannot see below
/// <c>sampleRate / 1024</c> (43.1 Hz at 44.1 kHz) no matter what <c>minFreq</c> says.
/// Lowering <c>minFreq</c> without also widening the window buys nothing beneath that. A bass
/// low E (MIDI 28, 41.2 Hz) needs τ ≈ 1070 and therefore a window of 4096.
/// <c>JuceGuitarAnalyzer</c> hit exactly this and now names both explicitly
/// (<c>YinMinFreqHz</c>, <c>LegatoYinWindow</c>) instead of inheriting the defaults.</para>
///
/// Pure managed, allocation-free per-call once constructed. Feed it
/// raw mono float samples from any source (live input ring, recorded
/// buffer, file).
/// </summary>
public sealed class JuceChromaticTuner
{
    /// <summary>Sample rate in Hz of the mono audio fed to <c>Detect</c>; together with
    /// <see cref="WindowSize"/> it fixes the lowest resolvable frequency.</summary>
    public int   SampleRate    { get; }

    /// <summary>Analysis window in samples. YIN searches lags only up to half of it, so
    /// this — not <see cref="MinFrequency"/> — is usually what sets the real floor; see
    /// <see cref="EffectiveMinFrequency"/>.</summary>
    public int   WindowSize    { get; }

    /// <summary>Requested lowest frequency in Hz; readings below it are rejected outright.
    /// Lowering it buys nothing unless <see cref="WindowSize"/> widens to match.</summary>
    public float MinFrequency  { get; }

    /// <summary>Highest frequency in Hz the search considers, which sets the shortest lag
    /// examined.</summary>
    public float MaxFrequency  { get; }

    /// <summary>YIN's cumulative-mean-normalised difference threshold: the first lag whose
    /// value falls below this is taken as the period. Lower is stricter about
    /// periodicity. Defaults to 0.15, the value in the original paper.</summary>
    public float Threshold     { get; init; } = 0.15f;

    /// <summary>
    /// The lowest frequency this instance can actually resolve:
    /// <c>SampleRate / maxTau</c>, where <c>maxTau = min(WindowSize/2,
    /// SampleRate/MinFrequency)</c>. <b>This is usually higher than
    /// <see cref="MinFrequency"/></b> — the window, not the requested floor, is
    /// normally the binding limit (a 2048-sample window at 44.1 kHz cannot see
    /// below 43.07 Hz whatever <c>minFreq</c> says, so a bass low E needs 4096).
    /// A reading at or below this value carries
    /// <see cref="JuceTunerReading.Clamped"/>.
    /// </summary>
    public float EffectiveMinFrequency { get; }

    /// <summary>
    /// Compute the YIN difference function through an FFT autocorrelation
    /// instead of the direct double loop. <b>Opt-in, and NOT bit-identical.</b>
    /// <para>The direct difference is O(W·τmax) — 2M multiply-adds per 256-sample
    /// legato hop in <see cref="JuceGuitarAnalyzer"/>, which is the dominant cost
    /// of the whole offline audio-to-chart pass. The identity
    /// <c>d(τ) = r(0..W-τ) + r(τ..W) - 2·r_x(τ)</c> turns it into one real FFT,
    /// one power spectrum and one inverse, with the two energy terms coming from
    /// a cumulative-energy prefix: O(N log N) with N = the next power of two
    /// above 2W.</para>
    /// <para>It is off by default because the result differs in the last few
    /// float digits, and <see cref="JuceGuitarAnalyzer"/>'s detection output is a
    /// bit-identity contract. Measured over 200 random guitar-range sines plus the
    /// synthetic corpus the existing tests use, the two agree to well under half a
    /// cent and 0.01 of confidence — see <c>FftYinEquivalenceTests</c>, which also
    /// reports the speed-up.</para>
    /// </summary>
    public bool UseFftDifference { get; }

    private readonly int _maxTau;

    // Reused YIN difference buffer (maxTau never exceeds WindowSize / 2) —
    // Detect runs every 256 samples in the analyzer's legato tracker, so a
    // per-call allocation here is a steady garbage stream on the hot path.
    private readonly float[] _diff;

    // FFT path only; null when UseFftDifference is false, so the default path
    // pays neither the memory nor the constructor time.
    private readonly JuceFft? _fft;
    private readonly float[]? _fftRe;
    private readonly float[]? _fftIm;
    private readonly float[]? _energyPrefix;

    /// <param name="sampleRate">Sample rate of the audio handed to <see cref="Detect"/>.</param>
    /// <param name="windowSize">Analysis window; also the lag ceiling, see
    /// <see cref="EffectiveMinFrequency"/>.</param>
    /// <param name="minFreq">Lowest frequency to report.</param>
    /// <param name="maxFreq">Highest frequency to report.</param>
    /// <param name="useFftDifference">See <see cref="UseFftDifference"/>. Opt-in,
    /// and a constructor parameter rather than an <c>init</c> property because it
    /// decides what the constructor allocates — an init property would have to
    /// allocate lazily on the first call, which is exactly the per-call garbage
    /// this type exists to avoid.</param>
    public JuceChromaticTuner(int sampleRate, int windowSize = 2048,
                               float minFreq = 50f, float maxFreq = 1500f,
                               bool useFftDifference = false)
    {
        if (sampleRate <= 0)  throw new ArgumentOutOfRangeException(nameof(sampleRate));
        if (windowSize < 256) throw new ArgumentOutOfRangeException(nameof(windowSize));
        if (minFreq <= 0 || maxFreq <= minFreq)
            throw new ArgumentException("0 < minFreq < maxFreq required.");
        SampleRate   = sampleRate;
        WindowSize   = windowSize;
        MinFrequency = minFreq;
        MaxFrequency = maxFreq;
        _diff        = new float[windowSize / 2 + 1];
        _maxTau      = Math.Min(windowSize / 2, (int)(sampleRate / minFreq));
        EffectiveMinFrequency = _maxTau > 0 ? (float)sampleRate / _maxTau : float.PositiveInfinity;

        UseFftDifference = useFftDifference;
        if (useFftDifference)
        {
            // >= 2W so the circular autocorrelation the FFT computes IS the
            // linear one over the lags we read. Anything less wraps the tail of
            // the window onto the head and the dip lands at the wrong lag.
            var n = 1;
            while (n < 2 * windowSize) n <<= 1;
            _fft          = new JuceFft(n);
            _fftRe        = new float[n];
            _fftIm        = new float[n];
            _energyPrefix = new float[windowSize + 1];
        }
    }

    /// <summary>Detect the fundamental pitch of <paramref name="samples"/>.
    /// Returns <c>null</c> if the signal is too quiet or aperiodic.</summary>
    public JuceTunerReading? Detect(ReadOnlySpan<float> samples)
    {
        if (samples.Length < WindowSize) return null;

        float energy = 0;
        for (int i = 0; i < WindowSize; i++) energy += samples[i] * samples[i];
        // BOTH halves of this test are needed and the first one is not decoration:
        // `NaN < 1e-6f` is FALSE, so an unscrubbed non-finite window sails past an
        // energy gate written the obvious way, and `freq < MinFrequency ||
        // freq > MaxFrequency` below is false for a NaN too. The result was a
        // returned reading of NoteName "C", Octave -1, CentsOff NaN — a confident
        // answer made of nothing. Detect is public and documented as taking "raw
        // mono float samples from any source", so it cannot rely on the three
        // streaming wrappers' boundary scrubs. Neither test can change a
        // finite-input result.
        if (!float.IsFinite(energy) || energy / WindowSize < 1e-6f) return null;

        int minTau = Math.Max(2, (int)(SampleRate / MaxFrequency));
        int maxTau = _maxTau;
        if (maxTau <= minTau) return null;

        var diff = _diff;
        if (UseFftDifference) FillDifferenceFft(samples, maxTau);
        else                  FillDifferenceDirect(samples, maxTau);

        int chosen = -1;
        for (int tau = minTau; tau <= maxTau; tau++)
        {
            if (diff[tau] < Threshold)
            {
                while (tau + 1 <= maxTau && diff[tau + 1] < diff[tau]) tau++;
                chosen = tau;
                break;
            }
        }
        if (chosen < 0) return null;

        float t0 = diff[chosen - 1];
        float t1 = diff[chosen];
        float t2 = chosen + 1 <= maxTau ? diff[chosen + 1] : t1;
        float denom = (t0 + t2 - 2 * t1);
        float refinedTau = denom == 0 ? chosen : chosen + 0.5f * (t0 - t2) / denom;

        float freq = SampleRate / refinedTau;
        if (!float.IsFinite(freq) || freq < MinFrequency || freq > MaxFrequency) return null;

        float midi = PitchMath.HzToMidiF(freq);
        int   midiNearest = (int)MathF.Round(midi);
        float cents = (midi - midiNearest) * 100f;
        int   octave = PitchMath.Octave(midiNearest);
        float confidence = 1f - Math.Clamp(diff[chosen], 0f, 1f);

        // chosen == maxTau means YIN never found a dip inside the searchable
        // range and ran into the ceiling. The reading is kept (JuceGuitarAnalyzer
        // and the tuner UI both need to SEE it to fall back), but it is flagged:
        // it is a floor artefact, not a measurement. See JuceTunerReading.Clamped.
        var clamped = chosen >= maxTau;

        return new JuceTunerReading(freq, PitchMath.NoteName(midiNearest), octave, cents, confidence, clamped);
    }

    /// <summary>The original O(W·τmax) difference function, UNCHANGED. This is the
    /// shipped detection path; <see cref="JuceGuitarAnalyzer"/>'s output is pinned
    /// bit-for-bit against it.</summary>
    private void FillDifferenceDirect(ReadOnlySpan<float> samples, int maxTau)
    {
        var diff = _diff;
        diff[0]  = 1f;
        float runningSum = 0;
        for (int tau = 1; tau <= maxTau; tau++)
        {
            float sum = 0;
            int n = WindowSize - tau;
            for (int i = 0; i < n; i++)
            {
                var delta = samples[i] - samples[i + tau];
                sum += delta * delta;
            }
            runningSum += sum;
            diff[tau] = runningSum == 0 ? 1f : sum * tau / runningSum;
        }
    }

    /// <summary>
    /// The same difference function via autocorrelation.
    /// <para>Expanding the square,
    /// <c>d(τ) = Σ_{i&lt;W-τ} x[i]² + Σ_{τ≤i&lt;W} x[i]² - 2·Σ_{i&lt;W-τ} x[i]·x[i+τ]</c>.
    /// The two energy sums are differences of a cumulative-energy prefix (O(1)
    /// each) and the cross term is the linear autocorrelation, which one
    /// zero-padded real FFT, a power spectrum and one inverse produce for every
    /// lag at once. The cumulative-mean normalisation on top is byte-for-byte the
    /// loop the direct path runs.</para>
    /// <para>Float rounding is why this is not bit-identical: the FFT's error is
    /// relative to the window's total ENERGY, while <c>d(τ)</c> near its dip is
    /// close to zero. It is still far inside a cent — the parabolic refinement
    /// reads the curvature over ±1 lag, which is orders of magnitude larger than
    /// the noise — but "far inside a cent" is not "identical", and a detection
    /// contract needs the difference stated rather than assumed.</para>
    /// </summary>
    private void FillDifferenceFft(ReadOnlySpan<float> samples, int maxTau)
    {
        var fft    = _fft!;
        var re     = _fftRe!;
        var im     = _fftIm!;
        var prefix = _energyPrefix!;
        var diff   = _diff;

        float acc = 0f;
        prefix[0] = 0f;
        for (int i = 0; i < WindowSize; i++)
        {
            acc += samples[i] * samples[i];
            prefix[i + 1] = acc;
        }

        fft.ForwardReal(samples[..WindowSize], re, im);
        // Power spectrum in place: the inverse of |X|^2 is the autocorrelation.
        for (int k = 0; k < re.Length; k++)
        {
            re[k] = re[k] * re[k] + im[k] * im[k];
            im[k] = 0f;
        }
        fft.Inverse(re, im);

        // Below this fraction of the window's total energy the cumulative
        // difference is ROUNDING NOISE rather than a measurement, and the
        // cumulative-mean normalisation divides by it. See the loop.
        var runningFloor = prefix[WindowSize] * RunningSumEnergyFloor;

        diff[0] = 1f;
        float runningSum = 0;
        for (int tau = 1; tau <= maxTau; tau++)
        {
            var sum = prefix[WindowSize - tau] + (prefix[WindowSize] - prefix[tau]) - 2f * re[tau];
            // The direct sum of squares cannot be negative; the FFT one can be, by
            // a rounding error, exactly where d(τ) is near zero — which is the dip
            // the threshold test is looking for, so an unclamped negative would be
            // picked as a spurious period.
            //
            // The clamp is ASYMMETRIC and that is a bug of its own: it keeps small
            // POSITIVE rounding error and maps negative error to EXACTLY ZERO, and
            // `d̃(τ) == 0` is the strongest possible crossing of `Threshold` —
            // `confidence = 1 - 0 = 1.0`, maximum confidence for a period that was
            // never measured. The reachable case is a DC / near-DC window (a common
            // interface artefact): it clears the energy gate while every true d(τ)
            // is ≈0, so as soon as one τ contributes positive noise `runningSum > 0`
            // and the next clamped τ reports a rock-solid bogus pitch that
            // JuceTunerStream then marks STABLE. The direct path returns 1f there
            // and Detect correctly returns null, so the two paths disagreed exactly
            // where FftYinEquivalenceTests' 200 guitar-range sines cannot see it.
            //
            // The fix is to refuse the normalisation, not the clamp: when the
            // cumulative difference has not risen above rounding noise for this
            // window's energy, there is no periodicity information here at all and
            // 1f ("perfectly aperiodic") is the honest answer — the same answer the
            // direct path's `runningSum == 0` arm gives. The floor is ~4 decades
            // below the smallest genuine d(1) in the guitar/bass range (a 41 Hz sine
            // at 44.1 kHz already yields d(1) ≈ 3.4e-5 · energy), so this can only
            // fire on a window that carries no resolvable period.
            if (sum < 0f) sum = 0f;
            runningSum += sum;
            diff[tau] = runningSum <= runningFloor ? 1f : sum * tau / runningSum;
        }
    }

    /// <summary>See <see cref="FillDifferenceFft"/>. FFT-path only, deliberately:
    /// the direct path's output is a bit-identity contract and its sum of squares
    /// cannot go negative, so it needs no floor.</summary>
    private const float RunningSumEnergyFloor = 1e-6f;
}
