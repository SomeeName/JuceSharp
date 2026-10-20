namespace JuceSharp.Dsp;

/// <summary>One fundamental estimated by <see cref="JucePolyphonicPitch"/>.
/// <paramref name="Salience"/> is normalised so the strongest candidate of a
/// window is exactly 1.</summary>
public readonly record struct JucePitchCandidate(int Midi, float FrequencyHz, float Salience);

/// <summary>
/// Multi-F0 (polyphonic pitch) estimator for a single analysis window,
/// tuned for guitar / bass with up to six simultaneous notes.
///
/// <para>Algorithm: the 4096-sample window is Hann-weighted, zero-padded to
/// an 8192-point FFT and reduced to a list of interpolated spectral peaks
/// (30 Hz – 5 kHz). A harmonic-summation salience is evaluated on a MIDI
/// 26..88 grid in 1/3-semitone steps: each candidate scores
/// <c>Σ_h (1/h) · mag(strongest peak within ±30 cents of h·f0)</c> for
/// harmonics 1..12. Fundamentals are extracted iteratively: the best
/// candidate is refined from its matched partials, recorded, its partials
/// attenuated by 80 %, and the search repeated. Extraction stops after six
/// notes or when the residual salience falls below 25 % of the first.
/// Near-duplicates within 60 cents are merged (stronger wins).</para>
///
/// <para>All scratch buffers are reused across calls; only the returned
/// candidate array is allocated per call.</para>
/// </summary>
public sealed class JucePolyphonicPitch
{
    /// <summary>Required input window length in samples.</summary>
    public const int WindowSize = 4096;
    /// <summary>Zero-padded FFT size.</summary>
    public const int FftSize = 8192;

    private const float MinFreqHz           = 30f;
    private const float MaxFreqHz           = 5000f;
    private const int   MaxPeaks            = 80;
    private const float PeakFloorRel        = 1e-3f;
    private const int   MidiLo              = 26;
    private const int   MidiHi              = 88;
    private const int   StepsPerSemitone    = 3;
    private const int   MaxHarmonics        = 12;
    private const float MatchToleranceCents = 30f;
    private const int   MaxNotes            = 6;
    private const float StopRelative        = 0.25f;
    private const float SalienceFloorRel    = 0.01f;
    private const float Attenuation         = 0.2f;
    private const float MergeCents          = 60f;

    /// <summary>Sample rate in Hz of the audio analysed; fixed at construction and what
    /// every reported frequency is derived against.</summary>
    public int SampleRate { get; }

    private readonly JuceFft _fft;
    private readonly float[] _hann;
    private readonly float[] _re;
    private readonly float[] _im;
    private readonly float[] _mag;
    private readonly float[] _refMag;
    private readonly float[] _peakFreq;
    private readonly float[] _peakMag;
    private readonly float[] _sortScratch;
    private readonly float[] _candFreq;
    private readonly int[]   _matchedPeak;
    private readonly int[]   _matchedHarm;
    private readonly int[]   _outMidi;
    private readonly float[] _outFreq;
    private readonly float[] _outSal;
    private readonly bool[]  _outDropped;
    private readonly float   _binHz;
    private readonly int     _binLo;
    private readonly int     _binHi;
    private readonly float   _tolLo;
    private readonly float   _tolHi;
    private readonly float   _maxSearchFreq;

    private int _peakCount;
    private int _matchedCount;

    /// <summary>Allocate the FFT, the Hann window and the magnitude scratch for one
    /// analysis instance. Not thread-safe: the scratch is reused across calls, so use one
    /// instance per thread.</summary>
    /// <param name="sampleRate">Sample rate in Hz of the audio to be analysed.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="sampleRate"/> is not positive.</exception>
    public JucePolyphonicPitch(int sampleRate)
    {
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        SampleRate = sampleRate;

        _fft  = new JuceFft(FftSize);
        _hann = Windows.Hann(WindowSize);

        _re     = new float[FftSize];
        _im     = new float[FftSize];
        _mag    = new float[FftSize / 2 + 1];
        _refMag = new float[FftSize / 2 + 1];

        _binHz         = (float)sampleRate / FftSize;
        _maxSearchFreq = MathF.Min(MaxFreqHz, sampleRate * 0.5f * 0.99f);
        _binLo = Math.Max(2, (int)MathF.Ceiling(MinFreqHz / _binHz));
        _binHi = Math.Min(FftSize / 2 - 1, (int)(_maxSearchFreq / _binHz));

        _peakFreq    = new float[FftSize / 2];
        _peakMag     = new float[FftSize / 2];
        _sortScratch = new float[FftSize / 2];

        int candidates = (MidiHi - MidiLo) * StepsPerSemitone + 1;
        _candFreq = new float[candidates];
        for (int i = 0; i < candidates; i++)
        {
            float midi = MidiLo + (float)i / StepsPerSemitone;
            _candFreq[i] = 440f * MathF.Pow(2f, (midi - 69f) / 12f);
        }

        _matchedPeak = new int[MaxHarmonics];
        _matchedHarm = new int[MaxHarmonics];
        _outMidi     = new int[MaxNotes];
        _outFreq     = new float[MaxNotes];
        _outSal      = new float[MaxNotes];
        _outDropped  = new bool[MaxNotes];

        _tolLo = MathF.Pow(2f, -MatchToleranceCents / 1200f);
        _tolHi = MathF.Pow(2f,  MatchToleranceCents / 1200f);
    }

    /// <summary>Estimate the fundamentals present in a 4096-sample mono
    /// window. Returns candidates sorted by salience descending, salience
    /// normalised so the strongest is 1. Returns an empty array for
    /// silence or a peak-free spectrum.</summary>
    public JucePitchCandidate[] Detect(ReadOnlySpan<float> window4096)
    {
        ValidateWindow(window4096, nameof(window4096));
        ComputeMagnitude(window4096, _mag);
        return DetectFromMagnitude();
    }

    /// <summary>Onset-synchronous variant: estimates only the energy that is
    /// NEW relative to <paramref name="referenceWindow4096"/> (typically the
    /// samples just before the onset). Both spectra run through the same
    /// Hann + zero-pad pipeline; peak-picking and salience are evaluated on
    /// the half-wave-rectified per-bin difference
    /// <c>max(0, |X_onset| - |X_ref|)</c>, so partials that were already
    /// ringing before the onset cancel and do not score. A reference window
    /// of all zeros reproduces <see cref="Detect(ReadOnlySpan{float})"/>
    /// exactly.</summary>
    public JucePitchCandidate[] Detect(ReadOnlySpan<float> window4096,
                                       ReadOnlySpan<float> referenceWindow4096)
    {
        ValidateWindow(window4096, nameof(window4096));
        ValidateWindow(referenceWindow4096, nameof(referenceWindow4096));
        ComputeMagnitude(referenceWindow4096, _refMag);
        ComputeMagnitude(window4096, _mag);
        for (int k = _binLo - 1; k <= _binHi + 1; k++)
            _mag[k] = MathF.Max(0f, _mag[k] - _refMag[k]);
        return DetectFromMagnitude();
    }

    private static void ValidateWindow(ReadOnlySpan<float> window, string paramName)
    {
        if (window.Length != WindowSize)
            throw new ArgumentException($"Window must be exactly {WindowSize} samples.", paramName);
    }

    private void ComputeMagnitude(ReadOnlySpan<float> window, float[] dest)
    {
        for (int i = 0; i < WindowSize; i++) _re[i] = window[i] * _hann[i];
        Array.Clear(_re, WindowSize, FftSize - WindowSize);
        Array.Clear(_im);
        _fft.Forward(_re, _im);
        for (int k = _binLo - 1; k <= _binHi + 1; k++)
            dest[k] = MathF.Sqrt(_re[k] * _re[k] + _im[k] * _im[k]);
    }

    private JucePitchCandidate[] DetectFromMagnitude()
    {
        PickPeaks();
        if (_peakCount == 0) return Array.Empty<JucePitchCandidate>();

        float maxPeakMag = 0f;
        for (int p = 0; p < _peakCount; p++)
            if (_peakMag[p] > maxPeakMag) maxPeakMag = _peakMag[p];
        float absFloor = maxPeakMag * SalienceFloorRel;

        int   count = 0;
        float first = 0f;
        for (int round = 0; round < MaxNotes; round++)
        {
            int   bestIdx = -1;
            float bestSal = 0f;
            for (int c = 0; c < _candFreq.Length; c++)
            {
                float s = MatchHarmonics(_candFreq[c], record: false);
                if (s > bestSal) { bestSal = s; bestIdx = c; }
            }
            if (bestIdx < 0 || bestSal <= absFloor) break;
            if (round == 0) first = bestSal;
            else if (bestSal < StopRelative * first) break;

            // Re-match to record the winning partials, refine f0 as the
            // magnitude-weighted mean of matchedPeakFreq / h.
            MatchHarmonics(_candFreq[bestIdx], record: true);
            float num = 0f, den = 0f;
            for (int m = 0; m < _matchedCount; m++)
            {
                int p = _matchedPeak[m];
                num += _peakMag[p] * (_peakFreq[p] / _matchedHarm[m]);
                den += _peakMag[p];
            }
            float refined = den > 0f ? num / den : _candFreq[bestIdx];

            _outFreq[count]    = refined;
            _outSal[count]     = bestSal;
            _outMidi[count]    = (int)MathF.Round(PitchMath.HzToMidiF(refined));
            _outDropped[count] = false;
            count++;

            for (int m = 0; m < _matchedCount; m++)
                _peakMag[_matchedPeak[m]] *= Attenuation;
        }
        if (count == 0) return Array.Empty<JucePitchCandidate>();

        // Merge candidates within MergeCents; earlier entries are stronger.
        for (int i = 0; i < count; i++)
        {
            if (_outDropped[i]) continue;
            for (int j = i + 1; j < count; j++)
            {
                if (_outDropped[j]) continue;
                float cents = MathF.Abs(PitchMath.CentsF(_outFreq[j], _outFreq[i]));
                if (cents < MergeCents) _outDropped[j] = true;
            }
        }

        int kept = 0;
        for (int i = 0; i < count; i++) if (!_outDropped[i]) kept++;
        var result = new JucePitchCandidate[kept];
        float norm = _outSal[0];
        int   w    = 0;
        for (int i = 0; i < count; i++)
        {
            if (_outDropped[i]) continue;
            result[w++] = new JucePitchCandidate(_outMidi[i], _outFreq[i], _outSal[i] / norm);
        }
        return result;
    }

    /// <summary>Collect local spectral maxima with parabolic interpolation,
    /// keep at most <see cref="MaxPeaks"/> (strongest), preserving ascending
    /// frequency order.</summary>
    private void PickPeaks()
    {
        _peakCount = 0;
        float maxMag = 0f;
        for (int k = _binLo; k <= _binHi; k++)
            if (_mag[k] > maxMag) maxMag = _mag[k];
        float floor = maxMag * PeakFloorRel;
        if (maxMag <= 0f) return;

        int total = 0;
        for (int k = _binLo; k <= _binHi; k++)
        {
            float m = _mag[k];
            if (m < floor || m <= _mag[k - 1] || m <= _mag[k + 1]) continue;

            float a = _mag[k - 1], c = _mag[k + 1];
            float denom = a - 2f * m + c;
            float delta = denom == 0f ? 0f : 0.5f * (a - c) / denom;
            delta = Math.Clamp(delta, -0.5f, 0.5f);
            _peakFreq[total] = (k + delta) * _binHz;
            _peakMag[total]  = m - 0.25f * (a - c) * delta;
            total++;
        }

        if (total > MaxPeaks)
        {
            // Find the magnitude cutoff of the MaxPeaks strongest, then
            // compact in-place preserving frequency order.
            Array.Copy(_peakMag, _sortScratch, total);
            Array.Sort(_sortScratch, 0, total);
            float cutoff = _sortScratch[total - MaxPeaks];
            int w = 0;
            for (int p = 0; p < total && w < MaxPeaks; p++)
            {
                if (_peakMag[p] < cutoff) continue;
                _peakFreq[w] = _peakFreq[p];
                _peakMag[w]  = _peakMag[p];
                w++;
            }
            total = w;
        }
        _peakCount = total;
    }

    /// <summary>Harmonic-summation salience of <paramref name="f0"/>. With
    /// <paramref name="record"/> the matched (peak, harmonic) pairs are
    /// captured for refinement / attenuation.</summary>
    private float MatchHarmonics(float f0, bool record)
    {
        if (record) _matchedCount = 0;
        float salience = 0f;
        for (int h = 1; h <= MaxHarmonics; h++)
        {
            float target = h * f0;
            if (target > _maxSearchFreq) break;
            float lo = target * _tolLo;
            float hi = target * _tolHi;

            int   best    = -1;
            float bestMag = 0f;
            for (int p = 0; p < _peakCount; p++)
            {
                float pf = _peakFreq[p];
                if (pf < lo) continue;
                if (pf > hi) break;
                if (_peakMag[p] > bestMag) { bestMag = _peakMag[p]; best = p; }
            }
            if (best < 0) continue;

            salience += bestMag / h;
            if (record)
            {
                _matchedPeak[_matchedCount] = best;
                _matchedHarm[_matchedCount] = h;
                _matchedCount++;
            }
        }
        return salience;
    }
}
