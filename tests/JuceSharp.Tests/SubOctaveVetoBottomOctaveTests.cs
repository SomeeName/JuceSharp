using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The sub-octave veto has to work in the register it was widened for.
///
/// <para><see cref="JuceChartVerifier.SubOctaveVeto"/> is what stops a player scoring a hit by
/// playing the charted note an OCTAVE DOWN — measured on IDMT, the octave-below decoy confirms
/// ~89.7% of the time without it, because the played note's 2nd harmonic lands exactly on the
/// charted bin, loud, and is a genuine local peak. It works by reading <c>bin − 12·Step</c>…
/// which is NEGATIVE for every pitch in the bottom octave of the charted range
/// (<c>fMinMidi .. fMinMidi + 11</c>), and a negative bin used to read as a hard 0. So the veto
/// was silently inoperative across exactly the register <c>LiveMinMidi</c> was lowered to cover
/// (the bass low E string when that was E1; since 0.57.4 the range starts at A0).
/// The same hole made <c>IsPeak</c> half-vacuous at bins 0 and 1: the missing lower neighbour
/// read as 0, so <c>c &gt;= 0</c> was unconditionally true.</para>
///
/// <para>All synthetic; no dataset.</para>
/// </summary>
public sealed class SubOctaveVetoBottomOctaveTests
{
    private const int Sr = 44100;

    private static JuceConstantQ NewCqt(int subOctaveBins) => new(
        Sr, hop: LiveCqtConfig.Hop, fMinMidi: LiveCqtConfig.MinMidi, binCount: LiveCqtConfig.BinCount,
        maxKernelSamples: LiveCqtConfig.MaxKernelSamples, subOctaveBins: subOctaveBins);

    private static JuceChartVerifier Verifier(JuceConstantQ cqt) => new(cqt)
    {
        // NoiseMargin off: this fixture is one sustained note in silence, so the whole-clip
        // 5th-percentile noise floor is meaningless here. The veto is what is under test.
        NoiseMargin = 0f,
        SubOctaveVeto = LiveCqtConfig.SubOctaveVeto,
    };

    /// <summary>A plucked-string-ish tone: fundamental plus a strong 2nd harmonic, which is
    /// exactly what makes the octave ABOVE confirm falsely.</summary>
    private static float[] Pluck(int midi, double seconds)
    {
        var f0 = 440.0 * Math.Pow(2.0, (midi - 69) / 12.0);
        var n = (int)(seconds * Sr);
        var y = new float[n];
        double[] w = { 1.0, 0.7, 0.4, 0.25, 0.15 };
        for (var i = 0; i < n; i++)
        {
            var t = i / (double)Sr;
            double s = 0;
            for (var k = 0; k < w.Length; k++) s += w[k] * Math.Sin(2 * Math.PI * f0 * (k + 1) * t);
            y[i] = (float)(0.3 * s / 2.5 * Math.Min(1.0, t / 0.004) * Math.Exp(-t * 1.5));
        }
        return y;
    }

    /// <summary>
    /// Play MIDI 23 (B0, a 5-string bass's low B). Its 2nd harmonic sits exactly on MIDI 35
    /// (B1), so a charted B1 confirms from an octave-down performance unless the veto can read
    /// the bin an octave below B1.
    /// <para>The BLIND control is the pre-0.57.4 layout — range starting at E1 (28), no
    /// sub-octave bins — where that bin is −10 and the decoy confirms. The shipping layout must
    /// reject it. (Since the range moved down to A0 the B0 bin is inside the charted range; the
    /// sub-octave bins now cover MIDI 8..20, where nothing a player can sound resolves at the
    /// shipping kernel cap — that half is pinned by <see cref="LowestBins_HaveARealLowerNeighbour"/>.)</para>
    /// </summary>
    [Fact]
    public void OctaveDownPerformance_IsVetoed_InTheBottomOctave()
    {
        var y = Pluck(23, 2.0);

        var blind = new JuceConstantQ(Sr, hop: LiveCqtConfig.Hop, fMinMidi: 28, binCount: 144,
            maxKernelSamples: LiveCqtConfig.MaxKernelSamples, subOctaveBins: 0);
        blind.AnalyzeWhole(y);
        Assert.True(blind.BinForMidi(35) - 12 * blind.Step < blind.LowestBin);
        Assert.True(Verifier(blind).Verify(35, 0.5, allowSubOctaveVeto: true).Hit,
            "fixture is wrong: the octave-down decoy must confirm when the veto cannot fire");

        // The shipping layout: rejected.
        var seeing = NewCqt(LiveCqtConfig.SubOctaveBins);
        seeing.AnalyzeWhole(y);
        Assert.True(seeing.BinForMidi(35) - 12 * seeing.Step >= seeing.LowestBin);
        Assert.False(Verifier(seeing).Verify(35, 0.5, allowSubOctaveVeto: true).Hit,
            "the sub-octave veto cannot fire against a 5-string's low B");

        // …and it is the VETO doing it, not a general loss of sensitivity: a genuinely played
        // B1 (no octave below it) must still confirm on the same configuration.
        var honest = NewCqt(LiveCqtConfig.SubOctaveBins);
        honest.AnalyzeWhole(Pluck(35, 2.0));
        Assert.True(Verifier(honest).Verify(35, 0.5, allowSubOctaveVeto: true).Hit,
            "the veto rejected a genuinely played note");
    }

    /// <summary>Chords carry real octaves, so the veto must stay opt-out — the extension
    /// must not change that.</summary>
    [Fact]
    public void ChordTones_OptOut_EvenWithSubOctaveBins()
    {
        var cqt = NewCqt(LiveCqtConfig.SubOctaveBins);
        cqt.AnalyzeWhole(Pluck(23, 2.0));
        Assert.True(Verifier(cqt).Verify(35, 0.5, allowSubOctaveVeto: false).Hit);
    }

    /// <summary>
    /// The peak test at the very bottom of the range. Bin 0's lower neighbour is bin −2; with
    /// no sub-octave bins that read as 0 and <c>c &gt;= 0</c> made every candidate a "peak"
    /// there, so the lowest semitone got a free pass on the one test that separates a note
    /// from its neighbour.
    /// </summary>
    [Fact]
    public void LowestBins_HaveARealLowerNeighbour()
    {
        var blind = NewCqt(subOctaveBins: 0);
        Assert.Equal(0, blind.LowestBin);
        Assert.Equal(0f, blind.Magnitude(-2, 0));                 // no neighbour: reads as 0
        Assert.Equal(float.MaxValue, blind.Floor(-2));

        var seeing = NewCqt(LiveCqtConfig.SubOctaveBins);
        Assert.Equal(-LiveCqtConfig.SubOctaveBins, seeing.LowestBin);
        seeing.AnalyzeWhole(Pluck(LiveCqtConfig.MinMidi, 2.0));   // bin 0

        var f = seeing.FrameOfSeconds(0.5);
        Assert.True(seeing.Magnitude(0, f) > 0f);
        Assert.True(seeing.Floor(-2) < float.MaxValue, "bin -2 must be a real bin now");
        // A semitone below the range is not being played, so bin 0 is a genuine peak against a
        // REAL neighbour rather than against a hard zero.
        Assert.True(seeing.Magnitude(0, f) > seeing.Magnitude(-2, f));
    }

    /// <summary>
    /// <c>subOctaveBins: 0</c> has to be a strict no-op, or this "extension" is a silent
    /// behaviour change for every caller that did not ask for it.
    /// </summary>
    [Fact]
    public void SubOctaveBinsZero_IsAStrictNoOp()
    {
        var y = Pluck(40, 1.5);

        var a = new JuceConstantQ(Sr, fMinMidi: 40, binCount: 60, maxKernelSamples: 4096);
        var b = new JuceConstantQ(Sr, fMinMidi: 40, binCount: 60, maxKernelSamples: 4096,
                                  subOctaveBins: 0);
        a.AnalyzeWhole(y);
        b.AnalyzeWhole(y);

        for (var f = 0L; f <= a.NewestFrame; f++)
        {
            Assert.Equal(a.FrameEnergy(f), b.FrameEnergy(f));
            for (var bin = 0; bin < a.BinCount; bin++)
                Assert.Equal(a.Magnitude(bin, f), b.Magnitude(bin, f));
        }
        Assert.Equal(a.NoiseFloorEnergy(), b.NoiseFloorEnergy());
    }

    /// <summary>
    /// The sub-octave bins must not leak into the broadband statistics: they are veto-only.
    /// If they did, every measured presence-gate number (noise margin 3, the idle
    /// phantom-confirm rate) would silently shift.
    /// </summary>
    [Fact]
    public void SubOctaveBins_DoNotMoveTheBroadbandEnergyOrTheNoiseFloor()
    {
        var y = Pluck(28, 2.0);

        var without = NewCqt(subOctaveBins: 0);
        var with = NewCqt(LiveCqtConfig.SubOctaveBins);
        without.AnalyzeWhole(y);
        with.AnalyzeWhole(y);

        Assert.Equal(without.NoiseFloorEnergy(), with.NoiseFloorEnergy());
        for (var f = 0L; f <= without.NewestFrame; f += 7)
            Assert.Equal(without.FrameEnergy(f), with.FrameEnergy(f));

        // Charted bins read identically too — only the negative ones are new.
        var frame = without.FrameOfSeconds(0.5);
        for (var bin = 0; bin < without.BinCount; bin++)
            Assert.Equal(without.Magnitude(bin, frame), with.Magnitude(bin, frame));
    }

    /// <summary>The streaming path has to agree with the offline one with the extension on,
    /// including at negative bins — the ring, the floor memo and the energy sum are all
    /// separately indexed.</summary>
    [Fact]
    public void Streaming_MatchesOffline_WithSubOctaveBins()
    {
        var y = Pluck(28, 2.5);

        var offline = NewCqt(LiveCqtConfig.SubOctaveBins);
        offline.AnalyzeWhole(y);

        var live = NewCqt(LiveCqtConfig.SubOctaveBins);
        live.Reset(0);
        for (var off = 0; off < y.Length; off += 512)
            live.Push(y.AsSpan(off, Math.Min(512, y.Length - off)), off);

        var compared = 0;
        for (var f = live.OldestFrame; f <= live.NewestFrame; f++)
        {
            Assert.Equal(offline.FrameEnergy(f), live.FrameEnergy(f));
            for (var bin = live.LowestBin; bin < live.BinCount; bin++)
            {
                Assert.Equal(offline.Magnitude(bin, f), live.Magnitude(bin, f));
                compared++;
            }
        }
        Assert.True(compared > 10_000, $"compared too few (bin, frame) pairs: {compared}");
    }
}
