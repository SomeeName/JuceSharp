using Xunit;

namespace JuceSharp.Tests;

public sealed class JuceLogTests : IDisposable
{
    private readonly JuceLogLevel _originalLevel = JuceLog.MinimumLevel;

    public void Dispose() => JuceLog.MinimumLevel = _originalLevel;

    [Fact]
    public void Emit_InvokesSubscribers()
    {
        var got = new List<(JuceLogLevel, string)>();
        void Handler(JuceLogLevel l, string m) => got.Add((l, m));
        JuceLog.MessageReceived += Handler;
        try
        {
            JuceLog.MinimumLevel = JuceLogLevel.Trace;
            JuceLog.Emit(JuceLogLevel.Info, "hello");
            JuceLog.Emit(JuceLogLevel.Error, "bad");
        }
        finally { JuceLog.MessageReceived -= Handler; }

        Assert.Equal(2, got.Count);
        Assert.Equal(JuceLogLevel.Info, got[0].Item1);
        Assert.Equal("hello", got[0].Item2);
        Assert.Equal(JuceLogLevel.Error, got[1].Item1);
    }

    [Fact]
    public void Emit_FiltersBelowMinimumLevel()
    {
        var count = 0;
        void Handler(JuceLogLevel _, string _2) => Interlocked.Increment(ref count);
        JuceLog.MessageReceived += Handler;
        try
        {
            JuceLog.MinimumLevel = JuceLogLevel.Warn;
            JuceLog.Emit(JuceLogLevel.Trace, "t");
            JuceLog.Emit(JuceLogLevel.Debug, "d");
            JuceLog.Emit(JuceLogLevel.Info, "i");
            JuceLog.Emit(JuceLogLevel.Warn, "w");
            JuceLog.Emit(JuceLogLevel.Error, "e");
        }
        finally { JuceLog.MessageReceived -= Handler; }

        Assert.Equal(2, count);
    }

    [Fact]
    public void Emit_SwallowsSubscriberExceptions()
    {
        static void Bad(JuceLogLevel _, string _2) => throw new InvalidOperationException("boom");
        JuceLog.MessageReceived += Bad;
        try
        {
            JuceLog.MinimumLevel = JuceLogLevel.Trace;
            JuceLog.Emit(JuceLogLevel.Info, "x");   // must not throw
        }
        finally { JuceLog.MessageReceived -= Bad; }
    }

    [Fact]
    public void Emit_WithNoSubscribers_IsNoOp()
    {
        JuceLog.MinimumLevel = JuceLogLevel.Trace;
        JuceLog.Emit(JuceLogLevel.Info, "silent"); // must not throw
    }
}
