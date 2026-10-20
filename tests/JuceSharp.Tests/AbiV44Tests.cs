using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v44 (DotCharter #317): Wwise-free Wwise Vorbis encoding.
/// <c>dca_wem_encode_vorbis</c> returns a natively allocated .wem that
/// <c>dca_wem_encode_free</c> releases. The behaviour is pinned in
/// <c>Wem/WemEncoderTests</c>; this class pins the ABI surface.
/// </summary>
public sealed class AbiV44Tests
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    public static TheoryData<string> Exports() => new() { "dca_wem_encode_vorbis", "dca_wem_encode_free" };

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 44u, "the v44 exports predate the current version");
    }

    [Theory]
    [MemberData(nameof(Exports))]
    public void EveryV44Export_IsInTheHeaderAndTheDll(string export)
    {
        Assert.Contains($"DCA_CALL {export}(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr), $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    /// <summary>The native boundary refuses what the managed wrapper refuses, and
    /// zeroes both outs on every refusal.</summary>
    [NativeAvailableFact]
    public unsafe void TheEncodeExport_RefusesBadArguments_AndZeroesItsOuts()
    {
        var pcm = new float[2 * 4800];
        fixed (float* p = pcm)
        {
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, Encode(p, 4800, 2, 44100, out var d1, out var s1));
            Assert.Equal((nint.Zero, 0L), (d1, s1));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, Encode(p, 4800, 1, 48000, out _, out _));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, Encode(p, 0, 2, 48000, out _, out _));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, Encode(null, 4800, 2, 48000, out _, out _));

            Assert.Equal(NativeMethods.DcaStatus.Ok, Encode(p, 4800, 2, 48000, out var data, out var size));
            Assert.NotEqual(nint.Zero, data);
            Assert.True(size > 0);
            NativeMethods.WemEncodeFree(data);
        }

        NativeMethods.WemEncodeFree(nint.Zero);   // a no-op
    }

    private static unsafe NativeMethods.DcaStatus Encode(float* p, long frames, int channels, int rate, out nint data, out long size)
    {
        data = -1;
        size = -1;
        return NativeMethods.WemEncodeVorbis(p, frames, channels, rate, out data, out size);
    }
}
