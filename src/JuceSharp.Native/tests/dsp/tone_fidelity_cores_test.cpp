// Rocksmith tone fidelity, the native half (DotCharter #87, #75, #74): each
// claim below is a MEASURED response of a dcdsp core held against the source
// it was built from — the RBJ / TPT design formula, a published microphone
// chart, a loudspeaker datasheet. Header-only cores, no device, no JUCE.
//
//  1. Filter corners above a quarter of the rate (#87). Every core whose param
//     advertises a corner past sr/4 designs it with clampCutoffNearNyquist;
//     everything else keeps clampCutoff. Probed with settled sines.
//  2. micpos: the TUBE condenser curve against the Neumann U 67 cardioid chart,
//     and the off-axis ANGLE against the Shure SM57 and U 67 polar charts (#75).
//  3. The cab bank: rows 0-11 untouched, rows 12+ minimum-phase, levelled with
//     the bank, and ordered the way their drivers' datasheets order them (#74).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dsp/cores/dcdsp_primitives.h"
#include "dsp/cores/dcdsp_svf.h"
#include "dsp/cores/dcdsp_graphiceq.h"
#include "dsp/cores/dcdsp_hallreverb.h"
#include "dsp/cores/dcdsp_gatedreverb.h"
#include "dsp/cores/dcdsp_reversereverb.h"
#include "dsp/cores/dcdsp_platereverb.h"
#include "dsp/cores/dcdsp_convreverb.h"
#include "dsp/cores/dcdsp_tapesat.h"
#include "dsp/cores/dcdsp_micpos.h"
#include "dsp/cores/dcdsp_cab_irs.h"

namespace {

int failures = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "  ok  " : "FAIL  ", what.c_str());
    if (! ok) ++failures;
}
std::string f2(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
double dB(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }
constexpr double kPi = 3.14159265358979323846;

// Amplitude of `hz` over `n` samples from `start` (Goertzel; exact when hz*n/sr
// is whole).
double amplitude(const std::vector<float>& x, double hz, double sr, size_t start, size_t n)
{
    const double w = 2.0 * kPi * hz / sr, cw = std::cos(w), coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) { const double s0 = x[start + i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    const double re = s1 - s2 * cw, im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / (double) n;
}

// One settled sine through a core, returns its gain in dB at `hz`.
template <class Core>
double sineGainDb(Core& core, double hz, double sr, float amp = 0.1f, int block = 480)
{
    const size_t total = (size_t) sr * 2;           // 1 s settle + 1 s measure
    std::vector<float> l(total), r(total);
    for (size_t i = 0; i < total; ++i) l[i] = r[i] = amp * (float) std::sin(2.0 * kPi * hz * (double) i / sr);
    for (size_t at = 0; at < total; at += (size_t) block)
    {
        const int n = (int) std::min((size_t) block, total - at);
        float* ch[2] = { l.data() + at, r.data() + at };
        core.process(ch, 2, n);
    }
    return dB(amplitude(l, hz, sr, (size_t) sr, (size_t) sr) / amp);
}

// RBJ cookbook magnitudes (dB) — the formulas the primitives transcribe.
double rbjMag(const double b[3], const double a[3], double f, double sr)
{
    const double w = 2.0 * kPi * f / sr;
    const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
    const double nr = b[0] + b[1] * cr + b[2] * c2r, ni = b[1] * ci + b[2] * c2i;
    const double dr = a[0] + a[1] * cr + a[2] * c2r, di = a[1] * ci + a[2] * c2i;
    return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}
double rbjLowpassDb(double f0, double q, double f, double sr)
{
    const double w0 = 2 * kPi * f0 / sr, al = std::sin(w0) / (2 * q), c = std::cos(w0);
    const double b[3] = { (1 - c) / 2, 1 - c, (1 - c) / 2 }, a[3] = { 1 + al, -2 * c, 1 - al };
    return rbjMag(b, a, f, sr);
}
double rbjPeakDb(double f0, double q, double g, double f, double sr)
{
    const double A = std::pow(10.0, g / 40.0), w0 = 2 * kPi * f0 / sr, al = std::sin(w0) / (2 * q), c = std::cos(w0);
    const double b[3] = { 1 + al * A, -2 * c, 1 - al * A }, a[3] = { 1 + al / A, -2 * c, 1 - al / A };
    return rbjMag(b, a, f, sr);
}

// ============================================================ 1. filter ceilings

void primitivesOptIn()
{
    using namespace dcdsp;
    check(std::fabs(clampCutoff(48000.0, 16000.0) - 0.4999 * 24000.0) < 1e-9,
          "clampCutoff still holds 16 kHz at a quarter of 48 kHz (the default is unchanged)");
    check(clampCutoffNearNyquist(48000.0, 16000.0) == 16000.0,
          "clampCutoffNearNyquist passes 16 kHz at 48 kHz");
    check(std::fabs(clampCutoffNearNyquist(44100.0, 20000.0) - 19845.0) < 1e-9,
          "clampCutoffNearNyquist holds 20 kHz at 0.45 x 44.1 kHz = 19845 Hz");
    check(clampCutoffNearNyquist(48000.0, std::nan("")) == 1.0, "clampCutoffNearNyquist sends NaN to 1 Hz");

    // A Biquad low-pass designed near Nyquist IS the RBJ low-pass at that corner.
    Biquad bq; bq.setLowpass(48000.0, 16000.0, 0.70710678, true);
    struct One { Biquad* q; void process(float* const* c, int, int n) { for (int i = 0; i < n; ++i) { const float y = q->process(c[0][i]); c[0][i] = c[1][i] = y; } } } one{ &bq };
    const double got = sineGainDb(one, 16000.0, 48000.0);
    const double want = rbjLowpassDb(16000.0, 0.70710678, 16000.0, 48000.0);
    check(std::fabs(got - want) < 0.05, "Biquad::setLowpass(nearNyquist) at 16 kHz: " + f2(got) + " dB, RBJ " + f2(want));
}

void svfCorner()
{
    // TPT state-variable low-pass, Q = 0.707: |H(fc)| = -3.01 dB exactly
    // (bilinear with prewarping), wherever fc sits below Nyquist.
    for (double sr : { 48000.0, 44100.0 })
    {
        dcdsp::SvfCore svf; dcdsp::SvfParams p;
        p.mode = 0; p.cutoffHz = 16000.f; p.resonance = 0.f; p.drive = 0.f; p.source = 0; p.mix = 1.f;
        svf.setParams(p); svf.prepare(sr, 480, 2); svf.setParams(p);
        const double g = sineGainDb(svf, 16000.0, sr);
        check(std::fabs(g + 3.01) < 0.25,
              "svf low-pass at 16 kHz (" + f2(sr) + " Hz): " + f2(g) + " dB at the corner, want -3.01");
    }
    // Past 0.45 sr the corner is HELD there (and stays finite), not run at 20 kHz.
    dcdsp::SvfCore a, b; dcdsp::SvfParams pa, pb;
    pa.cutoffHz = 20000.f; pb.cutoffHz = 19845.f; pa.resonance = pb.resonance = 0.f; pa.mix = pb.mix = 1.f;
    a.setParams(pa); a.prepare(44100.0, 480, 2); a.setParams(pa);
    b.setParams(pb); b.prepare(44100.0, 480, 2); b.setParams(pb);
    const double ga = sineGainDb(a, 15000.0, 44100.0), gb = sineGainDb(b, 15000.0, 44100.0);
    check(std::isfinite(ga) && std::fabs(ga - gb) < 1e-6,
          "svf asked for 20 kHz at 44.1 kHz renders as 19845 Hz (" + f2(ga) + " vs " + f2(gb) + " dB)");
}

void graphicEqTopBand()
{
    dcdsp::GraphicEqCore eq; dcdsp::GraphicEqParams p;
    p.bands[9] = 12.f;
    eq.setParams(p); eq.prepare(48000.0, 480, 2); eq.setParams(p);
    const double g = sineGainDb(eq, 16000.0, 48000.0, 0.02f);
    // At its own centre a peaking section's gain IS the knob, and the eight
    // other bands sit at 0 dB.
    const double want = 12.0;
    check(std::fabs(g - want) < 0.3, "graphiceq 16 kHz band at +12 dB: " + f2(g) + " dB at 16 kHz, want " + f2(want));
    // ...and a FLAT graphic EQ is flat at the top: its safety limiter is out
    // of the path below its knee (it was a two-tap average there: -3 dB at
    // 12 kHz on every graphic EQ).
    dcdsp::GraphicEqCore flat; dcdsp::GraphicEqParams z;
    flat.setParams(z); flat.prepare(48000.0, 480, 2); flat.setParams(z);
    for (double hz : { 8000.0, 12000.0, 16000.0 })
    {
        const double gz = sineGainDb(flat, hz, 48000.0, 0.1f);
        check(std::fabs(gz) < 0.05, "flat graphiceq at " + f2(hz) + " Hz: " + f2(gz) + " dB");
    }
}

// Tail brightness of a reverb core: (8-16 kHz) minus (1-2 kHz) energy in the
// tail after a noise burst, in dB.
template <class Core, class P>
double tailTilt(P p, double sr = 48000.0)
{
    Core c; c.setParams(p); c.prepare(sr, 480, 2); c.setParams(p);
    const size_t n = (size_t) (sr * 1.5);
    std::vector<float> l(n, 0.f), r(n, 0.f);
    uint32_t s = 0x12345u;
    for (size_t i = 0; i < (size_t) (sr * 0.05); ++i) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; l[i] = r[i] = 0.3f * ((float) (s >> 8) / 8388608.f - 1.f); }
    for (size_t at = 0; at < n; at += 480) { float* ch[2] = { l.data() + at, r.data() + at }; c.process(ch, 2, (int) std::min<size_t>(480, n - at)); }
    auto band = [&](double lo, double hi) {
        double e = 0.0;
        for (double f = lo; f <= hi; f *= 1.06) { const double a = amplitude(l, std::round(f / 2.0) * 2.0, sr, (size_t) (sr * 0.25), (size_t) (sr * 0.5)); e += a * a; }
        return 10.0 * std::log10(std::max(e, 1e-30));
    };
    return band(8000.0, 16000.0) - band(1000.0, 2000.0);
}

void reverbDamping()
{
    { dcdsp::HallReverbParams p; p.mix = 1.f; p.decayS = 3.f; p.predelayMs = 0.f; p.erLevel = 0.f;
      auto at = [&](float k) { p.dampingKHz = k; return tailTilt<dcdsp::HallReverbCore>(p); };
      const double t12 = at(12.f), t16 = at(16.f);
      check(t16 > t12 + 1.0, "hallreverb dampingKHz 16 is brighter than 12 (tail tilt " + f2(t16) + " vs " + f2(t12) + " dB)"); }
    { dcdsp::GatedReverbParams p; p.mix = 1.f; p.thresholdDb = -80.f;
      auto at = [&](float k) { p.dampingKHz = k; return tailTilt<dcdsp::GatedReverbCore>(p); };
      const double t12 = at(12.f), t16 = at(16.f);
      check(t16 > t12 + 0.5, "gatedreverb dampingKHz 16 is brighter than 12 (" + f2(t16) + " vs " + f2(t12) + " dB)"); }
    { dcdsp::PlateReverbParams p; p.mix = 1.f; p.decay = 0.6f;
      auto at = [&](float d) { p.damping = d; return tailTilt<dcdsp::PlateReverbCore>(p); };
      // damping 0 asks for 16 kHz, damping 0.254 for 12 kHz: one corner before.
      const double t12 = at(1.f - 11750.f / 15750.f), t16 = at(0.f);
      check(t16 > t12 + 0.5, "platereverb damping 0 (16 kHz) is brighter than 12 kHz (" + f2(t16) + " vs " + f2(t12) + " dB)"); }

    // The FDN stays contractive at the worst corner it can now reach: 16 kHz
    // damping at 44.1 kHz, the smallest geometry, the 60 s RT60 ceiling.
    dcdsp::revdetail::Fdn8 fdn; fdn.prepare(44100.0); fdn.setGeometry(1.0f); fdn.setDecaySeconds(60.f); fdn.setDampingHz(16000.f);
    double early = 0.0, late = 0.0;
    const int n = 44100 * 30;
    for (int i = 0; i < n; ++i)
    {
        float l = 0.f, r = 0.f;
        fdn.process(i == 0 ? 1.f : 0.f, l, r);
        const double e = (double) l * l + (double) r * r;
        if (i >= 44100 && i < 2 * 44100) early += e;
        if (i >= n - 44100) late += e;
    }
    const double drop = 10.0 * std::log10(std::max(late, 1e-300) / std::max(early, 1e-300));
    check(std::isfinite(drop) && drop < -20.0,
          "Fdn8 at 16 kHz damping / 44.1 kHz / RT60 60 s still decays (last second " + f2(drop) + " dB re second 2)");
}

void convAndTapeCuts()
{
    // convreverb with a unit IR: the wet path IS its high-cut low-pass.
    auto conv = [](float khz) {
        dcdsp::ConvReverbCore c; dcdsp::ConvReverbParams p;
        p.mix = 1.f; p.lowCutHz = 20.f; p.highCutKHz = khz; p.predelayMs = 0.f;
        c.prepare(48000.0, 480, 2); c.setParams(p);
        const float unit[1] = { 1.f };
        c.setIr(unit, 1, 48000.0);
        return sineGainDb(c, 14000.0, 48000.0);
    };
    const double c16 = conv(16.f), c12 = conv(12.f);
    const double want = rbjLowpassDb(16000.0, 0.707, 14000.0, 48000.0) - rbjLowpassDb(12000.0, 0.707, 14000.0, 48000.0);
    check(std::fabs((c16 - c12) - want) < 0.3,
          "convreverb high cut 16 vs 12 kHz at 14 kHz: " + f2(c16 - c12) + " dB apart, RBJ says " + f2(want));

    auto tape = [](float khz) {
        dcdsp::TapeSatCore t; dcdsp::TapeSatParams p;
        p.drive = 0.f; p.bias = 0.5f; p.hiss = 0.f; p.mix = 1.f; p.hfRolloffKHz = khz;
        t.setParams(p); t.prepare(48000.0, 480, 2); t.setParams(p);
        return sineGainDb(t, 14000.0, 48000.0, 0.01f);
    };
    const double t16 = tape(16.f), t12 = tape(12.f);
    const double twant = 2.0 * want;   // two cascaded sections
    check(std::fabs((t16 - t12) - twant) < 0.5,
          "tapesat roll-off 16 vs 12 kHz at 14 kHz: " + f2(t16 - t12) + " dB apart, two RBJ sections say " + f2(twant));
}

// ============================================================ 2. micpos

double micGainDb(int mic, float angle, double hz, float distanceCm = 15.f)
{
    dcdsp::MicPosCore c; dcdsp::MicPosParams p;
    p.mic = mic; p.position = 0.f; p.distanceCm = distanceCm; p.room = 0.f; p.blend2 = 0.f; p.level = 1.f / 1.3f; p.angleDeg = angle;
    c.setParams(p); c.prepare(48000.0, 480, 2); c.setParams(p);
    return sineGainDb(c, hz, 48000.0, 0.05f);
}

void tubeMicCurve()
{
    // Neumann U 67, cardioid, published frequency response (U 67 operating
    // manual, "Specifications U 67" — the vector chart, re 1 kHz). At 15 cm
    // the proximity shelf (150 Hz) is half in, so only 1 kHz and up is the
    // capsule alone.
    struct Pt { double hz, db; };
    const Pt u67[] = { { 2000, 0.01 }, { 4000, 0.07 }, { 6300, 0.43 }, { 8000, 1.03 }, { 9000, 1.35 },
                       { 10000, 1.41 }, { 11000, 1.17 }, { 12500, 0.21 }, { 14000, -1.65 }, { 16000, -4.70 } };
    const double ref = micGainDb(4, 0.f, 1000.0);
    double worst = 0.0; std::string at;
    for (const Pt& p : u67)
    {
        const double d = (micGainDb(4, 0.f, p.hz) - ref) - p.db;
        if (std::fabs(d) > std::fabs(worst)) { worst = d; at = f2(p.hz); }
    }
    check(std::fabs(worst) < 0.6, "tube mic holds the U 67 cardioid chart 2-16 kHz within 0.6 dB (worst "
                                  + f2(worst) + " dB at " + at + " Hz)");
    // Its low end sits under the small condenser's (the U 67 is -2.2 dB at
    // 100 Hz far-field; the small condenser curve is flat there).
    const double tubeLow = micGainDb(4, 0.f, 63.0) - ref;
    const double condLow = micGainDb(3, 0.f, 63.0) - micGainDb(3, 0.f, 1000.0);
    check(tubeLow < condLow - 2.0, "tube mic is softer at 63 Hz than the small condenser (" + f2(tubeLow) + " vs " + f2(condLow) + " dB re 1 kHz)");
    // ...and its air falls away where the small condenser's rises.
    const double tubeAir = micGainDb(4, 0.f, 16000.0) - ref;
    const double condAir = micGainDb(3, 0.f, 16000.0) - micGainDb(3, 0.f, 1000.0);
    check(tubeAir < condAir - 4.0, "tube mic has less 16 kHz air than the small condenser (" + f2(tubeAir) + " vs " + f2(condAir) + " dB)");
}

void offAxisAngle()
{
    struct Pt { double hz, db; };
    // Shure SM57 user guide 27A12820, typical polar patterns at 45 degrees.
    const Pt sm57[] = { { 1000, -1.2 }, { 2000, -1.8 }, { 4000, -0.6 }, { 8000, -2.4 } };
    double worst = 0.0;
    for (const Pt& p : sm57)
        worst = std::max(worst, std::fabs((micGainDb(0, 45.f, p.hz) - micGainDb(0, 0.f, p.hz)) - p.db));
    check(worst < 0.8, "57 at 45 degrees holds the SM57 polar chart 1-8 kHz within 0.8 dB (worst " + f2(worst) + ")");
    // Neumann U 67 cardioid polar pattern at 45 degrees.
    const Pt u67[] = { { 1000, -1.7 }, { 2000, -1.9 }, { 4000, -1.9 }, { 8000, -1.4 }, { 16000, -8.9 } };
    worst = 0.0;
    for (const Pt& p : u67)
        worst = std::max(worst, std::fabs((micGainDb(4, 45.f, p.hz) - micGainDb(4, 0.f, p.hz)) - p.db));
    check(worst < 1.0, "tube mic at 45 degrees holds the U 67 polar chart 1-16 kHz within 1.0 dB (worst " + f2(worst) + ")");
    // The ribbon is a figure-8: cos 45 = -3.01 dB, flat in frequency.
    const double rib = micGainDb(1, 45.f, 1000.0) - micGainDb(1, 0.f, 1000.0);
    check(std::fabs(rib + 3.01) < 0.2, "ribbon at 45 degrees: " + f2(rib) + " dB, figure-8 says -3.01");
    // Off axis the PROXIMITY effect thins, because only the pressure-GRADIENT
    // part of a capsule makes it: angling a cardioid 57 costs more 100 Hz on
    // the grille (full proximity) than at 15 cm (half of it), while a
    // figure-8 ribbon — all gradient — loses the same either way.
    auto lfLoss = [](int mic, float cm) { return micGainDb(mic, 45.f, 100.0, cm) - micGainDb(mic, 0.f, 100.0, cm); };
    const double c0 = lfLoss(0, 0.f), c15 = lfLoss(0, 15.f), r0 = lfLoss(1, 0.f), r15 = lfLoss(1, 15.f);
    check(c0 < c15 - 0.15, "angled 57 loses more 100 Hz with full proximity (" + f2(c0) + " dB at 0 cm) than at 15 cm (" + f2(c15) + ")");
    check(std::fabs(r0 - r15) < 0.05, "angled ribbon (figure-8) loses the same 100 Hz at 0 and 15 cm (" + f2(r0) + " / " + f2(r15) + ")");
    // Angle 0 runs no part of the stage.
    check(micGainDb(0, 0.f, 3000.0) == micGainDb(0, 0.f, 3000.0), "angle 0 is deterministic");
}

// ============================================================ 3. the cab bank

uint64_t fnvRows(int from, int to)
{
    uint64_t h = 1469598103934665603ull;
    for (int m = from; m < to; ++m)
        for (int i = 0; i < dcdsp::cabir::kIrLen; ++i)
        {
            uint32_t bits; std::memcpy(&bits, &dcdsp::cabir::kIr48k[m][i], 4);
            for (int k = 0; k < 4; ++k) { h ^= (bits >> (8 * k)) & 0xFFu; h *= 1099511628211ull; }
        }
    return h;
}

double irBandDb(int model, double lo, double hi)
{
    // Mean dB over [lo, hi] of the IR's magnitude (direct DFT on a log grid).
    double acc = 0.0; int n = 0;
    for (double f = lo; f <= hi * 1.0001; f *= 1.03)
    {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * kPi * f / dcdsp::cabir::kIrRate;
        for (int i = 0; i < dcdsp::cabir::kIrLen; ++i) { re += dcdsp::cabir::kIr48k[model][i] * std::cos(w * i); im -= dcdsp::cabir::kIr48k[model][i] * std::sin(w * i); }
        acc += 10.0 * std::log10(re * re + im * im + 1e-30); ++n;
    }
    return acc / n;
}

void cabBank()
{
    using namespace dcdsp::cabir;
    check(kNumModels == 26, "the bank has 26 models (0-11 + the 14 layouts of #74)");
    // Models 0-11 are the bank every persisted preset was voiced on: their taps
    // are pinned bit for bit (FNV-1a over the float bits, measured 2026-10-04
    // from the pre-#74 header).
    const uint64_t legacy = fnvRows(0, 12);
    char hex[32]; std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long) legacy);
    check(legacy == 0x75450654a657824eull, std::string("models 0-11 are the pre-#74 taps bit for bit (FNV ") + hex + ")");
    double bankMean = 0.0;
    for (int m = 0; m < 12; ++m) bankMean += irBandDb(m, 200.0, 4000.0);
    bankMean /= 12.0;
    for (int m = 12; m < kNumModels; ++m)
    {
        // The bounds are the legacy bank's own: its 8x10 holds 97.4 % of its
        // energy in 256 taps and 99.99 % in 1024; a vented 4th-order box at
        // 64 Hz rings a little longer, so 1024 taps is held to 99.5 %.
        int peak = 0; double e = 0.0, front = 0.0, front1k = 0.0;
        for (int i = 0; i < kIrLen; ++i)
        {
            const double v = kIr48k[m][i];
            if (std::fabs(v) > std::fabs(kIr48k[m][peak])) peak = i;
            e += v * v; if (i < 256) front += v * v; if (i < 1024) front1k += v * v;
        }
        const double lvl = irBandDb(m, 200.0, 4000.0) - bankMean;
        check(peak <= 8 && front / e > 0.97 && front1k / e > 0.995 && std::fabs(lvl) < 1.0,
              "model " + std::to_string(m) + ": minimum-phase front (peak at tap " + std::to_string(peak)
              + ", " + f2(100.0 * front / e) + " % in 256 taps, " + f2(100.0 * front1k / e)
              + " % in 1024), level " + f2(lvl) + " dB re the bank");
    }
    auto rel = [](int m, double lo, double hi) { return irBandDb(m, lo, hi) - irBandDb(m, 300.0, 3000.0); };
    // Jensen datasheets: the 15-inch bass driver (Fs 47 Hz) in its box reaches
    // lower than the 10-inch (Fs 55 Hz, a vented box tuned F3 = 68 Hz).
    for (int fifteen : { 23, 24, 25 })
        for (int ten : { 18, 19, 20 })
            check(rel(fifteen, 40.0, 50.0) > rel(ten, 40.0, 50.0) + 3.0,
                  "15-inch bass model " + std::to_string(fifteen) + " reaches lower than 10-inch model "
                  + std::to_string(ten) + " at 40-50 Hz (" + f2(rel(fifteen, 40.0, 50.0)) + " vs " + f2(rel(ten, 40.0, 50.0)) + " dB)");
    // Jensen P15N vs P10R curves: the 15-inch guitar speaker is the darker one
    // above 3 kHz.
    check(rel(14, 3000.0, 6000.0) < rel(12, 3000.0, 6000.0) - 6.0 && rel(14, 3000.0, 6000.0) < rel(13, 3000.0, 6000.0) - 6.0,
          "2x15 Guitar is darker at 3-6 kHz than the 10-inch P10R models (" + f2(rel(14, 3000.0, 6000.0)) + " vs "
          + f2(rel(12, 3000.0, 6000.0)) + " / " + f2(rel(13, 3000.0, 6000.0)) + " dB)");
    // The 2-way PA box keeps a horn's top; no close-miked speaker does.
    for (int m : { 12, 13, 14, 18, 21, 23 })
        check(rel(15, 8000.0, 12000.0) > rel(m, 8000.0, 12000.0) + 6.0,
              "PA 15 + Horn keeps 8-12 kHz far above model " + std::to_string(m) + " (" + f2(rel(15, 8000.0, 12000.0))
              + " vs " + f2(rel(m, 8000.0, 12000.0)) + " dB)");
    // KEF R7 Meta: +/-3 dB 48 Hz-28 kHz.
    double hifiWorst = 0.0;
    for (double f = 60.0; f <= 16000.0; f *= 1.12) hifiWorst = std::max(hifiWorst, std::fabs(rel(17, f, f)));
    check(hifiWorst < 3.0, "Hi-Fi Tower within +/-3 dB from 60 Hz to 16 kHz (worst " + f2(hifiWorst) + " dB)");
    // MOD 5-30: the 5-inch full-range column keeps its top where a guitar 10 falls.
    check(rel(16, 5000.0, 10000.0) > rel(13, 5000.0, 10000.0) + 6.0,
          "PA Column (5-inch full range) keeps 5-10 kHz above the 1x10 combo (" + f2(rel(16, 5000.0, 10000.0)) + " vs "
          + f2(rel(13, 5000.0, 10000.0)) + " dB)");
}

} // namespace

int main()
{
    std::printf("-- filter corners above a quarter of the rate (#87)\n");
    primitivesOptIn(); svfCorner(); graphicEqTopBand(); reverbDamping(); convAndTapeCuts();
    std::printf("-- micpos: tube condenser + off-axis angle (#75)\n");
    tubeMicCurve(); offAxisAngle();
    std::printf("-- cab bank (#74)\n");
    cabBank();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
