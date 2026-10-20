# WavWriter

`JuceSharp.AudioIo.WavWriter : IDisposable` — streaming RIFF/WAVE writer.

`WavIo.WriteFloat32` is a thin front end over this type (it keeps its own
`InvalidOperationException` overflow bound and then hands the bytes to a
`WavWriter`, so there is exactly ONE float32 header implementation), and it
needs the whole signal in one array, which is the wrong
shape for anything that produces audio incrementally: a render loop, a capture
ring, a stem being decoded. `WavWriter` writes a provisional header up front,
appends as you go, and patches the sizes on `Dispose`.

```csharp
using var w = new WavWriter(path, sampleRate: 44100, channels: 2, WavSampleFormat.Pcm24);
while (Render(block))
    w.Write(block);                 // interleaved; allocation-free
```

## API

| Member | Description |
|---|---|
| `WavWriter(string path, int sampleRate, int channels, WavSampleFormat format = Float32)` | Creates/truncates the file. |
| `WavWriter(Stream stream, int sampleRate, int channels, WavSampleFormat format = Float32, bool leaveOpen = false)` | Stream must be writable **and seekable**. |
| `void Write(ReadOnlySpan<float> interleaved)` | Length must be a whole number of frames. |
| `void WritePlanar(IReadOnlyList<float[]> planes, int offset, int frameCount)` | One array per channel. |
| `void WritePlanar(IReadOnlyList<float[]> planes)` | All frames. |
| `int SampleRate`, `int Channels`, `WavSampleFormat Format` | |
| `long FrameCount`, `long DataBytes` | Written so far. |
| `static bool IsUnfinished(string path)` / `(Stream)` | Was the header ever patched? |
| `void Dispose()` | Patches sizes, adds the RIFF pad byte, closes. Idempotent. |

`WavSampleFormat` is `Float32 | Pcm16 | Pcm24`. All three read back through
`WavIo.Read`.

## Quantisation

PCM output scales by **2^(bits-1)** — 32768 for 16-bit, 8388608 for 24-bit —
rounds half away from zero and clamps to `[-2^(bits-1), 2^(bits-1) - 1]`. That
is the exact inverse of the divisor `WavIo.Read` applies, so the only
round-trip error is the half LSB of rounding.

Scaling by 32767 "to avoid clipping at +1.0" is the common alternative and it is
worse: it adds a systematic gain error of `v / 32768` **on top** of the rounding.
Measured, that is 4.4e-5 against a 3.05e-5 quantisation step — the correction was
larger than the thing it corrected. Full scale clips by one LSB instead; that is
what 16-bit is.

There is **no dither**. Dither is a mastering decision the caller makes, not a
property of the container, and a writer that dithered silently would make its own
round-trip unreproducible.

A non-finite sample is written as **silence**. `(int)NaN` is 0 on x64 and
`INT_MIN` elsewhere, and an unguarded clamp lets a NaN straight through because
`NaN > max` is false — a full-scale click is a worse answer than a gap.

## An unfinished file is readable, not corrupt

The provisional sizes are `0xFFFFFFFF`, the RIFF streaming/unknown-length
placeholder — **not zero**. A process that dies before `Dispose` therefore leaves
a file `WavIo.Read` still decodes in full, because the reader clamps an unknown
length to the bytes actually present. Zeros would have produced a valid-looking
WAV declaring no audio: silent data loss with nothing to notice.
`IsUnfinished` reports the state explicitly.

## 4 GB

RIFF sizes are unsigned 32-bit and **RF64 is not written**. A `Write` that would
push the `data` chunk past the limit throws `NotSupportedException` *before*
writing anything, so the file on disk stays consistent. Discovering the overflow
at `Dispose` would mean it is already written and unrecoverable. Split long
captures across files.
