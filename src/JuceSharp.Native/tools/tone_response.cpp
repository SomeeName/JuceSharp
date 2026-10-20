// tone_response.cpp — offline, device-independent magnitude-response probe for
// the dcdsp tone rig. Answers "which stage removes the low end" with numbers.
//
// Build: an EXCLUDE_FROM_ALL CMake target since W4-D63 — a normal build and CI
// do not pay for it, but the compiler sees the file whenever somebody asks,
// which is what stops the next hand-transcribed copy of a core table.
//   cmake --build build --config Release --target juce_sharp_tone_response
// ...or still by hand, from JuceSharp/src/JuceSharp.Native:
//   cl /std:c++17 /O2 /EHsc /I src tools\tone_response.cpp /Fe:tone_response.exe
//
// METHOD. Every stage here is non-linear, so "the frequency response" only
// exists as a DESCRIBING FUNCTION: drive a settled sine of amplitude A at an
// integer frequency f, then read the output component AT f with a single-bin
// DFT over exactly one second (48000 samples at 48 kHz => 1 Hz bins, so an
// integer f is leakage-free and every harmonic lands in a different bin).
// The result is the gain the FUNDAMENTAL sees at that level — exactly the
// quantity a player calls "how much low end does it have".
//
// The chain is tapped at EVERY stage boundary in ONE pass, so the per-stage
// deltas are a true bisection, not a set of separately-measured runs.
//
// REFERENCE. Levels are quoted against the log-mean of 200 Hz..4 kHz — the same
// band `dcdsp_cab_irs.h` normalises its IR bank over. A single-frequency
// reference (1 kHz) is useless here because several cab IRs have a notch there.
//
// Modes:
//   chain   [voicing] [cabmodel] [inDbFS]   amp -> cab, tapped
//   amp     [voicing] [inDbFS]              amp alone, tapped by filter block
//   sweep   [inDbFS]                        every amp voicing, one row each
//   cabsweep                                every cab model, one row each
//   cabir   [model]                         bundled IR alone vs. catalogue cuts
//   filters [voicing]                       the amp's LTI cascade, per filter
//   preset  <id>                            a real factory chain, tapped
//   face    [neutral]                       namamp's amp face, ABSOLUTE dB
//                                           ("neutral" = the bit-identity proof)
//   src     [hostRate]                      namamp's rate converter, round trip
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dsp/cores/dcdsp_amp.h"
#include "dsp/cores/dcdsp_ampface.h"
#include "dsp/cores/dcdsp_detail_src.h"
#include "dsp/cores/dcdsp_cab.h"
#include "dsp/cores/dcdsp_compressor.h"
#include "dsp/cores/dcdsp_distortion.h"
#include "dsp/cores/dcdsp_eq.h"
#include "dsp/cores/dcdsp_gate.h"
#include "dsp/cores/dcdsp_overdrive.h"

namespace {

constexpr double kSr = 48000.0;
constexpr int    kBlock = 512;
constexpr double kPi = 3.14159265358979323846;

// ISO third-octave centres plus the four guitar fundamentals that matter,
// rounded to integers so each is an exact bin of the one-second window:
//   62 = B1 (7-string), 73 = D2 (drop D), 82 = E2 (low E), 110 = A2.
const int kFreqs[] = { 20, 25, 31, 40, 50, 62, 63, 73, 80, 82, 100, 110, 125,
                       160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250,
                       1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000,
                       12500, 16000 };
constexpr int kNumFreqs = (int) (sizeof(kFreqs) / sizeof(kFreqs[0]));

// Single-bin DFT accumulator (rectangular window, exact-bin frequency).
struct BinMeter {
    double re = 0.0, im = 0.0;
    long long n = 0;
    double w = 0.0;
    void arm(double freq) { re = im = 0.0; n = 0; w = 2.0 * kPi * freq / kSr; }
    void push(const float* x, int count) {
        for (int i = 0; i < count; ++i) {
            const double ph = w * (double) (n + i);
            re += (double) x[i] * std::cos(ph);
            im -= (double) x[i] * std::sin(ph);
        }
        n += count;
    }
    double amplitude() const {
        return n <= 0 ? 0.0 : 2.0 * std::sqrt(re * re + im * im) / (double) n;
    }
};

double db(double lin) { return lin <= 1e-12 ? -240.0 : 20.0 * std::log10(lin); }

// Log-mean of the row over 200 Hz..4 kHz — the IR bank's own normalisation band
// and the only notch-immune reference available here.
double bandRef(const std::vector<double>& row) {
    double sum = 0.0; int n = 0;
    for (int i = 0; i < kNumFreqs; ++i)
        if (kFreqs[i] >= 200 && kFreqs[i] <= 4000) { sum += row[(size_t) i]; ++n; }
    return n ? sum / n : 0.0;
}

struct Stage {
    std::string name;
    std::function<void(float*, int)> run;
};

void measure(std::vector<Stage>& stages,
             const std::function<void()>& resetAll,
             double inAmp,
             std::vector<std::vector<double>>& rows)
{
    const int taps = (int) stages.size();
    rows.assign((size_t) taps, std::vector<double>((size_t) kNumFreqs, -240.0));

    const int settleFrames = (int) (kSr * 1.0);   // sag release is 220 ms
    const int measFrames   = (int) kSr;           // exactly 1 s => 1 Hz bins

    std::vector<float> buf((size_t) kBlock);
    std::vector<BinMeter> meters((size_t) taps);

    for (int fi = 0; fi < kNumFreqs; ++fi) {
        const double f = (double) kFreqs[fi];
        const double w = 2.0 * kPi * f / kSr;
        resetAll();
        for (auto& m : meters) m.arm(f);

        long long phase = 0;
        auto runBlock = [&](int count, bool measuring) {
            for (int i = 0; i < count; ++i)
                buf[(size_t) i] = (float) (inAmp * std::sin(w * (double) (phase + i)));
            phase += count;
            for (int s = 0; s < taps; ++s) {
                stages[(size_t) s].run(buf.data(), count);
                if (measuring) meters[(size_t) s].push(buf.data(), count);
            }
        };

        for (int done = 0; done < settleFrames; done += kBlock)
            runBlock(std::min(kBlock, settleFrames - done), false);
        for (auto& m : meters) m.arm(f);
        for (int done = 0; done < measFrames; done += kBlock)
            runBlock(std::min(kBlock, measFrames - done), true);

        for (int s = 0; s < taps; ++s)
            rows[(size_t) s][(size_t) fi] = db(meters[(size_t) s].amplitude() / inAmp);
    }
}

void header(const char* title) {
    std::printf("%-28s", title);
    for (int i = 0; i < kNumFreqs; ++i) std::printf("%8d", kFreqs[i]);
    std::printf("\n");
}

void printTable(const std::vector<Stage>& stages,
                const std::vector<std::vector<double>>& rows)
{
    header("stage (dB re 200-4k mean)");
    for (size_t s = 0; s < stages.size(); ++s) {
        const double ref = bandRef(rows[s]);
        std::printf("%-28s", stages[s].name.c_str());
        for (int i = 0; i < kNumFreqs; ++i) std::printf("%8.2f", rows[s][(size_t) i] - ref);
        std::printf("\n");
    }
    std::printf("\n");
    header("per-stage delta (band-ref'd)");
    for (size_t s = 0; s < stages.size(); ++s) {
        const double ref = bandRef(rows[s]);
        const double pref = (s == 0) ? 0.0 : bandRef(rows[s - 1]);
        std::printf("%-28s", stages[s].name.c_str());
        for (int i = 0; i < kNumFreqs; ++i) {
            const double cur  = rows[s][(size_t) i] - ref;
            const double prev = (s == 0) ? 0.0 : rows[s - 1][(size_t) i] - pref;
            std::printf("%8.2f", cur - prev);
        }
        std::printf("\n");
    }
    std::printf("\n");
}

// Compact one-line summary: the guitar fundamentals against the midband.
void printSummary(const char* label, const std::vector<double>& row) {
    auto at = [&](int f) {
        for (int i = 0; i < kNumFreqs; ++i) if (kFreqs[i] == f) return row[(size_t) i];
        return -240.0;
    };
    const double ref = bandRef(row);
    std::printf("%-30s  B1(62)%7.2f  C2(65~63)%7.2f  D2(73)%7.2f  E2(82)%7.2f  A2(110)%7.2f  |  40Hz%7.2f\n",
                label, at(62) - ref, at(63) - ref, at(73) - ref, at(82) - ref,
                at(110) - ref, at(40) - ref);
}

void runMono(const std::function<void(float* const*, int, int)>& fn, float* x, int n) {
    float* chans[1] = { x };
    fn(chans, 1, n);
}

// ── power-chord alias / intermodulation metric ────────────────────────────────
// The 0.16 W6 overhaul is defended by a number: drive a POWER CHORD, whose every
// legitimate harmonic and intermodulation product lands on the gcd grid, then
// measure the energy that landed OFF that grid. That residue is aliasing plus
// high-order IMD — the "fizz". Any change to the drive path has to report it.
//
// N = 16384 at 48 kHz => 2.9297 Hz bins. f1 = 28 bins (82.03 Hz, ~E2), f2 = 42
// bins (123.05 Hz, ~B2); the grid is 14 bins (41.02 Hz), so every legitimate
// product is an exact bin and the rectangular window leaks nothing.
constexpr int kFftN    = 16384;
constexpr int kGridBin = 14;

void fftRadix2(std::vector<double>& re, std::vector<double>& im) {
    const int n = (int) re.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[(size_t) i], re[(size_t) j]); std::swap(im[(size_t) i], im[(size_t) j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; ++k) {
                const double wr = std::cos(ang * k), wi = std::sin(ang * k);
                const double ur = re[(size_t)(i + k)],          ui = im[(size_t)(i + k)];
                const double vr = re[(size_t)(i + k + len / 2)] * wr - im[(size_t)(i + k + len / 2)] * wi;
                const double vi = re[(size_t)(i + k + len / 2)] * wi + im[(size_t)(i + k + len / 2)] * wr;
                re[(size_t)(i + k)] = ur + vr;              im[(size_t)(i + k)] = ui + vi;
                re[(size_t)(i + k + len / 2)] = ur - vr;    im[(size_t)(i + k + len / 2)] = ui - vi;
            }
        }
    }
}

void measureAlias(const char* label, std::vector<Stage>& stages,
                  const std::function<void()>& resetAll, double peakAmp)
{
    resetAll();
    const double w1 = 2.0 * kPi * (kGridBin * 2.0) * (kSr / kFftN) / kSr;
    const double w2 = 2.0 * kPi * (kGridBin * 3.0) * (kSr / kFftN) / kSr;
    std::vector<float> buf((size_t) kBlock);
    std::vector<double> re, im;
    re.reserve(kFftN); im.reserve(kFftN);

    long long phase = 0;
    auto run = [&](int count, bool capture) {
        for (int i = 0; i < count; ++i) {
            const double t = (double) (phase + i);
            buf[(size_t) i] = (float) (0.5 * peakAmp * (std::sin(w1 * t) + std::sin(w2 * t)));
        }
        phase += count;
        for (auto& s : stages) s.run(buf.data(), count);
        if (capture) for (int i = 0; i < count && (int) re.size() < kFftN; ++i) {
            re.push_back(buf[(size_t) i]); im.push_back(0.0);
        }
    };
    for (int d = 0; d < (int) kSr; d += kBlock) run(std::min(kBlock, (int) kSr - d), false);
    while ((int) re.size() < kFftN) run(kBlock, true);
    re.resize(kFftN); im.resize(kFftN);

    fftRadix2(re, im);
    double onGrid = 0.0, offGrid = 0.0, worst = 0.0, fund = 0.0;
    int worstBin = 0;
    for (int b = 1; b < kFftN / 2; ++b) {
        const double p = re[(size_t) b] * re[(size_t) b] + im[(size_t) b] * im[(size_t) b];
        if (b == kGridBin * 2 || b == kGridBin * 3) fund += p;
        if (b % kGridBin == 0) onGrid += p;
        else { offGrid += p; if (p > worst) { worst = p; worstBin = b; } }
    }
    std::printf("%-30s  total off-grid %7.1f dBc   worst %7.1f dBc @ %6.0f Hz   (on-grid ref %.1f dB)\n",
                label, 10.0 * std::log10((offGrid + 1e-30) / (onGrid + 1e-30)),
                10.0 * std::log10((worst + 1e-30) / (fund + 1e-30)),
                worstBin * kSr / kFftN, 10.0 * std::log10(onGrid + 1e-30));
}

// ── LF-relevant slice of AmpCore's voicing table ──────────────────────────────
// READ FROM THE CORE, not transcribed (W4-D63). This used to be a hand-copied
// `voicingLf(24)` whose own comment said "a mismatch means this table drifted from
// the header" — a second source of truth for 24 rows of numbers, with nothing
// but a printed cross-check to notice when it rotted. `AmpCore::voicingLfForTest`
// returns the real row now, so `filters` mode cannot disagree with `amp` mode.
//
// The NAMES stay here, and only the names: they live in `//` comments on each
// row of `kVoicings` and are not data the core carries.
using VoicingLf = dcdsp::AmpCore::VoicingLfInfo;

const char* voicingName(int i)
{
    static const char* kNames[] = {
        "Blackface Twin 85", "Plexi 50", "Five-Oh-One Lead", "AC-Thirty",
        "Class-A Boutique 30", "Bass Three-Sixty", "Bass Flip-Top", "SVT Bass 300W",
        "Acoustic Pre", "Studio Direct", "Voicing 10", "Voicing 11",
        "Voicing 12", "Voicing 13", "Voicing 14", "Voicing 15",
        "Voicing 16", "Voicing 17", "Voicing 18", "Voicing 19",
        "Voicing 20", "Voicing 21", "Voicing 22", "Voicing 23",
    };
    const int n = (int) (sizeof(kNames) / sizeof(kNames[0]));
    return (i >= 0 && i < n) ? kNames[i] : "?";
}

VoicingLf voicingLf(int i) { return dcdsp::AmpCore::voicingLfForTest(i); }

const int kNumVoicings = dcdsp::AmpCore::numVoicingsForTest();

} // namespace

// ── namamp amp face (0.36) ───────────────────────────────────────────────────
// The face is LTI, so ABSOLUTE dB is the honest number here (printTable's
// 200 Hz-4 kHz band reference would hide a broad shelf's own level). Same
// single-bin DFT instrument, no reference subtraction.
namespace {

void faceRow(const char* label, dcdsp::AmpFace& face, double inAmp)
{
    std::printf("%-22s", label);
    std::vector<float> buf((size_t) kBlock);
    for (int fi = 0; fi < kNumFreqs; ++fi) {
        const double f = (double) kFreqs[fi];
        const double w = 2.0 * kPi * f / kSr;
        face.reset();
        BinMeter m; m.arm(f);
        long long phase = 0;
        auto run = [&](int count, bool measuring) {
            for (int i = 0; i < count; ++i)
                buf[(size_t) i] = (float) (inAmp * std::sin(w * (double) (phase + i)));
            phase += count;
            face.processPost(buf.data(), count);
            if (measuring) m.push(buf.data(), count);
        };
        const int settle = (int) (kSr * 0.25), meas = (int) kSr;
        for (int d = 0; d < settle; d += kBlock) run(std::min(kBlock, settle - d), false);
        m.arm(f);
        for (int d = 0; d < meas; d += kBlock) run(std::min(kBlock, meas - d), true);
        std::printf("%8.2f", db(m.amplitude() / inAmp));
    }
    std::printf("\n");
}

/// Bit-identity proof: neutral params must leave every sample untouched.
int faceNeutralCheck()
{
    dcdsp::AmpFace face;
    face.prepare(kSr, kBlock, 1);
    face.setParams(dcdsp::AmpFaceParams{});   // all defaults
    std::printf("neutral: preGain=%.17g  isNeutral=%d\n", (double) face.preGain(),
                face.isNeutral() ? 1 : 0);

    unsigned s = 12345u;
    std::vector<float> a((size_t) kBlock), b((size_t) kBlock);
    long long diffs = 0; double worst = 0.0;
    for (int blk = 0; blk < 4000; ++blk) {
        for (int i = 0; i < kBlock; ++i) {
            s = s * 1664525u + 1013904223u;
            const float v = (float) ((double) (s >> 8) / 8388608.0 - 1.0);
            a[(size_t) i] = b[(size_t) i] = v;
        }
        face.processPost(b.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            if (a[(size_t) i] != b[(size_t) i]) {
                ++diffs;
                worst = std::max(worst, (double) std::fabs(a[(size_t) i] - b[(size_t) i]));
            }
        }
    }
    std::printf("neutral face over %d samples: %lld differing, worst |delta| = %.3g\n",
                4000 * kBlock, diffs, worst);
    return diffs == 0 ? 0 : 1;
}

/// Round-trip magnitude response + error of the rate converter pair, measured
/// at the HOST rate (what a 44.1 kHz user actually hears through namamp).
void srcSweep(double hostRate, double modelRate)
{
    const int hostBlock = 512;
    dcdsp::detail::SincResampler down, up;
    const int modelCap = (int) std::ceil((double) hostBlock * modelRate / hostRate) + 32;
    down.prepare(hostRate, modelRate, hostBlock);
    up.prepare(modelRate, hostRate, modelCap);

    std::printf("== SRC round trip %.0f -> %.0f -> %.0f Hz ==\n", hostRate, modelRate, hostRate);
    std::printf("latency: %d host + %d model frames (= %.2f ms round trip incl. 32-frame cushion)\n",
                down.latencyInSamples(), up.latencyInSamples(),
                1000.0 * (32.0 + down.latencyInSamples()
                          + up.latencyInSamples() * hostRate / modelRate) / hostRate);
    std::printf("%-22s", "freq (Hz)");
    for (int i = 0; i < kNumFreqs; ++i) std::printf("%8d", kFreqs[i]);
    std::printf("\n%-22s", "round-trip (dB)");

    std::vector<float> in((size_t) hostBlock), mid((size_t) modelCap), fifo((size_t) (4 * hostBlock + 128));
    for (int fi = 0; fi < kNumFreqs; ++fi) {
        const double f = (double) kFreqs[fi];
        if (f >= hostRate * 0.5) { std::printf("%8s", "-"); continue; }
        const double w = 2.0 * kPi * f / hostRate;
        down.reset(); up.reset();
        int fifoCount = 32;
        std::fill(fifo.begin(), fifo.end(), 0.0f);

        double re = 0.0, im = 0.0; long long n = 0;
        const int settle = (int) (hostRate * 0.25), meas = (int) hostRate;
        long long phase = 0;
        auto run = [&](int count, bool measuring) {
            for (int i = 0; i < count; ++i)
                in[(size_t) i] = (float) (0.25 * std::sin(w * (double) (phase + i)));
            phase += count;
            down.push(in.data(), count);
            int k = std::min(down.available(), modelCap);
            if (k > 0) { down.pull(mid.data(), k); up.push(mid.data(), k); }
            int h = std::min(up.available(), (int) fifo.size() - fifoCount);
            if (h > 0) { up.pull(fifo.data() + fifoCount, h); fifoCount += h; }
            const int take = std::min(count, fifoCount);
            for (int i = 0; i < count; ++i)
                in[(size_t) i] = i < take ? fifo[(size_t) i] : 0.0f;
            if (take > 0) {
                std::memmove(fifo.data(), fifo.data() + take,
                             sizeof(float) * (size_t) (fifoCount - take));
                fifoCount -= take;
            }
            if (!measuring) return;
            for (int i = 0; i < count; ++i) {
                const double ph = 2.0 * kPi * f * (double) (n + i) / hostRate;
                re += (double) in[(size_t) i] * std::cos(ph);
                im -= (double) in[(size_t) i] * std::sin(ph);
            }
            n += count;
        };
        for (int d = 0; d < settle; d += hostBlock) run(std::min(hostBlock, settle - d), false);
        re = im = 0.0; n = 0;
        for (int d = 0; d < meas; d += hostBlock) run(std::min(hostBlock, meas - d), true);
        const double amp = n <= 0 ? 0.0 : 2.0 * std::sqrt(re * re + im * im) / (double) n;
        std::printf("%8.2f", db(amp / 0.25));
    }
    std::printf("\n\n");
}

} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "chain";

    // ── namamp amp face + rate converter (0.36) ─────────────────────────────
    if (mode == "face") {
        const double inAmp = std::pow(10.0, -18.0 / 20.0);
        if (argc > 2 && std::string(argv[2]) == "neutral") return faceNeutralCheck();

        dcdsp::AmpFace face;
        face.prepare(kSr, kBlock, 1);
        std::printf("== namamp amp face, ABSOLUTE dB, input -18 dBFS ==\n");
        header("knob (absolute dB)");
        struct KnobCase { const char* label; dcdsp::AmpFaceParams p; };
        auto with = [](float dcdsp::AmpFaceParams::* field, float v) {
            dcdsp::AmpFaceParams p{}; p.*field = v; return p;
        };
        const KnobCase cases[] = {
            { "neutral (all 0.5)", dcdsp::AmpFaceParams{} },
            { "bass 0.0",      with(&dcdsp::AmpFaceParams::bass, 0.0f) },
            { "bass 1.0",      with(&dcdsp::AmpFaceParams::bass, 1.0f) },
            { "mid 0.0",       with(&dcdsp::AmpFaceParams::mid, 0.0f) },
            { "mid 1.0",       with(&dcdsp::AmpFaceParams::mid, 1.0f) },
            { "treble 0.0",    with(&dcdsp::AmpFaceParams::treble, 0.0f) },
            { "treble 1.0",    with(&dcdsp::AmpFaceParams::treble, 1.0f) },
            { "presence 0.0",  with(&dcdsp::AmpFaceParams::presence, 0.0f) },
            { "presence 1.0",  with(&dcdsp::AmpFaceParams::presence, 1.0f) },
            { "resonance 0.0", with(&dcdsp::AmpFaceParams::resonance, 0.0f) },
            { "resonance 1.0", with(&dcdsp::AmpFaceParams::resonance, 1.0f) },
            { "master 0.0",    with(&dcdsp::AmpFaceParams::master, 0.0f) },
            { "master 1.0",    with(&dcdsp::AmpFaceParams::master, 1.0f) },
        };
        for (const auto& c : cases) {
            face.setParams(c.p);
            faceRow(c.label, face, inAmp);
        }
        dcdsp::AmpFaceParams g{};
        std::printf("\npre-capture drive (linear gain into the model):\n");
        for (float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
            g.gain = v; face.setParams(g);
            std::printf("  gain %.2f -> %+6.2f dB\n", (double) v, 20.0 * std::log10((double) face.preGain()));
        }
        std::printf("\n");
        return 0;
    }

    if (mode == "src") {
        const double host = argc > 2 ? std::atof(argv[2]) : 44100.0;
        srcSweep(host, 48000.0);
        return 0;
    }

    // ── the amp's LTI cascade, per filter (no non-linearities) ───────────────
    if (mode == "filters") {
        const int v = argc > 2 ? std::atoi(argv[2]) : 1;
        const VoicingLf& lf = voicingLf(v);
        // The amp configures these inside its 4x region. A OnePole/BiquadD
        // coefficient depends only on fc/sampleRate, so configuring at the base
        // rate with the SAME fc reproduces the identical ANALOG corner while
        // letting this harness run one sample per input sample.
        const double os = kSr;

        struct Blk { std::string name; std::function<float(float)> f; std::function<void()> rst; };
        std::vector<Blk> blks;

        auto inHp = std::make_shared<dcdsp::Biquad>();
        inHp->setHighpass(kSr, 40.0, 0.707);
        blks.push_back({ "inHpf 40Hz bq (base)",
                         [inHp](float x){ return inHp->process(x); },
                         [inHp]{ inHp->reset(); } });

        // Same bounded shelf the core builds: cplCutDb split over the stages.
        const int nCpl = lf.stages + (lf.postDrive > 0.f ? 1 : 0);
        const float cplFloor = std::pow(10.f, -(lf.cplCutDb / (float) nCpl) / 20.f);
        for (int s = 0; s < lf.stages; ++s) {
            auto p = std::make_shared<dcdsp::OnePole>();
            p->setHighpassShelf(os, lf.cplHz[s], cplFloor);
            char nm[64]; std::snprintf(nm, sizeof nm, "cpl[%d] %.0fHz shelf", s, lf.cplHz[s]);
            blks.push_back({ nm, [p](float x){ return p->process(x); }, [p]{ p->reset(); } });
        }
        {
            auto d = std::make_shared<dcdsp::DcBlocker>();
            d->prepare(os, 12.0);
            blks.push_back({ "preDc 12Hz (4x)", [d](float x){ return d->process(x); }, [d]{ d->reset(); } });
        }
        {
            auto b = std::make_shared<dcdsp::BiquadD>();
            b->setLowShelf(os, lf.bassFc, 0.707, lf.bassOfsDb);
            char nm[64]; std::snprintf(nm, sizeof nm, "bass shelf %.0fHz %+.1fdB", lf.bassFc, lf.bassOfsDb);
            blks.push_back({ nm, [b](float x){ return (float) b->processD(x); }, [b]{ b->reset(); } });
            auto m = std::make_shared<dcdsp::BiquadD>();
            m->setPeaking(os, lf.midFc, lf.midQ, lf.midOfsDb);
            char nm2[64]; std::snprintf(nm2, sizeof nm2, "mid peak %.0fHz %+.1fdB", lf.midFc, lf.midOfsDb);
            blks.push_back({ nm2, [m](float x){ return (float) m->processD(x); }, [m]{ m->reset(); } });
        }
        if (lf.postDrive > 0.f) {
            auto p = std::make_shared<dcdsp::OnePole>();
            p->setHighpassShelf(os, lf.cplHz[2], cplFloor);
            char nm[64]; std::snprintf(nm, sizeof nm, "postCpl %.0fHz shelf", lf.cplHz[2]);
            blks.push_back({ nm, [p](float x){ return p->process(x); }, [p]{ p->reset(); } });
        }
        {
            auto p = std::make_shared<dcdsp::OnePole>();
            p->setHighpass(os, lf.otHpfHz);
            char nm[64]; std::snprintf(nm, sizeof nm, "otHpf %.0fHz 1p (4x)", lf.otHpfHz);
            blks.push_back({ nm, [p](float x){ return p->process(x); }, [p]{ p->reset(); } });
            auto d = std::make_shared<dcdsp::DcBlocker>();
            d->prepare(kSr, 15.0);
            blks.push_back({ "outDc 15Hz (base)", [d](float x){ return d->process(x); }, [d]{ d->reset(); } });
        }

        std::vector<Stage> stages;
        for (auto& b : blks) {
            auto fn = b.f;
            stages.push_back({ b.name, [fn](float* x, int n){ for (int i = 0; i < n; ++i) x[i] = fn(x[i]); } });
        }
        std::vector<std::vector<double>> rows;
        measure(stages, [&blks]{ for (auto& b : blks) b.rst(); }, 1e-3, rows);

        std::printf("== voicing %d (%s): the amp's LTI cascade, small-signal ==\n", v, voicingName(v));
        printTable(stages, rows);
        std::printf("cumulative through the cascade:\n");
        for (size_t s = 0; s < stages.size(); ++s) printSummary(stages[s].name.c_str(), rows[s]);

        // Cross-check against the real core at the same tiny level.
        auto amp = std::make_shared<dcdsp::AmpCore>();
        dcdsp::AmpParams ap; ap.voicing = v;
        amp->prepare(kSr, kBlock, 1);
        amp->setParams(ap);
        std::vector<Stage> real1;
        real1.push_back({ "REAL AmpCore @ -60 dBFS",
                          [amp](float* x, int n){ runMono([amp](float* const* c,int nc,int nn){ amp->process(c,nc,nn); }, x, n); } });
        std::vector<std::vector<double>> rr;
        measure(real1, [amp]{ amp->reset(); }, 1e-3, rr);
        std::printf("\ncross-check (should match the last cascade row within ~0.3 dB):\n");
        printSummary("REAL AmpCore @ -60 dBFS", rr[0]);
        return 0;
    }

    if (mode == "cabir") {
        const int model = argc > 2 ? std::atoi(argv[2]) : 0;
        auto mk = [&](float lo, float hi) {
            auto c = std::make_shared<dcdsp::CabCore>();
            dcdsp::CabParams p; p.model = model; p.mix = 1.f; p.lowCutHz = lo; p.highCutHz = hi;
            c->prepare(kSr, kBlock, 1); c->setParams(p);
            return c;
        };
        auto open = mk(40.f, 12000.f);
        auto def  = mk(60.f, 8000.f);
        std::vector<Stage> stages;
        stages.push_back({ "IR only (lowCut 40)",
                           [open](float* x,int n){ runMono([open](float* const* c,int nc,int nn){ open->process(c,nc,nn); }, x, n); } });
        std::vector<std::vector<double>> r1;
        measure(stages, [open]{ open->reset(); }, 0.12589254, r1);
        std::vector<Stage> stages2;
        stages2.push_back({ "IR + lowCut 60 (default)",
                            [def](float* x,int n){ runMono([def](float* const* c,int nc,int nn){ def->process(c,nc,nn); }, x, n); } });
        std::vector<std::vector<double>> r2;
        measure(stages2, [def]{ def->reset(); }, 0.12589254, r2);
        std::printf("== cab model %d ==\n", model);
        printTable(stages, r1);
        printTable(stages2, r2);
        printSummary("IR only (lowCut 40)", r1[0]);
        printSummary("IR + lowCut 60 (default)", r2[0]);
        return 0;
    }

    if (mode == "cabsweep") {
        std::printf("cab models, dB re 200Hz-4kHz log-mean, catalogue defaults (lowCut 60 / highCut 8000)\n");
        for (int m = 0; m < 12; ++m) {
            auto c = std::make_shared<dcdsp::CabCore>();
            dcdsp::CabParams p; p.model = m; p.mix = 1.f; p.lowCutHz = 60.f; p.highCutHz = 8000.f;
            c->prepare(kSr, kBlock, 1); c->setParams(p);
            std::vector<Stage> st;
            st.push_back({ "cab", [c](float* x,int n){ runMono([c](float* const* cc,int nc,int nn){ c->process(cc,nc,nn); }, x, n); } });
            std::vector<std::vector<double>> rows;
            measure(st, [c]{ c->reset(); }, 0.12589254, rows);
            char lbl[64]; std::snprintf(lbl, sizeof lbl, "cab model %d", m);
            printSummary(lbl, rows[0]);
        }
        return 0;
    }

    if (mode == "sweep") {
        const double inDb = argc > 2 ? std::atof(argv[2]) : -18.0;
        const double inAmp = std::pow(10.0, inDb / 20.0);
        std::printf("amp voicings at catalogue defaults, input %.1f dBFS, dB re 200Hz-4kHz log-mean\n", inDb);
        for (int v = 0; v < 24; ++v) {
            auto amp = std::make_shared<dcdsp::AmpCore>();
            dcdsp::AmpParams p; p.voicing = v;
            amp->prepare(kSr, kBlock, 1); amp->setParams(p);
            std::vector<Stage> st;
            st.push_back({ "amp", [amp](float* x,int n){ runMono([amp](float* const* c,int nc,int nn){ amp->process(c,nc,nn); }, x, n); } });
            std::vector<std::vector<double>> rows;
            measure(st, [amp]{ amp->reset(); }, inAmp, rows);
            char lbl[64]; std::snprintf(lbl, sizeof lbl, "%2d %s", v, voicingName(v));
            printSummary(lbl, rows[0]);
        }
        return 0;
    }

    // Power-chord alias / IMD residue for every voicing — the 0.16 W6 metric.
    if (mode == "alias") {
        const double inDb = argc > 2 ? std::atof(argv[2]) : -18.0;
        const double amp = std::pow(10.0, inDb / 20.0);
        std::printf("power-chord (E2+B2, 41 Hz grid) off-grid residue, input %.1f dBFS peak\n", inDb);
        for (int v = 0; v < 24; ++v) {
            auto a = std::make_shared<dcdsp::AmpCore>();
            dcdsp::AmpParams p; p.voicing = v; p.gain = 0.75f;   // driven, where it matters
            a->prepare(kSr, kBlock, 1); a->setParams(p);
            std::vector<Stage> st;
            st.push_back({ "amp", [a](float* x,int n){ runMono([a](float* const* c,int nc,int nn){ a->process(c,nc,nn); }, x, n); } });
            char lbl[64]; std::snprintf(lbl, sizeof lbl, "%2d %s", v, voicingName(v));
            measureAlias(lbl, st, [a]{ a->reset(); }, amp);
        }
        return 0;
    }

    // The two drive PEDALS, at the drive the factory presets actually use.
    if (mode == "drive") {
        std::printf("drive pedals, dB re 200Hz-4kHz log-mean, input -18 dBFS\n");
        for (int m = 0; m < 4; ++m) {
            auto od = std::make_shared<dcdsp::OverdriveCore>();
            dcdsp::OverdriveParams op; op.model = m; op.drive = 0.15f; op.level = 0.85f; op.tone = 0.65f;
            od->prepare(kSr, kBlock, 1); od->setParams(op);
            std::vector<Stage> st;
            st.push_back({ "od", [od](float* x,int n){ runMono([od](float* const* c,int nc,int nn){ od->process(c,nc,nn); }, x, n); } });
            std::vector<std::vector<double>> rows;
            measure(st, [od]{ od->reset(); }, 0.12589254, rows);
            char lbl[64]; std::snprintf(lbl, sizeof lbl, "overdrive model %d (drive .15)", m);
            printSummary(lbl, rows[0]);
        }
        for (int m = 0; m < 4; ++m) {
            auto ds = std::make_shared<dcdsp::DistortionCore>();
            dcdsp::DistortionParams dp; dp.model = m; dp.gain = 0.6f; dp.level = 0.6f;
            ds->prepare(kSr, kBlock, 1); ds->setParams(dp);
            std::vector<Stage> st;
            st.push_back({ "dist", [ds](float* x,int n){ runMono([ds](float* const* c,int nc,int nn){ ds->process(c,nc,nn); }, x, n); } });
            std::vector<std::vector<double>> rows;
            measure(st, [ds]{ ds->reset(); }, 0.12589254, rows);
            char lbl[64]; std::snprintf(lbl, sizeof lbl, "distortion model %d (tight 90)", m);
            printSummary(lbl, rows[0]);
        }
        return 0;
    }

    if (mode == "amp") {
        const int v = argc > 2 ? std::atoi(argv[2]) : 1;
        const double inDb = argc > 3 ? std::atof(argv[3]) : -18.0;
        const double inAmp = std::pow(10.0, inDb / 20.0);
        auto amp = std::make_shared<dcdsp::AmpCore>();
        dcdsp::AmpParams p; p.voicing = v;
        amp->prepare(kSr, kBlock, 1); amp->setParams(p);
        std::vector<Stage> st;
        st.push_back({ "amp", [amp](float* x,int n){ runMono([amp](float* const* c,int nc,int nn){ amp->process(c,nc,nn); }, x, n); } });
        std::vector<std::vector<double>> rows;
        measure(st, [amp]{ amp->reset(); }, inAmp, rows);
        std::printf("== amp voicing %d (%s), input %.1f dBFS ==\n", v, voicingName(v), inDb);
        printTable(st, rows);
        printSummary("amp", rows[0]);
        return 0;
    }

    // ── preset chains (verbatim knob values from DspTonePresetLibrary) ───────
    if (mode == "preset") {
        const std::string id = argc > 2 ? argv[2] : "metal-5150-rhythm";

        auto ampC = std::make_shared<dcdsp::AmpCore>();
        auto cabC = std::make_shared<dcdsp::CabCore>();
        auto odC  = std::make_shared<dcdsp::OverdriveCore>();
        auto eqC  = std::make_shared<dcdsp::EqCore>();
        dcdsp::AmpParams ap; dcdsp::CabParams cp; dcdsp::OverdriveParams op; dcdsp::EqParams ep;
        bool useOd = false, useEq = false;

        if (id == "metal-5150-rhythm") {
            useOd = true; op.model = 1; op.drive = 0.15f; op.tone = 0.65f; op.level = 0.85f;
            ap.gain = 0.72f; ap.bass = 0.55f; ap.mid = 0.45f; ap.treble = 0.6f;
            ap.presence = 0.5f; ap.master = 0.46f; ap.voicing = 2;
            cp.model = 8; cp.lowCutHz = 100.f; cp.highCutHz = 8500.f;
            useEq = true; ep.midFreq = 700.f; ep.midGainDb = -2.f; ep.highFreq = 4500.f; ep.highGainDb = 2.f;
        } else if (id == "clean-blackface-sparkle") {
            ap.gain = 0.3f; ap.bass = 0.55f; ap.mid = 0.4f; ap.treble = 0.6f;
            ap.presence = 0.35f; ap.master = 0.6f; ap.voicing = 0;
            cp.model = 0;
        } else if (id == "crunch-vox-top-boost") {
            useOd = true; op.drive = 0.4f; op.tone = 0.6f; op.level = 0.7f;
            ap.gain = 0.55f; ap.bass = 0.45f; ap.mid = 0.55f; ap.treble = 0.65f;
            ap.presence = 0.5f; ap.master = 0.48f; ap.voicing = 1;
            cp.model = 1;
            useEq = true; ep.highGainDb = 2.f;
        } else if (id == "metal-modern-djent") {
            useOd = true; op.model = 1; op.drive = 0.15f; op.tone = 0.7f; op.level = 0.85f;
            ap.gain = 0.68f; ap.bass = 0.45f; ap.mid = 0.5f; ap.treble = 0.6f;
            ap.presence = 0.55f; ap.master = 0.48f; ap.voicing = 15;
            cp.model = 8; cp.lowCutHz = 110.f; cp.highCutHz = 9000.f;
            useEq = true; ep.lowFreq = 100.f; ep.lowGainDb = -2.f; ep.midFreq = 700.f;
            ep.midQ = 1.4f; ep.midGainDb = -3.f; ep.highFreq = 4500.f; ep.highGainDb = 2.f;
        } else {
            std::printf("unknown preset id '%s'\n", id.c_str());
            return 1;
        }

        odC->prepare(kSr, kBlock, 1); odC->setParams(op);
        ampC->prepare(kSr, kBlock, 1); ampC->setParams(ap);
        cabC->prepare(kSr, kBlock, 1); cabC->setParams(cp);
        eqC->prepare(kSr, kBlock, 1); eqC->setParams(ep);

        std::vector<Stage> st;
        if (useOd) st.push_back({ "1 overdrive", [odC](float* x,int n){ runMono([odC](float* const* c,int nc,int nn){ odC->process(c,nc,nn); }, x, n); } });
        st.push_back({ "2 + amp", [ampC](float* x,int n){ runMono([ampC](float* const* c,int nc,int nn){ ampC->process(c,nc,nn); }, x, n); } });
        st.push_back({ "3 + cab", [cabC](float* x,int n){ runMono([cabC](float* const* c,int nc,int nn){ cabC->process(c,nc,nn); }, x, n); } });
        if (useEq) st.push_back({ "4 + eq", [eqC](float* x,int n){ runMono([eqC](float* const* c,int nc,int nn){ eqC->process(c,nc,nn); }, x, n); } });

        std::vector<std::vector<double>> rows;
        measure(st, [odC,ampC,cabC,eqC]{ odC->reset(); ampC->reset(); cabC->reset(); eqC->reset(); },
                0.12589254, rows);
        std::printf("== preset '%s', input -18 dBFS ==\n", id.c_str());
        printTable(st, rows);
        for (size_t s = 0; s < st.size(); ++s) printSummary(st[s].name.c_str(), rows[s]);
        return 0;
    }

    // Default: amp -> cab, tapped.
    const int voicing  = argc > 2 ? std::atoi(argv[2]) : 1;
    const int cabModel = argc > 3 ? std::atoi(argv[3]) : 0;
    const double inDb  = argc > 4 ? std::atof(argv[4]) : -18.0;
    const double inAmp = std::pow(10.0, inDb / 20.0);

    auto amp = std::make_shared<dcdsp::AmpCore>();
    dcdsp::AmpParams ap; ap.voicing = voicing;
    amp->prepare(kSr, kBlock, 1); amp->setParams(ap);

    auto cab = std::make_shared<dcdsp::CabCore>();
    dcdsp::CabParams cp; cp.model = cabModel; cp.mix = 1.f;
    cp.lowCutHz = 60.f; cp.highCutHz = 8000.f;
    cab->prepare(kSr, kBlock, 1); cab->setParams(cp);

    std::vector<Stage> stages;
    stages.push_back({ "1 amp", [amp](float* x,int n){ runMono([amp](float* const* c,int nc,int nn){ amp->process(c,nc,nn); }, x, n); } });
    stages.push_back({ "2 + cab", [cab](float* x,int n){ runMono([cab](float* const* c,int nc,int nn){ cab->process(c,nc,nn); }, x, n); } });

    std::vector<std::vector<double>> rows;
    measure(stages, [amp,cab]{ amp->reset(); cab->reset(); }, inAmp, rows);

    std::printf("== amp voicing %d (%s) -> cab model %d, input %.1f dBFS ==\n",
                voicing, voicingName(voicing), cabModel, inDb);
    printTable(stages, rows);
    for (size_t s = 0; s < stages.size(); ++s) printSummary(stages[s].name.c_str(), rows[s]);
    return 0;
}
