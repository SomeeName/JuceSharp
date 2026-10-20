#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>
#include <limits>

namespace dotcharter {

/// Stereo constant-power pan wrapper around any AudioSource.
/// pan = -1.0 (full left) .. 0.0 (centre) .. +1.0 (full right).
class PanningAudioSource : public juce::AudioSource
{
public:
    PanningAudioSource(juce::AudioSource* inner, bool deleteWhenRemoved)
        : inner_(inner), deleteInner_(deleteWhenRemoved) {}

    ~PanningAudioSource() override
    {
        releaseResources();
        if (deleteInner_ && inner_ != nullptr) delete inner_;
        inner_ = nullptr;
    }

    void prepareToPlay(int blockSize, double sampleRate) override
    {
        if (inner_) inner_->prepareToPlay(blockSize, sampleRate);
        // A fresh device session starts AT the published pan rather than
        // fading up to it — the same reseed prepareMasterBus does for the
        // engine's own faders.
        primed_ = false;
    }

    void releaseResources() override
    {
        if (inner_) inner_->releaseResources();
    }

    void setPan(float p) noexcept
    {
        if (p < -1.0f) p = -1.0f;
        if (p >  1.0f) p =  1.0f;
        pan_.store(p, std::memory_order_relaxed);
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        if (inner_ == nullptr || info.buffer == nullptr || info.numSamples <= 0)
        {
            if (info.buffer) info.clearActiveBufferRegion();
            return;
        }
        inner_->getNextAudioBlock(info);

        const float p = pan_.load(std::memory_order_relaxed);

        // Equal-power: angle = (p + 1) * pi/4 ; left = cos, right = sin.
        //
        // p3-38: sin/cos are computed once per block, but the result only
        // depends on pan_, which the audio thread sees change rarely (UI
        // tweaks). Cache the last (p, lGain, rGain) triple so steady-state
        // blocks skip the transcendentals entirely.
        float lGain = lastLGain_;
        float rGain = lastRGain_;
        if (p != lastPan_)
        {
            const float angle = (p + 1.0f) * 0.25f * 3.14159265f;
            lGain = std::cos(angle) * 1.41421356f;
            rGain = std::sin(angle) * 1.41421356f;
            lastPan_   = p;
            lastLGain_ = lGain;
            lastRGain_ = rGain;
        }

        // RAMPED ACROSS THE BLOCK, never applied as a constant. A pan slider is
        // dragged, so one constant multiply per block steps the waveform at
        // every block boundary — the classic zipper, and the one
        // AudioEngine::rampOrScale exists to prevent for the master, monitor
        // and input faders. This source was the last user-facing level that
        // still stepped. (There used to be an early `return` for |p| < 1e-4 as
        // well, which turned a drag back to centre into a CUT from the panned
        // gains to unity in one sample.)
        //
        // W4-N20: THE CENTRE GAINS ARE NOT EXACTLY 1.0f, AND THIS COMMENT USED
        // TO SAY THEY WERE. Measured by RtSourcePanCentreIsUnity: at p == 0,
        // `cosf(0.25f * 3.14159265f) * 1.41421356f` is 0x3F7FFFFF — one ulp
        // BELOW unity — for both channels. Three roundings with a truncated pi
        // and a truncated sqrt(2) were never going to land on a power of two,
        // and "the gains are exactly 1.0 at p == 0" was an assumption about
        // rounding asserted as a property, which is the same shape of reasoning
        // that made the shortcut this one replaced wrong.
        //
        // The consequence was benign — a `!= 1.0f` test that never fires just
        // means a centred source paid a full `applyGain` pass per block — but
        // it meant the shortcut did not exist. `kUnityEps` restores it. 1e-6 is
        // 0.0000087 dB, i.e. far below anything audible and far above the ulp,
        // so the only level changes it swallows are ones nothing could hear.
        // The RAMP comparisons stay EXACT: a ramp between two nearly equal
        // gains is a no-op anyway, and an epsilon there would be one more
        // threshold to reason about for nothing.
        static constexpr float kUnityEps = 1.0e-6f;
        const auto isUnity = [](float g) noexcept { return std::abs(g - 1.0f) <= kUnityEps; };
        if (! primed_) { appliedL_ = lGain; appliedR_ = rGain; primed_ = true; }

        const int nch = info.buffer->getNumChannels();
        if (nch >= 2)
        {
            if (appliedL_ != lGain)
                info.buffer->applyGainRamp(0, info.startSample, info.numSamples, appliedL_, lGain);
            else if (! isUnity(lGain))
                info.buffer->applyGain(0, info.startSample, info.numSamples, lGain);

            if (appliedR_ != rGain)
                info.buffer->applyGainRamp(1, info.startSample, info.numSamples, appliedR_, rGain);
            else if (! isUnity(rGain))
                info.buffer->applyGain(1, info.startSample, info.numSamples, rGain);
        }
        else if (nch == 1)
        {
            const float from = (appliedL_ + appliedR_) * 0.5f;
            const float to   = (lGain + rGain) * 0.5f;
            if (from != to)
                info.buffer->applyGainRamp(0, info.startSample, info.numSamples, from, to);
            else if (! isUnity(to))
                info.buffer->applyGain(0, info.startSample, info.numSamples, to);
        }

        appliedL_ = lGain;
        appliedR_ = rGain;
    }

private:
    juce::AudioSource* inner_;
    bool               deleteInner_;
    std::atomic<float> pan_ { 0.0f };
    // p3-38: per-block cache for the equal-power gains. Audio thread only.
    float              lastPan_   { std::numeric_limits<float>::quiet_NaN() };
    float              lastLGain_ { 1.0f };
    float              lastRGain_ { 1.0f };
    // Where the PREVIOUS block's ramp ended. Audio thread only.
    float              appliedL_  { 1.0f };
    float              appliedR_  { 1.0f };
    bool               primed_    { false };
};

} // namespace dotcharter
