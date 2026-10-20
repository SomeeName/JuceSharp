using System.Text;
using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
///     A live hardware MIDI OUTPUT device (ABI v34). Created by
///     <see cref="JuceAudioEngine.OpenMidiOutput" />; dispose to close it.
///     <para>
///         Every message goes through ONE dedicated high-priority dispatcher
///         thread inside the native device, never the caller's, so a wedged
///         MIDI interface can never block a UI thread. That also means
///         <see cref="Send(byte,byte,byte)" /> is ASYNCHRONOUS — use
///         <see cref="SentCount" /> if you need to know something reached the
///         wire, and <see cref="DroppedCount" /> to know something did not.
///     </para>
///     <para>
///         Lifetime is the same contract as <see cref="JuceMidiInput" />: the
///         engine keeps a weak registry for teardown ORDER and a strong list so
///         an output the app wired up and kept no handle to cannot be closed by
///         a gen-2 collection.
///     </para>
/// </summary>
public sealed class JuceMidiOutput : IDisposable, INativeChild, IJuceMidiOutput
{
    private readonly NativeDestruction _destruction = new();
    NativeDestruction INativeChild.Destruction => _destruction;

    // Drains in-flight native calls before dca_midi_output_close, which is a
    // bare `delete` native-side: it joins the dispatcher thread and destroys
    // the juce::MidiOutput. Without the drain a sequencer thread inside Send /
    // SendAt while the UI thread tears the engine down is a native
    // use-after-free rather than a caught ObjectDisposedException — and a
    // sequencer flushing note-offs during teardown IS the ordinary shutdown
    // path. Null ONLY when the constructor threw before it was assigned, so
    // every use is `_owner?.` (the finalizer runs on that instance).
    private readonly NativeHandleOwner? _owner;
    private Action? _disposedCallback;

    /// <summary>The engine whose render clock <see cref="SendAt(ReadOnlySpan{byte},long)" /> resolves
    /// frames against. Held as a reference and never as a raw handle: the
    /// engine claims its own handle before it disposes its children, so a
    /// scheduled send racing teardown degrades to "send now" instead of
    /// dereferencing a dead engine.</summary>
    private readonly JuceAudioEngine _engine;

    /// <summary>Zero-based device index passed to
    /// <see cref="JuceAudioEngine.OpenMidiOutput" />.</summary>
    public int DeviceIndex { get; }

    /// <summary>True after <see cref="Dispose" /> has run.</summary>
    public bool IsDisposed => !(_owner?.IsAlive ?? false);

    /// <summary>
    ///     Longest message the native dispatcher accepts, in bytes
    ///     (<c>MidiOutputDevice::kMaxMessageBytes</c>). Short messages only —
    ///     SysEx would mean a heap message handed to a real-time dispatcher
    ///     thread and is deliberately out of scope.
    /// </summary>
    public const int MaxMessageBytes = 3;

    /// <summary>Pin the handle for one native call. Pair with <see cref="Release" />
    /// in a finally.</summary>
    private nint Acquire()
        => (_owner ?? throw new ObjectDisposedException(nameof(JuceMidiOutput)))
            .Acquire(nameof(JuceMidiOutput));

    private void Release() => _owner?.Release();

    /// <summary>Set by the owning engine so an explicit Dispose un-roots this
    /// output from the engine's strong list. Not called from the finalizer —
    /// which can only run once nothing roots the output anyway.</summary>
    internal Action? Disposed
    {
        get => _disposedCallback;
        set => _disposedCallback = value;
    }

    internal JuceMidiOutput(JuceAudioEngine engine, int deviceIndex)
    {
        // Suppress BEFORE any throw. The CLR queues ~JuceMidiOutput for a
        // partially-constructed instance and a throwing finalizer is a
        // process-wide FailFast, not a catchable error — opening a device index
        // nothing is plugged into is an ordinary failure and must not leave one
        // of those behind. Pinned by FinalizerSafetyTests, which runs every
        // finalizer in this assembly against an uninitialised instance.
        try
        {
            _engine     = engine ?? throw new ArgumentNullException(nameof(engine));
            DeviceIndex = deviceIndex;
            JuceAudioEngine.Check(
                NativeMethods.MidiOutputOpen(deviceIndex, out var handle),
                "midi_output_open");
            _owner = new NativeHandleOwner(handle);
        }
        catch
        {
            GC.SuppressFinalize(this);
            throw;
        }
    }

    /// <summary>List names of all available MIDI output devices. The index of a
    /// name is the <c>deviceIndex</c> to open.</summary>
    /// <returns>Device names in index order; empty when the machine has no MIDI
    /// output ports.</returns>
    public static IReadOnlyList<string> Enumerate()
    {
        JuceAudioEngine.Check(NativeMethods.MidiOutputEnumerate(null, 0, out var size, out var count),
            "midi_output_enumerate_size");
        if (count <= 0 || size <= 0) return Array.Empty<string>();
        var buf = new byte[size];
        JuceAudioEngine.Check(NativeMethods.MidiOutputEnumerate(buf, buf.Length, out _, out count),
            "midi_output_enumerate");
        var end = Array.IndexOf(buf, (byte)0);
        if (end < 0) end = buf.Length;
        var text = Encoding.UTF8.GetString(buf, 0, end);
        return text.Length == 0 ? Array.Empty<string>() : text.Split('\n');
    }

    /// <summary>Send a 3-byte short message as soon as the dispatcher can.</summary>
    /// <param name="status">Status byte: command nibble | channel, e.g. <c>0x90</c> for
    /// note-on on channel 0.</param>
    /// <param name="data1">First data byte (0..127) — note number for note-on/off,
    /// controller number for a CC.</param>
    /// <param name="data2">Second data byte (0..127) — velocity for note-on/off,
    /// controller value for a CC.</param>
    /// <exception cref="ObjectDisposedException">The output has been disposed.</exception>
    /// <exception cref="JuceAudioException">The dispatcher queue is full — the message
    /// was DROPPED and <see cref="DroppedCount" /> has moved.</exception>
    public void Send(byte status, byte data1, byte data2)
        => Send(stackalloc byte[] { status, data1, data2 });

    /// <summary>
    ///     Send a raw SHORT message. SysEx is not supported: the dispatcher carries at
    ///     most <see cref="MaxMessageBytes" /> bytes per message, and that limit used to
    ///     be discovered at runtime as a generic argument error.
    /// </summary>
    /// <param name="data">1..<see cref="MaxMessageBytes" /> raw MIDI bytes.</param>
    /// <exception cref="ArgumentException"><paramref name="data" /> is empty. A caller
    /// with nothing to send is a bug in the caller, not a silent no-op.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="data" /> is longer
    /// than <see cref="MaxMessageBytes" />.</exception>
    /// <exception cref="ObjectDisposedException">The output has been disposed.</exception>
    /// <exception cref="JuceAudioException">The dispatcher queue is full — the message
    /// was DROPPED and <see cref="DroppedCount" /> has moved.</exception>
    public void Send(ReadOnlySpan<byte> data)
    {
        ValidateMessageLength(data.Length);
        var h = Acquire();
        try
        {
            unsafe
            {
                fixed (byte* p = data)
                    JuceAudioEngine.Check(NativeMethods.MidiOutputSend(h, p, data.Length),
                        "midi_output_send");
            }
        }
        finally { Release(); }
    }

    // Takes the LENGTH rather than the span so a test can drive it: a
    // ReadOnlySpan<byte> parameter cannot be reached by reflection, and the only
    // other way in is a real MIDI device, which CI does not have.
    private static void ValidateMessageLength(int length)
    {
        if (length <= 0)
            throw new ArgumentException("A MIDI message must carry at least one byte.", "data");
        if (length > MaxMessageBytes)
            throw new ArgumentOutOfRangeException("data", length,
                $"Short messages only: at most {MaxMessageBytes} bytes. SysEx is not supported.");
    }

    /// <summary>
    ///     Send when output frame <paramref name="renderFrame" /> reaches the
    ///     EAR — i.e. the deadline carries the engine's output latency, master
    ///     limiter included, so a hardware synth's note lines up with what the
    ///     user hears and not with what the callback has just rendered.
    ///     <para>
    ///         Frames are in the domain
    ///         <see cref="JuceAudioEngine.TryGetRenderClock" /> publishes. A
    ///         negative frame, or an engine with no device open, means "as soon
    ///         as possible".
    ///     </para>
    /// </summary>
    /// <param name="data">1..<see cref="MaxMessageBytes" /> raw MIDI bytes.</param>
    /// <param name="renderFrame">Output frame in the render-clock domain; negative
    /// means "as soon as possible".</param>
    /// <exception cref="ArgumentException"><paramref name="data" /> is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="data" /> is longer
    /// than <see cref="MaxMessageBytes" />.</exception>
    /// <exception cref="ObjectDisposedException">The output has been disposed.</exception>
    /// <exception cref="JuceAudioException">The dispatcher queue is full — the message
    /// was DROPPED and <see cref="DroppedCount" /> has moved.</exception>
    public void SendAt(ReadOnlySpan<byte> data, long renderFrame)
    {
        ValidateMessageLength(data.Length);
        var h = Acquire();
        try
        {
            var engineHandle = _engine.TryAcquire();
            if (engineHandle == nint.Zero)
            {
                // The engine is gone or going. Still deliver the message — a
                // pending note-off during teardown is exactly the one that must
                // not be lost — just without a schedule to hang it on. Sent
                // through the handle already pinned above, not through Send(),
                // so the drain sees ONE acquire for the whole call.
                unsafe
                {
                    fixed (byte* p = data)
                        JuceAudioEngine.Check(NativeMethods.MidiOutputSend(h, p, data.Length),
                            "midi_output_send");
                }
                return;
            }
            try
            {
                unsafe
                {
                    fixed (byte* p = data)
                        JuceAudioEngine.Check(
                            NativeMethods.MidiOutputSendAt(h, engineHandle, p, data.Length, renderFrame),
                            "midi_output_send_at");
                }
            }
            finally { _engine.Release(); }
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Send a 3-byte short message when output frame <paramref name="renderFrame" />
    ///     reaches the EAR. See <see cref="SendAt(ReadOnlySpan{byte},long)" /> for the
    ///     frame domain and the latency the deadline carries.
    /// </summary>
    /// <param name="status">Status byte: command nibble | channel.</param>
    /// <param name="data1">First data byte (0..127).</param>
    /// <param name="data2">Second data byte (0..127).</param>
    /// <param name="renderFrame">Output frame in the render-clock domain; negative
    /// means "as soon as possible".</param>
    /// <exception cref="ObjectDisposedException">The output has been disposed.</exception>
    /// <exception cref="JuceAudioException">The dispatcher queue is full — the message
    /// was DROPPED and <see cref="DroppedCount" /> has moved.</exception>
    public void SendAt(byte status, byte data1, byte data2, long renderFrame)
        => SendAt(stackalloc byte[] { status, data1, data2 }, renderFrame);

    /// <summary>
    ///     How many messages the native dispatcher has actually put on the wire.
    ///     A scheduler is trivially "working" if nothing ever checks that
    ///     anything came out the far end; this is that check. 0 on a disposed
    ///     output.
    /// </summary>
    public long SentCount => ReadCounters().Sent;

    /// <summary>
    ///     How many messages the native dispatcher THREW AWAY because its queue was
    ///     full. A silently swallowed note-off is a stuck note, which is the loudest
    ///     possible way to find out about this; the engine side counts the same fault
    ///     for instrument MIDI in <see cref="JuceAudioEngine.TryGetMidiTimingStats" />.
    ///     0 on a disposed output.
    /// </summary>
    public long DroppedCount => ReadCounters().Dropped;

    /// <summary>
    ///     How many messages the DRIVER threw on: they reached
    ///     <c>sendMessageNow</c> and did not come out the other side. 0 on a
    ///     disposed output.
    /// </summary>
    /// <remarks>
    ///     A different fault from <see cref="DroppedCount" />, which means "our
    ///     queue was full and we never handed it over". Before the two were
    ///     separated, <see cref="SentCount" /> moved on a throw as well, so a
    ///     wedged interface read as working — <c>Sent + Failed</c> is what
    ///     <c>Sent</c> alone used to report. Requires ABI v36
    ///     (<c>dca_midi_output_get_counts3</c>).
    /// </remarks>
    public long FailedCount => ReadAllCounters().Failed;

    /// <summary>
    ///     Both dispatcher counters in ONE native call, so a caller comparing them
    ///     cannot read them a queue-full apart.
    /// </summary>
    /// <returns><c>Sent</c> — messages put on the wire; <c>Dropped</c> — messages
    /// discarded because the dispatcher queue was full. <c>(0, 0)</c> on a disposed
    /// output.</returns>
    public (long Sent, long Dropped) ReadCounters()
    {
        var h = _owner?.TryAcquire() ?? nint.Zero;
        if (h == nint.Zero) return (0, 0);
        try
        {
            return NativeMethods.MidiOutputGetCounts(h, out var sent, out var dropped)
                   == NativeMethods.DcaStatus.Ok
                ? (sent, dropped) : (0, 0);
        }
        finally { Release(); }
    }

    /// <summary>
    ///     All THREE dispatcher counters in one native call (ABI v36), so a caller
    ///     comparing them cannot read them a message apart.
    /// </summary>
    /// <returns><c>Sent</c> — messages put on the wire; <c>Dropped</c> — discarded
    /// because the dispatcher queue was full; <c>Failed</c> — handed to the driver,
    /// which threw. <c>(0, 0, 0)</c> on a disposed output.</returns>
    public (long Sent, long Dropped, long Failed) ReadAllCounters()
    {
        var h = _owner?.TryAcquire() ?? nint.Zero;
        if (h == nint.Zero) return (0, 0, 0);
        try
        {
            return NativeMethods.MidiOutputGetCounts3(h, out var sent, out var dropped, out var failed)
                   == NativeMethods.DcaStatus.Ok
                ? (sent, dropped, failed) : (0, 0, 0);
        }
        finally { Release(); }
    }

    /// <summary>
    /// Un-root this output from its engine and close the native MIDI device
    /// (<c>dca_midi_output_close</c>). Idempotent: the handle is claimed atomically so a
    /// concurrent Dispose or the finalizer cannot double-close, and in-flight
    /// <see cref="Send(ReadOnlySpan{byte})" /> / <see cref="SendAt(ReadOnlySpan{byte},long)" />
    /// calls are DRAINED first — the native close joins the dispatcher thread and destroys
    /// the device, so closing under a live send is a use-after-free.
    /// </summary>
    public void Dispose()
    {
        DisposeNativeHandle(disposing: true);
        GC.SuppressFinalize(this);
    }

    private void DisposeNativeHandle(bool disposing) => _destruction?.Run(() =>
    {
        if (_owner is null) return true;
        if (disposing)
        {
            try { Interlocked.Exchange(ref _disposedCallback, null)?.Invoke(); }
            catch (Exception ex)
            {
                JuceLog.Emit(JuceLogLevel.Warn,
                    $"MIDI output un-root threw: {ex.GetType().Name}: {ex.Message}");
            }
        }

        // THE FINALIZER MAY NOT WAIT (deadline 0): one finalizer thread serves
        // the whole process. On that path a send still in flight means the
        // handle is leaked and logged at Error — a leak beats a use-after-free.
        var h = _owner.TakeForDispose(nameof(JuceMidiOutput),
            disposing ? NativeHandleOwner.DefaultDrainTimeoutMs : 0, out _);
        if (h == nint.Zero) return false;

        var success = false;
        try { success = NativeMethods.MidiOutputClose(h) == NativeMethods.DcaStatus.Ok; }
        catch (Exception ex)
        {
            JuceLog.Emit(JuceLogLevel.Warn,
                $"MIDI output close threw: {ex.GetType().Name}: {ex.Message}");
        }
        return success;
    }, this);

    /// <summary>
    /// Close the device for an output nobody disposed. Touches no other managed object —
    /// the engine's list may already be gone — and tolerates a constructor that threw
    /// before the handle owner existed. Unlike <see cref="Dispose" /> it does NOT wait for
    /// in-flight sends, so a send still running means the handle is leaked.
    /// </summary>
    ~JuceMidiOutput()
    {
        // A finalizer must not touch other managed objects (the engine's list
        // may already be gone); it only closes the native device.
        _disposedCallback = null;
        DisposeNativeHandle(disposing: false);
    }
}
