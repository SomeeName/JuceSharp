namespace JuceSharp;

/// <summary>
/// Static description of a single hosted VST3 parameter. Returned by
/// <see cref="JuceAudioPlugin.GetParameterInfo"/> and
/// <see cref="JuceAudioPlugin.EnumerateParameters"/>.
/// </summary>
/// <param name="Index">Zero-based index into the plugin's parameter list.</param>
/// <param name="Name">Human-readable parameter name as reported by the plugin.</param>
/// <param name="DefaultValue">Default value in normalised (0..1) space.</param>
/// <param name="NumSteps">Discrete steps, or 0 for continuous parameters.</param>
public readonly record struct JuceParameterInfo(int Index, string Name, float DefaultValue, int NumSteps)
{
    /// <summary>True if the parameter is a switch (2 steps) or stepped selector.</summary>
    public bool IsDiscrete => NumSteps > 0;
}
