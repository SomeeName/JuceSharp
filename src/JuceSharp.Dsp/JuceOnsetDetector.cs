namespace JuceSharp.Dsp;

/// <summary>A single onset detected by <see cref="JuceOnsetDetector"/>.
/// <paramref name="SamplePosition"/> is relative to the monotonically
/// counted stream (samples pushed since construction / <see cref="JuceOnsetDetector.Reset"/>).
/// <paramref name="Strength"/> is the detection flux divided by the adaptive
/// threshold (always &gt; 1 for an emitted onset).</summary>
public readonly record struct JuceOnset(long SamplePosition, float Strength);

/// <summary>
/// Streaming spectral-flux onset detector.
///
/// <para>Runs a 1024-point Hann-windowed FFT every 256 samples and computes
/// the positive, log-compressed spectral flux between consecutive hops:
/// <c>flux = Σ max(0, log(1+|X_k|) - log(1+|X_k,prev|))</c> over bins
/// 1..N/2. An onset is reported when the flux exceeds an adaptive
/// threshold (median of the last 21 flux values * 1.5, floored) and is a
/// local maximum — the decision is therefore made one hop late so the
/// value can be compared with its successor. A minimum inter-onset gap
/// of 45 ms suppresses double triggers.</para>
///
/// <para>Pure managed, allocation-free per call apart from the returned
/// event list. Feed arbitrary-size mono blocks via <c>Push</c>.</para>
/// </summary>
public sealed class JuceOnsetDetector
{
    /// <summary>Analysis FFT size in samples.</summary>
    public const int FftSize = 1024;
    /// <summary>Analysis hop size in samples.</summary>
    public const int HopSize = 256;

    private const int    MedianWindow   = 21;
    private const float  ThresholdScale = 1.5f;
    private const float  ThresholdFloor = 1e-4f;
    private const double MinGapSeconds  = 0.045;

    /// <summary>Offset (samples) added to the triggering window's start to
    /// estimate the perceptual attack position. Tuned against synthetic
    /// plucks so reported positions land within a few hops of the true
    /// attack.</summary>
    public const int PositionOffset = 384;

    /// <summary>
    /// The detector's inherent REPORT delay, in samples: how far past a
    /// reported <see cref="JuceOnset.SamplePosition"/> the stream must have
    /// advanced before that onset comes out of <see cref="Push(ReadOnlySpan{float})"/>.
    /// <para>It is exact, not a tuning: the decision is made one hop late so
    /// the flux can be compared with its successor, so a window starting at
    /// <c>w</c> is judged once <c>w + FftSize + HopSize</c> samples have been
    /// consumed while it reports position <c>w + PositionOffset</c> —
    /// <c>FftSize + HopSize - PositionOffset</c> = 896 samples, about 20 ms at
    /// 44.1 kHz. A caller aligning onsets to its own input timeline needs this
    /// (and <see cref="PositionOffset"/>, which is already baked into the
    /// reported position) rather than guessing from the FFT size.</para>
    /// <para>Pushing in blocks larger than <see cref="HopSize"/> adds the
    /// block's own quantisation on top; this constant is the floor.</para>
    /// </summary>
    public const int LatencySamples = FftSize + HopSize - PositionOffset;

    /// <summary>Sample rate in Hz of the audio pushed in; the axis
    /// <c>StreamPosition</c> and every reported onset position are counted on.</summary>
    public int  SampleRate { get; }

    /// <summary>Total samples pushed since construction or <see cref="Reset"/>.</summary>
    public long StreamPosition { get; private set; }

    /// <summary>
    /// Lifetime count of non-finite (NaN / ±Inf) input samples replaced with
    /// silence at <c>Push</c>. <b>Not</b> cleared by <see cref="Reset"/> —
    /// like <see cref="JuceConstantQ.NonFiniteSamples"/> it is a health counter,
    /// not per-session state.
    /// <para>Unscrubbed, one NaN sample poisons <c>_prevLogMag</c> and then the
    /// flux ring: <c>Median()</c> sorts a ring containing NaN, and
    /// <c>Array.Sort</c> puts NaN FIRST, so the median moves and the adaptive
    /// threshold is wrong for the next 21 hops — during which every
    /// <c>flux &gt; threshold</c> comparison against a NaN is false, so the
    /// detector emits nothing and reports nothing.</para>
    /// </summary>
    public long NonFiniteSamples { get; private set; }

    private readonly JuceFft _fft;
    private readonly float[] _hann;
    private readonly float[] _window;
    private readonly float[] _re;
    private readonly float[] _im;
    private float[] _logMag;
    private float[] _prevLogMag;
    private readonly float[] _fluxRing;
    private readonly float[] _medianScratch;
    private readonly long    _minGapSamples;

    private int  _fill;
    private bool _haveMag;
    private int  _fluxCount;
    private int  _fluxHead;
    private long _windowsProcessed;

    // One-hop-delayed local-maximum decision state.
    private bool  _havePending;
    private float _pendingFlux;
    private float _pendingThreshold;
    private long  _pendingWindowStart;
    private float _fluxBeforePending;
    private long  _lastOnsetPos;

    /// <summary>Allocate the FFT, the Hann window and the flux history for one streaming
    /// detector. The transform size and hop are fixed constants of this type, so latency is
    /// the compile-time <c>LatencySamples</c>.</summary>
    /// <param name="sampleRate">Sample rate in Hz of the audio to be pushed in.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="sampleRate"/> is not positive.</exception>
    public JuceOnsetDetector(int sampleRate)
    {
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        SampleRate = sampleRate;

        _fft    = new JuceFft(FftSize);
        _hann   = Windows.Hann(FftSize);

        _window        = new float[FftSize];
        _re            = new float[FftSize];
        _im            = new float[FftSize];
        _logMag        = new float[FftSize / 2 + 1];
        _prevLogMag    = new float[FftSize / 2 + 1];
        _fluxRing      = new float[MedianWindow];
        _medianScratch = new float[MedianWindow];
        _minGapSamples = (long)(MinGapSeconds * sampleRate);
        Reset();
    }

    /// <summary>Reset all internal state and rewind <see cref="StreamPosition"/> to zero.</summary>
    public void Reset()
    {
        StreamPosition    = 0;
        _fill             = 0;
        _haveMag          = false;
        _fluxCount        = 0;
        _fluxHead         = 0;
        _windowsProcessed = 0;
        _havePending      = false;
        _pendingFlux      = 0;
        _pendingThreshold = 0;
        _fluxBeforePending = 0;
        _lastOnsetPos     = long.MinValue / 2;
        Array.Clear(_window);
        Array.Clear(_prevLogMag);
    }

    /// <summary>
    /// Lifetime count of onsets that could not be written because the caller's
    /// destination span in <see cref="Push(ReadOnlySpan{float}, Span{JuceOnset}, out int)"/>
    /// was full. <b>A dropped onset is silently missing detection</b>, so this
    /// is a health counter and not per-session state: <see cref="Reset"/> leaves
    /// it alone. Size the span with <see cref="RequiredOnsetCapacity"/> and it
    /// stays zero.
    /// </summary>
    public long DroppedOnsets { get; private set; }

    /// <summary>
    /// The largest number of onsets a single <c>Push</c> of
    /// <paramref name="blockSamples"/> samples can produce, and therefore the
    /// span length that guarantees <see cref="DroppedOnsets"/> never moves:
    /// one per analysis hop the block completes, plus the hop carried in from
    /// the previous call, plus one for the pending decision.
    /// </summary>
    public static int RequiredOnsetCapacity(int blockSamples) =>
        blockSamples <= 0 ? 0 : blockSamples / HopSize + 2;

    /// <summary>Push a block of mono samples. Returns the onsets detected
    /// within this block (positions are stream-relative and monotonic).
    /// The list is freshly allocated per call only when onsets occur.</summary>
    public IReadOnlyList<JuceOnset> Push(ReadOnlySpan<float> samples)
    {
        if (samples.IsEmpty) return Array.Empty<JuceOnset>();
        var sink = new OnsetSink((List<JuceOnset>?)null);
        PushCore(samples, ref sink);
        return (IReadOnlyList<JuceOnset>?)sink.List ?? Array.Empty<JuceOnset>();
    }

    /// <summary>
    /// Allocation-free <see cref="Push(ReadOnlySpan{float})"/>: the onsets are
    /// written into <paramref name="outOnsets"/> instead of a freshly allocated
    /// list. Detection is bit-identical to the list overload — it is the same
    /// code path with a different sink.
    /// </summary>
    /// <param name="samples">Mono block; any length.</param>
    /// <param name="outOnsets">Destination. Size it with
    /// <see cref="RequiredOnsetCapacity"/>; onsets past the end are DROPPED and
    /// counted in <see cref="DroppedOnsets"/> rather than throwing, because a
    /// throw halfway through a block would leave the detector's stream position
    /// past audio whose onsets were never reported.</param>
    /// <param name="count">How many entries of <paramref name="outOnsets"/> were
    /// written.</param>
    public void Push(ReadOnlySpan<float> samples, Span<JuceOnset> outOnsets, out int count)
    {
        if (samples.IsEmpty) { count = 0; return; }
        var sink = new OnsetSink(outOnsets);
        PushCore(samples, ref sink);
        count = sink.Count;
        DroppedOnsets += sink.Dropped;
    }

    // One sink so the two overloads cannot drift: a list (allocated lazily, as
    // before) or a caller-owned span.
    private ref struct OnsetSink
    {
        public List<JuceOnset>? List;
        private readonly Span<JuceOnset> _buffer;
        private readonly bool _spanMode;
        public int  Count;
        public long Dropped;

        public OnsetSink(Span<JuceOnset> buffer) { _buffer = buffer; _spanMode = true; List = null; }
        public OnsetSink(List<JuceOnset>? list)  { _buffer = default; _spanMode = false; List = list; }

        public void Add(JuceOnset onset)
        {
            if (!_spanMode) { (List ??= new()).Add(onset); Count++; return; }
            if (Count >= _buffer.Length) { Dropped++; return; }
            _buffer[Count++] = onset;
        }
    }

    private void PushCore(ReadOnlySpan<float> samples, ref OnsetSink onsets)
    {
        StreamPosition += samples.Length;

        int src = 0;
        while (src < samples.Length)
        {
            int copy = Math.Min(_window.Length - _fill, samples.Length - src);
            // Scrub at the public boundary (see NonFiniteSamples) instead of a
            // bulk CopyTo — same rule the CQT and the native chain follow.
            var chunk = samples.Slice(src, copy);
            for (int i = 0; i < copy; i++)
            {
                var s = chunk[i];
                if (float.IsFinite(s)) { _window[_fill + i] = s; }
                else { _window[_fill + i] = 0f; NonFiniteSamples++; }
            }
            _fill += copy;
            src   += copy;
            if (_fill < _window.Length) break;

            ProcessWindow(ref onsets);

            int retain = _window.Length - HopSize;
            Array.Copy(_window, HopSize, _window, 0, retain);
            _fill = retain;
        }
    }

    private void ProcessWindow(ref OnsetSink onsets)
    {
        long windowStart = _windowsProcessed * HopSize;
        _windowsProcessed++;

        for (int i = 0; i < FftSize; i++)
        {
            _re[i] = _window[i] * _hann[i];
            _im[i] = 0f;
        }
        _fft.Forward(_re, _im);

        float flux = 0f;
        for (int k = 1; k <= FftSize / 2; k++)
        {
            float mag = MathF.Sqrt(_re[k] * _re[k] + _im[k] * _im[k]);
            float lm  = MathF.Log(1f + mag);
            _logMag[k] = lm;
            if (_haveMag)
            {
                float d = lm - _prevLogMag[k];
                if (d > 0f) flux += d;
            }
        }
        (_prevLogMag, _logMag) = (_logMag, _prevLogMag);

        if (!_haveMag)
        {
            _haveMag = true;
            return;
        }

        _fluxRing[_fluxHead] = flux;
        _fluxHead = (_fluxHead + 1) % MedianWindow;
        if (_fluxCount < MedianWindow) _fluxCount++;
        float threshold = Median() * ThresholdScale + ThresholdFloor;

        // Evaluate the previous hop's flux now that we can see its successor.
        if (_havePending)
        {
            if (_pendingFlux > _pendingThreshold &&
                _pendingFlux > _fluxBeforePending &&
                _pendingFlux >= flux)
            {
                long pos = _pendingWindowStart + PositionOffset;
                if (pos - _lastOnsetPos >= _minGapSamples)
                {
                    onsets.Add(new JuceOnset(pos, _pendingFlux / _pendingThreshold));
                    _lastOnsetPos = pos;
                }
            }
            _fluxBeforePending = _pendingFlux;
        }

        _pendingFlux        = flux;
        _pendingThreshold   = threshold;
        _pendingWindowStart = windowStart;
        _havePending        = true;
    }

    private float Median()
    {
        Array.Copy(_fluxRing, _medianScratch, _fluxCount);
        Array.Sort(_medianScratch, 0, _fluxCount);
        return _medianScratch[_fluxCount / 2];
    }
}
