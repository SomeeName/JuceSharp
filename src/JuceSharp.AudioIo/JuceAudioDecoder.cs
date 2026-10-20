using System.Runtime.InteropServices;
using JuceSharp.Interop;

namespace JuceSharp.AudioIo;

/// <summary>
/// Decodes any audio file the JUCE basic format manager can read (WAV,
/// AIFF, FLAC, OGG Vorbis, plus MP3 on Windows) into a managed
/// <see cref="AudioBuffer"/>. Backed by the native <c>dca_decode_audio_file</c>
/// ABI v6 call — no managed decoder dependencies.
/// </summary>
public static class JuceAudioDecoder
{
    /// <summary>
    /// Returns the file's sample-rate, channel count and per-channel frame
    /// length without actually decoding. Useful for sizing buffers or
    /// rejecting too-long files before paying the decode cost.
    /// </summary>
    public static (int SampleRate, int Channels, long Frames) Probe(string path)
    {
        if (string.IsNullOrEmpty(path)) throw new ArgumentException("Path is required.", nameof(path));
        if (!File.Exists(path)) throw new FileNotFoundException("Audio file not found.", path);

        // MUST go through the sizing-only entry point (out_samples == NULL).
        // Probing through DecodeAudioFile decodes the entire file and hands us
        // ownership of a malloc'd buffer that this method has no way to return
        // — so every probe leaked frames * channels * 4 bytes and paid a full
        // decode for metadata the header serves for free.
        var status = NativeMethods.ProbeAudioFile(path, nint.Zero, out var sr, out var ch, out var frames);
        ThrowIfError(status, path, "probe");
        return (sr, ch, frames);
    }

    /// <summary>
    /// Fully decodes <paramref name="path"/> to interleaved float32.
    /// </summary>
    public static AudioBuffer Decode(string path)
    {
        if (string.IsNullOrEmpty(path)) throw new ArgumentException("Path is required.", nameof(path));
        if (!File.Exists(path)) throw new FileNotFoundException("Audio file not found.", path);

        // Probe + decode are a single native call when out_samples != null.
        // We pass via two-step to allow the native side to return the
        // freshly-allocated buffer pointer.
        var status = NativeMethods.DecodeAudioFile(path, out var ptr, out var sr, out var ch, out var frames);
        ThrowIfError(status, path, "decode");
        if (ptr == nint.Zero) throw new InvalidOperationException($"Native decode returned null for '{path}'.");

        try
        {
            // A `checked((int)...)` here threw a bare OverflowException — no file
            // name, no size, nothing that says the file was simply too long
            // (a 24-bit 96 kHz 6-channel master passes 2^31 samples at ~62 min).
            // The native buffer is still freed by the finally below.
            var totalFloats = CheckedTotalSamples(frames, ch, path);
            var interleaved = new float[totalFloats];
            Marshal.Copy(ptr, interleaved, 0, totalFloats);
            return new AudioBuffer(interleaved, sr, ch);
        }
        finally
        {
            NativeMethods.DecodeFree(ptr);
        }
    }

    /// <summary>
    /// Convenience: decode and resample to a single sample-rate / channel
    /// layout in one shot.
    /// <para><b>The resampler is a two-tap LINEAR interpolation with NO
    /// anti-aliasing filter, in either direction.</b> Downsampling therefore
    /// folds everything above the new Nyquist back into the band as audible
    /// alias tones (44.1 → 16 kHz mirrors 8–22 kHz down onto 0–8 kHz), and
    /// upsampling applies linear interpolation's low-pass roll-off — about
    /// −3.9 dB at the source Nyquist. That is acceptable for envelope- or
    /// onset-level analysis and for feeding a model that was trained on the same
    /// treatment; it is NOT acceptable for anything a user hears, nor for
    /// spectral analysis near the band edge. Use the JUCE-backed playback engine
    /// (or the polyphase load resampler) for those.</para>
    /// </summary>
    /// <param name="path">File to decode.</param>
    /// <param name="targetSampleRate">Rate to convert to, in Hz; must be positive.</param>
    /// <param name="targetChannels">Channel count to convert to; must be positive.</param>
    /// <exception cref="ArgumentOutOfRangeException">Either target is zero or negative.</exception>
    public static AudioBuffer DecodeAs(string path, int targetSampleRate, int targetChannels)
    {
        // BEFORE the decode, not after it: neither target was validated at all, so the
        // caller paid for a whole file first and then got silent data loss or an
        // exception naming nothing. `targetChannels = 0` reached Remix -> new float[0]
        // and returned a Channels = 0 buffer whose FrameCount is 0; `targetSampleRate = 0`
        // reached LinearResample with ratio 0 and returned 0 frames; a negative one threw
        // OverflowException out of an array allocation with no argument name on it.
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(targetSampleRate);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(targetChannels);
        var buf = Decode(path);
        if (buf.Channels != targetChannels)
            buf = Remix(buf, targetChannels);
        if (buf.SampleRate != targetSampleRate)
            buf = LinearResample(buf, targetSampleRate);
        return buf;
    }

    private static AudioBuffer Remix(AudioBuffer src, int targetChannels)
    {
        if (src.Channels == targetChannels) return src;
        var frames = src.FrameCount;
        var dst = new float[frames * targetChannels];

        if (src.Channels == 1 && targetChannels == 2)
        {
            for (var f = 0; f < frames; f++)
            {
                var s = src.Interleaved[f];
                dst[2 * f]     = s;
                dst[2 * f + 1] = s;
            }
        }
        else if (targetChannels == 1)
        {
            var mono = src.ToMono();
            Array.Copy(mono, dst, frames);
        }
        else
        {
            // Truncate / zero-pad channels.
            var copyCh = Math.Min(src.Channels, targetChannels);
            for (var f = 0; f < frames; f++)
            for (var c = 0; c < copyCh; c++)
                dst[f * targetChannels + c] = src.Interleaved[f * src.Channels + c];
        }
        return new AudioBuffer(dst, src.SampleRate, targetChannels);
    }

    private static AudioBuffer LinearResample(AudioBuffer src, int targetRate)
    {
        if (src.SampleRate == targetRate) return src;
        var ratio = (double)targetRate / src.SampleRate;
        var outFrames = (int)Math.Round(src.FrameCount * ratio);
        var ch = src.Channels;
        var dst = new float[outFrames * ch];

        for (var f = 0; f < outFrames; f++)
        {
            var srcPos = f / ratio;
            var i0 = (int)Math.Floor(srcPos);
            var i1 = Math.Min(i0 + 1, src.FrameCount - 1);
            var t = (float)(srcPos - i0);
            for (var c = 0; c < ch; c++)
            {
                var a = src.Interleaved[i0 * ch + c];
                var b = src.Interleaved[i1 * ch + c];
                dst[f * ch + c] = a + (b - a) * t;
            }
        }
        return new AudioBuffer(dst, targetRate, ch);
    }

    /// <summary>
    /// frames * channels as an int, or a NotSupportedException that says which
    /// file and how big. Internal so the overflow branch is testable without a
    /// multi-hour audio file.
    /// </summary>
    internal static int CheckedTotalSamples(long frames, int channels, string path)
    {
        var total = frames * channels;
        if (frames < 0 || channels < 0 || total > Array.MaxLength)
            throw new NotSupportedException(
                $"'{path}' decodes to {total} samples ({frames} frames x {channels} channels), " +
                $"more than a single managed array can address ({Array.MaxLength}). " +
                "Decode a shorter range, or stream it through the playback engine instead.");
        return (int)total;
    }

    internal static void ThrowIfError(NativeMethods.DcaStatus status, string path, string op)
    {
        if (status == NativeMethods.DcaStatus.Ok) return;
        throw status switch
        {
            NativeMethods.DcaStatus.InvalidArg => new ArgumentException($"Invalid argument to native {op} for '{path}'."),
            NativeMethods.DcaStatus.Decode     => new InvalidDataException($"Could not decode '{path}' — unsupported format or corrupt file."),
            // Pass the REAL status through. Hard-coding Internal discarded the one
            // piece of information the caller could act on — every native failure
            // (NotFound, Unsupported, OutOfMemory, …) arrived looking like an
            // internal engine fault.
            _ => new JuceAudioException($"Native {op} failed with status {status} for '{path}'.", status, op),
        };
    }
}
