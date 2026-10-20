using System.Text;
using JuceSharp.Interop;

namespace JuceSharp.Dsp;

/// <summary>
///     One module in a <see cref="JuceDspChain" />, addressed by its
///     ENGINE-GLOBAL node id. Obtained from
///     <see cref="JuceDspChain.AddNode(string)" /> or
///     <see cref="JuceDspChain.Nodes" />, never constructed.
///     <para>
///         Message-thread work, like the rest of the rig surface: parameters are
///         published to the audio thread through a mailbox, so a slider drag never
///         touches the callback with anything but a value. Every call pins the
///         engine handle, so one racing a dispose is a caught
///         <see cref="ObjectDisposedException" />.
///     </para>
/// </summary>
public sealed class JuceDspNode : IJuceDspNode
{
    private readonly JuceDspChain _chain;
    private readonly JuceAudioEngine _engine;
    private readonly string _createdTypeId;

    internal JuceDspNode(JuceDspChain chain, JuceAudioEngine engine, int id, string typeId)
    {
        _chain         = chain;
        _engine        = engine;
        Id             = id;
        _createdTypeId = typeId;
    }

    /// <summary>
    /// The engine-global node id: <c>player * PlayerNodeIdStride + local</c>.
    /// Stable for the life of the node, including across a
    /// <see cref="JuceDspChain.MoveNode(JuceDspNode, int)"/>.
    /// </summary>
    public int Id { get; }

    /// <summary>
    ///     The catalogue type id this node was created from, e.g. <c>"delay"</c>.
    ///     <para>
    ///         ABI v33 reads this from the ENGINE, so a node built by another
    ///         binding against the same engine — or found after re-attaching to a
    ///         live one — reports its real type instead of an empty string.
    ///         <b>There is no fast path</b>, and this doc claimed one until W4-C1:
    ///         every read is two P/Invokes (size, then copy) plus a
    ///         <see cref="byte" />[], and the id recorded at creation is returned
    ///         in exactly one case — the one the next paragraph describes. Cache
    ///         it caller-side if a UI reads it per frame; a node's type never
    ///         changes.
    ///     </para>
    ///     <para>
    ///         Empty when the engine has no such node, and empty rather than
    ///         guessed: a wrong type id would send a UI looking up the wrong
    ///         parameter list. Once the ENGINE is gone the type this node was
    ///         created with is reported instead — a node's type never changes, so
    ///         that answer is not stale, it is just the last one anybody can give.
    ///     </para>
    /// </summary>
    public string TypeId
    {
        get
        {
            // Poll-shaped, like LatencySamples and Meter: it does not throw out
            // of a property a ToString reads. A DISPOSED engine falls back to
            // the type id this node was created with (the best answer available,
            // and never a guess); a live engine that refuses the query reports
            // string.Empty. Neither is the literal "unknown" this comment
            // claimed until W5-D-low.
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return _createdTypeId;
            try
            {
                // Size-then-copy, the contract every out-buffer on this ABI uses.
                if (NativeMethods.EngineDspGetNodeType(h, Id, null, 0, out var len)
                    != NativeMethods.DcaStatus.Ok) return string.Empty;
                if (len <= 0) return string.Empty;
                var buf = new byte[len + 1];
                if (NativeMethods.EngineDspGetNodeType(h, Id, buf, buf.Length, out _)
                    != NativeMethods.DcaStatus.Ok) return string.Empty;
                return JuceDspModuleCatalog.Utf8(buf);
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>Which seat's chain holds this node, derived from
    /// <see cref="Id"/>.</summary>
    public int Player => Id / JuceAudioEngine.PlayerNodeIdStride;

    /// <summary>
    ///     Bypass this module. The chain CROSSFADES it over ~10 ms rather than
    ///     switching: bypass fades out, un-bypass fades in with the module already
    ///     running, which is what keeps an envelope follower warm across a
    ///     footswitch.
    ///     <para>
    ///         Un-bypassing a node whose fade-out has fully PARKED also resets the
    ///         module — true bypass. A parked node's delay lines and LFO phases are
    ///         frozen, not decaying, so fading that back in would replay audio from
    ///         before the bypass. A toggle INSIDE the 10 ms ramp is not a park and
    ///         keeps its state.
    ///     </para>
    ///     <para>
    ///         ABI v33: the getter asks the ENGINE, so a node bypassed through
    ///         another binding — or parked by an
    ///         <see cref="JuceDspChain.AddNodeBypassed" /> made elsewhere — reads
    ///         correctly. What it reports is the message-thread INTENT (the flag
    ///         last set), never the crossfade position: the ~10 ms ramp belongs to
    ///         the audio thread, and a getter that tracked it would flicker after
    ///         every toggle. A disposed engine reads false.
    ///     </para>
    /// </summary>
    public bool Bypass
    {
        get
        {
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return false;
            try
            {
                return NativeMethods.EngineDspGetBypass(h, Id, out var bypassed)
                       == NativeMethods.DcaStatus.Ok && bypassed != 0;
            }
            finally { _engine.Release(); }
        }
        set
        {
            var h = _engine.Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.EngineDspSetBypass(h, Id, value ? 1 : 0),
                    "dca_engine_dsp_set_bypass");
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>Read one float parameter.</summary>
    /// <exception cref="JuceAudioException">Unknown node or parameter id.</exception>
    public float GetParam(string paramId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(paramId);
        var h = _engine.Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.EngineDspGetParam(h, Id, paramId, out var value),
                $"dca_engine_dsp_get_param({paramId})");
            return value;
        }
        finally { _engine.Release(); }
    }

    /// <summary>
    ///     Set one float parameter. See <see cref="JuceDspParamInfo" /> for the
    ///     range and whether the param is discrete.
    ///     <para>
    ///         A NaN is refused HERE as well as native-side, and that redundancy is
    ///         deliberate: every core's clamp is NaN-transparent (<c>v &lt; lo</c>
    ///         and <c>v &gt; hi</c> are both false for a NaN), so a NaN that reached
    ///         the coefficients silences the module for good. Catching it in managed
    ///         code names the parameter that did it.
    ///     </para>
    /// </summary>
    /// <exception cref="ArgumentException"><paramref name="value"/> is NaN or infinite.</exception>
    /// <exception cref="JuceAudioException">Unknown node or parameter id.</exception>
    public void SetParam(string paramId, float value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(paramId);
        if (!float.IsFinite(value))
        {
            throw new ArgumentException(
                $"'{paramId}' was given {value}. A non-finite DSP parameter passes every clamp "
                + "(NaN compares false against both bounds) and reaches the coefficients, which "
                + "silences the module permanently.", nameof(value));
        }

        var h = _engine.Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.EngineDspSetParam(h, Id, paramId, value),
                $"dca_engine_dsp_set_param({paramId})");
        }
        finally { _engine.Release(); }
    }

    /// <summary>
    /// Read a string-valued parameter (a NAM capture path, a user IR WAV, a Miku
    /// phrase table). Returns an empty string when the value is unset; throws when
    /// the node or param id is unknown.
    /// </summary>
    public string GetString(string paramId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(paramId);
        var h = _engine.Acquire();
        try
        {
            // Size-then-copy: the first call returns the full byte length
            // excluding the NUL, or -1 for an unknown node/param.
            var len = NativeMethods.EngineDspGetNodeString(h, Id, paramId, null, 0);
            if (len < 0)
            {
                throw new JuceAudioException(
                    $"Node {Id} has no string parameter '{paramId}'.",
                    JuceAudioStatus.InvalidArg,
                    "dca_engine_dsp_get_node_string");
            }
            if (len == 0) return string.Empty;
            var buf = new byte[len + 1];
            // The COPY call's result was discarded, so a failure between the two
            // calls (the node removed, the param retired) decoded a zero-filled
            // buffer and returned "" — which is a LEGITIMATE value for a string
            // param, i.e. "unset". A second negative answer is the same fault the
            // sizing call already reports, and it is reported the same way.
            var copied = NativeMethods.EngineDspGetNodeString(h, Id, paramId, buf, buf.Length);
            if (copied < 0)
            {
                throw new JuceAudioException(
                    $"Node {Id}'s string parameter '{paramId}' went away between the sizing call "
                    + "and the copy.",
                    JuceAudioStatus.InvalidArg,
                    "dca_engine_dsp_get_node_string");
            }
            return JuceDspModuleCatalog.Utf8(buf);
        }
        finally { _engine.Release(); }
    }

    /// <summary>
    /// Set a string-valued parameter; pass null or empty to reset it to the
    /// module's built-in default (the bundled IR bank, the preset phrase
    /// pattern). Any heavy work the value implies — parsing a WAV, loading a
    /// neural capture — happens inside the module OFF the audio thread; the
    /// callback never blocks on it.
    /// </summary>
    public void SetString(string paramId, string? value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(paramId);
        var h = _engine.Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.EngineDspSetNodeString(h, Id, paramId, value ?? string.Empty),
                $"dca_engine_dsp_set_node_string({paramId})");
        }
        finally { _engine.Release(); }
    }

    /// <summary>
    ///     Which branch of a split/parallel region this node processes in.
    ///     <c>0</c> (the default) means branch 1; <c>1..3</c> select an explicit
    ///     branch. The tag is node state — it survives reorders — and is ignored
    ///     outside a region. An id outside <c>0..3</c> is rejected.
    /// </summary>
    public int Branch
    {
        get
        {
            var h = _engine.Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.EngineDspGetNodeBranch(h, Id, out var branch),
                    "dca_engine_dsp_get_node_branch");
                return branch;
            }
            finally { _engine.Release(); }
        }
        set
        {
            var h = _engine.Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.EngineDspSetNodeBranch(h, Id, value),
                    "dca_engine_dsp_set_node_branch");
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>
    ///     ABI v34. What this node's DETECTOR listens to. Only the detector
    ///     moves — the audio path, the crossfade and the scrub are unchanged.
    ///     <para>
    ///         <see cref="JuceDspSidechainSource.SeatInput" /> keys off the
    ///         seat's DRY input, so a pedal late in the chain follows the pick
    ///         rather than whatever the drive ahead of it produced;
    ///         <see cref="JuceDspSidechainSource.SongMix" /> keys off the song
    ///         as it stands when this seat is processed, which is how the rig
    ///         ducks under a backing track.
    ///     </para>
    ///     <para>
    ///         Only module types whose
    ///         <see cref="JuceDspModuleInfo.SupportsSidechain" /> is true accept
    ///         a non-<see cref="JuceDspSidechainSource.None" /> source; every
    ///         other one REJECTS it, because a source a module quietly ignored
    ///         would read as a live feature that does nothing. If the source is
    ///         unavailable when the block runs (no song mix on the offline
    ///         render path) the module keys off its own input — never off
    ///         silence, which on a gate would mean closed.
    ///     </para>
    /// </summary>
    /// <exception cref="JuceAudioException">This module type does not accept a
    /// key, or the source is not one of the three.</exception>
    public JuceDspSidechainSource Sidechain
    {
        get
        {
            var h = _engine.Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.EngineDspGetNodeSidechain(h, Id, out var source),
                    "dca_engine_dsp_get_node_sidechain");
                return (JuceDspSidechainSource)source;
            }
            finally { _engine.Release(); }
        }
        set
        {
            var h = _engine.Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.EngineDspSetNodeSidechain(h, Id, (int)value),
                    "dca_engine_dsp_set_node_sidechain");
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>
    /// Latency this node adds, in samples at the chain's prepared rate, or −1 when
    /// the engine cannot answer. A BYPASSED node reports 0 — it is not in the
    /// path, and a caller compensating for the rig must not budget for it. Most
    /// modules are 0; the pitch/neural family (harmonizer, whammy, klon, namamp's
    /// rate converter, the drop tuner) is not.
    /// </summary>
    public int LatencySamples
    {
        get
        {
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return -1;
            try
            {
                return NativeMethods.EngineDspGetNodeLatency(h, Id, out var samples) == NativeMethods.DcaStatus.Ok
                    ? samples
                    : -1;
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>
    /// This node's levels. All four are LINEAR amplitudes and read 0 while
    /// <see cref="JuceDspChain.MeteringEnabled"/> is false — which is how a caller
    /// tells "everything is silent" from "nobody switched the meters on". For a
    /// whole UI frame prefer <see cref="JuceDspChain.TryReadMeters(Span{JuceDspNodeMeter}, out int)"/>: one
    /// P/Invoke instead of one per pedal.
    /// </summary>
    public JuceDspNodeMeter Meter
    {
        get
        {
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return default;
            try
            {
                return NativeMethods.EngineDspGetNodeMeter(h, Id, out var ip, out var ir, out var op, out var orms)
                       == NativeMethods.DcaStatus.Ok
                    ? new JuceDspNodeMeter(ip, ir, op, orms)
                    : default;
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>Remove this node from its chain.</summary>
    public void Remove() => _chain.RemoveNode(this);

    /// <summary>Move this node to <paramref name="newIndex"/> in chain order.</summary>
    public void MoveTo(int newIndex) => _chain.MoveNode(this, newIndex);

    /// <inheritdoc />
    public override string ToString()
        => new StringBuilder("JuceDspNode(")
            .Append(TypeId is { Length: > 0 } t ? t : "<unknown type>")
            .Append(" #").Append(Id)
            .Append(", seat ").Append(Player)
            .Append(Bypass ? ", bypassed)" : ")")
            .ToString();
}
