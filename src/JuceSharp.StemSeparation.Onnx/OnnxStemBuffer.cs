namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Result of an <see cref="OnnxStemSeparator.SeparateAsync"/> call. One
/// instance per requested stem.
/// </summary>
/// <param name="Name">Conventional stem name (e.g. <c>"vocals"</c>,
/// <c>"drums"</c>, <c>"guitar"</c>). The mapping is model-dependent —
/// HTDemucs 4-stem yields drums/bass/other/vocals; HTDemucs 6-stem adds
/// guitar/piano. BS-RoFormer single-stem vocal models yield vocals+other.</param>
/// <param name="InterleavedStereo">Interleaved stereo PCM samples at
/// <paramref name="SampleRate"/> Hz.</param>
/// <param name="SampleRate">Sample rate the buffer is delivered at — equal
/// to the input sample rate (the engine resamples internally if the model
/// requires a different rate).</param>
public sealed record OnnxStemBuffer(string Name, float[] InterleavedStereo, int SampleRate);
