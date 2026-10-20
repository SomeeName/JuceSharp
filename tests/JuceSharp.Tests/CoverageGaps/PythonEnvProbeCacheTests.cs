using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// <c>PythonEnvManager.Probe()</c> is documented as safe to call from a UI
/// gate (<c>StemSeparator.IsReady()</c> decides whether "Separate stems" is
/// enabled). It used to spawn <c>python --version</c> — synchronously, with a
/// 5 s timeout — on EVERY call, on whatever thread asked. It is now cached per
/// interpreter identity.
///
/// <para>
/// Shares the serialized StemSeparator collection because the cache and its
/// spawn counter are process-global.
/// </para>
/// </summary>
[Collection("StemSeparatorConcurrency")]
public sealed class PythonEnvProbeCacheTests : IDisposable
{
    private readonly List<string> _scratch = [];

    public void Dispose()
    {
        PythonEnvManager.ClearVersionCacheForTests();
        foreach (var d in _scratch)
        {
            try { Directory.Delete(d, true); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }

    /// <summary>A root holding a file called <c>python.exe</c> that is not a
    /// PE image: <c>ResolveLocalExecutable</c> finds it, the version probe
    /// tries to start it and fails. That is exactly the shape we need — the
    /// question is how MANY times it tries.
    /// <para>
    /// The <c>.interpreter-ready</c> stamp is part of the shape now: without
    /// it the root is a HALF-EXTRACT and is deliberately invisible, because
    /// "python.exe exists" is not "the interpreter is usable".
    /// </para></summary>
    private string RootWithFakeInterpreter()
    {
        var p = Path.Combine(Path.GetTempPath(), "jucesharp-probe-" + Guid.NewGuid());
        Directory.CreateDirectory(p);
        _scratch.Add(p);
        File.WriteAllText(Path.Combine(p, "python.exe"), "not a real interpreter");
        File.WriteAllText(Path.Combine(p, PythonEnvManager.InterpreterReadyStampName), "3.11.9");
        return p;
    }

    [Fact]
    public void Repeated_probes_of_one_interpreter_spawn_at_most_one_child_process()
    {
        if (!OperatingSystem.IsWindows())
        {
            Assert.Skip("The embedded interpreter layout probed here is Windows-only.");
            return;
        }

        PythonEnvManager.ClearVersionCacheForTests();
        var mgr = new PythonEnvManager(RootWithFakeInterpreter());

        var first = mgr.Probe();
        var spawnsAfterFirst = Volatile.Read(ref PythonEnvManager.VersionProbeSpawnCount);

        for (var i = 0; i < 20; i++)
        {
            var again = mgr.Probe();
            Assert.Equal(first, again);
        }

        Assert.Equal(1, spawnsAfterFirst);
        Assert.Equal(1, Volatile.Read(ref PythonEnvManager.VersionProbeSpawnCount));
        Assert.True(first.InterpreterPresent);
    }

    [Fact]
    public async Task ProbeAsync_agrees_with_Probe_and_shares_the_cache()
    {
        if (!OperatingSystem.IsWindows())
        {
            Assert.Skip("The embedded interpreter layout probed here is Windows-only.");
            return;
        }

        PythonEnvManager.ClearVersionCacheForTests();
        var mgr = new PythonEnvManager(RootWithFakeInterpreter());

        var async1 = await mgr.ProbeAsync(TestContext.Current.CancellationToken);
        var sync1 = mgr.Probe();

        Assert.Equal(async1, sync1);
        Assert.Equal(1, Volatile.Read(ref PythonEnvManager.VersionProbeSpawnCount));
    }

    /// <summary>Non-vacuity: a DIFFERENT interpreter is a different cache key,
    /// so the cache cannot be masking a probe that never happens.</summary>
    [Fact]
    public void A_second_interpreter_is_probed_on_its_own()
    {
        if (!OperatingSystem.IsWindows())
        {
            Assert.Skip("The embedded interpreter layout probed here is Windows-only.");
            return;
        }

        PythonEnvManager.ClearVersionCacheForTests();
        new PythonEnvManager(RootWithFakeInterpreter()).Probe();
        new PythonEnvManager(RootWithFakeInterpreter()).Probe();

        Assert.Equal(2, Volatile.Read(ref PythonEnvManager.VersionProbeSpawnCount));
    }

    /// <summary>A missing interpreter must not spawn anything at all, and
    /// must report every flag false.</summary>
    [Fact]
    public void An_absent_interpreter_probes_without_spawning()
    {
        PythonEnvManager.ClearVersionCacheForTests();
        var empty = Path.Combine(Path.GetTempPath(), "jucesharp-probe-empty-" + Guid.NewGuid());
        Directory.CreateDirectory(empty);
        _scratch.Add(empty);

        var status = new PythonEnvManager(empty).Probe();

        Assert.False(status.InterpreterPresent);
        Assert.False(status.HasPip);
        Assert.False(status.HasTorch);
        Assert.Null(status.Executable);
        Assert.Null(status.Version);
        Assert.Equal(0, Volatile.Read(ref PythonEnvManager.VersionProbeSpawnCount));
        Assert.False(new StemSeparator(new PythonEnvManager(empty)).IsReady());
    }
}
