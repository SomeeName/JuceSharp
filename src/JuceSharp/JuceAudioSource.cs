using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>Handle to a single playable audio source owned by the native engine.</summary>
public sealed class JuceAudioSource : IDisposable, INativeChild, IJuceAudioSource
{
    private readonly NativeDestruction _destruction = new();
    NativeDestruction INativeChild.Destruction => _destruction;

    // Drains in-flight native calls before destroy: position polling runs on
    // a timer thread while load/free disposes the source from elsewhere.
    //
    // Null ONLY when the constructor threw before it was assigned. The
    // finalizer still runs on that partially-constructed instance, so
    // DisposeNativeHandle has to tolerate it — a throwing finalizer is a
    // process-wide FailFast, not a catchable error. Same note as
    // JuceAudioEngine._owner and JuceAudioPlugin._owner.
    private readonly NativeHandleOwner? _owner;

    internal JuceAudioSource(nint handle)
    {
        if (handle == nint.Zero)
        {
            // SUPPRESS BEFORE THE THROW. The CLR queues the finalizer the moment
            // the allocation succeeds, so ~JuceAudioSource runs on a constructor
            // that threw — and a finalizer that throws is a process-wide
            // FailFast, not a catchable error. DisposeNativeHandle tolerates the
            // null `_owner` anyway; this is the belt to that pair of braces, and
            // it is the one wrapper ctor that did not have it. Same shape as
            // JuceAudioPlugin / JuceAudioEngine / JuceMidiInput.
            GC.SuppressFinalize(this);
            throw new ArgumentException("A native source handle must be non-zero.", nameof(handle));
        }
        _owner = new NativeHandleOwner(handle);
    }

    /// <summary>True once <see cref="Dispose()" /> has claimed the native handle
    /// — every member then throws <see cref="ObjectDisposedException" />. Poll
    /// this rather than catching, when a corpse is a normal outcome (a source
    /// handed back by <see cref="JuceAudioEngine.OpenFileAsync" /> whose engine
    /// was disposed while the load ran).</summary>
    public bool IsDisposed => !(_owner?.IsAlive ?? false);

    /// <summary>Pin the handle for one native call. Pair with
    /// <see cref="Release"/> in a finally.</summary>
    private nint Acquire()
        => (_owner ?? throw new ObjectDisposedException(nameof(JuceAudioSource)))
            .Acquire(nameof(JuceAudioSource));

    private void Release() => _owner?.Release();

    /// <summary>Pins this source's handle for an ENGINE call that names it
    /// (<c>dca_engine_set_song_clock_source</c>). Pair with
    /// <see cref="ReleaseForEngineCall" /> in a finally.</summary>
    internal nint AcquireForEngineCall() => Acquire();

    /// <summary>Ends a pin taken by <see cref="AcquireForEngineCall" />.</summary>
    internal void ReleaseForEngineCall() => Release();

    /// <summary>
    /// Transport read position (<c>dca_source_get_position</c>) — where the source is
    /// reading the file, which at a tempo other than 1.0 runs AHEAD of what is being
    /// heard. Use <see cref="AudiblePosition"/> to drive a playhead. Safe to poll from a
    /// timer thread while another thread disposes the source: the call is drained, so the
    /// race surfaces as <see cref="ObjectDisposedException"/> rather than a native crash.
    /// </summary>
    public TimeSpan Position
    {
        get
        {
            var h = Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.SourceGetPosition(h, out var s), "source_get_position");
                return TimeSpan.FromSeconds(s);
            }
            finally { Release(); }
        }
    }

    /// <summary>Audible playback position — same as <see cref="Position"/>
    /// when tempo == 1.0, but compensates for the Rubber Band stretcher's
    /// internal read-ahead when tempo &lt;&gt; 1.0 so the reported playhead
    /// matches what's actually being heard.</summary>
    public TimeSpan AudiblePosition
    {
        get
        {
            var h = Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.SourceGetAudiblePosition(h, out var s), "source_get_audible_position");
                return TimeSpan.FromSeconds(s);
            }
            finally { Release(); }
        }
    }

    /// <summary>Total playing time of the loaded file (<c>dca_source_get_length</c>),
    /// unaffected by the tempo stretch. <see cref="TimeSpan.Zero"/> when nothing is
    /// loaded.</summary>
    public TimeSpan Length
    {
        get
        {
            var h = Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.SourceGetLength(h, out var s), "source_get_length");
                return TimeSpan.FromSeconds(s);
            }
            finally { Release(); }
        }
    }

    /// <summary>
    /// A fingerprint of what this source plays from RAM
    /// (<c>dca_source_get_preload_digest</c>, ABI v43): equal digests mean
    /// bit-identical samples and alignment. Hashes the whole song, so tests and
    /// diagnostics only. Throws <see cref="JuceAudioException"/> for a STREAMING
    /// source, which holds nothing to hash.
    /// </summary>
    public JucePreloadDigest GetPreloadDigest()
    {
        var h = Acquire();
        try
        {
            JuceAudioEngine.Check(
                NativeMethods.SourceGetPreloadDigest(h, out var hash, out var step, out var frames, out var rate,
                    out var converted),
                "source_get_preload_digest");
            return new JucePreloadDigest(hash, step, frames, rate, converted != 0);
        }
        finally { Release(); }
    }

    /// <summary>Start or resume playback from the current position
    /// (<c>dca_source_play</c>).</summary>
    public void Play()
    {
        var h = Acquire();
        try { JuceAudioEngine.Check(NativeMethods.SourcePlay(h), "source_play"); }
        finally { Release(); }
    }

    /// <summary>Stop pulling audio but keep the current position, so a later
    /// <see cref="Play"/> resumes where it left off (<c>dca_source_pause</c>).</summary>
    public void Pause()
    {
        var h = Acquire();
        try { JuceAudioEngine.Check(NativeMethods.SourcePause(h), "source_pause"); }
        finally { Release(); }
    }

    /// <summary>Stop playback and rewind to the start
    /// (<c>dca_source_stop</c>).</summary>
    public void Stop()
    {
        var h = Acquire();
        try { JuceAudioEngine.Check(NativeMethods.SourceStop(h), "source_stop"); }
        finally { Release(); }
    }

    /// <summary>Move the transport to <paramref name="position"/>
    /// (<c>dca_source_seek</c>), flushing the stretcher so the next audible sample comes
    /// from there. Works whether or not the source is playing.</summary>
    /// <param name="position">Target position from the start of the file.</param>
    public void Seek(TimeSpan position)
    {
        var h = Acquire();
        try { JuceAudioEngine.Check(NativeMethods.SourceSeek(h, position.TotalSeconds), "source_seek"); }
        finally { Release(); }
    }

    /// <summary>
    ///     Held across the FIELD write and the native store in both level
    ///     setters, so the two can never desync. Without it (concurrency audit
    ///     #10) the mixer's solo refresh could enter <see cref="Mute" /> and read
    ///     <c>_userVolume == 1.0</c>, the UI slider could write 0.3 and store 0.3,
    ///     and the refresh then stored 1.0 — native at full volume while the
    ///     getter reported 0.3, until the next write. <c>dca_source_set_volume</c>
    ///     is a lock-free native store, so this holds nothing across a blocking
    ///     P/Invoke; it is taken INSIDE the handle pin so a drain still completes.
    /// </summary>
    private readonly object _levelGate = new();

    /// <summary>
    ///     Linear gain, <c>[0, +inf)</c>. 1 is unity; above 1 is a real boost and is
    ///     supported (a stem fader at +6 dB is linear 2). A NEGATIVE value is
    ///     CLAMPED TO 0, here and natively, because a negative gain is a phase
    ///     inversion rather than a quieter source — it cancels against anything
    ///     correlated already in the mix and reads as a hollow mix, not as a wrong
    ///     level. The clamp is mirrored managed-side so this cached getter reports
    ///     what the engine actually runs.
    /// </summary>
    public float Volume
    {
        get => _userVolume;
        set
        {
            // A NaN is deliberately NOT clamped: it is passed through so the
            // native rejection surfaces as a JuceAudioException naming the call,
            // rather than being silently laundered into a muted source.
            var clamped = value < 0f ? 0f : value;
            var h = Acquire();
            try
            {
                lock (_levelGate)
                {
                    _userVolume = clamped;
                    JuceAudioEngine.Check(NativeMethods.SourceSetVolume(h, _muted ? 0f : clamped),
                        "source_set_volume");
                }
            }
            finally { Release(); }
        }
    }

    /// <summary>Mute / un-mute this source. Internally drives the volume
    /// to zero while remembering the user-set value so unmute restores it.</summary>
    public bool Mute
    {
        get => _muted;
        set
        {
            var h = Acquire();
            try
            {
                lock (_levelGate)
                {
                    if (_muted == value) return;
                    _muted = value;
                    JuceAudioEngine.Check(NativeMethods.SourceSetVolume(h, value ? 0f : _userVolume), "source_set_volume");
                }
            }
            finally { Release(); }
        }
    }

    /// <summary>
    ///     Optional A/B loop region. When the loop is active and the engine
    ///     reports an audible position past <see cref="JuceLoopRegion.End" />, the
    ///     source is seeked back to <see cref="JuceLoopRegion.Start" />.
    ///     Polling-based; precision is the configured
    ///     <see cref="JuceLoopRegion.PollIntervalMs" /> (default 5 ms).
    ///     <para>
    ///         Assigning the SAME region back (<c>src.Loop = src.Loop</c>, which a
    ///         "refresh the loop" UI does by accident all the time) is now a no-op.
    ///         It used to dispose the region and then re-attach it, and
    ///         <see cref="JuceLoopRegion.Attach" /> on a disposed region tears its
    ///         own timer straight back down — so the loop silently stopped looping
    ///         while the property still reported one.
    ///     </para>
    ///     <para>
    ///         The swap is a single <see cref="Interlocked.Exchange{T}" />: two
    ///         threads setting this concurrently used to read the old value, both
    ///         dispose it, and both store — leaking one live 5 ms timer per race.
    ///     </para>
    ///     <para>
    ///         <b>The ATTACH happens first, and the order is load-bearing.</b>
    ///         <see cref="JuceLoopRegion.Attach" /> throws when the region is
    ///         already bound to a different source; the setter used to store the
    ///         new region and dispose the old one BEFORE finding that out, so on
    ///         that throw the property reported a loop, the old loop was dead,
    ///         the new one was never armed, and nothing was looping — the exact
    ///         failure the paragraph above says was fixed. A rejected region now
    ///         leaves the existing loop running and untouched.
    ///     </para>
    /// </summary>
    /// <exception cref="InvalidOperationException">The region is already attached
    /// to a DIFFERENT source. A region owns one source for life; the source's
    /// current loop is left in place.</exception>
    public JuceLoopRegion? Loop
    {
        get => _loop;
        set
        {
            if (ReferenceEquals(Volatile.Read(ref _loop), value)) return;
            // Throws here => nothing has been mutated yet.
            value?.Attach(this);
            var previous = Interlocked.Exchange(ref _loop, value);
            previous?.Dispose();
        }
    }

    private float _userVolume = 1f;
    private bool  _muted;
    private JuceLoopRegion? _loop;
    private float _tempoRatio = 1f;
    private float _pitchSemitones;
    private float _pan;

    /// <summary>
    ///     Time-stretch ratio (1.0 = normal speed). Cached managed value — the
    ///     native side does not expose a read API.
    ///     <para>
    ///         <b>IGNORED while <see cref="StemGrouped" /> is true</b>, and the
    ///         cache is not moved either. The header is explicit that per-source
    ///         set_tempo / set_pitch are no-ops while grouped (the group stretcher
    ///         owns those) and <c>dca_source_set_tempo</c> answers <c>DCA_OK</c>
    ///         regardless — so the setter used to return success and the getter
    ///         then reported 1.4 while the source played at whatever
    ///         <see cref="JuceAudioEngine.StemGroupTempo" /> said (W4-M6). The
    ///         write is dropped with a Warn naming the member to use instead,
    ///         rather than thrown: a stem mixer sweeping every channel from one
    ///         slider is an ordinary shape and must not fault. This is the same
    ///         "a cached getter must report what the engine actually runs" rule
    ///         <see cref="Volume" /> and <see cref="Pan" /> already follow.
    ///     </para>
    /// </summary>
    public float TempoRatio
    {
        get => _tempoRatio;
        set
        {
            if (IgnoredWhileGrouped(nameof(TempoRatio), nameof(JuceAudioEngine.StemGroupTempo))) return;
            var h = Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.SourceSetTempo(h, value), "source_set_tempo");
                _tempoRatio = value;
            }
            finally { Release(); }
        }
    }

    /// <summary>Pitch shift in semitones. Cached managed value. IGNORED while
    /// <see cref="StemGrouped" /> is true, for the reason spelled out on
    /// <see cref="TempoRatio" /> — use
    /// <see cref="JuceAudioEngine.StemGroupPitchSemitones" />.</summary>
    public float PitchSemitones
    {
        get => _pitchSemitones;
        set
        {
            if (IgnoredWhileGrouped(nameof(PitchSemitones),
                                    nameof(JuceAudioEngine.StemGroupPitchSemitones))) return;
            var h = Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.SourceSetPitchSemitones(h, value), "source_set_pitch");
                _pitchSemitones = value;
            }
            finally { Release(); }
        }
    }

    /// <summary>True when this source is in the stem group, in which case the
    /// caller's write is dropped and logged instead of being accepted into a
    /// cache the engine is not honouring.</summary>
    private bool IgnoredWhileGrouped(string member, string groupMember)
    {
        if (!_stemGrouped) return false;
        JuceLog.Emit(JuceLogLevel.Warn,
            $"JuceAudioSource.{member} was set while StemGrouped is true. The engine ignores "
            + $"per-source tempo/pitch for a grouped stem — the group stretcher owns them — so "
            + $"this write is dropped rather than cached. Use JuceAudioEngine.{groupMember}.");
        return true;
    }

    /// <summary>
    ///     Stereo position, <c>[-1, +1]</c> (-1 = hard left). A value outside that
    ///     range is CLAMPED, not rejected — a caller dragging past the end wants the
    ///     hard left it asked for. The clamp is mirrored from the native side so
    ///     this cached getter cannot report a position the engine is not playing.
    /// </summary>
    public float Pan
    {
        get => _pan;
        set
        {
            var clamped = value < -1f ? -1f : (value > 1f ? 1f : value);
            var h = Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.SourceSetPan(h, clamped), "source_set_pan");
                _pan = clamped;
            }
            finally { Release(); }
        }
    }

    private bool _stemGrouped;

    /// <summary>
    /// When true this source is routed into the engine's shared stem-group
    /// bus: its own Rubber Band stretcher is bypassed and tempo / pitch are
    /// applied to the SUM of all grouped sources by ONE shared stretcher.
    /// Use for stem mixers (vocals / drums / bass / etc decomposed from the
    /// same recording) — independent per-stem stretchers introduce audible
    /// phasey artefacts at heavy slowdown because their analysis windows are
    /// not aligned, so summing the outputs re-creates comb-filter-style
    /// cancellations. Setting tempo / pitch on this source is a no-op while
    /// grouped — use <see cref="JuceAudioEngine.StemGroupTempo"/> and
    /// <see cref="JuceAudioEngine.StemGroupPitchSemitones"/> instead.
    /// </summary>
    public bool StemGrouped
    {
        get => _stemGrouped;
        set
        {
            var h = Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.SourceSetStemGrouped(h, value ? 1 : 0), "source_set_stem_grouped");
                _stemGrouped = value;
            }
            finally { Release(); }
        }
    }

    /// <summary>
    /// Dispose the loop region, then drain any in-flight native call and destroy the
    /// source. The drain WAITS on the last release rather than freeing under a live call;
    /// if it expires the handle is logged at Error and leaked, because a leak beats a
    /// use-after-free. Idempotent.
    /// </summary>
    public void Dispose()
    {
        // Managed cleanup — safe only when called explicitly (not from finalizer).
        Interlocked.Exchange(ref _loop, null)?.Dispose();
        DisposeNativeHandle(disposing: true);
        GC.SuppressFinalize(this);
    }

    private void DisposeNativeHandle(bool disposing)
    {
        // TakeForDispose blocks new acquires and drains in-flight calls
        // (bounded) so the native object is never freed under a running
        // P/Invoke from the position-polling thread.
        // `_owner?` — never `_owner.` — because the finalizer reaches here on a
        // partially-constructed instance whose fields are all still null.
        //
        // THE FINALIZER MAY NOT WAIT (deadline 0). One finalizer thread serves
        // the whole process, so waiting the full 60 s here stalls every other
        // finalizer behind it. On that path a call still in flight means the
        // handle is leaked and logged at Error — the trade this drain exists to
        // make.
        _destruction?.Run(() =>
        {
            if (_owner is null) return true;
            var h = _owner.TakeForDispose(nameof(JuceAudioSource),
                disposing ? NativeHandleOwner.DefaultDrainTimeoutMs : 0, out _);
            return h != nint.Zero && NativeMethods.SourceDestroy(h) == NativeMethods.DcaStatus.Ok;
        });
    }

    /// <summary>
    /// Last-resort release of the native handle for a source nobody disposed. Touches no
    /// other managed object and tolerates a constructor that threw before the handle
    /// owner existed — a throwing finalizer is a process-wide FailFast. Unlike
    /// <see cref="Dispose()"/> it does NOT wait for in-flight calls (one finalizer thread
    /// serves the whole process), so a call still running means the handle is leaked.
    /// </summary>
    ~JuceAudioSource()
    {
        // Finalizer must not touch other managed objects; only release the
        // native handle. The loop region (managed) will be collected on its
        // own finalisation pass.
        DisposeNativeHandle(disposing: false);
    }
}
