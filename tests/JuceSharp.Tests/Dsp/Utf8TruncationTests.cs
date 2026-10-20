using System.Text;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// Every string crossing this ABI is UTF-8 and every managed reader decodes it
/// as UTF-8, so a fixed-size out buffer that ends in the middle of a multi-byte
/// sequence does not return a slightly-short string — it returns one with
/// U+FFFD where the torn bytes were.
/// <para>
/// The case that actually bites is <c>dca_device_info</c>: the SAME device name
/// is written into <c>id[128]</c> and <c>name[256]</c>, so a name whose 127th
/// byte lands mid-sequence tears in the id and not in the name. The two then
/// disagree, and selecting the device by the id it was enumerated under falls
/// through to the fallback walk — the same failure the <c>LPStr</c> mojibake
/// bug produced, from the other end. Non-ASCII device names are the norm on a
/// localized Windows.
/// </para>
/// <para>
/// A device with such a name cannot be conjured in a test, but both ABI
/// string-out helpers (<c>copyTruncated</c> over <c>juce::String</c>,
/// <c>copyStdTruncated</c> over <c>std::string</c>) now go through the one
/// <c>dcabi::utf8TruncatedLength</c>, and <c>dca_engine_dsp_get_node_string</c>
/// drives the second of them with a caller-chosen capacity — so every cut
/// position through a real export is reachable from here.
/// </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class Utf8TruncationTests : IDisposable
{
    // Deliberately mixes 1-, 2-, 3- and 4-byte code points so that cutting at
    // most byte offsets lands INSIDE a sequence: ASCII, Latin-1 supplement
    // (2 bytes), katakana (3), and an emoji outside the BMP (4).
    private const string Mixed = "aäéミクは\U0001F3B8 ok";

    private readonly JuceAudioEngine? _engine;

    public Utf8TruncationTests()
    {
        if (NativeProbe.IsAvailable)
            _engine = new JuceAudioEngine();
    }

    public void Dispose() => _engine?.Dispose();

    [NativeAvailableFact]
    public void EveryTruncationOfAUtf8String_IsStillValidUtf8AndAPrefix()
    {
        var h = _engine!.NativeHandle;
        var node = DspAddNode(h, "miku");
        Assert.True(node >= 0);

        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.EngineDspSetNodeString(h, node, "phrases", Mixed));

        var full = NativeMethods.EngineDspGetNodeString(h, node, "phrases", null, 0);
        var expected = Encoding.UTF8.GetBytes(Mixed);
        Assert.Equal(expected.Length, full);

        // A strict decoder throws on a torn sequence instead of quietly
        // substituting U+FFFD, which is exactly the distinction under test:
        // Encoding.UTF8.GetString would have "passed" against the old code.
        var strict = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

        var sawAShortenedResult = false;
        for (var cap = 1; cap <= expected.Length + 2; ++cap)
        {
            var buf = new byte[cap];
            for (var i = 0; i < cap; ++i) buf[i] = 0xAB;   // guard: NUL must be written

            var reported = NativeMethods.EngineDspGetNodeString(h, node, "phrases", buf, cap);
            Assert.Equal(expected.Length, reported);       // always the FULL byte length

            var nul = Array.IndexOf(buf, (byte) 0);
            Assert.True(nul >= 0, $"capacity {cap}: the buffer was not NUL-terminated");
            Assert.True(nul <= cap - 1, $"capacity {cap}: NUL at {nul} is past the buffer");

            var got = strict.GetString(buf, 0, nul);       // throws on a torn sequence
            Assert.StartsWith(got, Mixed, StringComparison.Ordinal);
            if (nul < expected.Length) sawAShortenedResult = true;
        }

        Assert.True(sawAShortenedResult,
            "no capacity actually truncated anything — the probe never exercised a cut");
    }

    /// <summary>
    /// The probe above is only meaningful if the string it uses really does put
    /// multi-byte sequences across most cut positions. If this ever reads 0,
    /// the constant was flattened to ASCII and the test above is vacuous.
    /// </summary>
    [Fact]
    public void TheProbeString_PutsSequencesAcrossMostCuts()
    {
        var bytes = Encoding.UTF8.GetBytes(Mixed);
        var interior = bytes.Count(b => (b & 0xC0) == 0x80);   // continuation bytes
        Assert.True(interior >= 8,
            $"only {interior} of {bytes.Length} bytes are UTF-8 continuation bytes; a cut "
            + "would rarely land inside a sequence and the truncation probe proves little");
    }

    [System.Runtime.InteropServices.DllImport("juce_sharp",
        EntryPoint = "dca_engine_dsp_add_node",
        CallingConvention = System.Runtime.InteropServices.CallingConvention.Cdecl)]
    private static extern int DspAddNode(nint engine,
        [System.Runtime.InteropServices.MarshalAs(
            System.Runtime.InteropServices.UnmanagedType.LPUTF8Str)] string typeId);
}
