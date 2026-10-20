# JuceWemDecoder

Native decoder for Wwise RIFF Vorbis (`.wem`) files, the audio format Rocksmith
ships inside its PSARC archives (ABI v42). It rebuilds the
Vorbis headers Wwise strips and feeds the audio packets straight to libvorbis.
There is no Ogg file, no temporary file and no managed ww2ogg step.

```csharp
using var wem = JuceWemDecoder.OpenFile(@"C:\songs\song.wem");   // or OpenMemory(bytes)
Console.WriteLine($"{wem.Info.Channels} ch, {wem.Info.SampleRate} Hz, {wem.Info.Frames} frames, {wem.Info.Variant}");

var pcm = new short[4096 * wem.Info.Channels];
int frames;
while ((frames = wem.Read(pcm)) > 0)
    Write(pcm.AsSpan(0, frames * wem.Info.Channels));

// Or everything at once, as unclipped float:
float[] all = JuceWemDecoder.DecodeFile(path, out var info);
```

| Member | What it does |
|---|---|
| `OpenFile(path, variant = Auto)` / `OpenMemory(bytes, variant = Auto)` | Opens a decoder. The memory overload COPIES the bytes. Throws `JuceAudioException` (`Status == Decode`) for anything that is not a decodable `.wem`. |
| `Info` | `JuceWemInfo`: channels, sample rate, `Frames` (the total the reads deliver), the vorb chunk's sample count, the variant that decoded it, mod packets, granules, packet count. |
| `Read(Span<float>)` | Interleaved float, **not clipped**. Returns frames, `0` at the end. |
| `Read(Span<short>)` | Interleaved 16-bit with DotCharter's WAV formula: clamp to [-1, 1], -1 maps to -32768, everything else rounds half-to-even from `sample * 32767`. |
| `Seek(frame)` | Exact: libvorbis is restarted one packet early, so the samples equal a continuous decode. |
| `DecodeFile(path, out info)` | Whole file to interleaved float. |

**Variants** (`JuceWemVariant`): `Auto` tries `AoTuV`, `Default`, `Inline`,
`DefaultMod`, `DefaultStandard` in that order (DotCharter's managed order) and
keeps the first whose header rebuild AND libvorbis header parse succeed. A file
that carries its own Vorbis headers (old Wwise) decodes as `HeaderTriad` under
any variant.

**Output contract.** Output matches the managed Ww2Ogg.Core + NVorbis path it
replaces: same channels, same sample alignment and the same length. That length
is the decoded total clipped to the vorb sample count for granule-less files,
which covers every Rocksmith `.wem`. With NVorbis's clipping applied, the float
samples agree to within 1.0e-6. `Read(Span<short>)` is within 1 LSB of the
managed WAV. Measured on 94 Rocksmith songs, see `Wem/WemCorpusTests`.

**As an engine source** none of this is needed. Since v42 every engine's format
manager knows `.wem`, so `JuceAudioEngine.OpenFile("x.wem")` and
`OpenMemory(bytes, "wem")` open it like any other file. It is decoded on the
load thread and preloaded as 16-bit. `JuceAudioDecoder.Decode` (AudioIo) reads
`.wem` too.

**Threading:** one caller at a time per decoder, and never from an audio callback.
Internally a decoder reads front to back on up to `min(4, cores / 2)` worker
threads (a batch of packets cut into segments, each primed with the packet before
it), and its output is bit-identical to a single-threaded decode, float and
16-bit, across any read size and seek. A short file decodes on the caller's thread.
`DCA_WEM_DECODE_THREADS=1` forces the serial decode (tests and measurements).
