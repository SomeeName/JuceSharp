using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The per-chunk scratch, at the SHIPPING htdemucs shape. Two claims are pinned
/// here because both were made in prose and neither was measurable before:
/// the spectrogram storage halves when it stops being
/// <see cref="System.Numerics.Complex"/>, and the whole live set is smaller than
/// it was even after the transform workspaces moved into it.
/// </summary>
public sealed class HtdemucsScratchFootprintTests
{
    // htdemucs_ft: 6 stems, 7.8 s segment at 44.1 kHz, nFft 4096 / hop 1024.
    private const int Stems = 6;
    private const int Segment = 343_980;
    private const int TimeFrames = 336;
    private const int FreqBins = 2048;
    private const int Bins = FreqBins + 1;
    private const int NFft = 4096;
    private const int Hop = 1024;

    /// <summary>
    /// <c>Complex</c> is two doubles: 16 bytes a bin against 8 for a float
    /// re/im pair, for data that comes out of a float32 model and goes back into
    /// one. Six stems x two channels x 336 frames x 2049 bins is 132 MB of that.
    /// </summary>
    [Fact]
    public void SpectrogramStorageIsExactlyHalfWhatComplexCost()
    {
        var oldSpectraBytes = 2L * Stems * TimeFrames * Bins * 16;   // Complex
        var newSpectraBytes = 2L * Stems * TimeFrames * Bins * 8;    // float re + float im

        Assert.Equal(oldSpectraBytes / 2, newSpectraBytes);
        Assert.True(oldSpectraBytes > 125L * 1024 * 1024,
            $"the shape must still be the expensive one ({oldSpectraBytes} bytes)");

        // And the real object agrees with the arithmetic.
        var scratch = new HtdemucsChunkScratch(Stems, Segment, TimeFrames, FreqBins, Bins, NFft, Hop);
        long measured = 0;
        foreach (var p in scratch.SpecL) measured += 2L * p.FrameCount * p.BinCount * sizeof(float);
        foreach (var p in scratch.SpecR) measured += 2L * p.FrameCount * p.BinCount * sizeof(float);
        Assert.Equal(newSpectraBytes, measured);
    }

    /// <summary>
    /// The workspaces are NEW resident memory — the old code allocated their
    /// equivalents transiently, per chunk, per iSTFT. The whole live set must
    /// still come out smaller than the old one, or the churn fix would have been
    /// paid for in footprint.
    /// </summary>
    [Fact]
    public void TheWholeLiveSetIsSmallerThanTheComplexOne()
    {
        var scratch = new HtdemucsChunkScratch(Stems, Segment, TimeFrames, FreqBins, Bins, NFft, Hop);

        long oldBytes =
            4L * (2 * Segment)                          // Left + Right
            + 4L * Stems * Segment * 2                  // Interleaved
            + 2L * Stems * TimeFrames * Bins * 16       // SpecL + SpecR as Complex
            + 4L * Stems * 4 * FreqBins * TimeFrames    // SpecOut
            + 4L * Stems * 2 * Segment;                 // WaveOut

        Assert.True(scratch.EstimatedBytes < oldBytes,
            $"live set is {scratch.EstimatedBytes / (1024 * 1024)} MB against the old " +
            $"{oldBytes / (1024 * 1024)} MB");
    }

    /// <summary>
    /// Every stem owns its own workspace and its own planes, because the driver
    /// fills them from a <c>Parallel.For</c> over stems — sharing one would be a
    /// data race producing silently wrong stems rather than a crash.
    /// </summary>
    [Fact]
    public void PerStemSlotsAndWorkspacesAreIndependent()
    {
        var scratch = new HtdemucsChunkScratch(Stems, 4096, 8, 16, 17, 64, 16);
        Assert.True(scratch.Work.Length >= Stems);
        for (var a = 0; a < Stems; a++)
        for (var b = a + 1; b < Stems; b++)
        {
            Assert.NotSame(scratch.SpecL[a], scratch.SpecL[b]);
            Assert.NotSame(scratch.SpecR[a], scratch.SpecR[b]);
            Assert.NotSame(scratch.Work[a], scratch.Work[b]);
        }
    }

    /// <summary>
    /// The reused spectrogram rows carry one piece of state ACROSS chunks: the
    /// driver writes bins 0..FreqBins-1 and leaves the Nyquist bin zero, which is
    /// what the model contract requires. Allocation gives us that zero and
    /// nothing may ever write it.
    /// </summary>
    [Fact]
    public void TheNyquistBinStartsAtZeroSoReuseIsSafe()
    {
        var scratch = new HtdemucsChunkScratch(Stems, 4096, 8, 16, 17, 64, 16);
        foreach (var planes in scratch.SpecL)
        for (var f = 0; f < planes.FrameCount; f++)
        {
            Assert.Equal(0f, planes.Re[f][16]);
            Assert.Equal(0f, planes.Im[f][16]);
        }
    }
}
