namespace JuceSharp.Dsp;

/// <summary>
/// Streaming quarter-tone Constant-Q Transform — the log-frequency front end
/// for the chart verifier (<c>JuceChartVerifier</c>). Log-spaced bins
/// keep adjacent notes separable even on the low strings (where a linear FFT
/// smears them); at 24 bins/octave a wrong fret sits two bins from the target,
/// giving a sharp peak test.
///
/// <para>Ported from the NoteDetection testbed (validated on GuitarSet + IDMT,
/// see <c>PORTING.md</c>). Implemented directly via centered complex kernels
/// (one per bin, length set by the constant Q so low bins integrate longer)
/// rather than an FFT, so quarter-tone resolution holds down to E2.</para>
///
/// <para>Two entry points share the same read surface (<see cref="Magnitude"/>,
/// <see cref="Floor"/>, <see cref="BinForMidi"/>): <see cref="AnalyzeWhole"/>
/// computes the full <c>C[bin, frame]</c> matrix for a clip (tests + the
/// offline regression harness), while <see cref="Reset"/> + <see cref="Push"/>
/// keep a rolling buffer of recent frames for live play. Per-bin floors gate
/// "is this pitch meaningfully loud": a 95th-percentile floor offline, a slow
/// rolling max in streaming.</para>
/// </summary>
/// <remarks>
/// <para><b>Thread-safety:</b> instances are <b>not</b> thread-safe, and the natural
/// live-play split — audio thread <see cref="Push"/>, game thread <see cref="Floor"/> /
/// <see cref="NoiseFloorEnergy"/> — is exactly what breaks. The getter-shaped read surface
/// mutates INSTANCE scratch: <see cref="NoiseFloorEnergy"/> fills and sorts one shared
/// percentile buffer, and both it and <see cref="Floor"/> memoise through
/// <c>_floorCache</c> / <c>_floorStamp</c> stamped on the newest ring frame. Two concurrent
/// readers interleaving in the fill-then-sort yield a torn percentile; a reader racing
/// <see cref="Push"/> reads a ring being written. Drive one instance from ONE thread, or
/// hand the reader a snapshot. Making it genuinely concurrent is impractical given the
/// memoisation — its three siblings (<c>JuceChromagram</c>, <c>JucePolyphonicPitch</c>,
/// <c>JuceSpectrogramBuilder</c>) all carry this same statement, and its absence here was
/// the only reason the contract was in doubt.</para>
/// </remarks>
public sealed class JuceConstantQ
{
    /// <summary>Sample rate in Hz the kernels were designed for. Every frame index,
    /// floor and time conversion on this instance is relative to it.</summary>
    public int    SampleRate    { get; }

    /// <summary>How many times a <see cref="Push"/> arrived at a non-contiguous
    /// position and therefore threw away every buffered frame. Zero in steady state.
    /// A caller seeing this climb once per push is violating Push's contract (that is
    /// what a speed-scaled position does) and will never get a single frame out.</summary>
    public long DiscontinuityResets => _discontinuityResets;
    private long _discontinuityResets;

    /// <summary>
    /// How many non-finite (NaN / ±Inf) input samples have been scrubbed to 0 by
    /// <see cref="Push"/>. Zero for any sane source; a climbing counter means the audio
    /// reaching detection is corrupt (decoder glitch, a divide-by-zero upstream, a bad WAV).
    /// </summary>
    /// <remarks>
    /// The scrub itself is not optional and mirrors the native rule (<c>DspChain::process</c>
    /// scrubs at the input and after every stage). Without it ONE bad sample opens the
    /// verifier's gates rather than closing them, because <c>NaN &lt;= x</c> is FALSE: it
    /// passes the per-bin floor gate, passes the anti-phantom presence gate, and then persists
    /// — in <c>_raw</c> for the widest kernel, in the frame ring for 384 frames, and in the
    /// noise history for the full <see cref="NoiseHistoryFrames"/> horizon (10 s live), where
    /// a NaN percentile leaves the presence gate wide open for that whole time. Silently, with
    /// no log and no exception. Hence also this counter: the condition must be observable.
    /// </remarks>
    public long NonFiniteSamples => _nonFiniteSamples;
    private long _nonFiniteSamples;
    /// <summary>Advance in samples between successive CQT frames, and therefore the
    /// transform's time resolution (256 samples is ~5.8 ms at 44.1 kHz).</summary>
    public int    Hop           { get; }

    /// <summary>Log-frequency resolution: 24 means quarter-tone bins, so a wrong fret
    /// sits two bins from the target and the peak test stays sharp. Always a multiple
    /// of 12.</summary>
    public int    BinsPerOctave { get; }

    /// <summary>Number of bins at or above <see cref="FMinMidi"/>; 120 at quarter-tone
    /// resolution covers the five octaves E2..E7.</summary>
    public int    BinCount      { get; }

    /// <summary>MIDI note number of bin 0 — 40 (E2) for a standard-tuned low E.</summary>
    public int    FMinMidi      { get; }

    /// <summary>Centre frequency in Hz of bin 0, derived from
    /// <see cref="FMinMidi"/>.</summary>
    public float  FMinHz        { get; }

    /// <summary>How far below a bin's own floor a magnitude may sit and still count as
    /// present. Shipped from <c>LiveCqtConfig.RelFloor</c> — never re-type it, that is
    /// exactly the drift that made an eval report a pre-veto number as current.</summary>
    public float  RelFloor      { get; }
    /// <summary>CQT bins per semitone (2 at quarter-tone resolution).</summary>
    public int    Step          { get; }

    private readonly float[][] _kernelRe;  // [bin][n]
    private readonly float[][] _kernelIm;
    private readonly int[]     _halfLen;   // (L-1)/2 per bin
    private readonly int       _maxHalf;

    // Low temporal percentile of broadband frame energy = the adaptive noise
    // floor (minimum statistics): the total energy when nothing is played.
    // The VALUE lives in LiveCqtConfig with every other shipping constant: a
    // number re-typed here is a number the eval harness can measure differently
    // from what ships, which is exactly what happened to the noise horizon and
    // the sub-octave veto.
    private const float NoisePercentile = LiveCqtConfig.NoisePercentile;

    // Offline state (AnalyzeWhole).
    private float[][]? _matrix;            // [bin][frame]
    private float[]?   _floorOffline;      // [bin]
    private float[]?   _energy;            // [frame] broadband energy (Σ bins)
    private float      _noiseEnergyOffline;
    private int        _frameCount;

    // Streaming state (Reset/Push).
    private const int EmitChunk = 8192;     // max samples appended before emitting ready frames
    private bool     _streaming;
    private float[]  _raw = System.Array.Empty<float>();
    private int      _rawSize;
    private long     _writePos;             // absolute stream position of the next sample
    private long     _startPos;             // absolute position of this session's first sample
    private long     _nextFrame;            // next ABSOLUTE frame index to emit (center = frame·hop)
    private float[][] _ring = System.Array.Empty<float[]>(); // [ringFrame][bin]
    private float[]  _ringEnergy = System.Array.Empty<float>(); // [ringFrame] Σ bins
    private int      _ringFrames;
    private long     _ringNewest = -1;      // absolute frame index of newest stored
    private long     _ringOldest;

    // Long-horizon broadband energy history for the noise floor (NoiseHistoryFrames > 0).
    private float[]  _noiseHist = System.Array.Empty<float>();
    // Persistent sort scratch for NoiseFloorEnergy's percentile. Sized at Reset
    // for the widest horizon that method can be asked for, so the live path does
    // not allocate one array per emitted frame.
    private float[]  _noiseScratch = System.Array.Empty<float>();
    private long     _noiseHistNewest = -1;
    private long     _startFrame;           // first frame this session could emit

    // Idle floor (IdleSteadyFrames > 0): see NoiseFloorEnergy.
    private float[]  _idleWin = System.Array.Empty<float>();
    private float[]  _idleScratch = System.Array.Empty<float>();
    private int      _idleCount, _idlePos, _idleSinceCheck;
    private float    _idleFloor = -1f;      // < 0 = nothing learned yet
    private bool     _idleSteady;
    private readonly float _idleRisePerFrame;

    // Lazy memos so a percentile is sorted once per emitted frame, not once per Verify
    // (Verify queries Floor/NoiseFloorEnergy repeatedly, and the ring only moves on Push).
    private float[]  _floorCache = System.Array.Empty<float>();
    private long[]   _floorStamp = System.Array.Empty<long>();
    private float    _noiseCache;
    private long     _noiseStamp = long.MinValue;

    /// <summary>Longest kernel actually used (samples). Lower = less streaming
    /// latency at the cost of low-string quarter-tone resolution.</summary>
    public int MaxKernelSamples { get; }

    /// <summary>
    /// Percentile of the resident frames used for the STREAMING per-bin floor.
    /// <c>1.0</c> (the default) is a rolling <b>MAX</b>.
    ///
    /// <para><b>MEASURED AND REJECTED — do not re-enable without new evidence.</b> The
    /// theory was sound: a rolling MAX means one loud transient pins that bin's gate at 10%
    /// of the transient for the next 2.23 s, so a quieter note struck in its wake should be
    /// gated out; a percentile (0.95, mirroring the offline statistic) is robust to that
    /// single outlier. It does not show up on IDMT. Measured on the STREAMING path
    /// (<c>CqtStreamingEvalTests</c>, cap=120, 120 tracks / 893 notes), floorPct 1.0 → 0.95:</para>
    /// <list type="bullet">
    ///   <item><description>per-note   89.5% → 89.5%  (no change)</description></item>
    ///   <item><description>per-event  88.0% → 88.0%  (no change)</description></item>
    ///   <item><description>wrong-fret FA 3.2% → 3.2% (no change)</description></item>
    ///   <item><description>tritone FA    6.6% → <b>7.2%</b> (WORSE)</description></item>
    /// </list>
    /// <para>So on this material the per-bin floor is simply not the binding constraint —
    /// <c>IsPeak</c> and the presence gate decide almost every verdict — and loosening the
    /// floor only buys false-accepts. It may still be real on material with far more dynamic
    /// contrast than IDMT has (which is mostly uniform picking); if you retry it, retry it
    /// against a dataset that HAS quiet-note-after-loud-transient cases, and report the cap.</para>
    /// <para>Offline (<see cref="AnalyzeWhole"/>) always uses the 95th percentile of the
    /// whole clip and ignores this.</para>
    /// </summary>
    public float FloorPercentile { get; }

    /// <summary>
    /// Horizon (in frames) for the streaming broadband noise floor. <c>0</c> = use the
    /// frame ring itself (384 frames ≈ 2.23 s).
    /// <para>2.23 s is too short: during continuous playing its 5th percentile is not the
    /// idle noise floor, it is "the quietest moment while playing", so the presence gate
    /// ends up demanding <c>peak &gt; 3 × near-playing-level</c> — which a sustained legato
    /// line or a compressed high-gain tone can FAIL. A longer horizon (≈10 s) almost always
    /// contains a genuine gap, so the estimate falls back to the real noise floor.</para>
    /// </summary>
    public int NoiseHistoryFrames { get; }

    /// <summary>
    /// Streaming IDLE floor window in frames; <c>0</c> (the default) is the historical
    /// behaviour, where <see cref="NoiseFloorEnergy"/> is only the low percentile over
    /// <see cref="NoiseHistoryFrames"/>.
    /// <para><b>Why.</b> That percentile is "the quietest moment of the last 10 s", and
    /// during continuous playing the quietest moment is still PLAYING — so the presence gate
    /// demanded every note be several times louder than the riff around it, and on real DI
    /// songs it refused 38.6 % of perfectly played notes (measured 2026-09-29).</para>
    /// <para><b>What it does.</b> A floor that drops instantly to any quieter frame and
    /// creeps up only by <see cref="IdleFloorRiseDbPerSecond"/> while the player plays, and
    /// that SNAPS to the input's level whenever the last <see cref="IdleSteadyFrames"/>
    /// frames were steady (q95 ≤ <see cref="IdleSteadyRatio"/> × q05 — hum and hiss are,
    /// phrases are not). While the input is steady the floor IS that level; otherwise it is
    /// the lower of the idle floor and the percentile. Exact digital silence teaches it
    /// nothing. Offline (<see cref="AnalyzeWhole"/>) ignores it.</para>
    /// </summary>
    public int IdleSteadyFrames { get; }

    /// <summary>q95/q05 bound under which the idle window counts as steady; see
    /// <see cref="IdleSteadyFrames"/>.</summary>
    public float IdleSteadyRatio { get; }

    /// <summary>How fast the idle floor may rise while the input is not steady, in dB/s;
    /// see <see cref="IdleSteadyFrames"/>.</summary>
    public double IdleFloorRiseDbPerSecond { get; }

    /// <summary>
    /// Extra veto-only kernels BELOW <see cref="FMinHz"/>, addressable at NEGATIVE bin
    /// indices (down to <see cref="LowestBin"/>). <c>0</c> (the default) is exactly the
    /// historical behaviour.
    /// </summary>
    /// <remarks>
    /// <para><b>What this is for.</b> <see cref="JuceChartVerifier.SubOctaveVeto"/> asks
    /// whether a confirmation is better explained as the 2nd harmonic of the note an OCTAVE
    /// BELOW, by reading <c>bin - 12·Step</c>. Below <c>fMinMidi + 12</c> that index is
    /// negative, and a negative bin used to read as a hard 0 — so the veto silently could not
    /// fire for MIDI 28..39 at the shipping <c>fMinMidi = 28</c>, which is precisely the
    /// register E1 support was added for (bass low E/F/F#/G; a 5-string's low B at MIDI 23 is
    /// a real sounding octave below a charted B1). <see cref="JuceChartVerifier"/>'s peak test
    /// had the same hole: at bins 0 and 1 the lower comparison was against a hard 0 and
    /// therefore always true, making the peak test half-vacuous for the lowest semitone.</para>
    /// <para><b>What it deliberately does NOT change.</b> Sub-octave bins are excluded from
    /// <see cref="BinCount"/> and from the broadband energy sum, so <see cref="FrameEnergy"/>
    /// and <see cref="NoiseFloorEnergy"/> — and with them every measured presence-gate number
    /// — are bit-identical whether this is 0 or 26. <see cref="BinForMidi"/> is unaffected too.
    /// Only <see cref="Magnitude"/> and <see cref="Floor"/> at negative bins change, from
    /// "always 0 / always MaxValue" to a real reading.</para>
    /// <para>Cost is one extra kernel per bin at the longest (capped) length: +18% of the
    /// per-frame tap count at the shipping 144-bin config. See
    /// <see cref="LiveCqtConfig.SubOctaveBins"/> for the value that ships and why.</para>
    /// </remarks>
    public int SubOctaveBins { get; }

    /// <summary>Lowest addressable bin index: <c>-SubOctaveBins</c> (0 when off).</summary>
    public int LowestBin => -SubOctaveBins;

    // Physical storage index of a (possibly negative) bin. Every array below is sized
    // _totalBins and indexed physically; only the public read surface speaks bin indices.
    private readonly int _totalBins;

    /// <summary>
    /// Build the per-bin complex kernels (one per bin, length set by the constant Q so low
    /// bins integrate longer) for one analysis instance. This is the expensive step;
    /// construct once and reuse. The shipping configuration lives in
    /// <c>LiveCqtConfig</c> — pass those constants rather than literals.
    /// </summary>
    /// <param name="sampleRate">Sample rate in Hz of the audio to be analysed.</param>
    /// <param name="hop">Frame advance in samples.</param>
    /// <param name="fMinMidi">MIDI note of bin 0; 40 is E2.</param>
    /// <param name="binsPerOctave">Bins per octave; a positive multiple of 12.</param>
    /// <param name="binCount">How many bins at or above <paramref name="fMinMidi"/>.</param>
    /// <param name="relFloor">Relative presence floor; see <see cref="RelFloor"/>.</param>
    /// <param name="maxKernelSamples">Kernel length cap in samples; 0 means full
    /// constant-Q, capped only at the Nyquist-derived limit.</param>
    /// <param name="floorPercentile">Temporal percentile used for the per-bin floor;
    /// 1.0 is the legacy streaming rolling max.</param>
    /// <param name="noiseHistoryFrames">Horizon in frames for the adaptive noise floor;
    /// 0 estimates it over the whole ring.</param>
    /// <param name="subOctaveBins">How many bins to add BELOW <paramref name="fMinMidi"/>
    /// for the sub-octave veto; 0 adds none.</param>
    /// <param name="idleSteadyFrames">Streaming idle-floor window in frames; 0 turns the idle
    /// floor off. See <see cref="IdleSteadyFrames"/>.</param>
    /// <param name="idleSteadyRatio">q95/q05 bound for a steady (idle) window; at least 1.</param>
    /// <param name="idleFloorRiseDbPerSecond">How fast the idle floor may rise while the input
    /// is not steady, in dB/s; not negative.</param>
    /// <exception cref="ArgumentOutOfRangeException">A rate, hop or count is not positive.</exception>
    /// <exception cref="ArgumentException"><paramref name="binsPerOctave"/> is not a
    /// positive multiple of 12.</exception>
    public JuceConstantQ(
        int sampleRate = 44100,
        int hop = LiveCqtConfig.Hop,       // NOT a bare 256 — see LiveCqtConfig.Hop
        int fMinMidi = 40,                 // E2 (standard low E)
        int binsPerOctave = 24,            // quarter-tone
        int binCount = 120,                // 5 octaves E2..E7
        float relFloor = LiveCqtConfig.RelFloor,
        int maxKernelSamples = 0,          // 0 = full constant-Q (sr/2 cap)
        float floorPercentile = LiveCqtConfig.FloorPercentile,   // 1.0 = rolling max (legacy streaming floor)
        int noiseHistoryFrames = 0,        // 0 = estimate the noise floor over the ring
        int subOctaveBins = 0,             // 0 = no kernels below fMin (see SubOctaveBins)
        int idleSteadyFrames = 0,          // 0 = no idle floor (see IdleSteadyFrames)
        float idleSteadyRatio = LiveCqtConfig.IdleSteadyRatio,
        double idleFloorRiseDbPerSecond = LiveCqtConfig.IdleFloorRiseDbPerSecond)
    {
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        if (hop <= 0) throw new ArgumentOutOfRangeException(nameof(hop));
        if (binsPerOctave < 12 || binsPerOctave % 12 != 0)
            throw new ArgumentException("binsPerOctave must be a positive multiple of 12.", nameof(binsPerOctave));
        if (binCount <= 0) throw new ArgumentOutOfRangeException(nameof(binCount));
        if (floorPercentile is <= 0f or > 1f)
            throw new ArgumentOutOfRangeException(nameof(floorPercentile), "must be in (0, 1].");
        if (noiseHistoryFrames < 0) throw new ArgumentOutOfRangeException(nameof(noiseHistoryFrames));
        if (subOctaveBins < 0) throw new ArgumentOutOfRangeException(nameof(subOctaveBins));
        if (idleSteadyFrames < 0) throw new ArgumentOutOfRangeException(nameof(idleSteadyFrames));
        if (!(idleSteadyRatio >= 1f)) throw new ArgumentOutOfRangeException(nameof(idleSteadyRatio), "must be >= 1.");
        if (!(idleFloorRiseDbPerSecond >= 0)) throw new ArgumentOutOfRangeException(nameof(idleFloorRiseDbPerSecond));

        FloorPercentile = floorPercentile;
        NoiseHistoryFrames = noiseHistoryFrames;
        IdleSteadyFrames = idleSteadyFrames;
        IdleSteadyRatio = idleSteadyRatio;
        IdleFloorRiseDbPerSecond = idleFloorRiseDbPerSecond;
        _idleRisePerFrame = (float)Math.Pow(10.0, idleFloorRiseDbPerSecond / 20.0 * hop / sampleRate);
        SampleRate = sampleRate;
        Hop = hop;
        FMinMidi = fMinMidi;
        BinsPerOctave = binsPerOctave;
        BinCount = binCount;
        RelFloor = relFloor;
        Step = binsPerOctave / 12;
        FMinHz = 440f * MathF.Pow(2f, (fMinMidi - 69) / 12f);
        SubOctaveBins = subOctaveBins;
        _totalBins = subOctaveBins + binCount;

        // Constant Q: Q = 1 / (2^(1/bpo) - 1); kernel length L_b = Q * sr / f_b.
        double q = 1.0 / (Math.Pow(2.0, 1.0 / binsPerOctave) - 1.0);
        int maxLenCap = maxKernelSamples > 0 ? maxKernelSamples : sampleRate / 2; // bound the lowest-bin window
        MaxKernelSamples = maxLenCap;
        _kernelRe = new float[_totalBins][];
        _kernelIm = new float[_totalBins][];
        _halfLen = new int[_totalBins];
        int maxHalf = 0;
        for (int p = 0; p < _totalBins; p++)
        {
            // p is the PHYSICAL index; p - SubOctaveBins is the bin, negative below fMin.
            double fb = FMinHz * Math.Pow(2.0, (p - subOctaveBins) / (double)binsPerOctave);
            int len = (int)Math.Round(q * sampleRate / fb);
            len = Math.Min(len, maxLenCap);
            if (len < 4) len = 4;
            if ((len & 1) == 0) len++;     // odd → integer center
            int half = (len - 1) / 2;
            _halfLen[p] = half;
            if (half > maxHalf) maxHalf = half;

            var re = new float[len];
            var im = new float[len];
            double wsum = 0;
            for (int n = 0; n < len; n++)
                wsum += 0.5 - 0.5 * Math.Cos(2.0 * Math.PI * n / (len - 1));
            double norm = wsum > 0 ? wsum : 1.0;
            for (int n = 0; n < len; n++)
            {
                double w = 0.5 - 0.5 * Math.Cos(2.0 * Math.PI * n / (len - 1));
                double arg = 2.0 * Math.PI * fb * (n - half) / sampleRate;
                re[n] = (float)(w * Math.Cos(arg) / norm);
                im[n] = (float)(-w * Math.Sin(arg) / norm);
            }
            _kernelRe[p] = re;
            _kernelIm[p] = im;
        }
        _maxHalf = maxHalf;
    }

    /// <summary>Nearest CQT bin for a MIDI pitch.</summary>
    public int BinForMidi(double midi) => (int)Math.Round((midi - FMinMidi) * Step);

    /// <summary>Nearest frame index for a time in seconds (frame center = frame·hop).</summary>
    public long FrameOfSeconds(double seconds) => (long)Math.Round(seconds * SampleRate / Hop);

    /// <summary>Ring index for a possibly-negative absolute position/frame. The
    /// transport-aligned stream axis goes negative during count-in (before t=0),
    /// so a plain <c>(int)(x % n)</c> would produce a NEGATIVE array index and
    /// crash the tick loop — that was the "0 hits / 0 misses" regression.</summary>
    private static int Mod(long x, int n) { int r = (int)(x % n); return r < 0 ? r + n : r; }

    /// <summary>Lowest absolute frame index still readable. In streaming mode that is the
    /// oldest frame the ring still holds (it advances as frames are pushed out); offline it
    /// is always 0.</summary>
    public long OldestFrame => _streaming ? _ringOldest : 0;

    /// <summary>Highest absolute frame index readable — the most recent frame
    /// <c>Push</c> completed in streaming mode, or the last frame of the analysed clip
    /// offline. May be negative during count-in, when the transport axis is before t=0.</summary>
    public long NewestFrame => _streaming ? _ringNewest : _frameCount - 1;

    // ─── Offline whole-clip transform ──────────────────────────────────────
    /// <summary>Compute the full centered CQT of a clip and its per-bin floors.
    /// Switches this instance to offline mode for the accessors below.</summary>
    public void AnalyzeWhole(ReadOnlySpan<float> y)
    {
        _streaming = false;
        int frames = y.Length / Hop + 1;
        _frameCount = frames;
        var matrix = new float[_totalBins][];
        for (int p = 0; p < _totalBins; p++) matrix[p] = new float[frames];

        for (int f = 0; f < frames; f++)
        {
            int center = f * Hop;
            for (int p = 0; p < _totalBins; p++)
                matrix[p][f] = KernelMagnitude(y, center, p);
        }
        _matrix = matrix;

        var floor = new float[_totalBins];
        var scratch = new float[frames];
        for (int p = 0; p < _totalBins; p++)
        {
            System.Array.Copy(matrix[p], scratch, frames);
            System.Array.Sort(scratch);
            floor[p] = RelFloor * scratch[Math.Clamp((int)(0.95 * (frames - 1)), 0, frames - 1)];
        }
        _floorOffline = floor;

        // Broadband energy per frame (Σ bins) + its adaptive noise floor.
        // CHARTED bins only — the sub-octave extension must not move the noise floor
        // (see SubOctaveBins).
        var energy = new float[frames];
        for (int f = 0; f < frames; f++)
        {
            float sum = 0;
            for (int p = SubOctaveBins; p < _totalBins; p++) sum += matrix[p][f];
            energy[f] = sum;
        }
        _energy = energy;
        var esort = (float[])energy.Clone();
        System.Array.Sort(esort);
        _noiseEnergyOffline = esort[Math.Clamp((int)(NoisePercentile * (frames - 1)), 0, frames - 1)];
    }

    private float KernelMagnitude(ReadOnlySpan<float> y, int center, int physicalBin)
    {
        var re = _kernelRe[physicalBin];
        var im = _kernelIm[physicalBin];
        int half = _halfLen[physicalBin];
        int start = center - half;
        float sr = 0, si = 0;
        for (int n = 0; n < re.Length; n++)
        {
            int idx = start + n;
            if ((uint)idx >= (uint)y.Length) continue; // zero-pad edges
            float x = y[idx];
            sr += x * re[n];
            si += x * im[n];
        }
        return Finite(MathF.Sqrt(sr * sr + si * si));
    }

    /// <summary>
    /// Non-finite results never leave a kernel. <see cref="Push"/> scrubs its input, so the
    /// streaming path cannot see a NaN sample — but a dot product can still overflow to ±Inf
    /// on a pathological input, and <see cref="AnalyzeWhole"/> reads a caller-owned span it
    /// must not (and, at whole-song size, cannot affordably) copy to scrub. Guarding the
    /// output is one comparison per (bin, frame) instead of one per tap, and it is what keeps
    /// every downstream statistic — the per-bin floors, the frame energies, the noise floor —
    /// finite. It changes nothing for finite input.
    /// </summary>
    private static float Finite(float m) => float.IsFinite(m) ? m : 0f;

    // ─── Streaming transform ───────────────────────────────────────────────
    /// <summary>Begin (or restart) streaming; <paramref name="streamPosition"/>
    /// is the absolute capture-stream sample index of the next pushed sample.</summary>
    /// <param name="streamPosition">Absolute capture-stream sample index of the next
    /// pushed sample.</param>
    /// <param name="ringFrames">Frames held in the rolling frame ring; must be positive.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="ringFrames"/> is not
    /// positive.</exception>
    public void Reset(long streamPosition = 0, int ringFrames = LiveCqtConfig.RingFrames)
    {
        // The one public argument on this type that was NOT range-checked, and the failure
        // it produced named nothing: 0 left _ringFrames at 0 and the NEXT Push died in
        // Mod(frame, 0) with a DivideByZeroException (Push only substitutes the default
        // when it RE-ENTERS Reset, which streamPosition == _writePos does not), while a
        // negative value threw OverflowException out of the ring allocation below.
        if (ringFrames <= 0)
            throw new ArgumentOutOfRangeException(nameof(ringFrames),
                $"The rolling frame ring must hold at least one frame; was {ringFrames}.");
        _streaming = true;
        _matrix = null;
        _floorOffline = null;
        _startPos = streamPosition;
        _writePos = streamPosition;
        // Frames are numbered ABSOLUTELY (center = frame·hop on the pushed stream
        // axis), so a chart time maps straight to a frame regardless of where the
        // stream was (re)anchored. First computable centered frame: its window
        // [center-maxHalf, center+maxHalf] must start at/after the session start.
        _nextFrame = (streamPosition + _maxHalf + Hop - 1) / Hop;   // ceil((start+maxHalf)/hop)
        _startFrame = _nextFrame;
        _ringNewest = _nextFrame - 1;   // empty range
        _ringOldest = _nextFrame;
        _ringFrames = ringFrames;
        // Raw ring holds the widest kernel (2·maxHalf) plus one emit-chunk of
        // headroom, so a ready frame's whole window is still resident when Push
        // appends up to EmitChunk samples before emitting (captures can arrive in
        // large bursts). Push sub-chunks anything bigger.
        _rawSize = 2 * _maxHalf + EmitChunk + Hop + 4;

        // A re-Reset is not a rare event: Push treats ANY stream discontinuity as
        // one, so a resume, a seek or a ring overrun lands here mid-song. Every
        // buffer below is sized from either _totalBins (fixed for the instance)
        // or ringFrames, so a Reset that does not change ringFrames can keep the
        // whole lot — at the shipping config that is a 12,548-float raw ring, 384
        // rows of 170 floats, a 1,722-float noise history and the scratch, i.e.
        // ~390 arrays and ~340 KB re-allocated per discontinuity for nothing.
        // CLEARED, not merely reused: a freshly allocated array is zeroed, so the
        // clear is what makes this bit-identical rather than nearly so.
        _raw = Reuse(_raw, _rawSize);

        if (_ring.Length != ringFrames)
        {
            _ring = new float[ringFrames][];
            for (int i = 0; i < ringFrames; i++) _ring[i] = new float[_totalBins];
        }
        else
        {
            for (int i = 0; i < ringFrames; i++) System.Array.Clear(_ring[i]);
        }
        _ringEnergy = Reuse(_ringEnergy, ringFrames);

        _noiseHist = NoiseHistoryFrames > 0
            ? Reuse(_noiseHist, NoiseHistoryFrames)
            : System.Array.Empty<float>();
        // Scratch for NoiseFloorEnergy's percentile sort, sized ONCE here for the
        // widest horizon it can be asked for. Without it that method allocated a
        // fresh float[count] per EMITTED FRAME on the live path.
        _noiseScratch = Reuse(_noiseScratch, Math.Max(_noiseHist.Length, ringFrames));
        _noiseHistNewest = -1;
        _idleWin = IdleSteadyFrames > 0 ? Reuse(_idleWin, IdleSteadyFrames) : System.Array.Empty<float>();
        _idleScratch = IdleSteadyFrames > 0 ? Reuse(_idleScratch, IdleSteadyFrames) : System.Array.Empty<float>();
        _idleCount = 0;
        _idlePos = 0;
        _idleSinceCheck = 0;
        _idleFloor = -1f;
        _idleSteady = false;
        _floorCache = Reuse(_floorCache, _totalBins);
        _floorStamp = _floorStamp.Length == _totalBins ? _floorStamp : new long[_totalBins];
        System.Array.Fill(_floorStamp, long.MinValue);
        _noiseStamp = long.MinValue;
    }

    /// <summary>
    ///     Append captured mono samples. <paramref name="streamPosition"/> is the
    ///     absolute index of the first sample on the axis of the audio being pushed.
    /// </summary>
    /// <remarks>
    ///     <b>CONTRACT — <paramref name="streamPosition"/> is a WRITE CURSOR, not a
    ///     label.</b> It must equal the previous call's
    ///     <c>streamPosition + samples.Length</c>. Successive pushes are contiguous by
    ///     definition, because one position IS one sample here.
    ///     <para>
    ///     Anything else is treated as a stream discontinuity (a genuine ring overrun /
    ///     seek / resume) and <b>DISCARDS EVERY BUFFERED FRAME</b> via
    ///     <see cref="Reset(long,int)"/>. That is intended for real gaps — but it means a
    ///     caller that rescales the position (e.g. by playback speed) silently resets the
    ///     transform on every push, so it never accumulates the
    ///     <c>2 × maxKernelSamples/2</c> samples a frame needs and the ring stays
    ///     permanently EMPTY. A caller doing that gets no frames and no error. This
    ///     really happened: it made live note detection judge 100% of pitched notes a
    ///     Miss at any practice speed other than 1.0. Do not scale this parameter — map
    ///     the chart time into the capture axis instead
    ///     (<c>LiveTimeMapping.ToCaptureSeconds</c>).
    ///     </para>
    /// </remarks>
    public void Push(ReadOnlySpan<float> samples, long streamPosition)
    {
        if (!_streaming || streamPosition != _writePos)
        {
            if (_streaming) _discontinuityResets++;
            Reset(streamPosition, _ringFrames == 0 ? LiveCqtConfig.RingFrames : _ringFrames);
        }

        // Append + emit in bounded chunks so a frame that becomes ready is emitted
        // before the raw ring could evict the start of its kernel window.
        int headroom = _rawSize - 2 * _maxHalf - Hop;   // = EmitChunk + 4
        for (int offset = 0; offset < samples.Length; offset += headroom)
        {
            var chunk = samples.Slice(offset, Math.Min(headroom, samples.Length - offset));
            // The write cursor advances by exactly 1, so wrap by COMPARISON. See
            // RingKernelMagnitude for why a `%` on _rawSize is expensive here.
            int idx = Mod(_writePos, _rawSize);
            foreach (var s in chunk)
            {
                // Scrub non-finite input (see NonFiniteSamples). One NaN would otherwise
                // pass every gate downstream and poison the noise floor for 10 seconds.
                if (float.IsFinite(s)) { _raw[idx] = s; }
                else { _raw[idx] = 0f; _nonFiniteSamples++; }
                _writePos++;
                if (++idx == _rawSize) idx = 0;
            }
            EmitReadyFrames();
        }
    }

    /// <summary>The same array, zeroed, when it is already the right length —
    /// otherwise a new one, which is zeroed by definition. The two are
    /// indistinguishable to every reader, which is what makes skipping the
    /// allocation a pure optimisation rather than a behaviour change.</summary>
    private static float[] Reuse(float[] buffer, int length)
    {
        if (buffer.Length != length) return new float[length];
        System.Array.Clear(buffer);
        return buffer;
    }

    // Emit every centered frame whose full widest-kernel span is now resident.
    private void EmitReadyFrames()
    {
        while (true)
        {
            long center = _nextFrame * Hop;
            if (center - _maxHalf < _startPos) { _nextFrame++; continue; }   // predates session start
            if (center + _maxHalf >= _writePos) break;                        // need future samples
            if (center - _maxHalf < _writePos - _rawSize) { _nextFrame++; continue; } // safety (chunking prevents this)
            EmitFrame(_nextFrame, center);
            _nextFrame++;
        }
    }

    private void EmitFrame(long frame, long center)
    {
        int slot = Mod(frame, _ringFrames);
        var col = _ring[slot];
        float sum = 0;
        for (int p = 0; p < _totalBins; p++)
        {
            col[p] = RingKernelMagnitude(center, p);
            if (p >= SubOctaveBins) sum += col[p];   // CHARTED bins only (see SubOctaveBins)
        }
        _ringEnergy[slot] = sum;
        _ringNewest = frame;
        _ringOldest = Math.Max(_ringOldest, frame - _ringFrames + 1);

        if (_noiseHist.Length > 0)
        {
            _noiseHist[Mod(frame, _noiseHist.Length)] = sum;
            _noiseHistNewest = frame;
        }

        if (_idleWin.Length > 0) TrackIdleFloor(sum);
    }

    // How often (in frames) the idle window's steadiness is re-decided: a sort of
    // IdleSteadyFrames floats, ~12 times a second at 48 kHz instead of ~190.
    private const int IdleCheckEveryFrames = 16;

    // Exact digital silence (a device that has not started, a hard noise gate) says nothing
    // about the noise a live input carries.
    private const float DigitalSilenceEnergy = 1e-9f;

    private void TrackIdleFloor(float energy)
    {
        if (!(energy > DigitalSilenceEnergy)) return;

        _idleWin[_idlePos] = energy;
        if (++_idlePos == _idleWin.Length) _idlePos = 0;
        if (_idleCount < _idleWin.Length) _idleCount++;

        if (_idleFloor < 0f || energy < _idleFloor) _idleFloor = energy;
        else _idleFloor *= _idleRisePerFrame;

        if (++_idleSinceCheck < IdleCheckEveryFrames || _idleCount < _idleWin.Length) return;
        _idleSinceCheck = 0;
        System.Array.Copy(_idleWin, _idleScratch, _idleCount);
        System.Array.Sort(_idleScratch, 0, _idleCount);
        float q05 = _idleScratch[(int)(NoisePercentile * (_idleCount - 1))];
        float q95 = _idleScratch[(int)((1f - NoisePercentile) * (_idleCount - 1))];
        _idleSteady = q95 <= IdleSteadyRatio * q05;
        if (_idleSteady) _idleFloor = q05;
    }

    /// <summary>
    /// One bin's centered kernel against the raw ring.
    /// <para><b>The ring wrap is hoisted out of the tap loop on purpose — do not put a
    /// <c>%</c> back in.</b> The tap index advances by exactly 1, so one <see cref="Mod"/>
    /// per (bin, frame) plus a compare-and-reset per tap is equivalent to a modulo per tap,
    /// and vastly cheaper: <c>_rawSize</c> is a runtime field that is NOT a power of two
    /// (12,548 at the shipping config), and the dividend is a <c>long</c> on an unbounded
    /// stream axis, so the JIT can neither use a magic-number reciprocal nor narrow it — it
    /// emits a hardware integer division. That was ~4.3·10⁵ divisions per frame, ~7.5·10⁷/s
    /// at hop 256 / 44.1 kHz, on the live-play path; the hoist removes ~3,000× of them
    /// (144 per frame instead of 434,500). Taps are visited in the identical order with the
    /// identical values, so the output is bit-for-bit unchanged — pinned by
    /// <c>JuceConstantQRingWrapTests</c>.</para>
    /// <para>(<c>JuceGuitarAnalyzer.WriteToRing</c>/<c>ReadRing</c> do use a <c>%</c>, and
    /// that is fine: their <c>RingSize</c> is a compile-time power-of-two constant and the
    /// dividend is cast to <c>ulong</c> first, so RyuJIT lowers it to a single <c>and</c>.)</para>
    /// </summary>
    private float RingKernelMagnitude(long center, int physicalBin)
    {
        var re = _kernelRe[physicalBin];
        var im = _kernelIm[physicalBin];
        int half = _halfLen[physicalBin];
        long start = center - half;
        int idx = Mod(start, _rawSize);
        long oldest = _writePos - _rawSize;
        float sr = 0, si = 0;
        for (int n = 0; n < re.Length; n++)
        {
            long abs = start + n;
            if (abs >= _startPos && abs < _writePos && abs >= oldest)
            {
                float x = _raw[idx];
                sr += x * re[n];
                si += x * im[n];
            }
            if (++idx == _rawSize) idx = 0;
        }
        return Finite(MathF.Sqrt(sr * sr + si * si));
    }

    // ─── Unified read surface (consumed by JuceChartVerifier) ──────────────
    /// <summary>Magnitude at (bin, absolute frame). 0 when out of range/evicted.</summary>
    public float Magnitude(int bin, long frame)
    {
        int p = bin + SubOctaveBins;
        if ((uint)p >= (uint)_totalBins) return 0;
        if (_streaming)
        {
            if (frame < _ringOldest || frame > _ringNewest) return 0;
            return _ring[Mod(frame, _ringFrames)][p];
        }
        if (_matrix is null || frame < 0 || frame >= _frameCount) return 0;
        return _matrix[p][frame];
    }

    /// <summary>Per-bin "meaningfully loud" floor: <c>RelFloor</c> × the 95th percentile
    /// (offline) or × the <see cref="FloorPercentile"/> of the resident frames (streaming;
    /// 1.0 = the legacy rolling max).</summary>
    public float Floor(int bin)
    {
        int p = bin + SubOctaveBins;
        if ((uint)p >= (uint)_totalBins) return float.MaxValue;
        if (!_streaming) return _floorOffline is null ? 0f : _floorOffline[p];
        if (_ringNewest < _ringOldest) return 0f;
        if (_floorStamp[p] == _ringNewest) return _floorCache[p];

        float stat;
        if (FloorPercentile >= 1f)
        {
            float max = 0;
            for (long f = _ringOldest; f <= _ringNewest; f++)
            {
                float v = _ring[Mod(f, _ringFrames)][p];
                if (v > max) max = v;
            }
            stat = max;
        }
        else
        {
            int count = (int)(_ringNewest - _ringOldest + 1);
            Span<float> vals = count <= 512 ? stackalloc float[count] : new float[count];
            int i = 0;
            for (long f = _ringOldest; f <= _ringNewest; f++)
                vals[i++] = _ring[Mod(f, _ringFrames)][p];
            vals.Sort();
            stat = vals[Math.Clamp((int)(FloorPercentile * (count - 1)), 0, count - 1)];
        }

        var floor = RelFloor * stat;
        _floorCache[p] = floor;
        _floorStamp[p] = _ringNewest;
        return floor;
    }

    /// <summary>Broadband energy (Σ bins) of a frame. 0 when out of range.</summary>
    public float FrameEnergy(long frame)
    {
        if (_streaming)
        {
            if (frame < _ringOldest || frame > _ringNewest) return 0f;
            return _ringEnergy[Mod(frame, _ringFrames)];
        }
        if (_energy is null || frame < 0 || frame >= _frameCount) return 0f;
        return _energy[frame];
    }

    /// <summary>Adaptive noise floor of broadband energy: a low temporal percentile
    /// of Σ-bin energy (the total when nothing is played), tempered by the idle floor when
    /// <see cref="IdleSteadyFrames"/> is on. Because summing 120 bins
    /// averages out, idle input sits stably at this floor while a played note spikes
    /// well above it — so the verifier's presence gate can reject idle noise/hum
    /// without a per-bin argmax inflating the comparison. Gain-independent.</summary>
    public float NoiseFloorEnergy()
    {
        if (!_streaming) return _noiseEnergyOffline;
        if (_ringNewest < _ringOldest) return 0f;
        if (_noiseStamp == _ringNewest) return _noiseCache;

        float result;
        if (_noiseHist.Length > 0 && _noiseHistNewest >= 0)
        {
            // Long horizon (NoiseHistoryFrames): almost always contains a genuine gap in
            // the playing, so the low percentile lands on the real idle noise floor rather
            // than on "the quietest moment while playing".
            long oldest = Math.Max(_startFrame, _noiseHistNewest - _noiseHist.Length + 1);
            int count = (int)(_noiseHistNewest - oldest + 1);
            if (count <= 0) return 0f;
            // PERSISTENT scratch, sized at Reset. This allocated a float[count] —
            // 1,722 floats at the shipping 10 s horizon — and sorted it once per
            // EMITTED FRAME, i.e. ~172 times a second on the live path.
            // Array.Sort(array, index, length) is the same introsort the whole-
            // array overload runs, over the same values in the same order, so the
            // selected percentile is bit-identical.
            var vals = _noiseScratch;
            int i = 0;
            for (long f = oldest; f <= _noiseHistNewest; f++)
                vals[i++] = _noiseHist[Mod(f, _noiseHist.Length)];
            System.Array.Sort(vals, 0, count);
            result = vals[Math.Clamp((int)(NoisePercentile * (count - 1)), 0, count - 1)];
        }
        else
        {
            int count = (int)(_ringNewest - _ringOldest + 1);
            // The ring horizon fits the same scratch (it is sized for the LARGER
            // of the two). The stackalloc fallback stays for a caller that
            // reaches this before Reset has sized anything.
            Span<float> vals = _noiseScratch.Length >= count
                ? _noiseScratch.AsSpan(0, count)
                : (count <= 512 ? stackalloc float[count] : new float[count]);
            int i = 0;
            for (long f = _ringOldest; f <= _ringNewest; f++)
                vals[i++] = _ringEnergy[Mod(f, _ringFrames)];
            vals.Sort();
            result = vals[Math.Clamp((int)(NoisePercentile * (count - 1)), 0, count - 1)];
        }

        // Idle floor (IdleSteadyFrames > 0): the input's own level while it is steady,
        // otherwise never above what the player's quietest moment has shown.
        if (_idleFloor >= 0f) result = _idleSteady ? _idleFloor : MathF.Min(result, _idleFloor);

        _noiseCache = result;
        _noiseStamp = _ringNewest;
        return result;
    }
}
