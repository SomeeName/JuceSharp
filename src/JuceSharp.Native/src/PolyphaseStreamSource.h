#pragma once

#include "LoadResampler.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>

// The STREAMING half of JuceSharp #33: the load-time rational polyphase
// conversion (LoadResampler.h), run per block for a source that did not fit
// the preload budget.
//
// Before this, every streaming source went through juce::ResamplingAudioSource
// (AudioTransportSource's sourceSampleRateToCorrectFor): linear interpolation
// plus one 2nd-order Butterworth, -3.14 dB at 15 kHz upward and ALIASING
// downward. A streaming source is exactly the long or many-stem song that
// missed the budget, so it was the worst-sounding path for the biggest
// projects.
//
// WHY A STAGE AND NOT "PRELOAD FIRST": the budget is a quarter of physical RAM
// shared by every open source, and what misses it is decided by size, not
// rate. Raising a rate-converted source's priority would only move the miss to
// another stem (or a same-rate file) and could not help a single song longer
// than the budget. The stage fixes the converter for every source that streams.
//
// The arithmetic is LoadResampler.h's, sample for sample: the same cached
// kernel, the same phase walk, the same zero group delay -- output frame n is
// centred on input time n*M/L. Only the input is a sliding window instead of
// the whole song.
//
// REAL-TIME CONTRACT. getNextAudioBlock runs on the audio thread (pulled by
// AudioTransportSource under RubberBandAudioSource's stretcherMutex):
//   * nothing allocates there -- the history buffer is sized ONCE in create(),
//     on the load thread, and output is produced in chunks of at most
//     kChunkOut frames so its size never depends on the device block;
//   * no lock is taken here (the BufferingAudioSource below takes its own, as
//     it always did on the streaming path);
//   * the kernel is a shared_ptr<const> taken at create(); the audio thread
//     only reads it, and it is released with this object on the control thread.
// setNextReadPosition / prepareToPlay / releaseResources arrive on control
// threads that hold the same stretcherMutex the pull does (repositionInner),
// so they never interleave with getNextAudioBlock. getNextReadPosition is
// polled WITHOUT that lock (position reads, the song clock), hence the atomic.
namespace dcload
{

class PolyphaseStreamSource final : public juce::PositionableAudioSource
{
public:
    /// Output frames produced per inner step. Bounds the history buffer and so
    /// is the whole reason no allocation is ever needed on the callback.
    static constexpr int kChunkOut = 512;

    /// Built on the LOAD thread. Null when the ratio has no rational form (a
    /// non-integer rate, too many phases, more than kMaxDecimation:1) or memory
    /// runs out -- the caller then keeps JUCE's converter. `input` is not owned
    /// and must outlive this stage.
    static std::unique_ptr<PolyphaseStreamSource> create(juce::PositionableAudioSource& input,
                                                         double fromRate, double toRate,
                                                         int channels,
                                                         ResampleReport* report = nullptr)
    {
        int l = 0, m = 0;
        if (channels <= 0 || ! reduceRatio(fromRate, toRate, l, m)) return nullptr;

        auto kernel = acquireKernel(l, m);
        if (kernel == nullptr) return nullptr;

        std::unique_ptr<PolyphaseStreamSource> s;
        try
        {
            s.reset(new PolyphaseStreamSource(input, std::move(kernel), l, m, channels));
        }
        catch (const std::bad_alloc&) { return nullptr; }

        if (report != nullptr)
        {
            *report = ResampleReport{};
            report->polyphase    = true;
            report->converted    = true;
            report->converter    = Converter::StreamingPolyphase;
            report->fromRate     = fromRate;
            report->toRate       = toRate;
            report->interpolate  = l;
            report->decimate     = m;
            report->tapsPerPhase = s->kernel_->taps();
            report->threads      = 0;     // per block, on the audio thread
        }
        return s;
    }

    // --- AudioSource -------------------------------------------------------

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        // The input runs at the INPUT rate and is pulled at most one chunk's
        // worth of input at a time.
        const double inRate = sampleRate > 0.0 ? sampleRate * (double) m_ / (double) l_ : 0.0;
        juce::ignoreUnused(samplesPerBlockExpected);
        input_.prepareToPlay(maxChunkIn_, inRate);
        // Whatever the input buffered before, the window must be rebuilt from
        // the current position.
        resync(outPos_.load(std::memory_order_relaxed));
    }

    void releaseResources() override { input_.releaseResources(); }

    /// AUDIO THREAD. No allocation, no lock of its own.
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        if (info.buffer == nullptr || info.numSamples <= 0) return;

        const int outCh = info.buffer->getNumChannels();
        const int useCh = std::min(outCh, channels_);
        for (int c = useCh; c < outCh; ++c)
            info.buffer->clear(c, info.startSample, info.numSamples);

        int done = 0;
        while (done < info.numSamples)
        {
            const int want = std::min(kChunkOut, info.numSamples - done);

            // The last output of this chunk needs its window to end here.
            const long long lastBase = base_ + ((long long) p_ + (long long) (want - 1) * m_) / l_;
            fillHistoryTo(lastBase + taps_);

            int       p    = p_;
            long long base = base_;
            for (int c = 0; c < useCh; ++c)
            {
                p    = p_;
                base = base_;
                const float* hist = history_.getReadPointer(c);
                float*       out  = info.buffer->getWritePointer(c, info.startSample + done);
                for (int i = 0; i < want; ++i)
                {
                    out[i] = dotProduct(kernel_->phase(p), hist + (base - histStart_), taps_);
                    base += mWhole_;
                    p    += mFrac_;
                    if (p >= l_) { p -= l_; ++base; }
                }
            }
            if (useCh == 0)
            {
                // No channel to write, but time still passes.
                for (int i = 0; i < want; ++i)
                {
                    base += mWhole_;
                    p    += mFrac_;
                    if (p >= l_) { p -= l_; ++base; }
                }
            }
            p_    = p;
            base_ = base;
            done += want;

            dropHistoryBefore(base_);
        }

        outPos_.store(outPos_.load(std::memory_order_relaxed) + info.numSamples,
                      std::memory_order_release);
    }

    // --- PositionableAudioSource (positions in OUTPUT frames) ---------------

    void setNextReadPosition(juce::int64 newPosition) override
    {
        resync(std::max<juce::int64>(0, newPosition));
    }

    juce::int64 getNextReadPosition() const override
    {
        return outPos_.load(std::memory_order_acquire);
    }

    juce::int64 getTotalLength() const override
    {
        return (juce::int64) outputFrameCount((long long) input_.getTotalLength(), l_, m_);
    }

    bool isLooping() const override            { return input_.isLooping(); }
    void setLooping(bool shouldLoop) override  { input_.setLooping(shouldLoop); }

    // --- introspection (tests, the open log) --------------------------------
    int interpolation() const noexcept { return l_; }
    int decimation()    const noexcept { return m_; }

private:
    PolyphaseStreamSource(juce::PositionableAudioSource& input,
                          std::shared_ptr<const PolyphaseKernel> kernel,
                          int l, int m, int channels)
        : input_(input), kernel_(std::move(kernel)), l_(l), m_(m),
          mWhole_(m / l), mFrac_(m % l), channels_(channels)
    {
        taps_ = kernel_->taps();
        half_ = kernel_->halfTaps();
        // A chunk of kChunkOut outputs advances the window by at most
        // ceil(kChunkOut * M / L) input samples; the window itself is taps_.
        maxChunkIn_ = (int) (((long long) kChunkOut * m_ + l_ - 1) / l_) + 1;
        capacity_   = taps_ + maxChunkIn_ + 2;
        history_.setSize(channels_, capacity_, false, true, false);   // may throw bad_alloc
        resync(0);
    }

    /// Re-anchor on output frame `n`: phase and window start from n*M/L, the
    /// history emptied, the input moved to the window's first sample. Control
    /// thread (seek, prepare) or construction -- never mid-pull.
    void resync(juce::int64 n)
    {
        const long long q = (long long) n * (long long) m_;
        const long long i = q / l_;
        p_         = (int) (q - i * (long long) l_);
        base_      = i - half_;
        histStart_ = base_;
        histLen_   = 0;
        // Input before the song's first sample reads as silence, exactly as
        // the offline path's bounds check makes it.
        if (base_ < 0)
        {
            histLen_ = (int) std::min<long long>(-base_, capacity_);
            history_.clear(0, histLen_);
        }
        input_.setNextReadPosition((juce::int64) std::max<long long>(0, base_));
        outPos_.store(n, std::memory_order_release);
    }

    /// Pull from the input until the history covers input index `end`
    /// (exclusive). The input reads silence past its end on its own.
    void fillHistoryTo(long long end)
    {
        const long long have = histStart_ + histLen_;
        if (end <= have) return;
        int need = (int) (end - have);
        need = std::min(need, capacity_ - histLen_);   // by construction never clipped
        if (need <= 0) return;
        juce::AudioSourceChannelInfo pull(&history_, histLen_, need);
        input_.getNextAudioBlock(pull);
        histLen_ += need;
    }

    /// Drop history before input index `start` (the next window's first sample).
    void dropHistoryBefore(long long start)
    {
        const long long drop = std::min<long long>(start - histStart_, histLen_);
        if (drop <= 0) return;
        const int keep = histLen_ - (int) drop;
        for (int c = 0; c < channels_; ++c)
        {
            float* h = history_.getWritePointer(c);
            if (keep > 0) std::memmove(h, h + drop, sizeof(float) * (size_t) keep);
        }
        histStart_ += drop;
        histLen_    = keep;
    }

    juce::PositionableAudioSource&         input_;
    std::shared_ptr<const PolyphaseKernel> kernel_;
    const int                              l_;
    const int                              m_;
    const int                              mWhole_;
    const int                              mFrac_;
    const int                              channels_;
    int                                    taps_       = 0;
    int                                    half_       = 0;
    int                                    maxChunkIn_ = 0;
    int                                    capacity_   = 0;
    juce::AudioBuffer<float>               history_;
    long long                              histStart_ = 0;   ///< input index of history_[0]
    int                                    histLen_   = 0;
    int                                    p_         = 0;   ///< phase of the next output
    long long                              base_      = 0;   ///< its window's first input index
    std::atomic<juce::int64>               outPos_{ 0 };
};

} // namespace dcload
