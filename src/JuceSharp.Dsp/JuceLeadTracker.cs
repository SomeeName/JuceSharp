namespace JuceSharp.Dsp;

/// <summary>Playing technique inferred from the recent f0 trajectory.</summary>
public enum JuceLeadTechnique
{
    /// <summary>Nothing was playing: the block's RMS sat below the silence threshold.</summary>
    Silent,
    /// <summary>A held note whose pitch stayed inside the steady tolerance.</summary>
    Steady,
    /// <summary>A string bend: a sustained monotonic pitch rise inside one note.</summary>
    Bend,
    /// <summary>A slide: pitch moved far enough, fast enough, to have crossed frets.</summary>
    Slide,
    /// <summary>Vibrato: the pitch oscillated about a centre rather than travelling.</summary>
    Vibrato,
    /// <summary>Audio was present but no pitch could be resolved — unvoiced, or too noisy
    /// for YIN to lock.</summary>
    None,
}

/// <summary>One block's lead-tracker result: absolute time, fractional MIDI
/// (null = unvoiced), and the technique inferred from the recent trajectory.</summary>
public readonly record struct JuceLeadFrame(double TimeSeconds, float? Midi, JuceLeadTechnique Technique);

/// <summary>
/// Monophonic lead tracker — the expressive half of the dual pipeline (the
/// chord verifier answers yes/no per note; solos also need HOW the note was
/// played). A causal per-block YIN pitch tracker (reusing
/// <see cref="JuceChromaticTuner"/>) whose f0 vector over ~0.35 s is classified
/// into bend / slide / vibrato / steady. Ported from the NoteDetection
/// testbed's <c>lead_track.py</c>; causal and block-driven, so it runs
/// alongside the verifier in live play.
/// </summary>
public sealed class JuceLeadTracker
{
    /// <summary>Sample rate in Hz of the blocks pushed in; the axis every reported
    /// <c>TimeSeconds</c> is on.</summary>
    public int   SampleRate     { get; }

    /// <summary>Expected block length in samples. One <see cref="ProcessBlock"/> call
    /// yields one <see cref="JuceLeadFrame"/>, so this sets the tracker's output rate.</summary>
    public int   BlockSize      { get; }

    /// <summary>YIN analysis window in samples. It slides by one block per call, and its
    /// width sets the lowest pitch that can be resolved at all.</summary>
    public int   WindowSize     { get; }

    /// <summary>RMS below which a block is reported as
    /// <see cref="JuceLeadTechnique.Silent"/> without running pitch detection.</summary>
    public float SilenceRms     { get; init; } = 1e-3f;

    /// <summary>Total pitch excursion in semitones over the trajectory that marks a
    /// monotonic rise as a bend rather than drift. Defaults to 0.7.</summary>
    public float BendSemitones  { get; init; } = 0.7f;

    /// <summary>Total pitch excursion in semitones over the trajectory that marks the
    /// move as a slide rather than a bend — roughly two frets. Defaults to 1.8.</summary>
    public float SlideSemitones { get; init; } = 1.8f;

    /// <summary>Minimum oscillation half-range in semitones before a wobble counts as
    /// vibrato rather than an unsteady hand. Defaults to 0.15.</summary>
    public float VibratoDepth   { get; init; } = 0.15f;  // min osc half-range for vibrato

    /// <summary>Pitch spread in semitones a trajectory must stay inside to be called
    /// <see cref="JuceLeadTechnique.Steady"/>. Defaults to 0.2.</summary>
    public float SteadySemitones{ get; init; } = 0.2f;

    private readonly JuceChromaticTuner _tuner;
    private readonly float[] _window;
    private readonly int _trajLen;
    private readonly Queue<float?> _traj;
    private long _pos;

    /// <summary>
    /// Build the YIN tracker, its sliding window and the f0 trajectory ring for one
    /// analysis thread, then <see cref="Reset"/> it.
    /// </summary>
    /// <param name="sampleRate">Sample rate in Hz of the audio to be tracked.</param>
    /// <param name="blockSize">Samples per <see cref="ProcessBlock"/> call.</param>
    /// <param name="windowSize">YIN window in samples; at least 256, and the real limit on
    /// how low a note can be resolved.</param>
    /// <param name="fMin">Lowest fundamental in Hz to search for.</param>
    /// <param name="fMax">Highest fundamental in Hz to search for.</param>
    /// <param name="trajSeconds">How much recent pitch history the technique classifier
    /// looks at; ~0.35 s by default.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="windowSize"/> is below 256.</exception>
    public JuceLeadTracker(
        int sampleRate = 44100, int blockSize = 1024, int windowSize = 2048,
        float fMin = 65f, float fMax = 1320f, double trajSeconds = 0.35)
    {
        if (windowSize < 256) throw new ArgumentOutOfRangeException(nameof(windowSize));
        SampleRate = sampleRate;
        BlockSize = blockSize;
        WindowSize = windowSize;
        _tuner = new JuceChromaticTuner(sampleRate, windowSize, fMin, fMax);
        _window = new float[windowSize];
        _trajLen = Math.Max(4, (int)Math.Round(trajSeconds * sampleRate / blockSize));
        _traj = new Queue<float?>(_trajLen);
        Reset();
    }

    /// <summary>Clear the analysis window, the pitch trajectory and the stream position,
    /// so the next block is treated as the start of a new take. Call it after a seek or a
    /// transport stop, or the tracker classifies across the discontinuity.</summary>
    public void Reset()
    {
        Array.Clear(_window);
        _traj.Clear();
        _pos = 0;
    }

    /// <summary>
    /// Lifetime count of non-finite (NaN / ±Inf) input samples replaced with
    /// silence at <see cref="ProcessBlock"/>. Unscrubbed, a NaN makes
    /// <c>rms &gt; SilenceRms</c> false for the whole window it sits in, so the
    /// tracker reports <see cref="JuceLeadTechnique.Silent"/> through a note
    /// that was played — and does so with no indication anything went wrong.
    /// </summary>
    public long NonFiniteSamples { get; private set; }

    /// <summary>
    /// Slide one block into the analysis window, run YIN on it, append the result to the
    /// trajectory and classify the recent trajectory into a technique. Causal — it never
    /// looks ahead — so it can run live alongside the chart verifier. Non-finite input
    /// samples are replaced with silence and counted in <see cref="NonFiniteSamples"/>.
    /// </summary>
    /// <param name="block">The next <see cref="BlockSize"/> mono samples.</param>
    /// <returns>Absolute time, fractional MIDI pitch (<c>null</c> when unvoiced) and the
    /// inferred technique for this block.</returns>
    public JuceLeadFrame ProcessBlock(ReadOnlySpan<float> block)
    {
        // Slide the analysis window and append the new block (causal), scrubbing
        // at the public boundary (see NonFiniteSamples).
        if (block.Length >= WindowSize)
        {
            Scrub(block[^WindowSize..], _window.AsSpan());
        }
        else
        {
            Array.Copy(_window, block.Length, _window, 0, WindowSize - block.Length);
            Scrub(block, _window.AsSpan(WindowSize - block.Length));
        }
        double t = (double)_pos / SampleRate;
        _pos += block.Length;

        float rms = 0;
        for (int i = 0; i < WindowSize; i++) rms += _window[i] * _window[i];
        rms = MathF.Sqrt(rms / WindowSize);

        float? midi = null;
        var audiblePresent = rms > SilenceRms;
        if (audiblePresent)
        {
            var r = _tuner.Detect(_window);
            if (r is { } reading) midi = PitchMath.HzToMidiF(reading.FrequencyHz);
        }

        if (_traj.Count >= _trajLen) _traj.Dequeue();
        _traj.Enqueue(midi);

        // Silent means SILENT. `Smooth()` returns null whenever the last <=3 trajectory
        // entries are all unvoiced, which includes a LOUD unvoiced block — a pick scrape,
        // a palm mute, distorted noise — so this arm reported JuceLeadTechnique.Silent
        // ("the block's RMS sat below the silence threshold") for audio that was plainly
        // above it, and JuceLeadTechnique.None ("audio was present but no pitch could be
        // resolved") was reachable only from the span test in Classify. A consumer asking
        // "did the player stop, or am I failing to track them?" got the wrong answer.
        float? smoothed = Smooth();
        var technique = smoothed is null
            ? (audiblePresent ? JuceLeadTechnique.None : JuceLeadTechnique.Silent)
            : Classify();
        return new JuceLeadFrame(t, smoothed, technique);
    }

    private void Scrub(ReadOnlySpan<float> src, Span<float> dst)
    {
        for (var i = 0; i < src.Length; i++)
        {
            var s = src[i];
            if (float.IsFinite(s)) { dst[i] = s; }
            else { dst[i] = 0f; NonFiniteSamples++; }
        }
    }

    // 3-tap median of the last three voiced values — kills single-frame octave slips.
    private float? Smooth()
    {
        Span<float> recent = stackalloc float[3];
        int c = 0;
        int skip = Math.Max(0, _traj.Count - 3);
        int i = 0;
        foreach (var m in _traj)
        {
            if (i++ < skip) continue;
            if (m is { } v) recent[c++] = v;
        }
        if (c == 0) return null;
        recent[..c].Sort();
        return recent[c / 2];
    }

    private JuceLeadTechnique Classify()
    {
        Span<float> seq = stackalloc float[_trajLen];
        int n = 0;
        foreach (var m in _traj) if (m is { } v) seq[n++] = v;
        if (n < Math.Max(4, _trajLen / 2)) return JuceLeadTechnique.Steady;
        var a = seq[..n];

        float min = a[0], max = a[0];
        for (int i = 1; i < n; i++) { min = MathF.Min(min, a[i]); max = MathF.Max(max, a[i]); }
        float span = max - min;

        // Vibrato: oscillation around the trend. Detrend (remove any bend
        // component), then count sign crossings of the residual with a small depth.
        LinFit(a, out float slope, out float intercept);
        float rMin = float.MaxValue, rMax = float.MinValue;
        int crossings = 0, prevSign = 0;
        for (int i = 0; i < n; i++)
        {
            float resid = a[i] - (slope * i + intercept);
            rMin = MathF.Min(rMin, resid);
            rMax = MathF.Max(rMax, resid);
            int s = MathF.Sign(resid) switch { > 0 => 1, < 0 => -1, _ => 0 };
            if (i > 0 && s != prevSign) crossings++;
            prevSign = s;
        }
        float oscDepth = rMax - rMin;
        if (crossings >= 3 && oscDepth >= VibratoDepth && oscDepth <= 1.5f)
            return JuceLeadTechnique.Vibrato;

        // Bend / slide: a mostly-monotonic pitch move.
        float up = 0, down = 0;
        for (int i = 1; i < n; i++)
        {
            float d = a[i] - a[i - 1];
            if (d > 0) up += d; else down -= d;
        }
        bool monotonic = MathF.Max(up, down) >= 0.8f * (up + down + 1e-9f);
        if (span >= BendSemitones && monotonic)
            return span >= SlideSemitones ? JuceLeadTechnique.Slide : JuceLeadTechnique.Bend;

        return span <= SteadySemitones ? JuceLeadTechnique.Steady : JuceLeadTechnique.None;
    }

    private static void LinFit(ReadOnlySpan<float> y, out float slope, out float intercept)
    {
        int n = y.Length;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < n; i++)
        {
            sx += i; sy += y[i]; sxx += (double)i * i; sxy += (double)i * y[i];
        }
        double denom = n * sxx - sx * sx;
        slope = denom == 0 ? 0f : (float)((n * sxy - sx * sy) / denom);
        intercept = (float)((sy - slope * sx) / n);
    }
}
