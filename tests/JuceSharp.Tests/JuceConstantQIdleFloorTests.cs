using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The streaming IDLE floor (<see cref="JuceConstantQ.IdleSteadyFrames"/>) that the live
/// presence gate compares against. Every case runs the shipping configuration next to the
/// legacy one (<c>idleSteadyFrames: 0</c>) on the same audio, so each assertion shows the
/// difference it exists for, not just a number.
/// </summary>
public sealed class JuceConstantQIdleFloorTests
{
    private const int Sr = 48000;
    private const int Block = 1600;

    private static JuceConstantQ Streaming(bool idleFloor)
    {
        var cqt = new JuceConstantQ(Sr, hop: LiveCqtConfig.Hop, fMinMidi: LiveCqtConfig.MinMidi,
            binCount: LiveCqtConfig.BinCount, maxKernelSamples: LiveCqtConfig.MaxKernelSamples,
            noiseHistoryFrames: LiveCqtConfig.NoiseHistoryFrames(Sr),
            subOctaveBins: LiveCqtConfig.SubOctaveBins,
            idleSteadyFrames: idleFloor ? LiveCqtConfig.IdleSteadyFrames(Sr) : 0);
        cqt.Reset(0);
        return cqt;
    }

    private static void Push(JuceConstantQ cqt, float[] audio, ref long pos)
    {
        for (var off = 0; off < audio.Length; off += Block)
        {
            var n = Math.Min(Block, audio.Length - off);
            cqt.Push(audio.AsSpan(off, n), pos);
            pos += n;
        }
    }

    // Pickup hiss plus mains hum: what an idle guitar input carries.
    private static float[] Idle(double seconds, float gain, int seed)
    {
        var rng = new Random(seed);
        var buf = new float[(int)(seconds * Sr)];
        double lp = 0;
        for (var i = 0; i < buf.Length; i++)
        {
            var t = i / (double)Sr;
            lp += 0.5 * ((rng.NextDouble() * 2 - 1) - lp);
            var hum = 0.0;
            for (var h = 1; h <= 6; h++) hum += Math.Sin(2 * Math.PI * 50 * h * t + h) / h;
            buf[i] = (float)((0.0008 * lp + 0.0012 * hum) * gain);
        }
        return buf;
    }

    // A riff that never stops: a new note every 150 ms, each ringing for 1.5 s, so the
    // input is never quieter than the playing — the case a 10 s low percentile gets wrong.
    private static float[] ContinuousRiff(double seconds, int seed)
    {
        var buf = Idle(seconds, 1f, seed);
        var rng = new Random(seed + 1);
        var step = (int)(0.15 * Sr);
        for (var start = 0; start < buf.Length; start += step)
        {
            var f0 = 82.41 * Math.Pow(2, rng.Next(0, 17) / 12.0);
            var len = Math.Min((int)(1.5 * Sr), buf.Length - start);
            for (var i = 0; i < len; i++)
            {
                var t = i / (double)Sr;
                double s = 0;
                for (var h = 1; h <= 6; h++) s += Math.Sin(2 * Math.PI * f0 * h * t) / h;
                buf[start + i] += (float)(0.2 * s * (1 - Math.Exp(-t * 400)) * Math.Exp(-t * 1.5));
            }
        }
        return buf;
    }

    [Fact]
    public void DuringContinuousPlaying_TheFloorStaysAtTheIdleNoise_NotAtThePlayingLevel()
    {
        float Run(bool idleFloor, out float idleLevel)
        {
            var cqt = Streaming(idleFloor);
            long pos = 0;
            Push(cqt, Idle(3, 1f, 1), ref pos);
            idleLevel = cqt.NoiseFloorEnergy();
            Push(cqt, ContinuousRiff(15, 2), ref pos);
            return cqt.NoiseFloorEnergy();
        }

        var legacy = Run(idleFloor: false, out var legacyIdle);
        var shipping = Run(idleFloor: true, out var idle);

        Assert.True(idle > 0f, "no floor was measured over the idle second");
        // The legacy percentile has climbed to the riff itself — the reason the old gate
        // refused perfectly played notes.
        Assert.True(legacy > 5f * legacyIdle,
            $"legacy floor {legacy:G3} did not climb above the idle level {legacyIdle:G3}; the case is not exercised");
        // The idle floor has not: it is still the noise the player is playing over.
        Assert.True(shipping < 1.5f * idle,
            $"idle floor {shipping:G3} climbed with the playing (idle level {idle:G3}, legacy {legacy:G3})");
    }

    /// <summary>Non-stop 8th-note strumming of a ringing open G chord: the realistic version of
    /// the riff above, and the one that caught <c>LiveCqtConfig.IdleSteadyRatio</c> at 1.5 once
    /// the kernel went to 8192 — the smoothed energy of a steady strum read as "idle", and the
    /// floor rose ~165x to the playing level.</summary>
    [Fact]
    public void DuringContinuousStrumming_TheFloorStaysAtTheIdleNoise()
    {
        float Run(bool idleFloor, out float idleLevel)
        {
            var cqt = Streaming(idleFloor);
            long pos = 0;
            Push(cqt, Idle(3, 1f, 1), ref pos);
            idleLevel = cqt.NoiseFloorEnergy();
            var strum = Idle(15, 1f, 9);
            int[] openG = [43, 47, 50, 55, 59, 67];
            for (var start = 0; start < strum.Length; start += Sr / 4)
                for (var k = 0; k < openG.Length; k++)
                {
                    var f0 = 440.0 * Math.Pow(2, (openG[k] - 69) / 12.0);
                    var at = start + k * Sr / 125;                          // 8 ms a string
                    var len = Math.Min((int)(0.26 * Sr), strum.Length - at);
                    for (var i = 0; i < len; i++)
                    {
                        var t = i / (double)Sr;
                        double s = 0;
                        for (var h = 1; h <= 6; h++) s += Math.Sin(2 * Math.PI * f0 * h * t) / h;
                        strum[at + i] += (float)(0.12 * s * (1 - Math.Exp(-t * 400)) * Math.Exp(-t));
                    }
                }
            Push(cqt, strum, ref pos);
            return cqt.NoiseFloorEnergy();
        }

        var legacy = Run(idleFloor: false, out var legacyIdle);
        var shipping = Run(idleFloor: true, out var idle);

        Assert.True(legacy > 5f * legacyIdle, "the strum does not lift the legacy floor; the case is not exercised");
        Assert.True(shipping < 1.5f * idle,
            $"idle floor {shipping:G3} climbed with the strumming (idle level {idle:G3}, legacy {legacy:G3})");
    }

    [Fact]
    public void WhenTheGainRisesWhileIdle_TheFloorFollowsWithinSeconds()
    {
        float Run(bool idleFloor, out float before)
        {
            var cqt = Streaming(idleFloor);
            long pos = 0;
            Push(cqt, Idle(4, 1f, 3), ref pos);
            before = cqt.NoiseFloorEnergy();
            Push(cqt, Idle(4, 4f, 4), ref pos);      // +12 dB, still nothing played
            return cqt.NoiseFloorEnergy();
        }

        var legacy = Run(idleFloor: false, out var legacyBefore);
        var shipping = Run(idleFloor: true, out var before);

        // The 10 s percentile still remembers the quieter half — idle noise then clears the
        // presence gate and phantom-confirms notes (21 % of them in the eval).
        Assert.True(legacy < 2f * legacyBefore, $"legacy floor already followed ({legacyBefore:G3} → {legacy:G3})");
        Assert.True(shipping > 2.5f * before,
            $"idle floor did not follow a 12 dB rise in idle noise ({before:G3} → {shipping:G3})");
    }

    [Fact]
    public void DigitalSilence_TeachesTheFloorNothing()
    {
        float Run(bool idleFloor)
        {
            var cqt = Streaming(idleFloor);
            long pos = 0;
            Push(cqt, new float[3 * Sr], ref pos);   // the device before it streams
            Push(cqt, Idle(3, 1f, 5), ref pos);
            return cqt.NoiseFloorEnergy();
        }

        var noise = Streaming(idleFloor: true);
        long p = 0;
        Push(noise, Idle(3, 1f, 5), ref p);
        var noiseLevel = noise.NoiseFloorEnergy();

        // Half the legacy horizon is exact zeros, so its 5th percentile IS zero and the gate
        // passes anything at all.
        Assert.Equal(0f, Run(idleFloor: false));
        Assert.True(Run(idleFloor: true) > 0.5f * noiseLevel, "exact zeros pulled the idle floor to nothing");
    }

    [Fact]
    public void TheIdleFloor_IsOffUnlessAskedFor_AndOfflineNeverUsesIt()
    {
        Assert.Equal(0, new JuceConstantQ(Sr).IdleSteadyFrames);
        Assert.Throws<ArgumentOutOfRangeException>(() => new JuceConstantQ(Sr, idleSteadyFrames: -1));
        Assert.Throws<ArgumentOutOfRangeException>(() => new JuceConstantQ(Sr, idleSteadyFrames: 10, idleSteadyRatio: 0.5f));

        var audio = ContinuousRiff(4, 6);
        var plain = new JuceConstantQ(Sr, fMinMidi: LiveCqtConfig.MinMidi, binCount: LiveCqtConfig.BinCount,
            maxKernelSamples: LiveCqtConfig.MaxKernelSamples);
        var idle = new JuceConstantQ(Sr, fMinMidi: LiveCqtConfig.MinMidi, binCount: LiveCqtConfig.BinCount,
            maxKernelSamples: LiveCqtConfig.MaxKernelSamples, idleSteadyFrames: LiveCqtConfig.IdleSteadyFrames(Sr));
        plain.AnalyzeWhole(audio);
        idle.AnalyzeWhole(audio);
        Assert.Equal(plain.NoiseFloorEnergy(), idle.NoiseFloorEnergy());
    }
}
