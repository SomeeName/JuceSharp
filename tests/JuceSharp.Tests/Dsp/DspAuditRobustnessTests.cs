using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// D3 / D4 / D12 / D27. Four managed-DSP faults that all share one shape: the
/// class does not throw, it keeps running and returns a plausible-looking
/// number that is wrong. Every fixture here is synthetic — no dataset, no
/// native engine, no wall clock.
/// </summary>
[Collection("AllocationProbes")]
public sealed class DspAuditRobustnessTests
{
    private const int Sr = 22050;

    private static float[] ClickTrack(float bpm, int beats, int sr = Sr)
    {
        var samplesPerBeat = (int)(60f / bpm * sr);
        var audio = new float[samplesPerBeat * beats];
        for (var b = 0; b < beats; b++)
        {
            var start = b * samplesPerBeat;
            for (var i = 0; i < 200 && start + i < audio.Length; i++)
                audio[start + i] = MathF.Exp(-i / 30f) * MathF.Sin(2 * MathF.PI * 1000f * i / sr);
        }
        return audio;
    }

    // ---------------------------------------------------------------- D3 ----

    /// <summary>
    /// THE POINT of the beat detector's scrub: the result must be identical to
    /// the same audio with the bad sample zeroed. Unscrubbed it is not — every
    /// frame the NaN appears in contributes ZERO flux, so an onset landing there
    /// is lost and the beat grid moves. (The audit claimed <c>prevMag</c> stayed
    /// NaN for the rest of the song. It does not: the next frame overwrites that
    /// slot with its own finite magnitude.)
    /// </summary>
    [Fact]
    public void BeatDetector_OneNaNMidSong_StillFindsTheClickTrackTempo()
    {
        var clean = ClickTrack(120f, 40);
        var poisoned = (float[])clean.Clone();
        // ON an attack (50 samples into the second click), which is where the
        // flux the whole detector runs on actually lives. In the silence between
        // clicks a lost frame costs nothing and the fault is invisible.
        var samplesPerBeat = (int)(60f / 120f * Sr);
        var poisonAt = samplesPerBeat + 50;
        poisoned[poisonAt] = float.NaN;

        // The SAME audio with the bad sample already zeroed — which is exactly
        // what the scrub promises to be equivalent to.
        var prezeroed = (float[])poisoned.Clone();
        prezeroed[poisonAt] = 0f;

        var reference = new JuceBeatDetector(Sr).Analyze(prezeroed);
        var det = new JuceBeatDetector(Sr);
        var r = det.Analyze(poisoned);

        Assert.True(det.NonFiniteSamples > 0, "the scrub must be observable");
        Assert.Equal(reference.Bpm, r.Bpm);
        Assert.Equal(reference.Beats, r.Beats);
        Assert.NotEqual(det.MaxBpm, r.Bpm);
    }

    /// <summary>
    /// A hop longer than ~0.3 s of audio drives <c>60/MaxBpm*fps</c> below 1, so
    /// the integer floor was ZERO: <c>bpm = 60*fps/0 = +Inf</c> and the beat-grid
    /// loop <c>for (t += bestLag)</c> never advanced. This test's only real
    /// assertion is that it RETURNS.
    /// </summary>
    [Fact]
    public void BeatDetector_HopLongerThanTheMinimumBeat_Terminates()
    {
        // fps = 22050/8192 = 2.69; 60/200*2.69 = 0.81 -> floor 0 before the fix.
        var det = new JuceBeatDetector(Sr) { FftSize = 8192, HopSize = 8192, MaxBpm = 200f };
        var r = det.Analyze(ClickTrack(120f, 64));

        Assert.True(float.IsFinite(r.Bpm), $"BPM must be finite, was {r.Bpm}");
        Assert.True(r.Bpm > 0f);
        Assert.NotEmpty(r.Beats);
    }

    /// <summary>
    /// Integer autocorrelation lags quantise the tempo: at the default hop the
    /// envelope runs at ~43 fps here, where one lag step near 120 BPM is several
    /// BPM. Parabolic interpolation of the peak is what gets inside 0.5.
    /// </summary>
    [Theory]
    // 139.68 BPM (lag 18.5) is deliberately NOT here: the autocorrelation locks
    // onto its half-tempo, where lag 37 is almost exactly integral, so it cannot
    // distinguish an interpolated peak from a quantised one.
    [InlineData(114.84f)]   // lag 22.5 at 43.07 fps
    [InlineData(97.50f)]    // lag 26.5
    public void BeatDetector_InterpolatesThePeak_SoTempoIsNotLagQuantised(float bpm)
    {
        // Each of these sits HALFWAY between two integer autocorrelation lags at
        // the envelope rate (22050/512 = 43.07 fps), which is the worst case for
        // an un-interpolated peak: for 139.68 BPM the two neighbouring lags read
        // 143.6 and 136.0 — off by ~3.7 BPM in either direction.
        var r = new JuceBeatDetector(Sr).Analyze(ClickTrack(bpm, 96));

        // Accept the octave the autocorrelation locks onto, then demand precision.
        var folded = r.Bpm;
        while (folded < bpm * 0.75f) folded *= 2f;
        while (folded > bpm * 1.5f) folded /= 2f;
        Assert.InRange(folded, bpm - 1.5f, bpm + 1.5f);
    }

    [Theory]
    [InlineData(0, 512, 60f, 200f)]      // FftSize below the even-and->=2 minimum
    [InlineData(1023, 512, 60f, 200f)]   // FftSize odd — FftSize/2 would drop the top bin
    [InlineData(1024, 0, 60f, 200f)]     // HopSize zero
    [InlineData(1024, 512, 200f, 60f)]   // MinBpm >= MaxBpm
    [InlineData(1024, 512, 0f, 200f)]    // MinBpm zero
    public void BeatDetector_RejectsAnImpossibleConfiguration(int fft, int hop, float minBpm, float maxBpm)
    {
        var det = new JuceBeatDetector(Sr) { FftSize = fft, HopSize = hop, MinBpm = minBpm, MaxBpm = maxBpm };
        Assert.Throws<ArgumentException>(() => det.Analyze(new float[Sr]));
    }

    // ---------------------------------------------------------------- D4 ----

    /// <summary>
    /// <c>MinFreq</c>/<c>MaxFreq</c> are <c>init</c> properties, so the
    /// object-initializer setters run AFTER the constructor — a bin→chroma table
    /// built in the ctor was built from the DEFAULTS and the override was
    /// silently ignored. Two instances differing only in the override must
    /// therefore produce different chroma.
    /// </summary>
    [Fact]
    public void Chromagram_InitOverrideOfMinFreq_ActuallyChangesTheBinMapping()
    {
        const int sr = 44100;
        const int size = 4096;
        var frame = new float[size];
        // 82.4 Hz (E2) sits ABOVE the default 65 Hz floor and BELOW a 150 Hz one,
        // so the two tables must classify this frame differently.
        for (var i = 0; i < size; i++) frame[i] = MathF.Sin(2 * MathF.PI * 82.41f * i / sr);

        var withDefaultFloor = new JuceChromagram(sr).Compute(frame);
        var withRaisedFloor  = new JuceChromagram(sr) { MinFreq = 150f }.Compute(frame);

        var maxDiff = 0f;
        for (var i = 0; i < 12; i++)
            maxDiff = MathF.Max(maxDiff, MathF.Abs(withDefaultFloor[i] - withRaisedFloor[i]));

        Assert.True(maxDiff > 0.1f,
            "the MinFreq object-initializer override must reach the bin->chroma table " +
            $"(largest per-class difference was {maxDiff})");
    }

    /// <summary>
    /// The span overload must not allocate: <c>Compute</c> used to build a fresh
    /// <c>re</c>, <c>im</c> and <c>chroma</c> per call — 32 KB per frame at the
    /// default FFT size, on a per-frame analysis path.
    /// </summary>
    [Fact]
    public void Chromagram_RepeatedComputeAllocatesNothing()
    {
        const int sr = 44100;
        const int size = 4096;
        var frame = new float[size];
        for (var i = 0; i < size; i++) frame[i] = MathF.Sin(2 * MathF.PI * 220f * i / sr);

        var cg = new JuceChromagram(sr, size);
        // Heap, not stackalloc: a Span local cannot be captured, and this one is
        // allocated once BEFORE the window opens, so it is not in the reading.
        var chroma = new float[12];
        cg.Compute(frame, chroma);   // warm the lazy table + jit

        var delta = AllocationProbe.Measure(() => { for (var i = 0; i < 20; i++) cg.Compute(frame, chroma); });

        Assert.Equal(0, delta);
    }

    // --------------------------------------------------------------- D12 ----

    /// <summary>
    /// Below its lag ceiling YIN does not fail — it returns the CLAMPED lag as a
    /// confident reading. A 41.20 Hz bass low E through a 2048-sample window at
    /// 44.1 kHz reads ~43 Hz at high confidence: nearly a semitone sharp and
    /// indistinguishable from a real F1. The reading is deliberately still
    /// returned (JuceGuitarAnalyzer's long-window fallback keys off exactly this
    /// condition), so the FLAG is what makes it distinguishable.
    /// </summary>
    [Fact]
    public void Tuner_ReadingAtTheLagCeiling_IsFlaggedClamped()
    {
        const int sr = 44100;
        const int window = 2048;
        var samples = new float[window];
        for (var i = 0; i < window; i++) samples[i] = 0.5f * MathF.Sin(2 * MathF.PI * 41.20f * i / sr);

        var tuner = new JuceChromaticTuner(sr, window, minFreq: 25f, maxFreq: 1500f);

        // The window, not minFreq, is the binding limit: 44100/1024 = 43.07 Hz.
        Assert.Equal(sr / (window / 2f), tuner.EffectiveMinFrequency, 2);
        Assert.True(tuner.EffectiveMinFrequency > tuner.MinFrequency);

        var r = tuner.Detect(samples);
        Assert.NotNull(r);
        Assert.True(r!.Value.Clamped,
            $"a reading at the ceiling must be flagged (got {r.Value.FrequencyHz} Hz " +
            $"at confidence {r.Value.Confidence})");
        Assert.InRange(r.Value.FrequencyHz, tuner.EffectiveMinFrequency - 0.5f,
            tuner.EffectiveMinFrequency + 0.5f);
    }

    /// <summary>A note the window CAN resolve must not be flagged.</summary>
    [Fact]
    public void Tuner_AGenuineReading_IsNotFlaggedClamped()
    {
        const int sr = 44100;
        const int window = 4096;
        var samples = new float[window];
        for (var i = 0; i < window; i++) samples[i] = 0.5f * MathF.Sin(2 * MathF.PI * 440f * i / sr);

        var r = new JuceChromaticTuner(sr, window).Detect(samples);

        Assert.NotNull(r);
        Assert.False(r!.Value.Clamped);
        Assert.Equal("A", r.Value.NoteName);
    }

    // --------------------------------------------------------------- D27 ----

    private static float[] PluckedWithNonFinite(int sr, double seconds, float hz, out int injected)
    {
        var y = new float[(int)(seconds * sr)];
        var period = (int)(sr * 0.3);
        for (var i = 0; i < y.Length; i++)
        {
            var env = MathF.Exp(-(i % period) / (sr * 0.08f));
            y[i] = 0.5f * env * MathF.Sin(2 * MathF.PI * hz * i / sr);
        }
        float[] poison = [float.NaN, float.PositiveInfinity, float.NegativeInfinity];
        injected = 0;
        for (var i = sr / 2; i < y.Length; i += sr / 3) y[i] = poison[injected++ % poison.Length];
        return y;
    }

    /// <summary>
    /// The onset detector's <c>_prevLogMag</c> and 21-slot flux ring both persist
    /// across hops, so an unscrubbed NaN moves the adaptive median and then makes
    /// every <c>flux &gt; threshold</c> comparison false for the next 21 hops.
    /// </summary>
    [Fact]
    public void OnsetDetector_ScrubsNonFiniteInput_AndKeepsDetecting()
    {
        const int sr = 44100;
        var y = PluckedWithNonFinite(sr, 3.0, 220f, out var injected);
        Assert.True(injected > 4, "the fixture must inject a real burst");

        var clean = (float[])y.Clone();
        for (var i = 0; i < clean.Length; i++)
            if (!float.IsFinite(clean[i])) clean[i] = 0f;   // the same signal, pre-scrubbed

        var det = new JuceOnsetDetector(sr);
        var count = 0;
        for (var off = 0; off < y.Length; off += 512)
            count += det.Push(y.AsSpan(off, Math.Min(512, y.Length - off))).Count;

        var reference = new JuceOnsetDetector(sr);
        var cleanCount = 0;
        for (var off = 0; off < clean.Length; off += 512)
            cleanCount += reference.Push(clean.AsSpan(off, Math.Min(512, clean.Length - off))).Count;

        Assert.Equal(injected, det.NonFiniteSamples);
        Assert.Equal(0, reference.NonFiniteSamples);
        Assert.True(cleanCount >= 5, $"the fixture must trigger repeatedly, got {cleanCount}");
        // Scrubbing IS zeroing, so the two runs must agree exactly. Unscrubbed the
        // NaN poisons _prevLogMag and the flux median, and the run goes dead.
        Assert.Equal(cleanCount, count);
    }

    /// <summary>
    /// Every window a NaN sits in is DEAD in the note stream, and silently so:
    /// <c>energy</c> is NaN so the release test is false, and YIN returns null
    /// so no onset can be declared either. The scrub's contract is that the
    /// result equals the same audio with that sample zeroed — event for event,
    /// timestamp for timestamp.
    /// </summary>
    [Fact]
    public void NoteStream_ScrubsNonFiniteInput_AndKeepsEmitting()
    {
        const int sr = 44100;
        var poisoned = AlternatingNotes(sr, 3.0, out _);
        Poison(poisoned, sr, out var injected);
        // The same signal, pre-scrubbed: scrubbing IS zeroing, so the two runs
        // must agree EXACTLY once the fix is in.
        var clean = (float[])poisoned.Clone();
        for (var i = 0; i < clean.Length; i++) if (!float.IsFinite(clean[i])) clean[i] = 0f;

        var cleanEvents = RunNoteStream(sr, clean, out _);
        var poisonedEvents = RunNoteStream(sr, poisoned, out var stream);

        Assert.Equal(injected, stream.NonFiniteSamples);
        Assert.True(cleanEvents.Count > 4,
            $"the fixture must produce a stream of events, got {cleanEvents.Count}");
        // Scrubbing IS zeroing, so the two runs must agree EVENT FOR EVENT.
        Assert.Equal(cleanEvents, poisonedEvents);
    }

    private static List<JuceNoteEvent> RunNoteStream(int sr, float[] y, out JuceNoteStream stream)
    {
        stream = new JuceNoteStream(sr);
        var events = new List<JuceNoteEvent>();
        for (var off = 0; off < y.Length; off += 512)
            events.AddRange(stream.Push(y.AsSpan(off, Math.Min(512, y.Length - off))));
        return events;
    }

    /// <summary>Alternating 220/330 Hz plucks — a signal that keeps RETRIGGERING,
    /// which is what a latch suppresses.</summary>
    private static float[] AlternatingNotes(int sr, double seconds, out int noteCount)
    {
        var y = new float[(int)(seconds * sr)];
        var period = (int)(sr * 0.25);
        noteCount = 0;
        for (var i = 0; i < y.Length; i++)
        {
            var n = i / period;
            var hz = (n & 1) == 0 ? 220f : 330f;
            var env = MathF.Exp(-(i % period) / (sr * 0.10f));
            y[i] = 0.5f * env * MathF.Sin(2 * MathF.PI * hz * (i % period) / sr);
        }
        noteCount = y.Length / period;
        return y;
    }

    private static void Poison(float[] y, int sr, out int injected)
    {
        // Land each poison sample just AFTER an attack, i.e. while a note is being
        // held — that is the state the latch freezes.
        float[] poison = [float.NaN, float.PositiveInfinity, float.NegativeInfinity];
        var period = (int)(sr * 0.25);
        injected = 0;
        for (var n = 2; n * period + sr / 50 < y.Length; n++)
            y[n * period + sr / 50] = poison[injected++ % poison.Length];
    }

    /// <summary>The lead tracker's window is fed straight to YIN and to an RMS
    /// gate; a NaN makes <c>rms &gt; SilenceRms</c> false and the block reads as
    /// silence through a note that was played.</summary>
    [Fact]
    public void LeadTracker_ScrubsNonFiniteInput_AndKeepsTracking()
    {
        const int sr = 44100;
        const int block = 1024;
        var y = PluckedWithNonFinite(sr, 2.0, 220f, out var injected);

        var tracker = new JuceLeadTracker(sr, block);
        var voiced = 0;
        for (var off = 0; off + block <= y.Length; off += block)
            if (tracker.ProcessBlock(y.AsSpan(off, block)).Midi is not null) voiced++;

        var clean = (float[])y.Clone();
        for (var i = 0; i < clean.Length; i++) if (!float.IsFinite(clean[i])) clean[i] = 0f;
        var reference = new JuceLeadTracker(sr, block);
        var cleanVoiced = 0;
        for (var off = 0; off + block <= clean.Length; off += block)
            if (reference.ProcessBlock(clean.AsSpan(off, block)).Midi is not null) cleanVoiced++;

        Assert.Equal(injected, tracker.NonFiniteSamples);
        Assert.True(cleanVoiced > 10, $"the fixture must stay voiced, got {cleanVoiced} blocks");
        Assert.Equal(cleanVoiced, voiced);
    }
}
