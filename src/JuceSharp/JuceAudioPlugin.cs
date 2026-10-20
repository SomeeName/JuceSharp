using System.ComponentModel;
using System.Text;
using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
/// Safe RAII wrapper around a native VST3 plugin handle. Owns the
/// plugin's lifetime: disposing detaches it from the input and
/// instrument graphs and tears the editor down on the JUCE message
/// thread — even if the host code forgot to detach first.
/// </summary>
public sealed class JuceAudioPlugin : IDisposable, INativeChild, IJuceAudioPlugin
{
    private readonly NativeDestruction _destruction = new();
    NativeDestruction INativeChild.Destruction => _destruction;

    /// <summary>Path the plugin was loaded from.</summary>
    public string Path { get; }

    /// <summary>True after <see cref="Dispose()"/> has run.</summary>
    public bool IsDisposed => !(_owner?.IsAlive ?? false);

    /// <summary>Raw native handle. Prefer using the wrapper methods —
    /// this is exposed for interop with code that still takes
    /// <c>nint</c> plugin handles. Callers must externally guarantee the
    /// plugin outlives any use of the raw handle: it is UNPINNED, so it
    /// bypasses the drain that makes every method on this type safe against a
    /// concurrent Dispose. Hidden from IntelliSense so it stops reading as a
    /// normal way to use a plugin.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public nint Handle => _owner?.TryPeek() ?? nint.Zero;

    // Drains in-flight native calls before unload so a parameter query on
    // one thread can never race the plugin teardown on another.
    //
    // Null ONLY when the constructor threw before it was assigned. The
    // finalizer still runs on that partially-constructed instance, so
    // Dispose has to tolerate it — see JuceAudioEngine._owner, which carries
    // the same note. A finalizer that throws is not a caught exception: the
    // CLR fails the whole process fast, which is why this one null reference
    // used to kill the test host at an arbitrary point minutes later.
    private readonly NativeHandleOwner? _owner;

    // Rooted for the plugin's lifetime — the native side holds the raw
    // function pointer and would call into freed memory if this were
    // collected.
    private readonly NativeMethods.DcaEditorClosedCallback _editorClosedThunk;

    /// <summary>
    /// Raised on the JUCE message thread after the user closed the plugin's
    /// editor window with its native close button. Hosts that track an
    /// "editor open" flag must clear it here or the flag goes stale and the
    /// next open request toggles the wrong way. Marshal to your UI thread;
    /// do not block.
    /// </summary>
    public event Action? EditorClosedByUser;

    /// <summary>Pin the handle for one native call that must fail loudly when
    /// the plugin is gone. Pair with <see cref="Release"/> in a finally.</summary>
    private nint Acquire()
        => (_owner ?? throw new ObjectDisposedException(nameof(JuceAudioPlugin)))
            .Acquire(nameof(JuceAudioPlugin));

    /// <summary>Pin the handle for one fire-and-forget native call that should
    /// silently no-op when the plugin is gone. Returns <see cref="nint.Zero"/>
    /// when unavailable — call <see cref="Release"/> ONLY when it does not.</summary>
    private nint TryAcquire() => _owner?.TryAcquire() ?? nint.Zero;

    private void Release() => _owner?.Release();

    internal JuceAudioPlugin(nint handle, string path)
    {
        if (handle == nint.Zero)
        {
            // Suppress BEFORE throwing: the CLR still queues the finalizer for a
            // partially-constructed instance, and a throwing finalizer is a
            // process-wide FailFast rather than a catchable error.
            GC.SuppressFinalize(this);
            throw new ArgumentException("Plugin handle must not be zero.", nameof(handle));
        }
        _owner = new NativeHandleOwner(handle);
        Path   = path;

        _editorClosedThunk = _ =>
        {
            try { EditorClosedByUser?.Invoke(); }
            catch
            {
                // Never let a host exception escape into the JUCE message
                // thread — it would tear the native dispatch loop down.
            }
        };
        _ = NativeMethods.PluginSetEditorClosedCallback(handle, _editorClosedThunk, nint.Zero);
    }

    /// <summary>Insert this plugin into a player's input (effects) chain.
    /// <para>ABI v28: the engine hosts one input chain PER SEAT. Seat 0 is the
    /// default and is exactly the pre-v28 behaviour. Attaching an
    /// already-attached plugin to a DIFFERENT seat MOVES it — native detaches
    /// first — so one instance is never run twice per block over two different
    /// signals.</para></summary>
    public void AttachToInput(int player = 0)
    {
        var h = Acquire();
        try
        {
            // Under the same gate as Bypass: the two drive ONE piece of native
            // state (the input-chain attachment), so they have to serialise
            // against each other, not just each against itself.
            lock (_bypassGate)
            {
                JuceAudioEngine.Check(NativeMethods.PluginAttachToInputForPlayer(h, player),
                    "plugin_attach_to_input_for_player");
                // Remember the seat so a Bypass round trip re-attaches HERE. Without
                // this, bypassing player 2's amp sim and un-bypassing it would
                // silently move it onto player 1's chain.
                _inputPlayer = player;
            }
        }
        finally { Release(); }
    }

    /// <summary>Remove this plugin from whichever seat's input chain holds it.
    /// <para>This IS the bypass, so <see cref="Bypass" /> reports <c>true</c>
    /// afterwards — the two are one piece of state, and they disagreed until
    /// the getter stopped caching (W4-M2).</para></summary>
    public void DetachFromInput()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            lock (_bypassGate)
                JuceAudioEngine.WarnIfFailed(null, NativeMethods.PluginDetachFromInput(h),
                    "plugin_detach_from_input");
        }
        finally { Release(); }
    }

    /// <summary>The seat whose input chain currently holds this plugin, or
    /// <c>-1</c> when it is not attached (ABI v28).</summary>
    public int InputPlayer
    {
        get
        {
            var h = TryAcquire();
            if (h == nint.Zero) return -1;
            try { return NativeMethods.PluginGetInputPlayer(h); }
            finally { Release(); }
        }
    }

    // The seat AttachToInput last used. Bypass detaches and re-attaches, and it
    // has to land back on the same chain.
    private int _inputPlayer;

    /// <summary>Attach this plugin as the engine's MIDI instrument.
    /// Subsequent <c>SendMidi</c> calls are routed to it.</summary>
    public void AttachToInstrument()
    {
        var h = Acquire();
        try { JuceAudioEngine.Check(NativeMethods.PluginAttachToInstrument(h), "plugin_attach_to_instrument"); }
        finally { Release(); }
    }

    /// <summary>Remove this plugin from the instrument bus.</summary>
    public void DetachFromInstrument()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { JuceAudioEngine.WarnIfFailed(null, NativeMethods.PluginDetachFromInstrument(h), "plugin_detach_from_instrument"); }
        finally { Release(); }
    }

    /// <summary>Show the plugin's editor window on the JUCE message
    /// thread. Safe to call repeatedly.</summary>
    public void ShowEditor()
    {
        var h = Acquire();
        try { JuceAudioEngine.WarnIfFailed(null, NativeMethods.PluginShowEditor(h, nint.Zero), "plugin_show_editor"); }
        finally { Release(); }
    }

    /// <summary>Hide the plugin's editor window. No-op if it is not open.</summary>
    public void HideEditor()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { JuceAudioEngine.WarnIfFailed(null, NativeMethods.PluginHideEditor(h), "plugin_hide_editor"); }
        finally { Release(); }
    }

    /// <summary>
    ///     Bypass the plugin without unloading. Implemented by detaching it from
    ///     the input chain when set to <c>true</c> and re-attaching — to the seat
    ///     <see cref="AttachToInput(int)" /> last used, never seat 0 — on
    ///     <c>false</c>. Idempotent.
    ///     <para>
    ///         <b>The getter asks the ENGINE</b> (<see cref="InputPlayer" />,
    ///         <c>dca_plugin_get_input_player</c>): bypassed means "not in any
    ///         seat's input chain", which is the only thing this property has ever
    ///         meant and the only answer that cannot drift. It cached a
    ///         <c>bool</c> until W4-M2, and
    ///         <see cref="AttachToInput(int)" />/<see cref="DetachFromInput" />
    ///         wrote nothing to it, so two ordinary sequences lied:
    ///         <c>DetachFromInput(); Bypass = false;</c> early-outed on a flag that
    ///         was already <c>false</c> and left the plugin DETACHED while
    ///         reporting "active", and <c>Bypass = true; AttachToInput(2);</c>
    ///         processed audio while reporting "bypassed" — a host drawing its
    ///         footswitch from this property drew the wrong state either way.
    ///     </para>
    ///     <para>
    ///         Consequence worth knowing: a plugin that has never been attached to
    ///         an input chain (an instrument, or one just loaded) reads as
    ///         <c>true</c>, because it is not in the signal path. Setting it to
    ///         <c>false</c> attaches it to <see cref="InputPlayer" />'s remembered
    ///         seat — 0 by default.
    ///     </para>
    /// </summary>
    public bool Bypass
    {
        get => InputPlayer < 0;
        set
        {
            var h = Acquire();
            try
            {
                // The read and the native attach/detach are one unit, or two
                // toggles could interleave and leave the plugin in a state
                // neither caller asked for (concurrency audit #10). The gate is
                // private to this plugin and is shared with AttachToInput /
                // DetachFromInput, which drive the same attachment.
                lock (_bypassGate)
                {
                    // The engine's answer, not a cache, and read under the gate
                    // so the idempotence check and the write cannot be split.
                    if (NativeMethods.PluginGetInputPlayer(h) < 0 == value) return;
                    // Re-attach to the seat this plugin belongs to, never seat 0:
                    // un-bypassing player 2's amp sim must not move it onto player
                    // 1's chain (ABI v28).
                    var st = value
                        ? NativeMethods.PluginDetachFromInput(h)
                        : NativeMethods.PluginAttachToInputForPlayer(h, _inputPlayer);
                    if (st != NativeMethods.DcaStatus.Ok)
                        JuceLog.Emit(JuceLogLevel.Warn,
                            $"Plugin bypass toggle ({value}) for '{Path}' returned {st}.");
                }
            }
            finally { Release(); }
        }
    }
    private readonly object _bypassGate = new();

    /// <summary>Capture the plugin's opaque state blob (vendor format)
    /// for later restoration with <see cref="LoadState"/>.</summary>
    public byte[] SaveState()
    {
        var h = Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.PluginSaveState(h, null, 0, out var size), "plugin_save_state_size");
            if (size <= 0) return Array.Empty<byte>();
            var buf = new byte[size];
            JuceAudioEngine.Check(NativeMethods.PluginSaveState(h, buf, buf.Length, out var written), "plugin_save_state");
            if (written != size)
                Array.Resize(ref buf, Math.Max(0, written));
            return buf;
        }
        finally { Release(); }
    }

    /// <summary>Restore a previously captured state blob. Passing an
    /// empty array is a no-op.</summary>
    public void LoadState(byte[] data)
    {
        if (data is null) throw new ArgumentNullException(nameof(data));
        var h = Acquire();
        try
        {
            if (data.Length == 0) return;
            JuceAudioEngine.Check(NativeMethods.PluginLoadState(h, data, data.Length), "plugin_load_state");
        }
        finally { Release(); }
    }

    /// <summary>
    /// Number of hosted parameters exposed by the plugin. Use
    /// <see cref="GetParameter"/>, <see cref="SetParameter"/> and
    /// <see cref="GetParameterInfo"/> to access individual parameters.
    /// </summary>
    public int ParameterCount
    {
        get
        {
            var h = Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.PluginGetParameterCount(h, out var n), "plugin_get_parameter_count");
                return n;
            }
            finally { Release(); }
        }
    }

    /// <summary>Read the current normalised value (0..1) of a parameter.</summary>
    public float GetParameter(int index)
    {
        var h = Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.PluginGetParameter(h, index, out var v), "plugin_get_parameter");
            return v;
        }
        finally { Release(); }
    }

    /// <summary>Set the normalised value (0..1) of a parameter. Notifies the host.</summary>
    public void SetParameter(int index, float value)
    {
        var h = Acquire();
        try { JuceAudioEngine.Check(NativeMethods.PluginSetParameter(h, index, value), "plugin_set_parameter"); }
        finally { Release(); }
    }

    /// <summary>
    /// Reports the plugin's own processing latency, in samples at the engine's
    /// current sample rate. Use this to schedule MIDI events earlier so the
    /// plugin's processed audio arrives in sync with un-processed audio
    /// paths (e.g. file playback). Returns 0 when the plugin reports no
    /// latency. Re-query after engine sample-rate changes.
    /// </summary>
    public int LatencySamples
    {
        get
        {
            var h = Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.PluginGetLatencySamples(h, out var s), "plugin_get_latency_samples");
                return s;
            }
            finally { Release(); }
        }
    }

    /// <summary>Describe a parameter: human-readable name, default value
    /// (normalised) and number of discrete steps (0 = continuous).</summary>
    public JuceParameterInfo GetParameterInfo(int index)
    {
        var h = Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.PluginGetParameterInfo(h, index, null, 0,
                out var needed, out _, out _), "plugin_get_parameter_info_size");
            var buf = new byte[Math.Max(1, needed + 1)];
            JuceAudioEngine.Check(NativeMethods.PluginGetParameterInfo(h, index, buf, buf.Length,
                out var len, out var def, out var steps), "plugin_get_parameter_info");
            var name = Encoding.UTF8.GetString(buf, 0, Math.Min(len, buf.Length));
            return new JuceParameterInfo(index, name, def, steps);
        }
        finally { Release(); }
    }

    /// <summary>Enumerate all parameters as an array of info records.</summary>
    public JuceParameterInfo[] EnumerateParameters()
    {
        var count = ParameterCount;
        var result = new JuceParameterInfo[count];
        for (var i = 0; i < count; i++) result[i] = GetParameterInfo(i);
        return result;
    }

    /// <summary>
    /// Detach the editor-closed callback, drain any in-flight parameter query, and unload
    /// the VST3 (<c>dca_plugin_unload</c>). Idempotent and safe against a concurrent
    /// Dispose or the finalizer — the handle is claimed atomically, so only one caller
    /// unloads. A drain that expires leaks the handle (logged at Error) rather than
    /// freeing under a live call; a cold VST3 scan routinely runs past 25 s.
    /// </summary>
    public void Dispose() => Dispose(disposing: true);

    private void Dispose(bool disposing) => _destruction?.Run(() =>
    {
        // TakeForDispose atomically claims the handle (so concurrent Dispose
        // calls and the finalizer race cannot both unload) and drains any
        // in-flight parameter queries from other threads first.
        // `_owner?` — never `_owner.` — because the finalizer reaches here on a
        // partially-constructed instance whose fields are all still null.
        //
        // THE FINALIZER MAY NOT WAIT (deadline 0). There is one finalizer thread
        // in the process, so a 60 s drain here does not stall this plugin — it
        // stalls every other finalizer behind it, and a cold VST3 is exactly the
        // kind of call that reaches that ceiling. On that path a still-running
        // call means the handle is leaked and logged at Error, which is the
        // trade this drain exists to make anyway.
        var h = _owner?.TakeForDispose(nameof(JuceAudioPlugin),
                    disposing ? NativeHandleOwner.DefaultDrainTimeoutMs : 0, out _) ?? nint.Zero;
        if (h == nint.Zero) return _owner is null;
        GC.SuppressFinalize(this);
        // Detach the editor-closed notification first so a close event that
        // lands mid-unload can't call back into a disposing wrapper.
        try { _ = NativeMethods.PluginSetEditorClosedCallback(h, null, nint.Zero); }
        catch { /* best-effort */ }
        var success = false;
        try { success = NativeMethods.PluginUnload(h) == NativeMethods.DcaStatus.Ok; }
        catch (Exception ex)
        {
            JuceLog.Emit(JuceLogLevel.Warn,
                $"Plugin unload threw for '{Path}': {ex.GetType().Name}: {ex.Message}");
        }
        // The thunk registered in the constructor is reachable ONLY through
        // `this`, and `GC.SuppressFinalize` above is the last read of `this` the
        // JIT can see — so from there on the object is collectible while native
        // still holds the raw function pointer. Keep it alive until the
        // un-registration has actually happened. JuceMidiInput.Dispose has the
        // matching call for the same reason.
        GC.KeepAlive(_editorClosedThunk);
        GC.KeepAlive(this);
        return success;
    }, this);

    /// <summary>
    /// Unload a plugin nobody disposed. Tolerates a constructor that threw before the
    /// handle owner existed, and does NOT wait for in-flight calls (deadline 0), because
    /// the single finalizer thread must not stall behind a 60-second drain.
    /// </summary>
    ~JuceAudioPlugin() => Dispose(disposing: false);
}
