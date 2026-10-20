#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "MirrorClockController.h"

#include <atomic>
#include <vector>

namespace dotcharter {

/// Parallel WASAPI output that mirrors the master mix (including the live
/// post-VST guitar signal) to a chosen device so OBS / Discord / Twitch can
/// capture song + processed guitar via a virtual cable even while ASIO drives
/// low-latency monitoring on the primary device.
///
/// The mirror device runs on its OWN audio clock, independent of the primary
/// engine. The engine thread pushes the master mix into a lock-free FIFO at the
/// ENGINE rate; the mirror device callback drains it at the DEVICE rate through
/// a per-channel resampler driven by `MirrorClockController` (target-latency
/// pre-roll + fill-locked drift trim). This is a full asynchronous sample-rate
/// conversion, so a rate mismatch (48k engine vs a 44.1k virtual cable) or
/// crystal drift no longer produces the over/underrun crackle it used to.
class MirrorOutput : public juce::AudioIODeviceCallback
{
public:
    MirrorOutput();
    ~MirrorOutput() override;

    /// Open the named WASAPI device, resampling the mirror stream from
    /// `engineSampleRate` (the primary engine's device rate) to whatever rate
    /// the mirror device actually opens at. Returns "" on success, the JUCE
    /// error string on failure. Call with an empty name to leave the mirror
    /// closed.
    juce::String open(const juce::String& deviceName, double engineSampleRate);

    /// Close the mirror device. Idempotent.
    void close();

    /// ABI v41: the owning engine was created with DCA_ENGINE_CREATE_NULL_DEVICE,
    /// so mirroring to the Null device by name is allowed without the env var.
    /// Set once before the engine is published; read by open().
    void setNullDeviceOptions(bool enable) noexcept { nullDeviceByConfig_ = enable; }

    bool isOpen() const noexcept { return started_.load(std::memory_order_acquire); }

    /// Notify the mirror that the primary engine is now running at
    /// `engineSampleRate` (the FIFO producer rate). Safe to call from the
    /// engine's device-prepare context; the mirror audio thread applies the
    /// change on its next callback. No-op when the rate is unchanged.
    void setEngineSampleRate(double engineSampleRate) noexcept;

    /// Called from the PRIMARY engine audio thread once per block, AFTER the
    /// master bus + input monitor mix has been written into out[]. Lock-free.
    /// Pushes ENGINE-rate frames; the consumer resamples to the device rate.
    void pushBlock(const float* const* out, int numChannels, int numSamples) noexcept;

    // AudioIODeviceCallback (drives the mirror device).
    void audioDeviceIOCallbackWithContext(const float* const* in,
                                          int numInput,
                                          float* const* out,
                                          int numOutput,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& ctx) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    static constexpr int kFifoFrames = 1 << 16; // 65 536 stereo frames (~1.36 s @ 48k) — headroom above the pre-roll

    juce::AudioDeviceManager dm_;
    juce::AbstractFifo       fifo_{ kFifoFrames };
    std::vector<float>       ring_; // 2 floats per frame (stereo interleaved), ENGINE rate

    // Consumer-side resampling state. Touched only on the mirror audio thread
    // (allocated in audioDeviceAboutToStart, never on the callback hot path).
    MirrorClockController    clock_;
    juce::LagrangeInterpolator interp_[2];          // one per channel (stereo)
    std::vector<float>       scratchL_, scratchR_;  // deinterleaved engine-rate input
    std::vector<float>       outL_, outR_;          // resampled device-rate output (pre-distribute)
    int                      scratchCapacity_ = 0;
    int                      maxBlock_        = 0;
    double                   deviceRate_      = 48000.0;
    double                   appliedEngineRate_ = 48000.0; // last engine rate applied to clock_ (mirror thread)

    // Engine (producer) rate, published by open()/setEngineSampleRate() on a
    // non-audio thread and consumed on the mirror audio thread.
    std::atomic<double>      engineRate_{ 48000.0 };
    std::atomic<bool>        started_{ false };
    bool                     nullDeviceByConfig_ = false;

    // In-flight pushBlock() count (0 or 1 — single producer). open() resets
    // the FIFO/ring, and a pushBlock that passed the started_ check before
    // close() dropped the flag could still be mid-write during that reset
    // (2026-07-16 audit #7). Writers bump this AROUND the write and re-check
    // started_ after bumping; open() drops started_ (via close) and then
    // waits for this to reach 0 before touching the FIFO. Both sides use
    // seq_cst so the Dekker-style "either the writer sees started_==false or
    // the resetter sees pushBusy_>0" holds.
    std::atomic<int>         pushBusy_{ 0 };
};

} // namespace dotcharter
