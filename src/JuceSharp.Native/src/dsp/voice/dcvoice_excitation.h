// dcvoice_excitation.h — what goes INTO a string at a pick, a finger, a slap.
//
// Two sources, one destination (StringModel::beginExcitation):
//
//  1. RECORDED residuals. tools/voice/extract_excitations.py inverse-filters
//     short CC0 DI recordings (Karoryfer, see voice/CREDITS.md) through a
//     string loop fitted to each recording, keeps the first few tens of ms of
//     what the loop could NOT explain — the pick, the finger, the pickup and
//     the pick-position comb of that guitar — and stores it band-limited,
//     12-bit, as text (dcvoice_excitation_data.h; clipSample decodes it).
//     Played back at the pitch ratio target/source
//     the comb notches move WITH the note, which is why a clip a semitone or
//     two away still sounds like the same hand on the same guitar.
//
//  2. A SYNTHETIC pluck: raised-cosine pulse + a short noise burst, through
//     the pick-position comb (1 - z^-(beta P)), the magnetic-pickup position
//     comb (1 - z^-(gamma P)) and the pickup's LC resonance. Used when no bank
//     is loaded, and always for the gestures no recording covers: hammer-on
//     and tap thumps, fret clicks during a slide, slap and pop.
//
// A recorded residual already CONTAINS its guitar's pick and pickup combs
// (they are LTI and commute with the loop), so the synthetic combs are never
// applied on top of it.
//
// REAL-TIME: every function here is allocation-free and bounded by the
// caller's buffer length; they run on the audio thread at a note-on.
#pragma once

#include "dcvoice_string.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dcvoice {

enum class ExcKind : uint8_t
{
    Pick = 0,    // plectrum, guitar or bass
    Finger = 1,  // bass fingerstyle
    Ghost = 2,   // bass ghost note (fret hand muting, finger pluck)
    Dead = 3,    // guitar fret-hand-muted strum ("chk")
};

/// One recorded residual. `clipSample(clip, i) * scale` is the float excitation.
struct ExcitationClip
{
    const char* data; // 2 base-64 digits per sample (clipSample), a string literal
    int32_t length;   // samples
    float sampleRate; // the clip's own, band-limited rate (22050 or 11025 Hz)
    float f0Hz;       // the pitch the residual was extracted at
    float scale;      // clipSample -> float, already level-normalised per layer
    uint8_t kind;     // ExcKind
    uint8_t layer;    // 0 = softest
    uint8_t rr;       // round robin index within (kind, f0, layer)
    uint8_t source;   // index into the bank's source names (provenance)
    // The recording's own string, fitted by the extractor (calibrated
    // commuted synthesis: a note played from this clip decays like it did).
    float stiffness;  // inharmonicity B
    float t60Low;     // s, at f0Hz
    float t60High;    // s, plateau above cornerHz
    float cornerHz;   // centre of the loss shelf's transition
};

struct ExcitationBank
{
    const ExcitationClip* clips = nullptr;
    int count = 0;
    int layers = 1;   // velocity layers present
};

/// Nearest PITCH GROUP in log-pitch (every clip within a quarter semitone of
/// the nearest one — each recording has its own measured f0, so equality
/// would make a group of one), then the nearest velocity layer inside it,
/// then round robin. nullptr when the bank has no clip of that kind.
/// O(count), no allocation.
inline const ExcitationClip* selectClip(const ExcitationBank& bank, ExcKind kind, double f0, int layer,
                                        uint32_t roundRobin)
{
    if (bank.clips == nullptr || bank.count <= 0 || f0 <= 0.0)
        return nullptr;
    constexpr double kGroupOctaves = 0.25 / 12.0;
    const double lf = std::log2(f0);
    double bestLog = 0.0;
    double bestDist = 1.0e9;
    for (int i = 0; i < bank.count; ++i)
    {
        const ExcitationClip& c = bank.clips[i];
        if (c.kind != (uint8_t) kind)
            continue;
        const double l = std::log2((double) c.f0Hz);
        if (std::fabs(l - lf) < bestDist)
        {
            bestDist = std::fabs(l - lf);
            bestLog = l;
        }
    }
    if (bestDist > 1.0e8)
        return nullptr;
    auto inGroup = [&](const ExcitationClip& c) {
        return c.kind == (uint8_t) kind && std::fabs(std::log2((double) c.f0Hz) - bestLog) < kGroupOctaves;
    };
    int bestLayer = -1;
    int layerDist = 1 << 20;
    for (int i = 0; i < bank.count; ++i)
    {
        const ExcitationClip& c = bank.clips[i];
        if (! inGroup(c))
            continue;
        const int dl = std::abs((int) c.layer - layer);
        if (dl < layerDist)
        {
            layerDist = dl;
            bestLayer = c.layer;
        }
    }
    int candidates = 0;
    for (int i = 0; i < bank.count; ++i)
        if (inGroup(bank.clips[i]) && bank.clips[i].layer == bestLayer)
            ++candidates;
    if (candidates == 0)
        return nullptr;
    int want = (int) (roundRobin % (uint32_t) candidates);
    for (int i = 0; i < bank.count; ++i)
    {
        const ExcitationClip& c = bank.clips[i];
        if (inGroup(c) && c.layer == bestLayer && want-- == 0)
            return &c;
    }
    return nullptr;
}

/// Sample `i` of `clip` (0 <= i < length), in [-2047, 2047]: two base-64
/// digits, high first, offset by 2048. A digit is '0' + v skipping '?' and
/// the backslash, so the generated literals need no escapes. Allocation-free.
inline int clipSample(const ExcitationClip& clip, int i) noexcept
{
    auto digit = [](char ch) noexcept {
        const int u = (unsigned char) ch;
        return u - 48 - (u > 63 ? 1 : 0) - (u > 92 ? 1 : 0);
    };
    return (digit(clip.data[2 * i]) << 6) + digit(clip.data[2 * i + 1]) - 2048;
}

/// Plays `clip` at `pitchRatio` (target f0 / clip f0) into dst at outRate,
/// ADDING gain * sample (4-point Hermite). Optional one-pole low-pass
/// (lowpassHz > 0) for palm mutes. Returns the samples written.
inline int addClip(const ExcitationClip& clip, double outRate, double pitchRatio, float gain, float* dst,
                   int maxLen, double lowpassHz = 0.0)
{
    if (clip.length < 4 || maxLen <= 0)
        return 0;
    const double step = (double) clip.sampleRate / outRate * std::clamp(pitchRatio, 0.25, 4.0);
    const int n = std::min(maxLen, (int) ((clip.length - 3) / step));
    const float k = gain * clip.scale;
    float lpA = 0.0f;
    if (lowpassHz > 0.0)
        lpA = (float) std::exp(-kTwoPi * lowpassHz / outRate);
    float lp = 0.0f;
    double pos = 1.0;   // start one sample in so the Hermite has a left tap
    for (int i = 0; i < n; ++i, pos += step)
    {
        const int idx = (int) pos;
        const float t = (float) (pos - idx);
        const float xm1 = (float) clipSample(clip, idx - 1), x0 = (float) clipSample(clip, idx);
        const float x1 = (float) clipSample(clip, idx + 1), x2 = (float) clipSample(clip, idx + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        float v = ((c3 * t + c2) * t + c1) * t + x0;
        if (lowpassHz > 0.0)
        {
            lp = v + lpA * (lp - v);
            v = lp;
        }
        dst[i] += k * v;
    }
    return n;
}

struct SynthPluck
{
    double f0 = 110.0;
    double pickPos = 0.18;     // fraction of the string from the bridge (0 = off)
    double pickupPos = 0.12;   // fraction from the bridge (0 = off)
    double pulseMs = 0.6;      // raised-cosine width: smaller is brighter
    double noise = 0.15;       // noise burst level relative to the pulse
    double noiseMs = 1.5;      // noise burst decay
    double resonanceHz = 4500.0;  // pickup LC resonance (0 = off)
    double resonanceQ = 1.4;
    float gain = 1.0f;
    uint32_t seed = 1;
};

/// Synthesises a pluck excitation, ADDING it into dst (which the caller's
/// string buffer already offers). Uses `work` (>= maxLen floats) as scratch.
inline int addSynthPluck(const SynthPluck& s, double fs, float* work, float* dst, int maxLen)
{
    const double period = fs / std::max(s.f0, 8.0);
    const int pulse = std::max(2, (int) (s.pulseMs * 1.0e-3 * fs));
    const int noiseLen = (int) (s.noiseMs * 1.0e-3 * fs * 5.0);
    const double pickDelay = s.pickPos > 0.0 ? s.pickPos * period : 0.0;
    const double pickupDelay = s.pickupPos > 0.0 ? s.pickupPos * period : 0.0;
    int n = std::max(pulse, noiseLen) + (int) (pickDelay + pickupDelay) + 64;
    n = std::min(n, maxLen);
    if (n <= 0)
        return 0;
    std::fill(work, work + n, 0.0f);
    for (int i = 0; i < pulse && i < n; ++i)
        work[i] = (float) (0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / pulse));
    // Normalise the pulse to unit area so the width changes colour, not level.
    const float area = 0.5f * (float) pulse;
    for (int i = 0; i < pulse && i < n; ++i)
        work[i] /= area;
    uint32_t r = s.seed * 2654435761u + 0x9E3779B9u;
    const double nDecay = std::exp(-1.0 / std::max(1.0, s.noiseMs * 1.0e-3 * fs));
    double nEnv = s.noise * 2.0 / pulse;
    for (int i = 0; i < noiseLen && i < n; ++i)
    {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        const float u = (float) (r >> 8) * (1.0f / 8388608.0f) - 1.0f;
        work[i] += (float) (nEnv * u);
        nEnv *= nDecay;
    }
    // Combs, in place from the end backwards (each reads only earlier taps).
    auto comb = [&](double delay) {
        if (delay < 1.0)
            return;
        const int di = (int) delay;
        const float fr = (float) (delay - di);
        for (int i = n - 1; i >= 0; --i)
        {
            const int j0 = i - di;
            const float a = j0 >= 0 ? work[j0] : 0.0f;
            const float b = j0 - 1 >= 0 ? work[j0 - 1] : 0.0f;
            work[i] -= a + fr * (b - a);
        }
    };
    comb(pickDelay);
    comb(pickupDelay);
    // Pickup resonance: RBJ low-pass biquad.
    if (s.resonanceHz > 0.0 && s.resonanceHz < 0.45 * fs)
    {
        const double w0 = kTwoPi * s.resonanceHz / fs;
        const double alpha = std::sin(w0) / (2.0 * s.resonanceQ);
        const double cw = std::cos(w0);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 - cw) * 0.5 / a0, b1 = (1.0 - cw) / a0, b2 = b0;
        const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
        double z1 = 0.0, z2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double x = work[i];
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            work[i] = (float) y;
        }
    }
    // Remove any residual DC so the loop never integrates it.
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += work[i];
    const float corr = (float) (sum / n);
    for (int i = 0; i < n; ++i)
        dst[i] += s.gain * (work[i] - corr);
    return n;
}

} // namespace dcvoice
