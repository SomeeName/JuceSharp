using JuceSharp.StemSeparation.Onnx;
using Microsoft.ML.OnnxRuntime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The split-complex BS-RoFormer path with no real model (DotCharter #267): MSST's
/// chunk / fade loop (<see cref="FadeWindowChunker"/>), the two-slot pipeline that overlaps a
/// chunk's host work with the next chunk's inference (<see cref="ChunkPipeline"/>), the
/// metadata read that decides DirectML graph fusion (<see cref="OnnxModelMetadata"/>), the
/// resampler's phase table, and the whole driver end to end on a tiny ONNX model.
/// </summary>
public sealed class BsRoformerSwPipelineTests
{
    // ------------------------------------------------------------ FadeWindowChunker

    [Fact]
    public void TheTrainingChunk_GivesMsstsGeometry_AndThirtySixChunksForTheTestSong()
    {
        var chunker = new FadeWindowChunker(588_800, numOverlap: 2);

        Assert.Equal(294_400, chunker.Step);
        Assert.Equal(294_400, chunker.Border);
        Assert.Equal(58_880, chunker.FadeSize);
        // The 224.3 s test song at 44.1 kHz: PyTorch's demix runs 36 chunks over it.
        Assert.Equal(36, chunker.Begin(new float[2 * 9_891_924], 6).Count);
    }

    [Fact]
    public void TheWindow_IsMsstsDocumentedExample()
    {
        // utils/model_utils.py, _getWindowingArray's docstring: window_size=10, fade_size=3.
        Assert.Equal([0f, 0.5f, 1f, 1f, 1f, 1f, 1f, 1f, 0.5f, 0f], FadeWindowChunker.Window(10, 3));
    }

    /// <summary>MSST's own <c>demix</c> (ZFTurbo/Music-Source-Separation-Training at 84b1eac,
    /// 2026-09-26) run with a fake two-stem model, chunk 50, num_overlap 2, batch 1: stem
    /// <c>a</c> = x * (1 + mean|x| of the chunk) depends on the chunk, so every weight, pad
    /// and fade shows; stem <c>b</c> = x * x does not. Values copied from that run. 237 frames
    /// reflect-pad a border, 70 also end on a short reflect-padded chunk, 20 is shorter than
    /// two borders (no border) and than half a chunk (zero-padded).</summary>
    [Theory]
    [InlineData(237, new[] { 0, 1, 4, 5, 24, 25, 26, 49, 118, 231, 235, 236 },
        new[] { 0f, 0.5953099f, 1.63862f, 1.582469f, 0.8663685f, 0.3024318f, -0.3001175f, -1.047139f, -0.4368095f, -0.8324386f, -1.23568f, -0.8258004f },
        new[] { 0.650067f, 0.6387622f, 0.4389741f, 0.3272404f, 0.2140766f, 0.3406826f, 0.4520374f, -0.4300722f, 0.6231643f, -0.1186299f, 0.3936769f, 0.4917981f })]
    [InlineData(70, new[] { 0, 1, 4, 5, 24, 25, 26, 35, 49, 64, 68, 69 },
        new[] { 0f, 0.5981478f, 1.646585f, 1.592043f, 0.9049711f, 0.3433133f, -0.2557225f, 0.6870316f, -0.9428321f, -1.454362f, 0.2006631f, 0.7830865f },
        new[] { 0.650067f, 0.6387622f, 0.4389741f, 0.3272404f, 0.2140766f, 0.3406826f, 0.4504152f, 0.317895f, -0.4223363f, 0.4193194f, -0.09296688f, -0.2256825f })]
    [InlineData(20, new[] { 0, 1, 4, 5, 10, 14, 18, 19 },
        new[] { 0f, 0.453044f, 1.255372f, 1.218787f, -0.5929561f, -1.016585f, 0.5659234f, 0.9568447f },
        new[] { 0.5696561f, 0.5571412f, 0.3802243f, 0.2834444f, -0.2875887f, -0.5581199f, -0.45746f, -0.3766445f })]
    public void TheChunker_ReproducesMsstsDemix(int frames, int[] at, float[] expectedLeft, float[] expectedRight)
    {
        var mix = new float[2 * frames];
        for (var t = 0; t < frames; t++)
        {
            mix[2 * t] = (float)(Math.Sin(t * 0.37) + 0.1 * t / frames);
            mix[2 * t + 1] = (float)(Math.Cos(t * 0.21) * 0.5);
        }

        var stems = new FadeWindowChunker(50, 2).RunMulti(mix, 2, (l, r) =>
        {
            var ml = l.Average(Math.Abs);
            var mr = r.Average(Math.Abs);
            return
            [
                (l.Select(x => x * (1 + ml)).ToArray(), r.Select(x => x * (1 + mr)).ToArray()),
                (l.Select(x => x * x).ToArray(), r.Select(x => x * x).ToArray()),
            ];
        }, null, CancellationToken.None);

        for (var i = 0; i < at.Length; i++)
        {
            Assert.Equal(expectedLeft[i], stems[0][2 * at[i]], 5);
            Assert.Equal(expectedRight[i], stems[0][2 * at[i] + 1], 5);
        }
        // The chunk-independent stem comes back exactly, padding and fades notwithstanding.
        for (var t = 0; t < frames; t++)
            Assert.Equal(mix[2 * t] * mix[2 * t], stems[1][2 * t], 5);
    }

    [Theory]
    [InlineData(1_000, 100)]
    [InlineData(150, 100)]
    [InlineData(100, 100)]
    [InlineData(40, 100)]
    [InlineData(1, 100)]
    public void AModelThatReturnsItsInput_GivesTheMixBack(int frames, int chunk)
    {
        var random = new Random(frames);
        var mix = Enumerable.Range(0, 2 * frames).Select(_ => (float)(random.NextDouble() - 0.5)).ToArray();

        var stems = new FadeWindowChunker(chunk, 2).RunMulti(mix, 1, (l, r) => [(l, r)], null, CancellationToken.None);

        Assert.Equal(mix.Length, stems[0].Length);
        for (var i = 0; i < mix.Length; i++) Assert.Equal(mix[i], stems[0][i], 5);
    }

    [Fact]
    public void ANonFiniteModelSample_IsSilencedAndCounted()
    {
        var pass = new FadeWindowChunker(100, 2).Begin(new float[2 * 400], 1);
        var l = new float[100];
        var r = new float[100];
        for (var k = 0; k < pass.Count; k++)
        {
            pass.Fill(k, l, r);
            var bad = (float[])l.Clone();
            bad[10] = float.NaN; // inside every segment, the short last one included
            pass.Accumulate(k, [(bad, r)]);
        }

        Assert.Equal(pass.Count, pass.NonFiniteOutputs);
        Assert.All(pass.Finish()[0], v => Assert.True(float.IsFinite(v)));
    }

    // ------------------------------------------------------------ ChunkPipeline

    [Fact]
    public void ThePipeline_OverlapsAChunksPostWithTheNextRun_AndNeverSharesASlot()
    {
        var busy = new int[2];
        var events = new List<string>();
        var overlapped = 0;
        var runStarted = Enumerable.Range(0, 6).Select(_ => new ManualResetEventSlim()).ToArray();

        ChunkPipeline.Run(6,
            (k, slot) =>
            {
                Assert.Equal(k % 2, slot);
                Assert.Equal(0, Interlocked.Exchange(ref busy[slot], 1));
                lock (events) events.Add($"run {k}");
                runStarted[k].Set();
                Thread.Sleep(20);
                Interlocked.Exchange(ref busy[slot], 0);
            },
            (k, slot) =>
            {
                Assert.Equal(0, Interlocked.Exchange(ref busy[slot], 1));
                // The next run starts while this post is still going (there is none after the last).
                if (k < 5 && runStarted[k + 1].Wait(TimeSpan.FromSeconds(5))) Interlocked.Increment(ref overlapped);
                lock (events) events.Add($"post {k}");
                Interlocked.Exchange(ref busy[slot], 0);
            },
            CancellationToken.None);

        Assert.Equal(5, overlapped);
        Assert.Equal(12, events.Count);
        for (var k = 0; k < 6; k++)
            Assert.True(events.IndexOf($"run {k}") < events.IndexOf($"post {k}"), string.Join(", ", events));
        // post k finished before run k + 2 could reuse its slot.
        for (var k = 0; k < 4; k++)
            Assert.True(events.IndexOf($"post {k}") < events.IndexOf($"run {k + 2}"), string.Join(", ", events));
    }

    [Fact]
    public void AFailingRun_DrainsThePendingPostBeforeItSurfaces()
    {
        var postDone = false;
        var ex = Assert.Throws<InvalidOperationException>(() => ChunkPipeline.Run(4,
            (k, _) =>
            {
                if (k == 1) throw new InvalidOperationException("run 1");
            },
            (_, _) =>
            {
                Thread.Sleep(100);
                Volatile.Write(ref postDone, true);
            },
            CancellationToken.None));

        Assert.Equal("run 1", ex.Message);
        Assert.True(Volatile.Read(ref postDone));
    }

    [Fact]
    public void AFailingPost_Surfaces()
    {
        var ex = Assert.Throws<InvalidOperationException>(() => ChunkPipeline.Run(4,
            (_, _) => { },
            (k, _) =>
            {
                if (k == 2) throw new InvalidOperationException("post 2");
            },
            CancellationToken.None));

        Assert.Equal("post 2", ex.Message);
    }

    [Fact]
    public void ACancel_StopsBeforeTheNextChunk_AndDrains()
    {
        using var cts = new CancellationTokenSource();
        var runs = 0;
        var posts = 0;
        Assert.ThrowsAny<OperationCanceledException>(() => ChunkPipeline.Run(10,
            (k, _) =>
            {
                runs++;
                if (k == 2) cts.Cancel();
            },
            (_, _) =>
            {
                Thread.Sleep(30);
                Interlocked.Increment(ref posts);
            },
            cts.Token));

        Assert.Equal(3, runs);
        Assert.Equal(3, Volatile.Read(ref posts));
    }

    // ------------------------------------------------------------ metadata and fusion

    [Fact]
    public void TheMetadata_IsReadFromBehindTheGraph()
    {
        var path = WriteTiny(new Dictionary<string, string>
        {
            ["chunk_samples"] = "64",
            [OnnxSessionFactory.DirectMlGraphFusionKey] = OnnxSessionFactory.DirectMlGraphFusionVerified,
        });

        var metadata = OnnxModelMetadata.Read(path);

        Assert.Equal("64", metadata["chunk_samples"]);
        Assert.True(OnnxSessionFactory.AllowsDirectMlGraphFusion(metadata));
    }

    [Fact]
    public void AFileWithoutMetadata_OrNotAModel_HasNone()
    {
        Assert.Empty(OnnxModelMetadata.Read(WriteTiny(new Dictionary<string, string>())));
        var garbage = Path.Combine(Path.GetTempPath(), $"not-a-model-{Guid.NewGuid():N}.onnx");
        File.WriteAllBytes(garbage, [0xFF, 0xFF, 0xFF, 0x0A, 0x7F, 0x01, 0x02]);
        Assert.Empty(OnnxModelMetadata.Read(garbage));
        Assert.Empty(OnnxModelMetadata.Read(garbage + ".missing"));
    }

    [Fact]
    public void ATruncatedModel_KeepsTheEntriesBeforeTheCut_AndDoesNotThrow()
    {
        var full = File.ReadAllBytes(WriteTiny(new Dictionary<string, string> { ["a"] = "1", ["b"] = "2" }));
        var cut = Path.Combine(Path.GetTempPath(), $"cut-{Guid.NewGuid():N}.onnx");
        File.WriteAllBytes(cut, full[..^3]);

        var metadata = OnnxModelMetadata.Read(cut);

        Assert.Equal("1", metadata["a"]);
        Assert.False(metadata.ContainsKey("b"));
    }

    [Theory]
    [InlineData("verified", true)]
    [InlineData(" verified ", true)]
    [InlineData("Verified", false)]
    [InlineData("off-for-benchmark", false)]
    [InlineData(null, false)]
    public void OnlyAVerifiedModel_KeepsDirectMlGraphFusionOn(string? value, bool allowed)
    {
        var metadata = value is null
            ? new Dictionary<string, string>()
            : new Dictionary<string, string> { [OnnxSessionFactory.DirectMlGraphFusionKey] = value };
        Assert.Equal(allowed, OnnxSessionFactory.AllowsDirectMlGraphFusion(metadata));
    }

    // ------------------------------------------------------------ resampler phase table

    [Theory]
    [InlineData(48_000, 44_100)]
    [InlineData(44_100, 48_000)]
    [InlineData(96_000, 44_100)]
    [InlineData(44_100, 47_999)]
    public void ThePhaseTable_MatchesTheDirectKernel(int from, int to)
    {
        var random = new Random(from ^ to);
        var input = Enumerable.Range(0, 2 * 5_000).Select(_ => (float)(random.NextDouble() - 0.5)).ToArray();

        var table = SampleRateConverter.Resample(input, 2, from, to);
        var direct = SampleRateConverter.ResampleDirect(input, 2, from, to);

        Assert.Equal(direct.Length, table.Length);
        for (var i = 0; i < direct.Length; i++) Assert.Equal(direct[i], table[i], 5);
    }

    // ------------------------------------------------------------ the driver, end to end

    /// <summary>
    /// The real ONNX Runtime and the whole split-complex driver - geometry from the model's
    /// metadata, FadeWindowChunker, ChunkPipeline, SplitComplexIoBinding, the reflect STFT and
    /// the iSTFT - on a tiny model whose stem (s, c) is <c>w[s, c]</c> times the mix. Several
    /// chunks, a short reflect-padded tail, and a second song through the same separator.
    /// </summary>
    [Fact]
    public async Task TheDriver_OnATinyModel_GivesEachStemItsWeightTimesTheMix()
    {
        float[,] weights = { { 0.5f, 0.25f }, { 0.3f, 0.7f }, { 0.2f, 0.05f } };
        var path = WriteTiny(new Dictionary<string, string>
        {
            ["n_fft"] = "16",
            ["hop_length"] = "4",
            ["chunk_samples"] = "64",
            ["sample_rate"] = "44100",
            ["stems"] = "[\"bass\", \"drums\", \"other\"]",
        }, weights);

        using var separator = new OnnxStemSeparator(path, OnnxStemModelKind.BsRoformer, OnnxExecutionProvider.Cpu);
        foreach (var frames in new[] { 1_000, 333 })
        {
            var random = new Random(frames);
            var mix = Enumerable.Range(0, 2 * frames).Select(_ => (float)(random.NextDouble() - 0.5)).ToArray();

            var stems = await separator.SeparateAsync(mix, 44_100, ct: TestContext.Current.CancellationToken);

            Assert.Equal(["bass", "drums", "other"], stems.Select(s => s.Name));
            for (var s = 0; s < 3; s++)
            {
                var stem = stems[s].InterleavedStereo;
                Assert.Equal(mix.Length, stem.Length);
                for (var i = 0; i < mix.Length; i++)
                    Assert.Equal(weights[s, i % 2] * mix[i], stem[i], 4);
            }
        }
    }

    private static string WriteTiny(IReadOnlyDictionary<string, string> metadata, float[,]? weights = null)
    {
        var path = Path.Combine(Path.GetTempPath(), $"tiny-split-{Guid.NewGuid():N}.onnx");
        File.WriteAllBytes(path, TinyOnnxModel.SplitComplexBsRoformer(weights ?? new float[,] { { 1f, 1f } },
            bins: 9, frames: 17, metadata));
        return path;
    }
}
