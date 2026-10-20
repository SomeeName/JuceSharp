namespace JuceSharp.Dsp;

/// <summary>
/// One emitted frame of <see cref="JuceTunerStream"/>: everything a tuner UI
/// needs to draw a needle, in the form it wants to bind to.
/// </summary>
/// <param name="NoteName">Sharp name of the HELD note (C, C#, …) — the note the
/// display is currently showing, which is not necessarily the nearest semitone
/// to <paramref name="FrequencyHz"/> while a note change is still inside the
/// hysteresis window.</param>
/// <param name="Octave">Octave of the held note in scientific pitch notation
/// (MIDI 60 = C4).</param>
/// <param name="MidiNote">MIDI number of the held note.</param>
/// <param name="CentsOff">Signed cents of the smoothed frequency against the
/// HELD note — not against the nearest semitone. That is the needle deflection:
/// during a note change it grows past ±50 for a few frames rather than snapping
/// to zero on a note the UI is not showing yet.</param>
/// <param name="FrequencyHz">Median-smoothed fundamental in Hz.</param>
/// <param name="Confidence">The underlying <see cref="JuceTunerReading.Confidence"/>
/// of the newest reading (NOT smoothed).</param>
/// <param name="Clamped">The reading sat on the tuner's lag ceiling (see
/// <see cref="JuceTunerReading.Clamped"/>) or within
/// <see cref="JuceTunerStream.ClampedMargin"/> of
/// <see cref="JuceChromaticTuner.EffectiveMinFrequency"/>. A clamped frame is a
/// floor artefact, never a measurement — and is therefore NEVER
/// <paramref name="IsStable"/>.</param>
/// <param name="IsStable">The held note has been agreed on by
/// <see cref="JuceTunerStream.StableFrames"/> consecutive readings, the newest
/// reading cleared <see cref="JuceTunerStream.MinStableConfidence"/>, and the
/// frame is not <paramref name="Clamped"/>. A UI should only commit to a
/// "you are in tune" indication on a stable frame.</param>
/// <param name="TimeSeconds">
/// <b>The START of the analysis window that produced this frame</b>, in seconds since
/// construction or <see cref="JuceTunerStream.Reset"/> — the same convention
/// <see cref="JuceNoteEvent.TimestampSeconds"/> and <see cref="JuceLeadFrame.TimeSeconds"/>
/// carry, and derived from the stream position, so it advances by exactly
/// <see cref="JuceTunerStream.HopSize"/> samples per emitted frame whatever block sizes the
/// caller pushes. A frame is therefore up to <c>WindowSize</c> samples EARLY against the
/// audio that caused it.
/// <para>It exists because the sibling frame types have it and this one did not: a caller
/// logging a tuning session, or aligning a needle trace to a recording, had nothing but
/// <see cref="JuceTunerStream.FramesEmitted"/> and had to re-derive the hop itself.</para>
/// </param>
public readonly record struct JuceTunerFrame(
    string NoteName,
    int    Octave,
    int    MidiNote,
    float  CentsOff,
    float  FrequencyHz,
    float  Confidence,
    bool   Clamped,
    bool   IsStable,
    double TimeSeconds);

/// <summary>
/// UI-grade streaming wrapper over <see cref="JuceChromaticTuner"/>.
///
/// <para><b>Why this exists.</b> <c>Detect</c> is a one-shot over a full window:
/// a caller that wants a live needle has to own the ring, the emit rate, the
/// octave-slip filter and the note hysteresis itself — and every caller writes
/// those four differently. This type owns them once. Push arbitrary-size mono
/// blocks; a frame is produced every <see cref="HopSize"/> samples once the
/// first full window has arrived.</para>
///
/// <para>Three filters sit between the raw YIN reading and
/// <see cref="Latest"/>:</para>
/// <list type="bullet">
/// <item>a 3-tap MEDIAN on the frequency (the shape
/// <c>JuceLeadTracker.Smooth</c> uses) — a single-frame octave slip is a
/// classic YIN failure and a median removes it where an average would smear
/// it across three frames;</item>
/// <item>HYSTERESIS on the note: the displayed note only changes after
/// <see cref="NoteChangeFrames"/> consecutive readings agree on the new one, or
/// immediately if the reading is more than <see cref="ImmediateChangeCents"/>
/// from the held note (an unambiguous jump should not wait);</item>
/// <item>a RELEASE hold: unvoiced frames do not blank the display until
/// <see cref="ReleaseFrames"/> of them in a row, so a pick-hand mute does not
/// make the needle flicker.</item>
/// </list>
///
/// <para><b>Allocation-free after construction</b> (pinned by
/// <c>JuceTunerStreamTests.Push_DoesNotAllocate</c>) and non-finite input is
/// scrubbed at the boundary with a counter — the same rule
/// <see cref="JuceOnsetDetector.NonFiniteSamples"/> and the native
/// <c>DspChain::process</c> follow. Unscrubbed, one NaN makes the window's
/// energy NaN, <c>energy / WindowSize &lt; 1e-6f</c> false, and every YIN
/// difference NaN — so the tuner reports nothing for the whole window and says
/// nothing about why.</para>
/// </summary>
public sealed class JuceTunerStream
{
    private const int NoNote = int.MinValue;

    /// <summary>The tuner doing the work. Supplied by the caller so the
    /// frequency floor and window are the CALLER's choice (a bass tuner needs
    /// <c>minFreq: 25f</c> and a 4096-sample window; see
    /// <see cref="JuceChromaticTuner.EffectiveMinFrequency"/>).</summary>
    public JuceChromaticTuner Tuner { get; }

    /// <summary>Analysis window, in samples — <see cref="JuceChromaticTuner.WindowSize"/>.</summary>
    public int WindowSize => Tuner.WindowSize;

    /// <summary>Samples between emitted frames. Fixed at construction: the emit
    /// rate must not depend on the caller's block size.</summary>
    public int HopSize { get; }

    /// <summary>Consecutive agreeing readings a NEW note needs before it is
    /// displayed. 1 disables the hysteresis.</summary>
    public int NoteChangeFrames { get; init; } = 3;

    /// <summary>A reading further than this from the held note switches
    /// immediately, bypassing <see cref="NoteChangeFrames"/>. Default 250
    /// cents: more than two semitones is never a mis-read of the held note, but
    /// an ordinary whole-tone move (200 cents) still goes through the
    /// hysteresis.
    /// <para><b>An exact-octave jump is EXEMPT unless
    /// <see cref="OctaveJumpsBypassHysteresis"/> is set</b> — see that
    /// property. 250 cents was chosen against "a mis-read of the held note",
    /// and a YIN octave slip is 1200 cents, i.e. the one failure this type
    /// advertises filtering went straight through the bypass.</para></summary>
    public float ImmediateChangeCents { get; init; } = 250f;

    /// <summary>
    /// Let an exact-octave jump take the <see cref="ImmediateChangeCents"/> fast
    /// path too. Default <c>false</c>, which is the fix.
    /// <para><b>Why the default changed.</b> A YIN octave slip is 1200 cents —
    /// far past <see cref="ImmediateChangeCents"/> — so it switched the displayed
    /// note IMMEDIATELY and the recovery frame switched it immediately back. The
    /// 3-tap median removes a SINGLE-frame slip; two consecutive slipped readings
    /// are routine on a low E (see <see cref="JuceTunerReading.Clamped"/>'s
    /// measurements) and pass the median intact, so the class doc's "median +
    /// hysteresis is the octave-slip defence" was false for exactly the case it
    /// named. An exact multiple of twelve semitones now goes through
    /// <see cref="NoteChangeFrames"/> like any ordinary move; every other large
    /// jump still switches at once. Set this <c>true</c> only for a source where
    /// real octave leaps are common and slips are not.</para>
    /// </summary>
    public bool OctaveJumpsBypassHysteresis { get; init; }

    /// <summary>Consecutive readings agreeing with the HELD note before
    /// <see cref="JuceTunerFrame.IsStable"/> is set.</summary>
    public int StableFrames { get; init; } = 3;

    /// <summary>Minimum <see cref="JuceTunerReading.Confidence"/> for a stable
    /// frame.</summary>
    public float MinStableConfidence { get; init; } = 0.5f;

    /// <summary>Consecutive unvoiced windows before <see cref="Latest"/> goes
    /// null and the note tracking resets.</summary>
    public int ReleaseFrames { get; init; } = 12;

    /// <summary>How close to <see cref="JuceChromaticTuner.EffectiveMinFrequency"/>
    /// still counts as clamped. 1.05 ≈ one twelfth of a semitone of slack — the
    /// same margin <c>JuceGuitarAnalyzer</c> uses for its legato floor.</summary>
    public float ClampedMargin { get; init; } = 1.05f;

    /// <summary>
    /// The most recently emitted frame, or <c>null</c> before the first one /
    /// after a release. Bind a UI to this.
    /// <para><b>This is the one member that is safe to read from a thread other
    /// than the one calling <see cref="Push"/>, and it is safe because of the
    /// SEQLOCK below — not by accident.</b> <see cref="JuceTunerFrame"/> is an
    /// eight-field record struct (~40 bytes), so a plain auto-property assignment
    /// is several unordered stores and the designed use — a capture thread
    /// pushing while the UI thread reads — could observe <c>NoteName</c> from one
    /// frame with <c>CentsOff</c>/<c>MidiNote</c> from another: a needle pointing
    /// at a note it is not naming, which reads to a user as a detection bug. A
    /// reader that loses the race <see cref="LatestReadTries"/> times in a row
    /// reports <c>null</c> (no frame right now) rather than a torn one. There is
    /// exactly ONE writer — <see cref="Push"/> — and this stays allocation-free,
    /// which is why it is a seqlock and not an immutable published
    /// reference.</para>
    /// </summary>
    public JuceTunerFrame? Latest
    {
        get
        {
            for (var tries = 0; tries < LatestReadTries; tries++)
            {
                var before = Volatile.Read(ref _latestSeq);
                if ((before & 1) != 0) continue;          // a write is in progress
                var has   = _hasLatest;
                var frame = _latestFrame;
                Interlocked.MemoryBarrier();
                if (Volatile.Read(ref _latestSeq) == before) return has ? frame : null;
            }
            return null;
        }
    }

    /// <summary>How many times a <see cref="Latest"/> reader retries a torn read
    /// before reporting <c>null</c>. The writer's critical section is two field
    /// stores, so losing four in a row means the reader was descheduled, not that
    /// the publisher is slow.</summary>
    private const int LatestReadTries = 4;

    private int            _latestSeq;      // even = stable, odd = write in progress
    private JuceTunerFrame _latestFrame;
    private bool           _hasLatest;

    // The single writer. Odd-then-even around the payload stores is the
    // ParamMailbox pattern the native rig uses for exactly this shape; a
    // two-deep buffer flip would be ABA-unsafe and is banned repo-wide.
    private void PublishLatest(JuceTunerFrame? frame)
    {
        Volatile.Write(ref _latestSeq, _latestSeq + 1);
        Interlocked.MemoryBarrier();
        _hasLatest = frame.HasValue;
        if (frame is { } f) _latestFrame = f;
        Interlocked.MemoryBarrier();
        Volatile.Write(ref _latestSeq, _latestSeq + 1);
    }

    /// <summary>Total samples pushed since construction or <see cref="Reset"/>.</summary>
    public long StreamPosition { get; private set; }

    /// <summary>Frames emitted since construction or <see cref="Reset"/>.</summary>
    public long FramesEmitted { get; private set; }

    /// <summary>Lifetime count of non-finite input samples replaced with silence
    /// at <see cref="Push"/>. <b>Not</b> cleared by <see cref="Reset"/> — a
    /// health counter, not per-session state, exactly like
    /// <see cref="JuceOnsetDetector.NonFiniteSamples"/>.</summary>
    public long NonFiniteSamples { get; private set; }

    private readonly float[] _win;
    private readonly float[] _median = new float[3];

    private int _fill;
    private int _medianCount;
    private int _medianHead;
    private int _heldMidi = NoNote;
    private int _agree;
    private int _candMidi = NoNote;
    private int _candCount;
    private int _silent;

    // Start sample of the window ProcessWindow is about to judge; advances by HopSize per
    // emitted window. See JuceTunerFrame.TimeSeconds.
    private long _windowStart;

    /// <summary>Wrap an existing tuner.</summary>
    /// <param name="tuner">The tuner to drive. Not copied; do not call
    /// <see cref="JuceChromaticTuner.Detect"/> on it concurrently.</param>
    /// <param name="hopSize">Samples between frames; default
    /// <c>WindowSize / 4</c> (≈86 frames/s for a 2048-sample window at
    /// 44.1 kHz). Must be in <c>1..WindowSize</c>.</param>
    public JuceTunerStream(JuceChromaticTuner tuner, int hopSize = 0)
    {
        Tuner = tuner ?? throw new ArgumentNullException(nameof(tuner));
        HopSize = hopSize == 0 ? Math.Max(1, tuner.WindowSize / 4) : hopSize;
        if (HopSize < 1 || HopSize > tuner.WindowSize)
            throw new ArgumentOutOfRangeException(nameof(hopSize),
                $"Hop must be in 1..{tuner.WindowSize} (the analysis window); was {hopSize}.");
        _win = new float[tuner.WindowSize];
    }

    /// <summary>Convenience: build the tuner too.</summary>
    public JuceTunerStream(int sampleRate, int windowSize = 2048,
                           float minFreq = 50f, float maxFreq = 1500f, int hopSize = 0)
        : this(new JuceChromaticTuner(sampleRate, windowSize, minFreq, maxFreq), hopSize)
    {
    }

    /// <summary>Drop all state: the window, the median, the held note and the
    /// stream position. Leaves <see cref="NonFiniteSamples"/> alone.</summary>
    public void Reset()
    {
        Array.Clear(_win);
        Array.Clear(_median);
        _fill = _medianCount = _medianHead = 0;
        _agree = _candCount = _silent = 0;
        _heldMidi = _candMidi = NoNote;
        StreamPosition = 0;
        FramesEmitted  = 0;
        _windowStart   = 0;
        PublishLatest(null);
    }

    /// <summary>
    /// Push a block of mono samples. Returns <c>true</c> if at least one frame
    /// was emitted, in which case <see cref="Latest"/> is the newest of them.
    /// Allocation-free.
    /// <para><b>Cost scales with the BLOCK, and only the last frame survives.</b>
    /// A block spanning <c>n</c> hops runs a full YIN detection <c>n</c> times,
    /// synchronously on the calling thread, and every frame but the last is
    /// overwritten in <see cref="Latest"/> — a 1 s push at 44.1 kHz with the
    /// default 512-sample hop is 86 <see cref="JuceChromaticTuner.Detect"/> calls
    /// for one usable reading. That is DELIBERATE and not a bug: the hysteresis,
    /// the release hold and the 3-tap median are all per-FRAME state machines, so
    /// skipping intermediate hops would change the filtering as a function of the
    /// caller's block size — which is the exact coupling <see cref="HopSize"/>
    /// being fixed at construction exists to remove. Feed it live-sized blocks
    /// (a device callback's worth); do not use it to sweep an offline file.</para>
    /// </summary>
    public bool Push(ReadOnlySpan<float> samples)
    {
        if (samples.IsEmpty) return false;
        StreamPosition += samples.Length;

        var emitted = false;
        var src = 0;
        while (src < samples.Length)
        {
            var copy = Math.Min(_win.Length - _fill, samples.Length - src);
            var chunk = samples.Slice(src, copy);
            for (var i = 0; i < copy; i++)
            {
                var s = chunk[i];
                if (float.IsFinite(s)) { _win[_fill + i] = s; }
                else { _win[_fill + i] = 0f; NonFiniteSamples++; }
            }
            _fill += copy;
            src   += copy;
            if (_fill < _win.Length) break;

            emitted |= ProcessWindow();

            var retain = _win.Length - HopSize;
            if (retain > 0) Array.Copy(_win, HopSize, _win, 0, retain);
            _fill = retain;
            _windowStart += HopSize;
        }
        return emitted;
    }

    private bool ProcessWindow()
    {
        if (Tuner.Detect(_win) is not { } reading)
        {
            _silent++;
            if (_silent >= Math.Max(1, ReleaseFrames))
            {
                PublishLatest(null);
                _heldMidi = _candMidi = NoNote;
                _agree = _candCount = _medianCount = _medianHead = 0;
            }
            return false;
        }
        _silent = 0;

        var freq  = PushMedian(reading.FrequencyHz);
        var midi  = PitchMath.HzToMidiF(freq);
        var near  = (int)MathF.Round(midi);

        if (_heldMidi == NoNote)
        {
            Hold(near);
        }
        else if (near == _heldMidi)
        {
            if (_agree < int.MaxValue) _agree++;
            _candMidi  = NoNote;
            _candCount = 0;
        }
        else
        {
            if (near == _candMidi) _candCount++;
            else { _candMidi = near; _candCount = 1; }

            var centsFromHeld = (midi - _heldMidi) * 100f;
            // The integer distance, not the cents, decides "is this an octave":
            // `near` is already the rounded semitone, so an exact multiple of 12
            // is crisp where a cents tolerance would need tuning.
            var semitones  = Math.Abs(near - _heldMidi);
            var octaveSlip = !OctaveJumpsBypassHysteresis && semitones >= 12 && semitones % 12 == 0;
            if (_candCount >= Math.Max(1, NoteChangeFrames) ||
                (!octaveSlip && MathF.Abs(centsFromHeld) > ImmediateChangeCents))
                Hold(near);
            else
                _agree = 0;   // the display is showing a note the audio disagrees with.
        }

        // Cents against the HELD note, so the needle never contradicts the name.
        var cents   = (midi - _heldMidi) * 100f;
        var clamped = reading.Clamped || freq <= Tuner.EffectiveMinFrequency * ClampedMargin;
        var stable  = !clamped
                      && _agree >= Math.Max(1, StableFrames)
                      && reading.Confidence >= MinStableConfidence;

        PublishLatest(new JuceTunerFrame(
            PitchMath.NoteName(_heldMidi), PitchMath.Octave(_heldMidi), _heldMidi,
            cents, freq, reading.Confidence, clamped, stable,
            // The WINDOW START, not StreamPosition: Push advances StreamPosition by the
            // whole block before processing any of it, so it names the end of the caller's
            // block and not the audio this frame was computed from.
            _windowStart / (double)Tuner.SampleRate));
        FramesEmitted++;
        return true;
    }

    private void Hold(int midiNote)
    {
        _heldMidi  = midiNote;
        _agree     = 1;
        _candMidi  = NoNote;
        _candCount = 0;
    }

    // 3-tap median of the last three voiced readings. Below three readings the
    // newest value passes through unchanged: a median of one or two values is
    // not a filter, and pretending otherwise would delay the first frame.
    private float PushMedian(float hz)
    {
        _median[_medianHead] = hz;
        _medianHead = (_medianHead + 1) % 3;
        if (_medianCount < 3) _medianCount++;
        if (_medianCount < 3) return hz;

        float a = _median[0], b = _median[1], c = _median[2];
        return MathF.Max(MathF.Min(a, b), MathF.Min(MathF.Max(a, b), c));
    }
}
