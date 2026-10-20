using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     Concurrency audit #10 (2026-08-29): three wrapper setters wrote a cached
///     managed field and then made a native store with nothing holding the two
///     together. The mixer's solo refresh could enter <c>JuceAudioSource.Mute</c>
///     and read <c>_userVolume == 1.0</c>, the UI slider could write 0.3 and store
///     0.3, and the refresh then stored 1.0 — native at full volume while the
///     getter said 0.3. Same shape on <c>JuceMixerChannel.MutedByUser</c>/<c>Solo</c>
///     (a stale whole-mixer mute set applied over a newer one) and
///     <c>JuceAudioPlugin.Bypass</c> (natively detached while <c>_bypassed</c>
///     read false — that one has no field at all since W4-M2, so its row below
///     pins the ENGINE READ its idempotence check makes: that read and the
///     attach/detach it decides are still one unit, or two toggles interleave
///     into a state neither caller asked for).
///     <para>
///         Native exposes no volume GETTER, so a torn store cannot be observed
///         from a test; the guard is the source: each setter's field write and
///         native call sit inside one <c>lock</c>.
///     </para>
/// </summary>
public sealed class WrapperLevelGateLintTests
{
    [Theory]
    // `clamped`, not `value`, since the ABI v33 range mirror — the field must
    // hold what the ENGINE was given, which is the whole point of the lock.
    [InlineData("JuceAudioSource.cs", "public float Volume", "_userVolume = clamped;", "SourceSetVolume(", "_levelGate")]
    [InlineData("JuceAudioSource.cs", "public bool Mute", "_muted = value;", "SourceSetVolume(", "_levelGate")]
    // Bypass caches nothing since W4-M2: the "field write" here is the ENGINE
    // READ its idempotence check makes.
    [InlineData("JuceAudioPlugin.cs", "public bool Bypass", "NativeMethods.PluginGetInputPlayer(h)",
                "PluginDetachFromInput(", "_bypassGate")]
    public void TheFieldWriteAndTheNativeStore_SitInsideOneLock(string file, string setter, string fieldWrite,
        string nativeCall, string gate)
    {
        var source = StripComments(File.ReadAllText(Path.Combine(WrapperRoot(), file)));
        var start = source.IndexOf(setter, StringComparison.Ordinal);
        Assert.True(start >= 0, $"{file} no longer declares `{setter}`");
        var body = source[start..];
        body = body[..body.IndexOf("\n    }", StringComparison.Ordinal)];

        var lockAt = body.IndexOf($"lock ({gate})", StringComparison.Ordinal);
        var write = body.IndexOf(fieldWrite, StringComparison.Ordinal);
        var native = body.IndexOf(nativeCall, StringComparison.Ordinal);
        Assert.True(lockAt >= 0, $"{setter} no longer takes lock ({gate})");
        Assert.True(write > lockAt && native > lockAt,
            $"{setter}: the field write and the native store must both sit inside lock ({gate})");
    }

    [Fact]
    public void AMixerChannelFlag_IsWrittenUnderTheRefreshGate()
    {
        var source = StripComments(File.ReadAllText(Path.Combine(WrapperRoot(), "JuceMixer.cs")));
        Assert.Contains("set => _owner.SetChannelFlag(() => _mutedByUser = value);", source);
        Assert.Contains("set => _owner.SetChannelFlag(() => _solo = value);", source);

        var refresh = source.IndexOf("internal void RefreshSoloMuting()", StringComparison.Ordinal);
        Assert.True(refresh >= 0);
        var body = source[refresh..];
        Assert.True(body.IndexOf("lock (_soloGate)", StringComparison.Ordinal) <
                    body.IndexOf("ComputeMuteStates(", StringComparison.Ordinal),
            "RefreshSoloMuting must compute AND apply under _soloGate");
    }

    private static string StripComments(string code)
    {
        code = Regex.Replace(code, @"/\*.*?\*/", "", RegexOptions.Singleline);
        return Regex.Replace(code, @"//[^\n]*", "");
    }

    private static string WrapperRoot([CallerFilePath] string? thisFile = null)
    {
        var dir = Path.GetDirectoryName(thisFile);
        while (dir != null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        return Path.Combine(dir ?? throw new InvalidOperationException("Could not locate JuceSharp.sln."),
            "src", "JuceSharp");
    }
}
