using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceConstantQTests
{
    private const int Sr = 44100;

    private static float[] PureTone(int midi, double seconds)
    {
        int n = (int)(seconds * Sr);
        var y = new float[n];
        float f = Synth.MidiToHz(midi);
        for (int i = 0; i < n; i++) y[i] = 0.5f * MathF.Sin(2 * MathF.PI * f * i / Sr);
        return y;
    }

    // Argmax bin at a given frame, searched within a window of the target.
    private static int PeakBinNear(JuceConstantQ cqt, long frame, int expectedBin, int radius = 8)
    {
        int best = expectedBin;
        float bestMag = -1;
        for (int b = Math.Max(0, expectedBin - radius); b <= Math.Min(cqt.BinCount - 1, expectedBin + radius); b++)
        {
            float m = cqt.Magnitude(b, frame);
            if (m > bestMag) { bestMag = m; best = b; }
        }
        return best;
    }

    [Fact]
    public void BinForMidi_MapsQuarterTones()
    {
        var cqt = new JuceConstantQ();
        Assert.Equal(0, cqt.BinForMidi(40));    // E2
        Assert.Equal(24, cqt.BinForMidi(52));   // E3 (one octave = 24 bins)
        Assert.Equal(2, cqt.BinForMidi(41));    // one semitone = 2 bins
        Assert.Equal(2, cqt.Step);
    }

    [Fact]
    public void Analyze_PlacesMidStringFundamentalInCorrectBin()
    {
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(PureTone(52, 1.0));            // E3
        long frame = cqt.FrameOfSeconds(0.5);
        int bin = cqt.BinForMidi(52);                   // 24
        Assert.Equal(bin, PeakBinNear(cqt, frame, bin));
        // Strict local peak vs its semitone (±step) neighbours — the verify test.
        Assert.True(cqt.Magnitude(bin, frame) >= cqt.Magnitude(bin - cqt.Step, frame));
        Assert.True(cqt.Magnitude(bin, frame) >= cqt.Magnitude(bin + cqt.Step, frame));
    }

    [Fact]
    public void Analyze_SeparatesLowStringQuarterTones()
    {
        // The whole point of the CQT: E2 vs F2 stay distinct on the low string
        // where a linear FFT smears them.
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(PureTone(40, 1.6));            // E2
        long frame = cqt.FrameOfSeconds(0.8);
        Assert.Equal(0, PeakBinNear(cqt, frame, 0, radius: 6));  // peaks at bin 0, not bin 2 (F2)
        Assert.True(cqt.Magnitude(0, frame) > cqt.Magnitude(2, frame) * 1.5f);
    }

    [Fact]
    public void Analyze_WrongFretPeaksTwoBinsAway()
    {
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(PureTone(53, 1.0));            // F3 = E3 + 1 semitone
        long frame = cqt.FrameOfSeconds(0.5);
        Assert.Equal(26, PeakBinNear(cqt, frame, 26));  // (53-40)*2 = 26, two bins above E3's 24
        Assert.True(cqt.Magnitude(26, frame) > cqt.Magnitude(24, frame));
    }

    [Fact]
    public void Floor_IsLowForSilence()
    {
        var cqt = new JuceConstantQ();
        cqt.AnalyzeWhole(new float[Sr]);                // 1 s silence
        int bin = cqt.BinForMidi(52);
        Assert.True(cqt.Floor(bin) < 1e-3f);
        Assert.Equal(0f, cqt.Magnitude(bin, cqt.FrameOfSeconds(0.5)), 3);
    }

    [Fact]
    public void Streaming_AlignsFramesToAbsoluteStreamPosition()
    {
        // Regression for the pause/resume bug: frames must be numbered on the
        // ABSOLUTE (pushed) stream axis, so a note lands at its absolute frame
        // even when the buffer was (re)anchored at a non-zero position.
        const long origin = 500_000;                 // ~11 s at 44.1 kHz
        var y = PureTone(52, 1.0);
        var cqt = new JuceConstantQ();
        cqt.Reset(origin);
        for (int off = 0; off < y.Length; off += 1024)
            cqt.Push(y.AsSpan(off, Math.Min(1024, y.Length - off)), origin + off);

        int bin = cqt.BinForMidi(52);
        long midFrame = (long)Math.Round((origin + y.Length / 2) / 256.0);   // absolute
        Assert.True(cqt.Magnitude(bin, midFrame) > 0.01f, "note not found at its absolute frame");
        // The naive 0-based frame (the old bug's coordinate) must be empty.
        Assert.Equal(0f, cqt.Magnitude(bin, (long)Math.Round((y.Length / 2) / 256.0)), 3);
    }

    [Fact]
    public void Streaming_HandlesNegativeStreamPositions_NoCrash()
    {
        // Regression for the live "0 hits / 0 misses" crash: the transport-aligned
        // push axis goes NEGATIVE during count-in (before t=0). A plain (int)(x%n)
        // gave a negative array index -> IndexOutOfRangeException every tick.
        var y = PureTone(52, 1.0);
        var cqt = new JuceConstantQ();
        const long origin = -20000;   // ~-0.45 s, before the song's t=0
        cqt.Reset(origin);
        for (int off = 0; off < y.Length; off += 1024)
            cqt.Push(y.AsSpan(off, Math.Min(1024, y.Length - off)), origin + off);  // must not throw

        // A frame at positive absolute time (after the count-in) still reads.
        int bin = cqt.BinForMidi(52);
        long midFrame = (long)Math.Round((origin + y.Length / 2) / 256.0);
        Assert.True(cqt.Magnitude(bin, midFrame) > 0.01f);
    }

    [Fact]
    public void Streaming_MatchesOffline()
    {
        var y = PureTone(52, 1.0);

        var offline = new JuceConstantQ();
        offline.AnalyzeWhole(y);

        var live = new JuceConstantQ();
        live.Reset(streamPosition: 0);
        for (int off = 0; off < y.Length; off += 1024)
        {
            int len = Math.Min(1024, y.Length - off);
            live.Push(y.AsSpan(off, len), off);
        }

        int bin = live.BinForMidi(52);
        // A frame comfortably inside the clip and inside the streaming ring.
        long frame = live.NewestFrame - 20;
        Assert.True(frame > 0);
        float a = offline.Magnitude(bin, frame);
        float b = live.Magnitude(bin, frame);
        Assert.True(a > 0.01f, $"offline magnitude too small: {a}");
        Assert.Equal(a, b, 3);   // 3 decimal places — same centered kernels, same samples
    }
}
