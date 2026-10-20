#include "RubberBandAudioSource.h"

#include "RtDiag.h"
#include "rubberband/RubberBandStretcher.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace dotcharter {

using RubberBand::RubberBandStretcher;

// The slowest tempo a host is expected to set (DotCharter's speed floor,
// 0.25x), as a Rubber Band time ratio. See prepareToPlay.
constexpr double kSlowestPreparedTimeRatio = 4.0;

// The quality knob the host flips when stems are loaded: with 4+ R3 (Finer)
// stretchers running in parallel a busy CPU starts to glitch noticeably, and
// switching every stretcher to R2 (Faster) keeps the audio thread inside its
// budget at the cost of a small fidelity hit. 0 = Finer (default), 1 = Faster.
//
// It lives on the ENGINE (AudioEngine::stretchQualityFlag), not in a process
// global. dca_engine_set_stretch_quality takes a handle and used to ignore it,
// so one engine loading stems re-voiced every other engine in the process.

RubberBandAudioSource::RubberBandAudioSource(juce::PositionableAudioSource* inner,
                                             bool                           deleteInnerWhenRemoved,
                                             int                            numChannels,
                                             const std::atomic<int>*        engineQuality)
    : engineQuality_(engineQuality)
    , inner_(inner)
    , deleteInner_(deleteInnerWhenRemoved)
    , numChannels_(juce::jmax(1, numChannels))
{
}

RubberBandAudioSource::~RubberBandAudioSource()
{
    releaseResources();
    if (deleteInner_ && inner_ != nullptr) delete inner_;
    inner_ = nullptr;
}

void RubberBandAudioSource::prepareToPlay(int blockSize, double sampleRate)
{
    deviceSampleRate_ = sampleRate;
    deviceBlock_      = blockSize;

    if (inner_ != nullptr)
        inner_->prepareToPlay(blockSize, sampleRate);

    std::lock_guard<std::mutex> lk(stretcherMutex_);
    readAheadOut_.store(0, std::memory_order_release);
    stretching_       = false;
    discardRemaining_ = 0;

    // The OWNING engine's flag, not a process-global one.
    const bool useFaster = engineQuality_ != nullptr
                        && engineQuality_->load(std::memory_order_acquire) != 0;
    preparedFaster_ = useFaster;
    auto options = RubberBandStretcher::OptionProcessRealTime
                 | (useFaster ? RubberBandStretcher::OptionEngineFaster
                              : RubberBandStretcher::OptionEngineFiner)
                 | RubberBandStretcher::OptionPitchHighQuality
                 | RubberBandStretcher::OptionFormantPreserved;

    stretcher_ = std::make_unique<RubberBandStretcher>(
        static_cast<size_t>(juce::jmax(1, (int) std::lround(sampleRate))),
        static_cast<size_t>(numChannels_),
        options,
        1.0, // initial time ratio; set in applyRatiosLocked
        1.0);

    stretcher_->setMaxProcessSize(static_cast<size_t>(juce::jmax(blockSize, 1024)));

    // R2 (Faster) grows its output ring the first time a slower ratio is set,
    // and a ratio is set on the AUDIO thread (applyRatiosLocked): 2 x 320 KB
    // the first time a song went to 80 % (RT gate, JuceSharp #32). The ring
    // only ever grows, so size it here, once, for the slowest tempo a host
    // sets (DotCharter's floor, 0.25x); applyRatiosLocked below restores the
    // real ratio. R3 (Finer) sizes its buffers at construction.
    if (useFaster)
        stretcher_->setTimeRatio(kSlowestPreparedTimeRatio);

    const int big = juce::jmax(blockSize * 8, 8192);
    pullBuffer_    .setSize(numChannels_, big, false, true, true);
    retrieveBuffer_.setSize(numChannels_, big, false, true, true);
    pullPtrs_    .assign(numChannels_, nullptr);
    pushPtrs_    .assign(numChannels_, nullptr);
    retrievePtrs_.assign(numChannels_, nullptr);

    dirty_.store(true, std::memory_order_release);
    applyRatiosLocked();
}

void RubberBandAudioSource::releaseResources()
{
    if (inner_ != nullptr) inner_->releaseResources();
    // Destroying the stretcher must not race the audio thread using it, nor
    // the position poll: this runs on the device thread at every device
    // change, which is exactly when the other two are busiest.
    std::lock_guard<std::mutex> lk(stretcherMutex_);
    readAheadOut_.store(0, std::memory_order_release);
    stretcher_.reset();
    stretching_       = false;
    discardRemaining_ = 0;
    pullBuffer_.setSize(0, 0);
    retrieveBuffer_.setSize(0, 0);
}

void RubberBandAudioSource::applyRatiosLocked()
{
    if (! stretcher_) return;
    // Acquire matches the release stores in setBaseRatio/setTempoFactor/
    // setPitchSemitones, so concurrent setters from different threads (UI
    // tempo slider + MIDI pitch event) all become visible together once
    // the audio thread observes dirty_=true.
    const double base  = baseRatio_  .load(std::memory_order_acquire);
    const float  tempo = tempoFactor_.load(std::memory_order_acquire);
    const float  pitch = pitchScale_ .load(std::memory_order_acquire);

    // Effective rubberband time ratio: base (file/dev SR) divided by tempo factor.
    // tempo > 1.0 means play faster -> shorter output -> smaller time ratio.
    const double timeRatio = base / juce::jmax(0.01f, tempo);
    stretcher_->setTimeRatio(timeRatio);
    stretcher_->setPitchScale(juce::jmax(0.05f, pitch));
}

void RubberBandAudioSource::setBaseRatio(double r) noexcept
{
    baseRatio_.store(r > 0.0 ? r : 1.0, std::memory_order_release);
    dirty_.store(true, std::memory_order_release);
}

void RubberBandAudioSource::setTempoFactor(float f) noexcept
{
    tempoFactor_.store(f > 0.0f ? f : 1.0f, std::memory_order_release);
    dirty_.store(true, std::memory_order_release);
}

void RubberBandAudioSource::setPitchSemitones(float st) noexcept
{
    pitchScale_.store(std::pow(2.0f, st / 12.0f), std::memory_order_release);
    dirty_.store(true, std::memory_order_release);
}

void RubberBandAudioSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    // AUDIO THREAD, once per source per block — including the bypass and
    // unity fast paths below, which is why the RT gate measures this one.
    DCA_RT_LOCK_GUARD(stretcherMutex_, "stretcherMutex");

    if (! stretcher_ || inner_ == nullptr || info.numSamples <= 0)
    {
        readAheadOut_.store(0, std::memory_order_release);
        if (info.buffer != nullptr) info.clearActiveBufferRegion();
        return;
    }

    // Bypassed: an upstream stem-group stretcher handles tempo/pitch for the
    // whole group, so pass straight through. setBypass reset the stretcher.
    if (bypass_.load(std::memory_order_acquire))
    {
        readAheadOut_.store(0, std::memory_order_release);
        inner_->getNextAudioBlock(info);
        return;
    }

    // Fast path: no SR conversion, no tempo change and no pitch shift skips
    // Rubber Band completely — no process latency, no extra buffering, none of
    // the R3 engine's CPU. ONLY from a clean stretcher, though (JuceSharp #32):
    // once it has run, the inner has been read past what the listener has
    // heard, and that read-ahead is the song's next audio. Leaving for the fast
    // path then SKIPPED it (slowed -> 100 % jumped forward), and coming back
    // later replayed whatever the stretcher still held from then (100 % ->
    // slowed played stale audio). So a stretcher that has run keeps running —
    // at ratio 1.0, which Rubber Band treats as a near-transparent pass —
    // until the next seek resets it (repositionInner), where a jump is what
    // the listener asked for and the fast path resumes cleanly.
    // The alternatives, and why not: rewinding the inner by the read-ahead is
    // a seek on the AUDIO thread (AudioTransportSource::setNextReadPosition
    // takes ResamplingAudioSource's lock and, when streaming,
    // BufferingAudioSource's); draining the stretcher with a final process()
    // flushes its zero padding after the real audio, a dropout.
    const double base  = baseRatio_  .load(std::memory_order_relaxed);
    const float  tempo = tempoFactor_.load(std::memory_order_relaxed);
    const float  pitch = pitchScale_ .load(std::memory_order_relaxed);
    const bool   unity = std::abs(base - 1.0) < 1.0e-6
                      && std::abs(tempo - 1.0f) < 1.0e-4f
                      && std::abs(pitch - 1.0f) < 1.0e-4f;
    if (unity && ! stretching_)
    {
        // Nothing is being fed to the stretcher on this path, so there is no
        // read-ahead to compensate for.
        readAheadOut_.store(0, std::memory_order_release);
        inner_->getNextAudioBlock(info);
        return;
    }

    if (dirty_.exchange(false, std::memory_order_acq_rel))
        applyRatiosLocked();

    if (! stretching_)
        beginStretchLocked();

    renderStretchedLocked(info);

    // Publish for the position poll, which must never touch stretcher_ itself.
    const int avail = stretcher_->available();
    readAheadOut_.store(avail > 0 ? avail : 0, std::memory_order_release);
}

int RubberBandAudioSource::pullBlockSize() const noexcept
{
    return juce::jmin(pullBuffer_.getNumSamples(), deviceBlock_ > 0 ? deviceBlock_ : 1024);
}

void RubberBandAudioSource::resetStretcherLocked()
{
    if (stretcher_ && stretching_) stretcher_->reset();
    stretching_       = false;
    discardRemaining_ = 0;
    readAheadOut_.store(0, std::memory_order_release);
}

void RubberBandAudioSource::setBypass(bool bypassed)
{
    bypass_.store(bypassed, std::memory_order_release);
    resetStretcher();
}

void RubberBandAudioSource::beginStretchLocked()
{
    stretching_ = true;

    // Rubber Band's real-time mode does no start compensation of its own: fed
    // cold, its first output fades in over half an analysis window. Feed it the
    // silence it asks for and drop the matching start delay of output, so the
    // first sample out is the inner's next sample at full level — continuous
    // with the pass-through block before it (or with the seek target).
    // Both values follow the ratios, which the caller has just applied.
    int pad           = (int) stretcher_->getPreferredStartPad();
    discardRemaining_ = (int) stretcher_->getStartDelay();

    const int chunk = pullBlockSize();
    if (chunk <= 0) return;
    pullBuffer_.clear(0, chunk);
    for (int c = 0; c < numChannels_; ++c)
        pushPtrs_[c] = pullBuffer_.getReadPointer(juce::jmin(c, pullBuffer_.getNumChannels() - 1));
    while (pad > 0)
    {
        const int n = juce::jmin(pad, chunk);
        stretcher_->process(pushPtrs_.data(), static_cast<size_t>(n), false);
        pad -= n;
    }
}

void RubberBandAudioSource::renderStretchedLocked(const juce::AudioSourceChannelInfo& info)
{
    const int  ch     = juce::jmin(numChannels_, info.buffer->getNumChannels());
    const int  needed = info.numSamples;
    int        produced = 0;

    while (produced < needed)
    {
        // 1) Push samples until rubberband has enough output available.
        // Bound the inner loop: a decoder underrun or a malformed input can
        // leave stretcher_->available() stuck at zero (not -1) indefinitely,
        // which would spin the audio thread. Sixteen pull blocks is more
        // than enough to fill any sane stretcher latency window; beyond
        // that we treat it as an underrun and emit silence for the rest.
        // A padded start (beginStretchLocked) needs its start delay on top:
        // up to a whole analysis window of input before the first kept sample,
        // more at a fast tempo, hence the extra allowance while it lasts.
        constexpr int kMaxPushIterations  = 16;
        constexpr int kMaxStartIterations = 64;
        const int     maxPushes = kMaxPushIterations + (discardRemaining_ > 0 ? kMaxStartIterations : 0);
        int pushIterations = 0;
        while (stretcher_->available() < (needed - produced) + discardRemaining_)
        {
            if (++pushIterations > maxPushes)
            {
                for (int c = 0; c < ch; ++c)
                    info.buffer->clear(c, info.startSample + produced, needed - produced);
                return;
            }

            const int pullBlock = pullBlockSize();

            juce::AudioSourceChannelInfo pull;
            pull.buffer      = &pullBuffer_;
            pull.startSample = 0;
            pull.numSamples  = pullBlock;
            pullBuffer_.clear(0, pullBlock);
            inner_->getNextAudioBlock(pull);

            for (int c = 0; c < numChannels_; ++c)
                pushPtrs_[c] = pullBuffer_.getReadPointer(juce::jmin(c, pullBuffer_.getNumChannels() - 1));

            stretcher_->process(pushPtrs_.data(),
                                static_cast<size_t>(pullBlock),
                                false);

            // Guard: if the inner is at EOF, rubberband eventually returns -1 from available().
            if (stretcher_->available() < 0) break;
        }

        const int avail = juce::jmax(0, stretcher_->available());

        for (int c = 0; c < numChannels_; ++c)
            retrievePtrs_[c] = retrieveBuffer_.getWritePointer(c);

        if (discardRemaining_ > 0)
        {
            // The padded start's delay: output that belongs to the padding.
            const int    drop    = juce::jmin(discardRemaining_, avail, retrieveBuffer_.getNumSamples());
            const size_t dropped = drop > 0 ? stretcher_->retrieve(retrievePtrs_.data(), static_cast<size_t>(drop)) : 0;
            if (dropped == 0)
            {
                for (int c = 0; c < ch; ++c)
                    info.buffer->clear(c, info.startSample + produced, needed - produced);
                return;
            }
            discardRemaining_ -= static_cast<int>(dropped);
            continue;
        }

        const int take  = juce::jmin(needed - produced, avail, retrieveBuffer_.getNumSamples());
        if (take <= 0)
        {
            // Underrun (EOF): zero the rest.
            for (int c = 0; c < ch; ++c)
                info.buffer->clear(c, info.startSample + produced, needed - produced);
            return;
        }

        const size_t got = stretcher_->retrieve(retrievePtrs_.data(), static_cast<size_t>(take));
        if (got == 0)
        {
            // Stretcher reported samples available but produced none — treat
            // as an underrun and zero-fill the remainder so the output buffer
            // does not contain stale data from a previous block.
            for (int c = 0; c < ch; ++c)
                info.buffer->clear(c, info.startSample + produced, needed - produced);
            return;
        }

        for (int c = 0; c < ch; ++c)
            info.buffer->copyFrom(c, info.startSample + produced,
                                  retrieveBuffer_, c, 0, static_cast<int>(got));
        produced += static_cast<int>(got);
    }
}

void RubberBandAudioSource::setNextReadPosition(juce::int64 newPosition)
{
    // Seek arrives on a managed thread; reset() is not safe against the audio
    // thread's process()/retrieve(), nor against releaseResources() freeing
    // the stretcher underneath us — and the move and the reset are one step.
    repositionInner([this, newPosition] {
        if (inner_ != nullptr) inner_->setNextReadPosition(newPosition);
    });
}

juce::int64 RubberBandAudioSource::getNextReadPosition() const
{
    return inner_ ? inner_->getNextReadPosition() : 0;
}

juce::int64 RubberBandAudioSource::getTotalLength() const
{
    return inner_ ? inner_->getTotalLength() : 0;
}

bool RubberBandAudioSource::isLooping() const
{
    return inner_ && inner_->isLooping();
}

void RubberBandAudioSource::setLooping(bool shouldLoop)
{
    if (inner_) inner_->setLooping(shouldLoop);
}

int RubberBandAudioSource::getReadAheadOutputSamples() const noexcept
{
    // Polled at up to 250 Hz from the managed position timer. It reads the
    // value the audio thread last published rather than calling into the
    // stretcher: the old `if (! stretcher_) ... stretcher_->available()` was a
    // check-then-use that releaseResources() could invalidate in between,
    // dereferencing a freed stretcher on a device change. Lock-free by design
    // — taking stretcherMutex_ here would let a 250 Hz poll contend the audio
    // thread's own block lock.
    if (bypass_.load(std::memory_order_acquire)) return 0;
    return readAheadOut_.load(std::memory_order_acquire);
}

} // namespace dotcharter
