// droptune_measure.cpp — objective measurements for the CSVD drop tuner.
//
// The drop tuner's problem was never latency, it was SOUND. This harness turns
// "does it sound good" into numbers, so a change can be judged without a guitar.
//
// Build (from JuceSharp/src/JuceSharp.Native):
//   cl /std:c++17 /O2 /EHsc /I src tools\droptune_measure.cpp /Fe:droptune_measure.exe
// or
//   g++ -std=c++17 -O2 -I src tools/droptune_measure.cpp -o droptune_measure
//
// What it measures, and why each one is the thing that was wrong before:
//
//  1. PITCH ACCURACY — the shift must actually be the shift.
//  2. SIDEBAND LEVEL (dBc) — the splices amplitude-modulate the signal, and that
//     modulation IS the warble. The old dual-head shifter combed the signal
//     CONTINUOUSLY (both heads live, 20 ms apart) and measures around -15 dBc here.
//     Target: < -40 dBc.
//  3. HF RESPONSE — reading the delay line at r < 1 is interpolation, so the
//     interpolator is the reconstruction filter. Lagrange/linear droops with rate,
//     which is why a shifted signal sounds dull. Measured as the level of a 5 kHz
//     tone through the shifter, relative to its level at unity.
//  4. ATTACK LATENCY — the transient anchor should put a pick out in ~3-4 ms.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/cores/dcdsp_droptune.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kSampleRate = 48000.0f;
constexpr float kPi_f = 3.14159265f;

std::vector<float> run(std::vector<float> mono, float semitones, float maxLagMs = 20.0f) {
    dcdsp::DropTune dt;
    dt.prepare(kSampleRate, 512, 2);
    dcdsp::DropTuneParams p;
    p.semitones = semitones;
    p.maxLagMs = maxLagMs;
    dt.setParams(p);

    std::vector<float> l = mono, r = mono;
    float* ptrs[2] = {l.data(), r.data()};

    // Feed it in blocks, like a real device callback.
    const int block = 128;
    for (size_t i = 0; i < mono.size(); i += block) {
        const int n = (int) std::min<size_t>(block, mono.size() - i);
        float* bp[2] = {l.data() + i, r.data() + i};
        dt.process(bp, 2, n);
    }
    (void) ptrs;
    return l;
}

/// Goertzel magnitude at freq (linear).
double magAt(const std::vector<float>& x, size_t from, size_t count, double freq) {
    const double w = 2.0 * kPi * freq / kSampleRate;
    const double cw = std::cos(w), sw = std::sin(w);
    const double coeff = 2.0 * cw;
    double s0 = 0, s1 = 0, s2 = 0;
    for (size_t i = 0; i < count; ++i) {
        // Hann to keep leakage from swamping a -40 dBc sideband.
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (count - 1));
        s0 = coeff * s1 - s2 + x[from + i] * win;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sw;
    return std::sqrt(re * re + im * im) / (count * 0.25);
}

double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

std::vector<float> tone(double freq, double seconds, double amp = 0.5) {
    const size_t n = (size_t) (seconds * kSampleRate);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = (float) (amp * std::sin(2.0 * kPi * freq * i / kSampleRate));
    return x;
}

/// Estimate f0 by parabolic-interpolated peak of a coarse Goertzel sweep.
double estimatePitch(const std::vector<float>& x, size_t from, size_t count, double around) {
    double best = around, bestMag = -1;
    for (double f = around * 0.9; f <= around * 1.1; f += 0.05) {
        const double m = magAt(x, from, count, f);
        if (m > bestMag) { bestMag = m; best = f; }
    }
    return best;
}

int failures = 0;

void check(bool ok, const char* what, const char* detail) {
    std::printf("  [%s] %-42s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) ++failures;
}

/// The OLD "time-domain" shifter, reimplemented here purely as a reference point:
/// two read heads 180 degrees apart sweeping a ring over a 40 ms window, both
/// permanently live, power-complementary sin/cos gains, linear interpolation.
///
/// Keeping it here is the only honest way to claim an improvement — it turns
/// "sounds better" into a number. Both heads being audible at once is a
/// continuously swept comb filter, and that is what this measures.
std::vector<float> runLegacyDualHead(std::vector<float> mono, float semitones) {
    const float rate = std::exp2(semitones / 12.0f);
    const int window = (int) (0.040f * kSampleRate);  // 40 ms sweep
    std::vector<float> ring((size_t) window * 2, 0.0f);
    const int mask = (int) ring.size() - 1;
    // ring size must be a power of two for the mask; round up
    int size = 1;
    while (size < window * 2) size <<= 1;
    ring.assign((size_t) size, 0.0f);
    const int m = size - 1;
    (void) mask;

    int w = 0;
    float phase = 0.0f;
    const float phaseInc = (1.0f - rate) / (float) window;

    std::vector<float> out(mono.size(), 0.0f);
    for (size_t i = 0; i < mono.size(); ++i) {
        ring[(size_t) (w & m)] = mono[i];
        ++w;

        auto readLin = [&](float delay) -> float {
            const float pos = (float) w - delay;
            const int i0 = (int) std::floor(pos);
            const float f = pos - (float) i0;
            const float a = ring[(size_t) (i0 & m)];
            const float b = ring[(size_t) ((i0 + 1) & m)];
            return a + f * (b - a);
        };

        const float p0 = phase;
        const float p1 = phase + 0.5f < 1.0f ? phase + 0.5f : phase - 0.5f;

        const float d0 = p0 * (float) window;
        const float d1 = p1 * (float) window;

        const float g0 = std::sin(kPi_f * p0);
        const float g1 = std::sin(kPi_f * p1);

        out[i] = g0 * readLin(d0 + 1.0f) + g1 * readLin(d1 + 1.0f);

        phase += phaseInc;
        if (phase >= 1.0f) phase -= 1.0f;
        if (phase < 0.0f) phase += 1.0f;
    }
    return out;
}

}  // namespace

int main() {
    std::printf("CSVD drop tuner — objective measurements @ %.0f Hz\n\n", kSampleRate);

    // ---- 1. pitch accuracy -------------------------------------------------
    std::printf("Pitch accuracy (target: within 5 cents)\n");
    for (int semis : {-1, -2, -3, -5, -7, -12}) {
        const double fIn = 220.0;
        const auto y = run(tone(fIn, 2.0), (float) semis);
        const double expected = fIn * std::pow(2.0, semis / 12.0);
        // Skip the first 0.5 s: engagement + first splices.
        const double got = estimatePitch(y, (size_t) (0.5 * kSampleRate), (size_t) (1.0 * kSampleRate), expected);
        const double cents = 1200.0 * std::log2(got / expected);

        char detail[128];
        std::snprintf(detail, sizeof detail, "%3d st: %.1f Hz (want %.1f) = %+.1f cents",
                      semis, got, expected, cents);
        check(std::abs(cents) < 5.0, "pitch", detail);
    }

    // ---- 2. sidebands: THE warble test -------------------------------------
    // The splices amplitude-modulate the tone at the splice rate,
    // f_splice = (1 - r) * fs / L. Sidebands sit at f_out +/- f_splice.
    std::printf("\nSideband level (the warble; target < -40 dBc)\n");
    for (int semis : {-2, -5, -12}) {
        const double fIn = 220.0;
        const double r = std::pow(2.0, semis / 12.0);
        const double fOut = fIn * r;
        const double lagSec = 0.020;  // Balanced
        const double fSplice = (1.0 - r) / lagSec;

        const auto y = run(tone(fIn, 3.0), (float) semis, 20.0f);
        const size_t from = (size_t) (1.0 * kSampleRate);
        const size_t count = (size_t) (1.5 * kSampleRate);

        const double carrier = magAt(y, from, count, fOut);
        const double lo = magAt(y, from, count, fOut - fSplice);
        const double hi = magAt(y, from, count, fOut + fSplice);
        const double worst = dB(std::max(lo, hi) / std::max(carrier, 1e-12));

        char detail[160];
        std::snprintf(detail, sizeof detail, "%3d st: splice %.1f Hz, worst sideband %.1f dBc",
                      semis, fSplice, worst);
        check(worst < -40.0, "sidebands", detail);
    }

    // ---- 3. HF response: the interpolator test ------------------------------
    // Lagrange/linear interpolation droops with rate, so the shifted signal sounds
    // dull. A 5 kHz tone shifted -12 lands at 2.5 kHz and must keep its level.
    std::printf("\nHF retention (interpolator; target > -1.0 dB)\n");
    for (int semis : {-5, -12}) {
        const double fIn = 5000.0;
        const double r = std::pow(2.0, semis / 12.0);
        const auto y = run(tone(fIn, 2.0), (float) semis);

        const size_t from = (size_t) (0.5 * kSampleRate);
        const size_t count = (size_t) (1.0 * kSampleRate);
        const double got = magAt(y, from, count, fIn * r);
        const double loss = dB(got / 0.5);  // input amplitude was 0.5

        char detail[128];
        std::snprintf(detail, sizeof detail, "%3d st: 5 kHz -> %.0f Hz at %.2f dB", semis, fIn * r, loss);
        check(loss > -1.0, "HF retention", detail);
    }

    // ---- 4. attack latency --------------------------------------------------
    // The transient anchor should snap the read head forward at the pick.
    std::printf("\nAttack latency (transient anchor; target < 8 ms)\n");
    {
        // 200 ms of quiet, then a sharp plucked burst.
        std::vector<float> x((size_t) (0.6 * kSampleRate), 0.0f);
        const size_t onset = (size_t) (0.2 * kSampleRate);
        for (size_t i = onset; i < x.size(); ++i) {
            const double t = (double) (i - onset) / kSampleRate;
            const double env = std::exp(-t * 12.0);
            x[i] = (float) (0.7 * env * std::sin(2.0 * kPi * 196.0 * (i - onset) / kSampleRate));
        }

        const auto y = run(x, -2.0f);

        // First sample after the onset whose |value| passes 10% of the input peak.
        float peak = 0;
        for (float v : x) peak = std::max(peak, std::abs(v));
        size_t outOnset = 0;
        for (size_t i = onset; i < y.size(); ++i) {
            if (std::abs(y[i]) > 0.10f * peak) { outOnset = i; break; }
        }
        const double latencyMs = outOnset > onset ? (outOnset - onset) * 1000.0 / kSampleRate : -1.0;

        char detail[128];
        std::snprintf(detail, sizeof detail, "pick emitted %.2f ms after it arrived", latencyMs);
        check(latencyMs >= 0.0 && latencyMs < 8.0, "attack latency", detail);
    }

    // ---- 5. A/B against the algorithm this replaced -------------------------
    // Not decoration: "sounds better" is worthless without a number, and the old
    // dual-head shifter is the thing the complaint was ABOUT.
    std::printf("\nA/B vs the old dual-head shifter (sidebands, lower = cleaner)\n");
    for (int semis : {-2, -5, -12}) {
        const double fIn = 220.0;
        const double r = std::pow(2.0, semis / 12.0);
        const double fOut = fIn * r;
        const size_t from = (size_t) (1.0 * kSampleRate);
        const size_t count = (size_t) (1.5 * kSampleRate);

        auto worstSideband = [&](const std::vector<float>& y, double fSplice) {
            const double carrier = magAt(y, from, count, fOut);
            const double lo = magAt(y, from, count, fOut - fSplice);
            const double hi = magAt(y, from, count, fOut + fSplice);
            return dB(std::max(lo, hi) / std::max(carrier, 1e-12));
        };

        // The old shifter's sweep completes every window/(1-r) samples, and BOTH heads
        // are audible, so the comb modulates at twice that rate.
        const double fLegacy = 2.0 * (1.0 - r) * kSampleRate / (0.040 * kSampleRate);
        const double legacy = worstSideband(runLegacyDualHead(tone(fIn, 3.0), (float) semis), fLegacy);

        const double fNew = (1.0 - r) / 0.020;
        const double now = worstSideband(run(tone(fIn, 3.0), (float) semis, 20.0f), fNew);

        std::printf("  %3d st:  old %6.1f dBc   ->   new %6.1f dBc   (%.0f dB cleaner)\n",
                    semis, legacy, now, legacy - now);
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
