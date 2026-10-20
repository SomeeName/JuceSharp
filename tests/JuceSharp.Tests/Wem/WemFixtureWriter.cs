using System.Buffers.Binary;
using System.Text;

namespace JuceSharp.Tests.Wem;

/// <summary>
/// Builds Wwise RIFF Vorbis (.wem) files from an ordinary Ogg Vorbis file — the
/// inverse of ww2ogg's header rebuild — so the native decoder's variants can be
/// tested on LEGAL audio: the committed fixtures are synthetic tones rendered by
/// ffmpeg's libvorbis (<c>Fixtures/wem/*.ogg</c>, see the README there).
/// <para>
/// Covered layouts: the Rocksmith layout (fmt 0x42 with the vorb data embedded,
/// 2-byte packet headers, no granules) with INLINE stripped codebooks or packed
/// LIBRARY ids, with ww2ogg's "mod packets" or standard packets; the vorb 0x34
/// layout (6-byte packet headers with granules); and the old header-triad layout
/// (vorb 0x2C, real Vorbis headers, 8-byte packet headers).
/// </para>
/// </summary>
internal static class WemFixtureWriter
{
    internal enum Layout
    {
        /// <summary>fmt 0x42, mod packets (mod signal 0xD9) — the Rocksmith shape.</summary>
        EmbeddedMod,
        /// <summary>fmt 0x42, standard packets (mod signal 0x4A).</summary>
        EmbeddedStandard,
        /// <summary>separate vorb chunk 0x34, 6-byte packet headers with granules.</summary>
        GranuleChunk,
        /// <summary>separate vorb chunk 0x2C, the three real headers, 8-byte packet headers.</summary>
        HeaderTriad,
    }

    internal sealed record OggVorbis(byte[] Ident, byte[] Comment, byte[] Setup, List<byte[]> Audio,
        List<long> AudioGranules, long LastGranule, int Channels, int SampleRate, int Blocksize0Pow, int Blocksize1Pow);

    // ---- Ogg ---------------------------------------------------------------

    internal static OggVorbis ReadOgg(byte[] ogg)
    {
        var packets = new List<byte[]>();
        var granules = new List<long>();
        var current = new List<byte>();
        long lastGranule = 0;
        var off = 0;
        while (off < ogg.Length)
        {
            if (Encoding.ASCII.GetString(ogg, off, 4) != "OggS") throw new InvalidDataException("bad Ogg page");
            var granule = BinaryPrimitives.ReadInt64LittleEndian(ogg.AsSpan(off + 6));
            int segs = ogg[off + 26];
            var data = off + 27 + segs;
            for (var i = 0; i < segs; i++)
            {
                int lace = ogg[off + 27 + i];
                current.AddRange(ogg.AsSpan(data, lace).ToArray());
                data += lace;
                if (lace < 255)
                {
                    packets.Add(current.ToArray());
                    granules.Add(granule);
                    current.Clear();
                }
            }
            if (granule >= 0) lastGranule = granule;
            off = data;
        }

        var ident = packets[0];
        var channels = ident[11];
        var rate = BinaryPrimitives.ReadInt32LittleEndian(ident.AsSpan(12));
        var bs = ident[28];
        return new OggVorbis(ident, packets[1], packets[2], packets.Skip(3).ToList(), granules.Skip(3).ToList(),
            lastGranule, channels, rate, bs & 0x0F, bs >> 4);
    }

    // ---- bit I/O (LSB first, Vorbis order) ------------------------------------

    internal sealed class BitReader(byte[] data)
    {
        private long _pos;
        public long Position => _pos;

        public uint Read(int bits)
        {
            uint v = 0;
            for (var i = 0; i < bits; i++)
            {
                if (_pos >= data.Length * 8L) throw new InvalidDataException("read past end");
                var bit = (uint)(data[_pos >> 3] >> (int)(_pos & 7)) & 1u;
                v |= bit << i;
                _pos++;
            }
            return v;
        }
    }

    internal sealed class BitWriter
    {
        private readonly List<byte> _out = [];
        private long _pos;
        public long Bits => _pos;

        public void Write(uint value, int bits)
        {
            for (var i = 0; i < bits; i++)
            {
                if ((_pos & 7) == 0) _out.Add(0);
                if (((value >> i) & 1u) != 0) _out[^1] |= (byte)(1 << (int)(_pos & 7));
                _pos++;
            }
        }

        public byte[] ToArray() => _out.ToArray();
    }

    private static int ILog(uint v)
    {
        var r = 0;
        while (v != 0) { r++; v >>= 1; }
        return r;
    }

    private static uint Quantvals(uint entries, uint dims)
    {
        var bits = ILog(entries);
        var vals = (long)(entries >> (int)((bits - 1) * (dims - 1) / dims));
        while (true)
        {
            ulong acc = 1, acc1 = 1;
            for (var i = 0; i < dims; i++) { acc *= (ulong)vals; acc1 *= (ulong)(vals + 1); }
            if (acc <= entries && acc1 > entries) return (uint)vals;
            if (acc > entries) vals--; else vals++;
        }
    }

    // ---- setup header: full -> stripped (inverse of ww2ogg's rebuild) --------

    /// <summary>Strips one full Vorbis codebook into Wwise's packed form.</summary>
    private static void StripCodebook(BitReader r, BitWriter w, int forcedLengthBits = 0)
    {
        if (r.Read(24) != 0x564342) throw new InvalidDataException("codebook sync");
        var dims = r.Read(16);
        var entries = r.Read(24);
        if (dims >= 16 || entries >= 1 << 14) throw new InvalidDataException("codebook too big for the packed form");
        w.Write(dims, 4);
        w.Write(entries, 14);
        var ordered = r.Read(1);
        w.Write(ordered, 1);
        if (ordered != 0)
        {
            w.Write(r.Read(5), 5);
            uint current = 0;
            while (current < entries)
            {
                var n = ILog(entries - current);
                var num = r.Read(n);
                w.Write(num, n);
                current += num;
            }
        }
        else
        {
            var sparse = r.Read(1);
            var present = new bool[entries];
            var lengths = new uint[entries];
            uint max = 0;
            for (var i = 0; i < entries; i++)
            {
                present[i] = sparse == 0 || r.Read(1) != 0;
                if (present[i]) { lengths[i] = r.Read(5); max = Math.Max(max, lengths[i]); }
            }
            var lengthBits = forcedLengthBits > 0 ? forcedLengthBits : Math.Max(1, ILog(max));
            w.Write((uint)lengthBits, 3);
            w.Write(sparse, 1);
            for (var i = 0; i < entries; i++)
            {
                if (sparse != 0) w.Write(present[i] ? 1u : 0u, 1);
                if (present[i]) w.Write(lengths[i], lengthBits);
            }
        }

        var lookup = r.Read(4);
        if (lookup > 1) throw new InvalidDataException("lookup type 2 has no packed form");
        w.Write(lookup, 1);
        if (lookup == 1)
        {
            w.Write(r.Read(32), 32);
            w.Write(r.Read(32), 32);
            var valueLength = r.Read(4);
            w.Write(valueLength, 4);
            w.Write(r.Read(1), 1);
            var q = Quantvals(entries, dims);
            for (var i = 0; i < q; i++) w.Write(r.Read((int)valueLength + 1), (int)valueLength + 1);
        }
    }

    /// <summary>Full codebook bit-copy, used to cut the codebooks out of a setup header.</summary>
    private static byte[] CutCodebook(BitReader r)
    {
        var w = new BitWriter();
        // Re-read through the stripper's logic but write the FULL form back.
        if (r.Read(24) != 0x564342) throw new InvalidDataException("codebook sync");
        w.Write(0x564342, 24);
        var dims = r.Read(16); w.Write(dims, 16);
        var entries = r.Read(24); w.Write(entries, 24);
        var ordered = r.Read(1); w.Write(ordered, 1);
        if (ordered != 0)
        {
            w.Write(r.Read(5), 5);
            uint current = 0;
            while (current < entries)
            {
                var n = ILog(entries - current);
                var num = r.Read(n);
                w.Write(num, n);
                current += num;
            }
        }
        else
        {
            var sparse = r.Read(1); w.Write(sparse, 1);
            for (var i = 0; i < entries; i++)
            {
                var p = 1u;
                if (sparse != 0) { p = r.Read(1); w.Write(p, 1); }
                if (p != 0) w.Write(r.Read(5), 5);
            }
        }
        var lookup = r.Read(4); w.Write(lookup, 4);
        if (lookup == 1)
        {
            w.Write(r.Read(32), 32); w.Write(r.Read(32), 32);
            var vl = r.Read(4); w.Write(vl, 4);
            w.Write(r.Read(1), 1);
            var q = Quantvals(entries, dims);
            for (var i = 0; i < q; i++) w.Write(r.Read((int)vl + 1), (int)vl + 1);
        }
        else if (lookup != 0) throw new InvalidDataException("lookup type");
        return w.ToArray();
    }

    /// <summary>
    /// Finds each of the stream's codebooks in a packed library (ww2ogg's
    /// layout), or returns null when any one is missing.
    /// </summary>
    internal static int[]? MatchLibrary(OggVorbis ogg, byte[] library)
    {
        var tableOffset = (int)BinaryPrimitives.ReadUInt32LittleEndian(library.AsSpan(library.Length - 4));
        var count = (library.Length - tableOffset) / 4;
        var offsets = new int[count];
        for (var i = 0; i < count; i++)
            offsets[i] = (int)BinaryPrimitives.ReadUInt32LittleEndian(library.AsSpan(tableOffset + i * 4));

        var r = new BitReader(ogg.Setup);
        for (var i = 0; i < 7; i++) r.Read(8);
        var books = (int)r.Read(8) + 1;
        var ids = new int[books];
        for (var b = 0; b < books; b++)
        {
            var full = CutCodebook(r);
            var found = -1;
            for (var bits = 0; bits <= 5 && found < 0; bits++)
            {
                byte[] stripped;
                long strippedBits;
                try
                {
                    var w = new BitWriter();
                    StripCodebook(new BitReader(full), w, bits);
                    stripped = w.ToArray();
                    strippedBits = w.Bits;
                }
                catch (InvalidDataException) { continue; }

                var size = (int)(strippedBits / 8 + 1);   // ww2ogg's "one extra 0 byte" rule
                for (var id = 0; id < count - 1 && found < 0; id++)
                {
                    if (offsets[id + 1] - offsets[id] != size) continue;
                    if (library.AsSpan(offsets[id], stripped.Length).SequenceEqual(stripped)) found = id;
                }
            }
            if (found < 0) return null;
            ids[b] = found;
        }
        return ids;
    }

    /// <summary>The setup packet in Wwise's stripped form.</summary>
    internal static byte[] StripSetup(OggVorbis ogg, int[]? libraryIds, out bool[] modeBlockflags)
    {
        var r = new BitReader(ogg.Setup);
        var w = new BitWriter();
        void Copy(int n) => w.Write(r.Read(n), n);

        if (r.Read(8) != 5) throw new InvalidDataException("not a setup header");
        for (var i = 0; i < 6; i++) r.Read(8);
        var books = (int)r.Read(8) + 1;
        w.Write((uint)books - 1, 8);
        for (var b = 0; b < books; b++)
        {
            if (libraryIds is null) StripCodebook(r, w);
            else { CutCodebook(r); w.Write((uint)libraryIds[b], 10); }
        }

        var times = (int)r.Read(6) + 1;
        for (var i = 0; i < times; i++) r.Read(16);

        var floors = (int)r.Read(6) + 1;
        w.Write((uint)floors - 1, 6);
        for (var f = 0; f < floors; f++)
        {
            if (r.Read(16) != 1) throw new InvalidDataException("floor 0 has no packed form");
            var partitions = (int)r.Read(5);
            w.Write((uint)partitions, 5);
            var classes = new int[partitions];
            var maxClass = -1;
            for (var j = 0; j < partitions; j++)
            {
                classes[j] = (int)r.Read(4);
                w.Write((uint)classes[j], 4);
                maxClass = Math.Max(maxClass, classes[j]);
            }
            var dims = new int[maxClass + 1];
            for (var j = 0; j <= maxClass; j++)
            {
                dims[j] = (int)r.Read(3) + 1;
                w.Write((uint)dims[j] - 1, 3);
                var sub = (int)r.Read(2);
                w.Write((uint)sub, 2);
                if (sub != 0) Copy(8);
                for (var k = 0; k < 1 << sub; k++) Copy(8);
            }
            Copy(2);
            var rangeBits = (int)r.Read(4);
            w.Write((uint)rangeBits, 4);
            for (var j = 0; j < partitions; j++)
                for (var k = 0; k < dims[classes[j]]; k++) Copy(rangeBits);
        }

        var residues = (int)r.Read(6) + 1;
        w.Write((uint)residues - 1, 6);
        for (var i = 0; i < residues; i++)
        {
            w.Write(r.Read(16), 2);
            Copy(24); Copy(24); Copy(24);
            var classifications = (int)r.Read(6) + 1;
            w.Write((uint)classifications - 1, 6);
            Copy(8);
            var cascade = new int[classifications];
            for (var j = 0; j < classifications; j++)
            {
                var low = (int)r.Read(3);
                w.Write((uint)low, 3);
                var flag = r.Read(1);
                w.Write(flag, 1);
                var high = 0;
                if (flag != 0) { high = (int)r.Read(5); w.Write((uint)high, 5); }
                cascade[j] = high * 8 + low;
            }
            for (var j = 0; j < classifications; j++)
                for (var k = 0; k < 8; k++)
                    if ((cascade[j] & (1 << k)) != 0) Copy(8);
        }

        var mappings = (int)r.Read(6) + 1;
        w.Write((uint)mappings - 1, 6);
        for (var i = 0; i < mappings; i++)
        {
            if (r.Read(16) != 0) throw new InvalidDataException("mapping type");
            var submapsFlag = r.Read(1);
            w.Write(submapsFlag, 1);
            var submaps = 1;
            if (submapsFlag != 0) { submaps = (int)r.Read(4) + 1; w.Write((uint)submaps - 1, 4); }
            var squarePolar = r.Read(1);
            w.Write(squarePolar, 1);
            if (squarePolar != 0)
            {
                var steps = (int)r.Read(8) + 1;
                w.Write((uint)steps - 1, 8);
                var bits = ILog((uint)ogg.Channels - 1);
                for (var j = 0; j < steps; j++) { Copy(bits); Copy(bits); }
            }
            Copy(2);
            if (submaps > 1)
                for (var j = 0; j < ogg.Channels; j++) Copy(4);
            for (var j = 0; j < submaps; j++) { Copy(8); Copy(8); Copy(8); }
        }

        var modes = (int)r.Read(6) + 1;
        w.Write((uint)modes - 1, 6);
        modeBlockflags = new bool[modes];
        for (var i = 0; i < modes; i++)
        {
            var flag = r.Read(1);
            w.Write(flag, 1);
            modeBlockflags[i] = flag != 0;
            r.Read(16); r.Read(16);
            Copy(8);
        }
        return w.ToArray();
    }

    /// <summary>An Ogg audio packet with the packet-type bit and the window flags
    /// removed — what a mod-packet .wem stores.</summary>
    internal static byte[] StripAudioPacket(byte[] packet, bool[] modeBlockflags)
    {
        var modeBits = ILog((uint)modeBlockflags.Length - 1);
        var r = new BitReader(packet);
        if (r.Read(1) != 0) throw new InvalidDataException("not an audio packet");
        var mode = r.Read(modeBits);
        var w = new BitWriter();
        w.Write(mode, modeBits);
        if (modeBlockflags[mode]) { r.Read(1); r.Read(1); }
        var remaining = packet.Length * 8L - r.Position;
        for (long i = 0; i < remaining; i++) w.Write(r.Read(1), 1);
        return w.ToArray();
    }

    // ---- RIFF ------------------------------------------------------------------

    internal static byte[] Build(OggVorbis ogg, Layout layout, bool inlineCodebooks, int[]? libraryIds = null)
    {
        var data = new MemoryStream();
        uint setupOffset = 0, firstAudioOffset;

        void Packet(byte[] payload, uint granule)
        {
            Span<byte> h = stackalloc byte[8];
            switch (layout)
            {
                case Layout.HeaderTriad:
                    BinaryPrimitives.WriteUInt32LittleEndian(h, (uint)payload.Length);
                    BinaryPrimitives.WriteUInt32LittleEndian(h[4..], granule);
                    data.Write(h[..8]);
                    break;
                case Layout.GranuleChunk:
                    BinaryPrimitives.WriteUInt16LittleEndian(h, checked((ushort)payload.Length));
                    BinaryPrimitives.WriteUInt32LittleEndian(h[2..], granule);
                    data.Write(h[..6]);
                    break;
                default:
                    BinaryPrimitives.WriteUInt16LittleEndian(h, checked((ushort)payload.Length));
                    data.Write(h[..2]);
                    break;
            }
            data.Write(payload);
        }

        bool[] modeFlags;
        if (layout == Layout.HeaderTriad)
        {
            Packet(ogg.Ident, 0);
            Packet(ogg.Comment, 0);
            Packet(ogg.Setup, 0);
            StripSetup(ogg, null, out modeFlags);
        }
        else
        {
            Packet(StripSetup(ogg, inlineCodebooks ? null : libraryIds, out modeFlags), 0);
        }
        firstAudioOffset = (uint)data.Length;

        var mod = layout == Layout.EmbeddedMod;
        var modeBits = ILog((uint)modeFlags.Length - 1);
        long cumulative = 0;
        var prevLong = false;
        for (var i = 0; i < ogg.Audio.Count; i++)
        {
            var payload = mod ? StripAudioPacket(ogg.Audio[i], modeFlags) : ogg.Audio[i];
            // Granules (only the GranuleChunk/HeaderTriad layouts carry them): the
            // running decoded length after this packet, as Wwise writes it, the
            // last one the stream's end.
            var r = new BitReader(ogg.Audio[i]);
            r.Read(1);
            var isLong = modeFlags[r.Read(modeBits)];
            if (i > 0)
                cumulative += ((prevLong ? 1L << ogg.Blocksize1Pow : 1L << ogg.Blocksize0Pow) / 4)
                            + ((isLong ? 1L << ogg.Blocksize1Pow : 1L << ogg.Blocksize0Pow) / 4);
            prevLong = isLong;
            var granule = i == ogg.Audio.Count - 1 ? (uint)ogg.LastGranule : (uint)cumulative;
            Packet(payload, granule);
        }

        var riff = new MemoryStream();
        var bw = new BinaryWriter(riff);
        bw.Write("RIFF"u8);
        bw.Write(0u);   // patched below
        bw.Write("WAVE"u8);

        var embedded = layout is Layout.EmbeddedMod or Layout.EmbeddedStandard;
        bw.Write("fmt "u8);
        bw.Write(embedded ? 0x42u : 0x18u);
        bw.Write((ushort)0xFFFF);
        bw.Write((ushort)ogg.Channels);
        bw.Write(ogg.SampleRate);
        bw.Write(16000);                                       // avg bytes / s
        bw.Write((ushort)0);                                   // block align
        bw.Write((ushort)0);                                   // bps
        bw.Write((ushort)(embedded ? 0x42 - 0x12 : 0x18 - 0x12));
        bw.Write((ushort)0);                                   // ext unk
        bw.Write(0u);                                          // subtype
        if (embedded)
        {
            // vorb, 0x2A bytes at fmt + 0x18
            var vorb = new byte[0x2A];
            BinaryPrimitives.WriteUInt32LittleEndian(vorb, (uint)ogg.LastGranule);
            BinaryPrimitives.WriteUInt32LittleEndian(vorb.AsSpan(4), mod ? 0xD9u : 0x4Au);
            BinaryPrimitives.WriteUInt32LittleEndian(vorb.AsSpan(0x10), setupOffset);
            BinaryPrimitives.WriteUInt32LittleEndian(vorb.AsSpan(0x14), firstAudioOffset);
            BinaryPrimitives.WriteUInt32LittleEndian(vorb.AsSpan(0x24), 0x12345678u);
            vorb[0x28] = (byte)ogg.Blocksize0Pow;
            vorb[0x29] = (byte)ogg.Blocksize1Pow;
            bw.Write(vorb);
        }
        else
        {
            var size = layout == Layout.HeaderTriad ? 0x2C : 0x34;
            var vorb = new byte[size];
            BinaryPrimitives.WriteUInt32LittleEndian(vorb, (uint)ogg.LastGranule);
            BinaryPrimitives.WriteUInt32LittleEndian(vorb.AsSpan(0x18), setupOffset);
            BinaryPrimitives.WriteUInt32LittleEndian(vorb.AsSpan(0x1C), firstAudioOffset);
            if (layout == Layout.GranuleChunk)
            {
                BinaryPrimitives.WriteUInt32LittleEndian(vorb.AsSpan(0x2C), 0x12345678u);
                vorb[0x30] = (byte)ogg.Blocksize0Pow;
                vorb[0x31] = (byte)ogg.Blocksize1Pow;
            }
            bw.Write("vorb"u8);
            bw.Write((uint)size);
            bw.Write(vorb);
        }

        bw.Write("data"u8);
        bw.Write((uint)data.Length);
        bw.Write(data.ToArray());
        bw.Flush();
        var bytes = riff.ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), (uint)(bytes.Length - 8));
        return bytes;
    }
}
