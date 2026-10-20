using System.Buffers.Binary;
using JuceSharp.Interop;
using Ww2Ogg.Core;
using Xunit;

namespace JuceSharp.Tests.Wem;

/// <summary>
/// The native .wem decoder (ABI v42, DotCharter #426) against the managed path it
/// replaces (Ww2Ogg.Core + NVorbis), on .wem files BUILT at test time from the
/// committed synthetic Ogg fixtures (<see cref="WemFixtureWriter" />): every
/// layout and codebook variant the managed list covers, plus malformed input.
/// The owner's real library is <see cref="WemCorpusTests" />.
/// </summary>
public sealed class WemDecoderTests
{
    private readonly ITestOutputHelper _out;

    public WemDecoderTests(ITestOutputHelper output) => _out = output;

    internal static byte[] Fixture(string name)
        => File.ReadAllBytes(AbiV37Tests.FindRepoFile("tests/JuceSharp.Tests/Fixtures/wem/" + name));

    private static WemFixtureWriter.OggVorbis Ogg(string name) => WemFixtureWriter.ReadOgg(Fixture(name));

    /// <summary>The Rocksmith shape (mod packets, no granules) with inline codebooks.</summary>
    internal static byte[] RocksmithShapedWem(string name = "stereo48k.ogg")
        => WemFixtureWriter.Build(Ogg(name), WemFixtureWriter.Layout.EmbeddedMod, inlineCodebooks: true);

    // Max |native - managed| after clipping native like NVorbis does. Two float
    // Vorbis implementations (libvorbis vs NVorbis) agree to a few ULP of 1.0.
    private const double FloatTolerance = 1e-5;

    public static TheoryData<string, string> Layouts() => new()
    {
        { "stereo48k.ogg", nameof(WemFixtureWriter.Layout.EmbeddedMod) },
        { "stereo48k.ogg", nameof(WemFixtureWriter.Layout.EmbeddedStandard) },
        { "stereo48k.ogg", nameof(WemFixtureWriter.Layout.GranuleChunk) },
        { "stereo48k.ogg", nameof(WemFixtureWriter.Layout.HeaderTriad) },
        { "mono44k.ogg", nameof(WemFixtureWriter.Layout.EmbeddedMod) },
        { "mono44k.ogg", nameof(WemFixtureWriter.Layout.EmbeddedStandard) },
    };

    /// <summary>Inline codebooks, each packet layout: same length, same alignment,
    /// same samples as Ww2Ogg.Core + NVorbis, and the same 16-bit WAV.</summary>
    [NativeAvailableTheory]
    [MemberData(nameof(Layouts))]
    public void InlineCodebookWem_DecodesLikeTheManagedPath(string fixture, string layoutName)
    {
        var layout = Enum.Parse<WemFixtureWriter.Layout>(layoutName);
        var ogg = Ogg(fixture);
        var wem = WemFixtureWriter.Build(ogg, layout, inlineCodebooks: true);

        var native = WemReference.DecodeNative(wem);
        var managed = WemReference.DecodeManaged(wem);
        var cmp = WemReference.Compare(native, managed);
        _out.WriteLine($"{fixture} {layout}: native {cmp.NativeFrames} managed {cmp.ManagedFrames} " +
                       $"maxAbsDiff {cmp.MaxAbsDiff:E3} pcm16 mismatches {cmp.Pcm16Mismatches} (max {cmp.MaxPcm16Diff} LSB)");

        Assert.Equal(managed.Channels, native.Channels);
        Assert.Equal(managed.SampleRate, native.SampleRate);
        Assert.Equal(cmp.ManagedFrames, cmp.NativeFrames);
        Assert.Equal(ogg.LastGranule, cmp.NativeFrames);
        Assert.True(cmp.MaxAbsDiff <= FloatTolerance, $"max |native - managed| = {cmp.MaxAbsDiff:E3}");
        Assert.True(cmp.MaxPcm16Diff <= 1, $"16-bit samples differ by {cmp.MaxPcm16Diff} LSB");

        using var d = JuceWemDecoder.OpenMemory(wem);
        // AUTO walks aoTuV and default first; both must FAIL on inline books.
        var expected = layout == WemFixtureWriter.Layout.HeaderTriad ? JuceWemVariant.HeaderTriad : JuceWemVariant.Inline;
        Assert.Equal(expected, d.Info.Variant);
        Assert.Equal(layout == WemFixtureWriter.Layout.EmbeddedMod, d.Info.ModPackets);
        Assert.Equal(layout is WemFixtureWriter.Layout.GranuleChunk or WemFixtureWriter.Layout.HeaderTriad,
                     d.Info.GranulePresent);
    }

    /// <summary>The same audio, decoded from the ORIGINAL Ogg by NVorbis: the .wem
    /// round trip neither moves nor stretches anything.</summary>
    [NativeAvailableFact]
    public void TheWemRoundTrip_MatchesTheOriginalOgg()
    {
        var original = WemReference.DecodeOgg(Fixture("stereo48k.ogg"));
        var native = WemReference.DecodeNative(RocksmithShapedWem());
        var cmp = WemReference.Compare(native, original);
        _out.WriteLine($"vs original Ogg: maxAbsDiff {cmp.MaxAbsDiff:E3}, pcm16 mismatches {cmp.Pcm16Mismatches}");
        Assert.Equal(original.Frames, native.Frames);
        Assert.True(cmp.MaxAbsDiff <= FloatTolerance, $"max |native - original| = {cmp.MaxAbsDiff:E3}");
    }

    /// <summary>
    /// The packed-library variants: the fixture's codebooks are looked up in
    /// ww2ogg's libraries (the same bytes Ww2Ogg.Core embeds) and the .wem stores
    /// 10-bit ids, which is what a real Wwise file does.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(false)]
    [InlineData(true)]
    public void LibraryCodebookWem_DecodesLikeTheManagedPath(bool aoTuV)
    {
        var ogg = Ogg("stereo48k.ogg");
        var name = aoTuV ? "packed_codebooks_aoTuV_603.bin" : "packed_codebooks.bin";
        using var stream = typeof(CodebookLibrary).Assembly.GetManifestResourceStream("Ww2Ogg.Core.Internal.Codebooks." + name);
        Assert.NotNull(stream);
        var library = new byte[stream!.Length];
        stream.ReadExactly(library);

        var ids = WemFixtureWriter.MatchLibrary(ogg, library);
        if (ids is null)
        {
            Assert.Skip($"the fixture's codebooks are not all in {name}");
            return;
        }

        var wem = WemFixtureWriter.Build(ogg, WemFixtureWriter.Layout.EmbeddedMod, inlineCodebooks: false, ids);
        var variant = aoTuV ? JuceWemVariant.AoTuV : JuceWemVariant.Default;
        var native = WemReference.DecodeNative(wem);
        var managed = WemReference.DecodeManaged(wem, variant);
        var cmp = WemReference.Compare(native, managed);
        _out.WriteLine($"{name}: maxAbsDiff {cmp.MaxAbsDiff:E3}");
        Assert.Equal(cmp.ManagedFrames, cmp.NativeFrames);
        Assert.True(cmp.MaxAbsDiff <= FloatTolerance);

        using var d = JuceWemDecoder.OpenMemory(wem);
        Assert.Equal(variant, d.Info.Variant);
    }

    /// <summary>An explicit variant is the only one tried: the matching one decodes,
    /// a wrong one fails cleanly with Decode instead of falling through.</summary>
    [NativeAvailableFact]
    public void AnExplicitVariant_IsHonoured_AndAWrongOneFailsCleanly()
    {
        var wem = RocksmithShapedWem();
        using (var d = JuceWemDecoder.OpenMemory(wem, JuceWemVariant.Inline))
            Assert.Equal(JuceWemVariant.Inline, d.Info.Variant);

        // The aoTuV / default libraries do not contain the fixture's inline books'
        // ids: the setup bits read as ids fail the rebuild.
        var ex = Assert.Throws<JuceAudioException>(() => JuceWemDecoder.OpenMemory(wem, JuceWemVariant.AoTuV));
        Assert.Equal(JuceAudioStatus.Decode, ex.Status);
    }

    /// <summary>Seeking is bit-exact with a continuous decode, at packet edges, inside
    /// packets, at 0 and at the end.</summary>
    [NativeAvailableFact]
    public void Seek_IsBitExactWithAContinuousDecode()
    {
        var wem = RocksmithShapedWem();
        var full = WemReference.DecodeNative(wem);
        using var d = JuceWemDecoder.OpenMemory(wem);
        var ch = d.Info.Channels;
        var rng = new Random(426);
        var positions = new List<long> { 0, 1, 255, 256, 1024, d.Info.Frames - 1, d.Info.Frames };
        for (var i = 0; i < 40; i++) positions.Add(rng.NextInt64(0, d.Info.Frames));

        var buf = new float[777 * ch];
        foreach (var pos in positions)
        {
            d.Seek(pos);
            var got = d.Read(buf);
            Assert.Equal((int)Math.Min(777, d.Info.Frames - pos), got);
            for (var i = 0; i < got * ch; i++)
                Assert.True(full.Interleaved[pos * ch + i] == buf[i], $"seek {pos}: sample {i} differs");
        }
    }

    /// <summary>The 16-bit read is DotCharter's WAV formula applied to the float read.</summary>
    [NativeAvailableFact]
    public void ReadS16_IsTheWavFormulaOfTheFloatRead()
    {
        var wem = RocksmithShapedWem();
        var floats = WemReference.DecodeNative(wem);
        using var d = JuceWemDecoder.OpenMemory(wem);
        var pcm = new short[floats.Interleaved.Length];
        var at = 0;
        while (at < pcm.Length)
        {
            var got = d.Read(pcm.AsSpan(at, Math.Min(pcm.Length - at, 1000 * d.Info.Channels)));
            if (got == 0) break;
            at += got * d.Info.Channels;
        }
        Assert.Equal(pcm.Length, at);
        for (var i = 0; i < pcm.Length; i++)
            Assert.Equal(WemReference.ToPcm16(floats.Interleaved[i]), pcm[i]);
    }

    /// <summary>dca_decode_audio_file and the file opener know .wem too.</summary>
    [NativeAvailableFact]
    public void TheOfflineDecoderAndOpenFile_AcceptAWem()
    {
        var wem = RocksmithShapedWem();
        var native = WemReference.DecodeNative(wem);
        var path = Path.Combine(Path.GetTempPath(), $"jucesharp-wem-{Guid.NewGuid():N}.wem");
        File.WriteAllBytes(path, wem);
        try
        {
            var viaFile = JuceWemDecoder.DecodeFile(path, out var info);
            Assert.Equal(native.Interleaved, viaFile);
            Assert.Equal(native.Frames, info.Frames);

            var decoded = JuceSharp.AudioIo.JuceAudioDecoder.Decode(path);
            Assert.Equal(native.Channels, decoded.Channels);
            Assert.Equal(native.SampleRate, decoded.SampleRate);
            Assert.Equal(native.Interleaved, decoded.Interleaved);
        }
        finally { File.Delete(path); }
    }

    // ---- malformed input: fails cleanly, never crashes --------------------------

    [NativeAvailableFact]
    public void Garbage_WrongCodec_AndEmptyInput_FailCleanly()
    {
        Assert.Throws<ArgumentException>(() => JuceWemDecoder.OpenMemory(ReadOnlySpan<byte>.Empty));

        var rng = new Random(7);
        var garbage = new byte[4096];
        rng.NextBytes(garbage);
        AssertDecodeError(garbage);

        // A real RIFF WAVE whose codec is PCM (1), not Wwise Vorbis (0xFFFF).
        var wem = RocksmithShapedWem();
        var pcmCodec = (byte[])wem.Clone();
        var fmt = IndexOf(pcmCodec, "fmt "u8) + 8;
        BinaryPrimitives.WriteUInt16LittleEndian(pcmCodec.AsSpan(fmt), 1);
        AssertDecodeError(pcmCodec);

        // Not RIFF at all.
        var notRiff = (byte[])wem.Clone();
        notRiff[0] = (byte)'X';
        AssertDecodeError(notRiff);

        var missing = Path.Combine(Path.GetTempPath(), $"jucesharp-missing-{Guid.NewGuid():N}.wem");
        var ex = Assert.Throws<JuceAudioException>(() => JuceWemDecoder.OpenFile(missing));
        Assert.Equal(JuceAudioStatus.Decode, ex.Status);
    }

    /// <summary>Every truncation point of a valid file either opens (and then reads
    /// without crashing) or fails with Decode — never anything else.</summary>
    [NativeAvailableFact]
    public void EveryTruncation_FailsCleanlyOrDecodes()
    {
        var wem = RocksmithShapedWem("mono44k.ogg");
        var opened = 0;
        var cuts = new List<int>();
        for (var n = 1; n < 200; n++) cuts.Add(n);
        for (var n = 200; n < wem.Length; n += 97) cuts.Add(n);
        foreach (var n in cuts)
        {
            var cut = wem.AsSpan(0, n).ToArray();
            try
            {
                using var d = JuceWemDecoder.OpenMemory(cut);
                opened++;
            }
            catch (JuceAudioException ex) { Assert.Equal(JuceAudioStatus.Decode, ex.Status); }
        }
        // The RIFF size says the file is longer, so no truncation opens.
        Assert.Equal(0, opened);

        // Truncate AND patch the sizes so the header is consistent: the cut lands
        // inside the audio packets, which must be rejected as "page truncated".
        var patched = wem.AsSpan(0, wem.Length - 50).ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(patched.AsSpan(4), (uint)(patched.Length - 8));
        var data = IndexOf(patched, "data"u8);
        var dataSize = BinaryPrimitives.ReadUInt32LittleEndian(patched.AsSpan(data + 4));
        BinaryPrimitives.WriteUInt32LittleEndian(patched.AsSpan(data + 4), dataSize - 50);
        AssertDecodeError(patched);
    }

    /// <summary>Bit-flipped audio packets: opening succeeds (the headers are intact)
    /// and reading either decodes or reports Decode — never a crash.</summary>
    [NativeAvailableFact]
    public void CorruptAudioPackets_NeverCrash()
    {
        var wem = RocksmithShapedWem();
        var rng = new Random(426);
        var data = IndexOf(wem, "data"u8) + 8;
        for (var trial = 0; trial < 20; trial++)
        {
            var bad = (byte[])wem.Clone();
            for (var i = 0; i < 64; i++)
            {
                var at = rng.Next(data + 3000, bad.Length);
                bad[at] ^= (byte)(1 << rng.Next(8));
            }
            try
            {
                using var d = JuceWemDecoder.OpenMemory(bad);
                var buf = new float[4096 * d.Info.Channels];
                try { while (d.Read(buf) > 0) { } }
                catch (JuceAudioException ex) { Assert.Equal(JuceAudioStatus.Decode, ex.Status); }
            }
            catch (JuceAudioException ex) { Assert.Equal(JuceAudioStatus.Decode, ex.Status); }
        }
    }

    [NativeAvailableFact]
    public unsafe void TheRawExports_RejectNullsAndUnknownVariants()
    {
        var wem = RocksmithShapedWem();
        fixed (byte* p = wem)
        {
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.WemOpenMemory(p, (nuint)wem.Length, 99, out var h1));
            Assert.Equal(nint.Zero, h1);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.WemOpenMemory(p, 0, 0, out var h2));
            Assert.Equal(nint.Zero, h2);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.WemOpenMemory(null, 10, 0, out _));
        }
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.WemGetInfo(nint.Zero, out _));
        float f = 0;
        short s = 0;
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.WemReadF32(nint.Zero, &f, 1, out _));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.WemReadS16(nint.Zero, &s, 1, out _));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.WemSeek(nint.Zero, 0));
        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.WemDestroy(nint.Zero));
    }

    [NativeAvailableFact]
    public void ADisposedDecoder_Throws_AndDisposeTwiceIsSafe()
    {
        var d = JuceWemDecoder.OpenMemory(RocksmithShapedWem());
        d.Dispose();
        d.Dispose();
        Assert.True(d.IsDisposed);
        Assert.Throws<ObjectDisposedException>(() => d.Read(new float[16]));
        Assert.Throws<ObjectDisposedException>(() => d.Seek(0));
    }

    private static void AssertDecodeError(byte[] bytes)
    {
        var ex = Assert.Throws<JuceAudioException>(() => JuceWemDecoder.OpenMemory(bytes));
        Assert.Equal(JuceAudioStatus.Decode, ex.Status);
    }

    private static int IndexOf(byte[] haystack, ReadOnlySpan<byte> needle)
    {
        var i = haystack.AsSpan().IndexOf(needle);
        Assert.True(i >= 0);
        return i;
    }
}

/// <summary>A .wem opens as an ENGINE source (v42): the format manager knows it, so
/// the file and memory openers go through the ordinary transport and preload.</summary>
[Collection("NullDevice")]
public sealed class WemEngineSourceTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();

    public void Dispose() => _scope.Dispose();

    [NativeAvailableFact]
    public void AWem_OpensAsAFileAndAMemorySource_WithItsLength()
    {
        var wem = WemDecoderTests.RocksmithShapedWem();
        using var probe = JuceWemDecoder.OpenMemory(wem);
        var seconds = probe.Info.Frames / (double)probe.Info.SampleRate;

        var path = Path.Combine(Path.GetTempPath(), $"jucesharp-wem-src-{Guid.NewGuid():N}.wem");
        File.WriteAllBytes(path, wem);
        try
        {
            using var engine = NullDevice.CreateEngine();
            using (var file = engine.OpenFile(path))
                Assert.Equal(seconds, file.Length.TotalSeconds, 3);
            using (var memory = engine.OpenMemory(wem, "wem"))
                Assert.Equal(seconds, memory.Length.TotalSeconds, 3);
            using (var sniffed = engine.OpenMemory(wem))
                Assert.Equal(seconds, sniffed.Length.TotalSeconds, 3);
        }
        finally { File.Delete(path); }
    }
}
