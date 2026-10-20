using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// JuceSharp #27: a stopped engine cannot be restarted. <c>stop()</c> detaches every
/// source from the mixer for good, so a restart used to open a live device with
/// silence on it and report success. Both the native export and the managed
/// wrapper now refuse, each with the reason.
/// </summary>
[Collection("NativeEngine")]
public sealed class EngineRestartRefusalTests
{
    /// <summary>An engine on the native Null device: never the machine's real output.</summary>
    private static JuceAudioEngine NullEngine() => new(new JuceEngineConfig
    {
        EnableNullDevice = true, OutputTypeName = "Null", SampleRate = 48_000, BufferFrames = 480,
    });

    [NativeAvailableFact]
    public void TheNativeStart_RefusesAStoppedEngine()
    {
        using var engine = NullEngine();
        var h = engine.NativeHandle;
        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineStop(h));
        Assert.Equal(NativeMethods.DcaStatus.Device, NativeMethods.EngineStart(h));
        // ...and keeps refusing: a refusal must not leave it half-started.
        Assert.Equal(NativeMethods.DcaStatus.Device, NativeMethods.EngineStart(h));
        Assert.Equal(0, engine.SampleRate);
    }

    [NativeAvailableFact]
    public void TheManagedStart_ThrowsWithTheRule_AfterStop()
    {
        using var engine = NullEngine();
        engine.Stop();
        var ex = Assert.Throws<InvalidOperationException>(engine.Start);
        Assert.Contains("cannot be restarted", ex.Message, StringComparison.Ordinal);
        Assert.Contains("create a new one", ex.Message, StringComparison.Ordinal);
    }

    /// <summary>The refusal is about a STOP, not about starting twice: Start() on a
    /// running engine stays an idempotent no-op, and Stop() twice stays harmless.</summary>
    [NativeAvailableFact]
    public void StartOnARunningEngine_AndStopTwice_StayHarmless()
    {
        using var engine = NullEngine();
        engine.Start();
        Assert.Equal(48_000, engine.SampleRate);
        engine.Stop();
        engine.Stop();
    }
}
