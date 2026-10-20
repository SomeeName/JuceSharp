#pragma once

#include "LatestSnapshot.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "dotcharter_audio.h"
#include "ExternalProcessInsert.h"
#include "Int16PreloadSource.h"
#include "LoadResampler.h"
#include "PolyphaseStreamSource.h"
#include "MasterLimiter.h"
#include "MirrorOutput.h"
#include "RemoteVstBridge.h"
#include "RtDiag.h"
#include "dotcharter_vst_bridge.h"
#include "dsp/DspChain.h"
#include "dsp/DspModuleFactory.h"
#include "dsp/cores/dcdsp_droptune.h"
#include "synth/ChartScheduler.h"
#include "synth/ChartVoiceFactory.h"

#include <atomic>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace dotcharter {

class TransportSource;

/// Owns the JUCE AudioDeviceManager and the master mixer that drives both
/// song playback (TransportSource instances) and (eventually) the live-input
/// plugin chain. Thread-safe entry points are the C ABI in
/// dotcharter_audio.cpp.
class AudioEngine
{
public:
    /// ABI v28 — PER-PLAYER live-input rigs (couch co-op).
    ///
    /// The engine hosts `kMaxPlayers` INDEPENDENT live-input rigs. Each one owns
    /// its own dcrig::DspChain, its own VST3 input chain, its own mono capture
    /// ring (so live note detection can judge every player) and its own hardware
    /// INPUT CHANNEL. They all share ONE open device — this is channel routing
    /// inside a single AudioIODevice, never a second device open (an ASIO driver
    /// is typically exclusive, so two engines cannot split one interface's two
    /// inputs).
    ///
    /// `playerCount_` (dca_engine_set_player_count) decides how many are LIVE.
    /// It defaults to 1, and at 1 the audio callback runs the pre-v28 code path
    /// byte-for-byte — that is deliberate: a single-player user must not pay for,
    /// or hear any change from, a feature they are not using.
    static constexpr int kMaxPlayers = 4;

    /// Node ids are unique per ENGINE, not per chain: player `p`'s chain hands
    /// out `p * kPlayerNodeIdStride + local`. That is what lets every NODE-scoped
    /// ABI export (remove / move / bypass / set_param / get_param / node string /
    /// node branch) keep its pre-v28 signature — the player is recoverable from
    /// the id, so only the CHAIN-scoped exports (add / count / list / clear /
    /// process_buffer) needed player-indexed variants.
    ///
    /// Player 0's ids are therefore 0, 1, 2, … exactly as before v28.
    static constexpr int kPlayerNodeIdStride = 1000000;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Open the audio device described by cfg. Returns an empty string on
    /// success or the JUCE error string on failure.
    juce::String start(const dca_engine_config& cfg);

    /// Tear the device down. Safe to call multiple times.
    void stop();

    /// ABI v41 (dca_engine_create2): per-ENGINE Null-device options, set once
    /// right after construction and before the first start(). Read on the
    /// JuceHost message thread by start() and the mirror's open(). OR-ed with
    /// the process env vars (see NullAudioIODevice.h) — never a way to turn
    /// the env path off.
    void setNullDeviceOptions(bool enable, bool manual) noexcept
    {
        nullDeviceByConfig_       = enable || manual;
        nullDeviceManualByConfig_ = manual;
        mirror_.setNullDeviceOptions(nullDeviceByConfig_);
    }

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }
    juce::AudioFormatManager& formatManager() noexcept { return formatManager_; }

    /// MUTUAL EXCLUSION BETWEEN OPENING A DEVICE AND AN OFFLINE RENDER
    /// (W4-D73). The offline rig exports refuse to run while a device is open,
    /// and that check WAS a TOCTOU: nothing held the device closed between the
    /// check and a multi-minute bounce, so a device opening mid-render got its
    /// live tone retuned by the lazy `rig.prepare`. Memory safety was never the
    /// issue (`DspChain::prepare` takes both mutexes); the fault is that two
    /// callers each see a state the other is about to change.
    ///
    /// A TRY-LOCK, never a blocking one, on BOTH sides — that is the whole
    /// design. Holding this across a bounce would make `dca_engine_start` block
    /// for minutes behind a render, which is far worse for a user pressing play
    /// than the race it closes. Instead whichever arrives second is REFUSED
    /// with an honest error, which is a state both callers already handle:
    /// the offline exports answer DCA_ERR_DEVICE ("a device is in the way")
    /// and start/reconfigure answer their usual error string.
    ///
    /// Scope: held by the two offline render exports for the whole render, and
    /// by start / reconfigure / set_device_type across the device operation.
    /// Nothing on the audio thread ever touches it, so it is a plain
    /// `std::mutex` and NOT a `DCA_RT_LOCK_GUARD` — a name in
    /// `rt_lock_sites.txt` would claim the callback acquires it.
    class DeviceLifecycleLock
    {
    public:
        explicit DeviceLifecycleLock(AudioEngine& e) noexcept
            : lock_(e.deviceLifecycleMutex_, std::try_to_lock) {}
        /// false when the other side holds it — REFUSE, never wait.
        bool held() const noexcept { return lock_.owns_lock(); }
    private:
        std::unique_lock<std::mutex> lock_;
    };

    void addSource    (TransportSource* s);
    void removeSource (TransportSource* s);

    // --- Stem group (shared stretcher) -----------------------------------
    /// Flag a transport source as part of the "stem group". When a source is
    /// flagged in, its own Rubber Band stretcher is bypassed and its
    /// (un-stretched) post-gain post-pan output is routed into a single
    /// shared sub-mixer wrapped by ONE Rubber Band stretcher feeding the
    /// main mixer. This keeps multi-stem playback phase-coherent during
    /// tempo / pitch changes (independent per-stem stretchers introduce
    /// audible phasing because their windows are not aligned).
    /// Pass-through when grouped == currently-grouped state.
    void setSourceStemGrouped(TransportSource* s, bool grouped);
    /// Tempo / pitch on the shared stem-group stretcher. No-op when no
    /// sources are currently grouped.
    void setStemGroupTempo  (float factor)    noexcept;
    void setStemGroupPitch  (float semitones) noexcept;
    float  stemGroupTempo()           const noexcept { return stemGroupTempo_.load(std::memory_order_relaxed); }
    float  stemGroupPitchSemitones()  const noexcept { return stemGroupPitch_.load(std::memory_order_relaxed); }
    /// Read-ahead samples sitting in the shared group stretcher (in
    /// device-rate output samples). 0 when no group is active or when no
    /// audio has been pulled yet. Used by the host playhead compensation.
    int    stemGroupReadAheadSamples() const noexcept;
    /// A grouped stem was seeked: the shared stretcher's read-ahead is no
    /// longer the song's continuation (JuceSharp #32). Control thread; the
    /// stems move one by one, so this cannot be one step with their seeks.
    void   resetStemGroupStretcher();

    // --- Input monitoring -------------------------------------------------
    /// Output gain for live input passed through to the device output (0..N).
    /// Set to 0 to disable monitoring (default).
    void  setMonitorGain(float g) noexcept { monitorGain_.store(g, std::memory_order_relaxed); }
    /// Pre-monitor gain applied to incoming input samples (level/trim).
    void  setInputGain  (float g) noexcept { inputGain_  .store(g, std::memory_order_relaxed); }

    // --- Drop-tune pedal ---------------------------------------------------
    /// Semitone shift applied to the live input BEFORE the capture ring and
    /// the input VST chain — a digital drop pedal in front of the rig, so
    /// note detection, plugins and monitoring all hear the shifted signal.
    /// 0 = true bypass (no processing, no added latency).
    /// Seat 0's pedal — exactly setPlayerInputPitchShift(0, semitones), the same
    /// relationship setInputCaptureChannel has with setPlayerInputChannel.
    void setInputPitchShift(float semitones) noexcept { setPlayerInputPitchShift(0, semitones); }

    /// ABI v31: ONE SEAT's drop-tune pedal. Until v31 there was a single shifter
    /// applied to the raw device input BEFORE the per-player split, so on a couch
    /// with two guitars in different tunings, dropping one dropped both — and the
    /// seat that did not ask for it was detuned with nothing on screen saying so.
    /// Each seat now owns its own shifter and its own target.
    void setPlayerInputPitchShift(int player, float semitones) noexcept
    {
        if (validPlayer(player))
            inputPitchShiftSemis_[(size_t) player].store(semitones, std::memory_order_relaxed);
    }
    float playerInputPitchShift(int player) const noexcept
    {
        return validPlayer(player)
                   ? inputPitchShiftSemis_[(size_t) player].load(std::memory_order_relaxed)
                   : 0.0f;
    }
    /// ABI v25: the drop-tune pedal's quality/latency knob. There is now ONE engine
    /// (CSVD, see dcdsp_droptune.h) and this selects its splice-lag ceiling — the
    /// only real trade there is:
    ///   0 = Tight    (10 ms lag ceiling) — least delay between picks, more splices
    ///   1 = Balanced (20 ms) — the default
    ///   2 = Smooth   (35 ms) — fewest splices, best on sustained chords and the
    ///                          octave, at the cost of more delay during sustain
    /// Attack latency (~3-4 ms) is the same in all three: the transient anchor sets
    /// it, not this. The atomic is read once per callback on the audio thread.
    ///
    /// NOTE the meaning CHANGED at v25. Before, 0 = Rubber Band (>=50 ms) and
    /// 1 = a dual-head OLA. Both engines are gone.
    ///
    /// PER SEAT since v31, like the shift itself; this is seat 0's.
    void setInputPitchShiftMode(int mode) noexcept { setPlayerInputPitchShiftMode(0, mode); }

    /// ABI v31: one seat's splice-lag ceiling. Same 0/1/2 → 10/20/35 ms mapping.
    void setPlayerInputPitchShiftMode(int player, int mode) noexcept
    {
        if (! validPlayer(player)) return;
        const float lagMs = mode <= 0 ? 10.0f : (mode == 1 ? 20.0f : 35.0f);
        dropTuneMaxLagMs_[(size_t) player].store(lagMs, std::memory_order_relaxed);
    }
    /// Peak absolute input sample observed since the last call. Resets on read.
    /// Player 0's meter (the pre-v28 export). Use playerInputPeak for a seat.
    float getAndResetInputPeak() noexcept { return getAndResetPlayerInputPeak(0); }
    float getAndResetPlayerInputPeak(int player) noexcept
    {
        if (! validPlayer(player)) return 0.0f;
        return inputPeak_[(size_t) player].exchange(0.0f, std::memory_order_acq_rel);
    }

    // --- Per-player live-input rigs (ABI v28) ------------------------------
    /// How many live-input rigs run. 1 (the default) is the pre-v28 path
    /// byte-for-byte. Clamped to 1..kMaxPlayers.
    void setPlayerCount(int n) noexcept
    {
        playerCount_.store(juce::jlimit(1, kMaxPlayers, n), std::memory_order_relaxed);
    }
    int  playerCount() const noexcept { return playerCount_.load(std::memory_order_relaxed); }
    static bool validPlayer(int player) noexcept { return player >= 0 && player < kMaxPlayers; }

    /// Per-player monitor trim, MULTIPLIED into the global monitor gain when
    /// that player's processed rig output is summed into the device output.
    /// 1.0 (the default) leaves the pre-v28 level untouched — which is what
    /// keeps a single-player mix bit-identical.
    void setPlayerMonitorGain(int player, float g) noexcept
    {
        if (validPlayer(player)) playerMonitorGain_[(size_t) player].store(g, std::memory_order_relaxed);
    }
    float playerMonitorGain(int player) const noexcept
    {
        return validPlayer(player) ? playerMonitorGain_[(size_t) player].load(std::memory_order_relaxed) : 0.0f;
    }

    // --- Live input capture (dry, pre-chain, pre-gain) -------------------
    /// Select which input channel feeds player `player`'s capture ring AND —
    /// when more than one player is live — that player's DSP/VST rig. Clamped
    /// to the device's actual input count on the audio thread; -1 = mix all
    /// channels down to mono. Defaults to `player` (seat 0 → input 0, seat 1 →
    /// input 1), which is the two-guitars-on-one-interface case.
    void setInputCaptureChannel(int ch) noexcept { setPlayerInputChannel(0, ch); }
    int  inputCaptureChannel() const noexcept    { return playerInputChannel(0); }
    void setPlayerInputChannel(int player, int ch) noexcept
    {
        if (validPlayer(player)) inputCaptureChannel_[(size_t) player].store(ch, std::memory_order_relaxed);
    }
    int  playerInputChannel(int player) const noexcept
    {
        return validPlayer(player)
                   ? inputCaptureChannel_[(size_t) player].load(std::memory_order_relaxed)
                   : -1;
    }

    /// Pull up to `count` of the most-recent captured samples (mono float32,
    /// device sample rate). Returns the number actually written. Lock-free,
    /// safe to call from any non-audio thread.
    int  readInputCapture(float* dest, int count) noexcept { return readInputCapture(0, dest, count); }
    /// Streaming variant with its own cursor: returns only samples not yet
    /// consumed by a previous streaming read (0 when caught up), skipping
    /// ahead if the caller fell more than one ring behind. Use for
    /// continuous consumers (live note detection); the windowed
    /// readInputCapture above stays as-is for tuner/FFT-style consumers.
    int  readInputCaptureStream(float* dest, int count) noexcept { return readInputCaptureStream(0, dest, count); }
    int  readInputCaptureStream2(float* dest, int count, uint64_t& outStreamPosition) noexcept
    {
        return readInputCaptureStream2(0, dest, count, outStreamPosition);
    }
    bool getInputCaptureSync(uint64_t& outSamples, int64_t& outTicks) const noexcept
    {
        return getInputCaptureSync(0, outSamples, outTicks);
    }

    // Per-player rings (ABI v28). Each seat has its own cursors, so two live
    // detectors never steal each other's samples.
    int  readInputCapture(int player, float* dest, int count) noexcept;
    int  readInputCaptureStream(int player, float* dest, int count) noexcept;
    int  readInputCaptureStream2(int player, float* dest, int count, uint64_t& outStreamPosition) noexcept;
    bool getInputCaptureSync(int player, uint64_t& outSamples, int64_t& outTicks) const noexcept;
    /// ABI v37. CURSORLESS positional read: up to `count` samples starting at
    /// absolute stream position `from` (0 = the first sample ever captured).
    /// When `from` has already been overwritten the read starts at the oldest
    /// sample still in the ring, and `outFirst` says where it really began, so
    /// the caller can see (and count) the gap. The caller owns the cursor, so
    /// any number of these readers — a DI recorder, say — coexist with the
    /// streaming reader live detection drains, and none steals from another.
    int  readInputCaptureFrom(int player, uint64_t from, float* dest, int count, uint64_t& outFirst) noexcept;

    // --- Input injection / re-amp (ABI v37) -------------------------------
    // Control thread (any non-audio thread). See InputInjection below.
    /// Loads a mono take (engine rate) into `player`'s input, PAUSED at 0 with
    /// the given loop flag, replacing any take already there. The live input of
    /// that seat is muted from the next block. Copies `frames` samples.
    bool setInputInjection(int player, const float* mono, int64_t frames, bool loop);
    /// Removes the take: the seat is back on its live input next block.
    bool clearInputInjection(int player);
    bool setInputInjectionPlaying(int player, bool playing);
    bool setInputInjectionLoop(int player, bool loop);
    /// Moves the play head (clamped into the take) at the next block.
    bool seekInputInjection(int player, int64_t frame);
    /// False when no take is loaded (all outs zeroed).
    bool getInputInjectionStatus(int player, int64_t& position, int64_t& length,
                                 bool& playing, bool& loop);

    // --- External process insert (ABI v47) -------------------------------
    // Control thread (any non-audio thread). See ExternalProcessInsert.h.
    dca_status openExternalInsert(int player, const char* mappingName, const char* magic)
    {
        if (! validPlayer(player)) return DCA_ERR_INVALID_ARG;
        return extProc_[(size_t) player].open(mappingName, magic);
    }
    bool closeExternalInsert(int player)
    {
        return validPlayer(player) && extProc_[(size_t) player].close();
    }
    void getExternalInsertStats(int player, dca_extproc_stats& out)
    {
        if (validPlayer(player)) extProc_[(size_t) player].fillStats(out);
    }

    // --- Chart source (ABI v38) -------------------------------------------
    // Control thread (any non-audio thread). See ChartSourceSeat below.
    /// Binds the song clock every seat's chart source follows; null unbinds.
    /// False when `source` is not one of this engine's attached sources.
    bool setSongClockSource(TransportSource* source);
    /// Unbinds `source` if it is the clock and waits until no callback can
    /// still be reading it. Called by TransportSource before it tears its
    /// wiring down. Returns whether it WAS the clock.
    bool forgetSongClockSource(TransportSource* source) noexcept;
    /// Validates and publishes a new note table (count 0 clears). False with
    /// `error` set when the list breaks a rule; the previous table then stays.
    /// `notesBytes` is the size of the `notes` buffer (v48); -1 means the
    /// caller did not say (the v38 export), and the count and stride are trusted.
    bool setChartNotes(int player, const void* notes, int32_t count, int32_t stride,
                       const dca_chart_bend_point* bends, int32_t bendCount, std::string& error,
                       int64_t notesBytes = -1);
    /// ON builds the selected voice (loading its pack) before it publishes the
    /// flag; OFF fades the voice out and RELEASES it — and with it the sample
    /// pack's memory — on the chart reaper thread once it is silent (#61).
    bool setChartSourceEnabled(int player, bool enabled);
    /// TESTS ONLY (the RT gate's miss scenario, #417): false = a new chart's
    /// zones are NOT decoded before it is published, so its notes take the
    /// on-demand path (the nearest decoded recording, the reaper decodes).
    void setChartZonePreloadForTesting(bool on) noexcept { chartZonePreload_.store(on, std::memory_order_relaxed); }
    /// False when `voice` is not available in this build. Recorded always;
    /// built (and cross-faded in) only while the source is enabled.
    bool setChartVoice(int player, int voice);
    /// ABI v40 (#50): the instrument model / pickup / coil split of the guitar
    /// voice (a guitar model) or the bass voice (a bass model). False when the
    /// selection is not valid. Takes effect at once (a cross-fade) when that
    /// voice is the one playing, else when it is next built.
    bool setChartInstrument(int player, const dcvoice::InstrumentConfig& config);
    bool getChartInstrument(int player, bool bass, dcvoice::InstrumentConfig& out);
    bool setChartParams(int player, float gainLinear, double offsetSeconds, int mode);
    bool getChartStatus(int player, dca_chart_source_status& out);

    /// -1 = after the master (engine start), else that seat's rig input.
    bool setInstrumentRoute(int player) noexcept;
    int  instrumentRoute() const noexcept { return instrumentRoute_.load(std::memory_order_relaxed); }

    // --- Master / filter bus ---------------------------------------------
    /// Master output gain applied AFTER the mixer + before the input monitor mix.
    /// 1.0 = unity. Default 1.0.
    void setMasterGain(float g) noexcept { masterGain_.store(g, std::memory_order_relaxed); }

    // --- Master-bus ceiling (ABI v34) -------------------------------------
    /// Look-ahead soft limiter over the FINAL device buffer, before the
    /// streaming mirror pickup. **Default OFF** (v35; it was ON in v34) at a
    /// -0.3 dBFS ceiling when the host enables it. Switched off the stage is not
    /// in the path at all and the reported latency FOLLOWS the flag — see rule 1
    /// in MasterLimiter.h.
    ///
    /// Message thread. `ceilingDb` and `releaseMs` are clamped here as well as
    /// at the ABI, for the same reason the v29 EQ ranges are: a caller that
    /// caches what it wrote must not be able to disagree with the engine.
    /// A NON-FINITE ceiling or release is REJECTED — the whole call is a no-op
    /// and `enabled` does not move either. It used to substitute the defaults
    /// here while the ABI export rejected, so the same NaN meant two different
    /// things depending on which door it came through.
    /// Returns false when the arguments were rejected.
    bool setMasterLimiter(bool enabled, float ceilingDb, float releaseMs) noexcept
    {
        if (! std::isfinite(ceilingDb) || ! std::isfinite(releaseMs)) return false;
        ceilingDb = juce::jlimit(kLimiterMinCeilingDb, kLimiterMaxCeilingDb, ceilingDb);
        releaseMs = juce::jlimit(kLimiterMinReleaseMs, kLimiterMaxReleaseMs, releaseMs);
        masterLimiterCeilingLin_.store(std::pow(10.0f, ceilingDb * 0.05f), std::memory_order_relaxed);
        masterLimiterReleaseMs_ .store(releaseMs, std::memory_order_relaxed);
        masterLimiterEnabled_   .store(enabled,   std::memory_order_relaxed);
        return true;
    }
    bool  masterLimiterEnabled()   const noexcept { return masterLimiterEnabled_.load(std::memory_order_relaxed); }
    float masterLimiterCeilingDb() const noexcept
    {
        const float lin = masterLimiterCeilingLin_.load(std::memory_order_relaxed);
        return lin > 0.0f ? 20.0f * std::log10(lin) : MasterLimiter::kDefaultCeilingDb;
    }
    float masterLimiterReleaseMs() const noexcept { return masterLimiterReleaseMs_.load(std::memory_order_relaxed); }
    /// Poll-only, like the v30 node meters: a decaying hold of the deepest
    /// gain reduction applied, in POSITIVE dB. 0 when the limiter is idle.
    float masterLimiterReductionDb() const noexcept { return masterLimiter_.reductionDb(); }
    /// Samples of output delay this stage contributes, folded into
    /// dca_engine_get_output_latency_samples. **0 while it is switched off**,
    /// because while it is off it is not in the path at all — see rule 1 in
    /// MasterLimiter.h. Gated here, on the message thread that owns the flag.
    int   masterLimiterLatencySamples() const noexcept
    {
        return masterLimiterEnabled_.load(std::memory_order_relaxed)
                   ? masterLimiter_.latencySamples() : 0;
    }

    // --- Master-bus width (#31) -------------------------------------------
    /// The channel count every per-channel master-bus stage was prepared for,
    /// one field per stage, read back from the stage itself rather than from
    /// masterBusWidth_ — so a stage sized from anything else shows up as a
    /// disagreement. Message thread, between device sessions or while one runs
    /// (the sizes only change in prepareMasterBus). See prepareMasterBus for
    /// why the width is the ACTIVE output count.
    struct MasterBusWidths
    {
        int bus = 0, highPass = 0, lowPass = 0, bandPass = 0, songEq = 0, limiter = 0;
    };
    MasterBusWidths masterBusStageWidths() const noexcept
    {
        MasterBusWidths w;
        w.bus      = masterBusWidth_;
        w.highPass = (int) hpFilters_.size();
        w.lowPass  = (int) lpFilters_.size();
        w.bandPass = (int) bpFilters_.size();
        w.songEq   = (int) (eqSongFilters_.size() / (size_t) kEqBands);
        w.limiter  = masterLimiter_.preparedChannels();
        return w;
    }
    /// Blocks whose filter + song-EQ stages were SKIPPED because the callback
    /// handed more channels than masterBusWidth_. Cumulative; reset by
    /// prepareMasterBus. The limiter counts its own (masterLimiterSkippedBlocks).
    unsigned long masterBusSkippedBlocks() const noexcept
    { return masterBusSkipped_.load(std::memory_order_relaxed); }
    unsigned long masterLimiterSkippedBlocks() const noexcept
    { return masterLimiter_.skippedBlocks(); }

    static constexpr float kLimiterMinCeilingDb = -24.0f;
    static constexpr float kLimiterMaxCeilingDb =   0.0f;
    static constexpr float kLimiterMinReleaseMs =   1.0f;
    static constexpr float kLimiterMaxReleaseMs = 1000.0f;

    /// Hi-pass / Lo-pass / Band-pass over the mixed playback output. freq <= 0 = bypass.
    /// The three setters run on the message thread and PRECOMPUTE the biquad
    /// coefficients there (see publishMasterFilterCoeffs). The audio thread
    /// then only copies 5 floats per section: juce::dsp::IIR::Coefficients
    /// factory calls heap-allocate, and doing them inside processMasterBus
    /// meant sweeping a filter knob allocated on the callback at UI tick rate.
    void setHighPass(float freqHz, float q) noexcept { std::lock_guard<std::recursive_mutex> publishLock(filterPublishMutex_); hpFreq_.store(freqHz, std::memory_order_relaxed); hpQ_.store(q, std::memory_order_relaxed); publishMasterFilterCoeffs(); }
    void setLowPass (float freqHz, float q) noexcept { std::lock_guard<std::recursive_mutex> publishLock(filterPublishMutex_); lpFreq_.store(freqHz, std::memory_order_relaxed); lpQ_.store(q, std::memory_order_relaxed); publishMasterFilterCoeffs(); }
    void setBandPass(float freqHz, float q) noexcept { std::lock_guard<std::recursive_mutex> publishLock(filterPublishMutex_); bpFreq_.store(freqHz, std::memory_order_relaxed); bpQ_.store(q, std::memory_order_relaxed); publishMasterFilterCoeffs(); }

    // --- Parametric EQ (ABI v29) -----------------------------------------
    /// Six typed bands over the same master bus the three fixed filters run on,
    /// and optionally over each seat's post-rig guitar signal.
    ///
    /// Same discipline as those filters, for the same reason: every setter runs
    /// on the message thread and PRECOMPUTES the RBJ coefficients there, so the
    /// audio thread only copies 5 floats per engaged section. Dragging a band
    /// handle is a supported gesture and produces setter traffic at UI tick rate.
    ///
    /// The ranges below are duplicated from
    /// DotCharter.Services/Audio/Eq/EqBand.cs (EqLimits) deliberately: the ABI is
    /// a boundary, and a boundary that trusts its caller is not one. Change
    /// either side and change both.
    static constexpr int   kEqBands       = DCA_EQ_BAND_COUNT;
    static constexpr float kEqMinFreqHz   = 20.0f;
    static constexpr float kEqMaxFreqHz   = 20000.0f;
    static constexpr float kEqMinQ        = 0.3f;
    static constexpr float kEqMaxQ        = 18.0f;
    static constexpr float kEqMaxGainDb   = 18.0f;
    static constexpr float kEqMaxTrimDb   = 12.0f;

    void setEqBand(int index, int type, float freqHz, float q, float gainDb, bool enabled) noexcept;
    void setEqEnabled(bool on) noexcept;
    /// < 0 = no solo. While a band is soloed it is the ONLY one applied, whatever
    /// the per-band enables say.
    void setEqSolo(int bandIndex) noexcept;
    void setEqRoute(int route) noexcept;
    void setEqTrim(float trimDb) noexcept;
    /// How many bands the engine is actually running right now — master power and
    /// solo already folded in. The ONE source of truth behind the "n/6" badge;
    /// deriving it a second time in managed code is how a display drifts from the
    /// DSP.
    int  eqActiveCount() const noexcept;
    /// Message thread. Windows + FFTs a snapshot of the pre-EQ ring and writes
    /// `binCount` dBFS magnitudes, log-spaced 20 Hz … 20 kHz. Returns the number
    /// written, or 0 when the request is out of range.
    int  readEqSpectrum(float* destDb, int binCount) noexcept;

    /// Enumerate devices using THIS engine's already-initialised AudioDeviceManager.
    /// Safe to call after start() because we reuse the open driver/types instead
    /// of constructing a parallel manager (which would crash exclusive-mode ASIO drivers).
    /// One body, `isInput` selects the direction — the two used to be a
    /// copy-paste pair differing only in that flag. The thin wrappers keep the
    /// call sites reading the way they always did.
    int  enumerateDevicesLive(bool isInput, dca_device_info* out, int capacity);
    int  enumerateOutputDevicesLive(dca_device_info* out, int capacity)
    { return enumerateDevicesLive(false, out, capacity); }
    int  enumerateInputDevicesLive (dca_device_info* out, int capacity)
    { return enumerateDevicesLive(true,  out, capacity); }

    /// Open the ASIO control panel for the currently active device (Windows only,
    /// only when the active driver is ASIO). Returns empty string on success.
    juce::String showAsioControlPanel(void* parentWindow);

    // Static device enumeration. Works without an engine instance.
    static int  enumerateDevices(bool isInput, dca_device_info* out, int capacity);
    static int  enumerateOutputDevices(dca_device_info* out, int capacity)
    { return enumerateDevices(false, out, capacity); }
    static int  enumerateInputDevices (dca_device_info* out, int capacity)
    { return enumerateDevices(true,  out, capacity); }

    // ABI v13: enumerate devices for a single host type without opening any
    // device. typeName==nullptr or empty -> use the default device type.
    static int  enumerateHostDevices(const char* typeName, bool isInput,
                                     dca_device_info* out, int capacity);
    // ABI v13: list every host (AudioIODeviceType) JUCE recognises. Each
    // entry is a null-terminated string written into typeNames[i*64].
    static int  enumerateHostTypes(char* typeNames, int capacityEntries);

    // --- VST3 input chain -------------------------------------------------
    /// Attach a plugin to a player's live-input processing chain. The plugin is
    /// prepared with the current device sample rate / block size before being
    /// inserted, so it is safe to call from any thread.
    /// Returns true on success. `player` defaults to 0 (the pre-v28 chain).
    bool attachInputPlugin(juce::AudioPluginInstance* plugin, int player = 0);
    /// Detach from EVERY player chain — a plugin instance is about to be
    /// destroyed and the caller does not have to remember where it went.
    void detachInputPlugin(juce::AudioPluginInstance* plugin);
    /// Rubber Band engine quality for THIS engine's stretchers: 0 = Finer
    /// (default), 1 = Faster. Flipped by the host when several stems are
    /// loaded. It was a process-global until 2026-09-10, so
    /// dca_engine_set_stretch_quality ignored its handle and re-voiced every
    /// other engine in the process. Read when a stretcher is prepared.
    void setStretchFaster(bool faster) noexcept
    { stretchQuality_.store(faster ? 1 : 0, std::memory_order_release); }
    const std::atomic<int>& stretchQualityFlag() const noexcept { return stretchQuality_; }

    // --- Diagnostics sink --------------------------------------------------
    /// Where AudioEngine's own diagnostics go. The engine cannot reach the ABI
    /// translation unit's `emitLog`, so it wrote the two messages it produces
    /// straight to `stderr` — which the managed side never sees, so the single
    /// most useful message this file has ("here is every device name the driver
    /// actually offers", printed exactly when a device failed to open) reached
    /// nobody. dotcharter_audio.cpp installs a trampoline into `emitLog` at
    /// static-init time; unset, `engineLog` is a no-op, so the native test
    /// executables are unaffected.
    ///
    /// `level` mirrors `dca_log_level` (0 trace … 4 error). The ABI TU
    /// static_asserts that the two agree.
    using LogSink = void (*)(int level, const char* message);
    static void setLogSink(LogSink sink) noexcept;
    static void engineLog(int level, const juce::String& message) noexcept;
    static constexpr int kLogWarn  = 3;
    static constexpr int kLogError = 4;

    double currentSampleRate() const noexcept { return deviceSr_; }
    int    currentBlockSize() const noexcept  { return deviceBlock_; }

    /// The open device's numbers, published on the device thread and safe to
    /// read from ANY thread — unlike `getCurrentAudioDevice()`, whose raw
    /// pointer `setAudioDeviceSetup` deletes out from under a poller. All four
    /// read 0/false when no device is open, which is the same answer the old
    /// null check produced.
    struct DeviceSnapshot
    {
        int sampleRate, blockFrames, outputLatency, outputChannels, inputChannels;
        bool open;
    };
    DeviceSnapshot deviceSnapshot() const noexcept
    {
        return { snapSampleRate_.load(std::memory_order_acquire),
                 snapBlockFrames_.load(std::memory_order_relaxed),
                 snapOutputLatency_.load(std::memory_order_relaxed),
                 snapOutputChannels_.load(std::memory_order_relaxed),
                 snapInputChannels_.load(std::memory_order_relaxed),
                 snapDeviceOpen_.load(std::memory_order_relaxed) };
    }
    /// Device thread only (audioDeviceAboutToStart / audioDeviceStopped).
    void publishDeviceSnapshot(juce::AudioIODevice* device) noexcept
    {
        if (device == nullptr) { clearDeviceSnapshot(); return; }
        snapBlockFrames_  .store(device->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
        snapOutputLatency_.store(device->getOutputLatencyInSamples(),   std::memory_order_relaxed);
        snapOutputChannels_.store(device->getOutputChannelNames().size(), std::memory_order_relaxed);
        snapInputChannels_ .store(device->getInputChannelNames().size(),  std::memory_order_relaxed);
        snapDeviceOpen_   .store(true,                                  std::memory_order_relaxed);
        // Released last: a reader that sees a non-zero rate sees the rest.
        snapSampleRate_   .store((int) device->getCurrentSampleRate(),  std::memory_order_release);
    }
    void clearDeviceSnapshot() noexcept
    {
        snapDeviceOpen_   .store(false, std::memory_order_relaxed);
        snapBlockFrames_  .store(0,     std::memory_order_relaxed);
        snapOutputLatency_.store(0,     std::memory_order_relaxed);
        snapOutputChannels_.store(0,    std::memory_order_relaxed);
        snapInputChannels_ .store(0,    std::memory_order_relaxed);
        snapSampleRate_   .store(0,     std::memory_order_release);
    }

    // --- Internal MIDI instrument bus -------------------------------------
    /// Attach / detach the single internal instrument slot. The slot owns
    /// no plugin lifetime — that stays with the caller (the C ABI Plugin
    /// wrapper). Lock-free swap; the audio thread observes the new pointer
    /// on the next callback.
    bool attachInstrumentPlugin(juce::AudioPluginInstance* plugin);
    void detachInstrumentPlugin(juce::AudioPluginInstance* plugin);
    bool hasInstrument() const noexcept { return instrumentPlugin_.load(std::memory_order_acquire) != nullptr; }

    // --- Out-of-process VST bridge (ABI v19) -------------------------------
    /// Arm / disarm the shared-memory bridge to the sandboxed VST host
    /// process. While armed, the audio callback runs one bounded round trip
    /// per block: live input goes out for FX-chain processing, queued MIDI
    /// shorts go out for the remote instrument, and the child's processed
    /// chain / rendered instrument audio come back. Safe to call from any
    /// non-audio thread; disable waits out any in-flight block before
    /// closing the kernel objects.
    bool setRemoteBridge(const char* shmName, const char* requestEventName,
                         const char* responseEventName, bool enable);
    void getRemoteBridgeStats(uint32_t& processed, uint32_t& timeouts,
                              uint32_t& heartbeat) const noexcept
    {
        processed = remoteBridge_.processedBlocks();
        timeouts  = remoteBridge_.timeoutBlocks();
        heartbeat = remoteBridge_.lastHeartbeat();
    }
    void setInstrumentGain(float g) noexcept { instrumentGain_.store(g, std::memory_order_relaxed); }
    /// Queue a 3-byte MIDI short message for the instrument. Lock-free; drops
    /// silently when no instrument is attached or the FIFO is saturated.
    void sendMidiShort(uint8_t status, uint8_t data1, uint8_t data2) noexcept;
    /// ABI v34. The same queue, with an ABSOLUTE output-timeline frame in the
    /// domain dca_engine_get_render_clock publishes. The event is PARKED until
    /// the block that contains that frame and then placed at its exact sample
    /// offset inside it — the same mechanism ABI v26's scheduled clicks use,
    /// and for the same reason: output latency cancels out of the frame
    /// arithmetic, so the note leaves the engine in the very samples that carry
    /// its audio. A negative frame means "as soon as possible" and is exactly
    /// what sendMidiShort produces.
    void sendMidiAt(uint32_t packed, int64_t renderFrame) noexcept;
    /// Poll-only counters for the above. `late` counts events whose frame had
    /// already gone past when their block ran (placed at sample 0 instead);
    /// `dropped` counts events the audio thread could not park because the
    /// pending set was full.
    void getMidiTimingStats(int64_t& scheduled, int64_t& late, int64_t& dropped) const noexcept
    {
        scheduled = midiScheduledEvents_.load(std::memory_order_relaxed);
        late      = midiLateEvents_     .load(std::memory_order_relaxed);
        dropped   = midiDroppedEvents_  .load(std::memory_order_relaxed);
    }
    void resetMidiTimingStats() noexcept
    {
        midiScheduledEvents_.store(0, std::memory_order_relaxed);
        midiLateEvents_     .store(0, std::memory_order_relaxed);
        midiDroppedEvents_  .store(0, std::memory_order_relaxed);
    }

    // --- Streaming mirror -------------------------------------------------
    /// Open / re-open / close the parallel mirror output. Empty name closes.
    /// Returns "" on success, the JUCE error string on failure. Must be called
    /// from the JUCE message thread (the C ABI dispatches via JuceHost).
    juce::String setMirrorDevice(const juce::String& deviceName);

    // --- Click bus (metronome + clap) -------------------------------------
    /// Trigger a synthesised click on the audio thread. Type: 0 = beat tick,
    /// 1 = measure tick (higher pitch), 2 = clap (noise burst). Lock-free,
    /// safe to call from UI or worker threads. The voice starts at the head of
    /// the next audio block, so the trigger jitters by up to one block plus
    /// whatever quantum the caller's clock has. Use scheduleClick when the
    /// click has to land ON a known point of the output timeline.
    void triggerClick(int type) noexcept;

    /// ABI v26. Schedule a click to start at an ABSOLUTE frame on the engine's
    /// output render clock (see getRenderClock). The audio thread starts the
    /// voice at that exact sample OFFSET inside whichever block contains it —
    /// no block-boundary jitter. Lock-free, safe from any thread.
    ///
    /// A frame that has already been rendered fires at the head of the current
    /// block (never worse than triggerClick) unless it is stale by more than
    /// ~250 ms; a frame more than ~5 s in the future is dropped (that can only
    /// be a leftover from a device restart, which rewinds the render clock).
    void scheduleClick(int type, int64_t renderFrame) noexcept;

    /// Drop every scheduled-but-not-yet-sounded click. Ordered against
    /// scheduleClick (both travel through the same FIFO), so a clear issued
    /// after a schedule always cancels it. Call on seek / pause / stop / speed
    /// change — the render clock keeps running while the transport does not,
    /// so a stale queue would otherwise fire into silence.
    void clearScheduledClicks() noexcept;

    /// ABI v26. Atomically sample {frames rendered by the device callback so
    /// far, host high-resolution ticks at that same block boundary}. The frame
    /// count is the index of the NEXT sample the device will render; the tick
    /// source is juce::Time::getHighResolutionTicks (QPC on Windows, i.e. the
    /// same timebase as .NET's Stopwatch), matching getInputCaptureSync.
    ///
    /// Published IMMEDIATELY after the transport pull inside the callback, so a
    /// caller that reads this and a TransportSource's audible position
    /// back-to-back gets a pair describing the SAME output sample. That pair is
    /// what turns a song time into a render frame.
    ///
    /// False until the device has rendered its first block.
    bool getRenderClock(int64_t& outFrames, int64_t& outTicks) const noexcept;

    /// Master gain applied to all click voices. 1.0 = unity. Default 1.0.
    void setClickGain(float g) noexcept { clickGain_.store(g, std::memory_order_relaxed); }

    // --- Offline waveform peak generation ---------------------------------
    /// Decode `file` and emit one peak amplitude (0..1) per `blockMs` window
    /// into `out` (up to `capacity` floats). Returns the number of peaks
    /// that *would* have been written (may exceed capacity — call once with
    /// capacity=0 to size, then again to fill). Static: spins up its own
    /// AudioFormatManager, no engine instance required.
    static int generateWaveform(const juce::File& file,
                                double blockMs,
                                float* out,
                                int capacity);

    /// Same contract as generateWaveform, but decodes an in-memory audio
    /// blob. The data is not copied and only read during the call.
    static int generateWaveformFromMemory(const void* data,
                                          size_t dataSize,
                                          double blockMs,
                                          float* out,
                                          int capacity);

#if DCA_RTCHECK
    // Internal gate seam: supply JUCE's legal sparse channel pointers directly.
    void renderForTest(const float* const* in, int inputs, float* const* out, int outputs, int frames)
    { callback_.audioDeviceIOCallbackWithContext(in, inputs, out, outputs, frames, {}); }
    void masterForTest(float* const* out, int channels, int frames)
    {
        if (filterCoefficients_.consume()) { filterDirty_ = true; eqInputDirty_ = true; }
        processMasterBus(out, channels, frames);
    }
    /// What the LAST device callback was handed on its output side: the
    /// pointer count and a bit per null pointer (#31 — settles whether JUCE
    /// packs the active outputs or hands one pointer per physical channel).
    int      lastCallbackOutputChannelsForTest() const noexcept { return rtLastOutChannels_; }
    uint32_t lastCallbackOutputNullMaskForTest() const noexcept { return rtLastOutNullMask_; }
#endif

private:
    /// Width of the live-input processing bus. The DSP rig is prepared for
    /// exactly this many channels (prepareMasterBus), input VST plugins are
    /// forced to this layout (attachInputPlugin), the sandbox bridge carries
    /// this many (dcvb::kMaxChannels) and the callback fills exactly these
    /// channels of chainBuffer_ — which is itself sized for the DEVICE's
    /// output count and can therefore be WIDER. Everything that walks the
    /// chain buffer must clamp to this, not to getNumChannels().
    static constexpr int kChainChannels = 2;

    /// Scale `dst` by a gain over one block, LINEARLY RAMPED from `from` to
    /// `to` when they differ.
    ///
    /// Every user-facing level here — master volume, the guitar monitor, the
    /// input trim — is published from the UI as a bare atomic and was applied
    /// as ONE constant for the whole block. That steps the waveform at every
    /// block boundary while a slider is moving, and the step is largest
    /// exactly when the drag is fastest: the classic zipper. A drag from
    /// unity to silence over half a second at 256 frames / 48 kHz is ~94
    /// discontinuities, each up to the full sample value.
    ///
    /// When the gain has NOT moved this falls through to the same vectorised
    /// multiply it always used (and to nothing at all at unity), so a user
    /// who is not touching a slider pays exactly what they paid before.
    static void rampOrScale(float* dst, int numSamples, float from, float to) noexcept
    {
        if (dst == nullptr || numSamples <= 0) return;
        // Defence in depth against a non-finite gain that got past the ABI's
        // dcabi::finite guards (an internal caller, a future setter). A gain
        // that STAYS NaN is permanent silence, not a click: the block is
        // multiplied by NaN and the output scrub zeroes the bus, every block,
        // for the rest of the session, with the meters reading the same zeros.
        //
        // HOLD THE LAST GOOD VALUE — `to` first, from `from`, then `from` from
        // the repaired `to`. Unity is only the fallback when neither end is a
        // number at all, and it is deliberately not silence: this is the master
        // path, where the failure being defended against IS silence.
        if (!std::isfinite(to))   to   = std::isfinite(from) ? from : 1.0f;
        if (!std::isfinite(from)) from = to;
        if (from == to)
        {
            if (to != 1.0f) juce::FloatVectorOperations::multiply(dst, to, numSamples);
            return;
        }
        const float step = (to - from) / (float) numSamples;
        float g = from;
        for (int i = 0; i < numSamples; ++i) { dst[i] *= g; g += step; }
    }

    /// The same ramp, MIXING src into dst (the monitor sum).
    static void rampOrScaleAdd(float* dst, const float* src, int numSamples,
                               float from, float to) noexcept
    {
        if (dst == nullptr || src == nullptr || numSamples <= 0) return;
        // Same hold-the-last-good rule as rampOrScale above. The fallback is
        // 0.0f and not 1.0f: this helper MIXES a source in, so "no number at
        // all" must mean "do not add", never "add at full scale".
        if (!std::isfinite(to))   to   = std::isfinite(from) ? from : 0.0f;
        if (!std::isfinite(from)) from = to;
        if (from == to)
        {
            if (to != 0.0f)
                juce::FloatVectorOperations::addWithMultiply(dst, src, to, numSamples);
            return;
        }
        const float step = (to - from) / (float) numSamples;
        float g = from;
        for (int i = 0; i < numSamples; ++i) { dst[i] += src[i] * g; g += step; }
    }

    // Custom callback: drives the AudioSourcePlayer for playback output and
    // direct-monitors the input buffers into the output, while tracking peak.
    class EngineCallback : public juce::AudioIODeviceCallback
    {
    public:
        explicit EngineCallback(AudioEngine& owner) : owner_(owner) {}

        // THE BACKSTOP (W4-N1). Everything this engine runs on the callback is
        // its own code and cannot throw — except the third-party plugins, of
        // which there are three call sites (the input chain, the instrument
        // bus, and the bridge's response read). Each of those catches for
        // itself; this catch is the one that makes "nothing escapes the driver
        // callback" a property of the CLASS rather than of a list of sites
        // somebody has to keep complete. An exception here unwinds through
        // JUCE's ASIO/WASAPI frame into the driver's, which is UB and in
        // practice std::terminate — a process kill, not a dropped block.
        //
        // It must stay a bare `catch (...)`: logging, allocating or rethrowing
        // on the audio thread are each a worse fault than the silence below.
        //
        // …AND IT HAS TO MAKE THAT SILENCE ITSELF (W5-E8). This comment claimed
        // "the block of silence this produces" while the body was EMPTY, and
        // renderBlock does not clear `out` — the first thing it does is hand
        // the buffer to juce::AudioSourcePlayer, which is what fills or zeroes
        // it. So a throw from inside player_ left the DRIVER'S buffer, which on
        // several WASAPI/ASIO backends is the PREVIOUS block repeated at full
        // level, and a throw from anywhere after left the song without the
        // monitor. Neither is silence. Clearing per channel is RT-safe
        // (FloatVectorOperations, no allocation, no lock) and is the
        // conservative answer for an engine that has just lost its invariants
        // mid-block: one silent block beats one repeated one.
        void audioDeviceIOCallbackWithContext(const float* const* in,
                                              int numInputChannels,
                                              float* const* out,
                                              int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext& ctx) override
        {
#if DCA_RTCHECK
            owner_.rtLastOutChannels_ = numOutputChannels;
            owner_.rtLastOutNullMask_ = 0;
            for (int c = 0; c < numOutputChannels && c < 32; ++c)
                if (out == nullptr || out[c] == nullptr) owner_.rtLastOutNullMask_ |= (1u << c);
#endif
            try
            {
                renderBlock(in, numInputChannels, out, numOutputChannels, numSamples, ctx);
            }
            catch (...)
            {
                // Audio thread must never propagate — see above. Make the
                // block genuinely silent rather than leaving whatever the
                // driver handed us.
                if (out != nullptr)
                    for (int c = 0; c < numOutputChannels; ++c)
                        if (out[c] != nullptr)
                            juce::FloatVectorOperations::clear(out[c], numSamples);
            }
        }

    private:
        void renderBlock(const float* const* in,
                         int numInputChannels,
                         float* const* out,
                         int numOutputChannels,
                         int numSamples,
                         const juce::AudioIODeviceCallbackContext& ctx)
        {
            // Flush-to-zero / denormals-are-zero: filter tails, stretcher
            // output and click envelope decay can all generate subnormals.
            // On x86 without hardware FTZ the per-sample cost of subnormal
            // arithmetic is orders of magnitude worse than normals and
            // shows up as periodic audio-thread spikes.
            const juce::ScopedNoDenormals noDenormals;

            const auto callbackStart = juce::Time::getHighResolutionTicks();

            // Out-of-process bridge: reset the per-callback round-trip
            // bookkeeping (audio-thread-only fields).
            owner_.bridgeRanThisBlock_      = false;
            owner_.bridgeInstrumentActive_  = false;

            // The output-timeline index of out[..][0]. Scheduled clicks are
            // placed against exactly this clock.
            const int64_t blockStartFrame = owner_.renderFrames_;
            // The sandbox bridge places its MIDI against the same clock (bridge
            // v3), from inside the seat loop where blockStartFrame is not in
            // scope and renderFrames_ has already moved on.
            owner_.bridgeBlockStartFrame_ = blockStartFrame;

            // Playback output first.
            owner_.player_.audioDeviceIOCallbackWithContext(in, numInputChannels,
                                                            out, numOutputChannels,
                                                            numSamples, ctx);

            // Render clock (ABI v26). Publish IMMEDIATELY after the transport
            // pull — not at the end of the callback. Both this counter and a
            // TransportSource's audible position now describe the same output
            // sample (the first of the NEXT block), so a managed reader that
            // samples them back-to-back cannot observe a torn pair: any block
            // that lands between its two reads moves this counter, which the
            // reader re-checks. That {frame, song time} pair is the anchor that
            // maps a chart time onto the output timeline.
            owner_.publishRenderClock(blockStartFrame + numSamples);

            // Chart source (ABI v38): the song clock for THIS block, read right
            // after the pull that rendered it. Off-path unless a seat uses it.
            if (owner_.chartActive_.load(std::memory_order_relaxed) > 0)
                owner_.sampleSongClock();

            // One immutable coefficient generation for the entire callback.
            if (owner_.filterCoefficients_.consume())
            {
                owner_.filterDirty_ = true;
                owner_.eqInputDirty_ = true;
            }
            // Master bus: filters then master gain over playback output.
            owner_.processMasterBus(out, numOutputChannels, numSamples);

            // Direct monitor: mix input (possibly through the VST chain) into output
            // and track peak.
            const float mon = owner_.monitorGain_.load(std::memory_order_relaxed);
            const float ing = owner_.inputGain_  .load(std::memory_order_relaxed);
            // Re-amp (ABI v37): a seat with an injected take runs its rig even
            // on a device with no input at all. Nobody injecting = the pre-v37
            // condition exactly.
            const bool anyInjection = owner_.injectionActive_.load(std::memory_order_relaxed) > 0;
            // Chart source / routed instrument (ABI v38): a seat with a
            // generator runs its rig even on a device with no input. Nobody
            // using either = the pre-v38 condition exactly.
            int instrumentSeat = owner_.instrumentRoute_.load(std::memory_order_relaxed);
            if (instrumentSeat >= 0 && owner_.instrumentPlugin_.load(std::memory_order_acquire) == nullptr)
                instrumentSeat = -1;
            const bool anyGenerator = owner_.chartActive_.load(std::memory_order_relaxed) > 0
                                   || instrumentSeat >= 0;
            if ((numInputChannels > 0 && in != nullptr) || anyInjection || anyGenerator)
            {
                // ---- Per-player rigs (ABI v28) -------------------------------
                // ONE seat (the pre-v28 path) or N seats, each reading its own
                // hardware input channel out of the SAME open device. Two
                // guitars into a 2-in interface is the shipping case; nothing
                // here ever opens a second device.
                const int players = juce::jlimit(1, kMaxPlayers,
                                                 owner_.playerCount_.load(std::memory_order_relaxed));

                // The routed instrument renders BEFORE the seats so its seat can
                // take it into the rig; a route naming a seat that is not live
                // this block stays on the master (renderInstrumentBus below).
                if (instrumentSeat >= players) instrumentSeat = -1;
                if (instrumentSeat >= 0)
                {
                    owner_.instrumentWorkChannels_    = owner_.renderInstrumentWork(numSamples, blockStartFrame);
                    owner_.instrumentRenderedForSeat_ = true;
                }

                // ABI v29. The guitar banks need their OWN dirty flag: the song
                // bus consumed filterDirty_ in processMasterBus above, which runs
                // earlier in this same callback, so a shared flag would leave the
                // guitar EQ permanently one publish behind. Consumed ONCE here
                // rather than inside the loop — per-seat, the first seat would
                // clear it and every later seat would never retune.
                const bool eqRetune = std::exchange(owner_.eqInputDirty_, false);
                // Inactive seats can miss publications. Re-engaging them must
                // copy the current snapshot, even when it arrived blocks ago.
                for (int p = players; p < kMaxPlayers; ++p)
                {
                    owner_.eqInputEngaged_[(size_t) p].fill(false);
                    owner_.eqInputTrimCurrent_[(size_t) p] = 1.f;
                }


                for (int p = 0; p < players; ++p)
                {
                    // Which samples this seat hears: the raw device input, or
                    // this seat's OWN drop-tuned copy of it.
                    const float* const* pIn  = in;
                    int                 pNch = numInputChannels;

                    // ---- Drop-tune pedal, PER SEAT (CSVD, ABI v31) -----------
                    // Shift this seat's input FIRST, so its capture ring (note
                    // detection), its VST chain (amp sims) and the monitor all
                    // hear the dropped signal — exactly like a physical pitch
                    // pedal at the front of that player's rig.
                    //
                    // Until v31 there was ONE shifter and it ran on the raw
                    // device input ABOVE this loop, so a couch with two guitars
                    // in different tunings could not use it at all: dropping one
                    // dropped BOTH, and the seat that did not ask for it was
                    // detuned with nothing on screen saying so.
                    //
                    // The CSVD engine itself is unchanged. It replaced a Rubber
                    // Band LiveShifter (>=50 ms — a phase-vocoder floor, not a
                    // tuning problem) and a dual-head OLA whose two read heads
                    // were permanently live 20 ms apart, which is a continuously
                    // swept comb: that WAS the warble. It also replaced a
                    // "transient duck" that punched raw UNSHIFTED audio through
                    // at every pick — the attack arrived at the wrong pitch,
                    // then the shifted sustain behind it. See dcdsp_droptune.h.
                    {
                        auto& shifter = owner_.dropTune_[(size_t) p];
                        auto& scratch = owner_.dropScratch_[(size_t) p];
                        auto& ptrs    = owner_.dropPtrs_[(size_t) p];

                        dcdsp::DropTuneParams dp;
                        dp.semitones = owner_.inputPitchShiftSemis_[(size_t) p]
                                           .load(std::memory_order_relaxed);
                        dp.maxLagMs  = owner_.dropTuneMaxLagMs_[(size_t) p]
                                           .load(std::memory_order_relaxed);
                        shifter.setParams(dp);

                        // WHICH channels to shift. The shifter is hard-capped at
                        // 2, and blindly taking ch0/ch1 is wrong on a bigger
                        // interface: with the guitar on, say, input 6, engaging
                        // the pedal used to (a) shift inputs 1-2 — not the
                        // guitar — and (b) hand the capture ring a 2-channel
                        // buffer, so its channel index clamped from 5 down to 1
                        // and note detection silently started listening to a
                        // different physical input. So: shift exactly the signal
                        // THIS SEAT's capture ring consumes, as mono. The chain
                        // duplicates mono across L/R anyway (see chainChannels
                        // below), and a guitar input is mono regardless.
                        //
                        // The one exception is the pre-v28 case — ONE live seat
                        // on a 1-2 in device — where channels 0/1 are taken as
                        // they always were, so a genuine STEREO pickup stays
                        // stereo. That is the same split the chain fill below
                        // makes, and it is what keeps a solo player's signal
                        // path byte-for-byte what it was.
                        const int captureCh =
                            owner_.inputCaptureChannel_[(size_t) p].load(std::memory_order_relaxed);
                        // Never allocate here (audit #5): the scratch is
                        // pre-sized with 2x headroom in audioDeviceAboutToStart.
                        // If a variable-block driver still hands us more, SKIP
                        // the shifter for this block (dry input passes through)
                        // instead of resizing on the RT thread — same discipline
                        // as renderRemoteInstrument.
                        const bool dropScratchOk =
                               (int) scratch[0].size() >= numSamples
                            && (int) scratch[1].size() >= numSamples;
                        if (dropScratchOk && numInputChannels > 0 && in != nullptr)
                        {
                            int shiftCh;
                            if (players == 1 && numInputChannels <= 2)
                            {
                                // Ordinary 1-2 in interface, one player: unchanged.
                                shiftCh = juce::jlimit(1, 2, numInputChannels);
                                for (int c = 0; c < shiftCh; ++c)
                                {
                                    if (in[c] != nullptr)
                                        std::memcpy(scratch[(size_t) c].data(), in[c],
                                                    sizeof(float) * (size_t) numSamples);
                                    else
                                        std::fill_n(scratch[(size_t) c].data(), numSamples, 0.0f);
                                    ptrs[(size_t) c] = scratch[(size_t) c].data();
                                }
                            }
                            else
                            {
                                shiftCh = 1;
                                auto& mono = scratch[0];
                                // in[captureCh] can be NULL: JUCE hands a null
                                // pointer for a channel the driver did not
                                // actually provide, which is why every sibling
                                // read in this callback null-checks its source.
                                // This one did not, and would have memcpy'd from
                                // nullptr.
                                if (captureCh >= 0 && captureCh < numInputChannels
                                    && in[captureCh] != nullptr)
                                {
                                    std::memcpy(mono.data(), in[captureCh],
                                                sizeof(float) * (size_t) numSamples);
                                }
                                else
                                {
                                    // captureCh < 0 means "mix every input to mono" —
                                    // mirror that here so the shifted signal is the
                                    // same one detection expects.
                                    // Same null rule as above: a channel the
                                    // driver did not provide is a null pointer,
                                    // not silence, and this loop dereferenced
                                    // every one of them.
                                    const float g = 1.0f / (float) numInputChannels;
                                    for (int i = 0; i < numSamples; ++i)
                                    {
                                        float s = 0.0f;
                                        for (int c = 0; c < numInputChannels; ++c)
                                            if (in[c] != nullptr) s += in[c][i];
                                        mono[(size_t) i] = s * g;
                                    }
                                }
                                ptrs[0] = mono.data();
                            }

                            // Always call it: while bypassed it keeps its ring
                            // primed with dry audio, so ENGAGING mid-note splices
                            // against real signal instead of against silence.
                            shifter.process(ptrs.data(), shiftCh, numSamples);

                            if (! shifter.isBypassed())
                            {
                                pIn  = ptrs.data();
                                pNch = shiftCh;
                            }
                        }
                    }

                    // ---- Re-amp (ABI v37) -----------------------------------
                    // A loaded take REPLACES this seat's input from here on: the
                    // capture ring (tuner, detection, DI recorder), the rig and
                    // the meter all hear the take, and the live guitar is muted
                    // while it is loaded. After the drop tune on purpose — a DI
                    // recorded from the capture ring is already shifted.
                    if (anyInjection)
                    {
                        const float* const* injected = nullptr;
                        if (owner_.renderInputInjection(p, numSamples, injected))
                        {
                            pIn  = injected;
                            pNch = 1;
                        }
                    }
                    // ABI v38: a seat with a chart source (or a fading one) or
                    // the routed instrument runs its rig with no input at all.
                    const bool seatGenerator = owner_.chart_[(size_t) p].enabled.load(std::memory_order_relaxed)
                                            || owner_.chart_[(size_t) p].runState != 0
                                            || p == instrumentSeat;
                    const bool haveInput = pIn != nullptr && pNch > 0;
                    // Only reachable on a device with no input, for a seat with
                    // no take and no generator: nothing to capture, run or monitor.
                    if (! haveInput && ! seatGenerator) continue;

                    // Capture the (possibly shifted) pre-gain, pre-chain input
                    // into this seat's SPSC ring — this is what FFT / tuner /
                    // metering and live detection in the managed layer see.
                    // The generators are mixed in BELOW this line, which is the
                    // whole of "detection never hears the chart source".
                    if (haveInput) owner_.writeInputCapture(p, pIn, pNch, numSamples);

                    // The chain processes a stereo buffer regardless of physical
                    // input count: if we only have one input channel (mono
                    // guitar) we duplicate it across L/R so plugins that expect
                    // stereo (most amp sims do) get a sane signal and the monitor
                    // is centred on the output.
                    //
                    // Never setSize here (audit #5): chainBuffer_ is pre-sized
                    // with 2x headroom in prepareMasterBus. A still-bigger block
                    // skips the chain + monitor for this one block instead of
                    // heap-allocating on the RT thread (capture above already ran,
                    // so note detection keeps flowing).
                    const int chainChannels = kChainChannels;
                    auto& chainBuf = owner_.chainBuffer_[(size_t) p];
                    const bool chainOk = chainBuf.getNumChannels() >= chainChannels
                                      && chainBuf.getNumSamples() >= numSamples;
                    if (! chainOk) continue;

                    // WHICH samples become L/R.
                    //   players == 1 AND <= 2 physical inputs : unchanged from
                    //                  v27 — channels 0/1 of the effective
                    //                  input, i.e. a genuine stereo pickup stays
                    //                  stereo. THE CHANNEL COUNT IS PART OF THE
                    //                  CONDITION, and it was not until
                    //                  2026-09-10: on a 4-in interface a solo
                    //                  player's rig was filled from channels
                    //                  0/1 whatever `inputCaptureChannel_[0]`
                    //                  said, while writeInputCapture used the
                    //                  requested channel — so the amp and the
                    //                  note detection listened to DIFFERENT
                    //                  guitars, and engaging the drop tuner
                    //                  moved the rig onto the capture channel
                    //                  mid-song (the shifter publishes a mono
                    //                  copy of exactly that channel). This is
                    //                  the same split the shifter above makes,
                    //                  which is what makes the two agree.
                    //   players  > 1 : each seat is MONO from its OWN channel,
                    //                  duplicated L/R. It has to be: with two
                    //                  guitars on inputs 0 and 1, the v27 stereo
                    //                  fill would put BOTH players into seat 0's
                    //                  amp, which is exactly the bug this feature
                    //                  exists to remove.
                    //
                    // The input trim is applied as a RAMP after the fill, not
                    // folded into it: it is a user slider, and one constant per
                    // block steps the signal into the amp sim at every block
                    // boundary while the knob is moving (see rampOrScale). One
                    // extra pass over 2 x numSamples floats; the same ramp on
                    // both channels, so a stereo pickup stays coherent.
                    const float ingPrev = owner_.inputGainCurrent_[(size_t) p];
                    if (! haveInput)
                    {
                        // A generator-only seat (no input device): the rig
                        // starts from silence.
                        for (int ch = 0; ch < chainChannels; ++ch)
                            juce::FloatVectorOperations::clear(chainBuf.getWritePointer(ch), numSamples);
                    }
                    else if (players == 1 && numInputChannels <= 2)
                    {
                        for (int ch = 0; ch < chainChannels; ++ch)
                        {
                            const int srcCh = (pNch == 1) ? 0
                                            : (ch < pNch ? ch : pNch - 1);
                            const float* src = pIn[srcCh];
                            float*       dst = chainBuf.getWritePointer(ch);
                            if (src != nullptr)
                            {
                                std::memcpy(dst, src, sizeof(float) * (size_t) numSamples);
                                rampOrScale(dst, numSamples, ingPrev, ing);
                            }
                            else
                            {
                                juce::FloatVectorOperations::clear(dst, numSamples);
                            }
                        }
                    }
                    else
                    {
                        // Resolve the seat's channel with the SAME rule the
                        // capture ring uses, so a player's rig and a player's
                        // judging can never listen to different guitars.
                        const int want = owner_.inputCaptureChannel_[(size_t) p]
                                             .load(std::memory_order_relaxed);
                        float* dstL = chainBuf.getWritePointer(0);
                        if (pNch <= 0)
                        {
                            juce::FloatVectorOperations::clear(dstL, numSamples);
                        }
                        else if (want < 0)
                        {
                            const float g = 1.0f / (float) pNch;
                            for (int i = 0; i < numSamples; ++i)
                            {
                                float s = 0.0f;
                                for (int c = 0; c < pNch; ++c)
                                    if (pIn[c] != nullptr) s += pIn[c][i];
                                dstL[i] = s * g;
                            }
                            rampOrScale(dstL, numSamples, ingPrev, ing);
                        }
                        else
                        {
                            const float* src = pIn[juce::jmin(want, pNch - 1)];
                            if (src != nullptr)
                            {
                                std::memcpy(dstL, src, sizeof(float) * (size_t) numSamples);
                                rampOrScale(dstL, numSamples, ingPrev, ing);
                            }
                            else
                            {
                                juce::FloatVectorOperations::clear(dstL, numSamples);
                            }
                        }
                        for (int ch = 1; ch < chainChannels; ++ch)
                            std::memcpy(chainBuf.getWritePointer(ch), dstL,
                                        sizeof(float) * (size_t) numSamples);
                    }
                    owner_.inputGainCurrent_[(size_t) p] = ing;

                    // ABI v38: the chart source and the routed instrument enter
                    // the rig HERE — after the capture ring and the input trim,
                    // before the in-house rig, the VST chain and the sandbox.
                    if (seatGenerator)
                        owner_.mixSeatGenerators(p, chainBuf, numSamples, p == instrumentSeat);

                    // In-house DSP tone rig (0.16 Phase 0): the reorderable
                    // gate/drive/amp/cab/mod chain, run over the gained input
                    // BEFORE any user VST input chain (external plugins are the
                    // escape hatch that comes after our own rig). Per player
                    // since v28.
                    //
                    // IDLE SLEEP (dsp/IdleSleep.h): the live path may skip the
                    // rig while it provably outputs nothing audible. Everything
                    // that can change what the rig or the stages after it do
                    // goes into the context, and a change wakes it.
                    const float pmonNow = mon * owner_.playerMonitorGain_[(size_t) p]
                                                    .load(std::memory_order_relaxed);

                    // ABI v47: an open external process insert REPLACES the
                    // seat's rig, its input VST3 chain and the sandbox: the dry
                    // chain buffer goes to the other process and its processed
                    // block comes back in place (or the dry input, on an
                    // underrun). Nothing published: one atomic load, then the
                    // rig path below exactly as before.
                    auto& extProc = owner_.extProc_[(size_t) p];
                    const bool extEngaged = extProc.beginBlock();
                    bool inputPluginsRan = false;
                    if (extEngaged)
                    {
                        // The mono the other side hears follows the rule the
                        // stereo fill above used: a solo seat on a <= 2-input
                        // device has its two channels in L/R, so take the
                        // capture channel (or their average); every other fill
                        // is mono duplicated, so L is the input.
                        int monoCh = 0;
                        if (haveInput && players == 1 && numInputChannels <= 2 && pNch > 1)
                        {
                            const int want = owner_.inputCaptureChannel_[(size_t) p]
                                                 .load(std::memory_order_relaxed);
                            monoCh = (want == 0 || want == 1) ? want : -1;
                        }
                        float* xp[kChainChannels];
                        for (int ch = 0; ch < chainChannels; ++ch)
                            xp[ch] = chainBuf.getWritePointer(ch);
                        extProc.process(xp, chainChannels, numSamples, monoCh);
                    }
                    else
                    {
                    {
                        dcrig::RigIdleContext idleCtx;
                        idleCtx.chainGeneration      = owner_.dspRigs_[(size_t) p].rtEditGeneration();
                        idleCtx.inputChainGeneration = owner_.inputChainGeneration_.load(std::memory_order_acquire);
                        idleCtx.inputGain            = ing;
                        idleCtx.monitorGain          = pmonNow;
                        idleCtx.eqTrim               = owner_.eqTrimLin_.load(std::memory_order_relaxed);
                        idleCtx.eqRoute              = owner_.eqRoute_.load(std::memory_order_relaxed);
                        idleCtx.captureChannel       = owner_.inputCaptureChannel_[(size_t) p]
                                                           .load(std::memory_order_relaxed);
                        idleCtx.players              = players;
                        idleCtx.bridgeEnabled        = (p == 0 && owner_.remoteBridge_.isEnabled()) ? 1 : 0;
                        owner_.processDspRigLive(p, chainBuf, numSamples, out, numOutputChannels,
                                                 idleCtx, eqRetune);
                    }

                    // Run this seat's input plugin chain (under a short lock —
                    // plugins are not hot-swapped per block in normal use).
                    inputPluginsRan = owner_.processInputChain(p, chainBuf, numSamples);

                    // Out-of-process chain: round-trip the (gained) input through
                    // the sandbox child. On success chainBuf holds the child's
                    // processed output; on timeout it stays DRY (passthrough).
                    // SEAT 0 ONLY — the shared-memory bridge carries exactly one
                    // stereo stream per block, so a second seat would need a
                    // second child process and a second bridge. Player 2's
                    // in-process rig and VST chain are unaffected.
                    if (p == 0) owner_.processRemoteBridge(chainBuf, numSamples);
                    }
                    // The dip a session change runs through (a no-op in the
                    // steady state), over whichever path made this block.
                    {
                        float* xp[kChainChannels];
                        for (int ch = 0; ch < chainChannels; ++ch)
                            xp[ch] = chainBuf.getWritePointer(ch);
                        extProc.endBlock(xp, chainChannels, numSamples);
                    }

                    // EVERY UNTRUSTED STAGE IS FOLLOWED BY A SCRUB (dsp/Scrub.h).
                    // The two stages above are the only ones on this path whose
                    // samples this project did not compute: a user VST3, and a
                    // memcpy out of a child process's shared mapping. DspChain
                    // scrubs its own input and every stage boundary; nothing
                    // scrubbed after these, and the very next thing downstream is
                    // a bank of IIR biquads whose reset() the engine only calls on
                    // an OFF -> ON transition. One NaN sample out of a plugin
                    // therefore latched that seat's EQ silent for the rest of the
                    // session -- and the peak meter with it, so the UI showed a
                    // dead input and nothing said why.
                    {
                        float* scrub[kChainChannels];
                        for (int ch = 0; ch < chainChannels; ++ch)
                            scrub[ch] = chainBuf.getWritePointer(ch);
                        dcdsp::scrubNonFinite(scrub, chainChannels, numSamples);
                    }

                    // ABI v29: the inspector EQ over the GUITAR, when its route
                    // names the input. Applied to every live seat — each with
                    // its own biquad state, because two seats carry different
                    // signals — and before the peak meter, so the meter shows
                    // what the user actually hears.
                    {
                        const int eqRoute = owner_.eqRoute_.load(std::memory_order_relaxed);
                        if (eqRoute == DCA_EQ_ROUTE_INPUT || eqRoute == DCA_EQ_ROUTE_BOTH)
                        {
                            float* chPtrs[kChainChannels];
                            for (int ch = 0; ch < chainChannels; ++ch)
                                chPtrs[ch] = chainBuf.getWritePointer(ch);
                            // The analyser follows the route: it shows the bus
                            // the bands are on, and seat 0 is the one the editor
                            // is looking at.
                            if (p == 0 && eqRoute == DCA_EQ_ROUTE_INPUT)
                                owner_.writeSpectrumTap(chPtrs, chainChannels, numSamples);

                            owner_.applyEqBank(owner_.eqInputFilters_[(size_t) p], kChainChannels,
                                               chPtrs, chainChannels, numSamples,
                                               eqRetune, owner_.eqInputEngaged_[(size_t) p]);

                            const float eqTrim = owner_.eqTrimLin_.load(std::memory_order_relaxed);
                            for (int ch = 0; ch < chainChannels; ++ch)
                                rampOrScale(chPtrs[ch], numSamples,
                                            owner_.eqInputTrimCurrent_[(size_t) p], eqTrim);
                            owner_.eqInputTrimCurrent_[(size_t) p] = eqTrim;
                        }
                        else
                        {
                            owner_.eqInputEngaged_[(size_t) p].fill(false);
                            owner_.eqInputTrimCurrent_[(size_t) p] = 1.0f;
                        }
                    }

                    // Peak meter from post-chain signal.
                    float localPeak = 0.0f;
                    for (int ch = 0; ch < chainChannels; ++ch)
                    {
                        const float* pk = chainBuf.getReadPointer(ch);
                        for (int i = 0; i < numSamples; ++i)
                        {
                            const float a = std::abs(pk[i]);
                            if (a > localPeak) localPeak = a;
                        }
                    }
                    auto& peakSlot = owner_.inputPeak_[(size_t) p];
                    float prev = peakSlot.load(std::memory_order_relaxed);
                    while (localPeak > prev &&
                           !peakSlot.compare_exchange_weak(prev, localPeak,
                                std::memory_order_acq_rel, std::memory_order_relaxed)) {}

                    // Sum this seat into the monitor mix. The per-player trim
                    // defaults to 1.0, so a single-player mix is unchanged.
                    const float pmon = pmonNow;
                    // `|| pmonPrev > 0` matters: the old guard skipped the mix
                    // outright the moment the monitor reached zero, which is a
                    // full-amplitude cut in one sample — a CLICK, and the one
                    // gesture guaranteed to produce it is muting the monitor.
                    // The ramp has to run for the block that arrives at zero.
                    const float pmonPrev = owner_.monitorGainCurrent_[(size_t) p];
                    if ((pmon > 0.0f || pmonPrev > 0.0f) && numOutputChannels > 0 && out != nullptr)
                    {
                        for (int ch = 0; ch < numOutputChannels; ++ch)
                        {
                            float* dst = out[ch];
                            const int chainCh = (ch < chainChannels) ? ch : (chainChannels - 1);
                            const float* src = chainBuf.getReadPointer(chainCh);
                            if (dst == nullptr) continue;
                            rampOrScaleAdd(dst, src, numSamples, pmonPrev, pmon);
                        }
                        owner_.monitorGainCurrent_[(size_t) p] = pmon;
                    }

                    // Idle sleep bookkeeping for this seat: what the rig put on
                    // the device output this block (post-EQ, times the louder
                    // end of the monitor ramp, never below unity).
                    {
                        const float g = juce::jmax(1.0f, juce::jmax(pmon, pmonPrev));
                        owner_.rigIdleEndBlock(p, localPeak * g, numSamples,
                                               inputPluginsRan || extEngaged
                                                   || (p == 0 && owner_.bridgeRanThisBlock_));
                    }
                }
            }

            // Click bus: synthesised metronome + clap mixed AFTER master bus
            // so master volume + filters do not affect click level. The mirror
            // pickup still captures them because it samples the post-mix
            // buffer below. blockStartFrame lets a scheduled click start at a
            // sample offset inside this block instead of at its head.
            owner_.mixClickBus(out, numOutputChannels, numSamples, blockStartFrame);

            // Internal MIDI instrument: render scheduled MIDI events through
            // the attached VSTi (if any) and mix on top of the master output
            // BEFORE the mirror pickup so OBS / Discord hear the instrument.
            owner_.renderInstrumentBus(out, numOutputChannels, numSamples, blockStartFrame);

            // Out-of-process instrument: mix the sandbox child's rendered
            // VSTi block (running a zero-input round trip first when the
            // device has no live input, so MIDI still reaches the child).
            owner_.renderRemoteInstrument(out, numOutputChannels, numSamples);

            // EVERY UNTRUSTED STAGE IS FOLLOWED BY A SCRUB (dsp/Scrub.h). The
            // three mixes above are the last ones into the device buffer and two
            // of them are somebody else's arithmetic: an in-process VSTi and the
            // sandbox child's instrument leg (another memcpy out of a foreign
            // mapping). Everything past this point — the device, and the mirror
            // FIFO plus its resampler, i.e. what OBS / Discord hear — has no
            // guard of its own.
            dcdsp::scrubNonFinite(out, numOutputChannels, numSamples);

            // MASTER-BUS CEILING (ABI v34). The scrub above repairs NaN/Inf and
            // says NOTHING about magnitude, so until this stage existed the sum
            // of song + every seat's monitor + click + instrument reached the
            // driver — and the mirror — unbounded. It sits HERE, after the last
            // contributor and BEFORE mirror_.pushBlock, because the mirror's
            // float path is where over-range samples survive to wrap inside
            // OBS / Discord's encoder. See MasterLimiter.h.
            owner_.masterLimiter_.process(
                out, numOutputChannels, numSamples,
                owner_.masterLimiterEnabled_   .load(std::memory_order_relaxed),
                owner_.masterLimiterCeilingLin_.load(std::memory_order_relaxed),
                owner_.masterLimiterReleaseMs_ .load(std::memory_order_relaxed));

            // Streaming mirror: capture the FINAL post-master, post-monitor
            // signal so OBS / Discord / Twitch hear song + processed guitar.
            owner_.mirror_.pushBlock(out, numOutputChannels, numSamples);

            // Callback timing: lock-free max-tracker plus a one-shot warning
            // when the callback exceeds 80% of the block's wall-clock budget.
            // We never log from inside the audio thread by default — only
            // bump an atomic flag that a non-realtime drain thread can pick
            // up. Threshold check uses integer math; no allocations.
            owner_.recordCallbackTiming(callbackStart, numSamples);
        }

    public:
        void audioDeviceAboutToStart(juce::AudioIODevice* device) override
        {
            // W4-N6: ONE position on whether `device` can be null, taken here.
            // This method used to null-check it on one line
            // (`device != nullptr ? getXRunCount() : -1`) and dereference it
            // unconditionally on the four after, so a reader could not tell
            // which of the two beliefs was the live one — and if the null branch
            // was right, :1215 was a crash. JUCE never passes null; making that
            // the stated contract is the honest form, and it keeps
            // publishDeviceSnapshot's own null branch meaningful, because
            // audioDeviceStopped reaches clearDeviceSnapshot by another route.
            //
            // prepareMasterBus still has a device-less branch and that is NOT
            // dead: it is also reached from the offline/tooling path, where
            // there is genuinely no device.
            if (device == nullptr) { jassertfalse; return; }
            // The render clock is per-device-session: rewind it (and drop the
            // pending clicks that were placed against the old one) before the
            // first callback of the new session runs.
            owner_.resetRenderClock();
            // Same for the dropout detector: a new device session has no
            // previous callback to measure a gap against, and its first
            // callbacks are always late while the device thread warms up.
            // Missing this is how an engine REBUILD invents a dropout that
            // never happened (device change, NAM 48 kHz request, watchdog
            // recovery — all three go through here).
            owner_.resetXrunSession();
            // Publish the device's numbers so nothing outside this thread has
            // to dereference the AudioIODevice itself (see deviceSnapshot()).
            owner_.publishDeviceSnapshot(device);
            owner_.audioThreadDevice_.store(device, std::memory_order_release);
            owner_.deviceXrunRaw_.store((int32_t) device->getXRunCount(),
                                        std::memory_order_relaxed);
            // RATE **AND BLOCK**: the sandbox child re-prepares its plugins
            // from this, the way reprepareAttachedPlugins does for the
            // in-process ones. Publishing only the rate left every sandboxed
            // VST3 on the configuration the child was launched with.
            owner_.remoteBridge_.setDevicePrepare((int) device->getCurrentSampleRate(),
                                                  device->getCurrentBufferSizeSamples());
            {
                const int maxBlock = device->getCurrentBufferSizeSamples();
                // EVERY seat's shifter is prepared, not just the live ones
                // (ABI v31). playerCount_ can be raised from the message thread
                // mid-session — that is what starting a co-op run does — and a
                // shifter first prepared on the audio thread would be an
                // allocation on the audio thread. Same reasoning as
                // chainBuffer_, which is sized for kMaxPlayers whatever the
                // count is.
                for (int p = 0; p < kMaxPlayers; ++p)
                {
                    owner_.dropTune_[(size_t) p].prepare(device->getCurrentSampleRate(), maxBlock, 2);
                    // Same 2x headroom as chainBuffer_ (audit #5): a block-size
                    // renegotiation up to 2x must never make the callback want a
                    // resize — the callback now SKIPS the shifter instead of
                    // allocating when a block still exceeds this.
                    for (auto& s : owner_.dropScratch_[(size_t) p])
                        s.assign((size_t) juce::jmax(maxBlock * 2, 2048), 0.0f);
                    // Re-amp (v37): the injected take's per-block mono scratch,
                    // with the same headroom and the same skip-don't-grow rule.
                    owner_.injectionScratch_[(size_t) p].assign((size_t) juce::jmax(maxBlock * 2, 2048), 0.0f);
                    owner_.injectionGainCurrent_[(size_t) p] = 0.0f;
                    // v47: the insert learns the device rate (it engages only
                    // when the rate matches its ring's) and the dip length.
                    owner_.extProc_[(size_t) p].prepare(device->getCurrentSampleRate());
                }
                // Chart source (v38): scratch + every published voice follow the
                // new rate and block, with the same 2x headroom.
                owner_.prepareChartSourcesForDevice(device->getCurrentSampleRate(), maxBlock);
                owner_.injectionRate_.store(device->getCurrentSampleRate() > 0.0
                                                ? device->getCurrentSampleRate() : 44100.0,
                                            std::memory_order_relaxed);
                // Size the sandbox-bridge silence buffer HERE, not on the audio
                // callback. renderRemoteInstrument used to setSize() it lazily,
                // which heap-allocates on the real-time thread.
                std::lock_guard<std::mutex> lk(owner_.remoteBridgeMutex_);
                owner_.bridgeSilentBuf_.setSize(dcvb::kMaxChannels,
                                                juce::jmax(maxBlock, 512), false, true, true);
            }
            owner_.prepareMasterBus(device);
            owner_.player_.audioDeviceAboutToStart(device);
        }

        void audioDeviceStopped() override
        {
            owner_.player_.audioDeviceStopped();
            // The device object is about to be destroyed. Anything still
            // holding its NUMBERS is fine; anything holding a POINTER is not.
            owner_.audioThreadDevice_.store(nullptr, std::memory_order_release);
            owner_.clearDeviceSnapshot();
            // No callbacks follow: a meter must read idle, not the last load.
            owner_.loadAveragePub_.store(0.0f, std::memory_order_relaxed);
            owner_.loadPeakPub_   .store(0.0f, std::memory_order_relaxed);
            // v47: no callback follows, so each insert lets go of its session
            // (a close in between may then unmap it).
            for (auto& x : owner_.extProc_) x.deviceStopped();
        }

    private:
        AudioEngine& owner_;
    };

    // --- Device snapshot (2026-09-10) --------------------------------------
    //
    // `AudioDeviceManager::getCurrentAudioDevice()` returns a raw pointer that
    // `setAudioDeviceSetup` (i.e. dca_engine_reconfigure and
    // dca_engine_set_device_type) DELETES. Every caller below used to
    // dereference it from whatever thread it was on — the ABI's rate / block /
    // latency getters, the xrun stats the managed health timer polls, the load
    // thread, the offline-rig "is a device live" check — with nothing but a
    // null check between them and a use-after-free. Nobody had seen it crash,
    // which is the least reassuring property a race can have.
    //
    // The numbers are published on the DEVICE thread in
    // audioDeviceAboutToStart, and cleared in audioDeviceStopped, which JUCE
    // calls before the device is torn down. Readers get a value, never a
    // pointer.
    std::atomic<int>  stretchQuality_    { 0 };   // 0 = Finer, 1 = Faster (per ENGINE)

    std::atomic<int>  snapSampleRate_    { 0 };
    std::atomic<int>  snapBlockFrames_   { 0 };
    std::atomic<int>  snapOutputLatency_ { 0 };
    std::atomic<int>  snapOutputChannels_{ 0 };
    std::atomic<int>  snapInputChannels_ { 0 };
    std::atomic<bool> snapDeviceOpen_    { false };

    juce::AudioDeviceManager   deviceManager_;
    juce::AudioFormatManager   formatManager_;
    juce::TimeSliceThread      readThread_{ "DotCharter.AudioReadAhead" };
    std::atomic<int64_t>       preloadedBytes_{0};
    juce::MixerAudioSource     mixer_;
    juce::AudioSourcePlayer    player_;
    EngineCallback             callback_{*this};
    std::mutex                 sourcesMutex_;
    std::unordered_set<TransportSource*> sources_;

    // --- Stem group ------------------------------------------------------
    // Lazily created when first source is grouped, torn down when last
    // leaves. stemSubMixer_ holds the per-source panner outputs (which run
    // through the source's bypassed Rubber Band stretcher); the shared
    // stemGroupStretcher_ applies tempo/pitch to the SUM and feeds mixer_.
    std::mutex                                       stemGroupMutex_;
    std::unique_ptr<juce::MixerAudioSource>          stemSubMixer_;
    std::unique_ptr<class StemPositionableSource>    stemPositionable_;
    std::unique_ptr<class RubberBandAudioSource>     stemGroupStretcher_;
    std::unordered_set<TransportSource*>             stemSources_;
    std::atomic<float>                               stemGroupTempo_  { 1.0f };
    std::atomic<float>                               stemGroupPitch_  { 0.0f };
    std::atomic<bool>          started_{false};
    // Set by a stop() that actually stopped. start() then REFUSES (#27): stop()
    // detaches every source for good, so a restart would play silence.
    std::atomic<bool>          stoppedAfterStart_{false};
    /// dca_engine_create2 flags (ABI v41). Written once before the engine is
    /// published to any other thread; read-only afterwards.
    bool                       nullDeviceByConfig_       = false;
    bool                       nullDeviceManualByConfig_ = false;
    std::atomic<float>         monitorGain_{0.0f};
    /// Per-seat "what the last block ended on" for the two live-input gains,
    /// so a slider drag ramps instead of stepping (see rampOrScale).
    /// Audio-thread-only; reseeded by prepareMasterBus, which runs with no
    /// callback in flight.
    std::array<float, kMaxPlayers> monitorGainCurrent_{};
    std::array<float, kMaxPlayers> inputGainCurrent_{};
    // Drop-tune pedal (CSVD — dsp/cores/dcdsp_droptune.h). One engine; the "mode"
    // now only picks its splice-lag ceiling.
    //
    // PER SEAT since ABI v31. There used to be exactly one shifter, applied to
    // the raw device input BEFORE the per-player split — so on a couch with two
    // guitars in different tunings, dropping one dropped BOTH, and the seat that
    // did not ask for it was detuned with nothing on screen saying so. Each seat
    // now owns its shifter, its semitone target and its lag ceiling, and index 0
    // IS the pre-v31 pedal: `setInputPitchShift` is
    // `setPlayerInputPitchShift(0, …)`, exactly as
    // `setInputCaptureChannel` is `setPlayerInputChannel(0, …)`.
    std::array<std::atomic<float>, kMaxPlayers> inputPitchShiftSemis_{};
    std::array<std::atomic<float>, kMaxPlayers> dropTuneMaxLagMs_{};
    std::array<dcdsp::DropTune, kMaxPlayers>    dropTune_;
    std::array<std::array<std::vector<float>, 2>, kMaxPlayers> dropScratch_;
    std::array<std::array<float*, 2>, kMaxPlayers>            dropPtrs_{};
    std::atomic<float>         inputGain_  {1.0f};
    // Per-player post-rig peak meters. [0] is the pre-v28 meter.
    std::array<std::atomic<float>, kMaxPlayers> inputPeak_{};

    // --- Per-player live-input rigs (ABI v28) -----------------------------
    // 1 = the pre-v28 single-rig path, byte-for-byte. > 1 opens one rig per
    // seat, each reading its own hardware input channel.
    std::atomic<int>           playerCount_{1};
    std::array<std::atomic<float>, kMaxPlayers> playerMonitorGain_{};

    // --- Input capture ring (SPSC, audio-thread writer, managed reader) ---
    // Holds the DRY (pre-gain, pre-VST) input as mono float32 at the device
    // sample rate. Size is a power-of-two so the wrap mask is a single AND.
    // ~2.7s @ 48k. Sized generously on purpose: the sole streaming consumer
    // is a Background-priority UI timer (live note detection). When the audio
    // thread is saturated (master + stem-group stretchers) or the CPU is
    // pegged by a stem-separation job, that reader can stall for a second or
    // more; a deep ring lets it catch up without the stream skipping and the
    // analyzer losing its onset history ("detection stops with stems"). 512KB.
    //
    // ABI v28: ONE RING PER PLAYER. Live judging reads a seat's own ring, so
    // two guitars on two inputs are two independent detection streams instead
    // of one shared mono tap that hears both at once. 512 KB each; the rings
    // for inactive seats are simply never written.
    static constexpr int kInputCaptureCapacity = 1 << 17; // 131072 samples (~2.7s @ 48k)
    struct InputCaptureRing
    {
        std::array<float, kInputCaptureCapacity> buf{};
        std::atomic<uint64_t> write{0};
        // `read` used to sit here. readInputCapture is a WINDOWED read — it
        // always returns the last `count` samples — so it never consumed
        // anything and the cursor was stored twice per call and loaded nowhere.
        // The streaming API's cursor below is the one that is really a cursor.
        // Independent cursor for readInputCaptureStream so streaming consumers
        // don't disturb the windowed API (and vice versa).
        std::atomic<uint64_t> streamRead{0};
        // Seqlock-published {captured-total, host QPC ticks} pair, written once
        // per audio callback right after the capture block lands (ABI v16). Odd
        // sequence = writer in flight; readers retry.
        std::atomic<uint32_t> syncSeq{0};
        uint64_t              syncSamples = 0;
        int64_t               syncTicks   = 0;
    };
    std::array<InputCaptureRing, kMaxPlayers> captureRings_{};
    std::array<std::atomic<int>, kMaxPlayers> inputCaptureChannel_{};
    void writeInputCapture(int player, const float* const* in, int numInputChannels, int numSamples) noexcept;

    // --- Input injection / re-amp (ABI v37) --------------------------------
    // A mono take at the ENGINE rate that REPLACES one seat's live input, fed in
    // after that seat's drop tune and before its capture ring — so the rig, the
    // tuner and the input meter all hear it, and the live guitar is muted for
    // as long as a take is loaded (paused = silence).
    //
    // The take is immutable once published. The control thread owns it
    // (`injectionOwned_`, under injectionMutex_), publishes the raw pointer in
    // `injection_[p]`, and frees a replaced one only after a Dekker handshake
    // with the callback: the callback raises `injectionBusy_` BEFORE it loads
    // the pointer and drops it after its last read, both seq_cst, so a control
    // thread that swapped the pointer and then reads `busy == false` knows no
    // block can still be reading the old take. No allocation, lock or free on
    // the audio thread.
    struct InputInjection
    {
        std::vector<float>   samples;         // mono, engine rate; never resized once published
        std::atomic<int64_t> position{ 0 };   // audio thread writes; control reads
        std::atomic<int64_t> seekTo{ -1 };    // control writes; audio consumes
        uint64_t             serial = 0;      // unique per take: the callback's "is this a new take"
    };
    uint64_t                                               injectionSerialNext_ = 0;    // injectionMutex_
    std::array<uint64_t, kMaxPlayers>                      injectionLastSerial_{};      // audio thread
    std::array<std::atomic<InputInjection*>, kMaxPlayers>  injection_{};
    std::array<std::unique_ptr<InputInjection>, kMaxPlayers> injectionOwned_{};  // injectionMutex_
    std::array<std::atomic<bool>, kMaxPlayers>             injectionPlaying_{};
    std::array<std::atomic<bool>, kMaxPlayers>             injectionLoop_{};
    std::array<float, kMaxPlayers>                         injectionGainCurrent_{}; // audio thread
    std::array<std::vector<float>, kMaxPlayers>            injectionScratch_;       // sized in aboutToStart
    std::array<std::array<const float*, 1>, kMaxPlayers>   injectionPtrs_{};        // audio thread
    std::atomic<bool>                                      injectionBusy_{ false };
    std::atomic<int>                                       injectionActive_{ 0 };  // seats with a take
    std::mutex                                             injectionMutex_;
    std::atomic<double>                                    injectionRate_{ 44100.0 }; // deviceSr_ for the ramp

    // ABI v47: each seat's external process insert. Its session is published by
    // the control thread and taken by the audio thread at the bottom of a dip;
    // nothing on the callback path allocates, locks or waits for it.
    std::array<ExternalProcessInsert, kMaxPlayers>         extProc_;

    /// Audio thread. Renders this seat's injected take into its scratch and
    /// returns true when the seat's input is REPLACED this block (a take is
    /// loaded); `outPtrs` then names the mono scratch. False = live input.
    bool renderInputInjection(int player, int numSamples, const float* const*& outPtrs) noexcept;

    /// Control thread, injectionMutex_ held. Swaps `next` in (may be null) and
    /// frees the take it replaced once no block can be reading it.
    void swapInjectionLocked(int player, std::unique_ptr<InputInjection> next);

    // --- Chart source (ABI v38) --------------------------------------------
    // A per-seat note generator (src/synth/) mixed into the seat's RIG INPUT —
    // after writeInputCapture, so detection, scoring and the tuner never hear
    // it — and rendered against the SONG CLOCK sampled once per block below.
    //
    // Publication is the re-amp take's pattern: the note table and the voice
    // are built (and the voice prepared) on the CONTROL thread, published by an
    // atomic pointer swap, and a replaced one is freed on the control thread
    // only after a seq_cst Dekker handshake on `chartBusy_` (bounded 2 s, then
    // LEAKED and logged). Nothing on the audio thread allocates, locks or frees.
    struct ChartVoiceSlot
    {
        std::unique_ptr<dcsynth::ChartVoice> voice;
        int      kind   = DCA_CHART_VOICE_PLACEHOLDER;
        uint64_t serial = 0;        // "is this a new voice" for the scheduler
        dcvoice::InstrumentConfig config {};   // what it was built for (#50)
        double   preparedRate  = 0.0;          // control side
        int      preparedBlock = 0;
        bool     sampled   = false;
        bool     lazyZones = false;            // plays lazy packs (#417): the reaper services its requests
        // A REPLACED voice cross-fades out (ABI v40): the audio thread calls
        // fadeOutAll once (fadeStarted, audio thread only) and raises fadeDone
        // when it is silent; the chart reaper then frees it.
        bool              fadeStarted = false;
        std::atomic<bool> fadeDone { false };
    };
    struct ChartSourceSeat
    {
        std::atomic<dcsynth::ChartNoteTable*> table { nullptr };
        std::unique_ptr<dcsynth::ChartNoteTable> tableOwned;   // chartMutex_
        std::atomic<ChartVoiceSlot*>          voice { nullptr };
        std::unique_ptr<ChartVoiceSlot>       voiceOwned;      // chartMutex_
        // The voice being replaced, cross-fading out (ABI v40). Published
        // BEFORE the new `voice`, cleared by the reaper.
        std::atomic<ChartVoiceSlot*>          fading { nullptr };
        std::unique_ptr<ChartVoiceSlot>       fadingOwned;     // chartMutex_
        // The REQUEST (chartMutex_): what to build whenever a voice is needed.
        int                        requestedKind = DCA_CHART_VOICE_PLACEHOLDER;
        dcvoice::InstrumentConfig  guitarConfig  = dcvoice::legacyConfig(false);
        dcvoice::InstrumentConfig  bassConfig    = dcvoice::legacyConfig(true);
        // Reaper bookkeeping (chartMutex_): steady-clock milliseconds.
        bool                       releasePending  = false;
        int64_t                    releaseDeadline = 0;
        int64_t                    fadeDeadline    = 0;
        // Serialises this seat's control calls, INCLUDING the pack load, so
        // chartMutex_ (status, device start) is never held across a load.
        std::mutex                 controlMutex;
        // The audio thread's runState, mirrored for the reaper.
        std::atomic<int>           runStateShared { 0 };
        std::atomic<bool>    enabled { false };
        std::atomic<float>   gain    { 1.0f };
        std::atomic<double>  offset  { 0.0 };
        std::atomic<int>     mode    { DCA_CHART_SOURCE_MIX };
        // Audio thread only from here on.
        dcsynth::ChartScheduler scheduler;
        std::vector<float>   scratch;        // sized in aboutToStart (2x headroom)
        float                gainCurrent = 0.0f;
        int                  runState    = 0;    // 0 off, 1 on, 2 fading out after a disable
    };
    std::array<ChartSourceSeat, kMaxPlayers> chart_;
    std::atomic<bool>     chartBusy_   { false };
    std::atomic<int>      chartActive_ { 0 };       // seats enabled
    std::atomic<bool>     chartZonePreload_ { true };   // see setChartZonePreloadForTesting
    std::mutex            chartMutex_;              // control side only
    uint64_t              chartSerialNext_ = 0;     // chartMutex_

    // The song clock: ONE transport source, read on the audio thread right
    // after the pull (so it describes the block just rendered) — never
    // dereferenced anywhere else. Unbinding nulls it and waits `songClockBusy_`
    // out (Dekker), which is what makes a source's teardown safe.
    std::atomic<TransportSource*> songClockSource_ { nullptr };
    std::atomic<bool>             songClockBusy_   { false };
    std::mutex                    songClockMutex_;  // bind / forget
    dcsynth::SongClockSample      songClock_;       // audio thread

    // Instrument route (ABI v38): -1 = after the master (the pre-v38 path).
    std::atomic<int> instrumentRoute_ { -1 };
    /// Audio thread: the instrument was already rendered into a seat this block.
    bool             instrumentRenderedForSeat_ = false;
    int              instrumentWorkChannels_    = 0;

    /// Audio thread. Samples the song clock for this block.
    void sampleSongClock() noexcept;
    /// Audio thread. Renders seat `player`'s chart source (and the routed
    /// instrument, when it is routed here) into its chain buffer, after the
    /// input fill. `instrumentHere` = the instrument was rendered for this seat.
    void mixSeatGenerators(int player, juce::AudioBuffer<float>& chainBuf, int numSamples,
                           bool instrumentHere) noexcept;
    /// Audio thread. Drains the MIDI queue and runs the in-process instrument
    /// into instrumentBuffer_; returns the channels rendered (0 = none).
    int  renderInstrumentWork(int numSamples, int64_t blockStartFrame) noexcept;
    /// Control thread, chartMutex_ held: swap a table / voice in, free the old
    /// one once no block can be reading it.
    void retireChartLocked(std::unique_ptr<dcsynth::ChartNoteTable> table,
                           std::unique_ptr<ChartVoiceSlot> voice);
    /// Control thread, NO lock held: a new voice of `kind` for `config`, built
    /// and prepared (this is where a sample pack's zone table loads), its packs
    /// LAZY (#417): the zones `preload` can play are decoded here (null = none
    /// yet). `freshPacks` = new pack instances, not the shared ones. Null if
    /// unavailable.
    std::unique_ptr<ChartVoiceSlot> buildChartVoice(int kind, const dcvoice::InstrumentConfig& config,
                                                    const dcsynth::ChartNoteTable* preload, bool freshPacks);
    /// Control thread, chartMutex_ held: publish `next` as the seat's voice;
    /// the one it replaces cross-fades out when the seat is live (`fade`),
    /// otherwise it is retired at once.
    void publishChartVoiceLocked(int player, std::unique_ptr<ChartVoiceSlot> next, bool fade);
    /// chartMutex_ held: free the seat's fading voice once no block reads it.
    void retireFadingLocked(int player);
    /// The voice the seat's REQUEST names (kind + that family's config) is the
    /// published one. chartMutex_ held.
    bool chartVoiceMatchesLocked(int player) const;
    const dcvoice::InstrumentConfig& requestedConfigLocked(int player) const;
    /// Control thread, the seat's controlMutex held, chartMutex_ NOT held:
    /// build the requested voice when the published one does not match it.
    bool syncChartVoice(int player, bool fade);

    // The CHART REAPER (ABI v40, #61 part 2): one control-side thread, started
    // on first use, that frees replaced voices once their cross-fade is done
    // and a disabled seat's voice (and so its sample pack) once it is silent —
    // so neither a setter nor the audio thread ever waits or frees for it.
    std::thread             chartReaper_;
    std::condition_variable chartReaperCv_;
    bool                    chartReaperStop_ = false;   // chartMutex_
    bool                    chartReaperWork_ = false;   // chartMutex_
    void wakeChartReaperLocked();
    void chartReaperLoop();
    void stopChartReaper();
    /// Reaper, chartMutex_ held via `lk`: decodes the zones the audio thread
    /// asked for (#417) with the lock RELEASED. True when it decoded anything.
    bool serviceChartZonesLocked(std::unique_lock<std::mutex>& lk);
    /// chartMutex_ held: a published voice plays lazy packs (the reaper polls).
    bool anyLazyChartVoiceLocked() const;
    /// Device thread, no callback in flight (audioDeviceAboutToStart): size the
    /// scratch and re-prepare every published voice for the new rate / block.
    void prepareChartSourcesForDevice(double sampleRate, int maxBlock);

    // Master bus
    std::atomic<float>         masterGain_  { 1.0f };

    // --- Master-bus ceiling (ABI v34) --------------------------------------
    // Everything here is OFF-path until the callback reaches the final stage;
    // the limiter object itself is prepared in prepareMasterBus and never
    // allocates below that.
    //
    // DEFAULT OFF (v35), and that reverses v34. Default ON made this engine's
    // own rule — "everything is OFF at engine start; a user must not pay for, or
    // hear any change from, a feature they are not using", which MasterLimiter.h
    // rule 1 states and which playerCount_ and the v29 EQ both follow — true
    // only for a user who had switched the limiter OFF. Concretely: the live
    // monitor path runs through this stage, so every guitarist gained 1 ms of
    // latency on their own playing simply by upgrading, and every existing
    // consumer's click-alignment arithmetic shifted by 48 samples at 48 kHz.
    // The rationale for ON was real (the streaming mirror's float path wrapping
    // in OBS) but the mirror does not need the live monitor delayed, and the RT
    // gate had already had to add setMasterLimiter(false, ...) to one scenario
    // for exactly this reason. The HOST opts in.
    MasterLimiter              masterLimiter_;
    std::atomic<bool>          masterLimiterEnabled_    { false };
    std::atomic<float>         masterLimiterCeilingLin_ { 0.96605086f }; // -0.3 dBFS
    std::atomic<float>         masterLimiterReleaseMs_  { MasterLimiter::kDefaultReleaseMs };
    /// The master gain the LAST block actually ended on. Audio-thread-only
    /// (prepareMasterBus reseeds it with no callback in flight), so no atomic.
    /// See rampOrScale: a slider drag publishes a new gain per UI tick and a
    /// per-block constant multiply steps the waveform at every block boundary.
    float                      masterGainCurrent_ { 1.0f };
    std::atomic<float>         hpFreq_      { 0.0f };
    std::atomic<float>         hpQ_         { 0.707f };
    std::atomic<float>         lpFreq_      { 0.0f };
    std::atomic<float>         lpQ_         { 0.707f };
    std::atomic<float>         bpFreq_      { 0.0f };
    std::atomic<float>         bpQ_         { 0.707f };
    bool                      filterDirty_ = true;

    /// One published 2nd-order section: the 5 normalized RBJ coefficients JUCE
    /// keeps (b0,b1,b2,a1,a2) plus whether the band is engaged at all.
    struct BiquadCoeffs { float v[5] { 1.f, 0.f, 0.f, 0.f, 0.f }; bool engaged = false; };
    struct MasterFilterCoeffs
    {
        BiquadCoeffs hp, lp, bp;
        /// ABI v29. Published in the SAME triple-buffered slot as the three
        /// above, so one acquire load on the audio thread still covers
        /// everything: two publish paths would have needed two slot indices and
        /// the callback could then read a band set from one tick and a filter
        /// set from another.
        BiquadCoeffs eq[kEqBands];
    };
    /// Three exclusive slots: publishers cannot reuse the callback's current
    /// slot even if several updates arrive during one callback.
    LatestSnapshot<MasterFilterCoeffs> filterCoefficients_;
    std::recursive_mutex filterPublishMutex_; // publishers only; never the callback
    /// Message thread only. Recomputes all three sections from the current
    /// atomics + deviceSr_ and publishes them. The callback raises its own dirty flags.
    void publishMasterFilterCoeffs() noexcept;

    double                     deviceSr_    = 44100.0;
    int                        deviceBlock_ = 512;
    /// THE master-bus width (#31): the channel count every per-channel master
    /// stage — the HP/LP/BP banks, the song EQ bank, the limiter — is prepared
    /// for, and the only number any of them is sized from. The device's ACTIVE
    /// output count; see prepareMasterBus for why. 0 until the first prepare,
    /// so a block before it is skipped like any other over-wide block.
    /// Written only in prepareMasterBus (no callback in flight).
    int                        masterBusWidth_ = 0;
    /// Blocks the filter + song-EQ stages skipped for being wider than
    /// masterBusWidth_ (#31). Audio thread writes, anyone reads.
    std::atomic<unsigned long> masterBusSkipped_ { 0 };
#if DCA_RTCHECK
    int      rtLastOutChannels_ = -1;
    uint32_t rtLastOutNullMask_ = 0;
#endif
    std::vector<juce::dsp::IIR::Filter<float>> hpFilters_;
    std::vector<juce::dsp::IIR::Filter<float>> lpFilters_;
    std::vector<juce::dsp::IIR::Filter<float>> bpFilters_;
    // Audio-thread-only: whether each band was running before the current
    // filterDirty_ rebuild. Retuning a RUNNING band keeps its state (no click
    // while sweeping — audit #8); only an off->on transition resets.
    bool hpEngaged_ = false;
    bool lpEngaged_ = false;
    bool bpEngaged_ = false;

    // --- Parametric EQ (ABI v29) ------------------------------------------
    // Message-thread writes, audio-thread reads. Everything is off at start, so
    // an app that never touches the v29 exports runs the pre-v29 path exactly.
    std::array<std::atomic<int>,   kEqBands> eqType_ {};
    std::array<std::atomic<float>, kEqBands> eqFreq_ {};
    std::array<std::atomic<float>, kEqBands> eqQ_ {};
    std::array<std::atomic<float>, kEqBands> eqGain_ {};
    std::array<std::atomic<bool>,  kEqBands> eqOn_ {};
    std::atomic<bool>  eqEnabled_ { false };
    std::atomic<int>   eqSolo_    { -1 };
    std::atomic<int>   eqRoute_   { DCA_EQ_ROUTE_SONG };
    std::atomic<float> eqTrimLin_ { 1.0f };
    /// The guitar banks' own retune flag. See the comment at its consumer in the
    /// callback: filterDirty_ is consumed by processMasterBus, which runs first.
    bool eqInputDirty_ = true;
    float eqSongTrimCurrent_ = 1.0f;
    std::array<float, kMaxPlayers> eqInputTrimCurrent_ { 1.f, 1.f, 1.f, 1.f };

    /// The song bus's own biquad state: kEqBands sections per output channel,
    /// indexed [band * masterBusWidth_ + ch]. One flat vector rather than a
    /// vector of vectors so prepare() allocates exactly once.
    std::vector<juce::dsp::IIR::Filter<float>> eqSongFilters_;
    /// Per SEAT, because two seats carry different signals and biquad memory
    /// cannot be shared between them: [band * kChainChannels + ch].
    std::array<std::vector<juce::dsp::IIR::Filter<float>>, kMaxPlayers> eqInputFilters_ {};
    /// Audio-thread-only, mirroring hpEngaged_ &c: retuning a RUNNING band keeps
    /// its state (no click while dragging a handle); only off->on resets.
    std::array<bool, kEqBands> eqSongEngaged_ {};
    std::array<std::array<bool, kEqBands>, kMaxPlayers> eqInputEngaged_ {};

    /// Applies the published EQ sections to one interleaved-by-pointer buffer.
    /// `engaged` is the audio-thread's own per-band was-running memory for THIS
    /// bank and is updated in place. Channel `ch` runs on state `ch`, never on a
    /// clamped index: a buffer wider than `bankChannels` is retuned but left
    /// unfiltered (the caller counts the skip — #31).
    void applyEqBank(std::vector<juce::dsp::IIR::Filter<float>>& bank, int bankChannels,
                     float* const* data, int numChannels, int numSamples,
                     bool retune, std::array<bool, kEqBands>& engaged) noexcept;

    /// The analyser tap. A lock-free append-only ring of MONO-summed samples the
    /// audio thread writes and readEqSpectrum snapshots. A torn read costs a
    /// slightly wrong bin in one frame of a 15 Hz display, which is why this
    /// needs no synchronisation beyond the release store — and why it must never
    /// be used for anything that is not a picture.
    static constexpr int kSpectrumFftOrder = 11;              // 2048-point
    static constexpr int kSpectrumSize     = 1 << kSpectrumFftOrder;
    std::array<float, kSpectrumSize> spectrumRing_ {};
    std::atomic<uint64_t>            spectrumWritten_ { 0 };
    void writeSpectrumTap(const float* const* data, int numChannels, int numSamples) noexcept;
    /// Message-thread scratch for readEqSpectrum. Guarded by spectrumMutex_
    /// because two managed pollers would otherwise share one FFT workspace; the
    /// AUDIO thread never touches any of it.
    std::mutex                       spectrumMutex_;
    std::unique_ptr<juce::dsp::FFT>  spectrumFft_;
    std::vector<float>               spectrumScratch_;
    std::vector<float>               spectrumWindow_;

    // --- VST input chain ----------------------------------------------------
    // The chain is published as a const snapshot via a std::shared_ptr stored
    // atomically. Writers (attach/detach) take pluginMutex_, copy the current
    // snapshot, mutate the copy and atomically publish the new shared_ptr.
    // The audio thread iterates the captured snapshot under
    // inputChainProcessMutex_ (uncontended except during detach), which
    // doubles as the detach-side wait-one-block barrier protecting the
    // plugin *instances* — the snapshot refcount only protects the vector.
    //
    // ABI v28: one chain, one barrier mutex and one work buffer PER PLAYER.
    // The per-player mutexes keep the barrier precise (detaching from seat 1
    // must not wait on seat 0's chain), and they are separate lock OBJECTS at
    // the same RT lock SITE — tests/rt/rt_lock_sites.txt keys on the site name.
    mutable std::mutex                                              pluginMutex_;
    // Held by the audio thread across the chain's processBlock calls and
    // acquired once by detachInputPlugin after the snapshot swap as a
    // wait-one-block barrier (see processInputChain). Nothing slower than
    // the swap itself ever runs under it on the writer side.
    std::array<std::mutex, kMaxPlayers>                             inputChainProcessMutex_;
    std::array<std::shared_ptr<const std::vector<juce::AudioPluginInstance*>>,
               kMaxPlayers>                                         inputChain_;
    std::array<juce::AudioBuffer<float>, kMaxPlayers>               chainBuffer_;
    juce::MidiBuffer                                                emptyMidi_;

    // ---- Faulted input plugins (W5-E7) ------------------------------------
    // A third-party VST3 that throws out of processBlock is caught (see
    // processInputChain) — but until 2026-09-12 nothing RECORDED that it had,
    // so a plugin that faults deterministically (an expired trial's licence
    // check, a layout it was not prepared for) threw ONCE PER CALLBACK for
    // ever. A C++ throw is not RT-safe at any price: __cxa_allocate_exception
    // mallocs and the unwinder takes a global lock over the .pdata lookup, tens
    // of microseconds against a 2.6 ms budget at 128 frames / 48 kHz — a
    // permanent glitch, with the monitor path dry from the `break` and nothing
    // anywhere saying why.
    //
    // The bitset is AUDIO-THREAD-ONLY state and needs no atomic: it is read and
    // written solely inside processInputChain, under that seat's
    // inputChainProcessMutex_. Bit i is entry i; a chain longer than 32 stops
    // disabling past that point rather than mis-indexing (it still catches and
    // breaks, i.e. the pre-fix behaviour for those entries only).
    //
    // RE-ARMED BY A GENERATION COUNTER, **NOT BY SNAPSHOT POINTER IDENTITY**.
    // Comparing `snap.get()` was the obvious implementation and it is ABA-unsafe
    // in the ugliest way: attach and detach each publish a fresh vector and drop
    // the old one, and the allocator routinely hands the SAME ADDRESS back — so
    // a detach+attach compared EQUAL, the bits survived a chain whose entries
    // had all moved, and bit 0 (the plugin that had faulted) disabled whatever
    // now sat at index 0. The RT gate caught it as "the stage behind it ran 0
    // blocks". A counter that only ever increases cannot alias.
    std::atomic<uint64_t>                     inputChainGeneration_ { 1 };
    std::array<uint64_t, kMaxPlayers>         inputChainFaultGen_ {};
    std::array<std::uint32_t, kMaxPlayers>    inputChainFaulted_  {};
    /// Published counter, ONE per plugin-fault rather than one per block.
    /// Surfaced as dca_xrun_stats::input_plugin_faults.
    std::atomic<int64_t>                      inputPluginFaults_ { 0 };

    // --- Internal MIDI instrument bus ------------------------------------
    // Single-slot virtual instrument; lock-free pointer swap, with a
    // dedicated mutex around prepareToPlay/releaseResources so the audio
    // thread never observes a half-prepared plugin.
    std::atomic<juce::AudioPluginInstance*>       instrumentPlugin_ { nullptr };
    std::mutex                                    instrumentMutex_;
    juce::AudioBuffer<float>                      instrumentBuffer_;
    int                                           instrumentChannels_ { 2 };
    juce::MidiBuffer                              instrumentMidiPending_;  // audio-thread scratch
    std::atomic<float>                            instrumentGain_ { 1.0f };
    /// AUDIO THREAD ONLY, like masterGainCurrent_ / monitorGainCurrent_: the
    /// value instrumentGain_ was actually applied AT by the end of the previous
    /// block, so the next one can ramp from it instead of stepping (W4-N3).
    /// Reseeded in prepareMasterBus. ONE slot, not one per seat: the instrument
    /// bus is a single engine-wide VSTi, and both legs (in-process and sandbox
    /// child) are mutually exclusive within a block — bridgeInstrumentActive_
    /// is only true when nothing is attached in process.
    float                                         instrumentGainCurrent_ { 1.0f };

    // Lock-free queue of pending MIDI short events (ABI v34).
    //
    // Until v34 this carried a bare `uint32_t packed` and every event was
    // stamped at sample 0 of whichever block happened to drain it. The comment
    // that justified it said the queue "would need to carry (event,
    // sampleOffset) pairs synthesised from the host's monotonic clock, and we'd
    // need a free-running sample counter on the audio thread" — and ABI v26
    // shipped exactly that counter (renderFrames_) two years later without
    // anyone coming back here. So the skew stayed at up to one whole buffer,
    // 5-10 ms, on every note of a charted performance.
    //
    // `frame` is an ABSOLUTE output-timeline frame in the same domain as
    // dca_engine_get_render_clock; NEGATIVE means "as soon as possible", which
    // is what dca_engine_send_midi_short still produces and is bit-for-bit the
    // pre-v34 behaviour.
    struct QueuedMidiEvent
    {
        uint32_t packed = 0;      // 0 = empty slot: a real MIDI status is >= 0x80
        int64_t  frame  = -1;     // < 0 = immediate
    };
    static constexpr int kMidiQueueDepth = 512;
    juce::AbstractFifo                            midiFifo_{ kMidiQueueDepth };
    std::array<QueuedMidiEvent, kMidiQueueDepth>  midiQueue_{};
    /// SERIALISES THE PRODUCERS, and there is more than one.
    ///
    /// `juce::AbstractFifo` is SINGLE-producer / single-consumer. The consumer
    /// side was always treated carefully (`discardQueuedMidi` exists precisely
    /// because `reset()` is illegal against a live consumer) but the producer
    /// side was ASSERTED, not enforced: this file said "sendMidiShort is called
    /// from the JUCE MIDI input thread", singular, while
    /// `dca_engine_send_midi_at` and `dca_engine_send_midi_short` are callable
    /// from ANY managed thread. Two concurrent `prepareToWrite`s compute from
    /// the same `validStart`, so both write the same slot and `finishedWrite`
    /// double-advances: one event LOST, one slot of stale garbage forwarded to
    /// somebody's sampler, and no counter moves.
    ///
    /// Latent since the two exports existed; ABI v34's whole purpose is a
    /// sequencer scheduling hundreds of events ahead from its own thread, which
    /// turns that into the design.
    ///
    /// **THE AUDIO THREAD NEVER TAKES THIS.** The consumer
    /// (the parking loops in `collectBridgeMidi` and `renderInstrumentBus`)
    /// stays entirely lock-free — a single consumer against a serialised
    /// producer is exactly the SPSC shape `AbstractFifo` requires. It is a plain
    /// `std::mutex` and NOT a `DCA_RT_LOCK_GUARD` for that reason: a new name in
    /// `rt_lock_sites.txt` would claim the callback acquires it.
    std::mutex                                    midiProducerMutex_;

    // Audio thread ONLY. Drained events are PARKED here until the block that
    // contains their frame arrives, exactly like clickPending_ does for ABI
    // v26's scheduled clicks — the same problem, the same shape, and it is the
    // reason a scheduled event does not have to be clamped into whichever block
    // happened to drain it. A caller may therefore schedule ahead.
    static constexpr int kMidiPending = 512;
    std::array<QueuedMidiEvent, kMidiPending>     midiPending_{};
    /// AUDIO THREAD ONLY. Where the next free-slot search STARTS (W4-N4). A
    /// hint, not a free list — it can point at an occupied slot, so the search
    /// still wraps the whole array before it declares the set full. Turns a
    /// restart-from-zero scan per drained event (up to ~131,000 slot tests in
    /// one callback when a sequencer schedules ahead) into an amortised walk.
    size_t                                        midiPendingHint_ { 0 };

    // Poll-only diagnostics for the above (dca_engine_get_midi_timing_stats).
    // `late` is the interesting one: an event whose frame had already gone past
    // when its block ran is placed at sample 0 and counted here, which is the
    // only signal that a caller's scheduling horizon is too short.
    std::atomic<int64_t> midiScheduledEvents_ { 0 };
    std::atomic<int64_t> midiLateEvents_      { 0 };
    std::atomic<int64_t> midiDroppedEvents_   { 0 };

    // --- Out-of-process VST bridge ----------------------------------------
    // remoteBridgeMutex_ guards every audio-thread touch of the bridge and
    // doubles as the disable-side wait-one-block barrier (same pattern as
    // inputChainProcessMutex_). remoteBridgeConfigMutex_ serializes
    // enable/disable calls against each other.
    RemoteVstBridge            remoteBridge_;
    /// See DeviceLifecycleLock. Message/caller threads only; try-locked on both
    /// sides so neither an offline render nor a device open ever WAITS.
    std::mutex                 deviceLifecycleMutex_;
    std::mutex                 remoteBridgeMutex_;
    std::mutex                 remoteBridgeConfigMutex_;
    // Audio-thread-only per-callback state.
    bool                       bridgeRanThisBlock_     = false;
    bool                       bridgeInstrumentActive_ = false;
    const float*               bridgeInstrL_ = nullptr;
    const float*               bridgeInstrR_ = nullptr;
    juce::AudioBuffer<float>   bridgeSilentBuf_;
    std::array<uint32_t, dcvb::kMaxMidiEvents> bridgeMidiScratch_{};
    /// AUDIO THREAD ONLY. Sample offset of bridgeMidiScratch_[i] inside the
    /// block it is sent with (bridge v3, JuceSharp #28).
    std::array<uint32_t, dcvb::kMaxMidiEvents> bridgeMidiOffsets_{};
    /// AUDIO THREAD ONLY (under remoteBridgeMutex_). The sandbox leg's own
    /// PARKED set: the bridge drains the fifo here and forwards each event in
    /// the round trip of the block that contains its frame, at its offset —
    /// the shape renderInstrumentWork uses for the in-process instrument with
    /// midiPending_. A SEPARATE set because the two legs sit behind different
    /// barriers (remoteBridgeMutex_ vs instrumentMutex_) and must never share
    /// audio-thread state across them.
    std::array<QueuedMidiEvent, kMidiPending>  bridgeMidiPending_{};
    size_t                                     bridgeMidiPendingHint_ { 0 };
    /// AUDIO THREAD ONLY. Render-clock frame of sample 0 of the current block,
    /// stored at the head of the callback.
    int64_t                                    bridgeBlockStartFrame_ { 0 };
    /// AUDIO THREAD ONLY. How many events at the head of bridgeMidiScratch_ the
    /// sandbox child has NOT acknowledged, and which the next round trip must
    /// re-send (W4-N7). 0 on every successful trip.
    int                                        bridgeMidiHeld_ { 0 };

    /// Message thread: throw away every queued MIDI short, safely.
    ///
    /// NOT midiFifo_.reset(). juce::AbstractFifo is a single-producer /
    /// single-consumer structure and reset() rewrites BOTH index atomics, so it
    /// is only legal when nothing else is touching the fifo. Both callers
    /// (attach/detachInstrumentPlugin) had two racers:
    ///
    ///   * the CONSUMER — collectBridgeMidi runs on the audio thread whenever
    ///     no in-process instrument is attached, i.e. exactly around an attach,
    ///     which is when reset() was called. Taking remoteBridgeMutex_ here is
    ///     the same wait-one-block barrier that leg already uses, so the drain
    ///     and this cannot overlap.
    ///   * the PRODUCERS — plural, and that is the point. sendMidiShort runs on
    ///     the JUCE MIDI input thread, not the message thread, and
    ///     dca_engine_send_midi_at / _send_short are callable from ANY managed
    ///     thread; since ABI v35 they serialise against each other on
    ///     midiProducerMutex_ (a SPSC fifo with two writers double-advances the
    ///     index and loses an event). This drain does not take that mutex and
    ///     must not: a consumer-side drain (prepareToRead / finishedRead) is a
    ///     legal concurrent read against a live writer; reset() never was.
    void discardQueuedMidi() noexcept
    {
        // A PLAIN lock_guard on purpose: this runs on the message thread, and
        // DCA_RT_LOCK_GUARD would publish a new site name into the RT gate's
        // observed set the moment a scenario attached an instrument while
        // marked as the audio thread. rt_lock_sites.txt is for audio-thread
        // acquires.
        {
            std::lock_guard<std::mutex> barrier(remoteBridgeMutex_);
            int s1, n1, s2, n2;
            midiFifo_.prepareToRead(kMidiQueueDepth, s1, n1, s2, n2);
            midiFifo_.finishedRead(n1 + n2);
        }
        // ...and the PARKED set too (ABI v34). An event that has already left
        // the fifo and is waiting for its frame is exactly as stale as one
        // still in it, and a scheduled note-on surviving an instrument swap is
        // a note nothing will ever turn off. instrumentMutex_ is the audio
        // thread's barrier for midiPending_, and NEITHER caller holds it here
        // (attach discards before publishing the plugin, detach after its own
        // barrier), so this is a sequential acquire and not a nested one.
        {
            std::lock_guard<std::mutex> barrier(instrumentMutex_);
            for (auto& p : midiPending_) p.packed = 0;
        }
        // ...and the bridge CARRY (W4-N7), for exactly the same reason: an
        // unacknowledged note-on waiting to be re-sent to the child is as stale
        // as one still in the fifo. remoteBridgeMutex_ is the audio thread's
        // barrier for it, and it is taken sequentially above rather than held
        // across the instrument barrier, so take it again here.
        {
            std::lock_guard<std::mutex> barrier(remoteBridgeMutex_);
            clearBridgeMidiLocked();
        }
    }

    /// Caller holds remoteBridgeMutex_. Forget the bridge leg's carry AND its
    /// parked set (bridge v3): a scheduled note-on parked for a child that is
    /// gone, or for an instrument that was swapped, is a note nothing will
    /// ever turn off.
    void clearBridgeMidiLocked() noexcept
    {
        bridgeMidiHeld_ = 0;
        for (auto& p : bridgeMidiPending_) p.packed = 0;
    }

    /// Audio thread, under remoteBridgeMutex_: drain the engine fifo into the
    /// bridge's parked set, then move every event due in THIS block (or
    /// immediate, or late) into the carry buffer behind `held`, with its
    /// sample offset. Returns the new event count. Same frame arithmetic as
    /// renderInstrumentWork: an event whose frame has passed goes at sample 0
    /// and is counted late; one more than 5 s ahead (a rewound render clock)
    /// is dropped; anything still in the future stays parked.
    int collectBridgeMidi(int held, int numSamples) noexcept
    {
        int s1, n1, s2, n2;
        midiFifo_.prepareToRead(kMidiQueueDepth, s1, n1, s2, n2);
        const int total = n1 + n2;
        if (total > 0)
        {
            auto park = [this](const QueuedMidiEvent& ev)
            {
                const size_t n = bridgeMidiPending_.size();
                for (size_t k = 0; k < n; ++k)
                {
                    const size_t i = (bridgeMidiPendingHint_ + k) % n;
                    if (bridgeMidiPending_[i].packed == 0)
                    {
                        bridgeMidiPending_[i]  = ev;
                        bridgeMidiPendingHint_ = (i + 1) % n;
                        return;
                    }
                }
                // Parked set full (>512 in flight): drop and COUNT, as the
                // in-process leg does — a swallowed note-off is a stuck note.
                midiDroppedEvents_.fetch_add(1, std::memory_order_relaxed);
            };
            for (int i = 0; i < n1; ++i) park(midiQueue_[(size_t)(s1 + i)]);
            for (int i = 0; i < n2; ++i) park(midiQueue_[(size_t)(s2 + i)]);
            midiFifo_.finishedRead(total);
        }

        const int64_t blockStart   = bridgeBlockStartFrame_;
        const double  sr           = deviceSr_ > 0.0 ? deviceSr_ : 44100.0;
        const int64_t futureFrames = (int64_t) (sr * 5.0);
        const int     cap          = (int) bridgeMidiScratch_.size();
        int count = held;
        for (auto& p : bridgeMidiPending_)
        {
            if (count >= cap) break;   // carry full: the rest stays parked (and turns late)
            if (p.packed == 0) continue;
            uint32_t offset = 0;
            if (p.frame >= 0)
            {
                const int64_t delta = p.frame - blockStart;
                if (delta >= (int64_t) numSamples)
                {
                    if (delta > futureFrames) p.packed = 0;
                    continue;
                }
                if (delta < 0) midiLateEvents_.fetch_add(1, std::memory_order_relaxed);
                else           offset = (uint32_t) delta;
            }
            bridgeMidiScratch_[(size_t) count] = p.packed;
            bridgeMidiOffsets_[(size_t) count] = offset;
            ++count;
            p.packed = 0;
        }
        return count;
    }

    /// Audio thread: one bridge round trip for this callback. `buf` carries
    /// the gained live input (or silence); on success with an active remote
    /// chain it is replaced by the child's processed output.
    void processRemoteBridge(juce::AudioBuffer<float>& buf, int numSamples) noexcept
    {
        if (! remoteBridge_.isEnabled()) return;
        DCA_RT_LOCK_GUARD(remoteBridgeMutex_, "remoteBridgeMutex/process");
        if (! remoteBridge_.isEnabled()) return;
        processRemoteBridgeLocked(buf, numSamples);
    }

    void processRemoteBridgeLocked(juce::AudioBuffer<float>& buf, int numSamples) noexcept
    {
        bridgeRanThisBlock_ = true;
        // W4-N7. The drain CONSUMES the engine fifo, and the bridge writes the
        // events into a shared mapping the child may never read: on a timeout
        // the request is abandoned and the NEXT block overwrites h->midi
        // unconditionally, so the events are gone from both ends with nothing
        // counting them. A lost note-off is a stuck note.
        //
        // So the scratch is now a CARRY buffer, not a per-block scratch:
        // bridgeMidiHeld_ events at its head are ones the child has not
        // acknowledged, this block's drain APPENDS behind them, and only a
        // successful round trip clears the carry.
        //
        // THE CARRY CANNOT OVERFLOW, and this comment claimed it could until
        // W5-E6: the drain is capped at `room`, i.e. at exactly the space left,
        // so `midiCount <= size` always and the "lost" arithmetic below is
        // dead by construction. It is kept as a cheap assertion of that bound
        // rather than deleted, because the bound is the whole reason the carry
        // is safe. The REAL back-pressure is `room == 0`: with the carry full
        // the engine fifo simply stops being drained and fills, and the drop is
        // then accounted on the PRODUCER side (dca_engine_get_midi_timing_stats'
        // `dropped`), not here. That is where a diagnostician should look.
        const int held = bridgeMidiHeld_;
        const int room = (int) bridgeMidiScratch_.size() - held;
        int midiCount  = held;
        // The in-proc instrument owns the MIDI fifo when attached; otherwise
        // forward queued shorts to the sandbox child.
        //
        // Bridge v3 (JuceSharp #28): the drain PARKS scheduled events, and only
        // the ones due in this block join the carry, each with its sample
        // offset — so a sandboxed instrument is as sample-accurate as the
        // in-process one.
        if (instrumentPlugin_.load(std::memory_order_acquire) == nullptr && room > 0)
            midiCount = collectBridgeMidi(held, numSamples);
        bool chainActive = false, instrActive = false;
        const bool answered = remoteBridge_.process(buf, numSamples,
                                                    bridgeMidiScratch_.data(), bridgeMidiOffsets_.data(),
                                                    midiCount, bridgeBlockStartFrame_,
                                                    &bridgeInstrL_, &bridgeInstrR_,
                                                    chainActive, instrActive);
        if (answered)
        {
            bridgeMidiHeld_ = 0;
        }
        else
        {
            // Carry what fits; anything past the buffer is lost for real and is
            // reported as such through dca_engine_get_midi_timing_stats.
            bridgeMidiHeld_ = std::min(midiCount, (int) bridgeMidiScratch_.size());
            const int lost  = midiCount - bridgeMidiHeld_;
            if (lost > 0) midiDroppedEvents_.fetch_add(lost, std::memory_order_relaxed);
            // A carried event is re-sent with the NEXT block, by which time its
            // frame has passed: sample 0 of that block is the closest it can
            // still land.
            for (int i = 0; i < bridgeMidiHeld_; ++i) bridgeMidiOffsets_[(size_t) i] = 0;
        }
        bridgeInstrumentActive_ = instrActive;
    }

    /// Audio thread: mix the sandbox child's instrument output into `out`.
    /// Runs a zero-input round trip first when no input block ran this
    /// callback (input disabled) so MIDI still reaches the child.
    void renderRemoteInstrument(float* const* out, int numChannels, int numSamples) noexcept
    {
        if (! remoteBridge_.isEnabled()) return;
        if (numChannels <= 0 || numSamples <= 0 || out == nullptr) return;
        DCA_RT_LOCK_GUARD(remoteBridgeMutex_, "remoteBridgeMutex/instrument");
        if (! remoteBridge_.isEnabled()) return;
        try
        {
            if (! bridgeRanThisBlock_)
            {
                // Never allocate here — this is the audio callback. The buffer
                // is sized in audioDeviceAboutToStart. If the host somehow
                // hands us a block bigger than it declared, skip the zero-input
                // round trip for this callback rather than calling setSize()
                // (a heap allocation, and a lock, on the real-time thread).
                if (bridgeSilentBuf_.getNumSamples() < numSamples
                    || bridgeSilentBuf_.getNumChannels() < dcvb::kMaxChannels)
                    return;
                juce::AudioBuffer<float> sub(bridgeSilentBuf_.getArrayOfWritePointers(),
                                             dcvb::kMaxChannels, 0, numSamples);
                sub.clear();
                processRemoteBridgeLocked(sub, numSamples);
            }
            if (! bridgeInstrumentActive_ || bridgeInstrL_ == nullptr) return;
            // RAMP, do not step (W4-N3) — the same rule and the same helper as
            // the monitor mix. dca_engine_set_instrument_gain is a user-facing
            // fader, so it publishes once per UI tick and one constant per block
            // is a zipper; and `|| prev > 0` is what the old `if (g <= 0) return`
            // was missing — skipping the mix outright on the block that ARRIVES
            // at zero is a full-amplitude cut in one sample, a CLICK, and muting
            // the instrument is the one gesture guaranteed to produce it.
            const float g    = instrumentGain_.load(std::memory_order_relaxed);
            const float prev = instrumentGainCurrent_;
            if (g <= 0.0f && prev <= 0.0f) return;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* dst = out[ch];
                if (dst == nullptr) continue;
                const float* src = (ch == 0 || bridgeInstrR_ == nullptr) ? bridgeInstrL_ : bridgeInstrR_;
                rampOrScaleAdd(dst, src, numSamples, prev, g);
            }
            // AFTER the channel loop: every channel must ramp over the SAME
            // from/to pair, or the image shifts while the fader moves.
            instrumentGainCurrent_ = g;
        }
        catch (...) { /* audio thread must never propagate */ }
    }

    // --- Callback timing instrumentation ---------------------------------
    // Real-time-safe diagnostic counters updated from the audio thread.
    // recordCallbackTiming() never blocks, never allocates and never logs;
    // it only updates atomics. A non-RT thread can read & reset them.
    std::atomic<int64_t>  maxCallbackNs_   { 0 };
    std::atomic<uint64_t> callbackCount_   { 0 };
    std::atomic<uint64_t> overrunCount_    { 0 };  // near miss: >80% of budget, INSIDE it

    // --- Real dropout / xrun counters (see dca_xrun_stats) -----------------
    // `overrunCount_` above only ever said "this callback used most of its
    // budget"; it could not tell an audio-callback overrun from the callback
    // simply never arriving, which is the one thing a user actually hears. The
    // counters below split that apart. All relaxed atomics — the audio thread
    // must not allocate, lock, log or syscall, and none of these does.
    std::atomic<int64_t>  lastCallbackNs_    { 0 };
    std::atomic<int64_t>  blockBudgetNs_     { 0 };
    std::atomic<int64_t>  lateCallbacks_     { 0 };   // the callback ARRIVED late — a real dropout
    std::atomic<int64_t>  deadlineOverruns_  { 0 };   // WE overran the block we were rendering
    std::atomic<int64_t>  droppedFrames_     { 0 };   // estimated frames the device filled without us
    std::atomic<int64_t>  lastXrunHostTicks_ { 0 };   // QPC ticks of the most recent of either
    std::atomic<int64_t>  lastXrunFrame_     { -1 };  // render-clock frame at that moment
    std::atomic<int32_t>  deviceXrunBase_    { 0 };   // driver counter at the last reset

    // --- Callback LOAD (ABI v46, JuceSharp #29) ----------------------------
    // What the rig's CPU meter shows: callback cost as a fraction of the block
    // it was rendering (1.0 = the whole buffer period; above 1.0 is a deadline
    // overrun). Two numbers, both computed HERE on the audio thread so a read
    // is two relaxed loads and any number of pollers can read without
    // disturbing each other (a reset-on-read peak would let the health logger
    // steal the UI meter's peak):
    //   average  exponentially weighted, time constant kLoadAverageTauSec
    //            (per-block coefficient from the block's own duration, so a
    //            variable-block driver weighs time, not callbacks);
    //   peak     the largest block load, decaying exponentially with
    //            kLoadPeakDecayTauSec — a hold a 4 Hz poller cannot miss.
    // Both reset to 0 at a device session start and when the device stops, and
    // the warm-up blocks of a session are not counted (a cold device thread
    // would otherwise paint a phantom spike at every rebuild).
    // loadAverage_/loadPeak_ are AUDIO-THREAD-ONLY (written by the device thread
    // only in resetXrunSession, ordered before the first callback, exactly like
    // the gap detector). The published floats are what readers see.
    double              loadAverage_ = 0.0;
    double              loadPeak_    = 0.0;
    std::atomic<float>  loadAveragePub_ { 0.0f };
    std::atomic<float>  loadPeakPub_    { 0.0f };
    static_assert(std::atomic<float>::is_always_lock_free,
                  "the callback-load meter must be lock-free on the audio thread");
    static constexpr double kLoadAverageTauSec   = 0.5;
    static constexpr double kLoadPeakDecayTauSec = 1.5;

    /// Tick->nanosecond conversion, fixed for the life of the process and read
    /// on every callback. A member, never a function-local static: see
    /// recordCallbackTiming.
    const double perTick_ = 1.0e9 / (double) juce::Time::getHighResolutionTicksPerSecond();

    /// The open device, published on the DEVICE thread and read ONLY from the
    /// audio thread — which by construction cannot run before
    /// audioDeviceAboutToStart or after audioDeviceStopped, so unlike
    /// `getCurrentAudioDevice()` this pointer cannot be stale where it is used.
    /// It exists for one thing: sampling the driver's own xrun counter, which
    /// has no value form to cache.
    std::atomic<juce::AudioIODevice*> audioThreadDevice_ { nullptr };
    /// The driver's raw counter, sampled from the audio thread every
    /// kXrunDriverSamplePeriod callbacks. -1 means "this driver cannot report"
    /// (WASAPI, the CoreAudio aggregate path) and must never be surfaced as 0.
    std::atomic<int32_t>  deviceXrunRaw_     { -1 };
    static constexpr uint64_t kXrunDriverSamplePeriod = 128;

    // Audio-thread-ONLY state for the gap detector. Written by the device
    // thread in audioDeviceAboutToStart (which is ordered before the first
    // callback of the session) and by the audio thread afterwards — never
    // concurrently, so no atomics are needed or wanted here.
    int64_t xrunPrevStartTicks_ = 0;
    int     xrunPrevFrames_     = 0;
    int     xrunWarmupBlocks_   = 0;

    /// Blocks ignored at the head of a device session. A cold device thread is
    /// ALWAYS late on its first callbacks (thread spin-up, cold caches, the
    /// driver still settling), so counting them would report a phantom dropout
    /// at every device change, sample-rate change and watchdog recovery — the
    /// exact false positive that made a managed-side heuristic worthless.
    static constexpr int kXrunWarmupBlocks = 4;

    /// Reset the gap detector for a fresh device session. Device thread only,
    /// before the first callback (audioDeviceAboutToStart).
    void resetXrunSession() noexcept
    {
        xrunPrevStartTicks_ = 0;
        xrunPrevFrames_     = 0;
        xrunWarmupBlocks_   = kXrunWarmupBlocks;
        loadAverage_        = 0.0;
        loadPeak_           = 0.0;
        loadAveragePub_.store(0.0f, std::memory_order_relaxed);
        loadPeakPub_   .store(0.0f, std::memory_order_relaxed);
        // The driver's own xrun counter restarts at 0 for a new device.
        deviceXrunBase_.store(0, std::memory_order_relaxed);
    }

    void recordCallbackTiming(int64_t startTicks, int numSamples) noexcept
    {
        // p3-39: the tick->ns conversion is a hardware-frequency constant, and
        // calling juce::Time::getHighResolutionTicksPerSecond() per callback is
        // a QueryPerformanceFrequency SYSCALL. It used to be cached in a
        // FUNCTION-LOCAL STATIC, which is worse than it looks: a magic static
        // is initialised under a compiler-generated guard — an atomic load on
        // every call and a LOCK plus a syscall on the first one, which is the
        // first audio callback of the process. `perTick_` is a member,
        // initialised in the constructor, on the thread that built the engine.
        // (RemoteVstBridge's ctor reads QueryPerformanceFrequency the same way,
        // for the same reason.)
        const double perTick = perTick_;
        const int64_t endTicks   = juce::Time::getHighResolutionTicks();
        const int64_t deltaTicks = endTicks - startTicks;
        const int64_t deltaNs    = (int64_t) ((double) deltaTicks * perTick);

        // Track max-ever callback duration. Compare-exchange loop instead of
        // atomic::fetch_max so we stay portable to older standard libs.
        int64_t prev = maxCallbackNs_.load(std::memory_order_relaxed);
        while (deltaNs > prev &&
               ! maxCallbackNs_.compare_exchange_weak(prev, deltaNs,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {}

        const uint64_t seen = callbackCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        lastCallbackNs_.store(deltaNs, std::memory_order_relaxed);

        // Sample the DRIVER's own xrun counter here rather than from the
        // polling thread, which had to dereference getCurrentAudioDevice() —
        // a raw pointer setAudioDeviceSetup deletes. This thread cannot run
        // outside the device session, so the pointer is live by construction.
        // Every 128 callbacks (~0.34 s at 256/48k): it is a plain member read
        // in every JUCE backend, and the number only has to be right for a
        // health poll.
        if (seen % kXrunDriverSamplePeriod == 0)
            if (auto* dev = audioThreadDevice_.load(std::memory_order_acquire))
                deviceXrunRaw_.store((int32_t) dev->getXRunCount(), std::memory_order_relaxed);

        // Block budget = numSamples / sampleRate seconds.
        const double sr = deviceSr_ > 0.0 ? deviceSr_ : 44100.0;
        const int64_t budgetNs = (int64_t) (1.0e9 * (double) numSamples / sr);
        blockBudgetNs_.store(budgetNs, std::memory_order_relaxed);
        if (budgetNs > 0)
        {
            if (deltaNs > budgetNs)
            {
                // We did not finish inside the block we were rendering. This is
                // OUR fault, and it is what usually makes the NEXT callback late.
                deadlineOverruns_.fetch_add(1, std::memory_order_relaxed);
                lastXrunHostTicks_.store(endTicks, std::memory_order_relaxed);
                lastXrunFrame_    .store(renderFrames_, std::memory_order_relaxed);
            }
            else if (deltaNs > (budgetNs * 4) / 5)
            {
                // Near miss: loud enough to log, not a dropout. Disjoint from
                // the branch above ON PURPOSE — "over 80%" that also included
                // every real overrun is the shape of number that reads as a
                // problem when it is only a warning, and vice versa.
                overrunCount_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // --- Callback load (see loadAverage_) ------------------------------
        // Before the detector below, which consumes the warm-up count. std::exp
        // is a pure libm call: no allocation, no lock, no syscall.
        if (budgetNs > 0 && xrunWarmupBlocks_ == 0)
        {
            const double load     = (double) deltaNs / (double) budgetNs;
            const double blockSec = (double) budgetNs * 1.0e-9;
            const double alpha    = 1.0 - std::exp(-blockSec / kLoadAverageTauSec);
            loadAverage_ += alpha * (load - loadAverage_);
            const double decayed  = loadPeak_ * std::exp(-blockSec / kLoadPeakDecayTauSec);
            loadPeak_ = load > decayed ? load : decayed;
            loadAveragePub_.store((float) loadAverage_, std::memory_order_relaxed);
            loadPeakPub_   .store((float) loadPeak_,    std::memory_order_relaxed);
        }

        // --- The dropout detector -----------------------------------------
        // The gap between the START of this callback and the START of the
        // previous one should be the PREVIOUS block's own duration (a
        // variable-block driver is why it is the previous block's size, not
        // this one's). A gap materially larger than that means the device went
        // round at least once without us: it filled that buffer with silence or
        // with the stale one, and the user heard it. This is the only signal
        // available at all on WASAPI/CoreAudio, whose drivers report no xrun
        // count of their own.
        if (xrunWarmupBlocks_ > 0)
        {
            --xrunWarmupBlocks_;
        }
        else if (xrunPrevStartTicks_ != 0 && xrunPrevFrames_ > 0)
        {
            const int64_t gapNs      = (int64_t) ((double) (startTicks - xrunPrevStartTicks_) * perTick);
            const int64_t expectedNs = (int64_t) (1.0e9 * (double) xrunPrevFrames_ / sr);
            // 1.5x the period plus a 0.5 ms absolute floor. The floor is what
            // keeps a 64-frame buffer (1.33 ms at 48 kHz, where ordinary
            // scheduler jitter is a large fraction of a period) from crying
            // wolf, while 1.5x still catches a SINGLE missed period at the
            // 256-1024 frame sizes people actually run.
            const int64_t limitNs = expectedNs + expectedNs / 2 + 500000;
            if (expectedNs > 0 && gapNs > limitNs)
            {
                lateCallbacks_.fetch_add(1, std::memory_order_relaxed);
                const int64_t lost = (int64_t) (((double) (gapNs - expectedNs) * 1.0e-9) * sr);
                if (lost > 0) droppedFrames_.fetch_add(lost, std::memory_order_relaxed);
                lastXrunHostTicks_.store(startTicks, std::memory_order_relaxed);
                lastXrunFrame_    .store(renderFrames_, std::memory_order_relaxed);
            }
        }
        xrunPrevStartTicks_ = startTicks;
        xrunPrevFrames_     = numSamples;
    }

public:
    /// Snapshot the dropout / xrun counters and the device's health. Call from
    /// ANY non-real-time thread; it only reads atomics plus (once) the current
    /// device, and never touches the audio thread.
    void fillXrunStats(dca_xrun_stats& s) noexcept;

    /// Zero every "since reset" counter and rebase the driver's own count.
    void resetXrunStats() noexcept;

    /// ABI v46: the callback load (see loadAverage_). Two relaxed loads; any
    /// thread, any number of readers, never touches the audio thread.
    void getCallbackLoad(float& average, float& peak) const noexcept
    {
        average = loadAveragePub_.load(std::memory_order_relaxed);
        peak    = loadPeakPub_   .load(std::memory_order_relaxed);
    }

    /// Diagnostic counters. Safe to call from any thread. Each call returns
    /// the value since the last reset (the caller can choose to reset by
    /// passing reset=true). Used by managed-side health logging.
    int64_t  peekMaxCallbackNs(bool reset = false) noexcept
    {
        return reset ? maxCallbackNs_.exchange(0, std::memory_order_acq_rel)
                     : maxCallbackNs_.load(std::memory_order_acquire);
    }
    uint64_t peekCallbackCount(bool reset = false) noexcept
    {
        return reset ? callbackCount_.exchange(0, std::memory_order_acq_rel)
                     : callbackCount_.load(std::memory_order_acquire);
    }
    uint64_t peekOverrunCount(bool reset = false) noexcept
    {
        return reset ? overrunCount_.exchange(0, std::memory_order_acq_rel)
                     : overrunCount_.load(std::memory_order_acquire);
    }

private:

    // --- Streaming mirror (parallel WASAPI output). Push-only from the
    // primary audio callback; pulled by its own AudioDeviceManager.
    MirrorOutput                                  mirror_;

    // The mirror device the CALLER asked for, remembered across a rebuild.
    // stop() closes the mirror and nothing else in the process holds the name,
    // so without this a stop()/start() cycle ends the OBS / Discord stream
    // permanently and silently. Message thread only (setMirrorDevice / start).
    juce::String                                  requestedMirrorDevice_;
    /// Re-open the remembered mirror after a successful start(). Never fails
    /// the start: a mirror device that has gone away is logged and skipped.
    void restoreMirrorAfterStart();

    // --- Click bus -------------------------------------------------------
    // Lock-free trigger of synthesised clicks (metronome tick / measure tick
    // / clap). The audio thread holds up to kClickVoices simultaneously
    // sounding voices; triggers beyond the capacity reuse the oldest slot.
    static constexpr int kClickVoices = 8;
    struct ClickVoice
    {
        int   type     = -1; // -1 = idle, 0 = tick, 1 = measure, 2 = clap
        int   played   = 0;
        int   total    = 0;
        float phase    = 0.0f;
        uint32_t rng   = 0x12345u;
        // Samples to skip at the head of the CURRENT block before this voice
        // sounds. Non-zero only for a scheduled click whose frame lands inside
        // the block (that offset is the whole point of scheduling); consumed by
        // the first block the voice renders into.
        int   startOffset = 0;
    };
    std::array<ClickVoice, kClickVoices> clickVoices_{};
    juce::AbstractFifo                   clickFifo_{ 64 };
    std::array<int, 64>                  clickQueue_{};
    std::atomic<float>                   clickGain_{ 1.0f };

    // --- Scheduled clicks (ABI v26) --------------------------------------
    // {type, absolute render frame} pushed by any thread, drained by the audio
    // thread into clickPending_ and fired at a sample offset inside the block
    // that contains the frame. kClickClearSentinel travels through the SAME
    // FIFO as an ordinary entry so a clear issued after a schedule reliably
    // cancels it (no separate atomic flag to race with).
    static constexpr int   kClickScheduleCapacity = 128;
    static constexpr int   kPendingClicks         = 32;
    static constexpr int   kClickClearSentinel    = -2;
    struct ScheduledClick
    {
        int     type  = -1;  // -1 = empty slot, -2 = clear-all sentinel
        int64_t frame = 0;
    };
    juce::AbstractFifo                                 clickScheduleFifo_{ kClickScheduleCapacity };
    std::array<ScheduledClick, kClickScheduleCapacity> clickScheduleQueue_{};
    std::array<ScheduledClick, kPendingClicks>         clickPending_{};   // audio thread only

    // --- Output render clock (ABI v26) ------------------------------------
    // renderFrames_ counts every frame the device callback has rendered since
    // the device started; it IS the timeline scheduled clicks are placed on.
    // Published seqlock-style (same pattern as the capture sync above) right
    // after the transport pull, so a managed reader can sample it together with
    // a source's audible position and get a torn-free {frame, song time} pair.
    // Reset to 0 on every device (re)start — callers re-read it per schedule.
    std::atomic<uint32_t>     renderSyncSeq_{0};
    int64_t                   renderFrames_    = 0;  // audio/device thread writer
    int64_t                   renderSyncTicks_ = 0;

    void publishRenderClock(int64_t frames) noexcept
    {
        renderSyncSeq_.fetch_add(1, std::memory_order_acq_rel);
        renderFrames_    = frames;
        renderSyncTicks_ = juce::Time::getHighResolutionTicks();
        renderSyncSeq_.fetch_add(1, std::memory_order_release);
    }

    /// Device (re)start: rewind the render clock and drop the audio thread's
    /// pending clicks, whose frames refer to the previous clock. Called from
    /// audioDeviceAboutToStart, i.e. with no callback in flight.
    void resetRenderClock() noexcept
    {
        renderSyncSeq_.store(0, std::memory_order_release);
        renderFrames_    = 0;
        renderSyncTicks_ = 0;
        for (auto& p : clickPending_) p.type = -1;
    }

    /// Start a click voice, optionally at a sample offset into the current
    /// block. Picks the first idle voice, else steals the most-advanced
    /// (oldest) one, so a fast burst still fires. Audio thread only.
    void startClickVoice(int type, int offsetInBlock) noexcept;

    void mixClickBus(float* const* out, int numChannels, int numSamples, int64_t blockStartFrame);
    void renderInstrumentBus(float* const* out, int numChannels, int numSamples,
                             int64_t blockStartFrame);

    void prepareMasterBus(juce::AudioIODevice* device);

    /// Re-run prepareToPlay on every attached input VST3 and on the instrument
    /// VSTi with the CURRENT device rate / block size. Called from
    /// prepareMasterBus (device thread, no callback in flight); see the
    /// definition for why no barrier is needed.
    void reprepareAttachedPlugins();
    void processMasterBus(float* const* out, int numChannels, int numSamples);

    /// Returns true when the seat has at least one input plugin (whether or
    /// not it ran cleanly) — idle sleep never rests a seat whose rig output
    /// feeds a third-party stage it cannot reason about.
    bool processInputChain(int player, juce::AudioBuffer<float>& buf, int numSamples)
    {
        // Audio thread. The snapshot's refcount keeps the *vector* alive,
        // but NOT the AudioPluginInstance pointees — detach + unload frees
        // an instance while a block that loaded the old snapshot could still
        // be inside its processBlock (the intermittent unload crash). This
        // mutex is the same wait-one-block barrier the instrument bus uses:
        // uncontended on every normal block; detachInputPlugin acquires it
        // once after publishing the new snapshot, which waits out any
        // in-flight block before the caller may destroy the instance. Take
        // it BEFORE loading the snapshot, or the barrier could slip in
        // between our load and the processBlock call.
        if (! validPlayer(player)) return false;
        DCA_RT_LOCK_GUARD(inputChainProcessMutex_[(size_t) player], "inputChainProcessMutex");
        auto snap = std::atomic_load_explicit(&inputChain_[(size_t) player], std::memory_order_acquire);
        if (snap == nullptr || snap->empty()) return false;
        emptyMidi_.clear();
        // kChainChannels, NOT buf.getNumChannels(): chainBuffer_ is sized for
        // jmax(2, masterBusWidth_) so it can carry more than two channels on a
        // multi-out device, but the callback only ever FILLS channels 0..1 and
        // attachInputPlugin prepares every plugin with a STEREO layout. Handing
        // processBlock the wider buffer ran the chain over uninitialised
        // channels and stepped outside the layout the plugin was prepared for.
        const int chainCh = juce::jmin(buf.getNumChannels(), kChainChannels);
        if (chainCh <= 0) return true;
        juce::AudioBuffer<float> sub(buf.getArrayOfWritePointers(), chainCh, 0, numSamples);

        // Drop the faulted set whenever the chain was republished — see the
        // members' comment for why this is a GENERATION and not the snapshot
        // pointer.
        const size_t pl = (size_t) player;
        const uint64_t gen = inputChainGeneration_.load(std::memory_order_acquire);
        if (inputChainFaultGen_[pl] != gen)
        {
            inputChainFaultGen_[pl] = gen;
            inputChainFaulted_[pl]  = 0u;
        }

        const size_t count = snap->size();
        for (size_t idx = 0; idx < count; ++idx)
        {
            auto* p = (*snap)[idx];
            if (p == nullptr) continue;
            const std::uint32_t bit = idx < 32u ? (1u << idx) : 0u;
            // Already threw on an earlier block of this snapshot: skip it, do
            // NOT break. The rest of the chain is intact and the user keeps the
            // pedals after the broken one.
            if (bit != 0u && (inputChainFaulted_[pl] & bit) != 0u) continue;
            // p2-21 / W4-N1: a THIRD-PARTY plugin's processBlock, on the audio
            // thread. An exception out of it unwinds through JUCE's driver
            // callback frame with nothing to catch it — process termination,
            // not a dropped block. The engine's other two plugin call sites
            // (renderInstrumentBus, reprepareAttachedPlugins) have always
            // wrapped theirs; the INPUT chain — the path every amp-sim user is
            // on — was the one that did not.
            //
            // BREAK, not continue: a chain whose stage N threw has left `sub`
            // in whatever state that stage got to, and running stage N+1 over
            // half-processed audio is a second fault on top of the first.
            try { p->processBlock(sub, emptyMidi_); }
            catch (...)
            {
                // Mark it failed BEFORE the break, so the next block skips it
                // instead of paying for another throw (W5-E7). Both statements
                // are RT-safe: a bit-or into audio-thread-only state and one
                // relaxed fetch_add. No logging here — the catch body stays
                // allocation- and syscall-free; the COUNTER is the report, and
                // a host polling dca_engine_get_xrun_stats sees it.
                if (bit != 0u) inputChainFaulted_[pl] |= bit;
                inputPluginFaults_.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        return true;
    }

    // ---- In-house DSP tone rig (0.16 Phase 0) ------------------------------
    // Runs the reorderable in-house effect chain over the gained input BEFORE
    // the user's VST input chain (our rig first, external plugins as the escape
    // hatch after). RT-safe: DspChain loads an atomic ordering snapshot. Called
    // from the nested audio source (friend access), so it can stay non-public.
    /// `songMix` / `songChannels` (ABI v34, the SIDECHAIN feature) are the
    /// engine's own output block for THIS callback, handed to the rig
    /// read-only so a node keyed to the song mix can duck under the backing
    /// track. processMasterBus has already run by the time this is called, so
    /// it holds the song; for seat p it also holds the monitor of every seat
    /// before p, which is the honest cheap answer and is documented as such in
    /// dotcharter_audio.h. Null is a valid argument and means "that source is
    /// unavailable", which the chain falls back from.
    /// `noexcept` like every other audio-thread helper in this header — it was
    /// the only one that was not. The rig is entirely this project's own code
    /// and cannot throw, so this documents the contract rather than changing
    /// behaviour; the third-party call sites are the ones that catch.
    void processDspRig(int player, juce::AudioBuffer<float>& buf, int numSamples,
                       const float* const* songMix = nullptr, int songChannels = 0) noexcept
    {
        if (! validPlayer(player)) return;
        auto& rig = dspRigs_[(size_t) player];
        if (rig.nodeCountRt() == 0) return; // cheap early-out (real atomic — never a lock on the audio thread)
        // kChainChannels, NOT buf.getNumChannels(): prepareMasterBus prepares the
        // rig for exactly 2 channels while chainBuffer_ may be wider (it is sized
        // for the DEVICE's output count), and only channels 0..1 carry input. The
        // extra channels held stale data, every core clamped them back to its own
        // 2-channel state anyway, and DspChain's between-stage NaN scrub walked
        // them on every block for nothing.
        const int chainCh = juce::jmin(buf.getNumChannels(), kChainChannels);
        if (chainCh <= 0) return;
        rig.process(buf.getArrayOfWritePointers(), chainCh, numSamples, songMix, songChannels);
    }

    /// The DEVICE callback's rig pass: processDspRig plus IDLE SLEEP
    /// (dsp/IdleSleep.h). When the seat's rig has provably been producing
    /// nothing audible — what it contributed to the device output stayed below
    /// -90 dBFS for the chain's hold, over a stationary idle input — the rig is
    /// SKIPPED and its output channels zeroed, which is what it was outputting
    /// to within -90 dBFS. The block whose input rises above the idle floor
    /// wakes it and IS processed, so no attack is lost; so does any change in
    /// `ctx` (a chain edit or param write, the trim, the monitor, the input EQ,
    /// an input-VST attach, the bridge) or `forceAwake` (an EQ retune).
    ///
    /// Sleeping is EXACTLY "the idle stretch was cut out of this seat's
    /// input": the chain's state only ever advances inside process(), so a
    /// skipped block leaves every module where the last awake block left it,
    /// and the waking block continues from there. Resuming — not resetting —
    /// is what makes that hold: after the hold every allow-listed module's
    /// state is a function of the idle input alone (IdleSleep.h), so it is the
    /// state a never-slept rig is in too, bit for bit on digital silence and up
    /// to a different realization of the same noise otherwise. A reset would
    /// hand the pick a cold state no never-slept rig is ever in.
    ///
    /// RT: a peak scan, a POD compare and at most a clear — no allocation, no
    /// lock (DspChain::process keeps its own), no logging. Never called by the
    /// offline renders, which are exact by construction.
    void processDspRigLive(int player, juce::AudioBuffer<float>& buf, int numSamples,
                           const float* const* songMix, int songChannels,
                           const dcrig::RigIdleContext& ctx, bool forceAwake) noexcept
    {
        if (! validPlayer(player)) return;
        auto& rig  = dspRigs_[(size_t) player];
        auto& idle = rigIdle_[(size_t) player];
        if (rig.nodeCountRt() == 0) return;
        const int chainCh = juce::jmin(buf.getNumChannels(), kChainChannels);
        if (chainCh <= 0) return;
        if (! rigIdleEnabled_.load(std::memory_order_relaxed))
        {
            idle.reset();
        }
        else
        {
            const float inPeak = dcrig::blockPeak(buf.getArrayOfReadPointers(), chainCh, numSamples);
            if (idle.beginBlock(ctx, inPeak, forceAwake))
            {
                for (int c = 0; c < chainCh; ++c)
                    juce::FloatVectorOperations::clear(buf.getWritePointer(c), numSamples);
                return;
            }
        }
        rig.process(buf.getArrayOfWritePointers(), chainCh, numSamples, songMix, songChannels);
    }

    /// End of the seat's block in the device callback. `heardPeak` is what the
    /// rig contributed to the device output — the post-EQ peak times the
    /// monitor gain, never counted below unity so the seat's post-chain meter
    /// and every other consumer of chainBuffer_ stay honest with monitoring
    /// off. `downstreamOpaque` is true when a third-party stage (an input VST,
    /// the sandbox bridge) ran on the rig's output this block.
    void rigIdleEndBlock(int player, float heardPeak, int numSamples, bool downstreamOpaque) noexcept
    {
        if (! validPlayer(player)) return;
        auto& rig = dspRigs_[(size_t) player];
        rigIdle_[(size_t) player].endBlock(heardPeak, numSamples, rig.rtIdleHoldSamples(),
                                           ! downstreamOpaque && rig.rtMayRest());
    }

public:
    // Message-thread control surface (behind the C ABI). All thread-safe.
    //
    // ABI v28: CHAIN-scoped ops take a player index; NODE-scoped ops do not,
    // because a node id already carries its player (kPlayerNodeIdStride).
    // A node id belonging to a player that does not exist resolves to nothing
    // and every op fails — it is never clamped onto another player's rig.
    int  dspAddNode(int player, const char* typeId, bool initiallyBypassed = false)
    {
        if (! validPlayer(player)) return -1;
        auto mod = dcrig::createModule(typeId);
        if (mod == nullptr) return -1;
        const int local = dspRigs_[(size_t) player].addNode(std::move(mod), initiallyBypassed);
        if (local < 0) return -1;
        if (local >= kPlayerNodeIdStride)
        {
            // Astronomically unreachable (a million adds on one seat), but an
            // overflow here would silently alias another player's node ids.
            dspRigs_[(size_t) player].removeNode(local);
            return -1;
        }
        return player * kPlayerNodeIdStride + local;
    }
    bool dspRemoveNode(int nodeId) { int l; auto* r = rigOfNode(nodeId, l); return r && r->removeNode(l); }
    bool dspMoveNode(int nodeId, int newIndex) { int l; auto* r = rigOfNode(nodeId, l); return r && r->moveNode(l, newIndex); }
    bool dspSetBypass(int nodeId, bool bypassed) { int l; auto* r = rigOfNode(nodeId, l); return r && r->setBypass(l, bypassed); }
    bool dspSetNodeBranch(int nodeId, int branch) { int l; auto* r = rigOfNode(nodeId, l); return r && r->setNodeBranch(l, branch); }
    bool dspGetNodeBranch(int nodeId, int& out) const { int l; auto* r = rigOfNode(nodeId, l); return r && r->getNodeBranch(l, out); }
    // ABI v34 sidechain. Node-scoped, like every other per-node setter.
    bool dspSetNodeSidechain(int nodeId, int source) { int l; auto* r = rigOfNode(nodeId, l); return r && r->setNodeSidechain(l, source); }
    bool dspGetNodeSidechain(int nodeId, int& out) const { int l; auto* r = rigOfNode(nodeId, l); return r && r->getNodeSidechain(l, out); }
    // ABI v33 introspection. Node-scoped, so no player argument: rigOfNode
    // resolves the seat from the id and REJECTS one past kMaxPlayers.
    bool dspGetNodeTypeId(int nodeId, std::string& out) const { int l; auto* r = rigOfNode(nodeId, l); return r && r->getNodeTypeId(l, out); }
    bool dspGetBypass(int nodeId, bool& out) const { int l; auto* r = rigOfNode(nodeId, l); return r && r->getBypass(l, out); }
    bool dspSetParam(int nodeId, const char* paramId, float v) { int l; auto* r = rigOfNode(nodeId, l); return r && r->setParam(l, paramId, v); }
    bool dspGetParam(int nodeId, const char* paramId, float& out) const { int l; auto* r = rigOfNode(nodeId, l); return r && r->getParam(l, paramId, out); }
    bool dspSetStringParam(int nodeId, const char* paramId, const char* utf8) { int l; auto* r = rigOfNode(nodeId, l); return r && r->setStringParam(l, paramId, utf8); }
    bool dspGetStringParam(int nodeId, const char* paramId, std::string& out) const { int l; auto* r = rigOfNode(nodeId, l); return r && r->getStringParam(l, paramId, out); }
    int  dspNodeCount(int player) const { return validPlayer(player) ? dspRigs_[(size_t) player].nodeCount() : 0; }
    int  dspGetNodeIds(int player, int* out, int capacity) const
    {
        if (! validPlayer(player)) return 0;
        const int total = dspRigs_[(size_t) player].getNodeIds(out, capacity);
        // The chain speaks LOCAL ids; the ABI speaks engine-global ones.
        if (out != nullptr)
        {
            const int written = total < capacity ? total : capacity;
            for (int i = 0; i < written; ++i) out[i] += player * kPlayerNodeIdStride;
        }
        return total;
    }
    void dspClear(int player) { if (validPlayer(player)) dspRigs_[(size_t) player].clear(); }

    // ---- Edit scopes (ABI v34) ---------------------------------------------
    // CHAIN-scoped, so per seat: a scope on seat 0 does not suppress seat 1's
    // publications. An open scope is ended by ~DspChain, so destroying the
    // engine under one is defined rather than a leak of the parked slots.
    bool dspBeginEdit(int player)  { return validPlayer(player) && dspRigs_[(size_t) player].beginEdit(); }
    bool dspCommitEdit(int player) { return validPlayer(player) && dspRigs_[(size_t) player].commitEdit(); }
    bool dspAbortEdit(int player)  { return validPlayer(player) && dspRigs_[(size_t) player].abortEdit(); }
    bool dspEditing(int player) const { return validPlayer(player) && dspRigs_[(size_t) player].editing(); }
    /// Ordering snapshots this seat's rig has published. Test hook for the RT
    /// gate (a preset swap inside a scope must cost a BOUNDED number of them).
    /// Idle sleep test hooks (native tests only; no ABI). `setRigIdleSleepForTest`
    /// is read by the callback at the top of each seat's block.
    void setRigIdleSleepForTest(bool on) noexcept { rigIdleEnabled_.store(on, std::memory_order_relaxed); }
    /// A COPY of the seat's decision state. Safe only while no callback is in
    /// flight — on the manual-pump null device, between pumps.
    dcrig::RigIdleSleep rigIdleStateForTest(int player) const noexcept
    {
        return validPlayer(player) ? rigIdle_[(size_t) player] : dcrig::RigIdleSleep{};
    }

    unsigned long dspPublishCountForTest(int player) const
    {
        return validPlayer(player) ? dspRigs_[(size_t) player].publishCountForTest() : 0;
    }

    // ---- Per-node meters + latency (ABI v30) -------------------------------
    // All message-thread, all POLL-based: the audio thread only ever folds a
    // block into an envelope, and nothing is queued, marshalled or called back.
    void dspSetMetering(int player, bool on)
    {
        if (validPlayer(player)) dspRigs_[(size_t) player].setMetering(on);
    }
    bool dspMetering(int player) const
    {
        return validPlayer(player) && dspRigs_[(size_t) player].metering();
    }
    bool dspGetNodeMeter(int nodeId, float& inPeak, float& inRms,
                         float& outPeak, float& outRms) const
    {
        int l; auto* r = rigOfNode(nodeId, l);
        return r && r->getNodeMeter(l, inPeak, inRms, outPeak, outRms);
    }
    int dspReadMeters(int player, float* out, int capacityFloats) const
    {
        return validPlayer(player) ? dspRigs_[(size_t) player].readMeters(out, capacityFloats) : 0;
    }
    bool dspGetNodeLatency(int nodeId, int& out) const
    {
        int l; auto* r = rigOfNode(nodeId, l);
        return r && r->getNodeLatency(l, out);
    }
    // ---- Branch delay compensation (ABI v34) -------------------------------
    // Chain-scoped, so per seat. ON by default; off restores the pre-v34 sum.
    void dspSetBranchPdc(int player, bool on)
    {
        if (validPlayer(player)) dspRigs_[(size_t) player].setBranchPdc(on);
    }
    bool dspBranchPdc(int player) const
    {
        return validPlayer(player) && dspRigs_[(size_t) player].branchPdc();
    }

    // ---- Spillover (ABI v37) -----------------------------------------------
    // Chain-scoped, so per seat. OFF at engine start; the host opts in.
    void dspSetSpillover(int player, bool on)
    {
        if (validPlayer(player)) dspRigs_[(size_t) player].setSpillover(on);
    }
    bool dspSpillover(int player) const
    {
        return validPlayer(player) && dspRigs_[(size_t) player].spillover();
    }
    /// Ringing tails; retires the finished ones first (message thread).
    int dspSpillTailCount(int player)
    {
        if (! validPlayer(player)) return -1;
        dspRigs_[(size_t) player].reapFinishedSpills();
        return dspRigs_[(size_t) player].spillTailCount();
    }
    /// Tails the seat still HOLDS (ringing or awaiting retirement). RT gate hook.
    int dspSpillSlotCountForTest(int player) const
    {
        return validPlayer(player) ? dspRigs_[(size_t) player].spillSlotCountForTest() : -1;
    }

    int dspChainLatency(int player) const
    {
        return validPlayer(player) ? dspRigs_[(size_t) player].chainLatencySamples() : -1;
    }

    /// Tells the seat's chain, on every way out of an offline pass, that its
    /// modules were advanced off the device callback (DspChain::noteExternalProcess).
    struct OfflinePassNote
    {
        dcrig::DspChain& rig;
        ~OfflinePassNote() { rig.noteExternalProcess(); }
    };

    /// Offline test/tooling path (dca_engine_dsp_process_buffer): run a
    /// stereo buffer through the rig off the device callback, chunked to a
    /// size every prepare() of the rig guarantees (both prepareMasterBus and
    /// the lazy prepare here use maxBlock >= 2048 > kOfflineChunk). The
    /// chain's processMutex_ serializes each chunk against the live callback
    /// and against structure mutations — which is exactly what the DSP soak
    /// test wants to hammer.
    ///
    /// THIS METHOD DOES NOT CHECK FOR AN OPEN DEVICE, and that is deliberate.
    /// The processMutex_ it takes per chunk is the audio callback's own
    /// barrier, so running it against a live callback contends that callback
    /// once per 512 frames for the length of a whole rig pass; the ABI export
    /// therefore returns DCA_ERR_DEVICE when a device is open. The check lives
    /// THERE and not here because the native RT gate drives this method
    /// directly on the MANUAL-PUMP null device — where the callback only runs
    /// when the test thread pumps it — and would go silently vacuous if the
    /// method refused. An in-process caller that owns the callback's schedule
    /// may still use it; a caller across the ABI may not.
    void dspProcessOffline(int player, float* left, float* right, int numFrames, int sampleRate)
    {
        if (! validPlayer(player)) return;
        auto& rig = dspRigs_[(size_t) player];
        // Idle sleep: this pass moves the module state under a seat that may
        // be asleep on the device path, so it must wake and re-earn its hold.
        const OfflinePassNote offlineNote{ rig };
        const double wanted = sampleRate > 0 ? (double) sampleRate : 48000.0;
        // A rig prepared at one rate and rendered at another runs EVERY
        // coefficient at the wrong frequency — a 48 kHz-prepared chain asked
        // for a 44.1 kHz bounce puts every corner 8.8 % sharp and every delay
        // 8.8 % short, silently, for the whole render. The take re-amp drives a
        // dedicated DEVICE-LESS engine, so re-preparing there is free and
        // correct; when a device IS open the device owns the rate and the
        // caller has to match it (re-preparing under a live callback would
        // retune the player's live tone mid-song).
        const bool deviceLive = deviceSnapshot().open;
        const double prepared = rig.preparedSampleRate();
        if (prepared <= 0.0 || (! deviceLive && std::fabs(prepared - wanted) > 1.0e-6))
            rig.prepare(wanted, 2048, kChainChannels);
        // The device callback establishes FTZ/DAZ for the live path; this one
        // has none, so a reverb/delay tail decaying into subnormals costs
        // ~100x per sample for the rest of an offline bounce.
        const juce::ScopedNoDenormals noDenormals;
        static constexpr int kOfflineChunk = 512;
        for (int pos = 0; pos < numFrames; pos += kOfflineChunk)
        {
            const int n = juce::jmin(kOfflineChunk, numFrames - pos);
            float* chans[2] = { left + pos, right + pos };
            rig.process(chans, 2, n);
        }
    }

    /// One automation event, through the SAME entry points the live setters
    /// use. `"bypass"` is the one reserved name: no module declares a parameter
    /// by that id (the chain owns bypass, not the module), so routing it to
    /// setBypass costs nothing and gives automation the footswitch.
    bool applyAutomationEvent(const dca_dsp_automation_event& ev)
    {
        // The param id is a FIXED 32-byte field and a caller may legally fill
        // it without a terminator. Bound the read rather than trusting it.
        char id[sizeof(ev.param) + 1];
        std::memcpy(id, ev.param, sizeof(ev.param));
        id[sizeof(ev.param)] = '\0';
        if (id[0] == '\0') return false;

        if (std::strcmp(id, "bypass") == 0)
            return dspSetBypass(ev.node_id, ev.value != 0.0f);
        return dspSetParam(ev.node_id, id, ev.value);
    }

    /// Could `applyAutomationEvent` apply this event, WITHOUT applying it?
    ///
    /// This is what lets `dca_engine_dsp_process_buffer_automated_for_player`
    /// validate the whole list before a sample is rendered. The BUFFER was
    /// always safe (the write-back came after the render), but the RIG was not:
    /// every event before the failing one had already gone through
    /// `DspChain::setParam` / `setBypass`, so a failed render left the seat's
    /// chain at an arbitrary mid-sweep state with no way for the caller to know
    /// or undo it — while the export's own comment reasoned carefully about
    /// half-processed buffers and not at all about half-mutated state.
    ///
    /// It is a RESOLVE-ONLY probe through the same `rigOfNode` + module lookup
    /// the setter uses, not a second implementation of it.
    ///
    /// `player` is checked too: node ids are engine-global
    /// (`p * DCA_PLAYER_NODE_ID_STRIDE + local`), so an event carrying seat 1's
    /// id submitted to a render of seat 0 used to be applied to SEAT 1's chain.
    /// The `player` parameter read as a scope and was not one.
    bool canApplyAutomationEvent(int player, const dca_dsp_automation_event& ev) const
    {
        // Negative ids truncate toward zero, so -3 / stride is 0 and would pass
        // the seat check for seat 0. Refuse them outright.
        if (ev.node_id < 0 || ev.node_id / DCA_PLAYER_NODE_ID_STRIDE != player) return false;

        char id[sizeof(ev.param) + 1];
        std::memcpy(id, ev.param, sizeof(ev.param));
        id[sizeof(ev.param)] = '\0';
        if (id[0] == '\0') return false;

        int l; auto* r = rigOfNode(ev.node_id, l);
        if (r == nullptr) return false;
        if (std::strcmp(id, "bypass") == 0) return r->hasNode(l);
        return r->hasParam(l, id);
    }

    /// The same offline render, with PARAMETER AUTOMATION (ABI v34).
    ///
    /// `dspProcessOffline` renders a whole take at ONE static setting, which is
    /// the one thing a re-amp is never asked to do: a volume swell, a wah pass
    /// and a footswitch mid-solo are all ordinary. The alternative — the caller
    /// chunking the buffer itself and calling setParam between calls — is the
    /// same loop with a per-chunk re-prepare check and no way to say WHEN,
    /// because the caller cannot see the chunk boundary the engine picks.
    ///
    /// `events` must be sorted by frame, and every one of them goes through
    /// `DspChain::setParam` — the SAME mailbox the live callback's setters use.
    /// That is deliberate and not incidental: it is what makes a NaN rejected
    /// here exactly as it would be live, and it is why an automated render and
    /// a live performance of the same gesture cannot diverge.
    ///
    /// `interleaved` is `channels` samples per frame, in place. Returns the
    /// index of the first event that could not be applied, or -1 on success.
    int dspProcessOfflineAutomated(int player, float* interleaved, int channels,
                                   int numFrames, int sampleRate,
                                   const dca_dsp_automation_event* events, int eventCount)
    {
        if (! validPlayer(player) || interleaved == nullptr) return -1;
        if (channels < 1 || channels > 2 || numFrames <= 0) return -1;

        auto& rig = dspRigs_[(size_t) player];
        const OfflinePassNote offlineNote{ rig };   // idle sleep; see dspProcessOffline
        const double wanted = sampleRate > 0 ? (double) sampleRate : 48000.0;
        const bool   deviceLive = deviceSnapshot().open;
        const double prepared   = rig.preparedSampleRate();
        if (prepared <= 0.0 || (! deviceLive && std::fabs(prepared - wanted) > 1.0e-6))
            rig.prepare(wanted, 2048, kChainChannels);

        // The device callback establishes FTZ/DAZ for the live path; this one
        // has none, and a reverb tail decaying into subnormals costs ~100x per
        // sample for the rest of the render. Same one line as dspProcessOffline.
        const juce::ScopedNoDenormals noDenormals;

        // ONE CHUNK of scratch, not two whole-take buffers. De-interleaving the
        // entire render first cost 2 x numFrames floats on top of the caller's
        // buffer — a 5-minute stereo take at 48 kHz is ~115 MB, allocated on the
        // re-amp path of a machine that is also holding the stems — while
        // `dspProcessOffline`, the non-automated sibling written for the same
        // job, works in place with no copy at all. The chunk boundary already
        // existed; this just writes each chunk back before advancing, which also
        // deletes the whole-take write-back loop that used to follow.
        static constexpr int kOfflineChunk = 512;
        float scratchL[kOfflineChunk];
        float scratchR[kOfflineChunk];

        int  next     = 0;                // index of the next event to apply
        int  badEvent = -1;
        int  pos      = 0;
        while (pos < numFrames)
        {
            // Apply everything due AT this frame first, so an event at frame F
            // is APPLIED at sample F and not at sample F+1. That is the whole
            // point of chunking at the boundary rather than at a fixed stride.
            while (next < eventCount && events[next].frame <= (int64_t) pos)
            {
                // Not reachable from the ABI export, which resolves every event
                // through canApplyAutomationEvent before a sample is rendered —
                // kept because this method is also driven in-process (the RT
                // gate), where nothing has run that pass.
                if (! applyAutomationEvent(events[next])) { badEvent = next; return badEvent; }
                ++next;
            }

            // Render up to the NEXT event boundary, capped at the chunk size
            // every prepare() of the rig guarantees.
            int n = juce::jmin(kOfflineChunk, numFrames - pos);
            if (next < eventCount)
            {
                const int64_t until = events[next].frame - (int64_t) pos;
                if (until > 0 && until < (int64_t) n) n = (int) until;
            }

            for (int i = 0; i < n; ++i)
            {
                const size_t src = (size_t) (pos + i) * (size_t) channels;
                scratchL[i] = interleaved[src];
                scratchR[i] = channels == 2 ? interleaved[src + 1] : scratchL[i];
            }

            float* chans[2] = { scratchL, scratchR };
            rig.process(chans, 2, n);

            for (int i = 0; i < n; ++i)
            {
                if (channels == 2)
                {
                    interleaved[(size_t) (pos + i) * 2]     = scratchL[i];
                    interleaved[(size_t) (pos + i) * 2 + 1] = scratchR[i];
                }
                else
                {
                    // Mono in, mono out: the rig ran in stereo over a duplicated
                    // input, so fold it back rather than dropping one side — a
                    // stereo widener or a ping-pong delay writes real information
                    // into the right channel and picking `left` would silently
                    // discard half of it.
                    interleaved[(size_t) (pos + i)] = 0.5f * (scratchL[i] + scratchR[i]);
                }
            }
            pos += n;
        }

        return badEvent;
    }

public:
    juce::TimeSliceThread& readAheadThread() noexcept { return readThread_; }

    /// Preloaded-audio budget, shared by every TransportSource.
    ///
    /// Playing from RAM removes the only unbounded stall left on the audio
    /// thread (see TransportSource::wireReaderToTransport), but a chart with a
    /// song plus six stems is hundreds of megabytes, so this is a budget and
    /// not a promise: a source that does not fit streams instead, and is then
    /// merely as slow as it was before.
    bool reservePreloadBytes(int64_t bytes) noexcept;
    void releasePreloadBytes(int64_t bytes) noexcept;
    /// Ceiling for the above, derived from the machine rather than hard-coded.
    static int64_t preloadBudgetBytes() noexcept;

private:
    /// Resolve the chain a GLOBAL node id belongs to, and its LOCAL id inside
    /// that chain. Returns nullptr for a negative id or a player index past
    /// kMaxPlayers — never clamps, because clamping would silently apply one
    /// seat's edit to another seat's rig.
    dcrig::DspChain* rigOfNode(int nodeId, int& localOut) noexcept
    {
        if (nodeId < 0) return nullptr;
        const int p = nodeId / kPlayerNodeIdStride;
        if (! validPlayer(p)) return nullptr;
        localOut = nodeId % kPlayerNodeIdStride;
        return &dspRigs_[(size_t) p];
    }
    const dcrig::DspChain* rigOfNode(int nodeId, int& localOut) const noexcept
    {
        return const_cast<AudioEngine*>(this)->rigOfNode(nodeId, localOut);
    }

    std::array<dcrig::DspChain, kMaxPlayers> dspRigs_;

    // ---- Idle sleep (dsp/IdleSleep.h) --------------------------------------
    /// Per seat, AUDIO THREAD ONLY: the device callback owns every field, and
    /// prepareMasterBus resets them with no callback in flight. The offline
    /// renders never touch these.
    std::array<dcrig::RigIdleSleep, kMaxPlayers> rigIdle_{};
    /// Test hook, default ON (never exported, never off in the product): the
    /// RT gate's older scenarios pump silence for longer than the hold and
    /// would otherwise stop exercising the rig they exist to check.
    std::atomic<bool> rigIdleEnabled_{ true };
};

/// What convertFileForDevice did (ABI v43, DotCharter #435).
struct DeviceRateConversion
{
    /// 16 when a file was written; 0 when none was needed or it failed.
    int     bits   = 0;
    int64_t frames = 0;
    /// The preload step the written samples play at (the dcps chunk).
    float   step   = 0.0f;
    /// Set when the call failed; `writeFailed` tells a destination that could
    /// not be written from a source that could not be decoded or converted.
    juce::String error;
    bool         writeFailed = false;
    dcload::ResampleReport report{};
};

/// The load-time rate conversion, done once into a file (ABI v43,
/// dca_convert_audio_for_device). Decodes `src` with the formats every engine
/// registers and runs the SAME steps TransportSource::tryPreloadIntoMemory
/// runs at `deviceRate` -- the same decode, dcload::upsampleDecodedBuffer and
/// the same Int16PreloadSource quantiser -- so opening `dst` on a `deviceRate`
/// device preloads bit-identical samples. Writes exactly what the preload
/// holds: its int16 samples as 16-bit PCM plus its step in a dcps chunk
/// (DeviceRateWav.h), which every open honours -- so a song whose conversion
/// overshot full scale is bit-exact at 16 bits too.
/// Writes NOTHING (bits 0, no error) when the load would not convert, i.e.
/// the file is already at or above `deviceRate`. On failure `dst` is deleted.
/// Never on the audio thread: it allocates the whole song and may spread the
/// conversion over worker threads.
void convertFileForDevice(const juce::File& src, const juce::File& dst,
                          double deviceRate, DeviceRateConversion& out);

/// One playable audio file. Owns an AudioFormatReader, an
/// AudioFormatReaderSource, an AudioTransportSource and a
/// RubberBandAudioSource (so files of any sample rate join the device clock
/// and the user can tempo-stretch / pitch-shift in real time).
class TransportSource
{
public:
    TransportSource(AudioEngine& engine);
    ~TransportSource();

    juce::String openFile(const juce::File& file);
    /// Open from an in-memory blob (PSARC pipeline avoids materialising
    /// decoded WAVs to %LocalAppData% just to feed them back to JUCE).
    /// `bytes` is copied into an internal buffer so the caller can free
    /// the input immediately. `formatHint` is an optional file-extension
    /// hint ("wav", "ogg", ".flac") used to bias the format detector;
    /// when empty all registered formats are tried.
    juce::String openMemory(const void* bytes, size_t numBytes, const juce::String& formatHint);

    void play();
    void pause();
    void stop();

    double getPositionSeconds() const;
    /// Same as getPositionSeconds but compensates for the Rubber Band
    /// stretcher's read-ahead buffer so the audible playhead is reported
    /// when tempo != 1.
    double getAudiblePositionSeconds() const;
    double getLengthSeconds()   const;
    void   seekSeconds(double seconds);

    /// AUDIO THREAD, for the engine's song clock (ABI v38) only — and only
    /// while this source is bound, which is what keeps it alive. Reads the
    /// audible position (the same number dca_source_get_audible_position_seconds
    /// returns), the play flag, the seek generation and the effective tempo.
    /// No lock: the transport's position, the stretcher's published read-ahead
    /// and the tempo are all plain reads or atomics.
    void readSongClockRt(double& position, bool& playing, uint32_t& seekGeneration,
                         double& tempo) const noexcept;

    void setVolume(float gain);
    void setPan   (float pan);

    /// Which Rubber Band engine this source's stretcher was built with. Test
    /// hook for the per-engine quality flag (see AudioEngine::setStretchFaster).
    bool stretcherUsedFasterForTest() const noexcept;
    void setTempoFactor   (float factor);
    void setPitchSemitones(float semitones);

    juce::AudioSource* sourceForMixer() noexcept;

    /// True when this source plays from RAM, i.e. with no
    /// BufferingAudioSource and therefore no lock on the audio thread.
    bool    isPreloaded()    const noexcept { return memorySource_ != nullptr; }
    /// Decoded bytes this source is holding; 0 when streaming. 16-bit PCM
    /// (Int16PreloadSource), so frames x channels x 2 -- half what the
    /// float32 buffer this replaced held (#411).
    int64_t preloadedBytes() const noexcept { return preloadedBytes_; }
    /// The preloaded samples (tests and the RT gate); null when streaming.
    const Int16PreloadSource* preloadedSource() const noexcept { return memorySource_.get(); }
    /// Encoded/original bytes still held for the reader's benefit (openMemory
    /// only). A PRELOADED source needs neither the decoder nor the bytes it
    /// decoded from, and holding them is pure footprint — 50% on top of the
    /// float32 buffer for a 16-bit WAV. Zero on the preloaded path.
    int64_t retainedInputBytes() const noexcept { return (int64_t) memoryBytes_.getSize(); }
    /// Whether the format reader (file handle, decoder state) is still alive.
    /// False once a source is playing from RAM.
    bool    holdsDecoder() const noexcept { return reader_ != nullptr; }
    /// The rate the PRELOAD BUFFER is at. Equal to the device rate when the
    /// offline upsample ran, otherwise the file's own rate; 0 when streaming.
    double  playbackSampleRate() const noexcept { return playbackSampleRate_; }
    /// Which converter this source's audio goes through (JuceSharp #33): the
    /// offline polyphase / windowed-sinc conversion at load, the streaming
    /// polyphase stage, JUCE's linear converter (a ratio with no rational
    /// form), or none. Read only by the ABI layer's open log and tests -- never
    /// by the audio thread. converted == false when nothing converts or JUCE's
    /// linear converter does it.
    const dcload::ResampleReport& lastResampleReport() const noexcept { return resampleReport_; }

    /// Force the per-source Rubber Band stretcher into pass-through mode
    /// (used while this source is part of the shared stem-group bus).
    /// Also resets the per-source tempo/pitch back to 1.0 / 0 semis so a
    /// later un-group restores a clean state.
    void setStretcherBypass(bool bypassed) noexcept;
    bool isStretcherBypassed() const noexcept { return stretcherBypassed_; }

private:
    /// Seek the transport and reset the stretcher(s) in front of it.
    void moveTransportTo(double seconds);
    /// False only when `requirePreload` and the reader could not be decoded into
    /// RAM; nothing is wired then (a WEM is never streamed, see the .cpp).
    bool wireReaderToTransport(bool requirePreload);
    /// Decode the whole file into RAM and play from there. See the comment
    /// on wireReaderToTransport for why this is not merely an optimisation.
    bool tryPreloadIntoMemory();
    /// Build streamBuffer_ + streamResampler_ over `input` for the device rate.
    /// False (both null) when nothing converts or the ratio has no rational
    /// form. Load thread.
    bool tryWireStreamingResampler(juce::PositionableAudioSource& input);
    /// Tear the previous open all the way down: detach from the engine's mixer
    /// FIRST (it holds a raw pointer to panner_), then the chain, then the
    /// preload buffer and its budget reservation. Called before a re-open and
    /// shared with the destructor.
    void releaseWiring();

    AudioEngine&                                       engine_;
    std::unique_ptr<juce::AudioFormatReader>           reader_;
    std::unique_ptr<juce::AudioFormatReaderSource>     readerSource_;
    // STREAMING a device-rate file whose step is not 1/32768 (ABI v43): wraps
    // readerSource_ and plays its samples at the stored step. Null otherwise.
    std::unique_ptr<juce::PositionableAudioSource>     storedStepGain_;
    // STREAMING at a rate other than the device's (JuceSharp #33): the
    // read-ahead the transport used to build for itself, and the polyphase
    // stage on top of it. Both null when preloaded, when the rates match, or
    // when the ratio has no rational form (JUCE's converter then runs).
    std::unique_ptr<juce::BufferingAudioSource>        streamBuffer_;
    std::unique_ptr<dcload::PolyphaseStreamSource>     streamResampler_;
    // The whole song as 16-bit PCM at playbackSampleRate_ (#411). Owns its
    // samples: the float buffer it was quantised from lives only for the
    // length of tryPreloadIntoMemory.
    std::unique_ptr<Int16PreloadSource>                memorySource_;
    int64_t                                            preloadedBytes_ = 0;
    // Owned bytes for openMemory(); kept alive for the lifetime of the
    // reader because juce::MemoryInputStream does not copy.
    juce::MemoryBlock                                  memoryBytes_;
    std::unique_ptr<juce::AudioTransportSource>        transport_;
    std::unique_ptr<class RubberBandAudioSource>       stretcher_;
    std::unique_ptr<class PanningAudioSource>          panner_;
    // `gain_` used to sit here: setVolume stored it and nothing ever read it.
    // The gain that is really applied lives on the juce::AudioTransportSource.
    bool                                               attached_ = false;
    bool                                               stretcherBypassed_ = false;
    double                                             fileSampleRate_ = 0.0;
    // The step a device-rate WAV stores its samples at (DeviceRateWav.h, ABI
    // v43); 0 for any other file. Read by the preload and the streaming wiring.
    float                                              storedStep_ = 0.0f;
    // What the PRELOAD BUFFER is actually at, which is the device rate once
    // tryPreloadIntoMemory has upsampled it offline. This -- not the file's
    // own rate -- is what the transport is told, so its resampler sits at
    // ratio 1.0 (a bit-exact passthrough) while length, seek and audible
    // position stay in real seconds.
    double                                             playbackSampleRate_ = 0.0;
    // What the offline conversion did (which path, which ratio, how long).
    dcload::ResampleReport                             resampleReport_{};
    // Song clock (ABI v38): bumped by every seek and stop, so the chart source
    // can tell a seek from playback even when the jump is tiny.
    std::atomic<uint32_t>                              seekGeneration_{ 0 };
    // This source was the engine's song clock when a re-open tore its wiring
    // down; the re-wire binds it again.
    bool                                               wasSongClock_ = false;
};

} // namespace dotcharter
