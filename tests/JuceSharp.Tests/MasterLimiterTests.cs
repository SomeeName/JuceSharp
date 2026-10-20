using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     The ABI v34 master-bus ceiling, boundary half. No device is involved:
///     these pin the argument contract and the two-sided clamp, which is where
///     the v29 EQ's "a band the engine silently moved is a band whose drawn
///     curve and audible curve disagree" rule applies just as well.
/// </summary>
[Collection("NativeEngine")]
public sealed class MasterLimiterAbiTests : IDisposable
{
    private readonly JuceAudioEngine? _engine;

    public MasterLimiterAbiTests()
    {
        if (!NativeProbe.IsAvailable) return;
        // dca_engine_create opens a device; stop it immediately so nothing here
        // sits on the host's sound card (an ASIO driver is typically exclusive).
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    /// <summary>
    ///     <b>OFF at engine start, and the ceiling/release defaults are what the
    ///     host gets when it opts IN.</b> ABI v34 shipped this default ON and v35
    ///     reversed it: the live monitor path runs through the stage, so
    ///     default-ON made this engine's own rule — "everything is OFF at engine
    ///     start; a user must not pay for, or hear any change from, a feature they
    ///     are not using" — true only for a user who had switched it off. It cost
    ///     every guitarist 1 ms of latency on their own playing and moved every
    ///     consumer's click alignment by 48 samples at 48 kHz.
    /// </summary>
    [NativeAvailableFact]
    public void DefaultsAreOff_AndTheCeilingDefaultsApplyWhenTheHostOptsIn()
    {
        var atStart = _engine!.GetMasterLimiter();
        Assert.False(atStart.Enabled);
        // …and nothing is in the path while it is off.
        Assert.Equal(0, _engine.MasterLimiterLatencySamples);

        _engine.SetMasterLimiter(true);
        var (enabled, ceilingDb, releaseMs) = _engine.GetMasterLimiter();
        Assert.True(enabled);
        Assert.Equal(-0.3f, ceilingDb, 3);
        Assert.Equal(80f, releaseMs, 3);
        _engine.SetMasterLimiter(false);
    }

    [NativeAvailableFact]
    public void RangesAreClampedOnBothSides_SoACachedGetterCannotDisagree()
    {
        _engine!.SetMasterLimiter(true, ceilingDb: 12f, releaseMs: 100_000f);
        var high = _engine.GetMasterLimiter();
        Assert.Equal(0f, high.CeilingDb, 3);
        Assert.Equal(1000f, high.ReleaseMs, 3);

        _engine.SetMasterLimiter(true, ceilingDb: -500f, releaseMs: -5f);
        var low = _engine.GetMasterLimiter();
        Assert.Equal(-24f, low.CeilingDb, 3);
        Assert.Equal(1f, low.ReleaseMs, 3);

        _engine.SetMasterLimiter(true, -0.3f, 80f);
    }

    /// <summary>
    ///     The read-back a SETTINGS ROW has to trust. The clamp test above uses
    ///     +12 dB and 100,000 ms — values nobody types — so it reads as a
    ///     boundary curiosity; +5 dBFS is a number a user actually enters when
    ///     they want "a bit of headroom above unity", and it is silently
    ///     impossible. The engine clamps it to 0 and keeps the CLAMPED value,
    ///     and <c>GetMasterLimiter</c> derives its dB from what is stored
    ///     (<c>20·log10</c> of the linear ceiling the setter wrote), never from
    ///     the request. So a UI that renders the number it put in its own config
    ///     shows a ceiling the audio path is not using.
    /// </summary>
    [NativeAvailableFact]
    public void APlusFiveDbCeilingReadsBackClamped_SoASettingsRowMustRenderFromTheGetter()
    {
        _engine!.SetMasterLimiter(true, ceilingDb: 5f, releaseMs: 80f);

        var held = _engine.GetMasterLimiter();
        Assert.True(held.Enabled);
        Assert.Equal(0f, held.CeilingDb, 3);          // NOT the +5 that was asked for
        Assert.NotEqual(5f, held.CeilingDb, 3);
        Assert.Equal(80f, held.ReleaseMs, 3);

        // …and it is STICKY: reading twice does not drift back toward the
        // request, because the clamp happened once, on the way in.
        Assert.Equal(0f, _engine.GetMasterLimiter().CeilingDb, 3);

        _engine.SetMasterLimiter(false);
    }

    [Theory]
    [InlineData(float.NaN, 80f)]
    [InlineData(float.PositiveInfinity, 80f)]
    [InlineData(-0.3f, float.NaN)]
    [InlineData(-0.3f, float.NegativeInfinity)]
    public void NonFiniteIsRejected_NeverClamped(float ceilingDb, float releaseMs)
    {
        Assert.SkipUnless(NativeProbe.IsAvailable, "native engine not available");
        // A clamp is built out of `v < lo` / `v > hi`, and BOTH are false for a
        // NaN — so a clamped NaN reaches the ceiling and the limiter multiplies
        // the bus by NaN for the rest of the session. Same rule as
        // DspChain::setParam.
        Assert.Throws<JuceAudioException>(() => _engine!.SetMasterLimiter(true, ceilingDb, releaseMs));

        // ...and the rejection left the previous configuration intact.
        var held = _engine!.GetMasterLimiter();
        Assert.True(float.IsFinite(held.CeilingDb));
        Assert.True(float.IsFinite(held.ReleaseMs));
    }

    [NativeAvailableFact]
    public void ADisposedEngine_ThrowsRatherThanReportingAPlausibleZero()
    {
        var engine = new JuceAudioEngine();
        engine.Stop();
        engine.Dispose();
        // The SETTER no-ops, like every other master-bus setter (SetMasterGain,
        // SetMonitorGain, SetHighPass…): it is a UI toggle in a tick loop, and
        // this was the one that threw. The GETTERS still throw — reporting a
        // plausible "off, -0.3 dB" for a corpse is a worse answer than an
        // exception.
        engine.SetMasterLimiter(true);
        Assert.Throws<ObjectDisposedException>(() => engine.GetMasterLimiter());
        Assert.Throws<ObjectDisposedException>(() => engine.MasterLimiterLatencySamples);
        // The poll-only meter is deliberately the ONE that degrades quietly: a
        // UI timer that outlives the engine must not throw 30 times a second.
        Assert.Equal(0f, engine.MasterLimiterReductionDb);
    }
}

/// <summary>
///     The behavioural half, end to end through a real audio callback on the
///     null device (see <see cref="NullDeviceTests" />).
///     <para>
///         This is the only place the limiter can be OBSERVED from managed code:
///         it runs in <c>EngineCallback::audioDeviceIOCallbackWithContext</c>,
///         after the final scrub and before <c>mirror_.pushBlock</c>, and no
///         offline path reaches it. The sample-accurate "output never exceeds
///         the ceiling" assertion lives in the native RT gate
///         (<c>RtMasterLimiterHoldsTheCeilingWithoutAllocating</c>), which can
///         read the rendered block back; what is provable here is that the
///         stage is REACHED, that it engages on an over-unity bus and that it
///         does nothing at all when it is switched off.
///     </para>
/// </summary>
[Collection("NullDevice")]
public sealed class MasterLimiterNullDeviceTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    /// <summary>Poll until <paramref name="predicate"/> holds or the budget is
    /// spent. The ASSERTION is on the value, never on how long it took — a
    /// wall-clock threshold flakes the moment two suites run at once.
    /// <para>
    ///     <paramref name="steps"/> exists because the NEGATIVE polls — the ones
    ///     asserting the predicate never becomes true — spend the whole budget by
    ///     construction, and at the 300-step default that is 3 s per test doing
    ///     nothing. A negative poll only has to outlast a few callbacks.
    /// </para></summary>
    private static float PollReduction(JuceAudioEngine engine, Func<float, bool> predicate,
                                       int steps = 300)
    {
        var last = 0f;
        for (var i = 0; i < steps; i++)
        {
            last = engine.MasterLimiterReductionDb;
            if (predicate(last)) return last;
            Thread.Sleep(10);
        }
        return last;
    }

    [NativeAvailableFact]
    public void AnOverUnityBus_IsLimitedWhenOn_AndUntouchedWhenOff()
    {
        var wav = TinyWav.Create(seconds: 6.0);
        try
        {
            using var engine = NullDevice.CreateEngine();
            using var source = engine.OpenFile(wav);

            // TinyWav is a 0.1-amplitude sine; x20 puts the master bus at 2.0,
            // i.e. +6 dB past full scale. That is the shipping shape of this
            // fault — a loud rig over a loud backing track — reproduced with
            // the one gain a device-less test can reach.
            engine.SetMasterGain(20f);
            engine.SetMasterLimiter(false);
            source.Play();

            // --- OFF: the stage must do NOTHING -------------------------
            var off = PollReduction(engine, db => db > 0f, steps: 30);
            Assert.Equal(0f, off);

            // --- ON: it must engage on the very same signal -------------
            engine.SetMasterLimiter(true, -0.3f, 80f);
            var on = PollReduction(engine, db => db > 3f);
            Assert.True(on > 3f,
                $"a 2.0-amplitude master bus against a -0.3 dBFS ceiling reported only {on} dB of " +
                "gain reduction — the limiter stage is not being reached from the audio callback");
            // 20*log10(2.0/0.966) = 6.3 dB is the arithmetic answer; the bound
            // is loose because the source's own envelope is not a constant 0.1.
            Assert.True(on < 24f, $"{on} dB of reduction is past the -24 dBFS ceiling floor");

            // --- ...and switching it off again releases it ---------------
            engine.SetMasterLimiter(false);
            var released = PollReduction(engine, db => db == 0f);
            Assert.Equal(0f, released);
        }
        finally { TinyWav.TryDelete(wav); }
    }

    [NativeAvailableFact]
    public void TheLookaheadIsRealLatency_AndIsFoldedIntoTheOutputLatency()
    {
        using var engine = NullDevice.CreateEngine();

        // OFF at engine start since v35 — the host opts in, and this test is
        // about what the stage costs once it has.
        Assert.Equal(0, engine.MasterLimiterLatencySamples);
        engine.SetMasterLimiter(true, -0.3f, 80f);

        var limiter = engine.MasterLimiterLatencySamples;
        // 1.0 ms at 48 kHz. The ABI documents the concept as capped at 1.5 ms
        // because the LIVE MONITOR path runs through this stage — a guitarist
        // pays it on their own playing.
        Assert.InRange(limiter, 1, (int)(48000 * 0.0015));

        Assert.True(engine.OutputLatencySamples >= limiter,
            "dca_engine_get_output_latency_samples does not include the limiter's look-ahead — " +
            "a caller aligning scheduled clicks against it would place every click early by " +
            $"{limiter} samples");

        // It FOLLOWS the enabled flag, and that is the load-bearing part:
        // switched off the stage is not in the path at all, so the engine is
        // byte-for-byte the pre-v34 engine and this reports 0. Keeping the
        // delay line running so the number never moved was tried and rejected —
        // it makes every user pay 1 ms on their own monitored playing for a
        // feature they switched off, and it re-times the whole engine (it broke
        // the RT gate's RtEqTrimRampsAndSparseInputIsSilent).
        engine.SetMasterLimiter(false);
        Assert.Equal(0, engine.MasterLimiterLatencySamples);
        engine.SetMasterLimiter(true);
        Assert.Equal(limiter, engine.MasterLimiterLatencySamples);
    }

    /// <summary>
    ///     The N -> 0 -> N re-read contract WHILE THE CALLBACK IS RUNNING. The
    ///     sibling above asserts it on a stopped engine and the RT gate asserts it
    ///     natively; neither covered the managed half of the documented mid-stream
    ///     toggle, which is the only way a shipping app ever does it — a user
    ///     flipping the switch during a song.
    /// </summary>
    [NativeAvailableFact]
    public void TheLatencyFollowsAMidStreamToggle_SoAClickSchedulerMustReRead()
    {
        var wav = TinyWav.Create(seconds: 6.0);
        try
        {
            using var engine = NullDevice.CreateEngine();
            using var source = engine.OpenFile(wav);
            engine.SetMasterGain(20f);
            source.Play();

            // Playing, limiter still off: nothing in the path.
            Assert.Equal(0, engine.MasterLimiterLatencySamples);
            var offLatency = engine.OutputLatencySamples;

            engine.SetMasterLimiter(true, -0.3f, 80f);
            var on = engine.MasterLimiterLatencySamples;
            Assert.True(on > 0, "the limiter reported no look-ahead while switched ON mid-stream");
            Assert.Equal(offLatency + on, engine.OutputLatencySamples);
            // …and it really engaged, i.e. this is not just a flag moving.
            Assert.True(PollReduction(engine, db => db > 3f) > 3f);

            engine.SetMasterLimiter(false);
            Assert.Equal(0, engine.MasterLimiterLatencySamples);
            Assert.Equal(offLatency, engine.OutputLatencySamples);
            Assert.Equal(0f, PollReduction(engine, db => db == 0f));
        }
        finally { TinyWav.TryDelete(wav); }
    }

    /// <summary>
    ///     JuceSharp #31: a SPARSE output mask (outputs 3-4 on, 1-2 off). JUCE's
    ///     devices hand the callback the ENABLED outputs packed (two pointers
    ///     here), and every master-bus stage is now sized to exactly that width.
    ///     If a stage were ever sized narrower than what the callback is handed,
    ///     the limiter's skip-don't-grow rule would skip EVERY block of this
    ///     session: no reduction and a reported look-ahead of 0. The native RT
    ///     gate (<c>RtSparseOutputMaskIsPackedAndTheMasterBusKeepsLAndRApart</c>)
    ///     reads the pointers themselves; this is the managed, end-to-end half.
    /// </summary>
    [NativeAvailableFact]
    public void ASparseOutputMask_StillReachesTheLimiterOnEveryBlock()
    {
        var wav = TinyWav.Create(seconds: 6.0);
        try
        {
            using var engine = new JuceAudioEngine(new JuceEngineConfig
            {
                OutputTypeName    = NullDevice.TypeName,
                SampleRate        = 48000,
                BufferFrames      = 480,
                OutputChannelMask = 0b1100,
            });
            using var source = engine.OpenFile(wav);
            engine.SetMasterGain(20f);
            engine.SetMasterLimiter(true, -0.3f, 80f);
            source.Play();

            var on = PollReduction(engine, db => db > 3f);
            Assert.True(on > 3f,
                $"on a sparse output mask the limiter reported only {on} dB of reduction on a " +
                "2.0-amplitude bus — it is skipping the blocks the callback hands it");
            Assert.True(engine.MasterLimiterLatencySamples > 0,
                "on a sparse output mask the limiter reports no look-ahead: the last block skipped it");
        }
        finally { TinyWav.TryDelete(wav); }
    }
}
