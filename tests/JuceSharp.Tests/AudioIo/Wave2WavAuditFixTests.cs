using System.Buffers.Binary;
using System.Text;
using JuceSharp.AudioIo;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

/// <summary>
///     Wave-2 audit, the <c>JuceSharp.AudioIo</c> half: findings #20 and #31, the
///     <see cref="WavWriter" /> cleanups filed beside them (the lazily allocated
///     interleave scratch, the convenience overload's dereference order, the
///     unvalidated <c>ushort</c> casts, and <see cref="WavWriter.IsUnfinished(Stream, long)" />
///     reading at absolute zero), and the <see cref="WavWriter.DataBytes" /> gap.
/// </summary>
[Collection("AllocationProbes")]
public sealed class Wave2WavAuditFixTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("juce-wave2-wav-").FullName;

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string Path(string name) => System.IO.Path.Combine(_dir, name);

    // ---------------------------------------------------------------- #20

    /// <summary>
    /// #20. <c>WavWriter(string, …)</c> delegated through <c>File.Create(path)</c>, so
    /// every argument the target constructor rejects — an out-of-range
    /// <c>format</c>, a zero <c>sampleRate</c>, a zero <c>channels</c> — threw with the
    /// file already open. The handle then lived until finalization and a locked,
    /// zero-byte file sat on disk meanwhile.
    /// <para>The assertion is deliberately about the FILE SYSTEM and not about the
    /// exception: the exception was always correct, and the leak is what was not.</para>
    /// </summary>
    [Theory]
    [InlineData(0, 2, WavSampleFormat.Float32)]
    [InlineData(-1, 2, WavSampleFormat.Float32)]
    [InlineData(44100, 0, WavSampleFormat.Float32)]
    [InlineData(44100, -3, WavSampleFormat.Float32)]
    [InlineData(44100, 2, (WavSampleFormat)99)]
    public void FileConstructor_RejectsBadScalars_WithoutCreatingTheFile(
        int sampleRate, int channels, WavSampleFormat format)
    {
        var path = Path($"never-{sampleRate}-{channels}-{(int)format}.wav");

        Assert.Throws<ArgumentOutOfRangeException>(() => new WavWriter(path, sampleRate, channels, format));
        Assert.False(File.Exists(path),
            "the file was created before the arguments were validated — that is the leak");
    }

    /// <summary>Non-vacuity: the same call with valid arguments DOES create the file,
    /// and the file is immediately re-openable, i.e. nothing is left holding it.</summary>
    [Fact]
    public void FileConstructor_WithValidArguments_CreatesAndReleasesTheFile()
    {
        var path = Path("created.wav");
        using (var w = new WavWriter(path, 44100, 2))
            w.Write(new float[] { 0.1f, -0.1f, 0.25f, -0.25f });

        Assert.True(File.Exists(path));
        var read = WavIo.Read(path);      // would throw IOException if the handle leaked
        Assert.Equal(2, read.Channels);
        Assert.Equal(2, read.FrameCount);
    }

    /// <summary>The stream constructor owns the stream when <c>leaveOpen</c> is false,
    /// so a rejected argument must not leave it open either — that is what makes the
    /// delegating file constructor safe for the cases validation cannot pre-empt.</summary>
    [Fact]
    public void StreamConstructor_DisposesAStreamItOwns_WhenConstructionFails()
    {
        var owned = new MemoryStream();
        Assert.Throws<ArgumentOutOfRangeException>(() => new WavWriter(owned, 0, 2, leaveOpen: false));
        Assert.False(owned.CanWrite, "a stream the constructor took ownership of was left open");

        // …and leaves a stream it does NOT own alone.
        var borrowed = new MemoryStream();
        Assert.Throws<ArgumentOutOfRangeException>(() => new WavWriter(borrowed, 0, 2, leaveOpen: true));
        Assert.True(borrowed.CanWrite, "a borrowed stream was disposed");
    }

    // ------------------------------------------------- ushort truncation

    /// <summary>
    /// <c>channels</c> and <c>nBlockAlign</c> are written as 16-bit `fmt ` fields with
    /// a bare cast. 65,536 channels truncated to 0 and 30,000 24-bit channels
    /// overflowed <c>blockAlign</c> alone — both produced a header that silently
    /// declared a different layout from the one every <c>Write</c> validates against,
    /// with no error at the point of the mistake.
    /// </summary>
    [Theory]
    [InlineData(65536, WavSampleFormat.Float32)]   // channels itself does not fit
    [InlineData(70000, WavSampleFormat.Pcm16)]
    [InlineData(16384, WavSampleFormat.Float32)]   // blockAlign = 65536, one past the field
    [InlineData(30000, WavSampleFormat.Pcm24)]     // blockAlign = 90000
    public void Constructor_RejectsAChannelCountThatDoesNotFitTheHeader(int channels, WavSampleFormat format)
    {
        using var ms = new MemoryStream();
        Assert.Throws<ArgumentOutOfRangeException>(
            () => new WavWriter(ms, 44100, channels, format, leaveOpen: true));
        Assert.Equal(0L, ms.Length);   // nothing was written before the rejection
    }

    /// <summary>Non-vacuity: the largest layout that DOES fit still constructs, so the
    /// check is a bound and not a ban.</summary>
    [Fact]
    public void Constructor_AcceptsTheLargestLayoutThatFitsTheHeader()
    {
        using var ms = new MemoryStream();
        using var w = new WavWriter(ms, 44100, 16383, WavSampleFormat.Float32, leaveOpen: true);
        Assert.Equal(16383, w.Channels);   // blockAlign = 65532
    }

    // ------------------------------------------------------ WritePlanar

    /// <summary>
    /// The type doc promises "allocation-free per write after construction", and
    /// <c>WritePlanar</c> allocated its interleave scratch on FIRST USE — so the very
    /// first planar write broke the promise, for a caller with no way to see it coming.
    /// The scratch is now built in the constructor.
    ///
    /// <para>
    /// <b>The instrument needs as much care as the subject.</b>
    /// <c>GC.GetAllocatedBytesForCurrentThread()</c> is
    /// <c>alloc_bytes + alloc_bytes_uoh - (alloc_limit - alloc_ptr)</c>, and a GC
    /// RETIRES this thread's allocation context — the unconsumed remainder of the
    /// current quantum stops being subtracted and is charged to the thread for
    /// good. Two reads straddling a collection therefore differ by up to ~8 KB
    /// with nothing allocated between them, and this assertion has zero
    /// tolerance. Three things keep it honest and none of them is a wall clock:
    /// the class is in <c>[Collection("AllocationProbes")]</c> so no other test
    /// is making garbage alongside it; the window no longer contains the
    /// first-ever EXECUTION of <c>WritePlanar</c>-&gt;<c>Write</c>-&gt;encode,
    /// whose tier-0 JIT and cctors widened it by orders of magnitude; and a
    /// non-zero reading is taken once more before it is believed.
    /// </para>
    /// <para>
    /// The warm-up runs on a SEPARATE, throwaway writer, so every measured call
    /// is still the FIRST call on a FRESH instance — which is what the
    /// regression was. A returning <c>??=</c> allocates a 8,192-float scratch
    /// (32 KB) on each of those fresh writers, far outside anything the GC
    /// context can account for, so neither mitigation can hide the bug.
    /// </para>
    /// </summary>
    [Fact]
    public void WritePlanar_DoesNotAllocate_NotEvenOnTheFirstCall()
    {
        var planes = new[] { new float[4096], new float[4096] };
        for (var i = 0; i < 4096; i++)
        {
            planes[0][i] = MathF.Sin(i / 50f);
            planes[1][i] = MathF.Cos(i / 50f);
        }

        _ = MeasureFirstPlanarWrite(planes);        // JIT + cctors, outside the window
        var delta = MeasureFirstPlanarWrite(planes);
        if (delta != 0)
        {
            // A collection landed in the window. Settle and read once more; a real
            // per-call allocation is deterministic and survives the retry.
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            delta = MeasureFirstPlanarWrite(planes);
        }

        Assert.Equal(0, delta);
    }

    /// <summary>Bytes this thread allocated across a fresh writer's first two planar
    /// writes. Asserts the writer actually wrote, so a zero delta can never come
    /// from a call that did nothing.</summary>
    private static long MeasureFirstPlanarWrite(float[][] planes)
    {
        // Pre-sized: a MemoryStream that GROWS mid-test allocates, and this case is
        // about the writer's own allocations, not the sink's.
        using var ms = new MemoryStream(1 << 20);
        using var w = new WavWriter(ms, 44100, 2, WavSampleFormat.Pcm16, leaveOpen: true);

        var before = GC.GetAllocatedBytesForCurrentThread();
        w.WritePlanar(planes, 0, 4096);     // the FIRST call is the one that used to allocate
        w.WritePlanar(planes, 0, 4096);
        var delta = GC.GetAllocatedBytesForCurrentThread() - before;

        Assert.Equal(8192L * 2 * 2, w.DataBytes);
        return delta;
    }

    /// <summary>The convenience overload dereferenced <c>planes[0].Length</c> before
    /// anything checked that <c>planes[0]</c> was non-null, so a null PLANE (not a null
    /// list) surfaced as a bare <c>NullReferenceException</c> instead of the
    /// <c>ArgumentException</c> the three-argument form raises for the same input.</summary>
    [Fact]
    public void WritePlanar_ConvenienceOverload_ReportsANullPlaneAsAnArgumentError()
    {
        using var ms = new MemoryStream();
        using var w = new WavWriter(ms, 44100, 2, leaveOpen: true);

        var planes = new float[][] { null!, new float[8] };
        var ex = Assert.Throws<ArgumentException>(() => w.WritePlanar(planes));
        Assert.Equal("planes", ex.ParamName);

        // The three-argument form has always reported it; the two forms now agree.
        Assert.Throws<ArgumentException>(() => w.WritePlanar(planes, 0, 8));
        Assert.Throws<ArgumentNullException>(() => w.WritePlanar(null!));
    }

    // -------------------------------------------------------- DataBytes

    /// <summary><see cref="WavWriter.DataBytes" /> had no test at all. It is the payload
    /// length excluding the header and the RIFF pad byte, which makes it the number the
    /// 4 GB guard is written against — and it must agree with the `data` chunk size the
    /// reader sees.</summary>
    [Theory]
    [InlineData(WavSampleFormat.Float32, 4)]
    [InlineData(WavSampleFormat.Pcm16, 2)]
    [InlineData(WavSampleFormat.Pcm24, 3)]
    public void DataBytes_CountsPayloadOnly_AndAgreesWithFrameCount(WavSampleFormat format, int bytesPerSample)
    {
        var path = Path($"databytes-{format}.wav");
        const int frames = 333;
        const int channels = 2;

        using (var w = new WavWriter(path, 44100, channels, format))
        {
            Assert.Equal(0L, w.DataBytes);
            Assert.Equal(0L, w.FrameCount);

            w.Write(new float[frames * channels]);
            Assert.Equal((long)frames * channels * bytesPerSample, w.DataBytes);
            Assert.Equal((long)frames, w.FrameCount);

            w.Write(new float[channels]);
            Assert.Equal((long)(frames + 1) * channels * bytesPerSample, w.DataBytes);
            Assert.Equal((long)(frames + 1), w.FrameCount);
        }

        // The pad byte (24-bit, odd payload) is NOT counted — the reader agrees.
        var read = WavIo.Read(path);
        Assert.Equal(frames + 1, read.FrameCount);
    }

    // ------------------------------------------------------ IsUnfinished

    /// <summary>
    /// <c>IsUnfinished(Stream)</c> always sought to ABSOLUTE zero, so for a writer
    /// constructed over a stream at a non-zero position — which
    /// <c>WavWriter(Stream, …)</c> explicitly supports — it reported on somebody else's
    /// bytes. Usually a confident <c>false</c>: "this was closed properly", about a file
    /// that was not. The overload now takes the offset.
    /// </summary>
    [Fact]
    public void IsUnfinished_ReadsTheHeaderAtTheGivenOffset_NotAlwaysAtZero()
    {
        using var ms = new MemoryStream();
        ms.Write(new byte[] { 1, 2, 3, 4, 5, 6, 7 });   // a prefix the writer is told to sit after
        var headerOffset = ms.Position;

        var writer = new WavWriter(ms, 44100, 1, WavSampleFormat.Float32, leaveOpen: true);
        writer.Write(new float[] { 0.5f, -0.5f });

        // Unfinished, and only the offset-aware read can see it.
        Assert.True(WavWriter.IsUnfinished(ms, headerOffset));
        Assert.False(WavWriter.IsUnfinished(ms), "offset 0 is the prefix, not the RIFF header");

        // The probe restores the position, so the writer can carry on.
        var resumeAt = ms.Position;
        WavWriter.IsUnfinished(ms, headerOffset);
        Assert.Equal(resumeAt, ms.Position);
        writer.Write(new float[] { 0.25f });

        writer.Dispose();
        Assert.False(WavWriter.IsUnfinished(ms, headerOffset));
        Assert.Throws<ArgumentOutOfRangeException>(() => WavWriter.IsUnfinished(ms, -1));
    }

    // ---------------------------------------------------------------- #31

    /// <summary>
    /// #31, first half. <c>ReadSamples</c> ran BEFORE the <c>channels == 0</c> check, so
    /// a hostile or corrupt `fmt ` chunk declaring zero channels made the reader
    /// allocate and decode up to <c>Array.MaxLength</c> floats — ~2 GB — before failing.
    /// The validation is now in the `fmt ` branch, which is also why the message is
    /// specific: the old one said "WAV missing 'fmt ' chunk", which was not even true.
    /// </summary>
    [Theory]
    [InlineData(0, 44100)]
    [InlineData(2, 0)]
    public void Read_RejectsADegenerateFmtChunk_BeforeDecodingAnySamples(ushort channels, uint sampleRate)
    {
        var wav = BuildWav(channels, sampleRate, dataBytes: 4096);
        var ex = Assert.Throws<InvalidDataException>(() => WavIo.Read(new MemoryStream(wav)));

        // The `fmt `-branch message, not the post-decode one it used to reach.
        Assert.Contains("'fmt '", ex.Message);
        Assert.DoesNotContain("missing", ex.Message);
    }

    /// <summary>Non-vacuity: the same builder with sane values reads fine, so the
    /// rejection is about the degenerate header and not about the fixture.</summary>
    [Fact]
    public void Read_AcceptsTheSameFixtureWithSaneValues()
    {
        var buffer = WavIo.Read(new MemoryStream(BuildWav(2, 44100, dataBytes: 32)));
        Assert.Equal(2, buffer.Channels);
        Assert.Equal(44100, buffer.SampleRate);
        Assert.Equal(8, buffer.Interleaved.Length);
    }

    /// <summary>
    /// #31, second half. <c>sampleCount = byteCount / bytesPerSample</c> was not rounded
    /// down to a whole number of FRAMES, so a truncated file yielded an
    /// <see cref="AudioBuffer" /> whose length is not a multiple of
    /// <see cref="AudioBuffer.Channels" /> — and every consumer's
    /// <c>Length / Channels</c> frame math is then off by one interleaved group. A
    /// silent channel rotation rather than an error.
    /// </summary>
    [Theory]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(6)]
    public void Read_TruncatedFile_StillYieldsAWholeNumberOfFrames(int channels)
    {
        var frames = 5;
        var full = new float[frames * channels];
        for (var i = 0; i < full.Length; i++) full[i] = i / 100f;

        using var ms = new MemoryStream();
        WavIo.WriteFloat32(ms, new AudioBuffer(full, 44100, channels));
        var bytes = ms.ToArray();

        // Chop one SAMPLE (4 bytes) off the end: the data chunk now holds a partial
        // final frame, which is exactly what a half-written capture looks like.
        var truncated = bytes[..^4];
        var read = WavIo.Read(new MemoryStream(truncated));

        Assert.Equal(channels, read.Channels);
        Assert.Equal(0, read.Interleaved.Length % channels);
        Assert.Equal(frames - 1, read.FrameCount);
        Assert.Equal((frames - 1) * channels, read.Interleaved.Length);

        // The frames that survived are unchanged, so the truncation drops only the
        // partial group.
        for (var i = 0; i < read.Interleaved.Length; i++) Assert.Equal(full[i], read.Interleaved[i]);
    }

    /// <summary>A mono file has no frame grouping to lose: truncation keeps every whole
    /// sample. Non-vacuity for the rounding above — it is not simply dropping a sample
    /// from everything.</summary>
    [Fact]
    public void Read_TruncatedMonoFile_KeepsEverySample()
    {
        var full = new float[5];
        for (var i = 0; i < full.Length; i++) full[i] = i / 100f;

        using var ms = new MemoryStream();
        WavIo.WriteFloat32(ms, new AudioBuffer(full, 44100, 1));
        var truncated = ms.ToArray()[..^4];

        var read = WavIo.Read(new MemoryStream(truncated));
        Assert.Equal(1, read.Channels);
        Assert.Equal(4, read.Interleaved.Length);
    }

    /// <summary>A minimal float32 RIFF/WAVE with a caller-chosen `fmt ` chunk.</summary>
    private static byte[] BuildWav(ushort channels, uint sampleRate, int dataBytes)
    {
        using var ms = new MemoryStream();
        using (var bw = new BinaryWriter(ms, Encoding.ASCII, leaveOpen: true))
        {
            bw.Write("RIFF".ToCharArray());
            bw.Write((uint)(4 + 8 + 16 + 8 + dataBytes));
            bw.Write("WAVE".ToCharArray());
            bw.Write("fmt ".ToCharArray());
            bw.Write(16);
            bw.Write((ushort)3);                                   // WAVE_FORMAT_IEEE_FLOAT
            bw.Write(channels);
            bw.Write(sampleRate);
            bw.Write((uint)(sampleRate * Math.Max(1, (int)channels) * 4));
            bw.Write((ushort)(Math.Max(1, (int)channels) * 4));
            bw.Write((ushort)32);
            bw.Write("data".ToCharArray());
            bw.Write((uint)dataBytes);
            bw.Write(new byte[dataBytes]);
        }
        var wav = ms.ToArray();
        // Sanity on the fixture itself: the RIFF size field is consistent.
        Assert.Equal((uint)(wav.Length - 8), BinaryPrimitives.ReadUInt32LittleEndian(wav.AsSpan(4, 4)));
        return wav;
    }
}
