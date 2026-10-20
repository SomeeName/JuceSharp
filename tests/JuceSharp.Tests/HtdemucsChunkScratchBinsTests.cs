using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     <see cref="HtdemucsChunkScratch" />'s <c>bins</c> constructor argument was
///     DEAD: every plane sizes itself <c>nFft / 2 + 1</c> from the transform (see
///     <see cref="SpectrumPlanes" />), so a caller whose <c>bins</c> disagreed
///     with its <c>nFft</c> got the transform's geometry and no complaint — the
///     call site and the object said different things about the same shape.
///     <para>
///         0.9 drops it. The 7-argument constructor is kept as a forwarder (and
///         deliberately NOT <c>[Obsolete]</c>: DotCharter still calls it and builds
///         with warnings as errors), and the geometry is now readable as
///         <see cref="HtdemucsChunkScratch.Bins" /> instead of passed in.
///     </para>
/// </summary>
public sealed class HtdemucsChunkScratchBinsTests
{
    private const int Stems = 6;
    private const int Segment = 1024;
    private const int TimeFrames = 8;
    private const int FreqBins = 16;
    private const int NFft = 64;
    private const int Hop = 16;

    [Fact]
    public void Bins_IsTheTransformsGeometry_NeverAnArgument()
    {
        var s = new HtdemucsChunkScratch(Stems, Segment, TimeFrames, FreqBins, NFft, Hop);

        Assert.Equal(NFft / 2 + 1, s.Bins);
        // …and the planes really have that many bins, so Bins is a report, not a
        // second copy of the shape.
        Assert.Equal(s.Bins, s.SpecL[0].BinCount);
        Assert.Equal(s.Bins, s.SpecL[0].Re[0].Length);
        Assert.Equal(s.Bins, s.InL.BinCount);
    }

    /// <summary>
    /// THE COMPATIBILITY CLAIM: the legacy 7-argument call still compiles and
    /// still builds exactly the same object — including when the <c>bins</c> it
    /// passes is a value the transform would never produce, which is precisely
    /// what the argument's deadness means.
    /// </summary>
    [Fact]
    public void TheLegacySevenArgumentConstructor_IgnoresBins_AndBuildsTheSameObject()
    {
        var modern = new HtdemucsChunkScratch(Stems, Segment, TimeFrames, FreqBins, NFft, Hop);
        var legacy = new HtdemucsChunkScratch(Stems, Segment, TimeFrames, FreqBins, NFft / 2 + 1, NFft, Hop);
        var nonsense = new HtdemucsChunkScratch(Stems, Segment, TimeFrames, FreqBins, 9999, NFft, Hop);

        Assert.Equal(modern.Bins, legacy.Bins);
        Assert.Equal(modern.Bins, nonsense.Bins);
        Assert.Equal(modern.EstimatedBytes, legacy.EstimatedBytes);
        Assert.Equal(modern.EstimatedBytes, nonsense.EstimatedBytes);
        Assert.Equal(modern.SpecL[0].Re[0].Length, nonsense.SpecL[0].Re[0].Length);
    }
}
