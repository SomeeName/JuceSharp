using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceChartVerifierTests
{
    private static (JuceChartVerifier V, List<Synth.NoteEvent> Truth) Analyze(float[] audio)
    {
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(audio);
        return (new JuceChartVerifier(cqt), new List<Synth.NoteEvent>());
    }

    [Fact]
    public void ConfirmsPlayed_RejectsWrongFret()
    {
        // Ported from test_chart_verifier_confirms_played_rejects_wrong.
        int[] midis = { 40, 45, 50, 55, 59, 64 };   // E2 A2 D3 G3 B3 E4
        var (audio, truth) = Synth.RenderNotes(midis);
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(audio);
        var v = new JuceChartVerifier(cqt);

        int hits = 0, wrong = 0;
        foreach (var n in truth)
        {
            if (v.Verify(n.Midi, n.Onset).Hit) hits++;          // played → confirm
            if (v.Verify(n.Midi + 1, n.Onset).Hit) wrong++;     // wrong fret → reject
        }
        Assert.True(hits >= 5, $"missed played notes: {hits}/6");
        Assert.True(wrong <= 1, $"accepted wrong frets: {wrong}/6");
    }

    [Fact]
    public void PresenceBased_HeldNoteStillCounts()
    {
        // pre_ratio=0 default: a note sustained past its attack still verifies
        // mid-ring (no fresh attack required).
        var (audio, truth) = Synth.RenderNotes(new[] { 55 }, noteDur: 0.8);
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(audio);
        var v = new JuceChartVerifier(cqt);
        double onset = truth[0].Onset;
        Assert.True(v.Verify(55, onset).Hit);            // at the attack
        Assert.True(v.Verify(55, onset + 0.4).Hit);      // still ringing, no fresh attack
    }

    [Fact]
    public void AttackGatedMode_ConfirmsFreshAttack()
    {
        // pre_ratio>0 is the stricter attack-gated mode; a fresh attack must
        // still confirm (the mode raises the floor with a rise requirement but
        // never rejects a genuine pick).
        var (audio, truth) = Synth.RenderNotes(new[] { 55 }, noteDur: 0.9);
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(audio);
        var v = new JuceChartVerifier(cqt) { PreRatio = 1.0 };
        Assert.True(v.Verify(55, truth[0].Onset).Hit);
    }

    [Fact]
    public void Chord_ConfirmsMostTones_RejectsWrongChord()
    {
        var (audio, truth) = Synth.RenderChords(new IReadOnlyList<int>[] { new[] { 48, 52, 55, 59 } });
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(audio);
        var v = new JuceChartVerifier(cqt);

        int present = 0, wrongChord = 0;
        foreach (var n in truth)
        {
            if (v.Verify(n.Midi, n.Onset).Hit) present++;
            if (v.Verify(n.Midi + 1, n.Onset).Hit) wrongChord++;  // whole chord a semitone off
        }
        Assert.True(present >= 3, $"only {present}/4 chord tones confirmed");
        Assert.True(wrongChord <= 2, $"wrong-chord false-accepts too high: {wrongChord}/4");
    }

    [Fact]
    public void NoiseMargin_RejectsPhantomNotesAgainstPureNoise()
    {
        // Reproduces the live "confirms notes while idle" bug: pure input noise,
        // no playing. A relative-only floor phantom-confirms; the SNR gate rejects.
        var rng = new Random(1234);
        var noise = new float[44100];
        for (int i = 0; i < noise.Length; i++) noise[i] = (float)(rng.NextDouble() * 2 - 1) * 0.02f;
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(noise);

        int WithGate(float margin)
        {
            var v = new JuceChartVerifier(cqt) { NoiseMargin = margin };
            int hits = 0;
            for (int midi = 40; midi <= 76; midi++)
                if (v.Verify(midi, 0.5).Hit) hits++;   // 37 pitches, nothing played
            return hits;
        }

        int phantom = WithGate(0f);     // no gate — the bug
        int gated = WithGate(3f);       // SNR gate on
        Assert.True(gated <= 3, $"noise gate too weak: {gated}/37 phantom hits");
        Assert.True(gated < phantom, $"gate should cut phantom hits (was {phantom}, now {gated})");
    }

    /// <summary>
    /// The idle phantom-confirm guard on the path that actually SHIPS.
    /// <para><see cref="NoiseMargin_RejectsPhantomNotesAgainstPureNoise"/> only covers
    /// the OFFLINE (<c>AnalyzeWhole</c>) statistics; live play runs <c>Reset</c>/<c>Push</c>,
    /// whose noise floor is estimated over a rolling window instead of the whole clip.
    /// Any change to the streaming floor estimators must keep this at ZERO — the gate
    /// exists because the ungated verifier phantom-confirmed ~2/3 of charted notes
    /// against pure input noise.</para>
    /// </summary>
    [Fact]
    public void NoiseMargin_RejectsPhantomNotes_OnTheStreamingPath()
    {
        // Pure input noise, pushed in realistic 512-sample capture blocks — nothing is ever
        // played.
        var rng = new Random(1234);
        var noise = new float[44100 * 4];
        for (int i = 0; i < noise.Length; i++) noise[i] = (float)(rng.NextDouble() * 2 - 1) * 0.02f;

        // EXACTLY the live config (LiveCqtVerifier.CreateStreaming), read from LiveCqtConfig.
        // This guard hard-coded it as literals (E1 up, margin 3, no idle floor) and so went on
        // guarding the pre-0.57.4 configuration after live play had moved to A0, margin 2 and
        // the idle floor — the very mistake it says it exists to stop.
        var cqt = new JuceConstantQ(44100, hop: LiveCqtConfig.Hop, fMinMidi: LiveCqtConfig.MinMidi,
                                    binCount: LiveCqtConfig.BinCount,
                                    maxKernelSamples: LiveCqtConfig.MaxKernelSamples,
                                    noiseHistoryFrames: LiveCqtConfig.NoiseHistoryFrames(44100),
                                    subOctaveBins: LiveCqtConfig.SubOctaveBins,
                                    idleSteadyFrames: LiveCqtConfig.IdleSteadyFrames(44100));
        cqt.Reset(0);
        var v = new JuceChartVerifier(cqt)
            { NoiseMargin = LiveCqtConfig.StreamingNoiseMargin, SubOctaveVeto = LiveCqtConfig.SubOctaveVeto };

        int phantom = 0, probes = 0;
        for (int off = 0; off < noise.Length; off += 512)
        {
            int len = Math.Min(512, noise.Length - off);
            cqt.Push(noise.AsSpan(off, len), off);

            // Probe once the ring is warm, at the same cadence the matcher verifies.
            double now = (off + len) / 44100.0;
            if (now < 1.0 || off / 512 % 16 != 0) continue;
            for (int midi = 40; midi <= 76; midi += 6)
            {
                probes++;
                if (v.Verify(midi, now - 0.23).Hit) phantom++;   // window closed 230 ms ago
            }
        }

        Assert.True(probes > 50, $"guard probed too little: {probes}");
        Assert.True(phantom == 0, $"streaming idle phantom-confirms: {phantom}/{probes} (must be 0)");
    }

    [Fact]
    public void NoiseMargin_KeepsRealNoteOverNoise()
    {
        // A real note played into the same noise must still confirm with the gate.
        var (tone, truth) = Synth.RenderNotes(new[] { 52 }, noteDur: 0.8);
        var rng = new Random(99);
        for (int i = 0; i < tone.Length; i++) tone[i] += (float)(rng.NextDouble() * 2 - 1) * 0.01f;
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(tone);
        var v = new JuceChartVerifier(cqt) { NoiseMargin = 3f };
        Assert.True(v.Verify(52, truth[0].Onset).Hit, "gate wrongly rejected a real note over noise");
    }

    [Fact]
    public void Confidence_IsHigherForStrongHitThanNearThreshold()
    {
        var (audio, truth) = Synth.RenderNotes(new[] { 52 }, noteDur: 0.8);
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(audio);
        var v = new JuceChartVerifier(cqt);
        var hit = v.Verify(52, truth[0].Onset);
        Assert.True(hit.Hit);
        Assert.InRange(hit.Confidence, 0f, 1f);
        Assert.True(hit.Confidence > 0.5f);
    }
}
