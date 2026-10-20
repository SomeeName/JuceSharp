#pragma once

// A "Null" juce::AudioIODeviceType for device-independent testing.
//
// The null device accepts any requested configuration (sample rate, buffer
// size, channel counts up to kMaxChannels), spawns its own pacing thread and
// invokes the audio callback at the configured block cadence
// (blockSize / sampleRate) with silence input, discarding the output. That
// gives tests a deterministic callback pump with none of the WASAPI endpoint
// flake (busy/exclusive devices, missing inputs, driver-dependent timing).
//
// GUARD: the type is registered with an AudioDeviceManager ONLY when the
// engine was CREATED with DCA_ENGINE_CREATE_NULL_DEVICE (dca_engine_create2,
// ABI v41 — per engine) or the environment variable DCA_ENABLE_NULL_DEVICE=1
// is set at engine / device-manager init (process-wide; kept for tests and
// tools). Either source enables it; neither disables the other. It must NEVER
// appear in normal user-visible device lists — with both off,
// registerIfEnabled is a no-op and nothing else references the type.
//
// Prefer the flag: the env var is shared by every engine in the process, so a
// thread that set it around one engine's construction leaked the type into a
// concurrently rebuilt LIVE engine (DotCharter #85).
//
// Two registration gotchas, both load-bearing:
//  * createDeviceTypesIfNeeded() only creates JUCE's standard types when the
//    manager's type list is EMPTY. Registering the null type first would
//    silently suppress WASAPI/ASIO for that manager. registerIfEnabled
//    therefore forces standard-type creation (getAvailableDeviceTypes())
//    BEFORE appending — which also keeps the null type last, so it can never
//    become the default pick.
//  * The env var is read through juce::SystemStats::getEnvironmentVariable
//    (Win32 GetEnvironmentVariable), NOT std::getenv: the MSVC CRT caches the
//    environment at startup, so a managed test that sets the variable at
//    runtime (System.Environment.SetEnvironmentVariable) would be invisible
//    to getenv.

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace dotcharter {
namespace nulldev {

inline constexpr const char* kTypeName    = "Null";
inline constexpr const char* kDeviceName  = "Null Audio Device";
inline constexpr const char* kEnvVar      = "DCA_ENABLE_NULL_DEVICE";
inline constexpr int         kMaxChannels = 8;

inline constexpr const char* kManualEnvVar = "DCA_NULL_DEVICE_MANUAL";

/// True when DCA_ENABLE_NULL_DEVICE=1 in the *current* process environment.
/// Evaluated at registration time (engine start / mirror open), so a test can
/// set the variable just before creating an engine.
inline bool isEnabled()
{
    return juce::SystemStats::getEnvironmentVariable(kEnvVar, {}) == "1";
}

/// MANUAL PUMP MODE (native RT gate only). With DCA_NULL_DEVICE_MANUAL=1 (or
/// DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL on the engine that owns the type) the
/// null device does NOT spawn its pacing thread: start() prepares the callback
/// and returns, and the caller drives blocks synchronously through pump(). The
/// whole point is that the audio callback then runs on the *calling* thread, so
/// the RT sentinel's thread-local "we are on the audio thread" flag is exact
/// and the run is deterministic (no sleeping, no timing flake).
///
/// Read the same way as kEnvVar (GetEnvironmentVariable, not getenv) so a
/// runtime-set variable is visible. Never true in a shipped app: it also
/// requires DCA_ENABLE_NULL_DEVICE=1 for the type to exist at all.
inline bool isManual()
{
    return juce::SystemStats::getEnvironmentVariable(kManualEnvVar, {}) == "1";
}

/// The device: accepts whatever open() asks for, then paces the callback from
/// its own thread at blockSize/sampleRate with zeroed input buffers.
class NullAudioIODevice final : public juce::AudioIODevice
{
public:
    explicit NullAudioIODevice(bool manualByConfig = false)
        : juce::AudioIODevice(kDeviceName, kTypeName), manualByConfig_(manualByConfig) {}

    ~NullAudioIODevice() override
    {
        close();
    }

    juce::StringArray getOutputChannelNames() override { return channelNames("Null Out"); }
    juce::StringArray getInputChannelNames()  override { return channelNames("Null In"); }

    juce::Array<double> getAvailableSampleRates() override
    {
        return { 8000.0, 11025.0, 16000.0, 22050.0, 32000.0, 44100.0, 48000.0,
                 88200.0, 96000.0, 176400.0, 192000.0 };
    }

    juce::Array<int> getAvailableBufferSizes() override
    {
        return { 16, 32, 64, 128, 256, 441, 480, 512, 960, 1024, 2048, 4096 };
    }

    int getDefaultBufferSize() override { return 512; }

    juce::String open(const juce::BigInteger& inputChannels,
                      const juce::BigInteger& outputChannels,
                      double sampleRate,
                      int bufferSizeSamples) override
    {
        close();

        sampleRate_ = sampleRate        > 0.0 ? sampleRate        : 48000.0;
        blockSize_  = bufferSizeSamples > 0   ? bufferSizeSamples : 512;

        activeIn_.clear();
        activeOut_.clear();
        for (int i = 0; i < kMaxChannels; ++i)
        {
            if (inputChannels[i])  activeIn_.setBit(i);
            if (outputChannels[i]) activeOut_.setBit(i);
        }
        numIn_  = activeIn_.countNumberOfSetBits();
        numOut_ = activeOut_.countNumberOfSetBits();

        auto sizeBufs = [this](std::vector<std::vector<float>>& bufs,
                               std::vector<float*>& ptrs, int channels)
        {
            bufs.assign((size_t) channels, std::vector<float>((size_t) blockSize_, 0.0f));
            ptrs.resize((size_t) channels);
            for (int c = 0; c < channels; ++c) ptrs[(size_t) c] = bufs[(size_t) c].data();
        };
        sizeBufs(inBufs_,  inPtrs_,  numIn_);
        sizeBufs(outBufs_, outPtrs_, numOut_);

        isOpen_ = true;
        return {};
    }

    void close() override
    {
        stop();
        isOpen_ = false;
    }

    bool isOpen() override { return isOpen_; }

    void start(juce::AudioIODeviceCallback* callback) override
    {
        if (! isOpen_ || callback == nullptr || playing_.load(std::memory_order_acquire))
            return;

        callback_ = callback;
        callback_->audioDeviceAboutToStart(this);

        stopFlag_.store(false, std::memory_order_release);
        manual_ = manualByConfig_ || isManual();
        if (! manual_)
            thread_ = std::thread([this] { run(); });
        playing_.store(true, std::memory_order_release);
        if (manual_)
            currentManualDevice().store(this, std::memory_order_release);
    }

    /// Manual mode only: run the device callback `n` times synchronously on the
    /// CALLING thread. Returns the number of blocks actually rendered (0 when
    /// the device is not in manual mode or not started).
    int pumpBlocks(int n) noexcept
    {
        if (! manual_ || callback_ == nullptr || ! playing_.load(std::memory_order_acquire))
            return 0;
        int done = 0;
        for (int i = 0; i < n; ++i, ++done)
            renderOneBlock();
        return done;
    }

    /// Manual mode only: the device's input scratch, so a test can install a
    /// deterministic stimulus. Written once and left alone — the callback
    /// contract treats input as const, and run()/pumpBlocks only clear OUTPUT.
    std::vector<std::vector<float>>& manualInputBuffers() noexcept { return inBufs_; }
    /// Manual mode only: the output the callback just wrote (discarded by a
    /// real device). Lets a test assert the engine is still making sound.
    const std::vector<std::vector<float>>& manualOutputBuffers() const noexcept { return outBufs_; }

    void stop() override
    {
        if (! playing_.exchange(false, std::memory_order_acq_rel))
            return;

        auto* expected = this;
        currentManualDevice().compare_exchange_strong(expected, nullptr,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_relaxed);
        manual_ = false;

        stopFlag_.store(true, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();

        if (auto* cb = callback_)
        {
            callback_ = nullptr;
            cb->audioDeviceStopped();
        }
    }

    /// The single live manual-mode device, published for the free pump()
    /// helper below. Only ever set in manual mode, so a shipped app (which can
    /// never enter manual mode) leaves it permanently null.
    static std::atomic<NullAudioIODevice*>& currentManualDevice() noexcept
    {
        static std::atomic<NullAudioIODevice*> d{ nullptr };
        return d;
    }

    bool isPlaying() override { return playing_.load(std::memory_order_acquire); }

    juce::String     getLastError() override                  { return {}; }
    int              getCurrentBufferSizeSamples() override   { return blockSize_; }
    double           getCurrentSampleRate() override          { return sampleRate_; }
    int              getCurrentBitDepth() override            { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOut_; }
    juce::BigInteger getActiveInputChannels()  const override { return activeIn_; }
    int              getOutputLatencyInSamples() override     { return 0; }
    int              getInputLatencyInSamples() override      { return 0; }

private:
    static juce::StringArray channelNames(const char* prefix)
    {
        juce::StringArray names;
        for (int i = 1; i <= kMaxChannels; ++i)
            names.add(juce::String(prefix) + " " + juce::String(i));
        return names;
    }

    /// The pacing thread: one callback per blockSize/sampleRate period on an
    /// absolute steady_clock schedule (no cumulative drift). If the thread
    /// stalls (debugger, suspended process) it resyncs instead of
    /// machine-gunning the backlog.
    /// One device callback. Shared by the pacing thread and the manual pump so
    /// both drive byte-identical work.
    void renderOneBlock() noexcept
    {
        static const juce::AudioIODeviceCallbackContext ctx{};

        // Output is scratch (discarded), but hand the callback a clean
        // buffer each block; input buffers stay zeroed (silence) — the
        // callback contract treats them as const.
        for (auto& ch : outBufs_)
            std::fill(ch.begin(), ch.end(), 0.0f);

        callback_->audioDeviceIOCallbackWithContext(
            numIn_  > 0 ? const_cast<const float**>(inPtrs_.data()) : nullptr, numIn_,
            numOut_ > 0 ? outPtrs_.data() : nullptr, numOut_,
            blockSize_, ctx);
    }

    void run()
    {
        const auto blockNs = std::chrono::nanoseconds(
            (long long) std::llround(1.0e9 * (double) blockSize_ / sampleRate_));
        auto next = std::chrono::steady_clock::now();

        while (! stopFlag_.load(std::memory_order_acquire))
        {
            renderOneBlock();

            next += blockNs;
            const auto now = std::chrono::steady_clock::now();
            if (next < now - std::chrono::milliseconds(100))
                next = now; // stalled — resync rather than burst-catch-up
            std::this_thread::sleep_until(next);
        }
    }

    double           sampleRate_ = 48000.0;
    int              blockSize_  = 512;
    int              numIn_      = 0;
    int              numOut_     = 0;
    juce::BigInteger activeIn_, activeOut_;

    std::vector<std::vector<float>> inBufs_, outBufs_;
    std::vector<float*>             inPtrs_, outPtrs_;

    juce::AudioIODeviceCallback* callback_ = nullptr;
    std::thread                  thread_;
    std::atomic<bool>            stopFlag_{ false };
    std::atomic<bool>            playing_{ false };
    bool                         isOpen_ = false;
    /// Latched from (manualByConfig_ || isManual()) at start(). False in every
    /// shipped configuration.
    bool                         manual_ = false;
    /// DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL of the owning engine (ABI v41).
    const bool                   manualByConfig_;
};

/// Drive `n` blocks synchronously through the live manual-mode null device.
/// Returns the number of blocks rendered; 0 when no manual device is started.
inline int pump(int n) noexcept
{
    auto* d = NullAudioIODevice::currentManualDevice().load(std::memory_order_acquire);
    return d != nullptr ? d->pumpBlocks(n) : 0;
}

/// The live manual-mode device, or nullptr. Used by the RT gate to install a
/// stimulus and to read back the rendered output.
inline NullAudioIODevice* manualDevice() noexcept
{
    return NullAudioIODevice::currentManualDevice().load(std::memory_order_acquire);
}

class NullAudioIODeviceType final : public juce::AudioIODeviceType
{
public:
    explicit NullAudioIODeviceType(bool manualByConfig = false)
        : juce::AudioIODeviceType(kTypeName), manualByConfig_(manualByConfig) {}

    void scanForDevices() override {}

    juce::StringArray getDeviceNames(bool /*wantInputNames*/) const override
    {
        return { kDeviceName };
    }

    int getDefaultDeviceIndex(bool /*forInput*/) const override { return 0; }

    int getIndexOfDevice(juce::AudioIODevice* device, bool /*asInput*/) const override
    {
        return (device != nullptr && device->getName() == kDeviceName) ? 0 : -1;
    }

    bool hasSeparateInputsAndOutputs() const override { return false; }

    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName,
                                      const juce::String& inputDeviceName) override
    {
        if (outputDeviceName == kDeviceName || inputDeviceName == kDeviceName)
            return new NullAudioIODevice(manualByConfig_);
        return nullptr;
    }

private:
    const bool manualByConfig_;
};

/// Append the Null type to `dm` iff the owning engine was created with the
/// Null-device flag (`enabledByConfig`) OR DCA_ENABLE_NULL_DEVICE=1. Idempotent.
/// `manualByConfig` puts the devices this type creates in manual-pump mode.
/// Forces standard-type creation first (see the header comment: a manager
/// whose FIRST type arrives via addAudioDeviceType never creates the
/// standard ones), which also guarantees the null type sits LAST in the
/// list and can never be picked as the default.
inline void registerIfEnabled(juce::AudioDeviceManager& dm,
                              bool enabledByConfig = false,
                              bool manualByConfig  = false)
{
    if (! enabledByConfig && ! isEnabled())
        return;
    for (auto* t : dm.getAvailableDeviceTypes())
        if (t != nullptr && t->getTypeName() == kTypeName)
            return; // already registered
    dm.addAudioDeviceType(std::make_unique<NullAudioIODeviceType>(manualByConfig));
}

} // namespace nulldev
} // namespace dotcharter
