using System;
using JuceSharp;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     ABI v34 branch delay compensation. A parallel region whose branches carry
///     different latencies is a COMB FILTER, not a blend — the merger summed
///     x[n] from an empty branch with x[n-L] from a late one, and ABI v23
///     accepted that. These tests read the artefact straight off an IMPULSE,
///     which is the only stimulus where "one arrival" and "two arrivals" are not
///     a matter of opinion.
///     <para>
///         The late branch is a <c>tapestop</c> at its defaults: its own header
///         guarantees 2 samples of latency, always, at every rate, and that at a
///         delay of exactly 2.0 the Lagrange read is BIT-EXACT. So the expected
///         output is not approximately anything — with compensation on there is
///         one impulse at +2, with it off there are two, at +0 and +2, each at
///         half amplitude (the merger's default average mode).
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class DspBranchPdcTests : IDisposable
{
    private const int SampleRate = 48000;
    private const int Frames = 2048;
    private const int ImpulseAt = 100;
    private const int TapeStopLatency = 2;

    private readonly JuceAudioEngine? _engine;

    public DspBranchPdcTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();     // the offline render must be the only processor
    }

    public void Dispose() => _engine?.Dispose();

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    [NativeAvailableFact]
    public void CompensationIsOnByDefault_AndRoundTrips()
    {
        var chain = Engine.GetDspChain();
        Assert.True(chain.BranchLatencyCompensation, "branch PDC was OFF by default");
        chain.BranchLatencyCompensation = false;
        Assert.False(chain.BranchLatencyCompensation);
        chain.BranchLatencyCompensation = true;
        Assert.True(chain.BranchLatencyCompensation);
    }

    [NativeAvailableFact]
    public void ALateBranchAndAnEmptyOne_ArriveAsOneImpulse_WhenCompensated()
    {
        var chain = Engine.GetDspChain();
        try
        {
            BuildSplitRegion(chain);
            Assert.True(chain.BranchLatencyCompensation);

            var wet = RenderImpulse(chain);

            // One arrival, at the region's own latency, carrying BOTH branches.
            Assert.True(wet[ImpulseAt + TapeStopLatency] > 0.9f,
                $"the aligned arrival read {wet[ImpulseAt + TapeStopLatency]:F4}, expected ~1.0");
            // …and nothing before it: an early copy of the impulse IS the comb.
            Assert.True(MathF.Abs(wet[ImpulseAt]) < 0.02f,
                $"the uncompensated branch still arrived early at {wet[ImpulseAt]:F4}");
            Assert.True(MathF.Abs(wet[ImpulseAt + 1]) < 0.02f,
                $"something arrived between the two branches: {wet[ImpulseAt + 1]:F4}");
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void TurningCompensationOff_BringsTheSecondArrivalBack()
    {
        var chain = Engine.GetDspChain();
        try
        {
            BuildSplitRegion(chain);
            chain.BranchLatencyCompensation = false;

            var comb = RenderImpulse(chain);

            // Two arrivals, half amplitude each (the merger averages), exactly
            // TapeStopLatency apart — this is the pre-v34 behaviour, and the
            // assertion above would pass vacuously if the render did nothing.
            Assert.True(comb[ImpulseAt] > 0.4f && comb[ImpulseAt] < 0.6f,
                $"the early (empty-branch) arrival read {comb[ImpulseAt]:F4}, expected ~0.5");
            Assert.True(comb[ImpulseAt + TapeStopLatency] > 0.4f && comb[ImpulseAt + TapeStopLatency] < 0.6f,
                $"the late arrival read {comb[ImpulseAt + TapeStopLatency]:F4}, expected ~0.5");
        }
        finally
        {
            chain.BranchLatencyCompensation = true;
            chain.Clear();
        }
    }

    [NativeAvailableFact]
    public void TheRegionsReportedLatency_IsTheLatestBranch_EitherWay()
    {
        var chain = Engine.GetDspChain();
        try
        {
            BuildSplitRegion(chain);
            RenderImpulse(chain);       // prepare the rig at a known rate first

            Assert.Equal(TapeStopLatency, chain.LatencySamples);
            chain.BranchLatencyCompensation = false;
            Assert.Equal(TapeStopLatency, chain.LatencySamples);
        }
        finally
        {
            chain.BranchLatencyCompensation = true;
            chain.Clear();
        }
    }

    // ---- helpers ----------------------------------------------------------

    /// <summary>splitter → [branch 1: tapestop] [branch 2: empty] → merger.</summary>
    private static void BuildSplitRegion(JuceDspChain chain)
    {
        chain.AddNode("splitter");
        var late = chain.AddNode("tapestop");
        late.Branch = 1;                       // branch 1; branch 2 stays empty
        chain.AddNode("merger");
        Assert.Equal(TapeStopLatency, late.LatencySamples);
    }

    private static float[] RenderImpulse(JuceDspChain chain)
    {
        // One render to let the lazy prepare happen at THIS rate (it re-derives
        // every latency and the compensation with them), then the measured one.
        Render(chain, Impulse());
        var output = Render(chain, Impulse());
        var left = new float[Frames];
        for (var i = 0; i < Frames; ++i) left[i] = output[i * 2];
        return left;
    }

    private static float[] Impulse()
    {
        var buf = new float[Frames * 2];
        buf[ImpulseAt * 2] = 1f;
        buf[ImpulseAt * 2 + 1] = 1f;
        return buf;
    }

    private static float[] Render(JuceDspChain chain, float[] input)
    {
        var output = new float[input.Length];
        chain.ProcessOffline(input, 2, SampleRate, output);
        return output;
    }
}
