# The Wwise-free .wem encoder (ABI v44)

`src/wem/WemVorbisEncoder.cpp` writes Wwise Vorbis `.wem` files the way
Wwise 2021 writes them for Rocksmith 2014, with no Wwise installed
(DotCharter #317). It is the exact inverse of `WemVorbisDecoder.cpp`'s rebuild,
and both share `WemVorbisCommon.h` (bit I/O, the packed codebook libraries,
ww2ogg's codebook rebuild).

Entry points: `dca_wem_encode_vorbis` / `dca_wem_encode_free`
(`docs/native/c-abi.md`), managed `JuceWemEncoder.EncodeVorbis`.

## Why it can work at all

A Wwise `.wem` does not carry its Vorbis codebooks. It names each one by a
10-bit index into a packed library that the game's runtime ships: the aoTuV
6.03 library in `wem_codebooks.h`. An encoder must therefore only ever produce
codebooks that are in that library. Every codebook libvorbis 1.3.7 writes, at
every quality from −1 to 10 and at 44.1/48 kHz in mono and stereo, is in it bit
for bit; a codebook with one bit flipped is not (the negative control).

## The configuration (do not change it)

- libvorbis VBR **quality 0.4** (`ffmpeg -q:a 4`) with an **18 kHz lowpass**
  (`OV_ECTL_LOWPASS_SET`). With it, the Wwise-form setup header is
  **byte-identical** to the 215-byte setup of every Wwise Rocksmith `.wem`
  measured (FNV-1a-64 `0xF0FD447FF724785E`). Without the lowpass, libvorbis's
  residue ends are 208 / 1632 where Wwise's are 192 / 1536.
- The encoder **refuses** to write a file whose setup is not that one: the
  constants below are valid for exactly that setup. A libvorbis upgrade that
  changes it fails loudly, never silently.
- **48 kHz stereo only**, for the same reason. Callers resample and upmix first.

## Every field, and where the rule came from

Measured on 26 Wwise 2021 Vorbis files from the owner's corpus plus the in-game
playable `test_nocrypt_p.psarc`. The fmt offsets are relative to the fmt chunk
body; vgmstream's names in brackets.

| Where | Value | Rule |
|---|---|---|
| fmt `0x00` | `0xFFFF` | Wwise codec id |
| fmt `0x02..0x0F` | 2 ch, 48000, avg bytes/s, 0, 0 | avg = floor((data − seek table) / seconds) |
| fmt `0x10` | `0x30` | extra size |
| fmt `0x14` (file offset 40) | `3` | legacy channel MASK; the old runtime misreads a packed `AkChannelConfig` |
| fmt `0x18` | sample count | [dwTotalPCMFrames] |
| fmt `0x1C` | setup packet size + 2 | [loop start]: the data start after the seek table. Our decoder reads it as the "mod signal": any value but 0x4A/0x4B/0x69/0x70 means mod packets |
| fmt `0x20` | data − seek table | [loop end] |
| fmt `0x24` | 0 | |
| fmt `0x26`, `0x32` | decoded − header frames | the end padding; equal in every file |
| fmt `0x28` | seek table bytes | [dwSeekTableSize] |
| fmt `0x2C` | seek table + setup packet | [dwVorbisDataOffset] |
| fmt `0x30` | largest audio packet | [uMaxPacketSize] |
| fmt `0x34` | 16080 | [dwDecodeAllocSize], constant for this setup |
| fmt `0x38` | 16560 | constant for this setup, meaning unknown |
| fmt `0x3C` | `0xEC69CB18` | [uHashCodebook?], constant for this setup |
| fmt `0x40/0x41` | 8, 11 | block size exponents (256 / 2048) |
| data | seek table, setup packet, audio packets | no pad byte after an odd-sized data chunk |

- **Setup packet** (2-byte size header): the standard setup with the codebooks
  replaced by library indices and these fields dropped: the time-domain
  section, the floor type, the mapping type, each mode's window and transform
  type. Residue types shrink to 2 bits. The library has 26 duplicate groups; a
  duplicate takes the index that continues its neighbours' run, which is
  Wwise's choice (417 between 430 and 418), and what makes the bytes identical.
- **Audio packets** (2-byte size header, no granule): "mod" packets. The
  packet-type bit is dropped, and on a long block so are the previous/next
  window flags (the decoder recomputes them from the neighbouring packets).
  Every other bit is kept, trailing padding included: trimming zero bits could
  turn real trailing zeros into an end-of-packet, which libvorbis reads
  differently.
- **Seek table**: 4-byte entries (u16 frames, u16 bytes), each relative to the
  previous entry; the first byte offset is measured from the setup packet. An
  entry is written when a packet's END frame is ≥ 16384 past the previous
  entry. This rule reproduces all 26 files' tables exactly; the start-frame
  variant matches none.

## Tests

`Wem/WemEncoderTests` (round trip through the native decoder, frame-exact and
aligned; the managed ww2ogg + NVorbis path agrees to ~4e-7; the setup bytes;
every fmt field; the seek table rule; determinism; refusals; and, opt-in, the
setup of every corpus `.wem` equals ours) and `AbiV44Tests`. The test signal
has an odd length on purpose: a whole-block length makes the end padding 0, and
the `0x26` check then compares 0 with 0.

**In-game: NOT yet confirmed.** A probe package built from the Python
prototype of this writer (`rs7test_nowwise_p.psarc`, #317) is waiting for the
owner's check. Update this line with the result.
