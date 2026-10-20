using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     Wave-2 audit finding #19. <see cref="JuceChromagram.ApplyHannWindow" /> defaults
///     to <c>true</c>, and <see cref="JuceGuitarAnalyzer" /> builds both of its
///     chromagrams with no override — so the <c>Chroma</c> vector of every emitted
///     <c>JuceGuitarEvent</c> MOVED the day that default landed, with no baseline update
///     and no test: the flag had zero references anywhere under <c>tests/</c>.
///     <para>
///         The decision was to KEEP the windowing (it is deliberate — an arbitrary frame
///         boundary otherwise sprays a strong partial across every pitch class) and to
///         pin BOTH branches here so the next flip cannot be silent. The comment that
///         sat next to it ("this migration does not move a single analyzer or CQT
///         number") has been scoped to the Hann REFACTOR, which is the only thing it was
///         ever true of.
///     </para>
///     <para>
///         <b>The baseline is DERIVED, not captured.</b> The stimulus is a sine sitting
///         exactly on FFT bin 40 at 4096/44.1 kHz — 430.664 Hz, which rounds to MIDI 69,
///         pitch class 9 (A). For that one stimulus both branches have an analytic
///         answer, which is what makes these numbers a pin rather than a photograph of
///         whatever the code happened to produce:
///     </para>
///     <list type="bullet">
///     <item><b>Unwindowed</b> (rectangular): a bin-centred sine is a single DFT spike.
///       All the energy is in bin 40, so chroma is 1.0 at A and 0 everywhere else, down
///       to float32 FFT noise.</item>
///     <item><b>Hann-windowed</b>: the window's main lobe spreads that spike over bins
///       39/40/41 with amplitudes 0.25/0.5/0.25. Bins 40 and 41 (430.66 and 441.43 Hz)
///       both round to MIDI 69 = A; bin 39 (419.90 Hz) rounds to MIDI 68 = G#. So chroma
///       is ≈0.75 at A and ≈0.25 at G# — a DIFFERENT vector, which is exactly the move
///       the finding is about.</item>
///     </list>
///     <para>
///         The actual values are also logged, so a future tightening does not need a
///         re-derivation.
///     </para>
/// </summary>
public sealed class Wave2ChromagramBaselineTests
{
    private const int Sr = 44100;
    private const int N  = 4096;

    /// <summary>Exactly a bin centre: 40 · 44100 / 4096 = 430.664 Hz.</summary>
    private const int BinCentredK = 40;

    private const int PcA      = 9;   // MIDI 69
    private const int PcGSharp = 8;   // MIDI 68

    private readonly ITestOutputHelper _out;

    public Wave2ChromagramBaselineTests(ITestOutputHelper output) => _out = output;

    /// <summary>A sine at exactly <paramref name="k"/> cycles per frame, so the DFT of
    /// the unwindowed frame is a single spike at bin <paramref name="k"/>.</summary>
    private static float[] BinCentredSine(int k)
    {
        var frame = new float[N];
        for (var i = 0; i < N; i++) frame[i] = MathF.Sin(2f * MathF.PI * k * i / N);
        return frame;
    }

    /// <summary>The C-major triad the pre-existing <c>JuceChromagramTests</c> uses — a
    /// realistic frame whose partials are NOT on bin centres.</summary>
    private static float[] CMajorTriad()
    {
        var frame = new float[N];
        float[] freqs = [261.63f, 329.63f, 392.0f];
        foreach (var f in freqs)
            for (var i = 0; i < N; i++)
                frame[i] += MathF.Sin(2f * MathF.PI * f * i / Sr) / freqs.Length;
        return frame;
    }

    private static string Format(float[] chroma) =>
        string.Join(", ", chroma.Select((v, i) => $"{PitchMath.NoteNames[i]}={v:F5}"));

    /// <summary>The default IS windowing — the analyzer inherits it, and that is the
    /// whole reason the finding exists.</summary>
    [Fact]
    public void ApplyHannWindow_DefaultsToTrue_WhichIsWhatTheAnalyzerInherits()
    {
        Assert.True(new JuceChromagram(Sr, N).ApplyHannWindow);

        // …and the default really is the same code path as the explicit `true`,
        // bit for bit. If these ever differ, one of them is not the shipped one.
        var frame = CMajorTriad();
        Assert.Equal(new JuceChromagram(Sr, N).Compute(frame),
                     new JuceChromagram(Sr, N) { ApplyHannWindow = true }.Compute(frame));
    }

    /// <summary>The UNWINDOWED branch, against its analytic answer.</summary>
    [Fact]
    public void Unwindowed_BinCentredSine_IsASinglePitchClass()
    {
        var chroma = new JuceChromagram(Sr, N) { ApplyHannWindow = false }.Compute(BinCentredSine(BinCentredK));
        _out.WriteLine($"unwindowed: {Format(chroma)}");

        Assert.True(chroma[PcA] > 0.99f,
            $"a bin-centred sine is a single DFT spike; A should carry ~all of it, was {chroma[PcA]:F5}");
        for (var i = 0; i < 12; i++)
        {
            if (i == PcA) continue;
            Assert.True(chroma[i] < 0.01f, $"leakage into {PitchMath.NoteNames[i]} was {chroma[i]:F5}");
        }
        Assert.Equal(1.0, (double)chroma.Sum(), 4);
    }

    /// <summary>The WINDOWED branch — the one that ships — against ITS analytic answer.
    /// The Hann main lobe splits the same spike across two pitch classes in a 3:1
    /// ratio, which is the concrete shape of "every analyzer event's chroma moved".</summary>
    [Fact]
    public void Windowed_BinCentredSine_SplitsAcrossTheMainLobesTwoPitchClasses()
    {
        var chroma = new JuceChromagram(Sr, N).Compute(BinCentredSine(BinCentredK));
        _out.WriteLine($"windowed: {Format(chroma)}");

        // 0.5 / (0.5 + 0.25 + 0.25) = 0.75 at A (bins 40 + 41), 0.25 at G# (bin 39).
        Assert.InRange(chroma[PcA], 0.68f, 0.82f);
        Assert.InRange(chroma[PcGSharp], 0.16f, 0.30f);
        Assert.True(chroma[PcA] + chroma[PcGSharp] > 0.90f,
            $"the Hann main lobe is three bins wide; the sidelobes took {1f - chroma[PcA] - chroma[PcGSharp]:F5}");
        Assert.Equal(1.0, (double)chroma.Sum(), 4);
    }

    /// <summary>Non-vacuity for both of the above, and the finding itself in one line:
    /// the flag CHANGES the output, on a realistic frame as well as on the contrived
    /// one. A test that pinned only the shipping branch could not tell a broken flag
    /// from a working one.</summary>
    [Fact]
    public void BothBranches_ProduceDifferentVectors_OnRealisticAndOnBinCentredFrames()
    {
        foreach (var (name, frame) in new[] { ("triad", CMajorTriad()), ("bin-centred", BinCentredSine(BinCentredK)) })
        {
            var windowed   = new JuceChromagram(Sr, N).Compute(frame);
            var unwindowed = new JuceChromagram(Sr, N) { ApplyHannWindow = false }.Compute(frame);

            var worst = 0f;
            for (var i = 0; i < 12; i++) worst = MathF.Max(worst, MathF.Abs(windowed[i] - unwindowed[i]));
            _out.WriteLine($"{name}: worst |windowed - unwindowed| = {worst:F5}");
            Assert.True(worst > 1e-3f,
                $"{name}: ApplyHannWindow changed nothing (worst delta {worst:E3}) — the flag is not wired");
        }
    }

    /// <summary>Determinism: the same frame through two instances is bit-identical, so
    /// the numbers above are a baseline and not a sampling of float noise.</summary>
    [Fact]
    public void Compute_IsDeterministicAcrossInstances()
    {
        var frame = CMajorTriad();
        Assert.Equal(new JuceChromagram(Sr, N).Compute(frame), new JuceChromagram(Sr, N).Compute(frame));

        // The allocation-free overload is the same numbers as the allocating one.
        var into = new float[12];
        new JuceChromagram(Sr, N).Compute(frame, into);
        Assert.Equal(new JuceChromagram(Sr, N).Compute(frame), into);
    }
}
