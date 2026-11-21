using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// Part D. Seven classes hand-rolled the same Hann loop and seven re-typed the
/// same equal-tempered conversions. Collapsing them is only safe if it is
/// BIT-identical, because several of those sites are on the detection path
/// whose numbers are pinned elsewhere — so that is what these assert, with
/// <c>Assert.Equal</c> on floats and no tolerance anywhere.
/// </summary>
public sealed class WindowsAndPitchMathTests
{
    /// <summary>
    /// The two spellings the codebase used — <c>0.5f - 0.5f*cos(x)</c> and
    /// <c>0.5f*(1f - cos(x))</c> — are bit-identical in IEEE float: halving is
    /// exact for a power of two, so the rounding of the subtraction is
    /// scale-invariant. This is the claim the migration rests on.
    /// </summary>
    [Theory]
    [InlineData(1024)]
    [InlineData(2048)]
    [InlineData(4096)]
    public void Hann_MatchesBothHandRolledSpellings_Exactly(int n)
    {
        var shared = Windows.Hann(n);
        for (var i = 0; i < n; i++)
        {
            var subtractForm = 0.5f - 0.5f * MathF.Cos(2f * MathF.PI * i / (n - 1));
            var factorForm   = 0.5f * (1f - MathF.Cos(2f * MathF.PI * i / (n - 1)));
            Assert.Equal(subtractForm, shared[i]);
            Assert.Equal(factorForm, shared[i]);
        }
    }

    [Fact]
    public void Hann_IsCachedPerSizeAndForm()
    {
        Assert.Same(Windows.Hann(512), Windows.Hann(512));
        Assert.NotSame(Windows.Hann(512), Windows.Hann(512, periodic: true));
        Assert.NotSame(Windows.Hann(512), Windows.Hann(256));
    }

    /// <summary>Window lengths follow the audio length, so the cache cannot keep one array per
    /// length it was ever asked for: it holds a bounded number of bytes, and a length that no
    /// longer fits is built for the caller without being kept.</summary>
    [Fact]
    public void Hann_CacheRetainedBytesAreBounded()
    {
        const int lengths = 400;
        const int first = 100_000;   // 400 distinct lengths of 100k+ samples: about 160 MB if all were kept
        for (var i = 0; i < lengths; i++)
        {
            var n = first + i;
            var w = Windows.Hann(n);
            Assert.Equal(n, w.Length);
            Assert.Equal(0f, w[0]);
        }

        Assert.True(Windows.RetainedBytesForTest() <= Windows.MaxRetainedBytes,
            $"the cache holds {Windows.RetainedBytesForTest()} bytes, over its {Windows.MaxRetainedBytes} budget");
        Assert.Same(Windows.Hann(512), Windows.Hann(512));   // the small, reused lengths stay cached
    }

    /// <summary>A window over the per-window limit is built for each call and takes nothing
    /// from the cache budget.</summary>
    [Fact]
    public void Hann_WindowOverThePerWindowLimitIsNotKept()
    {
        var n = Windows.MaxCachedWindowSamples + 1;
        var before = Windows.RetainedBytesForTest();
        var a = Windows.Hann(n);
        var b = Windows.Hann(n);
        Assert.NotSame(a, b);
        Assert.Equal(n, a.Length);
        Assert.Equal(before, Windows.RetainedBytesForTest());
    }

    /// <summary>Many one-off long windows must not crowd out the short lengths every STFT asks for.</summary>
    [Fact]
    public void Hann_ShortLengthStaysCachedAfterManyDistinctLongLengths()
    {
        // Windows of this size are over the per-window limit: none of them may be kept, so the
        // budget is as free afterwards as before and the short length below still fits.
        var before = Windows.RetainedBytesForTest();
        for (var i = 0; i < 400; i++)
            Assert.Equal(0f, Windows.Hann(70_000 + i)[0]);

        Assert.Equal(before, Windows.RetainedBytesForTest());
        const int shortLength = 1237;
        Assert.Same(Windows.Hann(shortLength), Windows.Hann(shortLength));
    }

    /// <summary>Both precisions are kept ON PURPOSE — several callers are on a
    /// per-frame path where the float rounding IS the shipped behaviour.</summary>
    [Fact]
    public void PitchMath_ReproducesEachSitesExactExpression()
    {
        foreach (var hz in new[] { 41.20f, 82.41f, 220f, 440f, 1318.5f })
        {
            Assert.Equal(69f + 12f * MathF.Log2(hz / 440f), PitchMath.HzToMidiF(hz));
            Assert.Equal(1200f * MathF.Log2(hz / 440f), PitchMath.CentsF(hz, 440f));
        }
        foreach (var midi in new[] { 28.0, 40.0, 69.0, 88.5 })
            Assert.Equal(440.0 * Math.Pow(2.0, (midi - 69.0) / 12.0), PitchMath.MidiToHz(midi));
    }

    [Theory]
    [InlineData(60, "C", 4)]
    [InlineData(69, "A", 4)]
    [InlineData(28, "E", 1)]
    [InlineData(0, "C", -1)]
    public void PitchMath_NamesAndOctavesMatchScientificPitchNotation(int midi, string name, int octave)
    {
        Assert.Equal(name, PitchMath.NoteName(midi));
        Assert.Equal(octave, PitchMath.Octave(midi));
    }

    [Fact]
    public void PitchClass_RoundsToTheNearestSemitoneAndStaysInRange()
    {
        for (var midi = -12f; midi <= 127f; midi += 0.37f)
        {
            var pc = PitchMath.PitchClass(midi);
            Assert.InRange(pc, 0, 11);
            Assert.Equal(((int)MathF.Round(midi) % 12 + 12) % 12, pc);
        }
    }
}
