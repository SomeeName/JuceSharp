using System.Runtime.InteropServices;
using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
///     Wwise-free Wwise Vorbis (<c>.wem</c>) encoder (ABI v44, DotCharter #317):
///     interleaved float PCM in, a <c>.wem</c> written the way Wwise 2021 writes it
///     for Rocksmith 2014 out, with no Wwise installed.
/// </summary>
/// <remarks>
///     <para>
///         libvorbis (the bundled 1.3.7) at quality 0.4 with an 18 kHz lowpass, the
///         configuration whose setup header is byte-identical to Wwise's, framed the
///         way Wwise frames it. <see cref="JuceWemDecoder" /> decodes everything this
///         writes; that round trip is the test.
///     </para>
///     <para>
///         Only 48 kHz stereo: it is the one configuration whose Wwise header fields
///         are known. Resample and upmix first. Encodes on the calling thread (about
///         a second per minute of audio); never from an audio callback.
///     </para>
/// </remarks>
public static class JuceWemEncoder
{
    /// <summary>The only sample rate <see cref="EncodeVorbis" /> accepts.</summary>
    public const int SampleRate = 48000;

    /// <summary>The only channel count <see cref="EncodeVorbis" /> accepts.</summary>
    public const int Channels = 2;

    /// <summary>Encodes 48 kHz stereo interleaved float PCM to a Wwise Vorbis <c>.wem</c>.</summary>
    /// <exception cref="ArgumentException">Not 48 kHz stereo, empty, or not whole frames.</exception>
    /// <exception cref="JuceAudioException">The native encode failed (the reason is logged).</exception>
    public static unsafe byte[] EncodeVorbis(ReadOnlySpan<float> interleaved, int channels = Channels, int sampleRate = SampleRate)
    {
        if (channels != Channels || sampleRate != SampleRate)
            throw new ArgumentException($"Only {SampleRate} Hz stereo can be encoded (got {sampleRate} Hz, {channels} ch).");
        if (interleaved.IsEmpty || interleaved.Length % channels != 0)
            throw new ArgumentException("The audio must be a non-empty run of whole stereo frames.", nameof(interleaved));
        JuceWemDecoder.EnsureAbi();

        nint data;
        long size;
        fixed (float* p = interleaved)
        {
            JuceAudioEngine.Check(
                NativeMethods.WemEncodeVorbis(p, interleaved.Length / channels, channels, sampleRate, out data, out size),
                "wem_encode_vorbis");
        }

        try
        {
            var bytes = new byte[checked((int)size)];
            Marshal.Copy(data, bytes, 0, bytes.Length);
            return bytes;
        }
        finally
        {
            NativeMethods.WemEncodeFree(data);
        }
    }
}
