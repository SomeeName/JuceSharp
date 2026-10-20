using System.ComponentModel;

namespace JuceSharp;

/// <summary>
/// The contract of <see cref="JuceAudioPlugin" />; implemented by
/// <see cref="JuceAudioPlugin" />. A test seam for host code that attaches,
/// bypasses, saves and restores a VST3 without one being installed — see
/// <c>docs/guides/testing-without-native.md</c>.
/// <para>
///     Every member is <c>&lt;inheritdoc&gt;</c> onto its concrete counterpart:
///     one source of truth. Pinned by <c>Wave2DocsInterfaceDocTests</c>.
/// </para>
/// </summary>
public interface IJuceAudioPlugin : IDisposable
{
    /// <inheritdoc cref="JuceAudioPlugin.Path" />
    string Path { get; }

    /// <inheritdoc cref="JuceAudioPlugin.IsDisposed" />
    bool IsDisposed { get; }

    /// <inheritdoc cref="JuceAudioPlugin.Handle" />
    /// <remarks>Hidden from IntelliSense for the same reason the concrete member
    /// is: an UNPINNED raw handle bypasses the drain that makes every other member
    /// safe against a concurrent dispose, so it must not read as a normal way to
    /// use a plugin. Without the attribute here the interface re-exposed it.</remarks>
    [EditorBrowsable(EditorBrowsableState.Never)]
    nint Handle { get; }

    /// <inheritdoc cref="JuceAudioPlugin.InputPlayer" />
    int InputPlayer { get; }

    /// <inheritdoc cref="JuceAudioPlugin.Bypass" />
    bool Bypass { get; set; }

    /// <inheritdoc cref="JuceAudioPlugin.ParameterCount" />
    int ParameterCount { get; }

    /// <inheritdoc cref="JuceAudioPlugin.LatencySamples" />
    int LatencySamples { get; }

    /// <summary>Raised when the USER closed the plugin's editor window.</summary>
    event Action? EditorClosedByUser;

    /// <inheritdoc cref="JuceAudioPlugin.AttachToInput(int)" />
    void AttachToInput(int player = 0);

    /// <inheritdoc cref="JuceAudioPlugin.DetachFromInput" />
    void DetachFromInput();

    /// <inheritdoc cref="JuceAudioPlugin.AttachToInstrument" />
    void AttachToInstrument();

    /// <inheritdoc cref="JuceAudioPlugin.DetachFromInstrument" />
    void DetachFromInstrument();

    /// <inheritdoc cref="JuceAudioPlugin.ShowEditor" />
    void ShowEditor();

    /// <inheritdoc cref="JuceAudioPlugin.HideEditor" />
    void HideEditor();

    /// <inheritdoc cref="JuceAudioPlugin.SaveState" />
    byte[] SaveState();

    /// <inheritdoc cref="JuceAudioPlugin.LoadState(byte[])" />
    void LoadState(byte[] data);

    /// <inheritdoc cref="JuceAudioPlugin.GetParameter(int)" />
    float GetParameter(int index);

    /// <inheritdoc cref="JuceAudioPlugin.SetParameter(int, float)" />
    void SetParameter(int index, float value);

    /// <inheritdoc cref="JuceAudioPlugin.GetParameterInfo(int)" />
    JuceParameterInfo GetParameterInfo(int index);

    /// <inheritdoc cref="JuceAudioPlugin.EnumerateParameters" />
    JuceParameterInfo[] EnumerateParameters();
}
