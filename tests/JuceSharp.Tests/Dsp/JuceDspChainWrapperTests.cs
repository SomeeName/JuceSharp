using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp.Dsp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     The managed tone-rig wrapper (<see cref="JuceDspChain" />,
///     <see cref="JuceDspNode" />, <see cref="JuceDspModuleCatalog" />).
///     <para>
///         Until 0.8.x the JuceSharp package exposed NONE of the rig: nineteen
///         header exports had no <c>NativeMethods</c> declaration at all, the other
///         half was declared and wrapped by nothing, and the only public surface
///         was the v30 meter quartet behind raw <c>nint</c> arguments. So a NuGet
///         consumer could not use the 57-module rig without re-declaring the C ABI
///         itself — which DotCharter does for thirty exports and the ToneRig
///         example did for twenty-three.
///     </para>
///     <para>
///         Everything here runs OFFLINE with the device stopped:
///         <c>dca_engine_create</c> opens one and the offline render path refuses
///         to run while it is open, which is the same discipline
///         <see cref="DspPerPlayerChainTests" /> uses.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class JuceDspChainWrapperTests : IDisposable
{
    private const int SampleRate = 48000;
    private const int Frames = 4096;

    private readonly JuceAudioEngine? _engine;

    public JuceDspChainWrapperTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop(); // the offline render must be the only processor
    }

    public void Dispose() => _engine?.Dispose();

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    // ---------------------------------------------------------------- catalogue

    /// <summary>
    /// The catalogue needs no engine — that is what lets a pedalboard UI build its
    /// palette before any device is open.
    /// </summary>
    [NativeAvailableFact]
    public void TheCatalogue_ListsEveryModuleTypeTheEngineCanBuild()
    {
        var modules = JuceDspModuleCatalog.Modules;

        // 100 is the shipped catalogue size after the 2026-09 gear wave (57 original
        // rows + 43 under src/dsp/gear/; JuceSharp/AGENTS.md § dsp). Asserted
        // as a floor plus an exact match against the native count, so ADDING a
        // module does not fail this while a module vanishing still does.
        //
        // THE FLOOR IS CONDITIONAL ON THE BUILD (W5-D9). A
        // -DDOTCHARTER_DSP_NEURAL=OFF build is a SUPPORTED configuration and it
        // drops exactly one row — `namamp`, the only neural module
        // (DspModuleFactory.cpp) — so a flat `>= 100` failed a build the docs
        // say ships. The floor follows the one row that can be absent rather
        // than being lowered for everybody, which would stop catching a
        // vanished module on the full build.
        Assert.Equal(NativeMethods.DspModuleTypeCount(), modules.Count);
        var hasNamAmp = JuceDspModuleCatalog.Find("namamp") is not null;
        var floor = 100 + (hasNamAmp ? 1 : 0);   // 101 since `rsamp` (DotCharter #88)
        Assert.True(modules.Count >= floor,
            $"catalogue has {modules.Count} module types, expected at least {floor} "
            + $"(namamp {(hasNamAmp ? "present" : "absent — DOTCHARTER_DSP_NEURAL=OFF")})");

        Assert.All(modules, m =>
        {
            Assert.False(string.IsNullOrWhiteSpace(m.Id));
            Assert.False(string.IsNullOrWhiteSpace(m.DisplayName));
            Assert.NotNull(m.Params);
            Assert.NotNull(m.StringParams);
        });
        Assert.Equal(modules.Count, modules.Select(m => m.Id).Distinct(StringComparer.Ordinal).Count());
        Assert.NotNull(JuceDspModuleCatalog.Find("delay"));
        Assert.Null(JuceDspModuleCatalog.Find("no-such-module"));
    }

    /// <summary>
    ///     A discrete param must declare itself as exactly one of two shapes, or a
    ///     UI parks a knob at "6.5 stages" while the core runs 6. The delay's
    ///     tempo-sync <c>division</c> is the NAMED case: it carries value labels and
    ///     wants a picker.
    /// </summary>
    [NativeAvailableFact]
    public void TheDelaysDivisionParam_ComesBackAsANamedDiscreteParamWithItsLabels()
    {
        var delay = JuceDspModuleCatalog.Find("delay");
        Assert.NotNull(delay);

        var division = delay!.Params.SingleOrDefault(p => p.Id == "division");
        Assert.NotNull(division);
        Assert.True(division!.IsNamed, "delay 'division' lost its value labels — the UI falls back to a bare number");
        Assert.True(division.IsDiscrete);
        Assert.NotNull(division.Labels);
        Assert.All(division.Labels!, l => Assert.False(string.IsNullOrWhiteSpace(l)));
        // value = Min + index, which is what Describe encodes.
        Assert.Equal(division.Labels[0], division.Describe(division.Min));
    }

    /// <summary>An ORDINAL discrete param carries a step instead, and a continuous
    /// one carries neither. Phaser "stages" is step 2, not 1 — the core forces the
    /// allpass count even.</summary>
    [NativeAvailableFact]
    public void AnOrdinalDiscreteParam_CarriesAStepAndNoLabels()
    {
        var phaser = JuceDspModuleCatalog.Find("phaser");
        Assert.NotNull(phaser);
        var stages = phaser!.Params.SingleOrDefault(p => p.Id == "stages");
        Assert.NotNull(stages);
        Assert.True(stages!.IsOrdinal, "phaser 'stages' lost its step: the knob stops snapping");
        Assert.False(stages.IsNamed);
        Assert.Equal(2f, stages.Step);
        Assert.Equal("4", stages.Describe(4f));
    }

    // ---------------------------------------------------------------- structure

    [NativeAvailableFact]
    public void AddParamsBypassMoveRemoveClear_RoundTripThroughTheWrapper()
    {
        var chain = Engine.GetDspChain();
        Assert.Equal(0, chain.NodeCount);

        var gain = chain.AddNode("gain");
        var delay = chain.AddNode("delay");
        Assert.Equal(2, chain.NodeCount);
        Assert.Equal("gain", gain.TypeId);
        Assert.Equal(0, gain.Player);
        Assert.Equal([gain.Id, delay.Id], chain.Nodes.Select(n => n.Id));

        // Params round trip, and the same node object comes back from Nodes.
        gain.SetParam("gainDb", 6f);
        Assert.Equal(6f, gain.GetParam("gainDb"), 3);
        Assert.Same(gain, chain.Nodes[0]);

        // ABI v33: the bypass flag comes back from the ENGINE, not from a
        // managed memory of what was written.
        Assert.False(gain.Bypass);
        gain.Bypass = true;
        Assert.True(gain.Bypass);
        gain.Bypass = false;
        Assert.False(gain.Bypass);

        // Move reorders; the node keeps its id.
        chain.MoveNode(gain, 1);
        Assert.Equal([delay.Id, gain.Id], chain.Nodes.Select(n => n.Id));

        chain.RemoveNode(delay);
        Assert.Equal(1, chain.NodeCount);
        Assert.Equal(gain.Id, chain.Nodes[0].Id);

        chain.Clear();
        Assert.Equal(0, chain.NodeCount);
        Assert.Empty(chain.Nodes);
    }

    /// <summary>Node ids are engine-global — <c>player * stride + local</c> — which
    /// is what lets every node-scoped export keep its pre-v28 signature. A caller
    /// must never have to compute that.</summary>
    [NativeAvailableFact]
    public void NodeIds_CarryTheirSeat()
    {
        var seat1 = Engine.GetDspChain(1);
        var node = seat1.AddNode("gain");
        try
        {
            Assert.Equal(1, node.Player);
            Assert.InRange(node.Id, JuceAudioEngine.PlayerNodeIdStride, 2 * JuceAudioEngine.PlayerNodeIdStride - 1);
            Assert.Equal(node.Id, seat1.ToGlobalNodeId(node.Id - JuceAudioEngine.PlayerNodeIdStride));
        }
        finally { seat1.Clear(); }
    }

    [NativeAvailableFact]
    public void TheSameChainObject_IsReturnedPerSeat_AndABadSeatIsRejected()
    {
        Assert.Same(Engine.GetDspChain(0), Engine.GetDspChain());
        Assert.NotSame(Engine.GetDspChain(0), Engine.GetDspChain(1));
        Assert.Throws<ArgumentOutOfRangeException>(() => Engine.GetDspChain(-1));
        Assert.Throws<ArgumentOutOfRangeException>(() => Engine.GetDspChain(JuceAudioEngine.MaxPlayers));
    }

    [NativeAvailableFact]
    public void AnUnknownModuleType_IsReportedRatherThanReturningANegativeId()
    {
        var ex = Assert.Throws<JuceAudioException>(() => Engine.GetDspChain().AddNode("definitely-not-a-module"));
        Assert.Contains("definitely-not-a-module", ex.Message, StringComparison.Ordinal);
    }

    // ---------------------------------------------------------------- params

    /// <summary>
    ///     Every core's clamp is NaN-transparent (<c>v &lt; lo</c> and
    ///     <c>v &gt; hi</c> are both false for a NaN), so a NaN that reaches the
    ///     coefficients silences the module for good. Rejected managed-side too, so
    ///     the refusal names the parameter.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(float.NaN)]
    [InlineData(float.PositiveInfinity)]
    [InlineData(float.NegativeInfinity)]
    public void ANonFiniteParam_IsRefusedAndNamesItself(float bad)
    {
        var chain = Engine.GetDspChain();
        var node = chain.AddNode("gain");
        try
        {
            var ex = Assert.Throws<ArgumentException>(() => node.SetParam("gainDb", bad));
            Assert.Contains("gainDb", ex.Message, StringComparison.Ordinal);
            // …and the real value is untouched.
            Assert.True(float.IsFinite(node.GetParam("gainDb")));
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void StringParams_RoundTrip()
    {
        var cab = JuceDspModuleCatalog.Find("cab");
        Assert.NotNull(cab);
        Assert.Contains(cab!.StringParams, p => p.Id == "irPath");

        var chain = Engine.GetDspChain();
        var node = chain.AddNode("cab");
        try
        {
            Assert.Equal(string.Empty, node.GetString("irPath")); // "" = bundled bank
            Assert.Throws<JuceAudioException>(() => node.GetString("noSuchStringParam"));
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void BranchTags_RoundTrip()
    {
        var chain = Engine.GetDspChain();
        var node = chain.AddNode("gain");
        try
        {
            Assert.Equal(0, node.Branch);
            node.Branch = 2;
            Assert.Equal(2, node.Branch);
            Assert.Throws<JuceAudioException>(() => node.Branch = 9); // out of 0..3
        }
        finally { chain.Clear(); }
    }

    // ---------------------------------------------------------------- offline

    /// <summary>
    ///     ABI v32's parked insertion is the live-preset-construction shape: a node
    ///     added parked is created at ZERO WET, so it is inaudible until it is
    ///     un-bypassed with its parameters already in place. Added wet, the user
    ///     hears the module's DEFAULTS first.
    /// </summary>
    [NativeAvailableFact]
    public void AParkedNode_IsInaudibleUntilItIsUnbypassed()
    {
        var chain = Engine.GetDspChain();
        var input = Impulseless();

        var dry = Render(chain, input);
        Assert.Equal(0, chain.NodeCount);

        var parked = chain.AddNodeBypassed("gain");
        try
        {
            Assert.True(parked.Bypass);
            parked.SetParam("gainDb", 12f);
            var whileParked = Render(chain, input);
            Assert.True(Rms(whileParked) < Rms(dry) * 1.02f + 1e-6f,
                $"a PARKED node changed the signal: parked RMS {Rms(whileParked)} vs dry {Rms(dry)}");

            parked.Bypass = false;
            var wet = Render(chain, input);
            Assert.True(Rms(wet) > Rms(dry) * 1.5f,
                $"un-bypassing did not fade the node in: wet RMS {Rms(wet)} vs dry {Rms(dry)}");
        }
        finally { chain.Clear(); }
    }

    /// <summary>Two seats hold two chains. Editing one must not move a sample of
    /// the other — the whole point of the ABI v28 per-seat rigs.</summary>
    [NativeAvailableFact]
    public void SeatsAreIsolated()
    {
        var seat0 = Engine.GetDspChain(0);
        var seat1 = Engine.GetDspChain(1);
        var input = Impulseless();
        try
        {
            var seat1Before = Render(seat1, input);

            var loud = seat0.AddNode("gain");
            loud.SetParam("gainDb", 12f);

            var seat1After = Render(seat1, input);
            var seat0After = Render(seat0, input);

            Assert.Equal(seat1Before, seat1After); // bit-for-bit
            Assert.True(Rms(seat0After) > Rms(seat1After) * 1.5f,
                "seat 0's gain node did not change seat 0 — the two seats look like one chain");
        }
        finally { seat0.Clear(); seat1.Clear(); }
    }

    [NativeAvailableFact]
    public void ProcessOffline_ValidatesItsBuffers()
    {
        var chain = Engine.GetDspChain();
        var buf = new float[64];
        Assert.Throws<ArgumentOutOfRangeException>(() => chain.ProcessOffline(buf, 3, SampleRate, buf));
        Assert.Throws<ArgumentException>(() => chain.ProcessOffline(buf, 2, SampleRate, new float[32]));
        Assert.Throws<ArgumentException>(() => chain.ProcessOffline(new float[65], 2, SampleRate, new float[65]));
    }

    // ---------------------------------------------------------------- meters

    /// <summary>
    ///     Metering is OFF until asked for — while it is off the DSP callback is
    ///     byte-for-byte the pre-v30 path — and turning it off ZEROES the meters so
    ///     a UI that stops polling leaves no frozen picture. The bulk read is
    ///     index-aligned with <see cref="JuceDspChain.Nodes" />.
    /// </summary>
    [NativeAvailableFact]
    public void Meters_AreOffUntilEnabled_AndReadInChainOrder()
    {
        var chain = Engine.GetDspChain();
        var a = chain.AddNode("gain");
        var b = chain.AddNode("gain");
        try
        {
            Assert.False(chain.MeteringEnabled);
            Assert.Equal(default, a.Meter);

            chain.MeteringEnabled = true;
            Assert.True(chain.MeteringEnabled);

            a.SetParam("gainDb", 12f);
            _ = Render(chain, Impulseless());

            // Sizing call, then the real read — the same contract as the ids list.
            Assert.True(chain.TryReadMeters([], out var sized));
            Assert.Equal(2, sized);
            Span<JuceDspNodeMeter> meters = stackalloc JuceDspNodeMeter[2];
            Assert.True(chain.TryReadMeters(meters, out var read));
            Assert.Equal(2, read);

            Assert.True(meters[0].OutputPeak > 0f, "the first node metered silence after a real render");
            Assert.Equal(a.Id, chain.Nodes[0].Id);
            Assert.Equal(b.Id, chain.Nodes[1].Id);

            chain.MeteringEnabled = false;
            Assert.False(chain.MeteringEnabled);
        }
        finally { chain.MeteringEnabled = false; chain.Clear(); }
    }

    // ---------------------------------------------------------------- latency

    /// <summary>
    ///     A SERIAL chain's latency is the sum of its nodes'. (Inside a split region
    ///     it is the MAX over the branches instead — they run in parallel — and that
    ///     is not a claim they are aligned.) A bypassed node reports 0: it is not in
    ///     the path, so a caller compensating for the rig must not budget for it.
    /// </summary>
    [NativeAvailableFact]
    public void ASerialChainsLatency_IsTheSumOfItsNodes()
    {
        var chain = Engine.GetDspChain();
        try
        {
            // Prepare the rig at a known rate first — latency is reported in
            // samples AT THE PREPARED RATE.
            chain.AddNode("gain");
            _ = Render(chain, Impulseless());

            var withLatency = JuceDspModuleCatalog.Modules
                .Select(m => m.Id)
                .Where(id => id is "whammy" or "harmonizer" or "klon")
                .ToArray();
            Assert.NotEmpty(withLatency);

            foreach (var id in withLatency) chain.AddNode(id);
            _ = Render(chain, Impulseless());

            var nodes = chain.Nodes;
            var sum = nodes.Sum(n => Math.Max(0, n.LatencySamples));
            Assert.True(sum > 0,
                "no node in a chain of pitch/neural modules reports latency — the adapters swallowed "
                + "core_.latencySamples() and dca_engine_dsp_get_chain_latency is lying");
            Assert.Equal(sum, chain.LatencySamples);

            // …and a bypassed node drops out of the total.
            var last = nodes[^1];
            var lastLatency = last.LatencySamples;
            if (lastLatency > 0)
            {
                last.Bypass = true;
                _ = Render(chain, Impulseless()); // let the fade land
                Assert.Equal(sum - lastLatency, chain.LatencySamples);
            }
        }
        finally { chain.Clear(); }
    }

    // ---------------------------------------------------------------- lifetime

    /// <summary>
    ///     Every rig call pins the engine handle, so one racing a dispose is a
    ///     caught <see cref="ObjectDisposedException" /> and never a read through
    ///     freed native memory. That is the whole difference between this wrapper
    ///     and binding <c>dca_engine_dsp_*</c> against the unpinned
    ///     <c>NativeHandle</c>.
    /// </summary>
    [NativeAvailableFact]
    public void EveryRigCallOnADisposedEngine_ThrowsObjectDisposed()
    {
        var engine = new JuceAudioEngine();
        engine.Stop();
        var chain = engine.GetDspChain();
        var node = chain.AddNode("gain");
        engine.Dispose();

        Assert.Throws<ObjectDisposedException>(() => engine.GetDspChain());
        Assert.Throws<ObjectDisposedException>(() => chain.AddNode("gain"));
        Assert.Throws<ObjectDisposedException>(() => chain.AddNodeBypassed("gain"));
        Assert.Throws<ObjectDisposedException>(() => chain.RemoveNode(node));
        Assert.Throws<ObjectDisposedException>(() => chain.MoveNode(node, 0));
        Assert.Throws<ObjectDisposedException>(() => chain.Clear());
        Assert.Throws<ObjectDisposedException>(() => _ = chain.NodeCount);
        Assert.Throws<ObjectDisposedException>(() => _ = chain.Nodes);
        Assert.Throws<ObjectDisposedException>(() => _ = chain.LatencySamples);
        Assert.Throws<ObjectDisposedException>(() => chain.MeteringEnabled = true);
        Assert.Throws<ObjectDisposedException>(() => chain.ProcessOffline(new float[8], 2, SampleRate, new float[8]));
        Assert.Throws<ObjectDisposedException>(() => node.SetParam("gainDb", 1f));
        Assert.Throws<ObjectDisposedException>(() => _ = node.GetParam("gainDb"));
        Assert.Throws<ObjectDisposedException>(() => node.SetString("irPath", ""));
        Assert.Throws<ObjectDisposedException>(() => _ = node.GetString("irPath"));
        Assert.Throws<ObjectDisposedException>(() => node.Bypass = true);
        Assert.Throws<ObjectDisposedException>(() => _ = node.Branch);

        // The poll-shaped members no-op instead of throwing, like every other
        // TryAcquire reader on this wrapper.
        Assert.False(chain.MeteringEnabled);
        // …and the real Try overload says WHY: false means the engine is gone,
        // which is the distinction the int-returning one (now [Obsolete]) cannot
        // make — it answers 0 for this and for "this seat has no nodes" alike.
        Assert.False(chain.TryReadMeters([], out var noNodes));
        Assert.Equal(0, noNodes);
        Assert.Equal(-1, node.LatencySamples);
        Assert.Equal(default, node.Meter);
        // ABI v33's two getters join them: TypeId falls back to the type the node
        // was CREATED with (a node's type never changes, so that is the last
        // honest answer anybody can give), Bypass reads false.
        Assert.Equal("gain", node.TypeId);
        Assert.False(node.Bypass);
    }

    // ------------------------------------------------- ABI v33 introspection

    /// <summary>
    ///     Every catalogue module must round-trip its type id through the ENGINE.
    ///     Before ABI v33 this was impossible: the wrapper remembered the id of
    ///     each node IT created and reported an empty string for anything else, so
    ///     a chain built by a second binding came back as bare integers a UI could
    ///     not look a parameter list up for.
    /// </summary>
    [NativeAvailableFact]
    public void EveryModuleType_ReportsItsOwnTypeIdBackFromTheEngine()
    {
        var chain = Engine.GetDspChain();
        try
        {
            foreach (var module in JuceDspModuleCatalog.Modules)
            {
                var node = chain.AddNode(module.Id);
                Assert.Equal(module.Id, node.TypeId);
            }
            Assert.Equal(JuceDspModuleCatalog.Modules.Count, chain.NodeCount);
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     The discriminating half: nodes this wrapper did NOT create. Adding them
    ///     through the raw ABI is exactly what a second binding against the same
    ///     engine does — before v33 they came back with an empty
    ///     <see cref="JuceDspNode.TypeId" /> and every one of them reading
    ///     un-bypassed, because the only source was what the wrapper had recorded.
    /// </summary>
    [NativeAvailableFact]
    public void ANodeThisWrapperDidNotCreate_StillKnowsItsTypeAndBypass()
    {
        var chain = Engine.GetDspChain();
        var h = Engine.NativeHandle;
        try
        {
            var delayId  = NativeMethods.EngineDspAddNodeForPlayer(h, 0, "delay");
            var reverbId = NativeMethods.EngineDspAddNodeBypassedForPlayer(h, 0, "reverb");
            Assert.True(delayId >= 0 && reverbId >= 0);

            var nodes = chain.Nodes;   // the wrapper has never seen either id
            Assert.Equal([delayId, reverbId], nodes.Select(n => n.Id));
            Assert.Equal("delay", nodes[0].TypeId);
            Assert.Equal("reverb", nodes[1].TypeId);
            Assert.False(nodes[0].Bypass);
            Assert.True(nodes[1].Bypass);

            // …and the bypass flag FOLLOWS the engine rather than a cache, even
            // when the change is made behind the wrapper's back.
            Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDspSetBypass(h, delayId, 1));
            Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDspSetBypass(h, reverbId, 0));
            Assert.True(nodes[0].Bypass);
            Assert.False(nodes[1].Bypass);
        }
        finally { chain.Clear(); }
    }

    /// <summary>An id the engine does not have reports "unknown" rather than
    /// guessing — a wrong type id would send a UI to the wrong parameter list —
    /// and a node id naming a seat the engine does not have is REJECTED, never
    /// clamped onto seat 0's node of the same local index.</summary>
    [NativeAvailableFact]
    public void AnUnknownNodeId_IsUnknownAndASeatPastTheLastIsRejected()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var real = chain.AddNode("gain");
            Assert.Equal("gain", real.TypeId);

            var h = Engine.NativeHandle;
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineDspGetNodeType(h, real.Id + 4242, null, 0, out var len));
            Assert.Equal(0, len);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineDspGetBypass(h, real.Id + 4242, out var bypassed));
            Assert.Equal(0, bypassed);

            // Seat DCA_MAX_PLAYERS does not exist. Its stride-aliased id must not
            // resolve to seat 0's node.
            var offSeat = NativeMethods.MaxPlayers * NativeMethods.PlayerNodeIdStride + real.Id;
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineDspGetNodeType(h, offSeat, null, 0, out _));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineDspGetBypass(h, offSeat, out _));
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     Every catalogue entry carries a category out of the engine's fixed set.
    ///     It was permanently empty before ABI v33 (<c>dca_dsp_module_type_info</c>
    ///     yields an id and a name and nothing else), so a palette had no drawer to
    ///     put anything in without a managed guess table — a second source of truth
    ///     for something only the native factory knows.
    /// </summary>
    [NativeAvailableFact]
    public void EveryCatalogueEntry_CarriesACategoryFromTheFixedSet()
    {
        string[] known =
        [
            "dynamics", "drive", "amp", "cab", "modulation", "delay",
            "reverb", "pitch", "filter", "eq", "utility", "routing", "synth"
        ];

        var modules = JuceDspModuleCatalog.Modules;
        Assert.All(modules, m =>
            Assert.True(known.Contains(m.Category, StringComparer.Ordinal),
                $"module '{m.Id}' has category '{m.Category}', which is not in the fixed set"));

        // Not vacuous through a single-category catalogue: the rig really does
        // span drawers, and two anchors are checked by name.
        Assert.True(modules.Select(m => m.Category).Distinct(StringComparer.Ordinal).Count() >= 8);
        Assert.Equal("delay", JuceDspModuleCatalog.Find("delay")!.Category);
        Assert.Equal("routing", JuceDspModuleCatalog.Find("splitter")!.Category);
    }

    // -------------------------------------------- ABI v33 multi-component VST3

    /// <summary>
    ///     A path that is not a VST3 bundle has no components, and that is an
    ///     ERROR rather than a plausible zero — a caller sizing a picker off it
    ///     would otherwise show an empty list for a plugin that failed to scan.
    /// </summary>
    [NativeAvailableFact]
    public void CountVst3Components_OnANonexistentPath_Fails()
    {
        var missing = Path.Combine(Path.GetTempPath(), $"no-such-{Guid.NewGuid():N}.vst3");
        Assert.Throws<FileNotFoundException>(() => JuceAudioEngine.CountVst3Components(missing));
        Assert.Throws<ArgumentException>(() => JuceAudioEngine.CountVst3Components("  "));
    }

    /// <summary>Loading component 1 of a bundle that is not there must fail
    /// exactly the way component 0 does. A new overload that reported a DIFFERENT
    /// error for the same missing file would send a host down a second, untested
    /// error path.</summary>
    [NativeAvailableFact]
    public void LoadPlugin_WithAComponentIndex_FailsLikeIndexZero()
    {
        var missing = Path.Combine(Path.GetTempPath(), $"no-such-{Guid.NewGuid():N}.vst3");

        var zero = Assert.ThrowsAny<Exception>(() => Engine.LoadPlugin(missing));
        var one = Assert.ThrowsAny<Exception>(() => Engine.LoadPlugin(missing, 1));
        Assert.Equal(zero.GetType(), one.GetType());

        Assert.Throws<ArgumentOutOfRangeException>(() => Engine.LoadPlugin(missing, -1));
    }

    // ---------------------------------------------------------------- lint

    /// <summary>
    ///     EVERY tone-rig export in the header must have a <c>NativeMethods</c>
    ///     declaration. Nineteen did not, and nothing noticed, because an export
    ///     with no binding is not a compile error anywhere — it is simply a feature
    ///     the package does not have. Reads the header text, so it fails in CI and
    ///     in a worktree with no DLL present.
    /// </summary>
    [Fact]
    public void EveryDspExportInTheHeader_HasANativeMethodsDeclaration()
    {
        var header = File.ReadAllText(FindHeader());
        var exported = Regex.Matches(header,
                @"DCA_API\s+\S+\s+DCA_CALL\s+(dca_\w+)\s*\(", RegexOptions.Multiline)
            .Select(m => m.Groups[1].Value)
            .Where(n => n.Contains("dsp", StringComparison.Ordinal)
                        || n.StartsWith("dca_droptune_", StringComparison.Ordinal)
                        // ABI v33's multi-component VST3 pair. Not "dsp", but the
                        // same class of gap: an export with no binding is simply a
                        // feature the package does not have, and nothing else says so.
                        || n.Contains("vst3_component", StringComparison.Ordinal))
            .Distinct(StringComparer.Ordinal)
            .ToArray();

        Assert.True(exported.Length >= 27, $"only found {exported.Length} rig exports in the header — regex rotted?");

        var bound = typeof(NativeMethods)
            .GetMethods(BindingFlags.NonPublic | BindingFlags.Static)
            .Select(m => (DllImportAttribute?)Attribute.GetCustomAttribute(m, typeof(DllImportAttribute)))
            .Where(a => a is not null)
            .Select(a => a!.EntryPoint)
            .ToHashSet(StringComparer.Ordinal);

        var unbound = exported.Where(e => !bound.Contains(e)).ToArray();
        Assert.True(unbound.Length == 0,
            "tone-rig exports with no NativeMethods declaration — the package simply does not have these "
            + "features, and nothing else would ever say so: " + string.Join(", ", unbound));
    }

    /// <summary>The rig's P/Invokes must live in <c>NativeMethods</c> with every
    /// other one: <c>NativeMethodsTests</c> and <c>NativeCallbackSafetyTests</c>
    /// reflect over <c>typeof(NativeMethods)</c> ONLY, so a private DllImport
    /// elsewhere in the package (as the v30 meter quartet was) is invisible to
    /// both the UTF-8 lint and the callback lint.</summary>
    [Fact]
    public void NoOtherTypeInThePackage_DeclaresADspPInvoke()
    {
        var offenders = typeof(JuceDspChain).Assembly.GetTypes()
            .Where(t => t != typeof(NativeMethods))
            .SelectMany(t => t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic
                                          | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly))
            .Select(m => (m, a: (DllImportAttribute?)Attribute.GetCustomAttribute(m, typeof(DllImportAttribute))))
            .Where(x => x.a is not null)
            .Select(x => $"{x.m.DeclaringType?.Name}.{x.m.Name} -> {x.a!.EntryPoint}")
            .ToArray();

        Assert.Empty(offenders);
    }

    private static string FindHeader()
    {
        const string relative = "src/JuceSharp.Native/include/dotcharter_audio.h";
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, relative);
            if (File.Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }

    // ---------------------------------------------------------------- helpers

    /// <summary>A steady 220 Hz tone: no transient, so a level comparison across
    /// two renders is about the rig and not about where a click landed.</summary>
    private static float[] Impulseless()
    {
        var buf = new float[Frames * 2];
        for (var i = 0; i < Frames; i++)
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
        double sum = 0;
        foreach (var s in buf) sum += (double)s * s;
        return (float)Math.Sqrt(sum / Math.Max(1, buf.Length));
    }
}
