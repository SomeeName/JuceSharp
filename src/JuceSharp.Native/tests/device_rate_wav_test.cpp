// The device-rate WAV (DeviceRateWav.h, ABI v43, DotCharter #435): the writer,
// the probe that tells a valid file from a damaged one, the preload's stored-
// step adopt and the STREAMING path's gain source. Managed tests
// (AbiV43Tests) prove the preloaded open end to end through the DLL; the
// streaming path cannot be forced from there (the preload budget is a quarter
// of RAM), so it is proven here, against the same headers.

#include "DeviceRateWav.h"

#include <cmath>
#include <cstring>
#include <iostream>

using namespace dotcharter;

static int failures = 0;

#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (! (cond)) { std::cerr << "FAIL: " << what << '\n'; ++failures; }    \
    } while (0)

// Two channels of partials peaking at `peak` -- above 1 reproduces a loud
// master whose 44.1 -> 48 kHz upsample overshot full scale.
static juce::AudioBuffer<float> music(int frames, float peak)
{
    juce::AudioBuffer<float> b(2, frames);
    for (int i = 0; i < frames; ++i)
    {
        const double t = i / 48000.0;
        const double x = 0.6 * std::sin(2 * juce::MathConstants<double>::pi * 220 * t)
                       + 0.4 * std::sin(2 * juce::MathConstants<double>::pi * 1337 * t);
        b.setSample(0, i, (float) (peak * x));
        b.setSample(1, i, (float) (-peak * x * 0.8));
    }
    return b;
}

static void putU32(juce::MemoryBlock& m, size_t at, uint32_t v)
{
    auto* p = static_cast<uint8_t*>(m.getData()) + at;
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t) (v >> (8 * i));
}

int main()
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("jucesharp-devrate-", "", false);
    dir.createDirectory();
    const auto file = dir.getChildFile("song.wav");

    const auto source = music(48000, 1.06f);
    const Int16PreloadSource held(source);
    CHECK(held.step() > devwav::kBaseStep, "an overshooting buffer should take a coarser step");

    // --- write + probe --------------------------------------------------------
    juce::String error;
    CHECK(devwav::write(file, held, 48000, 44100, error), "write failed: " << error);
    const int64_t expectedBytes = 76 + (int64_t) held.numFrames() * 2 * 2;
    CHECK(file.getSize() == expectedBytes, "a 16-bit file plus a 24-byte chunk, got " << file.getSize());

    auto probe = devwav::probeFile(file);
    CHECK(probe.kind == devwav::Kind::Valid, "a fresh file probes valid: " << probe.why);
    CHECK(probe.chunk.step == held.step(), "the stored step is the preload's");
    CHECK(probe.chunk.frames == (uint64_t) held.numFrames(), "frames");
    CHECK(probe.chunk.sourceRate == 44100u, "source rate");
    CHECK(probe.channels == 2 && probe.sampleRate == 48000u, "fmt");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader(
        wav.createReaderFor(new juce::FileInputStream(file), true));
    CHECK(reader != nullptr, "JUCE reads the file as an ordinary WAV");
    if (reader == nullptr) return 1;
    devwav::checkAgainstReader(probe, *reader);
    CHECK(probe.kind == devwav::Kind::Valid, "the reader agrees with the chunk");

    // --- preload: decode + adopt is bit-exact ---------------------------------
    {
        juce::AudioBuffer<float> decoded(2, (int) reader->lengthInSamples);
        reader->read(&decoded, 0, decoded.getNumSamples(), 0, true, true);
        Int16PreloadSource reheld(decoded);
        CHECK(reheld.step() == devwav::kBaseStep, "a 16-bit decode re-quantises at 1/32768");
        CHECK(reheld.adoptStoredStep(probe.chunk.step), "adopt");
        CHECK(reheld.step() == held.step(), "the adopted step is the preload's");
        bool same = reheld.numFrames() == held.numFrames();
        for (int ch = 0; same && ch < 2; ++ch)
            same = std::memcmp(reheld.channelSamples(ch), held.channelSamples(ch),
                               (size_t) held.numFrames() * sizeof(int16_t)) == 0;
        CHECK(same, "the re-read int16 samples are the written ones, bit for bit");
        CHECK(! reheld.adoptStoredStep(probe.chunk.step), "a second adopt is refused");
    }

    // --- streaming: the gain source plays v x step -----------------------------
    {
        juce::AudioFormatReaderSource rs(reader.get(), false);
        devwav::StoredStepGainSource gained(rs, (float) probe.decodeGain());
        gained.prepareToPlay(512, 48000.0);
        juce::AudioBuffer<float> block(2, 512);
        double worst = 0.0, peak = 0.0;
        for (int pos = 0; pos + 512 <= held.numFrames(); pos += 512)
        {
            gained.getNextAudioBlock(juce::AudioSourceChannelInfo(&block, 0, 512));
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                {
                    const double want = held.sampleAt(ch, pos + i);
                    worst = std::max(worst, std::abs(block.getSample(ch, i) - want));
                    peak = std::max(peak, std::abs((double) block.getSample(ch, i)));
                }
        }
        CHECK(worst <= 1.0e-6, "streamed samples are the preload's, worst error " << worst);
        CHECK(peak > 1.0, "the streamed song keeps its over-full-scale peak, got " << peak);
        gained.releaseResources();
    }
    reader.reset();

    // --- damaged and plain files ---------------------------------------------
    juce::MemoryBlock good;
    file.loadFileAsData(good);
    const auto probeOf = [](const juce::MemoryBlock& m) { return devwav::probeMemory(m.getData(), m.getSize()); };
    CHECK(probeOf(good).kind == devwav::Kind::Valid, "memory probe of the good file");

    {
        juce::MemoryBlock cut(good.getData(), good.getSize() - 100);
        CHECK(probeOf(cut).kind == devwav::Kind::Damaged, "a truncated data chunk is damaged");
    }
    {
        auto m = good;
        putU32(m, 48, 0);   // step = 0.0f
        CHECK(probeOf(m).kind == devwav::Kind::Damaged, "a zero step is damaged");
    }
    {
        auto m = good;
        putU32(m, 44, 2);   // version 2
        CHECK(probeOf(m).kind == devwav::Kind::Damaged, "an unknown version is damaged");
    }
    {
        auto m = good;
        putU32(m, 52, (uint32_t) held.numFrames() + 1);   // frames low word
        CHECK(probeOf(m).kind == devwav::Kind::Damaged, "a frame count the data does not hold is damaged");
    }
    {
        auto m = good;
        static_cast<char*>(m.getData())[36] = 'x';   // rename the chunk: an ordinary WAV
        CHECK(probeOf(m).kind == devwav::Kind::Plain, "no dcps chunk is a plain file");
    }
    {
        const char junk[] = "OggS this is not a wav at all";
        CHECK(devwav::probeMemory(junk, sizeof junk).kind == devwav::Kind::Plain, "not RIFF is plain");
    }

    dir.deleteRecursively();
    if (failures == 0) std::cout << "device_rate_wav: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
