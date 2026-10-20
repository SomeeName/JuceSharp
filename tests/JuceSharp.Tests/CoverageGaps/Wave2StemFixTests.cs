using System.Security.Cryptography;
using System.Text.RegularExpressions;
using JuceSharp.StemSeparation.Onnx;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The download tests share one process-global knob
/// (<c>VerifiedDownloader.RequestTimeout</c>), so they may not run beside each
/// other: a 400 ms budget set here while another class is mid-transfer is a
/// spurious timeout over there.
/// </summary>
[CollectionDefinition("StemDownloads", DisableParallelization = true)]
public class StemDownloadsCollection { }

/// <summary>
/// Wave-2 audit fixes in the download path: the bounded 416 retry (#6), the
/// truncation that used to be renamed into place and executed (#8), the timeout
/// that read as a user cancel and killed the mirror list (#13), the per-URL
/// partial file, the non-atomic rename, and the missing same-destination gate.
///
/// <para>Nothing here touches the real network: every transfer is
/// <see cref="LoopbackFileServer"/> on 127.0.0.1.</para>
/// </summary>
[Collection("StemDownloads")]
public sealed class Wave2StemDownloadTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), $"jsw2-{Guid.NewGuid():N}");

    public Wave2StemDownloadTests() => Directory.CreateDirectory(_dir);

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

    private static OnnxModelEntry Entry(IReadOnlyList<string> urls, byte[] payload, string? sha, long size) =>
        new(
            Id: "test-model",
            Kind: OnnxStemModelKind.Htdemucs,
            DisplayName: "Test model",
            FileName: "model.onnx",
            Urls: urls,
            Sha256: sha,
            SizeBytes: size,
            HashSource: sha is null ? "no trustworthy source (test)" : "computed in the test",
            SourceCount: 4,
            LicenceNote: "test");

    // ------------------------------------------------------------- #6 (416)

    /// <summary>
    /// A 416 means our prefix is already at or past the resource. The retry
    /// must suppress the resume — the old code re-entered with IDENTICAL
    /// arguments, so it recomputed the same prefix, sent the same Range and was
    /// answered 416 again.
    /// </summary>
    [Fact]
    public async Task A_416_restarts_the_transfer_exactly_once_and_without_a_range()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload, new LoopbackServerOptions
        {
            SupportsRange = true,
            RangeNotSatisfiable = true,
        });
        var dest = Path.Combine(_dir, "model.onnx");
        var part = VerifiedDownloader.PartialPathFor(server.Url, dest);
        await File.WriteAllBytesAsync(part, payload[..20_000], TestContext.Current.CancellationToken);

        var written = await VerifiedDownloader.DownloadAsync(
            server.Url, dest, null, TestContext.Current.CancellationToken, Sha256Of(payload), allowResume: true);

        Assert.Equal(payload.Length, written);
        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
        // Exactly two requests, and the SECOND carried no Range at all. A retry
        // that resumed again would appear here as a third, a fourth, … until
        // the stack ran out — which is a process kill, not a failed download.
        Assert.Collection(server.ReceivedRangeHeaders,
            first => Assert.Equal("bytes=20000-", first),
            second => Assert.Null(second));
        Assert.False(File.Exists(part));
    }

    /// <summary>
    /// The unbounded case: <c>TryDelete</c> is best-effort and swallows
    /// everything, so a prefix that cannot be removed used to send the retry
    /// straight back into the same 416. It must fail instead.
    /// </summary>
    [Fact]
    public async Task A_416_whose_prefix_cannot_be_deleted_fails_instead_of_retrying()
    {
        if (!OperatingSystem.IsWindows())
            Assert.Skip("Only Windows refuses to delete a file that is open with FileShare.None.");

        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload, new LoopbackServerOptions
        {
            SupportsRange = true,
            RangeNotSatisfiable = true,
        });
        var dest = Path.Combine(_dir, "model.onnx");
        var part = VerifiedDownloader.PartialPathFor(server.Url, dest);
        await File.WriteAllBytesAsync(part, payload[..20_000], TestContext.Current.CancellationToken);

        // Somebody else (an antivirus scan, a second download) holds it.
        using var holder = new FileStream(part, FileMode.Open, FileAccess.Read, FileShare.None);

        await Assert.ThrowsAnyAsync<IOException>(() => VerifiedDownloader.DownloadAsync(
            server.Url, dest, null, TestContext.Current.CancellationToken, Sha256Of(payload), allowResume: true));
        Assert.False(File.Exists(dest));
    }

    // ------------------------------------------------------- #8 (truncation)

    /// <summary>
    /// A response that DECLARES more than it delivers must never reach the
    /// destination. This is the unpinned, non-resumable shape, where the only
    /// evidence of a short transfer is the declared length.
    /// </summary>
    [Fact]
    public async Task A_short_body_is_never_renamed_into_place_even_with_no_hash_to_check()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload,
            new LoopbackServerOptions { OverstateContentLengthBy = 4_096 });
        var dest = Path.Combine(_dir, "get-pip.py");

        await Assert.ThrowsAnyAsync<IOException>(() => VerifiedDownloader.DownloadAsync(
            server.Url, dest, null, TestContext.Current.CancellationToken,
            expectedSha256: null, allowResume: false));

        Assert.False(File.Exists(dest));
        Assert.Empty(Directory.GetFiles(_dir, "get-pip.py.tmp.*"));
    }

    /// <summary>
    /// The one truncation no HTTP client can detect: a close-delimited response
    /// (no <c>Content-Length</c>), where EOF IS the terminator. The catalogue's
    /// <c>SizeBytes</c> is the only net, and it now runs BEFORE the rename —
    /// checking it afterwards left a window where a concurrent reader saw a
    /// short model at the canonical path.
    /// </summary>
    [Fact]
    public async Task A_close_delimited_short_body_is_caught_by_SizeBytes_before_the_rename()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload, new LoopbackServerOptions
        {
            OmitContentLength = true,
            TruncateBodyTo = 30_000,
        });
        var dest = Path.Combine(_dir, "model.onnx");

        var ex = await Assert.ThrowsAsync<InvalidDataException>(() => VerifiedDownloader.DownloadAsync(
            server.Url, dest, null, TestContext.Current.CancellationToken,
            expectedSha256: null, allowResume: true, expectedSizeBytes: payload.Length));

        Assert.Contains("30000", ex.Message, StringComparison.Ordinal);
        Assert.False(File.Exists(dest));
        // Known-wrong bytes are not a prefix worth resuming.
        Assert.False(File.Exists(VerifiedDownloader.PartialPathFor(server.Url, dest)));
    }

    // ---------------------------------------------------------- #13 (timeout)

    /// <summary>
    /// The budget expiring is an <see cref="IOException"/>. As an
    /// <see cref="OperationCanceledException"/> it was indistinguishable from
    /// the user pressing Cancel, which every consumer of the stem pipeline
    /// reads per <c>IStemSeparator</c>'s documented contract.
    /// </summary>
    [Fact]
    public async Task A_transfer_that_exceeds_the_budget_throws_IOException_not_cancellation()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload,
            new LoopbackServerOptions { SupportsRange = true, StallAfterBytes = 4_096 });
        var dest = Path.Combine(_dir, "model.onnx");

        var previous = VerifiedDownloader.RequestTimeout;
        VerifiedDownloader.RequestTimeout = TimeSpan.FromMilliseconds(400);
        try
        {
            var ex = await Assert.ThrowsAnyAsync<IOException>(() => VerifiedDownloader.DownloadAsync(
                server.Url, dest, null, TestContext.Current.CancellationToken,
                Sha256Of(payload), allowResume: true));
            Assert.Contains("timed out", ex.Message, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            VerifiedDownloader.RequestTimeout = previous;
        }

        // A timeout is a network failure, not a bad payload, so whatever
        // arrived stays on disk for the next attempt — but the DESTINATION is
        // never created from it.
        Assert.False(File.Exists(dest));
    }

    /// <summary>
    /// …and therefore a slow mirror no longer ends the attempt. Before the fix
    /// the catalogue's "a user Cancel is not a broken mirror" rule rethrew it
    /// immediately, so a hanging first mirror permanently prevented the second
    /// from ever being tried.
    /// </summary>
    [Fact]
    public async Task A_timed_out_mirror_falls_through_to_the_next_one()
    {
        var payload = Payload();
        using var slow = LoopbackFileServer.Start(payload,
            new LoopbackServerOptions { SupportsRange = true, StallAfterBytes = 4_096 });
        using var good = LoopbackFileServer.Start(payload, new LoopbackServerOptions { SupportsRange = true });
        var entry = Entry([slow.Url, good.Url], payload, Sha256Of(payload), payload.Length);

        var previous = VerifiedDownloader.RequestTimeout;
        VerifiedDownloader.RequestTimeout = TimeSpan.FromMilliseconds(400);
        string path;
        try
        {
            path = await OnnxModelCatalog.EnsureAsync(
                entry, _dir, ct: TestContext.Current.CancellationToken);
        }
        finally
        {
            VerifiedDownloader.RequestTimeout = previous;
        }

        Assert.Equal(payload, await File.ReadAllBytesAsync(path, TestContext.Current.CancellationToken));
        Assert.NotEmpty(good.ReceivedRangeHeaders);   // the second mirror WAS tried
    }

    // ------------------------------------------ per-URL partial + the rename

    [Fact]
    public async Task A_prefix_from_one_mirror_is_never_resumed_into_another()
    {
        var payload = Payload();
        using var mirrorA = LoopbackFileServer.Start(payload, new LoopbackServerOptions { SupportsRange = true });
        using var mirrorB = LoopbackFileServer.Start(payload, new LoopbackServerOptions { SupportsRange = true });
        var dest = Path.Combine(_dir, "model.onnx");

        Assert.NotEqual(
            VerifiedDownloader.PartialPathFor(mirrorA.Url, dest),
            VerifiedDownloader.PartialPathFor(mirrorB.Url, dest));

        // Mirror A left a prefix behind. Mirror B must not continue it: the two
        // are different SERVERS, and appending B's body to A's prefix produces
        // a file that can only ever fail its hash.
        await File.WriteAllBytesAsync(VerifiedDownloader.PartialPathFor(mirrorA.Url, dest),
            payload[..20_000], TestContext.Current.CancellationToken);

        var written = await VerifiedDownloader.DownloadAsync(
            mirrorB.Url, dest, null, TestContext.Current.CancellationToken, Sha256Of(payload), allowResume: true);

        Assert.Equal(payload.Length, written);
        Assert.Null(Assert.Single(mirrorB.ReceivedRangeHeaders));
        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
    }

    /// <summary>The rename REPLACES; it does not delete-then-move, which left a
    /// window in which the destination existed in neither form.</summary>
    [Fact]
    public async Task An_existing_destination_is_replaced_by_the_rename()
    {
        var payload = Payload();
        using var server = LoopbackFileServer.Start(payload);
        var dest = Path.Combine(_dir, "model.onnx");
        await File.WriteAllBytesAsync(dest, "the previous model"u8.ToArray(),
            TestContext.Current.CancellationToken);

        await VerifiedDownloader.DownloadAsync(
            server.Url, dest, null, TestContext.Current.CancellationToken, Sha256Of(payload), allowResume: true);

        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
    }

    /// <summary>
    /// Two downloads of the same file are serialized. Without the gate they
    /// race over one temp file and one rename, and the loser fails on the
    /// <see cref="FileShare.None"/> handle with an IOException that looks like
    /// a disk problem. Measured by exclusion, with no clock: every holder is
    /// kept inside until all of them have announced they are about to acquire.
    /// </summary>
    [Fact]
    public async Task Downloads_of_the_same_destination_are_serialized()
    {
        var ct = TestContext.Current.CancellationToken;
        var dest = Path.Combine(_dir, "model.onnx");
        var inside = 0;
        var maxInside = 0;
        const int holders = 4;
        var remaining = holders;
        var allStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        async Task Holder()
        {
            if (Interlocked.Decrement(ref remaining) == 0) allStarted.SetResult();
            using var gate = await VerifiedDownloader.AcquireDestinationGateAsync(dest, ct);
            var now = Interlocked.Increment(ref inside);
            await allStarted.Task.WaitAsync(ct);
            int seen;
            do { seen = Volatile.Read(ref maxInside); }
            while (now > seen && Interlocked.CompareExchange(ref maxInside, now, seen) != seen);
            Interlocked.Decrement(ref inside);
        }

        await Task.WhenAll(Enumerable.Range(0, holders).Select(_ => Task.Run(Holder, ct)));
        Assert.Equal(1, Volatile.Read(ref maxInside));

        // …and a DIFFERENT destination is not blocked by it.
        using var held = await VerifiedDownloader.AcquireDestinationGateAsync(dest, ct);
        using var other = await VerifiedDownloader.AcquireDestinationGateAsync(
            Path.Combine(_dir, "other.onnx"), ct);
        Assert.NotSame(held, other);
    }
}

/// <summary>
/// Wave-2 fixes that need neither a socket nor a model file: the spectral
/// decoders' output bounds (#14), the catalogue entry validation (#15), and the
/// coverage gaps named in the audit (<c>ComputeSha256Async</c>, the pins,
/// <c>PipWheelUrl</c> / <c>PipWheelSha256</c>).
/// </summary>
public sealed class Wave2StemUnitTests
{
    // ------------------------------------------------------------ #14 bounds

    /// <summary>
    /// Both spectral drivers iterated the INPUT time dimension while indexing
    /// by the OUTPUT one. With <c>outT &lt; dimT</c> — reachable for
    /// BS-RoFormer, whose time axis is probed at runtime — every
    /// <c>t &gt;= outT</c> read the NEXT bin's samples, and the last bin read
    /// past the end of the span.
    /// </summary>
    [Fact]
    public void UnpackOutput_stops_at_the_real_output_extent_instead_of_reading_the_next_bin()
    {
        const int nFft = 8;                 // 5 bins
        const int bins = nFft / 2 + 1;
        const int dimT = 6;                 // what the INPUT said
        const int outT = 4;                 // what the model actually produced
        const int outF = bins;

        // Channel c, bin f, frame t  ->  a value that identifies all three.
        static float Expected(int c, int f, int t) => c * 1000 + f * 10 + t;

        var plane = outF * outT;
        var output = new float[4 * plane];
        for (var c = 0; c < 4; c++)
            for (var f = 0; f < outF; f++)
                for (var t = 0; t < outT; t++)
                    output[c * plane + f * outT + t] = Expected(c, f, t);

        var left = new SpectrumPlanes(dimT, nFft, 2);
        var right = new SpectrumPlanes(dimT, nFft, 2);

        SpectralOnnxRunner.UnpackOutput(output, left, right, dimF: bins, dimT: dimT, bins: bins,
            outF: outF, outT: outT);

        for (var f = 0; f < bins; f++)
        {
            for (var t = 0; t < outT; t++)
            {
                Assert.Equal(Expected(0, f, t), left.Re[t][f]);
                Assert.Equal(Expected(1, f, t), left.Im[t][f]);
                Assert.Equal(Expected(2, f, t), right.Re[t][f]);
                Assert.Equal(Expected(3, f, t), right.Im[t][f]);
            }
            // The frames the model did not produce are SILENCE, not the next
            // bin's audio.
            for (var t = outT; t < dimT; t++)
            {
                Assert.Equal(0f, left.Re[t][f]);
                Assert.Equal(0f, right.Im[t][f]);
            }
        }
    }

    /// <summary>A multi-stem unpack reads its own 4-channel group and no
    /// other.</summary>
    [Fact]
    public void UnpackOutput_reads_the_channel_group_of_the_stem_it_was_asked_for()
    {
        const int nFft = 8;
        const int bins = nFft / 2 + 1;
        const int frames = 3;
        var plane = bins * frames;
        var output = new float[8 * plane];          // two stems
        for (var c = 0; c < 8; c++)
            for (var i = 0; i < plane; i++)
                output[c * plane + i] = c;

        var left = new SpectrumPlanes(frames, nFft, 2);
        var right = new SpectrumPlanes(frames, nFft, 2);
        SpectralOnnxRunner.UnpackOutput(output, left, right, bins, frames, bins, bins, frames, baseChannel: 4);

        Assert.Equal(4f, left.Re[0][0]);
        Assert.Equal(5f, left.Im[0][0]);
        Assert.Equal(6f, right.Re[0][0]);
        Assert.Equal(7f, right.Im[0][0]);
    }

    /// <summary>A tensor too small for the shape it claims is an explicit
    /// error, not an out-of-range read inside inference.</summary>
    [Fact]
    public void UnpackOutput_rejects_a_tensor_that_is_smaller_than_its_declared_shape()
    {
        const int nFft = 8;
        const int bins = nFft / 2 + 1;
        var left = new SpectrumPlanes(2, nFft, 2);
        var right = new SpectrumPlanes(2, nFft, 2);
        var tooSmall = new float[3 * bins * 2];     // three planes where four are needed

        Assert.Throws<InvalidOperationException>(() =>
            SpectralOnnxRunner.UnpackOutput(tooSmall, left, right, bins, 2, bins, bins, 2));
    }

    // --------------------------------------------------------- #15 validation

    [Theory]
    [InlineData(@"..\..\startup.exe")]
    [InlineData("../../startup.exe")]
    [InlineData(@"sub\model.onnx")]
    [InlineData("")]
    public void An_entry_whose_file_name_escapes_the_target_directory_is_refused(string fileName)
    {
        var entry = CatalogEntry() with { FileName = fileName };
        var ex = Assert.Throws<ArgumentException>(() => OnnxModelCatalog.ValidateEntry(entry));
        Assert.Contains("file name", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData("http://example.com/model.onnx")]
    [InlineData("ftp://example.com/model.onnx")]
    [InlineData("file:///C:/model.onnx")]
    [InlineData("model.onnx")]
    public void An_entry_that_would_fetch_over_anything_but_https_is_refused(string url)
    {
        var entry = CatalogEntry() with { Urls = [url] };
        Assert.Throws<ArgumentException>(() => OnnxModelCatalog.ValidateEntry(entry));
    }

    /// <summary>Loopback http is the test server, and is not a transport an
    /// attacker is on — the existing download tests depend on it.</summary>
    [Fact]
    public void Loopback_http_is_accepted()
    {
        OnnxModelCatalog.ValidateEntry(CatalogEntry() with { Urls = ["http://127.0.0.1:8080/model.onnx"] });
        OnnxModelCatalog.ValidateEntry(CatalogEntry() with { Urls = ["http://localhost:8080/model.onnx"] });
    }

    [Fact]
    public void Every_shipped_entry_passes_the_validation_its_callers_now_get()
    {
        foreach (var entry in OnnxModelCatalog.All)
            OnnxModelCatalog.ValidateEntry(entry);
    }

    private static OnnxModelEntry CatalogEntry() =>
        new(
            Id: "test-model",
            Kind: OnnxStemModelKind.Htdemucs,
            DisplayName: "Test model",
            FileName: "model.onnx",
            Urls: ["https://example.com/model.onnx"],
            Sha256: null,
            SizeBytes: 0,
            HashSource: "test",
            SourceCount: 4,
            LicenceNote: "test");

    // ------------------------------------------------- ComputeSha256Async gap

    /// <summary>
    /// The one legitimate way to pin a model you obtained by other means. It
    /// had no test at all, and it is the value an entry's <c>Sha256</c> is
    /// compared against on every launch.
    /// </summary>
    [Fact]
    public async Task ComputeSha256Async_is_the_lower_case_hex_sha256_of_the_file()
    {
        var path = Path.Combine(Path.GetTempPath(), $"jsw2-hash-{Guid.NewGuid():N}.bin");
        var bytes = new byte[4096];
        for (var i = 0; i < bytes.Length; i++) bytes[i] = (byte)(i * 7);
        await File.WriteAllBytesAsync(path, bytes, TestContext.Current.CancellationToken);
        try
        {
            var actual = await OnnxModelCatalog.ComputeSha256Async(path, TestContext.Current.CancellationToken);
            Assert.Equal(Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(), actual);
            Assert.Equal(64, actual.Length);
            Assert.Matches("^[0-9a-f]{64}$", actual);

            // An empty file is a hash, not an exception — "the file exists" is
            // exactly the trap this whole path exists to close.
            var empty = Path.Combine(Path.GetTempPath(), $"jsw2-hash-{Guid.NewGuid():N}.bin");
            await File.WriteAllBytesAsync(empty, [], TestContext.Current.CancellationToken);
            try
            {
                Assert.Equal(
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                    await OnnxModelCatalog.ComputeSha256Async(empty, TestContext.Current.CancellationToken));
            }
            finally { File.Delete(empty); }
        }
        finally { File.Delete(path); }
    }

    // --------------------------------------------------------------- the pins

    /// <summary>
    /// Only torch and torchaudio were asserted. Every pin here is a
    /// deliberate, documented constraint whose loss is invisible until a wheel
    /// resolves differently on somebody else's machine.
    /// </summary>
    [Fact]
    public void Every_pin_keeps_the_constraint_it_was_written_for()
    {
        // numpy 2 breaks the ABI every torch < 2.3 wheel was built against.
        Assert.Equal("numpy<2", PythonPackagePins.Numpy);
        // The RoFormer family reads yaml configs.
        Assert.Equal("pyyaml", PythonPackagePins.PyYaml);
        // wav/flac IO and resampling for the sidecar.
        Assert.Equal("soundfile", PythonPackagePins.SoundFile);
        Assert.Equal("librosa", PythonPackagePins.Librosa);
        // htdemucs / mdx_extra need demucs 4; the 3.x CLI has a different
        // interface and no htdemucs.
        Assert.Equal("demucs>=4.1", PythonPackagePins.Demucs);
        Assert.Equal("bs_roformer>=1.1.0", PythonPackagePins.BsRoformer);
        Assert.Equal("onnx>=1.15", PythonPackagePins.Onnx);

        // The converter's extra pin is the converter's alone.
        Assert.Contains(PythonPackagePins.Onnx, CkptToOnnxConverter.RequiredPackages);
        Assert.DoesNotContain(PythonPackagePins.Onnx, StemSeparator.RequiredPackages);
    }

    /// <summary>
    /// The stamp-freshness check is only as good as this map: a pin that is not
    /// in it is never verified, and a pin mapped to the WRONG directory re-runs
    /// pip on every launch.
    /// </summary>
    [Fact]
    public void Every_shipped_pin_declares_the_directory_it_installs_into()
    {
        foreach (var pin in StemSeparator.RequiredPackages.Concat(CkptToOnnxConverter.RequiredPackages))
        {
            Assert.True(PythonPackagePins.InstalledDirectories.ContainsKey(pin),
                $"'{pin}' has no row in PythonPackagePins.InstalledDirectories, so the package stamp " +
                "cannot be checked against the tree for it.");
        }

        // The one that is NOT its distribution name, and the reason the map is
        // hand-written rather than derived from the spec string.
        Assert.Equal("yaml", PythonPackagePins.InstalledDirectories[PythonPackagePins.PyYaml]);
        Assert.All(PythonPackagePins.InstalledDirectories.Values,
            dir => Assert.Equal(dir, Path.GetFileName(dir)));
    }

    // ------------------------------------------------------ pip bootstrap

    /// <summary>
    /// The pip wheel is DOWNLOADED AND EXECUTED. Its source and pin are both
    /// settable, because pointing at a mirror is legitimate; the pin is what
    /// makes that safe, and <c>PythonSupplyChainTests</c> proves an unpinned or
    /// non-https source is refused.
    /// </summary>
    [Fact]
    public void The_pip_wheel_source_and_pin_are_settable_for_a_mirror()
    {
        var mgr = new PythonEnvManager(
            Path.Combine(Path.GetTempPath(), $"jsw2-env-{Guid.NewGuid():N}"));
        try
        {
            Assert.Equal(PythonEnvManager.DefaultPipWheelUrl, mgr.PipWheelUrl);
            mgr.PipWheelUrl = "https://mirror.internal/pip-26.2.1-py3-none-any.whl";
            mgr.PipWheelSha256 = new string('a', 64);
            Assert.Equal("https://mirror.internal/pip-26.2.1-py3-none-any.whl", mgr.PipWheelUrl);
            Assert.Equal(new string('a', 64), mgr.PipWheelSha256);
        }
        finally
        {
            try { Directory.Delete(mgr.Root, recursive: true); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }

    // ------------------------------------------------- ORT Terminate (source)

    /// <summary>
    /// Source lint. Setting <c>RunOptions.Terminate</c> is a P/Invoke into ORT
    /// and can throw; a throw out of a cancellation callback surfaces an ORT
    /// exception type where <c>IStemSeparator</c> promises
    /// <see cref="OperationCanceledException"/> — and a host with a "retry on
    /// CPU" branch fires it on a user cancel. There is no way to provoke that
    /// without a real ORT session, so the guard is that every registration goes
    /// through the one catch-all helper.
    /// </summary>
    [Fact]
    public void Every_cancellation_registration_in_the_onnx_package_uses_the_guarded_terminate_helper()
    {
        var root = Path.Combine(RepoRoot(), "src", "JuceSharp.StemSeparation.Onnx");
        Assert.True(Directory.Exists(root), $"'{root}' is missing — has the project moved?");

        var offenders = new List<string>();
        foreach (var file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
        {
            if (file.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}", StringComparison.Ordinal) ||
                file.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}", StringComparison.Ordinal))
                continue;
            var text = StripComments(File.ReadAllText(file));
            foreach (Match m in Regex.Matches(text, @"\.Register\s*\(([^;]*?)\)\s*;", RegexOptions.Singleline))
            {
                if (!m.Groups[1].Value.Contains("RequestTerminate", StringComparison.Ordinal))
                    offenders.Add($"{Path.GetFileName(file)}: {m.Value.Trim()}");
            }
        }

        Assert.True(offenders.Count == 0,
            "Every ct.Register in the ONNX package must route through " +
            "SpectralOnnxRunner.RequestTerminate:\n  " + string.Join("\n  ", offenders));

        // …and the helper really does swallow, rather than being a name that
        // reads like it does.
        Assert.Contains("catch (Exception ex)",
            File.ReadAllText(Path.Combine(root, "SpectralOnnxRunner.cs")), StringComparison.Ordinal);
    }

    private static string StripComments(string source) =>
        Regex.Replace(
            Regex.Replace(source, @"/\*.*?\*/", string.Empty, RegexOptions.Singleline),
            @"//[^\r\n]*", string.Empty);

    private static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "JuceSharp.sln")))
            dir = dir.Parent;
        Assert.NotNull(dir);
        return dir!.FullName;
    }
}

/// <summary>
/// The per-call concurrency request, and the two <see cref="StemSeparator"/>
/// surfaces that had no test. Shares the collection that owns the
/// process-global gate.
/// </summary>
[Collection("StemSeparatorConcurrency")]
public sealed class Wave2StemSeparatorGateTests
{
    /// <summary>
    /// A <c>MaxConcurrentSeparations = 8</c> on ONE call used to raise the
    /// process-wide ceiling permanently — for every future default-options
    /// caller, i.e. for exactly the callers the VRAM gate exists for.
    /// </summary>
    [Fact]
    public async Task A_per_call_concurrency_request_does_not_widen_the_gate_for_everybody_else()
    {
        var ct = TestContext.Current.CancellationToken;
        StemSeparator.ResetGlobalGateForTests();
        try
        {
            // One caller opts into 8. It gets its slot…
            (await StemSeparator.AcquireSeparationSlotAsync(8, ct)).Dispose();
            // …and the standing policy has not moved.
            Assert.Equal(1, StemSeparator.GlobalConcurrencyLimit);

            // The next default-options callers are STILL serialized, which is
            // the whole point.
            Assert.Equal(1, await MaxConcurrentSlotHoldersAsync(requested: 1, holders: 3, expected: 1, ct));

            // …while an explicit opt-in still parallelises, so the feature is
            // not simply gone.
            Assert.Equal(3, await MaxConcurrentSlotHoldersAsync(requested: 4, holders: 3, expected: 3, ct));

            // And after all that, the default is untouched.
            Assert.Equal(1, StemSeparator.GlobalConcurrencyLimit);
            Assert.Equal(1, await MaxConcurrentSlotHoldersAsync(requested: 1, holders: 2, expected: 1, ct));
        }
        finally
        {
            StemSeparator.ResetGlobalGateForTests();
        }
    }

    /// <summary>Raising the standing policy is the host's call, and that one
    /// DOES apply to everybody.</summary>
    [Fact]
    public async Task Raising_the_global_limit_lets_default_callers_run_in_parallel()
    {
        var ct = TestContext.Current.CancellationToken;
        StemSeparator.ResetGlobalGateForTests();
        try
        {
            StemSeparator.GlobalConcurrencyLimit = 3;
            Assert.Equal(3, await MaxConcurrentSlotHoldersAsync(requested: 1, holders: 3, expected: 3, ct));
        }
        finally
        {
            StemSeparator.ResetGlobalGateForTests();
        }
    }

    /// <summary>
    /// How many slot holders were ever inside at once. No clock: every holder
    /// is kept inside until all of them have announced they are about to
    /// acquire, so the answer is the gate's, not the scheduler's.
    /// </summary>
    /// <summary>
    /// The high-water mark of holders inside the gate AT ONCE.
    ///
    /// `expected` is what makes this deterministic, and it is load-bearing. The
    /// first version signalled when every holder had STARTED — i.e. before any
    /// of them had acquired — and each holder recorded its own arrival index
    /// rather than a simultaneous count, so a holder could acquire, record 1,
    /// release and exit before the next one even ran. It under-reported on a
    /// busy box (measured: 2 where 3 holders genuinely overlapped) and, worse,
    /// it would have reported 1 for a gate that was wide open, which is the
    /// direction that hides a bug.
    ///
    /// Now each holder parks inside the gate until `expected` of them are in
    /// there together. If the gate really is that wide they all arrive and the
    /// answer is exact; if it is NARROWER the rendezvous never completes and
    /// the wait expires, returning the smaller number actually observed so the
    /// caller's Assert.Equal reports the truth. The timeout is a DEADLOCK
    /// DETECTOR, not a budget — nothing is asserted about how long it took.
    /// </summary>
    private static async Task<int> MaxConcurrentSlotHoldersAsync(
        int requested, int holders, int expected, CancellationToken ct)
    {
        var maxInside = 0;
        var inside = 0;
        var rendezvous = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        async Task Holder()
        {
            using var slot = await StemSeparator.AcquireSeparationSlotAsync(requested, ct);
            var now = Interlocked.Increment(ref inside);

            int seen;
            do { seen = Volatile.Read(ref maxInside); }
            while (now > seen && Interlocked.CompareExchange(ref maxInside, now, seen) != seen);

            if (now >= expected) rendezvous.TrySetResult();
            // Hold the slot until the group is assembled, so "at once" is real.
            try { await rendezvous.Task.WaitAsync(TimeSpan.FromSeconds(30), ct); }
            catch (TimeoutException) { /* gate is narrower than `expected`; the caller asserts */ }

            Interlocked.Decrement(ref inside);
        }

        await Task.WhenAll(Enumerable.Range(0, holders).Select(_ => Task.Run(Holder, ct)));
        return Volatile.Read(ref maxInside);
    }

    /// <summary>
    /// <see cref="StemSeparator.IsReadyAsync"/> is the UI gate behind "Separate
    /// stems" and had no test. It must be false for a half-extracted root:
    /// "python.exe exists" is not "the interpreter is usable", which is the
    /// fast path a peer process reads before the bootstrap gate is taken.
    /// </summary>
    [Fact]
    public async Task IsReadyAsync_is_false_until_both_the_interpreter_and_torch_are_really_there()
    {
        if (!OperatingSystem.IsWindows())
            Assert.Skip("The embedded-interpreter layout this asserts is the Windows one.");

        var root = Path.Combine(Path.GetTempPath(), $"jsw2-ready-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        var ct = TestContext.Current.CancellationToken;
        try
        {
            var sep = new StemSeparator(new PythonEnvManager(root));
            Assert.False(await sep.IsReadyAsync(ct));

            // Half an extract: python.exe has landed, the stdlib zip and the
            // patched ._pth have not.
            await File.WriteAllTextAsync(Path.Combine(root, "python.exe"), "not really python", ct);
            Directory.CreateDirectory(Path.Combine(root, "Lib", "site-packages", "torch"));
            Assert.False(await sep.IsReadyAsync(ct));
            Assert.False((await sep.ProbeAsync(ct)).InterpreterPresent);

            // The stamp is what says the extract finished.
            await File.WriteAllTextAsync(
                Path.Combine(root, PythonEnvManager.InterpreterReadyStampName), "3.11.9", ct);
            Assert.True(await sep.IsReadyAsync(ct));
            Assert.True(sep.IsReady());

            // Torch gone => not ready, even though the interpreter is fine.
            Directory.Delete(Path.Combine(root, "Lib", "site-packages", "torch"));
            Assert.False(await sep.IsReadyAsync(ct));
            Assert.True((await sep.ProbeAsync(ct)).InterpreterPresent);
        }
        finally
        {
            try { Directory.Delete(root, recursive: true); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }

    /// <summary>
    /// A pre-stamp root — one a previous release installed — is COMPLETE and
    /// must be adopted, not re-extracted, or upgrading the library re-downloads
    /// everybody's interpreter.
    /// </summary>
    [Fact]
    public void A_root_installed_before_the_ready_stamp_existed_is_still_recognised()
    {
        if (!OperatingSystem.IsWindows())
            Assert.Skip("The embedded-interpreter layout this asserts is the Windows one.");

        var root = Path.Combine(Path.GetTempPath(), $"jsw2-legacy-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            File.WriteAllText(Path.Combine(root, "python.exe"), "not really python");
            File.WriteAllText(Path.Combine(root, "python311.zip"), "stdlib");
            File.WriteAllText(Path.Combine(root, "python311._pth"), "python311.zip\r\n.\r\nimport site\r\n");

            var mgr = new PythonEnvManager(root);
            Assert.True(mgr.Probe().InterpreterPresent);
            // …and it is stamped on the way past, so the check costs nothing
            // the second time.
            Assert.True(File.Exists(Path.Combine(root, PythonEnvManager.InterpreterReadyStampName)));
        }
        finally
        {
            try { Directory.Delete(root, recursive: true); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }

    /// <summary>
    /// The constructor sweep ran unconditionally over every <c>*.tmp.*</c> in
    /// the root, which is another manager's IN-FLIGHT download — a
    /// configuration this class's own comments call normal.
    /// </summary>
    [Fact]
    public void The_stale_temp_sweep_leaves_a_download_that_is_still_being_written()
    {
        var root = Path.Combine(Path.GetTempPath(), $"jsw2-sweep-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            var inFlight = Path.Combine(root, "python-embed.zip.tmp.aaaa");
            var orphan = Path.Combine(root, "python-embed.zip.tmp.bbbb");
            File.WriteAllText(orphan, "left by a crashed boot");

            // Exactly how VerifiedDownloader holds its temp file.
            using (new FileStream(inFlight, FileMode.Create, FileAccess.Write, FileShare.None))
            {
                var mgr = new PythonEnvManager(root);   // the ctor sweeps
                Assert.True(File.Exists(inFlight), "an in-flight download must survive another manager's sweep");
                Assert.False(File.Exists(orphan), "a genuinely orphaned partial must still be swept");
                Assert.Equal(0, mgr.CleanStaleTempFiles());
            }

            // Once the writer lets go, it is sweepable like any other orphan.
            Assert.True(File.Exists(inFlight));
            _ = new PythonEnvManager(root);
            Assert.False(File.Exists(inFlight));
        }
        finally
        {
            try { Directory.Delete(root, recursive: true); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }
}
