using JuceSharp.AudioIo;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

public sealed class JuceAudioDecoderTests
{
    private static string SamplePath()
    {
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "..", "TestData", "EOF", "Atarayo - Natsu ga Kuru Tabi", "guitar_preview.wav"),
            Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "..", "TestData", "EOF", "Ado - Jukkai (1984)", "guitar_preview.wav"),
        };
        foreach (var c in candidates)
        {
            var full = Path.GetFullPath(c);
            if (File.Exists(full)) return full;
        }
        return string.Empty;
    }

    [Fact]
    public void Probe_returns_sane_metadata()
    {
        var path = SamplePath();
        if (string.IsNullOrEmpty(path)) return;

        var (sr, ch, frames) = JuceAudioDecoder.Probe(path);
        Assert.InRange(sr, 8_000, 192_000);
        Assert.InRange(ch, 1, 8);
        Assert.True(frames > 0);
    }

    [Fact]
    public void Decode_returns_buffer_with_audio_content()
    {
        var path = SamplePath();
        if (string.IsNullOrEmpty(path)) return;

        var buf = JuceAudioDecoder.Decode(path);
        Assert.True(buf.FrameCount > 0);
        Assert.Equal(buf.FrameCount * buf.Channels, buf.Interleaved.Length);

        double sumSq = 0;
        for (var i = 0; i < buf.Interleaved.Length; i++) sumSq += buf.Interleaved[i] * buf.Interleaved[i];
        var rms = Math.Sqrt(sumSq / buf.Interleaved.Length);
        Assert.True(rms > 1e-5, $"Decoded buffer is silent (rms={rms}).");
    }

    [Fact]
    public void DecodeAs_resamples_and_remixes()
    {
        var path = SamplePath();
        if (string.IsNullOrEmpty(path)) return;

        var src = JuceAudioDecoder.Decode(path);
        var dst = JuceAudioDecoder.DecodeAs(path, 22_050, 1);
        Assert.Equal(22_050, dst.SampleRate);
        Assert.Equal(1, dst.Channels);
        var expected = (int)Math.Round(src.FrameCount * (22_050.0 / src.SampleRate));
        Assert.InRange(dst.FrameCount, expected - 2, expected + 2);
    }

    [Fact]
    public void Probe_throws_for_missing_file()
    {
        Assert.Throws<FileNotFoundException>(() => JuceAudioDecoder.Probe(Path.Combine(Path.GetTempPath(), "__no_such_file__.wav")));
    }

    /// <summary>
    /// <c>dca_decode_audio_file</c> hands the caller OWNERSHIP of a malloc'd
    /// interleaved buffer whenever <c>out_samples != NULL</c>, and
    /// <see cref="JuceAudioDecoder.Probe"/> has nowhere to return it — so a
    /// probe that goes through the decoding overload leaks
    /// <c>frames * channels * 4</c> bytes of NATIVE memory on every call and
    /// never shows up in any managed counter. The header's sizing path
    /// (<c>out_samples == NULL</c>) returns before the reader is even created,
    /// so a fixed Probe allocates essentially nothing.
    /// <para>Repeats are sized so a leaking implementation grows private bytes
    /// by ~180 MB while a correct one grows by ~0 — the assertion sits at a
    /// quarter of the leak, i.e. a 4x margin, and is a NATIVE-allocation
    /// assertion, not a timing one.</para>
    /// </summary>
    [NativeAvailableFact]
    public void Probe_does_not_leak_the_decoded_buffer()
    {
        const int repeats     = 48;
        const int sampleRate  = 48_000;
        const int seconds     = 20;
        // mono float32 == 4 bytes/frame; this is what one leaked probe costs.
        const long leakPerProbe = (long)sampleRate * seconds * sizeof(float);
        const long budget       = leakPerProbe * repeats / 4;

        var path = TinyWav.Create(seconds, sampleRate);
        try
        {
            // Prime: the first call JITs the stub and faults in the native
            // reader, so it is not representative of the steady state.
            _ = JuceAudioDecoder.Probe(path);

            var before = PrivateBytes();
            for (var i = 0; i < repeats; i++)
            {
                var (sr, ch, frames) = JuceAudioDecoder.Probe(path);
                Assert.Equal(sampleRate, sr);
                Assert.Equal(1, ch);
                Assert.Equal((long)sampleRate * seconds, frames);
            }
            var grown = PrivateBytes() - before;

            Assert.True(grown < budget,
                $"Probe grew private bytes by {grown / (1024 * 1024)} MB over {repeats} calls " +
                $"(budget {budget / (1024 * 1024)} MB). That is the decoded buffer being leaked — " +
                "Probe must call the sizing-only entry point with out_samples == NULL.");
        }
        finally { TinyWav.TryDelete(path); }

        static long PrivateBytes()
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            using var p = System.Diagnostics.Process.GetCurrentProcess();
            return p.PrivateMemorySize64;
        }
    }
}
