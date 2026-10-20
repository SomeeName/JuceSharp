// Device-free tests of the real factory and DspChain, plus a frozen legacy oracle.
#include "dsp/DspChain.h"
#include "dsp/DspModuleFactory.h"
#include "dsp/cores/dcdsp_delay.h"
#include "fixtures/delay_legacy.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

static int failures = 0;
#ifdef _MSC_VER
static thread_local bool audioArmed = false;
static thread_local int audioAllocations = 0;
static int __cdecl allocationHook(int kind, void*, size_t, int block, long, const unsigned char*, int) {
    if (audioArmed && (kind == _HOOK_ALLOC || kind == _HOOK_FREE) && block != _CRT_BLOCK) ++audioAllocations;
    return 1;
}
#endif
static void check(bool ok, const char* name) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}

class LegacyModule final : public dcrig::DspModule {
public:
    void prepare(double sr, int block, int ch) override { core.prepare(sr, block, ch); }
    void reset() override { core.reset(); }
    bool setParam(const char* id, float v) override {
        if (!std::strcmp(id, "timeMs")) p.timeMs = v;
        else if (!std::strcmp(id, "mix")) p.mix = v;
        else if (!std::strcmp(id, "feedback")) p.feedback = v;
        else return false;
        mail.publish(p); return true;
    }
    bool getParam(const char*, float&) const override { return false; }
    void process(float* const* c, int ch, int n) override { mail.applyPending(core); core.process(c, ch, n); }
    const char* typeId() const override { return "legacy-delay"; }
private:
    legacy_delay::DelayCore core;
    legacy_delay::DelayParams p;
    dcrig::ParamMailbox<legacy_delay::DelayParams> mail;
};

static void legacyCompatibility() {
    for (bool parked : {false, true}) for (double rate : {44100.0, 48000.0, 96000.0}) {
        dcrig::DspChain current, old;
        current.prepare(rate, 256, 2); old.prepare(rate, 256, 2);
        const int a = current.addNode(dcrig::createModule("delay"), parked);
        const int b = old.addNode(std::make_unique<LegacyModule>(), parked);
        for (int block = 0; block < 420; ++block) {
            if (block == 1 || block == 45 || block == 150) {
                for (const auto& p : {std::pair{"timeMs", block == 1 ? 45.0f : block == 45 ? 80.0f : 230.0f},
                                      std::pair{"mix", 0.65f}, std::pair{"feedback", 0.7f}}) {
                    current.setParam(a, p.first, p.second); old.setParam(b, p.first, p.second);
                }
            }
            if (block == 60) { current.setBypass(a, false); old.setBypass(b, false); }
            if (block == 95 || block == 96 || block == 140 || block == 180) {
                const bool bypass = block == 95 || block == 140;
                current.setBypass(a, bypass); old.setBypass(b, bypass);
            }
            std::array<float, 256> al{}, ar{}, bl{}, br{};
            for (int i = 0; i < 256; ++i) {
                al[i] = bl[i] = std::sin((block * 256 + i) * 0.031f) * 0.2f;
                ar[i] = br[i] = std::cos((block * 256 + i) * 0.047f) * 0.13f;
            }
            float* ca[] = {al.data(), ar.data()}; float* cb[] = {bl.data(), br.data()};
            current.process(ca, 2, 256); old.process(cb, 2, 256);
            if (std::memcmp(al.data(), bl.data(), sizeof(al)) || std::memcmp(ar.data(), br.data(), sizeof(ar))) {
                check(false, "sync/trails off is bit-identical through automation, bypass and reenable");
                std::printf("  rate %.0f block %d\n", rate, block); break;
            }
        }
    }
}

static void tempoMath() {
    dcdsp::DelayParams p;
    p.timeMs = 421.0f;
    check(dcdsp::delayTimeMs(p) == 421.0f, "sync off retains manual time");
    p.sync = true;
    const float expected[] = {2000, 1000, 500, 750, 1000.0f/3, 250, 375, 500.0f/3, 125};
    for (int i = 0; i < 9; ++i) { p.division = i; check(std::abs(dcdsp::delayTimeMs(p)-expected[i]) < 0.001f, "tempo division math"); }
    p.bpm = 20; p.division = 0; check(dcdsp::delayTimeMs(p) == 2000, "slow whole note clamps at two seconds");
    p.bpm = std::numeric_limits<float>::infinity(); p.division = 2;
    check(dcdsp::delayTimeMs(p) == 500, "non-finite BPM has finite fallback");
    auto module = dcrig::createModule("delay");
    for (const auto& pair : {std::pair{"sync", 1.0f}, std::pair{"bpm", 137.0f}, std::pair{"division", 6.0f}, std::pair{"trails", 1.0f}}) {
        float value = -1;
        check(module->setParam(pair.first, pair.second), "new persisted param accepted");
        check(module->getParam(pair.first, value) && value == pair.second, "new persisted param round trips");
    }
    check(!module->setParam("division", std::numeric_limits<float>::quiet_NaN()), "invalid division rejected before rounding");
    const auto& catalogue = dcrig::moduleCatalogue();
    const auto delay = std::find_if(catalogue.begin(), catalogue.end(), [](const auto& m) { return m.id == "delay"; });
    check(delay != catalogue.end() && delay->params.size() == 9, "factory exposes all nine real controls");
    if (delay != catalogue.end()) for (const auto& param : delay->params)
        if (param.id == "sync" || param.id == "trails") check(param.defaultValue == 0 && param.valueLabels.size() == 2, "opt-ins default off and are named");
}

struct Rig {
    dcrig::DspChain chain;
    int id;
    std::array<float, 64> left{}, right{};
    Rig(bool trails) {
        chain.prepare(48000, 64, 2);
        id = chain.addNode(dcrig::createModule("delay"));
        chain.setParam(id, "timeMs", 20); chain.setParam(id, "mix", 0.5f);
        chain.setParam(id, "feedback", 0); chain.setParam(id, "trails", trails ? 1.0f : 0.0f);
        for (int i = 0; i < 32; ++i) block();
    }
    float block(float impulse = 0) {
        left.fill(0); right.fill(0); left[0] = right[0] = impulse;
        float* ch[] = {left.data(), right.data()}; chain.process(ch, 2, 64);
        float peak = 0; for (float f : left) { check(std::isfinite(f), "output finite"); peak = std::max(peak, std::abs(f)); }
        return peak;
    }
};

static void actualChainTrails() {
    for (bool trails : {false, true}) {
        Rig r(trails);
        r.block(1);
        r.chain.setBypass(r.id, true);
        float tail = 0;
        for (int i = 0; i < 32; ++i) tail = std::max(tail, r.block());
        check(trails ? tail > 0.49f : tail == 0, "real DspChain retains wet tail only when enabled");
        check(r.block(0.73f) == 0.73f, "settled bypass passes fresh dry at unity");
        float leaked = 0;
        for (int i = 0; i < 48; ++i) leaked = std::max(leaked, r.block());
        check(leaked == 0, "bypassed fresh input never enters a trails delay");
        r.chain.setBypass(r.id, false);
        float stale = 0;
        for (int i = 0; i < 64; ++i) stale = std::max(stale, r.block());
        check(stale == 0, "reenable cannot revive a frozen or expired tail");
    }
    Rig resume(true);
    resume.block(1); resume.chain.setBypass(resume.id, true);
    for (int i = 0; i < 8; ++i) resume.block(); // parked before the 20ms echo
    resume.chain.setBypass(resume.id, false);
    float retained = 0;
    for (int i = 0; i < 24; ++i) retained = std::max(retained, resume.block());
    check(retained > 0.49f, "reenabling input preserves a live advancing tail");
    Rig decaying(true);
    decaying.chain.setParam(decaying.id, "feedback", 0.55f);
    for (int i = 0; i < 32; ++i) decaying.block();
    decaying.block(1); decaying.chain.setBypass(decaying.id, true);
    float early = 0, late = 0;
    for (int i = 0; i < 180; ++i) {
        const float peak = decaying.block();
        if (i < 40) early = std::max(early, peak);
        if (i > 140) late = std::max(late, peak);
    }
    check(early > 0.49f && late < 0.01f, "feedback tails advance and decay while bypassed");
    Rig r(true);
    // Before the 20ms echo, the exact chain ramp restores dry without doubling.
    r.block(0.25f); r.chain.setBypass(r.id, true); r.block(0.25f);
    const float weight = 1.0f - 1.0f / 480.0f;
    check(std::abs(r.left[0] - (1.0f - 0.5f * weight) * 0.25f) < 1e-6f, "bypass transition applies dry ramp exactly once");
    for (int i = 0; i < 12; ++i) { r.chain.setBypass(r.id, i % 2 == 0); r.block(0.1f); }
    r.chain.setBypass(r.id, true);
    r.chain.setParam(r.id, "trails", 0);
    for (int i = 0; i < 40; ++i) r.block();
    r.chain.setParam(r.id, "trails", 1);
    check(r.block() == 0, "enabling trails after hard bypass clears frozen history");
    r.chain.setBypass(r.id, false); r.block(1);
    r.chain.prepare(48000, 64, 2);
    float stale = 0;
    for (int i = 0; i < 64; ++i) stale = std::max(stale, r.block());
    check(stale == 0, "reprepare hard-reset clears all delay history");

    std::array<float, 8192> large{}; large[0] = 0.4f;
    float* ch[] = {large.data()};
    r.chain.setBypass(r.id, true); r.chain.process(ch, 1, (int)large.size());
    check(large[0] == 0.4f && std::all_of(large.begin(), large.end(), [](float x) { return std::isfinite(x); }), "oversized mono block uses safe bypass fallback without scratch growth");
}

static void syncSignalAndRtSafety() {
    dcrig::DspChain chain;
    chain.prepare(48000, 64, 2);
    const int id = chain.addNode(dcrig::createModule("delay"));
    chain.setParam(id, "mix", 1); chain.setParam(id, "feedback", 0);
    chain.setParam(id, "timeMs", 10); // Must lose to sync's 125ms.
    chain.setParam(id, "sync", 1); chain.setParam(id, "bpm", 120); chain.setParam(id, "division", 8);
    std::array<float, 64> left{}, right{};
    int first = -1;
    for (int b = 0; b < 120; ++b) {
        left.fill(0); right.fill(0);
        if (b == 0) left[0] = right[0] = 1;
        float* ch[] = {left.data(), right.data()}; chain.process(ch, 2, 64);
        for (int i = 0; i < 64; ++i) if (std::abs(left[i]) > 0.99f && first < 0) first = b * 64 + i;
    }
    // The unchanged delay line reads before pushing, retaining its one-sample convention.
    check(first == 6001, "actual factory/chain sync delays pulse by selected note division");
#ifdef _MSC_VER
    auto oldHook = _CrtSetAllocHook(allocationHook);
    audioAllocations = 0;
#endif
    for (int b = 0; b < 100; ++b) {
        chain.setParam(id, "trails", b % 3 == 0 ? 0.0f : 1.0f);
        chain.setBypass(id, b % 7 < 4);
        left.fill(0.01f); right.fill(-0.01f);
        float* ch[] = {left.data(), right.data()};
#ifdef _MSC_VER
        audioArmed = true;
#endif
        chain.process(ch, 2, 64);
#ifdef _MSC_VER
        audioArmed = false;
#endif
    }
#ifdef _MSC_VER
    _CrtSetAllocHook(oldHook);
    check(audioAllocations == 0, "tempo/trails/bypass processing allocates and frees nothing on callback");
#endif
}

int main() {
    tempoMath(); legacyCompatibility(); actualChainTrails(); syncSignalAndRtSafety();
    std::printf("Delay sync/trails: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
