using System.Runtime.InteropServices;
using System.Text;
using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
/// A delivered MIDI message. <paramref name="TimestampSeconds"/> is in the
/// driver's clock domain — useful for relative ordering, not wall time.
/// </summary>
public readonly record struct JuceMidiMessage(double TimestampSeconds, byte[] Data)
{
    /// <summary>Status byte (first byte). 0 if the message is empty.</summary>
    public byte Status  => Data is { Length: > 0 } ? Data[0] : (byte)0;
    /// <summary>First data byte (e.g. note number for note-on).</summary>
    public byte Data1   => Data is { Length: > 1 } ? Data[1] : (byte)0;
    /// <summary>Second data byte (e.g. velocity for note-on).</summary>
    public byte Data2   => Data is { Length: > 2 } ? Data[2] : (byte)0;
    /// <summary>Channel (0..15) extracted from the status byte.</summary>
    public int  Channel => Status & 0x0F;
    /// <summary>Channel-voice command nibble (0x80..0xF0).</summary>
    public int  Command => Status & 0xF0;
}

/// <summary>
///     Handler for a raw MIDI packet, invoked on the JUCE MIDI thread with the
///     native buffer borrowed IN PLACE.
///     <para>
///         The span is valid only for the duration of the call — it points at
///         native memory the driver owns. Copy anything you keep. In exchange
///         nothing is allocated per message, which is the point: the
///         <see cref="JuceMidiMessage" /> overload allocates a
///         <see cref="byte" />[] for every event, and a controller sweeping a
///         CC or a sustain pedal held under a chord produces hundreds a second
///         straight into gen0 from a real-time driver thread.
///     </para>
/// </summary>
public delegate void JuceMidiSpanHandler(double timestampSeconds, ReadOnlySpan<byte> data);

/// <summary>
/// Live MIDI input device. Created by
/// <see cref="JuceAudioEngine.OpenMidiInput(int, Action{JuceMidiMessage})"/>
/// or its allocation-free <see cref="JuceMidiSpanHandler"/> overload.
/// Dispose to stop and release the device.
/// </summary>
public sealed class JuceMidiInput : IDisposable, INativeChild, IJuceMidiInput
{
    private readonly NativeDestruction _destruction = new();
    NativeDestruction INativeChild.Destruction => _destruction;

    // Drains in-flight DISPATCHES before the close. This type makes no
    // post-construction native CALL of its own, which is exactly why the drain
    // was written as a no-op and stayed one: wave-2 #25 added the owner and not
    // the acquire, so `_inFlight` never left 0, `TakeForDispose` always reported
    // Drained, and `dca_midi_input_close` (`device->stop(); b->device.reset();
    // delete b;`) ran while the JUCE driver thread was inside OnMidi holding a
    // ReadOnlySpan<byte> over the message the object being deleted owns.
    // OnMidi now pins the handle for its whole body, so the close waits for it
    // and a callback that arrives after the claim returns without invoking the
    // user handler. Null ONLY when the constructor threw before it was
    // assigned — the finalizer still runs on that instance, so every use is
    // `_owner?.`.
    private readonly NativeHandleOwner? _owner;
    // Held as a field (not a local) so the marshalled delegate stays rooted
    // while native MIDI thread keeps invoking it. GC.KeepAlive in Dispose
    // makes the read observable so analyzers do not flag this as convertible
    // to a local variable.
    private readonly NativeMethods.DcaMidiCallback _callback;
    private readonly Action<JuceMidiMessage>? _userCallback;
    private readonly JuceMidiSpanHandler? _spanCallback;

    /// <summary>Zero-based device index passed to
    /// <see cref="JuceAudioEngine.OpenMidiInput(int, Action{JuceMidiMessage})"/>.</summary>
    public int DeviceIndex { get; }

    /// <summary>Set by the owning engine so an explicit Dispose un-roots this
    /// input from the engine's strong list. Not called from the finalizer —
    /// which can only run once nothing roots the input anyway.</summary>
    internal Action? Disposed
    {
        get => _disposedCallback;
        set => _disposedCallback = value;
    }

    /// <summary>True after <see cref="Dispose"/> has run.</summary>
    public bool IsDisposed => !(_owner?.IsAlive ?? false);

    internal JuceMidiInput(int deviceIndex, Action<JuceMidiMessage> handler)
        : this(deviceIndex, handler ?? throw new ArgumentNullException(nameof(handler)), null) { }

    internal JuceMidiInput(int deviceIndex, JuceMidiSpanHandler handler)
        : this(deviceIndex, null, handler ?? throw new ArgumentNullException(nameof(handler))) { }

    private JuceMidiInput(int deviceIndex, Action<JuceMidiMessage>? handler, JuceMidiSpanHandler? spanHandler)
    {
        // Suppress BEFORE any throw: the CLR queues ~JuceMidiInput for a
        // partially-constructed instance, and a throwing finalizer is a
        // process-wide FailFast, not a catchable error. midi_input_open on a
        // device index nothing is plugged into is an ordinary failure and used
        // to leave exactly that instance behind.
        try
        {
            DeviceIndex   = deviceIndex;
            _userCallback = handler;
            _spanCallback = spanHandler;
            _callback     = OnMidi;
            JuceAudioEngine.Check(
                NativeMethods.MidiInputOpen(deviceIndex, _callback, nint.Zero, out var handle),
                "midi_input_open");
            _owner = new NativeHandleOwner(handle);
        }
        catch
        {
            GC.SuppressFinalize(this);
            throw;
        }
    }

    private unsafe void OnMidi(nint user, double timestampSeconds, nint dataPtr, int length)
    {
        // Catch-all body, and it is not optional: this is a reverse-P/Invoke
        // from a JUCE driver thread, so an escaping exception unwinds through
        // C++ frames and kills the process. NativeCallbackSafetyTests lints it.
        try
        {
            // THE dispatch pin. Zero means either Dispose has claimed the
            // handle — the device is being deleted underneath this very call,
            // so returning is the only correct answer — or the constructor has
            // not published the owner yet, a window of a few instructions
            // between midi_input_open and the field store in which dropping one
            // message is the cheap side of the trade. Release ONLY when
            // non-zero (TryAcquire has already released otherwise).
            var h = _owner?.TryAcquire() ?? nint.Zero;
            if (h == nint.Zero) return;
            try
            {
                if (length <= 0 || dataPtr == nint.Zero) return;
                if (_spanCallback is not null)
                {
                    // Borrowed in place: no per-message byte[] on a real-time thread.
                    _spanCallback(timestampSeconds, new ReadOnlySpan<byte>((void*)dataPtr, length));
                    return;
                }
                var buf = new byte[length];
                Marshal.Copy(dataPtr, buf, 0, length);
                _userCallback!(new JuceMidiMessage(timestampSeconds, buf));
            }
            finally { _owner!.Release(); }
        }
        catch (Exception ex)
        {
            JuceLog.Emit(JuceLogLevel.Warn,
                $"MIDI input handler threw: {ex.GetType().Name}: {ex.Message}");
        }
    }

    /// <summary>List names of all available MIDI input devices.</summary>
    public static IReadOnlyList<string> Enumerate()
    {
        JuceAudioEngine.Check(NativeMethods.MidiInputEnumerate(null, 0, out var size, out var count),
            "midi_input_enumerate_size");
        if (count <= 0 || size <= 0) return Array.Empty<string>();
        var buf = new byte[size];
        JuceAudioEngine.Check(NativeMethods.MidiInputEnumerate(buf, buf.Length, out _, out count),
            "midi_input_enumerate");
        // Trim trailing NUL before splitting.
        var end = Array.IndexOf(buf, (byte)0);
        if (end < 0) end = buf.Length;
        var text = Encoding.UTF8.GetString(buf, 0, end);
        if (text.Length == 0) return Array.Empty<string>();
        return text.Split('\n');
    }

    /// <summary>
    /// Un-root this input from its engine and close the native MIDI device
    /// (<c>dca_midi_input_close</c>). Idempotent: the handle is claimed atomically so a
    /// concurrent Dispose or the finalizer cannot double-close, in-flight native calls are
    /// DRAINED before the close, and the managed callback thunk is kept alive until native
    /// has stopped calling it.
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
                    $"MIDI input un-root threw: {ex.GetType().Name}: {ex.Message}");
            }
        }

        // THE FINALIZER MAY NOT WAIT (deadline 0): one finalizer thread serves
        // the whole process. On that path a call still in flight means the
        // handle is leaked and logged at Error — the trade the drain exists to
        // make, and a leak beats a use-after-free.
        var h = _owner.TakeForDispose(nameof(JuceMidiInput),
            disposing ? NativeHandleOwner.DefaultDrainTimeoutMs : 0, out _);
        if (h == nint.Zero) return false;

        var success = false;
        try { success = NativeMethods.MidiInputClose(h) == NativeMethods.DcaStatus.Ok; }
        catch (Exception ex)
        {
            JuceLog.Emit(JuceLogLevel.Warn,
                $"MIDI input close threw: {ex.GetType().Name}: {ex.Message}");
        }
        GC.KeepAlive(_callback);
        return success;
    }, this);

    private Action? _disposedCallback;

    /// <summary>
    /// Close the device for an input nobody disposed. Touches no other managed object —
    /// the engine's list may already be gone — and tolerates a constructor that threw
    /// before the handle owner existed. Unlike <see cref="Dispose" /> it does NOT wait for
    /// in-flight calls, so a call still running means the handle is leaked.
    /// </summary>
    ~JuceMidiInput()
    {
        // A finalizer must not touch other managed objects (the engine's list
        // may already be gone); it only closes the native device.
        _disposedCallback = null;
        DisposeNativeHandle(disposing: false);
    }
}
