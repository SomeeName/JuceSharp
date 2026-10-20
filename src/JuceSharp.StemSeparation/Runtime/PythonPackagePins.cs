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
    /// <summary>PyTorch. The upper bound is deliberate: 2.6 changed
    /// <c>torch.load</c>'s default to <c>weights_only=True</c>, which breaks
    /// loading the demucs / RoFormer checkpoints as they are published.</summary>
    public const string Torch = "torch>=2.1,<2.6";

    /// <summary>
    /// torchaudio must be co-pinned with <see cref="Torch"/>: demucs depends
    /// on torchaudio and, left unconstrained, pip resolves a version whose
    /// native DLLs are ABI-incompatible with the pinned torch. The symptom is
    /// <c>OSError: [WinError 127]</c> when torchaudio's
    /// <c>_extension/_torchaudio.pyd</c> binds to torch C++ symbols.
    /// </summary>
    public const string TorchAudio = "torchaudio>=2.1,<2.6";

    /// <summary>numpy 2 breaks the ABI every torch &lt; 2.3 wheel was built against.</summary>
    public const string Numpy = "numpy<2";

    /// <summary>Config loading for the RoFormer family.</summary>
    public const string PyYaml = "pyyaml";

    /// <summary>wav/flac IO for the separator sidecar.</summary>
    public const string SoundFile = "soundfile";

    /// <summary>Resampling for the separator sidecar.</summary>
    public const string Librosa = "librosa";

    /// <summary>htdemucs / mdx_extra families.</summary>
    public const string Demucs = "demucs>=4.0";

    /// <summary>BS-RoFormer family.</summary>
    public const string BsRoformer = "bs_roformer>=1.1.0";

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
        };
}
