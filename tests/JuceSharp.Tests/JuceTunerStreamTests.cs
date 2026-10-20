using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     Behaviour pins for <see cref="JuceTunerStream" /> — the streaming, UI-facing
///     wrapper over <see cref="JuceChromaticTuner" />.
///     <para>
///         Every one of these asserts a filter that the raw <c>Detect</c> does NOT
///         have: the median (octave slips), the note hysteresis (name flicker at a
///         transition), the clamped/stable contract (a floor artefact must never read
///         as a measurement) and the allocation pin (this runs per hop on the UI
///         thread). Wall-clock is asserted nowhere.
///     </para>
/// </summary>
[Collection("AllocationProbes")]
public sealed class JuceTunerStreamTests
{
    private const int Sr = 44100;

    private static float[] Sine(float hz, int samples, float amp = 0.4f, int phaseOffset = 0)
    {
        var buf = new float[samples];
        for (var i = 0; i < samples; i++)
            buf[i] = amp * MathF.Sin(2f * MathF.PI * hz * (i + phaseOffset) / Sr);
        return buf;
    }

    private static JuceTunerStream Stream(int window = 2048, int hop = 512, float minFreq = 50f) =>
        new(new JuceChromaticTuner(Sr, window, minFreq), hop);

    /// <summary>Feed in hop-sized blocks and collect every emitted frame.</summary>
    private static List<JuceTunerFrame> Drive(JuceTunerStream s, ReadOnlySpan<float> audio, int block)
    {
        var frames = new List<JuceTunerFrame>();
        for (var i = 0; i + block <= audio.Length; i += block)
        {
            if (s.Push(audio.Slice(i, block)) && s.Latest is { } f) frames.Add(f);
        }
        return frames;
    }

    /// <summary>A2 is MIDI 45 = 110 Hz exactly. The frame must name it, sit on zero
    /// cents, and report stable within a handful of hops — that "within K frames" is
    /// the whole point of the type.</summary>
    [Fact]
    public void SteadyA2_BecomesStableAtZeroCents()
    {
        const int k = 8;
        var s = Stream();
        var frames = Drive(s, Sine(110f, 2048 + k * 512), 512);

        Assert.NotEmpty(frames);
        var stable = frames.FirstOrDefault(f => f.IsStable);
        Assert.True(stable.IsStable, "no stable frame within K hops");
        Assert.Equal("A", stable.NoteName);
        Assert.Equal(2, stable.Octave);
        Assert.Equal(45, stable.MidiNote);
        Assert.InRange(stable.CentsOff, -2f, 2f);
        Assert.InRange(stable.FrequencyHz, 109f, 111f);
        Assert.False(stable.Clamped);
        // …and it happened inside the budget: frame index of the first stable one.
        Assert.True(frames.IndexOf(stable) < k, $"first stable frame was #{frames.IndexOf(stable)}");
    }

    /// <summary>A ten-cent-sharp A2 must read +10, not 0: the median must not quantise
    /// the frequency and the cents must be measured against the held note.</summary>
    [Fact]
    public void TenCentsSharp_IsReportedAsTenCents()
    {
        var hz = 110f * MathF.Pow(2f, 10f / 1200f);
        var s = Stream();
        var frames = Drive(s, Sine(hz, 2048 + 8 * 512), 512);

        var stable = frames.First(f => f.IsStable);
        Assert.Equal("A", stable.NoteName);
        Assert.InRange(stable.CentsOff, 8f, 12f);
    }

    /// <summary>A2 → B2 is 200 cents: below <see cref="JuceTunerStream.ImmediateChangeCents" />,
    /// so it goes through the hysteresis. The displayed name must change EXACTLY once
    /// — no intermediate A#2, no flicker back and forth across the seam.</summary>
    [Fact]
    public void StepFromA2ToB2_ChangesTheNameExactlyOnce()
    {
        var a2 = Sine(110f, Sr);                      // 1 s
        var b2 = Sine(123.4708f, Sr, phaseOffset: Sr); // continues the phase clock
        var audio = new float[a2.Length + b2.Length];
        a2.CopyTo(audio, 0);
        b2.CopyTo(audio, a2.Length);

        var s = Stream();
        var frames = Drive(s, audio, 512);
        var names = frames.Select(f => f.NoteName + f.Octave).ToList();

        var changes = names.Where((n, i) => i > 0 && n != names[i - 1]).ToList();
        Assert.Equal(new[] { "B2" }, changes);
        Assert.Equal("A2", names[0]);

        // Non-vacuity: the hysteresis is what held the name back. With
        // NoteChangeFrames = 1 the SAME audio switches strictly earlier, so the
        // assertion above is measuring the filter and not the source material.
        var fast = Drive(new JuceTunerStream(Sr, 2048, hopSize: 512) { NoteChangeFrames = 1 }, audio, 512);
        var fastNames = fast.Select(f => f.NoteName + f.Octave).ToList();
        Assert.True(fastNames.IndexOf("B2") < names.IndexOf("B2"),
            $"hysteresis changed nothing: {fastNames.IndexOf("B2")} vs {names.IndexOf("B2")}");
    }

    /// <summary>A block of NaN must not latch the tuner off: the scrub counts the bad
    /// samples and the very next good window detects again.</summary>
    [Fact]
    public void NaNBlock_DoesNotLatchTheStreamOff()
    {
        var s = Stream();
        Drive(s, Sine(110f, 2048 + 4 * 512), 512);
        Assert.True(s.Latest is { });

        var poison = new float[512];
        Array.Fill(poison, float.NaN);
        s.Push(poison);
        Assert.Equal(512, s.NonFiniteSamples);

        var frames = Drive(s, Sine(110f, 12 * 512), 512);
        Assert.Contains(frames, f => f is { IsStable: true, NoteName: "A", Octave: 2 });
    }

    /// <summary>Below the window's lag ceiling YIN returns a CONFIDENT wrong answer.
    /// The stream must pass the flag through and must never call such a frame stable
    /// — a bass low E on a 2048-sample window at 44.1 kHz reads ≈43 Hz at high
    /// confidence.</summary>
    [Fact]
    public void ClampedReading_IsNeverStable()
    {
        var s = Stream(window: 2048, hop: 512, minFreq: 25f);
        var frames = Drive(s, Sine(41.2f, 2048 + 16 * 512), 512);

        Assert.NotEmpty(frames);
        Assert.All(frames.Where(f => f.Clamped), f => Assert.False(f.IsStable));
        Assert.Contains(frames, f => f.Clamped);

        // The same note on a window that CAN resolve it is neither clamped nor unstable
        // — which is what proves the flag is tracking the ceiling and not the pitch.
        var wide = Stream(window: 8192, hop: 512, minFreq: 25f);
        var ok = Drive(wide, Sine(41.2f, 8192 + 16 * 512), 512);
        Assert.Contains(ok, f => f is { Clamped: false, IsStable: true, MidiNote: 28 });
    }

    /// <summary>Sustained silence releases the display; a release is not a latch.</summary>
    [Fact]
    public void SustainedSilence_ReleasesTheDisplay()
    {
        var s = Stream();
        Drive(s, Sine(110f, 2048 + 8 * 512), 512);
        Assert.NotNull(s.Latest);

        Drive(s, new float[40 * 512], 512);
        Assert.Null(s.Latest);

        Drive(s, Sine(110f, 2048 + 8 * 512), 512);
        Assert.NotNull(s.Latest);
    }

    /// <summary>The hot path is per-hop on the UI thread: after warm-up, 100 pushes
    /// must allocate nothing at all.</summary>
    [Fact]
    public void Push_DoesNotAllocate()
    {
        var s = Stream();
        var block = Sine(110f, 512);
        for (var i = 0; i < 20; i++) s.Push(block);   // warm up JIT + first window

        var delta = AllocationProbe.Measure(() => { for (var i = 0; i < 100; i++) s.Push(block); });

        Assert.True(s.FramesEmitted > 100, $"nothing was emitted: {s.FramesEmitted}");
        Assert.Equal(0, delta);
    }
}
