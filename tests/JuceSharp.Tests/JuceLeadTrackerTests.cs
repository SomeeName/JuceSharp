using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceLeadTrackerTests
{
    private const int Block = 1024;

    private static List<JuceLeadFrame> Run(float[] y)
    {
        var lt = new JuceLeadTracker(blockSize: Block);
        var frames = new List<JuceLeadFrame>();
        for (int off = 0; off < y.Length; off += Block)
        {
            int len = Math.Min(Block, y.Length - off);
            frames.Add(lt.ProcessBlock(y.AsSpan(off, len)));
        }
        return frames;
    }

    private static int Count(List<JuceLeadFrame> frames, JuceLeadTechnique t)
        => frames.Count(f => f.Technique == t);

    [Fact]
    public void DetectsBend()
    {
        var (y, _) = Synth.Bend(52, 2.0);
        Assert.True(Count(Run(y), JuceLeadTechnique.Bend) >= 5, "bend not detected");
    }

    [Fact]
    public void DetectsVibrato()
    {
        var y = Synth.Vibrato(55);
        Assert.True(Count(Run(y), JuceLeadTechnique.Vibrato) >= 5, "vibrato not detected");
    }

    [Fact]
    public void DetectsSteady()
    {
        var (y, _) = Synth.RenderNotes(new[] { 55 });
        Assert.True(Count(Run(y), JuceLeadTechnique.Steady) >= 5, "steady pitch not detected");
    }

    [Fact]
    public void SilenceIsSilent()
    {
        var frames = Run(new float[Synth.SampleRate / 2]);   // 0.5 s silence
        Assert.All(frames, f => Assert.Equal(JuceLeadTechnique.Silent, f.Technique));
        Assert.All(frames, f => Assert.Null(f.Midi));
    }

    [Fact]
    public void BendReachesTargetPitch()
    {
        // The tracked pitch should climb to roughly the bend's target MIDI.
        var (y, target) = Synth.Bend(52, 2.0);
        var voiced = Run(y).Where(f => f.Midi is not null).Select(f => f.Midi!.Value).ToList();
        Assert.NotEmpty(voiced);
        Assert.True(voiced.Max() >= target - 0.6f, $"bend top {voiced.Max():0.0} short of target {target:0.0}");
    }
}
