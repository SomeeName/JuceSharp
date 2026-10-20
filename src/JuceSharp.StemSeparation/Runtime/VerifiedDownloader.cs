using System.Collections.Concurrent;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;

namespace JuceSharp.StemSeparation.Runtime;

/// <summary>One sample of a file download's progress.</summary>
/// <param name="BytesReceived">Bytes on disk so far, INCLUDING the bytes a
///   resumed <c>.part</c> file already held.</param>
/// <param name="TotalBytes">Total size of the finished file when the server
///   declared one, else null. A chunked response has no length, and that is
///   the case a truncating proxy produces — treat null as "unknown", never as
///   zero.</param>
/// <param name="Fraction">0..1 when <paramref name="TotalBytes"/> is known,
///   else -1. A UI shows an indeterminate bar for -1.</param>
/// <param name="Resumed">True when the transfer continued an existing partial
///   file rather than starting from byte 0.</param>
public readonly record struct DownloadProgress(
    long BytesReceived, long? TotalBytes, double Fraction, bool Resumed);

/// <summary>
/// The ONE place in the stem-separation packages that fetches bytes over HTTP.
///
/// <para>
/// <b>Why one place.</b> Everything downloaded here is subsequently EXECUTED or
/// loaded as a model: the embedded Python archive is unzipped and run, an ONNX
/// graph is handed to ONNX Runtime. TLS is not the whole trust story for that —
/// a substituted mirror, a corporate MITM proxy or a truncated transfer all
/// produce a file that arrived over a valid HTTPS connection. So every download
/// goes: temp file → hash the COMPLETE file → only then rename into place. A
/// second implementation of that sequence is a second chance to get the order
/// wrong, which is why <c>PythonEnvManager.DownloadWithProgress</c> is now a
/// forwarder and why <c>OnnxModelCatalog</c> does not own an
/// <see cref="HttpClient"/>. <c>HttpDownloadLintTests</c> fails the build if a
/// third one appears.
/// </para>
///
/// <para>
/// <b>Two temp-file policies, deliberately.</b>
/// <list type="bullet">
///   <item><b>Non-resumable</b> (<c>allowResume: false</c>, the Python
///     bootstrap): writes <c>{dest}.tmp.{guid}</c> and DELETES it on any
///     failure. A half-formed interpreter archive left on disk is a trap for
///     the next boot, and `PythonEnvManager.CleanStaleTempFiles` sweeps the
///     shape.</item>
///   <item><b>Resumable</b> (<c>allowResume: true</c>, model downloads):
///     writes <c>{dest}.part</c> and KEEPS it on cancellation or a network
///     failure, so the next attempt continues instead of re-fetching 300 MB.
///     It is deleted on a CHECKSUM failure, because those bytes are not a
///     prefix worth resuming. <b>The name carries a digest of the URL</b>
///     (<see cref="PartialPathFor"/>): a mirror list is a list of SERVERS, and
///     a prefix fetched from mirror A is not a prefix of what mirror B is
///     about to send — resuming one into the other concatenates two servers'
///     bytes and the result can only ever fail its hash.</item>
/// </list>
/// </para>
///
/// <para>
/// <b>A TIMEOUT is not a cancel.</b> <see cref="HttpClient.Timeout"/> covers
/// the whole operation including the content-stream reads, and it surfaces as
/// <c>TaskCanceledException : OperationCanceledException</c> — indistinguishable
/// from the user pressing Cancel, which made a slow first mirror permanently
/// prevent the second from being tried and made every consumer of the stem
/// pipeline report a user cancel. The budget therefore lives here, on a linked
/// token (<see cref="RequestTimeout"/>), and expiry is translated into an
/// <see cref="IOException"/> before it can leave this type.
/// </para>
/// </summary>
internal static class VerifiedDownloader
{
    /// <summary>
    /// Shared client. It has NO timeout of its own — see the class remarks;
    /// the budget is applied per call on a linked token so that expiry is
    /// distinguishable from the caller's own cancellation.
    /// <see cref="SocketsHttpHandler.PooledConnectionLifetime"/> keeps a
    /// process-lifetime client from pinning a mirror's resolved IP for ever.
    /// </summary>
    private static readonly HttpClient Http = new(new SocketsHttpHandler
    {
        PooledConnectionLifetime = TimeSpan.FromMinutes(5),
    })
    {
        Timeout = System.Threading.Timeout.InfiniteTimeSpan,
    };

    /// <summary>
    /// Whole-transfer budget. A resumable download is why this can be generous:
    /// a 300 MB model on a slow link may exceed it, and resuming turns that
    /// from "start again" into "carry on". Settable for tests only.
    /// </summary>
    internal static TimeSpan RequestTimeout { get; set; } = TimeSpan.FromMinutes(10);

    /// <summary>Suffix of a resumable partial download.</summary>
    internal const string PartialSuffix = ".part";

    /// <summary>
    /// Where the resumable prefix for <paramref name="url"/> →
    /// <paramref name="dest"/> lives. Keyed on the URL, not just the
    /// destination, so a mirror failover never appends one server's body to
    /// another server's prefix.
    /// </summary>
    internal static string PartialPathFor(string url, string dest)
    {
        var digest = Convert.ToHexString(
            SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(url ?? string.Empty)))[..8].ToLowerInvariant();
        return $"{dest}.{digest}{PartialSuffix}";
    }

    // --- One download per destination, process-wide ---------------------
    //
    // Two downloads of the same dest race over the same temp file and the
    // same rename; the FileShare.None handle turns that into a mystery
    // IOException from whichever lost. The python side has
    // PythonEnvManager.AcquireEnvLockAsync for exactly this shape; the model
    // side had nothing.
    //
    // REFERENCE-COUNTED, and that is not gold-plating: nothing ever removed an
    // entry, so this grew one SemaphoreSlim plus a normalised path per
    // destination for the life of the process — and "a static dictionary keyed
    // by a path, never pruned" is a leak the moment a host downloads into
    // per-song directories (W4-S18). The count is mutated only under
    // GatesLock, and an entry is removed only when nobody HOLDS or WAITS on it,
    // so a second caller can never be handed a fresh semaphore while the first
    // still owns the old one.
    private static readonly Dictionary<string, GateEntry> DestinationGates =
        new(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);

    private static readonly object GatesLock = new();

    private sealed class GateEntry
    {
        internal SemaphoreSlim Semaphore { get; } = new(1, 1);
        internal int Refs { get; set; }
    }

    /// <summary>Live gate count. Tests assert this returns to zero.</summary>
    internal static int DestinationGateCount
    {
        get { lock (GatesLock) return DestinationGates.Count; }
    }

    /// <summary>
    /// Serializes downloads that target the same file. Dispose to release.
    /// Exposed to tests so the exclusion can be proven without a server.
    /// </summary>
    internal static async Task<IDisposable> AcquireDestinationGateAsync(string dest, CancellationToken ct)
    {
        var key = Path.GetFullPath(dest);
        GateEntry entry;
        lock (GatesLock)
        {
            if (!DestinationGates.TryGetValue(key, out entry!))
                DestinationGates[key] = entry = new GateEntry();
            entry.Refs++;
        }

        try
        {
            await entry.Semaphore.WaitAsync(ct).ConfigureAwait(false);
        }
        catch
        {
            // Cancelled while queued: drop the reference we took, or the entry
            // outlives every user of it — the leak this refcount exists to stop.
            Unreference(key);
            throw;
        }

        return new GateHandle(key, entry);
    }

    private static void Unreference(string key)
    {
        lock (GatesLock)
        {
            if (!DestinationGates.TryGetValue(key, out var e)) return;
            if (--e.Refs <= 0) DestinationGates.Remove(key);
        }
    }

    private sealed class GateHandle(string key, GateEntry entry) : IDisposable
    {
        private int _disposed;
        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            entry.Semaphore.Release();
            Unreference(key);
        }
    }

    /// <summary>
    /// Downloads <paramref name="url"/> to <paramref name="dest"/>, verifying
    /// the COMPLETE file against <paramref name="expectedSha256"/> before the
    /// atomic rename. Returns the number of bytes in the finished file.
    /// </summary>
    /// <param name="url">Source URL.</param>
    /// <param name="dest">Destination path; only created on success.</param>
    /// <param name="progress">Optional progress sink.</param>
    /// <param name="ct">Cancellation token. Cancelling throws
    ///   <see cref="OperationCanceledException"/>; with
    ///   <paramref name="allowResume"/> the partial file is kept.</param>
    /// <param name="expectedSha256">Hex SHA-256, case-insensitive. Null skips
    ///   verification — the CALLER logs that, because "unverified" is a
    ///   decision, not a default.</param>
    /// <param name="allowResume">Enables the <c>.part</c> + HTTP Range policy
    ///   described on the class.</param>
    /// <param name="expectedSizeBytes">Exact size of the finished file when the
    ///   caller knows it, else 0. Checked BEFORE the rename, so a short body
    ///   never reaches the destination — with no pinned hash it is the only
    ///   integrity check there is.</param>
    internal static async Task<long> DownloadAsync(
        string url,
        string dest,
        Action<DownloadProgress>? progress,
        CancellationToken ct,
        string? expectedSha256 = null,
        bool allowResume = false,
        long expectedSizeBytes = 0)
    {
        using var destinationGate = await AcquireDestinationGateAsync(dest, ct).ConfigureAwait(false);
        return await DownloadGateHeldAsync(url, dest, progress, ct, expectedSha256, allowResume,
            expectedSizeBytes).ConfigureAwait(false);
    }

    /// <summary>
    /// <see cref="DownloadAsync"/> for a caller that ALREADY holds this
    /// destination's gate — <see cref="VerifiedDownload.EnsureFileAsync"/>, whose
    /// check-hash-and-delete step has to be inside the same exclusion as the
    /// transfer. The gate is a non-reentrant <see cref="SemaphoreSlim"/>, so
    /// going through <see cref="DownloadAsync"/> from in there would deadlock.
    /// </summary>
    internal static Task<long> DownloadGateHeldAsync(
        string url,
        string dest,
        Action<DownloadProgress>? progress,
        CancellationToken ct,
        string? expectedSha256 = null,
        bool allowResume = false,
        long expectedSizeBytes = 0)
        => DownloadCoreAsync(url, dest, progress, ct, expectedSha256, allowResume,
                             expectedSizeBytes, resume: allowResume);

    /// <param name="resume">False on the ONE retry a 416 is allowed to trigger:
    ///   without it the retry recomputes the same prefix length, sends the same
    ///   <c>Range</c>, is answered 416 again and recurses until the stack runs
    ///   out — which is a process kill, not a failed download.</param>
    /// <param name="url">Absolute source URL.</param>
    /// <param name="dest">Final destination path; written only after verification.</param>
    /// <param name="progress">Optional progress sink, called on the caller's thread.</param>
    /// <param name="ct">The CALLER's token. Only this one yields
    ///   <see cref="OperationCanceledException"/>; the internal request budget is
    ///   translated to <see cref="IOException"/> so a slow mirror is not mistaken
    ///   for a user cancel.</param>
    /// <param name="expectedSha256">Pinned content hash, or null to accept any bytes.</param>
    /// <param name="allowResume">Whether a surviving <c>.part</c> prefix may be resumed.</param>
    /// <param name="expectedSizeBytes">Expected final size, or 0 when unknown. Checked
    ///   BEFORE the rename, which is what catches a close-delimited truncated body that
    ///   no declared Content-Length would have revealed.</param>
    private static async Task<long> DownloadCoreAsync(
        string url,
        string dest,
        Action<DownloadProgress>? progress,
        CancellationToken ct,
        string? expectedSha256,
        bool allowResume,
        long expectedSizeBytes,
        bool resume)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(dest));
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        var tmp = allowResume ? PartialPathFor(url, dest) : $"{dest}.tmp.{Guid.NewGuid():N}";

        // How many bytes of a previous attempt we can keep. Only ever non-zero
        // on the resumable path, and never on the post-416 retry.
        long alreadyHave = 0;
        if (resume && File.Exists(tmp))
        {
            try { alreadyHave = new FileInfo(tmp).Length; }
            catch (IOException) { alreadyHave = 0; }
        }

        // The whole-transfer budget lives here rather than on HttpClient, so
        // that expiry is ours to recognise: see the class remarks.
        using var budget = CancellationTokenSource.CreateLinkedTokenSource(ct);
        budget.CancelAfter(RequestTimeout);
        var lct = budget.Token;

        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            if (alreadyHave > 0) request.Headers.Range = new RangeHeaderValue(alreadyHave, null);

            using var resp = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, lct)
                .ConfigureAwait(false);

            // 416: our partial is already >= the resource (the file changed, or
            // a previous run wrote past the end). Throw the prefix away and
            // start over rather than producing a file that can never hash.
            if (alreadyHave > 0 && resp.StatusCode == HttpStatusCode.RequestedRangeNotSatisfiable)
            {
                TryDelete(tmp);
                // TryDelete is best-effort and swallows everything (an AV scan,
                // a read-only file, another handle). Retrying with the prefix
                // still on disk is the unbounded-recursion case; fail instead.
                if (File.Exists(tmp))
                    throw new IOException(
                        $"'{url}' rejected the resume range and the stale prefix '{tmp}' could not be " +
                        "deleted, so the download cannot be restarted. Remove the file and try again.");
                return await DownloadCoreAsync(url, dest, progress, ct, expectedSha256, allowResume,
                    expectedSizeBytes, resume: false).ConfigureAwait(false);
            }

            resp.EnsureSuccessStatusCode();

            // THE STATUS CODE IS THE AUTHORITY, not the Accept-Ranges header.
            // A header is a claim; 206-vs-200 is what the server actually did.
            // A server that ignores Range answers 200 with the WHOLE body, and
            // appending that to our prefix would silently corrupt the file —
            // so a 200 restarts from byte 0 (FileMode.Create truncates).
            // …and a 206 says the server honoured *A* range, never that it
            // honoured OURS. Content-Range is the only place it says which, and
            // not reading it left two reachable corruptions: a proxy that
            // normalises the range to the whole resource answers
            // `206 Content-Range: bytes 0-N/N`, which this APPENDED to the
            // existing prefix (a file of alreadyHave + N bytes), and a
            // `multipart/byteranges` reply has MIME part headers inside the
            // body that went in verbatim. With a pinned hash that is a wasted
            // transfer; with Sha256 = null — which EnsureFileAsync permits and
            // the get-pip bootstrap uses — it is a corrupt file that gets
            // EXECUTED. A 206 we cannot confirm is re-requested WITHOUT a Range
            // header rather than written from byte 0, because the body in hand
            // may itself be a partial one and there is no way to tell which.
            if (alreadyHave > 0 && resp.StatusCode == HttpStatusCode.PartialContent
                && !RangeMatchesOurs(resp, alreadyHave))
            {
                TryDelete(tmp);
                if (File.Exists(tmp))
                    throw new IOException(
                        $"'{url}' answered our resume range with a different one, and the stale prefix " +
                        $"'{tmp}' could not be deleted, so the download cannot be restarted. " +
                        "Remove the file and try again.");
                return await DownloadCoreAsync(url, dest, progress, ct, expectedSha256, allowResume,
                    expectedSizeBytes, resume: false).ConfigureAwait(false);
            }

            var resumed = alreadyHave > 0 && resp.StatusCode == HttpStatusCode.PartialContent;
            var startAt = resumed ? alreadyHave : 0;

            var declared = resp.Content.Headers.ContentLength;
            long? total = declared is null ? null : startAt + declared.Value;

            var read = startAt;
            await using (var src = await resp.Content.ReadAsStreamAsync(lct).ConfigureAwait(false))
            await using (var dst = new FileStream(tmp,
                             resumed ? FileMode.Append : FileMode.Create,
                             FileAccess.Write, FileShare.None,
                             1 << 20, FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                Report(progress, read, total, resumed);

                var buf = new byte[1 << 20];
                int n;
                while ((n = await src.ReadAsync(buf, lct).ConfigureAwait(false)) > 0)
                {
                    await dst.WriteAsync(buf.AsMemory(0, n), lct).ConfigureAwait(false);
                    read += n;
                    Report(progress, read, total, resumed);
                }
                await dst.FlushAsync(lct).ConfigureAwait(false);
            }

            // A CLEAN early close — a proxy, a flaky mirror — makes ReadAsync
            // return 0, the loop exit normally and FlushAsync succeed. With a
            // pinned hash the next line catches it; with none (the shipping
            // get-pip default, and every allowUnverified model) the truncated
            // file would be renamed into place and then EXECUTED. The declared
            // length is the evidence we have, so use it.
            if (declared is { } declaredLength && read - startAt != declaredLength)
                throw new IOException(
                    $"'{url}' declared {declaredLength} bytes for this transfer but delivered " +
                    $"{read - startAt}. The download was truncated.");

            // Same check one level up: the catalogue's own size, when it has
            // one. Checking it AFTER the rename (which is where it used to
            // live) leaves a window where a concurrent reader sees a bad model
            // at the canonical path.
            if (expectedSizeBytes > 0 && read != expectedSizeBytes)
                throw new InvalidDataException(
                    $"'{url}' delivered {read} bytes; {expectedSizeBytes} were expected. " +
                    "The download was discarded.");

            // Verify BEFORE the rename, and verify the WHOLE file — a resumed
            // download is only trustworthy end to end, because the bytes it
            // kept came from an earlier, unverified attempt.
            await VerifyFileHashAsync(tmp, expectedSha256, url, ct).ConfigureAwait(false);

            // Atomic swap. File.Move with overwrite=true is atomic on Windows
            // for same-volume targets; ditto rename(2) on POSIX. A concurrent
            // reader sees the old file or the new one, never a half-written one
            // and never — as it did while this pre-deleted dest — NEITHER.
            File.Move(tmp, dest, overwrite: true);
            progress?.Invoke(new DownloadProgress(read, total ?? read, 1.0, resumed));
            return read;
        }
        catch (Exception ex) when (budget.IsCancellationRequested && !ct.IsCancellationRequested
                                   && ex is OperationCanceledException or IOException)
        {
            // The BUDGET expired, not the caller's token — and that distinction
            // is the whole fix: surfacing it as OperationCanceledException made
            // OnnxModelCatalog skip its remaining mirrors ("a user Cancel is
            // not a broken mirror") and made every stem consumer report a user
            // cancel. Both shapes are caught: a cancelled read is a
            // TaskCanceledException through most of the stack and, from some
            // handler paths, an IOException wrapping one.
            if (!allowResume) TryDelete(tmp);
            throw new IOException(
                $"Downloading '{url}' timed out after {RequestTimeout}. " +
                (allowResume ? "The partial file was kept, so the next attempt resumes." : string.Empty), ex);
        }
        catch (Exception ex)
        {
            // A cancelled or failed RESUMABLE transfer keeps its prefix: that
            // is the whole point. A non-resumable one must leave nothing.
            // A checksum failure has already deleted the file in either case
            // (VerifyFileHashAsync), so the prefix we keep is never one we know
            // to be wrong.
            if (!allowResume || ex is InvalidDataException) TryDelete(tmp);
            throw;
        }
    }

    private static void Report(Action<DownloadProgress>? progress, long read, long? total, bool resumed)
    {
        if (progress is null) return;
        var fraction = total is > 0 ? Math.Min(1.0, read / (double)total.Value) : -1.0;
        progress(new DownloadProgress(read, total, fraction, resumed));
    }

    /// <summary>
    /// Hashes <paramref name="path"/> and compares it with
    /// <paramref name="expectedSha256"/> (hex, case-insensitive). A mismatch
    /// DELETES the file and throws <see cref="InvalidDataException"/> — a
    /// payload that fails its pin must not be left on disk, where the resume
    /// path would otherwise pick it up as a prefix worth continuing. A null
    /// expectation is a no-op (the caller logs that).
    /// </summary>
    internal static async Task VerifyFileHashAsync(
        string path, string? expectedSha256, string source, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(expectedSha256)) return;

        var actual = await ComputeSha256Async(path, ct).ConfigureAwait(false);
        if (string.Equals(actual, expectedSha256.Trim(), StringComparison.OrdinalIgnoreCase)) return;

        TryDelete(path);
        throw new InvalidDataException(
            $"Checksum mismatch for '{source}': expected SHA-256 {expectedSha256.Trim().ToLowerInvariant()}, " +
            $"got {actual}. The download was discarded.");
    }

    /// <summary>Lower-case hex SHA-256 of a file.</summary>
    internal static async Task<string> ComputeSha256Async(string path, CancellationToken ct)
    {
        await using var s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read,
            1 << 20, FileOptions.Asynchronous | FileOptions.SequentialScan);
        return Convert.ToHexString(await SHA256.HashDataAsync(s, ct).ConfigureAwait(false))
            .ToLowerInvariant();
    }

    /// <summary>
    /// Does this 206 describe the range we asked for — <c>bytes {alreadyHave}-…</c>
    /// of a single byte range? A missing or unparsable <c>Content-Range</c>, a
    /// unit other than <c>bytes</c>, a different start, or a
    /// <c>multipart/byteranges</c> body (whose MIME part headers are IN the
    /// bytes) all answer no.
    /// </summary>
    private static bool RangeMatchesOurs(HttpResponseMessage resp, long alreadyHave)
    {
        var mediaType = resp.Content.Headers.ContentType?.MediaType;
        if (mediaType is not null
            && mediaType.StartsWith("multipart/byteranges", StringComparison.OrdinalIgnoreCase))
            return false;

        var cr = resp.Content.Headers.ContentRange;
        if (cr is null) return false;
        if (!string.Equals(cr.Unit, "bytes", StringComparison.OrdinalIgnoreCase)) return false;
        if (!cr.HasRange || cr.From is null) return false;
        return cr.From.Value == alreadyHave;
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch (Exception) { /* best effort */ }
    }
}
