using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using JuceSharp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// DotCharter #411: a preloaded source holds the decoded song as 16-bit PCM
/// (<c>Int16PreloadSource</c>), not float32 -- half the RAM for every song and
/// every stem. Read through the wrapper, the only witness of what the engine
/// holds is its own <c>preloaded into RAM (N MB)</c> open line, so that is what
/// this asserts on: 20 s of 48 kHz stereo is 3.7 MB as int16 and 7.3 MB as
/// float32. The native gate (<c>RtPreloadedSourceHoldsSixteenBitPcm</c>) pins
/// the exact byte count, the bit-exact round trip and the callback.
/// </summary>
[Collection("NativeEngine")]
public sealed class PreloadFootprintTests
{
    private const int Rate = 48_000;
    private const int Seconds = 20;

    private static byte[] StereoWav()
    {
        var frames = Rate * Seconds;
        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);
        w.Write("RIFF"u8); w.Write(36 + frames * 4); w.Write("WAVE"u8);
        w.Write("fmt "u8); w.Write(16); w.Write((short)1); w.Write((short)2); w.Write(Rate); w.Write(Rate * 4);
        w.Write((short)4); w.Write((short)16);
        w.Write("data"u8); w.Write(frames * 4);
        for (var i = 0; i < frames; i++)
        {
            var s = (short)(8000 * Math.Sin(2 * Math.PI * 220 * i / Rate));
            w.Write(s); w.Write(s);
        }
        w.Flush();
        return ms.ToArray();
    }

    [NativeAvailableFact]
    public void AMemoryOpenedSong_IsHeldAsSixteenBitPcm()
    {
        var logger = new CapturingLogger();
        // ON the Null device at 48 kHz, not merely with it registered: the held
        // length follows the DEVICE rate, and a default device at 44.1 kHz (a CI
        // runner's virtual cable) resamples the song down to 3.4 MB.
        using var engine = new JuceAudioEngine(new JuceEngineConfig
        {
            EnableNullDevice = true,
            OutputTypeName = "Null",
            SampleRate = Rate,
        }, logger);
        using var source = engine.OpenMemory(StereoWav(), "wav");

        var line = logger.Records.Select(r => r.Message)
            .LastOrDefault(m => m.Contains("source_open_memory: preloaded into RAM", StringComparison.Ordinal));
        Assert.True(line != null,
            "the engine logged no 'preloaded into RAM' line for a 20 s song -- it is streaming, or the log moved. "
            + string.Join(" | ", logger.Records.Select(r => r.Message)));

        var mb = double.Parse(Regex.Match(line!, @"preloaded into RAM \(([0-9.]+) MB\)").Groups[1].Value,
            CultureInfo.InvariantCulture);
        // Held at the device rate, which is pinned to the file's 48 kHz above.
        var int16Mb = (double)Rate * Seconds * 2 * sizeof(short) / (1024 * 1024);
        Assert.True(Math.Abs(mb - int16Mb) <= 0.1,
            $"a 20 s 48 kHz stereo song is held as {mb} MB; 16-bit PCM is {int16Mb:0.0} MB and float32 "
            + $"{int16Mb * 2:0.0} MB -- the preload is no longer half-size (#411).");
    }
}
