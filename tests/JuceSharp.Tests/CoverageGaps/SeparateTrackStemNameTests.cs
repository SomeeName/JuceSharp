using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// <c>separate_track.py</c> names each output file after a stem, and for the
/// RoFormer family the stem names come from the model's yaml — a file the user
/// supplies. A name is a bare <c>[A-Za-z0-9_-]+</c> token whose resolved path is
/// inside <c>--out</c>, or the run is refused before anything is written.
/// Both tests run the real script under the machine's python and skip without one.
/// </summary>
[Collection("StemSeparatorConcurrency")]
public sealed class SeparateTrackStemNameTests : IDisposable
{
    private readonly List<string> _dirs = [];

    private string TempDir()
    {
        var d = Path.Combine(Path.GetTempPath(), "jsw-stem-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(d);
        _dirs.Add(d);
        return d;
    }

    public void Dispose()
    {
        foreach (var d in _dirs)
        {
            try { Directory.Delete(d, true); }
            catch (IOException) { }
        }
    }

    [Fact]
    public async Task Only_bare_stem_names_inside_the_output_directory_are_accepted()
    {
        var python = TestPython();
        if (python is null) Assert.Skip("No python interpreter on PATH.");
        const string probe = """
            import importlib.util, os, sys
            spec = importlib.util.spec_from_file_location("st", sys.argv[1])
            st = importlib.util.module_from_spec(spec); spec.loader.exec_module(st)
            out = sys.argv[2]
            for good in ("vocals", "drums_2", "bass-1"):
                p = st._stem_path(out, good)
                assert os.path.dirname(p) == os.path.realpath(out), p
            for bad in ("../escaped", "..", "a/b", "a\\b", "", "C:evil", "x y", "evil.wav", None, 7):
                try:
                    st._stem_path(out, bad)
                except ValueError:
                    continue
                sys.exit(f"accepted {bad!r}")
            print("OK")
            """;
        var (ok, stdout, stderr) = await PythonEnvManager.RunStreaming(python,
            ["-I", "-c", probe, ScriptPath(), TempDir()], TimeSpan.FromSeconds(60), onLine: null,
            TestContext.Current.CancellationToken);
        Assert.True(ok, stderr);
        Assert.Contains("OK", stdout, StringComparison.Ordinal);
    }

    /// <summary>The whole script: a config whose stem list climbs out of
    /// <c>--out</c> is refused with <c>INVALID_STEM_NAME</c> before the
    /// checkpoint is even loaded, and nothing appears outside the directory.</summary>
    [Fact]
    public async Task A_config_with_a_traversal_stem_name_is_refused_and_writes_nothing_outside()
    {
        var python = TestPython();
        if (python is null) Assert.Skip("No python interpreter on PATH.");
        var ct = TestContext.Current.CancellationToken;
        var (hasYaml, _, _) = await PythonEnvManager.RunStreaming(python, ["-I", "-c", "import yaml"],
            TimeSpan.FromSeconds(30), onLine: null, ct);
        if (!hasYaml) Assert.Skip("The machine's python has no PyYAML.");

        var root = TempDir();
        var outDir = Path.Combine(root, "out");
        Directory.CreateDirectory(outDir);
        var model = Path.Combine(root, "model.safetensors");
        await File.WriteAllBytesAsync(model, [0], ct);
        var config = Path.Combine(root, "model.yaml");
        await File.WriteAllTextAsync(config, "model:\n  stems: ['vocals', '../escaped']\n", ct);
        var input = Path.Combine(root, "in.wav");
        await File.WriteAllBytesAsync(input, [0], ct);

        var (ok, _, stderr) = await PythonEnvManager.RunStreaming(python,
            ["-I", ScriptPath(), "--input", input, "--out", outDir, "--model", "bs_roformer",
             "--model-path", model, "--config", config],
            TimeSpan.FromSeconds(60), onLine: null, ct);

        Assert.False(ok);
        Assert.Contains("INVALID_STEM_NAME", stderr, StringComparison.Ordinal);
        Assert.False(File.Exists(Path.Combine(root, "escaped.wav")));
        Assert.Empty(Directory.GetFiles(outDir));
    }

    private static string ScriptPath()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "PythonScripts", "separate_track.py");
        Assert.True(File.Exists(path), path);
        return path;
    }

    private static string? TestPython()
    {
        var path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrEmpty(path)) return null;
        foreach (var dir in path.Split(Path.PathSeparator))
        {
            foreach (var name in new[] { "python3", "python" })
            {
                var full = Path.Combine(dir, name + (OperatingSystem.IsWindows() ? ".exe" : string.Empty));
                if (File.Exists(full) && !full.Contains("WindowsApps", StringComparison.OrdinalIgnoreCase))
                    return full;
            }
        }
        return null;
    }
}
