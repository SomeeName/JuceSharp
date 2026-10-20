#include "dsp/cores/dcdsp_dualcab.h"
#include "dsp/DspModuleFactory.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <chrono>
#ifdef _MSC_VER
#include <crtdbg.h>
static thread_local bool armed = false;
static thread_local int allocations = 0;
static int __cdecl allocHook(int kind, void*, size_t, int block, long, const unsigned char*, int) {
    if (armed && (kind == _HOOK_ALLOC || kind == _HOOK_FREE) && block != _CRT_BLOCK) ++allocations;
    return 1;
}
#endif
static int failures = 0, checks = 0;
static void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name); }
}
static constexpr int N = 512;
using Audio = std::array<float, N>;
static constexpr float irA[] = {.5f, .2f, -.1f};
static constexpr float irB[] = {.2f, -.1f, .3f};
static Audio render(dcdsp::DualCabParams p, bool b = true, double rate = 48000., double irRate = 48000.) {
    dcdsp::DualCabCore core;
    core.prepare(48000., 64, 1); core.setParams(p);
    core.slotA().setCustomIr(irA, 3, irRate);
    if (b) core.slotB().setCustomIr(irB, 3, irRate);
    core.prepare(rate, 64, 1); // starts at authored settings, tests re-prepare too
    Audio data{}; data[0] = .1f;
    float* channels[] = {data.data()};
    core.process(channels, 1, N); // deliberately larger than prepared block
    return data;
}
static bool close(const Audio& a, const Audio& b, float tolerance = 1e-7f) {
    for (int i = 0; i < N; ++i) if (std::abs(a[i] - b[i]) > tolerance) return false;
    return true;
}
static void legacy() {
    for (double rate : {44100., 48000., 96000.})
    for (float mix : {0.f, .37f, 1.f})
    for (bool custom : {false, true}) {
        dcdsp::CabParams p; p.mix = mix; p.model = 3;
        dcdsp::CabCore old; old.prepare(rate, 64, 2); old.setParams(p);
        dcdsp::DualCabCore dual; dual.prepare(rate, 64, 2);
        dcdsp::DualCabParams q; static_cast<dcdsp::CabParams&>(q) = p; dual.setParams(q);
        if (custom) { old.setCustomIr(irA, 3, 48000.); dual.slotA().setCustomIr(irA, 3, 48000.); }
        old.prepare(rate, 64, 2); dual.prepare(rate, 64, 2);
        bool exact = true;
        for (int block = 0; block < 8; ++block) {
            std::array<float, 64> a{}, b{}, ar{}, br{};
            for (int i = 0; i < 64; ++i) {
                a[i] = b[i] = .1f * std::sin((block * 64 + i) * .17f);
                ar[i] = br[i] = .03f * std::cos((block * 64 + i) * .23f);
            }
            float* oldChannels[] = {a.data(), ar.data()};
            float* newChannels[] = {b.data(), br.data()};
            old.process(oldChannels, 2, 64); dual.process(newChannels, 2, 64);
            exact &= a == b && ar == br;
        }
        check(exact, "legacy A/default arithmetic is sample-exact at every mix/rate");
    }
}
static void slots() {
    dcdsp::DualCabParams p;
    const auto a = render(p);
    p.blend = 1; const auto b = render(p);
    p.blend = .35f; auto expected = a;
    for (int i = 0; i < N; ++i) expected[i] = a[i] * .65f + b[i] * .35f;
    check(close(render(p), expected), "blend is the weighted A/B output");
    p.blend = 1; check(close(render(p, false), a, 0), "missing B never attenuates A at blend=1");
    p.blend = 0; p.gainADb = 6.0205999f; expected = a;
    for (auto& sample : expected) sample *= 2;
    check(close(render(p), expected), "A gain controls A alone");
    p = {}; p.blend = 1; p.gainBDb = 6.0205999f; expected = b;
    for (auto& sample : expected) sample *= 2;
    check(close(render(p), expected), "B gain controls B alone");
    p = {}; p.invertA = true; expected = a;
    for (auto& sample : expected) sample = -sample;
    check(close(render(p), expected), "A polarity inverts output");
    p = {}; p.blend = 1; p.invertB = true; expected = b;
    for (auto& sample : expected) sample = -sample;
    check(close(render(p), expected), "B polarity inverts output");
    for (bool slotB : {false, true}) {
        p = {}; p.blend = slotB ? 1.f : 0.f;
        if (slotB) p.delayBMs = 1.f; else p.delayAMs = 1.f;
        expected.fill(0.f); auto source = slotB ? b : a;
        for (int i = 48; i < N; ++i) expected[i] = source[i - 48];
        check(close(render(p), expected), "manual alignment shifts selected slot by 48 samples");
    }
    p = {}; p.outputDb = 6.0205999f; expected = a;
    for (auto& sample : expected) sample *= 2;
    check(close(render(p), expected), "output gain applies after combined signal");
    p = {}; p.blend = .5f;
    auto mixedRate = render(p, true, 96000., 44100.);
    check(std::all_of(mixedRate.begin(), mixedRate.end(), [](float s) { return std::isfinite(s); }), "mixed-rate re-prepare finite");
    check(std::any_of(mixedRate.begin(), mixedRate.end(), [](float s) { return std::abs(s) > 1e-5f; }), "mixed-rate re-prepare retains IR audio");

    dcdsp::DualCabCore core; core.prepare(48000., 64, 2);
    p = {}; p.blend = .5f; p.invertB = true;
    core.setParams(p); core.slotA().setCustomIr(irA, 3, 48000.); core.slotB().setCustomIr(irA, 3, 48000.);
    core.prepare(48000., 64, 2);
    Audio left{}, right{}; left[0] = .1f; float* channels[] = {left.data(), right.data()};
    core.process(channels, 2, N);
    check(std::all_of(left.begin(), left.end(), [](float s) { return std::abs(s) < 1e-8f; }), "equal IRs with opposite polarity cancel");
    check(std::all_of(right.begin(), right.end(), [](float s) { return s == 0.f; }), "silent stereo channel stays silent");
    p.invertB = false; core.setParams(p); core.prepare(48000., 64, 2);
    left.fill(0); right.fill(0); right[0] = .1f;
    core.process(channels, 2, N);
    check(std::all_of(left.begin(), left.end(), [](float s) { return s == 0.f; }), "right input cannot bleed into left");
    check(close(right, a), "right channel receives its own full response");

    p.blend = 1; core.setParams(p); core.prepare(48000., 64, 2);
    core.slotB().clearCustomIr();
    left.fill(0); right.fill(0); left[0] = .1f;
    core.process(channels, 2, N);
    check(close(left, a), "clearing B while blend=1 immediately restores full A");
}
static std::filesystem::path wav(int16_t impulse, int serial) {
    auto path = std::filesystem::temp_directory_path() / ("dualcab-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial) + ".wav");
    std::ofstream f(path, std::ios::binary);
    auto u16 = [&](uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); };
    f.write("RIFF",4); u32(38); f.write("WAVEfmt ",8); u32(16);
    u16(1); u16(1); u32(48000); u32(96000); u16(2); u16(16);
    f.write("data",4); u32(2); u16(static_cast<uint16_t>(impulse));
    return path;
}
static void adapter() {
    auto first = wav(16384, 1), second = wav(-8192, 2);
    auto module = dcrig::createModule("cab"); module->prepare(48000., 64, 2);
    check(module->setStringParam("irPath", first.u8string().c_str()), "A file accepted");
    check(module->setStringParam("irPathB", first.u8string().c_str()), "B file accepted");
    check(module->setStringParam("irPathB", second.u8string().c_str()), "latest B file replaces pending first");
    check(module->setParam("blend", 1), "blend advertised and accepted");
    std::string name;
    check(!module->setStringParam("irPathB", "this-dualcab-ir-does-not-exist.wav"), "failed load rejected");
    check(module->getStringParam("irPathB", name) && name == second.u8string(), "failed B load preserves last accepted path");
    Audio l{}, r{}; float* channels[] = {l.data(), r.data()};
#ifdef _MSC_VER
    allocations = 0; armed = true;
#endif
    module->process(channels, 2, N); // adopts both mailboxes and blend; oversized block
    module->reset();
    l[0] = .1f; module->process(channels, 2, N);
#ifdef _MSC_VER
    armed = false;
    check(allocations == 0, "A/B mailbox adoption and processing allocate/free nothing");
#endif
    check(l[0] < 0.f, "latest B kernel is audible, not stale first load");
    check(module->setStringParam("irPathB", ""), "B clear accepted");
    module->reset(); l.fill(0); r.fill(0); l[0] = .1f;
    module->process(channels, 2, N);
    check(l[0] > 0.f, "B clear at full blend leaves A audible");
    check(module->getStringParam("irPath", name) && name == first.u8string(), "clearing B preserves A file");
    check(module->setStringParam("irPathB", second.u8string().c_str()), "B reload accepted");
    check(module->setStringParam("irPath", ""), "A clear accepted independently");
    check(module->getStringParam("irPathB", name) && name == second.u8string(), "clearing A preserves B file");
    std::filesystem::remove(first); std::filesystem::remove(second);
}
// A bundled model switch is a RATE CONVERSION of a 2,048-tap table (up to
// 4,096 output taps, six Lanczos taps each, two std::sin per tap). It used to
// run inside the audio callback that drained the adapter's ParamMailbox — for
// BOTH slots, because DualCabCore::setParams forwards one CabParams (model
// included) to each, and slot B is only ever audible with a user IR loaded.
// Nothing allocates, so the RT gate could not see any of it.
static void modelConversionsHappenOffTheCallback() {
    // 44.1 kHz: the conversion is only skipped at 48 kHz (|dRate| < 1 Hz), so
    // the whole hazard is invisible on the rate most desks run at.
    auto module = dcrig::createModule("cab");
    module->prepare(44100., 64, 2);

    // Reach the cores through the adapter the engine uses, so this measures
    // the shipping wiring and not a hand-built core.
    auto& dual = *static_cast<dcdsp::DualCabCore*>(module->coreForTest());
    const long aAfterPrepare = dual.slotA().conversionsForTest();
    const long bAfterPrepare = dual.slotB().conversionsForTest();
    check(aAfterPrepare >= 1, "slot A converted its model at prepare (owner thread)");
    check(bAfterPrepare == 0, "slot B converted nothing at prepare — it has no bundled bank");

    Audio l{}, r{}; float* channels[] = {l.data(), r.data()};
    module->process(channels, 2, 64);   // drain whatever prepare published

    long conversionsSeen = 0;
    for (int m = 0; m < 12; ++m) {
        check(module->setParam("model", static_cast<float>(m)), "model accepted");
        const long before = dual.slotA().conversionsForTest() + dual.slotB().conversionsForTest();
        l.fill(0.f); r.fill(0.f); l[0] = .1f;
        module->process(channels, 2, 64);   // THIS is the callback
        const long after = dual.slotA().conversionsForTest() + dual.slotB().conversionsForTest();
        conversionsSeen += after - before;
        check(dual.slotA().model() == m, "the switched model actually reached the core");
        check(std::any_of(l.begin(), l.begin() + 64, [](float s) { return std::abs(s) > 1e-6f; }),
              "the cab is audible after the switch — a silent core would pass the counter check");
        check(std::all_of(l.begin(), l.begin() + 64, [](float s) { return std::isfinite(s); }),
              "switched model output finite");
    }
    check(conversionsSeen == 0, "a model switch converted a bundled IR ON THE PROCESS CALL");
    check(dual.slotB().conversionsForTest() == 0, "slot B never converts a bundled model");
    std::printf("model switches: %ld conversions on the process call\n", conversionsSeen);
}

// prepare() re-derives everything the RATE owns, and the kernel it derives must
// be the same one the mailbox would have published. If those two disagree, a
// device change silently re-voices the cab.
static void prepareAtEachRateMatchesTheMailboxKernel() {
    for (double rate : {44100., 96000.}) {
        // A: reached the model through prepare() (the in-core conversion).
        dcdsp::CabParams p; p.model = 7; p.mix = 1.f;
        dcdsp::CabCore viaPrepare;
        viaPrepare.setParams(p);
        viaPrepare.prepare(rate, 64, 1);

        // B: prepared at model 0, then handed model 7 already converted, the
        // way dcrig::ModelIrMailbox does it from the owner thread.
        dcdsp::CabParams q; q.model = 0; q.mix = 1.f;
        dcdsp::CabCore viaMailbox;
        viaMailbox.setParams(q);
        viaMailbox.prepare(rate, 64, 1);
        std::vector<float> converted(4096, 0.f);
        const int taps = dcdsp::detail::resampleIr(dcdsp::cabir::kIr48k[7], dcdsp::cabir::kIrLen,
                                                   dcdsp::cabir::kIrRate, converted.data(),
                                                   4096, rate);
        check(taps > 1000, "the reference conversion produced a real kernel");
        viaMailbox.adoptModelKernel(converted.data(), taps, 7);
        viaMailbox.setParams(p);

        Audio x{}, y{}; x[0] = .5f; y[0] = .5f;
        float* cx[] = {x.data()}; float* cy[] = {y.data()};
        viaPrepare.process(cx, 1, N);
        viaMailbox.process(cy, 1, N);

        double energy = 0.0, diff = 0.0;
        for (int i = 0; i < N; ++i) { energy += x[i] * (double) x[i]; diff += std::abs(x[i] - y[i]); }
        check(energy > 1e-6, "the reference impulse response has energy — the comparison is not vacuous");
        check(diff < 1e-6, "prepare() and the mailbox produce the SAME kernel at this rate");
        std::printf("rate %.0f: taps %d, |diff| %.3e over an impulse of energy %.3e\n",
                    rate, taps, diff, energy);
    }
}

int main() {
#ifdef _MSC_VER
    _CrtSetAllocHook(allocHook);
#endif
    legacy(); slots(); adapter();
    modelConversionsHappenOffTheCallback();
    prepareAtEachRateMatchesTheMailboxKernel();
    std::printf("Dual cabinet: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
