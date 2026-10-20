// miku_measure.cpp — objective measurements for the `mikustomp` vocal core.
//
// The owner's complaint is three things: (a) low intelligibility, (b) some
// letters do not come out, (c) "a weird blob sound at the end". This harness
// turns all three into numbers.
//
// Build (from JuceSharp/src/JuceSharp.Native) — NOT in CMake, on purpose:
//   cl /std:c++17 /O2 /EHsc /I src tools\miku_measure.cpp /Fe:miku_measure.exe
//
// What it measures:
//   T0  ANTIRESONATOR RESPONSE — the nasal zero's magnitude response, computed
//       straight from the same coefficient recipe the core uses. A Klatt nasal
//       ZERO without its matching nasal POLE has |H| that RISES +12 dB/oct
//       forever. If this prints tens of dB of HF gain, every nasal is broken.
//   T1  THE TAIL ("blob") — render a phrase, hard-cut the input to digital
//       silence, then measure the output RMS envelope + spectrum AFTER the last
//       sample of input. Also polls syllableIndex()/trackedHz() the whole time,
//       so a sequencer that advances when nobody picked shows up as a timestamp.
//   T2  PHONEME SWEEP — every consonant in the table x /a/. Reports the acoustic
//       signature each phoneme class MUST have (closure gap / burst / frication
//       band / nasal murmur) and flags the ones that do not produce it.
//   T3  COARTICULATION — F1/F2/F3 tracked by LPC from the moment the voice turns
//       on. The consonant cue IS the formant transition (Delattre); if F2 at
//       voicing onset already equals F2 at the vowel steady state, the transition
//       is inaudible and the consonant is unidentifiable. That number is the
//       intelligibility proxy.
//   T4  VOWEL ACCURACY — measured F1/F2/F3 vs the core's own vowel table.
//   WAV dumps for a human to audition.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "dsp/cores/dcdsp_mikustomp.h"
#ifdef MIKU_WITH_V1
  #include "dsp/cores/dcdsp_miku.h"     // read-only: v1 is FROZEN, we only measure it
#endif

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;

using Core = dcdsp::MikuStompCore;
using Syl  = Core::Syllable;

// ---------------------------------------------------------------------------
// WAV out (16-bit PCM mono) so the owner can just double-click the file.
// ---------------------------------------------------------------------------
void writeWav(const std::string& path, const std::vector<float>& x, double sr) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) { std::printf("  !! cannot open %s\n", path.c_str()); return; }
    const uint32_t n = (uint32_t) x.size();
    const uint32_t dataBytes = n * 2u;
    const uint32_t rate = (uint32_t) sr;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36u + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16u); u16(1); u16(1);
    u32(rate); u32(rate * 2u); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    for (float v : x) {
        if (!std::isfinite(v)) v = 0.0f;
        v = std::max(-1.0f, std::min(1.0f, v));
        u16((uint16_t) (int16_t) std::lround(v * 32767.0f));
    }
    std::fclose(f);
    std::printf("  wav -> %s  (%.2f s)\n", path.c_str(), (double) n / sr);
}

// ---------------------------------------------------------------------------
// Synthetic guitar: a plucked string (harmonics with per-harmonic decay) plus a
// short broadband pick transient — the HF burst the onset detector keys on.
// ---------------------------------------------------------------------------
uint32_t g_rng = 0x1234567u;
float noise() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return ((float) (g_rng & 0xFFFFFFu) * (1.0f / 8388608.0f)) - 1.0f;
}

// Writes one pluck into buf starting at `at`, replacing (muting) whatever was
// there — one string, re-picked.
void pluck(std::vector<float>& buf, int at, double f0, double decaySec, float amp) {
    const int n = (int) buf.size();
    for (int i = at; i < n; ++i) buf[(size_t) i] = 0.0f;   // the pick mutes the string
    const int nh = std::min(16, (int) (0.45 * kSr / f0));
    for (int i = at; i < n; ++i) {
        const double t = (double) (i - at) / kSr;
        double s = 0.0;
        for (int h = 1; h <= nh; ++h) {
            const double dk = decaySec / std::sqrt((double) h);
            s += (1.0 / (double) h) * std::exp(-t / dk)
               * std::sin(2.0 * kPi * f0 * (double) h * t + 0.7 * (double) h);
        }
        // pick transient: 2.5 ms of decaying broadband noise
        const double pt = (t < 0.0025) ? std::exp(-t / 0.0008) * 0.8 : 0.0;
        buf[(size_t) i] = amp * (float) (0.55 * s + pt * (double) noise());
    }
}

// ---------------------------------------------------------------------------
// Core driver. Renders `frames` samples, polling syllableIndex()/trackedHz()
// every `poll` samples so a spurious sequencer advance is visible in time.
// ---------------------------------------------------------------------------
struct Event { double tMs; int syl; float hz; };

std::vector<float> run(Core& c, const std::vector<float>& in, std::vector<Event>* ev = nullptr) {
    std::vector<float> y = in;
    const int block = 128;
    int last = c.syllableIndex();
    for (int i = 0; i < (int) y.size(); i += block) {
        const int n = std::min(block, (int) y.size() - i);
        float* ch[1] = { y.data() + i };
        c.process(ch, 1, n);
        if (ev != nullptr) {
            const int s = c.syllableIndex();
            if (s != last) { ev->push_back({ 1000.0 * i / kSr, s, c.trackedHz() }); last = s; }
        }
    }
    return y;
}

void makeCore(Core& c, int pattern, float vibrato = 0.45f, float breath = 0.35f, float octave = 1.0f) {
    c.prepare(kSr, 512, 1);
    dcdsp::MikuStompParams p;
    p.mix = 1.0f; p.character = 0.5f; p.breath = breath;
    p.attackMs = 12.0f; p.portamentoMs = 50.0f;
    p.vibrato = vibrato; p.vibratoRateHz = 5.8f; p.octave = octave;
    p.pattern = (float) pattern;
    c.setParams(p);
}

// ---------------------------------------------------------------------------
// LPC formant tracking. Decimate 48k -> 16k, pre-emphasise, Hamming, autocorr,
// Levinson-Durbin, then peak-pick the LPC spectral envelope. Standard recipe.
// ---------------------------------------------------------------------------
constexpr double kSrA   = 16000.0;   // analysis rate
constexpr int    kOrder = 18;

std::vector<float> decimate(const std::vector<float>& x) {
    dcdsp::Biquad lp1, lp2;
    lp1.setLowpass(kSr, 6800.0, 0.707);
    lp2.setLowpass(kSr, 6800.0, 0.707);
    std::vector<float> y;
    y.reserve(x.size() / 3 + 4);
    for (size_t i = 0; i < x.size(); ++i) {
        const float v = lp2.process(lp1.process(x[i]));
        if (i % 3 == 0) y.push_back(v);
    }
    return y;
}

struct Frame { double tMs; double rms; double f[4]; int nf; double voicing; };

double g_allPeaks[8] = { 0 };
int    g_allPeakCount = 0;

// Peak-pick |1/A(w)| on a fine grid.
void lpcFormants(const double* a, double& f1, double& f2, double& f3, double& f4, int& nf) {
    f1 = f2 = f3 = f4 = 0.0; nf = 0;
    const int N = 1024;
    std::vector<double> mag((size_t) N);
    for (int k = 0; k < N; ++k) {
        const double hz = 90.0 + (5200.0 - 90.0) * (double) k / (double) (N - 1);
        const double w  = 2.0 * kPi * hz / kSrA;
        double re = 1.0, im = 0.0;
        for (int i = 1; i <= kOrder; ++i) {
            re += a[i] * std::cos(w * (double) i);
            im -= a[i] * std::sin(w * (double) i);
        }
        const double d = std::sqrt(re * re + im * im);
        mag[(size_t) k] = 1.0 / ((d > 1e-12) ? d : 1e-12);
    }
    double out[8]; int m = 0;
    for (int k = 1; k < N - 1 && m < 8; ++k) {
        if (mag[(size_t) k] > mag[(size_t) (k - 1)] && mag[(size_t) k] >= mag[(size_t) (k + 1)]) {
            // parabolic refine on the log magnitude
            const double y0 = std::log(mag[(size_t) (k - 1)]);
            const double y1 = std::log(mag[(size_t) k]);
            const double y2 = std::log(mag[(size_t) (k + 1)]);
            const double den = y0 - 2.0 * y1 + y2;
            const double d = (std::fabs(den) > 1e-12) ? 0.5 * (y0 - y2) / den : 0.0;
            const double step = (5200.0 - 90.0) / (double) (N - 1);
            out[m++] = 90.0 + step * ((double) k + std::max(-1.0, std::min(1.0, d)));
        }
    }
    nf = m;
    if (m > 0) f1 = out[0];
    if (m > 1) f2 = out[1];
    if (m > 2) f3 = out[2];
    if (m > 3) f4 = out[3];
    for (int i = 0; i < 8; ++i) g_allPeaks[i] = (i < m) ? out[i] : 0.0;
    g_allPeakCount = m;
}

std::vector<Frame> track(const std::vector<float>& x48) {
    const std::vector<float> x = decimate(x48);
    const int win = (int) (0.030 * kSrA);   // 30 ms
    const int hop = (int) (0.005 * kSrA);   // 5 ms
    std::vector<Frame> out;
    std::vector<double> w((size_t) win);
    for (int i = 0; i < win; ++i)
        w[(size_t) i] = 0.54 - 0.46 * std::cos(2.0 * kPi * (double) i / (double) (win - 1));

    for (int s = 0; s + win <= (int) x.size(); s += hop) {
        std::vector<double> buf((size_t) win);
        double pe = 0.0, e = 0.0;
        for (int i = 0; i < win; ++i) {
            const double v = (double) x[(size_t) (s + i)];
            buf[(size_t) i] = (v - 0.97 * pe) * w[(size_t) i];
            pe = v;
            e += v * v;
        }
        Frame fr{};
        fr.tMs = 1000.0 * (double) (s + win / 2) / kSrA;
        fr.rms = std::sqrt(e / (double) win);

        // VOICING (periodicity). Needed because the bursts and frication are now
        // loud enough that an RMS threshold calls them "the voice has started" and
        // then LPC-tracks the NOISE band as if it were F2. A formant transition is
        // only a cue if it happens while the vocal folds are actually running, so
        // the transition metric must gate on periodicity, not on level.
        {
            const double* w0 = nullptr; (void) w0;
            double e0 = 1e-12;
            for (int i = 0; i < win; ++i) e0 += (double) x[(size_t)(s+i)] * (double) x[(size_t)(s+i)];
            double best = 0.0;
            const int loLag = (int) (kSrA / 500.0), hiLag = std::min(win / 2, (int) (kSrA / 70.0));
            for (int lag = loLag; lag <= hiLag; ++lag) {
                double acc = 0.0, el = 1e-12;
                for (int i = 0; i + lag < win; ++i) {
                    const double a0 = (double) x[(size_t)(s+i)];
                    const double b0 = (double) x[(size_t)(s+i+lag)];
                    acc += a0 * b0; el += b0 * b0;
                }
                const double nc = acc / std::sqrt(e0 * el);
                if (nc > best) best = nc;
            }
            fr.voicing = best;
        }

        // autocorrelation
        double r[kOrder + 1];
        for (int k = 0; k <= kOrder; ++k) {
            double acc = 0.0;
            for (int i = 0; i + k < win; ++i) acc += buf[(size_t) i] * buf[(size_t) (i + k)];
            r[k] = acc;
        }
        if (r[0] < 1e-12) { out.push_back(fr); continue; }
        r[0] *= 1.0001;   // ridge
        // Levinson-Durbin
        double a[kOrder + 1] = { 0 }, tmp[kOrder + 1] = { 0 };
        double err = r[0];
        a[0] = 1.0;
        for (int i = 1; i <= kOrder; ++i) {
            double acc = r[i];
            for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
            const double k = -acc / ((std::fabs(err) > 1e-18) ? err : 1e-18);
            for (int j = 0; j <= i; ++j) tmp[j] = a[j];
            for (int j = 1; j < i; ++j) a[j] = tmp[j] + k * tmp[i - j];
            a[i] = k;
            err *= (1.0 - k * k);
            if (err <= 0.0) { err = 1e-18; break; }
        }
        lpcFormants(a, fr.f[0], fr.f[1], fr.f[2], fr.f[3], fr.nf);
        out.push_back(fr);
    }
    return out;
}

// ---- band energy helpers ---------------------------------------------------
double rmsOf(const std::vector<float>& x, int a, int b) {
    a = std::max(0, a); b = std::min((int) x.size(), b);
    if (b <= a) return 0.0;
    double e = 0.0;
    for (int i = a; i < b; ++i) e += (double) x[(size_t) i] * (double) x[(size_t) i];
    return std::sqrt(e / (double) (b - a));
}
double db(double v) { return 20.0 * std::log10((v > 1e-12) ? v : 1e-12); }

// Goertzel-style band energy over [lo,hi] via a naive DFT on a decimated grid.
double bandEnergy(const std::vector<float>& x, int a, int b, double lo, double hi) {
    a = std::max(0, a); b = std::min((int) x.size(), b);
    if (b - a < 64) return 0.0;
    double acc = 0.0;
    const int steps = 40;
    for (int s = 0; s < steps; ++s) {
        const double hz = lo + (hi - lo) * (double) s / (double) (steps - 1);
        const double w = 2.0 * kPi * hz / kSr;
        double re = 0.0, im = 0.0;
        for (int i = a; i < b; ++i) {
            const double ph = w * (double) (i - a);
            re += (double) x[(size_t) i] * std::cos(ph);
            im -= (double) x[(size_t) i] * std::sin(ph);
        }
        acc += (re * re + im * im) / (double) ((b - a) * (b - a));
    }
    return std::sqrt(acc / (double) steps);
}

double centroid(const std::vector<float>& x, int a, int b) {
    a = std::max(0, a); b = std::min((int) x.size(), b);
    if (b - a < 64) return 0.0;
    double num = 0.0, den = 0.0;
    for (int s = 0; s < 60; ++s) {
        const double hz = 200.0 + (9000.0 - 200.0) * (double) s / 59.0;
        const double w = 2.0 * kPi * hz / kSr;
        double re = 0.0, im = 0.0;
        for (int i = a; i < b; ++i) {
            const double ph = w * (double) (i - a);
            re += (double) x[(size_t) i] * std::cos(ph);
            im -= (double) x[(size_t) i] * std::sin(ph);
        }
        const double m = std::sqrt(re * re + im * im);
        num += hz * m; den += m;
    }
    return (den > 1e-12) ? num / den : 0.0;
}

// ===========================================================================
// T0 — the nasal antiresonator's magnitude response, straight from the core's
// own coefficient recipe (AntiRes::set is private, so it is transcribed here
// verbatim; if this is wrong the test is wrong, so keep it in sync).
// ===========================================================================
void t0_antiresonator() {
    std::printf("\n=== T0  NASAL ANTIRESONATOR RESPONSE (core recipe, verbatim) ===\n");
    std::printf("A Klatt nasal ZERO is the INVERSE of a resonator: unity at DC, and it\n"
                "RISES ~+12 dB/oct above its centre forever. Klatt pairs it with a nasal\n"
                "POLE that cancels that rise. If the pole is missing, |H| below is the\n"
                "gain the murmur actually gets.\n\n");
    const double freqs[] = { 558.0, 950.0, 1500.0 };   // what the core sets (F2loc*0.62)
    for (double fz : freqs) {
        const double bw = 300.0;
        const double r  = std::exp(-kPi * bw / kSr);
        const double cc = -(r * r);
        const double bb = 2.0 * r * std::cos(2.0 * kPi * fz / kSr);
        const double aa = 1.0 - bb - cc;
        const double A = 1.0 / aa, B = -bb / aa, C = -cc / aa;
        std::printf("  zero @ %6.0f Hz (bw 300): leading coeff a = %8.1f\n", fz, A);
        std::printf("    |H|  ");
        for (double f : { 100.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 }) {
            const double w = 2.0 * kPi * f / kSr;
            const double re = A + B * std::cos(w) + C * std::cos(2 * w);
            const double im = -(B * std::sin(w) + C * std::sin(2 * w));
            const double m = std::sqrt(re * re + im * im);
            std::printf("%5.0fHz:%+7.1fdB  ", f, 20.0 * std::log10(m + 1e-12));
        }
        std::printf("\n");
    }
}

// ===========================================================================
// T1 — the tail. Play, then hard-cut to digital silence. Everything after the
// last input sample is, by definition, the module talking to itself.
// ===========================================================================
void t1_tail(int pattern, const char* name, const std::vector<Syl>* phrase) {
    const int preSec  = 3;     // 3 s of playing
    const int tailSec = 3;     // 3 s of digital silence after
    const int nPre  = (int) (preSec * kSr);
    const int nTail = (int) (tailSec * kSr);
    std::vector<float> in((size_t) (nPre + nTail), 0.0f);

    // four picks, 700 ms apart, then the last note is CUT dead at nPre.
    const double f0[] = { 110.0, 146.83, 130.81, 164.81 };
    for (int k = 0; k < 4; ++k)
        pluck(in, (int) (k * 0.7 * kSr), f0[k], 1.6, 0.35f);
    for (int i = nPre; i < nPre + nTail; ++i) in[(size_t) i] = 0.0f;

    Core c;
    makeCore(c, pattern);
    if (phrase != nullptr) c.setPhrase(0, phrase->data(), (int) phrase->size());
    std::vector<Event> ev;
    std::vector<float> y = run(c, in, &ev);

    std::printf("\n=== T1  TAIL / BLOB  [pattern %d = %s] ===\n", pattern, name);
    std::printf("  input is digital silence from t = %d.000 s onward.\n", preSec);

    const double sus = rmsOf(y, nPre - (int) (0.3 * kSr), nPre);
    std::printf("  sustained level before the cut: %.1f dBFS\n", db(sus));
    std::printf("  ms after cut :   RMS      vs sustain   centroid\n");
    for (double ms : { 20.0, 50.0, 100.0, 150.0, 200.0, 300.0, 500.0, 800.0, 1200.0, 2000.0 }) {
        const int a = nPre + (int) (ms * 0.001 * kSr);
        const int b = a + (int) (0.02 * kSr);
        const double r = rmsOf(y, a, b);
        std::printf("   %7.0f      %8.1f dB   %+7.1f dB   %6.0f Hz\n",
                    ms, db(r), db(r) - db(sus), centroid(y, a, b));
    }
    double peakAfter = 0.0; int peakAt = 0;
    for (int i = nPre; i < (int) y.size(); ++i)
        if (std::fabs(y[(size_t) i]) > peakAfter) { peakAfter = std::fabs(y[(size_t) i]); peakAt = i; }
    std::printf("  PEAK after the cut: %.1f dBFS at +%.0f ms\n",
                db(peakAfter), 1000.0 * (peakAt - nPre) / kSr);
    // time to fall 60 dB below the sustained level
    int t60 = -1;
    for (int i = nPre; i + (int) (0.02 * kSr) < (int) y.size(); i += (int) (0.005 * kSr)) {
        if (db(rmsOf(y, i, i + (int) (0.02 * kSr))) < db(sus) - 60.0) { t60 = i; break; }
    }
    std::printf("  time to -60 dB below sustain: %s\n",
                t60 < 0 ? "NEVER (still ringing at +3 s)"
                        : (std::to_string((long) std::lround(1000.0 * (t60 - nPre) / kSr)) + " ms").c_str());

    std::printf("  syllable-advance timeline (a pick is at 0/700/1400/2100 ms):\n");
    for (const Event& e : ev)
        std::printf("     t=%7.1f ms  -> syl %d   (trackedHz %.1f)%s\n", e.tMs, e.syl, e.hz,
                    (e.tMs > 2200.0) ? "   <-- NOBODY PICKED HERE" : "");
    if (ev.empty()) std::printf("     (none)\n");

    writeWav(std::string("miku_tail_") + name + ".wav", y, kSr);
}

// ===========================================================================
// T2 / T3 — per-phoneme signature + coarticulation.
// ===========================================================================
struct PhonRow {
    const char* name; int cons; const char* kind;
};

void t2_phonemes() {
    static const PhonRow rows[] = {
        { "a  (none)", Core::C_None, "vowel" },
        { "ka", Core::C_K,   "stop(vl)" },   { "ga", Core::C_G,  "stop(vd)" },
        { "ta", Core::C_T,   "stop(vl)" },   { "da", Core::C_D,  "stop(vd)" },
        { "pa", Core::C_P,   "stop(vl)" },   { "ba", Core::C_B,  "stop(vd)" },
        { "sa", Core::C_S,   "fric" },       { "sha",Core::C_SH, "fric" },
        { "za", Core::C_Z,   "fric" },       { "fa", Core::C_F,  "fric" },
        { "ha", Core::C_H,   "aspirate" },
        { "tsa",Core::C_TS,  "affric" },     { "cha",Core::C_CH, "affric" },
        { "ja", Core::C_J,   "affric" },
        { "na", Core::C_N,   "nasal" },      { "nya",Core::C_NY, "nasal" },
        { "ma", Core::C_M,   "nasal" },      { "Na", Core::C_NN, "nasal" },
        { "ya", Core::C_Y,   "approx" },     { "ra", Core::C_R,  "approx" },
        { "wa", Core::C_W,   "approx" },
    };

    std::printf("\n=== T2  PHONEME SIGNATURE (each consonant + /a/, one pick) ===\n");
    std::printf("  A stop MUST show a silent closure then an audible burst. A fricative MUST\n"
                "  show shaped HF noise. A nasal MUST show a DARK, damped murmur.\n"
                "  The two contrast columns are measured against the BARE VOWEL /a/ (row 1),\n"
                "  which is the only fair reference: a band energy cannot be compared to a\n"
                "  broadband RMS.\n\n");
    std::printf("  %-10s %-9s  closure   burst     HF 2-8k      murmur\n", "syllable", "kind");
    std::printf("  %-10s %-9s  (dB/vow)  (dB/vow)  (dB vs /a/)  (dB darker than /a/)  verdict\n", "", "");
    double refFric = 0.0, refLfHf = 0.0;

    for (const PhonRow& row : rows) {
        std::vector<float> in((size_t) (1.2 * kSr), 0.0f);
        pluck(in, (int) (0.05 * kSr), 130.81, 2.5, 0.35f);
        Core c;
        makeCore(c, 6, /*vibrato*/ 0.0f);
        const Syl s{ row.cons, Core::V_A };
        c.setPhrase(0, &s, 1);
        std::vector<float> y = run(c, in);

        const int on = (int) (0.05 * kSr);                       // the pick
        const int steady = on + (int) (0.35 * kSr);              // the vowel, well settled
        const double vow = rmsOf(y, steady, steady + (int) (0.2 * kSr));

        // closure: the quietest 10 ms in the first 60 ms after the pick
        double quiet = 1e9;
        for (int i = on; i < on + (int) (0.06 * kSr); i += (int) (0.005 * kSr))
            quiet = std::min(quiet, rmsOf(y, i, i + (int) (0.01 * kSr)));
        // burst: the loudest 8 ms in 0..110 ms after the pick
        double burst = 0.0;
        for (int i = on; i < on + (int) (0.11 * kSr); i += (int) (0.002 * kSr))
            burst = std::max(burst, rmsOf(y, i, i + (int) (0.008 * kSr)));
        // frication: HF energy 2-8 kHz over the first 90 ms
        const double fric = bandEnergy(y, on, on + (int) (0.09 * kSr), 2000.0, 8000.0);
        // nasal murmur: LF/HF ratio over the closure window
        const double lf = bandEnergy(y, on + (int) (0.01 * kSr), on + (int) (0.05 * kSr), 150.0, 500.0);
        const double hf = bandEnergy(y, on + (int) (0.01 * kSr), on + (int) (0.05 * kSr), 1500.0, 6000.0);

        if (row.cons == Core::C_None) { refFric = db(fric); refLfHf = db(lf) - db(hf); }
        const double hfContrast   = db(fric) - refFric;              // vs the bare vowel
        const double darkContrast = (db(lf) - db(hf)) - refLfHf;     // + = darker than a vowel

        char verdict[110] = "";
        const std::string k = row.kind;
        if (k.rfind("stop", 0) == 0) {
            if (db(quiet) - db(vow) > -12.0) std::strcpy(verdict, "NO CLOSURE (not a stop)");
            else if (db(burst) - db(vow) < -4.0) std::strcpy(verdict, "BURST INAUDIBLE");
            else if (hfContrast < 3.0) std::strcpy(verdict, "burst has no HF signature");
            else std::strcpy(verdict, "ok");
        } else if (k == "fric" || k == "affric") {
            if (hfContrast < 12.0) std::snprintf(verdict, sizeof verdict,
                                                 "FRICATION WEAK (only %+.0f dB HF over a vowel)", hfContrast);
            else std::strcpy(verdict, "ok");
        } else if (k == "aspirate") {
            if (hfContrast < 8.0) std::snprintf(verdict, sizeof verdict,
                                                "NO ASPIRATION (%+.1f dB HF over a vowel)", hfContrast);
            else std::strcpy(verdict, "ok");
        } else if (k == "nasal") {
            if (darkContrast < 8.0) std::snprintf(verdict, sizeof verdict,
                    "NOT A MURMUR: only %+.0f dB darker than a plain vowel", darkContrast);
            else std::strcpy(verdict, "ok");
        } else std::strcpy(verdict, "-");

        std::printf("  %-10s %-9s %+8.1f  %+8.1f  %+9.1f    %+9.1f          %s\n",
                    row.name, row.kind,
                    db(quiet) - db(vow), db(burst) - db(vow),
                    hfContrast, darkContrast, verdict);
    }
}

void t3_coarticulation() {
    std::printf("\n=== T3  COARTICULATION — the formant TRANSITION ===\n");
    std::printf("  Delattre: a consonant is identified by WHERE F2 COMES FROM. If F2 is\n"
                "  already at the vowel's value by the time the vocal folds start, there is\n"
                "  no transition to hear and the consonant is unidentifiable.\n"
                "  Frames are gated on PERIODICITY (>0.5), so a loud burst cannot be mistaken\n"
                "  for the voice starting. dF2 = |F2 at first voiced frame - F2 at steady state|.\n\n");
    std::printf("  %-5s %8s %8s  F2 at voiced onset +0 / +20 / +40 / +80 ms   steady    dF2\n",
                "syl", "F2 locus", "expect");

    struct R { const char* n; int c; double f2loc; bool murmur; };
    static const R rows[] = {
        { "ka", Core::C_K, 1900, false }, { "ta", Core::C_T, 1750, false }, { "pa", Core::C_P, 750, false },
        { "ga", Core::C_G, 1900, false }, { "da", Core::C_D, 1750, false }, { "ba", Core::C_B, 750, false },
        { "ma", Core::C_M,  900, true  }, { "na", Core::C_N, 1700, true  }, { "wa", Core::C_W, 700, false },
        { "ya", Core::C_Y, 2300, false }, { "ra", Core::C_R, 1600, false }, { "sa", Core::C_S, 1750, false },
    };
    const double scale = 1.08;             // `character` 0.5
    const double vowF2 = 1530.0 * scale;   // /a/
    for (const R& row : rows) {
        std::vector<float> in((size_t) (1.0 * kSr), 0.0f);
        pluck(in, (int) (0.05 * kSr), 130.81, 2.5, 0.35f);
        Core c;
        makeCore(c, 6, 0.0f, 0.35f, /*octave*/ 0.0f);   // low f0: LPC needs harmonics
        const Syl s{ row.c, Core::V_A };
        c.setPhrase(0, &s, 1);
        std::vector<float> y = run(c, in);

        std::vector<Frame> fr = track(y);
        double steadyF2 = 0.0, steadyRms = 0.0;
        for (const Frame& f : fr)
            if (f.tMs > 450.0 && f.tMs < 500.0 && f.voicing > 0.5) { steadyF2 = f.f[1]; steadyRms = f.rms; break; }

        double onMs = -1.0;
        for (const Frame& f : fr) {
            if (f.tMs < 55.0) continue;            // the pick lands at 50 ms
            // Gate on level TOO: a voiced-stop's voice bar and a nasal's murmur are
            // periodic (voicing > 0.5) but they are the CLOSURE, not the release —
            // LPC on those low-level frames returns garbage (measured: /ba/ "onset"
            // F2 1932 for an 810 locus, /ma/ 3083) and the metric then reported a
            // missing transition that was actually there one frame later.
            // A nasal's MURMUR is voiced and periodic, but its F2 is unmeasurable
            // (pole at ~300 Hz, everything else damped) — LPC returns garbage
            // there. The nasal's transition cue starts at the murmur -> vowel
            // OPENING, so for nasals the gate waits for the opening's level.
            const double rmsGate = row.murmur ? 0.70 : 0.35;
            if (f.voicing > 0.5 && f.rms > rmsGate * steadyRms) { onMs = f.tMs; break; }
        }
        // ... and on STABILITY: an LPC window that straddles the burst reads a
        // wild F2 for exactly one frame (measured on /ba/: 1965 Hz for an 810
        // locus, then 1209 one hop later). Two consecutive frames must agree.
        {
            auto rawF2 = [&](double ms) -> double {
                double best = 0.0, bd = 1e9;
                for (const Frame& f : fr) {
                    const double d = std::fabs(f.tMs - ms);
                    if (d < bd) { bd = d; best = f.f[1]; }
                }
                return best;
            };
            int guard = 0;
            while (onMs >= 0.0 && guard++ < 8
                   && std::fabs(rawF2(onMs) - rawF2(onMs + 5.0)) > 350.0)
                onMs += 5.0;
        }
        // Median-of-3 over the frame grid: one junk LPC frame (a window straddling
        // the burst) must not be able to fake or hide a transition.
        auto f2At = [&](double ms) -> double {
            double v[3] = { 0, 0, 0 };
            for (int k = -1; k <= 1; ++k) {
                double best = 0.0, bd = 1e9;
                for (const Frame& f : fr) {
                    const double d = std::fabs(f.tMs - (ms + 5.0 * k));
                    if (d < bd) { bd = d; best = f.f[1]; }
                }
                v[(size_t) (k + 1)] = best;
            }
            return std::max(std::min(v[0], v[1]), std::min(std::max(v[0], v[1]), v[2]));
        };
        if (onMs < 0.0) { std::printf("  %-5s  (no voiced frame found)\n", row.n); continue; }
        const double a0 = f2At(onMs), a1 = f2At(onMs + 20), a2 = f2At(onMs + 40), a3 = f2At(onMs + 80);
        // SIGNED fraction: is early F2 on the LOCUS side of the steady state, by
        // enough to hear? (For a voiceless stop the voicing starts mid-sweep —
        // after the burst and the VOT — so a fraction of the locus offset is what
        // remains BY DESIGN; the aspiration carried the rest.)
        const double signedWant = row.f2loc * scale - vowF2;
        const double want = std::fabs(signedWant);
        // Murmur rows: LPC is blind INSIDE the murmur (measured: /ma/'s murmur
        // frames read 2100-3100 Hz garbage while passing every level gate), so
        // the transition is judged where it actually runs — after the opening,
        // the +40 ms column.
        const double judged = row.murmur ? a2 : a0;
        const double frac = (steadyF2 != 0.0 && want > 1.0) ? (judged - steadyF2) / signedWant : 0.0;
        // < 200 Hz of locus offset is below the transition's perceptual JND — for
        // those consonants the identity is carried elsewhere (/r/ by its flap dip,
        // /n/ before /a/ by the murmur + antiformant), so there is nothing to fail.
        std::printf("  %-5s %8.0f %8.0f  %8.0f %7.0f %7.0f %7.0f   %7.0f  %5.0f%%  %s\n",
                    row.n, row.f2loc * scale, want, a0, a1, a2, a3, steadyF2, 100.0 * frac,
                    (want < 200.0) ? "(locus ~= vowel: nothing to sweep)"
                                   : (frac > 0.20 ? (row.murmur ? "ok (judged past the murmur)" : "ok")
                                                  : "TRANSITION MISSING"));
    }
}

void t4_vowels() {
    std::printf("\n=== T4  VOWEL FORMANT ACCURACY (LPC, steady state) ===\n");
    static const char* vn[] = { "a", "i", "u", "e", "o" };
    static const double tbl[5][3] = {
        { 850, 1530, 2900 }, { 330, 2790, 3400 }, { 380, 1350, 2950 },
        { 520, 2350, 3050 }, { 470,  850, 2900 },
    };
    std::printf("  vowel   F1 meas/target      F2 meas/target      F3 meas/target\n");
    for (int v = 0; v < 5; ++v) {
        std::vector<float> in((size_t) (1.2 * kSr), 0.0f);
        pluck(in, (int) (0.05 * kSr), 130.81, 3.0, 0.35f);
        Core c;
        makeCore(c, 6, 0.0f, 0.35f, /*octave*/ 0.0f);   // low f0: LPC needs harmonics
        const Syl s{ Core::C_None, v };
        c.setPhrase(0, &s, 1);
        std::vector<float> y = run(c, in);
        std::vector<Frame> fr = track(y);
        double f1 = 0, f2 = 0, f3 = 0; int n = 0;
        for (const Frame& f : fr)
            if (f.tMs > 300.0 && f.tMs < 700.0 && f.nf >= 3) { f1 += f.f[0]; f2 += f.f[1]; f3 += f.f[2]; ++n; }
        if (n == 0) { std::printf("  %-5s  (no frames)\n", vn[v]); continue; }
        f1 /= n; f2 /= n; f3 /= n;
        // `character` 0.5 scales the table by 0.88 + 0.40*0.5 = 1.08
        const double sc = 1.08;
        std::printf("  %-5s   %5.0f / %5.0f (%+5.1f%%)  %5.0f / %5.0f (%+5.1f%%)  %5.0f / %5.0f (%+5.1f%%)\n",
                    vn[v],
                    f1, tbl[v][0] * sc, 100.0 * (f1 / (tbl[v][0] * sc) - 1.0),
                    f2, tbl[v][1] * sc, 100.0 * (f2 / (tbl[v][1] * sc) - 1.0),
                    f3, tbl[v][2] * sc, 100.0 * (f3 / (tbl[v][2] * sc) - 1.0));
        // Every LPC peak, so a merged F1/F2 pair cannot masquerade as a bad synth.
        // (This is why /o/ reads badly: F1 549 and F2 991 are 442 Hz apart and the
        // LPC envelope merges them into one broad peak. Look for the shoulder.)
        std::printf("           all LPC peaks:");
        for (int i = 0; i < g_allPeakCount && i < 6; ++i) std::printf(" %5.0f", g_allPeaks[i]);
        std::printf("\n");
    }
}

void t5_audition() {
    std::printf("\n=== T5  AUDITION DUMPS ===\n");
    // A full sung phrase — this is what the owner should listen to.
    struct P { const char* file; const char* lyric; };
    static const P ps[] = {
        { "miku_hatsunemiku", "ha tsu ne mi ku" },
        { "miku_konnichiwa",  "ko nn ni chi wa" },
        { "miku_sakura",      "sa ku ra sa ku ra" },
        { "miku_allcons",     "ka ga sa sha ta da na ha ba pa ma ya ra wa" },
    };
    for (const P& p : ps) {
        Syl syl[Core::kMaxSyllables];
        const int n = Core::parsePhrases(p.lyric, syl, Core::kMaxSyllables);
        const double dur = 0.55;
        std::vector<float> in((size_t) ((n * dur + 2.0) * kSr), 0.0f);
        static const double scale[] = { 220.0, 246.94, 261.63, 293.66, 329.63, 349.23, 392.0 };
        for (int k = 0; k < n; ++k)
            pluck(in, (int) (k * dur * kSr), scale[k % 7] * 0.5, 1.4, 0.35f);
        Core c;
        makeCore(c, 6);
        c.setPhrase(0, syl, n);
        std::vector<float> y = run(c, in);
        writeWav(std::string(p.file) + ".wav", y, kSr);
    }
    // ALL 11 patterns straight off the knob (the hardware's own order), with the
    // three Phrase slots loaded the way the factory presets load them.
    for (int pat = 0; pat < 11; ++pat) {
        std::vector<float> in((size_t) (6.0 * kSr), 0.0f);
        for (int k = 0; k < 8; ++k) pluck(in, (int) (k * 0.6 * kSr), 130.81 * (1.0 + 0.06 * (k % 5)), 1.5, 0.35f);
        Core c;
        makeCore(c, pat);
        static const char* slotLyrics[3] = { "ha tsu ne mi ku", "ko nn ni chi wa", "sa ku ra sa ku ra" };
        for (int sl = 0; sl < 3; ++sl) {
            Syl syl[Core::kMaxSyllables];
            const int n = Core::parsePhrases(slotLyrics[sl], syl, Core::kMaxSyllables);
            c.setPhrase(sl, syl, n);
        }
        std::vector<float> y = run(c, in);
        writeWav("miku_pattern" + std::to_string(pat) + ".wav", y, kSr);
    }
    // The STRESS phrase: one mora from every row of the gojūon through the weak
    // classes (sibilants, affricates, aspirates, nasals, the flap), plus the
    // moraic nasal — if a letter class is broken, this is where you hear it.
    {
        const char* stress = "ka ki ku ke ko sa shi su se so ta chi tsu te to"
                             " na ni nu ne no ha hi fu he ho ma mi mu me mo"
                             " ra ri ru re ro nya n";
        Syl syl[Core::kMaxSyllables];
        const int n = Core::parsePhrases(stress, syl, Core::kMaxSyllables);
        const double dur = 0.42;
        std::vector<float> in((size_t) ((n * dur + 1.5) * kSr), 0.0f);
        static const double scale[] = { 220.0, 246.94, 261.63, 293.66, 329.63 };
        for (int k = 0; k < n; ++k)
            pluck(in, (int) (k * dur * kSr), scale[k % 5] * 0.5, 1.1, 0.35f);
        Core c;
        makeCore(c, 6);
        c.setPhrase(0, syl, n);
        writeWav("miku_stress_gojuon.wav", run(c, in), kSr);
    }
    // A fast riff (12 picks/s) — the sequencer keeping up is audible here.
    {
        std::vector<float> in((size_t) (4.0 * kSr), 0.0f);
        for (int k = 0; k < 40; ++k)
            pluck(in, (int) (k * kSr / 12.0), (k % 2) ? 146.83 : 110.0, 0.8, 0.35f);
        Core c;
        makeCore(c, 9);
        writeWav("miku_fastriff_12ps.wav", run(c, in), kSr);
    }
}

void t6_natural_and_noise() {
    std::printf("\n=== T6  NATURAL DECAY + NOISY INPUT (no hard cut) ===\n");
    std::printf("  A real player lets the string ring out, and a real pickup has a noise\n"
                "  floor. Does the sequencer fire syllables nobody played?\n");
    for (int noisy = 0; noisy < 2; ++noisy) {
        std::vector<float> in((size_t) (6.0 * kSr), 0.0f);
        for (int k = 0; k < 3; ++k) pluck(in, (int) (k * 0.8 * kSr), 110.0 * (1.0 + 0.12 * k), 2.2, 0.35f);
        if (noisy) {
            // pickup hiss + 50 Hz hum + a sympathetic string beating a few cents off
            for (int i = 0; i < (int) in.size(); ++i) {
                const double t = (double) i / kSr;
                in[(size_t) i] += 0.0015f * noise()
                                + 0.0010f * (float) std::sin(2.0 * kPi * 50.0 * t)
                                + 0.012f * (float) (std::exp(-std::fmod(t, 0.8) / 1.5)
                                                    * std::sin(2.0 * kPi * 110.9 * t));
            }
        }
        Core c;
        makeCore(c, 9);                       // Random 1: 16 syllables, so a stray advance shows
        std::vector<Event> ev;
        std::vector<float> y = run(c, in, &ev);
        std::printf("  --- %s input: picks at 0 / 800 / 1600 ms ---\n", noisy ? "NOISY " : "clean ");
        int stray = 0;
        for (const Event& e : ev) {
            const bool atPick = (std::fabs(e.tMs - 0.0) < 40) || (std::fabs(e.tMs - 800.0) < 40)
                             || (std::fabs(e.tMs - 1600.0) < 40);
            if (!atPick) ++stray;
            std::printf("     t=%7.1f ms -> syl %2d  (%.1f Hz) %s\n", e.tMs, e.syl, e.hz,
                        atPick ? "" : "  <-- STRAY: nobody picked");
        }
        std::printf("     3 picks -> %d syllable advances (%d stray)\n", (int) ev.size(), stray);
        writeWav(noisy ? "miku_natural_noisy.wav" : "miku_natural_clean.wav", y, kSr);
    }
}

// ===========================================================================
// T7 — the gate must not CHATTER. Shortening the gate's release to kill the
// tail is only safe if a held note stays smooth: a peak follower whose release
// is near one period of a low E ripples, and a ripple on the GATE amplitude-
// modulates the whole voice. Measure the AM depth of a sustained note.
// ===========================================================================
void t7_sustain_am() {
    std::printf("\n=== T7  SUSTAINED-NOTE AM (does the shortened gate chatter?) ===\n");
    std::printf("  peak-to-trough of the output envelope over a held note. Anything over\n"
                "  ~3 dB is audible amplitude modulation / gate chatter.\n");
    std::printf("  f0 (guitar)   AM depth (200-800 ms after the pick)\n");
    for (double f0 : { 82.41, 110.0, 164.81, 329.63 }) {
        std::vector<float> in((size_t) (2.0 * kSr), 0.0f);
        pluck(in, (int) (0.05 * kSr), f0, 3.0, 0.35f);
        Core c;
        makeCore(c, 0, /*vibrato*/ 0.0f);
        std::vector<float> y = run(c, in);
        double lo = 1e9, hi = 0.0;
        for (int i = (int) (0.25 * kSr); i + (int) (0.03 * kSr) < (int) (0.85 * kSr); i += (int) (0.01 * kSr)) {
            const double r = rmsOf(y, i, i + (int) (0.03 * kSr));
            lo = std::min(lo, r); hi = std::max(hi, r);
        }
        std::printf("  %7.1f Hz    %5.1f dB %s\n", f0, db(hi) - db(lo),
                    (db(hi) - db(lo) > 3.0) ? "  <-- CHATTER" : "");
    }
}

#ifdef MIKU_WITH_V1
// ===========================================================================
// T8 — does the FROZEN v1 `miku` core have the same "blob at the end"?
// v1 is not ours to change; we only need to know whether the defect is shared,
// so the owner knows whether v1 still has it. Same hard cut, same measurement.
// ===========================================================================
void t8_v1_tail() {
    std::printf("\n=== T8  THE FROZEN v1 `miku` CORE — same tail test ===\n");
    const int nPre = (int) (3.0 * kSr), nTail = (int) (3.0 * kSr);
    std::vector<float> in((size_t) (nPre + nTail), 0.0f);
    const double f0[] = { 110.0, 146.83, 130.81, 164.81 };
    for (int k = 0; k < 4; ++k) pluck(in, (int) (k * 0.7 * kSr), f0[k], 1.6, 0.35f);
    for (int i = nPre; i < nPre + nTail; ++i) in[(size_t) i] = 0.0f;

    dcdsp::MikuCore c;
    c.prepare(kSr, 512, 1);
    dcdsp::MikuParams p;
    p.mix = 1.0f; p.character = 0.5f; p.octave = 1.0f; p.pattern = 4;  // Nyan
    c.setParams(p);
    std::vector<float> y = in;
    for (int i = 0; i < (int) y.size(); i += 128) {
        const int n = std::min(128, (int) y.size() - i);
        float* ch[1] = { y.data() + i };
        c.process(ch, 1, n);
    }
    const double sus = rmsOf(y, nPre - (int) (0.3 * kSr), nPre);
    std::printf("  sustained before the cut: %.1f dBFS\n", db(sus));
    for (double ms : { 20.0, 50.0, 100.0, 200.0, 300.0, 500.0 }) {
        const int a = nPre + (int) (ms * 0.001 * kSr);
        const double r = rmsOf(y, a, a + (int) (0.02 * kSr));
        std::printf("   +%4.0f ms: %+7.1f dB vs sustain\n", ms, db(r) - db(sus));
    }
    int t60 = -1;
    for (int i = nPre; i + (int) (0.02 * kSr) < (int) y.size(); i += (int) (0.005 * kSr))
        if (db(rmsOf(y, i, i + (int) (0.02 * kSr))) < db(sus) - 60.0) { t60 = i; break; }
    std::printf("  time to -60 dB below sustain: %s\n",
                t60 < 0 ? "NEVER" : (std::to_string((long) std::lround(1000.0 * (t60 - nPre) / kSr)) + " ms").c_str());
    writeWav("miku_v1_tail.wav", y, kSr);
}
#endif

// ===========================================================================
// T9 — CPU. The nasal branch adds a pole + a zero to EVERY sample (they cancel
// when not nasalised, but they still run), plus a second frication pole and a
// stop-aspiration highpass. Prove that did not cost the budget.
// ===========================================================================
void t9_cpu() {
    std::printf("\n=== T9  CPU (60 s of audio through the core) ===\n");
    // 5 s of picking, replayed 12x = 60 s of audio through the core.
    std::vector<float> base((size_t) (5.0 * kSr), 0.0f);
    for (int k = 0; k < 9; ++k)
        pluck(base, (int) (k * 0.5 * kSr), 110.0 + 7.0 * (k % 13), 1.4, 0.3f);
    for (int block : { 64, 256, 1024 }) {
        Core c;
        makeCore(c, 9);
        std::vector<float> in = base;
        const auto t0 = std::chrono::steady_clock::now();
        for (int rep = 0; rep < 12; ++rep) {
            std::copy(base.begin(), base.end(), in.begin());
            for (int i = 0; i < (int) in.size(); i += block) {
                const int n = std::min(block, (int) in.size() - i);
                float* ch[1] = { in.data() + i };
                c.process(ch, 1, n);
            }
        }
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  block %4d:  %6.3f s wall for 60 s audio  ->  %.2f %% of one core\n",
                    block, sec, 100.0 * sec / 60.0);
    }
}

// ===========================================================================
// T10 — RT contract. The nasal antiresonator's leading coefficient is ~700, so
// a garbage input has a high-gain path to the output now. The core's promise is
// "always finite, |y| <= 8, and it RECOVERS". Prove it still holds.
// ===========================================================================
void t10_abuse() {
    std::printf("\n=== T10  ABUSE / RT CONTRACT ===\n");
    struct A { const char* name; float (*gen)(int); };
    static const A cases[] = {
        { "NaN",        [](int)   -> float { return std::numeric_limits<float>::quiet_NaN(); } },
        { "+Inf",       [](int)   -> float { return std::numeric_limits<float>::infinity(); } },
        { "1e6 DC",     [](int)   -> float { return 1e6f; } },
        { "full square",[](int i)  -> float { return ((i / 120) % 2) ? 1.0f : -1.0f; } },
        { "sweep",      [](int i)  -> float {
              const double t = (double) i / kSr;
              return 0.9f * (float) std::sin(2.0 * kPi * (30.0 + 8000.0 * t) * t); } },
        { "denormals",  [](int)   -> float { return 1e-38f; } },
    };
    for (const A& a : cases) {
        std::vector<float> in((size_t) (1.5 * kSr));
        for (int i = 0; i < (int) in.size(); ++i) in[(size_t) i] = a.gen(i);
        // then 0.5 s of a clean pluck: does it RECOVER?
        std::vector<float> tail((size_t) (1.0 * kSr), 0.0f);
        pluck(tail, 0, 130.81, 1.5, 0.35f);
        in.insert(in.end(), tail.begin(), tail.end());

        Core c;
        makeCore(c, 5);
        std::vector<float> y = run(c, in);
        bool finite = true; double peak = 0.0;
        for (float v : y) { if (!std::isfinite(v)) finite = false; peak = std::max(peak, (double) std::fabs(v)); }
        const double after = rmsOf(y, (int) (1.7 * kSr), (int) (2.2 * kSr));   // the clean pluck
        std::printf("  %-12s finite=%s  peak=%6.2f (limit 8)  recovers=%s (%.1f dBFS on the clean pluck)\n",
                    a.name, finite ? "yes" : "NO ", peak,
                    (after > 1e-4) ? "yes" : "NO ", db(after));
    }
}

// ===========================================================================
// T11 — PHRASE ORDER. The index is "syllable LAST sung" with a pre-increment
// advance, so a fresh phrase must start at mora 0. It used to start at 0 and
// therefore SANG table[1] first: "nyan" opened on the /N/ hum, Scat on "bi",
// every user phrase on its second mora. Also: turning the pattern knob or
// re-loading the active Phrase slot must restart the phrase.
// ===========================================================================
void t11_phrase_order() {
    std::printf("\n=== T11  PHRASE ORDER (first pick sings mora 0; knob restarts) ===\n");
    auto picks = [&](int n, double spacing) {
        std::vector<float> in((size_t) ((n * spacing + 0.8) * kSr), 0.0f);
        for (int k = 0; k < n; ++k)
            pluck(in, (int) (k * spacing * kSr), 130.81 * (1.0 + 0.05 * (k % 3)), 1.2, 0.35f);
        return in;
    };
    struct P { const char* name; int pattern; const char* lyric; int len; };
    static const P cases[] = {
        { "nyan (pattern 4)", 4, nullptr, 2 },
        { "scat (pattern 5)", 5, nullptr, 6 },
        { "phrase 'ko nn ni chi wa'", 6, "ko nn ni chi wa", 5 },
    };
    for (const P& p : cases) {
        Core c;
        makeCore(c, p.pattern, 0.0f);
        if (p.lyric != nullptr) {
            Syl syl[Core::kMaxSyllables];
            const int n = Core::parsePhrases(p.lyric, syl, Core::kMaxSyllables);
            c.setPhrase(0, syl, n);
        }
        std::vector<Event> ev;
        run(c, picks(p.len + 2, 0.5), &ev);
        std::printf("  %-28s sung order:", p.name);
        bool ok = !ev.empty();
        for (int i = 0; i < (int) ev.size(); ++i) {
            std::printf(" %d", ev[(size_t) i].syl);
            if (ev[(size_t) i].syl != i % p.len) ok = false;
        }
        std::printf("   %s\n", ok ? "ok (starts at mora 0)" : "<-- WRONG ORDER");
    }
    // Knob change mid-phrase: 2 picks on Nyan, switch to Scat, 2 picks.
    {
        Core c;
        makeCore(c, 4, 0.0f);
        std::vector<float> in = picks(4, 0.5);
        std::vector<Event> ev;
        const int block = 128;
        std::vector<float> y = in;
        int last = c.syllableIndex();
        for (int i = 0; i < (int) y.size(); i += block) {
            if (i == (int) (0.9 * kSr)) {   // between pick 2 and pick 3
                dcdsp::MikuStompParams pp;
                pp.pattern = 5.0f;   // Scat
                pp.vibrato = 0.0f;
                Core tmp;            // setParams clamps; reuse makeCore defaults
                (void) tmp;
                dcdsp::MikuStompParams q;
                q.mix = 1.0f; q.character = 0.5f; q.breath = 0.35f; q.attackMs = 12.0f;
                q.portamentoMs = 50.0f; q.vibrato = 0.0f; q.vibratoRateHz = 5.8f;
                q.octave = 1.0f; q.pattern = 5.0f;
                c.setParams(q);
            }
            const int n = std::min(block, (int) y.size() - i);
            float* ch[1] = { y.data() + i };
            c.process(ch, 1, n);
            const int s = c.syllableIndex();
            if (s != last) { ev.push_back({ 1000.0 * i / kSr, s, c.trackedHz() }); last = s; }
        }
        std::printf("  knob Nyan->Scat mid-phrase   sung order:");
        for (const Event& e : ev) std::printf(" %d", e.syl);
        const bool restart = ev.size() >= 3 && ev[2].syl == 0;
        std::printf("   %s\n", restart ? "ok (new pattern restarts at mora 0)"
                                       : "<-- pattern change must restart the phrase");
    }
}

// ===========================================================================
// T12 — THE FULL MORAIC MATRIX: every consonant x every vowel (110 cells).
// T2 sweeps consonants against /a/ only; the vowel matters (the velar pinch,
// /u/'s low F2, /i/'s high one), so a letter can pass on /a/ and fail on /o/.
// Per class cue (same thresholds as T2, plus the transition where one is due):
//   stop      closure < -12 dB, burst > -4 dB vs vowel
//   fric/affr HF 2-8k  > +12 dB over the bare vowel
//   aspirate  HF        > +8 dB
//   nasal     murmur    > +8 dB darker than the bare vowel
//   approx    (level cues only; the cue is the glide, checked in T3/T16)
// Prints only FAILING cells + a per-class tally, so a regression is one line.
// ===========================================================================
void t12_matrix() {
    std::printf("\n=== T12  FULL 22-CONSONANT x 5-VOWEL MATRIX ===\n");
    static const char* cn[] = { "",  "k", "g", "s", "sh", "z", "j", "t", "ts", "ch", "d",
                                "n", "ny", "h", "f", "b", "p", "m", "y", "r", "w", "N" };
    static const char* vn[] = { "a", "i", "u", "e", "o" };
    static const int kindOf[] = { 0, 1, 1, 2, 2, 2, 3, 1, 3, 3, 1,
                                  4, 4, 5, 2, 1, 1, 4, 6, 6, 6, 4 };
    // per-vowel bare-vowel references
    double refFric[5], refDark[5], refVow[5];
    int fails = 0, cells = 0;
    for (int v = 0; v < 5; ++v) {
        for (int cIdx = 0; cIdx < Core::kNumCons; ++cIdx) {
            std::vector<float> in((size_t) (1.2 * kSr), 0.0f);
            pluck(in, (int) (0.05 * kSr), 130.81, 2.5, 0.35f);
            Core c;
            makeCore(c, 6, 0.0f);
            const Syl s{ cIdx, v };
            c.setPhrase(0, &s, 1);
            std::vector<float> y = run(c, in);

            const int on = (int) (0.05 * kSr);
            const int steady = on + (int) (0.35 * kSr);
            const double vow = rmsOf(y, steady, steady + (int) (0.2 * kSr));
            double quiet = 1e9;
            for (int i = on; i < on + (int) (0.06 * kSr); i += (int) (0.005 * kSr))
                quiet = std::min(quiet, rmsOf(y, i, i + (int) (0.01 * kSr)));
            double burst = 0.0;
            for (int i = on; i < on + (int) (0.11 * kSr); i += (int) (0.002 * kSr))
                burst = std::max(burst, rmsOf(y, i, i + (int) (0.008 * kSr)));
            const double fric = bandEnergy(y, on, on + (int) (0.09 * kSr), 2000.0, 8000.0);
            const double lf = bandEnergy(y, on + (int) (0.01 * kSr), on + (int) (0.05 * kSr), 150.0, 500.0);
            const double hf = bandEnergy(y, on + (int) (0.01 * kSr), on + (int) (0.05 * kSr), 1500.0, 6000.0);

            if (cIdx == Core::C_None) {
                refFric[v] = db(fric); refDark[v] = db(lf) - db(hf); refVow[v] = vow;
                continue;
            }
            ++cells;
            const double hfC = db(fric) - refFric[v];
            const double dkC = (db(lf) - db(hf)) - refDark[v];
            const char* why = nullptr;
            char buf[96];
            switch (kindOf[cIdx]) {
                case 1:   // stop
                    if (db(quiet) - db(vow) > -12.0) { std::snprintf(buf, sizeof buf, "no closure (%+.0f dB)", db(quiet) - db(vow)); why = buf; }
                    else if (db(burst) - db(vow) < -4.0) { std::snprintf(buf, sizeof buf, "burst inaudible (%+.0f dB)", db(burst) - db(vow)); why = buf; }
                    break;
                case 2: case 3:   // fricative / affricate
                    if (hfC < 12.0) { std::snprintf(buf, sizeof buf, "frication weak (%+.0f dB HF)", hfC); why = buf; }
                    break;
                case 5: {  // aspirate: /h/ is the FOLLOWING vowel's formants excited
                           // by noise, so before /o/ its energy is at F1/F2 (~0.5-1 kHz),
                           // not 2-8 kHz. Measure the pre-voice noise IN THE VOWEL'S OWN
                           // BAND vs the bare vowel's onset — an HF-only metric flagged
                           // /he,ho/ while the aspiration was there and audible.
                    const double preVoice = db(bandEnergy(y, on, on + (int) (0.05 * kSr), 300.0, 3500.0));
                    const double refPre   = db(bandEnergy(y, steady, steady + (int) (0.05 * kSr), 300.0, 3500.0));
                    if (preVoice - refPre < -14.0) {
                        std::snprintf(buf, sizeof buf, "no aspiration (%+.0f dB in-band pre-voice)", preVoice - refPre);
                        why = buf;
                    }
                    break;
                }
                case 4:   // nasal: the murmur must be dark IN ITSELF (pole 302 Hz,
                          // zero kills the mids). Contrast against the following
                          // vowel is the wrong reference: /i/'s own LF/HF is ~16 dB
                          // darker than /a/'s (F2 3 kHz vs 1.65 kHz against the
                          // falling source), so 'ni' flagged while its murmur was
                          // identical to 'na''s.
                    if (db(lf) - db(hf) < 12.0) { std::snprintf(buf, sizeof buf, "murmur not dark (%+.0f dB LF/HF abs)", db(lf) - db(hf)); why = buf; }
                    break;
                default: break;   // approximants: level cues carry nothing here
            }
            if (why != nullptr) {
                ++fails;
                std::printf("  FAIL %s%s: %s\n", cn[cIdx], vn[v], why);
            }
        }
    }
    std::printf("  %d/%d consonant-vowel cells pass their class cue%s\n",
                cells - fails, cells, fails == 0 ? "" : "  <-- see FAIL lines");
}

// ===========================================================================
// T13 — THE /o/ MYSTERY PEAK. T4 shows an LPC peak at ~1730 Hz on /o/ that is
// in neither the table (F2 918, F3 3132) nor any other vowel. Two suspects:
// output-stage distortion (the adaaTanh runs at kMakeup * amp ~ saturating) or
// an LPC fitting artifact in /o/'s wide F2..F3 valley. The drive-ratio test
// separates them: render the SAME vowel at full and at -12 dB input; a
// nonlinear product falls ~2 dB per input dB (3rd order) relative to the
// signal, a linear/measurement feature stays put.
// ===========================================================================
void t13_o_probe() {
    std::printf("\n=== T13  /o/ SPECTRUM vs DRIVE (distortion or artifact?) ===\n");
    const double f0 = 130.81;
    for (int v : { 4, 0 }) {   // /o/ and the /a/ control
        std::printf("  vowel /%s/:\n", v == 4 ? "o" : "a");
        double gapHi = 0, gapLo = 0, f2Hi = 0, f2Lo = 0;
        for (double amp : { 0.35, 0.09 }) {
            std::vector<float> in((size_t) (1.2 * kSr), 0.0f);
            pluck(in, (int) (0.05 * kSr), f0, 3.0, (float) amp);
            Core c;
            makeCore(c, 6, 0.0f, 0.35f, /*octave*/ 0.0f);
            const Syl s{ Core::C_None, v };
            c.setPhrase(0, &s, 1);
            std::vector<float> y = run(c, in);
            // harmonic levels over the steady vowel
            const int a = (int) (0.40 * kSr), b = (int) (0.90 * kSr);
            auto hLevel = [&](int n) { return db(bandEnergy(y, a, b, n * f0 - 8.0, n * f0 + 8.0)); };
            // F2 region: harmonic nearest 918*1.08 = 992 -> n=8 (1046)/n=7 (916);
            // the valley/mystery region 1600-1900 -> n=13 (1700). F1 n=4 (523).
            const double h7 = hLevel(7), h13 = hLevel(13), h14 = hLevel(14);
            const double gap = std::max(h13, h14) - h7;
            std::vector<Frame> fr = track(y);
            double p2 = 0;
            for (const Frame& f : fr)
                if (f.tMs > 400.0 && f.tMs < 450.0 && f.nf >= 2) { p2 = f.f[1]; break; }
            std::printf("    in %5.2f: F2-harm %.0f dB, 1.7-1.9k harms %.0f dB (gap %+5.1f dB), LPC peaks:",
                        amp, h7, std::max(h13, h14), gap);
            for (int i = 0; i < g_allPeakCount && i < 6; ++i) std::printf(" %.0f", g_allPeaks[i]);
            std::printf("\n");
            if (amp > 0.2) { gapHi = gap; f2Hi = p2; } else { gapLo = gap; f2Lo = p2; }
        }
        (void) f2Hi; (void) f2Lo;
        std::printf("    valley-vs-F2 moved %+.1f dB for -11.8 dB of drive -> %s\n",
                    gapHi - gapLo,
                    (gapHi - gapLo > 6.0) ? "DISTORTION (nonlinear)" :
                    (gapHi - gapLo < -6.0) ? "level-dependent, inverse (?)" :
                    "linear: synthesis envelope / LPC artifact, not distortion");
    }
}

// ===========================================================================
// T14 — VOWEL IDENTITY ACROSS THE FRETBOARD. A vowel is its spectral envelope;
// a high fundamental samples that envelope sparsely and identity can collapse.
// Metric: 1/3-octave band profile (250-3800 Hz) of each vowel at each guitar
// f0, classified nearest-neighbour against the five profiles at f0 = 110 Hz.
// The confusion (if any) is the intelligibility ceiling at that pitch.
// ===========================================================================
void t14_vowels_vs_pitch() {
    std::printf("\n=== T14  VOWEL IDENTITY vs PITCH (octave +1, sung f0 = 2x guitar) ===\n");
    static const char* vn[] = { "a", "i", "u", "e", "o" };
    const int nb = 16;
    auto profile = [&](int v, double f0) {
        std::vector<float> in((size_t) (1.4 * kSr), 0.0f);
        pluck(in, (int) (0.05 * kSr), f0, 3.0, 0.35f);
        Core c;
        makeCore(c, 6, 0.0f);   // octave +1 (the default players use)
        const Syl s{ Core::C_None, v };
        c.setPhrase(0, &s, 1);
        std::vector<float> y = run(c, in);
        std::vector<double> p((size_t) nb);
        const int a = (int) (0.40 * kSr), b = (int) (1.00 * kSr);
        for (int k = 0; k < nb; ++k) {
            const double lo = 250.0 * std::pow(3800.0 / 250.0, (double) k / nb);
            const double hi = 250.0 * std::pow(3800.0 / 250.0, (double) (k + 1) / nb);
            p[(size_t) k] = db(bandEnergy(y, a, b, lo, hi));
        }
        // normalise: profiles compare shape, not level
        double m = 0;
        for (double x : p) m += x;
        m /= nb;
        for (double& x : p) x -= m;
        return p;
    };
    std::vector<std::vector<double>> ref;
    for (int v = 0; v < 5; ++v) ref.push_back(profile(v, 110.0));
    std::printf("  guitar f0    sung f0   a->  i->  u->  e->  o->   (margin dB; X = misclassified)\n");
    for (double f0 : { 82.41, 110.0, 164.81, 246.94, 329.63 }) {
        std::printf("  %7.1f Hz  %6.0f Hz  ", f0, 2.0 * f0);
        for (int v = 0; v < 5; ++v) {
            const auto p = profile(v, f0);
            double bestD = 1e18, secondD = 1e18;
            int best = -1;
            for (int r = 0; r < 5; ++r) {
                double d = 0;
                for (int k = 0; k < nb; ++k) d += (p[(size_t) k] - ref[(size_t) r][(size_t) k])
                                                * (p[(size_t) k] - ref[(size_t) r][(size_t) k]);
                if (d < bestD) { secondD = bestD; bestD = d; best = r; }
                else if (d < secondD) { secondD = d; }
            }
            const double margin = std::sqrt(secondD / nb) - std::sqrt(bestD / nb);
            if (best == v) std::printf(" %3.0f ", margin);
            else std::printf("  X%s ", vn[best]);
        }
        std::printf("\n");
    }
    std::printf("  (number = dB margin to the nearest WRONG vowel; X? = heard as ?)\n");
}

// ===========================================================================
// T15 — FAST RIFFS: does the sequencer keep up, skip, or double-fire?
// The refractory is 60 ms, so 16 picks/s (62.5 ms apart) is the design edge.
// ===========================================================================
void t15_fast_riffs() {
    std::printf("\n=== T15  FAST RIFFS (advances per pick; no doubles) ===\n");
    std::printf("  rate     picks  advances  doubled   verdict\n");
    for (double rate : { 6.0, 10.0, 12.0, 16.0 }) {
        const double spacing = 1.0 / rate;
        const int n = (int) (3.0 * rate);
        std::vector<float> in((size_t) (3.6 * kSr), 0.0f);
        for (int k = 0; k < n; ++k)
            pluck(in, (int) (k * spacing * kSr), (k % 2) ? 146.83 : 110.0, 0.8, 0.35f);
        Core c;
        makeCore(c, 9, 0.0f);
        std::vector<Event> ev;
        run(c, in, &ev);
        int doubled = 0;
        for (size_t i = 1; i < ev.size(); ++i)
            if (ev[i].tMs - ev[i - 1].tMs < 0.5 * spacing * 1000.0 * 0.9) ++doubled;
        const double pct = 100.0 * (double) ev.size() / n;
        std::printf("  %4.0f/s   %5d  %8d  %7d   %s\n", rate, n, (int) ev.size(), doubled,
                    doubled > 0 ? "<-- DOUBLE-FIRE" : (pct < 75.0 ? "<-- SKIPPING" : "ok"));
    }
}

// ===========================================================================
// T16 — FRICATION RISE TIME: the fricative/affricate contrast. Howell & Rosen
// (JASA 1983): fricatives rise slowly (~76 ms), affricates fast (~33 ms); the
// rise time IS the cue that separates /s/ from /ts/ and /sh/ from /ch/ beyond
// the closure. Both classes sharing one attack coefficient erases it.
// ===========================================================================
void t16_rise_time() {
    std::printf("\n=== T16  FRICATION RISE TIME (fricative slow vs affricate fast) ===\n");
    struct R { const char* n; int c; bool affric; };
    static const R rows[] = {
        { "sa",  Core::C_S,  false }, { "tsa", Core::C_TS, true },
        { "sha", Core::C_SH, false }, { "cha", Core::C_CH, true },
    };
    double fricRise = 0, affrRise = 0;
    for (const R& row : rows) {
        std::vector<float> in((size_t) (1.0 * kSr), 0.0f);
        pluck(in, (int) (0.05 * kSr), 130.81, 2.0, 0.35f);
        Core c;
        makeCore(c, 6, 0.0f);
        const Syl s{ row.c, Core::V_A };
        c.setPhrase(0, &s, 1);
        std::vector<float> y = run(c, in);
        // HF envelope in 2 ms hops over the consonant. The reference level is the
        // 85th percentile, NOT the max: frication is noise, and chasing a single
        // stochastic peak inflated every rise time by tens of ms.
        const int on = (int) (0.05 * kSr);
        const int hop = (int) (0.002 * kSr);
        std::vector<double> env;
        for (int i = on; i < on + (int) (0.16 * kSr); i += hop)
            env.push_back(bandEnergy(y, i, i + hop, 2500.0, 9000.0));
        std::vector<double> sorted = env;
        std::sort(sorted.begin(), sorted.end());
        const double ref = sorted[sorted.size() * 85 / 100];
        size_t t10 = env.size(), t80 = env.size();
        for (size_t i = 0; i < env.size(); ++i) {
            if (t10 == env.size() && env[i] >= 0.10 * ref) t10 = i;
            if (t80 == env.size() && env[i] >= 0.80 * ref) { t80 = i; break; }
        }
        const double rise = (t80 > t10 && t80 < env.size())
                          ? (double) (t80 - t10) * hop * 1000.0 / kSr : 0.0;
        if (row.affric) affrRise += rise; else fricRise += rise;
        std::printf("  %-4s %s  10-90%% rise %5.1f ms\n", row.n, row.affric ? "affricate" : "fricative", rise);
    }
    fricRise *= 0.5; affrRise *= 0.5;
    std::printf("  mean fricative %.1f ms vs affricate %.1f ms -> %s\n", fricRise, affrRise,
                (affrRise < 0.6 * fricRise) ? "contrast present"
                                            : "<-- NO CONTRAST: affricates must rise faster");
}

} // namespace

int main() {
    std::printf("mikustomp — objective measurements @ %.0f Hz\n", kSr);
    t0_antiresonator();

    std::vector<Syl> nyan = { { Core::C_NY, Core::V_A }, { Core::C_NN, Core::V_U } };
    t1_tail(4, "nyan", nullptr);
    t1_tail(0, "ahh", nullptr);
    t1_tail(9, "random1", nullptr);

    t6_natural_and_noise();
    t7_sustain_am();
    t2_phonemes();
    t3_coarticulation();
    t4_vowels();
    t11_phrase_order();
    t12_matrix();
    t13_o_probe();
    t14_vowels_vs_pitch();
    t15_fast_riffs();
    t16_rise_time();
#ifdef MIKU_WITH_V1
    t8_v1_tail();
#endif
    t9_cpu();
    t10_abuse();
    t5_audition();
    std::printf("\ndone.\n");
    return 0;
}
