using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The YIN half of <see cref="JuceGuitarAnalyzer"/> must reach the bottom of a bass.
///
/// <para><c>JuceChromaticTuner</c> defaults to a 50 Hz floor, and the analyzer inherited it —
/// so MIDI 32 (G♯1, 51.9 Hz) was the lowest note YIN could see. <c>StandardBassMidi</c> is
/// {28, 33, 38, 43}: the open low E (MIDI 28, 41.2 Hz) and frets 1–3 above it all sat under
/// the floor, as does a 5-string's low B (MIDI 23, 30.9 Hz). The polyphonic detector covers
/// that range, so notes were still found — but with no cent-accurate YIN snap, no added-YIN
/// candidate, and <b>no legato at all</b> down there.</para>
///
/// <para>The floor is two limits, not one: YIN also searches lags only to
/// <c>WindowSize / 2</c>, so the 2048-sample legato window physically tops out at 43.1 Hz
/// whatever <c>minFreq</c> says. Both halves are pinned here.</para>
/// </summary>
public sealed class BassRegisterYinTests
{
    private const int Sr = 44100;

    private static float MidiHz(double midi) => (float)(440.0 * Math.Pow(2.0, (midi - 69.0) / 12.0));

    /// <summary>A sustained string-like tone at a fixed pitch (fundamental + partials).</summary>
    private static float[] Tone(double midi, double seconds, double amp = 0.3)
        => Glide(midi, midi, seconds, 0.0, amp);

    /// <summary>A tone that glides from one pitch to another between
    /// <paramref name="glideAt"/> and 100 ms later — a pull-off / slide with no re-attack.
    /// Phase-accumulated so the waveform stays continuous across the move.</summary>
    private const double LeadIn = 0.15;    // silence first: the onset detector needs a
                                           // pre-onset reference window to fire at all

    private static float[] Glide(double fromMidi, double toMidi, double seconds, double glideAt,
                                 double amp = 0.3)
    {
        var lead = (int)(LeadIn * Sr);
        var n = (int)(seconds * Sr);
        var y = new float[lead + n];
        double[] w = { 1.0, 0.6, 0.35, 0.2, 0.12 };
        var phase = new double[w.Length];
        for (var i = 0; i < n; i++)
        {
            var t = i / (double)Sr;
            var u = Math.Clamp((t - glideAt) / 0.1, 0.0, 1.0);
            var f0 = MidiHz(fromMidi + (toMidi - fromMidi) * u);
            double s = 0;
            for (var k = 0; k < w.Length; k++)
            {
                phase[k] += 2.0 * Math.PI * f0 * (k + 1) / Sr;
                s += w[k] * Math.Sin(phase[k]);
            }
            var env = Math.Min(1.0, t / 0.004) * Math.Exp(-t * 0.6);   // slow decay: still ringing
            y[lead + i] = (float)(amp * env * s / 2.3);
        }
        return y;
    }

    private static List<JuceGuitarEvent> Push(JuceGuitarAnalyzer analyzer, float[] audio)
    {
        var events = new List<JuceGuitarEvent>();
        for (var off = 0; off < audio.Length; off += 1024)
            events.AddRange(analyzer.Push(audio.AsSpan(off, Math.Min(1024, audio.Length - off))));
        events.AddRange(analyzer.Push(new float[(int)(0.4 * Sr)]));   // flush
        return events;
    }

    private static List<JuceGuitarEvent> Analyze(float[] audio) => Push(new JuceGuitarAnalyzer(Sr), audio);

    /// <summary>The physics, pinned directly on the tuner: BOTH the floor and the window have
    /// to move for a bass low E. This is why lowering <c>minFreq</c> alone is not the fix.</summary>
    [Fact]
    public void Tuner_BassLowE_NeedsBothALowerFloorAndALongerWindow()
    {
        var e1 = Tone(28, 0.5);                       // 41.20 Hz, τ ≈ 1070 at 44.1 kHz
        var w4096 = e1.AsSpan((int)(LeadIn * Sr) + Sr / 10, 4096);
        var w2048 = e1.AsSpan((int)(LeadIn * Sr) + Sr / 10, 2048);

        // Default 50 Hz floor: rejected outright, however long the window.
        Assert.Null(new JuceChromaticTuner(Sr, 4096).Detect(w4096));

        // Floor lowered but window too short: maxTau = 2048/2 = 1024 < 1070. And it does not
        // fail — it CLAMPS, reporting ~43.05 Hz (a semitone sharp) at high confidence, which
        // is why the analyzer's fallback keys off "the reading is at the ceiling" and not off
        // "there is no reading".
        var clamped = new JuceChromaticTuner(Sr, 2048, minFreq: 30f).Detect(w2048);
        Assert.NotNull(clamped);
        Assert.InRange(clamped!.Value.FrequencyHz, 42.5f, 43.5f);       // NOT 41.20
        Assert.True(clamped.Value.Confidence > 0.5f, "the wrong answer even looks confident");

        // Both moved: found, and cent-accurate.
        var reading = new JuceChromaticTuner(Sr, 4096, minFreq: 30f).Detect(w4096);
        Assert.NotNull(reading);
        Assert.InRange(reading!.Value.FrequencyHz, MidiHz(28) * 0.99f, MidiHz(28) * 1.01f);

        // And the 2048 window IS enough one fret up (MIDI 29, 43.65 Hz, τ ≈ 1010) — the
        // short window is only short, not useless, which is what makes a fallback the right
        // shape rather than doubling every window.
        Assert.NotNull(new JuceChromaticTuner(Sr, 2048, minFreq: 30f)
            .Detect(Tone(29, 0.5).AsSpan((int)(LeadIn * Sr) + Sr / 10, 2048)));
    }

    /// <summary>
    /// The legato tracker on the bottom of the low E string. A pull-off from A1 (MIDI 33) to
    /// the open E1 (MIDI 28) with no re-attack: under the old 50 Hz floor the tracker returned
    /// nothing at all down here, so its stability counter never accumulated and the whole
    /// hammer-on / pull-off / slide path was dead on that string.
    /// </summary>
    /// <remarks>
    /// Asserted on the tracker rather than on an emitted event: a legato EVENT additionally
    /// depends on the onset path not suppressing the hop, and in this register the flux
    /// detector's documented window-leakage ripple keeps an un-finalized onset pending — a
    /// separate defect, untouched here. What this pins is that the tracker RESOLVES the
    /// register instead of being blind to it, or (worse) confidently wrong about it: see
    /// <see cref="Tuner_BassLowE_NeedsBothALowerFloorAndALongerWindow"/>.
    /// </remarks>
    [Fact]
    public void LegatoTracker_ResolvesTheBassRegister_ViaTheLongWindow()
    {
        var bass = new JuceGuitarAnalyzer(Sr);
        Push(bass, Glide(33, 28, 1.6, glideAt: 0.5));
        Assert.True(bass.LegatoLongWindowReadings > 50,
            $"the long-window fallback never resolved the bass register: {bass.LegatoLongWindowReadings}");

        // And it stays out of the way on ordinary guitar material, where the short window
        // resolves the period on its own — the fallback costs ~3.5x.
        var guitar = new JuceGuitarAnalyzer(Sr);
        Push(guitar, Glide(52, 47, 1.6, glideAt: 0.5));
        Assert.True(guitar.LegatoLongWindowReadings <= 10,
            $"the fallback fired on guitar-register material: {guitar.LegatoLongWindowReadings}");
    }

    /// <summary>
    /// End-to-end pin for the bass low E on the onset path: MIDI 28, within a semitone in Hz.
    /// </summary>
    /// <remarks>
    /// This one passes BEFORE the fix as well, and that is worth recording rather than
    /// hiding: <c>JucePolyphonicPitch</c> covers down to 30 Hz, so the note was always FOUND
    /// — it just never got the YIN half (measured on this fixture, poly alone reports
    /// 41.241 Hz and the YIN-snapped path 41.404 Hz against a true 41.203, so accuracy is not
    /// the discriminator either). What the 50 Hz floor really cost down here was the legato
    /// tracker, which is pinned by
    /// <see cref="LegatoTracker_ResolvesTheBassRegister_ViaTheLongWindow"/>, and the
    /// YIN-added candidate whenever poly finds nothing.
    /// </remarks>
    [Fact]
    public void Onset_BassLowE_ReportsMidi28()
    {
        var audio = Tone(28, 1.2);

        var onsets = Analyze(audio).Where(e => e.Kind == JuceGuitarEventKind.Onset).ToList();

        Assert.NotEmpty(onsets);
        var e1 = Assert.Single(onsets[0].Pitches, p => p.Midi == 28);
        Assert.InRange(e1.FrequencyHz, MidiHz(27.5), MidiHz(28.5));
    }
}
