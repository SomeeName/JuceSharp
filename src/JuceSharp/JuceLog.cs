using System.Diagnostics;

namespace JuceSharp;

/// <summary>
/// Severity of a message emitted by the native engine. Mirrors the
/// native <c>DcaLogLevel</c> enum exactly.
/// </summary>
public enum JuceLogLevel
{
    /// <summary>Per-callback and per-block detail; far too chatty to leave on in a shipping app.</summary>
    Trace = 0,
    /// <summary>Developer detail about a state change the user did not ask for.</summary>
    Debug = 1,
    /// <summary>Normal lifecycle progress: a device opened, a plugin loaded, a transport started.</summary>
    Info  = 2,
    /// <summary>Something degraded but the engine carried on — a fallback device, a dropout, a clamped request.</summary>
    Warn  = 3,
    /// <summary>An operation failed; the caller's request did not happen.</summary>
    Error = 4,
}

/// <summary>
/// Stable, public mirror of the internal native <c>DcaStatus</c> enum.
/// Returned by <see cref="JuceAudioException.Status"/>.
/// </summary>
public enum JuceAudioStatus
{
    /// <summary>The call succeeded; no exception is raised for this status.</summary>
    Ok             = 0,
    /// <summary>An argument was out of range, null, or named something the engine does not have —
    /// including a node id whose seat is past <c>DCA_MAX_PLAYERS</c>, which is rejected and never clamped.</summary>
    InvalidArg     = 1,
    /// <summary>The engine or the object addressed has not been started or configured yet.</summary>
    NotInitialized = 2,
    /// <summary>The audio device could not be opened, reconfigured, or was lost mid-session.</summary>
    Device         = 3,
    /// <summary>The audio file could not be decoded — unknown container, unsupported codec, or truncated data.</summary>
    Decode         = 4,
    /// <summary>A VST3 plugin failed to scan, load, instantiate, or its sandbox child died.</summary>
    Plugin         = 5,
    /// <summary>The caller asked to stop (ABI v36) — a stem separation whose progress callback
    /// returned "cancel". NOT a failure: the wrapper raises
    /// <see cref="System.OperationCanceledException"/> for it rather than
    /// <see cref="JuceAudioException"/>, because a user's Cancel must not read to a consumer as a
    /// reason to fall through to the next backend.</summary>
    Cancelled      = 6,
    /// <summary>An unexpected native failure that maps to none of the above; treat as a bug.</summary>
    Internal       = 99,
}

/// <summary>
/// Global, process-wide hook for native engine log messages. Subscribe
/// to <see cref="MessageReceived"/> to capture every message regardless
/// of which <c>ILogger</c> a particular <see cref="JuceAudioEngine"/> is
/// using. Subscribers must be fast and non-throwing — exceptions are
/// swallowed and reported via <see cref="System.Diagnostics.Debug"/>.
/// </summary>
public static class JuceLog
{
    /// <summary>Raised for every native log line. Handlers run on the
    /// thread that produced the log message (often the JUCE message
    /// thread) and must not block.</summary>
    public static event Action<JuceLogLevel, string>? MessageReceived;

    /// <summary>Minimum log level that will be forwarded to managed
    /// sinks. Defaults to <see cref="JuceLogLevel.Trace"/> (everything).
    /// Can be overridden by setting the <c>JUCESHARP_LOG_LEVEL</c>
    /// environment variable to <c>trace|debug|info|warn|error</c>.</summary>
    public static JuceLogLevel MinimumLevel { get; set; } = ReadEnvLevel();

    internal static void Emit(JuceLogLevel level, string message)
    {
        if (level < MinimumLevel) return;
        var handlers = MessageReceived;
        if (handlers is null) return;
        // Multicast invocation aborts at the first throwing subscriber, which
        // would silently drop log delivery to all subsequent subscribers.
        // Iterate the invocation list manually so every handler runs.
        foreach (var d in handlers.GetInvocationList())
        {
            try { ((Action<JuceLogLevel, string>)d)(level, message); }
            catch (Exception ex)
            {
                Debug.WriteLine($"[JuceLog] subscriber threw: {ex}");
            }
        }
    }

    private static JuceLogLevel ReadEnvLevel()
    {
        var env = Environment.GetEnvironmentVariable("JUCESHARP_LOG_LEVEL");
        return env?.Trim().ToLowerInvariant() switch
        {
            "trace" => JuceLogLevel.Trace,
            "debug" => JuceLogLevel.Debug,
            "info"  => JuceLogLevel.Info,
            "warn" or "warning" => JuceLogLevel.Warn,
            "error" => JuceLogLevel.Error,
            _       => JuceLogLevel.Trace,
        };
    }
}
