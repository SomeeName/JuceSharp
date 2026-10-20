// rsamp_synthetic_table.h — a SYNTHETIC `rsamp` chain table for the native tests
// (rsamp_table_test.cpp, the RT gate). No number here comes from Rocksmith: the
// real table is generated on the user's machine from the user's own install, and
// no test in this repository may carry it. These rows are made up so that, over
// all 63 rows, every slot kind, every Guitar Distortion waveshaper, every FutzBox
// section, every curve shape (0..9) and every curve scaling (0, 2, 3) is reached.
#pragma once

#include "dsp/cores/dcdsp_rsamp_table.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace rsamptest {

struct Blob {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back((uint8_t) v); }
    void u16(uint32_t v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void f32(float v) { uint32_t x; std::memcpy(&x, &v, 4); u32(x); }
    void str(const char* s) { const size_t n = std::strlen(s); u8((uint32_t) n); for (size_t i = 0; i < n; ++i) u8((uint8_t) s[i]); }
};

// Byte offsets of the header counts, for tests that corrupt one.
inline constexpr size_t kOffFormat = 8, kOffRows = 12, kHeaderBytes = 36;

// The blob for `rows` (each < kNumModels, ascending). Row r gets 1 + r % 10
// knobs, up to four slots cycling Gd / Futz / Comp, and two curves per knob.
inline std::vector<uint8_t> makeTable(const std::vector<int>& rows)
{
    using namespace dcdsp::rsamp;
    Blob rowsBlob, gd, futz, comp, points, curves;
    uint32_t numGd = 0, numFutz = 0, numComp = 0, numPoints = 0, numCurves = 0;
    for (const int r : rows)
    {
        rowsBlob.u8((uint32_t) r);
        rowsBlob.str(kModelRows[r].key);
        const int knobs = 1 + r % 10;
        rowsBlob.u8((uint32_t) knobs);
        for (int k = 0; k < knobs; ++k)
        {
            const bool discrete = (r + k) % 7 == 3;
            rowsBlob.f32(discrete ? 0.f : 1.f);
            rowsBlob.f32(discrete ? 4.f : 100.f);
            rowsBlob.f32(discrete ? 2.f : 50.f);
            rowsBlob.u8(discrete ? 1 : 0);
        }
        const int slots = 1 + r % 4;
        static const Slot kCycle[4] = { Slot::Gd, Slot::Futz, Slot::Gd, Slot::Comp };
        Slot kinds[4];
        rowsBlob.u8((uint32_t) slots);
        for (int s = 0; s < slots; ++s)
        {
            kinds[s] = (r % 5 == 4 && s == 0) ? Slot::Comp : kCycle[(r + s) % 4];
            rowsBlob.u8((uint32_t) kinds[s]);
            if (kinds[s] == Slot::Gd)
            {
                rowsBlob.u16(numGd++);
                for (int band = 0; band < 6; ++band)
                {
                    gd.u32((uint32_t) ((r + band) % 7));        // every filter type
                    gd.u8((r + band) % 3 != 0 ? 1 : 0);
                    gd.f32((float) ((band % 2 == 0 ? 1 : -1) * (3 + (r + band) % 6)));
                    gd.f32(80.f * (float) (1 << band));          // 80 Hz .. 2.56 kHz
                    gd.f32(0.4f + 0.2f * (float) band);
                }
                gd.u32((uint32_t) (r % 5));                      // every waveshaper, 0 = none
                gd.f32(30.f + (float) (r % 50)); gd.f32(50.f); gd.f32((float) (r % 3) * 30.f);
                gd.f32(-3.f); gd.f32(100.f);
            }
            else if (kinds[s] == Slot::Futz)
            {
                rowsBlob.u16(numFutz++);
                futz.u8(1); futz.f32(0.f); futz.f32(9000.f); futz.f32(20.f);
                futz.f32(0.f); futz.f32(60.f); futz.f32(10.f);
                futz.u8(r % 2); futz.u32(0); futz.f32(20.f); futz.f32((float) (r % 2) * 100.f); futz.f32(0.f);
                futz.u8(1); futz.u32((uint32_t) (r % 3)); futz.f32(1200.f); futz.f32(0.8f); futz.f32(-4.f);
                futz.f32(0.f); futz.f32(-2.f); futz.f32(100.f);
            }
            else
            {
                rowsBlob.u16(numComp++);
                comp.f32(-24.f); comp.f32(4.f); comp.f32(0.01f); comp.f32(0.2f); comp.f32(2.f);
            }
        }
        rowsBlob.u16(numCurves);
        rowsBlob.u16((uint32_t) (2 * knobs));
        for (int k = 0; k < knobs; ++k)
        {
            for (int c = 0; c < 2; ++c)
            {
                const int slot = (k + c) % slots;
                const bool isGd = kinds[slot] == Slot::Gd, isFutz = kinds[slot] == Slot::Futz;
                // A band gain (dB-scaled), a band / filter frequency (log-scaled) or a plain value.
                const int scaling = c == 0 ? 2 : (isGd || isFutz ? 3 : 0);
                int param;
                if (isGd) param = c == 0 ? 10 * ((k + r) % 6) + 1 : 10 * ((k + r) % 6) + 2;
                else if (isFutz) param = c == 0 ? 64 : 52;
                else param = c == 0 ? 4 : 0;
                const float lo = c == 0 ? -0.5f : (scaling == 3 ? 2.0f : -40.f);
                const float hi = c == 0 ? 0.6f : (scaling == 3 ? 3.9f : -10.f);
                curves.u8((uint32_t) k); curves.u8((uint32_t) slot); curves.u8((uint32_t) param); curves.u8((uint32_t) scaling);
                curves.u16(numPoints); curves.u8(3);
                const int shape = (r + k + c) % 10;
                points.f32(0.f);   points.f32(lo);                 points.u8((uint32_t) shape);
                points.f32(50.f);  points.f32(0.5f * (lo + hi));   points.u8((uint32_t) ((shape + 3) % 10));
                points.f32(100.f); points.f32(hi);                 points.u8(4);
                numPoints += 3;
                ++numCurves;
            }
        }
    }

    Blob o;
    for (const char ch : { 'D', 'C', 'R', 'S', 'A', 'M', 'P', 'T' }) o.u8((uint32_t) ch);
    o.u32(kTableFormat);
    o.u32((uint32_t) rows.size());
    o.u32(numGd); o.u32(numFutz); o.u32(numComp); o.u32(numPoints); o.u32(numCurves);
    for (const auto* part : { &rowsBlob, &gd, &futz, &comp, &points, &curves })
        o.b.insert(o.b.end(), part->b.begin(), part->b.end());
    return o.b;
}

inline std::vector<uint8_t> makeFullTable()
{
    std::vector<int> all;
    for (int r = 0; r < dcdsp::rsamp::kNumModels; ++r) all.push_back(r);
    return makeTable(all);
}

} // namespace rsamptest
