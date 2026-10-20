using System.Runtime.CompilerServices;
using System.Text;
using System.Text.RegularExpressions;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// Everything the embedded-Python bootstrap downloads and then executes or
/// loads is pinned by SHA-256 and fetched over https: the pip wheel, every
/// package wheel (a hash-locked set installed with <c>--require-hashes</c>) and
/// the Demucs weights. The interpreter is never taken from PATH, and the
/// cross-process gate is not a kernel object another user can name.
///
/// <para>Nothing here installs anything or reaches the internet: downloads are
/// served from a loopback socket, and refusals happen before any transfer.</para>
/// </summary>
[Collection("StemDownloads")]
public sealed partial class PythonSupplyChainTests : IDisposable
{
    private readonly List<string> _scratch = [];

    private string TempRoot()
    {
        var p = Path.Combine(Path.GetTempPath(), "jucesharp-supply-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(p);
        lock (_scratch) _scratch.Add(p);
        return p;
    }

    public void Dispose()
    {
        foreach (var d in _scratch)
        {
            try { Directory.Delete(d, true); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }

    /// <summary>A manager whose interpreter "exists" but has no pip, so
    /// <c>EnsurePipAsync</c> goes straight to the download.</summary>
    private PythonEnvManager ManagerWithoutPip()
    {
        var root = TempRoot();
        return new PythonEnvManager(root) { Executable = Path.Combine(root, "no-such-python.exe") };
    }

    // ------------------------------------------------------------ pip wheel

    [Fact]
    public void The_pip_bootstrap_defaults_to_a_pinned_wheel_over_https()
    {
        var mgr = ManagerWithoutPip();
        var uri = new Uri(mgr.PipWheelUrl);
        Assert.Equal(Uri.UriSchemeHttps, uri.Scheme);
        Assert.False(uri.IsLoopback);
        Assert.EndsWith(".whl", uri.AbsolutePath, StringComparison.Ordinal);
        Assert.Matches("^[0-9a-f]{64}$", mgr.PipWheelSha256);
        Assert.Equal(PythonEnvManager.DefaultPipWheelSha256, mgr.PipWheelSha256);
    }

    [Fact]
    public async Task EnsurePip_refuses_a_plain_http_source_before_any_transfer()
    {
        var mgr = ManagerWithoutPip();
        mgr.PipWheelUrl = "http://example.invalid/pip-26.2.1-py3-none-any.whl";

        var ex = await Assert.ThrowsAsync<ArgumentException>(
            () => mgr.EnsurePipAsync(ct: TestContext.Current.CancellationToken));
        Assert.Contains("https", ex.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task EnsurePip_refuses_an_unpinned_wheel()
    {
        var mgr = ManagerWithoutPip();
        mgr.PipWheelSha256 = "";

        await Assert.ThrowsAsync<InvalidOperationException>(
            () => mgr.EnsurePipAsync(ct: TestContext.Current.CancellationToken));
    }

    /// <summary>The served body is not the pinned wheel: it is refused before
    /// the rename, so there is nothing at the destination for pip to run.</summary>
    [Fact]
    public async Task EnsurePip_refuses_a_wheel_whose_hash_differs_and_runs_nothing()
    {
        var mgr = ManagerWithoutPip();
        using var server = LoopbackFileServer.Start(
            Encoding.ASCII.GetBytes("not the pinned pip wheel"), "/pip-26.2.1-py3-none-any.whl");
        mgr.PipWheelUrl = server.Url;

        await Assert.ThrowsAsync<InvalidDataException>(
            () => mgr.EnsurePipAsync(ct: TestContext.Current.CancellationToken));
        Assert.False(File.Exists(Path.Combine(mgr.Root, "pip-26.2.1-py3-none-any.whl")));
    }

    // ------------------------------------------------------- package lock

    [Fact]
    public async Task EnsurePackages_refuses_a_spec_outside_the_lock()
    {
        var mgr = ManagerWithoutPip();
        var ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => mgr.EnsurePackagesAsync(["requests"], ct: TestContext.Current.CancellationToken));
        Assert.Contains("requests", ex.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void Every_locked_package_is_pinned_exactly_with_at_least_one_sha256()
    {
        var entries = ParseLock(PythonPackageLock.Text);
        Assert.True(entries.Count > 20, $"only {entries.Count} locked packages");
        Assert.All(entries.Values, e =>
        {
            Assert.NotEmpty(e.Hashes);
            Assert.All(e.Hashes, h => Assert.Matches("^[0-9a-f]{64}$", h));
        });
        // Every requirement line is NAME==VERSION; nothing else may slip in.
        foreach (var line in PythonPackageLock.Text.Split('\n'))
        {
            var t = line.Trim();
            if (t.Length == 0 || t.StartsWith('#') || t.StartsWith("--hash=", StringComparison.Ordinal)) continue;
            Assert.Matches(@"^[A-Za-z0-9_.\-]+==[A-Za-z0-9_.+\-]+ \\$", t);
        }
    }

    /// <summary>The lock is generated from <see cref="PythonPackagePins"/>; a pin
    /// that moved without regenerating it would install versions the pin does
    /// not allow.</summary>
    [Fact]
    public void Every_pin_is_satisfied_by_its_locked_version()
    {
        var entries = ParseLock(PythonPackageLock.Text);
        foreach (var pin in PythonPackagePins.InstalledDirectories.Keys)
        {
            var m = PinPattern().Match(pin);
            Assert.True(m.Success, pin);
            var name = Normalize(m.Groups["name"].Value);
            Assert.True(entries.TryGetValue(name, out var locked), $"{pin} is not in the lock");
            foreach (var clause in m.Groups["spec"].Value.Split(',', StringSplitOptions.RemoveEmptyEntries))
            {
                var c = ClausePattern().Match(clause.Trim());
                Assert.True(c.Success, clause);
                var cmp = CompareVersions(locked.Version, c.Groups["v"].Value);
                var ok = c.Groups["op"].Value switch
                {
                    ">=" => cmp >= 0, ">" => cmp > 0, "<=" => cmp <= 0, "<" => cmp < 0, "==" => cmp == 0,
                    var op => throw new InvalidOperationException("unhandled operator " + op),
                };
                Assert.True(ok, $"{name}=={locked.Version} does not satisfy {pin}");
            }
        }
    }

    [Fact]
    public void The_lock_covers_every_package_both_components_ask_for()
    {
        Assert.All(StemSeparator.RequiredPackages.Concat(CkptToOnnxConverter.RequiredPackages),
            spec => Assert.Contains(spec, PythonPackageLock.CoveredPins));
    }

    [Fact]
    public void Regenerating_the_lock_changes_the_package_stamp()
    {
        var pins = StemSeparator.RequiredPackages;
        Assert.NotEqual(
            PythonEnvManager.PackageSetStampFileNameForLock(pins, "0000000000000000"),
            PythonEnvManager.PackageSetStampFileNameForLock(pins, "1111111111111111"));
        Assert.Equal(
            PythonEnvManager.PackageSetStampFileNameForLock(pins, PythonPackageLock.Digest),
            PythonEnvManager.PackageSetStampFileName(pins));
    }

    // ----------------------------------------- interpreter + bootstrap gate

    /// <summary>Whichever <c>python3</c> comes first on PATH is not an
    /// interpreter anybody pinned; the manager never looks there.</summary>
    [Fact]
    public void The_interpreter_is_never_resolved_from_PATH()
    {
        var source = File.ReadAllText(SourcePath("Runtime", "PythonEnvManager.cs"));
        Assert.DoesNotContain("GetEnvironmentVariable(\"PATH\")", source);
        Assert.DoesNotContain("ResolveSystemPython3", source);
    }

    /// <summary>
    /// The cross-process half of the gate is an exclusive handle on a file in
    /// the root — not a named kernel semaphore whose name any user could derive
    /// from the root path and squat on.
    /// </summary>
    [Fact]
    public async Task The_bootstrap_gate_is_an_exclusive_lock_file_in_the_root()
    {
        var root = TempRoot();
        var lockFile = Path.Combine(root, PythonEnvManager.EnvLockFileName);

        using (await PythonEnvManager.AcquireEnvLockAsync(root, TestContext.Current.CancellationToken))
        {
            Assert.True(File.Exists(lockFile));
            Assert.Throws<IOException>(() =>
                new FileStream(lockFile, FileMode.Open, FileAccess.ReadWrite, FileShare.None).Dispose());
        }

        using (new FileStream(lockFile, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }

        var source = File.ReadAllText(SourcePath("Runtime", "PythonEnvManager.cs"));
        Assert.DoesNotContain("new Semaphore(", source);
        Assert.DoesNotContain("@\"Global\\\"", source);
    }

    // --------------------------------------------------------- Demucs weights

    [Fact]
    public void Every_pinned_demucs_bag_names_a_revision_and_a_sha256_per_file()
    {
        Assert.Contains(DemucsWeightPins.All, b => b.Model == "htdemucs");
        Assert.All(DemucsWeightPins.All, bag =>
        {
            Assert.Matches("^[0-9a-f]{40}$", bag.Revision);
            Assert.StartsWith("adefossez/", bag.Repository, StringComparison.Ordinal);
            Assert.Contains(bag.Files, f => f.Name == bag.Model + ".yaml");
            Assert.Contains(bag.Files, f => f.Name.EndsWith(".safetensors", StringComparison.Ordinal));
            Assert.All(bag.Files, f =>
            {
                Assert.Matches("^[0-9a-f]{64}$", f.Sha256);
                Assert.True(f.Bytes > 0);
                Assert.Equal(f.Name, Path.GetFileName(f.Name));
            });
        });
    }

    [Fact]
    public void A_weight_file_is_fetched_from_its_pinned_revision_over_https()
    {
        var bag = DemucsWeightPins.Find("htdemucs")!;
        var uri = DemucsWeightPins.FileUri(bag, bag.Files[0]);
        Assert.Equal(Uri.UriSchemeHttps, uri.Scheme);
        Assert.Equal($"https://huggingface.co/{bag.Repository}/resolve/{bag.Revision}/{bag.Files[0].Name}",
            uri.ToString());
    }

    [Fact]
    public async Task A_weight_file_whose_hash_differs_is_refused_and_not_left_in_place()
    {
        var real = DemucsWeightPins.Find("htdemucs")!;
        var yaml = real.Files.Single(f => f.Name.EndsWith(".yaml", StringComparison.Ordinal));
        var body = Encoding.ASCII.GetBytes("models: ['00000000']\n");
        // Same name and size as the pin, different bytes: only the hash can tell.
        var forged = new DemucsWeightBag(real.Model, "adefossez/HTDemucs", real.Revision,
            [yaml with { Bytes = body.Length }]);
        using var server = LoopbackFileServer.Start(body,
            $"/adefossez/HTDemucs/resolve/{real.Revision}/{yaml.Name}");
        var dir = TempRoot();

        await Assert.ThrowsAsync<InvalidDataException>(() => DemucsWeightPins.EnsureAsync(
            forged, dir, log: null, TestContext.Current.CancellationToken,
            baseUri: new Uri(new Uri(server.Url), "/")));
        Assert.False(File.Exists(Path.Combine(dir, yaml.Name)));
    }

    [Fact]
    public async Task A_demucs_model_with_no_pinned_weights_is_refused_before_anything_is_provisioned()
    {
        var root = TempRoot();
        var input = Path.Combine(TempRoot(), "in.wav");
        await File.WriteAllBytesAsync(input, [1, 2, 3], TestContext.Current.CancellationToken);
        var script = Path.Combine(TempRoot(), "unused.py");
        await File.WriteAllTextAsync(script, "", TestContext.Current.CancellationToken);
        var sep = new StemSeparator(new PythonEnvManager(root), scriptPathOverride: script);

        // An already-cancelled token: anything that reached the provisioning
        // would throw OperationCanceledException instead.
        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();
        await Assert.ThrowsAsync<ArgumentException>(() => sep.SeparateTrackAsync(
            input, TempRoot(), new StemSeparationOptions { Model = "not_a_pinned_bag" }, ct: cts.Token));
        Assert.False(File.Exists(Path.Combine(root, "python.exe")));
    }

    [Fact]
    public void The_weights_directory_reaches_the_script_rooted()
    {
        var args = StemSeparator.BuildArgs("s.py", "in.wav", "out", StemSeparationOptions.Default, "weights");
        var i = args.IndexOf("--weights-dir");
        Assert.True(i > 0);
        Assert.Equal(Path.GetFullPath("weights"), args[i + 1]);
        Assert.DoesNotContain("--weights-dir",
            StemSeparator.BuildArgs("s.py", "in.wav", "out", StemSeparationOptions.Default));
    }

    // ---------------------------------------------------- the python script

    [Fact]
    public async Task The_script_refuses_a_demucs_model_without_pinned_weights()
    {
        var python = ResolveTestPython();
        if (python is null) Assert.Skip("No python interpreter on PATH.");
        var input = Path.Combine(TempRoot(), "in.wav");
        await File.WriteAllBytesAsync(input, [1, 2, 3], TestContext.Current.CancellationToken);

        var (ok, _, stderr) = await PythonEnvManager.RunStreaming(python,
            [ScriptPath(), "--input", input, "--out", TempRoot(), "--model", "htdemucs"],
            TimeSpan.FromSeconds(60), onLine: null, TestContext.Current.CancellationToken);

        Assert.False(ok);
        Assert.Contains("PINNED_WEIGHTS_REQUIRED", stderr, StringComparison.Ordinal);
    }

    [Fact]
    public async Task The_script_only_builds_classes_from_the_demucs_package()
    {
        var python = ResolveTestPython();
        if (python is null) Assert.Skip("No python interpreter on PATH.");
        const string probe = """
            import importlib.util, sys
            spec = importlib.util.spec_from_file_location("st", sys.argv[1])
            st = importlib.util.module_from_spec(spec); spec.loader.exec_module(st)
            st._require_demucs_class("demucs.htdemucs.HTDemucs")
            for bad in ("os.system", "builtins.eval", None, "demucs..x"):
                try:
                    st._require_demucs_class(bad)
                except ValueError:
                    continue
                sys.exit(f"accepted {bad!r}")
            try:
                st._check_structure_classes({"_dict": [["a", {"_class": "subprocess.Popen"}]]})
            except ValueError:
                print("OK")
                sys.exit(0)
            sys.exit("accepted a nested class")
            """;
        var (ok, stdout, stderr) = await PythonEnvManager.RunStreaming(python,
            ["-I", "-c", probe, ScriptPath()], TimeSpan.FromSeconds(60), onLine: null,
            TestContext.Current.CancellationToken);
        Assert.True(ok, stderr);
        Assert.Contains("OK", stdout, StringComparison.Ordinal);
    }

    // ---------------------------------------------------------------- helpers

    private sealed record Locked(string Version, List<string> Hashes);

    private static Dictionary<string, Locked> ParseLock(string text)
    {
        var result = new Dictionary<string, Locked>(StringComparer.Ordinal);
        Locked? current = null;
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.Trim().TrimEnd('\\').Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;
            if (line.StartsWith("--hash=sha256:", StringComparison.Ordinal))
            {
                Assert.NotNull(current);
                current.Hashes.Add(line["--hash=sha256:".Length..]);
                continue;
            }
            var eq = line.IndexOf("==", StringComparison.Ordinal);
            Assert.True(eq > 0, line);
            current = new Locked(line[(eq + 2)..], []);
            result.Add(Normalize(line[..eq]), current);
        }
        return result;
    }

    private static string Normalize(string name) =>
        Regex.Replace(name, "[-_.]+", "-").ToLowerInvariant();

    private static int CompareVersions(string a, string b)
    {
        var x = a.Split('.').Select(p => int.Parse(new string(p.TakeWhile(char.IsDigit).ToArray()) is { Length: > 0 } d ? d : "0")).ToArray();
        var y = b.Split('.').Select(p => int.Parse(new string(p.TakeWhile(char.IsDigit).ToArray()) is { Length: > 0 } d ? d : "0")).ToArray();
        for (var i = 0; i < Math.Max(x.Length, y.Length); i++)
        {
            var c = (i < x.Length ? x[i] : 0).CompareTo(i < y.Length ? y[i] : 0);
            if (c != 0) return c;
        }
        return 0;
    }

    [GeneratedRegex(@"^(?<name>[A-Za-z0-9_.\-]+)(?<spec>.*)$")]
    private static partial Regex PinPattern();

    [GeneratedRegex(@"^(?<op>>=|<=|==|>|<)(?<v>[0-9][0-9.]*)$")]
    private static partial Regex ClausePattern();

    private static string ScriptPath()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "PythonScripts", "separate_track.py");
        Assert.True(File.Exists(path), path);
        return path;
    }

    private static string SourcePath(string folder, string file, [CallerFilePath] string? thisFile = null)
    {
        var path = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(thisFile!)!, "..", "..", "..",
            "src", "JuceSharp.StemSeparation", folder, file));
        Assert.True(File.Exists(path), path);
        return path;
    }

    /// <summary>The TEST's interpreter for exercising the script; the
    /// product never searches PATH.</summary>
    private static string? ResolveTestPython()
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
