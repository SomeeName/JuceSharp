namespace JuceSharp;

/// <summary>
/// Lightweight managed mixer-style facade over a set of
/// <see cref="JuceAudioSource"/> instances. Provides per-source mute /
/// solo, group volume, and bulk transport so charting tools can
/// treat a multi-stem project as one logical unit.
///
/// Audio still flows through the native engine — this is purely a
/// convenience wrapper that translates "solo" into "everyone else mute"
/// and forwards group transport commands.
/// </summary>
public sealed class JuceMixer : IDisposable
{
    private readonly List<JuceMixerChannel> _channels = new();
    private readonly object _gate = new();
    private bool _disposed;

    /// <summary>Snapshot of the channels currently in the mixer, in the order they
    /// were added. Each read copies the list under the mixer's lock, so the result is
    /// safe to enumerate while another thread adds or removes a channel.</summary>
    public IReadOnlyList<JuceMixerChannel> Channels
    {
        get { lock (_gate) return _channels.ToArray(); }
    }

    /// <summary>Add a source to the mixer. The mixer assumes ownership
    /// only for mute/solo bookkeeping; disposal of the source is up to
    /// the caller (or pass <c>ownsSource: true</c> to have the mixer
    /// dispose it).
    /// <para>The new channel is folded into the mixer's solo state before this
    /// returns: adding a stem while a solo is up used to leave it UNMUTED and
    /// playing over the soloed one, which is the ordinary case for a
    /// stem-separation UI that adds stems as they finish decoding.</para></summary>
    public JuceMixerChannel Add(string name, JuceAudioSource source, bool ownsSource = false)
    {
        ArgumentNullException.ThrowIfNull(name);
        ArgumentNullException.ThrowIfNull(source);
        EnsureAlive();
        var ch = new JuceMixerChannel(this, name, source, ownsSource);
        lock (_gate) _channels.Add(ch);
        // Outside _gate, like Remove: the refresh takes _soloGate and stores
        // through every channel's source.
        RefreshSoloMuting();
        return ch;
    }

    /// <summary>Remove a channel from the mixer. If the channel was added
    /// with <c>ownsSource: true</c> its underlying source is disposed.
    /// Re-applies solo state to the remaining channels.</summary>
    public bool Remove(JuceMixerChannel channel)
    {
        ArgumentNullException.ThrowIfNull(channel);
        EnsureAlive();
        bool removed;
        lock (_gate) removed = _channels.Remove(channel);
        if (removed)
        {
            channel.DisposeOwned();
            RefreshSoloMuting();
        }
        return removed;
    }

    /// <summary>Start every source, muted ones included — mute is a GAIN here
    /// (<see cref="JuceAudioSource.Mute" /> writes <c>source_set_volume(0)</c>), not a
    /// transport state, so a muted stem keeps running and stays in sync with the
    /// rest of the mix.</summary>
    public void Play()  { EnsureAlive(); foreach (var c in Snapshot()) c.Source.Play(); }
    /// <summary>Pause every source, leaving each at its current position.</summary>
    public void Pause() { EnsureAlive(); foreach (var c in Snapshot()) c.Source.Pause(); }

    /// <summary>Stop every source and rewind it to the start.</summary>
    public void Stop()  { EnsureAlive(); foreach (var c in Snapshot()) c.Source.Stop(); }

    /// <summary>Seek every source to the same position.</summary>
    public void Seek(TimeSpan position)
    {
        EnsureAlive();
        foreach (var c in Snapshot()) c.Source.Seek(position);
    }

    /// <summary>
    ///     Serializes a channel's flag write with the whole-mixer recompute it
    ///     triggers. Two toggles on two threads used to interleave: A computed
    ///     its mute states, B computed and applied newer ones, then A applied
    ///     its stale set over them (concurrency audit #10). Held across the
    ///     per-source stores, which are lock-free native writes.
    /// </summary>
    private readonly object _soloGate = new();

    internal void RefreshSoloMuting()
    {
        lock (_soloGate)
        {
            var snap = Snapshot();
            var states = new bool[snap.Length];
            ComputeMuteStates(snap.Length, i => snap[i].MutedByUser, i => snap[i].Solo, states);
            for (var i = 0; i < snap.Length; i++)
                snap[i].Source.Mute = states[i];
        }
    }

    /// <summary>A channel flag write plus the refresh it implies, as one unit.</summary>
    internal void SetChannelFlag(Action write)
    {
        lock (_soloGate)
        {
            write();
            RefreshSoloMuting();
        }
    }

    /// <summary>Pure helper exposed for testing. Returns the effective
    /// mute state for each channel: muted by user OR (anyone soloed AND this channel not soloed).</summary>
    public static bool[] ComputeMuteStates(IReadOnlyList<bool> mutedByUser, IReadOnlyList<bool> solo)
    {
        ArgumentNullException.ThrowIfNull(mutedByUser);
        ArgumentNullException.ThrowIfNull(solo);
        if (mutedByUser.Count != solo.Count)
            throw new ArgumentException("mutedByUser and solo must have the same length.");
        var result = new bool[mutedByUser.Count];
        ComputeMuteStates(result.Length, i => mutedByUser[i], i => solo[i], result);
        return result;
    }

    private static void ComputeMuteStates(int count, Func<int, bool> muted, Func<int, bool> solo, bool[] result)
    {
        var anySolo = false;
        for (var i = 0; i < count; i++)
            if (solo(i)) { anySolo = true; break; }
        for (var i = 0; i < count; i++)
            result[i] = muted(i) || (anySolo && !solo(i));
    }

    private JuceMixerChannel[] Snapshot()
    {
        lock (_gate) return _channels.ToArray();
    }

    /// <summary>Empty the mixer and dispose every channel that was added with
    /// <c>ownsSource: true</c>; sources the caller still owns are left alone. Idempotent,
    /// and every later call on the mixer throws <see cref="ObjectDisposedException"/>.</summary>
    public void Dispose()
    {
        JuceMixerChannel[] toDispose;
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            toDispose = _channels.ToArray();
            _channels.Clear();
        }
        foreach (var c in toDispose) c.DisposeOwned();
    }

    private void EnsureAlive()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(JuceMixer));
    }
}

/// <summary>A single channel inside a <see cref="JuceMixer"/>.</summary>
public sealed class JuceMixerChannel
{
    private readonly JuceMixer _owner;
    private readonly bool      _ownsSource;
    private bool _mutedByUser;
    private bool _solo;

    /// <summary>The caller-supplied label this channel was added under. Not unique and
    /// not interpreted by the mixer.</summary>
    public string          Name   { get; }

    /// <summary>The native-backed source this channel drives. Still owned by whoever
    /// added it unless the channel was added with <c>ownsSource: true</c>.</summary>
    public JuceAudioSource Source { get; }

    /// <summary>Whether the user has explicitly muted this channel. Distinct from being
    /// silenced because some OTHER channel is soloed: setting this recomputes the whole
    /// mixer's mute state under the mixer's lock, so two toggles on two threads cannot
    /// interleave into a stale result.</summary>
    public bool MutedByUser
    {
        get => _mutedByUser;
        set => _owner.SetChannelFlag(() => _mutedByUser = value);
    }

    /// <summary>Whether this channel is soloed. While any channel in the mixer is soloed,
    /// every channel that is not is muted; clearing the last solo restores each channel's
    /// own <see cref="MutedByUser"/> state.</summary>
    public bool Solo
    {
        get => _solo;
        set => _owner.SetChannelFlag(() => _solo = value);
    }

    /// <summary>Linear gain of the underlying source, 1.0 being unity. Clamped by the
    /// engine to <c>[0, +inf)</c> — a negative value would be a phase inversion, not a
    /// quieter channel — with no upper bound, so boost above unity is allowed.</summary>
    public float Volume { get => Source.Volume; set => Source.Volume = value; }

    /// <summary>Stereo position of the underlying source, −1 hard left to +1 hard right,
    /// clamped at the ABI boundary as well as inside the panner.</summary>
    public float Pan    { get => Source.Pan;    set => Source.Pan    = value; }

    internal JuceMixerChannel(JuceMixer owner, string name, JuceAudioSource source, bool ownsSource)
    {
        _owner     = owner;
        _ownsSource = ownsSource;
        Name       = name;
        Source     = source;
    }

    internal void DisposeOwned()
    {
        if (_ownsSource) Source.Dispose();
    }
}
