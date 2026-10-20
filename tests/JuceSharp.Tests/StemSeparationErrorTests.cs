using JuceSharp.StemSeparation.Processing;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Unit tests for <see cref="StemSeparationError"/>. We assert on the
/// public <see cref="StemSeparationException"/> subtype the classifier
/// produces, since that's the contract surface third-party consumers
/// (DotCharter, external library users) program against.
/// </summary>
public class StemSeparationErrorTests
{
    [Theory]
    [InlineData("RuntimeError: CUDA out of memory. Tried to allocate 4.00 GiB")]
    [InlineData("torch.cuda.OutOfMemoryError: CUDA out of memory.")]
    [InlineData("HIP out of memory. Tried to allocate 2.00 GiB")]
    [InlineData("DML_OUT_OF_MEMORY: failed to allocate buffer")]
    [InlineData("RuntimeError: DirectML allocator: insufficient memory")]
    [InlineData("MPS backend out of memory (MPS allocated: 5.21 GB)")]
    [InlineData("RuntimeError: out of memory")]
    [InlineData("CUBLAS_STATUS_ALLOC_FAILED when calling cublasCreate")]
    public void Vram_signatures_map_to_StemSeparationVramException(string stderr)
    {
        var ex = StemSeparationError.Build(stderr, "", model: "htdemucs", device: "cuda");
        var vram = Assert.IsType<StemSeparationVramException>(ex);
        Assert.Equal(StemSeparationErrorKind.VramExhausted, vram.Kind);
        Assert.Equal("htdemucs", vram.Model);
        Assert.Equal("cuda", vram.Device);
        Assert.Contains("VRAM", vram.Message);
        Assert.Contains("cpu", vram.Message);
    }

    [Theory]
    [InlineData("CUDA driver version is insufficient for CUDA runtime version")]
    [InlineData("cuDNN error: CUDNN_STATUS_VERSION_MISMATCH")]
    [InlineData("CUDA error: invalid device function")]
    [InlineData("forward compatibility was attempted on non supported HW")]
    public void Cuda_driver_mismatch_signatures(string stderr)
    {
        var ex = StemSeparationError.Build(stderr, "", "htdemucs_ft", "cuda:0");
        var cuda = Assert.IsType<StemSeparationCudaDriverException>(ex);
        Assert.Equal(StemSeparationErrorKind.CudaDriverMismatch, cuda.Kind);
        Assert.Equal("htdemucs_ft", cuda.Model);
    }

    [Theory]
    [InlineData("RuntimeError: no CUDA GPUs are available")]
    [InlineData("AssertionError: Torch not compiled with CUDA enabled")]
    [InlineData("Torch not compiled with CUDA")]
    public void Cuda_unavailable_signatures(string stderr)
    {
        var ex = StemSeparationError.Build(stderr, "", "bs_roformer", "cuda");
        Assert.IsType<StemSeparationCudaUnavailableException>(ex);
        Assert.Equal(StemSeparationErrorKind.CudaUnavailable, ex.Kind);
    }

    [Theory]
    [InlineData("ModuleNotFoundError: No module named 'demucs'")]
    [InlineData("ImportError: cannot import name 'apply_model' from 'demucs.apply'")]
    public void Missing_package_signatures(string stderr)
    {
        var ex = StemSeparationError.Build(stderr, "", "htdemucs", "cpu");
        Assert.IsType<StemSeparationMissingPackageException>(ex);
    }

    [Theory]
    [InlineData("FileNotFoundError: checkpoint not found at /weights/xx.ckpt")]
    [InlineData("RuntimeError: checkpoint is corrupt")]
    [InlineData("safetensors invalid header")]
    public void Model_load_failure_signatures(string stderr)
    {
        var ex = StemSeparationError.Build(stderr, "", "roformer-sw", "cuda");
        var ml = Assert.IsType<StemSeparationModelLoadException>(ex);
        Assert.Equal("roformer-sw", ml.Model);
    }

    [Theory]
    [InlineData("soundfile.LibsndfileError: Format not recognised.")]
    [InlineData("Error opening 'broken.mp3': format not recognised")]
    public void Audio_format_signatures(string stderr)
    {
        var ex = StemSeparationError.Build(stderr, "", "htdemucs", "cpu");
        Assert.IsType<StemSeparationAudioFormatException>(ex);
    }

    [Fact]
    public void Unknown_signatures_fall_back_to_base_exception()
    {
        var ex = StemSeparationError.Build("totally unrelated traceback", "", "htdemucs", "cpu");
        Assert.Equal(typeof(StemSeparationException), ex.GetType()); // base, NOT a subtype
        Assert.Equal(StemSeparationErrorKind.Unknown, ex.Kind);
    }

    [Fact]
    public void Stderr_is_truncated_to_tail_for_long_blobs()
    {
        var huge = new string('x', 4000) + "\nCUDA out of memory";
        var ex = StemSeparationError.Build(huge, "", "htdemucs", "cuda");
        Assert.IsType<StemSeparationVramException>(ex);
        // tail comes from the *end* of the blob so the actual error line survives
        Assert.Contains("CUDA out of memory", ex.Message);
        Assert.StartsWith("…", ex.Message[(ex.Message.IndexOf("stderr tail: ", StringComparison.Ordinal) + "stderr tail: ".Length)..]);
    }

    [Fact]
    public void Classifier_searches_both_stderr_and_stdout()
    {
        // Some scripts print the OOM line to stdout (broken logging config) — must still trigger.
        var ex = StemSeparationError.Build(stderr: "", stdout: "RuntimeError: CUDA out of memory",
                                           model: "htdemucs", device: "cuda");
        Assert.IsType<StemSeparationVramException>(ex);
    }

    [Fact]
    public void Classifier_is_case_insensitive()
    {
        var ex = StemSeparationError.Build("Cuda Out Of Memory", "", "x", "cuda");
        Assert.Equal(StemSeparationErrorKind.VramExhausted, ex.Kind);
    }

    [Fact]
    public void Null_inputs_are_treated_as_empty_and_return_Unknown()
    {
        var ex = StemSeparationError.Build(null!, null!, "x", "cpu");
        Assert.Equal(StemSeparationErrorKind.Unknown, ex.Kind);
        Assert.NotNull(ex.Stderr);
    }

    [Fact]
    public void Vram_exception_message_lists_all_remediation_steps()
    {
        var ex = new StemSeparationVramException("htdemucs_ft", "cuda", "tail");
        Assert.Contains("cpu", ex.Message);
        Assert.Contains("Overlap", ex.Message);
        Assert.Contains("Shifts", ex.Message);
        Assert.Contains("smaller model", ex.Message);
    }
}
