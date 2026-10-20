using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// BS-RoFormer and MDX-Net are 44.1 kHz models and their drivers take "stereo
/// at 44.1 kHz" on trust — neither reads a rate. Until this seam existed a
/// 48 kHz session was fed 48 kHz samples as 44.1 kHz ones (htdemucs always
/// resampled; these two never did). The contract is pinned on
/// <see cref="OnnxStemSeparator.SeparateAtModelRate"/> with a fake predictor,
/// so no model file is involved: the predictor must RECEIVE the model rate's
/// frame count, and the caller must GET BACK the input rate's frame count.
/// </summary>
public sealed class SpectralModelResampleTests
{
    private const int Frames48k = 48_000; // one second
    private const int ModelRate = OnnxStemSeparator.SpectralModelSampleRate;

    private static float[] Sine(int frames, int rate, double hz)
    {
        var stereo = new float[frames * 2];
        for (var i = 0; i < frames; i++)
        {
            var v = (float)Math.Sin(2 * Math.PI * hz * i / rate);
            stereo[2 * i] = v;
            stereo[2 * i + 1] = v;
        }
        return stereo;
    }

    [Fact]
    public void A48kInput_IsHandedToTheModelAt44k1_AndComesBackAt48k_WithTheInputFrameCount()
    {
        var input = Sine(Frames48k, 48_000, 440.0);
        var seenFrames = -1;

        var stems = OnnxStemSeparator.SeparateAtModelRate(input, 48_000, ModelRate, "test", null,
            modelInput =>
            {
                seenFrames = modelInput.Length / 2;
                // The predictor's own output is at the MODEL rate, same length as
                // what it was given — what both real drivers do.
                return new[] { ("vocals", modelInput), ("other", new float[modelInput.Length]) };
            });

        Assert.Equal(44_100, seenFrames);
        Assert.Equal(2, stems.Count);
        foreach (var stem in stems)
        {
            Assert.Equal(48_000, stem.SampleRate);
            Assert.Equal(Frames48k * 2, stem.InterleavedStereo.Length);
        }

        // The 440 Hz tone survives the round trip as a 440 Hz tone at 48 kHz:
        // compare against the ORIGINAL 48 kHz signal well away from the edges
        // (the Lanczos kernel needs 8 lobes of context on each side).
        var back = stems[0].InterleavedStereo;
        var maxErr = 0.0;
        for (var i = 1_000; i < Frames48k - 1_000; i++)
            maxErr = Math.Max(maxErr, Math.Abs(back[2 * i] - input[2 * i]));
        Assert.True(maxErr < 0.02, $"round-trip error {maxErr:0.0000} — the stem is not at the input rate");
    }

    [Fact]
    public void AnInputAlreadyAtTheModelRate_IsNotCopiedOrResampled()
    {
        var input = Sine(ModelRate, ModelRate, 440.0);
        float[]? seen = null;

        var stems = OnnxStemSeparator.SeparateAtModelRate(input, ModelRate, ModelRate, "test", null,
            modelInput =>
            {
                seen = modelInput;
                return new[] { ("vocals", modelInput) };
            });

        Assert.Same(input, seen);
        Assert.Same(input, Assert.Single(stems).InterleavedStereo);
        Assert.Equal(ModelRate, stems[0].SampleRate);
    }

    [Fact]
    public void BothSpectralDrivers_GoThroughTheModelRateSeam()
    {
        // The seam proves the contract; this proves the two call sites USE it.
        // A driver that hands `interleavedStereo` straight to the engine again
        // is the bug this file exists for.
        var source = File.ReadAllText(SeparatorSourcePath());
        foreach (var method in new[] { "SeparateBsRoformer", "SeparateMdxNet" })
        {
            var start = source.IndexOf($"SeparateAtModelRate(interleavedStereo, sampleRate, SpectralModelSampleRate", StringComparison.Ordinal);
            Assert.True(start >= 0, "no call to the seam at all");
            var body = MethodBody(source, method);
            Assert.Contains("SeparateAtModelRate(interleavedStereo, sampleRate, SpectralModelSampleRate", body);
            Assert.DoesNotContain("Predict", body.Substring(0, body.IndexOf("SeparateAtModelRate", StringComparison.Ordinal)));
        }
    }

    private static string MethodBody(string source, string methodName)
    {
        var at = source.IndexOf($"private IReadOnlyList<OnnxStemBuffer> {methodName}(", StringComparison.Ordinal);
        Assert.True(at >= 0, $"{methodName} not found");
        var open = source.IndexOf('{', at);
        var depth = 0;
        for (var i = open; i < source.Length; i++)
        {
            if (source[i] == '{') depth++;
            else if (source[i] == '}' && --depth == 0) return source.Substring(open, i - open + 1);
        }
        throw new InvalidOperationException("unbalanced braces");
    }

    private static string SeparatorSourcePath([System.Runtime.CompilerServices.CallerFilePath] string? thisFile = null)
    {
        var testsDir = Path.GetDirectoryName(thisFile!)!;
        var path = Path.GetFullPath(Path.Combine(testsDir, "..", "..", "src", "JuceSharp.StemSeparation.Onnx", "OnnxStemSeparator.cs"));
        Assert.True(File.Exists(path), path);
        return path;
    }

    [Fact]
    public void TheDriversAreDeclaredAt44k1_AndThePassthroughRateIsThatConstant()
    {
        // The constant is what SeparateBsRoformer / SeparateMdxNet hand to the
        // seam; a typo there would silently undo the fix.
        Assert.Equal(44_100, OnnxStemSeparator.SpectralModelSampleRate);
        Assert.Equal(HtdemucsOnnxInference.SampleRate, OnnxStemSeparator.SpectralModelSampleRate);
    }
}
