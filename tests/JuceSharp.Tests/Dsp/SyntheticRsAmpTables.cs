using JuceSharp.Dsp;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// SYNTHETIC <c>rsamp</c> chain tables for the tests. Rocksmith's own numbers are
/// generated on the user's machine from the user's own install and are in no
/// repository, so every row here is made up — chosen so that each expected
/// response follows from the row's own definition (a band at a known frequency, a
/// curve with known ends) and from nothing else.
/// </summary>
internal static class SyntheticRsAmpTables
{
    /// <summary>The native row list's keys, in row order (public: the gear catalogue's
    /// amp keys, ordinal). Row 17 is <c>Amp_GB100</c>, 42 <c>Bass_Amp_BT600B</c>, 55
    /// <c>Bass_Amp_HT300B</c>.</summary>
    internal static readonly string[] Keys =
    [
        "Amp_AT120", "Amp_AT20", "Amp_BT15", "Amp_BT30", "Amp_BT45", "Amp_CA100", "Amp_CA38", "Amp_CA85",
        "Amp_CS100", "Amp_CS120", "Amp_CS90", "Amp_EN30", "Amp_EN50", "Amp_EpiphoneElectarCentury",
        "Amp_EpiphoneElectarM", "Amp_EpiphoneElectricHawaiian", "Amp_EpiphoneZephyr", "Amp_GB100", "Amp_GB38",
        "Amp_GB50", "Amp_GibsonGA79", "Amp_GibsonGA8", "Amp_GibsonGA88", "Amp_HG100", "Amp_HG180", "Amp_HG500",
        "Amp_Marshall1962Bluesbreaker", "Amp_MarshallDSL100H", "Amp_MarshallDSL15H", "Amp_MarshallJCM800",
        "Amp_MarshallJTM45", "Amp_MarshallJVM410H", "Amp_MarshallPlexi", "Amp_OrangeAD50", "Amp_OrangeJimmyBean",
        "Amp_OrangeOR100", "Amp_OrangeOR50H", "Amp_OrangeRockerverb", "Amp_OrangeTinyTerror", "Amp_TW22",
        "Amp_TW26", "Amp_TW40", "Bass_Amp_BT600B", "Bass_Amp_BT880B", "Bass_Amp_BT975B", "Bass_Amp_CH300B",
        "Bass_Amp_CH350B", "Bass_Amp_CH600B", "Bass_Amp_CS240B", "Bass_Amp_CS300B", "Bass_Amp_CS75B",
        "Bass_Amp_EdenE300", "Bass_Amp_EdenWT550", "Bass_Amp_EdenWT800", "Bass_Amp_HT100B", "Bass_Amp_HT300B",
        "Bass_Amp_HT400B", "Bass_Amp_LT25B", "Bass_Amp_LT85B", "Bass_Amp_OrangeAD200B", "DI_Amp_BassDriver",
        "DI_Amp_MixerPre", "DI_Amp_TubePre",
    ];

    private static readonly RsAmpBand Off = new(1, false, 0f, 1000f, 1f);

    /// <summary>The stored form of a dB value on a dB-scaled curve.</summary>
    internal static float DbStored(float db) => MathF.Sign(db) * (1f - MathF.Pow(10f, -MathF.Abs(db) / 20f));

    private static RsAmpCurvePoint[] Line(float x0, float y0, float x1, float y1) => [new(x0, y0, 4), new(x1, y1, 4)];

    /// <summary>
    /// Row <paramref name="row" />: one Guitar Distortion block whose post band 1
    /// (band index 3) is a 1 kHz, Q 1 peaking bell, and three knobs, all 0..100:
    /// k1 = the waveshaper's drive 0..100 % (heavy), k2 = that bell's gain −12..+12 dB
    /// (dB-scaled, so exactly ±12 at the ends), k3 = the block's output −6..+6 dB.
    /// With <paramref name="distortion" /> 0 the waveshaper is out of the path.
    /// </summary>
    internal static RsAmpChainRow EqRow(int row, uint distortion = 0)
    {
        var bands = new[] { Off, Off, Off, new RsAmpBand(1, true, 0f, 1000f, 1f), Off, Off };
        var gd = new RsAmpGdBlock(bands, distortion, 0f, 50f, 0f, 0f, 100f);
        return new RsAmpChainRow(row, Keys[row],
            [new(0f, 100f, 50f, false), new(0f, 100f, 50f, false), new(0f, 100f, 50f, false)],
            [new(RsAmpSlotKind.GuitarDistortion, Gd: gd)],
            [
                new(0, 0, 61, 0, Line(0f, 0f, 100f, 100f)),
                new(1, 0, 31, 2, Line(0f, DbStored(-12f), 100f, DbStored(12f))),
                new(2, 0, 64, 2, Line(0f, DbStored(-6f), 100f, DbStored(6f))),
            ]);
    }

    /// <summary>
    /// Row <paramref name="row" />: a FutzBox whose low-pass corner k1 sweeps
    /// 200 Hz .. 20 kHz (log-scaled), no distortion, no EQ; then a Wwise Compressor at
    /// a threshold no small signal reaches.
    /// </summary>
    internal static RsAmpChainRow FutzRow(int row)
    {
        var futz = new RsAmpFutzBlock(true, 0f, 20000f, 0f, 0f, 20f, 0f, false, 0, 0f, 0f, 0f, false, 1, 1000f, 1f, 0f,
            0f, 0f, 100f);
        var comp = new RsAmpCompBlock(-6f, 4f, 0.005f, 0.1f, 0f);
        return new RsAmpChainRow(row, Keys[row],
            [new(0f, 100f, 100f, false)],
            [new(RsAmpSlotKind.FutzBox, Futz: futz), new(RsAmpSlotKind.Compressor, Comp: comp)],
            [new(0, 0, 52, 3, Line(0f, MathF.Log10(200f), 100f, MathF.Log10(20000f)))]);
    }

    /// <summary>Every row an <see cref="EqRow" /> with a mild overdrive, except 55
    /// (a <see cref="FutzRow" />): every model then plays something.</summary>
    internal static RsAmpChainTable Full()
        => new([.. Enumerable.Range(0, Keys.Length).Select(r => r == 55 ? FutzRow(r) : EqRow(r, distortion: 1))]);

    /// <summary>Just the given rows.</summary>
    internal static RsAmpChainTable Of(params RsAmpChainRow[] rows) => new(rows);
}
