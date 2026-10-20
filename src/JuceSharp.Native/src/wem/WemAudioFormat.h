// juce::AudioFormat over the native WEM decoder (ABI v42, DotCharter #426).
//
// Registered with every engine's format manager, so dca_source_open_file on a
// ".wem" (and dca_source_open_memory with the "wem" hint, or no hint at all)
// opens it through the ordinary transport path -- including the 16-bit RAM
// preload (#411), which reads the reader front to back on the LOAD thread.
//
// The reader reads the whole (small, compressed) .wem into memory when it is
// created and decodes on demand. Random access is exact (WemVorbisDecoder::seek
// restarts libvorbis one packet early), so the streaming fallback -- used only
// when the preload budget is exhausted -- works too, just at decode cost.
// Never reached from the audio callback: the transport's read-ahead thread or
// the load thread drives it.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace dcwem
{

class WemAudioFormat final : public juce::AudioFormat
{
public:
    WemAudioFormat();
    ~WemAudioFormat() override;

    juce::Array<int> getPossibleSampleRates() override { return {}; }
    juce::Array<int> getPossibleBitDepths() override { return {}; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }

    juce::AudioFormatReader* createReaderFor(juce::InputStream* sourceStream,
                                             bool deleteStreamIfOpeningFails) override;

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor(std::unique_ptr<juce::OutputStream>&,
                                                             const juce::AudioFormatWriterOptions&) override
    {
        return nullptr;
    }

    using juce::AudioFormat::createWriterFor;
};

/// Cheap header sniff: a RIFF/RIFX WAVE whose fmt chunk says codec 0xFFFF.
bool looksLikeWem(const void* data, size_t size);

} // namespace dcwem
