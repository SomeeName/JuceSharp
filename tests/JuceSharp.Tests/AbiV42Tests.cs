using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v42 (DotCharter #426): native Wwise-Vorbis (.wem) decoding. Seven additive
/// <c>dca_wem_*</c> exports, <c>dca_wem_info</c> and the <c>DCA_WEM_VARIANT_*</c>
/// values. Behaviour lives in <c>Wem/WemDecoderTests</c>; this pins the contract.
/// </summary>
public sealed class AbiV42Tests
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    public static TheoryData<string> Exports() => new()
    {
        "dca_wem_open_file", "dca_wem_open_memory", "dca_wem_get_info", "dca_wem_read_f32",
        "dca_wem_read_s16", "dca_wem_seek", "dca_wem_destroy",
    };

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 42u, "the dca_wem_* exports predate the current version");
    }

    [Theory]
    [MemberData(nameof(Exports))]
    public void EveryV42Export_IsInTheHeaderAndTheDll(string export)
    {
        Assert.Contains($"DCA_CALL {export}(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr), $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    /// <summary>The managed enum's ordinals are the header's, read off the header.</summary>
    [Fact]
    public void TheVariantOrdinals_MatchTheHeader()
    {
        var header = Header();
        foreach (var (name, value) in new (string, JuceWemVariant)[]
                 {
                     ("AUTO", JuceWemVariant.Auto), ("AOTUV", JuceWemVariant.AoTuV),
                     ("DEFAULT", JuceWemVariant.Default), ("INLINE", JuceWemVariant.Inline),
                     ("DEFAULT_MOD", JuceWemVariant.DefaultMod), ("DEFAULT_STANDARD", JuceWemVariant.DefaultStandard),
                     ("HEADER_TRIAD", JuceWemVariant.HeaderTriad),
                 })
        {
            var m = Regex.Match(header, @"#define\s+DCA_WEM_VARIANT_" + name + @"\s+(\d+)");
            Assert.True(m.Success, $"DCA_WEM_VARIANT_{name} is missing from the header");
            Assert.Equal((int)value, int.Parse(m.Groups[1].Value));
        }
        Assert.Equal(7, Enum.GetValues<JuceWemVariant>().Length);
    }

    /// <summary>dca_wem_info is 48 bytes on both sides, field for field.</summary>
    [Fact]
    public void TheInfoStruct_Is48Bytes()
    {
        Assert.Equal(48, Marshal.SizeOf<NativeMethods.DcaWemInfo>());
        Assert.Equal(8, (int)Marshal.OffsetOf<NativeMethods.DcaWemInfo>(nameof(NativeMethods.DcaWemInfo.Frames)));
        Assert.Equal(32, (int)Marshal.OffsetOf<NativeMethods.DcaWemInfo>(nameof(NativeMethods.DcaWemInfo.Variant)));
        Assert.Matches(@"\}\s*dca_wem_info;\s*//\s*48 bytes", Header());
    }

    /// <summary>The engine registers the WEM format: a source can open a .wem.</summary>
    [Fact]
    public void TheEngineFormatManager_RegistersTheWemFormat()
    {
        var src = File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/src/AudioEngine.cpp"));
        Assert.Contains("formatManager_.registerFormat(new dcwem::WemAudioFormat(), false);", src, StringComparison.Ordinal);
        var cmake = File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/CMakeLists.txt"));
        Assert.Contains("src/wem/WemVorbisDecoder.cpp", cmake, StringComparison.Ordinal);
        Assert.Contains("src/wem/WemAudioFormat.cpp", cmake, StringComparison.Ordinal);
    }
}
