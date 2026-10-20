using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceBeatDetectorTests
{
    [Fact]
    public void Analyze_FindsBpmOfClickTrack()
    {
        const int sr = 22050;
        const float bpm = 120f;
        int samplesPerBeat = (int)(60f / bpm * sr);
        int totalBeats = 32;
        var audio = new float[samplesPerBeat * totalBeats];
        for (int b = 0; b < totalBeats; b++)
        {
            int start = b * samplesPerBeat;
            for (int i = 0; i < 200 && start + i < audio.Length; i++)
                audio[start + i] = MathF.Exp(-i / 30f) * MathF.Sin(2 * MathF.PI * 1000f * i / sr);
        }

        var det = new JuceBeatDetector(sr);
        var r = det.Analyze(audio);

        // Allow octave error (autocorrelation often locks on 1/2 or 2x for uniform clicks).
        float[] candidates = [bpm, bpm / 2f, bpm * 2f];
        Assert.Contains(candidates, c => MathF.Abs(c - r.Bpm) < 3f);
        Assert.NotEmpty(r.Beats);
    }
}
