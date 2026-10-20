using System;
using System.Buffers.Binary;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp.Dsp;
using JuceSharp.Interop;
using JuceSharp.Tests.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v45: the <c>rsamp</c> chain table is loaded at RUN TIME
/// (<c>dca_rsamp_set_table</c>, <c>dca_rsamp_table_row_count</c>). The numbers are
/// Rocksmith's own, so the library ships none of them: a host builds the table from
/// the user's own install with <see cref="RsAmpChainTable" />. Every table here is
/// synthetic. The table is process-wide, hence the <c>NativeEngine</c> collection.
/// </summary>
[Collection("NativeEngine")]
public sealed class AbiV45Tests : IDisposable
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    public void Dispose()
    {
        if (NativeProbe.IsAvailable) JuceRsAmpTable.Unload();
    }

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 45u, "dca_rsamp_set_table predates the current version");
    }

    [Theory]
    [InlineData("dca_rsamp_set_table")]
    [InlineData("dca_rsamp_table_row_count")]
    public void TheV45Exports_AreInTheHeaderAndTheDll(string export)
    {
        Assert.Contains($"DCA_CALL {export}(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr), $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    /// <summary>The writer's header is the native format's: magic, version, then the six
    /// counts (dcdsp_rsamp_table.h). A drift here is refused natively as "bad magic" or
    /// "unknown format version", which the behaviour tests below would also catch.</summary>
    [Fact]
    public void TheWriter_EmitsTheNativeHeader()
    {
        var blob = SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(0), SyntheticRsAmpTables.FutzRow(55)).ToBytes();
        Assert.Equal("DCRSAMPT"u8.ToArray(), blob[..8]);
        Assert.Equal(RsAmpChainTable.FormatVersion, BinaryPrimitives.ReadUInt32LittleEndian(blob.AsSpan(8)));
        Assert.Equal(2u, BinaryPrimitives.ReadUInt32LittleEndian(blob.AsSpan(12)));   // rows
        Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(blob.AsSpan(16)));   // Guitar Distortion blocks
        Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(blob.AsSpan(20)));   // FutzBox blocks
        Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(blob.AsSpan(24)));   // Compressor blocks
        Assert.Equal(8u, BinaryPrimitives.ReadUInt32LittleEndian(blob.AsSpan(28)));   // points: 3 + 1 curves x 2
        Assert.Equal(4u, BinaryPrimitives.ReadUInt32LittleEndian(blob.AsSpan(32)));   // curves
    }

    [NativeAvailableFact]
    public void ALoadedTable_IsCounted_AndUnloadClearsIt()
    {
        JuceRsAmpTable.Unload();
        Assert.Equal(0, JuceRsAmpTable.LoadedRowCount);
        Assert.True(JuceRsAmpTable.TryLoad(SyntheticRsAmpTables.Full()));
        Assert.Equal(63, JuceRsAmpTable.LoadedRowCount);
        JuceRsAmpTable.Unload();
        Assert.Equal(0, JuceRsAmpTable.LoadedRowCount);
    }

    /// <summary>A row whose key is not the amp that row names is refused WHOLE, and the
    /// table in force stays: a generator that ordered its rows differently from the
    /// native list must not silently run one amp's chain under another's name.</summary>
    [NativeAvailableFact]
    public void ARowWhoseKeyIsNotThatRowsAmp_IsRefused_AndTheTableInForceStays()
    {
        Assert.True(JuceRsAmpTable.TryLoad(SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(0), SyntheticRsAmpTables.EqRow(1))));
        var wrong = SyntheticRsAmpTables.EqRow(2) with { Key = SyntheticRsAmpTables.Keys[3] };
        Assert.False(JuceRsAmpTable.TryLoad(SyntheticRsAmpTables.Of(wrong)));
        Assert.Equal(2, JuceRsAmpTable.LoadedRowCount);
    }

    [NativeAvailableFact]
    public void AMalformedBlob_IsRefused()
    {
        var good = SyntheticRsAmpTables.Of(SyntheticRsAmpTables.EqRow(0)).ToBytes();
        var truncated = good[..^1];
        var trailing = new byte[good.Length + 1];
        good.CopyTo(trailing, 0);
        var badMagic = (byte[])good.Clone();
        badMagic[0] = (byte)'X';
        Assert.False(JuceRsAmpTable.TryLoad(truncated));
        Assert.False(JuceRsAmpTable.TryLoad(trailing));
        Assert.False(JuceRsAmpTable.TryLoad(badMagic));
        Assert.False(JuceRsAmpTable.TryLoad([]));
        Assert.True(JuceRsAmpTable.TryLoad(good));
    }

    [NativeAvailableFact]
    public void ANullOrNegativeLength_IsAnInvalidArgument()
    {
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.RsAmpSetTable(null, 5));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.RsAmpSetTable([1, 2, 3], -1));
        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.RsAmpSetTable(null, 0));
    }
}
