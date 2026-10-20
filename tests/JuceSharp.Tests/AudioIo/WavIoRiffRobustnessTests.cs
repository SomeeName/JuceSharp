using System.Text;
using JuceSharp.AudioIo;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

/// <summary>
/// D2. The RIFF reader shipped three faults that every one of them turns a
/// perfectly ordinary DAW export into an exception or silent garbage:
/// <list type="bullet">
///   <item>it never skipped the <b>pad byte</b> that follows an odd-sized chunk,
///     so a <c>LIST</c>/<c>bext</c>/<c>iXML</c> of odd length desynchronised the
///     walk and the file read as "missing 'data' chunk";</item>
///   <item><c>ReadBytes((int)chunkSize)</c> turned the <c>0xFFFFFFFF</c>
///     streaming placeholder — and any data chunk past 2 GB — into a NEGATIVE
///     count;</item>
///   <item><c>WAVE_FORMAT_EXTENSIBLE</c> (0xFFFE), which is what a DAW emits for
///     every &gt;2-channel, every 24-bit and most 32-bit-float export, was
///     rejected outright, as was 24-bit PCM in any form.</item>
/// </list>
/// All fixtures are hand-built byte-for-byte so each fault has its own case.
/// </summary>
public sealed class WavIoRiffRobustnessTests
{
    private static readonly Guid KsdataformatSubtypePcm =
        new("00000001-0000-0010-8000-00aa00389b71");

    private static readonly Guid KsdataformatSubtypeIeeeFloat =
        new("00000003-0000-0010-8000-00aa00389b71");

    private static void WriteFourCc(BinaryWriter bw, string cc) => bw.Write(Encoding.ASCII.GetBytes(cc));

    /// <summary>Writes a plain (non-extensible) fmt chunk.</summary>
    private static void WriteFmt(BinaryWriter bw, ushort tag, ushort channels, uint sampleRate, ushort bits)
    {
        WriteFourCc(bw, "fmt ");
        bw.Write(16);
        bw.Write(tag);
        bw.Write(channels);
        bw.Write(sampleRate);
        bw.Write(sampleRate * channels * (uint)(bits / 8));
        bw.Write((ushort)(channels * bits / 8));
        bw.Write(bits);
    }

    /// <summary>Writes a 40-byte WAVE_FORMAT_EXTENSIBLE fmt chunk.</summary>
    private static void WriteFmtExtensible(BinaryWriter bw, Guid subFormat, ushort channels,
                                           uint sampleRate, ushort bits)
    {
        WriteFourCc(bw, "fmt ");
        bw.Write(40);
        bw.Write((ushort)0xFFFE);
        bw.Write(channels);
        bw.Write(sampleRate);
        bw.Write(sampleRate * channels * (uint)(bits / 8));
        bw.Write((ushort)(channels * bits / 8));
        bw.Write(bits);
        bw.Write((ushort)22);   // cbSize
        bw.Write(bits);         // valid bits per sample
        bw.Write(3u);           // channel mask (FL | FR)
        bw.Write(subFormat.ToByteArray());
    }

    private static MemoryStream BuildWav(Action<BinaryWriter> writeChunks)
    {
        var ms = new MemoryStream();
        using (var bw = new BinaryWriter(ms, Encoding.ASCII, leaveOpen: true))
        {
            WriteFourCc(bw, "RIFF");
            bw.Write(0u); // patched below
            WriteFourCc(bw, "WAVE");
            writeChunks(bw);
        }
        var size = (uint)(ms.Length - 8);
        var buf = ms.GetBuffer();
        BitConverter.TryWriteBytes(buf.AsSpan(4, 4), size);
        ms.Position = 0;
        return ms;
    }

    /// <summary>
    /// THE PAD BYTE. A 5-byte LIST is padded to 6; a reader that seeks only the
    /// declared 5 lands one byte early, reads "ATA\0" + garbage as the next chunk
    /// id and walks off the end.
    /// </summary>
    [Fact]
    public void OddSizedListChunkBeforeData_IsSkippedIncludingItsPadByte()
    {
        using var ms = BuildWav(bw =>
        {
            WriteFourCc(bw, "LIST");
            bw.Write(5);                       // ODD
            bw.Write(Encoding.ASCII.GetBytes("INFOx"));
            bw.Write((byte)0);                 // RIFF pad byte, NOT counted in the size

            WriteFmt(bw, 1, 1, 8000, 16);
            WriteFourCc(bw, "data");
            bw.Write(4);
            bw.Write((short)16384);
            bw.Write((short)-16384);
        });

        var buf = WavIo.Read(ms);

        Assert.Equal(8000, buf.SampleRate);
        Assert.Equal(2, buf.FrameCount);
        Assert.Equal(0.5f, buf.Interleaved[0], 4);
        Assert.Equal(-0.5f, buf.Interleaved[1], 4);
    }

    /// <summary>An odd 'data' chunk is legal too and must not over-read.</summary>
    [Fact]
    public void OddSizedDataChunk_ReadsTheWholeBody()
    {
        using var ms = BuildWav(bw =>
        {
            WriteFmt(bw, 1, 1, 8000, 8);
            WriteFourCc(bw, "data");
            bw.Write(3);                       // ODD
            bw.Write(new byte[] { 128, 255, 0 });
            bw.Write((byte)0);                 // pad
        });

        var buf = WavIo.Read(ms);

        Assert.Equal(3, buf.FrameCount);
        Assert.Equal(0f, buf.Interleaved[0], 4);
        Assert.Equal(127f / 128f, buf.Interleaved[1], 4);
        Assert.Equal(-1f, buf.Interleaved[2], 4);
    }

    /// <summary>
    /// 0xFFFFFFFF is the "length unknown" placeholder a streaming writer leaves
    /// behind. Cast to int it is −1, and <c>ReadBytes(-1)</c> throws.
    /// </summary>
    [Fact]
    public void StreamingPlaceholderDataSize_IsClampedToWhatTheStreamActuallyHolds()
    {
        using var ms = BuildWav(bw =>
        {
            WriteFmt(bw, 1, 1, 8000, 16);
            WriteFourCc(bw, "data");
            bw.Write(uint.MaxValue);           // "I do not know yet"
            bw.Write((short)16384);
            bw.Write((short)-16384);
            bw.Write((short)8192);
        });

        var buf = WavIo.Read(ms);

        Assert.Equal(3, buf.FrameCount);
        Assert.Equal(0.5f, buf.Interleaved[0], 4);
        Assert.Equal(0.25f, buf.Interleaved[2], 4);
    }

    /// <summary>A declared size larger than the file (a truncated download) must
    /// not throw either — it clamps the same way.</summary>
    [Fact]
    public void DeclaredDataSizeLargerThanTheFile_IsClamped()
    {
        using var ms = BuildWav(bw =>
        {
            WriteFmt(bw, 1, 1, 8000, 16);
            WriteFourCc(bw, "data");
            bw.Write(1_000_000);
            bw.Write((short)16384);
        });

        var buf = WavIo.Read(ms);
        Assert.Equal(1, buf.FrameCount);
        Assert.Equal(0.5f, buf.Interleaved[0], 4);
    }

    /// <summary>The shape every 32-bit-float DAW export actually has.</summary>
    [Fact]
    public void Extensible_Float32_IsReadThroughItsSubFormatGuid()
    {
        float[] expected = [0.25f, -0.75f, 1f, -1f];
        using var ms = BuildWav(bw =>
        {
            WriteFmtExtensible(bw, KsdataformatSubtypeIeeeFloat, 2, 48000, 32);
            WriteFourCc(bw, "data");
            bw.Write(expected.Length * 4);
            foreach (var f in expected) bw.Write(f);
        });

        var buf = WavIo.Read(ms);

        Assert.Equal(48000, buf.SampleRate);
        Assert.Equal(2, buf.Channels);
        Assert.Equal(2, buf.FrameCount);
        for (var i = 0; i < expected.Length; i++) Assert.Equal(expected[i], buf.Interleaved[i], 6);
    }

    /// <summary>24-bit PCM, in the extensible container a DAW writes it in.</summary>
    [Fact]
    public void Extensible_Pcm24_IsDecoded()
    {
        // +0.5 => 0x400000, -0.5 => 0xC00000, 0 => 0x000000
        byte[] body = [0x00, 0x00, 0x40, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00];
        using var ms = BuildWav(bw =>
        {
            WriteFmtExtensible(bw, KsdataformatSubtypePcm, 1, 44100, 24);
            WriteFourCc(bw, "data");
            bw.Write(body.Length);
            bw.Write(body);
            bw.Write((byte)0); // odd body => pad
        });

        var buf = WavIo.Read(ms);

        Assert.Equal(44100, buf.SampleRate);
        Assert.Equal(3, buf.FrameCount);
        Assert.Equal(0.5f, buf.Interleaved[0], 6);
        Assert.Equal(-0.5f, buf.Interleaved[1], 6);
        Assert.Equal(0f, buf.Interleaved[2], 6);
    }

    /// <summary>Plain (non-extensible) 24-bit PCM is just as common from CLI tools.</summary>
    [Fact]
    public void Plain_Pcm24_IsDecoded()
    {
        byte[] body = [0xFF, 0xFF, 0x7F];  // +full scale - 1 lsb
        using var ms = BuildWav(bw =>
        {
            WriteFmt(bw, 1, 1, 44100, 24);
            WriteFourCc(bw, "data");
            bw.Write(body.Length);
            bw.Write(body);
            bw.Write((byte)0);
        });

        var buf = WavIo.Read(ms);
        Assert.Equal(1, buf.FrameCount);
        Assert.Equal(1f, buf.Interleaved[0], 5);
    }

    /// <summary>32-bit integer PCM (fmt=1, bps=32) is NOT float and must not be
    /// bit-reinterpreted as one.</summary>
    [Fact]
    public void Plain_Pcm32_IsDecodedAsIntegerNotFloat()
    {
        using var ms = BuildWav(bw =>
        {
            WriteFmt(bw, 1, 1, 44100, 32);
            WriteFourCc(bw, "data");
            bw.Write(4);
            bw.Write(1 << 30); // +0.5
        });

        var buf = WavIo.Read(ms);
        Assert.Equal(0.5f, buf.Interleaved[0], 6);
    }

    /// <summary>
    /// A float32 WAV must carry a 'fact' chunk (RIFF requires one for every
    /// non-PCM format tag) — and the reader must still round-trip it.
    /// </summary>
    [Fact]
    public void WriteFloat32_EmitsAFactChunk_AndStillRoundTrips()
    {
        var data = new float[64];
        for (var i = 0; i < data.Length; i++) data[i] = MathF.Sin(i * 0.1f) * 0.5f;

        using var ms = new MemoryStream();
        WavIo.WriteFloat32(ms, new AudioBuffer(data, 44100, 2));
        var bytes = ms.ToArray();

        var factIdx = IndexOf(bytes, "fact");
        Assert.True(factIdx > 0, "WriteFloat32 must emit a 'fact' chunk for IEEE_FLOAT.");
        Assert.Equal(4, BitConverter.ToInt32(bytes, factIdx + 4));
        Assert.Equal((uint)(data.Length / 2), BitConverter.ToUInt32(bytes, factIdx + 8));
        // The RIFF size field must count everything after it, 'fact' included.
        Assert.Equal((uint)(bytes.Length - 8), BitConverter.ToUInt32(bytes, 4));

        ms.Position = 0;
        var rt = WavIo.Read(ms);
        Assert.Equal(2, rt.Channels);
        Assert.Equal(data.Length / 2, rt.FrameCount);
        for (var i = 0; i < data.Length; i++) Assert.Equal(data[i], rt.Interleaved[i], 6);
    }

    private static int IndexOf(byte[] haystack, string needle)
    {
        var n = Encoding.ASCII.GetBytes(needle);
        for (var i = 0; i + n.Length <= haystack.Length; i++)
        {
            var ok = true;
            for (var j = 0; j < n.Length; j++)
                if (haystack[i + j] != n[j]) { ok = false; break; }
            if (ok) return i;
        }
        return -1;
    }
}
