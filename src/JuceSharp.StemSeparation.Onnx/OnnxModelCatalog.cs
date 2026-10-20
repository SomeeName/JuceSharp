using JuceSharp.StemSeparation.Runtime;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// One downloadable ONNX stem-separation model.
/// </summary>
/// <param name="Id">Stable identifier. Safe to persist in a settings file.</param>
/// <param name="Kind">Which inference driver consumes it.</param>
/// <param name="DisplayName">User-facing label.</param>
/// <param name="FileName">Name on disk. Also the cache key inside a
///   target directory.</param>
/// <param name="Urls">Download sources, tried in order. More than one is a
///   mirror list, not a fallback to a DIFFERENT file — every URL must serve
///   bytes matching <paramref name="Sha256"/>.</param>
/// <param name="Sha256">
/// Lower-case hex SHA-256 of the complete file, or <c>null</c> when no
/// trustworthy source for it exists. <b>Null is not "skip the check" — it is
/// "this entry is unverified", and <see cref="OnnxModelCatalog.EnsureAsync(OnnxModelEntry,string,IProgress{DownloadProgress}?,bool,Action{string}?,CancellationToken)"/>
/// refuses to fetch it without an explicit <c>allowUnverified: true</c>.</b>
/// </param>
/// <param name="SizeBytes">Exact size of the complete file, 0 when unknown.
///   An integrity constraint, not a UI hint: it is the only check available
///   against a server that sends no <c>Content-Length</c>, which is exactly
///   what a truncating proxy produces.</param>
/// <param name="HashSource">Where <paramref name="Sha256"/> came from, in
///   words. Required whenever a hash is present — a pin nobody can trace is a
///   pin nobody can re-check. Linted by <c>OnnxModelCatalogTests</c>.</param>
/// <param name="SourceCount">How many stems the model produces.</param>
/// <param name="LicenceNote">What is known about redistribution. Read it
///   before you bundle a model with an application.</param>
/// <param name="HtdemucsLayout">For an <see cref="OnnxStemModelKind.Htdemucs"/>
///   entry, the IO layout the file was MEASURED to have (and <c>null</c> for
///   every other kind). Linted against
///   <see cref="HtdemucsIoBinding.SupportedLayouts"/>, and checked against the
///   real file by the env-gated real-model test — an entry the driver cannot
///   bind must never be offered for download.</param>
/// <param name="BsRoformerLayout">For an <see cref="OnnxStemModelKind.BsRoformer"/>
///   entry, the tensor layout the driver binds it as; <c>null</c> for every other
///   kind. <c>bs-roformer-sw-6s</c>'s was read from the file (and the env-gated
///   <c>BsRoformerSwModelTests</c> re-reads it); <c>bs-roformer-fp16</c>'s is the
///   packed layout the driver has always assumed for it, not re-measured. Linted against
///   <see cref="BsRoformerIoBinding.SupportedLayouts"/>, like
///   <paramref name="HtdemucsLayout"/>.</param>
public sealed record OnnxModelEntry(
    string Id,
    OnnxStemModelKind Kind,
    string DisplayName,
    string FileName,
    IReadOnlyList<string> Urls,
    string? Sha256,
    long SizeBytes,
    string HashSource,
    int SourceCount,
    string LicenceNote,
    HtdemucsIoLayout? HtdemucsLayout = null,
    BsRoformerIoLayout? BsRoformerLayout = null);

/// <summary>
/// The ONNX models this package knows how to fetch and verify, and the
/// download entry point for them.
///
/// <para>
/// <b>What this replaces.</b> A host wanting an ONNX model had to carry its own
/// URL table, its own <see cref="HttpClient"/> and its own atomic-rename dance
/// — DotCharter's <c>StemModelManager</c> is exactly that, and it had a hash
/// for two of its eleven entries. Everything here goes through
/// <c>VerifiedDownloader</c>: temp file → hash the COMPLETE file → atomic
/// rename, plus HTTP Range resume, so an interrupted 300 MB model download
/// continues instead of starting over.
/// </para>
///
/// <para>
/// <b>Every hash in this table was read from the Hugging Face repository API
/// on 2026-09-11</b>, where an LFS object's <c>oid</c> IS its SHA-256 — the
/// same value the CDN serves and the same value <c>git lfs</c> verifies on
/// checkout. Each entry records that in <see cref="OnnxModelEntry.HashSource"/>.
/// The htdemucs 6-stem entry is additionally corroborated: it agrees byte for
/// byte with the hash DotCharter pinned independently, from a locally installed
/// copy, on 2026-09-04.
/// </para>
///
/// <para>
/// <b>No MrCitron htdemucs entries</b> (<c>htdemucs.onnx</c> /
/// <c>htdemucs_ft.onnx</c> from <c>MrCitron/demucs-v4-onnx</c>; removed
/// 2026-10-05, DotCharter #254). They run — <see cref="HtdemucsOnnxInference"/>
/// drives their single-input <see cref="HtdemucsIoLayout.WaveformOnly"/> layout —
/// but the graphs' own in-graph STFT branch is defective: on real audio their
/// stems add back up to 63–69 % of the mix at a correlation of 0.81–0.84
/// (the arjune123 6-stem export: 100 % / 0.999), keeping ~30 % of the
/// 300 Hz–8 kHz energy and adding 2.5–5x the mix's energy above 8 kHz. The
/// same holds for the bare graph run in Python ORT, so it is the file, not
/// the driver. <c>HtdemucsRealModelTests</c> holds every Htdemucs entry to the
/// reconstruction bar those two failed.
/// </para>
///
/// <para>
/// <b>Adding an entry.</b> Read the hash from the source repository (for
/// Hugging Face: <c>https://huggingface.co/api/models/&lt;repo&gt;/tree/main</c>,
/// field <c>lfs.oid</c>) and say so in <c>HashSource</c>. If there is no such
/// source, ship <c>Sha256 = null</c> with a <c>HashSource</c> that explains why
/// — do NOT paste a hash computed from a copy you downloaded over the same
/// unverified channel and call it a pin; that only proves the bytes did not
/// change between two GETs. <see cref="ComputeSha256Async"/> exists for the one
/// legitimate variant of that workflow: hashing a copy you obtained and
/// validated by some other means.
/// </para>
/// </summary>
public static class OnnxModelCatalog
{
    /// <summary>Every known model, in no particular order.</summary>
    public static IReadOnlyList<OnnxModelEntry> All { get; } =
    [
        new OnnxModelEntry(
            Id: "htdemucs-6s-onnx",
            Kind: OnnxStemModelKind.Htdemucs,
            DisplayName: "HTDemucs 6-stem (ONNX)",
            FileName: "htdemucs_6s.onnx",
            Urls: ["https://huggingface.co/arjune123/demucs-onnx/resolve/main/htdemucs_6s.onnx"],
            Sha256: "b19cdf832edeb50274b36d6928a8bf83202237c71a4836c4cca45e843316ee17",
            SizeBytes: 114_559_139,
            HashSource: "huggingface.co/api/models/arjune123/demucs-onnx/tree/main, lfs.oid, read 2026-09-11; " +
                        "identical to the hash DotCharter's StemModelCatalog pinned independently from an " +
                        "installed copy on 2026-09-04.",
            SourceCount: 6,
            LicenceNote: "Demucs v4 weights are MIT (facebookresearch/demucs). This ONNX export is a " +
                         "third-party re-host — check the model card before redistributing it.",
            HtdemucsLayout: HtdemucsIoLayout.WaveformAndSpectrogram),

        new OnnxModelEntry(
            Id: "bs-roformer-fp16",
            Kind: OnnxStemModelKind.BsRoformer,
            DisplayName: "BS-RoFormer fp16 (2 stems)",
            FileName: "bs_roformer_fp16.onnx",
            Urls: ["https://huggingface.co/safescribeai/bs-roformer-onnx-fp16/resolve/main/bs_roformer_fp16.onnx"],
            Sha256: "fc61d602f8903abc84579e534912b3ea28e61761d29c7cb9e5a05ac0886f8373",
            SizeBytes: 324_761_627,
            HashSource: "huggingface.co/api/models/safescribeai/bs-roformer-onnx-fp16/tree/main, lfs.oid, " +
                        "read 2026-09-11.",
            SourceCount: 2,
            LicenceNote: "viperx's BS-RoFormer checkpoint, re-hosted as ONNX. Research/community model — " +
                         "no explicit redistribution licence. Do not bundle without checking.",
            BsRoformerLayout: BsRoformerIoLayout.PackedChannels),

        new OnnxModelEntry(
            Id: "uvr-mdx-net-inst-hq-3",
            Kind: OnnxStemModelKind.MdxNet,
            DisplayName: "UVR-MDX-NET Inst HQ 3 (2 stems)",
            FileName: "UVR-MDX-NET-Inst_HQ_3.onnx",
            Urls:
            [
                "https://huggingface.co/seanghay/uvr_models/resolve/main/UVR-MDX-NET-Inst_HQ_3.onnx",
                "https://huggingface.co/Politrees/UVR_resources/resolve/main/models/MDXNet/UVR-MDX-NET-Inst_HQ_3.onnx",
            ],
            Sha256: "317554b07fe1ea5279a77f2b1520a41ea4b93432560c4ffd08792c30fddf9adc",
            SizeBytes: 66_759_214,
            HashSource: "lfs.oid from BOTH huggingface.co/api/models/seanghay/uvr_models/tree/main and " +
                        "huggingface.co/api/models/Politrees/UVR_resources/tree/main/models/MDXNet, read " +
                        "2026-09-11; the two independent re-hosts agree.",
            SourceCount: 2,
            LicenceNote: "Ultimate Vocal Remover community model. MIT-licensed tooling, but the weights " +
                         "carry no explicit redistribution licence."),

        new OnnxModelEntry(
            Id: "uvr-mdx-net-voc-ft",
            Kind: OnnxStemModelKind.MdxNet,
            DisplayName: "UVR-MDX-NET Voc FT (2 stems)",
            FileName: "UVR-MDX-NET-Voc_FT.onnx",
            Urls: ["https://huggingface.co/Politrees/UVR_resources/resolve/main/models/MDXNet/UVR-MDX-NET-Voc_FT.onnx"],
            Sha256: "534b2070fcc7df514b13ef660dc8cbb328679c2374d04354a5c42bb14ecce111",
            SizeBytes: 66_762_490,
            HashSource: "huggingface.co/api/models/Politrees/UVR_resources/tree/main/models/MDXNet, lfs.oid, " +
                        "read 2026-09-11.",
            SourceCount: 2,
            LicenceNote: "Ultimate Vocal Remover community model; no explicit redistribution licence."),

        new OnnxModelEntry(
            Id: "uvr-mdx-net-kim-vocal-2",
            Kind: OnnxStemModelKind.MdxNet,
            DisplayName: "UVR-MDX-NET Kim Vocal 2 (2 stems)",
            FileName: "Kim_Vocal_2.onnx",
            Urls:
            [
                "https://huggingface.co/seanghay/uvr_models/resolve/main/Kim_Vocal_2.onnx",
                "https://huggingface.co/Politrees/UVR_resources/resolve/main/models/MDXNet/Kim_Vocal_2.onnx",
            ],
            Sha256: "ce74ef3b6a6024ce44211a07be9cf8bc6d87728cc852a68ab34eb8e58cde9c8b",
            SizeBytes: 66_759_214,
            HashSource: "lfs.oid from both seanghay/uvr_models and Politrees/UVR_resources on " +
                        "huggingface.co/api, read 2026-09-11; the two agree.",
            SourceCount: 2,
            LicenceNote: "Ultimate Vocal Remover community model; no explicit redistribution licence."),

        new OnnxModelEntry(
            Id: "uvr-mdx-net-kim-inst",
            Kind: OnnxStemModelKind.MdxNet,
            DisplayName: "UVR-MDX-NET Kim Inst (2 stems)",
            FileName: "Kim_Inst.onnx",
            Urls:
            [
                "https://huggingface.co/seanghay/uvr_models/resolve/main/Kim_Inst.onnx",
                "https://huggingface.co/Politrees/UVR_resources/resolve/main/models/MDXNet/Kim_Inst.onnx",
            ],
            Sha256: "86b1940e7122fbdd2beadc65507cbff6c352d79012a8a7e60d56db98532af5f7",
            SizeBytes: 66_759_214,
            HashSource: "lfs.oid from both seanghay/uvr_models and Politrees/UVR_resources on " +
                        "huggingface.co/api, read 2026-09-11; the two agree.",
            SourceCount: 2,
            LicenceNote: "Ultimate Vocal Remover community model; no explicit redistribution licence."),

        new OnnxModelEntry(
            Id: "uvr-mdx-net-inst-main",
            Kind: OnnxStemModelKind.MdxNet,
            DisplayName: "UVR-MDX-NET Inst Main (2 stems)",
            FileName: "UVR-MDX-NET-Inst_Main.onnx",
            Urls: ["https://huggingface.co/seanghay/uvr_models/resolve/main/UVR-MDX-NET-Inst_Main.onnx"],
            Sha256: "8ab401dfe4a548b87deb64f975294bd56ff946aa32903f53b4b24bb13b2cce1e",
            SizeBytes: 52_786_726,
            HashSource: "huggingface.co/api/models/seanghay/uvr_models/tree/main, lfs.oid, read 2026-09-11.",
            SourceCount: 2,
            LicenceNote: "Ultimate Vocal Remover community model; no explicit redistribution licence."),

        // BS-RoFormer SW: the best downloadable 6-stem model for bass AND guitar on
        // MVSep's benchmarks (bass 14.62 / guitar 9.05 SDR against htdemucs_6s'
        // 11.22 / 5.25). The split-complex layout (BsRoformerIoKind.SplitComplex).
        // The fp16 file: weights stored fp16, a Cast to fp32 after each, so the
        // compute is fp32 on the CPU and on DirectML.
        new OnnxModelEntry(
            Id: "bs-roformer-sw-6s",
            Kind: OnnxStemModelKind.BsRoformer,
            DisplayName: "BS-RoFormer SW 6-stem (ONNX)",
            FileName: "bs_roformer_sw_6stem_fp16.onnx",
            Urls: ["https://huggingface.co/elicwhite/bs-roformer-sw-6stem-onnx/resolve/main/bs_roformer_sw_6stem_fp16.onnx"],
            Sha256: "d3d2bac77a7023282cb5f35a5807179e34076b60589867b572275f1a8ec36444",
            SizeBytes: 352_778_874,
            HashSource: "huggingface.co/api/models/elicwhite/bs-roformer-sw-6stem-onnx/tree/main, lfs.oid, " +
                        "read 2026-10-05; a download that day hashed to the same value.",
            SourceCount: 6,
            LicenceNote: "Weights licence UNKNOWN: the trainer of BS-RoFormer SW is not known and the original " +
                         "re-host (jarredou/BS-ROFO-SW-Fixed, since deleted) stated none. The MIT licence on " +
                         "this ONNX repository covers its export code only. A single third-party re-host; the " +
                         "PyTorch checkpoint survives at enerjazzer/BS-ROFO-SW-Fixed. Do not redistribute.",
            BsRoformerLayout: BsRoformerIoLayout.SplitComplex),
    ];

    /// <summary>The entry a bare <see cref="OnnxStemModelKind"/> means. One per
    /// kind; anything else must be named explicitly.</summary>
    private static readonly Dictionary<OnnxStemModelKind, string> DefaultIds = new()
    {
        [OnnxStemModelKind.Htdemucs] = "htdemucs-6s-onnx",
        [OnnxStemModelKind.BsRoformer] = "bs-roformer-fp16",
        [OnnxStemModelKind.MdxNet] = "uvr-mdx-net-inst-hq-3",
    };

    /// <summary>Case-insensitive lookup by <see cref="OnnxModelEntry.Id"/>.</summary>
    public static OnnxModelEntry? FindById(string? id) =>
        string.IsNullOrWhiteSpace(id)
            ? null
            : All.FirstOrDefault(e => string.Equals(e.Id, id, StringComparison.OrdinalIgnoreCase));

    /// <summary>Every entry for one driver.</summary>
    public static IReadOnlyList<OnnxModelEntry> ForKind(OnnxStemModelKind kind) =>
        All.Where(e => e.Kind == kind).ToArray();

    /// <summary>The default entry for a driver.</summary>
    public static OnnxModelEntry DefaultFor(OnnxStemModelKind kind) =>
        FindById(DefaultIds.TryGetValue(kind, out var id) ? id : null)
        ?? throw new NotSupportedException($"No catalogue entry for model kind {kind}.");

    /// <summary>
    /// Ensures the default model for <paramref name="kind"/> is present and
    /// CORRECT in <paramref name="targetDir"/>, and returns its path.
    /// </summary>
    public static Task<string> EnsureAsync(
        OnnxStemModelKind kind,
        string targetDir,
        IProgress<DownloadProgress>? progress = null,
        bool allowUnverified = false,
        Action<string>? log = null,
        CancellationToken ct = default)
        => EnsureAsync(DefaultFor(kind), targetDir, progress, allowUnverified, log, ct);

    /// <summary>
    /// Ensures <paramref name="entry"/> is present and CORRECT in
    /// <paramref name="targetDir"/>, and returns its path.
    ///
    /// <para>
    /// <b>An existing file is checked, not trusted.</b> If the file is already
    /// there and hashes correctly, nothing is downloaded. If it is there and
    /// hashes WRONG, it is discarded and re-downloaded — a half-written model
    /// from a killed process is the common case, and treating "the file exists"
    /// as "the model is installed" is how that becomes a permanent, mystifying
    /// load failure.
    /// </para>
    /// <para>
    /// <b>An unpinned entry is refused by default.</b> Downloading and then
    /// executing an unverifiable model is a decision, so it needs
    /// <paramref name="allowUnverified"/> at the call site; the warning is
    /// logged either way.
    /// </para>
    /// <para>
    /// Cancelling throws <see cref="OperationCanceledException"/> and KEEPS the
    /// per-URL <c>.part</c> file, so the next call resumes. A TIMEOUT is an
    /// <see cref="IOException"/>, not a cancellation, so it falls through to
    /// the next mirror instead of ending the whole attempt.
    /// </para>
    /// </summary>
    /// <exception cref="ArgumentException"><paramref name="entry"/> names a
    ///   file outside <paramref name="targetDir"/>, or a URL that is not
    ///   absolute https (loopback http excepted).</exception>
    /// <exception cref="InvalidOperationException">The entry has no pinned hash
    ///   and <paramref name="allowUnverified"/> is false.</exception>
    /// <exception cref="InvalidDataException">The completed download did not
    ///   match the pinned hash. No file is left at the destination.</exception>
    public static async Task<string> EnsureAsync(
        OnnxModelEntry entry,
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

        // The refusal, the mirror walk and the partial sweep are CatalogueDownload's
        // — they were line-for-line identical to DemucsGgmlModelCatalog's copy
        // (W4-S27). What stays here is what is specific to THIS catalogue: the
        // entry type, ValidateEntry above, and what the file is loaded into.
        return await CatalogueDownload.EnsureAsync(
                entry.Id, entry.Sha256, entry.SizeBytes, entry.FileName, entry.Urls, entry.HashSource,
                loadedAs: "ONNX Runtime",
                targetDir, progress, allowUnverified, log, ct)
            .ConfigureAwait(false);
    }

    /// <summary>
    /// Whether the file at <paramref name="path"/> is the model
    /// <paramref name="kind"/>'s default entry describes. A missing file is
    /// <c>false</c>, not an exception.
    /// </summary>
    public static Task<bool> VerifyAsync(OnnxStemModelKind kind, string path, CancellationToken ct = default)
        => VerifyAsync(DefaultFor(kind), path, ct);

    /// <summary>
    /// Whether the file at <paramref name="path"/> matches
    /// <paramref name="entry"/>'s pinned hash.
    /// </summary>
    /// <exception cref="InvalidOperationException">The entry has no pinned
    ///   hash. That is deliberately not <c>false</c>: "I cannot answer" and
    ///   "the file is corrupt" must not be the same value, or a caller deletes
    ///   a perfectly good model because nobody had hashed it.</exception>
    public static async Task<bool> VerifyAsync(
        OnnxModelEntry entry, string path, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(entry);
        if (entry.Sha256 is null)
            throw new InvalidOperationException(
                $"Model '{entry.Id}' has no pinned SHA-256, so there is nothing to verify against " +
                $"({entry.HashSource}).");

        return await VerifiedDownload.VerifyFileAsync(
            path, entry.Sha256, entry.SizeBytes > 0 ? entry.SizeBytes : null, ct).ConfigureAwait(false);
    }

    /// <summary>Lower-case hex SHA-256 of a file — for pinning a model you
    /// obtained and validated by some other means.</summary>
    /// <param name="path">File to hash.</param>
    /// <param name="ct">Cancellation token.</param>
    /// <returns>64 lower-case hex characters.</returns>
    public static Task<string> ComputeSha256Async(string path, CancellationToken ct = default)
        => VerifiedDownload.ComputeSha256Async(path, ct);

    /// <summary>
    /// Warnings go to the caller's log sink. There is deliberately no fallback
    /// to a process-global logger: this package takes its logging the same way
    /// <see cref="OnnxStemSeparator"/> does (an <c>Action&lt;string&gt;</c> the
    /// host passes in), and an unverified download that warns into a sink
    /// nobody wired is not meaningfully better than one that says nothing —
    /// which is why the unverified path also THROWS unless the caller opted in
    /// at the call site.
    /// </summary>
    private static void Warn(Action<string>? log, string message) => log?.Invoke("WARNING: " + message);

    /// <summary>
    /// <see cref="EnsureAsync(OnnxModelEntry,string,IProgress{DownloadProgress}?,bool,Action{string}?,CancellationToken)"/>
    /// is PUBLIC and takes a caller-constructed record, so the two fields that
    /// decide where bytes land and where they come from are validated here
    /// rather than trusted. The whole purpose of this type is to fetch bytes
    /// that are then loaded and executed by ONNX Runtime, so a host that lets
    /// a settings file or a user name a model would otherwise have handed over
    /// an arbitrary-path writer (<c>FileName = @"..\..\startup.exe"</c>) fed
    /// over whatever transport the entry asked for.
    /// <para>
    /// Plain <c>http</c> is allowed for LOOPBACK only: that is the test
    /// server's shape and it is not a transport an attacker is on. Everything
    /// else must be absolute <c>https</c> — the rule itself lives in
    /// <see cref="VerifiedDownload.ValidateSourceUrl"/>, so that a second
    /// catalogue cannot enforce a subtly different one.
    /// </para>
    /// <para>
    /// <b>It is PUBLIC</b> (it was internal through 0.9, reachable only through
    /// an <c>InternalsVisibleTo</c>): a host that SYNTHESIZES entries — from a
    /// settings file, a user-typed URL, its own catalogue of weights — wants to
    /// reject a bad one where it builds it, with its own error reporting,
    /// rather than discovering it as an <see cref="ArgumentException"/> out of
    /// a download that the user had already been told was starting.
    /// </para>
    /// </summary>
    /// <param name="entry">The entry to check. Nothing is fetched, read or
    ///   written.</param>
    /// <exception cref="ArgumentNullException"><paramref name="entry"/> is null.</exception>
    /// <exception cref="ArgumentException">The file name is not bare, or a URL
    ///   is missing, relative, or not https / loopback http.</exception>
    public static void ValidateEntry(OnnxModelEntry entry)
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
