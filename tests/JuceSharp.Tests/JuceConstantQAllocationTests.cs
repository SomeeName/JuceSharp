using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     Steady-state allocation pins for <see cref="JuceConstantQ" />'s STREAMING path —
///     the one live play runs, at ~172 emitted frames a second.
///     <para>
///         Two sites allocated per frame or per discontinuity and neither had to.
///         <see cref="JuceConstantQ.NoiseFloorEnergy" /> allocated a
///         <c>float[count]</c> and sorted it once per emitted frame (1,722 floats at
///         the shipping 10 s horizon), and <see cref="JuceConstantQ.Reset" /> — which
///         <see cref="JuceConstantQ.Push" /> calls for ANY stream discontinuity, so a
///         resume or a seek reaches it mid-song — rebuilt the whole ring, ~390 arrays
///         and ~340 KB.
///     </para>
///     <para>
///         Both are pure optimisations: every reused buffer is CLEARED, so it is
///         indistinguishable from the freshly allocated one it replaces, and the
///         percentile sort is the same <c>Array.Sort</c> introsort over the same
///         values. The bit-identity half is
///         <see cref="StreamingOutput_IsUnchangedAcrossADiscontinuity" /> plus the
///         untouched <see cref="JuceConstantQRingWrapTests" /> /
///         <see cref="JuceConstantQTests" /> / <see cref="SubOctaveVetoBottomOctaveTests" />.
///     </para>
///     <para>
///         An ALLOCATION COUNT, never a wall clock: it is exact, and it does not flake
///         when two suites run at once.
///     </para>
/// </summary>
[Collection("AllocationProbes")]
public sealed class JuceConstantQAllocationTests
{
    private const int Sr = 44100;

    private static JuceConstantQ Shipping() => new(
        sampleRate: Sr,
        hop: LiveCqtConfig.Hop,
        fMinMidi: LiveCqtConfig.MinMidi,
        binCount: LiveCqtConfig.BinCount,
        maxKernelSamples: LiveCqtConfig.MaxKernelSamples,
        floorPercentile: LiveCqtConfig.FloorPercentile,
        noiseHistoryFrames: LiveCqtConfig.NoiseHistoryFrames(Sr),
        subOctaveBins: LiveCqtConfig.SubOctaveBins);

    private static float[] Tone(int samples, double hz = 110.0)
    {
        var buf = new float[samples];
        for (var i = 0; i < samples; i++)
            buf[i] = 0.3f * MathF.Sin(2f * MathF.PI * (float)hz * i / Sr);
        return buf;
    }

    /// <summary>
    /// After warm-up, pushing audio and reading the noise floor once per emitted
    /// frame must allocate NOTHING. Before the persistent scratch this was one
    /// 1,722-float array per call.
    /// </summary>
    [Fact]
    public void PushingAndReadingTheNoiseFloor_AllocatesNothingInSteadyState()
    {
        var cqt = Shipping();
        cqt.Reset(0);

        var block = Tone(LiveCqtConfig.Hop);
        long pos = 0;

        // Warm up past the first computable frame so the ring, the noise history
        // and the scratch all exist, and so the JIT has compiled everything below.
        for (var i = 0; i < 128; i++, pos += block.Length)
        {
            cqt.Push(block, pos);
            _ = cqt.NoiseFloorEnergy();
        }
        Assert.True(cqt.NewestFrame >= cqt.OldestFrame, "no frames were emitted — the harness never reached steady state");

        var cursor = pos;
        var delta = AllocationProbe.Measure(() =>
        {
            for (var i = 0; i < 200; i++, cursor += block.Length)
            {
                cqt.Push(block, cursor);
                // Read it EVERY frame, defeating the newest-frame cache — that is the
                // shape the verifier drives and the shape that allocated.
                _ = cqt.NoiseFloorEnergy();
            }
        });

        Assert.True(delta == 0,
            $"the streaming path allocated {delta} bytes over 200 frames; it must allocate none");
    }

    /// <summary>
    /// The discontinuity path is the one that rebuilt ~390 arrays. A re-Reset that
    /// does not change the ring size now reuses every buffer.
    /// </summary>
    [Fact]
    public void AReResetAtTheSameSize_AllocatesNothing()
    {
        var cqt = Shipping();
        cqt.Reset(0);
        cqt.Push(Tone(LiveCqtConfig.Hop * 64), 0);
        cqt.Reset(0);   // warm the reuse path + JIT

        var delta = AllocationProbe.Measure(() => { for (var i = 0; i < 8; i++) cqt.Reset(i * 1_000_000L); });

        Assert.True(delta == 0, $"eight same-size Resets allocated {delta} bytes; they must allocate none");
    }

    /// <summary>
    ///     NON-VACUITY, and the bit-identity half in one: a reused buffer must be
    ///     indistinguishable from a fresh one, so the frames produced after a
    ///     discontinuity Reset must match the frames a NEW transform produces from the
    ///     same audio at the same stream origin — exactly, not to a tolerance. A reuse
    ///     that skipped the clear would fail here through the raw ring's stale tail.
    /// </summary>
    [Fact]
    public void StreamingOutput_IsUnchangedAcrossADiscontinuity()
    {
        var audio = Tone(LiveCqtConfig.Hop * 96, hz: 146.83); // D3

        // A: a transform that has already run a whole take and then hit a gap.
        var reused = Shipping();
        reused.Reset(0);
        reused.Push(Tone(LiveCqtConfig.Hop * 96, hz: 220.0), 0);
        reused.Reset(5_000_000);
        reused.Push(audio, 5_000_000);

        // B: a virgin transform fed the same audio at the same origin.
        var fresh = Shipping();
        fresh.Reset(5_000_000);
        fresh.Push(audio, 5_000_000);

        Assert.Equal(fresh.OldestFrame, reused.OldestFrame);
        Assert.Equal(fresh.NewestFrame, reused.NewestFrame);
        Assert.True(fresh.NewestFrame >= fresh.OldestFrame, "no frames emitted — the comparison would be vacuous");
        Assert.Equal(fresh.NoiseFloorEnergy(), reused.NoiseFloorEnergy());

        for (long f = fresh.OldestFrame; f <= fresh.NewestFrame; f++)
        {
            Assert.Equal(fresh.FrameEnergy(f), reused.FrameEnergy(f));
            for (var bin = 0; bin < fresh.BinCount; bin++)
                Assert.Equal(fresh.Magnitude(bin, f), reused.Magnitude(bin, f));
        }
    }
}
