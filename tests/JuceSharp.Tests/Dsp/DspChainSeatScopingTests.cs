using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     W4-M8 and W4-M7: the two ways <see cref="JuceDspChain" />'s <c>int</c>
///     overloads accepted something the type's own doc says it does not handle.
///     <para>
///         Offline with the device stopped, the same discipline
///         <see cref="JuceDspChainWrapperTests" /> uses.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class DspChainSeatScopingTests : IDisposable
{
    private readonly JuceAudioEngine? _engine;

    public DspChainSeatScopingTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    /// <summary>
    ///     THE finding. Node ids are engine-global and the NODE-scoped exports
    ///     resolve the seat FROM the id — which is what kept the ABI v28 delta
    ///     small — so <c>GetDspChain(0).RemoveNode(seat1Id)</c> removed seat 1's
    ///     pedal from an object whose <see cref="JuceDspChain.Player" /> says 0.
    ///     And because <c>_known</c>, the edit shadow and the journal all belong
    ///     to seat 0, the removal was also un-journalled and un-rollback-able.
    /// </summary>
    [NativeAvailableFact]
    public void RemoveNode_RefusesAnotherSeatsNodeId_AndLeavesItAlone()
    {
        var seat0 = Engine.GetDspChain(0);
        var seat1 = Engine.GetDspChain(1);
        try
        {
            var foreign = seat1.AddNode("gain");
            Assert.Contains(foreign.Id, seat1.NodeIds());
            Assert.Equal(1, foreign.Id / JuceAudioEngine.PlayerNodeIdStride);

            var ex = Assert.Throws<ArgumentOutOfRangeException>(() => seat0.RemoveNode(foreign.Id));
            Assert.Equal("nodeId", ex.ParamName);

            Assert.Contains(foreign.Id, seat1.NodeIds());
        }
        finally
        {
            seat0.Clear();
            seat1.Clear();
        }
    }

    /// <summary>MoveNode has the same hole, and the same fix. The negative-index
    /// guard still runs first, so the two rejections do not shadow each
    /// other.</summary>
    [NativeAvailableFact]
    public void MoveNode_RefusesAnotherSeatsNodeId()
    {
        var seat0 = Engine.GetDspChain(0);
        var seat1 = Engine.GetDspChain(1);
        try
        {
            var a = seat1.AddNode("gain");
            var b = seat1.AddNode("gain");
            Assert.Equal(new[] { a.Id, b.Id }, seat1.NodeIds());

            Assert.Throws<ArgumentOutOfRangeException>(() => seat0.MoveNode(b.Id, 0));
            Assert.Equal(new[] { a.Id, b.Id }, seat1.NodeIds());

            // The seat check must not swallow the pre-existing index check.
            Assert.Throws<ArgumentOutOfRangeException>(() => seat1.MoveNode(b.Id, -1));
        }
        finally
        {
            seat0.Clear();
            seat1.Clear();
        }
    }

    /// <summary>
    /// TryReadMeter gates on THIS seat's metering flag before reading another
    /// seat's node meter, so a foreign id could come back <c>false</c> for a node
    /// whose OWN seat has metering on — a wrong answer dressed as "no reading",
    /// which is the one shape a <c>bool</c>-returning reader can never surface.
    /// </summary>
    [NativeAvailableFact]
    public void TryReadMeter_RefusesAnotherSeatsNodeId_RatherThanAnsweringFalse()
    {
        var seat0 = Engine.GetDspChain(0);
        var seat1 = Engine.GetDspChain(1);
        try
        {
            var foreign = seat1.AddNode("gain");
            seat1.MeteringEnabled = true;

            Assert.Throws<ArgumentOutOfRangeException>(() => seat0.TryReadMeter(foreign.Id, out _));
        }
        finally
        {
            seat1.MeteringEnabled = false;
            seat0.Clear();
            seat1.Clear();
        }
    }

    /// <summary>
    ///     W4-M7. The ABI caps <c>event_count</c> at 262,144 and checks it
    ///     NATIVE-side, i.e. after the whole list has been copied into an
    ///     unmanaged block the wrapper sized in <c>int</c> arithmetic. A caller
    ///     past the cap therefore paid a full O(n) marshal and a
    ///     multi-hundred-megabyte <c>AllocHGlobal</c> before being told "no" with
    ///     <c>out_bad_event_index = -1</c> — and at <c>count == 89,478,486</c> the
    ///     product <c>element * count</c> WRAPPED to 32, giving a 32-byte
    ///     allocation the loop then wrote 4.2 GB into.
    ///     <para>
    ///         The buffer being untouched is how this says "before any native
    ///         call": a rejected render leaves it alone either way, but a
    ///         MARSHALLED one would have had to reach the engine first.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void ProcessOffline_RejectsMoreEventsThanTheAbiAccepts_BeforeMarshallingThem()
    {
        var chain = Engine.GetDspChain(0);
        var buffer = new float[64];
        buffer[0] = 0.5f;

        var tooMany = new JuceDspAutomationEvent[JuceDspChain.MaxAutomationEvents + 1];

        var ex = Assert.Throws<ArgumentOutOfRangeException>(
            () => chain.ProcessOffline(buffer, 1, 48000, tooMany));

        Assert.Equal("events", ex.ParamName);
        Assert.Equal(0.5f, buffer[0]);
    }

    /// <summary>…and exactly the cap is not rejected by the wrapper. It fails on
    /// the FIRST event instead (an empty param id), which is the engine
    /// answering — proof the bound is `&gt;` and not `&gt;=`.</summary>
    [NativeAvailableFact]
    public void ProcessOffline_AcceptsExactlyTheCap_AndFailsOnTheEventsThemselves()
    {
        var chain = Engine.GetDspChain(0);
        var buffer = new float[64];
        var atTheCap = new JuceDspAutomationEvent[JuceDspChain.MaxAutomationEvents];

        Assert.Throws<JuceAudioException>(() => chain.ProcessOffline(buffer, 1, 48000, atTheCap));
    }
}

/// <summary>
///     W4-E5. <see cref="JuceDspChain.ProcessOffline(ReadOnlySpan{float},int,int,Span{float})" />
///     with <c>channels: 1</c> documented the way IN ("mono, duplicated to both
///     rig channels") and said nothing about the way OUT, which is
///     <c>0.5 * (L + R)</c> — a mono FOLD-DOWN. For a channel-symmetric chain
///     that is a no-op, which is why it read as harmless; for a chain containing
///     <c>widener</c>, <c>rotary</c>, <c>chorus</c>, a ping-pong <c>delay</c> or
///     the panner it cancels exactly what those modules put out of phase, and a
///     user re-amping a mono take gets a thin or partially silent result with
///     nothing saying why.
/// </summary>
[Collection("NativeEngine")]
public sealed class DspMonoFoldDownTests : IDisposable
{
    private const int Sr = 48000;

    private readonly JuceAudioEngine? _engine;

    public DspMonoFoldDownTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    /// <summary>
    /// The same signal through the mono overload and through the stereo one with
    /// the take duplicated into both channels. Through a decorrelating module the
    /// two DIFFER, and the difference is the fold-down the doc now names.
    /// </summary>
    [NativeAvailableFact]
    public void AWideningChain_RendersDifferentlyThroughTheMonoOverload()
    {
        var chain = Engine.GetDspChain(0);
        try
        {
            var widener = chain.AddNode("widener");
            Assert.NotNull(widener);

            const int frames = 8192;
            var take = new float[frames];
            for (var i = 0; i < frames; i++)
                take[i] = 0.5f * (float)Math.Sin(2.0 * Math.PI * 220.0 * i / Sr);

            // Mono overload: L = R = take in, 0.5 * (L + R) out.
            var mono = new float[frames];
            chain.ProcessOffline(take, 1, Sr, mono);

            // Stereo overload with the SAME mono take in both channels — the
            // input the rig sees is identical; only the output path differs.
            var interleaved = new float[frames * 2];
            for (var i = 0; i < frames; i++)
            {
                interleaved[i * 2] = take[i];
                interleaved[i * 2 + 1] = take[i];
            }

            var stereo = new float[frames * 2];
            chain.ProcessOffline(interleaved, 2, Sr, stereo);

            // Compare the LEFT channel of the stereo render against the mono one.
            // Skip the first 2048 frames: the chain crossfades a newly added node
            // in over ~10 ms, so the head is a ramp on both sides.
            var differing = 0;
            for (var i = 2048; i < frames; i++)
                if (Math.Abs(stereo[i * 2] - mono[i]) > 1e-4f) differing++;

            Assert.True(differing > 0,
                "the mono overload produced the stereo render's left channel — if 'widener' has "
                + "stopped decorrelating, this test is measuring nothing and needs a different module");
        }
        finally { chain.Clear(); }
    }

    /// <summary>…and through a channel-SYMMETRIC chain the two agree, which is
    /// what makes the fold-down invisible and is the whole reason it needed
    /// documenting rather than fixing.</summary>
    [NativeAvailableFact]
    public void AChannelSymmetricChain_RendersIdenticallyThroughBothOverloads()
    {
        var chain = Engine.GetDspChain(0);
        try
        {
            chain.AddNode("gain");

            const int frames = 8192;
            var take = new float[frames];
            for (var i = 0; i < frames; i++)
                take[i] = 0.25f * (float)Math.Sin(2.0 * Math.PI * 330.0 * i / Sr);

            var mono = new float[frames];
            chain.ProcessOffline(take, 1, Sr, mono);

            var interleaved = new float[frames * 2];
            for (var i = 0; i < frames; i++)
            {
                interleaved[i * 2] = take[i];
                interleaved[i * 2 + 1] = take[i];
            }

            var stereo = new float[frames * 2];
            chain.ProcessOffline(interleaved, 2, Sr, stereo);

            for (var i = 2048; i < frames; i++)
                Assert.True(Math.Abs(stereo[i * 2] - mono[i]) <= 1e-4f,
                    $"a channel-symmetric chain disagreed at frame {i}");
        }
        finally { chain.Clear(); }
    }
}
