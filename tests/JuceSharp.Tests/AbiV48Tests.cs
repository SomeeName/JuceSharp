using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v48: <c>dca_engine_chart_source_set_notes_for_player2</c>, the note upload
/// that also names the BYTE LENGTH of the notes buffer. The v38 export takes a
/// count and a stride and can only trust that the caller sized the buffer for
/// them; this one checks <c>count * stride &lt;= notes_byte_length</c> (in 64 bits)
/// and rejects the list whole otherwise. <c>JuceAudioEngine.SetChartNotes</c>
/// calls it.
/// </summary>
[Collection("NullDevice")]
public sealed class AbiV48Tests : IDisposable
{
    private readonly NullDeviceScope? _scope;
    private readonly JuceAudioEngine? _engine;

    public AbiV48Tests()
    {
        if (!NativeProbe.IsAvailable) return;
        _scope = new NullDeviceScope();
        _engine = NullDevice.CreateEngine(inputChannels: 1);
    }

    public void Dispose()
    {
        _engine?.Dispose();
        _scope?.Dispose();
    }

    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 48u, "the byte-length note upload predates the current version");
    }

    [Fact]
    public void TheNewExport_IsDeclaredAndTheOldOneIsUnchanged()
    {
        var h = Header();
        Assert.Contains("DCA_CALL dca_engine_chart_source_set_notes_for_player2(", h);
        // The v38 signature is never changed: still seven parameters, no length.
        var v38 = Regex.Match(h, @"DCA_CALL dca_engine_chart_source_set_notes_for_player\((.*?)\);",
            RegexOptions.Singleline);
        Assert.True(v38.Success);
        Assert.DoesNotContain("byte_length", v38.Groups[1].Value);
        Assert.Equal(7, v38.Groups[1].Value.Split(',').Length);
    }

    [Fact]
    public void TheNewExport_IsInTheBuiltDll()
    {
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, "dca_engine_chart_source_set_notes_for_player2", out var addr),
            "dca_engine_chart_source_set_notes_for_player2 is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    [NativeAvailableFact]
    public unsafe void ANullEngine_AndANegativeLength_AreRejected()
    {
        var notes = ChartSourceNullDeviceTests.Notes(2);
        var native = MemoryMarshal.Cast<JuceChartNote, NativeMethods.DcaChartNote>(notes);
        var stride = Unsafe.SizeOf<NativeMethods.DcaChartNote>();
        fixed (NativeMethods.DcaChartNote* p = native)
        {
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineChartSourceSetNotesForPlayer2(
                nint.Zero, 0, p, native.Length, stride, (long)native.Length * stride, null, 0));
            var h = _engine!.Acquire();
            try
            {
                Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineChartSourceSetNotesForPlayer2(
                    h, 0, p, native.Length, stride, -1, null, 0));
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>
    /// The count claims one note more than the buffer holds: rejected, and the
    /// table that was there stays. The exact length is accepted, so the check
    /// is the length and nothing else.
    /// </summary>
    [NativeAvailableFact]
    public unsafe void ACountThatOverrunsTheByteLength_IsRejectedWhole_AndTheExactLengthIsAccepted()
    {
        var notes = ChartSourceNullDeviceTests.Notes(4);
        var native = MemoryMarshal.Cast<JuceChartNote, NativeMethods.DcaChartNote>(notes);
        var stride = Unsafe.SizeOf<NativeMethods.DcaChartNote>();
        var h = _engine!.Acquire();
        try
        {
            fixed (NativeMethods.DcaChartNote* p = native)
            {
                Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineChartSourceSetNotesForPlayer2(
                    h, 0, p, 3, stride, 3L * stride, null, 0));
                // Four notes claimed, the buffer said to hold three.
                Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.EngineChartSourceSetNotesForPlayer2(
                    h, 0, p, 4, stride, 3L * stride, null, 0));
            }
        }
        finally { _engine.Release(); }

        Assert.True(_engine.TryGetChartSourceStatus(0, out var st));
        Assert.Equal(3, st.NoteCount);
    }

    /// <summary>The wrapper goes through the new export, with the real length.</summary>
    [NativeAvailableFact]
    public void SetChartNotes_UsesTheByteLengthExport()
    {
        _engine!.SetChartNotes(0, ChartSourceNullDeviceTests.Notes(5), ReadOnlySpan<JuceChartBendPoint>.Empty);
        Assert.True(_engine.TryGetChartSourceStatus(0, out var st));
        Assert.Equal(5, st.NoteCount);
        var wrapper = File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp/JuceAudioEngine.ChartSource.cs"));
        Assert.Contains("EngineChartSourceSetNotesForPlayer2(", wrapper);
    }
}
