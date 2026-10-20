using System.Net;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Tests for the resilient-download pipeline used by the embedded Python
/// bootstrap. All tests run offline against a local <see cref="HttpListener"/>
/// so they pass on machines without internet access.
/// </summary>
// Shares the collection that owns VerifiedDownloader's process-global
// RequestTimeout: a test that shortens the budget to prove a timeout is not a
// cancellation must not be running beside another class's live transfer.
[Collection("StemDownloads")]
public class DownloadResilienceTests : IDisposable
{
    private readonly HttpListener _listener = new();
    private readonly string _baseUrl;

    public DownloadResilienceTests()
    {
        // xunit builds a fresh instance PER TEST, so this bind runs once per
        // test in the class. Picking a random port out of a fixed 2000-wide
        // window threw HttpListenerException straight out of the constructor
        // whenever one was already held or sitting in TIME_WAIT — the test
        // ERRORED rather than retrying. Ask the OS for a genuinely free
        // loopback port instead, and retry the handoff race.
        // (HttpListener on 127.0.0.1 needs no admin rights and no firewall
        // rule, so the repo's 47810-47829 reservation does not apply here.)
        for (var attempt = 0; ; attempt++)
        {
            var probe = new System.Net.Sockets.TcpListener(IPAddress.Loopback, 0);
            probe.Start();
            var port = ((IPEndPoint)probe.LocalEndpoint).Port;
            probe.Stop();

            _baseUrl = $"http://127.0.0.1:{port}/";
            try
            {
                _listener.Prefixes.Clear();
                _listener.Prefixes.Add(_baseUrl);
                _listener.Start();
                return;
            }
            catch (HttpListenerException) when (attempt < 20)
            {
                // Someone grabbed the port between probe.Stop() and here.
            }
        }
    }

    public void Dispose()
    {
        try { _listener.Stop(); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        try { _listener.Close(); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
    }

    private static string TempRoot()
    {
        var p = Path.Combine(Path.GetTempPath(), "jucesharp-dl-" + Guid.NewGuid());
        Directory.CreateDirectory(p);
        return p;
    }

    private async Task ServeBytesOnce(byte[] payload)
    {
        var ctx = await _listener.GetContextAsync();
        ctx.Response.ContentLength64 = payload.LongLength;
        await ctx.Response.OutputStream.WriteAsync(payload);
        ctx.Response.OutputStream.Close();
    }

    private async Task ServeBytesPartialAndDie(byte[] payload, int bytesToSend)
    {
        var ctx = await _listener.GetContextAsync();
        ctx.Response.ContentLength64 = payload.LongLength;
        await ctx.Response.OutputStream.WriteAsync(payload.AsMemory(0, bytesToSend));
        // Kill the connection in the middle of the body so the client sees
        // a truncated stream rather than a clean EOF.
        ctx.Response.Abort();
    }

    private static Action<double> CancelOnProgress(CancellationTokenSource source) => _ => source.Cancel();

    [Fact]
    public async Task Download_writes_atomic_file_with_no_tmp_residue()
    {
        var root = TempRoot();
        try
        {
            var dest = Path.Combine(root, "payload.bin");
            var bytes = new byte[256 * 1024];
            new Random(7).NextBytes(bytes);
            var serve = ServeBytesOnce(bytes);

            var written = await PythonEnvManager.DownloadWithProgress(
                _baseUrl, dest, _ => { }, TestContext.Current.CancellationToken);
            await serve;

            Assert.Equal(bytes.LongLength, written);
            Assert.True(File.Exists(dest));
            Assert.Equal(bytes, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
            Assert.Empty(Directory.GetFiles(root, "*.tmp.*"));
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Download_aborted_midstream_leaves_no_destination_and_no_tmp()
    {
        var root = TempRoot();
        try
        {
            var dest = Path.Combine(root, "payload.bin");
            var bytes = new byte[1024 * 1024];
            new Random(11).NextBytes(bytes);
            var serve = ServeBytesPartialAndDie(bytes, bytes.Length / 2);

            await Assert.ThrowsAnyAsync<Exception>(() =>
                PythonEnvManager.DownloadWithProgress(_baseUrl, dest, _ => { }, TestContext.Current.CancellationToken));
            try { await serve; } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }

            Assert.False(File.Exists(dest), "Destination must not exist after a truncated download.");
            Assert.Empty(Directory.GetFiles(root, "*.tmp.*"));
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Download_cancelled_cleans_up_tmp_file()
    {
        var root = TempRoot();
        try
        {
            var dest = Path.Combine(root, "payload.bin");
            var bytes = new byte[1024 * 1024];
            new Random(13).NextBytes(bytes);
            var cts = new CancellationTokenSource();
            try
            {
                // Cancel as soon as the first progress tick arrives.
                var serve = ServeBytesOnce(bytes);
                var cancelOnProgress = CancelOnProgress(cts);
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                    PythonEnvManager.DownloadWithProgress(_baseUrl, dest, cancelOnProgress, cts.Token));
                try { await serve; } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }

                Assert.False(File.Exists(dest));
                Assert.Empty(Directory.GetFiles(root, "*.tmp.*"));
            }
            finally { cts.Dispose(); }
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Download_overwrites_existing_destination_atomically()
    {
        var root = TempRoot();
        try
        {
            var dest = Path.Combine(root, "payload.bin");
            await File.WriteAllBytesAsync(dest, [9, 9, 9, 9], TestContext.Current.CancellationToken);

            var bytes = new byte[1024];
            new Random(17).NextBytes(bytes);
            var serve = ServeBytesOnce(bytes);

            await PythonEnvManager.DownloadWithProgress(_baseUrl, dest, _ => { }, TestContext.Current.CancellationToken);
            await serve;

            Assert.Equal(bytes, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
            Assert.Empty(Directory.GetFiles(root, "*.tmp.*"));
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public void CleanStaleTempFiles_removes_orphans_at_construction()
    {
        var root = TempRoot();
        try
        {
            // Simulate a crashed download leaving a stale .tmp.* artefact.
            var orphan = Path.Combine(root, "python-3.11.9-embed.zip.tmp.deadbeef");
            File.WriteAllText(orphan, "garbage");
            Assert.True(File.Exists(orphan));

            _ = new PythonEnvManager(rootDirectory: root);

            Assert.False(File.Exists(orphan),
                "Constructor should sweep stale .tmp.* files left behind by a crashed boot.");
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public void CleanStaleTempFiles_returns_count_and_does_not_touch_real_files()
    {
        var root = TempRoot();
        try
        {
            File.WriteAllText(Path.Combine(root, "python.exe"), "stub");
            File.WriteAllText(Path.Combine(root, "a.tmp.001"), "x");
            File.WriteAllText(Path.Combine(root, "b.tmp.002"), "y");

            var mgr = new PythonEnvManager(rootDirectory: root);
            // Constructor already swept; calling again should remove zero.
            var removed = mgr.CleanStaleTempFiles();
            Assert.Equal(0, removed);
            Assert.True(File.Exists(Path.Combine(root, "python.exe")),
                "Real interpreter files must never be touched by the temp sweeper.");
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public void CleanStaleTempFiles_on_missing_root_returns_zero()
    {
        var ghostRoot = Path.Combine(Path.GetTempPath(), "jucesharp-dl-" + Guid.NewGuid());
        var mgr = new PythonEnvManager(rootDirectory: ghostRoot);
        Directory.Delete(ghostRoot, true);
        Assert.Equal(0, mgr.CleanStaleTempFiles());
    }

    [Fact]
    public async Task Download_404_throws_and_leaves_no_files()
    {
        var root = TempRoot();
        try
        {
            var dest = Path.Combine(root, "payload.bin");
            var serve = Task.Run(async () =>
            {
                var ctx = await _listener.GetContextAsync();
                ctx.Response.StatusCode = 404;
                ctx.Response.OutputStream.Close();
            }, TestContext.Current.CancellationToken);

            await Assert.ThrowsAnyAsync<HttpRequestException>(() =>
                PythonEnvManager.DownloadWithProgress(_baseUrl, dest, _ => { }, TestContext.Current.CancellationToken));
            await serve;

            Assert.False(File.Exists(dest));
            Assert.Empty(Directory.GetFiles(root, "*.tmp.*"));
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }

    [Fact]
    public async Task Download_reports_monotonic_progress()
    {
        var root = TempRoot();
        try
        {
            var dest = Path.Combine(root, "payload.bin");
            var bytes = new byte[8 * 1024 * 1024];
            new Random(23).NextBytes(bytes);
            var samples = new List<double>();
            var serve = ServeBytesOnce(bytes);

            await PythonEnvManager.DownloadWithProgress(_baseUrl, dest, p =>
            {
                lock (samples) samples.Add(p);
            }, TestContext.Current.CancellationToken);
            await serve;

            Assert.NotEmpty(samples);
            Assert.Equal(1.0, samples[^1], 3);
            for (int i = 1; i < samples.Count; i++)
                Assert.True(samples[i] >= samples[i - 1] - 1e-9,
                    $"Progress regressed at {i}: {samples[i - 1]} -> {samples[i]}");
        }
        finally { try { Directory.Delete(root, true); } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
    }
}
