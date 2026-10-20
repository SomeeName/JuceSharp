using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;

namespace JuceSharp.AudioIo;

/// <summary>Sample format <see cref="WavWriter"/> writes to disk.</summary>
public enum WavSampleFormat
{
    /// <summary>32-bit IEEE float, `WAVE_FORMAT_IEEE_FLOAT` (3) + a `fact` chunk.
    /// Lossless for the float data you hand in, and the only one of the three
    /// that cannot clip.</summary>
    Float32,

    /// <summary>16-bit signed PCM. Scaled by 32768 (NOT 32767) and rounded half
    /// away from zero, then clamped to <c>[-32768, 32767]</c>.
    /// <para>32768 is the exact inverse of the <c>/ 32768f</c> the reader applies,
    /// so the only round-trip error is the half-LSB of rounding. Scaling by 32767
    /// "to avoid clipping at +1.0" instead adds a systematic gain error of
    /// <c>v / 32768</c> on TOP of it — measured 4.4e-5 against a 3.05e-5
    /// quantisation step, i.e. the correction was bigger than the thing it was
    /// correcting. Full scale clips to 32767 by one LSB; that is what 16-bit is.</para></summary>
    Pcm16,

    /// <summary>24-bit signed PCM (3 bytes, little-endian). Scaled by 8388608 — the
    /// exact inverse of the reader's divisor, same reasoning as
    /// <see cref="Pcm16"/> — and clamped to <c>[-8388608, 8388607]</c>.</summary>
    Pcm24,
}

/// <summary>
/// Streaming RIFF/WAVE writer: open once, push blocks, close.
///
/// <para><see cref="WavIo.WriteFloat32(string, AudioBuffer)"/> needs the whole
/// signal in one array, which is the wrong shape for anything that produces
/// audio incrementally (a render loop, a capture ring, a stem being decoded).
/// This writes a provisional header up front, appends as you go, and patches the
/// RIFF / `data` / `fact` sizes on <see cref="Dispose"/>. Allocation-free per
/// write after construction; the float32 path does not even copy, it hands the
/// caller's span straight to the stream.</para>
///
/// <para><b>An unfinished file is readable by <see cref="WavIo.Read(string)"/>,
/// not corrupt — and that promise is scoped to THAT reader.</b> The provisional
/// sizes are written as <c>0xFFFFFFFF</c> — the RIFF streaming/unknown-length
/// placeholder — rather than zero, so a process that dies before
/// <see cref="Dispose"/> leaves a file <see cref="WavIo.Read(string)"/> still
/// decodes in full, because it deliberately clamps an unknown length to the
/// bytes actually present. <b>Most other readers do not.</b>
/// <see cref="JuceAudioDecoder"/> (JUCE's <c>WavAudioFormat</c>), ffmpeg and the
/// common editors treat <c>0xFFFFFFFF</c> as a real declared size and will
/// report a truncated file, refuse it, or read past the end — so do not hand an
/// unfinished file to one and expect the audio back. Writing zeros would still
/// have been worse: a *valid-looking* WAV declaring no audio, i.e. silent data
/// loss with no error anywhere. <see cref="IsUnfinished(string)"/> reports the
/// state explicitly; the fix for a file that matters is to re-open it and let a
/// writer patch the sizes, not to hope.</para>
///
/// <para><b>4 GB is a hard limit and it THROWS.</b> RIFF sizes are unsigned
/// 32-bit; RF64 is not written. A <see cref="Write(ReadOnlySpan{float})"/> that
/// would push the `data` chunk past the limit throws
/// <see cref="NotSupportedException"/> <i>before</i> writing anything, so the
/// file on disk stays consistent — the alternative, discovering it at
/// <see cref="Dispose"/>, means the overflow is already on disk and
/// unrecoverable.</para>
/// </summary>
public sealed class WavWriter : IDisposable
{
    private const ushort FormatPcm       = 1;
    private const ushort FormatIeeeFloat = 3;

    /// <summary>The provisional size written into every size field until
    /// <see cref="Dispose"/> patches it. Not zero — see the type remarks.</summary>
    internal const uint UnknownSize = uint.MaxValue;

    // RIFF sizes are unsigned 32-bit and the header lives inside the RIFF chunk,
    // so the payload bound is what is left after it.
    private const long MaxDataBytes = uint.MaxValue - 64;

    private const int ScratchFrames = 16 * 1024;

    private readonly Stream _stream;
    private readonly bool   _leaveOpen;
    private readonly long   _riffSizeOffset;
    private readonly long   _factOffset;      // -1 when the format has no fact chunk
    private readonly long   _dataSizeOffset;
    private readonly int    _bytesPerSample;
    private readonly byte[] _scratch;
    // Allocated in the constructor, never lazily — see the note there.
    private readonly float[] _interleave;

    private long _dataBytes;
    private bool _disposed;

    /// <summary>Sample rate written into the header.</summary>
    public int SampleRate { get; }

    /// <summary>Channel count; every <see cref="Write(ReadOnlySpan{float})"/>
    /// must be a whole number of frames.</summary>
    public int Channels { get; }

    /// <summary>On-disk sample format.</summary>
    public WavSampleFormat Format { get; }

    /// <summary>Frames written so far.</summary>
    public long FrameCount => _dataBytes / (_bytesPerSample * Channels);

    /// <summary>Payload bytes written so far, excluding the header and any pad byte.</summary>
    public long DataBytes => _dataBytes;

    /// <summary>Create (or truncate) a file and write its provisional header.
    /// <para><b>Nothing is created until every scalar argument has been
    /// validated.</b> This used to delegate straight through
    /// <c>File.Create(path)</c>, so an out-of-range <paramref name="format"/>, a
    /// <paramref name="sampleRate"/> of 0 or a <paramref name="channels"/> of 0
    /// threw from the target constructor with the file already open — leaking the
    /// <c>FileStream</c> until finalization and leaving a locked, zero-byte file
    /// behind meanwhile. Should the header write itself fail (a full disk), the
    /// target constructor now disposes the stream it was handed, because
    /// <c>leaveOpen: false</c> means it took ownership.</para></summary>
    public WavWriter(string path, int sampleRate, int channels,
                     WavSampleFormat format = WavSampleFormat.Float32)
        : this(CreateValidated(path, sampleRate, channels, format), sampleRate, channels, format,
               leaveOpen: false)
    {
    }

    private static Stream CreateValidated(string path, int sampleRate, int channels, WavSampleFormat format)
    {
        ArgumentNullException.ThrowIfNull(path);
        ValidateScalars(sampleRate, channels, format);
        return File.Create(path);
    }

    // The one place the scalar rules live, so the file constructor can apply them
    // BEFORE opening anything and the stream constructor can apply them after.
    private static int ValidateScalars(int sampleRate, int channels, WavSampleFormat format)
    {
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        if (channels   <= 0) throw new ArgumentOutOfRangeException(nameof(channels));

        var bytesPerSample = format switch
        {
            WavSampleFormat.Float32 => 4,
            WavSampleFormat.Pcm16   => 2,
            WavSampleFormat.Pcm24   => 3,
            _ => throw new ArgumentOutOfRangeException(nameof(format)),
        };

        // `fmt ` stores nChannels and nBlockAlign as 16-bit fields. Casting past
        // them wrote a header that silently declared a DIFFERENT channel layout
        // from the one every Write then validates against: 65,536 channels
        // truncates to 0, and 22,000 stereo-equivalent 24-bit channels overflow
        // blockAlign alone. Neither is a real use; both produced a file no reader
        // could interpret, with no error at the point of the mistake.
        if (channels > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(channels),
                $"A RIFF/WAV 'fmt ' chunk stores the channel count in 16 bits; {channels} does not fit.");
        var blockAlign = (long)channels * bytesPerSample;
        if (blockAlign > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(channels),
                $"A RIFF/WAV 'fmt ' chunk stores nBlockAlign in 16 bits; {channels} channels of " +
                $"{bytesPerSample}-byte samples needs {blockAlign}.");
        return bytesPerSample;
    }

    /// <summary>Write into an open, SEEKABLE stream at its current position.</summary>
    /// <param name="stream">Destination; must be writable and seekable.</param>
    /// <param name="sampleRate">Rate written into the header.</param>
    /// <param name="channels">Channel count.</param>
    /// <param name="format">On-disk sample format.</param>
    /// <param name="leaveOpen">Leave <paramref name="stream"/> open on
    /// <see cref="Dispose"/>. The header is patched either way.</param>
    public WavWriter(Stream stream, int sampleRate, int channels,
                     WavSampleFormat format = WavSampleFormat.Float32, bool leaveOpen = false)
    {
        ArgumentNullException.ThrowIfNull(stream);
        // From here on this constructor OWNS the stream when leaveOpen is false,
        // so anything that throws below must not leave it open: the string
        // constructor delegates here with a freshly created FileStream, and a
        // throw used to leak it until finalization.
        try
        {
            if (!stream.CanWrite)
                throw new ArgumentException("WavWriter requires a writable stream.", nameof(stream));
            // The header is patched at the end, so the stream must be able to go back
            // to it. A non-seekable stream would leave every size at the placeholder.
            if (!stream.CanSeek)
                throw new ArgumentException(
                    "WavWriter requires a seekable stream: the RIFF/data sizes are patched on Dispose.",
                    nameof(stream));

            _bytesPerSample = ValidateScalars(sampleRate, channels, format);

            _stream    = stream;
            _leaveOpen = leaveOpen;
            SampleRate = sampleRate;
            Channels   = channels;
            Format     = format;

            _scratch = new byte[ScratchFrames * _bytesPerSample];

            var tag        = format == WavSampleFormat.Float32 ? FormatIeeeFloat : FormatPcm;
            var blockAlign = channels * _bytesPerSample;
            var byteRate   = (long)sampleRate * blockAlign;

            // Eagerly, NOT on first WritePlanar: the type doc promises
            // "allocation-free per write after construction", and a lazy `??=`
            // made the first planar write break that promise for a caller who had
            // no way to see it coming. One block's worth of interleave scratch,
            // the same order as _scratch itself.
            var framesPerBlock = Math.Max(1, _scratch.Length / (_bytesPerSample * channels));
            _interleave = new float[framesPerBlock * channels];

            using var bw = new BinaryWriter(stream, Encoding.ASCII, leaveOpen: true);
            bw.Write("RIFF"u8);
            _riffSizeOffset = stream.Position;
            bw.Write(UnknownSize);
            bw.Write("WAVE"u8);
            bw.Write("fmt "u8);
            bw.Write(16);
            bw.Write(tag);
            bw.Write((ushort)channels);
            bw.Write((uint)sampleRate);
            bw.Write((uint)byteRate);
            bw.Write((ushort)blockAlign);
            bw.Write((ushort)(_bytesPerSample * 8));

            if (tag == FormatIeeeFloat)
            {
                // A `fact` chunk carrying the per-channel sample length is REQUIRED
                // for every non-PCM format tag by the RIFF spec, and some strict
                // readers reject a float WAV without one.
                bw.Write("fact"u8);
                bw.Write(4);
                _factOffset = stream.Position;
                bw.Write(UnknownSize);
            }
            else
            {
                _factOffset = -1;
            }

            bw.Write("data"u8);
            _dataSizeOffset = stream.Position;
            bw.Write(UnknownSize);
        }
        catch
        {
            if (!leaveOpen) stream.Dispose();
            throw;
        }
    }

    /// <summary>
    /// Append interleaved frames (<c>samples[frame * Channels + channel]</c>).
    /// Length must be a whole number of frames. Allocation-free.
    /// </summary>
    public void Write(ReadOnlySpan<float> interleaved)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (interleaved.IsEmpty) return;
        if (interleaved.Length % Channels != 0)
            throw new ArgumentException(
                $"Interleaved length {interleaved.Length} is not a whole number of {Channels}-channel frames.",
                nameof(interleaved));

        var adding = (long)interleaved.Length * _bytesPerSample;
        // Checked BEFORE anything is written: an overflow discovered at Dispose
        // is already on disk.
        if (_dataBytes + adding > MaxDataBytes)
            throw new NotSupportedException(
                $"A RIFF/WAV data chunk cannot exceed {MaxDataBytes} bytes and this write would reach " +
                $"{_dataBytes + adding}. RF64 is not written; split the output across files.");

        if (Format == WavSampleFormat.Float32 && BitConverter.IsLittleEndian)
        {
            // Float32 little-endian IS the on-disk layout: no conversion, no copy.
            _stream.Write(MemoryMarshal.AsBytes(interleaved));
            _dataBytes += adding;
            return;
        }

        var perBlock = _scratch.Length / _bytesPerSample;
        var offset = 0;
        while (offset < interleaved.Length)
        {
            var take = Math.Min(perBlock, interleaved.Length - offset);
            Encode(interleaved.Slice(offset, take), _scratch);
            _stream.Write(_scratch, 0, take * _bytesPerSample);
            offset += take;
        }
        _dataBytes += adding;
    }

    /// <summary>
    /// Append from one array per channel (each at least
    /// <c>offset + frameCount</c> long), interleaving into the internal scratch
    /// buffer. Allocation-free.
    /// </summary>
    public void WritePlanar(IReadOnlyList<float[]> planes, int offset, int frameCount)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(planes);
        if (planes.Count != Channels)
            throw new ArgumentException($"Expected {Channels} planes, got {planes.Count}.", nameof(planes));
        if (offset < 0 || frameCount < 0) throw new ArgumentOutOfRangeException(nameof(offset));
        if (frameCount == 0) return;
        for (var c = 0; c < planes.Count; c++)
            if (planes[c] is null || planes[c].Length < offset + frameCount)
                throw new ArgumentException($"Plane {c} is shorter than offset + frameCount.", nameof(planes));

        var framesPerBlock = Math.Max(1, _scratch.Length / (_bytesPerSample * Channels));
        var interleaveBuffer = _interleave;
        var done = 0;
        while (done < frameCount)
        {
            var take = Math.Min(framesPerBlock, frameCount - done);
            for (var f = 0; f < take; f++)
            for (var c = 0; c < Channels; c++)
                interleaveBuffer[f * Channels + c] = planes[c][offset + done + f];
            Write(interleaveBuffer.AsSpan(0, take * Channels));
            done += take;
        }
    }

    /// <summary>Convenience overload: write every frame of each plane.</summary>
    public void WritePlanar(IReadOnlyList<float[]> planes)
    {
        ArgumentNullException.ThrowIfNull(planes);
        // `planes[0]` is a float[] that can itself be null, and reading `.Length`
        // off it here raised a bare NullReferenceException BEFORE the three-arg
        // form's per-plane check could report which plane was wrong. Same
        // diagnosis, same exception type, one call earlier.
        if (planes.Count > 0 && planes[0] is null)
            throw new ArgumentException("Plane 0 is null.", nameof(planes));
        WritePlanar(planes, 0, planes.Count == 0 ? 0 : planes[0].Length);
    }

    private void Encode(ReadOnlySpan<float> src, Span<byte> dst)
    {
        switch (Format)
        {
            case WavSampleFormat.Float32:
                for (var i = 0; i < src.Length; i++)
                    BinaryPrimitives.WriteInt32LittleEndian(dst.Slice(i * 4, 4), BitConverter.SingleToInt32Bits(src[i]));
                break;

            case WavSampleFormat.Pcm16:
                for (var i = 0; i < src.Length; i++)
                    BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(i * 2, 2), (short)Quantize(src[i], 32768f, -32768, 32767));
                break;

            case WavSampleFormat.Pcm24:
                for (var i = 0; i < src.Length; i++)
                {
                    var v = Quantize(src[i], 8388608f, -8388608, 8388607);
                    var o = i * 3;
                    dst[o]     = (byte)(v & 0xFF);
                    dst[o + 1] = (byte)((v >> 8) & 0xFF);
                    dst[o + 2] = (byte)((v >> 16) & 0xFF);
                }
                break;
        }
    }

    /// <summary>
    /// Scale by 2^(bits-1) — the exact inverse of the reader's divisor — round half
    /// away from zero, clamp. No dither — a writer that
    /// dithered silently would make its own round-trip test unreproducible, and
    /// dither is a mastering decision the caller makes, not a file format.
    /// <para>A non-finite input is written as SILENCE rather than as whatever
    /// <c>(int)NaN</c> happens to produce (0 on x64, INT_MIN elsewhere): a
    /// full-scale click is a worse answer than a gap, and the comparison
    /// <c>NaN &gt; max</c> is false, so an unguarded clamp lets it straight
    /// through.</para>
    /// </summary>
    private static int Quantize(float sample, float scale, int min, int max)
    {
        if (!float.IsFinite(sample)) return 0;
        var v = MathF.Round(sample * scale, MidpointRounding.AwayFromZero);
        if (v <= min) return min;
        if (v >= max) return max;
        return (int)v;
    }

    /// <summary>
    /// Does this file still carry the provisional (unknown-length) sizes — i.e.
    /// was it written by a <see cref="WavWriter"/> that never reached
    /// <see cref="Dispose"/>? Such a file is still fully readable by
    /// <see cref="WavIo.Read(string)"/> — and, as the type remarks spell out, by
    /// that reader specifically, not by JUCE's <c>WavAudioFormat</c> or by most
    /// third-party tools. This only reports that no writer closed it.
    /// </summary>
    public static bool IsUnfinished(string path)
    {
        using var fs = File.OpenRead(path);
        return IsUnfinished(fs);
    }

    /// <summary>
    /// Does the RIFF header at <paramref name="headerOffset"/> still carry the
    /// provisional (unknown-length) size? See <see cref="IsUnfinished(string)"/>.
    /// <para><b>The offset is not decoration.</b> This reads at an ABSOLUTE
    /// position and defaults to 0, which is right for a file and wrong for the
    /// case <see cref="WavWriter(Stream, int, int, WavSampleFormat, bool)"/>
    /// explicitly supports: a writer constructed over an open stream "at its
    /// current position". For such a writer, offset 0 is somebody else's bytes
    /// and the answer was nonsense — usually a confident <c>false</c>, i.e. "this
    /// file was closed properly", about a file that was not. Pass the position
    /// the writer started at.</para>
    /// <para>The stream's position is restored before returning either way.</para>
    /// </summary>
    public static bool IsUnfinished(Stream stream, long headerOffset = 0)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (!stream.CanSeek) throw new ArgumentException("Needs a seekable stream.", nameof(stream));
        ArgumentOutOfRangeException.ThrowIfNegative(headerOffset);
        var start = stream.Position;
        try
        {
            Span<byte> head = stackalloc byte[8];
            stream.Seek(headerOffset, SeekOrigin.Begin);
            if (stream.ReadAtLeast(head, 8, throwOnEndOfStream: false) < 8) return false;
            if (head[0] != (byte)'R' || head[1] != (byte)'I' || head[2] != (byte)'F' || head[3] != (byte)'F')
                return false;
            return BinaryPrimitives.ReadUInt32LittleEndian(head[4..]) == UnknownSize;
        }
        finally
        {
            stream.Seek(start, SeekOrigin.Begin);
        }
    }

    /// <summary>Patch the RIFF / `data` / `fact` sizes, add the RIFF pad byte if
    /// the payload length is odd, and close the stream unless the caller asked
    /// to keep it. Idempotent.</summary>
    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try
        {
            // Every chunk is padded to an even length; the pad is NOT counted in
            // the declared size. 24-bit PCM with an odd sample count is the case
            // that reaches this (float32 and 16-bit are always even).
            var end = _stream.Position;
            if ((_dataBytes & 1) != 0)
            {
                _stream.WriteByte(0);
                end++;
            }

            var riffSize = end - (_riffSizeOffset + 4);

            _stream.Seek(_dataSizeOffset, SeekOrigin.Begin);
            WriteUInt32(_stream, (uint)_dataBytes);

            if (_factOffset >= 0)
            {
                _stream.Seek(_factOffset, SeekOrigin.Begin);
                WriteUInt32(_stream, (uint)FrameCount);
            }

            _stream.Seek(_riffSizeOffset, SeekOrigin.Begin);
            WriteUInt32(_stream, (uint)riffSize);

            _stream.Seek(end, SeekOrigin.Begin);
            _stream.Flush();
        }
        finally
        {
            if (!_leaveOpen) _stream.Dispose();
        }
    }

    private static void WriteUInt32(Stream stream, uint value)
    {
        Span<byte> b = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(b, value);
        stream.Write(b);
    }
}
