using System.Text.RegularExpressions;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// DotCharter#105: a <c>.ckpt</c> is a pickle, and unpickling runs whatever
/// code the file names. No code from a model file may run unless the host
/// explicitly opts in (<see cref="CkptToOnnxConverter.AllowUnsafePickleFallback"/>),
/// and <c>weights_only=True</c> is only a real boundary on torch 2.6+
/// (CVE-2025-32434). These cases pin both halves without an interpreter.
/// </summary>
public sealed class CkptUnsafePickleTests : IDisposable
{
    private readonly string _dir =
        Path.Combine(Path.GetTempPath(), "jucesharp-pickle-" + Guid.NewGuid().ToString("N"));

    public CkptUnsafePickleTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
    }

    private static string Script(string name)
    {
        var path = Path.Combine(AppContext.BaseDirectory, "PythonScripts", name);
        Assert.True(File.Exists(path), $"Expected bundled script at {path}");
        return File.ReadAllText(path);
    }

    /// <summary>The body of a top-level <c>def</c>: every line up to the next
    /// line that starts in column 0.</summary>
    private static string FunctionBody(string source, string name)
    {
        var m = Regex.Match(source, $@"^def {name}\(.*?(?=^\S)", RegexOptions.Multiline | RegexOptions.Singleline);
        Assert.True(m.Success, $"def {name} not found.");
        return m.Value;
    }

    private static (int Major, int Minor) LowerBound(string spec)
    {
        var m = Regex.Match(spec, @">=\s*(\d+)\.(\d+)");
        Assert.True(m.Success, $"'{spec}' has no >= lower bound.");
        return (int.Parse(m.Groups[1].Value), int.Parse(m.Groups[2].Value));
    }

    [Fact]
    public void Torch_floor_is_the_release_where_weights_only_is_a_real_boundary()
    {
        // CVE-2025-32434: weights_only=True was bypassable before 2.6.0.
        Assert.True(LowerBound(PythonPackagePins.Torch).CompareTo((2, 6)) >= 0, PythonPackagePins.Torch);
        Assert.True(LowerBound(PythonPackagePins.TorchAudio).CompareTo((2, 6)) >= 0, PythonPackagePins.TorchAudio);
        // demucs 4.0.x calls torch.load with the old default, which torch 2.6
        // flipped; 4.1 loads its own sha-checked weights explicitly.
        Assert.True(LowerBound(PythonPackagePins.Demucs).CompareTo((4, 1)) >= 0, PythonPackagePins.Demucs);
    }

    [Fact]
    public void Safetensors_is_installed_wherever_a_checkpoint_is_loaded()
    {
        Assert.Contains(PythonPackagePins.SafeTensors, CkptToOnnxConverter.RequiredPackages);
        Assert.Contains(PythonPackagePins.SafeTensors, StemSeparator.RequiredPackages);
    }

    [Fact]
    public void Separator_script_never_unpickles_a_checkpoint()
    {
        var src = Script("separate_track.py");
        Assert.DoesNotContain("weights_only=False", src, StringComparison.Ordinal);
        foreach (Match call in Regex.Matches(src, @"torch\.load\([^)]*\)"))
            Assert.Contains("weights_only=True", call.Value, StringComparison.Ordinal);
    }

    [Fact]
    public void Converter_script_unpickles_only_behind_the_opt_in()
    {
        var src = Script("ckpt_to_onnx.py");

        // Every torch.load names weights_only; the one weights_only=False lives
        // in the single full-unpickle function and nowhere else.
        foreach (Match call in Regex.Matches(src, @"torch\.load\([^)]*\)"))
            Assert.Contains("weights_only=", call.Value, StringComparison.Ordinal);
        Assert.Single(Regex.Matches(src, "weights_only=False"));
        Assert.Contains("weights_only=False", FunctionBody(src, "_load_ckpt_full_pickle"), StringComparison.Ordinal);

        // Its only caller refuses before reaching it unless the flag was passed.
        Assert.Equal(2, Regex.Matches(src, @"_load_ckpt_full_pickle\(").Count);
        var loader = FunctionBody(src, "_load_ckpt");
        var refuse = loader.IndexOf("if not allow_unsafe_pickle:", StringComparison.Ordinal);
        var fallback = loader.IndexOf("_load_ckpt_full_pickle(", StringComparison.Ordinal);
        Assert.True(refuse >= 0 && fallback > refuse, "the full unpickle is reachable without the opt-in.");
        Assert.Contains("weights_only=True", loader, StringComparison.Ordinal);
        Assert.Contains("--allow-unsafe-pickle", src, StringComparison.Ordinal);
        Assert.Contains(CkptToOnnxConverter.UnsafePickleRefusedMarker, src, StringComparison.Ordinal);
    }

    private async Task<IReadOnlyList<string>> CapturedArgv(CkptToOnnxConverter conv)
    {
        var ckpt = Path.Combine(_dir, "model.ckpt");
        await File.WriteAllTextAsync(ckpt, "x", TestContext.Current.CancellationToken);
        IReadOnlyList<string> seen = [];
        await conv.ConvertCoreAsync(ckpt, null, Path.Combine(_dir, "model.onnx"), progress: null,
            runner: async (args, _) =>
            {
                seen = args.ToList();
                var outArg = args[args.ToList().IndexOf("--out") + 1];
                await File.WriteAllBytesAsync(outArg, [8, 1], CancellationToken.None);
                return (true, string.Empty, string.Empty);
            },
            ct: TestContext.Current.CancellationToken);
        return seen;
    }

    [Fact]
    public async Task By_default_the_converter_does_not_allow_a_full_unpickle()
    {
        var argv = await CapturedArgv(new CkptToOnnxConverter());
        Assert.DoesNotContain("--allow-unsafe-pickle", argv);
    }

    [Fact]
    public async Task The_opt_in_reaches_the_script()
    {
        var argv = await CapturedArgv(new CkptToOnnxConverter { AllowUnsafePickleFallback = true });
        Assert.Contains("--allow-unsafe-pickle", argv);
    }

    [Fact]
    public async Task A_refused_unpickle_surfaces_as_its_own_exception()
    {
        var ckpt = Path.Combine(_dir, "model.ckpt");
        await File.WriteAllTextAsync(ckpt, "x", TestContext.Current.CancellationToken);
        var conv = new CkptToOnnxConverter();

        var ex = await Assert.ThrowsAsync<UnsafeCheckpointException>(() => conv.ConvertCoreAsync(
            ckpt, null, Path.Combine(_dir, "model.onnx"), progress: null,
            runner: (_, _) => Task.FromResult((false, string.Empty,
                CkptToOnnxConverter.UnsafePickleRefusedMarker + ": needs full unpickling")),
            ct: TestContext.Current.CancellationToken));

        Assert.IsAssignableFrom<InvalidOperationException>(ex);
        Assert.DoesNotContain(Directory.GetFiles(_dir), f => f != ckpt);
    }
}
