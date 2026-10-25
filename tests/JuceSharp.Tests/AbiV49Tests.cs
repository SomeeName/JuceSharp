using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v49: <c>dca_engine_get_channel_names</c>, the open device's channel names, so a
/// host can label an output pair or an input jack the way the driver does. The fold of
/// L and R into a one-output device is native-only and is pinned by
/// <c>RtMonoOutputFoldsLAndRWithoutAllocating</c> in the RT gate.
/// </summary>
[Collection("NullDevice")]
public sealed class AbiV49Tests
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    private static JuceAudioEngine NullEngine() => new(new JuceEngineConfig
    {
        OutputTypeName = NullDevice.TypeName,
        SampleRate = 48000,
        BufferFrames = 480,
        InputChannels = 2,
        EnableNullDevice = true,
        NullDeviceManualPump = true,
        EnableConsoleLog = false,
    });

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 49u, "dca_engine_get_channel_names predates the current version");
    }

    [Theory]
    [InlineData("dca_engine_get_channel_names")]
    public void TheV49Export_IsInTheHeaderAndTheDll(string export)
    {
        Assert.Contains($"DCA_CALL {export}(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, export, out var addr), $"{export} is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    [NativeAvailableFact]
    public void ANullEngine_IsAnInvalidArgument_AndTheCountIsZeroed()
    {
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineGetChannelNames(nint.Zero, 0, null, null, 0, out var count));
        Assert.Equal(0, count);
    }

    [NativeAvailableFact]
    public void ANegativeCapacity_OrANullBufferWithACapacity_IsAnInvalidArgument()
    {
        using var engine = NullEngine();
        var h = engine.NativeHandle;
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineGetChannelNames(h, 0, null, null, -1, out var a));
        Assert.Equal(0, a);
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineGetChannelNames(h, 0, null, null, 4, out var b));
        Assert.Equal(0, b);
    }

    /// <summary>The Null device names its channels "Null Out N" / "Null In N"; the list
    /// comes back in physical order on both sides, with the open device named or not.</summary>
    [NativeAvailableFact]
    public void TheOpenDevice_NamesItsOutputsAndInputs_InPhysicalOrder()
    {
        using var engine = NullEngine();
        var outs = engine.GetChannelNames(isInput: false);
        Assert.True(outs.Count >= 2, $"the Null device reported {outs.Count} output names");
        Assert.Equal(Enumerable.Range(1, outs.Count).Select(i => $"Null Out {i}"), outs);
        Assert.Equal(outs, engine.GetChannelNames(isInput: false, NullDevice.DeviceName));

        var ins = engine.GetChannelNames(isInput: true);
        Assert.True(ins.Count >= 2, $"the Null device reported {ins.Count} input names");
        Assert.Equal(Enumerable.Range(1, ins.Count).Select(i => $"Null In {i}"), ins);
    }

    /// <summary>Another device's name gets no list: a picker must never label one
    /// device's jacks with the open device's names.</summary>
    [NativeAvailableFact]
    public void ADeviceThatIsNotOpen_GetsNoNames()
    {
        using var engine = NullEngine();
        Assert.Empty(engine.GetChannelNames(isInput: false, "Some Other Interface"));
        Assert.Empty(engine.GetChannelNames(isInput: true, "Some Other Interface"));
    }

    /// <summary>The count is the device's total even when the buffer holds fewer, and
    /// only the entries that fit are written.</summary>
    [NativeAvailableFact]
    public void ASmallBuffer_GetsTheTotalCount_AndOnlyTheEntriesThatFit()
    {
        using var engine = NullEngine();
        var total = engine.GetChannelNames(isInput: false).Count;
        var buf = new byte[NativeMethods.ChannelNameBytes];
        Assert.Equal(NativeMethods.DcaStatus.Ok,
            NativeMethods.EngineGetChannelNames(engine.NativeHandle, 0, null, buf, 1, out var count));
        Assert.Equal(total, count);
        var len = Array.IndexOf(buf, (byte)0);
        Assert.Equal("Null Out 1", System.Text.Encoding.UTF8.GetString(buf, 0, len));
    }

    [NativeAvailableFact]
    public void ADisposedEngine_ReturnsNoNames()
    {
        var engine = NullEngine();
        engine.Dispose();
        Assert.Empty(engine.GetChannelNames(isInput: false));
    }
}
