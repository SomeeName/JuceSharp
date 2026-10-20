using JuceSharp.StemSeparation.Onnx;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The stem-separation types a HOST touches but no test did: the
/// <see cref="IStemSeparator"/> contract itself (via a fake, which is the
/// interface's stated reason for existing), <see cref="PythonEnvStatus"/>,
/// <see cref="OnnxExecutionProviderParser"/> and
/// <see cref="SampleRateConverter"/>.
/// </summary>
public sealed class StemSeparationContractCoverageTests
{
    // ------------------------------------------------- IStemSeparator

    /// <summary>
    /// The fake the interface doc-comment promises is possible. If this stops
    /// compiling, the abstraction has grown something only the real
    /// python-sidecar implementation can satisfy.
    /// </summary>
    private sealed class FakeSeparator(bool ready, Func<string, string, SeparationResult>? produce = null)
        : IStemSeparator
    {
        public List<StemSeparationOptions> SeenOptions { get; } = [];

        public bool IsReady() => ready;

        public PythonEnvStatus Probe() => new(ready, ready, ready, ready ? "fake-python" : null, ready ? "3.11.9" : null);

        public Task<SeparationResult> SeparateTrackAsync(
            string inputPath, string outputDir, StemSeparationOptions? options = null,
            IProgress<StemSeparationProgress>? progress = null, CancellationToken ct = default)
        {
            ct.ThrowIfCancellationRequested();
            var opts = options ?? StemSeparationOptions.Default;
            SeenOptions.Add(opts);
            progress?.Report(new StemSeparationProgress("inference", 0.5, "half", null));
            progress?.Report(new StemSeparationProgress("done", 1.0, "done", null));
            return Task.FromResult(produce?.Invoke(inputPath, outputDir)
                ?? new SeparationResult(inputPath, outputDir, opts.Model, opts.Device,
                    [new StemFile("vocals", Path.Combine(outputDir, "vocals.wav"))]));
        }
    }

    [Fact]
    public async Task A_backend_can_be_substituted_through_the_interface_alone()
    {
        IStemSeparator sep = new FakeSeparator(ready: true);

        Assert.True(sep.IsReady());
        var status = sep.Probe();
        Assert.True(status.InterpreterPresent);
        Assert.Equal("fake-python", status.Executable);

        var seen = new List<StemSeparationProgress>();
        var result = await sep.SeparateTrackAsync(
            "song.wav", "out",
            new StemSeparationOptions { Model = "htdemucs_6s", Device = "cpu" },
            new InlineProgress<StemSeparationProgress>(seen.Add),
            TestContext.Current.CancellationToken);

        Assert.Equal("htdemucs_6s", result.Model);
        Assert.Equal("cpu", result.Device);
        Assert.Equal(2, seen.Count);
        Assert.Equal("done", seen[^1].Phase);
    }

    /// <summary>Options are OPTIONAL on the interface, and omitting them must
    /// land on the documented defaults rather than on nulls.</summary>
    [Fact]
    public async Task Omitting_the_options_uses_the_documented_defaults()
    {
        var fake = new FakeSeparator(ready: true);
        await ((IStemSeparator)fake).SeparateTrackAsync("song.wav", "out", ct: TestContext.Current.CancellationToken);

        var opts = Assert.Single(fake.SeenOptions);
        Assert.Same(StemSeparationOptions.Default, opts);
        Assert.Equal("htdemucs", opts.Model);
        Assert.Equal("auto", opts.Device);
        Assert.Equal(1, opts.Shifts);
        Assert.Equal(0.25, opts.Overlap, 6);
        Assert.Equal(TimeSpan.FromHours(4), opts.Timeout);
        Assert.Equal(1, opts.MaxConcurrentSeparations);
        Assert.False(opts.SkipPackageBootstrap);
        Assert.Null(opts.Stems);
    }

    /// <summary>A cancelled run is an <see cref="OperationCanceledException"/>,
    /// never a <see cref="StemSeparationException"/> — otherwise a user Cancel
    /// reads to every consumer as a reason to fall through to the next
    /// backend.</summary>
    [Fact]
    public async Task A_cancelled_run_is_a_cancellation_and_not_a_separation_failure()
    {
        IStemSeparator sep = new FakeSeparator(ready: true);
        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        var ex = await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => sep.SeparateTrackAsync("song.wav", "out", ct: cts.Token));
        Assert.IsNotType<StemSeparationException>(ex);
    }

    [Fact]
    public void A_result_resolves_a_stem_by_name_case_insensitively_and_reports_a_miss_as_null()
    {
        var result = new SeparationResult("in.wav", "out", "htdemucs", "cuda",
        [
            new StemFile("vocals", @"out\vocals.wav"),
            new StemFile("drums", @"out\drums.wav"),
        ]);

        Assert.Equal(@"out\vocals.wav", result.GetStemPath("vocals"));
        Assert.Equal(@"out\vocals.wav", result.GetStemPath("VOCALS"));
        Assert.Equal(@"out\drums.wav", result.GetStemPath("Drums"));
        // A stem the model did not produce is null — NOT the empty string a
        // FirstOrDefault over a struct would otherwise hand back for free.
        Assert.Null(result.GetStemPath("bass"));
        Assert.Null(new SeparationResult("in", "out", "m", "d", []).GetStemPath("vocals"));
    }

    // -------------------------------------------------- PythonEnvStatus

    [Fact]
    public void PythonEnvStatus_carries_its_flags_and_compares_structurally()
    {
        var installed = new PythonEnvStatus(true, true, true, @"C:\x\python.exe", "Python 3.11.9");
        Assert.True(installed.InterpreterPresent);
        Assert.True(installed.HasPip);
        Assert.True(installed.HasTorch);
        Assert.Equal(@"C:\x\python.exe", installed.Executable);
        Assert.Equal("Python 3.11.9", installed.Version);

        Assert.Equal(installed, new PythonEnvStatus(true, true, true, @"C:\x\python.exe", "Python 3.11.9"));
        Assert.NotEqual(installed, installed with { HasTorch = false });

        // The "nothing installed" shape the manager returns for a missing
        // interpreter: no executable, no version — not empty strings, which a
        // caller would print as a blank path.
        var missing = new PythonEnvStatus(false, false, false, null, null);
        Assert.Null(missing.Executable);
        Assert.Null(missing.Version);
        Assert.Equal(default, missing);
    }

    [Fact]
    public void PythonInstallProgress_carries_its_phase_and_fraction()
    {
        var p = new PythonInstallProgress("pip-install", 0.5, "Installing torch…");
        Assert.Equal("pip-install", p.Phase);
        Assert.Equal(0.5, p.Fraction, 6);
        Assert.Equal("Installing torch…", p.Message);
        Assert.Equal(p, new PythonInstallProgress("pip-install", 0.5, "Installing torch…"));
    }

    // --------------------------------------- OnnxExecutionProviderParser

    [Theory]
    [InlineData("directml", OnnxExecutionProvider.DirectML)]
    [InlineData("DirectML", OnnxExecutionProvider.DirectML)]
    [InlineData("  DML  ", OnnxExecutionProvider.DirectML)]
    [InlineData("gpu", OnnxExecutionProvider.DirectML)]
    [InlineData("cuda", OnnxExecutionProvider.Cuda)]
    [InlineData("NVIDIA", OnnxExecutionProvider.Cuda)]
    [InlineData("cpu", OnnxExecutionProvider.Cpu)]
    [InlineData("CPU", OnnxExecutionProvider.Cpu)]
    [InlineData("auto", OnnxExecutionProvider.Auto)]
    // Anything unrecognised is AUTO, never a throw: this parses a persisted
    // user setting, and a settings file from a newer build must not stop the
    // app from starting.
    [InlineData("vulkan", OnnxExecutionProvider.Auto)]
    [InlineData("", OnnxExecutionProvider.Auto)]
    [InlineData("   ", OnnxExecutionProvider.Auto)]
    [InlineData(null, OnnxExecutionProvider.Auto)]
    public void Parse_maps_every_accepted_spelling(string? input, OnnxExecutionProvider expected)
    {
        Assert.Equal(expected, OnnxExecutionProviderParser.Parse(input));
    }

    /// <summary>
    /// The round trip is what makes a persisted setting survive a restart: the
    /// string this writes must parse back to the same value for EVERY enum
    /// member, including any added later.
    /// </summary>
    [Fact]
    public void ToConfigString_round_trips_every_provider()
    {
        foreach (var ep in Enum.GetValues<OnnxExecutionProvider>())
        {
            var text = OnnxExecutionProviderParser.ToConfigString(ep);
            Assert.False(string.IsNullOrWhiteSpace(text));
            Assert.Equal(ep, OnnxExecutionProviderParser.Parse(text));
        }
    }

    // ------------------------------------------------ SampleRateConverter

    [Fact]
    public void Resampling_to_the_same_rate_is_the_identity_and_does_not_copy()
    {
        var input = new float[] { 0.1f, -0.2f, 0.3f, -0.4f };
        Assert.Same(input, SampleRateConverter.Resample(input, 2, 48000, 48000));
    }

    [Theory]
    [InlineData(44100, 48000)]
    [InlineData(48000, 44100)]
    [InlineData(22050, 44100)]
    public void Resampling_produces_the_frame_count_the_ratio_implies(int srcRate, int dstRate)
    {
        const int channels = 2;
        const int srcFrames = 4410;
        var input = new float[srcFrames * channels];

        var output = SampleRateConverter.Resample(input, channels, srcRate, dstRate);

        var expected = (int)Math.Round(srcFrames * (double)dstRate / srcRate);
        Assert.Equal(expected * channels, output.Length);
    }

    /// <summary>
    /// A 1 kHz tone is well inside the passband either way, so the resampler
    /// must preserve its amplitude and not smear it — this is the property a
    /// "good enough for separation" kernel still has to have, and a broken
    /// normalisation shows up here as a level error.
    /// </summary>
    [Theory]
    [InlineData(44100, 48000)]
    [InlineData(48000, 44100)]
    public void A_passband_tone_survives_the_conversion_at_its_original_level(int srcRate, int dstRate)
    {
        const double hz = 1000.0;
        const float amp = 0.5f;
        var srcFrames = srcRate;                       // one second, mono
        var input = new float[srcFrames];
        for (var i = 0; i < srcFrames; i++)
            input[i] = amp * MathF.Sin(2f * MathF.PI * (float)hz * i / srcRate);

        var output = SampleRateConverter.Resample(input, 1, srcRate, dstRate);

        // Ignore the kernel's edges, where the Lanczos window runs off the end.
        var guard = 256;
        var peak = 0f;
        var rmsAcc = 0.0;
        for (var i = guard; i < output.Length - guard; i++)
        {
            peak = MathF.Max(peak, MathF.Abs(output[i]));
            rmsAcc += output[i] * (double)output[i];
        }
        var rms = Math.Sqrt(rmsAcc / (output.Length - 2 * guard));

        Assert.InRange(peak, amp * 0.97f, amp * 1.03f);
        // A sine's RMS is amp / sqrt(2).
        Assert.InRange(rms, amp / Math.Sqrt(2) * 0.97, amp / Math.Sqrt(2) * 1.03);
    }

    [Fact]
    public void Resampling_rejects_a_nonsense_geometry()
    {
        var input = new float[16];
        Assert.Throws<ArgumentOutOfRangeException>(() => SampleRateConverter.Resample(input, 0, 44100, 48000));
        Assert.Throws<ArgumentOutOfRangeException>(() => SampleRateConverter.Resample(input, -1, 44100, 48000));
        Assert.Throws<ArgumentOutOfRangeException>(() => SampleRateConverter.Resample(input, 2, 0, 48000));
        Assert.Throws<ArgumentOutOfRangeException>(() => SampleRateConverter.Resample(input, 2, 44100, 0));
    }

    [Fact]
    public void Resampling_keeps_the_channels_apart()
    {
        // Left is a DC +0.5, right a DC -0.5. Any channel bleed collapses them.
        const int channels = 2;
        const int srcFrames = 2048;
        var input = new float[srcFrames * channels];
        for (var f = 0; f < srcFrames; f++)
        {
            input[f * channels] = 0.5f;
            input[f * channels + 1] = -0.5f;
        }

        var output = SampleRateConverter.Resample(input, channels, 44100, 48000);
        var frames = output.Length / channels;

        for (var f = 128; f < frames - 128; f++)
        {
            Assert.InRange(output[f * channels], 0.49f, 0.51f);
            Assert.InRange(output[f * channels + 1], -0.51f, -0.49f);
        }
    }
}

/// <summary>
/// <see cref="Progress{T}"/> posts to the captured synchronization context, so
/// a test that asserts on what it collected would race the thread pool.
/// Report inline instead.
/// </summary>
file sealed class InlineProgress<T>(Action<T> handler) : IProgress<T>
{
    public void Report(T value) => handler(value);
}

/// <summary>
/// W4-C5. Three paths reached the marshaller unvalidated while every other path
/// argument on this ABI leads with <c>ArgumentException.ThrowIfNullOrWhiteSpace</c>
/// (<c>OpenFile</c>, <c>LoadPlugin</c>, <c>AddNode</c>, <c>GetParam</c>,
/// <c>SetString</c>), so a null <c>modelPath</c> surfaced as a generic
/// <c>JuceAudioException(InvalidArg, "dca_stems_separate_file")</c> naming
/// neither the argument nor the problem.
/// </summary>
public sealed class StemSeparationArgumentValidationTests
{
    [Theory]
    [InlineData(null, "out", "model.bin", "inputAudioPath")]
    [InlineData("", "out", "model.bin", "inputAudioPath")]
    [InlineData("   ", "out", "model.bin", "inputAudioPath")]
    [InlineData("in.wav", null, "model.bin", "outputDirectory")]
    [InlineData("in.wav", "  ", "model.bin", "outputDirectory")]
    [InlineData("in.wav", "out", null, "modelPath")]
    [InlineData("in.wav", "out", "", "modelPath")]
    public void Separate_NamesTheEmptyPathArgument(string? input, string? output, string? model, string expected)
    {
        // No native probe and no Skip: the validation runs AHEAD of the
        // IsSupported capability check on purpose, because an argument is wrong
        // on every build and a caller branching on NotSupportedException must
        // not be told "no backend" about a typo.
        // The token-taking overload (ABI v36) on purpose, and not because the
        // analyzer asked: it is the one that carries the extra guard, so this
        // pins that the argument validation still runs AHEAD of everything the
        // cancellable path added rather than only on the older entry point.
        var ex = Assert.ThrowsAny<ArgumentException>(
            () => JuceStemSeparation.Separate(
                input!, output!, model!, 0, null, TestContext.Current.CancellationToken));
        Assert.Equal(expected, ex.ParamName);
    }

    /// <summary>
    /// <c>DescribeBackend</c> discarded the sizing call's status, so ANY failure
    /// fell through to a hard-coded "demucs.cpp" — the one answer it might not
    /// be. There is nothing to fake here, so the pin is that the happy path
    /// still answers and that the hard-coded string is no longer the only
    /// non-exceptional way out.
    /// </summary>
    [Fact]
    public void DescribeBackend_NeverAnswersFromADiscardedStatus()
    {
        var described = JuceStemSeparation.DescribeBackend();
        Assert.False(string.IsNullOrWhiteSpace(described));

        var source = WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp/JuceStemSeparation.cs");
        var at = source.IndexOf("public static string DescribeBackend()", StringComparison.Ordinal);
        Assert.True(at >= 0, "DescribeBackend is gone — repoint this lint.");
        var open = source.IndexOf('{', at);
        var depth = 0;
        var body = string.Empty;
        for (var i = open; i < source.Length; i++)
        {
            if (source[i] == '{') depth++;
            else if (source[i] == '}' && --depth == 0) { body = source[open..(i + 1)]; break; }
        }

        // Neither StemsDescribeBackend call may have its status dropped.
        Assert.DoesNotContain("NativeMethods.StemsDescribeBackend(null, 0, out var size);", body,
            StringComparison.Ordinal);
        Assert.DoesNotContain("NativeMethods.StemsDescribeBackend(buf, size, out _);", body,
            StringComparison.Ordinal);
        Assert.Equal(2, System.Text.RegularExpressions.Regex.Matches(
            body, @"if \(NativeMethods\.StemsDescribeBackend\(").Count);
    }
}
