#include "AudioEngine.h"
#include "NullAudioIODevice.h"
#include "Utf8Truncate.h"
#include "RubberBandAudioSource.h"
#include "PanningAudioSource.h"
#include "wem/WemAudioFormat.h"
#include "DeviceRateWav.h"
#include "dsp/voice/dcvoice_pack.h"

#include <rubberband/RubberBandLiveShifter.h>

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

namespace dotcharter {

namespace {
    // Where AudioEngine's own diagnostics go. Installed by the ABI translation
    // unit (dotcharter_audio.cpp) at static-init time; a null sink is a no-op,
    // which is what the native test executables run with.
    std::atomic<AudioEngine::LogSink> g_engineLogSink{ nullptr };
}

void AudioEngine::setLogSink(LogSink sink) noexcept
{
    g_engineLogSink.store(sink, std::memory_order_release);
}

void AudioEngine::engineLog(int level, const juce::String& message) noexcept
{
    // MESSAGE THREAD / DEVICE THREAD ONLY. Nothing on the audio callback may
    // call this: the sink crosses into managed code.
    if (auto sink = g_engineLogSink.load(std::memory_order_acquire))
    {
        try { sink(level, message.toRawUTF8()); } catch (...) {}
    }
}


/// Minimal PositionableAudioSource adapter around a MixerAudioSource so
/// we can wrap it in a RubberBandAudioSource (which requires a positionable
/// inner). The stem group has no canonical timeline of its own — every
/// contained TransportSource drives its own AudioTransportSource — so all
/// positionable methods are degenerate.
class StemPositionableSource : public juce::PositionableAudioSource
{
public:
    explicit StemPositionableSource(juce::MixerAudioSource* inner) : inner_(inner) {}

    void prepareToPlay(int blockSize, double sampleRate) override
    {
        if (inner_ != nullptr) inner_->prepareToPlay(blockSize, sampleRate);
    }
    void releaseResources() override
    {
        if (inner_ != nullptr) inner_->releaseResources();
    }
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        if (inner_ != nullptr) inner_->getNextAudioBlock(info);
        else if (info.buffer != nullptr) info.clearActiveBufferRegion();
    }
    void        setNextReadPosition(juce::int64) override {}
    juce::int64 getNextReadPosition()    const override { return 0; }
    juce::int64 getTotalLength()         const override { return std::numeric_limits<juce::int64>::max(); }
    bool        isLooping()              const override { return false; }
    void        setLooping(bool) override {}

private:
    juce::MixerAudioSource* inner_;
};


namespace {

dca_driver typeToDriver(const juce::String& typeName)
{
    if (typeName.containsIgnoreCase("ASIO"))           return DCA_DRIVER_ASIO;
    if (typeName.containsIgnoreCase("Windows Audio"))  return DCA_DRIVER_WASAPI;
    if (typeName.containsIgnoreCase("WASAPI"))         return DCA_DRIVER_WASAPI;
    if (typeName.containsIgnoreCase("DirectSound"))    return DCA_DRIVER_DIRECTSOUND;
    if (typeName.containsIgnoreCase("CoreAudio"))      return DCA_DRIVER_COREAUDIO;
    if (typeName.containsIgnoreCase("ALSA"))           return DCA_DRIVER_ALSA;
    if (typeName.containsIgnoreCase("JACK"))           return DCA_DRIVER_JACK;
    return DCA_DRIVER_DEFAULT;
}

const char* driverToTypeName(dca_driver driver)
{
    switch (driver)
    {
        case DCA_DRIVER_ASIO:        return "ASIO";
        case DCA_DRIVER_WASAPI:      return "Windows Audio";
        case DCA_DRIVER_DIRECTSOUND: return "DirectSound";
        case DCA_DRIVER_COREAUDIO:   return "CoreAudio";
        case DCA_DRIVER_ALSA:        return "ALSA";
        case DCA_DRIVER_JACK:        return "JACK";
        default:                     return nullptr;
    }
}

void copyTruncated(char* dest, size_t cap, const juce::String& src)
{
    if (dest == nullptr || cap == 0) return;
    const char* utf8 = src.toRawUTF8();
    // Manual bounded copy avoids the MSVC C4996 deprecation of std::strncpy
    // without requiring the non-portable strncpy_s — and it stops at a CODE
    // POINT boundary, not a byte one. dca_device_info writes the same device
    // name into id[128] and name[256], so a byte-wise cut tears the id of any
    // device whose 127th byte lands mid-sequence while leaving the name whole:
    // the two then disagree and selection by id falls through to the fallback
    // walk. See Utf8Truncate.h.
    const size_t len = std::strlen(utf8);
    const size_t n   = dcabi::utf8TruncatedLength(utf8, len, cap - 1);
    if (n > 0) std::memcpy(dest, utf8, n);
    dest[n] = '\0';
}

void fillDeviceInfo(dca_device_info& info,
                    const juce::String& name,
                    juce::AudioIODeviceType& type,
                    bool isInput,
                    bool isDefault)
{
    std::memset(&info, 0, sizeof(info));
    copyTruncated(info.id,   sizeof(info.id),   name);
    copyTruncated(info.name, sizeof(info.name), name);
    info.driver      = typeToDriver(type.getTypeName());
    info.is_default  = isDefault ? 1 : 0;
    copyTruncated(info.type_name, sizeof(info.type_name), type.getTypeName());

    // Fast path: ASIO drivers open in EXCLUSIVE mode. Calling
    // type.createDevice() here to inspect channel counts / sample rates
    // tears the driver up, claims the audio hardware and on a number of
    // poorly-behaved drivers (FlexASIO, ReaperASIO, M-Audio) it deadlocks
    // or blocks for seconds. Skip the device open for ASIO during static
    // enumeration and report zero — callers that need detailed channel
    // info should use enumerateOutputDevicesLive() on an already-open
    // engine which queries the driver in-place.
    if (type.getTypeName().equalsIgnoreCase("ASIO"))
    {
        info.max_output_channels = 0;
        info.max_input_channels  = 0;
        info.default_sample_rate = 0;
        return;
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = isInput ? juce::String() : name;
    setup.inputDeviceName  = isInput ? name           : juce::String();

    std::unique_ptr<juce::AudioIODevice> dev(
        type.createDevice(setup.outputDeviceName, setup.inputDeviceName));
    if (dev != nullptr)
    {
        info.max_output_channels = dev->getOutputChannelNames().size();
        info.max_input_channels  = dev->getInputChannelNames().size();
        auto rates = dev->getAvailableSampleRates();
        info.default_sample_rate = rates.isEmpty() ? 0 : static_cast<int32_t>(rates[rates.size() / 2]);
    }
}

} // namespace

// --- AudioEngine ----------------------------------------------------------

AudioEngine::AudioEngine()
{
    // Per-player rig defaults (ABI v28). std::atomic is not copy-initialisable
    // inside a std::array brace-initialiser, so the non-zero defaults are set
    // here rather than in the member declarations.
    //   * seat p listens to hardware input p — the two-guitars-on-one-interface
    //     case works with no extra configuration, and seat 0 keeps the channel-0
    //     default it has always had;
    //   * every seat's monitor trim starts at unity, so playerCount_ == 1 sums
    //     into the output at exactly the pre-v28 level.
    for (int p = 0; p < kMaxPlayers; ++p)
    {
        inputCaptureChannel_[(size_t) p].store(p, std::memory_order_relaxed);
        playerMonitorGain_ [(size_t) p].store(1.0f, std::memory_order_relaxed);
        inputPeak_         [(size_t) p].store(0.0f, std::memory_order_relaxed);
        //   * and every seat's drop-tune pedal starts BYPASSED at the Balanced
        //     lag ceiling (ABI v31) — the same two values the single pre-v31
        //     pedal held, so seat 0 is unchanged and a seat nobody configured
        //     never shifts anything.
        inputPitchShiftSemis_[(size_t) p].store(0.0f,  std::memory_order_relaxed);
        dropTuneMaxLagMs_    [(size_t) p].store(20.0f, std::memory_order_relaxed);
    }

    formatManager_.registerBasicFormats();
    // ABI v42 (#426): a Rocksmith .wem opens like any other file, decoded
    // natively on the load thread (src/wem/).
    formatManager_.registerFormat(new dcwem::WemAudioFormat(), false);
    readThread_.startThread();
    player_.setSource(&mixer_);
}

AudioEngine::~AudioEngine()
{
    stopChartReaper();
    stop();
    player_.setSource(nullptr);
    readThread_.stopThread(2000);
}

juce::String AudioEngine::start(const dca_engine_config& cfg)
{
    // #27: stop() detaches every source from the mixer for good (removeAllInputs +
    // sources_.clear(); re-adding them blindly would double-route stem-grouped
    // sources), so a restarted engine used to run a live device with silence on
    // it and report success. Refuse instead, and say what to do.
    if (stoppedAfterStart_.load() && ! started_.load())
        return "This engine was stopped, and a stopped engine cannot be restarted: stop() detaches every "
               "source and the audio callback for good. Destroy it and create a new one (to change device "
               "without stopping, use dca_engine_reconfigure).";

    if (started_.exchange(true))
        return {}; // already started: idempotent

    // Null test device: appended to this manager's type list ONLY when this
    // engine was created with DCA_ENGINE_CREATE_NULL_DEVICE (ABI v41) or
    // DCA_ENABLE_NULL_DEVICE=1 (see NullAudioIODevice.h). With both off this
    // is a no-op, so the type can never appear in user-visible lists.
    nulldev::registerIfEnabled(deviceManager_, nullDeviceByConfig_, nullDeviceManualByConfig_);

    // Pick driver type ----------------------------------------------------
    // ABI v7: explicit type name override (e.g. "Windows Audio (Exclusive Mode)")
    // wins over the dca_driver enum. This is the only way to pick between
    // WASAPI Shared / Exclusive / Low-Latency since they all share
    // DCA_DRIVER_WASAPI. Output side takes precedence for routing decisions.
    juce::String overrideTypeName;
    if (cfg.output_type_name[0] != '\0') overrideTypeName = juce::String::fromUTF8(cfg.output_type_name);
    else if (cfg.input_type_name[0] != '\0') overrideTypeName = juce::String::fromUTF8(cfg.input_type_name);

    const char* requestedTypeName = overrideTypeName.isNotEmpty()
        ? overrideTypeName.toRawUTF8()
        : driverToTypeName(cfg.preferred_driver);
    if (requestedTypeName != nullptr && requestedTypeName[0] != '\0')
    {
        bool matched = false;
        for (auto* type : deviceManager_.getAvailableDeviceTypes())
        {
            if (type->getTypeName().equalsIgnoreCase(requestedTypeName))
            {
                deviceManager_.setCurrentAudioDeviceType(type->getTypeName(), true);
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            // started_ MUST be cleared on every failure exit, not just the two
            // at the bottom. This one did not: a start() that failed here left
            // started_ == true with no device open and no audio callback added,
            // so the NEXT start() hit the idempotence guard at the top and
            // returned success — a permanently dead engine that reported itself
            // as running, and a stop() that then tore down nothing.
            started_ = false;
            return juce::String("Requested audio driver not available: ") + requestedTypeName;
        }

        // setCurrentAudioDeviceType does NOT eagerly scan device lists — the
        // type only knows about devices after scanForDevices() runs. Without
        // this, a subsequent setAudioDeviceSetup() with a specific output
        // device name fails with "No such device" because the type's device
        // list is still empty (regression hits ASIO especially hard since
        // JUCE 8.0.5+; WASAPI happened to lazy-scan inside the same call).
        if (auto* curType = deviceManager_.getCurrentDeviceTypeObject())
        {
            try { curType->scanForDevices(); } catch (...) {}
        }
    }

    // Build setup ---------------------------------------------------------
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager_.getAudioDeviceSetup(setup);

    if (cfg.output_device_id[0] != '\0')
        setup.outputDeviceName = juce::String::fromUTF8(cfg.output_device_id);
    if (cfg.input_device_id[0] != '\0')
        setup.inputDeviceName  = juce::String::fromUTF8(cfg.input_device_id);

    // If the caller didn't pin a device by name, fall back to the first
    // device the active host exposes. JUCE's initialiseWithDefaultDevices()
    // works for WASAPI/CoreAudio (which have a real OS-level default device)
    // but ASIO has no concept of a default — the driver MUST be named or
    // setAudioDeviceSetup will silently no-op and a later "default" call
    // ends up routing to WASAPI / DirectSound instead, which is the exact
    // symptom users see when switching the dropdown to ASIO.
    if (auto* curType = deviceManager_.getCurrentDeviceTypeObject())
    {
        if (setup.outputDeviceName.isEmpty())
        {
            const auto outs = curType->getDeviceNames(false);
            if (outs.size() > 0) setup.outputDeviceName = outs[0];
        }
        if (setup.inputDeviceName.isEmpty())
        {
            const auto ins = curType->getDeviceNames(true);
            if (ins.size() > 0) setup.inputDeviceName = ins[0];
        }
    }

    if (cfg.sample_rate   > 0) setup.sampleRate           = cfg.sample_rate;
    if (cfg.buffer_frames > 0) setup.bufferSize           = cfg.buffer_frames;

    // Channel selection: an explicit mask wins over the (input|output)_channels
    // count, which only picks the first N channels via setRange(0, N). Mask
    // lets the user route a guitar plugged into Focusrite input 2 (channel
    // index 1) without ever touching channel 0.
    if (cfg.input_channel_mask != 0)
    {
        juce::BigInteger b;
        for (int i = 0; i < 32; ++i) if (cfg.input_channel_mask & (1u << i)) b.setBit(i);
        setup.inputChannels            = b;
        setup.useDefaultInputChannels  = false;
    }
    else
    {
        setup.useDefaultInputChannels  = (cfg.input_channels  <= 0);
        if (cfg.input_channels  > 0) setup.inputChannels .setRange(0, cfg.input_channels,  true);
    }

    if (cfg.output_channel_mask != 0)
    {
        juce::BigInteger b;
        for (int i = 0; i < 32; ++i) if (cfg.output_channel_mask & (1u << i)) b.setBit(i);
        setup.outputChannels           = b;
        setup.useDefaultOutputChannels = false;
    }
    else
    {
        setup.useDefaultOutputChannels = (cfg.output_channels <= 0);
        if (cfg.output_channels > 0) setup.outputChannels.setRange(0, cfg.output_channels, true);
    }

    const int  inputChannels   = (cfg.input_channels  > 0) ? cfg.input_channels  : 0;
    const int  outputChannels  = (cfg.output_channels > 0) ? cfg.output_channels : 2;

    juce::String err;
    if (setup.outputDeviceName.isEmpty() && setup.inputDeviceName.isEmpty())
        err = deviceManager_.initialiseWithDefaultDevices(inputChannels, outputChannels);
    else
        err = deviceManager_.setAudioDeviceSetup(setup, true);

    // Device-walk fallback: when the host's selected device fails (most
    // common on ASIO when the breakout box is offline — error 0x54f),
    // walk the rest of the host's devices until one opens. We do this even
    // when the caller pinned a device because failing back to the caller
    // for them to retry would be worse UX than silently landing on the
    // sibling device of the same host and logging it.
    if (err.isNotEmpty())
    {
        if (auto* curType = deviceManager_.getCurrentDeviceTypeObject())
        {
            try { curType->scanForDevices(); } catch (...) {}
            const auto outs = curType->getDeviceNames(false);
            const auto ins  = curType->getDeviceNames(true);
            const int  outN = juce::jmax(1, outs.size());
            const int  inN  = juce::jmax(1, ins.size());
            auto attemptBase = setup;
            attemptBase.sampleRate = 0; // let host pick a supported rate
            attemptBase.bufferSize = 0; // and supported buffer size
            for (int o = 0; o < outN && err.isNotEmpty(); ++o)
            {
                for (int i = 0; i < inN && err.isNotEmpty(); ++i)
                {
                    auto attempt = attemptBase;
                    attempt.outputDeviceName = (o < outs.size()) ? outs[o] : juce::String();
                    attempt.inputDeviceName  = (i < ins .size()) ? ins [i] : juce::String();
                    // Don't repeat the very combination that just failed.
                    if (attempt.outputDeviceName == setup.outputDeviceName
                     && attempt.inputDeviceName  == setup.inputDeviceName) continue;
                    err = deviceManager_.setAudioDeviceSetup(attempt, true);
                }
            }
        }
    }

    if (err.isNotEmpty())
    {
        // Help diagnose "No such device": dump the actually-available device
        // names for the chosen type. Without this, users see a bare error
        // string and have no way to tell if their saved device name is stale
        // or if the driver scan returned nothing.
        if (auto* curType = deviceManager_.getCurrentDeviceTypeObject())
        {
            try
            {
                curType->scanForDevices();
                const auto outs = curType->getDeviceNames(false);
                const auto ins  = curType->getDeviceNames(true);
                juce::String dump;
                dump << "[start] requested output='" << setup.outputDeviceName
                     << "' input='" << setup.inputDeviceName
                     << "' on type='" << curType->getTypeName()
                     << "'. Available outputs={";
                for (int i = 0; i < outs.size(); ++i)
                    dump << (i ? ", " : "") << "'" << outs[i] << "'";
                dump << "} inputs={";
                for (int i = 0; i < ins.size(); ++i)
                    dump << (i ? ", " : "") << "'" << ins[i] << "'";
                dump << "}";
                engineLog(kLogError, dump);
            }
            catch (...) {}
        }

        started_ = false;
        return err;
    }

    // Belt-and-braces: JUCE can return an empty error string yet still leave
    // the device manager without a current device (the open failed silently
    // and then succeeded on a subsequent retry that picked nothing). Bail
    // out here so the C# fallback chain can pick a different driver instead
    // of pretending the engine is running.
    if (deviceManager_.getCurrentAudioDevice() == nullptr)
    {
        started_ = false;
        return "Audio device manager returned success but no device is open.";
    }

    // addAudioCallback runs audioDeviceAboutToStart synchronously, which is
    // where prepareMasterBus establishes deviceSr_ — so the mirror restore
    // below already sees the new device's rate.
    deviceManager_.addAudioCallback(&callback_);

    // A rebuild (stop() + start()) must not silently drop the streaming
    // mirror. Everything else the master bus owns is replayed by
    // prepareMasterBus; the mirror is the one piece that lives outside it.
    restoreMirrorAfterStart();
    return {};
}

// ONE body for both directions: the two used to be a copy-paste pair whose
// only difference was the `false`/`true` handed to getDeviceNames and
// getDefaultDeviceIndex — 35 duplicated lines including the channel-count
// block, which is exactly the shape that drifts.
int AudioEngine::enumerateDevicesLive(bool isInput, dca_device_info* out, int capacity)
{
    int written = 0;
    auto* curType = deviceManager_.getCurrentDeviceTypeObject();
    const auto snap = deviceSnapshot();
    for (auto* type : deviceManager_.getAvailableDeviceTypes())
    {
        if (type == nullptr) continue;
        type->scanForDevices();
        const auto names = type->getDeviceNames(isInput);
        const int  defaultIdx = type->getDefaultDeviceIndex(isInput);
        for (int i = 0; i < names.size(); ++i)
        {
            if (out != nullptr && written < capacity)
            {
                auto& info = out[written];
                std::memset(&info, 0, sizeof(info));
                copyTruncated(info.id,   sizeof(info.id),   names[i]);
                copyTruncated(info.name, sizeof(info.name), names[i]);
                info.driver     = typeToDriver(type->getTypeName());
                info.is_default = (i == defaultIdx) ? 1 : 0;
                copyTruncated(info.type_name, sizeof(info.type_name), type->getTypeName());
                // The OPEN device's numbers, for the current type only — the
                // cheap way to fill these in without opening anything. From the
                // SNAPSHOT, not from getCurrentAudioDevice(): this runs on
                // whatever thread the managed caller is on, and that pointer is
                // deleted by setAudioDeviceSetup (see deviceSnapshot()).
                //
                // `default_sample_rate` is therefore the rate the device is
                // OPEN at rather than the median of the rates it advertises.
                // For a LIVE enumeration that is the more useful of the two,
                // and it is the only one available without a device pointer.
                if (type == curType && snap.open)
                {
                    info.max_output_channels = snap.outputChannels;
                    info.max_input_channels  = snap.inputChannels;
                    info.default_sample_rate = snap.sampleRate;
                }
            }
            ++written;
        }
    }
    return written;
}


void AudioEngine::prepareMasterBus(juce::AudioIODevice* device)
{
    std::lock_guard<std::recursive_mutex> publishLock(filterPublishMutex_);
    deviceSr_       = device != nullptr ? device->getCurrentSampleRate() : 44100.0;
    deviceBlock_    = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;

    // THE MASTER-BUS WIDTH (JuceSharp #31): ONE number, and every per-channel
    // stage below — the HP/LP/BP banks, the song EQ bank, the limiter — is
    // sized from it and from nothing else.
    //
    // It is the ACTIVE output count, because that is how many pointers the
    // callback is handed. JUCE's Windows devices PACK the enabled outputs: one
    // non-null pointer per ENABLED channel, in order, whatever their physical
    // positions — WASAPI's run() passes
    // `numOutputBuffers = getActiveOutputChannels().countNumberOfSetBits()`,
    // ASIO fills `outBuffers[n]` for the n-th enabled channel and passes
    // `numActiveOutputChans`, DirectSound sizes its buffer from
    // `enabledOutputs.countNumberOfSetBits()`, and AudioDeviceManager passes
    // the count through. (The first paragraph of juce_AudioIODevice.h's
    // callback doc — "same array indices as getOutputChannelNames(), null for
    // the rest" — is not what any of them does.) So outputs 3-4 of an
    // interface with 1-2 off reach us as out[0], out[1].
    //
    // The limiter used to be sized from getOutputChannelNames() on the
    // opposite belief while the filter banks clamped `jmin(ch, size - 1)`: had
    // that belief been true, L and R would have run through ONE biquad state.
    // Now a block WIDER than this width is skipped and counted by every stage
    // (processMasterBus, MasterLimiter::process), never folded onto a shared
    // state. RtSparseOutputMaskIsPackedAndTheMasterBusKeepsLAndRApart reads the
    // pointers the callback is handed on a sparse mask and pins all of it.
    masterBusWidth_ = device != nullptr ? juce::jmax(1, device->getActiveOutputChannels().countNumberOfSetBits()) : 2;
    masterBusSkipped_.store(0, std::memory_order_relaxed);

    // A device session starts AT the published gains, never ramping up to them
    // from whatever the previous device ended on — that would be a fade-in on
    // every device change, sample-rate change and watchdog recovery. Safe to
    // write here: prepareMasterBus runs from audioDeviceAboutToStart, with no
    // callback in flight.
    masterGainCurrent_ = masterGain_.load(std::memory_order_relaxed);
    {
        const float mon = monitorGain_.load(std::memory_order_relaxed);
        for (int p = 0; p < kMaxPlayers; ++p)
        {
            monitorGainCurrent_[(size_t) p] =
                mon * playerMonitorGain_[(size_t) p].load(std::memory_order_relaxed);
            inputGainCurrent_[(size_t) p] = inputGain_.load(std::memory_order_relaxed);
        }
    }
    instrumentGainCurrent_ = instrumentGain_.load(std::memory_order_relaxed);

    // ABI v34. The master-bus ceiling follows the device the same way every
    // other stage here does — it is sized for the device's OUTPUT channel
    // count and its look-ahead is derived from the device rate, and it never
    // resizes below this point. A device change therefore starts it from a
    // cleared delay line and unity gain, which is correct: the old line holds
    // the previous session's samples at the previous rate.
    //
    // Sized from masterBusWidth_ like every other per-channel stage (see the
    // top of this function for why that is the ACTIVE output count).
    masterLimiter_.prepare(deviceSr_, masterBusWidth_);

    // Keep the streaming mirror's resampler in step with the engine (FIFO
    // producer) rate. Cheap atomic store; no-op when the mirror is closed or
    // the rate is unchanged. The mirror audio thread picks it up next callback.
    mirror_.setEngineSampleRate(deviceSr_);

    auto resize = [this](std::vector<juce::dsp::IIR::Filter<float>>& bank)
    {
        bank.clear();
        bank.resize(static_cast<size_t>(masterBusWidth_));
    };
    resize(hpFilters_);
    resize(lpFilters_);
    resize(bpFilters_);

    juce::dsp::ProcessSpec spec{ deviceSr_, static_cast<juce::uint32>(deviceBlock_), 1u };
    for (auto& f : hpFilters_) f.prepare(spec);
    for (auto& f : lpFilters_) f.prepare(spec);
    for (auto& f : bpFilters_) f.prepare(spec);

    // Seed every section with a real 2nd-order coefficient set so its array is
    // already sized to 5 here, on the message thread. The audio thread then
    // only ever overwrites those 5 floats in place and can never trigger a
    // juce::Array growth on the callback.
    //
    // ...and then reset() each filter HERE, which is the half that was missing.
    // juce::dsp::IIR::Filter::reset() re-mallocs its juce::HeapBlock state
    // whenever the coefficients' ORDER differs from the order it last saw
    // (juce_IIRFilter_Impl.h:81). prepare() above ran while the filter still
    // held its default FIRST-order coefficients, so the seed assignment left
    // every filter with a cached order of 1 against 2nd-order coefficients —
    // and processMasterBus's off->on `f.reset()` then heap-allocated 16 bytes
    // per channel ON THE AUDIO THREAD the first time a band was engaged.
    //
    // That is a HeapBlock/malloc allocation, not an operator new, which is
    // exactly why the RT gate hooks malloc. Caught by
    // RtFilterSweepAllocatesNothing (2 allocs + 2 frees at
    // juce_IIRFilter_Impl.h:81); resetting here makes the audio thread's reset
    // a pure memset.
    {
        auto seed = juce::dsp::IIR::Coefficients<float>::makeHighPass(
            deviceSr_ > 0.0 ? deviceSr_ : 44100.0, 1000.0f, 0.707f);
        auto seedBank = [&seed](std::vector<juce::dsp::IIR::Filter<float>>& bank)
        {
            for (auto& f : bank) { *f.coefficients = *seed; f.reset(); }
        };
        seedBank(hpFilters_);
        seedBank(lpFilters_);
        seedBank(bpFilters_);

        // ABI v29. Same allocate-and-seed-on-the-message-thread contract as the
        // three above, one bank per bus: the song bus at masterBusWidth_, and
        // one per SEAT at kChainChannels because two seats carry
        // different signals and cannot share biquad memory.
        // clear() + resize(), never assign(): juce::dsp::IIR::Filter is not
        // copy-assignable, so assign(n, {}) does not compile — the same reason
        // the `resize` lambda above is shaped the way it is.
        auto sizeAndSeed = [&](std::vector<juce::dsp::IIR::Filter<float>>& bank, int count)
        {
            bank.clear();
            bank.resize((size_t) count);
            for (auto& f : bank) f.prepare(spec);
            seedBank(bank);
        };
        sizeAndSeed(eqSongFilters_, kEqBands * masterBusWidth_);
        for (int p = 0; p < kMaxPlayers; ++p)
            sizeAndSeed(eqInputFilters_[(size_t) p], kEqBands * kChainChannels);

        eqSongEngaged_.fill(false);
        for (auto& seat : eqInputEngaged_) seat.fill(false);
    }
    // Device rate may have changed — recompute against the new deviceSr_.
    publishMasterFilterCoeffs();

    // Fresh (device-)session: the banks were just rebuilt, so the first dirty
    // pass must treat every band as newly engaged (reset once, then retunes
    // preserve state — audit #8).
    hpEngaged_ = lpEngaged_ = bpEngaged_ = false;
    filterDirty_ = true;
    eqInputDirty_ = true;
    eqSongTrimCurrent_ = eqTrimLin_.load(std::memory_order_relaxed);
    eqInputTrimCurrent_.fill(eqSongTrimCurrent_);

    // p0-2 / p0-3: ensure the audio-thread scratch buffers are large enough
    // for the active device's block size & channel count BEFORE the callback
    // can fire. Otherwise the first few callbacks reallocate on the audio
    // thread (the existing `if (numSamples > capacity) setSize(...)` paths
    // are correct but block xrun-style). We pre-size for the current device
    // plus a small headroom so block-size renegotiation up to ~2x doesn't
    // reallocate on the hot path either.
    // GROW-ONLY on both axes: naive setSize(prepCh, prepSize) could SHRINK the
    // channel count a multi-out instrument grew it to (attachInstrumentPlugin)
    // while growing samples — and the render paths no longer resize on the RT
    // thread (audit #5), so a shrink here would silence that bus permanently.
    const int prepCh   = juce::jmax(2, masterBusWidth_);
    const int prepSize = juce::jmax(2048, deviceBlock_ * 2);
    // ABI v28: one work buffer per player rig. Every seat is pre-sized here,
    // whether or not it is currently live — playerCount_ can be raised from the
    // message thread at any moment and the callback must never allocate.
    for (auto& cb : chainBuffer_)
        if (cb.getNumChannels() < prepCh || cb.getNumSamples() < prepSize)
            cb.setSize(juce::jmax(prepCh, cb.getNumChannels()),
                       juce::jmax(prepSize, cb.getNumSamples()), false, true, true);
    if (instrumentBuffer_.getNumChannels() < prepCh || instrumentBuffer_.getNumSamples() < prepSize)
        instrumentBuffer_.setSize(juce::jmax(prepCh, instrumentBuffer_.getNumChannels()),
                                  juce::jmax(prepSize, instrumentBuffer_.getNumSamples()), false, true, true);

    // In-house DSP tone rig (0.16 Phase 0): prepare every module for the active
    // device so the audio thread never allocates. The rig runs on the 2-channel
    // chain buffer (see the input path in AudioEngine.h) — kChainChannels is the
    // single source of truth for that width, and processDspRig clamps to it so
    // the rig is never handed more channels than it was prepared for.
    // ABI v28: every player's rig, live or not, for the same reason the chain
    // buffers above are all sized here.
    // prepSize carries the 2x scratch headroom; deviceBlock_ is what the
    // callback actually renders, and the rig uses it for the fade TIMING that
    // structure edits wait on (see DspChain::prepare).
    for (auto& rig : dspRigs_) rig.prepare(deviceSr_, prepSize, kChainChannels, deviceBlock_);
    // Idle sleep: a rebuilt engine starts every seat AWAKE with an empty hold
    // (the re-prepare above also moved each chain's edit generation, which
    // would wake it anyway; this makes it true before the first callback).
    for (auto& idle : rigIdle_) idle.reset();

    // juce::MidiBuffer keeps a std::vector<uint8_t> internally; ensureSize
    // grows it without freeing, so subsequent addEvent() calls on the audio
    // thread won't allocate. 4 KiB ~= 1000 short events / block which is
    // far above realistic worst-case MIDI density.
    // Sized from the FIFO depth, not a round number. juce::MidiBuffer stores
    // 4 (timestamp) + 2 (size) + N data bytes per event, so a 3-byte message
    // costs 9: the old flat 4096 held only 455 events while the drain below
    // reads up to kMidiQueueDepth (512) of them, and addEvent grew the vector
    // — a heap allocation on the audio thread — from the 456th event on. The
    // ×12 leaves room for 6-byte messages without re-deriving this.
    instrumentMidiPending_.ensureSize((size_t) kMidiQueueDepth * 12);

    // ...and the SAME hazard for the input chain's buffer (W4-N2). It is named
    // emptyMidi_ and it is cleared before every use, which is exactly what made
    // it look like it could not grow — but processBlock's MIDI argument is
    // IN/OUT. A plugin in the input chain that EMITS MIDI (a tuner, a MIDI-learn
    // echo, an arpeggiator) addEvent()s into it, and the first such write mallocs
    // the underlying vector ON THE AUDIO THREAD. clear() keeps capacity, so one
    // reservation here covers the whole session. The RT gate never caught it
    // because no scenario attached an input plugin that emits.
    emptyMidi_.ensureSize((size_t) kMidiQueueDepth * 12);

    // An attached VST3 is prepared ONCE, at attach time, with whatever
    // deviceSr_/deviceBlock_ were then. Nothing else ever re-prepared it, so a
    // dca_engine_reconfigure to a different rate or buffer size (a device
    // change, a NAM 48 kHz request, a watchdog recovery) left every already
    // loaded input plugin and the instrument VSTi running against the OLD
    // numbers: their internal time constants detune with the rate, and a
    // plugin prepared for 256 frames that is handed 1024 is entitled to
    // allocate — on the audio thread. Everything else the master bus owns is
    // rebuilt above; this is the one piece that was not.
    reprepareAttachedPlugins();
}

void AudioEngine::reprepareAttachedPlugins()
{
    const double sr  = deviceSr_    > 0 ? deviceSr_    : 44100.0;
    const int    blk = deviceBlock_ > 0 ? deviceBlock_ : 512;

    // Called from prepareMasterBus, i.e. from audioDeviceAboutToStart, i.e.
    // with NO audio callback in flight — JUCE's AudioDeviceManager holds its
    // own callback lock across the prepare. That is why this needs no
    // wait-one-block barrier, unlike detachInputPlugin. It is also exactly
    // where juce::AudioProcessorPlayer prepares the processor IT hosts, so a
    // VST3 is called here on the thread every JUCE host calls it on.
    {
        std::lock_guard<std::mutex> lk(pluginMutex_);
        for (auto& slot : inputChain_)
        {
            auto chain = std::atomic_load_explicit(&slot, std::memory_order_acquire);
            if (chain == nullptr) continue;
            for (auto* p : *chain)
                if (p != nullptr) { try { p->prepareToPlay(sr, blk); } catch (...) {} }
        }
    }

    // RE-ARM every seat's faulted set (W5-E7). A prepare is a legitimate second
    // chance: the plugin has just been handed a fresh rate and layout, and
    // "prepared for the wrong layout" is one of the two ordinary reasons a
    // processBlock throws. Bumping the GENERATION rather than clearing the bits
    // keeps every write to the bitset itself on the audio thread.
    inputChainGeneration_.fetch_add(1, std::memory_order_acq_rel);

    // The instrument, and THE CHANNEL COUNT THAT GOES WITH IT. `prepareToPlay`
    // may leave a plugin on a different bus layout than the one it had at
    // attach — a multi-output sampler renegotiating at a new rate is the
    // ordinary case — and `instrumentChannels_` was left at the attach-time
    // figure. renderInstrumentBus guards on
    // `instrumentBuffer_.getNumChannels() >= instrumentChannels_` and SKIPS the
    // block when it fails, so an instrument that came back wider than its
    // buffer went silent for the rest of the session with nothing logged.
    //
    // Same grow-only, allocate-off-the-lock shape as attachInstrumentPlugin:
    // the audio thread takes instrumentMutex_ every render block, so growing
    // the buffer while holding it would make the callback wait on a heap
    // allocation. Grow-only because the render path SKIPS rather than resizing,
    // so shrinking here would silence the instrument for good.
    auto* instr = instrumentPlugin_.load(std::memory_order_acquire);
    if (instr == nullptr) return;

    { std::lock_guard<std::mutex> lk(instrumentMutex_);
      try { instr->prepareToPlay(sr, blk); } catch (...) {} }

    const int pluginOutCh = juce::jmax(1, instr->getTotalNumOutputChannels());
    int prepCh = 0, prepSize = 0;
    {
        std::lock_guard<std::mutex> lk(instrumentMutex_);
        prepCh   = juce::jmax(pluginOutCh, instrumentBuffer_.getNumChannels());
        prepSize = juce::jmax(juce::jmax(blk * 2, 2048), instrumentBuffer_.getNumSamples());
        if (instrumentBuffer_.getNumChannels() >= prepCh
            && instrumentBuffer_.getNumSamples() >= prepSize)
        {
            instrumentChannels_ = pluginOutCh;   // already big enough: retag only
            prepCh = 0;
        }
    }

    if (prepCh > 0)
    {
        juce::AudioBuffer<float> fresh(prepCh, prepSize);
        fresh.clear();

        juce::AudioBuffer<float> retired;
        {
            std::lock_guard<std::mutex> lk(instrumentMutex_);
            retired = std::move(instrumentBuffer_);
            instrumentBuffer_ = std::move(fresh);
            instrumentChannels_ = pluginOutCh;
        }
        // `retired` frees here, off the lock.
    }
}

void AudioEngine::publishMasterFilterCoeffs() noexcept
{
    std::lock_guard<std::recursive_mutex> publishLock(filterPublishMutex_);
    // MESSAGE THREAD ONLY. The IIR::Coefficients factories allocate, which is
    // exactly why this cannot live in processMasterBus: every setHighPass/
    // setLowPass/setBandPass raised filterDirty_, so dragging a filter knob
    // made the audio callback heap-allocate (and free) up to six times per
    // block at UI tick rate — on a path whose own comment documents sweeping
    // as an intended, supported gesture.
    const double sr  = deviceSr_ > 0.0 ? deviceSr_ : 44100.0;
    MasterFilterCoeffs dst;

    auto fill = [](BiquadCoeffs& slot, juce::dsp::IIR::Coefficients<float>::Ptr c)
    {
        // A 2nd-order section keeps 5 normalized coefficients (b0,b1,b2,a1,a2).
        const auto* raw = c->getRawCoefficients();
        for (int i = 0; i < 5; ++i) slot.v[i] = raw[i];
        slot.engaged = true;
    };

    const float hpF = hpFreq_.load(std::memory_order_relaxed);
    const float lpF = lpFreq_.load(std::memory_order_relaxed);
    const float bpF = bpFreq_.load(std::memory_order_relaxed);
    const float hpQ = juce::jmax(0.05f, hpQ_.load(std::memory_order_relaxed));
    const float lpQ = juce::jmax(0.05f, lpQ_.load(std::memory_order_relaxed));
    const float bpQ = juce::jmax(0.05f, bpQ_.load(std::memory_order_relaxed));

    dst = MasterFilterCoeffs{};
    if (hpF > 0.0f) fill(dst.hp, juce::dsp::IIR::Coefficients<float>::makeHighPass(sr, hpF, hpQ));
    if (lpF > 0.0f) fill(dst.lp, juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, lpF, lpQ));
    if (bpF > 0.0f) fill(dst.bp, juce::dsp::IIR::Coefficients<float>::makeBandPass(sr, bpF, bpQ));

    // ABI v29: the six parametric bands ride in the same slot. A band is
    // published only when it is AUDIBLE — master power on, and either soloed or
    // (enabled with nothing soloed). Deciding that here rather than on the audio
    // thread is what makes eqActiveCount() and the callback agree by
    // construction: they read the same predicate.
    const bool eqOn   = eqEnabled_.load(std::memory_order_relaxed);
    const int  eqSolo = eqSolo_.load(std::memory_order_relaxed);
    if (eqOn)
    {
        for (int i = 0; i < kEqBands; ++i)
        {
            const bool audible = eqSolo >= 0 && eqSolo < kEqBands
                               ? (i == eqSolo)
                               : eqOn_[(size_t) i].load(std::memory_order_relaxed);
            if (! audible) continue;

            const float f = eqFreq_[(size_t) i].load(std::memory_order_relaxed);
            const float q = eqQ_   [(size_t) i].load(std::memory_order_relaxed);
            const float g = eqGain_[(size_t) i].load(std::memory_order_relaxed);
            // Never let a band sit at or above Nyquist: the RBJ formulae go
            // degenerate there and JUCE would hand back a section that blows up.
            const float fc = juce::jlimit(1.0f, (float) (sr * 0.49), f);

            using Coeffs = juce::dsp::IIR::Coefficients<float>;
            switch (eqType_[(size_t) i].load(std::memory_order_relaxed))
            {
                case DCA_EQ_HIGH_PASS:  fill(dst.eq[i], Coeffs::makeHighPass (sr, fc, q)); break;
                case DCA_EQ_LOW_SHELF:  fill(dst.eq[i], Coeffs::makeLowShelf (sr, fc, q, juce::Decibels::decibelsToGain(g))); break;
                case DCA_EQ_HIGH_SHELF: fill(dst.eq[i], Coeffs::makeHighShelf(sr, fc, q, juce::Decibels::decibelsToGain(g))); break;
                case DCA_EQ_LOW_PASS:   fill(dst.eq[i], Coeffs::makeLowPass  (sr, fc, q)); break;
                case DCA_EQ_NOTCH:      fill(dst.eq[i], Coeffs::makeNotch    (sr, fc, q)); break;
                case DCA_EQ_BELL:
                default:                fill(dst.eq[i], Coeffs::makePeakFilter(sr, fc, q, juce::Decibels::decibelsToGain(g))); break;
            }
        }
    }

    filterCoefficients_.publish(dst);
}

// ── Parametric EQ (ABI v29) ───────────────────────────────────────────────

void AudioEngine::setEqBand(int index, int type, float freqHz, float q, float gainDb, bool enabled) noexcept
{
    std::lock_guard<std::recursive_mutex> publishLock(filterPublishMutex_);
    if (index < 0 || index >= kEqBands) return;

    // Clamp HERE, at the boundary, to exactly the ranges EqLimits publishes on
    // the managed side. A band the engine silently moved is a band whose drawn
    // curve and audible curve disagree.
    const auto i = (size_t) index;
    eqType_[i].store(juce::jlimit(0, (int) DCA_EQ_NOTCH, type),           std::memory_order_relaxed);
    eqFreq_[i].store(juce::jlimit(kEqMinFreqHz, kEqMaxFreqHz, freqHz),    std::memory_order_relaxed);
    eqQ_   [i].store(juce::jlimit(kEqMinQ, kEqMaxQ, q),                   std::memory_order_relaxed);
    eqGain_[i].store(juce::jlimit(-kEqMaxGainDb, kEqMaxGainDb, gainDb),   std::memory_order_relaxed);
    eqOn_  [i].store(enabled,                                             std::memory_order_relaxed);
    publishMasterFilterCoeffs();
}

void AudioEngine::setEqEnabled(bool on) noexcept
{
    std::lock_guard<std::recursive_mutex> publishLock(filterPublishMutex_);
    eqEnabled_.store(on, std::memory_order_relaxed);
    publishMasterFilterCoeffs();
}

void AudioEngine::setEqSolo(int bandIndex) noexcept
{
    std::lock_guard<std::recursive_mutex> publishLock(filterPublishMutex_);
    eqSolo_.store(bandIndex >= 0 && bandIndex < kEqBands ? bandIndex : -1, std::memory_order_relaxed);
    publishMasterFilterCoeffs();
}

void AudioEngine::setEqRoute(int route) noexcept
{
    eqRoute_.store(juce::jlimit((int) DCA_EQ_ROUTE_SONG, (int) DCA_EQ_ROUTE_BOTH, route),
                   std::memory_order_relaxed);
}

void AudioEngine::setEqTrim(float trimDb) noexcept
{
    const float clamped = juce::jlimit(-kEqMaxTrimDb, kEqMaxTrimDb, trimDb);
    eqTrimLin_.store(juce::Decibels::decibelsToGain(clamped), std::memory_order_relaxed);
}

int AudioEngine::eqActiveCount() const noexcept
{
    if (! eqEnabled_.load(std::memory_order_relaxed)) return 0;

    const int solo = eqSolo_.load(std::memory_order_relaxed);
    if (solo >= 0 && solo < kEqBands) return 1;

    int count = 0;
    for (int i = 0; i < kEqBands; ++i)
        if (eqOn_[(size_t) i].load(std::memory_order_relaxed)) ++count;
    return count;
}

void AudioEngine::applyEqBank(std::vector<juce::dsp::IIR::Filter<float>>& bank, int bankChannels,
                              float* const* data, int numChannels, int numSamples,
                              bool retune, std::array<bool, kEqBands>& engaged) noexcept
{
    if (bankChannels <= 0 || bank.size() < (size_t) (kEqBands * bankChannels)) return;

    const auto& cf = filterCoefficients_.current();

    {
        // ALLOCATION-FREE retune, exactly as the three fixed sections do it: the
        // coefficients were computed on the message thread, and every filter's
        // coefficient array was already sized to 5 at prepare time.
        for (int b = 0; b < kEqBands; ++b)
        {
            if (! cf.eq[b].engaged) { engaged[(size_t) b] = false; continue; }
            if (!retune && engaged[(size_t) b]) continue;
            for (int ch = 0; ch < bankChannels; ++ch)
            {
                auto& f   = bank[(size_t) (b * bankChannels + ch)];
                auto& arr = f.coefficients->coefficients;
                if (arr.size() >= 5)
                {
                    auto* d = arr.getRawDataPointer();
                    for (int k = 0; k < 5; ++k) d[k] = cf.eq[b].v[k];
                }
                // Off -> on only: retuning a running band must keep its z^-1
                // memory or dragging its handle clicks on every tick.
                if (! engaged[(size_t) b]) f.reset();
            }
            engaged[(size_t) b] = true;
        }
    }

    // Channel ch runs on state ch. A buffer wider than the bank is left
    // unfiltered rather than clamped onto the last state, which would run two
    // channels through ONE biquad in sequence (#31); the retune above has still
    // run, so the bank is current for the next block that fits.
    if (numChannels > bankChannels) return;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* dst = data[ch];
        if (dst == nullptr) continue;

        for (int b = 0; b < kEqBands; ++b)
        {
            if (! cf.eq[b].engaged) continue;
            auto& f = bank[(size_t) (b * bankChannels + ch)];
            for (int i = 0; i < numSamples; ++i) dst[i] = f.processSample(dst[i]);
        }
    }
}

void AudioEngine::writeSpectrumTap(const float* const* data, int numChannels, int numSamples) noexcept
{
    if (data == nullptr || numChannels <= 0 || numSamples <= 0) return;

    auto     write = spectrumWritten_.load(std::memory_order_relaxed);
    const float scale = 1.0f / (float) numChannels;

    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            if (data[ch] != nullptr) sum += data[ch][i];
        spectrumRing_[(size_t) (write % kSpectrumSize)] = sum * scale;
        ++write;
    }

    spectrumWritten_.store(write, std::memory_order_release);
}

int AudioEngine::readEqSpectrum(float* destDb, int binCount) noexcept
{
    if (destDb == nullptr || binCount < 8 || binCount > 512) return 0;

    // MESSAGE THREAD. The FFT and its scratch are built lazily here rather than
    // in prepare() so an engine nobody ever asks for a spectrum never pays for
    // them; the audio thread touches none of it.
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    if (spectrumFft_ == nullptr)
    {
        spectrumFft_ = std::make_unique<juce::dsp::FFT>(kSpectrumFftOrder);
        spectrumScratch_.assign((size_t) kSpectrumSize * 2, 0.0f);
        spectrumWindow_.assign((size_t) kSpectrumSize, 0.0f);
        // Hann. Computed once: a rectangular window smears a guitar's
        // fundamental across half the plot and the analyser looks like noise.
        for (int i = 0; i < kSpectrumSize; ++i)
            spectrumWindow_[(size_t) i] =
                0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi
                                        * (float) i / (float) (kSpectrumSize - 1)));
    }

    const auto written = spectrumWritten_.load(std::memory_order_acquire);
    std::fill(spectrumScratch_.begin(), spectrumScratch_.end(), 0.0f);

    if (written >= (uint64_t) kSpectrumSize)
    {
        const auto start = written - (uint64_t) kSpectrumSize;
        for (int i = 0; i < kSpectrumSize; ++i)
            spectrumScratch_[(size_t) i] =
                spectrumRing_[(size_t) ((start + (uint64_t) i) % kSpectrumSize)]
                * spectrumWindow_[(size_t) i];
    }

    spectrumFft_->performFrequencyOnlyForwardTransform(spectrumScratch_.data(), true);

    const double sr      = deviceSr_ > 0.0 ? deviceSr_ : 44100.0;
    const double binHz   = sr / (double) kSpectrumSize;
    const int    maxBin  = kSpectrumSize / 2;
    // Normalising by half the window length makes a full-scale sine read ~0 dBFS
    // rather than a number that changes with the FFT size.
    const double norm    = 2.0 / (double) kSpectrumSize;

    for (int i = 0; i < binCount; ++i)
    {
        // Each output bin owns a slice of the LOG axis, so the low end reads one
        // FFT bin and the top reads hundreds — take the PEAK across the slice,
        // which is what makes a narrow hum spike visible at all.
        const double t0 = (double) i       / (double) binCount;
        const double t1 = (double) (i + 1) / (double) binCount;
        const double f0 = kEqMinFreqHz * std::pow(1000.0, t0);
        const double f1 = kEqMinFreqHz * std::pow(1000.0, t1);

        int lo = (int) std::floor(f0 / binHz);
        int hi = (int) std::ceil (f1 / binHz);
        lo = juce::jlimit(1, maxBin - 1, lo);
        hi = juce::jlimit(lo + 1, maxBin, hi);

        float peak = 0.0f;
        for (int b = lo; b < hi; ++b) peak = juce::jmax(peak, spectrumScratch_[(size_t) b]);

        destDb[i] = (float) juce::Decibels::gainToDecibels(juce::jmax(1.0e-6, peak * norm), -120.0);
    }

    return binCount;
}

void AudioEngine::processMasterBus(float* const* out, int numChannels, int numSamples)
{
    if (out == nullptr || numChannels <= 0 || numSamples <= 0) return;

    // EVERY UNTRUSTED STAGE IS FOLLOWED BY A SCRUB (dsp/Scrub.h). By the time
    // the master bus sees `out` it carries the mixer's sum: transport sources
    // (Rubber Band, decoders), the in-process VSTi, the sandbox child's
    // instrument leg and the monitor mix. The three fixed filters below and the
    // song EQ after them are IIR biquads that feed their own output back, so one
    // non-finite sample from ANY of those contributors silences the master
    // section permanently — reset() is only reached on an OFF -> ON transition,
    // which nothing in a normal session performs. It is also the last point
    // before the streaming mirror is fed.
    dcdsp::scrubNonFinite(out, numChannels, numSamples);

    // The dirty flag is CONSUMED here and the EQ banks need it too, so read it
    // once and pass it on — an exchange() per bank would let the song bus
    // retune and the guitar banks silently not.
    const bool retune = std::exchange(filterDirty_, false);

    if (retune)
    {
        // ALLOCATION-FREE retune. The coefficients were computed on the message
        // thread by publishMasterFilterCoeffs; here we only copy 5 floats per
        // section into arrays already sized at prepare time.
        //
        // Retune WITHOUT zeroing state (audit #8): all three sections are
        // 2nd-order RBJ biquads, so swapping coefficients keeps the state
        // array valid, and the running z^-1 memory carries the signal across
        // the change — resetting it on every knob tick discarded the tail and
        // clicked audibly when sweeping the filter during playback. Reset only
        // on an OFF -> ON transition, where the stale state is an old tail
        // that would otherwise replay into the freshly-engaged filter.
        const auto& cf = filterCoefficients_.current();

        auto applyBank = [](std::vector<juce::dsp::IIR::Filter<float>>& bank,
                            const BiquadCoeffs& src, bool wasEngaged)
        {
            if (!src.engaged) return;
            for (auto& f : bank)
            {
                auto& arr = f.coefficients->coefficients;
                if (arr.size() >= 5)
                {
                    auto* d = arr.getRawDataPointer();
                    for (int i = 0; i < 5; ++i) d[i] = src.v[i];
                }
                if (!wasEngaged) f.reset();
            }
        };

        applyBank(hpFilters_, cf.hp, hpEngaged_);
        applyBank(lpFilters_, cf.lp, lpEngaged_);
        applyBank(bpFilters_, cf.bp, bpEngaged_);

        hpEngaged_ = cf.hp.engaged;
        lpEngaged_ = cf.lp.engaged;
        bpEngaged_ = cf.bp.engaged;
    }

    const bool hpOn = hpEngaged_;
    const bool lpOn = lpEngaged_;
    const bool bpOn = bpEngaged_;
    const float gain = masterGain_.load(std::memory_order_relaxed);

    // ONE width for every per-channel stage (#31, see prepareMasterBus). Channel
    // ch runs on state ch. A block WIDER than the prepared width is skipped by
    // the three sections and the song EQ — never clamped onto the last state,
    // which would put two channels through one biquad — and COUNTED, the same
    // rule MasterLimiter::process follows. With JUCE's packed callback it does
    // not happen; the count is how we would find out that it did.
    const bool fits = numChannels <= masterBusWidth_;
    if (! fits) masterBusSkipped_.fetch_add(1, std::memory_order_relaxed);

    for (int ch = 0; fits && ch < numChannels; ++ch)
    {
        float* dst = out[ch];
        if (dst == nullptr) continue;
        const auto idx = (size_t) ch;

        if (hpOn)
        {
            auto& f = hpFilters_[idx];
            for (int i = 0; i < numSamples; ++i) dst[i] = f.processSample(dst[i]);
        }
        if (lpOn)
        {
            auto& f = lpFilters_[idx];
            for (int i = 0; i < numSamples; ++i) dst[i] = f.processSample(dst[i]);
        }
        if (bpOn)
        {
            auto& f = bpFilters_[idx];
            for (int i = 0; i < numSamples; ++i) dst[i] = f.processSample(dst[i]);
        }
    }

    // ABI v29: the parametric EQ runs AFTER the three fixed filters and BEFORE
    // the master gain, on whichever bus its route names — so the trim is a trim
    // of the EQ's own output and the user's master volume still sits last.
    //
    // The analyser tap is taken BEFORE the six bands, so dragging a band does
    // not make the spectrum chase the curve being drawn on top of it.
    const int  route  = eqRoute_.load(std::memory_order_relaxed);
    const bool eqSong = route == DCA_EQ_ROUTE_SONG || route == DCA_EQ_ROUTE_BOTH;
    if (eqSong)
    {
        writeSpectrumTap(out, numChannels, numSamples);
        applyEqBank(eqSongFilters_, masterBusWidth_, out, numChannels, numSamples,
                    retune, eqSongEngaged_);

        const float trim = eqTrimLin_.load(std::memory_order_relaxed);
        for (int ch = 0; ch < numChannels; ++ch)
            rampOrScale(out[ch], numSamples, eqSongTrimCurrent_, trim);
        eqSongTrimCurrent_ = trim;
    }
    else
    {
        // Routed away from the song bus while bands were running: their z^-1
        // memory is now a stale tail that would replay the moment the user
        // routes back. Forget it, so the next engage resets cleanly.
        eqSongEngaged_.fill(false);
        eqSongTrimCurrent_ = 1.0f;
    }

    // Master volume, RAMPED across the block (see rampOrScale). Every channel
    // gets the same from/to pair, so the image cannot shift while the slider
    // moves; the equal case is still the plain vectorised multiply this line
    // has always been.
    const float gainPrev = masterGainCurrent_;
    for (int ch = 0; ch < numChannels; ++ch)
        rampOrScale(out[ch], numSamples, gainPrev, gain);
    masterGainCurrent_ = gain;
}

juce::String AudioEngine::showAsioControlPanel ([[maybe_unused]] void* parentWindow)
{
   #if JUCE_WINDOWS && JUCE_ASIO
    auto* dev = deviceManager_.getCurrentAudioDevice();
    if (dev == nullptr)
        return "No audio device is currently open.";
    if (! dev->hasControlPanel())
        return "Active audio device does not expose a control panel.";
    if (! dev->showControlPanel())
        return "Audio device declined to open its control panel.";
    return {};
   #else
    juce::ignoreUnused(parentWindow);
    return "ASIO control panel is only available on Windows ASIO builds.";
   #endif
}

void AudioEngine::stop()
{
    if (!started_.exchange(false)) return;
    stoppedAfterStart_ = true;   // start() refuses from now on (#27)

    mirror_.close();
    deviceManager_.removeAudioCallback(&callback_);
    {
        std::lock_guard<std::mutex> lock(sourcesMutex_);
        mixer_.removeAllInputs();
        sources_.clear();
    }
    deviceManager_.closeAudioDevice();
}

juce::String AudioEngine::setMirrorDevice(const juce::String& deviceName)
{
    // Remember the REQUEST, not just the result: stop() closes the mirror, and
    // nothing else in the engine knows the name, so a stop()/start() rebuild
    // used to end the stream to OBS / Discord silently and permanently. See
    // restoreMirrorAfterStart().
    requestedMirrorDevice_ = deviceName;
    // Feed the mirror the primary engine's device rate so its consumer can
    // resample from the engine (FIFO producer) rate to the mirror device rate.
    return mirror_.open(deviceName, currentSampleRate());
}

void AudioEngine::restoreMirrorAfterStart()
{
    if (requestedMirrorDevice_.isEmpty() || mirror_.isOpen())
        return;
    // currentSampleRate() is already correct here: start() adds the audio
    // callback before calling us, and addAudioCallback runs
    // audioDeviceAboutToStart -> prepareMasterBus synchronously.
    const auto err = mirror_.open(requestedMirrorDevice_, currentSampleRate());
    if (err.isNotEmpty())
    {
        // A mirror device that has gone away must never fail the engine start;
        // the primary output is what the user is listening to.
        engineLog(kLogWarn, juce::String("[start] mirror device could not be reopened: ") + err);
    }
}

void AudioEngine::resetXrunStats() noexcept
{
    lateCallbacks_    .store(0, std::memory_order_relaxed);
    deadlineOverruns_ .store(0, std::memory_order_relaxed);
    droppedFrames_    .store(0, std::memory_order_relaxed);
    overrunCount_     .store(0, std::memory_order_relaxed);
    callbackCount_    .store(0, std::memory_order_relaxed);
    maxCallbackNs_    .store(0, std::memory_order_relaxed);
    lastXrunHostTicks_.store(0, std::memory_order_relaxed);
    lastXrunFrame_    .store(-1, std::memory_order_relaxed);
    // NOT the per-snapshot faulted BITS — only the counter. Clearing the bits
    // here would re-enable a plugin that is still going to throw, from a
    // health-window reset that has nothing to do with the plugin; re-arming is
    // the job of the next attach / detach / prepare, each of which republishes
    // the snapshot the bits are keyed to.
    inputPluginFaults_.store(0, std::memory_order_relaxed);

    // Rebase the driver's own counter so device_xruns describes the window the
    // caller just opened, exactly like every other field. The sampled value,
    // not the device pointer — see fillXrunStats.
    const int32_t raw = deviceXrunRaw_.load(std::memory_order_relaxed);
    deviceXrunBase_.store(raw > 0 ? raw : 0, std::memory_order_relaxed);
}

void AudioEngine::fillXrunStats(dca_xrun_stats& s) noexcept
{
    s.device_open    = 0;
    s.device_running = 0;
    s.device_xruns   = -1;

    // NO DEVICE POINTER. This is polled by the managed health TIMER, i.e. from
    // an arbitrary thread, and `getCurrentAudioDevice()` hands back a raw
    // pointer that setAudioDeviceSetup deletes — dca_engine_reconfigure and
    // dca_engine_set_device_type both go through it. The open flag comes from
    // the device SNAPSHOT (published on the device thread), and the driver's
    // own counter is sampled on the audio thread, where the device is alive by
    // construction.
    //
    // device_running follows device_open deliberately: audioDeviceAboutToStart
    // is the callback session, so an open device here IS one that is calling
    // us. The pair stays in the struct because the managed side uses it to tell
    // a live engine from a reconfigure whose rollback also failed.
    if (const auto snap = deviceSnapshot(); snap.open)
    {
        s.device_open    = 1;
        s.device_running = 1;
        const int raw = deviceXrunRaw_.load(std::memory_order_relaxed);  // -1 on WASAPI / aggregate CoreAudio
        if (raw >= 0)
        {
            const int32_t base = deviceXrunBase_.load(std::memory_order_relaxed);
            // A driver that restarted its own counter (device reopen) reads
            // BELOW the base — rebase rather than report a negative delta.
            if (raw < base) deviceXrunBase_.store(0, std::memory_order_relaxed);
            s.device_xruns = (int32_t) juce::jmax(0, raw - (raw < base ? 0 : base));
        }
    }

    s.callbacks              = (int64_t) callbackCount_.load(std::memory_order_relaxed);
    s.late_callbacks         = lateCallbacks_    .load(std::memory_order_relaxed);
    s.deadline_overruns      = deadlineOverruns_ .load(std::memory_order_relaxed);
    s.near_misses            = (int64_t) overrunCount_.load(std::memory_order_relaxed);
    s.dropped_frames         = droppedFrames_    .load(std::memory_order_relaxed);
    s.last_xrun_host_ticks   = lastXrunHostTicks_.load(std::memory_order_relaxed);
    s.last_xrun_render_frame = lastXrunFrame_    .load(std::memory_order_relaxed);
    s.max_callback_ns        = maxCallbackNs_    .load(std::memory_order_relaxed);
    s.last_callback_ns       = lastCallbackNs_   .load(std::memory_order_relaxed);
    s.block_budget_ns        = blockBudgetNs_    .load(std::memory_order_relaxed);
    s.input_plugin_faults    = inputPluginFaults_.load(std::memory_order_relaxed);
}

// --- Click bus ------------------------------------------------------------

void AudioEngine::triggerClick(int type) noexcept
{
    if (type < 0 || type > 2) return;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    clickFifo_.prepareToWrite(1, start1, size1, start2, size2);
    if (size1 > 0)      clickQueue_[(size_t)start1] = type;
    else if (size2 > 0) clickQueue_[(size_t)start2] = type;
    else return; // FIFO full — drop trigger (UI is hammering us)
    clickFifo_.finishedWrite(size1 + size2);
}

// --- Scheduled clicks (ABI v26) -------------------------------------------

void AudioEngine::scheduleClick(int type, int64_t renderFrame) noexcept
{
    if (type < 0 || type > 2) return;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    clickScheduleFifo_.prepareToWrite(1, start1, size1, start2, size2);
    const int slot = (size1 > 0) ? start1 : (size2 > 0 ? start2 : -1);
    if (slot < 0) return; // FIFO full — drop (caller is hammering us)
    clickScheduleQueue_[(size_t)slot] = ScheduledClick{ type, renderFrame };
    clickScheduleFifo_.finishedWrite(size1 + size2);
}

void AudioEngine::clearScheduledClicks() noexcept
{
    // Pushed as an ordinary FIFO entry, NOT a side-channel flag: that is what
    // makes "schedule A, then clear" always cancel A. The audio thread wipes
    // its pending set when it drains the sentinel, so the clear cannot be
    // reordered ahead of clicks that were queued before it.
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    clickScheduleFifo_.prepareToWrite(1, start1, size1, start2, size2);
    const int slot = (size1 > 0) ? start1 : (size2 > 0 ? start2 : -1);
    if (slot < 0) return;
    clickScheduleQueue_[(size_t)slot] = ScheduledClick{ kClickClearSentinel, 0 };
    clickScheduleFifo_.finishedWrite(size1 + size2);
}

bool AudioEngine::getRenderClock(int64_t& outFrames, int64_t& outTicks) const noexcept
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const uint32_t before = renderSyncSeq_.load(std::memory_order_acquire);
        if (before == 0) return false;          // no block rendered yet
        if ((before & 1u) != 0) continue;       // writer in flight
        const int64_t frames = renderFrames_;
        const int64_t ticks  = renderSyncTicks_;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (renderSyncSeq_.load(std::memory_order_relaxed) == before)
        {
            outFrames = frames;
            outTicks  = ticks;
            return true;
        }
    }
    return false;
}

void AudioEngine::startClickVoice(int type, int offsetInBlock) noexcept
{
    // Pick the first idle voice, or steal the most-advanced active one
    // (oldest), so a fast burst still fires.
    int target = -1;
    int oldestPlayed = -1;
    for (int v = 0; v < kClickVoices; ++v)
    {
        if (clickVoices_[(size_t)v].type < 0) { target = v; break; }
        if (clickVoices_[(size_t)v].played > oldestPlayed)
        { oldestPlayed = clickVoices_[(size_t)v].played; target = v; }
    }
    if (target < 0) target = 0;
    auto& voice = clickVoices_[(size_t)target];
    voice.type        = type;
    voice.played      = 0;
    voice.phase       = 0.0f;
    voice.rng         = 0x12345u ^ (uint32_t)(((uintptr_t)&voice) >> 4);
    voice.startOffset = offsetInBlock > 0 ? offsetInBlock : 0;
    const double durMs = (type == 2) ? 90.0 : 55.0;
    voice.total = juce::jmax(1, (int) std::round(deviceSr_ * durMs / 1000.0));
}

void AudioEngine::mixClickBus(float* const* out, int numChannels, int numSamples,
                              int64_t blockStartFrame)
{
    if (out == nullptr || numChannels <= 0 || numSamples <= 0) return;

    // --- 1. Immediate triggers: start at the head of this block (legacy path).
    int ready = clickFifo_.getNumReady();
    while (ready > 0)
    {
        int s1 = 0, sz1 = 0, s2 = 0, sz2 = 0;
        clickFifo_.prepareToRead(ready, s1, sz1, s2, sz2);
        auto consume = [&](int start, int count)
        {
            for (int i = 0; i < count; ++i)
                startClickVoice(clickQueue_[(size_t)(start + i)], 0);
        };
        consume(s1, sz1);
        consume(s2, sz2);
        clickFifo_.finishedRead(sz1 + sz2);
        ready = clickFifo_.getNumReady();
    }

    // --- 2. Scheduled clicks: park them until their frame lands in a block.
    int schedReady = clickScheduleFifo_.getNumReady();
    while (schedReady > 0)
    {
        int s1 = 0, sz1 = 0, s2 = 0, sz2 = 0;
        clickScheduleFifo_.prepareToRead(schedReady, s1, sz1, s2, sz2);
        auto consume = [&](int start, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                const ScheduledClick entry = clickScheduleQueue_[(size_t)(start + i)];
                if (entry.type == kClickClearSentinel)
                {
                    for (auto& p : clickPending_) p.type = -1;
                    continue;
                }
                for (auto& p : clickPending_)
                {
                    if (p.type < 0) { p = entry; break; }
                }
                // Pending set full (>32 clicks in flight): drop. A sane
                // lookahead never gets close, so this only trips on abuse.
            }
        };
        consume(s1, sz1);
        consume(s2, sz2);
        clickScheduleFifo_.finishedRead(sz1 + sz2);
        schedReady = clickScheduleFifo_.getNumReady();
    }

    // --- 3. Fire every pending click whose frame falls inside THIS block, at
    //        its exact sample offset. This is the entire point of the feature:
    //        the voice starts on the same sample the note's own audio occupies,
    //        so click and note leave the engine in one buffer and reach the ear
    //        together — whatever the output latency happens to be.
    {
        const double  sr           = deviceSr_ > 0.0 ? deviceSr_ : 44100.0;
        const int64_t staleFrames  = (int64_t) (sr * 0.25);  // 250 ms late  -> drop
        const int64_t futureFrames = (int64_t) (sr * 5.0);   //   5 s  early -> drop
        const int64_t blockEnd     = blockStartFrame + (int64_t) numSamples;
        for (auto& p : clickPending_)
        {
            if (p.type < 0) continue;
            if (p.frame - blockStartFrame > futureFrames)
            {
                // Only reachable if the render clock rewound under us (device
                // restart). Drop rather than sound minutes from now.
                p.type = -1;
                continue;
            }
            if (p.frame >= blockEnd) continue;               // still ahead of us
            int offset = (int) (p.frame - blockStartFrame);
            if (offset < 0)
            {
                // Late. Within a quarter second, fire at the head of the block
                // (no worse than the immediate path); beyond that the schedule
                // is stale — a stalled caller or a paused transport — so drop it
                // instead of machine-gunning the user.
                if (blockStartFrame - p.frame > staleFrames) { p.type = -1; continue; }
                offset = 0;
            }
            startClickVoice(p.type, offset);
            p.type = -1;
        }
    }

    // --- 4. Render the sounding voices.
    const float master  = clickGain_.load(std::memory_order_relaxed);
    const bool  audible = master > 0.0f;

    for (auto& voice : clickVoices_)
    {
        if (voice.type < 0) continue;

        // Consume the scheduling offset: it applies to this block only.
        const int off = voice.startOffset;
        if (off >= numSamples) { voice.startOffset = off - numSamples; continue; }
        voice.startOffset = 0;

        const int samplesLeft = voice.total - voice.played;
        const int n = juce::jmin(numSamples - off, samplesLeft);

        if (!audible)
        {
            // Click bus muted. Still age the voice, otherwise every slot stays
            // occupied forever and the bus never sounds again once unmuted.
            voice.played += n;
            if (voice.played >= voice.total) voice.type = -1;
            continue;
        }

        const double sr = deviceSr_ > 0.0 ? deviceSr_ : 44100.0;
        const float  freq = (voice.type == 0) ? 1000.0f
                          : (voice.type == 1) ? 1500.0f
                                              : 0.0f; // clap = noise
        const float  baseGain = (voice.type == 2) ? 0.45f : 0.35f;
        for (int i = 0; i < n; ++i)
        {
            const float t = (float) voice.played / (float) voice.total;
            const float env = (1.0f - t) * (1.0f - t); // quadratic decay
            float s;
            if (voice.type == 2)
            {
                // White noise -> shape with envelope
                voice.rng = voice.rng * 1664525u + 1013904223u;
                s = (((float)(voice.rng >> 8) / 16777216.0f) * 2.0f - 1.0f);
            }
            else
            {
                voice.phase += (float) (juce::MathConstants<double>::twoPi * freq / sr);
                if (voice.phase > juce::MathConstants<float>::twoPi)
                    voice.phase -= juce::MathConstants<float>::twoPi;
                s = std::sin(voice.phase);
            }
            const float sample = s * env * baseGain * master;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                if (out[ch] != nullptr) out[ch][off + i] += sample;
            }
            ++voice.played;
        }
        if (voice.played >= voice.total)
        {
            voice.type = -1;
        }
    }
}

// --- Offline waveform peak generation ------------------------------------

// Shared decode loop behind both generateWaveform variants: one peak per
// blockMs window over whichever reader the caller opened (file or memory).
//
// THE BLOCK WIDTH IS FRACTIONAL AND MUST STAY THAT WAY. Block k spans samples
// [llround(k*w), llround((k+1)*w)) where w = sr*blockMs/1000 — a real number,
// not a rounded integer samples-per-block. The caller's ONLY way to turn a
// block index back into a time is `index * blockMs` (that is literally what
// DotCharter's WaveformLayer does), so a block that is not blockMs wide is a
// PROGRESSIVE misalignment, not a rounding detail. At 44.1 kHz a 2 ms block is
// 88.2 samples: rounding it to 88 made every block 1.99546 ms, i.e. the drawn
// waveform ran 0.227% LATE — +68 ms 30 s into a song, +408 ms at three
// minutes, which reads as "the waveform is drawn to the right of its audio".
// 48 kHz was exact (96 samples) and hid the bug for everything but the CD-rate
// family. Measured per-impulse before/after with DotCharter's
// LocalOnly_WaveformAlignmentProbe.
//
// `gain` is a device-rate file's stored step (ABI v43, DeviceRateWav.h): its
// samples are worth v x step, not the v/32768 the reader decodes. 1 otherwise.
static int generateWaveformFromReader(juce::AudioFormatReader* readerPtr,
                                      double blockMs,
                                      float* out,
                                      int capacity,
                                      float gain = 1.0f)
{
    auto* reader = readerPtr;
    const juce::int64 totalSamples = reader->lengthInSamples;
    const double sr = reader->sampleRate;
    // >= 1 sample per block: a blockMs finer than one sample period cannot
    // resolve anything, and a zero width would divide by nothing below.
    const double samplesPerBlock = juce::jmax(1.0, sr * blockMs / 1000.0);
    const int    totalBlocks = (int) std::ceil((double) totalSamples / samplesPerBlock);
    if (out == nullptr || capacity <= 0) return totalBlocks;

    const int chans = juce::jmax(1, (int) reader->numChannels);
    const int chunk = juce::jlimit(1, 1 << 16, (int) std::lround(samplesPerBlock * 32.0));
    juce::AudioBuffer<float> buf(chans, chunk);

    int written = 0;
    juce::int64 pos = 0;
    bool blockHasSamples = false;
    // Exclusive end (in samples) of the block currently being accumulated.
    juce::int64 blockEnd = (juce::int64) std::llround(samplesPerBlock);
    float peak = 0.0f;
    while (pos < totalSamples && written < capacity)
    {
        const int toRead = (int) juce::jmin((juce::int64) chunk, totalSamples - pos);
        buf.clear();
        reader->read(&buf, 0, toRead, pos, true, chans > 1);
        for (int i = 0; i < toRead; ++i)
        {
            float v = 0.0f;
            for (int ch = 0; ch < chans; ++ch)
            {
                const float s = std::abs(buf.getReadPointer(ch)[i]) * gain;
                if (s > v) v = s;
            }
            if (v > peak) peak = v;
            blockHasSamples = true;

            if (pos + i + 1 >= blockEnd)
            {
                out[written++] = juce::jmin(1.0f, peak);
                peak = 0.0f;
                blockHasSamples = false;
                if (written >= capacity) break;
                // Boundaries are recomputed from the exact width every time, so
                // the rounding never accumulates.
                blockEnd = (juce::int64) std::llround((double) (written + 1) * samplesPerBlock);
                if (blockEnd <= pos + i + 1) blockEnd = pos + i + 2;
            }
        }
        pos += toRead;
    }
    if (blockHasSamples && written < capacity)
        out[written++] = juce::jmin(1.0f, peak);
    return totalBlocks;
}

int AudioEngine::generateWaveform(const juce::File& file,
                                  double blockMs,
                                  float* out,
                                  int capacity)
{
    if (!file.existsAsFile() || blockMs <= 0.0) return 0;

    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();
    fmt.registerFormat(new dcwem::WemAudioFormat(), false);
    std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0) return 0;

    auto stored = devwav::probeFile(file);
    devwav::checkAgainstReader(stored, *reader);
    return generateWaveformFromReader(reader.get(), blockMs, out, capacity, (float) stored.decodeGain());
}

int AudioEngine::generateWaveformFromMemory(const void* data,
                                            size_t dataSize,
                                            double blockMs,
                                            float* out,
                                            int capacity)
{
    if (data == nullptr || dataSize == 0 || blockMs <= 0.0) return 0;

    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();
    fmt.registerFormat(new dcwem::WemAudioFormat(), false);
    // keepInternalCopy=false: the managed caller pins its byte[] for the
    // duration of the call, so the stream reads the caller's memory directly.
    auto stream = std::make_unique<juce::MemoryInputStream>(data, dataSize, false);
    std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(std::move(stream)));
    if (reader == nullptr || reader->sampleRate <= 0.0) return 0;

    auto stored = devwav::probeMemory(data, dataSize);
    devwav::checkAgainstReader(stored, *reader);
    return generateWaveformFromReader(reader.get(), blockMs, out, capacity, (float) stored.decodeGain());
}

void AudioEngine::addSource(TransportSource* s)
{
    if (s == nullptr) return;
    std::lock_guard<std::mutex> lock(sourcesMutex_);
    if (sources_.insert(s).second)
        mixer_.addInputSource(s->sourceForMixer(), false);
}

void AudioEngine::removeSource(TransportSource* s)
{
    if (s == nullptr) return;
    // If the source is currently routed through the stem-group bus its
    // panner is owned by stemSubMixer_, NOT by mixer_. Un-group first so
    // that no one keeps a raw pointer to the about-to-be-freed panner.
    // (Audio thread reads stemSubMixer_ each block; failing to detach here
    // is a use-after-free that crashes with 0xC0000005 the moment the
    // source destructor frees panner_.)
    {
        std::lock_guard<std::mutex> stemLock(stemGroupMutex_);
        if (stemSources_.count(s) > 0)
        {
            stemSubMixer_->removeInputSource(s->sourceForMixer());
            s->setStretcherBypass(false);
            stemSources_.erase(s);
            if (stemSources_.empty())
            {
                {
                    std::lock_guard<std::mutex> srcLock(sourcesMutex_);
                    mixer_.removeInputSource(stemGroupStretcher_.get());
                }
                stemGroupStretcher_->releaseResources();
                stemGroupStretcher_.reset();
                stemPositionable_.reset();
                stemSubMixer_.reset();
            }
            // The source was NOT in mixer_ while grouped — nothing else
            // to remove there. Mark it un-attached and return.
            std::lock_guard<std::mutex> srcLock(sourcesMutex_);
            sources_.erase(s);
            return;
        }
    }

    std::lock_guard<std::mutex> lock(sourcesMutex_);
    if (sources_.erase(s) > 0)
        mixer_.removeInputSource(s->sourceForMixer());
}

// --- Stem group ----------------------------------------------------------

void AudioEngine::setSourceStemGrouped(TransportSource* s, bool grouped)
{
    if (s == nullptr) return;

    // Two locks: stemGroupMutex_ guards the sub-mixer / stretcher pair;
    // sourcesMutex_ guards mixer_ membership. Acquire in a fixed order
    // (stem first, sources second) to keep the lock graph acyclic.
    std::lock_guard<std::mutex> stemLock(stemGroupMutex_);

    const bool currentlyGrouped = stemSources_.count(s) > 0;
    if (currentlyGrouped == grouped) return;

    if (grouped)
    {
        // Lazily create the shared mixer + stretcher on first use.
        if (! stemSubMixer_)
        {
            stemSubMixer_      = std::make_unique<juce::MixerAudioSource>();
            stemPositionable_  = std::make_unique<StemPositionableSource>(stemSubMixer_.get());
            stemGroupStretcher_ = std::make_unique<RubberBandAudioSource>(
                stemPositionable_.get(), false, 2, &stretchQualityFlag());
            stemGroupStretcher_->setBaseRatio(1.0);
            stemGroupStretcher_->setTempoFactor (stemGroupTempo_.load(std::memory_order_relaxed));
            stemGroupStretcher_->setPitchSemitones(stemGroupPitch_.load(std::memory_order_relaxed));

            const int    blk = deviceBlock_ > 0 ? deviceBlock_ : 512;
            const double sr  = deviceSr_    > 0 ? deviceSr_    : 44100.0;
            stemGroupStretcher_->prepareToPlay(blk, sr);

            std::lock_guard<std::mutex> srcLock(sourcesMutex_);
            mixer_.addInputSource(stemGroupStretcher_.get(), false);
        }

        // Move the source: bypass its own stretcher and re-route its panner
        // output from the main mixer into the stem sub-mixer.
        {
            std::lock_guard<std::mutex> srcLock(sourcesMutex_);
            if (sources_.count(s) > 0)
                mixer_.removeInputSource(s->sourceForMixer());
        }
        s->setStretcherBypass(true);
        stemSubMixer_->addInputSource(s->sourceForMixer(), false);
        stemSources_.insert(s);
    }
    else
    {
        // Un-group: pull out of the stem sub-mixer, restore stretcher,
        // re-attach to the main mixer.
        stemSubMixer_->removeInputSource(s->sourceForMixer());
        s->setStretcherBypass(false);
        stemSources_.erase(s);
        {
            std::lock_guard<std::mutex> srcLock(sourcesMutex_);
            if (sources_.count(s) > 0)
                mixer_.addInputSource(s->sourceForMixer(), false);
        }

        // Tear the shared bus down when the last source leaves so we don't
        // keep paying for the stretcher's CPU when there's nothing to mix.
        if (stemSources_.empty())
        {
            {
                std::lock_guard<std::mutex> srcLock(sourcesMutex_);
                mixer_.removeInputSource(stemGroupStretcher_.get());
            }
            stemGroupStretcher_->releaseResources();
            stemGroupStretcher_.reset();
            stemPositionable_.reset();
            stemSubMixer_.reset();
        }
    }
}

void AudioEngine::setStemGroupTempo(float factor) noexcept
{
    stemGroupTempo_.store(factor > 0.0f ? factor : 1.0f, std::memory_order_release);
    std::lock_guard<std::mutex> lock(stemGroupMutex_);
    if (stemGroupStretcher_) stemGroupStretcher_->setTempoFactor(factor);
}

void AudioEngine::setStemGroupPitch(float semitones) noexcept
{
    stemGroupPitch_.store(semitones, std::memory_order_release);
    std::lock_guard<std::mutex> lock(stemGroupMutex_);
    if (stemGroupStretcher_) stemGroupStretcher_->setPitchSemitones(semitones);
}

void AudioEngine::resetStemGroupStretcher()
{
    std::lock_guard<std::mutex> lock(stemGroupMutex_);
    if (stemGroupStretcher_) stemGroupStretcher_->resetStretcher();
}

int AudioEngine::stemGroupReadAheadSamples() const noexcept
{
    // const_cast: stemGroupMutex_ is mutable; this is a read-only query.
    auto* self = const_cast<AudioEngine*>(this);
    std::lock_guard<std::mutex> lock(self->stemGroupMutex_);
    return self->stemGroupStretcher_ ? self->stemGroupStretcher_->getReadAheadOutputSamples() : 0;
}

// Same fold as enumerateDevicesLive: one body, `isInput` selects the direction.
int AudioEngine::enumerateDevices(bool isInput, dca_device_info* out, int capacity)
{
    juce::AudioDeviceManager tmp;
    tmp.initialiseWithDefaultDevices(0, 0); // populates device types

    int written = 0;
    for (auto* type : tmp.getAvailableDeviceTypes())
    {
        if (type == nullptr) continue;
        type->scanForDevices();
        const auto names = type->getDeviceNames(isInput);
        const int  defaultIdx = type->getDefaultDeviceIndex(isInput);
        for (int i = 0; i < names.size(); ++i)
        {
            if (out != nullptr && written < capacity)
                fillDeviceInfo(out[written], names[i], *type, isInput, i == defaultIdx);
            ++written;
        }
    }
    return written;
}


int AudioEngine::enumerateHostDevices(const char* typeName, bool isInput,
                                      dca_device_info* out, int capacity)
{
    juce::AudioDeviceManager tmp;
    tmp.initialiseWithDefaultDevices(0, 0);

    juce::AudioIODeviceType* target = nullptr;
    if (typeName != nullptr && typeName[0] != '\0')
    {
        const juce::String wanted = juce::String::fromUTF8(typeName);
        for (auto* t : tmp.getAvailableDeviceTypes())
        {
            if (t->getTypeName() == wanted) { target = t; break; }
        }
        if (target == nullptr) return 0;
    }
    else
    {
        target = tmp.getCurrentDeviceTypeObject();
        if (target == nullptr) return 0;
    }

    target->scanForDevices();
    const auto names = target->getDeviceNames(isInput);
    const int  defaultIdx = target->getDefaultDeviceIndex(isInput);
    int written = 0;
    for (int i = 0; i < names.size(); ++i)
    {
        if (out != nullptr && written < capacity)
            fillDeviceInfo(out[written], names[i], *target, isInput, i == defaultIdx);
        ++written;
    }
    return written;
}

int AudioEngine::enumerateHostTypes(char* typeNames, int capacityEntries)
{
    juce::AudioDeviceManager tmp;
    tmp.initialiseWithDefaultDevices(0, 0);

    int written = 0;
    for (auto* type : tmp.getAvailableDeviceTypes())
    {
        if (typeNames != nullptr && written < capacityEntries)
        {
            // 63 BYTES, not 63 characters: this was the last hand-rolled cut
            // in the engine, and a driver type name is exactly the kind of
            // string a localized Windows returns with multi-byte characters in
            // it. A torn sequence decodes to U+FFFD, so a short buffer CORRUPTS
            // the tail instead of clipping it, and the managed side can then no
            // longer match the name it was handed back.
            copyTruncated(typeNames + written * 64, 64, type->getTypeName());
        }
        ++written;
    }
    return written;
}

// --- TransportSource ------------------------------------------------------

TransportSource::TransportSource(AudioEngine& engine) : engine_(engine) {}

bool TransportSource::stretcherUsedFasterForTest() const noexcept
{
    return stretcher_ != nullptr && stretcher_->preparedWithFasterEngineForTest();
}

// Tear down everything the previous open built, in the one order that is safe.
//
// The mixer holds a RAW pointer to panner_ (AudioEngine::addSource passes
// sourceForMixer()), and the stem sub-mixer holds the same pointer while this
// source is grouped -- so the detach has to happen BEFORE panner_ is freed, and
// removeSource is the only thing that knows which of the two currently owns it.
// Everything after that is ownership order: readerSource_ references reader_.
// memorySource_ owns its samples.
//
// Shared with a RE-OPEN, which is why the budget release lives here: releasing
// it in the destructor alone meant a second openFile/openMemory on the same
// source overwrote preloadedBytes_ and leaked the previous reservation for the
// life of the engine. Enough re-opens and the engine-wide budget is exhausted,
// every later source silently falls back to STREAMING, and the JUCE read-ahead
// lock -- the 503 ms callback stall preloading exists to remove -- comes back.
void TransportSource::releaseWiring()
{
    // FIRST: the audio thread reads transport_ / stretcher_ through the song
    // clock while this source is bound. Unbind and wait it out before anything
    // below frees them.
    if (engine_.forgetSongClockSource(this)) wasSongClock_ = true;
    if (attached_)
    {
        engine_.removeSource(this);
        attached_ = false;
    }
    if (transport_) transport_->setSource(nullptr);
    panner_.reset();
    stretcher_.reset();
    transport_.reset();
    // The stage reads the read-ahead, which reads the reader: top down.
    streamResampler_.reset();
    streamBuffer_.reset();
    memorySource_.reset();
    if (preloadedBytes_ > 0)
    {
        engine_.releasePreloadBytes(preloadedBytes_);
        preloadedBytes_ = 0;
    }
    storedStepGain_.reset();
    readerSource_.reset();
    reader_.reset();
    playbackSampleRate_ = 0.0;
    storedStep_ = 0.0f;
    resampleReport_ = dcload::ResampleReport{};
}

TransportSource::~TransportSource()
{
    releaseWiring();
}

juce::String TransportSource::openFile(const juce::File& file)
{
    if (! file.existsAsFile())
        return juce::String("File not found: ") + file.getFullPathName();

    std::unique_ptr<juce::AudioFormatReader> incoming(engine_.formatManager().createReaderFor(file));
    if (incoming == nullptr)
        return juce::String("Unsupported audio format: ") + file.getFullPathName();

    // ABI v43: a device-rate file carries the step its samples play at, and a
    // damaged one is refused here rather than played at the wrong level.
    auto stored = devwav::probeFile(file);
    devwav::checkAgainstReader(stored, *incoming);
    if (stored.kind == devwav::Kind::Damaged)
        return "Damaged device-rate file (" + stored.why + "): " + file.getFullPathName();

    // Only once the new open is certain to proceed: a failed open above leaves
    // whatever was already playing alone.
    releaseWiring();

    reader_ = std::move(incoming);
    memoryBytes_.reset();
    fileSampleRate_ = reader_->sampleRate;
    storedStep_ = stored.kind == devwav::Kind::Valid ? stored.chunk.step : 0.0f;

    wireReaderToTransport();
    return {};
}

juce::String TransportSource::openMemory(const void* bytes, size_t numBytes, const juce::String& formatHint)
{
    if (bytes == nullptr || numBytes == 0)
        return juce::String("Empty in-memory audio blob.");

    // Own the bytes for the lifetime of the reader: MemoryInputStream takes a
    // raw pointer and does not copy; if the block went away the reader would
    // read freed memory on the audio thread.
    //
    // Built LOCALLY and moved into memoryBytes_ only once the reader exists.
    // Writing straight into the member clobbered the bytes the PREVIOUS
    // reader was still pointing at, so a re-open was a use-after-free before
    // it was anything else. juce::MemoryBlock's move keeps the same HeapBlock
    // address, so the stream inside `raw` stays valid across the move.
    juce::MemoryBlock incoming;
    incoming.setSize(numBytes, false);
    incoming.copyFrom(bytes, 0, numBytes);

    auto stream = std::make_unique<juce::MemoryInputStream>(incoming.getData(), numBytes, false);

    juce::AudioFormatReader* raw = nullptr;
    auto& fm = engine_.formatManager();

    // If the caller knows the format (PSARC pipeline always produces WAV
    // after WEM->Ogg->PCM), try that format first to avoid having JUCE walk
    // through every registered format probing the header.
    if (formatHint.isNotEmpty())
    {
        const juce::String ext = formatHint.startsWithChar('.') ? formatHint : ("." + formatHint);
        for (int i = 0; i < fm.getNumKnownFormats(); ++i)
        {
            auto* fmt = fm.getKnownFormat(i);
            if (fmt == nullptr) continue;
            const auto exts = fmt->getFileExtensions();
            if (exts.contains(ext, /*ignoreCase*/ true))
            {
                raw = fmt->createReaderFor(stream.release(), /*deleteStreamIfReadingFails*/ true);
                break;
            }
        }
    }

    if (raw == nullptr)
    {
        // Fallback: ask the format manager to figure it out. createReaderFor
        // takes ownership of the stream and deletes it on failure.
        if (! stream)
            stream = std::make_unique<juce::MemoryInputStream>(incoming.getData(), numBytes, false);
        raw = fm.createReaderFor(std::move(stream));
    }

    if (raw == nullptr)
        return juce::String("Unsupported in-memory audio format.");

    // ABI v43: see openFile.
    auto stored = devwav::probeMemory(incoming.getData(), numBytes);
    devwav::checkAgainstReader(stored, *raw);
    if (stored.kind == devwav::Kind::Damaged)
    {
        delete raw;
        return "Damaged device-rate file (" + stored.why + ") in memory.";
    }

    releaseWiring();

    memoryBytes_ = std::move(incoming);
    reader_.reset(raw);
    fileSampleRate_ = reader_->sampleRate;
    storedStep_ = stored.kind == devwav::Kind::Valid ? stored.chunk.step : 0.0f;

    wireReaderToTransport();
    return {};
}

// --- Preloaded-audio budget ----------------------------------------------

// Derived from the machine, not hard-coded: this ships to people whose boxes
// are nothing like the one it was tuned on. A quarter of physical RAM leaves
// the app's own ~1.5-2 GB and the OS alone, and the floor keeps a small
// machine able to preload at least the song.
int64_t AudioEngine::preloadBudgetBytes() noexcept
{
    const int64_t physicalMb = (int64_t) juce::SystemStats::getMemorySizeInMegabytes();
    const int64_t mb         = juce::jlimit<int64_t>(256, 4096, physicalMb / 4);
    return mb * 1024 * 1024;
}

// A compare-exchange rather than fetch_add: two sources loading at once must
// not both see room and both take it.
bool AudioEngine::reservePreloadBytes(int64_t bytes) noexcept
{
    if (bytes <= 0) return false;

    const int64_t budget = preloadBudgetBytes();
    int64_t held = preloadedBytes_.load(std::memory_order_relaxed);

    for (;;)
    {
        if (held + bytes > budget) return false;
        if (preloadedBytes_.compare_exchange_weak(held, held + bytes,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))
            return true;
    }
}

void AudioEngine::releasePreloadBytes(int64_t bytes) noexcept
{
    if (bytes > 0) preloadedBytes_.fetch_sub(bytes, std::memory_order_acq_rel);
}

// Offline file -> device rate conversion of an already-decoded buffer.
//
// The algorithm, why it exists at all and the three load-bearing invariants
// (upsample only, zero net group delay, the caller reserves the GROWN size)
// are all in LoadResampler.h. This is only the dispatcher: it hands the buffer
// to the rational-polyphase path when the ratio is exact -- 44.1 -> 48 kHz is
// 160:147 -- and to the original juce::WindowedSincInterpolator otherwise.
//
// Runs on the LOAD thread (openFile / openMemory), never on the audio thread,
// so it may allocate and may spread itself over a few worker threads.
static bool upsampleDecodedBuffer(juce::AudioBuffer<float>& buf,
                                  double fromRate, double toRate,
                                  dcload::ResampleReport& report)
{
    return dcload::upsampleDecodedBuffer(buf, fromRate, toRate, &report);
}

// Whether the ABI v43 converter (convertFileForDevice) writes a file: only for
// an UPSAMPLE. Its contract writes nothing for a file at or above the device
// rate, and a host that caches decoded songs replaces its original with the
// result -- a downsampled copy would make every later device at the original
// rate pay an upsample of audio that had already lost its top octave.
static bool loadWillUpsample(double deviceRate, double fileRate) noexcept
{
    return deviceRate > 0.0 && fileRate > 0.0 && deviceRate > fileRate;
}

// Whether a PRELOAD converts at all: any upsample, and since JuceSharp #33 a
// downsample whose ratio is rational (48 -> 44.1 kHz is 147:160). A downsample
// with no rational form is held at its own rate and the transport does the
// rest, as every downsample did before.
static bool loadWillConvert(double deviceRate, double fileRate) noexcept
{
    return deviceRate > 0.0 && dcload::loadCanConvert(fileRate, deviceRate);
}

// A device-rate file's samples are worth v x step (DeviceRateWav.h), not the
// v/32768 JUCE's 16-bit reader decodes. A load that rate-converts such a file
// AGAIN (a device at yet another rate) scales the decode first, so the
// conversion starts from what the song really is. Shared with
// convertFileForDevice for the same reason as decodeWholeReader.
static void applyStoredStepBeforeConversion(juce::AudioBuffer<float>& buf, double gain) noexcept
{
    if (gain != 1.0) buf.applyGain((float) gain);
}

// The whole-file decode a preload starts from, as float. Shared with
// convertFileForDevice so the two cannot drift apart: the converter's output is
// only bit-identical to the load's if both decode through the same call.
// False when the buffer cannot be allocated or the reader fails.
static bool decodeWholeReader(juce::AudioFormatReader& reader, int channels, int frames,
                              juce::AudioBuffer<float>& decoded)
{
    try
    {
        decoded.setSize(channels, frames, false, true, false);
        // The reader fills every channel; anything it cannot supply is cleared
        // by setSize's clearExtraSpace above.
        return reader.read(&decoded, 0, frames, 0, true, true);
    }
    catch (const std::bad_alloc&)
    {
        return false;   // the budget said yes and the allocator disagreed
    }
}

// Decode the whole source into RAM if it fits the engine-wide budget.
//
// Returns false for anything that does not fit, cannot be sized, or fails to
// read -- every one of which simply leaves the caller on the streaming path.
// Runs on the LOAD thread (openFile / openMemory), never on the audio thread.
bool TransportSource::tryPreloadIntoMemory()
{
    if (reader_ == nullptr) return false;

    const int64_t frames   = reader_->lengthInSamples;
    const int     channels = (int) reader_->numChannels;
    if (frames <= 0 || channels <= 0) return false;

    // AudioBuffer indexes samples with int. A file past that is also far past
    // any sane budget, but the cast has to be guarded regardless of the budget.
    if (frames > (int64_t) std::numeric_limits<int>::max()) return false;

    // Reserve for what we will HOLD, not for what we decode: an upsample to the
    // device rate grows the buffer (44.1 -> 48 kHz is +8.8%), and reserving the
    // smaller number would let the engine hold more than its budget says. A
    // downsample shrinks it (48 -> 44.1 kHz is -8.1%).
    const double devRate  = engine_.currentSampleRate();
    const double fileRate = reader_->sampleRate;
    const bool   willConvert = loadWillConvert(devRate, fileRate);
    const int64_t heldFrames = willConvert
        ? (int64_t) std::llround((double) frames * devRate / fileRate)
        : frames;

    // What the source HOLDS once loaded: 16-bit PCM (#411). The float buffer
    // below is a load-thread temporary that is freed before this returns, so
    // a load briefly needs ~3x this figure; the budget is for what stays.
    const int64_t bytes = Int16PreloadSource::bytesFor(heldFrames, channels);
    if (! engine_.reservePreloadBytes(bytes)) return false;

    // Decoded (and possibly rate-converted) as float first: the reader and the
    // offline resampler both work in float, and quantising AFTER the
    // conversion keeps it from adding a second rounding.
    juce::AudioBuffer<float> decoded;
    if (! decodeWholeReader(*reader_, channels, (int) frames, decoded))
    {
        // Silent here on purpose: the ABI layer logs the outcome for every
        // open, so a failed preload already shows up as "streaming".
        engine_.releasePreloadBytes(bytes);
        return false;
    }

    // Convert to the DEVICE rate here, once, offline, in either direction,
    // rather than leaving it to the transport's per-block linear interpolator
    // (LoadResampler.h: dcload::convertDecodedBuffer).
    // playbackSampleRate_ is what the transport is then told the buffer is, so
    // its own resampler runs at ratio 1.0 and the length in seconds, every seek
    // and the audible position are all unchanged.
    //
    // A device-rate file (ABI v43) is normally ALREADY at the device rate and
    // keeps its int16 samples as they are, played at its stored step; only one
    // made for a lower rate is scaled to its real level and converted again.
    const bool scaledForConversion = storedStep_ > 0.0f && willConvert;
    if (scaledForConversion)
        applyStoredStepBeforeConversion(decoded, (double) storedStep_ * 32768.0);

    playbackSampleRate_ = fileRate;
    resampleReport_ = dcload::ResampleReport{};
    if (willConvert && dcload::convertDecodedBuffer(decoded, fileRate, devRate,
                                                    &resampleReport_))
        playbackSampleRate_ = devRate;

    try
    {
        memorySource_ = std::make_unique<Int16PreloadSource>(decoded);
    }
    catch (const std::bad_alloc&)
    {
        playbackSampleRate_ = 0.0;
        engine_.releasePreloadBytes(bytes);
        return false;
    }

    // checkAgainstReader guaranteed a 16-bit PCM decode, which the quantiser
    // always holds at 1/32768, so the adopt cannot fail; if it ever did, stream
    // (where StoredStepGainSource applies the step) rather than play it wrong.
    if (storedStep_ > 0.0f && ! scaledForConversion && ! memorySource_->adoptStoredStep(storedStep_))
    {
        memorySource_.reset();
        playbackSampleRate_ = 0.0;
        engine_.releasePreloadBytes(bytes);
        return false;
    }

    // The reservation was for heldFrames; charge what is really held (the
    // resampler's own length can differ by a frame) and give back the rest.
    const int64_t held = memorySource_->bytesHeld();
    if (held < bytes) engine_.releasePreloadBytes(bytes - held);
    preloadedBytes_ = std::min(held, bytes);

    return true;
}

// ABI v43 (DotCharter #435): the load-time conversion above, done once into a
// file. A host that caches decoded songs (DotCharter's PSARC audio cache) used
// to pay the upsample on EVERY open of a 44.1 kHz song on a 48 kHz device --
// ~420 ms for a 10:40 song. It now writes the converted song once, in place of
// the original, and opens that instead.
//
// BIT-IDENTICAL BY CONSTRUCTION, not by tolerance: the steps are the preload's
// own -- the same formats (basic + .wem, as every engine registers), the same
// decodeWholeReader, the same stored-step honouring, the same loadWillUpsample
// gate, the same dcload::upsampleDecodedBuffer call and the same
// Int16PreloadSource quantiser. What is written is EXACTLY what the preload
// would hold, at 16 bits a sample: its int16 samples, plus its step in a dcps
// chunk (DeviceRateWav.h). JUCE's 16-bit reader decodes those samples as
// v/32768, which re-quantises to v at 1/32768 (the bit-exact round trip
// Int16PreloadSource documents), and the open then adopts the stored step
// (Int16PreloadSource::adoptStoredStep). So a loud master whose upsample
// overshot full scale -- a step coarser than 1/32768 -- is bit-exact too, at the
// same size as any 16-bit file at that rate.
// The written file is at `deviceRate`, so opening it on that device converts
// nothing (playbackSampleRate_ == fileSampleRate_ == the device rate) and
// holds the same frame count; length and every seek are unchanged.
void convertFileForDevice(const juce::File& src, const juce::File& dst,
                          double deviceRate, DeviceRateConversion& out)
{
    out = DeviceRateConversion{};

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    fm.registerFormat(new dcwem::WemAudioFormat(), false);
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(src));
    if (reader == nullptr)
    {
        out.error = "convert_for_device: cannot open " + src.getFullPathName();
        return;
    }

    // The source may itself be a device-rate file (made for another device
    // rate): the load honours its step, so the conversion must as well.
    auto stored = devwav::probeFile(src);
    devwav::checkAgainstReader(stored, *reader);
    if (stored.kind == devwav::Kind::Damaged)
    {
        out.error = "convert_for_device: damaged device-rate file (" + stored.why + ") " + src.getFullPathName();
        return;
    }

    const double  fileRate = reader->sampleRate;
    const int64_t frames   = reader->lengthInSamples;
    const int     channels = (int) reader->numChannels;
    if (! loadWillUpsample(deviceRate, fileRate)) return;   // nothing to convert: no file

    if (frames <= 0 || channels <= 0 || frames > (int64_t) std::numeric_limits<int>::max())
    {
        out.error = "convert_for_device: unsupported length or channel count";
        return;
    }

    juce::AudioBuffer<float> decoded;
    if (! decodeWholeReader(*reader, channels, (int) frames, decoded))
    {
        out.error = "convert_for_device: decode failed";
        return;
    }
    reader.reset();

    applyStoredStepBeforeConversion(decoded, stored.decodeGain());
    if (! upsampleDecodedBuffer(decoded, fileRate, deviceRate, out.report))
    {
        out.error = "convert_for_device: rate conversion failed";
        return;
    }

    std::unique_ptr<Int16PreloadSource> held;
    try { held = std::make_unique<Int16PreloadSource>(decoded); }
    catch (const std::bad_alloc&)
    {
        out.error = "convert_for_device: out of memory";
        return;
    }
    decoded.setSize(0, 0);   // only the int16 copy is written

    if (dst.exists() && ! dst.deleteFile())
    {
        out.error = "convert_for_device: cannot replace " + dst.getFullPathName();
        out.writeFailed = true;
        return;
    }

    juce::String why;
    if (! devwav::write(dst, *held, (uint32_t) deviceRate, (uint32_t) std::llround(fileRate), why))
    {
        out.error = "convert_for_device: " + why + " " + dst.getFullPathName();
        out.writeFailed = true;
        dst.deleteFile();
        return;
    }

    out.bits   = 16;
    out.frames = held->numFrames();
    out.step   = held->step();
}

// STREAMING sources and the device rate (JuceSharp #33). AudioTransportSource
// would wrap the reader in a BufferingAudioSource and correct the rate with
// juce::ResamplingAudioSource -- linear, dull upward, aliasing downward. So the
// read-ahead is built HERE, with exactly the parameters the transport used
// (32768 frames, the engine's read-ahead thread, two channels), the polyphase
// stage (PolyphaseStreamSource.h) goes on top at the device rate, and the
// transport is told that rate: its own resampler then sits at ratio 1.0, the
// same bit-exact passthrough the converted preload gets. Load thread.
bool TransportSource::tryWireStreamingResampler(juce::PositionableAudioSource& input)
{
    const double devRate  = engine_.currentSampleRate();
    const double fileRate = fileSampleRate_;
    if (! (devRate > 0.0) || ! (fileRate > 0.0) || devRate == fileRate) return false;

    try
    {
        streamBuffer_ = std::make_unique<juce::BufferingAudioSource>(
            &input, engine_.readAheadThread(), /*deleteSourceWhenDeleted*/ false,
            /*numberOfSamplesToBuffer*/ 32768, /*numberOfChannels*/ 2);
    }
    catch (const std::bad_alloc&) { streamBuffer_.reset(); return false; }

    streamResampler_ = dcload::PolyphaseStreamSource::create(*streamBuffer_, fileRate, devRate,
                                                            /*channels*/ 2, &resampleReport_);
    if (streamResampler_ == nullptr)
    {
        streamBuffer_.reset();
        resampleReport_ = dcload::ResampleReport{};
        return false;
    }
    return true;
}

// Why this decodes into RAM when it can, rather than streaming everything.
//
// juce::BufferingAudioSource holds ONE lock across the decode: its read-ahead
// thread takes `callbackLock` in readBufferSection() and calls the reader's
// getNextAudioBlock INSIDE it (juce_BufferingAudioSource.cpp), while the audio
// thread takes the same lock in its own getNextAudioBlock. So the audio callback
// blocks for exactly as long as a chunk takes to read and decode -- unbounded,
// and worst for a chart with several stem sources sharing one read-ahead
// thread. (The stem cache stores Opus, but StemCacheStore.ExtractAsync always
// decodes it to WAV before this engine ever opens a stem -- DotCharter's
// StemCacheStore.cs. What lands here is plain WAV, same as everything else;
// the point stands for any multi-stream chart regardless of source codec.)
//
// The owner's 2026-08-23 session logged 22 deadline overruns in 11,030 callbacks
// with the rig at 3-5% of its block budget and one callback at 503 ms: the
// callback was not computing, it was waiting on that lock. Neither RT gate could
// see it -- rt_lock_sites.txt only knows mutexes we wrap in DCA_RT_LOCK_GUARD,
// and this one lives inside JUCE.
//
// Int16PreloadSource::getNextAudioBlock (like the juce::MemoryAudioSource it
// replaced, #411) is a multiply-copy loop with no lock, no allocation and no
// I/O, so a preloaded source removes the mechanism outright instead of
// shortening the hold.
void TransportSource::wireReaderToTransport()
{
    const bool preloaded = tryPreloadIntoMemory();

    if (! preloaded)
    {
        readerSource_ = std::make_unique<juce::AudioFormatReaderSource>(reader_.get(), false);
        readerSource_->setLooping(false);
        // ABI v43: a device-rate file streams at its stored step too.
        if (storedStep_ > 0.0f && storedStep_ != devwav::kBaseStep)
            storedStepGain_ = std::make_unique<devwav::StoredStepGainSource>(
                *readerSource_, (float) ((double) storedStep_ * 32768.0));
    }

    transport_ = std::make_unique<juce::AudioTransportSource>();
    // Let JUCE's transport handle file->device SR conversion; Rubber Band only
    // does user-driven tempo/pitch shifting on the already-device-rate stream.
    //
    // readAheadBufferSize 0 + no background thread is what makes the preloaded
    // path lock-free: AudioTransportSource only wraps the source in a
    // BufferingAudioSource when readAheadSize > 0.
    if (preloaded)
        // playbackSampleRate_, NOT fileSampleRate_: the buffer may already have
        // been converted to the device rate offline, in which case telling the
        // transport the FILE's rate would make it resample a second time -- and
        // play the song at the wrong speed.
        transport_->setSource(memorySource_.get(),
                              /*readAheadBufferSize*/ 0,
                              /*backgroundThread*/    nullptr,
                              /*sourceSampleRateToCorrectFor*/ playbackSampleRate_);
    else
    {
        auto* streamIn = storedStepGain_ != nullptr
            ? storedStepGain_.get()
            : static_cast<juce::PositionableAudioSource*>(readerSource_.get());
        if (tryWireStreamingResampler(*streamIn))
            // Already at the device rate: the transport's resampler is a
            // passthrough, and its read-ahead is the one built above.
            transport_->setSource(streamResampler_.get(),
                                  /*readAheadBufferSize*/ 0,
                                  /*backgroundThread*/    nullptr,
                                  /*sourceSampleRateToCorrectFor*/ engine_.currentSampleRate());
        else
            transport_->setSource(streamIn,
                                  /*readAheadBufferSize*/ 32768,
                                  /*backgroundThread*/    &engine_.readAheadThread(),
                                  /*sourceSampleRateToCorrectFor*/ fileSampleRate_);
    }

    // Name the converter that is left when the engine's own did not run: a
    // rate the transport still has to correct goes through JUCE's linear one.
    {
        const double devRate  = engine_.currentSampleRate();
        const double heldRate = preloaded ? playbackSampleRate_
                              : (streamResampler_ != nullptr ? devRate : fileSampleRate_);
        if (resampleReport_.converter == dcload::Converter::None
            && devRate > 0.0 && heldRate > 0.0 && heldRate != devRate)
        {
            resampleReport_.converter = dcload::Converter::TransportLinear;
            resampleReport_.fromRate  = heldRate;
            resampleReport_.toRate    = devRate;
        }
    }

    if (preloaded)
    {
        // Nothing downstream of here can reach either of these again: the whole
        // file is already decoded into memorySource_, which reads its own
        // samples and only those, readerSource_ is null on this path, and fileSampleRate_
        // was cached before the decode.
        //
        // Holding them is pure footprint, and the openMemory case is the
        // expensive one: the PSARC pipeline hands over decoded 16-bit WAV, so
        // memoryBytes_ is about the size of the 16-bit preload built from it --
        // ~53 MB for a five-minute stereo song, held for the life of the
        // source for nothing.
        // reader_ is smaller but not free: an open file handle plus Opus /
        // Vorbis / FLAC decoder state, per stem, for a whole session.
        //
        // Order matters: reader_ owns the MemoryInputStream that points INTO
        // memoryBytes_, so it goes first.
        reader_.reset();
        memoryBytes_.reset();
    }

    const int channels = 2;
    stretcher_ = std::make_unique<RubberBandAudioSource>(transport_.get(), false, channels,
                                                        &engine_.stretchQualityFlag());
    stretcher_->setBaseRatio(1.0);

    // Load thread. The device pointer would be a use-after-free waiting for a
    // reconfigure; the snapshot is a value.
    if (const auto snap = engine_.deviceSnapshot(); snap.open && snap.sampleRate > 0)
        stretcher_->prepareToPlay(snap.blockFrames, (double) snap.sampleRate);
    else
        stretcher_->prepareToPlay(512, fileSampleRate_ > 0 ? fileSampleRate_ : 44100.0);

    panner_ = std::make_unique<PanningAudioSource>(stretcher_.get(), false);

    // Re-apply bypass flag if this source was previously grouped — the
    // freshly-constructed stretcher defaults to active.
    if (stretcherBypassed_) stretcher_->setBypass(true);

    if (! attached_)
    {
        engine_.addSource(this);
        attached_ = true;
    }
    if (std::exchange(wasSongClock_, false))
        engine_.setSongClockSource(this);
}

juce::AudioSource* TransportSource::sourceForMixer() noexcept
{
    return panner_.get();
}

void TransportSource::play()
{
    if (transport_) transport_->start();
}

void TransportSource::pause()
{
    if (transport_) transport_->stop();
}

void TransportSource::stop()
{
    if (transport_)
    {
        transport_->stop();
        moveTransportTo(0.0);
    }
    seekGeneration_.fetch_add(1, std::memory_order_release);
}

void TransportSource::moveTransportTo(double seconds)
{
    // The move and the stretcher reset are ONE step (repositionInner). Before
    // JuceSharp #32 a transport seek never reached the stretcher at all: a
    // seek while slowed played out the pre-seek read-ahead first, and a
    // stretcher that had run could never hand back to the unity fast path.
    if (stretcher_) stretcher_->repositionInner([this, seconds] { transport_->setPosition(seconds); });
    else            transport_->setPosition(seconds);
    // A grouped stem plays through the shared stem-group stretcher instead.
    if (stretcherBypassed_) engine_.resetStemGroupStretcher();
}

double TransportSource::getPositionSeconds() const
{
    return transport_ ? transport_->getCurrentPosition() : 0.0;
}

double TransportSource::getAudiblePositionSeconds() const
{
    if (! transport_) return 0.0;
    const double raw = transport_->getCurrentPosition();
    if (! stretcher_) return raw;

    const int    readAhead = stretcher_->getReadAheadOutputSamples();
    const float  tempo     = stretcher_->currentTempoFactor();
    const double devSr     = stretcher_->currentDeviceSampleRate();
    if (readAhead <= 0 || tempo <= 0.0f || devSr <= 0.0)
        return raw;

    // Output samples sitting in the stretcher correspond to
    // (readAhead / devSr) seconds of wall time, which is
    // (readAhead / devSr) * tempo file-seconds of un-emitted song.
    const double aheadFileSeconds = (static_cast<double>(readAhead) / devSr) * tempo;
    const double adj = raw - aheadFileSeconds;
    return adj > 0.0 ? adj : 0.0;
}

double TransportSource::getLengthSeconds() const
{
    return transport_ ? transport_->getLengthInSeconds() : 0.0;
}

void TransportSource::seekSeconds(double seconds)
{
    if (transport_) moveTransportTo(seconds);
    // AFTER the move: a block that sees the new generation sees the new position.
    seekGeneration_.fetch_add(1, std::memory_order_release);
}

void TransportSource::readSongClockRt(double& position, bool& playing, uint32_t& seekGeneration,
                                      double& tempo) const noexcept
{
    seekGeneration = seekGeneration_.load(std::memory_order_acquire);
    position = getAudiblePositionSeconds();
    playing  = transport_ != nullptr && transport_->isPlaying();
    tempo    = 1.0;
    if (stretcherBypassed_) tempo = (double) engine_.stemGroupTempo();
    else if (stretcher_)    tempo = (double) stretcher_->currentTempoFactor();
    if (! (tempo > 0.0)) tempo = 1.0;
}

void TransportSource::setVolume(float g)
{
    if (transport_) transport_->setGain(g);
}

void TransportSource::setPan(float p)
{
    if (panner_) panner_->setPan(p);
}

void TransportSource::setTempoFactor(float factor)
{
    if (stretcher_) stretcher_->setTempoFactor(factor);
}

void TransportSource::setPitchSemitones(float semitones)
{
    if (stretcher_) stretcher_->setPitchSemitones(semitones);
}

void TransportSource::setStretcherBypass(bool bypassed) noexcept
{
    stretcherBypassed_ = bypassed;
    if (stretcher_)
    {
        if (bypassed)
        {
            // Drop any user tempo/pitch state we may still hold so the
            // moment we un-bypass we start clean at 1.0 / 0 semis instead
            // of inheriting a stale value the shared bus has already
            // applied.
            stretcher_->setTempoFactor(1.0f);
            stretcher_->setPitchSemitones(0.0f);
        }
        stretcher_->setBypass(bypassed);
    }
}

// --- VST input chain -----------------------------------------------------

bool AudioEngine::attachInputPlugin(juce::AudioPluginInstance* plugin, int player)
{
    if (plugin == nullptr) return false;
    if (! validPlayer(player)) return false;

    const double sr   = deviceSr_   > 0   ? deviceSr_   : 44100.0;
    const int    blk  = deviceBlock_ > 0  ? deviceBlock_ : 512;

    // Force a stereo-in / stereo-out layout when the plugin supports it.
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses .add(juce::AudioChannelSet::stereo());
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    plugin->setBusesLayout(layout);
    plugin->enableAllBuses();
    plugin->setNonRealtime(false);
    plugin->prepareToPlay(sr, blk);

    // `current` is deliberately held until AFTER the barrier below. The
    // callback copies the snapshot for the whole of its chain pass; if this
    // thread dropped the old one first, the callback's copy would be the last
    // owner and the vector would be freed on the audio thread — the shape
    // DspChain::publishLocked was caught doing by the RT gate's structure
    // storm (435 frees in one run). The publisher owns the last reference.
    std::shared_ptr<const std::vector<juce::AudioPluginInstance*>> retired;
    {
        std::lock_guard<std::mutex> lk(pluginMutex_);
        auto& slot = inputChain_[(size_t) player];
        auto current = std::atomic_load_explicit(&slot, std::memory_order_acquire);
        auto next = std::make_shared<std::vector<juce::AudioPluginInstance*>>();
        if (current) next->reserve(current->size() + 1);
        if (current) next->insert(next->end(), current->begin(), current->end());
        next->push_back(plugin);
        retired = std::atomic_exchange_explicit(&slot,
            std::shared_ptr<const std::vector<juce::AudioPluginInstance*>>(std::move(next)),
            std::memory_order_acq_rel);
    }
    if (retired)
    {
        std::lock_guard<std::mutex> barrier(inputChainProcessMutex_[(size_t) player]);
    }
    // A new chain is a new chance for every plugin in it (W5-E7).
    inputChainGeneration_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void AudioEngine::detachInputPlugin(juce::AudioPluginInstance* plugin)
{
    if (plugin == nullptr) return;

    // Detach from EVERY seat. The caller is about to destroy the instance, so
    // "remove it wherever it is" is the only safe contract: an instance that
    // was moved between seats, or attached to two, must not survive in one of
    // them as a dangling pointer.
    bool removedAny = false;
    // The retired snapshots outlive the barrier on purpose — see
    // attachInputPlugin: the publisher, never the callback, drops the last
    // reference.
    std::array<std::shared_ptr<const std::vector<juce::AudioPluginInstance*>>, kMaxPlayers> retired;
    {
        std::lock_guard<std::mutex> lk(pluginMutex_);
        for (size_t p = 0; p < inputChain_.size(); ++p)
        {
            auto& slot = inputChain_[p];
            auto current = std::atomic_load_explicit(&slot, std::memory_order_acquire);
            if (current == nullptr || current->empty()) continue;
            auto next = std::make_shared<std::vector<juce::AudioPluginInstance*>>(*current);
            const auto before = next->size();
            next->erase(std::remove(next->begin(), next->end(), plugin), next->end());
            if (next->size() == before) continue;
            removedAny = true;
            retired[p] = std::atomic_exchange_explicit(&slot,
                std::shared_ptr<const std::vector<juce::AudioPluginInstance*>>(std::move(next)),
                std::memory_order_acq_rel);
            current.reset(); // `retired[p]` is the reference that survives
        }
    }
    if (! removedAny) return;

    // Wait-one-block barrier: an audio callback that loaded the OLD snapshot
    // may still be inside this plugin's processBlock. The audio thread holds
    // inputChainProcessMutex_[p] for the whole of seat p's chain pass (taken
    // before its snapshot load), so acquiring EVERY seat's mutex guarantees any
    // such block has finished — and every later block sees the new snapshot —
    // before our caller releases/destroys the instance. Mirrors
    // detachInstrumentPlugin. Taken one at a time, never nested: this is a
    // barrier, not a critical section, and holding them all at once would be
    // the only place in the engine that could deadlock against the callback.
    for (auto& m : inputChainProcessMutex_)
    {
        std::lock_guard<std::mutex> barrier(m);
    }
    // Indices have MOVED, so the faulted bitset no longer describes anything
    // (W5-E7). Bumping the generation drops it on the next block.
    inputChainGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

// --- Internal MIDI instrument bus ---------------------------------------

bool AudioEngine::attachInstrumentPlugin(juce::AudioPluginInstance* plugin)
{
    if (plugin == nullptr) return false;

    const double sr  = deviceSr_   > 0 ? deviceSr_   : 44100.0;
    const int    blk = deviceBlock_ > 0 ? deviceBlock_ : 512;

    // Most VST3 instruments expose an empty input bus + stereo output bus.
    // Try to enforce that layout; if the plugin refuses we just leave its
    // default layout in place and hope for the best.
    juce::AudioProcessor::BusesLayout layout;
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    plugin->setBusesLayout(layout);
    plugin->enableAllBuses();
    plugin->setNonRealtime(false);
    plugin->prepareToPlay(sr, blk);

    // Size the scratch buffer to the plugin's ACTUAL output channel count —
    // not a hardcoded 2 — because mono instruments, multi-output samplers
    // and surround synths can refuse our stereo layout and keep their
    // native one. processBlock expects a buffer matching the active layout.
    //
    // GROW-ONLY with the same 2x headroom as prepareMasterBus (audit #5):
    // this is the last place the buffer may legally allocate; the render
    // path now SKIPS the block instead of calling setSize on the RT thread,
    // so shrinking here would silence the instrument for good.
    const int pluginOutCh = juce::jmax(1, plugin->getTotalNumOutputChannels());
    {
        // The ALLOCATION happens outside instrumentMutex_. The audio thread
        // takes that same mutex every render block, so growing the buffer while
        // holding it made the render callback wait on a heap allocation — an
        // audible dropout whenever an instrument was attached. Size a fresh
        // buffer first, then swap it in under the lock (two moves, no alloc and
        // no free), and let the retired one release after the lock is dropped.
        int prepCh, prepSize;
        {
            std::lock_guard<std::mutex> lk(instrumentMutex_);
            prepCh   = juce::jmax(pluginOutCh, instrumentBuffer_.getNumChannels());
            prepSize = juce::jmax(juce::jmax(blk * 2, 2048), instrumentBuffer_.getNumSamples());
            if (instrumentBuffer_.getNumChannels() >= prepCh
                && instrumentBuffer_.getNumSamples() >= prepSize)
            {
                // Already big enough — grow-only, so nothing to do but retag.
                instrumentChannels_ = pluginOutCh;
                prepCh = 0;
            }
        }

        if (prepCh > 0)
        {
            juce::AudioBuffer<float> fresh(prepCh, prepSize);
            fresh.clear();

            juce::AudioBuffer<float> retired;
            {
                std::lock_guard<std::mutex> lk(instrumentMutex_);
                retired = std::move(instrumentBuffer_);
                instrumentBuffer_ = std::move(fresh);
                instrumentChannels_ = pluginOutCh;
            }
            // `retired` frees here, off the lock.
        }
    }
    // Drain any stale MIDI queued while the slot was empty. NOT reset() --
    // until this moment the SANDBOX BRIDGE leg is the fifo's consumer (it
    // drains whenever instrumentPlugin_ is null, which is right now), and
    // sendMidiShort produces from the JUCE MIDI input thread. See
    // discardQueuedMidi.
    discardQueuedMidi();
    instrumentPlugin_.store(plugin, std::memory_order_release);
    return true;
}

void AudioEngine::detachInstrumentPlugin(juce::AudioPluginInstance* plugin)
{
    if (plugin == nullptr) return;
    auto* cur = instrumentPlugin_.load(std::memory_order_acquire);
    if (cur != plugin) return;
    instrumentPlugin_.store(nullptr, std::memory_order_release);
    // Wait one block by acquiring the mutex (audio thread takes it during render).
    { std::lock_guard<std::mutex> lk(instrumentMutex_); }
    // ...and only THEN discard the queue: the store above just handed the fifo
    // to the sandbox-bridge leg, so the barrier that matters for the drain is
    // remoteBridgeMutex_, not this one. reset() under instrumentMutex_ alone
    // rewrote both index atomics under a live consumer.
    discardQueuedMidi();
}

// --- Out-of-process VST bridge (ABI v19) ----------------------------------

bool AudioEngine::setRemoteBridge(const char* shmName, const char* requestEventName,
                                  const char* responseEventName, bool enable)
{
    std::lock_guard<std::mutex> config(remoteBridgeConfigMutex_);
    if (enable)
    {
        // (Re)create under the audio barrier so the callback never observes
        // half-initialised kernel handles.
        std::lock_guard<std::mutex> barrier(remoteBridgeMutex_);
        const int sr  = deviceSr_ > 0 ? (int) deviceSr_ : 44100;
        const int blk = deviceBlock_ > 0 ? deviceBlock_ : 512;
        return remoteBridge_.enable(shmName, requestEventName, responseEventName, sr, blk);
    }

    // Disarm first (atomic flag), then acquire the audio barrier as a
    // wait-one-block fence, then release the kernel objects. Mirrors the
    // detachInputPlugin pattern.
    remoteBridge_.markDisabled();
    std::lock_guard<std::mutex> barrier(remoteBridgeMutex_);
    remoteBridge_.disable();
    // Bridge v3 parks scheduled events for the child; with the child gone they
    // have nowhere to go, and a note-on that outlived it would reach the NEXT
    // child with no note-off behind it.
    clearBridgeMidiLocked();
    return true;
}

void AudioEngine::sendMidiShort(uint8_t status, uint8_t data1, uint8_t data2) noexcept
{
    const uint32_t packed = (uint32_t) status
                          | ((uint32_t) data1 << 8)
                          | ((uint32_t) data2 << 16);
    // A negative frame is "as soon as possible", which is bit-for-bit the
    // pre-v34 behaviour of this entry point.
    sendMidiAt(packed, -1);
}

void AudioEngine::sendMidiAt(uint32_t packed, int64_t renderFrame) noexcept
{
    if (instrumentPlugin_.load(std::memory_order_acquire) == nullptr
        && ! remoteBridge_.isEnabled()) return;
    // A status byte below 0x80 is not a MIDI message, and packed == 0 is this
    // queue's EMPTY marker — accepting one would make the pending set
    // unreadable. Reject rather than forward garbage to a plugin.
    if ((packed & 0xFFu) < 0x80u) return;

    const QueuedMidiEvent ev{ packed, renderFrame };

    // SERIALISE THE PRODUCERS. midiFifo_ is SPSC and this entry point has at
    // least two writers — the JUCE MIDI input thread and whatever thread calls
    // dca_engine_send_midi_at / _send_short, which the ABI does not constrain.
    // Never taken by the audio thread; see midiProducerMutex_'s declaration.
    std::lock_guard<std::mutex> producer(midiProducerMutex_);

    int s1, n1, s2, n2;
    midiFifo_.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 + n2 < 1) { midiDroppedEvents_.fetch_add(1, std::memory_order_relaxed); return; }
    if (n1 > 0) midiQueue_[(size_t) s1] = ev;
    else if (n2 > 0) midiQueue_[(size_t) s2] = ev;
    midiFifo_.finishedWrite(n1 + n2);
    if (renderFrame >= 0) midiScheduledEvents_.fetch_add(1, std::memory_order_relaxed);
}

void AudioEngine::renderInstrumentBus(float* const* out, int numChannels, int numSamples,
                                      int64_t blockStartFrame)
{
    // ABI v38: the instrument was already rendered INTO A SEAT's rig this block
    // (dca_engine_set_instrument_route). It is not on the master as well.
    if (std::exchange(instrumentRenderedForSeat_, false)) return;
    // p2-21: nothing escapes (see renderInstrumentWork, which has the guard
    // around the plugin itself).
    try {
    if (numChannels <= 0 || out == nullptr) return;
    const int pluginCh = renderInstrumentWork(numSamples, blockStartFrame);
    if (pluginCh <= 0) return;

    // RAMP, do not step (W4-N3). See the identical block on the sandbox-bridge
    // leg in AudioEngine.h for the reasoning; `|| prev > 0` is the half that
    // keeps muting the instrument from being a one-sample cut.
    const float g    = instrumentGain_.load(std::memory_order_relaxed);
    const float prev = instrumentGainCurrent_;
    if (g <= 0.0f && prev <= 0.0f) return;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* dst = out[ch];
        if (dst == nullptr) continue;
        const int srcCh = juce::jmin(ch, pluginCh - 1);
        const float* src = instrumentBuffer_.getReadPointer(srcCh);
        rampOrScaleAdd(dst, src, numSamples, prev, g);
    }
    // AFTER the channel loop — one from/to pair for every channel.
    instrumentGainCurrent_ = g;
    } catch (...) { /* p2-21: swallow — audio thread must never propagate. */ }
}

int AudioEngine::renderInstrumentWork(int numSamples, int64_t blockStartFrame) noexcept
{
    // p2-21: This is an audio-thread method. Nothing must escape — uncaught
    // exceptions in JUCE's getNextAudioBlock would terminate the driver
    // callback. Inner processBlock already has its own try/catch; the outer
    // guard here covers buffer-resize / MIDI-queue paths that can in
    // principle throw std::bad_alloc on extreme buffer growth.
    try {
    // Fast pre-check WITHOUT the lock: the common case is "no instrument
    // attached" and must not pay a mutex acquire every block. Not
    // authoritative — the pointer is re-loaded under the lock below.
    if (instrumentPlugin_.load(std::memory_order_acquire) == nullptr
        || numSamples <= 0) return 0;

    DCA_RT_LOCK_GUARD(instrumentMutex_, "instrumentMutex");

    // Authoritative load UNDER the barrier mutex. detachInstrumentPlugin
    // stores nullptr and then acquires instrumentMutex_ as its wait-one-block
    // barrier before the caller frees the instance — loading the pointer
    // before taking the lock (the old code) left a window in which the
    // barrier was vacuous and this thread could call processBlock on a freed
    // plugin (2026-07-16 audit, finding #1). Same lock-then-load idiom as the
    // input chain and the remote bridge.
    auto* plugin = instrumentPlugin_.load(std::memory_order_acquire);
    if (plugin == nullptr) return 0;

    // Drain queued MIDI into the per-block buffer, AT ITS SAMPLE OFFSET
    // (ABI v34).
    //
    // Every event used to be stamped at sample 0 of whichever block drained it
    // — up to one whole buffer of skew, 5-10 ms, on every note of a charted
    // performance. The comment that justified it said the queue "would need to
    // carry (event, sampleOffset) pairs ... and we'd need a free-running sample
    // counter on the audio thread"; ABI v26 shipped exactly that counter
    // (renderFrames_, of which blockStartFrame is this block's value) and
    // nobody came back here.
    //
    // The shape is deliberately the SAME as ABI v26's scheduled clicks: drain
    // the lock-free fifo into an audio-thread-only PENDING set, then fire
    // everything whose frame lands inside this block at its exact offset. That
    // is what makes scheduling AHEAD legal — clamping into whichever block
    // happened to drain the event would have reintroduced the same error under
    // a different name — and it is why an out-of-order send cannot block the
    // events behind it.
    instrumentMidiPending_.clear();
    {
        int s1, n1, s2, n2;
        midiFifo_.prepareToRead(kMidiQueueDepth, s1, n1, s2, n2);
        const int total = n1 + n2;
        if (total > 0)
        {
            // W4-N4: the search RESUMES from where the last park landed instead
            // of restarting at slot 0. midiPending_ is kMidiPending (512) slots
            // and one block can drain kMidiQueueDepth (512) events — the stated
            // purpose of ABI v34 is a sequencer scheduling ahead — so the old
            // restart-from-zero scan was up to ~131,000 slot tests inside ONE
            // callback, and it got worse as occupancy rose, i.e. exactly when
            // the callback was already busiest. Allocation-free either way, so
            // the RT gate stayed green: this is a latency cliff, not a
            // correctness fault.
            //
            // A HINT, not a free list: it may point at an occupied slot (the
            // block that consumed a parked event leaves a hole behind the
            // cursor), so the loop below still WRAPS through the whole array
            // before declaring the set full. That keeps the drop path exactly
            // as conservative as it was, at one extra int of state.
            //
            // UNTESTED, DELIBERATELY, and this is the stated reason. The only
            // observable difference is TIME, so a probe would have to be a
            // wall-clock assertion, which this gate does not allow. Worse, the
            // obvious probe is VACUOUS: a SATURATED set wraps the full 512 in
            // both versions (that is the branch that then drops), so a
            // full-vs-empty timing comparison measures the one case the hint
            // cannot help — measured at 0.15 ms against 0.19 ms either way,
            // before and after, which is how this was found out rather than
            // assumed. The case the hint does help is a PARTIALLY full set with
            // free slots late in the array, and that state cannot be built
            // deterministically through the manual pump. The change is retained
            // because it is strictly better and cannot alter behaviour: the
            // wrap makes the set of slots examined identical, only the order
            // differs, and every slot is either free or not.
            auto park = [this](const QueuedMidiEvent& ev)
            {
                const size_t n = midiPending_.size();
                for (size_t k = 0; k < n; ++k)
                {
                    const size_t i = (midiPendingHint_ + k) % n;
                    if (midiPending_[i].packed == 0)
                    {
                        midiPending_[i]   = ev;
                        midiPendingHint_  = (i + 1) % n;
                        return;
                    }
                }
                // Pending set full (>512 events in flight). Drop and COUNT it:
                // a silently swallowed note-off is a stuck note, which is the
                // loudest possible way to find out about this.
                midiDroppedEvents_.fetch_add(1, std::memory_order_relaxed);
            };
            for (int i = 0; i < n1; ++i) park(midiQueue_[(size_t)(s1 + i)]);
            for (int i = 0; i < n2; ++i) park(midiQueue_[(size_t)(s2 + i)]);
            midiFifo_.finishedRead(total);
        }
    }

    {
        const double  sr           = deviceSr_ > 0.0 ? deviceSr_ : 44100.0;
        const int64_t futureFrames = (int64_t) (sr * 5.0);   // 5 s early -> drop
        for (auto& p : midiPending_)
        {
            if (p.packed == 0) continue;

            int offset = 0;
            if (p.frame >= 0)
            {
                const int64_t delta = p.frame - blockStartFrame;
                if (delta >= (int64_t) numSamples)
                {
                    // Not this block. Only reachable in the far future if the
                    // render clock rewound under us (a device restart drops
                    // renderFrames_ to 0), in which case the event would sound
                    // minutes from now — drop it rather than hold it for ever.
                    if (delta > futureFrames) p.packed = 0;
                    continue;
                }
                if (delta < 0)
                {
                    // The caller's scheduling horizon was shorter than the
                    // engine's own latency. Place it at the head of the block
                    // (the pre-v34 behaviour) and COUNT it — this counter is
                    // the only signal that says so.
                    offset = 0;
                    midiLateEvents_.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    offset = (int) delta;
                }
            }

            const uint32_t     w        = p.packed;
            const juce::uint8  bytes[3] = { (juce::uint8)(w & 0xFF),
                                            (juce::uint8)((w >> 8) & 0xFF),
                                            (juce::uint8)((w >> 16) & 0xFF) };
            instrumentMidiPending_.addEvent(juce::MidiMessage(bytes, 3), offset);
            p.packed = 0;
        }
    }

    // Render the plugin into the scratch buffer. Never setSize here (audit
    // #5): the buffer is pre-sized with 2x headroom by prepareMasterBus and
    // attachInstrumentPlugin. If a variable-block driver still hands us more,
    // skip the instrument for this one block instead of heap-allocating on
    // the RT thread — the same "skip the block" rule renderRemoteInstrument
    // already follows.
    const int pluginCh = juce::jmax(1, instrumentChannels_);
    if (instrumentBuffer_.getNumSamples() < numSamples
        || instrumentBuffer_.getNumChannels() < pluginCh)
        return 0;

    juce::AudioBuffer<float> work(instrumentBuffer_.getArrayOfWritePointers(), pluginCh, 0, numSamples);
    work.clear();
    try { plugin->processBlock(work, instrumentMidiPending_); }
    catch (...) { return 0; }
    // The rendered block stays in instrumentBuffer_ for the caller to mix —
    // onto the master (renderInstrumentBus) or into a seat (mixSeatGenerators).
    return pluginCh;
    } catch (...) { return 0; /* p2-21: swallow — audio thread must never propagate. */ }
}

// ---- Live input capture (SPSC ring, dry pre-gain pre-chain) -----------
void AudioEngine::writeInputCapture(int player,
                                    const float* const* in,
                                    int numInputChannels,
                                    int numSamples) noexcept
{
    if (! validPlayer(player)) return;
    if (numInputChannels <= 0 || in == nullptr || numSamples <= 0) return;

    auto& ring = captureRings_[(size_t) player];
    const int requested = inputCaptureChannel_[(size_t) player].load(std::memory_order_relaxed);
    constexpr uint64_t mask = kInputCaptureCapacity - 1;
    uint64_t w = ring.write.load(std::memory_order_relaxed);

    if (requested < 0)
    {
        // Mix down to mono.
        const float inv = 1.0f / static_cast<float>(numInputChannels);
        for (int i = 0; i < numSamples; ++i)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < numInputChannels; ++ch)
            {
                const float* src = in[ch];
                if (src != nullptr) sum += src[i];
            }
            ring.buf[(size_t)((w + (uint64_t)i) & mask)] = sum * inv;
        }
    }
    else
    {
        const int ch = juce::jmin(requested, numInputChannels - 1);
        const float* src = in[ch];
        if (src == nullptr)
        {
            for (int i = 0; i < numSamples; ++i)
                ring.buf[(size_t)((w + (uint64_t)i) & mask)] = 0.0f;
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
                ring.buf[(size_t)((w + (uint64_t)i) & mask)] = src[i];
        }
    }

    ring.write.store(w + (uint64_t)numSamples, std::memory_order_release);

    // Publish the {captured-total, host ticks} sync pair (seqlock). Lets the
    // managed side map capture-stream sample indices to QPC time without
    // fitting two free-running clocks. Plain stores between the seq bumps
    // are safe: readers reject any window where the seq was odd or moved.
    ring.syncSeq.fetch_add(1, std::memory_order_acq_rel);
    ring.syncSamples = w + (uint64_t)numSamples;
    ring.syncTicks   = juce::Time::getHighResolutionTicks();
    ring.syncSeq.fetch_add(1, std::memory_order_release);
}

bool AudioEngine::getInputCaptureSync(int player, uint64_t& outSamples, int64_t& outTicks) const noexcept
{
    if (! validPlayer(player)) return false;
    const auto& ring = captureRings_[(size_t) player];
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const uint32_t before = ring.syncSeq.load(std::memory_order_acquire);
        if (before == 0) return false;          // nothing captured yet
        if ((before & 1u) != 0) continue;       // writer in flight
        const uint64_t samples = ring.syncSamples;
        const int64_t  ticks   = ring.syncTicks;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (ring.syncSeq.load(std::memory_order_relaxed) == before)
        {
            outSamples = samples;
            outTicks   = ticks;
            return true;
        }
    }
    return false;
}

int AudioEngine::readInputCapture(int player, float* dest, int count) noexcept
{
    if (! validPlayer(player)) return 0;
    if (dest == nullptr || count <= 0) return 0;
    if (count > kInputCaptureCapacity) count = kInputCaptureCapacity;

    auto& ring = captureRings_[(size_t) player];
    constexpr uint64_t mask = kInputCaptureCapacity - 1;
    const uint64_t w = ring.write.load(std::memory_order_acquire);
    const uint64_t available = w; // monotonic write counter
    if (available < (uint64_t)count)
    {
        // Not enough samples yet — zero-pad the leading region so the
        // consumer always sees a fixed-size window of "latest N samples".
        const int have = (int)available;
        const int pad  = count - have;
        for (int i = 0; i < pad; ++i) dest[i] = 0.0f;
        for (int i = 0; i < have; ++i)
            dest[pad + i] = ring.buf[(size_t)(((uint64_t)i) & mask)];
        return count;
    }

    const uint64_t start = w - (uint64_t)count;
    for (int i = 0; i < count; ++i)
        dest[i] = ring.buf[(size_t)((start + (uint64_t)i) & mask)];
    return count;
}

int AudioEngine::readInputCaptureStream(int player, float* dest, int count) noexcept
{
    uint64_t ignored = 0;
    return readInputCaptureStream2(player, dest, count, ignored);
}

int AudioEngine::readInputCaptureStream2(int player, float* dest, int count, uint64_t& outStreamPosition) noexcept
{
    outStreamPosition = 0;
    if (! validPlayer(player)) return 0;
    if (dest == nullptr || count <= 0) return 0;

    auto& ring = captureRings_[(size_t) player];
    constexpr uint64_t mask = kInputCaptureCapacity - 1;
    const uint64_t w = ring.write.load(std::memory_order_acquire);
    uint64_t r = ring.streamRead.load(std::memory_order_relaxed);

    // Single non-audio consumer per PLAYER assumed (that seat's live-play
    // poller). If it fell more than one ring behind, the oldest samples were
    // overwritten — skip to the oldest still-valid sample instead of replaying
    // garbage. The caller can SEE the skip because the absolute position of
    // dest[0] is reported.
    if (w - r > (uint64_t)kInputCaptureCapacity)
        r = w - (uint64_t)kInputCaptureCapacity;

    outStreamPosition = r;
    const int n = (int)juce::jmin<uint64_t>((uint64_t)count, w - r);
    for (int i = 0; i < n; ++i)
        dest[i] = ring.buf[(size_t)((r + (uint64_t)i) & mask)];

    ring.streamRead.store(r + (uint64_t)n, std::memory_order_relaxed);
    return n;
}

int AudioEngine::readInputCaptureFrom(int player, uint64_t from, float* dest, int count,
                                      uint64_t& outFirst) noexcept
{
    outFirst = 0;
    if (! validPlayer(player)) return 0;
    if (dest == nullptr || count <= 0) return 0;

    auto& ring = captureRings_[(size_t) player];
    constexpr uint64_t mask = kInputCaptureCapacity - 1;
    const uint64_t w = ring.write.load(std::memory_order_acquire);
    uint64_t r = from;
    // A cursor from the FUTURE is a cursor into a different ring — an engine
    // rebuild restarts the stream at 0 — so the new stream is read from its
    // START (or its oldest surviving sample, below): the caller sees
    // `outFirst < from`, re-anchors, and loses nothing captured since.
    if (r > w) r = 0;
    // Fell more than a ring behind: the oldest samples are gone. Start at the
    // oldest one still valid; `outFirst > from` is the size of the gap.
    if (w - r > (uint64_t) kInputCaptureCapacity) r = w - (uint64_t) kInputCaptureCapacity;
    outFirst = r;
    const int n = (int) juce::jmin<uint64_t>((uint64_t) count, w - r);
    for (int i = 0; i < n; ++i)
        dest[i] = ring.buf[(size_t) ((r + (uint64_t) i) & mask)];
    return n;
}

// ---- Input injection / re-amp (ABI v37) -------------------------------------

bool AudioEngine::renderInputInjection(int player, int numSamples, const float* const*& outPtrs) noexcept
{
    outPtrs = nullptr;
    auto& scratch = injectionScratch_[(size_t) player];
    // Dekker handshake with swapInjectionLocked: raise BEFORE the load, both
    // seq_cst, and drop only after the last read of the take.
    injectionBusy_.store(true, std::memory_order_seq_cst);
    InputInjection* take = injection_[(size_t) player].load(std::memory_order_seq_cst);
    if (take == nullptr || (int) scratch.size() < numSamples)
    {
        // No take: live input. A block bigger than the prepared scratch keeps
        // the live input for that ONE block rather than growing on the audio
        // thread (audit #5).
        injectionBusy_.store(false, std::memory_order_seq_cst);
        return false;
    }

    float& gain = injectionGainCurrent_[(size_t) player];
    // A NEW take starts from silence: never ramp a previous take's gain out
    // over the first samples of the next one.
    if (injectionLastSerial_[(size_t) player] != take->serial)
    {
        injectionLastSerial_[(size_t) player] = take->serial;
        gain = 0.0f;
    }

    const int64_t len = (int64_t) take->samples.size();
    int64_t pos = take->position.load(std::memory_order_relaxed);
    const int64_t seek = take->seekTo.exchange(-1, std::memory_order_acq_rel);
    if (seek >= 0) pos = seek < len ? seek : (len > 0 ? len - 1 : 0);
    if (pos < 0 || pos > len) pos = 0;

    bool playing = injectionPlaying_[(size_t) player].load(std::memory_order_relaxed);
    const bool loop = injectionLoop_[(size_t) player].load(std::memory_order_relaxed);
    const double sr = injectionRate_.load(std::memory_order_relaxed);
    // 5 ms play/pause ramp: a transport button must not click.
    const float step = (float) (1.0 / (0.005 * (sr > 0.0 ? sr : 44100.0)));
    const float* src = take->samples.data();
    float* out = scratch.data();
    bool ended = false;
    for (int i = 0; i < numSamples; ++i)
    {
        const float target = playing ? 1.0f : 0.0f;
        if (gain < target)      gain = gain + step < target ? gain + step : target;
        else if (gain > target) gain = gain - step > target ? gain - step : target;
        float s = 0.0f;
        if (gain > 0.0f && len > 0)
        {
            if (pos >= len)
            {
                if (loop) pos = 0;
                else
                {
                    // End of a one-shot take: stop and rewind, so the next Play
                    // starts it again. The take itself is over, so the gain
                    // snaps rather than ramping over nothing.
                    ended   = true;
                    playing = false;
                    gain    = 0.0f;
                    pos     = 0;
                }
            }
            if (! ended) s = src[pos++] * gain;
        }
        out[i] = s;
    }
    take->position.store(pos, std::memory_order_relaxed);
    injectionBusy_.store(false, std::memory_order_seq_cst);
    if (ended) injectionPlaying_[(size_t) player].store(false, std::memory_order_relaxed);

    injectionPtrs_[(size_t) player][0] = out;
    outPtrs = injectionPtrs_[(size_t) player].data();
    return true;
}

void AudioEngine::swapInjectionLocked(int player, std::unique_ptr<InputInjection> next)
{
    InputInjection* raw = next.get();
    if (raw != nullptr) raw->serial = ++injectionSerialNext_;
    InputInjection* old = injection_[(size_t) player].exchange(raw, std::memory_order_seq_cst);
    if (old == nullptr && raw != nullptr) injectionActive_.fetch_add(1, std::memory_order_relaxed);
    else if (old != nullptr && raw == nullptr) injectionActive_.fetch_sub(1, std::memory_order_relaxed);

    std::unique_ptr<InputInjection> retired = std::move(injectionOwned_[(size_t) player]);
    injectionOwned_[(size_t) player] = std::move(next);
    if (retired == nullptr) return;

    // Wait out any block that loaded `old` before the exchange. The callback
    // holds the flag for one seat's render — microseconds — so this is a
    // handful of yields. BOUNDED: a wedged callback must not hang the caller,
    // and a leak beats a free under a live read.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (injectionBusy_.load(std::memory_order_seq_cst))
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            engineLog(kLogWarn, "Input injection: the audio callback held the take past 2 s; "
                                "the replaced take is leaked rather than freed under it.");
            (void) retired.release();
            return;
        }
        std::this_thread::yield();
    }
    retired.reset();
}

bool AudioEngine::setInputInjection(int player, const float* mono, int64_t frames, bool loop)
{
    if (! validPlayer(player) || mono == nullptr || frames <= 0) return false;
    // Allocated and filled HERE, on the caller's thread, before the callback
    // can see it — the take is immutable from its publication on.
    auto take = std::make_unique<InputInjection>();
    take->samples.assign(mono, mono + frames);
    std::lock_guard<std::mutex> lk(injectionMutex_);
    // Paused BEFORE the new take is visible, so it never starts playing off
    // the previous take's transport state.
    injectionPlaying_[(size_t) player].store(false, std::memory_order_relaxed);
    injectionLoop_[(size_t) player].store(loop, std::memory_order_relaxed);
    swapInjectionLocked(player, std::move(take));
    return true;
}

bool AudioEngine::clearInputInjection(int player)
{
    if (! validPlayer(player)) return false;
    std::lock_guard<std::mutex> lk(injectionMutex_);
    injectionPlaying_[(size_t) player].store(false, std::memory_order_relaxed);
    swapInjectionLocked(player, nullptr);
    return true;
}

bool AudioEngine::setInputInjectionPlaying(int player, bool playing)
{
    if (! validPlayer(player)) return false;
    std::lock_guard<std::mutex> lk(injectionMutex_);
    if (injectionOwned_[(size_t) player] == nullptr) return false;
    injectionPlaying_[(size_t) player].store(playing, std::memory_order_relaxed);
    return true;
}

bool AudioEngine::setInputInjectionLoop(int player, bool loop)
{
    if (! validPlayer(player)) return false;
    std::lock_guard<std::mutex> lk(injectionMutex_);
    if (injectionOwned_[(size_t) player] == nullptr) return false;
    injectionLoop_[(size_t) player].store(loop, std::memory_order_relaxed);
    return true;
}

bool AudioEngine::seekInputInjection(int player, int64_t frame)
{
    if (! validPlayer(player)) return false;
    std::lock_guard<std::mutex> lk(injectionMutex_);
    auto* take = injectionOwned_[(size_t) player].get();
    if (take == nullptr) return false;
    const int64_t len = (int64_t) take->samples.size();
    if (frame < 0) frame = 0;
    if (frame >= len) frame = len > 0 ? len - 1 : 0;
    take->seekTo.store(frame, std::memory_order_release);
    // Read back at once: a paused take has no block to publish the move.
    take->position.store(frame, std::memory_order_relaxed);
    return true;
}

bool AudioEngine::getInputInjectionStatus(int player, int64_t& position, int64_t& length,
                                          bool& playing, bool& loop)
{
    position = 0; length = 0; playing = false; loop = false;
    if (! validPlayer(player)) return false;
    std::lock_guard<std::mutex> lk(injectionMutex_);
    const auto* take = injectionOwned_[(size_t) player].get();
    if (take == nullptr) return false;
    length   = (int64_t) take->samples.size();
    position = take->position.load(std::memory_order_relaxed);
    playing  = injectionPlaying_[(size_t) player].load(std::memory_order_relaxed);
    loop     = injectionLoop_[(size_t) player].load(std::memory_order_relaxed);
    return true;
}

// ---- Chart source (ABI v38) -------------------------------------------------

namespace {
/// Bounded Dekker wait shared by the chart source's publishers: true once no
/// callback is inside the guarded section, false after 2 s (a wedged callback).
bool waitForCallbackFlag(const std::atomic<bool>& busy) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (busy.load(std::memory_order_seq_cst))
    {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}
} // namespace

bool AudioEngine::setSongClockSource(TransportSource* source)
{
    std::lock_guard<std::mutex> lk(songClockMutex_);
    if (source != nullptr)
    {
        std::lock_guard<std::mutex> sl(sourcesMutex_);
        if (sources_.count(source) == 0) return false;   // not attached to THIS engine
    }
    // A replaced source needs no wait: it is still alive, and it calls
    // forgetSongClockSource (which does wait) before it tears anything down.
    songClockSource_.store(source, std::memory_order_seq_cst);
    return true;
}

bool AudioEngine::forgetSongClockSource(TransportSource* source) noexcept
{
    if (source == nullptr) return false;
    try
    {
        std::lock_guard<std::mutex> lk(songClockMutex_);
        TransportSource* expected = source;
        if (! songClockSource_.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst))
            return false;
    }
    catch (...)
    {
        // The lock failed to construct; unbind without it — the wait below is
        // what protects the source, not the mutex.
        TransportSource* expected = source;
        if (! songClockSource_.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst))
            return false;
    }
    if (! waitForCallbackFlag(songClockBusy_))
        engineLog(kLogWarn, "Song clock: the audio callback held the clock source past 2 s while it was "
                            "being torn down.");
    return true;
}

void AudioEngine::sampleSongClock() noexcept
{
    dcsynth::SongClockSample c;
    // Dekker handshake with forgetSongClockSource: raise BEFORE the load, both
    // seq_cst, drop after the last read of the source.
    songClockBusy_.store(true, std::memory_order_seq_cst);
    if (auto* s = songClockSource_.load(std::memory_order_seq_cst))
    {
        double pos = 0.0, tempo = 1.0; bool playing = false; uint32_t gen = 0;
        s->readSongClockRt(pos, playing, gen, tempo);
        c.valid = true; c.position = pos; c.playing = playing; c.seekGeneration = gen; c.tempo = tempo;
    }
    songClockBusy_.store(false, std::memory_order_seq_cst);
    songClock_ = c;
}

void AudioEngine::retireChartLocked(std::unique_ptr<dcsynth::ChartNoteTable> table,
                                    std::unique_ptr<ChartVoiceSlot> voice)
{
    if (table == nullptr && voice == nullptr) return;
    if (! waitForCallbackFlag(chartBusy_))
    {
        engineLog(kLogWarn, "Chart source: the audio callback held a note table / voice past 2 s; "
                            "the replaced one is leaked rather than freed under it.");
        (void) table.release();
        (void) voice.release();
        return;
    }
    table.reset();
    voice.reset();
}

namespace {
int64_t steadyMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
// A replaced voice gets this long to fade (it needs ~10 ms of audio), and a
// disabled one this long to fall silent, before the reaper frees it anyway —
// a device with no callbacks running never reports either.
constexpr int64_t kChartFadeDeadlineMs    = 400;
constexpr int64_t kChartReleaseDeadlineMs = 400;
// #417: a new chart that leaves at least this much decoded for nothing gets a
// voice on FRESH packs (cross-faded in) instead of keeping the old zones; an
// edit's few new zones are decoded into the live packs.
constexpr int64_t kChartStaleZoneBytes    = 8 * 1024 * 1024;
// While a lazy voice is published the reaper looks for the audio thread's
// zone requests this often (the callback cannot signal it).
constexpr int     kChartZonePollMs        = 20;
} // namespace

std::unique_ptr<AudioEngine::ChartVoiceSlot> AudioEngine::buildChartVoice(int kind, const dcvoice::InstrumentConfig& config,
                                                                          const dcsynth::ChartNoteTable* preload, bool freshPacks)
{
    auto slot = std::make_unique<ChartVoiceSlot>();
    slot->voice = dcsynth::makeVoice(kind, config);
    if (slot->voice == nullptr) return nullptr;
    slot->kind   = kind;
    slot->config = config;
    slot->voice->useLazyZones(freshPacks);
    const auto snap = deviceSnapshot();
    slot->preparedRate  = snap.open && snap.sampleRate > 0 ? (double) snap.sampleRate : 48000.0;
    slot->preparedBlock = juce::jmax(snap.blockFrames * 2, 2048);
    slot->voice->prepare(slot->preparedRate, slot->preparedBlock);   // the pack's zone table loads here
    // #417: only the zones the chart can play are decoded, here, before the
    // voice is published - never on the callback.
    if (preload != nullptr) slot->voice->preloadZones(*preload);
    slot->sampled = slot->voice->sampled();
    std::vector<std::shared_ptr<const dcvoice::SamplePack>> lazy;
    slot->voice->lazyPacks(lazy);
    slot->lazyZones = ! lazy.empty();
    return slot;
}

void AudioEngine::retireFadingLocked(int player)
{
    auto& cs = chart_[(size_t) player];
    if (cs.fadingOwned == nullptr) return;
    cs.fading.store(nullptr, std::memory_order_seq_cst);
    retireChartLocked(nullptr, std::move(cs.fadingOwned));
}

void AudioEngine::publishChartVoiceLocked(int player, std::unique_ptr<ChartVoiceSlot> next, bool fade)
{
    auto& cs = chart_[(size_t) player];
    // The device may have restarted at another rate while the pack loaded.
    const auto snap = deviceSnapshot();
    const double sr = snap.open && snap.sampleRate > 0 ? (double) snap.sampleRate : 48000.0;
    const int block = juce::jmax(snap.blockFrames * 2, 2048);
    if (next != nullptr && (next->preparedRate != sr || next->preparedBlock < block))
    {
        next->voice->prepare(sr, block);   // the pack is loaded: cheap
        next->preparedRate  = sr;
        next->preparedBlock = block;
    }
    // A fade still in flight (two switches within a few ms) is cut short.
    retireFadingLocked(player);
    auto old = std::move(cs.voiceOwned);
    if (next != nullptr) next->serial = ++chartSerialNext_;
    if (old != nullptr && fade && snap.open)
    {
        // FADING first, then the new voice: a block that sees the new voice
        // must also see the old one fading, or it would drop it for a block.
        old->fadeStarted = false;
        old->fadeDone.store(false, std::memory_order_relaxed);
        cs.fading.store(old.get(), std::memory_order_seq_cst);
        cs.fadingOwned  = std::move(old);
        cs.fadeDeadline = steadyMs() + kChartFadeDeadlineMs;
        wakeChartReaperLocked();
    }
    cs.voice.store(next.get(), std::memory_order_seq_cst);
    const bool lazy = next != nullptr && next->lazyZones;
    cs.voiceOwned = std::move(next);
    if (old != nullptr) retireChartLocked(nullptr, std::move(old));
    if (lazy) wakeChartReaperLocked();   // it polls for the voice's zone requests (#417)
}

const dcvoice::InstrumentConfig& AudioEngine::requestedConfigLocked(int player) const
{
    const auto& cs = chart_[(size_t) player];
    return cs.requestedKind == DCA_CHART_VOICE_BASS ? cs.bassConfig : cs.guitarConfig;
}

bool AudioEngine::chartVoiceMatchesLocked(int player) const
{
    const auto& cs = chart_[(size_t) player];
    if (cs.voiceOwned == nullptr || cs.voiceOwned->kind != cs.requestedKind) return false;
    if (cs.requestedKind == DCA_CHART_VOICE_PLACEHOLDER) return true;
    return cs.voiceOwned->config == requestedConfigLocked(player);
}

bool AudioEngine::syncChartVoice(int player, bool fade)
{
    auto& cs = chart_[(size_t) player];
    int kind;
    dcvoice::InstrumentConfig config;
    const dcsynth::ChartNoteTable* table = nullptr;
    {
        std::lock_guard<std::mutex> lk(chartMutex_);
        if (chartVoiceMatchesLocked(player))
        {
            cs.releasePending = false;
            return true;
        }
        kind   = cs.requestedKind;
        config = requestedConfigLocked(player);
        table  = cs.tableOwned.get();
    }
    // Built OUTSIDE chartMutex_: a pack load must never block a status poll or
    // a device start. The seat's controlMutex keeps the request stable - and
    // the table alive (setChartNotes takes it too).
    auto next = buildChartVoice(kind, config, table, false);
    if (next == nullptr) return false;
    std::lock_guard<std::mutex> lk(chartMutex_);
    cs.releasePending = false;
    publishChartVoiceLocked(player, std::move(next), fade);
    return true;
}

void AudioEngine::wakeChartReaperLocked()
{
    chartReaperWork_ = true;
    if (! chartReaper_.joinable() && ! chartReaperStop_)
        chartReaper_ = std::thread([this] { chartReaperLoop(); });
    chartReaperCv_.notify_one();
}

bool AudioEngine::anyLazyChartVoiceLocked() const
{
    for (const auto& cs : chart_)
        if (cs.voiceOwned != nullptr && cs.voiceOwned->lazyZones) return true;
    return false;
}

bool AudioEngine::serviceChartZonesLocked(std::unique_lock<std::mutex>& lk)
{
    // The packs are SHARED owners, so the voice may be replaced or freed while
    // the lock is down and the decode still lands safely in its pack.
    std::vector<std::shared_ptr<const dcvoice::SamplePack>> packs;
    for (const auto& cs : chart_)
        if (cs.voiceOwned != nullptr && cs.voiceOwned->lazyZones && cs.voiceOwned->voice != nullptr)
            cs.voiceOwned->voice->lazyPacks(packs);
    packs.erase(std::remove_if(packs.begin(), packs.end(),
                               [](const std::shared_ptr<const dcvoice::SamplePack>& p) { return ! dcvoice::zoneRequestsPending(*p); }),
                packs.end());
    if (packs.empty()) return false;
    lk.unlock();
    int decoded = 0;
    for (const auto& p : packs) decoded += dcvoice::serviceZoneRequests(*p);
    packs.clear();   // the last owner may be this thread: free outside the lock
    lk.lock();
    return decoded > 0;
}

void AudioEngine::chartReaperLoop()
{
    std::unique_lock<std::mutex> lk(chartMutex_);
    for (;;)
    {
        if (chartReaperStop_) return;
        serviceChartZonesLocked(lk);
        if (chartReaperStop_) return;
        if (! chartReaperWork_)
        {
            // A lazy voice's zone requests come from the audio thread, which
            // cannot signal: poll for them while one is published (#417).
            if (anyLazyChartVoiceLocked())
                chartReaperCv_.wait_for(lk, std::chrono::milliseconds(kChartZonePollMs));
            else
                chartReaperCv_.wait(lk, [this] { return chartReaperStop_ || chartReaperWork_; });
            continue;
        }
        bool more = false;
        const int64_t now = steadyMs();
        for (int p = 0; p < kMaxPlayers; ++p)
        {
            auto& cs = chart_[(size_t) p];
            if (cs.fadingOwned != nullptr)
            {
                if (cs.fadingOwned->fadeDone.load(std::memory_order_acquire) || now >= cs.fadeDeadline)
                    retireFadingLocked(p);
                else
                    more = true;
            }
            if (cs.releasePending)
            {
                const bool silent = cs.runStateShared.load(std::memory_order_acquire) == 0
                                 || now >= cs.releaseDeadline;
                if (cs.enabled.load(std::memory_order_relaxed))
                    cs.releasePending = false;            // re-enabled meanwhile: keep it
                else if (silent && cs.fadingOwned == nullptr)
                {
                    // #61 part 2: the disabled seat's voice — and, with the last
                    // reference, its sample pack — goes, off the audio thread.
                    cs.voice.store(nullptr, std::memory_order_seq_cst);
                    retireChartLocked(nullptr, std::move(cs.voiceOwned));
                    cs.releasePending = false;
                }
                else
                    more = true;
            }
        }
        chartReaperWork_ = more;
        if (more)
            chartReaperCv_.wait_for(lk, std::chrono::milliseconds(3));
    }
}

void AudioEngine::stopChartReaper()
{
    {
        std::lock_guard<std::mutex> lk(chartMutex_);
        chartReaperStop_ = true;
    }
    chartReaperCv_.notify_all();
    if (chartReaper_.joinable()) chartReaper_.join();
}

void AudioEngine::prepareChartSourcesForDevice(double sampleRate, int maxBlock)
{
    std::lock_guard<std::mutex> lk(chartMutex_);
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int headroom = juce::jmax(maxBlock * 2, 2048);
    for (int p = 0; p < kMaxPlayers; ++p)
    {
        auto& cs = chart_[(size_t) p];
        cs.scratch.assign((size_t) headroom, 0.0f);
        // No callback is in flight here: a fade cannot continue across a
        // device restart, so the replaced voice goes now.
        if (cs.fadingOwned != nullptr)
        {
            cs.fading.store(nullptr, std::memory_order_seq_cst);
            cs.fadingOwned.reset();
        }
        if (cs.voiceOwned != nullptr && cs.voiceOwned->voice != nullptr)
        {
            cs.voiceOwned->voice->prepare(sr, headroom);
            cs.voiceOwned->preparedRate  = sr;
            cs.voiceOwned->preparedBlock = headroom;
        }
        cs.scheduler.reset();
        cs.gainCurrent = 0.0f;
    }
}

bool AudioEngine::setChartNotes(int player, const void* notes, int32_t count, int32_t stride,
                                const dca_chart_bend_point* bends, int32_t bendCount, std::string& error)
{
    error.clear();
    if (! validPlayer(player)) { error = "player out of range"; return false; }
    // Built and validated HERE, on the caller's thread, outside every lock: a
    // long chart is thousands of copies and the callback must never wait on it.
    std::unique_ptr<dcsynth::ChartNoteTable> next;
    if (count != 0)
    {
        next = dcsynth::ChartNoteTable::build(notes, count, stride, bends, bendCount, error);
        if (next == nullptr) return false;
    }
    auto& cs = chart_[(size_t) player];
    // #417: the live voice's packs decode the zones the new chart can play
    // BEFORE it is published, here on the caller's thread. A voice that would
    // keep many megabytes the new chart never plays (another song) is rebuilt
    // on fresh packs and cross-faded in. The seat's controlMutex keeps the
    // voice alive while this runs: it is only replaced under it, or released
    // after a disable, which also takes it.
    std::lock_guard<std::mutex> ctl(cs.controlMutex);
    ChartVoiceSlot* live = nullptr;
    {
        std::lock_guard<std::mutex> lk(chartMutex_);
        if (cs.voiceOwned != nullptr && cs.voiceOwned->lazyZones && ! cs.releasePending) live = cs.voiceOwned.get();
    }
    std::unique_ptr<ChartVoiceSlot> fresh;
    if (live != nullptr && next != nullptr && chartZonePreload_.load(std::memory_order_relaxed))
    {
        if (live->voice->staleZoneBytes(*next) >= kChartStaleZoneBytes)
            fresh = buildChartVoice(live->kind, live->config, next.get(), true);
        if (fresh == nullptr) live->voice->preloadZones(*next);
    }
    std::lock_guard<std::mutex> lk(chartMutex_);
    if (next != nullptr) next->serial = ++chartSerialNext_;
    cs.table.store(next.get(), std::memory_order_seq_cst);
    auto retired = std::move(cs.tableOwned);
    cs.tableOwned = std::move(next);
    retireChartLocked(std::move(retired), nullptr);
    // The table first: a block that pairs the new table with the old voice
    // only asks the OLD packs (about to go) for zones.
    if (fresh != nullptr) publishChartVoiceLocked(player, std::move(fresh), true);
    return true;
}

bool AudioEngine::setChartSourceEnabled(int player, bool enabled)
{
    if (! validPlayer(player)) return false;
    auto& cs = chart_[(size_t) player];
    std::lock_guard<std::mutex> ctl(cs.controlMutex);
    // A voice exists BEFORE the callback can be asked to render one: building
    // it loads the selection's pack (only that one — #50).
    if (enabled && ! syncChartVoice(player, false)) return false;
    std::lock_guard<std::mutex> lk(chartMutex_);
    const bool was = cs.enabled.exchange(enabled, std::memory_order_seq_cst);
    if (was != enabled) chartActive_.fetch_add(enabled ? 1 : -1, std::memory_order_relaxed);
    if (! enabled && cs.voiceOwned != nullptr)
    {
        // #61 part 2: release the voice and its pack once it has faded out —
        // on the reaper thread, never here and never on the audio thread.
        cs.releasePending  = true;
        cs.releaseDeadline = steadyMs() + kChartReleaseDeadlineMs;
        wakeChartReaperLocked();
    }
    return true;
}

bool AudioEngine::setChartVoice(int player, int voice)
{
    if (! validPlayer(player) || dcsynth::voiceAvailability(voice) != 1) return false;
    auto& cs = chart_[(size_t) player];
    std::lock_guard<std::mutex> ctl(cs.controlMutex);
    {
        std::lock_guard<std::mutex> lk(chartMutex_);
        cs.requestedKind = voice;
        if (! cs.enabled.load(std::memory_order_relaxed)) return true;   // built on the next enable
    }
    return syncChartVoice(player, true);
}

bool AudioEngine::setChartInstrument(int player, const dcvoice::InstrumentConfig& config)
{
    if (! validPlayer(player)) return false;
    const dcvoice::ModelSpec* model = dcvoice::findModel(config.model);
    if (model == nullptr || ! dcvoice::validConfig(config, model->bass)) return false;
    auto& cs = chart_[(size_t) player];
    std::lock_guard<std::mutex> ctl(cs.controlMutex);
    {
        std::lock_guard<std::mutex> lk(chartMutex_);
        (model->bass ? cs.bassConfig : cs.guitarConfig) = config;
        if (! cs.enabled.load(std::memory_order_relaxed)) return true;
        const bool playing = cs.requestedKind == (model->bass ? DCA_CHART_VOICE_BASS : DCA_CHART_VOICE_GUITAR);
        if (! playing) return true;
    }
    // The playing voice changes: a new one, cross-faded in.
    return syncChartVoice(player, true);
}

bool AudioEngine::getChartInstrument(int player, bool bass, dcvoice::InstrumentConfig& out)
{
    if (! validPlayer(player)) return false;
    std::lock_guard<std::mutex> lk(chartMutex_);
    const auto& cs = chart_[(size_t) player];
    out = bass ? cs.bassConfig : cs.guitarConfig;
    return true;
}

bool AudioEngine::setChartParams(int player, float gainLinear, double offsetSeconds, int mode)
{
    if (! validPlayer(player)) return false;
    if (! std::isfinite(gainLinear) || gainLinear < 0.0f || gainLinear > 4.0f) return false;
    if (! std::isfinite(offsetSeconds) || offsetSeconds < -2.0 || offsetSeconds > 2.0) return false;
    if (mode != DCA_CHART_SOURCE_MIX && mode != DCA_CHART_SOURCE_REPLACE) return false;
    auto& cs = chart_[(size_t) player];
    cs.gain  .store(gainLinear,    std::memory_order_relaxed);
    cs.offset.store(offsetSeconds, std::memory_order_relaxed);
    cs.mode  .store(mode,          std::memory_order_relaxed);
    return true;
}

bool AudioEngine::getChartStatus(int player, dca_chart_source_status& out)
{
    const int32_t bytes = out.struct_bytes;
    out = dca_chart_source_status{};
    out.struct_bytes = bytes;
    if (! validPlayer(player)) return false;
    auto& cs = chart_[(size_t) player];
    {
        std::lock_guard<std::mutex> lk(chartMutex_);
        out.note_count = cs.tableOwned != nullptr ? cs.tableOwned->count() : 0;
        out.voice      = cs.requestedKind;
        // ABI v40 (#50 / #61): the playing family's selection and what is held.
        const dcvoice::InstrumentConfig& c = requestedConfigLocked(player);
        {
            out.model            = c.model;
            out.pickup           = c.pickup;
            out.instrument_flags = c.coilSplit ? DCA_CHART_INSTRUMENT_COIL_SPLIT : 0u;
            out.sampled          = cs.voiceOwned != nullptr && cs.voiceOwned->sampled ? 1 : 0;
            // Live: a lazy pack grows as zones are decoded (#417).
            out.pack_bytes       = cs.voiceOwned != nullptr && cs.voiceOwned->voice != nullptr
                                       ? cs.voiceOwned->voice->packBytes() : 0;
            out.voice_loaded     = cs.voiceOwned != nullptr ? 1 : 0;
        }
    }
    out.enabled        = cs.enabled.load(std::memory_order_relaxed) ? 1 : 0;
    out.mode           = cs.mode.load(std::memory_order_relaxed);
    out.active_voices  = cs.scheduler.stats.activeVoices.load(std::memory_order_relaxed);
    out.clock_bound    = songClockSource_.load(std::memory_order_relaxed) != nullptr ? 1 : 0;
    out.clock_playing  = cs.scheduler.stats.clockPlaying.load(std::memory_order_relaxed);
    out.notes_started  = cs.scheduler.stats.notesStarted.load(std::memory_order_relaxed);
    out.notes_chased   = cs.scheduler.stats.notesChased.load(std::memory_order_relaxed);
    out.notes_dropped  = cs.scheduler.stats.notesDropped.load(std::memory_order_relaxed);
    out.song_seconds   = cs.scheduler.stats.songSeconds.load(std::memory_order_relaxed);
    out.gain           = cs.gain.load(std::memory_order_relaxed);
    out.offset_seconds = cs.offset.load(std::memory_order_relaxed);
    return true;
}

bool AudioEngine::setInstrumentRoute(int player) noexcept
{
    if (player != -1 && ! validPlayer(player)) return false;
    instrumentRoute_.store(player, std::memory_order_relaxed);
    return true;
}

// The cross-fade of a replaced chart voice (ABI v40).
static constexpr double kChartCrossfadeSecondsRt = 0.010;

void AudioEngine::mixSeatGenerators(int player, juce::AudioBuffer<float>& chainBuf, int numSamples,
                                    bool instrumentHere) noexcept
{
    auto& cs = chart_[(size_t) player];
    const int chans = juce::jmin(kChainChannels, chainBuf.getNumChannels());
    const bool enabled = cs.enabled.load(std::memory_order_relaxed);

    // A block bigger than the prepared scratch skips the generators for that
    // ONE block rather than growing on the audio thread (audit #5).
    // A voice REPLACED by a model / pickup change (ABI v40) keeps rendering its
    // cross-fade even if nothing else here would run.
    const bool fadingVoice = cs.fading.load(std::memory_order_relaxed) != nullptr;
    if ((enabled || cs.runState != 0 || fadingVoice) && (int) cs.scratch.size() >= numSamples)
    {
        float* mono = cs.scratch.data();
        juce::FloatVectorOperations::clear(mono, numSamples);

        // Dekker handshake with retireChartLocked: raise BEFORE the loads, drop
        // after the voice's last use.
        chartBusy_.store(true, std::memory_order_seq_cst);
        const dcsynth::ChartNoteTable* table = cs.table.load(std::memory_order_seq_cst);
        ChartVoiceSlot*                slot  = cs.voice.load(std::memory_order_seq_cst);
        dcsynth::ChartVoice*           voice = slot != nullptr ? slot->voice.get() : nullptr;

        if (enabled)
        {
            if (cs.runState != 1)
            {
                // Switched ON: a clean voice and a resync from the song clock.
                if (voice != nullptr) voice->reset();
                cs.scheduler.reset();
                cs.gainCurrent = 0.0f;
                cs.runState    = 1;
            }
            cs.scheduler.process(table, table != nullptr ? table->serial : 0,
                                 voice, slot != nullptr ? slot->serial : 0,
                                 songClock_, cs.offset.load(std::memory_order_relaxed),
                                 deviceSr_, mono, numSamples);
        }
        else
        {
            // Switched OFF: choke once, then let the voice finish its fade.
            if (cs.runState == 1)
            {
                if (voice != nullptr) voice->allNotesOff(dcsynth::NoteOffKind::Choke);
                cs.scheduler.reset();
                cs.runState = 2;
            }
            if (voice != nullptr) voice->render(mono, numSamples);
            if (voice == nullptr || voice->activeNotes() == 0) cs.runState = 0;
        }

        // The replaced voice: told ONCE to fade (~10 ms), rendered until it is
        // silent, then flagged for the reaper, which frees it. Skipped when it is
        // still the published voice (the two stores are not one atomic step).
        ChartVoiceSlot* fade = cs.fading.load(std::memory_order_seq_cst);
        if (fade != nullptr && fade != slot && fade->voice != nullptr && ! fade->fadeDone.load(std::memory_order_relaxed))
        {
            if (! fade->fadeStarted)
            {
                fade->voice->fadeOutAll((int) (kChartCrossfadeSecondsRt * deviceSr_));
                fade->fadeStarted = true;
            }
            fade->voice->render(mono, numSamples);
            if (fade->voice->activeNotes() == 0) fade->fadeDone.store(true, std::memory_order_release);
        }
        chartBusy_.store(false, std::memory_order_seq_cst);
        cs.runStateShared.store(cs.runState, std::memory_order_release);

        // A voice is somebody's arithmetic too; nothing non-finite enters a rig.
        float* scrub[1] = { mono };
        dcdsp::scrubNonFinite(scrub, 1, numSamples);
        const float target = cs.gain.load(std::memory_order_relaxed);
        rampOrScale(mono, numSamples, cs.gainCurrent, target);
        cs.gainCurrent = target;

        const bool replace = enabled && cs.mode.load(std::memory_order_relaxed) == DCA_CHART_SOURCE_REPLACE;
        for (int ch = 0; ch < chans; ++ch)
        {
            float* dst = chainBuf.getWritePointer(ch);
            if (replace) std::memcpy(dst, mono, sizeof(float) * (size_t) numSamples);
            else         juce::FloatVectorOperations::add(dst, mono, numSamples);
        }
    }

    // The routed in-process instrument (rendered before the seat loop): summed
    // to mono — a DI is mono — at the instrument gain, ramped like the master leg.
    if (instrumentHere && instrumentWorkChannels_ > 0 && (int) cs.scratch.size() >= numSamples
        && instrumentBuffer_.getNumSamples() >= numSamples)
    {
        const int ich = juce::jmin(instrumentWorkChannels_, instrumentBuffer_.getNumChannels());
        float* mono = cs.scratch.data();
        std::memcpy(mono, instrumentBuffer_.getReadPointer(0), sizeof(float) * (size_t) numSamples);
        for (int c = 1; c < ich; ++c)
            juce::FloatVectorOperations::add(mono, instrumentBuffer_.getReadPointer(c), numSamples);
        if (ich > 1) juce::FloatVectorOperations::multiply(mono, 1.0f / (float) ich, numSamples);
        float* scrub[1] = { mono };
        dcdsp::scrubNonFinite(scrub, 1, numSamples);
        const float g = instrumentGain_.load(std::memory_order_relaxed);
        rampOrScale(mono, numSamples, instrumentGainCurrent_, g);
        instrumentGainCurrent_ = g;
        for (int ch = 0; ch < chans; ++ch)
            juce::FloatVectorOperations::add(chainBuf.getWritePointer(ch), mono, numSamples);
    }
    if (instrumentHere) instrumentWorkChannels_ = 0;
}

} // namespace dotcharter
