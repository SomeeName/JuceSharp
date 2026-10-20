namespace JuceSharp.Dsp;

/// <summary>
/// The <b>shipping</b> live-detection configuration — one place, so nothing can mirror it wrong.
///
/// <para><b>Why this type exists.</b> These numbers used to live in
/// <c>DotCharter.Services.Audio.LivePlay.LiveCqtVerifier</c> and were hand-copied into
/// <c>DotCharter.Analysis.AudioToChart.ChartAudioLintAnalyzer</c> and into the
/// <c>CqtStreamingEvalTests</c> harness, each with a "keep in sync" comment. Two of the
/// harness's copies silently fell behind — it defaulted <c>SubOctaveVeto = 0</c> and the
/// 384-frame (2.23 s) noise ring long after both had been ADOPTED for live play — so the one
/// harness the agent guides name as THE way to measure detection was reporting a configuration
/// the app had not shipped in months (the pre-veto ~89.7% octave-down decoy rate reads as a
/// catastrophic regression that does not exist). None of the three could reference the others:
/// Services sits above Analysis, and the JuceSharp test project cannot reference either. They
/// can all reference <b>this</b>.</para>
///
/// <para><b>Rule.</b> Live play, the offline chart lint and the eval harness all take their
/// configuration from here. Changing a value here changes what ships AND what is measured, in
/// lock-step. Do not re-type any of these numbers anywhere else — pinned in-gate by
/// <c>CqtEvalConfigTests</c> (JuceSharp.Tests) and <c>LiveCqtConfigTests</c>
/// (DotCharter.Tests), neither of which needs a dataset.</para>
/// </summary>
public static class LiveCqtConfig
{
    /// <summary>Lowest verifiable note: MIDI 21 (A0) — a 5-string bass's low B (23) and
    /// its drop-A (21), and every bass tuned down to B standard. It was 28 (E1), which made
    /// each of those an automatic Miss before the audio was even read: on the chart eval a
    /// B-standard bass scored 12.4 % with a perfect performance (2026-09-29).</summary>
    public const int MinMidi = 21;

    /// <summary>Bins from <see cref="MinMidi"/> up, at quarter-tone resolution: A0..E7, so
    /// the top stays past the 24th fret of the high E (it was E1..E7, 144 bins).</summary>
    public const int BinCount = 158;

    /// <summary>CQT hop, samples. <see cref="NoiseHistorySeconds"/> converts through it.
    /// <para><b>Not every 256 in this assembly is this constant.</b>
    /// <c>JuceOnsetDetector.HopSize</c>, <c>JuceGuitarAnalyzer</c>'s <c>LegatoHop</c> and
    /// <c>JuceNoteStream</c>'s <c>max(256, windowSize/2)</c> default are three unrelated
    /// subsystems that happen to share the value; none of them is meant to track this one,
    /// and "fixing" one of them into this constant would change the onset detector's frame
    /// rate. Only <see cref="JuceConstantQ.Hop"/> follows it.</para></summary>
    public const int Hop = 256;

    /// <summary>
    /// Kernel cap: <b>8192</b> samples (~170 ms at 48 kHz). It was 4096 — the smallest cap
    /// that held full IDMT accuracy (per-note 100 %; 94.8 % at 2048) — but IDMT is isolated
    /// notes. Below ~400 Hz every bin is capped, so a 4096 kernel smears a low note across
    /// about a major third either side, and a loud open string ringing under a melody hid
    /// the notes right beside it: one Guitar-TECHS song (an A2 drone under B2/C3/D3) scored
    /// 69 %, and 83 % at 8192. Measured 2026-09-29 (<c>tools/detection-eval</c>): songs
    /// 87.3 → 90.1 %, the quick all-sets spread 95.1 → 96.4 %, idle input still 0 %;
    /// every note a semitone off 15.0 → 17.9 %. Cost: the streaming CQT 11 → 18 % of one core
    /// in real time. The longer kernel needs ~85 ms more audio after a note before its frame
    /// exists, which still lands inside the matcher's 250 ms window.
    /// </summary>
    public const int MaxKernelSamples = 8192;

    /// <summary>Presence-gate margin over the adaptive noise floor. Measured: margin 3 drives
    /// idle phantom-confirms to 0% at zero true-positive cost. The OFFLINE chart lint's
    /// margin; live play uses <see cref="StreamingNoiseMargin"/> over the idle floor.</summary>
    public const float NoiseMargin = 3f;

    /// <summary>
    /// Presence-gate margin for LIVE play, over the streaming idle floor
    /// (<see cref="IdleSteadySeconds"/>): <b>2</b>.
    /// <para>Measured 2026-09-29 on the Guitar-TECHS DI songs (12 real recordings, 1,263
    /// charted sounds, hex-pickup MIDI ground truth): the old gate (margin 3 over a 10 s 5th
    /// percentile) confirmed <b>61.4 %</b> of perfectly played notes, because during
    /// continuous playing that percentile IS the playing level; margin 2 over the idle floor
    /// confirms <b>83.1 %</b> (no gate at all: 85.4 %). Idle hiss + hum: 0 % phantom hits
    /// either way; the player stopping half way: 3.0 % → 0.0 %; the gain raised 12 dB while
    /// idle: 21 % → 4.5 %; every note played a semitone off: 10.1 % → 14.9 %.</para>
    /// </summary>
    public const float StreamingNoiseMargin = 2f;

    /// <summary>
    /// Window over which the streaming presence gate decides the input is IDLE: <b>2 s</b>
    /// of broadband frame energy whose 95th percentile is within
    /// <see cref="IdleSteadyRatio"/> of its 5th. Hum and pickup hiss are that steady; a
    /// played phrase — attacks and decays — is not. See
    /// <see cref="JuceConstantQ.IdleSteadyFrames"/>.
    /// </summary>
    public const double IdleSteadySeconds = 2.0;

    /// <summary>q95/q05 bound under which a <see cref="IdleSteadySeconds"/> window counts as
    /// idle: <b>1.2</b>. It was 1.5 at the 4096 kernel; the 8192 kernel smooths frame energy,
    /// and at 1.5 non-stop playing read as "steady" — continuous 8th-note strumming of a ringing
    /// open chord and a dense riff both lifted the floor to the playing level (~165x the idle
    /// noise), which is the failure the idle floor exists to prevent. At 1.2 neither does, nor
    /// does a legato line or palm-muted chugging. Measured on the Guitar-TECHS songs at 8192:
    /// 1.5 → 90.2 %, 1.3 and 1.2 → 91.6 %, with the same protection — idle input 0 %, the
    /// player stopping half way 0.0 %, +12 dB gain while idle 5.4-5.6 %.</summary>
    public const float IdleSteadyRatio = 1.2f;

    /// <summary>How fast the idle floor may creep UP between idle windows while the player
    /// plays: 0.05 dB/s (3 dB a minute). It drops instantly to any quieter frame.</summary>
    public const double IdleFloorRiseDbPerSecond = 0.05;

    /// <summary>
    /// How long after its charted time a CHORD may take to finish sounding: <b>0.25 s</b>
    /// (<c>JuceChartVerifier.PostWindow</c>'s after-edge for chord tones; single notes keep
    /// the verifier's 0.14 s).
    /// <para>A strum is not simultaneous: a slow downstroke puts its top strings 100 ms or more
    /// after the charted time, past the single-note edge, and a chord whose third is the top
    /// string then failed the quality rule outright. Measured 2026-09-29 on the Guitar-TECHS
    /// chord sets grouped as strums (<c>tools/detection-eval --strum 120</c>): 79.9 % → 95.6 %
    /// (4-note chords 53 % → 87 %) with the <see cref="MaxKernelSamples"/> of the same change;
    /// songs and wrong-note false accepts unchanged. Widening the SINGLE-note window instead
    /// cost the songs a point, which is why it is chords only.</para>
    /// </summary>
    public const double ChordPostWindowAfterSeconds = 0.25;

    /// <summary>
    /// How much the charted pitch's bin must have RISEN over its pre-onset level before a
    /// sound may be credited EARLY — before its hit window closes (<c>JuceChartVerifier.PreRatio</c>
    /// for the early check only; the final check stays presence-only).
    /// <para>Crediting a hit the moment it is confirmed (median 266 → 91 ms after the note on
    /// the Guitar-TECHS songs) re-asks the verifier on every tick, and in the first ticks after a
    /// note is due the charted bin still holds the PREVIOUS note's ring: with the plain check,
    /// every note played a semitone off was accepted 27.1 % of the time instead of 17.9 %. A
    /// rise is the evidence of a fresh attack at that pitch; a sound without one simply waits
    /// for the ordinary check at the window's close.</para>
    /// </summary>
    public const double EarlyCreditRiseRatio = 1.5;

    /// <summary>
    /// The early check is not asked before <b>90 ms</b> after a sound's charted time.
    /// <para>The first frames after an attack are broadband, so a note played a semitone off can
    /// look like the charted pitch for a moment; asking from 0 ms accepted every-note-a-semitone-
    /// off 25.3 % of the time (18.6 % with no early credit at all). Measured on the Guitar-TECHS
    /// songs (hit shown after the note, median / wrong notes accepted / songs credited):
    /// no early credit 395 ms / 18.6 % / 91.8 %; from 0 ms 100 / 25.3 / 92.1; from 60 ms 100 / 23.6
    /// / 92.1; <b>from 90 ms 111 / 19.4 / 91.9</b>; two confirming ticks 133 / 19.2 / 91.8; 60 ms and
    /// two ticks 133 / 18.8 / 91.8. A rise ratio of 3 instead of 1.5 changed nothing.</para>
    /// </summary>
    public const double EarlyCreditAfterMs = 90;

    // A semitone tolerance inside fast runs was MEASURED AND REJECTED (2026-09-30, Guitar-TECHS
    // songs): +0.8 points of correctly played notes, but notes played a semitone off went
    // 19.4 % -> 31.3 % accepted and idle hum scored a note. Do not re-add it without a way
    // to keep both of those where they are.

    /// <summary>A chord with at least this many distinct tones is DENSE: it is credited at
    /// <see cref="DenseChordCoverage"/> and without the lone-third rule, because a 4-6 string
    /// strum hides its middle tones and a player expects the chord to count once it clearly
    /// rings. Measured on the Guitar-TECHS songs and slow strums: a small gain (91.9 -> 92.0 %,
    /// 97.3 -> 97.4 %) at no cost — wrong notes, octave-down and idle unchanged.</summary>
    public const int DenseChordTones = 4;

    /// <summary>Weighted-coverage bar for a <see cref="DenseChordTones"/> chord (a normal chord
    /// needs 0.5 and its lone third).</summary>
    public const double DenseChordCoverage = 0.4;

    /// <summary>A charted PINCH harmonic also counts from the attack detector's evidence (a
    /// pitched attack at its fretted pitch or one of its overtones), not only from the CQT: it
    /// rings at an overtone that depends on where the pick caught the string, is hard to hear,
    /// and nobody fakes one.</summary>
    public const bool PinchHarmonicAttackEvidence = true;

    /// <summary>Consecutive ticks the early check must confirm before a sound is credited early:
    /// 1. Two cost ~20 ms more for 0.2 points of wrong-note acceptance
    /// (<see cref="EarlyCreditAfterMs"/> holds the measurement).</summary>
    public const int EarlyCreditConfirmations = 1;

    /// <summary>
    /// When a CQT verdict is final, in ms after the charted time: the chord window
    /// (<see cref="ChordPostWindowAfterSeconds"/>) plus half the longest kernel at 44.1 kHz
    /// (<see cref="MaxKernelSamples"/>), plus one 30 Hz tick. Before that the frames the window
    /// covers do not all exist yet, so a MISS is not decided sooner — which, with hits credited
    /// early, only delays the misses.
    /// </summary>
    public static double FinalVerdictAfterMs =>
        ChordPostWindowAfterSeconds * 1000 + MaxKernelSamples / 2 * 1000.0 / 44100 + 35;

    /// <summary><see cref="IdleSteadySeconds"/> in CQT frames at <paramref name="sampleRate"/>.</summary>
    public static int IdleSteadyFrames(int sampleRate) => (int)(IdleSteadySeconds * sampleRate / Hop);

    /// <summary>Sub-octave veto ratio. <b>Adopted</b> — without it the octave-below decoy
    /// confirms ~89.7% of the time; at 0.5 that falls to 35.3%. Single notes only.
    /// See <see cref="JuceChartVerifier.SubOctaveVeto"/>.</summary>
    public const double SubOctaveVeto = 0.5;

    /// <summary>
    /// Veto-only kernels BELOW <see cref="MinMidi"/>, in bins. <b>Adopted</b> —
    /// <see cref="SubOctaveVeto"/> reads <c>bin − 12·Step</c>, which is NEGATIVE for every
    /// pitch in MIDI 28..39, so without these the veto could not fire at all in exactly the
    /// register <see cref="MinMidi"/> was widened to cover (bass low E/F/F♯/G; a 5-string's
    /// low B at MIDI 23 is a real sounding octave below a charted B1). They also un-vacuum
    /// <c>JuceChartVerifier</c>'s peak test at the two lowest bins, whose lower neighbour used
    /// to read as a hard 0 — i.e. "is a peak" was unconditionally true there.
    /// <para>26 bins = 13 semitones at quarter-tone: a full octave below MIDI 28, plus one
    /// semitone so that bin's OWN peak test has a real lower neighbour too. Costs ~+18% of the
    /// per-frame kernel taps and moves no other statistic — sub-octave bins are excluded from
    /// <c>BinCount</c> and from the broadband energy sum, so the presence gate's measured
    /// numbers are untouched. See <see cref="JuceConstantQ.SubOctaveBins"/>.</para>
    /// </summary>
    public const int SubOctaveBins = 26;

    /// <summary>Horizon of the streaming broadband noise floor. <b>Adopted</b> — 2.23 s (the
    /// bare frame ring) measures "the quietest moment while playing"; 10 s took the streaming
    /// path 89.5% → 91.9% per-note at no false-accept cost.
    /// See <see cref="JuceConstantQ.NoiseHistoryFrames"/>.</summary>
    public const double NoiseHistorySeconds = 10.0;

    /// <summary>Streaming per-bin floor percentile. <b>Measured and REJECTED</b> — must stay
    /// 1.0 (rolling max). See <see cref="JuceConstantQ.FloorPercentile"/>.</summary>
    public const float FloorPercentile = 1.0f;

    /// <summary>Relocate the peak test to a resolved harmonic below this Hz. <b>Measured and
    /// REJECTED</b> — must stay 0. See <see cref="JuceChartVerifier.HarmonicPeakBelowHz"/>.</summary>
    public const double HarmonicPeakBelowHz = 0.0;

    /// <summary>Harmonic-comb requirement. <b>Measured and REJECTED</b> — must stay 0.
    /// See <see cref="JuceChartVerifier.HarmonicComb"/>.</summary>
    public const int HarmonicComb = 0;

    /// <summary>
    ///     Frames held in <see cref="JuceConstantQ"/>'s rolling frame ring — 384, i.e.
    ///     2.23 s at <see cref="Hop"/> 256 / 44.1 kHz. It is the window every rolling
    ///     statistic that is NOT the noise floor is computed over (the per-bin rolling max),
    ///     and the fallback horizon of the noise floor itself when
    ///     <see cref="NoiseHistorySeconds"/> is 0.
    ///     <para>It was a bare <c>384</c> literal in two places in <c>JuceConstantQ</c> — the
    ///     default of <c>Reset</c> and the re-Reset a stream discontinuity performs — which is
    ///     exactly the shape that let the eval harness fall behind on this very number. Same
    ///     value, one name.</para>
    /// </summary>
    public const int RingFrames = 384;

    /// <summary>
    ///     Temporal percentile of broadband frame energy that IS the adaptive noise floor
    ///     (minimum statistics): the 5th, so it lands on the total energy when nothing is
    ///     being played rather than on a quiet moment inside a phrase. Read by
    ///     <see cref="JuceConstantQ.NoiseFloorEnergy"/> over both horizons.
    /// </summary>
    public const float NoisePercentile = 0.05f;

    /// <summary>
    ///     Per-bin relative floor: a bin below this fraction of the rolling per-bin maximum is
    ///     not present. The <c>JuceConstantQ</c> constructor default, and what live play,
    ///     the chart lint and the eval harness all run.
    /// </summary>
    public const float RelFloor = 0.10f;

    /// <summary><see cref="NoiseHistorySeconds"/> expressed in CQT frames at
    /// <paramref name="sampleRate"/> — 1,722 at 44.1 kHz (10·44100/256 = 1722.66, truncated),
    /// 1,875 at 48 kHz.</summary>
    public static int NoiseHistoryFrames(int sampleRate) => (int)(NoiseHistorySeconds * sampleRate / Hop);
}
