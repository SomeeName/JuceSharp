using System.Runtime.InteropServices;
using System.Text;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// 2026-07-16 audit finding #3: <c>DcaDeviceInfo</c> / <c>DcaEngineConfig</c>
/// were marshalled CharSet.Ansi (system codepage) while the native side writes
/// and reads the embedded char[] fields as UTF-8. These tests pin the fixed
/// UTF-8 marshalling: byte-identical struct layout to the C header, and
/// lossless round-trips for non-ASCII device names (de-DE is a shipped
/// locale). Pure managed — no native DLL required.
/// </summary>
public sealed class Utf8InteropTests
{
    // Layouts are ABI-pinned by dotcharter_audio.h (char id[128]; char
    // name[256]; 5x int32; char type_name[64] == 468 bytes, and the 412-byte
    // config). A size change here means the native struct no longer matches.
    [Fact]
    public void DcaDeviceInfo_LayoutMatchesNativeHeader()
    {
        Assert.Equal(468, Marshal.SizeOf<NativeMethods.DcaDeviceInfo>());
        Assert.Equal(384, (int)Marshal.OffsetOf<NativeMethods.DcaDeviceInfo>("Driver"));
        Assert.Equal(400, (int)Marshal.OffsetOf<NativeMethods.DcaDeviceInfo>("IsDefault"));
    }

    [Fact]
    public void DcaEngineConfig_LayoutMatchesNativeHeader()
    {
        Assert.Equal(412, Marshal.SizeOf<NativeMethods.DcaEngineConfig>());
        Assert.Equal(24, (int)Marshal.OffsetOf<NativeMethods.DcaEngineConfig>("PreferredDriver"));
    }

    [Fact]
    public void DeviceInfo_NonAsciiName_RoundTripsLosslessly()
    {
        // A realistic localized endpoint name on a German Windows install.
        const string name = "Lautsprecher (Übungs-Gerät) ♪";
        var info = default(NativeMethods.DcaDeviceInfo);
        info.Id = name;
        info.Name = name;
        info.TypeName = "Windows Audio (Exklusiver Modus)";
        Assert.Equal(name, info.Id);
        Assert.Equal(name, info.Name);
        Assert.Equal("Windows Audio (Exklusiver Modus)", info.TypeName);
    }

    [Fact]
    public unsafe void DeviceInfo_DecodesRawUtf8BytesFromNative()
    {
        // Simulate exactly what native copyTruncated writes: raw UTF-8 bytes
        // plus a NUL. The old ANSI marshalling read these through the system
        // codepage and produced mojibake for every non-ASCII character.
        const string name = "Микрофон (Übung) 音声";
        var bytes = Encoding.UTF8.GetBytes(name);
        var info = default(NativeMethods.DcaDeviceInfo);
        for (var i = 0; i < bytes.Length; i++) info.NameUtf8[i] = bytes[i];
        info.NameUtf8[bytes.Length] = 0;

        Assert.Equal(name, info.Name);
    }

    [Fact]
    public unsafe void EngineConfig_WritesUtf8BytesTheNativeSideExpects()
    {
        // fromUTF8 on the native side must see the UTF-8 encoding of the id.
        const string id = "Übungs-Interface";
        var cfg = default(NativeMethods.DcaEngineConfig);
        cfg.OutputDeviceId = id;

        var expected = Encoding.UTF8.GetBytes(id);
        for (var i = 0; i < expected.Length; i++)
            Assert.Equal(expected[i], cfg.OutputDeviceIdUtf8[i]);
        Assert.Equal(0, cfg.OutputDeviceIdUtf8[expected.Length]); // NUL-terminated
        Assert.Equal(id, cfg.OutputDeviceId);
    }

    [Fact]
    public void EngineConfig_OverlongValue_TruncatesAtACharacterBoundary()
    {
        // 127 bytes of capacity-1; 'ü' is 2 bytes in UTF-8, so a string of 64
        // ü characters (128 bytes) must cut after 63 of them, never mid-char.
        var cfg = default(NativeMethods.DcaEngineConfig);
        cfg.OutputDeviceId = new string('ü', 64);
        Assert.Equal(new string('ü', 63), cfg.OutputDeviceId);
    }

    [Fact]
    public void EngineConfig_EmptyAndNullClearTheField()
    {
        var cfg = default(NativeMethods.DcaEngineConfig);
        cfg.InputDeviceId = "something";
        cfg.InputDeviceId = "";
        Assert.Equal(string.Empty, cfg.InputDeviceId);
    }

    [Fact]
    public void JuceEngineConfig_ToNative_CarriesNonAsciiIds()
    {
        var cfg = new JuceEngineConfig
        {
            OutputDeviceId = "Kopfhörer (Realtek® Audio)",
            OutputTypeName = "Windows Audio (exklusiv)",
        };
        var n = cfg.ToNative();
        Assert.Equal("Kopfhörer (Realtek® Audio)", n.OutputDeviceId);
        Assert.Equal("Windows Audio (exklusiv)", n.OutputTypeName);
    }

    /// <summary>
    /// <c>dca_stems_describe_backend</c> was the last hand-rolled
    /// <c>memcpy</c> + terminator in the ABI; it now goes through
    /// <c>copyStdTruncated</c> like every other fixed out buffer, so
    /// <c>dcabi::utf8TruncatedLength</c> is genuinely the single
    /// implementation.
    /// <para>
    /// HONEST LIMIT: the backend description is ASCII today, and for ASCII the
    /// old byte-wise cut and the code-point-aware one are identical — this test
    /// CANNOT tell them apart, and no managed caller can make the export return
    /// a multibyte string. <c>Utf8TruncationTests</c> covers the boundary
    /// behaviour through the exports that can. What this pins is the part that
    /// was uncovered entirely: the probe/terminator/size contract, which a
    /// re-hand-rolled copy would be free to get wrong.
    /// </para>
    /// </summary>
    [NativeAvailableFact]
    public void StemsDescribeBackend_HonoursItsProbeAndTerminatorContract()
    {
        // Probing call: capacity 0 with no buffer must still report the FULL
        // size including the terminator.
        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.StemsDescribeBackend(null, 0, out var required));
        Assert.True(required > 1, $"backend description reported {required} bytes");

        // An exact-size buffer round-trips whole.
        var exact = new byte[required];
        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.StemsDescribeBackend(exact, exact.Length, out var again));
        Assert.Equal(required, again);
        Assert.Equal(0, exact[required - 1]);
        var full = Encoding.UTF8.GetString(exact, 0, required - 1);
        Assert.NotEmpty(full);

        // Every short capacity yields a NUL-terminated prefix of it, never an
        // unterminated buffer and never a write past `capacity`.
        for (var cap = 1; cap < required; cap++)
        {
            var buf = new byte[cap + 4];
            for (var i = 0; i < buf.Length; i++) buf[i] = 0xAB; // guard band
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.StemsDescribeBackend(buf, cap, out var stillRequired));
            Assert.Equal(required, stillRequired);

            var nul = Array.IndexOf<byte>(buf, 0, 0, cap);
            Assert.True(nul >= 0, $"capacity {cap} produced no terminator inside the buffer");
            Assert.Equal(full[..nul], Encoding.UTF8.GetString(buf, 0, nul));
            for (var i = cap; i < buf.Length; i++)
                Assert.Equal(0xAB, buf[i]); // nothing written past `capacity`
        }
    }
}
