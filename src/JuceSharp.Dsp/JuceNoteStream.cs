namespace JuceSharp.Dsp;

/// <summary>A note-on or note-off event emitted by <see cref="JuceNoteStream"/>.</summary>
/// <param name="Kind">Note-on or note-off.</param>
/// <param name="Midi">MIDI number of the nearest semitone.</param>
/// <param name="NoteName">Sharp note name (C, C#, …).</param>
/// <param name="Octave">Octave in scientific pitch notation (MIDI 60 = C4).</param>
/// <param name="FrequencyHz">Detected fundamental; 0 on a note-off is not reported,
/// the note-off carries the last sustained frequency.</param>
/// <param name="CentsOff">Signed cents from the nearest semitone; 0 on a note-off.</param>
/// <param name="Velocity">sqrt(window energy) scaled and clamped to 0..1; 0 on a note-off.</param>
/// <param name="TimestampSeconds">
/// <b>The START of the analysis window that produced the event</b>, in seconds
/// since construction / <see cref="JuceNoteStream.Reset"/> — not its centre, and
/// not the sample the attack was heard on. An event is therefore up to
/// <see cref="JuceNoteStream.WindowSize"/> samples EARLY against the audio that
/// caused it (46 ms at the default window and 44.1 kHz). See
/// <see cref="JuceNoteStream.LatencySamples"/>.</param>
public readonly record struct JuceNoteEvent(
    JuceNoteEventKind Kind,
    int               Midi,
    string            NoteName,
    int               Octave,
    float             FrequencyHz,
    float             CentsOff,
    float             Velocity,
    double            TimestampSeconds);

/// <summary>Which edge of a note a <see cref="JuceNoteEvent"/> reports.</summary>
public enum JuceNoteEventKind
{
    /// <summary>The player attacked a note: energy crossed the onset threshold and the
    /// YIN pitch was stable across two consecutive frames.</summary>
    On,
    /// <summary>The sounding note ended — released below the hysteresis floor, or
    /// displaced by a retrigger onto a different pitch.</summary>
    Off,
}

/// <summary>
/// Streaming monophonic note detector. Wraps <see cref="JuceChromaticTuner"/>
/// with a hysteresis-based onset/release detector so callers can feed
/// arbitrary-size audio blocks and receive <see cref="JuceNoteEvent"/>s as
/// the player attacks, sustains and releases each note.
///
/// <para>Designed for live guitar/bass input. State machine:</para>
/// <list type="bullet">
/// <item>idle → note-on when energy &gt; <see cref="OnsetEnergyThreshold"/>
///   AND the YIN pitch is stable across two consecutive frames (within
///   <see cref="StablePitchCents"/>).</item>
/// <item>sustained → note-off when energy drops below
///   <see cref="ReleaseEnergyThreshold"/>, OR when pitch changes by more than
///   <see cref="RetriggerCents"/> (immediately followed by a new note-on).</item>
/// </list>
/// </summary>
public sealed class JuceNoteStream
{
    private readonly JuceChromaticTuner _tuner;
    private readonly int   _hopSamples;
    private readonly float[] _buffer;
    private int   _bufferFill;
    private double _samplePosition;

    private float _prevFreq;
    private int   _stableCount;
    private int   _currentMidi = int.MinValue;
    private float _currentFreq;

    /// <summary>Energy (mean-square) above which an onset can be declared.</summary>
    public float OnsetEnergyThreshold   { get; init; } = 1e-4f;
    /// <summary>Energy below which the current note is released.</summary>
    public float ReleaseEnergyThreshold { get; init; } = 2e-5f;
    /// <summary>Pitch must stay within +/- this many cents across two
    /// consecutive frames to count as a stable onset.</summary>
    public float StablePitchCents       { get; init; } = 40f;
    /// <summary>Once a note is on, a pitch change beyond this many cents
    /// retriggers (off + on).</summary>
    public float RetriggerCents         { get; init; } = 80f;

    /// <summary>Construct a streaming detector. The internal hop is
    /// <c>windowSize/2</c> by default, giving good temporal resolution
    /// without re-detecting from scratch each block.</summary>
    /// <param name="sampleRate">Sample rate of the audio pushed through
    /// <see cref="Push"/>, in Hz.</param>
    /// <param name="windowSize">Analysis window length in samples.</param>
    /// <param name="minFreq">Lowest fundamental the detector will report, in Hz.</param>
    /// <param name="maxFreq">Highest fundamental the detector will report, in Hz.</param>
    /// <param name="hopSamples">Samples between analysis windows. Must be in
    /// <c>1..windowSize</c> — the same bound
    /// <see cref="JuceTunerStream"/> enforces, and for the same two reasons:
    /// an explicit <c>0</c> is not <c>null</c>, so it used to pass straight
    /// through and make <see cref="Push"/> spin FOREVER on the caller's thread
    /// (<c>retain</c> becomes the whole buffer, the next <c>copy</c> is 0, the
    /// loop guard never fires and <c>ProcessOneWindow</c> runs without end,
    /// appending note events); and a hop GREATER than the window makes
    /// <c>retain</c> negative, skips the guarded <c>Array.Copy</c>, drives
    /// <c>_bufferFill</c> negative and indexes the buffer out of range.</param>
    public JuceNoteStream(int sampleRate, int windowSize = 2048, int? hopSamples = null,
                          float minFreq = 50f, float maxFreq = 1500f)
    {
        _tuner      = new JuceChromaticTuner(sampleRate, windowSize, minFreq, maxFreq);
        _hopSamples = hopSamples ?? Math.Max(256, windowSize / 2);
        if (_hopSamples < 1 || _hopSamples > _tuner.WindowSize)
            throw new ArgumentOutOfRangeException(nameof(hopSamples),
                $"Hop must be in 1..{_tuner.WindowSize} (the analysis window); was {_hopSamples}.");
        _buffer     = new float[windowSize];
    }

    /// <summary>
    /// Lifetime count of non-finite (NaN / ±Inf) input samples replaced with
    /// silence at <see cref="Push"/>.
    /// <para>Unscrubbed this class LATCHES. <c>energy</c> becomes NaN, so
    /// <c>energy &lt; ReleaseEnergyThreshold</c> is false and the current note is
    /// never released; if the NaN reaches <c>_currentFreq</c>, the drift test
    /// <c>|1200·log2(f/_currentFreq)| &gt; RetriggerCents</c> is false forever and
    /// <see cref="ProcessOneWindow"/> returns early on every window from then on —
    /// the stream goes permanently silent while holding one note on.</para>
    /// </summary>
    public long NonFiniteSamples { get; private set; }

    /// <summary>Sample rate in Hz of the audio pushed in, taken from the underlying
    /// tuner.</summary>
    public int SampleRate => _tuner.SampleRate;

    /// <summary>YIN analysis window in samples, taken from the underlying tuner. Also the
    /// stream's report delay — see <c>LatencySamples</c>.</summary>
    public int WindowSize => _tuner.WindowSize;

    /// <summary>Advance in samples between successive detection frames, and therefore the
    /// finest granularity at which an onset time can land.</summary>
    public int HopSamples => _hopSamples;

    /// <summary>
    /// The stream's inherent REPORT delay, in samples: how far past a reported
    /// <see cref="JuceNoteEvent.TimestampSeconds"/> the stream must have advanced
    /// before that event comes out of <see cref="Push"/>. Equal to
    /// <see cref="WindowSize"/>.
    /// <para><b>Every event is timestamped at the analysis window's START</b>, not
    /// at its centre and not at the sample the attack was heard on — so an event
    /// is up to <see cref="WindowSize"/> samples EARLY against the audio that
    /// produced it (46 ms at the default 2048-sample window and 44.1 kHz). That is
    /// the convention, it is not a bug, and it was undocumented: a caller aligning
    /// these to its own input timeline needs to know which end of the window the
    /// number names. The window starting at sample <c>w</c> is judged once
    /// <c>w + WindowSize</c> samples have been consumed and reports
    /// <c>w / SampleRate</c>, hence this constant. Pushing in blocks larger than
    /// <see cref="HopSamples"/> adds the block's own quantisation on top; this is
    /// the floor. Same shape as
    /// <see cref="JuceOnsetDetector.LatencySamples"/>, which additionally offsets
    /// the reported position toward the perceptual attack — this type does
    /// not.</para>
    /// </summary>
    public int LatencySamples => WindowSize;

    /// <summary>Reset internal state. Emits no events.</summary>
    public void Reset()
    {
        _bufferFill     = 0;
        _samplePosition = 0;
        _prevFreq       = 0;
        _stableCount    = 0;
        _currentMidi    = int.MinValue;
        _currentFreq    = 0;
    }

    /// <summary>Push a block of mono samples. Returns the list of events
    /// produced by this block (may be empty). The list is freshly allocated
    /// per call so callers can keep the references.</summary>
    public IReadOnlyList<JuceNoteEvent> Push(ReadOnlySpan<float> samples)
    {
        if (samples.IsEmpty) return Array.Empty<JuceNoteEvent>();
        List<JuceNoteEvent>? events = null;

        int srcIdx = 0;
        while (srcIdx < samples.Length)
        {
            // Slide the window: keep the most recent (WindowSize - hop)
            // samples and append `hop` new ones at a time.
            int copy = Math.Min(_buffer.Length - _bufferFill, samples.Length - srcIdx);
            // Scrub at the public boundary (see NonFiniteSamples).
            var chunk = samples.Slice(srcIdx, copy);
            for (int i = 0; i < copy; i++)
            {
                var s = chunk[i];
                if (float.IsFinite(s)) { _buffer[_bufferFill + i] = s; }
                else { _buffer[_bufferFill + i] = 0f; NonFiniteSamples++; }
            }
            _bufferFill += copy;
            srcIdx      += copy;

            if (_bufferFill < _buffer.Length) break;

            ProcessOneWindow(ref events);

            // Slide by hop: discard the oldest hop samples and shift the rest.
            int retain = _buffer.Length - _hopSamples;
            if (retain > 0)
                Array.Copy(_buffer, _hopSamples, _buffer, 0, retain);
            _bufferFill = retain;
            _samplePosition += _hopSamples;
        }
        return (IReadOnlyList<JuceNoteEvent>?)events ?? Array.Empty<JuceNoteEvent>();
    }

    private void ProcessOneWindow(ref List<JuceNoteEvent>? events)
    {
        float energy = 0;
        for (int i = 0; i < _buffer.Length; i++) energy += _buffer[i] * _buffer[i];
        energy /= _buffer.Length;

        var reading = _tuner.Detect(_buffer);

        // Release branch.
        if (_currentMidi != int.MinValue && energy < ReleaseEnergyThreshold)
        {
            EmitOff(ref events);
            return;
        }

        if (reading is not { } r)
        {
            _prevFreq    = 0;
            _stableCount = 0;
            return;
        }

        // If we're already sustaining a note and the pitch drifts past
        // RetriggerCents, emit off+on (e.g. user slides to a new fret).
        if (_currentMidi != int.MinValue && _currentFreq > 0)
        {
            float drift = PitchMath.CentsF(r.FrequencyHz, _currentFreq);
            if (MathF.Abs(drift) > RetriggerCents)
            {
                EmitOff(ref events);
                // …and fall through into onset evaluation WITH the stability state a
                // retrigger already earned. EmitOff clears _prevFreq and _stableCount, so
                // the onset branch below failed its `_prevFreq > 0` test, left _stableCount
                // at 0, and the `_stableCount >= 1` gate could not fire — the class doc's
                // "immediately followed by a new note-on" was one hop late at best. A
                // retrigger is its own evidence: the note it displaced was confirmed, and
                // THIS reading is the one that displaced it.
                _prevFreq    = r.FrequencyHz;
                _stableCount = 1;
            }
            else
            {
                // Same note continuing; track latest freq for drift compare.
                _currentFreq = r.FrequencyHz;
                return;
            }
        }

        // Onset branch: require two consecutive frames within StablePitchCents.
        if (_prevFreq > 0)
        {
            float dCents = PitchMath.CentsF(r.FrequencyHz, _prevFreq);
            if (MathF.Abs(dCents) <= StablePitchCents) _stableCount++;
            else _stableCount = 0;
        }
        _prevFreq = r.FrequencyHz;

        if (_stableCount >= 1 && energy >= OnsetEnergyThreshold && _currentMidi == int.MinValue)
        {
            float midi = PitchMath.HzToMidiF(r.FrequencyHz);
            int   m    = (int)MathF.Round(midi);
            _currentMidi = m;
            _currentFreq = r.FrequencyHz;
            var velocity = Math.Clamp(MathF.Sqrt(energy) * 4f, 0f, 1f);
            (events ??= new()).Add(new JuceNoteEvent(
                JuceNoteEventKind.On, m, r.NoteName, r.Octave,
                r.FrequencyHz, r.CentsOff, velocity,
                _samplePosition / SampleRate));
        }
    }

    private void EmitOff(ref List<JuceNoteEvent>? events)
    {
        if (_currentMidi == int.MinValue) return;
        var midi = _currentMidi;
        // PitchMath, not a local copy. This method used to derive the pitch class and
        // octave inline (`midi / 12 - 1`) and resolve the name through a private
        // 12-arm switch — two more of the re-typed copies the shared type exists to
        // retire, and the octave one carried the toward-zero truncation bug
        // PitchMath.Octave now documents. The siblings were migrated in the same wave;
        // this one was missed.
        (events ??= new()).Add(new JuceNoteEvent(
            JuceNoteEventKind.Off, midi, PitchMath.NoteName(midi), PitchMath.Octave(midi),
            _currentFreq, 0f, 0f, _samplePosition / SampleRate));
        _currentMidi = int.MinValue;
        _currentFreq = 0;
        _prevFreq    = 0;
        _stableCount = 0;
    }
}
