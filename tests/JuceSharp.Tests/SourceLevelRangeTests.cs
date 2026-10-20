using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     <c>JuceAudioSource.Volume</c> and <c>.Pan</c> were clamped on NEITHER side
///     until ABI v33. Two different faults:
///     <list type="bullet">
///         <item>
///             A NEGATIVE volume reached <c>juce::AudioTransportSource::setGain</c>,
///             which accepts it — that is a PHASE INVERSION, not a quieter source.
///             It cancels against anything correlated already in the mix (the same
///             take on another stem, a doubled guitar, the click), so it reads as a
///             hollow mix rather than as a wrong level, and nothing reports it.
///         </item>
///         <item>
///             A pan outside <c>[-1, +1]</c> was clamped deep inside
///             <c>PanningAudioSource</c> but not at the boundary, so the wrapper's
///             CACHED getter could report a position the engine was not playing.
///         </item>
///     </list>
///     <para>
///         Volume deliberately keeps NO upper bound. Boost above unity is a shipped
///         feature — DotCharter's stem faders reach +6 dB (linear 2), its per-stem
///         gain contract is documented 0..4, and a stem gain multiplied by a master
///         gain can legitimately arrive at 16. A ceiling invented at this boundary
///         would silently cap all of it.
///     </para>
///     <para>
///         The ABI exports no volume or pan GETTER, so the native clamp cannot be
///         observed from managed code; the native half is guarded by reading the
///         one ABI translation unit, the same way <see cref="WrapperLevelGateLintTests" />
///         guards the level gate.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class SourceLevelRangeTests : IDisposable
{
    private readonly JuceAudioEngine? _engine;
    private readonly string? _wavPath;

    public SourceLevelRangeTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _wavPath = TinyWav.Create(seconds: 0.5);
    }

    public void Dispose()
    {
        _engine?.Dispose();
        if (_wavPath is not null) TinyWav.TryDelete(_wavPath);
    }

    private JuceAudioSource OpenSource() => _engine!.OpenFile(_wavPath!);

    [NativeAvailableFact]
    public void ANegativeVolume_IsClampedToSilenceAndNotInverted()
    {
        using var src = OpenSource();

        src.Volume = -0.5f;
        Assert.Equal(0f, src.Volume);

        src.Volume = -1f;
        Assert.Equal(0f, src.Volume);
    }

    /// <summary>Boost is a FEATURE. +6 dB is linear 2, and a stem gain folded into
    /// a master gain reaches 16 in the app that drives this wrapper — a ceiling
    /// here would cap a user's fader without telling anyone.</summary>
    [NativeAvailableFact]
    public void AVolumeAboveUnity_IsNotCapped()
    {
        using var src = OpenSource();

        src.Volume = 2f;
        Assert.Equal(2f, src.Volume);

        src.Volume = 16f;
        Assert.Equal(16f, src.Volume);
    }

    [NativeAvailableFact]
    public void PanOutsideTheUnitRange_IsClampedToTheEndpointRatherThanCached()
    {
        using var src = OpenSource();

        src.Pan = 4f;
        Assert.Equal(1f, src.Pan);

        src.Pan = -9f;
        Assert.Equal(-1f, src.Pan);

        // Inside the range nothing moves.
        src.Pan = -0.25f;
        Assert.Equal(-0.25f, src.Pan);
    }

    /// <summary>A NaN is REJECTED, not laundered into 0 / centre — otherwise a
    /// caller that computed one gets a silently muted source and no exception
    /// naming the call that did it.</summary>
    [NativeAvailableFact]
    public void ANonFiniteLevel_IsStillRejected()
    {
        using var src = OpenSource();
        Assert.Throws<JuceAudioException>(() => src.Volume = float.NaN);
        Assert.Throws<JuceAudioException>(() => src.Pan = float.NaN);
        Assert.Throws<JuceAudioException>(() => src.Volume = float.PositiveInfinity);
    }

    // ------------------------------------------------------------------ native

    [Theory]
    [InlineData("dca_source_set_volume", "gain < 0.0f ? 0.0f : gain")]
    [InlineData("dca_source_set_pan", "pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan)")]
    public void TheAbiClampsBeforeItReachesTheTransport(string export, string clamp)
    {
        var body = ExportBody(export);
        Assert.Contains(clamp, body, StringComparison.Ordinal);

        // …and the raw parameter must not ALSO be handed through somewhere else in
        // the same body: `setVolume(gain)` next to the clamp would pass this test
        // on the Contains alone.
        var raw = export.EndsWith("volume", StringComparison.Ordinal) ? "setVolume(gain)" : "setPan(pan)";
        Assert.DoesNotContain(raw, body, StringComparison.Ordinal);
    }

    private static string ExportBody(string export)
    {
        var source = File.ReadAllText(Path.Combine(RepoRoot(),
            "src", "JuceSharp.Native", "src", "dotcharter_audio.cpp"));
        var at = source.IndexOf($"DCA_CALL {export}(", StringComparison.Ordinal);
        Assert.True(at >= 0, $"{export} is no longer defined in the one ABI translation unit");
        var end = source.IndexOf("\n}", at, StringComparison.Ordinal);
        Assert.True(end > at);
        return StripComments(source[at..end]);
    }

    private static string StripComments(string code)
    {
        code = Regex.Replace(code, @"/\*.*?\*/", "", RegexOptions.Singleline);
        return Regex.Replace(code, @"//[^\n]*", "");
    }

    private static string RepoRoot([CallerFilePath] string? thisFile = null)
    {
        var dir = Path.GetDirectoryName(thisFile);
        while (dir != null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        return dir ?? throw new InvalidOperationException("JuceSharp.sln not found above the test source.");
    }
}
