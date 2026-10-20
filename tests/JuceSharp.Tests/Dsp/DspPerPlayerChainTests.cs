using System.Runtime.InteropServices;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// ABI v28 per-player live-input rigs, verified OFFLINE and HEADLESS.
///
/// <para>Every test here drives real native DSP through
/// <c>dca_engine_dsp_process_buffer_for_player</c> with NO audio device
/// running: <c>dca_engine_create</c> opens one, so the fixture stops it
/// immediately and the offline render path is the only processor. That is the
/// whole reason this seam exists.</para>
///
/// <para>What is being pinned:</para>
/// <list type="number">
///   <item>Two seats holding different rigs produce DIFFERENT output from the
///         same input — i.e. there really are two chains, not one shared one.</item>
///   <item>The seats are INDEPENDENT: editing seat 0's rig does not move a
///         single sample of seat 1's buffer.</item>
///   <item>Node ids carry their seat, which is what lets every node-scoped
///         export keep its pre-v28 signature.</item>
///   <item>The pre-v28 exports are exactly seat 0.</item>
/// </list>
///
/// The chain add/param plumbing is declared test-locally (same pattern as
/// <see cref="DspSplitChainTests"/>) because the shipping managed mirror of
/// those entry points lives in the parent repo.
/// </summary>
[Collection("NativeEngine")]
public sealed class DspPerPlayerChainTests : IDisposable
{
    private const string Lib = "juce_sharp";

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_add_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAddNode(nint engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_add_node_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAddNodeFor(nint engine, int player,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_node_count", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspNodeCount(nint engine);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_node_count_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspNodeCountFor(nint engine, int player);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_get_node_ids_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspGetNodeIdsFor(nint engine, int player, [Out] int[]? outIds, int capacity);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_clear_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspClearFor(nint engine, int player);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, float value);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_get_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspGetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, out float value);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_remove_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspRemoveNode(nint engine, int nodeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_bypass", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetBypass(nint engine, int nodeId, int bypassed);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_process_buffer", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspProcess(nint engine, float[] left, float[] right, int frames, int sampleRate);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_process_buffer_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspProcessFor(nint engine, int player,
        float[] left, float[] right, int frames, int sampleRate);

    private const int Ok = 0;
    private const int Frames = 2048;
    private const int SampleRate = 48000;

    private readonly JuceAudioEngine? _engine;

    // dca_engine_create OPENS a device, so stop it immediately: the offline
    // render path must be the only processor and nothing here may sit on the
    // host's sound card (an ASIO driver is typically exclusive). Same discipline
    // as DotCharter's DspOfflineProcessingTests fixture.
    public DspPerPlayerChainTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose()
    {
        if (_engine is null) return;
        for (var p = 0; p < JuceAudioEngine.MaxPlayers; p++) DspClearFor(_engine.NativeHandle, p);
        _engine.Dispose();
    }

    private nint H => _engine!.NativeHandle;

    /// <summary>A deterministic, non-trivial probe signal. Not silence: a chain
    /// difference measured on zeros would pass with every module bypassed.</summary>
    private static float[] Probe()
    {
        var buf = new float[Frames];
        for (var i = 0; i < Frames; i++)
            buf[i] = 0.25f * MathF.Sin(2f * MathF.PI * 110f * i / SampleRate);
        return buf;
    }

    private static float Peak(float[] a)
    {
        var p = 0f;
        foreach (var v in a) p = MathF.Max(p, MathF.Abs(v));
        return p;
    }

    // ---------------------------------------------------------------------
    // (1) Two seats, two rigs, two different sounds.
    // ---------------------------------------------------------------------

    [NativeAvailableFact]
    public void TwoSeats_WithDifferentRigs_ProduceDifferentOutputFromTheSameInput()
    {
        // Seat 0: +12 dB. Seat 1: -12 dB. One shared chain could not do this.
        var n0 = DspAddNodeFor(H, 0, "gain");
        var n1 = DspAddNodeFor(H, 1, "gain");
        Assert.True(n0 >= 0);
        Assert.True(n1 >= 0);
        Assert.Equal(Ok, DspSetParam(H, n0, "gainDb", 12f));
        Assert.Equal(Ok, DspSetParam(H, n1, "gainDb", -12f));

        float[] l0 = Probe(), r0 = Probe(), l1 = Probe(), r1 = Probe();
        Assert.Equal(Ok, DspProcessFor(H, 0, l0, r0, Frames, SampleRate));
        Assert.Equal(Ok, DspProcessFor(H, 1, l1, r1, Frames, SampleRate));

        var peakIn = Peak(Probe());
        // ~3.981x and ~0.251x of the input peak.
        Assert.InRange(Peak(l0) / peakIn, 3.9f, 4.1f);
        Assert.InRange(Peak(l1) / peakIn, 0.24f, 0.26f);

        // And they are genuinely different buffers, sample for sample.
        Assert.NotEqual(l0, l1);
    }

    // ---------------------------------------------------------------------
    // (2) Independence: seat 0's rig cannot reach seat 1's audio.
    // ---------------------------------------------------------------------

    [NativeAvailableFact]
    public void EditingOneSeatsRig_DoesNotAlterAnotherSeatsOutput()
    {
        var n0 = DspAddNodeFor(H, 0, "gain");
        var n1 = DspAddNodeFor(H, 1, "gain");
        Assert.Equal(Ok, DspSetParam(H, n0, "gainDb", 0f));
        Assert.Equal(Ok, DspSetParam(H, n1, "gainDb", 6f));

        float[] before = Probe(), beforeR = Probe();
        Assert.Equal(Ok, DspProcessFor(H, 1, before, beforeR, Frames, SampleRate));

        // Now do everything a player could do to seat 0: re-gain it, stack a
        // second pedal on it, bypass one, remove one.
        Assert.Equal(Ok, DspSetParam(H, n0, "gainDb", 18f));
        var extra = DspAddNodeFor(H, 0, "gain");
        Assert.True(extra >= 0);
        Assert.Equal(Ok, DspSetParam(H, extra, "gainDb", -24f));
        Assert.Equal(Ok, DspSetBypass(H, n0, 1));
        Assert.Equal(Ok, DspRemoveNode(H, extra));

        float[] after = Probe(), afterR = Probe();
        Assert.Equal(Ok, DspProcessFor(H, 1, after, afterR, Frames, SampleRate));

        // Bit-identical. A shared chain would have moved every sample here.
        Assert.Equal(before, after);
        Assert.Equal(beforeR, afterR);
    }

    [NativeAvailableFact]
    public void ASeatWithNoRig_IsAStrictPassthrough()
    {
        // Seat 0 loaded, seat 2 empty.
        var n0 = DspAddNodeFor(H, 0, "gain");
        Assert.Equal(Ok, DspSetParam(H, n0, "gainDb", 12f));

        float[] l = Probe(), r = Probe();
        Assert.Equal(Ok, DspProcessFor(H, 2, l, r, Frames, SampleRate));
        Assert.Equal(Probe(), l);
        Assert.Equal(Probe(), r);
    }

    // ---------------------------------------------------------------------
    // (3) The node-id contract that keeps the node-scoped ABI unchanged.
    // ---------------------------------------------------------------------

    [NativeAvailableFact]
    public void NodeIds_CarryTheirSeat_SoNodeScopedCallsNeedNoPlayerArgument()
    {
        var stride = JuceAudioEngine.PlayerNodeIdStride;

        var n0 = DspAddNodeFor(H, 0, "gain");
        var n1 = DspAddNodeFor(H, 1, "gain");
        var n3 = DspAddNodeFor(H, 3, "gain");

        Assert.InRange(n0, 0, stride - 1);
        Assert.InRange(n1, stride, 2 * stride - 1);
        Assert.InRange(n3, 3 * stride, 4 * stride - 1);

        // Node-scoped set/get resolve the seat from the id alone.
        Assert.Equal(Ok, DspSetParam(H, n1, "gainDb", -3f));
        Assert.Equal(Ok, DspSetParam(H, n3, "gainDb", 9f));
        Assert.Equal(Ok, DspGetParam(H, n1, "gainDb", out var v1));
        Assert.Equal(Ok, DspGetParam(H, n3, "gainDb", out var v3));
        Assert.Equal(-3f, v1, 5);
        Assert.Equal(9f, v3, 5);

        // ...and the listing reports the same GLOBAL ids it handed out.
        var ids = new int[8];
        Assert.Equal(1, DspGetNodeIdsFor(H, 1, ids, ids.Length));
        Assert.Equal(n1, ids[0]);
    }

    [NativeAvailableFact]
    public void ANodeIdNamingANonExistentSeat_IsRejectedRatherThanClamped()
    {
        var stride = JuceAudioEngine.PlayerNodeIdStride;
        var n0 = DspAddNodeFor(H, 0, "gain");
        Assert.Equal(Ok, DspSetParam(H, n0, "gainDb", 4f));

        // Seat MaxPlayers does not exist. Clamping it onto a real seat would
        // silently apply one player's edit to another player's rig.
        var bogus = JuceAudioEngine.MaxPlayers * stride + (n0 % stride);
        Assert.NotEqual(Ok, DspSetParam(H, bogus, "gainDb", 99f));
        Assert.NotEqual(Ok, DspRemoveNode(H, bogus));

        Assert.Equal(Ok, DspGetParam(H, n0, "gainDb", out var v));
        Assert.Equal(4f, v, 5);
    }

    [NativeAvailableFact]
    public void AnOutOfRangeSeat_IsRejectedByEveryChainScopedCall()
    {
        var bad = JuceAudioEngine.MaxPlayers;
        Assert.True(DspAddNodeFor(H, bad, "gain") < 0);
        Assert.True(DspAddNodeFor(H, -1, "gain") < 0);
        // -1, NOT 0, since W4-N8: 0 is the count of an EMPTY chain, which
        // ThePreV28Exports_AreSeatZero asserts two tests down for the valid but
        // unused seat 1. Answering 0 here made "you passed a seat that does not
        // exist" and "this seat has no pedals" the same answer — the shape that
        // makes a disposed-handle bug read as an empty board.
        Assert.Equal(-1, DspNodeCountFor(H, bad));
        Assert.NotEqual(Ok, DspClearFor(H, bad));

        float[] l = Probe(), r = Probe();
        Assert.NotEqual(Ok, DspProcessFor(H, bad, l, r, Frames, SampleRate));
        Assert.Equal(Probe(), l);
    }

    // ---------------------------------------------------------------------
    // (4) Every pre-v28 export is exactly seat 0.
    // ---------------------------------------------------------------------

    [NativeAvailableFact]
    public void ThePreV28Exports_AreSeatZero()
    {
        var viaLegacy = DspAddNode(H, "gain");
        Assert.True(viaLegacy >= 0);
        Assert.True(viaLegacy < JuceAudioEngine.PlayerNodeIdStride); // seat 0 numbering, unchanged

        Assert.Equal(1, DspNodeCount(H));
        Assert.Equal(1, DspNodeCountFor(H, 0));
        Assert.Equal(0, DspNodeCountFor(H, 1));

        Assert.Equal(Ok, DspSetParam(H, viaLegacy, "gainDb", 6f));

        // The legacy offline render is seat 0's render.
        float[] lLegacy = Probe(), rLegacy = Probe(), lSeat0 = Probe(), rSeat0 = Probe();
        Assert.Equal(Ok, DspProcess(H, lLegacy, rLegacy, Frames, SampleRate));
        Assert.Equal(Ok, DspProcessFor(H, 0, lSeat0, rSeat0, Frames, SampleRate));
        Assert.Equal(lLegacy, lSeat0);

        // ...and the legacy clear only clears seat 0.
        var onSeat1 = DspAddNodeFor(H, 1, "gain");
        Assert.True(onSeat1 >= 0);
        Assert.Equal(Ok, DspClearFor(H, 0));
        Assert.Equal(0, DspNodeCountFor(H, 0));
        Assert.Equal(1, DspNodeCountFor(H, 1));
    }

    // ---------------------------------------------------------------------
    // Routing + seat-count surface (no device needed for the round trip).
    // ---------------------------------------------------------------------

    [NativeAvailableFact]
    public void PlayerCount_DefaultsToOne_AndClampsToTheSupportedRange()
    {
        Assert.Equal(1, _engine!.GetPlayerCount());

        _engine.SetPlayerCount(2);
        Assert.Equal(2, _engine.GetPlayerCount());

        // Out of range is REJECTED, not clamped silently onto a live value.
        _engine.SetPlayerCount(0);
        Assert.Equal(2, _engine.GetPlayerCount());
        _engine.SetPlayerCount(JuceAudioEngine.MaxPlayers + 1);
        Assert.Equal(2, _engine.GetPlayerCount());

        _engine.SetPlayerCount(1);
        Assert.Equal(1, _engine.GetPlayerCount());
    }

    [NativeAvailableFact]
    public void EachSeat_DefaultsToItsOwnInputChannel_AndRoundTrips()
    {
        // Two guitars into a 2-in interface works with no configuration.
        for (var p = 0; p < JuceAudioEngine.MaxPlayers; p++)
            Assert.Equal(p, _engine!.GetPlayerInputChannel(p));

        _engine!.SetPlayerInputChannel(1, 5);
        Assert.Equal(5, _engine.GetPlayerInputChannel(1));

        // -1 is the legal "mix every input to mono" value, not an error.
        _engine.SetPlayerInputChannel(1, -1);
        Assert.Equal(-1, _engine.GetPlayerInputChannel(1));

        // Seat 0 is exactly the pre-v28 capture channel.
        _engine.SetInputCaptureChannel(3);
        Assert.Equal(3, _engine.GetPlayerInputChannel(0));

        // Restore so nothing leaks into another test on this engine.
        _engine.SetInputCaptureChannel(0);
        _engine.SetPlayerInputChannel(1, 1);
    }

    [NativeAvailableFact]
    public void EachSeat_HasItsOwnCaptureRing_AndReadsSilenceBeforeAnyDeviceRuns()
    {
        // No device has ever run, so every ring is empty. The point of the
        // assertion is that each SEAT answers independently rather than one
        // shared ring answering for all of them.
        var buf = new float[256];
        for (var p = 0; p < JuceAudioEngine.MaxPlayers; p++)
        {
            Assert.Equal(0, _engine!.ReadInputCaptureStream(p, buf));
            Assert.False(_engine.TryGetInputCaptureSync(p, out _, out _));
        }

        // The windowed read zero-pads to a full window per seat.
        Assert.Equal(buf.Length, _engine!.ReadInputCapture(1, buf));
        Assert.All(buf, v => Assert.Equal(0f, v));
    }

    /// <summary>
    /// OFFLINE MEANS OFFLINE. The chunk loop behind these exports takes the
    /// chain's process barrier — the audio callback's own wait-one-block mutex
    /// — once per 512 frames, so rendering a three-minute take against a
    /// running device contends the callback thousands of times, each for a whole
    /// rig pass on a worker thread. Both exports refuse with
    /// <c>DCA_ERR_DEVICE</c> (3) while a device is open.
    /// <para>
    /// Every real caller was already device-less and said so: DotCharter's
    /// <c>TakeReampService</c> ("never a device — the offline path must be the
    /// only processor"), its <c>DspOfflineProcessingTests</c>, and all four DSP
    /// test classes here. Nothing had to change.
    /// </para>
    /// <para>
    /// This test does the opposite of the fixture: it opens a device on
    /// purpose, on a FRESH engine (a stopped engine cannot be restarted, #27),
    /// so it skips on a host with no audio hardware rather than failing, and
    /// stops that engine again in a finally.
    /// </para>
    /// </summary>
    [NativeAvailableFact]
    public void OfflineRender_RefusesWhileADeviceIsOpen_AndWorksWhenItIsNot()
    {
        var l = Probe();
        var r = Probe();

        // Device-less (the fixture's state): both exports render.
        Assert.Equal(Ok, DspProcess(H, l, r, Frames, SampleRate));
        Assert.Equal(Ok, DspProcessFor(H, 1, l, r, Frames, SampleRate));

        using var live = new JuceAudioEngine();   // the constructor opens a device
        if (live.SampleRate <= 0)
        {
            Assert.Skip("no audio device on this host — the open-device half cannot run");
            return;
        }

        var lh = live.NativeHandle;
        try
        {
            const int ErrDevice = 3; // DCA_ERR_DEVICE
            Assert.Equal(ErrDevice, DspProcess(lh, l, r, Frames, SampleRate));
            Assert.Equal(ErrDevice, DspProcessFor(lh, 1, l, r, Frames, SampleRate));

            // Argument validation still comes FIRST, so the device check cannot
            // shadow a more specific rejection (the rule the 0.48 null-handle
            // work established). These hold under the OLD behaviour too, which
            // is the point: they test the ORDER, not the new guard.
            const int ErrInvalidArg = 1;
            Assert.Equal(ErrInvalidArg, DspProcessFor(lh, 9999, l, r, Frames, SampleRate));
            Assert.Equal(ErrInvalidArg, DspProcess(lh, l, r, 0, SampleRate));
        }
        finally
        {
            live.Stop();
        }

        // ...and the refusal is not permanent: stopping the device restores it.
        Assert.Equal(Ok, DspProcess(lh, l, r, Frames, SampleRate));
    }

    /// <summary>
    /// A rate this far off would have re-derived every coefficient in the rig
    /// wrong and rendered a whole take that way, silently.
    /// <c>dca_droptune_process_offline</c> has always clamped 8000..192000;
    /// this one did not.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(0)]
    [InlineData(-48000)]
    [InlineData(7999)]
    [InlineData(192001)]
    [InlineData(int.MaxValue)]
    public void OfflineRender_RejectsAnImpossibleSampleRate(int sampleRate)
    {
        var l = Probe();
        var r = Probe();
        Assert.Equal(1, DspProcess(H, l, r, Frames, sampleRate));           // DCA_ERR_INVALID_ARG
        Assert.Equal(1, DspProcessFor(H, 1, l, r, Frames, sampleRate));
    }
}
