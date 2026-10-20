using System.Runtime.InteropServices;
using System.Text;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// ABI v22 string-parameter surface: set/get round-trips on real engine
/// nodes (cab irPath, miku phrases, namamp modelPath) plus the static
/// string-param catalogue. Uses the wrapper's new P/Invokes; the node
/// add/clear plumbing (owned by the parent repo's DspNativeMethods) is
/// declared test-locally below.
/// </summary>
[Collection("NativeEngine")]
public sealed class DspStringParamTests : IDisposable
{
    private const string Lib = "juce_sharp";

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_add_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAddNode(nint engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_clear", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspClear(nint engine);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_get_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspGetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, out float value);

    private readonly JuceAudioEngine? _engine;

    public DspStringParamTests()
    {
        if (NativeProbe.IsAvailable)
            _engine = new JuceAudioEngine();
    }

    public void Dispose()
    {
        if (_engine is not null)
        {
            DspClear(_engine.NativeHandle);
            _engine.Dispose();
        }
    }

    private static string? GetNodeString(nint engine, int nodeId, string paramId)
    {
        var len = NativeMethods.EngineDspGetNodeString(engine, nodeId, paramId, null, 0);
        if (len < 0) return null;
        var buf = new byte[len + 1];
        NativeMethods.EngineDspGetNodeString(engine, nodeId, paramId, buf, buf.Length);
        return Encoding.UTF8.GetString(buf, 0, len);
    }

    [NativeAvailableFact]
    public void CabIrPath_RoundTrips_AndEmptyReverts()
    {
        var h = _engine!.NativeHandle;
        var node = DspAddNode(h, "cab");
        Assert.True(node >= 0);

        // default: no user IR
        Assert.Equal(string.Empty, GetNodeString(h, node, "irPath"));

        var wav = TinyWav.Create(0.05);
        try
        {
            var status = NativeMethods.EngineDspSetNodeString(h, node, "irPath", wav);
            Assert.Equal(NativeMethods.DcaStatus.Ok, status);
            Assert.Equal(wav, GetNodeString(h, node, "irPath"));

            // empty reverts to the bundled bank
            status = NativeMethods.EngineDspSetNodeString(h, node, "irPath", "");
            Assert.Equal(NativeMethods.DcaStatus.Ok, status);
            Assert.Equal(string.Empty, GetNodeString(h, node, "irPath"));
        }
        finally
        {
            TinyWav.TryDelete(wav);
        }
    }

    [NativeAvailableFact]
    public void CabIrPath_BadFile_Rejected_KeepsPreviousValue()
    {
        var h = _engine!.NativeHandle;
        var node = DspAddNode(h, "cab");
        Assert.True(node >= 0);

        var status = NativeMethods.EngineDspSetNodeString(h, node, "irPath",
            Path.Combine(Path.GetTempPath(), "definitely-not-here-" + Guid.NewGuid().ToString("N") + ".wav"));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, status);
        Assert.Equal(string.Empty, GetNodeString(h, node, "irPath"));
    }

    [NativeAvailableFact]
    public void MikuPhrases_RoundTrip_IncludingUnicode()
    {
        var h = _engine!.NativeHandle;
        var node = DspAddNode(h, "miku");
        Assert.True(node >= 0);

        const string phrases = "mi ku mi ku da-yo ミク";
        var status = NativeMethods.EngineDspSetNodeString(h, node, "phrases", phrases);
        Assert.Equal(NativeMethods.DcaStatus.Ok, status);
        Assert.Equal(phrases, GetNodeString(h, node, "phrases"));

        // sized-getter contract: byte length (UTF-8), not char count
        var expectedBytes = Encoding.UTF8.GetByteCount(phrases);
        var len = NativeMethods.EngineDspGetNodeString(h, node, "phrases", null, 0);
        Assert.Equal(expectedBytes, len);

        // truncation stays NUL-terminated and returns the FULL length
        var tiny = new byte[4];
        len = NativeMethods.EngineDspGetNodeString(h, node, "phrases", tiny, tiny.Length);
        Assert.Equal(expectedBytes, len);
        Assert.Equal(0, tiny[3]);
    }

    [NativeAvailableFact]
    public void NamAmp_ModelPath_RoundTrips_AndReportsStatusParams()
    {
        var h = _engine!.NativeHandle;
        var node = DspAddNode(h, "namamp");
        Assert.True(node >= 0); // namamp must be compiled into the shipped DLL

        // async load of a nonexistent capture: path is accepted (load result
        // is reported via the poll-only "loaded" param, which stays 0)
        var fake = Path.Combine(Path.GetTempPath(), "nope-" + Guid.NewGuid().ToString("N") + ".nam");
        var status = NativeMethods.EngineDspSetNodeString(h, node, "modelPath", fake);
        Assert.Equal(NativeMethods.DcaStatus.Ok, status);
        Assert.Equal(fake, GetNodeString(h, node, "modelPath"));

        Assert.Equal(0, DspGetParam(h, node, "loaded", out var loaded));
        Assert.Equal(0f, loaded);
        Assert.Equal(0, DspGetParam(h, node, "rateMismatch", out _));

        // empty clears
        status = NativeMethods.EngineDspSetNodeString(h, node, "modelPath", "");
        Assert.Equal(NativeMethods.DcaStatus.Ok, status);
        Assert.Equal(string.Empty, GetNodeString(h, node, "modelPath"));
    }

    [NativeAvailableFact]
    public void UnknownParamOrNode_ReturnsErrors()
    {
        var h = _engine!.NativeHandle;
        var node = DspAddNode(h, "gain");
        Assert.True(node >= 0);

        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineDspSetNodeString(h, node, "irPath", "x")); // gain has no string params
        Assert.Equal(-1, NativeMethods.EngineDspGetNodeString(h, node, "irPath", null, 0));
        Assert.Equal(-1, NativeMethods.EngineDspGetNodeString(h, 987654, "irPath", null, 0));
    }

    [NativeAvailableFact]
    public void StringParamCatalogue_DescribesFilePickers()
    {
        Assert.Equal(2, NativeMethods.DspModuleStringParamCount("cab"));
        Assert.Equal(1, NativeMethods.DspModuleStringParamCount("acousticsim"));
        Assert.Equal(1, NativeMethods.DspModuleStringParamCount("miku"));
        Assert.Equal(1, NativeMethods.DspModuleStringParamCount("namamp"));
        Assert.Equal(0, NativeMethods.DspModuleStringParamCount("gain"));
        Assert.Equal(-1, NativeMethods.DspModuleStringParamCount("no-such-module"));

        var id = new byte[64];
        var name = new byte[64];
        var filter = new byte[64];
        var status = NativeMethods.DspModuleStringParamInfo("namamp", 0, id, id.Length, name, name.Length, filter, filter.Length);
        Assert.Equal(NativeMethods.DcaStatus.Ok, status);
        Assert.Equal("modelPath", Cstr(id));
        Assert.Equal("*.nam;*.aidax", Cstr(filter));

        status = NativeMethods.DspModuleStringParamInfo("cab", 0, id, id.Length, name, name.Length, filter, filter.Length);
        Assert.Equal(NativeMethods.DcaStatus.Ok, status);
        Assert.Equal("irPath", Cstr(id));
        Assert.Equal("*.wav", Cstr(filter));

        status = NativeMethods.DspModuleStringParamInfo("cab", 1, id, id.Length, name, name.Length, filter, filter.Length);
        Assert.Equal(NativeMethods.DcaStatus.Ok, status);
        Assert.Equal("irPathB", Cstr(id));
        Assert.Equal("*.wav", Cstr(filter));

        status = NativeMethods.DspModuleStringParamInfo("miku", 0, id, id.Length, name, name.Length, filter, filter.Length);
        Assert.Equal(NativeMethods.DcaStatus.Ok, status);
        Assert.Equal("phrases", Cstr(id));
        Assert.Equal(string.Empty, Cstr(filter)); // free text, no picker

        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.DspModuleStringParamInfo("cab", 2, id, id.Length, name, name.Length, filter, filter.Length));
    }

    private static string Cstr(byte[] buf)
    {
        var n = Array.IndexOf(buf, (byte)0);
        return Encoding.UTF8.GetString(buf, 0, n < 0 ? buf.Length : n);
    }
}
