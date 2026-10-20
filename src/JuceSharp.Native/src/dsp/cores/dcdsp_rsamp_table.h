// dcdsp_rsamp_table.h — the `rsamp` chain table, loaded at RUN TIME (ABI v45).
//
// The numbers a Rocksmith amp's chain runs (its Wwise plug-in blocks and the
// RTPC curves its knobs drive) are the game's, so none of them is in this
// library. The host reads them from the user's own install, packs them in the
// blob below and hands them in through dca_rsamp_set_table. Without a table
// every `rsamp` module passes audio through unchanged, and the host is expected
// to say so rather than offer the module as the game's amp.
//
// Blob, format 1. Little-endian, packed, no padding:
//   char     magic[8]   "DCRSAMPT"
//   uint32   format     1
//   uint32   numRows, numGd, numFutz, numComp, numPoints, numCurves
//   rows[numRows]:
//     uint8  row        index into kModelRows, strictly ascending
//     uint8  keyLen, char key[keyLen]   must equal kModelRows[row].key
//     uint8  numKnobs   <= kMaxKnobs; each: float min, max, def; uint8 discrete
//     uint8  numSlots   1..kMaxSlots; each: uint8 kind (1 Gd, 2 Futz, 3 Comp), uint16 index
//     uint16 firstCurve, numCurves      a range of curves[]
//   gd[numGd]:       6 x { uint32 type; uint8 on; float gainDb, hz, q },
//                    uint32 dist; float drive, tone, rect, outDb, wet
//   futz[numFutz]:   uint8 filtersOn; float lpfSlope, lpfHz, lpfRes, hpfSlope, hpfHz, hpfRes;
//                    uint8 distOn; uint32 mode; float amount, intensity, rectify;
//                    uint8 eqOn; uint32 eqType; float eqHz, eqQ, eqDb; float inDb, outDb, balance
//   comp[numComp]:   float thresholdDb, ratio, attackS, releaseS, outDb
//   points[numPoints]: float x, y; uint8 shape (AkCurveInterpolation, 0..9)
//   curves[numCurves]: uint8 knob, slot, param, scaling (0, 2 or 3); uint16 firstPoint; uint8 numPoints
// Every index is range-checked, every float must be finite and the blob must be
// consumed exactly; a blob that fails any check is refused WHOLE and the table
// in force stays. The managed writer is JuceSharp.Dsp.RsAmpChainTable.
//
// Real-time publication: a parsed table is published with ONE release store of
// a pointer (gCurrentTable); the audio thread reads it with one acquire load at
// the top of a block, no lock, no allocation. A published table is never freed
// while the process lives, so a core still running an older table never sees it
// die: an identical reload is a no-op (the table already kept is republished),
// and the number of DISTINCT tables one process may publish is capped
// (kMaxTables), which bounds what the retention keeps. Parsing, validation and
// the retention list are control-thread only (they allocate and take a mutex no
// audio thread ever touches).
#pragma once

#include "dcdsp_rsamp_types.h"
#include "dcdsp_rsamp_models.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace dcdsp {
namespace rsamp {

inline constexpr char     kTableMagic[8] = { 'D', 'C', 'R', 'S', 'A', 'M', 'P', 'T' };
inline constexpr uint32_t kTableFormat   = 1;
inline constexpr size_t   kTableHeaderBytes = 8 + 4 * 7;
inline constexpr int      kMaxTables     = 32;

struct Table {
    Model model[kNumModels] = {};
    int   numRows = 0;
    std::vector<GdBlock>    gd;
    std::vector<FutzBlock>  futz;
    std::vector<CompBlock>  comp;
    std::vector<CurvePoint> points;
    std::vector<Curve>      curves;
    std::vector<uint8_t>    bytes;   // what it was parsed from (the reload identity)

    // The row's chain, or nullptr when this table has none for it.
    const Model* row(int m) const noexcept
    {
        return (m >= 0 && m < kNumModels && model[m].numSlots > 0) ? &model[m] : nullptr;
    }
};

namespace detail_table {

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t off = 0;
    bool ok = true;

    bool need(size_t k) { if (!ok || off > n || n - off < k) ok = false; return ok; }
    uint8_t u8() { if (!need(1)) return 0; return p[off++]; }
    uint16_t u16() { if (!need(2)) return 0; const uint16_t v = (uint16_t) (p[off] | (p[off + 1] << 8)); off += 2; return v; }
    uint32_t u32()
    {
        if (!need(4)) return 0;
        const uint32_t v = (uint32_t) p[off] | ((uint32_t) p[off + 1] << 8)
                         | ((uint32_t) p[off + 2] << 16) | ((uint32_t) p[off + 3] << 24);
        off += 4;
        return v;
    }
    float f32()
    {
        const uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, sizeof v);
        if (!std::isfinite(v)) { ok = false; return 0.f; }
        return v;
    }
};

} // namespace detail_table

// Parses and validates a blob into `out`. False (with a static message in
// `error`) when anything is wrong; `out` is then unspecified.
inline bool parseTable(const uint8_t* data, size_t length, Table& out, const char** error)
{
    auto fail = [error](const char* why) { if (error) *error = why; return false; };
    if (data == nullptr || length < kTableHeaderBytes) return fail("rsamp table: too short");
    if (std::memcmp(data, kTableMagic, 8) != 0) return fail("rsamp table: bad magic");
    detail_table::Reader r { data, length, 8 };
    if (r.u32() != kTableFormat) return fail("rsamp table: unknown format version");
    const uint32_t numRows = r.u32(), numGd = r.u32(), numFutz = r.u32(), numComp = r.u32(),
                   numPoints = r.u32(), numCurves = r.u32();
    if (numRows == 0 || numRows > (uint32_t) kNumModels) return fail("rsamp table: row count out of range");
    if (numGd > 0xFFFFu || numFutz > 0xFFFFu || numComp > 0xFFFFu || numCurves > 0xFFFFu || numPoints > 0x10000u)
        return fail("rsamp table: block, curve or point count out of range");
    // Every record takes at least one byte, so counts the blob cannot hold are
    // refused before anything is sized by them.
    if ((uint64_t) numGd + numFutz + numComp + numPoints + numCurves > (uint64_t) length)
        return fail("rsamp table: counts larger than the blob");

    out = Table{};
    struct RowCurves { int row; uint16_t first, count; };
    std::vector<RowCurves> ranges;
    ranges.reserve(numRows);
    int last = -1;
    for (uint32_t i = 0; i < numRows; ++i)
    {
        const int row = r.u8();
        if (!r.ok || row <= last || row >= kNumModels) return fail("rsamp table: rows not strictly ascending or out of range");
        last = row;
        const size_t keyLen = r.u8();
        if (!r.need(keyLen)) return fail("rsamp table: truncated key");
        const char* expected = kModelRows[row].key;
        if (std::strlen(expected) != keyLen || std::memcmp(r.p + r.off, expected, keyLen) != 0)
            return fail("rsamp table: a row's key is not the amp that row names");
        r.off += keyLen;

        Model& m = out.model[row];
        m.numKnobs = r.u8();
        if (m.numKnobs > kMaxKnobs) return fail("rsamp table: too many knobs");
        for (int k = 0; k < m.numKnobs; ++k)
        {
            m.knob[k].min = r.f32();
            m.knob[k].max = r.f32();
            m.knob[k].def = r.f32();
            m.knob[k].discrete = r.u8() != 0 ? 1 : 0;
        }
        m.numSlots = r.u8();
        if (!r.ok) return fail("rsamp table: truncated row, or a value is not finite");
        if (m.numSlots < 1 || m.numSlots > kMaxSlots) return fail("rsamp table: slot count out of range");
        for (int s = 0; s < m.numSlots; ++s)
        {
            const uint8_t kind = r.u8();
            const uint16_t index = r.u16();
            const uint32_t limit = kind == 1 ? numGd : kind == 2 ? numFutz : kind == 3 ? numComp : 0;
            if (index >= limit) return fail("rsamp table: a slot names a block that does not exist");
            m.kind[s] = (Slot) kind;
            m.index[s] = index;
        }
        m.firstCurve = r.u16();
        m.numCurves = r.u16();
        if ((uint32_t) m.firstCurve + m.numCurves > numCurves) return fail("rsamp table: a row's curves run past the curve list");
        ranges.push_back({ row, m.firstCurve, m.numCurves });
        if (!r.ok) return fail("rsamp table: truncated row");
    }

    out.gd.resize(numGd);
    for (auto& b : out.gd)
    {
        for (auto& band : b.band)
        {
            band.type = r.u32();
            band.on = r.u8() != 0 ? 1 : 0;
            band.gainDb = r.f32();
            band.hz = r.f32();
            band.q = r.f32();
            if (band.type > 6) return fail("rsamp table: unknown band type");
        }
        b.dist = r.u32();
        if (b.dist > 4) return fail("rsamp table: unknown distortion type");
        b.drive = r.f32(); b.tone = r.f32(); b.rect = r.f32(); b.outDb = r.f32(); b.wet = r.f32();
    }
    out.futz.resize(numFutz);
    for (auto& b : out.futz)
    {
        b.filtersOn = r.u8() != 0 ? 1 : 0;
        b.lpfSlope = r.f32(); b.lpfHz = r.f32(); b.lpfRes = r.f32();
        b.hpfSlope = r.f32(); b.hpfHz = r.f32(); b.hpfRes = r.f32();
        b.distOn = r.u8() != 0 ? 1 : 0;
        b.mode = r.u32();
        b.amount = r.f32(); b.intensity = r.f32(); b.rectify = r.f32();
        b.eqOn = r.u8() != 0 ? 1 : 0;
        b.eqType = r.u32();
        b.eqHz = r.f32(); b.eqQ = r.f32(); b.eqDb = r.f32();
        b.inDb = r.f32(); b.outDb = r.f32(); b.balance = r.f32();
    }
    out.comp.resize(numComp);
    for (auto& b : out.comp)
    {
        b.thresholdDb = r.f32(); b.ratio = r.f32(); b.attackS = r.f32(); b.releaseS = r.f32(); b.outDb = r.f32();
    }
    out.points.resize(numPoints);
    for (auto& pt : out.points)
    {
        pt.x = r.f32();
        pt.y = r.f32();
        pt.shape = r.u8();
        if (pt.shape > 9) return fail("rsamp table: unknown curve shape");
    }
    out.curves.resize(numCurves);
    for (auto& c : out.curves)
    {
        c.knob = r.u8(); c.slot = r.u8(); c.param = r.u8(); c.scaling = r.u8();
        c.firstPoint = r.u16();
        c.numPoints = r.u8();
        if (c.scaling != 0 && c.scaling != 2 && c.scaling != 3) return fail("rsamp table: unknown curve scaling");
        if (c.numPoints < 1 || (uint32_t) c.firstPoint + c.numPoints > numPoints)
            return fail("rsamp table: a curve's points run past the point list");
    }
    if (!r.ok) return fail("rsamp table: truncated, or a value is not finite");
    if (r.off != length) return fail("rsamp table: trailing bytes");

    // Every curve a row runs must name one of that row's knobs and slots.
    for (const auto& rc : ranges)
    {
        const Model& m = out.model[rc.row];
        for (uint32_t i = rc.first; i < (uint32_t) rc.first + rc.count; ++i)
            if (out.curves[i].knob >= m.numKnobs || out.curves[i].slot >= m.numSlots)
                return fail("rsamp table: a curve names a knob or slot its row does not have");
    }
    out.numRows = (int) numRows;
    out.bytes.assign(data, data + length);
    return true;
}

// The table every rsamp core runs. Constant-initialised (no static constructor,
// nothing to destroy): the audio thread's one acquire load can never race a
// registry's construction or teardown.
inline std::atomic<const Table*> gCurrentTable { nullptr };

// The process-wide registry: what loads, keeps and clears the table. Control
// thread only, except current() / rowCount(), which any thread may call.
class TableRegistry {
public:
    static TableRegistry& instance()
    {
        // Placement-new into static storage: no heap on first use, and never
        // destroyed, because a core on a still-running audio thread may hold
        // any table this keeps until the process ends.
        alignas(TableRegistry) static unsigned char storage[sizeof(TableRegistry)];
        static TableRegistry* registry = new (storage) TableRegistry();
        return *registry;
    }

    static const Table* current() noexcept { return gCurrentTable.load(std::memory_order_acquire); }

    static int rowCount() noexcept
    {
        const Table* t = current();
        return t != nullptr ? t->numRows : 0;
    }

    // Parses, validates and publishes. False, with the table in force left as it
    // was, for a malformed blob or past kMaxTables distinct tables.
    bool load(const uint8_t* data, size_t length, const char** error)
    {
        auto table = std::make_unique<Table>();
        if (!parseTable(data, length, *table, error)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kept : kept_)
        {
            if (kept->bytes == table->bytes)
            {
                gCurrentTable.store(kept.get(), std::memory_order_release);
                return true;
            }
        }
        if ((int) kept_.size() >= kMaxTables)
        {
            if (error) *error = "rsamp table: too many distinct tables loaded in this process";
            return false;
        }
        kept_.push_back(std::move(table));
        gCurrentTable.store(kept_.back().get(), std::memory_order_release);
        return true;
    }

    // Unloads: every rsamp module dips and then passes audio through. The table
    // stays kept (a core may still be running it).
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gCurrentTable.store(nullptr, std::memory_order_release);
    }

private:
    TableRegistry() = default;
    std::mutex mutex_;
    std::vector<std::unique_ptr<Table>> kept_;
};

} // namespace rsamp
} // namespace dcdsp
