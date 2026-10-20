// juce::AudioFormat over the native WEM decoder -- see WemAudioFormat.h.

#include "WemAudioFormat.h"
#include "WemVorbisDecoder.h"

#include <cstring>

namespace dcwem
{

bool looksLikeWem(const void* data, size_t size)
{
    const auto* b = static_cast<const uint8_t*>(data);
    if (b == nullptr || size < 12) return false;
    const bool be = std::memcmp(b, "RIFX", 4) == 0;
    if (! be && std::memcmp(b, "RIFF", 4) != 0) return false;
    if (std::memcmp(b + 8, "WAVE", 4) != 0) return false;

    auto u32 = [&](size_t at) -> uint32_t {
        const uint8_t* p = b + at;
        return be ? ((uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3])
                  : ((uint32_t) p[3] << 24 | (uint32_t) p[2] << 16 | (uint32_t) p[1] << 8 | p[0]);
    };

    size_t chunk = 12;
    while (chunk + 10 <= size)
    {
        const uint32_t len = u32(chunk + 4);
        if (std::memcmp(b + chunk, "fmt ", 4) == 0)
        {
            const uint8_t* p = b + chunk + 8;
            const uint32_t codec = be ? ((uint32_t) p[0] << 8 | p[1]) : ((uint32_t) p[1] << 8 | p[0]);
            return codec == 0xFFFF;
        }
        if ((uint64_t) chunk + 8 + len > size) return false;
        chunk += 8 + (size_t) len;
    }
    return false;
}

namespace
{

class WemAudioFormatReader final : public juce::AudioFormatReader
{
public:
    WemAudioFormatReader(juce::InputStream* stream, std::unique_ptr<WemVorbisDecoder> decoder)
        : juce::AudioFormatReader(stream, "Wwise Vorbis (WEM)"), decoder_(std::move(decoder))
    {
        const auto& info = decoder_->info();
        sampleRate = info.sampleRate;
        bitsPerSample = 32;
        lengthInSamples = info.frames;
        numChannels = (unsigned int) info.channels;
        usesFloatingPointData = true;
    }

    bool readSamples(int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                     juce::int64 startSampleInFile, int numSamples) override
    {
        // AudioFormatReader::read already zero-fills past the end; still guard.
        if (numSamples <= 0) return true;

        float* dest[64] = {};
        const int channels = juce::jmin(numDestChannels, 64);
        for (int ch = 0; ch < channels; ++ch)
            dest[ch] = destChannels[ch] != nullptr
                ? reinterpret_cast<float*>(destChannels[ch]) + startOffsetInDestBuffer
                : nullptr;

        if (decoder_->position() != startSampleInFile && ! decoder_->seek(startSampleInFile))
            return false;

        const int64_t got = decoder_->read(dest, channels, numSamples);
        if (got < 0) return false;
        if (got < numSamples)
            for (int ch = 0; ch < channels; ++ch)
                if (dest[ch] != nullptr)
                    std::memset(dest[ch] + got, 0, sizeof(float) * (size_t) (numSamples - got));
        return true;
    }

private:
    std::unique_ptr<WemVorbisDecoder> decoder_;
};

} // namespace

WemAudioFormat::WemAudioFormat() : juce::AudioFormat("Wwise Vorbis (WEM)", ".wem") {}
WemAudioFormat::~WemAudioFormat() = default;

juce::AudioFormatReader* WemAudioFormat::createReaderFor(juce::InputStream* sourceStream,
                                                         bool deleteStreamIfOpeningFails)
{
    if (sourceStream == nullptr) return nullptr;

    const auto start = sourceStream->getPosition();
    auto reject = [&]() -> juce::AudioFormatReader* {
        if (deleteStreamIfOpeningFails) delete sourceStream;
        else sourceStream->setPosition(start);
        return nullptr;
    };

    uint8_t head[256] = {};
    const int headBytes = sourceStream->read(head, (int) sizeof(head));
    if (headBytes < 12 || ! looksLikeWem(head, (size_t) headBytes))
        return reject();

    sourceStream->setPosition(start);
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    try
    {
        const auto total = sourceStream->getTotalLength();
        if (total > 0 && total - start < (juce::int64) 1 << 31)
            bytes->reserve((size_t) (total - start));
        uint8_t buf[65536];
        for (;;)
        {
            const int n = sourceStream->read(buf, (int) sizeof(buf));
            if (n <= 0) break;
            bytes->insert(bytes->end(), buf, buf + n);
        }
    }
    catch (const std::bad_alloc&)
    {
        return reject();
    }

    std::string error;
    auto decoder = WemVorbisDecoder::open(std::move(bytes), Variant::Auto, error);
    if (decoder == nullptr)
    {
        DBG("WEM open failed: " << error);
        return reject();
    }

    // The reader owns the stream from here on (AudioFormatReader deletes it).
    return new WemAudioFormatReader(sourceStream, std::move(decoder));
}

} // namespace dcwem
