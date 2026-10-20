using JuceSharp.AudioIo;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

public sealed class WavIoTests
{
    [Fact]
    public void Float32_round_trips_through_disk()
    {
        var sr = 48_000;
        var ch = 2;
        var frames = sr / 4;
        var data = new float[frames * ch];
        for (var f = 0; f < frames; f++)
        {
            var s = (float)Math.Sin(2 * Math.PI * 440 * f / sr) * 0.5f;
            data[2 * f] = s;
            data[2 * f + 1] = -s;
        }
        var src = new AudioBuffer(data, sr, ch);

        var tmp = Path.Combine(Path.GetTempPath(), $"jucesharp_wavio_{Guid.NewGuid():N}.wav");
        try
        {
            WavIo.WriteFloat32(tmp, src);
            var rt = WavIo.Read(tmp);

            Assert.Equal(sr, rt.SampleRate);
            Assert.Equal(ch, rt.Channels);
            Assert.Equal(frames, rt.FrameCount);
            for (var i = 0; i < data.Length; i++)
                Assert.Equal(data[i], rt.Interleaved[i], 5);
        }
        finally
        {
            if (File.Exists(tmp)) File.Delete(tmp);
        }
    }
}
