using Microsoft.Extensions.Logging;

namespace JuceSharp;

/// <summary>
/// Minimal <see cref="ILogger"/> implementation that writes JuceSharp
/// engine messages to <see cref="Console.Out"/> (Info and below) and
/// <see cref="Console.Error"/> (Warn / Error). Used automatically when
/// no logger is supplied to <see cref="JuceAudioEngine"/> and
/// <see cref="JuceEngineConfig.EnableConsoleLog"/> is left at its
/// default <c>true</c>.
/// </summary>
public sealed class JuceConsoleLogger : ILogger
{
    /// <summary>Singleton instance — the logger is stateless.</summary>
    public static readonly JuceConsoleLogger Instance = new();

    private JuceConsoleLogger() { }

    /// <summary>Returns a no-op scope: this logger prints flat lines and carries no
    /// scope state.</summary>
    public IDisposable BeginScope<TState>(TState state) where TState : notnull => NullScope.Singleton;

    /// <summary>Always <c>true</c> — this logger prints every level, and filtering is
    /// left to <see cref="JuceLog.MinimumLevel"/> on the native side.</summary>
    public bool IsEnabled(LogLevel logLevel) => logLevel >= LogLevel.Trace;

    /// <summary>Format one entry as <c>JUCE  LEVEL  message</c> and write it to
    /// <see cref="Console.Error"/> for Warning and above, <see cref="Console.Out"/>
    /// otherwise; an exception is appended on its own lines.</summary>
    public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
        Exception? exception, Func<TState, Exception?, string> formatter)
    {
        if (!IsEnabled(logLevel)) return;
        var msg = formatter(state, exception);
        var tag = logLevel switch
        {
            LogLevel.Trace       => "TRACE",
            LogLevel.Debug       => "DEBUG",
            LogLevel.Information => "INFO ",
            LogLevel.Warning     => "WARN ",
            LogLevel.Error       => "ERROR",
            LogLevel.Critical    => "FATAL",
            _                    => "LOG  ",
        };
        var line = $"JUCE  {tag}  {msg}";
        if (exception is not null) line += Environment.NewLine + exception;

        if (logLevel >= LogLevel.Warning)
            Console.Error.WriteLine(line);
        else
            Console.Out.WriteLine(line);
    }

    private sealed class NullScope : IDisposable
    {
        public static readonly NullScope Singleton = new();
        public void Dispose() { }
    }
}
