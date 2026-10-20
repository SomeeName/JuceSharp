namespace JuceSharp.StemSeparation.Runtime;

/// <summary>
/// The policy a model CATALOGUE applies on top of
/// <see cref="VerifiedDownload.EnsureFileAsync"/>: refuse an unpinned row unless
/// the caller opted in, walk the mirror list, and sweep the partials the walk
/// may have left behind.
///
/// <para><b>Why this exists (W4-S27).</b> <c>DemucsGgmlModelCatalog.EnsureAsync</c>
/// and <c>OnnxModelCatalog.EnsureAsync</c> were line-for-line equivalent modulo
/// the entry type — the same duplication <c>VerifiedDownloader</c>'s own remarks
/// argue against ("a second implementation of that sequence is a second chance
/// to get the order wrong"), one layer up. The ENTRY TYPES stay separate: they
/// carry different domain fields (<c>SourceCount</c> and <c>Kind</c> against
/// <c>Quantisation</c>), they live in different assemblies and they are public
/// API. Only the policy is shared, which is the half that was duplicated.</para>
///
/// <para>What is deliberately NOT here: the two catalogues' <c>ValidateEntry</c>
/// checks stay with their own entry types, because what a valid row IS differs.
/// Each one calls its own before handing over.</para>
/// </summary>
internal static class CatalogueDownload
{
    /// <summary>
    /// The one sentence both catalogues refuse an unpinned row with, so the two
    /// cannot drift into saying different things about the same policy.
    /// </summary>
    /// <param name="id">Entry id, as the user sees it.</param>
    /// <param name="hashSource">The row's own explanation of where a hash would
    ///   come from — the useful half of the message.</param>
    /// <param name="loadedAs">What consumes the file, e.g. "ONNX Runtime".</param>
    internal static string UnpinnedWarning(string id, string hashSource, string loadedAs) =>
        $"Model '{id}' has no pinned SHA-256 ({hashSource}). It cannot be verified " +
        $"after download, and it is loaded into {loadedAs} as-is.";

    /// <summary>
    /// Refuse-or-warn, then fetch from the first mirror that works.
    /// </summary>
    /// <param name="id">Entry id.</param>
    /// <param name="sha256">Pinned hash, or null for an unverified row.</param>
    /// <param name="sizeBytes">Exact size, or 0 when unknown.</param>
    /// <param name="fileName">Name on disk inside <paramref name="targetDir"/>.</param>
    /// <param name="urls">Mirrors, tried in order. Every one must serve the SAME
    ///   bytes — this is a mirror list, not a fallback to a different file.</param>
    /// <param name="hashSource">Provenance of the hash, for the message.</param>
    /// <param name="loadedAs">What consumes the file, for the message.</param>
    /// <param name="targetDir">Directory to install into; created if missing.</param>
    /// <param name="progress">Optional progress sink.</param>
    /// <param name="allowUnverified">Opt in to fetching a row with no pin.</param>
    /// <param name="log">Optional log sink.</param>
    /// <param name="ct">Cancellation token. A user Cancel never advances to the
    ///   next mirror.</param>
    internal static async Task<string> EnsureAsync(
        string id,
        string? sha256,
        long sizeBytes,
        string fileName,
        IReadOnlyList<string> urls,
        string hashSource,
        string loadedAs,
        string targetDir,
        IProgress<DownloadProgress>? progress,
        bool allowUnverified,
        Action<string>? log,
        CancellationToken ct)
    {
        if (sha256 is null)
        {
            var warning = UnpinnedWarning(id, hashSource, loadedAs);
            log?.Invoke("WARNING: " + warning);
            if (!allowUnverified)
                throw new InvalidOperationException(
                    warning + " Pass allowUnverified: true to fetch it anyway.");
        }

        Directory.CreateDirectory(targetDir);
        var dest = Path.Combine(targetDir, fileName);

        Exception? last = null;
        foreach (var url in urls)
        {
            try
            {
                // The existing-file check, the .part resume, the length check
                // and the hash — all of it — is VerifiedDownload.EnsureFileAsync,
                // the same kind-less entry point a host with a ggml weight or a
                // user .ckpt calls. What a CATALOGUE owns on top is the two
                // things a single file does not have: the allowUnverified
                // decision above, and this mirror walk. SizeBytes travels INTO
                // the download rather than being checked after the rename,
                // because checking it afterwards leaves a window where a
                // concurrent reader sees a bad model at the canonical path.
                var path = await VerifiedDownload.EnsureFileAsync(
                        new Uri(url), dest, sha256, sizeBytes > 0 ? sizeBytes : null,
                        progress, log, ct)
                    .ConfigureAwait(false);

                // One .part per MIRROR is the design (the key is a digest of the
                // URL, so a failover never appends one server's body to
                // another's prefix) — which means a walk that failed over leaves
                // a 300 MB prefix per mirror it tried, and nothing else ever
                // sweeps them. Now that the file is here, none of them can be
                // resumed into anything.
                VerifiedDownload.CleanPartials(targetDir, TimeSpan.Zero, log);
                return path;
            }
            catch (OperationCanceledException)
            {
                // A user Cancel is not a broken mirror. Never try the next URL.
                throw;
            }
            catch (Exception ex) when (ex is HttpRequestException or InvalidDataException or IOException)
            {
                log?.Invoke($"WARNING: Mirror '{url}' failed for '{id}': {ex.Message}");
                last = ex;
            }
        }

        throw last ?? new InvalidOperationException($"Model '{id}' has no download URLs.");
    }
}
