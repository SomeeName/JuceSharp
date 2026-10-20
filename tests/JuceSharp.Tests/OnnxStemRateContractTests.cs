using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// W4-S11: every driver's stems come back at the input rate AND at the mix's
/// exact frame count. The htdemucs branch had its own copy of the post-resample
/// that had lost the <c>FitToFrames</c> correction its sibling applies, so a
/// stem could be one frame long or short against the mix — while
/// <c>SeparateAsync</c>'s contract is "the caller can write them directly
/// alongside the source audio".
/// <para>Driven through <c>ResampleStemsBack</c>, the one implementation both
/// paths now share, so no model file is involved.</para>
/// </summary>
public sealed class OnnxStemRateContractTests
{
    private static float[] Stereo(int frames)
    {
        var s = new float[frames * 2];
        for (var i = 0; i < frames; i++)
        {
            s[2 * i] = MathF.Sin(i * 0.01f);
            s[2 * i + 1] = MathF.Cos(i * 0.01f);
        }
        return s;
    }

    /// <summary>
    /// Awkward lengths at each rate the shipping models are used at. 44 100 is
    /// the htdemucs model rate — the pass-through branch, which also has to fit,
    /// because a driver's own internal padding can return a longer buffer.
    /// </summary>
    [Theory]
    [InlineData(44_100, 44_100, 100_001)]
    [InlineData(44_100, 48_000, 100_001)]
    [InlineData(44_100, 96_000, 33_333)]
    [InlineData(44_100, 22_050, 77_777)]
    public void Every_stem_comes_back_at_the_mixs_exact_frame_count(int modelRate, int sampleRate, int mixFrames)
    {
        // What a driver produces: its own rate, and a length that went through
        // its own rounding rather than the mix's.
        var modelFrames = (int)Math.Round(mixFrames * (double)modelRate / sampleRate) + 1;
        var stems = new (string Name, float[] Stereo)[]
        {
            ("vocals", Stereo(modelFrames)),
            ("other", Stereo(modelFrames)),
        };

        var results = OnnxStemSeparator.ResampleStemsBack(
            stems, modelRate, sampleRate, mixFrames, "test", log: null);

        Assert.Equal(2, results.Count);
        foreach (var r in results)
        {
            Assert.Equal(sampleRate, r.SampleRate);
            Assert.Equal(mixFrames * 2, r.InterleavedStereo.Length);
        }
    }

    /// <summary>
    /// A stem SHORTER than the mix is padded rather than left short — the other
    /// half of the same round-trip rounding.
    /// </summary>
    [Fact]
    public void A_short_stem_is_padded_to_the_mix_length()
    {
        var stems = new (string Name, float[] Stereo)[] { ("vocals", Stereo(1_000)) };
        var results = OnnxStemSeparator.ResampleStemsBack(stems, 44_100, 44_100, 1_010, "test", null);
        Assert.Equal(1_010 * 2, results[0].InterleavedStereo.Length);
    }
}
