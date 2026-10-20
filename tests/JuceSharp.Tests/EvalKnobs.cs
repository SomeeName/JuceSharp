using JuceSharp.Dsp;

namespace JuceSharp.Tests;

/// <summary>
/// The knob set <see cref="CqtStreamingEvalTests"/> runs with.
///
/// <para><b>The defaults ARE the shipping configuration</b> (<see cref="LiveCqtConfig"/>), so an
/// un-set run measures the code the app really runs. The <c>GND_*</c> environment variables
/// exist to EXPLORE alternatives — set one and the harness's own report labels the run
/// "OVERRIDDEN".</para>
///
/// <para><b>Why this is a type and not four local variables.</b> They used to be locals with
/// hard-coded defaults and a comment claiming "all default-OFF so an un-set run reproduces the
/// shipping code". Two of them stopped being true when the sub-octave veto and the 10 s noise
/// horizon were adopted, and nothing could notice: the eval is <c>Assert.Skip</c>-gated on
/// <c>GND_IDMT_ROOT</c>, so it never runs in the gate. As a type with an injectable environment
/// reader, <see cref="CqtEvalConfigTests"/> can pin the defaults against
/// <see cref="LiveCqtConfig"/> in the gate, with no dataset.</para>
/// </summary>
internal readonly record struct EvalKnobs(
    float FloorPercentile,
    double NoiseHistorySeconds,
    double HarmonicPeakBelowHz,
    double SubOctaveVeto,
    float NoiseMargin,
    double IdleSteadySeconds)
{
    /// <summary>Exactly what live play runs.</summary>
    public static EvalKnobs Shipping => new(
        LiveCqtConfig.FloorPercentile,
        LiveCqtConfig.NoiseHistorySeconds,
        LiveCqtConfig.HarmonicPeakBelowHz,
        LiveCqtConfig.SubOctaveVeto,
        LiveCqtConfig.StreamingNoiseMargin,
        LiveCqtConfig.IdleSteadySeconds);

    /// <summary><see cref="IdleSteadySeconds"/> in CQT frames at a sample rate (0 = the idle
    /// floor off).</summary>
    public int IdleSteadyFrames(int sampleRate) => (int)(IdleSteadySeconds * sampleRate / LiveCqtConfig.Hop);

    /// <summary><see cref="NoiseHistorySeconds"/> in CQT frames at a sample rate.</summary>
    public int NoiseHistoryFrames(int sampleRate) => (int)(NoiseHistorySeconds * sampleRate / LiveCqtConfig.Hop);

    /// <summary>Overlay the <c>GND_*</c> overrides on <see cref="Shipping"/>.</summary>
    /// <remarks>
    ///   <list type="bullet">
    ///     <item><description><c>GND_FLOOR_PCT</c> — streaming per-bin floor percentile
    ///     (1.0 = the rolling MAX that ships; measured and rejected below that).</description></item>
    ///     <item><description><c>GND_NOISE_SECONDS</c> — horizon of the streaming noise floor
    ///     (0 = the bare 384-frame ring, i.e. 2.23 s; 10 ships).</description></item>
    ///     <item><description><c>GND_HARM_BELOW</c> — relocate the peak test to a harmonic below
    ///     this Hz (0 = off, which ships).</description></item>
    ///     <item><description><c>GND_SUBOCT</c> — sub-octave veto ratio (0 = off; 0.5 ships).
    ///     MONOPHONIC expectations only.</description></item>
    ///     <item><description><c>GND_NOISE_MARGIN</c> — presence-gate margin (2 ships for live
    ///     play, over the idle floor; 3 is the offline lint's).</description></item>
    ///     <item><description><c>GND_IDLE_SECONDS</c> — the streaming idle floor's window
    ///     (0 = off, the pre-0.57.4 floor; 2 ships).</description></item>
    ///   </list>
    /// </remarks>
    public static EvalKnobs From(Func<string, string?> readEnv)
    {
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        const System.Globalization.NumberStyles f = System.Globalization.NumberStyles.Float;
        var s = Shipping;
        return new EvalKnobs(
            float.TryParse(readEnv("GND_FLOOR_PCT"), f, inv, out var fp) ? fp : s.FloorPercentile,
            double.TryParse(readEnv("GND_NOISE_SECONDS"), f, inv, out var ns) ? ns : s.NoiseHistorySeconds,
            double.TryParse(readEnv("GND_HARM_BELOW"), f, inv, out var hb) ? hb : s.HarmonicPeakBelowHz,
            double.TryParse(readEnv("GND_SUBOCT"), f, inv, out var so) ? so : s.SubOctaveVeto,
            float.TryParse(readEnv("GND_NOISE_MARGIN"), f, inv, out var nm) ? nm : s.NoiseMargin,
            double.TryParse(readEnv("GND_IDLE_SECONDS"), f, inv, out var idle) ? idle : s.IdleSteadySeconds);
    }

    public static EvalKnobs FromEnvironment() => From(Environment.GetEnvironmentVariable);
}
