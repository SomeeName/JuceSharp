using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The native "Null" AudioIODeviceType (src/JuceSharp.Native/src/
/// NullAudioIODevice.h): a device that accepts any requested config and paces
/// the audio callback from its own thread with silence input. It exists so
/// device-DEPENDENT logic tests (callback pumping, capture ring, mirror
/// switching, bridge round trips) stop opening real WASAPI endpoints.
///
/// Registration is guarded by the process env var DCA_ENABLE_NULL_DEVICE=1,
/// read at engine/device-manager init. Because the variable is process-wide
/// and xunit runs collections in parallel, every test that SETS the guard
/// lives in the "NullDevice" collection, sets it via
/// <see cref="NullDeviceScope"/> in its ctor and clears it on Dispose — so
/// the guard can never leak into the guard-off assertion below, which lives
/// in the same collection.
///
/// <para>
/// <b>That sentence was FALSE for a release and the two things that make it
/// true now are both load-bearing.</b> (1) <c>WrapperErgonomicsTests</c>
/// constructed a <see cref="NullDeviceScope"/> from OUTSIDE this collection —
/// the only one of the suite's scope construction sites that did — so it set
/// the guard while <c>GuardOff_NullType_NeverAppearsInDeviceLists</c> below
/// asserted the "Null" type is ABSENT, and this class's Dispose CLEARED the
/// guard while that class's <c>NullDevice.CreateEngine()</c> was running, which
/// unregisters the type and throws <c>JuceAudioException</c> out of the
/// constructor. It failed under load and passed in isolation, which is what a
/// process-global set from an uncollected class always looks like. It carries
/// <c>[Collection("NullDevice")]</c> now. (2) The definition carries
/// <c>DisableParallelization = true</c>: xunit's doc for that flag is
/// "determines whether tests in this collection runs in parallel with any
/// other tests", so without it this collection was serialized against ITSELF
/// and against nothing else — and a device enumeration in any of the 35
/// engine-creating classes reads the same registration table. The lint that
/// keeps both true is <c>CoverageGaps/TestCollectionLintTests</c>.
/// </para>
/// </summary>
[CollectionDefinition("NullDevice", DisableParallelization = true)]
public sealed class NullDeviceCollection;

/// <summary>Sets DCA_ENABLE_NULL_DEVICE=1 for the lifetime of a test-class
/// instance (xunit creates one per test). Must exist BEFORE the engine is
/// created — the native side reads the variable when the engine starts.</summary>
internal sealed class NullDeviceScope : IDisposable
{
    public NullDeviceScope() => Environment.SetEnvironmentVariable(NullDevice.EnvVar, "1");
    public void Dispose() => Environment.SetEnvironmentVariable(NullDevice.EnvVar, null);
}

internal static class NullDevice
{
    public const string EnvVar     = "DCA_ENABLE_NULL_DEVICE";
    public const string TypeName   = "Null";
    public const string DeviceName = "Null Audio Device";

    /// <summary>An engine on the null device. 48000/480 = a 10 ms block
    /// cadence, comfortably above Windows timer granularity.</summary>
    public static JuceAudioEngine CreateEngine(int sampleRate = 48000,
                                               int bufferFrames = 480,
                                               int inputChannels = 0)
        => new(new JuceEngineConfig
        {
            OutputTypeName = TypeName,
            SampleRate     = sampleRate,
            BufferFrames   = bufferFrames,
            InputChannels  = inputChannels,
        });
}

[Collection("NullDevice")]
public sealed class NullDeviceTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    private readonly ITestOutputHelper _out;

    public NullDeviceTests(ITestOutputHelper output) => _out = output;

    public void Dispose() => _scope.Dispose();

    [NativeAvailableFact]
    public void GuardOff_NullType_NeverAppearsInDeviceLists()
    {
        // This is THE invariant that lets the null device ship in the release
        // DLL: with the guard off it must be unreachable. Clear the guard for
        // this one test (the collection serializes us against every setter).
        Environment.SetEnvironmentVariable(NullDevice.EnvVar, null);

        JuceAudioEngine engine;
        try { engine = new JuceAudioEngine(); }
        catch (JuceAudioException) { Assert.Skip("no audio device on this host"); return; }

        using (engine)
        {
            Assert.DoesNotContain(engine.EnumerateOutputDevicesLive(),
                d => d.TypeName == NullDevice.TypeName || d.Name == NullDevice.DeviceName);
            Assert.DoesNotContain(engine.EnumerateInputDevicesLive(),
                d => d.TypeName == NullDevice.TypeName || d.Name == NullDevice.DeviceName);
        }
        Assert.DoesNotContain(JuceAudioEngine.EnumerateOutputDevices(),
            d => d.TypeName == NullDevice.TypeName || d.Name == NullDevice.DeviceName);
    }

    [NativeAvailableFact]
    public void GuardOn_EngineSelectsNullDevice_AndHonoursRequestedConfig()
    {
        using var engine = NullDevice.CreateEngine(sampleRate: 48000, bufferFrames: 480,
                                                   inputChannels: 2);
        // The null device accepts the exact requested rate/buffer — no
        // driver negotiation.
        Assert.Equal(48000, engine.SampleRate);
        Assert.Equal(480, engine.BufferFrames);

        // Selection is visible: the live list (this engine's own manager)
        // carries the null device...
        Assert.Contains(engine.EnumerateOutputDevicesLive(),
            d => d.TypeName == NullDevice.TypeName && d.Name == NullDevice.DeviceName);
        // ...but the STATIC list (a fresh manager that never registers the
        // type) still does not — the guard is per-manager-init, not global.
        Assert.DoesNotContain(JuceAudioEngine.EnumerateOutputDevices(),
            d => d.TypeName == NullDevice.TypeName);
    }

    [NativeAvailableFact]
    public void GuardOn_Reconfigure_AppliesNewSampleRateAndBuffer()
    {
        using (var a = NullDevice.CreateEngine(sampleRate: 44100, bufferFrames: 512))
        {
            Assert.Equal(44100, a.SampleRate);
            Assert.Equal(512, a.BufferFrames);
        }
        // Close + reopen at a different config: the device must accept the
        // new shape, not replay the old one.
        using (var b = NullDevice.CreateEngine(sampleRate: 96000, bufferFrames: 256))
        {
            Assert.Equal(96000, b.SampleRate);
            Assert.Equal(256, b.BufferFrames);
        }
    }

    [NativeAvailableFact]
    public void GuardOn_UnknownDeviceId_FallsBackToTheNullDevice()
    {
        // The engine's device-walk fallback (start()'s "walk the rest of the
        // host's devices" path) must land on the null device when the pinned
        // name does not exist within the Null type.
        using var engine = new JuceAudioEngine(new JuceEngineConfig
        {
            OutputTypeName = NullDevice.TypeName,
            OutputDeviceId = "No Such Device 059AF7",
        });
        Assert.True(engine.SampleRate > 0);
        Assert.True(engine.BufferFrames > 0);
    }

    /// <summary>
    /// <b>The realised rate is LOGGED; what is asserted is structure.</b>
    /// This ended in <c>Assert.InRange(rate, 48000 * 0.7, 48000 * 1.3)</c> after
    /// a <c>Thread.Sleep(500)</c>, and a ±30 % band on a pacing thread's
    /// achieved rate is a performance budget wearing a correctness assertion's
    /// clothes: a saturated box drops below 33.6 kHz and the test fails with
    /// nothing wrong.
    ///
    /// <para>
    /// The half of that band that CANNOT flake under load is the upper one. The
    /// pacing thread schedules on an absolute <c>steady_clock</c> timeline, so
    /// load can only make it LATE — never early — which means a rate ABOVE the
    /// configured one is a real defect (a double-render, a clock pair that does
    /// not describe the same instant) and a rate below it is a busy machine.
    /// The lower bound is replaced by two facts that hold at any speed: the
    /// clock advances, and it advances in WHOLE BLOCKS, because the render
    /// clock is published once per callback.
    /// </para>
    /// </summary>
    [NativeAvailableFact]
    public void GuardOn_CallbackCadence_TracksTheConfiguredRate()
    {
        using var engine = NullDevice.CreateEngine(sampleRate: 48000, bufferFrames: 480);

        // Wait for the first rendered block, then measure frames/wall-time.
        long f0 = 0, t0 = 0;
        var deadline = Environment.TickCount64 + 2000;
        while (!engine.TryGetRenderClock(out f0, out t0))
        {
            Assert.True(Environment.TickCount64 < deadline, "null device never rendered a block");
            Thread.Sleep(10);
        }

        Thread.Sleep(500);
        Assert.True(engine.TryGetRenderClock(out var f1, out var t1));
        Assert.True(f1 > f0, "render clock did not advance");
        Assert.True(t1 > t0, "sync ticks did not advance");

        // Structure: the render clock is published once per callback, so it can
        // only ever have moved by a whole number of blocks.
        Assert.Equal(0, (f1 - f0) % engine.BufferFrames);

        var elapsedSec = (t1 - t0) / (double)System.Diagnostics.Stopwatch.Frequency;
        var rate = (f1 - f0) / elapsedSec;

        // Load can only make the pacing thread LATE. A rate above the configured
        // one is a defect; a rate below it is a busy box, so it is logged.
        Assert.True(rate <= 48000 * 1.3,
            $"the null device rendered FASTER than its configured rate: {rate:F0} Hz against 48000 Hz.");
        _out.WriteLine($"null-device cadence: {rate:F0} Hz over {elapsedSec * 1000:F1} ms " +
                       $"({f1 - f0} frames, {(f1 - f0) / engine.BufferFrames} blocks of {engine.BufferFrames}).");
    }

    [NativeAvailableFact]
    public void GuardOn_SilenceInput_FeedsTheCaptureRingAtTheDeviceRate()
    {
        // The null device's input path is real (zeroed buffers through the
        // whole input chain), so the capture ring advances exactly like it
        // would on hardware — just deterministically.
        using var engine = NullDevice.CreateEngine(sampleRate: 48000, bufferFrames: 480,
                                                   inputChannels: 1);
        ulong s0 = 0; long t0 = 0;
        var deadline = Environment.TickCount64 + 2000;
        while (!engine.TryGetInputCaptureSync(out s0, out t0))
        {
            Assert.True(Environment.TickCount64 < deadline, "capture ring never advanced");
            Thread.Sleep(10);
        }

        var buf = new float[4096];
        Assert.Equal(buf.Length, engine.ReadInputCapture(buf));
        Assert.All(buf, v => Assert.Equal(0f, v)); // silence in, silence captured
    }
}
