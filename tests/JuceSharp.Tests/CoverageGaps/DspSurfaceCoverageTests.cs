using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// Managed-DSP public members with no test reference: <see cref="JuceNoteStream"/>,
/// <see cref="JuceOnsetDetector"/> driven directly, <see cref="JuceWaveformPeaks"/>,
/// and the small result records the whole detection surface hands back
/// (<see cref="JuceTunerReading"/>, <see cref="JuceVerifyResult"/>,
/// <see cref="JuceChordReading"/>, <see cref="JuceWaveformPeak"/>,
/// <see cref="JuceLeadFrame"/>, <see cref="JuceNoteEvent"/>,
/// <see cref="JuceOnset"/>).
///
/// <para>
/// Everything here is offline and deterministic: a synthesised signal in, a
/// structural assertion out. No wall clock, no device, no dataset.
/// </para>
/// </summary>
public sealed class DspSurfaceCoverageTests
{
    private const int Sr = 44100;

    private static float[] Sine(double hz, double seconds, float amp = 0.35f, int sr = Sr)
    {
        var n = (int)(seconds * sr);
        var buf = new float[n];
        for (var i = 0; i < n; i++)
            buf[i] = amp * MathF.Sin(2f * MathF.PI * (float)hz * i / sr);
        return buf;
    }

    private static float[] Silence(double seconds, int sr = Sr) => new float[(int)(seconds * sr)];

    // ------------------------------------------------------ JuceNoteStream

    [Fact]
    public void A_note_stream_reports_its_own_geometry()
    {
        var stream = new JuceNoteStream(Sr, windowSize: 2048);

        Assert.Equal(Sr, stream.SampleRate);
        Assert.Equal(2048, stream.WindowSize);
        Assert.True(stream.HopSamples > 0);
        Assert.True(stream.HopSamples <= stream.WindowSize);

        // An explicit hop is honoured rather than re-derived.
        Assert.Equal(256, new JuceNoteStream(Sr, 2048, hopSamples: 256).HopSamples);
    }

    /// <summary>
    /// Silence in, nothing out — the energy gate is the first thing that has
    /// to work, because everything downstream trusts it.
    /// </summary>
    [Fact]
    public void Silence_produces_no_note_events()
    {
        var stream = new JuceNoteStream(Sr);
        Assert.Empty(stream.Push(Silence(1.0)));
    }

    /// <summary>A steady tone must produce exactly one On and, once it stops,
    /// exactly one Off — not one pair per analysis hop.</summary>
    [Fact]
    public void A_sustained_tone_produces_one_note_on_then_one_note_off()
    {
        var stream = new JuceNoteStream(Sr);
        var events = new List<JuceNoteEvent>();

        events.AddRange(stream.Push(Sine(440.0, 0.8)));
        events.AddRange(stream.Push(Silence(0.8)));

        var ons = events.Where(e => e.Kind == JuceNoteEventKind.On).ToArray();
        var offs = events.Where(e => e.Kind == JuceNoteEventKind.Off).ToArray();

        Assert.Single(ons);
        Assert.Single(offs);
        // A440 is MIDI 69. A whole semitone of slack is generous and still
        // catches an octave error, which is the failure that actually happens.
        Assert.InRange(ons[0].Midi, 68, 70);
        Assert.InRange(ons[0].FrequencyHz, 420f, 460f);
        Assert.False(string.IsNullOrWhiteSpace(ons[0].NoteName));
        Assert.True(offs[0].TimestampSeconds > ons[0].TimestampSeconds);
    }

    [Fact]
    public void Reset_forgets_the_note_in_flight()
    {
        var stream = new JuceNoteStream(Sr);
        var started = stream.Push(Sine(440.0, 0.8));
        Assert.Contains(started, e => e.Kind == JuceNoteEventKind.On);

        stream.Reset();

        // Without the reset the silence would close the note; after it there
        // is no note to close.
        Assert.Empty(stream.Push(Silence(0.8)));
    }

    // -------------------------------------------------- JuceOnsetDetector

    [Fact]
    public void An_onset_detector_reports_its_geometry_and_advances_its_stream_position()
    {
        var det = new JuceOnsetDetector(Sr);

        Assert.Equal(Sr, det.SampleRate);
        Assert.Equal(0, det.StreamPosition);
        Assert.Equal(1024, JuceOnsetDetector.FftSize);
        Assert.Equal(256, JuceOnsetDetector.HopSize);

        var block = Silence(0.5);
        det.Push(block);
        Assert.Equal(block.Length, det.StreamPosition);

        det.Push(block);
        Assert.Equal(block.Length * 2, det.StreamPosition);

        det.Reset();
        Assert.Equal(0, det.StreamPosition);
    }

    [Fact]
    public void Silence_has_no_onsets_and_a_hard_attack_does()
    {
        Assert.Empty(new JuceOnsetDetector(Sr).Push(Silence(1.0)));

        var det = new JuceOnsetDetector(Sr);
        var signal = new List<float>();
        signal.AddRange(Silence(0.3));
        signal.AddRange(Sine(440.0, 0.5, amp: 0.6f));
        var onsets = det.Push(signal.ToArray());

        Assert.NotEmpty(onsets);
        var first = onsets[0];
        Assert.True(first.Strength > 0, $"strength was {first.Strength}");
        // The attack is at 0.3 s. The detector reports on hop boundaries and
        // needs a window of history, so allow a generous ±100 ms — the point
        // is that it is not at 0 and not at the end.
        Assert.InRange(first.SamplePosition, (long)(0.2 * Sr), (long)(0.45 * Sr));
        // Positions are monotonic across one Push.
        for (var i = 1; i < onsets.Count; i++)
            Assert.True(onsets[i].SamplePosition >= onsets[i - 1].SamplePosition);
    }

    // --------------------------------------------------- JuceWaveformPeaks

    [Fact]
    public void Waveform_peaks_bracket_the_signal_they_summarise()
    {
        var mono = Sine(220.0, 1.0, amp: 0.5f, sr: Sr);
        var peaks = JuceWaveformPeaks.Build(mono, channels: 1, sampleRate: Sr, bucketMilliseconds: 10.0);

        Assert.NotEmpty(peaks);
        // 1 s at 10 ms buckets, ±1 for the partial tail.
        Assert.InRange(peaks.Length, 99, 101);

        foreach (var p in peaks)
        {
            Assert.True(p.Min <= p.Max, $"min {p.Min} > max {p.Max}");
            Assert.InRange(p.Max, -1f, 1f);
            Assert.InRange(p.Min, -1f, 1f);
        }

        // A full-scale sine's envelope must actually reach both rails.
        Assert.True(peaks.Max(p => p.Max) > 0.45f);
        Assert.True(peaks.Min(p => p.Min) < -0.45f);

        // Silence collapses every bucket onto zero rather than producing
        // nothing at all — a waveform view with no buckets draws nothing.
        var quiet = JuceWaveformPeaks.Build(new float[Sr], 1, Sr, 10.0);
        Assert.NotEmpty(quiet);
        Assert.All(quiet, p => Assert.Equal(0f, p.Max));
    }

    [Fact]
    public void Waveform_peaks_of_an_empty_buffer_are_empty_not_a_throw()
    {
        Assert.Empty(JuceWaveformPeaks.Build(ReadOnlySpan<float>.Empty, 1, Sr));
    }

    // ------------------------------------------------------ result records

    /// <summary>
    /// These are the values every caller of the detection surface actually
    /// handles. They are records, so equality is structural and a field added
    /// or reordered changes it — which is exactly what a consumer's `switch`
    /// or dictionary key depends on.
    /// </summary>
    [Fact]
    public void The_detection_result_records_carry_their_values_and_compare_structurally()
    {
        var peak = new JuceWaveformPeak(-0.5f, 0.5f);
        Assert.Equal(-0.5f, peak.Min);
        Assert.Equal(0.5f, peak.Max);
        Assert.Equal(peak, new JuceWaveformPeak(-0.5f, 0.5f));
        Assert.NotEqual(peak, new JuceWaveformPeak(-0.5f, 0.6f));

        var verify = new JuceVerifyResult(Hit: true, Confidence: 0.8f, Level: 0.25f);
        Assert.True(verify.Hit);
        Assert.Equal(0.8f, verify.Confidence);
        Assert.Equal(0.25f, verify.Level);
        Assert.NotEqual(verify, verify with { Hit = false });

        var chord = new JuceChordReading("Am", 9, JuceChordQuality.Minor, 0.7f);
        Assert.Equal("Am", chord.Name);
        Assert.Equal(9, chord.RootSemitone);
        Assert.Equal(JuceChordQuality.Minor, chord.Quality);
        Assert.Equal(chord, new JuceChordReading("Am", 9, JuceChordQuality.Minor, 0.7f));

        var onset = new JuceOnset(12345L, 0.9f);
        Assert.Equal(12345L, onset.SamplePosition);
        Assert.Equal(0.9f, onset.Strength);

        var frame = new JuceLeadFrame(1.25, 64f, JuceLeadTechnique.Bend);
        Assert.Equal(1.25, frame.TimeSeconds);
        Assert.Equal(64f, frame.Midi);
        Assert.Equal(JuceLeadTechnique.Bend, frame.Technique);
        // A silent frame has NO pitch — null, never 0, which would read as a
        // real note at MIDI 0.
        Assert.Null(new JuceLeadFrame(0, null, JuceLeadTechnique.Silent).Midi);
    }

    // ------------------------------------------------ tuner / lead / chord

    [Fact]
    public void The_chromatic_tuner_reports_a_reading_for_a_tone_and_none_for_silence()
    {
        var tuner = new JuceChromaticTuner(Sr, windowSize: 4096);

        Assert.Null(tuner.Detect(Silence(0.2)));

        var reading = tuner.Detect(Sine(440.0, 0.2, amp: 0.5f));
        Assert.NotNull(reading);
        Assert.InRange(reading!.Value.FrequencyHz, 430f, 450f);
        Assert.Equal("A", reading.Value.NoteName);
        Assert.InRange(reading.Value.CentsOff, -25f, 25f);
        Assert.InRange(reading.Value.Confidence, 0f, 1f);
    }

    /// <summary>The thin wrapper contract: silence is <c>Silent</c> with no
    /// pitch, a steady tone is a pitched frame, and <c>Reset</c> puts the
    /// tracker back to a state where the next tone is a fresh attack.</summary>
    [Fact]
    public void The_lead_tracker_distinguishes_silence_from_a_steady_tone()
    {
        var tracker = new JuceLeadTracker(Sr, blockSize: 1024, windowSize: 2048);
        Assert.Equal(Sr, tracker.SampleRate);
        Assert.Equal(1024, tracker.BlockSize);
        Assert.Equal(2048, tracker.WindowSize);

        var quiet = tracker.ProcessBlock(Silence(1024.0 / Sr));
        Assert.Equal(JuceLeadTechnique.Silent, quiet.Technique);
        Assert.Null(quiet.Midi);

        // Feed enough blocks for the analysis window to fill.
        var tone = Sine(440.0, 1.0, amp: 0.5f);
        JuceLeadFrame? pitched = null;
        for (var off = 0; off + 1024 <= tone.Length; off += 1024)
        {
            var frame = tracker.ProcessBlock(tone.AsSpan(off, 1024));
            if (frame.Midi is not null) { pitched = frame; break; }
        }

        Assert.NotNull(pitched);
        Assert.InRange(pitched!.Value.Midi!.Value, 67f, 71f);
        Assert.NotEqual(JuceLeadTechnique.Silent, pitched.Value.Technique);
        Assert.True(pitched.Value.TimeSeconds >= 0);

        tracker.Reset();
        Assert.Equal(JuceLeadTechnique.Silent, tracker.ProcessBlock(Silence(1024.0 / Sr)).Technique);
    }

    [Fact]
    public void The_chord_detector_needs_a_chroma_that_actually_says_something()
    {
        var detector = new JuceChordDetector();

        // A flat chroma is every note equally — no chord, and certainly not
        // whichever one happens to sort first.
        var flat = new float[12];
        Array.Fill(flat, 1f / 12f);
        Assert.Null(detector.Detect(flat));

        // An A minor triad: A (9), C (0), E (4).
        var aMinor = new float[12];
        aMinor[9] = 1f; aMinor[0] = 1f; aMinor[4] = 1f;
        var reading = detector.Detect(aMinor);

        Assert.NotNull(reading);
        Assert.Equal(9, reading!.Value.RootSemitone);
        Assert.Equal(JuceChordQuality.Minor, reading.Value.Quality);
        Assert.InRange(reading.Value.Confidence, detector.MinConfidence, 1f);
        Assert.False(string.IsNullOrWhiteSpace(reading.Value.Name));
    }
}
