#nullable disable
namespace JuceSharp.Tests;

/// <summary>
/// A test double over <see cref="IJuceAudioSource" />: a transport with no
/// engine behind it. Small enough to write by hand, which is the argument for
/// the interface — the concrete <see cref="JuceAudioSource" /> is sealed with an
/// internal ctor because it owns a native handle, and nobody should be able to
/// make one without one.
/// </summary>
public sealed class FakeJuceAudioSource : IJuceAudioSource
{
    private readonly List<string> _calls = new();

    public IReadOnlyList<string> Calls => _calls;

    public bool Playing { get; private set; }

    public bool Disposed { get; private set; }

    /// <summary>
    ///     The contract member added in 0.9 so an async factory's caller can
    ///     tell a live child from a corpse. The fake already tracked exactly
    ///     this under its own name; this forwards rather than duplicating, so
    ///     the two can never disagree.
    /// </summary>
    public bool IsDisposed => Disposed;

    public TimeSpan Position { get; set; }

    public TimeSpan AudiblePosition => Position;

    public TimeSpan Length { get; set; } = TimeSpan.FromMinutes(3);

    public float Volume { get; set; } = 1f;

    public bool Mute { get; set; }

    public JuceLoopRegion Loop { get; set; }

    public float TempoRatio { get; set; } = 1f;

    public float PitchSemitones { get; set; }

    public float Pan { get; set; }

    public bool StemGrouped { get; set; }

    public void Play()
    {
        _calls.Add(nameof(Play));
        Playing = true;
    }

    public void Pause()
    {
        _calls.Add(nameof(Pause));
        Playing = false;
    }

    public void Stop()
    {
        _calls.Add(nameof(Stop));
        Playing = false;
        Position = TimeSpan.Zero;
    }

    /// <summary>Settable: a fake holds no samples, so its "digest" is whatever the test says.</summary>
    public JucePreloadDigest PreloadDigest { get; set; }

    public JucePreloadDigest GetPreloadDigest() => PreloadDigest;

    public void Seek(TimeSpan position)
    {
        _calls.Add(nameof(Seek));
        Position = position;
    }

    public void Dispose()
    {
        _calls.Add(nameof(Dispose));
        Disposed = true;
    }
}
