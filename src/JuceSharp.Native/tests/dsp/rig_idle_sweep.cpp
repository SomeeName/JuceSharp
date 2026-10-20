// juce_sharp_rig_idle_sweep — proves the idle-sleep allow-list (dsp/IdleSleep.h).
//
// For every module type in kIdleSleepTypes, under several parameter sets and
// two idle inputs (digital silence, -70 dBFS hum), two identical chains are
// driven through the same pluck -> idle -> pluck:
//
//   A  never sleeps (it is what the rig did before idle sleep existed),
//   B  runs the SHIPPING decision (RigIdleSleep) with the chain's own hold.
//
// and the type passes only when
//
//   1. B actually fell asleep in the silent-idle default run (non-vacuous),
//   2. while B slept, A never rose above kQuietPeak (-90 dBFS) — so nothing a
//      sleeping rig zeroes (an echo, a reverse window) was ever audible, and
//   3. from the waking block on, B matches A within kWakeDiffTolerance, or
//      differs from it by no more than a never-slept rig C does when the pick
//      lands 3 blocks later (wakeAcceptable has the whole rule) — so resuming
//      the frozen module state is the same sound as never having stopped.
//
// It also sweeps random CHAINS of allow-listed types (composition), checks the
// allow-list names only real catalogue types, and that a type NOT on the list
// keeps a chain awake.
//
// `--discover` runs the same measurement over the WHOLE catalogue with the base
// hold and prints the numbers the allow-list was chosen from; it asserts
// nothing. No JUCE, no device, no third-party model (namamp is proven by
// juce_sharp_rig_idle_nam against a real capture).

#include "dsp/DspChain.h"
#include "dsp/DspModuleFactory.h"
#include "dsp/IdleSleep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace {

constexpr double kRate  = 48000.0;
constexpr int    kBlock = 128;
constexpr double kPi    = 3.14159265358979323846;

int g_failures = 0;
void fail(const std::string& s) { ++g_failures; std::printf("FAIL %s\n", s.c_str()); }

float dbfs(float lin) { return lin > 0.0f ? 20.0f * std::log10(lin) : -999.0f; }

/// A plucked low A: harmonics of 110 Hz with a 3 ms attack and a 0.35 s decay,
/// peaking near -8 dBFS — a real pick, not a test tone.
float pluck(double t)
{
    if (t < 0.0) return 0.0f;
    const double env = (t < 0.003 ? t / 0.003 : 1.0) * std::exp(-t / 0.35);
    double s = 0.0;
    for (int k = 1; k <= 8; ++k) s += std::sin(2.0 * kPi * 110.0 * k * t + 0.3 * k) / k;
    return (float) (0.2 * env * s);
}

/// Mains hum at about -70 dBFS plus white hiss at about -90 dBFS, the idle
/// input a real interface delivers. The hiss matters: a pure hum is PERIODIC,
/// and a sleep that happens to last a whole number of cycles would hand the
/// waking rig the very history it would have had anyway — which made an early
/// version of this sweep look exact when it was only lucky. Deterministic
/// (a fixed LCG keyed by the sample index), so A and B see the same samples.
float hum(double t, uint32_t seed = 0u)
{
    const uint32_t n = (uint32_t) std::llround(t * kRate);
    const uint32_t x = n * 1664525u + 1013904223u + seed * 2654435761u;
    const uint32_t y = (x ^ (x >> 15)) * 2246822519u;
    const float hiss = 3.0e-5f * ((float) (y >> 8) / 8388608.0f - 1.0f);
    return (float) (3.0e-4 * (0.7 * std::sin(2.0 * kPi * 50.0 * t) + 0.3 * std::sin(2.0 * kPi * 150.0 * t + 1.0))) + hiss;
}

struct NodeSpec
{
    std::string                                type;
    std::vector<std::pair<std::string, float>> params;
};

struct RunResult
{
    bool   slept          = false;   // B fell asleep at least once
    bool   wokeOnPluck    = false;   // B processed the first block of the second pluck
    bool   wakeExpected   = false;   // that block's input peak cleared B's wake level
    double secondsToSleep = -1.0;    // from the end of the first pluck
    float  emergePeak     = 0.0f;    // A's loudest block while B slept
    float  wakeDiff       = 0.0f;    // max |A - B| from the waking block on
    float  idleOutPeak    = 0.0f;    // A's loudest block over the idle stretch
    double longestGap     = 0.0;     // A: longest quiet run that ENDED in a loud block (s)
    double sigEnergy      = 0.0;     // sum A^2 from the waking block on
    double diffEnergy     = 0.0;     // sum (A-B)^2 from the waking block on
    /// The yardstick: C never sleeps either, and idles kYardstickBlocks LONGER
    /// than A before the same pick. |A - C|, each measured from its own pick,
    /// is how much WHEN the pick lands in an idle rig moves the note — the
    /// hum's phase, the hiss's realization, a limit cycle's phase in some
    /// filter's last bit. Every never-slept rig already varies by that much
    /// from pick to pick, so no listener can attribute it to anything.
    float  refDiff        = 0.0f;
    double refDiffEnergy  = 0.0;
    double refEsrDb() const
    {
        if (refDiffEnergy <= 0.0) return -999.0;
        if (sigEnergy <= 0.0) return 999.0;
        return 10.0 * std::log10(refDiffEnergy / sigEnergy);
    }
    /// Error-to-signal ratio of B against A over the wake window, in dB.
    double esrDb() const
    {
        if (diffEnergy <= 0.0) return -999.0;
        if (sigEnergy <= 0.0) return 999.0;
        return 10.0 * std::log10(diffEnergy / sigEnergy);
    }
};

void setParams(dcrig::DspChain& c, int id, const NodeSpec& n)
{
    for (const auto& p : n.params) c.setParam(id, p.first.c_str(), p.second);
}

/// holdOverride < 0: use the chain's own hold (the shipping path).
double g_minHoldSeconds = 0.0; // --min-hold: a debugging override, never the gate

/// The yardstick C idles this many blocks LONGER than A before the same pick
/// (3 x 128 frames = 8 ms, about 0.4 of a 50 Hz hum cycle).
constexpr long long kYardstickBlocks = 3;

RunResult runChain(const std::vector<NodeSpec>& nodes, bool humIdle, long long holdOverride,
                   double maxIdleSeconds, double sleepSeconds = 3.0)
{
    dcrig::DspChain a, b, c;
    a.prepare(kRate, 2048, 2, kBlock);
    b.prepare(kRate, 2048, 2, kBlock);
    c.prepare(kRate, 2048, 2, kBlock);
    for (const auto& n : nodes)
    {
        const int ia = a.addNode(dcrig::createModule(n.type.c_str()));
        const int ib = b.addNode(dcrig::createModule(n.type.c_str()));
        const int ic = c.addNode(dcrig::createModule(n.type.c_str()));
        setParams(a, ia, n);
        setParams(b, ib, n);
        setParams(c, ic, n);
    }

    dcrig::RigIdleSleep sl;
    std::vector<float> al(kBlock), ar(kBlock), bl(kBlock), br(kBlock), cl(kBlock), cr(kBlock);
    float* ach[2] = { al.data(), ar.data() };
    float* bch[2] = { bl.data(), br.data() };
    float* cch[2] = { cl.data(), cr.data() };

    RunResult r;
    const long long playBlocks = (long long) std::ceil(1.0 * kRate / kBlock);
    const long long wakeBlocks = (long long) std::ceil(2.0 * kRate / kBlock);
    const long long maxIdleBlocks = (long long) std::ceil(maxIdleSeconds * kRate / kBlock);
    const long long sleepCheckBlocks = (long long) std::ceil(sleepSeconds * kRate / kBlock);

    long long block = 0;
    long long quietRun = 0;        // A's current quiet run, in blocks
    long long sleptBlocks = 0;     // how long B has been asleep this idle stretch
    long long idleBlocks = 0;
    int phase = 0;                 // 0 play, 1 idle, 2 wake (A and B)
    long long wakeAt = -1;         // A's and B's second pick, in blocks
    bool firstWakeBlock = true;
    std::vector<float> aWake, cWake; // A and C from their own second pick on

    auto input = [&](double t, long long pickBlock) -> float
    {
        float x = humIdle ? hum(t) : 0.0f;
        if (t < 1.0) x += pluck(t);
        if (pickBlock >= 0) x += pluck(t - (double) pickBlock * kBlock / kRate);
        return x;
    };

    for (;;)
    {
        const bool abActive = wakeAt < 0 || block < wakeAt + wakeBlocks;
        const long long cPick = wakeAt < 0 ? -1 : wakeAt + kYardstickBlocks;
        const bool cRecording = cPick >= 0 && block >= cPick;
        if (! abActive && cPick >= 0 && block >= cPick + wakeBlocks) break;

        const double t0 = (double) block * kBlock / kRate;
        for (int i = 0; i < kBlock; ++i)
        {
            const double t = t0 + (double) i / kRate;
            const float x = input(t, phase == 2 ? wakeAt : -1);
            al[(size_t) i] = ar[(size_t) i] = bl[(size_t) i] = br[(size_t) i] = x;
            cl[(size_t) i] = cr[(size_t) i] = input(t, cRecording ? cPick : -1);
        }

        c.process(cch, 2, kBlock);
        if (cRecording) { cWake.insert(cWake.end(), cl.begin(), cl.end()); cWake.insert(cWake.end(), cr.begin(), cr.end()); }
        if (! abActive) { ++block; continue; }

        a.process(ach, 2, kBlock);

        dcrig::RigIdleContext ctx;
        ctx.chainGeneration = b.rtEditGeneration();
        const float inPeak = dcrig::blockPeak(bch, 2, kBlock);
        const bool  wasAsleep = sl.asleep();
        const float wakeLevel = sl.wakeLevel();
        const bool skip = sl.beginBlock(ctx, inPeak, false);
        if (skip) { std::fill(bl.begin(), bl.end(), 0.0f); std::fill(br.begin(), br.end(), 0.0f); }
        else      b.process(bch, 2, kBlock);
        long long hold = holdOverride >= 0 ? holdOverride : b.rtIdleHoldSamples();
        if (hold >= 0 && g_minHoldSeconds > 0.0)
            hold = std::max(hold, (long long) (g_minHoldSeconds * kRate));
        sl.endBlock(dcrig::blockPeak(bch, 2, kBlock), kBlock, hold, b.rtMayRest());

        const float aPeak = dcrig::blockPeak(ach, 2, kBlock);
        if (phase != 2)
        {
            if (aPeak < dcrig::idle::kQuietPeak) ++quietRun;
            else
            {
                if (quietRun > 0)
                    r.longestGap = std::max(r.longestGap, (double) quietRun * kBlock / kRate);
                quietRun = 0;
            }
        }

        if (phase == 1)
        {
            ++idleBlocks;
            r.idleOutPeak = std::max(r.idleOutPeak, aPeak);
            if (skip)
            {
                r.emergePeak = std::max(r.emergePeak, aPeak);
                if (! r.slept) r.secondsToSleep = (double) idleBlocks * kBlock / kRate;
                r.slept = true;
                ++sleptBlocks;
            }
            if (sleptBlocks >= sleepCheckBlocks || idleBlocks >= maxIdleBlocks)
            {
                phase = 2;
                wakeAt = block + 1;
            }
        }
        else if (phase == 2)
        {
            if (firstWakeBlock)
            {
                // A rig muted by its OWN settings (a gate at 0 dBFS, a volume
                // pedal at zero) went quiet with the pluck in its input, so the
                // pluck is part of its idle floor and does not wake it — the
                // never-slept rig is silent too, which wakeDiff checks. Anything
                // above the wake level MUST be processed on this very block.
                r.wakeExpected = wasAsleep && inPeak > wakeLevel;
                r.wokeOnPluck = ! skip;
                firstWakeBlock = false;
            }
            aWake.insert(aWake.end(), al.begin(), al.end());
            aWake.insert(aWake.end(), ar.begin(), ar.end());
            for (int i = 0; i < kBlock; ++i)
            {
                const float dl = al[(size_t) i] - bl[(size_t) i];
                const float dr = ar[(size_t) i] - br[(size_t) i];
                r.wakeDiff = std::max(r.wakeDiff, std::max(std::fabs(dl), std::fabs(dr)));
                r.diffEnergy += (double) dl * dl + (double) dr * dr;
                r.sigEnergy  += (double) al[(size_t) i] * al[(size_t) i] + (double) ar[(size_t) i] * ar[(size_t) i];
            }
        }
        else if (block + 1 >= playBlocks)
        {
            phase = 1;
        }
        ++block;
    }

    // The yardstick: A against C, each from its own second pick on.
    for (size_t i = 0; i < std::min(aWake.size(), cWake.size()); ++i)
    {
        const float e = aWake[i] - cWake[i];
        r.refDiff = std::max(r.refDiff, std::fabs(e));
        r.refDiffEnergy += (double) e * e;
    }
    return r;
}

/// The acceptance rule for the waking block onward, in order:
///   1. within kWakeDiffTolerance (-80 dBFS) of the never-slept rig — what
///      digital silence gives in practice, bit-exact, because every state the
///      hold lets settle has flushed to zero;
///   2. or a difference whose ENERGY is 100 dB below the note's — past the
///      dynamic range of 16-bit audio. It is the shape a frozen hum phase takes
///      through a fuzz at full gain (fuzz at max: -77.6 dBFS peak, -102 dB of
///      the note's energy, as sub-sample shifts of its clipping edges);
///   3. or no further from A than the yardstick C (+6 dB in peak and in
///      energy): how much the MOMENT of the pick moves the note in a rig that
///      never slept. A sleeping rig's frozen state holds an older stretch of
///      the same idle — hum at another phase, other hiss, a limit cycle's
///      other bit — which is exactly what picking a moment later does.
bool wakeAcceptable(const RunResult& r)
{
    if (r.wakeDiff <= dcrig::idle::kWakeDiffTolerance) return true;
    if (r.esrDb() <= -100.0) return true;
    return dbfs(r.wakeDiff) <= dbfs(r.refDiff) + 6.0f && r.esrDb() <= r.refEsrDb() + 6.0;
}

/// Parameter sets for one type: catalogue defaults, every param at its max,
/// every param at its min, and two seeded random settings.
std::vector<std::pair<std::string, std::vector<std::pair<std::string, float>>>>
variantsFor(const dcrig::ModuleTypeInfo& info)
{
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, float>>>> out;
    out.push_back({ "default", {} });
    std::vector<std::pair<std::string, float>> hi, lo;
    for (const auto& p : info.params) { hi.push_back({ p.id, p.maxValue }); lo.push_back({ p.id, p.minValue }); }
    out.push_back({ "max", hi });
    out.push_back({ "min", lo });
    for (unsigned seed = 1; seed <= 2; ++seed)
    {
        std::mt19937 rng(seed * 7919u + (unsigned) info.id.size());
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        std::vector<std::pair<std::string, float>> rp;
        for (const auto& p : info.params) rp.push_back({ p.id, p.minValue + (p.maxValue - p.minValue) * u(rng) });
        out.push_back({ "rand" + std::to_string(seed), rp });
    }
    return out;
}

const dcrig::ModuleTypeInfo* findType(const char* id)
{
    for (const auto& t : dcrig::moduleCatalogue())
        if (t.id == id) return &t;
    return nullptr;
}

void printRow(const std::string& type, const std::string& variant, bool humIdle, const RunResult& r)
{
    std::printf("  %-14s %-8s %-7s slept=%d t=%5.2fs idleOut=%7.1f emerge=%7.1f gap=%5.2fs wakeDiff=%7.1f dBFS esr=%7.1f dB | noise-ref %7.1f dBFS esr=%7.1f dB%s\n",
                type.c_str(), variant.c_str(), humIdle ? "hum" : "silence", r.slept ? 1 : 0,
                r.secondsToSleep, dbfs(r.idleOutPeak), dbfs(r.emergePeak), r.longestGap,
                dbfs(r.wakeDiff), r.esrDb(), dbfs(r.refDiff), r.refEsrDb(), r.wakeExpected && ! r.wokeOnPluck ? "  (MISSED WAKE)" : "");
}

/// The shipping check for one allow-listed type. Returns false on failure.
bool checkType(const dcrig::IdleSleepType& t, bool verbose)
{
    const auto* info = findType(t.typeId);
    if (info == nullptr)
    {
        if (std::strcmp(t.typeId, "namamp") == 0) return true; // absent without NeuralAudio; proven by the NAM test
        fail(std::string("allow-list names '") + t.typeId + "', which is not a catalogue type");
        return false;
    }
    bool ok = true;
    bool sleptDefaultSilence = false;
    // The longest settle on the list (a gate at 500 ms hold / 1 s release, a
    // convolution reverb) is ~10.5 s; the run stops as soon as B has slept 3 s.
    const double maxIdle = 60.0;
    for (const auto& v : variantsFor(*info))
    {
        for (int h = 0; h < 2; ++h)
        {
            const RunResult r = runChain({ NodeSpec{ t.typeId, v.second } }, h == 1, -1, maxIdle);
            if (verbose) printRow(t.typeId, v.first, h == 1, r);
            if (v.first == "default" && h == 0 && r.slept) sleptDefaultSilence = true;
            const std::string where = std::string(t.typeId) + " [" + v.first + ", " + (h ? "hum" : "silence") + "]";
            if (r.wakeExpected && ! r.wokeOnPluck) { fail(where + ": the waking block was skipped"); ok = false; }
            if (r.emergePeak >= dcrig::idle::kQuietPeak)
            {
                fail(where + ": output rose to " + std::to_string(dbfs(r.emergePeak))
                     + " dBFS while the rig slept (raise its settle)");
                ok = false;
            }
            if (! wakeAcceptable(r))
            {
                fail(where + ": slept vs never-slept differ by " + std::to_string(dbfs(r.wakeDiff))
                     + " dBFS (noise yardstick " + std::to_string(dbfs(r.refDiff))
                     + " dBFS) from the waking block on");
                ok = false;
            }
        }
    }
    if (! sleptDefaultSilence)
    {
        fail(std::string(t.typeId) + ": never fell asleep on digital silence at its defaults — the check is vacuous");
        ok = false;
    }
    return ok;
}

void discover()
{
    std::printf("DISCOVERY (base hold %.1f s, nothing asserted)\n", dcrig::idle::kBaseHoldSeconds);
    const long long base = dcrig::idleHoldSamples(0.0, kRate);
    for (const auto& info : dcrig::moduleCatalogue())
    {
        float worstDiff = 0.0f, worstEmerge = 0.0f;
        double worstGap = 0.0;
        bool anySlept = false;
        for (const auto& v : variantsFor(info))
            for (int h = 0; h < 2; ++h)
            {
                const RunResult r = runChain({ NodeSpec{ info.id, v.second } }, h == 1, base, 16.0, 10.0);
                printRow(info.id, v.first, h == 1, r);
                anySlept |= r.slept;
                worstDiff = std::max(worstDiff, r.wakeDiff);
                worstEmerge = std::max(worstEmerge, r.emergePeak);
                worstGap = std::max(worstGap, r.longestGap);
            }
        std::printf("SUMMARY %-14s slept=%d worstEmerge=%7.1f worstGap=%5.2fs worstWakeDiff=%7.1f dBFS\n",
                    info.id.c_str(), anySlept ? 1 : 0, dbfs(worstEmerge), worstGap, dbfs(worstDiff));
    }
}

} // namespace

int main(int argc, char** argv)
{
    bool verbose = false, discoverMode = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--discover") == 0) discoverMode = true;
        if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
        if (std::strcmp(argv[i], "--min-hold") == 0 && i + 1 < argc) g_minHoldSeconds = std::atof(argv[i + 1]);
    }
    // The device callback runs with FTZ/DAZ on (juce::ScopedNoDenormals in
    // renderBlock); so does this, or a decaying tail would be measured in a
    // numeric regime the rig never runs in.
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
    if (discoverMode) { discover(); return 0; }
    // `--chain type:variant,type:variant,...` runs one chain and prints it (a
    // debugging aid for a random-chain failure; asserts nothing).
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--chain") == 0)
        {
            std::vector<NodeSpec> nodes;
            std::string spec = argv[i + 1];
            size_t pos = 0;
            while (pos <= spec.size())
            {
                const size_t comma = spec.find(',', pos);
                const std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                const size_t colon = item.find(':');
                const std::string type = item.substr(0, colon);
                const std::string var = colon == std::string::npos ? "default" : item.substr(colon + 1);
                const auto* info = findType(type.c_str());
                if (info == nullptr) { std::printf("unknown type %s\n", type.c_str()); return 2; }
                for (const auto& v : variantsFor(*info))
                    if (v.first == var) nodes.push_back(NodeSpec{ type, v.second });
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            for (int h = 0; h < 2; ++h) printRow(spec, "chain", h == 1, runChain(nodes, h == 1, -1, 60.0));
            return 0;
        }

    // 1. Every allow-listed type, alone.
    for (const auto& t : dcrig::kIdleSleepTypes)
        if (checkType(t, verbose)) std::printf("ok   %s\n", t.typeId);

    // 2. Composition: random chains of 2-5 allow-listed types at random settings.
    {
        std::vector<const dcrig::IdleSleepType*> pool;
        for (const auto& t : dcrig::kIdleSleepTypes)
            if (findType(t.typeId) != nullptr) pool.push_back(&t);
        // The owner's rig, with the in-house amp standing in for the two NAM
        // captures this binary cannot load (juce_sharp_rig_idle_nam runs the
        // real ones).
        const std::vector<NodeSpec> owner = {
            { "gate", { { "thresholdDb", -46.f }, { "ratio", 12.f }, { "attackMs", 1.f },
                        { "holdMs", 40.f }, { "releaseMs", 70.f } } },
            { "overdrive", {} },
            { "amp", {} },
            { "cab", { { "model", 8.f }, { "mix", 1.f }, { "lowCutHz", 95.f }, { "highCutHz", 8500.f } } },
            { "eq", { { "lowFreq", 110.f }, { "lowGainDb", 1.f }, { "midFreq", 1200.f },
                      { "midGainDb", 3.f }, { "highFreq", 4500.f }, { "highGainDb", 2.f } } },
            { "gain", { { "gainDb", 0.03f } } },
        };
        for (int h = 0; h < 2; ++h)
        {
            const RunResult r = runChain(owner, h == 1, -1, 60.0);
            printRow("owner-like", "chain", h == 1, r);
            const std::string where = std::string("owner-like chain") + (h ? " [hum]" : " [silence]");
            if (! r.slept) fail(where + ": never slept");
            if (r.wakeExpected && ! r.wokeOnPluck) fail(where + ": the waking block was skipped");
            if (r.emergePeak >= dcrig::idle::kQuietPeak) fail(where + ": emerged while asleep");
            if (! wakeAcceptable(r))
                fail(where + ": wake diff " + std::to_string(dbfs(r.wakeDiff)) + " dBFS (noise yardstick "
                     + std::to_string(dbfs(r.refDiff)) + " dBFS)");
        }

        std::mt19937 rng(20260924u);
        int chains = 0;
        for (int c = 0; c < 48 && ! pool.empty(); ++c)
        {
            std::vector<NodeSpec> nodes;
            const int len = 2 + (int) (rng() % 4u);
            std::string name;
            for (int k = 0; k < len; ++k)
            {
                const auto* t = pool[rng() % pool.size()];
                const auto vs = variantsFor(*findType(t->typeId));
                const auto& v = vs[rng() % vs.size()];
                nodes.push_back(NodeSpec{ t->typeId, v.second });
                name += (k ? ">" : "") + std::string(t->typeId) + ":" + v.first;
            }
            for (int h = 0; h < 2; ++h)
            {
                const RunResult r = runChain(nodes, h == 1, -1, 60.0);
                if (verbose) printRow(name, "chain", h == 1, r);
                const std::string where = "chain " + name + (h ? " [hum]" : " [silence]");
                if (r.wakeExpected && ! r.wokeOnPluck) fail(where + ": the waking block was skipped");
                if (r.emergePeak >= dcrig::idle::kQuietPeak) fail(where + ": emerged while asleep");
                if (! wakeAcceptable(r))
                    fail(where + ": wake diff " + std::to_string(dbfs(r.wakeDiff)) + " dBFS (noise yardstick "
                         + std::to_string(dbfs(r.refDiff)) + " dBFS)");
            }
            ++chains;
        }
        std::printf("ok   %d random chains\n", chains);
    }

    // 3. A type that is NOT allow-listed keeps the chain awake: its hold is -1.
    {
        const char* notListed = nullptr;
        for (const auto& info : dcrig::moduleCatalogue())
            if (! dcrig::idleSleepAllowed(info.id.c_str())) { notListed = info.id.c_str(); break; }
        if (notListed != nullptr)
        {
            dcrig::DspChain c;
            c.prepare(kRate, 2048, 2, kBlock);
            c.addNode(dcrig::createModule("gain"));
            const long long holdAllowed = c.rtIdleHoldSamples();
            c.addNode(dcrig::createModule(notListed));
            if (holdAllowed != dcrig::idleHoldSamples(0.0, kRate))
                fail("a gain-only chain should hold exactly the base hold, got " + std::to_string(holdAllowed));
            if (c.rtIdleHoldSamples() >= 0)
                fail(std::string("adding '") + notListed + "' (not allow-listed) did not veto the chain's sleep");
            else
                std::printf("ok   '%s' (not allow-listed) vetoes sleep\n", notListed);
        }
    }

    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nall idle-sleep checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
