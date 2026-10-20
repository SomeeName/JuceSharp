using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v39 (#51): <c>dca_chart_note</c> carries the arrangement's capo, in the first
/// reserved slot, so a voice can measure natural-harmonic nodes from it. The struct
/// stays 80 bytes and no export changed.
/// </summary>
public sealed class AbiV39Tests
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 39u, "the capo field predates the current version");
    }

    [Fact]
    public void TheCapo_SitsAtOffset64_AndTheNoteIsStill80Bytes()
    {
        Assert.Single(Regex.Matches(Header(), @"int32_t\s+capo;"));
        Assert.Matches(@"float\s+reserved\[3\];", Header());
        Assert.Equal(80, Unsafe.SizeOf<NativeMethods.DcaChartNote>());
        Assert.Equal(64, (int)Marshal.OffsetOf<NativeMethods.DcaChartNote>(nameof(NativeMethods.DcaChartNote.Capo)));
        Assert.Equal(68, (int)Marshal.OffsetOf<NativeMethods.DcaChartNote>(nameof(NativeMethods.DcaChartNote.Reserved1)));
        Assert.Equal(64, (int)Marshal.OffsetOf<JuceChartNote>(nameof(JuceChartNote.Capo)));
        Assert.Equal(typeof(int), typeof(JuceChartNote).GetField(nameof(JuceChartNote.Capo))!.FieldType);
    }

}

/// <summary>The capo round-trips through the public struct into the native layout
/// byte for byte, and an engine on the NULL device accepts it (and rejects a capo
/// past fret 24 with the whole list).</summary>
[Collection("NullDevice")]
public sealed class ChartNoteCapoNullDeviceTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();

    public void Dispose() => _scope.Dispose();

    [NativeAvailableFact]
    public void TheCapo_RoundTripsIntoTheNativeLayout_AndTheEngineAcceptsIt()
    {
        var note = new JuceChartNote
        {
            StartSeconds = 1.0, ReleaseSeconds = 1.5, Pitch = 54f, Velocity = 0.8f,
            StringIndex = 0, Fret = 14, Capo = 2, SlideToFret = -1f, ChordId = -1, BendFirst = -1,
            Flags = JuceChartNoteFlags.Harmonic,
        };
        var native = MemoryMarshal.Cast<JuceChartNote, NativeMethods.DcaChartNote>(new[] { note });
        Assert.Equal(2, native[0].Capo);
        Assert.Equal(14, native[0].Fret);
        Assert.Equal(54f, native[0].Pitch);
        Assert.Equal(2, MemoryMarshal.Read<int>(MemoryMarshal.AsBytes(native).Slice(64, 4)));

        using var engine = NullDevice.CreateEngine();
        engine.SetChartNotes(0, new[] { note }, default);
        Assert.True(engine.TryGetChartSourceStatus(0, out var st));
        Assert.Equal(1, st.NoteCount);
        note.Capo = 30;   // past fret 24: the whole list is rejected
        Assert.Throws<JuceAudioException>(() => engine.SetChartNotes(0, new[] { note }, default));
    }
}
