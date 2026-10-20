using System.Diagnostics.CodeAnalysis;
using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
/// The JUCE <c>AudioIODeviceType</c> family the engine should prefer when it opens a
/// device. Mirrors the native <c>DcaDriver</c> ordinals (pinned by
/// <c>NativeMethodsTests.DcaDriver_OrdinalsMatchManagedEnum</c>); a value the host
/// platform does not offer falls back to whatever JUCE finds. For a WASAPI MODE
/// (Shared / Exclusive / Low-Latency) the enum is not expressive enough — use
/// <see cref="JuceEngineConfig.OutputTypeName"/> instead.
/// </summary>
public enum AudioDriver
{
    /// <summary>Let JUCE pick the platform's default device type.</summary>
    Default     = 0,
    /// <summary>Windows Audio (WASAPI), shared mode.</summary>
    Wasapi      = 1,
    /// <summary>Windows DirectSound — the highest-latency Windows fallback.</summary>
    DirectSound = 2,
    /// <summary>ASIO; requires a vendor driver and the Steinberg SDK to have been built in.</summary>
    Asio        = 3,
    /// <summary>macOS Core Audio.</summary>
    CoreAudio   = 4,
    /// <summary>Linux ALSA.</summary>
    Alsa        = 5,
    /// <summary>The JACK Audio Connection Kit server.</summary>
    Jack        = 6,
}

/// <summary>Engine creation parameters. Zero / empty values mean "device default".</summary>
public sealed class JuceEngineConfig
{
    /// <summary>Requested device sample rate in Hz; 0 asks for the device's own rate.
    /// <see cref="Validate"/> rejects anything above 384,000.</summary>
    public int         SampleRate      { get; init; }

    /// <summary>Requested device buffer size in sample frames — this is the latency
    /// knob. 0 asks for the driver's default; <see cref="Validate"/> rejects anything
    /// above 16,384.</summary>
    public int         BufferFrames    { get; init; }

    /// <summary>How many output channels to open; 0 opens whatever the device offers.
    /// Capped at 64 by <see cref="Validate"/>.</summary>
    public int         OutputChannels  { get; init; }

    /// <summary>How many input channels to open; 0 opens whatever the device offers.
    /// This — not co-op arming — is what the live seat count follows. Capped at 64 by
    /// <see cref="Validate"/>.</summary>
    public int         InputChannels   { get; init; }
    /// <summary>Bitmask of output channels to activate (LSB = channel 0). 0 = let JUCE pick.</summary>
    public uint        OutputChannelMask { get; init; }
    /// <summary>Bitmask of input channels to activate (LSB = channel 0). 0 = let JUCE pick.</summary>
    public uint        InputChannelMask  { get; init; }
    /// <summary>Which device-type family to try first. Ignored when
    /// <see cref="OutputTypeName"/> / <see cref="InputTypeName"/> name a type explicitly.
    /// <see cref="Validate"/> rejects an undefined enum value.</summary>
    public AudioDriver PreferredDriver { get; init; } = AudioDriver.Default;
    private readonly string _outputDeviceId = string.Empty;
    private readonly string _inputDeviceId  = string.Empty;
    private readonly string _outputTypeName = string.Empty;
    private readonly string _inputTypeName  = string.Empty;
    
    /// <summary>JUCE device id of the output device to open, as reported by
    /// <see cref="JuceDeviceCatalog"/>. Empty (the default) means "device default";
    /// assigning <c>null</c> is normalised to empty rather than throwing. The id
    /// crosses the ABI as UTF-8, so non-ASCII device names round-trip.</summary>
    [AllowNull]
    public string OutputDeviceId
    {
        get => _outputDeviceId;
        init => _outputDeviceId = value ?? string.Empty;
    }
    
    /// <summary>See <see cref="OutputDeviceId"/>; the input-side counterpart.</summary>
    [AllowNull]
    public string InputDeviceId
    {
        get => _inputDeviceId;
        init => _inputDeviceId = value ?? string.Empty;
    }

    /// <summary>
    /// Explicit JUCE AudioIODeviceType name for the output device
    /// (e.g. "Windows Audio (Exclusive Mode)"). When non-empty, overrides
    /// <see cref="PreferredDriver"/> and lets callers pick a specific
    /// WASAPI mode (Shared / Exclusive / Low-Latency) that the enum
    /// otherwise cannot express. Leave empty to fall back to the enum.
    /// </summary>
    
    [AllowNull]
    public string OutputTypeName
    {
        get => _outputTypeName;
        init => _outputTypeName = value ?? string.Empty;
    }
    /// <summary>See <see cref="OutputTypeName"/>; input-side counterpart.</summary>
    [AllowNull]
    public string InputTypeName
    {
        get => _inputTypeName;
        init => _inputTypeName = value ?? string.Empty;
    }

    /// <summary>If <c>true</c> and no <c>ILogger</c> is supplied to
    /// <see cref="JuceAudioEngine"/>, JuceSharp wires a
    /// <see cref="JuceConsoleLogger"/> so native messages are visible
    /// out-of-the-box. Defaults to <c>true</c>. Has no effect when an
    /// explicit logger is supplied.</summary>
    public bool EnableConsoleLog { get; init; } = true;

    /// <summary>
    /// Register the headless "Null" device type (no sound card; the callback is paced
    /// from its own thread with silent input) with THIS engine only — ABI v41,
    /// <c>dca_engine_create2</c>'s <c>DCA_ENGINE_CREATE_NULL_DEVICE</c>. Pair it with
    /// <see cref="OutputTypeName"/> = <c>"Null"</c> to open it. Prefer this to the
    /// <c>DCA_ENABLE_NULL_DEVICE=1</c> environment variable: the variable is
    /// PROCESS-wide, so setting it around one engine's construction also registers
    /// the type in any engine another thread builds meanwhile. The variable still
    /// works and is OR-ed with this flag — <c>false</c> here never hides a type the
    /// environment enabled.
    /// </summary>
    public bool EnableNullDevice { get; init; }

    /// <summary>
    /// Manual-pump mode for the Null device (<c>DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL</c>,
    /// the per-engine form of <c>DCA_NULL_DEVICE_MANUAL=1</c>): the device starts no
    /// pacing thread and renders only when a native harness pumps it. Implies
    /// <see cref="EnableNullDevice"/>. OR-ed with the environment variable.
    /// </summary>
    public bool NullDeviceManualPump { get; init; }

    /// <summary>Throws <see cref="ArgumentOutOfRangeException"/> if any
    /// numeric field is negative or absurd. Called automatically by
    /// <see cref="JuceAudioEngine"/>'s constructor.</summary>
    public void Validate()
    {
        Check(SampleRate,     nameof(SampleRate),     0, 384_000);
        Check(BufferFrames,   nameof(BufferFrames),   0, 16_384);
        Check(OutputChannels, nameof(OutputChannels), 0, 64);
        Check(InputChannels,  nameof(InputChannels),  0, 64);
        if (!Enum.IsDefined(typeof(AudioDriver), PreferredDriver))
            throw new ArgumentOutOfRangeException(nameof(PreferredDriver),
                PreferredDriver, "Unknown AudioDriver value.");
    }

    private static void Check(int value, string name, int min, int max)
    {
        if (value < min || value > max)
            throw new ArgumentOutOfRangeException(name, value,
                $"{name} must be between {min} and {max} (0 = device default).");
    }

    /// <summary>The <c>DCA_ENGINE_CREATE_*</c> mask <c>dca_engine_create2</c> receives.</summary>
    internal uint CreateFlags()
        => (EnableNullDevice || NullDeviceManualPump ? NativeMethods.EngineCreateNullDevice : 0u)
         | (NullDeviceManualPump ? NativeMethods.EngineCreateNullDeviceManual : 0u);

    internal NativeMethods.DcaEngineConfig ToNative() => new()
    {
        SampleRate        = SampleRate,
        BufferFrames      = BufferFrames,
        OutputChannels    = OutputChannels,
        InputChannels     = InputChannels,
        OutputChannelMask = OutputChannelMask,
        InputChannelMask  = InputChannelMask,
        PreferredDriver   = (NativeMethods.DcaDriver)PreferredDriver,
        OutputDeviceId    = OutputDeviceId,
        InputDeviceId     = InputDeviceId,
        OutputTypeName    = OutputTypeName,
        InputTypeName     = InputTypeName,
    };
}
