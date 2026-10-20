using System.Runtime.InteropServices;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     ABI v34 sample-accurate MIDI, boundary half.
///     <para>
///         The PLACEMENT proof lives in the native RT gate
///         (<c>RtScheduledMidiLandsOnItsSampleWithoutAllocating</c>) and can
///         only live there: it needs an instrument attached and a manual-pump
///         device so a frame can be named with no clock estimation, and the
///         managed suite has neither a VST3 nor a way to read a rendered
///         MidiBuffer back. What IS provable here is the argument contract and
///         the counters — including the one thing a caller can get wrong, which
///         is a scheduling horizon shorter than the engine's own latency.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class MidiTimingTests : IDisposable
{
    private const string Lib = "juce_sharp";

    [DllImport(Lib, EntryPoint = "dca_engine_send_midi_at", CallingConvention = CallingConvention.Cdecl)]
    private static extern int SendMidiAt(nint engine, uint packed, long renderFrame);

    private readonly JuceAudioEngine? _engine;

    public MidiTimingTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    [Theory]
    [InlineData(0x00u)]   // the empty-slot marker
    [InlineData(0x7Fu)]   // a data byte, not a status byte
    [InlineData(0x4041u)] // status 0x41: still below 0x80
    public void AStatusByteBelow0x80_IsRejected(uint packed)
    {
        Assert.SkipUnless(NativeProbe.IsAvailable, "native engine not available");
        // Not pedantry: packed == 0 is the queue's EMPTY-SLOT marker, so
        // accepting one would make the audio thread's parked set unreadable —
        // every later event would be skipped as a free slot. Everything below
        // 0x80 is simply not a MIDI message.
        Assert.Equal(1 /* DCA_ERR_INVALID_ARG */, SendMidiAt(_engine!.NativeHandle, packed, 0));
    }

    [NativeAvailableFact]
    public void AValidMessageIsAccepted_EvenWithNoInstrumentAttached()
    {
        // The engine DROPS the event (no instrument, no bridge) but must still
        // report the call itself as well-formed: a status code is about the
        // arguments, not about whether anybody was listening.
        Assert.Equal(0 /* DCA_OK */, SendMidiAt(_engine!.NativeHandle, 0x90u | (60u << 8) | (100u << 16), 0));
        Assert.Equal(0, SendMidiAt(_engine.NativeHandle, 0x90u | (60u << 8) | (100u << 16), -1));
    }

    /// <summary>
    ///     The counters start at zero and the RESET is REPORTED. The reset half
    ///     used to assert <c>(0,0,0)</c>, reset, and assert <c>(0,0,0)</c> again
    ///     on a fixture where nothing had ever been scheduled — an assertion that
    ///     cannot fail. Moving a counter needs a running callback, so that lives
    ///     in <see cref="MidiTimingCounterTests" />; what this pins is the status
    ///     the tuple overload throws away.
    /// </summary>
    [NativeAvailableFact]
    public void TheCountersStartAtZero_AndTheResetIsReported()
    {
        Assert.Equal((0L, 0L, 0L), _engine!.GetMidiTimingStats());

        // TRUE, not void: "the engine accepted the reset" and "the engine is
        // gone" are different answers and the old void signature had no room
        // for either.
        Assert.True(_engine.ResetMidiTimingStats());
        Assert.True(_engine.TryGetMidiTimingStats(out var stats));
        Assert.Equal(default, stats);
    }

    [NativeAvailableFact]
    public void ADisposedEngine_DegradesQuietlyRatherThanThrowing()
    {
        var engine = new JuceAudioEngine();
        engine.Stop();
        engine.Dispose();
        // Every one of these is something a scheduler timer calls at rate; a
        // throw here would be an exception storm on shutdown, not a diagnostic.
        engine.SendMidi(0x90, 60, 100, 12345);
        // …and FALSE, which is the distinction a zeroed tuple cannot make: the
        // Try shape says "the engine is gone", where (0,0,0) also means
        // "nothing has been scheduled yet".
        Assert.False(engine.ResetMidiTimingStats());
        Assert.False(engine.TryGetMidiTimingStats(out var stats));
        Assert.Equal(default, stats);
        Assert.Equal((0L, 0L, 0L), engine.GetMidiTimingStats());
    }
}

// NOTE: the counters cannot be MOVED from managed code. `sendMidiAt` returns
// early when no instrument is attached and no sandbox bridge is enabled, so an
// event never reaches the queue at all, and the managed suite has no VST3. Both
// arms the wave-3 audit asked for — the far-future drop and the parked-set
// saturation — are therefore driven in the RT gate, where a stub instrument
// exists: phases E and F of RtScheduledMidiLandsOnItsSampleWithoutAllocating.

/// <summary>
///     ABI v34 MIDI hardware OUTPUT. Most of this skips on a box with no MIDI
///     interface, which is every CI runner — so the parts that do NOT need one
///     (enumeration, index validation, the lifetime contract) are deliberately
///     separated from the parts that do.
/// </summary>
[Collection("NativeEngine")]
public sealed class JuceMidiOutputTests : IDisposable
{
    private readonly JuceAudioEngine? _engine;

    public JuceMidiOutputTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    [NativeAvailableFact]
    public void Enumerate_NeverThrows_AndAgreesWithTheEngineHelper()
    {
        var direct = JuceMidiOutput.Enumerate();
        var viaEngine = JuceAudioEngine.EnumerateMidiOutputs();
        Assert.Equal(direct.Count, viaEngine.Count);
        // A host with no MIDI hardware returns an EMPTY list, never a
        // single-entry list containing "". That distinction is what the
        // two-call sizing pattern gets wrong when it forgets the NUL.
        Assert.DoesNotContain(direct, string.IsNullOrEmpty);
    }

    [NativeAvailableFact]
    public void ANegativeOrOutOfRangeIndex_Throws_AndLeavesNothingRooted()
    {
        Assert.Throws<JuceAudioException>(() => _engine!.OpenMidiOutput(-1));
        Assert.Throws<JuceAudioException>(() => _engine!.OpenMidiOutput(10_000));
        // A failed open must not leave a half-built child in either registry —
        // that is the shape that made a PASSING constructor test kill the host
        // hundreds of tests later through a throwing finalizer.
        Assert.Equal(0, _engine!.RootedMidiOutputsForTests);
    }

    [NativeAvailableFact]
    public void AnOpenOutput_IsRootedByTheEngine_AndUnrootedByItsOwnDispose()
    {
        if (JuceMidiOutput.Enumerate().Count == 0)
        {
            Assert.Skip("no MIDI output device on this host");
            return;
        }

        var output = _engine!.OpenMidiOutput(0);
        try
        {
            Assert.Equal(1, _engine.RootedMidiOutputsForTests);
            Assert.False(output.IsDisposed);
            // Strong rooting is the load-bearing half: an app that wires a
            // scheduler to an output and keeps no handle had its device closed
            // by the first gen-2 collection (JuceMidiInputRootingTests).
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            Assert.False(output.IsDisposed);
            Assert.Equal(1, _engine.RootedMidiOutputsForTests);
        }
        finally { output.Dispose(); }

        Assert.True(output.IsDisposed);
        Assert.Equal(0, _engine.RootedMidiOutputsForTests);
        Assert.Throws<ObjectDisposedException>(() => output.Send(0x90, 60, 100));
    }

    [NativeAvailableFact]
    public void TheDispatcherActuallyPutsMessagesOnTheWire()
    {
        if (JuceMidiOutput.Enumerate().Count == 0)
        {
            Assert.Skip("no MIDI output device on this host");
            return;
        }

        using var output = _engine!.OpenMidiOutput(0);
        Assert.Equal(0, output.SentCount);

        // An all-notes-off on channel 1, twice: harmless on any device, and the
        // ONLY way to tell a working scheduler from one that quietly queues
        // for ever. The assertion is on the COUNT, never on how long it took.
        output.Send(0xB0, 123, 0);
        output.SendAt(new byte[] { 0xB0, 123, 0 }, -1);

        for (var i = 0; i < 200 && output.SentCount < 2; i++) Thread.Sleep(10);
        Assert.Equal(2, output.SentCount);
    }

    [NativeAvailableFact]
    public void TheEngineClosesAnOutputItStillOwns()
    {
        if (JuceMidiOutput.Enumerate().Count == 0)
        {
            Assert.Skip("no MIDI output device on this host");
            return;
        }

        var engine = new JuceAudioEngine();
        engine.Stop();
        var output = engine.OpenMidiOutput(0);
        Assert.False(output.IsDisposed);

        // Teardown ORDER is the point: the engine claims its own handle, then
        // disposes every child, and only then calls dca_engine_destroy.
        engine.Dispose();
        Assert.True(output.IsDisposed);
    }
}
