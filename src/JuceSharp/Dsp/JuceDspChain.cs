using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.InteropServices;
using JuceSharp.Interop;

namespace JuceSharp.Dsp;

/// <summary>
///     One seat's in-house DSP tone rig: a reorderable effect chain applied to
///     that seat's guitar INPUT, before any user VST input chain. Obtained from
///     <see cref="JuceAudioEngine.GetDspChain" />, never constructed.
///     <para>
///         <b>Message-thread only.</b> Every call here is chain STRUCTURE or
///         PARAMETER work: the native side does it on the calling thread and hands
///         the audio callback an atomic ordering snapshot, so nothing below ever
///         runs on the audio thread. It is safe from any thread and safe against a
///         running device, but it is not a real-time API — do not call it from an
///         audio callback of your own.
///     </para>
///     <para>
///         <b>Structure changes are CROSSFADED by the chain</b>, over ~10 ms:
///         bypass fades out, un-bypass fades in with the module running, a remove
///         waits for its fade-out before the module is retired, and a move fades
///         out at the old slot and in at the new. Splitter/merger topology switches
///         instantly — that is routing, not level. So a call here returning does
///         not mean the change is fully audible yet, by design.
///     </para>
///     <para>
///         Every call pins the engine handle for its duration, so one racing an
///         engine <see cref="JuceAudioEngine.Dispose()" /> surfaces as a caught
///         <see cref="ObjectDisposedException" /> rather than a native crash. That
///         is the whole reason to use this instead of binding
///         <c>dca_engine_dsp_*</c> yourself against
///         <c>JuceAudioEngine.NativeHandle</c>, which is unpinned.
///     </para>
/// </summary>
public sealed partial class JuceDspChain
{
    private readonly JuceAudioEngine _engine;

    // Node ids the engine hands out are stable for the node's life, so the same
    // JuceDspNode instance can be handed back for one. Since ABI v33 the type id
    // and the bypass flag are read from the ENGINE rather than remembered here,
    // so a node this chain did NOT create is no longer a second-class citizen;
    // this map is identity, not state. See JuceDspNode.TypeId.
    private readonly ConcurrentDictionary<int, JuceDspNode> _known = new();

    // ---- edit-scope journal (findings #11 / #12) -----------------------------
    //
    // `_known` is the wrapper's IDENTITY map, and an ABORTED scope used to leave
    // it disagreeing with the engine in BOTH directions:
    //
    //   * a node ADDED inside the scope is destroyed natively by abortEdit, but
    //     stayed in `_known` for ever — so TryGetNode kept answering `true` with
    //     a wrapper for a node that no longer exists, whose throwing members
    //     raised and whose POLL-shaped ones (Bypass/TypeId/Meter/LatencySamples)
    //     answered false/""/default/-1, i.e. exactly what a real, bypassed,
    //     silent pedal looks like to a UI. `_known` also grew unbounded there.
    //   * a node REMOVED (or cleared) inside the scope is put straight back by
    //     abortEdit, but its wrapper had already been evicted — so the caller's
    //     own JuceDspNode object stopped being the one this chain hands out,
    //     breaking the reference-identity promise on `Nodes`/`TryGetNode` and
    //     losing the created-type-id fast path.
    //
    // So the scope journals both halves and the abort replays them. Only
    // membership is journalled; parameter VALUES are not rolled back natively
    // either, and this must not pretend otherwise.
    private readonly object _editJournalGate = new();
    private List<int>? _editAdded;
    private Dictionary<int, JuceDspNode>? _editEvicted;

    // Identity of the scope currently open on this chain, held WEAKLY: the whole
    // point is to notice that nobody else is holding it.
    private WeakReference<JuceDspEditScope>? _openScope;

    // …and its ID, which is the AUTHORITY. A short weak reference is cleared
    // before the finalizer runs, so object identity cannot answer "is the scope
    // whose finalizer is running still the current one?" — see ClaimScope.
    // 0 means no scope is open.
    private long _openScopeId;

    private static long s_scopeSequence;

    internal JuceDspChain(JuceAudioEngine engine, int player)
    {
        _engine = engine;
        Player  = player;
    }

    /// <summary>Which seat this chain belongs to. Seat 0 is the solo player and
    /// is byte-for-byte the pre-ABI-v28 signal path.</summary>
    public int Player { get; }

    /// <summary>
    ///     Turn a LOCAL index into the ENGINE-GLOBAL node id this seat uses:
    ///     <c>player * <see cref="JuceAudioEngine.PlayerNodeIdStride" /> + local</c>.
    ///     Exposed so the arithmetic is never re-typed by a caller — every id this
    ///     type returns or accepts is already global, so you should not need it.
    /// </summary>
    public int ToGlobalNodeId(int localNodeId)
        => Player * JuceAudioEngine.PlayerNodeIdStride + localNodeId;

    /// <summary>
    ///     Reject an engine-global node id belonging to ANOTHER seat (W4-M8).
    ///     Node ids are engine-global and the NODE-scoped exports resolve the
    ///     seat from the id, which is what kept the ABI v28 delta small — but it
    ///     also means <c>GetDspChain(0).RemoveNode(2_000_003)</c> removed seat 2's
    ///     third pedal from an object whose <see cref="Player" /> says 0 and whose
    ///     type doc says "one seat's rig". Worse than the wrong pedal: the
    ///     removal skipped this chain's <c>_known</c> map and the edit journal, so
    ///     it was also un-rollback-able.
    ///     <para>
    ///         Thrown, never clamped — the same answer the engine gives an id
    ///         naming a seat past <c>DCA_MAX_PLAYERS</c>.
    ///     </para>
    /// </summary>
    private void ValidateOwnNode(int nodeId, string paramName)
    {
        if (nodeId >= 0 && nodeId / JuceAudioEngine.PlayerNodeIdStride == Player) return;
        throw new ArgumentOutOfRangeException(
            paramName, nodeId,
            $"Node id {nodeId} belongs to seat {(nodeId < 0 ? -1 : nodeId / JuceAudioEngine.PlayerNodeIdStride)}, "
            + $"not to this chain's seat {Player}. Node ids are engine-global "
            + $"(seat * {JuceAudioEngine.PlayerNodeIdStride} + local); use GetDspChain(thatSeat).");
    }

    // ---------------------------------------------------------------- structure

    /// <summary>
    /// Append a module of <paramref name="moduleTypeId"/> (see
    /// <see cref="JuceDspModuleCatalog"/>) at the end of the chain, FULLY WET
    /// immediately. Use <see cref="AddNodeBypassed"/> when the node still has to
    /// be configured.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The engine is gone.</exception>
    /// <exception cref="JuceAudioException">Unknown module type, or the engine
    /// refused the seat.</exception>
    public JuceDspNode AddNode(string moduleTypeId) => Add(moduleTypeId, parked: false);

    /// <summary>
    ///     ABI v32: append a PARKED module — created at zero wet, so it is
    ///     INAUDIBLE until <see cref="JuceDspNode.Bypass" /> is set to false, which
    ///     fades it in over ~10 ms with its parameters already in place.
    ///     <para>
    ///         This is the live-preset-construction shape: add parked, set every
    ///         param and string, then un-bypass. Adding wet and configuring
    ///         afterwards means the user hears the module's DEFAULTS first. It does
    ///         not make a whole preset change atomic — splitter/merger topology is
    ///         still immediate.
    ///     </para>
    /// </summary>
    public JuceDspNode AddNodeBypassed(string moduleTypeId) => Add(moduleTypeId, parked: true);

    private JuceDspNode Add(string moduleTypeId, bool parked)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(moduleTypeId);
        var h = _engine.Acquire();
        try
        {
            var id = parked
                ? NativeMethods.EngineDspAddNodeBypassedForPlayer(h, Player, moduleTypeId)
                : NativeMethods.EngineDspAddNodeForPlayer(h, Player, moduleTypeId);
            if (id < 0)
            {
                throw new JuceAudioException(
                    $"Could not add a '{moduleTypeId}' node to seat {Player} (returned {id}: unknown module type "
                    + "or a seat the engine does not have).",
                    JuceAudioStatus.InvalidArg,
                    parked ? "dca_engine_dsp_add_node_bypassed_for_player" : "dca_engine_dsp_add_node_for_player");
            }
            var node = new JuceDspNode(this, _engine, id, moduleTypeId);
            _known[id] = node;
            JournalAdded(id);
            return node;
        }
        finally { _engine.Release(); }
    }

    /// <summary>Remove a node. The chain fades it out first and retires the
    /// module only once the fade has landed, so a delay does not click off.</summary>
    public void RemoveNode(JuceDspNode node)
    {
        ArgumentNullException.ThrowIfNull(node);
        RemoveNode(node.Id);
    }

    /// <summary>Remove a node by its engine-global id.</summary>
    /// <exception cref="ArgumentOutOfRangeException">The id belongs to another
    /// seat — see <see cref="Player" />.</exception>
    public void RemoveNode(int nodeId)
    {
        ValidateOwnNode(nodeId, nameof(nodeId));
        var h = _engine.Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.EngineDspRemoveNode(h, nodeId), "dca_engine_dsp_remove_node");
            if (_known.TryRemove(nodeId, out var evicted)) JournalEvicted(nodeId, evicted);
        }
        finally { _engine.Release(); }
    }

    /// <summary>
    /// Move a node to <paramref name="newIndex"/> in chain order. A reorder never
    /// clears a module's state — a delay keeps its buffer across the move; the
    /// chain fades out at the old slot and in at the new.
    /// </summary>
    public void MoveNode(JuceDspNode node, int newIndex)
    {
        ArgumentNullException.ThrowIfNull(node);
        MoveNode(node.Id, newIndex);
    }

    /// <summary>Move a node by its engine-global id.</summary>
    /// <exception cref="ArgumentOutOfRangeException">The id belongs to another
    /// seat, or <paramref name="newIndex" /> is negative.</exception>
    public void MoveNode(int nodeId, int newIndex)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(newIndex);
        ValidateOwnNode(nodeId, nameof(nodeId));
        var h = _engine.Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.EngineDspMoveNode(h, nodeId, newIndex), "dca_engine_dsp_move_node");
        }
        finally { _engine.Release(); }
    }

    /// <summary>Remove every node from this seat's chain.</summary>
    public void Clear()
    {
        var h = _engine.Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.EngineDspClearForPlayer(h, Player), "dca_engine_dsp_clear_for_player");
            // Journal BEFORE the wipe — an aborted scope puts every one of these
            // modules straight back natively, and the caller still holds the
            // wrapper objects for them.
            foreach (var pair in _known) JournalEvicted(pair.Key, pair.Value);
            _known.Clear();
        }
        finally { _engine.Release(); }
    }

    // ------------------------------------------------------------- edit scopes

    /// <summary>
    ///     ABI v34. Open an EDIT SCOPE: until the returned scope is committed,
    ///     every structure and parameter change on this seat accumulates against
    ///     a shadow chain and NOTHING is published to the audio callback.
    ///     Committing publishes the whole preset as one change.
    ///     <para>
    ///         Without a scope a twelve-pedal preset is twelve ordering
    ///         publications and twelve barriers, each individually audible — the
    ///         board assembles itself pedal by pedal.
    ///         <see cref="AddNodeBypassed" /> only ever covered ONE node at a
    ///         time. Inside a scope the new nodes are parked and fade in
    ///         together, the departing nodes fade out together, and it costs at
    ///         most two publications whatever the preset size.
    ///     </para>
    ///     <para>
    ///         Reads inside the scope (<see cref="NodeCount" />,
    ///         <see cref="NodeIds" />, <see cref="JuceDspNode.GetParam" />,
    ///         <see cref="JuceDspNode.Bypass" />, <see cref="LatencySamples" />)
    ///         report the SHADOW, so a caller can build a preset and read it
    ///         back before anyone hears it.
    ///     </para>
    ///     <para>
    ///         Disposing the scope without <see cref="JuceDspEditScope.Commit" />
    ///         ABORTS it: the pre-edit structure — membership, order, bypass
    ///         flags and branch tags — comes back and the nodes the scope added
    ///         are destroyed. Parameter VALUES are not journalled and are not
    ///         rolled back; a scope is a structure transaction, not an undo
    ///         stack. Wrap it in <c>using</c> so a throw mid-preset cannot leave
    ///         the rig editing for ever.
    ///     </para>
    /// </summary>
    /// <remarks>
    ///     <b>A nested begin is not an error any more, and that is deliberate.</b>
    ///     It used to be refused, which is the correct shape for a transaction —
    ///     but the state it refused from is UNRECOVERABLE without a managed call:
    ///     a caller who dropped the previous scope (no <c>using</c>, an early
    ///     return, a field overwritten) left <c>editing_</c> set with nobody left
    ///     to clear it, and then every <c>BeginEdit</c> for the rest of the
    ///     engine's life also failed. So the engine now ABORTS the stale scope,
    ///     logs a Warning, and opens a fresh one; this wrapper additionally marks
    ///     the superseded <see cref="JuceDspEditScope" /> closed, so the old
    ///     object's <c>Dispose</c> (or finalizer) cannot come along later and
    ///     abort the scope that replaced it. Nesting is still not supported —
    ///     the inner begin DISCARDS the outer scope's edits rather than stacking.
    /// </remarks>
    public JuceDspEditScope BeginEdit()
    {
        // Neutralise the previous scope BEFORE the native begin. The engine is
        // about to throw its edits away; if this object survived it would abort
        // the NEW scope on its way out.
        // The ID is the authority (see ClaimScope); the weak reference is kept
        // only so a scope object that IS still reachable can be told it is dead.
        Interlocked.Exchange(ref _openScopeId, 0);
        var stale = TakeOpenScope();
        if (stale is not null)
        {
            stale.MarkSuperseded();
            JuceLog.Emit(JuceLogLevel.Warn,
                $"JuceDspChain.BeginEdit on seat {Player} found JuceDspEditScope #{stale.ScopeId} still open. "
                + "The engine aborted it and its edits are discarded. A scope that outlives its caller "
                + "freezes the seat's rig — wrap BeginEdit() in a `using`.");
        }

        // ROLL BACK BEFORE THE NATIVE CALL. The stale scope's journal describes
        // edits the engine is about to throw away (or has, if it is already
        // gone), and the native begin CAN FAIL — a disposed engine, an invalid
        // seat. Rolling back after the Check that throws orphaned the journal:
        // the previous scope was already permanently closed managed-side, and
        // the NEXT successful BeginEdit then replayed a journal two generations
        // old, evicting `_known` entries for nodes still live in the engine.
        RollBackEditJournal();

        var h = _engine.Acquire();
        try
        {
            JuceAudioEngine.Check(NativeMethods.EngineDspBeginEditForPlayer(h, Player),
                "dca_engine_dsp_begin_edit_for_player");
        }
        finally { _engine.Release(); }

        StartEditJournal();
        var scope = new JuceDspEditScope(this, Interlocked.Increment(ref s_scopeSequence));
        Volatile.Write(ref _openScopeId, scope.ScopeId);
        _openScope = new WeakReference<JuceDspEditScope>(scope);
        return scope;
    }

    /// <summary>
    ///     Claim the right to close scope <paramref name="scopeId" />, ONCE.
    ///     Returns false when this scope is no longer the seat's current one, in
    ///     which case the caller must do nothing at all.
    /// </summary>
    /// <remarks>
    ///     <b>The open scope is identified by ID, not by object reference, and
    ///     that is load-bearing.</b> <c>_openScope</c> is a SHORT
    ///     <see cref="WeakReference{T}" />, which the GC clears BEFORE the
    ///     finalizer runs. So: the caller drops scope A; the GC clears the weak
    ///     ref and queues <c>~JuceDspEditScope</c>; before the finalizer thread
    ///     gets there the caller calls <see cref="BeginEdit" />, whose
    ///     <see cref="TakeOpenScope" /> now returns null, so A is never
    ///     <c>MarkSuperseded</c>; scope B opens; A's finalizer runs, sees itself
    ///     open and aborts — <b>B</b>. B's caller then keeps writing into a shadow
    ///     that no longer exists and the preset is silently discarded. The id
    ///     survives the weak reference being cleared, so the CompareExchange here
    ///     simply loses.
    /// </remarks>
    internal bool ClaimScope(long scopeId)
        => scopeId != 0 && Interlocked.CompareExchange(ref _openScopeId, 0, scopeId) == scopeId;

    /// <summary>
    ///     The scope currently open on this chain, if one is and it has not been
    ///     collected. Held WEAKLY on purpose — a strong reference here would root
    ///     a dropped scope for ever and its finalizer, the net that recovers a
    ///     leak, would never run.
    /// </summary>
    internal JuceDspEditScope? TakeOpenScope()
    {
        var weak = Interlocked.Exchange(ref _openScope, null);
        if (weak is null) return null;
        return weak.TryGetTarget(out var scope) && !scope.IsClosed ? scope : null;
    }

    /// <summary>Drop the chain's weak handle on <paramref name="scope" />, so a
    /// later <see cref="BeginEdit" /> does not report an already-closed scope as
    /// leaked. Ignores a scope that is not the one on record.</summary>
    internal void ForgetScope(JuceDspEditScope scope)
    {
        var weak = Volatile.Read(ref _openScope);
        if (weak is null) return;
        if (weak.TryGetTarget(out var current) && !ReferenceEquals(current, scope)) return;
        Interlocked.CompareExchange(ref _openScope, null, weak);
    }

    private void StartEditJournal()
    {
        lock (_editJournalGate)
        {
            _editAdded   = new List<int>();
            _editEvicted = new Dictionary<int, JuceDspNode>();
        }
    }

    private void JournalAdded(int nodeId)
    {
        lock (_editJournalGate) _editAdded?.Add(nodeId);
    }

    private void JournalEvicted(int nodeId, JuceDspNode node)
    {
        lock (_editJournalGate)
        {
            // First eviction wins: the wrapper the caller has held since before
            // the scope is the one an abort has to bring back.
            if (_editEvicted is not null && !_editEvicted.ContainsKey(nodeId)) _editEvicted[nodeId] = node;
        }
    }

    /// <summary>Forget the journal without replaying it — the commit published
    /// everything in it, so the engine and <c>_known</c> already agree.</summary>
    private void EndEditJournal()
    {
        lock (_editJournalGate)
        {
            _editAdded   = null;
            _editEvicted = null;
        }
    }

    /// <summary>Put <c>_known</c> back the way <c>abortEdit</c> put the ENGINE
    /// back: the scope's own additions are gone natively, and everything it
    /// evicted is alive again.</summary>
    private void RollBackEditJournal()
    {
        List<int>? added;
        Dictionary<int, JuceDspNode>? evicted;
        lock (_editJournalGate)
        {
            added        = _editAdded;
            evicted      = _editEvicted;
            _editAdded   = null;
            _editEvicted = null;
        }

        if (added is not null)
            foreach (var id in added) _known.TryRemove(id, out _);
        if (evicted is not null)
        {
            // A SET, not List.Contains inside the loop: a few-hundred-node
            // aborted preset was 10^4-10^5 comparisons under _editJournalGate.
            var addedSet = added is null ? null : new HashSet<int>(added);
            foreach (var pair in evicted)
            {
                // …but never resurrect a node the SAME scope both added and
                // removed. abortEdit destroys it (it is in editAdded_), so
                // putting its wrapper back would re-create the phantom this
                // journal exists to prevent.
                if (addedSet is not null && addedSet.Contains(pair.Key)) continue;
                _known.TryAdd(pair.Key, pair.Value);
            }
        }
    }

    /// <summary>Whether an edit scope is currently open on this seat. Poll-shaped:
    /// false on a disposed engine rather than throwing.</summary>
    public bool IsEditing
    {
        get
        {
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return false;
            try
            {
                return NativeMethods.EngineDspGetEditingForPlayer(h, Player, out var editing) == NativeMethods.DcaStatus.Ok
                       && editing != 0;
            }
            finally { _engine.Release(); }
        }
    }

    internal void CommitEdit()
    {
        try
        {
            var h = _engine.Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.EngineDspCommitEditForPlayer(h, Player),
                    "dca_engine_dsp_commit_edit_for_player");
            }
            finally { _engine.Release(); }
        }
        finally
        {
            // Dropped, never replayed — a commit PUBLISHED the scope's edits, so
            // `_known` and the engine already agree. Dropped even when the commit
            // threw: the scope is closed either way and replaying it then would
            // resurrect wrappers for modules the engine may well have retired.
            EndEditJournal();
        }
    }

    /// <summary>Abort on the way out of a using block. Never throws: it runs from
    /// Dispose, including the Dispose that is unwinding somebody else's
    /// exception, and a disposed engine has already aborted the scope for us
    /// (the native chain ends an open scope in its destructor).</summary>
    internal void AbortEditQuietly()
    {
        try
        {
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return;
            try { NativeMethods.EngineDspAbortEditForPlayer(h, Player); }
            finally { _engine.Release(); }
        }
        finally
        {
            // Replayed unconditionally, including on a disposed engine: the
            // native chain's destructor ends an open scope the same way, so
            // `_known` has to come back either way. See the journal fields.
            RollBackEditJournal();
        }
    }

    /// <summary>How many nodes this seat's chain currently holds.</summary>
    /// <remarks>
    ///     <c>0</c> means an EMPTY chain and nothing else. The native export
    ///     answers <c>-1</c> for "dead engine / seat out of range" — the ABI's
    ///     sentinel for every value-returning export since the v35 wave, where
    ///     this family used <c>0</c> for BOTH and a disposed handle was
    ///     indistinguishable from an empty pedalboard. The clamp below folds
    ///     that back to <c>0</c> deliberately: a property cannot report a fault,
    ///     and a count of zero is the honest thing to show for an engine that is
    ///     gone. Use <see cref="LatencySamples" />, which surfaces the
    ///     <c>-1</c>, when you need to tell the two apart.
    /// </remarks>
    public int NodeCount
    {
        get
        {
            var h = _engine.Acquire();
            try { return Math.Max(0, NativeMethods.EngineDspNodeCountForPlayer(h, Player)); }
            finally { _engine.Release(); }
        }
    }

    /// <summary>
    ///     A snapshot of this seat's nodes IN CHAIN ORDER. Index-aligned with
    ///     <see cref="TryReadMeters(Span{JuceDspNodeMeter}, out int)" />, which is what makes a per-pedal meter row
    ///     line up with the pedals.
    ///     <para>
    ///         A node this chain created comes back as the same
    ///         <see cref="JuceDspNode" /> instance. Since ABI v33 one it did NOT
    ///         create is just as complete: <see cref="JuceDspNode.TypeId" /> and
    ///         <see cref="JuceDspNode.Bypass" /> are read from the engine.
    ///     </para>
    ///     <para>
    ///         <b>THIS IS A PROPERTY THAT COSTS TWO P/INVOKES AND AN ARRAY
    ///         ALLOCATION PER GET.</b> It is a snapshot, not a view:
    ///         <c>ReferenceEquals(chain.Nodes, chain.Nodes)</c> is <c>false</c>,
    ///         and nothing in it updates when the chain changes. Bind a UI to a
    ///         copy you refresh deliberately, and never write
    ///         <c>for (i = 0; i &lt; chain.Nodes.Count; i++) chain.Nodes[i]</c> —
    ///         that is 2n+2 P/Invokes and n+1 arrays for one pass. Take it into a
    ///         local once. (It stays a property because callers already
    ///         <c>foreach</c> it; <see cref="NodeIds" /> beside it is a METHOD for
    ///         exactly the reason this one should have been.)
    ///     </para>
    /// </summary>
    public IReadOnlyList<JuceDspNode> Nodes
    {
        get
        {
            var ids = NodeIds();
            if (ids.Length == 0) return Array.Empty<JuceDspNode>();
            var nodes = new JuceDspNode[ids.Length];
            for (var i = 0; i < ids.Length; i++)
            {
                nodes[i] = _known.GetOrAdd(ids[i],
                    id => new JuceDspNode(this, _engine, id, string.Empty));
            }
            return nodes;
        }
    }

    /// <summary>This seat's node ids, in chain order. Engine-global, so they are
    /// safe to hand straight back to any node-scoped call.</summary>
    /// <remarks>
    ///     An empty array means an empty chain OR an engine that could not
    ///     answer: the native sizing call returns <c>-1</c> for a dead engine or
    ///     a seat out of range and <c>0</c> for a chain with no nodes, and the
    ///     <c>count &lt;= 0</c> test below folds them together on purpose —
    ///     there is no array to hand back in either case.
    /// </remarks>
    public int[] NodeIds()
    {
        var h = _engine.Acquire();
        try
        {
            // Size-then-fill: the count can exceed the capacity given.
            var count = NativeMethods.EngineDspGetNodeIdsForPlayer(h, Player, null, 0);
            if (count <= 0) return Array.Empty<int>();
            var ids = new int[count];
            var written = NativeMethods.EngineDspGetNodeIdsForPlayer(h, Player, ids, ids.Length);
            return written == count ? ids : ids[..Math.Max(0, Math.Min(written, count))];
        }
        finally { _engine.Release(); }
    }

    // ---------------------------------------------------------------- latency

    /// <summary>
    ///     Total latency this seat's chain adds, in samples at the chain's prepared
    ///     rate, or −1 when the engine cannot answer.
    ///     <para>
    ///         Inside a split/parallel region this is the MAXIMUM over the enabled
    ///         branches, not their sum — they run in parallel, so the region is as
    ///         late as its latest branch. Since ABI v34 it is also what the region
    ///         actually RUNS at, because
    ///         <see cref="BranchLatencyCompensation" /> delays every branch up to
    ///         that maximum; with the compensation switched off it is the old
    ///         figure, and the branches disagree by up to this much.
    ///     </para>
    /// </summary>
    public int LatencySamples
    {
        get
        {
            var h = _engine.Acquire();
            try { return NativeMethods.EngineDspGetChainLatencyForPlayer(h, Player); }
            finally { _engine.Release(); }
        }
    }

    /// <summary>
    ///     ABI v34. Whether a split/parallel region's branches are delay-ALIGNED
    ///     at the merger. <b>On by default.</b>
    ///     <para>
    ///         Summing branches that carry different latencies is a comb filter,
    ///         not a blend: one branch holding a pitch shifter or an oversampled
    ///         drive arrives hundreds of samples after an empty one, and the
    ///         merger used to add them as they fell. The merger now delays each
    ///         branch by (latest − its own), re-derived whenever a structure or
    ///         parameter change can have moved a latency, and on a device change.
    ///     </para>
    ///     <para>
    ///         Setting it false restores the pre-v34 sum byte for byte — the comb
    ///         is a sound some players want. The ceiling is 8,191 samples per
    ///         branch; a branch stacking past it is clamped (still better aligned
    ///         than not), and an UNMATCHED splitter auto-merges inside the chain
    ///         rather than at a merger node and is not compensated.
    ///     </para>
    /// </summary>
    public bool BranchLatencyCompensation
    {
        get
        {
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return false;
            try
            {
                return NativeMethods.EngineDspGetBranchPdcForPlayer(h, Player, out var on) == NativeMethods.DcaStatus.Ok
                       && on != 0;
            }
            finally { _engine.Release(); }
        }
        set
        {
            var h = _engine.Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.EngineDspSetBranchPdcForPlayer(h, Player, value ? 1 : 0),
                    "dca_engine_dsp_set_branch_pdc_for_player");
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>
    ///     ABI v37. SPILLOVER: when a structure change REMOVES an audible delay or
    ///     reverb (every catalogue entry whose category is <c>delay</c> or
    ///     <c>reverb</c>), its input ramps to silence in place and it rings on AFTER
    ///     the whole chain, summed into this seat's rig output, until it has rung
    ///     for 8 s or stayed under -90 dBFS for 200 ms; then it is retired off the
    ///     audio thread. At most four tails ring at once; a fifth fades the oldest.
    ///     <b>Off at engine start</b>; turning it off fades every ringing tail out.
    ///     A rebuilt engine starts off again, so a host replays it
    ///     (<c>dca_engine_dsp_set_spillover_for_player</c> /
    ///     <c>dca_engine_dsp_get_spillover_for_player</c>). The getter answers false
    ///     on a disposed engine.
    /// </summary>
    public bool Spillover
    {
        get
        {
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return false;
            try
            {
                return NativeMethods.EngineDspGetSpilloverForPlayer(h, Player, out var on) == NativeMethods.DcaStatus.Ok
                       && on != 0;
            }
            finally { _engine.Release(); }
        }
        set
        {
            var h = _engine.Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.EngineDspSetSpilloverForPlayer(h, Player, value ? 1 : 0),
                    "dca_engine_dsp_set_spillover_for_player");
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>
    ///     ABI v37. Spillover tails still RINGING on this seat; reading it also
    ///     retires the ones that have finished (message-thread work, never on the
    ///     callback). -1 on a disposed engine
    ///     (<c>dca_engine_dsp_get_spill_tail_count_for_player</c>).
    /// </summary>
    public int SpillTailCount
    {
        get
        {
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return -1;
            try { return NativeMethods.EngineDspGetSpillTailCountForPlayer(h, Player); }
            finally { _engine.Release(); }
        }
    }

    // ---------------------------------------------------------------- meters

    /// <summary>
    ///     Per-node metering for this seat. OFF at engine start and OFF until asked
    ///     for: while it is off the DSP callback is byte-for-byte the pre-ABI-v30
    ///     path, and a caller that never polls costs the audio thread nothing.
    ///     Turning it off also ZEROES the meters, so a UI that stops polling does
    ///     not leave a frozen picture behind.
    /// </summary>
    public bool MeteringEnabled
    {
        get
        {
            var h = _engine.TryAcquire();
            if (h == nint.Zero) return false;
            try { return NativeMethods.EngineDspGetMetering(h, Player) != 0; }
            finally { _engine.Release(); }
        }
        set
        {
            var h = _engine.Acquire();
            try
            {
                JuceAudioEngine.Check(NativeMethods.EngineDspSetMeteringForPlayer(h, Player, value ? 1 : 0),
                    "dca_engine_dsp_set_metering_for_player");
            }
            finally { _engine.Release(); }
        }
    }

    /// <summary>
    ///     Read every node's meter in ONE P/Invoke, in chain order, index-aligned
    ///     with <see cref="Nodes" /> — one call per UI frame instead of one per
    ///     pedal.
    /// </summary>
    /// <param name="dest">Filled with up to <c>dest.Length</c> entries. Pass an
    /// empty span to size the result.</param>
    /// <returns>The node COUNT, which may exceed <c>dest.Length</c>.</returns>
    /// <remarks>
    ///     All four values are LINEAR amplitudes — never dB, and legitimately
    ///     above 1 for a node driving into a clipper. Peaks decay ~34 dB/s so a
    ///     30 Hz poll sees every transient; the RMS integrates over ~300 ms. All
    ///     four read 0 while <see cref="MeteringEnabled" /> is false.
    ///     <para>
    ///         <b>This overload is named <c>Try*</c> and cannot report failure.</b>
    ///         It returns an <c>int</c>, and a <c>0</c> means either "this seat
    ///         has no nodes" or "the engine is gone" — note that the NATIVE call
    ///         does distinguish them (<c>-1</c> for the second, the ABI's
    ///         sentinel for every value-returning export since the v35 wave);
    ///         this overload clamps it away. The caller cannot tell,
    ///         which is precisely the distinction its own sibling
    ///         <see cref="TryReadMeter(int, out JuceDspNodeMeter)" /> exists to
    ///         make. It is kept because it is the shape
    ///         <see cref="IJuceDspChain" /> declares and callers already use;
    ///         prefer
    ///         <see cref="TryReadMeters(Span{JuceDspNodeMeter}, out int)" />,
    ///         which answers both questions.
    ///     </para>
    /// </remarks>
    [Obsolete("Use TryReadMeters(Span<JuceDspNodeMeter>, out int): this overload cannot distinguish 'no nodes' from 'engine gone'.")]
    public int TryReadMeters(Span<JuceDspNodeMeter> dest)
        => TryReadMeters(dest, out var count) ? count : 0;

    /// <summary>
    ///     <see cref="TryReadMeters(Span{JuceDspNodeMeter})" /> with "the engine
    ///     answered" separated from "the answer was zero nodes" — a real
    ///     <c>Try</c>. A <c>false</c> means the engine is GONE (a poll racing a
    ///     <see cref="JuceAudioEngine.Dispose()" />) and
    ///     <paramref name="nodeCount" /> is 0; a <c>true</c> with
    ///     <paramref name="nodeCount" /> 0 means this seat really has no nodes,
    ///     and a UI should draw an empty board rather than hold its last frame.
    /// </summary>
    /// <param name="dest">Filled with up to <c>dest.Length</c> entries, in chain
    /// order. Pass an empty span to size the result.</param>
    /// <param name="nodeCount">The node COUNT, which may exceed <c>dest.Length</c>.</param>
    /// <remarks>Values are LINEAR amplitudes; see the other overload. This does
    /// NOT report whether metering is ENABLED — with it off the engine answers
    /// true with zeros, exactly as documented on
    /// <see cref="MeteringEnabled" />.</remarks>
    public bool TryReadMeters(Span<JuceDspNodeMeter> dest, out int nodeCount)
    {
        nodeCount = 0;
        var h = _engine.TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            unsafe
            {
                // JuceDspNodeMeter is four sequential floats, so the span IS the
                // {in_peak, in_rms, out_peak, out_rms} block the ABI writes.
                var floats = MemoryMarshal.Cast<JuceDspNodeMeter, float>(dest);
                fixed (float* p = floats)
                {
                    nodeCount = Math.Max(0, NativeMethods.EngineDspReadNodeMeters(h, Player, p, floats.Length));
                }
            }
            return true;
        }
        finally { _engine.Release(); }
    }

    // ---------------------------------------------------------------- offline

    /// <summary>
    ///     Render a buffer through THIS seat's rig with no audio device involved —
    ///     the same code the live callback runs, chunked internally to a
    ///     prepared-safe block size. This is how per-seat tone is verified, and it
    ///     doubles as an offline preview / re-amp hook.
    ///     <para>
    ///         <b>OFFLINE MEANS OFFLINE.</b> The engine returns
    ///         <see cref="JuceAudioStatus.Device" /> while a device is OPEN, and that
    ///         refusal is deliberate: the chunk loop takes the chain's process
    ///         barrier — the audio callback's own wait-one-block mutex — once per
    ///         512 frames, so rendering a three-minute take against a running device
    ///         contends the callback ~16,000 times. Call
    ///         <see cref="JuceAudioEngine.Stop" /> first, or use a dedicated
    ///         device-less engine.
    ///     </para>
    /// </summary>
    /// <param name="input">Interleaved samples, <paramref name="channels"/> per frame.</param>
    /// <param name="channels">
    ///     1 or 2.
    ///     <para>
    ///         <b>1 is a mono fold-down on the way OUT as well as a duplication on
    ///         the way in</b>, and only the way in used to be documented (W4-E5).
    ///         The rig always runs two channels, so a mono render feeds
    ///         <c>L = R = input</c> and then writes <c>0.5 * (L + R)</c>. For a
    ///         channel-symmetric chain that is a no-op, which is why it reads as
    ///         harmless — but for a chain containing <c>widener</c>, <c>rotary</c>,
    ///         <c>chorus</c>, a ping-pong <c>delay</c> or the panner it is exactly
    ///         a mono fold-down, and whatever those modules put out of phase
    ///         CANCELS. Re-amping a mono take through a stereo board that way gets
    ///         you a thin or partially silent result with nothing saying why: use
    ///         the STEREO overload with the mono take duplicated into both
    ///         channels, which is also zero-copy in place where this one allocates
    ///         two <c>float[frames]</c> per call.
    ///     </para>
    /// </param>
    /// <param name="sampleRate">8000..192000. The rig is prepared lazily at this rate.</param>
    /// <param name="output">Interleaved result; must be the same length as <paramref name="input"/>.
    /// May alias <paramref name="input"/>.</param>
    public void ProcessOffline(ReadOnlySpan<float> input, int channels, int sampleRate, Span<float> output)
    {
        if (channels is not (1 or 2))
            throw new ArgumentOutOfRangeException(nameof(channels), channels, "The rig renders mono or stereo.");
        if (output.Length != input.Length)
            throw new ArgumentException("output must be exactly as long as input.", nameof(output));
        if (input.IsEmpty) return;
        if (input.Length % channels != 0)
            throw new ArgumentException("input length is not a whole number of frames.", nameof(input));

        var frames = input.Length / channels;
        var left = new float[frames];
        var right = new float[frames];
        for (var i = 0; i < frames; i++)
        {
            left[i]  = input[i * channels];
            right[i] = channels == 2 ? input[i * channels + 1] : left[i];
        }

        var h = _engine.Acquire();
        try
        {
            unsafe
            {
                fixed (float* l = left, r = right)
                {
                    JuceAudioEngine.Check(
                        NativeMethods.EngineDspProcessBufferForPlayer(h, Player, l, r, frames, sampleRate),
                        "dca_engine_dsp_process_buffer_for_player");
                }
            }
        }
        finally { _engine.Release(); }

        for (var i = 0; i < frames; i++)
        {
            output[i * channels] = channels == 2 ? left[i] : 0.5f * (left[i] + right[i]);
            if (channels == 2) output[i * channels + 1] = right[i];
        }
    }

    // ------------------------------------------------------- 0.9 additions
    // Appended, not woven in: these are the three gaps DotCharter found when it
    // adopted this type. Everything above is unchanged.

    /// <summary>
    ///     Find a node by its engine-global id. O(1) for a node this chain
    ///     created or has already handed out; otherwise ONE
    ///     <see cref="NodeIds" /> probe confirms the engine really has it, and the
    ///     resulting <see cref="JuceDspNode" /> is remembered so the next lookup is
    ///     O(1) too.
    ///     <para>
    ///         This exists because callers address the rig BY ID — a saved preset,
    ///         a UI row, an undo entry all store the id, not the object — and
    ///         without it every one of them kept its own id → node dictionary
    ///         alongside this type's.
    ///     </para>
    ///     <para>
    ///         A <c>false</c> means the engine does not have that node in THIS
    ///         seat's chain right now: a removed node, an id belonging to another
    ///         seat, or a disposed engine. It never throws for a missing id — that
    ///         is the question being asked.
    ///     </para>
    /// </summary>
    /// <remarks><see cref="MaybeNullWhenAttribute" />: on a <c>false</c> return
    /// <paramref name="node" /> IS null. Without the annotation the compiler told
    /// every caller it was not, so the `if (!TryGetNode(...)) … node.Bypass`
    /// mistake compiled with no warning at all.</remarks>
    public bool TryGetNode(int nodeId, [MaybeNullWhen(false)] out JuceDspNode node)
    {
        if (_known.TryGetValue(nodeId, out var known))
        {
            node = known;
            return true;
        }

        node = null!;
        if (nodeId < 0) return false;
        // Not ours (yet). One probe: a node created through another binding — or
        // by a chain this process re-attached to — is a first-class citizen since
        // ABI v33, so the only question is whether the engine still has it.
        if (Array.IndexOf(NodeIds(), nodeId) < 0) return false;
        node = _known.GetOrAdd(nodeId, id => new JuceDspNode(this, _engine, id, string.Empty));
        return true;
    }

    /// <summary>
    ///     PLANAR <see cref="ProcessOffline(ReadOnlySpan{float}, int, int, Span{float})" />:
    ///     the shape the ABI actually has. The native export processes two
    ///     separate channel buffers IN PLACE, so an interleaved caller pays a
    ///     de-interleave and a re-interleave — and a caller who already holds
    ///     planes (every stem path, every analysis buffer) paid BOTH of those to
    ///     hand planes to a function that wanted planes.
    ///     <para>
    ///         <paramref name="outLeft" /> / <paramref name="outRight" /> may alias
    ///         the inputs, which is the zero-copy case: pass the same spans twice
    ///         and the rig renders in place.
    ///     </para>
    ///     <para>
    ///         <b>OFFLINE MEANS OFFLINE</b>, exactly as on the interleaved
    ///         overload: the engine returns <see cref="JuceAudioStatus.Device" />
    ///         while a device is OPEN.
    ///     </para>
    /// </summary>
    /// <param name="left">Left/mono input plane.</param>
    /// <param name="right">Right input plane; must be the same length as
    /// <paramref name="left" />. Pass the left plane again for mono.</param>
    /// <param name="sampleRate">8000..192000. The rig is prepared lazily at this rate.</param>
    /// <param name="outLeft">Left result; same length as <paramref name="left" />.</param>
    /// <param name="outRight">Right result; same length as <paramref name="left" />.</param>
    public void ProcessOffline(ReadOnlySpan<float> left, ReadOnlySpan<float> right, int sampleRate,
                               Span<float> outLeft, Span<float> outRight)
    {
        if (right.Length != left.Length)
            throw new ArgumentException("right must be exactly as long as left.", nameof(right));
        if (outLeft.Length != left.Length)
            throw new ArgumentException("outLeft must be exactly as long as left.", nameof(outLeft));
        if (outRight.Length != left.Length)
            throw new ArgumentException("outRight must be exactly as long as left.", nameof(outRight));
        if (left.IsEmpty) return;

        // The export is in-place, so the output planes ARE the working buffers.
        // CopyTo over identical spans is a memmove onto itself, which is what
        // makes the aliased (zero-copy) call free.
        left.CopyTo(outLeft);
        right.CopyTo(outRight);

        var frames = left.Length;
        var h = _engine.Acquire();
        try
        {
            unsafe
            {
                fixed (float* l = outLeft, r = outRight)
                {
                    JuceAudioEngine.Check(
                        NativeMethods.EngineDspProcessBufferForPlayer(h, Player, l, r, frames, sampleRate),
                        "dca_engine_dsp_process_buffer_for_player");
                }
            }
        }
        finally { _engine.Release(); }
    }

    /// <summary>
    ///     ONE node's meter, by engine-global node id, with
    ///     <b>"no reading" distinguished from "silence"</b>: this returns
    ///     <c>false</c> — leaving <paramref name="meter" /> at its default —
    ///     when metering is OFF for this seat, when the engine is gone, or when
    ///     the engine has no such node. A <c>true</c> with four zeros therefore
    ///     means the node really was silent.
    ///     <para>
    ///         <see cref="JuceDspNode.Meter" /> cannot make that distinction: it
    ///         is a property and returns four zeros for both cases, which is
    ///         exactly the ambiguity that made callers bind
    ///         <see cref="JuceDspMeters" /> against a raw, UNPINNED engine handle
    ///         to ask <see cref="JuceDspMeters.IsEnabled" /> alongside it. Here
    ///         both questions are answered under ONE pin of the engine handle, so
    ///         the answer cannot change between them and a poll racing a dispose
    ///         is a <c>false</c>, not a read through freed memory.
    ///     </para>
    ///     <para>
    ///         For a whole UI frame prefer <see cref="TryReadMeters(Span{JuceDspNodeMeter}, out int)" /> — one
    ///         P/Invoke for every node instead of one per pedal.
    ///     </para>
    /// </summary>
    /// <remarks>All four values are LINEAR amplitudes, legitimately above 1 for a
    /// node driving into a clipper.</remarks>
    public bool TryReadMeter(int nodeId, out JuceDspNodeMeter meter)
    {
        meter = default;
        // Seat-scoped like the mutators: this gates on THIS seat's metering flag
        // and would otherwise answer false for another seat's node whose own seat
        // has metering on — a wrong answer dressed as "no reading".
        ValidateOwnNode(nodeId, nameof(nodeId));
        var h = _engine.TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            // Metering OFF is not a reading. The ABI reports Ok with zeros in
            // that state, so asking the engine is the only way to tell.
            if (NativeMethods.EngineDspGetMetering(h, Player) == 0) return false;
            if (NativeMethods.EngineDspGetNodeMeter(h, nodeId, out var ip, out var ir, out var op, out var orms)
                != NativeMethods.DcaStatus.Ok) return false;
            meter = new JuceDspNodeMeter(ip, ir, op, orms);
            return true;
        }
        finally { _engine.Release(); }
    }
}
