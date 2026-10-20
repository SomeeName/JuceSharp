using Microsoft.Extensions.Logging;
using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceConsoleLoggerTests
{
    [Fact]
    public void Singleton_IsSameInstance()
        => Assert.Same(JuceConsoleLogger.Instance, JuceConsoleLogger.Instance);

    [Theory]
    [InlineData(LogLevel.Trace, true)]
    [InlineData(LogLevel.Debug, true)]
    [InlineData(LogLevel.Information, true)]
    [InlineData(LogLevel.Warning, true)]
    [InlineData(LogLevel.Error, true)]
    public void IsEnabled_AcceptsAllLevels(LogLevel level, bool expected)
        => Assert.Equal(expected, JuceConsoleLogger.Instance.IsEnabled(level));

    [Fact]
    public void Log_WritesInfoToStdout()
    {
        var captured = new StringWriter();
        var prev = Console.Out;
        Console.SetOut(captured);
        try
        {
            JuceConsoleLogger.Instance.Log(
                LogLevel.Information, default, "hello world", null,
                (s, _) => s);
        }
        finally { Console.SetOut(prev); }

        var text = captured.ToString();
        Assert.Contains("INFO", text);
        Assert.Contains("hello world", text);
    }

    [Fact]
    public void Log_WritesWarningToStderr()
    {
        var captured = new StringWriter();
        var prev = Console.Error;
        Console.SetError(captured);
        try
        {
            JuceConsoleLogger.Instance.Log(
                LogLevel.Warning, default, "uh oh", null,
                (s, _) => s);
        }
        finally { Console.SetError(prev); }

        var text = captured.ToString();
        Assert.Contains("WARN", text);
        Assert.Contains("uh oh", text);
    }

    [Fact]
    public void Log_TraceLevel_WritesToStdout()
    {
        // p2-31 follow-up: Trace messages must be emitted, not dropped.
        var captured = new StringWriter();
        var prev = Console.Out;
        Console.SetOut(captured);
        try
        {
            JuceConsoleLogger.Instance.Log(
                LogLevel.Trace, default, "trace ping", null,
                (s, _) => s);
        }
        finally { Console.SetOut(prev); }

        var text = captured.ToString();
        Assert.Contains("TRACE", text);
        Assert.Contains("trace ping", text);
    }

    [Fact]
    public void BeginScope_ReturnsDisposable()
    {
        using var scope = JuceConsoleLogger.Instance.BeginScope("ctx");
        Assert.NotNull(scope);
    }
}
