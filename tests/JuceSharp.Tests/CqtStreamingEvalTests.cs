using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The eval that measures the code we actually SHIP.
///
/// <para><b>Why this exists.</b> <see cref="CqtDatasetEvalTests"/> calls
/// <c>JuceConstantQ.AnalyzeWhole</c> — the OFFLINE path. Live play never runs it:
/// it runs <c>Reset()</c> + <c>Push()</c>. The two do not merely differ in
/// buffering, they compute <b>different statistics</b>:</para>
/// <list type="table">
///   <item><term>Per-bin floor</term><description>offline = 0.10 × 95th percentile
///   over the WHOLE clip; streaming = 0.10 × rolling <b>MAX</b> over the last 384
///   frames (2.23 s).</description></item>
///   <item><term>Noise floor</term><description>offline = 5th percentile of frame
///   energy over the WHOLE clip; streaming = 5th percentile over a rolling
///   <see cref="LiveCqtConfig.NoiseHistorySeconds"/> horizon (10 s — the bare 2.23 s ring
///   measures "the quietest moment while playing").</description></item>
/// </list>
/// <para>So every accuracy number quoted from the offline eval describes code that
/// does not ship. This harness drives the streaming path exactly as
/// <c>LivePlayViewModel.Tick</c> + <c>LiveNoteMatcher.AdvanceTo</c> do — push capture
/// blocks on the raw capture axis, then pull-<c>Verify()</c> each note as its hit
/// window closes — and reports offline vs streaming side by side.</para>
///
/// <para>It also reports the two diagnostics that decide what to fix next: accuracy /
/// false-accepts split by <b>pitch register</b>, and false-accepts as a function of
/// <b>decoy interval</b> (the shape of that curve names the mechanism).</para>
///
/// <para><b>An un-set run measures exactly what ships.</b> Every knob defaults to
/// <see cref="LiveCqtConfig"/> (see <see cref="EvalKnobs"/>); the <c>GND_*</c> env vars exist
/// to explore alternatives and the report says so when one is set. This was NOT true for
/// months — the harness defaulted the sub-octave veto off and the noise horizon to the bare
/// 2.23 s ring after both had been adopted, so an un-set run reported the pre-veto ~89.7%
/// octave-down decoy rate as if it were current. <c>CqtEvalConfigTests</c> pins it now.</para>
///
/// <para>Opt-in: env <c>GND_IDMT_ROOT</c>, <c>GND_EVAL_CAP</c>. Results are
/// sample-dependent — only ever compare runs at the SAME cap.</para>
/// </summary>
public sealed class CqtStreamingEvalTests
{
    private readonly ITestOutputHelper _out;
    public CqtStreamingEvalTests(ITestOutputHelper output) => _out = output;

    // ─── the live configuration ────────────────────────────────────────────
    // Read from JuceSharp.Dsp.LiveCqtConfig — the SAME constants
    // DotCharter.Services' LiveCqtVerifier.CreateStreaming() builds from. This project
    // cannot reference DotCharter.Services (wrong direction), and the hand-copied mirror
    // that used to sit here is exactly what drifted: it kept defaulting the sub-octave veto
    // and the noise horizon OFF long after both had shipped. Do not re-type these numbers.
    private const int LiveMinMidi = LiveCqtConfig.MinMidi;         // A0
    private const int LiveBinCount = LiveCqtConfig.BinCount;       // A0..E7
    private const int LiveMaxKernel = LiveCqtConfig.MaxKernelSamples; // ~93 ms

    /// <summary>Capture block the live tick pushes with (a realistic device buffer).</summary>
    private const int CaptureBlock = 512;

    /// <summary>When the matcher pull-verifies: the CQT-mode hit window's late edge
    /// (<c>LiveHitWindows</c> holds it to ≥230 ms so the note is fully formed —
    /// 140 ms post-window + 46 ms half-kernel).</summary>
    private const double VerifyDelaySec = 0.230;

    /// <summary>Decoy intervals for the false-accept curve (Task 2b). The SHAPE names
    /// the mechanism: peaks at ±7/±12 = harmonic bleed; peaks at ±1/±2 = resolution
    /// smear; flat = broadband transient.</summary>
    private static readonly int[] DecoyIntervals = { -12, -7, -6, -5, -3, -2, -1, 1, 2, 3, 5, 6, 7, 12 };

    // Register bands (Task 2a). The prediction under test: false-accepts concentrate
    // BELOW 370 Hz, because the kernel cap (4096) means full constant-Q is only
    // achieved above f = Q·sr/L = 34.13 × 44100 / 4096 ≈ 367 Hz. Below that the
    // achieved Q collapses and IsPeak's ±1-semitone comparison bins fall INSIDE the
    // main lobe, making the peak test close to vacuous.
    private const double MidLoHz = 250.0;
    private const double MidHiHz = 370.0;
    private static int Band(double midi)
    {
        var hz = 440.0 * Math.Pow(2.0, (midi - 69.0) / 12.0);
        return hz < MidLoHz ? 0 : hz < MidHiHz ? 1 : 2;
    }
    private static readonly string[] BandNames = { "f0 <250 Hz  ", "f0 250-370Hz", "f0 >=370 Hz " };

    private sealed class Tally
    {
        public int Notes, Hits;
        public readonly int[] Decoy = new int[DecoyIntervals.Length];      // confirmed (clean only)
        public readonly int[] DecoyN = new int[DecoyIntervals.Length];     // decoys that were SILENT
        public readonly int[] DecoyRaw = new int[DecoyIntervals.Length];   // confirmed, sounding or not
        public void Add(Tally o)
        {
            Notes += o.Notes; Hits += o.Hits;
            for (int i = 0; i < Decoy.Length; i++)
            {
                Decoy[i] += o.Decoy[i]; DecoyN[i] += o.DecoyN[i]; DecoyRaw[i] += o.DecoyRaw[i];
            }
        }
    }

    [Fact]
    public void StreamingVerifier_MeasuredAgainstOffline_OnIdmtDataset()
    {
        var root = IdmtDataset.ResolveRoot();
        if (root is null)
            Assert.Skip("IDMT-SMT-GUITAR_V2 not found (set GND_IDMT_ROOT to run this eval).");

        var pairs = IdmtDataset.PairTracks(root!);
        Assert.NotEmpty(pairs);

        var cap = int.TryParse(Environment.GetEnvironmentVariable("GND_EVAL_CAP"), out var c) ? c : 60;

        // Every knob DEFAULTS to what ships (LiveCqtConfig); the GND_* env vars are for
        // EXPLORING alternatives, not for reproducing the app. Pinned in-gate by
        // CqtEvalConfigTests.
        var knobs = EvalKnobs.FromEnvironment();
        var noiseMargin = knobs.NoiseMargin;
        var floorPct = knobs.FloorPercentile;
        var harmBelow = knobs.HarmonicPeakBelowHz;
        var subOct = knobs.SubOctaveVeto;

        var sample = IdmtDataset.Stride(pairs, cap);

        // Per-band tallies for each path; index [band], plus a rolled-up total.
        var off = new[] { new Tally(), new Tally(), new Tally() };
        var str = new[] { new Tally(), new Tally(), new Tally() };
        int events = 0, eventHitsOff = 0, eventHitsStr = 0;
        int tracks = 0, skippedEdge = 0;

        foreach (var (wav, xml) in sample)
        {
            var y = IdmtDataset.ReadWavMono(wav, out int sr);
            if (y.Length == 0) continue;
            var refs = IdmtDataset.Parse(xml);
            if (refs.Count == 0) continue;

            // Only judge notes whose whole confirmation window is inside the clip on
            // BOTH paths, so the two see an identical note set and the comparison is
            // apples-to-apples. Head: the streaming CQT's first computable frame needs
            // maxHalf samples. Tail: the pull-verify happens VerifyDelaySec after onset.
            double clipSec = y.Length / (double)sr;
            double headSec = (LiveMaxKernel / 2.0) / sr + 0.04;   // half-kernel + PostWindow.Before
            var judged = refs.Where(r => r.Onset >= headSec && r.Onset + VerifyDelaySec <= clipSec)
                             .OrderBy(r => r.Onset).ToList();
            skippedEdge += refs.Count - judged.Count;
            if (judged.Count == 0) continue;

            // ── offline path (what the current eval measures) ──
            var cqtOff = new JuceConstantQ(sr, fMinMidi: LiveMinMidi, binCount: LiveBinCount,
                                           maxKernelSamples: LiveMaxKernel);
            cqtOff.AnalyzeWhole(y);
            var vOff = new JuceChartVerifier(cqtOff)
                { NoiseMargin = noiseMargin, HarmonicPeakBelowHz = harmBelow, SubOctaveVeto = subOct };

            // The sub-octave veto is only safe where the expectation is MONOPHONIC (guitar
            // chords contain real octaves). Mirror LiveCqtVerifier: a note struck together
            // with others is a chord tone and opts out.
            var chordTone = new HashSet<IdmtDataset.Ref>();
            foreach (var g in IdmtDataset.ClusterEvents(judged, 0.030))
                if (g.Count > 1) foreach (var n in g) chordTone.Add(n);
            bool Mono(IdmtDataset.Ref n) => !chordTone.Contains(n);

            var hitOff = new Dictionary<IdmtDataset.Ref, bool>();
            foreach (var n in judged)
            {
                var b = Band(n.Midi);
                off[b].Notes++;
                var hit = vOff.Verify(n.Midi, n.Onset, allowSubOctaveVeto: Mono(n)).Hit;
                if (hit) off[b].Hits++;
                hitOff[n] = hit;
                for (int i = 0; i < DecoyIntervals.Length; i++)
                {
                    var dm = n.Midi + DecoyIntervals[i];
                    var confirmed = vOff.Verify(dm, n.Onset, allowSubOctaveVeto: Mono(n)).Hit;
                    if (confirmed) off[b].DecoyRaw[i]++;
                    if (IdmtDataset.Sounding(refs, dm, n.Onset - 0.04, n.Onset + 0.14)) continue;
                    off[b].DecoyN[i]++;
                    if (confirmed) off[b].Decoy[i]++;
                }
            }

            // ── streaming path (what actually ships) ──
            // Reset + Push in realistic capture blocks on the raw capture axis, and
            // pull-Verify each note as its hit window closes — the exact shape of
            // LivePlayViewModel.Tick feeding LiveNoteMatcher.AdvanceTo.
            var cqtStr = new JuceConstantQ(sr, hop: LiveCqtConfig.Hop,
                                           fMinMidi: LiveMinMidi, binCount: LiveBinCount,
                                           maxKernelSamples: LiveMaxKernel,
                                           floorPercentile: floorPct,
                                           noiseHistoryFrames: knobs.NoiseHistoryFrames(sr),
                                           subOctaveBins: LiveCqtConfig.SubOctaveBins,
                                           idleSteadyFrames: knobs.IdleSteadyFrames(sr));
            cqtStr.Reset(0);
            var vStr = new JuceChartVerifier(cqtStr)
                { NoiseMargin = noiseMargin, HarmonicPeakBelowHz = harmBelow, SubOctaveVeto = subOct };

            var hitStr = new Dictionary<IdmtDataset.Ref, bool>();
            int next = 0;
            for (int o = 0; o < y.Length; o += CaptureBlock)
            {
                int len = Math.Min(CaptureBlock, y.Length - o);
                cqtStr.Push(y.AsSpan(o, len), o);
                double streamSec = (o + len) / (double)sr;

                // Every note whose window has now closed gets verified against the
                // ring as it stands at THIS moment — rolling floors and all.
                while (next < judged.Count && judged[next].Onset + VerifyDelaySec <= streamSec)
                {
                    var n = judged[next++];
                    var b = Band(n.Midi);
                    str[b].Notes++;
                    var hit = vStr.Verify(n.Midi, n.Onset, allowSubOctaveVeto: Mono(n)).Hit;
                    if (hit) str[b].Hits++;
                    hitStr[n] = hit;
                    for (int i = 0; i < DecoyIntervals.Length; i++)
                    {
                        var dm = n.Midi + DecoyIntervals[i];
                        var confirmed = vStr.Verify(dm, n.Onset, allowSubOctaveVeto: Mono(n)).Hit;
                        if (confirmed) str[b].DecoyRaw[i]++;
                        if (IdmtDataset.Sounding(refs, dm, n.Onset - 0.04, n.Onset + 0.14)) continue;
                        str[b].DecoyN[i]++;
                        if (confirmed) str[b].Decoy[i]++;
                    }
                }
            }
            Assert.Equal(judged.Count, next);   // every judged note's window closed in-clip

            // Per-event ('half' threshold), both paths, over the judged notes only.
            foreach (var grp in IdmtDataset.ClusterEvents(judged, 0.030))
            {
                events++;
                int need = (int)Math.Ceiling(grp.Count / 2.0);
                if (grp.Count(n => hitOff[n]) >= need) eventHitsOff++;
                if (grp.Count(n => hitStr[n]) >= need) eventHitsStr++;
            }
            tracks++;
        }

        var offAll = new Tally(); foreach (var t in off) offAll.Add(t);
        var strAll = new Tally(); foreach (var t in str) strAll.Add(t);
        Assert.True(tracks >= 5, $"too few tracks evaluated: {tracks}");
        Assert.True(offAll.Notes == strAll.Notes && offAll.Notes > 0,
            $"paths judged different note sets: offline={offAll.Notes} streaming={strAll.Notes}");

        int i1 = Array.IndexOf(DecoyIntervals, 1);
        int i6 = Array.IndexOf(DecoyIntervals, 6);
        double Pct(int n, int d) => n / (double)Math.Max(1, d);

        _out.WriteLine($"IDMT streaming-vs-offline eval — cap={cap}, tracks={tracks}, notes={offAll.Notes}, "
                     + $"events={events}, edge-excluded={skippedEdge}");
        _out.WriteLine($"  config: fMin=MIDI {LiveMinMidi} (A0), bins={LiveBinCount}, maxKernel={LiveMaxKernel} "
                     + $"(~{LiveMaxKernel / 44.1:0} ms), noiseMargin={noiseMargin}, block={CaptureBlock}");
        _out.WriteLine($"  knobs : floorPct={floorPct} (1.0=rolling MAX), noiseSeconds={knobs.NoiseHistorySeconds} "
                     + $"({knobs.NoiseHistoryFrames(44100)} frames @44.1k), harmonicPeakBelowHz={harmBelow} (0=off), "
                     + $"subOctaveVeto={subOct} (0=off, mono only)"
                     + (knobs == EvalKnobs.Shipping ? "  [= SHIPPING]" : "  [OVERRIDDEN — not the ship config]"));
        _out.WriteLine("");
        _out.WriteLine("  metric                 OFFLINE (AnalyzeWhole)   STREAMING (Push — ships)");
        _out.WriteLine($"  per-note               {Pct(offAll.Hits, offAll.Notes),12:P1}            {Pct(strAll.Hits, strAll.Notes),12:P1}");
        _out.WriteLine($"  per-event              {Pct(eventHitsOff, events),12:P1}            {Pct(eventHitsStr, events),12:P1}");
        _out.WriteLine($"  wrong-fret FA (+1)     {Pct(offAll.Decoy[i1], offAll.DecoyN[i1]),12:P1}            {Pct(strAll.Decoy[i1], strAll.DecoyN[i1]),12:P1}");
        _out.WriteLine($"  tritone FA (+6)        {Pct(offAll.Decoy[i6], offAll.DecoyN[i6]),12:P1}            {Pct(strAll.Decoy[i6], strAll.DecoyN[i6]),12:P1}");
        _out.WriteLine($"    (FA denominators are decoys ANNOTATED SILENT: +1 {strAll.DecoyN[i1]}/{strAll.Notes}, +6 {strAll.DecoyN[i6]}/{strAll.Notes})");
        _out.WriteLine($"  RAW FA (+1), decoy may be sounding — the historical metric: "
                     + $"{Pct(offAll.DecoyRaw[i1], offAll.Notes):P1} offline / {Pct(strAll.DecoyRaw[i1], strAll.Notes):P1} streaming");
        _out.WriteLine($"  RAW FA (+6): {Pct(offAll.DecoyRaw[i6], offAll.Notes):P1} offline / {Pct(strAll.DecoyRaw[i6], strAll.Notes):P1} streaming");
        _out.WriteLine("");

        // ── Task 2(a): by register ──
        _out.WriteLine("  BY REGISTER (streaming — the shipping path):");
        _out.WriteLine("    band            notes    per-note   FA(+1)   FA(+6)");
        for (int b = 0; b < 3; b++)
            _out.WriteLine($"    {BandNames[b]}  {str[b].Notes,6}   {Pct(str[b].Hits, str[b].Notes),8:P1}  "
                         + $"{Pct(str[b].Decoy[i1], str[b].DecoyN[i1]),7:P1}  {Pct(str[b].Decoy[i6], str[b].DecoyN[i6]),7:P1}");
        _out.WriteLine("  BY REGISTER (offline):");
        _out.WriteLine("    band            notes    per-note   FA(+1)   FA(+6)");
        for (int b = 0; b < 3; b++)
            _out.WriteLine($"    {BandNames[b]}  {off[b].Notes,6}   {Pct(off[b].Hits, off[b].Notes),8:P1}  "
                         + $"{Pct(off[b].Decoy[i1], off[b].DecoyN[i1]),7:P1}  {Pct(off[b].Decoy[i6], off[b].DecoyN[i6]),7:P1}");
        _out.WriteLine("");

        // ── Task 2(b): FA vs decoy interval ──
        _out.WriteLine("  FA vs DECOY INTERVAL (semitones). CLEAN = the decoy pitch is annotated SILENT in");
        _out.WriteLine("  the window; RAW counts every confirm (so on chord material it also counts notes the");
        _out.WriteLine("  player is genuinely holding — a power chord IS root+fifth+octave).");
        _out.WriteLine("    interval   clean-off   clean-str   |  raw-off   raw-str   | silent decoys");
        for (int i = 0; i < DecoyIntervals.Length; i++)
            _out.WriteLine($"    {DecoyIntervals[i],+5}    {Pct(offAll.Decoy[i], offAll.DecoyN[i]),9:P1}   {Pct(strAll.Decoy[i], strAll.DecoyN[i]),9:P1}   |"
                         + $"  {Pct(offAll.DecoyRaw[i], offAll.Notes),7:P1}  {Pct(strAll.DecoyRaw[i], strAll.Notes),7:P1}   | {strAll.DecoyN[i],6}");

        // Guard only: this harness's job is to REPORT. It fails on collapse, not on
        // the offline/streaming gap itself (measuring that gap is the whole point).
        Assert.True(Pct(strAll.Hits, strAll.Notes) >= 0.50,
            $"streaming per-note {Pct(strAll.Hits, strAll.Notes):P1} collapsed below 50%");
    }
}
