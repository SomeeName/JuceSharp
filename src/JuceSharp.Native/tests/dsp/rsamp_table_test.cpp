// juce_sharp_rsamp_table_test — the `rsamp` chain table loaded at RUN TIME
// (ABI v45, dcdsp_rsamp_table.h). The table is Rocksmith's data, generated on
// the user's machine; this repository carries none of it, so every table here
// is SYNTHETIC (rsamp_synthetic_table.h). What is pinned:
//
//  * the parser takes a well-formed blob and refuses every malformed one WHOLE
//    (bad magic / version, a row whose key is not that row's amp, rows out of
//    order, a block, curve or point index out of range, an unknown shape or
//    scaling, a non-finite value, a truncated blob, trailing bytes);
//  * the registry keeps the table in force when a load is refused, treats an
//    identical reload as a no-op, and clears;
//  * the core passes audio through UNCHANGED with no table, and for a row the
//    table has no chain for; it picks up a newly loaded table on its own, through
//    the 5 ms dip (no step), without allocating on the process path.
#include "dsp/cores/dcdsp_rsamp.h"
#include "rsamp_synthetic_table.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
    else std::printf("ok   %s\n", what);
}
#ifdef _MSC_VER
static thread_local bool armed = false;
static thread_local int allocations = 0;
static int __cdecl allocHook(int kind, void*, size_t, int block, long, const unsigned char*, int) {
    if (armed && (kind == _HOOK_ALLOC || kind == _HOOK_FREE) && block != _CRT_BLOCK) ++allocations;
    return 1;
}
#endif

using namespace dcdsp::rsamp;

static bool parses(const std::vector<uint8_t>& b, const char** why = nullptr) {
    Table t;
    const char* err = nullptr;
    const bool ok = parseTable(b.data(), b.size(), t, &err);
    if (why) *why = err;
    return ok;
}

static void putU32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + (size_t) i] = (uint8_t) (v >> (8 * i));
}

// --------------------------------------------------------------- the parser

static void aWellFormedTableParses() {
    const auto blob = rsamptest::makeFullTable();
    Table t;
    const char* err = nullptr;
    check(parseTable(blob.data(), blob.size(), t, &err), "a synthetic 63-row table parses");
    check(t.numRows == kNumModels, "...with every row");
    bool everyRow = true, kinds[4] = {}, shapes[10] = {}, dists[5] = {};
    for (int r = 0; r < kNumModels; ++r) {
        const Model* m = t.row(r);
        everyRow &= m != nullptr && m->numKnobs == 1 + r % 10 && m->numSlots == 1 + r % 4;
        if (m == nullptr) continue;
        for (int s = 0; s < m->numSlots; ++s) kinds[(int) m->kind[s]] = true;
    }
    for (const auto& p : t.points) shapes[p.shape] = true;
    for (const auto& g : t.gd) dists[g.dist] = true;
    check(everyRow, "every row has its knobs and slots");
    bool allShapes = true, allDists = true;
    for (bool b : shapes) allShapes &= b;
    for (bool b : dists) allDists &= b;
    check(kinds[1] && kinds[2] && kinds[3], "the fixture reaches every slot kind");
    check(allShapes && allDists, "...every curve shape and every waveshaper");

    const auto partial = rsamptest::makeTable({ 0, 17, 55 });
    Table p;
    check(parseTable(partial.data(), partial.size(), p, nullptr) && p.numRows == 3
              && p.row(17) != nullptr && p.row(1) == nullptr,
          "a table may carry only some rows; the others have no chain");
}

static void aMalformedTableIsRefusedWhole() {
    const auto good = rsamptest::makeTable({ 0, 1, 2 });
    const char* why = nullptr;

    auto b = good; b[0] = 'X';
    check(!parses(b, &why), "bad magic is refused");
    b = good; putU32(b, rsamptest::kOffFormat, 2);
    check(!parses(b), "an unknown format version is refused");
    b = good; putU32(b, rsamptest::kOffRows, 0);
    check(!parses(b), "an empty table is refused");
    b = good; b.pop_back();
    check(!parses(b), "a truncated blob is refused");
    b = good; b.push_back(0);
    check(!parses(b, &why) && why != nullptr, "trailing bytes are refused");

    // Row 0's key starts right after the header + its row byte + its length byte.
    b = good; b[rsamptest::kHeaderBytes + 2] = 'X';
    check(!parses(b), "a row whose key is not that row's amp is refused");
    // Rows out of order: build 2 then 1 by hand-swapping is awkward, so name row 1 twice.
    auto twice = rsamptest::makeTable({ 1, 2 });
    twice[rsamptest::kHeaderBytes] = 2;   // first row claims index 2; its key is still row 1's
    check(!parses(twice), "a row index that does not match its key is refused");
    auto descending = rsamptest::makeTable({ 0, 1 });
    // The second row byte follows the first row; find it by its key.
    for (size_t i = rsamptest::kHeaderBytes + 1; i + 9 < descending.size(); ++i)
        if (std::memcmp(&descending[i], "Amp_AT20", 8) == 0 && descending[i - 1] == 8) { descending[i - 2] = 0; break; }
    check(!parses(descending), "rows that are not strictly ascending are refused");

    // A non-finite knob range: row 0's first knob min sits after its 9-byte key
    // ("Amp_AT120") and the knob-count byte.
    b = good;
    const size_t knobMin = rsamptest::kHeaderBytes + 1 + 1 + 9 + 1;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(&b[knobMin], &nan, 4);
    check(!parses(b), "a non-finite value is refused");

    // Shape and scaling: the curves and points are the blob's tail.
    Table t;
    parseTable(good.data(), good.size(), t, nullptr);
    const size_t curveBytes = 7 * t.curves.size();
    b = good; b[b.size() - curveBytes + 3] = 1;   // the first curve's scaling
    check(!parses(b), "an unknown curve scaling is refused");
    b = good; b[b.size() - curveBytes - 1] = 10;  // the last point's shape
    check(!parses(b), "an unknown curve shape is refused");
    b = good; b[b.size() - curveBytes] = 9;       // the first curve's knob (row 0 has one knob)
    check(!parses(b), "a curve naming a knob its row does not have is refused");
    b = good; b[b.size() - 1] = 200;              // the last curve's point count
    check(!parses(b), "a curve whose points run past the point list is refused");
}

// ------------------------------------------------------------- the registry

static void theRegistryKeepsWhatIsInForce() {
    auto& reg = TableRegistry::instance();
    reg.clear();
    check(reg.current() == nullptr && reg.rowCount() == 0, "no table at start (cleared)");

    const auto a = rsamptest::makeTable({ 0, 1, 2, 3 });
    check(reg.load(a.data(), a.size(), nullptr) && reg.rowCount() == 4, "a table loads");
    const Table* first = reg.current();
    check(reg.load(a.data(), a.size(), nullptr) && reg.current() == first, "an identical reload is a no-op");

    auto bad = a; bad[0] = 'X';
    const char* why = nullptr;
    check(!reg.load(bad.data(), bad.size(), &why) && reg.current() == first && why != nullptr,
          "a refused load leaves the table in force");

    const auto b = rsamptest::makeFullTable();
    check(reg.load(b.data(), b.size(), nullptr) && reg.rowCount() == kNumModels, "a different table replaces it");
    check(reg.load(a.data(), a.size(), nullptr) && reg.current() == first,
          "switching back to a table already loaded reuses it (nothing retained twice)");
    reg.clear();
    check(reg.current() == nullptr, "clear unloads");
}

// ------------------------------------------------------------------ the core

static constexpr int kBlock = 256;

static void run(dcdsp::RsAmpCore& core, std::vector<float>& l, std::vector<float>& r, int startSample) {
    for (int i = 0; i < kBlock; ++i)
        l[(size_t) i] = r[(size_t) i] = 0.2f * (float) std::sin(6.283185307179586 * 220.0 * (startSample + i) / 48000.0);
    float* ch[2] = { l.data(), r.data() };
    core.process(ch, 2, kBlock);
}

static void withoutATableTheCorePassesThrough() {
    TableRegistry::instance().clear();
    dcdsp::RsAmpCore core;
    dcdsp::RsAmpParams p; p.model = 17;
    core.setParams(p);
    core.prepare(48000.0, kBlock, 2);
    std::vector<float> l(kBlock), r(kBlock);
    double err = 0.0;
    for (int b = 0; b < 8; ++b) {
        run(core, l, r, b * kBlock);
        for (int i = 0; i < kBlock; ++i)
            err = std::max(err, (double) std::fabs(l[(size_t) i] - 0.2f * (float) std::sin(6.283185307179586 * 220.0 * (b * kBlock + i) / 48000.0)));
    }
    check(core.tableForTest() == nullptr && !core.runsChainForTest(), "no table: the core runs no chain");
    check(err == 0.0, "no table: audio passes through bit-identically");
}

static void aRowTheTableLacksPassesThrough() {
    const auto t = rsamptest::makeTable({ 3 });
    TableRegistry::instance().load(t.data(), t.size(), nullptr);
    dcdsp::RsAmpCore core;
    dcdsp::RsAmpParams p; p.model = 5;   // not in the table
    core.setParams(p);
    core.prepare(48000.0, kBlock, 2);
    std::vector<float> l(kBlock), r(kBlock);
    run(core, l, r, 0);
    double err = 0.0;
    for (int i = 0; i < kBlock; ++i)
        err = std::max(err, (double) std::fabs(l[(size_t) i] - 0.2f * (float) std::sin(6.283185307179586 * 220.0 * i / 48000.0)));
    check(core.tableForTest() != nullptr && !core.runsChainForTest() && err == 0.0,
          "a row the loaded table has no chain for passes audio through");
    TableRegistry::instance().clear();
}

static void aLoadedTableIsPickedUpThroughTheDip() {
    TableRegistry::instance().clear();
    dcdsp::RsAmpCore core;
    dcdsp::RsAmpParams p; p.model = 17;
    for (float& k : p.knob) k = 0.8f;
    core.setParams(p);
    core.prepare(48000.0, kBlock, 2);
    std::vector<float> l(kBlock), r(kBlock), all;
    all.reserve(40 * kBlock);   // the test's own buffer must not allocate while armed
    for (int b = 0; b < 4; ++b) { run(core, l, r, b * kBlock); all.insert(all.end(), l.begin(), l.end()); }

    const auto t = rsamptest::makeFullTable();
    TableRegistry::instance().load(t.data(), t.size(), nullptr);   // control thread, while "playing"
#ifdef _MSC_VER
    allocations = 0; armed = true;
#endif
    for (int b = 4; b < 40; ++b) { run(core, l, r, b * kBlock); all.insert(all.end(), l.begin(), l.end()); }
#ifdef _MSC_VER
    armed = false;
    check(allocations == 0, "picking up a new table allocates nothing on the process path");
#endif
    check(core.tableForTest() == TableRegistry::instance().current() && core.runsChainForTest(),
          "the core picks the new table up on its own and runs the row's chain");

    // The swap rides the dip: no sample-to-sample step across it is larger than
    // the larger of the passthrough's and the settled chain's own.
    auto maxStep = [&all](size_t from, size_t to) {
        double m = 0.0;
        for (size_t i = from; i < to; ++i) m = std::max(m, (double) std::fabs(all[i] - all[i - 1]));
        return m;
    };
    const double before = maxStep(1, 4 * kBlock);
    const double across = maxStep(4 * kBlock, 9 * kBlock);
    const double after = maxStep(all.size() - 10 * kBlock, all.size());
    std::printf("     max step before %.5f, across %.5f, after %.5f\n", before, across, after);
    check(across <= 1.25 * std::max(before, after) + 1e-4, "the table swap dips instead of stepping");

    // ...and the chain now changes the sound.
    double diff = 0.0;
    for (size_t i = all.size() - kBlock; i < all.size(); ++i) {
        const double ref = 0.2 * std::sin(6.283185307179586 * 220.0 * (double) i / 48000.0);
        diff = std::max(diff, std::fabs(all[i] - ref));
    }
    check(diff > 1e-3, "with a table loaded the chain is in the path");
    TableRegistry::instance().clear();
    for (int b = 40; b < 60; ++b) run(core, l, r, b * kBlock);
    check(core.tableForTest() == nullptr, "an unloaded table is dropped the same way");
}

int main() {
#ifdef _MSC_VER
    _CrtSetAllocHook(allocHook);
#endif
    aWellFormedTableParses();
    aMalformedTableIsRefusedWhole();
    theRegistryKeepsWhatIsInForce();
    withoutATableTheCorePassesThrough();
    aRowTheTableLacksPassesThrough();
    aLoadedTableIsPickedUpThroughTheDip();
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
