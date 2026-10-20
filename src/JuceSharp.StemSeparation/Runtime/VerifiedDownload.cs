namespace JuceSharp.StemSeparation.Runtime;

/// <summary>
/// The public, KIND-LESS face of <c>VerifiedDownloader</c>: fetch one file from
/// one URL to one path, verified, resumable, atomically renamed.
///
/// <para>
/// <b>Why this exists.</b> Until 0.9 the only public route into the verified
/// download path was
/// <c>OnnxModelCatalog.EnsureAsync</c>, which takes an <c>OnnxModelEntry</c>
/// — and an entry carries an <c>OnnxStemModelKind</c> naming the INFERENCE
/// DRIVER that consumes the file. A host with a ggml weight for the
/// demucs.cpp backend, or a user-supplied <c>.ckpt</c>, has no honest value for
/// that field, so DotCharter dressed both as "the nearest ONNX kind" purely to
/// reach the downloader. A field that is a lie so the call compiles is worse
/// than no field: the next reader believes it. Everything downstream of the
/// entry ignores <c>Kind</c> anyway, so the fix is a door that never asks.
/// </para>
///
/// <para>
/// <b>This is not a second implementation.</b> Every method here forwards to
/// the one <c>VerifiedDownloader</c> that both stem packages share, so the
/// <c>.part</c>-per-URL resume policy, the status-code-not-header resume
/// decision, the length check before the rename, the hash of the WHOLE file
/// before the rename and the one-download-per-destination gate are the same
/// code the ONNX catalogue runs — <c>OnnxModelCatalog.EnsureAsync</c> now calls
/// <see cref="EnsureFileAsync"/> per mirror rather than the downloader
/// directly, and <c>HttpDownloadLintTests</c> keeps the network in one file.
/// </para>
///
/// <para>
/// <b>What it does NOT do.</b> There is no mirror list and no catalogue policy
/// here — one URL, one destination. A caller that wants mirror failover, an
/// <c>allowUnverified</c> decision or a table of pins wants a CATALOGUE
/// (<c>OnnxModelCatalog</c>, <see cref="DemucsGgmlModelCatalog"/>), both of
/// which are built on exactly this method.
/// </para>
/// </summary>
public static class VerifiedDownload
{
    /// <summary>
    /// Ensures the file at <paramref name="destinationPath"/> is present and
    /// CORRECT, downloading it from <paramref name="url"/> if it is not, and
    /// returns the destination path.
    ///
    /// <para>
    /// <b>An existing file is checked, not trusted.</b> With a hash it is
    /// re-hashed; with no hash but an expected size its length is checked. A
    /// file that fails is DELETED and fetched again, because "the file exists"
    /// ⇒ "it is installed" is how a half-written download from a killed
    /// process becomes a permanent, mystifying load failure. A file with
    /// neither a hash nor a size to check it against is kept, with a warning:
    /// there is nothing to check it against, and deleting it would throw away
    /// a good file on every launch.
    /// </para>
    /// <para>
    /// <b>Verification happens before the rename.</b> Bytes land in a
    /// per-URL <c>.part</c> file, the COMPLETE file is hashed and length-checked
    /// there, and only then is it moved into place — so a reader never sees a
    /// bad file at the canonical path. The <c>.part</c> file is KEPT on
    /// cancellation or a network failure so the next call resumes with an HTTP
    /// <c>Range</c> request, and deleted on a checksum failure.
    /// </para>
    /// <para>
    /// <b>A timeout is an <see cref="IOException"/>, never an
    /// <see cref="OperationCanceledException"/></b>, so a caller walking a
    /// mirror list can tell a slow server from a user pressing Cancel. Only
    /// <paramref name="ct"/> produces a cancellation.
    /// </para>
    /// </summary>
    /// <param name="url">Absolute <c>https</c> source URL (plain <c>http</c> is
    ///   accepted for loopback only — that is a test server, not a transport an
    ///   attacker is on).</param>
    /// <param name="destinationPath">Full path of the finished file. Its
    ///   directory is created if needed. Nothing is written there until the
    ///   download has passed every check.</param>
    /// <param name="sha256">Lower-case hex SHA-256 of the complete file, or
    ///   <c>null</c> to accept whatever arrives. <b>Null is a decision the
    ///   CALLER is making</b> — this method warns through
    ///   <paramref name="log"/> and proceeds, because it has no catalogue to
    ///   consult about whether an unpinned fetch is acceptable here.</param>
    /// <param name="expectedBytes">Exact size of the complete file when the
    ///   caller knows it, else <c>null</c>. An integrity constraint, not a UI
    ///   hint: it is the only check available against a close-delimited
    ///   response that declares no <c>Content-Length</c>, which is exactly what
    ///   a truncating proxy produces.</param>
    /// <param name="progress">Optional progress sink, invoked on the thread
    ///   doing the reads.</param>
    /// <param name="log">Optional log sink for the warnings above. There is
    ///   deliberately no fallback to a process-global logger.</param>
    /// <param name="ct">Cancellation token. Cancelling throws
    ///   <see cref="OperationCanceledException"/> and keeps the resumable
    ///   partial file.</param>
    /// <param name="requireHash">Opt in to CATALOGUE-strength policy for this
    ///   one URL: a <c>null</c> <paramref name="sha256"/> is refused with an
    ///   <see cref="InvalidOperationException"/> instead of warned about.
    ///   <para>This is deliberately opt-in and deliberately NOT the default.
    ///   The two download doors disagree on unpinned fetches on purpose: a
    ///   CATALOGUE knows its rows were supposed to be pinned and refuses
    ///   (<c>OnnxModelCatalog</c>, <c>DemucsGgmlModelCatalog</c>), while this is
    ///   the one-URL primitive with no table to consult. (Every fetch this
    ///   package makes on its own — the interpreter, the pip wheel, the Demucs
    ///   weights — IS pinned; the pip wheel and the weights pass this flag.) The gap this
    ///   closes is narrower than either: a host that WANTS the catalogue's
    ///   refusal for a single URL had to re-implement it.</para></param>
    /// <returns><paramref name="destinationPath"/>.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="url"/> is null.</exception>
    /// <exception cref="ArgumentException"><paramref name="destinationPath"/> is
    ///   blank, or <paramref name="url"/> is not absolute https/loopback-http.</exception>
    /// <exception cref="InvalidDataException">The completed download did not
    ///   match <paramref name="sha256"/> or <paramref name="expectedBytes"/>.
    ///   No file is left at the destination.</exception>
    /// <exception cref="IOException">The transfer was truncated, timed out, or
    ///   an unusable existing file could not be replaced.</exception>
    /// <exception cref="HttpRequestException">The server refused the request.</exception>
    public static async Task<string> EnsureFileAsync(
        Uri url,
        string destinationPath,
        string? sha256 = null,
        long? expectedBytes = null,
        IProgress<DownloadProgress>? progress = null,
        Action<string>? log = null,
        CancellationToken ct = default,
        bool requireHash = false)
    {
        ArgumentNullException.ThrowIfNull(url);
        if (string.IsNullOrWhiteSpace(destinationPath))
            throw new ArgumentException("A destination path is required.", nameof(destinationPath));

        ValidateSourceUrl(url.OriginalString, destinationPath, nameof(url));

        var dest = Path.GetFullPath(destinationPath);
        var dir = Path.GetDirectoryName(dest);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        var size = expectedBytes is > 0 ? expectedBytes.Value : 0L;
        var name = Path.GetFileName(dest);

        if (requireHash && sha256 is null)
            throw new InvalidOperationException(UnpinnedRefusal(name, "the caller required a pinned hash"));

        // The gate spans the EXISTENCE CHECK, not just the transfer. It used to
        // be taken inside DownloadAsync, so two concurrent calls for one
        // destination each hashed the same 300 MB file (wasted, not wrong) and —
        // the part that was wrong — either could DeleteOrThrow the canonical
        // file while the other was mid-hash on it, or had just returned it as
        // "already present and verified". That breaks the promise
        // VerifiedDownloader makes three lines above its own File.Move: a
        // concurrent reader sees the old file or the new one, never NEITHER.
        using var gate = await VerifiedDownloader.AcquireDestinationGateAsync(dest, ct).ConfigureAwait(false);

        if (File.Exists(dest))
        {
            if (sha256 is not null)
            {
                var actual = await VerifiedDownloader.ComputeSha256Async(dest, ct).ConfigureAwait(false);
                if (string.Equals(actual, sha256, StringComparison.OrdinalIgnoreCase))
                {
                    log?.Invoke($"'{name}' is already present and verified.");
                    return dest;
                }

                Warn(log,
                    $"The installed '{name}' does NOT match its pinned hash (expected {sha256}, " +
                    $"got {actual}). Discarding it and downloading again.");
                DeleteOrThrow(dest);
            }
            else if (size > 0)
            {
                var actualSize = new FileInfo(dest).Length;
                if (actualSize == size)
                {
                    // No hash to check it against; the length is the whole of
                    // the evidence, and it is the check that catches the common
                    // failure (a killed or truncated transfer).
                    Warn(log, $"Using the existing UNVERIFIED '{name}' — its length is right, but " +
                              "there is no hash to check its contents against.");
                    return dest;
                }

                Warn(log,
                    $"The installed '{name}' is {actualSize} bytes; {size} were expected. " +
                    "Discarding it and downloading again.");
                DeleteOrThrow(dest);
            }
            else
            {
                Warn(log, $"Using the existing UNVERIFIED '{name}' — there is neither a hash nor a " +
                          "size to check it against.");
                return dest;
            }
        }

        if (sha256 is null)
        {
            Warn(log, $"'{name}' is being fetched from '{url}' with no pinned SHA-256. It cannot be " +
                      "verified after download.");
        }

        var written = await VerifiedDownloader.DownloadGateHeldAsync(
                url.OriginalString, dest, progress is null ? null : progress.Report, ct,
                sha256, allowResume: true, expectedSizeBytes: size)
            .ConfigureAwait(false);

        log?.Invoke(sha256 is null
            ? $"'{name}' downloaded ({written} bytes, UNVERIFIED) from '{url}'."
            : $"'{name}' downloaded ({written} bytes) and verified from '{url}'.");
        return dest;
    }

    /// <summary>
    /// Whether the file at <paramref name="path"/> matches
    /// <paramref name="sha256"/> (and <paramref name="expectedBytes"/>, when
    /// given). A MISSING file is <c>false</c>, not an exception — "not
    /// installed" is a legitimate answer to this question.
    /// </summary>
    /// <param name="path">File to check. Nothing is written or deleted.</param>
    /// <param name="sha256">Lower-case hex SHA-256 to compare against,
    ///   case-insensitively. Required: there is deliberately no "verify against
    ///   nothing" overload, because <c>false</c> would then mean both "corrupt"
    ///   and "nobody pinned it", and a caller that conflates those deletes a
    ///   perfectly good file.</param>
    /// <param name="expectedBytes">Exact size, or <c>null</c> to skip the
    ///   length check. Checked first, because it is O(1) against hashing a
    ///   300 MB model.</param>
    /// <param name="ct">Cancellation token.</param>
    /// <returns>True only when the file exists and every given check passes.</returns>
    /// <exception cref="ArgumentException"><paramref name="sha256"/> is blank.</exception>
    public static async Task<bool> VerifyFileAsync(
        string path,
        string sha256,
        long? expectedBytes = null,
        CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(sha256))
            throw new ArgumentException(
                "A SHA-256 is required. Verifying against nothing would make 'false' mean both " +
                "'corrupt' and 'unpinned'.", nameof(sha256));

        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return false;
        if (expectedBytes is > 0 && new FileInfo(path).Length != expectedBytes.Value) return false;

        var actual = await VerifiedDownloader.ComputeSha256Async(path, ct).ConfigureAwait(false);
        return string.Equals(actual, sha256.Trim(), StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Lower-case hex SHA-256 of a file — for pinning a copy you obtained and
    /// validated by some other means. Do NOT pin a copy fetched over the same
    /// unverified channel you are about to fetch it over again; that proves
    /// only that two GETs agreed.
    /// </summary>
    /// <param name="path">File to hash.</param>
    /// <param name="ct">Cancellation token.</param>
    /// <returns>64 lower-case hex characters.</returns>
    public static Task<string> ComputeSha256Async(string path, CancellationToken ct = default)
        => VerifiedDownloader.ComputeSha256Async(path, ct);

    /// <summary>
    /// The transport rule every download in these packages is held to, in ONE
    /// place: absolute <c>https</c>, or plain <c>http</c> to a LOOPBACK host.
    ///
    /// <para>
    /// Everything fetched here is subsequently loaded as a model or executed,
    /// so a host that lets a settings file or a user name a source would
    /// otherwise have handed over a fetch across whatever transport the string
    /// asked for. Loopback http is excepted because it is the shape of a test
    /// server and is not a transport an attacker is on.
    /// </para>
    /// <para>
    /// It is public so that a catalogue can pre-flight a synthesized entry
    /// before download time, and shared so that a second catalogue cannot
    /// enforce a subtly different rule:
    /// <c>OnnxModelCatalog.ValidateEntry</c>,
    /// <see cref="DemucsGgmlModelCatalog.ValidateEntry"/> and
    /// <see cref="EnsureFileAsync"/> all call this.
    /// </para>
    /// </summary>
    /// <param name="url">The URL to check.</param>
    /// <param name="subject">What the URL belongs to, for the message — a model
    ///   id or a destination path. Never parsed.</param>
    /// <param name="paramName">Parameter name for the thrown
    ///   <see cref="ArgumentException"/>, so the caller's own argument is
    ///   named rather than this method's.</param>
    /// <exception cref="ArgumentException">The URL is not absolute, or would be
    ///   fetched over anything but https / loopback http.</exception>
    public static void ValidateSourceUrl(string? url, string subject, string paramName)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri))
            throw new ArgumentException(
                $"'{subject}' has a source URL that is not absolute: '{url}'.", paramName);

        var https = string.Equals(uri.Scheme, Uri.UriSchemeHttps, StringComparison.Ordinal);
        var loopbackHttp = string.Equals(uri.Scheme, Uri.UriSchemeHttp, StringComparison.Ordinal)
                           && uri.IsLoopback;
        if (!https && !loopbackHttp)
            throw new ArgumentException(
                $"'{subject}' would be fetched over '{uri.Scheme}' from '{url}'. Downloads must use " +
                "https (plain http is accepted for loopback only).", paramName);
    }

    /// <summary>
    /// A file that failed its check and cannot be removed is an
    /// <see cref="IOException"/>, not a silent fall-through: downloading over
    /// it would fail at the rename anyway, and much further from the cause.
    /// </summary>
    private static void DeleteOrThrow(string dest)
    {
        try { File.Delete(dest); }
        catch (IOException ex)
        {
            throw new IOException(
                $"'{dest}' failed its integrity check and could not be replaced: {ex.Message}", ex);
        }
        catch (UnauthorizedAccessException ex)
        {
            throw new IOException(
                $"'{dest}' failed its integrity check and could not be replaced: {ex.Message}", ex);
        }
    }

    /// <summary>
    /// Deletes abandoned resumable prefixes (<c>*.part</c>) in
    /// <paramref name="directory"/>. Returns how many were removed. Always safe
    /// to call; never throws for a file it cannot touch.
    ///
    /// <para><b>Why this has to exist (W4-S14).</b> A resumable download writes
    /// <c>{dest}.{urlhash}.part</c> and KEEPS it on cancellation and on network
    /// failure — that is the whole point of resuming — and the URL digest means
    /// one prefix per MIRROR. A catalogue walk that fails over therefore
    /// accumulates one partial file per mirror it tried, and until this method
    /// existed nothing anywhere swept them: a user who abandoned a 300 MB model
    /// twice had 600 MB of invisible, permanently-resident cache with no API to
    /// reclaim it. <c>PythonEnvManager.CleanStaleTempFiles</c> covers the
    /// NON-resumable <c>*.tmp.*</c> shape in the interpreter root only.</para>
    ///
    /// <para><b>An in-flight download is not stale.</b> The liveness test is
    /// <c>VerifiedDownloader</c>'s own <see cref="FileShare.None"/> handle —
    /// "can I open this exclusively?" is an exact answer to "is somebody
    /// writing it?", with no clock and no age heuristic. That probe is copied
    /// from <c>CleanStaleTempFiles</c> deliberately rather than re-invented.</para>
    /// </summary>
    /// <param name="directory">Directory to sweep, non-recursively. A missing
    ///   directory is 0, not an exception.</param>
    /// <param name="olderThan">Only remove prefixes last written at least this
    ///   long ago. <see cref="TimeSpan.Zero"/> sweeps every one that is not
    ///   currently open, which is what a catalogue does after the file it wanted
    ///   has landed.</param>
    /// <param name="log">Optional log sink.</param>
    public static int CleanPartials(string directory, TimeSpan olderThan, Action<string>? log = null)
    {
        if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory)) return 0;

        var cutoff = DateTime.UtcNow - (olderThan < TimeSpan.Zero ? TimeSpan.Zero : olderThan);
        var removed = 0;
        foreach (var f in Directory.EnumerateFiles(directory, "*" + VerifiedDownloader.PartialSuffix,
                                                   SearchOption.TopDirectoryOnly))
        {
            try
            {
                if (File.GetLastWriteTimeUtc(f) > cutoff) continue;
                // Opening it exclusively proves nobody is downloading into it.
                using (new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.None)) { }
            }
            catch (IOException) { continue; }
            catch (UnauthorizedAccessException) { continue; }

            try
            {
                File.Delete(f);
                removed++;
            }
            catch (Exception ex) { log?.Invoke($"Could not remove the abandoned partial '{f}': {ex.Message}"); }
        }
        return removed;
    }

    /// <summary>The refusal both model catalogues use for an unpinned row, so
    /// the one-URL door can state the same policy when a caller asks for it
    /// with <c>requireHash</c>.</summary>
    private static string UnpinnedRefusal(string name, string because) =>
        $"'{name}' has no pinned SHA-256 and {because}. Supply a hash, or call without requireHash " +
        "to accept an unverified fetch.";

    private static void Warn(Action<string>? log, string message) => log?.Invoke("WARNING: " + message);
}
