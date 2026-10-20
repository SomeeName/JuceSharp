using JuceSharp.AudioIo;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

/// <summary>
///     Round-trip and failure-mode pins for <see cref="WavWriter" />.
///     <para>
///         Everything here goes back through <see cref="WavIo.Read(string)" />, because
///         a writer that only its own reader understands is not a WAV writer. The two
///         had been deliberately asymmetric — the package could READ 8/16/24/32-bit PCM
///         and 32/64-bit float and could WRITE only float32 — and this closes the
///         16- and 24-bit halves of that gap.
///     </para>
/// </summary>
[Collection("AllocationProbes")]
public sealed class WavWriterTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("juce-wavwriter-").FullName;

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string Path(string name) => System.IO.Path.Combine(_dir, name);

    private static float[] Ramp(int frames, int channels)
    {
        var buf = new float[frames * channels];
        for (var f = 0; f < frames; f++)
        for (var c = 0; c < channels; c++)
            // Deterministic, spans the full range, hits exact ±1.0 at the ends.
            buf[f * channels + c] = MathF.Sin(2f * MathF.PI * (f * channels + c) / 97f);
        return buf;
    }

    /// <summary>Tolerance of one quantisation step for the format under test.</summary>
    private static float Tolerance(WavSampleFormat format) => format switch
    {
        WavSampleFormat.Float32 => 0f,
        // Half an LSB: the writer scales by 2^(bits-1), which is the exact
        // inverse of the reader's divisor, so rounding is the ONLY error.
        WavSampleFormat.Pcm16   => 1f / 65000f,
        _                       => 1f / 16_000_000f,
    };

    [Theory]
    [InlineData(WavSampleFormat.Float32, 1, 1000)]
    [InlineData(WavSampleFormat.Float32, 2, 999)]
    [InlineData(WavSampleFormat.Pcm16,   1, 1000)]
    [InlineData(WavSampleFormat.Pcm16,   2, 999)]
    [InlineData(WavSampleFormat.Pcm24,   1, 999)]     // odd frame count -> odd data size -> RIFF pad byte
    [InlineData(WavSampleFormat.Pcm24,   2, 777)]
    public void RoundTrip_MatchesWithinOneQuantisationStep(WavSampleFormat format, int channels, int frames)
    {
        var path = Path($"rt-{format}-{channels}-{frames}.wav");
        var data = Ramp(frames, channels);

        using (var w = new WavWriter(path, 44100, channels, format))
        {
            // Several ragged writes: the block boundary must not be a seam.
            var offset = 0;
            var chunk = channels * 37;
            while (offset < data.Length)
            {
                var take = Math.Min(chunk, data.Length - offset);
                w.Write(data.AsSpan(offset, take));
                offset += take;
            }
            Assert.Equal(frames, w.FrameCount);
        }

        var read = WavIo.Read(path);
        Assert.Equal(44100, read.SampleRate);
        Assert.Equal(channels, read.Channels);
        Assert.Equal(frames, read.FrameCount);

        var tol = Tolerance(format);
        for (var i = 0; i < data.Length; i++)
            Assert.True(MathF.Abs(data[i] - read.Interleaved[i]) <= tol,
                $"sample {i}: wrote {data[i]}, read {read.Interleaved[i]} (tol {tol})");
    }

    /// <summary>Non-vacuity for the tolerances above: 16-bit really is coarser than
    /// 24-bit, which is really coarser than float. If the writer silently wrote
    /// float32 for every format the round-trip test would pass and this would not.</summary>
    [Fact]
    public void EachFormat_HasTheQuantisationItClaims()
    {
        var data = Ramp(512, 1);
        var errors = new Dictionary<WavSampleFormat, float>();
        foreach (var format in new[] { WavSampleFormat.Float32, WavSampleFormat.Pcm16, WavSampleFormat.Pcm24 })
        {
            var path = Path($"q-{format}.wav");
            using (var w = new WavWriter(path, 48000, 1, format)) w.Write(data);
            var read = WavIo.Read(path);
            var max = 0f;
            for (var i = 0; i < data.Length; i++) max = MathF.Max(max, MathF.Abs(data[i] - read.Interleaved[i]));
            errors[format] = max;
            Assert.True(new FileInfo(path).Length > 0);
        }

        Assert.Equal(0f, errors[WavSampleFormat.Float32]);
        Assert.InRange(errors[WavSampleFormat.Pcm24], 1e-9f, 1f / 16_000_000f);
        Assert.InRange(errors[WavSampleFormat.Pcm16], 1e-6f, 1f / 65000f);
        Assert.True(errors[WavSampleFormat.Pcm16] > errors[WavSampleFormat.Pcm24] * 100f);
    }

    [Fact]
    public void PlanarWrite_MatchesTheInterleavedOne()
    {
        const int frames = 500, channels = 2;
        var interleaved = Ramp(frames, channels);
        var planes = new float[channels][];
        for (var c = 0; c < channels; c++)
        {
            planes[c] = new float[frames];
            for (var f = 0; f < frames; f++) planes[c][f] = interleaved[f * channels + c];
        }

        var a = Path("planar.wav");
        using (var w = new WavWriter(a, 44100, channels)) w.WritePlanar(planes);
        var b = Path("interleaved.wav");
        using (var w = new WavWriter(b, 44100, channels)) w.Write(interleaved);

        Assert.Equal(File.ReadAllBytes(b), File.ReadAllBytes(a));
    }

    /// <summary>Out-of-range and non-finite input must clip and mute, not wrap. An
    /// unguarded <c>(int)</c> of a NaN is 0 on x64 and INT_MIN elsewhere, and an
    /// unguarded clamp lets a NaN through because every comparison against it is
    /// false.</summary>
    [Fact]
    public void OutOfRangeAndNonFiniteSamples_ClipAndMute()
    {
        var path = Path("clip.wav");
        float[] data = [2f, -2f, float.NaN, float.PositiveInfinity, float.NegativeInfinity, 1f, -1f, 0f];
        using (var w = new WavWriter(path, 44100, 1, WavSampleFormat.Pcm16)) w.Write(data);

        var read = WavIo.Read(path).Interleaved;
        Assert.Equal(1f, read[0], 4);
        Assert.Equal(-1f, read[1], 4);
        Assert.Equal(0f, read[2]);
        Assert.Equal(0f, read[3]);
        Assert.Equal(0f, read[4]);
        Assert.Equal(1f, read[5], 4);
        Assert.Equal(-1f, read[6], 4);
        Assert.Equal(0f, read[7]);
    }

    [Fact]
    public void DisposeWithoutWrite_ProducesAValidEmptyFile()
    {
        var path = Path("empty.wav");
        using (var w = new WavWriter(path, 48000, 2)) { }

        Assert.False(WavWriter.IsUnfinished(path));
        var read = WavIo.Read(path);
        Assert.Equal(48000, read.SampleRate);
        Assert.Equal(2, read.Channels);
        Assert.Equal(0, read.FrameCount);
        Assert.Empty(read.Interleaved);
    }

    /// <summary>
    /// The crash case: the writer never reaches <see cref="WavWriter.Dispose" />, so
    /// no size field was ever patched.
    /// <para>The provisional sizes are the RIFF unknown-length placeholder
    /// (<c>0xFFFFFFFF</c>) and NOT zero, so such a file is detectable AND still
    /// decodes in full. Writing zeros would have produced a valid-looking WAV
    /// declaring no audio — silent data loss with nothing to notice.</para>
    /// </summary>
    [Fact]
    public void UnfinishedFile_IsDetectableAndStillReadsBackEverySample()
    {
        var path = Path("crashed.wav");
        var data = Ramp(300, 1);
        var stream = File.Create(path);
        var w = new WavWriter(stream, 44100, 1, WavSampleFormat.Pcm16, leaveOpen: true);
        w.Write(data);
        stream.Flush();
        stream.Dispose();                      // process died here: no Dispose on the writer

        Assert.True(WavWriter.IsUnfinished(path), "an unpatched header did not report as unfinished");

        var read = WavIo.Read(path);
        Assert.Equal(300, read.FrameCount);
        for (var i = 0; i < data.Length; i++)
            Assert.True(MathF.Abs(data[i] - read.Interleaved[i]) <= 1f / 65000f);

        // Non-vacuity: a properly closed file reports finished.
        var good = Path("finished.wav");
        using (var ok = new WavWriter(good, 44100, 1, WavSampleFormat.Pcm16)) ok.Write(data);
        Assert.False(WavWriter.IsUnfinished(good));
    }

    [Fact]
    public void Write_DoesNotAllocateAfterConstruction()
    {
        var path = Path("alloc.wav");
        var block = Ramp(1024, 2);
        using var w = new WavWriter(path, 44100, 2, WavSampleFormat.Pcm24);
        w.Write(block);    // warm up

        var delta = AllocationProbe.Measure(() => { for (var i = 0; i < 50; i++) w.Write(block); });

        Assert.Equal(0, delta);
        // AllocationProbe may run the region twice; every run writes 50 more blocks.
        Assert.True(w.FrameCount >= 1024 * 51 && w.FrameCount % 1024 == 0, $"unexpected frame count {w.FrameCount}");
    }

    [Fact]
    public void RaggedInterleavedLength_IsRejected()
    {
        var path = Path("ragged.wav");
        using var w = new WavWriter(path, 44100, 2);
        Assert.Throws<ArgumentException>(() => w.Write(new float[3]));
    }

    [Fact]
    public void NonSeekableStream_IsRejectedUpFront()
    {
        using var ns = new NonSeekableStream();
        var ex = Assert.Throws<ArgumentException>(() => new WavWriter(ns, 44100, 1));
        Assert.Contains("seekable", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    private sealed class NonSeekableStream : Stream
    {
        public override bool CanRead => false;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => 0;
        public override long Position { get => 0; set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) { }
    }
}
