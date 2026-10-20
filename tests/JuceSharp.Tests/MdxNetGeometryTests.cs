using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// W4-S1 / W4-S31: every catalogue entry's SHIPPING transform geometry has to be
/// one the transform stack accepts and can round-trip.
/// <para>Nothing constructed <c>MdxNetInference</c> — the one test that named it
/// (<c>OnnxDriverAuditTests</c>) is a source lint keyed on a file name — so
/// <c>NFft = 6144</c>, which is not a power of two, reached five catalogue
/// entries including <c>DefaultIds[MdxNet]</c> and threw on the first chunk of
/// each. A driver needs an <c>InferenceSession</c> and therefore a model file;
/// its GEOMETRY does not, and the geometry is what shipped broken.</para>
/// </summary>
public sealed class MdxNetGeometryTests
{
    private sealed record Geometry(int NFft, int Hop, bool Htdemucs);

    private static Geometry ForKind(OnnxStemModelKind kind) => kind switch
    {
        OnnxStemModelKind.MdxNet => new Geometry(new MdxNetInference.Settings().NFft,
                                                 new MdxNetInference.Settings().Hop, Htdemucs: false),
        OnnxStemModelKind.BsRoformer => new Geometry(new BsRoformerInference.Settings().NFft,
                                                     new BsRoformerInference.Settings().Hop, Htdemucs: false),
        OnnxStemModelKind.Htdemucs => new Geometry(HtdemucsOnnxInference.NFft,
                                                   HtdemucsOnnxInference.Hop, Htdemucs: true),
        _ => throw new ArgumentOutOfRangeException(nameof(kind), kind, "Unknown driver kind."),
    };

    public static TheoryData<string> EveryCatalogueId()
    {
        var data = new TheoryData<string>();
        foreach (var e in OnnxModelCatalog.All) data.Add(e.Id);
        return data;
    }

    private static float[] Noise(int n, int seed)
    {
        var rng = new Random(seed);
        var s = new float[n];
        for (var i = 0; i < n; i++) s[i] = (float)(rng.NextDouble() * 2 - 1);
        return s;
    }

    /// <summary>
    /// Construct the transform stack every driver builds on its first chunk —
    /// the workspace (which builds a <c>JuceFft</c>) and the spectrum planes —
    /// and push one chunk through the forward and inverse the driver uses.
    /// </summary>
    [Theory]
    [MemberData(nameof(EveryCatalogueId))]
    public void Every_catalogue_entry_round_trips_one_chunk_through_its_own_geometry(string id)
    {
        var entry = Assert.Single(OnnxModelCatalog.All, e => e.Id == id);
        var g = ForKind(entry.Kind);

        // A whole number of hops, which is what the drivers' chunk sizes are.
        var signal = Noise(20 * g.Hop, seed: id.GetHashCode(StringComparison.Ordinal));
        var restored = RoundTrip(signal, g);

        // The htdemucs convention discards the first two frames, so its window
        // roll-off is genuinely not unity for the first/last nFft samples —
        // StftHtdemucsTests makes the same exclusion. The centred path is exact
        // everywhere; asserting the interior covers both without a per-kind
        // tolerance.
        for (var i = g.NFft; i < signal.Length - g.NFft; i++)
            Assert.True(Math.Abs(restored[i] - signal[i]) < 1e-4f,
                        $"{id}: sample {i} came back {restored[i]}, not {signal[i]}.");
    }

    /// <summary>
    /// The full MDX-Net chunk, at the exact size <c>PredictInstrumental</c> feeds
    /// the model: <c>Hop * (DimT - 1)</c> samples must give <c>DimT</c> frames of
    /// at least <c>DimF</c> bins, and come back.
    /// </summary>
    [Fact]
    public void The_shipping_mdx_net_chunk_gives_the_model_the_shape_it_declares()
    {
        var s = new MdxNetInference.Settings();
        var pad = s.NFft / 2;
        var paddedLen = s.ChunkSize + 2 * pad;
        var frames = (paddedLen - s.NFft) / s.Hop + 1;

        Assert.Equal(s.DimT, frames);

        var work = new StftWorkspace(s.NFft, paddedLen, frames);
        var planes = new SpectrumPlanes(frames, s.NFft, s.Hop);
        Assert.True(planes.BinCount >= s.DimF,
                    $"nFft {s.NFft} yields {planes.BinCount} bins, fewer than the model's DimF {s.DimF}.");

        var signal = Noise(s.ChunkSize, seed: 4242);
        Stft.Forward(signal, planes, work, periodic: true);

        var dest = new float[signal.Length];
        Stft.Inverse(planes, dest, work, periodic: true);
        for (var i = 0; i < signal.Length; i++)
            Assert.True(Math.Abs(dest[i] - signal[i]) < 1e-4f,
                        $"sample {i} came back {dest[i]}, not {signal[i]}.");
    }

    /// <summary>
    /// The five MDX-Net rows are not a dead branch of the table: the default the
    /// factory hands out for that kind is one of them.
    /// </summary>
    [Fact]
    public void The_default_mdx_net_entry_is_a_real_row()
    {
        var def = OnnxModelCatalog.DefaultFor(OnnxStemModelKind.MdxNet);
        Assert.Equal(OnnxStemModelKind.MdxNet, def.Kind);
        Assert.Contains(OnnxModelCatalog.All, e => e.Id == def.Id);
    }

    private static float[] RoundTrip(float[] signal, Geometry g)
    {
        if (g.Htdemucs)
        {
            var frames = (int)Math.Ceiling((double)signal.Length / g.Hop);
            var padLeft = g.Hop / 2 * 3;
            var forwardPadded = padLeft + frames * g.Hop + padLeft;
            var inversePadded = (frames + 4) * g.Hop + g.NFft;
            var work = new StftWorkspace(g.NFft, Math.Max(forwardPadded, inversePadded), frames);
            var planes = new SpectrumPlanes(frames, g.NFft, g.Hop);
            Stft.ForwardHtdemucs(signal, planes, work);
            var dest = new float[signal.Length];
            Stft.InverseHtdemucs(planes, dest, work);
            return dest;
        }

        var pad = g.NFft / 2;
        var padded = signal.Length + 2 * pad;
        var frameCount = Math.Max(1, (padded - g.NFft) / g.Hop + 1);
        var w = new StftWorkspace(g.NFft, padded, frameCount);
        var p = new SpectrumPlanes(frameCount, g.NFft, g.Hop);
        Stft.Forward(signal, p, w, periodic: true);
        var outBuf = new float[signal.Length];
        Stft.Inverse(p, outBuf, w, periodic: true);
        return outBuf;
    }
}
