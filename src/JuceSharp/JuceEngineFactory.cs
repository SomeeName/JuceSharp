namespace JuceSharp;

/// <summary>
/// Helpers to open a <see cref="JuceAudioEngine"/> safely. JUCE's default
/// behaviour when an empty device ID is supplied is to pick the host's first
/// device — which is often an offline ASIO interface (e.g. "Focusrite
/// Thunderbolt ASIO" when nothing is plugged in), yielding a hard
/// 0x54f / "Couldn't open the output device" error.
/// </summary>
public static class JuceEngineFactory
{
    /// <summary>Result of <see cref="OpenWithFallback"/>.</summary>
    public sealed record OpenResult(
        JuceAudioEngine?  Engine,
        JuceEngineConfig  ConfigUsed,
        IReadOnlyList<string> Attempts)
    {
        /// <summary>
        ///     The same instance as <see cref="Engine" />, typed as the
        ///     abstraction seam <see cref="IJuceAudioEngine" /> — for a host that
        ///     holds its engine through the interface (DI, or a test that swaps in
        ///     <c>FakeJuceAudioEngine</c>) and would otherwise have to name the
        ///     concrete type here and nowhere else.
        ///     <para>
        ///         <b><see cref="Engine" /> stays CONCRETE and is not going to
        ///         change</b>: it converts implicitly to
        ///         <see cref="IJuceAudioEngine" /> already, so this exists for the
        ///         <c>var</c> case and for a member group that never mentions
        ///         <see cref="JuceAudioEngine" />. Retyping the positional member
        ///         instead would break every caller that passes it to a method
        ///         taking the concrete type — three of the six shipped examples
        ///         do, so that is not a theoretical concern.
        ///     </para>
        ///     <para>
        ///         Null exactly when <see cref="Engine" /> is: no device opened.
        ///         Disposing either disposes the one engine.
        ///     </para>
        /// </summary>
        public IJuceAudioEngine? EngineInterface => Engine;
    }

    /// <summary>
    /// Open an engine with the supplied config. If the requested device fails
    /// to open, walk every other device of the chosen host until one succeeds.
    /// Returns the opened engine + the config that actually worked plus a
    /// trail of attempted device names (useful for UI logging).
    /// </summary>
    public static OpenResult OpenWithFallback(JuceEngineConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);
        var attempts = new List<string>();

        try
        {
            return new OpenResult(new JuceAudioEngine(config), config, attempts);
        }
        catch (Exception ex)
        {
            attempts.Add($"'{config.OutputDeviceId}' on '{config.OutputTypeName}' failed: {ex.Message}");
        }

        // Enumerate the requested host ONLY (ABI v13 — no audio device is
        // opened during the scan). Critically, we must not fall back to
        // EnumerateOutputDevicesLive() here because that returns devices
        // from EVERY host mixed together — trying a WASAPI device name
        // against a JUCE type=ASIO ADM fails with "No such device" on every
        // iteration.
        IReadOnlyList<JuceAudioDevice> outs = Array.Empty<JuceAudioDevice>();
        IReadOnlyList<JuceAudioDevice> ins  = Array.Empty<JuceAudioDevice>();
        if (!string.IsNullOrEmpty(config.OutputTypeName))
        {
            try
            {
                var host = JuceDeviceCatalog.Enumerate()
                    .FirstOrDefault(h => string.Equals(h.TypeName, config.OutputTypeName, StringComparison.OrdinalIgnoreCase));
                if (host is not null) { outs = host.OutputDevices; ins = host.InputDevices; }
            }
            catch (Exception ex)
            {
                attempts.Add($"probe '{config.OutputTypeName}' failed: {ex.Message}");
            }
        }
        else
        {
            outs = JuceAudioEngine.EnumerateOutputDevices();
            ins  = JuceAudioEngine.EnumerateInputDevices();
        }

        foreach (var d in outs)
        {
            var attempt = new JuceEngineConfig
            {
                SampleRate        = config.SampleRate,
                BufferFrames      = config.BufferFrames,
                PreferredDriver   = config.PreferredDriver,
                OutputTypeName    = config.OutputTypeName,
                InputTypeName     = config.InputTypeName,
                OutputDeviceId    = d.Id,
                InputDeviceId     = ins.FirstOrDefault()?.Id ?? string.Empty,
                OutputChannelMask = config.OutputChannelMask,
                InputChannelMask  = config.InputChannelMask,
                EnableConsoleLog  = config.EnableConsoleLog,
            };
            try
            {
                var eng = new JuceAudioEngine(attempt);
                attempts.Add($"fallback opened '{d.Name}'");
                return new OpenResult(eng, attempt, attempts);
            }
            catch (Exception ex)
            {
                attempts.Add($"'{d.Name}' failed: {ex.Message}");
            }
        }

        return new OpenResult(null, config, attempts);
    }
}
