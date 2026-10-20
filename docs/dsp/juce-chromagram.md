# JuceChromagram

Computes a 12-bin pitch-class profile (C, C♯, D, …, B) from an FFT
magnitude spectrum. Bins below `MinFreq` (default C2) and above
`MaxFreq` (default C7) are ignored. Output is L1-normalised so the
sum equals 1.

```csharp
var chroma = new JuceChromagram(sampleRate, fftSize: 4096);
float[] c  = chroma.Compute(frame);   // frame.Length == FftSize
```

Typically used as input to [`JuceChordDetector`](juce-chord-detector.md).

## API

| Member | Description |
|--------|-------------|
| `JuceChromagram(int sampleRate, int fftSize = 4096)` | FFT size must be power of two. |
| `float MinFreq / MaxFreq { get; init; }` | Frequency window. |
| `float[] Compute(ReadOnlySpan<float> frame)` | Returns a 12-element vector. |
