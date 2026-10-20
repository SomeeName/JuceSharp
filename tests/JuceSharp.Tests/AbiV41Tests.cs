using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading;
using JuceSharp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v41 (DotCharter #85): the headless Null device is requested PER ENGINE through
/// <c>dca_engine_create2</c>'s flags instead of only through the process-wide
/// <c>DCA_ENABLE_NULL_DEVICE</c> environment variable. The variable is shared by every
/// engine in the process, so a host that set it around ONE engine's construction
/// registered the Null type in a live engine another thread built meanwhile.
/// </summary>
public sealed class AbiV41Tests
{
    private static string Header()
        => File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));

    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var m = Regex.Match(Header(), @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 41u, "dca_engine_create2 predates the current version");
    }

    [Fact]
    public void TheV41Export_IsInTheHeaderAndTheDll()
    {
        Assert.Contains("DCA_CALL dca_engine_create2(", Header(), StringComparison.Ordinal);
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }
        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(NativeLibrary.TryGetExport(lib, "dca_engine_create2", out var addr),
            "dca_engine_create2 is not exported by the built juce_sharp.dll.");
        Assert.NotEqual(nint.Zero, addr);
    }

    /// <summary>The managed flag constants are the header's, read off the header.</summary>
    [Fact]
    public void TheCreateFlags_MatchTheHeader()
    {
        var header = Header();
        static uint Bit(string header, string name)
        {
            var m = Regex.Match(header, @"#define\s+" + name + @"\s+\(1u\s*<<\s*(\d+)\)");
            Assert.True(m.Success, $"{name} is not a (1u << n) define in the header");
            return 1u << int.Parse(m.Groups[1].Value);
        }

        Assert.Equal(NativeMethods.EngineCreateNullDevice, Bit(header, "DCA_ENGINE_CREATE_NULL_DEVICE"));
        Assert.Equal(NativeMethods.EngineCreateNullDeviceManual, Bit(header, "DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL"));
        Assert.Equal(NativeMethods.EngineCreateNullDevice | NativeMethods.EngineCreateNullDeviceManual,
                     NativeMethods.EngineCreateKnownFlags);
        Assert.Matches(@"#define\s+DCA_ENGINE_CREATE_KNOWN_FLAGS\s+\(DCA_ENGINE_CREATE_NULL_DEVICE\s*\|\s*DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL\)", header);
    }

    /// <summary>The config → flags mapping: manual implies enable; default is 0, so an
    /// ordinary engine asks for nothing.</summary>
    [Fact]
    public void JuceEngineConfig_MapsToTheCreateFlags()
    {
        Assert.Equal(0u, new JuceEngineConfig().CreateFlags());
        Assert.Equal(NativeMethods.EngineCreateNullDevice,
                     new JuceEngineConfig { EnableNullDevice = true }.CreateFlags());
        Assert.Equal(NativeMethods.EngineCreateNullDevice | NativeMethods.EngineCreateNullDeviceManual,
                     new JuceEngineConfig { NullDeviceManualPump = true }.CreateFlags());
    }

    /// <summary>The constructor really uses create2 — a constructor still calling the
    /// v1 export would compile, pass every other test, and silently drop the flag.</summary>
    [Fact]
    public void TheEngineConstructor_CallsCreate2WithTheConfigFlags()
    {
        var src = File.ReadAllText(AbiV37Tests.FindRepoFile("src/JuceSharp/JuceAudioEngine.cs"));
        Assert.Contains("NativeMethods.EngineCreate2(in cfg, effectiveConfig.CreateFlags(), out handle)", src, StringComparison.Ordinal);
        Assert.DoesNotContain("NativeMethods.EngineCreate(in cfg", src, StringComparison.Ordinal);
    }

    [NativeAvailableFact]
    public void AnUnknownFlagBit_IsRejected_AndNoHandleEscapes()
    {
        var cfg = new JuceEngineConfig().ToNative();
        var status = NativeMethods.EngineCreate2(in cfg, 1u << 7, out var handle);
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, status);
        Assert.Equal(nint.Zero, handle);
    }
}

/// <summary>
/// The behaviour half. In the <c>NullDevice</c> collection because the tests CLEAR (and
/// one sets) the process env var to prove the flag works "regardless of the env var" —
/// the env path is what that collection serialises.
/// </summary>
[Collection("NullDevice")]
public sealed class AbiV41NullDeviceFlagTests : IDisposable
{
    public AbiV41NullDeviceFlagTests()
    {
        Environment.SetEnvironmentVariable(NullDevice.EnvVar, null);
        Environment.SetEnvironmentVariable("DCA_NULL_DEVICE_MANUAL", null);
    }

    public void Dispose()
    {
        Environment.SetEnvironmentVariable(NullDevice.EnvVar, null);
        Environment.SetEnvironmentVariable("DCA_NULL_DEVICE_MANUAL", null);
    }

    private static JuceAudioEngine NullEngine(bool flag, bool manual = false) => new(new JuceEngineConfig
    {
        OutputTypeName = NullDevice.TypeName,
        SampleRate = 48000,
        BufferFrames = 480,
        EnableNullDevice = flag,
        NullDeviceManualPump = manual,
        EnableConsoleLog = false,
    });

    /// <summary>With the env var CLEARED, the flag alone registers and opens the Null
    /// device, and the type is in that engine's own device list.</summary>
    [NativeAvailableFact]
    public void TheFlag_RegistersTheNullType_WithTheEnvVarCleared()
    {
        using var engine = NullEngine(flag: true);
        Assert.Contains(engine.EnumerateOutputDevicesLive(),
            d => d.TypeName == NullDevice.TypeName && d.Name == NullDevice.DeviceName);
    }

    /// <summary>No flag, no env var: asking for the Null type fails — it does not exist
    /// for that engine.</summary>
    [NativeAvailableFact]
    public void WithoutTheFlag_AndWithoutTheEnvVar_TheNullTypeDoesNotExist()
    {
        Assert.Throws<JuceAudioException>(() => NullEngine(flag: false).Dispose());
    }

    /// <summary>Backward compatibility: the env var alone still works (OR, not replace).</summary>
    [NativeAvailableFact]
    public void TheEnvVar_StillEnablesIt_WithoutTheFlag()
    {
        Environment.SetEnvironmentVariable(NullDevice.EnvVar, "1");
        using var engine = NullEngine(flag: false);
        Assert.Contains(engine.EnumerateOutputDevicesLive(), d => d.TypeName == NullDevice.TypeName);
    }

    /// <summary>
    /// THE #85 invariant at the wrapper level: while an engine built WITH the flag is
    /// alive (device open), an engine built without it on another thread does not get
    /// the type. Under the env-var mechanism this is exactly what leaked.
    /// </summary>
    [NativeAvailableFact]
    public void AFlaggedEngine_DoesNotLeakTheType_IntoAConcurrentEngine()
    {
        using var offline = NullEngine(flag: true);
        Exception? other = null;
        JuceAudioEngine? leaked = null;
        var t = new Thread(() =>
        {
            try { leaked = NullEngine(flag: false); }
            catch (Exception ex) { other = ex; }
        });
        t.Start();
        Assert.True(t.Join(TimeSpan.FromSeconds(30)), "engine construction on the second thread hung");
        leaked?.Dispose();
        Assert.Null(leaked);
        Assert.IsType<JuceAudioException>(other);
    }

    /// <summary>The manual flag reaches the device: a manual-pump Null device starts no
    /// pacing thread, so no callback ever runs, while a paced one does.</summary>
    [NativeAvailableFact]
    public void TheManualFlag_StopsThePacingThread()
    {
        using (var paced = NullEngine(flag: true))
        {
            var deadline = DateTime.UtcNow.AddSeconds(5);
            XrunStats s;
            while (!(paced.TryGetXrunStats(out s) && s.Callbacks > 0) && DateTime.UtcNow < deadline)
                Thread.Sleep(10);
            Assert.True(s.Callbacks > 0, "the paced Null device never called back");
        }

        using var manual = NullEngine(flag: false, manual: true);
        Thread.Sleep(200); // 20 blocks at 10 ms; a paced device would have run them
        Assert.True(manual.TryGetXrunStats(out var m));
        Assert.Equal(0, m.Callbacks);
    }
}
