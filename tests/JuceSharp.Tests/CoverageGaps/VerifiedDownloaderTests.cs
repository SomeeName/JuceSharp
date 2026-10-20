using System.Security.Cryptography;
using JuceSharp.StemSeparation.Onnx;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The verified-download path: the shared <c>VerifiedDownloader</c> and the
/// <see cref="OnnxModelCatalog"/> on top of it.
///
/// <para>
/// Nothing here touches the real network or a real model: every transfer goes
/// through <see cref="LoopbackFileServer"/> on 127.0.0.1, and the "model" is a
/// few kilobytes of deterministic bytes whose real SHA-256 the test computes.
/// </para>
/// </summary>
[Collection("StemDownloads")]
public sealed class VerifiedDownloaderTests : IDisposable
{
    private readonly string _dir = Path.Combine(
        Path.GetTempPath(), $"jsdl-{Guid.NewGuid():N}");

    public VerifiedDownloaderTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
    }

    /// <summary>A payload whose every byte is a function of its index, so a
    /// resumed file that spliced the wrong bytes in is detectable as such and
    /// not merely as "the hash differs".</summary>
    private static byte[] Payload(int length = 64 * 1024)
    {
        var bytes = new byte[length];
        for (var i = 0; i < length; i++) bytes[i] = (byte)((i * 31 + 7) & 0xFF);
        return bytes;
    }

    private static string Sha256Of(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    // ------------------------------------------------- VerifiedDownloader

    [Fact]
    public async Task A_fresh_download_lands_verified_and_leaves_no_partial()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var dest = Path.Combine(_dir, "model.onnx");

        var samples = new List<DownloadProgress>();
        var written = await VerifiedDownloader.DownloadAsync(
            server.Url, dest, samples.Add, TestContext.Current.CancellationToken,
            Sha256Of(payload), allowResume: true);

        Assert.Equal(payload.Length, written);
        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
        Assert.False(File.Exists(VerifiedDownloader.PartialPathFor(server.Url, dest)),
            "the partial file must be gone after a clean finish");
        Assert.Equal(1.0, samples[^1].Fraction, 6);
        Assert.All(samples, s => Assert.False(s.Resumed));
    }

    /// <summary>
    /// The whole point of hashing BEFORE the rename: a destination file must
    /// never come into existence holding bytes nobody authenticated, because
    /// the next thing that happens to it is that it is loaded and run.
    /// </summary>
    [Fact]
    public async Task A_wrong_hash_throws_and_leaves_no_file_at_the_destination()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var dest = Path.Combine(_dir, "model.onnx");
        var wrong = new string('a', 64);

        await Assert.ThrowsAsync<InvalidDataException>(() => VerifiedDownloader.DownloadAsync(
            server.Url, dest, null, TestContext.Current.CancellationToken, wrong, allowResume: true));

        Assert.False(File.Exists(dest));
        // ...and the bad prefix is not kept as something to "resume": those
        // bytes are known-wrong, not merely incomplete.
        Assert.False(File.Exists(VerifiedDownloader.PartialPathFor(server.Url, dest)));
    }

    /// <summary>
    /// A truncated <c>.part</c> is CONTINUED, not re-fetched, when the server
    /// honours Range — and the finished file is verified WHOLE, because the
    /// bytes it kept came from an earlier, unverified attempt.
    /// </summary>
    [Fact]
    public async Task A_truncated_partial_is_resumed_over_http_range()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload, new LoopbackServerOptions { SupportsRange = true });
        var dest = Path.Combine(_dir, "model.onnx");

        const int have = 20_000;
        await File.WriteAllBytesAsync(VerifiedDownloader.PartialPathFor(server.Url, dest), payload[..have],
            TestContext.Current.CancellationToken);

        var samples = new List<DownloadProgress>();
        var written = await VerifiedDownloader.DownloadAsync(
            server.Url, dest, samples.Add, TestContext.Current.CancellationToken,
            Sha256Of(payload), allowResume: true);

        Assert.Equal(payload.Length, written);
        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));

        // It really RESUMED: it asked for a range, it said so on every sample,
        // and it started counting from the bytes it already had.
        Assert.Equal($"bytes={have}-", Assert.Single(server.ReceivedRangeHeaders));
        Assert.All(samples, s => Assert.True(s.Resumed));
        Assert.Equal(have, samples[0].BytesReceived);
    }

    /// <summary>
    /// A server that IGNORES <c>Range</c> answers 200 with the whole body.
    /// Appending that to the prefix would double the file and corrupt it
    /// silently, so the transfer must restart from byte 0. The 206-vs-200
    /// status is the authority here, never the advertised
    /// <c>Accept-Ranges</c> header — a header is a claim, the status is what
    /// the server actually did.
    /// </summary>
    [Fact]
    public async Task A_server_that_ignores_range_makes_the_download_restart_whole()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload, new LoopbackServerOptions { SupportsRange = false });
        var dest = Path.Combine(_dir, "model.onnx");

        await File.WriteAllBytesAsync(VerifiedDownloader.PartialPathFor(server.Url, dest), payload[..20_000],
            TestContext.Current.CancellationToken);

        var samples = new List<DownloadProgress>();
        var written = await VerifiedDownloader.DownloadAsync(
            server.Url, dest, samples.Add, TestContext.Current.CancellationToken,
            Sha256Of(payload), allowResume: true);

        // Not 20_000 + payload.Length, and not a corrupt file.
        Assert.Equal(payload.Length, written);
        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
        Assert.Equal($"bytes=20000-", Assert.Single(server.ReceivedRangeHeaders));
        Assert.All(samples, s => Assert.False(s.Resumed));
    }

    /// <summary>
    /// A cancel mid-transfer is an <see cref="OperationCanceledException"/> and
    /// KEEPS the prefix — that is what makes the next attempt cheap. Made
    /// deterministic by a server that stops mid-body and waits.
    /// </summary>
    [Fact]
    public async Task Cancelling_mid_download_keeps_the_partial_for_the_next_attempt()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload,
            new LoopbackServerOptions { SupportsRange = true, StallAfterBytes = 16_384 });
        var dest = Path.Combine(_dir, "model.onnx");

        using var cts = new CancellationTokenSource();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => VerifiedDownloader.DownloadAsync(
            server.Url, dest,
            p => { if (p.BytesReceived > 0) cts.Cancel(); },
            cts.Token, Sha256Of(payload), allowResume: true));

        var part = VerifiedDownloader.PartialPathFor(server.Url, dest);
        Assert.False(File.Exists(dest), "a cancelled transfer must not produce a destination file");
        Assert.True(File.Exists(part), "the prefix must be kept so the next attempt can resume");
        var kept = await File.ReadAllBytesAsync(part, TestContext.Current.CancellationToken);
        Assert.NotEmpty(kept);
        // Whatever arrived is a genuine PREFIX of the payload — a resume that
        // starts from its length is therefore correct.
        Assert.Equal(payload[..kept.Length], kept);
    }

    /// <summary>
    /// The non-resumable policy is the OTHER one, and it must stay: a
    /// half-formed Python archive left on disk is a trap for the next boot.
    /// </summary>
    [Fact]
    public async Task A_cancelled_non_resumable_download_leaves_nothing_behind()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload,
            new LoopbackServerOptions { StallAfterBytes = 16_384 });
        var dest = Path.Combine(_dir, "python-embed.zip");

        using var cts = new CancellationTokenSource();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => VerifiedDownloader.DownloadAsync(
            server.Url, dest,
            p => { if (p.BytesReceived > 0) cts.Cancel(); },
            cts.Token, Sha256Of(payload), allowResume: false));

        Assert.False(File.Exists(dest));
        Assert.False(File.Exists(VerifiedDownloader.PartialPathFor(server.Url, dest)));
        Assert.Empty(Directory.GetFiles(_dir, "python-embed.zip.tmp.*"));
    }

    // -------------------------------------------------- OnnxModelCatalog

    private static OnnxModelEntry EntryFor(string url, byte[] payload, string? sha, string fileName = "model.onnx") =>
        new(
            Id: "test-model",
            Kind: OnnxStemModelKind.Htdemucs,
            DisplayName: "Test model",
            FileName: fileName,
            Urls: [url],
            Sha256: sha,
            SizeBytes: payload.Length,
            HashSource: sha is null ? "no trustworthy source (test)" : "computed in the test",
            SourceCount: 4,
            LicenceNote: "test");

    [Fact]
    public async Task EnsureAsync_downloads_verifies_and_is_a_no_op_the_second_time()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var entry = EntryFor(server.Url, payload, Sha256Of(payload));

        var path = await OnnxModelCatalog.EnsureAsync(entry, _dir, ct: TestContext.Current.CancellationToken);
        Assert.Equal(Path.Combine(_dir, "model.onnx"), path);
        Assert.Equal(payload, await File.ReadAllBytesAsync(path, TestContext.Current.CancellationToken));
        Assert.Single(server.ReceivedRangeHeaders);

        // Second call: verified in place, no second request.
        var again = await OnnxModelCatalog.EnsureAsync(entry, _dir, ct: TestContext.Current.CancellationToken);
        Assert.Equal(path, again);
        Assert.Single(server.ReceivedRangeHeaders);

        Assert.True(await OnnxModelCatalog.VerifyAsync(entry, path, TestContext.Current.CancellationToken));
    }

    /// <summary>
    /// "The file exists" is not "the model is installed". A half-written model
    /// from a killed process must be discarded and fetched again, or it becomes
    /// a permanent, mystifying load failure.
    /// </summary>
    [Fact]
    public async Task An_existing_file_that_fails_its_hash_is_replaced_not_trusted()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var entry = EntryFor(server.Url, payload, Sha256Of(payload));

        var dest = Path.Combine(_dir, "model.onnx");
        await File.WriteAllBytesAsync(dest, new byte[payload.Length], TestContext.Current.CancellationToken);
        Assert.False(await OnnxModelCatalog.VerifyAsync(entry, dest, TestContext.Current.CancellationToken));

        var warnings = new List<string>();
        var path = await OnnxModelCatalog.EnsureAsync(
            entry, _dir, log: warnings.Add, ct: TestContext.Current.CancellationToken);

        Assert.Equal(payload, await File.ReadAllBytesAsync(path, TestContext.Current.CancellationToken));
        Assert.Contains(warnings, w => w.Contains("does NOT match its pinned hash", StringComparison.Ordinal));
    }

    [Fact]
    public async Task A_server_serving_the_wrong_bytes_leaves_no_model_installed()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var entry = EntryFor(server.Url, payload, new string('b', 64));

        await Assert.ThrowsAsync<InvalidDataException>(() => OnnxModelCatalog.EnsureAsync(
            entry, _dir, ct: TestContext.Current.CancellationToken));

        Assert.False(File.Exists(Path.Combine(_dir, "model.onnx")));
    }

    /// <summary>
    /// An unpinned entry is a decision, not a default: it warns AND refuses
    /// until the caller says so at the call site.
    /// </summary>
    [Fact]
    public async Task An_unpinned_entry_is_refused_until_the_caller_opts_in()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var entry = EntryFor(server.Url, payload, sha: null);

        var warnings = new List<string>();
        var ex = await Assert.ThrowsAsync<InvalidOperationException>(() => OnnxModelCatalog.EnsureAsync(
            entry, _dir, log: warnings.Add, ct: TestContext.Current.CancellationToken));
        Assert.Contains("allowUnverified", ex.Message, StringComparison.Ordinal);
        Assert.Contains(warnings, w => w.Contains("no pinned SHA-256", StringComparison.Ordinal));
        Assert.False(File.Exists(Path.Combine(_dir, "model.onnx")));

        var path = await OnnxModelCatalog.EnsureAsync(
            entry, _dir, allowUnverified: true, log: warnings.Add, ct: TestContext.Current.CancellationToken);
        Assert.Equal(payload, await File.ReadAllBytesAsync(path, TestContext.Current.CancellationToken));
    }

    /// <summary>
    /// "I cannot answer" and "the file is corrupt" must not be the same value,
    /// or a caller deletes a good model because nobody had hashed it.
    /// </summary>
    [Fact]
    public async Task Verifying_an_unpinned_entry_throws_rather_than_reporting_corruption()
    {
        var payload = Payload();
        var entry = EntryFor("http://127.0.0.1:1/none", payload, sha: null);
        var dest = Path.Combine(_dir, "model.onnx");
        await File.WriteAllBytesAsync(dest, payload, TestContext.Current.CancellationToken);

        await Assert.ThrowsAsync<InvalidOperationException>(
            () => OnnxModelCatalog.VerifyAsync(entry, dest, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task Verifying_a_missing_file_is_false_and_not_an_exception()
    {
        var payload = Payload();
        var entry = EntryFor("http://127.0.0.1:1/none", payload, Sha256Of(payload));
        Assert.False(await OnnxModelCatalog.VerifyAsync(
            entry, Path.Combine(_dir, "nope.onnx"), TestContext.Current.CancellationToken));
    }

    /// <summary>A short body that still hashes… cannot happen, so the length
    /// check is about the OTHER case: a chunked response with no
    /// <c>Content-Length</c>, where length is the only check there is. Assert
    /// the constraint is enforced with a deliberately wrong declared size.</summary>
    [Fact]
    public async Task A_body_whose_length_disagrees_with_the_catalogue_is_rejected()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var entry = EntryFor(server.Url, payload, sha: null) with { SizeBytes = payload.Length + 1 };

        await Assert.ThrowsAsync<InvalidDataException>(() => OnnxModelCatalog.EnsureAsync(
            entry, _dir, allowUnverified: true, ct: TestContext.Current.CancellationToken));
        Assert.False(File.Exists(Path.Combine(_dir, "model.onnx")));
    }

    [Fact]
    public async Task A_cancelled_ensure_keeps_the_partial_and_does_not_try_the_next_mirror()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload,
            new LoopbackServerOptions { SupportsRange = true, StallAfterBytes = 16_384 });
        var entry = EntryFor(server.Url, payload, Sha256Of(payload)) with
        {
            Urls = [server.Url, server.Url],   // two mirrors; a cancel must use neither again
        };

        using var cts = new CancellationTokenSource();
        var progress = new ImmediateProgress<DownloadProgress>(p => { if (p.BytesReceived > 0) cts.Cancel(); });

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => OnnxModelCatalog.EnsureAsync(entry, _dir, progress, ct: cts.Token));

        Assert.True(File.Exists(
            VerifiedDownloader.PartialPathFor(server.Url, Path.Combine(_dir, "model.onnx"))));
        Assert.Single(server.ReceivedRangeHeaders);   // the second mirror was NOT tried
    }
}

/// <summary>
/// The catalogue table itself. These are lints over shipped data, not over
/// behaviour — the kind of thing that rots silently because nothing executes
/// it.
///
/// <para>
/// <b>Deliberately NOT in <c>[Collection("StemDownloads")]</c>, unlike the class
/// above it in this file.</b> Nothing here opens a socket, touches
/// <c>VerifiedDownloader</c> or reaches the process-global separation gate — it
/// reads the static <see cref="OnnxModelCatalog.All"/> table — so serializing it
/// would buy nothing. That is a decision, not an oversight: an attribute on the
/// first class in a file covers NONE of the others, so if a transfer is ever
/// added here it needs the attribute added with it.
/// <c>TestCollectionLintTests</c> enforces exactly that.
/// </para>
/// </summary>
public sealed class OnnxModelCatalogTests
{
    [Fact]
    public void Every_entry_is_well_formed_and_uniquely_identified()
    {
        Assert.NotEmpty(OnnxModelCatalog.All);
        Assert.Equal(
            OnnxModelCatalog.All.Count,
            OnnxModelCatalog.All.Select(e => e.Id.ToLowerInvariant()).Distinct().Count());

        foreach (var e in OnnxModelCatalog.All)
        {
            Assert.False(string.IsNullOrWhiteSpace(e.Id));
            Assert.False(string.IsNullOrWhiteSpace(e.DisplayName));
            Assert.EndsWith(".onnx", e.FileName, StringComparison.OrdinalIgnoreCase);
            Assert.NotEmpty(e.Urls);
            Assert.All(e.Urls, u => Assert.StartsWith("https://", u, StringComparison.Ordinal));
            Assert.True(e.SizeBytes > 0, $"{e.Id} has no size; it is the only integrity check a chunked response leaves.");
            Assert.False(string.IsNullOrWhiteSpace(e.LicenceNote));
            Assert.InRange(e.SourceCount, 2, 6);
        }
    }

    /// <summary>
    /// THE LINT THIS FILE EXISTS FOR: a hash nobody can trace is a hash nobody
    /// can re-check, so every pin has to say where it came from — and an entry
    /// WITHOUT a pin has to say why not. Either way <c>HashSource</c> is
    /// mandatory.
    /// </summary>
    [Fact]
    public void Every_entry_records_where_its_hash_did_or_did_not_come_from()
    {
        foreach (var e in OnnxModelCatalog.All)
        {
            Assert.False(string.IsNullOrWhiteSpace(e.HashSource),
                $"'{e.Id}' must record the provenance of its hash (or of its absence).");

            if (e.Sha256 is null)
            {
                // A null pin must explain itself, not just be blank.
                Assert.True(e.HashSource.Length > 20,
                    $"'{e.Id}' has no hash and must say why in HashSource.");
                continue;
            }

            Assert.Equal(64, e.Sha256.Length);
            Assert.Equal(e.Sha256, e.Sha256.ToLowerInvariant());
            Assert.True(e.Sha256.All(Uri.IsHexDigit), $"'{e.Id}' has a non-hex SHA-256.");
            Assert.True(e.HashSource.Length > 20,
                $"'{e.Id}' pins a hash but its HashSource says nothing traceable.");
        }
    }

    [Fact]
    public void Every_model_kind_has_a_default_and_at_least_one_entry()
    {
        foreach (var kind in Enum.GetValues<OnnxStemModelKind>())
        {
            var entries = OnnxModelCatalog.ForKind(kind);
            Assert.NotEmpty(entries);
            var fallback = OnnxModelCatalog.DefaultFor(kind);
            Assert.Equal(kind, fallback.Kind);
            Assert.Contains(fallback, entries);
        }
    }

    [Fact]
    public void Lookup_by_id_is_case_insensitive_and_a_miss_is_null()
    {
        var first = OnnxModelCatalog.All[0];
        Assert.Same(first, OnnxModelCatalog.FindById(first.Id));
        Assert.Same(first, OnnxModelCatalog.FindById(first.Id.ToUpperInvariant()));
        Assert.Null(OnnxModelCatalog.FindById("no-such-model"));
        Assert.Null(OnnxModelCatalog.FindById(null));
        Assert.Null(OnnxModelCatalog.FindById("   "));
    }
}
