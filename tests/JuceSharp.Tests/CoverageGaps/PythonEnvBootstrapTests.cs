using JuceSharp;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The embedded-Python bootstrap invariants: it is serialized across
/// processes, it runs pip at most once per pinned package set, it verifies
/// what it downloads, and probing it is cheap enough for a UI gate.
///
/// <para>
/// Nothing here touches the network or installs anything: the download tests
/// serve their own bytes from a loopback socket, and the pip tests drive the
/// early-out paths only.
/// </para>
/// </summary>
[Collection("StemDownloads")]
public sealed class PythonEnvBootstrapTests : IDisposable
{
    private readonly List<string> _scratch = [];

    private string TempRoot()
    {
        var p = Path.Combine(Path.GetTempPath(), "jucesharp-env-" + Guid.NewGuid());
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

    // ---------------------------------------------------------------- D6

    /// <summary>
    /// Two managers rooted at the same directory must not bootstrap at once.
    /// Before the gate existed, a StemSeparator and a CkptToOnnxConverter —
    /// which each DEFAULT-CONSTRUCT their own manager on the same default
    /// root — could run two <c>pip install torch</c> processes into one
    /// site-packages tree.
    /// </summary>
    [Fact]
    public async Task Two_managers_on_one_root_never_hold_the_bootstrap_gate_together()
    {
        var root = TempRoot();
        var a = new PythonEnvManager(root);
        var b = new PythonEnvManager(root);
        Assert.Equal(a.Root, b.Root);
        var ct = TestContext.Current.CancellationToken;

        // Four holders, and each is held INSIDE until all four have announced
        // they are about to acquire. That is what makes the number mean
        // something: an unguarded implementation puts all four inside at once
        // and the high-water mark is 4, deterministically. (This used to hold
        // each entrant for a fixed 75 ms and hope the schedules overlapped —
        // which is a test that passes whether or not the gate is there.)
        var (entries, maxInside) = await MaxConcurrentHoldersAsync(a.Root, 4, ct);

        Assert.Equal(4, entries);
        Assert.Equal(1, maxInside);
    }

    /// <summary>
    /// Runs <paramref name="holders"/> acquisitions of the bootstrap gate for
    /// <paramref name="root"/> concurrently and returns how many entered and
    /// how many were ever inside at the same moment.
    ///
    /// <para>
    /// No clock anywhere: the "everybody has started" signal is raised before
    /// any holder acquires, and each holder waits for it from INSIDE, so a gate
    /// that excludes yields 1 and a gate that does not yields
    /// <paramref name="holders"/> — neither outcome depends on scheduling.
    /// </para>
    /// </summary>
    private static async Task<(int Entries, int MaxInside)> MaxConcurrentHoldersAsync(
        string root, int holders, CancellationToken ct)
    {
        var inside = 0;
        var maxInside = 0;
        var entries = 0;
        var remainingToStart = holders;
        var allStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        async Task Holder()
        {
            if (Interlocked.Decrement(ref remainingToStart) == 0) allStarted.SetResult();
            using var gate = await PythonEnvManager.AcquireEnvLockAsync(root, ct);
            var now = Interlocked.Increment(ref inside);
            Interlocked.Increment(ref entries);
            await allStarted.Task.WaitAsync(ct);
            int seen;
            do { seen = Volatile.Read(ref maxInside); }
            while (now > seen && Interlocked.CompareExchange(ref maxInside, now, seen) != seen);
            Interlocked.Decrement(ref inside);
        }

        await Task.WhenAll(Enumerable.Range(0, holders).Select(_ => Task.Run(Holder, ct)));
        return (entries, Volatile.Read(ref maxInside));
    }

    /// <summary>
    /// A double dispose must not OVER-release the kernel semaphore: one extra
    /// release raises its count to 2 and two bootstraps get in from then on.
    /// Asserted the only way that shows — by measuring exclusion afterwards,
    /// rather than by observing that nothing threw.
    /// </summary>
    [Fact]
    public async Task The_bootstrap_gate_is_reacquirable_and_a_double_dispose_does_not_over_release_it()
    {
        var root = TempRoot();
        var ct = TestContext.Current.CancellationToken;
        (await PythonEnvManager.AcquireEnvLockAsync(root, ct)).Dispose();
        (await PythonEnvManager.AcquireEnvLockAsync(root, ct)).Dispose();

        var h = await PythonEnvManager.AcquireEnvLockAsync(root, ct);
        h.Dispose();
        h.Dispose();

        var (entries, maxInside) = await MaxConcurrentHoldersAsync(root, 3, ct);
        Assert.Equal(3, entries);
        Assert.Equal(1, maxInside);
    }

    /// <summary>
    /// W4-S18 (second half). The process-local half of the bootstrap gate was a
    /// static <c>ConcurrentDictionary</c> keyed by root path and never pruned,
    /// so a host that bootstraps into per-project roots grew one semaphore per
    /// root for the life of the process. The entry is refcounted now, exactly
    /// like <c>VerifiedDownloader.DestinationGates</c>: it exists while someone
    /// holds or waits on it, and is gone once the last handle is disposed —
    /// including a waiter that was cancelled while queued.
    /// </summary>
    [Fact]
    public async Task A_released_bootstrap_gate_is_not_kept_for_ever()
    {
        var ct = TestContext.Current.CancellationToken;
        var before = PythonEnvManager.LocalEnvGateCount;

        var rootA = TempRoot();
        var rootB = TempRoot();
        var a = await PythonEnvManager.AcquireEnvLockAsync(rootA, ct);
        var b = await PythonEnvManager.AcquireEnvLockAsync(rootB, ct);
        Assert.Equal(before + 2, PythonEnvManager.LocalEnvGateCount);

        // A waiter cancelled while queued must drop its reference too.
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var waiter = PythonEnvManager.AcquireEnvLockAsync(rootA, cts.Token);
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => waiter);
        Assert.Equal(before + 2, PythonEnvManager.LocalEnvGateCount);

        a.Dispose();
        a.Dispose(); // a double dispose must not double-unreference
        Assert.Equal(before + 1, PythonEnvManager.LocalEnvGateCount);
        b.Dispose();
        Assert.Equal(before, PythonEnvManager.LocalEnvGateCount);

        // …and the gate still serializes after having been dropped and re-created.
        var (entries, maxInside) = await MaxConcurrentHoldersAsync(rootA, 3, ct);
        Assert.Equal(3, entries);
        Assert.Equal(1, maxInside);
        Assert.Equal(before, PythonEnvManager.LocalEnvGateCount);
    }

    [Fact]
    public async Task Different_roots_do_not_block_each_other()
    {
        var ct = TestContext.Current.CancellationToken;
        using var first = await PythonEnvManager.AcquireEnvLockAsync(TempRoot(), ct);
        // Would deadlock (until the test host's own timeout) if the gate were
        // global rather than per-root.
        using var second = await PythonEnvManager.AcquireEnvLockAsync(TempRoot(), ct);
        Assert.NotSame(first, second);
    }

    // ---------------------------------------------------------------- D7

    [Fact]
    public void The_package_stamp_name_is_pin_sensitive_and_order_sensitive()
    {
        string[] pins = ["torch>=2.1,<2.6", "numpy<2"];
        var baseline = PythonEnvManager.PackageSetStampFileName(pins);

        Assert.StartsWith(".packages-", baseline, StringComparison.Ordinal);
        Assert.EndsWith(".stamp", baseline, StringComparison.Ordinal);
        // Stable for the same set…
        Assert.Equal(baseline, PythonEnvManager.PackageSetStampFileName(["torch>=2.1,<2.6", "numpy<2"]));
        // …and different the moment any pin moves, is added, or is removed.
        Assert.NotEqual(baseline, PythonEnvManager.PackageSetStampFileName(["torch>=2.1,<2.7", "numpy<2"]));
        Assert.NotEqual(baseline, PythonEnvManager.PackageSetStampFileName(["torch>=2.1,<2.6"]));
        Assert.NotEqual(baseline, PythonEnvManager.PackageSetStampFileName(["torch>=2.1,<2.6", "numpy<2", "onnx>=1.15"]));
        // Whitespace-only differences do not invalidate it.
        Assert.Equal(baseline, PythonEnvManager.PackageSetStampFileName([" torch>=2.1,<2.6 ", "numpy<2", "  "]));
    }

    /// <summary>
    /// With the stamp present, <c>EnsurePackagesAsync</c> returns before it
    /// does ANY work — proven by handing it an already-cancelled token, which
    /// the very next step (<c>EnsureInterpreterAsync</c>) would honour. Before
    /// the stamp existed, every separation shelled out to
    /// <c>pip install torch …</c>.
    /// </summary>
    [Fact]
    public async Task EnsurePackages_is_a_no_op_once_the_stamp_for_that_pin_set_exists()
    {
        var root = TempRoot();
        var pins = StemSeparator.RequiredPackages;
        FakeInstalledEnvironment(root, pins);
        var mgr = new PythonEnvManager(root);
        File.WriteAllText(Path.Combine(root, PythonEnvManager.PackageSetStampFileName(pins)), "installed");

        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        // It returns before it does ANY work — proven by the already-cancelled
        // token, which the very next step (EnsureInterpreterAsync) honours —
        // and it SAYS so: exactly one sample, the terminal one. Asserting the
        // observable output rather than the absence of a throw is the
        // difference between this and the test that stood here before.
        var samples = new List<PythonInstallProgress>();
        await mgr.EnsurePackagesAsync(pins, new ImmediateProgress<PythonInstallProgress>(samples.Add), cts.Token);

        Assert.Equal("pip-installed", Assert.Single(samples).Phase);
        Assert.Equal(1.0, samples[0].Fraction);
        // Nothing was bootstrapped: the fast path never assigns Executable.
        Assert.Equal(string.Empty, mgr.Executable);
    }

    /// <summary>
    /// A root that looks installed to every file-system check the manager
    /// makes: a stamped interpreter and one site-packages directory per pin.
    /// No python is executed by any test in this file.
    /// </summary>
    private static void FakeInstalledEnvironment(string root, IEnumerable<string> pins)
    {
        if (!OperatingSystem.IsWindows())
            Assert.Skip("The embedded-interpreter layout this asserts is the Windows one.");
        File.WriteAllText(Path.Combine(root, "python.exe"), "not really python");
        File.WriteAllText(Path.Combine(root, PythonEnvManager.InterpreterReadyStampName), "3.11.9");
        var site = Path.Combine(root, "Lib", "site-packages");
        Directory.CreateDirectory(site);
        foreach (var pin in pins)
        {
            if (PythonPackagePins.InstalledDirectories.TryGetValue(pin.Trim(), out var dir))
                Directory.CreateDirectory(Path.Combine(site, dir));
        }
    }

    /// <summary>
    /// The stamp records a pin SET and nothing about the tree those pins were
    /// installed into. With <c>site-packages/torch</c> deleted by hand, by a
    /// failed uninstall or by an antivirus quarantine, the stamp used to keep
    /// pip from EVER running again — every separation then failed with a Python
    /// <c>ImportError</c> for ever, while the file-system check knew the answer
    /// the whole time.
    /// </summary>
    [Fact]
    public async Task A_stamp_whose_packages_are_gone_does_not_count_as_installed()
    {
        var root = TempRoot();
        var pins = StemSeparator.RequiredPackages;
        FakeInstalledEnvironment(root, pins);
        var mgr = new PythonEnvManager(root);
        var stamp = Path.Combine(root, PythonEnvManager.PackageSetStampFileName(pins));
        File.WriteAllText(stamp, "installed");

        Directory.Delete(Path.Combine(root, "Lib", "site-packages", "torch"));

        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        // No longer a no-op: it proceeds to the bootstrap, which honours the
        // token. And the stamp that lied is gone, so the decision is not
        // re-made on every launch once the reinstall succeeds.
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => mgr.EnsurePackagesAsync(pins, progress: null, cts.Token));
        Assert.False(File.Exists(stamp));
    }

    /// <summary>Non-vacuity partner: with the tree intact the same stamp IS
    /// honoured. Without this, the test above would pass just as happily for
    /// an implementation that ignored every stamp and ran pip every launch.</summary>
    [Fact]
    public async Task A_stamp_whose_packages_are_present_still_counts_as_installed()
    {
        var root = TempRoot();
        var pins = StemSeparator.RequiredPackages;
        FakeInstalledEnvironment(root, pins);
        var mgr = new PythonEnvManager(root);
        var stamp = Path.Combine(root, PythonEnvManager.PackageSetStampFileName(pins));
        File.WriteAllText(stamp, "installed");

        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        await mgr.EnsurePackagesAsync(pins, progress: null, cts.Token);
        Assert.True(File.Exists(stamp));
    }

    /// <summary>Non-vacuity partner for the test above: without the stamp the
    /// same call reaches the bootstrap and honours the token (it does NOT
    /// silently return).</summary>
    [Fact]
    public async Task EnsurePackages_proceeds_to_the_bootstrap_when_no_stamp_exists()
    {
        var root = TempRoot();
        var mgr = new PythonEnvManager(root);

        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => mgr.EnsurePackagesAsync(StemSeparator.RequiredPackages, progress: null, cts.Token));
    }

    /// <summary>A stamp for a DIFFERENT pin set must not satisfy this one.</summary>
    [Fact]
    public async Task A_stamp_for_another_pin_set_does_not_count()
    {
        var root = TempRoot();
        var mgr = new PythonEnvManager(root);
        File.WriteAllText(
            Path.Combine(root, PythonEnvManager.PackageSetStampFileName(["something-else==1.0"])), "installed");

        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => mgr.EnsurePackagesAsync(StemSeparator.RequiredPackages, progress: null, cts.Token));
    }

    /// <summary>The stale-temp sweep must not eat the stamp.</summary>
    [Fact]
    public void CleanStaleTempFiles_does_not_remove_a_package_stamp()
    {
        var root = TempRoot();
        var stamp = Path.Combine(root, PythonEnvManager.PackageSetStampFileName(["torch>=2.1,<2.6"]));
        File.WriteAllText(stamp, "installed");
        File.WriteAllText(Path.Combine(root, "python.zip.tmp.abc123"), "partial");

        var mgr = new PythonEnvManager(root);
        Assert.True(File.Exists(stamp));
        Assert.Equal(0, mgr.CleanStaleTempFiles());   // ctor already swept the .tmp.
        Assert.True(File.Exists(stamp));
    }

    // ---------------------------------------------------------------- D8

    [Fact]
    public async Task A_download_whose_hash_does_not_match_is_discarded_and_reported()
    {
        var payload = "not the python you are looking for"u8.ToArray();
        using var server = LoopbackFileServer.Start(payload);
        var dest = Path.Combine(TempRoot(), "python-embed.zip");

        var ex = await Assert.ThrowsAsync<InvalidDataException>(() =>
            PythonEnvManager.DownloadWithProgress(
                server.Url, dest, _ => { }, TestContext.Current.CancellationToken,
                expectedSha256: new string('0', 64)));

        Assert.Contains("Checksum mismatch", ex.Message, StringComparison.Ordinal);
        // The whole point: nothing that failed its pin is left on disk where
        // the next boot could unzip and execute it.
        Assert.False(File.Exists(dest));
        Assert.Empty(Directory.GetFiles(Path.GetDirectoryName(dest)!));
    }

    [Fact]
    public async Task A_download_whose_hash_matches_lands_intact()
    {
        var payload = "the real payload"u8.ToArray();
        var sha = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(payload));
        using var server = LoopbackFileServer.Start(payload);
        var dest = Path.Combine(TempRoot(), "payload.bin");

        var written = await PythonEnvManager.DownloadWithProgress(
            server.Url, dest, _ => { }, TestContext.Current.CancellationToken, expectedSha256: sha);

        Assert.Equal(payload.Length, written);
        Assert.True(File.Exists(dest));
        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
        // Lower-case hex must verify too.
        await PythonEnvManager.VerifyFileHashAsync(dest, sha.ToLowerInvariant(), "test",
            TestContext.Current.CancellationToken);
    }

    [Fact]
    public async Task A_null_expectation_skips_verification()
    {
        var dest = Path.Combine(TempRoot(), "unpinned.bin");
        await File.WriteAllBytesAsync(dest, [1, 2, 3], TestContext.Current.CancellationToken);
        await PythonEnvManager.VerifyFileHashAsync(dest, null, "test", TestContext.Current.CancellationToken);
        Assert.True(File.Exists(dest));
    }

    /// <summary>
    /// The pin itself. python.org publishes MD5 for release artefacts; this
    /// SHA-256 was derived from the byte-identical archive (see the doc
    /// comment on the constant). Guarding the SHAPE catches the far more
    /// likely regression: someone bumps <c>DefaultPythonVersion</c> and
    /// leaves the hash behind.
    /// </summary>
    [Fact]
    public void The_embedded_python_archive_is_pinned_by_default()
    {
        Assert.Equal("3.11.9", PythonEnvManager.DefaultPythonVersion);
        Assert.Equal(64, PythonEnvManager.DefaultPythonEmbedSha256.Length);
        Assert.Matches("^[0-9a-f]{64}$", PythonEnvManager.DefaultPythonEmbedSha256);
        Assert.Equal("009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b",
            PythonEnvManager.DefaultPythonEmbedSha256);
        Assert.Equal(PythonEnvManager.DefaultPythonEmbedSha256, new PythonEnvManager(TempRoot()).PythonEmbedSha256);
    }
}
