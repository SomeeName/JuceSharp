using System.Runtime.CompilerServices;
using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// D9 / D10 / D11 and cleanups 20 and 26 on the ONNX drivers.
/// <para>Three of these are asserted on the SOURCE, and that is deliberate: the
/// drivers cannot be constructed without a real ONNX session and a multi-hundred-MB
/// model file, so the alternative is no guard at all — which is how the
/// cancellation gap and the per-call engine survived in the first place. Each
/// source assertion names the exact construct, so a rename fails it loudly rather
/// than passing vacuously.</para>
/// </summary>
public sealed class OnnxDriverAuditTests
{
    // ------------------------------------------------- D9: model-output scrub

    /// <summary>
    /// THE POINT. A model output is an untrusted stage. The overlap-add is
    /// <c>out[idx+i] += res[i] * window[i]</c> over OVERLAPPING chunks, so one
    /// non-finite sample does not stay in its own chunk — it poisons the tail of
    /// the previous one, survives the normalisation divide, and rides the
    /// resampler into the WAV. Nothing downstream rejects it, because every gate
    /// is "reject when &lt;= threshold" and <c>NaN &lt;= x</c> is false.
    /// </summary>
    [Fact]
    public void Chunker_ScrubsNonFiniteModelOutput_AndCountsIt()
    {
        const int chunkSize = 256;
        var chunker = new OverlapAddChunker(chunkSize, overlap: 4);
        var stereo = new float[8 * chunkSize];
        for (var i = 0; i < stereo.Length; i++) stereo[i] = 0.25f;

        var chunkIndex = 0;
        var result = chunker.Run(stereo, (l, r) =>
        {
            var outL = (float[])l.Clone();
            var outR = (float[])r.Clone();
            // Every third chunk comes back with a burst of rubbish.
            if (chunkIndex++ % 3 == 0)
            {
                outL[10] = float.NaN;
                outL[11] = float.PositiveInfinity;
                outR[12] = float.NegativeInfinity;
            }
            return (outL, outR);
        }, progress: null, ct: CancellationToken.None);

        Assert.True(chunker.NonFiniteOutputs >= 3,
            $"the scrub must be observable, counted {chunker.NonFiniteOutputs}");
        foreach (var s in result)
            Assert.True(float.IsFinite(s), "no non-finite sample may reach the caller");
    }

    /// <summary>Without a fault the chunker must reconstruct the input — a
    /// windowed overlap-add normalised by its own weight curve is identity.</summary>
    [Fact]
    public void Chunker_IsIdentityForAPassThroughModel()
    {
        const int chunkSize = 512;
        var chunker = new OverlapAddChunker(chunkSize, overlap: 4);
        var rng = new Random(9);
        var stereo = new float[16 * chunkSize];
        for (var i = 0; i < stereo.Length; i++) stereo[i] = (float)(rng.NextDouble() * 2 - 1);

        var result = chunker.Run(stereo, (l, r) => (l.ToArray(), r.ToArray()),
            progress: null, ct: CancellationToken.None);

        Assert.Equal(0, chunker.NonFiniteOutputs);
        Assert.Equal(stereo.Length, result.Length);
        for (var i = 0; i < stereo.Length; i++)
            Assert.True(MathF.Abs(stereo[i] - result[i]) < 1e-4f, $"sample {i}: {result[i]} vs {stereo[i]}");
    }

    /// <summary>Every stem shares one weight curve, so a multi-stem run must
    /// reconstruct each stem independently and identically.</summary>
    [Fact]
    public void Chunker_MultiStemKeepsEachStemSeparate()
    {
        const int chunkSize = 256;
        var chunker = new OverlapAddChunker(chunkSize, overlap: 4);
        var stereo = new float[8 * chunkSize];
        for (var i = 0; i < stereo.Length; i++) stereo[i] = MathF.Sin(i * 0.01f);

        var stems = chunker.RunMulti(stereo, 3, (l, r) =>
        [
            (Scale(l, 1f), Scale(r, 1f)),
            (Scale(l, 0.5f), Scale(r, 0.5f)),
            (Scale(l, 0.25f), Scale(r, 0.25f)),
        ], progress: null, ct: CancellationToken.None);

        Assert.Equal(3, stems.Length);
        for (var i = chunkSize; i < stereo.Length - chunkSize; i++)
        {
            Assert.True(MathF.Abs(stems[0][i] - stereo[i]) < 1e-4f);
            Assert.True(MathF.Abs(stems[1][i] - stereo[i] * 0.5f) < 1e-4f);
            Assert.True(MathF.Abs(stems[2][i] - stereo[i] * 0.25f) < 1e-4f);
        }

        static float[] Scale(float[] x, float g)
        {
            var y = new float[x.Length];
            for (var i = 0; i < x.Length; i++) y[i] = x[i] * g;
            return y;
        }
    }

    [Fact]
    public void Chunker_ObservesCancellation()
    {
        using var cts = new CancellationTokenSource();
        var chunker = new OverlapAddChunker(128, overlap: 4);
        var stereo = new float[16 * 128];

        Assert.Throws<OperationCanceledException>(() => chunker.Run(stereo, (l, r) =>
        {
            cts.Cancel();
            return (l.ToArray(), r.ToArray());
        }, progress: null, ct: cts.Token));
    }

    // ------------------------------------------------ input packing (cleanup 20)

    /// <summary>
    /// The layout the models expect is channel-first <c>[1, 4, dimF, dimT]</c>
    /// with channels <c>[re_L, im_L, re_R, im_R]</c>. Getting the index
    /// arithmetic wrong when moving off <c>DenseTensor</c>'s indexer would
    /// scramble the spectrogram without failing anything.
    /// </summary>
    [Fact]
    public void PackInput_UsesTheChannelFirstLayout()
    {
        const int nFft = 8;
        const int hop = 2;
        const int dimT = 3;
        const int dimF = 4;
        var bins = nFft / 2 + 1;

        var l = new SpectrumPlanes(dimT, nFft, hop);
        var r = new SpectrumPlanes(dimT, nFft, hop);
        for (var t = 0; t < dimT; t++)
        for (var f = 0; f < bins; f++)
        {
            l.Re[t][f] = 1000 + t * 10 + f;
            l.Im[t][f] = 2000 + t * 10 + f;
            r.Re[t][f] = 3000 + t * 10 + f;
            r.Im[t][f] = 4000 + t * 10 + f;
        }

        var dst = new float[4 * dimF * dimT];
        SpectralOnnxRunner.PackInput(dst, l, r, dimF, dimT, dimT, bins);

        var plane = dimF * dimT;
        for (var t = 0; t < dimT; t++)
        for (var f = 0; f < dimF; f++)
        {
            var idx = f * dimT + t;
            Assert.Equal(l.Re[t][f], dst[0 * plane + idx]);
            Assert.Equal(l.Im[t][f], dst[1 * plane + idx]);
            Assert.Equal(r.Re[t][f], dst[2 * plane + idx]);
            Assert.Equal(r.Im[t][f], dst[3 * plane + idx]);
        }
    }

    /// <summary>Bins the transform does not have must be left ZERO, not read out
    /// of bounds and not filled with the last value.</summary>
    [Fact]
    public void PackInput_ZeroFillsBeyondTheAvailableFramesAndBins()
    {
        const int nFft = 8;
        var bins = nFft / 2 + 1;   // 5
        const int dimT = 4;
        const int dimF = 7;        // wider than bins
        var l = new SpectrumPlanes(2, nFft, 2);
        var r = new SpectrumPlanes(2, nFft, 2);
        for (var t = 0; t < 2; t++)
        for (var f = 0; f < bins; f++) { l.Re[t][f] = 1; l.Im[t][f] = 1; r.Re[t][f] = 1; r.Im[t][f] = 1; }

        var dst = new float[4 * dimF * dimT];
        for (var i = 0; i < dst.Length; i++) dst[i] = -1f;   // poison, must be overwritten by the caller's clear
        Array.Clear(dst);
        SpectralOnnxRunner.PackInput(dst, l, r, dimF, dimT, frames: 2, bins: bins);

        var plane = dimF * dimT;
        for (var t = 2; t < dimT; t++)
        for (var f = 0; f < dimF; f++)
            Assert.Equal(0f, dst[0 * plane + f * dimT + t]);
        for (var f = bins; f < dimF; f++)
            Assert.Equal(0f, dst[0 * plane + f * dimT + 0]);
    }

    [Fact]
    public void InputBuffer_IsReusedAcrossChunksAndAlwaysHandedBackZeroed()
    {
        // The runner needs no session for this; only the buffer path is exercised.
        var runner = (SpectralOnnxRunner)RuntimeHelpers.GetUninitializedObject(typeof(SpectralOnnxRunner));
        var field = typeof(SpectralOnnxRunner).GetField("_input",
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!;
        field.SetValue(runner, Array.Empty<float>());

        var first = runner.InputBuffer(4, 3);
        first[0] = 42f;
        var second = runner.InputBuffer(4, 3);

        Assert.Same(first, second);
        Assert.Equal(0f, second[0]);
    }

    // ----------------------------------------------------------- D11: the probe

    /// <summary>
    /// The provider list was a STATIC FIELD INITIALISER calling
    /// <c>OrtEnv.Instance()</c>, so a missing or mismatched
    /// <c>onnxruntime.dll</c> surfaced as a <c>TypeInitializationException</c>
    /// two inner exceptions deep, from whichever member of the class the caller
    /// touched first. Reading it must now always be safe.
    /// </summary>
    [Fact]
    public void AvailableNativeProviders_NeverThrows()
    {
        var providers = OnnxSessionFactory.AvailableNativeProviders;
        Assert.NotNull(providers);
        // Either the runtime loaded and named its providers, or it did not and
        // said why. "Empty and silent" is the one state that must not happen.
        if (providers.Count == 0)
            Assert.False(string.IsNullOrEmpty(OnnxSessionFactory.NativeRuntimeFailure),
                "an empty provider list must carry the reason it is empty");
    }

    [Fact]
    public void AvailableNativeProviders_IsProbedOnceAndCached()
    {
        var a = OnnxSessionFactory.AvailableNativeProviders;
        var b = OnnxSessionFactory.AvailableNativeProviders;
        Assert.Same(a, b);
    }

    // -------------------------------------------------------- source assertions

    /// <summary>
    /// D10. Cancellation was observed only between chunks, and one chunk is
    /// seconds of inference. Every session Run must be wrapped in a
    /// <c>RunOptions.Terminate</c> registration, and — because Terminate makes
    /// Run return EARLY rather than throw — the token must be re-checked after
    /// it.
    /// </summary>
    [Theory]
    [InlineData("HtdemucsOnnxInference.cs")]
    [InlineData("SpectralOnnxRunner.cs")]
    public void EverySessionRun_IsCancellable(string file)
    {
        var src = Source(file);
        // Either the literal, or the shared helper that owns it. The 0.9 wave
        // routed both drivers through SpectralOnnxRunner.RequestTerminate so
        // that an OnnxRuntimeException out of Terminate could not surface where
        // IStemSeparator promises OperationCanceledException — the invariant is
        // "termination is REQUESTED around the Run", not the spelling of it.
        Assert.True(src.Contains("Terminate = true", StringComparison.Ordinal)
                    || src.Contains("RequestTerminate", StringComparison.Ordinal),
                    $"{file} neither sets RunOptions.Terminate nor calls RequestTerminate, "
                    + "so a cancel cannot interrupt a chunk of inference.");
        Assert.Contains("ct.Register(", src, StringComparison.Ordinal);
        Assert.Contains("_session.Run(", src, StringComparison.Ordinal);
        Assert.Contains("ThrowIfCancellationRequested", src, StringComparison.Ordinal);
    }

    /// <summary>
    /// The two spectral drivers must not go back to the
    /// <c>NamedOnnxValue.CreateFromTensor</c> overload: it allocates a fresh
    /// <c>DenseTensor</c> per chunk (12 MB of Large Object Heap for BS-RoFormer)
    /// and cannot carry RunOptions, which is what made them uncancellable.
    /// </summary>
    [Theory]
    [InlineData("BsRoformerInference.cs")]
    [InlineData("MdxNetInference.cs")]
    public void SpectralDrivers_DoNotBoxATensorPerChunk(string file)
    {
        var src = Source(file);
        Assert.DoesNotContain("NamedOnnxValue.CreateFromTensor", src, StringComparison.Ordinal);
        Assert.DoesNotContain("new DenseTensor<float>", src, StringComparison.Ordinal);
        Assert.Contains("SpectralOnnxRunner.PackInput", src, StringComparison.Ordinal);
    }

    /// <summary>
    /// Cleanup 26. The htdemucs engine's chunk scratch is hundreds of MB and was
    /// rebuilt on every <c>SeparateAsync</c>.
    /// </summary>
    [Fact]
    public void HtdemucsEngine_IsCachedPerSeparatorInstance()
    {
        var src = Source("OnnxStemSeparator.cs");
        Assert.Contains("_htdemucs ??= new HtdemucsOnnxInference", src, StringComparison.Ordinal);
        Assert.DoesNotContain("var engine = new HtdemucsOnnxInference", src, StringComparison.Ordinal);
    }

    /// <summary>
    /// The dead <c>filled</c> cursor: written on the last line of
    /// <c>BlendChunkIntoOutput</c>, threaded through by <c>ref</c>, guarded by an
    /// <c>isFirstStem</c> flag, and never read by anything.
    /// </summary>
    [Fact]
    public void TheDeadFilledCursorIsGone()
    {
        var src = Source("HtdemucsOnnxInference.cs");
        Assert.DoesNotContain("ref int filledForStem", src, StringComparison.Ordinal);
        Assert.DoesNotContain("isFirstStem:", src, StringComparison.Ordinal);
    }

    private static string Source(string fileName)
    {
        var path = Path.Combine(RepoRoot(), "src", "JuceSharp.StemSeparation.Onnx", fileName);
        Assert.True(File.Exists(path), $"could not find {path} — has the file moved?");
        return File.ReadAllText(path);
    }

    private static string RepoRoot([CallerFilePath] string? thisFile = null)
    {
        var dir = Path.GetDirectoryName(thisFile);
        while (dir != null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        return dir ?? throw new InvalidOperationException("Could not locate the JuceSharp root.");
    }
}
