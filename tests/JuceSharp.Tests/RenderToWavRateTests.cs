using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// <c>dca_render_to_wav</c>'s <c>sample_rate</c> argument used to be a LABEL,
/// not a conversion: the requested rate went onto the WAV header while the
/// source samples were copied 1:1, so a 44.1 kHz file "rendered at 48000" was
/// written as a 48 kHz file containing 44.1 kHz audio. It plays 8.8 % fast and
/// 1.5 semitones sharp, and the export returned <c>DCA_OK</c> while the header
/// advertised "format/rate conversion".
/// <para>
/// The detector is DURATION, because that is what a relabel gets wrong and a
/// conversion gets right: the same audio at a higher rate needs proportionally
/// MORE frames. The old code wrote exactly the source's frame count.
/// </para>
/// </summary>
public sealed class RenderToWavRateTests
{
    [NativeAvailableFact]
    public void RenderingAt48k_ConvertsA441kSource_InsteadOfRelabellingIt()
    {
        const double seconds = 2.0;
        var src = TinyWav.Create(seconds, sampleRate: 44100);
        var dst = Path.Combine(Path.GetDirectoryName(src)!, "rendered.wav");
        try
        {
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.RenderToWav(src, dst, 0f, 0f, 48000));

            var (rate, frames) = ReadWavHeader(dst);
            Assert.Equal(48000, rate);

            // 512 frames (10.7 ms) of slack — the streaming converter lands
            // within a handful of frames, and its own block is 4096, so the
            // bound must be well UNDER one block or it would swallow the very
            // failure it exists to catch: a RELABEL of this 2 s source writes
            // 88,200 frames, i.e. 1.8375 s, 162 ms out.
            var renderedSeconds = frames / (double) rate;
            Assert.True(Math.Abs(renderedSeconds - seconds) < 512 / 48000.0,
                $"rendered {frames} frames at {rate} Hz = {renderedSeconds:F4} s from a "
                + $"{seconds:F4} s source — the samples were relabelled, not converted "
                + $"(a relabel writes {(int) (seconds * 44100)} frames)");

            // …and it is not silence: a converter that wrote the right COUNT of
            // zeros would pass the duration check on its own.
            Assert.True(PeakAmplitude(dst) > 0.02,
                "the rendered file is silent — the duration check above proves nothing");
        }
        finally
        {
            TinyWav.TryDelete(src);
        }
    }

    /// <summary>The pass-through case must stay exactly what it was: same rate
    /// in, same rate and frame count out, no converter in the path.</summary>
    [NativeAvailableFact]
    public void RenderingAtTheSourceRate_IsStillAPlainCopy()
    {
        var src = TinyWav.Create(0.5, sampleRate: 48000);
        var dst = Path.Combine(Path.GetDirectoryName(src)!, "rendered.wav");
        try
        {
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.RenderToWav(src, dst, 0f, 0f, 0));
            var (rate, frames) = ReadWavHeader(dst);
            Assert.Equal(48000, rate);
            Assert.Equal(24000, frames);

            // Explicitly naming the source rate is the same path.
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.RenderToWav(src, dst, 0f, 0f, 48000));
            (rate, frames) = ReadWavHeader(dst);
            Assert.Equal(48000, rate);
            Assert.Equal(24000, frames);
        }
        finally
        {
            TinyWav.TryDelete(src);
        }
    }

    private static (int Rate, int Frames) ReadWavHeader(string path)
    {
        using var fs = File.OpenRead(path);
        using var br = new BinaryReader(fs);
        Assert.Equal("RIFF", new string(br.ReadChars(4)));
        br.ReadInt32();
        Assert.Equal("WAVE", new string(br.ReadChars(4)));

        var rate = 0;
        var channels = 0;
        var bits = 0;
        while (fs.Position + 8 <= fs.Length)
        {
            var id = new string(br.ReadChars(4));
            var size = br.ReadInt32();
            var next = fs.Position + size + (size & 1);
            if (id == "fmt ")
            {
                br.ReadInt16();
                channels = br.ReadInt16();
                rate = br.ReadInt32();
                br.ReadInt32();
                br.ReadInt16();
                bits = br.ReadInt16();
            }
            else if (id == "data")
            {
                Assert.True(channels > 0 && bits > 0, "the data chunk preceded fmt");
                return (rate, size / (channels * bits / 8));
            }
            fs.Position = next;
        }
        throw new InvalidDataException($"no data chunk in {path}");
    }

    private static double PeakAmplitude(string path)
    {
        using var fs = File.OpenRead(path);
        using var br = new BinaryReader(fs);
        br.ReadBytes(12);
        while (fs.Position + 8 <= fs.Length)
        {
            var id = new string(br.ReadChars(4));
            var size = br.ReadInt32();
            if (id == "data")
            {
                var peak = 0.0;
                for (var i = 0; i < size / 2; i++)
                    peak = Math.Max(peak, Math.Abs(br.ReadInt16() / (double) short.MaxValue));
                return peak;
            }
            fs.Position += size + (size & 1);
        }
        return 0.0;
    }
}
