namespace JuceSharp.Dsp;

/// <summary>
///     ABI v34. An open edit scope on one seat's tone rig, from
///     <see cref="JuceDspChain.BeginEdit" />. While it is open the native chain
///     publishes NOTHING to the audio callback: a whole preset — clear, twelve
///     adds, thirty parameter writes, the branch tags — reaches the audio thread
///     as one ordering snapshot when <see cref="Commit" /> runs.
///     <para>
///         <b>Dispose without <see cref="Commit" /> ABORTS.</b> That is the
///         point of the shape: wrap it in <c>using</c> and a throw halfway
///         through building a preset leaves the rig exactly as it was instead of
///         stuck in an edit nobody will ever close. Committing twice, or
///         disposing after committing, does nothing.
///     </para>
///     <para>
///         <b>A DROPPED scope is recovered, not tolerated.</b> Forgetting the
///         <c>using</c> used to freeze the seat for the life of the ENGINE:
///         <c>editing_</c> is cleared by no other managed call, so every later
///         add / remove / move / bypass accumulated in the shadow, returned
///         success, and nothing was ever published to the audio callback again.
///         Two nets close that now, and they are deliberately both present —
///         this type has a FINALIZER that aborts the scope and logs at
///         <see cref="JuceLogLevel.Error" />, and
///         <see cref="JuceDspChain.BeginEdit" /> aborts a stale scope rather
///         than refusing (a GC may not have run yet, and a caller that is
///         opening a new scope has plainly abandoned the old one). Neither is a
///         reason to drop the <c>using</c>: a finalizer runs at a
///         GC-dependent moment, so the seat is frozen until then.
///     </para>
///     <para>
///         The scope is not thread-safe and is not meant to be: it represents
///         one caller's transaction on one seat.
///     </para>
/// </summary>
public sealed class JuceDspEditScope : IDisposable
{
    private readonly JuceDspChain _chain;
    private readonly long _scopeId;
    private bool _closed;

    internal JuceDspEditScope(JuceDspChain chain, long scopeId)
    {
        _chain   = chain;
        _scopeId = scopeId;
    }

    /// <summary>The seat this scope is editing.</summary>
    public int Player => _chain.Player;

    /// <summary>Whether the scope has been committed or aborted.</summary>
    public bool IsClosed => _closed;

    /// <summary>
    ///     A process-wide, monotonic number for this scope. It exists so the
    ///     diagnostics for a LEAKED scope can name one — "seat 0" is not enough
    ///     when a preset loader and a UI both edit the same seat.
    /// </summary>
    public long ScopeId => _scopeId;

    /// <summary>
    ///     Publish everything the scope accumulated, as one change. The new
    ///     nodes fade in and the departing ones fade out in the SAME ~10 ms
    ///     window, so this returns once the fade-outs have landed, not once the
    ///     fade-ins have. Calling it twice is a no-op.
    /// </summary>
    /// <exception cref="JuceAudioException">The engine refused the commit.</exception>
    /// <exception cref="ObjectDisposedException">The engine is gone.</exception>
    public void Commit()
    {
        if (_closed) return;
        // Marked closed BEFORE the call: a commit that throws must not leave a
        // Dispose behind it that then aborts an already-published preset.
        _closed = true;
        GC.SuppressFinalize(this);
        _chain.ForgetScope(this);
        // CLAIM BY ID. If this scope is no longer the seat's current one it was
        // superseded by a BeginEdit that could not reach this object (the weak
        // reference had already been cleared), and committing would publish the
        // REPLACEMENT scope's half-built shadow.
        if (!_chain.ClaimScope(_scopeId)) return;
        _chain.CommitEdit();
    }

    /// <summary>Abort the scope explicitly — identical to disposing without
    /// committing. Restores the pre-edit structure; parameter values are not
    /// rolled back. A scope that has already been superseded by a later
    /// <see cref="JuceDspChain.BeginEdit" /> aborts NOTHING.</summary>
    public void Abort()
    {
        if (_closed) return;
        _closed = true;
        GC.SuppressFinalize(this);
        _chain.ForgetScope(this);
        if (!_chain.ClaimScope(_scopeId)) return;
        _chain.AbortEditQuietly();
    }

    /// <inheritdoc />
    public void Dispose() => Abort();

    /// <summary>
    ///     Called by <see cref="JuceDspChain.BeginEdit" /> when it found THIS
    ///     scope still open and had the engine abort it. The scope is already
    ///     dead natively, so it must never abort again — doing so would kill the
    ///     scope that replaced it.
    /// </summary>
    internal void MarkSuperseded()
    {
        _closed = true;
        GC.SuppressFinalize(this);
    }

    /// <summary>
    ///     Recovers a scope whose owner dropped it. <b>Everything here is inside
    ///     one catch-all</b>: a finalizer that throws is not a caught exception,
    ///     it is a process-wide FailFast. It also runs on a PARTIALLY-CONSTRUCTED
    ///     instance — the CLR makes the object finalizable the moment the
    ///     allocation succeeds — so <c>_chain</c> is read into a local and
    ///     null-checked rather than dereferenced (<c>FinalizerSafetyTests</c>).
    /// </summary>
    ~JuceDspEditScope()
    {
        try
        {
            if (_closed) return;
            var chain = _chain;
            if (chain is null) return;
            // CLAIM BEFORE LOGGING, let alone aborting. The chain's weak
            // reference to this scope was cleared before this finalizer was
            // queued, so a BeginEdit in that window could not MarkSuperseded us
            // — and aborting here would kill the scope that REPLACED this one,
            // silently discarding its preset. ClaimScope loses that race by id.
            if (!chain.ClaimScope(_scopeId)) return;
            JuceLog.Emit(JuceLogLevel.Error,
                $"JuceDspEditScope #{_scopeId} on seat {chain.Player} was garbage-collected while still "
                + "OPEN — it was never committed and never disposed. Aborting it: until this ran, that "
                + "seat's rig published NOTHING to the audio callback and every edit piled up in the "
                + "shadow chain while returning success. Wrap JuceDspChain.BeginEdit() in a `using`.");
            chain.ForgetScope(this);
            chain.AbortEditQuietly();
        }
        catch (Exception ex)
        {
            // Constrained to Exception so a StackOverflow / thread-abort still
            // propagates; anything else dies here rather than taking the process.
            try
            {
                JuceLog.Emit(JuceLogLevel.Error,
                    $"JuceDspEditScope finalizer threw and was swallowed: {ex.GetType().Name}: {ex.Message}");
            }
            catch { /* the log sink itself is on fire; there is nowhere left to say so */ }
        }
    }
}
