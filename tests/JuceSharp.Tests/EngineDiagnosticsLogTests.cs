using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// <c>AudioEngine</c> produces exactly two diagnostics of its own, and both
/// went to <c>stderr</c> — which no managed host reads. One of them is the most
/// useful message in that file: when a device fails to open it dumps every
/// device name the driver ACTUALLY offers, which is the difference between "my
/// saved device name is stale" and "the driver scan returned nothing". The
/// other says the streaming mirror (OBS / Discord) could not be reopened after
/// a device rebuild, i.e. the user's stream is now silent.
/// <para>
/// The engine cannot reach the ABI translation unit's <c>emitLog</c>, so it now
/// goes through <c>AudioEngine::setLogSink</c>, which <c>dotcharter_audio.cpp</c>
/// points at <c>emitLog</c> at static-init time. Unset — which is how the
/// native test executables run — it is a no-op.
/// </para>
/// </summary>
[Collection("NullDevice")]
public sealed class EngineDiagnosticsLogTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    private readonly JuceLogLevel _originalLevel = JuceLog.MinimumLevel;

    public void Dispose()
    {
        JuceLog.MinimumLevel = _originalLevel;
        _scope.Dispose();
    }

    /// <summary>
    /// A native message a test can provoke on the Null device: a restart of a
    /// stopped engine is refused (#27), and the native reason must reach the
    /// managed log rather than a stderr nobody is reading. (This used to provoke
    /// the mirror re-open of a stop/start rebuild; that rebuild is refused now,
    /// and <c>restoreMirrorAfterStart</c> only runs on a <c>cfg == NULL</c>
    /// engine's first <c>dca_engine_start</c>, which opens a real device.)
    /// </summary>
    [NativeAvailableFact]
    public void ARefusedRestart_ReachesTheManagedLog()
    {
        JuceLog.MinimumLevel = JuceLogLevel.Trace;
        var seen = new List<(JuceLogLevel Level, string Message)>();
        void Handler(JuceLogLevel l, string m) { lock (seen) seen.Add((l, m)); }
        JuceLog.MessageReceived += Handler;
        try
        {
            using var engine = NullDevice.CreateEngine(sampleRate: 48000, bufferFrames: 480);
            engine.Stop();

            lock (seen) seen.Clear();
            // The native export directly: the managed Start() refuses first.
            Assert.Equal(JuceSharp.Interop.NativeMethods.DcaStatus.Device,
                         JuceSharp.Interop.NativeMethods.EngineStart(engine.NativeHandle));

            List<(JuceLogLevel Level, string Message)> snapshot;
            lock (seen) snapshot = [.. seen];

            Assert.Contains(snapshot,
                r => r.Message.Contains("cannot be restarted", StringComparison.Ordinal));
        }
        finally
        {
            JuceLog.MessageReceived -= Handler;
        }
    }

    /// <summary>
    /// The device-dump half cannot be provoked from here — it needs an open
    /// that fails after the whole fallback walk, and the null device accepts
    /// every configuration by design — so it gets a lint. Together with the
    /// test above (which proves the sink is really installed and really
    /// reaches JuceLog) that covers both messages.
    /// </summary>
    [Fact]
    public void TheEngineWritesNoDiagnosticsToStderr()
    {
        var cpp = StripComments(File.ReadAllText(FindRepoFile(
            "src/JuceSharp.Native/src/AudioEngine.cpp")));

        Assert.DoesNotContain("stderr", cpp, StringComparison.Ordinal);
        Assert.Contains("engineLog(", cpp, StringComparison.Ordinal);

        // …and the ABI translation unit really installs the sink, or engineLog
        // is a no-op and the assertion above means nothing.
        var abi = StripComments(File.ReadAllText(FindRepoFile(
            "src/JuceSharp.Native/src/dotcharter_audio.cpp")));
        Assert.Contains("AudioEngine::setLogSink", abi, StringComparison.Ordinal);
    }

    private static string StripComments(string src)
    {
        src = Regex.Replace(src, @"/\*.*?\*/", " ", RegexOptions.Singleline);
        return Regex.Replace(src, @"//[^\n]*", " ");
    }

    private static string FindRepoFile(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
