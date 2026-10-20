using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     The five modules of the 2026-09 <c>filter</c> gear wave — <c>svf</c>,
///     <c>comb</c>, <c>vocoder</c>, <c>talkbox</c> and <c>tilt</c>
///     (docs/guides/gear-filter.md). Four sit in the <c>filter</c> drawer;
///     <c>tilt</c> is a tone control and sits in <c>eq</c>.
///     <para>
///         Everything here renders through the OFFLINE rig export on a stopped
///         engine, exactly as <see cref="DspGainZipperTests" /> and
///         <see cref="DspSidechainTests" /> do. Frequency responses are measured
///         with SINE PROBES rather than off noise: the claim ("this mode is
///         −40 dB a decade above its cutoff") is a steady-state gain, and a
///         deterministic probe measures it without the variance a noise estimate
///         carries. Every window length is a whole number of periods of every
///         probe frequency, so the Goertzel below leaks nothing.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearFilterTests : IDisposable
{
    private const string Lib = "juce_sharp";

    /// The RAW setter, for the one thing the wrapper deliberately refuses to
    /// pass through: a NaN. JuceDspNode.SetParam throws in MANAGED code, so it
    /// can never prove the CHAIN rejects one.
    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetParamRaw(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, float value);

    private const int SampleRate = 48000;
    private const int Block = 2048;

    /// The block rendered AFTER a seam change: four ordinary blocks, so the
    /// "has the ramp landed" half of the seam probe is read 125 ms in rather
    /// than 13 ms in. Two of the cases need it — see the theory.
    private const int SeamFrames = 4 * Block;

    /// One second at 48 kHz: every integer frequency fits a whole number of
    /// periods, which is what makes <see cref="Goertzel" /> exact here.
    private const int ProbeFrames = 48000;

    /// Every module this wave added. The catalogue is the source of truth; this
    /// is the list of ids the tests sweep.
    public static readonly string[] Modules = { "svf", "comb", "vocoder", "talkbox", "tilt" };

    private readonly JuceAudioEngine? _engine;

    public GearFilterTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();     // the offline render must be the only processor
    }

    public void Dispose()
    {
        if (_engine is null) return;
        try { _engine.GetDspChain().Clear(); } catch (JuceAudioException) { /* engine already gone */ }
        _engine.Dispose();
    }

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    // =======================================================================
    // Catalogue shape
    // =======================================================================

    [NativeAvailableFact]
    public void EveryFilterModule_IsInTheCatalogue_InTheDrawerItBelongsIn()
    {
        foreach (var id in Modules)
        {
            var info = JuceDspModuleCatalog.Find(id);
            Assert.True(info is not null, $"'{id}' is not in the catalogue");
            // `tilt` is a TONE CONTROL. Filing it under "filter" would put a
            // one-knob EQ in the drawer a player opens looking for a wah.
            Assert.Equal(id == "tilt" ? "eq" : "filter", info!.Category);
            Assert.False(string.IsNullOrWhiteSpace(info.DisplayName), $"'{id}' has no display name");
            Assert.NotEmpty(info.Params);
        }
    }

    [NativeAvailableFact]
    public void EveryParam_HasASaneRange_AndADefaultInsideIt()
    {
        foreach (var id in Modules)
        {
            var info = JuceDspModuleCatalog.Find(id)!;
            foreach (var p in info.Params)
            {
                Assert.True(p.Max > p.Min, $"{id}.{p.Id}: max {p.Max} is not above min {p.Min}");
                Assert.InRange(p.Default, p.Min, p.Max);
                Assert.False(string.IsNullOrWhiteSpace(p.Name), $"{id}.{p.Id} has no display name");
                // A NAMED discrete param must cover its range exactly, or the
                // UI renders a picker with a hole in it.
                if (p.IsNamed)
                    Assert.Equal((int)MathF.Round(p.Max - p.Min) + 1, p.Labels!.Count);
            }
        }
    }

    /// <summary>
    ///     Every param this wave runs as an INT declares itself, and nothing
    ///     else pretends to be discrete. The set is pinned exactly: five NAMED
    ///     pickers and one ORDINAL count. A param the adapter <c>lround</c>s and
    ///     that does not say so parks a UI knob at "10 bands" while the DSP runs
    ///     12.
    /// </summary>
    [NativeAvailableFact]
    public void ExactlyTheIntegerParams_DeclareThemselves()
    {
        var discrete = (from id in Modules
                        from p in JuceDspModuleCatalog.Find(id)!.Params
                        where p.IsDiscrete
                        select $"{id}.{p.Id}").OrderBy(s => s, StringComparer.Ordinal).ToArray();
        Assert.Equal(
            new[] { "comb.polarity", "svf.mode", "svf.source", "talkbox.sweep", "vocoder.bands", "vocoder.carrier" },
            discrete);

        var mode = JuceDspModuleCatalog.Find("svf")!.Params.Single(p => p.Id == "mode");
        Assert.True(mode.IsNamed);
        Assert.Equal(new[] { "LowPass", "BandPass", "HighPass", "Notch" }, mode.Labels!.ToArray());
        Assert.Equal("Notch", mode.Describe(3f));

        // The ONE ordinal: 8, 12, 16, 20, 24. It is a COUNT, not a choice, so it
        // carries a step and not labels.
        var bands = JuceDspModuleCatalog.Find("vocoder")!.Params.Single(p => p.Id == "bands");
        Assert.False(bands.IsNamed);
        Assert.Equal(4f, bands.Step);
    }

    /// <summary>
    ///     A sidechain flag is a SECOND source of truth for a capability, so the
    ///     set is pinned exactly. Only the vocoder takes a key — and for it the
    ///     key is not a detector input but the CARRIER, which is the whole
    ///     feature. The other four have no external detector at all, and a
    ///     source one of them ignored would read as a live control that does
    ///     nothing.
    /// </summary>
    [NativeAvailableFact]
    public void ExactlyTheVocoder_AdvertisesASidechain()
    {
        var keyed = Modules.Where(id => JuceDspModuleCatalog.Find(id)!.SupportsSidechain).ToArray();
        Assert.Equal(new[] { "vocoder" }, keyed);

        // …and the engine agrees with the advertisement, in both directions.
        var chain = Engine.GetDspChain();
        try
        {
            foreach (var id in Modules)
            {
                var node = chain.AddNode(id);
                if (id == "vocoder")
                {
                    node.Sidechain = JuceDspSidechainSource.SeatInput;
                    Assert.Equal(JuceDspSidechainSource.SeatInput, node.Sidechain);
                }
                else
                {
                    var thrown = Assert.Throws<JuceAudioException>(
                        () => node.Sidechain = JuceDspSidechainSource.SeatInput);
                    Assert.Equal(JuceAudioStatus.InvalidArg, thrown.Status);
                }
            }
        }
        finally { chain.Clear(); }
    }

    [NativeAvailableFact]
    public void EveryParam_ReadsBackExactlyWhatWasStored()
    {
        var chain = Engine.GetDspChain();
        try
        {
            foreach (var id in Modules)
            {
                var node = chain.AddNode(id);
                foreach (var p in JuceDspModuleCatalog.Find(id)!.Params)
                {
                    foreach (var v in new[] { p.Min, p.Default, p.Min + (p.Max - p.Min) * 0.37f, p.Max })
                    {
                        var stored = p.IsDiscrete ? MathF.Round(v) : v;
                        node.SetParam(p.Id, stored);
                        Assert.Equal(stored, node.GetParam(p.Id), 4);
                    }
                }
            }
        }
        finally { chain.Clear(); }
    }

    // =======================================================================
    // svf
    // =======================================================================

    /// (mode, probe Hz, min dB, max dB) — the textbook shape of each tap of a
    /// 2-pole state-variable filter at Q 0.707, a decade either side of a
    /// 1 kHz cutoff. A filter whose taps were wired to the wrong outputs, or
    /// whose `mode` weights never reached 1, fails here rather than sounding
    /// subtly odd.
    public static TheoryData<int, float, float, float> ModeShapeCases => new()
    {
        { 0, 100f, -1.5f, 0.5f },       // LowPass: flat a decade below
        { 0, 10000f, -200f, -30f },     //          and gone a decade above
        { 2, 10000f, -1.5f, 0.5f },     // HighPass: the mirror
        { 2, 100f, -200f, -30f },
        { 1, 100f, -200f, -15f },       // BandPass: down on BOTH sides
        { 1, 10000f, -200f, -15f },
        { 1, 1000f, -1.5f, 0.5f },      //           and through at the centre
        { 3, 1000f, -200f, -30f },      // Notch: a hole exactly at the cutoff
    };

    [NativeAvailableTheory]
    [MemberData(nameof(ModeShapeCases))]
    public void Svf_EachMode_HasItsTextbookShape(int mode, float probeHz, float minDb, float maxDb)
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("svf");
            n.SetParam("mode", mode);
            n.SetParam("cutoffHz", 1000f);
            n.SetParam("resonance", 0f);     // Q 0.707, the Butterworth reference
            n.SetParam("drive", 0f);
            n.SetParam("source", 0f);        // Manual: nothing sweeps the cutoff
            n.SetParam("mix", 1f);

            var db = ProbeDb(chain, probeHz);
            Assert.InRange(db, minDb, maxDb);
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     The point of the saturator inside the resonance path: at full Q with
    ///     full drive the filter SINGS instead of running away. Ten seconds of
    ///     signal at the resonant peak is the worst case a player can hand it.
    /// </summary>
    [NativeAvailableFact]
    public void Svf_AtFullResonanceAndDrive_StaysBounded()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("svf");
            n.SetParam("mode", 1f);          // BandPass: straight at the peak
            n.SetParam("cutoffHz", 1000f);
            n.SetParam("resonance", 1f);
            n.SetParam("drive", 1f);
            n.SetParam("mix", 1f);

            var peak = 0f;
            var rms = 0f;
            for (var s = 0; s < 10; s++)     // ten one-second renders
            {
                var outp = Render(chain, Sine(ProbeFrames, s * ProbeFrames, 0.5f, 1000f));
                foreach (var v in outp)
                {
                    Assert.True(float.IsFinite(v), "the resonant filter emitted a non-finite sample");
                    peak = MathF.Max(peak, MathF.Abs(v));
                }

                rms = Rms(outp, 0, ProbeFrames);
            }

            // Vacuity: it has to still be MAKING the sound it is being asked to
            // bound. A filter that silenced itself passes any ceiling.
            Assert.True(rms > 0.01f, $"the resonant filter went quiet ({Db(rms):F1} dBFS RMS)");
            Assert.True(peak < 2f,
                $"a full-resonance, full-drive filter peaked at {Db(peak):F2} dBFS over ten seconds "
                + "— the saturator in the resonance path is not bounding it");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     The LFO source really sweeps: the output level of broadband input
    ///     through a swept lowpass rises and falls, and it does so at
    ///     <c>lfoRateHz</c> — proved by halving the rate and watching the cycle
    ///     count halve, not by trusting one number.
    /// </summary>
    [NativeAvailableFact]
    public void Svf_LfoSource_SweepsTheCutoff_AtItsRate()
    {
        var fast = SweepCycles(4f);
        var slow = SweepCycles(2f);
        var manual = SweepCycles(0f);

        Assert.True(manual.rangeDb < 2f,
            $"a MANUAL cutoff moved the level by {manual.rangeDb:F1} dB across a second — "
            + "the case cannot tell a sweep from its own stimulus");
        Assert.True(fast.rangeDb > 8f,
            $"a full-depth sweep moved the level by only {fast.rangeDb:F1} dB — nothing is sweeping");
        Assert.InRange(fast.cycles, 3, 5);
        Assert.InRange(slow.cycles, 1, 3);
        Assert.True(fast.cycles > slow.cycles,
            $"4 Hz produced {fast.cycles} cycles and 2 Hz produced {slow.cycles} — "
            + "the rate knob is not the rate");

        (int cycles, float rangeDb) SweepCycles(float rateHz)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("svf");
                n.SetParam("mode", 0f);                       // LowPass
                n.SetParam("cutoffHz", 1000f);
                n.SetParam("source", rateHz > 0f ? 1f : 0f);  // LFO or Manual
                n.SetParam("lfoRateHz", MathF.Max(rateHz, 0.05f));
                n.SetParam("lfoDepth", 1f);                   // the full +/- 4 octaves
                n.SetParam("mix", 1f);

                // A band-limited saw, NOT noise: the probe reads windowed RMS,
                // and a noise stimulus's own 10 ms RMS wanders 6 dB on its own —
                // measured, and it swallowed the whole vacuity guard.
                Render(chain, Saw(ProbeFrames, 0, 0.4f, 100f));          // settle
                var outp = Render(chain, Saw(ProbeFrames, ProbeFrames, 0.4f, 100f));
                return Cycles(Windows(outp, 480));            // 10 ms windows
            }
            finally { chain.Clear(); }
        }
    }

    /// <summary>
    ///     The envelope source opens on a loud burst: with the cutoff parked low
    ///     a quiet signal is filtered away, and the same signal played loudly
    ///     pushes the corner up past it.
    /// </summary>
    [NativeAvailableFact]
    public void Svf_EnvelopeSource_OpensOnALoudBurst()
    {
        var quiet = EnvelopeGainDb(0.02f);
        var loud = EnvelopeGainDb(0.8f);

        Assert.True(loud - quiet > 10f,
            $"a loud burst passed at {loud:F1} dB and a quiet one at {quiet:F1} dB — "
            + "the envelope is not opening the filter");

        float EnvelopeGainDb(float amp)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("svf");
                n.SetParam("mode", 0f);         // LowPass, parked well below the probe
                n.SetParam("cutoffHz", 100f);
                n.SetParam("resonance", 0f);
                n.SetParam("source", 2f);       // Envelope
                n.SetParam("envSens", 1f);
                n.SetParam("envAttackMs", 5f);
                n.SetParam("envReleaseMs", 50f);
                n.SetParam("mix", 1f);

                Render(chain, Sine(ProbeFrames, 0, amp, 2000f));   // let the follower settle
                var outp = Render(chain, Sine(ProbeFrames, ProbeFrames, amp, 2000f));
                // The GAIN, not the level: the two bursts are 32 dB apart to
                // begin with, so only the ratio says anything.
                return Db(Goertzel(outp, 2000f)) - Db(amp);
            }
            finally { chain.Clear(); }
        }
    }

    // =======================================================================
    // comb
    // =======================================================================

    /// <summary>
    ///     A resonant comb tuned to 220 Hz peaks at every multiple of 220 and
    ///     troughs half way between, and the gap is what makes it a comb rather
    ///     than a delay. Measured with sine probes at the exact peak and trough
    ///     frequencies, so the claim is a steady-state gain and not an estimate.
    /// </summary>
    [NativeAvailableFact]
    public void Comb_PeaksAtEveryMultipleOfItsFundamental()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("comb");
            n.SetParam("freqHz", 220f);
            n.SetParam("feedback", 0.9f);
            n.SetParam("damping", 0f);
            n.SetParam("polarity", 1f);     // Feedback
            n.SetParam("mix", 1f);

            for (var k = 1; k <= 5; k++)
            {
                var peak = ProbeDb(chain, 220f * k);
                var troughBelow = ProbeDb(chain, 220f * (k - 0.5f));
                var troughAbove = ProbeDb(chain, 220f * (k + 0.5f));
                var worst = MathF.Max(troughBelow, troughAbove);
                Assert.True(peak - worst >= 15f,
                    $"harmonic {k}: the comb peaked at {peak:F1} dB at {220 * k} Hz but only fell to "
                    + $"{worst:F1} dB between the teeth — that is {peak - worst:F1} dB of comb");
            }
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     Retuning GLIDES. The delay is smoothed in samples, so a 220 → 440
    ///     jump walks there over 30 ms; the tell that it does not is a
    ///     sample-to-sample STEP in the output, which a fractional delay line
    ///     that re-pointed in one sample produces and a smoothed one cannot.
    /// </summary>
    [NativeAvailableFact]
    public void Comb_Retuning_GlidesInsteadOfStepping()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("comb");
            n.SetParam("freqHz", 220f);
            n.SetParam("feedback", 0.6f);
            n.SetParam("damping", 0f);
            n.SetParam("mix", 1f);

            for (var b = 0; b < 6; b++) Render(chain, Sine(Block, b * Block, 0.3f, 220f));
            var steady = Render(chain, Sine(Block, 6 * Block, 0.3f, 220f));
            n.SetParam("freqHz", 440f);
            var seam = Render(chain, Sine(Block, 7 * Block, 0.3f, 220f));

            var steadyStep = MaxStep(steady);
            var seamStep = MaxStep(seam);
            Assert.True(steadyStep > 1e-4f, "the steady stimulus is flat — the case is vacuous");
            Assert.True(seamStep < steadyStep * 3f,
                $"retuning produced a {seamStep:E3} sample-to-sample step against {steadyStep:E3} in "
                + "the settled block — the delay re-pointed instead of gliding");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     At the top of the feedback range the comb is still a filter and not
    ///     an oscillator: the level STOPS GROWING. Asserting a ceiling alone
    ///     would pass on a core that merely clamps its own output.
    /// </summary>
    [NativeAvailableFact]
    public void Comb_AtMaximumFeedback_SettlesInsteadOfGrowing()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("comb");
            n.SetParam("freqHz", 220f);
            n.SetParam("feedback", 0.98f);
            n.SetParam("damping", 0f);
            n.SetParam("mix", 1f);

            var trail = new float[6];
            for (var s = 0; s < trail.Length; s++)
            {
                var outp = Render(chain, Noise(ProbeFrames, (uint)(s + 7)));
                Assert.All(outp, v => Assert.True(float.IsFinite(v), "the comb emitted a non-finite sample"));
                trail[s] = Rms(outp, 0, ProbeFrames);
            }

            Assert.True(trail[1] > 0.001f, "the comb is silent — the case is vacuous");
            Assert.True(trail[5] < trail[2] * 1.2f,
                $"the comb's level went {trail[2]:E3} -> {trail[5]:E3} over three more seconds at "
                + "feedback 0.98 — it is still growing");
            Assert.True(trail[5] < 20f, $"the comb reached {Db(trail[5]):F1} dBFS RMS");
        }
        finally { chain.Clear(); }
    }

    // =======================================================================
    // vocoder
    // =======================================================================

    /// <summary>
    ///     The whole claim of a channel vocoder: the carrier is heard only where
    ///     the MODULATOR has energy. A 440 Hz sine modulator opens the band that
    ///     contains 440 and no other, so of the saw carrier's harmonics only the
    ///     one at 440 survives.
    /// </summary>
    [NativeAvailableFact]
    public void Vocoder_PassesTheCarrier_OnlyWhereTheModulatorHasEnergy()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("vocoder");
            n.SetParam("bands", 16f);
            n.SetParam("carrier", 1f);      // Saw
            n.SetParam("pitchHz", 110f);
            n.SetParam("emphasis", 0f);
            n.SetParam("mix", 1f);

            Render(chain, Sine(ProbeFrames, 0, 0.5f, 440f));
            var outp = Render(chain, Sine(ProbeFrames, ProbeFrames, 0.5f, 440f));

            var opened = Db(Goertzel(outp, 440f));       // harmonic 4 of the carrier
            var closed = Db(Goertzel(outp, 2310f));      // harmonic 21, in a silent band
            Assert.True(opened > -60f, $"the opened band came out at {opened:F1} dBFS — nothing spoke");
            Assert.True(opened - closed > 12f,
                $"the band the modulator opened is only {opened - closed:F1} dB above one it did not "
                + $"({opened:F1} vs {closed:F1} dBFS)");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     Silence in is silence out. A vocoder runs its own oscillator, so
    ///     "nothing playing" is the one case where a missing gate is audible as
    ///     a synth drone nobody asked for.
    /// </summary>
    [NativeAvailableFact]
    public void Vocoder_WithNoModulator_IsSilent()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("vocoder");
            n.SetParam("carrier", 1f);
            n.SetParam("mix", 1f);

            Render(chain, Sine(ProbeFrames, 0, 0.5f, 440f));     // give it something to forget
            Render(chain, new float[ProbeFrames]);               // then let the followers fall
            var outp = Render(chain, new float[ProbeFrames]);
            Assert.True(Db(Rms(outp, 0, ProbeFrames)) < -60f,
                $"a silent input produced {Db(Rms(outp, 0, ProbeFrames)):F1} dBFS of carrier");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     Keyed, the vocoder's carrier is the KEY — so the same modulator with
    ///     a different key is a different sound. The chain is
    ///     <c>comb → vocoder</c> so that the key (the chain's raw input) and the
    ///     module's own input are genuinely different waveforms; without the
    ///     comb both carriers would be the same signal and the case could not
    ///     fail.
    /// </summary>
    [NativeAvailableFact]
    public void Vocoder_KeyedFromTheDryInput_UsesTheKeyAsItsCarrier()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var comb = chain.AddNode("comb");
            comb.SetParam("freqHz", 330f);
            comb.SetParam("feedback", 0.9f);
            comb.SetParam("mix", 1f);
            var voc = chain.AddNode("vocoder");
            voc.SetParam("carrier", 0f);      // Sidechain
            voc.SetParam("mix", 1f);

            float[] Run()
            {
                for (var b = 0; b < 4; b++) Render(chain, Saw(Block, b * Block, 0.4f, 110f));
                return Render(chain, Saw(Block, 4 * Block, 0.4f, 110f));
            }

            var unkeyed = Run();              // no key: the carrier is its OWN input
            voc.Sidechain = JuceDspSidechainSource.SeatInput;
            var keyed = Run();

            var level = Rms(unkeyed, 0, Block);
            Assert.True(level > 0.001f, "the unkeyed vocoder is silent — the case is vacuous");
            var delta = RmsDiff(keyed, unkeyed, 0, Block);
            Assert.True(delta > level * 0.3f,
                $"keying the vocoder moved its output by {delta:E3} against a level of {level:E3} — "
                + "the key is not reaching the carrier");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     …and keyed it still carries the MODULATOR's envelope: gate the input
    ///     and the output follows, within the attack and release the knobs ask
    ///     for. This is the <c>processSidechained</c> path, which is a different
    ///     entry point from the one every other test here reaches.
    /// </summary>
    [NativeAvailableFact]
    public void Vocoder_KeyedFromANoiseKey_FollowsTheModulatorsEnvelope()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("vocoder");
            n.SetParam("carrier", 0f);        // Sidechain — the noise key IS the carrier
            n.SetParam("attackMs", 5f);
            n.SetParam("releaseMs", 40f);
            n.SetParam("mix", 1f);
            n.Sidechain = JuceDspSidechainSource.SeatInput;

            const int half = ProbeFrames / 2;
            var gated = Noise(ProbeFrames, 11u);
            for (var i = half; i < ProbeFrames; i++) gated[i] = 0f;   // loud half, then silence

            Render(chain, gated);                      // settle the add crossfade
            var outp = Render(chain, gated);

            var loud = Rms(outp, half / 2, half / 2);                       // deep in the loud half
            var quiet = Rms(outp, half + ProbeFrames / 8, ProbeFrames / 8); // well past the release
            Assert.True(loud > 0.001f, "the keyed vocoder is silent — the case is vacuous");
            Assert.True(Db(loud) - Db(quiet) > 30f,
                $"the output sat at {Db(loud):F1} dBFS while the input played and {Db(quiet):F1} dBFS "
                + "after it stopped — the envelope is not gating the carrier");

            // …and it FOLLOWS rather than merely differing: within three release
            // times of the gate closing it is already most of the way down.
            const int threeReleases = 3 * 40 * SampleRate / 1000;
            var afterRelease = Rms(outp, half + threeReleases, 2048);
            Assert.True(Db(afterRelease) - Db(quiet) < 12f,
                $"120 ms after the gate closed the output was still {Db(afterRelease):F1} dBFS against "
                + $"a settled {Db(quiet):F1} — that is slower than the 40 ms release asked for");
        }
        finally { chain.Clear(); }
    }

    // =======================================================================
    // talkbox
    // =======================================================================

    /// (vowel, F1, F2) — the two ends of this core's own vowel table. A
    /// sawtooth has a harmonic every 100 Hz, so the two strongest peaks in the
    /// output must be the harmonics nearest F1 and F2, and 10 % is the widest a
    /// 100 Hz grid can miss by at these frequencies.
    public static TheoryData<float, float, float> VowelEndCases => new()
    {
        { 0f, 700f, 1220f },     // "A"
        { 1f, 300f, 870f },      // "U"
    };

    [NativeAvailableTheory]
    [MemberData(nameof(VowelEndCases))]
    public void Talkbox_AtAnEndVowel_PutsItsTwoStrongestPeaksOnF1AndF2(float vowel, float f1, float f2)
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("talkbox");
            n.SetParam("vowel", vowel);
            n.SetParam("sweep", 0f);        // Manual
            n.SetParam("resonance", 0.8f);
            n.SetParam("drive", 0f);        // no added harmonics to confuse the peak finder
            n.SetParam("mix", 1f);

            Render(chain, Saw(ProbeFrames, 0, 0.4f, 100f));
            var outp = Render(chain, Saw(ProbeFrames, ProbeFrames, 0.4f, 100f));

            // The harmonic grid, strongest first.
            var peaks = Enumerable.Range(1, 30)
                                  .Select(h => (hz: h * 100f, mag: Goertzel(outp, h * 100f)))
                                  .OrderByDescending(p => p.mag)
                                  .ToArray();
            Assert.True(peaks[0].mag > 1e-4f, "the talkbox produced nothing — the case is vacuous");

            var top = new[] { peaks[0].hz, peaks[1].hz }.OrderBy(h => h).ToArray();
            Assert.True(MathF.Abs(top[0] - f1) / f1 <= 0.1f,
                $"vowel {vowel}: the lower peak landed at {top[0]} Hz against an F1 of {f1} Hz");
            Assert.True(MathF.Abs(top[1] - f2) / f2 <= 0.1f,
                $"vowel {vowel}: the upper peak landed at {top[1]} Hz against an F2 of {f2} Hz");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     The LFO sweep moves the vowel periodically, at the rate the knob
    ///     asks for. Measured on the energy at "A"'s F1, which peaks once per
    ///     sweep cycle; halving the rate halves the count.
    /// </summary>
    [NativeAvailableFact]
    public void Talkbox_LfoSweep_MovesTheVowelPeriodically()
    {
        var fast = VowelCycles(4f);
        var slow = VowelCycles(2f);
        var manual = VowelCycles(0f);

        Assert.True(manual.rangeDb < 3f,
            $"a MANUAL vowel moved the F1 energy by {manual.rangeDb:F1} dB — the probe is not stable "
            + "enough to say anything");
        Assert.True(fast.rangeDb > 10f,
            $"an LFO sweep moved the F1 energy by only {fast.rangeDb:F1} dB — nothing is sweeping");
        Assert.InRange(fast.cycles, 3, 5);
        Assert.InRange(slow.cycles, 1, 3);

        (int cycles, float rangeDb) VowelCycles(float rateHz)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("talkbox");
                n.SetParam("sweep", rateHz > 0f ? 2f : 0f);   // LFO or Manual
                n.SetParam("lfoRateHz", MathF.Max(rateHz, 0.05f));
                n.SetParam("resonance", 0.8f);
                n.SetParam("drive", 0f);
                n.SetParam("mix", 1f);

                Render(chain, Saw(ProbeFrames, 0, 0.4f, 100f));
                var outp = Render(chain, Saw(ProbeFrames, ProbeFrames, 0.4f, 100f));

                // Energy at "A"'s F1, in 20 ms windows.
                return Cycles(WindowsAt(outp, 960, 700f));
            }
            finally { chain.Clear(); }
        }
    }

    /// <summary>
    ///     The three switches in this drawer that move a FULL-SCALE quantity,
    ///     graded as what they are: splices. AGENTS.md states the rule — "a
    ///     param may not STEP a modulator, a delay tap, or one callback's
    ///     workload" — and <c>dcdsp_svf.h</c>'s own <c>mode</c> taps are the
    ///     worked example; these three shipped without it.
    ///     <c>comb.polarity</c> swaps the wet signal between the recirculated
    ///     tap and the dry-plus-tap sum, which at <c>feedback 0.9</c> are
    ///     comparable in level — and it changes what the LINE means, so the
    ///     push crossfades too.
    ///     <para>
    ///         Graded the way <c>micpos.distanceCm</c> and
    ///         <c>limiter.lookaheadMs</c> are, and for the same reason: the
    ///         largest sample-to-sample step across the seam block, against the
    ///         same render with the knob left alone. The
    ///         <see cref="RampCases" /> proxy PASSES this with the hard switch
    ///         in place — comb's wet reads a tap written 218 samples ago at
    ///         220 Hz, so the early window still carries the old signal — where
    ///         this reads 9.1x the resting slope. The drawer's other two
    ///         full-scale switches need different graders again and have their
    ///         own tests below; a splice is not one measurement.
    ///     </para>
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("comb", "freqHz=220;feedback=0.9;damping=0.3;mix=1", "polarity", 1f, 0f, "tone220")]
    public void SwitchingAFullScaleSource_DoesNotSpliceTheWaveform(
        string module, string setup, string param, float before, float after, string stimulus)
    {
        var steady = MaxStep(RenderSeam(module, setup, param, before, before, stimulus));
        var switched = MaxStep(RenderSeam(module, setup, param, before, after, stimulus));

        Assert.True(steady > 0f, $"{module}: the render is silent — the case is vacuous");
        Assert.True(switched < 3f * steady,
            $"{module}.{param} stepped {switched:E3} against a resting {steady:E3} "
            + $"(x{switched / steady:F2}) — the switch spliced the waveform instead of "
            + "crossfading");

        static float MaxStep(float[] y)
        {
            var m = 0f;
            for (var i = 1; i < y.Length; i++) m = MathF.Max(m, MathF.Abs(y[i] - y[i - 1]));
            return m;
        }
    }

    /// <summary>
    ///     Switching <c>source</c> GLIDES the cutoff rather than jumping it.
    ///     It moves the cutoff by up to four octaves on a resonant TPT section,
    ///     so a hard switch is a jump of the whole filter; the core carries
    ///     three ramped weights over the three modulation DEPTHS (Manual
    ///     contributes 0), the shape its own <c>mode</c> taps already used.
    ///     <para>
    ///         Measured as an AMPLITUDE trajectory at a fixed probe, because
    ///         neither of this file's other graders works here: the
    ///         <see cref="RampCases" /> RmsDiff proxy is dominated by the phase
    ///         a resonant filter's cutoff carries (it reads a perfectly smooth
    ///         glide as 92 % of a step), and a sample-to-sample step comparison
    ///         drowns in the ringing of a Q-15 section. A 1 kHz probe sits well
    ///         above the 300 Hz resting cutoff and inside the LFO's reach, so
    ///         how much of it survives IS where the cutoff is; two cycles of it
    ///         fit in 96 samples, which is a fifth of the 10 ms fade.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void Svf_SwitchingTheModulationSource_GlidesTheCutoff_InsteadOfJumpingIt()
    {
        // A slow LFO, so its own drift across the measured window is nothing
        // next to the switch.
        const string setup = "mode=0;cutoffHz=300;resonance=0.9;lfoDepth=1;lfoRateHz=0.25;"
                             + "drive=0;mix=1";
        const int window = 96;               // two cycles of the 1 kHz probe

        var refBefore = At(0f, 0f);
        var refAfter = At(1f, 1f);
        var seam = At(0f, 1f);

        var span = MathF.Abs(refAfter - refBefore);
        Assert.True(span > 0.01f,
            $"Manual and LFO put the same amount of 1 kHz through ({refBefore:F4} vs "
            + $"{refAfter:F4}) — the case is vacuous");

        var ratio = MathF.Abs(seam - refBefore) / span;
        Assert.True(ratio < 0.5f,
            $"svf.source: {window} samples after the switch the cutoff is already "
            + $"{ratio * 100f:F0} % of the way to the LFO's — a glide is well under half, a "
            + "jump is 100 %");

        float At(float first, float second)
        {
            var y = RenderSeam("svf", setup, "source", first, second, "probe1k");
            return Goertzel(y[..window], 1000f);
        }
    }

    /// <summary>
    ///     Both of the svf's modulators run in every <c>source</c> mode, so
    ///     switching TO one does not resume it from a frozen phase. The core
    ///     used to advance the LFO in the MANUAL branch only — and its comment
    ///     on that line claimed the phase was "kept running so a source switch
    ///     does not jump", which was false for exactly the switch a player
    ///     makes: Env -> LFO.
    ///     <para>
    ///         Measured by rendering the LFO's own sweep trajectory two ways —
    ///         LFO selected throughout, and Env selected for the first 1.43
    ///         cycles of the LFO and then switched — and comparing the two. A
    ///         free-running phase makes them the same trajectory; a frozen one
    ///         restarts 0.43 of a cycle out. The rate is 0.7 Hz over 2 s ON
    ///         PURPOSE: at any rate whose period divides 2 s a frozen phase and
    ///         a free-running one agree, and the case would be vacuous.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void Svf_TheLfoKeepsRunningWhileAnotherSourceIsSelected()
    {
        const string setup = "mode=0;cutoffHz=300;resonance=0.9;lfoDepth=1;lfoRateHz=0.7;drive=0;mix=1";
        var free = Trajectory(startOnLfo: true);
        var switched = Trajectory(startOnLfo: false);

        // Non-vacuity: the trajectory has to MOVE, or two flat lines would agree
        // whatever the phase did.
        var span = free.Max() - free.Min();
        Assert.True(span > 0.02f,
            $"the LFO moved the output by only {span:F4} over a second — the case is vacuous");

        var worst = 0f;
        for (var i = 0; i < free.Length; i++) worst = MathF.Max(worst, MathF.Abs(free[i] - switched[i]));
        Assert.True(worst < 0.25f * span,
            $"after a 2 s stretch on the envelope source the LFO resumed {worst / span * 100f:F0} % "
            + "of its own span away from where a free-running phase would have put it");

        // 20 ms windows of the last second, which is the LFO's sweep written as
        // an amplitude trajectory through a resonant low-pass.
        float[] Trajectory(bool startOnLfo)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("svf");
                foreach (var pair in setup.Split(';', StringSplitOptions.RemoveEmptyEntries))
                {
                    var kv = pair.Split('=');
                    n.SetParam(kv[0], float.Parse(kv[1], CultureInfo.InvariantCulture));
                }

                n.SetParam("source", startOnLfo ? 1f : 2f);
                // 2 s == 1.4 cycles of the 0.7 Hz LFO.
                for (var b = 0; b < 2; b++) Render(chain, Saw(ProbeFrames, b * ProbeFrames, 0.4f, 110f));
                n.SetParam("source", 1f);
                var outp = Render(chain, Saw(ProbeFrames, 2 * ProbeFrames, 0.4f, 110f));
                return Windows(outp, 960);
            }
            finally { chain.Clear(); }
        }
    }

    /// <summary>
    ///     What the vocoder's constant-Q actually buys. <c>sqrt(r)/(r-1)</c> is
    ///     the Q that puts ONE RBJ bandpass at -3 dB at the geometric midpoint
    ///     between neighbouring centres; the bank cascades TWO of them, so a
    ///     band is -6 dB there and the coverage RIPPLES rather than summing
    ///     flat. The header said "adjacent bands cross at -3 dB" until
    ///     2026-09-12, which the code has never done. This measures the real
    ///     number so the claim can be checked rather than believed.
    /// </summary>
    [NativeAvailableFact]
    public void Vocoder_BandCoverageRipples_ByTheAmountItsFourthOrderBankImplies()
    {
        var chain = Engine.GetDspChain();
        try
        {
            chain.Clear();
            var n = chain.AddNode("vocoder");
            n.SetParam("bands", 8f);
            n.SetParam("carrier", 3f);          // Noise: a flat carrier in every band
            n.SetParam("emphasis", 0f);
            n.SetParam("formantShift", 0f);
            n.SetParam("attackMs", 1f);
            n.SetParam("releaseMs", 5f);
            n.SetParam("mix", 1f);

            // 8 bands, 80 Hz .. 8 kHz: ratio = (8000/80)^(1/7) = 1.9307.
            const float ratio = 1.930698f;
            var centres = new float[8];
            for (var i = 0; i < 8; i++) centres[i] = 80f * MathF.Pow(ratio, i);

            // Drive ONE band's centre and ONE midpoint at a time and read the
            // output back at the same frequency: what reaches the output is the
            // bank's coverage at that frequency.
            float At(float hz)
            {
                Render(chain, Sine(ProbeFrames, 0, 0.3f, hz));
                var outp = Render(chain, Sine(ProbeFrames, ProbeFrames, 0.3f, hz));
                return Db(Rms(outp, 0, ProbeFrames));
            }

            var onCentre = 0f;
            var onEdge = 0f;
            var count = 0;
            // The interior bands only: the two outermost have no neighbour on
            // one side, so their skirt is not a crossover.
            for (var i = 1; i < 6; i++)
            {
                onCentre += At(centres[i]);
                onEdge += At(MathF.Sqrt(centres[i] * centres[i + 1]));
                count++;
            }

            var ripple = onCentre / count - onEdge / count;
            // gear-filter.md quotes 4.2 dB, so the bound is two-sided and
            // centred on it (audit W5-A7's rule, applied to a number this wave
            // put on a page). Under 3 dB the bands would be summing nearly flat
            // and the -6 dB claim would be wrong; over 5.5 they are holes, and
            // a vocoder that reads as unintelligible gets blamed on `bands`.
            Assert.InRange(ripple, 3.0f, 5.5f);
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     The <c>vowel</c> knob stays LIVE in every <c>sweep</c> mode: the LFO
    ///     and the envelope OFFSET it, they do not replace it.
    ///     <c>dcdsp_svf.h</c>'s own modulators add to the smoothed cutoff and
    ///     that is the mental model a player carries over, but this core wrote
    ///     the modulator straight into the vowel target — so in both non-Manual
    ///     modes the knob did NOTHING, and in Env mode the sweep ran
    ///     <c>0 .. envSens</c>, which pins the pedal on "a" whenever
    ///     <c>envSens = 0</c> whatever the knob says. The page
    ///     (<c>docs/guides/gear-filter.md</c>) described the svf behaviour, not
    ///     this one.
    ///     <para>
    ///         Measured at <c>sweep = Envelope</c> with <c>envSens = 0</c>, i.e.
    ///         with the modulator contributing exactly zero: F1 at
    ///         <c>vowel = 0</c> ("a", 730 Hz) has to differ from F1 at
    ///         <c>vowel = 1</c> ("u", 300 Hz). Before the fix both sat on "a".
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void Talkbox_TheVowelKnobIsStillLive_WhenASweepSourceIsSelected()
    {
        var atA = Formant(0f);
        var atU = Formant(1f);

        // "a" is F1 730 Hz, "u" is F1 300 Hz. A knob the sweep has taken over
        // leaves both on whatever the sweep says, which is what this catches.
        Assert.True(atA > 550f, $"vowel 0 under an Envelope sweep put F1 at {atA:F0} Hz, not on \"a\"");
        Assert.True(atU < 450f, $"vowel 1 under an Envelope sweep put F1 at {atU:F0} Hz, not on \"u\" — "
                                + "the sweep source has taken the knob over");

        float Formant(float vowel)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("talkbox");
                n.SetParam("sweep", 1f);        // Envelope
                n.SetParam("envSens", 0f);      // ...contributing exactly nothing
                n.SetParam("vowel", vowel);
                n.SetParam("resonance", 0.9f);
                n.SetParam("drive", 0f);
                n.SetParam("mix", 1f);

                Render(chain, Saw(ProbeFrames, 0, 0.4f, 100f));
                var outp = Render(chain, Saw(ProbeFrames, ProbeFrames, 0.4f, 100f));

                // The strongest harmonic of the 100 Hz saw below 1.2 kHz IS F1:
                // the formant tilt is fixed F1 > F2 > F3, so nothing above can
                // outrank it.
                var best = 0f;
                var bestHz = 0f;
                for (var h = 2; h <= 12; h++)
                {
                    var mag = Goertzel(outp, h * 100f);
                    if (mag > best) { best = mag; bestHz = h * 100f; }
                }

                return bestHz;
            }
            finally { chain.Clear(); }
        }
    }

    // =======================================================================
    // tilt
    // =======================================================================

    [NativeAvailableTheory]
    [InlineData(12f)]
    [InlineData(-12f)]
    [InlineData(6f)]
    public void Tilt_RocksTheSpectrumAboutItsPivot(float tilt)
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("tilt");
            n.SetParam("tilt", tilt);
            n.SetParam("pivotHz", 1000f);
            n.SetParam("level", 0f);

            var low = ProbeDb(chain, 100f);
            var mid = ProbeDb(chain, 1000f);
            var high = ProbeDb(chain, 10000f);

            Assert.True(MathF.Abs(high - low - tilt) <= 1.5f,
                $"tilt {tilt:F0}: 10 kHz sits {high - low:F2} dB above 100 Hz "
                + $"({high:F2} vs {low:F2})");
            Assert.True(MathF.Abs(mid) <= 0.5f,
                $"tilt {tilt:F0}: the pivot moved by {mid:F2} dB — a tilt rocks ABOUT it");
        }
        finally { chain.Clear(); }
    }

    // =======================================================================
    // Cross-cutting: ramps, NaN, rate, block independence, source shape
    // =======================================================================

    /// (module, setup, param, before, after, stimulus) — every user-facing gain,
    /// mix, level and coefficient sweep this wave added. Modelled on
    /// <see cref="DspGainZipperTests" />' MixCases: three renders of the same
    /// stimulus (never moved / moved from the start / moved at the seam), and
    /// the seam must still be near the OLD value 128 samples in. A per-block
    /// constant is 100 % of the way from sample 0.
    public static TheoryData<string, string, string, float, float, string> RampCases => new()
    {
        // A cutoff well UNDER the stimulus, so dry and wet are far apart.
        { "svf", "mode=0;cutoffHz=60;resonance=0.3;source=0", "mix", 0f, 1f, "sine" },
        { "svf", "mode=0;resonance=0.3;source=0;mix=1", "cutoffHz", 60f, 3000f, "sine" },
        { "comb", "freqHz=220;feedback=0.6;damping=0", "mix", 0f, 1f, "tone220" },
        // 220 is a PEAK of a 220 Hz comb and a trough of a 440 Hz one, which is
        // the widest A/B a retune has.
        { "comb", "feedback=0.6;damping=0;mix=1", "freqHz", 220f, 440f, "tone220" },
        { "vocoder", "carrier=1;pitchHz=110;emphasis=0", "mix", 0f, 1f, "saw" },
        // The one STRUCTURE change a core in this drawer makes for itself: two
        // whole banks, crossfaded.
        { "vocoder", "carrier=1;pitchHz=110;emphasis=0;mix=1", "bands", 8f, 24f, "saw" },
        { "talkbox", "sweep=0;resonance=0.8;drive=0", "mix", 0f, 1f, "saw" },
        { "talkbox", "sweep=0;resonance=0.8;drive=0;mix=1", "vowel", 0f, 1f, "saw" },
        { "tilt", "tilt=0;pivotHz=1000", "level", 0f, 12f, "saw" },
        { "tilt", "level=0;pivotHz=1000", "tilt", 0f, 12f, "saw" },
        { "tilt", "level=0;tilt=12", "pivotHz", 200f, 5000f, "saw" },
        // vocoder.formantShift, added 2026-09-12: it re-designs all 24
        // SYNTHESIS sections at Q up to 24, by up to an octave, with their
        // TDF-II state retained — the same discontinuity a `bands` change
        // makes, on a knob a user DRAGS. It now goes through the same two-bank
        // crossfade; measured 51 % of the way at sample 0 before that.
        { "vocoder", "carrier=1;pitchHz=110;bands=16;emphasis=0;mix=1", "formantShift", 0f, 12f, "saw" },
        // comb.polarity, vocoder.carrier and svf.source are deliberately NOT
        // here, for the reason GearAmpCabTests excludes micpos.distanceCm: this
        // theory's RmsDiff proxy cannot grade them.
        //   * comb's wet reads a delay TAP written 218 samples ago at 220 Hz,
        //     so the early window still carries the OLD signal whatever the
        //     switch did — measured: the hard switch passes this theory.
        //   * the vocoder's carrier reaches the output through 16 bands at Q up
        //     to 24, whose ringing smears a full-scale step over far more than
        //     the early window — the hard switch passes this theory too.
        //   * svf.source moves the CUTOFF, and `fc = 2^(logFc + octaves)` is
        //     exponential in the ramp weight, so 27 % of the way through a
        //     perfectly smooth fade the filter is already an octave out and the
        //     proxy grades it as a step (measured: 92 %).
        // All three are graded below as what they are: SPLICES.
    };

    [NativeAvailableTheory]
    [MemberData(nameof(RampCases))]
    public void ChangingAParam_WalksAcrossTheSeam_InsteadOfStepping(
        string module, string setup, string param, float before, float after, string stimulus)
    {
        var refBefore = RenderSeam(module, setup, param, before, before, stimulus);
        var refAfter = RenderSeam(module, setup, param, after, after, stimulus);
        var seam = RenderSeam(module, setup, param, before, after, stimulus);

        const int earlyWindow = 128;   // ~27 % of the shortest (10 ms) ramp at 48 kHz
        // 125 ms in. The seam render is FOUR blocks, not one, because two of
        // these cases need it: a Q-20 formant rings for ~21 ms after the vowel
        // smoother has landed, and a vocoder bank swap hands over to a bank
        // whose filters and followers start COLD — measured 90 % and 143 % away
        // from the settled reference at 648 samples, which is the cores being
        // honest rather than the ramps failing to land.
        const int lateStart = 6000;

        var earlySpan = RmsDiff(refBefore, refAfter, 0, earlyWindow);
        var lateSpan = RmsDiff(refBefore, refAfter, lateStart, SeamFrames - lateStart);
        Assert.True(earlySpan > 0.005f,
            $"{module}.{param}: the two settings are indistinguishable at the seam "
            + $"(early span {earlySpan:F5}) — the case is vacuous");
        Assert.True(lateSpan > 0.005f,
            $"{module}.{param}: the two settings are indistinguishable later in the block "
            + $"(late span {lateSpan:F5}) — the case is vacuous");

        var earlyRatio = RmsDiff(seam, refBefore, 0, earlyWindow) / earlySpan;
        Assert.True(earlyRatio < 0.5f,
            $"{module}.{param}: the first {earlyWindow} samples after the change are "
            + $"{earlyRatio * 100f:F0} % of the way to the new setting — a ramp is well under "
            + "half, a hard step is 100 %");

        var lateRatio = RmsDiff(seam, refAfter, lateStart, SeamFrames - lateStart) / lateSpan;
        Assert.True(lateRatio < 0.35f,
            $"{module}.{param}: {lateStart} samples later the seam is still "
            + $"{lateRatio * 100f:F0} % away from the new setting — the ramp never landed");
    }

    /// <summary>
    ///     A NaN is REJECTED at the chain, and the module survives it. This
    ///     matters more here than anywhere: every clamp in the tree is
    ///     NaN-transparent, and a NaN that reached a RESONANT filter's
    ///     coefficients is permanent — <c>reset()</c> clears state, never
    ///     coefficients, so the node would be silent for the life of the chain.
    /// </summary>
    [NativeAvailableFact]
    public void ANaNParam_IsRejected_AndTheModuleStillWorks()
    {
        var chain = Engine.GetDspChain();
        try
        {
            foreach (var id in Modules)
            {
                chain.Clear();
                var node = chain.AddNode(id);
                foreach (var p in JuceDspModuleCatalog.Find(id)!.Params)
                {
                    Assert.NotEqual(0, DspSetParamRaw(Engine.NativeHandle, node.Id, p.Id, float.NaN));
                    Assert.NotEqual(0, DspSetParamRaw(Engine.NativeHandle, node.Id, p.Id, float.PositiveInfinity));
                    Assert.True(float.IsFinite(node.GetParam(p.Id)),
                        $"{id}.{p.Id} kept a non-finite value after a rejected write");
                }

                for (var b = 0; b < 4; b++) Render(chain, Saw(Block, b * Block, 0.4f, 220f));
                var outp = Render(chain, Saw(Block, 4 * Block, 0.4f, 220f));
                Assert.All(outp, s => Assert.True(float.IsFinite(s), $"{id} emitted a non-finite sample"));
                Assert.True(Rms(outp, 0, Block) > 0.01f, $"{id} went silent after a rejected NaN write");
            }
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     A rejected NaN must land on a param's NEUTRAL value, never on the
    ///     BOTTOM of its range. For a symmetric param those are opposite ends:
    ///     <c>comb.feedback</c> became a near-maximal INVERTED resonator,
    ///     <c>tilt.tilt</c> a full −12 dB tilt and <c>acpre.bass</c> a −12 dB
    ///     shelf, i.e. the loudest possible answer to "this value is garbage".
    ///     <c>dcdsp_transient.h</c>, <c>dcdsp_multiband.h</c> and
    ///     <c>dcdsp_bassamp.h</c> already carried a <c>fallback</c> argument for
    ///     exactly this, and <c>dcdsp_primitives.h</c> documents
    ///     <c>clampGainDb</c>'s "NaN -> 0 dB" the same way; the rule was applied
    ///     in two of the four 2026-09 gear drawers and not the other two.
    ///     <para>
    ///         This is DEFENCE IN DEPTH and says so:
    ///         <c>DspChain::setParam</c> rejects a NaN before any adapter sees
    ///         one, which is what <see cref="ANaNParam_IsRejected_AndTheModuleStillWorks" />
    ///         really proves. It matters because a core is not allowed to depend
    ///         on a caller it cannot see — the RT gate's oversized-block probes,
    ///         <c>tools/</c> and any future offline path construct cores
    ///         directly. So it is linted at the SHAPE level, over the source,
    ///         rather than measured through a chain that can never deliver the
    ///         input.
    ///     </para>
    /// </summary>
    [Fact]
    public void EverySymmetricParam_RejectsANaNOntoItsNeutral_NotOntoTheBottomOfItsRange()
    {
        // Every core of the four 2026-09 gear drawers that clamps a param with
        // a NEGATIVE lower bound.
        string[] cores =
        {
            "comb", "tilt", "svf", "vocoder", "talkbox",
            "acpre", "bassamp", "poweramp", "micpos",
            "limiter", "transient", "multiband", "expander", "optocomp", "ducker",
            "rat", "ds1", "hm2", "tapesat", "bassod", "tubepre", "octfuzz",
        };
        // PARAM clamps only. The same helpers also guard the OUTPUT
        // (`clampf(y, -24, 24)`), and there `lo` is unreachable by a NaN: every
        // one of those sites does `if (!isfinite(y)) y = 0;` on the line above.
        var call = new Regex(@"\b(clampf|clampRange|clampGainDb)\(\s*(p\.|p_\.)([^()]*)\)",
                             RegexOptions.Compiled);

        var offenders = new List<string>();
        foreach (var core in cores)
        {
            var path = $"src/JuceSharp.Native/src/dsp/cores/dcdsp_{core}.h";
            var source = StripComments(WrapperLifetimeAuditTests.ReadRepoFile(path));
            foreach (Match m in call.Matches(source))
            {
                // field, lo, hi [, fallback]
                var args = m.Groups[3].Value.Split(',').Select(a => a.Trim()).ToArray();
                if (args.Length != 3) continue;                 // already carries a fallback
                if (!args[1].StartsWith('-')) continue;         // lower bound IS the neutral
                offenders.Add($"{core}: {m.Value}");
            }
        }

        Assert.True(offenders.Count == 0,
            "these clamps send a rejected NaN to the most extreme setting the knob has, "
            + "instead of to the param's neutral: " + string.Join("; ", offenders));
    }

    /// <summary>
    ///     No bare <c>std::lround</c> anywhere in the four 2026-09 gear drawers
    ///     this file's author owns. <c>std::lround(NaN)</c> has an UNSPECIFIED
    ///     result and raises <c>FE_INVALID</c>; the core's own
    ///     <c>clampIndex</c> then defends the SOUND, but <c>getParam</c> hands
    ///     the raw stored int straight back to the UI as
    ///     <c>(float) INT_MIN</c>. <c>FilterGear.cpp</c> wrote a
    ///     <c>quantise(v, fallback)</c> helper for exactly this and
    ///     <c>DynamicsGear.cpp</c> did it inline for one param;
    ///     <c>DriveGear.cpp</c> and <c>AmpCabGear.cpp</c> had seven unguarded
    ///     sites between them.
    ///     <para>
    ///         The rule is spelled as "a line that calls <c>std::lround</c> must
    ///         also test <c>std::isfinite</c>", which is true of the helper's
    ///         one line and of nothing else, so every call site has to go
    ///         through it. The five drawers this author does not own are NOT
    ///         swept: they carry the same defect (20 sites) and belong to their
    ///         own agents.
    ///     </para>
    /// </summary>
    [Fact]
    public void EveryDiscreteParam_RefusesANaNBeforeItReachesLround()
    {
        string[] files = { "Dynamics", "Drive", "AmpCab", "Filter", "RsAmp" };
        var offenders = new List<string>();
        foreach (var f in files)
        {
            var path = $"src/JuceSharp.Native/src/dsp/gear/{f}Gear.cpp";
            var source = StripComments(WrapperLifetimeAuditTests.ReadRepoFile(path));
            var lines = source.Split('\n');
            for (var i = 0; i < lines.Length; i++)
            {
                if (!lines[i].Contains("std::lround", StringComparison.Ordinal)) continue;
                if (lines[i].Contains("std::isfinite", StringComparison.Ordinal)) continue;
                offenders.Add($"{f}Gear.cpp:{i + 1}: {lines[i].Trim()}");
            }
        }

        Assert.True(offenders.Count == 0,
            "these adapters hand a NaN straight to std::lround: " + string.Join("; ", offenders));
    }

    /// <summary>
    ///     The vocoder's <c>setParams</c> does NOT re-design the filter bank.
    ///     It used to run <c>retuneBankImpl</c> unconditionally on EVERY param
    ///     write, <c>mix</c> included: at 24 bands that is 96
    ///     <c>setBandpass</c> designs plus 96 <c>EnvelopeFollower::setTimes</c>
    ///     plus 24 <c>std::pow</c> — roughly 300 transcendentals per accepted
    ///     write, on the audio thread, once per block while a knob is being
    ///     dragged. Nothing is allocated, so the RT gate is silent about it,
    ///     which is exactly why this is a SHAPE assertion over the source.
    ///     <para>
    ///         The design half now runs only from <c>configureBank</c>, i.e.
    ///         only on an IDLE bank behind the two-bank crossfade, and the
    ///         follower half only when <c>attackMs</c> / <c>releaseMs</c> moved.
    ///     </para>
    /// </summary>
    [Fact]
    public void TheVocoderDoesNotReDesignItsBank_OnEveryParamWrite()
    {
        var source = StripComments(
            WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp.Native/src/dsp/cores/dcdsp_vocoder.h"));
        var at = source.IndexOf("void setParams(", StringComparison.Ordinal);
        Assert.True(at >= 0, "dcdsp_vocoder.h has no setParams() — repoint this lint");
        var body = Body(source, at);
        Assert.True(body.Length > 200, $"setParams() body looks empty ({body.Length} chars)");

        Assert.DoesNotContain("designBank(", body, StringComparison.Ordinal);
        Assert.DoesNotContain("configureBank(", body, StringComparison.Ordinal);
        // ...and the guard that makes the follower half conditional is there.
        Assert.Contains("lastAttackMs_", body, StringComparison.Ordinal);
    }

    /// <summary>
    ///     The vocoder's carrier WAVEFORM is crossfaded, not switched. Saw and
    ///     Pulse differ by 1.7 at phase 0, so picking between them with a bare
    ///     <c>switch</c> splices the carrier — the rule AGENTS.md states for
    ///     <c>ringmod</c> and <c>vibrato</c>, which crossfade a waveform change
    ///     over 10 ms from the same phase.
    ///     <para>
    ///         This one is a SHAPE assertion over the source and says so. The
    ///         carrier is internal, and everything it reaches the output through
    ///         — up to 24 bandpass pairs at Q up to 24, each divided by its own
    ///         slow carrier envelope — rings for far longer than the 10 ms fade,
    ///         so every behavioural grader in this file passes with the hard
    ///         switch in place: measured, the RmsDiff seam proxy and a
    ///         sample-to-sample step comparison BOTH do. Asserting the mechanism
    ///         is honest; asserting an effect that cannot be measured would not
    ///         be.
    ///     </para>
    /// </summary>
    [Fact]
    public void TheVocodersCarrierWaveform_IsCrossfaded_NotSwitched()
    {
        var source = StripComments(
            WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp.Native/src/dsp/cores/dcdsp_vocoder.h"));
        var at = source.IndexOf("void process(", StringComparison.Ordinal);
        Assert.True(at >= 0, "dcdsp_vocoder.h has no process() — repoint this lint");
        var body = Body(source, at);
        Assert.True(body.Length > 200, $"process() body looks empty ({body.Length} chars)");

        // The whole fault in one token: selecting the carrier by branching on
        // the live param is the splice.
        Assert.DoesNotContain("switch (params_.carrier)", body, StringComparison.Ordinal);
        Assert.Contains("carrierFade_", body, StringComparison.Ordinal);
        // Both waveforms are evaluated from the SAME phase and blended.
        Assert.Contains("carrierOf(prevCarrier_", body, StringComparison.Ordinal);
        Assert.Contains("carrierOf(curCarrier_", body, StringComparison.Ordinal);
    }

    /// (module, param, probe Hz) — one rate-owned quantity per module, chosen so
    /// that a value derived once at 48 kHz and never re-derived would move it:
    /// the SVF's and the talkbox's LFO increments, the comb's delay LENGTH in
    /// samples, the vocoder's band coefficients, the tilt's shelf corners.
    [NativeAvailableTheory]
    [InlineData("svf")]
    [InlineData("comb")]
    [InlineData("vocoder")]
    [InlineData("talkbox")]
    [InlineData("tilt")]
    public void PrepareReDerivesEveryRateOwnedQuantity_SoTheAnswerIsTheSameAtEveryRate(string module)
    {
        var at48 = Measure(48000);
        var at192 = Measure(192000);

        Assert.True(MathF.Abs(at48) > 3f,
            $"{module} measured {at48:F2} at 48 kHz — too close to nothing for the case to say "
            + "anything, so it is vacuous");
        Assert.True(MathF.Abs(at48 - at192) < MathF.Max(2f, MathF.Abs(at48) * 0.2f),
            $"{module} measured {at48:F2} at 48 kHz and {at192:F2} at 192 kHz — something "
            + "rate-owned is not re-derived by prepare()");

        float Measure(int rate)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode(module);
                var frames = rate;                 // one second, whatever the rate
                switch (module)
                {
                    case "svf":
                    {
                        // The LFO increment: how many sweep cycles fit in a second.
                        n.SetParam("mode", 0f); n.SetParam("cutoffHz", 1000f);
                        n.SetParam("source", 1f); n.SetParam("lfoRateHz", 4f);
                        n.SetParam("lfoDepth", 1f); n.SetParam("mix", 1f);
                        Render(chain, Saw(frames, 0, 0.4f, 100f, rate), rate);
                        var outp = Render(chain, Saw(frames, frames, 0.4f, 100f, rate), rate);
                        return Cycles(Windows(outp, rate / 100)).cycles;   // 10 ms windows
                    }

                    case "talkbox":
                    {
                        n.SetParam("sweep", 2f); n.SetParam("lfoRateHz", 4f);
                        n.SetParam("resonance", 0.8f); n.SetParam("drive", 0f);
                        n.SetParam("mix", 1f);
                        Render(chain, Saw(frames, 0, 0.4f, 100f, rate), rate);
                        var outp = Render(chain, Saw(frames, frames, 0.4f, 100f, rate), rate);
                        // The energy at "A"'s F1, which has exactly ONE maximum
                        // per sweep cycle. Windowed RMS does not: three formants
                        // crossing a harmonic grid give it several, and the
                        // count then reads 10 at 48 kHz and 7 at 192 kHz for a
                        // sweep that is the same speed in both.
                        return Cycles(WindowsAt(outp, rate / 50, 700f, rate)).cycles;
                    }

                    case "comb":
                    {
                        // The delay LENGTH: 220 Hz must still be a peak and 330 a
                        // trough, which is only true if it follows the rate.
                        n.SetParam("freqHz", 220f); n.SetParam("feedback", 0.9f);
                        n.SetParam("damping", 0f); n.SetParam("mix", 1f);
                        return ProbeDb(chain, 220f, rate) - ProbeDb(chain, 330f, rate);
                    }

                    case "vocoder":
                    {
                        // The band coefficients: the opened band against a closed one.
                        n.SetParam("bands", 16f); n.SetParam("carrier", 1f);
                        n.SetParam("pitchHz", 110f); n.SetParam("emphasis", 0f);
                        n.SetParam("mix", 1f);
                        Render(chain, Sine(frames, 0, 0.5f, 440f, rate), rate);
                        var outp = Render(chain, Sine(frames, frames, 0.5f, 440f, rate), rate);
                        return Db(Goertzel(outp, 440f, rate)) - Db(Goertzel(outp, 2310f, rate));
                    }

                    default:
                    {
                        // The shelf corners: the whole tilt, measured end to end.
                        n.SetParam("tilt", 12f); n.SetParam("pivotHz", 1000f);
                        return ProbeDb(chain, 10000f, rate) - ProbeDb(chain, 100f, rate);
                    }
                }
            }
            finally { chain.Clear(); }
        }
    }

    /// <summary>
    ///     No core in this wave sizes ANY buffer by the block — the comb's line
    ///     is sized by its longest delay and everything else by the channel
    ///     count — so a render must be bit-identical however it is cut up. That
    ///     is the reachable form of "an oversized block is never half-processed":
    ///     a core with block-sized state would diverge the moment the chunking
    ///     changed.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("svf")]
    [InlineData("comb")]
    [InlineData("vocoder")]
    [InlineData("talkbox")]
    [InlineData("tilt")]
    public void ARender_IsIndependentOfHowItIsCutIntoBlocks(string module)
    {
        const int total = 8192;
        var whole = RenderCut(total);
        var pieces = RenderCut(577);      // deliberately not a divisor of anything

        Assert.True(Rms(whole, 0, total) > 0.01f, $"{module} produced silence — the case is vacuous");
        for (var i = 0; i < total; i++)
            Assert.True(MathF.Abs(whole[i] - pieces[i]) < 1e-6f,
                $"{module} sample {i} differs by {MathF.Abs(whole[i] - pieces[i]):E3} between a "
                + "one-shot render and a chopped one — something is sized by the block");

        float[] RenderCut(int chunk)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                chain.AddNode(module);
                var outp = new float[total];
                for (var pos = 0; pos < total; pos += chunk)
                {
                    var n = Math.Min(chunk, total - pos);
                    var piece = Render(chain, Saw(n, pos, 0.4f, 220f));
                    Array.Copy(piece, 0, outp, pos, n);
                }

                return outp;
            }
            finally { chain.Clear(); }
        }
    }

    /// <summary>
    ///     A source lint over the five new cores: nothing inside a
    ///     <c>process(...)</c> body may allocate, resize or take a lock. The RT
    ///     gate catches this at runtime for a module it happens to construct;
    ///     this catches it at the shape level for every one of them, including
    ///     the paths a stimulus never reaches.
    /// </summary>
    [Fact]
    public void NoFilterCore_AllocatesOrLocksInsideProcess()
    {
        string[] cores = { "svf", "comb", "vocoder", "talkbox", "tilt" };
        // `std::fill`, `memset` and `line.reset()` are here for a fault the
        // allocation gate CANNOT see, because it does not allocate: comb's
        // non-finite-tap recovery called InterpolatedDelayLine::reset() from the
        // innermost sample loop, and that is `std::fill` over
        // kMaxDelaySamples + 4 == 9,604 floats — 38 KB of work keyed on audio
        // CONTENT, inside the callback. The recovery is now a deferred flag
        // consumed at the top of the next block. A small `.reset()` on a biquad
        // or a one-pole is four floats and stays allowed; the delay LINE's is
        // the one that is spelled out.
        var banned = new Regex(@"\b(new\s|malloc|calloc|realloc|std::vector\s*<|\.resize\(|\.assign\(|"
                               + @"\.push_back\(|\.emplace|std::lock_guard|std::unique_lock|\.lock\(|"
                               + @"std::string|printf|fopen|std::fill|memset|line\.reset\()",
                               RegexOptions.Compiled);

        var offenders = new List<string>();
        foreach (var core in cores)
        {
            var path = $"src/JuceSharp.Native/src/dsp/cores/dcdsp_{core}.h";
            var source = StripComments(WrapperLifetimeAuditTests.ReadRepoFile(path));
            var at = source.IndexOf("void process(", StringComparison.Ordinal);
            Assert.True(at >= 0, $"dcdsp_{core}.h has no process() — repoint this lint");
            var body = Body(source, at);
            Assert.True(body.Length > 200, $"dcdsp_{core}.h: process() body looks empty ({body.Length} chars)");
            foreach (Match m in banned.Matches(body))
                offenders.Add($"{core}: '{m.Value.Trim()}'");
        }

        Assert.True(offenders.Count == 0,
            "these filter cores do something forbidden on the audio thread: "
            + string.Join(", ", offenders));
    }

    // =======================================================================
    // Helpers
    // =======================================================================

    /// Steady-state gain of the whole chain at one frequency, in dB. Two renders:
    /// the first settles the node's add crossfade and every smoother, the second
    /// is measured.
    private float ProbeDb(JuceDspChain chain, float hz, int rate = SampleRate)
    {
        var frames = rate;
        const float amp = 0.25f;
        Render(chain, Sine(frames, 0, amp, hz, rate), rate);
        var outp = Render(chain, Sine(frames, frames, amp, hz, rate), rate);
        return Db(Goertzel(outp, hz, rate)) - Db(amp);
    }

    private float[] RenderSeam(string module, string setup, string param, float first, float second,
                               string stimulus)
    {
        float[] Stim(int offset, int frames) => stimulus switch
        {
            "saw" => Saw(frames, offset, 0.4f, 110f),
            "tone220" => Sine(frames, offset, 0.3f, 220f),
            "probe1k" => Sine(frames, offset, 0.3f, 1000f),
            _ => Sine(frames, offset, 0.25f, 110f),
        };

        var chain = Engine.GetDspChain();
        try
        {
            chain.Clear();
            var n = chain.AddNode(module);
            foreach (var pair in setup.Split(';', StringSplitOptions.RemoveEmptyEntries))
            {
                var kv = pair.Split('=');
                n.SetParam(kv[0], float.Parse(kv[1], CultureInfo.InvariantCulture));
            }

            n.SetParam(param, first);

            // Settle followers, delay lines, both vocoder banks and the chain's
            // own add crossfade.
            const int preRoll = 8;
            for (var b = 0; b < preRoll; b++) Render(chain, Stim(b * Block, Block));
            Render(chain, Stim(preRoll * Block, Block));
            n.SetParam(param, second);
            return Render(chain, Stim((preRoll + 1) * Block, SeamFrames));
        }
        finally { chain.Clear(); }
    }

    private static float[] Render(JuceDspChain chain, float[] mono, int sampleRate = SampleRate)
    {
        var left = (float[])mono.Clone();
        var right = (float[])mono.Clone();
        chain.ProcessOffline(mono, mono, sampleRate, left, right);
        return left;
    }

    /// A sine of the ABSOLUTE sample index, so two renders of the same case are
    /// sample-aligned and can be compared directly.
    private static float[] Sine(int frames, int offset, float amp, float hz, int rate = SampleRate)
    {
        var buf = new float[frames];
        for (var i = 0; i < frames; i++)
            buf[i] = amp * MathF.Sin(2f * MathF.PI * hz * (offset + i) / rate);
        return buf;
    }

    /// A band-limited sawtooth (30 harmonics, 1/n): a harmonic every `hz` for a
    /// formant to land on, and no aliasing to confuse a peak finder.
    private static float[] Saw(int frames, int offset, float amp, float hz, int rate = SampleRate)
    {
        var buf = new float[frames];
        var harmonics = Math.Min(30, (int)(0.45f * rate / hz));
        for (var i = 0; i < frames; i++)
        {
            var t = (offset + i) / (float)rate;
            var acc = 0f;
            for (var h = 1; h <= harmonics; h++) acc += MathF.Sin(2f * MathF.PI * hz * h * t) / h;
            buf[i] = amp * acc;
        }

        return buf;
    }

    /// Deterministic white noise (xorshift32), seeded per call so two renders of
    /// the same seed are identical.
    private static float[] Noise(int frames, uint seed)
    {
        var buf = new float[frames];
        var s = seed | 1u;
        for (var i = 0; i < frames; i++)
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            buf[i] = ((s >> 8) / (float)(1 << 24)) * 0.5f - 0.25f;
        }

        return buf;
    }

    private static float[] Windows(float[] x, int win)
    {
        var trail = new float[x.Length / Math.Max(win, 1)];
        for (var w = 0; w < trail.Length; w++) trail[w] = Rms(x, w * win, win);
        return trail;
    }

    /// The same trajectory, but at ONE frequency. A windowed RMS has several
    /// maxima per sweep of a three-formant filter; the energy at a single
    /// formant centre has exactly one.
    private static float[] WindowsAt(float[] x, int win, float hz, int rate = SampleRate)
    {
        win = Math.Max(win, 1);
        var trail = new float[x.Length / win];
        var scratch = new float[win];
        for (var w = 0; w < trail.Length; w++)
        {
            Array.Copy(x, w * win, scratch, 0, win);
            trail[w] = Goertzel(scratch, hz, rate);
        }

        return trail;
    }

    /// How many times a trajectory rises across its own range (= cycles), and
    /// how far it moved end to end in dB. Both read off the samples, so neither
    /// depends on this test agreeing with the core's arithmetic.
    /// <para>
    ///     The counter is a SCHMITT TRIGGER at 30 % / 70 % of the range, not a
    ///     mean crossing. A swept filter's level does not rise smoothly: the
    ///     harmonics of the stimulus cross the corner one at a time, so the
    ///     trajectory climbs as a staircase and a bare mean crossing counts a
    ///     4 Hz sweep as EIGHT cycles — measured.
    /// </para>
    private static (int cycles, float rangeDb) Cycles(float[] trail)
    {
        if (trail.Length < 4) return (0, 0f);
        var db = Array.ConvertAll(trail, Db);
        var lo = db.Min();
        var hi = db.Max();
        var range = hi - lo;
        if (range <= 0.01f) return (0, range);

        var low = lo + range * 0.3f;
        var high = lo + range * 0.7f;
        var cycles = 0;
        var armed = db[0] < high;
        foreach (var v in db)
        {
            if (armed && v > high) { cycles++; armed = false; }
            else if (!armed && v < low) armed = true;
        }

        return (cycles, range);
    }

    private static float MaxStep(float[] x)
    {
        var max = 0f;
        for (var i = 1; i < x.Length; i++) max = MathF.Max(max, MathF.Abs(x[i] - x[i - 1]));
        return max;
    }

    private static float Rms(float[] x, int start, int count)
    {
        double acc = 0;
        for (var i = start; i < start + count; i++) acc += (double)x[i] * x[i];
        return count <= 0 ? 0f : (float)Math.Sqrt(acc / count);
    }

    private static float RmsDiff(float[] a, float[] b, int start, int count)
    {
        double acc = 0;
        for (var i = start; i < start + count; i++)
        {
            double d = a[i] - b[i];
            acc += d * d;
        }

        return count <= 0 ? 0f : (float)Math.Sqrt(acc / count);
    }

    private static float Db(float linear) => 20f * MathF.Log10(MathF.Max(linear, 1e-12f));

    /// Magnitude at one frequency (Goertzel). Exact when the window holds a
    /// whole number of periods, which every call site here arranges.
    private static float Goertzel(float[] x, float hz, int rate = SampleRate)
    {
        var w = 2.0 * Math.PI * hz / rate;
        var coeff = 2.0 * Math.Cos(w);
        double s1 = 0, s2 = 0;
        foreach (var v in x)
        {
            var s = v + coeff * s1 - s2;
            s2 = s1;
            s1 = s;
        }

        var re = s1 - s2 * Math.Cos(w);
        var im = s2 * Math.Sin(w);
        return (float)(2.0 * Math.Sqrt(re * re + im * im) / x.Length);
    }

    private static string StripComments(string source)
    {
        var noBlocks = Regex.Replace(source, @"/\*.*?\*/", string.Empty, RegexOptions.Singleline);
        return Regex.Replace(noBlocks, "//[^\r\n]*", string.Empty);
    }

    private static string Body(string source, int at)
    {
        var open = source.IndexOf('{', at);
        var depth = 0;
        for (var i = open; i < source.Length; i++)
        {
            if (source[i] == '{') depth++;
            else if (source[i] == '}' && --depth == 0) return source[open..(i + 1)];
        }

        throw new InvalidOperationException("Unbalanced braces.");
    }
}
