namespace JuceSharp.StemSeparation.Runtime;

/// <summary>
/// The ONE place a Python package pin is written down.
///
/// <para>
/// <see cref="Processing.StemSeparator.RequiredPackages"/> and
/// <see cref="Processing.CkptToOnnxConverter.RequiredPackages"/> both install
/// into the SAME embedded interpreter root, so a pin that differs between
/// them is not two opinions — it is one interpreter being re-resolved back
/// and forth by whichever component ran last. The two lists carried
/// independent copies of the torch / numpy / pyyaml / bs_roformer pins until
/// this type existed; keeping them in sync was a manual step nobody was
/// reminded to take.
/// </para>
/// <para>
/// Changing any value here changes the package-set stamp hash
/// (<see cref="PythonEnvManager.PackageSetStampFileName"/>) and therefore
/// re-runs pip on the next launch, for both components, automatically.
/// </para>
/// </summary>
internal static class PythonPackagePins
{
    /// <summary>PyTorch. The floor is a security boundary: before 2.6.0
    /// <c>torch.load(weights_only=True)</c> could still run code from the file
    /// (CVE-2025-32434), and the scripts rely on it to read checkpoints
    /// without executing them (DotCharter#105). The ceiling keeps the
    /// range to releases the scripts and torchaudio were checked against.</summary>
    public const string Torch = "torch>=2.6,<2.9";

    /// <summary>
    /// torchaudio must be co-pinned with <see cref="Torch"/>: demucs depends
    /// on torchaudio and, left unconstrained, pip resolves a version whose
    /// native DLLs are ABI-incompatible with the pinned torch. The symptom is
    /// <c>OSError: [WinError 127]</c> when torchaudio's
    /// <c>_extension/_torchaudio.pyd</c> binds to torch C++ symbols.
    /// </summary>
    public const string TorchAudio = "torchaudio>=2.6,<2.9";

    /// <summary>numpy 2 broke the ABI every torch &lt; 2.3 wheel was built against; kept
    /// for the rest of the stack (demucs, librosa) now that torch is 2.6+.</summary>
    public const string Numpy = "numpy<2";

    /// <summary>Config loading for the RoFormer family.</summary>
    public const string PyYaml = "pyyaml";

    /// <summary>wav/flac IO for the separator sidecar.</summary>
    public const string SoundFile = "soundfile";

    /// <summary>Resampling for the separator sidecar.</summary>
    public const string Librosa = "librosa";

    /// <summary>htdemucs / mdx_extra families. 4.1 is the floor because 4.0.x
    /// calls <c>torch.load</c> with the default torch 2.6 flipped to
    /// <c>weights_only=True</c>.
    /// <para>4.1's own weight loading is NOT a pin, checked in its source: it
    /// tries the HuggingFace hub first with no revision and no hash, and falls
    /// back to <c>dl.fbaipublicfiles.com</c> with <c>check_hash=True</c>, which
    /// compares only the 8-hex-digit (32-bit) SHA-256 prefix in the file name
    /// before an unpickle with <c>weights_only=False</c>. So the script never
    /// lets demucs fetch: <see cref="DemucsWeightPins"/> does, with full
    /// hashes.</para></summary>
    public const string Demucs = "demucs>=4.1";

    /// <summary>BS-RoFormer family.</summary>
    public const string BsRoformer = "bs_roformer>=1.1.0";

    /// <summary>Pickle-free <c>.safetensors</c> checkpoints, read by both scripts.</summary>
    public const string SafeTensors = "safetensors>=0.4";

    /// <summary>ONNX graph export, converter only.</summary>
    public const string Onnx = "onnx>=1.15";

    /// <summary>
    /// The directory each pin lands in under <c>Lib/site-packages</c>.
    ///
    /// <para>
    /// Used by <see cref="PythonEnvManager.EnsurePackagesAsync"/> to decide
    /// whether a <c>.packages-&lt;sha&gt;.stamp</c> is still TRUE: the stamp
    /// records the pin SET and nothing about the tree it was installed into,
    /// so a deleted <c>torch</c>, a failed uninstall or an antivirus
    /// quarantine otherwise left pip permanently un-run.
    /// </para>
    /// <para>
    /// It is a hand-written map and not a guess from the spec string on
    /// purpose: the distribution name and the import name differ often enough
    /// (<c>pyyaml</c> installs <c>yaml/</c>) that deriving it wrong would
    /// re-run pip on every launch — which is the failure the stamp exists to
    /// prevent. A pin that is not in here is simply not checked, so adding a
    /// new pin is never a regression; adding its row here is what makes the
    /// check cover it. <c>PythonPackagePinTests</c> keeps the map complete for
    /// the pins this repo ships.
    /// </para>
    /// </summary>
    public static IReadOnlyDictionary<string, string> InstalledDirectories { get; } =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            [Torch] = "torch",
            [TorchAudio] = "torchaudio",
            [Numpy] = "numpy",
            [PyYaml] = "yaml",
            [SoundFile] = "soundfile",
            [Librosa] = "librosa",
            [Demucs] = "demucs",
            [BsRoformer] = "bs_roformer",
            [Onnx] = "onnx",
            [SafeTensors] = "safetensors",
        };
}
