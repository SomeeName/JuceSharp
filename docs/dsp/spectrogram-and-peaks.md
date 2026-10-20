# Spectrogram & Waveform Peaks

Two offline visualisation helpers from `JuceSharp.Dsp`. Both are pure managed
and take samples you already hold.

## `JuceSpectrogramBuilder`

`sealed class`

| Member | Default | Description |
|---|---|---|
| `JuceSpectrogramBuilder(int fftSize = 2048, int hopSize = 512)` | | Hann-windowed STFT. |
| `int FftSize` | `2048` | |
| `int HopSize` | `512` | |
| `JuceSpectrogram Build(ReadOnlySpan<float> mono, int sampleRate, bool decibelScale = false)` | | |

## `JuceSpectrogram`

`sealed record JuceSpectrogram(float[][] Frames, int FftSize, int HopSize, int SampleRate, bool DecibelScale)`

| Member | Description |
|---|---|
| `Frames` | One magnitude array per frame. |
| `FrameCount` | `Frames.Length`. |
| `BinCount` | `FftSize / 2`. |
| `double FrameTimeSeconds(int frameIndex)` | Time of a frame. |
| `double BinHz(int bin)` | Centre frequency of a bin. |
| `DecibelScale` | Whether `Frames` holds dB or linear magnitudes. |

```csharp
var builder = new JuceSpectrogramBuilder(fftSize: 2048, hopSize: 512);
JuceSpectrogram spec = builder.Build(mono, 44_100, decibelScale: true);

for (int f = 0; f < spec.FrameCount; f++)
{
    int loudest = 0;
    for (int b = 1; b < spec.BinCount; b++)
        if (spec.Frames[f][b] > spec.Frames[f][loudest]) loudest = b;

    Console.WriteLine($"{spec.FrameTimeSeconds(f):F3}s  {spec.BinHz(loudest):F0} Hz");
}
```

Time resolution is `HopSize / sampleRate`; frequency resolution is
`sampleRate / FftSize`. You trade one for the other — a 4096-point FFT at hop
512 gives finer bins with the same frame rate, at four times the work per
frame.

## `JuceWaveformPeaks`

`static class`

| Member | Description |
|---|---|
| `static JuceWaveformPeak[] Build(ReadOnlySpan<float> samples, int channels, int sampleRate, double bucketMilliseconds = 10.0)` | Min/max per bucket, over an interleaved buffer. |

`JuceWaveformPeak` is `readonly record struct (float Min, float Max)`.

```csharp
JuceWaveformPeak[] peaks =
    JuceWaveformPeaks.Build(buffer.Interleaved, buffer.Channels,
                            buffer.SampleRate, bucketMilliseconds: 10.0);

foreach (var p in peaks)
    DrawColumn(top: p.Max, bottom: p.Min);
```

## Which peak API?

| | `JuceWaveformPeaks.Build` | `JuceAudioEngine.GenerateWaveform` |
|---|---|---|
| Input | samples you already have | a file path, or a byte blob |
| Output | **min and max** per bucket | one absolute peak per block |
| Decodes | no | yes, natively |
| Needs the native DLL | no | yes |

Min/max draws better at low zoom because it preserves asymmetry and DC offset;
a single absolute peak folds the waveform about zero. Use the engine's version
when you have a file and no samples, and this one when you have already
decoded — or when you are drawing a stem that came out of a separator.

See also [Waveform Generation](../guides/waveform.md).
