using System.Text;
using JuceSharp.AudioIo;
using JuceSharp.Diagnostics;
using JuceSharp.Dsp;
using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

/// <summary>
/// The wave-4 audit fixes outside <c>JuceSharp.Dsp</c>: the AudioIo surface (W4-S19,
/// W4-S28), the diagnostics CLI (W4-S15, W4-S16) and the ONNX sample-rate converter's
/// truncated anti-alias kernel (W4-S10). Pure, offline, deterministic — synthesised
/// signals, in-memory streams and an in-memory console.
/// </summary>
public sealed class Wave4AudioIoAndCliTests
{
    // ─────────────────────────────────────────────── W4-S19

    /// <summary>
    /// <c>DecodeAs</c> validated NEITHER target. <c>targetChannels = 0</c> reached
    /// <c>Remix</c> and returned a <c>Channels = 0</c> buffer — silent data loss reported as
    /// success; <c>targetSampleRate = 0</c> returned 0 frames; a negative one threw
    /// <see cref="OverflowException"/> from an array allocation, naming no argument. All of
    /// it AFTER a full decode, so the caller paid the whole cost first. The throw must now
    /// come from the argument check, before any file is touched — which is why this can
    /// pass a path that does not exist.
    /// </summary>
    [Theory]
    [InlineData(0, 2, "targetSampleRate")]
    [InlineData(-1, 2, "targetSampleRate")]
    [InlineData(44100, 0, "targetChannels")]
    [InlineData(44100, -2, "targetChannels")]
    public void DecodeAs_RejectsANonPositiveTarget_BeforeDecoding(int rate, int channels, string paramName)
    {
        var missing = Path.Combine(Path.GetTempPath(), $"no-such-file-{Guid.NewGuid():N}.wav");
        var ex = Assert.Throws<ArgumentOutOfRangeException>(
            () => JuceAudioDecoder.DecodeAs(missing, rate, channels));
        Assert.Equal(paramName, ex.ParamName);
    }

    // ─────────────────────────────────────────────── W4-S28

    /// <summary>
    /// There must be exactly ONE float32 WAV header implementation in this assembly.
    /// <c>WavIo.WriteFloat32</c> hand-wrote a second one beside <see cref="WavWriter"/>'s,
    /// with independently-derived size arithmetic and two different overflow bounds; they
    /// agreed, and nothing kept them agreeing. A source lint, because the behavioural test
    /// below passes either way — that is the point of it.
    /// </summary>
    [Fact]
    public void WavIo_WriteFloat32_RoutesThroughWavWriter()
    {
        var src = File.ReadAllText(Path.Combine(RepoRoot(), "src", "JuceSharp.AudioIo", "WavIo.cs"));
        var body = src[src.IndexOf("public static void WriteFloat32(Stream", StringComparison.Ordinal)..];
        Assert.Contains("new WavWriter(", body, StringComparison.Ordinal);
        // No second header writer: no BinaryWriter, and no chunk tag actually EMITTED.
        // (The words still appear in prose above the call, which is why this matches the
        // write expression and not the bare string.)
        Assert.DoesNotContain("new BinaryWriter", body, StringComparison.Ordinal);
        Assert.DoesNotContain("bw.Write(\"RIFF\"", body, StringComparison.Ordinal);
        Assert.DoesNotContain("bw.Write(\"fmt \"", body, StringComparison.Ordinal);
        Assert.DoesNotContain("bw.Write(\"fact\"", body, StringComparison.Ordinal);
    }

    /// <summary>The bytes on disk must not move when the header implementation is shared:
    /// the same buffer through <c>WavIo.WriteFloat32</c> and through a bare
    /// <see cref="WavWriter"/> is byte-for-byte identical.</summary>
    [Theory]
    [InlineData(1, 0)]
    [InlineData(1, 1000)]
    [InlineData(2, 4097)]
    [InlineData(6, 333)]
    public void WavIo_WriteFloat32_IsByteIdenticalToWavWriter(int channels, int frames)
    {
        var data = new float[frames * channels];
        for (var i = 0; i < data.Length; i++) data[i] = MathF.Sin(i * 0.01f);

        using var a = new MemoryStream();
        WavIo.WriteFloat32(a, new AudioBuffer(data, 44100, channels));

        using var b = new MemoryStream();
        using (var w = new WavWriter(b, 44100, channels, WavSampleFormat.Float32, leaveOpen: true))
            w.Write(data);

        Assert.Equal(b.ToArray(), a.ToArray());
    }

    // ─────────────────────────────────────────────── W4-S16

    /// <summary>
    /// The CLI printed "press Ctrl+C again to force" and then set <c>e.Cancel = true</c>
    /// unconditionally, so press two was swallowed exactly like press one — the escape
    /// hatch it advertised did not exist, and Task Manager was the only way out of a wedged
    /// four-hour torch install. Press one suppresses and cancels; press two must NOT
    /// suppress.
    /// </summary>
    [Fact]
    public void Cli_SecondCtrlC_DoesNotSuppressTheDefaultHandler()
    {
        using var cts = new CancellationTokenSource();
        var stderr = new StringWriter();
        var presses = 0;

        Assert.True(Program.HandleCancel(ref presses, cts, stderr));
        Assert.True(cts.IsCancellationRequested);
        Assert.Contains("again to force", stderr.ToString(), StringComparison.Ordinal);

        Assert.False(Program.HandleCancel(ref presses, cts, stderr));
        Assert.False(Program.HandleCancel(ref presses, cts, stderr));
        Assert.Contains("forcing.", stderr.ToString(), StringComparison.Ordinal);
    }

    // ─────────────────────────────────────────────── W4-S15

    /// <summary>
    /// <c>--probe</c> was the one arm with no exception handling, while
    /// <c>ExitCodes.EnvironmentFailed</c>'s own doc described that in the past tense.
    /// <c>new StemSeparator()</c> constructs a <c>PythonEnvManager</c> whose ctor does
    /// <c>Directory.CreateDirectory(Root)</c>, so a locked-down or full profile volume
    /// exited 0xE0434352 instead of 12. Driven through the factory the probe arm now takes,
    /// because the throw happens in the CONSTRUCTOR.
    /// </summary>
    [Theory]
    [InlineData(typeof(UnauthorizedAccessException))]
    [InlineData(typeof(IOException))]
    [InlineData(typeof(InvalidOperationException))]
    public async Task Cli_Probe_ClassifiesAnEnvironmentFailure_InsteadOfThrowing(Type exceptionType)
    {
        var stdout = new StringWriter();
        var stderr = new StringWriter();
        var code = await Program.RunProbeAsync(
            () => throw (Exception)Activator.CreateInstance(exceptionType, "profile volume is read-only")!,
            stdout, stderr, CancellationToken.None);

        Assert.Equal(ExitCodes.EnvironmentFailed, code);
        Assert.Contains("ENVIRONMENT FAILED", stderr.ToString(), StringComparison.Ordinal);
    }

    /// <summary>A cancelled probe still exits 130, not 12.</summary>
    [Fact]
    public async Task Cli_Probe_ReportsCancellationSeparately()
    {
        var code = await Program.RunProbeAsync(
            () => throw new OperationCanceledException(),
            new StringWriter(), new StringWriter(), CancellationToken.None);
        Assert.Equal(ExitCodes.Cancelled, code);
    }

    // ─────────────────────────────────────────────── W4-S10

    /// <summary>
    /// Scaling the Lanczos argument by <c>cutoff</c> lowers the kernel's cutoff AND
    /// stretches its support by <c>1/cutoff</c> in source samples, but the loop still
    /// visited a fixed 2·Lobes of them — so at <c>cutoff &lt; 1</c> the sum was a hard
    /// truncation of the intended window: a rectangular window on a sinc, i.e. the very
    /// aliasing the <c>cutoff</c> line exists to prevent. At 96 → 44.1 kHz
    /// (<c>cutoff</c> 0.459) the kernel was cut at HALF its support, and a 30 kHz tone
    /// folded back to 14.1 kHz.
    /// </summary>
    [Fact]
    public void SampleRateConverter_Downsampling96kTo44k_RejectsAnOutOfBandTone()
    {
        const int srcRate = 96000, dstRate = 44100, frames = 96000;
        var input = new float[frames];
        for (var i = 0; i < frames; i++) input[i] = MathF.Sin(2f * MathF.PI * 30000f * i / srcRate);

        var output = SampleRateConverter.Resample(input, 1, srcRate, dstRate);

        // 30 kHz at 96 kHz aliases to |44100 - 30000| = 14,100 Hz after the decimation.
        // Measured: -28.6 dBFS with the truncated kernel, -51.2 dBFS with its full
        // support. The threshold sits between the two, not on either.
        var aliasDb = ToneLevelDb(output, dstRate, 14100f);
        Assert.True(aliasDb < -45f,
            $"the 14.1 kHz alias must sit below -45 dBFS; measured {aliasDb:F1} dBFS");
    }

    /// <summary>…and the widened kernel must not have cost the passband anything: a 1 kHz
    /// tone through the same conversion still comes out at full level.</summary>
    [Fact]
    public void SampleRateConverter_Downsampling_KeepsThePassband()
    {
        const int srcRate = 96000, dstRate = 44100, frames = 96000;
        var input = new float[frames];
        for (var i = 0; i < frames; i++) input[i] = MathF.Sin(2f * MathF.PI * 1000f * i / srcRate);

        var output = SampleRateConverter.Resample(input, 1, srcRate, dstRate);
        Assert.InRange(ToneLevelDb(output, dstRate, 1000f), -1.0f, 0.5f);
    }

    /// <summary>
    /// Level of one frequency in dBFS, by a direct Goertzel-style correlation over the
    /// steady middle of the signal (the edges are the resampler's own ramp-in). Windowed,
    /// so a tone off a bin centre does not read as leakage.
    /// </summary>
    private static float ToneLevelDb(float[] mono, int sampleRate, float hz)
    {
        var start = mono.Length / 4;
        var n = mono.Length / 2;
        var w = Windows.Hann(n);
        double re = 0, im = 0, norm = 0;
        for (var i = 0; i < n; i++)
        {
            var ang = 2.0 * Math.PI * hz * i / sampleRate;
            re += mono[start + i] * w[i] * Math.Cos(ang);
            im += mono[start + i] * w[i] * Math.Sin(ang);
            norm += w[i];
        }
        // A unit-amplitude sine correlates to norm/2 on each of the two quadratures
        // combined, so norm/2 is the 0 dBFS reference.
        var amp = 2.0 * Math.Sqrt(re * re + im * im) / norm;
        return (float)(20.0 * Math.Log10(Math.Max(amp, 1e-12)));
    }

    private static string RepoRoot()
    {
        var dir = AppContext.BaseDirectory;
        while (dir is not null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        Assert.NotNull(dir);
        return dir!;
    }
}
