using System.Buffers.Binary;
using System.Text;
using JuceSharp.Interop;

namespace JuceSharp.Dsp;

/// <summary>Which Wwise plug-in a slot of a Rocksmith amp chain clones.</summary>
public enum RsAmpSlotKind : byte
{
    /// <summary>Audiokinetic Wwise Guitar Distortion (plug-in 0x007E0003).</summary>
    GuitarDistortion = 1,

    /// <summary>McDSP FutzBox (plug-in 0x006E1003).</summary>
    FutzBox = 2,

    /// <summary>Audiokinetic Wwise Compressor (plug-in 0x006C0003).</summary>
    Compressor = 3,
}

/// <summary>One Guitar Distortion EQ band.</summary>
/// <param name="Type">Filter type: 0 low shelf, 1 peaking, 2 high shelf, 3 low-pass, 4 high-pass, 5 band-pass, 6 notch.</param>
/// <param name="On">Whether the band is in the path.</param>
/// <param name="GainDb">Gain in dB.</param>
/// <param name="Hz">Centre or corner frequency in Hz.</param>
/// <param name="Q">Quality factor.</param>
public sealed record RsAmpBand(uint Type, bool On, float GainDb, float Hz, float Q);

/// <summary>
///     A Guitar Distortion block: three bands before the waveshaper, three after.
/// </summary>
/// <param name="Bands">Exactly six bands, pre-EQ first.</param>
/// <param name="Distortion">Waveshaper: 0 none, 1 overdrive, 2 heavy, 3 fuzz, 4 clip.</param>
/// <param name="Drive">Drive, percent.</param>
/// <param name="Tone">Tone, percent.</param>
/// <param name="Rectification">Rectification, percent.</param>
/// <param name="OutputDb">Output gain in dB.</param>
/// <param name="Wet">Wet share, percent.</param>
public sealed record RsAmpGdBlock(
    IReadOnlyList<RsAmpBand> Bands, uint Distortion, float Drive, float Tone, float Rectification, float OutputDb, float Wet);

/// <summary>A FutzBox block: the sections Rocksmith's amps switch on.</summary>
/// <param name="FiltersOn">Whether the high- and low-pass filters run.</param>
/// <param name="LpfSlope">Low-pass slope setting.</param>
/// <param name="LpfHz">Low-pass corner in Hz.</param>
/// <param name="LpfResonance">Low-pass resonance, percent.</param>
/// <param name="HpfSlope">High-pass slope setting.</param>
/// <param name="HpfHz">High-pass corner in Hz.</param>
/// <param name="HpfResonance">High-pass resonance, percent.</param>
/// <param name="DistortionOn">Whether the distortion runs.</param>
/// <param name="Mode">Distortion mode.</param>
/// <param name="Amount">Distortion amount, percent.</param>
/// <param name="Intensity">Distortion intensity, percent.</param>
/// <param name="Rectify">Rectification, percent.</param>
/// <param name="EqOn">Whether the EQ band runs.</param>
/// <param name="EqType">EQ band type: 0 high-pass, 1 peaking, 2 low-pass.</param>
/// <param name="EqHz">EQ band frequency in Hz.</param>
/// <param name="EqQ">EQ band Q.</param>
/// <param name="EqDb">EQ band gain in dB.</param>
/// <param name="InputDb">Input gain in dB.</param>
/// <param name="OutputDb">Output gain in dB.</param>
/// <param name="Balance">Wet share, percent.</param>
public sealed record RsAmpFutzBlock(
    bool FiltersOn, float LpfSlope, float LpfHz, float LpfResonance, float HpfSlope, float HpfHz, float HpfResonance,
    bool DistortionOn, uint Mode, float Amount, float Intensity, float Rectify,
    bool EqOn, uint EqType, float EqHz, float EqQ, float EqDb,
    float InputDb, float OutputDb, float Balance);

/// <summary>A Wwise Compressor block.</summary>
/// <param name="ThresholdDb">Threshold in dB.</param>
/// <param name="Ratio">Ratio (n:1).</param>
/// <param name="AttackS">Attack in seconds.</param>
/// <param name="ReleaseS">Release in seconds.</param>
/// <param name="OutputDb">Output gain in dB.</param>
public sealed record RsAmpCompBlock(float ThresholdDb, float Ratio, float AttackS, float ReleaseS, float OutputDb);

/// <summary>One slot of a chain: the plug-in it clones and that plug-in's base block.
/// Exactly the block matching <paramref name="Kind" /> is set.</summary>
/// <param name="Kind">The plug-in.</param>
/// <param name="Gd">The Guitar Distortion block, for <see cref="RsAmpSlotKind.GuitarDistortion" />.</param>
/// <param name="Futz">The FutzBox block, for <see cref="RsAmpSlotKind.FutzBox" />.</param>
/// <param name="Comp">The Compressor block, for <see cref="RsAmpSlotKind.Compressor" />.</param>
public sealed record RsAmpSlot(RsAmpSlotKind Kind, RsAmpGdBlock? Gd = null, RsAmpFutzBlock? Futz = null, RsAmpCompBlock? Comp = null);

/// <summary>A knob's travel, in the game's own units.</summary>
/// <param name="Min">Minimum.</param>
/// <param name="Max">Maximum.</param>
/// <param name="Default">Default.</param>
/// <param name="Discrete">A switch or selector: the core rounds it.</param>
public sealed record RsAmpKnobRange(float Min, float Max, float Default, bool Discrete);

/// <summary>One point of an RTPC curve.</summary>
/// <param name="X">Knob value.</param>
/// <param name="Y">The STORED parameter value (see <see cref="RsAmpCurve.Scaling" />).</param>
/// <param name="Shape">The <c>AkCurveInterpolation</c> of the segment starting here, 0..9.</param>
public sealed record RsAmpCurvePoint(float X, float Y, byte Shape);

/// <summary>An RTPC curve: one knob onto one parameter of one slot.</summary>
/// <param name="Knob">Knob index in the row.</param>
/// <param name="Slot">Slot index in the row.</param>
/// <param name="Param">The plug-in's RTPC parameter id.</param>
/// <param name="Scaling">0 none, 2 dB (stored <c>sign(d)(1 - 10^(-|d|/20))</c>), 3 log10 Hz.</param>
/// <param name="Points">At least one point, ascending in <see cref="RsAmpCurvePoint.X" />.</param>
public sealed record RsAmpCurve(int Knob, int Slot, int Param, int Scaling, IReadOnlyList<RsAmpCurvePoint> Points);

/// <summary>One amp's chain: a row of the native <c>rsamp</c> model list.</summary>
/// <param name="Row">The native row index (<c>dcdsp_rsamp_models.h</c>).</param>
/// <param name="Key">The row's gear key; the native side refuses a mismatch.</param>
/// <param name="Knobs">Up to ten knobs, in <c>k1</c>.. order.</param>
/// <param name="Slots">One to four plug-ins, in signal order.</param>
/// <param name="Curves">The knobs' curves.</param>
public sealed record RsAmpChainRow(
    int Row, string Key, IReadOnlyList<RsAmpKnobRange> Knobs, IReadOnlyList<RsAmpSlot> Slots, IReadOnlyList<RsAmpCurve> Curves);

/// <summary>
///     The <c>rsamp</c> module's chain table (ABI v45). Its numbers are Rocksmith
///     2014's own, so neither this library nor its host ship any: a host builds this
///     from the user's own install and loads it with <see cref="JuceRsAmpTable.TryLoad(RsAmpChainTable)" />.
///     <see cref="ToBytes" /> writes the blob format of
///     <c>src/dsp/cores/dcdsp_rsamp_table.h</c>.
/// </summary>
/// <param name="Rows">The rows with a chain, ascending by <see cref="RsAmpChainRow.Row" />.</param>
public sealed record RsAmpChainTable(IReadOnlyList<RsAmpChainRow> Rows)
{
    /// <summary>The blob format version this writer emits.</summary>
    public const uint FormatVersion = 1;

    /// <summary>Packs the table into the native blob. Throws <see cref="ArgumentException" />
    /// for a shape the format cannot carry (more than 10 knobs, a block list past 65,535,
    /// a slot whose block is missing, a key longer than 255 bytes).</summary>
    public byte[] ToBytes()
    {
        var gd = new List<RsAmpGdBlock>();
        var futz = new List<RsAmpFutzBlock>();
        var comp = new List<RsAmpCompBlock>();
        var points = new List<RsAmpCurvePoint>();
        var curves = new List<(RsAmpCurve Curve, int FirstPoint)>();
        var rows = new MemoryStream();
        foreach (var row in Rows)
        {
            if (row.Row is < 0 or > 255) throw new ArgumentException($"row {row.Row} out of range", nameof(Rows));
            var key = Encoding.ASCII.GetBytes(row.Key);
            if (key.Length > 255) throw new ArgumentException($"key '{row.Key}' too long", nameof(Rows));
            if (row.Knobs.Count > 10) throw new ArgumentException($"{row.Key}: more than ten knobs", nameof(Rows));
            if (row.Slots.Count is < 1 or > 4) throw new ArgumentException($"{row.Key}: one to four slots", nameof(Rows));
            rows.WriteByte((byte)row.Row);
            rows.WriteByte((byte)key.Length);
            rows.Write(key);
            rows.WriteByte((byte)row.Knobs.Count);
            foreach (var k in row.Knobs)
            {
                F32(rows, k.Min);
                F32(rows, k.Max);
                F32(rows, k.Default);
                rows.WriteByte(k.Discrete ? (byte)1 : (byte)0);
            }

            rows.WriteByte((byte)row.Slots.Count);
            foreach (var s in row.Slots)
            {
                rows.WriteByte((byte)s.Kind);
                switch (s.Kind)
                {
                    case RsAmpSlotKind.GuitarDistortion:
                        U16(rows, gd.Count);
                        gd.Add(s.Gd ?? throw new ArgumentException($"{row.Key}: Guitar Distortion slot without a block", nameof(Rows)));
                        break;
                    case RsAmpSlotKind.FutzBox:
                        U16(rows, futz.Count);
                        futz.Add(s.Futz ?? throw new ArgumentException($"{row.Key}: FutzBox slot without a block", nameof(Rows)));
                        break;
                    case RsAmpSlotKind.Compressor:
                        U16(rows, comp.Count);
                        comp.Add(s.Comp ?? throw new ArgumentException($"{row.Key}: Compressor slot without a block", nameof(Rows)));
                        break;
                    default:
                        throw new ArgumentException($"{row.Key}: unknown slot kind {s.Kind}", nameof(Rows));
                }
            }

            U16(rows, curves.Count);
            U16(rows, row.Curves.Count);
            foreach (var c in row.Curves)
            {
                if (c.Points.Count is < 1 or > 255) throw new ArgumentException($"{row.Key}: a curve has {c.Points.Count} points", nameof(Rows));
                curves.Add((c, points.Count));
                points.AddRange(c.Points);
            }
        }

        if (gd.Count > 0xFFFF || futz.Count > 0xFFFF || comp.Count > 0xFFFF || curves.Count > 0xFFFF || points.Count > 0x10000)
            throw new ArgumentException("the table is larger than the format carries", nameof(Rows));

        var o = new MemoryStream();
        o.Write("DCRSAMPT"u8);
        U32(o, FormatVersion);
        U32(o, (uint)Rows.Count);
        U32(o, (uint)gd.Count);
        U32(o, (uint)futz.Count);
        U32(o, (uint)comp.Count);
        U32(o, (uint)points.Count);
        U32(o, (uint)curves.Count);
        rows.Position = 0;
        rows.CopyTo(o);
        foreach (var b in gd)
        {
            if (b.Bands.Count != 6) throw new ArgumentException("a Guitar Distortion block has six bands", nameof(Rows));
            foreach (var band in b.Bands)
            {
                U32(o, band.Type);
                o.WriteByte(band.On ? (byte)1 : (byte)0);
                F32(o, band.GainDb);
                F32(o, band.Hz);
                F32(o, band.Q);
            }

            U32(o, b.Distortion);
            F32(o, b.Drive);
            F32(o, b.Tone);
            F32(o, b.Rectification);
            F32(o, b.OutputDb);
            F32(o, b.Wet);
        }

        foreach (var b in futz)
        {
            o.WriteByte(b.FiltersOn ? (byte)1 : (byte)0);
            F32(o, b.LpfSlope);
            F32(o, b.LpfHz);
            F32(o, b.LpfResonance);
            F32(o, b.HpfSlope);
            F32(o, b.HpfHz);
            F32(o, b.HpfResonance);
            o.WriteByte(b.DistortionOn ? (byte)1 : (byte)0);
            U32(o, b.Mode);
            F32(o, b.Amount);
            F32(o, b.Intensity);
            F32(o, b.Rectify);
            o.WriteByte(b.EqOn ? (byte)1 : (byte)0);
            U32(o, b.EqType);
            F32(o, b.EqHz);
            F32(o, b.EqQ);
            F32(o, b.EqDb);
            F32(o, b.InputDb);
            F32(o, b.OutputDb);
            F32(o, b.Balance);
        }

        foreach (var b in comp)
        {
            F32(o, b.ThresholdDb);
            F32(o, b.Ratio);
            F32(o, b.AttackS);
            F32(o, b.ReleaseS);
            F32(o, b.OutputDb);
        }

        foreach (var p in points)
        {
            F32(o, p.X);
            F32(o, p.Y);
            o.WriteByte(p.Shape);
        }

        foreach (var (c, first) in curves)
        {
            o.WriteByte((byte)c.Knob);
            o.WriteByte((byte)c.Slot);
            o.WriteByte((byte)c.Param);
            o.WriteByte((byte)c.Scaling);
            U16(o, first);
            o.WriteByte((byte)c.Points.Count);
        }

        return o.ToArray();
    }

    private static void U16(Stream s, int v)
    {
        Span<byte> b = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16LittleEndian(b, checked((ushort)v));
        s.Write(b);
    }

    private static void U32(Stream s, uint v)
    {
        Span<byte> b = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(b, v);
        s.Write(b);
    }

    private static void F32(Stream s, float v)
    {
        Span<byte> b = stackalloc byte[4];
        BinaryPrimitives.WriteSingleLittleEndian(b, v);
        s.Write(b);
    }
}

/// <summary>
///     The process-wide <c>rsamp</c> chain table (ABI v45,
///     <c>dca_rsamp_set_table</c>). Every <c>rsamp</c> module of every engine in
///     the process runs the table loaded here; until one is, they pass audio
///     through. Control thread: loading copies and parses the blob natively.
/// </summary>
public static class JuceRsAmpTable
{
    /// <summary>Loads <paramref name="table" />. False when the native side refused it
    /// (a row whose key is not that row's amp, an index out of range, a non-finite value);
    /// the table in force then stays and the reason is in the engine log. Throws
    /// <see cref="ArgumentException" /> when the table cannot be packed at all.</summary>
    public static bool TryLoad(RsAmpChainTable table)
    {
        ArgumentNullException.ThrowIfNull(table);
        return TryLoad(table.ToBytes());
    }

    /// <summary>Loads an already packed blob (<see cref="RsAmpChainTable.ToBytes" />).
    /// False when the native side refused it.</summary>
    public static bool TryLoad(byte[] blob)
    {
        ArgumentNullException.ThrowIfNull(blob);
        if (blob.Length == 0) return false;
        return NativeMethods.RsAmpSetTable(blob, blob.Length) == NativeMethods.DcaStatus.Ok;
    }

    /// <summary>Unloads the table: every <c>rsamp</c> module dips and then passes audio through.</summary>
    public static void Unload() => NativeMethods.RsAmpSetTable(null, 0);

    /// <summary>How many rows the loaded table has a chain for; 0 when none is loaded
    /// (<c>dca_rsamp_table_row_count</c>).</summary>
    public static int LoadedRowCount => NativeMethods.RsAmpTableRowCount();
}
