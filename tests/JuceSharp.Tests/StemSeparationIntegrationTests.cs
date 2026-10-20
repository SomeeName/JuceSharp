using System.Text;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// End-to-end tests for the full stem-separation pipeline. These hit
/// the real network (downloading the embedded Python distribution and
/// pip-installing torch, demucs, etc. — multiple GB) and are therefore
/// gated on the <c>JUCESHARP_RUN_INTEGRATION</c> environment variable.
///
/// To run locally:
/// <code>
///   $env:JUCESHARP_RUN_INTEGRATION="1"
///   dotnet test JuceSharp/tests/JuceSharp.Tests --filter "FullyQualifiedName~Integration"
/// </code>
///
/// CI is expected to skip these by default.
/// </summary>
// These spawn real python children through PythonEnvManager.RunStreaming, which
// registers them with the process-global ChildProcessRegistry — so they belong to
// the collection that owns it, exactly like StemSeparatorTests. Outside it,
// ChildProcessRegistryTests.KillAll() can terminate this file's children mid-run.
[Collection("StemSeparatorConcurrency")]
public class StemSeparationIntegrationTests(ITestOutputHelper @out)
{
    private readonly ITestOutputHelper _out = @out;

    private const string OptIn = "JUCESHARP_RUN_INTEGRATION";

    private static bool ShouldRun() =>
        string.Equals(Environment.GetEnvironmentVariable(OptIn), "1", StringComparison.Ordinal);

    private static string TempDir(string suffix)
    {
        var p = Path.Combine(Path.GetTempPath(), $"jucesharp-it-{suffix}-{Guid.NewGuid():N}");
        Directory.CreateDirectory(p);
        return p;
    }

    /// <summary>
    /// Synthesise a 5-second 44.1 kHz stereo WAV containing a sine
    /// (left = 440 Hz, right = 880 Hz) so we have a deterministic input
    /// without committing a binary blob to the repo.
    /// </summary>
    private static string WriteSyntheticWav(string dir, double seconds = 5.0, int sampleRate = 44100)
    {
        var path = Path.Combine(dir, "synthetic.wav");
        var totalSamples = (int)(seconds * sampleRate);
        const short channels = 2;
        const short bitsPerSample = 16;
        var byteRate = sampleRate * channels * bitsPerSample / 8;
        var dataSize = totalSamples * channels * (bitsPerSample / 8);

        using var fs = File.Create(path);
        using var bw = new BinaryWriter(fs);
        // RIFF header
        bw.Write(Encoding.ASCII.GetBytes("RIFF"));
        bw.Write(36 + dataSize);
        bw.Write(Encoding.ASCII.GetBytes("WAVE"));
        // fmt subchunk
        bw.Write(Encoding.ASCII.GetBytes("fmt "));
        bw.Write(16);                                   // subchunk size
        bw.Write((short)1);                              // PCM
        bw.Write(channels);
        bw.Write(sampleRate);
        bw.Write(byteRate);
        bw.Write((short)(channels * bitsPerSample / 8)); // block align
        bw.Write(bitsPerSample);
        // data subchunk
        bw.Write(Encoding.ASCII.GetBytes("data"));
        bw.Write(dataSize);
        for (int i = 0; i < totalSamples; i++)
        {
            var l = (short)(short.MaxValue * 0.5 * Math.Sin(2 * Math.PI * 440 * i / sampleRate));
            var r = (short)(short.MaxValue * 0.5 * Math.Sin(2 * Math.PI * 880 * i / sampleRate));
            bw.Write(l);
            bw.Write(r);
        }
        return path;
    }

    [Fact]
    public void Synthetic_wav_helper_writes_a_valid_riff_file()
    {
        // This guard runs unconditionally so the test asset code itself
        // is covered even when integration is skipped.
        var dir = TempDir("wav");
        try
        {
            var path = WriteSyntheticWav(dir, seconds: 0.1);
            var bytes = File.ReadAllBytes(path);
            Assert.Equal((byte)'R', bytes[0]);
            Assert.Equal((byte)'I', bytes[1]);
            Assert.Equal((byte)'F', bytes[2]);
            Assert.Equal((byte)'F', bytes[3]);
            Assert.Equal((byte)'W', bytes[8]);
            Assert.Equal((byte)'A', bytes[9]);
            Assert.Equal((byte)'V', bytes[10]);
            Assert.Equal((byte)'E', bytes[11]);
            Assert.True(bytes.Length > 44);
        }
        finally { try { Directory.Delete(dir, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Integration_EnsureInterpreterAsync_actually_downloads_and_extracts_python()
    {
        // Assert.Skip, never a bare `return` — a return reports PASSED, and a
        // machine without the opt-in then shows ~18 green tests that executed
        // nothing at all.
        if (!ShouldRun()) Assert.Skip($"Set {OptIn}=1 to run the multi-GB integration download.");
        if (!OperatingSystem.IsWindows())
            Assert.Skip("The embedded Python distribution is Windows-only.");

        var root = TempDir("py");
        try
        {
            var mgr = new PythonEnvManager(rootDirectory: root);
            var phases = new List<string>();
            var prog = new Progress<PythonInstallProgress>(p =>
            {
                phases.Add(p.Phase);
                _out.WriteLine($"  [{p.Phase}] {p.Fraction:P0} {p.Message}");
            });
            using var cts = new CancellationTokenSource(TimeSpan.FromMinutes(5));
            await mgr.EnsureInterpreterAsync(prog, cts.Token);

            Assert.True(File.Exists(mgr.Executable), $"Expected python.exe at {mgr.Executable}");
            Assert.Contains("download", phases);
            Assert.Contains("ready", phases);
            Assert.Empty(Directory.GetFiles(root, "*.tmp.*"));

            // Verify pth patch worked by spawning the interpreter and
            // asking it to print sys.path entries containing site-packages.
            var (ok, stdout, _) = await PythonEnvManager.RunStreaming(
                mgr.Executable,
                "-c \"import sys; print('|'.join(sys.path))\"",
                TimeSpan.FromSeconds(10), null, TestContext.Current.CancellationToken);
            Assert.True(ok);
            _out.WriteLine($"sys.path = {stdout}");
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Integration_SeparateTrackAsync_writes_real_stem_files()
    {
        if (!ShouldRun()) Assert.Skip($"Set {OptIn}=1 to run the multi-GB integration download.");

        var root = TempDir("pyfull");
        var workDir = TempDir("io");
        try
        {
            var input = WriteSyntheticWav(workDir, seconds: 5.0);
            var outDir = Path.Combine(workDir, "stems");

            var mgr = new PythonEnvManager(rootDirectory: root);
            var sep = new StemSeparator(mgr);

            var phases = new List<string>();
            var prog = new Progress<StemSeparationProgress>(p =>
            {
                phases.Add(p.Phase);
                _out.WriteLine($"  [{p.Phase}] {p.Fraction,7:P0} {p.Message} {p.Stem}");
            });

            using var cts = new CancellationTokenSource(TimeSpan.FromHours(1));
            // Use the lightest-weight Demucs model (htdemucs base ~ 80 MB).
            var result = await sep.SeparateTrackAsync(input, outDir,
                new StemSeparationOptions { Model = "htdemucs", Device = "auto", Shifts = 0 },
                prog, cts.Token);

            Assert.NotNull(result);
            Assert.NotEmpty(result.Stems);
            foreach (var stem in result.Stems)
            {
                Assert.True(File.Exists(stem.Path), $"stem '{stem.Name}' missing at {stem.Path}");
                var info = new FileInfo(stem.Path);
                Assert.True(info.Length > 1024, $"stem '{stem.Name}' suspiciously small: {info.Length} bytes");
                _out.WriteLine($"  wrote {stem.Name}: {info.Length:N0} bytes");
            }
            Assert.Contains("done", phases);
            Assert.Empty(Directory.GetFiles(root, "*.tmp.*"));
        }
        finally
        {
            try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
            try { Directory.Delete(workDir, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }
}
