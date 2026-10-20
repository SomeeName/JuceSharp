using Microsoft.Extensions.Logging;

namespace JuceSharp.Tests;

internal sealed class CapturingLogger : ILogger<JuceAudioEngine>
{
    public List<(LogLevel Level, string Message)> Records { get; } = [];

    public IDisposable BeginScope<TState>(TState state) where TState : notnull => NullScope.Instance;
    public bool IsEnabled(LogLevel level) => true;

    public void Log<TState>(LogLevel level, EventId id, TState state, Exception? ex,
        Func<TState, Exception?, string> formatter)
    {
        Records.Add((level, formatter(state, ex)));
    }

    private sealed class NullScope : IDisposable
    {
        public static readonly NullScope Instance = new();
        public void Dispose() { }
    }
}
