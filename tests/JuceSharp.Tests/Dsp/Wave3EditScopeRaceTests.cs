using System.Reflection;
using System.Text.RegularExpressions;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     Wave-3 audit findings #3, #4 and #10 — the three seams the wave-2
///     edit-scope fix left between two well-argued halves. All of them are
///     reachable with NO deliberate threading, because the
///     <see cref="JuceDspEditScope" /> finalizer runs on the GC thread by
///     construction.
/// </summary>
[Collection("NativeEngine")]
public sealed class Wave3EditScopeRaceTests : IDisposable
{
    private readonly JuceAudioEngine? _engine;

    public Wave3EditScopeRaceTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    /// <summary>
    ///     <b>#4 — a scope whose weak reference was already cleared cannot abort
    ///     its replacement.</b> <c>_openScope</c> is a SHORT
    ///     <see cref="WeakReference{T}" />, and the GC clears one BEFORE running
    ///     the finalizer. So: the caller drops scope A; the GC clears the weak ref
    ///     and queues <c>~JuceDspEditScope</c>; before the finalizer thread gets
    ///     there the caller calls <c>BeginEdit</c>, whose <c>TakeOpenScope</c> now
    ///     returns null so A is never <c>MarkSuperseded</c>; scope B opens; A's
    ///     finalizer runs, sees itself open, and aborts <b>B</b>. B's caller keeps
    ///     writing into a shadow that no longer exists and the preset is silently
    ///     discarded.
    ///     <para>
    ///         The window is reproduced EXACTLY rather than raced for: clearing
    ///         the chain's weak field by reflection is what the GC would have
    ///         done, and it makes the test deterministic.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void AScopeWhoseWeakRefWasCleared_CannotAbortTheScopeThatReplacedIt()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var a = chain.BeginEdit();

            // The GC has cleared the short weak reference but has not yet run
            // ~JuceDspEditScope. `a` is still very much alive and still open.
            typeof(JuceDspChain)
                .GetField("_openScope", BindingFlags.NonPublic | BindingFlags.Instance)!
                .SetValue(chain, null);
            Assert.False(a.IsClosed,
                "the fixture neutralised the stale scope by itself — the race below is vacuous");

            var b = chain.BeginEdit();
            var addedByB = chain.AddNode("gain").Id;
            Assert.True(chain.IsEditing);

            // A arriving late — its Dispose, or equivalently its finalizer.
            a.Dispose();

            Assert.True(chain.IsEditing,
                "the superseded scope A aborted scope B: B's caller goes on writing into a shadow that no "
                + "longer exists and its whole preset is silently discarded");

            b.Commit();
            Assert.Contains(addedByB, chain.NodeIds());
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     …and the same claim guards COMMIT. A superseded scope that commits must
    ///     publish nothing: the shadow it would publish belongs to the scope that
    ///     replaced it.
    /// </summary>
    [NativeAvailableFact]
    public void AScopeWhoseWeakRefWasCleared_CannotCommitOverTheScopeThatReplacedIt()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var a = chain.BeginEdit();
            typeof(JuceDspChain)
                .GetField("_openScope", BindingFlags.NonPublic | BindingFlags.Instance)!
                .SetValue(chain, null);

            var b = chain.BeginEdit();
            chain.AddNode("gain");

            a.Commit();
            Assert.True(chain.IsEditing,
                "the superseded scope A committed scope B's half-built shadow");

            b.Commit();
            Assert.False(chain.IsEditing);
            Assert.Single(chain.NodeIds());
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     <b>#3 — a <c>BeginEdit</c> that lands inside another thread's
    ///     <c>commitEdit</c> WAITS for it instead of aborting it.</b>
    ///     <c>commitEdit</c> has to release <c>writeMutex_</c> for up to the 10 ms
    ///     crossfade (holding it there blocks every other message-thread mutator),
    ///     and it released it with <c>editing_</c> still true — so
    ///     <c>beginEdit</c>'s stale-scope valve fired into the middle of it, and
    ///     the resumed commit then clobbered the state the NEW scope had just
    ///     established. The result was a scope that returned success, published
    ///     immediately from then on, and could not be aborted meaningfully.
    ///     <para>
    ///         <b>The assertions are ordering-AGNOSTIC on purpose, so this can
    ///         never falsely fail.</b> Only two outcomes are legal, and which one
    ///         happens depends on whether the racer's begin arrived before the
    ///         commit or during it:
    ///     </para>
    ///     <list type="bullet">
    ///         <item>the commit went first — <c>doomed</c> is gone and
    ///             <c>addedInScope</c> is published (the begin WAITED);</item>
    ///         <item>the begin went first — it found an open scope, aborted it as
    ///             a stale one, so <c>doomed</c> is back and <c>addedInScope</c>
    ///             never existed.</item>
    ///     </list>
    ///     <para>
    ///         The BUG produces neither: the abort fires into the middle of the
    ///         commit, the resumed commit clobbers the new scope's state, and what
    ///         comes out is a seat that is still EDITING, or one holding both
    ///         nodes, or one holding neither. So the invariant asserted here —
    ///         exactly one of the two is published, and nothing is left open — is
    ///         what the broken state cannot satisfy.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void ABeginEditRacingACommit_WaitsForIt_RatherThanAbortingItHalfway()
    {
        var chain = Engine.GetDspChain();
        try
        {
            // A node that is AUDIBLE, so the commit really takes the
            // fade-out-and-wait path (~10 ms with writeMutex_ released) rather
            // than the one-publication shortcut. That wait IS the window.
            var doomed = chain.AddNode("gain").Id;

            int addedInScope;
            var addedAfter = 0;
            var committing = new ManualResetEventSlim(false);

            using (var scope = chain.BeginEdit())
            {
                chain.RemoveNode(doomed);
                addedInScope = chain.AddNode("gain").Id;

                var racer = new Thread(() =>
                {
                    committing.Wait();
                    Thread.Sleep(1);          // aim for the middle of the fade wait
                    using var second = chain.BeginEdit();
                    addedAfter = chain.AddNode("gain").Id;
                    second.Commit();
                });

                var committer = new Thread(() =>
                {
                    committing.Set();
                    scope.Commit();
                });

                racer.Start();
                committer.Start();
                committer.Join();
                racer.Join();
            }

            var ids = chain.NodeIds();
            Assert.False(chain.IsEditing, "a scope was left open by the race");
            Assert.Contains(addedAfter, ids);
            Assert.True(ids.Contains(doomed) != ids.Contains(addedInScope),
                "neither legal outcome happened: the commit either finished (doomed gone, "
                + $"addedInScope published) or was superseded before it started (doomed back, "
                + $"addedInScope never published). Got [{string.Join(", ", ids)}] with doomed={doomed}, "
                + $"addedInScope={addedInScope} — which is the shape a begin/abort landing INSIDE "
                + "the commit produces: the abort restores the pre-edit order and the resumed "
                + "commit then clobbers the scope that replaced it.");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     <b>#10 — the journal is rolled back BEFORE the native begin that can
    ///     fail.</b> <c>BeginEdit</c> closed the stale scope managed-side, then
    ///     called native, then rolled back — so a native failure (a disposed
    ///     engine, an invalid seat) left the journal orphaned, and the NEXT
    ///     successful <c>BeginEdit</c> replayed a journal describing a scope two
    ///     generations old, evicting <c>_known</c> entries for nodes still live in
    ///     the engine. That is the wave-2 #11/#12 fault re-entering through the fix
    ///     for it.
    ///     <para>
    ///         A source lint, because the failure needs the native begin to fail
    ///         and the next one on the SAME chain to succeed — which no reachable
    ///         engine state produces.
    ///     </para>
    /// </summary>
    [Fact]
    public void TheEditJournalIsRolledBack_BeforeTheNativeBeginThatCanFail()
    {
        var text = WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp/Dsp/JuceDspChain.cs");
        var body = text[text.IndexOf("public JuceDspEditScope BeginEdit()", StringComparison.Ordinal)..];
        body = body[..body.IndexOf("StartEditJournal();", StringComparison.Ordinal)];

        var rollback = body.IndexOf("RollBackEditJournal();", StringComparison.Ordinal);
        var check    = body.IndexOf("EngineDspBeginEditForPlayer", StringComparison.Ordinal);
        Assert.True(rollback >= 0 && check >= 0, "BeginEdit no longer has the shape this lint reads");
        Assert.True(rollback < check,
            "RollBackEditJournal() runs AFTER the native begin, so a begin that throws orphans the "
            + "previous scope's journal and the next successful begin replays it");
    }

    /// <summary>
    ///     <b>#16 — the stale-scope warning is emitted OUTSIDE
    ///     <c>writeMutex_</c>.</b> <c>logSink_</c> trampolines into the managed log
    ///     delegate: arbitrary user code, possibly file I/O, possibly a UI
    ///     dispatcher. Under the chain's write mutex it blocked every other
    ///     message-thread mutator for its duration, and a handler that read back
    ///     from the same chain — "log, then dump the node list" — deadlocked. The
    ///     surrounding code already got the harder half right: <c>doomed</c> is
    ///     declared before the guard precisely so module destructors run outside it.
    /// </summary>
    [Fact]
    public void TheStaleScopeWarning_IsEmittedOutsideTheWriteMutex()
    {
        var text = WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp.Native/src/dsp/DspChain.h");
        var body = text[text.IndexOf("bool beginEdit()", StringComparison.Ordinal)..];
        body = body[..body.IndexOf("bool editing() const", StringComparison.Ordinal)];

        var unlock = body.LastIndexOf('}', body.IndexOf("if (aborted && logSink_", StringComparison.Ordinal));
        Assert.True(unlock > 0, "beginEdit no longer closes its lock scope before the log call");
        // …and the lock really is a scoped unique_lock that ends there.
        Assert.Matches(new Regex(@"std::unique_lock<std::mutex>\s+lock\(writeMutex_\);"), body);
    }
}
