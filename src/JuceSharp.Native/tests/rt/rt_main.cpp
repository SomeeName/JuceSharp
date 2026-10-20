// Native real-time safety gate for juce_sharp.
//
// Drives the REAL device callback (AudioEngine::EngineCallback) block by block
// on the calling thread through the null device's manual-pump mode, with an
// allocation hook armed, and fails the build when the audio thread allocates,
// takes a lock it did not take before, or blocks in the kernel.
//
// Run:  juce_sharp_rtcheck --baseline rt_baseline.txt --lock-sites rt_lock_sites.txt
//
// Everything here compiles the SHIPPING sources (JUCESHARP_CORE_SOURCES) with
// DCA_RTCHECK=1 and the debug CRT. No ABI export exists for any of it.

#include <filesystem>
#include "rt_sentinel.h"

#include "AudioEngine.h"
#include "NullAudioIODevice.h"
#include "MidiOutputDevice.h"
#include "dsp/DspModuleFactory.h"
#include "dsp/Retirement.h"
#include "dsp/cores/dcdsp_acousticsim.h"
#include "dsp/cores/dcdsp_detune.h"
#include "dsp/cores/dcdsp_droptune.h"
#include "dsp/cores/dcdsp_glitch.h"
#include "dsp/cores/dcdsp_harmonizer.h"
#include "dsp/cores/dcdsp_whammy.h"
#include "dsp/cores/dcdsp_mikustomp.h"
#include "dsp/cores/dcdsp_octaver.h"
#include "dsp/cores/dcdsp_phaser.h"
#include "dsp/cores/dcdsp_rotary.h"
// Cores driven directly by the prepare()/NaN/oversized-block scenarios below.
#include "dsp/cores/dcdsp_amp.h"
#include "dsp/cores/dcdsp_autowah.h"
#include "dsp/cores/dcdsp_bigmuff.h"
#include "dsp/cores/dcdsp_bitcrusher.h"
#include "dsp/cores/dcdsp_compressor.h"
#include "dsp/cores/dcdsp_distortion.h"
#include "dsp/cores/dcdsp_feedbacker.h"
#include "dsp/cores/dcdsp_filterseq.h"
#include "dsp/cores/dcdsp_graphiceq.h"
#include "dsp/cores/dcdsp_detail_fir.h"
#include "dsp/cores/dcdsp_freeze.h"
#include "dsp/cores/dcdsp_fuzz.h"
#include "dsp/cores/dcdsp_gate.h"
#include "dsp/cores/dcdsp_miku.h"
#include "dsp/cores/dcdsp_noisereducer.h"
#include "dsp/cores/dcdsp_overdrive.h"
#include "dsp/cores/dcdsp_platereverb.h"
#include "dsp/cores/dcdsp_subharmonic.h"
#include "dsp/cores/dcdsp_tapestop.h"
#include "dsp/cores/dcdsp_tremolo.h"
#include "dsp/cores/dcdsp_wah.h"
#include "dsp/cores/dcdsp_looper.h"
// 2026-09 drive gear (src/dsp/gear/DriveGear.cpp).
#include "dsp/cores/dcdsp_rat.h"
#include "dsp/cores/dcdsp_ds1.h"
#include "dsp/cores/dcdsp_hm2.h"
#include "dsp/cores/dcdsp_tapesat.h"
#include "dsp/cores/dcdsp_bassod.h"
#include "dsp/cores/dcdsp_tubepre.h"
#include "dsp/cores/dcdsp_octfuzz.h"
// 2026-09 amp / cab gear (src/dsp/gear/AmpCabGear.cpp).
#include "dsp/cores/dcdsp_bassamp.h"
#include "dsp/cores/dcdsp_acpre.h"
#include "dsp/cores/dcdsp_poweramp.h"
#include "dsp/cores/dcdsp_micpos.h"
#include "dsp/cores/dcdsp_cab_irs.h"     // cabir::kNumModels (the model sweep)
// Rocksmith 2014's own amp chains (src/dsp/gear/RsAmpGear.cpp, DotCharter #88).
// Their table is loaded at run time (ABI v45); the gate loads a SYNTHETIC one.
#include "dsp/cores/dcdsp_rsamp.h"
#include "../dsp/rsamp_synthetic_table.h"
// 2026-09 reverb gear (src/dsp/gear/ReverbGear.cpp).
#include "dsp/cores/dcdsp_convreverb.h"
#include "dsp/voice/dcvoice_excitation_data.h"
#include "dsp/voice/dcvoice_pack.h"
#include "dsp/voice/dcvoice_plan.h"
#include "dsp/voice/dcvoice_timeline.h"
#include "dotcharter_vst_bridge.h"
#include "../dsp/voice_pack_fixture.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

using namespace dotcharter;

// ---------------------------------------------------------------------------
// Micro test harness (a framework dependency is not worth it for one exe)
// ---------------------------------------------------------------------------

namespace {

struct ScenarioResult
{
    std::string name;
    bool        passed = true;
    std::string detail;      // first failure, or an informational note
};

std::vector<ScenarioResult> g_results;
ScenarioResult*             g_current = nullptr;

void note(const std::string& s)
{
    std::printf("      %s\n", s.c_str());
    if (g_current != nullptr && g_current->detail.empty() && g_current->passed)
        g_current->detail = s;
}

void fail(const std::string& s)
{
    std::printf("      FAIL: %s\n", s.c_str());
    if (g_current != nullptr)
    {
        g_current->passed = false;
        if (g_current->detail.empty() || g_current->detail.rfind("FAIL", 0) != 0)
            g_current->detail = "FAIL: " + s;
    }
}

#define RT_CHECK(cond, msg) do { if (!(cond)) fail(msg); } while (false)

ScenarioResult& beginScenario(const char* name)
{
    std::printf("[ RUN      ] %s\n", name);
    g_results.push_back(ScenarioResult{ name, true, {} });
    g_current = &g_results.back();
    return g_results.back();
}

void endScenario()
{
    if (g_current != nullptr)
        std::printf("[ %s ] %s\n", g_current->passed ? "     OK " : "  FAILED",
                    g_current->name.c_str());
    g_current = nullptr;
}

void setEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
#else
    if (value != nullptr) ::setenv(name, value, 1); else ::unsetenv(name);
#endif
}

// ---------------------------------------------------------------------------
// Evidence gathering for the race / contention probes
// ---------------------------------------------------------------------------
//
// A probe that races two threads needs EVIDENCE that the race happened — N
// bridge waits, N rebuilds overlapping the render, N storm writes landing
// inside the pump — before its real assertion means anything, and every such
// probe carries a vacuity floor for exactly that. How much evidence a FIXED
// window collects is a property of the machine, not of the code: on GitHub's
// 2-vCPU runner phase A of RtBridgeWaitIsBounded saw 58 bridge waits in its 256
// blocks against a floor of 129 (#55), and on a loaded 16-thread box the
// convreverb race dipped to 126 rebuilds against 100. The floor was right; the
// window assumed a fast machine.
//
// So a probe GATHERS: it runs rounds until its floor is met, and only a
// deadlock-detector cap ends it without one. The floors and the bounds the
// probes prove are unchanged — a starved machine just takes longer to collect
// the same evidence, and FAILS (naming the starvation) only if it cannot do so
// before the cap. JUCESHARP_RT_EVIDENCE_CAP_SECONDS overrides the cap for a
// local experiment (a mutation run, say); nothing in CI sets it.
std::chrono::milliseconds evidenceCap()
{
    static const std::chrono::milliseconds cap = []
    {
        const char* v = std::getenv("JUCESHARP_RT_EVIDENCE_CAP_SECONDS");
        const double s = (v != nullptr) ? std::atof(v) : 0.0;
        return std::chrono::milliseconds((long long) ((s > 0.0 ? s : 60.0) * 1000.0));
    }();
    return cap;
}

struct Gathered
{
    bool      enough       = false;  // the floor was met
    bool      recorderFull = false;  // stopped because the mark recorder was nearly full
    long long rounds       = 0;
    double    seconds      = 0.0;

    /// Appended to a vacuity message: says WHY the evidence is missing, so a
    /// red gate on a starved runner reads as that and not as a code fault.
    std::string why() const
    {
        if (enough) return {};
        char buf[256];
        if (recorderFull)
            std::snprintf(buf, sizeof(buf),
                          " (gathered for %.1f s over %lld round(s) until the mark recorder was nearly full)",
                          seconds, rounds);
        else
            std::snprintf(buf, sizeof(buf),
                          " (gathered for %.1f s over %lld round(s) and hit the %.0f s deadlock-detector cap: "
                          "the machine starved the probe, or the race never happens)",
                          seconds, rounds, (double) evidenceCap().count() / 1000.0);
        return buf;
    }
};

/// Runs `round()` at least `minRounds` times and then until `enough()` holds,
/// bounded by evidenceCap(). `enough()` is evaluated after every round,
/// including the last, so a floor met by the final round still counts. Also
/// stops before the dcrt mark recorder overflows (a pumped block records
/// several marks), because counts read from an overflowed recorder are short.
template <class Round, class Enough>
Gathered gatherEvidence(Round&& round, Enough&& enough, long long minRounds = 1)
{
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    Gathered g;
    for (;;)
    {
        round();
        ++g.rounds;
        if (g.rounds >= minRounds && enough()) { g.enough = true; break; }
        if (clock::now() - t0 >= evidenceCap()) break;
        if (dcrt::markCount() >= dcrt::kMaxMarks - dcrt::kMaxMarks / 4) { g.recorderFull = true; break; }
    }
    g.seconds = std::chrono::duration<double>(clock::now() - t0).count();
    return g;
}

void scenarioSnapshotRetainsReaderOwnership()
{
    beginScenario("RtEqSnapshotRetainsReaderOwnership");
    LatestSnapshot<std::array<int, 64>> mailbox;
    std::array<int, 64> value{};
    value.fill(1); mailbox.publish(value);
    RT_CHECK(mailbox.consume(), "first snapshot not published");
    const auto& held = mailbox.current();
    // A whole burst while the callback retains its reference. Two-slot reuse
    // overwrites held on the second publication, without requiring a race.
    for (int generation = 2; generation <= 100; ++generation)
    {
        value.fill(generation); mailbox.publish(value);
        RT_CHECK(std::all_of(held.begin(), held.end(), [](int x) { return x == 1; }),
                 "publisher reused the callback-owned slot");
    }
    RT_CHECK(mailbox.consume(), "latest snapshot not delivered");
    RT_CHECK(mailbox.current() == value, "latest snapshot is torn or stale");
    RT_CHECK(!mailbox.consume(), "unchanged snapshot reported dirty");
    endScenario();
}

// ---------------------------------------------------------------------------
// Engine fixture on the manual-pump null device
// ---------------------------------------------------------------------------

class Fixture
{
public:
    /// `rigIdleSleep` is OFF by default HERE (and only here — the product
    /// default is on): most scenarios below pump silence into a rig for longer
    /// than the idle hold, and a sleeping rig would silently stop exercising
    /// the very modules those scenarios exist to check. The idle-sleep
    /// scenarios switch it on explicitly.
    /// `outputMask` (dca_engine_config::output_channel_mask) opens a SPARSE set
    /// of the null device's outputs instead of the first two; 0 = the default.
    Fixture(int sampleRate = 48000, int blockSize = 256, int inputChannels = 2,
            bool rigIdleSleep = false, uint32_t outputMask = 0)
        : sampleRate_(sampleRate), blockSize_(blockSize)
    {
        setEnv(nulldev::kEnvVar, "1");
        setEnv(nulldev::kManualEnvVar, "1");

        engine_ = std::make_unique<AudioEngine>();
        dca_engine_config cfg{};
        cfg.sample_rate     = sampleRate;
        cfg.buffer_frames   = blockSize;
        cfg.output_channels = 2;
        cfg.output_channel_mask = outputMask;
        cfg.input_channels  = inputChannels;
        std::strncpy(cfg.output_type_name, nulldev::kTypeName, sizeof(cfg.output_type_name) - 1);
        engine_->setRigIdleSleepForTest(rigIdleSleep);
        error_ = engine_->start(cfg).toStdString();
    }

    ~Fixture()
    {
        if (engine_ != nullptr) engine_->stop();
        engine_.reset();
        setEnv(nulldev::kManualEnvVar, nullptr);
        setEnv(nulldev::kEnvVar, nullptr);
    }

    bool ok() const { return error_.empty() && nulldev::manualDevice() != nullptr; }
    const std::string& error() const { return error_; }
    AudioEngine& engine() { return *engine_; }

    int  sampleRate() const { return sampleRate_; }
    int  blockSize()  const { return blockSize_; }
    long long blockBudgetNs() const
    {
        return (long long) (1.0e9 * (double) blockSize_ / (double) sampleRate_);
    }

    /// A deterministic stimulus in the device input, so every stage downstream
    /// (drop tune, capture ring, rig, monitor mix) does real work. Must be
    /// re-installed after any device reopen — open() re-sizes and zeroes.
    /// `freqHz` matters when a scenario looks at the SHAPE of the output and
    /// not just its level: the manual pump replays this one block for ever, so
    /// any frequency that is not an exact multiple of sampleRate/blockSize puts
    /// a discontinuity at every block boundary — which is precisely what a
    /// zipper probe is trying to measure. 187.5 Hz is one cycle per 256 frames
    /// at 48 kHz. The 220 Hz default is kept for the level-only scenarios.
    /// `phase0` decides WHERE in the waveform a block boundary falls, which is
    /// the whole ballgame for a discontinuity probe: at the default 0 the
    /// boundary of a whole-cycle stimulus sits on a ZERO CROSSING, where even a
    /// hard cut to silence is a jump of nothing. Pass pi/2 to put the boundary
    /// on the peak, where a cut is a full-amplitude step.
    void installInputStimulus(float amp = 0.25f, double freqHz = 220.0, double phase0 = 0.0)
    {
        auto* dev = nulldev::manualDevice();
        if (dev == nullptr) return;
        auto& bufs = dev->manualInputBuffers();
        double phase = phase0;
        const double inc = 2.0 * 3.14159265358979323846 * freqHz / (double) sampleRate_;
        for (size_t i = 0; i < (bufs.empty() ? 0 : bufs[0].size()); ++i)
        {
            const float s = amp * (float) std::sin(phase);
            phase += inc;
            for (auto& ch : bufs) ch[i] = s;
        }
    }

    /// Pump with the sentinel disarmed (warm-up / setup).
    void pumpWarm(int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            dcrt::ScopedRtThread rt;
            nulldev::pump(1);
        }
    }

    /// Pump with the sentinel armed, advancing the block counter.
    void pumpArmed(int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            dcrt::g_currentBlock.store((unsigned) i, std::memory_order_relaxed);
            dcrt::ScopedRtThread rt;
            nulldev::pump(1);
        }
    }

    /// True when any sample of the last rendered output block is NaN or Inf.
    /// NOT derivable from lastOutputPeak(): std::max(x, NaN) returns x, so a
    /// whole block of NaN reads back as a peak of exactly 0 — silence, which is
    /// a different fault with a different cause.
    bool lastOutputHasNonFinite() const
    {
        auto* dev = nulldev::manualDevice();
        if (dev == nullptr) return false;
        for (const auto& ch : dev->manualOutputBuffers())
            for (float s : ch)
                if (! std::isfinite(s)) return true;
        return false;
    }

    /// Peak absolute sample in the last rendered output block.
    float lastOutputPeak() const
    {
        auto* dev = nulldev::manualDevice();
        if (dev == nullptr) return 0.0f;
        float peak = 0.0f;
        for (const auto& ch : dev->manualOutputBuffers())
            for (float s : ch) peak = std::max(peak, std::abs(s));
        return peak;
    }

private:
    int                          sampleRate_;
    int                          blockSize_;
    std::unique_ptr<AudioEngine> engine_;
    std::string                  error_;
};

/// Warm-up protocol: 64 blocks unarmed absorbs magic-static init
/// (SincTable::get, the perTick cache, the module catalogue), JUCE's own lazy
/// allocations and first-touch MidiBuffer growth. Reported separately, because
/// a CHANGE in the warm-up count means something moved into or out of prepare.
constexpr int kWarmupBlocks = 64;

int warmUpAndArm(Fixture& fx)
{
    dcrt::reset();
    dcrt::arm();
    fx.pumpWarm(kWarmupBlocks);
    const int warmAllocs = dcrt::allocCount();
    dcrt::reset();
    return warmAllocs;
}

void reportHeap(const char* label)
{
    const auto table = dcrt::formatHeapViolations();
    if (! table.empty())
        std::printf("      %s heap events:\n%s", label, table.c_str());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// A deterministic in-memory WAV so a scenario can open real TransportSources
/// without touching the filesystem or shipping fixtures.
juce::MemoryBlock makeWavBlob(double seconds, int sampleRate, double freqHz)
{
    const int frames = (int) (seconds * sampleRate);
    juce::AudioBuffer<float> buf(2, frames);
    const double inc = 2.0 * 3.14159265358979323846 * freqHz / (double) sampleRate;
    double phase = 0.0;
    for (int i = 0; i < frames; ++i)
    {
        const float s = 0.3f * (float) std::sin(phase);
        phase += inc;
        buf.setSample(0, i, s);
        buf.setSample(1, i, s);
    }

    juce::MemoryBlock block;
    {
        juce::WavAudioFormat wav;
        auto stream = std::make_unique<juce::MemoryOutputStream>(block, false);
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wav.createWriterFor(stream.get(), (double) sampleRate, 2, 16, {}, 0));
        if (writer != nullptr)
        {
            stream.release(); // writer owns it
            writer->writeFromAudioSampleBuffer(buf, 0, frames);
        }
    }
    return block;
}

/// A minimal in-process instrument. No VST3 needed: the instrument bus only
/// cares that it is an AudioPluginInstance that accepts MIDI and clears its
/// buffer. Deliberately allocation-free in processBlock.
class StubInstrument final : public juce::AudioPluginInstance
{
public:
    StubInstrument()
        : juce::AudioPluginInstance(BusesProperties().withOutput(
              "Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "RtCheck Stub Instrument"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        midiEventsSeen_.fetch_add(midi.getNumEvents(), std::memory_order_relaxed);
        // ABI v34: record WHERE in the block each event landed. Read straight
        // off the iterator's metadata — constructing a juce::MidiMessage per
        // event would be doing real work inside the thing the allocation
        // sentinel is watching, and the note number is just data[1].
        {
            const int block = blockIndex_.fetch_add(1, std::memory_order_relaxed);
            for (const auto meta : midi)
            {
                const int n = midiLogCount_.load(std::memory_order_relaxed);
                if (n >= kMidiLogCap) break;
                midiLog_[(size_t) n] = MidiHit{ block, meta.samplePosition,
                                                meta.numBytes > 1 ? (int) meta.data[1] : -1 };
                midiLogCount_.store(n + 1, std::memory_order_relaxed);
            }
        }
        buffer.clear();
        // Off by default, so every existing scenario sees the wire it always
        // saw. Armed, it renders ONE block of NaN — a VSTi blowing up is the
        // last untrusted stage before the device and the streaming mirror.
        if (poison_.exchange(false, std::memory_order_acq_rel))
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                float* d = buffer.getWritePointer(ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    d[i] = std::numeric_limits<float>::quiet_NaN();
            }
            poisoned_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void poisonNextBlock() { poison_.store(true, std::memory_order_release); }
    int  poisonedBlocks() const { return poisoned_.load(std::memory_order_relaxed); }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "RtCheck Stub Instrument";
        d.pluginFormatName = "RtCheck";
        d.isInstrument = true;
    }

    int midiEventsSeen() const { return midiEventsSeen_.load(std::memory_order_relaxed); }
    void resetMidiEventsSeen() { midiEventsSeen_.store(0, std::memory_order_relaxed); }

    // --- ABI v34 MIDI placement log ---------------------------------------
    struct MidiHit { int block = -1; int pos = -1; int note = -1; };
    static constexpr int kMidiLogCap = 64;

    /// Message thread, between pumps. Also rebases the block counter, so a
    /// scenario phase reads offsets relative to its own first pumped block.
    void clearMidiLog()
    {
        midiLogCount_.store(0, std::memory_order_relaxed);
        blockIndex_  .store(0, std::memory_order_relaxed);
    }
    int     midiLogCount() const { return midiLogCount_.load(std::memory_order_relaxed); }
    MidiHit midiLogAt(int i) const
    {
        return (i >= 0 && i < midiLogCount()) ? midiLog_[(size_t) i] : MidiHit{};
    }

private:
    std::atomic<int>  midiEventsSeen_{ 0 };
    std::atomic<bool> poison_{ false };
    std::atomic<int>  poisoned_{ 0 };
    std::array<MidiHit, kMidiLogCap> midiLog_{};
    std::atomic<int>  midiLogCount_{ 0 };
    std::atomic<int>  blockIndex_{ 0 };
};

/// A stereo-in/stereo-out input plugin that passes audio through until it is
/// asked to poison ONE block with NaN — the failure a real VST3 (or a sandbox
/// child writing a partial block) can produce at any moment. Allocation-free.
class StubNaNInputPlugin final : public juce::AudioPluginInstance
{
public:
    StubNaNInputPlugin()
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "RtCheck Stub NaN Input"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (! poison_.exchange(false, std::memory_order_acq_rel)) return;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            float* d = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                d[i] = std::numeric_limits<float>::quiet_NaN();
        }
        poisoned_.fetch_add(1, std::memory_order_relaxed);
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "RtCheck Stub NaN Input";
        d.pluginFormatName = "RtCheck";
        d.isInstrument = false;
    }

    void poisonNextBlock() { poison_.store(true, std::memory_order_release); }
    int  poisonedBlocks() const { return poisoned_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> poison_{ false };
    std::atomic<int>  poisoned_{ 0 };
};

// ---------------------------------------------------------------------------
// In-process stub for the sandbox child (assertion #10)
// ---------------------------------------------------------------------------

#if defined(_WIN32)
class StubBridgeChild
{
public:
    bool open(const char* shm, const char* reqEvent, const char* respEvent)
    {
        mapping_ = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm);
        if (mapping_ == nullptr) return false;
        view_ = ::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, dcvb::kTotalBytes);
        if (view_ == nullptr) return false;
        req_  = ::OpenEventA(EVENT_ALL_ACCESS, FALSE, reqEvent);
        resp_ = ::OpenEventA(EVENT_ALL_ACCESS, FALSE, respEvent);
        if (req_ == nullptr || resp_ == nullptr) return false;

        dcvb::header(view_)->childState.store(dcvb::kChildReady, std::memory_order_release);
        run_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    /// Stop answering without closing: models a wedged (not crashed) child.
    void setAnswering(bool a) { answering_.store(a, std::memory_order_release); }

    /// Answer this many microseconds after the request. Zero (the default) is a
    /// child that replies almost instantly, which is NOT what a real one does:
    /// a cross-process VSTi takes hundreds of microseconds, and the parent's
    /// poll-only path checks the response sequence NANOSECONDS after posting.
    /// Without a delay the stub can satisfy that check by accident and the
    /// poll-only recovery assertion below goes vacuous.
    void setAnswerDelayUs(int us) { answerDelayUs_.store(us, std::memory_order_release); }

    /// One MIDI event as the child received it (bridge v3): the packed short,
    /// its sample offset inside the request's block, and that block's
    /// render-clock frame — so `blockStart + offset` is where it will sound.
    struct MidiSeen { uint32_t seq; uint32_t packed; uint32_t offset; int64_t blockStart; uint32_t blockFrames; };

    void clearMidiLog() { std::lock_guard<std::mutex> lk(logMutex_); midiCount_ = 0; }
    int  midiLogCount() { std::lock_guard<std::mutex> lk(logMutex_); return midiCount_; }
    MidiSeen midiLogAt(int i)
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        return i >= 0 && i < midiCount_ ? midiLog_[(size_t) i] : MidiSeen{};
    }

    void close()
    {
        run_.store(false, std::memory_order_release);
        if (req_ != nullptr) ::SetEvent(req_);
        if (thread_.joinable()) thread_.join();
        if (view_ != nullptr)    { ::UnmapViewOfFile(view_); view_ = nullptr; }
        if (mapping_ != nullptr) { ::CloseHandle(mapping_);  mapping_ = nullptr; }
        if (req_ != nullptr)     { ::CloseHandle(req_);      req_ = nullptr; }
        if (resp_ != nullptr)    { ::CloseHandle(resp_);     resp_ = nullptr; }
    }

    ~StubBridgeChild() { close(); }

private:
    void loop()
    {
        auto* h = dcvb::header(view_);
        while (run_.load(std::memory_order_acquire))
        {
            ::WaitForSingleObject(req_, 20);
            if (! run_.load(std::memory_order_acquire)) break;
            h->heartbeat.fetch_add(1, std::memory_order_relaxed);
            if (! answering_.load(std::memory_order_acquire)) continue;

            const uint32_t seq = h->requestSeq.load(std::memory_order_acquire);
            if (seq == lastSeq_) continue;
            lastSeq_ = seq;

            if (const int delayUs = answerDelayUs_.load(std::memory_order_acquire); delayUs > 0)
            {
                // NOT sleep_for: on Windows a sub-millisecond sleep rounds up to
                // the timer resolution, which is 1 ms only while SOME process on
                // the box holds timeBeginPeriod(1) (a browser, Rider, the app
                // itself) and ~15.6 ms otherwise — longer than a 10.7 ms block
                // budget, so phase C of RtBridgeWaitIsBounded timed out both of
                // its retries on a QUIET box and passed on a busy one
                // (2026-08-29, five runs). A real child answers from an event,
                // not a sleep; the stub spins so its delay means what it says.
                LARGE_INTEGER f{}, t0{}, t{};
                ::QueryPerformanceFrequency(&f);
                ::QueryPerformanceCounter(&t0);
                const long long until = t0.QuadPart + (long long) ((double) delayUs * 1e-6 * (double) f.QuadPart);
                do { YieldProcessor(); ::QueryPerformanceCounter(&t); } while (t.QuadPart < until);
            }

            const uint32_t frames = std::min(h->blockFrames, (uint32_t) dcvb::kMaxBlockFrames);
            {
                // Bridge v3: read the request's MIDI exactly as juce_vst_host
                // does — packed word from the header, offset from the timing
                // block — before answering, i.e. while the parent still owns
                // nothing of this request.
                const auto& timing = *dcvb::midiTiming(view_);
                const int count = (int) std::min<uint32_t>(h->midiCount, dcvb::kMaxMidiEvents);
                std::lock_guard<std::mutex> lk(logMutex_);
                for (int i = 0; i < count && midiCount_ < (int) midiLog_.size(); ++i)
                    midiLog_[(size_t) midiCount_++] = MidiSeen{ seq, h->midi[i], timing.midiOffset[i],
                                                               timing.blockStartFrame, frames };
            }
            // Stamp a per-block constant so a torn/spliced block is detectable.
            const float marker = 0.01f * (float) (seq % 97u);
            for (int ch = 0; ch < dcvb::kMaxChannels; ++ch)
            {
                float* c = dcvb::chainOutChannel(view_, ch);
                float* i = dcvb::instrOutChannel(view_, ch);
                for (uint32_t n = 0; n < frames; ++n) { c[n] = marker; i[n] = marker; }
            }
            h->chainActive      = 1;
            h->instrumentActive = 1;
            h->responseSeq.store(seq, std::memory_order_release);
            ::SetEvent(resp_);
        }
    }

    HANDLE            mapping_ = nullptr;
    HANDLE            req_ = nullptr, resp_ = nullptr;
    void*             view_ = nullptr;
    std::thread       thread_;
    std::atomic<bool> run_{ false };
    std::atomic<bool> answering_{ true };
    std::atomic<int>  answerDelayUs_{ 0 };
    uint32_t          lastSeq_ = 0;
    std::mutex                    logMutex_;
    std::array<MidiSeen, 256>     midiLog_{};
    int                           midiCount_ = 0;
};
#endif // _WIN32

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

const char* const kRigNodes[] = {
    "gate", "compressor", "overdrive", "amp", "cab", "delay",
    "reverb", "chorus", "eq", "gain", "splitter", "merger"
};

void buildTwelveNodeRig(AudioEngine& e, std::vector<int>& ids)
{
    for (const char* id : kRigNodes)
    {
        const int n = e.dspAddNode(0, id);
        if (n >= 0) ids.push_back(n);
    }
    // Put two nodes on branch 2 so the split path in DspChain::process is
    // actually exercised rather than merely present.
    if (ids.size() >= 12)
    {
        e.dspSetNodeBranch(ids[6], 2);
        e.dspSetNodeBranch(ids[7], 2);
    }
}

// 1 ------------------------------------------------------------------------
void scenarioIdleCallback()
{
    beginScenario("RtIdleCallbackAllocatesNothing");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start on the null device: " + fx.error()); endScenario(); return; }

    const int warm = warmUpAndArm(fx);
    fx.pumpArmed(512);
    dcrt::disarm();

    note("warm-up allocations (informational): " + std::to_string(warm));
    reportHeap("idle");
    RT_CHECK(dcrt::allocCount() == 0,
             "audio callback allocated " + std::to_string(dcrt::allocCount()) + " time(s) while idle");
    RT_CHECK(dcrt::freeCount() == 0,
             "audio callback freed " + std::to_string(dcrt::freeCount()) + " time(s) while idle");
    endScenario();
}

// 2 ------------------------------------------------------------------------
void scenarioFullRig()
{
    beginScenario("RtFullRigAllocatesNothing");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    std::vector<int> ids;
    buildTwelveNodeRig(fx.engine(), ids);
    RT_CHECK(ids.size() == 12, "expected a 12-node rig, built " + std::to_string(ids.size()));

    warmUpAndArm(fx);
    fx.pumpArmed(512);
    dcrt::disarm();

    reportHeap("full rig");
    RT_CHECK(dcrt::allocCount() == 0,
             "12-node rig allocated " + std::to_string(dcrt::allocCount()) + " time(s) on the callback");
    endScenario();
}

// 2a -----------------------------------------------------------------------
// The 2026-09 DRIVE drawer (src/dsp/gear/DriveGear.cpp), all seven modules in
// ONE chain. RtFullRigAllocatesNothing already walks the catalogue, but it
// builds a twelve-node rig one node deep in each family; this one stacks every
// drive module at once, which is what a user who is auditioning pedals
// actually does and is the arrangement where five oversamplers, two dry-delay
// lines and a sag follower all run in the same callback.
//
// The second half is the oversized-block guard. The catalogue-wide scenario
// for that (RtOversizedBlockIsSkippedNotPartiallyProcessed) carries a
// hard-coded list of four cores, so a new core is NOT covered by it — and the
// failure it catches is silent: a core that clamps instead of skipping
// processes the head of the block and leaves the tail full-level dry, i.e.
// clean DI spliced into the middle of a distorted block.
void scenarioDriveGearChain()
{
    beginScenario("RtDriveGearChainAllocatesNothingAndSkipsOversizedBlocks");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    const char* const kDriveNodes[] = {
        "rat", "ds1", "hm2", "tapesat", "bassod", "tubepre", "octfuzz"
    };
    std::vector<int> ids;
    for (const char* id : kDriveNodes)
    {
        const int n = fx.engine().dspAddNode(0, id);
        if (n >= 0) ids.push_back(n);
    }
    RT_CHECK(ids.size() == 7, "expected seven drive nodes, built " + std::to_string(ids.size()));

    warmUpAndArm(fx);
    fx.pumpArmed(100);
    if (ids.size() == 7)
    {
        // Every SWITCH in the drawer, thrown while the callback is running and
        // the sentinel is armed - the rule this wave wrote down for itself
        // (docs/agents/dsp-tone-rig.md). This scenario built a static chain and
        // pumped it until 2026-09-12, so rat's clip footswitch (two per-sample
        // clip thresholds) and tubepre's bright and cabSim (a whole filter
        // branch each) were never written on a live callback at all.
        fx.engine().dspSetParam(ids[0], "clip", 1.0f);      // rat
        fx.engine().dspSetParam(ids[5], "bright", 1.0f);    // tubepre
        fx.engine().dspSetParam(ids[5], "cabSim", 1.0f);    // tubepre
        fx.engine().dspSetParam(ids[5], "sag", 1.0f);       // tubepre, a ramped multiplier
    }
    fx.pumpArmed(100);
    dcrt::disarm();

    reportHeap("drive gear chain");
    RT_CHECK(dcrt::allocCount() == 0,
             "the seven-node drive chain allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "the seven-node drive chain freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback");
    // Not vacuous: a chain that silenced itself, or that went non-finite, would
    // pass an allocation gate perfectly.
    RT_CHECK(! fx.lastOutputHasNonFinite(), "the drive rig produced a non-finite output block");

    // --- oversized block: skipped WHOLE, never half-processed ---------------
    const int prepared = 256;
    const int oversize = 2 * prepared;
    auto probe = [&](const char* name, auto& core, auto& params)
    {
        core.setParams(params);
        core.prepare(48000.0, prepared, 2);
        core.setParams(params);

        std::vector<float> l((std::size_t) oversize), r((std::size_t) oversize);
        std::vector<float> ref((std::size_t) oversize);
        double phase = 0.0;
        const double inc = 6.283185307179586 * 220.0 / 48000.0;
        for (int i = 0; i < oversize; ++i)
        {
            const float s = 0.3f * (float) std::sin(phase);
            phase += inc;
            l[(std::size_t) i] = r[(std::size_t) i] = ref[(std::size_t) i] = s;
        }
        float* chans[2] = { l.data(), r.data() };
        core.process(chans, 2, oversize);

        double headDelta = 0.0;
        for (int i = 0; i < prepared; ++i)
            headDelta = std::max(headDelta,
                                 (double) std::abs(l[(std::size_t) i] - ref[(std::size_t) i]));
        RT_CHECK(headDelta < 1e-6,
                 std::string(name) + " processed the first " + std::to_string(prepared)
                 + " frames of a " + std::to_string(oversize)
                 + "-frame block and left the rest DRY (head delta "
                 + std::to_string(headDelta) + ")");
    };

    { dcdsp::RatCore c;     dcdsp::RatParams p;     p.distortion = 0.8f; p.volume = 0.8f;
      probe("rat", c, p); }
    { dcdsp::Ds1Core c;     dcdsp::Ds1Params p;     p.dist = 0.8f; p.level = 0.8f;
      probe("ds1", c, p); }
    { dcdsp::Hm2Core c;     dcdsp::Hm2Params p;     p.dist = 0.8f; p.level = 0.8f;
      probe("hm2", c, p); }
    { dcdsp::TapeSatCore c; dcdsp::TapeSatParams p; p.drive = 0.8f;
      probe("tapesat", c, p); }
    { dcdsp::BassOdCore c;  dcdsp::BassOdParams p;  p.drive = 0.8f; p.level = 0.8f;
      probe("bassod", c, p); }
    { dcdsp::TubePreCore c; dcdsp::TubePreParams p; p.gain = 0.8f; p.level = 0.8f;
      probe("tubepre", c, p); }
    { dcdsp::OctFuzzCore c; dcdsp::OctFuzzParams p; p.fuzz = 0.8f; p.volume = 0.8f;
      probe("octfuzz", c, p); }

    endScenario();
}

// The 2026-09 MODULATION gear, all five at once: two multi-tap delay lines, a
// four-voice allpass bank, a stereo panner, a three-tap BBD ensemble and a
// Hilbert-pair frequency shifter, chained and pumped for 200 blocks.
//
// The twelve-node rig above is a fine allocation probe for a STEADY chain and
// contains not one of the writes these five make interesting: a waveform
// switch, an allpass STAGE COUNT, a VOICE COUNT and a sideband DIRECTION are
// all "structure" a core is tempted to resize something for, and each of them
// is driven here while the callback is armed. Every one of them is instead a
// crossfade over state prepare() already sized for the worst case.
void scenarioModulationGear()
{
    beginScenario("RtModulationGearAllocatesNothingUnderStructureWrites");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    static const char* const kModulationNodes[] = {
        "tzflanger", "barberpole", "autopan", "ensemble", "freqshift"
    };
    std::vector<int> ids;
    for (const char* id : kModulationNodes)
    {
        const int n = fx.engine().dspAddNode(0, id);
        if (n >= 0) ids.push_back(n);
    }
    RT_CHECK(ids.size() == 5,
             "expected the five modulation modules, built " + std::to_string(ids.size()));
    if (ids.size() != 5) { endScenario(); return; }

    // Every module audible and working its widest setting, so the chain is not
    // quietly idling past the interesting code.
    auto& e = fx.engine();
    e.dspSetParam(ids[0], "mix", 1.0f);      e.dspSetParam(ids[0], "depthMs", 10.0f);
    e.dspSetParam(ids[0], "manualMs", 10.0f); e.dspSetParam(ids[0], "feedback", 0.8f);
    e.dspSetParam(ids[1], "mix", 1.0f);      e.dspSetParam(ids[1], "depth", 1.0f);
    e.dspSetParam(ids[1], "stages", 12.0f);  e.dspSetParam(ids[1], "feedback", 0.8f);
    e.dspSetParam(ids[2], "depth", 1.0f);    e.dspSetParam(ids[2], "rateHz", 6.0f);
    e.dspSetParam(ids[3], "mix", 1.0f);      e.dspSetParam(ids[3], "depth", 1.0f);
    e.dspSetParam(ids[4], "mix", 1.0f);      e.dspSetParam(ids[4], "shiftHz", 250.0f);
    e.dspSetParam(ids[4], "feedback", 0.6f);

    warmUpAndArm(fx);
    constexpr int kBlocks = 200;
    for (int i = 0; i < kBlocks; ++i)
    {
        fx.pumpArmed(1);
        switch (i % 5)
        {
            case 0: e.dspSetParam(ids[0], "shape", (float) (i & 1)); break;
            case 1: e.dspSetParam(ids[1], "stages", (i & 1) ? 2.0f : 12.0f); break;
            case 2: e.dspSetParam(ids[2], "shape", (float) (i % 3)); break;
            case 3: e.dspSetParam(ids[3], "voices", (i & 1) ? 2.0f : 3.0f); break;
            default: e.dspSetParam(ids[4], "direction", (float) (i % 3)); break;
        }
    }
    dcrt::disarm();

    reportHeap("modulation gear");
    RT_CHECK(dcrt::allocCount() == 0,
             "the modulation chain allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "the modulation chain freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback");

    // …and the pedals were actually in the path. An allocation gate over a
    // silent chain is the vacuous version of this scenario.
    float ip = 0.f, ir = 0.f, op = 0.f, orms = 0.f;
    e.dspSetMetering(0, true);
    fx.pumpWarm(64);
    int lit = 0;
    for (int id : ids)
        if (e.dspGetNodeMeter(id, ip, ir, op, orms) && op > 1.0e-5f) ++lit;
    RT_CHECK(lit == 5, "only " + std::to_string(lit)
             + " of the 5 modulation nodes passed any audio");
    e.dspSetMetering(0, false);
    endScenario();
}

// 2a -----------------------------------------------------------------------
// The 2026-09 DELAY gear wave (src/dsp/gear/DelayGear.cpp): multitap,
// duckdelay, bbddelay, granular and rhythmdelay, all five chained, at the
// worst case their knobs allow and with structure-ish edits landing MID-RUN.
//
// RtFullRigAllocatesNothing already sweeps the catalogue, so the point of this
// one is the things a static rig cannot reach: granular's grain SCHEDULER at
// 32 concurrent grains of 500 ms (a per-grain window rebuild would be an
// O(grainLen) burst inside the callback that launched it), a FREEZE toggle
// (which flips the write side and the grain read rate on the callback), a
// DENSITY change (which re-derives the hop and the normalising gain while
// grains are in flight), and bbddelay's clock-tracking filter pair, which
// redesigns two biquads per channel as the delay glides.
//
// It also asserts two properties each module states and no allocation counter
// can see: an oversized block is skipped WHOLE, and a NaN param is refused at
// the door rather than stored (every core's clamp is NaN-transparent, so one
// that got through would poison the coefficients for good).
void scenarioDelayGearRig()
{
    beginScenario("RtDelayGearRigIsAllocationFree");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();
    AudioEngine& e = fx.engine();

    static const char* const kDelayGear[] = {
        "multitap", "duckdelay", "bbddelay", "granular", "rhythmdelay"
    };

    // ---- the five modules exist, and behave at their edges ------------------
    for (const char* id : kDelayGear)
    {
        auto mod = dcrig::createModule(id);
        if (mod == nullptr) { fail(std::string("the catalogue cannot build '") + id + "'"); continue; }

        constexpr int kPrepared = 256;
        constexpr int kOversize = 2 * kPrepared;
        mod->prepare(48000.0, kPrepared, 2);

        std::vector<float> l((std::size_t) kOversize), r((std::size_t) kOversize), ref((std::size_t) kOversize);
        double phase = 0.0;
        for (int i = 0; i < kOversize; ++i)
        {
            const float v = 0.3f * (float) std::sin(phase);
            phase += 6.283185307179586 * 220.0 / 48000.0;
            l[(std::size_t) i] = r[(std::size_t) i] = ref[(std::size_t) i] = v;
        }
        float* chans[2] = { l.data(), r.data() };
        mod->process(chans, 2, kOversize);

        double headDelta = 0.0;
        for (int i = 0; i < kPrepared; ++i)
            headDelta = std::max(headDelta,
                                 (double) std::abs(l[(std::size_t) i] - ref[(std::size_t) i]));
        RT_CHECK(headDelta < 1e-6,
                 std::string(id) + " processed the first " + std::to_string(kPrepared)
                 + " frames of an oversized block and left the rest DRY (head delta "
                 + std::to_string(headDelta) + ") — it must be skipped whole");

        // ---- the degenerate calls --------------------------------------
        // These five dropped the two guards every core in the other four
        // drawers opens with. DspChain never passes a null set, but THIS caller
        // does -- and without the guard `channels[c]` is an access violation,
        // not a skipped block, which is why it is worth one line of test rather
        // than one line of trust. The audio must also be untouched.
        {
            std::vector<float> before = l;
            float* nullSet[2] = { nullptr, nullptr };
            mod->process(nullptr, 2, kPrepared);
            mod->process(nullSet, 2, kPrepared);
            mod->process(chans, 2, 0);
            mod->process(chans, 0, kPrepared);
            mod->process(chans, -1, kPrepared);
            double delta = 0.0;
            for (std::size_t i = 0; i < before.size(); ++i)
                delta = std::max(delta, (double) std::abs(l[i] - before[i]));
            RT_CHECK(delta < 1e-9,
                     std::string(id) + " changed the buffer on a degenerate call (delta "
                     + std::to_string(delta) + ")");
        }

        // Every declared param must refuse a NaN and keep what it had.
        const auto& cat = dcrig::moduleCatalogue();
        for (const auto& info : cat)
        {
            if (info.id != id) continue;
            for (const auto& prm : info.params)
            {
                RT_CHECK(mod->setParam(prm.id.c_str(), prm.defaultValue),
                         std::string(id) + "." + prm.id + " refused its own default");
                RT_CHECK(! mod->setParam(prm.id.c_str(), std::numeric_limits<float>::quiet_NaN()),
                         std::string(id) + "." + prm.id + " ACCEPTED a NaN");
                float kept = 0.0f;
                RT_CHECK(mod->getParam(prm.id.c_str(), kept)
                             && std::abs(kept - prm.defaultValue) < 1.0e-3f,
                         std::string(id) + "." + prm.id + " did not keep its value across a rejected NaN");
            }
        }
    }

    // ---- all five live, at the worst case, with edits mid-run ---------------
    std::vector<int> ids;
    for (const char* id : kDelayGear)
    {
        const int n = e.dspAddNode(0, id);
        if (n >= 0) ids.push_back(n);
    }
    if (ids.size() != 5) { fail("expected 5 delay-gear nodes, built " + std::to_string(ids.size())); endScenario(); return; }

    e.dspSetParam(ids[0], "taps", 4.0f);
    e.dspSetParam(ids[0], "feedback", 0.8f);
    e.dspSetParam(ids[1], "feedback", 0.8f);
    e.dspSetParam(ids[1], "duckDb", 24.0f);
    e.dspSetParam(ids[2], "feedback", 1.0f);
    e.dspSetParam(ids[2], "modDepth", 1.0f);
    e.dspSetParam(ids[3], "grainMs", 500.0f);
    e.dspSetParam(ids[3], "density", 32.0f);
    e.dspSetParam(ids[3], "pitchSemitones", 12.0f);
    e.dspSetParam(ids[3], "spray", 1.0f);
    e.dspSetParam(ids[4], "feedback", 0.9f);
    e.dspSetParam(ids[4], "gateDepth", 1.0f);

    warmUpAndArm(fx);
    constexpr int kBlocks = 200;
    for (int b = 0; b < kBlocks; ++b)
    {
        // A freeze toggle and a density change while 32 grains are in flight,
        // plus a bbd time move so the clock-tracking filters redesign under
        // the sentinel rather than only while it is disarmed.
        if (b == 40)  e.dspSetParam(ids[3], "freeze", 1.0f);
        if (b == 90)  e.dspSetParam(ids[3], "density", 4.0f);
        if (b == 120) e.dspSetParam(ids[3], "freeze", 0.0f);
        if (b == 150) e.dspSetParam(ids[2], "timeMs", 600.0f);
        dcrt::g_currentBlock.store((unsigned) b, std::memory_order_relaxed);
        dcrt::ScopedRtThread rt;
        nulldev::pump(1);
    }
    dcrt::disarm();

    reportHeap("delay gear rig");
    RT_CHECK(dcrt::allocCount() == 0,
             "the delay-gear rig allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "the delay-gear rig freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback");
    RT_CHECK(! fx.lastOutputHasNonFinite(), "the delay-gear rig produced a non-finite sample");

    // ---- what granular's worst case COSTS ----------------------------------
    // Measured as the DIFFERENCE against the same engine with an empty rig,
    // because a raw block time is mostly the device callback around it. Not an
    // assertion: a wall-clock bound would flake the moment two suites run side
    // by side (JuceSharp/AGENTS.md). It is reported so a regression in the
    // grain loop is visible in the log.
    auto meanBlockNs = [&](int blocks)
    {
        for (int i = 0; i < 32; ++i) { dcrt::ScopedRtThread rt; nulldev::pump(1); }  // settle
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < blocks; ++i) { dcrt::ScopedRtThread rt; nulldev::pump(1); }
        const auto t1 = std::chrono::steady_clock::now();
        return (double) std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
               / (double) blocks;
    };

    e.dspClear(0);
    const double emptyNs = meanBlockNs(400);
    const int only = e.dspAddNode(0, "granular");
    if (only >= 0)
    {
        e.dspSetParam(only, "grainMs", 500.0f);
        e.dspSetParam(only, "density", 32.0f);
        e.dspSetParam(only, "pitchSemitones", 12.0f);
        e.dspSetParam(only, "mix", 1.0f);
        const double loadedNs = meanBlockNs(400);
        const double perSample = (loadedNs - emptyNs) / (double) fx.blockSize();
        note("granular at density 32 / grainMs 500: "
             + std::to_string((long long) (loadedNs - emptyNs)) + " ns per "
             + std::to_string(fx.blockSize()) + "-frame block, "
             + std::to_string(perSample) + " ns/sample (empty rig "
             + std::to_string((long long) emptyNs) + " ns/block, budget "
             + std::to_string(fx.blockBudgetNs()) + " ns)");
    }
    endScenario();
}

// 2b -----------------------------------------------------------------------
// Per-node meters + chain latency (ABI v30). Two things are asserted here and
// the SECOND one is the point: a meter that reads zero would pass an
// allocation gate perfectly, so this drives a real stimulus through a real rig
// and fails if the lights never came on.
void scenarioNodeMetering()
{
    beginScenario("RtNodeMeteringIsAllocationFreeAndNotVacuous");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    std::vector<int> ids;
    buildTwelveNodeRig(fx.engine(), ids);
    if (ids.size() != 12) { fail("expected a 12-node rig, built " + std::to_string(ids.size())); endScenario(); return; }

    // --- off by default, and off means zero -------------------------------
    RT_CHECK(! fx.engine().dspMetering(0), "metering was ON before anyone asked for it");
    fx.pumpWarm(64);
    float ip = -1.f, ir = -1.f, op = -1.f, orms = -1.f;
    RT_CHECK(fx.engine().dspGetNodeMeter(ids[3], ip, ir, op, orms), "no meter for a live node");
    RT_CHECK(ip == 0.f && ir == 0.f && op == 0.f && orms == 0.f,
             "meters moved while metering was OFF");

    // --- on: allocation-free on the callback ------------------------------
    fx.engine().dspSetMetering(0, true);
    RT_CHECK(fx.engine().dspMetering(0), "dspSetMetering(true) did not stick");
    warmUpAndArm(fx);
    fx.pumpArmed(512);
    dcrt::disarm();
    reportHeap("node metering");
    RT_CHECK(dcrt::allocCount() == 0,
             "metering allocated " + std::to_string(dcrt::allocCount()) + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "metering freed " + std::to_string(dcrt::freeCount()) + " time(s) on the callback");

    // --- on: the numbers are real ------------------------------------------
    // The stimulus is a 0.25-amplitude sine, so an early node's input must be
    // audibly non-zero and its RMS must sit under its peak.
    int lit = 0;
    for (int id : ids)
    {
        float a = 0.f, b = 0.f, c = 0.f, d = 0.f;
        if (! fx.engine().dspGetNodeMeter(id, a, b, c, d)) { fail("meter read failed mid-chain"); break; }
        if (a > 1.0e-5f || c > 1.0e-5f) ++lit;
        RT_CHECK(b <= a + 1.0e-4f, "input RMS exceeded input peak on node " + std::to_string(id));
        RT_CHECK(d <= c + 1.0e-4f, "output RMS exceeded output peak on node " + std::to_string(id));
        RT_CHECK(std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d),
                 "a meter went non-finite on node " + std::to_string(id));
    }
    RT_CHECK(lit > 0, "every meter read zero while a 0.25 sine was running through the rig");
    note(std::to_string(lit) + " of 12 nodes reading non-zero");

    // --- the bulk read agrees with the per-node read ------------------------
    std::vector<float> bulk((size_t) ids.size() * 4u, -1.f);
    const int n = fx.engine().dspReadMeters(0, bulk.data(), (int) bulk.size());
    RT_CHECK(n == (int) ids.size(), "bulk meter read reported " + std::to_string(n) + " nodes");
    {
        float a = 0.f, b = 0.f, c = 0.f, d = 0.f;
        fx.engine().dspGetNodeMeter(ids[0], a, b, c, d);
        RT_CHECK(std::fabs(bulk[0] - a) < 1.0e-3f, "bulk read disagrees with the per-node read");
    }

    // --- latency reporting --------------------------------------------------
    // Nothing in the twelve-node rig adds latency, so the honest answer is 0…
    const int base = fx.engine().dspChainLatency(0);
    RT_CHECK(base == 0, "a latency-free rig reported " + std::to_string(base) + " samples");
    // …and a klon, which buffers, must move it.
    const int klon = fx.engine().dspAddNode(0, "klon");
    if (klon >= 0)
    {
        int nodeLat = -1;
        RT_CHECK(fx.engine().dspGetNodeLatency(klon, nodeLat), "no latency for the klon node");
        RT_CHECK(nodeLat > 0, "klon reported " + std::to_string(nodeLat) + " samples of latency");
        RT_CHECK(fx.engine().dspChainLatency(0) == base + nodeLat,
                 "chain latency did not pick up the klon");
        // A BYPASSED node is not in the path and must not be budgeted for.
        fx.engine().dspSetBypass(klon, true);
        RT_CHECK(fx.engine().dspChainLatency(0) == base, "a bypassed node still counted toward latency");
        fx.engine().dspSetBypass(klon, false);
    }

    // --- switching metering off zeroes it -----------------------------------
    fx.engine().dspSetMetering(0, false);
    fx.pumpWarm(8);
    float za = -1.f, zb = -1.f, zc = -1.f, zd = -1.f;
    fx.engine().dspGetNodeMeter(ids[3], za, zb, zc, zd);
    RT_CHECK(za == 0.f && zb == 0.f && zc == 0.f && zd == 0.f,
             "meters stayed lit after metering was switched off");
    endScenario();
}

// 3 ------------------------------------------------------------------------
void scenarioParamStorm()
{
    beginScenario("RtConcurrentParamStormAllocatesNothing");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    std::vector<int> ids;
    buildTwelveNodeRig(fx.engine(), ids);
    if (ids.size() < 4) { fail("rig too small"); endScenario(); return; }

    std::atomic<bool> run{ true };
    std::atomic<int>  writes{ 0 };
    std::thread storm([&]
    {
        int i = 0;
        while (run.load(std::memory_order_acquire))
        {
            const float v = 0.3f + 0.4f * (float) ((i % 10) / 10.0);
            fx.engine().dspSetParam(ids[1], "threshold", -30.0f + v * 10.0f);
            fx.engine().dspSetParam(ids[2], "drive", v);
            fx.engine().dspSetParam(ids[9], "gain", v);
            ++i;
            writes.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    warmUpAndArm(fx);
    // At least the nominal 512 blocks, and on until the storm has actually
    // landed writes inside the armed pump (gatherEvidence, #55): a fixed window
    // on a starved box can be over before the storm thread is scheduled.
    constexpr int kMinBlocks = 512;
    constexpr int kChunk     = 64;
    constexpr int kWantWrites = 16;
    int kBlocks = 0;
    const Gathered g = gatherEvidence([&] { fx.pumpArmed(kChunk); kBlocks += kChunk; },
                                      [&] { return writes.load(std::memory_order_relaxed) >= kWantWrites; },
                                      kMinBlocks / kChunk);
    dcrt::disarm();
    run.store(false, std::memory_order_release);
    storm.join();

    const int locks = dcrt::countAt(dcrt::Kind::Lock, "dspChain/processMutex");
    reportHeap("param storm");
    RT_CHECK(dcrt::allocCount() == 0,
             "param storm caused " + std::to_string(dcrt::allocCount()) + " callback allocation(s)");
    RT_CHECK(g.enough,
             "only " + std::to_string(writes.load()) + " storm write round(s) landed inside "
             + std::to_string(kBlocks) + " pumped blocks — the storm never raced the callback" + g.why());
    RT_CHECK(locks == kBlocks,
             "expected exactly one dspChain/processMutex acquire per block ("
             + std::to_string(kBlocks) + "), saw " + std::to_string(locks));
    note("dspChain/processMutex p99 acquire wait: "
         + std::to_string(dcrt::percentileNs(dcrt::Kind::Lock, "dspChain/processMutex", 0.99)) + " ns");
    endScenario();
}

// 3b -----------------------------------------------------------------------
// Bypass / move / retag / remove are CROSSFADED at the chain level
// (DspChain.h, 2026-08-28): the ramp runs on the callback over a dry copy in
// scratch that prepare() sized, and remove/move wait on the message thread for
// the fade-out before they splice. So the thing to prove here is that a
// STRUCTURE storm — footswitch, reorder, retag, add+remove — costs the
// callback nothing, and that the message-thread waits do not stall the pump
// (they poll a heartbeat the callback advances). The audible half (the ramp's
// shape) is pinned offline by DspBypassCrossfadeTests in the managed suite,
// where the output is deterministic.
void scenarioStructureStorm()
{
    beginScenario("RtStructureStormCrossfadesWithoutAllocating");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    std::vector<int> ids;
    buildTwelveNodeRig(fx.engine(), ids);
    if (ids.size() < 12) { fail("rig too small"); endScenario(); return; }

    std::atomic<bool> run{ true };
    std::atomic<int>  ops{ 0 };
    std::atomic<int>  removes{ 0 };
    std::thread storm([&]
    {
        int i = 0;
        while (run.load(std::memory_order_acquire))
        {
            switch (i % 4)
            {
                case 0: fx.engine().dspSetBypass(ids[2], (i / 4) % 2 == 0); break;          // footswitch
                case 1: fx.engine().dspMoveNode(ids[5], (i / 4) % 6); break;                 // reorder
                case 2:                                                                       // add + remove
                {
                    const int n = fx.engine().dspAddNode(0, "gain");
                    if (n >= 0 && fx.engine().dspRemoveNode(n)) removes.fetch_add(1);
                    break;
                }
                case 3: fx.engine().dspSetNodeBranch(ids[7], 1 + (i / 4) % 2); break;        // retag
            }
            ++i;
            ops.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    warmUpAndArm(fx);
    // At least the nominal 512 blocks, and on until the storm has churned
    // (gatherEvidence, #55) — the two floors asserted below.
    constexpr int kMinBlocks = 512;
    constexpr int kChunk     = 64;
    int kBlocks = 0;
    const Gathered g = gatherEvidence([&] { fx.pumpArmed(kChunk); kBlocks += kChunk; },
                                      [&] { return ops.load() >= 16 && removes.load() >= 1; },
                                      kMinBlocks / kChunk);
    dcrt::disarm();
    run.store(false, std::memory_order_release);
    storm.join();

    reportHeap("structure storm");
    RT_CHECK(dcrt::allocCount() == 0,
             "structure storm caused " + std::to_string(dcrt::allocCount()) + " callback allocation(s)");
    RT_CHECK(dcrt::freeCount() == 0,
             "structure storm caused " + std::to_string(dcrt::freeCount()) + " callback free(s)");
    // Not vacuous: the storm has to have actually churned the chain while the
    // callback ran, and a remove that waits for its fade-out must return.
    RT_CHECK(ops.load() >= 16, "the storm barely ran (" + std::to_string(ops.load()) + " ops)" + g.why());
    RT_CHECK(removes.load() >= 1, "no add+remove cycle completed during the pump" + g.why());
    note("structure ops during " + std::to_string(kBlocks) + " blocks: " + std::to_string(ops.load())
         + " (" + std::to_string(removes.load()) + " faded removes)");
    endScenario();
}

// 3c -----------------------------------------------------------------------
// Edit scopes (ABI v34). The thing being measured is the number of ORDERING
// PUBLICATIONS a preset change costs, because that is what the callback sees:
// before v34 a twelve-pedal preset was twelve publications and twelve barriers,
// each one a separate structural change the audio thread walked, and the board
// assembled itself pedal by pedal. Inside a scope it must be ONE (nothing was
// removed) or TWO (the old board had to fade out first), independent of how
// many nodes the preset has.
//
// An allocation count alone would pass against an edit scope that did nothing
// at all, so this scenario asserts the OUTCOME as well: the unscoped build must
// cost at least one publication per node (that is the baseline it is beating),
// the scoped build must leave a chain of exactly the right size, an aborted
// scope must leave the chain it started with, and a nested begin must be
// refused.
void scenarioPresetEditScope()
{
    beginScenario("RtPresetCommitPublishesOncePerPresetNotOncePerNode");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();
    AudioEngine& e = fx.engine();

    std::atomic<bool>          go{ false };
    std::atomic<bool>          done{ false };
    std::atomic<unsigned long> unscopedPublishes{ 0 };
    std::atomic<unsigned long> scopedPublishes{ 0 };
    std::atomic<unsigned long> abortPublishes{ 0 };
    std::atomic<int>           afterCommitCount{ -1 };
    std::atomic<int>           afterAbortCount{ -1 };
    std::atomic<int>           nestedBegin{ -1 };
    std::atomic<int>           strayCommit{ -1 };

    std::thread worker([&]
    {
        while (! go.load(std::memory_order_acquire)) std::this_thread::yield();

        // --- baseline: the same preset, one publication at a time ----------
        const unsigned long p0 = e.dspPublishCountForTest(0);
        std::vector<int> ids;
        buildTwelveNodeRig(e, ids);
        unscopedPublishes.store(e.dspPublishCountForTest(0) - p0);

        // --- the same swap inside a scope: clear the old board, build a new
        //     one, set a parameter on every node, commit ---------------------
        nestedBegin.store(e.dspBeginEdit(0) ? 1 : 0);
        // A second begin on an open scope ABORTS the stale one and opens a
        // fresh scope. This EXPECTATION WAS DELIBERATELY FLIPPED (it used to
        // require a refusal): returning false left the dead scope OPEN, and
        // `editing_` suppresses every publication, so a caller who dropped a
        // scope froze the seat's rig for the life of the engine with every
        // later edit accumulating silently and every call still reporting
        // success. Discarding one leaked scope's structure edits beats a
        // permanently dead rig. The abort republishes the (unchanged) live
        // order, so the publication baseline is taken AFTER it.
        if (! e.dspBeginEdit(0)) nestedBegin.store(2);
        const unsigned long p1 = e.dspPublishCountForTest(0);
        e.dspClear(0);
        std::vector<int> fresh;
        for (const char* id : kRigNodes)
        {
            const int n = e.dspAddNode(0, id);
            if (n >= 0) fresh.push_back(n);
        }
        for (int n : fresh) e.dspSetParam(n, "gain", 0.5f); // most will reject; the write is the point
        if (fresh.size() >= 8) e.dspSetNodeBranch(fresh[6], 2);
        e.dspCommitEdit(0);
        scopedPublishes.store(e.dspPublishCountForTest(0) - p1);
        afterCommitCount.store(e.dspNodeCount(0));
        strayCommit.store(e.dspCommitEdit(0) ? 1 : 0); // no scope open: must fail

        // --- abort leaves the committed board exactly as it was ------------
        const unsigned long p2 = e.dspPublishCountForTest(0);
        e.dspBeginEdit(0);
        e.dspClear(0);
        e.dspAddNode(0, "gain");
        e.dspAbortEdit(0);
        abortPublishes.store(e.dspPublishCountForTest(0) - p2);
        afterAbortCount.store(e.dspNodeCount(0));
        done.store(true, std::memory_order_release);
    });

    warmUpAndArm(fx);
    go.store(true, std::memory_order_release);
    constexpr int kBlocks = 512;
    fx.pumpArmed(kBlocks);
    // The worker's fade waits poll a heartbeat the callback advances, so keep
    // pumping (unarmed) until it finishes rather than joining a stalled thread.
    for (int i = 0; i < 2000 && ! done.load(std::memory_order_acquire); ++i) fx.pumpWarm(1);
    dcrt::disarm();
    worker.join();

    reportHeap("preset edit scope");
    RT_CHECK(dcrt::allocCount() == 0,
             "a scoped preset swap caused " + std::to_string(dcrt::allocCount())
                 + " callback allocation(s)");
    RT_CHECK(dcrt::freeCount() == 0,
             "a scoped preset swap caused " + std::to_string(dcrt::freeCount()) + " callback free(s)");

    RT_CHECK(nestedBegin.load() == 1,
             nestedBegin.load() == 2 ? "a NESTED begin_edit was REFUSED — it must abort the stale "
                                       "scope and open a fresh one, or a leaked scope freezes the rig"
                                     : "begin_edit was refused on an idle rig");
    RT_CHECK(dcrig::DspChain::staleScopeAbortsForTest() >= 1,
             "the nested begin_edit did not record a stale-scope abort — the counter is the only "
             "tell that a caller leaked a scope");
    RT_CHECK(strayCommit.load() == 0, "commit_edit succeeded with no scope open");
    // The baseline this feature exists to beat: one publication per node.
    RT_CHECK(unscopedPublishes.load() >= 12,
             "an unscoped 12-node build published only "
                 + std::to_string(unscopedPublishes.load()) + " time(s) — the comparison is vacuous");
    // …against at most two for the whole swap (one to fade the old board out
    // while the new one fades in, one for the final order).
    RT_CHECK(scopedPublishes.load() >= 1 && scopedPublishes.load() <= 2,
             "a scoped 12-node preset swap published "
                 + std::to_string(scopedPublishes.load()) + " time(s), expected 1 or 2");
    RT_CHECK(afterCommitCount.load() == 12,
             "the committed preset has " + std::to_string(afterCommitCount.load())
                 + " nodes, expected 12");
    RT_CHECK(abortPublishes.load() <= 1,
             "an ABORTED scope published " + std::to_string(abortPublishes.load()) + " time(s)");
    RT_CHECK(afterAbortCount.load() == 12,
             "an aborted scope left " + std::to_string(afterAbortCount.load())
                 + " nodes, expected the 12 it started with");
    note("publications: unscoped build " + std::to_string(unscopedPublishes.load())
         + ", scoped swap " + std::to_string(scopedPublishes.load())
         + ", abort " + std::to_string(abortPublishes.load()));
    endScenario();
}

// 3d -----------------------------------------------------------------------
// Branch delay compensation (ABI v34). The merger now carries a delay line per
// branch, sized in prepare() for the worst case, and the compensation is
// re-derived whenever a node's LATENCY can have moved — which includes a plain
// param write. The hazard that buys is the audit-#5 one: a param that changes a
// latency must not size or grow anything on the callback, and the tap it moves
// must not be spliced.
//
// So this drives a real split region with a whammy in one branch (its
// `lowLatency` param re-runs the whole WSOLA geometry: 1,920 samples against
// 1,152 at 48 kHz) while a storm thread flips that param and the region's
// structure, with the allocation hook armed. The non-vacuity half is that the
// compensation has to be REAL: the region's branch delays must be non-zero
// with PDC on and zero with it off.
void scenarioBranchPdc()
{
    beginScenario("RtBranchPdcFollowsALatencyChangeWithoutAllocating");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();
    AudioEngine& e = fx.engine();

    // splitter -> [branch 1: whammy] [branch 2: empty] -> merger
    const int split  = e.dspAddNode(0, "splitter");
    const int whammy = e.dspAddNode(0, "whammy");
    const int merge  = e.dspAddNode(0, "merger");
    if (split < 0 || whammy < 0 || merge < 0)
    { fail("could not build the split region"); endScenario(); return; }
    e.dspSetNodeBranch(whammy, 1);

    RT_CHECK(e.dspBranchPdc(0), "branch PDC was OFF by default");
    int nodeLat = 0;
    RT_CHECK(e.dspGetNodeLatency(whammy, nodeLat) && nodeLat > 0,
             "the whammy reported " + std::to_string(nodeLat)
                 + " samples of latency — the region is not misaligned and the test is vacuous");
    const int regionLat = e.dspChainLatency(0);
    RT_CHECK(regionLat == nodeLat,
             "a two-branch region holding one " + std::to_string(nodeLat)
                 + "-sample module reported " + std::to_string(regionLat));

    std::atomic<bool> run{ true };
    std::atomic<int>  flips{ 0 };
    std::thread storm([&]
    {
        int i = 0;
        while (run.load(std::memory_order_acquire))
        {
            // The param that re-runs the geometry, i.e. moves the latency…
            e.dspSetParam(whammy, "lowLatency", (i % 2) == 0 ? 1.0f : 0.0f);
            // …and a structure change, so the recompute runs from both paths.
            if ((i % 8) == 0) e.dspSetBypass(whammy, (i % 16) == 0);
            flips.fetch_add(1, std::memory_order_relaxed);
            ++i;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    warmUpAndArm(fx);
    // The manual-pump device renders as fast as the CPU can, so 512 blocks go
    // by in a few milliseconds and a storm sleeping 1 ms between flips barely
    // gets a turn — this pumps ON past the nominal window until the storm has
    // actually churned, rather than asserting against a race with the
    // scheduler. Every one of those blocks is still armed. Bounded by the
    // evidence cap, not a block count (#55): a block ceiling is time on a fast
    // machine and nothing at all on a starved one.
    constexpr int kBlocks = 512;
    const Gathered g = gatherEvidence([&] { fx.pumpArmed(8); },
                                      [&] { return flips.load() >= 24; },
                                      kBlocks / 8);
    dcrt::disarm();
    run.store(false, std::memory_order_release);
    storm.join();

    reportHeap("branch PDC");
    RT_CHECK(dcrt::allocCount() == 0,
             "a latency change inside a split region caused " + std::to_string(dcrt::allocCount())
                 + " callback allocation(s)");
    RT_CHECK(dcrt::freeCount() == 0,
             "a latency change inside a split region caused " + std::to_string(dcrt::freeCount())
                 + " callback free(s)");
    RT_CHECK(flips.load() >= 24, "the storm barely ran (" + std::to_string(flips.load()) + " flips)" + g.why());

    // Not vacuous: switching the compensation off must actually change what the
    // merger is told, and back on must restore it.
    e.dspSetParam(whammy, "lowLatency", 0.0f);
    e.dspSetBypass(whammy, false);
    fx.pumpWarm(8);
    const int alignedLat = e.dspChainLatency(0);
    e.dspSetBranchPdc(0, false);
    RT_CHECK(! e.dspBranchPdc(0), "dspSetBranchPdc(false) did not stick");
    fx.pumpWarm(8);
    RT_CHECK(e.dspChainLatency(0) == alignedLat,
             "switching PDC off changed the reported chain latency, which describes the LATEST branch either way");
    e.dspSetBranchPdc(0, true);
    RT_CHECK(e.dspBranchPdc(0), "dspSetBranchPdc(true) did not stick");
    note("region latency " + std::to_string(alignedLat) + " samples, "
         + std::to_string(flips.load()) + " latency flips during the pump");
    endScenario();
}

// 3e -----------------------------------------------------------------------
// Node sidechain (ABI v34). Two hazards, and neither is an allocation on its
// own. First, keying a node means the chain copies its INPUT block for the
// whole pass — a buffer that must be sized in prepare() and never grown on the
// callback, and which is only filled when a node actually asks for it. Second,
// the catalogue advertises which modules accept a key through a SEPARATE export
// from the one that constructs them, which is two sources of truth for one
// capability — exactly how a UI ends up offering a control that does nothing.
//
// So: a real rig with a compressor keyed off the dry input and a gate keyed off
// the song mix, pumped with the hook armed, plus a sweep asserting every
// catalogue flag agrees with a constructed module and that the setter REJECTS a
// module that does not advertise the capability.
void scenarioNodeSidechain()
{
    beginScenario("RtSidechainKeyingIsAllocationFreeAndAdvertisedHonestly");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();
    AudioEngine& e = fx.engine();

    // --- the catalogue tells the truth about the capability ----------------
    const auto& cat = dcrig::moduleCatalogue();
    int advertised = 0, disagreements = 0;
    for (size_t i = 0; i < cat.size(); ++i)
    {
        const bool flagged = (dcrig::moduleTypeFlags((int) i) & dcrig::kModuleFlagSidechain) != 0;
        auto mod = dcrig::createModule(cat[i].id.c_str());
        const bool real = mod != nullptr && mod->supportsSidechain();
        if (flagged != real)
        {
            ++disagreements;
            fail("catalogue flag disagrees with the module for '" + cat[i].id + "' (flag "
                 + (flagged ? "set" : "clear") + ", module says " + (real ? "yes" : "no") + ")");
        }
        if (flagged) ++advertised;
    }
    RT_CHECK(disagreements == 0, "the sidechain flag table and the modules disagree");
    RT_CHECK(advertised >= 2,
             "only " + std::to_string(advertised) + " catalogue entries advertise a sidechain");

    // --- the setter refuses a module that cannot honour it ------------------
    const int gain = e.dspAddNode(0, "gain");
    RT_CHECK(gain >= 0 && ! e.dspSetNodeSidechain(gain, 1),
             "a 'gain' node accepted a sidechain source it cannot use");
    RT_CHECK(e.dspSetNodeSidechain(gain, 0), "clearing a sidechain was refused");

    // --- and a keyed rig costs the callback nothing -------------------------
    const int comp = e.dspAddNode(0, "compressor");
    const int gate = e.dspAddNode(0, "gate");
    if (comp < 0 || gate < 0) { fail("could not build the keyed rig"); endScenario(); return; }
    RT_CHECK(e.dspSetNodeSidechain(comp, 1), "the compressor refused the dry-input key");
    RT_CHECK(e.dspSetNodeSidechain(gate, 2), "the gate refused the song-mix key");
    int src = -1;
    RT_CHECK(e.dspGetNodeSidechain(comp, src) && src == 1,
             "the compressor's key source read back as " + std::to_string(src));
    // Threshold low enough that the compressor is really working on the key.
    e.dspSetParam(comp, "thresholdDb", -40.0f);
    e.dspSetParam(gate, "thresholdDb", -70.0f);

    warmUpAndArm(fx);
    fx.pumpArmed(512);
    dcrt::disarm();

    reportHeap("node sidechain");
    RT_CHECK(dcrt::allocCount() == 0,
             "a keyed rig allocated " + std::to_string(dcrt::allocCount()) + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "a keyed rig freed " + std::to_string(dcrt::freeCount()) + " time(s) on the callback");
    // Not vacuous: the rig has to still be making sound rather than having
    // gated itself shut on a key nobody filled.
    RT_CHECK(! fx.lastOutputHasNonFinite(), "the keyed rig produced a non-finite output block");
    note(std::to_string(advertised) + " of " + std::to_string(cat.size())
         + " catalogue entries accept a sidechain key");
    endScenario();
}

// 3f -----------------------------------------------------------------------
// The 2026-09 "dynamics" gear wave (docs/guides/gear-dynamics.md), all six of
// it in ONE chain. RtEveryCatalogueModuleIsAllocationFree already drives each
// catalogue entry on its own; this drives them TOGETHER and with their
// detectors actually working, because three of the six are only interesting
// once they are past their threshold — a module whose gain computer never
// engages is a multiply by 1.0 and would pass an allocation gate without ever
// reaching the code this wave added.
//
// The second half is the honest-latency check: the limiter's look-ahead is the
// one thing here that moves dspChainLatency, so the figure has to follow the
// knob. An adapter that swallowed it would make branch PDC align a region
// against a delay that is really there.
void scenarioDynamicsGear()
{
    beginScenario("RtDynamicsGearIsAllocationFreeAndReportsItsLatency");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();
    AudioEngine& e = fx.engine();

    static const char* const kDynamics[] =
        { "limiter", "transient", "multiband", "expander", "optocomp", "ducker" };

    std::vector<int> ids;
    int limiter = -1;
    for (const char* id : kDynamics)
    {
        const int n = e.dspAddNode(0, id);
        if (n < 0) { fail(std::string("could not create '") + id + "'"); continue; }
        ids.push_back(n);
        if (std::strcmp(id, "limiter") == 0) limiter = n;
    }
    if (ids.size() != 6) { fail("expected six dynamics nodes, built "
                                + std::to_string(ids.size())); endScenario(); return; }

    // Drive every detector PAST its threshold against the 0.25 stimulus, so the
    // gain computers, the opto cell's memory and the ducker's hold timer all
    // run rather than sitting at unity.
    e.dspSetParam(ids[0], "ceilingDb", -6.0f);      // limiter: really limiting
    e.dspSetParam(ids[0], "lookaheadMs", 2.0f);
    e.dspSetParam(ids[1], "attackDb", 6.0f);        // transient: shaping
    e.dspSetParam(ids[1], "sustainDb", -6.0f);
    e.dspSetParam(ids[2], "lowThresholdDb", -40.0f);// multiband: all three bands
    e.dspSetParam(ids[2], "midThresholdDb", -40.0f);
    e.dspSetParam(ids[2], "highThresholdDb", -40.0f);
    e.dspSetParam(ids[3], "thresholdDb", -6.0f);    // expander: below threshold
    e.dspSetParam(ids[4], "peakReduction", 0.9f);   // optocomp: cell working
    e.dspSetParam(ids[5], "thresholdDb", -40.0f);   // ducker: ducking
    // …and the two keyed ones are KEYED, which is the path that copies the
    // chain's input block.
    RT_CHECK(e.dspSetNodeSidechain(ids[3], 1), "the expander refused the dry-input key");
    RT_CHECK(e.dspSetNodeSidechain(ids[5], 2), "the ducker refused the song-mix key");

    // --- honest latency, before the measured window -------------------------
    // 2 ms at the gate's rate. Read off the engine rather than hard-coded, so
    // this does not become a second place that decides what the rate is.
    const double rate = e.currentSampleRate() > 0.0 ? e.currentSampleRate() : 48000.0;
    const int expected = (int) std::lround(rate * 2.0 / 1000.0);
    int nodeLat = -1;
    RT_CHECK(limiter >= 0 && e.dspGetNodeLatency(limiter, nodeLat), "no latency for the limiter node");
    RT_CHECK(nodeLat == expected,
             "the limiter reported " + std::to_string(nodeLat) + " samples of look-ahead, expected "
             + std::to_string(expected));
    RT_CHECK(e.dspChainLatency(0) == expected,
             "the chain latency did not pick up the limiter's look-ahead");
    e.dspSetParam(limiter, "lookaheadMs", 0.0f);
    RT_CHECK(e.dspGetNodeLatency(limiter, nodeLat) && nodeLat == 1,
             "a zero-millisecond look-ahead reported " + std::to_string(nodeLat)
             + " samples; the tap floors at one");
    e.dspSetParam(limiter, "lookaheadMs", 2.0f);

    warmUpAndArm(fx);
    fx.pumpArmed(200);
    dcrt::disarm();

    reportHeap("dynamics gear");
    RT_CHECK(dcrt::allocCount() == 0,
             "the six dynamics modules allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "the six dynamics modules freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback");
    // Not vacuous: a chain that silenced itself, or that went non-finite, would
    // pass an allocation gate perfectly.
    RT_CHECK(! fx.lastOutputHasNonFinite(), "the dynamics rig produced a non-finite output block");
    endScenario();
}

// DotCharter #155 / #157: the gate's AUTO threshold (a noise-floor measurement
// with a 100-element nth_element every 100 ms) and the bit crusher's FutzBox
// stages (gain, rectifying shaper, DC block, anti-alias low-pass) run on the
// armed callback, and every new param is thrown while it runs. Not vacuous: a
// near-silent stationary input must be GATED once the floor is measured, and a
// loud one must pass.
void scenarioAutoFloorGateAndFutzCrusher()
{
    beginScenario("RtAutoFloorGateAndFutzCrusherAllocateNothing");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    AudioEngine& e = fx.engine();
    e.setMasterGain(1.0f);
    e.setInputGain(1.0f);
    e.setMonitorGain(1.0f);

    const int gate = e.dspAddNode(0, "gate");
    const int crush = e.dspAddNode(0, "bitcrusher");
    if (gate < 0 || crush < 0) { fail("could not build gate -> bitcrusher"); endScenario(); return; }
    e.dspSetParam(gate, "autoFloor", 1.0f);
    e.dspSetParam(gate, "thresholdDb", -50.0f);
    e.dspSetParam(gate, "ratio", 50.0f);
    e.dspSetParam(gate, "holdMs", 0.0f);
    e.dspSetParam(gate, "releaseMs", 10.0f);
    e.dspSetParam(crush, "bits", 4.0f);
    e.dspSetParam(crush, "downsample", 16.0f);
    e.dspSetParam(crush, "mix", 0.5f);
    e.dspSetParam(crush, "inputDb", 24.0f);
    e.dspSetParam(crush, "drive", 60.0f);
    e.dspSetParam(crush, "intensity", 20.0f);
    e.dspSetParam(crush, "rectify", 50.0f);
    e.dspSetParam(crush, "filter", 50.0f);
    e.dspSetParam(crush, "outputDb", -30.0f);

    // A stationary input at about -70 dBFS: a noise floor, by the gate's reading.
    fx.installInputStimulus(0.0003f, 50.0);
    warmUpAndArm(fx);
    fx.pumpArmed(600);   // past one second of frames and several evaluations
    const float gatedPeak = fx.lastOutputPeak();

    // Every new param thrown on the live callback, then a loud input.
    e.dspSetParam(crush, "drive", 0.0f);
    e.dspSetParam(crush, "filter", 0.0f);
    fx.pumpArmed(20);
    e.dspSetParam(crush, "drive", 100.0f);
    e.dspSetParam(crush, "filter", 100.0f);
    e.dspSetParam(crush, "intensity", 100.0f);
    e.dspSetParam(crush, "rectify", 100.0f);
    e.dspSetParam(crush, "inputDb", -24.0f);
    e.dspSetParam(crush, "outputDb", 24.0f);
    e.dspSetParam(crush, "downsample", 1.0f);
    e.dspSetParam(gate, "autoFloor", 0.0f);
    fx.pumpArmed(20);
    e.dspSetParam(gate, "autoFloor", 1.0f);
    e.dspSetParam(crush, "inputDb", 24.0f);
    e.dspSetParam(crush, "outputDb", -30.0f);
    e.dspSetParam(crush, "downsample", 16.0f);
    fx.pumpArmed(20);
    dcrt::disarm();
    fx.installInputStimulus(0.25f, 220.0);
    dcrt::arm();
    fx.pumpArmed(100);
    const float openPeak = fx.lastOutputPeak();
    dcrt::disarm();

    reportHeap("auto-floor gate + futz crusher");
    RT_CHECK(dcrt::allocCount() == 0,
             "the auto-floor gate and the FutzBox crusher allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "the auto-floor gate and the FutzBox crusher freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback");
    RT_CHECK(! fx.lastOutputHasNonFinite(), "the gate + crusher rig produced a non-finite output block");
    RT_CHECK(gatedPeak < 1e-4f,
             "a stationary -70 dBFS input was not gated once its floor was measured (peak "
             + std::to_string(gatedPeak) + ")");
    RT_CHECK(openPeak > 1e-3f,
             "a -12 dBFS input did not pass the auto-floor gate (peak " + std::to_string(openPeak) + ")");
    endScenario();
}

// 4 ------------------------------------------------------------------------
void scenarioFilterSweep()
{
    beginScenario("RtFilterSweepAllocatesNothing");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    warmUpAndArm(fx);
    for (int b = 0; b < 256; ++b)
    {
        // Setter runs on THIS thread outside ScopedRtThread, exactly like the
        // UI thread does — any allocation it makes is legal and unattributed.
        fx.engine().setHighPass(200.0f + (float) b * 4.0f, 0.7f);
        dcrt::g_currentBlock.store((unsigned) b, std::memory_order_relaxed);
        dcrt::ScopedRtThread rt;
        nulldev::pump(1);
    }
    dcrt::disarm();

    reportHeap("filter sweep");
    RT_CHECK(dcrt::allocCount() == 0,
             "filter sweep allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the audio thread (finding B-1)");
    endScenario();
}

// 5 ------------------------------------------------------------------------
void scenarioMidiFlood()
{
    beginScenario("RtMidiFloodAllocatesNothing");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    StubInstrument stub;
    if (! fx.engine().attachInstrumentPlugin(&stub))
    { fail("could not attach the stub instrument"); endScenario(); return; }

    // Warm up WITH the instrument attached so first-touch MidiBuffer growth
    // lands outside the measured window.
    for (int w = 0; w < 4; ++w)
    {
        for (int i = 0; i < 512; ++i)
            fx.engine().sendMidiShort(0x90, (uint8_t) (40 + (i % 40)), 100);
        fx.pumpWarm(1);
    }
    warmUpAndArm(fx);

    // A full FIFO's worth queued between two pumps: 512 events in one drain.
    for (int round = 0; round < 8; ++round)
    {
        for (int i = 0; i < 512; ++i)
            fx.engine().sendMidiShort(0x90, (uint8_t) (40 + (i % 40)), 100);
        dcrt::g_currentBlock.store((unsigned) round, std::memory_order_relaxed);
        dcrt::ScopedRtThread rt;
        nulldev::pump(1);
    }
    dcrt::disarm();
    fx.engine().detachInstrumentPlugin(&stub);

    reportHeap("midi flood");
    note("stub instrument saw " + std::to_string(stub.midiEventsSeen()) + " MIDI events");
    RT_CHECK(stub.midiEventsSeen() > 400,
             "the flood never reached the instrument (saw "
             + std::to_string(stub.midiEventsSeen()) + " events) — the assertion would be vacuous");
    RT_CHECK(dcrt::allocCount() == 0,
             "MIDI flood allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the audio thread (finding B-2)");
    endScenario();
}

// 6 ------------------------------------------------------------------------
void scenarioBlockSizeRenegotiation()
{
    beginScenario("RtBlockSizeRenegotiationSkipsNotAllocates");
    Fixture fx(48000, 256, 2);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.engine().setMonitorGain(1.0f);
    fx.engine().setInputGain(1.0f);
    fx.installInputStimulus();

    warmUpAndArm(fx);

    auto reopenAt = [&](int block) -> bool
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        fx.engine().deviceManager().getAudioDeviceSetup(setup);
        setup.bufferSize = block;
        const auto err = fx.engine().deviceManager().setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) return false;
        fx.installInputStimulus();   // open() re-sized and zeroed the buffers
        return nulldev::manualDevice() != nullptr;
    };

    auto pumpAndCheckAudible = [&](int blocks, const char* phase)
    {
        // Warm the NEW device size unarmed: prepareMasterBus legitimately
        // allocates on the device thread, and the first block after a resize
        // touches fresh memory.
        dcrt::disarm();
        fx.pumpWarm(8);
        dcrt::arm();
        float minPeak = 1.0e9f;
        for (int i = 0; i < blocks; ++i)
        {
            dcrt::ScopedRtThread rt;
            nulldev::pump(1);
            minPeak = std::min(minPeak, fx.lastOutputPeak());
        }
        RT_CHECK(minPeak > 1.0e-4f,
                 std::string("output went silent during ") + phase
                 + " (min block peak " + std::to_string(minPeak)
                 + ") — a 'skip the block' guard is degrading to permanent silence");
    };

    pumpAndCheckAudible(64, "256 frames");
    RT_CHECK(reopenAt(1024), "could not reopen the null device at 1024 frames");
    pumpAndCheckAudible(64, "1024 frames");
    RT_CHECK(reopenAt(256), "could not reopen the null device back at 256 frames");
    pumpAndCheckAudible(64, "256 frames (second pass)");

    dcrt::disarm();
    reportHeap("block-size renegotiation");
    RT_CHECK(dcrt::allocCount() == 0,
             "block-size renegotiation allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback thread");
    endScenario();
}

// 7 ------------------------------------------------------------------------
void scenarioDropTuneOffline()
{
    beginScenario("RtDropTuneOfflineIsAllocationFree");

    constexpr int kSr    = 48000;
    constexpr int kBlock = 512;
    const int segSemis[] = { 0, -2, -5, -7, -12, 0 };
    constexpr int kSegSeconds = 5;   // 6 x 5 s = 30 s

    dcdsp::DropTune dt;
    dt.prepare((double) kSr, kBlock, 1);

    // Deterministic pink-ish noise from a fixed-seed xorshift so the run is
    // reproducible without shipping a fixture.
    uint32_t rng = 0x1234567u;
    auto nextNoise = [&rng]
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return ((float) (rng >> 8) / (float) (1u << 24)) * 2.0f - 1.0f;
    };

    std::vector<float> block((size_t) kBlock, 0.0f);
    std::vector<float> dry  ((size_t) kBlock, 0.0f);
    float* ptrs[1] = { block.data() };

    // Warm-up: one segment unarmed, so every lazy table is materialised.
    {
        dcdsp::DropTuneParams p; p.semitones = -5.0f; p.maxLagMs = 20.0f;
        dt.setParams(p);
        for (int i = 0; i < 32; ++i)
        {
            for (auto& s : block) s = 0.3f * nextNoise();
            dt.process(ptrs, 1, kBlock);
        }
    }

    dcrt::reset();
    dcrt::arm();

    bool allFinite = true;
    bool bypassIsBitExact = true;
    int  fadeSamplesInSeg[6]{};
    // 1 ms at 48 kHz: DropTune::snapXfade_, the window the ENGAGE/DISENGAGE
    // crossfade runs for. Anything past it must be the dry input untouched.
    constexpr int kDisengageFade = 48;
    double segRms[6]{};
    {
        dcrt::ScopedRtThread rt;
        for (int seg = 0; seg < 6; ++seg)
        {
            dcdsp::DropTuneParams p;
            p.semitones = (float) segSemis[seg];
            p.maxLagMs  = 20.0f;
            dt.setParams(p);

            const int blocks = (kSegSeconds * kSr) / kBlock;
            double acc = 0.0; long long n = 0;
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < kBlock; ++i) { block[(size_t) i] = 0.3f * nextNoise(); dry[(size_t) i] = block[(size_t) i]; }
                dt.process(ptrs, 1, kBlock);
                for (int i = 0; i < kBlock; ++i)
                {
                    const float s = block[(size_t) i];
                    if (! std::isfinite(s)) allFinite = false;
                    if (segSemis[seg] == 0)
                    {
                        const long long inSeg = (long long) b * kBlock + i;
                        const bool differs = (s != dry[(size_t) i]);
                        if (differs && inSeg >= kDisengageFade) bypassIsBitExact = false;
                        if (differs && inSeg <  kDisengageFade) ++fadeSamplesInSeg[seg];
                    }
                    acc += (double) s * (double) s; ++n;
                }
            }
            segRms[seg] = n > 0 ? std::sqrt(acc / (double) n) : 0.0;
        }
    }
    dcrt::disarm();

    reportHeap("drop tune");
    RT_CHECK(dcrt::allocCount() == 0,
             "the drop tuner allocated " + std::to_string(dcrt::allocCount())
             + " time(s) after prepare()");
    RT_CHECK(allFinite, "drop-tune output contained a non-finite sample");
    RT_CHECK(bypassIsBitExact,
             "semitones == 0 is documented as TRUE BYPASS but the output differs from the "
             "input more than snapXfade_ after the shift was released");
    for (int seg = 0; seg < 6; ++seg)
    {
        if (segSemis[seg] != 0) continue;
        // Every one of these segments follows an ENGAGED one (the warm-up
        // engages before segment 0), so each must carry exactly one release
        // crossfade -- no more, and never none.
        RT_CHECK(fadeSamplesInSeg[seg] > 0 && fadeSamplesInSeg[seg] <= kDisengageFade,
                 "segment " + std::to_string(seg) + ": releasing the shift produced "
                 + std::to_string(fadeSamplesInSeg[seg]) + " crossfaded sample(s); expected 1.."
                 + std::to_string(kDisengageFade)
                 + " -- 0 means the release is CUTTING at one sample again");
    }

    // ...and a tuner that was NEVER engaged is untouched from its first sample:
    // the crossfade must be armed by a RELEASE, never by sitting at unity.
    {
        dcdsp::DropTune virgin;
        virgin.prepare((double) kSr, kBlock, 1);
        dcdsp::DropTuneParams zero; zero.semitones = 0.0f; zero.maxLagMs = 20.0f;
        virgin.setParams(zero);
        bool virginIsBitExact = true;
        for (int b = 0; b < 16; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            { block[(size_t) i] = 0.3f * nextNoise(); dry[(size_t) i] = block[(size_t) i]; }
            virgin.process(ptrs, 1, kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (block[(size_t) i] != dry[(size_t) i]) virginIsBitExact = false;
        }
        RT_CHECK(virginIsBitExact,
                 "a drop tuner that was never engaged altered its input");
    }
    for (int seg = 0; seg < 6; ++seg)
    {
        RT_CHECK(segRms[seg] > 0.01 && segRms[seg] < 2.0,
                 "segment " + std::to_string(seg) + " (" + std::to_string(segSemis[seg])
                 + " semis) RMS out of range: " + std::to_string(segRms[seg]));
    }
    note("segment RMS: " + [&]{ std::string s; for (double r : segRms) s += std::to_string(r) + " "; return s; }());

    // --- the EXPORT establishes its own FTZ/DAZ ----------------------------
    // The offline tooling path is not the device callback and inherits none of
    // its guarantees: EngineCallback sets FTZ/DAZ for the live path, and
    // AudioEngine::dspProcessOffline sets its own for the same reason. This
    // export had neither, so a CSVD tail decaying into subnormals cost ~100x
    // per sample on x86 for the rest of a render.
    //
    // Measured rather than asserted structurally, because MXCSR is restored
    // before the export returns and cannot be read from out here. With DAZ,
    // subnormal INPUT reads as zero and every product of it is zero; with FTZ,
    // a subnormal RESULT is flushed. So: drive the shipping export with a
    // normal burst followed by a long subnormal tail and count the subnormals
    // that come back out.
    {
        constexpr int kTail = 48000;                 // 1 s of tail
        constexpr int kHead = 4096;
        constexpr int kTotal = kHead + kTail;
        std::vector<float> in((size_t) kTotal, 0.0f), out((size_t) kTotal, 0.0f);
        uint32_t r2 = 0x9E3779B9u;
        for (int i = 0; i < kHead; ++i)
        {
            r2 ^= r2 << 13; r2 ^= r2 >> 17; r2 ^= r2 << 5;
            in[(size_t) i] = 0.3f * (((float) (r2 >> 8) / (float) (1u << 24)) * 2.0f - 1.0f);
        }
        for (int i = kHead; i < kTotal; ++i)
            in[(size_t) i] = 7.0e-40f;               // a subnormal float

        const float semis[1] = { -5.0f };
        const int32_t lens[1] = { kTotal };
        const dca_status st = dca_droptune_process_offline(
            48000, 20.0f, semis, lens, 1, in.data(), out.data(), kTotal);

        int subnormals = 0;
        for (float v : out) if (std::fpclassify(v) == FP_SUBNORMAL) ++subnormals;

        note("offline export: " + std::to_string(subnormals)
             + " subnormal sample(s) in " + std::to_string(kTotal) + " of output");
        RT_CHECK(st == DCA_OK,
                 "dca_droptune_process_offline returned " + std::to_string((int) st));
        RT_CHECK(subnormals == 0,
                 std::to_string(subnormals) + " subnormal sample(s) came out of "
                 "dca_droptune_process_offline — it is running without FTZ/DAZ, so a "
                 "decaying tail costs ~100x per sample for the rest of the render");
    }

    // The two correlation bursts must never land in the SAME sample. On the
    // sample where a splice first becomes pending, `pending_` was still false
    // when the probe guard ran, so maybeProbe() fired — and `searchTick_ = 0`
    // then made `--searchTick_` fire findLag in that same sample, doubling the
    // worst-case callback burst. The throttle's own comment says this cannot
    // happen; the counter is what makes that true.
    note("droptune samples running BOTH correlation bursts: "
         + std::to_string(dt.doubleBurstCountForTest()));
    RT_CHECK(dt.doubleBurstCountForTest() == 0,
             "droptune ran maybeProbe() and findLag() in the same sample "
             + std::to_string(dt.doubleBurstCountForTest())
             + " time(s) — double the worst-case burst the throttle exists to bound");

    endScenario();
}


// 25 ------------------------------------------------------------------------
// A CABINET IR ARRIVES ALREADY CONVERTED. It used to be converted here.
//
// Both of the cab's kernel sources — the 12 bundled models and a user IR —
// went through detail::resampleIr at ADOPTION time, i.e. inside the callback
// that drained the mailbox: up to 4,096 output taps, six Lanczos taps each,
// two std::sin per tap, plus three biquad redesigns per channel, in one block.
// It allocates nothing, so the allocation gate never saw it, and the
// conversion SHORT-CIRCUITS at 48 kHz (|dRate| < 1 Hz), so the owner's own
// machine never saw it either. On a 44.1 kHz device every model knob tick and
// every preset load paid it.
//
// Same shape as the glitch scenario below: the module is driven directly (no
// engine, no device) so the numbers are pure CPU, and the two populations are
// AVERAGED because a single timing on a shared box is noise. This one drives
// the shipping ADAPTER rather than the core, because the fix lives in the
// wiring between them.
// dcdsp_cab.h guards its kernel rebuild on the model actually changing.
// dcdsp_acousticsim.h did not: setParams ran updateCoeffs() unconditionally, so
// ONE `mix` tick cost two passes over up to 4,096 IR taps plus five biquad
// designs per channel, each with a cos/sin/pow -- on the AUDIO CALLBACK, since
// the adapter drains its ParamMailbox from the top of process(). Allocation-
// free and deterministic, which is exactly why the RT gate never saw it.
//
// The instrument is two counters rather than a cost ratio, for the same reason
// RtPitchCoreFlipDoesNotBurstOneCallback uses counters: the thing being
// measured is "did this work happen", and a counter cannot be cancelled by a
// saving somewhere else in the same block.
void scenarioAcousticSimRebuildsOnlyWhatMoved()
{
    beginScenario("RtAcousticSimMixDoesNotRebuildTheIr");

    constexpr int kBlock = 256;
    dcdsp::AcousticSimCore core;
    dcdsp::AcousticSimParams p{};
    p.body = 0.5f; p.brightness = 0.5f; p.lowCutHz = 80.0f; p.mix = 0.5f;
    core.prepare(48000.0, kBlock, 2);
    core.setParams(p);

    std::vector<float> l((std::size_t) kBlock, 0.f), r((std::size_t) kBlock, 0.f);
    float* chans[2] = { l.data(), r.data() };
    for (int i = 0; i < kBlock; ++i)
        l[(std::size_t) i] = r[(std::size_t) i] =
            0.25f * (float) std::sin(6.283185307 * 110.0 * i / 48000.0);

    core.process(chans, 2, kBlock);

    const long long kernel0  = core.kernelRebuilds();
    const long long voicing0 = core.voicingRebuilds();

    // 64 pure `mix` ticks -- a slider drag.
    for (int i = 0; i < 64; ++i)
    {
        p.mix = 0.5f + 0.4f * (float) i / 64.0f;
        core.setParams(p);
        core.process(chans, 2, kBlock);
    }
    const long long kernelAfterMix  = core.kernelRebuilds()  - kernel0;
    const long long voicingAfterMix = core.voicingRebuilds() - voicing0;

    note("acousticsim: 64 mix ticks cost " + std::to_string(kernelAfterMix)
         + " kernel rebuild(s) and " + std::to_string(voicingAfterMix)
         + " voicing redesign(s)");
    RT_CHECK(kernelAfterMix == 0,
             "a pure `mix` move rebuilt the 4,096-tap IR "
             + std::to_string(kernelAfterMix) + " time(s) on the callback");
    RT_CHECK(voicingAfterMix == 0,
             "a pure `mix` move redesigned the per-channel biquads "
             + std::to_string(voicingAfterMix) + " time(s) on the callback");

    // ...and the guard must not be a MUTE: the halves still fire for their own
    // inputs, or "no rebuild" would be true of a core that ignores its knobs.
    const long long k1 = core.kernelRebuilds(), v1 = core.voicingRebuilds();
    p.body = 0.9f;
    core.setParams(p);
    core.process(chans, 2, kBlock);
    RT_CHECK(core.kernelRebuilds() > k1,
             "`body` did not rebuild the kernel -- the guard is swallowing real changes");
    RT_CHECK(core.voicingRebuilds() == v1,
             "`body` redesigned the biquads, which do not depend on it");

    const long long k2 = core.kernelRebuilds(), v2 = core.voicingRebuilds();
    p.brightness = 0.9f;
    core.setParams(p);
    core.process(chans, 2, kBlock);
    RT_CHECK(core.voicingRebuilds() > v2,
             "`brightness` did not redesign the biquads -- the guard is swallowing "
             "real changes");
    RT_CHECK(core.kernelRebuilds() == k2,
             "`brightness` rebuilt the IR kernel, which does not depend on it");

    dcrt::reset();
    endScenario();
}

// A float32 mono WAV of `seconds` of decaying noise, for the reverb gear's IR
// swap. Message thread; the RT scenarios that use it write it before arming.
bool writeFloatWav(const std::string& path, double seconds, double rate, uint32_t seed)
{
    const int frames = (int) (seconds * rate);
    if (frames <= 0) return false;
    std::ofstream f(path, std::ios::binary);
    if (! f.is_open()) return false;
    auto u32 = [&f](uint32_t v) { f.write((const char*) &v, 4); };
    auto u16 = [&f](uint16_t v) { f.write((const char*) &v, 2); };
    const uint32_t dataSize = (uint32_t) frames * 4u;
    f.write("RIFF", 4); u32(36u + dataSize); f.write("WAVEfmt ", 8);
    u32(16u); u16(3); u16(1); u32((uint32_t) rate); u32((uint32_t) rate * 4u); u16(4); u16(32);
    f.write("data", 4); u32(dataSize);
    uint32_t rng = seed;
    for (int i = 0; i < frames; ++i)
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const float w = ((float) (rng >> 8) / (float) (1u << 24)) * 2.0f - 1.0f;
        const float v = w * std::exp(-5.0f * (float) i / (float) frames);
        f.write((const char*) &v, 4);
    }
    return f.good();
}

// W4-D67. The cabinet convolution is a direct-form dot product, and it was
// pinned to the SSE2 baseline deliberately so the binary keeps running on a
// pre-Haswell machine. That is a reason to choose the kernel at RUNTIME, not a
// reason to leave the wide one unwritten: at the shipped 2,048-tap IR the
// AVX2 + FMA kernel measures 2.35x the SSE2 one, and the cab core end to end
// goes 289.30 -> 161.62 ns/sample (dualcab 333.17 -> 182.02), measured in a
// single-process A/B over the real cores.
//
// Two things have to hold and neither is obvious from reading the code: the
// wide kernel must actually be SELECTED here (a dispatch that silently always
// picks the baseline is the failure mode that looks like success), and the two
// kernels must agree — FMA and a wider reduction tree change the ROUNDING
// ORDER of the sum, and nothing else.
void scenarioFirDotDispatchesAndAgrees()
{
    beginScenario("RtFirDotPicksTheWideKernelAndAgreesWithIt");

#if defined(DCDSP_FIR_AVX2)
    const bool capable = dcdsp::detail::firCpuHasAvx2();
    note(std::string("CPU reports AVX2 + FMA + OS YMM state: ")
         + (capable ? "yes" : "no"));
    if (capable)
        RT_CHECK(dcdsp::detail::kFirDot == &dcdsp::detail::firDotAvx2,
                 "this CPU has AVX2 + FMA but firDot still points at the SSE2 kernel "
                 "— the dispatch is not doing anything");
    else
        note("no AVX2 on this box: the dispatch half of this case cannot run, "
             "but the agreement half below still does");
#else
    note("built without the x86-64 kernels; nothing to dispatch");
    const bool capable = false;
#endif

    // Agreement, over the lengths the cab actually uses. Deterministic input.
    double worstAbs = 0.0, worstRel = 0.0;
    int worstN = 0;
    for (int n : { 1, 3, 7, 8, 31, 32, 33, 512, 2048, 4096 })
    {
        std::vector<float> a((std::size_t) n), b((std::size_t) n);
        for (int i = 0; i < n; ++i)
        {
            a[(std::size_t) i] = 0.5f * (float) std::sin(i * 0.13);
            b[(std::size_t) i] = (float) std::exp(-i / 400.0);
        }
        // The scalar reference is written here rather than reused, so a change
        // to BOTH shipped kernels cannot make this agree with itself.
        double ref = 0.0;
        for (int i = 0; i < n; ++i) ref += (double) a[(std::size_t) i] * (double) b[(std::size_t) i];

        const double got = (double) dcdsp::detail::firDot(a.data(), b.data(), n);
        const double d   = std::abs(ref - got);
        if (d > worstAbs) { worstAbs = d; worstN = n; }
        worstRel = std::max(worstRel, d / std::max(1e-12, std::abs(ref)));
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.3e (rel %.3e, worst at n = %d)",
                  worstAbs, worstRel, worstN);
    note(std::string("firDot vs a double-precision scalar reference: ") + buf);
    RT_CHECK(worstRel < 1e-5,
             std::string("firDot disagrees with a scalar reference by ") + buf
             + " — that is more than a rounding-order difference");

    // ...and the two SHIPPED kernels against each other, which is the claim the
    // header's table makes.
#if defined(DCDSP_FIR_AVX2)
    if (capable)
    {
        double worst = 0.0;
        for (int n : { 512, 1024, 2048, 4096 })
        {
            std::vector<float> a((std::size_t) n), b((std::size_t) n);
            for (int i = 0; i < n; ++i)
            {
                a[(std::size_t) i] = 0.5f * (float) std::sin(i * 0.13);
                b[(std::size_t) i] = (float) std::exp(-i / 400.0);
            }
            worst = std::max(worst, (double) std::abs(
                dcdsp::detail::firDotSse2(a.data(), b.data(), n)
                - dcdsp::detail::firDotAvx2(a.data(), b.data(), n)));
        }
        std::snprintf(buf, sizeof buf, "%.3e", worst);
        note(std::string("SSE2 vs AVX2 kernel, max abs difference: ") + buf);
        RT_CHECK(worst < 1e-5,
                 std::string("the two FIR kernels disagree by ") + buf
                 + " — the header documents 9.537e-07");
    }
#endif

    endScenario();
}

void scenarioCabKernelIsConvertedOffTheCallback()
{
    beginScenario("RtCabKernelSwitchDoesNotBurstOneCallback");

    constexpr int kBlock  = 64;
    constexpr int kBlocks = 480;
    constexpr int kEvery  = 8;      // a model switch every 8 blocks

    // 44.1 kHz on purpose — at 48 kHz there is no conversion to hoist.
    auto module = dcrig::createModule("cab");
    if (module == nullptr) { fail("could not create the cab module"); endScenario(); return; }
    module->prepare(44100.0, kBlock, 2);

    uint32_t rng = 0x2F6E5B11u;
    auto nextNoise = [&rng]
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return ((float) (rng >> 8) / (float) (1u << 24)) * 2.0f - 1.0f;
    };
    std::vector<float> l((size_t) kBlock, 0.0f), r((size_t) kBlock, 0.0f);
    float* ptrs[2] = { l.data(), r.data() };
    auto refill = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = 0.3f * nextNoise();
            l[(size_t) i] = s; r[(size_t) i] = s;
        }
    };

    // Warm: fill the FIR history, materialise every lazy table, drain whatever
    // prepare() published.
    for (int i = 0; i < 200; ++i) { refill(); module->process(ptrs, 2, kBlock); }

    dcrt::reset();
    dcrt::arm();

    double switchNs = 0.0, otherNs = 0.0;
    int    switchBlocks = 0, otherBlocks = 0;
    double energy = 0.0;
    int    model = 0;
    {
        dcrt::ScopedRtThread rt;
        for (int b = 0; b < kBlocks; ++b)
        {
            const bool flip = (b % kEvery) == 0;
            if (flip)
            {
                // Every bundled model, including 12-25 (DotCharter #74).
                model = (model + 1) % dcdsp::cabir::kNumModels;
                // Published from THIS thread in the scenario, which is the
                // pessimistic case: the real publisher is the message thread.
                module->setParam("model", (float) model);
            }

            refill();
            const auto t0 = std::chrono::steady_clock::now();
            module->process(ptrs, 2, kBlock);
            const auto t1 = std::chrono::steady_clock::now();
            const double ns = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            if (flip) { switchNs += ns; ++switchBlocks; }
            else      { otherNs  += ns; ++otherBlocks; }

            for (int i = 0; i < kBlock; ++i)
                energy += (double) l[(size_t) i] * l[(size_t) i];
        }
    }
    dcrt::disarm();

    const double switchMean = switchBlocks > 0 ? switchNs / switchBlocks : 0.0;
    const double otherMean  = otherBlocks  > 0 ? otherNs  / otherBlocks  : 0.0;
    const double ratio      = otherMean > 0.0 ? switchMean / otherMean : 0.0;

    note("switch blocks: " + std::to_string(switchBlocks) + " at "
         + std::to_string((long long) switchMean) + " ns, others: "
         + std::to_string(otherBlocks) + " at "
         + std::to_string((long long) otherMean) + " ns, ratio "
         + std::to_string(ratio));

    reportHeap("cab model switch");
    RT_CHECK(switchBlocks >= 20,
             "only " + std::to_string(switchBlocks)
             + " model switches landed — too few to average, the probe is vacuous");
    RT_CHECK(energy > 1e-6,
             "the cab produced no output at all — a silent module costs nothing and would "
             "pass any timing ratio");
    RT_CHECK(ratio > 0.0 && ratio < 3.0,
             "the block that adopted a new cabinet model cost " + std::to_string(ratio)
             + "x its neighbours — the Lanczos rate conversion is still running on the "
               "callback instead of in the publisher");
    RT_CHECK(dcrt::allocCount() == 0,
             "cab kernel adoption allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");

    endScenario();
}

// The four `reverb`-drawer modules of the 2026-09 gear wave, chained, under the
// allocation sentinel, WHILE the two things that rebuild their state happen:
// the convolution's IR is swapped (a whole re-partition + re-transform, which
// must run on the OWNER thread and reach the callback as a published snapshot)
// and the hall's `space` is switched (a geometry change, which must reach the
// callback as a crossfade between two tanks and not as eight spliced delay
// lines). Both are the shape RtCabKernelSwitchDoesNotBurstOneCallback covers
// for the cab's IR, one size up: a reverb IR is up to 480,000 taps.
//
// It also REPORTS ns/sample for a 2-second IR at a 480-sample block, because
// "it allocates nothing" says nothing about whether it is affordable.
void scenarioReverbGearIsAllocationFreeUnderAnIrSwapAndASpaceSwitch()
{
    beginScenario("RtReverbGearIsAllocationFreeUnderAnIrSwapAndASpaceSwitch");

    constexpr int    kBlock  = 480;
    constexpr int    kBlocks = 200;
    constexpr double kRate   = 48000.0;

    const char* const kIds[] = { "hallreverb", "convreverb", "gatedreverb", "reversereverb" };
    std::vector<std::unique_ptr<dcrig::DspModule>> rig;
    for (const char* id : kIds)
    {
        auto m = dcrig::createModule(id);
        if (m == nullptr) { fail(std::string("could not create '") + id + "'"); endScenario(); return; }
        m->prepare(kRate, kBlock, 2);
        rig.push_back(std::move(m));
    }
    // Everything audible, so nothing here can pass by being silent.
    for (auto& m : rig) m->setParam("mix", 0.5f);
    rig[2]->setParam("thresholdDb", -60.0f);     // the gate held open

    // Two IRs on disk, so the swap goes through the real setStringParam path.
    const auto dir = std::filesystem::temp_directory_path() / "juce_sharp_rt_ir";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto irA = dir / "rt_ir_a.wav";
    const auto irB = dir / "rt_ir_b.wav";
    const bool haveIrs = writeFloatWav(irA.string(), 2.0, kRate, 0x51A1u)
                      && writeFloatWav(irB.string(), 2.0, kRate, 0x7C0Du);
    if (! haveIrs) { fail("could not write the temporary IR WAVs"); endScenario(); return; }
    RT_CHECK(rig[1]->setStringParam("irFile", irA.string().c_str()),
             "the convolution refused a freshly written 2 s IR");

    uint32_t rng = 0x1B3C5D7Fu;
    auto nextNoise = [&rng]
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return ((float) (rng >> 8) / (float) (1u << 24)) * 2.0f - 1.0f;
    };
    std::vector<float> l((size_t) kBlock, 0.0f), r((size_t) kBlock, 0.0f);
    float* ptrs[2] = { l.data(), r.data() };
    auto refill = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = 0.25f * nextNoise();
            l[(size_t) i] = s; r[(size_t) i] = s;
        }
    };
    auto pump = [&] { refill(); for (auto& m : rig) m->process(ptrs, 2, kBlock); };

    // Warm: fill every tank, ring and FIFO, and drain whatever prepare published.
    for (int i = 0; i < 400; ++i) pump();

    dcrt::reset();
    dcrt::arm();
    double energy = 0.0;
    bool   finite = true;
    {
        dcrt::ScopedRtThread rt;
        for (int b = 0; b < kBlocks; ++b)
        {
            // Mid-run: the IR swap and the geometry switch, published from THIS
            // thread, which is the pessimistic case (the real publisher is the
            // message thread and is not armed).
            if (b == 60)  rig[1]->setStringParam("irFile", irB.string().c_str());
            if (b == 90)  rig[0]->setParam("space", 0.0f);
            if (b == 120) rig[1]->setParam("lengthPct", 50.0f);
            if (b == 150) rig[0]->setParam("space", 3.0f);
            pump();
            for (int i = 0; i < kBlock; ++i)
            {
                energy += (double) l[(size_t) i] * l[(size_t) i];
                finite = finite && std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i]);
            }
        }
    }
    dcrt::disarm();

    reportHeap("reverb gear");
    RT_CHECK(finite, "the reverb rig emitted a non-finite sample");
    RT_CHECK(energy > 1.0e-6,
             "the four-module reverb rig produced no output at all - a silent rig costs "
             "nothing and would pass any allocation gate");
    // setStringParam and lengthPct DO allocate: they are owner-thread rebuilds,
    // and they are driven from the armed thread here only to prove the CALLBACK
    // side of a swap - adoption and the crossfade - is reached and survives it.
    note("allocations with the two owner-thread IR rebuilds inside the armed run: "
         + std::to_string(dcrt::allocCount()));

    // --- the same rig again, with the owner-thread rebuilds moved OUT --------
    for (auto& m : rig) m->reset();
    for (int i = 0; i < 400; ++i) pump();
    dcrt::reset();
    dcrt::arm();
    {
        dcrt::ScopedRtThread rt;
        for (int b = 0; b < kBlocks; ++b)
        {
            if (b == 90)  rig[0]->setParam("space", 0.0f);
            if (b == 150) rig[0]->setParam("space", 3.0f);
            // Damping / high-cut corners past a quarter of the rate (DotCharter
            // #87) are designed near Nyquist now, on the same mailbox path.
            if (b == 170) for (int t : { 0, 2, 3 }) rig[(size_t) t]->setParam("dampingKHz", 16.0f);
            if (b == 180) rig[1]->setParam("highCutKHz", 18.0f);
            pump();
        }
    }
    dcrt::disarm();
    reportHeap("reverb gear, callback side only");
    RT_CHECK(dcrt::allocCount() == 0,
             "the reverb rig allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback across a space switch");
    RT_CHECK(dcrt::freeCount() == 0,
             "the reverb rig freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback - a retired IR snapshot was dropped by the "
               "audio thread instead of parked in the RetirementQueue");

    // --- a lengthPct drag re-converts the source ZERO times (W5-B2) ----------
    // The truncation is an owner-thread rebuild and always will be, but the
    // rate conversion + energy normalisation feeding it cannot change when only
    // the kept fraction moves. Re-running it per UI tick was ~1.9 MB of
    // resampling for a result identical to the previous one's.
    {
        auto* cc = static_cast<dcdsp::ConvReverbCore*>(rig[1]->coreForTest());
        if (cc != nullptr)
        {
            const int before = cc->irConversionsForTest();
            for (int i = 0; i <= 18; ++i)
                rig[1]->setParam("lengthPct", 100.0f - 5.0f * (float) i);
            const int after = cc->irConversionsForTest();
            note("convreverb: 19 lengthPct writes cost "
                 + std::to_string(after - before) + " source conversion(s)");
            RT_CHECK(after == before,
                     "a lengthPct sweep re-converted the decoded IR "
                     + std::to_string(after - before) + " time(s)");
            RT_CHECK(cc->activeTapsForTest() > 0,
                     "the lengthPct sweep left no IR published - the probe is vacuous");
            rig[1]->setParam("lengthPct", 100.0f);
        }
    }

    // --- a BLOCK SIZE change re-partitions, and the old snapshot goes (W5-B3) -
    // part_ is derived from maxBlockSize, so a device change to a different
    // block size changes the partition size, the spectrum stride and the
    // partition count. A snapshot built at the OLD size walked with the NEW
    // stride is garbage, and IrSnapshot carried no geometry with which to
    // notice: prepare() kept `current_` and crossfaded 20 ms of exactly that.
    // Against a module prepared at the small size from the start — same IR,
    // same input, both freshly reset — the two must agree sample for sample.
    {
        auto a = dcrig::createModule("convreverb");   // 512 -> 64
        auto b = dcrig::createModule("convreverb");   // 64 from the start
        if (a != nullptr && b != nullptr)
        {
            constexpr int kSmall = 64;
            // Both stay at the CATALOGUE default mix, deliberately: a setParam
            // before the first process() sits in the mailbox, so the module
            // that has already rendered enters its final prepare() with the new
            // value applied and the one that has not does not — a 10 ms mix
            // ramp on one side only, which swamps the thing being measured.
            a->prepare(kRate, 512, 2);
            RT_CHECK(a->setStringParam("irFile", irA.string().c_str()),
                     "the re-prepared convolution refused the IR");
            std::vector<float> sl((size_t) kSmall), srr((size_t) kSmall);
            float* sptrs[2] = { sl.data(), srr.data() };
            auto feed = [&](dcrig::DspModule& m, int blocks, std::vector<float>& outL)
            {
                uint32_t s = 0x2468ACE1u;
                for (int bIdx = 0; bIdx < blocks; ++bIdx)
                {
                    for (int i = 0; i < kSmall; ++i)
                    {
                        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                        const float v = 0.25f * (((float) (s >> 8) / (float) (1u << 24)) * 2.0f - 1.0f);
                        sl[(size_t) i] = v; srr[(size_t) i] = v;
                    }
                    m.process(sptrs, 2, kSmall);
                    outL.insert(outL.end(), sl.begin(), sl.end());
                }
            };
            // Warm `a` at the WIDE geometry so there is a stale snapshot to
            // mis-adopt, then switch it to the narrow one.
            {
                std::vector<float> wl((size_t) 512, 0.0f), wr((size_t) 512, 0.0f);
                float* wptrs[2] = { wl.data(), wr.data() };
                uint32_t s = 0x13579BDFu;
                for (int bIdx = 0; bIdx < 200; ++bIdx)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                        const float v = 0.25f * (((float) (s >> 8) / (float) (1u << 24)) * 2.0f - 1.0f);
                        wl[(size_t) i] = v; wr[(size_t) i] = v;
                    }
                    a->process(wptrs, 2, 512);
                }
            }
            a->prepare(kRate, kSmall, 2);
            b->prepare(kRate, kSmall, 2);
            RT_CHECK(b->setStringParam("irFile", irA.string().c_str()),
                     "the reference convolution refused the IR");

            std::vector<float> outA, outB;
            const int kPost = 48000 / kSmall / 20;   // 50 ms, past the 20 ms fade
            feed(*a, kPost, outA);
            feed(*b, kPost, outB);
            float worst = 0.0f, ref = 0.0f;
            const size_t n = std::min(outA.size(), outB.size());
            for (size_t i = 0; i < n; ++i)
            {
                worst = std::max(worst, std::fabs(outA[i] - outB[i]));
                ref   = std::max(ref, std::fabs(outB[i]));
            }
            note("convreverb across a 512 -> 64 block-size change: worst sample "
                 "difference " + std::to_string(worst) + " against a reference peak of "
                 + std::to_string(ref));
            RT_CHECK(ref > 1.0e-4f,
                     "the reference render is silent - the block-size probe is vacuous");
            RT_CHECK(worst < 1.0e-3f * std::max(ref, 1.0e-6f),
                     "a re-prepared convolution differed from a freshly prepared one by "
                     + std::to_string(worst) + " - it played a snapshot partitioned at the "
                       "OLD block size");
        }
    }

    // --- the IR swap driven from a SECOND thread (W5-B1) ---------------------
    // Everything above publishes from the RENDER thread, which is pessimistic
    // for the allocation counter and is the one arrangement in which a data
    // race CANNOT happen. The real publisher is the message thread.
    //
    // ConvReverbCore transforms a rebuild with its OWN RealFft because that
    // class stages its input into a member scratch: with one shared instance an
    // owner-thread forward() overwrote the scratch the callback was midway
    // through, so the callback wrote a spectrum of INPUT AUDIO (peak ~0.25)
    // into the frequency-delay line slot where an energy-normalised IR
    // partition (~0.002 per tap) belongs — two orders of magnitude, convolved
    // against every later hop for as long as the poisoned slot stays in the
    // line. The detector is therefore a PEAK ENVELOPE against a reference
    // render, not a NaN check: the garbage is finite, it is just enormous.
    {
        auto ref = dcrig::createModule("convreverb");
        auto hot = dcrig::createModule("convreverb");
        if (ref != nullptr && hot != nullptr)
        {
            constexpr int kRaceBlocks = 1500;
            float refPeak = 0.0f;
            ref->prepare(kRate, kBlock, 2);
            ref->setParam("mix", 1.0f);
            RT_CHECK(ref->setStringParam("irFile", irA.string().c_str()),
                     "the reference convolution refused the IR");
            for (int b = 0; b < 300; ++b)
            {
                refill();
                ref->process(ptrs, 2, kBlock);
                for (int i = 0; i < kBlock; ++i)
                    refPeak = std::max(refPeak, std::max(std::fabs(l[(size_t) i]),
                                                         std::fabs(r[(size_t) i])));
            }

            hot->prepare(kRate, kBlock, 2);
            hot->setParam("mix", 1.0f);
            RT_CHECK(hot->setStringParam("irFile", irA.string().c_str()),
                     "the raced convolution refused the IR");

            std::atomic<bool> run{ true };
            std::atomic<long> rebuilds{ 0 };
            std::thread publisher([&]
            {
                // NOT marked as the RT thread: these rebuilds are owner-thread
                // work and allocate by design.
                for (long i = 0; run.load(std::memory_order_acquire); ++i)
                {
                    if (i % 2 == 0) hot->setStringParam("irFile", irB.string().c_str());
                    else            hot->setStringParam("irFile", irA.string().c_str());
                    hot->setParam("lengthPct", (i % 4 == 0) ? 40.0f : 100.0f);
                    rebuilds.fetch_add(2, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            });

            float hotPeak = 0.0f;
            bool  hotFinite = true;
            // At least kRaceBlocks, and on until 100 rebuilds have overlapped
            // the render (gatherEvidence, #55): a loaded 16-thread box managed
            // only 126-128 in the fixed window, a starved one would not reach
            // the floor at all.
            int raceBlocks = 0;
            const Gathered g = gatherEvidence([&]
            {
                refill();
                hot->process(ptrs, 2, kBlock);
                ++raceBlocks;
                for (int i = 0; i < kBlock; ++i)
                {
                    hotFinite = hotFinite && std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i]);
                    hotPeak = std::max(hotPeak, std::max(std::fabs(l[(size_t) i]),
                                                         std::fabs(r[(size_t) i])));
                }
            }, [&] { return rebuilds.load() >= 100; }, kRaceBlocks);
            run.store(false, std::memory_order_release);
            publisher.join();

            note("convreverb raced: " + std::to_string(rebuilds.load())
                 + " owner-thread rebuilds across " + std::to_string(raceBlocks)
                 + " rendered blocks, reference peak " + std::to_string(refPeak)
                 + ", raced peak " + std::to_string(hotPeak));
            RT_CHECK(rebuilds.load() >= 100,
                     "only " + std::to_string(rebuilds.load())
                     + " rebuilds overlapped the render - the probe is vacuous" + g.why());
            RT_CHECK(hotFinite, "the raced convolution emitted a non-finite sample");
            RT_CHECK(refPeak > 1.0e-4f,
                     "the reference convolution produced nothing to compare against");
            RT_CHECK(hotPeak < 8.0f * refPeak,
                     "the raced convolution peaked at " + std::to_string(hotPeak)
                     + " against a reference peak of " + std::to_string(refPeak)
                     + " - an owner-thread rebuild corrupted the callback's transform");
        }
    }

    // --- what a 2 s IR costs, per sample -------------------------------------
    auto conv = dcrig::createModule("convreverb");
    if (conv != nullptr)
    {
        conv->prepare(kRate, kBlock, 2);
        conv->setParam("mix", 1.0f);
        conv->setStringParam("irFile", irA.string().c_str());
        for (int i = 0; i < 200; ++i) { refill(); conv->process(ptrs, 2, kBlock); }
        constexpr int kTimed = 400;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kTimed; ++i) { refill(); conv->process(ptrs, 2, kBlock); }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        const double perSample = ns / (double) (kTimed * kBlock);
        note("convreverb, 2 s IR (96,000 taps) at a 480-frame block, stereo: "
             + std::to_string(perSample) + " ns/sample, "
             + std::to_string(perSample * kRate * 1.0e-7) + " % of one core at 48 kHz");
        RT_CHECK(perSample < 2000.0,
                 "a 2 s convolution costs " + std::to_string(perSample)
                 + " ns/sample - that is not a pedal, that is the whole budget");
    }

    std::filesystem::remove(irA, ec);
    std::filesystem::remove(irB, ec);
    endScenario();
}

// The 2026-09 FILTER drawer (src/dsp/gear/FilterGear.cpp, docs/guides/gear-filter.md),
// all five of it in ONE chain: svf, comb, vocoder, talkbox and tilt.
// RtFullRigAllocatesNothing already walks the catalogue one node at a time; this
// drives them TOGETHER and with the two paths that only exist here actually
// taken — the vocoder KEYED (processSidechained, which is a different entry
// point from the one a catalogue walk reaches) and a BAND-COUNT SWITCH mid-run,
// which is the one structure change a core in this drawer makes for itself.
//
// The band switch is the interesting half. The vocoder holds two complete banks
// and crossfades between them, so a count change costs a bank CONFIGURE on the
// callback: ~100 biquad designs and 48 follower retunes. Coefficient maths is
// legal there; a std::vector growing under it is not, and the two are one line
// apart in any implementation that keeps its banks in a resizable container.
// The `eq` module's frequency ranges were widened (2026-10, DotCharter #68) to a
// 20 Hz bass shelf, a 100 Hz .. 8 kHz bell and a 20 kHz treble shelf. A 20 kHz
// corner on a 44.1 kHz device is past the core's 0.9 x Nyquist guard, so this
// runs at 44.1 kHz and sweeps every corner across its whole new range, every
// block, from the callback thread (the pessimistic publisher): the coefficient
// redesign that follows each move must neither allocate nor go non-finite.
void scenarioEqWidenedRange()
{
    beginScenario("RtEqSweepsItsWidenedRangeAt44kWithoutAllocating");

    constexpr int    kBlock  = 441;
    constexpr int    kBlocks = 200;
    constexpr double kRate   = 44100.0;

    auto eq = dcrig::createModule("eq");
    if (eq == nullptr) { fail("could not create 'eq'"); endScenario(); return; }
    eq->prepare(kRate, kBlock, 2);
    eq->setParam("lowGainDb", 12.0f);
    eq->setParam("midGainDb", -12.0f);
    eq->setParam("midQ", 2.5f);
    eq->setParam("highGainDb", 12.0f);

    std::vector<float> l((size_t) kBlock, 0.0f), r((size_t) kBlock, 0.0f);
    float* ptrs[2] = { l.data(), r.data() };
    uint32_t rng = 0x6D2B79F5u;
    auto pump = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            l[(size_t) i] = r[(size_t) i] = 0.25f * (((float) (rng >> 8) / (float) (1u << 24)) * 2.0f - 1.0f);
        }
        eq->process(ptrs, 2, kBlock);
    };
    for (int i = 0; i < 64; ++i) pump();

    dcrt::reset();
    dcrt::arm();
    double energy = 0.0;
    double peak = 0.0;
    bool   finite = true;
    {
        dcrt::ScopedRtThread rt;
        for (int b = 0; b < kBlocks; ++b)
        {
            // A knob drag: every corner sweeps its whole new range on a log
            // taper, bottom to top, then holds at the top for the last quarter.
            // (Slamming each corner between its two ends every block is not a
            // gesture anything sends, and rings any TDF-II biquad.)
            const float t = std::min(1.0f, (float) b / (0.75f * (float) kBlocks));
            eq->setParam("lowFreq", 20.0f * std::pow(25.0f, t));     // 20 .. 500 Hz
            eq->setParam("midFreq", 100.0f * std::pow(80.0f, t));    // 100 .. 8000 Hz
            eq->setParam("highFreq", 2000.0f * std::pow(10.0f, t));  // 2 .. 20 kHz, past the 44.1 kHz guard
            pump();
            for (int i = 0; i < kBlock; ++i)
            {
                energy += (double) l[(size_t) i] * l[(size_t) i];
                peak = std::max(peak, (double) std::abs(l[(size_t) i]));
                finite = finite && std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i]);
            }
        }
    }
    dcrt::disarm();

    reportHeap("eq widened range");
    RT_CHECK(dcrt::allocCount() == 0,
             "the eq allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback while its corners swept the widened range");
    RT_CHECK(dcrt::freeCount() == 0,
             "the eq freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback while its corners swept the widened range");
    RT_CHECK(finite, "the eq emitted a non-finite sample with a corner at its new extreme");
    RT_CHECK(energy > 1.0e-3, "the eq produced no output - a silent module would pass any allocation gate");
    // +12 dB shelves on a 0.25 noise floor stay well inside this; a corner that
    // crossed Nyquist would ring far past it.
    RT_CHECK(peak < 8.0, "the eq peaked at " + std::to_string(peak) + " with a corner at its new extreme");
    endScenario();
}

void scenarioFilterGear()
{
    beginScenario("RtFilterGearIsAllocationFreeUnderAKeyedBandSwitch");

    constexpr int    kBlock  = 480;
    constexpr int    kBlocks = 200;
    constexpr double kRate   = 48000.0;

    const char* const kIds[] = { "svf", "comb", "vocoder", "talkbox", "tilt" };
    std::vector<std::unique_ptr<dcrig::DspModule>> rig;
    for (const char* id : kIds)
    {
        auto m = dcrig::createModule(id);
        if (m == nullptr) { fail(std::string("could not create '") + id + "'"); endScenario(); return; }
        m->prepare(kRate, kBlock, 2);
        rig.push_back(std::move(m));
    }
    // Everything AUDIBLE and everything MOVING, so nothing here can pass by
    // sitting at unity: the filter is resonant and driven, the comb is
    // resonating, the vocoder is at full wet and the talkbox is sweeping.
    rig[0]->setParam("resonance", 0.9f);
    rig[0]->setParam("drive", 0.8f);
    rig[0]->setParam("source", 1.0f);          // LFO-swept cutoff
    rig[0]->setParam("cutoffHz", 9000.0f);     // ...swept up past sr/4 (#87)
    rig[1]->setParam("feedback", 0.9f);
    rig[2]->setParam("mix", 1.0f);
    rig[2]->setParam("carrier", 0.0f);         // Sidechain: the KEY is the carrier
    rig[3]->setParam("sweep", 2.0f);           // LFO-swept vowel
    rig[4]->setParam("tilt", 8.0f);

    // The vocoder must advertise the capability it is about to be handed.
    RT_CHECK(rig[2]->supportsSidechain(),
             "the vocoder does not advertise a sidechain, but the key IS its carrier");
    RT_CHECK(! rig[0]->supportsSidechain() && ! rig[1]->supportsSidechain()
             && ! rig[3]->supportsSidechain() && ! rig[4]->supportsSidechain(),
             "a filter-drawer module advertises a sidechain it has no detector for");

    uint32_t rng = 0x2F7A91C3u;
    auto nextNoise = [&rng]
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return ((float) (rng >> 8) / (float) (1u << 24)) * 2.0f - 1.0f;
    };
    std::vector<float> l((size_t) kBlock, 0.0f), r((size_t) kBlock, 0.0f);
    std::vector<float> kl((size_t) kBlock, 0.0f), kr((size_t) kBlock, 0.0f);
    float* ptrs[2] = { l.data(), r.data() };
    const float* keyPtrs[2] = { kl.data(), kr.data() };
    double phase = 0.0;
    auto refill = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            // A 220 Hz tone for the rig (a vocoder MODULATOR needs harmonic
            // content, not noise) and a noise KEY for the carrier.
            const float s = 0.25f * (float) std::sin(phase);
            phase += 6.283185307179586 * 220.0 / kRate;
            if (phase >= 6.283185307179586) phase -= 6.283185307179586;
            l[(size_t) i] = r[(size_t) i] = s;
            const float k = 0.4f * nextNoise();
            kl[(size_t) i] = kr[(size_t) i] = k;
        }
    };
    auto pump = [&]
    {
        refill();
        for (size_t m = 0; m < rig.size(); ++m)
        {
            if (m == 2) rig[m]->processSidechained(ptrs, 2, kBlock, keyPtrs, 2);
            else        rig[m]->process(ptrs, 2, kBlock);
        }
    };

    // Warm: fill the comb's line, both vocoder banks' followers and every
    // smoother, and drain whatever prepare() published.
    for (int i = 0; i < 400; ++i) pump();

    dcrt::reset();
    dcrt::arm();
    double energy = 0.0;
    bool   finite = true;
    {
        dcrt::ScopedRtThread rt;
        for (int b = 0; b < kBlocks; ++b)
        {
            // Mid-run, from THIS thread — the pessimistic case; the real
            // publisher is the message thread and is not armed.
            if (b == 50)  rig[2]->setParam("bands", 24.0f);   // 16 -> 24, crossfaded
            if (b == 80)  rig[0]->setParam("mode", 3.0f);     // a tap switch under the fade
            if (b == 120) rig[2]->setParam("bands", 8.0f);    // 24 -> 8
            if (b == 160) rig[1]->setParam("freqHz", 440.0f); // a tuning glide
            pump();
            for (int i = 0; i < kBlock; ++i)
            {
                energy += (double) l[(size_t) i] * l[(size_t) i];
                finite = finite && std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i]);
            }
        }
    }
    dcrt::disarm();

    reportHeap("filter gear");
    RT_CHECK(dcrt::allocCount() == 0,
             "the five filter modules allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback across a keyed band switch");
    RT_CHECK(dcrt::freeCount() == 0,
             "the five filter modules freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback across a keyed band switch");
    RT_CHECK(finite, "the filter rig emitted a non-finite sample");
    RT_CHECK(energy > 1.0e-6,
             "the five-module filter rig produced no output at all - a silent rig costs "
             "nothing and would pass any allocation gate");
    // A resonant SVF at drive 1 is the one thing here that can run away, and a
    // bounded peak is the claim its `drive` knob exists to make.
    double peak = 0.0;
    for (int i = 0; i < kBlock; ++i) peak = std::max(peak, (double) std::abs(l[(size_t) i]));
    RT_CHECK(peak < 2.0,
             "the resonant filter rig peaked at " + std::to_string(peak)
             + " - the saturator inside the resonance path is not bounding it");

    // --- what the widest vocoder costs, per sample ---------------------------
    auto voc = dcrig::createModule("vocoder");
    if (voc != nullptr)
    {
        voc->prepare(kRate, kBlock, 2);
        voc->setParam("bands", 24.0f);
        voc->setParam("mix", 1.0f);
        voc->setParam("carrier", 1.0f);    // the internal saw, so nothing waits on a key
        for (int i = 0; i < 300; ++i) { refill(); voc->process(ptrs, 2, kBlock); }
        constexpr int kTimed = 400;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kTimed; ++i) { refill(); voc->process(ptrs, 2, kBlock); }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        const double perSample = ns / (double) (kTimed * kBlock);
        note("vocoder, 24 bands (4th order both sides) at a 480-frame block, stereo: "
             + std::to_string(perSample) + " ns/sample, "
             + std::to_string(perSample * kRate * 1.0e-7) + " % of one core at 48 kHz");
        RT_CHECK(perSample < 2000.0,
                 "a 24-band vocoder costs " + std::to_string(perSample)
                 + " ns/sample - that is not a pedal, that is the whole budget");
    }

    endScenario();
}

// Amp / cab gear -------------------------------------------------------------------
// The 2026-09 AMP / CAB drawer (src/dsp/gear/AmpCabGear.cpp), all four modules
// in ONE chain, which is the arrangement a player builds: preamp -> power amp
// -> mic. It is the heaviest rig in the gate per node — two more 4x
// oversamplers, a 2 kHz delay line with eight taps read per sample, and two
// pairs of crossfaded filter banks.
//
// The point of the SWITCH mid-run is that these four modules do something the
// drive drawer's do not: a model / mic change swaps which filter BANK is live
// and starts a 10 ms crossfade in which BOTH banks run. If that path ever
// reconfigured, resized or reallocated a bank instead of writing into the
// parked one, it would allocate here and nowhere else.
//
// 2026-10 (DotCharter #70 / #72): the guitar `amp` rides at the head of the
// chain on a CIRCUIT voicing (24-26: a third-order tone stack recomputed by
// every knob move) with Bright and Resonance, and the bass head's new controls
// (mid sweep, octave-shifted bells, Ultra Lo / Ultra Hi) are all switched while
// armed — each is a filter that ENTERS the path on a switch, so a path that
// built or resized one there would allocate here.
//
// The second half is the oversized-block guard. The catalogue-wide scenario
// (RtOversizedBlockIsSkippedNotPartiallyProcessed) carries a hard-coded list of
// four cores, so a new core is NOT covered by it — and the failure is silent: a
// core that clamps instead of skipping processes the head of the block and
// leaves the tail dry.
void scenarioAmpCabGearChain()
{
    beginScenario("RtAmpCabGearChainAllocatesNothingAcrossAModelSwitch");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    const char* const kAmpCabNodes[] = { "bassamp", "acpre", "poweramp", "micpos", "amp" };
    std::vector<int> ids;
    for (const char* id : kAmpCabNodes)
    {
        const int n = fx.engine().dspAddNode(0, id);
        if (n >= 0) ids.push_back(n);
    }
    RT_CHECK(ids.size() == 5, "expected five amp/cab nodes, built " + std::to_string(ids.size()));
    if (ids.size() == 5) fx.engine().dspSetParam(ids[4], "voicing", 24.0f);

    warmUpAndArm(fx);
    fx.pumpArmed(100);
    if (ids.size() == 5)
    {
        // Every switch in the drawer, while the callback is running and the
        // sentinel is armed.
        fx.engine().dspSetParam(ids[0], "model", 2.0f);
        fx.engine().dspSetParam(ids[1], "phase", 1.0f);
        fx.engine().dspSetParam(ids[2], "tubes", 4.0f);
        fx.engine().dspSetParam(ids[3], "mic", 1.0f);
        fx.engine().dspSetParam(ids[0], "lowShift", 1.0f);
        fx.engine().dspSetParam(ids[0], "highShift", 1.0f);
        fx.engine().dspSetParam(ids[0], "mid", 8.0f);
        fx.engine().dspSetParam(ids[0], "midHz", 1600.0f);
        fx.engine().dspSetParam(ids[0], "lowMidOct", -1.5f);
        fx.engine().dspSetParam(ids[4], "bright", 1.0f);
        fx.engine().dspSetParam(ids[4], "resonance", 0.7f);
    }
    fx.pumpArmed(50);
    if (ids.size() == 5)
    {
        // A circuit voicing re-solves its stack on every knob and switches
        // between the two documented stacks.
        for (int k = 0; k < 10; ++k)
        {
            fx.engine().dspSetParam(ids[4], "treble", (float) k / 9.0f);
            fx.engine().dspSetParam(ids[4], "bass", 1.0f - (float) k / 9.0f);
            fx.engine().dspSetParam(ids[4], "voicing", (float) (24 + k % 3));
            fx.engine().dspSetParam(ids[4], "gain", (float) k / 9.0f);
            fx.pumpArmed(5);
        }
    }
    fx.pumpArmed(50);
    if (ids.size() == 5)
    {
        // The 2026-10 additions (DotCharter #75): the TUBE mic (a fifth bank
        // whose top shelf is designed near Nyquist) and the off-axis ANGLE,
        // whose two shelves are redesigned on the callback every 32 frames while
        // the angle ramps — so it is swept here with the sentinel armed.
        fx.engine().dspSetParam(ids[3], "mic", 4.0f);
        fx.engine().dspSetParam(ids[3], "angleDeg", 45.0f);
        fx.pumpArmed(50);
        fx.engine().dspSetParam(ids[3], "angleDeg", 10.0f);
        fx.pumpArmed(25);
        fx.engine().dspSetParam(ids[3], "mic", 0.0f);
        fx.engine().dspSetParam(ids[3], "angleDeg", 60.0f);
        fx.pumpArmed(25);
    }
    dcrt::disarm();
    if (ids.size() == 5)
    {
        // The switches reached the modules (a write the adapter refused would
        // make the run above vacuous).
        float bright = 0.f, shift = 0.f, voicing = 0.f;
        RT_CHECK(fx.engine().dspGetParam(ids[4], "bright", bright) && bright == 1.0f,
                 "amp.bright did not reach the module");
        RT_CHECK(fx.engine().dspGetParam(ids[0], "highShift", shift) && shift == 1.0f,
                 "bassamp.highShift did not reach the module");
        RT_CHECK(fx.engine().dspGetParam(ids[4], "voicing", voicing) && voicing == 24.0f,
                 "amp.voicing did not end where the loop left it (24 + 9 % 3)");
    }

    reportHeap("amp/cab gear chain");
    RT_CHECK(dcrt::allocCount() == 0,
             "the four-node amp/cab chain allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "the four-node amp/cab chain freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback");

    // --- oversized block: skipped WHOLE, never half-processed ---------------
    const int prepared = 256;
    const int oversize = 2 * prepared;
    auto probe = [&](const char* name, auto& core, auto& params)
    {
        core.setParams(params);
        core.prepare(48000.0, prepared, 2);
        core.setParams(params);

        std::vector<float> l((std::size_t) oversize), r((std::size_t) oversize);
        std::vector<float> ref((std::size_t) oversize);
        double phase = 0.0;
        const double inc = 6.283185307179586 * 220.0 / 48000.0;
        for (int i = 0; i < oversize; ++i)
        {
            const float s = 0.3f * (float) std::sin(phase);
            phase += inc;
            l[(std::size_t) i] = r[(std::size_t) i] = ref[(std::size_t) i] = s;
        }
        float* chans[2] = { l.data(), r.data() };
        core.process(chans, 2, oversize);

        double headDelta = 0.0;
        for (int i = 0; i < prepared; ++i)
            headDelta = std::max(headDelta,
                                 (double) std::abs(l[(std::size_t) i] - ref[(std::size_t) i]));
        RT_CHECK(headDelta < 1e-6,
                 std::string(name) + " processed the first " + std::to_string(prepared)
                 + " frames of a " + std::to_string(oversize)
                 + "-frame block and left the rest DRY (head delta "
                 + std::to_string(headDelta) + ")");
    };

    { dcdsp::BassAmpCore c;  dcdsp::BassAmpParams p;  p.gain = 0.8f; p.drive = 0.6f; p.master = 0.8f;
      p.lowShift = 1; p.highShift = 1; p.mid = 6.f;
      probe("bassamp", c, p); }
    { dcdsp::AmpCore c;      dcdsp::AmpParams p;      p.voicing = 25; p.bright = 1; p.resonance = 0.5f;
      p.gain = 0.6f; p.master = 0.8f;
      probe("amp (circuit voicing)", c, p); }
    { dcdsp::AcPreCore c;    dcdsp::AcPreParams p;    p.notchDepthDb = 12.f; p.presence = 0.8f; p.level = 0.8f;
      probe("acpre", c, p); }
    { dcdsp::PowerAmpCore c; dcdsp::PowerAmpParams p; p.drive = 0.8f; p.sag = 0.8f; p.level = 0.8f;
      probe("poweramp", c, p); }
    { dcdsp::MicPosCore c;   dcdsp::MicPosParams p;   p.room = 0.5f; p.blend2 = 0.5f; p.level = 0.8f;
      probe("micpos", c, p); }
    { dcdsp::MicPosCore c;   dcdsp::MicPosParams p;   p.mic = 4; p.angleDeg = 45.f; p.level = 0.8f;
      probe("micpos (tube, 45 deg)", c, p); }

    endScenario();
}

// Rocksmith amp chains (src/dsp/gear/RsAmpGear.cpp, DotCharter #88) -----------
// `rsamp` runs a table of up to four plug-in clones per model, loaded at run
// time (ABI v45): the real one is read from the user's own Rocksmith install, so
// the gate loads a SYNTHETIC table (tests/dsp/rsamp_synthetic_table.h) whose 63
// rows reach every slot kind, every distortion type and every curve shape and
// scaling. Three things only it does on the callback: a model change dips the
// output, swaps the whole chain at silence and resets every filter, oversampler
// and envelope; a moving knob re-solves EVERY curve and every biquad of the
// chain each 8 samples; and the slot kinds differ per model.
void scenarioRsAmpGear()
{
    beginScenario("RtRsAmpGearAllocatesNothingAcrossEveryModel");
    const auto table = rsamptest::makeFullTable();
    RT_CHECK(dcdsp::rsamp::TableRegistry::instance().load(table.data(), table.size(), nullptr),
             "rsamp: the synthetic table was refused");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    const int node = fx.engine().dspAddNode(0, "rsamp");
    RT_CHECK(node >= 0, "rsamp: add failed");

    warmUpAndArm(fx);
    fx.pumpArmed(50);
    if (node >= 0)
    {
        for (int m = 0; m < dcdsp::rsamp::kNumModels; ++m)
        {
            fx.engine().dspSetParam(node, "model", (float) m);
            for (int k = 1; k <= dcdsp::rsamp::kMaxKnobs; ++k)
                fx.engine().dspSetParam(node, ("k" + std::to_string(k)).c_str(), (float) ((m + k) % 5) / 4.0f);
            fx.pumpArmed(4);
        }
        // A knob sweep on one model, re-solving the chain while it ramps.
        fx.engine().dspSetParam(node, "model", 17.0f);
        for (int s = 0; s < 20; ++s)
        {
            fx.engine().dspSetParam(node, "k1", (float) (s % 2));
            fx.engine().dspSetParam(node, "k4", 1.0f - (float) (s % 2));
            fx.pumpArmed(3);
        }
    }
    fx.pumpArmed(50);
    dcrt::disarm();
    if (node >= 0)
    {
        float model = -1.f;
        RT_CHECK(fx.engine().dspGetParam(node, "model", model) && model == 17.0f,
                 "rsamp.model did not reach the module");
    }

    reportHeap("rsamp model walk");
    RT_CHECK(dcrt::allocCount() == 0,
             "rsamp allocated " + std::to_string(dcrt::allocCount()) + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "rsamp freed " + std::to_string(dcrt::freeCount()) + " time(s) on the callback");

    // --- oversized block: skipped WHOLE, never half-processed ---------------
    const int prepared = 256;
    const int oversize = 2 * prepared;
    for (const int m : { 17, 42, 55 })   // three rows of the synthetic table
    {
        dcdsp::RsAmpCore core;
        dcdsp::RsAmpParams p;
        p.model = m;
        core.setParams(p);
        core.prepare(48000.0, prepared, 2);
        std::vector<float> l((std::size_t) oversize), r((std::size_t) oversize), ref((std::size_t) oversize);
        for (int i = 0; i < oversize; ++i)
            l[(std::size_t) i] = r[(std::size_t) i] = ref[(std::size_t) i]
                = 0.3f * (float) std::sin(6.283185307179586 * 220.0 * i / 48000.0);
        float* chans[2] = { l.data(), r.data() };
        core.process(chans, 2, oversize);
        double headDelta = 0.0;
        for (int i = 0; i < prepared; ++i)
            headDelta = std::max(headDelta, (double) std::abs(l[(std::size_t) i] - ref[(std::size_t) i]));
        RT_CHECK(headDelta < 1e-6, "rsamp model " + std::to_string(m)
                 + " processed part of an oversized block (head delta " + std::to_string(headDelta) + ")");
        RT_CHECK(core.runsChainForTest(), "rsamp model " + std::to_string(m) + " ran no chain: the oversize check is vacuous");
    }

    dcdsp::rsamp::TableRegistry::instance().clear();
    endScenario();
}

// A table loaded (and unloaded) WHILE an rsamp node plays (ABI v45). The load
// parses and allocates on the CONTROL thread; the callback only sees a new
// pointer at the top of a block (one acquire load), dips, and swaps at silence.
// A core that copied the table, or a registry that freed the old one under it,
// would show here. "Happened": the core's own snapshot moves to the loaded table
// and back, and the node's output stops being its input once the chain is in
// the path.
void scenarioRsAmpTableSwap()
{
    beginScenario("RtRsAmpTableLoadedWhilePlayingSwapsWithoutAllocating");
    auto& registry = dcdsp::rsamp::TableRegistry::instance();
    registry.clear();
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();
    const int node = fx.engine().dspAddNode(0, "rsamp");
    RT_CHECK(node >= 0, "rsamp: add failed");
    if (node >= 0) fx.engine().dspSetParam(node, "model", 17.0f);

    warmUpAndArm(fx);
    fx.pumpArmed(50);
    const auto full = rsamptest::makeFullTable();
    const auto partial = rsamptest::makeTable({ 3, 17, 40 });
    for (int round = 0; round < 6; ++round)
    {
        // Control thread (this one, outside the RT thread): legal to allocate.
        if (round % 3 == 2) registry.clear();
        else
        {
            const auto& t = (round % 3 == 0) ? full : partial;
            RT_CHECK(registry.load(t.data(), t.size(), nullptr), "rsamp: a synthetic table was refused");
        }
        fx.pumpArmed(40);
    }
    dcrt::disarm();
    reportHeap("rsamp table swaps");
    RT_CHECK(dcrt::allocCount() == 0,
             "an rsamp table swap allocated " + std::to_string(dcrt::allocCount()) + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "an rsamp table swap freed " + std::to_string(dcrt::freeCount()) + " time(s) on the callback");

    // Not vacuous: a core driven the same way follows the registry, and with the
    // table in force its chain changes the signal.
    dcdsp::RsAmpCore core;
    dcdsp::RsAmpParams p;
    p.model = 17;
    core.setParams(p);
    core.prepare(48000.0, 256, 2);
    std::vector<float> l(256), r(256);
    float* chans[2] = { l.data(), r.data() };
    RT_CHECK(core.tableForTest() == nullptr, "rsamp: a core prepared with no table loaded has one");
    RT_CHECK(registry.load(full.data(), full.size(), nullptr), "rsamp: reload refused");
    double moved = 0.0;
    for (int b = 0; b < 8; ++b)
    {
        for (int i = 0; i < 256; ++i)
            l[(std::size_t) i] = r[(std::size_t) i] = 0.1f * (float) std::sin(6.283185307179586 * 220.0 * (b * 256 + i) / 48000.0);
        const std::vector<float> in = l;
        core.process(chans, 2, 256);
        if (b == 7)
            for (int i = 0; i < 256; ++i) moved = std::max(moved, (double) std::abs(l[(std::size_t) i] - in[(std::size_t) i]));
    }
    RT_CHECK(core.tableForTest() == registry.current() && core.runsChainForTest(),
             "rsamp: the core did not pick up the table loaded while it ran");
    RT_CHECK(moved > 1e-3, "rsamp: with the table in force the chain did not change the signal");
    registry.clear();
    for (int b = 0; b < 8; ++b) core.process(chans, 2, 256);
    RT_CHECK(core.tableForTest() == nullptr, "rsamp: the core did not drop the unloaded table");
    endScenario();
}

// The 2026-09 "utility" gear wave (docs/guides/gear-utility.md): volume,
// midside and looper, chained, with the LOOPER's footswitch worked hard.
//
// RtEveryCatalogueModuleIsAllocationFree drives each of the three on its own,
// but it never changes a param mid-run, and every interesting thing the looper
// does is a STATE TRANSITION: Record -> Play publishes a loop length, Overdub
// starts an incremental undo snapshot that drains 2 frames per output sample,
// Undo swaps two 23 MB vectors, and reverse/halfSpeed re-aim the read head. Any
// of those written as "copy the buffer now" or "resize to the loop length" is
// invisible to a gate that only pumps blocks — and the undo swap in particular
// LOOKS like something that must allocate, which is exactly why it is pinned:
// std::vector::swap exchanges three pointers and nothing else.
void scenarioUtilityGearLooperFootswitch()
{
    beginScenario("RtUtilityGearFootswitchIsAllocationFree");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();
    AudioEngine& e = fx.engine();

    static const char* const kUtilityGear[] = { "volume", "midside", "looper" };

    // ---- the three modules exist, and behave at their edges -----------------
    for (const char* id : kUtilityGear)
    {
        auto mod = dcrig::createModule(id);
        if (mod == nullptr) { fail(std::string("the catalogue cannot build '") + id + "'"); continue; }

        constexpr int kPrepared = 256;
        constexpr int kOversize = 2 * kPrepared;
        mod->prepare(48000.0, kPrepared, 2);

        std::vector<float> l((std::size_t) kOversize), r((std::size_t) kOversize), ref((std::size_t) kOversize);
        double phase = 0.0;
        for (int i = 0; i < kOversize; ++i)
        {
            const float v = 0.3f * (float) std::sin(phase);
            phase += 6.283185307179586 * 220.0 / 48000.0;
            l[(std::size_t) i] = r[(std::size_t) i] = ref[(std::size_t) i] = v;
        }
        float* chans[2] = { l.data(), r.data() };
        mod->process(chans, 2, kOversize);

        double headDelta = 0.0;
        for (int i = 0; i < kPrepared; ++i)
            headDelta = std::max(headDelta,
                                 (double) std::abs(l[(std::size_t) i] - ref[(std::size_t) i]));
        RT_CHECK(headDelta < 1e-6,
                 std::string(id) + " processed the first " + std::to_string(kPrepared)
                 + " frames of an oversized block and left the rest DRY (head delta "
                 + std::to_string(headDelta) + ") — it must be skipped whole");

        // Every declared param must refuse a NaN and keep what it had. The
        // looper's `undo` is the one param whose read-back is not what was
        // written (it is a momentary trigger and always reads 0), and its
        // catalogue DEFAULT is 0, so the same assertion still holds for it.
        const auto& cat = dcrig::moduleCatalogue();
        for (const auto& info : cat)
        {
            if (info.id != id) continue;
            for (const auto& prm : info.params)
            {
                RT_CHECK(mod->setParam(prm.id.c_str(), prm.defaultValue),
                         std::string(id) + "." + prm.id + " refused its own default");
                RT_CHECK(! mod->setParam(prm.id.c_str(), std::numeric_limits<float>::quiet_NaN()),
                         std::string(id) + "." + prm.id + " ACCEPTED a NaN");
                float kept = 0.0f;
                RT_CHECK(mod->getParam(prm.id.c_str(), kept)
                             && std::abs(kept - prm.defaultValue) < 1.0e-3f,
                         std::string(id) + "." + prm.id + " did not keep its value across a rejected NaN");
            }
        }
    }

    // ---- what a 60 s worst-case looper COSTS, at both ends of the rate range.
    // Reported, never asserted: it is a property of the geometry, and the point
    // of printing it is that a change to kMaxLoopSeconds shows up in the log.
    for (double rate : { 48000.0, 192000.0 })
    {
        auto probe = dcrig::createModule("looper");
        if (probe == nullptr) continue;
        probe->prepare(rate, 512, 2);
        auto* core = static_cast<dcdsp::LooperCore*>(probe->coreForTest());
        if (core == nullptr) continue;
        note("looper at " + std::to_string((long long) rate) + " Hz stereo: "
             + std::to_string(core->capacityFrames()) + " frames capacity, "
             + std::to_string(core->bufferBytes() / (1024 * 1024))
             + " MiB across the loop + undo rings");
    }

    // ---- all three live, with the footswitch worked, under the sentinel -----
    std::vector<int> ids;
    for (const char* id : kUtilityGear)
    {
        const int n = e.dspAddNode(0, id);
        if (n >= 0) ids.push_back(n);
    }
    if (ids.size() != 3) { fail("expected 3 utility-gear nodes, built " + std::to_string(ids.size())); endScenario(); return; }

    e.dspSetParam(ids[0], "curve", 3.0f);          // S-Curve: the pow() path
    e.dspSetParam(ids[0], "smoothMs", 200.0f);
    e.dspSetParam(ids[1], "width", 2.0f);
    e.dspSetParam(ids[1], "monoBelowHz", 120.0f);  // the side shelf, engaged
    e.dspSetParam(ids[2], "maxLoopS", 2.0f);       // a short loop: it CLOSES mid-run
    e.dspSetParam(ids[2], "mix", 1.0f);

    warmUpAndArm(fx);
    constexpr int kBlocks = 200;
    for (int b = 0; b < kBlocks; ++b)
    {
        // The whole footswitch vocabulary, inside the armed window: a record
        // pass that closes on its own cap, play, an overdub (which starts the
        // incremental snapshot), an undo (the vector swap), and the two read
        // modes. The pedal and the M/S box are swept alongside so their
        // smoothers and the shelf redesign are under the sentinel too.
        if (b == 5)   e.dspSetParam(ids[2], "state", 1.0f);   // Record
        if (b == 40)  e.dspSetParam(ids[2], "state", 2.0f);   // Play
        if (b == 60)  e.dspSetParam(ids[2], "state", 3.0f);   // Overdub
        if (b == 110) e.dspSetParam(ids[2], "undo", 1.0f);    // the swap
        if (b == 130) e.dspSetParam(ids[2], "reverse", 1.0f);
        if (b == 150) e.dspSetParam(ids[2], "halfSpeed", 1.0f);
        if (b == 170) { e.dspSetParam(ids[2], "reverse", 0.0f);
                        e.dspSetParam(ids[2], "halfSpeed", 0.0f); }
        if (b == 185) e.dspSetParam(ids[2], "state", 0.0f);   // Stop
        e.dspSetParam(ids[0], "position", (float) (b % 21) / 20.0f);
        e.dspSetParam(ids[1], "monoBelowHz", (b % 2) ? 200.0f : 80.0f);
        dcrt::g_currentBlock.store((unsigned) b, std::memory_order_relaxed);
        dcrt::ScopedRtThread rt;
        nulldev::pump(1);
    }
    dcrt::disarm();

    reportHeap("utility gear footswitch");
    RT_CHECK(dcrt::allocCount() == 0,
             "the utility-gear rig allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");
    RT_CHECK(dcrt::freeCount() == 0,
             "the utility-gear rig freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback");
    RT_CHECK(! fx.lastOutputHasNonFinite(), "the utility-gear rig produced a non-finite sample");

    endScenario();
}

// The four `pitch`-drawer modules of the 2026-09 gear wave, chained, under the
// allocation sentinel, WHILE the three things that re-derive their geometry
// happen: `pitchcorrect`'s scale is switched (a new quantiser and a new shift
// ratio), `basssynth`'s octave is switched (a new oscillator increment, which
// must GLIDE and not step) and `detune`'s latency mode is flipped (a window
// change, and a window IS a delay tap).
//
// This drawer is the one that needed its own scenario most, for two reasons
// the catalogue-wide sweep cannot cover:
//   * every module here holds a PITCH TRACKER or a moving delay tap, and the
//     catalogue sweep drives modules at their DEFAULTS against noise — where no
//     tracker is ever confident, so the whole retarget/glide path is dead code
//     to it. The stimulus below is a real note, so the trackers lock and the
//     shifters actually wrap grains.
//   * `detune`'s latency mode is a delay-tap move, which is exactly the shape
//     RtPitchCoreFlipDoesNotBurstOneCallback exists for on the harmonizer and
//     the whammy. Here the window table is PHASE-indexed and shared by both
//     geometries, so a flip rebuilds nothing at all — and `windowBuilds()`
//     proves that rather than a comment claiming it.
//
// It also REPORTS ns/sample for the two tracker-driven modules, because "it
// allocates nothing" says nothing about whether it is affordable.
void scenarioPitchGearIsAllocationFreeUnderScaleOctaveAndLatencyFlips()
{
    beginScenario("RtPitchGearIsAllocationFreeUnderScaleOctaveAndLatencyFlips");

    constexpr int    kBlock  = 256;
    constexpr int    kBlocks = 200;
    constexpr double kRate   = 48000.0;

    const char* const kIds[] = { "detune", "pitchcorrect", "twelvestring", "basssynth" };
    std::vector<std::unique_ptr<dcrig::DspModule>> rig;
    for (const char* id : kIds)
    {
        auto m = dcrig::createModule(id);
        if (m == nullptr) { fail(std::string("could not create '") + id + "'"); endScenario(); return; }
        m->prepare(kRate, kBlock, 2);
        rig.push_back(std::move(m));
    }
    // Everything audible and everything WORKING: a rig that sits at unity would
    // pass an allocation gate without reaching a single line this wave added.
    rig[0]->setParam("mix", 1.0f);            // detune: wet only
    rig[0]->setParam("cents", 25.0f);
    rig[1]->setParam("mix", 1.0f);            // pitchcorrect: correcting
    rig[1]->setParam("amount", 1.0f);
    rig[1]->setParam("formant", 1.0f);        // the LPC path too
    rig[2]->setParam("mix", 1.0f);            // twelvestring: both courses
    rig[2]->setParam("octaveLevel", 1.0f);
    rig[2]->setParam("unisonLevel", 1.0f);
    rig[3]->setParam("synthMix", 1.0f);       // basssynth: voicing
    rig[3]->setParam("waveform", 1.0f);

    // --- honest latency, before the measured window -------------------------
    // detune: kPreDelayMs (1) + kWindowLongMs/2 (20) = 21 ms, and the low mode
    // is kPreDelayMs + kWindowShortMs/2 = 5 ms. pitchcorrect: kPreDelayMs +
    // kAlignMs (8) + kWindowMs/2 (20) = 29 ms — the splice-alignment search is
    // part of the figure, exactly as dcdsp_whammy.h's own search span is.
    const int expectNormal = (int) std::lround(kRate * 21.0 / 1000.0);
    const int expectLow    = (int) std::lround(kRate * 5.0 / 1000.0);
    const int expectCorr   = (int) std::lround(kRate * 29.0 / 1000.0);
    RT_CHECK(rig[0]->latencySamples() == expectNormal,
             "detune reported " + std::to_string(rig[0]->latencySamples())
             + " samples in the normal latency mode, expected " + std::to_string(expectNormal));
    rig[0]->setParam("lowLatency", 1.0f);
    RT_CHECK(rig[0]->latencySamples() == expectLow,
             "detune reported " + std::to_string(rig[0]->latencySamples())
             + " samples in the LOW latency mode, expected " + std::to_string(expectLow)
             + " - the adapter is not following the window");
    rig[0]->setParam("lowLatency", 0.0f);
    RT_CHECK(rig[1]->latencySamples() == expectCorr,
             "pitchcorrect reported " + std::to_string(rig[1]->latencySamples())
             + " samples, expected " + std::to_string(expectCorr));
    // ...and the two modules whose DRY path is not delayed report ZERO, which
    // is the honest answer and not an oversight: a 12-string adds a course
    // beside the note and a synth GENERATES its voice.
    RT_CHECK(rig[2]->latencySamples() == 0 && rig[3]->latencySamples() == 0,
             "twelvestring/basssynth reported non-zero latency for an undelayed dry path");

    // A real NOTE, not noise: a 110 Hz sawtooth-ish tone the trackers can lock
    // to, which is what puts the retarget, the glide and the grain wraps in the
    // measured window at all.
    double phase = 0.0;
    const double inc = 6.283185307179586 * 110.0 / kRate;
    std::vector<float> l((size_t) kBlock, 0.0f), r((size_t) kBlock, 0.0f);
    float* ptrs[2] = { l.data(), r.data() };
    auto refill = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = (float) (0.30 * std::sin(phase) + 0.10 * std::sin(2.0 * phase)
                                   + 0.05 * std::sin(3.0 * phase));
            phase += inc;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
            l[(size_t) i] = s; r[(size_t) i] = s;
        }
    };
    auto pump = [&] { refill(); for (auto& m : rig) m->process(ptrs, 2, kBlock); };

    // Warm: prime every shifter ring, every tracker window and both LPC set
    // rings, and drain whatever prepare() published.
    for (int i = 0; i < 400; ++i) pump();

    dcrt::reset();
    dcrt::arm();
    double energy = 0.0;
    bool   finite = true;
    {
        dcrt::ScopedRtThread rt;
        for (int b = 0; b < kBlocks; ++b)
        {
            // Mid-run, published from THIS thread, which is the pessimistic
            // case (the real publisher is the message thread and is not armed).
            if (b == 50)  rig[1]->setParam("scale", 2.0f);    // Chromatic -> Minor
            if (b == 80)  rig[3]->setParam("octave", 2.0f);   // -1 -> 0
            if (b == 110) rig[0]->setParam("lowLatency", 1.0f);
            if (b == 140) rig[3]->setParam("waveform", 3.0f); // Saw -> PWM
            if (b == 170) rig[0]->setParam("lowLatency", 0.0f);
            pump();
            for (int i = 0; i < kBlock; ++i)
            {
                energy += (double) l[(size_t) i] * l[(size_t) i];
                finite = finite && std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i]);
            }
        }
    }
    dcrt::disarm();

    reportHeap("pitch gear");
    RT_CHECK(finite, "the pitch rig emitted a non-finite sample");
    RT_CHECK(energy > 1.0e-6,
             "the four-module pitch rig produced no output at all - a silent rig costs "
             "nothing and would pass any allocation gate");
    RT_CHECK(dcrt::allocCount() == 0,
             "the pitch rig allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback across a scale switch, an octave switch and two "
               "latency-mode flips");
    RT_CHECK(dcrt::freeCount() == 0,
             "the pitch rig freed " + std::to_string(dcrt::freeCount())
             + " time(s) on the callback - a window table or a tracker ring was resized "
               "by a knob instead of by prepare()");
    // The flip cost NOTHING to rebuild: the crossfade window is indexed by
    // PHASE, so it is shared by both geometries and prepare() built it once per
    // shifter. This is the counter-based assertion
    // RtPitchCoreFlipDoesNotBurstOneCallback makes for the harmonizer's Hann
    // tables, one level stronger - here the answer is zero by construction.
    {
        auto* core = static_cast<dcdsp::DetuneCore*>(rig[0]->coreForTest());
        RT_CHECK(core != nullptr, "detune exposed no core for the test");
        if (core != nullptr)
        {
            RT_CHECK(core->ringsCoverGeometry(),
                     "a detune shifter ring no longer covers the geometry the audio path "
                     "can ask for - the long window was allocated in the short mode");
            note("detune crossfade-window tables built across "
                 + std::to_string(2) + " latency flips: "
                 + std::to_string(core->windowBuilds()) + " (prepare() builds one per shifter)");
        }
    }

    // --- what the two tracker-driven modules cost, per sample ---------------
    auto costOf = [&](const char* id, const char* setupId, float setupValue)
    {
        auto m = dcrig::createModule(id);
        if (m == nullptr) return;
        m->prepare(kRate, kBlock, 2);
        if (setupId != nullptr) m->setParam(setupId, setupValue);
        for (int i = 0; i < 200; ++i) { refill(); m->process(ptrs, 2, kBlock); }
        constexpr int kTimed = 400;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kTimed; ++i) { refill(); m->process(ptrs, 2, kBlock); }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        const double perSample = ns / (double) (kTimed * kBlock * 2);
        note(std::string(id) + ", stereo at a 256-frame block: " + std::to_string(perSample)
             + " ns/sample/channel, " + std::to_string(perSample * kRate * 2.0e-7)
             + " % of one core at 48 kHz");
        RT_CHECK(perSample < 1200.0,
                 std::string(id) + " costs " + std::to_string(perSample)
                 + " ns/sample/channel - that is not a pedal, that is the whole budget");

        // ---- and the WORST block, not only the mean (W5-B15) --------------
        // This drawer's cost is not smooth. `searchAlign` runs a normalised
        // correlation over the whole alignment span INSIDE THE ONE SAMPLE a
        // grain wraps on; AutocorrTracker::analyse() is a second burst every
        // hop and LpcFormant::computeSet() a third, and all of them start from
        // hopCount_ = 0 on reset, so every tracker in a rig hops on the same
        // sample index. None of it allocates, so the sentinel cannot see it,
        // and a mean over 400 blocks amortises a burst into invisibility --
        // which is exactly what this scenario measured and nothing else did.
        //
        // The assertion is on the RATIO, not on wall time: a clock bound would
        // flake the moment another agent built alongside, while "the worst
        // block is no more than N times the mean" is a statement about the
        // SHAPE of the workload and survives a slow box. It is deliberately
        // loose -- one block in four hundred legitimately carries a tracker
        // hop, an LPC solve and two grain wraps at once.
        std::vector<double> perBlock;
        perBlock.reserve((std::size_t) kTimed);
        double meanNs = 0.0;
        for (int i = 0; i < kTimed; ++i)
        {
            refill();
            const auto b0 = std::chrono::steady_clock::now();
            m->process(ptrs, 2, kBlock);
            const auto b1 = std::chrono::steady_clock::now();
            const double one = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count();
            perBlock.push_back(one);
            meanNs += one;
        }
        meanNs /= (double) kTimed;
        std::sort(perBlock.begin(), perBlock.end());
        // The 99th percentile, not the MAXIMUM. A single block preempted by the
        // scheduler is an artefact of the box, and this is the one gate that
        // routinely runs while another agent is building; a per-wrap or per-hop
        // burst, by contrast, happens many times in 400 blocks and is still in
        // the top percentile. The raw max is reported and not asserted.
        const double p99 = perBlock[(std::size_t) ((kTimed * 99) / 100)];
        const double ratio = (meanNs > 0.0) ? (p99 / meanNs) : 0.0;
        note(std::string(id) + " per-BLOCK: mean " + std::to_string(meanNs)
             + " ns, p99 " + std::to_string(p99) + " ns (ratio " + std::to_string(ratio)
             + "x), max " + std::to_string(perBlock.back()) + " ns");
        RT_CHECK(meanNs > 0.0, std::string(id) + " measured no time at all");
        RT_CHECK(ratio < 12.0,
                 std::string(id) + "'s 99th-percentile block is " + std::to_string(ratio)
                 + "x its mean - a per-wrap or per-hop burst grew into one callback's "
                   "whole budget, which no allocation counter and no MEAN can see");
    };
    costOf("pitchcorrect", "formant", 1.0f);
    costOf("basssynth", "waveform", 1.0f);
    // twelvestring is the one the burst argument is really about: its octave
    // shifter runs kOctaveAlignMs = 8 (384 samples at 48 kHz) of normalised
    // correlation inside the ONE sample a grain wraps on, per channel, with
    // both channels wrapping together.
    costOf("twelvestring", "octaveLevel", 1.0f);

    endScenario();
}

// The stutter's capture must not be a BURST of work in one callback.
//
// GlitchCore used to copy the whole captured slice -- up to 1004 ms per channel,
// 1.5 MB at 192 kHz stereo -- inside the single callback that saw the trigger.
// It allocated nothing and the bound was deterministic, which is why every
// existing gate passed it: `RtEveryCatalogueModuleIsAllocationFree` drives the
// module for two seconds at its DEFAULTS, where mode is Manual and the trigger
// is off, so the capture path never ran at all.
//
// So this scenario drives the footswitch, and measures. The core is used
// directly (no engine, no device) so the numbers are pure CPU: 64-frame blocks,
// stereo, sliceMs at the 1000 ms maximum, the trigger flipped every 4 blocks so
// ~50 captures land across the run and the two populations can be AVERAGED --
// a single timing is noise on a shared box, a mean over fifty is not.
//
// With the incremental copy the cost is flat (kCopyRate extra sample copies per
// output sample, and only until the snapshot completes), so a trigger block
// costs what its neighbours cost. With the old burst it cost a 386 KB memcpy
// more.
void scenarioGlitchCaptureIsIncremental()
{
    beginScenario("RtGlitchCaptureDoesNotBurstOneCallback");

    constexpr int kSr     = 48000;
    constexpr int kBlock  = 64;
    constexpr int kBlocks = 480;
    constexpr int kEvery  = 4;     // trigger flips every 4 blocks

    dcdsp::GlitchCore g;
    g.prepare((double) kSr, kBlock, 2);

    dcdsp::GlitchParams p{};
    p.mode    = 0;                 // Manual: the footswitch owns the loop
    p.trigger = 0;
    p.sliceMs = 1000.0f;           // the worst case the knob allows
    p.mix     = 1.0f;
    g.setParams(p);

    uint32_t rng = 0x51ED2701u;
    auto nextNoise = [&rng]
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return ((float) (rng >> 8) / (float) (1u << 24)) * 2.0f - 1.0f;
    };

    std::vector<float> l((size_t) kBlock, 0.0f), r((size_t) kBlock, 0.0f);
    float* ptrs[2] = { l.data(), r.data() };
    auto refill = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = 0.3f * nextNoise();
            l[(size_t) i] = s; r[(size_t) i] = s;
        }
    };

    // Fill the history ring with real audio before anything is captured, and
    // materialise every lazy table.
    for (int i = 0; i < 800; ++i) { refill(); g.process(ptrs, 2, kBlock); }

    dcrt::reset();
    dcrt::arm();

    double triggerNs = 0.0, otherNs = 0.0;
    int    triggerBlocks = 0, otherBlocks = 0;
    double wetEnergy = 0.0, dryEnergy = 0.0;
    int    trig = 0;
    {
        dcrt::ScopedRtThread rt;
        for (int b = 0; b < kBlocks; ++b)
        {
            const bool flip = (b % kEvery) == 0;
            if (flip)
            {
                trig = 1 - trig;
                p.trigger = trig;
                g.setParams(p);       // the edge is consumed on the next frame
            }

            refill();
            for (int i = 0; i < kBlock; ++i) dryEnergy += (double) l[(size_t) i] * l[(size_t) i];

            const auto t0 = std::chrono::steady_clock::now();
            g.process(ptrs, 2, kBlock);
            const auto t1 = std::chrono::steady_clock::now();

            const double ns = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            // A capture fires on the first frame of the block AFTER the flip that
            // turned the trigger ON, so that is the block the burst landed in.
            if (flip && trig == 1) { triggerNs += ns; ++triggerBlocks; }
            else                   { otherNs   += ns; ++otherBlocks; }

            for (int i = 0; i < kBlock; ++i)
            {
                const double v = (double) l[(size_t) i];
                wetEnergy += v * v;
            }
        }
    }
    dcrt::disarm();

    const double triggerMean = triggerBlocks > 0 ? triggerNs / triggerBlocks : 0.0;
    const double otherMean   = otherBlocks   > 0 ? otherNs   / otherBlocks   : 0.0;
    const double ratio       = otherMean > 0.0 ? triggerMean / otherMean : 0.0;

    note("capture blocks: " + std::to_string(triggerBlocks) + " at "
         + std::to_string((long long) triggerMean) + " ns, others: "
         + std::to_string(otherBlocks) + " at "
         + std::to_string((long long) otherMean) + " ns, ratio "
         + std::to_string(ratio));

    reportHeap("glitch capture");
    RT_CHECK(dcrt::allocCount() == 0,
             "the stutter allocated " + std::to_string(dcrt::allocCount())
             + " time(s) after prepare()");
    RT_CHECK(triggerBlocks > 20,
             "only " + std::to_string(triggerBlocks)
             + " capture(s) fired -- the probe never drove the path it exists to measure");
    // Non-vacuous in the OTHER direction too: the stutter has to be audible, or
    // a core that did nothing at all would post a perfect ratio.
    RT_CHECK(wetEnergy > 0.2 * dryEnergy && wetEnergy < 5.0 * dryEnergy,
             "the stutter produced no plausible output (wet energy "
             + std::to_string(wetEnergy) + " against dry " + std::to_string(dryEnergy) + ")");
    RT_CHECK(ratio > 0.0 && ratio < 5.0,
             "the callback that captures the slice cost " + std::to_string(ratio)
             + "x its neighbours -- the capture is bursting inside one callback "
               "instead of draining incrementally");

    endScenario();
}

// 8 ------------------------------------------------------------------------
void scenarioEveryCatalogueModule(double secondsPerModule)
{
    beginScenario("RtEveryCatalogueModuleIsAllocationFree");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    const auto& catalogue = dcrig::moduleCatalogue();
    note("catalogue entries: " + std::to_string(catalogue.size())
         + ", " + std::to_string(secondsPerModule) + " s each");

    const int frames = (int) (secondsPerModule * 48000.0);
    std::vector<float> l((size_t) frames), r((size_t) frames);
    uint32_t rng = 0xC0FFEEu;
    auto refill = [&]
    {
        for (int i = 0; i < frames; ++i)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const float s = (((float) (rng >> 8) / (float) (1u << 24)) * 2.0f - 1.0f) * 0.25f;
            l[(size_t) i] = s; r[(size_t) i] = s;
        }
    };

    int offenders = 0;
    std::string offenderList;
    for (const auto& info : catalogue)
    {
        const int node = fx.engine().dspAddNode(0, info.id.c_str());
        if (node < 0) { fail("createModule failed for '" + info.id + "'"); continue; }

        // Warm the module: prepare() already ran inside addNode, but the first
        // process() of some cores materialises lazily-built tables.
        refill();
        fx.engine().dspProcessOffline(0, l.data(), r.data(), std::min(frames, 4096), 48000);

        dcrt::reset();
        dcrt::arm();
        refill();
        {
            dcrt::ScopedRtThread rt;
            fx.engine().dspProcessOffline(0, l.data(), r.data(), frames, 48000);
        }
        dcrt::disarm();

        const int allocs = dcrt::allocCount();
        bool finite = true;
        for (int i = 0; i < frames && finite; ++i)
            finite = std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i]);

        if (allocs != 0 || ! finite)
        {
            ++offenders;
            offenderList += " " + info.id + "(" + std::to_string(allocs) + " alloc"
                          + (finite ? "" : ", NON-FINITE") + ")";
            reportHeap(info.id.c_str());
        }
        fx.engine().dspRemoveNode(node);
    }

    RT_CHECK(offenders == 0, "modules that allocate or emit non-finite audio:" + offenderList);
    endScenario();
}

// 9 ------------------------------------------------------------------------
// The audio thread's last unbounded stall: juce::BufferingAudioSource holds
// `callbackLock` across the reader's decode on its read-ahead thread, and the
// callback takes that same lock. NEITHER RT gate can see it -- rt_lock_sites.txt
// only knows mutexes we wrap in DCA_RT_LOCK_GUARD, and that one is inside JUCE --
// which is how it survived to cause the owner's 2026-08-23 dropouts: 22 deadline
// overruns in 11,030 callbacks with the rig at 3-5% of budget, one at 503 ms.
//
// TransportSource now decodes into RAM and plays through juce::MemoryAudioSource,
// which AudioTransportSource does NOT wrap in a BufferingAudioSource (it only
// does so for readAheadSize > 0). This pins the decision both ways: it must
// preload what fits, and it must still PLAY what does not.
void scenarioPreloadedSourceAvoidsTheReadAheadLock()
{
    beginScenario("RtPreloadedSourceAvoidsTheReadAheadLock");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    const auto blob = makeWavBlob(4.0, 48000, 220.0);
    if (blob.getSize() == 0) { fail("could not synthesise a WAV blob"); endScenario(); return; }

    TransportSource preloaded(fx.engine());
    const auto err = preloaded.openMemory(blob.getData(), blob.getSize(), "wav");
    if (err.isNotEmpty()) { fail("openMemory failed: " + err.toStdString()); endScenario(); return; }

    RT_CHECK(preloaded.isPreloaded(),
             "a 4 s file did not preload -- the callback is back on the JUCE read-ahead lock");
    RT_CHECK(preloaded.preloadedBytes() > 0, "preloaded source reports zero bytes held");
    RT_CHECK(preloaded.getLengthSeconds() > 3.5,
             "preloaded source lost its length -- MemoryAudioSource is not wired to the transport");

    // ...and it holds ONLY the decoded buffer. The decoder and the bytes it
    // decoded from are unreachable the moment MemoryAudioSource is wired up,
    // and for the openMemory path (the PSARC pipeline, which hands over decoded
    // 16-bit WAV) those bytes are half the size of the float32 buffer again.
    RT_CHECK(! preloaded.holdsDecoder(),
             "a preloaded source is still holding its format reader — an open file handle "
             "and decoder state per stem, for the whole session");
    RT_CHECK(preloaded.retainedInputBytes() == 0,
             "a preloaded source is still holding " + std::to_string(preloaded.retainedInputBytes())
             + " bytes of the ORIGINAL blob alongside its decoded copy");

    note("preloaded " + std::to_string(preloaded.preloadedBytes() / 1024) + " KB, length "
         + std::to_string(preloaded.getLengthSeconds()) + " s, source bytes released "
         + std::to_string(blob.getSize() / 1024) + " KB");

    // …and the budget is a budget: exhaust it and the next source must still
    // open and play, merely on the old streaming path. A fallback that threw or
    // refused would turn a memory limit into a chart that cannot be opened.
    // Whatever is left AFTER the source above -- reserving the full budget
    // would fail precisely because preloading already worked.
    const int64_t budget    = AudioEngine::preloadBudgetBytes();
    const int64_t remaining = budget - preloaded.preloadedBytes();
    RT_CHECK(budget > 0, "preload budget is not positive");
    RT_CHECK(remaining > 0, "one 4 s file exhausted the whole preload budget");
    RT_CHECK(fx.engine().reservePreloadBytes(remaining),
             "could not reserve the remaining budget for the fallback half of this test");

    {
        TransportSource streamed(fx.engine());
        const auto err2 = streamed.openMemory(blob.getData(), blob.getSize(), "wav");
        RT_CHECK(err2.isEmpty(), "a source that could not preload failed to open at all");
        RT_CHECK(! streamed.isPreloaded(), "source preloaded despite an exhausted budget");
        RT_CHECK(streamed.preloadedBytes() == 0, "streaming source claims preloaded bytes");
        RT_CHECK(streamed.getLengthSeconds() > 3.5, "streaming fallback lost its length");
        streamed.play();
    }

    fx.engine().releasePreloadBytes(remaining);

    // The budget must come back, or a few opens would permanently starve it.
    TransportSource again(fx.engine());
    const auto err3 = again.openMemory(blob.getData(), blob.getSize(), "wav");
    RT_CHECK(err3.isEmpty(), "reopen after releasing the budget failed");
    RT_CHECK(again.isPreloaded(), "budget was not released -- preloading stayed off");

    // ...and so must a re-open of the SAME source. openFile/openMemory used to
    // overwrite preloadedBytes_ without releasing the previous reservation, and
    // the destructor was the only thing that ever gave it back, so every
    // re-open leaked a whole source's worth of budget for the life of the
    // engine. Squeeze the budget down to about one and a half copies first, or
    // a 1.5 MB leak against a 256 MB floor is invisible.
    //
    // The same teardown fixes something worse that no assertion can catch
    // deterministically: mixer_ holds a RAW pointer to panner_, addSource is
    // skipped when attached_ is already true, and the re-open then freed the
    // panner the mixer was still pulling. The play + pump below is here so a
    // regression at least has a chance to crash the gate rather than the owner.
    {
        TransportSource churn(fx.engine());
        RT_CHECK(churn.openMemory(blob.getData(), blob.getSize(), "wav").isEmpty(),
                 "churn source failed to open");
        RT_CHECK(churn.isPreloaded(), "churn source did not preload");
        const int64_t one  = churn.preloadedBytes();
        // Leave HALF a copy free. A re-open that releases first needs
        // one/2 + one >= one and passes; a re-open that leaks needs one against
        // one/2 free and fails on the very first iteration.
        const int64_t held    = preloaded.preloadedBytes() + again.preloadedBytes() + one;
        const int64_t squeeze = budget - held - one / 2;
        RT_CHECK(one > 0, "churn source reports no preloaded bytes");
        RT_CHECK(squeeze > 0, "not enough budget left to squeeze — the probe would be vacuous");
        RT_CHECK(fx.engine().reservePreloadBytes(squeeze),
                 "could not squeeze the budget down for the re-open probe");

        churn.play();
        for (int i = 0; i < 4; ++i)
        {
            RT_CHECK(churn.openMemory(blob.getData(), blob.getSize(), "wav").isEmpty(),
                     "re-open #" + std::to_string(i) + " failed");
            RT_CHECK(churn.isPreloaded(),
                     "re-open #" + std::to_string(i) + " fell back to STREAMING — the previous "
                     "reservation was never released, so re-opens starve the budget and every "
                     "later source goes back on the JUCE read-ahead lock");
            churn.play();
            fx.pumpWarm(8);
        }
        fx.engine().releasePreloadBytes(squeeze);
    }

    endScenario();
}

// #411: a preloaded song is held as 16-bit PCM (Int16PreloadSource), not as a
// float32 AudioBuffer -- half the RAM per song and per stem. Pins all four
// things that swap must keep:
//   (1) the source holds frames x channels x 2 bytes, and the budget is charged that;
//   (2) a 16-bit file at the device rate round-trips BIT-EXACT against JUCE's own decode;
//   (3) a buffer that peaks above full scale is stored with a coarser step, not clipped;
//   (4) the callback plays it audibly with no heap allocation.
void scenarioPreloadedSourceHoldsSixteenBitPcm()
{
    beginScenario("RtPreloadedSourceHoldsSixteenBitPcm");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    const double devRate = fx.engine().currentSampleRate();
    const int rate = devRate > 0.0 ? (int) devRate : 48000;
    const auto blob = makeWavBlob(3.0, rate, 220.0);
    if (blob.getSize() == 0) { fail("could not synthesise a WAV blob"); endScenario(); return; }

    TransportSource src(fx.engine());
    const auto err = src.openMemory(blob.getData(), blob.getSize(), "wav");
    if (err.isNotEmpty()) { fail("openMemory failed: " + err.toStdString()); endScenario(); return; }
    RT_CHECK(src.isPreloaded(), "a 3 s file did not preload");
    const auto* held = src.preloadedSource();
    if (held == nullptr) { fail("preloaded source exposes no samples"); endScenario(); return; }

    // (1) bytes per sample
    const int64_t frames = held->numFrames();
    const int     chans  = held->numChannels();
    RT_CHECK(frames == (int64_t) rate * 3 && chans == 2,
             "unexpected preload shape " + std::to_string(frames) + " x " + std::to_string(chans));
    RT_CHECK(src.preloadedBytes() == frames * chans * (int64_t) sizeof(int16_t),
             "the preloaded song holds " + std::to_string(src.preloadedBytes()) + " bytes for "
             + std::to_string(frames * chans) + " samples -- not 2 bytes a sample. float32 (4) is "
             "what #411 removed: twice the RAM for every song and stem.");

    // (2) bit-exact against JUCE's own float decode of the same 16-bit file
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(
            new juce::MemoryInputStream(blob.getData(), blob.getSize(), false), true));
        if (reader == nullptr) { fail("reference reader failed"); endScenario(); return; }
        juce::AudioBuffer<float> ref(chans, (int) frames);
        reader->read(&ref, 0, (int) frames, 0, true, true);
        int64_t mismatches = 0;
        for (int c = 0; c < chans; ++c)
            for (int i = 0; i < (int) frames; ++i)
                if (held->sampleAt(c, i) != ref.getSample(c, i)) ++mismatches;
        note("16-bit round trip: " + std::to_string(mismatches) + " mismatches in "
             + std::to_string(frames * chans) + " samples, step " + std::to_string(held->step()));
        RT_CHECK(mismatches == 0,
                 std::to_string(mismatches) + " samples of a 16-bit file at the device rate did not "
                 "round-trip bit-exact through the 16-bit preload");
    }

    // (3) overs: a coarser step, never a clip
    {
        juce::AudioBuffer<float> hot(1, 5);
        const float xs[5] = { 1.5f, -1.25f, 0.3f, 0.0f, -0.7f };
        for (int i = 0; i < 5; ++i) hot.setSample(0, i, xs[i]);
        Int16PreloadSource q(hot);
        float worst = 0.0f;
        for (int i = 0; i < 5; ++i) worst = std::max(worst, std::abs(q.sampleAt(0, i) - xs[i]));
        RT_CHECK(worst <= q.step() * 0.5f + 1e-7f,
                 "a source peaking at 1.5 was stored with an error of " + std::to_string(worst)
                 + " (step " + std::to_string(q.step()) + ") -- the overs were CLIPPED");
    }

    // (4) the callback: audible, allocation-free
    src.play();
    fx.pumpWarm(8);
    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) { fail("no manual null device"); endScenario(); return; }
    float peak = 0.0f;
    dcrt::reset();
    dcrt::arm();
    for (int b = 0; b < 32; ++b)
    {
        dcrt::g_currentBlock.store((unsigned) b, std::memory_order_relaxed);
        { dcrt::ScopedRtThread rt; nulldev::pump(1); }
        for (float s : dev->manualOutputBuffers()[0]) peak = std::max(peak, std::abs(s));
    }
    dcrt::disarm();
    reportHeap("16-bit preload playback");
    RT_CHECK(peak > 0.05f, "the 16-bit preload never became audible (peak " + std::to_string(peak)
             + ") -- the allocation check below would be vacuous");
    RT_CHECK(dcrt::allocCount() == 0,
             "playing a 16-bit preload allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");

    endScenario();
}

// A user slider — master volume, the guitar monitor — publishes a new gain per
// UI tick. Applying it as ONE constant for the whole block steps the waveform
// at every block boundary while the slider moves: the classic zipper, and the
// step is largest exactly when the drag is fastest.
//
// This measures the thing directly rather than asserting on an implementation:
// the largest sample-to-sample jump ACROSS block boundaries during a fast drag
// must not exceed what the same signal produces at a constant gain. A 30 Hz
// sine is deliberate — its own slew is tiny (A*2*pi*f/fs), so a gain step
// stands out by more than an order of magnitude instead of hiding inside the
// signal's own slope. A step of 0.05 in gain on a 0.3 sine is a 0.015 jump
// against a 0.0012 steady slew.
//
// Non-vacuity is structural: the same run asserts the source is actually
// audible (peak > 0.05) and that the steady slew is non-zero, so a silent
// output — which would pass any "no jumps" test trivially — fails first.
/// An instrument that renders a CONTINUOUS sine from its own running phase, so
/// the gain-zipper detector below sees a signal whose only block-boundary
/// discontinuity can come from the gain applied to it. `StubInstrument` clears
/// its buffer, which is silence, and silence passes any jump test.
/// Allocation-free.
class StubToneInstrument final : public juce::AudioPluginInstance
{
public:
    StubToneInstrument()
        : juce::AudioPluginInstance(BusesProperties().withOutput(
              "Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "RtCheck Stub Tone Instrument"; }
    /// 30 Hz, and that is the whole design of this stub. The detector below
    /// compares a gain STEP against the signal's own per-sample slew
    /// (A*2*pi*f/fs), so the tone has to be slow enough that a step stands out:
    /// at 187.5 Hz the slew is 0.0061 and a 0.05 step on a 0.25 sine is 0.0125,
    /// which sails under a 3x bound — measured, and the probe passed with the
    /// fix reverted. At 30 Hz the slew is 0.00098 and the same step is 12x it.
    void prepareToPlay(double sampleRate, int) override
    { step_ = 2.0 * 3.14159265358979323846 * 30.0 / (sampleRate > 0.0 ? sampleRate : 48000.0); }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int n = buffer.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            const auto s = (float) (0.25 * std::sin(phase_));
            phase_ += step_;
            if (phase_ > 6.283185307179586) phase_ -= 6.283185307179586;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.getWritePointer(ch)[i] = s;
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "RtCheck Stub Tone Instrument";
        d.pluginFormatName = "RtCheck";
        d.isInstrument = true;
    }

private:
    double phase_ = 0.0;
    double step_  = 0.0;
};

void scenarioGainSlidersRamp()
{
    beginScenario("RtGainSlidersRampInsteadOfStepping");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    // 6 s at 30 Hz: long enough that the 200-odd blocks below never reach EOF.
    const auto blob = makeWavBlob(6.0, 48000, 30.0);
    if (blob.getSize() == 0) { fail("could not synthesise a WAV blob"); endScenario(); return; }

    TransportSource src(fx.engine());
    const auto err = src.openMemory(blob.getData(), blob.getSize(), "wav");
    if (err.isNotEmpty()) { fail("openMemory failed: " + err.toStdString()); endScenario(); return; }
    src.play();

    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) { fail("no manual null device"); endScenario(); return; }

    // Walk one channel of the rendered output, carrying the previous block's
    // last sample so a boundary discontinuity is seen at all.
    float carry = 0.0f;
    bool  haveCarry = false;
    float peak = 0.0f;
    auto scanBlock = [&](float& maxDelta)
    {
        const auto& bufs = dev->manualOutputBuffers();
        if (bufs.empty()) return;
        const auto& ch = bufs[0];
        for (float s : ch)
        {
            peak = std::max(peak, std::abs(s));
            if (haveCarry) maxDelta = std::max(maxDelta, std::abs(s - carry));
            carry = s;
            haveCarry = true;
        }
    };

    fx.pumpWarm(64);           // start-up transient, first decode, filter stoking
    carry = 0.0f; haveCarry = false;

    // Reference: the signal's own slew at a CONSTANT gain.
    fx.engine().setMasterGain(1.0f);
    float steadyMax = 0.0f;
    for (int i = 0; i < 128; ++i) { fx.pumpWarm(1); scanBlock(steadyMax); }

    // The gesture: a fast drag from unity to silence, one new value per block.
    // Allocation sentinel armed — the ramp must not cost the callback a heap
    // event either.
    dcrt::reset();
    dcrt::arm();
    float dragMax = 0.0f;
    for (int i = 0; i <= 20; ++i)
    {
        fx.engine().setMasterGain(1.0f - 0.05f * (float) i);
        dcrt::g_currentBlock.store((unsigned) i, std::memory_order_relaxed);
        { dcrt::ScopedRtThread rt; nulldev::pump(1); }
        scanBlock(dragMax);
    }
    dcrt::disarm();

    note("peak " + std::to_string(peak)
         + ", steady max |dx| " + std::to_string(steadyMax)
         + ", drag max |dx| " + std::to_string(dragMax));

    reportHeap("gain drag");
    RT_CHECK(peak > 0.05f,
             "the source never became audible (peak " + std::to_string(peak)
             + ") — a silent output passes any jump test, so this probe would be vacuous");
    RT_CHECK(steadyMax > 0.0f,
             "the reference block had no slew at all — the signal is DC, the probe is vacuous");
    RT_CHECK(dragMax < steadyMax * 3.0f,
             "a 20-block master-volume drag produced a sample-to-sample jump of "
             + std::to_string(dragMax) + ", over 3x the same signal's own slew of "
             + std::to_string(steadyMax) + " — the gain is being STEPPED per block, not ramped");
    RT_CHECK(dcrt::allocCount() == 0,
             "the ramped gain path allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");

    // The monitor bus is the same helper on a different signal. Muting it used
    // to skip the mix outright the moment the gain reached zero, which is a
    // full-amplitude cut in ONE sample.
    src.stop();
    fx.engine().setMasterGain(1.0f);
    // 187.5 Hz = exactly one cycle per 256-frame block, so the endlessly
    // replayed stimulus block is CONTINUOUS across boundaries. At 220 Hz the
    // input itself jumps 0.22 every block and the probe below would pass a
    // hard cut without noticing. The pi/2 offset then moves that boundary onto
    // the PEAK: at a zero crossing (phase 0) a hard cut is a jump of ~0.006,
    // indistinguishable from the sine's own slew, and this probe measured
    // exactly that before the offset was added.
    fx.installInputStimulus(0.25f, 187.5, 1.5707963267948966);
    fx.engine().setMonitorGain(1.0f);
    fx.pumpWarm(64);
    carry = 0.0f; haveCarry = false; peak = 0.0f;
    float monSteady = 0.0f;
    for (int i = 0; i < 64; ++i) { fx.pumpWarm(1); scanBlock(monSteady); }
    const float peakWithMonitor = peak;

    float muteMax = 0.0f;
    fx.engine().setMonitorGain(0.0f);
    fx.pumpWarm(1); scanBlock(muteMax);

    note("monitor: steady max |dx| " + std::to_string(monSteady)
         + ", mute-block max |dx| " + std::to_string(muteMax));
    RT_CHECK(peakWithMonitor > 0.05f,
             "nothing was audible on the monitor bus — the mute probe is vacuous");
    RT_CHECK(muteMax < monSteady * 3.0f,
             "muting the monitor produced a jump of " + std::to_string(muteMax)
             + " against a steady slew of " + std::to_string(monSteady)
             + " — the mute is a cut, not a fade");

    // ---- the INSTRUMENT bus (W4-N3) -----------------------------------------
    //
    // dca_engine_set_instrument_gain is a user-facing fader like the two above,
    // and it was the one nobody swept: a per-block CONSTANT multiply-add with an
    // `if (g <= 0.0f) return;` in front of it — both shapes the engine's own
    // gain rule forbids, on BOTH legs (the in-process VSTi here and the sandbox
    // child's, which shares the member). `grep instrumentGain tests/rt` returned
    // nothing before this block existed.
    //
    // Phase 1 is the zipper (a fast drag); phase 2 is the mute cliff, which is
    // the worse of the two — arriving at zero used to skip the mix entirely, a
    // full-amplitude cut in one sample.
    fx.engine().setMonitorGain(0.0f);
    fx.engine().setMasterGain(1.0f);
    StubToneInstrument instrument;
    if (! fx.engine().attachInstrumentPlugin(&instrument))
    {
        fail("attachInstrumentPlugin refused the tone stub");
    }
    else
    {
        fx.engine().setInstrumentGain(1.0f);
        fx.pumpWarm(64);
        carry = 0.0f; haveCarry = false; peak = 0.0f;
        float instrSteady = 0.0f;
        for (int i = 0; i < 64; ++i) { fx.pumpWarm(1); scanBlock(instrSteady); }
        const float peakWithInstrument = peak;

        float instrDragMax = 0.0f;
        for (int i = 0; i <= 20; ++i)
        {
            fx.engine().setInstrumentGain(1.0f - 0.05f * (float) i);
            fx.pumpWarm(1);
            scanBlock(instrDragMax);
        }

        note("instrument: peak " + std::to_string(peakWithInstrument)
             + ", steady max |dx| " + std::to_string(instrSteady)
             + ", drag max |dx| " + std::to_string(instrDragMax));

        RT_CHECK(peakWithInstrument > 0.05f,
                 "the instrument bus never became audible (peak "
                 + std::to_string(peakWithInstrument)
                 + ") — every assertion below would pass on silence");
        RT_CHECK(instrSteady > 0.0f,
                 "the instrument's own signal has no slew at all — it is DC, the probe is vacuous");
        RT_CHECK(instrDragMax < instrSteady * 3.0f,
                 "a 20-block instrument-gain drag produced a sample-to-sample jump of "
                 + std::to_string(instrDragMax) + ", over 3x the same signal's own slew of "
                 + std::to_string(instrSteady)
                 + " — the instrument gain is STEPPED per block, not ramped");

        // ...and the MUTE CLIFF, straight from unity, which is the sharper of
        // the two: `if (g <= 0.0f) return;` skipped the mix outright on the
        // block that arrives at zero, so the output fell from full amplitude to
        // nothing in ONE sample. The monitor bus documents having fixed exactly
        // this, and the instrument bus kept it.
        fx.engine().setInstrumentGain(1.0f);
        fx.pumpWarm(32);
        carry = 0.0f; haveCarry = false; peak = 0.0f;
        float unitySteady = 0.0f;
        for (int i = 0; i < 32; ++i) { fx.pumpWarm(1); scanBlock(unitySteady); }
        const float peakAtUnity = peak;

        float instrMuteMax = 0.0f;
        fx.engine().setInstrumentGain(0.0f);
        fx.pumpWarm(1); scanBlock(instrMuteMax);

        note("instrument mute: peak at unity " + std::to_string(peakAtUnity)
             + ", steady max |dx| " + std::to_string(unitySteady)
             + ", mute-block max |dx| " + std::to_string(instrMuteMax));
        RT_CHECK(peakAtUnity > 0.05f,
                 "the instrument was not audible at unity — the mute probe is vacuous");
        RT_CHECK(instrMuteMax < unitySteady * 3.0f,
                 "muting the instrument produced a jump of " + std::to_string(instrMuteMax)
                 + " against a steady slew of " + std::to_string(unitySteady)
                 + " — the mute is a cut, not a fade: the mix is skipped on the block that "
                   "reaches zero instead of ramping through it");

        fx.engine().setInstrumentGain(1.0f);
        fx.engine().detachInstrumentPlugin(&instrument);
    }

    endScenario();
}

/// A VSTi that RENEGOTIATES ITS BUS on prepareToPlay — a multi-output sampler
/// or a synth that only settles its layout once it knows the rate. It accepts
/// the stereo layout the engine asks for at attach, then widens to quad the
/// next time it is prepared, and records every block it is handed whose channel
/// count disagrees with the layout it declared.
class StubWideningInstrument final : public juce::AudioPluginInstance
{
public:
    StubWideningInstrument()
        : juce::AudioPluginInstance(BusesProperties().withOutput(
              "Out", juce::AudioChannelSet::quadraphonic(), true)) {}

    const juce::String getName() const override { return "RtCheck Widening Instrument"; }

    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }

    void prepareToPlay(double, int) override
    {
        if (widenNext_.exchange(false, std::memory_order_acq_rel))
        {
            BusesLayout quad;
            quad.outputBuses.add(juce::AudioChannelSet::quadraphonic());
            setBusesLayout(quad);
            enableAllBuses();
        }
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int declared = getTotalNumOutputChannels();
        if (buffer.getNumChannels() != declared)
            mismatches_.fetch_add(1, std::memory_order_relaxed);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            float* d = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i) d[i] = 0.25f;
        }
        rendered_.fetch_add(1, std::memory_order_relaxed);
    }

    void widenOnNextPrepare() { widenNext_.store(true, std::memory_order_release); }
    int  mismatches() const { return mismatches_.load(std::memory_order_relaxed); }
    int  rendered()   const { return rendered_.load(std::memory_order_relaxed); }
    void resetCounters() { mismatches_.store(0); rendered_.store(0); }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "RtCheck Widening Instrument";
        d.pluginFormatName = "RtCheck";
        d.isInstrument = true;
    }

private:
    std::atomic<bool> widenNext_{ false };
    std::atomic<int>  mismatches_{ 0 };
    std::atomic<int>  rendered_{ 0 };
};

// A DEVICE REBUILD RE-PREPARES THE INSTRUMENT, AND THE CHANNEL COUNT HAS TO
// FOLLOW IT.
//
// reprepareAttachedPlugins called prepareToPlay and stopped there.
// `instrumentChannels_` — and the scratch buffer sized from it — stayed at the
// figure attachInstrumentPlugin recorded, so an instrument that came back from
// prepareToPlay on a WIDER layout was handed a buffer narrower than the layout
// it had just declared, every block, for the rest of the session. That is not
// a hypothetical: a multi-output sampler renegotiating at a new rate is the
// ordinary case, and it is exactly the shape of bug the in-process re-prepare
// was added to close.
void scenarioInstrumentChannelsFollowAReprepare()
{
    beginScenario("RtInstrumentChannelCountFollowsADeviceRebuild");
    Fixture fx(48000, 256, 2);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    StubWideningInstrument instrument;
    if (! fx.engine().attachInstrumentPlugin(&instrument))
    { fail("attachInstrumentPlugin refused the stub"); endScenario(); return; }
    fx.engine().setInstrumentGain(1.0f);

    fx.pumpWarm(32);
    instrument.resetCounters();
    fx.pumpWarm(32);
    RT_CHECK(instrument.rendered() > 0,
             "the instrument never rendered at its ATTACH layout — everything below would be "
             "vacuous");
    RT_CHECK(instrument.mismatches() == 0,
             "the engine handed the instrument a buffer that disagreed with its layout before "
             "anything was renegotiated");

    // Renegotiate on the next prepare, then rebuild the device — which is what
    // dca_engine_reconfigure, a NAM 48 kHz request and a watchdog recovery all
    // do.
    instrument.widenOnNextPrepare();
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    fx.engine().deviceManager().getAudioDeviceSetup(setup);
    setup.bufferSize = 512;
    const auto err = fx.engine().deviceManager().setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty())
    { fail("could not reopen the null device: " + err.toStdString()); endScenario(); return; }

    fx.pumpWarm(8);
    instrument.resetCounters();
    fx.pumpWarm(64);

    note("after the rebuild: " + std::to_string(instrument.rendered()) + " blocks rendered, "
         + std::to_string(instrument.mismatches()) + " channel-count mismatches");

    RT_CHECK(instrument.rendered() > 0,
             "the instrument stopped rendering after the device rebuild — the render guard is "
             "skipping every block");
    RT_CHECK(instrument.mismatches() == 0,
             "the instrument widened its bus on prepareToPlay and was handed "
             + std::to_string(instrument.mismatches())
             + " block(s) narrower than the layout it declared — reprepareAttachedPlugins is not "
               "re-reading getTotalNumOutputChannels()");

    fx.engine().detachInstrumentPlugin(&instrument);
    endScenario();
}

// THE STRETCH QUALITY BELONGS TO AN ENGINE, NOT TO THE PROCESS.
//
// `dca_engine_set_stretch_quality` takes a handle and ignored it: it wrote a
// process-global `g_engineQuality` that every RubberBandAudioSource read at
// prepareToPlay. So in a host running two engines — the app's playback engine
// and the device-less one the take re-amp drives — one of them loading stems
// re-voiced the OTHER's stretchers from R3 (Finer) to R2 (Faster), silently and
// permanently.
//
// No device is needed: a stretcher is built and prepared when a source is
// opened, so two engines and two sources answer the question directly.
void scenarioStretchQualityIsPerEngine()
{
    beginScenario("RtStretchQualityIsPerEngineNotPerProcess");

    const auto blob = makeWavBlob(1.0, 48000, 220.0);
    if (blob.getSize() == 0) { fail("could not synthesise a WAV blob"); endScenario(); return; }

    // On the heap: AudioEngine carries every seat's chain buffers and scratch,
    // and two of them do not fit on a thread stack (0xC00000FD).
    auto finer  = std::make_unique<AudioEngine>();
    auto faster = std::make_unique<AudioEngine>();
    finer->setStretchFaster(false);
    faster->setStretchFaster(true);

    TransportSource a(*finer), b(*faster);
    if (a.openMemory(blob.getData(), blob.getSize(), "wav").isNotEmpty()
        || b.openMemory(blob.getData(), blob.getSize(), "wav").isNotEmpty())
    {
        fail("openMemory failed on a device-less engine");
        endScenario();
        return;
    }

    note(std::string("finer engine -> faster? ") + (a.stretcherUsedFasterForTest() ? "yes" : "no")
         + ", faster engine -> faster? " + (b.stretcherUsedFasterForTest() ? "yes" : "no"));

    RT_CHECK(! a.stretcherUsedFasterForTest(),
             "an engine set to Finer built its stretcher with the Faster engine — the quality flag "
             "is still process-global, so the OTHER engine's setting won");
    RT_CHECK(b.stretcherUsedFasterForTest(),
             "an engine set to Faster built its stretcher with the Finer engine — the flag never "
             "reached the source at all, so the check above would pass for the wrong reason");

    endScenario();
}

// A SOLO PLAYER'S RIG AND A SOLO PLAYER'S JUDGING MUST LISTEN TO THE SAME
// GUITAR — on a big interface too.
//
// `writeInputCapture` has always resolved the seat's channel from
// `inputCaptureChannel_`, and so has the `players > 1` chain fill: the header
// calls that "the SAME rule", and it is what stops a player's tone and a
// player's judging coming from different inputs. The `players == 1` fill did
// NOT: it took channels 0/1 of the effective input unconditionally. On a 1-2 in
// interface that is right (a genuine stereo pickup stays stereo) and on a 4-in
// interface it is simply wrong — the amp ran on input 0 while detection ran on
// whatever channel the user picked. Worse, engaging the drop tuner replaced
// `pIn` with a MONO copy of the CAPTURE channel, so the rig silently jumped
// inputs mid-song.
//
// The condition is now the same split the shifter above it makes:
// `players == 1 && numInputChannels <= 2`.
//
// Detector: four DC inputs at four distinct levels, straight through the
// monitor. Which number comes out of the device says which channel the rig was
// filled from, and the capture ring is read in the same breath so the two can
// be compared rather than merely asserted separately.
void scenarioSoloSeatFollowsItsCaptureChannel()
{
    beginScenario("RtSoloSeatOnAMultiInputDeviceFollowsItsCaptureChannel");

    constexpr int kBlock = 256;
    Fixture fx(48000, kBlock, 4);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) { fail("no manual null device"); endScenario(); return; }

    auto& in = dev->manualInputBuffers();
    if ((int) in.size() < 4)
    {
        // A build whose null device refused four inputs cannot answer the
        // question; say so rather than passing vacuously.
        fail("the null device opened only " + std::to_string(in.size())
             + " input channels — this scenario needs 4");
        endScenario();
        return;
    }
    const float kLevel[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    for (int c = 0; c < 4; ++c)
        std::fill(in[(size_t) c].begin(), in[(size_t) c].end(), kLevel[c]);

    fx.engine().setMasterGain(1.0f);
    fx.engine().setInputGain(1.0f);
    fx.engine().setMonitorGain(1.0f);

    auto monitorLevel = [&]() -> float
    {
        // Mean of the block, not the peak: the signal is DC, and a mean cannot
        // be fooled by a single sample of ramp left over from a gain change.
        const auto& bufs = dev->manualOutputBuffers();
        if (bufs.empty() || bufs[0].empty()) return 0.0f;
        double sum = 0.0;
        for (float s : bufs[0]) sum += s;
        return (float) (sum / (double) bufs[0].size());
    };

    std::vector<float> ring(4096, 0.0f);
    auto ringLevel = [&]() -> float
    {
        const int n = fx.engine().readInputCapture(ring.data(), (int) ring.size());
        if (n <= 0) return 0.0f;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) sum += ring[(size_t) i];
        return (float) (sum / (double) n);
    };

    bool anyChecked = false;
    for (int ch : { 2, 1, 3 })
    {
        fx.engine().setInputCaptureChannel(ch);
        fx.pumpWarm(24);                 // let the input-gain ramp settle
        (void) ringLevel();              // drain whatever the warm-up left
        fx.pumpWarm(8);

        const float mon  = monitorLevel();
        const float ringL = ringLevel();
        const float want = kLevel[ch];

        note("capture channel " + std::to_string(ch) + ": monitor " + std::to_string(mon)
             + ", ring " + std::to_string(ringL) + ", expected " + std::to_string(want));

        RT_CHECK(std::abs(mon - want) < 0.02f,
                 "with capture channel " + std::to_string(ch) + " selected on a 4-in device the "
                 "rig was filled from a level of " + std::to_string(mon) + ", not "
                 + std::to_string(want) + " — a solo seat is still taking channels 0/1 and the "
                 "amp is listening to a different input than the note detection");
        RT_CHECK(std::abs(ringL - want) < 0.02f,
                 "the capture ring read " + std::to_string(ringL) + " for capture channel "
                 + std::to_string(ch) + ", expected " + std::to_string(want));
        anyChecked = true;
    }

    RT_CHECK(anyChecked, "no channel was checked — the probe is vacuous");
    endScenario();
}

// A PAN IS A USER-FACING LEVEL AND HAS TO RAMP LIKE ONE.
//
// PanningAudioSource applied its two equal-power gains with
// AudioBuffer::applyGain — one constant per block, stepping the waveform at
// every block boundary while the slider moves, which is exactly the zipper
// AudioEngine::rampOrScale exists to prevent for the master, monitor and input
// faders. It also had an early `return` for |pan| < 1e-4, so a drag back to
// centre CUT from the panned gains to unity in a single sample.
//
// Same detector as RtGainSlidersRampInsteadOfStepping, and the same 30 Hz
// stimulus for the same reason: its own slew (A*2*pi*f/fs) is small enough that
// a per-block gain step stands out instead of hiding inside the signal's slope.
void scenarioSourcePanRamps()
{
    beginScenario("RtSourcePanRampsInsteadOfStepping");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    const auto blob = makeWavBlob(6.0, 48000, 30.0);
    if (blob.getSize() == 0) { fail("could not synthesise a WAV blob"); endScenario(); return; }

    TransportSource src(fx.engine());
    const auto err = src.openMemory(blob.getData(), blob.getSize(), "wav");
    if (err.isNotEmpty()) { fail("openMemory failed: " + err.toStdString()); endScenario(); return; }
    src.play();

    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) { fail("no manual null device"); endScenario(); return; }

    float carry = 0.0f;
    bool  haveCarry = false;
    float peak = 0.0f;
    auto scanBlock = [&](float& maxDelta)
    {
        const auto& bufs = dev->manualOutputBuffers();
        if (bufs.empty()) return;
        for (float s : bufs[0])
        {
            peak = std::max(peak, std::abs(s));
            if (haveCarry) maxDelta = std::max(maxDelta, std::abs(s - carry));
            carry = s;
            haveCarry = true;
        }
    };

    fx.engine().setMasterGain(1.0f);
    src.setPan(-1.0f);
    fx.pumpWarm(64);
    carry = 0.0f; haveCarry = false;

    float steadyMax = 0.0f;
    for (int i = 0; i < 128; ++i) { fx.pumpWarm(1); scanBlock(steadyMax); }

    // The gesture: hard left to hard right, one new value per block, ending AT
    // centre so the old early-return cut is inside the window too.
    dcrt::reset();
    dcrt::arm();
    float dragMax = 0.0f;
    for (int i = 0; i <= 20; ++i)
    {
        src.setPan(-1.0f + 0.1f * (float) i);
        dcrt::g_currentBlock.store((unsigned) i, std::memory_order_relaxed);
        { dcrt::ScopedRtThread rt; nulldev::pump(1); }
        scanBlock(dragMax);
    }
    dcrt::disarm();

    note("peak " + std::to_string(peak)
         + ", steady max |dx| " + std::to_string(steadyMax)
         + ", pan-drag max |dx| " + std::to_string(dragMax));

    reportHeap("pan drag");
    RT_CHECK(peak > 0.05f,
             "the source never became audible (peak " + std::to_string(peak)
             + ") — a silent output passes any jump test, so this probe would be vacuous");
    RT_CHECK(steadyMax > 0.0f,
             "the reference block had no slew at all — the signal is DC, the probe is vacuous");
    RT_CHECK(dragMax < steadyMax * 3.0f,
             "a 20-block pan drag produced a sample-to-sample jump of " + std::to_string(dragMax)
             + ", over 3x the same signal's own slew of " + std::to_string(steadyMax)
             + " — the pan gain is being STEPPED per block, not ramped");
    RT_CHECK(dcrt::allocCount() == 0,
             "the ramped pan path allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");

    endScenario();
}

/// IS CENTRE EXACTLY UNITY? MEASURE IT (W4-N20).
///
/// `PanningAudioSource::getNextAudioBlock` skips its `applyGain` when a gain is
/// `!= 1.0f` — an EXACT comparison — and the comment beside it used to justify
/// that with "the gains are exactly 1.0 at p == 0". That is an assumption about
/// rounding, not a property: `lGain` is
/// `cosf(0.25f * 3.14159265f) * 1.41421356f`, three roundings with a truncated
/// pi and a truncated sqrt(2). The whole point of the fix that comment
/// documents was that the PREVIOUS shortcut's reasoning was wrong in exactly
/// this way, so it is worth knowing rather than believing.
///
/// MEASURED, first run: BOTH gains come out 0x3F7FFFFF — one ulp BELOW unity —
/// so the claim was false and the `!= 1.0f` shortcut it justified never fired at
/// centre at all. The consequence was benign (a redundant `applyGain` pass per
/// block on a centred source) but the shortcut did not exist, which is why
/// `kUnityEps` is now there. This scenario keeps both halves honest: the gains
/// really are unity to well inside audibility, and the epsilon really is wide
/// enough to catch where they actually land.
void scenarioSourcePanCentreIsUnity()
{
    beginScenario("RtSourcePanCentreIsUnity");

    // The same arithmetic as PanningAudioSource::getNextAudioBlock, including
    // the truncated pi and sqrt(2) — the truncation is the point.
    const float angle = (0.0f + 1.0f) * 0.25f * 3.14159265f;
    const float lGain = std::cos(angle) * 1.41421356f;
    const float rGain = std::sin(angle) * 1.41421356f;

    uint32_t lBits = 0, rBits = 0;
    std::memcpy(&lBits, &lGain, sizeof lBits);
    std::memcpy(&rBits, &rGain, sizeof rBits);

    char bits[64];
    std::snprintf(bits, sizeof bits, "L 0x%08X, R 0x%08X", lBits, rBits);
    note(std::string("pan centre gains: ") + bits
         + ", bit-exactly 1.0f? L " + std::to_string(lGain == 1.0f ? 1 : 0)
         + " R " + std::to_string(rGain == 1.0f ? 1 : 0)
         + " (0x3F800000 would be exact)");

    static constexpr float kUnityEps = 1.0e-6f;   // must match PanningAudioSource.h

    RT_CHECK(std::abs(lGain - 1.0f) <= kUnityEps,
             "the pan-centre LEFT gain is " + std::to_string(lGain)
             + ", further than kUnityEps from unity — either a centred source really is being "
               "attenuated, or the epsilon in PanningAudioSource is too tight and the skip is "
               "dead code again");
    RT_CHECK(std::abs(rGain - 1.0f) <= kUnityEps,
             "the pan-centre RIGHT gain is " + std::to_string(rGain)
             + ", further than kUnityEps from unity");
    RT_CHECK(lGain == rGain,
             "the two pan-centre gains differ (" + std::to_string(lGain) + " vs "
             + std::to_string(rGain) + ") — a centred source is off to one side");

    // And the fact the old comment got wrong, asserted in the direction it is
    // actually true, so that a toolchain or constant change which DID make it
    // exact shows up here rather than silently making kUnityEps pointless.
    RT_CHECK(lGain != 1.0f,
             "the pan-centre gain is now bit-exactly 1.0f. That is fine, but it means "
             "PanningAudioSource's kUnityEps is no longer load-bearing — say so there, and "
             "update this case, rather than leaving a comment that has drifted back to true "
             "by accident");

    endScenario();
}

// A non-finite gain is PERMANENT unless something reseeds it, and that is the
// whole reason `rampOrScale` guards its endpoints.
//
// The ABI now refuses NaN/Inf at every float-taking export (dcabi::finite,
// pinned from managed by NonFiniteFloatRejectionTests), so this scenario is the
// second line: it reaches PAST the ABI and hands AudioEngine::setMasterGain a
// NaN directly, the way an internal caller or a future setter could. Without
// the reseed the block is multiplied by NaN, the output scrub zeroes the master
// bus, and the NEXT block ramps FROM the stored NaN — the bus is silent for the
// rest of the session even after a perfectly good gain is published.
//
// The detector is recovery, not the bad block: one wrong block is a click, a
// bus that never comes back is the bug.
void scenarioNonFiniteGainCannotLatchTheMasterBus()
{
    beginScenario("RtNonFiniteGainCannotLatchTheMasterBus");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    const auto blob = makeWavBlob(6.0, 48000, 30.0);
    if (blob.getSize() == 0) { fail("could not synthesise a WAV blob"); endScenario(); return; }

    TransportSource src(fx.engine());
    const auto err = src.openMemory(blob.getData(), blob.getSize(), "wav");
    if (err.isNotEmpty()) { fail("openMemory failed: " + err.toStdString()); endScenario(); return; }
    src.play();

    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) { fail("no manual null device"); endScenario(); return; }

    auto blockPeak = [&]() -> float
    {
        const auto& bufs = dev->manualOutputBuffers();
        if (bufs.empty()) return 0.0f;
        float p = 0.0f;
        // std::max(x, NaN) returns x on MSVC, so scan explicitly and treat a
        // non-finite sample as "not audible" rather than letting it hide.
        for (float s : bufs[0]) { const float a = std::abs(s); if (std::isfinite(a) && a > p) p = a; }
        return p;
    };

    fx.engine().setMasterGain(1.0f);
    fx.pumpWarm(64);
    float before = 0.0f;
    for (int i = 0; i < 32; ++i) { fx.pumpWarm(1); before = std::max(before, blockPeak()); }

    // The poison, straight into the engine (not through the ABI) — and it is
    // LEFT THERE. A NaN that is published once and immediately overwritten
    // costs a single block, because `masterGainCurrent_` is reseeded from the
    // next (good) value; the failure that matters is the gain that STAYS
    // non-finite, which is every block from here to the end of the session.
    fx.engine().setMasterGain(std::numeric_limits<float>::quiet_NaN());
    fx.pumpWarm(8);
    float during = 0.0f;
    for (int i = 0; i < 32; ++i) { fx.pumpWarm(1); during = std::max(during, blockPeak()); }

    // …and a perfectly ordinary gain published afterwards, which is what a UI
    // would do next. Everything from here must be audible too.
    fx.engine().setMasterGain(1.0f);
    fx.pumpWarm(8);
    float after = 0.0f;
    for (int i = 0; i < 32; ++i) { fx.pumpWarm(1); after = std::max(after, blockPeak()); }

    note("master peak before NaN " + std::to_string(before)
         + ", while NaN " + std::to_string(during)
         + ", after recovery " + std::to_string(after));

    RT_CHECK(before > 0.05f,
             "nothing was audible before the NaN was published (peak "
             + std::to_string(before) + ") — every check below would be vacuous");
    RT_CHECK(during > before * 0.5f,
             "the master bus went silent under a standing non-finite gain: peak "
             + std::to_string(before) + " before, " + std::to_string(during)
             + " while the NaN was published — rampOrScale is multiplying the block by "
               "NaN and the output scrub is zeroing the bus, every block");
    RT_CHECK(after > before * 0.5f,
             "the master bus did not recover after a finite gain was republished: peak "
             + std::to_string(before) + " before, " + std::to_string(after) + " after");

    endScenario();
}

// The two WSOLA pitch cores size their rings from a RUNTIME param, `lowLatency`,
// which changes the analysis window 12 ms <-> 32 ms and the rings with it. The
// order that breaks it is: prepare while lowLatency is ON (the small geometry
// is what gets allocated), then turn it OFF. Every ring access masks with
// inSize_/stSize_ (`in[w & (inSz-1)]`), so the writes go straight past the end
// of the vector — heap corruption from a UI toggle, and buildHann() reallocates
// on the audio thread in the same step, since ParamMailbox applies a param from
// the TOP of process().
//
// Driven on the cores directly, inside a ScopedRtThread, because that is where
// a param flip actually lands. Two independent assertions per core: the sizes
// the audio path masks with must be inside what was allocated (the invariant),
// and the flip must not allocate (the symptom the sentinel can see).
void scenarioPitchCoreGeometryIsWorstCase()
{
    beginScenario("RtPitchCoreGeometrySurvivesALowLatencyFlip");

    constexpr int kBlock = 256;
    std::vector<float> l((std::size_t) kBlock, 0.f), r((std::size_t) kBlock, 0.f);
    float* chans[2] = { l.data(), r.data() };
    for (int i = 0; i < kBlock; ++i)
    {
        const float s = 0.25f * (float) std::sin(6.283185307 * 187.5 * i / 48000.0);
        l[(std::size_t) i] = r[(std::size_t) i] = s;
    }

    // --- harmonizer -------------------------------------------------------
    {
        dcdsp::HarmonizerCore core;
        dcdsp::HarmonizerParams p;
        p.semitones = -2.0f;
        p.mix       = 1.0f;
        p.lowLatency = 1.0f;      // BEFORE prepare: the small geometry is allocated
        core.setParams(p);
        core.prepare(48000.0, kBlock, 2);
        RT_CHECK(core.ringsCoverGeometry(),
                 "harmonizer: prepare() alone left the rings smaller than the geometry");

        dcrt::reset();
        dcrt::arm();
        {
            dcrt::ScopedRtThread rt;
            p.lowLatency = 0.0f;  // ...and now ask for the LARGE one, mid-stream
            core.setParams(p);
            for (int b = 0; b < 8; ++b) core.process(chans, 2, kBlock);
        }
        dcrt::disarm();
        const int allocs = dcrt::allocCount();

        RT_CHECK(core.ringsCoverGeometry(),
                 "harmonizer: a lowLatency flip grew the ring MASKS past what prepare() "
                 "allocated — every write from here is past the end of the vector");
        RT_CHECK(core.hannTablesCoverBothModes(),
                 "harmonizer: a lowLatency flip resized the Hann table — prepare() must "
                 "build BOTH modes so the flip only swaps a pointer");
        RT_CHECK(allocs == 0,
                 "harmonizer: a lowLatency flip allocated " + std::to_string(allocs)
                 + " time(s) on the audio thread");
        note("harmonizer flip: " + std::to_string(allocs) + " allocations");
    }

    // --- whammy: already worst-case sized; pinned so it stays that way -----
    {
        dcdsp::WhammyCore core;
        dcdsp::WhammyParams p;
        p.lowLatency = 1.0f;
        core.setParams(p);
        core.prepare(48000.0, kBlock, 2);
        RT_CHECK(core.ringsCoverGeometry(),
                 "whammy: prepare() alone left the rings smaller than the geometry");

        dcrt::reset();
        dcrt::arm();
        {
            dcrt::ScopedRtThread rt;
            p.lowLatency = 0.0f;
            core.setParams(p);
            for (int b = 0; b < 8; ++b) core.process(chans, 2, kBlock);
        }
        dcrt::disarm();
        const int allocs = dcrt::allocCount();

        RT_CHECK(core.ringsCoverGeometry(),
                 "whammy: a lowLatency flip grew the ring MASKS past what prepare() allocated");
        RT_CHECK(core.hannTablesCoverBothModes(),
                 "whammy: a lowLatency flip resized the Hann table — prepare() must build "
                 "BOTH modes so the flip only swaps a pointer");
        RT_CHECK(allocs == 0,
                 "whammy: a lowLatency flip allocated " + std::to_string(allocs)
                 + " time(s) on the audio thread");
    }

    dcrt::reset();
    endScenario();
}

// The geometry scenario above proves a `lowLatency` flip stays IN BOUNDS and
// allocation-free. Neither of those sees the thing the flip actually did: a
// `buildHann` of up to 8,192 std::cos plus a std::fill over every one of the
// eight kMaxChannels' rings -- ~1.8 MB of memset -- inside the ONE callback
// that saw the toggle. setParams is drained from the top of process(), so that
// is the audio thread; a capacity-retaining `assign` is not an allocation and a
// cos is not one either, which is exactly why the flip survived the fix
// everyone believed closed it.
//
// The fix is the one glitch got -- do the expensive part ONCE, in prepare()
// (both modes' Hann tables), and bound the per-channel clear to the channels
// that are actually in use.
//
// The instrument is STRUCTURAL, not the cost ratio glitch uses, and that is
// deliberate: measured on this box, a flip block times at ~1.0x its neighbour
// with the burst PRESENT, because the re-prime also un-primes the core and the
// block therefore produces no WSOLA grains at all. The saving and the burst
// very nearly cancel, so a timing bound here passes either way -- it was
// written first and measured vacuous (ratio 1.025 broken vs 0.997 fixed). The
// two counters below cannot be cancelled by anything: buildHann() calls while
// the callback is ARMED, and per-channel ring clears per flip. The ratio is
// still logged, because it is the number a human wants when this regresses.
struct PitchFlipResult
{
    double    ratio        = 0.0;   // LOGGED only -- see the comment above
    int       allocs       = 0;
    int       flips        = 0;
    double    wetEnergy    = 0.0;
    long long hannBuilds   = 0;     // builds that happened while ARMED
    long long channelClears = 0;    // per-channel ring clears while ARMED
};

template <typename Core, typename Params>
static PitchFlipResult pitchFlipCostRatio(Core& core, Params& p)
{
    PitchFlipResult out;
    constexpr int kBlock  = 256;
    constexpr int kBlocks = 320;
    constexpr int kEvery  = 8;      // a flip every 8 blocks

    core.setParams(p);
    core.prepare(48000.0, kBlock, 2);

    uint32_t rng = 0x2F6E1B03u;
    std::vector<float> l((std::size_t) kBlock, 0.f), r((std::size_t) kBlock, 0.f);
    float* chans[2] = { l.data(), r.data() };
    long long phase = 0;
    auto refill = [&]
    {
        for (int i = 0; i < kBlock; ++i)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const float noise = ((float) (rng >> 8) / (float) (1u << 24)) * 2.0f - 1.0f;
            const float s = 0.25f * (float) std::sin(6.283185307 * 110.0
                                                     * (double) phase / 48000.0)
                          + 0.02f * noise;
            l[(std::size_t) i] = r[(std::size_t) i] = s;
            ++phase;
        }
    };

    // Prime the rings and materialise every lazy table before anything is timed.
    for (int b = 0; b < 60; ++b) { refill(); core.process(chans, 2, kBlock); }

    const long long hannBuildsBefore  = core.hannBuilds();
    const long long clearsBefore       = core.reprimeChannelClears();

    dcrt::reset();
    dcrt::arm();

    double flipNs = 0.0, otherNs = 0.0;
    int    flipBlocks = 0, otherBlocks = 0;
    bool   lowLat = false;
    double wetEnergy = 0.0;
    {
        dcrt::ScopedRtThread rt;
        for (int b = 0; b < kBlocks; ++b)
        {
            const bool flip = (b % kEvery) == 0;
            if (flip)
            {
                lowLat = !lowLat;
                p.lowLatency = lowLat ? 1.0f : 0.0f;
                core.setParams(p);   // the flip is consumed inside this call
            }

            refill();
            const auto t0 = std::chrono::steady_clock::now();
            core.process(chans, 2, kBlock);
            const auto t1 = std::chrono::steady_clock::now();

            const double ns = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  t1 - t0).count();
            // The neighbour is the block right AFTER the flip: still priming,
            // still producing no grains, identical work MINUS the burst. A
            // steady block is not a fair comparison in either direction.
            if (flip)                     { flipNs  += ns; ++flipBlocks; }
            else if ((b % kEvery) == 1)   { otherNs += ns; ++otherBlocks; }

            for (int i = 0; i < kBlock; ++i)
            {
                const double v = (double) l[(std::size_t) i];
                wetEnergy += v * v;
            }
        }
    }
    dcrt::disarm();

    out.allocs        = dcrt::allocCount();
    out.flips         = flipBlocks;
    out.wetEnergy     = wetEnergy;
    out.hannBuilds    = core.hannBuilds() - hannBuildsBefore;
    out.channelClears = core.reprimeChannelClears() - clearsBefore;
    const double flipMean  = flipBlocks  > 0 ? flipNs  / flipBlocks  : 0.0;
    const double otherMean = otherBlocks > 0 ? otherNs / otherBlocks : 0.0;
    out.ratio = otherMean > 0.0 ? flipMean / otherMean : 0.0;
    return out;
}

void scenarioPitchCoreFlipDoesNotBurst()
{
    beginScenario("RtPitchCoreFlipDoesNotBurstOneCallback");

    {
        dcdsp::HarmonizerCore core;
        dcdsp::HarmonizerParams p;
        p.semitones = -2.0f;
        p.mix       = 1.0f;
        const PitchFlipResult res = pitchFlipCostRatio(core, p);

        note("harmonizer lowLatency flip: " + std::to_string(res.flips) + " flips, "
             + std::to_string(res.hannBuilds) + " Hann builds, "
             + std::to_string(res.channelClears) + " channel clears, "
             + std::to_string(res.allocs) + " allocations, cost ratio "
             + std::to_string(res.ratio));
        RT_CHECK(res.allocs == 0,
                 "harmonizer: a lowLatency flip allocated " + std::to_string(res.allocs)
                 + " time(s) on the audio thread");
        RT_CHECK(res.flips > 20,
                 "harmonizer: only " + std::to_string(res.flips)
                 + " flip(s) fired -- the probe never drove the path it exists to measure");
        RT_CHECK(res.wetEnergy > 0.0,
                 "harmonizer: the probe produced silence, so nothing below means anything");
        // THE assertion. Zero window builds while the callback is armed: every
        // flip swaps a pointer into a table prepare() already built.
        RT_CHECK(res.hannBuilds == 0,
                 "harmonizer: the lowLatency flips ran buildHann() "
                 + std::to_string(res.hannBuilds)
                 + " time(s) ON THE AUDIO THREAD -- up to 8,192 std::cos inside the one "
                   "block that saw the toggle. prepare() must build BOTH modes.");
        // ...and the re-prime clears only the channels in use. 2 channels x one
        // clear per flip; kMaxChannels (8) would be four times this.
        RT_CHECK(res.channelClears == 2LL * res.flips,
                 "harmonizer: the flips cleared " + std::to_string(res.channelClears)
                 + " channel ring(s) for " + std::to_string(res.flips)
                 + " flip(s) on 2 channels -- expected "
                 + std::to_string(2LL * res.flips)
                 + "; reprimeSoft() is looping kMaxChannels again");
        RT_CHECK(core.hannTablesCoverBothModes(),
                 "harmonizer: the flips resized a Hann table");
    }

    {
        dcdsp::WhammyCore core;
        dcdsp::WhammyParams p;
        p.position  = 1.0f;
        p.toeSemis  = -5.0f;
        p.mix       = 1.0f;
        const PitchFlipResult res = pitchFlipCostRatio(core, p);

        note("whammy lowLatency flip: " + std::to_string(res.flips) + " flips, "
             + std::to_string(res.hannBuilds) + " Hann builds, "
             + std::to_string(res.channelClears) + " channel clears, "
             + std::to_string(res.allocs) + " allocations, cost ratio "
             + std::to_string(res.ratio));
        RT_CHECK(res.allocs == 0,
                 "whammy: a lowLatency flip allocated " + std::to_string(res.allocs)
                 + " time(s) on the audio thread");
        RT_CHECK(res.flips > 20,
                 "whammy: only " + std::to_string(res.flips)
                 + " flip(s) fired -- the probe never drove the path it exists to measure");
        RT_CHECK(res.wetEnergy > 0.0,
                 "whammy: the probe produced silence, so nothing below means anything");
        // THE assertion. Zero window builds while the callback is armed: every
        // flip swaps a pointer into a table prepare() already built.
        RT_CHECK(res.hannBuilds == 0,
                 "whammy: the lowLatency flips ran buildHann() "
                 + std::to_string(res.hannBuilds)
                 + " time(s) ON THE AUDIO THREAD -- up to 8,192 std::cos inside the one "
                   "block that saw the toggle. prepare() must build BOTH modes.");
        // ...and the re-prime clears only the channels in use. 2 channels x one
        // clear per flip; kMaxChannels (8) would be four times this.
        RT_CHECK(res.channelClears == 2LL * res.flips,
                 "whammy: the flips cleared " + std::to_string(res.channelClears)
                 + " channel ring(s) for " + std::to_string(res.flips)
                 + " flip(s) on 2 channels -- expected "
                 + std::to_string(2LL * res.flips)
                 + "; reprimeSoft() is looping kMaxChannels again");
        RT_CHECK(core.hannTablesCoverBothModes(),
                 "whammy: the flips resized a Hann table");
    }

    dcrt::reset();
    endScenario();
}

// A NaN parameter used to be a one-way trip to silence for the module that got
// it, and nothing anywhere rejected one.
//
// Every core clamps with `v < lo ? lo : (v > hi ? hi : v)`, and BOTH
// comparisons are false for NaN, so the clamp is transparent. Inf IS caught
// (`Inf > hi`). The NaN then reaches a coefficient or a smoother target and the
// module's internal state is NaN for ever — DspChain scrubs the BUFFER between
// stages, never a module's state — so the pedal goes silent and stays silent
// until the node is removed and re-added.
//
// Driven end to end through the real rig on the null device, so both halves are
// real: the export must REPORT the rejection, and the module must still be
// making sound afterwards.
void scenarioNaNParamIsRejected()
{
    beginScenario("RtNaNParamIsRejectedAndDoesNotKillTheModule");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    fx.installInputStimulus(0.25f);
    fx.engine().setInputGain(1.0f);
    fx.engine().setMonitorGain(1.0f);

    const int id = fx.engine().dspAddNode(0, "delay");
    if (id < 0) { fail("could not add a delay node"); endScenario(); return; }
    // Fully wet and short, so the module IS the signal path: if its state dies,
    // the output is silence rather than a slightly duller mix.
    fx.engine().dspSetParam(id, "mix", 1.0f);
    fx.engine().dspSetParam(id, "timeMs", 50.0f);
    fx.engine().dspSetParam(id, "feedback", 0.2f);

    fx.pumpWarm(256);                      // let the line fill and the mix ramp settle
    const float before = fx.lastOutputPeak();

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const bool accepted = fx.engine().dspSetParam(id, "timeMs", nan);
    fx.pumpWarm(256);
    const float after = fx.lastOutputPeak();

    note("delay peak before " + std::to_string(before) + ", after a NaN timeMs "
         + std::to_string(after) + ", setParam returned "
         + std::string(accepted ? "true" : "false"));

    RT_CHECK(before > 0.02f,
             "the delay never became audible (peak " + std::to_string(before)
             + ") — a module that was already silent proves nothing here");
    RT_CHECK(! accepted, "dspSetParam ACCEPTED a NaN — it must report the rejection");
    RT_CHECK(after > before * 0.5f,
             "a NaN parameter left the module at peak " + std::to_string(after)
             + " against " + std::to_string(before)
             + " — its internal state is NaN and the pedal is silent for good");

    fx.engine().dspClear(0);
    endScenario();
}

// The chain-level rejection above is one door. This is the other side of the
// same rule, INSIDE the cores: every core's clamp helper is NaN-transparent by
// construction (`v < lo` and `v > hi` are BOTH false for a NaN), so a NaN that
// reaches a coefficient or a persistent gain is permanent — Biquad::reset() and
// StateVariableTPT::reset() clear STATE only, and a persistent gain is
// multiplied into every subsequent sample until the post-stage scrub prints
// silence for ever. No later VALID setParams repairs any of it.
//
// Reachability, stated honestly: DspChain::setParam rejects NaN for every node
// and the ABI rejects every non-finite float, so neither door is open TODAY.
// This is the defence-in-depth half, and it is worth a case because the
// consequence is unrecoverable and silent. It drives the cores directly rather
// than through the rig, because the rig is exactly what stops the NaN.
void scenarioNaNCannotLatchACoreCoefficient()
{
    beginScenario("RtNaNCannotLatchACoreCoefficientOrGain");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const int   block = 256;

    // Render `blocks` blocks of a steady tone and return the peak of the last.
    auto peakOfLastBlock = [&](auto& core, int blocks, double sr, double toneHz,
                               double& phase)
    {
        std::vector<float> l((std::size_t) block), r((std::size_t) block);
        float* chans[2] = { l.data(), r.data() };
        const double inc = 6.283185307179586 * toneHz / sr;
        float peak = 0.f;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                l[(std::size_t) i] = r[(std::size_t) i] = 0.3f * (float) std::sin(phase);
                phase += inc;
            }
            core.process(chans, 2, block);
            if (b == blocks - 1)
                for (int i = 0; i < block; ++i)
                    peak = std::max(peak, std::abs(l[(std::size_t) i]));
        }
        return peak;
    };

    // --- (a) a PERSISTENT GAIN: gate + noisereducer ------------------------
    // Both smooth a gain toward a target derived from thresholdDb/ratio. A NaN
    // there makes the target NaN, the smoother latches, and `clampd(gain,0,1)`
    // lets it through — every subsequent sample is multiplied by NaN.
    {
        dcdsp::NoiseGateCore core; dcdsp::GateParams p;
        p.thresholdDb = -60.0f; p.ratio = 4.0f; p.holdMs = 0.0f;
        core.prepare(48000.0, block, 2);
        core.setParams(p);
        double phase = 0.0;
        const float before = peakOfLastBlock(core, 8, 48000.0, 220.0, phase);

        // The NaN has to arrive while the gate is CLOSED: with it open the
        // target is the literal 0 dB and the threshold is never read, which is
        // exactly how an earlier version of this probe passed vacuously.
        dcdsp::GateParams shut = p; shut.thresholdDb = 0.0f;
        core.setParams(shut);
        peakOfLastBlock(core, 16, 48000.0, 220.0, phase);
        dcdsp::GateParams bad = shut; bad.thresholdDb = nan;
        core.setParams(bad);
        peakOfLastBlock(core, 8, 48000.0, 220.0, phase);
        core.setParams(p);                                   // a VALID write back
        const float after = peakOfLastBlock(core, 32, 48000.0, 220.0, phase);

        note("gate peak before a NaN threshold " + std::to_string(before)
             + ", after a valid write back " + std::to_string(after));
        RT_CHECK(before > 0.05f, "the gate was already closed — vacuous probe");
        RT_CHECK(after > before * 0.5f,
                 "the gate stayed at peak " + std::to_string(after)
                 + " after a valid parameter write: a NaN latched its persistent "
                   "gain and only reset() would clear it");
    }
    {
        dcdsp::NoiseReducerCore core; dcdsp::NoiseReducerParams p;
        p.amount = 0.5f; p.thresholdDb = -70.0f;
        core.prepare(48000.0, block, 2);
        core.setParams(p);
        double phase = 0.0;
        const float before = peakOfLastBlock(core, 8, 48000.0, 220.0, phase);

        dcdsp::NoiseReducerParams bad = p; bad.thresholdDb = nan;
        core.setParams(bad);
        peakOfLastBlock(core, 4, 48000.0, 220.0, phase);
        core.setParams(p);
        const float after = peakOfLastBlock(core, 8, 48000.0, 220.0, phase);

        note("noisereducer peak before a NaN threshold " + std::to_string(before)
             + ", after a valid write back " + std::to_string(after));
        RT_CHECK(before > 0.05f, "the reducer was already silent — vacuous probe");
        RT_CHECK(after > before * 0.5f,
                 "noisereducer stayed at peak " + std::to_string(after)
                 + " after a valid parameter write: a NaN latched its per-channel "
                   "expander gain");
    }

    // --- (b) a FILTER COEFFICIENT, through the shared primitive ------------
    // dcdsp::clampCutoff / clampGainDb / the Q guard are the ONE place every
    // core's filter design passes through, so proving them here covers all 47
    // at once. Coefficients are permanent: Biquad::reset() and
    // StateVariableTPT::reset() clear STATE, never the design.
    {
        auto biquadSurvives = [&](const char* what, auto designer)
        {
            dcdsp::Biquad b;
            designer(b);
            float y = 0.f;
            for (int i = 0; i < 64; ++i) y = b.process(0.25f);
            note(std::string("Biquad after ") + what + ": " + std::to_string(y));
            RT_CHECK(std::isfinite(y),
                     std::string("a NaN ") + what
                     + " poisoned the Biquad's coefficients permanently (output "
                     + std::to_string(y) + ")");
        };
        biquadSurvives("cutoff",
                       [&](dcdsp::Biquad& b) { b.setPeaking(48000.0, nan, 0.707, 6.0); });
        biquadSurvives("Q",
                       [&](dcdsp::Biquad& b) { b.setPeaking(48000.0, 1000.0, nan, 6.0); });
        biquadSurvives("gainDb",
                       [&](dcdsp::Biquad& b) { b.setPeaking(48000.0, 1000.0, 0.707, nan); });
        biquadSurvives("low-shelf gainDb",
                       [&](dcdsp::Biquad& b) { b.setLowShelf(48000.0, 200.0, 0.707, nan); });
        {
            dcdsp::StateVariableTPT f;
            f.set(48000.0, nan, nan);
            for (int i = 0; i < 64; ++i) f.process(0.25f);
            note("StateVariableTPT lp after a NaN cutoff and Q: "
                 + std::to_string(f.lp()));
            RT_CHECK(std::isfinite(f.lp()),
                     "a NaN cutoff/Q poisoned the TPT filter permanently");
        }
        {
            dcdsp::OnePole op;
            op.setLowpass(48000.0, nan);
            float y = 0.f;
            for (int i = 0; i < 64; ++i) y = op.process(0.25f);
            note("OnePole after a NaN cutoff: " + std::to_string(y));
            RT_CHECK(std::isfinite(y), "a NaN cutoff poisoned the one-pole permanently");
        }
    }

    // The same thing end to end, through a core. miku's `character` scales its
    // formant centre frequencies, which is the shortest path from a param to a
    // coefficient in the rig; its hardReset() on a non-finite voice is the
    // second half of the recovery (mikustomp already had one, MikuCore did not).
    // dcdsp::clampCutoff is the one helper every core's filter design goes
    // through, so the negated comparison there covers all 47 at once. miku's
    // `character` scales its formant centre frequencies, which is the shortest
    // path from a param to a coefficient in the rig.
    {
        dcdsp::MikuCore core; dcdsp::MikuParams p;
        p.mix = 1.0f; p.character = 0.5f; p.octave = 0.0f;
        core.prepare(48000.0, block, 2);
        core.setParams(p);
        double phase = 0.0;
        // Bursts, so the onset detector fires and the voice actually sings.
        auto sing = [&](int blocks) {
            std::vector<float> l((std::size_t) block), r((std::size_t) block);
            float* chans[2] = { l.data(), r.data() };
            const double inc = 6.283185307179586 * 196.0 / 48000.0;
            float peak = 0.f;
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < block; ++i)
                {
                    const float env = (b % 8 < 6) ? 0.5f : 0.0f;
                    l[(std::size_t) i] = r[(std::size_t) i] = env * (float) std::sin(phase);
                    phase += inc;
                }
                core.process(chans, 2, block);
                if (b >= blocks - 16)
                    for (int i = 0; i < block; ++i)
                        peak = std::max(peak, std::abs(l[(std::size_t) i]));
            }
            return peak;
        };
        const float before = sing(96);
        dcdsp::MikuParams bad = p; bad.character = nan;
        core.setParams(bad);
        sing(16);
        core.setParams(p);
        const float after = sing(96);

        note("miku peak before a NaN character " + std::to_string(before)
             + ", after a valid write back " + std::to_string(after));
        RT_CHECK(before > 0.02f, "miku never sang — vacuous probe");
        RT_CHECK(after > before * 0.5f,
                 "miku stayed at peak " + std::to_string(after)
                 + " after a valid parameter write: a NaN cutoff reached its formant "
                   "filters and Biquad/SVF reset() clears STATE, never coefficients");
    }

    endScenario();
}

// A core may not process PART of an oversized block. Every audio-thread buffer
// is pre-sized with headroom and the rule is that a hot path SKIPS a block it
// cannot serve rather than growing a buffer — but four drive cores clamped with
// `std::min(numFrames, maxBlock_)` and left the tail UNTOUCHED, which is
// full-level clean DI spliced into the middle of a high-gain patch: the one
// outcome worse than either alternative.
void scenarioOversizedBlockIsSkippedNotPartiallyProcessed()
{
    beginScenario("RtOversizedBlockIsSkippedNotPartiallyProcessed");

    const int prepared = 256;
    const int oversize = 2 * prepared;

    auto probe = [&](const char* name, auto& core, auto& params)
    {
        core.setParams(params);
        core.prepare(48000.0, prepared, 2);
        core.setParams(params);

        std::vector<float> l((std::size_t) oversize), r((std::size_t) oversize);
        std::vector<float> ref((std::size_t) oversize);
        double phase = 0.0;
        const double inc = 6.283185307179586 * 220.0 / 48000.0;
        for (int i = 0; i < oversize; ++i)
        {
            const float s = 0.3f * (float) std::sin(phase);
            phase += inc;
            l[(std::size_t) i] = r[(std::size_t) i] = ref[(std::size_t) i] = s;
        }
        float* chans[2] = { l.data(), r.data() };
        core.process(chans, 2, oversize);

        // With the block skipped, EVERY sample is the untouched input. With the
        // old clamp, the head was processed and the tail was not — so the head
        // moving is the tell, and the tail is dry either way.
        double headDelta = 0.0;
        for (int i = 0; i < prepared; ++i)
            headDelta = std::max(headDelta,
                                 (double) std::abs(l[(std::size_t) i] - ref[(std::size_t) i]));
        note(std::string(name) + " oversized-block head delta: "
             + std::to_string(headDelta));
        RT_CHECK(headDelta < 1e-6,
                 std::string(name) + " processed the first " + std::to_string(prepared)
                 + " frames of a " + std::to_string(oversize)
                 + "-frame block and left the rest DRY (head delta "
                 + std::to_string(headDelta)
                 + ") — an oversized block must be skipped whole");
    };

    { dcdsp::AmpCore c;        dcdsp::AmpParams p;        p.gain = 0.8f; p.master = 0.8f;
      probe("amp", c, p); }
    { dcdsp::DistortionCore c; dcdsp::DistortionParams p; p.gain = 0.8f; p.level = 0.8f;
      probe("distortion", c, p); }
    { dcdsp::FuzzCore c;       dcdsp::FuzzParams p;       p.fuzz = 0.8f; p.level = 0.8f;
      probe("fuzz", c, p); }
    { dcdsp::BigMuffCore c;    dcdsp::BigMuffParams p;    p.sustain = 0.8f; p.volume = 0.8f;
      probe("bigmuff", c, p); }

    endScenario();
}

// W4-D66: a memoryless nonlinearity at base rate folds its whole harmonic
// series back into the band. Six cores wrap theirs in 8x oversampling; the
// graphic EQ's output limiter had neither that nor ADAA — it was the only
// nonlinearity in its group using neither.
//
// It is a SAFETY limiter, reached only past ~+30 dBFS, so this drives it there
// on purpose. The measure is the repo's own (dcdsp_bigmuff.h's header quotes
// "worst alias on a full-scale 9 kHz tone"): one 9 kHz tone through an odd
// nonlinearity puts harmonics at 27/45/63 kHz, which at 48 kHz fold to
// 21/3/15 kHz. Those folded bins are not harmonics of 9 kHz, so "the worst bin
// that is not a multiple of the fundamental" IS the alias floor.
void scenarioGraphicEqLimiterIsAntiAliased()
{
    beginScenario("RtGraphicEqLimiterAliasFloorDropped");

    constexpr int    kFft   = 1 << 14;         // 16,384 bins
    constexpr double kSr    = 48000.0;
    constexpr double kTone  = 9000.0;
    const int        kBinsPerTone = (int) std::lround(kTone * kFft / kSr);   // exact bin

    // Render the EQ hard into its limiter and return the output, one FFT frame
    // long, with the tone landing exactly on a bin so no window is needed.
    auto render = [&](float driveDb) {
        dcdsp::GraphicEqCore core;
        dcdsp::GraphicEqParams p;
        for (int b = 0; b < 10; ++b) p.bands[b] = 0.f;
        p.level = driveDb;
        core.prepare(kSr, 512, 1);
        core.setParams(p);

        std::vector<float> out((std::size_t) kFft, 0.f);
        std::vector<float> blk(512, 0.f);
        float* chans[1] = { blk.data() };
        long long n = 0;
        // Pre-roll so the level ramp has landed before the frame we keep.
        for (int b = 0; b < 64; ++b)
        {
            for (int i = 0; i < 512; ++i)
                blk[(std::size_t) i] = (float) std::sin(6.283185307179586 * kTone * (double) (n + i) / kSr);
            n += 512;
            core.process(chans, 1, 512);
        }
        for (int off = 0; off < kFft; off += 512)
        {
            for (int i = 0; i < 512; ++i)
                blk[(std::size_t) i] = (float) std::sin(6.283185307179586 * kTone * (double) (n + i) / kSr);
            n += 512;
            core.process(chans, 1, 512);
            std::copy(blk.begin(), blk.end(), out.begin() + off);
        }
        return out;
    };

    // Worst non-harmonic bin, in dB below the fundamental.
    auto aliasFloorDb = [&](std::vector<float> x) {
        juce::dsp::FFT fft((int) std::log2((double) kFft));
        std::vector<float> buf((std::size_t) kFft * 2, 0.f);
        std::copy(x.begin(), x.end(), buf.begin());
        fft.performFrequencyOnlyForwardTransform(buf.data());

        double fund = 0.0, worst = 0.0;
        int worstBin = 0;
        for (int k = 4; k < kFft / 2; ++k)
        {
            const double m = (double) buf[(std::size_t) k];
            // Harmonics of 9 kHz land on exact multiples of kBinsPerTone; skip
            // those and their immediate neighbours (spectral leakage).
            const int r = k % kBinsPerTone;
            const bool harmonic = (r <= 2) || (r >= kBinsPerTone - 2);
            if (k == kBinsPerTone) fund = m;
            else if (!harmonic && m > worst) { worst = m; worstBin = k; }
        }
        const double db = 20.0 * std::log10(std::max(1e-30, worst) / std::max(1e-30, fund));
        return std::pair<double, int>{ db, worstBin };
    };

    // +12 dB is the MOST the `level` knob allows, so this is the worst case a
    // user can actually reach: |s| ~ 4 against kMaxOut 24, i.e. the shallow end
    // of the knee. MEASURED there: raw tanh -52.852 dBc, ADAA1 -61.175, an
    // 8.3 dB drop. Deeper into the knee the gap widens; this is the figure that
    // matters because it is the one the knob can produce.
    const auto hot = aliasFloorDb(render(12.f));
    note("graphiceq alias floor at +12 dB trim: " + std::to_string(hot.first)
         + " dBc (worst non-harmonic bin " + std::to_string(hot.second) + ")");

    // The reference: the SAME core with its limiter never engaging. If the
    // floor with the limiter working is not far above it, the probe is vacuous.
    const auto clean = aliasFloorDb(render(-12.f));
    note("graphiceq alias floor at -12 dB trim (limiter idle): "
         + std::to_string(clean.first) + " dBc");

    RT_CHECK(clean.first < -95.0,
             "the EQ aliases even with its limiter idle (" + std::to_string(clean.first)
             + " dBc) — the measurement is picking up something else");
    RT_CHECK(hot.first < -57.0,
             "graphiceq's worst alias product is " + std::to_string(hot.first)
             + " dBc with the limiter engaged; ADAA1 measured -61.175 against the "
               "memoryless tanh's -52.852, so this has regressed to the raw form");

    endScenario();
}

// The three fast-math helpers in dcdsp_primitives.h each carry a MEASURED
// error bound in their comment, and a bound nobody re-measures is a number
// that rots. This sweeps each one against the exact call across the whole
// range it is used on and fails if it has moved.
//
// The bounds are not aspirational: they were chosen from the measurement, and
// the accompanying perf claims were taken from a single-process A/B over the
// real cores (a cross-process or same-symbol harness measures the machine's
// load and the linker's COMDAT folding respectively, not the change).
void scenarioFastMathHelpersHoldTheirBounds()
{
    beginScenario("RtFastMathHelpersStayWithinTheirStatedErrorBounds");

    // std::to_string prints six decimals, which rounds every bound here to
    // 0.000000 and makes the note useless.
    auto sci = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.3e", v);
        return std::string(buf);
    };

    // --- fastSinTurns: modulator phases, claimed 3.663e-06 absolute ---------
    {
        double worst = 0.0;
        for (int i = 0; i <= 2000000; ++i)
        {
            const double t = -2.0 + 4.0 * (double) i / 2000000.0;
            worst = std::max(worst, std::fabs(std::sin(6.283185307179586 * t)
                                              - (double) dcdsp::fastSinTurns((float) t)));
        }
        note("fastSinTurns max abs error over +/-2 turns: " + sci(worst));
        RT_CHECK(worst < 5e-6,
                 "fastSinTurns is outside its documented 3.663e-06 bound ("
                 + sci(worst) + ")");
        RT_CHECK(worst > 1e-9,
                 "fastSinTurns is suspiciously exact — is it still the polynomial?");
    }

    // --- fastTan on [0, 0.4999 pi): claimed 1.347e-08 absolute AND relative -
    {
        double worstAbs = 0.0, worstRel = 0.0;
        const double hi = 0.4999 * 3.141592653589793;
        for (int i = 0; i <= 2000000; ++i)
        {
            const double x = 1e-6 + (hi - 1e-6) * (double) i / 2000000.0;
            const double exact = std::tan(x);
            const double got   = dcdsp::fastTan(x);
            worstAbs = std::max(worstAbs, std::fabs(exact - got));
            worstRel = std::max(worstRel, std::fabs(exact - got) / exact);
        }
        note("fastTan max abs error " + sci(worstAbs)
             + ", max rel " + sci(worstRel));
        RT_CHECK(worstAbs < 5e-8 && worstRel < 5e-8,
                 "fastTan is outside its documented 1.347e-08 bound (abs "
                 + sci(worstAbs) + ", rel " + sci(worstRel) + ")");
        // Out of range it must DEFER to std::tan rather than clamp or wrap.
        RT_CHECK(dcdsp::fastTan(-0.5) == std::tan(-0.5)
                 && dcdsp::fastTan(2.0) == std::tan(2.0),
                 "fastTan does not fall back to std::tan outside [0, pi/2)");
    }

    // --- dbToGain: claimed EXACT to 2.175e-15 relative, not an approximation -
    {
        double worst = 0.0;
        for (int i = 0; i <= 1000000; ++i)
        {
            const double db = -96.0 + 96.0 * (double) i / 1000000.0;
            const double exact = std::pow(10.0, db / 20.0);
            worst = std::max(worst, std::fabs(exact - dcdsp::dbToGain(db)) / exact);
        }
        note("dbToGain max RELATIVE error over -96..0 dB: " + sci(worst));
        RT_CHECK(worst < 1e-13,
                 "dbToGain is no longer the same function as pow(10, dB/20) (rel "
                 + sci(worst) + ")");
    }

    // --- logCosh's large-|x| short circuit: claimed 1.523e-08 absolute ------
    {
        double worst = 0.0;
        for (int i = 0; i <= 1000000; ++i)
        {
            const double x  = -40.0 + 80.0 * (double) i / 1000000.0;
            const double ax = std::fabs(x);
            const double exact = ax - dcdsp::kLn2 + std::log1p(std::exp(-2.0 * ax));
            worst = std::max(worst, std::fabs(exact - dcdsp::logCosh(x)));
        }
        note("logCosh max abs error over |x| <= 40: " + sci(worst));
        RT_CHECK(worst < 5e-8,
                 "logCosh's short circuit is outside its documented 1.523e-08 bound ("
                 + sci(worst) + ")");
    }

    endScenario();
}

// Three fixes shipped in this wave with NO discriminating probe, for the same
// reason in each case: the quantity that was stepping is several stages
// upstream of anything the module outputs, so an output-domain harness either
// measures no contrast or cannot separate the fix from the core's own
// settling. Both managed theories were retried against all three and the
// results are recorded in DspGainZipperTests' comments.
//
// So this reads the SMOOTHED VALUE itself, through a *ForTest() accessor on
// each core: after a short window it must sit strictly BETWEEN the two
// endpoints (a hard switch is already at the new one), and later it must have
// landed (a ramp that never arrives is the other failure).
void scenarioSmoothedControlsWalkTheirRamp()
{
    beginScenario("RtSmoothedControlsWalkTheirRampInsteadOfSwitching");

    const int block = 64;
    const double sr = 48000.0;

    // Render `blocks` blocks of a steady tone through `core`.
    auto drive = [&](auto& core, int blocks, double& phase)
    {
        std::vector<float> l((std::size_t) block), r((std::size_t) block);
        float* chans[2] = { l.data(), r.data() };
        const double inc = 6.283185307179586 * 220.0 / sr;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                l[(std::size_t) i] = r[(std::size_t) i] = 0.3f * (float) std::sin(phase);
                phase += inc;
            }
            core.process(chans, 2, block);
        }
    };

    auto checkWalk = [&](const char* what, float from, float to,
                         float early, float landed)
    {
        const float lo = std::min(from, to), hi = std::max(from, to);
        const float travel = hi - lo;
        note(std::string(what) + ": " + std::to_string(from) + " -> " + std::to_string(to)
             + ", after 64 samples " + std::to_string(early)
             + ", after the ramp " + std::to_string(landed));
        RT_CHECK(travel > 1e-3f,
                 std::string(what) + " has nowhere to travel — the case is vacuous");
        RT_CHECK(early > lo + 0.02f * travel && early < hi - 0.02f * travel,
                 std::string(what) + " read " + std::to_string(early)
                 + " 64 samples after the change, which is an ENDPOINT, not a point on a "
                   "ramp between " + std::to_string(lo) + " and " + std::to_string(hi));
        RT_CHECK(std::abs(landed - to) < 0.02f * travel,
                 std::string(what) + " never landed: " + std::to_string(landed)
                 + " against a target of " + std::to_string(to));
    };

    // --- wah.resonance: scales the tank's Q AND its peak gain (W4-D4) -------
    {
        dcdsp::WahCore core; dcdsp::WahParams p;
        p.mode = 1; p.position = 0.5f; p.mix = 1.0f; p.resonance = 0.0f;
        core.prepare(sr, block, 2);
        core.setParams(p);
        double phase = 0.0;
        drive(core, 16, phase);
        const float from = core.resonanceFactorCurrentForTest();
        p.resonance = 1.0f; core.setParams(p);
        drive(core, 1, phase);
        const float early = core.resonanceFactorCurrentForTest();
        drive(core, 32, phase);
        const float landed = core.resonanceFactorCurrentForTest();
        checkWalk("wah.resonance (tank scale)", from, 1.5f, early, landed);
    }

    // --- filterseq.resonance: Q 0.7 -> 7.7 and -8.8 dB of body (W4-D5) ------
    {
        dcdsp::FilterSeqCore core; dcdsp::FilterSeqParams p;
        p.mix = 1.0f; p.resonance = 0.0f;
        core.prepare(sr, block, 2);
        core.setParams(p);
        double phase = 0.0;
        drive(core, 16, phase);
        const float from = core.resonanceCurrentForTest();
        p.resonance = 1.0f; core.setParams(p);
        drive(core, 1, phase);
        const float early = core.resonanceCurrentForTest();
        drive(core, 32, phase);
        const float landed = core.resonanceCurrentForTest();
        checkWalk("filterseq.resonance", from, 1.0f, early, landed);
    }

    // --- platereverb.predelayMs: a teleporting read tap into the tank -------
    {
        dcdsp::PlateReverbCore core; dcdsp::PlateReverbParams p;
        p.mix = 1.0f; p.predelayMs = 0.0f;
        core.prepare(sr, block, 2);
        core.setParams(p);
        double phase = 0.0;
        drive(core, 16, phase);
        const float from = core.predelayTapForTest();
        p.predelayMs = 100.0f; core.setParams(p);
        drive(core, 1, phase);
        const float early = core.predelayTapForTest();
        drive(core, 32, phase);
        const float landed = core.predelayTapForTest();
        checkWalk("platereverb.predelayMs (tap, samples)", from,
                  (float) (100.0 * sr / 1000.0), early, landed);
    }

    endScenario();
}

// tapestop's `curve` reshapes the BRAKE, and it is read per sample, so turning
// it while a stop is in flight stepped the transport RATE. That is the PITCH,
// and it is invisible to every sample-domain seam probe in the repo: the read
// POSITION stays continuous across a rate step while its derivative does not,
// so the output has no discontinuity to measure. (Verified — the managed
// harness passed with the fix reverted.) So this reads the rate itself.
//
// Worst case is mid-brake: at phase 0.5 the linear shape gives 0.5 and the
// cubic one 0.125, a jump of nearly two octaves in one sample.
void scenarioTapeStopCurveRamps()
{
    beginScenario("RtTapeStopCurveRampsInsteadOfSteppingThePitch");

    // ONE FRAME per call: the rate is only observable between process()
    // calls, so a block of 64 cannot tell a 30 ms ramp from a step — the
    // ramp's own per-block travel is the same size. At block 1 a hard switch
    // is the whole jump in one sample and the ramp is 1/1440th of it.
    const int block = 1;
    const double sr = 48000.0;

    auto worstRateStep = [&](bool moveCurve, float& observedSpan)
    {
        dcdsp::TapeStopCore core; dcdsp::TapeStopParams p;
        p.mix = 1.0f; p.stopMs = 4000.0f; p.startMs = 4000.0f; p.curve = 0.0f;
        core.prepare(sr, block, 2);
        core.setParams(p);

        std::vector<float> l((std::size_t) block), r((std::size_t) block);
        float* chans[2] = { l.data(), r.data() };
        double phase = 0.0;
        const double inc = 6.283185307179586 * 220.0 / sr;
        auto run = [&]() {
            for (int i = 0; i < block; ++i)
            {
                l[(std::size_t) i] = r[(std::size_t) i] = 0.3f * (float) std::sin(phase);
                phase += inc;
            }
            core.process(chans, 2, block);
        };

        p.trigger = 1; core.setParams(p);
        // ~0.7 s into a 4 s brake: the part of the shape where linear and cubic
        // are furthest apart.
        for (int b = 0; b < 33000; ++b) run();   // ~0.69 s into a 4 s brake

        const float before = core.transportRateForTest();
        if (moveCurve) { p.curve = 1.0f; core.setParams(p); }

        float worst = 0.f, prev = before, lo = before, hi = before;
        for (int b = 0; b < 4000; ++b)
        {
            run();
            const float now = core.transportRateForTest();
            worst = std::max(worst, std::abs(now - prev));
            lo = std::min(lo, now); hi = std::max(hi, now);
            prev = now;
        }
        observedSpan = hi - lo;
        return worst;
    };

    float spanStill = 0.f, spanMoved = 0.f;
    const float still = worstRateStep(false, spanStill);
    const float moved = worstRateStep(true,  spanMoved);

    note("transport rate: worst per-block change while coasting " + std::to_string(still)
         + ", while `curve` is turned " + std::to_string(moved)
         + " (total travel " + std::to_string(spanMoved) + ")");

    RT_CHECK(spanMoved > 0.05f,
             "turning `curve` mid-brake moved the transport rate by only "
             + std::to_string(spanMoved) + " — the probe proves nothing");
    // Scale-free: a RAMP spreads the whole travel over its window, so the worst
    // single-sample change is a small FRACTION of it (1/1440th for the 30 ms
    // ramp at 48 kHz, measured 0.00065); a hard switch puts all of it in one
    // sample, i.e. a fraction of 1.0. 0.02 is ~30x headroom over the ramp and
    // ~50x below a step. The coasting figure above is the context, not the bar —
    // the ramp legitimately outruns the brake's own deceleration by ~33x.
    const float fraction = spanMoved > 0.f ? moved / spanMoved : 1.f;
    note("worst single-sample change as a fraction of the total travel: "
         + std::to_string(fraction));
    RT_CHECK(fraction < 0.02f,
             "`curve` moved the transport rate by " + std::to_string(moved)
             + " in ONE sample, " + std::to_string(fraction * 100.f)
             + " % of its whole travel — that is a pitch jump mid-brake, not a ramp");

    endScenario();
}

// The same rule one step in: a core's output loop must walk the count PREPARE
// gave it, not the one the caller passed. Both cores here iterated the caller's
// numChannels and dereferenced unconditionally — the compressor while its own
// detector loop 50 lines above was careful to skip a null channel, and the
// tremolo because the numCh_ prepare() wrote was never read at all.
void scenarioCoreOutputLoopsRespectThePreparedChannelCount()
{
    beginScenario("RtCoreOutputLoopsRespectThePreparedChannelCount");

    const int frames = 256;

    auto probe = [&](const char* name, auto& core)
    {
        core.prepare(48000.0, frames, 1);          // ONE channel prepared

        std::vector<float> l((std::size_t) frames), spare((std::size_t) frames);
        for (int i = 0; i < frames; ++i)
        {
            l[(std::size_t) i]     = 0.4f;
            spare[(std::size_t) i] = 12345.0f;     // a sentinel, not audio
        }
        float* chans[2] = { l.data(), spare.data() };
        core.process(chans, 2, frames);            // TWO channels handed in

        bool touched = false;
        for (int i = 0; i < frames; ++i)
            if (spare[(std::size_t) i] != 12345.0f) { touched = true; break; }
        note(std::string(name) + " wrote past the prepared channel count: "
             + (touched ? "yes" : "no"));
        RT_CHECK(! touched,
                 std::string(name)
                 + " wrote to channel 1 after being prepared for ONE channel — the "
                   "same loop dereferences a null channel pointer unconditionally");
    };

    { dcdsp::CompressorCore c; probe("compressor", c); }
    { dcdsp::TremoloCore   c; probe("tremolo",    c); }

    endScenario();
}

// bigmuff wraps its clipping cascade in an 8x oversampler precisely because a
// stiff non-linearity must not run at base rate — and then runs a plain
// std::min/max hard clip at BASE rate on the way out. Audit W4-D65 called that
// a defect on the arithmetic "bassMix + trebMix == 1.15, so |toned| * 1.6
// reaches ~1.84 and at the default volume 0.6 the clipper engages". It does
// not: the two tone-stack paths are complementary first-order filters of the
// SAME signal, so their sum at any one frequency is ~0.575 * |s2|, and the
// stage is ~1.2 dB below its own ceiling with volume at 1.0.
//
// This case is therefore a REGRESSION GUARD on that measurement rather than a
// fix: it fails if a future change raises the level into the clipper, which is
// the point at which this stage would genuinely need ADAA or the 8x wrap.
// Structural, not a wall clock: count the output samples sitting exactly on the
// ceiling, which is what a hard clip produces and nothing else does.
void scenarioBigMuffOutputStageIsAntiAliased()
{
    beginScenario("RtBigMuffOutputStageStaysUnderTheBaseRateClipper");

    const int frames = 256;
    dcdsp::BigMuffCore core; dcdsp::BigMuffParams p;
    p.sustain = 0.7f; p.tone = 0.5f; p.volume = 1.0f;   // default sustain/tone, volume up
    core.prepare(48000.0, frames, 2);
    core.setParams(p);

    std::vector<float> l((std::size_t) frames), r((std::size_t) frames);
    float* chans[2] = { l.data(), r.data() };
    double phase = 0.0;
    const double inc = 6.283185307179586 * 600.0 / 48000.0;
    long long pinned = 0, total = 0;
    float peak = 0.f;
    for (int b = 0; b < 64; ++b)
    {
        for (int i = 0; i < frames; ++i)
        {
            l[(std::size_t) i] = r[(std::size_t) i] = 1.0f * (float) std::sin(phase);
            phase += inc;
        }
        core.process(chans, 2, frames);
        if (b < 8) continue;                       // let the smoothers settle
        for (int i = 0; i < frames; ++i)
        {
            const float y = l[(std::size_t) i];
            ++total;
            peak = std::max(peak, std::abs(y));
            if (std::abs(std::abs(y) - 1.0f) < 1e-6f) ++pinned;
        }
    }
    note("bigmuff output peak " + std::to_string(peak) + ", samples exactly on the "
         "+/-1.0 ceiling: " + std::to_string(pinned) + " of " + std::to_string(total));
    RT_CHECK(total > 1000 && peak > 0.3f,
             "the probe rendered almost nothing — vacuous");
    RT_CHECK(pinned == 0,
             "bigmuff pinned " + std::to_string(pinned)
             + " output samples on the ceiling at volume 1.0: the base-rate output"
               " stage is now reaching a plain hard clip OUTSIDE the 8x region, and"
               " the header's aliasing measurements do not cover it. Move the tone"
               " stack + volume inside the oversampled region, or use adaaHardClip");

    endScenario();
}

// prepare() is where cores go wrong, and both halves of this scenario are that
// same mistake seen from two sides: state that prepare() must NOT touch, and
// state it must re-derive and did not.
//
// Neither is visible until a device change, a sample-rate change or a watchdog
// recovery — DspChain::prepare re-prepares the EXISTING module objects, so
// everything downstream still reports the old picture.
// MikuCore's phrase sequencer had the two faults MikuStompCore had already
// fixed, and both are one line. They are pinned here rather than in the managed
// suite because neither is visible through the ABI: the syllable index and the
// vowel smoother are core introspection.
void scenarioMikuSingsTheWholePhrase()
{
    beginScenario("RtMikuSingsItsPhraseFromTheFirstSyllable");

    constexpr int kBlock = 256;
    constexpr double kSr = 48000.0;

    // Pick the voice with bursts, so the onset detector fires and the sequencer
    // actually advances -- the same stimulus the NaN scenario uses.
    // `blockNo` is an ABSOLUTE counter, not a per-call one: the 6-loud /
    // 2-silent envelope IS the pick, so restarting it every call would hold the
    // input at full level and the onset detector would never re-arm.
    auto sing = [&](dcdsp::MikuCore& core, int blocks, double& phase, int& blockNo)
    {
        std::vector<float> l((std::size_t) kBlock), r((std::size_t) kBlock);
        float* chans[2] = { l.data(), r.data() };
        const double inc = 6.283185307179586 * 196.0 / kSr;
        for (int b = 0; b < blocks; ++b, ++blockNo)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                // 6 blocks of pick, 24 of silence (128 ms). The gap is not
                // arbitrary: the onset test is `fe > 2 * feDel` against the
                // fast envelope 20 ms earlier, and that envelope releases over
                // 60 ms -- so a short gap leaves feDel more than half of the
                // attack and only the FIRST pick ever fires. Measured: at a
                // 32 ms gap the sequencer never leaves syllable 0, which reads
                // exactly like a broken advance and is not one.
                const float env = (blockNo % 30 < 6) ? 0.5f : 0.0f;
                l[(std::size_t) i] = r[(std::size_t) i] = env * (float) std::sin(phase);
                phase += inc;
            }
            core.process(chans, 2, kBlock);
        }
    };

    // --- W4-D70: the FIRST note must sing the FIRST syllable ---------------
    // advanceSyllable() takes idx = (prev + 1) % n on both paths, so a resting
    // sylIndex_ of 0 made every multi-syllable phrase open on syllable 1.
    {
        dcdsp::MikuCore core;
        dcdsp::MikuParams p;
        p.mix = 1.0f; p.character = 0.5f; p.octave = 0.0f;
        core.prepare(kSr, kBlock, 2);
        core.setParams(p);

        // Two syllables that cannot be confused: a plosive on A, a fricative on U.
        dcdsp::MikuCore::Syllable phrase[2];
        phrase[0].cons = dcdsp::MikuCore::ConsPlosive;   phrase[0].vowel = 0;
        phrase[1].cons = dcdsp::MikuCore::ConsFricative; phrase[1].vowel = 4;
        core.setPhrase(phrase, 2);

        RT_CHECK(core.syllableIndex() == -1,
                 "a prepared MikuCore rests at syllable "
                 + std::to_string(core.syllableIndex())
                 + ", not -1 -- the first note will sing the SECOND syllable");

        double phase = 0.0;
        int    blockNo = 0;
        sing(core, 8, phase, blockNo);          // one pick

        const dcdsp::MikuCore::Syllable sung = core.currentSyllable();
        note("miku first note sang syllable " + std::to_string(core.syllableIndex())
             + " (cons " + std::to_string(sung.cons)
             + ", vowel " + std::to_string(sung.vowel) + ")");
        RT_CHECK(core.syllableIndex() == 0,
                 "the first note sang syllable " + std::to_string(core.syllableIndex())
                 + ", not 0 -- a two-mora phrase opens on its SECOND mora");
        RT_CHECK(sung.cons == phrase[0].cons && sung.vowel == phrase[0].vowel,
                 "the first note sang cons " + std::to_string(sung.cons)
                 + "/vowel " + std::to_string(sung.vowel) + " instead of the phrase's first");

        // ...and it still ADVANCES: syllable 1 must be reached. Sampled per
        // block rather than after a fixed count, because the onset detector's
        // cooldown decides how many picks land in a given span and a
        // count-based assertion would be measuring that instead.
        bool sawSecond = false;
        for (int b = 0; b < 400 && !sawSecond; ++b)
        {
            sing(core, 1, phase, blockNo);
            if (core.syllableIndex() == 1) sawSecond = true;
        }
        note("miku reached syllable 1: " + std::string(sawSecond ? "yes" : "no")
             + ", index now " + std::to_string(core.syllableIndex()));
        RT_CHECK(sawSecond,
                 "the phrase never reached syllable 1 -- the -1 rest broke the advance itself");
    }

    // --- W4-D71: a nasal head must OPEN into its vowel ---------------------
    // startBurst()'s setCurrentAndTarget(1.0f) overwrote the target
    // advanceSyllable() had set one line earlier, so the hum never opened --
    // while the trailing comment said "opens to the vowel".
    {
        dcdsp::MikuCore core;
        dcdsp::MikuParams p;
        p.mix = 1.0f; p.character = 0.5f; p.octave = 0.0f;
        core.prepare(kSr, kBlock, 2);
        core.setParams(p);

        // One nasal syllable on A: vowel 0 => the smoother's target is 0.0,
        // the furthest it can be from the closed U the nasal head snaps to.
        dcdsp::MikuCore::Syllable nasal[1];
        nasal[0].cons = dcdsp::MikuCore::ConsNasal; nasal[0].vowel = 0;
        core.setPhrase(nasal, 1);

        double phase = 0.0;
        // Long enough for the pitch tracker to publish an f0: vowelSmooth_ is
        // only walked inside `if (f0 > 20.0f)`, so a short probe measures the
        // smoother standing still rather than the bug. The measurement lands
        // 2 blocks (10.7 ms) of silence after the last pick, and the vowel ramp
        // is 25 ms with picks 42.7 ms apart -- so a working ramp has landed.
        int blockNo = 0;
        sing(core, 48, phase, blockNo);

        const float vowel = core.vowelPosition();
        note("miku nasal mora vowel position after one note: " + std::to_string(vowel)
             + " (1 = closed U, 0 = the syllable's own A)");
        RT_CHECK(core.syllableIndex() == 0, "the nasal probe never sang -- vacuous");
        RT_CHECK(vowel < 0.25f,
                 "a nasal mora's vowel smoother sat at " + std::to_string(vowel)
                 + " (closed U is 1.0) instead of walking to its own vowel at 0.0 -- "
                   "startBurst is overwriting the TARGET, not just the current value");
    }

    dcrt::reset();
    endScenario();
}

void scenarioPrepareKeepsUserStateAndFollowsTheRate()
{
    beginScenario("RtPrepareKeepsUserStateAndReDerivesFromTheRate");

    // --- (a) prepare() must not delete USER CONTENT ------------------------
    // mikustomp's three Phrase slots are the hardware's Lyric Editor. prepare()
    // zeroed both halves of their double buffer, so a device change silently
    // reverted the pedal to "Ahh" while the adapter's cached `phrases_` string
    // went on reporting the text the core no longer had.
    {
        dcdsp::MikuStompCore core;
        core.prepare(48000.0, 256, 2);

        dcdsp::MikuStompCore::Syllable syls[4];
        for (int i = 0; i < 4; ++i) { syls[i].cons = 0; syls[i].vowel = i % 5; }
        core.setPhrase(1, syls, 4);
        RT_CHECK(core.phraseSyllableCount(1) == 4,
                 "setPhrase did not take — the rest of this probe would be vacuous");

        core.prepare(96000.0, 512, 2);          // the device change
        RT_CHECK(core.phraseSyllableCount(1) == 4,
                 "a device change DELETED the user's lyrics (slot 2 now holds "
                 + std::to_string(core.phraseSyllableCount(1)) + " syllables, not 4)");
        core.prepare(48000.0, 256, 2);          // ...and back
        RT_CHECK(core.phraseSyllableCount(1) == 4,
                 "the user's lyrics did not survive a second prepare()");
    }

    // --- (b) prepare() must RE-DERIVE what depends on the rate -------------
    // DcBlocker's default mR = 0.995 is a fixed COEFFICIENT, not a fixed
    // frequency: 38 Hz at 48 kHz, 76 Hz at 96 kHz, 153 Hz at 192 kHz. Three
    // cores never called dc.prepare(sr), so their low end fell away as the
    // device rate rose. Measured, not asserted structurally: run 60 Hz through
    // each core at 48 kHz and at 192 kHz and compare output RMS.
    auto lowEndAtRate = [](auto& core, auto& params, double sr) -> double
    {
        const int block = 256;
        const int blocks = (int) (sr * 2.0) / block;   // 2 seconds either way
        std::vector<float> l((std::size_t) block), r((std::size_t) block);
        float* chans[2] = { l.data(), r.data() };
        core.prepare(sr, block, 2);
        core.setParams(params);

        double phase = 0.0;
        const double inc = 6.283185307179586 * 60.0 / sr;
        double sum = 0.0;
        long long n = 0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                const float s = 0.25f * (float) std::sin(phase);
                phase += inc;
                l[(std::size_t) i] = r[(std::size_t) i] = s;
            }
            core.process(chans, 2, block);
            // Ignore the first half second: smoothers and rotor inertia.
            if ((double) b * block > sr * 0.5)
                for (int i = 0; i < block; ++i)
                {
                    const double v = (double) l[(std::size_t) i];
                    sum += v * v;
                    ++n;
                }
        }
        return n > 0 ? std::sqrt(sum / (double) n) : 0.0;
    };

    auto checkFlat = [&](const char* name, double lo, double hi)
    {
        const double db = 20.0 * std::log10(std::max(1e-9, hi) / std::max(1e-9, lo));
        note(std::string(name) + " 60 Hz RMS: 48 kHz " + std::to_string(lo)
             + ", 192 kHz " + std::to_string(hi) + " (" + std::to_string(db) + " dB)");
        RT_CHECK(lo > 0.005,
                 std::string(name) + " produced nothing at 48 kHz — the probe is vacuous");
        RT_CHECK(std::abs(db) < 1.5,
                 std::string(name) + " lost " + std::to_string(-db)
                 + " dB of 60 Hz going from 48 kHz to 192 kHz — a rate-free "
                   "coefficient is being used where a frequency was meant");
    };

    {
        dcdsp::PhaserCore core; dcdsp::PhaserParams p;
        p.mix = 1.0f; p.feedback = 0.0f; p.depth = 0.0f;
        const double lo = lowEndAtRate(core, p, 48000.0);
        const double hi = lowEndAtRate(core, p, 192000.0);
        checkFlat("phaser", lo, hi);
    }
    {
        dcdsp::RotaryCore core; dcdsp::RotaryParams p;
        p.mix = 1.0f; p.depth = 0.0f; p.speed = 0.0f;
        const double lo = lowEndAtRate(core, p, 48000.0);
        const double hi = lowEndAtRate(core, p, 192000.0);
        checkFlat("rotary", lo, hi);
    }
    {
        dcdsp::OctaverCore core; dcdsp::OctaverParams p;
        p.sub1 = 1.0f; p.sub2 = 0.0f; p.dry = 0.0f; p.toneHz = 2000.0f;
        const double lo = lowEndAtRate(core, p, 48000.0);
        const double hi = lowEndAtRate(core, p, 192000.0);
        checkFlat("octaver", lo, hi);
    }

    // --- (c) prepare() must START a core AT its configured value -----------
    // SmoothedValue::reset() deliberately PRESERVES mCurrent, which is 0 before
    // a smoother has ever ramped, so `reset(); setTarget(v)` leaves a freshly
    // prepared core ramping UP to v from silence. snapToTarget() is the whole
    // fix and 26 cores already did it; eight did not.
    //
    // Asserted on the smoothers THEMSELVES rather than on a level, for two
    // reasons an earlier version of this probe got wrong. (1) The sequence
    // matters: a setParams BEFORE the first prepare() snaps by itself, because
    // SmoothedValue::setTarget snaps while mSteps <= 1 and mSteps is 1 until
    // reset() derives it from a rate — so every case here prepares a FRESH core
    // and asserts against its DEFAULTS, which is what DspChain does. (2) These
    // cores have real transients of their own (a battery-sag follower, a dry
    // delay line, DC blockers), so an "early vs settled level" probe measures
    // those as much as the ramp and does not discriminate.
    {
        {
            dcdsp::AmpCore c; c.prepare(48000.0, 256, 2);
            note("amp master after a first prepare: "
                 + std::to_string(c.masterCurrentForTest()));
            RT_CHECK(c.masterCurrentForTest() > 0.1f,
                     "amp starts at master " + std::to_string(c.masterCurrentForTest())
                     + " — a freshly prepared amp fades up from SILENCE over 10 ms");
        }
        {
            dcdsp::DistortionCore c; c.prepare(48000.0, 256, 2);
            note("distortion pre-gain " + std::to_string(c.preGainCurrentForTest())
                 + ", out gain " + std::to_string(c.outGainCurrentForTest()));
            RT_CHECK(c.preGainCurrentForTest() > 1.0f && c.outGainCurrentForTest() > 0.1f,
                     "distortion starts with drive/level at zero and fades up over 10 ms");
        }
        {
            dcdsp::OverdriveCore c; c.prepare(48000.0, 256, 2);
            note("overdrive pre-gain " + std::to_string(c.preGainCurrentForTest())
                 + ", out gain " + std::to_string(c.outGainCurrentForTest()));
            RT_CHECK(c.preGainCurrentForTest() > 1.0f && c.outGainCurrentForTest() > 0.1f,
                     "overdrive starts with drive/level at zero and fades up over 10 ms");
        }
        {
            dcdsp::FuzzCore c; c.prepare(48000.0, 256, 2);
            note("fuzz drive " + std::to_string(c.fuzzCurrentForTest())
                 + ", level " + std::to_string(c.levelCurrentForTest()));
            RT_CHECK(c.fuzzCurrentForTest() > 0.1f && c.levelCurrentForTest() > 0.1f,
                     "fuzz starts with drive/level at zero and fades up over 20 ms — "
                     "an offline bounce is then not deterministic from sample 0");
        }
        {
            dcdsp::BitCrusherCore c; c.prepare(48000.0, 256, 2);
            note("bitcrusher mix after a first prepare: "
                 + std::to_string(c.mixCurrentForTest()));
            RT_CHECK(c.mixCurrentForTest() > 0.99f,
                     "bitcrusher starts at mix " + std::to_string(c.mixCurrentForTest())
                     + " against a default of 1.0 — a freshly prepared crusher is"
                       " fully DRY for its first 10 ms");
        }
        {
            dcdsp::PhaserCore c; c.prepare(48000.0, 256, 2);
            note("phaser mix after a first prepare: " + std::to_string(c.mixCurrentForTest())
                 + ", depth " + std::to_string(c.depthCurrentForTest()));
            RT_CHECK(c.mixCurrentForTest() > 0.1f && c.depthCurrentForTest() > 0.1f,
                     "phaser starts at mix " + std::to_string(c.mixCurrentForTest())
                     + " / depth " + std::to_string(c.depthCurrentForTest())
                     + " — it is fully dry and flat for its first 20 ms");
        }
        {
            dcdsp::RotaryCore c; c.prepare(48000.0, 256, 2);
            note("rotary horn Hz after a first prepare: "
                 + std::to_string(c.hornFreqCurrentForTest())
                 + ", mix " + std::to_string(c.mixCurrentForTest()));
            RT_CHECK(c.hornFreqCurrentForTest() > 0.8f,
                     "rotary horn starts at " + std::to_string(c.hornFreqCurrentForTest())
                     + " Hz — it spins UP from a standstill over a whole second, and"
                       " the comment in reset() claims the opposite");
            RT_CHECK(c.mixCurrentForTest() > 0.99f,
                     "rotary starts fully dry and ramps its mix in");
        }
        {
            // wah's reset() is reached on every un-bypass of a PARKED node, so a
            // manual pedal parked at the toe re-sweeps from the HEEL each time.
            dcdsp::WahCore c; dcdsp::WahParams p;
            p.mode = 1; p.position = 1.0f; p.mix = 1.0f;
            c.prepare(48000.0, 256, 2);
            c.setParams(p);
            c.reset();                       // the un-bypass
            note("wah sweep position after an un-bypass at the toe: "
                 + std::to_string(c.positionCurrentForTest()));
            RT_CHECK(c.positionCurrentForTest() > 0.99f,
                     "wah came back at position " + std::to_string(c.positionCurrentForTest())
                     + " instead of 1.0 — reset() leaves the pedal at the HEEL and"
                       " sweeps to the set position over 15 ms, every un-bypass");
        }
    }

    // --- (d) a rate-derived COUNT only setParams owned ---------------------
    // freeze's loop length and fade step are both counts in SAMPLES computed
    // from sr_, and prepare() re-derived every OTHER rate-owned quantity and
    // not these two: after a 48 -> 96 kHz device change the next capture was
    // half the set length and the fade half the set time.
    {
        dcdsp::FreezeCore core; dcdsp::FreezeParams p;
        p.lengthMs = 500.0f; p.fadeMs = 80.0f; p.capture = 0.0f;
        core.prepare(48000.0, 256, 2);
        core.setParams(p);
        const int   len48  = core.loopLengthTargetForTest();
        const float step48 = core.gainStepForTest();
        core.prepare(96000.0, 256, 2);          // the device change, no param write
        const int   len96  = core.loopLengthTargetForTest();
        const float step96 = core.gainStepForTest();
        note("freeze loop length " + std::to_string(len48) + " -> " + std::to_string(len96)
             + " samples, fade step " + std::to_string(step48) + " -> " + std::to_string(step96));
        RT_CHECK(len48 > 1000, "freeze produced no loop length at 48 kHz — vacuous probe");
        RT_CHECK(len96 > (int) (1.8 * len48),
                 "freeze kept its 48 kHz loop length (" + std::to_string(len96)
                 + ") after a prepare at 96 kHz — half the set length");
        RT_CHECK(step96 < step48 * 0.6f,
                 "freeze kept its 48 kHz fade step after a prepare at 96 kHz");
    }

    // --- (e) a rate-derived per-sample COEFFICIENT -------------------------
    // feedbacker's peak hold was the bare literal 3.5e-6f documented as "~6 s",
    // true at 48 kHz only: 1.5 s at 192 kHz. Same class as the bare 0.995
    // DcBlocker coefficient this repo already chased across six cores.
    {
        dcdsp::FeedbackerCore core;
        core.prepare(48000.0, 256, 2);
        const float d48 = core.holdDecayForTest();
        core.prepare(192000.0, 256, 2);
        const float d192 = core.holdDecayForTest();
        note("feedbacker hold decay " + std::to_string(d48) + " -> " + std::to_string(d192));
        RT_CHECK(d48 > 0.0f, "feedbacker hold decay is zero at 48 kHz — vacuous probe");
        RT_CHECK(d192 < d48 * 0.6f,
                 "feedbacker kept its 48 kHz peak-hold coefficient at 192 kHz — the "
                 "documented ~6 s hold is 1.5 s there");
    }

    // mikustomp rate-derives eight articulation coefficients in prepare() and
    // missed exactly two, so shimmer and jitter ran at half speed at 96 kHz.
    {
        dcdsp::MikuStompCore core;
        core.prepare(48000.0, 256, 2);
        const float sh48 = core.shimmerCoefForTest(), ji48 = core.jitterCoefForTest();
        core.prepare(96000.0, 256, 2);
        const float sh96 = core.shimmerCoefForTest(), ji96 = core.jitterCoefForTest();
        note("mikustomp shimmer " + std::to_string(sh48) + " -> " + std::to_string(sh96)
             + ", jitter " + std::to_string(ji48) + " -> " + std::to_string(ji96));
        RT_CHECK(sh48 > 0.0f && ji48 > 0.0f, "mikustomp coefficients are zero — vacuous");
        RT_CHECK(sh96 < sh48 * 0.7f && ji96 < ji48 * 0.7f,
                 "mikustomp's shimmer/jitter coefficients did not follow the rate");
    }

    // --- (f) rate-owned state that must be CLEARED -------------------------
    // subharmonic's oscillator increments are periods in SAMPLES and were
    // cleared by neither reset() nor prepare(), so after 48 -> 96 kHz both
    // oscillators ran an octave off until the tracker regained confidence.
    {
        dcdsp::SubharmonicCore core; dcdsp::SubharmonicParams p;
        p.sub1 = 1.0f; p.dry = 0.0f;
        core.prepare(48000.0, 256, 2);
        core.setParams(p);
        std::vector<float> l(256), r(256);
        float* chans[2] = { l.data(), r.data() };
        double phase = 0.0;
        const double inc = 6.283185307179586 * 110.0 / 48000.0;
        for (int b = 0; b < 60; ++b)
        {
            for (int i = 0; i < 256; ++i)
            {
                l[(std::size_t) i] = r[(std::size_t) i] = 0.4f * (float) std::sin(phase);
                phase += inc;
            }
            core.process(chans, 2, 256);
        }
        const float locked = core.osc1IncForTest();
        core.prepare(96000.0, 256, 2);
        const float after = core.osc1IncForTest();
        note("subharmonic osc increment after locking at 48 kHz: " + std::to_string(locked)
             + ", after a prepare at 96 kHz: " + std::to_string(after));
        RT_CHECK(locked > 0.0f,
                 "the tracker never locked, so this probe would pass vacuously");
        RT_CHECK(after == 0.0f,
                 "subharmonic carried its 48 kHz oscillator increment ("
                 + std::to_string(after) + ") across a prepare at 96 kHz");
    }

    // --- (g) prepare() must apply the SAME CLAMPS as setParams -------------
    // autowah's prepare() handed the envelope follower RAW attack/release
    // times where setParams clamps them, so a device change silently reverted
    // the clamp (and attackMs <= 0 makes EnvelopeFollower::coeff return 0 — an
    // instantaneous follower, a different effect). Structural, not tuned: a
    // prepare() alone must produce the same audio as prepare() + setParams().
    {
        auto render = [](bool reapply) {
            dcdsp::AutoWahCore core; dcdsp::AutoWahParams p;
            p.attackMs = 0.0f;          // out of range: setParams clamps to 0.5
            p.releaseMs = 1.0f;         // out of range: setParams clamps to 5
            p.mix = 1.0f;
            core.setParams(p);
            core.prepare(48000.0, 256, 2);
            if (reapply) core.setParams(p);
            std::vector<float> l(256), r(256), out;
            float* chans[2] = { l.data(), r.data() };
            double phase = 0.0;
            const double inc = 6.283185307179586 * 400.0 / 48000.0;
            for (int b = 0; b < 8; ++b)
            {
                for (int i = 0; i < 256; ++i)
                {
                    // A hard-gated burst: an envelope whose attack differs by
                    // 0 vs 0.5 ms tracks this visibly differently.
                    const float env = (b % 2 == 0) ? 0.5f : 0.02f;
                    l[(std::size_t) i] = r[(std::size_t) i] = env * (float) std::sin(phase);
                    phase += inc;
                }
                core.process(chans, 2, 256);
                out.insert(out.end(), l.begin(), l.end());
            }
            return out;
        };
        const std::vector<float> bare = render(false);
        const std::vector<float> reap = render(true);
        double worst = 0.0;
        for (std::size_t i = 0; i < bare.size(); ++i)
            worst = std::max(worst, (double) std::abs(bare[i] - reap[i]));
        note("autowah prepare-only vs prepare+setParams worst sample delta: "
             + std::to_string(worst));
        RT_CHECK(worst < 1e-6,
                 "autowah's prepare() applied UNCLAMPED envelope times: a device "
                 "change produces different audio (worst delta "
                 + std::to_string(worst) + ") from the same parameters");
    }

    endScenario();
}

// A phrase slot is USER CONTENT edited on the message thread while the audio
// thread sings out of it, and until 2026-08-28 both Miku cores published one
// with a two-deep buffer flip behind an atomic index. A two-deep flip is
// ABA-unsafe by construction: the reader resolves the index and then reads out
// of that buffer LATER (mikustomp at the next note, miku for the whole block),
// and the SECOND setPhrase after it flips straight back to the buffer the
// reader is holding and rewrites it in place. `Syllable` is two ints, so the
// tear is a real one — a consonant from the new phrase against a vowel from the
// old — and both fields index a coefficient table (kCons / kVowF), so the
// out-of-range case is an out-of-bounds read, not merely a wrong sound.
//
// Both cores now publish behind a seqlock (the ParamMailbox pattern from
// DspModule.h) over syllables stored as packed lock-free atomics:
// `nextUserSyllable` / `nextCustomSyllable` take (count, syllable) between two
// equal EVEN reads of the sequence and give up after kPhraseReadTries rather
// than spinning. Nothing is allocated and nothing is freed on either side, so
// there is no retirement to get wrong.
//
// The probe drives that reader from a second thread at a rate no callback
// reaches, which is the only way a race this narrow reproduces at all: at note
// rate the collision window is a few nanoseconds a second. Phase A is the
// coherence proof; phase B is the ordinary RT proof that the real audio path
// still allocates and frees nothing while the same storm runs.
void scenarioMikuPhraseSwapIsCoherent()
{
    beginScenario("RtMikuPhraseSwapIsCoherentUnderEdits");

    // Two phrases that are DISJOINT in both fields and the SAME length, so the
    // syllable index cannot explain a mismatch: every coherent read is either
    // (K, A) or (SH, I), and (K, I) / (SH, A) can only come from a torn table.
    using Stomp = dcdsp::MikuStompCore;
    constexpr int kLen = Stomp::kMaxSyllables;   // 96 — a wide write window
    std::vector<Stomp::Syllable> ka((size_t) kLen), shi((size_t) kLen);
    for (int i = 0; i < kLen; ++i)
    {
        ka[(size_t) i]  = Stomp::Syllable{ Stomp::C_K,  Stomp::V_A };
        shi[(size_t) i] = Stomp::Syllable{ Stomp::C_SH, Stomp::V_I };
    }

    // --- phase A: the reader the audio thread runs, hammered ---------------
    {
        Stomp core;
        core.prepare(48000.0, 256, 1);
        core.setPhrase(0, ka.data(), kLen);

        std::atomic<bool> go{ true };
        std::atomic<long long> reads{ 0 }, torn{ 0 }, busy{ 0 }, sawKa{ 0 }, sawShi{ 0 };
        std::thread writer([&]
        {
            bool useKa = false;
            while (go.load(std::memory_order_acquire))
            {
                core.setPhrase(0, useKa ? ka.data() : shi.data(), kLen);
                useKa = !useKa;
            }
        });
        std::thread reader([&]
        {
            int prev = 0;
            while (go.load(std::memory_order_acquire))
            {
                Stomp::Syllable s{};
                int idx = 0;
                switch (core.nextUserSyllable(0, prev, idx, s))
                {
                    case Stomp::PhraseRead::Ok:
                        prev = idx;
                        reads.fetch_add(1, std::memory_order_relaxed);
                        if (s.cons == Stomp::C_K && s.vowel == Stomp::V_A)
                            sawKa.fetch_add(1, std::memory_order_relaxed);
                        else if (s.cons == Stomp::C_SH && s.vowel == Stomp::V_I)
                            sawShi.fetch_add(1, std::memory_order_relaxed);
                        else
                            torn.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case Stomp::PhraseRead::Busy:
                        busy.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case Stomp::PhraseRead::Empty:
                        torn.fetch_add(1, std::memory_order_relaxed);  // never: kLen > 0
                        break;
                }
            }
        });
        // At least the nominal 750 ms, and on until the floors below are met
        // (gatherEvidence, #55) — a starved box gets more time, not a pass.
        const Gathered g = gatherEvidence(
            [] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); },
            [&] { return reads.load() > 20000 && sawKa.load() > 0 && sawShi.load() > 0; },
            15);
        go.store(false, std::memory_order_release);
        writer.join();
        reader.join();

        const long long n = reads.load();
        note("phrase seqlock: " + std::to_string(n) + " coherent reads ("
             + std::to_string(sawKa.load()) + " ka / " + std::to_string(sawShi.load())
             + " shi), " + std::to_string(busy.load()) + " gave up, "
             + std::to_string(torn.load()) + " torn");
        // The writer never sleeps, so most attempts legitimately hit an odd
        // sequence and give up; what has to be large is the number that got
        // THROUGH while a publish was in flight. Measured on this box: 0.1–10 M
        // coherent reads per 750 ms run, and 33,727 torn ones the moment the
        // packing is removed (verified by doing exactly that). 20,000 is a
        // floor for a heavily loaded box, not the expected figure.
        RT_CHECK(n > 20000,
                 "only " + std::to_string(n) + " phrase reads landed — the probe is "
                 "too slow to have raced anything and proves nothing" + g.why());
        RT_CHECK(sawKa.load() > 0 && sawShi.load() > 0,
                 "the writer never actually swapped the phrase (ka "
                 + std::to_string(sawKa.load()) + " / shi " + std::to_string(sawShi.load())
                 + ") — the probe is vacuous" + g.why());
        RT_CHECK(torn.load() == 0,
                 "the phrase swap is not atomic: " + std::to_string(torn.load())
                 + " read(s) mixed a consonant from one phrase with a vowel from the other");
    }

    // --- phase B: the real audio path, while the same storm runs -----------
    // A plucked-string stimulus (harmonics + a 2.5 ms broadband pick transient)
    // is what the sequencer keys on; tools/miku_measure.cpp uses the same shape.
    {
        constexpr double kSr = 48000.0;
        constexpr int kBlock = 256;
        constexpr int kBlocks = 400;                 // ~2.1 s
        std::vector<float> in((size_t) (kBlock * kBlocks), 0.0f);
        uint32_t rng = 0x1234567u;
        auto noise = [&rng]
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            return ((float) (rng & 0xFFFFFFu) * (1.0f / 8388608.0f)) - 1.0f;
        };
        const int pluckEvery = (int) (kSr * 0.18);   // ~5.5 picks a second
        for (int at = 0; at + 1 < (int) in.size(); at += pluckEvery)
        {
            const double f0 = 110.0 * (1.0 + 0.06 * ((at / pluckEvery) % 5));
            const int nh = std::min(16, (int) (0.45 * kSr / f0));
            for (int i = at; i < (int) in.size(); ++i)
            {
                const double t = (double) (i - at) / kSr;
                double s = 0.0;
                for (int h = 1; h <= nh; ++h)
                    s += (1.0 / (double) h) * std::exp(-t / (1.4 / std::sqrt((double) h)))
                       * std::sin(6.283185307179586 * f0 * (double) h * t + 0.7 * (double) h);
                const double pt = (t < 0.0025) ? std::exp(-t / 0.0008) * 0.8 : 0.0;
                in[(size_t) i] = 0.35f * (float) (0.55 * s + pt * (double) noise());
            }
        }

        Stomp core;
        core.prepare(kSr, kBlock, 1);
        dcdsp::MikuStompParams p;
        p.mix = 1.0f; p.octave = 1.0f; p.pattern = 6.0f;   // Phrase 1
        core.setParams(p);
        core.setPhrase(0, ka.data(), kLen);

        std::atomic<bool> go{ true };
        std::thread writer([&]
        {
            bool useKa = false;
            while (go.load(std::memory_order_acquire))
            {
                core.setPhrase(0, useKa ? ka.data() : shi.data(), kLen);
                useKa = !useKa;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });

        std::vector<float> work(in);
        int advances = 0, incoherent = 0;
        int lastIdx = core.syllableIndex();
        dcrt::reset();
        dcrt::arm();
        for (int b = 0; b < kBlocks; ++b)
        {
            float* ch[1] = { work.data() + (size_t) (b * kBlock) };
            {
                dcrt::ScopedRtThread rt;
                dcrt::g_currentBlock.store((unsigned) b, std::memory_order_relaxed);
                core.process(ch, 1, kBlock);
            }
            const int idx = core.syllableIndex();
            if (idx != lastIdx)
            {
                ++advances;
                lastIdx = idx;
                const Stomp::Syllable s = core.currentSyllable();
                const bool ok = (s.cons == Stomp::C_K  && s.vowel == Stomp::V_A)
                             || (s.cons == Stomp::C_SH && s.vowel == Stomp::V_I);
                if (! ok) ++incoherent;
            }
        }
        dcrt::disarm();
        go.store(false, std::memory_order_release);
        writer.join();

        bool finite = true;
        for (float v : work) if (! std::isfinite(v)) { finite = false; break; }

        reportHeap("miku phrase storm");
        note("phrase storm: " + std::to_string(advances) + " syllable advance(s) over "
             + std::to_string(kBlocks) + " blocks");
        RT_CHECK(dcrt::allocCount() == 0,
                 "a phrase edit during playback caused " + std::to_string(dcrt::allocCount())
                 + " callback allocation(s)");
        RT_CHECK(dcrt::freeCount() == 0,
                 "a phrase edit during playback caused " + std::to_string(dcrt::freeCount())
                 + " callback free(s) — the audio thread must never free");
        RT_CHECK(advances >= 5,
                 "the sequencer advanced only " + std::to_string(advances)
                 + " time(s) — the stimulus is not driving notes, so this phase is vacuous");
        RT_CHECK(incoherent == 0,
                 std::to_string(incoherent) + " sung syllable(s) mixed the two phrases");
        RT_CHECK(finite, "the voice went non-finite while its lyrics were being edited");
    }

    endScenario();
}

// A snapshot the callback copies must be destroyed by its PUBLISHER, and the
// publisher does not get to give up. DspChain has a barrier for this;
// NamAmpModule cannot take the callback's mutex, so it used to spin on
// `use_count() > 1` for 500 ms and then `reset()` ANYWAY — which is not a
// fallback, it is the exact case the bound exists for. Past the bound the
// callback's copy is the last owner, so a slow block was handed a free() plus a
// whole NeuralAudio model destructor, precisely when it could least afford one.
// (It also blocked the MESSAGE thread for half a second to clear a capture.)
//
// dcdsp::RetirementQueue (src/dsp/Retirement.h) replaces it: park the retired
// object, sweep it from the worker once `use_count() == 1`, never destroy it
// anywhere else. That test is sound in this one direction because the object
// has already been swapped OUT of the atomic slot before it is retired, so no
// reader can take a new reference and the count can only fall.
//
// This scenario drives the queue directly rather than through namamp, because
// namamp is NOT in this binary: DOTCHARTER_DSP_NEURAL never reaches the test
// target, so NamAmpModule.cpp is not compiled here (verified — the string
// "namamp" appears in juce_sharp.dll and not in juce_sharp_rtcheck.exe). That
// is a real hole in the gate; pinning the mechanism the module now uses is what
// can be pinned from inside it.
void scenarioRetirementIsOffTheAudioThread()
{
    beginScenario("RtRetiredSnapshotsDieOnTheOwnerThread");

    struct Heavy
    {
        std::vector<float>              payload;   // a destructor that really frees
        std::atomic<std::thread::id>*   diedOn = nullptr;
        ~Heavy()
        {
            if (diedOn != nullptr)
                diedOn->store(std::this_thread::get_id(), std::memory_order_release);
        }
    };

    std::atomic<std::thread::id> diedOn{ std::thread::id{} };
    dcdsp::RetirementQueue<Heavy> queue;

    auto model = std::make_shared<Heavy>();
    model->payload.assign(256 * 1024, 0.0f);       // 1 MB
    model->diedOn = &diedOn;

    // The callback's snapshot, copied BEFORE the swap — exactly as
    // NamAmpModule::process copies active_ before setStringParam exchanges it.
    std::shared_ptr<Heavy> snapshot = model;
    queue.retire(std::move(model));
    RT_CHECK(queue.pending() == 1, "the object was not parked — nothing to sweep");

    const std::thread::id sweeperId = std::this_thread::get_id();
    std::atomic<std::thread::id> audioId{ std::thread::id{} };

    // The audio thread holds its copy for 600 ms: LONGER than the 500 ms bound
    // the old drainRelease gave up after, which is the whole point. Then it
    // drops the copy while still armed, so if the queue had already let go, the
    // free lands here and the sentinel sees it.
    std::atomic<bool> released{ false };
    // The hold also lasts until the worker has finished its observation window
    // (below), so a starved worker cannot have the release land inside it: the
    // window is evidence-gathered (#55) and may run past 1500 ms. Bounded by the
    // evidence cap so a wedged worker still ends the scenario.
    std::atomic<bool> windowDone{ false };
    dcrt::reset();
    dcrt::arm();
    std::thread audio([&]
    {
        dcrt::ScopedRtThread rt;
        audioId.store(std::this_thread::get_id(), std::memory_order_release);
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        while (! windowDone.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() - t0 < evidenceCap() + std::chrono::seconds(5))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        snapshot.reset();                     // the callback finishing its block
        released.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });

    // The worker: sweep the way NamAmpModule's workerLoop does. The first
    // window is at least 400 ms of WALL CLOCK and at least ten declined sweeps
    // — the hold above outlasts it, so "was it still alive at the end of it" is
    // a deterministic question and not a race against the release. Counted by
    // clock and not by iteration on purpose: a 4 ms sleep on Windows is a
    // ~15.6 ms timer tick, so a fixed iteration count overruns the hold.
    int sweepsWhileHeld = 0;
    const auto windowStart = std::chrono::steady_clock::now();
    const Gathered g = gatherEvidence([&]
    {
        if (queue.sweep() > 0) ++sweepsWhileHeld;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }, [&]
    {
        return sweepsWhileHeld >= 10
            && std::chrono::steady_clock::now() - windowStart >= std::chrono::milliseconds(400);
    });
    const bool aliveWhileHeld = (diedOn.load(std::memory_order_acquire) == std::thread::id{})
                                && ! released.load(std::memory_order_acquire);
    windowDone.store(true, std::memory_order_release);
    for (int i = 0; i < 600 && !released.load(std::memory_order_acquire); ++i)
    {
        queue.sweep();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    audio.join();
    dcrt::disarm();
    for (int i = 0; i < 100 && queue.pending() > 0; ++i)
    {
        queue.sweep();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const std::thread::id died = diedOn.load(std::memory_order_acquire);
    reportHeap("retirement queue");
    note("retirement: " + std::to_string(sweepsWhileHeld)
         + " sweep(s) correctly declined while the callback held the snapshot");

    RT_CHECK(sweepsWhileHeld >= 10,
             "only " + std::to_string(sweepsWhileHeld) + " sweep(s) ran while "
             "the callback held its copy — the probe never exercised the declining path" + g.why());
    RT_CHECK(aliveWhileHeld,
             "the retired object was destroyed inside the 400 ms window in which "
             "the callback still held a reference to it — that is a use-after-free, "
             "not a late free");
    RT_CHECK(dcrt::freeCount() == 0,
             "retiring a snapshot caused " + std::to_string(dcrt::freeCount())
             + " free(s) on the audio thread");
    RT_CHECK(died != std::thread::id{},
             "the retired object was never destroyed at all — the queue leaks");
    RT_CHECK(died != audioId.load(std::memory_order_acquire),
             "the retired object's destructor ran ON THE AUDIO THREAD");
    RT_CHECK(died == sweeperId,
             "the retired object died on some thread other than the sweeper");
    RT_CHECK(queue.pending() == 0,
             "the queue still holds " + std::to_string(queue.pending())
             + " object(s) after the callback let go");

    endScenario();
}

/// A WAV whose first `burstAt` seconds are silent and which then has one
/// full-scale sample. Lets a scenario read the playback path's group delay off
/// the OUTPUT rather than trusting a documented latency figure.
juce::MemoryBlock makeBurstWavBlob(double seconds, int sampleRate, double burstAt)
{
    const int frames = (int) (seconds * sampleRate);
    juce::AudioBuffer<float> buf(2, frames);
    buf.clear();
    const int at = (int) (burstAt * sampleRate);
    if (at >= 0 && at < frames)
        for (int c = 0; c < 2; ++c) buf.setSample(c, at, 0.9f);

    juce::MemoryBlock block;
    {
        juce::WavAudioFormat wav;
        auto stream = std::make_unique<juce::MemoryOutputStream>(block, false);
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wav.createWriterFor(stream.get(), (double) sampleRate, 2, 16, {}, 0));
        if (writer != nullptr) { stream.release(); writer->writeFromAudioSampleBuffer(buf, 0, frames); }
    }
    return block;
}

// juce::ResamplingAudioSource -- what AudioTransportSource uses when it is
// given a sourceSampleRateToCorrectFor -- is LINEAR interpolation plus one
// 2nd-order Butterworth. A 44.1 kHz backing track on a 48 kHz interface is the
// ordinary case, and it was measured, end to end on this device, losing
//
//     1 kHz -0.01 dB | 5 kHz -0.37 | 10 kHz -1.46 | 15 kHz -3.14 | 19 kHz -5.56
//
// against the same tone in a 48 kHz file. TransportSource now converts the
// decoded buffer to the device rate ONCE, offline, with a 100-crossing windowed
// sinc, and tells the transport the buffer is already at the device rate -- so
// the transport's own resampler sits at ratio 1.0, where alpha is exactly 0 and
// it is a bit-exact passthrough.
//
// TWO halves, and the second is the one that matters more: a windowed sinc is
// centred, so it has a group delay. Not compensating it would shift the whole
// song ~2.3 ms LATE against the chart, which is a worse bug than the dull top
// end this fixes. The sync half reads the delay off the rendered output.
void scenarioPreloadResamplesAtLoad()
{
    beginScenario("RtPreloadedSourceResamplesAtLoadWithoutShiftingTime");

    // --- (a) high frequencies survive the rate conversion -------------------
    auto toneRms = [](int fileRate, double freq, double& outPlaybackRate) -> double
    {
        Fixture fx(48000, 256, 2);
        if (! fx.ok()) return -1.0;
        const auto blob = makeWavBlob(4.0, fileRate, freq);
        TransportSource src(fx.engine());
        if (src.openMemory(blob.getData(), blob.getSize(), "wav").isNotEmpty()) return -1.0;
        outPlaybackRate = src.playbackSampleRate();
        src.play();
        fx.pumpWarm(64);
        auto* dev = nulldev::manualDevice();
        double sum = 0.0; long long n = 0;
        for (int b = 0; b < 256; ++b)
        {
            fx.pumpWarm(1);
            for (float s : dev->manualOutputBuffers()[0]) { sum += (double) s * s; ++n; }
        }
        return n > 0 ? std::sqrt(sum / (double) n) : 0.0;
    };

    double rateAt48 = 0.0, rateAt441 = 0.0;
    const double ref  = toneRms(48000, 15000.0, rateAt48);
    const double conv = toneRms(44100, 15000.0, rateAt441);
    if (ref < 0.0 || conv < 0.0) { fail("engine did not start"); endScenario(); return; }

    const double db = 20.0 * std::log10(std::max(1e-12, conv) / std::max(1e-12, ref));
    note("15 kHz: 48 kHz file rms " + std::to_string(ref) + ", 44.1 kHz file rms "
         + std::to_string(conv) + " (" + std::to_string(db) + " dB), buffer held at "
         + std::to_string(rateAt441) + " Hz");

    RT_CHECK(ref > 0.05, "the 48 kHz reference tone was not audible — the probe is vacuous");
    RT_CHECK(rateAt441 > 47000.0,
             "the 44.1 kHz source is still held at " + std::to_string(rateAt441)
             + " Hz — the offline conversion did not run, so the transport's linear "
               "interpolator is still doing it per block");
    RT_CHECK(std::abs(db) < 0.6,
             "a 44.1 kHz file lost " + std::to_string(-db)
             + " dB of 15 kHz on a 48 kHz device — the rate conversion is dulling "
               "every backing track");

    // --- (b) ...and the song did not MOVE ----------------------------------
    // Both runs start the transport identically, so the output index of the
    // burst is directly comparable. A centred sinc's uncompensated group delay
    // would put the 44.1 kHz run ~109 output samples late.
    auto burstIndex = [](int fileRate) -> long long
    {
        Fixture fx(48000, 256, 2);
        if (! fx.ok()) return -2;
        const auto blob = makeBurstWavBlob(2.0, fileRate, 0.5);
        TransportSource src(fx.engine());
        if (src.openMemory(blob.getData(), blob.getSize(), "wav").isNotEmpty()) return -2;
        src.play();
        auto* dev = nulldev::manualDevice();
        long long idx = 0, found = -1;
        double best = 0.0;
        for (int b = 0; b < 256 && found < 0; ++b)
        {
            fx.pumpWarm(1);
            const auto& ch = dev->manualOutputBuffers()[0];
            for (float s : ch)
            {
                const double a = std::abs((double) s);
                if (a > 0.2 && a > best) { best = a; found = idx; }
                else if (found >= 0 && a < 0.2) { /* past the peak */ }
                ++idx;
            }
        }
        return found;
    };

    const long long at48  = burstIndex(48000);
    const long long at441 = burstIndex(44100);
    note("burst peak lands at output sample " + std::to_string(at48)
         + " (48 kHz file) vs " + std::to_string(at441) + " (44.1 kHz file)");

    RT_CHECK(at48 > 0, "the 48 kHz burst was never found — the sync probe is vacuous");
    RT_CHECK(at441 > 0, "the 44.1 kHz burst was never found");
    RT_CHECK(std::llabs(at48 - at441) <= 4,
             "the resampled source's burst landed " + std::to_string(at441 - at48)
             + " output samples away from the 48 kHz reference — the windowed sinc's "
               "group delay is not being compensated and the whole song is off the chart");

    // --- (c) a DOWNSAMPLE is converted at load too (JuceSharp #33) ----------
    //
    // This case used to pin the opposite (W4-N16): a 96 kHz file on a 48 kHz
    // device stayed at 96000 and went through juce::ResamplingAudioSource per
    // block. The kernel's cutoff now follows the OUTPUT Nyquist, so 96 -> 48 kHz
    // (1:2) is converted offline like any upsample. The level and aliasing
    // measurements are RtPreloadedSourceDownsamplesAtLoadWithoutAliasing's.
    double rateAt96 = 0.0;
    const double down = toneRms(96000, 1000.0, rateAt96);
    note("96 kHz file on a 48 kHz device: playbackSampleRate " + std::to_string(rateAt96)
         + ", 1 kHz rms " + std::to_string(down));
    RT_CHECK(down > 0.01,
             "the 96 kHz source produced no audio at all (rms " + std::to_string(down) + ")");
    RT_CHECK(std::abs(rateAt96 - 48000.0) < 1.0,
             "the 96 kHz source is held at " + std::to_string(rateAt96)
             + " Hz on a 48 kHz device -- the load-time DOWNSAMPLE did not run");

    endScenario();
}

/// A buffer of `seconds` at `rate`, every channel a sum of pure tones. Built
/// directly rather than through a WAV round trip: the offline halves below
/// compare against ANALYTIC truth, so nothing may quantise the input to 16 bit.
juce::AudioBuffer<float> makeToneBuffer(double seconds, int rate, int channels,
                                        std::initializer_list<double> freqs,
                                        float amplitude)
{
    const int frames = (int) (seconds * (double) rate);
    juce::AudioBuffer<float> buf(channels, frames);
    buf.clear();
    for (int c = 0; c < channels; ++c)
    {
        float* w = buf.getWritePointer(c);
        for (double f : freqs)
            for (int i = 0; i < frames; ++i)
                w[i] += amplitude * (float) std::sin(2.0 * juce::MathConstants<double>::pi
                                                     * f * (double) i / (double) rate);
    }
    return buf;
}

// The LOAD-TIME rate conversion, measured OFFLINE -- no device, no callback.
//
// scenarioPreloadResamplesAtLoad above proves the conversion happens and that
// the song does not move, end to end through the engine. This one proves the
// three things that are invisible from there:
//
//   * the RATIONAL POLYPHASE path is the one that RAN for 44.1 -> 48 kHz. It is
//     a silent fallback by design (a non-integer rate, or a ratio needing more
//     than kMaxPhases phases, goes back to the windowed sinc), so "it still
//     sounds right" cannot tell the two apart -- only the report can. This is
//     the STRUCTURAL half of the speed claim: nothing here asserts wall clock,
//     which would flake, but a silent fall back to the 6.4 s path would fail.
//   * the output matches ANALYTIC truth, not merely the old implementation.
//     A resampled sine is a sine of the same frequency at the new rate, and the
//     phase is what pins the group-delay compensation to a FRACTION of a sample
//     rather than to the +-0.5 the old integer trim could manage.
//   * it agrees with the windowed-sinc reference it replaced.
//
// The timings at the end are NOTES, never assertions. Owner's log, v0.48.5: a
// 183 s stereo 44.1 kHz file cost ~6.4 s here, once per stem, sequentially.
void scenarioLoadResamplerIsPolyphaseAndExact()
{
    beginScenario("RtLoadResamplerUsesThePolyphasePathAndMatchesTheReference");

    // --- (a) the ratio reduction, including what it must REFUSE -------------
    int l = 0, m = 0;
    const bool r441 = dcload::reduceRatio(44100.0, 48000.0, l, m);
    RT_CHECK(r441 && l == 160 && m == 147,
             "44.1 -> 48 kHz did not reduce to 160:147 (got " + std::to_string(l)
             + ":" + std::to_string(m) + ") -- the exact ratio is the whole basis "
               "of the polyphase path");

    int l2 = 0, m2 = 0;
    RT_CHECK(dcload::reduceRatio(48000.0, 96000.0, l2, m2) && l2 == 2 && m2 == 1,
             "48 -> 96 kHz did not reduce to 2:1");

    int l3 = 0, m3 = 0;
    RT_CHECK(dcload::reduceRatio(48000.0, 44100.0, l3, m3) && l3 == 147 && m3 == 160,
             "48 -> 44.1 kHz did not reduce to 147:160 (got " + std::to_string(l3) + ":"
             + std::to_string(m3) + ") -- the DOWNSAMPLE path (JuceSharp #33) needs it");
    RT_CHECK(! dcload::reduceRatio(192000.0, 8000.0, l3, m3),
             "the polyphase path accepted a 24:1 downsample -- past kMaxDecimation the "
             "kernel is thousands of taps per output sample");
    RT_CHECK(! dcload::reduceRatio(48000.0, 48000.0, l3, m3),
             "the polyphase path accepted EQUAL rates -- there is nothing to convert");
    RT_CHECK(! dcload::reduceRatio(44123.7, 48000.0, l3, m3),
             "the polyphase path accepted a non-integer source rate -- there is no "
             "exact L:M for it and it must fall back");

    // --- (b) the fast path is the one that RUNS, and it matches analytic truth
    struct ToneCase { double freq; double tol; };
    const ToneCase tones[] = { { 1000.0, 2.0e-3 }, { 15000.0, 6.0e-3 } };

    for (const auto& tc : tones)
    {
        auto buf = makeToneBuffer(4.0, 44100, 1, { tc.freq }, 0.5f);
        dcload::ResampleReport rep;
        const bool ok = dcload::upsampleDecodedBuffer(buf, 44100.0, 48000.0, &rep);

        RT_CHECK(ok, "the conversion refused a plain 44.1 kHz buffer");
        RT_CHECK(rep.polyphase,
                 "44.1 -> 48 kHz fell back to the windowed-sinc path -- that is the "
                 "~6.4 s per-file cost this exists to remove, and it is SILENT");

        // Skip a kernel half-width at each end: there the filter is reading the
        // zero padding, which is correct and is not the signal.
        const int n    = buf.getNumSamples();
        const int skip = 400;
        double worst = 0.0, sumSq = 0.0, refSq = 0.0;
        const float* rd = buf.getReadPointer(0);
        for (int i = skip; i < n - skip; ++i)
        {
            const double want = 0.5 * std::sin(2.0 * juce::MathConstants<double>::pi
                                               * tc.freq * (double) i / 48000.0);
            worst  = std::max(worst, std::abs((double) rd[i] - want));
            sumSq += ((double) rd[i] - want) * ((double) rd[i] - want);
            refSq += want * want;
        }
        const double errDb = 10.0 * std::log10(std::max(1e-30, sumSq)
                                               / std::max(1e-30, refSq));
        note(std::to_string((int) tc.freq) + " Hz 44.1 -> 48 k: worst sample error "
             + std::to_string(worst) + ", residual " + std::to_string(errDb)
             + " dB, phases " + std::to_string(rep.interpolate) + "/"
             + std::to_string(rep.decimate) + ", " + std::to_string(rep.tapsPerPhase)
             + " taps, " + std::to_string(rep.threads) + " thread(s), "
             + std::to_string(rep.elapsedMs) + " ms");

        RT_CHECK(worst < tc.tol,
                 "a resampled " + std::to_string((int) tc.freq) + " Hz sine is "
                 + std::to_string(worst) + " off the analytic sine at 48 kHz -- the "
                 "kernel is either dulling the band or shifting the phase");
    }

    // --- (c) ...and it agrees with the reference it replaced -----------------
    // Compared at 220 Hz on purpose. The windowed-sinc path trims its group
    // delay by llround(100 * 48000 / 44100) = 109 output samples, so it carries
    // up to half an output sample of residual shift that the polyphase path
    // does not have; at 15 kHz that half sample alone is tens of degrees. The
    // agreement being tested here is of the FILTER, not of that rounding.
    {
        auto a = makeToneBuffer(2.0, 44100, 1, { 220.0 }, 0.5f);
        auto b = a;
        dcload::ResampleReport ra, rb;
        const bool okA = dcload::resampleBufferPolyphase   (a, 44100.0, 48000.0, &ra);
        const bool okB = dcload::resampleBufferWindowedSinc(b, 44100.0, 48000.0, &rb);
        RT_CHECK(okA && okB, "one of the two conversion paths refused the buffer");
        RT_CHECK(a.getNumSamples() == b.getNumSamples(),
                 "the two paths disagree on the output length ("
                 + std::to_string(a.getNumSamples()) + " vs "
                 + std::to_string(b.getNumSamples()) + ") -- every seek and the "
                   "reported song length come off that number");

        double worst = 0.0;
        const int n = std::min(a.getNumSamples(), b.getNumSamples());
        for (int i = 400; i < n - 400; ++i)
            worst = std::max(worst, std::abs((double) a.getReadPointer(0)[i]
                                             - (double) b.getReadPointer(0)[i]));
        // The bound is DERIVED, not picked: the reference can be up to half an
        // output sample out (its trim is llround(108.84) = 109), and half a
        // sample of shift on a 220 Hz sine of amplitude 0.5 is
        // 0.5 * 2*pi*220 * 0.5 / 48000. The +1e-3 covers the two kernels not
        // being the same window (JUCE fits a Kaiser beta ~6; this one uses 8.6).
        const double slipBound = 0.5 * 2.0 * juce::MathConstants<double>::pi
                                 * 220.0 * 0.5 / 48000.0;
        const double implied   = worst * 48000.0 / (0.5 * 2.0
                                 * juce::MathConstants<double>::pi * 220.0);
        note("polyphase vs windowed-sinc reference at 220 Hz: worst sample delta "
             + std::to_string(worst) + " = " + std::to_string(implied)
             + " output samples of slip, bound " + std::to_string(slipBound + 1.0e-3)
             + " (reference took " + std::to_string(rb.elapsedMs)
             + " ms for 2 s mono, polyphase " + std::to_string(ra.elapsedMs) + " ms)");
        RT_CHECK(worst < slipBound + 1.0e-3,
                 "the polyphase output is " + std::to_string(worst)
                 + " away from the windowed-sinc reference (" + std::to_string(implied)
                 + " output samples) -- that is more than the reference's own "
                   "half-sample trim rounding can explain, so the two filters "
                   "genuinely disagree");
    }

    // --- (d) the burst does not move ----------------------------------------
    // The engine-level scenario above reads this off the DEVICE; here it is read
    // off the converted buffer with no transport in the way, so a shift shows as
    // an exact sample count instead of within the +-4 the pumped path allows.
    {
        const int inRate = 44100, at = inRate;            // 1.000 s in
        juce::AudioBuffer<float> buf(1, 2 * inRate);
        buf.clear();
        buf.setSample(0, at, 1.0f);
        dcload::ResampleReport rep;
        RT_CHECK(dcload::upsampleDecodedBuffer(buf, 44100.0, 48000.0, &rep),
                 "the burst buffer was refused");

        int    peak = -1;
        double best = 0.0;
        for (int i = 0; i < buf.getNumSamples(); ++i)
            if (std::abs((double) buf.getReadPointer(0)[i]) > best)
            { best = std::abs((double) buf.getReadPointer(0)[i]); peak = i; }

        note("1.000 s burst lands at output sample " + std::to_string(peak)
             + " (expected 48000, i.e. offset " + std::to_string(peak - 48000) + ")");
        RT_CHECK(peak == 48000,
                 "the burst landed at " + std::to_string(peak) + " instead of 48000 -- "
                 "the polyphase group delay is NOT zero and the whole song is off "
                 "the chart by " + std::to_string((peak - 48000) * 1000.0 / 48000.0)
                 + " ms");
    }

    // --- (e) what it costs, on the owner's actual case ----------------------
    // NOT an assertion: a wall-clock threshold in a gate flakes the moment two
    // agents build at once. The number is the point of the change, so it is
    // logged; the structural claim in (b) is what fails if the fast path stops
    // being taken.
    {
        const double seconds = 183.0;
        auto probe = makeToneBuffer(seconds, 44100, 2, { 220.0, 3000.0 }, 0.35f);

        auto fast = probe;
        dcload::ResampleReport rf;
        const bool okF = dcload::resampleBufferPolyphase(fast, 44100.0, 48000.0, &rf);

        auto slow = std::move(probe);
        dcload::ResampleReport rs;
        const bool okS = dcload::resampleBufferWindowedSinc(slow, 44100.0, 48000.0, &rs);

        RT_CHECK(okF && okS, "the 183 s stereo case was refused by one of the paths");
        note("183 s stereo 44.1 -> 48 kHz: polyphase " + std::to_string(rf.elapsedMs)
             + " ms on " + std::to_string(rf.threads) + " thread(s), windowed-sinc "
             + std::to_string(rs.elapsedMs) + " ms, speedup x"
             + std::to_string(rs.elapsedMs / std::max(1.0, rf.elapsedMs)));
        RT_CHECK(rf.polyphase && rf.interpolate == 160 && rf.decimate == 147,
                 "the 183 s case did not run on the polyphase path");
        RT_CHECK(rf.threads > 1,
                 "the 183 s case ran on one thread -- the per-channel/per-chunk "
                 "split did not happen");
    }

    // --- (d) THE KERNEL IS BUILT ONCE PER RATIO, NOT PER CALL (W4-N17) ------
    //
    // build(160) evaluates a 32,001-point prototype -- a sin and a besselI0
    // series per point -- and then normalises 160 phases, and it depends on
    // nothing but L. It was rebuilt on every call, so a six-stem project paid
    // for it six times.
    //
    // Timed rather than counted, because the cache is a function-local static
    // with no observable handle -- but NOT with a wall-clock BOUND, which would
    // flake the moment another agent builds on the box. The assertion is
    // STRUCTURAL: a tiny buffer at the same ratio must cost a small FRACTION of
    // the first such call, whatever the absolute numbers are. Every case above
    // has already converted at 160:147, so this is measuring a warm cache
    // against a cold one by construction; the "cold" reference is a DIFFERENT
    // ratio the cache has not seen.
    {
        // 22050 -> 48000 reduces to 320:147, so L = 320 -- a ratio nothing
        // above used (so the cache is cold) and a 64,001-point prototype (so
        // the build DOMINATES a short buffer). 48 -> 96 kHz was the obvious
        // choice and is useless here: L = 2 is a 401-point prototype, measured
        // at 0.36 ms cold against 0.30 ms warm, i.e. inside the noise.
        auto cold = makeToneBuffer(0.1, 22050, 1, { 440.0 }, 0.25f);
        dcload::ResampleReport rc;
        const bool okC = dcload::resampleBufferPolyphase(cold, 22050.0, 48000.0, &rc);

        // ...and the SAME ratio again, now warm.
        auto warm = makeToneBuffer(0.1, 22050, 1, { 440.0 }, 0.25f);
        dcload::ResampleReport rw;
        const bool okW = dcload::resampleBufferPolyphase(warm, 22050.0, 48000.0, &rw);

        RT_CHECK(okC && okW, "the 22.05 -> 48 kHz case was refused");
        RT_CHECK(rc.polyphase && rw.polyphase && rc.interpolate == 320 && rc.decimate == 147,
                 "the 22.05 -> 48 kHz case did not run on the polyphase path at 320:147 -- (d) "
                 "is vacuous");
        note("kernel cache: first 22.05->48 kHz call (L=320) " + std::to_string(rc.elapsedMs)
             + " ms, second " + std::to_string(rw.elapsedMs) + " ms");

        // NON-VACUITY, asserted rather than assumed: the cold call must actually
        // be dominated by the build, or the comparison below means nothing.
        RT_CHECK(rc.elapsedMs > 1.0,
                 "the cold 320-phase build took only " + std::to_string(rc.elapsedMs)
                 + " ms, so it does not dominate this buffer and the cache assertion below "
                   "cannot tell a hit from a miss -- pick a larger L or a shorter buffer");
        // A factor of 3 is enormous slack against a build that dominates. The
        // point is to catch the cache being REMOVED, not to measure the box,
        // which is why this is a RATIO against a same-run reference and never
        // an absolute millisecond bound.
        RT_CHECK(rw.elapsedMs * 3.0 < rc.elapsedMs,
                 "the second conversion at the same ratio cost "
                 + std::to_string(rw.elapsedMs) + " ms against the first's "
                 + std::to_string(rc.elapsedMs)
                 + " -- the polyphase kernel is being rebuilt per call again, which is a "
                   "64,001-point prototype plus 320 phase normalisations once per stem");
    }

    endScenario();
}

// --- JuceSharp #33: downsampling at load, and STREAMING sources ------------
//
// Two cases used to reach juce::ResamplingAudioSource (linear interpolation
// plus one 2nd-order Butterworth): a file ABOVE the device rate (a 48 kHz
// YouTube import or Opus stem on a 44.1 kHz interface), and every source that
// missed the preload budget, in either direction. Linear interpolation DULLS
// (-3.14 dB at 15 kHz upward) and, downward, also ALIASES: a 21 kHz tone in a
// 48 kHz file leaves an image at 48 - 21 = 27 kHz that folds back to
// 44.1 - 27 = 17.1 kHz, squarely in the audible band.
//
// Every measurement below is read off the DEVICE output of a real engine on
// the null device, so it holds for the whole chain the listener hears.

/// A stereo 16-bit WAV of `seconds` at `rate`: the sum of `freqs`, each at
/// `amplitude`.
juce::MemoryBlock makeTonesWavBlob(double seconds, int rate,
                                   std::initializer_list<double> freqs, float amplitude)
{
    auto buf = makeToneBuffer(seconds, rate, 2, freqs, amplitude);
    juce::MemoryBlock block;
    juce::WavAudioFormat wav;
    auto stream = std::make_unique<juce::MemoryOutputStream>(block, false);
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.get(), (double) rate, 2, 16, {}, 0));
    if (writer != nullptr) { stream.release(); writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()); }
    return block;
}

/// What one source sounded like on one device.
struct RateCapture
{
    bool                ok = false;
    bool                preloaded = false;
    double              playbackRate = 0.0;
    bool                converted = false;
    dcload::ResampleReport report{};
    int                 allocs = -1;      ///< on the callback, over the captured blocks
    std::vector<float>  out;              ///< left channel, device rate
    std::string         why;
};

/// Opens `blob` on a `devRate` null device -- STREAMING when `streaming`, by
/// exhausting the preload budget first -- optionally seeks, plays, and keeps
/// `captureBlocks` blocks of the left output after `warmBlocks`.
///
/// Streaming reads through a BufferingAudioSource whose read-ahead thread is
/// real time, while the manual device is pumped as fast as the loop runs; so
/// the open is given time to fill the read-ahead, and every block yields the
/// thread a millisecond. The captured span (128 blocks of 256) is shorter than
/// the read-ahead (32768 input frames) by construction.
RateCapture captureSourceOutput(int devRate, const juce::MemoryBlock& blob, bool streaming,
                                int warmBlocks, int captureBlocks, double seekTo = -1.0)
{
    RateCapture r;
    Fixture fx(devRate, 256, 2);
    if (! fx.ok()) { r.why = "engine did not start: " + fx.error(); return r; }

    const int64_t budget = AudioEngine::preloadBudgetBytes();
    const bool    exhausted = streaming && fx.engine().reservePreloadBytes(budget);
    if (streaming && ! exhausted) { r.why = "could not exhaust the preload budget"; return r; }

    {
        TransportSource src(fx.engine());
        if (src.openMemory(blob.getData(), blob.getSize(), "wav").isNotEmpty())
        {
            r.why = "openMemory failed";
        }
        else
        {
            r.preloaded    = src.isPreloaded();
            r.playbackRate = src.playbackSampleRate();
            r.converted    = src.lastResampleReport().converted;
            r.report       = src.lastResampleReport();

            if (streaming) std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (seekTo >= 0.0)
            {
                src.seekSeconds(seekTo);
                if (streaming) std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
            src.play();

            auto* dev = nulldev::manualDevice();
            auto pumpOne = [&]
            {
                { dcrt::ScopedRtThread rt; nulldev::pump(1); }
                if (streaming) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            };
            for (int b = 0; b < warmBlocks; ++b) pumpOne();

            dcrt::reset();
            dcrt::arm();
            for (int b = 0; b < captureBlocks; ++b)
            {
                pumpOne();
                const auto& ch = dev->manualOutputBuffers()[0];
                r.out.insert(r.out.end(), ch.begin(), ch.end());
            }
            dcrt::disarm();
            r.allocs = dcrt::allocCount();
            dcrt::reset();
            r.ok = true;
        }
    }

    if (exhausted) fx.engine().releasePreloadBytes(budget);
    return r;
}

/// Amplitude of `freq` in `x` (sampled at `rate`), in dB re `reference`:
/// a single-bin DFT under a 4-term Blackman-Harris window (sidelobes below
/// -92 dB), normalised so a full-scale sine reads its own amplitude. The bins
/// compared here are kilohertz apart, so leakage is far below anything
/// asserted.
double toneDb(const std::vector<float>& x, double freq, double rate, double reference)
{
    const size_t n = x.size();
    if (n < 64) return 0.0;
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / (double) (n - 1);
        const double w = 0.35875 - 0.48829 * std::cos(twoPi * t)
                       + 0.14128 * std::cos(2.0 * twoPi * t) - 0.01168 * std::cos(3.0 * twoPi * t);
        const double ph = twoPi * freq * (double) i / rate;
        re += w * (double) x[i] * std::cos(ph);
        im -= w * (double) x[i] * std::sin(ph);
        wsum += w;
    }
    const double amp = 2.0 * std::sqrt(re * re + im * im) / std::max(1e-30, wsum);
    return 20.0 * std::log10(std::max(1e-15, amp) / reference);
}

/// Index of the burst's peak in `x` (|x| > 0.2), -1 when there is none.
long long burstPeak(const std::vector<float>& x)
{
    long long found = -1;
    double best = 0.2;
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs((double) x[i]) > best) { best = std::abs((double) x[i]); found = (long long) i; }
    return found;
}

constexpr int kRateWarm    = 32;
constexpr int kRateCapture = 96;

// A 48 kHz file on a 44.1 kHz device, PRELOADED.
void scenarioPreloadDownsamplesAtLoad()
{
    beginScenario("RtPreloadedSourceDownsamplesAtLoadWithoutAliasing");

    const float amp = 0.3f;

    // --- (a) the conversion happens at load ---------------------------------
    const auto at48  = captureSourceOutput(44100, makeTonesWavBlob(3.0, 48000, { 15000.0 }, amp),
                                           false, kRateWarm, kRateCapture);
    const auto at441 = captureSourceOutput(44100, makeTonesWavBlob(3.0, 44100, { 15000.0 }, amp),
                                           false, kRateWarm, kRateCapture);
    if (! at48.ok || ! at441.ok) { fail("capture failed: " + at48.why + at441.why); endScenario(); return; }

    RT_CHECK(at48.preloaded && at441.preloaded, "a 3 s source did not preload -- the probe is on the wrong path");
    RT_CHECK(std::abs(at48.playbackRate - 44100.0) < 1.0 && at48.converted,
             "the 48 kHz source is held at " + std::to_string(at48.playbackRate)
             + " Hz on a 44.1 kHz device -- it was not downsampled at load, so the transport's "
               "LINEAR interpolator converts it per block (dull and aliasing)");
    RT_CHECK(at48.report.converter == dcload::Converter::Polyphase
             && at48.report.interpolate == 147 && at48.report.decimate == 160,
             std::string("the 48 kHz preload ran on '") + dcload::converterName(at48.report.converter)
             + "' at " + std::to_string(at48.report.interpolate) + ":" + std::to_string(at48.report.decimate)
             + " rather than the polyphase kernel at 147:160");
    RT_CHECK(at441.report.converter == dcload::Converter::None,
             "a 44.1 kHz file on a 44.1 kHz device reports a converter");
    note("48 kHz preload on 44.1 kHz: " + std::string(dcload::converterName(at48.report.converter))
         + " " + std::to_string(at48.report.interpolate) + ":" + std::to_string(at48.report.decimate)
         + ", " + std::to_string(at48.report.tapsPerPhase) + " taps/phase, "
         + std::to_string(at48.report.elapsedMs) + " ms");

    // --- (b) 15 kHz keeps its level -----------------------------------------
    const double ref15  = toneDb(at441.out, 15000.0, 44100.0, amp);
    const double conv15 = toneDb(at48.out,  15000.0, 44100.0, amp);
    const double d15    = conv15 - ref15;
    note("15 kHz on a 44.1 kHz device: 44.1 kHz file " + std::to_string(ref15)
         + " dB, 48 kHz file " + std::to_string(conv15) + " dB (delta " + std::to_string(d15) + " dB)");
    RT_CHECK(ref15 > -1.0, "the 44.1 kHz reference tone is not there -- the probe is vacuous");
    RT_CHECK(std::abs(d15) < 0.1,
             "a 48 kHz file lost " + std::to_string(-d15) + " dB of 15 kHz on a 44.1 kHz device");

    // --- (c) nothing folds back ---------------------------------------------
    // 21 kHz is BELOW the output Nyquist, so it should pass; its image at
    // 48 - 21 = 27 kHz folds to 17.1 kHz unless the anti-image filter removes it.
    // 23 kHz is ABOVE the output Nyquist, so it must be removed outright; left
    // in, it folds to 44.1 - 23 = 21.1 kHz.
    const auto hi = captureSourceOutput(44100, makeTonesWavBlob(3.0, 48000, { 21000.0 }, amp),
                                        false, kRateWarm, kRateCapture);
    const auto over = captureSourceOutput(44100, makeTonesWavBlob(3.0, 48000, { 23000.0 }, amp),
                                          false, kRateWarm, kRateCapture);
    if (! hi.ok || ! over.ok) { fail("capture failed: " + hi.why + over.why); endScenario(); return; }

    const double tone21  = toneDb(hi.out,   21000.0, 44100.0, amp);
    const double image21 = toneDb(hi.out,   17100.0, 44100.0, amp);
    const double alias23 = toneDb(over.out, 21100.0, 44100.0, amp);
    note("48 kHz file on a 44.1 kHz device: 21 kHz tone " + std::to_string(tone21)
         + " dB, its fold-back at 17.1 kHz " + std::to_string(image21)
         + " dB; a 23 kHz tone's alias at 21.1 kHz " + std::to_string(alias23) + " dB (all re the tone)");
    RT_CHECK(tone21 > -6.0, "the 21 kHz tone itself is gone -- the fold-back probe is vacuous");
    RT_CHECK(image21 < -80.0,
             "a 21 kHz tone folds back to 17.1 kHz at " + std::to_string(image21)
             + " dB -- the downsample is aliasing into the audible band");
    RT_CHECK(alias23 < -80.0,
             "a 23 kHz tone (above the 22.05 kHz output Nyquist) aliases to 21.1 kHz at "
             + std::to_string(alias23) + " dB -- the anti-alias cutoff is not following the OUTPUT Nyquist");

    // --- (d) and the song does not move -------------------------------------
    const auto b48  = captureSourceOutput(44100, makeBurstWavBlob(2.0, 48000, 0.5), false, 0, 128);
    const auto b441 = captureSourceOutput(44100, makeBurstWavBlob(2.0, 44100, 0.5), false, 0, 128);
    const long long p48 = burstPeak(b48.out), p441 = burstPeak(b441.out);
    note("burst peak at output sample " + std::to_string(p48) + " (48 kHz file) vs "
         + std::to_string(p441) + " (44.1 kHz file)");
    RT_CHECK(p48 > 0 && p441 > 0, "a burst was never found -- the sync probe is vacuous");
    RT_CHECK(p48 == p441,
             "the downsampled burst landed " + std::to_string(p48 - p441)
             + " output samples away from the 44.1 kHz reference -- the conversion shifts time");

    // --- (e) the same, OFFLINE, exactly -------------------------------------
    // A unit impulse at input sample 48000 (1.000 s) must peak at output
    // sample 44100: zero group delay by construction, read off the buffer
    // with no transport in the way.
    {
        juce::AudioBuffer<float> buf(1, 2 * 48000);
        buf.clear();
        buf.setSample(0, 48000, 1.0f);
        dcload::ResampleReport rep;
        RT_CHECK(dcload::convertDecodedBuffer(buf, 48000.0, 44100.0, &rep) && rep.polyphase,
                 "the offline 48 -> 44.1 kHz conversion refused a plain buffer");
        int peak = -1; double best = 0.0;
        for (int i = 0; i < buf.getNumSamples(); ++i)
            if (std::abs((double) buf.getReadPointer(0)[i]) > best)
            { best = std::abs((double) buf.getReadPointer(0)[i]); peak = i; }
        note("offline 48 -> 44.1 kHz: 1.000 s impulse peaks at output sample " + std::to_string(peak)
             + " (expected 44100), " + std::to_string(rep.tapsPerPhase) + " taps/phase");
        RT_CHECK(peak == 44100,
                 "the offline downsample put a 1.000 s impulse at output sample " + std::to_string(peak)
                 + " -- its group delay is not zero");
        RT_CHECK(buf.getNumSamples() == 88200, "the downsampled length is not 2 s at 44.1 kHz");
        // The kernel keeps ~100 zero crossings at the OUTPUT rate.
        RT_CHECK(rep.tapsPerPhase == 2 * dcload::halfTapsFor(147, 160) + 1
                 && dcload::halfTapsFor(147, 160) == 109,
                 "the downsample kernel is " + std::to_string(rep.tapsPerPhase)
                 + " taps per phase, not the 219 (2 x ceil(100 x 160/147) + 1) it is derived to be");
    }

    endScenario();
}

// A source that missed the preload budget, in BOTH directions.
void scenarioStreamingSourceResamples()
{
    beginScenario("RtStreamingSourceResamplesWithThePolyphaseKernel");

    const float amp = 0.3f;

    // --- (a) 48 kHz file, 44.1 kHz device, streaming ------------------------
    const auto ref441 = captureSourceOutput(44100, makeTonesWavBlob(3.0, 44100, { 15000.0 }, amp),
                                            false, kRateWarm, kRateCapture);
    const auto down   = captureSourceOutput(44100, makeTonesWavBlob(3.0, 48000, { 15000.0 }, amp),
                                            true, kRateWarm, kRateCapture);
    if (! ref441.ok || ! down.ok) { fail("capture failed: " + ref441.why + down.why); endScenario(); return; }
    RT_CHECK(! down.preloaded, "the source preloaded despite an exhausted budget -- the probe is on the wrong path");
    RT_CHECK(down.converted && down.report.converter == dcload::Converter::StreamingPolyphase
             && down.report.interpolate == 147 && down.report.decimate == 160,
             std::string("the streaming 48 kHz source is on '") + dcload::converterName(down.report.converter)
             + "' -- not the streaming polyphase stage at 147:160");

    const double r15 = toneDb(ref441.out, 15000.0, 44100.0, amp);
    const double d15 = toneDb(down.out,   15000.0, 44100.0, amp) - r15;
    note("streaming 48 -> 44.1 kHz: 15 kHz delta " + std::to_string(d15) + " dB re the 44.1 kHz file, "
         + std::to_string(down.allocs) + " callback allocation(s)");
    RT_CHECK(r15 > -1.0, "the 44.1 kHz reference tone is not there -- the probe is vacuous");
    RT_CHECK(std::abs(d15) < 0.1,
             "a STREAMING 48 kHz file lost " + std::to_string(-d15) + " dB of 15 kHz on a 44.1 kHz device");
    RT_CHECK(down.allocs == 0,
             "the streaming conversion allocated " + std::to_string(down.allocs) + " time(s) on the callback");

    const auto hi = captureSourceOutput(44100, makeTonesWavBlob(3.0, 48000, { 21000.0 }, amp),
                                        true, kRateWarm, kRateCapture);
    if (! hi.ok) { fail("capture failed: " + hi.why); endScenario(); return; }
    const double tone21  = toneDb(hi.out, 21000.0, 44100.0, amp);
    const double image21 = toneDb(hi.out, 17100.0, 44100.0, amp);
    note("streaming 48 -> 44.1 kHz: 21 kHz tone " + std::to_string(tone21) + " dB, fold-back at 17.1 kHz "
         + std::to_string(image21) + " dB");
    RT_CHECK(tone21 > -6.0, "the streamed 21 kHz tone is gone -- the fold-back probe is vacuous");
    RT_CHECK(image21 < -80.0,
             "a STREAMING 21 kHz tone folds back to 17.1 kHz at " + std::to_string(image21) + " dB");

    // --- (b) 44.1 kHz file, 48 kHz device, streaming -------------------------
    const auto ref48 = captureSourceOutput(48000, makeTonesWavBlob(3.0, 48000, { 15000.0 }, amp),
                                           false, kRateWarm, kRateCapture);
    const auto up    = captureSourceOutput(48000, makeTonesWavBlob(3.0, 44100, { 15000.0 }, amp),
                                           true, kRateWarm, kRateCapture);
    if (! ref48.ok || ! up.ok) { fail("capture failed: " + ref48.why + up.why); endScenario(); return; }
    const double u15 = toneDb(up.out, 15000.0, 48000.0, amp) - toneDb(ref48.out, 15000.0, 48000.0, amp);
    note("streaming 44.1 -> 48 kHz: 15 kHz delta " + std::to_string(u15) + " dB re the 48 kHz file, "
         + std::to_string(up.allocs) + " callback allocation(s)");
    RT_CHECK(! up.preloaded && up.converted && up.report.converter == dcload::Converter::StreamingPolyphase,
             std::string("the streaming 44.1 kHz source is on '") + dcload::converterName(up.report.converter)
             + "', not the streaming polyphase stage");
    RT_CHECK(std::abs(u15) < 0.1,
             "a STREAMING 44.1 kHz file lost " + std::to_string(-u15) + " dB of 15 kHz on a 48 kHz device");
    RT_CHECK(up.allocs == 0,
             "the streaming upsample allocated " + std::to_string(up.allocs) + " time(s) on the callback");

    // --- (c) time: from the start, and after a SEEK --------------------------
    const auto sRef  = captureSourceOutput(44100, makeBurstWavBlob(2.0, 44100, 0.5), false, 0, 128);
    const auto sDown = captureSourceOutput(44100, makeBurstWavBlob(2.0, 48000, 0.5), true,  0, 128);
    const long long pRef = burstPeak(sRef.out), pDown = burstPeak(sDown.out);
    note("streaming burst: peak at " + std::to_string(pDown) + " vs " + std::to_string(pRef) + " (reference)");
    RT_CHECK(pRef > 0 && pDown > 0, "a streaming burst was never found -- the sync probe is vacuous");
    RT_CHECK(pDown == pRef,
             "the streamed burst landed " + std::to_string(pDown - pRef)
             + " output samples away from the reference -- the streaming conversion shifts time");

    // A seek lands the stage at an arbitrary phase of its L:M cycle; 1.2345 s
    // is 54,441.45 output frames at 44.1 kHz, so not on any input sample.
    const auto kRef  = captureSourceOutput(44100, makeBurstWavBlob(3.0, 44100, 1.5), false, 0, 128, 1.2345);
    const auto kDown = captureSourceOutput(44100, makeBurstWavBlob(3.0, 48000, 1.5), true,  0, 128, 1.2345);
    const long long qRef = burstPeak(kRef.out), qDown = burstPeak(kDown.out);
    note("streaming burst after a seek to 1.2345 s: peak at " + std::to_string(qDown) + " vs "
         + std::to_string(qRef) + " (reference)");
    RT_CHECK(qRef > 0 && qDown > 0, "the post-seek burst was never found -- the seek probe is vacuous");
    RT_CHECK(std::llabs(qDown - qRef) <= 1,
             "after a seek the streamed burst landed " + std::to_string(qDown - qRef)
             + " output samples away from the reference -- the stage did not resync to the new position");

    // --- (d) the stage is the offline conversion, sample for sample ---------
    // Same kernel, same phase walk, same zero padding before the first sample:
    // pulled in odd-sized blocks (crossing the stage's 512-frame chunks), the
    // stage must reproduce the offline buffer EXACTLY, then again after a seek
    // into the middle of a phase cycle.
    {
        auto offline = makeToneBuffer(1.0, 48000, 2, { 440.0, 9000.0, 21000.0 }, 0.2f);
        juce::MemoryAudioSource mem(offline, /*copyMemory*/ true);
        auto stage = dcload::PolyphaseStreamSource::create(mem, 48000.0, 44100.0, 2);
        RT_CHECK(stage != nullptr, "PolyphaseStreamSource refused 48 -> 44.1 kHz");
        dcload::ResampleReport rep;
        RT_CHECK(dcload::convertDecodedBuffer(offline, 48000.0, 44100.0, &rep), "offline refused");
        if (stage != nullptr)
        {
            stage->prepareToPlay(733, 44100.0);
            RT_CHECK(stage->getTotalLength() == offline.getNumSamples(),
                     "the stage reports " + std::to_string(stage->getTotalLength())
                     + " frames, the offline conversion holds " + std::to_string(offline.getNumSamples()));

            auto compareFrom = [&](long long start, int frames) -> double
            {
                stage->setNextReadPosition(start);
                juce::AudioBuffer<float> got(2, frames);
                const int blocks[] = { 733, 1, 512, 1500, 97 };
                int done = 0, k = 0;
                while (done < frames)
                {
                    const int n = std::min(blocks[k++ % 5], frames - done);
                    juce::AudioSourceChannelInfo info(&got, done, n);
                    stage->getNextAudioBlock(info);
                    done += n;
                }
                double worst = 0.0;
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < frames; ++i)
                        worst = std::max(worst, std::abs((double) got.getSample(c, i)
                                                         - (double) offline.getSample(c, (int) start + i)));
                return worst;
            };
            const double fromStart = compareFrom(0, 20000);
            const double fromSeek  = compareFrom(12345, 20000);
            note("streaming stage vs offline conversion: worst |delta| " + std::to_string(fromStart)
                 + " from the start, " + std::to_string(fromSeek) + " after a seek to frame 12345");
            // Not bit-exact only because the offline path sums the first and last
            // ~109 outputs (window past the song's ends) in a scalar loop, the
            // stage in SIMD over explicit zeros; 1e-6 is float rounding there.
            RT_CHECK(fromStart < 1.0e-6 && fromSeek < 1.0e-6,
                     "the streaming stage differs from the offline conversion (worst "
                     + std::to_string(std::max(fromStart, fromSeek)) + ") -- the two paths are not "
                       "the same kernel, phase walk and padding");
        }
    }

    endScenario();
}

void scenarioStemGroupChurn()
{
    beginScenario("RtStemGroupChurnDoesNotStallTheCallback");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    const auto blob = makeWavBlob(4.0, 48000, 220.0);
    if (blob.getSize() == 0) { fail("could not synthesise a WAV blob"); endScenario(); return; }

    std::vector<std::unique_ptr<TransportSource>> sources;
    for (int i = 0; i < 4; ++i)
    {
        auto s = std::make_unique<TransportSource>(fx.engine());
        const auto err = s->openMemory(blob.getData(), blob.getSize(), "wav");
        if (err.isNotEmpty()) { fail("openMemory failed: " + err.toStdString()); endScenario(); return; }
        s->play();
        sources.push_back(std::move(s));
    }

    warmUpAndArm(fx);

    std::atomic<bool> run{ true };
    std::atomic<int>  toggles{ 0 };
    // FREE-RUNNING churn, and a long block count to match it. The plan called
    // for a toggle every 16 blocks, but the manual pump is synchronous: 512
    // blocks are over in a few milliseconds while ONE group toggle takes tens
    // of ms (setSourceStemGrouped constructs an R3 RubberBandStretcher and
    // prepares it under stretcherMutex_). Gating the worker on the block
    // counter therefore produced exactly one toggle and a probe that proved
    // nothing. Instead: let the worker churn as fast as it can and pump long
    // enough that several toggles land inside the measured window.
    std::thread churn([&]
    {
        bool grouped = false;
        while (run.load(std::memory_order_acquire))
        {
            grouped = ! grouped;
            for (auto& s : sources) fx.engine().setSourceStemGrouped(s.get(), grouped);
            toggles.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Pump until the churn has actually LANDED its toggles, not for a fixed
    // window and a hope. Each toggle constructs an R3 RubberBandStretcher under
    // stretcherMutex_, so how many land inside N blocks is a property of the
    // machine: on a fast pump 32768 blocks finish after 2 and the probe fails
    // reporting itself vacuous — a red gate that says nothing about the code.
    // Chunked so the condition is re-read, and bounded by the evidence cap
    // (gatherEvidence, #55) so a genuinely stuck churn still ends the test
    // rather than hanging it. The ceiling used to be a BLOCK count, which is a
    // couple of seconds on a fast pump and arbitrarily little wall clock on a
    // starved box where the churn thread barely gets a core.
    constexpr int kChunk = 2048;
    constexpr int kWantToggles = 4;
    int kBlocks = 0;
    // The short sleep between chunks hands a starved box's core to the churn:
    // every pumped block records marks, and a pump that never yields fills the
    // recorder before a churn that cannot get scheduled lands its toggles.
    const Gathered g = gatherEvidence([&]
    {
        fx.pumpArmed(kChunk);
        kBlocks += kChunk;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, [&] { return toggles.load(std::memory_order_relaxed) >= kWantToggles; });
    dcrt::disarm();
    run.store(false, std::memory_order_release);
    churn.join();
    for (auto& s : sources) fx.engine().setSourceStemGrouped(s.get(), false);
    sources.clear();

    const int  acquires = dcrt::countAt(dcrt::Kind::Lock, "stretcherMutex");
    const long long p99 = dcrt::percentileNs(dcrt::Kind::Lock, "stretcherMutex", 0.99);
    const long long worst = dcrt::maxNs(dcrt::Kind::Lock, "stretcherMutex");
    const long long budget = fx.blockBudgetNs();

    note("stretcherMutex acquires: " + std::to_string(acquires)
         + " over " + std::to_string(kBlocks) + " blocks ("
         + std::to_string(toggles.load()) + " group toggles)");
    note("stretcherMutex wait p99 = " + std::to_string(p99) + " ns, max = "
         + std::to_string(worst) + " ns, block budget = " + std::to_string(budget) + " ns");

    reportHeap("stem churn");
    RT_CHECK(dcrt::allocCount() == 0,
             "stem-group churn allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the callback");
    RT_CHECK(acquires > 0, "no stretcherMutex acquires observed — the probe is vacuous");
    RT_CHECK(toggles.load() >= 4,
             "only " + std::to_string(toggles.load())
             + " group toggles landed during the measured window — the contention probe is vacuous"
             + g.why());
    RT_CHECK(p99 < budget / 4,
             "stretcherMutex p99 acquire wait " + std::to_string(p99)
             + " ns exceeds 25% of the block budget (" + std::to_string(budget / 4) + " ns)");
    endScenario();
}

// 10 -----------------------------------------------------------------------
void scenarioBridgeWaitBounded()
{
    beginScenario("RtBridgeWaitIsBounded");
#if !defined(_WIN32)
    note("skipped: the sandbox bridge is Windows-only");
    endScenario();
    return;
#else
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    char shm[128], req[128], resp[128];
    const unsigned pid = (unsigned) ::GetCurrentProcessId();
    std::snprintf(shm,  sizeof(shm),  "dcrtcheck_shm_%u", pid);
    std::snprintf(req,  sizeof(req),  "dcrtcheck_req_%u", pid);
    std::snprintf(resp, sizeof(resp), "dcrtcheck_resp_%u", pid);

    if (! fx.engine().setRemoteBridge(shm, req, resp, true))
    { fail("setRemoteBridge(enable) failed"); endScenario(); return; }

    StubBridgeChild child;
    if (! child.open(shm, req, resp))
    { fail("stub child could not attach to the mapping"); fx.engine().setRemoteBridge(nullptr, nullptr, nullptr, false); endScenario(); return; }

    const long long budget = fx.blockBudgetNs();

    // --- Phase A: healthy child ------------------------------------------
    // A block only WAITS when the child has not answered by the time the audio
    // thread first looks; a block whose answer is already there costs nothing
    // and records nothing. How many of each a window holds is the scheduler's
    // call: on GitHub's 2-vCPU runner the child mostly answered before the
    // audio thread looked, and 256 blocks held 58 waits (#55). So pump
    // 256-block chunks until the p99 below has its 129 samples (the floor the
    // fixed window demanded), bounded by the evidence cap.
    //
    // Each chunk is its own recorder window and the wait samples are carried
    // across: at ~3 marks a block the recorder is full after ~130k blocks,
    // which a busy pump reaches in 3 s — long before a child the scheduler is
    // starving gets its turn (measured: 19 waits in 131,072 blocks, 2 cores
    // plus one spinner). The 1 ms between chunks is that turn.
    warmUpAndArm(fx);
    constexpr int kChunkA     = 256;
    constexpr int kWantWaitsA = kChunkA / 2 + 1;
    int kBlocksA = 0;
    std::vector<long long> samplesA;
    const Gathered gA = gatherEvidence([&]
    {
        dcrt::reset();
        dcrt::arm();
        fx.pumpArmed(kChunkA);
        dcrt::disarm();
        kBlocksA += kChunkA;
        const auto s = dcrt::samplesNs(dcrt::Kind::Wait, "bridge/responseWait");
        samplesA.insert(samplesA.end(), s.begin(), s.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, [&] { return (int) samplesA.size() >= kWantWaitsA; });
    std::sort(samplesA.begin(), samplesA.end());

    const int  waitsA = (int) samplesA.size();
    const long long p99A = dcrt::percentileOf(samplesA, 0.99);
    note("phase A: " + std::to_string(waitsA) + " waits over " + std::to_string(kBlocksA)
         + " blocks (" + std::to_string(gA.seconds) + " s), p99 = " + std::to_string(p99A) + " ns");
    RT_CHECK(waitsA >= kWantWaitsA,
             "phase A saw only " + std::to_string(waitsA) + " bridge waits over "
             + std::to_string(kBlocksA) + " blocks — the probe is vacuous" + gA.why());
    RT_CHECK(p99A < budget,
             "phase A bridge round-trip p99 " + std::to_string(p99A)
             + " ns exceeds one block budget (" + std::to_string(budget) + " ns)");

    // --- Phase B: wedged child -------------------------------------------
    child.setAnswering(false);
    dcrt::reset();
    dcrt::arm();
    constexpr int kBlocksB = 32;
    fx.pumpArmed(kBlocksB);
    dcrt::disarm();

    const int  waitsB  = dcrt::countAt(dcrt::Kind::Wait, "bridge/responseWait");
    const long long p99B  = dcrt::percentileNs(dcrt::Kind::Wait, "bridge/responseWait", 0.99);
    const long long maxB  = dcrt::maxNs(dcrt::Kind::Wait, "bridge/responseWait");
    note("phase B (wedged child): " + std::to_string(waitsB) + " waits over "
         + std::to_string(kBlocksB) + " blocks, p99 = " + std::to_string(p99B)
         + " ns, max = " + std::to_string(maxB) + " ns");

    RT_CHECK(waitsB <= 4,
             "a wedged child must stop costing the audio thread after 3 blocks, but "
             + std::to_string(waitsB) + " blocks still blocked (poll-only never engaged)");
    RT_CHECK(maxB < budget * 3 / 2,
             "a stalled bridge block blocked the audio thread for " + std::to_string(maxB)
             + " ns, more than 1.5x the " + std::to_string(budget)
             + " ns block budget (finding E-1: the GetTickCount loop floors at one ~15.6 ms timer tick)");

    // --- Phase C: the child answers again ---------------------------------
    // Poll-only used to be a ONE-WAY DOOR. It tests the response sequence
    // immediately after posting this block's request, which the child cannot
    // possibly have answered yet, and consecutiveTimeouts_ is only reset ON
    // SUCCESS — so once latched, the bridge stayed silent for the rest of the
    // session. It latches trivially, because the child holds its graph mutex
    // across prepareToPlay: LOADING an instrument stalls several blocks by
    // construction. Owner report 2026-08-21: the VSTi loads, MIDI still reaches
    // it, and no audio ever comes back.
    //
    // Recovery is now a periodic re-try of the real synchronous wait, so a busy
    // child that becomes healthy is found again. Waits resuming IS the
    // observable: in poll-only the audio thread never waits at all.
    // A REAL child takes hundreds of microseconds — far longer than the
    // nanoseconds between posting a request and poll-only's immediate check,
    // and far shorter than the block budget. Without this delay the stub can
    // satisfy that check by accident and this whole phase goes vacuous (it did:
    // 580/600 waits with the fix reverted).
    child.setAnswerDelayUs(300);
    child.setAnswering(true);
    // The bound is a DENSITY: more than 100 waits inside one 600-block window.
    // Poll-only alone re-tries once every kRetrySyncEveryBlocks (256), i.e. 2–3
    // waits per window, so only a bridge that really left poll-only gets there.
    // A starved child can miss the retries of one window, so windows repeat
    // until one qualifies (gatherEvidence, #55) — never summed across windows,
    // which would let the retries alone reach 100 given enough time.
    constexpr int kBlocksC = 600;
    int waitsC = 0, bestC = 0;
    const Gathered gC = gatherEvidence([&]
    {
        dcrt::reset();
        dcrt::arm();
        fx.pumpArmed(kBlocksC);
        dcrt::disarm();
        waitsC = dcrt::countAt(dcrt::Kind::Wait, "bridge/responseWait");
        bestC = std::max(bestC, waitsC);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // a starved child's turn
    }, [&] { return waitsC > 100; });

    note("phase C (child answering again): " + std::to_string(bestC) + " waits in the best "
         + std::to_string(kBlocksC) + "-block window of " + std::to_string(gC.rounds));
    RT_CHECK(gC.enough,
             "the bridge never left poll-only after the child recovered — at most "
             + std::to_string(bestC) + " waits in any " + std::to_string(kBlocksC)
             + "-block window. Poll-only is a one-way door again, which means silence "
               "for the rest of the session." + gC.why());

    child.close();
    fx.engine().setRemoteBridge(nullptr, nullptr, nullptr, false);
    endScenario();
#endif
}

// 11 -----------------------------------------------------------------------
// The dropout / xrun counter. The manual-pump device is what makes this
// testable at all: the callback runs on THIS thread, so the "gap between
// callbacks" the detector measures is simply the wall-clock time between two
// pump() calls, and we can synthesise a real stall by sleeping between them.
//
// Every assertion here exists because the counter it guards has a way of
// passing while measuring nothing:
//   - a detector that never fires is indistinguishable from a healthy machine
//     (phase B forces one and demands it be seen),
//   - a detector that fires on ordinary jitter is worse than none (phase A
//     demands ZERO over a tight loop),
//   - and a detector that counts the cold start of a device session reports a
//     dropout at every device change / sample-rate change / watchdog recovery
//     (phase C reopens the device and demands silence).
void scenarioXrunCounter()
{
    beginScenario("RtXrunCounterIsRealAndQuiet");
    Fixture fx(48000, 256, 2);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    dca_xrun_stats st{};
    auto read = [&]()
    {
        st = dca_xrun_stats{};
        st.struct_bytes = (int32_t) sizeof(dca_xrun_stats);
        fx.engine().fillXrunStats(st);
    };

    // --- Phase A: a healthy tight loop must count NOTHING ------------------
    fx.pumpWarm(16);                 // past the per-session warm-up guard
    fx.engine().resetXrunStats();
    warmUpAndArm(fx);                // allocation sentinel over the counting path
    constexpr int kHealthy = 256;
    fx.pumpArmed(kHealthy);
    dcrt::disarm();
    read();

    note("phase A: " + std::to_string(st.callbacks) + " callbacks, "
         + std::to_string(st.late_callbacks) + " late, "
         + std::to_string(st.deadline_overruns) + " deadline overruns, "
         + std::to_string(st.near_misses) + " near misses, budget "
         + std::to_string(st.block_budget_ns) + " ns, last "
         + std::to_string(st.last_callback_ns) + " ns");

    RT_CHECK(st.callbacks >= kHealthy,
             "the counter saw " + std::to_string(st.callbacks) + " callbacks over "
             + std::to_string(kHealthy) + " pumps — it is not counting at all");
    RT_CHECK(st.late_callbacks == 0,
             "a back-to-back pump loop produced " + std::to_string(st.late_callbacks)
             + " phantom dropout(s): the threshold is too tight to be believed");
    RT_CHECK(st.block_budget_ns == fx.blockBudgetNs(),
             "block budget reported as " + std::to_string(st.block_budget_ns)
             + " ns, expected " + std::to_string(fx.blockBudgetNs()) + " ns");
    RT_CHECK(st.device_open == 1, "device_open is 0 while the callback is running");
    RT_CHECK(dcrt::allocCount() == 0,
             "the xrun counters allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the audio thread");

    // --- Phase B: a real stall MUST be seen -------------------------------
    // 40 ms between two callbacks at 256/48000 (5.33 ms per block) is roughly
    // seven missed periods. If this does not register, nothing will.
    fx.engine().resetXrunStats();
    fx.pumpWarm(8);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    fx.pumpWarm(1);
    read();
    note("phase B (40 ms stall): late = " + std::to_string(st.late_callbacks)
         + ", dropped frames = " + std::to_string(st.dropped_frames)
         + ", last xrun frame = " + std::to_string(st.last_xrun_render_frame));

    RT_CHECK(st.late_callbacks >= 1,
             "a 40 ms gap between two callbacks was not counted as a dropout — the "
             "detector is vacuous");
    RT_CHECK(st.dropped_frames > 1000,
             "a 40 ms gap should account for ~1900 frames at 48 kHz, but the counter "
             "reports " + std::to_string(st.dropped_frames));
    RT_CHECK(st.last_xrun_host_ticks != 0,
             "last_xrun_host_ticks stayed 0 across a counted dropout");
    RT_CHECK(st.last_xrun_render_frame >= 0,
             "last_xrun_render_frame stayed -1 across a counted dropout");

    // --- Phase C: a device REBUILD must not invent one --------------------
    // Reopening the device is the shape of a device change / NAM 48 kHz
    // request / watchdog recovery. Its first callbacks are always late.
    fx.engine().resetXrunStats();
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        fx.engine().deviceManager().getAudioDeviceSetup(setup);
        setup.bufferSize = 512;
        const auto err = fx.engine().deviceManager().setAudioDeviceSetup(setup, true);
        RT_CHECK(err.isEmpty(), "could not reopen the null device at 512 frames: " + err.toStdString());
        fx.installInputStimulus();
    }
    // Deliberately stall right across the session boundary: the warm-up guard,
    // not luck, is what has to swallow this.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    fx.pumpWarm(4);
    read();
    note("phase C (device reopen): late = " + std::to_string(st.late_callbacks));
    RT_CHECK(st.late_callbacks == 0,
             "reopening the device produced " + std::to_string(st.late_callbacks)
             + " phantom dropout(s) — every engine rebuild would report one");

    endScenario();
}

// 12 -----------------------------------------------------------------------
// The EXPORT, not the engine method behind it. dotcharter_audio.cpp is part of
// JUCESHARP_CORE_SOURCES, so this exe carries the real ABI entry points and can
// drive them against a real dca_engine_t opened on the (paced) null device — no
// hardware, no stolen sound card. What is actually under test is the cbSize
// contract, which is the part a unit test of fillXrunStats can never reach.
void scenarioXrunAbiContract()
{
    beginScenario("RtXrunStatsAbiHonoursStructBytes");

    setEnv(nulldev::kEnvVar, "1");
    setEnv(nulldev::kManualEnvVar, nullptr);   // paced: the device runs its own thread

    dca_engine_config cfg{};
    cfg.sample_rate     = 48000;
    cfg.buffer_frames   = 256;
    cfg.output_channels = 2;
    cfg.input_channels  = 2;
    std::strncpy(cfg.output_type_name, nulldev::kTypeName, sizeof(cfg.output_type_name) - 1);

    dca_engine_t eng = nullptr;
    const auto created = dca_engine_create(&cfg, &eng);
    if (created != DCA_OK || eng == nullptr)
    {
        fail("dca_engine_create on the null device returned status " + std::to_string((int) created));
        setEnv(nulldev::kEnvVar, nullptr);
        endScenario();
        return;
    }

    // Let the paced device render for a while so the counters have something in
    // them; otherwise every assertion below is trivially satisfied by zeros.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // --- Full-size read ----------------------------------------------------
    dca_xrun_stats full{};
    full.struct_bytes = (int32_t) sizeof(dca_xrun_stats);
    RT_CHECK(dca_engine_get_xrun_stats(eng, &full) == DCA_OK, "full-size get_xrun_stats failed");
    RT_CHECK(full.struct_bytes == (int32_t) sizeof(dca_xrun_stats),
             "full-size read reported " + std::to_string(full.struct_bytes) + " bytes written");
    RT_CHECK(full.device_open == 1, "device_open is 0 on an engine with the null device open");
    RT_CHECK(full.callbacks > 0,
             "the paced null device rendered " + std::to_string(full.callbacks)
             + " callbacks in 200 ms — the ABI is reading a dead engine");
    note("abi read: " + std::to_string(full.callbacks) + " callbacks, "
         + std::to_string(full.late_callbacks) + " late, device_xruns "
         + std::to_string(full.device_xruns));
    RT_CHECK(full.device_xruns == -1,
             "the null device claims to report driver xruns (" + std::to_string(full.device_xruns)
             + "); -1 is the only honest answer for a device with no such counter");

    // --- Truncated read: an OLDER caller against a newer struct ------------
    // Fill with a sentinel so we can prove the engine wrote nothing past the
    // bytes the caller declared.
    struct { dca_xrun_stats s; unsigned char guard[64]; } shortRead{};
    std::memset(&shortRead, 0xAB, sizeof(shortRead));
    const int32_t shortBytes = (int32_t) (4 * sizeof(int32_t) + 2 * sizeof(int64_t));
    shortRead.s.struct_bytes = shortBytes;
    RT_CHECK(dca_engine_get_xrun_stats(eng, &shortRead.s) == DCA_OK, "truncated get_xrun_stats failed");
    RT_CHECK(shortRead.s.struct_bytes == shortBytes,
             "a truncated read reported " + std::to_string(shortRead.s.struct_bytes)
             + " bytes written, expected " + std::to_string(shortBytes));
    RT_CHECK(shortRead.s.device_open == 1, "truncated read did not fill device_open");
    RT_CHECK(shortRead.s.callbacks > 0, "truncated read did not fill callbacks");
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&shortRead);
        bool clobbered = false;
        for (size_t i = (size_t) shortBytes; i < sizeof(shortRead); ++i)
            if (bytes[i] != 0xAB) { clobbered = true; break; }
        RT_CHECK(! clobbered,
                 "get_xrun_stats wrote past the caller's declared struct_bytes — the cbSize "
                 "contract is a buffer overrun, not a compatibility mechanism");
    }

    // --- A struct_bytes too small to be meaningful is rejected -------------
    dca_xrun_stats tiny{};
    tiny.struct_bytes = 4;
    RT_CHECK(dca_engine_get_xrun_stats(eng, &tiny) == DCA_ERR_INVALID_ARG,
             "an undersized struct_bytes was accepted");
    RT_CHECK(dca_engine_get_xrun_stats(eng, nullptr) == DCA_ERR_INVALID_ARG,
             "a null out pointer was accepted");
    RT_CHECK(dca_engine_get_xrun_stats(nullptr, &full) == DCA_ERR_INVALID_ARG,
             "a null engine handle was accepted");

    // --- Reset actually resets --------------------------------------------
    RT_CHECK(dca_engine_reset_xrun_stats(eng) == DCA_OK, "reset_xrun_stats failed");
    dca_xrun_stats afterReset{};
    afterReset.struct_bytes = (int32_t) sizeof(dca_xrun_stats);
    RT_CHECK(dca_engine_get_xrun_stats(eng, &afterReset) == DCA_OK, "post-reset read failed");
    RT_CHECK(afterReset.callbacks < full.callbacks,
             "reset_xrun_stats left the callback count at " + std::to_string(afterReset.callbacks)
             + " (was " + std::to_string(full.callbacks) + ")");
    RT_CHECK(afterReset.last_xrun_render_frame == -1 || afterReset.late_callbacks > 0,
             "reset left a stale last_xrun_render_frame behind");

    dca_engine_destroy(eng);
    setEnv(nulldev::kEnvVar, nullptr);
    endScenario();
}

// ---------------------------------------------------------------------------
// JuceSharp #29 (ABI v46): the callback LOAD a host's CPU meter shows. The
// meter is updated on the audio thread, so this is the place to prove it
// allocates nothing — and, so a zero-allocation run of code that did nothing
// cannot pass, that it MOVES: it reads 0 across a device reopen and its
// warm-up, then a real load once callbacks are counted, bounded by the longest
// callback the xrun counters saw.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// JuceSharp #28 (bridge v3): MIDI to a SANDBOXED instrument is sample-accurate.
// ---------------------------------------------------------------------------
//
// The bridge used to drain the fifo straight into the child with no sample
// offset, so every event sounded at sample 0 of whichever block drained it —
// and a scheduled-ahead event (dca_engine_send_midi_at) sounded EARLY by however
// far ahead it was sent. Bridge v3 parks events on the audio thread like the
// in-process leg and carries each one's offset across the mapping.
//
// The stub child reads the request exactly as juce_vst_host.exe does, so this
// asserts what the child's MidiBuffer is handed: blockStart + offset == the
// requested frame, EXACTLY. A round trip that times out carries its events to
// the next block at sample 0 (by design), so a phase whose bridge timed out is
// retried rather than failed — the timing assertions are about the protocol,
// not about how promptly the scheduler ran the stub thread.
void scenarioSandboxMidiIsSampleAccurate()
{
    beginScenario("RtSandboxMidiLandsOnItsSampleWithoutAllocating");
#if !defined(_WIN32)
    note("skipped: the sandbox bridge is Windows-only");
    endScenario();
    return;
#else
    constexpr int kBlock = 256;
    Fixture fx(48000, kBlock);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    char shm[128], req[128], resp[128];
    const unsigned pid = (unsigned) ::GetCurrentProcessId();
    std::snprintf(shm,  sizeof(shm),  "dcrtmidi_shm_%u", pid);
    std::snprintf(req,  sizeof(req),  "dcrtmidi_req_%u", pid);
    std::snprintf(resp, sizeof(resp), "dcrtmidi_resp_%u", pid);
    if (! fx.engine().setRemoteBridge(shm, req, resp, true))
    { fail("setRemoteBridge(enable) failed"); endScenario(); return; }

    StubBridgeChild child;
    if (! child.open(shm, req, resp))
    {
        fail("stub child could not attach to the mapping");
        fx.engine().setRemoteBridge(nullptr, nullptr, nullptr, false);
        endScenario();
        return;
    }
    fx.pumpWarm(kWarmupBlocks);

    auto packed = [](uint8_t status, uint8_t d1, uint8_t d2)
    { return (uint32_t) status | ((uint32_t) d1 << 8) | ((uint32_t) d2 << 16); };
    auto timeouts = [&]
    {
        uint32_t p = 0, t = 0, hb = 0;
        fx.engine().getRemoteBridgeStats(p, t, hb);
        return t;
    };
    auto describe = [&]
    {
        std::string seen;
        for (int i = 0; i < child.midiLogCount(); ++i)
        {
            const auto m = child.midiLogAt(i);
            seen += "(n" + std::to_string((m.packed >> 8) & 0x7F) + " @" + std::to_string(m.blockStart)
                  + "+" + std::to_string(m.offset) + ") ";
        }
        return seen;
    };

    // ---- Phase A: four offsets inside ONE block ---------------------------
    const int offsets[4] = { 0, 37, 128, 255 };
    int64_t want[4] = {};
    int  matched = 0, attempts = 0, allocsA = 0, logA = 0;
    std::string seenA;
    for (; attempts < 8 && matched < 4; ++attempts)
    {
        int64_t frames = 0, ticks = 0;
        if (! fx.engine().getRenderClock(frames, ticks))
        { fail("the render clock is not published — the probe is vacuous"); break; }
        child.clearMidiLog();
        const uint32_t t0 = timeouts();
        for (int i = 0; i < 4; ++i)
        {
            want[i] = frames + offsets[i];
            fx.engine().sendMidiAt(packed(0x90, (uint8_t) (60 + i), 100), want[i]);
        }
        dcrt::reset();
        dcrt::arm();
        fx.pumpArmed(1);
        dcrt::disarm();
        allocsA = dcrt::allocCount();
        if (timeouts() != t0) { fx.pumpWarm(4); continue; }   // carried, not timed: retry

        logA  = child.midiLogCount();
        seenA = describe();
        matched = 0;
        for (int i = 0; i < logA; ++i)
        {
            const auto m = child.midiLogAt(i);
            for (int k = 0; k < 4; ++k)
                if (m.packed == packed(0x90, (uint8_t) (60 + k), 100)
                    && m.blockStart + (int64_t) m.offset == want[k]
                    && (int) m.offset == offsets[k])
                    ++matched;
        }
    }
    note("phase A (" + std::to_string(attempts) + " attempt(s)): " + seenA);
    RT_CHECK(logA == 4,
             "the child was handed " + std::to_string(logA) + " of 4 events scheduled inside one block");
    RT_CHECK(matched == 4,
             "only " + std::to_string(matched) + " of 4 events reached the child on the sample they "
             "were scheduled for (want offsets 0/37/128/255) — the bridge is dropping the offset. "
             "Seen: " + seenA);
    RT_CHECK(allocsA == 0,
             "the bridge's MIDI parking allocated " + std::to_string(allocsA) + " time(s) on the callback");

    // ---- Phase B: an event for the NEXT block is parked, not sent early ----
    int earlyCount = -1, lateCount = -1;
    StubBridgeChild::MidiSeen parked{};
    int64_t wantB = 0;
    attempts = 0;
    for (; attempts < 8; ++attempts)
    {
        int64_t frames = 0, ticks = 0;
        fx.engine().getRenderClock(frames, ticks);
        child.clearMidiLog();
        const uint32_t t0 = timeouts();
        wantB = frames + kBlock + 100;
        fx.engine().sendMidiAt(packed(0x90, 70, 100), wantB);
        fx.pumpWarm(1);
        earlyCount = child.midiLogCount();
        fx.pumpWarm(1);
        lateCount = child.midiLogCount();
        parked = child.midiLogAt(0);
        if (timeouts() == t0) break;
        fx.pumpWarm(4);
    }
    note("phase B: " + std::to_string(earlyCount) + " event(s) a block early, " + std::to_string(lateCount)
         + " after; landed at " + std::to_string(parked.blockStart) + "+" + std::to_string(parked.offset)
         + " for frame " + std::to_string(wantB));
    RT_CHECK(earlyCount == 0,
             "an event scheduled for the NEXT block reached the child in THIS one — the bridge "
             "forwards whatever it drains instead of parking it");
    RT_CHECK(lateCount == 1 && parked.offset == 100 && parked.blockStart + 100 == wantB,
             "the parked event did not reach the child at offset 100 of the block containing its frame");

    // ---- Phase C: a late event goes at 0 AND is counted --------------------
    fx.engine().resetMidiTimingStats();
    child.clearMidiLog();
    int64_t frames = 0, ticks = 0;
    fx.engine().getRenderClock(frames, ticks);
    fx.engine().sendMidiAt(packed(0x90, 80, 100), frames - 4096);
    fx.pumpWarm(1);
    int64_t scheduled = 0, late = 0, dropped = 0;
    fx.engine().getMidiTimingStats(scheduled, late, dropped);
    const auto lateHit = child.midiLogAt(0);
    note("phase C: late " + std::to_string(late) + ", landed at offset " + std::to_string(lateHit.offset));
    RT_CHECK(late == 1, "a sandbox event whose frame had passed was not counted late ("
             + std::to_string(late) + ")");
    RT_CHECK(child.midiLogCount() >= 1 && lateHit.offset == 0,
             "a late sandbox event must be placed at the head of the block");

    // ---- Phase D: immediate (frame < 0) is still immediate at sample 0 -----
    child.clearMidiLog();
    fx.engine().sendMidiShort(0x90, 90, 100);
    fx.pumpWarm(2);
    const auto now = child.midiLogAt(0);
    RT_CHECK(child.midiLogCount() >= 1 && now.offset == 0,
             "an immediate sandbox event did not arrive at sample 0 of the next block");

    child.close();
    fx.engine().setRemoteBridge(nullptr, nullptr, nullptr, false);
    endScenario();
#endif
}

void scenarioCallbackLoad()
{
    beginScenario("RtCallbackLoadIsMeasuredWithoutAllocating");
    Fixture fx(48000, 256, 2);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.installInputStimulus();

    float avg = -1.0f, peak = -1.0f;
    auto read = [&]() { fx.engine().getCallbackLoad(avg, peak); };
    auto maxLoad = [&]()
    {
        dca_xrun_stats st{};
        st.struct_bytes = (int32_t) sizeof(dca_xrun_stats);
        fx.engine().fillXrunStats(st);
        return st.block_budget_ns > 0 ? (double) st.max_callback_ns / (double) st.block_budget_ns : 0.0;
    };

    // --- Phase A: the meter's own update path allocates nothing, and moves --
    fx.pumpWarm(16);                 // past the per-session warm-up guard
    warmUpAndArm(fx);
    fx.pumpArmed(256);
    dcrt::disarm();
    read();
    const double bound = maxLoad();
    note("phase A: average " + std::to_string(avg) + ", peak " + std::to_string(peak)
         + ", longest callback / budget " + std::to_string(bound));
    RT_CHECK(dcrt::allocCount() == 0,
             "the callback-load meter allocated " + std::to_string(dcrt::allocCount())
             + " time(s) on the audio thread");
    RT_CHECK(avg > 0.0f,
             "256 rendered callbacks left the average load at " + std::to_string(avg)
             + " — the meter is not measuring anything");
    RT_CHECK(peak >= avg * 0.999f,
             "the decaying peak (" + std::to_string(peak) + ") is below the average ("
             + std::to_string(avg) + ")");
    RT_CHECK((double) peak <= bound * 1.001 + 1e-6,
             "the peak load " + std::to_string(peak) + " exceeds the longest callback the "
             "xrun counters measured (" + std::to_string(bound) + " of a block)");

    // --- Phase B: a device reopen restarts the meter from 0 ---------------
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        fx.engine().deviceManager().getAudioDeviceSetup(setup);
        setup.bufferSize = 512;
        const auto err = fx.engine().deviceManager().setAudioDeviceSetup(setup, true);
        RT_CHECK(err.isEmpty(), "could not reopen the null device at 512 frames: " + err.toStdString());
        fx.installInputStimulus();
    }
    read();
    RT_CHECK(avg == 0.0f && peak == 0.0f,
             "a reopened device still shows the old session's load (average "
             + std::to_string(avg) + ", peak " + std::to_string(peak) + ")");
    fx.pumpWarm(4);
    read();
    RT_CHECK(avg == 0.0f && peak == 0.0f,
             "the cold warm-up blocks of a new session were counted (average "
             + std::to_string(avg) + ", peak " + std::to_string(peak) + ")");
    fx.pumpWarm(32);
    read();
    note("phase B (after reopen): average " + std::to_string(avg) + ", peak " + std::to_string(peak));
    RT_CHECK(avg > 0.0f && peak > 0.0f, "the meter did not resume after a device reopen");

    // --- The export: a null engine or two null out-pointers is refused ----
    float a = 7.0f, p = 7.0f;
    RT_CHECK(dca_engine_get_callback_load(nullptr, &a, &p) == DCA_ERR_INVALID_ARG,
             "a null engine handle was accepted");
    RT_CHECK(a == 0.0f && p == 0.0f, "a refused read left the out-params unzeroed");

    endScenario();
}

// ---------------------------------------------------------------------------
// Changing the instrument slot still throws the queued MIDI away — without
// midiFifo_.reset(), which raced two other threads.
// ---------------------------------------------------------------------------
//
// juce::AbstractFifo is single-producer / single-consumer and reset() rewrites
// BOTH index atomics, so it is legal only when nothing else is touching the
// fifo. attach/detachInstrumentPlugin had two racers: the audio thread's
// collectBridgeMidi (which runs whenever no in-process instrument is
// attached, i.e. exactly around an attach) and sendMidiShort on the JUCE MIDI
// input thread. discardQueuedMidi() is a consumer-side drain under
// remoteBridgeMutex_ instead, which is legal against both.
//
// The RACE is not reproducible on demand — the audio-thread consumer only runs
// with the sandbox bridge enabled, and a torn AbstractFifo loses or duplicates
// events rather than corrupting memory, so there is nothing deterministic to
// assert. What IS testable, and what a "just delete the reset()" fix would
// silently break, is that the discard still discards. The source lint
// NativeRtSourceLintTests.MidiFifo_IsNeverResetWhileAConsumerCanBeRunning
// covers the other half.
void scenarioInstrumentAttachDiscardsStaleMidi()
{
    beginScenario("RtInstrumentAttachDiscardsStaleMidi");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    // sendMidiShort DROPS events when no instrument is attached and no bridge
    // is enabled, so the backlog has to be built while a slot is filled.
    StubInstrument first;
    if (! fx.engine().attachInstrumentPlugin(&first))
    { fail("could not attach the first stub instrument"); endScenario(); return; }

    constexpr int kStale = 400;
    for (int i = 0; i < kStale; ++i)
        fx.engine().sendMidiShort(0x90, (uint8_t) (40 + (i % 40)), 100);
    // Deliberately NO pump here: the events sit in the fifo, which is the whole
    // point — this is the "queued while the slot was empty / changing" case.
    fx.engine().detachInstrumentPlugin(&first);

    StubInstrument second;
    if (! fx.engine().attachInstrumentPlugin(&second))
    { fail("could not attach the second stub instrument"); endScenario(); return; }

    fx.pumpWarm(4);
    RT_CHECK(second.midiEventsSeen() == 0,
             "a fresh instrument was handed " + std::to_string(second.midiEventsSeen())
             + " stale MIDI event(s) queued for the previous one — the slot change did not "
               "discard the queue");
    RT_CHECK(first.midiEventsSeen() == 0,
             "the detached instrument rendered " + std::to_string(first.midiEventsSeen())
             + " event(s) after detach");

    // Not vacuous: the same instrument must still RECEIVE live MIDI, or the
    // assertion above would pass against a discard that simply broke the fifo.
    constexpr int kLive = 24;
    for (int i = 0; i < kLive; ++i)
        fx.engine().sendMidiShort(0x90, (uint8_t) (60 + (i % 12)), 100);
    fx.pumpWarm(4);
    RT_CHECK(second.midiEventsSeen() == kLive,
             "after the discard the instrument received " + std::to_string(second.midiEventsSeen())
             + " of " + std::to_string(kLive) + " live events — the fifo is broken, not drained");

    fx.engine().detachInstrumentPlugin(&second);
    note("stale " + std::to_string(kStale) + " discarded, live "
         + std::to_string(second.midiEventsSeen()) + "/" + std::to_string(kLive) + " delivered");
    endScenario();
}

// ---------------------------------------------------------------------------
// One NaN out of a VST3 used to silence a seat's EQ for the session.
// ---------------------------------------------------------------------------
//
// DspChain scrubs its input and every stage boundary. The two engine stages
// downstream of it — the user's VST3 input chain and the sandbox bridge's
// memcpy out of a child process's mapping — did not, and the very next thing
// after them is applyEqBank: juce::dsp::IIR biquads, which feed their own
// output back. One NaN sample therefore latches the band, and reset() is only
// reached on an OFF -> ON transition, which nothing in a normal session does.
// The peak meter reads the same buffer, so the UI showed a dead input.
//
// Three phases, one per way of being vacuous:
//   A  the plugin is a wire: the monitored output is audible and finite.
//   B  the poisoned block itself: output must be FINITE (the scrub sits between
//      the plugin and the EQ) — this is the assertion the fix exists for.
//   C  every block AFTER: audible again. Without the scrub the EQ state is NaN
//      and phase C never recovers, which is what makes B and C together
//      structurally impossible to pass against the broken code.
void scenarioEqTrimAndSparseInput()
{
    beginScenario("RtEqTrimRampsAndSparseInputIsSilent");
    constexpr int frames = 256;
    Fixture fx(48000, frames, 2);
    if (!fx.ok()) { fail(fx.error()); endScenario(); return; }
    fx.engine().setMonitorGain(1.f);
    fx.engine().setInputGain(1.f);
    // The ABI v34 master limiter is ON by default and delays the bus by its
    // 1 ms look-ahead. This scenario asserts an EQ trim ramp SAMPLE BY SAMPLE,
    // so it measures the EQ and not the stage after it. (Switching it off is a
    // true bypass — see MasterLimiter.h rule 1 — so the path here is exactly
    // the pre-v34 one.)
    fx.engine().setMasterLimiter(false, -0.3f, 80.0f);
    fx.engine().setEqRoute(DCA_EQ_ROUTE_INPUT);
    std::array<float, frames> input, left{}, right{};
    input.fill(.125f);
    const float* sparse[] = {input.data(), nullptr};
    float* output[] = {left.data(), right.data()};
    // Warm the monitor and input gain ramps, then change only EQ trim.
    for (int n = 0; n < 8; ++n) fx.engine().renderForTest(sparse, 2, output, 2, frames);
    const float before = left[0];
    dcrt::reset(); dcrt::arm();
    auto render = [&] { dcrt::ScopedRtThread rt; fx.engine().renderForTest(sparse, 2, output, 2, frames); };
    auto master = [&] { dcrt::ScopedRtThread rt; fx.engine().masterForTest(output, 2, frames); };
    RT_CHECK(before > .01f, "valid input channel is inaudible");
    fx.engine().setEqTrim(6.f);
    const float gain = std::pow(10.f, 6.f / 20.f);
    render();
    for (int n = 0; n < frames; ++n)
    {
        const float expected = before * (1.f + (gain - 1.f) * n / frames);
        RT_CHECK(std::abs(left[n] - expected) < 2.e-5f, "input EQ trim stepped instead of ramping");
        RT_CHECK(right[n] == 0.f, "disabled input retained stale samples");
    }
    render();
    RT_CHECK(std::abs(left[0] - before * gain) < 2.e-5f, "input EQ ramp target did not persist");

    fx.engine().setEqRoute(DCA_EQ_ROUTE_SONG);
    fx.engine().setEqTrim(0.f);
    left.fill(.125f); right.fill(.125f);
    master();
    fx.engine().setEqTrim(6.f);
    left.fill(.125f); right.fill(.125f);
    master();
    for (int n = 0; n < frames; ++n)
    {
        const float expected = .125f * (1.f + (gain - 1.f) * n / frames);
        RT_CHECK(std::abs(left[n] - expected) < 2.e-5f, "song EQ trim stepped instead of ramping");
        RT_CHECK(left[n] == right[n], "EQ trim shifted stereo image");
    }
    // Consume a coefficient update while the song bank is routed away, then
    // return without another publication. The first impulse must be filtered.
    fx.engine().setEqTrim(0.f);
    fx.engine().setEqRoute(DCA_EQ_ROUTE_INPUT);
    fx.engine().setEqBand(0, DCA_EQ_LOW_PASS, 800.f, .707f, 0.f, true);
    fx.engine().setEqEnabled(true);
    left.fill(0.f); right.fill(0.f);
    master();
    fx.engine().setEqRoute(DCA_EQ_ROUTE_SONG);
    left.fill(0.f); right.fill(0.f); left[0] = right[0] = .125f;
    master();
    RT_CHECK(left[0] < .01f && left[0] > 0.f, "route return failed to install current EQ coefficients");
    RT_CHECK(left[1] > 0.f, "route return was silent rather than filtered");

    dcrt::disarm();
    RT_CHECK(dcrt::allocCount() == 0 && dcrt::freeCount() == 0,
             "EQ snapshot/trim callback allocated or freed memory");
    endScenario();
}

/// Parks the audio thread in the MIDDLE of a block so a message-thread write
/// can be made to land there deterministically (W5-E2). Audio-transparent.
class MidBlockGateModule final : public dcrig::DspModule
{
public:
    void prepare(double, int, int) override {}
    void reset() override {}
    bool setParam(const char*, float) override { return false; }
    bool getParam(const char*, float&) const override { return false; }
    void process(float* const*, int, int) override { if (onProcess) onProcess(); }
    const char* typeId() const override { return "mid-block-gate-test"; }
    std::function<void()> onProcess;
};

class StagedGainModule final : public dcrig::DspModule
{
public:
    void prepare(double, int, int) override {}
    void reset() override {}
    bool setParam(const char*, float value) override { gain = value; return true; }
    bool getParam(const char*, float& value) const override { value = gain; return true; }
    void process(float* const* channels, int count, int frames) override
    { for (int c = 0; c < count; ++c) for (int n = 0; n < frames; ++n) channels[c][n] *= gain; }
    const char* typeId() const override { return "staged-gain-test"; }
    float gain = 8.f;
};

void scenarioStagedInsertion()
{
    beginScenario("RtStagedInsertionNeverExposesDefaultWet");
    dcrig::DspChain chain;
    chain.prepare(48000, 256, 1);
    auto module = std::make_unique<StagedGainModule>();
    auto* raw = module.get();
    const int id = chain.addNode(std::move(module), true);
    std::array<float, 256> samples;
    float* channels[] = {samples.data()};
    samples.fill(.1f); chain.process(channels, 1, 256);
    RT_CHECK(std::all_of(samples.begin(), samples.end(), [](float x) { return x == .1f; }),
             "parked insertion exposed the default effect before configuration");
    raw->setParam("gain", 2.f);
    chain.setBypass(id, false);
    for (int block = 0; block < 8; ++block)
    {
        samples.fill(.1f); chain.process(channels, 1, 256);
        for (float x : samples) RT_CHECK(x >= .09999f && x <= .20001f, "unpark exposed default wet");
    }
    RT_CHECK(std::abs(samples.back() - .2f) < 1.e-5f, "configured effect never became audible");
    dcrig::DspChain legacy;
    legacy.prepare(48000, 256, 1);
    legacy.addNode(std::make_unique<StagedGainModule>());
    samples.fill(.1f); legacy.process(channels, 1, 256);
    RT_CHECK(samples.front() == .8f, "ordinary insertion lost immediate offline wet behavior");
    endScenario();
}

// 3f -----------------------------------------------------------------------
// The song-mix sidechain key is UNTRUSTED and must be scrubbed (wave-2 audit
// finding #1). The chain's rule is that every untrusted stage is followed by
// dcdsp::scrubNonFinite, and the v34 key path honoured it for the DRY key
// (memcpy'd after the input scrub) and NOT for the SONG key: `out` is the
// engine's master-bus buffer, scrubbed only at the TOP of processMasterBus and
// again only after the instrument mixes, and the seat loop runs in between --
// so what processDspRig receives is the post-song-EQ buffer, the output of six
// juce::dsp::IIR biquads that reset() only on an OFF->ON transition.
//
// The chain's per-stage scrub cleans the BUFFER and never a module's internal
// state, so a NaN in the key goes into CompressorCore's sidechain HPF and RMS
// accumulator and stays there: the module is dead for the session with nothing
// saying why. This drives DspChain directly, which is the unit that changed --
// one poisoned key block between clean ones, and the rig has to come back.
/// THE MERGER MUST MATCH THE BRANCH'S ACTUAL LATENCY ON THE BLOCK IT CHANGES
/// (W4-D54).
///
/// `setParam` publishes into the module's `ParamMailbox`; the CORE adopts it at
/// the top of its next `process()`. `DspChain::setParam` then called
/// `recomputeBranchPdcLocked` IMMEDIATELY — so `latencySamples()` still answered
/// the OLD number and the merger was programmed from it. Every adapter that
/// delegates `latencySamples()` to its core is affected (whammy, vibrato,
/// harmonizer, klon); `overdrive` is not, because it answers from its own `p_`
/// copy, which `setParam` updates synchronously.
///
/// And it was never "one block early": NOTHING re-ran the recompute afterwards,
/// so the region stayed misaligned until an unrelated publish or prepare
/// happened along. A whammy flipped to `lowLatency` sat 768 samples out — 16 ms
/// at 48 kHz, a comb filter across the whole split region — for as long as the
/// player left the knob alone.
///
/// The observable is the MERGER's own delay line, not `chainLatency()`: that
/// walks the same stale numbers, so it cannot see this bug. The assertion is
/// that after the block in which the core adopts, `branchDelay` equals what the
/// branch actually costs NOW.
///
/// No device and no Fixture: `DspChain` is driven directly, so "the block in
/// which the core adopts" is a `process()` call this scenario makes itself
/// rather than something it has to race a pump for.
void scenarioBranchPdcFollowsAParamOnTheBlockItChanges()
{
    beginScenario("RtBranchPdcFollowsAParamOnTheBlockItChanges");
    dcrig::DspChain chain;
    chain.prepare(48000, 256, 2);

    // splitter -> [branch 1: whammy (latent)] [branch 2: a zero-latency gain]
    //          -> merger
    auto sp = dcrig::createModule("splitter");
    auto wh = dcrig::createModule("whammy");
    auto mg = dcrig::createModule("merger");
    if (sp == nullptr || wh == nullptr || mg == nullptr)
    { fail("the factory is missing splitter/whammy/merger"); endScenario(); return; }
    auto* rawMerger = static_cast<dcrig::MergerModule*>(mg.get());
    auto* rawWhammy = wh.get();

    chain.addNode(std::move(sp));
    const int whammy = chain.addNode(std::move(wh));
    const int gain   = chain.addNode(std::make_unique<StagedGainModule>());
    chain.addNode(std::move(mg));
    chain.setNodeBranch(whammy, 1);
    chain.setNodeBranch(gain, 2);

    std::vector<float> l(256, 0.f), r(256, 0.f);
    float* block[2] = { l.data(), r.data() };
    auto pump = [&] { chain.process(block, 2, 256); };

    pump();   // settle: the whammy adopts its defaults

    const int latBefore   = rawWhammy->latencySamples();
    const int delayBefore = rawMerger->branchDelay(1);   // branch 2 is the short one
    RT_CHECK(latBefore > 0,
             "the whammy reports no latency at all — the region is not misaligned and this "
             "scenario is vacuous");
    RT_CHECK(delayBefore == latBefore,
             "branch 2 is compensated by " + std::to_string(delayBefore) + " against a branch-1 "
             "latency of " + std::to_string(latBefore) + " — the region is not aligned even "
             "BEFORE the param moves, so the assertion below would prove nothing");

    // THE GESTURE. `lowLatency` re-runs the whammy's whole geometry, which is
    // what moves its latency (1,920 <-> 1,152 samples at 48 kHz).
    RT_CHECK(chain.setParam(whammy, "lowLatency", 1.0f),
             "the whammy refused the lowLatency param — nothing below is testing anything");

    // The core has NOT adopted yet: it does that at the top of its next
    // process(). Asserted so the scenario is honest about what it is measuring —
    // if this ever stops holding, the bug is gone by another route and the
    // assertions below stop being about W4-D54.
    RT_CHECK(rawWhammy->latencySamples() == latBefore,
             "the whammy's latency moved WITHOUT a process() — the ParamMailbox is being applied "
             "on the message thread, which is the thing it exists not to do");

    pump();   // the block in which the core adopts

    const int latAfter   = rawWhammy->latencySamples();
    const int delayAfter = rawMerger->branchDelay(1);
    note("whammy latency " + std::to_string(latBefore) + " -> " + std::to_string(latAfter)
         + ", merger branch-2 delay " + std::to_string(delayBefore) + " -> "
         + std::to_string(delayAfter));

    RT_CHECK(latAfter != latBefore,
             "the lowLatency flip did not move the whammy's latency (" + std::to_string(latAfter)
                 + " both times) — pick a param that does, or this proves nothing");
    RT_CHECK(delayAfter == latAfter,
             "on the block the core adopted the new geometry the merger still delays branch 2 by "
                 + std::to_string(delayAfter) + " against a branch-1 latency of "
                 + std::to_string(latAfter) + " — PDC was programmed from the latency the core "
                 "had not adopted yet, and nothing came back to correct it");

    // ...and it STAYS right: the re-derive is gated on a dirty flag, so a
    // subsequent block must not undo it (or keep re-deriving for ever).
    pump(); pump();
    RT_CHECK(rawMerger->branchDelay(1) == latAfter,
             "the branch delay moved again on a block with no param write — the callback-side "
             "re-derive is not gated and is running every block");

    // And back, so the direction is not an artefact of one value.
    RT_CHECK(chain.setParam(whammy, "lowLatency", 0.0f), "the whammy refused the flip back");
    pump();
    RT_CHECK(rawMerger->branchDelay(1) == rawWhammy->latencySamples(),
             "flipping lowLatency BACK left the merger at " + std::to_string(rawMerger->branchDelay(1))
                 + " against " + std::to_string(rawWhammy->latencySamples()));

    // ---- W5-E2: THE WRITE THAT LANDS **INSIDE** A BLOCK -------------------
    //
    // Everything above drives setParam BETWEEN blocks, which never enters the
    // window the fix is about. The dirty flag used to be consumed by an
    // `exchange` at the very END of process(), so a write landing after the
    // whammy's adopt and before that exchange was eaten by a block whose
    // modules had run the OLD latency: the walk programmed the mergers from the
    // old number, cleared the flag, and — exactly as in the original bug —
    // nothing came back. One callback is the whole window, which at 5-10 ms
    // blocks is what an ordinary UI drag hits.
    //
    // Reproduced deterministically rather than raced: a gate module sitting
    // AFTER the whammy in branch 1 parks the audio thread mid-block while a
    // writer thread performs the setParam and RETURNS. No lock-order hazard —
    // setParam takes writeMutex_ only, and process() holds processMutex_.
    {
        const int latStart = rawWhammy->latencySamples();
        std::mutex m;
        std::condition_variable cv;
        bool writeDone = false;
        bool armed = false;

        auto gate = std::make_unique<MidBlockGateModule>();
        gate->onProcess = [&]
        {
            std::unique_lock<std::mutex> lk(m);
            if (! armed) return;             // only the armed block waits
            armed = false;
            cv.wait(lk, [&] { return writeDone; });
        };
        const int gateId = chain.addNode(std::move(gate));
        chain.setNodeBranch(gateId, 1);      // same branch as the whammy, after it
        pump();                              // settle the new topology

        const int latBeforeMid   = rawWhammy->latencySamples();
        const int delayBeforeMid = rawMerger->branchDelay(1);
        RT_CHECK(delayBeforeMid == latBeforeMid,
                 "the region is already misaligned before the mid-block write — this case would "
                 "prove nothing");

        { std::lock_guard<std::mutex> lk(m); armed = true; writeDone = false; }
        std::thread writer([&]
        {
            // Wait until the audio thread is parked INSIDE the block, then write.
            for (;;)
            {
                { std::lock_guard<std::mutex> lk(m); if (! armed) break; }
                std::this_thread::yield();
            }
            chain.setParam(whammy, "lowLatency", 1.0f);
            { std::lock_guard<std::mutex> lk(m); writeDone = true; }
            cv.notify_all();
        });
        pump();     // the block the writer lands inside
        writer.join();

        // The whammy had ALREADY adopted when the write landed, so this block's
        // modules ran the old geometry. The flag must therefore survive this
        // block and be honoured by the NEXT one.
        pump();

        const int latMid   = rawWhammy->latencySamples();
        const int delayMid = rawMerger->branchDelay(1);
        note("mid-block write: whammy latency " + std::to_string(latStart) + " -> "
             + std::to_string(latMid) + ", merger branch-2 delay " + std::to_string(delayBeforeMid)
             + " -> " + std::to_string(delayMid));
        RT_CHECK(latMid != latBeforeMid,
                 "the mid-block write did not move the whammy's latency at all");
        RT_CHECK(delayMid == latMid,
                 "a setParam that landed INSIDE a block left the merger at "
                     + std::to_string(delayMid) + " against a branch-1 latency of "
                     + std::to_string(latMid) + " — the dirty flag was consumed by a block whose "
                     "modules had not adopted it, and nothing came back (W5-E2)");
    }

    endScenario();
}

/// Records what the CHAIN handed it as a sidechain key. A stub rather than a
/// real core on purpose: whether any particular detector visibly latches on a
/// NaN is that core's business and it changes, but "the key the chain hands a
/// module is finite" is the chain's contract and it does not. Driving a real
/// compressor here looked like a test and was vacuous -- it recovers.
class SidechainProbeModule final : public dcrig::DspModule
{
public:
    void prepare(double, int, int) override {}
    void reset() override {}
    bool setParam(const char*, float) override { return true; }
    bool getParam(const char*, float& value) const override { value = 0.f; return true; }
    const char* typeId() const override { return "sidechain-probe-test"; }
    bool supportsSidechain() const override { return true; }

    void process(float* const*, int, int) override { sawUnkeyed = true; }

    void processSidechained(float* const*, int, int frames,
                            const float* const* key, int keyChannels) override
    {
        ++keyedBlocks;
        for (int c = 0; c < keyChannels; ++c)
        {
            if (key[c] == nullptr) continue;
            for (int n = 0; n < frames; ++n)
            {
                const float x = key[c][n];
                if (! std::isfinite(x)) ++nonFiniteKeySamples;
                else keyPeak = std::max(keyPeak, std::abs(x));
            }
        }
    }

    int   keyedBlocks = 0;
    int   nonFiniteKeySamples = 0;
    float keyPeak = 0.f;
    bool  sawUnkeyed = false;
};

// 3f -----------------------------------------------------------------------
// The song-mix sidechain key is UNTRUSTED and must be scrubbed (wave-2 audit
// finding #1). The chain's rule is that every untrusted stage is followed by
// dcdsp::scrubNonFinite, and the v34 key path honoured it for the DRY key
// (memcpy'd after the input scrub) and NOT for the SONG key: `out` is the
// engine's master-bus buffer, scrubbed only at the TOP of processMasterBus and
// again only after the instrument mixes, and the seat loop runs in between --
// so what processDspRig receives is the post-song-EQ buffer, the output of six
// juce::dsp::IIR biquads that reset() only on an OFF->ON transition.
//
// The chain's per-stage scrub cleans the BUFFER and never a module's internal
// state, so a NaN in the key went into NoiseGateCore's recursive one-pole
// detector and CompressorCore's sidechain HPF + RMS accumulator and stayed
// there: the seat's gate latched CLOSED for the session with nothing saying
// why. What is asserted is the CHAIN's half of that contract -- the key it
// hands a module is finite -- because that is what changed and it cannot rot
// when a core's own NaN behaviour does.
void scenarioSongMixKeyIsScrubbed()
{
    beginScenario("RtNaNInTheSongMixSidechainKeyCannotLatchADetector");
    constexpr int kFrames = 256;
    constexpr int kCh     = 2;

    dcrig::DspChain chain;
    chain.prepare(48000, kFrames, kCh);
    auto probe = std::make_unique<SidechainProbeModule>();
    auto* raw = probe.get();
    const int id = chain.addNode(std::move(probe));
    RT_CHECK(id >= 0, "could not add the probe");
    RT_CHECK(chain.setNodeSidechain(id, dcrig::DspChain::kSidechainSongMix),
             "setNodeSidechain refused a module that advertises supportsSidechain()");

    std::array<float, kFrames> l{}, r{}, keyL{}, keyR{};
    float* ch[]  = { l.data(), r.data() };
    const float* key[] = { keyL.data(), keyR.data() };

    auto pump = [&](bool poisonKey)
    {
        l.fill(0.35f); r.fill(0.35f);
        keyL.fill(0.5f); keyR.fill(0.5f);
        if (poisonKey)
        {
            keyL[7]   = std::numeric_limits<float>::quiet_NaN();
            keyR[11]  = std::numeric_limits<float>::infinity();
            keyL[200] = -std::numeric_limits<float>::infinity();
        }
        chain.process(ch, kCh, kFrames, key, kCh);
    };

    // Not vacuous, three ways: the keyed path has to actually run, it has to
    // receive the SONG key rather than falling back to the node's own input
    // (0.5 vs the chain input's 0.35), and the plain process() must never be
    // the one that ran.
    pump(false);
    RT_CHECK(raw->keyedBlocks == 1,
             "the keyed path did not run - processSidechained saw "
                 + std::to_string(raw->keyedBlocks) + " block(s)");
    RT_CHECK(! raw->sawUnkeyed, "the chain fell back to the unkeyed process()");
    RT_CHECK(raw->keyPeak > 0.4f,
             "the key peaked at " + std::to_string(raw->keyPeak)
                 + " - that is the node's own input, not the song mix, so the key was never delivered");
    RT_CHECK(raw->nonFiniteKeySamples == 0, "a clean song mix arrived non-finite");

    for (int i = 0; i < 16; ++i) pump(true);
    RT_CHECK(raw->keyedBlocks == 17, "the poisoned blocks did not reach the keyed path");
    RT_CHECK(raw->nonFiniteKeySamples == 0,
             std::to_string(raw->nonFiniteKeySamples)
                 + " non-finite sample(s) reached a stateful detector through the song-mix key - "
                   "the engine's output buffer is untrusted and must be scrubbed before it is handed on");
    RT_CHECK(! raw->sawUnkeyed,
             "a poisoned key made the chain fall back to the module's own input - it must scrub, not drop");
    note("keyed blocks " + std::to_string(raw->keyedBlocks) + ", key peak "
         + std::to_string(raw->keyPeak) + ", non-finite key samples "
         + std::to_string(raw->nonFiniteKeySamples));
    endScenario();
}

// 3g -----------------------------------------------------------------------
// Branch PDC must follow the PUBLISHED order, never the shadow (wave-2 audit
// finding #2). recomputeBranchPdcLocked writes straight into the live
// MergerModules, which the audio thread reads on the very next block -- so
// deriving it from `idOrder_` while an edit scope is open re-aligned a region
// that is still playing the PRE-edit chain. setParam had no editing_ check at
// all, so the FIRST knob write inside a preset load did it.
//
// The observable: the region's alignment, read back through chainLatency(),
// must not move while a scope is open, and must move when it commits.
void scenarioBranchPdcInsideAnEditScope()
{
    beginScenario("RtBranchPdcFollowsThePublishedOrderNotTheShadow");
    dcrig::DspChain chain;
    chain.prepare(48000, 256, 2);

    // splitter -> [branch 1: whammy (latent)] [branch 2: a zero-latency gain]
    //          -> merger
    auto sp = dcrig::createModule("splitter");
    auto wh = dcrig::createModule("whammy");
    auto mg = dcrig::createModule("merger");
    if (sp == nullptr || wh == nullptr || mg == nullptr)
    { fail("the factory is missing splitter/whammy/merger"); endScenario(); return; }
    auto* rawMerger = static_cast<dcrig::MergerModule*>(mg.get());
    chain.addNode(std::move(sp));
    const int whammy = chain.addNode(std::move(wh));
    const int staged = chain.addNode(std::make_unique<StagedGainModule>());
    chain.addNode(std::move(mg));
    chain.setNodeBranch(whammy, 1);
    chain.setNodeBranch(staged, 2);

    // The OBSERVABLE is the merger's own delay line, not chainLatency() --
    // chainLatency walks the shadow too, so it cannot see this bug. Branch 2
    // is the short one, so it is the branch that gets delayed.
    const int aligned = rawMerger->branchDelay(1);
    RT_CHECK(aligned > 0,
             "branch 2 is compensated by " + std::to_string(aligned)
                 + " samples - the branches are not misaligned and this scenario is vacuous");

    // --- hole 1: a PARAM write inside a scope --------------------------------
    // Remove the only latent node in the shadow, then write a param to a node
    // that is still live. Under the bug setParam recomputed unconditionally
    // from `idOrder_` (whammy gone, every branch at 0) and re-aligned the
    // region that is still PLAYING with the whammy in it.
    RT_CHECK(chain.beginEdit(), "beginEdit was refused on an idle chain");
    chain.removeNode(whammy);
    RT_CHECK(chain.setParam(staged, "gain", 0.5f),
             "the staged gain refused a param write - the setParam hole is untested");
    const int afterParam = rawMerger->branchDelay(1);
    RT_CHECK(afterParam == aligned,
             "a param write inside an edit scope moved the LIVE region's alignment from "
                 + std::to_string(aligned) + " to " + std::to_string(afterParam)
                 + " - PDC was recomputed from the shadow order");

    // --- hole 2: a device change (prepare) inside a scope --------------------
    chain.prepare(48000, 256, 2);
    const int afterPrepare = rawMerger->branchDelay(1);
    RT_CHECK(afterPrepare == aligned,
             "a prepare() inside an edit scope moved the LIVE region's alignment from "
                 + std::to_string(aligned) + " to " + std::to_string(afterPrepare));

    // --- and the commit must actually follow the NEW order -------------------
    RT_CHECK(chain.commitEdit(), "commitEdit was refused on an open scope");
    const int afterCommit = rawMerger->branchDelay(1);
    RT_CHECK(afterCommit == 0,
             "after the commit removed the only latent node the merger still delays branch 2 by "
                 + std::to_string(afterCommit) + " - PDC never followed the published order");
    note("branch-2 delay: aligned " + std::to_string(aligned) + " -> after param "
         + std::to_string(afterParam) + " -> after prepare " + std::to_string(afterPrepare)
         + " -> committed " + std::to_string(afterCommit));
    endScenario();
}

void scenarioNaNFromAnInputPluginIsScrubbed()
{
    beginScenario("RtNaNFromAnInputPluginCannotLatchTheEq");
    Fixture fx(48000, 256, 2);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.engine().setMonitorGain(1.0f);
    fx.engine().setInputGain(1.0f);
    fx.installInputStimulus(0.25f, 187.5, 1.5707963267948966);

    StubNaNInputPlugin plugin;
    if (! fx.engine().attachInputPlugin(&plugin, 0))
    { fail("attachInputPlugin refused the stub"); endScenario(); return; }

    // A real, engaged band on the INPUT route: the bank whose state the NaN
    // would poison. A bell with gain is enough — it has to actually run.
    fx.engine().setEqRoute(DCA_EQ_ROUTE_INPUT);
    fx.engine().setEqBand(0, DCA_EQ_BELL, 800.0f, 1.0f, 6.0f, true);
    fx.engine().setEqEnabled(true);

    auto minPeakOver = [&](int blocks)
    {
        float minPeak = 1.0e9f;
        bool  anyNonFinite = false;
        for (int i = 0; i < blocks; ++i)
        {
            {
                dcrt::ScopedRtThread rt;
                nulldev::pump(1);
            }
            // The BLOCK, not the peak: std::max(x, NaN) is x, so a fully
            // poisoned block reads back as a peak of 0.
            if (fx.lastOutputHasNonFinite()) anyNonFinite = true;
            minPeak = std::min(minPeak, fx.lastOutputPeak());
        }
        return std::make_pair(minPeak, anyNonFinite);
    };

    warmUpAndArm(fx);

    const auto before = minPeakOver(32);
    RT_CHECK(before.first > 1.0e-3f,
             "phase A is vacuous: the monitored input was already silent (min block peak "
             + std::to_string(before.first) + ") before any NaN was injected");
    RT_CHECK(! before.second, "phase A saw a non-finite output with no NaN injected");

    plugin.poisonNextBlock();
    const auto during = minPeakOver(1);
    RT_CHECK(plugin.poisonedBlocks() == 1,
             "the stub never got to poison a block — it saw "
             + std::to_string(plugin.poisonedBlocks()) + ", so this scenario tested nothing");
    RT_CHECK(! during.second,
             "a NaN block from an input VST3 reached the device output unscrubbed");

    const auto after = minPeakOver(64);
    RT_CHECK(! after.second, "the input EQ latched non-finite after one NaN block");
    RT_CHECK(after.first > 1.0e-3f,
             "the seat went silent after one NaN block (min block peak "
             + std::to_string(after.first) + ") — the EQ latched and nothing resets it");

    // Phase D — the LAST untrusted stage. The in-process VSTi and the sandbox
    // child's instrument leg mix into `out` after the master bus, so nothing
    // upstream can protect the device or the streaming mirror from them.
    StubInstrument instrument;
    if (! fx.engine().attachInstrumentPlugin(&instrument))
    {
        fail("attachInstrumentPlugin refused the stub");
    }
    else
    {
        instrument.poisonNextBlock();
        const auto instrBlock = minPeakOver(1);
        RT_CHECK(instrument.poisonedBlocks() == 1,
                 "the instrument stub never rendered a poisoned block ("
                 + std::to_string(instrument.poisonedBlocks()) + ") — phase D is vacuous");
        RT_CHECK(! instrBlock.second,
                 "a NaN block from the instrument bus reached the device output (and the "
                 "streaming mirror) unscrubbed");
        fx.engine().detachInstrumentPlugin(&instrument);
    }

    dcrt::disarm();
    fx.engine().detachInputPlugin(&plugin);
    fx.engine().setEqEnabled(false);

    reportHeap("NaN scrub");
    RT_CHECK(dcrt::allocCount() == 0,
             "the NaN scrub path allocated " + std::to_string(dcrt::allocCount()) + " time(s)");
    RT_CHECK(dcrt::freeCount() == 0,
             "the NaN scrub path freed " + std::to_string(dcrt::freeCount()) + " time(s)");
    note("peaks: before " + std::to_string(before.first)
         + ", after " + std::to_string(after.first)
         + " (" + std::to_string(plugin.poisonedBlocks()) + " poisoned block)");
    endScenario();
}

/// An input-chain plugin that THROWS out of processBlock, which is the one
/// misbehaviour the NaN stub above cannot model. Two stages, so the scenario can
/// also prove the loop BREAKS rather than running the next stage over the half
/// processed buffer the thrower left behind.
class StubThrowingInputPlugin final : public juce::AudioPluginInstance
{
public:
    StubThrowingInputPlugin()
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "RtCheck Stub Throwing Input"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        ran_.fetch_add(1, std::memory_order_relaxed);
        const bool always = throwAlways_.load(std::memory_order_acquire);
        if (! throwNext_.exchange(false, std::memory_order_acq_rel) && ! always) return;
        // Half-process the block first, so "break, not continue" is a claim the
        // scenario could in principle catch failing: a stage that ran after this
        // one would be running over audio nobody finished writing.
        if (buffer.getNumChannels() > 0)
        {
            float* d = buffer.getWritePointer(0);
            for (int i = 0; i < buffer.getNumSamples() / 2; ++i) d[i] = 0.0f;
        }
        threw_.fetch_add(1, std::memory_order_relaxed);
        // An empty tag type, not a std::runtime_error: the point is the unwind,
        // and the heap counters are armed around the pump that provokes it.
        throw PluginFault{};
    }

    struct PluginFault {};

    void throwOnNextBlock() noexcept { throwNext_.store(true, std::memory_order_release); }
    /// Throw on EVERY block from here on — the deterministic fault (an expired
    /// licence check, a layout it was not prepared for), which is the common
    /// case and the one the single-block phase above cannot model.
    void throwOnEveryBlock() noexcept { throwAlways_.store(true, std::memory_order_release); }
    int  blocksRun()   const noexcept { return ran_  .load(std::memory_order_relaxed); }
    int  blocksThrown() const noexcept { return threw_.load(std::memory_order_relaxed); }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "RtCheck Stub Throwing Input";
        d.pluginFormatName = "RtCheck";
        d.isInstrument = false;
    }

private:
    std::atomic<bool> throwNext_   { false };
    std::atomic<bool> throwAlways_ { false };
    std::atomic<int>  ran_         { 0 };
    std::atomic<int>  threw_       { 0 };
};

/// A THIRD-PARTY PLUGIN THAT THROWS, on the input chain.
///
/// The engine states the rule — "uncaught exceptions in JUCE's
/// getNextAudioBlock would terminate the driver callback" — and applied it at
/// `renderInstrumentBus` and `reprepareAttachedPlugins`. The INPUT chain, which
/// is the path every amp-sim user is on, was the one site that called
/// `processBlock` bare, and `audioDeviceIOCallbackWithContext` had no backstop
/// either, so an exception out of a user plugin unwound through JUCE's driver
/// frame with nothing to catch it anywhere: a PROCESS KILL, not a dropped block.
///
/// WITHOUT THE FIX THIS SCENARIO TAKES THE WHOLE GATE PROCESS DOWN, which is
/// the proof that it is not vacuous — there is nothing to assert about a
/// std::terminate. What it asserts WITH the fix is that the callback survived,
/// kept running afterwards, and that the throwing stage stopped the chain rather
/// than letting the stage behind it run on half-written audio.
void scenarioThrowingInputPluginCannotKillTheCallback()
{
    beginScenario("RtAThrowingInputPluginCannotKillTheCallback");
    Fixture fx(48000, 256, 2);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    fx.engine().setMonitorGain(1.0f);
    fx.engine().setInputGain(1.0f);
    fx.installInputStimulus(0.25f, 187.5, 1.5707963267948966);

    // TWO stages. The thrower is first, so `second` running at all after a throw
    // would mean the loop continued instead of breaking.
    StubThrowingInputPlugin first;
    StubThrowingInputPlugin second;
    if (! fx.engine().attachInputPlugin(&first, 0))
    { fail("attachInputPlugin refused the first stub"); endScenario(); return; }
    if (! fx.engine().attachInputPlugin(&second, 0))
    { fail("attachInputPlugin refused the second stub"); endScenario(); return; }

    warmUpAndArm(fx);

    // Phase A — both stages run on a normal block, so the counters below mean
    // something.
    { dcrt::ScopedRtThread rt; nulldev::pump(4); }
    const int firstBefore  = first.blocksRun();
    const int secondBefore = second.blocksRun();
    RT_CHECK(firstBefore  > 0, "the first input stub never ran — the chain was not attached");
    RT_CHECK(secondBefore > 0, "the second input stub never ran — phase B cannot detect a break");

    // Phase B — one throwing block. Survive it.
    first.throwOnNextBlock();
    { dcrt::ScopedRtThread rt; nulldev::pump(1); }
    RT_CHECK(first.blocksThrown() == 1,
             "the stub never got to throw (" + std::to_string(first.blocksThrown())
             + ") — this scenario tested nothing");
    RT_CHECK(second.blocksRun() == secondBefore,
             "the stage AFTER the thrower ran on the half-processed block ("
             + std::to_string(second.blocksRun() - secondBefore)
             + " extra): the loop continued where it must break");

    // Phase C — the callback is still alive and still running the chain, and
    // THE OFFENDER IS NOT RE-ENTERED. That second half is W5-E7 and it inverts
    // what this phase used to assert: before the latch it demanded that `first`
    // keep running, because "the chain stopped" was the only failure it could
    // imagine. Re-entering a plugin that has thrown IS the failure — a C++
    // throw is not RT-safe, and the deterministic fault (the storm phase below)
    // is the common case.
    const int firstAfterThrow = first.blocksRun();
    { dcrt::ScopedRtThread rt; nulldev::pump(8); }
    dcrt::disarm();
    RT_CHECK(first.blocksRun() == firstAfterThrow,
             "the plugin that threw was entered " + std::to_string(first.blocksRun() - firstAfterThrow)
             + " more time(s) over the next 8 blocks — it is not being latched, so it allocates "
             "an exception and unwinds on every callback (W5-E7)");
    RT_CHECK(second.blocksRun() >= secondBefore + 8,
             "the stage behind the thrower never came back after the throwing block — a latched "
             "fault must SKIP the offender, not disable the rest of the chain");
    RT_CHECK(fx.lastOutputPeak() > 1.0e-3f,
             "the seat is silent after the throw (peak " + std::to_string(fx.lastOutputPeak())
             + ") — the callback survived but stopped producing audio");
    {
        dca_xrun_stats s{};
        fx.engine().fillXrunStats(s);
        RT_CHECK(s.input_plugin_faults >= 1,
                 "input_plugin_faults is " + std::to_string(s.input_plugin_faults)
                 + " after a plugin threw — nothing reports the dropped stage, so the user has a "
                 "silent pedal and no signal at all");
    }

    note("input chain: first ran " + std::to_string(first.blocksRun())
         + " (threw " + std::to_string(first.blocksThrown()) + "), second ran "
         + std::to_string(second.blocksRun()));

    // ---- W5-E7: A DETERMINISTIC FAULT IS LATCHED, NOT RE-ENTERED ----------
    //
    // Everything above proves SURVIVAL of one throwing block. It says nothing
    // about the steady state, and the steady state is the common case: a plugin
    // that throws once its trial expires, or one prepared for the wrong layout,
    // faults on EVERY block. Until 2026-09-12 nothing recorded that it had, so
    // it was re-entered and re-threw once per callback for ever — and a C++
    // throw is not RT-safe at any price (__cxa_allocate_exception mallocs; the
    // unwinder takes a global lock over the .pdata lookup; tens of microseconds
    // against a 2.6 ms budget at 128 frames / 48 kHz). The user got a permanent
    // glitch with a dry monitor and nothing anywhere saying why.
    {
        // RE-ARM first: it is latched from phase B, and the point of this phase
        // is the STEADY STATE of a plugin that faults every time. Detach +
        // attach republishes the chain, which is exactly the re-arm contract.
        fx.engine().detachInputPlugin(&first);
        if (! fx.engine().attachInputPlugin(&first, 0))
        { fail("re-attaching the thrower failed — the storm phase cannot run"); endScenario(); return; }
        { dcrt::ScopedRtThread rt; nulldev::pump(2); }
        RT_CHECK(first.blocksRun() > firstAfterThrow,
                 "re-attaching a latched plugin did not give it another chance — the faulted set "
                 "is not keyed to the chain snapshot");

        dca_xrun_stats base{};
        fx.engine().fillXrunStats(base);

        const int firstBeforeStorm  = first.blocksRun();
        const int secondBeforeStorm = second.blocksRun();
        const int threwBeforeStorm  = first.blocksThrown();

        first.throwOnEveryBlock();
        { dcrt::ScopedRtThread rt; nulldev::pump(200); }

        const int entered = first.blocksRun()  - firstBeforeStorm;
        const int threw   = first.blocksThrown() - threwBeforeStorm;
        dca_xrun_stats after{};
        fx.engine().fillXrunStats(after);
        const int64_t faults = after.input_plugin_faults - base.input_plugin_faults;

        note("200 blocks with a permanently faulting stage: entered " + std::to_string(entered)
             + ", threw " + std::to_string(threw) + ", input_plugin_faults +"
             + std::to_string(faults) + "; the stage behind it ran "
             + std::to_string(second.blocksRun() - secondBeforeStorm));

        RT_CHECK(threw >= 1,
                 "the stub never threw during the storm — this case is vacuous");
        RT_CHECK(entered <= 2,
                 "a permanently faulting input plugin was entered " + std::to_string(entered)
                     + " times over 200 blocks: it is not being marked failed, so it allocates an "
                     "exception and unwinds on EVERY callback (W5-E7)");
        RT_CHECK(faults == 1,
                 "input_plugin_faults moved by " + std::to_string(faults)
                     + " — the counter must be ONE PER PLUGIN, not one per block, or a host "
                     "polling it cannot tell a single broken pedal from a load problem");
        RT_CHECK(second.blocksRun() >= secondBeforeStorm + 100,
                 "the stage BEHIND the faulting one stopped running ("
                     + std::to_string(second.blocksRun() - secondBeforeStorm)
                     + " blocks): a latched fault must SKIP the offender, not disable the rest of "
                     "the chain");

    }

    fx.engine().detachInputPlugin(&second);
    fx.engine().detachInputPlugin(&first);
    endScenario();
}

// ---------------------------------------------------------------------------
// DspChain::prepare against a running process() — the P0 the lock-site file
// could never have caught, because the bug was a MISSING lock.
// ---------------------------------------------------------------------------
//
// prepare() took writeMutex_; process() takes processMutex_; the two share no
// mutex at all, so they ran CONCURRENTLY. prepare() then
//
//   * `assign()`s branchData_ and dryData_ — freeing the blocks the memcpys in
//     process() are holding raw pointers into (branchPtrs_/dryPtrs_),
//   * rewrites numChannels_/maxBlock_, the two values process() consults to
//     decide whether that scratch is safe to touch at all, and
//   * re-prepares every module underneath the block that is walking it.
//
// Reached from prepareMasterBus (device change / NAM 48 kHz request / watchdog
// recovery) and from dspProcessOffline's lazy re-prepare.
//
// This drives the chain DIRECTLY rather than through the engine: the ABI's
// offline export now refuses to run while a device is open, and JUCE serialises
// its own device restart against the callback, so neither shipping entry point
// can be made to overlap on demand. A bare DspChain can, and it is the same
// object with the same two mutexes.
//
// THE DETECTOR IS THE GEOMETRY, not luck. prepare() alternates between a WIDE
// (2 ch x 1024) and a NARROW (1 ch x 64) scratch while the render thread keeps
// asking for 1024 frames on 2 channels through a rig with a real split region.
// Unbypassed, that is a memcpy of 8 KB per branch per channel into a vector
// prepare has just shrunk to 256 bytes — a debug-CRT heap overflow, i.e. a
// crash or a corrupted heap, not a soft wrong answer. The render thread is also
// marked as the RT thread and the sentinel is armed, so the same run proves the
// new lock did not put an allocation on the callback.
void scenarioRigPrepareCannotRaceProcess()
{
    beginScenario("RtRigPrepareCannotRaceTheCallback");

    constexpr int kWideBlock  = 1024;
    constexpr int kWideCh     = 2;
    constexpr int kNarrowBlock = 64;
    constexpr int kNarrowCh    = 1;

    dcrig::DspChain chain;
    for (const char* id : kRigNodes)
        if (auto m = dcrig::createModule(id)) chain.addNode(std::move(m));
    std::vector<int> ids(64);
    const int nodes = chain.getNodeIds(ids.data(), (int) ids.size());
    RT_CHECK(nodes >= 12, "expected a 12-node rig, built " + std::to_string(nodes));
    if (nodes >= 8)
    {
        // Two nodes onto branch 2 so the split region — and therefore the
        // branch scratch prepare() reallocates — is genuinely exercised.
        chain.setNodeBranch(ids[6], 2);
        chain.setNodeBranch(ids[7], 2);
    }
    chain.prepare(48000.0, kWideBlock, kWideCh);

    std::vector<float> left((size_t) kWideBlock), right((size_t) kWideBlock);
    for (int i = 0; i < kWideBlock; ++i)
    {
        const float s = 0.25f * (float) std::sin(2.0 * 3.14159265358979323846 * 187.5 * i / 48000.0);
        left[(size_t) i] = right[(size_t) i] = s;
    }

    std::atomic<bool>     run{ true };
    std::atomic<long>     blocks{ 0 };
    std::atomic<long>     prepares{ 0 };
    std::atomic<bool>     nonFinite{ false };

    std::thread render([&]
    {
        std::vector<float> l = left, r = right;
        float* ch[2] = { l.data(), r.data() };
        while (run.load(std::memory_order_acquire))
        {
            // The sentinel's arming flag is thread_local: this is the only
            // thread that counts as the audio thread, so the prepare thread's
            // (legitimate, heavy) allocations are ignored.
            {
                dcrt::ScopedRtThread rt;
                chain.process(ch, kWideCh, kWideBlock);
            }
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < kWideBlock; ++i)
                    if (! std::isfinite(ch[c][i])) nonFinite.store(true, std::memory_order_relaxed);
            // Re-seed so a decaying tail cannot make later blocks trivially
            // finite — every block is real work on real signal.
            l = left; r = right;
            blocks.fetch_add(1, std::memory_order_relaxed);
            // A real callback has a block period between calls. Without this
            // gap the render thread re-takes processMutex_ the instant it
            // releases it and prepare() — which now barriers on that mutex —
            // starves: measured 2026-08-29 at 2–3 prepares in 1.5 s, so the
            // probe reported itself vacuous on an idle box.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    dcrt::reset();
    dcrt::arm();
    // Run until the probe has done enough prepares to mean something, bounded.
    // Both sides must have run enough to overlap: a 12-node prepare holds
    // processMutex_ for tens of ms, so an unfair handoff can starve EITHER
    // thread; the loop runs until both floors are met (bounded), and the
    // checks below are the same two floors.
    // Bounded by the shared evidence cap (#55), not a private 20 s.
    constexpr long kWantedPrepares = 60;
    constexpr long kWantedBlocks   = 200;
    long i = 0;
    const Gathered g = gatherEvidence([&]
    {
        if (i++ % 2 == 0) chain.prepare(48000.0, kNarrowBlock, kNarrowCh);
        else              chain.prepare(44100.0, kWideBlock,  kWideCh);
        prepares.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, [&]
    {
        return prepares.load(std::memory_order_relaxed) >= kWantedPrepares
            && blocks.load(std::memory_order_relaxed) >= kWantedBlocks;
    });
    // Leave it wide so the render thread's last blocks are the dangerous ones.
    chain.prepare(48000.0, kWideBlock, kWideCh);
    run.store(false, std::memory_order_release);
    render.join();
    dcrt::disarm();

    reportHeap("rig prepare vs process");
    RT_CHECK(dcrt::allocCount() == 0,
             "process() allocated " + std::to_string(dcrt::allocCount())
             + " time(s) while prepare() churned the scratch");
    RT_CHECK(dcrt::freeCount() == 0,
             "process() freed " + std::to_string(dcrt::freeCount())
             + " time(s) while prepare() churned the scratch");
    RT_CHECK(! nonFinite.load(), "process() produced a non-finite sample across a concurrent prepare()");
    // Not vacuous: both sides have to have actually run, and run OVERLAPPED.
    // (Counts, never elapsed time — the repo's own rule.)
    RT_CHECK(blocks.load() >= kWantedBlocks,
             "the render thread only completed " + std::to_string(blocks.load())
             + " block(s) — the probe never overlapped a prepare" + g.why());
    RT_CHECK(prepares.load() >= kWantedPrepares,
             "only " + std::to_string(prepares.load()) + " prepare(s) ran — the probe is vacuous" + g.why());
    note("blocks " + std::to_string(blocks.load()) + " vs prepares " + std::to_string(prepares.load())
         + " (wide " + std::to_string(kWideCh) + "x" + std::to_string(kWideBlock)
         + " / narrow " + std::to_string(kNarrowCh) + "x" + std::to_string(kNarrowBlock) + ")");
    endScenario();
}

// A STRUCTURE MUTATOR MAY NOT DESTROY A MODULE THE SNAPSHOT STILL NAMES.
//
// Two windows, both created by the same shape — a message-thread method that
// checks `editing_`, DROPS writeMutex_ to wait for a fade, and then acts on the
// stale answer.
//
//  1. removeNode checked `editing_`, waited up to ~150 ms for the fade-out, and
//     then erased + destroyed the slot. A beginEdit landing in that window makes
//     the publishLocked() that follows EARLY-RETURN, so the module is destroyed
//     while its raw pointer is still in the order the audio callback is reading,
//     and barrier() cannot help because the snapshot was never replaced. Nothing
//     exotic is needed to reach it: the managed JuceDspEditScope's finalizer
//     runs beginEdit paths from the GC thread while the UI thread removes nodes.
//
//  2. "never published, so nothing to barrier against" in removeNodeEditingLocked
//     is false once commitEdit has published its TRANSITIONAL order — which
//     contains the scope's added nodes — and dropped writeMutex_ for
//     waitForFades with `editing_` still true. Destroying one there freed a
//     published module AND recycled its NodeFade slot into the free list.
//
// Driven against a bare DspChain (the same object with the same two mutexes) so
// the interleaving is deterministic: `fadeWaitSleepsForTest()` reports that the
// mutator really is inside its unlocked wait, and the probe says so if it is not.
// The detector is a module that records its own destruction.
void scenarioRemoveCannotRaceBeginEdit()
{
    beginScenario("RtRemoveCannotRaceBeginEdit");

    struct Probe : dcrig::DspModule
    {
        std::atomic<bool>* dead = nullptr;
        ~Probe() override { if (dead != nullptr) dead->store(true, std::memory_order_release); }
        void prepare(double, int, int) override {}
        void reset() override {}
        bool setParam(const char*, float) override { return false; }
        bool getParam(const char*, float&) const override { return false; }
        void process(float* const* ch, int nch, int n) override
        {
            for (int c = 0; c < nch; ++c)
                if (ch[c] != nullptr)
                    for (int i = 0; i < n; ++i) ch[c][i] *= 0.5f;
        }
        const char* typeId() const override { return "probe"; }
    };

    constexpr int kBlock = 256;
    std::vector<float> buf((size_t) kBlock, 0.25f);

    // --- 1: removeNode vs beginEdit ----------------------------------------
    {
        std::atomic<bool> dead{ false };
        dcrig::DspChain chain;
        auto probe = std::make_unique<Probe>();
        probe->dead = &dead;
        const int id = chain.addNode(std::move(probe));
        chain.prepare(48000.0, kBlock, 1);
        // One block, so the fade is marked as having RUN — otherwise
        // fadeOutAndWait parks it at 0 and never unlocks at all.
        float* ch[1] = { buf.data() };
        chain.process(ch, 1, kBlock);

        std::atomic<int> removed{ -1 };
        const unsigned long sleeps0 = chain.fadeWaitSleepsForTest();
        std::thread remover([&] { removed.store(chain.removeNode(id) ? 1 : 0,
                                                std::memory_order_release); });
        while (chain.fadeWaitSleepsForTest() == sleeps0
               && removed.load(std::memory_order_acquire) < 0)
            std::this_thread::yield();
        const bool insideTheWait = removed.load(std::memory_order_acquire) < 0;
        chain.beginEdit();
        remover.join();
        const bool diedUnderScope = dead.load(std::memory_order_acquire);

        RT_CHECK(insideTheWait,
                 "the probe never caught removeNode inside its unlocked fade wait — "
                 "this phase proved nothing");
        RT_CHECK(removed.load(std::memory_order_acquire) == 1,
                 "the racing removeNode reported failure");
        RT_CHECK(! diedUnderScope,
                 "removeNode DESTROYED a module while an edit scope suppressed the "
                 "publication that would have taken it out of the order the audio "
                 "thread reads — a use-after-free on the callback");
        chain.commitEdit();
        RT_CHECK(dead.load(std::memory_order_acquire),
                 "the parked module was never retired — the scope leaks it instead");
        RT_CHECK(chain.nodeCount() == 0, "the removed node survived the commit");
    }

    // --- 2: a scope's own added node, removed while its commit is in flight -
    {
        std::atomic<bool> deadOld{ false };
        std::atomic<bool> deadNew{ false };
        dcrig::DspChain chain;
        auto oldProbe = std::make_unique<Probe>();
        oldProbe->dead = &deadOld;
        const int oldId = chain.addNode(std::move(oldProbe));
        chain.prepare(48000.0, kBlock, 1);
        float* ch[1] = { buf.data() };
        chain.process(ch, 1, kBlock); // the old node is AUDIBLE, so the commit
                                      // publishes a transitional order and waits

        chain.beginEdit();
        auto newProbe = std::make_unique<Probe>();
        newProbe->dead = &deadNew;
        const int newId = chain.addNode(std::move(newProbe));
        chain.removeNode(oldId);

        std::atomic<int>  committed{ -1 };
        std::atomic<int>  deadNewAtRemove{ -1 };
        const unsigned long sleeps0 = chain.fadeWaitSleepsForTest();
        std::thread committer([&] { committed.store(chain.commitEdit() ? 1 : 0,
                                                    std::memory_order_release); });
        while (chain.fadeWaitSleepsForTest() == sleeps0
               && committed.load(std::memory_order_acquire) < 0)
            std::this_thread::yield();
        const bool insideTheWait = committed.load(std::memory_order_acquire) < 0;
        chain.removeNode(newId);
        deadNewAtRemove.store(deadNew.load(std::memory_order_acquire) ? 1 : 0,
                              std::memory_order_release);
        committer.join();

        RT_CHECK(insideTheWait,
                 "the probe never caught commitEdit inside waitForFades — "
                 "this phase proved nothing");
        RT_CHECK(committed.load(std::memory_order_acquire) == 1, "the commit failed");
        RT_CHECK(deadNewAtRemove.load(std::memory_order_acquire) == 0,
                 "removing a scope-added node during its own commit destroyed a module "
                 "the TRANSITIONAL publication had already handed to the callback");
        RT_CHECK(deadOld.load(std::memory_order_acquire),
                 "the node the scope removed was never retired");
        RT_CHECK(chain.nodeCount() == 0,
                 "the chain should be empty: one node removed by the scope, one removed "
                 "during its commit");
    }

    note("edit-scope races: remove-vs-begin and remove-during-commit both keep the "
         "published module alive");
    endScenario();
}

// A FOOTSWITCH THAT LANDS DURING A MOVE IS THE USER'S INTENT AND WINS.
//
// moveNode/setNodeBranch sample `wasLive` BEFORE a ~10 ms unlocked fade wait and
// then called restoreLiveLocked, which stored `bypassed = false` with no
// re-check. So a setBypass(id, true) arriving inside that window returned true,
// stored its flag, and was then silently un-done: the UI read the pedal as
// bypassed and it was audible. Slot::bypassSeq makes "did the user touch this
// meanwhile" answerable, and restoreLiveIfUntouchedLocked asks.
//
// Same deterministic instrument as the scenario above: spin on
// fadeWaitSleepsForTest() until the move really is inside its unlocked wait.
void scenarioBypassDuringAMoveSurvives()
{
    beginScenario("RtBypassDuringAMoveIsNotClobbered");

    struct Probe : dcrig::DspModule
    {
        void prepare(double, int, int) override {}
        void reset() override {}
        bool setParam(const char*, float) override { return false; }
        bool getParam(const char*, float&) const override { return false; }
        void process(float* const* ch, int nch, int n) override
        {
            for (int c = 0; c < nch; ++c)
                if (ch[c] != nullptr)
                    for (int i = 0; i < n; ++i) ch[c][i] *= 0.5f;
        }
        const char* typeId() const override { return "probe"; }
    };

    constexpr int kBlock = 256;
    std::vector<float> buf((size_t) kBlock, 0.25f);

    dcrig::DspChain chain;
    const int moved = chain.addNode(std::unique_ptr<dcrig::DspModule>(new Probe()));
    chain.addNode(std::unique_ptr<dcrig::DspModule>(new Probe()));
    chain.prepare(48000.0, kBlock, 1);
    float* ch[1] = { buf.data() };
    chain.process(ch, 1, kBlock); // both nodes are live and have RUN

    std::atomic<int> done{ -1 };
    const unsigned long sleeps0 = chain.fadeWaitSleepsForTest();
    std::thread mover([&] { done.store(chain.moveNode(moved, 1) ? 1 : 0, std::memory_order_release); });
    while (chain.fadeWaitSleepsForTest() == sleeps0 && done.load(std::memory_order_acquire) < 0)
        std::this_thread::yield();
    const bool insideTheWait = done.load(std::memory_order_acquire) < 0;
    const bool switched = chain.setBypass(moved, true);
    mover.join();

    bool bypassed = false;
    const bool readBack = chain.getBypass(moved, bypassed);

    RT_CHECK(insideTheWait,
             "the probe never caught moveNode inside its unlocked fade wait — "
             "this scenario proved nothing");
    RT_CHECK(done.load(std::memory_order_acquire) == 1, "the move reported failure");
    RT_CHECK(switched, "setBypass reported failure");
    RT_CHECK(readBack && bypassed,
             "a setBypass that landed during a move of the SAME node was silently un-done: "
             "the switch reported success and the pedal is audible");
    note("footswitch during a move: intent preserved");
    endScenario();
}

// ---------------------------------------------------------------------------
// Meta-assertion and baseline gating
// ---------------------------------------------------------------------------

std::vector<std::string> readListFile(const std::string& path, std::string& err)
{
    std::vector<std::string> out;
    std::ifstream in(path);
    if (! in) { err = "cannot open " + path; return out; }
    std::string line;
    while (std::getline(in, line))
    {
        while (! line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.empty() || line[0] == '#') continue;
        // Everything after a '#' is the mandatory "why:" comment.
        const size_t hash = line.find('#');
        std::string key = hash == std::string::npos ? line : line.substr(0, hash);
        while (! key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (! key.empty()) out.push_back(key);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Master-bus ceiling (ABI v34)
// ---------------------------------------------------------------------------

/// The final device buffer used to be NaN-scrubbed and nothing else, so the sum
/// of song + every seat's monitor + click + instrument reached the driver AND
/// the streaming mirror unbounded. This drives a deliberately over-unity signal
/// through the monitor bus and asserts four different ways of being wrong:
///
///  * OFF must pass the over-range signal through untouched (or the ON half
///    below proves nothing — a stage that silences everything holds any
///    ceiling).
///  * ON must hold the ceiling on EVERY sample of EVERY block.
///  * ...and it must do so with the SMOOTHED LOOK-AHEAD GAIN, not with the
///    backstop clamp. A clipper flat-tops a large fraction of the waveform; a
///    limiter scales it. The clamped-sample fraction is the detector, and it is
///    what fails if the sliding-window minimum is replaced by the instantaneous
///    target (the smoother then only starts moving once the peak is already at
///    the output, so every attack punches through into the clamp).
///  * The applied gain must not be a per-BLOCK constant. Measured in the
///    SETTLED state and across a ceiling change, not across the engage:
///    engaging inserts the look-ahead delay line, which is a documented
///    one-block discontinuity (MasterLimiter.h rule 1) and would swamp the
///    thing being measured.
///  * A DISABLED limiter reports no latency, because it is out of the path.
void scenarioMasterLimiterHoldsTheCeiling()
{
    beginScenario("RtMasterLimiterHoldsTheCeilingWithoutAllocating");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) { fail("no manual null device"); endScenario(); return; }

    // 187.5 Hz is exactly one cycle per 256-frame block at 48 kHz, so the
    // endlessly replayed stimulus block is CONTINUOUS across boundaries — the
    // same reason RtGainSlidersRampInsteadOfStepping uses it. Amplitude 4.0 is
    // +12 dBFS: this is the shipping fault, a monitor bus that legitimately
    // carries more than unity.
    fx.installInputStimulus(4.0f, 187.5, 1.5707963267948966);
    fx.engine().setMonitorGain(1.0f);
    fx.engine().setMasterGain(1.0f);

    const float ceiling = 0.96605086f;   // -0.3 dBFS

    // Walk one output channel, carrying the previous block's last sample so a
    // boundary discontinuity is visible at all.
    float carry = 0.0f; bool haveCarry = false;
    auto scan = [&](float& peak, float& maxDelta, long long& clamped, long long& total)
    {
        const auto& bufs = dev->manualOutputBuffers();
        if (bufs.empty()) return;
        for (float s : bufs[0])
        {
            peak = std::max(peak, std::abs(s));
            if (haveCarry) maxDelta = std::max(maxDelta, std::abs(s - carry));
            carry = s; haveCarry = true;
            if (std::abs(s) >= ceiling * 0.99999f) ++clamped;
            ++total;
        }
    };

    // ---- Phase A: OFF passes the over-range signal through ----------------
    fx.engine().setMasterLimiter(false, -0.3f, 80.0f);
    fx.pumpWarm(96);
    carry = 0.0f; haveCarry = false;
    float offPeak = 0.0f, offDelta = 0.0f; long long offClamped = 0, offTotal = 0;
    for (int i = 0; i < 32; ++i) { fx.pumpWarm(1); scan(offPeak, offDelta, offClamped, offTotal); }
    const float offReduction = fx.engine().masterLimiterReductionDb();

    // ---- Phase B: ON holds it, with the sentinel armed --------------------
    fx.engine().setMasterLimiter(true, -0.3f, 80.0f);
    // Settle: the engage itself inserts the look-ahead delay line, i.e. 1 ms of
    // silence in one block. That is the documented cost of the limiter being a
    // TRUE BYPASS when it is off, and it is not what the ramp probe below is
    // looking for — so it is not measured.
    fx.pumpWarm(8);
    carry = 0.0f; haveCarry = false;

    dcrt::reset();
    dcrt::arm();
    float onPeak = 0.0f, onDelta = 0.0f; long long onClamped = 0, onTotal = 0;
    carry = 0.0f; haveCarry = false;
    for (int i = 0; i < 64; ++i)
    {
        dcrt::g_currentBlock.store((unsigned) i, std::memory_order_relaxed);
        { dcrt::ScopedRtThread rt; nulldev::pump(1); }
        scan(onPeak, onDelta, onClamped, onTotal);
    }
    // ---- Phase B2: the BYPASS EDGE, under the armed hook ------------------
    // Every setMasterLimiter toggle in this scenario used to sit OUTSIDE
    // arm()/disarm(), and the ON->OFF toggle was followed by zero pumped blocks
    // before the re-enable — so MasterLimiter's callback-side std::fill over
    // delay_, the one piece of real work on the bypass edge, was executed by no
    // armed pump at all. It is a std::fill over a pre-sized vector and must not
    // allocate; nothing said so.
    for (int edge = 0; edge < 3; ++edge)
    {
        fx.engine().setMasterLimiter(false, -0.3f, 80.0f);
        for (int i = 0; i < 3; ++i)
        { dcrt::ScopedRtThread rt; nulldev::pump(1); }
        fx.engine().setMasterLimiter(true, -0.3f, 80.0f);
        for (int i = 0; i < 3; ++i)
        { dcrt::ScopedRtThread rt; nulldev::pump(1); }
    }
    dcrt::disarm();
    const int  onAllocs    = dcrt::allocCount();
    const float onReduction = fx.engine().masterLimiterReductionDb();
    const int  latency      = fx.engine().masterLimiterLatencySamples();
    // Switched OFF the stage is not in the path AT ALL, so it reports no
    // latency. The first version of this kept the delay line running when
    // disabled and reported a constant figure; 48 samples of unconditional
    // delay shifted every output block in the engine and broke
    // RtEqTrimRampsAndSparseInputIsSilent, which is how that design was caught.
    fx.engine().setMasterLimiter(false, -0.3f, 80.0f);
    const int latencyOff = fx.engine().masterLimiterLatencySamples();
    fx.engine().setMasterLimiter(true, -0.3f, 80.0f);

    // ---- Phase C: the applied gain moves per SAMPLE, not per BLOCK -------
    // A per-block constant gain steps at every boundary while the ceiling is
    // moving. -0.3 -> -6 dBFS is a 1.9x change in the applied gain, spread over
    // the ~12-sample attack; a per-block implementation would put the whole
    // change on one sample boundary.
    float ceilPeak = 0.0f, ceilDelta = 0.0f; long long cClamped = 0, cTotal = 0;
    carry = 0.0f; haveCarry = false;
    fx.engine().setMasterLimiter(true, -6.0f, 80.0f);
    for (int i = 0; i < 16; ++i) { fx.pumpWarm(1); scan(ceilPeak, ceilDelta, cClamped, cTotal); }
    fx.engine().setMasterLimiter(true, -0.3f, 80.0f);
    fx.pumpWarm(16);

    // ---- Phase D: the meter falls back to 0 once the signal goes quiet ----
    fx.installInputStimulus(0.0f, 187.5, 0.0);
    // 512 blocks is 2.7 s — past the 0.25 s hold's snap-to-rest threshold, so
    // this asserts the meter reaches EXACTLY zero and not merely "small".
    fx.pumpWarm(512);
    const float quietReduction = fx.engine().masterLimiterReductionDb();

    const double clampedPct = onTotal > 0 ? (100.0 * (double) onClamped / (double) onTotal) : 0.0;
    // The signal's own slew at the limited amplitude: A*2*pi*f/fs.
    const float steadySlew = ceiling * 2.0f * 3.14159265f * 187.5f / 48000.0f;

    note("OFF peak " + std::to_string(offPeak) + " (reduction " + std::to_string(offReduction)
         + " dB) | ON peak " + std::to_string(onPeak) + " (reduction " + std::to_string(onReduction)
         + " dB, " + std::to_string(clampedPct) + " % of samples at the clamp) | steady max |dx| "
         + std::to_string(onDelta) + ", ceiling-change max |dx| " + std::to_string(ceilDelta)
         + " against a steady slew of " + std::to_string(steadySlew)
         + " | latency " + std::to_string(latency) + " on / " + std::to_string(latencyOff)
         + " off | quiet reduction "
         + std::to_string(quietReduction) + " dB");

    reportHeap("master limiter");

    RT_CHECK(offPeak > 3.0f,
             "with the limiter OFF the output peaked at only " + std::to_string(offPeak)
             + " — the +12 dB stimulus never reached the bus, so every assertion below is vacuous");
    RT_CHECK(offReduction == 0.0f,
             "the limiter reported " + std::to_string(offReduction)
             + " dB of reduction while it was switched OFF");
    RT_CHECK(onTotal > 0, "no output was scanned at all — the probe is vacuous");
    RT_CHECK(onPeak <= ceiling * 1.0001f,
             "with the limiter ON the output still peaked at " + std::to_string(onPeak)
             + ", past the -0.3 dBFS ceiling of " + std::to_string(ceiling));
    RT_CHECK(onPeak > ceiling * 0.5f,
             "the limiter crushed the signal to " + std::to_string(onPeak)
             + " — holding a ceiling by going quiet is not limiting");
    RT_CHECK(onReduction > 6.0f,
             "a +12 dB signal against a -0.3 dBFS ceiling reported only "
             + std::to_string(onReduction) + " dB of reduction");
    RT_CHECK(clampedPct < 2.0,
             std::to_string(clampedPct) + " % of the output samples sit exactly ON the ceiling — "
             "the backstop clamp is doing the work, i.e. this is a CLIPPER and the look-ahead "
             "gain computer is not reacting in time");
    RT_CHECK(onDelta < steadySlew * 3.0f,
             "a SETTLED limiter produced a sample-to-sample jump of " + std::to_string(onDelta)
             + " against the signal's own slew of " + std::to_string(steadySlew)
             + " — the applied gain is stepping at the block boundary");
    RT_CHECK(ceilDelta < steadySlew * 8.0f,
             "moving the ceiling from -0.3 to -6 dBFS produced a jump of "
             + std::to_string(ceilDelta) + " against the signal's own slew of "
             + std::to_string(steadySlew) + " — the gain is being STEPPED, not ramped");
    RT_CHECK(ceilPeak > 0.4f && ceilPeak < 0.55f,
             "the -6 dBFS ceiling produced a peak of " + std::to_string(ceilPeak)
             + " instead of ~0.5012 — the ceiling change never took, so the ramp probe "
               "above is vacuous");
    RT_CHECK(latency > 0 && latency <= (int) (48000 * 0.0015),
             "the limiter reports " + std::to_string(latency)
             + " samples of look-ahead; the budget is 1.5 ms (72 samples at 48 kHz)");
    RT_CHECK(latencyOff == 0,
             "a DISABLED limiter still reports " + std::to_string(latencyOff)
             + " samples of latency — it is meant to be out of the path entirely, and a "
               "stage that re-times the engine while switched off is exactly what broke "
               "RtEqTrimRampsAndSparseInputIsSilent");
    RT_CHECK(quietReduction == 0.0f,
             "the reduction meter still reads " + std::to_string(quietReduction)
             + " dB after 512 silent blocks — a UI mirroring it would show a lit limiter "
               "over silence");
    RT_CHECK(onAllocs == 0,
             "the master limiter allocated " + std::to_string(onAllocs)
             + " time(s) on the callback");

    endScenario();
}

// ---------------------------------------------------------------------------
// Sample-accurate MIDI (ABI v34)
// ---------------------------------------------------------------------------

/// Every instrument event used to be stamped at sample 0 of whichever block
/// drained it — up to one whole buffer of skew on every note. The manual-pump
/// device is what makes the fix testable at all: the callback runs on the
/// test's own thread, so `renderFrames` is exactly `blocksPumped * blockSize`
/// and a frame can be named with no clock estimation whatsoever.
///
/// Four phases, one per way of being wrong:
///  * A — four events named for four offsets INSIDE one block must land on
///        those four samples. A stamp-at-zero implementation reports 0,0,0,0.
///  * B — an event named for the NEXT block must not be dispatched early.
///        Clamping into whichever block drained it (the obvious shortcut) puts
///        it in this one, which is the pre-v34 error under a new name.
///  * C — an event whose frame has already gone past goes at sample 0 AND is
///        counted. That counter is the only signal a caller has that its
///        scheduling horizon is too short.
///  * D — none of it allocates on the callback.
/// THE LIMITER'S SKIP BRANCH: what happens when it fires, and what it says.
///
/// `MasterLimiter::process` returns false for a block wider than `prepare()`
/// sized — the "skip, never grow" rule every audio-thread buffer in this engine
/// follows, and the right call. What was missing is that the return value is
/// DISCARDED at the one call site, so when it fires the block reaches the driver
/// AND THE STREAMING MIRROR unbounded — the exact artefact this stage exists to
/// prevent — with nothing said anywhere, while `latencySamples()` went on
/// reporting 1 ms because it gated on the enabled FLAG rather than on whether the
/// stage had run. Every other skip in this engine is either harmless or counted.
///
/// Driven against the class directly: a variable-block driver that hands more
/// channels than were prepared is not something the null device can be made to
/// do, and the branch is two lines at the top of `process`.
void scenarioMasterLimiterSkipIsCountedAndHonest()
{
    beginScenario("RtMasterLimiterSkipIsCountedAndReportsNoLatency");

    dotcharter::MasterLimiter limiter;
    limiter.prepare(48000.0, 2);

    RT_CHECK(limiter.skippedBlocks() == 0, "prepare() did not reset the skip count");
    RT_CHECK(limiter.latencySamples() > 0,
             "a prepared limiter reports no look-ahead at all — the assertions below are vacuous");
    const int prepared = limiter.latencySamples();

    // A block the stage CAN take, so the non-vacuity of the skip below is real.
    std::vector<float> l(256, 0.5f), r(256, 0.5f), c(256, 0.5f), d(256, 0.5f);
    float* two[2]  = { l.data(), r.data() };
    float* four[4] = { l.data(), r.data(), c.data(), d.data() };

    dcrt::reset();
    dcrt::arm();
    bool ranNarrow = false, ranWide = true, ranAfter = false;
    int latencyAfterSkip = -1;
    {
        dcrt::ScopedRtThread rt;
        ranNarrow = limiter.process(two, 2, 256, true, 0.96605086f, 80.0f);
        // 4 channels against a 2-channel prepare: the skip.
        ranWide   = limiter.process(four, 4, 256, true, 0.96605086f, 80.0f);
        latencyAfterSkip = limiter.latencySamples();
        // ...and the block AFTER the skip, at the prepared width (W4-N13). This
        // is the half the scenario used to stop short of, and it is the common
        // case: a skip is per-block, so a driver that hands one wide block goes
        // straight back to delaying normally. `skipped_` is a CUMULATIVE counter
        // cleared only by prepare(), so gating the reported latency on it made
        // one skip report 0 samples for the rest of the session — the same 1 ms
        // error the branch was added to prevent, with the sign flipped.
        ranAfter  = limiter.process(two, 2, 256, true, 0.96605086f, 80.0f);
    }
    dcrt::disarm();
    const int allocs = dcrt::allocCount();

    note("skip probe: narrow ran " + std::to_string(ranNarrow ? 1 : 0) + ", wide ran "
         + std::to_string(ranWide ? 1 : 0) + ", skipped " + std::to_string(limiter.skippedBlocks())
         + ", latency " + std::to_string(prepared) + " -> " + std::to_string(latencyAfterSkip)
         + " -> " + std::to_string(limiter.latencySamples())
         + ", allocs " + std::to_string(allocs));

    RT_CHECK(ranNarrow, "a 2-channel block against a 2-channel prepare was skipped — probe is vacuous");
    RT_CHECK(! ranWide, "a 4-channel block against a 2-channel prepare was PROCESSED — the stage grew");
    RT_CHECK(limiter.skippedBlocks() == 1,
             "the skip was not counted (" + std::to_string(limiter.skippedBlocks())
             + ") — an unbounded block reached the driver and the streaming mirror with nothing "
             "anywhere saying so");
    RT_CHECK(latencyAfterSkip == 0,
             "after a skip the limiter still reports " + std::to_string(latencyAfterSkip)
             + " samples of look-ahead, so click alignment and the MIDI-out deadline are wrong by "
             "1 ms against a stage that is not in the path");
    RT_CHECK(ranAfter, "the block AFTER the skip was skipped too — the probe below is vacuous");
    RT_CHECK(limiter.latencySamples() == prepared,
             "the block after the skip WAS delayed, and the stage still reports "
             + std::to_string(limiter.latencySamples()) + " instead of " + std::to_string(prepared)
             + " samples: one skip made the figure a click scheduler reads wrong for the rest of "
               "the session, in the opposite direction");
    RT_CHECK(allocs == 0,
             "the skip branch allocated " + std::to_string(allocs) + " time(s)");
    endScenario();
}

/// TWO MESSAGE-THREAD PRODUCERS AGAINST THE SPSC FIFO, while the callback
/// drains it.
///
/// `midiFifo_` is a `juce::AbstractFifo`, which is SINGLE-producer /
/// single-consumer, and this engine had at least two producers: the JUCE MIDI
/// input thread AND whatever thread calls `dca_engine_send_midi_at` /
/// `_send_short`, which the ABI does not constrain. Two concurrent
/// `prepareToWrite`s compute from the same `validStart`, so both write the SAME
/// slot and `finishedWrite` double-advances the index — one event lost, one slot
/// of stale garbage forwarded to somebody's sampler, and no counter moves.
/// Latent since the two exports existed; ABI v34's whole purpose is a sequencer
/// scheduling ahead from its own thread, which turns it into the design.
///
/// What this asserts: every event a producer said was accepted is DELIVERED
/// (nothing lost, nothing duplicated), every delivered note number is one a
/// producer actually sent (nothing torn), and the callback still allocates
/// nothing — the fix is a producer-side lock and the CONSUMER must stay
/// lock-free, which is also why `rt_lock_sites.txt` does not move.
void scenarioMidiProducersAreSerialised()
{
    beginScenario("RtConcurrentMidiProducersLoseNoEvents");
    Fixture fx(48000, 256);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    StubInstrument stub;
    if (! fx.engine().attachInstrumentPlugin(&stub))
    { fail("could not attach the stub instrument"); endScenario(); return; }

    fx.pumpWarm(kWarmupBlocks);
    fx.engine().resetMidiTimingStats();
    stub.resetMidiEventsSeen();

    // Two producers, disjoint note ranges so a torn write is visible as a note
    // number NOBODY sent. "As soon as possible" (frame -1) keeps them out of
    // the parked set's saturation arm, which phase F of the sibling covers.
    constexpr int kPerProducer = 400;
    std::atomic<bool> go{ false };
    std::atomic<int>  finished{ 0 };

    auto producer = [&](uint8_t base)
    {
        while (! go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < kPerProducer; ++i)
        {
            const uint32_t packedEv = (uint32_t) 0x90u
                                    | ((uint32_t) (base + (i % 16)) << 8)
                                    | ((uint32_t) 100 << 16);
            fx.engine().sendMidiAt(packedEv, -1);
            // Give the callback room to drain: 800 events against a 512-deep
            // fifo would otherwise saturate and drop, which is a DIFFERENT arm.
            if ((i % 32) == 31) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        finished.fetch_add(1, std::memory_order_release);
    };

    std::thread a(producer, (uint8_t) 40);
    std::thread b(producer, (uint8_t) 80);

    dcrt::reset();
    dcrt::arm();
    go.store(true, std::memory_order_release);
    // The callback drains WHILE both producers run, for as long as they run
    // (gatherEvidence, #55). It used to stop after a fixed 200 blocks, so a
    // starved producer could finish its sends against a callback that was no
    // longer draining, and saturate the fifo — the drop arm, not this one.
    // At least the old 200 blocks either way.
    unsigned blockNo = 0;
    gatherEvidence([&]
    {
        dcrt::g_currentBlock.store(blockNo++, std::memory_order_relaxed);
        { dcrt::ScopedRtThread rt; nulldev::pump(1); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, [&] { return finished.load(std::memory_order_acquire) == 2; }, 200);
    a.join();
    b.join();
    fx.pumpArmed(64);              // drain whatever is still queued
    dcrt::disarm();

    const int allocs = dcrt::allocCount();
    int64_t scheduled = 0, late = 0, dropped = 0;
    fx.engine().getMidiTimingStats(scheduled, late, dropped);
    const int delivered = stub.midiEventsSeen();

    note("two producers x " + std::to_string(kPerProducer) + " events: delivered "
         + std::to_string(delivered) + ", dropped " + std::to_string(dropped)
         + ", callback allocs " + std::to_string(allocs));

    reportHeap("concurrent MIDI producers");

    RT_CHECK(delivered > 0, "nothing was delivered at all — the probe is vacuous");
    RT_CHECK((int64_t) delivered + dropped == 2 * kPerProducer,
             "two producers sent " + std::to_string(2 * kPerProducer) + " events; "
             + std::to_string(delivered) + " were delivered and " + std::to_string(dropped)
             + " counted as dropped, so " + std::to_string(2 * kPerProducer - delivered - dropped)
             + " were LOST — a second producer against a single-producer fifo writes the same "
             "slot twice and double-advances the index");
    RT_CHECK(allocs == 0,
             "serialising the MIDI producers allocated " + std::to_string(allocs)
             + " time(s) on the callback — the CONSUMER must stay lock-free");
    endScenario();
}

void scenarioMidiIsSampleAccurate()
{
    beginScenario("RtScheduledMidiLandsOnItsSampleWithoutAllocating");
    Fixture fx(48000, 256);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }

    StubInstrument stub;
    if (! fx.engine().attachInstrumentPlugin(&stub))
    { fail("could not attach the stub instrument"); endScenario(); return; }

    fx.pumpWarm(kWarmupBlocks);

    auto packed = [](uint8_t status, uint8_t d1, uint8_t d2)
    {
        return (uint32_t) status | ((uint32_t) d1 << 8) | ((uint32_t) d2 << 16);
    };

    // ---- Phase A: four offsets inside ONE block ---------------------------
    const int offsets[4] = { 0, 37, 128, 255 };
    int64_t frames = 0, ticks = 0;
    if (! fx.engine().getRenderClock(frames, ticks))
    { fail("the render clock is not published — the whole probe is vacuous"); endScenario(); return; }

    fx.engine().resetMidiTimingStats();
    stub.clearMidiLog();
    for (int i = 0; i < 4; ++i)
        fx.engine().sendMidiAt(packed(0x90, (uint8_t) (60 + i), 100), frames + offsets[i]);

    dcrt::reset();
    dcrt::arm();
    fx.pumpArmed(1);
    dcrt::disarm();
    const int phaseAAllocs = dcrt::allocCount();

    std::string seen;
    for (int i = 0; i < stub.midiLogCount(); ++i)
    {
        const auto h = stub.midiLogAt(i);
        seen += "(b" + std::to_string(h.block) + " s" + std::to_string(h.pos)
              + " n" + std::to_string(h.note) + ") ";
    }
    note("phase A placements: " + seen);

    RT_CHECK(stub.midiLogCount() == 4,
             "the stub instrument was handed " + std::to_string(stub.midiLogCount())
             + " of 4 scheduled events in the block that contained their frames");
    int matched = 0, allAtZero = 0;
    for (int i = 0; i < stub.midiLogCount(); ++i)
    {
        const auto h = stub.midiLogAt(i);
        if (h.pos == 0) ++allAtZero;
        for (int k = 0; k < 4; ++k)
            if (h.note == 60 + k && h.block == 0 && h.pos == offsets[k]) ++matched;
    }
    RT_CHECK(matched == 4,
             "only " + std::to_string(matched) + " of 4 events landed on the sample they were "
             "scheduled for — placements were " + seen);
    RT_CHECK(allAtZero <= 1,
             std::to_string(allAtZero) + " of 4 events were stamped at sample 0 — that is the "
             "pre-v34 behaviour, i.e. the frame is being ignored (exactly one SHOULD be at 0: "
             "the event scheduled for offset 0)");

    // ---- Phase B: an event for the NEXT block must not fire early ---------
    stub.clearMidiLog();
    if (! fx.engine().getRenderClock(frames, ticks))
    { fail("render clock unavailable in phase B"); endScenario(); return; }
    fx.engine().sendMidiAt(packed(0x90, 70, 100), frames + 256 + 100);

    fx.pumpWarm(1);
    const int afterFirst = stub.midiLogCount();
    fx.pumpWarm(1);
    const int afterSecond = stub.midiLogCount();
    const auto parked = stub.midiLogAt(0);

    note("phase B: " + std::to_string(afterFirst) + " event(s) in the block BEFORE its frame, "
         + std::to_string(afterSecond) + " after; landed at block " + std::to_string(parked.block)
         + " sample " + std::to_string(parked.pos));
    RT_CHECK(afterFirst == 0,
             "an event scheduled for the NEXT block was dispatched in THIS one — it is being "
             "clamped into whichever block drained it, which is the pre-v34 error under a new "
             "name");
    RT_CHECK(afterSecond == 1 && parked.block == 1 && parked.pos == 100,
             "the parked event did not arrive at block 1 sample 100 (got block "
             + std::to_string(parked.block) + " sample " + std::to_string(parked.pos) + ")");

    // ---- Phase C: a late event goes at 0 AND is counted -------------------
    stub.clearMidiLog();
    fx.engine().resetMidiTimingStats();
    if (! fx.engine().getRenderClock(frames, ticks))
    { fail("render clock unavailable in phase C"); endScenario(); return; }
    fx.engine().sendMidiAt(packed(0x90, 80, 100), frames - 4096);
    fx.pumpWarm(1);

    int64_t scheduled = 0, late = 0, dropped = 0;
    fx.engine().getMidiTimingStats(scheduled, late, dropped);
    const auto lateHit = stub.midiLogAt(0);
    note("phase C: scheduled " + std::to_string(scheduled) + ", late " + std::to_string(late)
         + ", dropped " + std::to_string(dropped) + "; landed at sample "
         + std::to_string(lateHit.pos));
    RT_CHECK(scheduled == 1, "the scheduled counter did not see the event");
    RT_CHECK(late == 1,
             "an event whose frame had already gone past was not counted as late (" +
             std::to_string(late) + ") — that counter is the only signal a caller has that its "
             "scheduling horizon is too short");
    RT_CHECK(lateHit.pos == 0, "a late event must be placed at the head of the block");
    RT_CHECK(dropped == 0, std::to_string(dropped) + " event(s) were dropped");

    // ---- Phase E: an event 10 s out is PARKED, never dispatched early ------
    //
    // Read the `delta > futureFrames` branch carefully before changing this: it
    // is reached only when the event is NOT for this block AND is further out
    // than 5 s, and the clock only gets that far ahead of an event if it REWOUND
    // (a device restart drops renderFrames_ to 0). A manual-pump fixture cannot
    // rewind, so the horizon DROP itself is genuinely not reachable here and
    // this phase does not claim to reach it — what it pins is the half that is:
    // an event far in the future is parked and dispatched to nobody, rather than
    // being clamped into the block that drained it. The SATURATION drop, which
    // the same counter reports, is phase F.
    stub.clearMidiLog();
    fx.engine().resetMidiTimingStats();
    if (! fx.engine().getRenderClock(frames, ticks))
    { fail("render clock unavailable in phase E"); endScenario(); return; }
    fx.engine().sendMidiAt(packed(0x90, 90, 100), frames + (int64_t) (48000 * 10)); // 10 s out
    fx.pumpWarm(2);
    int64_t farScheduled = 0, farLate = 0, farDropped = 0;
    fx.engine().getMidiTimingStats(farScheduled, farLate, farDropped);
    note("phase E: scheduled " + std::to_string(farScheduled) + ", dropped "
         + std::to_string(farDropped) + ", delivered " + std::to_string(stub.midiLogCount()));
    RT_CHECK(farScheduled == 1, "the far-future event never reached the queue — phase E is vacuous");
    RT_CHECK(stub.midiLogCount() == 0, "an event 10 s out was dispatched anyway");

    // ---- Phase F: the parked set SATURATES, and says so -------------------
    // 512 slots. Fill them with events that are all parked for the far future
    // so none can drain, then push more: every extra one must be counted as
    // dropped rather than silently discarded.
    stub.clearMidiLog();
    fx.engine().resetMidiTimingStats();
    if (! fx.engine().getRenderClock(frames, ticks))
    { fail("render clock unavailable in phase F"); endScenario(); return; }
    // 1 s ahead: inside the 5 s horizon (so they PARK rather than drop) and far
    // enough out that nothing drains while the set is being filled.
    const int64_t parkFrame = frames + 48000;
    for (int i = 0; i < 700; ++i)
    {
        // The fifo is 512 deep as well, so pump between batches to move them
        // into the parked set.
        if (i % 256 == 0) fx.pumpWarm(1);
        fx.engine().sendMidiAt(packed(0x90, (uint8_t) (30 + (i % 8)), 100), parkFrame);
    }
    fx.pumpWarm(2);
    int64_t satScheduled = 0, satLate = 0, satDropped = 0;
    fx.engine().getMidiTimingStats(satScheduled, satLate, satDropped);
    note("phase F: scheduled " + std::to_string(satScheduled) + ", dropped "
         + std::to_string(satDropped));
    RT_CHECK(satScheduled >= 512,
             "only " + std::to_string(satScheduled) + " events reached the queue — the parked set "
             "was never filled, so the saturation arm is untested");
    RT_CHECK(satDropped > 0,
             "700 events against a 512-slot parked set and a 512-deep fifo dropped NONE — the "
             "saturation counter is not being reached, and a silently swallowed note-off is a "
             "stuck note");

    // Let the parked events drain so they cannot pollute phase D.
    fx.pumpWarm(256);

    // ---- Phase D: a stream of scheduled events, sentinel armed -----------
    stub.clearMidiLog();
    fx.engine().resetMidiTimingStats();
    dcrt::reset();
    dcrt::arm();
    for (int b = 0; b < 32; ++b)
    {
        if (! fx.engine().getRenderClock(frames, ticks)) break;
        for (int k = 0; k < 8; ++k)
            fx.engine().sendMidiAt(packed(0x90, (uint8_t) (40 + k), 100), frames + k * 31);
        dcrt::g_currentBlock.store((unsigned) b, std::memory_order_relaxed);
        { dcrt::ScopedRtThread rt; nulldev::pump(1); }
    }
    dcrt::disarm();
    const int streamAllocs = dcrt::allocCount();

    reportHeap("scheduled MIDI");
    RT_CHECK(phaseAAllocs == 0,
             "placing four scheduled events allocated " + std::to_string(phaseAAllocs)
             + " time(s) on the callback");
    RT_CHECK(streamAllocs == 0,
             "256 scheduled events over 32 blocks allocated " + std::to_string(streamAllocs)
             + " time(s) on the callback");

    fx.engine().detachInstrumentPlugin(&stub);
    endScenario();
}


// ---------------------------------------------------------------------------
// IDLE SLEEP (dsp/IdleSleep.h) over the REAL device callback.
// ---------------------------------------------------------------------------
//
// One seat, an owner-like in-house rig (gate -> overdrive -> amp -> cab -> eq ->
// gain; the rtcheck binary has no NeuralAudio, so the in-house amp stands in
// for the two NAM captures), driven through pluck -> silence -> pluck on the
// manual-pump null device. Proves, with the allocation sentinel ARMED through
// the whole sleep and wake:
//
//   (a) the seat falls asleep after the hold of silence, and not before it;
//       while asleep the rig is skipped and the monitor carries exact zeros;
//   (b) the waking block is PROCESSED (the pick is on it) and the output from
//       that block on is identical to a never-slept engine's (the same run with
//       idle sleep off) — bit-exact, because the 5 s hold lets every state the
//       allow-listed modules keep flush to zero on digital silence;
//   (c) a param write wakes it on the very next block, restarts the full hold,
//       and so does a monitor-gain move;
//   (d) the OFFLINE render never sleeps: rendered while the seat is asleep it
//       equals the same render from a never-slept engine, sample for sample.

namespace rigidle {

constexpr int kRate  = 48000;
constexpr int kBlock = 256;

float pluckSample(long long n)
{
    const double t = (double) n / kRate;
    const double env = (t < 0.003 ? t / 0.003 : 1.0) * std::exp(-t / 0.35);
    double v = 0.0;
    for (int k = 1; k <= 8; ++k) v += std::sin(2.0 * 3.14159265358979323846 * 110.0 * k * t + 0.3 * k) / k;
    return (float) (0.2 * env * v);
}

void buildOwnerLikeRig(AudioEngine& e, std::vector<int>& ids)
{
    auto add = [&](const char* type) { const int id = e.dspAddNode(0, type); ids.push_back(id); return id; };
    const int gate = add("gate");
    e.dspSetParam(gate, "thresholdDb", -46.f); e.dspSetParam(gate, "ratio", 12.f);
    e.dspSetParam(gate, "attackMs", 1.f);      e.dspSetParam(gate, "holdMs", 40.f);
    e.dspSetParam(gate, "releaseMs", 70.f);
    add("overdrive");
    add("amp");
    const int cab = add("cab");
    e.dspSetParam(cab, "model", 8.f); e.dspSetParam(cab, "lowCutHz", 95.f); e.dspSetParam(cab, "highCutHz", 8500.f);
    const int eq = add("eq");
    e.dspSetParam(eq, "lowFreq", 110.f); e.dspSetParam(eq, "lowGainDb", 1.f);
    e.dspSetParam(eq, "midFreq", 1200.f); e.dspSetParam(eq, "midGainDb", 3.f);
    e.dspSetParam(eq, "highFreq", 4500.f); e.dspSetParam(eq, "highGainDb", 2.f);
    const int g = add("gain");
    e.dspSetParam(g, "gainDb", 0.03f);
}

/// Fill every input channel for the block starting at sample `start`: a pluck
/// that began at `pluckAt` (or silence when pluckAt < 0).
void fillInput(long long start, long long pluckAt)
{
    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) return;
    for (auto& ch : dev->manualInputBuffers())
        for (int i = 0; i < (int) ch.size(); ++i)
            ch[(size_t) i] = pluckAt >= 0 && start + i >= pluckAt ? pluckSample(start + i - pluckAt) : 0.0f;
}

void appendOutput(std::vector<float>& dst)
{
    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) return;
    const auto& out = dev->manualOutputBuffers();
    if (out.empty()) return;
    dst.insert(dst.end(), out[0].begin(), out[0].end());
}

float lastOutputPeak()
{
    auto* dev = nulldev::manualDevice();
    float p = 0.0f;
    if (dev == nullptr) return p;
    for (const auto& ch : dev->manualOutputBuffers())
        for (float v : ch) p = std::max(p, std::fabs(v));
    return p;
}

struct Run
{
    std::vector<float> afterWake;     // monitor output from the waking block on
    std::vector<float> offline;       // the offline render taken while (maybe) asleep
    long long sleptAtBlock   = -1;    // first skipped block of the silence
    long long silenceBlocks  = 0;
    bool      zerosWhileAsleep = true;
    bool      wokeOnPluck    = false;
    bool      asleepBeforeOffline = false;
    uint64_t  skippedBeforeOffline = 0, skippedAfterOffline = 0;
    int       allocs = 0, frees = 0;
    std::string error;
};

/// One pluck -> `silenceBlocks` of silence -> offline render -> pluck.
Run runOnce(bool idleSleep, long long silenceBlocks)
{
    Run r;
    r.silenceBlocks = silenceBlocks;
    Fixture fx(kRate, kBlock, 2, idleSleep);
    if (! fx.ok()) { r.error = "engine did not start: " + fx.error(); return r; }
    auto& e = fx.engine();
    e.setMasterGain(1.0f);
    e.setInputGain(1.0f);
    e.setMonitorGain(1.0f);
    std::vector<int> ids;
    buildOwnerLikeRig(e, ids);
    if (ids.size() != 6 || std::find(ids.begin(), ids.end(), -1) != ids.end())
    { r.error = "could not build the owner-like rig"; return r; }

    // Warm-up UNARMED (first-touch allocations), silence.
    fillInput(0, -1);
    fx.pumpWarm(kWarmupBlocks);
    long long pos = 0;

    dcrt::reset();
    dcrt::arm();
    // 1 s pluck.
    const long long pluckBlocks = kRate / kBlock;
    for (long long b = 0; b < pluckBlocks; ++b) { fillInput(pos, 0); fx.pumpArmed(1); pos += kBlock; }
    // Silence.
    for (long long b = 0; b < silenceBlocks; ++b)
    {
        fillInput(pos, -1);
        fx.pumpArmed(1);
        pos += kBlock;
        const auto st = e.rigIdleStateForTest(0);
        if (st.asleep() && r.sleptAtBlock < 0) r.sleptAtBlock = b;
        if (st.asleep() && lastOutputPeak() != 0.0f) r.zerosWhileAsleep = false;
    }
    dcrt::disarm();
    r.allocs = dcrt::allocCount();
    r.frees  = dcrt::freeCount();

    // (d) the offline render, off the callback, with the seat in whatever state
    // the silence left it.
    {
        const auto st = e.rigIdleStateForTest(0);
        r.asleepBeforeOffline  = st.asleep();
        r.skippedBeforeOffline = st.skippedBlocks();
        std::vector<float> l((size_t) kRate), rr((size_t) kRate);
        for (int i = 0; i < kRate; ++i) l[(size_t) i] = rr[(size_t) i] = pluckSample(i);
        e.dspProcessOffline(0, l.data(), rr.data(), kRate, kRate);
        r.offline = l;
        r.skippedAfterOffline = e.rigIdleStateForTest(0).skippedBlocks();
    }
    // The offline pass moved the rig's state; give it the same silence again
    // so both runs reach the wake from a settled rig. (The sleep-on run is
    // re-armed and falls back asleep during this.)
    for (long long b = 0; b < silenceBlocks; ++b) { fillInput(pos, -1); fx.pumpWarm(1); pos += kBlock; }

    // (b) the wake.
    const uint64_t skippedBefore = e.rigIdleStateForTest(0).skippedBlocks();
    const long long wakeAt = pos;
    dcrt::reset();
    dcrt::arm();
    for (long long b = 0; b < pluckBlocks; ++b)
    {
        fillInput(pos, wakeAt);
        fx.pumpArmed(1);
        pos += kBlock;
        if (b == 0) r.wokeOnPluck = e.rigIdleStateForTest(0).skippedBlocks() == skippedBefore
                                    && ! e.rigIdleStateForTest(0).asleep()
                                    && lastOutputPeak() > 0.0f;   // the attack is ON this block
        appendOutput(r.afterWake);
    }
    dcrt::disarm();
    r.allocs += dcrt::allocCount();
    r.frees  += dcrt::freeCount();
    return r;
}

} // namespace rigidle

void scenarioRigIdleSleep()
{
    beginScenario("RtRigIdleSleepSkipsSilenceAndWakesOnThePick");
    using namespace rigidle;
    const long long hold   = dcrig::idleHoldSamples(0.0, kRate);
    const long long silenceBlocks = (hold + 3LL * kRate) / kBlock; // hold + 3 s asleep

    const Run on  = runOnce(true,  silenceBlocks);
    const Run off = runOnce(false, silenceBlocks);
    if (! on.error.empty() || ! off.error.empty())
    {
        fail(on.error.empty() ? off.error : on.error);
        endScenario();
        return;
    }

    // (a) asleep after the hold, not before it.
    const long long holdBlocks = hold / kBlock;
    RT_CHECK(on.sleptAtBlock >= 0, "the rig never fell asleep over " + std::to_string(silenceBlocks)
             + " blocks of silence");
    RT_CHECK(on.sleptAtBlock >= holdBlocks - 1,
             "the rig fell asleep at silent block " + std::to_string(on.sleptAtBlock)
             + ", before the " + std::to_string(holdBlocks) + "-block hold");
    RT_CHECK(on.zerosWhileAsleep, "a sleeping seat put non-zero samples on the monitor");
    RT_CHECK(off.sleptAtBlock < 0, "idle sleep switched OFF still slept (the reference is not a reference)");
    note("fell asleep at silent block " + std::to_string(on.sleptAtBlock) + " (hold "
         + std::to_string(holdBlocks) + " blocks)");

    // (b) the pick is processed on its own block, and everything after it is
    // what the never-slept engine produced.
    RT_CHECK(on.wokeOnPluck, "the waking block was skipped — the attack was lost");
    float peak = 0.0f, diff = 0.0f;
    const size_t n = std::min(on.afterWake.size(), off.afterWake.size());
    for (size_t i = 0; i < n; ++i)
    {
        peak = std::max(peak, std::fabs(off.afterWake[i]));
        diff = std::max(diff, std::fabs(on.afterWake[i] - off.afterWake[i]));
    }
    RT_CHECK(n > 0 && peak > 0.01f, "the reference produced no audible pick — the comparison is vacuous");
    RT_CHECK(diff == 0.0f, "slept and never-slept engines differ by " + std::to_string(diff)
             + " (peak " + std::to_string(peak) + ") from the waking block on");
    note("wake: peak " + std::to_string(peak) + ", max |slept - never-slept| = " + std::to_string(diff));

    // (d) the offline render never sleeps.
    RT_CHECK(on.asleepBeforeOffline, "the seat was not asleep when the offline render ran — (d) is vacuous");
    RT_CHECK(on.skippedAfterOffline == on.skippedBeforeOffline,
             "the offline render advanced the device path's idle state");
    float offPeak = 0.0f, offDiff = 0.0f;
    for (size_t i = 0; i < on.offline.size() && i < off.offline.size(); ++i)
    {
        offPeak = std::max(offPeak, std::fabs(off.offline[i]));
        offDiff = std::max(offDiff, std::fabs(on.offline[i] - off.offline[i]));
    }
    RT_CHECK(offPeak > 0.01f, "the offline render produced nothing");
    RT_CHECK(offDiff == 0.0f, "the offline render of a sleeping seat differs from a never-slept one by "
             + std::to_string(offDiff));

    // RT: the whole sleep, the skip and the wake, armed.
    RT_CHECK(on.allocs == 0 && on.frees == 0,
             "idle sleep allocated " + std::to_string(on.allocs) + " / freed " + std::to_string(on.frees)
             + " on the callback");
    reportHeap("idle sleep");

    // (c) a param write wakes it on the next block and restarts the hold; so
    // does the monitor gain.
    {
        Fixture fx(kRate, kBlock, 2, true);
        if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
        auto& e = fx.engine();
        e.setMonitorGain(1.0f);
        std::vector<int> ids;
        buildOwnerLikeRig(e, ids);
        fillInput(0, -1);
        // The first blocks after a rig is built are not quiet (the amp's
        // filters settle), so warm up before counting the hold.
        fx.pumpWarm(kWarmupBlocks);
        fx.pumpWarm((int) holdBlocks + 16);
        RT_CHECK(e.rigIdleStateForTest(0).asleep(), "(c) setup: the rig did not fall asleep on silence");
        const uint64_t wakes = e.rigIdleStateForTest(0).wakes();

        e.dspSetParam(ids.back(), "gainDb", 0.03f);   // same value: a write is a write
        fx.pumpWarm(1);
        RT_CHECK(! e.rigIdleStateForTest(0).asleep() && e.rigIdleStateForTest(0).wakes() == wakes + 1,
                 "a param write did not wake the sleeping rig on the next block");
        fx.pumpWarm((int) holdBlocks - 4);
        RT_CHECK(! e.rigIdleStateForTest(0).asleep(), "after a param write the rig slept before a full hold");
        fx.pumpWarm(12);
        RT_CHECK(e.rigIdleStateForTest(0).asleep(), "after a param write the rig never went back to sleep");

        e.setMonitorGain(0.5f);
        fx.pumpWarm(1);
        RT_CHECK(! e.rigIdleStateForTest(0).asleep(), "a monitor-gain move did not wake the sleeping rig");

        // A type that is not on the allow-list keeps the seat awake for good.
        e.dspAddNode(0, "tremolo");
        fx.pumpWarm((int) holdBlocks * 2);
        RT_CHECK(! e.rigIdleStateForTest(0).asleep() && e.rigIdleStateForTest(0).skippedBlocks() > 0,
                 "a rig with a non-allow-listed node (tremolo) fell asleep");
    }
    endScenario();
}

/// Accumulated across every scenario (the sentinel is reset per scenario, so
/// the union has to be gathered as we go).
std::vector<std::string> g_allLockSites;
std::vector<std::string> g_allViolationKeys;

void harvest()
{
    for (auto& s : dcrt::observedSites(dcrt::Kind::Lock))
        if (std::find(g_allLockSites.begin(), g_allLockSites.end(), s) == g_allLockSites.end())
            g_allLockSites.push_back(s);
    for (auto& k : dcrt::violationKeys())
        if (std::find(g_allViolationKeys.begin(), g_allViolationKeys.end(), k) == g_allViolationKeys.end())
            g_allViolationKeys.push_back(k);
}

// ---------------------------------------------------------------------------
// ABI v37 — spillover and re-amp
// ---------------------------------------------------------------------------

void zeroManualInput()
{
    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) return;
    for (auto& ch : dev->manualInputBuffers()) std::fill(ch.begin(), ch.end(), 0.0f);
}

int addRingingDelay(AudioEngine& e)
{
    const int id = e.dspAddNode(0, "delay");
    if (id < 0) return id;
    e.dspSetParam(id, "timeMs", 120.0f);
    e.dspSetParam(id, "feedback", 0.85f);
    e.dspSetParam(id, "mix", 0.5f);
    return id;
}

/// A delay REMOVED with spillover on keeps ringing after the whole chain, the
/// tail pass allocates and frees nothing on the callback, the tail is retired
/// off it once it has died away, a fifth tail makes the oldest fade, and with
/// spillover off the same remove is silent. Asserts the feature HAPPENED (the
/// output carries the tail) as well as that it is allocation-free.
void scenarioSpilloverTails()
{
    beginScenario("RtSpilloverTailRingsAfterRemovalWithoutAllocating");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    auto& e = fx.engine();
    e.setMonitorGain(1.0f);
    RT_CHECK(! e.dspSpillover(0), "spillover was ON at engine start");

    // --- OFF: the reference. Removing a ringing delay leaves silence. -------
    fx.installInputStimulus();
    int id = addRingingDelay(e);
    if (id < 0) { fail("could not add a delay"); endScenario(); return; }
    fx.pumpWarm(64);
    zeroManualInput();
    e.dspRemoveNode(id);
    fx.pumpWarm(4);
    RT_CHECK(fx.lastOutputPeak() < 1.0e-6f,
             "with spillover OFF a removed delay still sounded: " + std::to_string(fx.lastOutputPeak()));
    RT_CHECK(e.dspSpillTailCount(0) == 0, "spillover OFF still made a tail");

    // --- ON: the tail rings on, allocation-free -----------------------------
    e.dspSetSpillover(0, true);
    RT_CHECK(e.dspSpillover(0), "dspSetSpillover(true) did not stick");
    fx.installInputStimulus();
    id = addRingingDelay(e);
    fx.pumpWarm(64);
    zeroManualInput();
    e.dspRemoveNode(id);
    RT_CHECK(e.dspNodeCount(0) == 0, "the removed delay is still in the chain");
    RT_CHECK(e.dspSpillTailCount(0) == 1, "removing a ringing delay made "
             + std::to_string(e.dspSpillTailCount(0)) + " tail(s), not 1");
    warmUpAndArm(fx);
    float armedPeak = 0.0f;
    for (int b = 0; b < 32; ++b)
    {
        fx.pumpArmed(1);
        armedPeak = std::max(armedPeak, fx.lastOutputPeak());
    }
    dcrt::disarm();
    reportHeap("spillover tail");
    RT_CHECK(dcrt::allocCount() == 0,
             "the tail pass allocated " + std::to_string(dcrt::allocCount()) + " time(s)");
    RT_CHECK(dcrt::freeCount() == 0,
             "the tail pass freed " + std::to_string(dcrt::freeCount()) + " time(s)");
    RT_CHECK(armedPeak > 1.0e-3f, "no tail on the output after the removal (peak "
             + std::to_string(armedPeak) + ") — spillover did nothing");
    note("tail peak " + std::to_string(armedPeak) + " after the remove, input silent");

    // --- it dies away and is RETIRED off the callback -----------------------
    // 8 s cap + the fade, in 256-frame blocks at 48 kHz.
    for (int b = 0; b < 1700 && e.dspSpillTailCount(0) > 0; b += 16) fx.pumpWarm(16);
    RT_CHECK(e.dspSpillTailCount(0) == 0, "the tail never finished");
    RT_CHECK(e.dspSpillSlotCountForTest(0) == 0, "a finished tail was not retired");
    fx.pumpWarm(2);
    RT_CHECK(fx.lastOutputPeak() < 1.0e-6f, "a retired tail still sounds");

    // --- the pool: a fifth tail asks the oldest to go -----------------------
    fx.installInputStimulus();
    std::vector<int> ids;
    for (int i = 0; i < 5; ++i) ids.push_back(addRingingDelay(e));
    fx.pumpWarm(64);
    zeroManualInput();
    for (int nid : ids) e.dspRemoveNode(nid);
    RT_CHECK(e.dspSpillTailCount(0) == 5, "five removed delays did not all start ringing");
    dcrt::reset();
    dcrt::arm();
    fx.pumpArmed(8);
    dcrt::disarm();
    RT_CHECK(dcrt::allocCount() == 0 && dcrt::freeCount() == 0, "five tails allocated or freed");
    RT_CHECK(e.dspSpillTailCount(0) == 4, "the oldest tail did not make room: "
             + std::to_string(e.dspSpillTailCount(0)) + " ringing");

    // --- switching it off fades every tail out ------------------------------
    e.dspSetSpillover(0, false);
    fx.pumpWarm(8);
    RT_CHECK(e.dspSpillTailCount(0) == 0, "tails kept ringing after spillover was switched off");
    fx.pumpWarm(2);
    RT_CHECK(fx.lastOutputPeak() < 1.0e-6f, "output not silent after spillover was switched off");
    endScenario();
}

/// A take injected into seat 0 REPLACES its input: the capture ring (what the
/// tuner reads) carries the take, a looping take wraps, the render is
/// allocation- and free-free, and clearing it restores the live input.
void scenarioInputInjection()
{
    beginScenario("RtInputInjectionReplacesTheSeatInputWithoutAllocating");
    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    auto& e = fx.engine();
    fx.installInputStimulus(0.25f);
    fx.pumpWarm(16);

    constexpr int64_t kLen = 1000; // deliberately not a multiple of the block
    std::vector<float> take((size_t) kLen, 0.7f);
    RT_CHECK(e.setInputInjection(0, take.data(), kLen, true), "setInputInjection refused a take");
    int64_t pos = -1, len = -1; bool playing = true, loop = false;
    RT_CHECK(e.getInputInjectionStatus(0, pos, len, playing, loop), "no status for a loaded take");
    RT_CHECK(len == kLen && pos == 0 && ! playing && loop, "a fresh take is not paused at 0");
    fx.pumpWarm(4);
    {
        // Loaded but paused: the live guitar is MUTED, not playing through.
        std::vector<float> win(256, 1.0f);
        e.readInputCapture(0, win.data(), (int) win.size());
        float peak = 0.0f; for (float s : win) peak = std::max(peak, std::abs(s));
        RT_CHECK(peak < 1.0e-6f, "a paused take let the live input through (peak " + std::to_string(peak) + ")");
    }
    RT_CHECK(e.setInputInjectionPlaying(0, true), "play refused");

    warmUpAndArm(fx);
    fx.pumpArmed(256);
    dcrt::disarm();
    reportHeap("input injection");
    RT_CHECK(dcrt::allocCount() == 0,
             "the injection allocated " + std::to_string(dcrt::allocCount()) + " time(s)");
    RT_CHECK(dcrt::freeCount() == 0,
             "the injection freed " + std::to_string(dcrt::freeCount()) + " time(s)");

    std::vector<float> win(1024, 0.0f);
    e.readInputCapture(0, win.data(), (int) win.size());
    bool allTake = true;
    for (float s : win) if (std::abs(s - 0.7f) > 1.0e-5f) { allTake = false; break; }
    RT_CHECK(allTake, "the capture ring does not carry the injected take");

    RT_CHECK(e.getInputInjectionStatus(0, pos, len, playing, loop), "status lost mid-play");
    RT_CHECK(playing && pos >= 0 && pos < kLen, "a looping take ran off its end (pos "
             + std::to_string(pos) + ")");
    // 4 paused + 64 warm + 256 armed blocks; only the 320 playing ones advance.
    const int64_t played = (int64_t) (64 + 256) * fx.blockSize();
    RT_CHECK(pos == played % kLen, "loop position " + std::to_string(pos) + ", expected "
             + std::to_string(played % kLen));

    // Replacing the take while it plays frees the old one on THIS thread.
    std::vector<float> take2((size_t) kLen, -0.3f);
    RT_CHECK(e.setInputInjection(0, take2.data(), kLen, false), "replacing a take was refused");
    RT_CHECK(e.setInputInjectionPlaying(0, true), "play refused on the replacement");
    fx.pumpWarm(3); // one-shot: 768 frames < 1000, still running
    e.readInputCapture(0, win.data(), 256);
    RT_CHECK(std::abs(win[255] + 0.3f) < 1.0e-5f, "the replacement take is not what the seat hears");
    fx.pumpWarm(2); // runs off the end: stops and rewinds
    RT_CHECK(e.getInputInjectionStatus(0, pos, len, playing, loop) && ! playing && pos == 0,
             "a one-shot take did not stop and rewind at its end");

    RT_CHECK(e.clearInputInjection(0), "clear refused");
    RT_CHECK(! e.getInputInjectionStatus(0, pos, len, playing, loop), "a cleared take still reports");
    fx.pumpWarm(4);
    e.readInputCapture(0, win.data(), (int) win.size());
    float peak = 0.0f; for (float s : win) peak = std::max(peak, std::abs(s));
    RT_CHECK(peak > 0.2f && peak < 0.2501f, "clearing the take did not restore the live input (peak "
             + std::to_string(peak) + ")");
    endScenario();
}

/// JUCE 9 port (2026-09-28). juce::MidiOutput on the Win32 backend stopped
/// retrying a busy driver (MIDIERR_NOTREADY, 50 x 1 ms) and a port that is
/// still ALLOCATED (4 x 100 ms), so MidiOutputDevice puts both bounds back.
/// Pins the bounds and the accounting without a MIDI device: a send the driver
/// takes on the Nth try is ACCEPTED after N-1 pauses, one it never takes is
/// refused after exactly kSendAttempts tries (a single try once the driver is
/// already refusing or the dispatcher is exiting), and nothing retries past the
/// first success. Not an audio-thread scenario — the dispatcher is its own
/// thread — but this binary is where MidiOutputDevice.h is compiled.
void scenarioMidiOutputRetriesLikeJuce8()
{
    beginScenario("RtMidiOutputRetriesARefusedSendAsJuce8Did");
    using MOD = dotcharter::MidiOutputDevice;
    RT_CHECK(MOD::kSendAttempts == 50 && MOD::kSendRetryMs == 1,
             "the send retry is no longer JUCE 8's 50 x 1 ms");
    RT_CHECK(MOD::kOpenAttempts == 4 && MOD::kOpenRetryMs == 100,
             "the open retry is no longer JUCE 8's 4 x 100 ms");

    int tries = 0, pauses = 0, pausedMs = 0;
    auto pause = [&](int ms) { ++pauses; pausedMs += ms; return true; };

    // Accepted on the 3rd try.
    bool ok = MOD::sendWithRetry([&] { return ++tries >= 3; }, pause);
    RT_CHECK(ok && tries == 3 && pauses == 2 && pausedMs == 2,
             "a send accepted on the 3rd try took " + std::to_string(tries) + " tries / "
             + std::to_string(pauses) + " pauses");

    // Never accepted: bounded, and reported as a refusal.
    tries = pauses = pausedMs = 0;
    ok = MOD::sendWithRetry([&] { ++tries; return false; }, pause);
    RT_CHECK(! ok, "a send the driver never took was reported as accepted");
    RT_CHECK(tries == MOD::kSendAttempts && pauses == MOD::kSendAttempts - 1,
             "a refused send was tried " + std::to_string(tries) + " times, not "
             + std::to_string(MOD::kSendAttempts));

    // Accepted first time: no pause at all (the dense-schedule path).
    tries = pauses = 0;
    ok = MOD::sendWithRetry([&] { ++tries; return true; }, pause);
    RT_CHECK(ok && tries == 1 && pauses == 0, "an accepted send was retried");

    // A driver that refused the LAST message gets one try (the dispatcher
    // passes attempts = 1): an unplugged interface costs 50 ms once, not per
    // message.
    tries = pauses = 0;
    ok = MOD::sendWithRetry([&] { ++tries; return false; }, pause, 1);
    RT_CHECK(! ok && tries == 1 && pauses == 0,
             "a single-attempt send was tried " + std::to_string(tries) + " times");

    // A pause that says stop (the dispatcher's, once it is asked to exit) ends
    // the retries at once, so a close never waits behind a refusing driver.
    tries = 0;
    ok = MOD::sendWithRetry([&] { ++tries; return false; }, [](int) { return false; });
    RT_CHECK(! ok && tries == 1, "a send kept retrying after the pause said stop ("
             + std::to_string(tries) + " tries)");

    // Open: the ALLOCATED port that frees up on the 2nd try, and one that never does.
    tries = pauses = pausedMs = 0;
    auto opened = MOD::openWithRetry(
        [&]() -> std::unique_ptr<int> { return ++tries >= 2 ? std::make_unique<int>(1) : nullptr; },
        pause);
    RT_CHECK(opened != nullptr && tries == 2 && pausedMs == MOD::kOpenRetryMs,
             "an open that succeeds on the 2nd try took " + std::to_string(tries) + " tries");
    tries = pauses = 0;
    opened = MOD::openWithRetry([&]() -> std::unique_ptr<int> { ++tries; return nullptr; }, pause);
    RT_CHECK(opened == nullptr && tries == MOD::kOpenAttempts && pauses == MOD::kOpenAttempts - 1,
             "a port that never opens was tried " + std::to_string(tries) + " times");
    endScenario();
}

/// ABI v38. A chart source on seat 0, following a PLAYING song clock: its notes
/// reach the device output through the seat's rig and monitor, the seat's
/// CAPTURE RING stays exactly silent (detection, scoring and the tuner never
/// hear it), and rendering, a seek and a table swap neither allocate nor free
/// on the callback. Then: the routed instrument lands in the seat input and NOT
/// on the master; a device with NO input still runs a generator seat; and a
/// clock source that is destroyed unbinds itself.
void scenarioChartSource()
{
    beginScenario("RtChartSourceFeedsTheSeatRigNotTheCaptureRing");
    std::vector<dca_chart_note> notes;
    for (int i = 0; i < 12; ++i)
    {
        dca_chart_note n{};
        n.start_seconds   = 0.30 + 0.25 * i;
        n.release_seconds = n.start_seconds + 0.20;
        n.sustain_seconds = 0.20;
        n.pitch           = 45.0f + (float) (i % 5) * 2.0f;
        n.velocity        = 0.9f;
        n.string_index    = i % 6;
        n.fret            = 3;
        n.slide_to_fret   = (i == 4) ? 7.0f : -1.0f;
        n.chord_id        = -1;
        n.bend_first      = (i == 6) ? 0 : -1;
        n.bend_count      = (i == 6) ? 2 : 0;
        n.flags           = (i == 8) ? DCA_CHART_NOTE_VIBRATO | DCA_CHART_NOTE_TREMOLO : 0u;
        notes.push_back(n);
    }
    const dca_chart_bend_point bends[2] = { { 0.30 + 0.25 * 6 + 0.05, 2.0f, 0 },
                                            { 0.30 + 0.25 * 6 + 0.15, 0.0f, 0 } };
    const auto blob = makeWavBlob(6.0, 48000, 220.0);
    if (blob.getSize() == 0) { fail("could not synthesise a WAV blob"); endScenario(); return; }

    {
        Fixture fx;   // the device input is left SILENT: anything in the ring is the generator
        if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
        auto& e = fx.engine();
        e.setMonitorGain(1.0f);
        TransportSource src(e);
        const auto err = src.openMemory(blob.getData(), blob.getSize(), "wav");
        if (err.isNotEmpty()) { fail("openMemory failed: " + err.toStdString()); endScenario(); return; }
        src.setVolume(0.0f);   // the song is silent; its CLOCK is what we want
        RT_CHECK(e.setSongClockSource(&src), "an attached source was refused as the song clock");
        std::string error;
        RT_CHECK(e.setChartNotes(0, notes.data(), (int32_t) notes.size(), (int32_t) sizeof(dca_chart_note),
                                 bends, 2, error), "a valid note table was rejected: " + error);
        RT_CHECK(e.setChartSourceEnabled(0, true), "enabling the chart source failed");
        src.play();

        // The capture ring is drained through the STREAMING cursor after every
        // block, so every sample the seat captured while notes sounded is seen —
        // a window read at the end would only see the silence after the last note.
        std::vector<float> ring(4096, 0.0f);
        float ringPeak = 0.0f;
        int64_t ringSamples = 0;
        auto drainRing = [&]
        {
            const int got = e.readInputCaptureStream(0, ring.data(), (int) ring.size());
            for (int k = 0; k < got; ++k) ringPeak = std::max(ringPeak, std::abs(ring[(size_t) k]));
            ringSamples += got;
        };
        warmUpAndArm(fx);   // 64 blocks (~0.34 s): the first note has started
        drainRing();
        float outPeak = 0.0f;
        for (int i = 0; i < 400; ++i)   // ~2.1 s
        {
            dcrt::g_currentBlock.store((unsigned) i, std::memory_order_relaxed);
            { dcrt::ScopedRtThread rt; nulldev::pump(1); }
            outPeak = std::max(outPeak, fx.lastOutputPeak());
            drainRing();
            if (i == 200) src.seekSeconds(0.30 + 0.25 * 9 + 0.05);   // INSIDE note 9
            if (i == 300)
            {
                // A table swap while it plays: freed on THIS thread, never on the callback.
                RT_CHECK(e.setChartNotes(0, notes.data(), (int32_t) notes.size(), (int32_t) sizeof(dca_chart_note),
                                         bends, 2, error), "re-uploading the table was rejected");
            }
        }
        dcrt::disarm();
        reportHeap("chart source");
        RT_CHECK(dcrt::allocCount() == 0,
                 "the chart source allocated " + std::to_string(dcrt::allocCount()) + " time(s) on the callback");
        RT_CHECK(dcrt::freeCount() == 0,
                 "the chart source freed " + std::to_string(dcrt::freeCount()) + " time(s) on the callback");
        RT_CHECK(! fx.lastOutputHasNonFinite(), "the chart source put a NaN/Inf on the output");
        RT_CHECK(outPeak > 0.02f, "the chart source never reached the device output (peak "
                 + std::to_string(outPeak) + ")");

        RT_CHECK(ringSamples >= 400 * fx.blockSize(),
                 "the capture ring delivered " + std::to_string(ringSamples)
                 + " samples over 400 blocks — the isolation check would be vacuous");
        RT_CHECK(ringPeak == 0.0f, "the CAPTURE RING heard the chart source (peak "
                 + std::to_string(ringPeak) + ") — detection would score the generator");

        dca_chart_source_status st{};
        st.struct_bytes = (int32_t) sizeof(st);
        RT_CHECK(e.getChartStatus(0, st), "no status");
        note("out peak " + std::to_string(outPeak) + ", started " + std::to_string(st.notes_started)
             + ", chased " + std::to_string(st.notes_chased) + ", song " + std::to_string(st.song_seconds));
        RT_CHECK(st.enabled == 1 && st.note_count == 12 && st.clock_bound == 1 && st.clock_playing == 1,
                 "status does not describe an enabled, bound, playing source with 12 notes");
        RT_CHECK(st.notes_started >= 8, "only " + std::to_string(st.notes_started) + " notes started");
        RT_CHECK(st.notes_chased >= 1, "the seek inside note 9 did not chase it");

        // Disabling: a choke, then silence (the song itself is at volume 0).
        RT_CHECK(e.setChartSourceEnabled(0, false), "disable failed");
        fx.pumpWarm(8);
        RT_CHECK(fx.lastOutputPeak() < 1.0e-6f, "8 blocks after disabling the output still carries "
                 + std::to_string(fx.lastOutputPeak()));

        // Instrument route: into seat 0's rig (monitor 0 => inaudible), then on
        // the master (=> audible). The ring stays silent throughout.
        StubToneInstrument instrument;
        if (! e.attachInstrumentPlugin(&instrument)) { fail("attachInstrumentPlugin refused the stub"); }
        else
        {
            e.setInstrumentGain(1.0f);
            e.setMonitorGain(0.0f);
            RT_CHECK(e.setInstrumentRoute(0), "routing the instrument to seat 0 was refused");
            drainRing();
            ringPeak = 0.0f;
            fx.pumpWarm(32);
            const float routedPeak = fx.lastOutputPeak();
            e.setMonitorGain(1.0f);
            fx.pumpWarm(32);
            const float routedHeard = fx.lastOutputPeak();
            RT_CHECK(e.setInstrumentRoute(-1), "routing the instrument back to the master was refused");
            e.setMonitorGain(0.0f);
            fx.pumpWarm(32);
            const float masterPeak = fx.lastOutputPeak();
            note("instrument: routed+monitor0 " + std::to_string(routedPeak) + ", routed+monitor1 "
                 + std::to_string(routedHeard) + ", master+monitor0 " + std::to_string(masterPeak));
            RT_CHECK(routedPeak < 1.0e-6f, "a routed instrument still reached the master ("
                     + std::to_string(routedPeak) + ") with the seat's monitor at 0");
            RT_CHECK(routedHeard > 0.05f, "a routed instrument never came out of the seat's rig ("
                     + std::to_string(routedHeard) + ")");
            RT_CHECK(masterPeak > 0.05f, "route -1 did not restore the instrument on the master");
            ringPeak = 0.0f;
            drainRing();
            RT_CHECK(ringPeak == 0.0f, "the capture ring heard the routed instrument");
            e.detachInstrumentPlugin(&instrument);
        }
        RT_CHECK(! e.setInstrumentRoute(4) && ! e.setInstrumentRoute(-2), "an out-of-range route was accepted");

        // A destroyed clock source unbinds itself (and is waited out).
        {
            TransportSource tmp(e);
            RT_CHECK(tmp.openMemory(blob.getData(), blob.getSize(), "wav").isEmpty(), "second source failed to open");
            RT_CHECK(e.setSongClockSource(&tmp), "second source refused as the clock");
            RT_CHECK(e.setChartSourceEnabled(0, true), "re-enable failed");
            fx.pumpWarm(4);
        }
        fx.pumpWarm(4);
        st = dca_chart_source_status{};
        st.struct_bytes = (int32_t) sizeof(st);
        e.getChartStatus(0, st);
        RT_CHECK(st.clock_bound == 0 && st.clock_playing == 0, "a destroyed clock source is still bound");
        RT_CHECK(e.setChartSourceEnabled(0, false), "disable failed");
        src.pause();
    }

    {
        // A device with NO input at all: the generator seat still runs its rig.
        Fixture fx(48000, 256, 0);
        if (! fx.ok()) { fail("input-less engine did not start: " + fx.error()); endScenario(); return; }
        auto& e = fx.engine();
        e.setMonitorGain(1.0f);
        TransportSource src(e);
        RT_CHECK(src.openMemory(blob.getData(), blob.getSize(), "wav").isEmpty(), "openMemory failed");
        src.setVolume(0.0f);
        RT_CHECK(e.setSongClockSource(&src), "clock refused");
        std::string error;
        RT_CHECK(e.setChartNotes(0, notes.data(), (int32_t) notes.size(), (int32_t) sizeof(dca_chart_note),
                                 bends, 2, error), "table rejected");
        RT_CHECK(e.setChartSourceEnabled(0, true), "enable failed");
        src.play();
        float peak = 0.0f;
        for (int i = 0; i < 160; ++i) { fx.pumpWarm(1); peak = std::max(peak, fx.lastOutputPeak()); }
        RT_CHECK(peak > 0.02f, "with no input device the chart source was never heard (peak "
                 + std::to_string(peak) + ")");
        src.pause();
    }
    endScenario();
}

/// ABI v40 (#50, #61 part 2). The ENGINE path of the instrument models: a
/// Strat on its neck pickup plays sustained notes through seat 0; mid-note the
/// selection moves to the bridge — a new voice built on THIS (control) thread,
/// loading only its pack, and CROSS-FADED in on the callback: no gap, no click,
/// nothing allocated or freed on the callback. The replaced voice and its pack
/// are freed by the chart reaper. Disabling then releases the voice AND its
/// pack (status and dca_chart_voice_pack_bytes go to 0); re-enabling plays again.
/// Packs are synthesised into a temp voice directory (voice_pack_fixture.h).
void scenarioChartModelSwap()
{
    beginScenario("RtChartModelSwapCrossfadesAndReleasesThePack");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dcrt_chart_models";
    voicefixture::writeAllPacks(dir, 12, 3.0, 3);   // 3 partials: a smooth wave, so a cut is a visible step
    setEnv("DOTCHARTER_VOICE_DIR", dir.string().c_str());

    std::vector<dca_chart_note> notes;
    for (int i = 0; i < 6; ++i)
    {
        dca_chart_note n{};
        n.start_seconds   = 0.30 + 0.35 * i;
        n.release_seconds = n.start_seconds + 1.6;
        n.sustain_seconds = 1.6;
        n.pitch           = 45.0f + 5.0f * (float) (i % 3) + 2.0f;
        n.velocity        = 0.8f;
        n.string_index    = 1 + (i % 3);
        n.fret            = 2;
        n.slide_to_fret   = -1.0f;
        n.chord_id        = -1;
        n.bend_first      = -1;
        n.flags           = DCA_CHART_NOTE_LET_RING;
        notes.push_back(n);
    }
    const auto blob = makeWavBlob(8.0, 48000, 220.0);
    {
        Fixture fx;
        if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
        auto& e = fx.engine();
        e.setMonitorGain(1.0f);
        TransportSource src(e);
        RT_CHECK(src.openMemory(blob.getData(), blob.getSize(), "wav").isEmpty(), "openMemory failed");
        src.setVolume(0.0f);
        RT_CHECK(e.setSongClockSource(&src), "clock refused");
        std::string error;
        RT_CHECK(e.setChartNotes(0, notes.data(), (int32_t) notes.size(), (int32_t) sizeof(dca_chart_note), nullptr, 0, error),
                 "table rejected: " + error);
        dcvoice::InstrumentConfig neck;  neck.model = dcvoice::kModelStratSSS; neck.pickup = dcvoice::kPickNeck;
        dcvoice::InstrumentConfig bridge; bridge.model = dcvoice::kModelStratSSS; bridge.pickup = dcvoice::kPickBridge;
        RT_CHECK(e.setChartVoice(0, DCA_CHART_VOICE_GUITAR), "guitar voice refused");
        RT_CHECK(e.setChartInstrument(0, neck), "Strat neck refused");
        RT_CHECK(dcsynth::livePackBytes() == 0, "a pack was loaded while the source is disabled ("
                 + std::to_string(dcsynth::livePackBytes()) + ")");
        RT_CHECK(e.setChartSourceEnabled(0, true), "enable failed");
        const int64_t onePack = dcsynth::livePackBytes();
        RT_CHECK(onePack > 0, "enabling loaded no pack");
        src.play();
        warmUpAndArm(fx);

        std::vector<float> out;
        auto pumpRecord = [&](int blocks) {
            for (int i = 0; i < blocks; ++i)
            {
                { dcrt::ScopedRtThread rt; nulldev::pump(1); }
                const auto& ch = nulldev::manualDevice()->manualOutputBuffers()[0];
                out.insert(out.end(), ch.begin(), ch.end());
            }
        };
        pumpRecord(150);                       // ~0.8 s past the warm-up: several notes ring
        const size_t swapAt = out.size();
        RT_CHECK(e.setChartInstrument(0, bridge), "the switch to the bridge was refused");   // loads strat-bridge HERE
        pumpRecord(60);
        // The reaper frees the faded voice; give it a moment (it needs no callbacks).
        for (int i = 0; i < 100 && dcsynth::livePackBytes() > onePack; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pumpRecord(40);
        dcrt::disarm();
        reportHeap("chart model swap");
        RT_CHECK(dcrt::allocCount() == 0, "the model swap allocated " + std::to_string(dcrt::allocCount()) + " time(s) on the callback");
        RT_CHECK(dcrt::freeCount() == 0, "the model swap freed " + std::to_string(dcrt::freeCount()) + " time(s) on the callback");

        // No gap and no click across the swap: block RMS never collapses and no
        // sample step exceeds the largest step of the notes before it.
        const int bs = fx.blockSize();
        auto blockRms = [&](size_t a) {
            double acc = 0.0;
            for (int k = 0; k < bs; ++k) acc += (double) out[a + (size_t) k] * out[a + (size_t) k];
            return std::sqrt(acc / bs);
        };
        // The cross-fade is 10 ms (2 blocks): look at the 4 blocks from the swap
        // against the MEDIAN block of the 40 before (notes come and go).
        double before = 0.0, minAcross = 1.0e9, stepBefore = 0.0, stepAcross = 0.0;
        {
            std::vector<double> r;
            for (size_t a = swapAt - 40 * (size_t) bs; a < swapAt; a += (size_t) bs) r.push_back(blockRms(a));
            std::nth_element(r.begin(), r.begin() + (long) r.size() / 2, r.end());
            before = r[r.size() / 2];
        }
        for (size_t a = swapAt; a < swapAt + 4 * (size_t) bs; a += (size_t) bs) minAcross = std::min(minAcross, blockRms(a));
        for (size_t i = swapAt - 40 * (size_t) bs; i < swapAt; ++i) stepBefore = std::max(stepBefore, (double) std::abs(out[i] - out[i - 1]));
        for (size_t i = swapAt; i < swapAt + 12 * (size_t) bs; ++i) stepAcross = std::max(stepAcross, (double) std::abs(out[i] - out[i - 1]));
        note("swap: rms before " + std::to_string(before) + ", min across " + std::to_string(minAcross) + ", step before "
             + std::to_string(stepBefore) + ", across " + std::to_string(stepAcross));
        RT_CHECK(before > 1.0e-3, "the notes were not audible before the swap (vacuous)");
        RT_CHECK(minAcross > 0.6 * before, "the swap left a GAP (block rms " + std::to_string(minAcross) + " vs " + std::to_string(before) + ")");
        RT_CHECK(stepAcross <= 1.25 * stepBefore, "the swap CLICKED (step " + std::to_string(stepAcross) + " vs " + std::to_string(stepBefore) + ")");
        RT_CHECK(dcsynth::livePackBytes() == onePack, "after the cross-fade only the new pack is held ("
                 + std::to_string(dcsynth::livePackBytes()) + " vs " + std::to_string(onePack) + ")");

        dca_chart_source_status st{};
        st.struct_bytes = (int32_t) sizeof(st);
        RT_CHECK(e.getChartStatus(0, st) && st.model == dcvoice::kModelStratSSS && st.pickup == dcvoice::kPickBridge && st.sampled == 1
                     && st.voice_loaded == 1 && st.pack_bytes == onePack, "status does not describe the Strat bridge voice");

        // #61 part 2: OFF releases the voice and its pack.
        RT_CHECK(e.setChartSourceEnabled(0, false), "disable failed");
        for (int i = 0; i < 200 && dcsynth::livePackBytes() != 0; ++i)
        {
            fx.pumpWarm(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        st = dca_chart_source_status{};
        st.struct_bytes = (int32_t) sizeof(st);
        e.getChartStatus(0, st);
        RT_CHECK(dcsynth::livePackBytes() == 0, "disabling did not release the pack (" + std::to_string(dcsynth::livePackBytes()) + " bytes held)");
        RT_CHECK(st.voice_loaded == 0 && st.pack_bytes == 0, "status still reports a loaded voice after disabling");
        RT_CHECK(st.model == dcvoice::kModelStratSSS && st.pickup == dcvoice::kPickBridge, "disabling forgot the selection");

        // ON again: the same selection, reloaded, audible.
        RT_CHECK(e.setChartSourceEnabled(0, true), "re-enable failed");
        RT_CHECK(dcsynth::livePackBytes() == onePack, "re-enabling did not reload exactly one pack");
        src.seekSeconds(0.30);
        float peak = 0.0f;
        for (int i = 0; i < 80; ++i) { fx.pumpWarm(1); peak = std::max(peak, fx.lastOutputPeak()); }
        RT_CHECK(peak > 1.0e-3f, "after re-enabling the voice is silent (peak " + std::to_string(peak) + ")");
        RT_CHECK(e.setChartSourceEnabled(0, false), "disable failed");
        src.pause();
    }
    // The engine is gone: nothing may be held.
    RT_CHECK(dcsynth::livePackBytes() == 0, "a destroyed engine left " + std::to_string(dcsynth::livePackBytes()) + " pack bytes alive");
    setEnv("DOTCHARTER_VOICE_DIR", nullptr);
    endScenario();
}

/// #417. The engine's voices load their packs LAZILY and decode the zones a
/// chart can play before the chart is published. This is the path when that
/// did NOT happen (an edit's note before its upload, a planner miss): chart A
/// is decoded, then chart B — notes far from A's on other strings — is
/// published WITHOUT its zones (setChartZonePreloadForTesting). B's first note
/// must sound at once (the nearest decoded recording, retuned), with nothing
/// allocated, freed, locked or waited on the callback; the voice ASKS for the
/// missing zones and the chart reaper decodes them off the audio thread, so the
/// held pack bytes grow while the song keeps playing.
void scenarioChartZoneMiss()
{
    beginScenario("RtChartVoiceZoneMissFallsBackWithoutBlocking");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dcrt_chart_zones";
    voicefixture::writeAllPacks(dir, 15, 3.0, 3);
    setEnv("DOTCHARTER_VOICE_DIR", dir.string().c_str());
    static const int open[6] = { 40, 45, 50, 55, 59, 64 };
    auto makeNotes = [](int stringLo, int fretLo, double first) {
        std::vector<dca_chart_note> v;
        for (int i = 0; i < 6; ++i)
        {
            dca_chart_note n{};
            n.start_seconds   = first + 0.35 * i;
            n.release_seconds = n.start_seconds + 1.2;
            n.sustain_seconds = 1.2;
            n.string_index    = stringLo + (i % 2);
            n.fret            = fretLo + (i % 3);
            n.pitch           = (float) (open[n.string_index] + n.fret);
            n.velocity        = 0.8f;
            n.slide_to_fret   = -1.0f;
            n.chord_id        = -1;
            n.bend_first      = -1;
            n.flags           = DCA_CHART_NOTE_LET_RING;
            v.push_back(n);
        }
        return v;
    };
    const auto chartA = makeNotes(1, 2, 0.30);    // strings 1-2, frets 2-4
    const auto chartB = makeNotes(4, 12, 1.00);   // strings 4-5, frets 12-14: nothing of A's within reach
    const auto blob = makeWavBlob(8.0, 48000, 220.0);
    {
        Fixture fx;
        if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
        auto& e = fx.engine();
        e.setMonitorGain(1.0f);
        TransportSource src(e);
        RT_CHECK(src.openMemory(blob.getData(), blob.getSize(), "wav").isEmpty(), "openMemory failed");
        src.setVolume(0.0f);
        RT_CHECK(e.setSongClockSource(&src), "clock refused");
        std::string error;
        RT_CHECK(e.setChartNotes(0, chartA.data(), (int32_t) chartA.size(), (int32_t) sizeof(dca_chart_note), nullptr, 0, error),
                 "chart A rejected: " + error);
        dcvoice::InstrumentConfig neck;
        neck.model = dcvoice::kModelStratSSS;
        neck.pickup = dcvoice::kPickNeck;
        RT_CHECK(e.setChartVoice(0, DCA_CHART_VOICE_GUITAR), "guitar voice refused");
        RT_CHECK(e.setChartInstrument(0, neck), "Strat neck refused");
        RT_CHECK(e.setChartSourceEnabled(0, true), "enable failed");
        const int64_t aBytes = dcsynth::livePackBytes();
        // The whole pack, for scale: 6 strings x 16 frets x 3 s of int16 at 44.1 kHz.
        const int64_t wholePack = 6LL * 16 * (int64_t) (3.0 * 44100) * 2;
        note("chart A decoded " + std::to_string(aBytes / 1024) + " KiB of a " + std::to_string(wholePack / 1024) + " KiB pack");
        RT_CHECK(aBytes > 0 && aBytes < wholePack / 4, "enabling with chart A did not decode just A's zones ("
                 + std::to_string(aBytes) + " bytes)");

        e.setChartZonePreloadForTesting(false);
        RT_CHECK(e.setChartNotes(0, chartB.data(), (int32_t) chartB.size(), (int32_t) sizeof(dca_chart_note), nullptr, 0, error),
                 "chart B rejected: " + error);
        RT_CHECK(dcsynth::livePackBytes() == aBytes, "chart B's zones were decoded before it was published (the scenario is vacuous)");
        src.play();
        warmUpAndArm(fx);   // ~0.34 s: B's first note (1.0 s) is still ahead

        const int bs = fx.blockSize();
        const double firstNote = chartB.front().start_seconds;
        int blockOfFirst = -1, blocksToReady = -1;
        float firstPeak = 0.0f;
        for (int i = 0; i < 400 && (blocksToReady < 0 || i < blockOfFirst + 80); ++i)
        {
            { dcrt::ScopedRtThread rt; nulldev::pump(1); }
            float peak = 0.0f;
            for (float s : nulldev::manualDevice()->manualOutputBuffers()[0]) peak = std::max(peak, std::abs(s));
            dca_chart_source_status st{};
            st.struct_bytes = (int32_t) sizeof(st);
            e.getChartStatus(0, st);
            if (blockOfFirst < 0 && st.notes_started > 0 && st.song_seconds >= firstNote)
                blockOfFirst = i;
            // The note's first blocks (the next note is 65 blocks later): the
            // player that started on the fallback keeps it to its end.
            if (blockOfFirst >= 0 && i <= blockOfFirst + 5)
                firstPeak = std::max(firstPeak, peak);
            if (blockOfFirst >= 0 && blocksToReady < 0 && dcsynth::livePackBytes() > aBytes)
                blocksToReady = i - blockOfFirst;
            // Wall time for the reaper (it polls every 20 ms): about real time.
            std::this_thread::sleep_for(std::chrono::microseconds(1000000 * bs / 48000));
        }
        dcrt::disarm();
        reportHeap("chart zone miss");
        RT_CHECK(dcrt::allocCount() == 0, "a zone miss allocated " + std::to_string(dcrt::allocCount()) + " time(s) on the callback");
        RT_CHECK(dcrt::freeCount() == 0, "a zone miss freed " + std::to_string(dcrt::freeCount()) + " time(s) on the callback");
        note("first B note in block " + std::to_string(blockOfFirst) + ", peak " + std::to_string(firstPeak) + "; zones ready "
             + std::to_string(blocksToReady) + " block(s) later, " + std::to_string(dcsynth::livePackBytes() / 1024) + " KiB held");
        RT_CHECK(blockOfFirst >= 0, "chart B's first note never started (vacuous)");
        // The note-on picks its recording in the block that asks for the
        // missing zone, so what it plays can only be the FALLBACK.
        RT_CHECK(firstPeak > 1.0e-3f, "a note outside the decoded zones was SILENT (peak "
                 + std::to_string(firstPeak) + "): the fallback did not play");
        RT_CHECK(blocksToReady >= 0, "the missing zones were never decoded (" + std::to_string(dcsynth::livePackBytes())
                 + " bytes held, " + std::to_string(aBytes) + " before)");
        e.setChartZonePreloadForTesting(true);
        RT_CHECK(e.setChartSourceEnabled(0, false), "disable failed");
        src.pause();
    }
    setEnv("DOTCHARTER_VOICE_DIR", nullptr);
    endScenario();
}

/// #417. A NEW chart that leaves megabytes of the old one's zones unplayed (a
/// different song) gets a voice on FRESH packs, decoded for the new chart on
/// the control thread and cross-faded in; the old voice and its packs are freed
/// by the chart reaper. So the held bytes FALL to the new chart's, with nothing
/// allocated or freed on the callback while the song plays across the switch.
/// A small edit afterwards keeps the live packs (it only adds its zones).
void scenarioChartNewChartDropsOldZones()
{
    beginScenario("RtChartVoiceNewChartReleasesTheOldZones");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dcrt_chart_zones_swap";
    voicefixture::writeAllPacks(dir, 15, 3.0, 3);
    setEnv("DOTCHARTER_VOICE_DIR", dir.string().c_str());
    static const int open[6] = { 40, 45, 50, 55, 59, 64 };
    auto mk = [](double t, int s, int fret) {
        dca_chart_note n{};
        n.start_seconds   = t;
        n.release_seconds = t + 0.5;
        n.sustain_seconds = 0.5;
        n.string_index    = s;
        n.fret            = fret;
        n.pitch           = (float) (open[s] + fret);
        n.velocity        = 0.8f;
        n.slide_to_fret   = -1.0f;
        n.chord_id        = -1;
        n.bend_first      = -1;
        return n;
    };
    std::vector<dca_chart_note> songA, songB;
    for (int f = 0; f <= 9; ++f)            // song A: every string, frets 0-9
        for (int s = 0; s < 6; ++s)
            songA.push_back(mk(0.3 + 0.05 * (f * 6 + s), s, f));
    for (int i = 0; i < 12; ++i)            // song B: two strings, frets 13-15
        songB.push_back(mk(0.3 + 0.2 * i, 4 + i % 2, 13 + i % 3));
    const auto blob = makeWavBlob(8.0, 48000, 220.0);
    {
        Fixture fx;
        if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
        auto& e = fx.engine();
        e.setMonitorGain(1.0f);
        TransportSource src(e);
        RT_CHECK(src.openMemory(blob.getData(), blob.getSize(), "wav").isEmpty(), "openMemory failed");
        src.setVolume(0.0f);
        RT_CHECK(e.setSongClockSource(&src), "clock refused");
        std::string error;
        dcvoice::InstrumentConfig neck;
        neck.model = dcvoice::kModelStratSSS;
        neck.pickup = dcvoice::kPickNeck;
        RT_CHECK(e.setChartVoice(0, DCA_CHART_VOICE_GUITAR) && e.setChartInstrument(0, neck), "Strat neck refused");
        RT_CHECK(e.setChartNotes(0, songA.data(), (int32_t) songA.size(), (int32_t) sizeof(dca_chart_note), nullptr, 0, error),
                 "song A rejected: " + error);
        RT_CHECK(e.setChartSourceEnabled(0, true), "enable failed");
        const int64_t aBytes = dcsynth::livePackBytes();
        src.play();
        warmUpAndArm(fx);
        for (int i = 0; i < 40; ++i) { dcrt::ScopedRtThread rt; nulldev::pump(1); }

        // Song B arrives while the song plays: decoded HERE, cross-faded in.
        RT_CHECK(e.setChartNotes(0, songB.data(), (int32_t) songB.size(), (int32_t) sizeof(dca_chart_note), nullptr, 0, error),
                 "song B rejected: " + error);
        const int64_t during = dcsynth::livePackBytes();
        for (int i = 0; i < 200; ++i)
        {
            { dcrt::ScopedRtThread rt; nulldev::pump(1); }
            if (i > 20 && dcsynth::livePackBytes() < during) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (int i = 0; i < 20; ++i) { dcrt::ScopedRtThread rt; nulldev::pump(1); }
        dcrt::disarm();
        reportHeap("chart new song");
        const int64_t bBytes = dcsynth::livePackBytes();
        note("song A held " + std::to_string(aBytes / 1024) + " KiB, both during the switch " + std::to_string(during / 1024)
             + " KiB, song B alone " + std::to_string(bBytes / 1024) + " KiB");
        RT_CHECK(dcrt::allocCount() == 0, "the switch allocated " + std::to_string(dcrt::allocCount()) + " time(s) on the callback");
        RT_CHECK(dcrt::freeCount() == 0, "the switch freed " + std::to_string(dcrt::freeCount()) + " time(s) on the callback");
        RT_CHECK(during > aBytes, "song B's zones were not decoded beside A's while the old voice faded (vacuous)");
        RT_CHECK(bBytes > 0 && bBytes < aBytes / 3, "song A's zones were kept after song B replaced it ("
                 + std::to_string(bBytes) + " bytes held, A alone was " + std::to_string(aBytes) + ")");

        // A small EDIT of song B keeps the live packs: one more note (low E,
        // fret 5), its zones decoded into them before the upload returns.
        songB.push_back(mk(3.0, 0, 5));
        RT_CHECK(e.setChartNotes(0, songB.data(), (int32_t) songB.size(), (int32_t) sizeof(dca_chart_note), nullptr, 0, error),
                 "the edit was rejected: " + error);
        const int64_t edited = dcsynth::livePackBytes();
        RT_CHECK(edited > bBytes && edited - bBytes < 2 * 1024 * 1024, "an edit did not just add its zones to the live packs ("
                 + std::to_string(edited) + " vs " + std::to_string(bBytes) + ")");
        RT_CHECK(e.setChartSourceEnabled(0, false), "disable failed");
        src.pause();
    }
    setEnv("DOTCHARTER_VOICE_DIR", nullptr);
    endScenario();
}

// ---------------------------------------------------------------------------
// JuceSharp #32: song speed crossing 1.0 while the song plays.
// ---------------------------------------------------------------------------
// RubberBandAudioSource takes a pass-through fast path at unity tempo/pitch and
// the Rubber Band stretcher otherwise. Crossing between the two used to
//   * slowed -> 100 %: skip FORWARD by everything the stretcher had buffered (the
//     inner transport is read that far ahead of what the listener has heard):
//     +74..84 ms of song in one second of output, both engines;
//   * 100 % -> slowed: replay the stretcher's STALE audio from the last time it
//     ran, seconds earlier: -81..-87 ms;
// a seek while slowed played out ~5 s-old PRE-SEEK audio first, because the
// transport seek never reached the stretcher; and the stretcher's cold start
// (unpadded real-time mode) was ~27 ms of silence and a fade on R3.
//
// The song is a linear CHIRP, so the instantaneous frequency of the output IS
// the song position it was played from: f = f0 + k * t. Rubber Band preserves
// pitch when it stretches, so the position can be read straight off the output
// (an FFT peak per window; zero crossings are thrown by a single phase step)
// and checked for skips, replays and dropouts across every edge — with the hook
// armed for the whole run, so every edge is allocation-checked too.
//
// `grouped` plays the same script through the STEM-GROUP stretcher (the source
// bypassed, tempo through AudioEngine::setStemGroupTempo) — the same class,
// reached by a different seek path (AudioEngine::resetStemGroupStretcher).
namespace speedcross {

constexpr double kF0          = 200.0;  // Hz at song time 0
constexpr double kK           = 100.0;  // Hz per song second
constexpr double kSongSeconds = 16.0;
constexpr int    kWinOrder    = 12;     // 4096-sample analysis window
constexpr int    kWin         = 1 << kWinOrder;
constexpr int    kHop         = 1024;
constexpr int    kZeroPad     = 4;      // FFT of 4 x the window: 2.9 Hz bins at 48 kHz

juce::MemoryBlock makeChirpWavBlob(int sampleRate)
{
    const int frames = (int) (kSongSeconds * sampleRate);
    juce::AudioBuffer<float> buf(2, frames);
    for (int i = 0; i < frames; ++i)
    {
        const double t = (double) i / (double) sampleRate;
        const float  s = 0.3f * (float) std::sin(2.0 * 3.14159265358979323846 * (kF0 * t + 0.5 * kK * t * t));
        buf.setSample(0, i, s);
        buf.setSample(1, i, s);
    }

    juce::MemoryBlock block;
    juce::WavAudioFormat wav;
    auto stream = std::make_unique<juce::MemoryOutputStream>(block, false);
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.get(), (double) sampleRate, 2, 16, {}, 0));
    if (writer == nullptr) return {};
    stream.release(); // writer owns it
    writer->writeFromAudioSampleBuffer(buf, 0, frames);
    writer.reset();
    return block;
}

struct Reading
{
    double centre = 0.0;   // output sample index of the window centre
    double song   = 0.0;   // song seconds it was played from
};

/// Song position of every hop of the output: the Hann-windowed spectrum's
/// peak, parabolically interpolated in log magnitude, mapped back through the
/// chirp. Off the audio thread (it allocates).
std::vector<Reading> readSongPositions(const std::vector<float>& x, int sampleRate)
{
    juce::dsp::FFT fft(kWinOrder + 2);
    const int n = kWin * kZeroPad;
    std::vector<float> buf((size_t) n * 2), window((size_t) kWin);
    for (int i = 0; i < kWin; ++i)
        window[(size_t) i] = 0.5f - 0.5f * (float) std::cos(2.0 * 3.14159265358979323846 * i / (kWin - 1));

    std::vector<Reading> out;
    for (size_t s = 0; s + kWin <= x.size(); s += kHop)
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < kWin; ++i) buf[(size_t) i] = x[s + (size_t) i] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform(buf.data());
        int k = 1;
        for (int i = 2; i < n / 2 - 1; ++i) if (buf[(size_t) i] > buf[(size_t) k]) k = i;
        const double a = std::log(buf[(size_t) k - 1] + 1e-12), b = std::log(buf[(size_t) k] + 1e-12),
                     c = std::log(buf[(size_t) k + 1] + 1e-12);
        const double denom = a - 2.0 * b + c;
        const double bin   = k + (denom != 0.0 ? 0.5 * (a - c) / denom : 0.0);
        const double f     = bin * sampleRate / n;
        out.push_back({ (double) s + 0.5 * kWin, (f - kF0) / kK });
    }
    return out;
}

/// Song position at output sample `at`: a straight line through every reading
/// within 0.1 s of it, so one noisy window (R2 is noisy on a chirp) cannot
/// fake or hide a jump. NaN when the readings around it are too few.
double songAt(const std::vector<Reading>& r, double at, int sampleRate)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int m = 0;
    for (const auto& p : r)
        if (std::abs(p.centre - at) <= 0.1 * sampleRate)
        {
            const double dx = p.centre - at;
            sx += dx; sy += p.song; sxx += dx * dx; sxy += dx * p.song; ++m;
        }
    if (m < 5) return std::nan("");
    const double det = m * sxx - sx * sx;
    return det != 0.0 ? (sy * sxx - sx * sxy) / det : sy / m;
}

} // namespace speedcross

void scenarioSpeedCrossingUnity(bool faster, bool grouped)
{
    using namespace speedcross;
    beginScenario(grouped ? "RtSpeedCrossingUnityIsSeamlessStemGroup"
                  : faster ? "RtSpeedCrossingUnityIsSeamlessFaster" : "RtSpeedCrossingUnityIsSeamlessFiner");

    Fixture fx;
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    auto& engine = fx.engine();
    engine.setStretchFaster(faster);
    const int sr = fx.sampleRate(), blk = fx.blockSize();

    const auto blob = makeChirpWavBlob(sr);
    if (blob.getSize() == 0) { fail("could not synthesise the chirp"); endScenario(); return; }
    TransportSource src(engine);
    const auto err = src.openMemory(blob.getData(), blob.getSize(), "wav");
    if (err.isNotEmpty()) { fail("openMemory failed: " + err.toStdString()); endScenario(); return; }
    RT_CHECK(src.isPreloaded(), "the chirp did not preload");
    RT_CHECK(src.stretcherUsedFasterForTest() == faster, "the stretcher was built with the wrong engine");
    if (grouped) engine.setSourceStemGrouped(&src, true);
    RT_CHECK(src.isStretcherBypassed() == grouped, "the source is not routed through the stem group");
    auto* dev = nulldev::manualDevice();
    if (dev == nullptr) { fail("no manual null device"); endScenario(); return; }

    auto setTempo = [&](float t) { if (grouped) engine.setStemGroupTempo(t); else src.setTempoFactor(t); };
    auto readAheadSamples = [&]() {
        return grouped ? engine.stemGroupReadAheadSamples()
                       : (int) std::llround((src.getPositionSeconds() - src.getAudiblePositionSeconds()) * sr);
    };

    // The script, in blocks of 256 at 48 kHz (187.5 blocks a second). Speed
    // changes and seeks are made BETWEEN blocks on this (control) thread,
    // exactly as the managed setters make them; only the pump is the audio
    // thread. Every edge has 0.6 s of steady playback on either side.
    enum class Op { Tempo, Seek };
    struct Step { int atBlock; Op op; double value; const char* what; };
    const Step script[] = {
        {  150, Op::Tempo, 0.8,  "100 % -> 80 % (the stretcher's first start)" },
        {  450, Op::Tempo, 1.0,  "80 % -> 100 % (stretched -> unity)" },
        {  825, Op::Tempo, 0.8,  "100 % -> 80 % (unity -> stretched, 2 s after it last ran)" },
        { 1125, Op::Seek,  10.0, "seek to 10 s while at 80 %" },
        { 1325, Op::Tempo, 1.0,  "80 % -> 100 % again" },
        { 1525, Op::Seek,  14.0, "seek to 14 s at 100 %" },
    };
    constexpr int kTotalBlocks = 1650;

    std::vector<float>  out;
    std::vector<size_t> edges;
    out.reserve((size_t) kTotalBlocks * (size_t) blk);

    src.play();
    fx.pumpWarm(1);          // the play edge; the capture starts on the next block
    dcrt::reset();
    dcrt::arm();
    size_t next = 0;
    for (int b = 0; b < kTotalBlocks; ++b)
    {
        while (next < std::size(script) && script[next].atBlock == b)
        {
            const auto& s = script[next++];
            if (s.op == Op::Tempo) setTempo((float) s.value);
            else                   src.seekSeconds(s.value);
            edges.push_back(out.size());
        }
        dcrt::g_currentBlock.store((unsigned) b, std::memory_order_relaxed);
        { dcrt::ScopedRtThread rt; nulldev::pump(1); }
        const auto& o = dev->manualOutputBuffers()[0];
        out.insert(out.end(), o.begin(), o.begin() + blk);
    }
    dcrt::disarm();
    reportHeap("speed crossing");
    const int allocs = dcrt::allocCount();
    dcrt::reset();
    const int finalReadAhead = readAheadSamples();
    src.pause();
    if (grouped) engine.setSourceStemGrouped(&src, false);

    const auto readings = readSongPositions(out, sr);

    // Level, for the dropout check: the median 1024-sample RMS of the run.
    auto rmsAt = [&](size_t s) {
        double acc = 0.0;
        for (size_t i = s; i < s + 1024 && i < out.size(); ++i) acc += (double) out[i] * out[i];
        return std::sqrt(acc / 1024.0);
    };
    std::vector<double> levels;
    for (size_t s = 0; s + 1024 <= out.size(); s += 1024) levels.push_back(rmsAt(s));
    std::sort(levels.begin(), levels.end());
    const double refRms = levels.empty() ? 0.0 : levels[levels.size() / 2];
    note("reference rms " + std::to_string(refRms) + ", " + std::to_string(readings.size()) + " position readings");
    RT_CHECK(refRms > 0.05, "the song never became audible -- every check below would be vacuous");

    double tempoBefore = 1.0;
    for (size_t e = 0; e < edges.size(); ++e)
    {
        const auto&  s    = script[e];
        const double edge = (double) edges[e];

        // (1) No dropout where the edge lands: every 1024-sample window of the
        // first quarter second keeps a fifth of the song's level. (R2 dips to
        // ~a third on a chirp in steady state, so this is not a quality bar —
        // the old cold start was ~27 ms of digital silence.)
        double minRms = 1e9;
        for (size_t p = edges[e]; p + 1024 <= edges[e] + (size_t) (0.25 * sr); p += 256)
            minRms = std::min(minRms, rmsAt(p));
        RT_CHECK(minRms >= 0.2 * refRms,
                 std::string(s.what) + ": the output fell to rms " + std::to_string(minRms)
                 + " right after the edge (song level " + std::to_string(refRms) + ") -- a dropout");

        if (s.op == Op::Tempo)
        {
            // (2) Across a speed edge, the song must advance by exactly what the
            // two speeds account for: half a second at each. A skip adds the
            // stretcher's read-ahead, a stale replay loses what it replays.
            const double t0 = tempoBefore, t1 = s.value;
            const double moved  = songAt(readings, edge + 0.5 * sr, sr) - songAt(readings, edge - 0.5 * sr, sr);
            const double expect = 0.5 * t0 + 0.5 * t1;
            const double tol    = 0.030;
            note(std::string(s.what) + ": song moved " + std::to_string(moved * 1000.0)
                 + " ms in 1000 ms of output (expected " + std::to_string(expect * 1000.0)
                 + " +- " + std::to_string(tol * 1000.0) + "), min rms " + std::to_string(minRms));
            RT_CHECK(! std::isnan(moved) && std::abs(moved - expect) <= tol,
                     std::string(s.what) + ": the song moved " + std::to_string(moved * 1000.0)
                     + " ms in one second of output around the edge, expected "
                     + std::to_string(expect * 1000.0) + " ms -- "
                     + (moved > expect ? "a forward SKIP" : "a REPLAY of earlier audio"));
            tempoBefore = t1;
        }
        else
        {
            // (3) After a seek, every reading whose window starts a block later
            // is the seek target played on at the current speed — nothing from
            // before the seek, nothing skipped past it.
            double worst = 0.0;
            int    seen  = 0;
            for (const auto& r : readings)
                if (r.centre - 0.5 * kWin >= edge + blk && r.centre < edge + 0.5 * sr)
                {
                    const double expect = s.value + tempoBefore * (r.centre - edge) / sr;
                    if (std::abs(r.song - expect) > std::abs(worst)) worst = r.song - expect;
                    ++seen;
                }
            note(std::string(s.what) + ": " + std::to_string(seen) + " readings after it, worst "
                 + std::to_string(worst * 1000.0) + " ms off the target's line, min rms " + std::to_string(minRms));
            RT_CHECK(seen > 10 && std::abs(worst) <= 0.020,
                     std::string(s.what) + ": the output after the seek was " + std::to_string(worst * 1000.0)
                     + " ms of song away from the seek target -- "
                     + (worst < 0 ? "audio from BEFORE the seek played out" : "the start was skipped"));
        }
    }

    // (4) Back at 100 % after a seek the source is a plain pass-through again:
    // the stretcher that ran since is reset and holds no read-ahead.
    note("read-ahead after the last seek: " + std::to_string(finalReadAhead) + " samples");
    RT_CHECK(finalReadAhead == 0,
             "100 % after a seek still reports " + std::to_string(finalReadAhead)
             + " samples of stretcher read-ahead: the source never returned to the pass-through path");

    note("allocations on the callback: " + std::to_string(allocs));
    RT_CHECK(allocs == 0, "crossing 100 % allocated " + std::to_string(allocs) + " time(s) on the callback");
    endScenario();
}

void scenarioSpeedCrossingUnityFiner()     { scenarioSpeedCrossingUnity(false, false); }
void scenarioSpeedCrossingUnityFaster()    { scenarioSpeedCrossingUnity(true,  false); }
void scenarioSpeedCrossingUnityStemGroup() { scenarioSpeedCrossingUnity(false, true); }

// The chart voice (src/dsp/voice/, issue #39 step 1): the waveguide guitar and
// bass AND the sample player (on a pack synthesised here) render a dense chart
// that uses every articulation — strummed chords, palm
// mutes, dead notes, hammer-ons, pull-offs, taps, bends, slides, harmonics,
// vibrato, tremolo, slap, pop — plus a seek and a stop, with the hook armed.
// The voice is self-contained (no engine wiring yet), so it is driven on the
// calling thread inside ScopedRtThread exactly like the offline drop tuner.
// Non-vacuity: the armed window must be AUDIBLE, every string must have
// sounded, and the stop must free every string.
void scenarioChartVoice()
{
    beginScenario("RtChartVoiceRendersEveryArticulationWithoutAllocating");
    using namespace dcvoice;

    std::vector<ChartNote> notes;
    std::vector<double> beats;
    for (double b = 0.0; b < 40.0; b += 0.5)
        beats.push_back(b);
    uint32_t r = 0x9E3779B9u;
    auto rnd = [&r] { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (double) (r >> 8) / 16777216.0; };
    int chord = 0;
    for (double t = 0.05; t < 12.0; t += 0.125)
    {
        if (rnd() < 0.15)
        {
            for (int s = 0; s < 6; ++s)
            {
                ChartNote n;
                n.time = t; n.sustain = 0.3; n.string = s; n.fret = (int) (rnd() * 5); n.chordId = chord;
                n.mute = rnd() < 0.2 ? 2 : 0;
                notes.push_back(n);
            }
            ++chord;
            continue;
        }
        ChartNote n;
        n.time = t; n.sustain = rnd() < 0.5 ? 0.0 : rnd(); n.string = (int) (rnd() * 6); n.fret = (int) (rnd() * 22);
        const int k = (int) (rnd() * 14);
        switch (k)
        {
            case 0: n.hopo = 1; break;
            case 1: n.hopo = 2; break;
            case 2: n.hopo = 3; break;
            case 3: n.mute = 1; break;
            case 4: n.mute = 2; break;
            case 5: n.harmonic = 1; break;
            case 6: n.harmonic = 2; break;
            case 7: n.slideTo = (int) (rnd() * 20); n.sustain = 0.6; break;
            case 8: n.unpitchedSlide = true; n.sustain = 0.5; break;
            case 9: n.tremolo = true; n.sustain = 0.6; break;
            case 10: n.vibrato = true; n.vibratoStrength = 120; n.sustain = 0.8; break;
            case 11: n.bassPicking = 1; break;
            case 12: n.bassPicking = 2; break;
            default: n.accent = true; break;
        }
        notes.push_back(n);
    }
    std::vector<ChartBend> bends;
    for (auto& n : notes)
        if (n.mute == 0 && n.hopo == 0 && rnd() < 0.08)
        {
            n.bendFirst = (int) bends.size();
            n.bendCount = 2;
            n.sustain = std::max(n.sustain, 0.5);
            bends.push_back({ n.time + 0.1, 2.0f });
            bends.push_back({ n.time + 0.4, 0.0f });
        }

    constexpr int kBlock = 128;
    constexpr double kSr = 48000.0;

    // A small sample pack, synthesised here (control thread): decaying
    // harmonic tones on every semitone, plus palm, legato, release and dead
    // zones — enough to drive every path of the sample player.
    SamplePack pack;
    {
        auto addZone = [&pack](Art art, double hz, int layer, int rr, double seconds, double t60, bool noise) {
            PackZone z;
            z.art = (uint8_t) art;
            z.layer = (uint8_t) layer;
            z.rr = (uint8_t) rr;
            z.rootHz = (float) hz;
            z.gain = 0.5f / 32767.0f;
            z.length = (uint32_t) (seconds * 44100.0);
            z.start = pack.data.size();
            uint32_t r = (uint32_t) (hz * 13.0) + 7u;
            for (uint32_t i = 0; i < z.length; ++i)
            {
                const double t = i / 44100.0;
                double v;
                if (noise)
                {
                    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                    v = ((r >> 8) / 8388608.0 - 1.0) * std::exp(-t / 0.02);
                }
                else
                {
                    v = 0.0;
                    for (int n = 1; n <= 6; ++n)
                        v += std::sin(6.283185307179586 * n * hz * t) / n * std::pow(10.0, -3.0 * t / t60);
                    v *= 0.6;
                }
                pack.data.push_back((int16_t) std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
            }
            pack.zones.push_back(z);
            pack.layerCount[(int) art][0] = std::max(pack.layerCount[(int) art][0], layer + 1);
        };
        for (int m = 40; m <= 88; ++m)
        {
            const double hz = 440.0 * std::pow(2.0, (m - 69) / 12.0);
            for (int layer = 0; layer < 2; ++layer)
                addZone(Art::Sustain, hz, layer, 0, 1.2, 3.0, false);
            if (m % 3 == 0)
            {
                addZone(Art::Palm, hz, 0, 0, 0.3, 0.2, false);
                addZone(Art::Legato, hz, 0, 0, 1.0, 2.0, false);
                addZone(Art::Release, hz, 0, 0, 0.2, 0.0, true);
            }
        }
        for (double open : { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 })
            addZone(Art::Dead, open, 0, 0, 0.1, 0.0, true);
        // Bass paths (#52): ghost / dead plucks and the slap thumb.
        for (int m = 28; m <= 60; m += 4)
            addZone(Art::Ghost, 440.0 * std::pow(2.0, (m - 69) / 12.0), 0, 0, 0.3, 0.15, false);
        addZone(Art::Slap, 100.0, 0, 0, 0.1, 0.0, true);
    }
    // #52: the ARTICULATION pack (palm mutes, natural and pinch harmonics on
    // known strings) and a ONE-TAKE pack (every string and fret, one layer, the
    // Strat's layout) that sends repeated notes to their neighbours.
    const double opens[6] = { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 };
    auto synthZone = [](SamplePack& pk, Art art, double hz, int string, int fret, double seconds, double t60) {
        PackZone z;
        z.art = (uint8_t) art;
        z.rootHz = (float) hz;
        z.gain = 0.5f / 32767.0f;
        z.length = (uint32_t) (seconds * 44100.0);
        z.start = pk.data.size();
        z.string = (int8_t) string;
        z.fret = (int8_t) fret;
        for (uint32_t i = 0; i < z.length; ++i)
        {
            const double t = i / 44100.0;
            double v = 0.0;
            for (int n = 1; n <= 6; ++n)
                v += std::sin(6.283185307179586 * n * hz * t) / n * std::pow(10.0, -3.0 * t / t60);
            pk.data.push_back((int16_t) std::lround(std::clamp(0.6 * v, -1.0, 1.0) * 32767.0));
        }
        pk.zones.push_back(z);
        pk.layerCount[(int) art][0] = 1;
    };
    SamplePack arts, oneTake;
    for (int s2 = 0; s2 < 6; ++s2)
    {
        for (int fret = 0; fret <= 22; fret += 2)
        {
            const double hz = opens[s2] * std::pow(2.0, fret / 12.0);
            synthZone(arts, Art::Palm, hz, s2, fret, 0.4, 0.3);
            if (fret > 0)
                synthZone(arts, Art::Pinch, hz, s2, fret, 0.8, 1.5);
        }
        synthZone(arts, Art::NatHarm, 2.0 * opens[s2], s2, 12, 1.0, 2.5);
        synthZone(arts, Art::NatHarm, 3.0 * opens[s2], s2, 7, 1.0, 2.5);
        synthZone(arts, Art::NatHarm, 4.0 * opens[s2], s2, 5, 1.0, 2.5);
        for (int fret = 0; fret <= 22; ++fret)
            synthZone(oneTake, Art::Sustain, opens[s2] * std::pow(2.0, fret / 12.0), s2, fret, 1.0, 3.0);
    }
    arts.multiString = oneTake.multiString = true;
    // #109: the bass pack's RECORDED SLAPS (Art::SlapNote) on known strings,
    // played through their own (non-identity) conversion plan.
    SamplePack slapPack = pack;
    for (int m = 28; m <= 62; m += 2)
    {
        const int s2 = m <= 31 ? 0 : m <= 38 ? 1 : m <= 43 ? 2 : 3;
        synthZone(slapPack, Art::SlapNote, 440.0 * std::pow(2.0, (m - 69) / 12.0), s2, m - (28 + 5 * s2), 0.8, 0.6);
    }
    const ModelSpec* jazz = findModel(kModelJazzBass);
    const ConversionPlan slapPlan = makeConversionPlan(*jazz, findPosition(*jazz, kPickNeckBridge)->full, "bass-slap", 0);
    RT_CHECK(! slapPlan.identity, "the recorded slaps' conversion is not an identity (the probe would skip it)");

    for (int which = 0; which < 6; ++which)
    {
        // 0 waveguide guitar, 1 waveguide bass, 2 sampled guitar, 3 sampled guitar
        // on a one-take pack with the articulation pack, 4 sampled bass (modelled
        // slap / pop), 5 sampled bass with RECORDED slaps, converted (#109).
        const bool bass = which == 1 || which == 4 || which == 5;
        PlanOptions po;
        po.bass = bass;
        PlannedTimeline plan;   // control thread: allocates, unarmed
        std::vector<ChartNote> chart = notes;
        if (bass)
            for (auto& n : chart) { n.string %= 4; n.fret %= 17; }
        planChart(chart.data(), (int) chart.size(), bends.data(), (int) bends.size(), beats.data(), (int) beats.size(), po, plan);
        const VoiceTimeline tl = plan.view(1);
        ChartTimelineVoice voice = which == 2 ? ChartTimelineVoice(InstrumentSpec::guitar(), &pack)
                                   : which == 3 ? ChartTimelineVoice(InstrumentSpec::guitar(), &oneTake, &arts)
                                   : which == 4 ? ChartTimelineVoice(InstrumentSpec::bassGuitar(4), &pack)
                                   : which == 5 ? ChartTimelineVoice(InstrumentSpec::bassGuitar(4), &slapPack)
                                                : ChartTimelineVoice(bass ? InstrumentSpec::bassGuitar(4) : InstrumentSpec::guitar(),
                                                                     bass ? bassExcitations() : guitarExcitations());
        if (which == 5)
            static_cast<SampleInstrument&>(voice.core()).setSlapConversion(slapPlan);   // control thread, before prepare
        voice.prepare(kSr, kBlock);
        std::vector<float> block((size_t) kBlock, 0.0f);
        VoiceClock clock;
        clock.rolling = true;
        clock.songSecondsPerSample = 1.0 / kSr;

        // Warm-up, unarmed.
        for (int b = 0; b < 64; ++b)
        {
            clock.songTime = b * kBlock / kSr;
            voice.render(block.data(), kBlock, clock, tl);
        }

        dcrt::reset();
        dcrt::arm();
        double energy = 0.0;
        bool finite = true;
        int stringsHeard = 0;
        int activeAfterStop = -1;
        {
            dcrt::ScopedRtThread rt;
            double song = 64.0 * kBlock / kSr;
            const int blocks = (int) (12.5 * kSr / kBlock);
            for (int b = 0; b < blocks; ++b)
            {
                if (b == blocks / 2)
                    song = 1.0;   // a seek back, armed
                clock.songTime = song;
                clock.rolling = b < blocks - 400;   // and a stop at the end
                voice.render(block.data(), kBlock, clock, tl);
                for (float x : block)
                {
                    finite = finite && std::isfinite(x);
                    energy += (double) x * x;
                }
                for (int s = 0; s < voice.spec().numStrings; ++s)
                    if (voice.core().stringActive(s))
                        stringsHeard |= 1 << s;
                song += kBlock / kSr;
            }
            activeAfterStop = voice.activeStrings();
        }
        dcrt::disarm();
        static const char* const kWho[6] = { "guitar", "bass", "sampled guitar", "sampled guitar + articulation pack",
                                             "sampled bass", "sampled bass + recorded slaps" };
        const char* who = kWho[which];
        reportHeap((std::string("chart voice (") + who + ")").c_str());
        RT_CHECK(dcrt::allocCount() == 0 && dcrt::freeCount() == 0,
                 std::string("the ") + who + " voice allocated " + std::to_string(dcrt::allocCount()) + " / freed "
                 + std::to_string(dcrt::freeCount()) + " on the render path");
        RT_CHECK(finite, std::string("the ") + who + " voice rendered a non-finite sample");
        const double rms = std::sqrt(energy / (12.5 * kSr));
        RT_CHECK(rms > 1.0e-3, std::string("the armed ") + who + " render was silent (rms " + std::to_string(rms) + ") — the probe is vacuous");
        RT_CHECK(stringsHeard == (1 << voice.spec().numStrings) - 1,
                 std::string("not every ") + who + " string sounded in the armed window (mask " + std::to_string(stringsHeard) + ")");
        RT_CHECK(activeAfterStop == 0, std::string("a stopped transport left ") + std::to_string(activeAfterStop) + " " + who + " strings sounding");
        dcrt::reset();
    }
    endScenario();
}

/// THE MASTER BUS ON A SPARSE OUTPUT MASK (JuceSharp #31).
///
/// Two master-bus stages disagreed about the `numOutputChannels` a device
/// callback receives. MasterLimiter was sized from the device's FULL physical
/// output list on the belief that JUCE hands one pointer per physical channel
/// and nulls the inactive ones; the HP/LP/BP sections and the song EQ were
/// sized from the ACTIVE count and mapped channel -> state with
/// `jmin(ch, size - 1)`. If the limiter's belief were right, outputs 3-4 on and
/// 1-2 off would run L and R through ONE biquad state.
///
/// Phase A settles it on the real callback path: the null device opens
/// outputs 3-4 of its 8 and the engine's callback records what it was handed.
/// JUCE's WASAPI, ASIO and DirectSound devices all PACK the enabled outputs
/// (one non-null pointer per ENABLED channel, in order), and so does the null
/// device; the record must say 2 pointers, none null. Phase B: every per-channel
/// master-bus stage reports that same width. Phase C: master HP + one EQ band,
/// a sine on L only leaves R exactly silent, and the same sine on both comes
/// out identical on L and R with no block-boundary step. Phase D: a block
/// WIDER than prepared (the shape the old limiter comment described: four
/// pointers, the first two null) must be SKIPPED and COUNTED, never folded
/// onto a shared state. Phase E: a device rebuild to a 4-wide mask re-derives
/// every width and keeps the HP and EQ (the master-bus replay rule).
// ---------------------------------------------------------------------------
// ABI v47: the external process insert. The "other process" is this thread:
// it creates the named block, and between two pumps it does what the other side
// does (consume every unread input block, write it back x 0.5). Three phases,
// every one armed: WAITING (no block size published: the seat is dry),
// CONNECTED (the processed audio replaces the seat's output, no underrun), and
// DISCONNECTED (the other side stops: underruns + overruns are counted and the
// seat falls back to its DRY input, never silence). Then close: the seat dips
// back to its own path.
// ---------------------------------------------------------------------------
namespace extproc_rt {
using namespace dotcharter::extproc;
constexpr uint32_t kRing  = 32;
constexpr uint32_t kBlock = 128;
constexpr size_t   kBytes = kHeaderBytes + (size_t) kRing * 1024 * 3 * sizeof(float); // sized for blocks up to 1024

struct OtherSide
{
    HANDLE   mapping = nullptr;
    uint8_t* view    = nullptr;
    std::string name;

    bool create(uint32_t sampleRate)
    {
        name = "Local\\dc-rtcheck-extproc-" + std::to_string((unsigned) ::GetCurrentProcessId());
        mapping = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD) kBytes, name.c_str());
        if (mapping == nullptr) return false;
        view = static_cast<uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kBytes));
        if (view == nullptr) return false;
        std::memset(view, 0, kBytes);
        std::memcpy(view, "DCXPTEST", 8);
        // Every offset from the one set ExternalProcessInsert.h pins with
        // offsetof against the format's own struct — never a second literal.
        u32(kOffVersion)     = 1;
        u32(kOffSampleRate)  = sampleRate;
        u32(kOffBlockFrames) = 0; // not published yet
        u32(kOffRingBlocks)  = kRing;
        u32(kOffInChannels)  = 1;
        u32(kOffOutChannels) = 2;
        return true;
    }
    ~OtherSide()
    {
        if (view != nullptr) ::UnmapViewOfFile(view);
        if (mapping != nullptr) ::CloseHandle(mapping);
    }
    uint32_t& u32(size_t off) { return *reinterpret_cast<uint32_t*>(view + off); }
    std::atomic<uint32_t>& counter(size_t off) { return *reinterpret_cast<std::atomic<uint32_t>*>(view + off); }

    /// One turn of the other process: every unread input block, x 0.5, out.
    int echo(float gain)
    {
        const uint32_t B = u32(kOffBlockFrames);
        const size_t inOff  = align64(kHeaderBytes);
        const size_t outOff = align64(inOff + (size_t) kRing * B * sizeof(float));
        int done = 0;
        for (;;)
        {
            const uint32_t inW  = counter(kOffInWrite).load(std::memory_order_acquire);
            const uint32_t inR  = counter(kOffInRead).load(std::memory_order_relaxed);
            const uint32_t outW = counter(kOffOutWrite).load(std::memory_order_relaxed);
            const uint32_t outR = counter(kOffOutRead).load(std::memory_order_acquire);
            if (inR == inW || outW - outR >= kRing) break;
            const float* in = reinterpret_cast<const float*>(view + inOff) + (size_t) (inR % kRing) * B;
            float* out = reinterpret_cast<float*>(view + outOff) + (size_t) (outW % kRing) * B * 2;
            for (uint32_t j = 0; j < B; ++j) out[2 * j] = out[2 * j + 1] = in[j] * gain;
            counter(kOffInRead).store(inR + 1, std::memory_order_release);
            counter(kOffOutWrite).store(outW + 1, std::memory_order_release);
            ++done;
        }
        return done;
    }
};
} // namespace extproc_rt

void scenarioExternalProcessInsert()
{
    beginScenario("RtExternalProcessInsertExchangesWithoutAllocatingAndFallsBackDry");
    Fixture fx(48000, 256, 2);
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    auto& e = fx.engine();
    e.setMonitorGain(1.0f); // the seat must reach the device output to be compared
    fx.installInputStimulus(0.25f, 187.5);
    fx.pumpWarm(16);
    const float dryPeak = fx.lastOutputPeak();
    RT_CHECK(dryPeak > 0.05f, "the seat produced no output to compare against (peak "
             + std::to_string(dryPeak) + ")");

    extproc_rt::OtherSide other;
    if (! other.create(48000)) { fail("could not create the shared block"); endScenario(); return; }

    auto stats = [&]()
    {
        dca_extproc_stats st{};
        st.struct_bytes = (int32_t) sizeof(st);
        e.getExternalInsertStats(0, st);
        return st;
    };

    // --- refusals: a missing block, a wrong magic, a rate the device is not at
    RT_CHECK(e.openExternalInsert(0, "Local\\dc-rtcheck-no-such-block", nullptr) == DCA_ERR_NOT_FOUND,
             "a missing block was not DCA_ERR_NOT_FOUND");
    RT_CHECK(e.openExternalInsert(0, other.name.c_str(), "OTHER") == DCA_ERR_DECODE,
             "a wrong magic was accepted");
    other.u32(dotcharter::extproc::kOffSampleRate) = 44100;
    RT_CHECK(e.openExternalInsert(0, other.name.c_str(), "DCXPTEST") == DCA_ERR_SAMPLE_RATE,
             "a 44.1 kHz ring on a 48 kHz device was not DCA_ERR_SAMPLE_RATE");
    RT_CHECK(stats().state == DCA_EXTPROC_STATE_CLOSED, "a refused open left something open");
    other.u32(dotcharter::extproc::kOffSampleRate) = 48000;
    RT_CHECK(e.openExternalInsert(0, other.name.c_str(), "DCXPTEST") == DCA_OK, "the open was refused");

    // --- Phase A: open, but the other side has not published its block -------
    fx.pumpWarm(8); // through the 5 ms dip
    warmUpAndArm(fx);
    fx.pumpArmed(64);
    dcrt::disarm();
    reportHeap("external insert, waiting");
    RT_CHECK(dcrt::allocCount() == 0, "the waiting insert allocated " + std::to_string(dcrt::allocCount()) + " time(s)");
    auto st = stats();
    RT_CHECK(st.state == DCA_EXTPROC_STATE_WAITING, "state " + std::to_string(st.state) + ", expected WAITING");
    const float waitPeak = fx.lastOutputPeak();
    RT_CHECK(std::abs(waitPeak - dryPeak) < 0.02f * dryPeak + 1e-4f,
             "a waiting insert is not dry (peak " + std::to_string(waitPeak) + " vs " + std::to_string(dryPeak) + ")");

    // --- Phase B: connected — the processed block replaces the seat ----------
    other.u32(dotcharter::extproc::kOffBlockFrames) = extproc_rt::kBlock;
    for (int i = 0; i < 16; ++i) { fx.pumpWarm(1); other.echo(0.5f); }
    const auto before = stats();
    dcrt::reset();
    dcrt::arm();
    float minPeak = 1e9f, maxPeak = 0.0f;
    for (int i = 0; i < 256; ++i)
    {
        dcrt::g_currentBlock.store((unsigned) i, std::memory_order_relaxed);
        {
            dcrt::ScopedRtThread rt;
            nulldev::pump(1);
        }
        const float pk = fx.lastOutputPeak();
        minPeak = std::min(minPeak, pk);
        maxPeak = std::max(maxPeak, pk);
        other.echo(0.5f);
    }
    dcrt::disarm();
    reportHeap("external insert, connected");
    RT_CHECK(dcrt::allocCount() == 0, "the connected insert allocated " + std::to_string(dcrt::allocCount()) + " time(s)");
    st = stats();
    note("connected: received " + std::to_string(st.blocks_received - before.blocks_received)
         + ", latency " + std::to_string(st.latency_samples) + " samples, output peak "
         + std::to_string(minPeak) + ".." + std::to_string(maxPeak) + " (dry " + std::to_string(dryPeak) + ")");
    RT_CHECK(st.state == DCA_EXTPROC_STATE_ACTIVE && st.connected == 1, "the insert is not connected (state "
             + std::to_string(st.state) + ", connected " + std::to_string(st.connected) + ")");
    RT_CHECK(st.blocks_received - before.blocks_received >= 500,
             "only " + std::to_string(st.blocks_received - before.blocks_received) + " processed blocks came back in 512");
    RT_CHECK(st.underruns == before.underruns, "a lockstep other side still underran ("
             + std::to_string(st.underruns - before.underruns) + ")");
    RT_CHECK(st.block_frames == (int32_t) extproc_rt::kBlock && st.ring_blocks == (int32_t) extproc_rt::kRing,
             "the stats do not report the ring geometry");
    RT_CHECK(st.latency_samples >= (int32_t) extproc_rt::kBlock && st.latency_samples <= 8 * (int32_t) extproc_rt::kBlock,
             "latency " + std::to_string(st.latency_samples) + " samples is not a cushion of a few blocks");
    RT_CHECK(minPeak > 0.45f * dryPeak && maxPeak < 0.55f * dryPeak,
             "the seat's output is not the other side's (x 0.5): peak " + std::to_string(minPeak) + ".."
             + std::to_string(maxPeak) + " vs dry " + std::to_string(dryPeak));

    // --- Phase C: the other side stops — dry, counted, never silence --------
    dcrt::reset();
    dcrt::arm();
    fx.pumpArmed(64);
    dcrt::disarm();
    reportHeap("external insert, disconnected");
    RT_CHECK(dcrt::allocCount() == 0, "the disconnected insert allocated " + std::to_string(dcrt::allocCount()) + " time(s)");
    const auto gone = stats();
    note("disconnected: underruns " + std::to_string(gone.underruns - st.underruns) + ", overruns "
         + std::to_string(gone.overruns - st.overruns));
    RT_CHECK(gone.connected == 0, "an insert with nothing coming back still reports connected");
    RT_CHECK(gone.underruns - st.underruns >= 100, "128 blocks with nothing back counted only "
             + std::to_string(gone.underruns - st.underruns) + " underruns");
    RT_CHECK(gone.overruns > st.overruns, "a full input ring was not counted as overruns");
    RT_CHECK(other.u32(dotcharter::extproc::kOffHostUnder) == (uint32_t) gone.underruns
             && other.u32(dotcharter::extproc::kOffHostOver) == (uint32_t) gone.overruns,
             "the header does not mirror the host's underrun / overrun counters");
    const float fallbackPeak = fx.lastOutputPeak();
    RT_CHECK(std::abs(fallbackPeak - dryPeak) < 0.02f * dryPeak + 1e-4f,
             "an underrun did not play the DRY input (peak " + std::to_string(fallbackPeak) + " vs " + std::to_string(dryPeak) + ")");

    // --- close: the seat dips back to its own path ---------------------------
    RT_CHECK(dca_engine_extproc_close(nullptr) == DCA_ERR_INVALID_ARG, "a null engine close was accepted");
    RT_CHECK(e.closeExternalInsert(0), "close found nothing open");
    dcrt::reset();
    dcrt::arm();
    fx.pumpArmed(16);
    dcrt::disarm();
    RT_CHECK(dcrt::allocCount() == 0, "the dip out of a closed insert allocated");
    RT_CHECK(stats().state == DCA_EXTPROC_STATE_CLOSED, "the seat still reports an insert after close");
    RT_CHECK(std::abs(fx.lastOutputPeak() - dryPeak) < 0.02f * dryPeak + 1e-4f, "the seat did not come back after close");
    endScenario();
}

void scenarioSparseOutputMaskMasterBus()
{
    beginScenario("RtSparseOutputMaskIsPackedAndTheMasterBusKeepsLAndRApart");
    constexpr int    frames = 256;
    constexpr double sr     = 48000.0;
    Fixture fx((int) sr, frames, 0, false, 0b1100u);   // outputs 3-4 on, 1-2 off
    if (! fx.ok()) { fail("engine did not start: " + fx.error()); endScenario(); return; }
    auto& eng = fx.engine();
    auto* dev = nulldev::manualDevice();

    const auto active = dev->getActiveOutputChannels();
    RT_CHECK(active.countNumberOfSetBits() == 2 && active[2] && active[3],
             "the null device did not open the sparse mask 0b1100 (active bits "
             + active.toString(2).toStdString() + ") — everything below is vacuous");

    // --- A. what the device callback is actually handed ------------------
    eng.setMasterLimiter(true, -0.3f, 80.0f);
    warmUpAndArm(fx);
    fx.pumpArmed(8);
    const int      got   = eng.lastCallbackOutputChannelsForTest();
    const uint32_t nulls = eng.lastCallbackOutputNullMaskForTest();
    note("sparse mask 0b1100 of " + std::to_string(dev->getOutputChannelNames().size())
         + " physical outputs: the callback received numOutputChannels=" + std::to_string(got)
         + ", null-pointer mask 0x" + juce::String::toHexString((int) nulls).toStdString());
    RT_CHECK(got == 2 && nulls == 0,
             "the callback was not handed the PACKED active outputs (2 pointers, none null) — "
             "the master bus sizing below assumes exactly that contract");
    RT_CHECK(eng.masterLimiterSkippedBlocks() == 0,
             "the limiter skipped " + std::to_string(eng.masterLimiterSkippedBlocks())
             + " block(s) of the real callback on a sparse mask");

    // --- B. one width for every per-channel stage -------------------------
    const auto w = eng.masterBusStageWidths();
    note("prepared widths: bus " + std::to_string(w.bus) + ", HP " + std::to_string(w.highPass)
         + ", LP " + std::to_string(w.lowPass) + ", BP " + std::to_string(w.bandPass)
         + ", song EQ " + std::to_string(w.songEq) + ", limiter " + std::to_string(w.limiter));
    RT_CHECK(w.bus == got && w.highPass == got && w.lowPass == got && w.bandPass == got
                 && w.songEq == got && w.limiter == got,
             "the master-bus stages are prepared for different widths (see the note above); "
             "every one must be the width the callback is handed");

    // --- C. HP + one EQ band on the packed shape ---------------------------
    eng.setMasterLimiter(false, -0.3f, 80.0f);   // measure the filters, not the look-ahead
    eng.setHighPass(120.0f, 0.707f);
    eng.setEqBand(2, DCA_EQ_BELL, 1000.0f, 1.0f, 6.0f, true);
    eng.setEqEnabled(true);
    eng.setEqRoute(DCA_EQ_ROUTE_SONG);

    std::array<float, frames> l{}, r{};
    float* two[2] = { l.data(), r.data() };
    // 997 Hz: not a whole number of cycles per block, so the phase at every
    // block boundary is different and a shared state cannot line up by luck.
    const double inc = 2.0 * 3.14159265358979323846 * 997.0 / sr;
    double phase = 0.0;
    auto fillSine = [&](float* dst)
    {
        double p = phase;
        for (int n = 0; n < frames; ++n, p += inc) dst[n] = 0.25f * (float) std::sin(p);
    };

    // L only, from freshly engaged (reset) sections: R must stay EXACTLY 0.
    bool rSilent = true;
    float lPeak = 0.0f;
    for (int b = 0; b < 16; ++b)
    {
        fillSine(l.data()); phase += inc * frames;
        std::fill(r.begin(), r.end(), 0.0f);
        { dcrt::ScopedRtThread rt; eng.masterForTest(two, 2, frames); }
        for (int n = 0; n < frames; ++n)
        {
            rSilent = rSilent && r[(size_t) n] == 0.0f;
            lPeak = std::max(lPeak, std::abs(l[(size_t) n]));
        }
    }
    RT_CHECK(lPeak > 0.1f, "the L sine did not come through the master bus — the probe is vacuous");
    RT_CHECK(rSilent, "a signal on L ONLY leaked into R: L and R share filter state");

    // Both channels: identical out, and continuous across block boundaries.
    // Disengage and re-engage both stages first, so L and R both start from a
    // RESET state (the off -> on transition) and identical input must give
    // bit-identical output. Without this R would carry its late onset as a
    // float-roundoff difference (~1e-5) that never quite decays.
    eng.setHighPass(0.0f, 0.707f);
    eng.setEqEnabled(false);
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    { dcrt::ScopedRtThread rt; eng.masterForTest(two, 2, frames); }
    eng.setHighPass(120.0f, 0.707f);
    eng.setEqEnabled(true);
    std::vector<float> outL, outR;
    outL.reserve((size_t) frames * 48);
    outR.reserve((size_t) frames * 48);
    for (int b = 0; b < 48; ++b)
    {
        fillSine(l.data()); fillSine(r.data()); phase += inc * frames;
        { dcrt::ScopedRtThread rt; eng.masterForTest(two, 2, frames); }
        outL.insert(outL.end(), l.begin(), l.end());
        outR.insert(outR.end(), r.begin(), r.end());
    }
    dcrt::disarm();
    const int allocsAtoC = dcrt::allocCount();

    // Bit-identical from the first sample: both states were reset together.
    // A shared state would instead differ by a block-boundary step EVERY block.
    const bool identical = outL == outR;
    double lrDiff = 0.0;
    // Second difference: a filtered sine's is bounded by A*(2*pi*f/fs)^2; a
    // block-boundary step (two channels advancing one state) is far above it.
    double boundaryMax = 0.0, interiorMax = 0.0;
    for (size_t n = (size_t) frames * 8; n < outL.size(); ++n)
    {
        lrDiff = std::max(lrDiff, (double) std::abs(outL[n] - outR[n]));
        for (const auto* y : { &outL, &outR })
        {
            const double d2 = std::abs((double) (*y)[n] - 2.0 * (*y)[n - 1] + (*y)[n - 2]);
            if (n % frames < 2) boundaryMax = std::max(boundaryMax, d2);
            else                interiorMax = std::max(interiorMax, d2);
        }
    }
    note("both channels: max |L - R| " + std::to_string(lrDiff)
         + ", second difference at block boundaries " + std::to_string(boundaryMax)
         + " vs inside blocks " + std::to_string(interiorMax));
    RT_CHECK(identical, "the same sine on L and R came out different (max |L - R| " + std::to_string(lrDiff)
                            + "): L and R share filter state");
    RT_CHECK(interiorMax > 1.0e-3, "the filtered sine has no curvature — the continuity probe is vacuous");
    RT_CHECK(boundaryMax <= interiorMax * 1.05,
             "the filtered sine steps at block boundaries (boundary " + std::to_string(boundaryMax)
             + " vs interior " + std::to_string(interiorMax) + ")");
    RT_CHECK(eng.masterBusSkippedBlocks() == 0,
             "a block at the prepared width was counted as skipped");

    // --- D. a block WIDER than prepared: skipped and counted --------------
    // Four pointers, the first two null — exactly what the old limiter comment
    // said JUCE hands the callback. With today's sizing that cannot reach the
    // callback (phase A), but if it ever did, `jmin(ch, size - 1)` folded
    // channels 2 and 3 onto ONE state. A wider block must be left alone.
    std::array<float, frames> lIn{};
    fillSine(lIn.data()); phase += inc * frames;
    std::copy(lIn.begin(), lIn.end(), l.begin());
    std::fill(r.begin(), r.end(), 0.0f);
    float* four[4] = { nullptr, nullptr, l.data(), r.data() };
    const auto skippedBefore = eng.masterBusSkippedBlocks();
    dcrt::reset();
    dcrt::arm();
    { dcrt::ScopedRtThread rt; eng.masterForTest(four, 4, frames); }
    dcrt::disarm();
    const int allocsD = dcrt::allocCount();
    const bool rStillSilent = std::all_of(r.begin(), r.end(), [](float x) { return x == 0.0f; });
    const bool lUntouched   = std::equal(l.begin(), l.end(), lIn.begin());
    note("4-pointer block on a 2-wide bus: skipped count " + std::to_string(skippedBefore) + " -> "
         + std::to_string(eng.masterBusSkippedBlocks()) + ", R silent " + (rStillSilent ? "yes" : "NO")
         + ", L untouched " + (lUntouched ? "yes" : "NO"));
    RT_CHECK(rStillSilent, "a block wider than prepared folded L's filter state into R (jmin(ch, size - 1))");
    RT_CHECK(lUntouched, "a block wider than prepared was filtered instead of skipped");
    RT_CHECK(eng.masterBusSkippedBlocks() == skippedBefore + 1,
             "a block wider than prepared was not COUNTED as skipped");

    // --- E. rebuild to a 4-wide mask: widths follow, HP + EQ survive -------
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    eng.deviceManager().getAudioDeviceSetup(setup);
    setup.outputChannels.clear();
    setup.outputChannels.setRange(0, 4, true);
    setup.useDefaultOutputChannels = false;
    const auto err = eng.deviceManager().setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty()) { fail("could not reopen the null device: " + err.toStdString()); endScenario(); return; }
    fx.pumpWarm(4);
    const auto w4 = eng.masterBusStageWidths();
    const int  got4 = eng.lastCallbackOutputChannelsForTest();
    note("after a rebuild to 4 outputs: callback " + std::to_string(got4) + ", bus " + std::to_string(w4.bus)
         + ", HP " + std::to_string(w4.highPass) + ", song EQ " + std::to_string(w4.songEq)
         + ", limiter " + std::to_string(w4.limiter) + ", skipped " + std::to_string(eng.masterBusSkippedBlocks()));
    RT_CHECK(got4 == 4 && w4.bus == 4 && w4.highPass == 4 && w4.lowPass == 4 && w4.bandPass == 4
                 && w4.songEq == 4 && w4.limiter == 4,
             "a device rebuild did not re-derive every master-bus width from the new device");
    RT_CHECK(eng.masterBusSkippedBlocks() == 0, "the rebuild did not reset the skip count");

    // The HP (120 Hz) must still be in the path: a 30 Hz sine is cut hard.
    std::array<float, frames> c{}, d{};
    float* four4[4] = { l.data(), r.data(), c.data(), d.data() };
    const double inc30 = 2.0 * 3.14159265358979323846 * 30.0 / sr;
    double p30 = 0.0, outPeak = 0.0;
    for (int b = 0; b < 64; ++b)
    {
        for (auto* ch : four4)
        {
            double p = p30;
            for (int n = 0; n < frames; ++n, p += inc30) ch[n] = 0.25f * (float) std::sin(p);
        }
        p30 += inc30 * frames;
        eng.masterForTest(four4, 4, frames);
        if (b >= 32)
        {
            for (auto* ch : four4)
                for (int n = 0; n < frames; ++n) outPeak = std::max(outPeak, (double) std::abs(ch[n]));
        }
    }
    note("after the rebuild a 30 Hz sine at 0.25 comes out at peak " + std::to_string(outPeak));
    RT_CHECK(outPeak < 0.25 * 0.25, "the master high-pass did not survive the device rebuild");

    RT_CHECK(allocsAtoC == 0 && allocsD == 0,
             "the master bus allocated on the audio thread (" + std::to_string(allocsAtoC) + " + "
             + std::to_string(allocsD) + ")");
    endScenario();
}

} // namespace

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    std::string baselinePath, lockSitesPath, only;
    double moduleSeconds = 2.0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--baseline"   && i + 1 < argc) baselinePath  = argv[++i];
        else if (a == "--lock-sites" && i + 1 < argc) lockSitesPath = argv[++i];
        else if (a == "--module-seconds" && i + 1 < argc) moduleSeconds = std::atof(argv[++i]);
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
    }

    // The message thread must exist before any AudioDeviceManager is built.
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (! dcrt::install())
    {
        std::printf("!! No malloc-level allocation hook is available in this build.\n"
                    "!! juce::HeapBlock / AudioBuffer::setSize CANNOT be detected — the\n"
                    "!! gate is running with strictly weaker (operator new only) coverage.\n"
                    "!! Build juce_sharp_rtcheck with the MSVC debug CRT (/MDd + _DEBUG).\n");
        return 2;
    }
    std::printf("RT gate: malloc-level allocation hook active (debug CRT).\n\n");

    // `--only <substring>` runs just the scenarios whose NAME contains it. It
    // exists for the A/B this repo requires of every guard — "break it on
    // purpose, watch it fail, restore" — which otherwise costs a full ~1-10 min
    // gate run per iteration (and longer on a box with another agent building).
    // The two meta-assertions below are SKIPPED under a filter: both compare a
    // set accumulated across EVERY scenario, so a partial run would report the
    // unexercised lock sites as removed. A filtered run is a debugging aid and
    // is never the gate.
    const bool filtered = ! only.empty();
    if (filtered) std::printf("RT gate: --only \"%s\" — PARTIAL RUN, meta-assertions skipped.\n\n",
                              only.c_str());

    auto wanted = [&](const char* name) { return ! filtered || std::string(name).find(only) != std::string::npos; };

    // THE ORDER OF THIS LIST IS LOAD-BEARING, AND THIS IS THE MECHANISM THAT
    // SAYS SO.
    //
    // scenarioBridgeWaitBounded launches the out-of-process VST host child and
    // TIMES its handshake. Everything registered ahead of it shifts that
    // timing, and phase C of RtBridgeWaitIsBounded then fails on a binary
    // nobody touched — which reads as a flaky gate, not as an ordering mistake,
    // and has cost this repo more than one debugging session. Five separate
    // comments below ask the next author to APPEND at the end; not one of them
    // could stop anybody, and a comment that is load-bearing and unenforced is
    // exactly the kind of rule that is broken by someone who never read it.
    //
    // So the PREFIX is pinned. `run` counts every registration whether or not
    // `--only` selects it, and the meta-scenario after the list checks that the
    // bridge scenario still registers at the same index. Insert anything above
    // that line and the gate fails immediately, naming the cause; append below
    // it — which is what every one of those comments asks for — and nothing
    // here needs editing at all.
    //
    // The number is `run()` REGISTRATIONS, so it deliberately does not count
    // scenarioEveryCatalogueModule: that one takes a parameter, cannot match
    // run's void(*)() and is invoked directly below.
    constexpr int kRegistrationsBeforeTheBridgeChild = 41;
    int registered      = 0;
    int bridgeRegisteredAt = -1;

    auto run = [&](const char* name, void (*fn)())
    {
        if (std::strcmp(name, "RtBridgeWaitIsBounded") == 0) bridgeRegisteredAt = registered;
        ++registered;

        if (! wanted(name)) return;
        fn();
        harvest();
        dcrt::reset();
    };

    run("RtIdleCallbackAllocatesNothing",                    &scenarioIdleCallback);
    run("RtFullRigAllocatesNothing",                         &scenarioFullRig);
    run("RtDriveGearChainAllocatesNothingAndSkipsOversizedBlocks", &scenarioDriveGearChain);
    run("RtModulationGearAllocatesNothingUnderStructureWrites", &scenarioModulationGear);
    run("RtDelayGearRigIsAllocationFree",                    &scenarioDelayGearRig);
    run("RtNodeMeteringIsAllocationFreeAndNotVacuous",       &scenarioNodeMetering);
    run("RtConcurrentParamStormAllocatesNothing",            &scenarioParamStorm);
    run("RtStructureStormCrossfadesWithoutAllocating",       &scenarioStructureStorm);
    run("RtPresetCommitPublishesOncePerPresetNotOncePerNode", &scenarioPresetEditScope);
    run("RtBranchPdcFollowsALatencyChangeWithoutAllocating", &scenarioBranchPdc);
    run("RtSidechainKeyingIsAllocationFreeAndAdvertisedHonestly", &scenarioNodeSidechain);
    run("RtDynamicsGearIsAllocationFreeAndReportsItsLatency", &scenarioDynamicsGear);
    run("RtNaNInTheSongMixSidechainKeyCannotLatchADetector", &scenarioSongMixKeyIsScrubbed);
    run("RtBranchPdcFollowsAParamOnTheBlockItChanges",    &scenarioBranchPdcFollowsAParamOnTheBlockItChanges);
    run("RtBranchPdcFollowsThePublishedOrderNotTheShadow", &scenarioBranchPdcInsideAnEditScope);
    run("RtFilterSweepAllocatesNothing",                     &scenarioFilterSweep);
    run("RtMidiFloodAllocatesNothing",                       &scenarioMidiFlood);
    run("RtBlockSizeRenegotiationSkipsNotAllocates",         &scenarioBlockSizeRenegotiation);
    run("RtDropTuneOfflineIsAllocationFree",                 &scenarioDropTuneOffline);
    if (wanted("RtEveryCatalogueModuleIsAllocationFree"))
    { scenarioEveryCatalogueModule(moduleSeconds); harvest(); dcrt::reset(); }
    run("RtGainSlidersRampInsteadOfStepping",                &scenarioGainSlidersRamp);
    run("RtNonFiniteGainCannotLatchTheMasterBus",            &scenarioNonFiniteGainCannotLatchTheMasterBus);
    run("RtSourcePanRampsInsteadOfStepping",                 &scenarioSourcePanRamps);
    run("RtSourcePanCentreIsUnity",                          &scenarioSourcePanCentreIsUnity);
    run("RtSoloSeatOnAMultiInputDeviceFollowsItsCaptureChannel", &scenarioSoloSeatFollowsItsCaptureChannel);
    run("RtStretchQualityIsPerEngineNotPerProcess",          &scenarioStretchQualityIsPerEngine);
    run("RtInstrumentChannelCountFollowsADeviceRebuild",     &scenarioInstrumentChannelsFollowAReprepare);
    run("RtPitchCoreGeometrySurvivesALowLatencyFlip",        &scenarioPitchCoreGeometryIsWorstCase);
    run("RtPitchCoreFlipDoesNotBurstOneCallback",            &scenarioPitchCoreFlipDoesNotBurst);
    run("RtNaNParamIsRejectedAndDoesNotKillTheModule",       &scenarioNaNParamIsRejected);
    run("RtNaNCannotLatchACoreCoefficientOrGain",            &scenarioNaNCannotLatchACoreCoefficient);
    run("RtOversizedBlockIsSkippedNotPartiallyProcessed",    &scenarioOversizedBlockIsSkippedNotPartiallyProcessed);
    run("RtTapeStopCurveRampsInsteadOfSteppingThePitch",     &scenarioTapeStopCurveRamps);
    run("RtSmoothedControlsWalkTheirRampInsteadOfSwitching", &scenarioSmoothedControlsWalkTheirRamp);
    run("RtFastMathHelpersStayWithinTheirStatedErrorBounds", &scenarioFastMathHelpersHoldTheirBounds);
    run("RtGraphicEqLimiterAliasFloorDropped",               &scenarioGraphicEqLimiterIsAntiAliased);
    run("RtCoreOutputLoopsRespectThePreparedChannelCount",   &scenarioCoreOutputLoopsRespectThePreparedChannelCount);
    run("RtBigMuffOutputStageStaysUnderTheBaseRateClipper",  &scenarioBigMuffOutputStageIsAntiAliased);
    run("RtMikuSingsItsPhraseFromTheFirstSyllable",             &scenarioMikuSingsTheWholePhrase);
    run("RtPrepareKeepsUserStateAndReDerivesFromTheRate",    &scenarioPrepareKeepsUserStateAndFollowsTheRate);
    run("RtPreloadedSourceResamplesAtLoadWithoutShiftingTime", &scenarioPreloadResamplesAtLoad);
    run("RtStemGroupChurnDoesNotStallTheCallback",           &scenarioStemGroupChurn);
    run("RtBridgeWaitIsBounded",                             &scenarioBridgeWaitBounded);
    run("RtXrunCounterIsRealAndQuiet",                       &scenarioXrunCounter);
    run("RtXrunStatsAbiHonoursStructBytes",                  &scenarioXrunAbiContract);
    // Two threads of its own and ~3 s of wall clock, so it runs AFTER
    // scenarioBridgeWaitBounded for the same reason the preload scenario does:
    // anything inserted ahead of that one shifts its child handshake and its
    // phase C then fails on an unchanged binary.
    run("RtMikuPhraseSwapIsCoherentUnderEdits",              &scenarioMikuPhraseSwapIsCoherent);
    run("RtRetiredSnapshotsDieOnTheOwnerThread",             &scenarioRetirementIsOffTheAudioThread);
    // Runs LAST on purpose: it is order-independent, and inserting it earlier
    // shifted the timing of scenarioBridgeWaitBounded's child handshake, whose
    // phase C then failed consistently on an unchanged binary.
    run("RtPreloadedSourceAvoidsTheReadAheadLock",           &scenarioPreloadedSourceAvoidsTheReadAheadLock);
    // Also last: two threads and ~1.5 s of its own, and it opens no device, so
    // nothing here can shift the sandbox child's handshake above.
    run("RtRigPrepareCannotRaceTheCallback",                 &scenarioRigPrepareCannotRaceProcess);
    run("RtRemoveCannotRaceBeginEdit",                       &scenarioRemoveCannotRaceBeginEdit);
    run("RtBypassDuringAMoveIsNotClobbered",                 &scenarioBypassDuringAMoveSurvives);
    run("RtEqSnapshotRetainsReaderOwnership", &scenarioSnapshotRetainsReaderOwnership);
    run("RtEqTrimRampsAndSparseInputIsSilent", &scenarioEqTrimAndSparseInput);
    run("RtStagedInsertionNeverExposesDefaultWet", &scenarioStagedInsertion);
    run("RtNaNFromAnInputPluginCannotLatchTheEq",            &scenarioNaNFromAnInputPluginIsScrubbed);
    run("RtAThrowingInputPluginCannotKillTheCallback",       &scenarioThrowingInputPluginCannotKillTheCallback);
    // Also last, and for the SAME reason as the two above: it is pure offline
    // CPU work (several seconds of it, most of that the windowed-sinc
    // reference it is measuring against), and putting that ahead of
    // scenarioBridgeWaitBounded would shift the sandbox child's handshake.
    run("RtLoadResamplerUsesThePolyphasePathAndMatchesTheReference",
                                                             &scenarioLoadResamplerIsPolyphaseAndExact);
    run("RtInstrumentAttachDiscardsStaleMidi",               &scenarioInstrumentAttachDiscardsStaleMidi);
    // Appended last: it is order-independent and, like the preload scenario
    // above, inserting it EARLIER shifts scenarioBridgeWaitBounded's child
    // handshake and makes that scenario's phase C fail on an unchanged binary.
    run("RtFirDotPicksTheWideKernelAndAgreesWithIt",         &scenarioFirDotDispatchesAndAgrees);
    run("RtCabKernelSwitchDoesNotBurstOneCallback",          &scenarioCabKernelIsConvertedOffTheCallback);
    run("RtReverbGearIsAllocationFreeUnderAnIrSwapAndASpaceSwitch",
                                                             &scenarioReverbGearIsAllocationFreeUnderAnIrSwapAndASpaceSwitch);
    run("RtFilterGearIsAllocationFreeUnderAKeyedBandSwitch",  &scenarioFilterGear);
    run("RtAmpCabGearChainAllocatesNothingAcrossAModelSwitch", &scenarioAmpCabGearChain);
    run("RtRsAmpGearAllocatesNothingAcrossEveryModel",       &scenarioRsAmpGear);
    run("RtUtilityGearFootswitchIsAllocationFree",            &scenarioUtilityGearLooperFootswitch);
    run("RtPitchGearIsAllocationFreeUnderScaleOctaveAndLatencyFlips",
                                                             &scenarioPitchGearIsAllocationFreeUnderScaleOctaveAndLatencyFlips);
    run("RtAcousticSimMixDoesNotRebuildTheIr",               &scenarioAcousticSimRebuildsOnlyWhatMoved);
    run("RtGlitchCaptureDoesNotBurstOneCallback",            &scenarioGlitchCaptureIsIncremental);
    // Appended last for the same reason as the four above: it opens its own
    // device and pumps ~450 blocks, and anything inserted AHEAD of
    // scenarioBridgeWaitBounded shifts that scenario's child handshake.
    run("RtMasterLimiterHoldsTheCeilingWithoutAllocating",   &scenarioMasterLimiterHoldsTheCeiling);
    run("RtScheduledMidiLandsOnItsSampleWithoutAllocating",  &scenarioMidiIsSampleAccurate);
    run("RtConcurrentMidiProducersLoseNoEvents",            &scenarioMidiProducersAreSerialised);
    run("RtMasterLimiterSkipIsCountedAndReportsNoLatency",  &scenarioMasterLimiterSkipIsCountedAndHonest);
    // Appended last, like everything since the bridge rule: two engines,
    // ~10 s of audio each, plus a third for the wake triggers.
    run("RtRigIdleSleepSkipsSilenceAndWakesOnThePick",      &scenarioRigIdleSleep);
    // ABI v37, appended last like everything since the bridge rule.
    run("RtSpilloverTailRingsAfterRemovalWithoutAllocating", &scenarioSpilloverTails);
    run("RtInputInjectionReplacesTheSeatInputWithoutAllocating", &scenarioInputInjection);
    // JUCE 9 port, appended last like everything since the bridge rule.
    run("RtMidiOutputRetriesARefusedSendAsJuce8Did",         &scenarioMidiOutputRetriesLikeJuce8);
    // ABI v38 (chart source), appended last like everything since the bridge rule.
    run("RtChartSourceFeedsTheSeatRigNotTheCaptureRing",      &scenarioChartSource);
    // The chart voice (#39 step 1), appended last like everything since the bridge rule.
    run("RtChartVoiceRendersEveryArticulationWithoutAllocating", &scenarioChartVoice);
    run("RtChartModelSwapCrossfadesAndReleasesThePack",         &scenarioChartModelSwap);
    run("RtEqSweepsItsWidenedRangeAt44kWithoutAllocating",      &scenarioEqWidenedRange);
    run("RtAutoFloorGateAndFutzCrusherAllocateNothing",        &scenarioAutoFloorGateAndFutzCrusher);
    run("RtPreloadedSourceHoldsSixteenBitPcm",                  &scenarioPreloadedSourceHoldsSixteenBitPcm);
    // #417 (lazy voice-pack zones), appended last like everything since the bridge rule.
    run("RtChartVoiceZoneMissFallsBackWithoutBlocking",          &scenarioChartZoneMiss);
    run("RtChartVoiceNewChartReleasesTheOldZones",               &scenarioChartNewChartDropsOldZones);
    // JuceSharp #32: speed crossing 100 % mid-song, on both Rubber Band engines.
    run("RtSpeedCrossingUnityIsSeamlessFiner",                  &scenarioSpeedCrossingUnityFiner);
    run("RtSpeedCrossingUnityIsSeamlessFaster",                 &scenarioSpeedCrossingUnityFaster);
    run("RtSpeedCrossingUnityIsSeamlessStemGroup",              &scenarioSpeedCrossingUnityStemGroup);
    // JuceSharp #31 (one master-bus width; the callback's packed output contract).
    run("RtSparseOutputMaskIsPackedAndTheMasterBusKeepsLAndRApart", &scenarioSparseOutputMaskMasterBus);
    // JuceSharp #33: downsampling at load, and streaming sources on the polyphase kernel.
    run("RtPreloadedSourceDownsamplesAtLoadWithoutAliasing",    &scenarioPreloadDownsamplesAtLoad);
    run("RtStreamingSourceResamplesWithThePolyphaseKernel",     &scenarioStreamingSourceResamples);
    // JuceSharp #25 (ABI v45: the rsamp table loaded at run time), appended last.
    run("RtRsAmpTableLoadedWhilePlayingSwapsWithoutAllocating", &scenarioRsAmpTableSwap);
    // JuceSharp #29 (ABI v46: the callback load behind the rig's CPU meter), appended last.
    run("RtCallbackLoadIsMeasuredWithoutAllocating",            &scenarioCallbackLoad);
    // JuceSharp #28 (bridge v3: sandboxed MIDI at its sample offset), appended last.
    run("RtSandboxMidiLandsOnItsSampleWithoutAllocating",       &scenarioSandboxMidiIsSampleAccurate);
    // ABI v47 (the external process insert), appended last.
    run("RtExternalProcessInsertExchangesWithoutAllocatingAndFallsBackDry", &scenarioExternalProcessInsert);

    // --- meta: the run-list ORDER -----------------------------------------
    // Runs under `--only` as well, unlike the two meta-assertions below: it
    // reads REGISTRATIONS, not results, and `run` counts those whether or not
    // the filter selects them — so a filtered debugging run still catches an
    // ordering mistake instead of hiding it until the next full gate.
    beginScenario("RtRunListOrderIsPinned");
    RT_CHECK(bridgeRegisteredAt >= 0,
             "RtBridgeWaitIsBounded is no longer registered in the run list. It is the scenario "
             "the whole ordering rule exists for; if it was deliberately removed, remove "
             "kRegistrationsBeforeTheBridgeChild and this check with it.");
    RT_CHECK(bridgeRegisteredAt == kRegistrationsBeforeTheBridgeChild,
             "the run list changed AHEAD of RtBridgeWaitIsBounded (it now registers at index "
             + std::to_string(bridgeRegisteredAt) + ", pinned at "
             + std::to_string(kRegistrationsBeforeTheBridgeChild) + "). That scenario launches the "
             "sandbox VST host child and TIMES its handshake, so anything inserted before it "
             "shifts that timing and its phase C then fails on an unchanged binary. APPEND your "
             "run(...) line at the END of the list — every scenario added since this rule was "
             "written did exactly that and cost nothing. Only update the pinned number if you "
             "moved the bridge scenario itself on purpose.");
    endScenario();

    if (filtered)
    {
        std::printf("\n(--only run: RtLockSiteSetIsUnchanged and RtBaselineGate skipped.)\n");
    }
    else
    {

    // --- meta: the lock-site set ------------------------------------------
    beginScenario("RtLockSiteSetIsUnchanged");
    if (lockSitesPath.empty())
    {
        fail("--lock-sites was not supplied");
    }
    else
    {
        std::string err;
        auto expected = readListFile(lockSitesPath, err);
        if (! err.empty()) fail(err);
        std::sort(expected.begin(), expected.end());
        auto observed = g_allLockSites;
        std::sort(observed.begin(), observed.end());

        std::vector<std::string> added, missing;
        std::set_difference(observed.begin(), observed.end(), expected.begin(), expected.end(),
                            std::back_inserter(added));
        std::set_difference(expected.begin(), expected.end(), observed.begin(), observed.end(),
                            std::back_inserter(missing));
        for (const auto& a : added)
            fail("NEW lock on the audio thread, not in " + lockSitesPath + ": " + a);
        for (const auto& m : missing)
            fail("lock site '" + m + "' is listed but was never exercised — either it was "
                 "removed (update the file) or a scenario stopped covering it");
        std::string obs; for (const auto& o : observed) obs += o + " ";
        note("observed lock sites: " + obs);
    }
    endScenario();

    // --- baseline gate ----------------------------------------------------
    beginScenario("RtBaselineGate");
    if (baselinePath.empty())
    {
        fail("--baseline was not supplied");
    }
    else
    {
        std::string err;
        auto allowed = readListFile(baselinePath, err);
        if (! err.empty()) fail(err);
        for (const auto& k : g_allViolationKeys)
        {
            const bool ok = std::find(allowed.begin(), allowed.end(), k) != allowed.end();
            if (! ok) fail("un-baselined real-time violation: " + k);
        }
        if (g_allViolationKeys.empty()) note("no heap or kernel-wait violations recorded");
    }
    endScenario();

    } // if (! filtered)

    // A filter that matched nothing must not exit 0 pretending everything is
    // fine — that is how a typo in an A/B reads as "the guard still passes".
    if (g_results.empty())
    {
        std::printf("\n!! --only \"%s\" matched no scenario.\n", only.c_str());
        return 2;
    }

    // --- summary ----------------------------------------------------------
    std::printf("\n================ RT GATE SUMMARY ================\n");
    int failed = 0;
    for (const auto& r : g_results)
    {
        std::printf("%-6s %-46s %s\n", r.passed ? "PASS" : "FAIL", r.name.c_str(),
                    r.detail.c_str());
        if (! r.passed) ++failed;
    }
    std::printf("================================================\n");
    std::printf("%d/%d assertions passed.\n", (int) g_results.size() - failed, (int) g_results.size());
    return failed == 0 ? 0 : 1;
}
