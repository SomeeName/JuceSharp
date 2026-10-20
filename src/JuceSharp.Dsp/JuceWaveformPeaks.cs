namespace JuceSharp.Dsp;

/// <summary>
/// One waveform bucket — the signed min/max sample pair for a span of
/// frames. Use min and max together to draw the classic "double-envelope"
/// waveform visual in a single pass.
/// </summary>
public readonly record struct JuceWaveformPeak(float Min, float Max);

/// <summary>
/// Pure-managed downsampler that turns a raw audio buffer into a series of
/// min/max peak buckets at a fixed time resolution. Mirrors the native
/// <c>JuceWaveform</c> helper but does not require a JUCE engine — useful
/// for offline thumbnails over arrays already in managed memory.
/// </summary>
public static class JuceWaveformPeaks
{
    /// <summary>
    /// Build peaks from an interleaved-or-mono buffer.
    /// </summary>
    /// <param name="samples">Interleaved samples (or mono if <paramref name="channels"/> == 1).</param>
    /// <param name="channels">Channel count (≥1).</param>
    /// <param name="sampleRate">Audio sample rate.</param>
    /// <param name="bucketMilliseconds">Width of each peak bucket; default 10 ms.</param>
    public static JuceWaveformPeak[] Build(ReadOnlySpan<float> samples, int channels, int sampleRate, double bucketMilliseconds = 10.0)
    {
        if (channels < 1) throw new ArgumentOutOfRangeException(nameof(channels));
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        if (bucketMilliseconds <= 0) throw new ArgumentOutOfRangeException(nameof(bucketMilliseconds));

        var totalFrames = samples.Length / channels;
        if (totalFrames == 0) return [];

        var framesPerBucket = Math.Max(1, (int)Math.Round(sampleRate * bucketMilliseconds / 1000.0));
        var bucketCount = (totalFrames + framesPerBucket - 1) / framesPerBucket;
        var peaks = new JuceWaveformPeak[bucketCount];

        for (var b = 0; b < bucketCount; b++)
        {
            var frameStart = b * framesPerBucket;
            var frameEnd = Math.Min(frameStart + framesPerBucket, totalFrames);
            var min = float.PositiveInfinity;
            var max = float.NegativeInfinity;
            for (var f = frameStart; f < frameEnd; f++)
            {
                var idx = f * channels;
                var s = samples[idx];
                for (var c = 1; c < channels; c++) s += samples[idx + c];
                s /= channels;
                if (s < min) min = s;
                if (s > max) max = s;
            }
            if (float.IsInfinity(min)) { min = 0; max = 0; }
            peaks[b] = new JuceWaveformPeak(min, max);
        }
        return peaks;
    }
}
