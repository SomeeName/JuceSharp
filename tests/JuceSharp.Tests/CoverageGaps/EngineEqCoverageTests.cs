using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The ABI v29 parametric EQ. The entire managed surface —
/// <c>SetEqBand / SetEqEnabled / SetEqRoute / SetEqSolo / SetEqTrim /
/// ReadEqSpectrum / GetEqActiveCount / EqBandCount</c> — had ZERO test
/// references, which for a feature whose whole contract is "a band is
/// published only when it is AUDIBLE" means nothing was checking the
/// predicate that decides it.
///
/// <para>
/// Everything here reads <c>GetEqActiveCount()</c>, which is the ENGINE's own
/// answer rather than a second count computed managed-side, so these are
/// assertions about the DSP and not about the wrapper's bookkeeping. The band
/// type / route are passed as the raw ABI integers on purpose (see
/// <c>dca_eq_band_type</c> / <c>dca_eq_route</c> in
/// <c>include/dotcharter_audio.h</c>).
/// </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class EngineEqCoverageTests
{
    // dca_eq_band_type
    private const int HighPass  = 0;
    private const int LowShelf  = 1;
    private const int Bell      = 2;
    private const int Notch     = 5;

    // dca_eq_route
    private const int RouteSong  = 0;
    private const int RouteInput = 1;
    private const int RouteBoth  = 2;

    private static JuceAudioEngine? OpenOrSkip()
    {
        var engine = new JuceAudioEngine();
        if (engine.SampleRate > 0) return engine;
        engine.Dispose();
        Assert.Skip("no audio device on this host — the constructor opened none");
        return null;
    }

    [Fact]
    public void EqBandCount_matches_the_abi()
    {
        // DCA_EQ_BAND_COUNT. A managed constant that drifts from the header
        // silently truncates or over-runs the caller's band array.
        Assert.Equal(6, JuceAudioEngine.EqBandCount);
    }

    /// <summary>Everything is OFF at engine start — a solo player must not pay
    /// for a feature they have not switched on.</summary>
    [NativeAvailableFact]
    public void A_fresh_engine_runs_no_eq_bands()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        Assert.Equal(0, engine.GetEqActiveCount());
    }

    /// <summary>
    /// The audibility predicate, exactly as
    /// <c>AudioEngine::eqActiveCount()</c> and the message-thread publish loop
    /// both spell it: master power on, AND (soloed, or enabled with nothing
    /// soloed). Note what it deliberately does NOT include — a band's GAIN.
    /// A bell parked at 0 dB is a wire but still counts, because the count
    /// answers "how many sections is the callback running", which is what the
    /// badge showing it is for.
    /// </summary>
    [NativeAvailableFact]
    public void A_band_counts_only_while_it_is_audible()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        // Configured but the section is off.
        engine.SetEqBand(0, Bell, 1000f, 1.0f, 6f, enabled: true);
        Assert.Equal(0, engine.GetEqActiveCount());

        // Powered: now it is audible.
        engine.SetEqEnabled(true);
        Assert.Equal(1, engine.GetEqActiveCount());

        // Gain is not part of the predicate.
        engine.SetEqBand(0, Bell, 1000f, 1.0f, 0f, enabled: true);
        Assert.Equal(1, engine.GetEqActiveCount());

        // Band disabled while the section stays on.
        engine.SetEqBand(0, Bell, 1000f, 1.0f, 6f, enabled: false);
        Assert.Equal(0, engine.GetEqActiveCount());

        // …and back.
        engine.SetEqBand(0, Bell, 1000f, 1.0f, 6f, enabled: true);
        Assert.Equal(1, engine.GetEqActiveCount());

        engine.SetEqEnabled(false);
        Assert.Equal(0, engine.GetEqActiveCount());
    }

    /// <summary>Every shape the ABI defines is accepted and published.</summary>
    [NativeAvailableFact]
    public void The_gainless_shapes_are_audible_without_a_gain()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        engine.SetEqEnabled(true);
        foreach (var type in new[] { HighPass, Notch })
        {
            engine.SetEqBand(0, type, 800f, 0.7f, 0f, enabled: true);
            Assert.Equal(1, engine.GetEqActiveCount());
        }

        engine.SetEqBand(0, LowShelf, 120f, 0.7f, -8f, enabled: true);
        Assert.Equal(1, engine.GetEqActiveCount());
    }

    [NativeAvailableFact]
    public void Several_bands_accumulate_up_to_the_band_count()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        engine.SetEqEnabled(true);
        for (var i = 0; i < JuceAudioEngine.EqBandCount; i++)
        {
            engine.SetEqBand(i, Bell, 200f * (i + 1), 1.0f, 4f, enabled: true);
            Assert.Equal(i + 1, engine.GetEqActiveCount());
        }

        // An out-of-range index is rejected, never clamped onto a real band —
        // and since W4-C4 the rejection is the caller's to see. This case
        // asserted only that the active count did not move, which is exactly
        // what a swallowed index looks like from outside.
        Assert.Throws<ArgumentOutOfRangeException>(
            () => engine.SetEqBand(JuceAudioEngine.EqBandCount, Bell, 1000f, 1f, 6f, enabled: true));
        Assert.Throws<ArgumentOutOfRangeException>(
            () => engine.SetEqBand(-1, Bell, 1000f, 1f, 6f, enabled: true));
        Assert.Equal(JuceAudioEngine.EqBandCount, engine.GetEqActiveCount());
    }

    /// <summary>Solo collapses the section to exactly one band; a negative
    /// index is the documented "no solo".</summary>
    [NativeAvailableFact]
    public void Solo_narrows_the_active_set_to_one_band_and_releases_cleanly()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        engine.SetEqEnabled(true);
        for (var i = 0; i < 3; i++)
            engine.SetEqBand(i, Bell, 200f * (i + 1), 1.0f, 4f, enabled: true);
        Assert.Equal(3, engine.GetEqActiveCount());

        engine.SetEqSolo(1);
        Assert.Equal(1, engine.GetEqActiveCount());

        engine.SetEqSolo(-1);
        Assert.Equal(3, engine.GetEqActiveCount());
    }

    [NativeAvailableFact]
    public void Route_and_trim_do_not_change_which_bands_are_audible()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        engine.SetEqEnabled(true);
        engine.SetEqBand(0, Bell, 1000f, 1.0f, 6f, enabled: true);
        Assert.Equal(1, engine.GetEqActiveCount());

        foreach (var route in new[] { RouteSong, RouteInput, RouteBoth, RouteSong })
        {
            engine.SetEqRoute(route);
            Assert.Equal(1, engine.GetEqActiveCount());
        }

        foreach (var trim in new[] { -12f, 0f, 6f })
        {
            engine.SetEqTrim(trim);
            Assert.Equal(1, engine.GetEqActiveCount());
        }
    }

    /// <summary>
    /// <c>ReadEqSpectrum</c> documents a
    /// [<see cref="JuceAudioEngine.MinEqSpectrumBins" />,
    /// <see cref="JuceAudioEngine.MaxEqSpectrumBins" />] buffer window and a
    /// written count. Outside it the answer used to be 0 — indistinguishable
    /// from "the engine had nothing" — and is an
    /// <see cref="ArgumentOutOfRangeException" /> since W4-C4.
    /// </summary>
    [NativeAvailableFact]
    public void ReadEqSpectrum_honours_its_buffer_bounds()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        Assert.Throws<ArgumentOutOfRangeException>(() => engine.ReadEqSpectrum(new float[7]));
        Assert.Throws<ArgumentOutOfRangeException>(() => engine.ReadEqSpectrum(new float[513]));
        Assert.Throws<ArgumentNullException>(() => engine.ReadEqSpectrum(null!));

        foreach (var size in new[] { 8, 128, 512 })
        {
            var dest = new float[size];
            var written = engine.ReadEqSpectrum(dest);
            Assert.InRange(written, 0, size);
            // Every value the engine claims to have written must be a real
            // dBFS number — a NaN here would poison whatever draws it.
            for (var i = 0; i < written; i++)
                Assert.True(float.IsFinite(dest[i]), $"bin {i} of {written} was {dest[i]}");
        }
    }

    /// <summary>Every EQ entry point is a no-op (never a throw, never a
    /// crash) once the engine has gone away — they all resolve the handle
    /// through TryAcquire.</summary>
    [NativeAvailableFact]
    public void The_eq_surface_is_inert_after_dispose()
    {
        var engine = OpenOrSkip();
        if (engine is null) return;
        engine.Dispose();

        engine.SetEqEnabled(true);
        engine.SetEqBand(0, Bell, 1000f, 1f, 6f, enabled: true);
        engine.SetEqSolo(0);
        engine.SetEqRoute(RouteBoth);
        engine.SetEqTrim(3f);

        Assert.Equal(0, engine.GetEqActiveCount());
        Assert.Equal(0, engine.ReadEqSpectrum(new float[64]));
    }
}

/// <summary>
/// W4-C4. Two argument bugs on the EQ surface, both the shape
/// <c>JuceAudioEngine.ValidatePlayer</c>'s own doc argues against at length:
/// a swallowed index "reads exactly like 'the ring is empty', i.e. a guitar
/// that is plugged in and silent".
/// </summary>
[Collection("NullDevice")]
public sealed class EngineEqArgumentValidationTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    /// <summary>
    /// <c>SetEqBand(7, …)</c> was a LogWarning and nothing else, five lines under
    /// the constant that bounds it, while every seat index in the same file
    /// threw. The untyped <c>int type</c> overload is the one under test because
    /// the typed one forwards to it.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(-1)]
    [InlineData(JuceAudioEngine.EqBandCount)]
    [InlineData(7)]
    public void SetEqBand_RejectsABandIndexTheEngineDoesNotHave(int index)
    {
        using var engine = NullDevice.CreateEngine();

        var ex = Assert.Throws<ArgumentOutOfRangeException>(
            () => engine.SetEqBand(index, 0, 1000f, 0.707f, 3f, enabled: true));
        Assert.Equal("index", ex.ParamName);

        // …and the typed twin inherits it rather than having its own hole.
        Assert.Throws<ArgumentOutOfRangeException>(
            () => engine.SetEqBand(index, JuceEqBandType.Bell, 1000f, 0.707f, 3f, enabled: true));
    }

    /// <summary>Every index the engine DOES have is still accepted, so the guard
    /// is a bound and not a blanket refusal.</summary>
    [NativeAvailableFact]
    public void SetEqBand_AcceptsEveryBandTheEngineHas()
    {
        using var engine = NullDevice.CreateEngine();
        for (var i = 0; i < JuceAudioEngine.EqBandCount; i++)
            engine.SetEqBand(i, JuceEqBandType.Bell, 1000f, 0.707f, 0f, enabled: false);
    }

    /// <summary>
    /// <c>ReadEqSpectrum</c> answered a wrong-sized buffer with 0 — which a
    /// caller cannot tell from "the engine had nothing" — off two unnamed magic
    /// numbers. Both bounds are now named constants and both throw.
    /// </summary>
    [NativeAvailableFact]
    public void ReadEqSpectrum_RejectsABufferOutsideItsNamedBounds()
    {
        using var engine = NullDevice.CreateEngine();

        var tooSmall = Assert.Throws<ArgumentOutOfRangeException>(
            () => engine.ReadEqSpectrum(new float[JuceAudioEngine.MinEqSpectrumBins - 1]));
        Assert.Equal("destDb", tooSmall.ParamName);

        var tooBig = Assert.Throws<ArgumentOutOfRangeException>(
            () => engine.ReadEqSpectrum(new float[JuceAudioEngine.MaxEqSpectrumBins + 1]));
        Assert.Equal("destDb", tooBig.ParamName);

        // Both ends of the accepted range still return a count rather than
        // throwing, so the two constants really are the bounds.
        Assert.True(engine.ReadEqSpectrum(new float[JuceAudioEngine.MinEqSpectrumBins]) >= 0);
        Assert.True(engine.ReadEqSpectrum(new float[JuceAudioEngine.MaxEqSpectrumBins]) >= 0);
    }
}
