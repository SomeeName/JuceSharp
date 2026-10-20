namespace JuceSharp.StemSeparation.Runtime;

/// <summary>
/// One downloadable ggml weight file for the native demucs.cpp backend.
/// </summary>
/// <param name="Id">Stable identifier. Safe to persist in a settings file.</param>
/// <param name="DisplayName">User-facing label.</param>
/// <param name="FileName">Name on disk, and the cache key inside a target
///   directory. demucs.cpp loads the file by path, so this is also what the
///   engine is pointed at.</param>
/// <param name="Urls">Download sources, tried in order. More than one is a
///   MIRROR LIST, not a fallback to a different file — every URL must serve
///   bytes matching <paramref name="Sha256"/>.</param>
/// <param name="Sha256">Lower-case hex SHA-256 of the complete file, or
///   <c>null</c> when no trustworthy source for it exists. <b>Null is not
///   "skip the check" — it is "this entry is unverified", and
///   <see cref="DemucsGgmlModelCatalog.EnsureAsync(DemucsGgmlModelEntry,string,IProgress{DownloadProgress}?,bool,Action{string}?,CancellationToken)"/>
///   refuses to fetch it without an explicit <c>allowUnverified: true</c>.</b></param>
/// <param name="SizeBytes">Exact size of the complete file, 0 when unknown. An
///   integrity constraint, not a UI hint: it is the only check available
///   against a response that declares no <c>Content-Length</c>, which is
///   exactly what a truncating proxy produces.</param>
/// <param name="HashSource">Where <paramref name="Sha256"/> came from, in
///   words — or, when it is null, WHY there is none. Required either way: a pin
///   nobody can trace is a pin nobody can re-check. Linted by
///   <c>DemucsGgmlModelCatalogTests</c>.</param>
/// <param name="SourceCount">How many sources the model separates (4 =
///   drums/bass/other/vocals, 6 = + guitar/piano).</param>
/// <param name="LicenceNote">What is known about redistribution. Read it before
///   you bundle a model with an application.</param>
public sealed record DemucsGgmlModelEntry(
    string Id,
    string DisplayName,
    string FileName,
    IReadOnlyList<string> Urls,
    string? Sha256,
    long SizeBytes,
    string HashSource,
    int SourceCount,
    string LicenceNote);

/// <summary>
/// The ggml weights the NATIVE demucs.cpp backend loads, and the download entry
/// point for them.
///
/// <para>
/// <b>What this replaces.</b> <c>OnnxModelCatalog</c> is, correctly, ONNX-only,
/// so a host running the in-process demucs.cpp backend had to carry its own
/// table of file names, URLs and sizes — DotCharter's <c>StemModelCatalog</c>
/// is exactly that, and it reached JuceSharp's downloader by dressing a ggml
/// weight as an ONNX entry with "the nearest kind label", because there was no
/// kind-less door. There is one now
/// (<see cref="VerifiedDownload.EnsureFileAsync"/>), and this table is the
/// catalogue on top of it.
/// </para>
///
/// <para>
/// <b>Every hash here was read from the Hugging Face repository API on
/// 2026-09-11</b> — <c>https://huggingface.co/api/datasets/Retrobear/demucs.cpp/tree/main</c>,
/// field <c>lfs.oid</c>, which for an LFS object IS its SHA-256: the same value
/// the CDN serves and the same value <c>git lfs</c> verifies on checkout. Each
/// entry records that in <see cref="DemucsGgmlModelEntry.HashSource"/>. The
/// 6-source entry is additionally corroborated — its oid agrees byte for byte
/// with the hash DotCharter computed independently, on 2026-09-04, from a copy
/// it had been separating with for months.
/// </para>
/// <para>
/// That is worth stating plainly because the hand-written table this replaces
/// said the opposite: it recorded "UNVERIFIED: Retrobear/demucs.cpp is a
/// Hugging Face DATASET repo and its files are not LFS objects with a published
/// oid". They are, under the <c>api/datasets/</c> route rather than
/// <c>api/models/</c>; the note was a reasonable inference from the wrong
/// endpoint, and it left two of three weights unpinned for a release.
/// </para>
///
/// <para>
/// <b>Adding an entry.</b> Read the hash from the source repository and say so
/// in <c>HashSource</c>. If there is no such source, ship <c>Sha256 = null</c>
/// with a <c>HashSource</c> that explains why — do NOT paste a hash computed
/// from a copy you downloaded over the same unverified channel and call it a
/// pin; that only proves the bytes did not change between two GETs.
/// </para>
/// </summary>
public static class DemucsGgmlModelCatalog
{
    private const string RepoBase =
        "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/";

    private const string HfApiSource =
        "huggingface.co/api/datasets/Retrobear/demucs.cpp/tree/main, lfs.oid, read 2026-09-11. " +
        "An LFS object's oid IS its SHA-256.";

    private const string DemucsLicence =
        "Demucs weights are MIT (facebookresearch/demucs). This ggml conversion is a third-party " +
        "re-host — check the dataset card before redistributing it.";

    /// <summary>Every known ggml weight, in no particular order.</summary>
    public static IReadOnlyList<DemucsGgmlModelEntry> All { get; } =
    [
        new DemucsGgmlModelEntry(
            Id: "ggml-htdemucs-4s",
            DisplayName: "HTDemucs 4-stem (ggml f16)",
            FileName: "ggml-model-htdemucs-4s-f16.bin",
            Urls: [RepoBase + "ggml-model-htdemucs-4s-f16.bin"],
            Sha256: "72b17c42d308982ddb5069bc3bf48b81a5aac4cb6516e4366c0fa7cef6df0064",
            SizeBytes: 83_994_361,
            HashSource: HfApiSource,
            SourceCount: 4,
            LicenceNote: DemucsLicence),

        new DemucsGgmlModelEntry(
            Id: "ggml-htdemucs-6s",
            DisplayName: "HTDemucs 6-stem (ggml f16)",
            FileName: "ggml-model-htdemucs-6s-f16.bin",
            Urls: [RepoBase + "ggml-model-htdemucs-6s-f16.bin"],
            Sha256: "09704f4ceae204e56e77d5eefd6ac71d7275be81fd507e6913371d59abcee856",
            SizeBytes: 54_855_129,
            HashSource: HfApiSource + " Corroborated: identical to the hash DotCharter computed " +
                        "independently on 2026-09-04 from an installed copy it had been separating " +
                        "with — i.e. a file validated by USE, not by a second download over the same " +
                        "channel.",
            SourceCount: 6,
            LicenceNote: DemucsLicence),

        new DemucsGgmlModelEntry(
            Id: "ggml-hdemucs-mmi-v3",
            DisplayName: "HDemucs MMI v3 4-stem (ggml f16)",
            FileName: "ggml-model-hdemucs_mmi-v3-f16.bin",
            Urls: [RepoBase + "ggml-model-hdemucs_mmi-v3-f16.bin"],
            Sha256: "f72a3b8ecce3aa50da9e062a91c46367e15b5f5a71d070874217c5e4cb6b35a4",
            SizeBytes: 167_294_211,
            HashSource: HfApiSource,
            SourceCount: 4,
            LicenceNote: DemucsLicence),
    ];

    /// <summary>
    /// What a host gets when it does not name a model: the 4-source HTDemucs,
    /// which is the balanced default demucs.cpp itself documents.
    /// </summary>
    public static DemucsGgmlModelEntry Default =>
        FindById("ggml-htdemucs-4s")
        ?? throw new InvalidOperationException("The default ggml entry is missing from the catalogue.");

    /// <summary>Case-insensitive lookup by <see cref="DemucsGgmlModelEntry.Id"/>.</summary>
    /// <param name="id">The id to look for; null or blank yields null.</param>
    /// <returns>The entry, or null when no row has that id.</returns>
    public static DemucsGgmlModelEntry? FindById(string? id) =>
        string.IsNullOrWhiteSpace(id)
            ? null
            : All.FirstOrDefault(e => string.Equals(e.Id, id, StringComparison.OrdinalIgnoreCase));

    /// <summary>
    /// Ensures <paramref name="entry"/> is present and CORRECT in
    /// <paramref name="targetDir"/>, and returns its path.
    ///
    /// <para>
    /// This is <see cref="VerifiedDownload.EnsureFileAsync"/> per mirror, plus
    /// the two policies a catalogue adds: an unpinned entry is REFUSED unless
    /// the caller opts in at the call site, and a failed mirror falls through
    /// to the next one while a user Cancel does not.
    /// </para>
    /// <para>
    /// <b>An existing file is checked, not trusted</b> — a file that fails its
    /// hash is discarded and fetched again. Cancelling throws
    /// <see cref="OperationCanceledException"/> and KEEPS the per-URL
    /// <c>.part</c> file, so the next call resumes; a TIMEOUT is an
    /// <see cref="IOException"/>, not a cancellation, so it falls through to
    /// the next mirror instead of ending the whole attempt.
    /// </para>
    /// </summary>
    /// <param name="entry">The model to install.</param>
    /// <param name="targetDir">Directory the file lands in; created if needed.</param>
    /// <param name="progress">Optional progress sink.</param>
    /// <param name="allowUnverified">Required to fetch an entry with no pinned
    ///   hash. The warning is logged either way.</param>
    /// <param name="log">Optional log sink.</param>
    /// <param name="ct">Cancellation token.</param>
    /// <returns>The full path of the installed file.</returns>
    /// <exception cref="ArgumentException"><paramref name="entry"/> names a file
    ///   outside <paramref name="targetDir"/>, or a URL that is not absolute
    ///   https (loopback http excepted), or <paramref name="targetDir"/> is
    ///   blank.</exception>
    /// <exception cref="InvalidOperationException">The entry has no pinned hash
    ///   and <paramref name="allowUnverified"/> is false.</exception>
    /// <exception cref="InvalidDataException">The completed download did not
    ///   match the pinned hash or size. No file is left at the destination.</exception>
    public static async Task<string> EnsureAsync(
        DemucsGgmlModelEntry entry,
        string targetDir,
        IProgress<DownloadProgress>? progress = null,
        bool allowUnverified = false,
        Action<string>? log = null,
        CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(entry);
        if (string.IsNullOrWhiteSpace(targetDir))
            throw new ArgumentException("Target directory is required.", nameof(targetDir));
        ValidateEntry(entry);

        // Shared with OnnxModelCatalog — see CatalogueDownload for why the
        // POLICY is one implementation and the entry types are not (W4-S27).
        return await CatalogueDownload.EnsureAsync(
                entry.Id, entry.Sha256, entry.SizeBytes, entry.FileName, entry.Urls, entry.HashSource,
                loadedAs: "the native engine",
                targetDir, progress, allowUnverified, log, ct)
            .ConfigureAwait(false);
    }

    /// <summary>
    /// Whether the file at <paramref name="path"/> matches
    /// <paramref name="entry"/>'s pinned hash and size.
    /// </summary>
    /// <param name="entry">The entry to check against.</param>
    /// <param name="path">File to check. A missing file is <c>false</c>.</param>
    /// <param name="ct">Cancellation token.</param>
    /// <returns>True only when the file exists and matches.</returns>
    /// <exception cref="InvalidOperationException">The entry has no pinned
    ///   hash. That is deliberately not <c>false</c>: "I cannot answer" and
    ///   "the file is corrupt" must not be the same value, or a caller deletes
    ///   a perfectly good model because nobody had hashed it.</exception>
    public static Task<bool> VerifyAsync(
        DemucsGgmlModelEntry entry, string path, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(entry);
        if (entry.Sha256 is null)
            throw new InvalidOperationException(
                $"Model '{entry.Id}' has no pinned SHA-256, so there is nothing to verify against " +
                $"({entry.HashSource}).");

        return VerifiedDownload.VerifyFileAsync(
            path, entry.Sha256, entry.SizeBytes > 0 ? entry.SizeBytes : null, ct);
    }

    /// <summary>
    /// Pre-flights the two fields that decide WHERE bytes land and WHERE they
    /// come from.
    ///
    /// <para>
    /// <see cref="EnsureAsync(DemucsGgmlModelEntry,string,IProgress{DownloadProgress}?,bool,Action{string}?,CancellationToken)"/>
    /// is public and takes a caller-constructed record, so a host that lets a
    /// settings file or a user name a model would otherwise have handed over an
    /// arbitrary-path writer (<c>FileName = @"..\..\startup.exe"</c>) fed over
    /// whatever transport the entry asked for. It is public in its own right so
    /// that a host synthesizing entries can check them at the point it builds
    /// them rather than at download time.
    /// </para>
    /// </summary>
    /// <param name="entry">The entry to check. Nothing is fetched.</param>
    /// <exception cref="ArgumentNullException"><paramref name="entry"/> is null.</exception>
    /// <exception cref="ArgumentException">The file name is not bare, or a URL
    ///   is missing, relative, or not https / loopback http.</exception>
    public static void ValidateEntry(DemucsGgmlModelEntry entry)
    {
        ArgumentNullException.ThrowIfNull(entry);

        if (string.IsNullOrWhiteSpace(entry.FileName) ||
            !string.Equals(entry.FileName, Path.GetFileName(entry.FileName), StringComparison.Ordinal))
            throw new ArgumentException(
                $"Model '{entry.Id}' has FileName '{entry.FileName}', which is not a bare file name. " +
                "A model file name may not contain a directory component.", nameof(entry));

        if (entry.Urls is null || entry.Urls.Count == 0)
            throw new ArgumentException($"Model '{entry.Id}' has no download URLs.", nameof(entry));

        foreach (var url in entry.Urls)
            VerifiedDownload.ValidateSourceUrl(url, entry.Id, nameof(entry));
    }
}
