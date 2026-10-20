#pragma once

// The device-rate WAV (ABI v43, DotCharter #435): a song stored EXACTLY as a
// preloaded TransportSource holds it -- Int16PreloadSource's int16 samples at
// the device rate, plus the one per-song step they are played at.
//
// Why a step at all. The preload quantises a converted song at the smallest
// step that keeps every sample inside int16, never below 1/32768
// (Int16PreloadSource::stepFor). A loud master that the 44.1 -> 48 kHz upsample
// pushes past full scale gets a COARSER step, e.g. 1.06/32767. A plain 16-bit
// WAV can only say "v / 32768", so it cannot carry those samples bit-exactly;
// 32-bit float could, at 2.2x the bytes. This format keeps the 16-bit samples
// and adds one chunk that says what they are worth:
//
//   RIFF <size> WAVE
//   fmt  16    PCM (tag 1), channels, device rate, 16 bits
//   dcps 24    u32 version (1) | f32 step | u64 frames | u32 source rate | u32 0
//   data <n>   frames x channels interleaved int16, little-endian
//
// Any WAV reader plays the file at the right rate and length; the step only
// matters for a song whose conversion overshot (step > 1/32768), which every
// other reader hears at most ~1.3 dB quieter. The engine honours it everywhere
// it reads audio for playback or analysis: TransportSource (preload and
// streaming), the waveform, dca_decode_audio_file and the stem separator (and
// managed WavIo.Read does too).
//
// A file that HAS the chunk but does not add up -- a truncated data chunk, a
// frame count that disagrees, a step out of range, a non-16-bit fmt -- is
// DAMAGED, and TransportSource refuses to open it rather than play it wrong.
// A file without the chunk is an ordinary WAV.

#include "Int16PreloadSource.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace dotcharter::devwav
{

inline constexpr char     kChunkId[4]   = { 'd', 'c', 'p', 's' };
inline constexpr uint32_t kVersion      = 1;
inline constexpr uint32_t kChunkBytes   = 24;
inline constexpr float    kBaseStep     = 1.0f / 32768.0f;

struct Chunk
{
    float    step       = kBaseStep;
    uint64_t frames     = 0;
    uint32_t sourceRate = 0;
};

enum class Kind
{
    Plain,     // no dcps chunk (or not a RIFF/WAVE at all): an ordinary file
    Valid,     // a device-rate file whose chunk, fmt and data agree
    Damaged,   // has the chunk, but something does not add up
};

struct Probe
{
    Kind         kind = Kind::Plain;
    Chunk        chunk{};
    int          channels = 0;
    uint32_t     sampleRate = 0;
    juce::String why;

    /// What a JUCE 16-bit decode (v / 32768) must be multiplied by to give the
    /// value the song was stored at (v x step). 1 for anything but a valid
    /// device-rate file.
    double decodeGain() const noexcept
    {
        return kind == Kind::Valid ? (double) chunk.step * 32768.0 : 1.0;
    }
};

/// Walks the RIFF chunks of `in` (from position 0) for a dcps chunk and checks
/// it against fmt and data. Reads only chunk headers, the fmt body and the
/// chunk itself: data is skipped with a seek, so it is cheap on a file and on
/// memory alike.
inline Probe probe(juce::InputStream& in)
{
    Probe r;
    const int64_t total = in.getTotalLength();
    if (total < 12 || ! in.setPosition(0)) return r;

    char tag[4];
    if (in.read(tag, 4) != 4 || std::memcmp(tag, "RIFF", 4) != 0) return r;
    (void) in.readInt();
    if (in.read(tag, 4) != 4 || std::memcmp(tag, "WAVE", 4) != 0) return r;

    bool haveChunk = false, haveFmt = false, haveData = false, chunkShort = false;
    uint32_t version = 0;
    int formatTag = 0, bits = 0;
    int64_t dataStart = 0, dataBytes = 0;

    for (int i = 0; i < 64; ++i)
    {
        if (in.getPosition() + 8 > total) break;
        char id[4];
        if (in.read(id, 4) != 4) break;
        const uint32_t size = (uint32_t) in.readInt();
        const int64_t body = in.getPosition();

        if (std::memcmp(id, kChunkId, 4) == 0)
        {
            haveChunk = true;
            if (size < kChunkBytes || body + kChunkBytes > total) { chunkShort = true; break; }
            version = (uint32_t) in.readInt();
            const uint32_t stepBits = (uint32_t) in.readInt();
            std::memcpy(&r.chunk.step, &stepBits, sizeof stepBits);
            r.chunk.frames = (uint64_t) in.readInt64();
            r.chunk.sourceRate = (uint32_t) in.readInt();
        }
        else if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16)
        {
            haveFmt = true;
            formatTag = (uint16_t) in.readShort();
            r.channels = (uint16_t) in.readShort();
            r.sampleRate = (uint32_t) in.readInt();
            (void) in.readInt();
            (void) in.readShort();
            bits = (uint16_t) in.readShort();
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            haveData = true;
            dataStart = body;
            dataBytes = (int64_t) size;
        }

        const int64_t next = body + (int64_t) size + (int64_t) (size & 1u);
        if (next > total || ! in.setPosition(next)) break;
    }

    if (! haveChunk) return r;

    r.kind = Kind::Damaged;
    if (chunkShort)                     { r.why = "the dcps chunk is short"; return r; }
    if (version != kVersion)            { r.why = "unknown dcps version " + juce::String((int) version); return r; }
    if (! std::isfinite(r.chunk.step) || r.chunk.step < kBaseStep || r.chunk.step >= 1.0f)
                                        { r.why = "the stored step is out of range"; return r; }
    if (! haveFmt || formatTag != 1 || bits != 16 || r.channels <= 0 || r.sampleRate == 0)
                                        { r.why = "the fmt chunk is not 16-bit PCM"; return r; }
    if (! haveData)                     { r.why = "there is no data chunk"; return r; }
    const uint64_t expected = r.chunk.frames * (uint64_t) r.channels * 2u;
    if (r.chunk.frames == 0 || (uint64_t) dataBytes != expected)
                                        { r.why = "the data chunk does not hold the stored frame count"; return r; }
    if (dataStart + dataBytes > total)  { r.why = "the data chunk is truncated"; return r; }

    r.kind = Kind::Valid;
    return r;
}

inline Probe probeFile(const juce::File& file)
{
    juce::FileInputStream in(file);
    if (! in.openedOk()) return {};
    return probe(in);
}

inline Probe probeMemory(const void* data, size_t bytes)
{
    if (data == nullptr || bytes == 0) return {};
    juce::MemoryInputStream in(data, bytes, false);
    return probe(in);
}

/// A valid probe must also agree with the reader JUCE built for the same bytes:
/// the reader is what actually decodes the song.
inline void checkAgainstReader(Probe& p, const juce::AudioFormatReader& reader)
{
    if (p.kind != Kind::Valid) return;
    if (reader.usesFloatingPointData || reader.bitsPerSample != 16
        || (int) reader.numChannels != p.channels
        || reader.lengthInSamples != (int64_t) p.chunk.frames)
    {
        p.kind = Kind::Damaged;
        p.why = "the reader disagrees with the dcps chunk";
    }
}

/// Writes `held` -- the preload's own int16 samples and step -- as a device-rate
/// WAV at `deviceRate`. The caller deletes `dst` on failure.
inline bool write(const juce::File& dst, const Int16PreloadSource& held, uint32_t deviceRate,
                  uint32_t sourceRate, juce::String& error)
{
    const int channels = held.numChannels();
    const int frames   = held.numFrames();
    const uint64_t dataBytes = (uint64_t) frames * (uint64_t) channels * 2u;
    const uint64_t riffBytes = 4u + (8u + 16u) + (8u + kChunkBytes) + 8u + dataBytes;
    if (channels <= 0 || channels > 65535 || frames <= 0 || riffBytes > 0xFFFFFFFFull)
    {
        error = "too large or empty for a WAV";
        return false;
    }

    juce::FileOutputStream out(dst, 1 << 20);
    if (out.failedToOpen()) { error = "cannot create"; return false; }

    const auto u32 = [&out](uint32_t v) { return out.writeInt((int) v); };
    const auto u16 = [&out](uint32_t v) { return out.writeShort((short) (uint16_t) v); };
    bool ok = out.write("RIFF", 4) && u32((uint32_t) riffBytes) && out.write("WAVE", 4)
           && out.write("fmt ", 4) && u32(16) && u16(1) && u16((uint32_t) channels) && u32(deviceRate)
           && u32(deviceRate * (uint32_t) channels * 2u) && u16((uint32_t) channels * 2u) && u16(16);

    uint32_t stepBits = 0;
    const float step = held.step();
    std::memcpy(&stepBits, &step, sizeof stepBits);
    ok = ok && out.write(kChunkId, 4) && u32(kChunkBytes) && u32(kVersion) && u32(stepBits)
            && out.writeInt64((juce::int64) frames) && u32(sourceRate) && u32(0)
            && out.write("data", 4) && u32((uint32_t) dataBytes);

    constexpr int block = 16384;
    std::vector<int16_t> interleaved((size_t) block * (size_t) channels);
    for (int pos = 0; ok && pos < frames; pos += block)
    {
        const int n = std::min(block, frames - pos);
        for (int ch = 0; ch < channels; ++ch)
        {
            const int16_t* s = held.channelSamples(ch) + pos;
            for (int i = 0; i < n; ++i)
                interleaved[(size_t) i * (size_t) channels + (size_t) ch] =
                    (int16_t) juce::ByteOrder::swapIfBigEndian((uint16_t) s[i]);
        }
        ok = out.write(interleaved.data(), (size_t) n * (size_t) channels * sizeof(int16_t));
    }

    out.flush();
    ok = ok && out.getStatus().wasOk();
    if (! ok) error = "write failed for";
    return ok;
}

/// TransportSource's STREAMING path for a device-rate file whose step is not
/// 1/32768: the reader decodes v / 32768, this plays it at v x step. Runs on
/// the read-ahead thread (BufferingAudioSource pulls through it), never the
/// audio callback.
class StoredStepGainSource final : public juce::PositionableAudioSource
{
public:
    StoredStepGainSource(juce::PositionableAudioSource& input, float gain) : input_(input), gain_(gain) {}

    void prepareToPlay(int block, double rate) override { input_.prepareToPlay(block, rate); }
    void releaseResources() override { input_.releaseResources(); }
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        input_.getNextAudioBlock(info);
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->applyGain(ch, info.startSample, info.numSamples, gain_);
    }
    void setNextReadPosition(juce::int64 p) override { input_.setNextReadPosition(p); }
    juce::int64 getNextReadPosition() const override { return input_.getNextReadPosition(); }
    juce::int64 getTotalLength() const override { return input_.getTotalLength(); }
    bool isLooping() const override { return input_.isLooping(); }
    void setLooping(bool loop) override { input_.setLooping(loop); }

private:
    juce::PositionableAudioSource& input_;
    float gain_;
};

} // namespace dotcharter::devwav
