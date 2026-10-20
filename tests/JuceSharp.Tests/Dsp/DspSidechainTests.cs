using System;
using JuceSharp;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     ABI v34 node sidechain. The measurable claim is that only the DETECTOR
///     moves: a compressor whose own input is 40 dB down does nothing, and the
///     SAME compressor keyed off the seat's dry input — which is loud — pulls
///     the same quiet signal down further.
///     <para>
///         The chain is <c>gain(−40 dB) → compressor</c> on purpose. Keyed off
///         its own input the compressor sits far below its threshold and is a
///         wire; keyed off the chain's input it is well above it. That makes the
///         two renders each other's A/B on one binary, with one property
///         changed and nothing else — and neither result can be produced by a
///         sidechain that is quietly ignored.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class DspSidechainTests : IDisposable
{
    private const int SampleRate = 48000;
    private const int Frames = 8192;

    private readonly JuceAudioEngine? _engine;

    public DspSidechainTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();     // the offline render must be the only processor
    }

    public void Dispose() => _engine?.Dispose();

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    [NativeAvailableFact]
    public void TheCatalogueAdvertisesExactlyTheModulesThatAcceptAKey()
    {
        var compressor = JuceDspModuleCatalog.Find("compressor");
        var gate = JuceDspModuleCatalog.Find("gate");
        var gain = JuceDspModuleCatalog.Find("gain");
        Assert.NotNull(compressor);
        Assert.NotNull(gate);
        Assert.NotNull(gain);
        Assert.True(compressor!.SupportsSidechain, "the compressor does not advertise a sidechain");
        Assert.True(gate!.SupportsSidechain, "the gate does not advertise a sidechain");
        Assert.False(gain!.SupportsSidechain, "a clean boost advertised a sidechain it has no detector for");
    }

    [NativeAvailableFact]
    public void AModuleThatCannotBeKeyed_RejectsASource()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var gain = chain.AddNode("gain");
            Assert.Equal(JuceDspSidechainSource.None, gain.Sidechain);
            var thrown = Assert.Throws<JuceAudioException>(() => gain.Sidechain = JuceDspSidechainSource.SeatInput);
            Assert.Equal(JuceAudioStatus.InvalidArg, thrown.Status);
            Assert.Equal(JuceDspSidechainSource.None, gain.Sidechain);

            // …and clearing one is always allowed, on any module.
            gain.Sidechain = JuceDspSidechainSource.None;
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void TheKeyRoundTrips_OnAModuleThatAcceptsOne()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var comp = chain.AddNode("compressor");
            Assert.Equal(JuceDspSidechainSource.None, comp.Sidechain);
            comp.Sidechain = JuceDspSidechainSource.SongMix;
            Assert.Equal(JuceDspSidechainSource.SongMix, comp.Sidechain);
            comp.Sidechain = JuceDspSidechainSource.SeatInput;
            Assert.Equal(JuceDspSidechainSource.SeatInput, comp.Sidechain);
            comp.Sidechain = JuceDspSidechainSource.None;
            Assert.Equal(JuceDspSidechainSource.None, comp.Sidechain);
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void ACompressorKeyedFromTheDryInput_CompressesWhatItsOwnInputCannotTrigger()
    {
        var chain = Engine.GetDspChain();
        try
        {
            // gain(-40 dB) -> compressor. The compressor's OWN input is far
            // below its threshold; the chain's input is far above it.
            var pad = chain.AddNode("gain");
            pad.SetParam("gainDb", -40f);
            var comp = chain.AddNode("compressor");
            comp.SetParam("thresholdDb", -24f);
            comp.SetParam("ratio", 20f);
            comp.SetParam("attackMs", 0.1f);
            comp.SetParam("releaseMs", 5f);
            comp.SetParam("kneeDb", 0f);
            comp.SetParam("detector", 1f);

            // Unkeyed: the compressor is a wire (its input is ~-52 dBFS).
            Render(chain);                                   // settle the rig
            var unkeyed = Rms(Render(chain));
            Assert.True(unkeyed > 0f, "the padded signal is silent — the test is vacuous");

            // Keyed off the dry input: the same audio, a loud detector.
            comp.Sidechain = JuceDspSidechainSource.SeatInput;
            Render(chain);
            var keyed = Rms(Render(chain));

            Assert.True(keyed < unkeyed * 0.6f,
                $"keying the compressor off the dry input left the level at {keyed:E3} "
                + $"against {unkeyed:E3} unkeyed — the key is not reaching the detector");
            Assert.True(keyed > 0f, "the keyed compressor silenced the signal outright");
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void AnUnavailableKey_FallsBackToTheModulesOwnInput_NeverToSilence()
    {
        var chain = Engine.GetDspChain();
        try
        {
            // SongMix has no source on the OFFLINE render path. A module keyed
            // to it must behave exactly as if it were unkeyed — a gate keyed
            // from silence would sit closed, and the rig would go quiet for a
            // reason nothing on screen could explain.
            var gate = chain.AddNode("gate");
            gate.SetParam("thresholdDb", -70f);
            Render(chain);
            var unkeyed = Rms(Render(chain));

            gate.Sidechain = JuceDspSidechainSource.SongMix;
            Render(chain);
            var keyed = Rms(Render(chain));

            Assert.True(unkeyed > 0.05f, "the stimulus is too quiet to open the gate — the test is vacuous");
            Assert.Equal(unkeyed, keyed, 3);
        }
        finally { chain.Clear(); }
    }

    // ---- helpers ----------------------------------------------------------

    private static float[] Stimulus()
    {
        var buf = new float[Frames * 2];
        for (var i = 0; i < Frames; ++i)
        {
            var s = 0.5f * MathF.Sin(2f * MathF.PI * 220f * i / SampleRate);
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        return buf;
    }

    private static float[] Render(JuceDspChain chain)
    {
        var input = Stimulus();
        var output = new float[input.Length];
        chain.ProcessOffline(input, 2, SampleRate, output);
        return output;
    }

    private static float Rms(float[] buf)
    {
        double acc = 0;
        foreach (var s in buf) acc += (double)s * s;
        return buf.Length == 0 ? 0f : (float)Math.Sqrt(acc / buf.Length);
    }
}
