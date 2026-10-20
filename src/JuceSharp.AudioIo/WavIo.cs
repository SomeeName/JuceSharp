using System.Buffers.Binary;
using System.Text;

namespace JuceSharp.AudioIo;

/// <summary>
/// Pure-managed RIFF/WAVE PCM reader/writer.
/// <para>Reads 8/16/24/32-bit integer PCM and 32/64-bit IEEE float, in either
/// the plain <c>WAVE_FORMAT_PCM</c>/<c>WAVE_FORMAT_IEEE_FLOAT</c> form or
/// <c>WAVE_FORMAT_EXTENSIBLE</c> (0xFFFE) — which is what every DAW emits for
/// more than two channels, for 24-bit, and for most 32-bit float exports.
/// Chunks it does not understand are skipped, INCLUDING the RIFF pad byte that
/// follows an odd-sized chunk: <c>LIST</c>, <c>bext</c> and <c>iXML</c> are
/// routinely odd, and a reader that forgets the pad lands one byte off and then
/// reports a missing <c>data</c> chunk.</para>
/// Useful when you need to round-trip audio without touching the native engine
/// (tests, offline analysis, simple tooling). For non-WAV inputs use
/// <see cref="JuceAudioDecoder.Decode(string)"/>.
/// </summary>
public static class WavIo
{
    private const ushort FormatPcm        = 1;
    private const ushort FormatIeeeFloat  = 3;
    private const ushort FormatExtensible = 0xFFFE;

    /// <summary>Read a WAV file into an interleaved <see cref="AudioBuffer"/>.</summary>
    public static AudioBuffer Read(string path)
    {
        using var fs = File.OpenRead(path);
        return Read(fs);
    }

    /// <summary>Read a WAV from any seekable stream.</summary>
    public static AudioBuffer Read(Stream stream)
    {
        if (!stream.CanSeek) throw new ArgumentException("WavIo.Read requires a seekable stream.", nameof(stream));
        using var br = new BinaryReader(stream, Encoding.ASCII, leaveOpen: true);
        if (ReadFourCc(br) != "RIFF") throw new InvalidDataException("Not a RIFF file.");
        br.ReadUInt32(); // RIFF size — not trusted; the stream length bounds every read.
        if (ReadFourCc(br) != "WAVE") throw new InvalidDataException("Not a WAVE file.");

        ushort fmt = 0, channels = 0, bitsPerSample = 0;
        uint sampleRate = 0;
        var haveFmt = false;
        // A JuceSharp device-rate WAV (ABI v43) stores int16 samples at the engine's
        // preload step, which can be coarser than 1/32768; its 'dcps' chunk says how
        // much one unit is worth. 0 = no such chunk.
        var storedStep = 0f;

        // Chunk headers are 8 bytes, so a chunk cannot start within 8 bytes of EOF.
        while (stream.Position + 8 <= stream.Length)
        {
            var chunkId = ReadFourCc(br);
            var declaredSize = br.ReadUInt32();
            var bodyStart = stream.Position;
            var remaining = stream.Length - bodyStart;

            // 0xFFFFFFFF is the streaming/unknown-length placeholder, and a
            // truncated or lying header is just as common. Clamp to what the
            // stream actually holds; never hand a negative or absurd count on.
            var chunkSize = declaredSize == uint.MaxValue ? remaining : Math.Min((long)declaredSize, remaining);

            if (chunkId == "fmt ")
            {
                if (chunkSize < 16) throw new InvalidDataException("WAV 'fmt ' chunk is too short.");
                fmt           = br.ReadUInt16();
                channels      = br.ReadUInt16();
                sampleRate    = br.ReadUInt32();
                br.ReadUInt32(); // byte rate
                br.ReadUInt16(); // block align
                bitsPerSample = br.ReadUInt16();
                haveFmt       = true;

                // Validate HERE, in the `fmt ` branch, not after the data chunk has
                // been decoded. A hostile or corrupt header declaring 0 channels
                // used to reach `ReadSamples` first, which allocates and decodes up
                // to Array.MaxLength floats (~2 GB at 4 bytes each) before the
                // `channels == 0` throw one line further down could fire.
                if (channels == 0)
                    throw new InvalidDataException("WAV 'fmt ' chunk declares 0 channels.");
                if (sampleRate == 0)
                    throw new InvalidDataException("WAV 'fmt ' chunk declares a 0 sample rate.");

                if (fmt == FormatExtensible)
                {
                    // fmt_ext: cbSize(2) validBitsPerSample(2) channelMask(4) SubFormat GUID(16).
                    // The first two GUID bytes are the real format tag, little-endian.
                    if (chunkSize < 40)
                        throw new InvalidDataException(
                            "WAVE_FORMAT_EXTENSIBLE 'fmt ' chunk is too short to carry a sub-format GUID.");
                    br.ReadUInt16();                       // cbSize
                    var validBits = br.ReadUInt16();       // valid bits per sample
                    br.ReadUInt32();                       // channel mask
                    var guid = br.ReadBytes(16);
                    fmt = BinaryPrimitives.ReadUInt16LittleEndian(guid);
                    // A container that carries 24 valid bits in 32-bit words is a
                    // different layout; we only support container == valid bits.
                    if (validBits != 0 && validBits != bitsPerSample)
                        throw new InvalidDataException(
                            $"Unsupported WAVE_FORMAT_EXTENSIBLE layout: {validBits} valid bits in a " +
                            $"{bitsPerSample}-bit container.");
                }

                SeekChunkEnd(stream, bodyStart, chunkSize);
            }
            else if (chunkId == "dcps" && chunkSize >= 8)
            {
                var version = br.ReadUInt32();
                var step = br.ReadSingle();
                if (version == 1 && float.IsFinite(step) && step >= 1f / 32768f && step < 1f) storedStep = step;
                SeekChunkEnd(stream, bodyStart, chunkSize);
            }
            else if (chunkId == "data")
            {
                if (!haveFmt) throw new InvalidDataException("WAV 'data' chunk precedes 'fmt '.");
                // channels / sampleRate were validated in the `fmt ` branch above,
                // BEFORE this decode, which is the whole point of moving them there.
                var samples = ReadSamples(br, chunkSize, fmt, bitsPerSample, channels);
                if (storedStep > 1f / 32768f && fmt == FormatPcm && bitsPerSample == 16)
                {
                    var gain = (float)(storedStep * 32768.0);
                    for (var i = 0; i < samples.Length; i++) samples[i] *= gain;
                }
                return new AudioBuffer(samples, (int)sampleRate, channels);
            }
            else
            {
                SeekChunkEnd(stream, bodyStart, chunkSize);
            }
        }

        if (!haveFmt) throw new InvalidDataException("WAV missing 'fmt ' chunk.");
        throw new InvalidDataException("WAV missing 'data' chunk.");
    }

    private static string ReadFourCc(BinaryReader br)
    {
        var b = br.ReadBytes(4);
        if (b.Length < 4) throw new InvalidDataException("Truncated WAV: incomplete chunk identifier.");
        return Encoding.ASCII.GetString(b);
    }

    /// <summary>
    /// Seek past a chunk body AND its RIFF pad byte. Every chunk is padded to an
    /// even length; the pad is NOT counted in the declared size. Forgetting it
    /// desynchronises the reader after any odd chunk.
    /// </summary>
    private static void SeekChunkEnd(Stream stream, long bodyStart, long chunkSize)
    {
        var end = bodyStart + chunkSize;
        if ((chunkSize & 1) != 0 && end < stream.Length) end++;
        stream.Seek(Math.Min(end, stream.Length), SeekOrigin.Begin);
    }

    private static float[] ReadSamples(BinaryReader br, long byteCount, ushort fmt, ushort bitsPerSample,
                                       ushort channels)
    {
        var bytesPerSample = FormatBytesPerSample(fmt, bitsPerSample);
        var sampleCount = byteCount / bytesPerSample;
        // Round DOWN to a whole number of interleaved frames. A truncated file
        // otherwise produces an AudioBuffer whose length is not a multiple of
        // Channels, and every consumer's `Length / Channels` frame math is then
        // off by one interleaved group — a silent channel rotation rather than an
        // error. Dropping the partial frame loses at most one sample per channel.
        if (channels > 1) sampleCount -= sampleCount % channels;
        if (sampleCount > Array.MaxLength)
            throw new NotSupportedException(
                $"WAV 'data' chunk holds {sampleCount} samples, more than a single managed array can " +
                $"address ({Array.MaxLength}). Stream the file instead of reading it whole.");

        var samples = new float[sampleCount];
        // Read in bounded blocks: a >2 GB data chunk must never become one
        // intermediate byte[] (and ReadBytes((int)size) turned it negative).
        var block = new byte[Math.Min(1 << 16, Math.Max(bytesPerSample, byteCount))];
        block = new byte[block.Length - block.Length % bytesPerSample];
        var written = 0L;
        while (written < sampleCount)
        {
            var want = (int)Math.Min(block.Length, (sampleCount - written) * bytesPerSample);
            var got = br.Read(block, 0, want);
            if (got == 0) break; // truncated file: keep what we have, zero the rest
            got -= got % bytesPerSample;
            if (got == 0) break;
            Decode(block.AsSpan(0, got), samples.AsSpan((int)written), fmt, bitsPerSample);
            written += got / bytesPerSample;
        }
        return samples;
    }

    private static int FormatBytesPerSample(ushort fmt, ushort bitsPerSample) => (fmt, bitsPerSample) switch
    {
        (FormatIeeeFloat, 32) => 4,
        (FormatIeeeFloat, 64) => 8,
        (FormatPcm, 8)        => 1,
        (FormatPcm, 16)       => 2,
        (FormatPcm, 24)       => 3,
        (FormatPcm, 32)       => 4,
        _ => throw new InvalidDataException(
            $"Unsupported WAV format: fmt={fmt}, bitsPerSample={bitsPerSample}. " +
            "Supported: PCM 8/16/24/32-bit and IEEE_FLOAT 32/64-bit (plain or WAVE_FORMAT_EXTENSIBLE). " +
            "Use JuceAudioDecoder for other formats."),
    };

    private static void Decode(ReadOnlySpan<byte> src, Span<float> dst, ushort fmt, ushort bitsPerSample)
    {
        switch (fmt, bitsPerSample)
        {
            case (FormatIeeeFloat, 32):
                for (var i = 0; i < dst.Length && (i + 1) * 4 <= src.Length; i++)
                    dst[i] = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(src.Slice(i * 4, 4)));
                break;
            case (FormatIeeeFloat, 64):
                for (var i = 0; i < dst.Length && (i + 1) * 8 <= src.Length; i++)
                    dst[i] = (float)BitConverter.Int64BitsToDouble(BinaryPrimitives.ReadInt64LittleEndian(src.Slice(i * 8, 8)));
                break;
            case (FormatPcm, 8):
                // 8-bit WAV PCM is UNSIGNED with a 128 offset — the one integer
                // depth that is not two's complement.
                for (var i = 0; i < dst.Length && i < src.Length; i++)
                    dst[i] = (src[i] - 128) / 128f;
                break;
            case (FormatPcm, 16):
                for (var i = 0; i < dst.Length && (i + 1) * 2 <= src.Length; i++)
                    dst[i] = BinaryPrimitives.ReadInt16LittleEndian(src.Slice(i * 2, 2)) / 32768f;
                break;
            case (FormatPcm, 24):
                for (var i = 0; i < dst.Length && (i + 1) * 3 <= src.Length; i++)
                {
                    var o = i * 3;
                    var v = src[o] | (src[o + 1] << 8) | (src[o + 2] << 16);
                    if ((v & 0x800000) != 0) v |= unchecked((int)0xFF000000); // sign extend
                    dst[i] = v / 8388608f;
                }
                break;
            case (FormatPcm, 32):
                for (var i = 0; i < dst.Length && (i + 1) * 4 <= src.Length; i++)
                    dst[i] = BinaryPrimitives.ReadInt32LittleEndian(src.Slice(i * 4, 4)) / 2147483648f;
                break;
            default:
                throw new InvalidDataException($"Unsupported WAV format: fmt={fmt}, bitsPerSample={bitsPerSample}.");
        }
    }

    /// <summary>Write an interleaved float32 buffer to a 32-bit float WAV.</summary>
    public static void WriteFloat32(string path, AudioBuffer buffer)
    {
        using var fs = File.Create(path);
        WriteFloat32(fs, buffer);
    }

    /// <summary>
    /// Write an interleaved float32 buffer to <paramref name="stream"/> as a
    /// 32-bit IEEE float WAV (<c>fmt </c> + <c>fact</c> + <c>data</c>). The stream
    /// is left open. Throws <see cref="InvalidOperationException"/> when the
    /// sample data would overflow RIFF's unsigned 32-bit chunk size.
    /// <para>A thin front end over <see cref="WavWriter"/>, which owns the only header
    /// implementation in this assembly — so the stream must be WRITABLE and SEEKABLE
    /// (the RIFF and data sizes are patched once the payload is down).</para>
    /// </summary>
    /// <param name="stream">Destination; writable and seekable, left open.</param>
    /// <param name="buffer">Interleaved float32 samples plus their rate and channel count.</param>
    /// <exception cref="InvalidOperationException">The data would overflow a RIFF chunk size.</exception>
    public static void WriteFloat32(Stream stream, AudioBuffer buffer)
    {
        // ONE header implementation. This method used to write the whole
        // RIFF/fmt /fact/data header itself — fifty lines of independently-written field
        // code beside WavWriter's, with independently-derived size arithmetic (two
        // different overflow bounds, uint.MaxValue-44 here against uint.MaxValue-64
        // there). They agreed, and nothing kept them agreeing. The bytes on disk are
        // unchanged: WavWriter writes the same chunks in the same order and patches the
        // two sizes on Dispose.
        //
        // The overflow CHECK stays here, because this method's documented contract is an
        // InvalidOperationException at this exact bound and WavWriter's is a
        // NotSupportedException at a stricter one.
        const int bytesPerSample = 4;
        const long headerAfterRiffSize = 4L      // "WAVE"
                                       + 8 + 16  // "fmt " + PCMWAVEFORMAT
                                       + 8 + 4   // "fact" + dwSampleLength
                                       + 8;      // "data" header
        long dataSize = (long)buffer.Interleaved.Length * bytesPerSample;
        // RIFF chunk sizes are unsigned 32-bit. The header carries everything
        // after the size field, so the strict upper bound is what is left.
        if (dataSize < 0 || dataSize > uint.MaxValue - headerAfterRiffSize)
            throw new InvalidOperationException(
                $"Audio buffer too large to fit in a RIFF/WAV container: {dataSize} bytes " +
                $"(max {uint.MaxValue - headerAfterRiffSize}).");

        using var w = new WavWriter(stream, buffer.SampleRate, buffer.Channels,
                                    WavSampleFormat.Float32, leaveOpen: true);
        w.Write(buffer.Interleaved);
        // No RIFF pad byte is ever required: the payload is a whole number of 4-byte
        // samples, so it is always even.
    }
}
