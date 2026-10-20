namespace JuceSharp.AudioIo;

/// <summary>
/// In-memory interleaved-float32 audio buffer. Format is identical to what
/// <see cref="JuceAudioDecoder"/> and <see cref="WavIo"/> produce:
/// <c>samples[frame * Channels + channel]</c>.
/// </summary>
public sealed record AudioBuffer(float[] Interleaved, int SampleRate, int Channels)
{
    /// <summary>
    /// Number of sample frames in the buffer — <c>Interleaved.Length / Channels</c>,
    /// and 0 for a channel-less buffer rather than a divide by zero.
    /// </summary>
    public int FrameCount => Channels == 0 ? 0 : Interleaved.Length / Channels;

    /// <summary>
    /// Playing time of the buffer in seconds (<see cref="FrameCount"/> divided by
    /// <c>SampleRate</c>), or 0.0 when the sample rate is unknown.
    /// </summary>
    public double DurationSeconds => SampleRate == 0 ? 0.0 : FrameCount / (double)SampleRate;

    /// <summary>De-interleave into one float[] per channel (lengths == FrameCount).</summary>
    public float[][] SplitChannels()
    {
        var frames = FrameCount;
        var planes = new float[Channels][];
        for (var c = 0; c < Channels; c++) planes[c] = new float[frames];
        for (var f = 0; f < frames; f++)
        for (var c = 0; c < Channels; c++)
            planes[c][f] = Interleaved[f * Channels + c];
        return planes;
    }

    /// <summary>Sum to mono (simple average across channels).</summary>
    public float[] ToMono()
    {
        var frames = FrameCount;
        var mono = new float[frames];
        if (Channels == 0) return mono;
        var inv = 1f / Channels;
        for (var f = 0; f < frames; f++)
        {
            var sum = 0f;
            for (var c = 0; c < Channels; c++) sum += Interleaved[f * Channels + c];
            mono[f] = sum * inv;
        }
        return mono;
    }
}
