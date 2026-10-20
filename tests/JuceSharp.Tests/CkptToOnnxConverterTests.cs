using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

public class CkptToOnnxConverterTests
{
    [Fact]
    public void Probe_returns_status_without_throwing()
    {
        var conv = new CkptToOnnxConverter(new PythonEnvManager(rootDirectory: TempRoot()));
        var status = conv.Probe();
        // Either the env hasn't been bootstrapped (interpreter absent) or it
        // has — both states are valid. We only assert that probing is safe.
        Assert.True(status.InterpreterPresent || !status.InterpreterPresent);
    }

    [Fact]
    public async Task ConvertAsync_throws_FileNotFound_for_missing_ckpt()
    {
        var conv = new CkptToOnnxConverter(new PythonEnvManager(rootDirectory: TempRoot()));
        var bogus = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".ckpt");
        var outOnnx = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".onnx");
        await Assert.ThrowsAsync<FileNotFoundException>(
            () => conv.ConvertAsync(bogus, null, outOnnx, progress: null, ct: TestContext.Current.CancellationToken));
    }

    [Fact]
    public void IsReady_returns_false_for_empty_root()
    {
        // A pristine env directory cannot have torch installed, so IsReady
        // must be false. This guards against regressions where someone
        // accidentally short-circuits the probe.
        var conv = new CkptToOnnxConverter(new PythonEnvManager(rootDirectory: TempRoot()));
        Assert.False(conv.IsReady());
    }

    [Fact]
    public void RequiredPackages_includes_torch_and_bs_roformer()
    {
        Assert.Contains(CkptToOnnxConverter.RequiredPackages, pkg =>
        {
            _ = pkg;
            return pkg.StartsWith("torch", StringComparison.OrdinalIgnoreCase);
        });
        Assert.Contains(CkptToOnnxConverter.RequiredPackages, pkg => pkg.Contains("bs_roformer", StringComparison.OrdinalIgnoreCase));
        Assert.Contains(CkptToOnnxConverter.RequiredPackages, pkg => pkg.StartsWith("onnx", StringComparison.OrdinalIgnoreCase));
    }

    private static string TempRoot()
    {
        var p = Path.Combine(Path.GetTempPath(), "jucesharp-py-" + Guid.NewGuid());
        Directory.CreateDirectory(p);
        return p;
    }
}

public class PythonEnvManagerTests
{
    [Fact]
    public void Probe_returns_not_present_for_pristine_root()
    {
        var root = Path.Combine(Path.GetTempPath(), "jucesharp-py-" + Guid.NewGuid());
        try
        {
            var mgr = new PythonEnvManager(rootDirectory: root);
            var s = mgr.Probe();
            Assert.False(s.InterpreterPresent);
            Assert.False(s.HasPip);
            Assert.False(s.HasTorch);
            Assert.Null(s.Executable);
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public void Root_is_created_if_missing()
    {
        var root = Path.Combine(Path.GetTempPath(), "jucesharp-py-" + Guid.NewGuid());
        try
        {
            _ = new PythonEnvManager(rootDirectory: root);
            Assert.True(Directory.Exists(root));
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public void SitePackages_resolves_under_root()
    {
        var root = Path.Combine(Path.GetTempPath(), "jucesharp-py-" + Guid.NewGuid());
        try
        {
            var mgr = new PythonEnvManager(rootDirectory: root);
            Assert.StartsWith(root, mgr.SitePackages);
            Assert.Contains("site-packages", mgr.SitePackages);
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task EnsurePackagesAsync_with_empty_list_is_a_noop()
    {
        var root = Path.Combine(Path.GetTempPath(), "jucesharp-py-" + Guid.NewGuid());
        try
        {
            var mgr = new PythonEnvManager(rootDirectory: root);
            // Empty enumerable must short-circuit before touching the network
            // or trying to bootstrap pip — otherwise we'd hang offline tests.
            await mgr.EnsurePackagesAsync(Array.Empty<string>(), progress: null, ct: TestContext.Current.CancellationToken);
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public void DefaultPythonVersion_is_3_11_or_higher()
    {
        var v = PythonEnvManager.DefaultPythonVersion.Split('.');
        Assert.True(int.Parse(v[0]) >= 3);
        Assert.True(int.Parse(v[1]) >= 11);
    }
}
