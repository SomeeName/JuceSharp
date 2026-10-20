using System.Security.Cryptography;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// <see cref="DemucsGgmlModelCatalog"/> — the weights the NATIVE demucs.cpp
/// backend loads, which had no catalogue at all until 0.9 because
/// <c>OnnxModelCatalog</c> is (correctly) ONNX-only.
///
/// <para>
/// These mirror the ONNX catalogue's validation lints one for one, because the
/// failure they guard against is identical: a pin nobody can trace is a pin
/// nobody can re-check, and a table of URLs that is not held to the transport
/// and file-name rules is an arbitrary-path writer fed over an arbitrary
/// transport.
/// </para>
/// </summary>
[Collection("StemDownloads")]
public sealed class DemucsGgmlModelCatalogTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), $"jsggml-{Guid.NewGuid():N}");

    public DemucsGgmlModelCatalogTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
    }

    // ------------------------------------------------------------ the table

    [Fact]
    public void Every_shipped_entry_passes_the_validation_its_callers_get()
    {
        Assert.NotEmpty(DemucsGgmlModelCatalog.All);
        foreach (var entry in DemucsGgmlModelCatalog.All)
            DemucsGgmlModelCatalog.ValidateEntry(entry);
    }

    /// <summary>
    /// The provenance rule, which is the whole reason this table exists rather
    /// than the hand-written one it replaces: every entry says where its hash
    /// came from, and an entry with no hash has to say WHY.
    /// </summary>
    [Fact]
    public void Every_entry_records_where_its_hash_came_from()
    {
        foreach (var entry in DemucsGgmlModelCatalog.All)
        {
            Assert.False(string.IsNullOrWhiteSpace(entry.HashSource),
                $"'{entry.Id}' has no HashSource. A pin nobody can trace is a pin nobody can re-check, " +
                "and an ABSENT pin has to say why it is absent.");

            if (entry.Sha256 is null) continue;

            Assert.Matches("^[0-9a-f]{64}$", entry.Sha256);
            // The hashes shipped today all come from the Hugging Face repo API,
            // where an LFS object's oid IS its SHA-256. If a future entry is
            // pinned from somewhere else that is fine — but it must say so, and
            // it must not be a hash computed from a copy fetched over the same
            // unverified channel.
            Assert.True(
                entry.HashSource.Contains("huggingface.co/api", StringComparison.OrdinalIgnoreCase) ||
                entry.HashSource.Contains("computed", StringComparison.OrdinalIgnoreCase),
                $"'{entry.Id}' is pinned but its HashSource does not say where the value came from: " +
                $"\"{entry.HashSource}\"");
        }
    }

    /// <summary>
    /// A size of 0 means "unknown", and for these three it is knowable — it is
    /// published beside the oid. It is not a UI hint: it is the only integrity
    /// check there is against a close-delimited response, which is exactly the
    /// shape a truncating proxy produces.
    /// </summary>
    [Fact]
    public void Every_entry_carries_its_exact_size_and_a_unique_id_and_file_name()
    {
        foreach (var entry in DemucsGgmlModelCatalog.All)
        {
            Assert.True(entry.SizeBytes > 0, $"'{entry.Id}' has no exact size.");
            Assert.True(entry.SourceCount is 4 or 6, $"'{entry.Id}' separates {entry.SourceCount} sources.");
            Assert.False(string.IsNullOrWhiteSpace(entry.LicenceNote));
            Assert.False(string.IsNullOrWhiteSpace(entry.DisplayName));
        }

        Assert.Equal(
            DemucsGgmlModelCatalog.All.Count,
            DemucsGgmlModelCatalog.All.Select(e => e.Id).Distinct(StringComparer.OrdinalIgnoreCase).Count());
        // Two entries sharing a FILE NAME share a destination, and the second
        // one installed would be reported "already present" by the first one's
        // hash — or discarded and re-fetched on every launch, for ever.
        Assert.Equal(
            DemucsGgmlModelCatalog.All.Count,
            DemucsGgmlModelCatalog.All.Select(e => e.FileName).Distinct(StringComparer.OrdinalIgnoreCase).Count());
    }

    /// <summary>
    /// The three weights DotCharter's hand-written table named, with the sizes
    /// and hashes the repository API actually publishes. Its own row for the
    /// 4-stem model said "~80 MB" and carried NO hash, on the stated grounds
    /// that a Hugging Face DATASET repo publishes no LFS oid — it does, under
    /// <c>api/datasets/</c> rather than <c>api/models/</c>. Pinning these
    /// values here is what makes that correction re-checkable.
    /// </summary>
    [Fact]
    public void The_three_demucs_cpp_weights_are_pinned_with_the_values_the_repository_publishes()
    {
        var four = DemucsGgmlModelCatalog.FindById("ggml-htdemucs-4s");
        Assert.NotNull(four);
        Assert.Equal("ggml-model-htdemucs-4s-f16.bin", four!.FileName);
        Assert.Equal(83_994_361L, four.SizeBytes);
        Assert.Equal("72b17c42d308982ddb5069bc3bf48b81a5aac4cb6516e4366c0fa7cef6df0064", four.Sha256);

        var six = DemucsGgmlModelCatalog.FindById("ggml-htdemucs-6s");
        Assert.NotNull(six);
        Assert.Equal("ggml-model-htdemucs-6s-f16.bin", six!.FileName);
        Assert.Equal(54_855_129L, six.SizeBytes);
        // Corroborated: DotCharter computed this independently on 2026-09-04
        // from a copy it had been separating with, and it agrees with the oid.
        Assert.Equal("09704f4ceae204e56e77d5eefd6ac71d7275be81fd507e6913371d59abcee856", six.Sha256);
        Assert.Equal(6, six.SourceCount);

        var mmi = DemucsGgmlModelCatalog.FindById("ggml-hdemucs-mmi-v3");
        Assert.NotNull(mmi);
        Assert.Equal("ggml-model-hdemucs_mmi-v3-f16.bin", mmi!.FileName);
        Assert.Equal(167_294_211L, mmi.SizeBytes);
        Assert.Equal("f72a3b8ecce3aa50da9e062a91c46367e15b5f5a71d070874217c5e4cb6b35a4", mmi.Sha256);

        Assert.Same(four, DemucsGgmlModelCatalog.Default);
        Assert.Null(DemucsGgmlModelCatalog.FindById("no-such-model"));
        Assert.Null(DemucsGgmlModelCatalog.FindById(null));
        // Ids are the thing a host persists in a settings file, so the lookup
        // has to be case-insensitive or a config round-trip can lose a model.
        Assert.Same(four, DemucsGgmlModelCatalog.FindById("GGML-HTDEMUCS-4S"));
    }

    // ------------------------------------------------------------ validation

    [Theory]
    [InlineData(@"..\..\startup.exe")]
    [InlineData("../../startup.exe")]
    [InlineData(@"sub\model.bin")]
    [InlineData("")]
    public void An_entry_whose_file_name_escapes_the_target_directory_is_refused(string fileName)
    {
        var ex = Assert.Throws<ArgumentException>(
            () => DemucsGgmlModelCatalog.ValidateEntry(Entry() with { FileName = fileName }));
        Assert.Contains("file name", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData("http://example.com/model.bin")]
    [InlineData("ftp://example.com/model.bin")]
    [InlineData("file:///C:/model.bin")]
    [InlineData("model.bin")]
    public void An_entry_that_would_fetch_over_anything_but_https_is_refused(string url)
        => Assert.Throws<ArgumentException>(
            () => DemucsGgmlModelCatalog.ValidateEntry(Entry() with { Urls = [url] }));

    [Fact]
    public void Loopback_http_is_accepted_and_an_empty_mirror_list_is_not()
    {
        DemucsGgmlModelCatalog.ValidateEntry(Entry() with { Urls = ["http://127.0.0.1:8080/model.bin"] });
        Assert.Throws<ArgumentException>(
            () => DemucsGgmlModelCatalog.ValidateEntry(Entry() with { Urls = [] }));
    }

    // ------------------------------------------------------------- downloads

    [Fact]
    public async Task EnsureAsync_downloads_verifies_and_is_a_no_op_the_second_time()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var ct = TestContext.Current.CancellationToken;
        var entry = Entry() with
        {
            Urls = [server.Url], Sha256 = Sha256Of(payload), SizeBytes = payload.Length,
        };

        var path = await DemucsGgmlModelCatalog.EnsureAsync(entry, _dir, ct: ct);
        Assert.Equal(Path.Combine(_dir, "model.bin"), path);
        Assert.Equal(payload, await File.ReadAllBytesAsync(path, ct));
        Assert.Single(server.ReceivedRangeHeaders);

        Assert.Equal(path, await DemucsGgmlModelCatalog.EnsureAsync(entry, _dir, ct: ct));
        Assert.Single(server.ReceivedRangeHeaders);

        Assert.True(await DemucsGgmlModelCatalog.VerifyAsync(entry, path, ct));
    }

    /// <summary>
    /// Downloading and then loading an unverifiable set of weights into the
    /// native engine is a decision, so it needs saying at the call site — the
    /// same rule the ONNX catalogue enforces, and the reason a ggml weight
    /// should never have had to be dressed as an ONNX entry to get it.
    /// </summary>
    [Fact]
    public async Task An_unpinned_entry_is_refused_until_the_caller_opts_in()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var ct = TestContext.Current.CancellationToken;
        var entry = Entry() with
        {
            Urls = [server.Url], Sha256 = null, SizeBytes = payload.Length,
            HashSource = "no trustworthy source (test)",
        };

        var log = new List<string>();
        var ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => DemucsGgmlModelCatalog.EnsureAsync(entry, _dir, log: log.Add, ct: ct));
        Assert.Contains("allowUnverified", ex.Message, StringComparison.Ordinal);
        Assert.Contains(log, w => w.Contains("no pinned SHA-256", StringComparison.Ordinal));
        Assert.False(File.Exists(Path.Combine(_dir, "model.bin")));

        var path = await DemucsGgmlModelCatalog.EnsureAsync(
            entry, _dir, allowUnverified: true, log: log.Add, ct: ct);
        Assert.Equal(payload, await File.ReadAllBytesAsync(path, ct));

        // …and verifying an unpinned entry is NOT false. "I cannot answer" and
        // "the file is corrupt" must not be the same value, or a caller deletes
        // a perfectly good model because nobody had hashed it.
        await Assert.ThrowsAsync<InvalidOperationException>(
            () => DemucsGgmlModelCatalog.VerifyAsync(entry, path, ct));
    }

    [Fact]
    public async Task A_server_serving_the_wrong_bytes_leaves_nothing_installed()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var ct = TestContext.Current.CancellationToken;
        var entry = Entry() with
        {
            Urls = [server.Url], Sha256 = new string('b', 64), SizeBytes = payload.Length,
        };

        await Assert.ThrowsAsync<InvalidDataException>(
            () => DemucsGgmlModelCatalog.EnsureAsync(entry, _dir, ct: ct));
        Assert.False(File.Exists(Path.Combine(_dir, "model.bin")));
    }

    /// <summary>
    /// A mirror list is a list of SERVERS for the same bytes: a dead first
    /// mirror falls through to the second, and the file that lands still has to
    /// hash. (A user Cancel deliberately does NOT fall through; that half is
    /// pinned in <see cref="VerifiedDownloadTests"/> for both doors.)
    /// </summary>
    [Fact]
    public async Task A_dead_mirror_falls_through_to_the_next_one()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var ct = TestContext.Current.CancellationToken;
        var entry = Entry() with
        {
            // Port 1 on loopback: nothing is listening, so this is a connection
            // refusal rather than a slow server.
            Urls = ["http://127.0.0.1:1/dead.bin", server.Url],
            Sha256 = Sha256Of(payload),
            SizeBytes = payload.Length,
        };

        var log = new List<string>();
        var path = await DemucsGgmlModelCatalog.EnsureAsync(entry, _dir, log: log.Add, ct: ct);
        Assert.Equal(payload, await File.ReadAllBytesAsync(path, ct));
        Assert.Contains(log, w => w.Contains("Mirror 'http://127.0.0.1:1/dead.bin' failed", StringComparison.Ordinal));
    }

    // ---------------------------------------------------------------- shared

    private static DemucsGgmlModelEntry Entry() =>
        new(
            Id: "test-ggml",
            DisplayName: "Test ggml weights",
            FileName: "model.bin",
            Urls: ["https://example.com/model.bin"],
            Sha256: null,
            SizeBytes: 0,
            HashSource: "test",
            SourceCount: 4,
            LicenceNote: "test");

    private static byte[] Payload(int length = 32 * 1024)
    {
        var bytes = new byte[length];
        for (var i = 0; i < length; i++) bytes[i] = (byte)((i * 17 + 3) & 0xFF);
        return bytes;
    }

    private static string Sha256Of(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}
