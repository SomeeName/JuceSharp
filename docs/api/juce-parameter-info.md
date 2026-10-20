# JuceParameterInfo

`readonly record struct JuceParameterInfo(int Index, string Name, float DefaultValue, int NumSteps)`

A static description of one hosted VST3 parameter (ABI v4). Returned by
[`JuceAudioPlugin.GetParameterInfo(int)`](juce-audio-plugin.md) and
`EnumerateParameters()`.

## Members

| Member | Type | Description |
|---|---|---|
| `Index` | `int` | The parameter index — what you pass to `GetParameter` / `SetParameter`. |
| `Name` | `string` | The plugin's own display name. |
| `DefaultValue` | `float` | The plugin's default, **normalised 0…1**. |
| `NumSteps` | `int` | Discrete step count; `0` for a continuous parameter. |
| `IsDiscrete` | `bool` | `NumSteps > 0`. |

## Example

```csharp
foreach (JuceParameterInfo p in amp.EnumerateParameters())
{
    float value = amp.GetParameter(p.Index);

    if (p.IsDiscrete)
        Console.WriteLine($"{p.Name}: step {(int)MathF.Round(value * (p.NumSteps - 1))} " +
                          $"of {p.NumSteps}");
    else
        Console.WriteLine($"{p.Name}: {value:F3}  (default {p.DefaultValue:F3})");
}

amp.SetParameter(0, 0.75f);
```

## Notes

* **Values are normalised 0…1 in both directions.** The plugin's own units —
  dB, Hz, milliseconds — are not exposed across this ABI. If you need them,
  show the plugin's editor.
* `IsDiscrete` is what tells your UI to draw a stepped control rather than a
  continuous slider. Ignoring it produces a knob that reads "6.5 of 12".
* This is the VST3 parameter description. The **DSP tone rig** has its own,
  separate catalogue (`dca_dsp_module_param_info`, plus value labels at ABI
  v24 and an ordinal step at v27), and since 0.9 it has its own managed types
  too: `JuceDspModuleCatalog.Modules` / `.Find(id)` return
  `JuceDspModuleInfo`, whose params are `JuceDspParamInfo` (`Id`, `Name`,
  `Min`, `Max`, `Default`, `Step`, `Labels`, `IsDiscrete` / `IsNamed` /
  `IsOrdinal`, `Describe(value)`) — see
  [`juce-dsp-chain.md`](juce-dsp-chain.md) and
  [The DSP Tone Rig](../guides/tone-rig.md). Do not re-declare them.
