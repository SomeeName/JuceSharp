using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
///     Which codebook source / packet format <see cref="JuceWemDecoder" /> uses
///     (<c>DCA_WEM_VARIANT_*</c>, ABI v42). The ORDINALS are the contract.
/// </summary>
public enum JuceWemVariant
{
    /// <summary>Try <see cref="AoTuV" />, <see cref="Default" />, <see cref="Inline" />,
    /// <see cref="DefaultMod" />, <see cref="DefaultStandard" /> in that order and keep
    /// the first that decodes — DotCharter's managed ww2ogg order.</summary>
    Auto = 0,
    /// <summary>The packed aoTuV 6.03 codebooks (every Rocksmith .wem).</summary>
    AoTuV = 1,
    /// <summary>The packed default libvorbis codebooks.</summary>
    Default = 2,
    /// <summary>Codebooks stored (stripped) inline in the setup packet.</summary>
    Inline = 3,
    /// <summary>Default codebooks, ww2ogg's "mod packets" forced on.</summary>
    DefaultMod = 4,
    /// <summary>Default codebooks, "mod packets" forced off.</summary>
    DefaultStandard = 5,
    /// <summary>Old Wwise: the file carries its own Vorbis headers.</summary>
    HeaderTriad = 6,
}

/// <summary>What a <see cref="JuceWemDecoder" /> found in its .wem (<c>dca_wem_info</c>).</summary>
/// <param name="Channels">Channel count (Vorbis order).</param>
/// <param name="SampleRate">Sample rate in Hz.</param>
/// <param name="Frames">Per-channel length the reads deliver in total.</param>
/// <param name="HeaderSampleCount">The vorb chunk's sample count.</param>
/// <param name="DecodedFrames">What the packets decode to before the end-granule clip.</param>
/// <param name="Variant">The variant that decoded it (never <see cref="JuceWemVariant.Auto" />).</param>
/// <param name="ModPackets">Whether ww2ogg's "mod packet" rebuild is in use.</param>
/// <param name="GranulePresent">Whether packet headers carry granules.</param>
/// <param name="PacketCount">Audio packets in the file.</param>
public readonly record struct JuceWemInfo(
    int Channels,
    int SampleRate,
    long Frames,
    long HeaderSampleCount,
    long DecodedFrames,
    JuceWemVariant Variant,
    bool ModPackets,
    bool GranulePresent,
    int PacketCount);

/// <summary>
///     Native Wwise-Vorbis (.wem) decoder (ABI v42, DotCharter #426): the
///     header/codebook rebuild ww2ogg does, in native code, feeding libvorbis
///     directly — no Ogg file and no managed ww2ogg. Marshalling and lifetime
///     only; the decoder is <c>src/JuceSharp.Native/src/wem/</c>.
///     <para>
///         Output matches the managed Ww2Ogg.Core + NVorbis path in length and
///         sample alignment; float samples are NOT clipped (NVorbis clips to
///         [-1, 1]), and <see cref="Read(Span{short})" /> applies DotCharter's
///         16-bit WAV formula, which clamps.
///     </para>
///     <para>
///         One caller at a time. The native side decodes a front-to-back read on
///         up to four worker threads, bit-identical to a single-threaded decode
///         (DotCharter #434). A .wem as an ENGINE SOURCE needs none of this:
///         <see cref="JuceAudioEngine" />'s file/memory sources open <c>.wem</c>
///         directly since v42.
///     </para>
/// </summary>
public sealed class JuceWemDecoder : IDisposable
{
    // Null ONLY when the constructor threw before it was assigned; the finalizer
    // runs on that instance too (FinalizerSafetyTests).
    private readonly NativeHandleOwner? _owner;

    /// <summary>What the decoder found; fixed for the decoder's lifetime.</summary>
    public JuceWemInfo Info { get; }

    /// <summary>True after <see cref="Dispose" />.</summary>
    public bool IsDisposed => !(_owner?.IsAlive ?? false);

    private JuceWemDecoder(nint handle)
    {
        try
        {
            _owner = new NativeHandleOwner(handle);
            JuceAudioEngine.Check(NativeMethods.WemGetInfo(handle, out var i), "wem_get_info");
            Info = new JuceWemInfo(i.Channels, i.SampleRate, i.Frames, i.HeaderSampleCount, i.DecodedFrames,
                (JuceWemVariant)i.Variant, i.ModPackets != 0, i.GranulePresent != 0, i.PacketCount);
        }
        catch
        {
            if (_owner?.TakeForDispose(nameof(JuceWemDecoder)) is { } h && h != nint.Zero)
                NativeMethods.WemDestroy(h);
            GC.SuppressFinalize(this);
            throw;
        }
    }

    /// <summary>Opens a .wem file (read whole into native memory).</summary>
    /// <exception cref="JuceAudioException">Not a decodable .wem (status <c>Decode</c>).</exception>
    public static JuceWemDecoder OpenFile(string path, JuceWemVariant variant = JuceWemVariant.Auto)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        EnsureAbi();
        JuceAudioEngine.Check(NativeMethods.WemOpenFile(path, (int)variant, out var handle), "wem_open_file");
        return new JuceWemDecoder(handle);
    }

    /// <summary>Opens a .wem held in memory. The bytes are COPIED natively.</summary>
    /// <exception cref="JuceAudioException">Not a decodable .wem (status <c>Decode</c>).</exception>
    public static unsafe JuceWemDecoder OpenMemory(ReadOnlySpan<byte> data, JuceWemVariant variant = JuceWemVariant.Auto)
    {
        if (data.IsEmpty) throw new ArgumentException("A .wem cannot be empty.", nameof(data));
        EnsureAbi();
        nint handle;
        fixed (byte* p = data)
            JuceAudioEngine.Check(NativeMethods.WemOpenMemory(p, (nuint)data.Length, (int)variant, out handle), "wem_open_memory");
        return new JuceWemDecoder(handle);
    }

    /// <summary>
    ///     Decodes a whole .wem file to interleaved float (unclipped). Convenience
    ///     over <see cref="OpenFile" /> + <see cref="Read(Span{float})" />.
    /// </summary>
    public static float[] DecodeFile(string path, out JuceWemInfo info, JuceWemVariant variant = JuceWemVariant.Auto)
    {
        using var decoder = OpenFile(path, variant);
        info = decoder.Info;
        var samples = new float[checked(info.Frames * info.Channels)];
        var span = samples.AsSpan();
        var at = 0;
        while (at < span.Length)
        {
            var frames = decoder.Read(span[at..]);
            if (frames == 0) break;
            at += frames * info.Channels;
        }
        return at == samples.Length ? samples : samples[..at];
    }

    /// <summary>Decodes the next frames into <paramref name="interleaved" /> (whole
    /// frames only). Returns the frames written; 0 at the end.</summary>
    public unsafe int Read(Span<float> interleaved)
    {
        var capacity = interleaved.Length / Math.Max(1, Info.Channels);
        if (capacity == 0) return 0;
        var h = Acquire();
        try
        {
            int got;
            fixed (float* p = interleaved)
                JuceAudioEngine.Check(NativeMethods.WemReadF32(h, p, capacity, out got), "wem_read_f32");
            return got;
        }
        finally { _owner!.Release(); }
    }

    /// <summary>Decodes the next frames as 16-bit PCM with DotCharter's WAV formula
    /// (clamp, -1 → -32768, else round-half-even of <c>sample * 32767</c>).</summary>
    public unsafe int Read(Span<short> interleaved)
    {
        var capacity = interleaved.Length / Math.Max(1, Info.Channels);
        if (capacity == 0) return 0;
        var h = Acquire();
        try
        {
            int got;
            fixed (short* p = interleaved)
                JuceAudioEngine.Check(NativeMethods.WemReadS16(h, p, capacity, out got), "wem_read_s16");
            return got;
        }
        finally { _owner!.Release(); }
    }

    /// <summary>Positions the next read at <paramref name="frame" /> (clamped to
    /// [0, <see cref="JuceWemInfo.Frames" />]); bit-exact with a continuous decode.</summary>
    public void Seek(long frame)
    {
        var h = Acquire();
        try { JuceAudioEngine.Check(NativeMethods.WemSeek(h, frame), "wem_seek"); }
        finally { _owner!.Release(); }
    }

    private nint Acquire()
        => (_owner ?? throw new ObjectDisposedException(nameof(JuceWemDecoder))).Acquire(nameof(JuceWemDecoder));

    private static int _abiChecked;

    internal static void EnsureAbi()
    {
        if (Volatile.Read(ref _abiChecked) != 0) return;
        var actual = NativeMethods.AbiVersion();
        if (actual != NativeMethods.ExpectedAbiVersion)
            throw new JuceAbiMismatchException(NativeMethods.ExpectedAbiVersion, actual);
        Volatile.Write(ref _abiChecked, 1);
    }

    /// <summary>Frees the native decoder (waits for an in-flight read).</summary>
    public void Dispose()
    {
        Destroy(NativeHandleOwner.DefaultDrainTimeoutMs);
        GC.SuppressFinalize(this);
    }

    /// <summary>Frees a decoder nobody disposed. It may not wait (deadline 0): a
    /// read still in flight means the handle is leaked rather than freed under it.</summary>
    ~JuceWemDecoder() => Destroy(0);

    private void Destroy(int drainMs)
    {
        if (_owner is null) return;
        var h = _owner.TakeForDispose(nameof(JuceWemDecoder), drainMs, out _);
        if (h != nint.Zero) NativeMethods.WemDestroy(h);
    }
}
