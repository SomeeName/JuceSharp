using System.Runtime.InteropServices;
using System.Text;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// ABI v23 split/parallel-chain surface: the splitter/merger catalogue
/// entries and the per-node branch-tag round trip on a real engine. The node
/// add/move/clear plumbing (owned by the parent repo's DspNativeMethods) is
/// declared test-locally below, same pattern as <see cref="DspStringParamTests"/>.
/// The audio-path numerics (null test, branch isolation, implicit merge) are
/// covered by the native-side harness; these tests pin the ABI contract.
/// </summary>
[Collection("NativeEngine")]
public sealed class DspSplitChainTests : IDisposable
{
    private const string Lib = "juce_sharp";

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_add_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAddNode(nint engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_clear", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspClear(nint engine);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_move_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspMoveNode(nint engine, int nodeId, int newIndex);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, float value);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_get_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspGetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, out float value);

    [DllImport(Lib, EntryPoint = "dca_dsp_module_param_count", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspModuleParamCount([MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport(Lib, EntryPoint = "dca_dsp_module_param_info", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspModuleParamInfo([MarshalAs(UnmanagedType.LPUTF8Str)] string typeId, int index,
        [Out] byte[]? outId, int idCap, [Out] byte[]? outName, int nameCap,
        out float min, out float max, out float def);

    private readonly JuceAudioEngine? _engine;

    public DspSplitChainTests()
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

    [NativeAvailableFact]
    public void SplitterAndMerger_AreInTheCatalogue()
    {
        Assert.Equal(1, DspModuleParamCount("splitter"));
        Assert.Equal(13, DspModuleParamCount("merger"));

        var id = new byte[64];
        var name = new byte[64];
        Assert.Equal(0, DspModuleParamInfo("splitter", 0, id, id.Length, name, name.Length,
            out var min, out var max, out var def));
        Assert.Equal("enabledBranches", Cstr(id));
        Assert.Equal(2.0f, min);
        Assert.Equal(3.0f, max);
        Assert.Equal(2.0f, def);

        // merger param 0/1 pin the level/pan id scheme the managed mirror uses
        Assert.Equal(0, DspModuleParamInfo("merger", 0, id, id.Length, name, name.Length, out min, out max, out def));
        Assert.Equal("level1", Cstr(id));
        Assert.Equal(1.0f, def);
        Assert.Equal(0, DspModuleParamInfo("merger", 1, id, id.Length, name, name.Length, out min, out max, out def));
        Assert.Equal("pan1", Cstr(id));
        Assert.Equal(-1.0f, min);
        Assert.Equal(0.0f, def);
        Assert.Equal(0, DspModuleParamInfo("merger", 6, id, id.Length, name, name.Length, out min, out max, out def));
        Assert.Equal("mode", Cstr(id));
        Assert.Equal(0.0f, def); // average by default => unity null test
    }

    [NativeAvailableFact]
    public void NodeBranch_RoundTrips_AndValidates()
    {
        var h = _engine!.NativeHandle;
        var split = DspAddNode(h, "splitter");
        var fx = DspAddNode(h, "gain");
        var merge = DspAddNode(h, "merger");
        Assert.True(split >= 0 && fx >= 0 && merge >= 0);

        // default tag is 0 (= branch 1 inside a region)
        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDspGetNodeBranch(h, fx, out var branch));
        Assert.Equal(0, branch);

        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDspSetNodeBranch(h, fx, 2));
        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDspGetNodeBranch(h, fx, out branch));
        Assert.Equal(2, branch);

        // out-of-range and unknown-node rejection
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineDspSetNodeBranch(h, fx, 4));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineDspSetNodeBranch(h, fx, -1));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineDspSetNodeBranch(h, 987654, 1));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineDspGetNodeBranch(h, 987654, out _));

        // rejected set didn't clobber the tag
        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDspGetNodeBranch(h, fx, out branch));
        Assert.Equal(2, branch);
    }

    [NativeAvailableFact]
    public void NodeBranch_SurvivesReorder()
    {
        var h = _engine!.NativeHandle;
        var split = DspAddNode(h, "splitter");
        var a = DspAddNode(h, "gain");
        var b = DspAddNode(h, "eq");
        var merge = DspAddNode(h, "merger");
        Assert.True(split >= 0 && a >= 0 && b >= 0 && merge >= 0);

        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDspSetNodeBranch(h, b, 2));
        Assert.Equal(0, DspMoveNode(h, b, 1)); // swap a and b inside the region

        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDspGetNodeBranch(h, b, out var branch));
        Assert.Equal(2, branch);
    }

    [NativeAvailableFact]
    public void SplitterParams_ClampToSupportedBranchCount()
    {
        var h = _engine!.NativeHandle;
        var split = DspAddNode(h, "splitter");
        Assert.True(split >= 0);

        Assert.Equal(0, DspGetParam(h, split, "enabledBranches", out var branches));
        Assert.Equal(2.0f, branches);

        Assert.Equal(0, DspSetParam(h, split, "enabledBranches", 3.0f));
        Assert.Equal(0, DspGetParam(h, split, "enabledBranches", out branches));
        Assert.Equal(3.0f, branches);

        // out-of-range writes clamp instead of failing (module contract)
        Assert.Equal(0, DspSetParam(h, split, "enabledBranches", 99.0f));
        Assert.Equal(0, DspGetParam(h, split, "enabledBranches", out branches));
        Assert.Equal(3.0f, branches);
    }

    private static string Cstr(byte[] buf)
    {
        var n = Array.IndexOf(buf, (byte)0);
        return Encoding.UTF8.GetString(buf, 0, n < 0 ? buf.Length : n);
    }
}
