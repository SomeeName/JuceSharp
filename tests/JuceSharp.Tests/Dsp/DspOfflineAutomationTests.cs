using System.Runtime.InteropServices;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     ABI v34 offline render WITH PARAMETER AUTOMATION, end to end and
///     HEADLESS: <c>dca_engine_create</c> opens a device, so the fixture stops
///     it immediately and the offline render path is the only processor. Same
///     discipline as <see cref="DspPerPlayerChainTests" />.
///     <para>
///         The point of this surface is that the render is chunked at each
///         event boundary, so an event at frame <c>F</c> is heard BY sample
///         <c>F</c> — which is exactly what the "two halves differ by the
///         expected dB" assertion measures, and what a whole-buffer render
///         followed by a second call cannot express.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class DspOfflineAutomationTests : IDisposable
{
    private const int SampleRate = 48000;
    private const int HalfFrames = SampleRate;            // 1 s
    private const int Frames     = HalfFrames * 2;        // 2 s

    private readonly JuceAudioEngine? _engine;

    public DspOfflineAutomationTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose()
    {
        if (_engine is null) return;
        try { _engine.GetDspChain().Clear(); } catch { /* best effort */ }
        _engine.Dispose();
    }

    /// <summary>A steady 110 Hz sine, interleaved stereo. Not silence: a level
    /// change measured on zeros is zero either way.</summary>
    private static float[] Probe()
    {
        var buf = new float[Frames * 2];
        for (var i = 0; i < Frames; i++)
        {
            var s = 0.25f * MathF.Sin(2f * MathF.PI * 110f * i / SampleRate);
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        return buf;
    }

    private static double Rms(ReadOnlySpan<float> interleaved, int startFrame, int frameCount)
    {
        double sum = 0;
        for (var i = startFrame; i < startFrame + frameCount; i++)
            sum += (double)interleaved[i * 2] * interleaved[i * 2];
        return Math.Sqrt(sum / frameCount);
    }

    // -----------------------------------------------------------------
    // (1) The event really lands where it says it does.
    // -----------------------------------------------------------------

    [NativeAvailableFact]
    public void AGainChangeAtOneSecond_SplitsTheRenderIntoTwoLevels()
    {
        var chain = _engine!.GetDspChain();
        var node = chain.AddNode("gain");
        node.SetParam("gainDb", 0f);

        var buffer = Probe();
        var events = new[]
        {
            JuceDspAutomationEvent.Create(HalfFrames, node.Id, "gainDb", 12f),
        };
        chain.ProcessOffline(buffer, channels: 2, SampleRate, events);

        // Skip 2,400 frames (50 ms) either side of the boundary: the `gain`
        // node RAMPS its level over ~10 ms rather than stepping it (the
        // "every user-facing gain is ramped" rule), so the samples straddling
        // the event are legitimately mid-ramp and would blur the measurement.
        const int guard = 2400;
        var before = Rms(buffer, guard, HalfFrames - 2 * guard);
        var after  = Rms(buffer, HalfFrames + guard, HalfFrames - 2 * guard);

        Assert.True(before > 1e-4, $"the first half is silent ({before}) — the probe is vacuous");
        var dB = 20.0 * Math.Log10(after / before);
        Assert.InRange(dB, 11.0, 13.0);
    }

    /// <summary>
    ///     The sharp version of the one above. "Heard BY sample F" is the whole
    ///     claim of this surface, and an RMS over half a second cannot tell
    ///     frame F from frame F + 128 — which is exactly the error a render
    ///     chunked at a FIXED stride makes, because it can only apply an event
    ///     at the next multiple of the chunk size. So: render the same rig twice,
    ///     once clean and once with a single event, and assert the FIRST frame
    ///     at which they differ is the event's own frame.
    /// </summary>
    [NativeAvailableFact]
    public void TheEventTakesEffectOnExactlyItsSample_NotAtTheNextChunkBoundary()
    {
        // Deliberately not a multiple of 512 (the engine's offline chunk), and
        // not near one: a stride-chunked render would land at 12,800.
        const long eventFrame = 12_345;

        var chain = _engine!.GetDspChain();
        var node = chain.AddNode("gain");
        node.SetParam("gainDb", 0f);

        var clean = Probe();
        chain.ProcessOffline(clean, 2, SampleRate, ReadOnlySpan<JuceDspAutomationEvent>.Empty);

        chain.Clear();
        var node2 = chain.AddNode("gain");
        node2.SetParam("gainDb", 0f);
        var automated = Probe();
        chain.ProcessOffline(automated, 2, SampleRate,
            new[] { JuceDspAutomationEvent.Create(eventFrame, node2.Id, "gainDb", 12f) });

        var firstDiff = -1L;
        for (var i = 0; i < Frames; i++)
        {
            if (Math.Abs(clean[i * 2] - automated[i * 2]) > 1e-7f) { firstDiff = i; break; }
        }

        Assert.True(firstDiff >= 0, "the event changed nothing at all — the probe is vacuous");
        // The chunk BEGINS at the event's own frame — that is the claim, and it
        // is what a stride-chunked render (12,800) would break.
        //
        // The first differing SAMPLE is frame + 1, not frame, and that is the
        // correct behaviour of a zipper-free gain: `gain` ramps across its block
        // from the value the previous block landed on, so sample 0 of the new
        // chunk carries the OLD gain exactly and the ramp moves off it from
        // there. It used to read `frame` because the ramp advanced BEFORE the
        // multiply — one whole increment of discontinuity against the previous
        // block's last sample, on every ramping block (W4-D72), which this
        // assertion was unknowingly measuring. `juce::AudioBuffer::applyGainRamp`,
        // the reference CleanGainModule's own comment invokes, applies `start` to
        // sample 0 too.
        Assert.InRange(firstDiff, eventFrame, eventFrame + 1);
        Assert.True(firstDiff < 12_800,
            $"the event landed at {firstDiff}: that is the next chunk boundary, not its own frame");
    }

    [NativeAvailableFact]
    public void WithNoEvents_ItMatchesTheStaticRenderExactly()
    {
        var chain = _engine!.GetDspChain();
        var node = chain.AddNode("gain");
        node.SetParam("gainDb", 6f);

        var automated = Probe();
        chain.ProcessOffline(automated, 2, SampleRate, ReadOnlySpan<JuceDspAutomationEvent>.Empty);

        // The pre-v34 path, same rig, same input. If the chunking changed the
        // arithmetic at all — a re-prepare, a reset, a different block split —
        // this is where it shows.
        chain.Clear();
        var node2 = chain.AddNode("gain");
        node2.SetParam("gainDb", 6f);
        var stat = Probe();
        var outp = new float[stat.Length];
        chain.ProcessOffline(stat, 2, SampleRate, outp);

        for (var i = 0; i < automated.Length; i += 997)   // a prime stride, ~100 samples apart
            Assert.Equal(outp[i], automated[i], 6);
    }

    // -----------------------------------------------------------------
    // (2) Rejection, by index, with the buffer untouched.
    // -----------------------------------------------------------------

    [NativeAvailableFact]
    public void AnUnsortedEventList_IsRejectedRatherThanReordered()
    {
        var chain = _engine!.GetDspChain();
        var node = chain.AddNode("gain");

        var buffer = Probe();
        var original = (float[])buffer.Clone();
        var events = new[]
        {
            JuceDspAutomationEvent.Create(1000, node.Id, "gainDb", 3f),
            JuceDspAutomationEvent.Create(5000, node.Id, "gainDb", 6f),
            JuceDspAutomationEvent.Create(4000, node.Id, "gainDb", 9f),   // index 2: goes backwards
        };

        var ex = Assert.Throws<JuceAudioException>(
            () => chain.ProcessOffline(buffer, 2, SampleRate, events));
        Assert.Contains("2", ex.Message);
        // Nothing was rendered: a caller holding a half-processed buffer cannot
        // tell it apart from a good one.
        Assert.Equal(original, buffer);
    }

    [Theory]
    [InlineData(float.NaN)]
    [InlineData(float.PositiveInfinity)]
    public void ANonFiniteValue_IsRejectedWithItsIndex(float bad)
    {
        Assert.SkipUnless(NativeProbe.IsAvailable, "native engine not available");
        var chain = _engine!.GetDspChain();
        var node = chain.AddNode("gain");

        var buffer = Probe();
        var original = (float[])buffer.Clone();
        var events = new[]
        {
            JuceDspAutomationEvent.Create(100, node.Id, "gainDb", 3f),
            JuceDspAutomationEvent.Create(200, node.Id, "gainDb", bad),   // index 1
        };

        var ex = Assert.Throws<JuceAudioException>(
            () => chain.ProcessOffline(buffer, 2, SampleRate, events));
        Assert.Contains("1", ex.Message);
        Assert.Equal(original, buffer);
    }

    [NativeAvailableFact]
    public void AFrameOutsideTheBuffer_AndAnUnknownParam_AreBothRejectedByIndex()
    {
        var chain = _engine!.GetDspChain();
        var node = chain.AddNode("gain");
        var buffer = Probe();

        // TWO events, so "rejected by INDEX" is testable at all: with a
        // single-event list the index can only ever be 0 and
        // Assert.Contains("0", ...) passes whatever the engine reports.
        var outOfRange = new[]
        {
            JuceDspAutomationEvent.Create(10, node.Id, "gainDb", 3f),
            JuceDspAutomationEvent.Create(Frames, node.Id, "gainDb", 3f),   // index 1
        };
        Assert.Contains("automation event 1 was rejected", Assert.Throws<JuceAudioException>(
            () => chain.ProcessOffline(buffer, 2, SampleRate, outOfRange)).Message);

        var unknown = new[]
        {
            JuceDspAutomationEvent.Create(10, node.Id, "gainDb", 3f),
            JuceDspAutomationEvent.Create(20, node.Id, "notAParam", 3f),   // index 1
        };
        Assert.Contains("1", Assert.Throws<JuceAudioException>(
            () => chain.ProcessOffline(buffer, 2, SampleRate, unknown)).Message);
    }

    // -----------------------------------------------------------------
    // (3) "bypass" is the reserved id that gives automation the footswitch.
    // -----------------------------------------------------------------

    [NativeAvailableFact]
    public void TheReservedBypassId_SilencesTheNodeFromItsFrameOn()
    {
        var chain = _engine!.GetDspChain();
        var node = chain.AddNode("gain");
        node.SetParam("gainDb", 18f);   // loud enough that bypassing it is obvious

        var buffer = Probe();
        var events = new[]
        {
            JuceDspAutomationEvent.Create(HalfFrames, node.Id, "bypass", 1f),
        };
        chain.ProcessOffline(buffer, 2, SampleRate, events);

        // The CHAIN crossfades a bypass over ~10 ms, so guard the boundary the
        // same way the gain case does.
        const int guard = 2400;
        var before = Rms(buffer, guard, HalfFrames - 2 * guard);
        var after  = Rms(buffer, HalfFrames + guard, HalfFrames - 2 * guard);

        Assert.True(before > 1e-4, $"the first half is silent ({before}) — the probe is vacuous");
        var dB = 20.0 * Math.Log10(after / before);
        Assert.InRange(dB, -19.5, -16.5);   // +18 dB of gain removed
    }

    // -----------------------------------------------------------------
    // (4) v35: the RIG guarantee, the seat check and the event cap.
    // -----------------------------------------------------------------

    /// <summary>
    ///     <b>A rejected list leaves the RIG alone, not just the buffer.</b> Events
    ///     go through the live setter path as the render walks its chunks, so until
    ///     v35 a failure part-way left the seat's chain at whatever the last
    ///     applied event set — a wah parked at 0.37, a pedal bypassed — with no way
    ///     for the caller to know or undo it, while the export's own comment
    ///     reasoned carefully about half-processed BUFFERS and not at all about
    ///     half-mutated state. The whole list is now RESOLVED before a single event
    ///     is applied.
    /// </summary>
    [NativeAvailableFact]
    public void ARejectedList_LeavesTheRigUntouched_NotOnlyTheBuffer()
    {
        var chain = _engine!.GetDspChain();
        var node = chain.AddNode("gain");
        node.SetParam("gainDb", 0f);

        var buffer = Probe();
        var events = new[]
        {
            JuceDspAutomationEvent.Create(10, node.Id, "gainDb", 12f),      // would apply
            JuceDspAutomationEvent.Create(20, node.Id, "notAParam", 1f),    // index 1: rejected
        };

        Assert.Throws<JuceAudioException>(() => chain.ProcessOffline(buffer, 2, SampleRate, events));

        Assert.Equal(0f, node.GetParam("gainDb"), 3);
    }

    /// <summary>
    ///     Node ids are engine-global (<c>player * stride + local</c>), so an event
    ///     carrying seat 1's id submitted to a render of seat 0 used to be applied
    ///     to SEAT 1's chain: the <c>player</c> parameter read as a scope and was
    ///     not one. The export refuses while a device is open, which is all that
    ///     kept it from being an audible live bug.
    /// </summary>
    [NativeAvailableFact]
    public void AnEventNamingAnotherSeatsNode_IsRejectedByIndex()
    {
        var seat0 = _engine!.GetDspChain(0);
        var node0 = seat0.AddNode("gain");
        var foreignId = node0.Id + JuceAudioEngine.PlayerNodeIdStride;   // the same local slot on seat 1

        var buffer = Probe();
        var original = (float[])buffer.Clone();
        var events = new[]
        {
            JuceDspAutomationEvent.Create(10, node0.Id, "gainDb", 3f),
            JuceDspAutomationEvent.Create(20, foreignId, "gainDb", 3f),   // index 1
        };

        Assert.Contains("automation event 1 was rejected", Assert.Throws<JuceAudioException>(
            () => seat0.ProcessOffline(buffer, 2, SampleRate, events)).Message);
        Assert.Equal(original, buffer);
    }

    /// <summary>
    ///     An EMPTY buffer plus a populated event list used to be a silent no-op,
    ///     ahead of every other validation in the method — the one shape where a
    ///     caller that got its arguments wrong was told nothing at all.
    /// </summary>
    [NativeAvailableFact]
    public void AnEmptyBuffer_ThrowsRatherThanSilentlyDoingNothing()
    {
        var chain = _engine!.GetDspChain();
        var node = chain.AddNode("gain");
        var events = new[] { JuceDspAutomationEvent.Create(0, node.Id, "gainDb", 3f) };
        Assert.Throws<ArgumentException>(
            () => chain.ProcessOffline(Array.Empty<float>(), 2, SampleRate, events));
    }
}

/// <summary>
///     OFFLINE MEANS OFFLINE: the automated render returns
///     <c>DCA_ERR_DEVICE</c> while a device is OPEN, and that is the single most
///     likely failure a NuGet consumer meets (calling a re-amp while the engine is
///     started). Nothing asserted it — <see cref="DspOfflineAutomationTests" />
///     runs entirely against a STOPPED engine, so it has no
///     <c>NullDevice</c> reference at all and cannot.
/// </summary>
[Collection("NullDevice")]
public sealed class DspOfflineAutomationDeviceRefusalTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    [NativeAvailableFact]
    public void WhileADeviceIsOpen_TheAutomatedRenderIsRefused()
    {
        using var engine = NullDevice.CreateEngine();
        var chain = engine.GetDspChain();
        var node = chain.AddNode("gain");

        var buffer = new float[4096 * 2];
        var events = new[] { JuceDspAutomationEvent.Create(10, node.Id, "gainDb", 3f) };

        var ex = Assert.Throws<JuceAudioException>(
            () => chain.ProcessOffline(buffer, 2, 48000, events));
        Assert.Equal(JuceAudioStatus.Device, ex.Status);
    }
}

/// <summary>
///     <c>dca_dsp_automation_event</c> crosses the ABI BY VALUE, so its size and
///     field offsets are part of the contract — the same reason
///     <c>dca_device_info</c> and <c>dca_xrun_stats</c> are pinned. Native side
///     has the matching <c>static_assert</c>s; an accidental reorder or a
///     padding change marshals garbage with no diagnostic at all.
/// </summary>
[Collection("NativeEngine")]
public sealed class JuceDspAutomationLayoutTests
{
    [Fact]
    public void TheStructMatchesTheHeader()
    {
        Assert.Equal(48, Marshal.SizeOf<JuceDspAutomationEvent>());
        Assert.Equal(0,  (int)Marshal.OffsetOf<JuceDspAutomationEvent>(nameof(JuceDspAutomationEvent.Frame)));
        Assert.Equal(8,  (int)Marshal.OffsetOf<JuceDspAutomationEvent>(nameof(JuceDspAutomationEvent.NodeId)));
        Assert.Equal(12, (int)Marshal.OffsetOf<JuceDspAutomationEvent>(nameof(JuceDspAutomationEvent.Param)));
        Assert.Equal(44, (int)Marshal.OffsetOf<JuceDspAutomationEvent>(nameof(JuceDspAutomationEvent.Value)));
    }

    [Fact]
    public void AnIdThatWouldNotFit_IsRejectedRatherThanTruncated()
    {
        // A silently cut id resolves to nothing, and "nothing happened" is the
        // hardest automation bug there is to see.
        Assert.Throws<ArgumentException>(
            () => JuceDspAutomationEvent.Create(0, 0, new string('x', 33), 1f));
        Assert.Throws<ArgumentException>(() => JuceDspAutomationEvent.Create(0, 0, "", 1f));

        var ok = JuceDspAutomationEvent.Create(7, 3, "gainDb", 1.5f);
        Assert.Equal(32, ok.Param.Length);
        Assert.Equal((byte)'g', ok.Param[0]);
        Assert.Equal((byte)0, ok.Param[6]);   // NUL-terminated inside the field
    }

    /// <summary>
    ///     <b>32 bytes with NO terminator is LEGAL.</b> The factory refused it and
    ///     its own <c>&lt;exception&gt;</c> doc blamed the ABI ("32 bytes INCLUDING
    ///     its terminator") — duplicated validation disagreeing with native in the
    ///     strict direction, on a boundary
    ///     <c>include/dotcharter_audio.h</c> goes out of its way to specify: "a
    ///     caller may legally fill all 32 bytes without a terminator; the engine
    ///     bounds the read", which <c>applyAutomationEvent</c> does by copying into
    ///     a 33-byte local.
    /// </summary>
    [Fact]
    public void AThirtyTwoByteId_IsLegal_BecauseTheFieldNeedsNoTerminator()
    {
        var id = new string('x', 32);
        var ev = JuceDspAutomationEvent.Create(0, 0, id, 1f);
        Assert.Equal(32, ev.Param.Length);
        Assert.All(ev.Param, b => Assert.Equal((byte)'x', b));
    }

    /// <summary>
    ///     The two defensive marshalling paths in
    ///     <c>JuceDspChain.ProcessOffline(..., events)</c>. They exist because
    ///     <see cref="JuceDspAutomationEvent" /> is a MUTABLE PUBLIC struct with a
    ///     <c>byte[]</c> field, so <c>default(JuceDspAutomationEvent)</c> marshals
    ///     a null and a hand-built one can carry any length — and neither had a
    ///     test. A null <c>Param</c> must become the empty id the ABI rejects (by
    ///     index), never a <see cref="NullReferenceException" /> out of the
    ///     marshaller; a wrong-length one must be an
    ///     <see cref="ArgumentException" /> naming the index.
    /// </summary>
    [NativeAvailableFact]
    public void AMalformedEventStruct_IsRejectedManagedSide_NotByTheMarshaller()
    {
        using var engine = new JuceAudioEngine();
        engine.Stop();
        var chain  = engine.GetDspChain();
        var buffer = new float[512 * 2];

        // (a) default(...) — Param is null.
        var ex = Assert.Throws<JuceAudioException>(
            () => chain.ProcessOffline(buffer, 2, 48000, new[] { default(JuceDspAutomationEvent) }));
        Assert.Contains("0", ex.Message);

        // (b) a 16-byte Param.
        var short16 = new JuceDspAutomationEvent
        {
            Frame = 0, NodeId = 0, Param = new byte[16], Value = 1f,
        };
        Assert.Throws<ArgumentException>(
            () => chain.ProcessOffline(buffer, 2, 48000, new[] { short16 }));
    }
}
