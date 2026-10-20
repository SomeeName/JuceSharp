// Wwise-free Wwise Vorbis (.wem) ENCODER (DotCharter #317): interleaved float PCM
// in, a .wem the way Wwise 2021 writes it for Rocksmith 2014 out -- no Wwise.
//
// libvorbis (JUCE's bundled 1.3.7) encodes at quality 0.4 with an 18 kHz
// lowpass, which is the configuration whose setup header, converted to Wwise
// form, is BYTE-IDENTICAL to the one Wwise writes (measured on 26 real files).
// The writer is the exact inverse of WemVorbisDecoder's rebuild: codebooks named
// by their index in the packed aoTuV 6.03 library, the setup's fixed fields
// stripped, 2-byte packet headers, "mod" audio packets (no packet-type bit, no
// long-window neighbour flags), the 0x42 fmt chunk with the vorb fields, and the
// relative seek table. Layout and every field rule: docs/agents/wem-encoder.md.
//
// Only 48 kHz stereo is accepted: it is the one configuration whose setup is
// pinned (kWwiseSetupFnv64), and the constants copied from Wwise (decode alloc,
// codebook hash) are only valid for that setup. Callers resample/upmix first.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dcwem
{

struct WemEncodeStats
{
    int64_t frames = 0;          // = the header sample count
    int64_t decodedFrames = 0;   // frames the packets decode to (>= frames)
    int     packets = 0;         // audio packets
    int     seekEntries = 0;
    int     maxPacket = 0;       // largest audio packet payload, bytes
    int     setupBytes = 0;      // Wwise setup packet payload, bytes
};

inline constexpr int kWemEncodeSampleRate = 48000;
inline constexpr int kWemEncodeChannels = 2;

// Encode `frames` frames of interleaved float PCM. Returns false with `error`
// set when the input is not 48 kHz stereo, when libvorbis fails, or when the
// setup header is not the pinned Wwise one (a libvorbis change).
bool encodeVorbisWem(const float* interleaved, int64_t frames, int channels, int sampleRate,
                     std::vector<uint8_t>& out, WemEncodeStats& stats, std::string& error);

} // namespace dcwem
