using Xunit;

namespace JuceSharp.Tests;

/// <summary>Integration tests for <see cref="JuceAudioSource"/>. Opens a
/// fresh source per test against a generated tiny WAV.</summary>
[Collection("NativeEngine")]
public sealed class JuceAudioSourceTests : IDisposable
{
    private readonly JuceAudioEngine? _engine;
    private readonly string? _wavPath;

    public JuceAudioSourceTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _wavPath = TinyWav.Create(seconds: 1.0);
    }

    public void Dispose()
    {
        _engine?.Dispose();
        if (_wavPath is not null) TinyWav.TryDelete(_wavPath);
    }

    private JuceAudioSource OpenSource() => _engine!.OpenFile(_wavPath!);

    [NativeAvailableFact]
    public void OpenFile_Length_IsPositive()
    {
        using var src = OpenSource();
        Assert.True(src.Length.TotalSeconds > 0);
    }

    [NativeAvailableFact]
    public void Position_StartsNearZero()
    {
        using var src = OpenSource();
        Assert.True(src.Position.TotalSeconds < 0.5);
    }

    [NativeAvailableFact]
    public void AudiblePosition_StartsNearZero()
    {
        using var src = OpenSource();
        Assert.True(src.AudiblePosition.TotalSeconds < 0.5);
    }

    [NativeAvailableFact]
    public void PlayPauseStop_DoNotThrow()
    {
        using var src = OpenSource();
        src.Play();
        src.Pause();
        src.Stop();
    }

    [NativeAvailableFact]
    public void Seek_ChangesPosition()
    {
        using var src = OpenSource();
        src.Seek(TimeSpan.FromMilliseconds(250));
        // Position should reflect the seek (within float tolerance).
        Assert.True(src.Position.TotalMilliseconds >= 200);
    }

    [NativeAvailableFact]
    public void Volume_Setter_DoesNotThrow()
    {
        using var src = OpenSource();
        src.Volume = 0.0f;
        src.Volume = 0.5f;
        src.Volume = 1.0f;
    }

    [NativeAvailableFact]
    public void TempoRatio_Setter_DoesNotThrow()
    {
        using var src = OpenSource();
        src.TempoRatio = 1.0f;
        src.TempoRatio = 0.5f;
        src.TempoRatio = 2.0f;
    }

    [NativeAvailableFact]
    public void StemGrouped_Toggle_And_Group_Tempo_Pitch_Are_Honored()
    {
        // ABI v9 round-trip: enable stem-group routing, push tempo/pitch
        // through the engine's shared stretcher, then un-group. None of
        // these operations should throw; the SUM-then-stretch path is
        // exercised by the engine internally.
        using var a = OpenSource();
        using var b = OpenSource();

        Assert.False(a.StemGrouped);
        a.StemGrouped = true;
        b.StemGrouped = true;
        Assert.True(a.StemGrouped);
        Assert.True(b.StemGrouped);

        _engine!.StemGroupTempo = 0.5f;
        _engine!.StemGroupPitchSemitones = 2.0f;
        Assert.Equal(0.5f, _engine.StemGroupTempo);
        Assert.Equal(2.0f, _engine.StemGroupPitchSemitones);

        // Tear down in reverse — last leave should drop the shared stretcher
        // without leaking. (No throw is the assertion.)
        a.StemGrouped = false;
        b.StemGrouped = false;
        Assert.False(a.StemGrouped);
    }

    [NativeAvailableFact]
    public void Disposing_A_Grouped_Source_Detaches_From_StemBus()
    {
        // Regression: disposing a JuceAudioSource while it's still marked
        // StemGrouped used to leave a dangling panner pointer inside the
        // engine's stem-group sub-mixer; the audio thread's next block was
        // an immediate 0xC0000005 access violation. Native removeSource()
        // now un-groups first.
        var a = OpenSource();
        var b = OpenSource();
        a.StemGrouped = true;
        b.StemGrouped = true;

        // Drop both sources without first setting StemGrouped=false.
        a.Dispose();
        b.Dispose();

        // Engine remains usable — fetch a config-level value and create a
        // fresh source to prove the bus was cleanly torn down.
        Assert.True(_engine!.SampleRate > 0);
        using var c = OpenSource();
        c.StemGrouped = true;
        c.StemGrouped = false;
    }

    [NativeAvailableFact]
    public void PitchSemitones_Setter_DoesNotThrow()
    {
        using var src = OpenSource();
        src.PitchSemitones = 0f;
        src.PitchSemitones = 7f;
        src.PitchSemitones = -5f;
    }

    [NativeAvailableFact]
    public void Pan_Setter_DoesNotThrow()
    {
        using var src = OpenSource();
        src.Pan = -1f;
        src.Pan = 0f;
        src.Pan = 1f;
    }

    [NativeAvailableFact]
    public void TempoPitchPan_RoundTripCachedValues()
    {
        using var src = OpenSource();
        Assert.Equal(1f, src.TempoRatio);
        Assert.Equal(0f, src.PitchSemitones);
        Assert.Equal(0f, src.Pan);

        src.TempoRatio = 0.75f;
        src.PitchSemitones = 3f;
        src.Pan = -0.5f;

        Assert.Equal(0.75f, src.TempoRatio);
        Assert.Equal(3f, src.PitchSemitones);
        Assert.Equal(-0.5f, src.Pan);
    }

    [NativeAvailableFact]
    public void Dispose_IsIdempotent()
    {
        var src = OpenSource();
        src.Dispose();
        src.Dispose();
    }

    [NativeAvailableFact]
    public void AccessAfterDispose_Throws()
    {
        var src = OpenSource();
        src.Dispose();
        Assert.Throws<ObjectDisposedException>(() => _ = src.Position);
        Assert.Throws<ObjectDisposedException>(() => _ = src.AudiblePosition);
        Assert.Throws<ObjectDisposedException>(() => _ = src.Length);
        Assert.Throws<ObjectDisposedException>(() => src.Play());
        Assert.Throws<ObjectDisposedException>(() => src.Pause());
        Assert.Throws<ObjectDisposedException>(() => src.Stop());
        Assert.Throws<ObjectDisposedException>(() => src.Seek(TimeSpan.Zero));
        Assert.Throws<ObjectDisposedException>(() => src.Volume = 1f);
        Assert.Throws<ObjectDisposedException>(() => src.TempoRatio = 1f);
        Assert.Throws<ObjectDisposedException>(() => src.PitchSemitones = 0f);
        Assert.Throws<ObjectDisposedException>(() => src.Pan = 0f);
    }
}

/// <summary>
///     W4-M6. <c>dca_source_set_tempo</c> / <c>_set_pitch</c> return
///     <c>DCA_OK</c> for a source that is in the stem group and do NOTHING —
///     the header says so outright ("per-source set_tempo / set_pitch are
///     no-ops while grouped (the group stretcher owns those)"). The wrapper
///     took that success and moved its cache, so
///     <see cref="JuceAudioSource.TempoRatio" /> reported 1.4 while the stem
///     played at whatever <see cref="JuceAudioEngine.StemGroupTempo" /> said.
///     <see cref="JuceAudioSource.StemGrouped" />'s own doc already knew — "setting
///     tempo / pitch on this source is a no-op while grouped" — and the two
///     members it was talking about did not. Same rule
///     <see cref="JuceAudioSource.Volume" /> and <see cref="JuceAudioSource.Pan" />
///     follow: a cached getter must report what the engine actually runs.
/// </summary>
[Collection("NullDevice")]
public sealed class GroupedStemTempoAndPitchTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    [NativeAvailableFact]
    public void AGroupedStem_DoesNotCacheATempoTheEngineIsNotRunning()
    {
        using var engine = NullDevice.CreateEngine();
        using var src = engine.OpenFile(TinyWav.Create());

        src.TempoRatio = 1.25f;                 // ungrouped: honoured, cached
        Assert.Equal(1.25f, src.TempoRatio);

        src.StemGrouped = true;
        src.TempoRatio = 2f;                    // the engine ignores this

        Assert.NotEqual(2f, src.TempoRatio);
        Assert.Equal(1.25f, src.TempoRatio);
    }

    [NativeAvailableFact]
    public void AGroupedStem_DoesNotCacheAPitchTheEngineIsNotRunning()
    {
        using var engine = NullDevice.CreateEngine();
        using var src = engine.OpenFile(TinyWav.Create());

        src.PitchSemitones = -2f;
        Assert.Equal(-2f, src.PitchSemitones);

        src.StemGrouped = true;
        src.PitchSemitones = 7f;

        Assert.NotEqual(7f, src.PitchSemitones);
        Assert.Equal(-2f, src.PitchSemitones);
    }

    /// <summary>Un-grouping hands the source back its own stretcher, and the
    /// setters work again — the drop is scoped to the grouped state, not a
    /// permanent latch.</summary>
    [NativeAvailableFact]
    public void UnGrouping_RestoresThePerSourceSetters()
    {
        using var engine = NullDevice.CreateEngine();
        using var src = engine.OpenFile(TinyWav.Create());

        src.StemGrouped = true;
        src.TempoRatio = 2f;
        Assert.Equal(1f, src.TempoRatio);

        src.StemGrouped = false;
        src.TempoRatio = 2f;
        Assert.Equal(2f, src.TempoRatio);
    }
}
