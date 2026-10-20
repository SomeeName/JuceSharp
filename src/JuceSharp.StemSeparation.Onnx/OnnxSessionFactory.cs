using Microsoft.ML.OnnxRuntime;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Resolves an <see cref="SessionOptions"/> with the best available execution
/// provider for the user's preference. Returns the actual EP that was loaded
/// so the UI can show the user "DirectML active" vs "fell back to CPU".
///
/// Order of preference for <see cref="OnnxExecutionProvider.Auto"/>:
/// DirectML → CUDA → CPU (<see cref="FallbackOrder"/>). The CPU EP is always
/// built in to onnxruntime so it's the final fallback. This package ships the
/// DirectML build of onnxruntime, which has no CUDA provider; the CUDA arm stays
/// for a consumer that swaps in the Gpu build, and is skipped (with a logged
/// reason) here.
/// </summary>
public sealed class OnnxSessionFactory
{
    private readonly Action<string>? _log;
    /// <summary>Create a factory, optionally with a log sink so the provider probe and any
    /// fallback are visible. No ONNX Runtime work happens here.</summary>
    /// <param name="log">Receives one line per probe result and per fallback decision.</param>
    public OnnxSessionFactory(Action<string>? log = null) { _log = log; }

    /// <summary>The session options to build an <c>InferenceSession</c> with, plus which
    /// execution provider actually loaded — which is not necessarily the one that was
    /// requested, since every provider falls back to CPU.</summary>
    /// <param name="Options">Configured ONNX Runtime session options; the caller owns and
    /// disposes them.</param>
    /// <param name="Active">The provider that actually loaded.</param>
    /// <param name="Description">One line fit to show a user, e.g. "DirectML active" or
    /// "fell back to CPU".</param>
    /// <param name="FallbackReason">Why the requested provider did not attach, when the
    /// bundle landed on a different one (an explicit request that degraded, or
    /// <see cref="OnnxExecutionProvider.Auto"/> that found no GPU provider at all);
    /// <c>null</c> when the request was honoured.</param>
    public sealed record SessionBundle(SessionOptions Options, OnnxExecutionProvider Active, string Description,
        string? FallbackReason = null);

    /// <summary>
    /// Providers actually compiled into the onnxruntime.dll that ended up next
    /// to our binary. The Gpu and DirectML NuGet packages each ship their own
    /// onnxruntime.dll and cannot coexist — whichever wins the file-copy race
    /// dictates which EPs are available. We probe this once so explicit
    /// requests can fail fast with a useful log message instead of silently
    /// falling back.
    /// </summary>
    /// <remarks>
    /// <b>Lazy, and guarded.</b> This was a static field initialiser, so
    /// <c>OrtEnv.Instance()</c> ran during type initialisation — which loads
    /// <c>onnxruntime.dll</c>. On a machine where that native library is missing
    /// or the wrong architecture, the user got a
    /// <c>TypeInitializationException</c> wrapping a
    /// <c>TypeInitializationException</c> wrapping the real
    /// <c>DllNotFoundException</c>, from whichever member of this class they
    /// happened to touch first. Probing on demand, in a try/catch, turns that
    /// into an empty provider list and one clear log line — and every caller
    /// below already handles "no providers" by degrading.
    /// </remarks>
    public static IReadOnlyCollection<string> AvailableNativeProviders => ProbeNativeProviders();

    /// <summary>
    /// The probe's two halves, resolved TOGETHER exactly once.
    /// <para>Two separate statics let a thread observe an empty provider list
    /// with <c>_probeFailure</c> still null, so <see cref="NativeRuntimeFailure"/>
    /// could answer "no providers, and no reason" — which is precisely the
    /// distinction that property exists to make (W4-S20).
    /// <c>ExecutionAndPublication</c> means the second caller waits for the
    /// first's result rather than racing it.</para>
    /// </summary>
    private static readonly Lazy<(IReadOnlyCollection<string> Providers, string? Failure)> Probe =
        new(static () =>
        {
            try
            {
                return (OrtEnv.Instance().GetAvailableProviders(), (string?)null);
            }
            catch (Exception ex)
            {
                return ((IReadOnlyCollection<string>)Array.Empty<string>(),
                    "ONNX Runtime could not be initialised — onnxruntime.dll is missing, is the wrong " +
                    $"architecture, or its dependencies are absent ({ex.GetType().Name}: {ex.Message}). " +
                    "Stem separation will report no execution providers.");
            }
        }, LazyThreadSafetyMode.ExecutionAndPublication);

    private static IReadOnlyCollection<string> ProbeNativeProviders() => Probe.Value.Providers;

    /// <summary>The reason the provider probe failed, or <c>null</c> if it did
    /// not. Surfaced so "no GPU" and "no ONNX Runtime at all" are
    /// distinguishable.</summary>
    public static string? NativeRuntimeFailure
    {
        get { return Probe.Value.Failure; }
    }

    private static bool HasNativeProvider(string name)
        => AvailableNativeProviders.Any(p => string.Equals(p, name, StringComparison.OrdinalIgnoreCase));

    /// <param name="preferred">Requested execution provider (falls back down the chain).</param>
    /// <param name="intraOpNumThreads">Cap on the CPU EP intra-op thread pool.
    /// 0 (default) lets ONNX Runtime use every core; a positive value keeps stem
    /// separation from saturating the machine and starving the UI / audio thread.</param>
    /// <param name="deviceId">The GPU to run on: for DirectML the DXGI adapter index
    /// (<see cref="OnnxGpuAdapters.Enumerate"/>), for CUDA the CUDA device ordinal. Ignored
    /// by the CPU provider. A negative value is treated as 0.</param>
    /// <param name="directMlGraphFusion">Leave DirectML's graph fusion ON. Only for a model
    /// verified correct with it (<see cref="DirectMlGraphFusionKey"/>); every other session
    /// gets <see cref="DirectMlSessionConfig"/>. Ignored by the other providers.</param>
    /// <returns>The configured options, the provider that actually attached and, when that
    /// is not the one asked for, <see cref="SessionBundle.FallbackReason"/>.</returns>
    public SessionBundle Create(OnnxExecutionProvider preferred, int intraOpNumThreads = 0, int deviceId = 0,
        bool directMlGraphFusion = false)
    {
        deviceId = Math.Max(0, deviceId);
        var opts = NewOptions(intraOpNumThreads);
        string? firstFailure = null;

        foreach (var ep in FallbackOrder(preferred))
        {
            try
            {
                switch (ep)
                {
                    case OnnxExecutionProvider.DirectML:
                        if (!HasNativeProvider("DmlExecutionProvider"))
                        {
                            throw new InvalidOperationException(
                                "DirectML is not in this onnxruntime.dll " +
                                $"(available: {string.Join(", ", AvailableNativeProviders)}).");
                        }

                        ApplyDirectMlOptions(opts, directMlGraphFusion);
                        opts.AppendExecutionProvider_DML(deviceId);
                        _log?.Invoke($"ONNX session: DirectML execution provider attached (deviceId={deviceId}, " +
                            $"graph fusion {(directMlGraphFusion ? "on: the model is verified with it" : "off")}).");
                        return new SessionBundle(opts, OnnxExecutionProvider.DirectML, "DirectML (any DX12 GPU)",
                            FallbackNote(preferred, ep, firstFailure));
                    case OnnxExecutionProvider.Cuda:
                        if (!HasNativeProvider("CUDAExecutionProvider"))
                        {
                            throw new InvalidOperationException(
                                "CUDA is not in this onnxruntime.dll " +
                                $"(available: {string.Join(", ", AvailableNativeProviders)}).");
                        }
                        // Tuned CUDA provider options for stem-separation models:
                        //   - cudnn_conv_algo_search=EXHAUSTIVE makes the first call slower
                        //     but lets cuDNN pick the fastest kernel for subsequent
                        //     identically-shaped runs (every chunk has the same shape, so
                        //     this is essentially free after one warmup).
                        //   - do_copy_in_default_stream=1 keeps H2D/D2H copies on the
                        //     compute stream so they pipeline with the next kernel.
                        //   - arena_extend_strategy=kSameAsRequested avoids grabbing 4 GB
                        //     of VRAM up front on cards with small GPU memory.
                        // OrtCUDAProviderOptions is a SafeHandle; disposing it after the
                        // append is correct (SessionOptionsAppendExecutionProvider_CUDA_V2
                        // COPIES the options) and is what ORT's own C# samples do.
                        {
                            using var cudaOpts = new OrtCUDAProviderOptions();
                            cudaOpts.UpdateOptions(new Dictionary<string, string>
                            {
                                ["device_id"]                  = deviceId.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                ["cudnn_conv_algo_search"]     = "EXHAUSTIVE",
                                ["do_copy_in_default_stream"]  = "1",
                                ["arena_extend_strategy"]      = "kSameAsRequested",
                            });
                            opts.AppendExecutionProvider_CUDA(cudaOpts);
                        }
                        _log?.Invoke($"ONNX session: CUDA execution provider attached (deviceId={deviceId}, cudnn=EXHAUSTIVE).");
                        return new SessionBundle(opts, OnnxExecutionProvider.Cuda, "CUDA (NVIDIA)",
                            FallbackNote(preferred, ep, firstFailure));
                    case OnnxExecutionProvider.Cpu:
                        opts.AppendExecutionProvider_CPU();
                        _log?.Invoke("ONNX session: CPU execution provider attached.");
                        return new SessionBundle(opts, OnnxExecutionProvider.Cpu, "CPU",
                            FallbackNote(preferred, ep, firstFailure));
                }
            }
            catch (Exception ex)
            {
                firstFailure ??= $"{ep}: {ex.Message}";
                _log?.Invoke($"ONNX session: {ep} execution provider not available ({ex.Message}). Falling back.");
                // Recreate opts — appending an EP and failing can leave the
                // SessionOptions in a half-configured state in some ORT versions.
                opts.Dispose();
                opts = NewOptions(intraOpNumThreads);
            }
        }

        // Unreachable: AppendExecutionProvider_CPU always succeeds with the
        // bundled CPU EP, but be defensive.
        return new SessionBundle(opts, OnnxExecutionProvider.Cpu, "CPU (default)", firstFailure);
    }

    /// <summary>The order the factory tries providers in for a preference. Pure, so the
    /// fallback policy is testable without a GPU: every chain ends at the CPU, and
    /// <see cref="OnnxExecutionProvider.Auto"/> tries DirectML first, then CUDA.</summary>
    /// <param name="preferred">The requested provider.</param>
    /// <returns>The providers to attempt, first to last.</returns>
    public static IReadOnlyList<OnnxExecutionProvider> FallbackOrder(OnnxExecutionProvider preferred) => preferred switch
    {
        OnnxExecutionProvider.DirectML => [OnnxExecutionProvider.DirectML, OnnxExecutionProvider.Cuda, OnnxExecutionProvider.Cpu],
        OnnxExecutionProvider.Cuda     => [OnnxExecutionProvider.Cuda, OnnxExecutionProvider.DirectML, OnnxExecutionProvider.Cpu],
        OnnxExecutionProvider.Cpu      => [OnnxExecutionProvider.Cpu],
        _                              => [OnnxExecutionProvider.DirectML, OnnxExecutionProvider.Cuda, OnnxExecutionProvider.Cpu],
    };

    /// <summary>The reason to surface when the attached provider is not the requested one.
    /// <see cref="OnnxExecutionProvider.Auto"/> asks for "the best GPU provider there is",
    /// so landing on its second GPU choice is not a fallback; landing on the CPU is.</summary>
    /// <param name="preferred">The requested provider.</param>
    /// <param name="active">The provider that attached.</param>
    /// <param name="firstFailure">The first provider failure seen, or <c>null</c>.</param>
    /// <returns>The reason, or <c>null</c> when the request counts as honoured.</returns>
    public static string? FallbackNote(OnnxExecutionProvider preferred, OnnxExecutionProvider active, string? firstFailure)
    {
        if (firstFailure == null || preferred == active) return null;
        if (preferred == OnnxExecutionProvider.Auto && active != OnnxExecutionProvider.Cpu) return null;
        return firstFailure;
    }

    /// <summary>
    /// Session configuration entries every DirectML session gets, as (key, value).
    /// <c>ep.dml.disable_graph_fusion=1</c> is a CORRECTNESS setting, not a tuning one:
    /// with DirectML's own graph fusion on (ONNX Runtime 1.24.4, DirectML 1.15.4), the
    /// htdemucs export returns stems 35-65x louder than the CPU's — deterministic garbage,
    /// on every adapter tried, at every ORT optimisation level. With fusion off the output
    /// matches the CPU to a relative error of about 5e-5. Measured 2026-10-05 on an
    /// RTX 5070 Ti and an RTX 3080; pinned by the GPU-gated parity test.
    /// <para>
    /// BS-RoFormer SW needs it too, and it is global for that reason rather than per
    /// model: with fusion ON its six output spectrograms are garbage (relative L2 against
    /// the PyTorch reference 0.79 for "other" and 420 to 2,400 for the other five stems);
    /// with fusion OFF they are within 6.6e-6 at both ORT_ENABLE_ALL and ORT_ENABLE_BASIC
    /// (RTX 5070 Ti, 2026-10-05, <c>BsRoformerSwDirectMlTests</c>).
    /// </para>
    /// </summary>
    public static IReadOnlyList<KeyValuePair<string, string>> DirectMlSessionConfig { get; } =
    [
        new("ep.dml.disable_graph_fusion", "1"),
    ];

    /// <summary>
    /// The model metadata key, and <see cref="DirectMlGraphFusionVerified"/> its value, by which
    /// an export declares that it was checked against its PyTorch reference WITH DirectML's
    /// graph fusion on, so <see cref="OnnxStemSeparator"/> leaves fusion on for it.
    /// <para>
    /// Fusion is worth having where it is correct: BS-RoFormer SW's training-chunk export
    /// (#267) runs 10-12 % faster with it and peaks at ~5 GB of video memory instead of ~8.
    /// That export replaces the per-band mask estimators' 62-way <c>Concat</c> - the one
    /// node the fused graph miscomputes (located by exposing every intermediate: everything
    /// before it matches, relative L2 1.2 at it) - with seven batched groups. A model without
    /// the key keeps fusion off.
    /// </para>
    /// </summary>
    public const string DirectMlGraphFusionKey = "dml_graph_fusion";

    /// <summary>The value of <see cref="DirectMlGraphFusionKey"/> that turns fusion on.</summary>
    public const string DirectMlGraphFusionVerified = "verified";

    /// <summary>Whether a model's metadata declares it verified with DirectML graph fusion.</summary>
    /// <param name="metadata">The model's custom metadata.</param>
    /// <returns><c>true</c> only for exactly <see cref="DirectMlGraphFusionVerified"/>.</returns>
    public static bool AllowsDirectMlGraphFusion(IReadOnlyDictionary<string, string> metadata)
    {
        ArgumentNullException.ThrowIfNull(metadata);
        return metadata.TryGetValue(DirectMlGraphFusionKey, out var value) &&
               string.Equals(value.Trim(), DirectMlGraphFusionVerified, StringComparison.Ordinal);
    }

    /// <summary>
    /// What a session needs before <c>AppendExecutionProvider_DML</c>: memory-pattern
    /// planning and parallel execution off (the DirectML provider supports neither, and
    /// ONNX Runtime's DirectML documentation requires both off), plus every entry of
    /// <see cref="DirectMlSessionConfig"/>.
    /// </summary>
    /// <param name="options">The options to configure; modified in place.</param>
    /// <param name="allowGraphFusion">Skip <see cref="DirectMlSessionConfig"/> and leave
    /// DirectML's graph fusion on: only for a model verified with it
    /// (<see cref="AllowsDirectMlGraphFusion"/>).</param>
    public static void ApplyDirectMlOptions(SessionOptions options, bool allowGraphFusion = false)
    {
        ArgumentNullException.ThrowIfNull(options);
        options.EnableMemoryPattern = false;
        options.ExecutionMode = ExecutionMode.ORT_SEQUENTIAL;
        if (allowGraphFusion) return;
        foreach (var (key, value) in DirectMlSessionConfig)
            options.AddSessionConfigEntry(key, value);
    }

    private static SessionOptions NewOptions(int intraOpNumThreads) => new()
    {
        GraphOptimizationLevel = GraphOptimizationLevel.ORT_ENABLE_ALL,
        EnableMemoryPattern    = true,
        LogSeverityLevel       = OrtLoggingLevel.ORT_LOGGING_LEVEL_WARNING,
        IntraOpNumThreads      = intraOpNumThreads > 0 ? intraOpNumThreads : 0,
    };
}
