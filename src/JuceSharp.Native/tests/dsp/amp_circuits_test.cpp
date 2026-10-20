// juce_sharp_amp_circuits_test — the documented-circuit amp voicings and the
// amp / bass-head switches of DotCharter #70 / #72, against their SOURCES:
//
//  * dcdsp_tonestack.h — the Yeh & Smith (DAFx-06) closed form, checked against
//    an INDEPENDENT complex nodal solve of the same netlist (two derivations
//    that must agree to rounding), the bilinear discretisation against the
//    analog response, stability over the knob grid, and Kuehnel's published
//    first-order estimates for the 5F6-A and the Marshall 1987 (Amp Books,
//    "Fender Bassman Tone Stack");
//  * AmpCore circuit voicings 24-26 — the stack is really in the path (a noon
//    scoop the legacy shelf rows do not have), the bright cap (a gain-dependent
//    treble lift that vanishes at full gain), resonance (bounded low shelf,
//    skipped at 0) and that process() allocates nothing;
//  * BassAmpCore — Ultra Hi / Ultra Lo at the figures Ampeg publishes for its
//    SVT-CL, the sweepable mid at every position of that head's published
//    midrange selector, and the octave-shifted mid bells.
//
// Device-free: the cores are driven directly with sine probes small enough
// that every waveshaper is in its linear region, so a gain RATIO between two
// settings of the same core is the filter's response.
#include "dsp/cores/dcdsp_tonestack.h"
#include "dsp/cores/dcdsp_amp.h"
#include "dsp/cores/dcdsp_bassamp.h"

#include <cmath>
#include <complex>
#include <cstdio>
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

using cplx = std::complex<double>;
static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- nodal solve

// The TMB netlist of Yeh & Smith Fig. 1, solved by plain nodal analysis at one
// frequency — written from the schematic, NOT from eq. (1):
//   C1 Vi-T, (1-t)R1 T-O, tR1 O-B, R4 Vi-N, C2 N-B, C3 N-W,
//   lR2 B-X, (1-m)R3 X-W, mR3 W-ground; the output is O, unloaded.
static cplx nodalTmb(const dcdsp::TmbComponents& k, double t, double m, double l, double hz) {
    enum { T, O, B, N, W, X, kN };
    cplx Y[kN][kN] = {};
    cplx I[kN] = {};
    const cplx s(0.0, 2.0 * kPi * hz);
    auto R = [](double r) { return cplx(1.0 / std::max(r, 1e-9), 0.0); };
    auto between = [&](int a, int b, cplx y) {
        Y[a][a] += y; Y[b][b] += y; Y[a][b] -= y; Y[b][a] -= y;
    };
    auto fromSource = [&](int a, cplx y) { Y[a][a] += y; I[a] += y; };   // Vi = 1
    auto toGround   = [&](int a, cplx y) { Y[a][a] += y; };
    fromSource(T, s * k.C1);
    between(T, O, R((1.0 - t) * k.R1));
    between(O, B, R(t * k.R1));
    fromSource(N, R(k.R4));
    between(N, B, s * k.C2);
    between(N, W, s * k.C3);
    between(B, X, R(l * k.R2));
    between(X, W, R((1.0 - m) * k.R3));
    toGround(W, R(m * k.R3));
    // Gaussian elimination with partial pivoting.
    for (int c = 0; c < kN; ++c) {
        int piv = c;
        for (int r = c + 1; r < kN; ++r) if (std::abs(Y[r][c]) > std::abs(Y[piv][c])) piv = r;
        if (piv != c) { for (int j = 0; j < kN; ++j) std::swap(Y[c][j], Y[piv][j]); std::swap(I[c], I[piv]); }
        for (int r = c + 1; r < kN; ++r) {
            const cplx f = Y[r][c] / Y[c][c];
            for (int j = c; j < kN; ++j) Y[r][j] -= f * Y[c][j];
            I[r] -= f * I[c];
        }
    }
    cplx v[kN];
    for (int r = kN - 1; r >= 0; --r) {
        cplx acc = I[r];
        for (int j = r + 1; j < kN; ++j) acc -= Y[r][j] * v[j];
        v[r] = acc / Y[r][r];
    }
    return v[O];
}

static double db(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }

static double digitalMag(const double B[4], const double A[4], double hz, double fs) {
    const cplx z1 = std::exp(cplx(0.0, -2.0 * kPi * hz / fs));
    const cplx z2 = z1 * z1, z3 = z2 * z1;
    return std::abs((B[0] + B[1] * z1 + B[2] * z2 + B[3] * z3) / (A[0] + A[1] * z1 + A[2] * z2 + A[3] * z3));
}

static void closedFormMatchesTheNetlist() {
    double worst = 0.0;
    const double knobs[] = { 0.02, 0.25, 0.5, 0.75, 0.98 };
    const double freqs[] = { 20, 55, 120, 330, 800, 2000, 5000, 12000 };
    for (const auto& k : dcdsp::kTmbStacks)
        for (double t : knobs) for (double m : knobs) for (double l : knobs)
            for (double hz : freqs) {
                const auto h = dcdsp::tmbAnalogFractions(k, t, m, l);
                worst = std::max(worst, std::fabs(db(dcdsp::tmbMagnitude(h, hz)) - db(std::abs(nodalTmb(k, t, m, l, hz)))));
            }
    std::printf("     closed form vs nodal solve: worst %.3g dB\n", worst);
    check(worst < 1e-6, "Yeh & Smith eq. (1) equals a nodal solve of the Fig. 1 netlist (both stacks, 125 knob sets)");
}

static void bilinearTracksTheAnalogStack() {
    const double fs = 192000.0;   // the amp's 4x oversampled rate at 48 kHz
    double worst = 0.0;
    bool stable = true;
    const double knobs[] = { 0.0, 0.3, 0.5, 0.8, 1.0 };
    for (const auto& k : dcdsp::kTmbStacks)
        for (double t : knobs) for (double m : knobs) for (double l : knobs) {
            const auto h = dcdsp::tmbAnalog(k, t, m, l);
            double B[4], A[4];
            stable &= dcdsp::bilinear3(h, fs, B, A);
            for (double hz = 20.0; hz <= 10000.0; hz *= 1.25)
                worst = std::max(worst, std::fabs(db(digitalMag(B, A, hz, fs)) - db(dcdsp::tmbMagnitude(h, hz))));
            // Impulse response must decay: a passive network has every pole in
            // the left half plane, and the bilinear map keeps them inside |z|<1.
            dcdsp::ThirdOrderD f; f.setCoeffs(B, A);
            double tail = 0.0;
            for (int n = 0; n < (int) (fs * 3.0); ++n) {
                const double y = f.processD(n == 0 ? 1.0 : 0.0);
                if (n > (int) (fs * 2.0)) tail += y * y;
                if (!std::isfinite(y)) { stable = false; break; }
            }
            stable &= tail < 1e-18;
        }
    std::printf("     bilinear vs analog, 20 Hz - 10 kHz at 192 kHz: worst %.3g dB\n", worst);
    check(worst < 0.1, "the discretised stack tracks H(s) within 0.1 dB to 10 kHz (bilinear warping)");
    check(stable, "the discretised stack is stable at every corner of the knob grid");
}

/// Kuehnel's first-order estimates (Amp Books, "Fender Bassman Tone Stack"):
/// treble max / mid min: the treble band starts at 2.5 kHz on the 5F6-A and
/// "cuts this in half to 1.25kHz" on the 1987; bass max / mid min: the bass
/// band's upper break is "142Hz" on the 5F6-A and "219Hz" on the 1987. The exact
/// network must land within 30 % of each and keep the order.
static void kuehnelsEstimatesHold() {
    auto trebleCorner = [](const dcdsp::TmbComponents& k) {
        const auto h = dcdsp::tmbAnalogFractions(k, 1.0, 0.0, 0.0);
        const double hi = db(dcdsp::tmbMagnitude(h, 15000.0));
        for (double hz = 100.0; hz < 15000.0; hz *= 1.002)
            if (db(dcdsp::tmbMagnitude(h, hz)) >= hi - 3.0) return hz;
        return 15000.0;
    };
    auto bassBreak = [](const dcdsp::TmbComponents& k) {
        const auto h = dcdsp::tmbAnalogFractions(k, 0.0, 0.0, 1.0);
        double pk = -1e9, fpk = 10.0;
        for (double hz = 10.0; hz < 1000.0; hz *= 1.002) {
            const double v = db(dcdsp::tmbMagnitude(h, hz));
            if (v > pk) { pk = v; fpk = hz; }
        }
        for (double hz = fpk; hz < 5000.0; hz *= 1.002)
            if (db(dcdsp::tmbMagnitude(h, hz)) < pk - 3.0) return hz;
        return 5000.0;
    };
    const auto& fender = dcdsp::kTmbStacks[0];
    const auto& marshall = dcdsp::kTmbStacks[1];
    const double tf = trebleCorner(fender), tm = trebleCorner(marshall);
    const double bf = bassBreak(fender), bm = bassBreak(marshall);
    std::printf("     treble corner 5F6-A %.0f Hz (Kuehnel 2500), 1987 %.0f Hz (1250); bass break %.0f (142), %.0f (219)\n",
                tf, tm, bf, bm);
    auto near = [](double v, double ref) { return v > ref * 0.7 && v < ref * 1.3; };
    check(near(tf, 2500.0) && near(tm, 1250.0) && tm < tf,
          "treble band: 5F6-A ~2.5 kHz, the 1987's doubled C1 halves it (Kuehnel)");
    check(near(bf, 142.0) && near(bm, 219.0) && bm > bf,
          "bass band upper break: 5F6-A ~142 Hz, 1987 ~219 Hz (Kuehnel)");
}

static void tapersAreTheirSources() {
    check(std::fabs(dcdsp::potFraction(0.5, dcdsp::PotTaper::Audio) - 0.10) < 1e-12 &&
          dcdsp::potFraction(0.0, dcdsp::PotTaper::Audio) == 0.0 &&
          std::fabs(dcdsp::potFraction(1.0, dcdsp::PotTaper::Audio) - 1.0) < 1e-12 &&
          dcdsp::potFraction(0.37, dcdsp::PotTaper::Linear) == 0.37,
          "audio taper is 10 % at half rotation (Kuehnel), linear is linear");
    check(dcdsp::potFraction(std::nan(""), dcdsp::PotTaper::Audio) == 0.0,
          "a NaN knob is a pot at zero, not a NaN coefficient");
}

// ---------------------------------------------------------------- core probes

/// RMS gain (dB) of `Core` for a small sine at `hz`, after a settle.
template <typename Core, typename Params>
static double gainAt(const Params& p, double hz, double sr = 48000.0, float amp = 1e-3f) {
    Core core;
    core.setParams(p);
    core.prepare(sr, 512, 1);
    core.setParams(p);
    const int settle = (int) (sr * 0.6), measure = (int) (sr * 0.3);
    std::vector<float> buf(512);
    double in2 = 0.0, out2 = 0.0;
    long long n = 0;
    while (n < settle + measure) {
        for (int i = 0; i < 512; ++i)
            buf[(size_t) i] = amp * (float) std::sin(2.0 * kPi * hz * (double) (n + i) / sr);
        std::vector<float> in = buf;
        float* ch = buf.data();
        core.process(&ch, 1, 512);
        for (int i = 0; i < 512; ++i)
            if (n + i >= settle) { in2 += (double) in[(size_t) i] * in[(size_t) i]; out2 += (double) buf[(size_t) i] * buf[(size_t) i]; }
        n += 512;
    }
    return 10.0 * std::log10(out2 / std::max(in2, 1e-30));
}

static double ampGain(int voicing, float gain, float bass, float mid, float treble, double hz,
                      int bright = 0, float reso = 0.f) {
    dcdsp::AmpParams p;
    p.voicing = voicing; p.gain = gain; p.bass = bass; p.mid = mid; p.treble = treble;
    p.presence = 0.f; p.master = 0.5f; p.bright = bright; p.resonance = reso;
    return gainAt<dcdsp::AmpCore>(p, hz);
}

static void circuitVoicingsCarryTheStack() {
    // At noon a passive TMB stack scoops the mids (eq. (1): -7.8 dB at ~650 Hz
    // against -3.2 dB at 100 Hz for the 1987). Each circuit row is its family
    // row with the stack swapped in, so the DIFFERENCE between the two at noon
    // is the stack against the family's shelf trio: it must carry the scoop.
    const int pairs[3][2] = { { 24, 8 }, { 25, 1 }, { 26, 10 } };
    for (const auto& pr : pairs) {
        auto d = [&](double hz) {
            return ampGain(pr[0], 0.0f, .5f, .5f, .5f, hz) - ampGain(pr[1], 0.0f, .5f, .5f, .5f, hz);
        };
        const double lo = d(100.0), mi = d(700.0), hi = d(3000.0);
        std::printf("     voicing %d minus family row %d at noon: 100 Hz %.1f, 700 Hz %.1f, 3 kHz %.1f dB\n",
                    pr[0], pr[1], lo, mi, hi);
        check(mi < lo - 3.0 && mi < hi - 3.0, "a circuit voicing scoops the mids at noon against its shelf-stack family row");
    }
    {
        // The probe is not vacuous: two legacy rows differ by no such scoop.
        auto d = [&](double hz) { return ampGain(10, 0.0f, .5f, .5f, .5f, hz) - ampGain(1, 0.0f, .5f, .5f, .5f, hz); };
        const double lo = d(100.0), mi = d(700.0), hi = d(3000.0);
        std::printf("     legacy row 10 minus row 1 at noon: 100 Hz %.1f, 700 Hz %.1f, 3 kHz %.1f dB\n", lo, mi, hi);
        check(!(mi < lo - 3.0 && mi < hi - 3.0), "two shelf-stack rows show no such scoop (the probe is not vacuous)");
    }
    // The knobs move the circuit, in the circuit's direction.
    const double t0 = ampGain(24, 0.0f, .5f, .5f, 0.f, 3000.0) - ampGain(24, 0.0f, .5f, .5f, 0.f, 300.0);
    const double t1 = ampGain(24, 0.0f, .5f, .5f, 1.f, 3000.0) - ampGain(24, 0.0f, .5f, .5f, 1.f, 300.0);
    check(t1 > t0 + 6.0, "Treble 0 -> 1 lifts 3 kHz over 300 Hz by more than 6 dB on the circuit stack");
    const double b0 = ampGain(25, 0.0f, 0.f, .5f, .5f, 80.0) - ampGain(25, 0.0f, 0.f, .5f, .5f, 1000.0);
    const double b1 = ampGain(25, 0.0f, 1.f, .5f, .5f, 80.0) - ampGain(25, 0.0f, 1.f, .5f, .5f, 1000.0);
    check(b1 > b0 + 6.0, "Bass 0 -> 1 lifts 80 Hz over 1 kHz by more than 6 dB on the circuit stack");
}

static void brightCapFollowsTheVolumePot() {
    const double offLo = ampGain(24, 0.3f, .5f, .5f, .5f, 200.0, 0);
    const double onLo  = ampGain(24, 0.3f, .5f, .5f, .5f, 200.0, 1);
    const double offHi = ampGain(24, 0.3f, .5f, .5f, .5f, 6000.0, 0);
    const double onHi  = ampGain(24, 0.3f, .5f, .5f, .5f, 6000.0, 1);
    std::printf("     bright at gain 0.3: 200 Hz %+.1f dB, 6 kHz %+.1f dB\n", onLo - offLo, onHi - offHi);
    check(onHi - offHi > 6.0 && std::fabs(onLo - offLo) < 1.5,
          "Bright lifts the treble, not the lows, at a low volume setting");
    const double offFull = ampGain(24, 1.0f, .5f, .5f, .5f, 6000.0, 0, 0.f);
    const double onFull  = ampGain(24, 1.0f, .5f, .5f, .5f, 6000.0, 1, 0.f);
    check(std::fabs(onFull - offFull) < 1e-9, "at full volume the bright cap is shorted out: no change at all");
}

static void resonanceIsABoundedLowShelf() {
    const double base80 = ampGain(25, 0.0f, .5f, .5f, .5f, 80.0, 0, 0.f);
    const double base1k = ampGain(25, 0.0f, .5f, .5f, .5f, 1000.0, 0, 0.f);
    const double res80  = ampGain(25, 0.0f, .5f, .5f, .5f, 80.0, 0, 1.f);
    const double res1k  = ampGain(25, 0.0f, .5f, .5f, .5f, 1000.0, 0, 1.f);
    const double lift = (res80 - base80) - (res1k - base1k);
    std::printf("     resonance 1: 80 Hz %+.1f dB relative to 1 kHz\n", lift);
    check(lift > 4.0 && lift < dcdsp::PowerAmpResonance::kMaxDb + 0.5,
          "Resonance lifts the low end by up to the shared poweramp shelf's maximum");
}

static void ampProcessAllocatesNothing() {
#ifdef _MSC_VER
    dcdsp::AmpCore core;
    dcdsp::AmpParams p; p.voicing = 26; p.bright = 1; p.resonance = 0.7f; p.gain = 0.4f;
    core.prepare(48000.0, 512, 2);
    std::vector<float> l(512, 0.01f), r(512, 0.01f);
    float* ch[2] = { l.data(), r.data() };
    armed = true; allocations = 0;
    for (int b = 0; b < 200; ++b) {
        p.bass = (float) (b % 10) / 10.f; p.treble = 1.f - p.bass; p.voicing = 24 + b % 3; p.bright = b & 1;
        core.setParams(p);
        core.process(ch, 2, 512);
    }
    armed = false;
    check(allocations == 0, "amp setParams + process (circuit voicings, bright, resonance) allocate nothing");
#endif
}

// ---------------------------------------------------------------- bass head

static double bassGain(const dcdsp::BassAmpParams& p, double hz) {
    return gainAt<dcdsp::BassAmpCore>(p, hz);
}

static dcdsp::BassAmpParams cleanHead() {
    dcdsp::BassAmpParams p;
    p.model = 1; p.gain = 0.f; p.drive = 0.f; p.compress = 0.f; p.master = 0.6f;
    return p;
}

// The values the shifts must produce: Ampeg's SVT-CL owner's manual gives Ultra
// Lo as "+2 dB at 40 Hz and -10 dB cut at 500 Hz" and Ultra Hi as "+9 dB at
// 8 kHz"; the core runs each as an RBJ peaking bell at Q 0.7 (Ampeg publishes no
// bandwidth). The two Ultra Lo bells overlap, so they sum to +1.84 dB at 40 Hz
// and -9.97 dB at 500 Hz — computed from the band definitions offline, so the
// probe checks the CORE against them.
static void shiftsAreAmpegsPublishedFigures() {
    auto off = cleanHead();
    auto hi = off; hi.highShift = 1;
    auto lo = off; lo.lowShift = 1;
    const double uh8k = bassGain(hi, 8000.0) - bassGain(off, 8000.0);
    const double ul40 = bassGain(lo, 40.0) - bassGain(off, 40.0);
    const double ul500 = bassGain(lo, 500.0) - bassGain(off, 500.0);
    std::printf("     Ultra Hi at 8 kHz %+.2f dB; Ultra Lo at 40 Hz %+.2f, at 500 Hz %+.2f dB\n", uh8k, ul40, ul500);
    check(std::fabs(uh8k - 9.0) < 0.2, "Ultra Hi: Ampeg's +9 dB at 8 kHz");
    check(std::fabs(ul40 - 1.84) < 0.2 && std::fabs(ul500 + 9.97) < 0.2,
          "Ultra Lo: Ampeg's +2 dB at 40 Hz and -10 dB at 500 Hz (+1.84 / -9.97 dB as overlapping bells)");
}

static void midSelectorPeaksAtEveryPosition() {
    // Ampeg's published SVT-CL midrange selector (220 Hz .. 3 kHz), each at a
    // different Q of our choosing so the sweep is exercised across bandwidths.
    const double hz[] = { 220.0, 450.0, 800.0, 1600.0, 3000.0 };
    const double q[]  = { 0.5, 0.9, 0.7, 1.4, 1.0 };
    bool ok = true;
    for (int i = 0; i < 5; ++i) {
        const double f0 = hz[i];
        auto flat = cleanHead();
        auto boost = flat; boost.mid = 10.f; boost.midHz = (float) f0; boost.midQ = (float) q[i];
        double best = -1e9, at = 0.0;
        for (double f = f0 / 2.0; f <= f0 * 2.0; f *= 1.03) {
            const double d = bassGain(boost, f) - bassGain(flat, f);
            if (d > best) { best = d; at = f; }
        }
        std::printf("     mid +10 dB at %.0f Hz Q %.3f: peak %+.1f dB at %.0f Hz\n", f0, q[i], best, at);
        ok &= std::fabs(at / f0 - 1.0) < 0.07 && std::fabs(best - 10.0) < 0.6;
    }
    check(ok, "the sweepable mid peaks at each selector frequency with its full boost");
    // ...and its Q is the param's: a Q 0.5 bell is wider than a Q 1.4 one.
    auto flat = cleanHead();
    auto wide = flat; wide.mid = 10.f; wide.midHz = 1000.f; wide.midQ = 0.5f;
    auto narrow = wide; narrow.midQ = 1.4f;
    const double w = bassGain(wide, 2000.0) - bassGain(flat, 2000.0);
    const double n = bassGain(narrow, 2000.0) - bassGain(flat, 2000.0);
    check(w > n + 2.0, "midQ widens and narrows the sweepable bell");
}

static void midBellsMoveByOctaves() {
    auto flat = cleanHead();
    auto bell = flat; bell.lowMid = 12.f;
    auto down = bell; down.lowMidOct = -1.f;
    auto peakOf = [&](const dcdsp::BassAmpParams& p) {
        double best = -1e9, at = 0.0;
        for (double hz = 60.0; hz <= 1000.0; hz *= 1.03) {
            const double d = bassGain(p, hz) - bassGain(flat, hz);
            if (d > best) { best = d; at = hz; }
        }
        return at;
    };
    const double centre = peakOf(bell), moved = peakOf(down);
    std::printf("     low-mid bell peak %.0f Hz, one octave down %.0f Hz\n", centre, moved);
    check(std::fabs(moved / centre - 0.5) < 0.05, "lowMidOct -1 moves the low-mid bell one octave down");
}

static void bassDefaultsAreTheOldPath() {
    // At their defaults the new controls are SKIPPED, so two heads that differ
    // only in a parked new control render identically (a parked mid band at a
    // different centre, a zero octave shift).
    auto a = cleanHead();
    auto b = a; b.midHz = 3000.f;
    check(bassGain(a, 700.0) == bassGain(b, 700.0), "a parked mid band is out of the path whatever its frequency");
}

static void bassProcessAllocatesNothing() {
#ifdef _MSC_VER
    dcdsp::BassAmpCore core;
    auto p = cleanHead();
    core.prepare(48000.0, 512, 2);
    std::vector<float> l(512, 0.01f), r(512, 0.01f);
    float* ch[2] = { l.data(), r.data() };
    armed = true; allocations = 0;
    for (int b = 0; b < 200; ++b) {
        p.lowShift = b & 1; p.highShift = (b >> 1) & 1; p.mid = (float) (b % 7) - 3.f;
        p.midHz = 200.f + 10.f * (float) b; p.lowMidOct = (float) (b % 5) - 2.f; p.model = b % 4;
        core.setParams(p);
        core.process(ch, 2, 512);
    }
    armed = false;
    check(allocations == 0, "bassamp setParams + process (mid, shifts, octaves, model swaps) allocate nothing");
#endif
}

int main() {
#ifdef _MSC_VER
    _CrtSetAllocHook(allocHook);
#endif
    closedFormMatchesTheNetlist();
    bilinearTracksTheAnalogStack();
    kuehnelsEstimatesHold();
    tapersAreTheirSources();
    circuitVoicingsCarryTheStack();
    brightCapFollowsTheVolumePot();
    resonanceIsABoundedLowShelf();
    ampProcessAllocatesNothing();
    shiftsAreAmpegsPublishedFigures();
    midSelectorPeaksAtEveryPosition();
    midBellsMoveByOctaves();
    bassDefaultsAreTheOldPath();
    bassProcessAllocatesNothing();
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
