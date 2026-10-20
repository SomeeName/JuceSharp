namespace JuceSharp;

/// <summary>
/// The contract of <see cref="JuceAudioSource" />; implemented by
/// <see cref="JuceAudioSource" />. A test seam, so transport logic can be
/// exercised without a native handle — see
/// <c>docs/guides/testing-without-native.md</c>.
/// <para>
///     Every member is <c>&lt;inheritdoc&gt;</c> onto its concrete counterpart:
///     one source of truth. Pinned by <c>Wave2DocsInterfaceDocTests</c>.
/// </para>
/// </summary>
public interface IJuceAudioSource : IDisposable
{
    /// <inheritdoc cref="JuceAudioSource.Position" />
    TimeSpan Position { get; }

    /// <inheritdoc cref="JuceAudioSource.AudiblePosition" />
    TimeSpan AudiblePosition { get; }

    /// <inheritdoc cref="JuceAudioSource.Length" />
    TimeSpan Length { get; }

    /// <inheritdoc cref="JuceAudioSource.Volume" />
    float Volume { get; set; }

    /// <inheritdoc cref="JuceAudioSource.Mute" />
    bool Mute { get; set; }

    /// <summary>The loop region, if any. Left CONCRETE deliberately:
    /// <see cref="JuceLoopRegion" /> owns no handle of its own and is reached
    /// through this source, so faking it separately buys nothing.</summary>
    JuceLoopRegion? Loop { get; set; }

    /// <inheritdoc cref="JuceAudioSource.TempoRatio" />
    float TempoRatio { get; set; }

    /// <inheritdoc cref="JuceAudioSource.PitchSemitones" />
    float PitchSemitones { get; set; }

    /// <inheritdoc cref="JuceAudioSource.Pan" />
    float Pan { get; set; }

    /// <inheritdoc cref="JuceAudioSource.StemGrouped" />
    bool StemGrouped { get; set; }

    /// <inheritdoc cref="JuceAudioSource.Play" />
    void Play();

    /// <inheritdoc cref="JuceAudioSource.Pause" />
    void Pause();

    /// <inheritdoc cref="JuceAudioSource.Stop" />
    void Stop();

    /// <inheritdoc cref="JuceAudioSource.Seek(TimeSpan)" />
    void Seek(TimeSpan position);

    /// <inheritdoc cref="JuceAudioSource.GetPreloadDigest" />
    JucePreloadDigest GetPreloadDigest();

    /// <inheritdoc cref="JuceAudioSource.IsDisposed" />
    bool IsDisposed { get; }
}
