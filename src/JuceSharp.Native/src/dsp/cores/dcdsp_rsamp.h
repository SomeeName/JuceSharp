// dcdsp_rsamp.h — Rocksmith 2014's own amps: each amp's soundbank FX chain,
// run as the game defines it (DotCharter #88, #70, #72).
//
// Every Rocksmith amp is a chain of up to four Wwise effect plug-ins, and every
// knob is a set of RTPC curves onto their parameters. The table of chains is the
// GAME'S data, so this library ships none of it: the host reads it from the
// user's own install and loads it at run time (dcdsp_rsamp_table.h, ABI v45).
// Without one, or for a row the loaded table has no chain for, the core passes
// audio through unchanged. This core clones the plug-ins and evaluates the
// curves:
//
//   * Audiokinetic Wwise Guitar Distortion — three pre-EQ bands, a waveshaper,
//     three post-EQ bands, output gain, wet/dry. DOCUMENTED (Wwise Help,
//     "Wwise Guitar Distortion", 2018.1): band types, which band sits before
//     the distortion, and the five transfer functions as figures. The figures
//     were digitised (axes from their tick marks; ~0.002 rms residual) and fitted:
//       Clip       y = clamp(x, -0.5, 0.5)                      (exact)
//       Overdrive  slope 1.318 to |x| = 0.254, then a C1 quadratic knee onto
//                  0.5 at |x| = 0.505
//       Heavy      y = sign(x) (1 - e^(-9.983 |x|))
//       Fuzz       x >= 0: min(x - 0.01826 (1 - e^(-x / 0.05918)), 0.5);
//                  x < 0: c (e^(x/c) - 1), c = 0.1205 at the figure's Tone 10
//     Rectification per the same page (half-wave up to 50 %, full-wave above).
//     INFERRED: the published figures carry no drive, and the drive law is not
//     published — here Drive 0..100 % is 0..+25 dB of input gain; how Tone
//     moves the Fuzz knee away from Tone 10 (c = 0.012 x Tone); the band
//     filters are RBJ's cookbook sections (peaking Q as RBJ's Q; shelves at
//     S = 1 and the low/high-pass at Q 0.707, since the page says those types
//     take no Q).
//   * Rocksmith's Windows build runs iZotope Trash Multiband Distortion as the
//     amp's drive stage, whose algorithm and parameter block are unpublished.
//     The game's own Mac build replaces it with a Guitar Distortion block in
//     the same bank, and that is the block this core runs.
//   * McDSP FutzBox — the help page ("McDSP FutzBox", Wwise 2018.1) documents
//     its sections, their ranges and that "the first stage of audio processing
//     is the high pass and Low-Pass Filters" with resonance up to "approximately
//     24 dB". Its distortion MODES are unpublished. INFERRED here: resonance
//     r % is a peak of 0.24 r dB (Q = 10^(0.012 r), at least 0.707); a 12 dB/oct
//     slope (the only one this core runs); the distortion is
//     input gain (Amount 0..100 % = 0..+50 dB) into a blend of tanh (Intensity 0)
//     and a hard clip (Intensity 100, "bright"), whichever mode the bank names.
//   * Audiokinetic Wwise Compressor: threshold, ratio,
//     attack, release, output; per channel, hard knee.
//   * RTPC curves: AkCurveInterpolation shapes as in RsGameCurves.cs; a dB curve
//     is stored sign(d)(1 - 10^(-|d|/20)) and interpolated in that domain, a Hz
//     curve in log10. The S-curves' formula is unpublished: they run linear in
//     the stored domain (the converter reports every knob that rides one).
//   * Level: the chain sees the rig's guitar 18 dB hotter and hands it back
//     18 dB lower, so a full strum at the rig's -18 dBFS reference reaches the
//     chain peaking near 0 dBFS — a calibrated, near-full-scale instrument
//     input, the level a chain's gain staging is built around — and the
//     module stays at unity overall.
//
// Params: `model` (a row of dcdsp_rsamp_models.h), `k1`..`k10`, each the 0..1 travel of
// the model's knob i between its manifest min and max (a switch or selector
// rounds), and `level`, an output trim in dB (ramped over 30 ms) that DotCharter's
// loudness levelling uses when a chain needs more than its trailing gain's ±24 dB. Knobs ramp over 30 ms and re-solve the chain every 32 samples while
// they move; a model change dips out and back in over 5 ms each way, and so does
// a newly loaded (or unloaded) table: the core sees it at the top of a block with
// one acquire load and swaps at the silent bottom of the dip.
// Pure dcdsp core: no JUCE, no I/O, RT-safe (alloc only in prepare()).
#pragma once

#ifdef DCDSP_TEST_STUBS
  #include "test_stubs.h"
#else
  #include "dcdsp_primitives.h"
#endif
#include "dcdsp_detail_os.h"
#include "dcdsp_rsamp_table.h"

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace dcdsp {

struct RsAmpParams {
    int   model = 0;
    float levelDb = 0.f;   // output trim, -24..+24 dB (the levelling's spill past its trailing gain)
    float knob[rsamp::kMaxKnobs] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
};

namespace rsamp {

inline constexpr double kInputDb       = 18.0;
inline constexpr double kOutputDb      = -18.0;
inline constexpr double kGdDriveDbPct  = 0.25;   // Guitar Distortion drive: dB of input gain per %
inline constexpr double kFutzAmountDbPct = 0.5; // FutzBox amount: dB of input gain per %
inline constexpr double kFutzResDbPct  = 0.24;   // FutzBox filter resonance: peak dB per %

// ---------------------------------------------------------------- curves
inline double shapeFraction(int shape, double t) {
    switch (shape) {
        case 6: return std::pow(t, 1.41);                       // Exp1
        case 8: return t * t * t;                               // Exp3
        case 2: return 1.0 - std::pow(1.0 - t, 1.41);           // Log1
        case 0: { const double u = 1.0 - t; return 1.0 - u * u * u; } // Log3
        case 1: return std::sin(t * 1.5707963267948966);        // Sine
        case 7: return 1.0 - std::cos(t * 1.5707963267948966);  // SineRecip
        case 9: return t >= 1.0 ? 1.0 : 0.0;                    // Constant
        default: return t;                                      // Linear; S-curves (3, 5) run linear
    }
}

inline double fromStored(int scaling, double y) {
    if (scaling == 2) {
        const double a = std::min(std::fabs(y), 0.999999);
        const double db = -20.0 * std::log10(1.0 - a);
        return y < 0.0 ? -db : db;
    }
    if (scaling == 3) return std::pow(10.0, y);
    return y;
}

inline double evaluate(const Curve& c, const CurvePoint* points, double x) {
    const CurvePoint* p = &points[c.firstPoint];
    const int n = c.numPoints;
    if (n <= 0) return 0.0;
    if (!(x > p[0].x)) return fromStored(c.scaling, p[0].y);    // NaN lands here
    for (int i = 0; i + 1 < n; ++i) {
        if (x <= p[i + 1].x) {
            const double span = (double) p[i + 1].x - p[i].x;
            const double t = span > 0.0 ? (x - p[i].x) / span : 1.0;
            return fromStored(c.scaling, p[i].y + ((double) p[i + 1].y - p[i].y) * shapeFraction(p[i].shape, t));
        }
    }
    return fromStored(c.scaling, p[n - 1].y);
}

// ------------------------------------------------------------- waveshapers
inline double gdRectify(double x, double rectPct) {
    if (!(rectPct > 0.0) || x >= 0.0) return x;
    if (rectPct <= 50.0) return std::max(x, -(1.0 - rectPct / 50.0));
    return -x * (std::min(rectPct, 100.0) - 50.0) / 50.0;
}

inline double gdShape(uint32_t type, double x, double tonePct) {
    switch (type) {
        case 4: return std::clamp(x, -0.5, 0.5);                                   // Clip
        case 1: {                                                                  // Overdrive
            constexpr double m = 1.3179774, a = 0.25394439;
            constexpr double b = a + 2.0 * (0.5 - m * a) / m;
            const double ax = std::fabs(x);
            double y;
            if (ax <= a)      y = m * ax;
            else if (ax < b)  { const double d = ax - a; y = m * a + m * d - m * d * d / (2.0 * (b - a)); }
            else              y = 0.5;
            return x < 0.0 ? -y : y;
        }
        case 2: {                                                                  // Heavy
            const double y = 1.0 - std::exp(-9.98345241 * std::fabs(x));
            return x < 0.0 ? -y : y;
        }
        case 3: {                                                                  // Fuzz
            if (x >= 0.0) return std::min(x - 0.01826209 * (1.0 - std::exp(-x / 0.0591784)), 0.5);
            const double c = std::clamp(0.012048127 * tonePct, 0.005, 0.5);
            return c * (std::exp(x / c) - 1.0);
        }
        default: return x;
    }
}

// ------------------------------------------------------- biquad (RBJ, double)
struct Coefs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };

struct BqState {
    double z1 = 0.0, z2 = 0.0;
    inline double run(const Coefs& c, double x) {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.0; }
};

// type: 0 low shelf, 1 peaking, 2 high shelf, 3 low-pass, 4 high-pass,
// 5 band-pass (0 dB peak), 6 notch. Shelves run at S = 1, the passes at Q 0.707.
inline Coefs design(uint32_t type, double sr, double hz, double q, double gainDb) {
    const double s = sr > 0.0 ? sr : 48000.0;
    if (!(hz >= 10.0)) hz = 10.0;
    hz = std::min(hz, 0.49 * s);
    if (!(q >= 0.1)) q = 0.1;
    q = std::min(q, 40.0);
    if (!(gainDb > -48.0)) gainDb = (gainDb < 0.0) ? -48.0 : 0.0;
    gainDb = std::min(gainDb, 48.0);
    const double w0 = 6.283185307179586 * hz / s, cw = std::cos(w0), sw = std::sin(w0);
    const double A = std::pow(10.0, gainDb / 40.0);
    double b0, b1, b2, a0, a1, a2;
    switch (type) {
        case 0: case 2: {
            const double t = 2.0 * std::sqrt(A) * (sw * 0.7071067811865476);
            const double sg = (type == 0) ? 1.0 : -1.0;   // low shelf: -cw terms, high: +
            b0 =      A * ((A + 1.0) - sg * (A - 1.0) * cw + t);
            b1 = sg * 2.0 * A * ((A - 1.0) - sg * (A + 1.0) * cw);
            b2 =      A * ((A + 1.0) - sg * (A - 1.0) * cw - t);
            a0 =           (A + 1.0) + sg * (A - 1.0) * cw + t;
            a1 = -sg * 2.0 * ((A - 1.0) + sg * (A + 1.0) * cw);
            a2 =           (A + 1.0) + sg * (A - 1.0) * cw - t;
            break;
        }
        case 1: {
            const double al = sw / (2.0 * q);
            b0 = 1.0 + al * A; b1 = -2.0 * cw; b2 = 1.0 - al * A;
            a0 = 1.0 + al / A; a1 = -2.0 * cw; a2 = 1.0 - al / A;
            break;
        }
        case 3: case 4: {
            const double al = sw / (2.0 * 0.7071067811865476);
            const double k = (type == 3) ? (1.0 - cw) : (1.0 + cw);
            b0 = k * 0.5; b1 = (type == 3) ? k : -k; b2 = k * 0.5;
            a0 = 1.0 + al; a1 = -2.0 * cw; a2 = 1.0 - al;
            break;
        }
        case 5: {
            const double al = sw / (2.0 * q);
            b0 = al; b1 = 0.0; b2 = -al; a0 = 1.0 + al; a1 = -2.0 * cw; a2 = 1.0 - al;
            break;
        }
        case 6: {
            const double al = sw / (2.0 * q);
            b0 = 1.0; b1 = -2.0 * cw; b2 = 1.0; a0 = 1.0 + al; a1 = -2.0 * cw; a2 = 1.0 - al;
            break;
        }
        default: return Coefs{};
    }
    Coefs c;
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
    return c;
}

// A resonant 2-pole low-/high-pass at an explicit Q (the FutzBox filters).
inline Coefs designPass(bool low, double sr, double hz, double q) {
    const double s = sr > 0.0 ? sr : 48000.0;
    if (!(hz >= 10.0)) hz = 10.0;
    hz = std::min(hz, 0.49 * s);
    if (!(q >= 0.1)) q = 0.1;
    const double w0 = 6.283185307179586 * hz / s, cw = std::cos(w0), al = std::sin(w0) / (2.0 * q);
    const double k = low ? (1.0 - cw) : (1.0 + cw);
    const double a0 = 1.0 + al;
    Coefs c;
    c.b0 = k * 0.5 / a0; c.b1 = (low ? k : -k) / a0; c.b2 = k * 0.5 / a0;
    c.a1 = -2.0 * cw / a0; c.a2 = (1.0 - al) / a0;
    return c;
}

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double clampPct(double v) { return (v > 0.0) ? std::min(v, 100.0) : 0.0; }   // NaN -> 0

} // namespace rsamp

class RsAmpCore {
public:
    static constexpr int kOs  = 4;
    static constexpr int kSub = 32;
    static constexpr int kSubRamp = 8;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.resize((size_t) std::max(1, numChannels));
        for (auto& c : ch_) {
            for (int s = 0; s < rsamp::kMaxSlots; ++s) {
                c.os[s].prepare(sr_, kSub, kOs);
                c.dc[s].prepare(sr_, 5.0);
            }
        }
        for (auto& k : knobSm_) k.reset(sr_, 0.03f);
        fade_.reset(sr_, 0.005f);
        level_.reset(sr_, 0.03f);
        reset();
        activeModel_ = clampModel(params_.model);
        pendingModel_ = activeModel_;
        table_ = pendingTable_ = rsamp::TableRegistry::current();
        for (int i = 0; i < rsamp::kMaxKnobs; ++i) { knobSm_[i].setTarget(params_.knob[i]); knobSm_[i].snapToTarget(); }
        fade_.setTarget(1.f); fade_.snapToTarget();
        level_.setTarget(levelGain(params_.levelDb)); level_.snapToTarget();
        derive();
        prepared_ = true;
    }

    void reset() {
        for (auto& c : ch_) {
            for (int s = 0; s < rsamp::kMaxSlots; ++s) {
                for (auto& b : c.bq[s]) b.reset();
                c.os[s].reset();
                c.dc[s].reset();
                c.envDb[s] = -120.0;
            }
        }
    }

    void setParams(const RsAmpParams& p) {
        params_ = p;
        for (int i = 0; i < rsamp::kMaxKnobs; ++i) {
            const float v = p.knob[i];
            params_.knob[i] = (v > 0.f) ? std::min(v, 1.f) : 0.f;   // NaN -> 0
            knobSm_[i].setTarget(params_.knob[i]);
        }
        const int m = clampModel(p.model);
        params_.model = m;
        level_.setTarget(levelGain(p.levelDb));
        if (!prepared_) { activeModel_ = pendingModel_ = m; return; }
        // A model change dips out, swaps the chain at silence and comes back; a
        // change BACK before the dip lands just returns to full level.
        pendingModel_ = m;
        fade_.setTarget(swapPending() ? 0.f : 1.f);
        dirty_ = true;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (numFrames > maxBlock_ || numFrames <= 0) return;
        const int nch = std::min(numChannels, (int) ch_.size());
        // A table loaded (or cleared) since the last block: dip and swap, exactly
        // as for a model change. One acquire load; the table itself is never
        // freed under us (dcdsp_rsamp_table.h).
        if (const rsamp::Table* t = rsamp::TableRegistry::current(); t != pendingTable_) {
            pendingTable_ = t;
            fade_.setTarget(swapPending() ? 0.f : 1.f);
        }
        for (int off = 0; off < numFrames;) {
            // While a knob ramps the chain is re-solved every kSubRamp samples
            // (a coefficient set held for 32 samples ticks audibly on a sine);
            // at rest the sub-block is kSub.
            bool moving = false;
            for (auto& k : knobSm_) moving |= k.isRamping();
            const int m = std::min(moving ? kSubRamp : kSub, numFrames - off);
            for (auto& k : knobSm_) k.advanceBlock(m);
            if (moving || dirty_) { derive(); dirty_ = false; }

            // The model dip and the level trim ride one per-sample line.
            float step = 0.f, lstep = 0.f;
            const float f0 = fade_.blockStart(m, step);
            const float l0 = level_.blockStart(m, lstep);
            fade_.advanceBlock(m);
            level_.advanceBlock(m);
            const float g0 = f0 * l0;
            for (int c = 0; c < nch; ++c) {
                float* x = channels[c];
                if (!x) continue;
                runChain(ch_[(size_t) c], x + off, m);
                if (g0 != 1.f || step != 0.f || lstep != 0.f)
                    for (int i = 0; i < m; ++i)
                        x[off + i] *= (f0 + step * (float) i) * (l0 + lstep * (float) i);
            }
            if (swapPending() && !fade_.isRamping() && fade_.current() <= 0.f) {
                activeModel_ = pendingModel_;
                table_ = pendingTable_;
                reset();
                for (auto& k : knobSm_) k.snapToTarget();
                derive();
                fade_.setTarget(1.f);
            }
            off += m;
        }
    }

    int latencySamples() const { return 0; }

    // ---- test hooks -------------------------------------------------------
    int activeModelForTest() const { return activeModel_; }
    // The table the chain runs now (nullptr: none, so it passes audio through).
    const rsamp::Table* tableForTest() const { return table_; }
    // Whether the active model has a chain in that table.
    bool runsChainForTest() const { return activeRow() != nullptr; }
    double knobValueForTest(int i) const { return (i >= 0 && i < rsamp::kMaxKnobs) ? raw_[i] : 0.0; }
    const rsamp::GdBlock&   gdForTest(int slot) const   { return gd_[slot]; }
    const rsamp::FutzBlock& futzForTest(int slot) const { return fz_[slot]; }
    const rsamp::CompBlock& compForTest(int slot) const { return cp_[slot]; }

private:
    static float levelGain(float db) {
        const float d = (db > -24.f) ? std::min(db, 24.f) : (db < 0.f ? -24.f : 0.f);   // NaN -> 0 dB
        return std::pow(10.f, d / 20.f);
    }

    static int clampModel(int m) { return m < 0 ? 0 : (m >= rsamp::kNumModels ? rsamp::kNumModels - 1 : m); }

    bool swapPending() const { return pendingModel_ != activeModel_ || pendingTable_ != table_; }

    const rsamp::Model* activeRow() const { return table_ != nullptr ? table_->row(activeModel_) : nullptr; }

    struct Channel {
        rsamp::BqState bq[rsamp::kMaxSlots][6];
        detail::OversamplerNx os[rsamp::kMaxSlots];
        DcBlocker dc[rsamp::kMaxSlots];
        double envDb[rsamp::kMaxSlots] = {};
        float dry[kSub] = {};
    };

    // Live parameters of one slot, solved from the base block + the curves.
    struct Live {
        rsamp::Coefs c[6];
        bool on[6] = {};
        bool dist = false;
        uint32_t distType = 0;
        double drive = 1.0, tone = 50.0, rect = 0.0, out = 1.0, wet = 1.0;
        // FutzBox
        double in = 1.0, intensity = 0.0;
        // Compressor
        double thr = 0.0, ratio = 1.0, att = 0.0, rel = 0.0;
    };

    void applyParam(int slot, rsamp::Slot kind, int p, double v) {
        using rsamp::Slot;
        if (kind == Slot::Gd) {
            auto& b = gd_[slot];
            if (p < 60) {
                auto& band = b.band[p / 10];
                switch (p % 10) {
                    case 0: band.type = (uint32_t) std::clamp((int) std::lround(v), 0, 6); break;
                    case 1: band.gainDb = (float) v; break;
                    case 2: band.hz = (float) v; break;
                    case 3: band.q = (float) v; break;
                    case 4: band.on = v >= 0.5 ? 1 : 0; break;
                    default: break;
                }
            } else {
                switch (p) {
                    case 60: b.dist = (uint32_t) std::clamp((int) std::lround(v), 0, 4); break;
                    case 61: b.drive = (float) v; break;
                    case 62: b.tone = (float) v; break;
                    case 63: b.rect = (float) v; break;
                    case 64: b.outDb = (float) v; break;
                    case 65: b.wet = (float) v; break;
                    default: break;
                }
            }
        } else if (kind == Slot::Futz) {
            auto& b = fz_[slot];
            switch (p) {
                case 0: b.distOn = v >= 0.5 ? 1 : 0; break;
                case 1: b.mode = (uint32_t) std::max(0L, std::lround(v)); break;
                case 2: b.amount = (float) v; break;
                case 3: b.intensity = (float) v; break;
                case 4: b.rectify = (float) v; break;
                case 40: b.inDb = (float) v; break;
                case 41: b.outDb = (float) v; break;
                case 42: b.balance = (float) v; break;
                case 50: b.filtersOn = v >= 0.5 ? 1 : 0; break;
                case 52: b.lpfHz = (float) v; break;
                case 53: b.lpfRes = (float) v; break;
                case 55: b.hpfHz = (float) v; break;
                case 56: b.hpfRes = (float) v; break;
                case 60: b.eqOn = v >= 0.5 ? 1 : 0; break;
                case 61: b.eqType = (uint32_t) std::clamp((int) std::lround(v), 0, 2); break;
                case 62: b.eqHz = (float) v; break;
                case 63: b.eqQ = (float) v; break;
                case 64: b.eqDb = (float) v; break;
                default: break;
            }
        } else if (kind == Slot::Comp) {
            auto& b = cp_[slot];
            switch (p) {
                case 0: b.thresholdDb = (float) v; break;
                case 1: b.ratio = (float) v; break;
                case 2: b.attackS = (float) v; break;
                case 3: b.releaseS = (float) v; break;
                case 4: b.outDb = (float) v; break;
                default: break;
            }
        }
    }

    // Base blocks + every curve at the knobs' current values -> live coefficients.
    void derive() {
        using namespace rsamp;
        const Model* row = activeRow();
        if (row == nullptr) {
            for (double& r : raw_) r = 0.0;
            return;
        }
        const Model& md = *row;
        const Table& tb = *table_;
        for (int i = 0; i < kMaxKnobs; ++i) {
            if (i >= md.numKnobs) { raw_[i] = 0.0; continue; }
            const Knob& k = md.knob[i];
            double v = k.min + (double) knobSm_[i].current() * ((double) k.max - k.min);
            if (k.discrete) v = std::round(v);
            raw_[i] = v;
        }
        for (int s = 0; s < md.numSlots; ++s) {
            switch (md.kind[s]) {
                case Slot::Gd:   gd_[s] = tb.gd[md.index[s]]; break;
                case Slot::Futz: fz_[s] = tb.futz[md.index[s]]; break;
                case Slot::Comp: cp_[s] = tb.comp[md.index[s]]; break;
                default: break;
            }
        }
        for (int i = 0; i < md.numCurves; ++i) {
            const Curve& c = tb.curves[(size_t) md.firstCurve + (size_t) i];
            if (c.slot >= md.numSlots || c.knob >= md.numKnobs) continue;
            applyParam(c.slot, md.kind[c.slot], c.param, evaluate(c, tb.points.data(), raw_[c.knob]));
        }
        for (int s = 0; s < md.numSlots; ++s) {
            Live& L = live_[s];
            switch (md.kind[s]) {
                case Slot::Gd: {
                    const GdBlock& b = gd_[s];
                    for (int j = 0; j < 6; ++j) {
                        L.on[j] = b.band[j].on != 0;
                        if (L.on[j]) L.c[j] = design(b.band[j].type, sr_, b.band[j].hz, b.band[j].q, b.band[j].gainDb);
                    }
                    L.distType = b.dist;
                    L.dist = b.dist >= 1 && b.dist <= 4;
                    L.drive = dbToLin(clampPct(b.drive) * kGdDriveDbPct);
                    L.tone = clampPct(b.tone);
                    L.rect = clampPct(b.rect);
                    L.out = dbToLin(std::clamp((double) b.outDb, -96.0, 48.0));
                    L.wet = clampPct(b.wet) / 100.0;
                    break;
                }
                case Slot::Futz: {
                    const FutzBlock& b = fz_[s];
                    L.in = dbToLin(std::clamp((double) b.inDb, -96.0, 48.0));
                    L.on[0] = L.on[1] = b.filtersOn != 0;
                    if (L.on[0]) {
                        const double qh = std::max(0.7071067811865476, std::pow(10.0, clampPct(b.hpfRes) * kFutzResDbPct / 20.0));
                        const double ql = std::max(0.7071067811865476, std::pow(10.0, clampPct(b.lpfRes) * kFutzResDbPct / 20.0));
                        L.c[0] = designPass(false, sr_, b.hpfHz, qh);
                        L.c[1] = designPass(true, sr_, b.lpfHz, ql);
                    }
                    L.dist = b.distOn != 0;
                    L.drive = dbToLin(clampPct(b.amount) * kFutzAmountDbPct);
                    L.intensity = clampPct(b.intensity) / 100.0;
                    L.rect = clampPct(b.rectify) / 100.0;
                    L.on[2] = b.eqOn != 0;
                    if (L.on[2]) {
                        static constexpr uint32_t kEqType[3] = { 4, 1, 3 };   // HPF, EQ (peaking), LPF
                        L.c[2] = design(kEqType[std::min<uint32_t>(b.eqType, 2)], sr_, b.eqHz, b.eqQ, b.eqDb);
                    }
                    L.out = dbToLin(std::clamp((double) b.outDb, -96.0, 48.0));
                    L.wet = clampPct(b.balance) / 100.0;
                    break;
                }
                case Slot::Comp: {
                    const CompBlock& b = cp_[s];
                    L.thr = std::clamp((double) b.thresholdDb, -96.0, 0.0);
                    L.ratio = std::max(1.0, std::isfinite(b.ratio) ? (double) b.ratio : 1.0);
                    L.att = std::exp(-1.0 / (std::max(1e-4, (double) b.attackS) * sr_));
                    L.rel = std::exp(-1.0 / (std::max(1e-4, (double) b.releaseS) * sr_));
                    L.out = dbToLin(std::clamp((double) b.outDb, -96.0, 48.0));
                    break;
                }
                default: break;
            }
        }
    }

    void runChain(Channel& ch, float* x, int m) {
        using namespace rsamp;
        const Model* row = activeRow();
        if (row == nullptr) return;   // no chain for this row: audio passes through
        const Model& md = *row;
        const double inG = dbToLin(kInputDb), outG = dbToLin(kOutputDb);
        for (int i = 0; i < m; ++i) x[i] = (float) (x[i] * inG);
        for (int s = 0; s < md.numSlots; ++s) {
            const Live& L = live_[s];
            switch (md.kind[s]) {
                case Slot::Gd:   runGd(ch, s, L, x, m); break;
                case Slot::Futz: runFutz(ch, s, L, x, m); break;
                case Slot::Comp: runComp(ch, s, L, x, m); break;
                default: break;
            }
        }
        for (int i = 0; i < m; ++i) x[i] = detail::sanitize((float) (x[i] * outG));
    }

    void runGd(Channel& ch, int s, const Live& L, float* x, int m) {
        for (int i = 0; i < m; ++i) ch.dry[i] = x[i];
        for (int j = 0; j < 3; ++j)
            if (L.on[j]) for (int i = 0; i < m; ++i) x[i] = (float) ch.bq[s][j].run(L.c[j], x[i]);
        if (L.dist) {
            ch.os[s].upsample(x, m);
            float* up = ch.os[s].data();
            const int u = ch.os[s].upSize(m);
            for (int i = 0; i < u; ++i)
                up[i] = (float) rsamp::gdShape(L.distType, rsamp::gdRectify(up[i] * L.drive, L.rect), L.tone);
            ch.os[s].downsample(x, m);
            for (int i = 0; i < m; ++i) x[i] = ch.dc[s].process(x[i]);
        }
        for (int j = 3; j < 6; ++j)
            if (L.on[j]) for (int i = 0; i < m; ++i) x[i] = (float) ch.bq[s][j].run(L.c[j], x[i]);
        for (int i = 0; i < m; ++i) x[i] = (float) (L.wet * L.out * x[i] + (1.0 - L.wet) * ch.dry[i]);
    }

    void runFutz(Channel& ch, int s, const Live& L, float* x, int m) {
        for (int i = 0; i < m; ++i) { ch.dry[i] = x[i]; x[i] = (float) (x[i] * L.in); }
        if (L.on[0])
            for (int i = 0; i < m; ++i) x[i] = (float) ch.bq[s][1].run(L.c[1], ch.bq[s][0].run(L.c[0], x[i]));
        if (L.dist) {
            ch.os[s].upsample(x, m);
            float* up = ch.os[s].data();
            const int u = ch.os[s].upSize(m);
            for (int i = 0; i < u; ++i) {
                double v = up[i] * L.drive;
                if (v < 0.0) v *= 1.0 - L.rect;
                up[i] = (float) ((1.0 - L.intensity) * std::tanh(v) + L.intensity * std::clamp(v, -1.0, 1.0));
            }
            ch.os[s].downsample(x, m);
            for (int i = 0; i < m; ++i) x[i] = ch.dc[s].process(x[i]);
        }
        if (L.on[2]) for (int i = 0; i < m; ++i) x[i] = (float) ch.bq[s][2].run(L.c[2], x[i]);
        for (int i = 0; i < m; ++i) x[i] = (float) (L.wet * L.out * x[i] + (1.0 - L.wet) * ch.dry[i]);
    }

    void runComp(Channel& ch, int s, const Live& L, float* x, int m) {
        double env = ch.envDb[s];
        const double slope = 1.0 - 1.0 / L.ratio;
        for (int i = 0; i < m; ++i) {
            const double lvl = 20.0 * std::log10(std::fabs((double) x[i]) + 1e-9);
            env = lvl > env ? L.att * env + (1.0 - L.att) * lvl : L.rel * env + (1.0 - L.rel) * lvl;
            const double gr = env > L.thr ? (env - L.thr) * slope : 0.0;
            x[i] = (float) (x[i] * L.out * std::pow(10.0, -gr / 20.0));
        }
        ch.envDb[s] = env;
    }

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    bool prepared_ = false, dirty_ = false;
    int activeModel_ = 0, pendingModel_ = 0;
    const rsamp::Table* table_ = nullptr;          // what derive()/runChain() read
    const rsamp::Table* pendingTable_ = nullptr;   // the registry's, seen at a block top
    RsAmpParams params_;
    SmoothedValue knobSm_[rsamp::kMaxKnobs];
    SmoothedValue fade_;
    SmoothedValue level_;
    double raw_[rsamp::kMaxKnobs] = {};
    rsamp::GdBlock gd_[rsamp::kMaxSlots] = {};
    rsamp::FutzBlock fz_[rsamp::kMaxSlots] = {};
    rsamp::CompBlock cp_[rsamp::kMaxSlots] = {};
    Live live_[rsamp::kMaxSlots];
    std::vector<Channel> ch_;
};

} // namespace dcdsp
