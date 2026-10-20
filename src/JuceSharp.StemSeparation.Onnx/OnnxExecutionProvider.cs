namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Which ONNX Runtime execution provider the stem-separation backend should
/// try first. <see cref="Auto"/> means the service picks the fastest backend
/// available at runtime (DirectML &gt; CUDA &gt; CPU); the explicit values
/// force a specific EP and fall back to CPU if it can't be loaded.
/// </summary>
public enum OnnxExecutionProvider
{
    /// <summary>Let the factory pick the fastest provider actually compiled into the
    /// onnxruntime.dll next to the binary, falling back to CPU.</summary>
    Auto,
    // ReSharper disable once InconsistentNaming -- "DirectML" is Microsoft's brand name.
    /// <summary>Force DirectML, the vendor-neutral GPU provider on Windows (any DirectX 12
    /// GPU). This package ships the DirectML build of onnxruntime plus DirectML.dll, so it
    /// is also what <see cref="Auto"/> lands on whenever a GPU is present.</summary>
    DirectML,
    /// <summary>Force the CUDA provider; needs an NVIDIA GPU and the CUDA (Gpu) build of
    /// onnxruntime, which this package does NOT ship, so here it falls back to DirectML,
    /// then the CPU.</summary>
    Cuda,
    /// <summary>Force the CPU provider, which is always compiled in and is therefore the
    /// final fallback for every other value.</summary>
    Cpu,
}

/// <summary>Converts between <see cref="OnnxExecutionProvider"/> and the string form used
/// in configuration files and on the command line. Both directions are total: neither
/// throws on an unrecognised value.</summary>
public static class OnnxExecutionProviderParser
{
    /// <summary>Read a configured provider name, case- and whitespace-insensitively, and
    /// accepting the common aliases (<c>dml</c> and <c>gpu</c> for DirectML,
    /// <c>nvidia</c> for CUDA). Anything unrecognised — including <c>null</c> and empty —
    /// becomes <see cref="OnnxExecutionProvider.Auto"/> rather than an error.</summary>
    /// <param name="value">The configured provider name.</param>
    /// <returns>The matching provider, or <see cref="OnnxExecutionProvider.Auto"/>.</returns>
    public static OnnxExecutionProvider Parse(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return OnnxExecutionProvider.Auto;
        return value.Trim().ToLowerInvariant() switch
        {
            "directml" or "dml" or "gpu" => OnnxExecutionProvider.DirectML,
            "cuda" or "nvidia"           => OnnxExecutionProvider.Cuda,
            "cpu"                        => OnnxExecutionProvider.Cpu,
            _                            => OnnxExecutionProvider.Auto,
        };
    }

    /// <summary>Render a provider back to the canonical string
    /// <see cref="Parse"/> accepts, so a round-trip through configuration is stable.</summary>
    /// <param name="ep">The provider to render.</param>
    /// <returns><c>DirectML</c>, <c>Cuda</c>, <c>Cpu</c>, or <c>Auto</c>.</returns>
    public static string ToConfigString(OnnxExecutionProvider ep) => ep switch
    {
        OnnxExecutionProvider.DirectML => "DirectML",
        OnnxExecutionProvider.Cuda     => "Cuda",
        OnnxExecutionProvider.Cpu      => "Cpu",
        _                              => "Auto",
    };
}
