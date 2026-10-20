# JuceChordDetector

Identifies triads (major, minor, diminished, augmented) from a 12-bin
chroma vector using cosine-similarity template matching. 48 templates
are pre-computed at construction time (12 roots × 4 qualities).

```csharp
var det = new JuceChordDetector { MinConfidence = 0.6f };
var r   = det.Detect(chroma);          // chroma.Length == 12

if (r is { } hit)
    Console.WriteLine($"{hit.Name}  ({hit.Confidence:P0})");
```

## API

| Member | Description |
|--------|-------------|
| `JuceChordDetector()` | Precomputes templates. |
| `float MinConfidence { get; init; } = 0.55f` | Minimum cosine similarity to report. |
| `JuceChordReading? Detect(ReadOnlySpan<float> chroma)` | Throws if not 12 bins. |

`JuceChordReading` exposes `Name` (`"C"`, `"Am"`, `"F#dim"`, …),
`RootSemitone` (0..11), `Quality` and `Confidence`.
