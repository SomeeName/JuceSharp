using System;
using System.Runtime.InteropServices;
using JuceSharp;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     ABI v34 edit scopes. The native half — that a whole preset costs ONE
///     ordering publication instead of one per node — is measured by the RT gate
///     (<c>RtPresetCommitPublishesOncePerPresetNotOncePerNode</c>), which can
///     count publications while a callback runs. What is testable HERE is the
///     audible contract, offline and deterministically: a preset built inside a
///     scope is INAUDIBLE until the commit, and an aborted scope leaves the rig
///     exactly as it was.
/// </summary>
[Collection("NativeEngine")]
public sealed class DspEditScopeTests : IDisposable
{
    private const string Lib = "juce_sharp";
    private const int SampleRate = 48000;
    private const int Frames = 4096;

    private readonly JuceAudioEngine? _engine;

    public DspEditScopeTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        // dca_engine_create opens a device; the offline render must be the only
        // thing processing this rig.
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    // The stray-commit case has no managed entry point by design — the scope
    // type cannot express "commit something that is not open" — so it is driven
    // against the raw export, the way the other raw-ABI rig tests do.
    [DllImport(Lib, EntryPoint = "dca_engine_dsp_commit_edit_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspCommitEdit(nint engine, int player);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_abort_edit_for_player", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAbortEdit(nint engine, int player);

    [NativeAvailableFact]
    public void APresetBuiltInsideAScope_IsSilentUntilCommit_AndAudibleAfter()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var dry = Rms(Render(chain, Stimulus()));
            Assert.True(dry > 0.01f, "the stimulus itself is silent — the test is vacuous");

            using (var edit = chain.BeginEdit())
            {
                var gain = chain.AddNode("gain");
                gain.SetParam("gainDb", 12f);
                // The SHADOW has the node…
                Assert.True(chain.IsEditing);
                Assert.Equal(1, chain.NodeCount);
                // …and the audio does not. Nothing has been published, so the
                // chain the callback (and the offline render, which walks the
                // same snapshot) sees is still empty.
                var duringEdit = Rms(Render(chain, Stimulus()));
                Assert.Equal(dry, duringEdit, 4);

                edit.Commit();
            }

            Assert.False(chain.IsEditing);
            // One render to walk the 10 ms fade-in, then measure.
            Render(chain, Stimulus());
            var wet = Rms(Render(chain, Stimulus()));
            Assert.True(wet > dry * 2.0f,
                $"committing a +12 dB gain left the level at {wet:F4} against a dry {dry:F4}");
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void AnAbortedScope_RestoresTheStructureItStartedWith()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var gain = chain.AddNode("gain");
            gain.SetParam("gainDb", 12f);
            Render(chain, Stimulus());
            var before = Rms(Render(chain, Stimulus()));

            using (var edit = chain.BeginEdit())
            {
                chain.Clear();
                Assert.Equal(0, chain.NodeCount);      // the shadow is empty…
                chain.AddNode("reverb");
                chain.AddNode("delay");
                Assert.Equal(2, chain.NodeCount);
                // …and disposing without Commit throws all of it away.
            }

            Assert.False(chain.IsEditing);
            Assert.Equal(1, chain.NodeCount);
            var ids = chain.NodeIds();
            Assert.Single(ids);
            Assert.Equal(gain.Id, ids[0]);
            Assert.Equal("gain", new JuceDspChainProbe(chain).TypeOf(ids[0]));

            var after = Rms(Render(chain, Stimulus()));
            Assert.Equal(before, after, 3);
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     <b>The nested-begin half of this test is deliberately REVERSED from
    ///     what it asserted when it was written</b> (wave-2 finding #4). Refusing
    ///     a nested begin is the correct shape for a transaction, but the state it
    ///     refused from could not be cleared by any managed call: a caller who
    ///     dropped a scope left <c>editing_</c> set, so every later
    ///     <c>BeginEdit</c> on that seat failed for the rest of the ENGINE's life
    ///     while the rig published nothing to the audio callback. A caller opening
    ///     a new scope has plainly abandoned the old one, so the engine aborts the
    ///     stale scope, logs a Warning and opens a fresh one. Nesting is still not
    ///     supported — the inner begin DISCARDS the outer scope's edits rather
    ///     than stacking, which is what this asserts. The
    ///     superseded-scope-cannot-abort-the-new-one half lives in
    ///     <c>Wave2EditScopeLeakTests</c>.
    ///     <para>
    ///         The stray commit / abort half is UNCHANGED: with nothing open both
    ///         are still errors, not no-ops.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void ANestedBeginEdit_SupersedesTheOpenScope_AndACommitWithNoScopeOpenIsRejected()
    {
        var chain = Engine.GetDspChain();
        try
        {
            // Nothing open: a commit and an abort are both errors, not no-ops.
            Assert.NotEqual(0, DspCommitEdit(Engine.NativeHandle, 0));
            Assert.NotEqual(0, DspAbortEdit(Engine.NativeHandle, 0));

            var stale = chain.BeginEdit();
            chain.AddNode("reverb");
            Assert.Equal(1, chain.NodeCount);

            using var edit = chain.BeginEdit();   // used to throw
            Assert.True(stale.IsClosed);
            Assert.True(chain.IsEditing);
            // The stale scope's edits were thrown away with it.
            Assert.Equal(0, chain.NodeCount);

            chain.AddNode("gain");
            Assert.Equal(1, chain.NodeCount);
            edit.Commit();
            Assert.Equal(1, chain.NodeCount);
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void CommittingTwice_IsANoOp_AndDisposeAfterCommitDoesNotAbort()
    {
        var chain = Engine.GetDspChain();
        try
        {
            using (var edit = chain.BeginEdit())
            {
                chain.AddNode("gain");
                edit.Commit();
                edit.Commit();           // second commit: nothing to publish
                Assert.True(edit.IsClosed);
            }                            // dispose after commit must NOT abort
            Assert.Equal(1, chain.NodeCount);
            Assert.False(chain.IsEditing);
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void AScopeIsPerSeat_SoASecondSeatStillPublishes()
    {
        var seat0 = Engine.GetDspChain(0);
        var seat1 = Engine.GetDspChain(1);
        try
        {
            using var edit = seat0.BeginEdit();
            seat0.AddNode("gain");
            Assert.True(seat0.IsEditing);
            Assert.False(seat1.IsEditing);
            // Seat 1 is untouched by seat 0's transaction.
            seat1.AddNode("gain");
            Assert.Equal(1, seat1.NodeCount);
            edit.Commit();
        }
        finally
        {
            seat0.Clear();
            seat1.Clear();
        }
    }

    // ---- helpers ----------------------------------------------------------

    private static float[] Stimulus()
    {
        var buf = new float[Frames * 2];
        for (var i = 0; i < Frames; ++i)
        {
            var s = 0.25f * MathF.Sin(2f * MathF.PI * 220f * i / SampleRate);
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        return buf;
    }

    private static float[] Render(JuceDspChain chain, float[] input)
    {
        var output = new float[input.Length];
        chain.ProcessOffline(input, 2, SampleRate, output);
        return output;
    }

    private static float Rms(float[] buf)
    {
        double acc = 0;
        foreach (var s in buf) acc += (double)s * s;
        return buf.Length == 0 ? 0f : (float)Math.Sqrt(acc / buf.Length);
    }

    /// <summary>Reads a node's type id through the public node wrapper without
    /// needing the JuceDspNode instance the chain handed out.</summary>
    private readonly struct JuceDspChainProbe(JuceDspChain chain)
    {
        public string TypeOf(int nodeId)
        {
            foreach (var n in chain.Nodes)
                if (n.Id == nodeId)
                    return n.TypeId;
            return string.Empty;
        }
    }
}
