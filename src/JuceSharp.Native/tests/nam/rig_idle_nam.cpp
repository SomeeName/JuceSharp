// juce_sharp_rig_idle_nam — idle sleep (dsp/IdleSleep.h) with REAL neural
// captures, over the REAL device callback.
//
// The RT gate and juce_sharp_rig_idle_sweep cannot load a NAM capture (they
// build without NeuralAudio, which needs the release CRT), so `namamp` — the
// module the owner's rig spends 12 of its 13.5 % in — is proven here: a
// shipping-configuration build of the engine (release CRT, NeuralAudio,
// DOTCHARTER_DSP_NEURAL=1) on the manual-pump null device.
//
//   juce_sharp_rig_idle_nam <capture.nam> [<second.nam>]
//       gate -> namamp -> [namamp] -> cab -> eq -> gain, pluck -> idle -> pluck,
//       slept vs never-slept, from the waking block on, must agree within
//       -80 dBFS, or differ by no more (+6 dB) than a never-slept engine does
//       when the pick lands 16 ms later, or sit 100 dB below the note's energy.
//
//   juce_sharp_rig_idle_nam --bench <capture.nam> [<second.nam>] [--frames N]
//       times the device callback per block: playing, idle with sleep off,
//       idle with sleep on (hum + hiss, and digital silence). Asserts nothing.

#include "AudioEngine.h"
#include "NullAudioIODevice.h"
#include "dsp/IdleSleep.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

using namespace dotcharter;

namespace {

int g_failures = 0;
void fail(const std::string& s) { ++g_failures; std::printf("FAIL %s\n", s.c_str()); }
float dbfs(double lin) { return lin > 0.0 ? (float) (20.0 * std::log10(lin)) : -999.0f; }

void setEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    SetEnvironmentVariableA(name, value);
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

constexpr int    kRate = 48000;
constexpr double kPi   = 3.14159265358979323846;

float pluckSample(long long n)
{
    const double t = (double) n / kRate;
    const double env = (t < 0.003 ? t / 0.003 : 1.0) * std::exp(-t / 0.35);
    double v = 0.0;
    for (int k = 1; k <= 8; ++k) v += std::sin(2.0 * kPi * 110.0 * k * t + 0.3 * k) / k;
    return (float) (0.2 * env * v);
}

/// -70 dBFS mains hum plus -90 dBFS hiss (deterministic LCG, keyed by the
/// sample index and `seed`, so two engines see the same samples).
float humSample(long long n, uint32_t seed)
{
    const double t = (double) n / kRate;
    const uint32_t x = (uint32_t) n * 1664525u + 1013904223u + seed * 2654435761u;
    const uint32_t y = (x ^ (x >> 15)) * 2246822519u;
    const float hiss = 3.0e-5f * ((float) (y >> 8) / 8388608.0f - 1.0f);
    return (float) (3.0e-4 * (0.7 * std::sin(2.0 * kPi * 50.0 * t) + 0.3 * std::sin(2.0 * kPi * 150.0 * t + 1.0))) + hiss;
}

class Engine
{
public:
    Engine(int block, bool idleSleep) : block_(block)
    {
        setEnv(nulldev::kEnvVar, "1");
        setEnv(nulldev::kManualEnvVar, "1");
        engine_ = std::make_unique<AudioEngine>();
        dca_engine_config cfg{};
        cfg.sample_rate     = kRate;
        cfg.buffer_frames   = block;
        cfg.output_channels = 2;
        cfg.input_channels  = 2;
        std::strncpy(cfg.output_type_name, nulldev::kTypeName, sizeof(cfg.output_type_name) - 1);
        engine_->setRigIdleSleepForTest(idleSleep);
        error_ = engine_->start(cfg).toStdString();
        engine_->setMasterGain(1.0f);
        engine_->setInputGain(1.0f);
        engine_->setMonitorGain(1.0f);
    }
    ~Engine()
    {
        if (engine_) engine_->stop();
        engine_.reset();
        setEnv(nulldev::kManualEnvVar, nullptr);
        setEnv(nulldev::kEnvVar, nullptr);
    }
    bool ok() const { return error_.empty() && nulldev::manualDevice() != nullptr; }
    const std::string& error() const { return error_; }
    AudioEngine& e() { return *engine_; }

    /// The owner's rig: gate -> namamp(s) -> cab -> eq -> gain, with the saved
    /// profile's settings. Blocks until every capture reports loaded.
    bool buildRig(const std::vector<std::string>& models)
    {
        auto& e = *engine_;
        // RIG_IDLE_NAM_SKIP=gate,cab,eq,gain drops those stages (a bisecting
        // aid for a failure; the gate always runs the full owner rig).
        const char* skipEnv = std::getenv("RIG_IDLE_NAM_SKIP");
        const std::string skip = skipEnv ? skipEnv : "";
        auto want = [&](const char* t) { return skip.find(t) == std::string::npos; };
        const int gate = want("gate") ? e.dspAddNode(0, "gate") : 0;
        e.dspSetParam(gate, "thresholdDb", -46.f); e.dspSetParam(gate, "ratio", 12.f);
        e.dspSetParam(gate, "attackMs", 1.f);      e.dspSetParam(gate, "holdMs", 40.f);
        e.dspSetParam(gate, "releaseMs", 70.f);
        std::vector<int> nams;
        for (size_t i = 0; i < models.size(); ++i)
        {
            const int n = e.dspAddNode(0, "namamp");
            if (n < 0) return false;
            if (i == 1)
            {
                e.dspSetParam(n, "gain", .58f); e.dspSetParam(n, "bass", .65f); e.dspSetParam(n, "mid", .4f);
                e.dspSetParam(n, "treble", .58f); e.dspSetParam(n, "master", .48f); e.dspSetParam(n, "outputDb", 3.8f);
            }
            if (! e.dspSetStringParam(n, "modelPath", models[i].c_str())) return false;
            nams.push_back(n);
        }
        const int cab = want("cab") ? e.dspAddNode(0, "cab") : 0;
        e.dspSetParam(cab, "model", 8.f); e.dspSetParam(cab, "lowCutHz", 95.f); e.dspSetParam(cab, "highCutHz", 8500.f);
        const int eq = want("eq") ? e.dspAddNode(0, "eq") : 0;
        e.dspSetParam(eq, "lowFreq", 110.f); e.dspSetParam(eq, "lowGainDb", 1.f);
        e.dspSetParam(eq, "midFreq", 1200.f); e.dspSetParam(eq, "midGainDb", 3.f);
        e.dspSetParam(eq, "highFreq", 4500.f); e.dspSetParam(eq, "highGainDb", 2.f);
        const int g = want("gain") ? e.dspAddNode(0, "gain") : 0;
        e.dspSetParam(g, "gainDb", 0.03f);
        if (gate < 0 || cab < 0 || eq < 0 || g < 0) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        for (int n : nams)
            for (;;)
            {
                float loaded = 0.f;
                if (e.dspGetParam(n, "loaded", loaded) && loaded == 1.f) break;
                if (std::chrono::steady_clock::now() > deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        return true;
    }

    enum class In { Silence, Hum, Pluck, PluckOnHum };
    /// One device block, with input generated for absolute sample `pos`.
    void pump(In what, long long pluckAt, uint32_t seed = 0u)
    {
        auto* dev = nulldev::manualDevice();
        for (auto& ch : dev->manualInputBuffers())
            for (int i = 0; i < (int) ch.size(); ++i)
            {
                const long long n = pos_ + i;
                float x = 0.f;
                if (what == In::Hum || what == In::PluckOnHum) x += humSample(n, seed);
                if ((what == In::Pluck || what == In::PluckOnHum) && n >= pluckAt) x += pluckSample(n - pluckAt);
                ch[(size_t) i] = x;
            }
        nulldev::pump(1);
        pos_ += block_;
    }
    const std::vector<float>& out0() const { return nulldev::manualDevice()->manualOutputBuffers()[0]; }
    long long pos() const { return pos_; }
    int block() const { return block_; }

private:
    int block_;
    long long pos_ = 0;
    std::unique_ptr<AudioEngine> engine_;
    std::string error_;
};

struct WakeRun
{
    std::vector<float> after;     // monitor output from the waking block on
    bool slept = false;
    bool wokeOnPluck = false;
    std::string error;
};

/// pluck (1 s) -> idle (the hold + 3 s) -> pluck (2 s), recording from the
/// second pluck on. `seed` changes only the idle stretch's hiss.
WakeRun runWake(const std::vector<std::string>& models, bool idleSleep, bool hum, int extraIdleBlocks)
{
    WakeRun r;
    Engine eng(256, idleSleep);
    if (! eng.ok()) { r.error = "engine did not start: " + eng.error(); return r; }
    if (! eng.buildRig(models)) { r.error = "could not build the rig / load the captures"; return r; }
    for (int i = 0; i < 64; ++i) eng.pump(hum ? Engine::In::Hum : Engine::In::Silence, -1);
    const long long first = eng.pos();
    const int blocksPerSec = kRate / eng.block();
    for (int i = 0; i < blocksPerSec; ++i) eng.pump(hum ? Engine::In::PluckOnHum : Engine::In::Pluck, first);
    const long long idleBlocks = dcrig::idleHoldSamples(0.0, kRate) / eng.block() + 3LL * blocksPerSec
                                 + extraIdleBlocks;
    for (long long i = 0; i < idleBlocks; ++i)
    {
        eng.pump(hum ? Engine::In::Hum : Engine::In::Silence, -1);
        r.slept |= eng.e().rigIdleStateForTest(0).asleep();
    }
    const long long second = eng.pos();
    const uint64_t skipped = eng.e().rigIdleStateForTest(0).skippedBlocks();
    for (int i = 0; i < 2 * blocksPerSec; ++i)
    {
        eng.pump(hum ? Engine::In::PluckOnHum : Engine::In::Pluck, second);
        if (i == 0)
        {
            float pk = 0.f;
            for (float v : eng.out0()) pk = std::max(pk, std::fabs(v));
            r.wokeOnPluck = eng.e().rigIdleStateForTest(0).skippedBlocks() == skipped && pk > 0.f;
        }
        r.after.insert(r.after.end(), eng.out0().begin(), eng.out0().end());
    }
    return r;
}

struct Cmp { double peak = 0, diffPeak = 0, sig = 0, diff = 0; double esr() const { return diff > 0 ? 10 * std::log10(diff / sig) : -999; } };
Cmp compare(const std::vector<float>& a, const std::vector<float>& b)
{
    Cmp c;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        const double d = (double) a[i] - b[i];
        c.peak = std::max(c.peak, (double) std::fabs(a[i]));
        c.diffPeak = std::max(c.diffPeak, std::fabs(d));
        c.sig += (double) a[i] * a[i];
        c.diff += d * d;
    }
    return c;
}

int runTests(const std::vector<std::string>& models)
{
    std::string rig = "gate";
    for (const auto& m : models) rig += " -> namamp(" + m.substr(m.find_last_of("/\\") + 1) + ")";
    rig += " -> cab -> eq -> gain";
    std::printf("rig: %s\n", rig.c_str());

    // Determinism first: two never-slept engines fed the same input must agree
    // to the bit, or no comparison below means anything.
    {
        const WakeRun a = runWake(models, false, true, 0);
        const WakeRun b = runWake(models, false, true, 0);
        if (! a.error.empty() || ! b.error.empty()) { fail("determinism: setup failed"); return 1; }
        const Cmp c = compare(a.after, b.after);
        std::printf("determinism: two never-slept engines differ by peak %.1f dBFS\n", dbfs(c.diffPeak));
        if (c.diffPeak != 0.0) fail("two never-slept engines on the same input disagree - the engine is not deterministic");
    }
    // Slept vs never-slept, against the yardstick: a never-slept engine that
    // idled 3 blocks (16 ms) LONGER before the same pick. That is how much the
    // moment of the pick moves the note in a rig that never sleeps - the hum's
    // phase, the hiss, the gate's detector ripple, a filter's last-bit limit
    // cycle - and a sleep may move it by no more (+6 dB), or sit 100 dB below
    // the note's energy.
    for (int h = 0; h < 2; ++h)
    {
        const bool hum = h == 1;
        const std::string name = hum ? "hum    " : "silence";
        const WakeRun on    = runWake(models, true,  hum, 0);
        const WakeRun off   = runWake(models, false, hum, 0);
        const WakeRun later = runWake(models, false, hum, 3);
        if (! on.error.empty() || ! off.error.empty() || ! later.error.empty())
        { fail(name + ": engine/rig setup failed"); return 1; }
        const Cmp c   = compare(off.after, on.after);
        const Cmp ref = compare(off.after, later.after);
        std::printf("%s: slept=%d wokeOnPluck=%d  note peak %.1f dBFS  |slept-never| peak %.1f dBFS esr %.1f dB"
                    "  | yardstick (pick 16 ms later) peak %.1f dBFS esr %.1f dB\n",
                    name.c_str(), (int) on.slept, (int) on.wokeOnPluck, dbfs(c.peak), dbfs(c.diffPeak), c.esr(),
                    dbfs(ref.diffPeak), ref.esr());
        if (! hum && ! on.slept) fail("silence: the rig never slept - the check is vacuous");
        if (on.slept && ! on.wokeOnPluck) fail(name + ": the waking block was not processed");
        if (off.slept) fail(name + ": the never-slept reference slept");
        if (! (c.peak > 0.01)) fail(name + ": the reference note is inaudible - vacuous");
        const bool okDiff = c.diffPeak <= dcrig::idle::kWakeDiffTolerance || c.esr() <= -100.0
                            || (dbfs(c.diffPeak) <= dbfs(ref.diffPeak) + 6.0f && c.esr() <= ref.esr() + 6.0);
        if (! okDiff) fail(name + ": slept vs never-slept differ by more than the moment of the pick does");
        if (hum && ! on.slept) std::printf("       (the rig did not sleep on hum: its output of the hum is above -90 dBFS)\n");
    }
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nall NAM idle-sleep checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}

/// Mean and 99th percentile of the callback's wall time over `blocks` blocks.
struct Timing { double meanUs = 0, p99Us = 0; };
Timing timeBlocks(Engine& eng, int blocks, Engine::In what, long long pluckAt, int repluckEvery = 0)
{
    std::vector<double> us;
    us.reserve((size_t) blocks);
    long long at = pluckAt;
    for (int i = 0; i < blocks; ++i)
    {
        if (repluckEvery > 0 && i % repluckEvery == 0) at = eng.pos();
        const auto t0 = std::chrono::steady_clock::now();
        eng.pump(what, at);
        const auto t1 = std::chrono::steady_clock::now();
        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    Timing t;
    for (double v : us) t.meanUs += v;
    t.meanUs /= (double) us.size();
    std::sort(us.begin(), us.end());
    t.p99Us = us[(size_t) (0.99 * (double) (us.size() - 1))];
    return t;
}

int runBench(const std::vector<std::string>& models, int frames)
{
    const double budgetUs = 1e6 * frames / kRate;
    std::printf("bench: %d frames @ %d Hz = %.1f us of audio per block\n", frames, kRate, budgetUs);
    auto line = [&](const char* label, const Timing& t)
    { std::printf("  %-44s mean %7.1f us = %5.2f %% of a core   p99 %7.1f us\n", label, t.meanUs, 100.0 * t.meanUs / budgetUs, t.p99Us); };
    const int measure = (int) (10.0 * kRate / frames);   // 10 s of audio per line

    // OFF, ON, OFF, ON: each engine is a fresh one in the same process, and the
    // first of a run measured ~15 % cheaper than the second whichever it was,
    // so a single pair would report the ORDER as a cost of idle sleep.
    for (int pass = 0; pass < 4; ++pass)
    {
        const int sleep = pass % 2;
        Engine eng(frames, sleep == 1);
        if (! eng.ok() || ! eng.buildRig(models)) { std::printf("bench: setup failed\n"); return 1; }
        // Warm-up: model prewarm, caches.
        timeBlocks(eng, (int) (2.0 * kRate / frames), Engine::In::PluckOnHum, eng.pos(), (int) (kRate / frames));
        std::printf("idle sleep %s:\n", sleep ? "ON" : "OFF");
        line("playing (a pluck a second, on hum)", timeBlocks(eng, measure, Engine::In::PluckOnHum, eng.pos(), (int) (kRate / frames)));
        // Let the hold pass before measuring idle.
        timeBlocks(eng, (int) (7.0 * kRate / frames), Engine::In::Hum, -1);
        const auto st = eng.e().rigIdleStateForTest(0);
        std::printf("    (after 7 s of hum: asleep=%d)\n", (int) st.asleep());
        line("idle, -70 dBFS hum + -90 dBFS hiss", timeBlocks(eng, measure, Engine::In::Hum, -1));
        timeBlocks(eng, (int) (7.0 * kRate / frames), Engine::In::Silence, -1);
        line("idle, digital silence", timeBlocks(eng, measure, Engine::In::Silence, -1));
    }
    {
        // The engine with NO rig: what the callback costs without the tone rig.
        Engine eng(frames, true);
        if (! eng.ok()) return 1;
        std::printf("no rig at all:\n");
        line("idle, -70 dBFS hum", timeBlocks(eng, measure, Engine::In::Hum, -1));
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    bool bench = false;
    int frames = 128;
    std::vector<std::string> models;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--bench") bench = true;
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else models.push_back(a);
    }
    if (models.empty() || models.size() > 2)
    {
        std::printf("usage: juce_sharp_rig_idle_nam [--bench [--frames N]] <capture.nam> [<second.nam>]\n");
        return 2;
    }
    return bench ? runBench(models, frames) : runTests(models);
}
