using System.Reflection;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using JuceSharp.StemSeparation.Onnx;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The KIND-LESS verified download (<see cref="VerifiedDownload"/>) and the
/// ggml catalogue on top of it.
///
/// <para>
/// <b>The gap these close.</b> Until 0.9 the only public route into the
/// verified download path was <c>OnnxModelCatalog.EnsureAsync</c>, which
/// demands an <c>OnnxModelEntry</c> carrying an <c>OnnxStemModelKind</c> — the
/// inference driver that consumes the file. A host with a ggml weight for the
/// native demucs.cpp backend, or a user-supplied <c>.ckpt</c>, has no honest
/// value for that field, so DotCharter shipped a
/// <c>NearestKindLabel</c> helper whose own comment admitted the field was "a
/// label rather than a fact". The fix is a door that never asks, and a
/// catalogue for the weights that had none.
/// </para>
/// <para>
/// Nothing here touches the real network or a real model: every transfer goes
/// through <see cref="LoopbackFileServer"/> on 127.0.0.1.
/// </para>
/// </summary>
[Collection("StemDownloads")]
public sealed class VerifiedDownloadTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), $"jskd-{Guid.NewGuid():N}");

    public VerifiedDownloadTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
    }

    private static byte[] Payload(int length = 64 * 1024)
    {
        var bytes = new byte[length];
        for (var i = 0; i < length; i++) bytes[i] = (byte)((i * 31 + 7) & 0xFF);
        return bytes;
    }

    private static string Sha256Of(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    // ================================================================
    //  The two doors are the SAME code
    // ================================================================

    /// <summary>
    /// The ONNX catalogue and the kind-less entry point must not be two
    /// implementations of "download this file safely" — that is the exact shape
    /// <c>VerifiedDownloader</c>'s remarks call the bug. Every behavioural test
    /// below is therefore a THEORY over both doors, and each one asserts the
    /// same outcome for both.
    /// </summary>
    /// <param name="viaCatalogue">True drives
    ///   <c>OnnxModelCatalog.EnsureAsync</c>; false drives
    ///   <see cref="VerifiedDownload.EnsureFileAsync"/> with no model kind
    ///   anywhere in sight.</param>
    /// <param name="url">Source URL.</param>
    /// <param name="fileName">Destination file name inside the test directory.</param>
    /// <param name="sha">Pinned hash, or null.</param>
    /// <param name="size">Pinned exact size, or null.</param>
    /// <param name="log">Log sink both doors write their warnings into.</param>
    /// <param name="progress">Optional progress sink.</param>
    /// <param name="ct">Cancellation token.</param>
    private Task<string> EnsureEitherWay(
        bool viaCatalogue, string url, string fileName, string? sha, long? size,
        List<string> log, IProgress<DownloadProgress>? progress, CancellationToken ct)
    {
        if (!viaCatalogue)
            return VerifiedDownload.EnsureFileAsync(
                new Uri(url), Path.Combine(_dir, fileName), sha, size, progress, log.Add, ct);

        var entry = new OnnxModelEntry(
            Id: "same-code-probe",
            Kind: OnnxStemModelKind.Htdemucs,
            DisplayName: "Same-code probe",
            FileName: fileName,
            Urls: [url],
            Sha256: sha,
            SizeBytes: size ?? 0,
            HashSource: sha is null ? "no trustworthy source (test)" : "computed in the test",
            SourceCount: 4,
            LicenceNote: "test");
        return OnnxModelCatalog.EnsureAsync(
            entry, _dir, progress, allowUnverified: sha is null, log: log.Add, ct: ct);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Both_doors_download_verify_and_are_a_no_op_the_second_time(bool viaCatalogue)
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var log = new List<string>();
        var ct = TestContext.Current.CancellationToken;

        var path = await EnsureEitherWay(
            viaCatalogue, server.Url, "model.bin", Sha256Of(payload), payload.Length, log, null, ct);

        Assert.Equal(Path.Combine(_dir, "model.bin"), path);
        Assert.Equal(payload, await File.ReadAllBytesAsync(path, ct));
        Assert.Single(server.ReceivedRangeHeaders);

        // Second call: verified in place, and NOT a second request.
        var again = await EnsureEitherWay(
            viaCatalogue, server.Url, "model.bin", Sha256Of(payload), payload.Length, log, null, ct);
        Assert.Equal(path, again);
        Assert.Single(server.ReceivedRangeHeaders);
    }

    /// <summary>
    /// "The file exists" is not "the model is installed" — through either door.
    /// A half-written file from a killed process must be discarded and fetched
    /// again, or it becomes a permanent, mystifying load failure.
    /// </summary>
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Both_doors_replace_an_existing_file_that_fails_its_hash(bool viaCatalogue)
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var dest = Path.Combine(_dir, "model.bin");
        var ct = TestContext.Current.CancellationToken;
        await File.WriteAllBytesAsync(dest, new byte[payload.Length], ct);

        var log = new List<string>();
        var path = await EnsureEitherWay(
            viaCatalogue, server.Url, "model.bin", Sha256Of(payload), payload.Length, log, null, ct);

        Assert.Equal(payload, await File.ReadAllBytesAsync(path, ct));
        Assert.Contains(log, w => w.Contains("does NOT match its pinned hash", StringComparison.Ordinal));
    }

    /// <summary>
    /// The close-delimited truncation case, which no HTTP client can detect and
    /// which is what a pinned size exists for. Neither door may leave a file at
    /// the canonical path.
    /// </summary>
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Both_doors_reject_a_body_whose_length_disagrees_with_the_pin(bool viaCatalogue)
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var log = new List<string>();
        var ct = TestContext.Current.CancellationToken;

        await Assert.ThrowsAsync<InvalidDataException>(() => EnsureEitherWay(
            viaCatalogue, server.Url, "model.bin", sha: null, size: payload.Length + 1, log, null, ct));

        Assert.False(File.Exists(Path.Combine(_dir, "model.bin")));
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Both_doors_keep_the_part_file_when_the_caller_cancels(bool viaCatalogue)
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload,
            new LoopbackServerOptions { SupportsRange = true, StallAfterBytes = 16_384 });

        using var cts = new CancellationTokenSource();
        var progress = new ImmediateProgress<DownloadProgress>(p => { if (p.BytesReceived > 0) cts.Cancel(); });

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => EnsureEitherWay(
            viaCatalogue, server.Url, "model.bin", Sha256Of(payload), payload.Length,
            [], progress, cts.Token));

        Assert.True(File.Exists(
            VerifiedDownloader.PartialPathFor(server.Url, Path.Combine(_dir, "model.bin"))));
        Assert.False(File.Exists(Path.Combine(_dir, "model.bin")));
    }

    /// <summary>
    /// Source lint, and the reason the theories above can claim "the same
    /// code": <c>OnnxModelCatalog</c> no longer reaches the downloader at all —
    /// it goes through the kind-less entry point per mirror, so a fix to the
    /// resume or verify order cannot land on one path and miss the other.
    /// </summary>
    [Fact]
    public void The_onnx_catalogue_reaches_the_network_only_through_the_kind_less_entry_point()
    {
        var catalogue = File.ReadAllText(Path.Combine(
            RepoRoot(), "src", "JuceSharp.StemSeparation.Onnx", "OnnxModelCatalog.cs"));
        var code = StripComments(catalogue);

        Assert.DoesNotContain("VerifiedDownloader.", code, StringComparison.Ordinal);
        Assert.Contains("VerifiedDownload.ValidateSourceUrl(", code, StringComparison.Ordinal);
        // Since W4-S27 the mirror walk is CatalogueDownload's, once, for both
        // catalogues — so the hop this lint follows is one longer. (Re-pointing
        // it was required, not optional: a path-keyed source lint whose subject
        // moved passes VACUOUSLY.)
        Assert.Contains("CatalogueDownload.EnsureAsync(", code, StringComparison.Ordinal);

        var ggml = StripComments(File.ReadAllText(Path.Combine(
            RepoRoot(), "src", "JuceSharp.StemSeparation", "Runtime", "DemucsGgmlModelCatalog.cs")));
        Assert.DoesNotContain("VerifiedDownloader.", ggml, StringComparison.Ordinal);
        Assert.Contains("CatalogueDownload.EnsureAsync(", ggml, StringComparison.Ordinal);

        // The shared policy is the ONE place either catalogue reaches a
        // transfer, and it goes through the kind-less door like everyone else.
        var shared = StripComments(File.ReadAllText(Path.Combine(
            RepoRoot(), "src", "JuceSharp.StemSeparation", "Runtime", "CatalogueDownload.cs")));
        Assert.DoesNotContain("VerifiedDownloader.", shared, StringComparison.Ordinal);
        Assert.Contains("VerifiedDownload.EnsureFileAsync(", shared, StringComparison.Ordinal);

        // Non-vacuity: the token being hunted for really does exist in the
        // sources, so a rename that silently emptied this assertion would be
        // caught here rather than passing.
        var door = File.ReadAllText(Path.Combine(
            RepoRoot(), "src", "JuceSharp.StemSeparation", "Runtime", "VerifiedDownload.cs"));
        Assert.Contains("VerifiedDownloader.DownloadGateHeldAsync(", StripComments(door), StringComparison.Ordinal);
    }

    // ================================================================
    //  VerifiedDownload's own surface
    // ================================================================

    /// <summary>
    /// The kind-less door takes a <see cref="Uri"/>, not a model, and it is the
    /// one call DotCharter's checkpoint importer and ggml fetcher needed: no
    /// <c>OnnxStemModelKind</c> appears anywhere in this test.
    /// </summary>
    [Fact]
    public async Task EnsureFileAsync_downloads_a_file_that_belongs_to_no_model_kind()
    {
        var payload = Payload(8192);
        using var server = LoopbackFileServer.Start(payload);
        var ct = TestContext.Current.CancellationToken;
        var dest = Path.Combine(_dir, "sub", "weights.ckpt");   // a directory that does not exist yet

        var samples = new List<DownloadProgress>();
        var path = await VerifiedDownload.EnsureFileAsync(
            new Uri(server.Url), dest, Sha256Of(payload), payload.Length,
            new ImmediateProgress<DownloadProgress>(samples.Add), ct: ct);

        Assert.Equal(Path.GetFullPath(dest), path);
        Assert.Equal(payload, await File.ReadAllBytesAsync(path, ct));
        Assert.NotEmpty(samples);
        Assert.Equal(1.0, samples[^1].Fraction, 6);
    }

    /// <summary>
    /// With no hash, the pinned LENGTH is the whole of the evidence — so a file
    /// of the right length is kept (with a warning that says what was and was
    /// not checked) and one of the wrong length is discarded rather than
    /// trusted. Getting this backwards either re-downloads 300 MB on every
    /// launch or keeps a truncated file for ever.
    /// </summary>
    [Fact]
    public async Task An_unhashed_existing_file_is_kept_when_its_length_is_right_and_replaced_when_it_is_not()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var ct = TestContext.Current.CancellationToken;
        var dest = Path.Combine(_dir, "weights.bin");

        // Right length, wrong bytes: nothing can tell, so it is kept.
        await File.WriteAllBytesAsync(dest, new byte[payload.Length], ct);
        var log = new List<string>();
        await VerifiedDownload.EnsureFileAsync(
            new Uri(server.Url), dest, sha256: null, expectedBytes: payload.Length, log: log.Add, ct: ct);
        Assert.Equal(new byte[payload.Length], await File.ReadAllBytesAsync(dest, ct));
        Assert.Empty(server.ReceivedRangeHeaders);
        Assert.Contains(log, w => w.Contains("UNVERIFIED", StringComparison.Ordinal));

        // Wrong length: discarded and fetched again.
        await File.WriteAllBytesAsync(dest, new byte[payload.Length - 1], ct);
        await VerifiedDownload.EnsureFileAsync(
            new Uri(server.Url), dest, sha256: null, expectedBytes: payload.Length, log: log.Add, ct: ct);
        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, ct));
        Assert.Single(server.ReceivedRangeHeaders);
    }

    [Theory]
    [InlineData("http://example.com/weights.bin")]
    [InlineData("ftp://example.com/weights.bin")]
    [InlineData("file:///C:/weights.bin")]
    public async Task EnsureFileAsync_refuses_a_source_that_is_not_https_or_loopback(string url)
    {
        await Assert.ThrowsAsync<ArgumentException>(() => VerifiedDownload.EnsureFileAsync(
            new Uri(url), Path.Combine(_dir, "weights.bin"),
            ct: TestContext.Current.CancellationToken));
        Assert.False(File.Exists(Path.Combine(_dir, "weights.bin")));
    }

    [Fact]
    public async Task EnsureFileAsync_rejects_a_blank_destination_and_a_null_url()
    {
        var ct = TestContext.Current.CancellationToken;
        await Assert.ThrowsAsync<ArgumentNullException>(() =>
            VerifiedDownload.EnsureFileAsync(null!, Path.Combine(_dir, "x.bin"), ct: ct));
        await Assert.ThrowsAsync<ArgumentException>(() =>
            VerifiedDownload.EnsureFileAsync(new Uri("https://example.com/x.bin"), "  ", ct: ct));
    }

    /// <summary>
    /// <see cref="VerifiedDownload.VerifyFileAsync"/> is the pre-flight a host
    /// runs before deciding to download. A MISSING file is false — "not
    /// installed" is a legitimate answer — while a blank hash THROWS, because
    /// returning false for "nobody pinned it" is how a caller deletes a
    /// perfectly good model.
    /// </summary>
    [Fact]
    public async Task VerifyFileAsync_separates_missing_corrupt_and_unanswerable()
    {
        var payload = Payload(4096);
        var ct = TestContext.Current.CancellationToken;
        var dest = Path.Combine(_dir, "weights.bin");
        var sha = Sha256Of(payload);

        Assert.False(await VerifiedDownload.VerifyFileAsync(dest, sha, payload.Length, ct));

        await File.WriteAllBytesAsync(dest, payload, ct);
        Assert.True(await VerifiedDownload.VerifyFileAsync(dest, sha, payload.Length, ct));
        Assert.True(await VerifiedDownload.VerifyFileAsync(dest, sha.ToUpperInvariant(), null, ct));

        // The length check is first and is enough on its own.
        Assert.False(await VerifiedDownload.VerifyFileAsync(dest, sha, payload.Length + 1, ct));

        await File.WriteAllBytesAsync(dest, new byte[payload.Length], ct);
        Assert.False(await VerifiedDownload.VerifyFileAsync(dest, sha, payload.Length, ct));

        await Assert.ThrowsAsync<ArgumentException>(
            () => VerifiedDownload.VerifyFileAsync(dest, "  ", null, ct));
    }

    /// <summary>
    /// <c>OnnxModelCatalog.ValidateEntry</c> is part of the PUBLIC surface. It
    /// was internal through 0.9, so the only host that could pre-flight a
    /// synthesized entry was one with an <c>InternalsVisibleTo</c> — DotCharter's
    /// tests reached it exactly that way, which is the tell that the PRODUCT
    /// wanted it too. A host that builds entries from a settings file or a
    /// user-typed URL wants to reject a bad one where it builds it, with its
    /// own error reporting, rather than as an <see cref="ArgumentException"/>
    /// out of a download the user was already told had started.
    ///
    /// <para>
    /// Asserted by REFLECTION over the public binding flags rather than by
    /// calling it: this test project has <c>InternalsVisibleTo</c>, so a direct
    /// call would compile and pass just as happily with the member internal
    /// again — the exact vacuity this guard exists to avoid.
    /// </para>
    /// </summary>
    [Fact]
    public void ValidateEntry_is_public_so_a_host_can_pre_flight_an_entry_it_synthesized()
    {
        var method = typeof(OnnxModelCatalog).GetMethod(
            nameof(OnnxModelCatalog.ValidateEntry),
            BindingFlags.Public | BindingFlags.Static,
            [typeof(OnnxModelEntry)]);

        Assert.NotNull(method);
        Assert.True(method!.IsPublic);
        Assert.Equal(typeof(void), method.ReturnType);

        // …and the ggml catalogue's twin, for the same reason.
        Assert.NotNull(typeof(DemucsGgmlModelCatalog).GetMethod(
            nameof(DemucsGgmlModelCatalog.ValidateEntry),
            BindingFlags.Public | BindingFlags.Static,
            [typeof(DemucsGgmlModelEntry)]));
    }

    // ================================================================
    //  Helpers shared with the other source lints
    // ================================================================

    private static string StripComments(string text)
    {
        text = Regex.Replace(text, @"/\*.*?\*/", string.Empty, RegexOptions.Singleline);
        return Regex.Replace(text, @"//[^\r\n]*", string.Empty);
    }

    private static string RepoRoot([CallerFilePath] string? thisFile = null)
    {
        var dir = Path.GetDirectoryName(thisFile);
        while (dir != null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        return dir ?? throw new InvalidOperationException("Could not locate the JuceSharp root.");
    }
}
