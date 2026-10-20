using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     Wave-2 audit, the <c>JuceSharp.Dsp</c> half: findings #9, #10, #18, #27, #28,
///     #29 and #30, plus the "additional cleanups" in the same files and the test gaps
///     the audit named (the span-push drop counters, the tuner stream's tunables,
///     <see cref="PitchMath" />'s remaining conversions,
///     <see cref="JuceOnsetDetector.PositionOffset" /> and the three
///     <see cref="LiveCqtConfig" /> literals that had no mechanical backing).
///     <para>
///         Every case here is either a derivation or a differential: nothing asserts a
///         wall clock, and the concurrency case asserts an INVARIANT (a frame is
///         self-consistent) rather than a timing.
///     </para>
/// </summary>
[Collection("AllocationProbes")]
public sealed class Wave2DspAuditFixTests
{
    private const int Sr = 44100;

    private readonly ITestOutputHelper _out;

    public Wave2DspAuditFixTests(ITestOutputHelper output) => _out = output;

    // ------------------------------------------------------------------ #9

    /// <summary>
    /// #9. <c>JuceNoteStream</c> never validated <c>hopSamples</c>. An explicitly
    /// passed <c>0</c> is not <c>null</c>, so it slipped past the <c>??</c> default and
    /// made <c>Push</c> spin FOREVER on the caller's thread; a hop greater than the
    /// window drove <c>_bufferFill</c> negative and indexed out of range. Its sibling
    /// written in the same wave, <see cref="JuceTunerStream" />, had the guard from the
    /// start — this is that guard, copied.
    /// </summary>
    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    [InlineData(2049)]
    [InlineData(int.MaxValue)]
    public void NoteStream_RejectsAHopOutsideTheAnalysisWindow(int hop)
    {
        var ex = Assert.Throws<ArgumentOutOfRangeException>(
            () => new JuceNoteStream(Sr, windowSize: 2048, hopSamples: hop));
        Assert.Equal("hopSamples", ex.ParamName);
    }

    /// <summary>Non-vacuity for the guard: both ENDS of the accepted range still
    /// construct, and a legal hop still runs to completion — which is the property the
    /// rejected <c>0</c> destroyed.</summary>
    [Theory]
    [InlineData(1)]
    [InlineData(1024)]
    [InlineData(2048)]
    public void NoteStream_AcceptsEveryHopInsideTheWindow(int hop)
    {
        var stream = new JuceNoteStream(Sr, windowSize: 2048, hopSamples: hop);
        Assert.Equal(hop, stream.HopSamples);
    }

    /// <summary>A legal hop terminates and produces events. The value of this case is
    /// that it RETURNS — with <c>hopSamples: 0</c> the same call never did.</summary>
    [Fact]
    public void NoteStream_WithALegalHop_Terminates_AndTimestampsAtTheWindowStart()
    {
        var stream = new JuceNoteStream(Sr, windowSize: 2048, hopSamples: 1024);
        Assert.Equal(2048, stream.LatencySamples);   // the window, documented

        // Five windows' worth of a steady 220 Hz tone, pushed whole.
        var audio = new float[2048 + 5 * 1024];
        for (var i = 0; i < audio.Length; i++)
            audio[i] = 0.5f * MathF.Sin(2f * MathF.PI * 220f * i / Sr);

        var events = stream.Push(audio);
        Assert.Contains(events, e => e.Kind == JuceNoteEventKind.On);
        var on = events.First(e => e.Kind == JuceNoteEventKind.On);

        // The documented convention: the timestamp names the START of the analysis
        // window, so it is a whole number of hops and NEVER past the audio consumed.
        var atSample = on.TimestampSeconds * Sr;
        var hops = atSample / stream.HopSamples;
        _out.WriteLine($"note-on: midi {on.Midi} {on.NoteName}{on.Octave} at sample {atSample:F1} " +
                       $"({hops:F4} hops) of {audio.Length} consumed");
        Assert.Equal(Math.Round(hops), hops, 3);
        Assert.True(atSample <= audio.Length - 2048 + 1e-6,
            "an event may not be timestamped past the start of the last completed window");
        Assert.Equal(57, on.Midi);                         // 220 Hz = A3
        Assert.Equal("A", on.NoteName);
        Assert.Equal(3, on.Octave);
    }

    // ---------------------------------------------------------------- #10

    /// <summary>
    /// #10. The FFT-YIN difference clamps a negative rounding error to EXACTLY zero,
    /// and <c>d̃(τ) == 0</c> is the strongest possible crossing of <c>Threshold</c> —
    /// <c>confidence = 1 - 0 = 1.0</c>. The failing inputs (DC and near-DC windows) are
    /// pinned where the audit asked for them,
    /// <c>FftYinEquivalenceTests.FftDifference_OnADcWindow_AgreesWithTheDirectSum</c>,
    /// beside the 200-sine corpus that structurally could not reach them.
    /// <para>This is the other half: the energy floor that fix adds must not cost a real
    /// detection. A real tone is still found by the FFT path, at the same note and to
    /// well under half a cent.</para>
    /// </summary>
    [Theory]
    [InlineData(82.41f)]
    [InlineData(220f)]
    [InlineData(659.26f)]
    public void FftYin_StillDetectsARealTone_AfterTheEnergyFloor(float hz)
    {
        const int window = 2048;
        var buf = new float[window];
        for (var i = 0; i < window; i++) buf[i] = 0.5f * MathF.Sin(2f * MathF.PI * hz * i / Sr);

        var directReading = new JuceChromaticTuner(Sr, window, minFreq: 50f).Detect(buf);
        var fastReading   = new JuceChromaticTuner(Sr, window, minFreq: 50f, useFftDifference: true).Detect(buf);
        Assert.NotNull(directReading);
        Assert.NotNull(fastReading);
        var x = directReading!.Value;
        var y = fastReading!.Value;

        var cents = Math.Abs(1200.0 * Math.Log2(y.FrequencyHz / x.FrequencyHz));
        Assert.True(cents < 0.5, $"{cents:F4} cents apart");
        Assert.Equal(x.NoteName, y.NoteName);
    }

    // ---------------------------------------------------------------- #18

    /// <summary>
    /// #18. <c>Detect</c> is the one detection entry point that did not scrub, and BOTH
    /// of its gates are NaN-transparent: <c>NaN &lt; 1e-6f</c> is false and so are
    /// <c>NaN &lt; MinFrequency</c> and <c>NaN &gt; MaxFrequency</c>.
    /// <para><b>The reachable input is a window whose ENERGY overflows, not one holding
    /// a NaN</b> — that is the derivation, and it is the non-vacuity argument, so it is
    /// written out. Take a square wave of period 200 samples at amplitude 1e19f. Every
    /// sample is finite and <c>x²  = 1e38</c> is inside float range, but the sum over
    /// 2048 of them is +Inf, so the energy gate passes. <c>(2·1e19)² </c> is NOT inside
    /// float range, so every τ that straddles a transition contributes +Inf and
    /// <c>runningSum</c> becomes +Inf — while at τ = 200, the true period, <c>sum</c> is
    /// exactly 0. <c>diff[200] = 0·200/Inf = 0</c>, which is below <c>Threshold</c>, so
    /// τ is chosen; its neighbours are NaN, so <c>denom</c> is NaN, <c>refinedTau</c> is
    /// NaN and <c>freq</c> is NaN — which then passes the range gate untouched and
    /// returns <c>{ FrequencyHz = NaN, NoteName = "C", Octave = -1, CentsOff = NaN,
    /// Confidence = 1.0 }</c>. A maximum-confidence reading made of nothing, which is
    /// verbatim what the audit describes.</para>
    /// </summary>
    [Fact]
    public void Detect_OnAWindowWhoseEnergyOverflows_ReturnsNull_NotAConfidentNaN()
    {
        var buf = Square(1e19f, periodSamples: 200, length: 2048);
        Assert.All(buf, s => Assert.True(float.IsFinite(s), "the stimulus itself must be finite"));

        Assert.Null(new JuceChromaticTuner(Sr, 2048, minFreq: 50f).Detect(buf));
        Assert.Null(new JuceChromaticTuner(Sr, 2048, minFreq: 50f, useFftDifference: true).Detect(buf));
    }

    /// <summary>Non-vacuity for the case above: the SAME waveform at a sane amplitude is
    /// detected normally, so the guard rejects the overflow and not the signal.</summary>
    [Fact]
    public void Detect_OnTheSameSquareWaveAtUnitAmplitude_StillFindsThePeriod()
    {
        var buf = Square(0.5f, periodSamples: 200, length: 2048);
        var reading = new JuceChromaticTuner(Sr, 2048, minFreq: 50f).Detect(buf);
        Assert.NotNull(reading);
        var r = reading!.Value;

        // 44100 / 200 = 220.5 Hz.
        _out.WriteLine($"unit-amplitude square: {r.FrequencyHz:F3} Hz @ {r.Confidence:F3}");
        Assert.InRange(r.FrequencyHz, 210f, 232f);
        Assert.True(float.IsFinite(r.CentsOff));
    }

    /// <summary>A window holding a raw NaN or ±Inf yields no reading either, from either
    /// difference path. Free, and it cannot change a finite-input result.</summary>
    [Theory]
    [InlineData(float.NaN)]
    [InlineData(float.PositiveInfinity)]
    [InlineData(float.NegativeInfinity)]
    public void Detect_OnANonFiniteSample_ReturnsNull(float poison)
    {
        var buf = new float[2048];
        for (var i = 0; i < buf.Length; i++) buf[i] = 0.5f * MathF.Sin(2f * MathF.PI * 220f * i / Sr);
        buf[777] = poison;

        Assert.Null(new JuceChromaticTuner(Sr, 2048, minFreq: 50f).Detect(buf));
        Assert.Null(new JuceChromaticTuner(Sr, 2048, minFreq: 50f, useFftDifference: true).Detect(buf));
    }

    private static float[] Square(float amplitude, int periodSamples, int length)
    {
        var buf = new float[length];
        for (var i = 0; i < length; i++)
            buf[i] = i % periodSamples < periodSamples / 2 ? amplitude : -amplitude;
        return buf;
    }

    // ---------------------------------------------------------------- #27

    /// <summary>
    /// #27. <see cref="JuceTunerStream.Latest" /> is documented "bind a UI to this" and
    /// the designed use is a capture thread pushing while the UI thread reads — but the
    /// frame is an eight-field record struct, so a plain property assignment is several
    /// unordered stores. A reader could see <c>NoteName</c> from one frame with
    /// <c>MidiNote</c>/<c>CentsOff</c> from another: a needle pointing at a note it is
    /// not naming.
    /// <para>The assertion is an INVARIANT, never a timing: every frame a reader
    /// observes must be internally consistent — its name and octave must be the ones
    /// <see cref="PitchMath" /> derives from its own MIDI number. That can only fail if
    /// the read tore.</para>
    /// </summary>
    [Fact]
    public void Latest_IsNeverObservedTorn_WhileAnotherThreadPushes()
    {
        // A short window and a high floor keep each YIN call cheap; the point is the
        // publication, not the detection.
        var stream = new JuceTunerStream(new JuceChromaticTuner(Sr, 1024, minFreq: 200f, maxFreq: 1500f),
                                         hopSize: 256);
        var audio = new float[1024 + 400 * 256];
        for (var i = 0; i < audio.Length; i++)
        {
            // Alternate between A4 and A5 every ~20 hops so the published frame really
            // does change name, octave and MIDI number together.
            var hz = i / (20 * 256) % 2 == 0 ? 440f : 880f;
            audio[i] = 0.5f * MathF.Sin(2f * MathF.PI * hz * i / Sr);
        }

        var stop = false;
        var seen = 0;
        var torn = (JuceTunerFrame?)null;

        var reader = new Thread(() =>
        {
            while (!Volatile.Read(ref stop))
            {
                if (stream.Latest is not { } f) continue;
                seen++;
                if (f.NoteName != PitchMath.NoteName(f.MidiNote) || f.Octave != PitchMath.Octave(f.MidiNote))
                {
                    torn = f;
                    return;
                }
            }
        }) { IsBackground = true };

        reader.Start();
        for (var i = 0; i + 256 <= audio.Length; i += 256) stream.Push(audio.AsSpan(i, 256));
        Volatile.Write(ref stop, true);
        reader.Join();

        _out.WriteLine($"reader observed {seen} frames across {stream.FramesEmitted} publications");
        Assert.Null(torn);
        Assert.True(stream.FramesEmitted > 100, $"only {stream.FramesEmitted} frames were published");
        Assert.True(seen > 0, "the reader never saw a frame — the case did not exercise the publication");
    }

    /// <summary>The publication is still allocation-free — which is exactly why it is a
    /// seqlock and not an immutable published reference.</summary>
    [Fact]
    public void Latest_PublicationDoesNotAllocate()
    {
        var stream = new JuceTunerStream(new JuceChromaticTuner(Sr, 1024, minFreq: 200f), hopSize: 256);
        var block = new float[256];
        for (var i = 0; i < block.Length; i++) block[i] = 0.5f * MathF.Sin(2f * MathF.PI * 440f * i / Sr);

        for (var i = 0; i < 16; i++) stream.Push(block);          // warm up

        var delta = AllocationProbe.Measure(
            () => { for (var i = 0; i < 64; i++) { stream.Push(block); _ = stream.Latest; } });
        Assert.Equal(0L, delta);
    }

    // ---------------------------------------------------------------- #28

    /// <summary>
    /// #28. A YIN octave slip is 1200 cents, far past
    /// <see cref="JuceTunerStream.ImmediateChangeCents" /> (250), so it used to switch
    /// the displayed note IMMEDIATELY and the recovery frame switched it immediately
    /// back — bypassing the hysteresis for precisely the failure the class doc sells
    /// median + hysteresis as the defence against. The 3-tap median removes only a
    /// SINGLE-frame slip.
    /// <para>Driven with <c>hopSize == WindowSize</c> so every frame is a clean window of
    /// one frequency and the frame count is exact. A2 (110 Hz, MIDI 45) is held, then
    /// A3 (220 Hz, MIDI 57) arrives: an exact octave.</para>
    /// </summary>
    [Theory]
    [InlineData(false)]   // the fix: an octave goes through NoteChangeFrames
    [InlineData(true)]    // the opt-in old behaviour
    public void AnOctaveJump_TakesTheHysteresis_UnlessExplicitlyExempted(bool bypass)
    {
        const int window = 2048;
        var stream = new JuceTunerStream(new JuceChromaticTuner(Sr, window, minFreq: 50f), hopSize: window)
        {
            NoteChangeFrames = 3,
            OctaveJumpsBypassHysteresis = bypass,
        };

        for (var i = 0; i < 6; i++) stream.Push(Window(110f, window, i));
        Assert.NotNull(stream.Latest);
        Assert.Equal(45, stream.Latest!.Value.MidiNote);        // A2

        var switchedAfter = -1;
        for (var frame = 1; frame <= 8; frame++)
        {
            stream.Push(Window(220f, window, frame));
            if (switchedAfter < 0 && stream.Latest is { MidiNote: 57 }) switchedAfter = frame;
        }

        _out.WriteLine($"bypass={bypass}: switched after {switchedAfter} octave-up frames");
        Assert.True(switchedAfter > 0, "the stream never switched to A3 at all");
        if (bypass)
            // Two frames is the 3-tap median's own latency; the hysteresis adds nothing.
            Assert.Equal(2, switchedAfter);
        else
            // Median latency (2) plus NoteChangeFrames agreeing readings.
            Assert.True(switchedAfter >= 4,
                $"an exact octave still bypassed the hysteresis (switched after {switchedAfter} frames)");
    }

    private static float[] Window(float hz, int length, int index)
    {
        var buf = new float[length];
        for (var i = 0; i < length; i++)
            buf[i] = 0.5f * MathF.Sin(2f * MathF.PI * hz * (index * (long)length + i) / Sr);
        return buf;
    }

    /// <summary>The whole tunable surface, which had defaults and nothing else. Each of
    /// these is load-bearing for a tuner UI and each was unreachable from any test.</summary>
    [Fact]
    public void TunerStream_TunablesAreReadableAndDefaulted()
    {
        var tuner  = new JuceChromaticTuner(Sr, 4096, minFreq: 25f);
        var stream = new JuceTunerStream(tuner);

        Assert.Same(tuner, stream.Tuner);                   // not copied — the caller's floor and window
        Assert.Equal(4096, stream.WindowSize);
        Assert.Equal(1024, stream.HopSize);                 // WindowSize / 4
        Assert.Equal(3, stream.StableFrames);
        Assert.Equal(0.5f, stream.MinStableConfidence);
        Assert.Equal(12, stream.ReleaseFrames);
        Assert.Equal(1.05f, stream.ClampedMargin);
        Assert.Equal(3, stream.NoteChangeFrames);
        Assert.Equal(250f, stream.ImmediateChangeCents);
        Assert.False(stream.OctaveJumpsBypassHysteresis);

        var tweaked = new JuceTunerStream(tuner, hopSize: 512)
        {
            StableFrames = 7, MinStableConfidence = 0.9f, ReleaseFrames = 2, ClampedMargin = 2f,
        };
        Assert.Equal(512, tweaked.HopSize);
        Assert.Equal(7, tweaked.StableFrames);
        Assert.Equal(0.9f, tweaked.MinStableConfidence);
        Assert.Equal(2, tweaked.ReleaseFrames);
        Assert.Equal(2f, tweaked.ClampedMargin);
    }

    /// <summary><see cref="JuceTunerStream.StableFrames" /> and
    /// <see cref="JuceTunerStream.MinStableConfidence" /> actually gate
    /// <see cref="JuceTunerFrame.IsStable" />: an impossible confidence bar can never
    /// produce a stable frame, an ordinary one can.</summary>
    [Fact]
    public void MinStableConfidence_GatesIsStable()
    {
        const int window = 2048;
        static float[] Tone(int index, int length)
        {
            var buf = new float[length];
            for (var i = 0; i < length; i++)
                buf[i] = 0.5f * MathF.Sin(2f * MathF.PI * 440f * (index * (long)length + i) / Sr);
            return buf;
        }

        var reachable  = new JuceTunerStream(new JuceChromaticTuner(Sr, window, minFreq: 50f), hopSize: window);
        var impossible = new JuceTunerStream(new JuceChromaticTuner(Sr, window, minFreq: 50f), hopSize: window)
        {
            MinStableConfidence = 1.1f,   // no confidence can ever clear this
        };

        var sawStable = false;
        for (var i = 0; i < 8; i++)
        {
            reachable.Push(Tone(i, window));
            impossible.Push(Tone(i, window));
            if (reachable.Latest is { IsStable: true }) sawStable = true;
            Assert.False(impossible.Latest?.IsStable ?? false,
                "a frame cleared a confidence bar above 1.0");
        }
        Assert.True(sawStable, "no frame was ever stable — the gate was not exercised");
    }

    // ------------------------------------------------------------ #29 / #30

    /// <summary>
    /// #29. <c>bestVal</c> starts at 0 and <c>v &gt; bestVal</c> is never true for an
    /// all-zero envelope, so <c>bestLag</c> stayed at its <c>minLag</c> seed and
    /// <c>Analyze</c> returned <c>bpm ≈ MaxBpm</c> with a beat chain through arbitrary
    /// tied frames and a tempo map built from them — indistinguishable from a real
    /// detection, for SILENCE.
    /// </summary>
    [Fact]
    public void Analyze_OnSilence_ReportsNothing_RatherThanFabricatingMaxBpm()
    {
        const int sr = 22050;
        var det = new JuceBeatDetector(sr);
        var r = det.Analyze(new float[sr * 4]);

        Assert.Equal(0f, r.Bpm);
        Assert.Empty(r.Beats);
        Assert.Empty(r.TempoMap);
        Assert.Empty(r.Downbeats);
        Assert.Equal(0, r.BeatsPerBar);
        Assert.Equal(0f, r.Confidence);
        // The specific wrong answer it used to give.
        Assert.NotEqual(det.MaxBpm, r.Bpm);
    }

    /// <summary>Non-vacuity: a real click track is detected AND carries a confidence
    /// well above the silent case, so <see cref="JuceBeatDetector.Result.Confidence" />
    /// separates the two rather than being a constant.</summary>
    [Fact]
    public void Confidence_SeparatesARealClickTrackFromSilenceAndFromAFlatEnvelope()
    {
        const int sr = 22050;
        var clicks = ClickTrack(sr, 120f, 32);

        var beat  = new JuceBeatDetector(sr).Analyze(clicks);
        var quiet = new JuceBeatDetector(sr).Analyze(new float[clicks.Length]);

        _out.WriteLine($"click track: {beat.Bpm:F2} BPM @ confidence {beat.Confidence:F4}; " +
                       $"silence: {quiet.Bpm:F2} BPM @ {quiet.Confidence:F4}");

        Assert.InRange(beat.Confidence, 0f, 1f);
        Assert.True(beat.Confidence > 0.05f, $"a clean click track scored only {beat.Confidence:F4}");
        Assert.Equal(0f, quiet.Confidence);
        Assert.True(beat.Bpm > 0f);
    }

    /// <summary>
    /// #30. <see cref="JuceBeatDetector.MinBpm" /> had no lower bound while driving an
    /// O(windows · maxLag²) local search: at <c>MinBpm = 1</c> a five-minute track is
    /// ~6·10¹⁰ multiply-adds — hours, with no progress reporting and no cancellation
    /// token. The floor is a cost bound.
    /// </summary>
    [Theory]
    [InlineData(1f)]
    [InlineData(5f)]
    [InlineData(19.9f)]
    public void Analyze_RejectsAMinBpmBelowTheFloor(float minBpm)
    {
        var det = new JuceBeatDetector(22050) { MinBpm = minBpm };
        var ex = Assert.Throws<ArgumentException>(() => det.Analyze(new float[22050 * 2]));
        Assert.Contains(nameof(JuceBeatDetector.MinBpm), ex.Message);
    }

    /// <summary>Non-vacuity for the floor: the floor itself is accepted, so the bound is
    /// a bound and not a ban.</summary>
    [Fact]
    public void Analyze_AcceptsExactlyTheFloor()
    {
        const int sr = 22050;
        Assert.Equal(20f, JuceBeatDetector.MinAllowedBpm);
        var det = new JuceBeatDetector(sr) { MinBpm = JuceBeatDetector.MinAllowedBpm };
        var r = det.Analyze(ClickTrack(sr, 120f, 24));
        Assert.True(r.Bpm > 0f);
    }

    // ------------------------------------------- JuceTempoSegment / tolerance

    /// <summary><c>JuceTempoSegment</c> itself: a value type whose equality is what the
    /// tempo-map assertions elsewhere lean on.</summary>
    [Fact]
    public void JuceTempoSegment_IsAValueWithComponentEquality()
    {
        var a = new JuceTempoSegment(TimeSpan.FromSeconds(1.5), 128.5f);
        var b = new JuceTempoSegment(TimeSpan.FromSeconds(1.5), 128.5f);

        Assert.Equal(a, b);
        Assert.Equal(a.GetHashCode(), b.GetHashCode());
        Assert.NotEqual(a, a with { Bpm = 129f });

        var (time, bpm) = a;
        Assert.Equal(TimeSpan.FromSeconds(1.5), time);
        Assert.Equal(128.5f, bpm);
    }

    /// <summary>
    /// <see cref="JuceBeatDetector.AnalysisOptions.SegmentToleranceBpm" /> had a default
    /// and nothing that read it. A 100 → 140 BPM ramp segments at the default and
    /// collapses to ONE segment under a tolerance nothing can exceed — which is the only
    /// way to show the value reaches <c>BuildTempoMap</c> at all.
    /// </summary>
    [Fact]
    public void SegmentToleranceBpm_DecidesHowManySegmentsARampBecomes()
    {
        Assert.Equal(2f, new JuceBeatDetector.AnalysisOptions().SegmentToleranceBpm);

        const int sr = 22050;
        var audio = TempoRamp(sr, 100f, 140f, seconds: 24.0);

        var fine   = Detector(sr, 2f).Analyze(audio);
        var coarse = Detector(sr, 1000f).Analyze(audio);

        _out.WriteLine($"ramp: {fine.TempoMap.Count} segment(s) at tolerance 2, " +
                       $"{coarse.TempoMap.Count} at 1000");
        Assert.True(fine.TempoMap.Count > 1, $"a 40 BPM ramp collapsed to {fine.TempoMap.Count} segment(s)");
        Assert.Single(coarse.TempoMap);

        static JuceBeatDetector Detector(int rate, float tolerance) => new(rate)
        {
            FftSize = 512,
            HopSize = 64,
            Options = new JuceBeatDetector.AnalysisOptions { SegmentToleranceBpm = tolerance },
        };
    }

    private static float[] ClickTrack(int sr, float bpm, int beats, double lead = 0.5)
    {
        var total = (int)((lead + beats * 60.0 / bpm + 1.0) * sr);
        var audio = new float[total];
        for (var b = 0; b < beats; b++)
        {
            var start = (int)((lead + b * 60.0 / bpm) * sr);
            for (var i = 0; i < 200 && start + i < total; i++)
                audio[start + i] += 0.8f * MathF.Exp(-i / 30f) * MathF.Sin(2f * MathF.PI * 1000f * i / sr);
        }
        return audio;
    }

    private static float[] TempoRamp(int sr, float fromBpm, float toBpm, double seconds)
    {
        var times = new List<double>();
        var now = 0.5;
        while (now < 0.5 + seconds)
        {
            times.Add(now);
            var bpm = fromBpm + (toBpm - fromBpm) * Math.Clamp((now - 0.5) / seconds, 0, 1);
            now += 60.0 / bpm;
        }
        var total = (int)((times[^1] + 1.0) * sr);
        var audio = new float[total];
        foreach (var t in times)
        {
            var start = (int)(t * sr);
            for (var i = 0; i < 200 && start + i < total; i++)
                audio[start + i] += 0.8f * MathF.Exp(-i / 30f) * MathF.Sin(2f * MathF.PI * 1000f * i / sr);
        }
        return audio;
    }

    // ------------------------------------------------------------- PitchMath

    /// <summary>
    /// The cleanup the audit filed under "this type exists to be THE correct copy":
    /// <c>midi / 12 - 1</c> truncates toward ZERO, so every NEGATIVE MIDI number was an
    /// octave too high and the whole band −11..−1 collapsed onto the same octave as
    /// 0..11. Unreachable below 8.18 Hz today; wrong in the one place that is supposed
    /// to be trustworthy.
    /// </summary>
    [Theory]
    [InlineData(0, -1)]      // C-1
    [InlineData(11, -1)]     // B-1
    [InlineData(12, 0)]      // C0
    [InlineData(60, 4)]      // C4 — the anchor
    [InlineData(-1, -2)]     // B-2, NOT -1
    [InlineData(-11, -2)]    // C#-2
    [InlineData(-12, -2)]    // C-2
    [InlineData(-13, -3)]    // B-3
    [InlineData(-24, -3)]    // C-3
    public void Octave_FloorsRatherThanTruncating(int midi, int expected)
    {
        Assert.Equal(expected, PitchMath.Octave(midi));
        // The definition, independently: octave n spans MIDI [12(n+1), 12(n+1)+11].
        Assert.Equal((int)Math.Floor(midi / 12.0) - 1, PitchMath.Octave(midi));
    }

    /// <summary>The three conversions nothing referenced. Both precisions are kept ON
    /// PURPOSE, so each is asserted against its own exact expression — never against the
    /// other one.</summary>
    [Fact]
    public void MidiToHzF_HzToMidi_AndCents_ReproduceTheirExactExpressions()
    {
        foreach (var midi in new[] { 28f, 40f, 69f, 88.5f })
            Assert.Equal(440f * MathF.Pow(2f, (midi - 69f) / 12f), PitchMath.MidiToHzF(midi));

        foreach (var hz in new[] { 41.20, 82.41, 220.0, 440.0, 1318.5 })
        {
            Assert.Equal(69.0 + 12.0 * Math.Log2(hz / 440.0), PitchMath.HzToMidi(hz));
            Assert.Equal(1200.0 * Math.Log2(hz / 440.0), PitchMath.Cents(hz, 440.0));
        }

        // The anchor, and the two directions agreeing on it.
        Assert.Equal(440f, PitchMath.MidiToHzF(69f));   // 2^0 is exact, so this one is bit-equal
        Assert.Equal(69.0, PitchMath.HzToMidi(440.0), 10);
        Assert.Equal(0.0, PitchMath.Cents(440.0, 440.0));
        Assert.Equal(1200.0, PitchMath.Cents(880.0, 440.0), 10);
        Assert.Equal(-1200.0, PitchMath.Cents(220.0, 440.0), 10);
    }

    // ------------------------------------------------- onset / analyzer gaps

    /// <summary>
    /// <see cref="JuceOnsetDetector.PositionOffset" /> is 384 and NOTHING asserted that
    /// the positions it reports actually carry it. The modular identity is exact: a
    /// reported position is a whole number of hops PLUS the offset, so a change to how
    /// the offset is applied (half of it, or applied twice) fails here even though the
    /// value of the constant is unchanged.
    /// </summary>
    [Fact]
    public void PositionOffset_IsCarriedByEveryReportedPosition()
    {
        Assert.Equal(384, JuceOnsetDetector.PositionOffset);
        Assert.Equal(JuceOnsetDetector.FftSize + JuceOnsetDetector.HopSize - JuceOnsetDetector.PositionOffset,
                     JuceOnsetDetector.LatencySamples);

        const int attack = 8192;
        var audio = new float[attack + 4 * Sr / 10];
        for (var i = 0; i < 400 && attack + i < audio.Length; i++)
            audio[attack + i] = 0.8f * MathF.Exp(-i / 60f) * MathF.Sin(2f * MathF.PI * 1000f * i / Sr);

        var det = new JuceOnsetDetector(Sr);
        var onsets = new List<JuceOnset>();
        for (var i = 0; i + JuceOnsetDetector.HopSize <= audio.Length; i += JuceOnsetDetector.HopSize)
            onsets.AddRange(det.Push(audio.AsSpan(i, JuceOnsetDetector.HopSize)));

        Assert.NotEmpty(onsets);
        foreach (var o in onsets)
            Assert.Equal(0L, (o.SamplePosition - JuceOnsetDetector.PositionOffset) % JuceOnsetDetector.HopSize);

        // …and it is a TUNING toward the perceptual attack, so the first report lands
        // near the real one rather than at an arbitrary window boundary.
        _out.WriteLine($"attack at {attack}, first onset reported at {onsets[0].SamplePosition} " +
                       $"(delta {onsets[0].SamplePosition - attack})");
        Assert.InRange(onsets[0].SamplePosition, (long)(attack - JuceOnsetDetector.FftSize),
                       (long)(attack + JuceOnsetDetector.FftSize));
    }

    /// <summary><see cref="JuceGuitarAnalyzer.DroppedEvents" /> is only ever read as
    /// "still zero". This is the other half: it COUNTS, and — being a health counter —
    /// <see cref="JuceGuitarAnalyzer.Reset" /> leaves it alone. Same shape as
    /// <see cref="JuceOnsetDetector.DroppedOnsets" />, which is checked beside it.</summary>
    [Fact]
    public void DropCounters_CountOverflow_AndSurviveReset()
    {
        var audio = new float[8192 + 6 * (int)(0.5 * Sr) + 2 * Sr];
        for (var p = 0; p < 6; p++)
        {
            var start = 8192 + p * (int)(0.5 * Sr);
            for (var i = 0; i < (int)(0.5 * Sr) && start + i < audio.Length; i++)
            {
                var env = MathF.Exp(-i / (0.25f * Sr));
                var t = (start + i) / (float)Sr;
                audio[start + i] = 0.6f * env *
                    (MathF.Sin(2f * MathF.PI * 196f * t) + 0.4f * MathF.Sin(2f * MathF.PI * 392f * t));
            }
        }

        const int block = 1024;
        var expected = 0;
        var reference = new JuceGuitarAnalyzer(Sr);
        for (var i = 0; i + block <= audio.Length; i += block) expected += reference.Push(audio.AsSpan(i, block)).Count;
        Assert.True(expected > 0, "the corpus emitted no events at all");

        var analyzer = new JuceGuitarAnalyzer(Sr);
        var none = Array.Empty<JuceGuitarEvent>();
        var written = 0;
        for (var i = 0; i + block <= audio.Length; i += block)
        {
            analyzer.Push(audio.AsSpan(i, block), none, out var n);
            written += n;
        }

        Assert.Equal(0, written);
        Assert.Equal(expected, analyzer.DroppedEvents);

        // A health counter, not per-session state.
        analyzer.Reset();
        Assert.Equal(expected, analyzer.DroppedEvents);

        var onsetDet = new JuceOnsetDetector(Sr);
        var noOnsets = Array.Empty<JuceOnset>();
        for (var i = 0; i + block <= audio.Length; i += block) onsetDet.Push(audio.AsSpan(i, block), noOnsets, out _);
        var dropped = onsetDet.DroppedOnsets;
        Assert.True(dropped > 0, "no onsets were dropped — the overflow path was not exercised");
        onsetDet.Reset();
        Assert.Equal(dropped, onsetDet.DroppedOnsets);
    }

    // --------------------------------------------------------- LiveCqtConfig

    /// <summary>
    /// The guides call <see cref="LiveCqtConfig" /> the single source of truth, and
    /// <c>CqtEvalConfigTests</c> pins most of it — but <c>RingFrames</c>,
    /// <c>NoisePercentile</c> and <c>RelFloor</c> were three named constants with no
    /// mechanical backing at all, which is the exact shape that let the eval harness
    /// fall behind on <c>NoiseHistorySeconds</c>. Same value, one name, now asserted.
    /// </summary>
    [Fact]
    public void RingFrames_NoisePercentile_AndRelFloor_AreTheirDocumentedValues()
    {
        Assert.Equal(384, LiveCqtConfig.RingFrames);
        Assert.Equal(0.05f, LiveCqtConfig.NoisePercentile);   // the 5th percentile
        Assert.Equal(0.10f, LiveCqtConfig.RelFloor);

        // The XML doc's own arithmetic: 384 frames at hop 256 is 2.23 s at 44.1 kHz —
        // and emphatically NOT the 10 s noise horizon it used to be confused with.
        Assert.Equal(2.23, LiveCqtConfig.RingFrames * (double)LiveCqtConfig.Hop / 44100.0, 2);
        Assert.NotEqual(LiveCqtConfig.RingFrames, LiveCqtConfig.NoiseHistoryFrames(44100));
    }

    /// <summary>…and <c>RelFloor</c> is observably the constructor default, not merely a
    /// constant sitting next to one.</summary>
    [Fact]
    public void JuceConstantQ_TakesRelFloorFromLiveCqtConfig()
    {
        var cqt = new JuceConstantQ(Sr, hop: LiveCqtConfig.Hop, fMinMidi: LiveCqtConfig.MinMidi,
                                    binCount: LiveCqtConfig.BinCount);
        Assert.Equal(LiveCqtConfig.RelFloor, cqt.RelFloor);
    }
}
