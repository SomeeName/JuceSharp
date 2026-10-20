using System.Text.RegularExpressions;

namespace JuceSharp.StemSeparation.Processing;

/// <summary>
/// Categorises a Python / PyTorch failure surfaced by the bundled
/// <c>separate_track.py</c> sidecar. Used by <see cref="StemSeparationError"/>
/// to drive friendly, actionable C# exception types instead of dumping a
/// raw stderr blob on the user.
/// </summary>
public enum StemSeparationErrorKind
{
    /// <summary>The classifier saw no signature it recognises.</summary>
    Unknown,
    /// <summary>CUDA / DirectML / MPS reported insufficient VRAM. Caller
    /// should retry on CPU or lower <see cref="StemSeparationOptions.Overlap"/> /
    /// <see cref="StemSeparationOptions.Shifts"/>.</summary>
    VramExhausted,
    /// <summary>CUDA toolkit / driver mismatch — the installed torch was
    /// built against a different CUDA version than the system driver.</summary>
    CudaDriverMismatch,
    /// <summary>No CUDA-capable device visible to torch.</summary>
    CudaUnavailable,
    /// <summary>A required pip package (torch, demucs, bs_roformer …) was
    /// missing or broken — usually a half-installed embed.</summary>
    MissingPackage,
    /// <summary>The model checkpoint file could not be located or was
    /// corrupted.</summary>
    ModelLoadFailure,
    /// <summary>The input audio file is unsupported or unreadable.</summary>
    AudioFormatUnsupported,
}

/// <summary>
/// Inspects captured stderr (and stray stdout) from the bundled Python
/// sidecar and classifies the failure into a <see cref="StemSeparationErrorKind"/>.
/// All matching is case-insensitive and works against substrings — the
/// patterns are deliberately broad because torch/demucs/bs_roformer all
/// raise slightly different wording for the same condition.
///
/// <para>Recognised signature strings:</para>
/// <list type="bullet">
///   <item><c>CUDA out of memory</c> — canonical torch CUDA OOM</item>
///   <item><c>torch.cuda.OutOfMemoryError</c> — newer torch type name</item>
///   <item><c>HIP out of memory</c> — ROCm/AMD path</item>
///   <item><c>DML_OUT_OF_MEMORY</c> / <c>DirectML allocator</c> — DirectML provider</item>
///   <item><c>MPS backend out of memory</c> — Apple silicon</item>
///   <item><c>RuntimeError: out of memory</c> — generic torch raise</item>
///   <item><c>CUDA driver version is insufficient</c> — mismatch</item>
///   <item><c>no CUDA GPUs are available</c> / <c>Torch not compiled with CUDA</c></item>
///   <item><c>ModuleNotFoundError</c> / <c>ImportError</c></item>
///   <item><c>checkpoint</c> + <c>not found</c> / <c>corrupt</c></item>
///   <item><c>soundfile</c> + (<c>format not recognised</c> or <c>could not open</c>)</item>
/// </list>
/// </summary>
internal static class StemSeparationError
{
    private static readonly (StemSeparationErrorKind Kind, Regex Pattern)[] Rules =
    {
        (StemSeparationErrorKind.VramExhausted, new Regex(
            @"CUDA out of memory|torch\.cuda\.OutOfMemoryError|HIP out of memory|DML_OUT_OF_MEMORY|DirectML allocator|MPS backend out of memory|RuntimeError:\s*out of memory|CUBLAS_STATUS_ALLOC_FAILED",
            RegexOptions.IgnoreCase | RegexOptions.Compiled)),

        (StemSeparationErrorKind.CudaDriverMismatch, new Regex(
            @"CUDA driver version is insufficient|cuDNN error|CUDA error:\s*invalid device|forward compatibility was attempted",
            RegexOptions.IgnoreCase | RegexOptions.Compiled)),

        (StemSeparationErrorKind.CudaUnavailable, new Regex(
            @"no CUDA GPUs are available|Torch not compiled with CUDA|AssertionError:\s*Torch not compiled",
            RegexOptions.IgnoreCase | RegexOptions.Compiled)),

        (StemSeparationErrorKind.MissingPackage, new Regex(
            @"ModuleNotFoundError|ImportError:\s|No module named",
            RegexOptions.IgnoreCase | RegexOptions.Compiled)),

        (StemSeparationErrorKind.ModelLoadFailure, new Regex(
            @"checkpoint.*(not found|missing|corrupt)|safetensors.*invalid|state_dict.*unexpected",
            RegexOptions.IgnoreCase | RegexOptions.Compiled)),

        (StemSeparationErrorKind.AudioFormatUnsupported, new Regex(
            @"soundfile.*(format not recognised|could not open)|LibsndfileError|Error opening",
            RegexOptions.IgnoreCase | RegexOptions.Compiled)),
    };

    /// <summary>Classify a (stdout, stderr) pair into an exception kind.</summary>
    public static StemSeparationErrorKind Classify(string? stderr, string? stdout = "")
    {
        var haystack = (stderr ?? string.Empty) + "\n" + (stdout ?? string.Empty);
        foreach (var (kind, pattern) in Rules)
            if (pattern.IsMatch(haystack)) return kind;
        return StemSeparationErrorKind.Unknown;
    }

    /// <summary>
    /// Build the concrete <see cref="StemSeparationException"/> subtype
    /// most accurately representing the captured failure. Returns the
    /// generic base type when nothing matches so callers always get a
    /// strongly typed exception they can catch.
    /// </summary>
    public static StemSeparationException Build(
        string? stderr, string? stdout, string model, string device)
    {
        var kind = Classify(stderr, stdout);
        var tail = Tail(stderr, 800);
        return kind switch
        {
            StemSeparationErrorKind.VramExhausted => new StemSeparationVramException(model, device, tail),
            StemSeparationErrorKind.CudaDriverMismatch => new StemSeparationCudaDriverException(model, device, tail),
            StemSeparationErrorKind.CudaUnavailable => new StemSeparationCudaUnavailableException(model, device, tail),
            StemSeparationErrorKind.MissingPackage => new StemSeparationMissingPackageException(tail),
            StemSeparationErrorKind.ModelLoadFailure => new StemSeparationModelLoadException(model, tail),
            StemSeparationErrorKind.AudioFormatUnsupported => new StemSeparationAudioFormatException(tail),
            _ => new StemSeparationException(
                $"Stem separation failed (model='{model}', device='{device}'). stderr tail: {tail}",
                StemSeparationErrorKind.Unknown, stderr ?? string.Empty),
        };
    }

    private static string Tail(string? s, int max)
    {
        if (string.IsNullOrEmpty(s)) return string.Empty;
        return s.Length <= max ? s : "…" + s[^max..];
    }
}

/// <summary>Base type for every strongly-typed separation failure.</summary>
public class StemSeparationException : Exception
{
    /// <summary>Which classified failure this is, decided once by <c>Classify</c> from the
    /// child process's stderr. Branch on this rather than on the message text.</summary>
    public StemSeparationErrorKind Kind { get; }
    /// <summary>The captured stderr — useful for bug reports / logs. NOTE that
    /// <c>Build</c> passes only the last 800 characters for every
    /// CLASSIFIED kind (the tail carries the traceback's cause, and a GPU log is
    /// megabytes); the <c>Unknown</c> fall-through keeps it whole. Whatever
    /// arrives here is stored verbatim.</summary>
    public string Stderr { get; }

    /// <summary>Construct a separation failure with an already-classified kind. Prefer
    /// <c>Build</c>, which classifies the captured output for you and picks the right
    /// derived type.</summary>
    /// <param name="message">Human-readable description, ideally naming the remedy.</param>
    /// <param name="kind">The classified failure kind.</param>
    /// <param name="stderr">Full captured stderr, kept verbatim for bug reports.</param>
    public StemSeparationException(string message, StemSeparationErrorKind kind, string stderr)
        : base(message)
    {
        Kind = kind;
        Stderr = stderr;
    }
}

/// <summary>
/// The GPU ran out of video memory while loading the model or processing
/// a chunk. The model and device strings are surfaced verbatim so the UI
/// can show "Demucs htdemucs_ft ran out of VRAM on cuda".
///
/// <para>Recommended remediation, in order of friendliness:</para>
/// <list type="number">
///   <item>Retry with <see cref="StemSeparationOptions.Device"/> = <c>"cpu"</c>.</item>
///   <item>Lower <see cref="StemSeparationOptions.Overlap"/> (e.g. 0.25 → 0.1).</item>
///   <item>Set <see cref="StemSeparationOptions.Shifts"/> = 0.</item>
///   <item>Pick a smaller model (e.g. <c>htdemucs</c> instead of <c>htdemucs_ft</c>).</item>
/// </list>
/// </summary>
public sealed class StemSeparationVramException : StemSeparationException
{
    /// <summary>The separation model that was loading or running when the GPU ran out —
    /// e.g. <c>htdemucs_ft</c>. Surfaced verbatim so the UI can name it.</summary>
    public string Model { get; }

    /// <summary>The torch device string that was requested, e.g. <c>cuda</c>.</summary>
    public string Device { get; }

    /// <summary>Compose the out-of-VRAM failure, including the remediation order (CPU,
    /// then less overlap, then no shifts, then a smaller model) in the message.</summary>
    /// <param name="model">Model that exhausted VRAM.</param>
    /// <param name="device">Device it was running on.</param>
    /// <param name="stderrTail">Last 800 characters of the child's stderr.</param>
    public StemSeparationVramException(string model, string device, string stderrTail)
        : base($"Out of VRAM running '{model}' on '{device}'. Retry with Device=\"cpu\", " +
               $"reduce Overlap, set Shifts=0, or pick a smaller model. stderr tail: {stderrTail}",
               StemSeparationErrorKind.VramExhausted, stderrTail)
    {
        Model = model;
        Device = device;
    }
}

/// <summary>CUDA / cuDNN driver-toolkit mismatch.</summary>
public sealed class StemSeparationCudaDriverException : StemSeparationException
{
    /// <summary>The separation model that was being run when the driver mismatch surfaced.</summary>
    public string Model { get; }

    /// <summary>The torch device string that was requested, e.g. <c>cuda</c>.</summary>
    public string Device { get; }

    /// <summary>Compose the driver/toolkit-mismatch failure, whose remedy is to update the
    /// NVIDIA driver or reinstall a matching torch wheel.</summary>
    /// <param name="model">Model that was being loaded or run.</param>
    /// <param name="device">Device it was requested on.</param>
    /// <param name="stderrTail">Last 800 characters of the child's stderr.</param>
    public StemSeparationCudaDriverException(string model, string device, string stderrTail)
        : base($"CUDA driver/toolkit mismatch running '{model}' on '{device}'. " +
               $"Update the NVIDIA driver or reinstall a matching torch wheel. stderr tail: {stderrTail}",
               StemSeparationErrorKind.CudaDriverMismatch, stderrTail)
    {
        Model = model;
        Device = device;
    }
}

/// <summary>No CUDA-capable device found (torch built without CUDA, no GPU, or all hidden).</summary>
public sealed class StemSeparationCudaUnavailableException : StemSeparationException
{
    /// <summary>The separation model whose run asked for a CUDA device that is not there.</summary>
    public string Model { get; }

    /// <summary>The torch device string that was requested but could not be provided.</summary>
    public string Device { get; }

    /// <summary>Compose the no-CUDA-device failure, whose remedy is to fall back to
    /// <c>cpu</c> or install a CUDA-enabled torch build.</summary>
    /// <param name="model">Model that was being loaded or run.</param>
    /// <param name="device">Device that was requested.</param>
    /// <param name="stderrTail">Last 800 characters of the child's stderr.</param>
    public StemSeparationCudaUnavailableException(string model, string device, string stderrTail)
        : base($"CUDA requested but unavailable for '{model}' on '{device}'. " +
               $"Set Device=\"cpu\" or install a CUDA-enabled torch build. stderr tail: {stderrTail}",
               StemSeparationErrorKind.CudaUnavailable, stderrTail)
    {
        Model = model;
        Device = device;
    }
}

/// <summary>A required pip package was missing from the embedded interpreter.</summary>
public sealed class StemSeparationMissingPackageException : StemSeparationException
{
    /// <summary>Compose the missing-pip-package failure, whose remedy is to delete the
    /// embed root so the interpreter is re-provisioned from scratch.</summary>
    /// <param name="stderrTail">Last 800 characters of the child's stderr, which names the
    /// missing import.</param>
    public StemSeparationMissingPackageException(string stderrTail)
        : base("Required Python package missing in embedded interpreter. " +
               "Delete the embed root to force a clean re-install. stderr tail: " + stderrTail,
               StemSeparationErrorKind.MissingPackage, stderrTail) { }
}

/// <summary>Model checkpoint / config was missing or corrupted.</summary>
public sealed class StemSeparationModelLoadException : StemSeparationException
{
    /// <summary>The model whose checkpoint or config could not be loaded.</summary>
    public string Model { get; }

    /// <summary>Compose the checkpoint-load failure — missing, corrupted, or built for an
    /// incompatible architecture.</summary>
    /// <param name="model">Model that failed to load.</param>
    /// <param name="stderrTail">Last 800 characters of the child's stderr.</param>
    public StemSeparationModelLoadException(string model, string stderrTail)
        : base($"Failed to load model '{model}'. The checkpoint may be missing, " +
               $"corrupted, or built for an incompatible architecture. stderr tail: {stderrTail}",
               StemSeparationErrorKind.ModelLoadFailure, stderrTail)
    {
        Model = model;
    }
}

/// <summary>The input audio file was unreadable / in an unsupported format.</summary>
public sealed class StemSeparationAudioFormatException : StemSeparationException
{
    /// <summary>Compose the undecodable-input failure, whose remedy is to re-export the
    /// source as 16/24-bit WAV or FLAC.</summary>
    /// <param name="stderrTail">Last 800 characters of the child's stderr.</param>
    public StemSeparationAudioFormatException(string stderrTail)
        : base("Input audio could not be decoded. Re-export as 16/24-bit WAV or FLAC. " +
               "stderr tail: " + stderrTail,
               StemSeparationErrorKind.AudioFormatUnsupported, stderrTail) { }
}
