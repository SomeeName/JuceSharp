using System.Security.Cryptography;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// What the resumable download path promises about a server's answer (W4-S6),
/// what holds the destination while it decides (W4-S17), what it leaves behind
/// (W4-S14), and what it does NOT leak (W4-S18).
/// <para>Every transfer is a <see cref="LoopbackFileServer"/> on 127.0.0.1;
/// nothing here touches the network. Serialised with the other download classes
/// because <c>VerifiedDownloader</c>'s gate table is process-global.</para>
/// </summary>
[Collection("StemDownloads")]
public sealed class DownloadResumeContractTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), $"jsrc-{Guid.NewGuid():N}");

    public DownloadResumeContractTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
    }

    private static byte[] Payload(int length = 32 * 1024)
    {
        var bytes = new byte[length];
        for (var i = 0; i < length; i++) bytes[i] = (byte)((i * 17 + 3) & 0xFF);
        return bytes;
    }

    private static string Sha256Of(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    // ---------------------------------------------------------------- W4-S6

    /// <summary>
    /// A 206 whose <c>Content-Range</c> starts at 0 — a caching proxy that
    /// clamped our range — is the WHOLE body. Appending it to the prefix
    /// produced a file of <c>prefix + whole</c> bytes, which with a pinned hash
    /// is a wasted transfer and with <c>sha256: null</c> is a corrupt file that
    /// then gets loaded or executed.
    /// </summary>
    [Fact]
    public async Task A_206_that_did_not_honour_our_range_restarts_instead_of_appending()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload, new LoopbackServerOptions
        {
            ClampRangeToWholeBody = true,
        });

        var dest = Path.Combine(_dir, "model.bin");
        // A prefix from an earlier attempt, which is what makes the client resume.
        var part = VerifiedDownloader.PartialPathFor(server.Url, dest);
        await File.WriteAllBytesAsync(part, payload[..4096], TestContext.Current.CancellationToken);

        // No pinned hash: the case where nothing downstream would catch it.
        var written = await VerifiedDownloader.DownloadAsync(
            server.Url, dest, progress: null, TestContext.Current.CancellationToken,
            expectedSha256: null, allowResume: true);

        Assert.Equal(payload.Length, written);
        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
        // It has to have ASKED to resume first — otherwise this passes for the
        // wrong reason (a client that never resumes at all).
        Assert.Contains(server.ReceivedRangeHeaders, h => h is not null);
    }

    [Fact]
    public async Task A_206_that_did_honour_our_range_still_resumes()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload, new LoopbackServerOptions
        {
            SupportsRange = true,
        });

        var dest = Path.Combine(_dir, "model.bin");
        var part = VerifiedDownloader.PartialPathFor(server.Url, dest);
        await File.WriteAllBytesAsync(part, payload[..4096], TestContext.Current.CancellationToken);

        var written = await VerifiedDownloader.DownloadAsync(
            server.Url, dest, progress: null, TestContext.Current.CancellationToken,
            expectedSha256: Sha256Of(payload), allowResume: true);

        Assert.Equal(payload.Length, written);
        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
        Assert.Contains(server.ReceivedRangeHeaders, h => h is not null && h.Contains("4096", StringComparison.Ordinal));
    }

    // --------------------------------------------------------------- W4-S17

    /// <summary>
    /// The gate has to span the EXISTENCE CHECK, not just the transfer:
    /// <c>EnsureFileAsync</c>'s hash-and-delete step could otherwise remove the
    /// canonical file while another caller was mid-hash on it, or had just
    /// returned it as verified.
    /// <para>The assertion is one-directional — the call completing while the
    /// gate is held is the failure, and no amount of load can produce it.</para>
    /// </summary>
    [Fact]
    public async Task EnsureFile_waits_for_the_destination_gate_before_it_inspects_the_file()
    {
        var payload = Payload();
        var dest = Path.Combine(_dir, "already-here.bin");
        await File.WriteAllBytesAsync(dest, payload, TestContext.Current.CancellationToken);

        using var server = LoopbackFileServer.Start(payload);
        var held = await VerifiedDownloader.AcquireDestinationGateAsync(dest, TestContext.Current.CancellationToken);
        try
        {
            // Present and matching: without the gate this returns immediately.
            var call = VerifiedDownload.EnsureFileAsync(
                new Uri(server.Url), dest, Sha256Of(payload), payload.Length,
                ct: TestContext.Current.CancellationToken);

            var first = await Task.WhenAny(call, Task.Delay(TimeSpan.FromMilliseconds(250),
                                                            TestContext.Current.CancellationToken));
            Assert.NotSame(call, first);

            held.Dispose();
            Assert.Equal(dest, await call);
        }
        finally
        {
            held.Dispose();
        }
    }

    [Fact]
    public async Task EnsureFile_can_refuse_an_unpinned_fetch_when_the_caller_asks_it_to()
    {
        using var server = LoopbackFileServer.Start(Payload());
        var dest = Path.Combine(_dir, "unpinned.bin");

        await Assert.ThrowsAsync<InvalidOperationException>(() => VerifiedDownload.EnsureFileAsync(
            new Uri(server.Url), dest, sha256: null, ct: TestContext.Current.CancellationToken,
            requireHash: true));

        Assert.False(File.Exists(dest));

        // …and the default is unchanged: an unpinned fetch still warns and proceeds,
        // because the get-pip bootstrap has nothing to pin.
        var warnings = new List<string>();
        var path = await VerifiedDownload.EnsureFileAsync(
            new Uri(server.Url), dest, sha256: null, log: warnings.Add,
            ct: TestContext.Current.CancellationToken);
        Assert.True(File.Exists(path));
        Assert.Contains(warnings, w => w.Contains("no pinned SHA-256", StringComparison.Ordinal));
    }

    // --------------------------------------------------------------- W4-S14

    [Fact]
    public void CleanPartials_removes_an_abandoned_prefix_and_spares_a_live_one()
    {
        var abandoned = Path.Combine(_dir, "model.bin.deadbeef" + VerifiedDownloader.PartialSuffix);
        var live = Path.Combine(_dir, "other.bin.cafebabe" + VerifiedDownloader.PartialSuffix);
        File.WriteAllBytes(abandoned, [1, 2, 3]);
        File.WriteAllBytes(live, [4, 5, 6]);
        var untouched = Path.Combine(_dir, "model.bin");
        File.WriteAllBytes(untouched, [7]);

        // Exactly the handle VerifiedDownloader holds while it is writing.
        using (new FileStream(live, FileMode.Open, FileAccess.Write, FileShare.None))
        {
            var removed = VerifiedDownload.CleanPartials(_dir, TimeSpan.Zero);
            Assert.Equal(1, removed);
        }

        Assert.False(File.Exists(abandoned));
        Assert.True(File.Exists(live), "an in-flight download's prefix was deleted under it.");
        Assert.True(File.Exists(untouched), "a finished file was swept.");
    }

    [Fact]
    public void CleanPartials_is_zero_for_a_directory_that_does_not_exist()
        => Assert.Equal(0, VerifiedDownload.CleanPartials(Path.Combine(_dir, "nope"), TimeSpan.Zero));

    // --------------------------------------------------------------- W4-S18

    [Fact]
    public async Task A_released_destination_gate_is_not_kept_for_ever()
    {
        var before = VerifiedDownloader.DestinationGateCount;
        for (var i = 0; i < 25; i++)
        {
            var gate = await VerifiedDownloader.AcquireDestinationGateAsync(
                Path.Combine(_dir, $"song-{i}", "vocals.bin"), TestContext.Current.CancellationToken);
            gate.Dispose();
        }

        Assert.Equal(before, VerifiedDownloader.DestinationGateCount);
    }

    [Fact]
    public async Task A_held_destination_gate_still_excludes_a_second_caller()
    {
        var dest = Path.Combine(_dir, "contended.bin");
        var first = await VerifiedDownloader.AcquireDestinationGateAsync(dest, TestContext.Current.CancellationToken);
        try
        {
            var second = VerifiedDownloader.AcquireDestinationGateAsync(dest, TestContext.Current.CancellationToken);
            var winner = await Task.WhenAny(second, Task.Delay(TimeSpan.FromMilliseconds(250),
                                                               TestContext.Current.CancellationToken));
            Assert.NotSame(second, winner);

            first.Dispose();
            (await second).Dispose();
        }
        finally
        {
            first.Dispose();
        }
    }
}
