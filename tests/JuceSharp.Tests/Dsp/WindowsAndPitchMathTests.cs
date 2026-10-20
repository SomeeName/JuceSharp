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
