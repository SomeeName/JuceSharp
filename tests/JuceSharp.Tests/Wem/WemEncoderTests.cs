using System.Buffers.Binary;
using Xunit;

namespace JuceSharp.Tests.Wem;

/// <summary>
/// The Wwise-free .wem encoder (ABI v44, DotCharter #317). Every rule asserted here
/// was measured on 26 Wwise 2021 Vorbis files Rocksmith plays (see
/// docs/agents/wem-encoder.md): the setup header, the fmt/vorb fields and the seek
/// table. Two independent decoders read what it writes: the native one and the
/// managed ww2ogg + NVorbis path the native one replaced.
/// </summary>
public sealed class WemEncoderTests
{
    // The Wwise-form setup packet of every Wwise 2021 Rocksmith .wem measured:
    // 215 bytes, this FNV-1a-64.
    private const int WwiseSetupBytes = 215;
    private const ulong WwiseSetupFnv64 = 0xF0FD447FF724785EUL;

    private const int Rate = JuceWemEncoder.SampleRate;

    private readonly ITestOutputHelper _out;

    public WemEncoderTests(ITestOutputHelper output) => _out = output;

    /// <summary>Ten seconds of stereo that exercises both block sizes: a decaying
    /// pluck every half second (transients force short blocks) over a chord, with
    /// a little noise on the right channel. Deterministic.</summary>
    internal static float[] TestSignal(int seconds = 10)
    {
        // An odd length, so the last block runs past the end and the padding fields
        // (fmt +0x26 / +0x32) are non-zero -- a whole-block length makes them 0.
        var frames = seconds * Rate + 333;
        var x = new float[frames * 2];
        var noise = new Random(317);
        for (var i = 0; i < frames; i++)
        {
            var t = i / (double)Rate;
            var sincePluck = t % 0.5;
            var pluck = Math.Exp(-sincePluck * 9) * Math.Sin(2 * Math.PI * 329.63 * t);
            var chord = 0.25 * (Math.Sin(2 * Math.PI * 110 * t) + Math.Sin(2 * Math.PI * 164.81 * t));
            x[i * 2] = (float)(0.45 * pluck + chord * 0.6);
            x[i * 2 + 1] = (float)(0.35 * pluck + chord * 0.5 + (noise.NextDouble() - 0.5) * 0.02);
        }

        return x;
    }

    internal sealed record Parsed(byte[] Fmt, int DataOffset, int DataSize, byte[] Bytes)
    {
        public uint U32(int at) => BinaryPrimitives.ReadUInt32LittleEndian(Fmt.AsSpan(at));
        public ushort U16(int at) => BinaryPrimitives.ReadUInt16LittleEndian(Fmt.AsSpan(at));
        public int SeekSize => (int)U32(0x28);
        public int AudioOffset => (int)U32(0x2C);
        public ReadOnlySpan<byte> Data => Bytes.AsSpan(DataOffset, DataSize);

        public byte[] SetupPacket()
        {
            var size = BinaryPrimitives.ReadUInt16LittleEndian(Data[SeekSize..]);
            return Data.Slice(SeekSize + 2, size).ToArray();
        }

        /// <summary>(payload offset relative to the setup packet, payload size) of every audio packet.</summary>
        public List<(int Offset, int Size, byte First)> Packets()
        {
            var list = new List<(int, int, byte)>();
            var data = Data;
            for (var p = AudioOffset; p + 2 <= data.Length;)
            {
                var size = BinaryPrimitives.ReadUInt16LittleEndian(data[p..]);
                list.Add((p - SeekSize, size, size > 0 ? data[p + 2] : (byte)0));
                p += 2 + size;
            }

            return list;
        }
    }

    internal static Parsed Parse(byte[] wem)
    {
        Assert.Equal("RIFF"u8.ToArray(), wem[..4]);
        Assert.Equal("WAVE"u8.ToArray(), wem[8..12]);
        Assert.Equal((uint)(wem.Length - 8), BinaryPrimitives.ReadUInt32LittleEndian(wem.AsSpan(4)));
        byte[]? fmt = null;
        int dataOffset = -1, dataSize = -1;
        for (var pos = 12; pos + 8 <= wem.Length;)
        {
            var id = System.Text.Encoding.ASCII.GetString(wem, pos, 4);
            var size = (int)BinaryPrimitives.ReadUInt32LittleEndian(wem.AsSpan(pos + 4));
            if (id == "fmt ") fmt = wem.AsSpan(pos + 8, size).ToArray();
            if (id == "data") { dataOffset = pos + 8; dataSize = size; }
            pos += 8 + size;
        }

        Assert.NotNull(fmt);
        Assert.True(dataOffset > 0);
        return new Parsed(fmt, dataOffset, dataSize, wem);
    }

    private static ulong Fnv1a64(ReadOnlySpan<byte> b)
    {
        var h = 0xCBF29CE484222325UL;
        foreach (var x in b) { h ^= x; h *= 0x100000001B3UL; }
        return h;
    }

    private static double SnrDb(float[] reference, float[] test)
    {
        double sig = 0, err = 0;
        for (var i = 0; i < reference.Length; i++)
        {
            var d = (double)test[i] - reference[i];
            sig += (double)reference[i] * reference[i];
            err += d * d;
        }

        return 10 * Math.Log10(sig / Math.Max(err, 1e-30));
    }

    [NativeAvailableFact]
    public void AnEncodedWem_DecodesFrameExact_AsAWwiseAoTuVModPacketFile()
    {
        var input = TestSignal();
        var wem = JuceWemEncoder.EncodeVorbis(input);
        using var decoder = JuceWemDecoder.OpenMemory(wem);
        var info = decoder.Info;
        _out.WriteLine($"{wem.Length} bytes, {info.PacketCount} packets, frames {info.Frames}, decoded {info.DecodedFrames}");

        Assert.Equal(2, info.Channels);
        Assert.Equal(Rate, info.SampleRate);
        Assert.Equal(JuceWemVariant.AoTuV, info.Variant);   // codebooks named by library index
        Assert.True(info.ModPackets);
        Assert.False(info.GranulePresent);
        Assert.Equal(input.Length / 2, info.Frames);
        Assert.Equal(input.Length / 2, info.HeaderSampleCount);
        Assert.True(info.DecodedFrames >= info.Frames);
    }

    /// <summary>Lossy, so not bit-exact; aligned and close. A shifted or garbled
    /// decode is far below this.</summary>
    [NativeAvailableFact]
    public void TheDecodedAudio_IsTheInput_AlignedAndClose()
    {
        var input = TestSignal();
        var decoded = DecodeMemory(JuceWemEncoder.EncodeVorbis(input), out _);
        Assert.Equal(input.Length, decoded.Length);
        var snr = SnrDb(input, decoded);
        _out.WriteLine($"SNR {snr:F1} dB");
        Assert.True(snr > 20, $"SNR {snr:F1} dB: the decode does not match the input");

        // Aligned: of every lag within +-64 frames, the decode correlates best with
        // the input at zero (the encoder adds no delay, and none is trimmed wrong).
        var best = int.MinValue;
        var bestCorrelation = double.MinValue;
        for (var lag = -64; lag <= 64; lag++)
        {
            double c = 0;
            for (var i = 128; i < input.Length / 2 - 128; i++)
                c += input[i * 2] * (double)decoded[(i + lag) * 2];
            if (c > bestCorrelation) { bestCorrelation = c; best = lag; }
        }

        Assert.Equal(0, best);
    }

    /// <summary>The managed ww2ogg port (Ww2Ogg.Core) is an independent reading of
    /// the Wwise format; it must rebuild the same audio.</summary>
    [NativeAvailableFact]
    public void TheManagedWw2OggPath_DecodesItLikeTheNativeDecoder()
    {
        var wem = JuceWemEncoder.EncodeVorbis(TestSignal(4));
        var managed = WemReference.DecodeOgg(WemReference.ToOgg(wem, JuceWemVariant.AoTuV));
        var native = DecodeMemory(wem, out var info);
        Assert.Equal(info.Frames, managed.Frames);
        var worst = 0.0;
        for (var i = 0; i < native.Length; i++)
            worst = Math.Max(worst, Math.Abs(Math.Clamp(native[i], -1f, 1f) - managed.Interleaved[i]));
        _out.WriteLine($"max |native - ww2ogg/NVorbis| = {worst:G3}");
        Assert.True(worst < 1e-5, $"the two decoders disagree by {worst:G3}");
    }

    [NativeAvailableFact]
    public void TheSetupHeader_IsWwises_ByteForByte()
    {
        var setup = Parse(JuceWemEncoder.EncodeVorbis(TestSignal(2))).SetupPacket();
        Assert.Equal(WwiseSetupBytes, setup.Length);
        Assert.Equal(WwiseSetupFnv64, Fnv1a64(setup));
    }

    [NativeAvailableFact]
    public void TheFmtFields_FollowTheWwiseRules()
    {
        var wem = JuceWemEncoder.EncodeVorbis(TestSignal());
        var f = Parse(wem);
        using var decoder = JuceWemDecoder.OpenMemory(wem);
        var info = decoder.Info;
        var packets = f.Packets();

        Assert.Equal(0x42, f.Fmt.Length);
        Assert.Equal(0xFFFF, f.U16(0x00));
        Assert.Equal(2, f.U16(0x02));
        Assert.Equal((uint)Rate, f.U32(0x04));
        Assert.Equal(0, f.U16(0x0C));                              // block align
        Assert.Equal(0, f.U16(0x0E));                              // bits per sample
        Assert.Equal(0x30, f.U16(0x10));                           // extra size
        Assert.Equal(3u, BinaryPrimitives.ReadUInt32LittleEndian(wem.AsSpan(40)));   // channel mask at offset 40
        Assert.Equal((uint)info.HeaderSampleCount, f.U32(0x18));
        Assert.Equal((uint)(2 + WwiseSetupBytes), f.U32(0x1C));   // loop start: right after the setup packet
        Assert.Equal((uint)(f.DataSize - f.SeekSize), f.U32(0x20)); // loop end: the data end
        Assert.Equal(0, f.U16(0x24));
        Assert.True(info.DecodedFrames > info.HeaderSampleCount, "the fixture must leave end padding, or the +0x26 check is vacuous");
        Assert.Equal((ushort)(info.DecodedFrames - info.HeaderSampleCount), f.U16(0x26));
        Assert.Equal(f.U16(0x26), f.U16(0x32));
        Assert.Equal(f.SeekSize + 2 + WwiseSetupBytes, f.AudioOffset);
        Assert.Equal(packets.Max(p => p.Size), f.U16(0x30));      // max packet size
        Assert.Equal(16080u, f.U32(0x34));
        Assert.Equal(16560u, f.U32(0x38));
        Assert.Equal(0xEC69CB18u, f.U32(0x3C));
        Assert.Equal(8, f.Fmt[0x40]);
        Assert.Equal(11, f.Fmt[0x41]);
        var seconds = info.HeaderSampleCount / (double)Rate;
        Assert.Equal((uint)((f.DataSize - f.SeekSize) / seconds), f.U32(0x08));
    }

    /// <summary>An entry when a packet's END frame is >= 16384 past the previous
    /// entry; (frames, bytes) relative to it, bytes from the setup packet.</summary>
    [NativeAvailableFact]
    public void TheSeekTable_FollowsTheWwiseRule_AndEveryEntryIsAPacketBoundary()
    {
        var f = Parse(JuceWemEncoder.EncodeVorbis(TestSignal()));
        var data = f.Data;
        var expected = new List<(int Frames, int Bytes)>();
        int prevFlag = -1, pos = 0, lastFrames = 0, lastOffset = 0;
        var boundaries = new HashSet<int>();
        foreach (var (offset, size, first) in f.Packets())
        {
            boundaries.Add(offset);
            if (size == 0) continue;
            var flag = first & 1;   // two modes: mode 0 short, mode 1 long
            var frames = prevFlag < 0 ? 0 : (prevFlag == 1 ? 2048 : 256) / 4 + (flag == 1 ? 2048 : 256) / 4;
            prevFlag = flag;
            var end = pos + frames;
            if (end - lastFrames >= 16384)
            {
                expected.Add((end - lastFrames, offset - lastOffset));
                lastFrames = end;
                lastOffset = offset;
            }

            pos = end;
        }

        var actual = new List<(int, int)>();
        for (var i = 0; i < f.SeekSize; i += 4)
            actual.Add((BinaryPrimitives.ReadUInt16LittleEndian(data[i..]), BinaryPrimitives.ReadUInt16LittleEndian(data[(i + 2)..])));
        Assert.NotEmpty(actual);
        Assert.Equal(expected, actual);

        var at = 0;
        foreach (var (_, bytes) in actual)
        {
            at += bytes;
            Assert.Contains(at, boundaries);
        }
    }

    [NativeAvailableFact]
    public void Encoding_IsDeterministic()
        => Assert.Equal(JuceWemEncoder.EncodeVorbis(TestSignal(3)), JuceWemEncoder.EncodeVorbis(TestSignal(3)));

    [NativeAvailableFact]
    public void AVeryShortInput_StillDecodesToItsOwnLength()
    {
        var input = TestSignal(1).AsSpan(0, 2 * 100).ToArray();
        using var decoder = JuceWemDecoder.OpenMemory(JuceWemEncoder.EncodeVorbis(input));
        Assert.Equal(100, decoder.Info.Frames);
    }

    [Theory]
    [InlineData(44100, 2)]
    [InlineData(48000, 1)]
    [InlineData(96000, 2)]
    public void AnythingButFortyEightKiloHertzStereo_IsRefused(int rate, int channels)
        => Assert.Throws<ArgumentException>(() => JuceWemEncoder.EncodeVorbis(new float[channels * 1000], channels, rate));

    [Fact]
    public void EmptyOrPartialFrames_AreRefused()
    {
        Assert.Throws<ArgumentException>(() => JuceWemEncoder.EncodeVorbis(ReadOnlySpan<float>.Empty));
        Assert.Throws<ArgumentException>(() => JuceWemEncoder.EncodeVorbis(new float[3]));
    }

    /// <summary>Real Rocksmith .wem files (the owner's PsarcAudioCache, or
    /// <c>JUCESHARP_WEM_CORPUS_DIR</c>): every 48 kHz stereo Wwise file carries the
    /// setup header the encoder writes, byte for byte.</summary>
    [NativeAvailableFact]
    public void RealRocksmithWems_CarryTheSetupHeaderTheEncoderWrites()
    {
        var files = WemCorpusTests.CorpusFiles(defaultCount: 6);
        if (files.Count == 0)
        {
            Assert.Skip("no .wem corpus on this machine");
            return;
        }

        var ours = Parse(JuceWemEncoder.EncodeVorbis(TestSignal(1))).SetupPacket();
        var compared = 0;
        foreach (var file in files)
        {
            var bytes = File.ReadAllBytes(file);
            Parsed p;
            try { p = Parse(bytes); }
            catch (Exception) { continue; }
            if (p.Fmt.Length != 0x42 || p.U16(0) != 0xFFFF || p.U16(2) != 2 || p.U32(4) != Rate) continue;
            Assert.Equal(ours, p.SetupPacket());
            compared++;
        }

        _out.WriteLine($"{compared} of {files.Count} corpus files compared");
        Assert.True(compared > 0, "the corpus held no 48 kHz stereo Wwise Vorbis file");
    }

    private static float[] DecodeMemory(byte[] wem, out JuceWemInfo info)
    {
        using var decoder = JuceWemDecoder.OpenMemory(wem);
        info = decoder.Info;
        var samples = new float[checked(info.Frames * info.Channels)];
        var at = 0;
        while (at < samples.Length)
        {
            var frames = decoder.Read(samples.AsSpan(at));
            if (frames == 0) break;
            at += frames * info.Channels;
        }

        Assert.Equal(samples.Length, at);
        return samples;
    }
}
