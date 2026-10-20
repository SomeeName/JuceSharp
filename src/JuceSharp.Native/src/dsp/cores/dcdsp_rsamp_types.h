// dcdsp_rsamp_types.h — the record types of the Rocksmith amp-chain table that
// the `rsamp` core (dcdsp_rsamp.h) runs. NO values ship with them: the table is
// read on the user's machine from the user's own Rocksmith 2014 install and
// handed in at run time (dcdsp_rsamp_table.h, ABI v45 dca_rsamp_set_table). The
// model ROWS — which amp each `model` int names — are fixed and public
// (dcdsp_rsamp_models.h).
#pragma once

#include <cstdint>

namespace dcdsp {
namespace rsamp {

inline constexpr int kMaxKnobs = 10;
inline constexpr int kMaxSlots = 4;

enum class Slot : uint8_t { None, Gd, Futz, Comp };

// One Audiokinetic Guitar Distortion EQ band. type: 0 LOWSHELF, 1 PEAKINGEQ,
// 2 HIGHSHELF, 3 LOWPASS, 4 HIGHPASS, 5 BANDPASS, 6 NOTCH (wwiser's
// CAkGuitarDistortion__AkFilterType).
struct Band {
    uint32_t type;
    uint8_t  on;
    float    gainDb, hz, q;
};

// Audiokinetic Wwise Guitar Distortion (0x007E0003): bands 0-2 before the
// distortion, 3-5 after it. dist: 0 NONE, 1 OVERDRIVE, 2 HEAVY, 3 FUZZ, 4 CLIP.
struct GdBlock {
    Band     band[6];
    uint32_t dist;
    float    drive, tone, rect, outDb, wet;   // %, %, %, dB, %
};

// McDSP FutzBox (0x006E1003), the sections Rocksmith's amps switch on.
struct FutzBlock {
    uint8_t  filtersOn;
    float    lpfSlope, lpfHz, lpfRes, hpfSlope, hpfHz, hpfRes;
    uint8_t  distOn;
    uint32_t mode;
    float    amount, intensity, rectify;      // %
    uint8_t  eqOn;
    uint32_t eqType;                          // 0 HPF, 1 EQ (peaking), 2 LPF
    float    eqHz, eqQ, eqDb;
    float    inDb, outDb, balance;            // dB, dB, % wet
};

// Audiokinetic Wwise Compressor (0x006C0003).
struct CompBlock {
    float thresholdDb, ratio, attackS, releaseS, outDb;
};

struct CurvePoint {
    float   x, y;    // knob value, STORED y (see Curve::scaling)
    uint8_t shape;   // AkCurveInterpolation of the segment starting here
};

struct Curve {
    uint8_t  knob, slot, param, scaling;      // scaling: 0 none, 2 dB, 3 log10 Hz
    uint16_t firstPoint;
    uint8_t  numPoints;
};

struct Knob {
    float   min, max, def;
    uint8_t discrete;                         // a switch or a selector: rounds
};

// One row's chain. numSlots == 0: the loaded table has no chain for this row,
// and the core passes audio through.
struct Model {
    uint8_t     numKnobs;
    Knob        knob[kMaxKnobs];
    uint8_t     numSlots;
    Slot        kind[kMaxSlots];
    uint16_t    index[kMaxSlots];             // into the table's Gd / Futz / Comp blocks by kind
    uint16_t    firstCurve, numCurves;
};

} // namespace rsamp
} // namespace dcdsp
