using System;
using System.Collections.Generic;
using System.Linq;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     Wave-4 audit fixes that are observable from the managed side, offline.
///     Every case here FAILS without its fix; the comment on each says how.
///     <para>
///         Headless like its siblings: <c>dca_engine_create</c> opens a device,
///         so the fixture stops it immediately and the offline render path is the
///         only processor.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class Wave4DspChainFixTests : IDisposable
{
    private const int SampleRate = 48000;

    private readonly JuceAudioEngine? _engine;

    public Wave4DspChainFixTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose()
    {
        if (_engine is null) return;
        try { _engine.GetDspChain().Clear(); } catch { /* best effort */ }
        _engine.Dispose();
    }

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    // ---- W4-D51: the catalogue's range is enforced at ONE place -------------

    /// <summary>
    ///     A value outside a param's declared <c>[Min, Max]</c> is CLAMPED by
    ///     <c>DspChain::setParam</c>, so the value read back is one the DSP is
    ///     actually running.
    ///     <para>
    ///         Neither the ABI (non-finite only) nor the chain (NaN only) used to
    ///         clamp, and only 3 of 57 adapters did it themselves — so
    ///         <c>thresholdDb = +50</c> on a param declared <c>[-80, 0]</c> was
    ///         accepted, stored raw and read back as 50 while the core ran 0.
    ///         <c>DspModule.h</c> asserts the opposite, and this is the defect ABI
    ///         v33 fixed for <c>dca_source_set_pan</c> ("so a cached managed getter
    ///         cannot disagree with the engine") left unfixed one layer in.
    ///     </para>
    ///     <para>
    ///         Swept over the catalogue rather than spot-checked, because a
    ///         per-adapter convention is exactly what failed: this asserts the
    ///         property for every declared param of every module the factory can
    ///         build. Without the fix it fails on the first continuous param of
    ///         the first module.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void EveryCatalogueParam_ReadsBackInsideItsDeclaredRange()
    {
        var chain = Engine.GetDspChain();
        var failures = new List<string>();
        var checkedParams = 0;

        foreach (var module in JuceDspModuleCatalog.Modules)
        {
            JuceDspNode node;
            try { node = chain.AddNode(module.Id); }
            catch (JuceAudioException) { continue; } // a type this DLL cannot build
            try
            {
                foreach (var p in module.Params)
                {
                    if (!(p.Min <= p.Max)) continue;
                    var span = MathF.Max(1f, MathF.Abs(p.Max - p.Min));
                    foreach (var probe in new[] { p.Max + span, p.Min - span })
                    {
                        node.SetParam(p.Id, probe);
                        var read = node.GetParam(p.Id);
                        checkedParams++;
                        // A discrete param quantises, so allow one step of slack;
                        // a continuous one has no excuse at all.
                        var slack = MathF.Max(p.Step, 1e-4f * span);
                        if (read < p.Min - slack || read > p.Max + slack)
                            failures.Add($"{module.Id}.{p.Id}: set {probe}, read back {read}, declared [{p.Min}, {p.Max}]");
                    }
                }
            }
            finally { node.Remove(); }
        }

        Assert.True(checkedParams > 100, $"only {checkedParams} params were probed — the sweep is vacuous");
        Assert.True(failures.Count == 0,
            $"{failures.Count} param(s) read back outside the range the catalogue declares:\n"
            + string.Join("\n", failures.Take(20)));
    }

    // ---- W4-D45: the reported latency and the compensation agree -----------

    /// <summary>
    ///     Inside an edit scope, <c>LatencySamples</c> describes the PUBLISHED
    ///     chain — the one the callback is still running — not the shadow.
    ///     <para>
    ///         <c>recomputeBranchPdcLocked</c> states that rule in bold and all
    ///         five of its call sites obey it; <c>chainLatencySamples()</c>, the
    ///         backing of <c>dca_engine_dsp_get_chain_latency</c> that click
    ///         alignment consumes, walked <c>idOrder_</c> instead. So inside a
    ///         scope a node the scope had removed was still compensated for by the
    ///         mergers and no longer counted here, and a node it had added did the
    ///         reverse. The two are now ONE walk.
    ///     </para>
    ///     <para>
    ///         Without the fix the assertion inside the scope reads 0 instead of
    ///         the tapestop's 2.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void ChainLatency_InsideAnEditScope_DescribesThePublishedChain()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var late = chain.AddNode("tapestop");
            RenderSilence(chain, 512);              // prepare at THIS rate
            Assert.Equal(2, late.LatencySamples);
            Assert.Equal(2, chain.LatencySamples);

            using (var scope = chain.BeginEdit())
            {
                chain.RemoveNode(late);
                // The node is OUT of the shadow chain and STILL PLAYING out of
                // the published snapshot, so it still contributes.
                Assert.Equal(2, chain.LatencySamples);
                scope.Commit();
            }

            // ...and once the commit publishes the shadow, it does not.
            Assert.Equal(0, chain.LatencySamples);
        }
        finally { chain.Clear(); }
    }

    // ---- W4-D48: the pre-pass asks the SETTER's question -------------------

    /// <summary>
    ///     An automation event naming a POLL-ONLY parameter id is rejected by the
    ///     pre-pass, with its index, leaving the rig untouched.
    ///     <para>
    ///         <c>DspChain::hasParam</c> used to ask <c>getParam</c>, on the
    ///         stated grounds that it and <c>setParam</c> share one contract. They
    ///         do not: <c>namamp</c> answers <c>getParam</c> for three status ids
    ///         (<c>loaded</c>, <c>rateMismatch</c>, <c>resampling</c>) that its
    ///         <c>setParam</c> rejects. So the pre-pass accepted the event, the
    ///         render started, and <c>applyAutomationEvent</c> failed MID-RENDER —
    ///         leaving the seat's chain in exactly the arbitrary half-swept state
    ///         the pre-pass exists to prevent.
    ///     </para>
    ///     <para>
    ///         Without the fix this throws with the index of the LAST event
    ///         (mid-render, from the "defensive, cannot happen" branch) and the
    ///         earlier <c>outputDb</c> event has already been applied.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void AnAutomationEventNamingAPollOnlyParam_IsRejectedByThePrePass()
    {
        var chain = Engine.GetDspChain();
        JuceDspNode amp;
        try { amp = chain.AddNode("namamp"); }
        catch (JuceAudioException) { return; } // built without DOTCHARTER_DSP_NEURAL
        try
        {
            Assert.Equal(0f, amp.GetParam("outputDb"), 3);

            var buffer = new float[256 * 2];
            var events = new[]
            {
                JuceDspAutomationEvent.Create(0,  amp.Id, "outputDb", 6f),
                JuceDspAutomationEvent.Create(64, amp.Id, "loaded",   1f),
            };

            var ex = Assert.Throws<JuceAudioException>(
                () => chain.ProcessOffline(buffer, 2, SampleRate, events));
            Assert.Contains("automation event 1 was rejected", ex.Message);

            // The rig is untouched: the event the pre-pass DID accept was never
            // applied either, because nothing is applied until the whole list
            // resolves.
            Assert.Equal(0f, amp.GetParam("outputDb"), 3);
        }
        finally { chain.Clear(); }
    }

    // ---- W4-D72: the clean gain's ramp starts where the last block ended ----

    /// <summary>
    ///     <c>CleanGainModule</c>'s block ramp applies its START value to sample
    ///     0. It advanced first, so sample 0 was scaled by <c>start + inc</c> —
    ///     one whole increment of discontinuity against the previous block's last
    ///     sample, on every ramping block, which is the zipper the ramp exists to
    ///     remove one step smaller.
    ///     <c>juce::AudioBuffer::applyGainRamp</c>, the reference this code's own
    ///     comment invokes, applies <c>start</c> to sample 0.
    ///     <para>
    ///         Measured on DC so the gain IS the sample. The first render lands
    ///         the gain at unity; the second sweeps to +20 dB, and sample 0 must
    ///         still read 1.0 (not 1.0 + (10-1)/frames).
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void TheCleanGainRamp_AppliesItsStartValueToSampleZero()
    {
        const int frames = 64;
        var chain = Engine.GetDspChain();
        try
        {
            var gain = chain.AddNode("gain");
            gain.SetParam("gainDb", 0f);
            RenderDc(chain, frames);                 // prime + land at unity
            var landed = RenderDc(chain, frames)[0];
            Assert.Equal(1f, landed, 3);

            gain.SetParam("gainDb", 20f);            // -> x10 across this block
            var ramping = RenderDc(chain, frames);

            var increment = (10f - 1f) / frames;     // ~0.1406: not subtle
            Assert.True(MathF.Abs(ramping[0] - landed) < increment * 0.25f,
                $"sample 0 of the ramping block read {ramping[0]:F5}; it must continue from the "
                + $"previous block's landed gain {landed:F5}, not jump one increment ({increment:F5})");
            // ...and it still LANDS on the target, which is what makes the
            // assertion above a shift and not a broken ramp.
            Assert.True(MathF.Abs(ramping[frames - 1] - 10f) < 0.3f,
                $"the ramp ended at {ramping[frames - 1]:F5}, expected ~10");
        }
        finally { chain.Clear(); }
    }

    // ---- W4-D49: a narrowed branch comes back from SILENCE ------------------

    /// <summary>
    ///     A branch the splitter narrows away (3 → 2) and then re-enables
    ///     (2 → 3) must not replay the audio that was in its delay ring before
    ///     the narrowing.
    ///     <para>
    ///         While a branch is disabled the merger stops writing its PDC ring,
    ///         so the ring froze holding pre-narrowing audio and
    ///         <c>lastDelay_</c>/<c>last_</c> froze with it; widening back then
    ///         crossfaded from a stale tap INTO stale content at a stale gain. The
    ///         disabled branches' rings are now kept filled with silence and their
    ///         ramp state marked cold.
    ///     </para>
    ///     <para>
    ///         Only observable when the region carries a non-zero branch delay,
    ///         which is why there is a <c>tapestop</c> on branch 1. Without the
    ///         fix the silent render after the re-enable contains the loud tail.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void AReEnabledBranch_ComesBackFromSilence_NotFromItsStaleRing()
    {
        const int frames = 512;
        var chain = Engine.GetDspChain();
        try
        {
            var split = chain.AddNode("splitter");
            split.SetParam("enabledBranches", 3);
            chain.AddNode("tapestop").Branch = 1;   // gives branch 3 a PDC delay
            chain.AddNode("gain").Branch = 3;       // the branch that gets narrowed away
            chain.AddNode("merger");

            // WRAP the merger's PDC rings (8,192 samples per branch per channel)
            // with DC, so the slots the gap skips hold old LOUD audio rather than
            // the zeros prepare() left. Without the wrap there is nothing stale
            // to replay and the case passes either way — which the first version
            // of it did.
            for (var i = 0; i < 5; ++i) RenderDc(chain, 4096);

            // The gap has to be SHORTER than the branch delay, or the merger's
            // ring cursor simply outruns the stale content and there is nothing
            // left to replay — which is how the first version of this case
            // passed with the fix reverted.
            split.SetParam("enabledBranches", 2);   // branch 3 goes dark
            RenderSilence(chain, 2);

            split.SetParam("enabledBranches", 3);   // ...and comes back
            var after = RenderSilence(chain, frames);

            var worst = 0f;
            foreach (var s in after) worst = MathF.Max(worst, MathF.Abs(s));
            Assert.True(worst < 1e-3f,
                $"the re-enabled branch replayed pre-narrowing audio: worst |sample| {worst:F6}");
        }
        finally { chain.Clear(); }
    }

    // ---- helpers ------------------------------------------------------------

    private static float[] RenderDc(JuceDspChain chain, int frames)
    {
        var buf = new float[frames * 2];
        Array.Fill(buf, 1f);
        return RenderLeft(chain, buf, frames);
    }

    private static float[] RenderSilence(JuceDspChain chain, int frames)
        => RenderLeft(chain, new float[frames * 2], frames);

    private static float[] RenderLeft(JuceDspChain chain, float[] interleaved, int frames)
    {
        var output = new float[interleaved.Length];
        chain.ProcessOffline(interleaved, 2, SampleRate, output);
        var left = new float[frames];
        for (var i = 0; i < frames; ++i) left[i] = output[i * 2];
        return left;
    }
}
