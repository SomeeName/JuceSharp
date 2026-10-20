#include "MirrorOutput.h"

#include "NullAudioIODevice.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace dotcharter {

MirrorOutput::MirrorOutput()
{
    ring_.assign(static_cast<size_t>(kFifoFrames) * 2u, 0.0f);
}

MirrorOutput::~MirrorOutput()
{
    close();
}

juce::String MirrorOutput::open(const juce::String& deviceName, double engineSampleRate)
{
    close();
    if (deviceName.isEmpty()) return {};

    // Publish the producer (engine) rate BEFORE the callback can fire so
    // audioDeviceAboutToStart configures the resampler for the right ratio.
    if (engineSampleRate > 0.0)
        engineRate_.store(engineSampleRate, std::memory_order_release);

    // close() dropped started_, but a pushBlock() on the primary audio thread
    // that loaded started_==true just before that could still be mid-write.
    // Resetting the FIFO indices under a concurrent prepareToWrite/
    // finishedWrite is a data race on the AbstractFifo cursors (audit #7) —
    // wait the (at most one-block, microseconds-long) writer out first.
    // pushBlock re-checks started_ AFTER bumping pushBusy_, and both sides
    // are seq_cst, so once we observe pushBusy_ == 0 here no writer that saw
    // the mirror as started can still be inside the FIFO. Bounded defensively;
    // the writer's critical section cannot block, so the bound never trips.
    for (int spin = 0; pushBusy_.load() != 0 && spin < 1'000'000; ++spin)
        std::this_thread::yield();

    fifo_.reset();
    std::fill(ring_.begin(), ring_.end(), 0.0f);

    // Null test device (guarded by the owning engine's
    // DCA_ENGINE_CREATE_NULL_DEVICE flag or DCA_ENABLE_NULL_DEVICE=1 — see
    // NullAudioIODevice.h): when a test mirrors to the null device by name,
    // register + select the Null type. Unreachable in normal runs: with the
    // guard off registerIfEnabled is a no-op and the name check fails anyway.
    if ((nullDeviceByConfig_ || nulldev::isEnabled()) && deviceName == nulldev::kDeviceName)
    {
        nulldev::registerIfEnabled(dm_, nullDeviceByConfig_);
        dm_.setCurrentAudioDeviceType(nulldev::kTypeName, true);
    }
    else
    {
        // Force a WASAPI / shared-mode driver. ASIO would deadlock against the
        // primary engine on the same hardware; the mirror is intended for virtual
        // cables (VB-Cable, VoiceMeeter), system speakers, or headphones.
        for (auto* t : dm_.getAvailableDeviceTypes())
        {
            if (t == nullptr) continue;
            const auto name = t->getTypeName();
            if (name.containsIgnoreCase("Windows Audio") || name.containsIgnoreCase("WASAPI"))
            {
                dm_.setCurrentAudioDeviceType(name, true);
                break;
            }
        }
    }

    // setCurrentAudioDeviceType does not eagerly scan the type's device list.
    // Without an explicit scan, setAudioDeviceSetup() below fails with
    // "No such device" because the type's device-name array is still empty.
    // AudioEngine::start() does the same scan for the same reason.
    if (auto* curType = dm_.getCurrentDeviceTypeObject())
    {
        try { curType->scanForDevices(); } catch (...) {}
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    dm_.getAudioDeviceSetup(setup);
    setup.outputDeviceName         = deviceName;
    setup.inputDeviceName          = {};
    setup.useDefaultOutputChannels = true;
    setup.useDefaultInputChannels  = true;
    // Prefer matching the engine rate (cheapest ratio, best quality), but the
    // async resampler copes with whatever the device actually opens at, so this
    // is only an optimisation — see the actual-rate read in
    // audioDeviceAboutToStart. A bufferSize of 0 lets JUCE pick the device pref.
    setup.sampleRate = (engineSampleRate > 0.0) ? engineSampleRate : 0.0;
    setup.bufferSize = 0;

    auto err = dm_.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty())
    {
        dm_.closeAudioDevice();
        return err;
    }

    dm_.addAudioCallback(this);
    started_.store(true, std::memory_order_release);
    return {};
}

void MirrorOutput::close()
{
    // Drop the started_ flag first so pushBlock() (called from the primary
    // engine audio thread) bails out before touching the FIFO. Then remove
    // our callback — JUCE serializes against any in-flight mirror-device
    // callback before returning — and close the device.
    //
    // Deliberately do NOT call fifo_.reset() here: pushBlock() from the
    // primary engine thread can run concurrently with close() (it is not
    // synchronised with the mirror device's audio thread) and resetting
    // the FIFO indices while pushBlock is mid-write would race. The next
    // open() resets the FIFO before re-enabling pushBlock anyway.
    started_.store(false, std::memory_order_release);
    dm_.removeAudioCallback(this);
    dm_.closeAudioDevice();
}

void MirrorOutput::setEngineSampleRate(double engineSampleRate) noexcept
{
    if (engineSampleRate > 0.0)
        engineRate_.store(engineSampleRate, std::memory_order_release);
}

void MirrorOutput::pushBlock(const float* const* out, int numChannels, int numSamples) noexcept
{
    if (!started_.load(std::memory_order_acquire) || numChannels <= 0 || numSamples <= 0 || out == nullptr)
        return;

    const float* L = out[0];
    const float* R = (numChannels >= 2 && out[1] != nullptr) ? out[1] : L;
    if (L == nullptr) return;

    // Announce the write, then RE-CHECK started_ (both seq_cst — see the
    // pushBusy_ comment in the header): either we see close()'s flag drop and
    // bail here, or open()'s reset sees pushBusy_ > 0 and waits for us. One
    // uncontended RMW per block; the audio thread never spins or blocks.
    pushBusy_.fetch_add(1);
    if (!started_.load())
    {
        pushBusy_.fetch_sub(1);
        return;
    }

    const int free    = fifo_.getFreeSpace();
    const int toWrite = std::min(free, numSamples);
    if (toWrite > 0) // else: mirror device is behind — drop newest block; primary path keeps real-time
    {
        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        fifo_.prepareToWrite(toWrite, s1, n1, s2, n2);

        if (n1 > 0)
        {
            for (int i = 0; i < n1; ++i)
            {
                ring_[2 * (s1 + i) + 0] = L[i];
                ring_[2 * (s1 + i) + 1] = R[i];
            }
        }
        if (n2 > 0)
        {
            for (int i = 0; i < n2; ++i)
            {
                ring_[2 * (s2 + i) + 0] = L[n1 + i];
                ring_[2 * (s2 + i) + 1] = R[n1 + i];
            }
        }

        fifo_.finishedWrite(n1 + n2);
    }

    // SEQ_CST, matching the bail path above and the open-side spin. The release
    // form was enough for the FIFO data, but open()'s wait reasons about
    // pushBusy_ and started_ TOGETHER — "once we observe pushBusy_ == 0 here no
    // writer that saw the mirror as started can still be inside the FIFO" is a
    // claim about a total order over two atomics, and that comment says in as
    // many words that both sides are seq_cst. One of the two decrements
    // disagreed with it.
    pushBusy_.fetch_sub(1);
}

void MirrorOutput::audioDeviceIOCallbackWithContext(const float* const*,
                                                    int,
                                                    float* const* out,
                                                    int numOutput,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    const juce::ScopedNoDenormals noDenormals;

    // Validate BEFORE walking `out` — the clear loop used to run above this
    // guard, so it indexed out[ch] on arguments the guard exists to reject.
    if (out == nullptr || numOutput <= 0 || numSamples <= 0) return;

    for (int ch = 0; ch < numOutput; ++ch)
        if (out[ch] != nullptr) std::fill(out[ch], out[ch] + numSamples, 0.0f);

    // Apply an engine-rate change published from the engine thread (device
    // switch / renegotiation). Rebuild the pre-roll and reset the resamplers;
    // no allocation — scratch was sized for the worst-case ratio at start.
    const double er = engineRate_.load(std::memory_order_acquire);
    if (er != appliedEngineRate_)
    {
        appliedEngineRate_ = er;
        clock_.configure(er, deviceRate_, MirrorClockController::targetFramesForRate(er));
        interp_[0].reset();
        interp_[1].reset();
    }

    // Never process more frames than the buffers were sized for (WASAPI shared
    // mode is fixed-size, so this guard practically never trips).
    const int outFrames = std::min(numSamples, maxBlock_);
    if (outFrames <= 0) return;

    const int avail = fifo_.getNumReady();
    const MirrorClockDecision d = clock_.step(avail, outFrames);
    if (!d.produce)
        return; // priming or an underrun gap — silence already written

    int needed = std::min(d.inputFramesNeeded, scratchCapacity_);
    needed = std::min(needed, avail);
    if (needed <= 0) return;

    // Peek `needed` engine frames from the FIFO into deinterleaved scratch
    // WITHOUT committing — we only advance the read pointer by the frames the
    // interpolator actually consumes, so the fractional remainder stays queued
    // for the next block (exact resampling continuity).
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    fifo_.prepareToRead(needed, s1, n1, s2, n2);

    for (int i = 0; i < n1; ++i)
    {
        scratchL_[i] = ring_[2 * (s1 + i) + 0];
        scratchR_[i] = ring_[2 * (s1 + i) + 1];
    }
    for (int i = 0; i < n2; ++i)
    {
        scratchL_[n1 + i] = ring_[2 * (s2 + i) + 0];
        scratchR_[n1 + i] = ring_[2 * (s2 + i) + 1];
    }

    const double ratio = d.effectiveRatio;
    const int usedL = interp_[0].process(ratio, scratchL_.data(), outL_.data(), outFrames);
    const int usedR = interp_[1].process(ratio, scratchR_.data(), outR_.data(), outFrames);

    // Both channels share ratio + phase history, so they consume the same
    // count; take the max defensively and never exceed what we peeked.
    int used = std::max(usedL, usedR);
    used = std::min(used, n1 + n2);

    float* dl = out[0];
    float* dr = (numOutput >= 2) ? out[1] : nullptr;
    if (dr != nullptr)
    {
        if (dl != nullptr) std::copy(outL_.data(), outL_.data() + outFrames, dl);
        std::copy(outR_.data(), outR_.data() + outFrames, dr);
    }
    else if (dl != nullptr)
    {
        // Down-mix for a MONO device. A device with MORE than two outputs
        // leaves channels 2..n untouched, which is correct — the mirror is a
        // stereo programme feed and writing silence into a channel somebody
        // else's routing owns would be worse — but it was undocumented, and a
        // reader here could not otherwise tell it from an oversight.
        for (int i = 0; i < outFrames; ++i)
            dl[i] = 0.5f * (outL_[i] + outR_[i]);
    }

    fifo_.finishedRead(used);
}

void MirrorOutput::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    deviceRate_ = (device != nullptr && device->getCurrentSampleRate() > 0.0)
                      ? device->getCurrentSampleRate()
                      : 48000.0;
    const int reported = (device != nullptr) ? device->getCurrentBufferSizeSamples() : 1024;
    maxBlock_ = std::max(64, reported);

    const double er = engineRate_.load(std::memory_order_acquire);
    appliedEngineRate_ = er;

    // Size the deinterleaved input scratch for the worst-case resample ratio so
    // a later engine-rate bump (up to 192k) never reallocates on the audio
    // thread. inputPerBlock ~= outFrames * ratio; add the interpolator guard.
    const double worstRatio = std::max(er, 192000.0) / std::max(1.0, deviceRate_);
    scratchCapacity_ = static_cast<int>(std::ceil(static_cast<double>(maxBlock_) * worstRatio))
                     + MirrorClockController::kInterpGuard + 8;

    scratchL_.assign(static_cast<size_t>(scratchCapacity_), 0.0f);
    scratchR_.assign(static_cast<size_t>(scratchCapacity_), 0.0f);
    outL_.assign(static_cast<size_t>(maxBlock_), 0.0f);
    outR_.assign(static_cast<size_t>(maxBlock_), 0.0f);

    interp_[0].reset();
    interp_[1].reset();
    clock_.configure(er, deviceRate_, MirrorClockController::targetFramesForRate(er));
}

void MirrorOutput::audioDeviceStopped()
{
    clock_.reset();
}

} // namespace dotcharter
