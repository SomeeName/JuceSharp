using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
/// One row from <see cref="JuceAudioEngine.EnumerateOutputDevices"/>,
/// <see cref="JuceAudioEngine.EnumerateInputDevices"/> or
/// <see cref="JuceDeviceCatalog"/>: an audio device exposed by JUCE.
/// <para>
/// <see cref="TypeName"/> carries the JUCE AudioIODeviceType name (e.g.
/// "Windows Audio", "Windows Audio (Exclusive Mode)", "Windows Audio (Low
/// Latency Mode)", "ASIO", "CoreAudio") and disambiguates WASAPI Shared /
/// Exclusive / Low-Latency — which all share <see cref="AudioDriver.Wasapi"/>.
/// Empty for static enumerations returned by the no-engine path.
/// </para>
/// </summary>
public sealed record JuceAudioDevice(
    string      Id,
    string      Name,
    AudioDriver Driver,
    int         MaxInputChannels,
    int         MaxOutputChannels,
    int         DefaultSampleRate,
    bool        IsDefault,
    string      TypeName = "")
{
    /// <summary>
    ///     The ONE place a native <c>dca_device_info</c> becomes a
    ///     <see cref="JuceAudioDevice" />. There were three hand-written copies of
    ///     this projection (the engine's live enumeration, the static one, and
    ///     <see cref="JuceDeviceCatalog" />'s per-host walk) — eight POSITIONAL
    ///     arguments each, so swapping two same-typed fields in one copy compiles
    ///     perfectly and mislabels every device that one path returns.
    /// </summary>
    /// <param name="d">The native row.</param>
    /// <param name="driver">
    ///     The catalogue already knows the driver from the HOST it is scanning and
    ///     passes it. The two device-list paths pass <c>null</c> and take the
    ///     struct's own field — through <see cref="Enum.IsDefined{TEnum}(TEnum)" />,
    ///     because an unchecked cast of a value a newer DLL invented yields an
    ///     <see cref="AudioDriver" /> outside the enum, which every
    ///     <c>switch</c> over it then falls straight through.
    /// </param>
    internal static JuceAudioDevice FromNative(in NativeMethods.DcaDeviceInfo d, AudioDriver? driver = null)
        => new(d.Id,
               d.Name,
               driver ?? ToDriver(d.Driver),
               d.MaxInputChannels,
               d.MaxOutputChannels,
               d.DefaultSampleRate,
               d.IsDefault != 0,
               d.TypeName);

    internal static AudioDriver ToDriver(NativeMethods.DcaDriver native)
        => Enum.IsDefined((AudioDriver)(int)native) ? (AudioDriver)(int)native : AudioDriver.Default;
}
