namespace JuceSharp.Dsp;

/// <summary>Kind of event produced by <see cref="JuceGuitarAnalyzer"/>.</summary>
public enum JuceGuitarEventKind
{
    /// <summary>A picked attack — a note or chord struck with the pick or fingers.</summary>
    Onset,
    /// <summary>A pitch change with no new attack: a hammer-on, pull-off or slide.</summary>
    Legato,
}

/// <summary>Playing-technique flags inferred per event.</summary>
[Flags]
public enum JuceGuitarTechniques
{
    /// <summary>No technique was inferred — an ordinary picked note.</summary>
    None          = 0,
    /// <summary>Palm mute: the picking hand damps the strings at the bridge, stripping the
    /// upper partials and shortening the decay.</summary>
    PalmMute      = 1,
    /// <summary>Natural harmonic: a nearly pure partial with almost no fundamental.</summary>
    Harmonic      = 2,
    /// <summary>Pinch harmonic: a picked attack whose energy sits on a high partial rather
    /// than the fretted fundamental.</summary>
    PinchHarmonic = 4,
    /// <summary>Percussive hit with no resolvable pitch — a muted scrape or a body knock.</summary>
    Percussive    = 8,
    /// <summary>Bass slap: a thumb strike against the fretboard, broadband and very fast
    /// in attack.</summary>
    Slap          = 16,
    /// <summary>Bass pop: a string pulled away from the fretboard and released, bright and
    /// snapped.</summary>
    Pop           = 32,
    /// <summary>The event was a legato transition rather than a new pick; set on every
    /// event whose kind is <see cref="JuceGuitarEventKind.Legato"/>.</summary>
    Legato        = 64,
}

/// <summary>One analyzed guitar event (picked note/chord attack or legato
/// transition) with its pitch content and timbral features.</summary>
public readonly record struct JuceGuitarEvent(
    JuceGuitarEventKind Kind,
    double StreamSeconds,
    ulong StreamSamplePosition,
    JucePitchCandidate[] Pitches,   // strongest first; may be empty (percussive)
    float[] Chroma,                 // 12 bins
    float SpectralCentroidHz,
    float Rms,
    float CrestFactor,
    float SpectralFlatness,
    float DecayRatio,               // RMS ~80 ms after onset / RMS at onset peak; low = damped
    JuceGuitarTechniques Techniques)
{
    /// <summary>The position of the provisional onset this event FINALIZES — the
    /// <see cref="JuceOnsetNotice.DetectorSamplePosition"/> of the
    /// <see cref="JuceOnsetNoticeKind.Detected"/> notice published for it
    /// <see cref="JuceGuitarAnalyzer.LatencySamples"/> earlier, if one was. Set on every event the onset
    /// detector started (a picked <see cref="JuceGuitarEventKind.Onset"/>, and one demoted to
    /// <see cref="JuceGuitarEventKind.Legato"/> for want of a fresh attack); null on a legato
    /// event the pitch tracker found with no detector onset behind it. It is the detector's
    /// raw position, NOT <see cref="StreamSamplePosition"/>, which has been refined onto the
    /// attack since.</summary>
    public ulong? DetectorSamplePosition { get; init; }
}

/// <summary>What a <see cref="JuceOnsetNotice"/> reports about one detector onset.</summary>
public enum JuceOnsetNoticeKind
{
    /// <summary>The onset detector fired on an attack the audio so far shows: a PROVISIONAL
    /// attack, published the moment it is detected — <see cref="JuceGuitarAnalyzer.LatencySamples"/>
    /// before the analyzed <see cref="JuceGuitarEvent"/> that will finalize it (or a
    /// <see cref="Withdrawn"/> notice that it never will). A detector onset with no rise behind
    /// it yet (the flux detector also fires on the ripple of a ringing note) is not published.</summary>
    Detected,

    /// <summary>The onset produced no event: the novelty gate, the noise gate or the
    /// attack-ratio test rejected it, or it refined onto an attack an earlier onset already
    /// reported. Nothing will finalize it.</summary>
    Withdrawn,
}

/// <summary>
/// A provisional attack report from <see cref="JuceGuitarAnalyzer"/>: the onset detector's own
/// position and strength, out of
/// <see cref="JuceGuitarAnalyzer.Push(ReadOnlySpan{float}, List{JuceOnsetNotice})"/> as soon as
/// the detector fires, long before the event is analyzed. Every
/// <see cref="JuceOnsetNoticeKind.Detected"/> notice is resolved exactly once: by a
/// <see cref="JuceGuitarEvent"/> whose <see cref="JuceGuitarEvent.DetectorSamplePosition"/>
/// equals <see cref="DetectorSamplePosition"/>, or by a <see cref="JuceOnsetNoticeKind.Withdrawn"/>
/// notice with that position — unless <see cref="JuceGuitarAnalyzer.Reset"/> drops it first. The
/// reverse does not hold: an event can finalize a detector onset that was never published (its
/// attack was not audible yet, or showed too little rise, when the detector fired).
/// </summary>
/// <param name="Kind">Detected (a new provisional onset) or Withdrawn (one that will produce no
/// event).</param>
/// <param name="DetectorSamplePosition">The onset detector's own absolute stream position — the
/// KEY that pairs the notice with its finalized event
/// (<see cref="JuceGuitarEvent.DetectorSamplePosition"/>). Not an attack estimate: the detector
/// reports a fixed offset into the triggering FFT window, measured ~300 samples early at
/// 48 kHz.</param>
/// <param name="StreamSamplePosition">The PROVISIONAL attack: the detector position snapped onto
/// the attack by the same short-frame RMS-rise search that refines the finalized event's
/// <see cref="JuceGuitarEvent.StreamSamplePosition"/>, over the audio heard so far (that search
/// reads up to ~80 ms past the attack, this one stops at the end of the stream). Within one
/// <see cref="JuceOnsetDetector.HopSize"/> of the attack on a clean pluck. Equal to
/// <paramref name="DetectorSamplePosition"/> in a Withdrawn notice.</param>
/// <param name="StreamSeconds"><paramref name="StreamSamplePosition"/> in seconds, on the same axis
/// as <see cref="JuceGuitarEvent.StreamSeconds"/>.</param>
/// <param name="Strength">Detection flux over the adaptive threshold
/// (<see cref="JuceOnset.Strength"/>; always above 1).</param>
public readonly record struct JuceOnsetNotice(
    JuceOnsetNoticeKind Kind,
    ulong DetectorSamplePosition,
    ulong StreamSamplePosition,
    double StreamSeconds,
    float Strength);

/// <summary>
/// Streaming composite analyzer for live guitar / bass: the one-stop
/// event stream combining onset detection, polyphonic pitch estimation,
/// chroma, timbral features and technique heuristics.
///
/// <para>Pipeline: incoming audio feeds a <see cref="JuceOnsetDetector"/>
/// continuously and is mirrored into an internal history ring. When an
/// onset fires at position P it is queued; once samples up to
/// <c>P + LatencySamples</c> are available the event is finalized — that lag covers
/// the REFINED position (up to <c>AnalysisWindow - PreOnsetLookback</c> further on)
/// plus the decay window read past it, not merely <c>4096 + 80 ms</c>: a
/// 4096-sample window starting at <c>P - 256</c> is analyzed with
/// <see cref="JucePolyphonicPitch"/> against a reference window ending at
/// <c>P - 256</c>, so still-ringing previous strings cancel out and only
/// newly excited energy scores (cross-checked against the YIN pitch
/// from <see cref="JuceChromaticTuner"/>), chroma is computed via
/// <see cref="JuceChromagram"/>, and RMS / crest / centroid / flatness /
/// decay drive the technique flags.</para>
///
/// <para>Legato (hammer-on / pull-off / slide) transitions that produce no
/// attack are caught two ways: a rolling YIN tracker emits a
/// <see cref="JuceGuitarEventKind.Legato"/> event when a stable pitch moves
/// ≥ 60 cents without a recent onset, and onsets whose local energy shows
/// no fresh attack (pre/post RMS ratio below a threshold) are demoted to
/// Legato events instead of being reported as picked notes.</para>
///
/// <para>Events are emitted in stream order with absolute sample
/// positions; <see cref="Reset"/> re-anchors the absolute position when
/// the caller's source ring skipped.</para>
///
/// <para><b>Provisional onsets.</b> An event is reported
/// <see cref="LatencySamples"/> (~190 ms) after its attack, because analyzing it needs the
/// audio that follows. A caller that only needs WHEN — a game timing a hit another detector
/// already confirmed — passes a notice list to
/// <see cref="Push(ReadOnlySpan{float}, List{JuceOnsetNotice})"/> and receives each detector
/// onset as a <see cref="JuceOnsetNoticeKind.Detected"/> notice the moment it fires, then the
/// finalized event (paired by <see cref="JuceGuitarEvent.DetectorSamplePosition"/>) or a
/// <see cref="JuceOnsetNoticeKind.Withdrawn"/> notice. Detection itself is unchanged.</para>
/// </summary>
public sealed class JuceGuitarAnalyzer
{
    private const int   AnalysisWindow      = 4096;
    private const int   RingSize            = 32768;
    private const int   ChunkSamples        = 4096;
    private const int   LegatoWindow        = 2048;
    private const int   LegatoHop           = 256;

    /// <summary>
    /// Lowest pitch the YIN cross-check and the legato tracker may report.
    /// <para><b>Not the tuner's 50 Hz default — that default blinded this analyzer to a
    /// bass.</b> <c>JuceChromaticTuner.Detect</c> rejects anything under its
    /// <c>MinFrequency</c> outright, so at 50 Hz the lowest detectable note was MIDI 32
    /// (G♯1, 51.9 Hz) and the whole bottom of the low E string was invisible:
    /// <c>AudioToChartOptions.StandardBassMidi</c> is {28, 33, 38, 43}, and MIDI 28 (E1,
    /// 41.2 Hz) — the open low E — plus frets 1–3 all sat below the floor, as does a
    /// 5-string's low B (MIDI 23, 30.9 Hz). The poly detector covers the range
    /// (<c>MinFreqHz = 30</c>), so notes were still FOUND — they just never got the YIN half
    /// of the pipeline: no cent-accurate snap, no <see cref="YinAddedSalience"/> candidate,
    /// and no legato at all. Same class of bug as the live verifier's E2→E1 widening.</para>
    /// <para>30 Hz reaches MIDI 23 and below. Nothing above 50 Hz changes: YIN scans lags
    /// ascending and takes the FIRST below threshold, and the extra lags this admits are all
    /// LONGER, so an existing detection picks the identical τ.</para>
    /// </summary>
    private const float YinMinFreqHz = 30f;

    /// <summary>
    /// Fallback window for the legato tracker's YIN, used only when
    /// <see cref="LegatoWindow"/> could not resolve a pitch.
    /// <para><b>A lower frequency floor alone cannot fix legato — the window is the binding
    /// constraint.</b> YIN searches lags up to <c>WindowSize / 2</c>, so a 2048-sample window
    /// tops out at 43.1 Hz at 44.1 kHz (τ = 1024) and a bass low E needs τ ≈ 1070. That is
    /// window theory, not a threshold to tune: at τ = 1070 a 2048-sample window leaves 978
    /// samples of overlap — less than one period. 4096 gives τ up to 2048 and ~2.8 periods
    /// of overlap at E1.</para>
    /// <para>It runs only on the FALLBACK, and only when the window already carries
    /// note-level signal (<see cref="LegatoMinRms"/> — the same gate the emitted event must
    /// pass anyway), because the YIN difference function is O(W·τmax): unconditionally
    /// doubling the window would have roughly 3.5×'d what is already the dominant cost on
    /// the offline audio→chart path. In ordinary playing above 43 Hz the short window
    /// succeeds and this never runs.</para>
    /// </summary>
    private const int   LegatoYinWindow     = 2 * LegatoWindow;

    /// <summary>How close to the short window's lag ceiling still counts as "clamped".
    /// 5% ≈ one twelfth of a semitone of slack.</summary>
    private const float LegatoShortFloorMargin = 1.05f;
    private const int   ShortRmsWindow      = 1024;
    private const int   PreOnsetLookback    = 256;
    private const double DecayDelaySeconds  = 0.080;
    private const float MinCandidateSalience = 0.20f;
    private const float YinSnapCents        = 60f;
    private const float YinAddedSalience    = 0.5f;
    private const float FeatureMaxFreqHz    = 5000f;

    // Technique thresholds.
    private const float PalmMuteMaxDecay    = 0.35f;
    private const float PalmMuteMaxCentroid = 2600f;
    private const float PercussiveFlatness  = 0.45f;
    private const float SlapMaxCentroid     = 1200f;
    private const float PopMinCentroid      = 1800f;
    private const float SlapPopMinCrest     = 4f;
    private const float HarmonicMinPitchHz  = 280f;
    private const int   HarmonicMaxCount    = 2;
    private const float HarmonicCentroidMul = 1.9f;
    private const float PinchMinPitchHz     = 500f;

    // Legato thresholds.
    private const float  LegatoStableCents      = 50f;
    private const float  LegatoTriggerCents     = 60f;
    private const float  LegatoMinRms           = 0.01f;
    private const double LegatoOnsetGapSeconds  = 0.050;
    private const double LegatoRateLimitSeconds = 0.060;

    /// <summary>Attack ratio (RMS just after the onset over RMS just
    /// before it) below which a detected onset is considered attack-free
    /// and demoted to a Legato event (or dropped).</summary>
    private const float AttackRatioMin = 1.8f;

    /// <summary>Minimum spectral novelty (energy of the half-wave-rectified
    /// magnitude difference between the analysis window and the pre-onset
    /// reference window, relative to the reference energy) for a detected
    /// onset to be analyzed at all. Smoothly decaying notes score near zero
    /// here even when window-leakage ripple makes the 1024-point flux
    /// detector fire, so this rejects spurious mid-note onsets.</summary>
    private const float NoveltyMin = 0.3f;

    /// <summary>Attack-position refinement: search from RefineBack samples
    /// before the detector position to the full analysis lookahead after it
    /// (everything the analysis window can see), in RefineFrame steps, for
    /// short-frame RMS rises. The earliest frame within RefineRiseFraction
    /// of the best rise wins, so the position snaps to the true attack
    /// rather than to waveform-phase ripple.</summary>
    private const int   RefineBack         = 2048;
    private const int   RefineFrame        = 256;
    private const float RefineRiseFraction = 0.6f;
    private const float RefineMinRise      = 1.05f;

    /// <summary>
    /// The least audio after the provisional attack estimate that the publish test
    /// (<see cref="ShowsFreshAttack"/>) will judge on: half a <see cref="ShortRmsWindow"/>, about
    /// one period of a bass low E at 44.1 kHz. The detector reports
    /// <see cref="JuceOnsetDetector.LatencySamples"/> (896) past its position, which sits a few
    /// hundred samples before the attack, so this is normally there; when it is not, the onset is
    /// simply not published.
    /// </summary>
    private const int ProvisionalMinPostSamples = ShortRmsWindow / 2;

    /// <summary>Absolute noise gate for picked attacks: onsets whose
    /// short-window RMS just after the attack stays below this are dropped
    /// entirely. Background static, interface hiss and electrical crackle
    /// ride well below real playing levels, but they still modulate the
    /// spectrum enough to fire the relative flux detector — an absolute
    /// floor is the only thing that reliably silences them. Matches
    /// <see cref="LegatoMinRms"/> by default so the two event paths gate
    /// consistently.</summary>
    public float MinAttackRms { get; init; } = 0.01f;

    /// <summary>Sample rate in Hz of the audio pushed into this analyzer; fixed at
    /// construction and the axis every reported <c>StreamSeconds</c> is on.</summary>
    public int SampleRate { get; }

    /// <summary>Absolute stream position: the anchor passed to
    /// <see cref="Reset"/> plus every sample pushed since.</summary>
    public ulong StreamPosition => (ulong)_writePos;

    /// <summary>How many non-finite (NaN / ±Inf) input samples <c>Push</c> has
    /// scrubbed to 0. Zero for any sane source; a climbing counter means the audio reaching
    /// detection is corrupt. Mirrors the native rule that <c>DspChain::process</c> scrubs
    /// non-finite samples at the input and after every stage — a comparison against NaN is
    /// false, so an unscrubbed one passes gates instead of failing them.</summary>
    public long NonFiniteSamples => _nonFiniteSamples;
    private long _nonFiniteSamples;

    /// <summary>How many legato hops the long YIN window (see <see cref="LegatoYinWindow"/>)
    /// RESOLVED after the short one could not. Non-zero means bass-register material is
    /// being tracked; zero on ordinary guitar, where the short window always succeeds.
    /// <para><b>Successes, not fallbacks</b> — the summary said "fell back", which is what
    /// <see cref="LegatoLongWindowFallbacks"/> counts. Two fallback paths never reach the
    /// increment (the RMS gate, and a long window that also fails to resolve), so a reader
    /// diagnosing "is bass material being tracked at all" got a false negative out of the
    /// one counter meant to answer it.</para></summary>
    public long LegatoLongWindowReadings => _legatoLongWindowReadings;
    private long _legatoLongWindowReadings;

    /// <summary>How many legato hops FELL BACK to the long YIN window — i.e. the short
    /// window returned nothing, or returned a reading at its own lag ceiling (a clamp, not a
    /// measurement). Always at least <see cref="LegatoLongWindowReadings"/>; the difference
    /// is the hops the long window could not resolve either, or that its RMS gate
    /// dropped.</summary>
    public long LegatoLongWindowFallbacks => _legatoLongWindowFallbacks;
    private long _legatoLongWindowFallbacks;

    private readonly JuceOnsetDetector  _onset;
    private readonly JucePolyphonicPitch _poly;
    private readonly JuceChromaticTuner _yinAnalysis;
    private readonly JuceChromaticTuner _yinLegato;
    private readonly JuceChromaticTuner _yinLegatoLong;
    private readonly JuceChromagram     _chromaAnalysis;
    private readonly JuceChromagram     _chromaLegato;
    private readonly JuceFft            _featFftAnalysis;
    private readonly JuceFft            _featFftLegato;
    private readonly float[]            _hannAnalysis;
    private readonly float[]            _hannLegato;
    private readonly float[]            _featRe;
    private readonly float[]            _featIm;
    private readonly float[]            _ring;
    private readonly float[]            _winAnalysis;
    private readonly float[]            _winReference;
    private readonly float[]            _winLegato;
    private readonly float[]            _winLegatoLong;
    private readonly float[]            _winShort;
    private readonly float[]            _winFrame;
    private readonly float[]            _refSpec;
    private readonly JuceOnset[]        _onsetScratch =
        new JuceOnset[JuceOnsetDetector.RequiredOnsetCapacity(ChunkSamples)];
    private readonly int                _decayDelaySamples;
    private readonly long               _minOnsetGap;
    private readonly long               _finalizeLag;

    /// <summary>
    /// Lowest frequency the SHORT legato window can genuinely measure:
    /// <c>sampleRate / (LegatoWindow / 2)</c>, with a small margin.
    /// <para><b>The trap this guards.</b> Below its lag ceiling YIN does not fail — it
    /// returns the CLAMPED lag as a confident reading. Measured on a synthetic bass low E
    /// (MIDI 28, 41.20 Hz) at 44.1 kHz, the 2048-sample window reports <b>43.05 Hz</b> (τ
    /// pinned at maxTau = 1024) with 0.93 confidence: nearly a semitone sharp, and
    /// indistinguishable from a real F1. So the long-window fallback cannot key off "no
    /// reading"; it keys off "the reading sits at the ceiling".</para>
    /// </summary>
    private readonly float              _legatoShortFloorHz;
    private readonly long               _legatoOnsetGap;
    private readonly long               _legatoRateLimit;

    private struct Pending
    {
        public long Pos;
        public float Strength;            // the detector's, echoed by a Withdrawn notice
        public bool Published;            // a Detected notice went out, so a Withdrawn one is owed
        public JuceGuitarEvent? Deferred; // pre-built Legato event, null = onset awaiting finalization
    }

    private readonly List<Pending> _pending = new();

    private long  _writePos;
    private long  _anchor;
    private long  _legatoNext;
    private float _legatoPrevFreq;
    private int   _legatoStable;
    private float _lastEventTopFreq;
    private long  _lastOnsetDetectPos;
    private long  _lastOnsetEventPos;
    private long  _lastLegatoPos;
    private long  _lastProvisionalAttack;

    /// <summary>Whether this instance drives its three YIN tuners through the
    /// FFT difference function (<see cref="JuceChromaticTuner.UseFftDifference"/>).
    /// Off by default: the direct path is the bit-identity contract.</summary>
    public bool UseFastYin { get; }

    /// <param name="sampleRate">Sample rate of the pushed audio.</param>
    /// <param name="useFastYin">Drive the three internal
    /// <see cref="JuceChromaticTuner"/>s through the FFT difference function. The
    /// YIN difference is O(W·τmax) and is the dominant cost of the offline
    /// audio-to-chart pass; the FFT form is a large constant-factor win and agrees
    /// with the direct one to well under half a cent, but it is NOT bit-identical,
    /// so it is opt-in and the default detection path is unchanged. A constructor
    /// parameter, not an init property, because the tuners are built here.</param>
    public JuceGuitarAnalyzer(int sampleRate, bool useFastYin = false)
    {
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        SampleRate = sampleRate;
        UseFastYin = useFastYin;

        _onset          = new JuceOnsetDetector(sampleRate);
        _poly           = new JucePolyphonicPitch(sampleRate);
        _yinAnalysis    = new JuceChromaticTuner(sampleRate, AnalysisWindow, minFreq: YinMinFreqHz, useFftDifference: useFastYin);
        _yinLegato      = new JuceChromaticTuner(sampleRate, LegatoWindow, minFreq: YinMinFreqHz, useFftDifference: useFastYin);
        _yinLegatoLong  = new JuceChromaticTuner(sampleRate, LegatoYinWindow, minFreq: YinMinFreqHz, useFftDifference: useFastYin);
        _chromaAnalysis = new JuceChromagram(sampleRate, AnalysisWindow);
        _chromaLegato   = new JuceChromagram(sampleRate, LegatoWindow);
        _featFftAnalysis = new JuceFft(AnalysisWindow);
        _featFftLegato   = new JuceFft(LegatoWindow);

        _hannAnalysis = MakeHann(AnalysisWindow);
        _hannLegato   = MakeHann(LegatoWindow);
        _featRe       = new float[AnalysisWindow];
        _featIm       = new float[AnalysisWindow];
        _ring         = new float[RingSize];
        _winAnalysis  = new float[AnalysisWindow];
        _winReference = new float[AnalysisWindow];
        _winLegato    = new float[LegatoWindow];
        _winLegatoLong = new float[LegatoYinWindow];
        _winShort     = new float[ShortRmsWindow];
        _winFrame     = new float[RefineFrame];
        _refSpec      = new float[AnalysisWindow / 2 + 1];

        _decayDelaySamples = (int)(DecayDelaySeconds * sampleRate);
        _minOnsetGap       = (long)(0.045 * sampleRate);
        // Cover what FinalizeOnset reads AFTER refinement, not before it. The pending queue
        // releases on the UNREFINED position (`p.Pos + _finalizeLag <= _writePos`), but
        // RefineAttackPosition can move that position FORWARD by up to
        // `AnalysisWindow - PreOnsetLookback` (3,840 samples) — and the refined position is
        // then read a further `_decayDelaySamples + ShortRmsWindow` ahead for the decay RMS
        // (4,552) and `AnalysisWindow - PreOnsetLookback` ahead for the analysis window.
        // At the old `AnalysisWindow + _decayDelaySamples` (7,624) the worst case ran 768
        // samples past _writePos, where ReadRing CLAMPS and zero-fills: decayRms came back
        // deflated, decayRatio collapsed, and the note was silently misclassified PalmMute
        // with no counter moving.
        _finalizeLag       = (AnalysisWindow - PreOnsetLookback)
                           + Math.Max(_decayDelaySamples + ShortRmsWindow,
                                      AnalysisWindow - PreOnsetLookback);
        _legatoOnsetGap    = (long)(LegatoOnsetGapSeconds * sampleRate);
        _legatoRateLimit   = (long)(LegatoRateLimitSeconds * sampleRate);
        // ASK the tuner, never re-derive its geometry. This was
        // `LegatoShortFloorMargin * sampleRate / (LegatoWindow / 2)`, i.e. the window half
        // assumed to be what binds the lag ceiling — but JuceChromaticTuner clamps at
        // `min(windowSize/2, sampleRate/minFreq)`, so below
        // `YinMinFreqHz * LegatoWindow / 2` (30,720 Hz) it is MIN FREQ that binds. At
        // 22,050 Hz the tuner clamps at tau 735 (EffectiveMinFrequency 30.0 Hz) while the
        // hand-copied formula computed 22.6 Hz, so the clamped 30 Hz reading passed the
        // floor test below and was accepted as a MEASUREMENT — precisely the artefact this
        // floor exists to reject.
        _legatoShortFloorHz = LegatoShortFloorMargin * _yinLegato.EffectiveMinFrequency;

        Reset();
    }

    /// <summary>Reset all state and re-anchor the absolute stream position
    /// (use when the capture ring skipped samples).</summary>
    /// <remarks>Every onset still waiting to be finalized is dropped WITHOUT a notice: a
    /// caller holding provisional onsets (<see cref="JuceOnsetNotice"/>) forgets them when it
    /// resets.</remarks>
    public void Reset(ulong streamPosition = 0)
    {
        _writePos = (long)streamPosition;
        _anchor   = (long)streamPosition;
        Array.Clear(_ring);
        _onset.Reset();
        _pending.Clear();
        _legatoNext         = _anchor;
        _legatoPrevFreq     = 0f;
        _legatoStable       = 0;
        _lastEventTopFreq   = 0f;
        _lastOnsetDetectPos = long.MinValue / 2;
        _lastOnsetEventPos  = long.MinValue / 2;
        _lastLegatoPos      = long.MinValue / 2;
        _lastProvisionalAttack = long.MinValue / 2;
        // NOT the counters: NonFiniteSamples and LegatoLongWindowReadings are lifetime
        // diagnostics, and Reset is called on every capture-ring skip.
    }

    /// <summary>
    /// The analyzer's inherent REPORT delay, in samples: an event at position
    /// <c>P</c> is not emitted until the stream has reached
    /// <c>P + LatencySamples</c>, because finalizing it needs the whole
    /// 4096-sample analysis window plus the 80 ms the decay ratio measures.
    /// <para>A caller aligning events to its own input timeline needs this;
    /// <see cref="JuceOnsetDetector.LatencySamples"/> is smaller and is NOT the
    /// binding constraint here. Pushing in blocks adds the block's own
    /// quantisation on top — this is the floor.</para>
    /// </summary>
    public int LatencySamples => (int)_finalizeLag;

    /// <summary>
    /// Lifetime count of events dropped because the caller's destination span in
    /// <see cref="Push(ReadOnlySpan{float}, Span{JuceGuitarEvent}, out int)"/>
    /// was full. A health counter — <see cref="Reset"/> leaves it alone.
    /// </summary>
    public long DroppedEvents { get; private set; }

    /// <summary>
    /// The largest number of events a single push of
    /// <paramref name="blockSamples"/> samples can finalize: the onset detector
    /// cannot fire more often than one per hop, and the legato tracker no more
    /// often than one per legato hop.
    /// </summary>
    public static int RequiredEventCapacity(int blockSamples) =>
        blockSamples <= 0 ? 0 : blockSamples / JuceOnsetDetector.HopSize + blockSamples / LegatoHop + 4;

    /// <summary>
    /// Span form of the list-returning <c>Push</c>: events are written
    /// into <paramref name="outEvents"/> instead of a freshly allocated list.
    /// Detection is bit-identical — it is the same code path with a different
    /// sink.
    /// <para><b>This is not allocation-free when events are emitted</b>, and it
    /// cannot be: <see cref="JuceGuitarEvent"/> carries a
    /// <c>JucePitchCandidate[]</c> and a 12-bin chroma array. What it removes is
    /// the per-call <c>List</c> on the overwhelmingly common path where a block
    /// finalizes nothing — pinned by
    /// <c>JuceGuitarAnalyzerSpanPushTests.SpanPush_AllocatesNothingWhenNoEventsAreEmitted</c>.</para>
    /// </summary>
    /// <param name="samples">Mono block; any length.</param>
    /// <param name="outEvents">Destination; size it with
    /// <see cref="RequiredEventCapacity"/>. Overflow is DROPPED and counted in
    /// <see cref="DroppedEvents"/>, never thrown — a throw halfway through would
    /// leave the stream position past audio whose events were never reported.</param>
    /// <param name="count">Entries written.</param>
    public void Push(ReadOnlySpan<float> samples, Span<JuceGuitarEvent> outEvents, out int count)
    {
        if (samples.IsEmpty) { count = 0; return; }
        var sink = new EventSink(outEvents);
        PushCore(samples, ref sink, null);
        count = sink.Count;
        DroppedEvents += sink.Dropped;
    }

    // One sink so the two overloads cannot drift.
    private ref struct EventSink
    {
        public List<JuceGuitarEvent>? List;
        private readonly Span<JuceGuitarEvent> _buffer;
        private readonly bool _spanMode;
        public int  Count;
        public long Dropped;

        public EventSink(Span<JuceGuitarEvent> buffer) { _buffer = buffer; _spanMode = true; List = null; }
        public EventSink(List<JuceGuitarEvent>? list)  { _buffer = default; _spanMode = false; List = list; }

        public void Add(JuceGuitarEvent ev)
        {
            if (!_spanMode) { (List ??= new()).Add(ev); Count++; return; }
            if (Count >= _buffer.Length) { Dropped++; return; }
            _buffer[Count++] = ev;
        }
    }

    /// <summary>Push a block of mono samples; returns the events finalized
    /// by this block in stream order (may be empty). Event positions are
    /// absolute (anchor + samples pushed).</summary>
    public IReadOnlyList<JuceGuitarEvent> Push(ReadOnlySpan<float> samples)
    {
        if (samples.IsEmpty) return Array.Empty<JuceGuitarEvent>();
        var sink = new EventSink((List<JuceGuitarEvent>?)null);
        PushCore(samples, ref sink, null);
        return (IReadOnlyList<JuceGuitarEvent>?)sink.List ?? Array.Empty<JuceGuitarEvent>();
    }

    /// <summary>
    /// <see cref="Push(ReadOnlySpan{float})"/>, also reporting each detector onset the moment
    /// it fires: a <see cref="JuceOnsetNoticeKind.Detected"/> notice
    /// <see cref="LatencySamples"/> before the event that finalizes it, and a
    /// <see cref="JuceOnsetNoticeKind.Withdrawn"/> notice for one that produces no event (see
    /// <see cref="JuceOnsetNotice"/>). The events are bit-identical to the other overloads'.
    /// </summary>
    /// <param name="samples">Mono block; any length.</param>
    /// <param name="notices">Caller-owned; notices are APPENDED in stream order (it is not
    /// cleared). Reuse one list and nothing is allocated for them after warm-up.</param>
    /// <returns>The events finalized by this block, in stream order (may be empty).</returns>
    public IReadOnlyList<JuceGuitarEvent> Push(ReadOnlySpan<float> samples, List<JuceOnsetNotice> notices)
    {
        ArgumentNullException.ThrowIfNull(notices);
        if (samples.IsEmpty) return Array.Empty<JuceGuitarEvent>();
        var sink = new EventSink((List<JuceGuitarEvent>?)null);
        PushCore(samples, ref sink, notices);
        return (IReadOnlyList<JuceGuitarEvent>?)sink.List ?? Array.Empty<JuceGuitarEvent>();
    }

    private void PushCore(ReadOnlySpan<float> samples, ref EventSink emitted, List<JuceOnsetNotice>? notices)
    {
        int offset = 0;
        while (offset < samples.Length)
        {
            int n = Math.Min(ChunkSamples, samples.Length - offset);
            var chunk = samples.Slice(offset, n);
            offset += n;

            WriteToRing(chunk);

            // The span overload, not the list one: boxing the IReadOnlyList's
            // enumerator allocated on EVERY chunk, including the empty ones.
            // _onsetScratch is sized by RequiredOnsetCapacity(ChunkSamples), so
            // JuceOnsetDetector.DroppedOnsets cannot move here.
            _onset.Push(chunk, _onsetScratch, out var onsetCount);
            for (int i = 0; i < onsetCount; i++)
            {
                long pos = _anchor + _onsetScratch[i].SamplePosition;
                float strength = _onsetScratch[i].Strength;
                _lastOnsetDetectPos = pos;
                // Provisional: published now, LatencySamples before FinalizeOnset can run, snapped
                // onto the attack with what the ring holds so far (the chunk is already in it) —
                // and only when that audio shows a fresh attack (ShowsFreshAttack).
                bool published = false;
                if (notices is not null)
                {
                    long attack = RefineAttackPosition(pos, Math.Min(pos + AnalysisWindow - PreOnsetLookback,
                                                                     _writePos - RefineFrame));
                    // Several detector hits snap onto one attack (FinalizeOnset dedupes them the
                    // same way): publish the attack once.
                    if (attack - _lastProvisionalAttack >= _minOnsetGap && ShowsFreshAttack(attack))
                    {
                        _lastProvisionalAttack = attack;
                        notices.Add(new JuceOnsetNotice(JuceOnsetNoticeKind.Detected, (ulong)pos, (ulong)attack,
                                                        (double)attack / SampleRate, strength));
                        published = true;
                    }
                }
                AddPending(new Pending { Pos = pos, Strength = strength, Published = published, Deferred = null });
            }

            while (_legatoNext + LegatoWindow <= _writePos)
            {
                ProcessLegatoHop(_legatoNext);
                _legatoNext += LegatoHop;
            }

            while (_pending.Count > 0 && _pending[0].Pos + _finalizeLag <= _writePos)
            {
                var p = _pending[0];
                _pending.RemoveAt(0);
                if (p.Deferred is { } prebuilt)
                {
                    emitted.Add(prebuilt);
                }
                else if (FinalizeOnset(p.Pos) is { } ev)
                {
                    emitted.Add(ev with { DetectorSamplePosition = (ulong)p.Pos });
                }
                else if (p.Published)
                {
                    notices?.Add(new JuceOnsetNotice(JuceOnsetNoticeKind.Withdrawn, (ulong)p.Pos, (ulong)p.Pos,
                                                     (double)p.Pos / SampleRate, p.Strength));
                }
            }
        }
    }

    // ---------------------------------------------------------------- ring

    private void WriteToRing(ReadOnlySpan<float> samples)
    {
        int src = 0;
        while (src < samples.Length)
        {
            int idx  = (int)((ulong)_writePos % RingSize);
            int copy = Math.Min(RingSize - idx, samples.Length - src);
            var run  = samples.Slice(src, copy);
            var dest = _ring.AsSpan(idx, copy);
            // Scrub non-finite input (see NonFiniteSamples) instead of a bulk copy. One NaN
            // sample otherwise reaches MidiFromFrequency -> (int)MathF.Round(NaN) ->
            // int.MinValue, and slips through every `rms < threshold` gate on the way, because
            // a comparison against NaN is FALSE.
            for (int i = 0; i < copy; i++)
            {
                float s = run[i];
                if (float.IsFinite(s)) { dest[i] = s; }
                else { dest[i] = 0f; _nonFiniteSamples++; }
            }
            _writePos += copy;
            src       += copy;
        }
    }

    /// <summary>Read <paramref name="dest"/>.Length samples starting at the
    /// absolute position <paramref name="start"/>. Regions outside the valid
    /// range (before the anchor / already overwritten / not yet written)
    /// are zero-filled.</summary>
    private void ReadRing(long start, Span<float> dest)
    {
        dest.Clear();
        long earliest = Math.Max(_anchor, _writePos - RingSize);
        long from = Math.Max(start, earliest);
        long to   = Math.Min(start + dest.Length, _writePos);
        while (from < to)
        {
            int idx  = (int)((ulong)from % RingSize);
            int copy = (int)Math.Min(RingSize - idx, to - from);
            _ring.AsSpan(idx, copy).CopyTo(dest.Slice((int)(from - start)));
            from += copy;
        }
    }

    private void AddPending(Pending p)
    {
        int i = _pending.Count;
        while (i > 0 && _pending[i - 1].Pos > p.Pos) i--;
        _pending.Insert(i, p);
    }

    // ------------------------------------------------------------- onsets

    /// <summary>
    /// Whether the audio heard so far shows a FRESH attack at <paramref name="attack"/>: the RMS
    /// from it to the end of the stream (at most <see cref="ShortRmsWindow"/>, at least
    /// <see cref="ProvisionalMinPostSamples"/>) over the RMS of the <see cref="ShortRmsWindow"/>
    /// before it reaches <see cref="AttackRatioMin"/> — the ratio <c>FinalizeOnset</c> demands of
    /// a picked attack, over the windows it can already read.
    /// <para><b>Why a provisional onset needs a gate the finalized one gets later.</b> The
    /// 1024-point flux detector also fires every ~50 ms on the window-leakage ripple of a ringing
    /// low note; the novelty gate withdraws those ~190 ms later, but a provisional onset is read
    /// at once, so each would time a cue from the ripple (measured: ~10 per 0.5 s E2 pluck). A
    /// short-frame rise cannot tell them apart — a 256-sample frame is under half a period of E2,
    /// so its RMS swings with phase — but a 1024-sample window spans whole periods. An onset not
    /// published still finalizes as usual; its event's
    /// <see cref="JuceGuitarEvent.DetectorSamplePosition"/> then names no notice.</para>
    /// </summary>
    private bool ShowsFreshAttack(long attack)
    {
        int post = (int)Math.Min(ShortRmsWindow, _writePos - attack);
        if (post < ProvisionalMinPostSamples) return false;
        float postRms = RmsOver(attack, post);
        float preRms  = RmsOver(attack - ShortRmsWindow, ShortRmsWindow);
        return postRms / MathF.Max(preRms, 1e-6f) >= AttackRatioMin;
    }

    private float RmsOver(long start, int length)
    {
        var win = _winShort.AsSpan(0, length);
        ReadRing(start, win);
        float sum = 0f;
        for (int i = 0; i < win.Length; i++) sum += win[i] * win[i];
        return MathF.Sqrt(sum / win.Length);
    }

    private JuceGuitarEvent? FinalizeOnset(long pos)
    {
        // 1. Novelty gate. The 1024-point flux detector fires spuriously on
        // window-leakage ripple of low notes; a smoothly decaying signal has
        // (almost) no rectified spectral gain over the pre-onset reference,
        // so such onsets are rejected before any state is touched.
        ReadRing(Math.Max(pos - PreOnsetLookback, _anchor), _winAnalysis);
        ReadRing(pos - PreOnsetLookback - AnalysisWindow, _winReference);
        if (SpectralNovelty(_winAnalysis, _winReference) < NoveltyMin) return null;

        // 2. Snap the detector position to the strongest local RMS rise so
        // the analysis / reference / attack windows straddle the true attack
        // even when the detector anchored a hop early or late.
        pos = RefineAttackPosition(pos, pos + AnalysisWindow - PreOnsetLookback);

        // 3. Dedupe: several detector hits can refine to the same attack.
        if (pos - _lastOnsetEventPos < _minOnsetGap) return null;

        long winStart = Math.Max(pos - PreOnsetLookback, _anchor);
        ReadRing(winStart, _winAnalysis);

        // Reference window: the samples ending just before the onset.
        // Ring-out of previously played strings appears in both windows and
        // cancels in the differenced spectrum; near stream start / after
        // Reset the missing leading part is zero-padded by ReadRing, which
        // degrades gracefully to plain single-window detection.
        ReadRing(pos - PreOnsetLookback - AnalysisWindow, _winReference);

        // Pitch content: onset-synchronous polyphonic estimate (only energy
        // NEW relative to the reference) cross-checked with YIN.
        var candidates = new List<JucePitchCandidate>();
        foreach (var c in _poly.Detect(_winAnalysis, _winReference))
            if (c.Salience >= MinCandidateSalience) candidates.Add(c);

        if (_yinAnalysis.Detect(_winAnalysis) is { } yin)
        {
            int   best     = -1;
            float bestDiff = YinSnapCents;
            for (int i = 0; i < candidates.Count; i++)
            {
                float cents = MathF.Abs(CentsBetween(yin.FrequencyHz, candidates[i].FrequencyHz));
                if (cents < bestDiff) { bestDiff = cents; best = i; }
            }
            if (best >= 0)
            {
                candidates[best] = candidates[best] with
                {
                    FrequencyHz = yin.FrequencyHz,
                    Midi        = MidiFromFrequency(yin.FrequencyHz),
                };
            }
            else
            {
                var added = new JucePitchCandidate(
                    MidiFromFrequency(yin.FrequencyHz), yin.FrequencyHz, YinAddedSalience);
                int i = candidates.Count;
                while (i > 0 && candidates[i - 1].Salience < added.Salience) i--;
                candidates.Insert(i, added);
            }
        }
        var pitches = candidates.ToArray();

        // Features over the analysis window.
        var chroma = _chromaAnalysis.Compute(_winAnalysis);
        (float rms, float crest) = RmsAndCrest(_winAnalysis);
        (float centroid, float flatness) =
            SpectralFeatures(_featFftAnalysis, _hannAnalysis, _winAnalysis);

        // Short-window RMS around the attack.
        float attackRms = ShortRms(pos + PreOnsetLookback);
        float decayRms  = ShortRms(pos + _decayDelaySamples);
        float preRms    = ShortRms(pos - PreOnsetLookback - ShortRmsWindow);
        float decayRatio  = Math.Clamp(decayRms / MathF.Max(attackRms, 1e-9f), 0f, 4f);
        float attackRatio = attackRms / MathF.Max(preRms, 1e-6f);

        // Noise gate: an attack that never reaches real playing level is
        // background static / hiss, not a note — drop it before any of the
        // relative measures (novelty, attack ratio) can be fooled by a
        // quiet-over-quieter transient.
        if (attackRms < MinAttackRms) return null;

        float topFreq = pitches.Length > 0 ? pitches[0].FrequencyHz : 0f;

        if (attackRatio < AttackRatioMin)
        {
            // No fresh attack behind this onset: either a legato transition
            // (pitch moved) or spectral jitter (drop it silently). Fade-outs
            // and release noise are rejected by the level gate.
            if (attackRms >= LegatoMinRms &&
                topFreq > 0f && _lastEventTopFreq > 0f &&
                MathF.Abs(CentsBetween(topFreq, _lastEventTopFreq)) >= LegatoTriggerCents &&
                pos - _lastLegatoPos >= _legatoRateLimit)
            {
                _lastEventTopFreq  = topFreq;
                _lastLegatoPos     = pos;
                _lastOnsetEventPos = pos;
                return new JuceGuitarEvent(
                    JuceGuitarEventKind.Legato, (double)pos / SampleRate, (ulong)pos,
                    pitches, chroma, centroid, rms, crest, flatness,
                    DecayRatio: 1f, Techniques: JuceGuitarTechniques.Legato);
            }
            return null;
        }

        var tech = JuceGuitarTechniques.None;
        if (decayRatio < PalmMuteMaxDecay && centroid < PalmMuteMaxCentroid)
            tech |= JuceGuitarTechniques.PalmMute;

        bool percussive = pitches.Length == 0 || flatness > PercussiveFlatness;
        if (percussive)
        {
            tech |= JuceGuitarTechniques.Percussive;
            if (centroid < SlapMaxCentroid && crest >= SlapPopMinCrest)
                tech |= JuceGuitarTechniques.Slap;
            else if (centroid >= PopMinCentroid && crest >= SlapPopMinCrest)
                tech |= JuceGuitarTechniques.Pop;
        }
        else if (topFreq >= HarmonicMinPitchHz && pitches.Length <= HarmonicMaxCount &&
                 centroid < HarmonicCentroidMul * topFreq)
        {
            tech |= JuceGuitarTechniques.Harmonic;
            if (topFreq >= PinchMinPitchHz)
                tech |= JuceGuitarTechniques.PinchHarmonic;
        }

        _lastEventTopFreq  = topFreq;
        _lastOnsetEventPos = pos;
        return new JuceGuitarEvent(
            JuceGuitarEventKind.Onset, (double)pos / SampleRate, (ulong)pos,
            pitches, chroma, centroid, rms, crest, flatness, decayRatio, tech);
    }

    /// <summary>Energy of the half-wave-rectified magnitude difference
    /// between <paramref name="window"/> and <paramref name="reference"/>,
    /// relative to the reference energy (bins up to 5 kHz). ~0 for a
    /// smoothly decaying signal, large when genuinely new energy appears.</summary>
    private float SpectralNovelty(ReadOnlySpan<float> window, ReadOnlySpan<float> reference)
    {
        int n      = AnalysisWindow;
        int maxBin = Math.Min(n / 2 - 1, (int)(FeatureMaxFreqHz * n / SampleRate));
        var re     = _featRe.AsSpan(0, n);
        var im     = _featIm.AsSpan(0, n);

        for (int i = 0; i < n; i++) re[i] = reference[i] * _hannAnalysis[i];
        im.Clear();
        _featFftAnalysis.Forward(re, im);
        for (int k = 1; k <= maxBin; k++)
            _refSpec[k] = MathF.Sqrt(re[k] * re[k] + im[k] * im[k]);

        for (int i = 0; i < n; i++) re[i] = window[i] * _hannAnalysis[i];
        im.Clear();
        _featFftAnalysis.Forward(re, im);

        double diffEnergy = 0, refEnergy = 0;
        for (int k = 1; k <= maxBin; k++)
        {
            float mag = MathF.Sqrt(re[k] * re[k] + im[k] * im[k]);
            float d   = mag - _refSpec[k];
            if (d > 0f) diffEnergy += (double)d * d;
            refEnergy += (double)_refSpec[k] * _refSpec[k];
        }
        return (float)(diffEnergy / (refEnergy + 1e-9));
    }

    /// <summary>Snap a detected onset to the true attack: scan short-frame
    /// RMS rises from <see cref="RefineBack"/> samples before the detector
    /// position to <paramref name="to"/> (the end of the analysis lookahead when
    /// finalizing; the end of the stream for a provisional onset), then return the
    /// earliest frame whose rise reaches <see cref="RefineRiseFraction"/> of
    /// the best (never less than <see cref="RefineMinRise"/>). Falls back to
    /// the original position when nothing rises.</summary>
    private long RefineAttackPosition(long pos, long to)
    {
        long from = pos - RefineBack;

        float bestRise = 0f;
        float prev     = -1f;
        for (long s = from - RefineFrame; s <= to; s += RefineFrame)
        {
            if (s < _anchor) { prev = -1f; continue; }
            float rms = FrameRms(s);
            if (prev >= 0f && s >= from)
            {
                float rise = (rms + 1e-6f) / (prev + 1e-6f);
                if (rise > bestRise) bestRise = rise;
            }
            prev = rms;
        }
        if (bestRise < RefineMinRise) return pos;

        float threshold = MathF.Max(RefineMinRise, RefineRiseFraction * bestRise);
        prev = -1f;
        for (long s = from - RefineFrame; s <= to; s += RefineFrame)
        {
            if (s < _anchor) { prev = -1f; continue; }
            float rms = FrameRms(s);
            if (prev >= 0f && s >= from)
            {
                float rise = (rms + 1e-6f) / (prev + 1e-6f);
                if (rise >= threshold) return s;
            }
            prev = rms;
        }
        return pos;
    }

    private float FrameRms(long start)
    {
        ReadRing(start, _winFrame);
        float sum = 0f;
        for (int i = 0; i < _winFrame.Length; i++) sum += _winFrame[i] * _winFrame[i];
        return MathF.Sqrt(sum / _winFrame.Length);
    }

    // ------------------------------------------------------------- legato

    private void ProcessLegatoHop(long windowStart)
    {
        ReadRing(windowStart, _winLegato);
        var reading = _yinLegato.Detect(_winLegato);
        // A reading AT the short window's lag ceiling is a clamp, not a measurement (see
        // _legatoShortFloorHz), and a pitch further down produces no reading at all. Both
        // mean the same thing: this window cannot resolve the period. Retry long.
        if (reading is null || reading.Value.FrequencyHz < _legatoShortFloorHz)
        {
            _legatoLongWindowFallbacks++;
            reading = DetectLongWindow(windowStart);
        }
        if (reading is null)
        {
            _legatoStable   = 0;
            _legatoPrevFreq = 0f;
            return;
        }

        float freq = reading.Value.FrequencyHz;
        if (_legatoPrevFreq > 0f &&
            MathF.Abs(CentsBetween(freq, _legatoPrevFreq)) <= LegatoStableCents)
            _legatoStable++;
        else
            _legatoStable = 1;
        _legatoPrevFreq = freq;

        if (_legatoStable < 2) return;

        // Suppress while an onset is being finalized or fired recently —
        // the onset path owns those transitions.
        for (int i = 0; i < _pending.Count; i++)
            if (_pending[i].Deferred is null) return;
        long windowEnd = windowStart + LegatoWindow;
        if (windowEnd - _lastOnsetDetectPos < _legatoOnsetGap) return;

        if (_lastEventTopFreq <= 0f) return;
        if (MathF.Abs(CentsBetween(freq, _lastEventTopFreq)) < LegatoTriggerCents) return;

        long pos = windowStart + LegatoWindow / 2;
        if (pos - _lastLegatoPos < _legatoRateLimit) return;

        (float rms, float crest) = RmsAndCrest(_winLegato);
        if (rms < LegatoMinRms) return; // decay tails drift in pitch; ignore them

        var chroma = _chromaLegato.Compute(_winLegato);
        (float centroid, float flatness) =
            SpectralFeatures(_featFftLegato, _hannLegato, _winLegato);

        var ev = new JuceGuitarEvent(
            JuceGuitarEventKind.Legato, (double)pos / SampleRate, (ulong)pos,
            new[] { new JucePitchCandidate(MidiFromFrequency(freq), freq, 1f) },
            chroma, centroid, rms, crest, flatness,
            DecayRatio: 1f, Techniques: JuceGuitarTechniques.Legato);

        AddPending(new Pending { Pos = pos, Deferred = ev });
        _lastLegatoPos    = pos;
        _lastEventTopFreq = freq;
    }

    /// <summary>
    /// Second attempt on a window twice as long, ending at the SAME sample as the short one,
    /// for the pitches <see cref="LegatoWindow"/> physically cannot resolve (below ~43 Hz at
    /// 44.1 kHz — see <see cref="LegatoYinWindow"/>). Extending backwards rather than forwards
    /// keeps the emit cadence, the event positions and the analysis "now" edge exactly where
    /// they were: only the pitch estimate integrates longer, which is appropriate for a
    /// fundamental whose period is 26 ms in the first place.
    /// </summary>
    private JuceTunerReading? DetectLongWindow(long windowStart)
    {
        // Guard the cost: the long YIN is ~3.5x the short one, so it only runs where an
        // event could actually be emitted (the gate ProcessLegatoHop applies below anyway).
        if (RmsAndCrest(_winLegato).Rms < LegatoMinRms) return null;
        ReadRing(windowStart + LegatoWindow - LegatoYinWindow, _winLegatoLong);
        var reading = _yinLegatoLong.Detect(_winLegatoLong);
        if (reading is not null) _legatoLongWindowReadings++;
        return reading;
    }

    // ----------------------------------------------------------- features

    private float ShortRms(long start)
    {
        ReadRing(start, _winShort);
        float sum = 0f;
        for (int i = 0; i < _winShort.Length; i++) sum += _winShort[i] * _winShort[i];
        return MathF.Sqrt(sum / _winShort.Length);
    }

    private static (float Rms, float Crest) RmsAndCrest(ReadOnlySpan<float> window)
    {
        float sum = 0f, peak = 0f;
        for (int i = 0; i < window.Length; i++)
        {
            float v = window[i];
            sum += v * v;
            float a = MathF.Abs(v);
            if (a > peak) peak = a;
        }
        float rms = MathF.Sqrt(sum / window.Length);
        return (rms, peak / MathF.Max(rms, 1e-9f));
    }

    private (float CentroidHz, float Flatness) SpectralFeatures(
        JuceFft fft, float[] hann, ReadOnlySpan<float> window)
    {
        int n  = fft.Size;
        var re = _featRe.AsSpan(0, n);
        var im = _featIm.AsSpan(0, n);
        for (int i = 0; i < n; i++) re[i] = window[i] * hann[i];
        im.Clear();
        fft.Forward(re, im);

        int maxBin = Math.Min(n / 2 - 1, (int)(FeatureMaxFreqHz * n / SampleRate));
        double sumMag = 0, sumFreqMag = 0, sumLog = 0;
        int bins = 0;
        for (int k = 1; k <= maxBin; k++)
        {
            double mag  = Math.Sqrt((double)re[k] * re[k] + (double)im[k] * im[k]);
            double freq = (double)k * SampleRate / n;
            sumMag     += mag;
            sumFreqMag += freq * mag;
            sumLog     += Math.Log(mag + 1e-10);
            bins++;
        }
        if (bins == 0 || sumMag <= 0) return (0f, 0f);
        float centroid = (float)(sumFreqMag / sumMag);
        float flatness = (float)(Math.Exp(sumLog / bins) / (sumMag / bins + 1e-10));
        return (centroid, flatness);
    }

    private static float CentsBetween(float a, float b) => PitchMath.CentsF(a, b);

    private static int MidiFromFrequency(float freq)
        => (int)MathF.Round(PitchMath.HzToMidiF(freq));

    // One Hann implementation for the assembly. `0.5f - 0.5f*c` and
    // `0.5f*(1f - c)` are BIT-identical in IEEE float — halving is exact for a
    // power of two, so the rounding of the subtraction is scale-invariant — which
    // is why THE HANN REFACTOR, and nothing else, moves no analyzer or CQT
    // number.
    //
    // Read that sentence narrowly. It is a claim about collapsing seven copies of
    // one formula into `Windows.Hann`, and it was adjacent to a change that DID
    // move a number: `JuceChromagram.ApplyHannWindow` defaulting to true (D5)
    // windows every chroma frame, and `_chromaAnalysis` / `_chromaLegato` above
    // take that default — so the `Chroma` vector of every JuceGuitarEvent this
    // analyzer emits changed with it. That was deliberate (an unwindowed frame
    // boundary sprays a partial across every pitch class) and it is KEPT; it is
    // pinned by Wave2ChromagramBaselineTests rather than by this comment. The
    // two sat side by side and read as one global "nothing moved".
    private static float[] MakeHann(int size) => Windows.Hann(size);
}
