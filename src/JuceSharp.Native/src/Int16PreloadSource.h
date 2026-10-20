#pragma once

// A preloaded song held as 16-bit PCM instead of float32 (DotCharter #411).
//
// TransportSource used to keep the whole decoded song in a
// juce::AudioBuffer<float> at the DEVICE rate and play it through
// juce::MemoryAudioSource: ~10.6 MB per stereo minute at 44.1 kHz, ~11.5 MB at
// 48 kHz -- 83-147 MB per song in the owner's logs, and the same again for
// every stem. This holds the same samples as int16 with one per-source scale,
// which is HALF that, and converts back to float in the callback.
//
// Precision. The scale is the smallest that fits the buffer's own peaks, and
// never smaller than 1/32768, which is the step JUCE's own 16-bit readers
// decode with. So a 16-bit file that was NOT rate-converted round-trips
// BIT-EXACT (the PSARC pipeline hands over 16-bit WAV), and anything else --
// a 44.1 -> 48 kHz conversion, a 24-bit FLAC, a Vorbis decode -- is rounded to
// the nearest step: error <= half a step, i.e. -96 dBFS for a source that
// peaks at or below full scale. A source that peaks ABOVE full scale (Vorbis
// overs, resampler overshoot) gets a coarser step instead of clipping.
//
// Real-time contract: getNextAudioBlock is a multiply-copy loop over memory
// that was filled on the load thread -- no allocation, no lock, no I/O, the
// same properties juce::MemoryAudioSource had, which is what keeps the
// preloaded path off the JUCE read-ahead lock (see
// TransportSource::wireReaderToTransport). Its block semantics are a copy of
// juce::MemoryAudioSource's (channel mapping, end-of-data clearing, position
// arithmetic) so swapping the storage changes nothing a listener can time.

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dotcharter
{

class Int16PreloadSource final : public juce::PositionableAudioSource
{
public:
    /// Bytes this source holds for `frames` x `channels` -- what the engine's
    /// preload budget is charged.
    static constexpr int64_t bytesFor(int64_t frames, int channels) noexcept
    {
        return frames * (int64_t) channels * (int64_t) sizeof(int16_t);
    }

    /// The step a buffer with these extreme sample values is stored at:
    /// the smallest that keeps every sample inside int16, never below 1/32768.
    static float stepFor(float minSample, float maxSample) noexcept
    {
        const double base = 1.0 / 32768.0;
        double step = base;
        if (maxSample > 0.0f) step = std::max(step, (double) maxSample / 32767.0);
        if (minSample < 0.0f) step = std::max(step, (double) -minSample / 32768.0);
        return (float) step;
    }

    /// Load thread only. Quantises `source` into this object's own storage;
    /// the caller may free `source` as soon as this returns. Throws
    /// std::bad_alloc when the storage cannot be allocated.
    explicit Int16PreloadSource(const juce::AudioBuffer<float>& source)
        : channels_(source.getNumChannels()), frames_(source.getNumSamples())
    {
        float lo = 0.0f, hi = 0.0f;
        for (int ch = 0; ch < channels_; ++ch)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax(source.getReadPointer(ch), frames_);
            lo = std::min(lo, range.getStart());
            hi = std::max(hi, range.getEnd());
        }
        step_ = stepFor(lo, hi);

        const size_t total = (size_t) channels_ * (size_t) frames_;
        data_.malloc(total == 0 ? 1 : total);   // HeapBlock throws on failure (JUCE_EXCEPTIONS)
        if (data_.get() == nullptr) throw std::bad_alloc();

        const double inv = 1.0 / (double) step_;
        for (int ch = 0; ch < channels_; ++ch)
        {
            const float* src = source.getReadPointer(ch);
            int16_t* dst = channelData(ch);
            for (int i = 0; i < frames_; ++i)
            {
                const double q = std::nearbyint((double) src[i] * inv);
                dst[i] = (int16_t) std::clamp(q, -32768.0, 32767.0);
            }
        }
    }

    int     numChannels()   const noexcept { return channels_; }
    int     numFrames()     const noexcept { return frames_; }
    float   step()          const noexcept { return step_; }
    int64_t bytesHeld()     const noexcept { return bytesFor(frames_, channels_); }
    /// The decoded value of one stored sample (tests).
    float sampleAt(int ch, int frame) const noexcept { return (float) channelData(ch)[frame] * step_; }
    /// One channel's stored samples, numFrames() of them. Load thread only:
    /// the device-rate converter writes them out and the preload digest hashes
    /// them (ABI v43, DotCharter #435).
    const int16_t* channelSamples(int ch) const noexcept { return channelData(ch); }

    /// A device-rate WAV (DeviceRateWav.h, ABI v43) stores the samples a
    /// preload quantised at `step`. JUCE's 16-bit reader decodes them as
    /// v / 32768, which this source re-quantises to v at 1/32768 bit-exactly;
    /// play them at the stored step instead, and the source holds exactly what
    /// the original open held. Load thread only, before the source is wired.
    /// False, and nothing changes, unless the samples are held at 1/32768.
    bool adoptStoredStep(float step) noexcept
    {
        if (step_ != 1.0f / 32768.0f || ! (step >= step_)) return false;
        step_ = step;
        return true;
    }

    // --- juce::PositionableAudioSource, mirroring juce::MemoryAudioSource --

    void prepareToPlay(int, double) override { position_ = 0; }
    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        if (frames_ == 0)
        {
            info.clearActiveBufferRegion();
            return;
        }

        auto& dst = *info.buffer;
        const int channels = std::min(dst.getNumChannels(), channels_);
        const int n = frames_;
        const int m = info.numSamples;
        int max = 0, pos = 0;

        int i = position_;
        for (; i < n && pos < m; i += max)
        {
            max = std::min(m - pos, n - i);

            int ch = 0;
            for (; ch < channels; ++ch)
            {
                const int16_t* s = channelData(ch) + i;
                float* d = dst.getWritePointer(ch, info.startSample + pos);
                for (int k = 0; k < max; ++k)
                    d[k] = (float) s[k] * step_;
            }
            for (; ch < dst.getNumChannels(); ++ch)
                dst.clear(ch, info.startSample + pos, max);

            pos += max;
        }

        if (pos < m)
            dst.clear(info.startSample + pos, m - pos);

        position_ = i;
    }

    void  setNextReadPosition(juce::int64 p) override { position_ = (int) p; }
    juce::int64 getNextReadPosition() const override { return position_; }
    juce::int64 getTotalLength() const override { return frames_; }
    bool  isLooping() const override { return false; }
    void  setLooping(bool) override {}

private:
    int16_t*       channelData(int ch) noexcept       { return data_.get() + (size_t) ch * (size_t) frames_; }
    const int16_t* channelData(int ch) const noexcept { return data_.get() + (size_t) ch * (size_t) frames_; }

    int                       channels_ = 0;
    int                       frames_   = 0;
    float                     step_     = 1.0f / 32768.0f;
    juce::HeapBlock<int16_t>  data_;
    int                       position_ = 0;

    JUCE_DECLARE_NON_COPYABLE(Int16PreloadSource)
};

} // namespace dotcharter
