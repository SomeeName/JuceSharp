using System.Text;

namespace JuceSharp.Tests;

/// <summary>Generates a tiny self-contained PCM 16-bit WAV file in a
/// temp directory for source-loading tests.</summary>
internal static class TinyWav
{
    public static string Create(double seconds = 0.25, int sampleRate = 48000)
    {
        var dir = Path.Combine(Path.GetTempPath(), "JuceSharpTests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "tiny.wav");

        var frames = (int)Math.Max(1, seconds * sampleRate);
        const int channels = 1;
        const int bitsPerSample = 16;
        var byteRate = sampleRate * channels * bitsPerSample / 8;
        var dataSize = frames * channels * 2;

        using var fs = File.Create(path);
        using var bw = new BinaryWriter(fs);
        bw.Write(Encoding.ASCII.GetBytes("RIFF"));
        bw.Write(36 + dataSize);
        bw.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
        bw.Write(16);                            // PCM fmt chunk size
        bw.Write((short)1);                      // PCM format
        bw.Write((short)channels);
        bw.Write(sampleRate);
        bw.Write(byteRate);
        bw.Write((short)(channels * 2));         // block align
        bw.Write((short)bitsPerSample);
        bw.Write(Encoding.ASCII.GetBytes("data"));
        bw.Write(dataSize);
        // 440 Hz sine, low amplitude to avoid surprising anyone listening
        for (var i = 0; i < frames; i++)
        {
            var s = Math.Sin(2.0 * Math.PI * 440.0 * i / sampleRate) * 0.1;
            bw.Write((short)(s * short.MaxValue));
        }
        return path;
    }

    public static void TryDelete(string path)
    {
        try
        {
            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir)) Directory.Delete(dir, true);
        }
        catch { /* best-effort */ }
    }
}
