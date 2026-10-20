using JuceSharp.Dsp;

namespace JuceSharp;

// The IJuceAudioEngine implementation, kept OUT of JuceAudioEngine.cs so that
// file reads unchanged. Every member here is an EXPLICIT interface
// implementation over an existing public one: the interface's factories answer
// in interface terms (so a fake engine can hand back fake children) while a
// caller holding the concrete type still gets JuceAudioSource / JuceAudioPlugin
// / JuceMidiInput / JuceDspChain. Nothing public moved.
//
// Everything else on the interface — ~70 members — is implemented IMPLICITLY by
// the existing public surface, which is the point: adding the interface changed
// no behaviour at all.
public sealed partial class JuceAudioEngine : IJuceAudioEngine
{
    IJuceDspChain IJuceAudioEngine.GetDspChain(int player) => GetDspChain(player);

    IJuceAudioSource IJuceAudioEngine.OpenFile(string path) => OpenFile(path);

    IJuceAudioSource IJuceAudioEngine.OpenMemory(ReadOnlySpan<byte> bytes, string? formatHint)
        => OpenMemory(bytes, formatHint);

    IJuceAudioPlugin IJuceAudioEngine.LoadPlugin(string path) => LoadPlugin(path);

    IJuceAudioPlugin IJuceAudioEngine.LoadPlugin(string path, int componentIndex)
        => LoadPlugin(path, componentIndex);

    IJuceMidiInput IJuceAudioEngine.OpenMidiInput(int deviceIndex, Action<JuceMidiMessage> handler)
        => OpenMidiInput(deviceIndex, handler);

    IJuceMidiInput IJuceAudioEngine.OpenMidiInput(int deviceIndex, JuceMidiSpanHandler handler)
        => OpenMidiInput(deviceIndex, handler);

    IJuceMidiOutput IJuceAudioEngine.OpenMidiOutput(int deviceIndex) => OpenMidiOutput(deviceIndex);

    void IJuceAudioEngine.SetSongClockSource(IJuceAudioSource? source)
    {
        if (source is not null and not JuceAudioSource)
            throw new ArgumentException("The song clock must be a source this engine opened.", nameof(source));
        SetSongClockSource((JuceAudioSource?)source);
    }

    // The async factories need an await rather than a cast: Task<T> is not
    // covariant, so Task<JuceAudioSource> is not a Task<IJuceAudioSource>.
    async Task<IJuceAudioSource> IJuceAudioEngine.OpenFileAsync(string path, CancellationToken cancellationToken)
        => await OpenFileAsync(path, cancellationToken).ConfigureAwait(false);

    async Task<IJuceAudioPlugin> IJuceAudioEngine.LoadPluginAsync(string path, CancellationToken cancellationToken)
        => await LoadPluginAsync(path, cancellationToken).ConfigureAwait(false);

    async Task<IJuceAudioPlugin> IJuceAudioEngine.LoadPluginAsync(string path, int componentIndex,
                                                                 CancellationToken cancellationToken)
        => await LoadPluginAsync(path, componentIndex, cancellationToken).ConfigureAwait(false);
}
