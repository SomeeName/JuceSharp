// dcvoice_models.h — the chart voice's INSTRUMENT MODELS (DotCharter #50): which
// guitar or bass the built-in voice sounds like, which pickup(s) are selected,
// and how a recording made on ONE instrument / pickup is turned into another.
// Plain C++17, no JUCE; the table is constant data, read on any thread.
//
// WHAT IS RECORDED AND WHAT IS SIMULATED
//   * Stratocaster, all five positions: REAL (EGFxSet, one pack per position).
//   * Strat HSS neck / neck+middle / middle: the same real Strat recordings (the
//     HSS has Strat single coils there).
//   * Strat HSS bridge humbucker (full or split), Telecaster, Les Paul: the
//     closest REAL Strat position, converted (below).
//   * Semi-hollow (Gretsch Anniversary): REAL on its bridge pickup — the
//     library does not say; the recordings' own comb does (below) — converted
//     for neck / both.
//   * Jazz bass, both pickups, pick: REAL (Growlybass, Squier Jazz, both
//     pickups); everything else on bass is converted — from Growlybass for the
//     pick, from the Washburn AB-95's neck pickup (Black And Blue Basses) for
//     the fingers, and from the recorded slaps' source bass, if a pack has any (#109,
//     "bass-slap" below; every bass model converts them).
//
// THE CONVERSION (dcvoice_sampler.h, per playing recording, per note)
//   A magnetic pickup at distance d from the bridge senses the string's
//   displacement there, so harmonic n of a string of vibrating length L is
//   weighted by sin(pi n d / L) — a comb whose notches move with the fret.
//   Two pickups in parallel (and the two coils of a humbucker) SUM their signed
//   combs; a coil-split humbucker is one coil. The conversion divides out the
//   SOURCE recording's comb (a regularised inverse: its notches are not boosted
//   past +16 dB), applies the TARGET's, swaps the source pickup's loaded
//   resonance for the target's (one biquad), and applies a two-band body EQ.
//   The Strat geometry below was FITTED to EGFxSet by predicting every
//   position from every other one through this very conversion
//   (tools/voice/calibrate_pickups.py): bridge 43.3 mm (low E) to 39.4 mm
//   (high E), middle 98.9 mm, neck 158.2 mm — within a few mm of Fender's
//   drawings — and the prediction error fell from 8.18 dB (no conversion) to
//   5.37 dB RMS per harmonic over all 20 position pairs, the rest being
//   take-to-take differences in the pick. The same data chose the source
//   inverse's g (0.5; 0.7 ties, 0.85 and 0.3 are worse) and the target's leak
//   (0.7, against 6.03 dB for an exact comb), and showed a two-pickup recording
//   inverts as well as a single one.
//   WHERE THE KARORYFER SETS WERE RECORDED was measured the same way: the comb
//   left in each recording's harmonics, correlated against every candidate
//   placement (the check names the right Strat position at r = 0.31..0.42 and
//   every wrong one near 0): the Gretsch on its BRIDGE pickup at 45 mm (r 0.42),
//   the AB-95 on a neck pickup at 150 mm (r 0.41), Growlybass on both, about
//   150 + 60 mm (r 0.21; a single 110 mm reads 0.23 — its readme says both).
//   Every other geometry is nominal (published placements), and every non-Strat
//   resonance and body EQ is a documented ESTIMATE — those instruments have no
//   shippable recordings.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dcvoice {

// Pickup selector positions. The numbers are the ABI's DCA_CHART_PICKUP_* and
// are permanent.
enum PickupSel : int32_t
{
    kPickNeck = 0,
    kPickNeckMiddle = 1,
    kPickMiddle = 2,
    kPickMiddleBridge = 3,
    kPickBridge = 4,
    kPickNeckBridge = 5,
};
inline constexpr int kPickupSelCount = 6;

// Instrument models. The numbers are the ABI's DCA_CHART_MODEL_* and are
// permanent. Guitar models are < 16, bass models >= 16.
enum ModelId : int32_t
{
    kModelSemiHollow = 0,
    kModelStratSSS = 1,
    kModelStratHSS = 2,
    kModelTele = 3,
    kModelLesPaul = 4,
    kModelJazzBass = 16,
    kModelPBass = 17,
    kModelPJBass = 18,
    kModelHollowBass = 19,
};

/// One point where a coil senses the strings: its distance from the bridge
/// saddle at the lowest and the highest string (a slanted pickup), or — `step`
/// — one distance for the low half of the strings and another for the high half
/// (a P-bass split coil, each half under two strings). Distances are
/// interpolated by the string's OPEN pitch, so a 5- or 7-string chart lands on
/// the right side of the pickup. `weight`: its share of the summed signal.
struct SensePoint
{
    float dLowMm = 0.0f, dHighMm = 0.0f;
    float weight = 0.0f;
    bool step = false;
};

/// What the selected pickup(s) sense, and how they sound electrically.
struct PickupSound
{
    int count = 0;              // sense points used
    SensePoint pt[4] {};
    float resonanceHz = 6000.0f; // loaded resonance (pickup L + cable C)
    float resonanceQ = 1.5f;
    float levelDb = 0.0f;        // output against a Strat single coil
};

/// A two-band body EQ against the reference solid body (an alder Strat). A
/// band with gainDb == 0 is skipped. type 0 = peak, 1 = low shelf, 2 = high shelf.
struct BodyBand { int type = 0; float hz = 1000.0f; float gainDb = 0.0f; float q = 0.7f; };
struct BodyEq { BodyBand band[2] {}; };

/// Where a recording was made: the instrument's scale, its open-string pitch
/// range (for interpolating slanted pickups), the pickup(s) used and the body.
struct SourceSound
{
    float scaleMm = 648.0f;
    /// How hard the conversion divides this recording's comb out: the source
    /// inverse's g (dcvoice_pickup.h). 0.5 predicted EGFxSet's positions as well
    /// as 0.7 and better than 0.85; a placement known only weakly (Growlybass)
    /// is divided out more gently, so a guess cannot carve its notches into
    /// every converted note.
    float inverseG = 0.5f;
    float midiLow = 40.0f, midiHigh = 64.0f;
    PickupSound pickup {};
    BodyEq body {};
};

/// One position of a model's selector.
struct PositionSpec
{
    int32_t sel = kPickNeck;
    PickupSound full {};          // humbuckers as humbuckers
    PickupSound split {};         // coil split on (== full when no humbucker is involved)
    const char* sourcePack = "";  // the pack converted from ("" = the model's base pack)
    bool splitDiffers = false;
};

struct ModelSpec
{
    int32_t id = 0;
    const char* key = "";
    bool bass = false;
    float scaleMm = 648.0f;
    float midiLow = 40.0f, midiHigh = 64.0f;
    int positionCount = 0;
    PositionSpec pos[5] {};
    int32_t defaultSel = kPickBridge;
    BodyEq body {};
};

namespace models {

// ---- building blocks ----------------------------------------------------------

inline PickupSound single(float dLow, float dHigh, float hz, float q, float levelDb = 0.0f)
{
    PickupSound p;
    p.count = 1;
    p.pt[0] = { dLow, dHigh, 1.0f, false };
    p.resonanceHz = hz; p.resonanceQ = q; p.levelDb = levelDb;
    return p;
}
/// A humbucker centred at `centre`, coils `spacing` apart, in series.
inline PickupSound humbucker(float centre, float hz, float q, float levelDb, float spacing = 18.0f)
{
    PickupSound p;
    p.count = 2;
    p.pt[0] = { centre - 0.5f * spacing, centre - 0.5f * spacing, 0.5f, false };
    p.pt[1] = { centre + 0.5f * spacing, centre + 0.5f * spacing, 0.5f, false };
    p.resonanceHz = hz; p.resonanceQ = q; p.levelDb = levelDb;
    return p;
}
/// Two selections in parallel: the points of both at half weight each.
inline PickupSound parallel(const PickupSound& a, const PickupSound& b, float hz, float q, float levelDb)
{
    PickupSound p;
    for (int i = 0; i < a.count && p.count < 4; ++i) { p.pt[p.count] = a.pt[i]; p.pt[p.count].weight *= 0.5f; ++p.count; }
    for (int i = 0; i < b.count && p.count < 4; ++i) { p.pt[p.count] = b.pt[i]; p.pt[p.count].weight *= 0.5f; ++p.count; }
    p.resonanceHz = hz; p.resonanceQ = q; p.levelDb = levelDb;
    return p;
}
inline PositionSpec position(int32_t sel, const PickupSound& full, const char* pack)
{
    PositionSpec s; s.sel = sel; s.full = full; s.split = full; s.sourcePack = pack; s.splitDiffers = false;
    return s;
}
inline PositionSpec positionSplit(int32_t sel, const PickupSound& full, const PickupSound& split, const char* pack)
{
    PositionSpec s; s.sel = sel; s.full = full; s.split = split; s.sourcePack = pack; s.splitDiffers = true;
    return s;
}
inline BodyEq body(BodyBand a, BodyBand b = {}) { BodyEq e; e.band[0] = a; e.band[1] = b; return e; }

// ---- the Stratocaster as EGFxSet recorded it (FITTED) ---------------------------

inline constexpr float kStratScale = 648.0f;
inline constexpr float kStratBridgeLow = 43.3f, kStratBridgeHigh = 39.4f;
inline constexpr float kStratMiddle = 98.9f, kStratNeck = 158.2f;
// Loaded resonances: the first fit moved them little (the comb dominates below
// 8 kHz, where the error is measured); kept as typical Strat values.
inline constexpr float kStratSingleHz = 5714.0f, kStratSingleQ = 2.0f;
inline constexpr float kStratMixHz = 7147.0f, kStratMixQ = 1.56f;

inline PickupSound stratNeck()   { return single(kStratNeck, kStratNeck, kStratSingleHz, kStratSingleQ); }
inline PickupSound stratMiddle() { return single(kStratMiddle, kStratMiddle, kStratSingleHz, kStratSingleQ); }
inline PickupSound stratBridge() { return single(kStratBridgeLow, kStratBridgeHigh, kStratSingleHz, kStratSingleQ); }
inline PickupSound stratNeckMiddle()   { return parallel(stratNeck(), stratMiddle(), kStratMixHz, kStratMixQ, -1.0f); }
inline PickupSound stratMiddleBridge() { return parallel(stratMiddle(), stratBridge(), kStratMixHz, kStratMixQ, -1.0f); }

// The bass pickups the recordings were made on (measured, see the file comment).
inline PickupSound jazzNeck()   { return single(150.0f, 150.0f, 4200.0f, 1.6f); }
inline PickupSound jazzBridge() { return single(60.0f, 60.0f, 4200.0f, 1.6f); }
inline PickupSound jazzBoth()   { return parallel(jazzNeck(), jazzBridge(), 5500.0f, 1.4f, 0.0f); }
inline PickupSound hollowBassNeck() { return single(150.0f, 150.0f, 4000.0f, 1.3f); }
// The bass the recorded slaps were made on (#109; no pack built by DotCharter
// carries them now): the comb in 126 of its slap recordings (layers 3-5, string and fret from their measured
// inharmonicity) correlates best with ONE pickup ~100 mm from the bridge
// (r 0.32; 50 mm 0.13, the 4001's two pickups together 0.11-0.30). Resonance:
// an ESTIMATE (a high-impedance single coil).
inline PickupSound rick4001Slap() { return single(100.0f, 100.0f, 4500.0f, 1.5f); }

// Humbucker voicing (a medium-output PAF-style pair in series): estimates.
inline constexpr float kHbHz = 3800.0f, kHbQ = 1.8f, kHbLevel = 1.0f;
inline constexpr float kSplitHz = 6200.0f, kSplitQ = 1.9f, kSplitLevel = -1.0f;

} // namespace models

/// Where each pack's recordings were made. Variant: the pack's zone variant
/// (bass: 0 fingers, 1 pick); -1 = every variant.
inline bool sourceFor(const char* pack, int variant, SourceSound& out)
{
    using namespace models;
    SourceSound s;
    if (std::strncmp(pack, "strat-", 6) == 0)
    {
        s.scaleMm = kStratScale;
        const char* p = pack + 6;
        if (std::strcmp(p, "neck") == 0)               s.pickup = stratNeck();
        else if (std::strcmp(p, "neck-middle") == 0)   s.pickup = stratNeckMiddle();
        else if (std::strcmp(p, "middle") == 0)        s.pickup = stratMiddle();
        else if (std::strcmp(p, "middle-bridge") == 0) s.pickup = stratMiddleBridge();
        else if (std::strcmp(p, "bridge") == 0)        s.pickup = stratBridge();
        else return false;
        out = s;
        return true;
    }
    if (std::strcmp(pack, "guitar-arts") == 0)
    {
        // The shared ARTICULATIONS (palm mutes, harmonics): Guitar-TECHS player
        // 1, an Ibanez PF-300 Les Paul type (24.75"), 11-50 FLATWOUNDS, on its
        // BRIDGE humbucker. Placement measured from the same guitar's single
        // notes (2026-10-04, the comb correlation calibrate_pickups.py uses):
        // centre ~40 mm (r 0.29). Body: the Les Paul's, with the flatwounds'
        // dull 1-3 kHz as a broad -5 dB dip the conversion gives back — fitted
        // by packing the same guitar's OPEN notes, converting them to the default
        // model and comparing with its own open notes (tools/voice: the residual
        // is within +-2 dB 160 Hz-2.5 kHz at the attack). A deeper dip only lifts
        // the recordings' noise floor.
        s.scaleMm = 628.0f;
        s.pickup = humbucker(40.0f, kHbHz, kHbQ, kHbLevel);
        s.body = body({ 0, 350.0f, 2.0f, 0.8f }, { 0, 1800.0f, -5.0f, 0.6f });
        out = s;
        return true;
    }
    if (std::strcmp(pack, "guitar") == 0)
    {
        // Gretsch Anniversary (24.6" scale), two HiLo'Tron single coils,
        // recorded on the BRIDGE pickup (measured, 45 mm — see the file
        // comment). Hollow body.
        s.scaleMm = 625.0f;
        s.pickup = single(45.0f, 45.0f, 5800.0f, 1.6f);
        s.body = body({ 0, 220.0f, 3.0f, 0.9f }, { 2, 3500.0f, -1.5f, 0.7f });
        out = s;
        return true;
    }
    if (std::strcmp(pack, "bass-slap") == 0)
    {
        // The bass pack's RECORDED SLAPS (Art::SlapNote, #109), if it has any:
        // the source bass is 33.25" scale, solid body, roundwounds. The
        // placement is weakly known (r 0.32), so it is divided out gently, like
        // Growlybass's.
        s.scaleMm = 845.0f;
        s.midiLow = 28.0f; s.midiHigh = 43.0f;
        s.pickup = rick4001Slap();
        s.inverseG = 0.3f;
        out = s;
        return true;
    }
    if (std::strcmp(pack, "bass") == 0)
    {
        s.scaleMm = 864.0f;
        s.midiLow = 28.0f; s.midiHigh = 43.0f;
        if (variant == 1)
        {
            // Growlybass: Squier Jazz, BOTH pickups, every knob on 10 (its readme);
            // placement measured from the recordings (150 + 60 mm).
            s.pickup = jazzBoth();
            s.inverseG = 0.3f;   // the weakest placement evidence (r 0.21)
        }
        else
        {
            // Black And Blue Basses "darkblack": a Washburn AB-95 hollowbody,
            // fingers, NECK pickup (its user guide), measured at 150 mm.
            s.pickup = hollowBassNeck();
            s.body = body({ 0, 160.0f, 3.5f, 1.0f }, { 2, 2500.0f, -2.0f, 0.7f });
        }
        out = s;
        return true;
    }
    return false;
}

/// The model table. Every pickup voicing that is not a Strat's is an ESTIMATE
/// (nominal placement, typical loaded resonance) — see the file comment.
inline const ModelSpec* findModel(int32_t id)
{
    using namespace models;
    static const ModelSpec table[] = {
        [] {
            ModelSpec m; m.id = kModelSemiHollow; m.key = "semi-hollow"; m.scaleMm = 625.0f;
            const PickupSound n = single(158.0f, 158.0f, 5800.0f, 1.6f), b = single(45.0f, 45.0f, 5800.0f, 1.6f);
            m.positionCount = 3;
            m.pos[0] = position(kPickNeck, n, "guitar");
            m.pos[1] = position(kPickNeckBridge, parallel(n, b, 6500.0f, 1.3f, -1.0f), "guitar");
            m.pos[2] = position(kPickBridge, b, "guitar");   // as recorded
            m.defaultSel = kPickBridge;
            m.body = body({ 0, 220.0f, 3.0f, 0.9f }, { 2, 3500.0f, -1.5f, 0.7f });
            return m;
        }(),
        [] {
            ModelSpec m; m.id = kModelStratSSS; m.key = "strat-sss"; m.scaleMm = kStratScale;
            m.positionCount = 5;
            m.pos[0] = position(kPickNeck, stratNeck(), "strat-neck");
            m.pos[1] = position(kPickNeckMiddle, stratNeckMiddle(), "strat-neck-middle");
            m.pos[2] = position(kPickMiddle, stratMiddle(), "strat-middle");
            m.pos[3] = position(kPickMiddleBridge, stratMiddleBridge(), "strat-middle-bridge");
            m.pos[4] = position(kPickBridge, stratBridge(), "strat-bridge");
            m.defaultSel = kPickBridge;
            return m;
        }(),
        [] {
            // HSS: Strat single coils at neck and middle, a humbucker at the
            // bridge (centre 43 mm). Coil split leaves its BRIDGE-side coil —
            // the wiring that sounds most like a Strat bridge single coil.
            ModelSpec m; m.id = kModelStratHSS; m.key = "strat-hss"; m.scaleMm = kStratScale;
            const PickupSound hb = humbucker(43.0f, kHbHz, kHbQ, kHbLevel);
            const PickupSound coil = single(34.0f, 34.0f, kSplitHz, kSplitQ, kSplitLevel);
            m.positionCount = 5;
            m.pos[0] = position(kPickNeck, stratNeck(), "strat-neck");
            m.pos[1] = position(kPickNeckMiddle, stratNeckMiddle(), "strat-neck-middle");
            m.pos[2] = position(kPickMiddle, stratMiddle(), "strat-middle");
            m.pos[3] = positionSplit(kPickMiddleBridge, parallel(stratMiddle(), hb, 5200.0f, 1.5f, 1.0f),
                                     parallel(stratMiddle(), coil, kStratMixHz, kStratMixQ, -1.0f), "strat-middle-bridge");
            m.pos[4] = positionSplit(kPickBridge, hb, coil, "strat-bridge");
            m.defaultSel = kPickBridge;
            return m;
        }(),
        [] {
            // Telecaster: a slanted bridge single coil on a steel plate (lower,
            // sharper resonance — the twang) and a covered neck pickup (damped).
            ModelSpec m; m.id = kModelTele; m.key = "tele"; m.scaleMm = kStratScale;
            const PickupSound n = single(152.0f, 152.0f, 4500.0f, 1.1f, -2.0f);
            const PickupSound b = single(48.0f, 37.0f, 5000.0f, 2.4f, 1.0f);
            m.positionCount = 3;
            m.pos[0] = position(kPickNeck, n, "strat-neck");
            m.pos[1] = position(kPickNeckBridge, parallel(n, b, 6500.0f, 1.5f, -1.0f), "strat-middle");
            m.pos[2] = position(kPickBridge, b, "strat-bridge");
            m.defaultSel = kPickBridge;
            m.body = body({ 0, 2800.0f, 2.5f, 1.0f }, { 1, 150.0f, -1.0f, 0.7f });
            return m;
        }(),
        [] {
            // Les Paul: 24.75" scale, two humbuckers, mahogany / maple top. Coil
            // split leaves the neck humbucker's neck-side (screw) coil and the
            // bridge humbucker's bridge-side coil — the outer coils.
            ModelSpec m; m.id = kModelLesPaul; m.key = "les-paul"; m.scaleMm = 628.0f;
            const PickupSound n = humbucker(152.0f, kHbHz, kHbQ, kHbLevel), b = humbucker(44.0f, kHbHz, kHbQ, kHbLevel);
            const PickupSound ns = single(161.0f, 161.0f, kSplitHz, kSplitQ, kSplitLevel);
            const PickupSound bs = single(35.0f, 35.0f, kSplitHz, kSplitQ, kSplitLevel);
            m.positionCount = 3;
            m.pos[0] = positionSplit(kPickNeck, n, ns, "strat-neck");
            m.pos[1] = positionSplit(kPickNeckBridge, parallel(n, b, 5000.0f, 1.4f, 2.0f),
                                     parallel(ns, bs, 7000.0f, 1.5f, -2.0f), "strat-middle");
            m.pos[2] = positionSplit(kPickBridge, b, bs, "strat-bridge");
            m.defaultSel = kPickBridge;
            m.body = body({ 0, 350.0f, 2.0f, 0.8f }, { 2, 5000.0f, -2.0f, 0.7f });
            return m;
        }(),
        [] {
            // Jazz bass: two single coils, 34" scale (60s spacing).
            ModelSpec m; m.id = kModelJazzBass; m.key = "jazz-bass"; m.bass = true; m.scaleMm = 864.0f;
            m.midiLow = 28.0f; m.midiHigh = 43.0f;
            m.positionCount = 3;
            m.pos[0] = position(kPickNeck, jazzNeck(), "bass");
            m.pos[1] = position(kPickNeckBridge, jazzBoth(), "bass");
            m.pos[2] = position(kPickBridge, jazzBridge(), "bass");
            m.defaultSel = kPickNeckBridge;
            return m;
        }(),
        [] {
            // Precision bass: ONE split-coil humbucking pickup — the E/A half
            // nearer the neck, the D/G half nearer the bridge; each string is
            // under one half. Dark, high-inductance.
            ModelSpec m; m.id = kModelPBass; m.key = "p-bass"; m.bass = true; m.scaleMm = 864.0f;
            m.midiLow = 28.0f; m.midiHigh = 43.0f;
            PickupSound p; p.count = 1; p.pt[0] = { 162.0f, 146.0f, 1.0f, true };
            p.resonanceHz = 3000.0f; p.resonanceQ = 1.6f; p.levelDb = 3.0f;
            m.positionCount = 1;
            m.pos[0] = position(kPickMiddle, p, "bass");
            m.defaultSel = kPickMiddle;
            m.body = body({ 0, 250.0f, 1.5f, 0.9f });
            return m;
        }(),
        [] {
            ModelSpec m; m.id = kModelPJBass; m.key = "pj-bass"; m.bass = true; m.scaleMm = 864.0f;
            m.midiLow = 28.0f; m.midiHigh = 43.0f;
            PickupSound p; p.count = 1; p.pt[0] = { 162.0f, 146.0f, 1.0f, true };
            p.resonanceHz = 3000.0f; p.resonanceQ = 1.6f; p.levelDb = 3.0f;
            const PickupSound j = jazzBridge();
            m.positionCount = 3;
            m.pos[0] = position(kPickMiddle, p, "bass");
            m.pos[1] = position(kPickMiddleBridge, parallel(p, j, 4300.0f, 1.4f, 1.0f), "bass");
            m.pos[2] = position(kPickBridge, j, "bass");
            m.defaultSel = kPickMiddleBridge;
            return m;
        }(),
        [] {
            // The Washburn AB-95 hollowbody as recorded (neck pickup, fingers).
            ModelSpec m; m.id = kModelHollowBass; m.key = "hollow-bass"; m.bass = true; m.scaleMm = 864.0f;
            m.midiLow = 28.0f; m.midiHigh = 43.0f;
            m.positionCount = 1;
            m.pos[0] = position(kPickNeck, hollowBassNeck(), "bass");
            m.defaultSel = kPickNeck;
            m.body = body({ 0, 160.0f, 3.5f, 1.0f }, { 2, 2500.0f, -2.0f, 0.7f });
            return m;
        }(),
    };
    for (const ModelSpec& m : table)
        if (m.id == id)
            return &m;
    return nullptr;
}

inline const PositionSpec* findPosition(const ModelSpec& m, int32_t sel) noexcept
{
    for (int i = 0; i < m.positionCount; ++i)
        if (m.pos[i].sel == sel)
            return &m.pos[i];
    return nullptr;
}

/// True when the model has a humbucker the coil-split switch changes.
inline bool hasCoilSplit(const ModelSpec& m) noexcept
{
    for (int i = 0; i < m.positionCount; ++i)
        if (m.pos[i].splitDiffers)
            return true;
    return false;
}

/// Every model id, guitar first. For enumerating (tests, the info export).
inline constexpr int32_t kAllModels[] = { kModelSemiHollow, kModelStratSSS, kModelStratHSS, kModelTele, kModelLesPaul,
                                         kModelJazzBass, kModelPBass, kModelPJBass, kModelHollowBass };

/// The voice's selection: model, pickup selector, coil split.
struct InstrumentConfig
{
    int32_t model = kModelSemiHollow;
    int32_t pickup = kPickBridge;
    bool coilSplit = false;
    bool operator==(const InstrumentConfig& o) const noexcept
    {
        return model == o.model && pickup == o.pickup && coilSplit == o.coilSplit;
    }
    bool operator!=(const InstrumentConfig& o) const noexcept { return ! (*this == o); }
};

/// Validates a selection: the model exists, is of the right family, and has
/// that selector position. Coil split on a model without humbuckers is
/// accepted and has no effect.
inline bool validConfig(const InstrumentConfig& c, bool bass) noexcept
{
    const ModelSpec* m = findModel(c.model);
    return m != nullptr && m->bass == bass && findPosition(*m, c.pickup) != nullptr;
}

/// The default of each family: the owner's HSS Strat on its bridge humbucker,
/// and a Jazz bass on both pickups (the real Growlybass setting).
inline InstrumentConfig defaultConfig(bool bass) noexcept
{
    InstrumentConfig c;
    if (bass) { c.model = kModelJazzBass; c.pickup = kPickNeckBridge; }
    else      { c.model = kModelStratHSS; c.pickup = kPickBridge; }
    return c;
}

/// The legacy voice before #50 (the Gretsch set as recorded: its bridge
/// pickup, unconverted, exactly the v38/39 sound) — what a host
/// that never sets a model gets. Bass: the Jazz both-pickups default.
inline InstrumentConfig legacyConfig(bool bass) noexcept
{
    InstrumentConfig c;
    if (bass) { c.model = kModelJazzBass; c.pickup = kPickNeckBridge; }
    else      { c.model = kModelSemiHollow; c.pickup = kPickBridge; }
    return c;
}

} // namespace dcvoice
