# Audio I/O

`JuceSharp.AudioIo` is a small package for getting samples in and out of files
without an engine: a native decoder, a minimal WAV reader/writer, and the
buffer type they share. Three public types, none of them `IDisposable`.

```xml
<PackageReference Include="JuceSharp.AudioIo" />
```

`JuceAudioDecoder` calls into `juce_sharp.dll` (the `dca_decode_audio_file`
export), so it needs the native DLL present. `WavIo` and `AudioBuffer` are
pure managed.

## `AudioBuffer`

`public sealed record AudioBuffer(float[] Interleaved, int SampleRate, int Channels)`

| Member | Description |
|---|---|
| `Interleaved` | `float[]`, channel-interleaved. |
| `SampleRate` | Hz. |
| `Channels` | Channel count. |
| `FrameCount` | `Interleaved.Length / Channels`, or 0 when `Channels == 0`. |
| `DurationSeconds` | `FrameCount / SampleRate`, or 0. |
| `float[][] SplitChannels()` | De-interleave into one array per channel. |
| `float[] ToMono()` | Average every channel down to one. |

## `JuceAudioDecoder`

`public static class JuceAudioDecoder`

| Member | Description |
|---|---|
| `(int SampleRate, int Channels, long Frames) Probe(string path)` | Header read — no samples decoded. |
| `AudioBuffer Decode(string path)` | Full decode at the file's own rate and channel count. |
| `AudioBuffer DecodeAs(string path, int targetSampleRate, int targetChannels)` | Decode, then remix and resample. |

Formats come from the native JUCE format manager: **WAV, AIFF, FLAC, OGG
Vorbis**, plus **MP3** on Windows.

```csharp
var (sr, ch, frames) = JuceAudioDecoder.Probe("song.flac");
Console.WriteLine($"{sr} Hz, {ch} ch, {frames / (double)sr:F1} s");

AudioBuffer analysis = JuceAudioDecoder.DecodeAs("song.flac", 44_100, 1);
```

> ⚠️ **Warning**
> **`DecodeAs` resamples with linear interpolation.** That is fine for analysis
> — a tuner, a chord detector, an onset detector — and it is *not* good enough
> for playback-quality output. For playback, open the file through the engine:
> `JuceAudioSource` converts to the device rate once at load with a rational
> polyphase FIR and zero group delay.

## `WavIo`

`public static class WavIo`

| Member | Description |
|---|---|
| `AudioBuffer Read(string path)` | Read a WAV file. |
| `AudioBuffer Read(Stream stream)` | Read from an open stream. |
| `void WriteFloat32(string path, AudioBuffer buffer)` | Write 32-bit IEEE float. |
| `void WriteFloat32(Stream stream, AudioBuffer buffer)` | Same, to a stream. |

**Read** accepts:

| `fmt` | Bits | Accepted |
|---|---|---|
| `3` (IEEE_FLOAT) | 32, 64 | ✅ |
| `1` (PCM) | 8, 16, 24, 32 | ✅ |
| `0xFFFE` (EXTENSIBLE) | any of the above via the sub-format GUID | ✅ |
| anything else | — | `InvalidDataException` |

The reader honours RIFF pad bytes after odd-sized chunks, clamps a `0xFFFFFFFF`
or oversized declared data length to the bytes actually present, and refuses a
data chunk over 2 GB with a `NotSupportedException`. For a non-WAV container
use `JuceAudioDecoder`.

**Write** on `WavIo` itself is float32 only (with a `fact` chunk) and takes the
whole buffer at once. For anything else use `WavWriter`.

## `WavWriter`

`public sealed class WavWriter : IDisposable` — the streaming writer, and the
one that closes the old read/write asymmetry: it emits **Float32, Pcm16 or
Pcm24**, all of which `WavIo.Read` accepts.

```csharp
using var w = new WavWriter(path, sampleRate: 44100, channels: 2, WavSampleFormat.Pcm24);
while (Render(block))
    w.Write(block);                       // interleaved, any block size, no allocation
```

| Member | Description |
|---|---|
| `WavWriter(string path, int sampleRate, int channels, WavSampleFormat format = Float32)` | |
| `WavWriter(Stream stream, …, bool leaveOpen = false)` | Stream must be writable **and seekable**. |
| `void Write(ReadOnlySpan<float> interleaved)` | Whole frames only. |
| `void WritePlanar(IReadOnlyList<float[]> planes[, int offset, int frameCount])` | One array per channel. |
| `long FrameCount`, `long DataBytes` | Written so far. |
| `static bool IsUnfinished(string path)` | Was the header ever patched? |

Three rules worth knowing before you use it:

* **PCM scales by `2^(bits-1)`**, not `2^(bits-1) - 1` — the exact inverse of the
  reader's divisor, so the only round-trip error is the half LSB of rounding.
  There is no dither, and a non-finite sample is written as silence.
* **An unfinished file is readable.** The provisional sizes are the RIFF
  unknown-length placeholder `0xFFFFFFFF`, not zero, so a crash before `Dispose`
  leaves a file `WavIo.Read` still decodes in full. `IsUnfinished` reports it.
* **4 GB throws.** RF64 is not written; a `Write` that would cross the limit
  throws `NotSupportedException` *before* writing, so the file stays consistent.

Full page: [`WavWriter`](../api/wav-writer.md).

## Round trip

```csharp
using JuceSharp.AudioIo;

AudioBuffer decoded = JuceAudioDecoder.Decode(@"C:\Music\track.mp3");
WavIo.WriteFloat32(@"C:\Music\track.wav", decoded);
AudioBuffer again = WavIo.Read(@"C:\Music\track.wav");
```

`JuceSharp.Examples.Decode` does exactly this end to end — see
[Examples](examples.md).

`JuceSharp.AudioIo.WavIo` is the only WAV reader/writer in this repo.
`JuceSharp.StemSeparation.Onnx.PcmWaveIo` — an older, near-identical copy with
its own nested `AudioBuffer` — was DELETED in the 0.9 wave. Do not reintroduce
it; if you need a second entry point, add it to `WavIo` or `WavWriter`.
