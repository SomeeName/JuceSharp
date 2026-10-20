using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The wave-4 managed-DSP audit fixes (`JuceSharp-audit-2026-09-11-wave4-managed-satellites.md`
/// §6). Every case here fails on the code as it stood before its fix; the comment on each one
/// says what it saw.
/// <para>All of them are pure, offline and deterministic — synthesised signals only, no
/// dataset, no clock.</para>
/// </summary>
public sealed class Wave4DspAuditTests
{
    // ─────────────────────────────────────────────── W4-D7

    /// <summary>
    /// The legato short-window floor must come from the TUNER, not from a hand-copied
    /// window formula. At 22,050 Hz <c>JuceChromaticTuner</c> clamps its lag at
    /// <c>sampleRate / YinMinFreqHz</c> = 735 (EffectiveMinFrequency 30.0 Hz), not at
    /// <c>windowSize / 2</c> = 1024 — so below 30,720 Hz it is MIN FREQ that binds and the
    /// hand-copied formula computed 22.6 Hz where the real clamp sits at 30.0. A 30.8 Hz
    /// tone is one lag step off that ceiling, i.e. indistinguishable from a clamp, and the
    /// floor exists precisely to refuse it: the tuner-derived floor (31.5 Hz) sends it to
    /// the long-window retry, the hand-copied one waved it straight through as a
    /// MEASUREMENT.
    /// <para>Measured over 3 s: 239 fall-backs with the floor asked of the tuner, 2 with
    /// the re-derived formula.</para>
    /// </summary>
    [Fact]
    public void LegatoShortFloor_RejectsAReadingAtTheTunersClamp()
    {
        const int sr = 22050;
        var a = new JuceGuitarAnalyzer(sr);
        var block = new float[sr];                       // 1 s, repeated three times
        for (var i = 0; i < block.Length; i++)
            block[i] = 0.3f * MathF.Sin(2f * MathF.PI * 30.8f * i / sr);
        for (var rep = 0; rep < 3; rep++) a.Push(block);

        Assert.True(a.LegatoLongWindowFallbacks > 100,
            "a short-window reading sitting on the tuner's own lag ceiling must fall " +
            $"through to the long window; saw {a.LegatoLongWindowFallbacks} fall-backs");
    }

    // ─────────────────────────────────────────────── W4-D1

    /// <summary>
    /// <c>ringFrames</c> was the one unvalidated public argument in the type. <c>Reset(0, 0)</c>
    /// left <c>_ringFrames == 0</c> and the NEXT <c>Push</c> died in <c>Mod(frame, 0)</c> with a
    /// <see cref="DivideByZeroException"/> — an arithmetic fault a caller cannot attribute to
    /// the argument that caused it. A negative value threw <see cref="OverflowException"/> from
    /// the ring allocation instead.
    /// </summary>
    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    public void ConstantQ_Reset_RejectsANonPositiveRingFrames(int ringFrames)
    {
        var cqt = new JuceConstantQ();
        var ex = Assert.Throws<ArgumentOutOfRangeException>(() => cqt.Reset(0, ringFrames));
        Assert.Equal("ringFrames", ex.ParamName);
    }

    // ─────────────────────────────────────────────── W4-D3

    /// <summary>
    /// <c>Silent</c> is documented as "the block's RMS sat below the silence threshold" and
    /// <c>None</c> as "audio was present but no pitch could be resolved". The unvoiced branch
    /// returned <c>Silent</c> for BOTH, so a consumer could not tell "the player stopped" from
    /// "the player is making noise I cannot track" — pick scrapes, palm mutes and distorted
    /// noise all read as silence.
    /// </summary>
    [Fact]
    public void LeadTracker_LoudUnpitchedAudio_IsNone_SilenceIsSilent()
    {
        const int sr = 44100, block = 1024;
        var tracker = new JuceLeadTracker(sr, block);

        var rng = new Random(12345);                      // deterministic: fixed seed, no clock
        var noise = new float[block];
        JuceLeadFrame last = default;
        for (var b = 0; b < 40; b++)
        {
            for (var i = 0; i < block; i++) noise[i] = (float)(rng.NextDouble() * 2.0 - 1.0) * 0.5f;
            last = tracker.ProcessBlock(noise);
        }
        Assert.Equal(JuceLeadTechnique.None, last.Technique);

        var silence = new float[block];
        JuceLeadFrame quiet = default;
        for (var b = 0; b < 8; b++) quiet = tracker.ProcessBlock(silence);
        Assert.Equal(JuceLeadTechnique.Silent, quiet.Technique);
    }

    // ─────────────────────────────────────────────── W4-D4

    /// <summary>
    /// The class doc promises a retrigger is "immediately followed by a new note-on".
    /// <c>EmitOff</c> clears <c>_prevFreq</c>/<c>_stableCount</c>, so the fall-through onset
    /// evaluation failed its <c>_prevFreq &gt; 0</c> test and the note-on arrived one hop
    /// later — or, if the new pitch wobbled, not at all. Assert the pair shares a timestamp.
    /// </summary>
    [Fact]
    public void NoteStream_Retrigger_EmitsTheNoteOnInTheSameWindowAsTheOff()
    {
        const int sr = 44100;
        var stream = new JuceNoteStream(sr, windowSize: 2048);
        var events = new List<JuceNoteEvent>();

        events.AddRange(stream.Push(Tone(sr, 82.41f, sr)));        // E2, 1 s
        events.AddRange(stream.Push(Tone(sr, 110.00f, sr)));       // A2, 1 s — a retrigger

        var offIdx = events.FindIndex(e => e.Kind == JuceNoteEventKind.Off);
        Assert.True(offIdx >= 0, "the pitch step must produce a note-off");
        Assert.True(offIdx + 1 < events.Count, "the note-off must be followed by a note-on");

        var off = events[offIdx];
        var on  = events[offIdx + 1];
        Assert.Equal(JuceNoteEventKind.On, on.Kind);
        Assert.Equal(off.TimestampSeconds, on.TimestampSeconds, 9);
    }

    // ─────────────────────────────────────────────── W4-D12 / W4-D13

    /// <summary>
    /// <c>JuceChordDetector</c> carried a fourth private copy of the note-name table
    /// <c>PitchMath</c> was created to own, and normalised every template by a constant that
    /// <c>CosineSim</c> (scale-invariant) cannot see. Names must still be the sharp spelling
    /// for all twelve roots, and the verdict must be unchanged by the dropped normalisation.
    /// </summary>
    [Fact]
    public void ChordDetector_NamesEveryRoot_AndIsUnchangedByTemplateScale()
    {
        var det = new JuceChordDetector();
        string[] expected = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
        for (var root = 0; root < 12; root++)
        {
            var chroma = new float[12];
            chroma[root] = 1f;
            chroma[(root + 4) % 12] = 1f;
            chroma[(root + 7) % 12] = 1f;
            var r = det.Detect(chroma);
            Assert.NotNull(r);
            Assert.Equal(root, r!.Value.RootSemitone);
            Assert.Equal(expected[root], r.Value.Name);
            // Scale-invariance of the score: a template divided by 3 (what the dropped
            // normalisation did) cannot move a cosine similarity.
            var scaled = new float[12];
            for (var i = 0; i < 12; i++) scaled[i] = chroma[i] * 7.5f;
            Assert.Equal(r.Value.Confidence, det.Detect(scaled)!.Value.Confidence, 5);
        }
    }

    // ─────────────────────────────────────────────── W4-D11

    /// <summary>
    /// <c>ComputeSpectralFlux</c> dropped the last complete frame: the last valid offset is
    /// <c>frames·Hop</c>, so the count is <c>(n - Fft)/Hop + 1</c>. At the documented minimum
    /// input (4·FftSize) that was 6 frames instead of 7 — 14 % of the envelope on the shortest
    /// legal input, which is also where <c>maxLag = env.Length / 2</c> already binds.
    /// </summary>
    [Fact]
    public void BeatDetector_SpectralFlux_KeepsTheLastCompleteFrame()
    {
        // Sized so the dropped frame is the difference between a throw and an answer.
        // sr 7,000 / hop 1,024 = 6.836 fps: minLag = floor(60/200·fps) = 2, and
        // maxLag is clamped to env.Length / 2. At exactly 4·FftSize — the documented
        // minimum input — the old count was (4096-1024)/1024 = 3 frames, so maxLag
        // collapsed to 1 < minLag and Analyze threw "audio is too short". With the last
        // complete window kept it is 4 frames, maxLag 2, and the analysis runs.
        const int sr = 7000, fft = 1024, hop = 1024;
        var det = new JuceBeatDetector(sr) { FftSize = fft, HopSize = hop };
        var mono = new float[fft * 4];
        for (var b = 0; b * 700 < mono.Length; b++)
            for (var i = 0; i < 60 && b * 700 + i < mono.Length; i++)
                mono[b * 700 + i] = MathF.Exp(-i / 12f) * MathF.Sin(2f * MathF.PI * 900f * i / sr);

        var r = det.Analyze(mono);   // the assertion IS that this returns
        Assert.True(float.IsFinite(r.Bpm));
    }

    // ─────────────────────────────────────────────── W4-D14

    /// <summary>
    /// <c>Build</c>'s dB path is documented as floored at −120 dB, but
    /// <c>MathF.Max(-120f, NaN)</c> is NaN — so a single non-finite input sample reached the
    /// caller's frame through the one line whose whole purpose is that floor. Unlike its
    /// siblings this does not latch, which is why it is the last unscrubbed entry point in
    /// the assembly rather than a silent killer.
    /// </summary>
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void Spectrogram_NonFiniteInput_StaysInsideTheDocumentedFloor(bool decibelScale)
    {
        var b = new JuceSpectrogramBuilder(256, 64);
        var mono = new float[2048];
        for (var i = 0; i < mono.Length; i++) mono[i] = 0.25f * MathF.Sin(2f * MathF.PI * 1000f * i / 44100f);
        mono[700] = float.NaN;
        mono[701] = float.PositiveInfinity;

        var s = b.Build(mono, 44100, decibelScale);
        Assert.True(b.NonFiniteSamples > 0, "the scrub must be observable");
        Assert.NotEmpty(s.Frames);
        foreach (var frame in s.Frames)
        foreach (var v in frame)
        {
            Assert.True(float.IsFinite(v), "a non-finite magnitude must never reach the caller");
            Assert.True(decibelScale ? v >= -120f : v >= 0f, $"value {v} is outside the documented floor");
        }
    }

    // ─────────────────────────────────────────────── W4-D2

    /// <summary>
    /// The pending queue releases on the UNREFINED onset position, so <c>_finalizeLag</c>
    /// has to cover what <c>FinalizeOnset</c> reads AFTER <c>RefineAttackPosition</c> has
    /// moved it forward — up to <c>AnalysisWindow - PreOnsetLookback</c> = 3,840 samples.
    /// At the old lag the decay window could end past <c>_writePos</c>, where
    /// <c>ReadRing</c> clamps and ZERO-FILLS: the decay RMS came back deflated,
    /// <c>decayRatio</c> collapsed, and the note was silently demoted to <c>PalmMute</c>.
    /// <para>This is pinned STRUCTURALLY, through the public <c>LatencySamples</c> (which
    /// IS <c>_finalizeLag</c>), rather than by audio: the worst case needs the detector to
    /// fire &gt;3,072 samples early on a particular attack shape, so a stimulus that
    /// reproduces it is a stimulus that would silently stop reproducing it. The arithmetic
    /// is the invariant. At 44.1 kHz the refinement reach is
    /// <c>AnalysisWindow - PreOnsetLookback</c> = 4096 − 256 = 3,840 and the furthest read
    /// past the refined position is <c>DecayDelay + ShortRmsWindow</c> = 3,528 + 1,024 =
    /// 4,552, so the lag must be at least 8,392. It was 7,624.</para>
    /// </summary>
    [Theory]
    [InlineData(44100, 8392)]
    [InlineData(48000, 3840 + 3840 + 1024)]   // at 48 kHz the decay delay is 3,840 -> 8,704
    public void GuitarAnalyzer_FinalizeLag_CoversWhatItReadsAfterRefinement(int sampleRate, int minimum)
    {
        var a = new JuceGuitarAnalyzer(sampleRate);
        Assert.True(a.LatencySamples >= minimum,
            $"the pending queue releases on the UNREFINED position, so the lag must cover " +
            $"the refinement reach plus the decay window; {a.LatencySamples} < {minimum}");
    }

    // ─────────────────────────────────────────────── W4-D9

    /// <summary>
    /// <c>HarmonicSupport</c>'s comment promised an octave partial "that also rose", but at
    /// the SHIPPING <c>PreRatio</c> of 0 the test it ran was <c>magnitude &gt; 0·baseline</c>
    /// — i.e. <c>&gt; 0</c>, which any non-zero bin passes. A safety claim about a check
    /// that was not running. With no baseline to rise from the octave must at least clear
    /// its OWN bin floor.
    /// <para>The stimulus: a constant tone one quarter-tone ABOVE the charted note (so the
    /// charted bin clears its floor on spill but is not a strict peak, which is the masked
    /// chord tone this rescue exists for) plus the octave partial, loud for the first
    /// second and 2 % of that afterwards. At the tested moment the octave is a peak and is
    /// far below its own floor — the vacuous test accepted it, the floor test does not.
    /// </para>
    /// </summary>
    [Fact]
    public void ChartVerifier_HarmonicSupport_RequiresTheOctaveToClearItsOwnFloor()
    {
        const int sr = 44100;
        const int chartedMidi = 52;                                    // E3
        // ONE SEMITONE above, not a quarter tone: IsPeak compares bin ± Step, i.e. ± a
        // semitone, so a quarter-tone neighbour is invisible to it and the charted bin
        // would pass the peak test outright and never reach the HarmonicSupport rescue.
        var semitoneAbove = (float)(PitchHz(chartedMidi) * Math.Pow(2.0, 1.0 / 12.0));
        var octaveHz = (float)PitchHz(chartedMidi + 12);               // E4

        var y = new float[sr * 2];
        for (var i = 0; i < y.Length; i++)
        {
            var t = i / (float)sr;
            var octaveAmp = i < sr ? 1.0f : 0.02f;
            y[i] = 0.5f * MathF.Sin(2f * MathF.PI * semitoneAbove * i / sr)
                 + octaveAmp * 0.5f * MathF.Sin(2f * MathF.PI * octaveHz * i / sr);
            _ = t;
        }

        var cqt = new JuceConstantQ(sr, maxKernelSamples: LiveCqtConfig.MaxKernelSamples);
        cqt.AnalyzeWhole(y);
        var v = new JuceChartVerifier(cqt) { HarmonicSupport = true, PreRatio = 0.0, NoiseMargin = 0f };

        Assert.False(v.Verify(chartedMidi, 1.5).Hit,
            "an octave partial at 2 % of its own rolling level is not support for anything");

        // …and the rescue still WORKS where the octave really is present: the same chart
        // note, checked while the octave is loud.
        Assert.True(v.Verify(chartedMidi, 0.5).Hit,
            "HarmonicSupport must still rescue a masked tone whose octave is genuinely there");
    }

    private static double PitchHz(int midi) => 440.0 * Math.Pow(2.0, (midi - 69) / 12.0);

    // ─────────────────────────────────────────────── W4-D8

    /// <summary>
    /// <c>JuceConstantQ</c> and <c>JuceChartVerifier</c> mutate instance scratch from
    /// getter-shaped members (the percentile buffer, the floor memoisation) while their
    /// three siblings all carry an explicit "not thread-safe / one per thread" statement and
    /// these two carried none. Making them genuinely concurrent is impractical given the
    /// memoisation, so the DOC is the fix — and a doc is only a fix if something fails when
    /// it goes away.
    /// </summary>
    [Theory]
    [InlineData("JuceConstantQ.cs")]
    [InlineData("JuceChartVerifier.cs")]
    public void ScratchMutatingTypes_StateTheirThreadingContract(string fileName)
    {
        var path = Path.Combine(RepoRoot(), "src", "JuceSharp.Dsp", fileName);
        Assert.True(File.Exists(path), path);
        var text = File.ReadAllText(path);
        Assert.Contains("Thread-safety", text, StringComparison.Ordinal);
        Assert.Contains("not</b> thread-safe", text, StringComparison.Ordinal);
    }

    private static string RepoRoot()
    {
        var dir = AppContext.BaseDirectory;
        while (dir is not null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        Assert.NotNull(dir);
        return dir!;
    }

    private static float[] Tone(int sampleRate, float hz, int samples)
    {
        var x = new float[samples];
        for (var i = 0; i < samples; i++)
            x[i] = 0.4f * MathF.Sin(2f * MathF.PI * hz * i / sampleRate);
        return x;
    }
}
