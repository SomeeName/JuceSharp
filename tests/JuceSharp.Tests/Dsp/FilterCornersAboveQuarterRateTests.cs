using JuceSharp;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     DotCharter #87: every rig corner whose PARAM advertises a range past a
///     quarter of the sample rate is designed under 0.9 x Nyquist
///     (<c>dcdsp::clampCutoffNearNyquist</c>), not at the shared quarter-rate
///     ceiling (<c>clampCutoff</c>: 12 kHz at 48 kHz) every core was voiced
///     against. Each case renders through the module on the OFFLINE rig export
///     (the same path the app's renders take) and fails on the pre-#87 DLL —
///     where a 16 kHz corner WAS a 12 kHz corner, so the svf read -10 dB at its
///     own cutoff and every 16-vs-12 kHz pair rendered sample-identical. Model:
///     <see cref="EqWidenedRangeTests" />.
/// </summary>
[Collection("NativeEngine")]
public sealed class FilterCornersAboveQuarterRateTests : IDisposable
{
    private const int Rate = 48000;
    private readonly JuceAudioEngine? _engine;

    public FilterCornersAboveQuarterRateTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop(); // the offline render must be the only processor
    }

    public void Dispose()
    {
        if (_engine is null) return;
        try { _engine.GetDspChain().Clear(); } catch (JuceAudioException) { /* engine already gone */ }
        _engine.Dispose();
    }

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    /// <summary>The TPT state-variable low-pass at Q 0.707 is exactly -3.01 dB at
    /// its own corner (bilinear, prewarped) — wherever the corner sits.</summary>
    [NativeAvailableTheory]
    [InlineData(16000f)]
    [InlineData(18000f)]
    [InlineData(1000f)] // under the old ceiling: unchanged
    public void Svf_LowPass_IsThreeDbDown_AtItsOwnCutoff(float cutoffHz)
    {
        var chain = Engine.GetDspChain();
        chain.Clear();
        var svf = chain.AddNode("svf");
        svf.SetParam("mode", 0f);
        svf.SetParam("resonance", 0f);
        svf.SetParam("source", 0f);
        svf.SetParam("mix", 1f);
        svf.SetParam("cutoffHz", cutoffHz);

        var db = ProbeDb(chain, cutoffHz);
        Assert.True(Math.Abs(db + 3.01) < 0.25, $"svf low-pass at {cutoffHz} Hz reads {db:F2} dB at its cutoff, want -3.01");
    }

    /// <summary>The 16 kHz band at +12 dB is +12 dB at 16 kHz — a peaking
    /// section's gain at its centre IS the knob — and a flat graphic EQ is flat
    /// at the top (its safety limiter, a two-tap average below its knee, cost
    /// 3 dB at 12 kHz until the same change).</summary>
    [NativeAvailableFact]
    public void GraphicEq_SixteenKiloHertzBand_IsASixteenKiloHertzBand()
    {
        var chain = Engine.GetDspChain();
        chain.Clear();
        var eq = chain.AddNode("graphiceq");
        const string top = "band16k";
        Assert.Contains(JuceDspModuleCatalog.Find("graphiceq")!.Params, p => p.Id == top);
        eq.SetParam(top, 12f);
        var boosted = ProbeDb(chain, 16000f, 0.02f);
        Assert.True(Math.Abs(boosted - 12.0) < 0.3, $"graphiceq {top} at +12 dB reads {boosted:F2} dB at 16 kHz");

        eq.SetParam(top, 0f);
        foreach (var hz in new[] { 8000f, 12000f, 16000f })
        {
            var flat = ProbeDb(chain, hz);
            Assert.True(Math.Abs(flat) < 0.05, $"a flat graphiceq reads {flat:F2} dB at {hz} Hz");
        }
    }

    /// <summary>A corner past a quarter of the rate is no longer the quarter-rate
    /// corner: each pair rendered sample-identical before #87.</summary>
    [NativeAvailableTheory]
    [InlineData("hallreverb", "dampingKHz", 16f, 12f, "mix=1")]
    [InlineData("gatedreverb", "dampingKHz", 16f, 12f, "mix=1;thresholdDb=-60")]
    [InlineData("reversereverb", "dampingKHz", 16f, 12f, "mix=1")]
    [InlineData("convreverb", "highCutKHz", 18f, 12f, "mix=1")]
    [InlineData("tapesat", "hfRolloffKHz", 16f, 12f, "mix=1")]
    [InlineData("platereverb", "damping", 0f, 0.254f, "mix=1")] // 16 kHz vs 12 kHz damping corner
    public void ACornerPastAQuarterOfTheRate_IsNotHeldThere(string module, string param, float above, float atCeiling, string setup)
    {
        float[] Render(float value)
        {
            var chain = Engine.GetDspChain();
            chain.Clear();
            var node = chain.AddNode(module);
            foreach (var kv in setup.Split(';'))
            {
                var parts = kv.Split('=');
                node.SetParam(parts[0], float.Parse(parts[1], System.Globalization.CultureInfo.InvariantCulture));
            }

            node.SetParam(param, value);
            Process(chain, new float[Rate / 4]); // settle the add crossfade
            return Process(chain, Noise(Rate / 2));
        }

        var high = Render(above);
        var low = Render(atCeiling);
        Assert.All(high, v => Assert.True(float.IsFinite(v)));
        double diff = 0, energy = 0;
        for (var i = 0; i < high.Length; i++)
        {
            diff += (high[i] - low[i]) * (double)(high[i] - low[i]);
            energy += high[i] * (double)high[i];
        }

        Assert.True(energy > 1e-6, $"{module} rendered silence — the case is vacuous");
        Assert.True(diff / energy > 1e-6,
            $"{module}.{param} = {above} renders like {atCeiling}: the corner is still held at a quarter of the rate");
    }

    // ------------------------------------------------------------------ helpers

    private static float ProbeDb(JuceDspChain chain, float hz, float amp = 0.25f)
    {
        Process(chain, Sine(Rate, 0, amp, hz));
        var outp = Process(chain, Sine(Rate, Rate, amp, hz));
        return 20f * MathF.Log10(Goertzel(outp, hz) / amp);
    }

    private static float[] Process(JuceDspChain chain, float[] mono)
    {
        var left = (float[])mono.Clone();
        var right = (float[])mono.Clone();
        chain.ProcessOffline(mono, mono, Rate, left, right);
        return left;
    }

    private static float[] Sine(int frames, int offset, float amp, float hz)
    {
        var buf = new float[frames];
        for (var i = 0; i < frames; i++) buf[i] = amp * MathF.Sin(2f * MathF.PI * hz * (offset + i) / Rate);
        return buf;
    }

    private static float[] Noise(int frames)
    {
        var buf = new float[frames];
        var s = 0x9E3779B9u;
        for (var i = 0; i < frames; i++)
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            buf[i] = 0.2f * ((s >> 8) / (float)(1 << 24) * 2f - 1f);
        }

        return buf;
    }

    /// One second of an integer frequency is a whole number of periods, so the
    /// single-bin DFT leaks nothing.
    private static float Goertzel(float[] x, float hz)
    {
        var w = 2.0 * Math.PI * hz / Rate;
        var coeff = 2.0 * Math.Cos(w);
        double s1 = 0, s2 = 0;
        foreach (var v in x)
        {
            var s = v + coeff * s1 - s2;
            s2 = s1;
            s1 = s;
        }

        var re = s1 - s2 * Math.Cos(w);
        var im = s2 * Math.Sin(w);
        return (float)(2.0 * Math.Sqrt(re * re + im * im) / x.Length);
    }
}
