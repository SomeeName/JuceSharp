namespace JuceSharp.Dsp;

/// <summary>Outcome of one <see cref="JuceChartVerifier.Verify"/> call.</summary>
public readonly record struct JuceVerifyResult(bool Hit, float Confidence, float Level);

/// <summary>
/// Chart-informed note verification — the "Binary Match Mask" (how Rocksmith
/// works). A rhythm game already KNOWS which note is expected at which time,
/// so it never transcribes blindly; it just confirms each charted note: did
/// the expected pitch's energy spike at the expected moment? That turns the
/// impossible blind-polyphony problem into an easy per-note yes/no.
///
/// <para>Two ideas make it robust (see <c>PORTING.md §1.2</c>): the
/// quarter-tone <see cref="JuceConstantQ"/> keeps a wrong fret two bins away
/// for a sharp peak test, and each note is checked against its OWN bin's floor
/// (and, optionally, its pre-onset baseline) so it works inside a chord where
/// global loudness is already high.</para>
///
/// <para>Validated on GuitarSet mono mix: ~98% of solo notes / ~97% of chord
/// notes confirmed per-note at ~11% wrong-fret false-accept. Presence-based by
/// default (a held/re-struck note still counts); set <see cref="PreRatio"/> &gt; 0
/// for stricter attack-gated verification.</para>
/// </summary>
/// <remarks>
/// <para><b>Thread-safety:</b> instances are <b>not</b> thread-safe, and neither is the
/// <see cref="JuceConstantQ"/> they read. <see cref="Verify"/> calls <c>Floor</c>,
/// <c>NoiseFloorEnergy</c> and <c>FrameEnergy</c> per note, all of which mutate the
/// transform's memoisation and percentile scratch — so two concurrent <see cref="Verify"/>
/// calls against one transform can each see a torn percentile. The verifier and every
/// <c>Push</c> into its transform belong on ONE thread.</para>
/// </remarks>
public sealed class JuceChartVerifier
{
    private readonly JuceConstantQ _cqt;

    /// <summary>&gt;0 also demands an energy RISE vs the pre-onset baseline
    /// (rejects held/re-struck notes). 0 = presence-only (recommended — this one
    /// choice took per-event 92.5%→96% in the testbed).</summary>
    public double PreRatio { get; init; } = 0.0;

    /// <summary>Confirmation (hit) window around the charted time: look from
    /// t-Before to t+After. Wider = more forgiving on timing.</summary>
    public (double Before, double After) PostWindow { get; init; } = (0.04, 0.14);

    /// <summary>Pre-onset baseline window (only used when <see cref="PreRatio"/> &gt; 0).</summary>
    public (double Before, double After) PreWindow { get; init; } = (0.18, 0.05);

    /// <summary>Rescue a masked chord tone via its octave partial. Raises recall
    /// ~2% but also raises wrong-fret false-accepts; off by default.</summary>
    public bool HarmonicSupport { get; init; } = false;

    /// <summary>Presence gate: broadband energy must exceed the adaptive noise
    /// floor by this factor for a note to confirm. **On by default (3)** because
    /// the raw ungated verifier phantom-confirms ~2/3 of charted notes against
    /// pure input noise/hum — a footgun for live scoring. Independently measured:
    /// margin 3 drives idle phantom-confirms to 0% at zero true-positive cost. Set
    /// to 0 only for offline analysis of audio you know is never idle.
    /// <para>The default READS <see cref="LiveCqtConfig.NoiseMargin"/> rather than re-typing
    /// it. It was a bare <c>3f</c>, and <c>CqtEvalConfigTests</c> compares the eval harness
    /// against the CONFIG and never against this default — so the product default and the
    /// measured default could have drifted apart invisibly, which is the exact failure
    /// <see cref="LiveCqtConfig"/> was created to end.</para></summary>
    public float NoiseMargin { get; init; } = LiveCqtConfig.NoiseMargin;

    /// <summary>Harmonic-comb requirement: after the fundamental peaks, require at
    /// least this many of the note's harmonics (octave, +oct-fifth, 2-oct, …) to
    /// also be present. 0 = off. A real string is rich in harmonics; a decoy
    /// peaking at the fundamental via another string's harmonic bleed rarely fills
    /// the comb, so this cuts wrong-note false-accepts (notably the tritone case).</summary>
    public int HarmonicComb { get; init; } = 0;

    // Overtone offsets in semitones: 2f0 (octave), 3f0 (oct+fifth), 4f0, 5f0.
    private static readonly int[] HarmonicSemis = { 12, 19, 24, 28 };

    /// <summary>
    /// Relocate the discriminating PEAK test to a resolved HARMONIC for expected notes
    /// below this frequency (Hz). 0 = off.
    /// <para><b>Why.</b> <see cref="JuceConstantQ"/> caps its kernel length, so the full
    /// constant-Q — and with it the quarter-tone main lobe — is only achieved above
    /// <c>f = Q·sr/L</c> (≈367 Hz at L=4096, sr=44100). Below that the achieved Q
    /// collapses (at E2 the main lobe is ≈3.2 semitones wide), so <see cref="IsPeak"/>'s
    /// ±1-semitone comparison bins sit INSIDE the note's own main lobe and the peak test
    /// is close to vacuous. The note's HARMONICS are resolved, though: a semitone gap at
    /// k·f0 is k times wider in Hz. So run the same ±1-semitone peak test up there.</para>
    /// <para><b>This is not the harmonic comb</b> (<see cref="HarmonicComb"/>), which was
    /// measured and rejected: that was an AND-gated presence test requiring N harmonics to
    /// exist. This MOVES one peak test to a bin that is actually resolved.</para>
    ///
    /// <para><b>MEASURED — off by default. It works exactly as designed, but it trades the
    /// wrong way.</b> On the STREAMING path (<c>CqtStreamingEvalTests</c>, cap=120,
    /// 120 tracks / 893 notes), 0 → 370 Hz:</para>
    /// <list type="bullet">
    ///   <item><description>per-note   89.5% → <b>93.7%</b> (+4.2)</description></item>
    ///   <item><description>per-event  88.0% → <b>92.7%</b> (+4.7)</description></item>
    ///   <item><description>wrong-fret FA 3.2% → <b>5.6%</b> (+2.4, WORSE)</description></item>
    ///   <item><description>tritone FA    6.6% → <b>8.9%</b> (+2.3, WORSE)</description></item>
    ///   <item><description>by register: f0&lt;250 Hz recall 87.6% → 93.2%; the ≥370 Hz band is
    ///   bit-identical (4.9% / 12.0% FA), which is the sanity check that it only fires where
    ///   it is supposed to.</description></item>
    /// </list>
    /// <para>It is a pure recall↔false-accept trade: an OR can only ADD confirmations. False
    /// accepts are the metric we care most about, and <see cref="JuceConstantQ.NoiseHistoryFrames"/>
    /// bought +2.4 per-note for ~0 FA — strictly better. So this stays off.</para>
    /// <para><b>Its stated premise was also falsified.</b> The plan predicted false-accepts
    /// would concentrate BELOW 370 Hz (where the kernel cap collapses the achieved Q and the
    /// ±1-semitone peak test goes vacuous). Measured, the opposite is true — FA is LOWEST in
    /// the low band and WORST at the top (streaming, cap=120): wrong-fret FA 2.1% / 4.8% /
    /// 4.9% and tritone FA 5.3% / 6.4% / <b>12.0%</b> across &lt;250 / 250–370 / ≥370 Hz.
    /// What IS real down low is a RECALL deficit (87.6% vs 92.2/92.3%), and that is the only
    /// thing this rule actually fixes. Re-enable it only if you are optimising recall and can
    /// afford the FA — and re-measure, at a stated cap.</para>
    /// </summary>
    public double HarmonicPeakBelowHz { get; init; } = 0.0;

    /// <summary>
    /// Reject a confirmation whose energy is better explained as the SECOND HARMONIC of a
    /// note an octave BELOW. 0 = off; otherwise the ratio the sub-octave bin must reach,
    /// relative to the target bin, to veto (≈0.5 = "the octave below is at least half as
    /// loud as what we are confirming").
    /// <para><b>Why.</b> Measured on IDMT: a decoy an octave ABOVE a played note confirms
    /// ~90% of the time — even when that octave is annotated SILENT. A presence test simply
    /// cannot tell "played X" from "played X−12" on its own, because X−12's 2nd harmonic
    /// sits exactly on X's bin, is loud, and is a real local peak. What DOES separate them
    /// is the bin an octave below: a genuinely played X has (near) nothing there — strings
    /// have no subharmonic — while a played X−12 has its own fundamental there.</para>
    /// <para><b>Only safe for MONOPHONIC expectations.</b> Guitar chords are full of real
    /// octaves (power chords, barre chords), so a chord tone whose octave-below is genuinely
    /// fretted would be vetoed away. <c>LiveCqtVerifier</c> passes
    /// <c>allowSubOctaveVeto: false</c> for chord tones.</para>
    /// <para><b>It needs somewhere to look.</b> The test reads <c>bin − 12·Step</c>, so it can
    /// only fire where that bin exists. Give the CQT
    /// <see cref="JuceConstantQ.SubOctaveBins"/> (the shipping config does, via
    /// <see cref="LiveCqtConfig.SubOctaveBins"/>); without them this is inoperative for the
    /// bottom octave of the range — which, at <c>fMinMidi = 28</c>, is the whole bass low E
    /// string, the register that floor was lowered for in the first place.</para>
    /// </summary>
    public double SubOctaveVeto { get; init; } = 0.0;

    // k → the harmonic's offset in semitones, rounded to the tempered grid.
    // k=2:+12.00  k=3:+19.02  k=4:+24.00  k=6:+31.02 — all land on a bin.
    // k=5 (+27.86) and k=7 (+33.69) are deliberately absent: the 5th partial sits
    // ~13.7 cents below its bin and the 7th ~31 cents, so their energy straddles two
    // quarter-tone bins and the peak test blurs.
    private static readonly int[] HarmonicPeakKs = { 2, 3, 4, 6 };

    private static int HarmonicSemisForK(int k) => k switch
    {
        2 => 12,
        3 => 19,
        4 => 24,
        6 => 31,
        _ => 0,
    };

    private static double MidiToHz(double midi) => PitchMath.MidiToHz(midi);

    /// <summary>
    /// Wrap a CQT front end for verification. The verifier holds the transform but does not
    /// own it: the caller keeps driving it with <c>Push</c> (live) or <c>AnalyzeWhole</c>
    /// (offline), and every time and bin the verifier reports is on that instance's axis.
    /// </summary>
    /// <param name="cqt">The already-configured transform to read magnitudes and floors from.</param>
    /// <exception cref="ArgumentNullException"><paramref name="cqt"/> is <c>null</c>.</exception>
    public JuceChartVerifier(JuceConstantQ cqt) => _cqt = cqt ?? throw new ArgumentNullException(nameof(cqt));

    /// <summary>True if the expected <paramref name="soundingMidi"/> spiked at
    /// charted time <paramref name="tSeconds"/> (the Binary Match Mask).</summary>
    /// <param name="soundingMidi">The expected sounding pitch (MIDI).</param>
    /// <param name="tSeconds">The charted time, on the CQT's own sample axis.</param>
    /// <param name="allowHarmonicPeak">
    /// Whether <see cref="HarmonicPeakBelowHz"/> may rescue this note via a harmonic.
    /// Pass <c>false</c> for PALM-MUTED notes: palm muting strips the upper partials, so
    /// the harmonic the test would relocate to is not reliably there.
    /// </param>
    /// <param name="allowSubOctaveVeto">
    /// Whether <see cref="SubOctaveVeto"/> may reject this note. Pass <c>false</c> for CHORD
    /// TONES — guitar chords contain real octaves, so the veto would reject a tone whose
    /// octave-below is genuinely fretted.
    /// </param>
    public JuceVerifyResult Verify(double soundingMidi, double tSeconds,
                                   bool allowHarmonicPeak = true, bool allowSubOctaveVeto = true)
    {
        int b = _cqt.BinForMidi(soundingMidi);
        if (b < 0 || b >= _cqt.BinCount) return new JuceVerifyResult(false, 0f, 0f);

        long i0 = _cqt.FrameOfSeconds(tSeconds - PostWindow.Before);
        long i1 = _cqt.FrameOfSeconds(tSeconds + PostWindow.After);
        if (i1 < i0) return new JuceVerifyResult(false, 0f, 0f);

        long fi = -1;
        float level = 0f;
        for (long f = i0; f <= i1; f++)
        {
            float m = _cqt.Magnitude(b, f);
            if (m > level || fi < 0) { level = m; fi = f; }
        }
        if (fi < 0) return new JuceVerifyResult(false, 0f, 0f);

        float gate = _cqt.Floor(b);
        if (PreRatio > 0)
            gate = MathF.Max(gate, (float)PreRatio * RoseLevel(b, tSeconds));
        if (level <= gate) return new JuceVerifyResult(false, Confidence(level, gate), level);

        // Presence gate (anti-phantom): broadband energy must rise clearly above
        // the idle noise floor in the window. Idle input noise/hum sits stably at
        // that floor (summing 120 bins averages out), so it can't clear the margin
        // and phantom-confirm; a played note spikes total energy well above it.
        // Gain-independent — this is what stops confirms while nothing is played.
        if (NoiseMargin > 0)
        {
            float presence = 0f;
            for (long f = i0; f <= i1; f++) presence = MathF.Max(presence, _cqt.FrameEnergy(f));
            if (presence <= NoiseMargin * _cqt.NoiseFloorEnergy())
                return new JuceVerifyResult(false, 0f, level);
        }

        // Is this bin better explained as the 2nd harmonic of a note an octave below?
        // (A played string has no subharmonic, so a real fundamental has nothing there.)
        bool vetoed = allowSubOctaveVeto && SubOctaveVeto > 0 && IsSubOctaveDominant(b, fi, level);

        if (IsPeak(b, fi))
        {
            if (vetoed) return new JuceVerifyResult(false, 0f, level);
            if (HarmonicComb > 0 && HarmonicsPresent(b, fi) < HarmonicComb)
                return new JuceVerifyResult(false, 0f, level); // fundamental only — likely bleed/decoy
            return new JuceVerifyResult(true, Confidence(level, gate), level);
        }
        if (vetoed) return new JuceVerifyResult(false, 0f, level);

        // The fundamental's bin is unresolved down here (see HarmonicPeakBelowHz), so the
        // peak test above was near-vacuous. Re-run it at the lowest harmonic that IS
        // resolved — same ±1-semitone test, at a bin where a semitone is k times wider.
        if (allowHarmonicPeak && HarmonicPeakBelowHz > 0)
        {
            double f0 = MidiToHz(soundingMidi);
            if (f0 > 0 && f0 < HarmonicPeakBelowHz)
            {
                int need = (int)Math.Ceiling(HarmonicPeakBelowHz / f0);
                foreach (var k in HarmonicPeakKs)
                {
                    if (k < need) continue;
                    int hb = b + HarmonicSemisForK(k) * _cqt.Step;
                    if (hb >= _cqt.BinCount) break;
                    if (_cqt.Magnitude(hb, fi) > _cqt.Floor(hb) && IsPeak(hb, fi))
                        return new JuceVerifyResult(true, Confidence(level, gate), level);
                    break;   // only the lowest resolved harmonic — higher ones are weaker
                }
            }
        }

        // A masked chord tone may not be a strict peak; accept if its octave
        // partial is a peak that is ALSO meaningfully loud.
        if (HarmonicSupport)
        {
            int octave = b + 12 * _cqt.Step;
            // At the SHIPPING PreRatio of 0 the right-hand side used to be `> 0`, so any
            // non-zero magnitude passed and the "also rose" half this comment used to
            // promise was not running at all — a safety claim about a vacuous check. There
            // is no baseline to rise from when PreRatio is 0, so require the octave to clear
            // its OWN bin floor instead: the same gate the fundamental passes, and the
            // thing that actually distinguishes a real octave partial from bin noise.
            // HarmonicSupport is off by default, so this moves no shipping number.
            var octaveGate = PreRatio > 0
                ? (float)PreRatio * RoseLevel(octave, tSeconds)
                : _cqt.Floor(octave);
            if (IsPeak(octave, fi) && _cqt.Magnitude(octave, fi) > octaveGate)
                return new JuceVerifyResult(true, Confidence(level, gate), level);
        }
        return new JuceVerifyResult(false, Confidence(level, gate), level);
    }

    // MIN over the pre-onset window (not median): a repeated/legato note that
    // was ringing still has a quiet dip to rise from, so it isn't wrongly dropped.
    private float RoseLevel(int bin, double t)
    {
        long lo = _cqt.FrameOfSeconds(t - PreWindow.Before);
        long hi = _cqt.FrameOfSeconds(t - PreWindow.After);
        if (hi <= lo) hi = lo + 1;
        float min = float.MaxValue;
        for (long f = lo; f < hi; f++) min = MathF.Min(min, _cqt.Magnitude(bin, f));
        return min == float.MaxValue ? 0f : min;
    }

    // Peak vs the SEMITONE-away bins (±step). Looser than "loudest in the whole
    // window" so a masked chord tone still passes, while an adjacent wrong fret
    // (whose target bin sits a semitone away, under the real note) is rejected.
    //
    // NOTE the lower bound is the CQT's lowest addressable bin, not 0: with
    // JuceConstantQ.SubOctaveBins > 0 the bins below fMin are real, which is what lets this
    // run on the sub-octave bin AND what stops the test being half-vacuous at bins 0/1 (a
    // missing lower neighbour reads as magnitude 0, so `c >= 0` was unconditionally true —
    // the lowest semitone of the range got a free pass).
    private bool IsPeak(int bin, long frame)
    {
        if (bin < _cqt.LowestBin || bin >= _cqt.BinCount) return false;
        float c = _cqt.Magnitude(bin, frame);
        return c >= _cqt.Magnitude(bin - _cqt.Step, frame)
            && c >= _cqt.Magnitude(bin + _cqt.Step, frame);
    }

    // True when the bin an OCTAVE BELOW carries its own (peaked, above-floor) fundamental
    // at least SubOctaveVeto × as loud as the bin we are about to confirm — i.e. the energy
    // at the target is most likely that lower note's 2nd harmonic, not a played fundamental.
    private bool IsSubOctaveDominant(int bin, long frame, float level)
    {
        int bs = bin - 12 * _cqt.Step;
        // The bound is the CQT's LOWEST bin, not 0. It used to be 0, which meant the veto
        // silently never fired for the bottom octave of the range — MIDI 28..39 at the
        // shipping fMinMidi = 28, i.e. exactly the register E1 support was added for. Give
        // the CQT JuceConstantQ.SubOctaveBins and bs is addressable for the whole charted
        // range; leave it at 0 and this returns false down there, as before.
        if (bs < _cqt.LowestBin || level <= 0f) return false;
        float sub = _cqt.Magnitude(bs, frame);
        return sub > _cqt.Floor(bs)
            && sub >= (float)SubOctaveVeto * level
            && IsPeak(bs, frame);
    }

    // Count how many of the note's overtone bins carry energy above their own
    // floor at the confirmation frame — a played string's harmonic signature.
    private int HarmonicsPresent(int b, long fi)
    {
        int count = 0;
        foreach (var semis in HarmonicSemis)
        {
            int hb = b + semis * _cqt.Step;
            if (hb >= _cqt.BinCount) break;   // overtone above the CQT range
            if (_cqt.Magnitude(hb, fi) > _cqt.Floor(hb)) count++;
        }
        return count;
    }

    private static float Confidence(float level, float gate)
        => level <= 0f ? 0f : Math.Clamp(1f - gate / level, 0f, 1f);
}
