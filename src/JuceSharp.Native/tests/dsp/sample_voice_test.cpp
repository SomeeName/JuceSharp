// juce_sharp_sample_voice_test — the SAMPLE-based chart voice
// (src/dsp/voice/dcvoice_sampler.h + dcvoice_pack.h), device-free.
//
// The real packs are built outside git (tools/voice/build_voice_pack.py), so
// these tests build their own packs IN MEMORY from synthesised tones of known
// pitch — the format goes through the real parser byte for byte — and every
// check measures the rendered signal.
#include "dsp/voice/dcvoice_pack.h"
#include "dsp/voice/dcvoice_plan.h"
#include "dsp/voice/dcvoice_sampler.h"
#include "dsp/voice/dcvoice_timeline.h"
#include "synth/GuitarChartVoice.h"
#include "synth/ChartScheduler.h"
#include "synth/ChartZoneDemand.h"

#include <array>
#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace dcvoice;

namespace {

int failures = 0, checks = 0;
void check(bool ok, const std::string& what)
{
    ++checks;
    if (! ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

#ifdef _MSC_VER
thread_local bool armed = false;
thread_local long allocations = 0;
int __cdecl allocHook(int kind, void*, size_t, int block, long, const unsigned char*, int)
{
    if (armed && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC || kind == _HOOK_FREE) && block != _CRT_BLOCK)
        ++allocations;
    return 1;
}
#endif

double db(double v) { return 20.0 * std::log10(v + 1e-30); }
double cents(double f, double r) { return 1200.0 * std::log2(f / r); }
double hzOf(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

double dtftMag(const std::vector<float>& x, size_t a, size_t n, double f, double fs)
{
    const double w = kTwoPi * f / fs;
    double cr = 1.0, ci = 0.0, re = 0.0, im = 0.0;
    const double sr = std::cos(w), si = -std::sin(w);
    for (size_t i = 0; i < n && a + i < x.size(); ++i)
    {
        const double v = (0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / n)) * x[a + i];
        re += v * cr;
        im += v * ci;
        const double t = cr * sr - ci * si;
        ci = cr * si + ci * sr;
        cr = t;
    }
    return std::sqrt(re * re + im * im);
}

double peakFreq(const std::vector<float>& x, double t0, double t1, double around, double fs, double rel = 0.03)
{
    const size_t a = (size_t) (t0 * fs), n = (size_t) ((t1 - t0) * fs);
    double best = around, bm = -1.0;
    for (int k = 0; k <= 40; ++k)
    {
        const double f = around * (1.0 - rel + 2.0 * rel * k / 40.0);
        const double m = dtftMag(x, a, n, f, fs);
        if (m > bm) { bm = m; best = f; }
    }
    double lo = best - around * rel / 20.0, hi = best + around * rel / 20.0;
    for (int it = 0; it < 40; ++it)
    {
        const double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
        if (dtftMag(x, a, n, m1, fs) < dtftMag(x, a, n, m2, fs)) lo = m1; else hi = m2;
    }
    return 0.5 * (lo + hi);
}

double magAt(const std::vector<float>& x, double t0, double t1, double f, double fs)
{
    return dtftMag(x, (size_t) (t0 * fs), (size_t) ((t1 - t0) * fs), f, fs);
}

double rms(const std::vector<float>& x, double t0, double t1, double fs)
{
    const size_t a = std::min((size_t) (t0 * fs), x.size()), b = std::min((size_t) (t1 * fs), x.size());
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
    return b > a ? std::sqrt(s / (double) (b - a)) : 0.0;
}

// ---- fixture packs -----------------------------------------------------------

struct Fixture
{
    std::vector<uint8_t> bytes;
    SamplePack pack;
};

/// A decaying harmonic tone: partial n at n*f0 with amplitude 1/n, the upper
/// partials dying faster, plus a short noise click. `seconds`, `decay` (T60).
std::vector<float> tone(double f0, double seconds, double t60, double bright, uint32_t seed)
{
    std::vector<float> y((size_t) (seconds * 44100.0));
    uint32_t r = seed * 2654435761u + 1u;
    for (size_t i = 0; i < y.size(); ++i)
    {
        const double t = i / 44100.0;
        double v = 0.0;
        for (int n = 1; n <= 10 && n * f0 < 18000.0; ++n)
            v += std::pow(bright, n - 1) / n * std::sin(kTwoPi * n * f0 * t) * std::pow(10.0, -3.0 * t * (1.0 + 0.4 * n) / t60);
        if (i < 220)
        {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            v += 0.3 * ((r >> 8) / 8388608.0 - 1.0) * (1.0 - i / 220.0);
        }
        y[i] = (float) (0.5 * v);
    }
    return y;
}

std::vector<float> noiseBurst(double seconds, double tau, uint32_t seed)
{
    std::vector<float> y((size_t) (seconds * 44100.0));
    uint32_t r = seed * 747796405u + 7u;
    for (size_t i = 0; i < y.size(); ++i)
    {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        y[i] = (float) (0.4 * ((r >> 8) / 8388608.0 - 1.0) * std::exp(-(i / 44100.0) / tau));
    }
    return y;
}

/// Peak-normalised like the pack builder does (top layer peak 0.5).
std::vector<float> normalised(std::vector<float> y, float peak)
{
    float p = 1e-9f;
    for (float v : y) p = std::max(p, std::fabs(v));
    for (float& v : y) v *= peak / p;
    return y;
}

struct Z
{
    Art art;
    int variant, layer, rr;
    double root;
    std::vector<float> y;
    int string = -1, fret = -1;   // recorded on (pack bytes 28 / 29); -1 = unknown
};

/// MSB-first bit writer: the test's half of the zone codec (dcvoice_codec.h).
struct BitWriter
{
    std::vector<uint8_t> bytes;
    size_t bits = 0;
    void put(uint64_t v, int n)
    {
        for (int i = n - 1; i >= 0; --i)
        {
            if ((bits & 7) == 0)
                bytes.push_back(0);
            if ((v >> i) & 1)
                bytes.back() |= (uint8_t) (0x80u >> (bits & 7));
            ++bits;
        }
    }
};

/// Codes one zone the way the builder's stream is laid out, with a FIXED
/// second-order predictor (2, -1; shift 0) in every block instead of the
/// builder's fitted LPC: same stream, simpler encoder. Rice k per partition
/// from the mean, raised so every quotient stays below 65536.
std::vector<uint8_t> encodeZoneFixed2(const std::vector<int16_t>& x)
{
    BitWriter w;
    for (size_t s = 0; s < x.size();)
    {
        const size_t blockEnd = std::min(x.size(), s + (size_t) codec::kBlock);
        w.put(2, 6);
        w.put(0, 4);
        w.put((uint16_t) 2, 16);
        w.put((uint16_t) -1, 16);
        while (s < blockEnd)
        {
            const size_t partEnd = std::min(blockEnd, s + (size_t) codec::kPartition);
            std::vector<uint64_t> u;
            for (size_t i = s; i < partEnd; ++i)
            {
                const int64_t a = i >= 1 ? x[i - 1] : 0, b2 = i >= 2 ? x[i - 2] : 0;
                const int64_t e = x[i] - (2 * a - b2);
                u.push_back(e >= 0 ? (uint64_t) (2 * e) : (uint64_t) (-2 * e - 1));
            }
            uint64_t sum = 0, mx = 0;
            for (uint64_t v : u) { sum += v; mx = std::max(mx, v); }
            int k = 0;
            while (k < codec::kMaxRice && ((uint64_t) 1 << (k + 1)) <= sum / u.size())
                ++k;
            while ((mx >> k) > codec::kMaxQuotient)
                ++k;
            w.put((uint64_t) k, 5);
            for (uint64_t v : u)
            {
                w.put(0, (int) (v >> k));
                w.put(1, 1);
                w.put(v & (((uint64_t) 1 << k) - 1), k);
            }
            s = partEnd;
        }
    }
    return w.bytes;
}

/// Serialises zones exactly as tools/voice/build_voice_pack.py does: version 2
/// (each zone coded, what the builder writes since #408) or version 1 (raw).
std::vector<uint8_t> writePack(int instrument, const std::vector<Z>& zones, const std::string& text, uint32_t version = 2)
{
    std::vector<uint8_t> b(64 + 40 * zones.size(), 0);
    size_t dataOff = (b.size() + 15) & ~(size_t) 15;
    b.resize(dataOff, 0);
    std::vector<uint8_t> recs;
    uint64_t start = 0;
    for (const Z& z : zones)
    {
        float peak = 1e-9f;
        for (float v : z.y) peak = std::max(peak, std::fabs(v));
        const float gain = peak / 32767.0f;
        uint8_t rec[40] = {};
        rec[0] = (uint8_t) z.art; rec[1] = (uint8_t) z.variant; rec[2] = (uint8_t) z.layer; rec[3] = (uint8_t) z.rr;
        const float root = (float) z.root;
        const uint32_t len = (uint32_t) z.y.size();
        std::memcpy(rec + 4, &root, 4);
        std::memcpy(rec + 8, &gain, 4);
        std::memcpy(rec + 12, &len, 4);
        std::memcpy(rec + 16, &start, 8);
        rec[28] = (uint8_t) (z.string + 1);
        rec[29] = (uint8_t) (z.fret + 1);
        std::vector<int16_t> q;
        for (float v : z.y)
            q.push_back((int16_t) std::lround(std::clamp(v / gain, -32767.0f, 32767.0f)));
        if (version == 2)
        {
            const std::vector<uint8_t> coded = encodeZoneFixed2(q);
            const uint32_t codedBytes = (uint32_t) coded.size();
            const uint64_t codedOff = b.size() - dataOff;
            std::memcpy(rec + 24, &codedBytes, 4);
            std::memcpy(rec + 32, &codedOff, 8);
            b.insert(b.end(), coded.begin(), coded.end());
        }
        else
            for (int16_t v : q)
            {
                b.push_back((uint8_t) (v & 0xFF));
                b.push_back((uint8_t) ((v >> 8) & 0xFF));
            }
        std::memcpy(b.data() + 64 + 40 * (&z - zones.data()), rec, 40);
        start += len;
    }
    const uint64_t textOff = b.size();
    b.insert(b.end(), text.begin(), text.end());
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
    auto put64 = [&](size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); };
    std::memcpy(b.data(), "DCVP", 4);
    put32(4, version);
    put32(8, (uint32_t) instrument);
    if (version == 2)
        put32(20, kPackCodecLossless);
    put32(12, (uint32_t) zones.size());
    const float fs = 44100.0f;
    std::memcpy(b.data() + 16, &fs, 4);
    put64(24, 64);
    put64(32, dataOff);
    put64(40, start);
    put64(48, textOff);
    put32(56, (uint32_t) text.size());
    return b;
}

/// The bass pack's RECORDED SLAPS in the builder's layout (#109): natural notes
/// E1..D4 only, two layers x two takes, string and fret as measured on the
/// the slap source bass (E string to G1, A string to D2, D string to G2, G string up).
/// Each layer carries its own marker no other zone has: layer 0 at 6 kHz,
/// layer 1 at 7 kHz, so a render shows which recording played.
constexpr double kSlapMarkerHz[2] = { 6000.0, 7000.0 };
std::vector<float> markedAt(std::vector<float> y, double hz)
{
    for (size_t i = 0; i < y.size(); ++i)
        y[i] += (float) (0.06 * std::sin(kTwoPi * hz * i / 44100.0));
    return y;
}
void addRecordedSlaps(std::vector<Z>& z)
{
    const int naturals[7] = { 0, 2, 4, 5, 7, 9, 11 };
    const int open[4] = { 28, 33, 38, 43 };
    for (int m = 28; m <= 62; ++m)
    {
        if (std::find(std::begin(naturals), std::end(naturals), m % 12) == std::end(naturals))
            continue;
        const int string = m <= 31 ? 0 : m <= 38 ? 1 : m <= 43 ? 2 : 3;
        const double f = hzOf(m);
        for (int layer = 0; layer < 2; ++layer)
            for (int rr = 0; rr < 2; ++rr)
                z.push_back({ Art::SlapNote, 0, layer, rr, f,
                              normalised(markedAt(tone(f, 1.5, 2.0, 0.8, (uint32_t) (m * 11 + layer * 5 + rr)), kSlapMarkerHz[layer]),
                                         0.5f),
                              string, m - open[string] });
    }
}

Fixture makeFixture(bool bass, bool recordedSlaps = false)
{
    std::vector<Z> z;
    const int lo = bass ? 23 : 40, hi = bass ? 70 : 86, stepSemis = bass ? 3 : 1;
    for (int m = lo; m <= hi; m += stepSemis)
    {
        const double f = hzOf(m);
        for (int layer = 0; layer < 2; ++layer)
            for (int rr = 0; rr < 2; ++rr)
                z.push_back({ Art::Sustain, 0, layer, rr, f,
                              normalised(tone(f, 2.5, 4.0, 0.55 + 0.15 * layer, (uint32_t) (m * 7 + layer * 3 + rr)),
                                         layer == 1 ? 0.5f : 0.35f) });
        if (m % 2 == 0)
        {
            z.push_back({ Art::Palm, 0, 0, 0, f, tone(f, 0.5, 0.25, 0.4, (uint32_t) m) });
            if (! bass)
                z.push_back({ Art::Legato, 0, 0, 0, f, tone(f, 2.0, 3.0, 0.45, (uint32_t) m + 99) });
            z.push_back({ Art::Release, 0, 0, 0, f, noiseBurst(0.2, 0.03, (uint32_t) m) });
            if (bass)
                z.push_back({ Art::Ghost, 0, 0, 0, f, tone(f, 0.4, 0.15, 0.3, (uint32_t) m + 5) });
        }
    }
    if (! bass)
        for (double open : { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 })
            z.push_back({ Art::Dead, 0, 0, 0, open, noiseBurst(0.12, 0.012, (uint32_t) open) });
    else
        z.push_back({ Art::Slap, 0, 0, 0, 100.0, noiseBurst(0.1, 0.01, 3) });
    if (recordedSlaps)
        addRecordedSlaps(z);
    Fixture fx;
    fx.bytes = writePack(bass ? 1 : 0, z, "fixture");
    std::string err;
    const bool ok = parsePack(fx.bytes.data(), fx.bytes.size(), fx.pack, err);
    check(ok, std::string("fixture pack parses: ") + err);
    return fx;
}

/// A tone with a MARKER: a steady 7 kHz sine no ordinary fixture zone has, so a
/// render shows whether a recording of the articulation pack played.
std::vector<float> marked(std::vector<float> y)
{
    for (size_t i = 0; i < y.size(); ++i)
        y[i] += (float) (0.06 * std::sin(kTwoPi * 7000.0 * i / 44100.0));
    return y;
}
constexpr double kMarkerHz = 7000.0;

/// The ARTICULATION pack (#52), in the builder's layout: real palm mutes,
/// natural harmonics and pinch harmonics, each recorded on a known string and
/// fret, each carrying the marker. Natural harmonics: k = 2 (12th fret) on every
/// string, k = 3 (7th fret) on strings 0..2 only, so strings 3..5 must borrow it.
Fixture makeArtsFixture()
{
    const double open[6] = { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 };
    std::vector<Z> z;
    for (int s = 0; s < 6; ++s)
    {
        for (int fret = 0; fret <= 12; fret += 3)
        {
            const double f = open[s] * std::pow(2.0, fret / 12.0);
            z.push_back({ Art::Palm, 0, 0, 0, f, normalised(marked(tone(f, 0.6, 0.3, 0.6, (uint32_t) (s * 31 + fret))), 0.5f), s, fret });
        }
        for (int fret : { 3, 7, 12 })
        {
            const double f = open[s] * std::pow(2.0, fret / 12.0);
            z.push_back({ Art::Pinch, 0, 0, 0, f, normalised(marked(tone(f, 1.5, 2.0, 0.9, (uint32_t) (s * 17 + fret))), 0.5f), s, fret });
        }
        z.push_back({ Art::NatHarm, 0, 0, 0, 2.0 * open[s], normalised(marked(tone(2.0 * open[s], 1.5, 3.0, 0.3, (uint32_t) s + 400u)), 0.5f), s, 12 });
        if (s < 3)
            z.push_back({ Art::NatHarm, 0, 0, 0, 3.0 * open[s], normalised(marked(tone(3.0 * open[s], 1.5, 3.0, 0.3, (uint32_t) s + 500u)), 0.5f), s, 7 });
    }
    Fixture fx;
    fx.bytes = writePack(0, z, "arts fixture");
    std::string err;
    check(parsePack(fx.bytes.data(), fx.bytes.size(), fx.pack, err), std::string("the articulation fixture parses: ") + err);
    return fx;
}

struct Song
{
    std::vector<ChartNote> notes;
    std::vector<ChartBend> bends;
    ChartNote& add(double t, double sus, int s, int fret)
    {
        ChartNote n;
        n.time = t; n.sustain = sus; n.string = s; n.fret = fret;
        notes.push_back(n);
        return notes.back();
    }
};

std::vector<float> render(const Song& s, ChartTimelineVoice& v, double seconds, int block = 128, double rate = 48000.0)
{
    PlanOptions po;
    po.bass = v.spec().bass;
    PlannedTimeline plan;
    planChart(s.notes.data(), (int) s.notes.size(), s.bends.data(), (int) s.bends.size(), nullptr, 0, po, plan);
    const VoiceTimeline tl = plan.view(1);
    v.prepare(rate, block);
    std::vector<float> out((size_t) (seconds * rate));
    VoiceClock c;
    c.rolling = true;
    c.songSecondsPerSample = 1.0 / rate;
    for (size_t i = 0; i < out.size(); i += (size_t) block)
    {
        c.songTime = (double) i / rate;
        v.render(out.data() + i, (int) std::min<size_t>((size_t) block, out.size() - i), c, tl);
    }
    return out;
}

// ---- the zone codec (#408) -------------------------------------------------------

/// tools/voice/build_voice_pack.py codec_golden_signal, regenerated with the
/// same integer arithmetic.
std::vector<int16_t> codecGoldenSignal(int n = 5000)
{
    std::vector<int16_t> out;
    uint32_t s = 12345;
    int64_t acc = 0;
    for (int i = 0; i < n; ++i)
    {
        s = s * 1103515245u + 12345u;
        acc = acc - (acc >> 5) + (int64_t) ((s >> 16) & 2047u) - 1024;
        int64_t v = acc + (i < 2500 ? (int64_t) ((i * 37) & 1023) * 16 - 8192 : 0);
        if (i >= 4000 && i < 4100)
            v = (i / 7) % 2 != 0 ? 32767 : -32768;
        out.push_back((int16_t) std::clamp<int64_t>(v, -32768, 32767));
    }
    return out;
}

/// `python build_voice_pack.py codec-golden`: the PYTHON encoder's bytes for
/// that signal (fitted LPC, order 16 in the first block). Regenerate it only
/// when the stream format changes on purpose — then every shipped pack must be
/// rebuilt and the pack version bumped.
const char* const kCodecGolden =
    "438d6c406cbf637fd7804cff6f391f45f57fde3f9d0006bff97faf44de7bc67fa9981047829493a1176f99e7401d629a"
    "59e4025ff3a2cc24669d26c6c7c0ac5882f577137ad9327e4a66224de2ef0c3f4c057de2038deb514bb10e2e740817c7"
    "c701d04e9ab755aa185682d6d81c81bf3790eeb5074cb8019284f3901c6a896be02377af2bd4206d135c9b5090f18e65"
    "30655d19364e868fcfc67e2a96561a4cca7f1da8c922185ca86f99640fc831f80714ed00867611a9788c0212c739f769"
    "30686756b245d8ce174e419b1456ee4803b59a884f6ba90d6530c8b9d3784072fee4997895e667256db7e327fa6c8f2c"
    "da2331300b030803217d02f4553e0598af6666597a266d922854815a2752808b0d5214a8bd36f491eeb54131bc7c5abb"
    "8ad2cac246650ef36df905a415119a47314d444abf148fe2600f423eacdff01503d6d53d100c91b769b34c5cff6c109c"
    "9565a3b68d708166642134f13baf527d49396e1129b8e3d2354c1e7eb80b90c2fade97e4bd573e38ec66c64413979ce6"
    "7d25057a43ea986ed142309f6ec0d632ae9d998546949e63516945fe80c35c0db8f1d1a9d6788abbc587667da18974c1"
    "4e31707134ed7eafb85f7b8e9a46d967c531788df7cc103c6f2184203a8f03e833cd06fcd72e5cbf01adfcb72d5591a8"
    "572178e08ce826d886fc267149440375d13cdaf8b703ef1b0c001d97597645e8f0a1505ef80218d23aedfd43b45713e3"
    "6840c43adb1aef2212b237a93144587691578e3c3d21ff00004b9a1a220658ad81b817660737d1ad2a57d66b6ad41887"
    "9624798548da21396c0dae42113c471717242dd9bb0220800144f0eee4c2283823c291380ddf99bd26a519639d8a7f0f"
    "b8af6fc6f3a61e388b271c148883619950fc47380002ace22b86964d99501a82f0645d55d1ee4bc5962dffd823e29762"
    "86c473a172cbc4480466abe32499fc5fc1710000631d85434d4362d5727388136b0c93e7dcf364ab7920070e29fa77a3"
    "2a20a5a187cc3279360f1aa8fe03e69efc000d75aaeb1ca644b96f873200869932d4e91e82b46ef4586ea69046e4a946"
    "b29d986d9acf548c04e77e218a0001aaeaef5a452bd69d7be2801036bb98fd8d14bdc74cba84d94967f956b2439344ef"
    "e5f27d51ba9e7c6fe3e98702000580d0d2012921444d7e58803d663b2d17486baf0927fc381a0291fcaac5344c146256"
    "038a127ce9239439e90b000069adb557ba356ccf857ebc0207505a8cdf7fb7141cae874e8093152fa2e217f51322312a"
    "3aad00a4446513b8000762a7d538b5a3faf39d0c606de91fbc1b7fbac752ace91cfd58b8bd0b713ec0ce7eb569f6b745"
    "5261739a1ee8000c61e234c7e324e0529ec381d6f41fe231aaa9b537ebf8555405daa23c35f20351acb4093c73520c18"
    "68424083800031863a114ca09a914c271f0083541223c64b66ee1790ee13acc52cba47536c9dc0ec62131f5029789641"
    "b9c38a000293d7d04d96d337f9eb04b903e1f38115d1b5f8caec43f07171561a48bacf62315111043518e020d0aa83f4"
    "0e46145c00324f240cfa4c767f7ed7aa06a6d3126f2de3f6a54f0c80b242633fc92bcd507f8161cf263b38a602401314"
    "6270cca000674e5434919e89a28fa99c0367e66a8df47226f94a06016d92755eaa4842712087d1191cb66eca71b0b6a6"
    "6b0005d3c13af7ad21174a12a3d0558c704b8dc85b7759fad03b59f750573a137bee896eba64ec74921316e04ab36400"
    "01f2f2e55a73810e75e885e03f0e25fddfec54677ad13414516af81f5ef5cdc55264fc8e8aa5e5d87cc2132de8000f53"
    "a34d89a22c81882991010dbb42f4d425c8f5c7c6182b53a9e4335aa742e3997feac378b942649f2f8c92900893e77fce"
    "98348b7e8a329c251706d9c94542371ac8bef157af445cd34f88d627ade4c4c01924e5e2734afa2d04df70373ffceb6c"
    "6bba6fc8147888a83f8aba2d1b886e612ad8866210c8d8d7af10aeb37271de9e49792146a894c66a617ae0334fc86c9b"
    "5b7b304ac55e18c58eeaaaf96f699c428a88c24308b179bbb55b76dcb5f734f9e0edc578e1cf062680d7ed52b855900b"
    "5050138cc52a75fd9b69bd4fda09f22811c0e50764ae41b76ca57f5b7889c83559e2c6dd92187c780bb1d519072a9b71"
    "ab329bb227bb49fcac6e12939181af518a0c20222dc3ef656f7904aa8c2301e2ac902228cf2314c078472fd38d9714f0"
    "26813523c27261cd3c4626df85dce3989cb93d59e651696d71493e8edbeae587931814f9e00b7eea975f1613a985be22"
    "2e2f471ddcf6d5e72af8edaff98e0af140ab04d49c8c84852b41cc641031e897f6c117320186bb5045368774bb9bc494"
    "85a7780ba4fe5be646183e736d2baab519aaed7468db2b5b45d6e69bf2de961c5cfa6403c83e65957fa07d4bb2fc9c84"
    "6df1bff235a1d5455aa5b0f515b33aa30dfff21fce19bde6e62931ea88b604a31eb00016eeeafe8361bfd6cb891a2017"
    "cef4f2604917bcb6889282327276ad2eb2e5722210e89dc20e3583f3b931f946c100015db57d1d4839d05807a760100f"
    "39d16f82697f9ade5f1f556d99e4956c5d5302cd25c961884a59630e9eec7b20001113617aa2807f78af0c5a0716ad5e"
    "77bb56ae3c47d1c17f57ccd044c45e241d2fe95ec4b02088e303b70a340836000d07eda51f3719504b4044603e3ed39c"
    "8d0336472e92c01da1d716649e09809f0b5459d30519642293d30c4e0002aaf51f6e272a1ab106b040c33feb8064b6a7"
    "077ad6ca19f4990e7c99f45936b1b8cfc840025996f4ab23028eb400050cfc7e95529219ec5e8500d2f9d0c29f4159da"
    "e75b161dddaa530dfc15ff7edf0cbf08807701bd85712dd8d12000da54d5ae6b3220790513180b29876cb5c42a09efcb"
    "3404235d06d390b4a0a6d634c9a29b8881c206126027a0000a0bde997bb444f9c33fa100c0d727d0fd431023361eb232"
    "1abe70513fc8fe59e20a908853f2f51ee02278c1e79c0002cfe02ac737329066229dc0539bf4d4931abc6b9fbf661175"
    "127a3d4a523b8ecb4c42d22dbec091e63726a9838000bf9c0490abd0a8eb865c8081817fcf1c584b335ebfecc1319501"
    "8ac8f0aee83322f9f187501cb023b11942b500008435eb5434e85a2380a7301f4fa2b2d4a47532a7ab7042e5aa272316"
    "d4b1070d322252099c4bb11c042610d8e46000ec5b35fdcaa8ac796891c0622a73f6e24050b78168331ad0a709294de2"
    "1bb8894c980466347518c04501eec4a5000277f6496cd53f255de48a307e0d6d765431462627e2886c755a7b120938c9"
    "d8de9dbb440814385c937c8a620002abe134882c3467c29f3ab80a8b925affb67f1caedc6104eb364a8d6b325715553a"
    "e4d7211117cbbc22f041c173400045eafc6c66c398b1590b3401c6bc1f75b8e304b2cd9d2150d69dba96ed031c519e8a"
    "124a123389e04d63b615f8000e8588cd1dc24efde4c61881e7355fa77ed2c23fc774d87c071c4d9c4c78127eb913b213"
    "59128b7037c8ba2000d7d58eeab5a9c8ac896ec03a1e9d6f8ba0718706d0380a94e268568a25a46522699c38e5662519"
    "dc2310b2e2a780010c7b20eee6a32efb9b10741e338abf0131752daa3aac05e06ba101d4acfc85f23fb0fee3da8d7010"
    "1834707290b1400097970e39313856d0f7cbf2034a61aae32cbee8f6f17dbe1005e5dc50d8650d21e17244a8154c5f41"
    "cb0c022a60002a2f46bff210f4a21055c80c7d964ebac46c3346c58e07423b492706cf24f1395a385691fe4a0822904a"
    "53989df800088b88b922aa3eb44106e703ddf81eeee1b61337faa2409c46fb55431b928897023e1e789e47592bd9a586"
    "540a1e000ddf410f88704fbf770712810c744d485e3f1bc9c95c08ab672c2d934713fdbc5f6cde635c9abc78e098e3f9"
    "8002c65190ba2b137035af3b781dcf2cdb51a6d4b174fe141ab44d4103c992ce6e5470b3f90f563f110405511f4078e0"
    "00516cb832dbc59c38fb4ba60ed1daa9028fcf7e9ef97c02ba897485c7f811a0f7e2598be429d0bbc2f60aac17ec5970"
    "0056db513fba4bd404db6445062db0d2c8ee353d9c849d40f7069e12b091242814ac75426b9fe46ec1d54cce1750000d"
    "0985cd7240b2ba96688700103fc393d12b1e18d6e84207e837e9094b4a77c2efbe10c0cfd856710ac5a7113874e00063"
    "2c55fba79e58aff722a80a39802aa1d6dd5994ac0e05854ad444ebf49649b3971d619b9dece96ea27632f900015eb7d0"
    "8f2c84ef2a94c8c07b0edba0423790659f0cb80ce0b1b8b05ea28c8d726229c8f1727b70cc5b2cc8ea601a09ffd6eea5"
    "e1a9abab6430fa1dbda4fd757bfd5e8a68c780ad09fc2f58776920c5e891225231ad6557971c68e9a8c035a28156b835"
    "39960da258a8f4dcff33e17b8a2b4ea644265da3a1bcaa83215df72fb129c48502844a0f26d4b7c383c06607335826d4"
    "f659a6e132615f3c0f5832ca54ac8eb5028e0928bca37639b308a802610b58783129858e1e347d700a18ce937bdc0c4a"
    "2b2d5ac23153854b6352fb23d2f684098cc4b4a2957d6bff78a2d25630b9468a9024e53719182c036ff3e0c44501b02d"
    "af4e8208dce2fb229604c3161a2d3058b26dc93f29b493a7dd3d34bd96e21cba4ba1aea4d1988202b4747cf8b4c1f795"
    "ed0e241c5f931dc1a2aba54f027478d7c6dd2ff4a0677611ca4f03f28ed62bb0b862554aec0104f96c72a3a01353ca96"
    "830f9acbd6a4926eb7147f613e4f3a7296e68ba66bba5d0c3cbc630a4414c8f152ed9df2039c713cb0f5f464ab968a12"
    "9e0bb1fc3b208117b996c5a0d476ef30e1ea0ff1544b645b90f486f4397cc1f36a094e012c5ab069d294d8f0f9460e8e"
    "f3e51a5d994c9dc41daec0619387da544b894be45d98c2410b884c2539a4feb3cbcb00178f8884f2114dc64d78bfe04a"
    "9a6b33c16f1aa38d9f2c0d667d37cf3b61714b8c2cb97184f43d58c523a88eaa2e88000b71ac155a29baa2899ae30012"
    "9aac758242f79bde763387757c52359804a3e8da5c22d80f4440d19905c434508d80004e7a71bbfa26a0bfb93a080ea7"
    "8f4811942b4cb9a9950f72f10cfa7093363f7a331e7c8fa4a81c345783db84ec000649f4cd94ece4ff4363638125f3a9"
    "8efb879ab0951460a9027c30aa48166c5695733e20e8a2928bc430c844f0006f8ac09ef499e4c93149d041933db8aa28"
    "af9edc8673c02e7141046f11bc88c6941da8a04399867204780b861ec827000019f250134a8824ef02da33301fbea2ac"
    "9ebda88bb4a90d056d3e41af93edd08e90232bd60e044d128d08dc7b01bd40006dde32215ad867e71bae4c05f7c3ca3c"
    "a05943351ab4d0f1058a496b67142937f8c0409263913c81823db8f0001a4b1b1535b892dc92aeec0577e964dcc032c0"
    "b7592cc059a2bcb663738c08cae74711947401708e1234e187044d000318f00a5d9401a22b56b97013f6f0184e8a189f"
    "fd2cac1e3937b9f9fec4222871c59d84a16ef85193740a1a7cb00015b762a8e4b2dce586f5dea81ecb2fbdc35051ffcb"
    "f21a1319709361f75414c8c55a4715361c68b42eee577000282e02a78e16797e3f9916023664d160e42fe2ec0d22c45b"
    "553151b5f0acd2b1bcb1560c085b83b4144497468f0002977c2b97687149309586e02e7753486f261f1e720666451923"
    "4e132f3563d2f2eb06da2bb170c7063b11f28df200054ce845a761c750c9f54e81eef4f0847789a12775fddc6ee27da6"
    "874d7eba0fa8010c095abc858f3548806000676e50640ab16287e05130061da82d976b6f45cb79f9ec2ee36afe0e2235"
    "87a2fe3236667b399dc8a2a47c1418000712b49e6ad2ea87fba0b010149b89ee233644a03dadfdf09d2f16b7dbdba0cf"
    "d84e02daae7164093e2d98ccc5b200035260136d365d220a9a80b0316fb32e130633fedb7f601310adaee853254ac0bb"
    "a1112ca6a6912a78b646c7000198f61b1a74a995f9c166203425322d844f6f6ce5ba6247614ce555561c606f0e4fcb1d"
    "d73ade4e2ac6da6cdfa7a6873b742af2135997e8328139c7eb2d7047754b57ffe25ef4b6500e76519c79e9f431da85bc"
    "6695602682ea60a2c1a1f52251b4ad8472520f326cde872a74abf12d13bf9068cc23523b7e0d6764e131a2a5ef453573"
    "01329eb0e30959b09718c5d960a147a2110b808c05e0165ce7df528b4ff48c27455f714d6be1dbf94bb10f5479f5bb7c"
    "c414d5a5074b9b01d6b36759579ce4a22c09f16ebe5663d88c9bc8de972da88cb3c95460a1755d20a80e2a77e78dd858"
    "7be76a1a2cedda17b29d9b6dcee4d7745d497bd21ebedfe2a89b4e4d7509070b190d019b5968555bc33e4918780a7549"
    "3f9fc130178eed3c40a54411eea65249ed4c8c245420147f86932c80cd9b6139973c66178caa33e09cbe1960bbf38d27"
    "5d39976e4de7bb25aec6b5a666fe6b60af8d31f4570f00ff87b83a317ca163ea9211df15b980aec1220b0d35ac2c9973"
    "26fe65c1639913c994c0b7030525438f088023106d834a04c463acc86301061871bf8550964f9893c71e9a37770d942a"
    "854b78f5d51eaf57da28ffb0b333bb5a4fc90710022c11689b42206187a0b57c10bf48bc8e4811b666c4b12ef32b64d9"
    "6fac5e9bebc644eac04cc9c559192d6fb2bc76da48a65c832d48bf9acc9e1997b6299c7525c504b6aa03133b91dbfdaa"
    "cf9774f12fd74deb317ecb1efd2e067dfd7a99741ef8b29adfb2c9375736e5d76b1d93e3b9df7fec7e41a908e4d30981"
    "16121346863423c7b215609a1e477f3062463dd09f979c9293cb9e426a3a8809cc47cf13de9369f7e338904dfecc5b40"
    "d11e00d494072caf74054ae5fa6825814d261c76a29428217a1bf0d61419380b4060430a0f84646fb87a63c80ebc22f0"
    "6660adc40a0c50c8c1f504a812104ba1589f90f30acc25809cc3281f09b59be3c18fd0132067874a68ee809bc4742fb5"
    "cf2950ac09c0ce94ece4ca625aa20d2f94f5e75b21d4a8d21cd70bbd4dd62e4cbd1e4081f87bd0580561e1355ab4aba9"
    "5190490918577e2e83d8b3d2a8cacba1ab608cee2017816b2acf6c929c910813a2215ee9be8e4ca30eab30664792ad04"
    "22f5cac7fd320cb35604bd79a90a5dab69cd660f6348821f202310233aca1c8124b6a20c54bfe91323914ea3033b4741"
    "37164ef044d311751b92768d4f4221128ce8b2b9be9c3097e6878b9bb85d2924a0c5cc42f2efc3657cede71bb553bde8"
    "7d35d3846559ce8a9fd4df56bbe4b58fa10721f1f8c4b46d7b38a6eb37eee58430a90b71f5a88d99b08aeaa0cc240f88"
    "cf025b751f4c18ae525138443b6a36f3445f0d639016e93fad0a990bef51ae8b1a1c1d007a98d33e2487be1258886320"
    "2d3703329bb4d9ba52876b5499c4fff3f7aaa117e522d161ef53d5444ebee2b6c1736c9c3756936cd238b305a597d774"
    "04266afc42a365f79978dea0e9368ab85870f74cc2ab8648ead6286185babc26146e4a1d81e90b0077af3d0b524cc710"
    "4e11feba7df93e87f6adbd476f216a561eb3cb2957946f24cdb4975e241cf88e110d7c577e8df5af4ac04aa793b91439"
    "a5242de1258d8c782f3017374810673b6f3180d5e0b4eb449168f9b838cca3fc716b253cdd63ae85efe70a44c978ee81"
    "35b9d2dd7936b3c1a5a06edea10954d8497912eb2fd63ae23d20b20b3ccaaf20309086362a609250d08d4536b9d941e9"
    "8d2c49ac5ce4536d443c22c7703831b0bd669a55ce6d57497ced0f85717f0f3ab813cd47b9c2d72ba0a55707f77045c0"
    "8d90fe02f2a7ecc5988da76e1713e3efa124e7fa71739db57395e26f992925a5f38425311d3e334b2831e10eb45ae23c"
    "5d62896541459fe04d03f30d10e9c7560b79b18f0c70c1521dd839f064e14105b61a786201b2012f814081a703bf02a9"
    "069435d16f01d28d18510189841068817b1f0ae00ac83cc694d9c79e8e383079a13691765d7dfb678e994d7f9ea56f92"
    "85a156d45cb7ed1207a98a163958d866593658e2a46b117c9bfd96d0bbe8c7e265a969ffd5d4e16398932473d0a0a0b1"
    "e092669b1fdd3147e7078f25e89725ed4c115d4fb2e8626f8fd62c191d4e22cdb20c3a8af49c7dbbff797dd746e9eb83"
    "6091a4db662d987b6959d54e69c6c795bfd2c574707a9413d3e02002a34f1bf9ceb44aa164af54432c2fb23916444c2f"
    "09b3b8140429e1241dcafdb2e1a82c256413f21dbaa30a2ad6b4eaf8f5080c1cb4cd44dfd5cad10c513ea6d1c3845c10"
    "eb162416bb7937864d8f957b32876b7a6b82928a6c6b81731791f6bb19000898a0d228fe8f40b71df0c9459875492d01"
    "937904c92062632961853b68428153831b1a20b40e287be6e45b04aa460642145121ba449cd4536bed145345a2d6014b"
    "0fbfd5de93e6bb691d200982c9ff2976eda3c8c4dfb8eace42d50820f5cb37da52a581756abc5edc1c5e70db07cd8944"
    "6f8fb7d7d79dd4c1a86235e8a7a94a47560591ee12206b24cea932d5ad43d95307189c1d30e329fd66cae89055a7ceee"
    "c6ac835099269da5e4a461556da4da888ccd010dab9c76a92844ebd19088e42178ea58cb88a370300c68ce7abfe5114e"
    "952a65ec8038e290fddfc0c9d89fd3fea4945f65866fac13e7a9968b98363d04fc39ec89dd0fe284c9cc44042b05a4c0"
    "c7bcc462980e0c2d9325c19b14bc27243531b8a512a820b4c511ba7be00ebd35c5767e221f54cba3581abfa3c5af63d2"
    "9993f5c90f5245faf5d7bfe494df5f8785ec8d53df59cf5279b65528ad8a922f4ac1d06926fd0b444c67157a2bd12736"
    "1bd8529b3c4345f2caafaff1595f8a3140fa1b9162a8049e60b38a1a91d90e01f3e69cceaa7b2fc81ae9d157a17e41f8"
    "1d9bf363771a78fd4e63bebda3ac77fe7fce1ff1c8922c352190a735797fb878b95e074db2969f1e7566f9373cfecf16"
    "e378ef9e9c68f2df4aa96fdbeea7b8c1cbbf5de65ba7fdfbd69651b3a9fc1f736c0b78fac7cb865ed1ea9fed356f6744"
    "8822dc5a6e12dc963456a87b2dda0c60ae851136e2b52865ef6718004d7a40331f8e9d56286b1f88ba4eae5931c483fd"
    "8467d37a78905e5226f32c9c9c5aa1876eaa5352fb7619be85cc16a23f758c416b4b6392078c7d116d09012941434a07"
    "d27286f535e1c1680a4c52933f59eca6ac6238394c2b1a53fa18339a9b5d8daeae00012ee3028faa1f52000000045ecc"
    "07a84a19546b7fbc2e1d800064f17fef460d5ffc2b41890a00b04f98eec7e9b51ef41690980b067992ec9e9c51f34189"
    "0a00b04f98eec7e9b51ef41690980b067992ec9e9c51f341890a00b04f98eec7e9b51ef41690980b067992ec9e9c51f3"
    "41890a00b04f98eec7e9b51ef41690980b067992ec9e9c51f341890a00b04f98eec7e9b51ef41690980b067992ec9e9c"
    "51f341890a00b04f98eec7e9b51ef41690980b0619c5e4e06e60bebfe61f1340161c25c3f4a00b20471fe800286b009e"
    "8048c2cc176000002004ca6000000003b602da8101521fe9c0002d2e367117f6f2b53dcaf6c96c08b79b50c75b9a944f"
    "72856efefaad3ce6ecb7db643525893792e0a00e4dedc480249a6034c354b58a1469e73fbcd9e2c8384ee45fef8d142f"
    "5169dad49ea5474012ea91a8d964f3132c4e072a79f2d30a364cd3cb92d554d2b3a9d9e1761275a95c8b859aabdd35e9"
    "0adc99eef147705d674d7125bcf285f6fe44981ce3e496a3e3079bf0057d629d4622a6c7f2a61553f95330ad67f199c1"
    "483a5f94e4f7ef1526324d31d271a3be9ca1a106970302f57f7433132ad119d05edf0d52dba47782905b9d471fa2d762"
    "ef5e16481b6adda2cb35999012ddf31aef7c9d36d8e90dbca7a508766eadfd939d6e1f464ac863b732d2b7b7754bb432"
    "ef35b10f09ba555dbed7f6f24b6612f1c09b8327b555c219fdb0b30982b35eae6ffcc45c8bb18d44bdcbe89ed915bb8f"
    "d84f9445f0e64bdeaf00e7f7e54234d8262e89ad5ad7afa647eedf1a9561585f0b1d5257666b1c999e6284dbdaf7d933"
    "4fc6c06aa562e467dcd5c4e8e76c2d7a7a02951071ec2d249c49a37a095c5796dbdbf55b22b98e497757bb574034b911"
    "cd825058317bd8cd8acfb23a8bd4bc8fd55cde1e527c12453de4fa66612b292cd6ec593ebff3cc62274a29fffc79f8f2"
    "6774dd3a9700cfb461a944585034f398a625f09eeb34fb064d8c659d21d6733caee7349986cfd2a0a2ec89d69f23909f"
    "ff99906eccd49bf138e07b7e3a45c8df7d595295822837c4c941c36a39163f267f6de1a928eb49166033a8d92a2bf3f1"
    "6d2d787ce33c75a155ccaf30fa090e5f14acfdcf4ede87031946311a3d5111126b48a889a7321a3c2ba71948bbc6b66e"
    "dbccd89d2353bdec73b8887098e8f74c9a28c2aeb9563d9a0fb0211cb54c792367e64e3a6caa58df280ef0aa63102ddf"
    "f1833425f5dd2577599f0ada37d5ab15049824adade67f45b6605f289338b5e66395f5c54b12ebb7abc3c74b4d916d54"
    "4e60741714ced4f93255c65c577e1fffd2d972f0431372359fea69348b176b22c446864a5f2a9c89a660bb7a48292041"
    "d4d27d721697d525bbc70f55143d2ec8291a0de9ac6681ee67ab258a45a11747e029a2917bcd110abce9768b25efb219"
    "b0dc73357a50b82ad9adf9cdda9c6d7a8a79bf5650967d573199db642d6934c76fe77c6ea4bf5d9bbed4d11a88dd132c"
    "c24b4477fe75823939eff0b0ef9ad291b0a9d5cbb271e7c346261e14689406e8fcceb165063008155dbac696410c09a7"
    "169894e4bd646455489753b78e7e64fb06d4ac1bb5c877814c1f932f15587c0dd82102480c78ab526d559661c791684f"
    "aeba855a1358b013825b09a44208ab7f67f496f6d444b156a9f31d499e0bd88225204321b49b185e8e94800f58bc8914"
    "909d6bec92b1919cd36515b1141576b4e286b8dfc532443d1cb0ea704964979987a1f8779928b061545be51ff3d204a0"
    "b276235e33a7c508d907d010e0890832dad00dc3b513cd3a66a52ac25091d2f111009357cc54ebc075a7e422e0994494"
    "490db18465f9ae871818c37c22b44f79a14287c63f10d35b905501b886b046618d9b50f3080832222a2273e5279145af"
    "f1c82ad1bb9dd0500650e1421e2ace155ba10c80aa5e08574770b69cc82001b00aa97da5bd4b10884bef1d887582c46b"
    "a2201be395cf1e9ca588752228744866b22daf9dfb4ab53357cd15b55b5ee3f0246b4dacc28eac8b9ab402ae95044599"
    "0b0f4af4a3bc3e65991f5af48919421f3dbea3a15b7f0114fc0b2ff2d4631aa631263c7a47058b901478ae4d319a4bd5"
    "49d1d999b849538a82b80b9c54e3c1283e41389a6b1c6043ac79c5ccf776e4f9891092e42192501d11930b4c4e438208"
    "7811c0af867e5aa3b218185403ee543ef4fbc746ae549ef9c290";

std::vector<uint8_t> unhex(const char* h)
{
    std::vector<uint8_t> b;
    for (; h[0] != 0 && h[1] != 0; h += 2)
        b.push_back((uint8_t) std::stoi(std::string(h, 2), nullptr, 16));
    return b;
}

void testCodec(const Fixture& g)
{
    // The Python encoder's stream decodes to EXACTLY the signal it coded.
    const std::vector<int16_t> want = codecGoldenSignal();
    const std::vector<uint8_t> golden = unhex(kCodecGolden);
    check((golden[0] >> 2) == 16, "the golden stream's first block is LPC order 16 (the fitted predictor path)");
    std::vector<int16_t> got(want.size(), 0);
    const bool ok = codec::decodeZone(golden.data(), golden.size(), got.data(), (uint32_t) got.size());
    size_t firstDiff = 0;
    while (firstDiff < want.size() && got[firstDiff] == want[firstDiff])
        ++firstDiff;
    check(ok && firstDiff == want.size(), "the builder's coded golden zone decodes bit-exact (first difference at "
                                              + std::to_string(firstDiff) + " of " + std::to_string(want.size()) + ")");
    check(! codec::decodeZone(golden.data(), golden.size() - 1, got.data(), (uint32_t) got.size()),
          "a truncated zone stream is rejected");
    std::vector<uint8_t> bad = golden;
    bad[0] |= 0xFC;   // order 63
    check(! codec::decodeZone(bad.data(), bad.size(), got.data(), (uint32_t) got.size()), "an LPC order above 32 is rejected");
    std::vector<uint8_t> zeros(4096, 0);
    check(! codec::decodeZone(zeros.data(), zeros.size(), got.data(), (uint32_t) got.size()),
          "an endless unary run is rejected");

    // A v2 pack loads to the same samples and zones as the same zones raw (v1).
    std::vector<Z> zs;
    for (int m = 40; m <= 52; m += 4)
        zs.push_back({ Art::Sustain, 0, 0, 0, hzOf(m), tone(hzOf(m), 0.4, 0.3, 0.6, (uint32_t) m), m % 3, m - 40 });
    zs.push_back({ Art::Release, 0, 0, 0, 100.0, noiseBurst(0.2, 0.03, 5) });
    const std::vector<uint8_t> raw = writePack(0, zs, "parity", 1), coded = writePack(0, zs, "parity", 2);
    SamplePack p1, p2;
    std::string err;
    check(parsePack(raw.data(), raw.size(), p1, err) && parsePack(coded.data(), coded.size(), p2, err),
          "the parity packs parse: " + err);
    bool same = p1.data == p2.data && p1.zones.size() == p2.zones.size() && p1.credits == p2.credits;
    for (size_t i = 0; same && i < p1.zones.size(); ++i)
        same = p1.zones[i].start == p2.zones[i].start && p1.zones[i].length == p2.zones[i].length
               && p1.zones[i].gain == p2.zones[i].gain && p1.zones[i].string == p2.zones[i].string
               && p1.zones[i].fret == p2.zones[i].fret;
    check(same, "a version-2 pack loads bit-identical to its version-1 twin");
    check(coded.size() < raw.size(), "the coded pack is smaller (" + std::to_string(coded.size()) + " vs "
                                         + std::to_string(raw.size()) + " bytes)");
    uint32_t version = 0;
    std::memcpy(&version, g.bytes.data() + 4, 4);
    check(version == kPackVersionCoded, "the fixture packs every other test plays are version 2");

    // Versions and codecs the reader does not know are REJECTED, never guessed.
    auto rejects = [&](std::vector<uint8_t> b, size_t at, uint32_t v, const char* what) {
        std::memcpy(b.data() + at, &v, 4);
        SamplePack p;
        std::string e;
        check(! parsePack(b.data(), b.size(), p, e) && p.zones.empty(), std::string(what) + " (" + e + ")");
    };
    rejects(coded, 4, 3, "an unknown pack version is rejected");
    rejects(raw, 4, 3, "an unknown version over raw samples is rejected, not read as version 1");
    rejects(raw, 4, 0, "pack version 0 is rejected");
    rejects(coded, 20, 2, "an unknown sample codec is rejected");
    rejects(coded, 64 + 24, 0x7FFFFFFF, "a zone whose coded bytes run past the data is rejected");
    uint32_t zone0Bytes = 0;
    std::memcpy(&zone0Bytes, coded.data() + 64 + 24, 4);
    rejects(coded, 64 + 24, zone0Bytes + 1, "a zone stream that does not end in its last byte is rejected");
    // A v2 pack whose zone bytes are damaged fails to load instead of playing noise.
    std::vector<uint8_t> damaged = coded;
    uint64_t dataOff = 0;
    std::memcpy(&dataOff, damaged.data() + 32, 8);
    std::fill(damaged.begin() + (std::ptrdiff_t) dataOff, damaged.begin() + (std::ptrdiff_t) dataOff + 64, (uint8_t) 0);
    SamplePack pd;
    check(! parsePack(damaged.data(), damaged.size(), pd, err) && pd.zones.empty(), "damaged coded samples are rejected (" + err + ")");
}

// ---- tests ---------------------------------------------------------------------

void testParser(const Fixture& g)
{
    check(g.pack.zones.size() > 100 && g.pack.has(Art::Sustain, 0) && g.pack.has(Art::Dead, 0), "fixture has zones");
    check(g.pack.credits == "fixture", "credits text survives");
    SamplePack p;
    std::string err;
    std::vector<uint8_t> bad = g.bytes;
    bad[0] = 'X';
    check(! parsePack(bad.data(), bad.size(), p, err), "bad magic is rejected");
    bad = g.bytes;
    bad.resize(bad.size() / 2);
    check(! parsePack(bad.data(), bad.size(), p, err), "a truncated pack is rejected");
    bad = g.bytes;
    const uint32_t huge = 0x7FFFFFFF;
    std::memcpy(bad.data() + 64 + 12, &huge, 4);   // zone 0 length past the data
    check(! parsePack(bad.data(), bad.size(), p, err), "a zone outside the data is rejected (" + err + ")");
    bad = g.bytes;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(bad.data() + 64 + 4, &nan, 4);
    check(! parsePack(bad.data(), bad.size(), p, err), "a NaN root is rejected");
    check(p.zones.empty(), "a rejected pack leaves the output untouched");
    const PackZone* z = selectZone(g.pack, Art::Sustain, 0, hzOf(57), 1, 0);
    check(z != nullptr && std::fabs(cents(z->rootHz, hzOf(57))) < 1.0 && z->layer == 1, "selectZone picks pitch and layer");
    const PackZone* z2 = selectZone(g.pack, Art::Sustain, 0, hzOf(57), 1, 1);
    check(z2 != nullptr && z2 != z && z2->rr != z->rr, "selectZone round-robins");
    check(selectZone(g.pack, Art::Slap, 0, 100.0, 0, 0) == nullptr, "an absent articulation selects nothing");
}

void testPitch(const Fixture& g, const Fixture& b)
{
    for (int which = 0; which < 2; ++which)
    {
        const bool bass = which == 1;
        double worst = 0.0;
        ChartTimelineVoice v(bass ? InstrumentSpec::bassGuitar(4) : InstrumentSpec::guitar(), bass ? &b.pack : &g.pack);
        for (int s = 0; s < v.spec().numStrings; ++s)
            for (int fret : { 0, 2, 5, 12, 19 })
            {
                Song song;
                song.add(0.01, 1.5, s, fret);
                const auto x = render(song, v, 1.0);
                const double f = v.spec().openHz[(size_t) s] * std::pow(2.0, fret / 12.0);
                const double c = cents(peakFreq(x, 0.15, 0.9, f, 48000.0), f);
                worst = std::max(worst, std::fabs(c));
                check(std::fabs(c) < 2.0, std::string(bass ? "bass" : "guitar") + " s" + std::to_string(s) + " f" + std::to_string(fret)
                                              + " plays in tune (" + std::to_string(c) + " c)");
            }
        std::printf("  %s (sampled): worst pitch error %.3f cents\n", bass ? "bass" : "guitar", worst);
    }
}

void testGestures(const Fixture& g)
{
    ChartTimelineVoice v(InstrumentSpec::guitar(), &g.pack);
    const double base = v.spec().openHz[3] * std::pow(2.0, 7 / 12.0);
    {
        Song s;
        ChartNote& n = s.add(0.0, 2.0, 3, 7);
        n.bendFirst = 0; n.bendCount = 3;
        s.bends = { { 0.3, 2.0f }, { 0.9, 2.0f }, { 1.3, 0.0f } };
        const auto x = render(s, v, 1.9);
        const double held = cents(peakFreq(x, 0.35, 0.85, base * std::pow(2.0, 2.0 / 12.0), 48000.0, 0.05), base);
        const double back = cents(peakFreq(x, 1.35, 1.85, base, 48000.0, 0.05), base);
        check(std::fabs(held - 200.0) < 4.0, "a sampled bend lands on +200 cents (" + std::to_string(held) + ")");
        check(std::fabs(back) < 4.0, "...and releases (" + std::to_string(back) + ")");
    }
    {
        Song s;
        s.add(0.0, 2.0, 2, 5).slideTo = 12;
        const auto x = render(s, v, 2.1);
        const double f12 = v.spec().openHz[2] * std::pow(2.0, 12 / 12.0);
        check(std::fabs(cents(peakFreq(x, 1.91, 2.01, f12, 48000.0, 0.02), f12)) < 8.0, "a sampled slide ends on fret 12");
        int onFret = 0, windows = 0;
        for (double t0 = 0.75; t0 < 1.85; t0 += 0.02, ++windows)
        {
            const double est = cents(peakFreq(x, t0, t0 + 0.025, v.spec().openHz[2] * std::pow(2.0, 8.5 / 12.0), 48000.0, 0.25),
                                     v.spec().openHz[2]);
            if (std::fabs(est - 100.0 * std::round(est / 100.0)) < 15.0) ++onFret;
        }
        check(onFret * 100 / windows >= 60, "the sampled slide moves fret by fret (" + std::to_string(onFret * 100 / windows) + " %)");
    }
}

/// Every note-off is honoured: in a sweep each note is damped when the next
/// starts; only a let-ring (hand shape) note rings on.
void testNoteOffsHonoured(const Fixture& g)
{
    ChartTimelineVoice v(InstrumentSpec::guitar(), &g.pack);
    const double fa = v.spec().openHz[1] * std::pow(2.0, 2 / 12.0);
    for (int letRing = 0; letRing < 2; ++letRing)
    {
        Song s;
        s.add(0.0, 0.0, 1, 2).letRing = letRing == 1;   // A on string 1 ...
        s.add(0.25, 0.0, 2, 2).letRing = letRing == 1;  // ... then the next string
        s.add(0.5, 0.0, 3, 2).letRing = letRing == 1;
        const auto x = render(s, v, 1.2);
        const double before = magAt(x, 0.06, 0.21, fa, 48000.0);
        // Between the attacks: 50-200 ms after the note-off at 0.28 s.
        const double after = magAt(x, 0.33, 0.48, fa, 48000.0);
        const double drop = db(after / before);
        if (letRing)
            check(drop > -12.0, "inside a hand shape the first note rings on (" + std::to_string(drop) + " dB)");
        else
            check(drop < -35.0, "in a sweep the first note is damped by the next one (" + std::to_string(drop) + " dB)");
    }
    {
        // A same-string note damps the old one within ~12 ms, no click.
        Song s;
        s.add(0.0, 2.0, 1, 0);   // a long sustain: only the next note on the string can stop it
        s.add(0.3, 0.6, 1, 7);
        const auto x = render(s, v, 1.0);
        const double f0 = v.spec().openHz[1];
        check(db(magAt(x, 0.35, 0.7, f0, 48000.0) / magAt(x, 0.05, 0.28, f0, 48000.0)) < -35.0,
              "a new note on the same string damps the old one");
        // ...and nothing of it lingers at ANY pitch: the string after the change
        // is exactly the new note alone.
        Song alone;
        alone.add(0.3, 0.6, 1, 7);
        const auto y = render(alone, v, 1.0);
        // Each relative to its own attack: every pick is a little louder or
        // softer than the last (#52), the SHAPE after it must be the same.
        const double extra = db((rms(x, 0.36, 0.7, 48000.0) / rms(x, 0.315, 0.36, 48000.0))
                                / (rms(y, 0.36, 0.7, 48000.0) / rms(y, 0.315, 0.36, 48000.0)));
        check(std::fabs(extra) < 0.5, "the replaced note leaves nothing behind (" + std::to_string(extra) + " dB vs the new note alone)");
    }
    {
        // A released note is gone within tens of ms.
        Song s;
        s.add(0.0, 0.5, 2, 3);
        const auto x = render(s, v, 1.0);
        check(db(rms(x, 0.62, 0.7, 48000.0) / rms(x, 0.3, 0.45, 48000.0)) < -40.0, "a note-off damps the note within ~100 ms");
    }
}

void testArticulations(const Fixture& g, const Fixture& b)
{
    ChartTimelineVoice v(InstrumentSpec::guitar(), &g.pack);
    Song open, palm, dead, harm, pinch;
    open.add(0.0, 1.4, 0, 3);
    palm.add(0.0, 1.4, 0, 3).mute = 2;
    dead.add(0.0, 0.0, 0, 3).mute = 1;
    harm.add(0.0, 1.4, 1, 12).harmonic = 1;
    pinch.add(0.0, 1.4, 1, 5).harmonic = 2;
    const auto xo = render(open, v, 1.5), xp = render(palm, v, 1.5), xd = render(dead, v, 1.5);
    const auto xh = render(harm, v, 1.5), xq = render(pinch, v, 1.5);
    const double dropOpen = db(rms(xo, 0.45, 0.55, 48000.0) / rms(xo, 0.02, 0.1, 48000.0));
    const double dropPalm = db(rms(xp, 0.45, 0.55, 48000.0) / rms(xp, 0.02, 0.1, 48000.0));
    check(dropPalm < dropOpen - 15.0, "a sampled palm mute decays much faster (" + std::to_string(dropPalm) + " vs " + std::to_string(dropOpen) + ")");
    const double f3 = v.spec().openHz[0] * std::pow(2.0, 3 / 12.0);
    check(magAt(xd, 0.0, 0.1, f3, 48000.0) < 0.2 * magAt(xo, 0.0, 0.1, f3, 48000.0), "a dead note has no pitched note in it");
    check(rms(xd, 0.0, 0.05, 48000.0) > 1e-3 && rms(xd, 0.3, 0.5, 48000.0) < 1e-5, "a dead note is a short sound");
    const double a = v.spec().openHz[1];
    check(db(magAt(xh, 0.2, 1.0, a, 48000.0) / magAt(xh, 0.2, 1.0, 2 * a, 48000.0)) < -30.0, "a natural harmonic has no open-string fundamental");
    check(std::fabs(cents(peakFreq(xh, 0.2, 1.0, 2 * a, 48000.0), 2 * a)) < 2.0, "...and sounds at 2 x open");
    const double f5 = a * std::pow(2.0, 5 / 12.0);
    const int kp = std::clamp((int) std::lround(1800.0 / f5), 3, 8);
    const double rPinch = db(magAt(xq, 0.2, 1.0, kp * f5, 48000.0) / magAt(xq, 0.2, 1.0, f5, 48000.0));
    check(rPinch > 6.0, "a pinch harmonic's partial " + std::to_string(kp) + " rings over the note (" + std::to_string(rPinch) + " dB)");

    ChartTimelineVoice bv(InstrumentSpec::bassGuitar(4), &b.pack);
    Song ghost, slap;
    ghost.add(0.0, 0.0, 1, 3).mute = 1;
    slap.add(0.0, 1.0, 0, 5).bassPicking = 2;
    const auto xg = render(ghost, bv, 1.0), xs = render(slap, bv, 1.0);
    check(rms(xg, 0.0, 0.05, 48000.0) > 1e-3 && db(rms(xg, 0.5, 0.7, 48000.0) / rms(xg, 0.0, 0.05, 48000.0)) < -50.0, "a bass ghost is short");
    const double fs5 = bv.spec().openHz[0] * std::pow(2.0, 5 / 12.0);
    check(std::fabs(cents(peakFreq(xs, 0.2, 0.9, fs5, 48000.0), fs5)) < 3.0, "a slap is in tune");
}

/// Natural harmonics count their node from the CAPO, not the nut (#51): capo c +
/// a harmonic charted at fret c + h sounds k * (open + c), k from h. Standard,
/// Drop D and Eb tunings, capo 0 / 2 / 5, h = 12 / 7 / 5 / 4 (k = 2 / 3 / 4 / 5).
/// Measured on the rendered signal through the core's own event API (NoteStart
/// carries `pitch` from the nut and `capo`, exactly what the engine hands over).
template <typename MakeCore>
void checkHarmonicsWithCapo(const char* name, MakeCore makeCore, double tolCents)
{
    const double fs = 48000.0;
    const std::pair<const char*, std::array<int, 6>> tunings[] = {
        { "standard", { 0, 0, 0, 0, 0, 0 } }, { "drop D", { -2, 0, 0, 0, 0, 0 } }, { "Eb", { -1, -1, -1, -1, -1, -1 } } };
    const std::pair<int, int> nodes[] = { { 12, 2 }, { 7, 3 }, { 5, 4 }, { 4, 5 } };
    double worst = 0.0;
    for (const auto& [tname, offs] : tunings)
        for (int capo : { 0, 2, 5 })
            for (const auto& [h, k] : nodes)
                for (int s : { 0, 3 })
                {
                    InstrumentSpec spec = InstrumentSpec::guitar();
                    spec.applyTuningOffsets(offs.data(), 6);
                    auto core = makeCore(spec);
                    core->prepare(fs, 128);
                    const double open = 69.0 + 12.0 * std::log2(spec.openHz[(size_t) s] / 440.0);
                    NoteStart n;
                    n.id = 1;
                    n.string = s;
                    n.fret = capo + h;
                    n.capo = capo;
                    n.pitch = open + n.fret;   // from the nut, as the engine's builder computes it
                    n.flags = kFlagHarmonic;
                    n.velocity = 0.7f;
                    core->noteOn(n);
                    std::vector<float> out((size_t) (0.9 * fs), 0.0f);
                    for (size_t i = 0; i < out.size(); i += 128)
                        core->render(out.data() + i, (int) std::min<size_t>(128, out.size() - i));
                    const double expect = 440.0 * std::pow(2.0, (open + capo - 69.0) / 12.0) * k;
                    const double c = cents(peakFreq(out, 0.2, 0.85, expect, fs, 0.04), expect);
                    worst = std::max(worst, std::fabs(c));
                    check(std::fabs(c) < tolCents, std::string(name) + " " + tname + " capo " + std::to_string(capo) + " harmonic at fret "
                                                       + std::to_string(capo + h) + " (k=" + std::to_string(k) + ") string "
                                                       + std::to_string(s) + ": " + std::to_string(c) + " cents");
                }
    std::printf("  %s: harmonics above a capo, worst %.2f cents\n", name, worst);
}

/// The ENGINE's path end to end: a pack on disk found through
/// DOTCHARTER_VOICE_DIR by dcvoice_pack_store.h, the real GuitarChartVoice
/// adapter (what the chart source constructs), and a natural harmonic with a
/// capo arriving as a dcsynth::ChartNoteOn — capo 2, charted at fret 14 (from
/// the nut): the octave of the capo'd string, not the 14th fret's node (#51).
void testEnginePathWithCapo(const Fixture& g)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dcvoice_sample_voice_test";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "guitar.dcvp", std::ios::binary);
        f.write((const char*) g.bytes.data(), (std::streamsize) g.bytes.size());
    }
#ifdef _WIN32
    _wputenv_s(L"DOTCHARTER_VOICE_DIR", dir.wstring().c_str());
#else
    setenv("DOTCHARTER_VOICE_DIR", dir.string().c_str(), 1);
#endif
    dcsynth::GuitarChartVoice v(false);
    v.prepare(48000.0, 256);
    check(v.sampled() && std::string(v.name()) == "sampled-guitar", "the adapter finds and loads the pack from the voice directory");

    const double open = 69.0 + 12.0 * std::log2(InstrumentSpec::guitar().openHz[1] / 440.0);   // A2
    dcsynth::ChartNoteOn n;
    n.noteId = 7;
    n.stringIndex = 1;
    n.fret = 14;
    n.capo = 2;
    n.pitch = (float) (open + 14.0);   // from the nut, as the engine's table holds it
    n.velocity = 0.7f;
    n.flags = DCA_CHART_NOTE_HARMONIC;
    n.slideToPitch = n.pitch;
    v.noteOn(n);
    std::vector<float> out(48000, 0.0f);
    for (size_t i = 0; i < out.size(); i += 256)
        v.render(out.data() + i, (int) std::min<size_t>(256, out.size() - i));
    const double expect = 440.0 * std::pow(2.0, (open + 2.0 - 69.0) / 12.0) * 2.0;
    const double c = cents(peakFreq(out, 0.2, 0.9, expect, 48000.0, 0.04), expect);
    check(std::fabs(c) < 3.0, "through the adapter, capo 2 + harmonic at fret 14 is the capo'd octave (" + std::to_string(c) + " cents)");
    // ...and it is the loudest thing there: a scan of 60 Hz - 2 kHz in quarter
    // tones must peak at it (a wrong node rings somewhere else entirely).
    double loudest = 0.0, loudestHz = 0.0;
    for (double f = 60.0; f < 2000.0; f *= std::pow(2.0, 1.0 / 24.0))
    {
        const double m = magAt(out, 0.2, 0.9, f, 48000.0);
        if (m > loudest) { loudest = m; loudestHz = f; }
    }
    check(std::fabs(cents(loudestHz, expect)) < 60.0, "the capo'd octave is the loudest partial (peak at " + std::to_string(loudestHz) + " Hz)");
    v.noteOff(7, dcsynth::NoteOffKind::Choke);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- the realism pass (#52) ----------------------------------------------------

/// The articulation pack: palm mutes, natural and pinch harmonics play its
/// recordings (the marker is there, the old filtered stand-ins are not), every
/// one in tune — a harmonic number a string did not record is borrowed from
/// another string and retuned — and without the pack the voice still plays.
void testArticulationPack(const Fixture& g, const Fixture& a)
{
    check(a.pack.has(Art::NatHarm, 0) && a.pack.has(Art::Pinch, 0) && a.pack.has(Art::Palm, 0) && a.pack.multiString,
          "the articulation fixture has palm / natural / pinch zones on known strings");
    {
        std::vector<uint8_t> bad = a.bytes;
        bad[64] = (uint8_t) kArtCount;   // zone 0's articulation past the last known one
        SamplePack p;
        std::string err;
        check(! parsePack(bad.data(), bad.size(), p, err), "an articulation the reader does not know is rejected");
    }
    ChartTimelineVoice with(InstrumentSpec::guitar(), &g.pack, &a.pack), without(InstrumentSpec::guitar(), &g.pack);
    const double fs = 48000.0;
    const InstrumentSpec spec = InstrumentSpec::guitar();
    // A retuned recording carries its marker retuned too: the marker is read
    // where ANY zone of that articulation would put it for the note's pitch.
    double wantHz = 0.0;
    Art wantArt = Art::Palm;
    auto marker = [&](const std::vector<float>& x) {
        double m = 0.0;
        for (const PackZone& z : a.pack.zones)
            if (z.art == (uint8_t) wantArt)
                m = std::max(m, magAt(x, 0.05, 0.45, kMarkerHz * wantHz / z.rootHz, fs));
        return m;
    };
    {
        Song s;
        s.add(0.0, 1.0, 0, 3).mute = 2;
        wantArt = Art::Palm;
        wantHz = spec.openHz[0] * std::pow(2.0, 3 / 12.0);
        const auto xw = render(s, with, 1.2), xo = render(s, without, 1.2);
        check(marker(xw) > 20.0 * marker(xo), "a palm mute plays the articulation pack's recording (marker "
                                                  + std::to_string(marker(xw)) + " vs " + std::to_string(marker(xo)) + ")");
        const double f = spec.openHz[0] * std::pow(2.0, 3 / 12.0);
        check(std::fabs(cents(peakFreq(xw, 0.05, 0.45, f, fs), f)) < 3.0, "...in tune");
    }
    for (const auto& [str, fret, k] : { std::tuple<int, int, int> { 1, 12, 2 }, { 4, 7, 3 }, { 0, 7, 3 } })
    {
        Song s;
        s.add(0.0, 1.4, str, fret).harmonic = 1;
        wantArt = Art::NatHarm;
        wantHz = spec.openHz[(size_t) str] * k;
        const auto x = render(s, with, 1.5);
        const double f = wantHz;
        const double c = cents(peakFreq(x, 0.2, 1.0, f, fs), f);
        check(marker(x) > 20.0 * marker(render(s, without, 1.5)),
              "a natural harmonic s" + std::to_string(str) + " fret " + std::to_string(fret) + " plays a recorded harmonic");
        check(std::fabs(c) < 2.0, "...and sounds k = " + std::to_string(k) + " times the open string (" + std::to_string(c) + " cents)");
    }
    {
        Song s;
        s.add(0.0, 1.4, 2, 5).harmonic = 2;   // no fret-5 recording: the nearest, retuned
        wantArt = Art::Pinch;
        wantHz = spec.openHz[2] * std::pow(2.0, 5 / 12.0);
        const auto xw = render(s, with, 1.5), xo = render(s, without, 1.5);
        check(marker(xw) > 20.0 * marker(xo), "a pinch harmonic plays the articulation pack's recording (marker "
                                                  + std::to_string(marker(xw)) + " vs " + std::to_string(marker(xo)) + ")");
        const double f = spec.openHz[2] * std::pow(2.0, 5 / 12.0);
        check(std::fabs(cents(peakFreq(xw, 0.2, 1.0, f, fs), f)) < 3.0, "...on the fretted note's pitch");
    }
#ifdef _MSC_VER
    {
        Song s;
        for (int i = 0; i < 60; ++i)
        {
            ChartNote& n = s.add(0.05 + 0.1 * i, 0.3, i % 6, 3 + (i % 9));
            if (i % 3 == 0) n.mute = 2;
            else if (i % 3 == 1) n.harmonic = 1;
            else n.harmonic = 2;
        }
        PlanOptions po;
        PlannedTimeline plan;
        planChart(s.notes.data(), (int) s.notes.size(), nullptr, 0, nullptr, 0, po, plan);
        const auto tl = plan.view(1);
        ChartTimelineVoice v(InstrumentSpec::guitar(), &g.pack, &a.pack);
        v.prepare(48000.0, 128);
        std::vector<float> buf(128);
        VoiceClock c;
        c.rolling = true;
        c.songSecondsPerSample = 1.0 / 48000.0;
        allocations = 0;
        armed = true;
        for (int bl = 0; bl < 2700; ++bl)
        {
            c.songTime = bl * 128.0 / 48000.0;
            v.render(buf.data(), 128, c, tl);
        }
        armed = false;
        check(allocations == 0, "the articulation pack's render path allocated " + std::to_string(allocations) + " times");
    }
#endif
}

/// A 2nd-order high-pass at `hz`, then its level against the input's, dB.
double brightnessDb(const std::vector<float>& x, double t0, double t1, double hz, double fs)
{
    const double w = kTwoPi * hz / fs, cw = std::cos(w), al = std::sin(w) / (2.0 * 0.7071);
    const double a0 = 1 + al, b0 = (1 + cw) / 2 / a0, b1 = -(1 + cw) / a0, b2 = b0, a1 = -2 * cw / a0, a2 = (1 - al) / a0;
    double z1 = 0, z2 = 0, e = 0, t = 0;
    const size_t a = (size_t) (t0 * fs), b = std::min(x.size(), (size_t) (t1 * fs));
    for (size_t i = 0; i < b; ++i)
    {
        const double y = b0 * x[i] + z1;
        z1 = b1 * x[i] - a1 * y + z2;
        z2 = b2 * x[i] - a2 * y;
        if (i >= a) { e += y * y; t += (double) x[i] * x[i]; }
    }
    return 10.0 * std::log10((e + 1e-30) / (t + 1e-30));
}

/// Slap and pop (#42, #52): the hard impact's brightness DECAYS — a static
/// shelf (what made them a "retro synth") keeps the same excess for the whole
/// note — a pop's string snaps onto the frets, both stay in tune, and a muted
/// pluck (dead note) peaks at least as loud as a note, as real ones do.
void testSlapPopAndDeadLevels(const Fixture& g, const Fixture& b)
{
    const double fs = 48000.0;
    ChartTimelineVoice bv(InstrumentSpec::bassGuitar(4), &b.pack);
    // The fixture's tones are dark (partial n at 0.55^n / n), so brightness is
    // read on ONE upper partial, the 9th, against the same note plucked plainly:
    // a slap on A1 (495 Hz, above its 400 Hz corner), a pop on G3 (1764 Hz,
    // above 1.5 kHz).
    Song plain, slap, pop;
    plain.add(0.0, 1.0, 0, 5);
    slap.add(0.0, 1.0, 0, 5).bassPicking = 2;
    pop.add(0.0, 1.0, 3, 12).bassPicking = 1;
    Song plainHigh;
    plainHigh.add(0.0, 1.0, 3, 12);
    const auto xp = render(plain, bv, 1.0), xs = render(slap, bv, 1.0), xq = render(pop, bv, 1.0), xph = render(plainHigh, bv, 1.0);
    const double a9 = bv.spec().openHz[0] * std::pow(2.0, 5 / 12.0) * 9.0, g9 = bv.spec().openHz[3] * 2.0 * 9.0;
    auto excess = [&](const std::vector<float>& x, const std::vector<float>& ref, double t0, double t1, double f) {
        return db(magAt(x, t0, t1, f, fs) / magAt(ref, t0, t1, f, fs)) - db(rms(x, t0, t1, fs) / rms(ref, t0, t1, fs));
    };
    const double sEarly = excess(xs, xp, 0.03, 0.12, a9), sLate = excess(xs, xp, 0.45, 0.65, a9);
    check(sEarly - sLate > 3.0, "a slap's extra brightness decays (" + std::to_string(sEarly) + " dB early, "
                                   + std::to_string(sLate) + " dB late)");
    check(sLate > 0.0 && sLate < 8.0, "...and settles a little brighter than a plucked note (" + std::to_string(sLate) + " dB)");
    const double pEarly = excess(xq, xph, 0.03, 0.12, g9), pLate = excess(xq, xph, 0.45, 0.65, g9);
    check(pEarly - pLate > 3.0, "a pop's extra brightness decays (" + std::to_string(pEarly) + " / " + std::to_string(pLate) + " dB)");
    const double clank = brightnessDb(xq, 0.0, 0.02, 4000.0, fs) - brightnessDb(xph, 0.0, 0.02, 4000.0, fs);
    check(clank > 6.0, "a pop's string snaps onto the frets: its first 20 ms are far brighter above 4 kHz (" + std::to_string(clank) + " dB)");
    const double f5 = bv.spec().openHz[0] * std::pow(2.0, 5 / 12.0), p5 = bv.spec().openHz[3] * 2.0;
    check(std::fabs(cents(peakFreq(xs, 0.2, 0.9, f5, fs), f5)) < 3.0 && std::fabs(cents(peakFreq(xq, 0.2, 0.9, p5, fs), p5)) < 3.0,
          "slap and pop are in tune");

    auto peakDb = [](const std::vector<float>& x) {
        float p = 1e-9f;
        for (float v : x) p = std::max(p, std::fabs(v));
        return 20.0 * std::log10(p);
    };
    ChartTimelineVoice gv(InstrumentSpec::guitar(), &g.pack);
    Song open, dead, bdead;
    open.add(0.0, 1.0, 1, 3);
    dead.add(0.0, 0.0, 1, 3).mute = 1;
    bdead.add(0.0, 0.0, 0, 5).mute = 1;
    const double gDead = peakDb(render(dead, gv, 0.6)) - peakDb(render(open, gv, 0.6));
    check(gDead > -4.0, "a dead note is not a faint tick under the notes (" + std::to_string(gDead) + " dB against a note)");
    const double bDead = peakDb(render(bdead, bv, 0.6)) - peakDb(xp);
    check(bDead > -4.0, "a bass dead note peaks near a note's level (" + std::to_string(bDead) + " dB)");
}

/// A bass note started through the core's own event API (the velocity is the
/// one the engine hands over), rendered for `seconds`.
std::vector<float> renderCore(SampleInstrument& core, int string, int fret, uint32_t flags, float velocity, double seconds)
{
    const double fs = 48000.0;
    core.prepare(fs, 128);
    NoteStart n;
    n.id = 1;
    n.string = string;
    n.fret = fret;
    n.pitch = 69.0 + 12.0 * std::log2(core.spec().openHz[(size_t) string] / 440.0) + fret;
    n.flags = flags;
    n.velocity = velocity;
    core.noteOn(n);
    std::vector<float> out((size_t) (seconds * fs), 0.0f);
    for (size_t i = 0; i < out.size(); i += 128)
        core.render(out.data() + i, (int) std::min<size_t>(128, out.size() - i));
    return out;
}

/// RECORDED SLAPS (#109): with a pack that has them, a slap plays the recorded
/// slap of the note (its marker), retuned to the charted pitch, its layer by
/// velocity; a pop does NOT (no pop is recorded: it stays the model); the
/// recorded slaps convert with THEIR OWN plan (another bass), never the finger
/// notes'; and the engine's adapter gives them the slap source bass's.
void testRecordedSlaps(const Fixture& b, const Fixture& bs)
{
    const double fs = 48000.0;
    check(bs.pack.has(Art::SlapNote, 0) && bs.pack.layerCount[(int) Art::SlapNote][0] == 2 && ! b.pack.has(Art::SlapNote, 0),
          "the slap fixture has two layers of recorded slaps, the plain one none");
    const InstrumentSpec spec = InstrumentSpec::bassGuitar(4);
    // The marker of `layer` where any recorded slap would put it for `hz`.
    auto marker = [&](const std::vector<float>& x, int layer, double hz) {
        double m = 0.0;
        for (const PackZone& z : bs.pack.zones)
            if (z.art == (uint8_t) Art::SlapNote)
                m = std::max(m, magAt(x, 0.05, 0.45, kSlapMarkerHz[layer] * hz / z.rootHz, fs));
        return m;
    };
    // E string fret 2 (F#1): no recording at that pitch, the nearest is retuned.
    const double f2 = spec.openHz[0] * std::pow(2.0, 2 / 12.0);
    Song slap;
    slap.add(0.0, 1.0, 0, 2).bassPicking = 2;
    ChartTimelineVoice with(spec, &bs.pack), without(spec, &b.pack);
    const auto xw = render(slap, with, 1.0), xo = render(slap, without, 1.0);
    check(marker(xw, 1, f2) > 20.0 * marker(xo, 1, f2), "a slap plays the RECORDED slap (marker " + std::to_string(marker(xw, 1, f2))
                                                             + " vs " + std::to_string(marker(xo, 1, f2)) + " without recorded slaps)");
    check(std::fabs(cents(peakFreq(xw, 0.2, 0.9, f2, fs), f2)) < 3.0, "...retuned to the charted pitch");
    check(rms(xo, 0.05, 0.3, fs) > 1e-3, "without recorded slaps a slap still sounds (the model)");

    // Velocity picks the layer: soft -> layer 0, hard -> layer 1.
    {
        SampleInstrument soft(spec, &bs.pack), hard(spec, &bs.pack);
        const auto xs = renderCore(soft, 0, 2, kFlagSlap, 0.3f, 0.6), xh = renderCore(hard, 0, 2, kFlagSlap, 0.95f, 0.6);
        check(marker(xs, 0, f2) > 5.0 * marker(xs, 1, f2), "a soft slap plays the soft layer");
        check(marker(xh, 1, f2) > 5.0 * marker(xh, 0, f2), "a hard slap plays the hard layer");
    }

    // A pop is not recorded: it plays the model on the same pack.
    const double g2 = spec.openHz[3] * std::pow(2.0, 2 / 12.0);   // G string fret 2: a recorded slap pitch
    Song pop;
    pop.add(0.0, 1.0, 3, 2).bassPicking = 1;
    Song slapHigh;
    slapHigh.add(0.0, 1.0, 3, 2).bassPicking = 2;
    const auto xp = render(pop, with, 1.0), xsh = render(slapHigh, with, 1.0);
    check(marker(xsh, 1, g2) > 20.0 * marker(xp, 1, g2), "a pop does NOT play a recorded slap (marker " + std::to_string(marker(xp, 1, g2))
                                                              + " vs the slap's " + std::to_string(marker(xsh, 1, g2)) + ")");
    check(rms(xp, 0.05, 0.3, fs) > 1e-3 && std::fabs(cents(peakFreq(xp, 0.2, 0.9, g2, fs), g2)) < 3.0, "...it sounds, in tune");

    // The recorded slaps convert with their OWN plan.
    const ModelSpec* jazz = findModel(kModelJazzBass);
    const PositionSpec* both = jazz != nullptr ? findPosition(*jazz, kPickNeckBridge) : nullptr;
    check(both != nullptr, "the Jazz bass model has both pickups");
    if (both != nullptr)
    {
        const ConversionPlan slapPlan = makeConversionPlan(*jazz, both->full, "bass-slap", 0);
        check(! slapPlan.identity && std::fabs(slapPlan.source.scaleMm - 845.0f) < 0.5f,
              "the recorded slaps' source is the Rickenbacker 4001 (33.25\" scale), converted to a Jazz bass");
        SampleInstrument plain(spec, &bs.pack), fingersOnly(spec, &bs.pack), slapsOnly(spec, &bs.pack);
        fingersOnly.setConversion(-1, slapPlan);
        slapsOnly.setSlapConversion(slapPlan);
        const auto a = renderCore(plain, 1, 2, kFlagSlap, 0.7f, 0.5), f = renderCore(fingersOnly, 1, 2, kFlagSlap, 0.7f, 0.5),
                   c = renderCore(slapsOnly, 1, 2, kFlagSlap, 0.7f, 0.5);
        double dF = 0.0, dC = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            dF = std::max(dF, (double) std::fabs(a[i] - f[i]));
            dC = std::max(dC, (double) std::fabs(a[i] - c[i]));
        }
        check(dF < 1e-6, "the finger notes' conversion leaves a recorded slap alone (" + std::to_string(dF) + ")");
        check(dC > 1e-3, "the slaps' own conversion converts it (" + std::to_string(dC) + ")");
    }

    // The engine's adapter: a bass pack with recorded slaps gets the slaps' plan.
    namespace fsys = std::filesystem;
    const fsys::path dir = fsys::temp_directory_path() / "dcvoice_sample_voice_test_slaps";
    fsys::create_directories(dir);
    {
        std::ofstream file(dir / "bass.dcvp", std::ios::binary);
        file.write((const char*) bs.bytes.data(), (std::streamsize) bs.bytes.size());
    }
#ifdef _WIN32
    _wputenv_s(L"DOTCHARTER_VOICE_DIR", dir.wstring().c_str());
#else
    setenv("DOTCHARTER_VOICE_DIR", dir.string().c_str(), 1);
#endif
    {
        dcsynth::GuitarChartVoice v(true);
        v.prepare(48000.0, 256);
        const SampleInstrument* si = v.sampler();
        check(si != nullptr && ! si->slapConversion().identity && std::fabs(si->slapConversion().source.scaleMm - 845.0f) < 0.5f
                  && ! v.recordedAsIs(),
              "the adapter converts the recorded slaps from the Rickenbacker");
    }
    std::error_code ec;
    fsys::remove_all(dir, ec);
}

/// The vibrato is a hand, not an LFO (#52): it starts late, a guitar's only
/// bends up while a bass rocks both ways, its rate averages ~4.3 Hz and its
/// cycles are NOT all the same length; deterministic per note.
void testVibratoIsAHand()
{
    std::vector<double> g, b;
    for (int i = 0; i < 48000 * 5; i += 48)
    {
        g.push_back(SampleInstrument::vibrato(0.25, i / 48000.0, 1234u, false));
        b.push_back(SampleInstrument::vibrato(0.22, i / 48000.0, 1234u, true));
    }
    double early = 0.0, gMin = 1.0, bMin = 1.0;
    for (size_t i = 0; i < g.size(); ++i)
    {
        if (i * 48 < (size_t) (0.12 * 48000)) early = std::max(early, std::fabs(g[i]) + std::fabs(b[i]));
        gMin = std::min(gMin, g[i]);
        bMin = std::min(bMin, b[i]);
    }
    check(early == 0.0, "the vibrato waits before it starts");
    check(gMin >= 0.0 && bMin < -0.05, "a guitar's vibrato bends up only, a bass's both ways");
    // Upward zero crossings of the bass curve after 1 s: the cycle lengths.
    std::vector<double> cross;
    for (size_t i = 1001; i < b.size(); ++i)
        if (b[i - 1] < 0.0 && b[i] >= 0.0)
            cross.push_back(i * 0.001);
    std::vector<double> periods;
    for (size_t i = 1; i < cross.size(); ++i) periods.push_back(cross[i] - cross[i - 1]);
    double mean = 0.0, var = 0.0;
    for (double p : periods) mean += p;
    mean /= std::max<size_t>(periods.size(), 1);
    for (double p : periods) var += (p - mean) * (p - mean);
    const double sd = std::sqrt(var / std::max<size_t>(periods.size(), 1));
    check(periods.size() > 10 && 1.0 / mean > 4.0 && 1.0 / mean < 5.2, "the vibrato averages ~4.3 Hz (" + std::to_string(1.0 / mean) + ")");
    check(sd / mean > 0.02, "...and its cycles are not all alike (sd " + std::to_string(100.0 * sd / mean) + " %)");
    check(SampleInstrument::vibrato(0.28, 2.0, 1234u, false) == SampleInstrument::vibrato(0.28, 2.0, 1234u, false)
              && SampleInstrument::vibrato(0.28, 2.0, 1234u, false) != SampleInstrument::vibrato(0.28, 2.0, 99u, false),
          "the vibrato is deterministic per note and differs between notes");
}

/// A pack with ONE take per string and fret (the Strat's) does not machine-gun:
/// a repeated note alternates between its own recording and its neighbours'
/// retuned, so consecutive attacks differ — and every one is in tune.
void testRepeatedNotesVary()
{
    const double open[6] = { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 };
    std::vector<Z> z;
    for (int s = 0; s < 6; ++s)
        for (int fret = 0; fret <= 12; ++fret)
        {
            const double f = open[s] * std::pow(2.0, fret / 12.0);
            // Neighbouring takes differ like real plucks do (brighter / darker).
            z.push_back({ Art::Sustain, 0, 0, 0, f, normalised(tone(f, 1.5, 3.0, fret % 2 ? 0.75 : 0.45, (uint32_t) (s * 50 + fret)), 0.5f), s, fret });
        }
    Fixture fx;
    fx.bytes = writePack(0, z, "one take");
    std::string err;
    check(parsePack(fx.bytes.data(), fx.bytes.size(), fx.pack, err) && fx.pack.multiString, "the one-take fixture parses as multi-string");
    ChartTimelineVoice v(InstrumentSpec::guitar(), &fx.pack);
    Song s;
    for (int i = 0; i < 3; ++i)
        s.add(0.5 * i, 0.4, 2, 5);
    const auto x = render(s, v, 1.5);
    const size_t n = (size_t) (0.2 * 48000.0);
    double diff = 0.0, ref = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        diff += std::pow((double) x[i] - x[i + 24000], 2);
        ref += (double) x[i] * x[i];
    }
    check(diff > 0.01 * ref, "two attacks of one note are different recordings (" + std::to_string(10.0 * std::log10(diff / ref)) + " dB apart)");
    const double f = open[2] * std::pow(2.0, 5 / 12.0);
    for (int i = 0; i < 3; ++i)
        check(std::fabs(cents(peakFreq(x, 0.5 * i + 0.05, 0.5 * i + 0.35, f, 48000.0), f)) < 2.0,
              "repeated attack " + std::to_string(i) + " is in tune");
}

/// A strum is a hand, not a sequencer (#52): each string after the first lands
/// within +-1.5 ms of the even spread (not exactly on it) and is hit within
/// +-8 %, deterministically.
void testStrumHumanised()
{
    bool within = true, off = false, scaled = true, varied = false;
    for (int rank = 0; rank < 6; ++rank)
    {
        const double d = dcsynth::GuitarChartVoice::strumDelayMs(rank, 8.0, 42, rank);
        within &= std::fabs(d - rank * 8.0) <= 1.5 + 1e-9 && d >= 0.0;
        off |= rank > 0 && std::fabs(d - rank * 8.0) > 0.05;
        if (rank == 0) within &= d == 0.0;
        const float v = dcsynth::GuitarChartVoice::strumVelocityScale(42, rank);
        scaled &= v >= 0.92f && v <= 1.08f;
        varied |= std::fabs(v - 1.0f) > 0.005f;
    }
    check(within && off, "strummed strings land near, not exactly on, the even spread");
    check(scaled && varied, "strummed strings are hit a little harder or softer");
    check(dcsynth::GuitarChartVoice::strumDelayMs(3, 8.0, 7, 2) == dcsynth::GuitarChartVoice::strumDelayMs(3, 8.0, 7, 2),
          "the strum is deterministic");
}

/// A pull-off has no pick in it (#52): its first milliseconds carry far less
/// of the pick's click than a picked note's, and it is in tune.
void testPullOffHasNoPick(const Fixture& g)
{
    ChartTimelineVoice v(InstrumentSpec::guitar(), &g.pack);
    const double fs = 48000.0;
    Song picked, pulled;
    picked.add(0.0, 0.3, 2, 7);
    picked.add(0.3, 0.6, 2, 5);
    pulled.add(0.0, 0.3, 2, 7);
    pulled.add(0.3, 0.6, 2, 5).hopo = 2;
    const auto xa = render(picked, v, 1.0), xb = render(pulled, v, 1.0);
    // The fixture's pick click is broadband noise; its tones stop below 4 kHz here.
    const double click = brightnessDb(xb, 0.3, 0.306, 4000.0, fs) - brightnessDb(xa, 0.3, 0.306, 4000.0, fs);
    check(click < -15.0, "a pull-off carries no pick click (" + std::to_string(click) + " dB against a picked note)");
    const double f = v.spec().openHz[2] * std::pow(2.0, 5 / 12.0);
    check(std::fabs(cents(peakFreq(xb, 0.4, 0.85, f, fs), f)) < 2.0, "...and lands in tune");
}

/// A hammer-on, pull-off or tap onto a SILENT string (a legato run moving to a
/// new string, tapping from silence) is sounded by the fretting hand: no pick,
/// and softer than a picked note (DotCharter #100). Every fixture recording
/// opens with a 5 ms noise burst standing in for the pick, so "no click" is
/// measurable where the voice enters a recording PAST it: every gesture on the
/// bass (its pack has no hammer-on recordings) and the guitar's pull-off. The
/// guitar's hammer-on and tap play the pack's own hammer-on recording instead;
/// there it is the level that tells it from a pick.
void testLegatoOntoASilentStringHasNoPick(const Fixture& g, const Fixture& b)
{
    const double fs = 48000.0;
    const char* names[] = { "hammer-on", "pull-off", "tap" };
    for (int bass = 0; bass <= 1; ++bass)
    {
        ChartTimelineVoice v(bass ? InstrumentSpec::bassGuitar(4) : InstrumentSpec::guitar(), bass ? &b.pack : &g.pack);
        const std::string who = bass ? "bass " : "guitar ";
        Song picked;
        picked.add(0.3, 0.6, 2, 5);
        const auto xp = render(picked, v, 1.0);
        for (int hopo = 1; hopo <= 3; ++hopo)
        {
            Song s;
            s.add(0.3, 0.6, 2, 5).hopo = (uint8_t) hopo;
            const auto x = render(s, v, 1.0);
            const std::string name = who + names[hopo - 1];
            const double level = 20.0 * std::log10(rms(x, 0.31, 0.5, fs) / std::max(rms(xp, 0.31, 0.5, fs), 1.0e-9));
            check(level < -1.0 && level > -12.0,
                  "a " + name + " onto a silent string is softer than a picked note, not silent (" + std::to_string(level) + " dB)");
            if (bass || hopo == 2)
            {
                const double click = brightnessDb(x, 0.3, 0.306, 4000.0, fs) - brightnessDb(xp, 0.3, 0.306, 4000.0, fs);
                check(click < -15.0, "...and carries no pick click (" + std::to_string(click) + " dB)");
            }
        }
    }
}

void testDeterminismAndRt(const Fixture& g)
{
    Song s;
    uint32_t r = 99;
    auto rnd = [&]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (double) (r >> 8) / 16777216.0; };
    for (double t = 0.05; t < 12.0; t += 0.125)
    {
        ChartNote& n = s.add(t, rnd() < 0.5 ? 0.0 : rnd() * 1.2, (int) (rnd() * 6), (int) (rnd() * 22));
        const int k = (int) (rnd() * 12);
        if (k == 0) n.hopo = 1;
        else if (k == 1) n.hopo = 2;
        else if (k == 2) n.mute = 1;
        else if (k == 3) n.mute = 2;
        else if (k == 4) n.harmonic = 1;
        else if (k == 5) n.harmonic = 2;
        else if (k == 6) { n.slideTo = (int) (rnd() * 20); n.sustain = 0.6; }
        else if (k == 7) { n.tremolo = true; n.sustain = 0.5; }
        else if (k == 8) { n.vibrato = true; n.sustain = 0.8; }
        else if (k == 9)
        {
            n.bendFirst = (int) s.bends.size(); n.bendCount = 2; n.sustain = 0.5;
            s.bends.push_back({ t + 0.1, 2.0f });
            s.bends.push_back({ t + 0.4, 0.0f });
        }
        n.letRing = rnd() < 0.1;
    }
    ChartTimelineVoice a(InstrumentSpec::guitar(), &g.pack), b(InstrumentSpec::guitar(), &g.pack);
    const auto xa = render(s, a, 13.0, 128), xb = render(s, b, 13.0, 128), xc = render(s, a, 13.0, 97);
    bool fin = true, bounded = true;
    for (float x : xa) { fin &= std::isfinite(x); bounded &= std::fabs(x) < 1.0f; }
    check(fin && bounded, "a dense sampled chart renders finite and bounded");
    check(std::memcmp(xa.data(), xb.data(), xa.size() * sizeof(float)) == 0, "two sampled voices render the same samples");
    double md = 0.0;
    for (size_t i = 0; i < xa.size(); ++i) md = std::max(md, (double) std::fabs(xa[i] - xc[i]));
    check(md < 1e-4, "the block size does not change the sampled output (" + std::to_string(md) + ")");
    check(rms(xa, 1.0, 12.0, 48000.0) > 1e-3, "the dense chart is audible");
#ifdef _MSC_VER
    PlanOptions po;
    PlannedTimeline plan;
    planChart(s.notes.data(), (int) s.notes.size(), s.bends.data(), (int) s.bends.size(), nullptr, 0, po, plan);
    const auto tl = plan.view(1);
    ChartTimelineVoice v(InstrumentSpec::guitar(), &g.pack);
    v.prepare(48000.0, 128);
    std::vector<float> buf(128);
    VoiceClock c;
    c.rolling = true;
    c.songSecondsPerSample = 1.0 / 48000.0;
    allocations = 0;
    armed = true;
    for (int bl = 0; bl < 4500; ++bl)
    {
        c.songTime = bl * 128.0 / 48000.0;
        v.render(buf.data(), 128, c, tl);
    }
    armed = false;
    check(allocations == 0, "the sampled render path allocated " + std::to_string(allocations) + " times");
#endif
}

// ---- LAZY ZONES (#417) -------------------------------------------------------

/// The Strat-like layout (every string x frets 0..15, ONE take each, string and
/// fret recorded) in this file's v2 writer: the multiString / one-take paths.
Fixture makeStratFixture()
{
    const int open[6] = { 40, 45, 50, 55, 59, 64 };
    std::vector<Z> z;
    for (int s = 0; s < 6; ++s)
        for (int f = 0; f <= 15; ++f)
        {
            const double hz = hzOf(open[s] + f);
            z.push_back({ Art::Sustain, 0, 0, 0, hz, normalised(tone(hz, 1.6, 3.0, 0.5 + 0.05 * s, (uint32_t) (s * 97 + f)), 0.5f), s, f });
        }
    for (double o : { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 })
        z.push_back({ Art::Dead, 0, 0, 0, o, noiseBurst(0.12, 0.012, (uint32_t) o + 7u) });
    for (int m = 40; m <= 80; m += 2)
        z.push_back({ Art::Release, 0, 0, 0, hzOf(m), noiseBurst(0.2, 0.03, (uint32_t) m + 1000u) });
    Fixture fx;
    fx.bytes = writePack(0, z, "strat fixture");
    std::string err;
    check(parsePack(fx.bytes.data(), fx.bytes.size(), fx.pack, err), std::string("the strat fixture parses: ") + err);
    return fx;
}

SamplePack lazyOf(const std::vector<uint8_t>& bytes)
{
    SamplePack p;
    std::string err;
    check(parsePackLazy(std::make_shared<const std::vector<uint8_t>>(bytes), p, err), "a lazy pack parses: " + err);
    return p;
}

int readyCount(const SamplePack& p)
{
    int n = 0;
    for (const PackZone& z : p.zones)
        n += p.ready(z) ? 1 : 0;
    return n;
}

/// The format side: a lazy pack holds its zone table only, decodes exactly the
/// zones asked for — bit-identical to the eager pack — counts them, and a zone
/// that does not decode (a damaged stream, a file changed on disk) stays absent
/// without failing the pack.
void testLazyPackDecodesOnDemand(const Fixture& g)
{
    for (uint32_t version : { 2u, 1u })
    {
        const std::string v = " (v" + std::to_string(version) + ")";
        std::vector<uint8_t> bytes = g.bytes;
        if (version == 1)
        {
            // The v1 twin of the same zones.
            std::vector<Z> z;
            for (const PackZone& pz : g.pack.zones)
            {
                std::vector<float> y(pz.length);
                for (uint32_t i = 0; i < pz.length; ++i)
                    y[i] = g.pack.samples(pz)[i] * pz.gain;
                z.push_back({ (Art) pz.art, pz.variant, pz.layer, pz.rr, pz.rootHz, y, pz.string, pz.fret });
            }
            bytes = writePack(0, z, "fixture", 1);
        }
        SamplePack eager;
        std::string err;
        check(parsePack(bytes.data(), bytes.size(), eager, err), "eager twin parses" + v);
        SamplePack lazy = lazyOf(bytes);
        check(lazy.lazy != nullptr && lazy.data.empty() && lazy.zones.size() == eager.zones.size(), "a lazy pack holds the zone table only" + v);
        bool sameTable = lazy.multiString == eager.multiString && lazy.credits == eager.credits;
        for (size_t i = 0; i < lazy.zones.size(); ++i)
        {
            const PackZone &x = lazy.zones[i], &y = eager.zones[i];
            sameTable &= x.art == y.art && x.variant == y.variant && x.layer == y.layer && x.rr == y.rr && x.rootHz == y.rootHz
                         && x.gain == y.gain && x.length == y.length && x.start == y.start && x.string == y.string && x.fret == y.fret;
        }
        for (int a = 0; a < kArtCount; ++a)
            sameTable &= lazy.layerCount[a][0] == eager.layerCount[a][0] && lazy.layerCount[a][1] == eager.layerCount[a][1];
        check(sameTable, "the lazy zone table is the eager one" + v);
        check(readyCount(lazy) == 0 && lazy.bytes() < eager.bytes() / 50, "nothing is decoded before it is asked for" + v + " ("
              + std::to_string(lazy.bytes()) + " of " + std::to_string(eager.bytes()) + " bytes)");

        std::vector<uint8_t> want(lazy.zones.size(), 0);
        int64_t wantBytes = 0;
        for (size_t i = 0; i < want.size(); i += 3)
        {
            want[i] = 1;
            wantBytes += (int64_t) lazy.zones[i].length * 2;
        }
        const int got = ensureZones(lazy, want);
        bool exact = true, onlyWanted = true;
        for (size_t i = 0; i < want.size(); ++i)
        {
            const PackZone& z = lazy.zones[i];
            onlyWanted &= lazy.ready(z) == (want[i] != 0);
            if (want[i] != 0 && lazy.ready(z))
                exact &= std::memcmp(lazy.samples(z), eager.samples(eager.zones[i]), (size_t) z.length * 2) == 0;
        }
        check(got == (int) ((want.size() + 2) / 3) && onlyWanted, "exactly the zones asked for are decoded" + v);
        check(exact, "a decoded zone is bit-identical to the eager pack's" + v);
        check(lazy.lazy->bytes.load() == wantBytes, "the decoded bytes are counted" + v);
        check(ensureZones(lazy, want) == 0, "a decoded zone is never decoded twice" + v);
    }

    // A damaged zone stream: the table is valid, that ONE zone never decodes.
    {
        std::vector<uint8_t> bytes = g.bytes;
        SamplePack probe = lazyOf(bytes);
        const size_t victim = 5;
        const uint64_t at = probe.lazy->dataOff + probe.lazy->srcOff[victim];
        for (uint32_t k = 0; k < probe.lazy->srcBytes[victim]; ++k)
            bytes[(size_t) (at + k)] = 0xFF;
        SamplePack lazy = lazyOf(bytes);
        std::vector<uint8_t> want(lazy.zones.size(), 1);
        ensureZones(lazy, want);
        check(! lazy.ready(lazy.zones[victim]) && readyCount(lazy) == (int) lazy.zones.size() - 1,
              "a damaged zone stays absent and the rest decode");
        check(ensureZones(lazy, want) == 0, "a damaged zone is not retried");
    }

    // A pack FILE that changes on disk after its table was read decodes nothing.
    {
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "dc_lazy_zone_test.dcvp";
        { std::ofstream f(file, std::ios::binary | std::ios::trunc); f.write((const char*) g.bytes.data(), (std::streamsize) g.bytes.size()); }
        SamplePack lazy;
        std::string err;
        check(loadPackLazy(file, lazy, err), "a lazy pack loads from a file: " + err);
        std::vector<uint8_t> want(lazy.zones.size(), 0);
        want[0] = 1;
        check(ensureZones(lazy, want) == 1 && lazy.ready(lazy.zones[0])
                  && std::memcmp(lazy.samples(lazy.zones[0]), g.pack.samples(g.pack.zones[0]), (size_t) lazy.zones[0].length * 2) == 0,
              "a lazy file pack decodes a zone bit-identical");
        { std::ofstream f(file, std::ios::binary | std::ios::trunc); f.write((const char*) g.bytes.data(), (std::streamsize) g.bytes.size() / 2); }
        want[1] = 1;
        check(ensureZones(lazy, want) == 0 && ! lazy.ready(lazy.zones[1]), "a pack file changed on disk decodes nothing more");
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
}

/// selectZone on a lazy pack: with the chosen group decoded it is the eager
/// choice (every art, variant, pitch, layer, round robin, string); with it
/// missing it ASKS for that group, returns a decoded zone or nothing, never
/// allocates; serviced, the choice is the eager one again.
void testLazySelectionMatchesEager(const Fixture& g, const Fixture& strat)
{
    for (const Fixture* fx : { &g, &strat })
    {
        const std::string which = fx == &g ? " (chromatic)" : " (one take per string and fret)";
        SamplePack lazy = lazyOf(fx->bytes);
        ensureZones(lazy, std::vector<uint8_t>(lazy.zones.size(), 1));
        bool same = true;
        for (int a = 0; a < kArtCount; ++a)
            for (int variant = 0; variant < 2; ++variant)
                for (double m = 30.0; m < 95.0; m += 0.61)
                    for (int layer : { 0, 1, 99 })
                        for (uint32_t rr = 0; rr < 3; ++rr)
                            for (int s : { -1, 0, 3, 5 })
                            {
                                const PackZone* e = selectZone(fx->pack, (Art) a, variant, hzOf(m), layer, rr, s);
                                const PackZone* l = selectZone(lazy, (Art) a, variant, hzOf(m), layer, rr, s);
                                same &= (e == nullptr) == (l == nullptr)
                                        && (e == nullptr || e - fx->pack.zones.data() == l - lazy.zones.data());
                            }
        check(same, "a fully decoded lazy pack selects exactly the eager zones" + which);

        // Only the group of fret 5 on string 2 (pitch 55) decoded.
        SamplePack part = lazyOf(fx->bytes);
        std::vector<uint8_t> want(part.zones.size(), 0);
        markGroup(part, Art::Sustain, 0, hzOf(55.0), 2, want);
        ensureZones(part, want);
        const PackZone* in = selectZone(part, Art::Sustain, 0, hzOf(55.0), 1, 1, 2);
        const PackZone* inE = selectZone(fx->pack, Art::Sustain, 0, hzOf(55.0), 1, 1, 2);
        check(in != nullptr && inE != nullptr && in - part.zones.data() == inE - fx->pack.zones.data() && ! zoneRequestsPending(part),
              "a decoded group is the eager choice and asks for nothing" + which);
#ifdef _MSC_VER
        allocations = 0;
        armed = true;
#endif
        const PackZone* miss = selectZone(part, Art::Sustain, 0, hzOf(62.0), 1, 0, 3);
#ifdef _MSC_VER
        armed = false;
        check(allocations == 0, "a miss allocated " + std::to_string(allocations) + " times" + which);
#endif
        check(miss != nullptr && part.ready(*miss) && std::fabs(miss->rootHz - hzOf(55.0)) < 1.0,
              "a miss plays the nearest DECODED recording" + which);
        check(zoneRequestsPending(part), "a miss asks a worker for its group" + which);
        check(serviceZoneRequests(part) > 0 && ! zoneRequestsPending(part), "the worker decodes the asked group" + which);
        const PackZone* now = selectZone(part, Art::Sustain, 0, hzOf(62.0), 1, 0, 3);
        const PackZone* nowE = selectZone(fx->pack, Art::Sustain, 0, hzOf(62.0), 1, 0, 3);
        check(now != nullptr && nowE != nullptr && now - part.zones.data() == nowE - fx->pack.zones.data(),
              "once decoded the miss is the eager choice" + which);
        SamplePack none = lazyOf(fx->bytes);
        check(selectZone(none, Art::Sustain, 0, hzOf(50.0), 0, 0, 1) == nullptr && zoneRequestsPending(none),
              "nothing decoded: a miss plays nothing and asks" + which);
    }
}

dca_chart_note chartNote(double t, double len, int string, int fret, const int* open, uint32_t flags = 0)
{
    dca_chart_note n {};
    n.start_seconds = t;
    n.release_seconds = t + len;
    n.sustain_seconds = len > 0.3 ? len : 0.0;
    n.pitch = (float) (open[string] + fret);
    n.velocity = 0.72f;
    n.string_index = string;
    n.fret = fret;
    n.slide_to_fret = -1.0f;
    n.flags = flags;
    n.chord_id = -1;
    n.bend_first = -1;
    return n;
}

/// A dense chart in a fret WINDOW (so only part of a pack is wanted) with every
/// articulation the voice knows: chords, slides, bends, legato, mutes,
/// harmonics, pinches, tremolo, vibrato; bass: slaps, pops, ghosts, a pick.
std::unique_ptr<dcsynth::ChartNoteTable> windowChart(bool bass, uint32_t seed, int fretLo, int fretHi, double seconds)
{
    static const int gOpen[6] = { 40, 45, 50, 55, 59, 64 }, bOpen[6] = { 28, 33, 38, 43, 43, 43 };
    const int* open = bass ? bOpen : gOpen;
    const int strings = bass ? 4 : 6;
    uint32_t r = seed;
    auto rnd = [&]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (double) (r >> 8) / 16777216.0; };
    std::vector<dca_chart_note> notes;
    std::vector<dca_chart_bend_point> bends;
    int chord = 0;
    for (double t = 0.1; t < seconds; t += 0.18)
    {
        const int fret = fretLo + (int) (rnd() * (fretHi - fretLo + 1));
        const int s = (int) (rnd() * strings);
        const double len = rnd() < 0.4 ? 0.15 : 0.3 + rnd();
        const int k = (int) (rnd() * 16);
        if (! bass && k == 0 && s <= 3)
        {
            for (int c = 0; c < 3; ++c)
            {
                dca_chart_note n = chartNote(t, len, s + c, fret, open, rnd() < 0.3 ? DCA_CHART_NOTE_PALM_MUTE : 0u);
                n.chord_id = chord;
                n.strum = DCA_CHART_STRUM_DOWN;
                notes.push_back(n);
            }
            ++chord;
            continue;
        }
        uint32_t f = 0;
        dca_chart_note n = chartNote(t, len, s, fret, open);
        switch (k)
        {
            case 1: f = DCA_CHART_NOTE_HAMMER_ON; break;
            case 2: f = DCA_CHART_NOTE_PULL_OFF; break;
            case 3: f = DCA_CHART_NOTE_TAP; break;
            case 4: f = DCA_CHART_NOTE_PALM_MUTE; break;
            case 5: f = DCA_CHART_NOTE_FRET_HAND_MUTE; break;
            case 6: f = bass ? DCA_CHART_NOTE_SLAP : DCA_CHART_NOTE_HARMONIC; if (! bass) { n.fret = (int) (rnd() < 0.5 ? 12 : 7); n.pitch = (float) (open[s] + n.fret); } break;
            case 7: f = bass ? DCA_CHART_NOTE_POP : DCA_CHART_NOTE_PINCH_HARMONIC; break;
            case 8: f = DCA_CHART_NOTE_TREMOLO; n.sustain_seconds = 0.5; n.release_seconds = t + 0.5; break;
            case 9: f = DCA_CHART_NOTE_VIBRATO; n.sustain_seconds = 0.8; n.release_seconds = t + 0.8; break;
            case 10: n.slide_to_fret = (float) std::clamp(fret + (rnd() < 0.5 ? -3 : 3), fretLo, fretHi); n.sustain_seconds = 0.6; n.release_seconds = t + 0.6; break;
            case 11:
                n.bend_first = (int32_t) bends.size(); n.bend_count = 2; n.sustain_seconds = 0.6; n.release_seconds = t + 0.6;
                bends.push_back({ t + 0.1, 2.0f, 0 });
                bends.push_back({ t + 0.4, 0.0f, 0 });
                break;
            case 12: f = DCA_CHART_NOTE_ACCENT; break;
            case 13: f = bass ? DCA_CHART_NOTE_PICKED : DCA_CHART_NOTE_LET_RING; break;
            case 14: f = DCA_CHART_NOTE_GHOST; break;
            case 15:
            {
                // A TIE from a ringing note into a muted one on the same string:
                // the string keeps sounding (no new recording), so the muted
                // note's pitch still needs the sustain and its release noise.
                n.flags |= DCA_CHART_NOTE_LINK_NEXT | (bass ? DCA_CHART_NOTE_BASS : 0u);
                n.release_seconds = t + 0.12;
                notes.push_back(n);
                const int tiedFret = fret + 4 <= fretHi ? fret + 4 : std::max(fretLo, fret - 4);   // past the first note's margin
                dca_chart_note tied = chartNote(t + 0.09, 0.6, s, tiedFret, open,
                                                (rnd() < 0.5 ? DCA_CHART_NOTE_PALM_MUTE : DCA_CHART_NOTE_FRET_HAND_MUTE) | (bass ? DCA_CHART_NOTE_BASS : 0u));
                notes.push_back(tied);
                continue;
            }
            default: break;
        }
        n.flags |= f;
        if (bass) n.flags |= DCA_CHART_NOTE_BASS;
        notes.push_back(n);
    }
    std::string err;
    auto t = dcsynth::ChartNoteTable::build(notes.data(), (int32_t) notes.size(), (int32_t) sizeof(dca_chart_note),
                                            bends.data(), (int32_t) bends.size(), err);
    check(t != nullptr, "the window chart is accepted: " + err);
    return t;
}

/// Renders `table` through `voice` on an ideal song clock (the engine's path).
std::vector<float> renderTable(const dcsynth::ChartNoteTable& table, dcsynth::ChartVoice& voice, double seconds)
{
    constexpr double sr = 48000.0;
    constexpr int n = 256;
    voice.prepare(sr, 2048);
    dcsynth::ChartScheduler sched;
    std::vector<float> out, buf(n);
    for (int64_t k = 1; k * n < (int64_t) (seconds * sr); ++k)
    {
        dcsynth::SongClockSample c;
        c.valid = true;
        c.playing = true;
        c.position = (double) (k * n) / sr;
        c.tempo = 1.0;
        std::fill(buf.begin(), buf.end(), 0.0f);
        sched.process(&table, 1, &voice, 1, c, 0.0, sr, buf.data(), n);
        out.insert(out.end(), buf.begin(), buf.end());
    }
    return out;
}

/// THE CONTRACT (#417): a lazy voice that decoded only what planZoneDemand asks
/// for renders a chart BIT-IDENTICAL to a voice on fully decoded packs, never
/// asked a worker for anything (the plan covered every note), and holds a
/// fraction of the RAM. Every voice family: the chromatic guitar with the
/// articulation pack, a one-take Strat, the bass with recorded slaps.
void testDemandCoversEveryNote(const Fixture& g, const Fixture& arts, const Fixture& strat, const Fixture& slaps)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dc_lazy_zone_packs";
    std::filesystem::create_directories(dir);
    auto put = [&](const char* name, const std::vector<uint8_t>& b) {
        std::ofstream f(dir / (std::string(name) + ".dcvp"), std::ios::binary | std::ios::trunc);
        f.write((const char*) b.data(), (std::streamsize) b.size());
    };
    put("guitar", g.bytes);
    put("guitar-arts", arts.bytes);
    put("strat-neck", strat.bytes);
    put("bass", slaps.bytes);
#ifdef _WIN32
    _putenv_s("DOTCHARTER_VOICE_DIR", dir.string().c_str());
#else
    setenv("DOTCHARTER_VOICE_DIR", dir.string().c_str(), 1);
#endif
    InstrumentConfig strat1;
    strat1.model = kModelStratSSS;
    strat1.pickup = kPickNeck;
    // `most`: the share of the packs the plan may decode. The fixtures are
    // small (a chromatic set of 47 pitches, every string of which overlaps), so
    // only the multi-string pack shows a real chart's saving here; the real
    // packs are measured by ChartVoiceRealPackRamTests.
    struct Case { const char* name; bool bass; InstrumentConfig config; int lo, hi; double most; };
    const Case cases[] = {
        { "Gretsch + articulations", false, legacyConfig(false), 2, 9, 0.95 },
        { "Strat neck (one take)", false, strat1, 3, 8, 0.85 },
        { "bass with recorded slaps", true, legacyConfig(true), 0, 7, 0.9 },
    };
    for (const Case& c : cases)
    {
        const std::string name = std::string(" (") + c.name + ")";
        for (uint32_t seed : { 7u, 1234u })
        {
            const auto table = windowChart(c.bass, seed, c.lo, c.hi, 24.0);
            dcsynth::GuitarChartVoice eager(c.bass, c.config), lazy(c.bass, c.config);
            lazy.useLazyZones(true);
            const auto xe = renderTable(*table, eager, 26.0);   // prepare() loads the packs
            lazy.prepare(48000.0, 2048);
            lazy.preloadZones(*table);
            const int64_t held = lazy.packBytes(), full = eager.packBytes();
            const auto xl = renderTable(*table, lazy, 26.0);
            std::vector<std::shared_ptr<const SamplePack>> packs;
            lazy.lazyPacks(packs);
            bool asked = false;
            for (const auto& p : packs)
                asked |= zoneRequestsPending(*p);
            size_t firstDiff = xe.size();
            for (size_t i = 0; i < xe.size() && i < xl.size(); ++i)
                if (std::memcmp(&xe[i], &xl[i], sizeof(float)) != 0)
                {
                    firstDiff = i;
                    break;
                }
            const std::string s = name + " seed " + std::to_string(seed);
            check(eager.sampled() && lazy.sampled() && ! packs.empty(), "both voices play recordings, the lazy one lazily" + s);
            check(rms(xe, 1.0, 20.0, 48000.0) > 1e-3, "the window chart is audible" + s);
            check(xe.size() == xl.size() && firstDiff == xe.size(), "the lazy voice renders the chart BIT-IDENTICAL" + s
                  + (firstDiff < xe.size() ? " (first difference at " + std::to_string(firstDiff / 48000.0) + " s)" : std::string()));
            check(! asked, "the plan covered every note: nothing was asked of a worker" + s);
            check(held > 0 && (double) held < c.most * (double) full, "the lazy voice holds part of the packs: " + std::to_string(held / 1024) + " of "
                  + std::to_string(full / 1024) + " KiB" + s);
            std::printf("  lazy zones%s: %lld of %lld KiB decoded\n", s.c_str(), (long long) (held / 1024), (long long) (full / 1024));
        }
    }

    // A TIE into a palm mute far up the string: the string keeps sounding (no
    // new recording), so its release noise is at the TIED note's pitch even
    // though a palm mute alone would play the articulation pack's recording.
    {
        static const int open[6] = { 40, 45, 50, 55, 59, 64 };
        std::vector<dca_chart_note> v { chartNote(0.1, 0.12, 1, 2, open, DCA_CHART_NOTE_LINK_NEXT),
                                        chartNote(0.19, 0.6, 1, 9, open, DCA_CHART_NOTE_PALM_MUTE) };
        std::string err;
        auto tt = dcsynth::ChartNoteTable::build(v.data(), 2, (int32_t) sizeof(dca_chart_note), nullptr, 0, err);
        dcsynth::GuitarChartVoice eager(false, strat1), lazy(false, strat1);
        lazy.useLazyZones(true);
        lazy.prepare(48000.0, 2048);
        lazy.preloadZones(*tt);
        const auto xe = renderTable(*tt, eager, 1.2), xl = renderTable(*tt, lazy, 1.2);
        std::vector<std::shared_ptr<const SamplePack>> packs;
        lazy.lazyPacks(packs);
        bool asked = false;
        for (const auto& p : packs)
            asked |= zoneRequestsPending(*p);
        check(rms(xe, 0.1, 0.9, 48000.0) > 1e-4 && xe.size() == xl.size() && std::memcmp(xe.data(), xl.data(), xe.size() * sizeof(float)) == 0
                  && ! asked,
              "a tie into a palm mute renders bit-identical from its planned zones");
    }

    // A note OUTSIDE the decoded set: the voice plays the nearest decoded
    // recording without allocating, asks for the zone, and once a worker has
    // decoded it the note is the eager voice's again.
    {
        static const int open[6] = { 40, 45, 50, 55, 59, 64 };
        std::vector<dca_chart_note> a { chartNote(0.1, 1.0, 2, 3, open) }, b { chartNote(0.1, 1.0, 4, 14, open) };
        std::string err;
        auto ta = dcsynth::ChartNoteTable::build(a.data(), 1, (int32_t) sizeof(dca_chart_note), nullptr, 0, err);
        auto tb = dcsynth::ChartNoteTable::build(b.data(), 1, (int32_t) sizeof(dca_chart_note), nullptr, 0, err);
        dcsynth::GuitarChartVoice eager(false, strat1), lazy(false, strat1);
        lazy.useLazyZones(true);
        lazy.prepare(48000.0, 2048);
        lazy.preloadZones(*ta);   // fret 3 on string 2 (+ margin), nothing near fret 14 on string 4
        const auto xe = renderTable(*tb, eager, 1.2);
        const auto xMiss = renderTable(*tb, lazy, 1.2);
        std::vector<std::shared_ptr<const SamplePack>> packs;
        lazy.lazyPacks(packs);
        check(rms(xMiss, 0.15, 0.6, 48000.0) > 1e-3, "a note outside the decoded set still sounds (the nearest decoded recording)");
        check(! packs.empty() && zoneRequestsPending(*packs[0]), "a note outside the decoded set asks a worker for its zones");
        int decoded = 0;
        for (const auto& p : packs)
            decoded += serviceZoneRequests(*p);
        const auto xNow = renderTable(*tb, lazy, 1.2);
        check(decoded > 0 && xNow.size() == xe.size() && std::memcmp(xNow.data(), xe.data(), xe.size() * sizeof(float)) == 0,
              "once its zones are decoded the note is bit-identical to the eager voice's");
    }
#ifdef _WIN32
    _putenv_s("DOTCHARTER_VOICE_DIR", "");
#else
    unsetenv("DOTCHARTER_VOICE_DIR");
#endif
}

/// The miss path allocates nothing on the render side: a lazy sampler with
/// NOTHING decoded plays a dense chart (every note a miss) under the CRT hook.
void testLazyMissesNeverAllocate(const Fixture& g)
{
#ifdef _MSC_VER
    SamplePack lazy = lazyOf(g.bytes);
    Song s;
    for (double t = 0.05; t < 6.0; t += 0.1)
        s.add(t, 0.3, (int) (t * 7) % 6, (int) (t * 13) % 15);
    PlanOptions po;
    PlannedTimeline plan;
    planChart(s.notes.data(), (int) s.notes.size(), s.bends.data(), (int) s.bends.size(), nullptr, 0, po, plan);
    const auto tl = plan.view(1);
    // Half the pack decoded: some notes play, some miss and fall back.
    std::vector<uint8_t> want(lazy.zones.size(), 0);
    for (size_t i = 0; i < want.size(); i += 2)
        want[i] = 1;
    ensureZones(lazy, want);
    ChartTimelineVoice v(InstrumentSpec::guitar(), &lazy);
    v.prepare(48000.0, 128);
    std::vector<float> buf(128);
    VoiceClock c;
    c.rolling = true;
    c.songSecondsPerSample = 1.0 / 48000.0;
    allocations = 0;
    armed = true;
    for (int bl = 0; bl < 2400; ++bl)
    {
        c.songTime = bl * 128.0 / 48000.0;
        v.render(buf.data(), 128, c, tl);
    }
    armed = false;
    check(allocations == 0, "a lazy pack's misses allocated " + std::to_string(allocations) + " times on the render path");
    check(zoneRequestsPending(lazy), "the misses were asked for");
#else
    (void) g;
#endif
}

} // namespace

int main()
{
#ifdef _MSC_VER
    _CrtSetAllocHook(allocHook);
#endif
    std::printf("sample voice tests\n");
    const Fixture g = makeFixture(false), b = makeFixture(true), arts = makeArtsFixture(), slaps = makeFixture(true, true);
    testParser(g);
    testCodec(g);
    testPitch(g, b);
    testGestures(g);
    testNoteOffsHonoured(g);
    testArticulations(g, b);
    checkHarmonicsWithCapo("sampled", [&g](const InstrumentSpec& spec) { return std::make_unique<SampleInstrument>(spec, &g.pack); }, 3.0);
    testEnginePathWithCapo(g);
    testArticulationPack(g, arts);
    testSlapPopAndDeadLevels(g, b);
    testRecordedSlaps(b, slaps);
    testVibratoIsAHand();
    testRepeatedNotesVary();
    testStrumHumanised();
    testPullOffHasNoPick(g);
    testLegatoOntoASilentStringHasNoPick(g, b);
    testDeterminismAndRt(g);
    const Fixture strat = makeStratFixture();
    testLazyPackDecodesOnDemand(g);
    testLazySelectionMatchesEager(g, strat);
    testDemandCoversEveryNote(g, arts, strat, slaps);
    testLazyMissesNeverAllocate(g);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
