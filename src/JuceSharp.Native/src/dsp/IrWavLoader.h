// IrWavLoader.h — minimal, bounded WAV reader for USER impulse responses
// (cab / acousticsim `irPath`, ABI v22). Deliberately NOT the JUCE format
// manager: the dcdsp layer stays framework-free, and an IR loader needs only
// canonical RIFF/WAVE with PCM16 / PCM24 / float32 samples (mono, or the
// first channel of a multichannel file).
//
// Threat model: the file is UNTRUSTED user input. Every size field is
// distrusted — chunk walking is capped, sample conversion reads only what
// was actually read from disk, the frame count is capped by the caller
// (kMaxTaps = 4096 in both IR cores), and the decoded taps are sanitised
// (non-finite -> 0) and L1-bounded so a hostile IR cannot turn the FIR into
// a gain bomb. Message-thread only (called from setStringParam); never on
// the audio path.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dcrig {
namespace irwav {

struct IrData
{
    std::vector<float> samples;    // first channel, <= maxFrames taps
    double             sampleRate = 48000.0;
};

namespace detail {

inline uint32_t readU32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
inline uint16_t readU16(const uint8_t* p) {
    return (uint16_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8));
}

} // namespace detail

// Parse `utf8Path` as a RIFF/WAVE file and decode up to `maxFrames` frames of
// the FIRST channel into `out`. Accepts PCM16 / PCM24 / IEEE float32 in plain
// or WAVE_FORMAT_EXTENSIBLE framing. Returns false (out untouched semantics
// not guaranteed — check the return, not the struct) on any malformed input.
inline bool loadIrWav(const std::string& utf8Path, int maxFrames, IrData& out)
{
    if (utf8Path.empty() || maxFrames < 1) return false;

    std::ifstream f;
    try {
        f.open(std::filesystem::u8path(utf8Path), std::ios::binary);
    } catch (...) { return false; }
    if (!f.is_open()) return false;

    uint8_t hdr[12];
    if (!f.read((char*) hdr, 12)) return false;
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    // fmt state (filled by the "fmt " chunk; required before "data").
    bool     haveFmt      = false;
    uint16_t format       = 0;   // 1 = PCM, 3 = IEEE float (possibly via EXTENSIBLE)
    uint16_t numChannels  = 0;
    uint32_t sampleRate   = 0;
    uint16_t bitsPerSample = 0;

    // Bounded chunk walk: a hostile file cannot loop us or seek us wild.
    const int kMaxChunks = 64;
    for (int chunk = 0; chunk < kMaxChunks; ++chunk)
    {
        uint8_t ch[8];
        if (!f.read((char*) ch, 8)) return false;   // ran out before "data"
        const uint32_t chunkSize = detail::readU32(ch + 4);

        if (std::memcmp(ch, "fmt ", 4) == 0)
        {
            if (chunkSize < 16 || chunkSize > 4096) return false;
            uint8_t fmt[40] = { 0 };
            const uint32_t take = std::min<uint32_t>(chunkSize, 40);
            if (!f.read((char*) fmt, take)) return false;
            // Skip the remainder (+ pad byte on odd sizes).
            const uint32_t rest = chunkSize - take + (chunkSize & 1u);
            if (rest > 0 && !f.seekg((std::streamoff) rest, std::ios::cur)) return false;

            format        = detail::readU16(fmt + 0);
            numChannels   = detail::readU16(fmt + 2);
            sampleRate    = detail::readU32(fmt + 4);
            bitsPerSample = detail::readU16(fmt + 14);

            if (format == 0xFFFE) // WAVE_FORMAT_EXTENSIBLE: real format leads the SubFormat GUID
            {
                if (chunkSize < 40) return false;
                format = detail::readU16(fmt + 24);
            }
            if (numChannels < 1 || numChannels > 8) return false;
            if (sampleRate < 8000 || sampleRate > 384000) return false;
            if (format == 1) { if (bitsPerSample != 16 && bitsPerSample != 24) return false; }
            else if (format == 3) { if (bitsPerSample != 32) return false; }
            else return false;
            haveFmt = true;
        }
        else if (std::memcmp(ch, "data", 4) == 0)
        {
            if (!haveFmt) return false;
            const uint32_t bytesPerSample = bitsPerSample / 8u;
            const uint32_t frameBytes     = bytesPerSample * numChannels;
            if (frameBytes == 0) return false;

            const uint32_t framesInFile = chunkSize / frameBytes;
            const uint32_t frames = std::min<uint32_t>(framesInFile, (uint32_t) maxFrames);
            if (frames == 0) return false;

            std::vector<uint8_t> raw((size_t) frames * frameBytes);
            if (!f.read((char*) raw.data(), (std::streamsize) raw.size())) return false;

            out.samples.assign(frames, 0.0f);
            out.sampleRate = (double) sampleRate;
            const uint8_t* p = raw.data();
            for (uint32_t i = 0; i < frames; ++i, p += frameBytes)
            {
                float v = 0.0f;
                if (format == 3) // float32
                {
                    uint32_t bits = detail::readU32(p);
                    std::memcpy(&v, &bits, 4);
                }
                else if (bitsPerSample == 16)
                {
                    v = (float) (int16_t) detail::readU16(p) * (1.0f / 32768.0f);
                }
                else // PCM24
                {
                    int32_t s = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24);
                    v = (float) (s >> 8) * (1.0f / 8388608.0f);
                }
                out.samples[i] = std::isfinite(v) ? v : 0.0f;
            }

            // Safety bound: cap the worst-case convolution gain (L1 norm) so
            // a hostile / clipped IR cannot explode the chain. 32 ~= +30 dB
            // worst case; real cab IRs sit around 2..10.
            double l1 = 0.0;
            bool  any = false;
            for (float s : out.samples) { l1 += std::fabs((double) s); any = any || s != 0.0f; }
            if (!any) return false;                 // silent file: not an IR
            if (l1 > 32.0)
            {
                const float g = (float) (32.0 / l1);
                for (float& s : out.samples) s *= g;
            }
            return true;
        }
        else
        {
            // Unknown chunk: skip (word-aligned). seekg past EOF fails the
            // next read, which fails the load — bounded either way.
            const std::streamoff skip = (std::streamoff) chunkSize + (std::streamoff) (chunkSize & 1u);
            if (!f.seekg(skip, std::ios::cur)) return false;
        }
    }
    return false; // too many chunks without a data chunk
}

} // namespace irwav
} // namespace dcrig
