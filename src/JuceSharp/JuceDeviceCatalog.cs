namespace JuceSharp;

/// <summary>
/// A single audio host (AudioIODeviceType) discovered on the current
/// platform — e.g. "Windows Audio", "ASIO", "CoreAudio" — together with its
/// input and output devices.
/// </summary>
public sealed record JuceAudioHost(
    string                          TypeName,
    AudioDriver                     Driver,
    IReadOnlyList<JuceAudioDevice>  OutputDevices,
    IReadOnlyList<JuceAudioDevice>  InputDevices)
{
    /// <summary>Whether this host offers at least one output device — false for a host
    /// that is installed but has no hardware behind it, which is the common ASIO case.</summary>
    public bool HasOutputs => OutputDevices.Count > 0;

    /// <summary>Whether this host offers at least one input (capture) device.</summary>
    public bool HasInputs  => InputDevices .Count > 0;
}

/// <summary>
/// One-call catalog of every host + every device available to JUCE on this
/// platform. Used to populate a "device manager" UI without callers having to
/// spin probe engines themselves.
/// <para>
/// NO PROBE ENGINES. This used to describe a design where the catalog spun one
/// short-lived <see cref="JuceAudioEngine"/> per known host; since ABI v13 it
/// calls <c>dca_enumerate_host_devices</c>, which scans a single host without
/// opening any device at all (see <see cref="Enumerate"/>). Nothing here
/// constructs an engine — which is what makes it safe to call from the UI
/// thread while another engine is running, and fast even when an ASIO driver
/// cannot open its hardware.
/// </para>
/// </summary>
public static class JuceDeviceCatalog
{
    private static readonly (AudioDriver Driver, string TypeName, string Display)[] s_known =
    {
        (AudioDriver.Wasapi,      "Windows Audio",                       "Windows Audio (Shared)"),
        (AudioDriver.Wasapi,      "Windows Audio (Exclusive Mode)",      "Windows Audio (Exclusive)"),
        (AudioDriver.Wasapi,      "Windows Audio (Low Latency Mode)",    "Windows Audio (Low Latency)"),
        (AudioDriver.DirectSound, "DirectSound",                         "DirectSound"),
        (AudioDriver.Asio,        "ASIO",                                "ASIO"),
        (AudioDriver.CoreAudio,   "CoreAudio",                           "CoreAudio (macOS)"),
        (AudioDriver.Alsa,        "ALSA",                                "ALSA (Linux)"),
        (AudioDriver.Jack,        "JACK",                                "JACK"),
    };

    /// <summary>Enumerate every host JUCE recognises on this platform together
    /// with its devices. Hosts whose probe fails (unsupported on this OS, no
    /// driver installed, etc.) are silently skipped.</summary>
    public static IReadOnlyList<JuceAudioHost> Enumerate()
    {
        // ABI v13: use the native dca_enumerate_host_devices which scans a
        // single host without opening any device. No probe engines, no audio
        // I/O — completes in milliseconds even when an ASIO driver fails to
        // open its hardware.
        var nativeTypes = EnumerateNativeHostTypes();
        var result = new List<JuceAudioHost>();
        foreach (var (driver, typeName, _) in s_known)
        {
            if (!nativeTypes.Contains(typeName, StringComparer.OrdinalIgnoreCase))
                continue;
            try
            {
                var outs = EnumerateHost(typeName, isInput: false, driver);
                var ins  = EnumerateHost(typeName, isInput: true , driver);
                result.Add(new JuceAudioHost(typeName, driver, outs, ins));
            }
            catch
            {
                // Per-host failure: skip but keep going for the others.
            }
        }
        return result;
    }

    private static IReadOnlyList<string> EnumerateNativeHostTypes()
    {
        const int max = 16;
        var buf = new byte[max * 64];
        var s = Interop.NativeMethods.EnumerateHostTypes(buf, max, out var n);
        if (s != Interop.NativeMethods.DcaStatus.Ok || n <= 0)
            return Array.Empty<string>();
        var list = new List<string>(n);
        for (int i = 0; i < n; ++i)
        {
            int off = i * 64, len = 0;
            while (len < 64 && buf[off + len] != 0) ++len;
            if (len > 0) list.Add(System.Text.Encoding.UTF8.GetString(buf, off, len));
        }
        return list;
    }

    private static IReadOnlyList<JuceAudioDevice> EnumerateHost(string typeName, bool isInput, AudioDriver driver)
    {
        var probe = Interop.NativeMethods.EnumerateHostDevices(typeName, isInput ? 1 : 0, null, 0, out var count);
        if (probe != Interop.NativeMethods.DcaStatus.Ok || count <= 0)
            return Array.Empty<JuceAudioDevice>();
        var buf = new Interop.NativeMethods.DcaDeviceInfo[count];
        var s = Interop.NativeMethods.EnumerateHostDevices(typeName, isInput ? 1 : 0, buf, count, out var actual);
        if (s != Interop.NativeMethods.DcaStatus.Ok || actual <= 0)
            return Array.Empty<JuceAudioDevice>();
        var list = new List<JuceAudioDevice>(actual);
        // driver comes from the HOST being scanned, not from the row.
        for (int i = 0; i < actual; ++i) list.Add(JuceAudioDevice.FromNative(buf[i], driver));
        return list;
    }

    /// <summary>Friendly human-readable label for a known host TypeName.</summary>
    public static string DisplayName(string typeName)
    {
        foreach (var h in s_known)
            if (string.Equals(h.TypeName, typeName, StringComparison.OrdinalIgnoreCase))
                return h.Display;
        return string.IsNullOrEmpty(typeName) ? "Default" : typeName;
    }

    /// <summary>The canonical AudioDriver enum value for a host TypeName.</summary>
    public static AudioDriver DriverFor(string typeName)
    {
        foreach (var h in s_known)
            if (string.Equals(h.TypeName, typeName, StringComparison.OrdinalIgnoreCase))
                return h.Driver;
        return AudioDriver.Default;
    }
}
