// ReSharper disable InconsistentNaming -- private static engine state uses s_/_camelCase to distinguish per-call vs cross-call locks.
using JuceSharp.Dsp;
using System.Diagnostics;
using System.Runtime.InteropServices;
using JuceSharp.Interop;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace JuceSharp;

/// <summary>
/// High-level managed wrapper around the native JUCE engine. One instance
/// owns the audio device, the output graph, and any opened sources or
/// plugins.
/// </summary>
public sealed partial class JuceAudioEngine : IDisposable
{
    // --- Process-wide native log callback registry --------------------
    //
    // The native side exposes a single (callback, userdata) slot via
    // dca_set_log_callback. When more than one JuceAudioEngine exists in a
    // process (uncommon but legal: tests, embedded scenarios) the *last*
    // ctor wins, and the *first* Dispose used to clear the callback while
    // the other engine was still running, blinding it to native log
    // messages. Track active engines and reference-count the registration
    // so only the last surviving engine clears the slot.
    private static readonly object s_logRegistryLock = new();
    private static readonly List<WeakReference<JuceAudioEngine>> s_activeEngines = new();
    private static readonly NativeMethods.DcaLogCallback s_staticLogTrampoline = StaticOnNativeLog;
    private static int s_logRegistrations;

    private readonly ILogger _log;

    // Drains in-flight native calls before destroy, exactly like JuceAudioSource.
    // Callers poll this engine from several threads at once — note detection
    // pulls the capture ring, the tuner reads the input peak, the UI reads
    // SampleRate — while another thread can Dispose it on a device change.
    // With a raw handle, destroy could land between the alive-check and the
    // P/Invoke and the poll would dereference freed native memory.
    //
    // Null ONLY when the constructor threw before engine_create returned. The
    // finalizer still runs on that partially-constructed instance, so every
    // use below has to tolerate it.
    private readonly NativeHandleOwner? _owner;
    private bool _started;
    private bool _stoppedOnce;   // Start() refuses once set (#27)

    // --- Child wrapper registry ---------------------------------------
    //
    // THE ENGINE'S OWN DRAIN SAYS NOTHING ABOUT ITS CHILDREN. Every child
    // wrapper (JuceAudioSource, JuceAudioPlugin, JuceMidiInput) owns a
    // SEPARATE NativeHandleOwner, so `_owner.TakeForDispose()` reaching zero
    // only proves that no `dca_engine_*` call is running — a `dca_source_*`
    // call on the 4 ms position timer is invisible to it. Until 0.61 that is
    // exactly what shipped: a device change or a watchdog recovery disposed
    // the engine while JuceAudioPlaybackService's position poll was inside
    // `dca_source_get_audible_position_seconds`, and `dca_engine_destroy`
    // reset the source's transport underneath it. An access violation, not
    // the ObjectDisposedException the guides promise.
    //
    // WEAK references on purpose: a host that opens a source and drops it must
    // still be able to collect it, and a strong list here would turn the
    // engine into a leak for the life of the process. The list is claimed
    // atomically at dispose (set to null) AFTER the engine handle is taken, so
    // no new child can be registered past that point — OpenFile / LoadPlugin
    // both Acquire the engine first and now throw.
    //
    // JuceLoopRegion is NOT tracked here and does not need to be: a region is
    // owned by its JuceAudioSource (`JuceAudioSource.Loop`), and that source's
    // Dispose disposes it. Draining the sources drains the regions with them.
    /// <summary>
    ///     MIDI inputs are held STRONGLY, unlike every other child. The weak
    ///     child registry below exists for teardown ORDER; it does not keep a
    ///     child alive, and nothing else does for a MIDI input — the app wires
    ///     the callback and has no further use for the handle. A host that
    ///     discarded the returned <see cref="JuceMidiInput" /> got working MIDI
    ///     until the first gen-2 collection ran its finalizer and closed the
    ///     device, silently (concurrency audit #14; the comment on OpenMidiInput
    ///     claimed the weak registry fixed this, and it never could). An input
    ///     leaves this list when IT is disposed, or when the engine is.
    /// </summary>
    private readonly List<JuceMidiInput> _midiInputs = new();
    // Same strong-rooting reason as _midiInputs (ABI v34): an app that opens an
    // output, wires a scheduler to it and keeps no handle would otherwise have
    // the device closed by the first gen-2 collection. The weak child registry
    // is for teardown ORDER and roots nothing.
    private readonly List<JuceMidiOutput> _midiOutputs = new();

    private readonly object _childrenGate = new();
    private List<(WeakReference<IDisposable> Child, NativeDestruction Destruction)>? _children = new();

    /// <summary>The opaque native <c>dca_engine*</c> handle, or
    /// <see cref="nint.Zero"/> before create / after dispose. Exposed so the
    /// in-house DSP tone-rig control surface (0.16, <c>dca_engine_dsp_*</c>) can
    /// address this engine's chain. Do not dereference on the managed side.
    /// Unpinned: the caller must guarantee this engine outlives the use.</summary>
    public nint NativeHandle => _owner?.TryPeek() ?? nint.Zero;

    /// <summary>
    /// Validate <paramref name="config"/>, check the loaded <c>juce_sharp.dll</c>'s
    /// <c>DCA_ABI_VERSION</c> against the one this wrapper mirrors, and create the native
    /// engine (<c>dca_engine_create</c>). The device is not opened until
    /// <see cref="Start"/>.
    /// </summary>
    /// <param name="config">Device request; <c>null</c> means all-defaults. Its
    /// <c>Validate()</c> runs first and throws <see cref="ArgumentOutOfRangeException"/>
    /// on an out-of-range field.</param>
    /// <param name="logger">Sink for native log lines. When <c>null</c> and
    /// <see cref="JuceEngineConfig.EnableConsoleLog"/> is true, a
    /// <see cref="JuceConsoleLogger"/> is wired so messages are visible out of the box.</param>
    /// <exception cref="JuceAbiMismatchException">The DLL's ABI version is not exactly the
    /// one this wrapper expects — rebuild the native DLL; never widen the check.</exception>
    /// <exception cref="JuceAudioException"><c>dca_engine_create</c> failed.</exception>
    public JuceAudioEngine(JuceEngineConfig? config = null, ILogger<JuceAudioEngine>? logger = null)
    {
        // EVERY throwing exit below suppresses finalization FIRST. The CLR
        // queues ~JuceAudioEngine for a partially-constructed instance the
        // moment the allocation succeeds, and a throwing finalizer is a
        // process-wide FailFast rather than a catchable error — the shape that
        // made a PASSING JuceAudioPlugin unit test kill the whole test host
        // hundreds of tests later, which is why that ctor already does this and
        // this one did not. Validate(), VerifyAbi() and engine_create can all
        // throw, and none of them leaves the finalizer anything to do.
        try
        {
            var effectiveConfig = config ?? new JuceEngineConfig();
            effectiveConfig.Validate();

            _log = (ILogger?)logger ?? (effectiveConfig.EnableConsoleLog
                ? JuceConsoleLogger.Instance
                : NullLogger.Instance);

            VerifyAbi();

            RegisterForNativeLog(this);

            var cfg = effectiveConfig.ToNative();
            nint handle;
            try
            {
                // dca_engine_create2 (ABI v41): the Null-device options travel with
                // THIS engine instead of the process environment (DotCharter #85).
                Check(NativeMethods.EngineCreate2(in cfg, effectiveConfig.CreateFlags(), out handle), "engine_create");
            }
            catch
            {
                UnregisterFromNativeLog(this);
                throw;
            }
            _owner = new NativeHandleOwner(handle);
            // dca_engine_create OPENS AND STARTS THE DEVICE when it is given a
            // config, and it always is (cfg is built unconditionally above). So
            // the engine is started from the moment the constructor returns, and
            // `_started` has to say so — it did not, and `Stop()` early-outs on
            // `if (!_started)`, which made every `new JuceAudioEngine(); Stop();`
            // a NO-OP that left the device wide open.
            //
            // That is not a cosmetic flag: EIGHT call sites do exactly that pair
            // and every one of them comments that it is deliberately device-less
            // — DotCharter's TakeReampService ("never a device — the offline path
            // must be the only processor"), its four offline DSP test fixtures,
            // and four here. All of them were rendering against a live callback,
            // contending the chain's process barrier once per 512 frames, and
            // holding the host's sound card (typically EXCLUSIVE on ASIO) for the
            // length of a re-amp. Found by dca_engine_dsp_process_buffer's new
            // open-device rejection: the guard fired everywhere, which is how a
            // stale flag with no test finally became visible.
            _started = true;
            _log.LogInformation("[native] JuceAudioEngine created. Driver={Driver}, sr={SR}, buf={Buf}",
                effectiveConfig.PreferredDriver, effectiveConfig.SampleRate, effectiveConfig.BufferFrames);
        }
        catch
        {
            GC.SuppressFinalize(this);
            throw;
        }
    }

    /// <summary>Pin the handle for one native call that must fail loudly when
    /// the engine is gone. Pair with <see cref="Release"/> in a finally.</summary>
    internal nint Acquire()
        => (_owner ?? throw new ObjectDisposedException(nameof(JuceAudioEngine)))
            .Acquire(nameof(JuceAudioEngine));

    /// <summary>Pin the handle for one fire-and-forget native call that should
    /// silently no-op when the engine is gone. Returns <see cref="nint.Zero"/>
    /// when unavailable — call <see cref="Release"/> ONLY when it does not.</summary>
    internal nint TryAcquire() => _owner?.TryAcquire() ?? nint.Zero;

    internal void Release() => _owner?.Release();

    /// <summary>
    /// Register a child wrapper so <see cref="Dispose()" /> drains and destroys
    /// it BEFORE <c>dca_engine_destroy</c>. Returns the child so factories can
    /// tail-call it. Callers must already hold the engine handle pinned
    /// (<see cref="Acquire" />), which is what makes "no child is registered
    /// after the engine handle is claimed" true.
    /// </summary>
    private T Track<T>(T child) where T : IDisposable, INativeChild
    {
        lock (_childrenGate)
        {
            if (_children is null)
            {
                // Dispose won the race between our native open and its claim of
                // the child list. Destroy what we just opened rather than
                // leaking it, and report the engine as gone.
                try { child.Dispose(); } catch { /* best effort */ }
                throw new ObjectDisposedException(nameof(JuceAudioEngine));
            }

            // Compact completed native destructions. A collected wrapper whose
            // destroy failed must retain its token to protect the parent.
            for (var i = _children.Count - 1; i >= 0; i--)
            {
                if (_children[i].Destruction.IsComplete) _children.RemoveAt(i);
            }

            _children.Add((new WeakReference<IDisposable>(child, trackResurrection: true), child.Destruction));
        }

        return child;
    }

    /// <summary>
    /// Drain + destroy every live child wrapper. Each child's own
    /// <c>NativeHandleOwner</c> does the waiting, so a position poll inside
    /// <c>dca_source_*</c> is finished before its source is destroyed, and every
    /// source is destroyed before the engine that owns its transport.
    /// <para>The finalizer checks completion tokens only; it cannot wait for
    /// another finalizer on the same finalizer thread.</para>
    /// </summary>
    /// <summary>
    ///     How long <see cref="DisposeChildren" /> waits for ONE child's native
    ///     destruction to complete, and it depends on whether anything can still
    ///     make it happen (W4-M4).
    ///     <list type="bullet">
    ///         <item>
    ///             <b>Wrapper still reachable</b> — we just called its
    ///             <c>Dispose</c>, so the destroy is running on THIS thread's
    ///             behalf and the wait is the same 60 s backstop the handle drain
    ///             uses: it has to clear a modal ASIO panel or a cold VST3 scan.
    ///         </item>
    ///         <item>
    ///             <b>Wrapper COLLECTED but not yet finalized</b> — nothing here
    ///             calls <c>Destruction.Run</c>, the state is still "unclaimed",
    ///             and the full deadline therefore parks the disposing thread
    ///             (normally the host's UI thread) for 60 s PER SUCH CHILD, in
    ///             series. <c>Track</c>'s compaction only drops entries that are
    ///             already complete, so such a child is guaranteed to be in the
    ///             list. A short grace — the same 2 s wave-2 #22 gave the worker
    ///             join, for the case where the finalizer is already in flight —
    ///             and then the existing "child destruction incomplete ⇒ LEAK the
    ///             engine" branch, which the queued finalizer resolves anyway.
    ///         </item>
    ///     </list>
    /// </summary>
    internal static int ChildDestructionDeadlineMs(bool wrapperReachable)
        => wrapperReachable
            ? NativeHandleOwner.DefaultDrainTimeoutMs
            : EngineWorker.DisposeJoinTimeoutMs;

    private bool DisposeChildren(bool disposeWrappers = true)
    {
        List<(WeakReference<IDisposable> Child, NativeDestruction Destruction)>? children;
        lock (_childrenGate)
        {
            children = _children;
            _children = null;
            _midiInputs.Clear(); // the weak walk below closes them; nothing roots them past here
            _midiOutputs.Clear(); // ditto (ABI v34)
        }

        if (children is null) return true;
        var complete = true;

        // Reverse order: children opened later may be layered on earlier ones
        // (a plugin attached to an input the source feeds), so tear down LIFO.
        for (var i = children.Count - 1; i >= 0; i--)
        {
            var entry = children[i];
            var reachable = entry.Child.TryGetTarget(out var child);
            try
            {
                if (disposeWrappers && reachable) child!.Dispose();
            }
            catch (Exception ex)
            {
                JuceLog.Emit(JuceLogLevel.Warn,
                    $"Disposing a child of the engine threw: {ex.GetType().Name}: {ex.Message}");
            }
            complete &= disposeWrappers
                ? entry.Destruction.Wait(ChildDestructionDeadlineMs(reachable))
                : entry.Destruction.IsComplete;
        }
        return complete;
    }

    /// <summary>
    /// The loaded native library's version string (<c>dca_version_string</c>), decoded as
    /// UTF-8 like every other string crossing this ABI. Empty when the DLL returns
    /// nothing. Static: it describes the DLL, not an engine instance, and is kept in step
    /// with the package version by <c>PackagingLintTests</c>.
    /// </summary>
    public static string NativeVersion
    {
        get
        {
            var ptr = NativeMethods.VersionStringRaw();
            // UTF-8, like every other string crossing this ABI — PtrToStringAnsi
            // decodes with the SYSTEM CODEPAGE, which is the 2026-07-16 audit #3
            // bug in miniature. Today's native literal is pure ASCII so the two
            // agree byte for byte; that is exactly why it survived.
            return ptr == nint.Zero ? string.Empty : Marshal.PtrToStringUTF8(ptr) ?? string.Empty;
        }
    }

    /// <summary>
    /// The sample rate in Hz the device is actually running at
    /// (<c>dca_engine_get_sample_rate</c>) — which is not necessarily the rate that was
    /// requested. 0 before <see cref="Start"/> has opened a device. Re-read it after any
    /// device change: a rebuild re-derives everything rate-dependent.
    /// </summary>
    public int SampleRate
    {
        get
        {
            var h = Acquire();
            try
            {
                Check(NativeMethods.EngineGetSampleRate(h, out var sr), "engine_get_sample_rate");
                return sr;
            }
            finally { Release(); }
        }
    }

    /// <summary>
    /// The device's actual callback size in sample frames
    /// (<c>dca_engine_get_buffer_frames</c>), which drivers routinely round away from what
    /// was asked for. 0 before a device is open.
    /// </summary>
    public int BufferFrames
    {
        get
        {
            var h = Acquire();
            try
            {
                Check(NativeMethods.EngineGetBufferFrames(h, out var frames), "engine_get_buffer_frames");
                return frames;
            }
            finally { Release(); }
        }
    }

    /// <summary>
    /// Output latency reported by the active device, in samples at the device
    /// sample rate. Includes the device buffer plus driver/hardware delay.
    /// Returns 0 when no device is open or when running against an older
    /// native DLL that does not export the entry point (ABI &lt; 7).
    /// </summary>
    public int OutputLatencySamples
    {
        get
        {
            var h = Acquire();
            try
            {
                // No EntryPointNotFoundException catch here, and there must not
                // be one: VerifyAbi compares DCA_ABI_VERSION for EQUALITY at
                // open, so a DLL old enough to be missing this export cannot
                // open an engine to call it on. A catch that cannot fire reads
                // as graceful degradation on older runtimes — there is no such
                // thing on this boundary, and saying otherwise stops the next
                // reader looking.
                Check(NativeMethods.EngineGetOutputLatencySamples(h, out var s), "engine_get_output_latency_samples");
                return s;
            }
            finally { Release(); }
        }
    }

    /// <summary>
    ///     Start the audio device. The constructor already did this — the engine
    ///     is running from the moment it returns — so this is only for a device
    ///     that <see cref="Stop" /> parked.
    ///     <para>
    ///         <b>A stopped engine cannot be restarted</b>: this throws
    ///         <see cref="InvalidOperationException" /> after <see cref="Stop" />,
    ///         and the native side refuses too (#27). The native stop path detaches
    ///         every source from the mixer for good, so a restart used to run a live
    ///         device with silence on it while reporting success. To get audio back,
    ///         dispose it and create a new one; to change device WITHOUT stopping, use
    ///         <see cref="Reconfigure" />.
    ///     </para>
    /// </summary>
    /// <exception cref="InvalidOperationException">The engine was stopped.</exception>
    public void Start()
    {
        var h = Acquire();
        try
        {
            if (_started) return;
            if (_stoppedOnce)
            {
                throw new InvalidOperationException(
                    "A stopped JuceAudioEngine cannot be restarted: Stop() detaches every source and the audio "
                    + "callback for good. Dispose it and create a new one (to change device without stopping, "
                    + "use Reconfigure).");
            }

            Check(NativeMethods.EngineStart(h), "engine_start");
            _started = true;
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Stop the audio device. This is the right call before an offline render
    ///     (<c>dca_engine_dsp_process_buffer*</c> refuses to run while a device is
    ///     open) and before handing the sound card back to another app.
    ///     <para>
    ///         <b>It is not a pause, and it is final.</b> Stopping DETACHES every
    ///         source from the mixer for good, so <see cref="Start" /> refuses
    ///         afterwards. Use <see cref="JuceAudioSource.Pause" /> to pause.
    ///     </para>
    /// </summary>
    public void Stop()
    {
        if (!_started) return;
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            Check(NativeMethods.EngineStop(h), "engine_stop");
            _started = false;
            _stoppedOnce = true;
        }
        finally { Release(); }
    }

    /// <summary>
    /// In-place reconfigure of the active audio device. Wraps JUCE's
    /// <c>AudioDeviceManager::setAudioDeviceSetup</c> so settings changes that
    /// only touch sample rate, buffer frames or device selection do NOT have
    /// to dispose and recreate the engine. Loaded VST plugins, transport
    /// sources, mixer state and stem mappings are preserved.
    ///
    /// Pass <c>0</c> for <paramref name="sampleRate"/> or
    /// <paramref name="bufferFrames"/> to keep the current value, and
    /// <c>null</c>/empty for either device id to keep the current device.
    ///
    /// Changing the driver TYPE (e.g. WASAPI ↔ ASIO) is NOT supported by this
    /// path — dispose the engine and create a new one for that. On failure
    /// the engine is rolled back to its previous working configuration and
    /// a <see cref="JuceAudioException"/> is thrown.
    ///
    /// Additive ABI v8+. NOT a version an older runtime can decline: the
    /// engine's ABI check is an EQUALITY comparison at open, so a DLL missing
    /// this export never gets an engine to call it on.
    /// </summary>
    public void Reconfigure(int sampleRate, int bufferFrames, string? outputDeviceId, string? inputDeviceId)
    {
        var h = Acquire();
        try
        {
            Check(
                NativeMethods.EngineReconfigure(h, sampleRate, bufferFrames,
                    string.IsNullOrEmpty(outputDeviceId) ? null : outputDeviceId,
                    string.IsNullOrEmpty(inputDeviceId)  ? null : inputDeviceId),
                "engine_reconfigure");
        }
        finally { Release(); }
    }

    /// <summary>
    /// In-place host swap (ABI v12). Switch the audio host to
    /// <paramref name="typeName"/> (e.g. <c>"ASIO"</c>,
    /// <c>"Windows Audio (Exclusive Mode)"</c>) and reopen with the supplied
    /// device IDs / channel masks — without destroying the engine. Plugins,
    /// transports and mixer state are preserved. Pass an empty
    /// <paramref name="typeName"/> to keep the current host. Empty device IDs
    /// fall back to the host's default. Channel masks of <c>0</c> let JUCE
    /// pick (preserves current behaviour); a non-zero mask pins exactly the
    /// selected channels (see <see cref="ChannelMask"/>).
    /// </summary>
    public void SetDeviceType(string? typeName,
                              string? outputDeviceId  = null,
                              string? inputDeviceId   = null,
                              uint    outputChannelMask = 0,
                              uint    inputChannelMask  = 0)
    {
        var h = Acquire();
        try
        {
            Check(
                NativeMethods.EngineSetDeviceType(h,
                    string.IsNullOrEmpty(typeName)       ? null : typeName,
                    string.IsNullOrEmpty(outputDeviceId) ? null : outputDeviceId,
                    string.IsNullOrEmpty(inputDeviceId)  ? null : inputDeviceId,
                    outputChannelMask,
                    inputChannelMask),
                "engine_set_device_type");
        }
        finally { Release(); }
    }

    /// <summary>
    /// Set the linear gain of the live-input monitor path
    /// (<c>dca_engine_set_monitor_gain</c>) — how loud the player hears their own guitar.
    /// The engine RAMPS the change across the block, so a per-tick slider does not zipper.
    /// A no-op on a disposed engine rather than a throw, because this is called from UI
    /// ticks.
    /// </summary>
    /// <param name="gain">Linear gain; 1.0 is unity, 0 is silent.</param>
    public void SetMonitorGain(float gain)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetMonitorGain(h, gain), "engine_set_monitor_gain"); }
        finally { Release(); }
    }

    private float _stemGroupTempo = 1.0f;
    private float _stemGroupPitchSemitones;

    /// <summary>
    ///     Tempo factor applied by the shared stem-group stretcher (1.0 = normal).
    ///     Lazily-created sub-bus: the stretcher only exists while at least one
    ///     source has <see cref="JuceAudioSource.StemGrouped" /> set; until then
    ///     the setter just caches the value for when the first source joins.
    ///     <para>
    ///         <b>Contract (one of three, picked deliberately):</b> this is a
    ///         FIRE-AND-FORGET setter. It no-ops when the engine has been disposed,
    ///         it never throws on a native failure — it reports one through the
    ///         engine's <c>ILogger</c> and <see cref="JuceLog" /> — and the getter
    ///         always returns the value last REQUESTED, including after a failure
    ///         and after dispose. It used to <c>Check</c> (throw) on failure while
    ///         silently no-opping when disposed, which is two different contracts
    ///         in one property and is why every DotCharter call site wraps it in a
    ///         bare <c>try { } catch { }</c>.
    ///     </para>
    /// </summary>
    public float StemGroupTempo
    {
        get => _stemGroupTempo;
        set
        {
            _stemGroupTempo = value;
            var h = TryAcquire();
            if (h == nint.Zero) return;
            try
            {
                WarnIfFailed(NativeMethods.EngineSetStemGroupTempo(h, value), "engine_set_stem_group_tempo");
            }
            finally { Release(); }
        }
    }

    /// <summary>
    ///     Pitch shift in semitones for the shared stem-group stretcher. Same
    ///     fire-and-forget contract as <see cref="StemGroupTempo" /> — read that
    ///     one.
    /// </summary>
    public float StemGroupPitchSemitones
    {
        get => _stemGroupPitchSemitones;
        set
        {
            _stemGroupPitchSemitones = value;
            var h = TryAcquire();
            if (h == nint.Zero) return;
            try
            {
                WarnIfFailed(NativeMethods.EngineSetStemGroupPitch(h, value), "engine_set_stem_group_pitch");
            }
            finally { Release(); }
        }
    }

    /// <summary>
    /// Set the linear input trim applied to the captured signal before the rig
    /// (<c>dca_engine_set_input_gain</c>). Ramped across the block like every other
    /// user-facing gain. A no-op on a disposed engine.
    /// </summary>
    /// <param name="gain">Linear gain; 1.0 is unity.</param>
    public void SetInputGain(float gain)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetInputGain(h, gain), "engine_set_input_gain"); }
        finally { Release(); }
    }

    /// <summary>
    ///     Digital drop-tune pedal: shifts the live input by
    ///     <paramref name="semitones" /> (negative = drop) before the capture
    ///     ring and the input VST chain, so note detection, plugins and
    ///     monitoring all hear the shifted signal. 0 = true bypass with no
    ///     added latency.
    ///     <para>
    ///         DOWN-ONLY, and the shifter is CSVD
    ///         (<c>src/dsp/cores/dcdsp_droptune.h</c>, ABI v25) — not the Rubber
    ///         Band live shifter this used to say. Attack latency is ~1.5–4 ms and
    ///         is the SAME in all three <see cref="JuceDropTuneMode" />s; the "adds
    ///         ~50 ms" note here described the phase-vocoder engine that was
    ///         removed at v25, and 50 ms of latency on a live guitar is the
    ///         difference between playable and not.
    ///     </para>
    /// </summary>
    public void SetInputPitchShift(float semitones)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetInputPitchShift(h, semitones), "engine_set_input_pitch_shift"); }
        finally { Release(); }
    }

    /// <summary>
    ///     Selects the drop-tune pedal's SPLICE-LAG CEILING —
    ///     <see cref="JuceDropTuneMode.Tight" /> (10 ms),
    ///     <see cref="JuceDropTuneMode.Balanced" /> (20 ms, the default) or
    ///     <see cref="JuceDropTuneMode.Smooth" /> (35 ms, fewest splices, best on
    ///     sustained chords). Applied atomically on the audio thread at the next
    ///     callback. A value outside 0..2 is REJECTED by the engine, not clamped.
    ///     <para>
    ///         <b>The meaning of this argument changed at ABI v25.</b> It used to
    ///         pick a shifter ENGINE (0 = Rubber Band, 1 = OLA); both are gone, the
    ///         shifter is CSVD, and this now picks a lag floor within it. Attack
    ///         latency (~1.5–4 ms) is set by the transient anchor and does not vary
    ///         between the three. Prefer the
    ///         <see cref="SetInputPitchShiftMode(JuceDropTuneMode)" /> overload; the
    ///         <see cref="int" /> one is kept for callers that persist the ordinal.
    ///     </para>
    /// </summary>
    public void SetInputPitchShiftMode(int mode)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetInputPitchShiftMode(h, mode), "engine_set_input_pitch_shift_mode"); }
        finally { Release(); }
    }

    /// <summary>Typed twin of <see cref="SetInputPitchShiftMode(int)" />: seat 0's
    /// drop-tune splice-lag ceiling.</summary>
    public void SetInputPitchShiftMode(JuceDropTuneMode mode)
        => SetInputPitchShiftMode((int)mode);

    /// <summary>Typed twin of <see cref="SetInputPitchShiftMode(int, int)" />: ONE
    /// seat's drop-tune splice-lag ceiling (ABI v31).</summary>
    public void SetInputPitchShiftMode(int player, JuceDropTuneMode mode)
        => SetInputPitchShiftMode(player, (int)mode);

    /// <summary>
    ///     ABI v31: ONE SEAT's drop-tune pedal. Until v31 the engine held a
    ///     single shifter and ran it on the raw device input BEFORE the
    ///     per-player split, so a couch with two guitars in different tunings
    ///     could not use it: dropping one dropped both, and the seat that did not
    ///     ask for it was detuned with nothing on screen saying so.
    ///     <para>
    ///         <see cref="SetInputPitchShift(float)" /> is this with
    ///         <paramref name="player" /> 0 — the same relationship
    ///         <c>SetInputCaptureChannel</c> has with
    ///         <c>SetPlayerInputChannel</c>. A seat past the native ceiling is
    ///         REJECTED, never clamped: clamping would detune somebody else.
    ///     </para>
    /// </summary>
    public void SetInputPitchShift(int player, float semitones)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            WarnIfFailed(NativeMethods.EngineSetPlayerInputPitchShift(h, player, semitones),
                "engine_set_player_input_pitch_shift");
        }
        finally { Release(); }
    }

    /// <summary>ABI v31: one seat's drop-tune splice-lag ceiling
    /// (0 = Tight / 1 = Balanced / 2 = Smooth — see
    /// <see cref="JuceDropTuneMode" />), the per-seat twin of
    /// <see cref="SetInputPitchShiftMode(int)" />. Out of range is rejected,
    /// never clamped.</summary>
    public void SetInputPitchShiftMode(int player, int mode)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            WarnIfFailed(NativeMethods.EngineSetPlayerInputPitchShiftMode(h, player, mode),
                "engine_set_player_input_pitch_shift_mode");
        }
        finally { Release(); }
    }

    /// <summary>
    /// Set the master output volume (<c>dca_engine_set_master_volume</c>), applied after
    /// the mix and ramped across the block. A no-op on a disposed engine.
    /// </summary>
    /// <param name="gain">Linear gain; 1.0 is unity.</param>
    public void SetMasterGain(float gain)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetMasterVolume(h, gain), "engine_set_master_volume"); }
        finally { Release(); }
    }

    // ---- Master-bus ceiling (ABI v34) -------------------------------------

    /// <summary>
    ///     Configure the look-ahead soft limiter over the FINAL device buffer.
    ///     <b>It is OFF by default and the host must opt in</b> (it was ON by
    ///     default in ABI v34; −0.3 dBFS and 80 ms remain the defaults for the
    ///     ceiling and release once it is enabled). Default-ON contradicted the
    ///     engine's "everything is OFF at engine start" rule: the live monitor
    ///     path runs through this stage, so it cost every guitarist 1 ms of
    ///     latency on their own playing for a feature they had not asked for.
    ///     <para>
    ///         The engine's output is the sum of the song bus, every seat's
    ///         monitor mix, the click bus and both instrument legs; nothing
    ///         bounded that sum before, so a loud rig over a loud backing track
    ///         could hand the driver samples past ±1.0 — and the streaming
    ///         mirror carries FLOAT, so they survived into OBS / Discord's
    ///         encoder and wrapped there.
    ///     </para>
    ///     <para>
    ///         <b>Switched off it is not in the path at all</b> — the output is
    ///         bit-for-bit the pre-v34 engine and
    ///         <see cref="MasterLimiterLatencySamples" /> reports 0. The cost of
    ///         that is a one-block discontinuity when a user toggles it
    ///         mid-song.
    ///     </para>
    ///     <para>
    ///         <b><see cref="GetMasterLimiter" /> is the read-back, and it
    ///         returns what the engine HOLDS — post-clamp — not what was
    ///         asked for.</b> The engine clamps <paramref name="ceilingDb" />
    ///         into [−24, 0] dBFS and <paramref name="releaseMs" /> into
    ///         [1, 1000] ms and stores the clamped pair; the getter derives its
    ///         dB from that stored value, so a request of +5 dBFS reads back as
    ///         0. A settings row must therefore render from
    ///         <c>GetMasterLimiter()</c> rather than from the number it wrote
    ///         to its own config — otherwise the UI shows a ceiling the audio
    ///         path is not using, which is the same "a band the engine silently
    ///         moved is a band whose drawn curve and audible curve disagree"
    ///         failure the ABI v29 EQ ranges exist to prevent. A NON-FINITE
    ///         pair is the one case that is rejected rather than clamped, and
    ///         then the read-back still shows the PREVIOUS configuration,
    ///         because nothing moved.
    ///     </para>
    /// </summary>
    /// <param name="enabled">False passes the bus through unlimited.</param>
    /// <param name="ceilingDb">Clamped to [−24, 0] dBFS.</param>
    /// <param name="releaseMs">Clamped to [1, 1000] ms.</param>
    /// <exception cref="JuceAudioException">
    ///     A non-finite ceiling or release is REJECTED, not clamped: every
    ///     comparison a clamp is built from is false for a NaN, and a NaN
    ///     ceiling is permanent silence.
    /// </exception>
    public void SetMasterLimiter(bool enabled, float ceilingDb = -0.3f, float releaseMs = 80f)
    {
        // TryAcquire, like every other master-bus SETTER (SetMasterGain,
        // SetMonitorGain, SetInputGain, SetHighPass/LowPass/BandPass): a no-op
        // on a disposed engine rather than a throw, because this is a UI toggle
        // in the same tick loop. The GETTERS keep Acquire — reporting a
        // plausible "off, −0.3 dB" for a corpse is a different and worse answer.
        //
        // The parity with those siblings is the DISPOSED half ONLY, and this
        // comment claimed both until W4-C6. They use WarnIfFailed; this one
        // Checks, deliberately: a device-level gain that native refuses is a
        // condition a UI tick should log and survive, while a NON-FINITE ceiling
        // or release is a caller BUG — everything else here is clamped, so the
        // only way to be rejected is to have passed a NaN — and one of those
        // must not be swallowed into a limiter the host believes it configured.
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            Check(NativeMethods.EngineSetMasterLimiter(h, enabled ? 1 : 0, ceilingDb, releaseMs),
                "engine_set_master_limiter");
        }
        finally { Release(); }
    }

    /// <summary>What the engine actually holds, post-clamp.</summary>
    /// <returns><c>Enabled</c> — whether the stage is in the path at all;
    /// <c>CeilingDb</c> — the ceiling in dBFS; <c>ReleaseMs</c> — the release in
    /// milliseconds. Both floats are the engine's post-clamp values, not the ones
    /// that were passed in.</returns>
    /// <exception cref="ObjectDisposedException">The engine has been disposed. A
    /// getter throws rather than reporting a plausible default.</exception>
    /// <exception cref="JuceAudioException">The native call failed.</exception>
    public (bool Enabled, float CeilingDb, float ReleaseMs) GetMasterLimiter()
    {
        var h = Acquire();
        try
        {
            Check(NativeMethods.EngineGetMasterLimiter(h, out var enabled, out var ceiling, out var release),
                "engine_get_master_limiter");
            return (enabled != 0, ceiling, release);
        }
        finally { Release(); }
    }

    /// <summary>
    ///     POLL-ONLY, exactly like the ABI v30 node meters: a decaying hold
    ///     (0.25 s) of the deepest gain reduction the limiter applied, as a
    ///     POSITIVE number of dB. 0 while it is idle or disabled. Nothing is
    ///     queued and no callback crosses the ABI, so a caller that never reads
    ///     this pays nothing for it.
    /// </summary>
    public float MasterLimiterReductionDb
    {
        get
        {
            var h = TryAcquire();
            if (h == nint.Zero) return 0f;
            try
            {
                return NativeMethods.EngineGetMasterLimiterReductionDb(h, out var db)
                       == NativeMethods.DcaStatus.Ok ? db : 0f;
            }
            finally { Release(); }
        }
    }

    /// <summary>
    ///     The limiter's own contribution to <see cref="OutputLatencySamples" />
    ///     (which already includes it). 0 with no device open, and <b>0 while
    ///     the limiter is switched off</b> — while it is off it is not in the
    ///     path.
    ///     <para>
    ///         So a caller that aligns scheduled clicks against
    ///         <see cref="OutputLatencySamples" /> must re-read it after a
    ///         toggle rather than cache it for the session. Keeping the delay
    ///         line running while the limiter is off so this number never moved
    ///         was tried and rejected: it makes every user pay 1 ms on their own
    ///         monitored playing for a feature they switched off.
    ///     </para>
    /// </summary>
    public int MasterLimiterLatencySamples
    {
        get
        {
            var h = Acquire();
            try
            {
                Check(NativeMethods.EngineGetMasterLimiterLatencySamples(h, out var s),
                    "engine_get_master_limiter_latency_samples");
                return s;
            }
            finally { Release(); }
        }
    }

    /// <summary>
    /// Configure the master bus's simple high-pass filter
    /// (<c>dca_engine_set_high_pass</c>). Independent of the six-band parametric EQ
    /// (ABI v29). A no-op on a disposed engine.
    /// </summary>
    /// <param name="freqHz">Corner frequency in Hz; 0 disables the filter.</param>
    /// <param name="q">Resonance; 0.707 is Butterworth.</param>
    public void SetHighPass(float freqHz, float q)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetHighPass(h, freqHz, q), "engine_set_high_pass"); }
        finally { Release(); }
    }

    /// <summary>
    /// Configure the master bus's simple low-pass filter
    /// (<c>dca_engine_set_low_pass</c>). See <see cref="SetHighPass"/>.
    /// </summary>
    /// <param name="freqHz">Corner frequency in Hz; 0 disables the filter.</param>
    /// <param name="q">Resonance; 0.707 is Butterworth.</param>
    public void SetLowPass(float freqHz, float q)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetLowPass(h, freqHz, q), "engine_set_low_pass"); }
        finally { Release(); }
    }

    /// <summary>
    /// Configure the master bus's simple band-pass filter
    /// (<c>dca_engine_set_band_pass</c>). See <see cref="SetHighPass"/>.
    /// </summary>
    /// <param name="freqHz">Centre frequency in Hz; 0 disables the filter.</param>
    /// <param name="q">Bandwidth; higher is narrower.</param>
    public void SetBandPass(float freqHz, float q)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetBandPass(h, freqHz, q), "engine_set_band_pass"); }
        finally { Release(); }
    }

    // ---- Parametric EQ (ABI v29) ------------------------------------------
    // Six typed bands over the master bus and/or the live guitar. Marshalling
    // and lifetime only, like the rest of this wrapper: the RBJ coefficients are
    // computed native-side on the message thread, so a slider drag never touches
    // the audio callback with anything but five floats.

    /// <summary>How many bands the engine's EQ section has. Fixed by the ABI
    /// (<c>DCA_EQ_BAND_COUNT</c>).</summary>
    public const int EqBandCount = 6;

    /// <summary>Smallest buffer <see cref="ReadEqSpectrum" /> accepts. Below this
    /// the log-spaced 20 Hz … 20 kHz sweep has too few points to draw a curve
    /// from, so the call is refused rather than answering with a shape nobody
    /// can read.</summary>
    public const int MinEqSpectrumBins = 8;

    /// <summary>Largest buffer <see cref="ReadEqSpectrum" /> accepts — the
    /// engine's own analyser resolution. Asking for more does not produce
    /// more.</summary>
    public const int MaxEqSpectrumBins = 512;

    /// <summary>Configure one band. <paramref name="type" /> is
    /// <c>dca_eq_band_type</c>; <paramref name="gainDb" /> is ignored by the
    /// shapes that have no gain.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="index" /> is
    /// outside <c>[0, <see cref="EqBandCount" />)</c>.</exception>
    public void SetEqBand(int index, int type, float freqHz, float q, float gainDb, bool enabled)
    {
        // Every SEAT index in this file throws through ValidatePlayer, and that
        // helper's own doc is an essay on why a swallowed index is the wrong
        // answer — it "reads exactly like the ring is empty, i.e. a guitar that
        // is plugged in and silent". A band index is the same shape one type
        // over: index == 7 was reported only as a LogWarning, five lines under
        // the constant that bounds it (W4-C4).
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(index, EqBandCount);

        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            WarnIfFailed(NativeMethods.EngineSetEqBand(h, index, type, freqHz, q, gainDb, enabled ? 1 : 0),
                "engine_set_eq_band");
        }
        finally { Release(); }
    }

    /// <summary>Typed twin of <see cref="SetEqBand(int, int, float, float, float, bool)" />.
    /// <paramref name="gainDb" /> is ignored by <see cref="JuceEqBandType.HighPass" />,
    /// <see cref="JuceEqBandType.LowPass" /> and <see cref="JuceEqBandType.Notch" />.</summary>
    public void SetEqBand(int index, JuceEqBandType type, float freqHz, float q, float gainDb, bool enabled)
        => SetEqBand(index, (int)type, freqHz, q, gainDb, enabled);

    /// <summary>Master power for the whole six-band section.</summary>
    public void SetEqEnabled(bool enabled)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetEqEnabled(h, enabled ? 1 : 0), "engine_set_eq_enabled"); }
        finally { Release(); }
    }

    /// <summary>Solo one band, or pass a negative index for "no solo".</summary>
    public void SetEqSolo(int bandIndex)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetEqSolo(h, bandIndex), "engine_set_eq_solo"); }
        finally { Release(); }
    }

    /// <summary>Which bus the bands run on (<c>dca_eq_route</c>).</summary>
    public void SetEqRoute(int route)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetEqRoute(h, route), "engine_set_eq_route"); }
        finally { Release(); }
    }

    /// <summary>Typed twin of <see cref="SetEqRoute(int)" />.</summary>
    public void SetEqRoute(JuceEqRoute route) => SetEqRoute((int)route);

    /// <summary>Output trim in dB, applied after the six bands.</summary>
    public void SetEqTrim(float trimDb)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetEqTrim(h, trimDb), "engine_set_eq_trim"); }
        finally { Release(); }
    }

    /// <summary>
    ///     How many bands the ENGINE is running right now — master power and
    ///     solo already folded in.
    ///     <para>
    ///         Read rather than derived on purpose: a second count computed in
    ///         managed code is a second source of truth, and the badge that shows
    ///         it would then drift from the DSP the moment either side changed.
    ///     </para>
    /// </summary>
    /// <returns>The count, or 0 when the engine has gone away.</returns>
    public int GetEqActiveCount()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try
        {
            return NativeMethods.EngineGetEqActiveCount(h, out var count) == NativeMethods.DcaStatus.Ok ? count : 0;
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Magnitude spectrum of the bus the EQ is routed to, sampled BEFORE the
    ///     bands, log-spaced 20 Hz … 20 kHz in dBFS.
    /// </summary>
    /// <param name="destDb">Filled with <paramref name="destDb" />.Length values;
    ///     its length must be in
    ///     <c>[<see cref="MinEqSpectrumBins" />, <see cref="MaxEqSpectrumBins" />]</c>.</param>
    /// <returns>How many bins were written; 0 when the engine is gone.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="destDb" /> is
    ///     shorter than <see cref="MinEqSpectrumBins" /> or longer than
    ///     <see cref="MaxEqSpectrumBins" />. It used to return 0, which a caller
    ///     cannot tell from "the engine had nothing" — and the two bounds were
    ///     unnamed magic numbers (W4-C4).</exception>
    public int ReadEqSpectrum(float[] destDb)
    {
        ArgumentNullException.ThrowIfNull(destDb);
        ArgumentOutOfRangeException.ThrowIfLessThan(destDb.Length, MinEqSpectrumBins, nameof(destDb));
        ArgumentOutOfRangeException.ThrowIfGreaterThan(destDb.Length, MaxEqSpectrumBins, nameof(destDb));

        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try
        {
            return NativeMethods.EngineReadEqSpectrum(h, destDb, destDb.Length, out var written) == NativeMethods.DcaStatus.Ok
                ? written
                : 0;
        }
        finally { Release(); }
    }

    /// <summary>Open / close the streaming-mirror WASAPI output. Pass
    /// <c>null</c> or empty to disable. Returns <c>true</c> when the call
    /// succeeded (or successfully closed the mirror).</summary>
    public bool SetMirrorDevice(string? deviceId)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            var status = NativeMethods.EngineSetMirrorDevice(h, deviceId);
            if (status != NativeMethods.DcaStatus.Ok)
                _log.LogWarning("[native] engine_set_mirror_device('{Dev}') returned {Status}", deviceId, status);
            return status == NativeMethods.DcaStatus.Ok;
        }
        finally { Release(); }
    }

    /// <summary>Friendly alias of <see cref="SetMirrorDevice"/>.
    /// Pass <c>null</c> to disable the mirror.</summary>
    /// <remarks>Two names for one call is one name too many — no caller in this
    /// repo or in DotCharter uses this one (grep, 2026-09-10). The two aliases
    /// that ARE used, <c>SetMasterGain</c> and <c>EnumerateMidiInputs</c>, are
    /// deliberately left unmarked.</remarks>
    [Obsolete("Use SetMirrorDevice(string?) — this is the same call under a second name.")]
    public bool SetMirrorOutput(string? deviceId) => SetMirrorDevice(deviceId);

    /// <summary>Play a synthesised click on the master output.
    /// 0 = beat tick, 1 = downbeat, 2 = clap. The voice starts at the head of
    /// the next audio block, so it jitters by up to one block and is heard one
    /// full output latency after this call. Use <see cref="ScheduleClick(int, long)"/>
    /// when the click has to land on a known point of the output timeline.</summary>
    public void PlayClick(int clickType)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EnginePlayClick(h, clickType), "engine_play_click"); }
        finally { Release(); }
    }

    /// <summary>Typed twin of <see cref="PlayClick(int)" />.</summary>
    public void PlayClick(JuceClickType clickType) => PlayClick((int)clickType);

    /// <summary>Typed twin of <see cref="ScheduleClick(int, long)" />.</summary>
    public void ScheduleClick(JuceClickType clickType, long renderFrame)
        => ScheduleClick((int)clickType, renderFrame);

    /// <summary>ABI v26. Atomically samples the engine's output render clock:
    /// {frames rendered by the device callback so far, host high-resolution
    /// ticks at that same block boundary}. The frame count is the index of the
    /// NEXT sample the device will render; the tick timebase equals
    /// <see cref="System.Diagnostics.Stopwatch.GetTimestamp"/> (QPC on Windows).
    ///
    /// <para>The pair is published immediately after the transport pull, so this
    /// counter and <c>JuceAudioSource.AudiblePosition</c> describe the same
    /// output sample. Read them back-to-back and re-read this counter to confirm
    /// no audio block landed in between, and you have an exact
    /// {render frame ↔ song time} anchor.</para>
    ///
    /// Returns false until the device has rendered its first block.</summary>
    public bool TryGetRenderClock(out long renderFrames, out long hostTicks)
    {
        renderFrames = 0;
        hostTicks = 0;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            return NativeMethods.EngineGetRenderClock(h, out renderFrames, out hostTicks)
                   == NativeMethods.DcaStatus.Ok;
        }
        finally { Release(); }
    }

    /// <summary>
    ///     ABI v30. Snapshots the engine's own dropout counters. Safe from any
    ///     thread and never blocks the audio thread.
    ///
    ///     <para>
    ///         These are what the audio callback actually observed, so unlike a
    ///         managed poll of the render clock they see a single sub-poll glitch
    ///         — and they SEPARATE the causes. <see cref="XrunStats.LateCallbacks" />
    ///         is the audible event; <see cref="XrunStats.DeadlineOverruns" /> says
    ///         it was OUR rig. Overruns flat while late callbacks climb means the
    ///         machine, not the DSP chain.
    ///     </para>
    ///     <para>
    ///         Everything except the device flags and
    ///         <see cref="XrunStats.DeviceXruns" /> is "since the last
    ///         <see cref="ResetXrunStats" />", and every counter is rebased when
    ///         the device (re)starts — the first four callbacks of a device
    ///         session are ignored, so a device change, a sample-rate change or a
    ///         watchdog recovery does not report a phantom dropout.
    ///     </para>
    ///
    ///     Returns false when the engine handle is gone or the call fails.
    /// </summary>
    public bool TryGetXrunStats(out XrunStats stats)
    {
        stats = default;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            var native = new NativeMethods.DcaXrunStats
            {
                StructBytes = Marshal.SizeOf<NativeMethods.DcaXrunStats>()
            };
            if (NativeMethods.EngineGetXrunStats(h, ref native) != NativeMethods.DcaStatus.Ok)
                return false;

            stats = XrunStats.From(native);
            return true;
        }
        finally { Release(); }
    }

    /// <summary>
    ///     ABI v46. The audio callback's load as a fraction of the buffer period
    ///     (0.25 = a quarter of the block's wall-clock budget; above 1.0 the
    ///     callback overran its deadline), measured inside the callback. The
    ///     average is exponentially weighted (0.5 s); the peak is the largest
    ///     block, decaying over 1.5 s, and is NOT reset by reading, so several
    ///     pollers see the same numbers. Both read 0 while no device runs and
    ///     restart at every device session. Not touched by
    ///     <see cref="ResetXrunStats" />.
    ///
    ///     Returns false when the engine handle is gone or the call fails.
    /// </summary>
    public bool TryGetCallbackLoad(out CallbackLoad load)
    {
        load = default;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            if (NativeMethods.EngineGetCallbackLoad(h, out var average, out var peak) != NativeMethods.DcaStatus.Ok)
                return false;
            load = new CallbackLoad(average, peak);
            return true;
        }
        finally { Release(); }
    }

    /// <summary>ABI v30. Zeroes every "since reset" dropout counter and rebases
    /// the driver's own count. Call it when a health window opens, so the numbers
    /// describe that window rather than the whole process lifetime.</summary>
    public bool ResetXrunStats()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try { return NativeMethods.EngineResetXrunStats(h) == NativeMethods.DcaStatus.Ok; }
        finally { Release(); }
    }

    /// <summary>ABI v26. Schedules a click (0 = beat tick, 1 = downbeat,
    /// 2 = clap) to start at an absolute frame on the output render clock. The
    /// audio thread starts the voice at that exact sample offset inside the
    /// block containing the frame — no block-boundary jitter. A frame already
    /// rendered fires at the head of the current block unless it is more than
    /// ~250 ms stale (dropped); a frame more than ~5 s ahead is dropped.</summary>
    public void ScheduleClick(int clickType, long renderFrame)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineScheduleClick(h, clickType, renderFrame), "engine_schedule_click"); }
        finally { Release(); }
    }

    /// <summary>ABI v26. Drops every scheduled-but-not-yet-sounded click.
    /// Ordered against <see cref="ScheduleClick(int, long)"/>, so a clear issued after a
    /// schedule always cancels it. Call on seek / pause / stop / speed change —
    /// the render clock keeps running while the transport does not.</summary>
    public void ClearScheduledClicks()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineClearScheduledClicks(h), "engine_clear_scheduled_clicks"); }
        finally { Release(); }
    }

    /// <summary>
    ///     Run a MONO buffer through the CSVD drop tuner offline
    ///     (<c>dca_droptune_process_offline</c>) — the same
    ///     <c>dcdsp_droptune.h</c> core the live input path runs, with no engine
    ///     and no device.
    ///     <para>
    ///         The shift is applied in SEGMENTS: <c>semitonesPerSegment[i]</c> for
    ///         <c>segmentLengths[i]</c> samples, in order, through ONE DropTune
    ///         instance — so an engage/disengage lands mid-stream against a primed
    ///         ring exactly as it does under a footswitch, rather than against a
    ///         cold one. That is the whole reason the segment list exists instead
    ///         of a single semitone argument; a caller that just wants a constant
    ///         shift passes one segment covering the buffer.
    ///     </para>
    ///     <para>
    ///         DOWN-ONLY: semitones are clamped native-side to [-24, 0]. The
    ///         shifter is CSVD and not a phase vocoder precisely because
    ///         down-shifting has no latency floor, so there is no up-shift to ask
    ///         for.
    ///     </para>
    ///     <para>
    ///         In place is allowed — pass the same array as
    ///         <paramref name="input" /> and <paramref name="output" />.
    ///     </para>
    /// </summary>
    /// <param name="input">Mono samples at <paramref name="sampleRate" />.</param>
    /// <param name="output">Destination, the same length as
    ///     <paramref name="input" />. May be the same buffer.</param>
    /// <param name="sampleRate">8000..192000; clamped native-side.</param>
    /// <param name="semitonesPerSegment">One shift per segment, in order.</param>
    /// <param name="segmentLengths">Sample count per segment. Must sum to
    ///     <paramref name="input" />.Length.</param>
    /// <param name="maxLagMs">The splice-lag floor knob — 10, 20 and 35 are the
    ///     three shipped modes; see the
    ///     <see cref="ProcessDropTuneOffline(ReadOnlySpan{float},Span{float},int,ReadOnlySpan{float},ReadOnlySpan{int},JuceDropTuneMode)" />
    ///     overload.</param>
    /// <exception cref="ArgumentException"><paramref name="input" /> is empty,
    ///     <paramref name="output" /> is a different length, the two segment
    ///     spans differ in length, or the lengths do not sum to the buffer.</exception>
    /// <exception cref="JuceAudioException">The native call rejected the
    ///     request.</exception>
    public static void ProcessDropTuneOffline(
        ReadOnlySpan<float> input,
        Span<float> output,
        int sampleRate,
        ReadOnlySpan<float> semitonesPerSegment,
        ReadOnlySpan<int> segmentLengths,
        float maxLagMs)
    {
        if (input.IsEmpty) throw new ArgumentException("There is nothing to process.", nameof(input));
        if (output.Length != input.Length)
            throw new ArgumentException("output must be the same length as input.", nameof(output));
        if (semitonesPerSegment.Length != segmentLengths.Length)
            throw new ArgumentException(
                "semitonesPerSegment and segmentLengths describe the same segments and must have the "
                + "same length.", nameof(segmentLengths));
        if (segmentLengths.IsEmpty)
            throw new ArgumentException("At least one segment is required.", nameof(segmentLengths));

        // The sum is checked HERE rather than left to the engine because the
        // failure it prevents is silent: a short sum leaves the tail of the
        // output untouched, which on an in-place call is the ORIGINAL audio at
        // the original pitch — a render that is plausible and wrong.
        long total = 0;
        for (var i = 0; i < segmentLengths.Length; i++)
        {
            if (segmentLengths[i] < 0)
                throw new ArgumentException($"segmentLengths[{i}] is negative.", nameof(segmentLengths));
            total += segmentLengths[i];
        }

        if (total != input.Length)
            throw new ArgumentException(
                $"segmentLengths sum to {total} but the buffer holds {input.Length} samples.",
                nameof(segmentLengths));

        unsafe
        {
            fixed (float* pIn = input)
            fixed (float* pOut = output)
            fixed (float* pSemis = semitonesPerSegment)
            fixed (int* pLens = segmentLengths)
            {
                Check(
                    NativeMethods.DropTuneProcessOffline(sampleRate, maxLagMs, pSemis, pLens,
                        segmentLengths.Length, pIn, pOut, input.Length),
                    "dca_droptune_process_offline");
            }
        }
    }

    /// <summary>
    /// <see cref="ProcessDropTuneOffline(ReadOnlySpan{float},Span{float},int,ReadOnlySpan{float},ReadOnlySpan{int},float)" />
    /// with the lag ceiling of one of the three shipped modes —
    /// <see cref="JuceDropTuneMode.Tight" /> 10 ms,
    /// <see cref="JuceDropTuneMode.Balanced" /> 20 ms (the engine default),
    /// <see cref="JuceDropTuneMode.Smooth" /> 35 ms — so an offline bounce and the
    /// live pedal can be given the same setting by name.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="mode" /> is
    /// not one of the three.</exception>
    public static void ProcessDropTuneOffline(
        ReadOnlySpan<float> input,
        Span<float> output,
        int sampleRate,
        ReadOnlySpan<float> semitonesPerSegment,
        ReadOnlySpan<int> segmentLengths,
        JuceDropTuneMode mode = JuceDropTuneMode.Balanced)
        => ProcessDropTuneOffline(input, output, sampleRate, semitonesPerSegment, segmentLengths,
            mode switch
            {
                JuceDropTuneMode.Tight    => 10f,
                JuceDropTuneMode.Balanced => 20f,
                JuceDropTuneMode.Smooth   => 35f,
                _ => throw new ArgumentOutOfRangeException(nameof(mode), mode,
                         "Not one of the three shipped drop-tune modes."),
            });

    /// <summary>Generate per-block peak amplitudes (0..1) for a media file
    /// in a single shot. <paramref name="blockMs"/> chooses the resolution
    /// of the base level.</summary>
    public static float[] GenerateWaveform(string path, double blockMs)
    {
        if (string.IsNullOrEmpty(path)) return Array.Empty<float>();
        if (blockMs <= 0.0) blockMs = 20.0;
        var probe = NativeMethods.GenerateWaveform(path, blockMs, null, 0, out var total);
        if (probe != NativeMethods.DcaStatus.Ok || total <= 0) return Array.Empty<float>();
        var buf = new float[total];
        var fill = NativeMethods.GenerateWaveform(path, blockMs, buf, total, out _);
        if (fill != NativeMethods.DcaStatus.Ok) return Array.Empty<float>();
        return buf;
    }

    /// <summary>
    /// Do the load-time rate conversion ONCE, into a file
    /// (<c>dca_convert_audio_for_device</c>, ABI v43). Opening
    /// <paramref name="destinationWavPath"/> on a <paramref name="deviceSampleRate"/>
    /// device then plays bit-identical samples to opening
    /// <paramref name="sourcePath"/> there, without the per-open upsample.
    /// </summary>
    /// <returns><see cref="JuceDeviceRateConversion.NotNeeded"/> when the engine
    /// would not convert this file at that rate; nothing is written then.</returns>
    /// <exception cref="JuceAudioException">The source could not be decoded or
    /// converted (<c>Decode</c>), or the destination could not be written.</exception>
    /// <remarks>Slow: it decodes the whole song. Never on a UI or audio thread.
    /// Write to a temporary name and rename, so a reader never sees a partial file.</remarks>
    public static JuceDeviceRateConversion ConvertAudioForDevice(string sourcePath, string destinationWavPath,
        int deviceSampleRate)
    {
        ArgumentException.ThrowIfNullOrEmpty(sourcePath);
        ArgumentException.ThrowIfNullOrEmpty(destinationWavPath);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(deviceSampleRate);
        Check(NativeMethods.ConvertAudioForDevice(sourcePath, destinationWavPath, deviceSampleRate, out var bits),
            "convert_audio_for_device");
        return (JuceDeviceRateConversion)bits;
    }

    /// <summary>Same as <see cref="GenerateWaveform"/>, but decodes an
    /// in-memory audio blob (any basic-format container) — callers that
    /// already hold the file bytes (e.g. PSARC memory playback) skip a
    /// second end-to-end disk read. The array is only read during the call
    /// (the marshaller pins it; the native side keeps no copy).</summary>
    public static float[] GenerateWaveformFromMemory(byte[] data, double blockMs)
    {
        if (data == null || data.Length == 0) return Array.Empty<float>();
        if (blockMs <= 0.0) blockMs = 20.0;
        var probe = NativeMethods.GenerateWaveformFromMemory(data, data.LongLength, blockMs, null, 0, out var total);
        if (probe != NativeMethods.DcaStatus.Ok || total <= 0) return Array.Empty<float>();
        var buf = new float[total];
        var fill = NativeMethods.GenerateWaveformFromMemory(data, data.LongLength, blockMs, buf, total, out _);
        if (fill != NativeMethods.DcaStatus.Ok) return Array.Empty<float>();
        return buf;
    }

    /// <summary>
    /// Read the peak absolute sample seen on the live input since the last call and clear
    /// the accumulator (<c>dca_engine_get_input_peak</c>) — the read-and-reset shape a
    /// level meter wants. Returns 0 on a disposed engine instead of throwing, because this
    /// is polled from a UI timer.
    /// </summary>
    /// <returns>Peak magnitude in the linear 0..1 sample range; above 1.0 means clipping.</returns>
    public float GetAndResetInputPeak()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return 0f;
        try
        {
            WarnPollFailure(NativeMethods.EngineGetInputPeak(h, out var p), "engine_get_input_peak");
            return p;
        }
        finally { Release(); }
    }

    /// <summary>Pick which input channel feeds the live capture ring (the
    /// dry pre-gain pre-VST signal used by <see cref="ReadInputCapture(System.Span{float})"/>).
    /// <c>-1</c> mixes all input channels down to mono.</summary>
    public void SetInputCaptureChannel(int channel)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetInputCaptureChannel(h, channel), "engine_set_input_capture_channel"); }
        finally { Release(); }
    }

    /// <summary>Pull the most recent <paramref name="dest"/>.Length samples
    /// of the live dry-input capture ring into the caller's buffer. The ring
    /// holds the raw, pre-gain, pre-VST signal at the device sample rate so
    /// FFT / tuner / metering see exactly what the interface delivers. Older
    /// samples are silently overwritten by the audio thread; callers that
    /// want continuous capture should poll faster than the ring depth
    /// (currently 32768 samples). Returns the number of samples written.
    /// </summary>
    public int ReadInputCapture(Span<float> dest)
    {
        if (dest.IsEmpty) return 0;
        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try
        {
            unsafe
            {
                fixed (float* p = dest)
                {
                    WarnPollFailure(NativeMethods.EngineReadInputCapture(h, p, dest.Length, out var written),
                        "engine_read_input_capture");
                    return written;
                }
            }
        }
        finally { Release(); }
    }

    /// <summary>Streaming read of the dry-input capture ring: returns only
    /// samples not yet consumed by a previous streaming read (0 when caught
    /// up), skipping ahead if the caller fell more than one ring (~0.7 s)
    /// behind. Use for continuous consumers such as live note detection —
    /// unlike <see cref="ReadInputCapture(System.Span{float})"/>, which always re-serves the
    /// latest window and therefore must never be drain-polled.</summary>
    public int ReadInputCaptureStream(Span<float> dest)
    {
        if (dest.IsEmpty) return 0;
        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try
        {
            unsafe
            {
                fixed (float* p = dest)
                {
                    WarnPollFailure(NativeMethods.EngineReadInputCaptureStream(h, p, dest.Length, out var written),
                        "engine_read_input_capture_stream");
                    return written;
                }
            }
        }
        finally { Release(); }
    }

    /// <summary>Like <see cref="ReadInputCaptureStream(Span{float})"/> but also reports
    /// the ABSOLUTE capture-stream position (total samples ever captured
    /// before <c>dest[0]</c>). A jump between calls beyond what was consumed
    /// means the ring overran and skipped — consumers re-anchor on it.</summary>
    public int ReadInputCaptureStream(Span<float> dest, out ulong streamPosition)
    {
        streamPosition = 0;
        if (dest.IsEmpty) return 0;
        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try
        {
            unsafe
            {
                fixed (float* p = dest)
                {
                    WarnPollFailure(NativeMethods.EngineReadInputCaptureStream2(h, p, dest.Length,
                            out var written, out streamPosition),
                        "engine_read_input_capture_stream2");
                    return written;
                }
            }
        }
        finally { Release(); }
    }

    /// <summary>Atomically samples {total captured input samples, host
    /// high-resolution ticks at the end of the same audio block}. The tick
    /// timebase equals <see cref="System.Diagnostics.Stopwatch.GetTimestamp"/>
    /// (QPC on Windows), so callers can map a capture-stream sample index to
    /// host time exactly: <c>ticks - (capturedSamples - index) / sampleRate *
    /// Stopwatch.Frequency</c>. Returns false until the first input block.</summary>
    public bool TryGetInputCaptureSync(out ulong capturedSamples, out long hostTicks)
    {
        capturedSamples = 0;
        hostTicks = 0;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            return NativeMethods.EngineGetInputCaptureSync(h, out capturedSamples, out hostTicks)
                   == NativeMethods.DcaStatus.Ok;
        }
        finally { Release(); }
    }

    // ---- Per-player seats (ABI v28) ---------------------------------------
    // The engine hosts <see cref="MaxPlayers"/> independent live-input rigs.
    // Each owns its own DSP tone chain, VST3 input chain, capture ring and
    // hardware input channel, and they all share ONE open device — channel
    // routing inside a single AudioIODevice, never a second device open.
    // Every member above is the same call with player = 0.

    /// <summary>How many independent live-input rigs the native engine hosts
    /// (mirrors <c>DCA_MAX_PLAYERS</c>).</summary>
    public static int MaxPlayers => NativeMethods.MaxPlayers;

    /// <summary>DSP node ids are unique per ENGINE: seat <c>p</c> hands out
    /// <c>p * PlayerNodeIdStride + local</c>. Seat 0's ids are 0, 1, 2, …
    /// exactly as before ABI v28.</summary>
    public static int PlayerNodeIdStride => NativeMethods.PlayerNodeIdStride;

    // ---- In-house DSP tone rig -------------------------------------------

    private readonly JuceDspChain?[] _dspChains = new JuceDspChain?[NativeMethods.MaxPlayers];
    private readonly object _dspChainGate = new();

    /// <summary>
    ///     One seat's in-house DSP tone rig — the reorderable effect chain applied
    ///     to that seat's guitar INPUT before any user VST input chain. Seat 0 is
    ///     the solo player and is byte-for-byte the pre-ABI-v28 path.
    ///     <para>
    ///         The same <see cref="JuceDspChain" /> instance is returned for a given
    ///         seat, so the type ids of nodes IT created stay known (the ABI has no
    ///         "what type is node N" export). Build the palette from
    ///         <see cref="JuceDspModuleCatalog" />, which needs no engine at all.
    ///     </para>
    ///     <para>
    ///         Every call on the returned chain pins this engine's handle for its
    ///         duration. That is the difference between this and binding
    ///         <c>dca_engine_dsp_*</c> against <see cref="NativeHandle" /> yourself,
    ///         which is unpinned: a rig call racing a device change surfaces as a
    ///         caught <see cref="ObjectDisposedException" /> rather than a native
    ///         crash.
    ///     </para>
    /// </summary>
    /// <param name="player">Seat, <c>0..MaxPlayers-1</c>.</param>
    /// <exception cref="ArgumentOutOfRangeException">Not a seat this engine has.</exception>
    /// <exception cref="ObjectDisposedException">The engine has been disposed.</exception>
    public JuceDspChain GetDspChain(int player = 0)
    {
        ValidatePlayer(player, nameof(player));
        // Acquire so a disposed engine is reported HERE rather than at the first
        // call on a chain that looked fine.
        _ = Acquire();
        Release();
        lock (_dspChainGate) return _dspChains[player] ??= new JuceDspChain(this, player);
    }

    /// <summary>How many live-input rigs run. 1 (the default) is the pre-v28
    /// signal path byte-for-byte; 2 is couch co-op. Clamped to
    /// <c>1..MaxPlayers</c>.</summary>
    public void SetPlayerCount(int count)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetPlayerCount(h, count), "engine_set_player_count"); }
        finally { Release(); }
    }

    /// <summary>How many live-input rigs are currently running; 0 when the
    /// engine handle is gone.</summary>
    public int GetPlayerCount()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try { return NativeMethods.EngineGetPlayerCount(h); }
        finally { Release(); }
    }

    /// <summary>Pick which hardware input channel feeds seat
    /// <paramref name="player"/>'s capture ring AND — when more than one seat is
    /// live — that seat's DSP + VST rig. <c>-1</c> mixes all input channels down
    /// to mono. Defaults to <paramref name="player"/>, i.e. two guitars into one
    /// 2-in interface works with no configuration.</summary>
    public void SetPlayerInputChannel(int player, int channel)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            WarnIfFailed(NativeMethods.EngineSetPlayerInputChannel(h, player, channel),
                "engine_set_player_input_channel");
        }
        finally { Release(); }
    }

    /// <summary>The hardware input channel feeding a seat, or -1 for
    /// "mix to mono" / an unknown seat / a dead handle.</summary>
    public int GetPlayerInputChannel(int player)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return -1;
        try { return NativeMethods.EngineGetPlayerInputChannel(h, player); }
        finally { Release(); }
    }

    /// <summary>Per-seat monitor trim, MULTIPLIED into the global monitor gain.
    /// 1.0 (the default) leaves the pre-v28 level untouched; this is how two
    /// guitars get balanced against each other in one monitor mix.</summary>
    public void SetPlayerMonitorGain(int player, float linearGain)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            WarnIfFailed(NativeMethods.EngineSetPlayerMonitorGain(h, player, linearGain),
                "engine_set_player_monitor_gain");
        }
        finally { Release(); }
    }

    /// <summary>Peak absolute POST-rig sample for one seat since the last call;
    /// resets on read, like <see cref="GetAndResetInputPeak"/> (which is seat 0).</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player"/> is
    /// not a seat this engine has.</exception>
    public float GetAndResetPlayerInputPeak(int player)
    {
        ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return 0f;
        try
        {
            WarnPollFailure(NativeMethods.EngineGetPlayerInputPeak(h, player, out var p),
                "engine_get_player_input_peak");
            return p;
        }
        finally { Release(); }
    }

    /// <summary>Windowed capture read for one seat — <see cref="ReadInputCapture(System.Span{float})"/>
    /// with an explicit seat. Each seat has its own cursors, so two live
    /// detectors never steal each other's samples.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player"/> is
    /// not a seat this engine has.</exception>
    public int ReadInputCapture(int player, Span<float> dest)
    {
        ValidatePlayer(player, nameof(player));
        if (dest.IsEmpty) return 0;
        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try
        {
            unsafe
            {
                fixed (float* p = dest)
                {
                    WarnPollFailure(
                        NativeMethods.EngineReadInputCaptureForPlayer(h, player, p, dest.Length, out var written),
                        "engine_read_input_capture_for_player");
                    return written;
                }
            }
        }
        finally { Release(); }
    }

    /// <summary>Streaming capture read for one seat — this is what makes live
    /// note detection able to judge BOTH players: each seat drains its own ring
    /// from its own cursor.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player"/> is
    /// not a seat this engine has.</exception>
    public int ReadInputCaptureStream(int player, Span<float> dest)
    {
        ValidatePlayer(player, nameof(player));
        if (dest.IsEmpty) return 0;
        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try
        {
            unsafe
            {
                fixed (float* p = dest)
                {
                    WarnPollFailure(
                        NativeMethods.EngineReadInputCaptureStreamForPlayer(h, player, p, dest.Length, out var written),
                        "engine_read_input_capture_stream_for_player");
                    return written;
                }
            }
        }
        finally { Release(); }
    }

    /// <summary>Position-reporting streaming capture read for one seat.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player"/> is
    /// not a seat this engine has.</exception>
    public int ReadInputCaptureStream(int player, Span<float> dest, out ulong streamPosition)
    {
        ValidatePlayer(player, nameof(player));
        streamPosition = 0;
        if (dest.IsEmpty) return 0;
        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try
        {
            unsafe
            {
                fixed (float* p = dest)
                {
                    WarnPollFailure(
                        NativeMethods.EngineReadInputCaptureStream2ForPlayer(h, player, p, dest.Length,
                            out var written, out streamPosition),
                        "engine_read_input_capture_stream2_for_player");
                    return written;
                }
            }
        }
        finally { Release(); }
    }

    /// <summary>{captured samples, host ticks} for one seat's ring.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player"/> is
    /// not a seat this engine has.</exception>
    public bool TryGetInputCaptureSync(int player, out ulong capturedSamples, out long hostTicks)
    {
        ValidatePlayer(player, nameof(player));
        capturedSamples = 0;
        hostTicks = 0;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            return NativeMethods.EngineGetInputCaptureSyncForPlayer(h, player, out capturedSamples, out hostTicks)
                   == NativeMethods.DcaStatus.Ok;
        }
        finally { Release(); }
    }

    /// <summary>
    /// Open the current ASIO driver's own control panel
    /// (<c>dca_engine_show_asio_panel</c>). <b>This blocks for as long as the vendor's
    /// MODAL dialog is up</b>, which is why the handle drain waits on the last release
    /// rather than freeing under it. Never throws: failures come back as
    /// <paramref name="error"/>.
    /// </summary>
    /// <param name="parentWindow">Native parent window handle, or <see cref="nint.Zero"/>.</param>
    /// <param name="error">Why it failed, or <c>null</c> on success.</param>
    /// <returns><c>true</c> when the panel was shown.</returns>
    public bool TryShowAsioControlPanel(nint parentWindow, out string? error)
    {
        error = null;
        var h = TryAcquire();
        if (h == nint.Zero) { error = "engine disposed"; return false; }
        try
        {
            var s = NativeMethods.EngineShowAsioPanel(h, parentWindow);
            if (s == NativeMethods.DcaStatus.Ok) return true;
            error = $"engine_show_asio_panel returned {s}";
            return false;
        }
        finally { Release(); }
    }

    /// <summary>
    /// Decode an audio file and attach it to the mixer as a new source
    /// (<c>dca_source_open_file</c>). Any file-to-device rate conversion happens ONCE here,
    /// at load, through the rational polyphase resampler — never per block. The returned
    /// source is tracked so <see cref="Dispose()"/> tears it down in the right order, but
    /// the caller still owns it.
    /// </summary>
    /// <param name="path">Path to the audio file.</param>
    /// <returns>The new source, stopped at position zero.</returns>
    /// <exception cref="JuceAudioException">The file could not be opened or decoded
    /// (<see cref="JuceAudioStatus.Decode"/>).</exception>
    public JuceAudioSource OpenFile(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var h = Acquire();
        try
        {
            Check(NativeMethods.SourceOpenFile(h, path, out var handle), "source_open_file");
            return Track(new JuceAudioSource(handle));
        }
        finally { Release(); }
    }

    /// <summary>Switches the global Rubber Band stretcher engine to the
    /// lighter R2 (Faster) algorithm when <paramref name="faster"/> is true,
    /// or back to the default R3 (Finer) algorithm. Applies to sources
    /// constructed/prepared after the call.</summary>
    public void SetStretchQuality(bool faster)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetStretchQuality(h, faster ? 1 : 0), "engine_set_stretch_quality"); }
        catch (Exception ex)
        {
            // Constrained to Exception so OOM / thread-abort propagate.
            JuceLog.Emit(JuceLogLevel.Warn,
                $"EngineSetStretchQuality threw: {ex.GetType().Name}: {ex.Message}");
        }
        finally { Release(); }
    }

    /// <summary>Open a playable audio source backed by an in-memory blob.
    /// The bytes are copied into a native-side buffer so the caller can
    /// reuse / free the input array immediately. <paramref name="formatHint"/>
    /// is an optional file-extension hint (<c>"wav"</c>, <c>"ogg"</c>, etc.)
    /// used to bias format detection; pass <c>null</c> to let JUCE probe.</summary>
    public unsafe JuceAudioSource OpenMemory(ReadOnlySpan<byte> bytes, string? formatHint = null)
    {
        // Acquire first so a disposed engine reports ObjectDisposedException
        // rather than an argument complaint about the payload.
        var h = Acquire();
        try
        {
            if (bytes.IsEmpty) throw new ArgumentException("Audio blob is empty.", nameof(bytes));
            fixed (byte* p = bytes)
            {
                Check(NativeMethods.SourceOpenMemory(h, p, (nuint)bytes.Length, formatHint, out var handle),
                      "source_open_memory");
                return Track(new JuceAudioSource(handle));
            }
        }
        finally { Release(); }
    }

    // The nine raw-nint plugin members that used to live here — LoadVst3,
    // UnloadPlugin, Attach/DetachPluginToInput, Show/HidePluginEditor,
    // Attach/DetachPluginToInstrument and GetPluginLatencySamples — are GONE
    // (0.8.x). Eight were already [Obsolete] and the ninth
    // (GetPluginLatencySamples) never was, which is the part that mattered: it
    // took a raw handle, skipped JuceAudioPlugin's drain entirely, and
    // duplicated JuceAudioPlugin.LatencySamples with no marking to say so — an
    // unmarked raw-handle door next to eight marked ones reads as the supported
    // way in. Nothing outside this repo's own tests referenced any of the nine
    // (DotCharter grep, 2026-09-10). Use JuceAudioPlugin: LoadPlugin(path) hands
    // you one, and it drains.

    /// <summary>Load a VST3 plugin and wrap it in a safe <see cref="JuceAudioPlugin"/>,
    /// tracked by this engine so it is destroyed before the engine is.
    /// Throws <see cref="JuceAudioException"/> on failure.</summary>
    public JuceAudioPlugin LoadPlugin(string path) => LoadPlugin(path, 0);

    /// <summary>
    ///     ABI v33: load ONE component of a multi-component VST3 bundle. A single
    ///     <c>.vst3</c> may declare several plugins (an amp and its cab, an effect
    ///     and its companion instrument, a vendor's whole suite in one file); until
    ///     v33 this ABI took descriptor 0 and nothing else, so every component past
    ///     the first was unreachable — and unreported, because loading the bundle
    ///     looked like it had worked.
    ///     <para>
    ///         <see cref="LoadPlugin(string)" /> is exactly
    ///         <c>LoadPlugin(path, 0)</c>. An index the bundle does not have is
    ///         REJECTED, never clamped onto component 0: a silently substituted
    ///         plugin is a saved rig that reloads as a different effect.
    ///     </para>
    /// </summary>
    /// <param name="path">The bundle path.</param>
    /// <param name="componentIndex">0..<see cref="CountVst3Components"/> − 1.</param>
    /// <exception cref="FileNotFoundException">No such bundle.</exception>
    /// <exception cref="JuceAudioException">The load or the index was refused.</exception>
    public JuceAudioPlugin LoadPlugin(string path, int componentIndex)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentOutOfRangeException.ThrowIfNegative(componentIndex);
        // Acquire BEFORE the existence probe: a disposed engine must report
        // ObjectDisposedException, not FileNotFoundException, whatever the path.
        var h = Acquire();
        try
        {
            if (!File.Exists(path) && !Directory.Exists(path))
                throw new FileNotFoundException($"VST3 not found at '{path}'.", path);
            var status = NativeMethods.PluginLoadVst3Component(h, path, componentIndex, out var handle);
            if (status != NativeMethods.DcaStatus.Ok || handle == nint.Zero)
                throw new JuceAudioException(
                    $"plugin_load_vst3 failed for '{path}' component {componentIndex} with {status}.",
                    status, "plugin_load_vst3");
            return Track(new JuceAudioPlugin(handle, path));
        }
        finally { Release(); }
    }

    /// <summary>
    ///     ABI v33: how many plugins the VST3 bundle at <paramref name="path" />
    ///     declares. Almost always 1; a vendor suite shipped as one bundle is the
    ///     case this exists for. Needs no engine state, but it IS a scan — a cold
    ///     bundle can take seconds, so do not call it per UI frame.
    /// </summary>
    /// <returns>The component count (at least 1).</returns>
    /// <exception cref="FileNotFoundException">No such bundle.</exception>
    /// <exception cref="JuceAudioException">The file yields no VST3 descriptors at
    /// all — not a VST3, or the scan failed.</exception>
    public static int CountVst3Components(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        if (!File.Exists(path) && !Directory.Exists(path))
            throw new FileNotFoundException($"VST3 not found at '{path}'.", path);
        var status = NativeMethods.PluginCountVst3Components(path, out var count);
        if (status != NativeMethods.DcaStatus.Ok || count <= 0)
            throw new JuceAudioException($"plugin_count_vst3_components failed for '{path}' with {status}.",
                status, "plugin_count_vst3_components");
        return count;
    }

    /// <summary>
    /// Arm / disarm the shared-memory bridge to the sandboxed out-of-process
    /// VST host (ABI v19). While armed, the audio callback round-trips live
    /// input, queued MIDI and the child's processed/rendered audio through
    /// the named mapping once per block; on a missed deadline the input
    /// passes through dry. Returns <c>true</c> on success. Additive ABI
    /// v19+ — and, as everywhere on this boundary, not something an older
    /// runtime can decline: the ABI check at open is an equality comparison.
    /// </summary>
    public bool SetRemoteBridge(string? shmName, string? requestEventName, string? responseEventName, bool enable)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            var status = NativeMethods.EngineSetRemoteBridge(h,
                shmName, requestEventName, responseEventName, enable ? 1 : 0);
            if (status != NativeMethods.DcaStatus.Ok)
                _log.LogWarning("[native] engine_set_remote_bridge(enable={Enable}) returned {Status}", enable, status);
            return status == NativeMethods.DcaStatus.Ok;
        }
        finally { Release(); }
    }

    /// <summary>
    /// Health counters for the out-of-process VST bridge: cumulative blocks
    /// processed / timed out plus the child's free-running heartbeat counter
    /// (stalls when the child dies). All zero while the bridge is disabled.
    /// </summary>
    public bool TryGetRemoteBridgeStats(out uint processed, out uint timeouts, out uint heartbeat)
    {
        processed = timeouts = heartbeat = 0;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            return NativeMethods.EngineGetRemoteBridgeStats(h, out processed, out timeouts, out heartbeat)
                   == NativeMethods.DcaStatus.Ok;
        }
        finally { Release(); }
    }

    /// <summary>Lock-free send of a 3-byte MIDI short message to the attached
    /// instrument. No-op if no instrument is attached.</summary>
    public void SendMidi(byte status, byte data1, byte data2)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSendMidiShort(h, status, data1, data2), "engine_send_midi_short"); }
        finally { Release(); }
    }

    /// <summary>
    ///     Queue a 3-byte MIDI short for the attached instrument at an EXACT
    ///     output frame (ABI v34).
    ///     <para>
    ///         <see cref="SendMidi(byte,byte,byte)" /> stamps every event at
    ///         sample 0 of whichever block happens to drain it — up to one whole
    ///         buffer of skew, 5–10 ms, on every note. This names the sample.
    ///         Frames are in the domain
    ///         <see cref="TryGetRenderClock" /> publishes:
    ///     </para>
    ///     <code>
    ///     engine.TryGetRenderClock(out var frames, out _);
    ///     engine.SendMidi(0x90, 60, 100, frames + offsetInSamples);
    ///     </code>
    ///     <para>
    ///         The event is PARKED on the audio thread until the block that
    ///         contains its frame, so scheduling ahead is legal (up to 5 s) and
    ///         an out-of-order send cannot hold back the ones behind it. A
    ///         negative <paramref name="renderFrame" /> means "as soon as
    ///         possible" and is bit-for-bit the three-argument overload.
    ///     </para>
    /// </summary>
    /// <param name="status">Status byte: command nibble | channel, e.g. <c>0x90</c>
    /// for note-on on channel 0. A status below <c>0x80</c> is REJECTED native
    /// side — packed 0 is the queue's empty-slot marker.</param>
    /// <param name="data1">First data byte (0..127) — note number, or controller
    /// number for a CC.</param>
    /// <param name="data2">Second data byte (0..127) — velocity, or controller
    /// value for a CC.</param>
    /// <param name="renderFrame">ABSOLUTE output-timeline frame, in the domain
    /// <see cref="TryGetRenderClock" /> publishes. Negative means "as soon as
    /// possible"; more than 5 s ahead is DROPPED and counted.</param>
    public void SendMidi(byte status, byte data1, byte data2, long renderFrame)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            var packed = (uint)status | ((uint)data1 << 8) | ((uint)data2 << 16);
            WarnIfFailed(NativeMethods.EngineSendMidiAt(h, packed, renderFrame), "engine_send_midi_at");
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Poll-only counters for scheduled MIDI. <c>Late</c> is the one worth
    ///     watching: it counts events whose frame had already gone past when
    ///     their block ran (placed at sample 0 instead), and it is the ONLY
    ///     signal that a caller's scheduling horizon is shorter than the
    ///     engine's own latency.
    ///     <para>
    ///         Prefer <see cref="TryGetMidiTimingStats" />: this overload reports
    ///         <c>(0, 0, 0)</c> for BOTH "the engine is disposed" and "nothing has
    ///         been scheduled yet", which is the same conflation
    ///         <see cref="TryGetXrunStats" /> was shaped to avoid.
    ///     </para>
    /// </summary>
    /// <returns><c>Scheduled</c> — events parked for a future block;
    /// <c>Late</c> — events whose frame had already passed, placed at sample 0;
    /// <c>Dropped</c> — events the queue or the parked set could not hold.</returns>
    public (long Scheduled, long Late, long Dropped) GetMidiTimingStats()
        => TryGetMidiTimingStats(out var stats)
            ? (stats.Scheduled, stats.Late, stats.Dropped)
            : (0, 0, 0);

    /// <summary>
    ///     Read the scheduled-MIDI counters, mirroring
    ///     <see cref="TryGetXrunStats" />: <c>false</c> means the engine is gone
    ///     or the native read failed, which a zeroed tuple cannot say.
    /// </summary>
    /// <param name="stats">The counters; all zero when this returns false.</param>
    /// <returns>True when <paramref name="stats" /> was read from the engine.</returns>
    public bool TryGetMidiTimingStats(out MidiTimingStats stats)
    {
        stats = default;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            if (NativeMethods.EngineGetMidiTimingStats(h, out var s, out var l, out var d)
                != NativeMethods.DcaStatus.Ok) return false;
            stats = new MidiTimingStats { Scheduled = s, Late = l, Dropped = d };
            return true;
        }
        finally { Release(); }
    }

    /// <summary>Zero the counters <see cref="TryGetMidiTimingStats" /> reports.</summary>
    /// <returns>True when the engine accepted the reset; false when it is
    /// disposed or the native call failed — the same shape as
    /// <see cref="ResetXrunStats" />, so a caller polling a delta can tell a
    /// reset that did not happen from one that did.</returns>
    public bool ResetMidiTimingStats()
    {
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            return NativeMethods.EngineResetMidiTimingStats(h) == NativeMethods.DcaStatus.Ok;
        }
        finally { Release(); }
    }

    /// <summary>
    /// Set the linear output gain of the MIDI instrument (VSTi) bus
    /// (<c>dca_engine_set_instrument_gain</c>), ramped across the block. A no-op on a
    /// disposed engine.
    /// </summary>
    /// <param name="gain">Linear gain; 1.0 is unity.</param>
    public void SetInstrumentGain(float gain)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try { WarnIfFailed(NativeMethods.EngineSetInstrumentGain(h, gain), "engine_set_instrument_gain"); }
        finally { Release(); }
    }

    /// <summary>
    /// List names of all available MIDI input devices. The index of a
    /// name in the returned list is the <c>deviceIndex</c> to pass to
    /// <see cref="OpenMidiInput(int, Action{JuceMidiMessage})"/>.
    /// </summary>
    public static IReadOnlyList<string> EnumerateMidiInputs() => JuceMidiInput.Enumerate();

    /// <summary>
    /// Open a hardware MIDI input by index. The handler is invoked on the
    /// JUCE MIDI thread for each incoming message; do not block in it.
    /// Dispose the returned <see cref="JuceMidiInput"/> to stop and close.
    /// </summary>
    public JuceMidiInput OpenMidiInput(int deviceIndex, Action<JuceMidiMessage> handler)
        => OpenMidiInputCore(() => new JuceMidiInput(deviceIndex, handler));

    /// <summary>
    ///     Open a hardware MIDI input whose handler borrows the driver's bytes IN
    ///     PLACE — no <see cref="byte" />[] and no <see cref="JuceMidiMessage" />
    ///     per event. Prefer this for anything that sees continuous controller
    ///     traffic; the span is only valid for the duration of the call, so copy
    ///     what you keep. Same lifetime rules as the
    ///     <see cref="Action{T}" /> overload: dispose the returned input, or the
    ///     engine, to close the device.
    /// </summary>
    public JuceMidiInput OpenMidiInput(int deviceIndex, JuceMidiSpanHandler handler)
        => OpenMidiInputCore(() => new JuceMidiInput(deviceIndex, handler));

    private JuceMidiInput OpenMidiInputCore(Func<JuceMidiInput> open)
    {
        // Acquire so a disposed engine reports ObjectDisposedException rather
        // than handing back a live device nothing will ever close, and so the
        // Track below cannot race a Dispose that already claimed the child
        // list. Track gives teardown ORDER only — the registry is weak — so
        // the input is ALSO rooted in _midiInputs until it or the engine is
        // disposed: the "MIDI dies at a random GC" shape needs a strong ref.
        var h = Acquire();
        try
        {
            var input = Track(open());
            lock (_childrenGate) _midiInputs.Add(input);
            input.Disposed = () =>
            {
                lock (_childrenGate) _midiInputs.Remove(input);
            };
            return input;
        }
        finally { Release(); }
    }

    /// <summary>
    /// List names of all available MIDI OUTPUT devices (ABI v34). The index of
    /// a name is the <c>deviceIndex</c> to pass to
    /// <see cref="OpenMidiOutput" />.
    /// </summary>
    public static IReadOnlyList<string> EnumerateMidiOutputs() => JuceMidiOutput.Enumerate();

    /// <summary>
    ///     Open a hardware MIDI output by index. Dispose the returned
    ///     <see cref="JuceMidiOutput" /> — or the engine — to close it.
    ///     <para>
    ///         Same lifetime contract as
    ///         <see cref="OpenMidiInput(int, Action{JuceMidiMessage})" />: the
    ///         weak child registry gives teardown ORDER, and the strong list is
    ///         what stops an output the app wired up and kept no handle to
    ///         being closed by the first gen-2 collection.
    ///     </para>
    /// </summary>
    public JuceMidiOutput OpenMidiOutput(int deviceIndex)
    {
        var h = Acquire();
        try
        {
            var output = Track(new JuceMidiOutput(this, deviceIndex));
            lock (_childrenGate) _midiOutputs.Add(output);
            output.Disposed = () =>
            {
                lock (_childrenGate) _midiOutputs.Remove(output);
            };
            return output;
        }
        finally { Release(); }
    }

    /// <summary>MIDI outputs this engine is keeping alive. Test seam.</summary>
    internal int RootedMidiOutputsForTests
    {
        get
        {
            lock (_childrenGate) return _midiOutputs.Count;
        }
    }

    /// <summary>MIDI inputs this engine is keeping alive. Test seam.</summary>
    internal int RootedMidiInputsForTests
    {
        get
        {
            lock (_childrenGate) return _midiInputs.Count;
        }
    }

    /// <summary>
    /// Render an audio file to a 16-bit WAV on disk. Faster-than-realtime; does
    /// not require the engine to be running. Useful for stem export, preview
    /// clips and format/rate conversion. <paramref name="sampleRate"/> = 0
    /// keeps the source rate.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <b>Time stretch and pitch shift are NOT implemented on this path.</b>
    /// <paramref name="tempo"/> must be 0 (source default) or 1.0 and
    /// <paramref name="semitones"/> must be 0; anything else throws
    /// <see cref="NotSupportedException"/>.
    /// </para>
    /// <para>
    /// This used to be documented as going "through the same Rubber Band
    /// stretcher used by the live engine". It never did: the native body was
    /// <c>(void) tempo; (void) semitones;</c> behind a "real DSP TBD" comment,
    /// so a caller asking for −2 semitones got <c>DCA_OK</c> and a bit-exact
    /// copy at the original pitch — a practice file that is silently not
    /// transposed. The native export now rejects it too
    /// (<c>DCA_ERR_INVALID_ARG</c>); this check exists so the refusal names
    /// itself instead of arriving as a generic bad-argument error.
    /// </para>
    /// </remarks>
    /// <exception cref="NotSupportedException">
    /// A non-unity <paramref name="tempo"/> or a non-zero
    /// <paramref name="semitones"/> was requested.
    /// </exception>
    public static void RenderToWav(string srcPath, string dstPath,
        float tempo = 1.0f, float semitones = 0.0f, int sampleRate = 0)
    {
        if (string.IsNullOrEmpty(srcPath)) throw new ArgumentException("Source path required.", nameof(srcPath));
        if (string.IsNullOrEmpty(dstPath)) throw new ArgumentException("Destination path required.", nameof(dstPath));
        // Argument validation the native side also performs (NaN/Inf tempo, out
        // of range rate) is left to it, so the two cannot drift; this one is
        // here purely to give the unsupported case an honest exception type.
        if (float.IsFinite(tempo) && float.IsFinite(semitones)
            && (tempo is not (0.0f or 1.0f) || semitones != 0.0f))
        {
            throw new NotSupportedException(
                $"RenderToWav cannot stretch or pitch-shift (asked for tempo={tempo}, "
                + $"semitones={semitones}). This offline path is a decode + WAV write; "
                + "use the live engine's Rubber Band transport for stretch/shift.");
        }

        Check(NativeMethods.RenderToWav(srcPath, dstPath, tempo, semitones, sampleRate), "render_to_wav");
    }

    /// <summary>Enumerate output devices using this engine's live AudioDeviceManager.</summary>
    public IReadOnlyList<JuceAudioDevice> EnumerateOutputDevicesLive() => EnumerateLive(isInput: false);

    /// <summary>Enumerate input devices using this engine's live AudioDeviceManager.</summary>
    public IReadOnlyList<JuceAudioDevice> EnumerateInputDevicesLive()  => EnumerateLive(isInput: true);

    private IReadOnlyList<JuceAudioDevice> EnumerateLive(bool isInput)
    {
        var h = TryAcquire();
        if (h == nint.Zero) return Array.Empty<JuceAudioDevice>();
        try
        {
            var probe = isInput
                ? NativeMethods.EngineEnumerateInputs (h, null, 0, out var probeCount)
                : NativeMethods.EngineEnumerateOutputs(h, null, 0, out probeCount);
            if (probe != NativeMethods.DcaStatus.Ok || probeCount <= 0) return Array.Empty<JuceAudioDevice>();
            var buffer = new NativeMethods.DcaDeviceInfo[probeCount];
            var status = isInput
                ? NativeMethods.EngineEnumerateInputs (h, buffer, probeCount, out var actual)
                : NativeMethods.EngineEnumerateOutputs(h, buffer, probeCount, out actual);
            if (status != NativeMethods.DcaStatus.Ok || actual <= 0) return Array.Empty<JuceAudioDevice>();
            var list = new List<JuceAudioDevice>(actual);
            for (var i = 0; i < actual; i++) list.Add(JuceAudioDevice.FromNative(buffer[i]));
            return list;
        }
        finally { Release(); }
    }

    /// <summary>Enumerate output devices across every available host (ASIO, WASAPI, DirectSound, CoreAudio, …).</summary>
    public static IReadOnlyList<JuceAudioDevice> EnumerateOutputDevices()
        => EnumerateDevices(isInput: false);

    /// <summary>Enumerate input devices across every available host.</summary>
    public static IReadOnlyList<JuceAudioDevice> EnumerateInputDevices()
        => EnumerateDevices(isInput: true);

    private static IReadOnlyList<JuceAudioDevice> EnumerateDevices(bool isInput)
    {
        var status = isInput
            ? NativeMethods.EnumerateInputDevices(null, 0, out var probeCount)
            : NativeMethods.EnumerateOutputDevices(null, 0, out probeCount);
        if (status != NativeMethods.DcaStatus.Ok || probeCount <= 0)
            return Array.Empty<JuceAudioDevice>();

        var buffer = new NativeMethods.DcaDeviceInfo[probeCount];
        status = isInput
            ? NativeMethods.EnumerateInputDevices(buffer, probeCount, out var actual)
            : NativeMethods.EnumerateOutputDevices(buffer, probeCount, out actual);
        if (status != NativeMethods.DcaStatus.Ok || actual <= 0)
            return Array.Empty<JuceAudioDevice>();

        var list = new List<JuceAudioDevice>(actual);
        for (var i = 0; i < actual; i++) list.Add(JuceAudioDevice.FromNative(buffer[i]));
        return list;
    }

    /// <summary>
    /// Claim the engine handle (which closes the child registry so no new source, plugin
    /// or MIDI device can be created), stop the device, dispose every tracked child, then
    /// destroy the native engine. Disposing the children FIRST is required: the drain is
    /// per-handle, so the engine's own counter says nothing about a <c>dca_source_*</c>
    /// call in flight. Idempotent.
    /// </summary>
    public void Dispose() => Dispose(disposing: true);

    private void Dispose(bool disposing)
    {
        // TakeForDispose atomically claims the handle (so concurrent Dispose
        // calls and a finalizer race cannot both reach EngineDestroy) AND
        // drains in-flight P/Invokes, so a capture-ring poll or a peak read
        // on another thread can no longer be mid-call when the engine is
        // destroyed under it. Claiming it FIRST is also what closes the child
        // registry: OpenFile / OpenMemory / LoadPlugin / OpenMidiInput all
        // Acquire before they Track, so past this line no new child exists.
        // `_owner` is null only when the constructor threw before engine_create
        // returned; the finalizer still reaches here on that instance.
        //
        // THE FINALIZER MAY NOT WAIT. There is ONE finalizer thread in the
        // process, so a 60 s drain here does not stall this engine — it stalls
        // every finalizer behind it, including the child wrappers whose
        // completion this method is about to check. On that path the deadline is
        // 0: either nothing is in flight (drain succeeds immediately) or the
        // handle is leaked, logged at Error by TakeForDispose. An explicit
        // Dispose keeps the generous timeout — it is on a caller's own thread
        // and has a modal ASIO panel / a cold VST3 scan to clear.
        // …but FIRST the async worker (JuceAudioEngine.Async.cs), because an
        // in-flight async call is inside a native call holding this very handle
        // pinned. Draining before joining it would mean waiting the full 60 s
        // deadline for a call we are about to wait for anyway, and then LEAKING
        // the engine on expiry. Nothing may wait on the finalizer path, so there
        // the join deadline is 0.
        ShutdownWorker(disposing);

        var outcome = HandleDrain.AlreadyClaimed;
        var h = _owner is null
            ? nint.Zero
            : _owner.TakeForDispose(nameof(JuceAudioEngine),
                                    disposing ? NativeHandleOwner.DefaultDrainTimeoutMs : 0, out outcome);
        if (h == nint.Zero)
        {
            // AlreadyClaimed: nothing to do. TimedOut: a native call is wedged
            // and TakeForDispose has already logged; the handle is leaked on
            // purpose. Either way there is no engine left to hand to the child
            // teardown, but the children still have to go — their own drains
            // are independent of ours.
            if (outcome == HandleDrain.TimedOut)
            {
                if (disposing) DisposeChildren();
                // …and the process-wide log registration goes with it. Leaving it
                // counted keeps dca_set_log_callback wired to a trampoline whose
                // last engine is gone, and the refcount then never reaches 0
                // again for anyone.
                UnregisterFromNativeLog(this);
            }
            return;
        }
        // Suppress finalization FIRST: even if a subsequent native call
        // throws, we must never run Dispose() twice on the same instance.
        GC.SuppressFinalize(this);
        _started = false;
        try { _ = NativeMethods.EngineStop(h); } catch { /* swallow on dispose */ }
        // …then every child, BEFORE dca_engine_destroy. Each child drains its
        // OWN in-flight native calls, so the 4 ms position poll finishes inside
        // dca_source_get_audible_position_seconds and the source is destroyed
        // before the engine whose transport it points at. The finalizer checks
        // completion without invoking or waiting for child finalizers.
        if (!DisposeChildren(disposing))
        {
            JuceLog.Emit(JuceLogLevel.Error,
                "JuceAudioEngine: child native destruction did not complete. LEAKING the engine to preserve child pointers.");
            // This is the NORMAL finalizer outcome, not an exotic one:
            // finalization order is undefined, so DisposeChildren(false) — which
            // only CHECKS completion tokens — routinely reports incomplete.
            // Returning here without unregistering leaked one log registration
            // per finalized engine, so s_logRegistrations never fell back to 0
            // and dca_set_log_callback(NULL) was never issued for the process.
            // The native engine is leaked deliberately; the managed registration
            // is not part of that bargain.
            UnregisterFromNativeLog(this);
            return;
        }
        // Decrement the process-wide log-callback refcount BEFORE destroying
        // the native engine. The static trampoline holds a strong reference
        // so leaving the slot wired while EngineDestroy runs is safe — the
        // slot is only cleared once no engines remain.
        UnregisterFromNativeLog(this);
        try { _ = NativeMethods.EngineDestroy(h); } catch { /* swallow */ }
    }

    /// <summary>
    /// Destroy the native engine for an instance nobody disposed. Tolerates a constructor
    /// that threw before <c>dca_engine_create</c> returned, and does not wait on in-flight
    /// calls — the process has one finalizer thread and every other finalizer queues
    /// behind this one.
    /// </summary>
    ~JuceAudioEngine() => Dispose(disposing: false);

    private static void RegisterForNativeLog(JuceAudioEngine engine)
    {
        lock (s_logRegistryLock)
        {
            // Compact dead weak refs while we're here.
            for (var i = s_activeEngines.Count - 1; i >= 0; i--)
            {
                if (!s_activeEngines[i].TryGetTarget(out _))
                    s_activeEngines.RemoveAt(i);
            }
            s_activeEngines.Add(new WeakReference<JuceAudioEngine>(engine));
            if (s_logRegistrations++ == 0)
                NativeMethods.SetLogCallback(s_staticLogTrampoline, nint.Zero);
        }
    }

    private static void UnregisterFromNativeLog(JuceAudioEngine engine)
    {
        lock (s_logRegistryLock)
        {
            for (var i = s_activeEngines.Count - 1; i >= 0; i--)
            {
                if (!s_activeEngines[i].TryGetTarget(out var target) || ReferenceEquals(target, engine))
                {
                    s_activeEngines.RemoveAt(i);
                }
            }
            if (s_logRegistrations > 0 && --s_logRegistrations == 0)
            {
                try { NativeMethods.SetLogCallback(null, nint.Zero); }
                catch { /* swallow on dispose */ }
            }
        }
    }

    private static void StaticOnNativeLog(nint _, NativeMethods.DcaLogLevel level, string msg)
    {
        // Never let a logger exception cross back into native code.
        try
        {
            var juceLevel = (JuceLogLevel)(int)level;
            if (juceLevel < JuceLog.MinimumLevel) return;

            var managed = juceLevel switch
            {
                JuceLogLevel.Trace => LogLevel.Trace,
                JuceLogLevel.Debug => LogLevel.Debug,
                JuceLogLevel.Info  => LogLevel.Information,
                JuceLogLevel.Warn  => LogLevel.Warning,
                JuceLogLevel.Error => LogLevel.Error,
                _                  => LogLevel.Information,
            };

            JuceAudioEngine[]? snapshot = null;
            lock (s_logRegistryLock)
            {
                if (s_activeEngines.Count > 0)
                {
                    snapshot = new JuceAudioEngine[s_activeEngines.Count];
                    var n = 0;
                    foreach (var weak in s_activeEngines)
                        if (weak.TryGetTarget(out var live)) snapshot[n++] = live;
                    if (n < snapshot.Length) Array.Resize(ref snapshot, n);
                }
            }
            if (snapshot != null)
            {
                foreach (var engine in snapshot)
                {
                    try { engine._log.Log(managed, "[native] {Message}", msg); }
                    catch (Exception ex)
                    {
                        Debug.WriteLine($"[JuceAudioEngine] ILogger threw: {ex}");
                    }
                }
            }

            JuceLog.Emit(juceLevel, msg);
        }
        catch (Exception ex)
        {
            // Last line of defence — must never propagate back to native.
            Debug.WriteLine($"[JuceAudioEngine] OnNativeLog crashed: {ex}");
        }
    }

    private static void VerifyAbi()
    {
        var actual = NativeMethods.AbiVersion();
        if (actual != NativeMethods.ExpectedAbiVersion)
            throw new JuceAbiMismatchException(NativeMethods.ExpectedAbiVersion, actual);
    }

    internal static void Check(NativeMethods.DcaStatus status, string op)
    {
        if (status == NativeMethods.DcaStatus.Ok) return;
        throw new JuceAudioException($"{op} failed with {status}.", status, op);
    }

    /// <summary>
    /// For fire-and-forget setters that intentionally never throw (volume,
    /// filters, clicks): keep the no-throw contract but stop swallowing the
    /// status — a persistent failure here used to be completely invisible.
    /// </summary>
    private void WarnIfFailed(NativeMethods.DcaStatus status, string op)
        => WarnIfFailed(_log, status, op);

    /// <summary>
    /// The same no-throw report for wrappers that own no <see cref="ILogger" />
    /// of their own (<see cref="JuceAudioPlugin" />). A null logger still emits
    /// through <see cref="JuceLog" />, which is the process-wide sink every
    /// native line already goes to — so a discarded status is visible from
    /// exactly one place whichever wrapper dropped it.
    /// </summary>
    internal static void WarnIfFailed(ILogger? log, NativeMethods.DcaStatus status, string op)
    {
        if (status == NativeMethods.DcaStatus.Ok) return;
        (log ?? NullLogger.Instance).LogWarning("[native] {Op} failed with {Status}", op, status);
        JuceLog.Emit(JuceLogLevel.Warn, $"{op} failed with {status}");
    }

    // Capture / peak polls run at up to 250 Hz on a timer thread. A per-call
    // LogWarning there would turn one broken seat into a log flood that costs
    // more than the fault, so the report is throttled to one line a second per
    // engine — enough to make a silent guitar visible, cheap enough to leave on.
    private long _lastPollWarnTicks = long.MinValue;

    private void WarnPollFailure(NativeMethods.DcaStatus status, string op)
    {
        if (status == NativeMethods.DcaStatus.Ok) return;
        var now = Environment.TickCount64;
        var last = Interlocked.Read(ref _lastPollWarnTicks);
        if (now - last < 1000) return;
        if (Interlocked.CompareExchange(ref _lastPollWarnTicks, now, last) != last) return;
        _log.LogWarning(
            "[native] {Op} failed with {Status} (throttled to one report per second)", op, status);
        JuceLog.Emit(JuceLogLevel.Warn, $"{op} failed with {status}");
    }

    /// <summary>
    /// A seat index outside <c>0..MaxPlayers-1</c> is a CALLER bug, and the
    /// per-seat reads used to swallow it: the native side returned
    /// <c>DCA_ERR_INVALID_ARG</c> without touching <c>written</c>/<c>peak</c>,
    /// the wrapper discarded the status and handed back 0 — which reads exactly
    /// like "the ring is empty", i.e. a guitar that is plugged in and silent.
    /// The native contract rejects such a seat rather than clamping it, and so
    /// does this: clamping would meter or judge somebody else's signal.
    /// </summary>
    private static void ValidatePlayer(int player, string paramName)
    {
        if (player < 0 || player >= NativeMethods.MaxPlayers)
        {
            throw new ArgumentOutOfRangeException(paramName, player,
                $"Seat index must be in [0, {NativeMethods.MaxPlayers}).");
        }
    }

    /// <summary>
    ///     Whether this engine still holds a share of the process-wide native log
    ///     registration. Test seam for the dispose/finalize accounting — asked per
    ///     ENGINE rather than as a count, because the count is process-global and
    ///     xunit runs collections in parallel, so a second engine in another
    ///     collection makes any before/after total a coin flip.
    ///     <para>
    ///         Membership and the refcount are written together by
    ///         <c>UnregisterFromNativeLog</c>, so this is a faithful proxy for
    ///         "s_logRegistrations was decremented".
    ///     </para>
    /// </summary>
    internal static bool IsRegisteredForNativeLog(JuceAudioEngine engine)
    {
        lock (s_logRegistryLock)
        {
            foreach (var weak in s_activeEngines)
                if (weak.TryGetTarget(out var live) && ReferenceEquals(live, engine)) return true;
            return false;
        }
    }
}
