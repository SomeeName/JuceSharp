namespace JuceSharp;

/// <summary>
/// The contract of <see cref="JuceMidiInput" />; implemented by
/// <see cref="JuceMidiInput" />. A test seam for code that opens a MIDI device
/// and hands the engine its messages — see
/// <c>docs/guides/testing-without-native.md</c>.
/// <para>
///     Every member is <c>&lt;inheritdoc&gt;</c> onto its concrete counterpart:
///     one source of truth. Pinned by <c>Wave2DocsInterfaceDocTests</c>.
/// </para>
/// </summary>
public interface IJuceMidiInput : IDisposable
{
    /// <inheritdoc cref="JuceMidiInput.DeviceIndex" />
    int DeviceIndex { get; }

    /// <inheritdoc cref="JuceMidiInput.IsDisposed" />
    bool IsDisposed { get; }
}
