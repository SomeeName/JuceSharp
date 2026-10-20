using System;
using System.Linq;
using System.Runtime.CompilerServices;
using JuceSharp;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     Wave-2 audit findings #4, #11 and #12 — the three ways an edit scope could
///     leave the managed wrapper and the engine disagreeing.
///     <list type="number">
///         <item>
///             <b>#4</b> A DROPPED scope froze the seat's rig for the life of the
///             ENGINE. <c>editing_</c> is cleared by no other managed call and the
///             rigs are value members of <c>AudioEngine</c>, so a caller who
///             forgot the <c>using</c> left every later add / remove / move /
///             bypass accumulating in the shadow, returning success, and never
///             reaching the audio callback again. The only tell was
///             <see cref="JuceDspChain.IsEditing" />, which nobody polls.
///         </item>
///         <item>
///             <b>#11</b> An ABORTED scope destroys the nodes it added natively,
///             but their wrappers stayed in the chain's identity map — so
///             <see cref="JuceDspChain.TryGetNode" /> kept answering <c>true</c>
///             with a phantom whose poll-shaped members (Bypass / TypeId / Meter /
///             LatencySamples) read to a UI as a real pedal that is bypassed and
///             silent.
///         </item>
///         <item>
///             <b>#12</b> …and the mirror image: an abort puts a removed module
///             straight back, but its wrapper had already been evicted, so the
///             chain minted a FRESH <see cref="JuceDspNode" /> for an id the
///             caller was already holding an object for — breaking the reference
///             identity the type documents and losing the created-type-id fast
///             path.
///         </item>
///     </list>
/// </summary>
[Collection("NativeEngine")]
public sealed class Wave2EditScopeLeakTests : IDisposable
{
    private readonly JuceAudioEngine? _engine;

    public Wave2EditScopeLeakTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        // dca_engine_create opens a device; the rig under test must be quiet.
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    // ---------------------------------------------------------------- #4

    /// <summary>
    ///     The scope's finalizer is the net under a caller who never disposed it.
    ///     Forced with an explicit collection rather than waited for: the point is
    ///     that finalization RECOVERS the seat, not how soon.
    /// </summary>
    [NativeAvailableFact]
    public void ADroppedEditScope_IsAbortedByItsFinalizer_AndTheSeatPublishesAgain()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var addedId = LeakAScope(chain);

            // Non-vacuous: without this the GC below would be asserting nothing.
            Assert.True(chain.IsEditing,
                "the scope was not left open, so this test proves nothing about recovering a leak");

            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();

            Assert.False(chain.IsEditing,
                "a garbage-collected JuceDspEditScope left the seat EDITING: every later structure change "
                + "accumulates in the shadow and nothing is ever published to the audio callback again");
            // The abort destroyed what the scope added, and the wrapper cache
            // must agree with that (#11, on the finalizer path).
            Assert.Empty(chain.NodeIds());
            Assert.False(chain.TryGetNode(addedId, out _));
        }
        finally { chain.Clear(); }
    }

    /// <summary>Opens a scope, adds a node, and drops the scope on the floor —
    /// the mistake the finalizer exists for. Non-inlined so the scope cannot stay
    /// rooted by the caller's frame.</summary>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int LeakAScope(JuceDspChain chain)
    {
        _ = chain.BeginEdit();          // deliberately no `using`
        return chain.AddNode("gain").Id;
    }

    /// <summary>
    ///     <b>This expectation is deliberately REVERSED from the one that
    ///     shipped.</b> A nested begin used to be refused, which is the correct
    ///     shape for a transaction — but it refused from a state nothing could
    ///     clear: once a scope was dropped, EVERY <c>BeginEdit</c> on that seat
    ///     failed for the rest of the engine's life. A caller opening a new scope
    ///     has plainly abandoned the old one, so the engine now aborts the stale
    ///     scope, logs a Warning and opens a fresh one. The wrapper additionally
    ///     marks the superseded scope closed, which is the half tested here: the
    ///     old object's Dispose must NOT abort the scope that replaced it.
    /// </summary>
    [NativeAvailableFact]
    public void ANestedBeginEdit_NowSucceeds_AndTheSupersededScopeCannotAbortTheNewOne()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var stale = chain.BeginEdit();
            chain.AddNode("reverb");        // an edit the stale scope will lose

            var fresh = chain.BeginEdit();  // used to throw JuceAudioException
            Assert.True(chain.IsEditing);
            Assert.True(stale.IsClosed, "the superseded scope must be inert, or its Dispose kills the new one");
            Assert.False(fresh.IsClosed);

            stale.Dispose();                // the `using` of the abandoned scope, arriving late
            Assert.True(chain.IsEditing, "disposing the superseded scope aborted the scope that replaced it");

            // The stale scope's node went with it, and so did its wrapper.
            Assert.Empty(chain.NodeIds());

            var gain = chain.AddNode("gain");
            fresh.Commit();
            Assert.False(chain.IsEditing);
            Assert.Equal(new[] { gain.Id }, chain.NodeIds());
        }
        finally { chain.Clear(); }
    }

    // ---------------------------------------------------------------- #11

    [NativeAvailableFact]
    public void AnAbortedScope_PurgesTheNodesItAdded_SoTryGetNodeStopsResolvingThem()
    {
        var chain = Engine.GetDspChain();
        try
        {
            int addedId;
            using (chain.BeginEdit())
            {
                addedId = chain.AddNode("delay").Id;
                Assert.True(chain.TryGetNode(addedId, out _));
            }                                    // no Commit: abort

            Assert.False(chain.IsEditing);
            Assert.Empty(chain.NodeIds());
            Assert.False(chain.TryGetNode(addedId, out var phantom),
                "abortEdit destroyed the module, but the wrapper cache still resolved it — and a phantom "
                + "node answers false/\"\"/default/-1 on every poll-shaped member, which reads to a UI as "
                + "a real pedal that is bypassed and silent");
            Assert.Null(phantom);
        }
        finally { chain.Clear(); }
    }

    // ---------------------------------------------------------------- #12

    [NativeAvailableFact]
    public void AnAbortedScope_GivesBackTheSameNodeInstanceItEvicted()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var gain = chain.AddNode("gain");

            using (chain.BeginEdit())
            {
                chain.RemoveNode(gain);
                Assert.False(chain.TryGetNode(gain.Id, out _));   // gone from the shadow
            }                                                     // abort puts the MODULE back…

            Assert.True(chain.TryGetNode(gain.Id, out var back));
            Assert.Same(gain, back);                              // …and the WRAPPER with it
            Assert.Equal("gain", back.TypeId);
            Assert.Same(gain, Assert.Single(chain.Nodes));
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void AnAbortedScope_RestoresWrappersEvictedByClear()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var a = chain.AddNode("gain");
            var b = chain.AddNode("delay");

            using (chain.BeginEdit())
            {
                chain.Clear();
                Assert.Empty(chain.NodeIds());
                chain.AddNode("reverb");     // …and something the abort must NOT keep
            }

            var ids = chain.NodeIds();
            Assert.Equal(new[] { a.Id, b.Id }, ids);
            Assert.True(chain.TryGetNode(a.Id, out var backA));
            Assert.True(chain.TryGetNode(b.Id, out var backB));
            Assert.Same(a, backA);
            Assert.Same(b, backB);
            Assert.Equal(new[] { "gain", "delay" }, chain.Nodes.Select(n => n.TypeId).ToArray());
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     A scope that both ADDS and REMOVES the same node must not resurrect it:
    ///     <c>abortEdit</c> destroys everything in <c>editAdded_</c>, so replaying
    ///     the eviction journal blindly would re-create the #11 phantom through
    ///     the #12 fix.
    /// </summary>
    [NativeAvailableFact]
    public void AnAbortedScope_DoesNotResurrectANodeItBothAddedAndRemoved()
    {
        var chain = Engine.GetDspChain();
        try
        {
            int id;
            using (chain.BeginEdit())
            {
                var node = chain.AddNode("delay");
                id = node.Id;
                chain.RemoveNode(node);
            }

            Assert.Empty(chain.NodeIds());
            Assert.False(chain.TryGetNode(id, out _));
        }
        finally { chain.Clear(); }
    }

    // ---------------------------------------------------------------- commit

    /// <summary>A COMMITTED scope journals nothing back — the edits were
    /// published, so the cache and the engine already agree. Guards against the
    /// journal being replayed on the wrong path.</summary>
    [NativeAvailableFact]
    public void ACommittedScope_KeepsTheNodesItAdded()
    {
        var chain = Engine.GetDspChain();
        try
        {
            int id;
            using (var edit = chain.BeginEdit())
            {
                id = chain.AddNode("gain").Id;
                edit.Commit();
            }

            Assert.Equal(new[] { id }, chain.NodeIds());
            Assert.True(chain.TryGetNode(id, out var node));
            Assert.Equal("gain", node.TypeId);
        }
        finally { chain.Clear(); }
    }
}
