// Standalone regression gate: no audio device, JUCE, or third-party models.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <fstream>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include "dsp/IrMailbox.h"
#include "dsp/DspChain.h"
#include "dsp/DspModuleFactory.h"
#include "dsp/cores/dcdsp_overdrive.h"
#include "dsp/cores/dcdsp_distortion.h"
#include "dsp/cores/dcdsp_tapedelay.h"
#include "dsp/cores/dcdsp_springreverb.h"
#include "dsp/cores/dcdsp_platereverb.h"
#include "dsp/cores/dcdsp_acousticsim.h"
#include "dsp/cores/dcdsp_slapback.h"
#include "dsp/cores/dcdsp_shimmer.h"
#include "dsp/cores/dcdsp_cab.h"

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}
#ifdef _MSC_VER
static thread_local bool armed = false;
static thread_local int allocations = 0;
static int __cdecl allocHook(int kind, void*, size_t, int block, long, const unsigned char*, int) {
    if (armed && (kind == _HOOK_ALLOC || kind == _HOOK_FREE) && block != _CRT_BLOCK) ++allocations;
    return 1;
}
#endif

struct Params { std::array<uint32_t, 32> words{}; };
static void mailbox() {
    dcrig::ParamMailbox<Params> box;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint32_t i = 1; i <= 200000; ++i) {
            Params p; p.words.fill(i); box.publish(p);
        }
        done.store(true);
    });
    size_t accepted = 0;
    bool coherent = true;
    Params out;
    while (!done.load()) if (box.consume(out)) {
        ++accepted;
        for (auto v : out.words) coherent &= v == out.words[0];
    }
    writer.join();
    if (box.consume(out)) ++accepted;
    check(coherent && accepted > 0 && out.words[0] == 200000, "mailbox coherent and latest eventually delivered");
    check(!box.consume(out), "unchanged mailbox not reapplied");
    std::printf("mailbox accepted %zu snapshots\n", accepted);
}

static void irPublication() {
    struct Probe {
        size_t accepted = 0;
        bool coherent = true;
        void clearCustomIr() { ++accepted; }
        void setCustomIr(const float* samples, int count, double rate) {
            ++accepted;
            // The mailbox converts to the engine rate on the PUBLISHING thread
            // now, so the rate is a constant and can no longer carry the edit's
            // identity. The COUNT does: it is derived from the sample value, so
            // a reader that pairs one edit's length with another edit's samples
            // is caught exactly as before.
            coherent &= rate == 48000.;
            coherent &= count == expectedCount(samples[0]);
            for (int i = 1; i < count; ++i) coherent &= samples[i] == samples[0];
        }
        static int expectedCount(float marker) { return 1 + (static_cast<int>(marker) % 4000); }
    } probe;
    dcrig::IrMailbox box;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        std::array<float, 4096> ir;
        for (int n = 1; n <= 4000; ++n) {
            ir.fill(static_cast<float>(n));
            box.publish(ir.data(), Probe::expectedCount(static_cast<float>(n)), 48000.);
        }
        done.store(true);
    });
    while (!done.load()) box.applyPending(probe);
    writer.join(); box.applyPending(probe);
    check(probe.coherent && probe.accepted > 0, "IR samples and rate published coherently under edits");
    std::printf("IR coherent snapshots %zu\n", probe.accepted);
}

template<class Core> static void irAdoption(const char* name) {
    Core core; core.prepare(96000., 64, 2);
    dcrig::IrMailbox box;
    // The shipping path: the module tells the mailbox the engine rate in
    // prepare(), so the 48 kHz IRs below are converted HERE, on this thread,
    // and the core's adoption is a copy.
    box.setEngineRate(96000.);
    std::array<float, 4096> ir{}; ir[0] = 1.f; ir[4095] = .01f;
    std::array<float, 64> left{}, right{};
    float* ch[] = {left.data(), right.data()};
    int allocs = 0;
    for (int i = 0; i < 12; ++i) {
        const bool clear = i % 3 == 2;
        box.publish(clear ? nullptr : ir.data(), i % 3 == 0 ? 1 : 4096, 48000.);
#ifdef _MSC_VER
        allocations = 0; armed = true;
#endif
        box.applyPending(core);
        core.process(ch, 2, 64);
#ifdef _MSC_VER
        armed = false; allocs += allocations;
#endif
        check(core.hasCustomIr() != clear, "IR adoption/clear is audible-core state, not only cached path");
        for (auto v : left) check(std::isfinite(v), "IR output finite");
    }
    std::printf("%s IR adoption allocations %d\n", name, allocs);
    check(allocs == 0, "IR adoption must not allocate");
    box.publish(ir.data(), 4096, 48000.); box.applyPending(core);
    core.prepare(48000., 64, 2); box.setEngineRate(48000.);
    check(core.hasCustomIr(), "custom IR survives rate prepare");
}

static void adapterIr() {
    // Bounded PCM WAV exercises the actual string adapter, not just the mailbox.
    const auto path = std::filesystem::temp_directory_path() / ("juce-tone-regression-ir-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    {
        std::ofstream f(path, std::ios::binary);
        auto u16 = [&](uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
        auto u32 = [&](uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); };
        f.write("RIFF", 4); u32(36 + 8192); f.write("WAVEfmt ", 8); u32(16);
        u16(1); u16(1); u32(48000); u32(96000); u16(2); u16(16);
        f.write("data", 4); u32(8192);
        for (int i = 0; i < 4096; ++i) u16(i == 0 ? 16384 : 0);
    }
    const std::string utf8 = path.u8string();
    for (const char* name : {"cab", "acousticsim"}) {
        auto module = dcrig::createModule(name);
        module->prepare(96000., 64, 2);
        std::array<float, 64> l{}, r{}; float* ch[] = {l.data(), r.data()};
        int allocs = 0;
        bool ok = true;
        for (int i = 0; i < 12; ++i) {
            ok &= module->setStringParam("irPath", i % 2 ? "" : utf8.c_str());
            ok &= module->setParam("mix", .75f);
#ifdef _MSC_VER
            allocations = 0; armed = true;
#endif
            module->process(ch, 2, 64);
#ifdef _MSC_VER
            armed = false; allocs += allocations;
#endif
        }
        check(ok && allocs == 0, "real IR adapter adopts without callback allocation");
        std::printf("%s adapter allocations %d\n", name, allocs);
    }
    std::filesystem::remove(path);
}

template<class Core, class P> static double lowEnd(Core& core, const P& p, double sr) {
    core.prepare(sr, 256, 1); core.setParams(p);
    std::array<float, 256> audio{}; float* ch[] = {audio.data()};
    double sum = 0.; size_t count = 0;
    const int blocks = static_cast<int>(sr * 3.) / 256;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < 256; ++i)
            audio[i] = .01f * static_cast<float>(std::sin(6.283185307179586 * 60. * (b * 256 + i) / sr));
        core.process(ch, 1, 256);
        if (b > blocks / 2) for (float v : audio) { sum += double(v) * v; ++count; }
    }
    return std::sqrt(sum / count);
}

template<class Core, class P> static void rate(const char* name, P p) {
    Core core;
    const double a = lowEnd(core, p, 48000.);
    const double b = lowEnd(core, p, 96000.);
    const double db = 20. * std::log10(b / a);
    const double restored = lowEnd(core, p, 48000.);
    check(restored == a, "48k response restored exactly after rate change");
    std::printf("%s 60Hz 48k %.8f 96k %.8f delta %.3fdB\n", name, a, b, db);
    check(a > 1e-6 && b > 1e-6, "rate response fixture audible");
    check(std::abs(db) < 1.2, name);
}

static void overdriveBlend() {
    for (double sr : {44100., 48000., 96000.}) {
        dcdsp::OverdriveCore core; core.prepare(sr, 256, 2);
        dcdsp::OverdriveParams p; p.model = 3; p.drive = 0.f; p.level = 1.f / 1.2f;
        core.setParams(p);
        // Sweep the band containing the old sample-rate-dependent cancellation.
        for (double hz : {2000., 3000., 4000., 5000., 6000.}) {
            core.reset();
            std::array<float, 256> l{}, r{}; float* ch[] = {l.data(), r.data()};
            double sum = 0.; size_t count = 0; bool stereo = true;
            for (int b = 0; b < static_cast<int>(sr) / 256; ++b) {
                for (int i = 0; i < 256; ++i)
                    l[i] = r[i] = .0001f * static_cast<float>(std::sin(6.283185307179586 * hz * (b * 256 + i) / sr));
                core.process(ch, 2, 256);
                for (int i = 0; i < 256; ++i) stereo &= l[i] == r[i] && std::isfinite(l[i]);
                if (b > static_cast<int>(sr) / 512) for (float v : l) { sum += double(v) * v; ++count; }
            }
            const double db = 20. * std::log10(std::sqrt(sum / count) / (.0001 / std::sqrt(2.)));
            std::printf("transparent OD %.0fHz at%.0fkHz %.2fdB\n", hz, sr / 1000., db);
            check(stereo, "overdrive preserves identical stereo channels");
            check(db > -12., "transparent OD must not comb a notch into its passband");
        }
    }
    static constexpr float reference[3][8] = {
        {.0804207548f,.0790445954f,.0760715008f,.0715834945f,.0656884164f,.058519084f,.0502320454f,.0410058647f},
        {.156821892f,.154508621f,.14977625f,.142728209f,.133507892f,.122297943f,.109318852f,.094826445f},
        {.0775183886f,.075900197f,.0728214756f,.068366006f,.0626420379f,.0557809398f,.0479355045f,.039277833f}
    };
    for (int model = 0; model < 3; ++model) {
        dcdsp::OverdriveCore core; core.prepare(48000., 256, 1);
        dcdsp::OverdriveParams p; p.model = model; core.setParams(p);
        std::array<float, 256> x; float* ch[] = {x.data()};
        for (int b = 0; b < 16; ++b) {
            for (int i = 0; i < 256; ++i)
                x[i] = .01f * float(std::sin((b * 256 + i) * .043)) + .02f * float(std::cos((b * 256 + i) * .137));
            core.process(ch, 1, 256);
        }
        // Pre-change rendering, small tolerance only for platform math rounding.
        for (int i = 0; i < 8; ++i) check(std::abs(x[248+i] - reference[model][i]) < 2e-7f, "OD models0-2 legacy reference");
        check(core.latencySamples() == 0, "OD models0-2 latency remains zero");
    }
}

static void overdriveContinuityAndLatency() {
    auto render = [](int block) {
        dcdsp::OverdriveCore core; core.prepare(48000., 256, 1);
        dcdsp::OverdriveParams p; p.model = 3; core.setParams(p);
        std::vector<float> out(4096);
        for (int i = 0; i < 4096; ++i) out[i] = .02f * float(std::sin(i * .043));
        for (int i = 0; i < 4096; i += block) {
            float* ch[] = {out.data() + i}; core.process(ch, 1, block);
        }
        return out;
    };
    check(render(64) == render(256), "transparent OD block partition continuity");
    auto module = dcrig::createModule("overdrive"); module->prepare(48000., 64, 2);
    std::array<float,64> l{},r{}; float* ch[] = {l.data(),r.data()};
    module->setParam("model", 3.f);
    check(module->latencySamples() > 0, "OD latency follows owner configuration before callback adoption");
    module->process(ch,2,64);
    dcdsp::detail::OversamplerNx os; os.prepare(48000.,64,2);
    const int expected = static_cast<int>(std::lround(os.groupDelayBaseSamples() + .25));
    check(expected > 0 && module->latencySamples() == expected, "transparent OD adapter reports dry compensation latency");
    std::printf("transparent OD reported latency %d samples\n", module->latencySamples());
    module->setParam("model", 0.f);
    check(module->latencySamples() == 0, "OD latency does not read stale callback model");
    module->process(ch,2,64);
    check(module->latencySamples() == 0, "OD model0 restores zero reported latency");
}

static void legacyDcReference() {
    dcdsp::DcBlocker old, derived;
    derived.prepareLegacy48k(48000.);
    bool exact = true;
    for (int i = 0; i < 8192; ++i) {
        const float x = static_cast<float>(std::sin(i * .017));
        exact &= old.process(x) == derived.process(x);
    }
    check(exact, "legacy48k DC filter bit-exact reference");
}

// A STRUCTURE EDIT MUST NOT SLEEP OUT A FADE NOBODY IS RENDERING.
//
// fadeOutAndWait derives its stall bound from `maxBlock_`, which is the SIZING
// figure — the scratch carries 2x headroom, so it is >= 2048 frames, i.e. ~43 ms
// a block and an ~85 ms stall bound. On an engine with no device open (an
// offline bounce, a preset built while the transport is stopped) NOTHING is
// ever going to move that fade, so every remove, move and retag paid the full
// bound; on a live engine it was 10 ms per step of a UI drag, off a device
// block that is really 5.3 ms.
//
// Two fixes, and this asserts the structural one: the timing now comes from the
// DEVICE block, and a chain whose process() has never run parks the node at
// once. The counter is the detector — a wall-clock threshold would flake the
// moment two agents build at the same time.
static void structureEditsDoNotWaitOnAChainNobodyIsProcessing() {
    dcrig::DspChain chain;
    chain.prepare(48000., 2048, 2, 256);

    std::vector<int> ids;
    for (const char* type : {"gate", "overdrive", "delay", "reverb"}) {
        auto m = dcrig::createModule(type);
        check(m != nullptr, "catalogue module created");
        if (m != nullptr) ids.push_back(chain.addNode(std::move(m)));
    }
    check(ids.size() == 4 && chain.nodeCount() == 4, "four nodes in the chain");

    check(chain.removeNode(ids[1]), "remove accepted on an unprocessed chain");
    check(chain.moveNode(ids[2], 0), "move accepted on an unprocessed chain");
    check(chain.setBypass(ids[0], true), "bypass accepted on an unprocessed chain");

    check(chain.nodeCount() == 3, "the remove really removed a node");
    check(chain.fadeWaitSleepsForTest() == 0,
          "a structure edit slept waiting for a fade on a chain nothing has ever processed");

    // THE DISCRIMINATING ASSERTION, and it is structural on purpose: the wait
    // bound is 2 x one block, so what matters is WHICH block size it is derived
    // from. A wall-clock assertion on the wait itself would flake the moment
    // two builds run at once.
    check(chain.timingBlockForTest() == 256,
          "fade timing is derived from the SIZING block (>= 2048 frames, ~43 ms each) instead of "
          "the device block — every remove/move/retag waits ~85 ms");

    dcrig::DspChain fallback;
    fallback.prepare(48000., 1024, 2);      // no device block: offline callers
    check(fallback.timingBlockForTest() == 1024,
          "an offline chain with no device block must fall back to the sizing block");

    std::printf("structure edits on an idle chain: %lu ms of fade waiting, timing block %d\n",
                chain.fadeWaitSleepsForTest(), chain.timingBlockForTest());
}

// ABI v33. The category is what a pedalboard palette groups by, so an EMPTY
// one is a module that silently falls out of every drawer — and adding a module
// without one compiles perfectly cleanly, because the field defaults to "". The
// vocabulary is closed on purpose: a typo ("reverbs") would read as a brand new
// drawer holding one pedal, which looks like a layout bug rather than a data
// one. namamp is absent here (DOTCHARTER_DSP_NEURAL=0, see tests/CMakeLists.txt).
static void everyCatalogueEntryHasAKnownCategory() {
    static const char* const kKnown[] = {
        "dynamics", "drive", "amp", "cab", "modulation", "delay",
        "reverb", "pitch", "filter", "eq", "utility", "routing", "synth" };

    const auto& cat = dcrig::moduleCatalogue();
    check(cat.size() >= 56, "catalogue is populated");

    for (const auto& m : cat) {
        bool known = false;
        for (const char* k : kKnown) if (m.category == k) { known = true; break; }
        if (!known) {
            std::printf("FAIL module '%s' has category '%s', which is not in the fixed set\n",
                        m.id.c_str(), m.category.c_str());
            ++failures;
        }
    }

    // The type id a node reports must be the id it was created from, or
    // dca_engine_dsp_get_node_type sends a UI to the wrong parameter list.
    for (const auto& m : cat) {
        auto mod = dcrig::createModule(m.id.c_str());
        check(mod != nullptr, "every catalogue id is constructible");
        if (mod != nullptr)
            check(m.id == mod->typeId(), "module typeId() matches its catalogue id");
    }

    // ... and the chain hands both of them back for a node it holds, which is
    // the whole point of the v33 getters.
    dcrig::DspChain chain;
    chain.prepare(48000., 2048, 2, 256);
    const int id = chain.addNode(dcrig::createModule("delay"), /*initiallyBypassed*/ true);
    std::string type;
    bool bypassed = false;
    check(chain.getNodeTypeId(id, type) && type == "delay", "getNodeTypeId reports the created type");
    check(chain.getBypass(id, bypassed) && bypassed, "getBypass reports a parked node as bypassed");
    check(chain.setBypass(id, false), "un-bypass accepted");
    check(chain.getBypass(id, bypassed) && !bypassed, "getBypass follows setBypass");
    check(!chain.getNodeTypeId(id + 4242, type), "an unknown node id is refused, not answered");
}

int main() {
#ifdef _MSC_VER
    _CrtSetAllocHook(allocHook);
#endif
    everyCatalogueEntryHasAKnownCategory();
    structureEditsDoNotWaitOnAChainNobodyIsProcessing();
    overdriveBlend(); overdriveContinuityAndLatency(); legacyDcReference(); mailbox(); irPublication(); adapterIr(); irAdoption<dcdsp::CabCore>("cab"); irAdoption<dcdsp::AcousticSimCore>("acoustic");
    dcdsp::OverdriveParams od; od.drive = 0.f; rate<dcdsp::OverdriveCore>("overdrive", od);
    dcdsp::DistortionParams dist; dist.gain = 0.f; rate<dcdsp::DistortionCore>("distortion", dist);
    dcdsp::TapeDelayParams tape; tape.mix = 1.f; tape.feedback = .5f; tape.wowFlutter = 0.f; rate<dcdsp::TapeDelayCore>("tapedelay", tape);
    dcdsp::SpringReverbParams spring; spring.mix = 1.f; rate<dcdsp::SpringReverbCore>("spring", spring);
    dcdsp::PlateReverbParams plate; plate.mix = 1.f; rate<dcdsp::PlateReverbCore>("plate", plate);
    dcdsp::AcousticSimParams acoustic; acoustic.mix = 1.f; acoustic.body = 0.f; // isolate DC response from resampled body IR
    rate<dcdsp::AcousticSimCore>("acoustic", acoustic);
    dcdsp::SlapbackParams slap; slap.mix = 1.f; slap.wobble = 0.f; rate<dcdsp::SlapbackCore>("slapback", slap);
    dcdsp::ShimmerParams shimmer; shimmer.mix = 1.f; shimmer.shimmer = 0.f; rate<dcdsp::ShimmerCore>("shimmer", shimmer);
    std::printf("%d failures\n", failures); return failures ? 1 : 0;
}
