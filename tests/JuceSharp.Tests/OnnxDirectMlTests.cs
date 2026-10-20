using JuceSharp.StemSeparation.Onnx;
using Microsoft.ML.OnnxRuntime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The ONNX stem backend runs on the DirectML build of ONNX Runtime (no CUDA). These
/// pin the parts of that switch a GPU-less machine can check — the provider fallback
/// policy, the DirectML session settings, what the shipped runtime contains and that
/// DirectML.dll is deployed — plus one GPU-gated parity run that skips cleanly
/// without a DirectX 12 adapter or the model file.
/// </summary>
public sealed class OnnxDirectMlTests
{
    [Theory]
    [InlineData(OnnxExecutionProvider.Auto, new[] { OnnxExecutionProvider.DirectML, OnnxExecutionProvider.Cuda, OnnxExecutionProvider.Cpu })]
    [InlineData(OnnxExecutionProvider.DirectML, new[] { OnnxExecutionProvider.DirectML, OnnxExecutionProvider.Cuda, OnnxExecutionProvider.Cpu })]
    [InlineData(OnnxExecutionProvider.Cuda, new[] { OnnxExecutionProvider.Cuda, OnnxExecutionProvider.DirectML, OnnxExecutionProvider.Cpu })]
    [InlineData(OnnxExecutionProvider.Cpu, new[] { OnnxExecutionProvider.Cpu })]
    public void FallbackOrder_TriesDirectMlFirstForAuto_AndEveryChainEndsOnTheCpu(
        OnnxExecutionProvider preferred, OnnxExecutionProvider[] expected)
    {
        Assert.Equal(expected, OnnxSessionFactory.FallbackOrder(preferred));
    }

    [Fact]
    public void FallbackNote_IsNullWhenTheRequestWasHonoured()
    {
        Assert.Null(OnnxSessionFactory.FallbackNote(OnnxExecutionProvider.DirectML, OnnxExecutionProvider.DirectML, null));
        Assert.Null(OnnxSessionFactory.FallbackNote(OnnxExecutionProvider.Cpu, OnnxExecutionProvider.Cpu, null));
        // Auto asks for the best GPU provider there is; DirectML after a missing CUDA is that.
        Assert.Null(OnnxSessionFactory.FallbackNote(OnnxExecutionProvider.Auto, OnnxExecutionProvider.DirectML, "Cuda: absent"));
    }

    [Fact]
    public void FallbackNote_CarriesTheReason_WhenAGpuRequestLandsOnTheCpu()
    {
        Assert.Equal("DirectML: absent",
            OnnxSessionFactory.FallbackNote(OnnxExecutionProvider.DirectML, OnnxExecutionProvider.Cpu, "DirectML: absent"));
        Assert.Equal("DirectML: absent",
            OnnxSessionFactory.FallbackNote(OnnxExecutionProvider.Auto, OnnxExecutionProvider.Cpu, "DirectML: absent"));
        Assert.Equal("Cuda: absent",
            OnnxSessionFactory.FallbackNote(OnnxExecutionProvider.Cuda, OnnxExecutionProvider.DirectML, "Cuda: absent"));
    }

    /// <summary>
    /// The correctness setting. With DirectML's graph fusion on, htdemucs separated into
    /// stems 35-65x too loud on every adapter tried (measured 2026-10-05); the GPU-gated
    /// parity test below is what fails if this entry goes.
    /// </summary>
    [Fact]
    public void DirectMlSessions_TurnDirectMlGraphFusionOff()
    {
        Assert.Contains(new KeyValuePair<string, string>("ep.dml.disable_graph_fusion", "1"),
            OnnxSessionFactory.DirectMlSessionConfig);
        using var options = new SessionOptions();
        OnnxSessionFactory.ApplyDirectMlOptions(options);
        Assert.False(options.EnableMemoryPattern);
        Assert.Equal(ExecutionMode.ORT_SEQUENTIAL, options.ExecutionMode);
    }

    [Fact]
    public void TheShippedRuntime_HasDirectMl_AndNoCuda()
    {
        if (!OperatingSystem.IsWindows()) Assert.Skip("The DirectML build of ONNX Runtime is Windows-only.");
        Assert.Null(OnnxSessionFactory.NativeRuntimeFailure);
        Assert.Contains("DmlExecutionProvider", OnnxSessionFactory.AvailableNativeProviders);
        Assert.DoesNotContain("CUDAExecutionProvider", OnnxSessionFactory.AvailableNativeProviders);
    }

    /// <summary>The package's own targets only copy DirectML.dll into a project that
    /// references it directly; this assembly reaches it through a ProjectReference, so
    /// this is the check that the None item travels.</summary>
    [Fact]
    public void DirectMlDll_IsDeployedBesideTheConsumer_AndTheCudaProviderIsNot()
    {
        if (!OperatingSystem.IsWindows()) Assert.Skip("DirectML.dll is Windows-only.");
        Assert.True(File.Exists(Path.Combine(AppContext.BaseDirectory, "DirectML.dll")),
            "DirectML.dll is not beside the test assembly; onnxruntime.dll would load the System32 copy.");
        Assert.False(File.Exists(Path.Combine(AppContext.BaseDirectory, "onnxruntime_providers_cuda.dll")),
            "the CUDA provider is still deployed; the Gpu package came back.");
    }

    [Fact]
    public void Adapters_AreListedInDeviceIdOrder()
    {
        var adapters = OnnxGpuAdapters.Enumerate();
        Assert.Equal(Enumerable.Range(0, adapters.Count), adapters.Select(a => a.DeviceId));
        Assert.All(adapters, a => Assert.False(string.IsNullOrWhiteSpace(a.Name)));
        if (OperatingSystem.IsWindows())
            Assert.Contains(adapters, a => a.IsSoftware); // WARP is always enumerated on Windows
    }

    [Fact]
    public void OpenWithCpuFallback_RebuildsOnTheCpu_WhenTheGpuCannotLoadTheModel()
    {
        var gpu = new OnnxSessionFactory.SessionBundle(new SessionOptions(), OnnxExecutionProvider.DirectML, "DirectML");
        var cpu = new OnnxSessionFactory.SessionBundle(new SessionOptions(), OnnxExecutionProvider.Cpu, "CPU");
        var logs = new List<string>();

        var (session, bundle, reason) = OnnxStemSeparator.OpenWithCpuFallback(gpu, () => cpu,
            options => options == gpu.Options ? throw new InvalidOperationException("out of video memory") : "cpu-session",
            "model.onnx", logs.Add);

        Assert.Equal("cpu-session", session);
        Assert.Same(cpu, bundle);
        Assert.Equal("DirectML: out of video memory", reason);
        Assert.True(gpu.Options.IsClosed, "the GPU bundle's native options leaked");
        Assert.False(cpu.Options.IsClosed);
        Assert.Contains(logs, l => l.Contains("Rebuilding the session on the CPU", StringComparison.Ordinal));
        cpu.Options.Dispose();
    }

    [Fact]
    public void OpenWithCpuFallback_KeepsTheGpu_AndItsFactoryNote_WhenItLoads()
    {
        using var gpuOptions = new SessionOptions();
        var gpu = new OnnxSessionFactory.SessionBundle(gpuOptions, OnnxExecutionProvider.DirectML, "DirectML");
        var (session, bundle, reason) = OnnxStemSeparator.OpenWithCpuFallback(gpu,
            () => throw new InvalidOperationException("the CPU must not be built"), _ => "gpu-session", "m", null);
        Assert.Equal("gpu-session", session);
        Assert.Same(gpu, bundle);
        Assert.Null(reason);
    }

    [Fact]
    public void OpenWithCpuFallback_Throws_WhenTheCpuItselfCannotLoad()
    {
        var cpu = new OnnxSessionFactory.SessionBundle(new SessionOptions(), OnnxExecutionProvider.Cpu, "CPU");
        var ex = Assert.Throws<InvalidDataException>(() => OnnxStemSeparator.OpenWithCpuFallback<string>(cpu,
            () => throw new InvalidOperationException("no second CPU attempt"),
            _ => throw new InvalidDataException("corrupt model"), "m", null));
        Assert.Equal("corrupt model", ex.Message);
        Assert.True(cpu.Options.IsClosed);
    }

    [Fact]
    public void OpenWithCpuFallback_Throws_WhenTheCpuRetryFailsToo()
    {
        var gpu = new OnnxSessionFactory.SessionBundle(new SessionOptions(), OnnxExecutionProvider.DirectML, "DirectML");
        var cpu = new OnnxSessionFactory.SessionBundle(new SessionOptions(), OnnxExecutionProvider.Cpu, "CPU");
        Assert.Throws<InvalidDataException>(() => OnnxStemSeparator.OpenWithCpuFallback<string>(gpu, () => cpu,
            _ => throw new InvalidDataException("corrupt model"), "m", null));
        Assert.True(gpu.Options.IsClosed);
        Assert.True(cpu.Options.IsClosed);
    }

    /// <summary>
    /// DirectML against the CPU on the real htdemucs model, on the adapter with the most
    /// video memory. Skips without the model (<c>%APPDATA%\DotCharter\models</c>, read
    /// only) or without a hardware DirectX 12 adapter. Fusion on gave a relative error of
    /// 35-65; fusion off gives ~5e-5.
    /// </summary>
    [OnnxResourceFact("htdemucs_6s.onnx")]
    [Trait("Category", "Gpu")]
    public async Task Htdemucs_OnDirectMl_MatchesTheCpu()
    {
        var adapter = OnnxGpuAdapters.Enumerate()
            .Where(a => !a.IsSoftware)
            .OrderByDescending(a => a.DedicatedVideoMemoryBytes)
            .FirstOrDefault();
        if (adapter is not { DedicatedVideoMemoryBytes: >= 3UL << 30 })
        {
            Assert.Skip("No hardware DirectX 12 adapter with 3 GB of video memory.");
            return;
        }

        var audio = SyntheticMix(seconds: 10, sampleRate: 44100);
        var model = OnnxStemSeparatorIntegrationTests.ResolveModel("htdemucs_6s.onnx");

        IReadOnlyList<OnnxStemBuffer> reference;
        using (var cpu = new OnnxStemSeparator(model, OnnxStemModelKind.Htdemucs, OnnxExecutionProvider.Cpu))
            reference = await cpu.SeparateAsync(audio, 44100, ct: TestContext.Current.CancellationToken);

        using var dml = new OnnxStemSeparator(model, OnnxStemModelKind.Htdemucs, OnnxExecutionProvider.DirectML,
            deviceId: adapter.DeviceId);
        Assert.Equal(OnnxExecutionProvider.DirectML, dml.ActiveExecutionProvider);
        Assert.Null(dml.FallbackReason);
        var stems = await dml.SeparateAsync(audio, 44100, ct: TestContext.Current.CancellationToken);

        Assert.Equal(reference.Count, stems.Count);
        for (var i = 0; i < stems.Count; i++)
        {
            var error = RelativeError(reference[i].InterleavedStereo, stems[i].InterleavedStereo);
            Assert.True(error < 1e-2,
                $"stem '{stems[i].Name}' on DirectML ({adapter.Name}) is off the CPU by {error:0.####} (relative L2)");
        }
    }

    private static float[] SyntheticMix(int seconds, int sampleRate)
    {
        var random = new Random(151);
        var frames = seconds * sampleRate;
        var mix = new float[frames * 2];
        for (var n = 0; n < frames; n++)
        {
            var t = n / (double)sampleRate;
            var beat = (n % (sampleRate / 2)) < 2000 ? random.NextDouble() - 0.5 : 0; // a click track
            var tone = 0.2 * Math.Sin(2 * Math.PI * 110 * t) + 0.1 * Math.Sin(2 * Math.PI * 440 * t * (1 + 0.01 * Math.Sin(t)));
            mix[2 * n] = (float)(tone + 0.3 * beat);
            mix[2 * n + 1] = (float)(0.9 * tone + 0.3 * beat);
        }

        return mix;
    }

    private static double RelativeError(float[] expected, float[] actual)
    {
        Assert.Equal(expected.Length, actual.Length);
        double diff = 0, norm = 0;
        for (var i = 0; i < expected.Length; i++)
        {
            var d = (double)expected[i] - actual[i];
            diff += d * d;
            norm += (double)expected[i] * expected[i];
        }

        return Math.Sqrt(diff / Math.Max(norm, 1e-20));
    }
}
