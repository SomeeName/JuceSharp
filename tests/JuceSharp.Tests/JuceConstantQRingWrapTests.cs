using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Equivalence pins for the raw-ring address wrap in <see cref="JuceConstantQ"/>.
///
/// <para>The kernel tap loop used to compute <c>_raw[Mod(abs, _rawSize)]</c> per tap — a
/// 64-bit hardware division per tap, ~4.3·10⁵ per frame at the shipping config because
/// <c>_rawSize</c> (12,548) is a runtime field and not a power of two. The wrap is now hoisted
/// to one division per (bin, frame) with a compare-and-reset per tap. That is a PERFORMANCE
/// change with no permitted behavioural component, so these tests prove equality rather than
/// improvement:</para>
/// <list type="number">
///   <item><description>the two index sequences are identical, tap for tap, including
///   negative stream positions and multi-lap spans;</description></item>
///   <item><description>the streaming transform reproduces the offline one EXACTLY (not to a
///   tolerance) at the shipping config, over every resident bin and frame, under ragged push
///   sizes — any addressing error anywhere shows up here.</description></item>
/// </list>
/// <para>No dataset: all synthetic.</para>
/// </summary>
public sealed class JuceConstantQRingWrapTests
{
    private const int Sr = 44100;

    /// <summary>The OLD wrap, verbatim (<c>JuceConstantQ.Mod</c>): the reference this
    /// equivalence is measured against.</summary>
    private static int Mod(long x, int n) { int r = (int)(x % n); return r < 0 ? r + n : r; }

    [Theory]
    // (start, taps, rawSize). 12,548 is the shipping _rawSize: 2·2048 + 8192 + 256 + 4.
    [InlineData(0L, 4097, 12548)]
    [InlineData(12547L, 4097, 12548)]          // starts on the last slot — wraps immediately
    [InlineData(-1L, 4097, 12548)]             // negative dividend: C# % keeps the sign
    [InlineData(-20224L, 4097, 12548)]         // a realistic count-in origin
    [InlineData(-12548L, 4097, 12548)]         // exactly one negative lap
    [InlineData(1_000_000_007L, 4097, 12548)]  // long past int range
    [InlineData(5L, 40000, 12548)]             // three full laps in one kernel
    [InlineData(0L, 9, 4)]                     // degenerate tiny ring
    public void IncrementingIndex_MatchesModuloPerTap(long start, int taps, int rawSize)
    {
        var idx = Mod(start, rawSize);
        for (var n = 0; n < taps; n++)
        {
            Assert.Equal(Mod(start + n, rawSize), idx);   // per-tap modulo == hoisted index
            Assert.InRange(idx, 0, rawSize - 1);
            if (++idx == rawSize) idx = 0;
        }
    }

    [Fact]
    public void Streaming_MatchesOfflineExactly_AtTheShippingConfig()
    {
        // A realistic buffer: a bass low E, a mid note and a chord, so bins across the whole
        // range carry energy (a wrap bug that only hits long kernels must not hide).
        var y = RealisticBuffer(3.0);

        var offline = NewLiveCqt();
        offline.AnalyzeWhole(y);

        var live = NewLiveCqt();
        live.Reset(0);
        PushRagged(live, y, 0);

        // The whole resident ring: 384 frames x 144 bins.
        Assert.Equal(384 * live.BinCount, AssertRingMatches(live, offline, frameOffset: 0));
    }

    [Fact]
    public void Streaming_MatchesOfflineExactly_FromANegativeStreamPosition()
    {
        // The count-in axis goes negative, which is the case a naive hoist gets wrong (C#'s
        // `%` keeps the dividend's sign). -20224 = -79·256 keeps frame centres aligned to the
        // offline grid, so frame f here is offline frame f + 79 and the two are comparable.
        const long origin = -79L * 256;
        var y = RealisticBuffer(3.0);

        var offline = NewLiveCqt();
        offline.AnalyzeWhole(y);

        var live = NewLiveCqt();
        live.Reset(origin);
        PushRagged(live, y, origin);

        Assert.Equal(384 * live.BinCount, AssertRingMatches(live, offline, frameOffset: 79));
    }

    // Exactly LiveCqtConfig / LiveCqtVerifier.CreateStreaming: _rawSize = 12,548, not a
    // power of two, which is the whole point.
    private static JuceConstantQ NewLiveCqt() => new(
        Sr, hop: LiveCqtConfig.Hop, fMinMidi: LiveCqtConfig.MinMidi, binCount: LiveCqtConfig.BinCount,
        maxKernelSamples: LiveCqtConfig.MaxKernelSamples,
        noiseHistoryFrames: LiveCqtConfig.NoiseHistoryFrames(Sr));

    // Ragged blocks, including 1 and 7 samples and blocks larger than the emit chunk, so the
    // sub-chunking and the wrap are exercised at every phase.
    private static void PushRagged(JuceConstantQ cqt, float[] y, long origin)
    {
        int[] sizes = { 1, 7, 512, 4096, 8193, 333, 1 };
        var off = 0;
        var s = 0;
        while (off < y.Length)
        {
            var len = Math.Min(sizes[s++ % sizes.Length], y.Length - off);
            cqt.Push(y.AsSpan(off, len), origin + off);
            off += len;
        }
    }

    private static int AssertRingMatches(JuceConstantQ live, JuceConstantQ offline, long frameOffset)
    {
        var compared = 0;
        for (var f = live.OldestFrame; f <= live.NewestFrame; f++)
        {
            for (var b = 0; b < live.BinCount; b++)
            {
                // EXACT: same samples, same kernel, same summation order ⇒ same bits.
                Assert.Equal(offline.Magnitude(b, f + frameOffset), live.Magnitude(b, f));
                compared++;
            }
        }
        return compared;
    }

    private static float[] RealisticBuffer(double seconds)
    {
        var (notes, _) = Synth.RenderNotes(new[] { 28, 40, 52 }, Sr, noteDur: 0.6, gap: 0.15);
        var (chords, _) = Synth.RenderChords(new IReadOnlyList<int>[] { new[] { 40, 47, 52 } }, Sr);
        var y = new float[(int)(seconds * Sr)];
        for (var i = 0; i < y.Length; i++)
        {
            var v = i < notes.Length ? notes[i] : 0f;
            var j = i - Sr;                                  // overlap the chord from 1 s in
            if (j >= 0 && j < chords.Length) v += chords[j];
            y[i] = v * 0.5f;
        }
        return y;
    }
}
