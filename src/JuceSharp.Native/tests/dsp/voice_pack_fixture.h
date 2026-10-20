// voice_pack_fixture.h — synthetic sample packs for the chart-voice tests
// (instrument_models_test, the RT gate). Written byte for byte in the format
// tools/voice/build_voice_pack.py writes, so every test goes through the real
// parser. The real packs are built outside git and are not needed here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace voicefixture {

inline constexpr double kTau = 6.283185307179586;

inline double hzOf(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

/// One zone to write: articulation, variant, layer, rr, root, string, fret
/// (-1 = unknown), samples at 44.1 kHz.
struct Zone
{
    int art = 0, variant = 0, layer = 0, rr = 0;
    double root = 110.0;
    int string = -1, fret = -1;
    std::vector<float> y;
};

/// A FLAT-spectrum harmonic tone (every partial up to `partials` at equal
/// level, slowly decaying), so a filter's effect on each harmonic can be read
/// straight off the rendered spectrum.
inline std::vector<float> flatTone(double f0, double seconds, double t60, int partials = 24)
{
    std::vector<float> y((size_t) (seconds * 44100.0));
    for (size_t i = 0; i < y.size(); ++i)
    {
        const double t = i / 44100.0;
        double v = 0.0;
        for (int n = 1; n <= partials && n * f0 < 18000.0; ++n)
            v += std::sin(kTau * n * f0 * t + 0.7 * n * n);
        y[i] = (float) (0.04 * v * std::pow(10.0, -3.0 * t / t60));
    }
    return y;
}

inline std::vector<uint8_t> writePack(int instrument, const std::vector<Zone>& zones, const std::string& text)
{
    std::vector<uint8_t> b(64 + 40 * zones.size(), 0);
    const size_t dataOff = (b.size() + 15) & ~(size_t) 15;
    b.resize(dataOff, 0);
    uint64_t start = 0;
    for (size_t k = 0; k < zones.size(); ++k)
    {
        const Zone& z = zones[k];
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
        std::memcpy(b.data() + 64 + 40 * k, rec, 40);
        for (float v : z.y)
        {
            const int16_t q = (int16_t) std::lround(std::clamp(v / gain, -32767.0f, 32767.0f));
            b.push_back((uint8_t) (q & 0xFF));
            b.push_back((uint8_t) ((q >> 8) & 0xFF));
        }
        start += len;
    }
    const uint64_t textOff = b.size();
    b.insert(b.end(), text.begin(), text.end());
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
    auto put64 = [&](size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); };
    std::memcpy(b.data(), "DCVP", 4);
    put32(4, 1);
    put32(8, (uint32_t) instrument);
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

/// A guitar pack that recorded every string at frets 0..`frets` (like the Strat
/// packs): flat tones, so the pack is "pickup-less" and any colour comes from
/// the voice. `seconds` per note.
inline std::vector<uint8_t> stratLikePack(int frets = 12, double seconds = 2.0, int partials = 24)
{
    static const int open[6] = { 40, 45, 50, 55, 59, 64 };
    std::vector<Zone> zs;
    for (int s = 0; s < 6; ++s)
        for (int f = 0; f <= frets; ++f)
        {
            Zone z;
            z.art = 0; z.root = hzOf(open[s] + f); z.string = s; z.fret = f;
            z.y = flatTone(z.root, seconds, 6.0, partials);
            zs.push_back(std::move(z));
        }
    return writePack(0, zs, "fixture strat");
}

/// A chromatic bass pack with both variants (fingers 0, pick 1), strings
/// estimated like the builder does.
inline std::vector<uint8_t> bassLikePack(double seconds = 2.0)
{
    std::vector<Zone> zs;
    for (int m = 28; m <= 60; ++m)
        for (int variant = 0; variant < 2; ++variant)
        {
            Zone z;
            z.art = 0; z.variant = variant; z.root = hzOf(m);
            z.y = flatTone(z.root, seconds, 6.0, 16);
            zs.push_back(std::move(z));
        }
    return writePack(1, zs, "fixture bass");
}

inline void writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*) bytes.data(), (std::streamsize) bytes.size());
}

/// Writes the five Strat position packs (each its own synthetic content, the
/// same flat tones of `partials` harmonics — few make a smooth waveform on
/// which a click stands out), the Gretsch `guitar`
/// pack and the `bass` pack into `dir`.
inline void writeAllPacks(const std::filesystem::path& dir, int frets = 12, double seconds = 2.0, int partials = 24)
{
    std::filesystem::create_directories(dir);
    const auto strat = stratLikePack(frets, seconds, partials);
    for (const char* n : { "strat-neck", "strat-neck-middle", "strat-middle", "strat-middle-bridge", "strat-bridge", "guitar" })
        writeFile(dir / (std::string(n) + ".dcvp"), strat);
    writeFile(dir / "bass.dcvp", bassLikePack(seconds));
}

} // namespace voicefixture
