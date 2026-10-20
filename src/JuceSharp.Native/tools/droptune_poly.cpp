// droptune_poly.cpp — the POLYPHONIC drop-tuner harness.
//
// droptune_measure.cpp measures a SINGLE SINE. Everything it reported was excellent
// and the pedal still sounded bad on chords, so the gap was somewhere a sine cannot
// show. This harness looks there. It found the bug (see below) and it now exists to
// stop anyone re-introducing it.
//
// WHAT IT DOES THAT THE SINE HARNESS DOES NOT
//
//  * REAL MATERIAL — stiff-string plucked chords (per-partial inharmonicity
//    B = 5e-5..2.4e-4, pluck-comb partial amplitudes, per-partial decay, pick
//    noise, per-string detune, strum stagger), plus a real guitar recording.
//  * IT WATCHES THE SHIFTER'S DECISIONS. We open the core read-only
//    (`#define private public`) and log every splice, every transient anchor and the
//    period the adaptive ceiling has locked onto: when, what lag, what score. That is
//    what turned "it sounds bad" into "the correlation splice never ran".
//  * A COUNTERFACTUAL. The same run with the transient anchor suppressed (harness
//    side: retrigger_ pushed out of reach — the DSP file is untouched). If an
//    artifact disappears when a mechanism is disabled, that mechanism is the cause.
//  * AN ORACLE. For each chord, the best normalized cross-correlation ACHIEVABLE at
//    each splice-lag ceiling. This separates "the correlator chose badly" from "no
//    good lag EXISTS below the ceiling" — and it is what proved the ceiling had to
//    become adaptive.
//  * AN A/B CEILING. Every chord is also synthesized natively at the shifted pitch,
//    same seed, same performance — a physically down-tuned guitar. The only honest
//    reference.
//
// ===========================================================================
// WHAT IT FOUND (0.19). Keep these numbers: they are the regression guard.
//
//  1. `updateOnset` was a CREST-FACTOR detector, not an onset detector. It compared a
//     0.5 ms-release PEAK follower against 2.5x a 200 ms MEAN *of the same signal at
//     the same instant* — nothing in it asked whether the level went UP — and the
//     0.5 ms release re-armed every waveform period below 500 Hz. Measured: ~20
//     anchors/second against <=2 real picks/second. Each false anchor did an unmatched
//     jump AND cleared `pending_`, cancelling the pending correlation splice: the
//     mechanism the core's header calls "what makes it sound good" ran at 0.0
//     splices/s on EVERY chord. A sine has no HF crest and cannot trigger it, which is
//     exactly why the sine harness measured -43 dBc warble and declared victory.
//     FIXED by comparing the fast HF envelope against the SAME envelope ~30 ms ago and
//     requiring an actual RISE. Section 1 now reads ~0.2 anchors/s (= the real picks).
//  2. The lag CEILING was fixed, and every fixed value is wrong for something. A power
//     chord is NOT periodic at the root period (a fifth at 1.5*f0 is not a harmonic of
//     f0: lcm = 2T), so E5/Emaj/Emin need 24.3 ms, open G needs 40.8 ms and a bare
//     major third needs 48.3 ms — while "Tight" was 10 ms, below one period of even a
//     single low E (12.1 ms). FIXED by measuring the material's period and raising the
//     ceiling to it (section 1's `period` column); the knob is now a FLOOR.
//  3. The splice crossfade was power-complementary (cos/sin). A matched splice blends
//     two nearly IDENTICAL waveforms, and cos+sin sums correlated content to +3 dB in
//     the middle of the fade — an amplitude bump at exactly the splice rate. FIXED by
//     going linear: worth 9.6-20.8 dB of sine warble and 1-10 dB of chord SER.
//
// THE METRIC THAT MATTERS: SER (artifact-to-tonal ratio). A sustained chord is a
// line spectrum; an ideal pitch shift is still a line spectrum, just lower. Every
// splice, every glitch, every warble sideband lands BETWEEN the lines. SER = the
// energy that is not on a partial, over the energy that is. The native reference
// scores about -70 dB. That is the ceiling, and the distance to it is the artifact.
//
// Build (from JuceSharp/src/JuceSharp.Native):
//   cl /std:c++17 /O2 /EHsc /I src tools\droptune_poly.cpp /Fe:droptune_poly.exe
//   g++ -std=c++17 -O2 -I src tools/droptune_poly.cpp -o droptune_poly
//
// Usage:
//   droptune_poly                 the report
//   droptune_poly --wav <dir>     also dump audio (dry / shifted / native reference)
//   droptune_poly --di <file.wav> [--di <more.wav> ...]
//                                 ALSO run the real-material section on external
//                                 recordings (real DI guitar). Any sample rate;
//                                 resampled to 48 kHz with the shifter's own sinc.
//                                 With --wav, dumps dry + shifted A/B for each.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#define private public
#include "dsp/cores/dcdsp_droptune.h"
#undef private

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSR = 48000.0;

// ---------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------
using cd = std::complex<double>;

void fft(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / (double) len * (inverse ? 1.0 : -1.0);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k];
                const cd v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= (double) n;
}

size_t pow2Below(size_t n) {
    size_t p = 1;
    while (p * 2 <= n) p *= 2;
    return p;
}

double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-15)); }
double dB10(double x) { return 10.0 * std::log10(std::max(x, 1e-20)); }

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------
bool readWavMono(const std::string& path, std::vector<float>& out, double& sr) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw((size_t) len);
    const bool ok = std::fread(raw.data(), 1, (size_t) len, f) == (size_t) len;
    std::fclose(f);
    if (!ok || len < 44 || std::memcmp(raw.data(), "RIFF", 4) != 0) return false;

    size_t p = 12;
    int channels = 1, bits = 16, fmt = 1;
    sr = 44100.0;
    while (p + 8 <= (size_t) len) {
        char id[5] = {0};
        std::memcpy(id, &raw[p], 4);
        uint32_t sz;
        std::memcpy(&sz, &raw[p + 4], 4);
        const size_t body = p + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= (size_t) len) {
            uint16_t f16;
            std::memcpy(&f16, &raw[body], 2); fmt = f16;
            std::memcpy(&f16, &raw[body + 2], 2); channels = f16;
            uint32_t s32;
            std::memcpy(&s32, &raw[body + 4], 4); sr = (double) s32;
            std::memcpy(&f16, &raw[body + 14], 2); bits = f16;
        } else if (std::memcmp(id, "data", 4) == 0) {
            const size_t bytes = std::min<size_t>(sz, (size_t) len - body);
            const int bps = bits / 8;
            if (bps <= 0 || channels <= 0) return false;
            const size_t frames = bytes / (size_t) (bps * channels);
            out.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                double acc = 0;
                for (int c = 0; c < channels; ++c) {
                    const size_t off = body + (i * (size_t) channels + (size_t) c) * (size_t) bps;
                    if (fmt == 3 && bits == 32) { float v; std::memcpy(&v, &raw[off], 4); acc += v; }
                    else if (bits == 16)        { int16_t v; std::memcpy(&v, &raw[off], 2); acc += v / 32768.0; }
                    else if (bits == 24)        { const int32_t v = (int32_t) ((raw[off] << 8) | (raw[off + 1] << 16) | (raw[off + 2] << 24)) >> 8; acc += v / 8388608.0; }
                    else if (bits == 32)        { int32_t v; std::memcpy(&v, &raw[off], 4); acc += v / 2147483648.0; }
                }
                out[i] = (float) (acc / channels);
            }
            return true;
        }
        p = body + sz + (sz & 1u);
    }
    return false;
}

void writeWav(const std::string& path, const std::vector<float>& x, double sr) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t n = (uint32_t) x.size();
    const uint32_t dataBytes = n * 2;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
    u32((uint32_t) sr); u32((uint32_t) sr * 2); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    for (float v : x) {
        const int s = (int) std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f);
        u16((uint16_t) (int16_t) s);
    }
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// Guitar synthesis — stiff string, plucked.
//   f_n = n*f0*sqrt(1 + B n^2)   B: 5e-5 plain .. 2.4e-4 wound low E
//   A_n = |sin(n pi p)| / n^2    ideal pluck at position p, + pickup roll-off
//   per-partial decay (HF dies first), pick noise, per-string detune, strum stagger
// ---------------------------------------------------------------------------
struct StringNote { int midi; double startSec; };

double midiHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

double stiffnessFor(int midi) {
    if (midi <= 45) return 2.4e-4;
    if (midi <= 52) return 1.5e-4;
    if (midi <= 59) return 8.0e-5;
    return 5.0e-5;
}

/// pitchRatio 1.0 = source. r = the A/B reference: the SAME performance on a guitar
/// physically tuned down (same strings, same stiffness — only f0 moves).
std::vector<float> synthChord(const std::vector<StringNote>& notes, double seconds,
                              bool sustained, double pitchRatio, unsigned seed) {
    const size_t N = (size_t) (seconds * kSR);
    std::vector<double> acc(N, 0.0);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    for (const auto& note : notes) {
        const double cents = (uni(rng) - 0.5) * 6.0;
        const double f0 = midiHz(note.midi) * pitchRatio * std::pow(2.0, cents / 1200.0);
        const double B = stiffnessFor(note.midi);
        const double pluckPos = 0.12 + 0.04 * uni(rng);
        const size_t start = sustained ? 0 : (size_t) (note.startSec * kSR);
        if (start >= N) continue;

        for (int n = 1; n <= 48; ++n) {
            const double fn = (double) n * f0 * std::sqrt(1.0 + B * (double) (n * n));
            if (fn > 0.45 * kSR) break;
            double amp = std::abs(std::sin((double) n * kPi * pluckPos)) / ((double) n * (double) n);
            amp /= std::sqrt(1.0 + (fn / 2500.0) * (fn / 2500.0));
            amp *= 1.0 + 0.30 * std::exp(-std::pow((fn - 2800.0) / 900.0, 2.0));  // pickup resonance
            if (amp < 1e-5) continue;

            const double tau = 2.6 / (1.0 + fn / 700.0);
            const double ph = uni(rng) * 2.0 * kPi;
            const double w = 2.0 * kPi * fn / kSR;
            for (size_t i = start; i < N; ++i) {
                const double t = (double) (i - start) / kSR;
                double env;
                if (sustained) {
                    env = t < 0.025 ? 0.5 - 0.5 * std::cos(kPi * t / 0.025) : 1.0;
                } else {
                    const double atk = t < 0.002 ? 0.5 - 0.5 * std::cos(kPi * t / 0.002) : 1.0;
                    env = atk * std::exp(-t / tau);
                    // Only give up once the note has actually decayed — the attack
                    // ramp starts AT zero, and breaking on that kills every partial.
                    if (t > 0.005 && env < 1e-4) break;
                }
                acc[i] += amp * env * std::sin(w * (double) (i - start) + ph);
            }
        }

        if (!sustained) {  // pick noise — the transient test is about whether it survives
            std::normal_distribution<double> gauss(0.0, 1.0);
            double z1 = 0, z2 = 0;
            const size_t nb = (size_t) (0.006 * kSR);
            for (size_t i = 0; i < nb && start + i < N; ++i) {
                const double e = std::exp(-((double) i / kSR) / 0.0015);
                const double v = gauss(rng);
                z1 = 0.55 * z1 + 0.45 * v;
                const double hp = v - z1;
                z2 = 0.7 * z2 + 0.3 * hp;
                acc[start + i] += 0.12 * e * z2;
            }
        }
    }

    double peak = 1e-9;
    for (double v : acc) peak = std::max(peak, std::abs(v));
    std::vector<float> out(N);
    const double g = 0.5 / peak;
    for (size_t i = 0; i < N; ++i) out[i] = (float) (acc[i] * g);
    return out;
}

std::vector<float> sine(double f, double seconds, double amp = 0.5) {
    const size_t n = (size_t) (seconds * kSR);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = (float) (amp * std::sin(2.0 * kPi * f * (double) i / kSR));
    return x;
}

// ---------------------------------------------------------------------------
// Run the shifter, logging every decision it makes.
// ---------------------------------------------------------------------------
struct Event { size_t sample; int lag; bool anchor; };

struct RunResult {
    std::vector<float> y;
    std::vector<Event> events;
    int splices = 0, anchors = 0, onsets = 0;
    double spliceRateHz = 0, anchorRateHz = 0, onsetRateHz = 0;
    double meanLagMs = 0, lagSdMs = 0;
    double meanAnchorJumpMs = 0, maxAnchorJumpMs = 0;
    double periodMs = 0;   // what the periodicity probe locked onto (the adaptive ceiling)
    double effLagMs = 0;   // the ceiling the splice scheduler actually used
    double meanDelayMs = 0;// mean sustain delay — the cost of a bigger ceiling
};

/// suppressAnchors: push retrigger_ beyond sinceOnset_'s initial value so the onset
/// detector can never fire. The DSP file is NOT modified — this is the harness
/// reaching into the object to run a counterfactual.
RunResult runShifter(const std::vector<float>& mono, float semitones, float maxLagMs,
                     bool suppressAnchors = false) {
    RunResult res;
    dcdsp::DropTune dt;
    dt.prepare((float) kSR, 512, 2);
    dcdsp::DropTuneParams p;
    p.semitones = semitones;
    p.maxLagMs = maxLagMs;
    dt.setParams(p);
    if (suppressAnchors) dt.retrigger_ = 1 << 25;   // sinceOnset_ starts at 1<<20 < this

    std::vector<float> l = mono, r = mono;
    int prevFade = 0;
    double delaySum = 0;
    size_t delayN = 0;
    const size_t settle = (size_t) (0.5 * kSR);   // skip engage
    // Per-sample so the internal state can be read around every event. The core is a
    // per-sample loop, so this is functionally identical to a 128-frame block.
    for (size_t i = 0; i < mono.size(); ++i) {
        float* bp[2] = {l.data() + i, r.data() + i};
        dt.process(bp, 2, 1);
        // sinceOnset_ == 0 on exactly the sample the detector fires, whether or not
        // there was any delay for the anchor to discharge. This is the DETECTOR's
        // verdict; `anchors` below is only the subset that moved the read head.
        if (dt.sinceOnset_ == 0) ++res.onsets;
        if (prevFade == 0 && dt.fade_ > 0)
            res.events.push_back({i, (int) std::lround(dt.r_ - dt.rOld_), dt.fadeLen_ == dt.snapXfade_});
        prevFade = dt.fade_;
        if (i >= settle && !dt.bypassed_) { delaySum += (double) dt.w_ - dt.r_; ++delayN; }
    }
    res.y = l;
    res.periodMs = dt.periodLag_ * 1000.0 / kSR;
    res.effLagMs = dt.effLag() * 1000.0 / kSR;
    res.meanDelayMs = delayN ? delaySum / (double) delayN * 1000.0 / kSR : 0.0;

    const double dur = (double) mono.size() / kSR;
    std::vector<double> lags, jumps;
    for (const auto& e : res.events) {
        if (e.anchor) { ++res.anchors; jumps.push_back(e.lag * 1000.0 / kSR); }
        else          { ++res.splices; lags.push_back(e.lag * 1000.0 / kSR); }
    }
    auto mean = [](const std::vector<double>& v) {
        double a = 0; for (double x : v) a += x; return v.empty() ? 0.0 : a / (double) v.size();
    };
    res.spliceRateHz = res.splices / dur;
    res.anchorRateHz = res.anchors / dur;
    res.onsetRateHz = res.onsets / dur;
    res.meanLagMs = mean(lags);
    double var = 0;
    for (double x : lags) var += (x - res.meanLagMs) * (x - res.meanLagMs);
    res.lagSdMs = lags.empty() ? 0 : std::sqrt(var / (double) lags.size());
    res.meanAnchorJumpMs = mean(jumps);
    for (double j : jumps) res.maxAnchorJumpMs = std::max(res.maxAnchorJumpMs, j);
    return res;
}

// ---------------------------------------------------------------------------
// ONSET-DETECTOR PROBE.
//
// Measures the two competing quantities on the SAME signal, so the failure of the
// old detector and the margin of the new one are visible side by side:
//
//   CREST — what the old detector thresholded: a 0.5 ms-release peak follower of the
//     >1.2 kHz residual over its own 200 ms MEAN, both taken at the same instant. It
//     fired above 2.5. A ringing chord sits at 2.4-2.9. That is the whole bug: a
//     property of TIMBRE, not of picking, and the detector free-ran on it.
//
//   RISE  — what the new detector thresholds: the same HF envelope (0.5 ms attack,
//     50 ms RELEASE) over ITS OWN VALUE 30 ms AGO. It fires above 2.0. A ringing
//     chord cannot exceed 1.0 for long, because a ringing chord is DECAYING; only an
//     actual new pick makes the level go up. The margin between the two columns is
//     the margin the pedal now has.
// ---------------------------------------------------------------------------
struct OnsetProbe {
    double p95Crest = 0;     // OLD detector's quantity; it fired above 2.50
    double p95Rise = 0;      // NEW detector's quantity; it fires above 2.00
    double hfLevelDb = 0;    // mean HF residual, dBFS (the -55 dBFS floor gates below this)
    double crestTrigHz = 0;  // what the old detector would have done
    double riseTrigHz = 0;   // what the new one does
};

OnsetProbe probeOnset(const std::vector<float>& x, double skipSec = 0.30) {
    const float hpCoef = std::exp(-2.0f * (float) kPi * 1200.0f / (float) kSR);
    const float oldFast = std::exp(-1.0f / (0.0005f * (float) kSR));
    const float slowCoef = std::exp(-1.0f / (0.200f * (float) kSR));
    // Mirrors DropTune::updateOnset exactly (kRiseRatio 1.5, 35 ms lookback, 50 ms
    // release, 40 ms refractory). Keep these in lockstep with the core.
    const float newAtk = std::exp(-1.0f / (0.0005f * (float) kSR));
    const float newRel = std::exp(-1.0f / (0.050f * (float) kSR));
    const int retrigger = (int) (kSR * 0.040);
    const int lookback = (int) (kSR * 0.035);
    const float riseRatio = 1.5f;
    const float floorLevel = 1.8e-3f;

    float hpZ = 0;
    float oFast = 0, oSlow = 0;              // old detector state
    float nFast = 0;                         // new detector state
    std::vector<float> hist((size_t) lookback + 1, 0.0f);
    size_t hw = 0;
    bool oArmed = true, nArmed = true;
    int oSince = 1 << 20, nSince = 1 << 20;
    int oTrig = 0, nTrig = 0;
    double hfSum = 0;
    std::vector<double> crest, rise;
    const size_t skip = (size_t) (skipSec * kSR);

    for (size_t i = 0; i < x.size(); ++i) {
        const float v = x[i];
        hpZ = hpCoef * hpZ + (1.0f - hpCoef) * v;
        const float hp = std::abs(v - hpZ);

        // --- old: crest factor of the same instant ---------------------------
        oFast = hp > oFast ? hp : oldFast * oFast + (1.0f - oldFast) * hp;
        oSlow = slowCoef * oSlow + (1.0f - slowCoef) * hp;
        if (oSince < (1 << 20)) ++oSince;
        const float oRef = std::max(oSlow, floorLevel);
        if (oArmed && oFast > 2.5f * oRef && oSince > retrigger) {
            oSince = 0; oArmed = false;
            if (i >= skip) ++oTrig;
        } else if (!oArmed && oFast < 1.2f * oRef) {
            oArmed = true;
        }

        // --- new: a RISE against the same envelope 30 ms ago ------------------
        const float c = (hp > nFast) ? newAtk : newRel;
        nFast = c * nFast + (1.0f - c) * hp;
        const float past = hist[(hw + 1) % hist.size()];
        hist[hw] = nFast;
        hw = (hw + 1) % hist.size();
        if (nSince < (1 << 20)) ++nSince;
        const float nRef = std::max(past, floorLevel);
        if (nArmed && nFast > riseRatio * nRef && nFast > floorLevel && nSince > retrigger) {
            nSince = 0; nArmed = false;
            if (i >= skip) ++nTrig;
        } else if (!nArmed && nFast < 1.2f * nRef) {
            nArmed = true;
        }

        if (i >= skip) {
            hfSum += hp;
            crest.push_back(oFast / oRef);
            rise.push_back(nFast / nRef);
        }
    }

    OnsetProbe p;
    if (crest.empty()) return p;
    auto p95 = [](std::vector<double> v) {
        const size_t k = v.size() * 95 / 100;
        std::nth_element(v.begin(), v.begin() + (ptrdiff_t) k, v.end());
        return v[k];
    };
    p.p95Crest = p95(crest);
    p.p95Rise = p95(rise);
    p.hfLevelDb = dB(hfSum / (double) crest.size());
    const double secs = (double) (x.size() - skip) / kSR;
    p.crestTrigHz = oTrig / secs;
    p.riseTrigHz = nTrig / secs;
    return p;
}

// ---------------------------------------------------------------------------
// Spectrum, and the artifact-to-tonal ratio (SER).
// ---------------------------------------------------------------------------
std::vector<double> spectrum(const std::vector<float>& x, size_t from, size_t N) {
    std::vector<cd> a(N);
    for (size_t i = 0; i < N; ++i) {
        const double t = (double) i / (double) (N - 1);
        // Blackman-Harris: -92 dB sidelobes, so a partial 10 Hz away cannot fake a
        // -50 dBc sideband.
        const double w = 0.35875 - 0.48829 * std::cos(2 * kPi * t) + 0.14128 * std::cos(4 * kPi * t)
                         - 0.01168 * std::cos(6 * kPi * t);
        a[i] = cd(x[from + i] * w, 0.0);
    }
    fft(a, false);
    std::vector<double> mag(N / 2);
    for (size_t k = 0; k < N / 2; ++k) mag[k] = std::abs(a[k]);
    return mag;
}

std::vector<double> partialsOf(const std::vector<StringNote>& notes, double ratio, double maxHz) {
    std::vector<double> out;
    for (const auto& note : notes) {
        const double f0 = midiHz(note.midi) * ratio;
        const double B = stiffnessFor(note.midi);
        for (int n = 1; n <= 48; ++n) {
            const double fn = (double) n * f0 * std::sqrt(1.0 + B * (double) (n * n));
            if (fn > maxHz) break;
            out.push_back(fn);
        }
    }
    return out;
}

struct SpecResult {
    double serDb = 0;
    double pitchCents = 0;
    double centroidHz = 0;
    double topBandDb = 0;   // 3-6 kHz
};

SpecResult analyseSpectrum(const std::vector<float>& x, size_t from, size_t N,
                           const std::vector<StringNote>& notes, double nominalRatio) {
    SpecResult r;
    const auto mag = spectrum(x, from, N);
    const double binHz = kSR / (double) N;

    // Measure the ACTUAL pitch ratio from the fundamental, so the tonal mask is not
    // smeared by a 1-cent nominal error at 5 kHz.
    const double f0nom = midiHz(notes[0].midi) * nominalRatio;
    size_t bestK = (size_t) (f0nom / binHz);
    double best = 0;
    for (size_t k = (size_t) (f0nom * 0.94 / binHz); k <= (size_t) (f0nom * 1.06 / binHz) && k < mag.size(); ++k)
        if (mag[k] > best) { best = mag[k]; bestK = k; }
    double bestF = (double) bestK * binHz;
    if (bestK > 0 && bestK + 1 < mag.size()) {
        const double a = mag[bestK - 1], b = mag[bestK], c = mag[bestK + 1];
        const double den = a - 2.0 * b + c;               // NEGATIVE at a peak
        const double d = std::abs(den) > 1e-15 ? 0.5 * (a - c) / den : 0.0;
        bestF = ((double) bestK + std::max(-0.5, std::min(0.5, d))) * binHz;
    }
    const double measRatio = bestF / midiHz(notes[0].midi);
    r.pitchCents = 1200.0 * std::log2(std::max(1e-9, measRatio / nominalRatio));

    std::vector<uint8_t> tonal(mag.size(), 0);
    for (double f : partialsOf(notes, measRatio, 6000.0)) {
        const double half = std::max(2.0, 0.0005 * f);
        const size_t k0 = (size_t) std::max(0.0, (f - half) / binHz);
        const size_t k1 = (size_t) std::min((double) mag.size() - 1, (f + half) / binHz);
        for (size_t k = k0; k <= k1 && k < mag.size(); ++k) tonal[k] = 1;
    }

    double eTon = 0, eAll = 0, cnum = 0, cden = 0, eTop = 0;
    for (size_t k = 1; k < mag.size(); ++k) {
        const double f = (double) k * binHz;
        if (f < 60.0 || f > 6000.0) continue;
        const double e = mag[k] * mag[k];
        eAll += e;
        if (tonal[k]) eTon += e;
        cnum += f * e; cden += e;
        if (f >= 3000.0) eTop += e;
    }
    r.serDb = dB10(std::max(1e-20, eAll - eTon) / std::max(1e-20, eTon));
    r.centroidHz = cden > 0 ? cnum / cden : 0;
    r.topBandDb = dB10(eTop);
    return r;
}

// ---------------------------------------------------------------------------
// Band-limited analytic envelope (FFT Hilbert with a brick-wall band mask).
// ---------------------------------------------------------------------------
std::vector<double> bandEnvelope(const std::vector<float>& x, size_t from, size_t N,
                                 double f1, double f2) {
    std::vector<cd> a(N);
    for (size_t i = 0; i < N; ++i) a[i] = cd(x[from + i], 0.0);
    fft(a, false);
    const double binHz = kSR / (double) N;
    for (size_t k = 0; k < N; ++k) {
        const double f = (double) k * binHz;
        a[k] = (k > 0 && k < N / 2 && f >= f1 && f <= f2) ? a[k] * 2.0 : cd(0.0, 0.0);
    }
    fft(a, true);
    std::vector<double> env(N);
    for (size_t i = 0; i < N; ++i) env[i] = std::abs(a[i]);
    return env;
}

/// EVENT-SYNCHRONOUS ENVELOPE DEFLECTION.
/// Average the envelope around every splice/anchor. Chord beating is asynchronous
/// with the events and averages away; what survives is the dent each event puts in
/// the signal. Reported as the worst deviation from the local mean, in dB.
double eventDeflectionDb(const std::vector<float>& y, const std::vector<Event>& events,
                         bool anchorsOnly, double f1, double f2) {
    const size_t N = pow2Below(y.size());
    const auto env = bandEnvelope(y, 0, N, f1, f2);
    const int pre = (int) (0.008 * kSR), post = (int) (0.020 * kSR);
    std::vector<double> acc((size_t) (pre + post), 0.0);
    int used = 0;
    for (const auto& e : events) {
        if (e.anchor != anchorsOnly) continue;
        if (e.sample < (size_t) pre + (size_t) (0.5 * kSR)) continue;   // skip engage
        if (e.sample + (size_t) post >= N) break;
        for (int k = -pre; k < post; ++k) acc[(size_t) (k + pre)] += env[e.sample + (size_t) k];
        ++used;
    }
    if (used < 4) return 0.0;
    double mean = 0;
    for (double v : acc) mean += v;
    mean /= (double) acc.size();
    if (mean < 1e-9) return 0.0;
    double worst = 0;
    for (double v : acc) worst = std::max(worst, std::abs(dB(v / mean)));
    return worst;
}

/// ADDED AMPLITUDE MODULATION — the warble metric that works on REAL material.
///
/// A sustained chord's line-spectrum SER needs the exact partial list, which only
/// exists for synthesized material. On a real recording the honest observable is
/// the ENVELOPE: every splice/anchor dents it, and warble is by definition
/// amplitude modulation in the few-Hz range. So: band envelope -> its 2..15 Hz
/// modulation RMS over its mean. Reported as shifted MINUS dry, because real
/// playing already modulates its own envelope (that is what picking is) — the
/// pedal's guilt is only the modulation it ADDS.
double modDepthDb(const std::vector<float>& x, double lo, double hi) {
    const size_t N = pow2Below(std::min(x.size(), (size_t) (8.0 * kSR)));
    if (N < (size_t) kSR) return 0.0;
    const auto env = bandEnvelope(x, 0, N, lo, hi);
    double mean = 0;
    for (size_t i = 0; i < N; ++i) mean += env[i];
    mean /= (double) N;
    if (mean < 1e-9) return -120.0;
    std::vector<cd> a(N);
    for (size_t i = 0; i < N; ++i) a[i] = cd(env[i] - mean, 0.0);
    fft(a, false);
    const double binHz = kSR / (double) N;
    double e = 0;
    const size_t k0 = std::max<size_t>(1, (size_t) (2.0 / binHz));
    const size_t k1 = std::min(N / 2, (size_t) (15.0 / binHz));
    for (size_t k = k0; k <= k1; ++k) e += std::norm(a[k]);
    const double rms = std::sqrt(2.0 * e) / (double) N;
    return dB(rms / mean);
}

/// Load an external recording, mono, resampled to 48 kHz (with the shifter's own
/// windowed sinc — transparent for r <= 1), peak-normalized to 0.5 like the synth.
bool loadReal48k(const std::string& path, std::vector<float>& out, double maxSec = 14.0) {
    std::vector<float> raw;
    double rawSr = 0;
    if (!readWavMono(path, raw, rawSr) || raw.empty()) return false;
    if (std::abs(rawSr - kSR) < 1.0) {
        out = raw;
    } else {
        const auto& tbl = dcdsp::SincTable::get();
        const double step = rawSr / kSR;
        const size_t n = (size_t) ((double) raw.size() / step) - 64;
        out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const double pos = 32.0 + (double) i * step;
            const long long i0 = (long long) std::floor(pos);
            const float* h = tbl.at(std::min(511, (int) ((pos - (double) i0) * 512)));
            double acc = 0;
            for (int k = 0; k < 32; ++k) {
                const long long idx = i0 - 15 + k;
                if (idx >= 0 && idx < (long long) raw.size()) acc += h[k] * raw[(size_t) idx];
            }
            out[i] = (float) acc;
        }
    }
    if (out.size() > (size_t) (maxSec * kSR)) out.resize((size_t) (maxSec * kSR));
    double pk = 1e-9;
    for (float v : out) pk = std::max(pk, (double) std::abs(v));
    for (float& v : out) v = (float) (v * 0.5 / pk);
    return true;
}

// ---------------------------------------------------------------------------
// THE ORACLE — what lag is even AVAILABLE at each ceiling?
//
// Evaluated twice: on the band-limited x4-decimated mid the correlator actually
// sees (LPF 1.2 kHz, exactly as dcdsp_droptune builds it), and on the FULL BAND,
// which is what the splice actually cuts.
// ---------------------------------------------------------------------------
struct OracleRow {
    double ceilMs, bestLagMs, nccLp, nccFullAtLp, bestFullNcc, bestFullLagMs;
};

std::vector<float> lpDecimatedMid(const std::vector<float>& x) {
    const float w = std::tan((float) kPi * 1200.0f / (float) kSR);
    const float k = 1.41421356f;
    const float n = 1.0f / (1.0f + k * w + w * w);
    const float b0 = w * w * n, b1 = 2 * b0, b2 = b0;
    const float a1 = 2.0f * (w * w - 1.0f) * n;
    const float a2 = (1.0f - k * w + w * w) * n;
    float z1 = 0, z2 = 0;
    std::vector<float> out;
    out.reserve(x.size() / 4 + 1);
    int c = 0;
    for (float v : x) {
        const float y = b0 * v + z1;
        z1 = b1 * v - a1 * y + z2;
        z2 = b2 * v - a2 * y;
        if (++c >= 4) { c = 0; out.push_back(y); }
    }
    return out;
}

double ncc(const std::vector<float>& s, long long base, int lag, int win) {
    double dot = 0, ea = 0, eb = 0;
    for (int j = 0; j < win; ++j) {
        const long long ia = base - j, ib = base + lag - j;
        if (ia < 0 || ib < 0 || ib >= (long long) s.size()) return 0;
        const double a = s[(size_t) ia], b = s[(size_t) ib];
        dot += a * b; ea += a * a; eb += b * b;
    }
    return dot / (std::sqrt(ea * eb) + 1e-12);
}

std::vector<OracleRow> oracle(const std::vector<float>& src, double atSec,
                              std::initializer_list<double> ceils = {10.0, 20.0, 35.0, 50.0, 70.0}) {
    const auto lp = lpDecimatedMid(src);
    const double aRate = kSR / 4.0;
    const int aWin = (int) (aRate * 0.033);
    const int fullWin = (int) (kSR * 0.033);
    const long long aBase = (long long) (atSec * aRate);
    const long long fBase = (long long) (atSec * kSR);

    std::vector<OracleRow> rows;
    for (double ceilMs : ceils) {
        OracleRow r{};
        r.ceilMs = ceilMs;
        const int aMin = (int) (kSR * 0.003) / 4;
        const int aMax = (int) (kSR * ceilMs * 0.001) / 4;
        double bestLp = -2; int bestLpLag = aMin;
        for (int lag = aMin; lag <= aMax; ++lag) {
            const double s = ncc(lp, aBase, lag, aWin);
            if (s > bestLp) { bestLp = s; bestLpLag = lag; }
        }
        r.bestLagMs = bestLpLag * 4 * 1000.0 / kSR;
        r.nccLp = bestLp;
        r.nccFullAtLp = ncc(src, fBase, bestLpLag * 4, fullWin);
        double bestFull = -2; int bestFullLag = 0;
        for (int lag = (int) (kSR * 0.003); lag <= (int) (kSR * ceilMs * 0.001); ++lag) {
            const double s = ncc(src, fBase, lag, fullWin);
            if (s > bestFull) { bestFull = s; bestFullLag = lag; }
        }
        r.bestFullNcc = bestFull;
        r.bestFullLagMs = bestFullLag * 1000.0 / kSR;
        rows.push_back(r);
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Transients
// ---------------------------------------------------------------------------
struct TransientStats { double latencyMs = 0, riseMs = 0, peakDb = 0; int doubled = 0; };

/// Onset = the point of steepest envelope RISE in the window. A 10%-of-peak
/// threshold cannot be used here: on a chord picked every 500 ms the PREVIOUS note
/// is still ringing well above 10%, so the threshold trips at the window edge and
/// reports a negative latency (the reference does it too — it is a measurement
/// artifact, not a shifter defect).
size_t steepestRise(const std::vector<double>& env, size_t p0, size_t p1) {
    const size_t w = (size_t) (0.001 * kSR);
    double best = -1e30;
    size_t bestI = p0;
    for (size_t i = p0; i + w < p1; ++i) {
        const double d = env[i + w] - env[i];
        if (d > best) { best = d; bestI = i; }
    }
    return bestI;
}

/// Latency is measured RELATIVE to the native reference, with the same detector on
/// both, so any bias in the detector cancels.
TransientStats transientOf(const std::vector<float>& y, const std::vector<float>& ref,
                           const std::vector<double>& picks) {
    TransientStats t;
    const size_t N = pow2Below(std::min(y.size(), ref.size()));
    const auto envY = bandEnvelope(y, 0, N, 60.0, 8000.0);
    const auto envR = bandEnvelope(ref, 0, N, 60.0, 8000.0);
    double lat = 0, rise = 0, pk = 0;
    int used = 0;
    for (double ps : picks) {
        const size_t p0 = (size_t) ((ps - 0.015) * kSR), p1 = (size_t) ((ps + 0.10) * kSR);
        if (p1 >= N) continue;

        double peakY = 0, peakR = 0;
        size_t peakIdx = p0;
        for (size_t i = p0; i < p1; ++i) {
            if (envY[i] > peakY) { peakY = envY[i]; peakIdx = i; }
            peakR = std::max(peakR, envR[i]);
        }
        if (peakY < 1e-6 || peakR < 1e-6) continue;

        // Each strum starts from silence, so a plain 10%-of-peak crossing is the
        // right (and unambiguous) onset. steepestRise latches onto envelope beats.
        size_t onY = p0, onR = p0;
        for (size_t i = p0; i < p1; ++i) if (envY[i] > 0.10 * peakY) { onY = i; break; }
        for (size_t i = p0; i < p1; ++i) if (envR[i] > 0.10 * peakR) { onR = i; break; }
        (void) onR;

        // Absolute: how long after the pick ARRIVED did it come out? Meaningful
        // because each strum here starts from silence. The REF row is the control.
        (void) onR;
        lat += ((double) onY - ps * kSR) * 1000.0 / kSR;
        pk += dB(peakY / peakR);

        // 10->90% of the rise above the level the note started from
        const double base = envY[onY];
        const double span = std::max(1e-9, peakY - base);
        size_t t10 = onY, t90 = peakIdx;
        for (size_t i = onY; i <= peakIdx; ++i) {
            if (envY[i] < base + 0.10 * span) t10 = i;
            if (envY[i] < base + 0.90 * span) t90 = i;
        }
        rise += (double) (t90 > t10 ? t90 - t10 : 0) * 1000.0 / kSR;
        ++used;

        bool dipped = false;
        const size_t end = std::min(p1, peakIdx + (size_t) (0.045 * kSR));
        for (size_t i = peakIdx; i < end; ++i) {
            if (envY[i] < 0.45 * peakY) dipped = true;
            else if (dipped && envY[i] > 0.60 * peakY) { ++t.doubled; break; }
        }
    }
    if (used) { t.latencyMs = lat / used; t.riseMs = rise / used; t.peakDb = pk / used; }
    return t;
}

// ---------------------------------------------------------------------------
struct Chord { const char* name; const char* what; std::vector<StringNote> notes; };

std::vector<Chord> chords() {
    auto n = [](int m, double s) { return StringNote{m, s}; };
    return {
        {"E2 single", "one note — the old harness's whole world",   {n(40, 0.0)}},
        {"E5 power",  "root+5th+8ve — the splice-FRIENDLY case",    {n(40, 0.0), n(47, 0.0), n(52, 0.0)}},
        {"E+G# 3rd",  "a bare MAJOR THIRD — the doc's hypothesis",  {n(40, 0.0), n(44, 0.0)}},
        {"E major",   "open E major (has the third)",               {n(40, .000), n(47, .011), n(52, .022), n(56, .033), n(59, .044), n(64, .055)}},
        {"E minor",   "open E minor (minor third)",                 {n(40, .000), n(47, .011), n(52, .022), n(55, .033), n(59, .044), n(64, .055)}},
        {"G major",   "open G major",                               {n(43, .000), n(47, .011), n(50, .022), n(55, .033), n(59, .044), n(67, .055)}},
    };
}

const char* modeName(float lagMs) { return lagMs < 12 ? "Tight" : (lagMs < 25 ? "Balanced" : "Smooth"); }

const float kModes[3] = {10.0f, 20.0f, 35.0f};
const int   kShifts[5] = {-1, -2, -3, -5, -12};

}  // namespace

int main(int argc, char** argv) {
    std::string wavDir;
    std::vector<std::string> diFiles;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc) wavDir = argv[++i];
        else if (std::strcmp(argv[i], "--di") == 0 && i + 1 < argc) diFiles.push_back(argv[++i]);
    }

    const auto cs = chords();
    const size_t anaFrom = (size_t) (0.7 * kSR);
    const size_t anaN = pow2Below((size_t) (3.0 * kSR));   // 2.73 s, 0.37 Hz bins

    std::printf("========================================================================\n");
    std::printf(" CSVD drop tuner — POLYPHONIC harness   48 kHz\n");
    std::printf(" Modes: Tight 10 ms / Balanced 20 ms / Smooth 35 ms splice-lag ceiling\n");
    std::printf("========================================================================\n\n");

    // =======================================================================
    // 1. THE TRANSIENT-ANCHOR CENSUS.
    //    The onset detector snaps the read head forward at every pick. How often
    //    does it think it heard a pick? A sine cannot answer this. A chord can.
    // =======================================================================
    std::printf("--- 1. TRANSIENT-ANCHOR CENSUS -----------------------------------------\n");
    std::printf("'picks' = the real note attacks in the material. 'onsets' = the detector's\n");
    std::printf("verdict; every onset beyond a real pick is a FALSE anchor, which jumps the\n");
    std::printf("read head with a 1 ms crossfade and NO waveform matching AND cancels any\n");
    std::printf("pending correlation splice. onsets ~= picks is the pass condition.\n");
    std::printf("'old' = what the pre-0.19 crest-factor detector would have fired on the SAME\n");
    std::printf("signal — the regression guard. retrigger_ = 40 ms caps both at 25/s.\n");
    std::printf("'period' = what the periodicity probe locked onto; the splice-lag ceiling is\n");
    std::printf("raised to it (the knob is a FLOOR). 'delay' = mean sustain delay, its cost.\n\n");
    std::printf("%-24s %6s %6s | %6s %7s | %7s %7s | %7s %6s\n",
                "material (-2 st, Balanced)", "picks", "HF dB", "old", "onsets", "splices",
                "lag ms", "period", "delay");
    {
        auto census = [&](const std::string& name, const std::vector<float>& src, double picksHz) {
            const auto run = runShifter(src, -2.0f, 20.0f);
            const auto pr = probeOnset(src);
            std::printf("%-24s %5.1f/s %6.0f | %4.1f/s %5.1f/s | %5.1f/s %7.2f | %6.2f %6.1f\n",
                        name.c_str(), picksHz, pr.hfLevelDb,
                        pr.crestTrigHz, run.onsetRateHz,
                        run.spliceRateHz, run.meanLagMs, run.periodMs, run.meanDelayMs);
        };
        census("220 Hz sine", sine(220.0, 4.0), 0.0);
        census("5 kHz sine", sine(5000.0, 4.0), 0.0);
        std::printf("%-24s  -- all the sine harness could see. No HF crest, so even the OLD\n", "");
        std::printf("%-24s     detector stayed quiet on it: that is why it missed the bug.\n\n", "");

        // The realistic sustain: strike once, let it RING. This is the material the
        // "sustained ringing chord" limit in the header is actually about.
        for (const auto& c : cs) {
            std::vector<StringNote> once;
            for (const auto& nn : c.notes) once.push_back({nn.midi, 0.2 + nn.startSec});
            census(std::string(c.name) + " (ring out)", synthChord(once, 4.0, false, 1.0, 1234), 0.25);
        }
        std::printf("\n");
        // Repeatedly picked — a real riff.
        for (const auto& c : cs) {
            std::vector<StringNote> rep;
            for (double p : {0.30, 0.80, 1.30, 1.80, 2.30, 2.80})
                for (const auto& nn : c.notes) rep.push_back({nn.midi, p + nn.startSec});
            census(std::string(c.name) + " (picked)", synthChord(rep, 3.4, false, 1.0, 4321), 6.0 / 3.4);
        }
        std::printf("\n");
        // The synthetic constant-amplitude sustain used by sections 2/3/5.
        for (const auto& c : cs)
            census(std::string(c.name) + " (held)", synthChord(c.notes, 4.0, true, 1.0, 1234), 0.25);
    }
    std::printf("\n");

    // =======================================================================
    // 1b. CREST vs RISE — why the old detector had no margin and the new one does.
    //
    // The old updateOnset() thresholded fastEnv_ (a 0.5 ms-release PEAK follower)
    // against 2.5x slowEnv_ (a 200 ms MEAN of the same signal, at the same instant).
    // That ratio is the CREST FACTOR of the >1.2 kHz residual, not a RISE. Nothing in
    // it asked "did the level go UP?". And a 0.5 ms release falls back to the
    // instantaneous value between waveform peaks, so it re-armed EVERY PERIOD of any
    // note below 500 Hz — i.e. every guitar note. Whether the pedal fell apart then
    // depended on whether your TIMBRE AND LEVEL happened to clear 2.5.
    //
    // The new one compares the HF envelope against ITS OWN VALUE 30 ms AGO. A ringing
    // chord is decaying, so that ratio cannot stay above 1; only a real pick pushes it
    // over 2.0. This sweep is the proof: same chord, same single pick, only brightness
    // and level change, and the answer must not move.
    // =======================================================================
    std::printf("--- 1b. CREST vs RISE: THE DETECTOR'S MARGIN ---------------------------\n");
    std::printf("One E-major strum, left to ring for 4 s. ONE real pick (0.25/s). Only the\n");
    std::printf("tone and the level of the guitar change — the verdict must not.\n");
    std::printf("crest = the OLD quantity (fired above 2.50). rise = the NEW one (fires above\n");
    std::printf("1.50). Both are p95 over the ring-out. (An HF floor gates below -55 dB.)\n\n");
    std::printf("%-22s %7s %7s | %7s %10s %10s\n", "same chord, but...", "crest", "rise",
                "HF dB", "OLD anch", "NOW anch");
    {
        std::vector<StringNote> once;
        for (const auto& nn : cs[3].notes) once.push_back({nn.midi, 0.2 + nn.startSec});
        const auto base = synthChord(once, 4.0, false, 1.0, 1234);

        auto tone = [&](double fc, double gain) {
            std::vector<float> y = base;
            if (fc > 0) {  // one-pole LPF: a darker pickup / rolled-off tone knob
                const float a = std::exp(-2.0f * (float) kPi * (float) fc / (float) kSR);
                float z = 0;
                for (float& v : y) { z = a * z + (1 - a) * v; v = z; }
            }
            for (float& v : y) v = (float) (v * gain);
            return y;
        };
        auto row = [&](const char* label, const std::vector<float>& x) {
            const auto pr = probeOnset(x);
            const auto run = runShifter(x, -2.0f, 20.0f);
            std::printf("%-22s %7.2f %7.2f | %7.0f %8.1f/s %8.1f/s\n", label,
                        pr.p95Crest, pr.p95Rise, pr.hfLevelDb, pr.crestTrigHz, run.onsetRateHz);
        };
        row("as modelled (0 dB)",     tone(0, 1.0));
        row("  -12 dB (quieter)",     tone(0, 0.25));
        row("  -26 dB (fingerstyle)", tone(0, 0.05));
        row("tone rolled to 4 kHz",   tone(4000, 1.0));
        row("tone rolled to 2 kHz",   tone(2000, 1.0));
        row("tone rolled to 1 kHz",   tone(1000, 1.0));
        std::printf("\nA ringing chord's CREST sits at 2.4-2.6 against a 2.50 trigger, so the old\n");
        std::printf("detector free-ran at ~18 false anchors/s on ONE strum — and rolling the tone\n");
        std::printf("off barely helped, because a crest ratio is level-INVARIANT. The only thing\n");
        std::printf("suppressing it was the -55 dB floor gate, i.e. the guitar going inaudible.\n");
        std::printf("The RISE column stays near 1.2 on the same signal, under its 1.50 trigger: a\n");
        std::printf("decaying chord cannot out-run its own past. That is the margin.\n\n");
    }

    // =======================================================================
    // 2. ARTIFACT LEVEL, AND THE COUNTERFACTUAL.
    //    SER = energy that is NOT on a partial / energy that is. The native
    //    down-tuned guitar is the floor. 'no-anchor' is the same shifter with the
    //    onset detector disabled — if the artifact collapses, the anchor caused it.
    // =======================================================================
    std::printf("--- 2. ARTIFACT-TO-TONAL RATIO, SUSTAINED CHORDS (dB, lower=cleaner) ---\n");
    std::printf("ship = as shipped.  noAnc = transient anchor suppressed (counterfactual).\n");
    std::printf("REF = the same chord on a natively down-tuned guitar (the A/B ceiling).\n\n");
    std::printf("%-11s %5s | %-24s | %-24s | %6s\n", "", "", "as shipped", "anchor suppressed", "");
    std::printf("%-11s %5s | %7s %7s %7s | %7s %7s %7s | %6s\n",
                "chord", "st", "Tight", "Balan", "Smooth", "Tight", "Balan", "Smooth", "REF");
    std::map<std::string, double> serShip, serNoAnc;
    for (const auto& c : cs) {
        for (int s : kShifts) {
            const double r = std::pow(2.0, s / 12.0);
            const auto src = synthChord(c.notes, 4.0, true, 1.0, 1234);
            const auto ref = synthChord(c.notes, 4.0, true, r, 1234);
            const auto sr = analyseSpectrum(ref, anaFrom, anaN, c.notes, r);
            double a[3], b[3];
            for (int mi = 0; mi < 3; ++mi) {
                a[mi] = analyseSpectrum(runShifter(src, (float) s, kModes[mi]).y, anaFrom, anaN, c.notes, r).serDb;
                b[mi] = analyseSpectrum(runShifter(src, (float) s, kModes[mi], true).y, anaFrom, anaN, c.notes, r).serDb;
                char k[96];
                std::snprintf(k, sizeof k, "%s|%d|%d", c.name, s, mi);
                serShip[k] = a[mi];
                serNoAnc[k] = b[mi];
            }
            std::printf("%-11s %5d | %7.1f %7.1f %7.1f | %7.1f %7.1f %7.1f | %6.1f\n",
                        c.name, s, a[0], a[1], a[2], b[0], b[1], b[2], sr.serDb);
        }
    }
    std::printf("\n");

    // =======================================================================
    // 3. DOES SMOOTH ACTUALLY HELP ON CHORDS?  Asked twice: as shipped, and in a
    //    world where the correlation splice is allowed to run at all.
    // =======================================================================
    std::printf("--- 3. DOES 'SMOOTH' HELP ON CHORDS? (SER, dB; negative delta = better) --\n\n");
    std::printf("%-11s %5s | %-22s | %-22s\n", "", "", "as shipped", "anchor suppressed");
    std::printf("%-11s %5s | %6s %6s %8s | %6s %6s %8s\n",
                "chord", "st", "Balan", "Smooth", "delta", "Balan", "Smooth", "delta");
    for (const auto& c : cs) {
        for (int s : kShifts) {
            char k1[96], k2[96];
            std::snprintf(k1, sizeof k1, "%s|%d|1", c.name, s);
            std::snprintf(k2, sizeof k2, "%s|%d|2", c.name, s);
            std::printf("%-11s %5d | %6.1f %6.1f %+7.1f | %6.1f %6.1f %+7.1f\n", c.name, s,
                        serShip[k1], serShip[k2], serShip[k2] - serShip[k1],
                        serNoAnc[k1], serNoAnc[k2], serNoAnc[k2] - serNoAnc[k1]);
        }
    }
    std::printf("\n");

    // =======================================================================
    // 4. THE SPLICE-LAG ORACLE — the doc's hypothesis, tested directly.
    // =======================================================================
    std::printf("--- 4. SPLICE-LAG ORACLE ------------------------------------------------\n");
    std::printf("The best waveform match a PERFECT correlator could find under each ceiling.\n");
    std::printf("lp = what the shifter's 1.2 kHz / x4-decimated correlator sees.\n");
    std::printf("full@lp = how well that lag ACTUALLY aligns the full-band audio it cuts.\n");
    std::printf("A splice is inaudible only when full@lp ~ 1.0.\n");
    std::printf("THIS IS THE JUSTIFICATION FOR THE ADAPTIVE CEILING. Read the 'lag' column:\n");
    std::printf("nothing usable exists below 12.1 ms for even ONE low E, below 24.3 ms for a\n");
    std::printf("power chord (a fifth is NOT a harmonic of the root — lcm(T,2T/3) = 2T), below\n");
    std::printf("40.8 ms for an open G or below 48.3 ms for a bare major third. The shifter now\n");
    std::printf("MEASURES this number at run time (section 1's 'period' column) instead of\n");
    std::printf("guessing it from a knob.\n\n");
    std::printf("%-11s %8s %9s %8s %9s %14s\n", "chord", "ceiling", "lag(ms)", "NCC lp", "full@lp", "best full");
    for (const auto& c : cs) {
        const auto src = synthChord(c.notes, 3.0, true, 1.0, 1234);
        double f0 = 1e9;
        for (const auto& nn : c.notes) f0 = std::min(f0, midiHz(nn.midi));
        bool first = true;
        for (const auto& r : oracle(src, 1.5)) {
            std::printf("%-11s %6.0f ms %9.2f %8.3f %9.3f %8.3f@%.1fms\n",
                        first ? c.name : "", r.ceilMs, r.bestLagMs, r.nccLp, r.nccFullAtLp,
                        r.bestFullNcc, r.bestFullLagMs);
            first = false;
        }
        std::printf("%-11s root period %.1f ms\n\n", "", 1000.0 / f0);
    }

    // =======================================================================
    // 5. EVENT-SYNCHRONOUS ENVELOPE DEFLECTION — how audible is one event?
    // =======================================================================
    std::printf("--- 5. HOW BIG A DENT DOES EACH EVENT PUT IN THE SIGNAL? ----------------\n");
    std::printf("Envelope averaged over every event (chord beating is asynchronous and\n");
    std::printf("averages out). Worst deviation from the local mean, dB. Sustained, -2 st.\n\n");
    std::printf("%-11s %-9s | %10s %10s | %10s %10s\n",
                "chord", "mode", "anchor LO", "anchor TOP", "splice LO", "splice TOP");
    for (const auto& c : cs) {
        const auto src = synthChord(c.notes, 4.0, true, 1.0, 1234);
        for (int mi = 0; mi < 3; ++mi) {
            const auto run = runShifter(src, -2.0f, kModes[mi]);
            const auto noA = runShifter(src, -2.0f, kModes[mi], true);
            std::printf("%-11s %-9s | %9.2f  %9.2f  | %9.2f  %9.2f\n", c.name, modeName(kModes[mi]),
                        eventDeflectionDb(run.y, run.events, true, 80, 400),
                        eventDeflectionDb(run.y, run.events, true, 3000, 8000),
                        eventDeflectionDb(noA.y, noA.events, false, 80, 400),
                        eventDeflectionDb(noA.y, noA.events, false, 3000, 8000));
        }
    }
    std::printf("\n");

    // =======================================================================
    // 6. TRANSIENTS (plucked, 6 picks) — with the reference as the control.
    // =======================================================================
    std::printf("--- 6. TRANSIENT INTEGRITY (one strum, from silence) --------------------\n");
    std::printf("A single strum so the attack is unambiguous (repeated picks confound this:\n");
    std::printf("the previous note is still ringing over the next one's onset).\n");
    std::printf("latency = pick out vs pick in. dPeak = attack peak vs the native reference.\n");
    std::printf("The REF row is the reference through the same measurement — the control.\n\n");
    std::printf("%-11s %5s %-9s | %9s %9s %8s %5s\n", "chord", "st", "mode", "latency", "rise", "dPeak", "dbl");
    {
        const std::vector<double> picks = {0.30};
        for (const auto& c : cs) {
            std::vector<StringNote> notes;
            for (double p : picks)
                for (const auto& nn : c.notes) notes.push_back(StringNote{nn.midi, p + nn.startSec});
            for (int s : {-2, -12}) {
                const double r = std::pow(2.0, s / 12.0);
                const auto src = synthChord(notes, 1.8, false, 1.0, 4321);
                const auto ref = synthChord(notes, 1.8, false, r, 4321);
                for (int mi = 0; mi < 3; ++mi) {
                    const auto run = runShifter(src, (float) s, kModes[mi]);
                    const auto t = transientOf(run.y, ref, picks);
                    std::printf("%-11s %5d %-9s | %7.2f ms %6.2f ms %+7.1f %5d\n",
                                c.name, s, modeName(kModes[mi]), t.latencyMs, t.riseMs, t.peakDb, t.doubled);
                }
                // What deleting the anchor outright would cost. It is NOT the fix —
                // the anchor is what makes the attack arrive on time. It needs to
                // become a real onset detector, not to go away.
                const auto na = runShifter(src, (float) s, 20.0f, true);
                const auto tn = transientOf(na.y, ref, picks);
                std::printf("%-11s %5d %-9s | %7.2f ms %6.2f ms %+7.1f %5d\n",
                            "", s, "noAnchor", tn.latencyMs, tn.riseMs, tn.peakDb, tn.doubled);
                const auto tr = transientOf(ref, ref, picks);
                std::printf("%-11s %5d %-9s | %7.2f ms %6.2f ms %+7.1f %5d\n",
                            "", s, "REF", tr.latencyMs, tr.riseMs, tr.peakDb, tr.doubled);
            }
        }
    }
    std::printf("\n");

    // =======================================================================
    // 7. REAL GUITAR — the DI signal is the one that matters.
    //    The drop tuner is on the RAW INPUT, before our amp sim. So the honest
    //    test signal is a DI guitar, not an amped one: an amp/cab compresses the
    //    HF crest, which is the exact quantity the onset detector thresholds.
    //    Both are measured here, because the difference IS the finding.
    // =======================================================================
    std::printf("--- 7. REAL GUITAR RECORDINGS ------------------------------------------\n");
    {
        std::printf("  HONEST CAVEAT: this repo contains NO clean/DI guitar recording. The two\n");
        std::printf("  candidates are both unusable as a proxy, and WHY they are unusable is\n");
        std::printf("  itself informative — neither can trigger the onset detector:\n");
        std::printf("    NAM input.wav      = 1 s of silence + 1 s of a SINE test tone (not a\n");
        std::printf("                         guitar at all: constant -9 dB RMS, crest 1.58).\n");
        std::printf("    JUCE guitar_amp.wav= post amp+cab, HF residual at -71 dB, which is\n");
        std::printf("                         BELOW the detector's -55 dB floorLevel gate. The\n");
        std::printf("                         gate is the only thing suppressing it, not logic.\n");
        std::printf("  The false-anchor rate below therefore rests on the SYNTHESIZED guitar and\n");
        std::printf("  on the mechanism in 1b — not on these files. Feed a real DI clip to settle it.\n\n");

        struct Src { const char* label; const char* what; std::vector<const char*> paths; };
        const std::vector<Src> sources = {
            {"NAM input", "a SINE test tone, mislabelled as guitar — control only",
             {"build/_deps/neuralaudio-src/deps/NeuralAmpModelerCore/example_audio/input.wav",
              "../build/_deps/neuralaudio-src/deps/NeuralAmpModelerCore/example_audio/input.wav"}},
            {"amped gtr", "post amp+cab — HF below the noise gate, cannot trigger",
             {"build/_deps/juce-src/examples/Assets/guitar_amp.wav",
              "../build/_deps/juce-src/examples/Assets/guitar_amp.wav"}},
        };

        for (const auto& srcDef : sources) {
            std::vector<float> raw;
            double rawSr = 0;
            std::string used;
            for (const char* p : srcDef.paths)
                if (readWavMono(p, raw, rawSr)) { used = p; break; }
            if (raw.empty()) {
                std::printf("  %s: not found — skipped\n\n", srcDef.label);
                continue;
            }

            // Resample to 48 kHz with the shifter's own windowed-sinc (r<=1 => the
            // transparent path). A no-op when the file is already 48 kHz.
            std::vector<float> x;
            if (std::abs(rawSr - kSR) < 1.0) {
                x = raw;
            } else {
                const auto& tbl = dcdsp::SincTable::get();
                const double step = rawSr / kSR;
                const size_t n = (size_t) ((double) raw.size() / step) - 64;
                x.resize(n);
                for (size_t i = 0; i < n; ++i) {
                    const double pos = 32.0 + (double) i * step;
                    const long long i0 = (long long) std::floor(pos);
                    const float* h = tbl.at(std::min(511, (int) ((pos - (double) i0) * 512)));
                    double acc = 0;
                    for (int k = 0; k < 32; ++k) {
                        const long long idx = i0 - 15 + k;
                        if (idx >= 0 && idx < (long long) raw.size()) acc += h[k] * raw[(size_t) idx];
                    }
                    x[i] = (float) acc;
                }
            }
            double pkIn = 1e-9;
            for (float v : x) pkIn = std::max(pkIn, (double) std::abs(v));
            for (float& v : x) v = (float) (v * 0.5 / pkIn);   // same headroom as the synth

            std::vector<float> loop;
            while (loop.size() < (size_t) (4.0 * kSR)) loop.insert(loop.end(), x.begin(), x.end());
            loop.resize((size_t) (4.0 * kSR));

            // Real onsets: envelope crests > 40% of peak, > 80 ms apart. The honest
            // denominator for the anchor count.
            const size_t N = pow2Below(loop.size());
            const auto env = bandEnvelope(loop, 0, N, 80, 8000);
            double pk = 0;
            for (double v : env) pk = std::max(pk, v);
            int realOnsets = 0;
            size_t lastOn = 0;
            bool below = true;
            for (size_t i = 0; i < N; ++i) {
                if (below && env[i] > 0.40 * pk && (lastOn == 0 || i - lastOn > (size_t) (0.08 * kSR))) {
                    ++realOnsets; lastOn = i; below = false;
                } else if (env[i] < 0.20 * pk) below = true;
            }
            const auto pr = probeOnset(loop);
            std::printf("  %s — %s\n", srcDef.label, srcDef.what);
            std::printf("    %s (%.0f Hz, %.2f s, looped to 4 s)\n", used.c_str(), rawSr,
                        (double) raw.size() / rawSr);
            std::printf("    real onsets ~%d (%.1f/s) | HF crest p95 %.2f (fires above 2.50) |"
                        " HF level %.0f dB\n\n", realOnsets, realOnsets / 4.0, pr.p95Crest, pr.hfLevelDb);
            std::printf("    %-4s %-9s | %8s %8s | %8s %8s\n",
                        "st", "mode", "anchors", "splices", "jump ms", "lag ms");
            for (int s : kShifts)
                for (int mi = 0; mi < 3; ++mi) {
                    const auto run = runShifter(loop, (float) s, kModes[mi]);
                    std::printf("    %-4d %-9s | %6.1f/s %6.1f/s | %8.2f %8.2f\n",
                                s, modeName(kModes[mi]), run.anchorRateHz, run.spliceRateHz,
                                run.meanAnchorJumpMs, run.meanLagMs);
                }
            std::printf("\n    oracle:\n");
            std::printf("    %8s %9s %8s %9s %14s\n", "ceiling", "lag(ms)", "NCC lp", "full@lp", "best full");
            for (const auto& r : oracle(loop, 1.5))
                std::printf("    %6.0f ms %9.2f %8.3f %9.3f %8.3f@%.1fms\n",
                            r.ceilMs, r.bestLagMs, r.nccLp, r.nccFullAtLp, r.bestFullNcc, r.bestFullLagMs);
            std::printf("\n");

            if (!wavDir.empty()) {
                std::string tag = srcDef.label[0] == 'D' ? "di" : "amped";
                writeWav(wavDir + "/" + tag + "_dry.wav", loop, kSR);
                for (int mi = 0; mi < 3; ++mi) {
                    writeWav(wavDir + "/" + tag + "_-2st_" + modeName(kModes[mi]) + ".wav",
                             runShifter(loop, -2.0f, kModes[mi]).y, kSR);
                    writeWav(wavDir + "/" + tag + "_-2st_" + modeName(kModes[mi]) + "_noAnchor.wav",
                             runShifter(loop, -2.0f, kModes[mi], true).y, kSR);
                }
            }
        }
    }

    // =======================================================================
    // 7b. EXTERNAL REAL RECORDINGS (--di). Section 7's honest caveat was that
    //     this repo carries no real DI guitar; this section is the hole being
    //     filled from outside. Everything here is the metric set the synthetic
    //     sections established, pointed at material somebody actually played:
    //     the detector census (false anchors END the correlation splice), the
    //     ADDED envelope modulation (the audible warble, dry-referenced), the
    //     per-event dent, and the oracle (does a usable lag even exist?).
    // =======================================================================
    if (!diFiles.empty()) {
        std::printf("--- 7b. EXTERNAL REAL RECORDINGS (--di) --------------------------------\n");
        std::printf("addAM = envelope modulation (2-15 Hz RMS / mean) the shifter ADDS over the\n");
        std::printf("dry take, per band. THAT is the warble a player hears. dent = worst\n");
        std::printf("event-synchronous envelope deflection (dB, splices).\n\n");
        for (const auto& path : diFiles) {
            std::vector<float> x;
            if (!loadReal48k(path, x)) {
                std::printf("  %s: cannot read — skipped\n\n", path.c_str());
                continue;
            }
            std::string base = path;
            {
                const size_t cut = base.find_last_of("/\\");
                if (cut != std::string::npos) base = base.substr(cut + 1);
                const size_t dot = base.find_last_of('.');
                if (dot != std::string::npos) base = base.substr(0, dot);
            }

            // Real onsets (crest > 40% of peak, > 80 ms apart) — the denominator.
            const size_t N = pow2Below(x.size());
            const auto env = bandEnvelope(x, 0, N, 80, 8000);
            double pk = 0;
            for (double v : env) pk = std::max(pk, v);
            int realOnsets = 0;
            size_t lastOn = 0;
            bool below = true;
            std::vector<double> picks;
            for (size_t i = 0; i < N; ++i) {
                if (below && env[i] > 0.40 * pk && (lastOn == 0 || i - lastOn > (size_t) (0.08 * kSR))) {
                    ++realOnsets; lastOn = i; below = false;
                    picks.push_back((double) i / kSR);
                } else if (env[i] < 0.20 * pk) {
                    below = true;
                }
            }
            const double secs = (double) x.size() / kSR;
            const auto pr = probeOnset(x);
            std::printf("  %s  (%.1f s, ~%d onsets = %.1f/s, HF %.0f dB, rise p95 %.2f)\n",
                        base.c_str(), secs, realOnsets, realOnsets / secs, pr.hfLevelDb, pr.p95Rise);

            const double dryLoAm = modDepthDb(x, 150.0, 800.0);
            const double dryHiAm = modDepthDb(x, 800.0, 2500.0);
            std::printf("    %-4s %-9s | %7s %7s | %6s %6s %6s | %8s %8s | %6s\n",
                        "st", "mode", "onsets", "splices", "lag", "period", "delay",
                        "addAM lo", "addAM hi", "dent");
            for (int s : {-2, -5}) {
                for (int mi = 0; mi < 3; ++mi) {
                    const auto run = runShifter(x, (float) s, kModes[mi]);
                    const double amLo = modDepthDb(run.y, 150.0, 800.0) - dryLoAm;
                    const double amHi = modDepthDb(run.y, 800.0, 2500.0) - dryHiAm;
                    const double dent = eventDeflectionDb(run.y, run.events, false, 150, 2500);
                    std::printf("    %-4d %-9s | %5.1f/s %5.1f/s | %6.2f %6.2f %6.1f | %+7.1f  %+7.1f  | %6.2f\n",
                                s, modeName(kModes[mi]), run.onsetRateHz, run.spliceRateHz,
                                run.meanLagMs, run.periodMs, run.meanDelayMs, amLo, amHi, dent);
                }
            }
            std::printf("\n    oracle (mid-file):\n");
            std::printf("    %8s %9s %8s %9s %14s\n", "ceiling", "lag(ms)", "NCC lp", "full@lp", "best full");
            for (const auto& r : oracle(x, 0.5 * secs))
                std::printf("    %6.0f ms %9.2f %8.3f %9.3f %8.3f@%.1fms\n",
                            r.ceilMs, r.bestLagMs, r.nccLp, r.nccFullAtLp, r.bestFullNcc, r.bestFullLagMs);
            std::printf("\n");

            if (!wavDir.empty()) {
                writeWav(wavDir + "/" + base + "_dry.wav", x, kSR);
                for (int s : {-2, -4}) {
                    char sfx[16];
                    std::snprintf(sfx, sizeof sfx, "%d", s);
                    writeWav(wavDir + "/" + base + "_" + sfx + "st_Balanced.wav",
                             runShifter(x, (float) s, 20.0f).y, kSR);
                }
                writeWav(wavDir + "/" + base + "_-12st_Balanced.wav",
                         runShifter(x, -12.0f, 20.0f).y, kSR);
            }
        }
    }

    // =======================================================================
    // 8. SANITY: reproduce the old harness's number, and show why it was blind.
    // =======================================================================
    std::printf("--- 8. SANITY vs droptune_measure.cpp (220 Hz sine, Balanced) ----------\n");
    {
        for (int s : {-2, -5, -12}) {
            const double r = std::pow(2.0, s / 12.0);
            const auto run = runShifter(sine(220.0, 4.0), (float) s, 20.0f);
            const double fOut = 220.0 * r;
            const double fSpl = run.spliceRateHz > 0 ? run.spliceRateHz : (1.0 - r) / 0.020;
            const auto mag = spectrum(run.y, anaFrom, anaN);
            const double binHz = kSR / (double) anaN;
            auto peakNear = [&](double f) {
                double m = 0;
                for (int k = -6; k <= 6; ++k) {
                    const size_t idx = (size_t) std::lround(f / binHz) + (size_t) k;
                    if (idx < mag.size()) m = std::max(m, mag[idx]);
                }
                return m;
            };
            const double car = peakNear(fOut);
            const double sb = std::max(peakNear(fOut - fSpl), peakNear(fOut + fSpl));
            std::printf("  %3d st: splice %.1f/s, anchors %.1f/s, worst sideband %6.1f dBc\n",
                        s, run.spliceRateHz, run.anchorRateHz, dB(sb / std::max(car, 1e-12)));
        }
        std::printf("  A sine has no HF crest, so not even the OLD detector fired on it.\n");
        std::printf("  That is exactly why the sine harness could not see section 1.\n\n");
    }

    // =======================================================================
    // 9. REAL-TIME SAFETY. The correlator runs on the audio thread, and the adaptive
    //    ceiling added a second correlation burst (the periodicity probe). A mean
    //    that fits the budget is NOT the test — one late block is a dropout. So this
    //    times every 64-frame block and reports the WORST one.
    //
    //    Blocks are real here (runShifter feeds one sample at a time so it can read
    //    state around each event; that is the wrong shape for a CPU number).
    // =======================================================================
    std::printf("--- 9. REAL-TIME SAFETY (block = 64 frames, 1.33 ms of budget) ---------\n");
    std::printf("The correlation search and the periodicity probe both run on the audio\n");
    std::printf("thread. WORST is the number that matters: the mean can fit and still drop.\n\n");
    std::printf("%-12s %-9s %5s | %8s %8s | %8s\n",
                "material", "mode", "st", "mean us", "WORST us", "worst %");
    {
        const int block = 64;
        const double budgetUs = block * 1e6 / kSR;
        // The heaviest cases: the octave (most splices) and the widest chord (the
        // longest period, so the biggest probe range).
        struct Case { const char* name; std::vector<StringNote> notes; };
        const std::vector<Case> cases = {
            {"E5 power", cs[1].notes},
            {"E+G# 3rd", cs[2].notes},   // 48.3 ms period => the widest probe
            {"G major",  cs[5].notes},
        };
        for (const auto& cse : cases) {
            const auto src = synthChord(cse.notes, 4.0, true, 1.0, 1234);
            for (int mi = 0; mi < 3; ++mi) {
                for (int s : {-2, -12}) {
                    dcdsp::DropTune dt;
                    dt.prepare((float) kSR, block, 2);
                    dcdsp::DropTuneParams p;
                    p.semitones = (float) s;
                    p.maxLagMs = kModes[mi];
                    dt.setParams(p);

                    std::vector<float> l(src.begin(), src.end()), r(src.begin(), src.end());
                    double worst = 0, total = 0;
                    int blocks = 0;
                    for (size_t i = 0; i + (size_t) block <= src.size(); i += (size_t) block) {
                        float* bp[2] = {l.data() + i, r.data() + i};
                        const auto t0 = std::chrono::steady_clock::now();
                        dt.process(bp, 2, block);
                        const auto t1 = std::chrono::steady_clock::now();
                        const double us =
                            std::chrono::duration<double, std::micro>(t1 - t0).count();
                        total += us;
                        worst = std::max(worst, us);
                        ++blocks;
                    }
                    std::printf("%-12s %-9s %5d | %8.1f %8.1f | %7.1f%%\n",
                                cse.name, modeName(kModes[mi]), s,
                                blocks ? total / blocks : 0.0, worst, 100.0 * worst / budgetUs);
                }
            }
        }
        std::printf("\n");
    }

    if (!wavDir.empty()) {
        std::printf("--- AUDIO DUMPS -> %s\n", wavDir.c_str());
        const std::vector<double> picks = {0.30, 0.90, 1.50, 2.10};
        for (const auto& c : cs) {
            std::vector<StringNote> notes;
            for (double p : picks)
                for (const auto& nn : c.notes) notes.push_back(StringNote{nn.midi, p + nn.startSec});
            std::string base = c.name;
            for (auto& ch : base) if (ch == ' ' || ch == '+' || ch == '#') ch = '_';
            for (int s : {-2, -5}) {
                const double r = std::pow(2.0, s / 12.0);
                const auto src = synthChord(notes, 2.9, false, 1.0, 4321);
                char sfx[16];
                std::snprintf(sfx, sizeof sfx, "%d", s);
                writeWav(wavDir + "/" + base + "_dry.wav", src, kSR);
                writeWav(wavDir + "/" + base + "_" + sfx + "st_REF.wav",
                         synthChord(notes, 2.9, false, r, 4321), kSR);
                for (int mi = 0; mi < 3; ++mi) {
                    writeWav(wavDir + "/" + base + "_" + sfx + "st_" + modeName(kModes[mi]) + ".wav",
                             runShifter(src, (float) s, kModes[mi]).y, kSR);
                    writeWav(wavDir + "/" + base + "_" + sfx + "st_" + modeName(kModes[mi]) + "_noAnchor.wav",
                             runShifter(src, (float) s, kModes[mi], true).y, kSR);
                }
            }
        }
        std::printf("    done\n");
    }
    return 0;
}
