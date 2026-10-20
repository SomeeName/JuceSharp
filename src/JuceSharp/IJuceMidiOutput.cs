namespace JuceSharp;

/// <summary>
/// The contract of <see cref="JuceMidiOutput" />; implemented by
/// <see cref="JuceMidiOutput" />. A test seam for host code that drives a
/// hardware synth — see <c>docs/guides/testing-without-native.md</c>. Without it
/// <see cref="IJuceAudioEngine.OpenMidiOutput" /> returned a sealed concrete
/// type with an internal constructor, which no fake could satisfy: every host
/// path that opened an output got a <see cref="NullReferenceException" /> on
/// first use through the seam.
/// <para>
///     Every member is <c>&lt;inheritdoc&gt;</c> onto its concrete counterpart:
///     one source of truth. Pinned by <c>Wave2DocsInterfaceDocTests</c>.
/// </para>
/// </summary>
public interface IJuceMidiOutput : IDisposable
{
    /// <inheritdoc cref="JuceMidiOutput.DeviceIndex" />
    int DeviceIndex { get; }

    /// <inheritdoc cref="JuceMidiOutput.IsDisposed" />
    bool IsDisposed { get; }

    /// <inheritdoc cref="JuceMidiOutput.Send(byte,byte,byte)" />
    void Send(byte status, byte data1, byte data2);

    /// <inheritdoc cref="JuceMidiOutput.Send(ReadOnlySpan{byte})" />
    void Send(ReadOnlySpan<byte> data);

    /// <inheritdoc cref="JuceMidiOutput.SendAt(byte,byte,byte,long)" />
    void SendAt(byte status, byte data1, byte data2, long renderFrame);

    /// <inheritdoc cref="JuceMidiOutput.SendAt(ReadOnlySpan{byte},long)" />
    void SendAt(ReadOnlySpan<byte> data, long renderFrame);

    /// <inheritdoc cref="JuceMidiOutput.SentCount" />
    long SentCount { get; }

    /// <inheritdoc cref="JuceMidiOutput.DroppedCount" />
    long DroppedCount { get; }

    /// <inheritdoc cref="JuceMidiOutput.FailedCount" />
    long FailedCount { get; }

    /// <inheritdoc cref="JuceMidiOutput.ReadCounters" />
    (long Sent, long Dropped) ReadCounters();

    /// <inheritdoc cref="JuceMidiOutput.ReadAllCounters" />
    (long Sent, long Dropped, long Failed) ReadAllCounters();
}
