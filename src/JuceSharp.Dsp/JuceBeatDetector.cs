namespace JuceSharp.Dsp;

/// <summary>One constant-tempo stretch of a song.</summary>
/// <param name="Time">When the segment starts - the time of its first beat.</param>
/// <param name="Bpm">Tempo held across the segment, measured over its whole span
/// rather than from one inter-beat interval.</param>
public readonly record struct JuceTempoSegment(TimeSpan Time, float Bpm);

/// <summary>
/// Offline beat / BPM / tempo-map detector. Computes a spectral-flux onset
/// envelope, autocorrelates it for the dominant period, then tracks the beats
/// with a dynamic program over a LOCAL tempo estimate.
///
/// <para>Typical use: feed it a fully-decoded mono song, get back the BPM, the
/// beat times, a tempo map and the downbeats. Designed for the PSARC-import
/// path where the supplied beat map is wrong or missing.</para>
///
/// <para><b>Behaviour change (0.9).</b> <see cref="Result.Beats"/> used to be a
/// FIXED GRID: one global period stepped from the first half-beat to the end,
/// each step snapped to the nearest local envelope maximum inside a ±period/4
/// window. That grid cannot represent a song that speeds up, and every beat
/// past a tempo change drifted. It is now the output of an Ellis-style beat
/// tracker. <see cref="JuceBeatDetector.AnalysisOptions.FixedGrid"/> restores the old
/// behaviour bit-for-bit (pinned by
/// <c>JuceBeatDetectorTempoMapTests.FixedGrid_ReproducesThePreChangeBeats</c>);
/// <see cref="Result.Bpm"/> is the same global autocorrelation figure either
/// way and did NOT change.</para>
/// </summary>
public sealed class JuceBeatDetector
{
    /// <summary>Sample rate in Hz of the mono audio handed to <see cref="Analyze"/>;
    /// fixed at construction and used to turn frame indices into times.</summary>
    public int   SampleRate { get; }

    /// <summary>Window length in samples of the spectral-flux FFT. Larger is better
    /// frequency resolution and worse time resolution; <see cref="Analyze"/> needs at
    /// least four of these windows of audio. Must be EVEN and at least 2; beyond that the
    /// accepted set is whatever <see cref="JuceFft"/> supports, and a power of two is the
    /// fastest case rather than a requirement of this type.</summary>
    public int   FftSize    { get; init; } = 1024;

    /// <summary>Advance in samples between successive flux frames. Sets the onset
    /// envelope's frame rate (<c>SampleRate / HopSize</c>) and therefore the best
    /// achievable beat precision.</summary>
    public int   HopSize    { get; init; } = 512;

    /// <summary>
    /// Slowest tempo to search for. Must be at least
    /// <see cref="MinAllowedBpm"/>.
    /// <para><b>The floor is a COST bound, not a musical opinion.</b> <c>maxLag</c>
    /// is <c>60/MinBpm · fps</c>, and <c>LocalPeriods</c> sizes each 0.5 s window
    /// as <c>max(4·maxLag, 6·fps)</c> while scanning every lag in
    /// <c>[minLag, localMax]</c> with an inner loop of <c>len − lag</c> terms —
    /// so the local search is O(windows · maxLag²). At the defaults that is
    /// ~19 M multiply-adds for a five-minute track; at <c>MinBpm = 1</c> on the
    /// same track it is ~6·10¹⁰, i.e. HOURS, with no progress reporting and no
    /// cancellation token to get out with. The global autocorrelation and the DP
    /// are both fine; that one method is the blow-up.</para>
    /// </summary>
    public float MinBpm     { get; init; } = 60f;
    /// <summary>Fastest tempo to search for. Sets the shortest autocorrelation lag
    /// considered; a hop long enough to push <c>60/MaxBpm * fps</c> below one frame is
    /// floored at one lag, since an envelope cannot carry a period shorter than a
    /// frame.</summary>
    public float MaxBpm     { get; init; } = 200f;

    /// <summary>Lower bound on <see cref="MinBpm"/>. 20 BPM is one beat every
    /// three seconds — already slower than any music this detector is pointed
    /// at, and the point below which <c>LocalPeriods</c>' quadratic cost stops
    /// being bounded by anything. See <see cref="MinBpm"/>.</summary>
    public const float MinAllowedBpm = 20f;

    /// <summary>
    /// Count of non-finite (NaN / ±Inf) input samples that <see cref="Analyze"/>
    /// replaced with silence.
    /// <para><b>Why this is not optional.</b> The flux loop is
    /// <c>d = mag - prevMag[k]; if (d &gt; 0) …; prevMag[k] = mag</c>. One NaN
    /// sample makes every <c>mag</c> in that frame NaN, <c>d &gt; 0</c> is FALSE
    /// for a NaN so nothing is added, and then the NaN is STORED into
    /// <c>prevMag</c> — where it stays for the rest of the song, because
    /// <c>NaN - anything</c> is NaN. The flux envelope goes flat, the
    /// autocorrelation finds nothing, <c>bestLag</c> stays at its
    /// <c>minLag</c> seed and <see cref="Analyze"/> returns <see cref="MaxBpm"/>
    /// with a fabricated beat grid — silently, with no exception. Mirrors the
    /// scrub rule the native <c>DspChain::process</c> and
    /// <see cref="JuceConstantQ.NonFiniteSamples"/> follow.</para>
    /// </summary>
    public long NonFiniteSamples { get; private set; }

    /// <summary>Create a detector for audio at <paramref name="sampleRate"/> Hz.
    /// Throws <see cref="ArgumentOutOfRangeException"/> when the rate is not
    /// positive.</summary>
    /// <param name="sampleRate">Sample rate of the audio this detector will analyse.</param>
    public JuceBeatDetector(int sampleRate)
    {
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        SampleRate = sampleRate;
    }

    /// <summary>Analysis options. One knob today; a record so adding the next
    /// one is not another init property on the detector.</summary>
    public sealed record AnalysisOptions
    {
        /// <summary>
        /// Emit the pre-0.9 FIXED beat grid instead of tracked beats: one global
        /// period stepped from the first half-beat, each step snapped to the
        /// nearest envelope maximum within ±period/4. Default <c>false</c>.
        /// <para>A grid is the right answer only when the tempo is known to be
        /// constant — a click track, a quantised export, a test fixture. On real
        /// playing every beat past a tempo change drifts, which is what the
        /// tracker exists to fix. With this set, <see cref="Result.TempoMap"/>
        /// carries the single global tempo and <see cref="Result.Downbeats"/> is
        /// empty: a fixed grid has no evidence about meter.</para>
        /// </summary>
        public bool FixedGrid { get; init; }

        /// <summary>
        /// How far the local tempo must move before a new
        /// <see cref="JuceTempoSegment"/> is started, in BPM.
        /// <para><b>Floored by what the envelope can actually resolve.</b> One
        /// frame of inter-beat interval is <c>bpm² / (60 · fps)</c> BPM —
        /// 5.6 BPM at 120 BPM with the default 512-sample hop at 22 kHz — so a
        /// 2 BPM tolerance there would segment on quantisation noise alone. The
        /// effective tolerance is <c>max(this, 1.5 × frame resolution)</c>.
        /// Halve <see cref="HopSize"/> to buy real resolution.</para>
        /// </summary>
        public float SegmentToleranceBpm { get; init; } = 2f;
    }

    /// <summary>Analysis options; see <see cref="AnalysisOptions"/>.</summary>
    public AnalysisOptions Options { get; init; } = new();

    /// <summary>
    /// Detector output.
    /// </summary>
    /// <param name="Bpm">Global tempo from the whole-signal autocorrelation with
    /// parabolic interpolation. Unchanged by the 0.9 tracker, and NOT derived
    /// from <paramref name="TempoMap"/>.</param>
    /// <param name="Beats">Tracked beat times — or the pre-0.9 fixed grid when
    /// <see cref="JuceBeatDetector.AnalysisOptions.FixedGrid"/> is set.</param>
    /// <param name="TempoMap">Piecewise-constant tempo: one segment per stretch
    /// of beats whose local tempo agrees. A steady song yields exactly one.</param>
    /// <param name="Downbeats">Estimated bar starts, empty in fixed-grid mode or
    /// when there are too few beats to judge a bar.</param>
    /// <param name="BeatsPerBar">3 or 4, or 0 when no meter was judged.</param>
    /// <param name="Confidence">
    /// How periodic the onset envelope actually was, in <c>0..1</c>: the
    /// PROMINENCE of the winning autocorrelation peak over the mean
    /// autocorrelation across the searched lag range,
    /// <c>(best − mean) / best</c>.
    /// <para><b>It exists because this detector used to fabricate a whole
    /// analysis for SILENCE and there was no way to tell.</b> <c>bestVal</c>
    /// starts at 0 and <c>v &gt; bestVal</c> is never true for an all-zero
    /// envelope, so <c>bestLag</c> stayed at its <c>minLag</c> seed and
    /// <see cref="Bpm"/> came out at ≈<see cref="MaxBpm"/>, with a beat chain
    /// through arbitrary tied frames and a tempo map to match — indistinguishable
    /// from a real detection. That is the same shape as the
    /// <see cref="NonFiniteSamples"/> failure the class doc describes; the NaN
    /// path was fixed and the silent/ambient one was not. A degenerate analysis
    /// now returns <see cref="Bpm"/> 0 with empty beats AND
    /// <c>Confidence == 0</c>; a genuine detection is well above zero. Treat a
    /// low value as "this audio may not have a beat", never as an error.</para>
    /// </param>
    public sealed record Result(
        float Bpm,
        IReadOnlyList<TimeSpan> Beats,
        IReadOnlyList<JuceTempoSegment>? TempoMap = null,
        IReadOnlyList<TimeSpan>? Downbeats = null,
        int BeatsPerBar = 0,
        float Confidence = 0f)
    {
        /// <inheritdoc cref="Result(float, IReadOnlyList{TimeSpan}, IReadOnlyList{JuceTempoSegment}, IReadOnlyList{TimeSpan}, int, float)"/>
        public IReadOnlyList<JuceTempoSegment> TempoMap { get; init; } =
            TempoMap ?? Array.Empty<JuceTempoSegment>();

        /// <inheritdoc cref="Result(float, IReadOnlyList{TimeSpan}, IReadOnlyList{JuceTempoSegment}, IReadOnlyList{TimeSpan}, int, float)"/>
        public IReadOnlyList<TimeSpan> Downbeats { get; init; } =
            Downbeats ?? Array.Empty<TimeSpan>();
    }

    /// <summary>
    /// Run the whole offline pass over a fully-decoded mono song: spectral flux, a global
    /// autocorrelation for the dominant period, then a dynamic program that tracks beats
    /// against a LOCAL tempo estimate, plus a tempo map and downbeats. Non-finite input
    /// samples are replaced with silence and counted in <see cref="NonFiniteSamples"/>.
    /// Synchronous, CPU-bound and not cancellable — call it off the UI thread.
    /// </summary>
    /// <param name="mono">Decoded mono audio; at least four <see cref="FftSize"/> windows long.</param>
    /// <returns>BPM, beat times, tempo map and downbeats.</returns>
    /// <exception cref="ArgumentException">Fewer than four FFT windows of audio.</exception>
    public Result Analyze(ReadOnlySpan<float> mono)
    {
        Validate();
        if (mono.Length < FftSize * 4)
            throw new ArgumentException("Need at least 4 FFT windows of audio.", nameof(mono));

        var flux = ComputeSpectralFlux(mono);
        var fps  = (float)SampleRate / HopSize;

        var env = MovingAverage(flux, Math.Max(3, (int)(fps * 0.05f)));

        // A hop longer than ~0.3 s makes 60/MaxBpm*fps < 1, so the floor was ZERO
        // — bpm = 60*fps/0 = +Inf, and `for (t += bestLag)` with bestLag 0 never
        // advances and never terminates. One lag is the true floor: an envelope
        // cannot carry a period shorter than one frame.
        int minLag = Math.Max(1, (int)MathF.Floor(60f / MaxBpm * fps));
        int maxLag = (int)MathF.Ceiling(60f / MinBpm * fps);
        maxLag = Math.Min(maxLag, env.Length / 2);
        if (maxLag < minLag)
            throw new ArgumentException(
                $"Audio is too short for the requested tempo range: it yields {env.Length} envelope " +
                $"frames, which cannot hold one period at {MinBpm} BPM.", nameof(mono));

        float bestVal = 0;
        int   bestLag = minLag;
        var   ac      = new float[maxLag + 2];
        for (int lag = minLag; lag <= maxLag; lag++)
        {
            float v = 0;
            for (int i = 0; i + lag < env.Length; i++) v += env[i] * env[i + lag];
            ac[lag] = v;
            if (v > bestVal) { bestVal = v; bestLag = lag; }
        }

        // No autocorrelation peak at ANY lag means the envelope carries no
        // periodicity — silence, a DC offset, an ambient pad. `bestVal` starting
        // at 0 and `v > bestVal` never firing left `bestLag` at its `minLag` seed,
        // which yields bpm ≈ MaxBpm plus a beat chain through arbitrary tied
        // frames and a tempo map built from them: a complete, confident-looking
        // analysis of nothing. Say so instead. See Result.Confidence.
        if (!(bestVal > 0f))
            return new Result(0f, Array.Empty<TimeSpan>(), Array.Empty<JuceTempoSegment>(),
                              Array.Empty<TimeSpan>(), 0, 0f);

        // PROMINENCE, not raw correlation: a flat non-zero envelope (a pad, a
        // sustained chord, room tone) autocorrelates near its own energy at EVERY
        // lag, so `bestVal / energy` would report near 1.0 for audio with no beat
        // in it at all. How far the winning lag stands above the average over the
        // searched range is the thing that distinguishes a pulse from a level.
        double acSum = 0;
        for (int lag = minLag; lag <= maxLag; lag++) acSum += ac[lag];
        var acMean = (float)(acSum / (maxLag - minLag + 1));
        var confidence = Math.Clamp((bestVal - acMean) / bestVal, 0f, 1f);

        // Parabolic interpolation around the integer peak. At the default hop the
        // envelope runs at ~86 fps, where one lag step near 120 BPM is ~2.5 BPM —
        // the integer peak alone cannot land inside a 1 BPM tolerance no matter
        // how clean the signal is.
        float refinedLag = bestLag;
        if (bestLag > minLag && bestLag < maxLag)
        {
            float y0 = ac[bestLag - 1], y1 = ac[bestLag], y2 = ac[bestLag + 1];
            float denom = y0 + y2 - 2f * y1;
            if (denom != 0f)
            {
                var delta = 0.5f * (y0 - y2) / denom;
                if (MathF.Abs(delta) <= 1f && float.IsFinite(delta)) refinedLag = bestLag + delta;
            }
        }

        float bpm = 60f * fps / refinedLag;

        if (Options.FixedGrid)
            return new Result(bpm, FixedGrid(env, bestLag, fps),
                              new[] { new JuceTempoSegment(TimeSpan.Zero, bpm) },
                              Confidence: confidence);

        var beatFrames = TrackBeats(env, fps, refinedLag, minLag, maxLag);
        // Two beats is the minimum an inter-beat interval needs. Below that
        // there is nothing to track and the grid is the honest answer.
        if (beatFrames.Count < 2)
            return new Result(bpm, FixedGrid(env, bestLag, fps),
                              new[] { new JuceTempoSegment(TimeSpan.Zero, bpm) },
                              Confidence: confidence);

        var beats = new List<TimeSpan>(beatFrames.Count);
        foreach (var f in beatFrames) beats.Add(TimeSpan.FromSeconds(f / fps));

        var tempoMap = BuildTempoMap(beatFrames, fps, Options.SegmentToleranceBpm);
        var (downbeats, beatsPerBar) = FindDownbeats(env, beatFrames, fps);
        return new Result(bpm, beats, tempoMap, downbeats, beatsPerBar, confidence);
    }

    /// <summary>The pre-0.9 beat grid, VERBATIM: one global period stepped from
    /// the first half-beat, each step snapped to the nearest envelope maximum
    /// inside +/- period/4. Kept bit-for-bit so
    /// <see cref="JuceBeatDetector.AnalysisOptions.FixedGrid"/> is a real escape hatch
    /// and not a re-derivation that happens to look similar.</summary>
    private static List<TimeSpan> FixedGrid(float[] env, int bestLag, float fps)
    {
        var beats   = new List<TimeSpan>();
        int window  = bestLag / 4;
        for (float t = bestLag * 0.5f; t < env.Length; t += bestLag)
        {
            int center = (int)t;
            int lo = Math.Max(0, center - window);
            int hi = Math.Min(env.Length - 1, center + window);
            int peak = lo;
            for (int i = lo + 1; i <= hi; i++) if (env[i] > env[peak]) peak = i;
            beats.Add(TimeSpan.FromSeconds(peak / fps));
        }
        return beats;
    }

    /// <summary>Tightness of the DP transition cost - the penalty for an
    /// inter-beat interval that is not the local period, as
    /// <c>-tightness * ln(interval / period)^2</c>. Ellis's published default;
    /// lower lets the tracker chase every onset, higher makes it ignore a real
    /// tempo change.</summary>
    private const float Tightness = 100f;

    /// <summary>
    /// Ellis-style dynamic-programming beat tracker.
    /// <para>Every frame gets the best cumulative score of a beat landing there:
    /// the (normalised) onset strength plus the best predecessor within
    /// <c>[t - 2p, t - p/2]</c>, penalised by how far that interval strays from
    /// the LOCAL period <c>p</c>. Backtracking from the best score in the final
    /// period gives a beat sequence that is globally optimal rather than
    /// greedy, which is what lets it ride through a bar of missing onsets.</para>
    /// <para>The local period is what makes a tempo RAMP trackable at all: with
    /// one global period the transition cost fights the ramp from both ends.</para>
    /// </summary>
    private static List<int> TrackBeats(float[] env, float fps, float globalLag, int minLag, int maxLag)
    {
        int n = env.Length;
        var period = LocalPeriods(env, fps, globalLag, minLag, maxLag);
        var onset  = NormalizeEnvelope(env);

        var score = new float[n];
        var back  = new int[n];
        for (int t = 0; t < n; t++)
        {
            float p  = period[t];
            int   hi = t - (int)MathF.Round(0.5f * p);
            int   lo = t - (int)MathF.Round(2.0f * p);
            if (lo < 0) lo = 0;

            float best = float.NegativeInfinity;
            int   bestI = -1;
            for (int tau = lo; tau <= hi; tau++)
            {
                float v = score[tau] - Tightness * Sq(MathF.Log((t - tau) / p));
                if (v > best) { best = v; bestI = tau; }
            }
            if (bestI < 0) { score[t] = onset[t]; back[t] = -1; }
            else           { score[t] = onset[t] + best; back[t] = bestI; }
        }

        // Start from the best score in the final period rather than the last
        // frame: the tail of the envelope is a fade, not a beat.
        int tailStart = Math.Max(0, n - (int)MathF.Round(2f * period[n - 1]));
        int end = tailStart;
        for (int i = tailStart; i < n; i++) if (score[i] > score[end]) end = i;

        var reversed = new List<int>();
        for (int i = end; i >= 0; i = back[i])
        {
            reversed.Add(i);
            if (back[i] < 0) break;
        }
        reversed.Reverse();
        return TrimEvidenceFreeEnds(reversed, onset);
    }

    /// <summary>
    /// Drop beats at either END of the tracked chain that sit on no onset at all.
    /// <para>The DP has no predecessor for the first half-period of the signal, so
    /// <c>score[t]</c> there is just <c>onset[t]</c> — which is ZERO across the
    /// lead-in silence, and every such frame ties. The backtrack therefore lands
    /// the chain's first beat on an arbitrary silent frame (frame 0 in practice),
    /// and the same happens in the trailing fade. One phantom beat at each end is
    /// invisible in the beat list and LOUD in the tempo map: it makes the first and
    /// last inter-beat interval meaningless, which is a whole spurious segment at
    /// each end. Interior beats are never trimmed — riding through a bar of missing
    /// onsets is the entire point of the tracker.</para>
    /// </summary>
    private static List<int> TrimEvidenceFreeEnds(List<int> beats, float[] onset)
    {
        if (beats.Count < 3) return beats;
        var strengths = new float[beats.Count];
        for (int i = 0; i < beats.Count; i++) strengths[i] = onset[beats[i]];
        var sorted = (float[])strengths.Clone();
        Array.Sort(sorted);
        float median = sorted[sorted.Length / 2];
        float floor  = 0.1f * median;

        int first = 0, last = beats.Count - 1;
        while (last - first >= 2 && strengths[first] <= floor) first++;
        while (last - first >= 2 && strengths[last]  <= floor) last--;
        return first == 0 && last == beats.Count - 1 ? beats : beats.GetRange(first, last - first + 1);
    }

    /// <summary>
    /// Per-frame beat period, from a windowed autocorrelation of the envelope
    /// with parabolic interpolation - the same refinement the global estimate
    /// uses, because an integer lag near 120 BPM is ~2.5 BPM wide at the default
    /// hop and that is already wider than a tempo map is worth.
    /// <para>Each window's autocorrelation is biased by a log-Gaussian prior
    /// around the GLOBAL lag (sigma = 0.7 octaves). Without it a local window
    /// routinely locks onto half or double the tempo and the tracker then emits
    /// a segment boundary that is an octave error, not a tempo change; with a
    /// prior that wide a genuine 100 to 140 BPM ramp (0.49 octaves end to end)
    /// is still free to move.</para>
    /// </summary>
    private static float[] LocalPeriods(float[] env, float fps, float globalLag, int minLag, int maxLag)
    {
        int n   = env.Length;
        var result = new float[n];
        int win = Math.Min(n, Math.Max(4 * maxLag, (int)(6f * fps)));
        int hop = Math.Max(1, (int)(0.5f * fps));

        var centers = new List<int>();
        var periods = new List<float>();
        var ac      = new float[maxLag + 2];

        for (int start = 0; start < n; start += hop)
        {
            int len = Math.Min(win, n - start);
            if (len <= minLag + 2) break;

            float bestVal = float.NegativeInfinity;
            int   bestLag = minLag;
            int   localMax = Math.Min(maxLag, len - 2);
            for (int lag = minLag; lag <= localMax; lag++)
            {
                float v = 0;
                int count = len - lag;
                for (int i = 0; i < count; i++) v += env[start + i] * env[start + i + lag];
                // Per-term normalisation: a longer lag has fewer terms, so a raw
                // sum is biased toward the short end of the search.
                v /= count;
                // Log-Gaussian tempo prior around the global estimate.
                v *= MathF.Exp(-0.5f * Sq(MathF.Log2(lag / globalLag) / 0.7f));
                ac[lag] = v;
                if (v > bestVal) { bestVal = v; bestLag = lag; }
            }

            float refined = bestLag;
            if (bestLag > minLag && bestLag < localMax)
            {
                float y0 = ac[bestLag - 1], y1 = ac[bestLag], y2 = ac[bestLag + 1];
                float denom = y0 + y2 - 2f * y1;
                if (denom != 0f)
                {
                    var delta = 0.5f * (y0 - y2) / denom;
                    if (MathF.Abs(delta) <= 1f && float.IsFinite(delta)) refined = bestLag + delta;
                }
            }
            centers.Add(start + len / 2);
            periods.Add(refined);
        }

        if (centers.Count == 0)
        {
            Array.Fill(result, globalLag);
            return result;
        }

        // Linear interpolation between window centres, held flat at both ends.
        int seg = 0;
        for (int t = 0; t < n; t++)
        {
            while (seg + 1 < centers.Count && centers[seg + 1] < t) seg++;
            if (seg + 1 >= centers.Count || t <= centers[0])
            {
                result[t] = periods[Math.Min(seg, periods.Count - 1)];
                continue;
            }
            int   c0 = centers[seg], c1 = centers[seg + 1];
            float f  = c1 == c0 ? 0f : (t - c0) / (float)(c1 - c0);
            f = Math.Clamp(f, 0f, 1f);
            result[t] = periods[seg] + f * (periods[seg + 1] - periods[seg]);
        }
        return result;
    }

    /// <summary>Zero-mean, unit-variance, half-wave-rectified onset strength.
    /// The DP adds the transition penalty to this, so an un-normalised envelope
    /// would make <see cref="Tightness"/> mean something different for every
    /// track.</summary>
    private static float[] NormalizeEnvelope(float[] env)
    {
        var o = new float[env.Length];
        if (env.Length == 0) return o;
        double mean = 0;
        foreach (var v in env) mean += v;
        mean /= env.Length;
        double var2 = 0;
        foreach (var v in env) var2 += (v - mean) * (v - mean);
        var sd = Math.Sqrt(var2 / env.Length);
        var inv = sd > 1e-12 ? 1.0 / sd : 0.0;
        for (int i = 0; i < env.Length; i++)
        {
            var z = (env[i] - mean) * inv;
            o[i] = z > 0 ? (float)z : 0f;
        }
        return o;
    }

    /// <summary>
    /// Piecewise-constant tempo from the tracked beats. A segment's tempo is
    /// taken over its whole SPAN (<c>60 * fps * beats / frames</c>), never from
    /// one inter-beat interval: a single interval is quantised to whole envelope
    /// frames, and averaging over the span is what makes the number finer than
    /// the frame grid.
    /// </summary>
    private static List<JuceTempoSegment> BuildTempoMap(List<int> beats, float fps, float toleranceBpm)
    {
        var segments = new List<JuceTempoSegment>();
        int intervals = beats.Count - 1;
        if (intervals < 1) return segments;

        var raw = new float[intervals];
        for (int i = 0; i < intervals; i++) raw[i] = 60f * fps / (beats[i + 1] - beats[i]);
        var sm = Median5(raw);

        int segStart = 0;
        for (int i = 1; i < intervals; i++)
        {
            float mean = SpanBpm(beats, fps, segStart, i - 1);
            // One frame of IBI is bpm^2 / (60*fps) BPM wide. Segmenting finer
            // than that is segmenting on quantisation noise.
            float floorTol = 1.5f * mean * mean / (60f * fps);
            if (MathF.Abs(sm[i] - mean) > MathF.Max(toleranceBpm, floorTol))
            {
                segments.Add(new JuceTempoSegment(TimeSpan.FromSeconds(beats[segStart] / fps), mean));
                segStart = i;
            }
        }
        segments.Add(new JuceTempoSegment(TimeSpan.FromSeconds(beats[segStart] / fps),
                                          SpanBpm(beats, fps, segStart, intervals - 1)));
        return segments;
    }

    private static float SpanBpm(List<int> beats, float fps, int firstInterval, int lastInterval)
    {
        int frames = beats[lastInterval + 1] - beats[firstInterval];
        int count  = lastInterval - firstInterval + 1;
        return frames <= 0 ? 0f : 60f * fps * count / frames;
    }

    private static float[] Median5(float[] x)
    {
        var y = new float[x.Length];
        Span<float> w = stackalloc float[5];
        for (int i = 0; i < x.Length; i++)
        {
            int lo = Math.Max(0, i - 2), hi = Math.Min(x.Length - 1, i + 2);
            int c = 0;
            for (int k = lo; k <= hi; k++) w[c++] = x[k];
            var slice = w[..c];
            for (int a = 1; a < c; a++)          // insertion sort; c <= 5
            {
                var v = slice[a];
                int b = a - 1;
                while (b >= 0 && slice[b] > v) { slice[b + 1] = slice[b]; b--; }
                slice[b + 1] = v;
            }
            y[i] = slice[c / 2];
        }
        return y;
    }

    /// <summary>
    /// Meter + downbeat phase from BEAT-SYNCHRONOUS onset energy: sample the
    /// envelope at each tracked beat, then ask which bar length (3 or 4) and
    /// which phase within it collects the accents. A 4/4 accent pattern spreads
    /// evenly over every phase class mod 3 and vice versa, which is exactly what
    /// makes the two separable without a trained model.
    /// </summary>
    private static (List<TimeSpan> Downbeats, int BeatsPerBar) FindDownbeats(
        float[] env, List<int> beats, float fps)
    {
        var empty = new List<TimeSpan>();
        if (beats.Count < 8) return (empty, 0);

        var e = new float[beats.Count];
        for (int i = 0; i < beats.Count; i++)
        {
            int lo = Math.Max(0, beats[i] - 2), hi = Math.Min(env.Length - 1, beats[i] + 2);
            float m = env[lo];
            for (int k = lo + 1; k <= hi; k++) if (env[k] > m) m = env[k];
            e[i] = m;
        }
        float mean = 0;
        foreach (var v in e) mean += v;
        mean /= e.Length;

        int   bestBar = 0, bestPhase = 0;
        float bestScore = 0f;
        foreach (var bar in new[] { 3, 4 })
        {
            for (int phase = 0; phase < bar; phase++)
            {
                float sum = 0; int count = 0;
                for (int i = phase; i < e.Length; i += bar) { sum += e[i]; count++; }
                if (count < 2) continue;
                float score = sum / count - mean;
                if (score > bestScore) { bestScore = score; bestBar = bar; bestPhase = phase; }
            }
        }
        if (bestBar == 0) return (empty, 0);

        var downbeats = new List<TimeSpan>();
        for (int i = bestPhase; i < beats.Count; i += bestBar)
            downbeats.Add(TimeSpan.FromSeconds(beats[i] / fps));
        return (downbeats, bestBar);
    }

    private static float Sq(float v) => v * v;

    private void Validate()
    {
        // EVEN and >= 2, not power-of-two: nothing in this detector is radix-bound. The
        // window (Windows.Hann(FftSize)), the bin loop (FftSize / 2) and the hop are all
        // size-agnostic, so the only constraint is whatever JuceFft accepts — and it is
        // that transform's business to state it, not a second copy of the rule here. Even
        // is this type's own requirement: FftSize / 2 must not silently drop the top bin.
        if (FftSize < 2 || FftSize % 2 != 0)
            throw new ArgumentException($"{nameof(FftSize)} must be an even value >= 2 (was {FftSize}).");
        if (HopSize <= 0)
            throw new ArgumentException($"{nameof(HopSize)} must be > 0 (was {HopSize}).");
        if (!(MinBpm >= MinAllowedBpm) || !float.IsFinite(MinBpm))
            throw new ArgumentException(
                $"{nameof(MinBpm)} must be a finite value >= {MinAllowedBpm} (was {MinBpm}). " +
                "The floor bounds the LOCAL tempo search, which is O(windows * maxLag^2) and " +
                "reaches ~6e10 multiply-adds — hours, uncancellable — at MinBpm = 1 on a " +
                "five-minute track.");
        if (!(MaxBpm > MinBpm) || !float.IsFinite(MaxBpm))
            throw new ArgumentException(
                $"{nameof(MaxBpm)} must be a finite value greater than {nameof(MinBpm)} " +
                $"(was {MaxBpm} vs {MinBpm}).");
    }

    private float[] ComputeSpectralFlux(ReadOnlySpan<float> mono)
    {
        var fft = new JuceFft(FftSize);
        // +1: the last VALID window offset is frames·Hop, because frames·Hop + FftSize
        // <= mono.Length holds by construction — so the bare division was one frame short
        // and dropped the last complete window. At the documented minimum input (4·FftSize)
        // that is 6 frames instead of 7, a 14 % loss of envelope on the shortest legal
        // input, which is also exactly where `maxLag = env.Length / 2` is already the
        // binding constraint that throws "audio is too short".
        int frames = (mono.Length - FftSize) / HopSize + 1;
        var flux = new float[Math.Max(0, frames)];
        var prevMag = new float[FftSize / 2];
        var window  = Windows.Hann(FftSize);
        var re      = new float[FftSize];
        var im      = new float[FftSize];
        long nonFinite = 0;

        for (int f = 0; f < frames; f++)
        {
            int offset = f * HopSize;
            for (int i = 0; i < FftSize; i++)
            {
                // Scrub at the input (see NonFiniteSamples). Note this counts a
                // sample once per window it appears in — windows overlap — so it
                // is a "how bad" signal, not an exact sample census.
                var s = mono[offset + i];
                if (!float.IsFinite(s)) { s = 0f; nonFinite++; }
                re[i] = s * window[i];
                im[i] = 0;
            }
            fft.Forward(re, im);

            float sum = 0;
            for (int k = 0; k < FftSize / 2; k++)
            {
                float mag = MathF.Sqrt(re[k] * re[k] + im[k] * im[k]);
                float d   = mag - prevMag[k];
                if (d > 0) sum += d;
                prevMag[k] = mag;
            }
            flux[f] = sum;
        }
        NonFiniteSamples = nonFinite;
        return flux;
    }

    private static float[] MovingAverage(float[] x, int span)
    {
        var y = new float[x.Length];
        float acc = 0;
        for (int i = 0; i < x.Length; i++)
        {
            acc += x[i];
            if (i >= span) acc -= x[i - span];
            y[i] = acc / Math.Min(span, i + 1);
        }
        return y;
    }
}
