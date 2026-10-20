using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The guard that stops the measuring instrument from drifting away from the code it measures.
///
/// <para><c>CqtStreamingEvalTests</c> is the harness the agent guides name as THE way to measure
/// detection, and it is <c>Assert.Skip</c>-gated on <c>GND_IDMT_ROOT</c> — so it never runs in
/// the gate, and for months nobody noticed that its defaults had stopped reproducing the
/// shipping configuration (sub-octave veto off, 2.23 s noise horizon, both long since adopted).
/// Anyone reading its un-set output was reasoning about a config the app does not run, and
/// would have seen the pre-veto ~89.7% octave-down decoy rate as a live regression.</para>
///
/// <para>These tests need <b>no dataset</b> and cost nothing: they assert the harness's defaults
/// ARE <see cref="LiveCqtConfig"/>, so the two can never separate again without a red test.</para>
/// </summary>
public sealed class CqtEvalConfigTests
{
    [Fact]
    public void EvalDefaults_AreTheShippingConfiguration()
    {
        // No env vars set ⇒ the harness measures exactly what ships.
        Assert.Equal(EvalKnobs.Shipping, EvalKnobs.From(_ => null));
    }

    [Fact]
    public void ShippingKnobs_EqualLiveCqtConfig()
    {
        var s = EvalKnobs.Shipping;
        Assert.Equal(LiveCqtConfig.FloorPercentile, s.FloorPercentile);
        Assert.Equal(LiveCqtConfig.NoiseHistorySeconds, s.NoiseHistorySeconds);
        Assert.Equal(LiveCqtConfig.HarmonicPeakBelowHz, s.HarmonicPeakBelowHz);
        Assert.Equal(LiveCqtConfig.SubOctaveVeto, s.SubOctaveVeto);
        Assert.Equal(LiveCqtConfig.StreamingNoiseMargin, s.NoiseMargin);
        Assert.Equal(LiveCqtConfig.IdleSteadySeconds, s.IdleSteadySeconds);
    }

    /// <summary>
    /// The other half, and the one that was missing: the harness was compared to the CONFIG,
    /// never the PRODUCT DEFAULTS, so a re-typed literal in <see cref="JuceConstantQ"/> or
    /// <see cref="JuceChartVerifier"/> was invisible to every test. Three had drifted back
    /// into literals (<c>NoiseMargin = 3f</c>, <c>hop = 256</c>, <c>floorPercentile = 1.0f</c>).
    /// </summary>
    [Fact]
    public void ShippingDefaults_AreLiveCqtConfig_NotRetypedLiterals()
    {
        var cqt = new JuceConstantQ();
        Assert.Equal(LiveCqtConfig.Hop, cqt.Hop);
        Assert.Equal(LiveCqtConfig.FloorPercentile, cqt.FloorPercentile);
        Assert.Equal(LiveCqtConfig.RelFloor, cqt.RelFloor);

        Assert.Equal(LiveCqtConfig.NoiseMargin, new JuceChartVerifier(cqt).NoiseMargin);
    }

    /// <summary>
    /// …and the value check above cannot see the fault while the two numbers still AGREE,
    /// which is the whole point: a re-typed literal is identical right up to the moment
    /// somebody changes <see cref="LiveCqtConfig"/> and it silently does not follow. So
    /// lint the SOURCE — the shipping defaults must NAME the constant.
    /// </summary>
    [Theory]
    [InlineData("JuceConstantQ.cs", "int hop = LiveCqtConfig.Hop")]
    [InlineData("JuceConstantQ.cs", "float floorPercentile = LiveCqtConfig.FloorPercentile")]
    [InlineData("JuceConstantQ.cs", "float relFloor = LiveCqtConfig.RelFloor")]
    [InlineData("JuceChartVerifier.cs", "NoiseMargin { get; init; } = LiveCqtConfig.NoiseMargin")]
    public void ShippingDefaults_NameLiveCqtConfig_RatherThanRetypingIt(string fileName, string expected)
    {
        var path = Path.Combine(RepoRoot(), "src", "JuceSharp.Dsp", fileName);
        Assert.True(File.Exists(path), path);
        Assert.Contains(expected, File.ReadAllText(path), StringComparison.Ordinal);
    }

    /// <summary>
    /// <c>JuceGuitarAnalyzer</c>'s legato floor re-DERIVED the tuner's lag ceiling instead
    /// of asking for it, and got it wrong below 30.7 kHz. Ask the object that owns it.
    /// </summary>
    [Fact]
    public void LegatoShortFloor_AsksTheTuner_RatherThanReDerivingItsGeometry()
    {
        var src = File.ReadAllText(Path.Combine(RepoRoot(), "src", "JuceSharp.Dsp", "JuceGuitarAnalyzer.cs"));
        Assert.Contains("_legatoShortFloorHz = LegatoShortFloorMargin * _yinLegato.EffectiveMinFrequency;",
                        src, StringComparison.Ordinal);
    }

    private static string RepoRoot()
    {
        var dir = AppContext.BaseDirectory;
        while (dir is not null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        Assert.NotNull(dir);
        return dir!;
    }

    [Fact]
    public void AdoptedSettings_AreOn_RejectedSettings_AreOff()
    {
        // The two MEASURED-AND-ADOPTED settings. A default of 0 for either is the exact
        // drift this file exists to catch — it is what the harness shipped with.
        Assert.Equal(0.5, LiveCqtConfig.SubOctaveVeto);
        Assert.Equal(10.0, LiveCqtConfig.NoiseHistorySeconds);
        // 10 s at hop 256 = 1722.66 frames, truncated. Emphatically not 384 (the bare ring).
        Assert.Equal(1722, LiveCqtConfig.NoiseHistoryFrames(44100));
        Assert.Equal(1875, LiveCqtConfig.NoiseHistoryFrames(48000));

        // The three MEASURED-AND-REJECTED ones, whose XML docs say "must stay off".
        Assert.Equal(1.0f, LiveCqtConfig.FloorPercentile);   // 1.0 = rolling max = off
        Assert.Equal(0.0, LiveCqtConfig.HarmonicPeakBelowHz);
        Assert.Equal(0, LiveCqtConfig.HarmonicComb);
    }

    [Fact]
    public void EnvironmentOverrides_ApplyOverTheShippingDefaults()
    {
        // The knobs still explore — that is what they are for; they just no longer decide
        // what "un-set" means.
        var k = EvalKnobs.From(name => name switch
        {
            "GND_SUBOCT" => "0",
            "GND_NOISE_SECONDS" => "0",
            _ => null,
        });
        Assert.Equal(0.0, k.SubOctaveVeto);
        Assert.Equal(0, k.NoiseHistoryFrames(44100));          // 0 ⇒ the bare frame ring
        Assert.Equal(LiveCqtConfig.StreamingNoiseMargin, k.NoiseMargin); // untouched knobs keep shipping
    }

    [Fact]
    public void EnvironmentOverrides_AreCultureInvariant()
    {
        // A decimal comma must never be read as "0.5 → 5" on a de-DE machine.
        var k = EvalKnobs.From(name => name == "GND_SUBOCT" ? "0.75" : null);
        Assert.Equal(0.75, k.SubOctaveVeto);
    }
}
