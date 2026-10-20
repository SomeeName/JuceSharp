using JuceSharp.Dsp;
using Xunit;
using Ref = JuceSharp.Tests.IdmtDataset.Ref;

namespace JuceSharp.Tests;

/// <summary>
/// Port-fidelity check: runs the C# CQT chart verifier over the SAME
/// IDMT-SMT-GUITAR_V2 recordings + annotations the Python testbed used, and
/// reports per-note / per-event accuracy and wrong-fret false-accepts so the
/// numbers can be compared directly to the Python results (~97% per-note,
/// ~98% per-event, ~11% FA on the mono mix).
///
/// Opt-in: skipped unless the dataset is present (env <c>GND_IDMT_ROOT</c> or
/// the default NoteDetection checkout). Runs a deterministic capped sample
/// (env <c>GND_EVAL_CAP</c>, default 60) so it stays a reasonable length.
///
/// <para><b>THIS DOES NOT MEASURE WHAT SHIPS — use <see cref="CqtStreamingEvalTests"/>.</b>
/// It drives the OFFLINE <c>AnalyzeWhole</c> path (whole-clip percentile statistics), which
/// live play never runs, and it deliberately does NOT mirror the shipping verifier
/// configuration: no <c>SubOctaveVeto</c>, no noise-history horizon. That is fine for its
/// stated job — reproducing the Python port's numbers on the same statistics — and wrong for
/// anything else. The shipping configuration lives in <see cref="LiveCqtConfig"/>.</para>
/// </summary>
public sealed class CqtDatasetEvalTests
{
    private readonly ITestOutputHelper _out;
    public CqtDatasetEvalTests(ITestOutputHelper output) => _out = output;

    [Fact]
    public void CqtVerifier_ReproducesPythonAccuracy_OnIdmtDataset()
    {
        var root = IdmtDataset.ResolveRoot();
        if (root is null)
            Assert.Skip("IDMT-SMT-GUITAR_V2 not found (set GND_IDMT_ROOT to run this eval).");

        var pairs = IdmtDataset.PairTracks(root!);
        Assert.NotEmpty(pairs);

        var cap = int.TryParse(Environment.GetEnvironmentVariable("GND_EVAL_CAP"), out var c) ? c : 60;
        // Default to the live streaming kernel cap (4096 ≈ 93 ms) so this gate
        // validates the config that actually ships; env-override to sweep.
        var maxKernel = int.TryParse(Environment.GetEnvironmentVariable("GND_CQT_MAXKERNEL"), out var mk) ? mk : 4096;
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        // Default to the live config (gate on) so the gate measures reality, not
        // the phantom-prone ungated path; env-override to sweep.
        var noiseMargin = float.TryParse(Environment.GetEnvironmentVariable("GND_NOISE_MARGIN"), System.Globalization.NumberStyles.Float, inv, out var nm) ? nm : 3f;
        var harmonicComb = int.TryParse(Environment.GetEnvironmentVariable("GND_HARMONIC_COMB"), out var hc) ? hc : 0;
        var sample = IdmtDataset.Stride(pairs, cap);

        int notes = 0, noteHits = 0, events = 0, eventHits = 0, faNotes = 0, faHits = 0, faTritone = 0, tracks = 0;
        int candHits = 0;   // the current blind-transcription path, same notes
        int kernelSamples = 0;
        foreach (var (wav, xml) in sample)
        {
            var y = IdmtDataset.ReadWavMono(wav, out int sr);
            if (y.Length == 0) continue;
            var refs = IdmtDataset.Parse(xml);
            if (refs.Count == 0) continue;

            var cqt = new JuceConstantQ(sr, maxKernelSamples: maxKernel);
            kernelSamples = cqt.MaxKernelSamples;
            cqt.AnalyzeWhole(y);
            var v = new JuceChartVerifier(cqt) { NoiseMargin = noiseMargin, HarmonicComb = harmonicComb };
            var candidates = RunCandidatePath(y, sr);   // current JuceGuitarAnalyzer events
            tracks++;

            foreach (var n in refs)
            {
                notes++;
                if (v.Verify(n.Midi, n.Onset).Hit) noteHits++;
                if (CandidateDetects(candidates, n)) candHits++;
                faNotes++;
                if (v.Verify(n.Midi + 1, n.Onset).Hit) faHits++;      // wrong-fret decoy (+1)
                if (v.Verify(n.Midi + 6, n.Onset).Hit) faTritone++;   // tritone decoy (+6)
            }
            foreach (var grp in IdmtDataset.ClusterEvents(refs, 0.030))
            {
                events++;
                var present = grp.Count(n => v.Verify(n.Midi, n.Onset).Hit);
                if (present >= (int)Math.Ceiling(grp.Count / 2.0)) eventHits++;   // 'half' threshold
            }
        }

        double perNote = noteHits / (double)Math.Max(1, notes);
        double perEvent = eventHits / (double)Math.Max(1, events);
        double fa = faHits / (double)Math.Max(1, faNotes);
        _out.WriteLine($"IDMT C# CQT eval — tracks={tracks}, notes={notes}, events={events}, maxKernel={kernelSamples} (~{kernelSamples / 44.1:0} ms)");
        double candNote = candHits / (double)Math.Max(1, notes);
        _out.WriteLine($"  per-note      = {perNote:P1}   (Python ~97%)");
        _out.WriteLine($"  per-event     = {perEvent:P1}   (Python ~98%)");
        _out.WriteLine($"  wrong-fret FA = {fa:P1}   (Python ~11%)");
        _out.WriteLine($"  tritone FA    = {faTritone / (double)Math.Max(1, notes):P1}   (harmonicComb={harmonicComb})");
        _out.WriteLine($"  vs current path per-note (blind transcription) = {candNote:P1}");

        Assert.True(tracks >= 5, $"too few tracks evaluated: {tracks}");
        Assert.True(perEvent >= 0.90, $"per-event {perEvent:P1} regressed below 90% (Python ~98%)");
        Assert.True(fa <= 0.30, $"wrong-fret false-accept {fa:P1} exceeded 30%");
    }

    // Drive the current streaming analyzer over the clip and collect its
    // detection events (blind polyphonic transcription + YIN) — the same
    // evidence the classic candidate matcher judges against.
    private static List<(double T, JucePitchCandidate[] P)> RunCandidatePath(float[] y, int sr)
    {
        var analyzer = new JuceGuitarAnalyzer(sr);
        var outp = new List<(double, JucePitchCandidate[])>();
        const int block = 2048;
        for (int off = 0; off < y.Length; off += block)
        {
            int len = Math.Min(block, y.Length - off);
            foreach (var e in analyzer.Push(y.AsSpan(off, len))) outp.Add((e.StreamSeconds, e.Pitches));
        }
        foreach (var e in analyzer.Push(new float[8192])) outp.Add((e.StreamSeconds, e.Pitches)); // flush pending onsets
        return outp;
    }

    // Octave-tolerant pitch match within a hit window at the matcher's note floor
    // (MinNoteSalience 0.25): the current path "detects" the note if a salient
    // candidate near the onset carries the right pitch.
    private static bool CandidateDetects(List<(double T, JucePitchCandidate[] P)> events, Ref n)
    {
        foreach (var (t, ps) in events)
        {
            if (Math.Abs(t - n.Onset) > 0.15) continue;
            foreach (var p in ps)
            {
                if (p.Salience < 0.25f) continue;
                var d = Math.Abs(p.Midi - n.Midi);
                if (d < 0.5 || Math.Abs(d - 12) < 0.5) return true;
            }
        }
        return false;
    }

}
