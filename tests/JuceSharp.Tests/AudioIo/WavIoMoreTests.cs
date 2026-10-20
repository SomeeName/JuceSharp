using JuceSharp.AudioIo;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

public sealed class WavIoMoreTests
{
    [Fact]
    public void Read_Pcm16_DecodesToFloat()
    {
        // Hand-craft a tiny mono 8 kHz 16-bit PCM WAV with samples [0, 16384, -16384].
        // 16384 / 32768 = 0.5
        using var ms = new MemoryStream();
        using (var bw = new BinaryWriter(ms, System.Text.Encoding.ASCII, leaveOpen: true))
        {
            short[] samples = [0, 16384, -16384];
            var dataSize = samples.Length * 2;
            bw.Write("RIFF".ToCharArray());
            bw.Write(36 + dataSize);
            bw.Write("WAVE".ToCharArray());
            bw.Write("fmt ".ToCharArray());
            bw.Write(16);
            bw.Write((ushort)1);    // PCM
            bw.Write((ushort)1);    // channels
            bw.Write((uint)8000);   // sr
            bw.Write((uint)(8000 * 2)); // byterate
            bw.Write((ushort)2);    // block align
            bw.Write((ushort)16);   // bps
            bw.Write("data".ToCharArray());
            bw.Write(dataSize);
            foreach (var s in samples) bw.Write(s);
        }
        ms.Position = 0;

        var buf = WavIo.Read(ms);

        Assert.Equal(8000, buf.SampleRate);
        Assert.Equal(1, buf.Channels);
        Assert.Equal(3, buf.FrameCount);
        Assert.Equal(0f, buf.Interleaved[0], 5);
        Assert.Equal(0.5f, buf.Interleaved[1], 4);
        Assert.Equal(-0.5f, buf.Interleaved[2], 4);
    }

    [Fact]
    public void Read_NotRiff_Throws()
    {
        using var ms = new MemoryStream([(byte)'J', (byte)'U', (byte)'N', (byte)'K', 0, 0, 0, 0]);
        Assert.Throws<InvalidDataException>(() => WavIo.Read(ms));
    }

    [Fact]
    public void Read_RiffButNotWave_Throws()
    {
        using var ms = new MemoryStream();
        using (var bw = new BinaryWriter(ms, System.Text.Encoding.ASCII, leaveOpen: true))
        {
            bw.Write("RIFF".ToCharArray());
            bw.Write(36);
            bw.Write("AVI ".ToCharArray());
        }
        ms.Position = 0;
        Assert.Throws<InvalidDataException>(() => WavIo.Read(ms));
    }

    [Fact]
    public void Read_UnsupportedFormat_Throws()
    {
        // IMA ADPCM (0x11) at 4 bits per sample: a real WAV codec this reader
        // does not decode. (24-bit PCM used to be the fixture here and IS
        // supported now — see WavIoRiffRobustnessTests.)
        using var ms = new MemoryStream();
        using (var bw = new BinaryWriter(ms, System.Text.Encoding.ASCII, leaveOpen: true))
        {
            bw.Write("RIFF".ToCharArray());
            bw.Write(36 + 3);
            bw.Write("WAVE".ToCharArray());
            bw.Write("fmt ".ToCharArray());
            bw.Write(16);
            bw.Write((ushort)0x11); // IMA ADPCM
            bw.Write((ushort)1);
            bw.Write((uint)8000);
            bw.Write((uint)(8000 * 3));
            bw.Write((ushort)3);
            bw.Write((ushort)4); // unsupported bps
            bw.Write("data".ToCharArray());
            bw.Write(3);
            bw.Write(new byte[3]);
        }
        ms.Position = 0;
        Assert.Throws<InvalidDataException>(() => WavIo.Read(ms));
    }

    [Fact]
    public void Read_SkipsUnknownChunksBeforeData()
    {
        // RIFF / WAVE / LIST / fmt / data — reader must skip LIST.
        using var ms = new MemoryStream();
        using (var bw = new BinaryWriter(ms, System.Text.Encoding.ASCII, leaveOpen: true))
        {
            short[] samples = [0, 16384];
            var dataSize = samples.Length * 2;
            var listSize = 4;

            bw.Write("RIFF".ToCharArray());
            bw.Write(36 + dataSize + 8 + listSize);
            bw.Write("WAVE".ToCharArray());
            bw.Write("LIST".ToCharArray());
            bw.Write(listSize);
            bw.Write(new byte[listSize]);
            bw.Write("fmt ".ToCharArray());
            bw.Write(16);
            bw.Write((ushort)1);
            bw.Write((ushort)1);
            bw.Write((uint)8000);
            bw.Write((uint)(8000 * 2));
            bw.Write((ushort)2);
            bw.Write((ushort)16);
            bw.Write("data".ToCharArray());
            bw.Write(dataSize);
            foreach (var s in samples) bw.Write(s);
        }
        ms.Position = 0;

        var buf = WavIo.Read(ms);
        Assert.Equal(2, buf.FrameCount);
    }
}
