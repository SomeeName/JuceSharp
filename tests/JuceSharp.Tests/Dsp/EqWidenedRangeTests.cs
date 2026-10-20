using JuceSharp;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     The <c>eq</c> module's frequency ranges, widened 2026-10 (DotCharter #68) to
///     a four-band studio parametric's: low shelf 20-500 Hz, bell 100-8000 Hz, high
///     shelf 2-20 kHz. Each case renders a settled sine through the module on the
///     OFFLINE rig export and holds the measured gain to the RBJ cookbook response
///     the core is written against (dcdsp_eq.h: shelves at Q 1/sqrt2) — so a value
///     the old 40-400 / 200-4000 / 2000-12000 Hz clamp would have moved lands
///     measurably somewhere else, and the cases at those old ends fail on the
///     pre-#68 DLL.
/// </summary>
[Collection("NativeEngine")]
public sealed class EqWidenedRangeTests : IDisposable
{
    private readonly JuceAudioEngine? _engine;

    public EqWidenedRangeTests()
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

    [NativeAvailableFact]
    public void TheCatalogue_AdvertisesTheWidenedRanges()
    {
        var info = JuceDspModuleCatalog.Find("eq")!;
        (float, float) Range(string id)
        {
            var p = info.Params.Single(x => x.Id == id);
            return (p.Min, p.Max);
        }

        Assert.Equal((20f, 500f), Range("lowFreq"));
        Assert.Equal((100f, 8000f), Range("midFreq"));
        Assert.Equal((2000f, 20000f), Range("highFreq"));
        // Defaults did not move: an existing preset that never set them sounds the same.
        Assert.Equal(new[] { 100f, 1000f, 6000f },
            new[] { "lowFreq", "midFreq", "highFreq" }.Select(id => info.Params.Single(p => p.Id == id).Default));
    }

    [NativeAvailableTheory]
    // (param, corner Hz, gain dB, probe Hz, Q for the bell) at 48 kHz
    [InlineData("low", 20f, 12f, 20f, 0f)]       // below the old 40 Hz floor
    [InlineData("low", 500f, 12f, 250f, 0f)]     // above the old 400 Hz ceiling
    [InlineData("low", 100f, 12f, 100f, 0f)]     // inside the old range: unchanged
    [InlineData("mid", 100f, 12f, 100f, 2f)]     // below the old 200 Hz floor
    [InlineData("mid", 8000f, -12f, 8000f, 2.5f)] // above the old 4 kHz ceiling
    [InlineData("high", 20000f, 12f, 12000f, 0f)] // above the old 12 kHz ceiling
    [InlineData("high", 6000f, 12f, 6000f, 0f)]   // inside the old range: unchanged
    public void EachBand_FollowsTheRbjResponse_AcrossTheWidenedRange(string band, float cornerHz, float gainDb, float probeHz, float q)
    {
        var chain = Engine.GetDspChain();
        chain.Clear();
        var eq = chain.AddNode("eq");
        eq.SetParam(band + "Freq", cornerHz);
        eq.SetParam(band + "GainDb", gainDb);
        if (band == "mid") eq.SetParam("midQ", q);

        var measured = ProbeDb(chain, probeHz, 48000);
        var expected = band switch
        {
            "low" => Rbj.Shelf(false, cornerHz, gainDb, probeHz, 48000),
            "high" => Rbj.Shelf(true, cornerHz, gainDb, probeHz, 48000),
            _ => Rbj.Peak(cornerHz, gainDb, q, probeHz, 48000)
        };
        Assert.True(Math.Abs(measured - expected) < 0.2,
            $"{band} {cornerHz} Hz {gainDb:+0;-0} dB at {probeHz} Hz: measured {measured:F2} dB, RBJ {expected:F2} dB");
    }

    [NativeAvailableFact]
    public void ATwentyKiloHertzShelf_OnA44kDevice_IsHeldAtNinetyPercentOfNyquist()
    {
        // 20 kHz is past 0.9 x 22.05 kHz: the core runs the shelf at 19.845 kHz,
        // so it renders sample for sample like a 19 845 Hz setting — and stays
        // finite — while at 48 kHz the two differ (the guard is not engaged there).
        float[] RenderAt(float highFreq, int rate)
        {
            var chain = Engine.GetDspChain();
            chain.Clear();
            var eq = chain.AddNode("eq");
            eq.SetParam("highFreq", highFreq);
            eq.SetParam("highGainDb", 12f);
            var input = Noise(rate / 2);
            Render(chain, new float[rate / 4], rate); // settle the chain's add crossfade
            return Render(chain, input, rate);
        }

        var asked = RenderAt(20000f, 44100);
        var guarded = RenderAt(19845f, 44100);
        Assert.All(asked, v => Assert.True(float.IsFinite(v)));
        Assert.Equal(guarded, asked);

        Assert.NotEqual(RenderAt(19845f, 48000), RenderAt(20000f, 48000));
    }

    private static float ProbeDb(JuceDspChain chain, float hz, int rate)
    {
        const float amp = 0.25f;
        Render(chain, Sine(rate, 0, amp, hz, rate), rate);
        var outp = Render(chain, Sine(rate, rate, amp, hz, rate), rate);
        return 20f * MathF.Log10(Goertzel(outp, hz, rate) / amp);
    }

    private static float[] Render(JuceDspChain chain, float[] mono, int rate)
    {
        var left = (float[])mono.Clone();
        var right = (float[])mono.Clone();
        chain.ProcessOffline(mono, mono, rate, left, right);
        return left;
    }

    private static float[] Sine(int frames, int offset, float amp, float hz, int rate)
    {
        var buf = new float[frames];
        for (var i = 0; i < frames; i++) buf[i] = amp * MathF.Sin(2f * MathF.PI * hz * (offset + i) / rate);
        return buf;
    }

    private static float[] Noise(int frames)
    {
        var buf = new float[frames];
        var s = 0x9E3779B9u;
        for (var i = 0; i < frames; i++)
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            buf[i] = 0.2f * ((s >> 8) / (float)(1 << 24) * 2f - 1f);
        }

        return buf;
    }

    /// One second of an integer frequency is a whole number of periods, so the
    /// single-bin DFT leaks nothing.
    private static float Goertzel(float[] x, float hz, int rate)
    {
        var w = 2.0 * Math.PI * hz / rate;
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

    /// <summary>The RBJ Audio-EQ-Cookbook magnitude, in dB, at <c>f</c>.</summary>
    private static class Rbj
    {
        public static double Shelf(bool high, double f0, double gainDb, double f, double fs, double q = 0.70710678118654752)
        {
            var a = Math.Pow(10, gainDb / 40);
            var w0 = 2 * Math.PI * f0 / fs;
            var alpha = Math.Sin(w0) / (2 * q);
            var c = Math.Cos(w0);
            var s = 2 * Math.Sqrt(a) * alpha;
            double[] b, d;
            if (!high)
            {
                b = [a * ((a + 1) - (a - 1) * c + s), 2 * a * ((a - 1) - (a + 1) * c), a * ((a + 1) - (a - 1) * c - s)];
                d = [(a + 1) + (a - 1) * c + s, -2 * ((a - 1) + (a + 1) * c), (a + 1) + (a - 1) * c - s];
            }
            else
            {
                b = [a * ((a + 1) + (a - 1) * c + s), -2 * a * ((a - 1) + (a + 1) * c), a * ((a + 1) + (a - 1) * c - s)];
                d = [(a + 1) - (a - 1) * c + s, 2 * ((a - 1) - (a + 1) * c), (a + 1) - (a - 1) * c - s];
            }

            return Mag(b, d, f, fs);
        }

        public static double Peak(double f0, double gainDb, double q, double f, double fs)
        {
            var a = Math.Pow(10, gainDb / 40);
            var w0 = 2 * Math.PI * f0 / fs;
            var alpha = Math.Sin(w0) / (2 * q);
            var c = Math.Cos(w0);
            return Mag([1 + alpha * a, -2 * c, 1 - alpha * a], [1 + alpha / a, -2 * c, 1 - alpha / a], f, fs);
        }

        private static double Mag(double[] b, double[] a, double f, double fs)
        {
            var w = 2 * Math.PI * f / fs;
            System.Numerics.Complex z1 = System.Numerics.Complex.FromPolarCoordinates(1, -w), z2 = z1 * z1;
            var h = (b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2);
            return 20 * Math.Log10(h.Magnitude);
        }
    }
}
