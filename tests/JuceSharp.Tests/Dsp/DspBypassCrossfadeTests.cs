using System.Runtime.InteropServices;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// Bypass is CROSSFADED at the chain level (<c>DspChain.h</c>, 2026-08-28), not
/// spliced between two blocks. Before this, every one of the ~60 cores switched
/// as a hard cut: the footswitch clicked, and a core with an envelope follower
/// came back from bypass with the follower frozen at the level it saw when it
/// left. The chain owns the ramp so no core has to.
///
/// <para>Rendered OFFLINE through <c>dca_engine_dsp_process_buffer</c> on a
/// stopped engine, so the output is deterministic and the ramp's shape can be
/// read off the samples: a +12 dB gain node is bypassed between two renders of
/// one continuous sine, and the seam must ramp from 4x to 1x over
/// <c>kFadeSeconds</c> (10 ms = 480 samples at 48 kHz) instead of dropping in
/// one sample. The test computes what the OLD cut would have been at the seam
/// and requires it to be large, so a fixture that happened to land on a zero
/// crossing cannot pass vacuously.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class DspBypassCrossfadeTests : IDisposable
{
    private const string Lib = "juce_sharp";

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_add_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAddNode(nint engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_clear_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspClearFor(nint engine, int player);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, float value);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_bypass", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetBypass(nint engine, int nodeId, int bypassed);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_remove_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspRemoveNode(nint engine, int nodeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_move_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspMoveNode(nint engine, int nodeId, int newIndex);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_process_buffer", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspProcess(nint engine, float[] left, float[] right, int frames, int sampleRate);

    private const int Ok = 0;
    private const int Frames = 2048;
    private const int SampleRate = 48000;
    private const int FadeSamples = 480; // kFadeSeconds (10 ms) at 48 kHz
    private const float GainDb = 12f;
    private const float Gain = 3.98107171f; // 10^(12/20)

    private readonly JuceAudioEngine? _engine;

    // dca_engine_create OPENS a device, so stop it immediately: the offline
    // render path must be the only processor (same discipline as
    // DspPerPlayerChainTests).
    public DspBypassCrossfadeTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose()
    {
        if (_engine is null) return;
        DspClearFor(_engine.NativeHandle, 0);
        _engine.Dispose();
    }

    private nint H => _engine!.NativeHandle;

    /// <summary>One block of a continuous 110 Hz sine starting at sample <paramref name="offset"/>.</summary>
    private static float[] Sine(int offset)
    {
        var buf = new float[Frames];
        for (var i = 0; i < Frames; i++)
            buf[i] = 0.25f * MathF.Sin(2f * MathF.PI * 110f * (offset + i) / SampleRate);
        return buf;
    }

    private static float MaxStep(float previous, float[] block)
    {
        var max = MathF.Abs(block[0] - previous);
        for (var i = 1; i < block.Length; i++) max = MathF.Max(max, MathF.Abs(block[i] - block[i - 1]));
        return max;
    }

    /// <summary>Renders one block through the chain, returning the left channel.</summary>
    private float[] Render(int offset)
    {
        var left = Sine(offset);
        var right = Sine(offset);
        Assert.Equal(Ok, DspProcess(H, left, right, Frames, SampleRate));
        return left;
    }

    [NativeAvailableFact]
    public void BypassingAGainNode_RampsOverTenMilliseconds_InsteadOfCutting()
    {
        var n = DspAddNode(H, "gain");
        Assert.True(n >= 0);
        Assert.Equal(Ok, DspSetParam(H, n, "gainDb", GainDb));

        var live = Render(0);
        Assert.Equal(Ok, DspSetBypass(H, n, 1));
        var faded = Render(Frames);
        var dry = Sine(Frames);

        // Fixture check: the OLD splice would have dropped the seam sample from
        // Gain*x to x. That drop must be big, or a zero crossing makes any
        // bypass look smooth.
        var oldCut = (Gain - 1f) * MathF.Abs(dry[0]);
        Assert.True(oldCut > 0.3f, $"fixture: seam sits too close to a zero crossing (cut would be {oldCut})");

        // 1) No click: every sample-to-sample step, INCLUDING the seam, stays at
        // the order of the sine's own slope (0.25 * 2*pi*110/48000 ~ 0.0036,
        // times the gain, plus a 3x/480-sample ramp) — far below the cut.
        var maxStep = MaxStep(live[^1], faded);
        Assert.True(maxStep < 0.05f, $"seam stepped by {maxStep} (a hard cut would be {oldCut})");

        // 2) The ramp starts wet: the first faded sample is still ~Gain*x.
        Assert.InRange(faded[0] / dry[0], Gain * 0.97f, Gain * 1.001f);

        // 3) ...and lands dry within kFadeSeconds: past the ramp the output IS
        // the input, to float precision.
        for (var i = FadeSamples + 16; i < Frames; i++)
            Assert.True(MathF.Abs(faded[i] - dry[i]) < 1e-5f, $"sample {i} still wet after the ramp: {faded[i]} vs {dry[i]}");

        // 4) Mid-ramp it is genuinely between, not parked at either end.
        var mid = faded[FadeSamples / 2] / dry[FadeSamples / 2];
        Assert.InRange(mid, 1.5f, Gain - 0.5f);
    }

    [NativeAvailableFact]
    public void UnbypassingAGainNode_RampsBackIn_AndTheModuleRunsDuringTheRamp()
    {
        var n = DspAddNode(H, "gain");
        Assert.True(n >= 0);
        Assert.Equal(Ok, DspSetParam(H, n, "gainDb", GainDb));
        Assert.Equal(Ok, DspSetBypass(H, n, 1));

        var bypassed = Render(0);
        Assert.Equal(Ok, DspSetBypass(H, n, 0));
        var rising = Render(Frames);
        var dry = Sine(Frames);

        var maxStep = MaxStep(bypassed[^1], rising);
        Assert.True(maxStep < 0.05f, $"un-bypass seam stepped by {maxStep}");
        Assert.InRange(rising[0] / dry[0], 0.99f, 1.03f);
        for (var i = FadeSamples + 16; i < Frames; i++)
            Assert.True(MathF.Abs(rising[i] - Gain * dry[i]) < 1e-4f, $"sample {i} not fully wet after the ramp");
    }

    [NativeAvailableFact]
    public void AParkedBypassedNode_IsStillSkipped_AndStructureOpsReturnWithoutACallback()
    {
        // A bypassed node whose ramp has landed must cost nothing: its output is
        // the input exactly (no dry/wet mix rounding), which is what the skip
        // path guarantees and the mix path does not.
        var n = DspAddNode(H, "gain");
        Assert.True(n >= 0);
        Assert.Equal(Ok, DspSetParam(H, n, "gainDb", GainDb));
        Assert.Equal(Ok, DspSetBypass(H, n, 1));
        Render(0); // the ramp lands inside this block
        var parked = Render(Frames);
        Assert.Equal(Sine(Frames), parked);

        // Remove and move wait for a fade-out on the message thread, polling
        // the callback's heartbeat. With NO callback running (offline, stopped
        // device) they must give up promptly and still succeed — a hang here
        // would be a hang in every rig edit made while the device is closed.
        Assert.Equal(Ok, DspSetBypass(H, n, 0));
        var m = DspAddNode(H, "gain");
        Assert.True(m >= 0);
        Assert.Equal(Ok, DspMoveNode(H, m, 0));
        Assert.Equal(Ok, DspRemoveNode(H, n));
        Assert.Equal(Ok, DspRemoveNode(H, m));
    }
}
