namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Which family of ONNX model an <see cref="OnnxStemSeparator"/> instance
/// should drive. The model file itself doesn't carry a "kind" tag in its
/// metadata; the caller knows whether they downloaded an HTDemucs export,
/// a BS-RoFormer checkpoint converted to ONNX, or an MDX-Net file.
/// </summary>
public enum OnnxStemModelKind
{
    /// <summary>An HTDemucs export: a hybrid time/spectrogram model producing four stems
    /// (drums, bass, other, vocals) from an interleaved stereo waveform.</summary>
    Htdemucs,
    /// <summary>A BS-RoFormer checkpoint converted to ONNX: a band-split transformer that
    /// works on the STFT and is driven with its own chunk and overlap settings.</summary>
    BsRoformer,
    /// <summary>An MDX-Net file: a spectrogram U-Net whose fixed <c>dimF</c>/<c>dimT</c>
    /// geometry has to match the settings it is constructed with.</summary>
    MdxNet,
}
