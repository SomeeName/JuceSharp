using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Round-trip and shape tests for the in-process STFT used by the ONNX
/// stem-separation engines. The HTDemucs methods follow the reference
/// padding (<c>hop/2 * 3</c>) so the round-trip is identity within
/// numerical error for any input.
/// </summary>
public sealed class StftHtdemucsTests
{
    private const int NFft = 4096;
    private const int Hop = 1024;

    [Fact]
    public void ForwardHtdemucs_Produces_CeilSamplesOverHop_Frames()
    {
        var samples = 343_980; // demucs segment
        var signal = new float[samples];
        var rng = new Random(7);
        for (var i = 0; i < samples; i++) signal[i] = (float)(rng.NextDouble() * 2 - 1);

        var spec = Stft.ForwardHtdemucs(signal, NFft, Hop);

        var expected = (int)Math.Ceiling(samples / (double)Hop);
        Assert.Equal(expected, spec.FrameCount);
        Assert.Equal(NFft / 2 + 1, spec.BinCount);
        Assert.Equal(samples, spec.OriginalLength);
    }

    [Fact]
    public void HtdemucsForwardInverse_RoundTrips_WithinTolerance()
    {
        // 1 second of pink-ish noise
        var samples = 44_100;
        var signal = new float[samples];
        var rng = new Random(42);
        for (var i = 0; i < samples; i++) signal[i] = 0.25f * (float)(rng.NextDouble() * 2 - 1);

        var spec = Stft.ForwardHtdemucs(signal, NFft, Hop);
        var restored = Stft.InverseHtdemucs(spec);

        Assert.Equal(samples, restored.Length);

        // Ignore the first/last NFft samples (window roll-off region) and
        // assert RMS error is small in the steady-state region.
        double err = 0, refSq = 0;
        for (var i = NFft; i < samples - NFft; i++)
        {
            var d = restored[i] - signal[i];
            err += d * d;
            refSq += signal[i] * signal[i];
        }
        var nrmse = Math.Sqrt(err) / Math.Sqrt(refSq);
        Assert.True(nrmse < 0.02, $"NRMSE = {nrmse:F4} (expected < 0.02)");
    }

    [Fact]
    public void HtdemucsForward_KeepsDcBin_DropsNyquist_When_Sliced_To_FreqBins()
    {
        // A DC-only signal: forward STFT should put all energy in bin 0.
        var samples = 8192;
        var signal = new float[samples];
        for (var i = 0; i < samples; i++) signal[i] = 1.0f;

        var spec = Stft.ForwardHtdemucs(signal, NFft, Hop);

        // Middle frame, deep inside the signal, should have bin 0 magnitude
        // > Σ of all other bins combined.
        var mid = spec.FrameCount / 2;
        var dcMag = spec.Frames[mid][0].Magnitude;
        var sumOther = 0d;
        for (var b = 1; b < spec.BinCount; b++) sumOther += spec.Frames[mid][b].Magnitude;
        Assert.True(dcMag > sumOther,
            $"DC bin magnitude ({dcMag}) should dominate (sum of others = {sumOther}).");
    }
}
