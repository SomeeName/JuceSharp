using System.Runtime.InteropServices;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// <c>DspModule::reset()</c> has exactly ONE caller (the 0.48 audit):
/// <c>DspChain::setBypass(id, false)</c> on a node whose fade-out has fully
/// landed.
///
/// <para>A parked node is skipped outright by <c>DspChain::process</c>, so its
/// delay lines, reverb tanks, followers and LFO phases FREEZE at the instant the
/// fade reached 0 — they do not decay. Fading that back in replays audio from
/// before the bypass, which is neither of the two things a real pedal does: true
/// bypass kills the tail, trails let it decay away. Clearing on the way back in
/// is the true-bypass answer, and it is the only place in the engine where
/// clearing state is both wanted and safe (the audio thread is provably not
/// inside <c>process()</c> for a node whose weight has landed at 0).</para>
///
/// <para>Move and remove drive the bypass flag directly through
/// <c>fadeOutAndWait</c>/<c>fadeIn</c>, never through <c>setBypass</c>, so
/// DspChain.h's own "reordering a delay never clears its buffer" still holds —
/// which is the second case below.</para>
/// </summary>
[Collection("NativeEngine")]
public sealed class DspBypassClearsStateTests : IDisposable
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

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_move_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspMoveNode(nint engine, int nodeId, int newIndex);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_process_buffer", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspProcess(nint engine, float[] left, float[] right, int frames, int sampleRate);

    private const int Ok = 0;
    private const int Frames = 2048;
    private const int SampleRate = 48000;

    private readonly JuceAudioEngine? _engine;

    public DspBypassClearsStateTests()
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

    [NativeAvailableFact]
    public void UnBypassingAParkedDelay_ClearsItsTail()
    {
        var n = AddLoudDelay();

        // Excite it, then let the repeats build. Silence in from here on, so
        // anything that comes out is the delay's own memory.
        RenderBurst();
        var liveTail = Rms(RenderSilence());
        Assert.True(liveTail > 0.02f,
            $"the fixture never got a tail to lose (RMS {liveTail:F4})");

        // Park it: one silent block is 42 ms, well past the 10 ms fade.
        Assert.Equal(Ok, DspSetBypass(H, n, 1));
        RenderSilence();
        RenderSilence();

        Assert.Equal(Ok, DspSetBypass(H, n, 0));
        var afterUnBypass = Rms(RenderSilence());

        Assert.True(afterUnBypass < 0.1f * liveTail,
            $"un-bypassing replayed a tail from before the bypass (RMS {afterUnBypass:F4} "
            + $"against {liveTail:F4} live) — the parked module was never cleared");
    }

    /// <summary>
    /// The other half of the decision, and the reason the reset is in
    /// <c>setBypass</c> and not in the fade itself: a MOVE uses the same internal
    /// fade and must NOT clear the delay's buffer.
    /// </summary>
    [NativeAvailableFact]
    public void MovingADelay_KeepsItsTail()
    {
        Assert.Equal(Ok, DspClearFor(H, 0));
        var head = DspAddNode(H, "gain");   // something to move past
        Assert.True(head >= 0);
        var n = AddLoudDelay(clearFirst: false);

        RenderBurst();
        var before = Rms(RenderSilence());
        Assert.True(before > 0.02f, $"the fixture never got a tail (RMS {before:F4})");

        Assert.Equal(Ok, DspMoveNode(H, n, 0));
        var after = Rms(RenderSilence());

        Assert.True(after > 0.3f * before,
            $"moving the delay cleared its buffer (RMS {after:F4} against {before:F4}) — "
            + "DspChain.h promises a reorder never does that");
    }

    private int AddLoudDelay(bool clearFirst = true)
    {
        if (clearFirst) Assert.Equal(Ok, DspClearFor(H, 0));
        var n = DspAddNode(H, "delay");
        Assert.True(n >= 0, "delay: add failed");
        Assert.Equal(Ok, DspSetParam(H, n, "timeMs", 60f));
        Assert.Equal(Ok, DspSetParam(H, n, "feedback", 0.9f));
        Assert.Equal(Ok, DspSetParam(H, n, "mix", 1f));
        // One silent block so the chain's own ADD crossfade is finished before
        // anything is measured.
        RenderSilence();
        return n;
    }

    private void RenderBurst()
    {
        var l = new float[Frames];
        var r = new float[Frames];
        for (var i = 0; i < Frames; i++)
        {
            var s = 0.5f * MathF.Sin(2f * MathF.PI * 220f * i / SampleRate);
            l[i] = s; r[i] = s;
        }
        Assert.Equal(Ok, DspProcess(H, l, r, Frames, SampleRate));
    }

    private float[] RenderSilence()
    {
        var l = new float[Frames];
        var r = new float[Frames];
        Assert.Equal(Ok, DspProcess(H, l, r, Frames, SampleRate));
        return l;
    }

    private static float Rms(float[] x)
    {
        double acc = 0;
        foreach (var v in x) acc += (double)v * v;
        return (float)Math.Sqrt(acc / x.Length);
    }
}
