using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Streaming-mirror lifecycle coverage (2026-07-16 audit test-gap list: only
/// the null/close paths were tested). Repeated open -> reopen -> close cycles
/// run WHILE the primary engine's callback is live-pushing blocks, which is
/// exactly the window audit finding #7 closed (open()'s FIFO/ring reset
/// racing an in-flight pushBlock). The pure resampler/clock math is covered
/// by the standalone MirrorClockControllerTests.cpp harness.
///
/// The stress loop runs on the NULL device (both the primary engine and the
/// mirror endpoint): the race under test is FIFO-reset vs pushBlock, which
/// only needs two live callbacks — which physical driver pumps them is
/// incidental, and real WASAPI endpoints made this the flakiest test in the
/// suite (busy/exclusive endpoints, missing devices). One real-WASAPI
/// open/close smoke below keeps the "actually opens a shared-mode endpoint"
/// coverage.
/// </summary>
[Collection("NullDevice")]
public sealed class MirrorOutputSwitchTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    [NativeAvailableFact]
    public void RepeatedMirrorSwitches_WhileEngineRuns_AreCrashFree()
    {
        using var engine = NullDevice.CreateEngine();
        Assert.True(engine.SampleRate > 0);

        Assert.True(engine.SetMirrorDevice(NullDevice.DeviceName),
            "mirror open on the null device failed");

        try
        {
            // Every re-open runs close() + the FIFO/ring reset while the
            // primary callback keeps calling pushBlock — the audit-#7 window.
            for (var cycle = 0; cycle < 5; cycle++)
            {
                Assert.True(engine.SetMirrorDevice(NullDevice.DeviceName),
                    $"re-open failed on cycle {cycle}");
                Thread.Sleep(80); // let the mirror device render a few blocks

                if (cycle % 2 == 1)
                {
                    Assert.True(engine.SetMirrorDevice(null)); // close
                    Thread.Sleep(40);
                }
            }
        }
        finally
        {
            engine.SetMirrorDevice(null);
        }
    }
}

/// <summary>
/// Real-device smoke for the mirror family: production's job IS opening a
/// real shared-mode WASAPI endpoint, so one open/close cycle stays on real
/// hardware (skipping, not failing, when no endpoint will open).
/// </summary>
[Collection("NativeEngine")]
public sealed class MirrorOutputRealDeviceSmokeTests
{
    [NativeAvailableFact]
    public void MirrorOpensAndCloses_OnARealWasapiEndpoint()
    {
        using var engine = new JuceAudioEngine();
        if (engine.SampleRate <= 0)
            Assert.Skip("no audio device on this host");

        var devices = JuceAudioEngine.EnumerateOutputDevices();
        var wasapi = devices.FirstOrDefault(d => d.Driver == AudioDriver.Wasapi)
                  ?? devices.FirstOrDefault(d => d.IsDefault);
        if (wasapi is null)
            Assert.Skip("no WASAPI output endpoint to mirror to");

        if (!engine.SetMirrorDevice(wasapi.Id))
            Assert.Skip($"mirror device '{wasapi.Name}' would not open (busy/exclusive?)");

        try
        {
            Thread.Sleep(80); // let the mirror device render a few blocks
        }
        finally
        {
            Assert.True(engine.SetMirrorDevice(null));
        }
    }
}
