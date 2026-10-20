using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using JuceSharp;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     The six modules of the 2026-09 <c>dynamics</c> gear wave — <c>limiter</c>,
///     <c>transient</c>, <c>multiband</c>, <c>expander</c>, <c>optocomp</c> and
///     <c>ducker</c> (docs/guides/gear-dynamics.md).
///     <para>
///         Everything here renders through the OFFLINE rig export on a stopped
///         engine, exactly as <see cref="DspGainZipperTests" /> and
///         <see cref="DspSidechainTests" /> do. Each behavioural fact asserts the
///         claim the module's doc row makes, and carries its own vacuity guard —
///         a stimulus that cannot move, or two settings that sound the same,
///         fails the case before the claim is even reached.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class GearDynamicsTests : IDisposable
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

    /// Every module this wave added, with its full param list. The catalogue is
    /// the source of truth; this is the list of ids the tests sweep.
    public static readonly string[] Modules =
        { "limiter", "transient", "multiband", "expander", "optocomp", "ducker" };

    private readonly JuceAudioEngine? _engine;

    public GearDynamicsTests()
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
    public void EveryDynamicsModule_IsInTheCatalogue_InItsOwnDrawer()
    {
        foreach (var id in Modules)
        {
            var info = JuceDspModuleCatalog.Find(id);
            Assert.True(info is not null, $"'{id}' is not in the catalogue");
            Assert.Equal("dynamics", info!.Category);
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
    ///     The one param in this wave the DSP runs as an INT
    ///     (<c>optocomp.limit</c>) declares itself NAMED, and no other param
    ///     here pretends to be discrete. A param the adapter <c>lround</c>s and
    ///     that does not say so parks a UI knob at "0.5 modes".
    /// </summary>
    [NativeAvailableFact]
    public void TheOnlyDiscreteParam_IsOptocompLimit_AndItIsNamed()
    {
        var discrete = (from id in Modules
                        from p in JuceDspModuleCatalog.Find(id)!.Params
                        where p.IsDiscrete
                        select $"{id}.{p.Id}").ToArray();
        Assert.Equal(new[] { "optocomp.limit" }, discrete);

        var limit = JuceDspModuleCatalog.Find("optocomp")!.Params.Single(p => p.Id == "limit");
        Assert.True(limit.IsNamed);
        Assert.Equal(new[] { "Compress", "Limit" }, limit.Labels!.ToArray());
        Assert.Equal("Compress", limit.Describe(0f));
        Assert.Equal("Limit", limit.Describe(1f));
    }

    /// <summary>
    ///     A sidechain flag is a SECOND source of truth for a capability, so the
    ///     set is pinned exactly: the two keyed modules advertise it, the four
    ///     that have no external detector do not. A source a module ignored
    ///     would read as a live control that does nothing.
    /// </summary>
    [NativeAvailableFact]
    public void ExactlyTheKeyedModules_AdvertiseASidechain()
    {
        var keyed = Modules.Where(id => JuceDspModuleCatalog.Find(id)!.SupportsSidechain).ToArray();
        Assert.Equal(new[] { "expander", "ducker" }, keyed);

        // …and the engine agrees with the advertisement, in both directions.
        var chain = Engine.GetDspChain();
        try
        {
            foreach (var id in Modules)
            {
                var node = chain.AddNode(id);
                if (JuceDspModuleCatalog.Find(id)!.SupportsSidechain)
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
                    // Three points inside the declared range, plus the default.
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
    // limiter
    // =======================================================================

    /// <summary>
    ///     The ceiling is a GUARANTEE: a square 12 dB over it never crosses it,
    ///     measured after the look-ahead has had time to fill.
    /// </summary>
    [NativeAvailableFact]
    public void Limiter_HoldsTheCeiling_AgainstASquareTwelveDbOverIt()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("limiter");
            n.SetParam("ceilingDb", -1f);
            n.SetParam("gainDb", 12f);
            n.SetParam("lookaheadMs", 1f);

            Render(chain, Square(0));                       // settle the fade + gain ramp
            var outp = Render(chain, Square(Block));

            var ceiling = MathF.Pow(10f, -1f / 20f);
            var allowed = ceiling * MathF.Pow(10f, 0.1f / 20f);   // +0.1 dB of slack
            var peak = 0f;
            for (var i = 96; i < Block; i++) peak = MathF.Max(peak, MathF.Abs(outp[i]));

            // Vacuity: the stimulus really is over the ceiling, so the pedal had
            // work to do — a silent input passes this assertion trivially.
            Assert.True(peak > ceiling * 0.5f,
                $"the limited square came out at {Db(peak):F2} dBFS — the stimulus never reached the pedal");
            Assert.True(peak <= allowed,
                $"a +12 dB square reached {Db(peak):F2} dBFS against a -1.00 dBFS ceiling "
                + $"(allowed {Db(allowed):F2})");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     …and under the ceiling it is a WIRE. The one-pole gain never reaches
    ///     unity on its own, so without the snap in
    ///     <c>dcdsp_limiter.h</c> this sits a hundredth of a dB low for ever.
    /// </summary>
    [NativeAvailableFact]
    public void Limiter_PassesASineUnderTheCeiling_WithinAHundredthOfADb()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("limiter");
            n.SetParam("ceilingDb", 0f);
            n.SetParam("gainDb", 0f);

            // 1 kHz at 48 kHz is exactly 48 samples a period, and the default
            // 1 ms look-ahead is exactly 48 samples — so the delay shifts the
            // wave by a WHOLE period and the window below holds a whole number
            // of them. With 220 Hz and an arbitrary window this probe reported
            // 0.138 dB of "error" that was nothing but a partial period.
            var input = Sine(Block, 0, 0.2f, 1000f);
            Render(chain, Sine(Block, 0, 0.2f, 1000f));
            var outp = Render(chain, input);

            const int start = 256;
            const int count = 36 * 48;   // 36 whole periods
            var inRms = Rms(input, start, count);
            var outRms = Rms(outp, start, count);
            Assert.True(inRms > 0.05f, "the stimulus is silent — the case is vacuous");
            var deltaDb = MathF.Abs(Db(outRms) - Db(inRms));
            Assert.True(deltaDb < 0.01f,
                $"a sine under the ceiling came out {deltaDb:F4} dB away from its input");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     The look-ahead is what makes this a limiter and not a clipper: the
    ///     gain computer is a SLIDING MINIMUM over the window, so the gain is
    ///     already down when the peak arrives and the backstop clamp has almost
    ///     nothing left to do. Measured as the fraction of output samples
    ///     sitting ON the ceiling while a level-stepping stimulus drives it
    ///     hard — the same figure src/MasterLimiter.h reports for the master bus
    ///     (0.78 % with the window minimum, 13.28 % with the instantaneous
    ///     target).
    /// </summary>
    [NativeAvailableFact]
    public void Limiter_HoldsTheCeilingByGain_NotByClipping()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("limiter");
            n.SetParam("ceilingDb", -1f);
            n.SetParam("lookaheadMs", 2f);
            n.SetParam("releaseMs", 50f);

            for (var b = 0; b < 3; b++) Render(chain, Stepped(b * Block));
            var outp = Render(chain, Stepped(3 * Block));

            var ceiling = MathF.Pow(10f, -1f / 20f);
            var pinned = 0;
            var peak = 0f;
            for (var i = 0; i < Block; i++)
            {
                var a = MathF.Abs(outp[i]);
                if (a > peak) peak = a;
                if (a >= ceiling * (1f - 1e-5f)) pinned++;
            }

            // Vacuity, both ways: the pedal really is limiting (the input peaks
            // at 1.2, the output may not pass the ceiling)…
            var inPeak = Stepped(3 * Block).Max(MathF.Abs);
            Assert.True(inPeak > ceiling * 1.3f, "the stimulus never reaches the ceiling");
            Assert.True(peak <= ceiling * MathF.Pow(10f, 0.1f / 20f),
                $"the output reached {Db(peak):F2} dBFS against a -1.00 dBFS ceiling");
            Assert.True(peak > ceiling * 0.9f,
                "the output never came near the ceiling — the limiter is not working at all");

            // …and it got there by GAIN, not by clipping into the backstop.
            var fraction = pinned / (float)Block;
            Assert.True(fraction < 0.05f,
                $"{fraction:P2} of output samples sit exactly on the ceiling — the gain computer "
                + "is reacting to the peak instead of seeing it coming, so the backstop clamp is "
                + "doing the limiting");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     The look-ahead IS latency and the chain's PDC budgets for what
    ///     <c>latencySamples()</c> reports — so the figure has to follow both the
    ///     knob and the RATE. An adapter that answered 0, or that answered in
    ///     48 kHz samples at 192 kHz, makes a split rig comb.
    /// </summary>
    [NativeAvailableFact]
    public void Limiter_ReportsItsLookaheadAsLatency_AtEveryRate()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("limiter");
            n.SetParam("lookaheadMs", 2f);
            Render(chain, Sine(512, 0, 0.2f, 220f));
            Assert.Equal(96, n.LatencySamples);                // 2 ms at 48 kHz
            Assert.Equal(96, chain.LatencySamples);

            n.SetParam("lookaheadMs", 0f);
            Assert.Equal(1, n.LatencySamples);                 // the tap floors at one sample

            n.SetParam("lookaheadMs", 2f);
            Render(chain, Sine(512, 0, 0.2f, 220f), 192000);   // re-prepares at the new rate
            Assert.Equal(384, n.LatencySamples);               // 2 ms at 192 kHz
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     Moving <c>lookaheadMs</c> moves a read TAP, and the core crossfades
    ///     between the old and the new one over 10 ms rather than teleporting.
    ///     Nothing exercised that until 2026-09-12: no <see cref="RampCases" />
    ///     row touched the param, the latency test above only reads a reported
    ///     number back, and the RT scenario wrote it BEFORE arming — so the
    ///     whole mechanism could be deleted and the suite stayed green, which is
    ///     the definition of a guard nobody should trust.
    ///     <para>
    ///         Graded the way a tap must be (the shape
    ///         <c>MicPos_Distance_MovesItsDelayTapWithoutAClick</c> uses): the
    ///         largest sample-to-sample step against the same render with the
    ///         knob left alone. <c>ceilingDb=0</c> with a 0.25 sine keeps the
    ///         gain computer at exactly unity, so the tap is the only thing in
    ///         play. A teleport from 1 sample to 240 splices ~pi radians of a
    ///         110 Hz sine in.
    ///     </para>
    ///     <para>
    ///         It reads ACROSS the render boundary, and that is the whole test:
    ///         a param write lands at sample 0 of the NEXT block, so a step
    ///         there sits between the last sample of one render and the first
    ///         of the next — where a scan of the seam block alone never looks.
    ///         Graded on the seam block only, this passed with the crossfade
    ///         DELETED (measured). It is lesson (3) of the 2026-09 gear wave,
    ///         <c>docs/agents/dsp-tone-rig.md</c>, and the reason this helper
    ///         exists instead of <c>RenderSeam</c>.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void Limiter_MovingTheLookaheadTap_DoesNotSpliceTheWaveform()
    {
        var steady = MaxStep(RenderAcrossTheSeam(0f));
        var moved = MaxStep(RenderAcrossTheSeam(5f));

        Assert.True(steady > 0, "the stimulus is silent — the case is vacuous");
        Assert.True(moved < 3f * steady,
            $"limiter's look-ahead tap stepped {moved:E3} against a resting {steady:E3} "
            + $"(x{moved / steady:F2}) — the tap jumped instead of crossfading");

        // The block BEFORE the write and the block after it, joined, so the
        // sample pair the write lands between is in the scan.
        float[] RenderAcrossTheSeam(float after)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("limiter");
                n.SetParam("ceilingDb", 0f);
                n.SetParam("gainDb", 0f);
                n.SetParam("releaseMs", 200f);
                n.SetParam("lookaheadMs", 0f);

                float[] Stim(int b) => Sine(Block, b * Block, 0.25f, 110f);
                const int preRoll = 6;
                for (var b = 0; b < preRoll; b++) Render(chain, Stim(b));
                var before = Render(chain, Stim(preRoll));
                n.SetParam("lookaheadMs", after);
                var seam = Render(chain, Stim(preRoll + 1));

                var joined = new float[before.Length + seam.Length];
                Array.Copy(before, joined, before.Length);
                Array.Copy(seam, 0, joined, before.Length, seam.Length);
                return joined;
            }
            finally { chain.Clear(); }
        }

        static float MaxStep(float[] y)
        {
            var m = 0f;
            for (var i = 1; i < y.Length; i++) m = MathF.Max(m, MathF.Abs(y[i] - y[i - 1]));
            return m;
        }
    }

    // =======================================================================
    // transient
    // =======================================================================

    /// <summary>
    ///     <c>attackDb</c> moves the first few ms of a pluck and leaves its tail
    ///     alone; <c>sustainDb</c> does the mirror image. The second half of each
    ///     assertion is the one that matters — a shaper that gained the whole
    ///     note would pass the first and is just a volume knob.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("attackDb")]
    [InlineData("sustainDb")]
    public void Transient_MovesItsOwnPhase_AndLeavesTheOtherAlone(string param)
    {
        var flat = RenderPluck(param, 0f);
        var lifted = RenderPluck(param, 6f);

        const int attackLen = 5 * SampleRate / 1000;    // the first 5 ms
        const int tailStart = 150 * SampleRate / 1000;  // 150..250 ms
        const int tailLen = 100 * SampleRate / 1000;

        var attackDelta = Db(Rms(lifted, 0, attackLen)) - Db(Rms(flat, 0, attackLen));
        var tailDelta = Db(Rms(lifted, tailStart, tailLen)) - Db(Rms(flat, tailStart, tailLen));

        Assert.True(Rms(flat, tailStart, tailLen) > 1e-4f,
            "the pluck has decayed into nothing by the tail window — the case is vacuous");

        if (param == "attackDb")
        {
            Assert.True(attackDelta > 3f,
                $"attackDb +6 moved the first 5 ms by {attackDelta:F2} dB");
            Assert.True(MathF.Abs(tailDelta) < 0.5f,
                $"attackDb +6 moved the 200 ms TAIL by {tailDelta:F2} dB — it is a volume knob, "
                + "not a transient shaper");
        }
        else
        {
            Assert.True(tailDelta > 3f,
                $"sustainDb +6 moved the tail by only {tailDelta:F2} dB");
            Assert.True(MathF.Abs(attackDelta) < 0.5f,
                $"sustainDb +6 moved the ATTACK by {attackDelta:F2} dB — the two phases are not "
                + "separated");
        }

        float[] RenderPluck(string id, float value)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("transient");
                n.SetParam(id, value);
                Render(chain, new float[Block]);          // silence: settle the add crossfade
                var frames = 300 * SampleRate / 1000;
                return Render(chain, Pluck(frames));
            }
            finally { chain.Clear(); }
        }
    }

    // =======================================================================
    // multiband
    // =======================================================================

    /// <summary>
    ///     At ratio 1 in every band the three Linkwitz-Riley legs must sum back
    ///     to FLAT. This is the assertion that catches the classic three-band
    ///     tree bug — a low band that skipped the second crossover's allpass
    ///     dips several dB around the first split.
    /// </summary>
    [NativeAvailableFact]
    public void Multiband_AtRatioOne_SumsBackToFlat_AcrossTheWholeBand()
    {
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("multiband");
            foreach (var p in new[] { "lowRatio", "midRatio", "highRatio" }) n.SetParam(p, 1f);

            var worst = 0f;
            var worstHz = 0f;
            var worstSplit = "";
            // TWO crossover settings, and the SECOND one is the discriminating
            // half. At 150/2500 the second crossover's allpass is flat-phase
            // everywhere the first one is doing anything, so a low band that
            // skipped the compensator still measures flat — the defect only
            // appears once the two splits are close enough to interact.
            foreach (var (lowHz, highHz) in new[] { (150f, 2500f), (500f, 1000f) })
            {
                n.SetParam("lowXoverHz", lowHz);
                n.SetParam("highXoverHz", highHz);
                foreach (var hz in new[] { 40f, 100f, 150f, 300f, 500f, 700f, 1000f, 2500f, 5000f, 16000f })
                {
                    var frames = 4 * Block;
                    var input = Sine(frames, 0, 0.3f, hz);
                    Render(chain, input);                 // settle the filters at this frequency
                    var outp = Render(chain, input);
                    var delta = MathF.Abs(Db(Rms(outp, Block, frames - Block))
                                          - Db(Rms(input, Block, frames - Block)));
                    if (delta > worst)
                    {
                        worst = delta; worstHz = hz;
                        worstSplit = $"{lowHz:F0}/{highHz:F0}";
                    }
                }
            }

            Assert.True(worst < 0.1f,
                $"the band split is {worst:F3} dB off flat at {worstHz:F0} Hz with the {worstSplit} Hz "
                + "crossovers — the three legs do not sum to an allpass");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     …and the bands are really independent: compressing the LOW band on a
    ///     two-tone stimulus must leave the high tone exactly where it was.
    /// </summary>
    [NativeAvailableFact]
    public void Multiband_CompressesOneBand_WithoutTouchingAnother()
    {
        var flat = RenderTwoTone(1f);
        var squashed = RenderTwoTone(10f);

        var lowDelta = Db(Goertzel(squashed, 80f)) - Db(Goertzel(flat, 80f));
        var highDelta = Db(Goertzel(squashed, 6000f)) - Db(Goertzel(flat, 6000f));

        Assert.True(Goertzel(flat, 80f) > 1e-3f && Goertzel(flat, 6000f) > 1e-4f,
            "one of the two tones is missing from the reference render — the case is vacuous");
        Assert.True(lowDelta < -6f,
            $"10:1 on the low band moved the 80 Hz tone by only {lowDelta:F2} dB");
        Assert.True(MathF.Abs(highDelta) < 0.5f,
            $"compressing the LOW band moved the 6 kHz tone by {highDelta:F2} dB — the bands are "
            + "not independent");

        float[] RenderTwoTone(float lowRatio)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("multiband");
                n.SetParam("lowRatio", lowRatio);
                n.SetParam("lowThresholdDb", -30f);
                n.SetParam("midRatio", 1f);
                n.SetParam("highRatio", 1f);
                n.SetParam("attackMs", 5f);
                n.SetParam("releaseMs", 50f);
                for (var b = 0; b < 3; b++) Render(chain, TwoTone(b * Block));
                return Render(chain, TwoTone(3 * Block));
            }
            finally { chain.Clear(); }
        }
    }

    // =======================================================================
    // expander
    // =======================================================================

    /// <summary>
    ///     The expander's whole contract in one table: (level − threshold) ×
    ///     (ratio − 1) dB of attenuation below the threshold, clamped at
    ///     <c>rangeDb</c>, and NOTHING above it.
    /// </summary>
    [NativeAvailableTheory]
    // levelDb, ratio, rangeDb, expected attenuation in dB
    [InlineData(-50f, 2f, 30f, 10f)]
    [InlineData(-50f, 3f, 30f, 20f)]
    [InlineData(-50f, 3f, 4f, 4f)]     // clamped by the range, not by the ratio
    [InlineData(-20f, 3f, 30f, 0f)]    // above the threshold: a wire
    public void Expander_AttenuatesByTheCurve_AndStopsAtTheRange(
        float levelDb, float ratio, float rangeDb, float expectedDb)
    {
        var amplitude = MathF.Pow(10f, levelDb / 20f);
        // The reference moves the THRESHOLD out of the way, it does not set the
        // ratio to 1. A slope written as `ratio` instead of `ratio - 1` breaks
        // both renders by the same amount, so a ratio-1 reference cancels the
        // error out and the case passes with the curve wrong — measured.
        var open = RenderTone(amplitude, ratio, rangeDb, -80f);
        var expanded = RenderTone(amplitude, ratio, rangeDb, -40f);

        Assert.True(Rms(open, 0, Block) > 0f, "the reference render is silent — the case is vacuous");
        var measured = Db(Rms(open, 0, Block)) - Db(Rms(expanded, 0, Block));
        Assert.True(MathF.Abs(measured - expectedDb) < 1.0f,
            $"a {levelDb:F0} dB tone at {ratio:F0}:1 with a {rangeDb:F0} dB range lost "
            + $"{measured:F2} dB, expected {expectedDb:F2}");

        float[] RenderTone(float amp, float r, float range, float threshold)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("expander");
                n.SetParam("thresholdDb", threshold);
                n.SetParam("ratio", r);
                n.SetParam("rangeDb", range);
                n.SetParam("attackMs", 1f);
                n.SetParam("releaseMs", 20f);
                for (var b = 0; b < 4; b++) Render(chain, Sine(Block, b * Block, amp, 1000f));
                return Render(chain, Sine(Block, 4 * Block, amp, 1000f));
            }
            finally { chain.Clear(); }
        }
    }

    /// <summary>
    ///     Keyed, an expander stops following its own input — which is the whole
    ///     reason it carries the flag. A pad in front of it puts its own input
    ///     far below the threshold; keyed from the dry input it is far above it.
    /// </summary>
    [NativeAvailableFact]
    public void Expander_KeyedFromTheDryInput_StopsExpandingWhatItsOwnInputCannotOpen()
    {
        // The level a -40 dB pad alone leaves behind: the "expander fully open"
        // reference, so each half of the claim below is measured against what
        // the audio path actually carries rather than against arithmetic.
        var open = Db(Level(null));
        var unkeyed = Db(Level(JuceDspSidechainSource.None));
        var keyed = Db(Level(JuceDspSidechainSource.SeatInput));

        Assert.True(open > -80f, $"the padded reference is silent ({open:F2} dBFS) — the case is vacuous");
        Assert.True(open - unkeyed > 12f,
            $"unkeyed, the expander only pulled the padded signal down by {open - unkeyed:F2} dB "
            + $"(open {open:F2} dBFS, unkeyed {unkeyed:F2} dBFS) — there is nothing for the key to undo");
        Assert.True(MathF.Abs(open - keyed) < 1.5f,
            $"keyed off the dry input the expander should be a wire, but it sits {open - keyed:F2} dB "
            + $"under the open reference (open {open:F2} dBFS, keyed {keyed:F2} dBFS) — the key is "
            + "not reaching the detector");

        // `null` = the pad alone, no expander at all.
        float Level(JuceDspSidechainSource? source)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var pad = chain.AddNode("gain");
                pad.SetParam("gainDb", -40f);
                if (source is { } src)
                {
                    var exp = chain.AddNode("expander");
                    // -20, not -30. The `gain` node's range STOPS AT -24 dB, so
                    // a "-40 dB pad" is really -24 and the padded tone peaks at
                    // exactly -30.0 dBFS — right on a -30 threshold, where the
                    // expander does nothing and the whole case reads as 1.3 dB.
                    exp.SetParam("thresholdDb", -20f);
                    exp.SetParam("ratio", 4f);
                    exp.SetParam("rangeDb", 24f);
                    exp.SetParam("releaseMs", 20f);
                    exp.Sidechain = src;
                }

                for (var b = 0; b < 6; b++) Render(chain, Sine(Block, b * Block, 0.5f, 1000f));
                return Rms(Render(chain, Sine(Block, 6 * Block, 0.5f, 1000f)), 0, Block);
            }
            finally { chain.Clear(); }
        }
    }

    // =======================================================================
    // optocomp
    // =======================================================================

    /// <summary>
    ///     The opto cell REMEMBERS. Recovery after a long hold is slower than
    ///     after a short one, and monotonically so — that program dependence is
    ///     the entire character of the topology, and a single-time-constant
    ///     release would make all three of these equal.
    /// </summary>
    [NativeAvailableFact]
    public void Optocomp_RecoversMoreSlowly_TheLongerItWasHeld()
    {
        var t50 = RecoveryMs(50);
        var t500 = RecoveryMs(500);
        var t2000 = RecoveryMs(2000);

        Assert.True(t50 > 0 && t2000 > 0,
            $"the cell never recovered at all (50 ms hold -> {t50} ms, 2 s hold -> {t2000} ms)");
        Assert.True(t500 >= t50 && t2000 >= t500,
            $"recovery is not monotone in hold length: {t50} / {t500} / {t2000} ms");
        Assert.True(t2000 > t50 * 1.5f,
            $"a 2 s hold recovered in {t2000} ms against {t50} ms for a 50 ms hold — the release "
            + "is not program-dependent");

        // How long after the loud passage stops the gain is back within 1 dB of
        // unity, measured on a quiet probe tone that cannot itself compress.
        int RecoveryMs(int holdMs)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("optocomp");
                n.SetParam("peakReduction", 0.9f);
                n.SetParam("emphasis", 0f);

                Render(chain, new float[Block]);                       // settle the add crossfade
                var holdFrames = holdMs * SampleRate / 1000;
                for (var pos = 0; pos < holdFrames; pos += Block)
                    Render(chain, Sine(Block, pos, 0.6f, 1000f));      // the loud passage

                const float probe = 0.002f;                            // -54 dBFS: never compresses
                const int tailMs = 6000;
                var window = 20 * SampleRate / 1000;
                for (var ms = 0; ms < tailMs; ms += 20)
                {
                    var outp = Render(chain, Sine(window, ms * SampleRate / 1000, probe, 1000f));
                    var reduction = Db(Rms(Sine(window, 0, probe, 1000f), 0, window))
                                    - Db(Rms(outp, 0, window));
                    if (reduction < 1f) return ms;
                }

                return -1;
            }
            finally { chain.Clear(); }
        }
    }

    /// <summary>
    ///     …and the knob goes the way it says: more peak reduction, more gain
    ///     reduction, monotonically. Both modes, because <c>limit</c> only
    ///     changes the ratio and a mode that inverted the curve would still
    ///     compress.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(0f)]
    [InlineData(1f)]
    public void Optocomp_GainReduction_IsMonotoneInPeakReduction(float limit)
    {
        var levels = new[] { 0f, 0.3f, 0.6f, 0.9f }.Select(pr => Db(Level(pr))).ToArray();
        for (var i = 1; i < levels.Length; i++)
            Assert.True(levels[i] <= levels[i - 1] + 0.05f,
                $"peak reduction is not monotone in mode {limit}: {string.Join(" / ", levels.Select(l => l.ToString("F2", CultureInfo.InvariantCulture)))} dB");
        Assert.True(levels[0] - levels[^1] > 6f,
            $"the whole peakReduction sweep only moved the level by {levels[0] - levels[^1]:F2} dB "
            + "in mode " + limit + " — the case is vacuous");

        float Level(float peakReduction)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode("optocomp");
                n.SetParam("peakReduction", peakReduction);
                n.SetParam("limit", limit);
                n.SetParam("emphasis", 0f);
                for (var b = 0; b < 6; b++) Render(chain, Sine(Block, b * Block, 0.5f, 1000f));
                return Rms(Render(chain, Sine(Block, 6 * Block, 0.5f, 1000f)), 0, Block);
            }
            finally { chain.Clear(); }
        }
    }

    // =======================================================================
    // ducker
    // =======================================================================

    /// <summary>
    ///     Through the ABI's own sidechain path: a loud KEY pulls a quiet
    ///     through-signal down by exactly <c>duckDb</c>. The pad in front makes
    ///     the two renders each other's A/B on one property — unkeyed the
    ///     ducker's own input is far under the threshold and it is a wire.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(6f)]
    [InlineData(12f)]
    [InlineData(24f)]
    public void Ducker_KeyedFromTheDryInput_DucksByExactlyItsDepth(float duckDb)
    {
        var chain = Engine.GetDspChain();
        try
        {
            var pad = chain.AddNode("gain");
            pad.SetParam("gainDb", -40f);
            var duck = chain.AddNode("ducker");
            // -20, not -30: the `gain` node clamps at -24 dB, so the padded tone
            // peaks at exactly -30.0 dBFS and a -30 threshold sits ON it.
            duck.SetParam("thresholdDb", -20f);
            duck.SetParam("duckDb", duckDb);
            duck.SetParam("attackMs", 5f);
            duck.SetParam("holdMs", 100f);

            for (var b = 0; b < 4; b++) Render(chain, Sine(Block, b * Block, 0.5f, 1000f));
            var unkeyed = Rms(Render(chain, Sine(Block, 4 * Block, 0.5f, 1000f)), 0, Block);

            duck.Sidechain = JuceDspSidechainSource.SeatInput;
            for (var b = 0; b < 4; b++) Render(chain, Sine(Block, b * Block, 0.5f, 1000f));
            var keyed = Rms(Render(chain, Sine(Block, 4 * Block, 0.5f, 1000f)), 0, Block);

            Assert.True(unkeyed > 0f, "the padded signal is silent — the case is vacuous");
            var ducked = Db(unkeyed) - Db(keyed);
            Assert.True(MathF.Abs(ducked - duckDb) < 0.5f,
                $"a loud key ducked the tone by {ducked:F2} dB, asked for {duckDb:F2}");
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     …and it comes back. Self-keyed (the engine's documented fallback when
    ///     no external source is available), the ducker must be within 1 dB of
    ///     unity well inside three release times of the key going quiet.
    /// </summary>
    [NativeAvailableFact]
    public void Ducker_RecoversWithinThreeReleaseTimes_OfTheKeyGoingQuiet()
    {
        const float releaseMs = 200f;
        var chain = Engine.GetDspChain();
        try
        {
            var n = chain.AddNode("ducker");
            n.SetParam("thresholdDb", -30f);
            n.SetParam("duckDb", 6f);
            n.SetParam("attackMs", 5f);
            n.SetParam("holdMs", 0f);
            n.SetParam("releaseMs", releaseMs);

            // Loud: self-keyed, it ducks itself.
            for (var b = 0; b < 6; b++) Render(chain, Sine(Block, b * Block, 0.5f, 1000f));
            var loud = Render(chain, Sine(Block, 6 * Block, 0.5f, 1000f));
            var ducked = Db(Rms(Sine(Block, 0, 0.5f, 1000f), 0, Block)) - Db(Rms(loud, 0, Block));
            Assert.True(ducked > 5f,
                $"the ducker only pulled its own input down by {ducked:F2} dB — it never engaged, "
                + "so the recovery measurement below is vacuous");

            // Quiet: far below the threshold, so nothing re-triggers it.
            const float probe = 0.005f;
            var window = 10 * SampleRate / 1000;
            var reference = Db(Rms(Sine(window, 0, probe, 1000f), 0, window));
            var recoveredMs = -1;
            for (var ms = 0; ms < 2000; ms += 10)
            {
                var outp = Render(chain, Sine(window, ms * SampleRate / 1000, probe, 1000f));
                if (reference - Db(Rms(outp, 0, window)) < 1f) { recoveredMs = ms; break; }
            }

            Assert.True(recoveredMs >= 0, "the ducker never came back up at all");
            Assert.True(recoveredMs <= releaseMs * 3f,
                $"the ducker took {recoveredMs} ms to recover, budget {releaseMs * 3f:F0} ms");
        }
        finally { chain.Clear(); }
    }

    // =======================================================================
    // Cross-cutting: ramps, NaN, rate, block independence, source shape
    // =======================================================================

    /// (module, setup, param, before, after) — every user-facing gain or depth
    /// this wave added. Modelled on <see cref="DspGainZipperTests" />' MixCases:
    /// three renders of the same stimulus (never moved / moved from the start /
    /// moved at the seam), and the seam must still be near the OLD value 128
    /// samples in. A per-block constant is 100 % of the way from sample 0.
    /// The `plucks` stimulus exists for ONE row and the reason is the module's
    /// whole design: a transient shaper is level-INDEPENDENT, so on a steady
    /// tone its two envelope followers agree, the gain is 0 dB and `mix` blends
    /// two identical signals. Measured: early span 0.00000 on the sine, i.e.
    /// this theory's own vacuity guard, whatever the core does.
    public static TheoryData<string, string, string, float, float, string> RampCases => new()
    {
        { "limiter",   "ceilingDb=0;releaseMs=500",                        "gainDb",    0f, 6f, "sine" },
        // attackDb, not sustainDb: the pluck train puts an ATTACK at sample 0 of
        // the measured block (its period divides the block), so a sustain-only
        // setting is 0 dB across the whole early window and the case reads as
        // vacuous — measured span 0.00000.
        { "transient", "attackDb=12;sustainDb=0;sensitivity=1",            "mix",       0f, 1f, "plucks" },
        { "multiband", "lowRatio=1;midRatio=1;highRatio=1",                "lowGainDb", 0f, 12f, "sine" },
        // rangeDb 12 -> 0 is the OPENING direction, and `attackMs` is set as
        // fast as the range allows so the ballistics are not what the probe is
        // measuring: without the smoother the gain is back at unity in ~5
        // samples.
        { "expander",  "thresholdDb=0;ratio=4;attackMs=0.1;releaseMs=5",   "rangeDb",   12f, 0f, "sine" },
        { "optocomp",  "peakReduction=0.8;emphasis=0",                     "gainDb",    0f, 12f, "sine" },
        { "ducker",    "thresholdDb=-60;attackMs=1;releaseMs=10;holdMs=500", "duckDb",  0f, 12f, "sine" },
        // The drawer's one "depth", and the row it was missing. `sensitivity`
        // sets the dB SPAN the fast-vs-slow difference is divided by, so it is
        // a multiplier on the applied gain: 0 -> 1 moves the span 18 -> 5 dB
        // and the gain by up to 3.6x. It was an unsmoothed double until
        // 2026-09-12 and read 100 % of the way at sample 0. `plucks` for the
        // same reason the row above it uses it — on a steady tone the two
        // followers agree and the span multiplies zero.
        { "transient", "attackDb=12;sustainDb=0;mix=1",                    "sensitivity", 0f, 1f, "plucks" },
    };

    [NativeAvailableTheory]
    [MemberData(nameof(RampCases))]
    public void ChangingAGainOrDepth_WalksAcrossTheSeam_InsteadOfStepping(
        string module, string setup, string param, float before, float after, string stimulus)
    {
        var refBefore = RenderSeam(module, setup, param, before, before, stimulus);
        var refAfter = RenderSeam(module, setup, param, after, after, stimulus);
        var seam = RenderSeam(module, setup, param, before, after, stimulus);

        const int earlyWindow = 128;   // ~27 % of the 10 ms ramp at 48 kHz
        const int lateStart = 1200;

        var earlySpan = RmsDiff(refBefore, refAfter, 0, earlyWindow);
        var lateSpan = RmsDiff(refBefore, refAfter, lateStart, Block - lateStart);
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

        var lateRatio = RmsDiff(seam, refAfter, lateStart, Block - lateStart) / lateSpan;
        Assert.True(lateRatio < 0.2f,
            $"{module}.{param}: {Block - lateStart} samples later the seam is still "
            + $"{lateRatio * 100f:F0} % away from the new setting — the ramp never landed");
    }

    /// <summary>
    ///     A NaN is REJECTED at the chain, and the module survives it: every
    ///     core's clamp is NaN-transparent, so a NaN that got through would
    ///     silence the node for the life of the chain.
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
                    // …and the value the module holds is untouched by the refusal.
                    Assert.True(float.IsFinite(node.GetParam(p.Id)),
                        $"{id}.{p.Id} kept a non-finite value after a rejected write");
                }

                // Drive it: a poisoned module is silent, not merely wrong.
                for (var b = 0; b < 3; b++) Render(chain, Sine(Block, b * Block, 0.4f, 220f));
                var outp = Render(chain, Sine(Block, 3 * Block, 0.4f, 220f));
                Assert.All(outp, s => Assert.True(float.IsFinite(s), $"{id} emitted a non-finite sample"));
                Assert.True(Rms(outp, 0, Block) > 0.01f,
                    $"{id} went silent after a rejected NaN write");
            }
        }
        finally { chain.Clear(); }
    }

    /// <summary>
    ///     Every rate-owned quantity is re-derived by <c>prepare()</c>, so the
    ///     same module behaves the same in TIME at 48 kHz and at 192 kHz — both
    ///     where it SETTLES and how long it takes to get there. A coefficient
    ///     computed once at 48 kHz runs four times too slowly at 192 kHz, which
    ///     is invisible until a device change.
    ///     <para>
    ///         The time half is the one that does the work: a settled level is
    ///         rate-independent by CONSTRUCTION (it comes out of the gain
    ///         computer, which knows nothing about the rate), so a probe that
    ///         only reads a steady state passes with every ballistic pinned to
    ///         48 kHz — measured.
    ///     </para>
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("expander")]
    [InlineData("ducker")]
    [InlineData("optocomp")]
    public void PrepareReDerivesEveryRateOwnedQuantity_SoTheTimingIsTheSameAtEveryRate(string module)
    {
        var (settled48, half48) = Trajectory(module, 48000);
        var (settled192, half192) = Trajectory(module, 192000);

        Assert.True(settled48 < -3f,
            $"{module} did not engage at 48 kHz ({settled48:F2} dB) — the case is vacuous");
        Assert.True(half48 > 4,
            $"{module} reached half its attenuation in {half48} ms at 48 kHz — too fast for this "
            + "probe's resolution to say anything, so the case is vacuous");
        Assert.True(MathF.Abs(settled48 - settled192) < 0.5f,
            $"{module} settles at {settled48:F2} dB at 48 kHz and {settled192:F2} dB at 192 kHz");

        var tolerance = MathF.Max(6f, half48 * 0.25f);
        Assert.True(MathF.Abs(half48 - half192) <= tolerance,
            $"{module} took {half48} ms to reach half its attenuation at 48 kHz and {half192} ms at "
            + $"192 kHz (tolerance {tolerance:F0} ms) — something rate-owned is not re-derived by "
            + "prepare()");

        // Where the module settles, and how many ms it took to get half way
        // there. Both are read off the SAMPLES, in 2 ms windows, so neither
        // depends on this test agreeing with the core's arithmetic.
        (float settledDb, int halfMs) Trajectory(string id, int rate)
        {
            var chain = Engine.GetDspChain();
            try
            {
                chain.Clear();
                var n = chain.AddNode(id);
                switch (id)
                {
                    // Ballistics slow enough that 2 ms windows can resolve them.
                    case "expander":
                        n.SetParam("thresholdDb", -20f); n.SetParam("ratio", 3f);
                        n.SetParam("rangeDb", 24f); n.SetParam("releaseMs", 200f);
                        break;
                    case "ducker":
                        n.SetParam("thresholdDb", -40f); n.SetParam("duckDb", 9f);
                        n.SetParam("attackMs", 150f); n.SetParam("holdMs", 500f);
                        break;
                    case "optocomp":
                        n.SetParam("peakReduction", 0.9f); n.SetParam("emphasis", 0f);
                        break;
                }

                const int stepMs = 2;
                const int spanMs = 2000;
                var amp = id == "expander" ? 0.01f : 0.5f;
                var window = stepMs * rate / 1000;
                var reference = Db(Rms(Sine(window, 0, amp, 1000f, rate), 0, window));

                var trail = new List<float>();
                for (var ms = 0; ms < spanMs; ms += stepMs)
                {
                    var outp = Render(chain, Sine(window, ms * rate / 1000, amp, 1000f, rate), rate);
                    trail.Add(Db(Rms(outp, 0, window)) - reference);
                }

                var settled = trail.Skip(trail.Count - 20).Average();
                var halfAt = -1;
                for (var i = 0; i < trail.Count; i++)
                    if (trail[i] <= settled * 0.5f) { halfAt = i * stepMs; break; }
                return (settled, halfAt);
            }
            finally { chain.Clear(); }
        }
    }

    /// <summary>
    ///     No core in this wave sizes ANY buffer by the block — everything is
    ///     sized by the channel count or by a param maximum — so a render must
    ///     be bit-identical however it is cut up. That is the reachable form of
    ///     "an oversized block is never half-processed": a core with block-sized
    ///     state would diverge the moment the chunking changed.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData("limiter")]
    [InlineData("transient")]
    [InlineData("multiband")]
    [InlineData("expander")]
    [InlineData("optocomp")]
    [InlineData("ducker")]
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
                    var piece = Render(chain, Sine(n, pos, 0.4f, 220f));
                    Array.Copy(piece, 0, outp, pos, n);
                }

                return outp;
            }
            finally { chain.Clear(); }
        }
    }

    /// <summary>
    ///     A source lint over the six new cores: nothing inside a
    ///     <c>process(...)</c> body may allocate, resize or take a lock. The RT
    ///     gate catches this at runtime for a module it happens to construct;
    ///     this catches it at the shape level for every one of them, including
    ///     the paths a stimulus never reaches.
    /// </summary>
    [Fact]
    public void NoDynamicsCore_AllocatesOrLocksInsideProcess()
    {
        string[] cores =
            { "limiter", "transient", "multiband", "expander", "optocomp", "ducker" };
        var banned = new Regex(@"\b(new\s|malloc|calloc|realloc|std::vector\s*<|\.resize\(|\.assign\(|"
                               + @"\.push_back\(|\.emplace|std::lock_guard|std::unique_lock|\.lock\(|"
                               + @"std::string|printf|fopen)", RegexOptions.Compiled);

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
            "these dynamics cores do something forbidden on the audio thread: "
            + string.Join(", ", offenders));
    }

    // =======================================================================
    // Helpers
    // =======================================================================

    private float[] RenderSeam(string module, string setup, string param, float first, float second,
                               string stimulus)
    {
        float[] Stim(int offset) => stimulus == "plucks" ? PluckTrain(offset) : Sine(Block, offset, 0.25f, 110f);

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

            // Settle detectors, followers and the chain's own add crossfade.
            const int preRoll = 6;
            for (var b = 0; b < preRoll; b++) Render(chain, Stim(b * Block));
            Render(chain, Stim(preRoll * Block));
            n.SetParam(param, second);
            return Render(chain, Stim((preRoll + 1) * Block));
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

    /// A ±0.5 square at 220 Hz — the worst case for a peak limiter.
    private static float[] Square(int offset)
    {
        var buf = new float[Block];
        for (var i = 0; i < Block; i++)
            buf[i] = MathF.Sin(2f * MathF.PI * 220f * (offset + i) / SampleRate) >= 0f ? 0.5f : -0.5f;
        return buf;
    }

    /// A 220 Hz sine whose amplitude jumps between 0.15 and 1.2 every 512
    /// samples: the level steps are what a look-ahead sees coming and an
    /// instantaneous gain computer does not.
    private static float[] Stepped(int offset)
    {
        var buf = new float[Block];
        for (var i = 0; i < Block; i++)
        {
            var t = offset + i;
            var amp = (t / 512) % 2 == 0 ? 0.15f : 1.2f;
            buf[i] = amp * MathF.Sin(2f * MathF.PI * 220f * t / SampleRate);
        }

        return buf;
    }

    /// A loud low tone plus a quiet high one, for the band-independence probe.
    private static float[] TwoTone(int offset)
    {
        var buf = new float[Block];
        for (var i = 0; i < Block; i++)
        {
            var t = offset + i;
            buf[i] = 0.4f * MathF.Sin(2f * MathF.PI * 80f * t / SampleRate)
                   + 0.05f * MathF.Sin(2f * MathF.PI * 6000f * t / SampleRate);
        }

        return buf;
    }

    /// An exponentially decaying 220 Hz note: a sharp attack and a long tail,
    /// which is exactly the two phases a transient designer separates.
    private static float[] Pluck(int frames)
    {
        var buf = new float[frames];
        const float tau = 0.08f * SampleRate;   // 80 ms
        for (var i = 0; i < frames; i++)
            buf[i] = 0.8f * MathF.Exp(-i / tau) * MathF.Sin(2f * MathF.PI * 220f * i / SampleRate);
        return buf;
    }

    /// A pluck every 512 samples, as a function of the ABSOLUTE index. A
    /// transient shaper is only ever doing anything while an envelope is
    /// MOVING, so this is the only stimulus its knobs can be measured on.
    private static float[] PluckTrain(int offset)
    {
        var buf = new float[Block];
        for (var i = 0; i < Block; i++)
        {
            var t = offset + i;
            var phase = t % 512;
            buf[i] = 0.5f * MathF.Exp(-phase / 120f) * MathF.Sin(2f * MathF.PI * 330f * t / SampleRate);
        }

        return buf;
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

    /// Magnitude at one frequency (Goertzel), so the two tones of a two-tone
    /// stimulus can be measured independently of each other.
    private static float Goertzel(float[] x, float hz)
    {
        var w = 2.0 * Math.PI * hz / SampleRate;
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
