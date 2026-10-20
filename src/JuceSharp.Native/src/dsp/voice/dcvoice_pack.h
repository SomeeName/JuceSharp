// dcvoice_pack.h — the chart voice's SAMPLE PACK (guitar.dcvp / bass.dcvp):
// format, parser, loader and zone selection. Plain C++17, no JUCE.
//
// The packs are built OUTSIDE git by tools/voice/build_voice_pack.py from
// pinned recordings and shipped next to the app. Every pack a release ships is
// built from CC0 1.0 / CC BY 4.0 recordings — see CREDITS.md. The packs ship
// (`voices/` beside juce_sharp.dll). The format is documented in that tool;
// the reader below validates every offset before it trusts it, because a pack
// is a file on a user's disk.
//
// VERSIONS: 1 stores the samples as raw int16; 2 (#408) stores each zone
// LOSSLESSLY CODED (dcvoice_codec.h) and parsePack decodes every zone into the
// same int16 buffer, so a loaded v2 pack is bit-identical to its v1 build. Any
// other version is REJECTED: an older DLL refuses a v2 pack rather than reading
// coded bytes as audio.
//
// THREADS: parsePack / loadPackFile allocate (and decode, on worker threads they
// join before returning) — control thread only. selectZone and everything const
// is allocation-free and safe on the audio thread.
//
// LAZY ZONES (#417): a pack loaded by loadPackLazy / parsePackLazy holds its
// zone TABLE only; each zone's samples are decoded on a CONTROL thread when
// someone asks for them (ensureZones: the chart a voice is about to play, plus
// a margin) and published by an atomic pointer. The audio thread never decodes,
// allocates or waits: selectZone picks the group a fully decoded pack would
// pick when every zone of it is decoded (so the render is bit-identical), and
// otherwise FLAGS that group for a worker (serviceZoneRequests) and plays the
// nearest group that IS decoded, retuned — or nothing when there is none. A
// decoded zone stays until the pack is freed (its last holder goes).
#pragma once

#include "dcvoice_codec.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace dcvoice {

/// The pack versions this reader understands (see VERSIONS above).
inline constexpr uint32_t kPackVersionRaw = 1;
inline constexpr uint32_t kPackVersionCoded = 2;
/// Header bytes 20..23 of a version-2 pack: the zone codec. 1 = dcvoice_codec.h.
inline constexpr uint32_t kPackCodecLossless = 1;

enum class Art : uint8_t
{
    Sustain = 0,   // an ordinary picked / plucked note, natural decay
    Palm = 1,      // palm-muted / staccato note
    Legato = 2,    // a hammered-on note (no pick)
    Release = 3,   // the fret hand letting go (string noise)
    Ghost = 4,     // bass ghost note
    Dead = 5,      // fret-hand-muted string ("chk"); rootHz = the open string it was played on
    Slap = 6,      // the thumb of a slap on muted strings (unpitched)
    NatHarm = 7,   // natural harmonic; rootHz = the harmonic's pitch, fret = the node touched (12 / 7 / 5 / 4 / 3)
    Pinch = 8,     // pinch harmonic; rootHz = the fretted note's pitch
    SlapNote = 9,  // a RECORDED thumb slap of a pitched note (bass, #109); string / fret measured
};
inline constexpr int kArtCount = 10;

/// The harmonic number of a natural harmonic recorded at node fret `fret`
/// (the pack builder's HARMONIC_NODE), 0 when it is not a node it records.
inline int harmonicOfNode(int fret) noexcept
{
    switch (fret)
    {
        case 12: return 2;
        case 7: return 3;
        case 5: return 4;
        case 4: return 5;
        case 3: return 6;
        default: return 0;
    }
}

struct PackZone
{
    uint8_t art = 0;
    uint8_t variant = 0;   // 0 default / fingers, 1 pick
    uint8_t layer = 0;     // velocity layer, 0 = softest
    uint8_t rr = 0;        // round robin
    float rootHz = 0.0f;   // measured pitch (pitched articulations)
    float gain = 0.0f;     // int16 -> float, level-normalised
    uint32_t length = 0;   // samples
    uint64_t start = 0;    // into SamplePack::data
    int8_t string = -1;    // the string it was recorded on, 0 = lowest (-1 = unknown)
    int8_t fret = -1;      // the fret it was recorded at (-1 = unknown)
};

/// The decoded-on-demand samples of a LAZY pack (see LAZY ZONES above). The
/// atomics are read by the audio thread; everything else belongs to the
/// control thread that holds `mutex`.
struct ZoneStore
{
    // Where the zones decode from: a file (the engine), or an image in memory.
    std::filesystem::path path;
    std::shared_ptr<const std::vector<uint8_t>> image;
    uint64_t fileBytes = 0;   // the file's size when the table was read: a changed file decodes nothing
    bool coded = false;       // version 2 (codec 1); version 1 is raw int16
    uint64_t dataOff = 0;
    std::vector<uint64_t> srcOff;    // per zone, into the data region
    std::vector<uint32_t> srcBytes;

    std::unique_ptr<std::atomic<const int16_t*>[]> ptr;   // per zone; nullptr = not decoded
    std::unique_ptr<std::atomic<uint8_t>[]> requested;    // per zone; set by the audio thread on a miss
    std::atomic<bool> pending { false };                  // any `requested` set since the last service
    std::atomic<int64_t> bytes { 0 };                     // decoded sample bytes held

    std::mutex mutex;
    std::vector<std::unique_ptr<int16_t[]>> owned;   // per zone (mutex)
    std::vector<uint8_t> broken;                     // per zone: failed to decode, never retried (mutex)
};

struct SamplePack
{
    int instrument = 0;    // 0 guitar, 1 bass
    double sampleRate = 44100.0;
    std::vector<PackZone> zones;
    std::vector<int16_t> data;   // every zone's samples (an EAGER pack); empty in a lazy one
    std::string credits;
    int layerCount[kArtCount][2] {};
    /// True when the pack recorded the SAME pitch on more than one string (the
    /// Strat packs: every string and fret). Only then does a zone's string mean
    /// something to selectZone; a chromatic set's strings are estimates.
    bool multiString = false;
    /// Non-null: a LAZY pack (#417) — zones are decoded on demand (ensureZones).
    std::shared_ptr<ZoneStore> lazy;

    size_t indexOf(const PackZone& z) const noexcept { return (size_t) (&z - zones.data()); }
    /// The zone's samples; nullptr only for a lazy zone not decoded yet (which
    /// selectZone never returns).
    const int16_t* samples(const PackZone& z) const noexcept
    {
        return lazy == nullptr ? data.data() + z.start : lazy->ptr[indexOf(z)].load(std::memory_order_acquire);
    }
    /// True when the zone's samples can be played now. Audio-thread safe.
    bool ready(const PackZone& z) const noexcept
    {
        return lazy == nullptr || lazy->ptr[indexOf(z)].load(std::memory_order_acquire) != nullptr;
    }
    /// AUDIO thread: asks a worker to decode the zone (two relaxed stores, no
    /// lock, no allocation). A no-op on an eager pack.
    void request(const PackZone& z) const noexcept
    {
        if (lazy == nullptr)
            return;
        std::atomic<uint8_t>& r = lazy->requested[indexOf(z)];
        if (r.load(std::memory_order_relaxed) == 0)
        {
            r.store(1, std::memory_order_relaxed);
            lazy->pending.store(true, std::memory_order_release);
        }
    }
    /// Memory this pack holds (its audio and zone table) — a lazy pack's
    /// DECODED zones only.
    size_t bytes() const noexcept
    {
        size_t b = data.size() * sizeof(int16_t) + zones.size() * sizeof(PackZone) + credits.size();
        if (lazy != nullptr)
            b += (size_t) lazy->bytes.load(std::memory_order_relaxed)
                 + zones.size() * (sizeof(std::atomic<const int16_t*>) + sizeof(std::unique_ptr<int16_t[]>) + sizeof(uint64_t) + sizeof(uint32_t) + 2);
        return b;
    }
    bool has(Art a, int variant) const noexcept { return layerCount[(int) a][variant & 1] > 0; }
};

namespace packdetail {
template <typename T>
inline T rd(const uint8_t* p) noexcept
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
} // namespace packdetail

namespace packdetail {
/// Where a pack's samples are, per zone (filled by parseIndex).
struct Index
{
    bool coded = false;
    uint64_t dataOff = 0, dataSamples = 0, textOff = 0;
    uint32_t textBytes = 0;
    std::vector<uint64_t> srcOff;   // per zone, into the data region (raw: start * 2)
    std::vector<uint32_t> srcBytes;
};

/// Parses and validates the header and the zone table of a pack of `n` bytes,
/// of which the first `avail` are in `b` (the zone table must lie inside them).
/// Fills everything of `p` but the samples and the credits. Control thread.
inline bool parseIndex(const uint8_t* b, size_t avail, uint64_t n, SamplePack& p, Index& ix, std::string& error)
{
    if (b == nullptr || n < 64 || avail < 64 || std::memcmp(b, "DCVP", 4) != 0)
    {
        error = "not a DCVP pack";
        return false;
    }
    const uint32_t version = rd<uint32_t>(b + 4);
    if (version != kPackVersionRaw && version != kPackVersionCoded)
    {
        error = "unsupported pack version " + std::to_string(version);
        return false;
    }
    p.instrument = (int) rd<uint32_t>(b + 8);
    const uint32_t zoneCount = rd<uint32_t>(b + 12);
    p.sampleRate = rd<float>(b + 16);
    const uint64_t zonesOff = rd<uint64_t>(b + 24);
    const uint64_t dataOff = rd<uint64_t>(b + 32);
    const uint64_t dataSamples = rd<uint64_t>(b + 40);
    const uint64_t textOff = rd<uint64_t>(b + 48);
    const uint32_t textBytes = rd<uint32_t>(b + 56);
    if (! (p.sampleRate >= 8000.0 && p.sampleRate <= 384000.0) || zoneCount == 0 || zoneCount > 100000)
    {
        error = "bad header";
        return false;
    }
    const bool coded = version == kPackVersionCoded;
    if (coded && rd<uint32_t>(b + 20) != kPackCodecLossless)
    {
        error = "unsupported sample codec " + std::to_string(rd<uint32_t>(b + 20));
        return false;
    }
    // A coded pack's data region runs up to the text; its sample count is what
    // it DECODES to, so it is capped (1 GiB of int16) instead of tied to the file.
    if (zonesOff + (uint64_t) zoneCount * 40 > n || dataOff > n || textOff > n || textBytes > n - textOff
        || (dataOff & 1) != 0
        || (coded ? (textOff < dataOff || dataSamples > (uint64_t) 512 * 1024 * 1024) : dataSamples > (n - dataOff) / 2))
    {
        error = "offsets outside the file";
        return false;
    }
    if (zonesOff + (uint64_t) zoneCount * 40 > avail)
    {
        error = "the zone table was not read";
        return false;
    }
    const uint64_t codedRegion = coded ? textOff - dataOff : 0;
    ix.coded = coded;
    ix.dataOff = dataOff;
    ix.dataSamples = dataSamples;
    ix.textOff = textOff;
    ix.textBytes = textBytes;
    ix.srcOff.assign(zoneCount, 0);
    ix.srcBytes.assign(zoneCount, 0);
    p.zones.resize(zoneCount);
    for (uint32_t i = 0; i < zoneCount; ++i)
    {
        const uint8_t* z = b + zonesOff + (uint64_t) i * 40;
        PackZone& o = p.zones[i];
        o.art = z[0];
        o.variant = z[1];
        o.layer = z[2];
        o.rr = z[3];
        o.rootHz = rd<float>(z + 4);
        o.gain = rd<float>(z + 8);
        o.length = rd<uint32_t>(z + 12);
        o.start = rd<uint64_t>(z + 16);
        // Bytes 28 / 29 (string + 1, fret + 1; 0 = unknown) were reserved zeros
        // before #50, so an older pack reads as "unknown" — still version 1.
        o.string = z[28] > 0 && z[28] <= 16 ? (int8_t) (z[28] - 1) : (int8_t) -1;
        o.fret = z[29] > 0 && z[29] <= 64 ? (int8_t) (z[29] - 1) : (int8_t) -1;
        if (coded)
        {
            // v2: bytes 24..27 the zone's coded size, 32..39 its offset into the
            // data region (both reserved zeros in v1, never read there).
            ix.srcBytes[i] = rd<uint32_t>(z + 24);
            ix.srcOff[i] = rd<uint64_t>(z + 32);
            if (ix.srcOff[i] > codedRegion || ix.srcBytes[i] > codedRegion - ix.srcOff[i])
            {
                error = "zone " + std::to_string(i) + "'s coded samples lie outside the data";
                return false;
            }
        }
        if (o.art >= kArtCount || o.variant > 1 || ! std::isfinite(o.rootHz) || o.rootHz <= 0.0f
            || ! std::isfinite(o.gain) || o.gain <= 0.0f || o.gain > 1.0f || o.length < 8
            || o.start + o.length > dataSamples)
        {
            error = "zone " + std::to_string(i) + " is malformed";
            return false;
        }
        if (! coded)
        {
            ix.srcOff[i] = o.start * 2;
            ix.srcBytes[i] = o.length * 2;
        }
        p.layerCount[o.art][o.variant] = std::max(p.layerCount[o.art][o.variant], (int) o.layer + 1);
    }
    {
        std::map<long, int> stringAt;   // (art, quarter-tone) -> first string seen
        for (const PackZone& z : p.zones)
        {
            if (z.string < 0)
                continue;
            const long key = (long) z.art * 100000L + (long) std::lround(std::log2((double) z.rootHz) * 24.0);
            auto it = stringAt.find(key);
            if (it == stringAt.end())
                stringAt.emplace(key, z.string);
            else if (it->second != z.string)
                p.multiString = true;
        }
    }
    return true;
}

/// Decodes zone `i` from its stored bytes into `dst` (`zone.length` samples).
inline bool decodeOne(bool coded, const uint8_t* src, uint32_t srcBytes, int16_t* dst, uint32_t length) noexcept
{
    if (coded)
        return codec::decodeZone(src, srcBytes, dst, length);
    if (srcBytes != (uint64_t) length * 2)
        return false;
    std::memcpy(dst, src, (size_t) srcBytes);
    return true;
}

/// Runs `job(k)` for k in [0, count) on up to 8 threads (half the cores),
/// joined before it returns. Control thread.
template <typename Job>
inline void parallelFor(uint32_t count, Job&& job)
{
    std::atomic<uint32_t> next { 0 };
    auto work = [&] {
        for (uint32_t k; (k = next.fetch_add(1)) < count;)
            job(k);
    };
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned workers = std::min<unsigned>(std::max<unsigned>(hw / 2, 1u), std::min<uint32_t>(count, 8u));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < workers; ++t)
    {
        try { pool.emplace_back(work); }
        catch (...) { break; }   // no thread: this one does the rest
    }
    work();
    for (std::thread& t : pool)
        t.join();
}
} // namespace packdetail

/// Parses a whole pack image. Control thread (allocates). False + `error` on
/// anything malformed; `out` is then untouched.
inline bool parsePack(const uint8_t* b, size_t n, SamplePack& out, std::string& error)
{
    SamplePack p;
    packdetail::Index ix;
    if (! packdetail::parseIndex(b, n, n, p, ix, error))
        return false;
    const uint32_t zoneCount = (uint32_t) p.zones.size();
    p.data.resize((size_t) ix.dataSamples);
    if (! ix.coded)
        std::memcpy(p.data.data(), b + ix.dataOff, (size_t) ix.dataSamples * 2);
    else
    {
        // Every zone decodes independently: spread them over a few workers,
        // joined before this returns (pack load is on the control thread).
        std::atomic<int64_t> failed { -1 };
        packdetail::parallelFor(zoneCount, [&](uint32_t i) {
            if (failed.load() >= 0)
                return;
            const PackZone& zone = p.zones[i];
            if (! codec::decodeZone(b + ix.dataOff + ix.srcOff[i], ix.srcBytes[i], p.data.data() + zone.start, zone.length))
                failed.store((int64_t) i);
        });
        if (failed.load() >= 0)
        {
            error = "zone " + std::to_string(failed.load()) + "'s coded samples are malformed";
            return false;
        }
    }
    p.credits.assign((const char*) b + ix.textOff, ix.textBytes);
    out = std::move(p);
    return true;
}

/// Reads and parses a pack file. Control thread.
inline bool loadPackFile(const std::string& path, SamplePack& out, std::string& error)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (! f)
    {
        error = "cannot open " + path;
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size <= 0 || size > (std::streamoff) 2000 * 1024 * 1024)
    {
        error = "bad size";
        return false;
    }
    std::vector<uint8_t> bytes((size_t) size);
    f.seekg(0);
    if (! f.read((char*) bytes.data(), size))
    {
        error = "read failed";
        return false;
    }
    return parsePack(bytes.data(), bytes.size(), out, error);
}

namespace packdetail {
inline void makeLazy(SamplePack& p, Index& ix, std::filesystem::path path, std::shared_ptr<const std::vector<uint8_t>> image,
                     uint64_t fileBytes)
{
    auto s = std::make_shared<ZoneStore>();
    const size_t n = p.zones.size();
    s->path = std::move(path);
    s->image = std::move(image);
    s->fileBytes = fileBytes;
    s->coded = ix.coded;
    s->dataOff = ix.dataOff;
    s->srcOff = std::move(ix.srcOff);
    s->srcBytes = std::move(ix.srcBytes);
    s->ptr.reset(new std::atomic<const int16_t*>[n]);
    s->requested.reset(new std::atomic<uint8_t>[n]);
    for (size_t i = 0; i < n; ++i)
    {
        s->ptr[i].store(nullptr, std::memory_order_relaxed);
        s->requested[i].store(0, std::memory_order_relaxed);
    }
    s->owned.resize(n);
    s->broken.assign(n, 0);
    p.data.clear();
    p.lazy = std::move(s);
}
} // namespace packdetail

/// A LAZY pack from an image in memory (kept, shared): the zone table is
/// validated now, no zone is decoded. Control thread.
inline bool parsePackLazy(std::shared_ptr<const std::vector<uint8_t>> image, SamplePack& out, std::string& error)
{
    if (image == nullptr)
    {
        error = "no image";
        return false;
    }
    SamplePack p;
    packdetail::Index ix;
    if (! packdetail::parseIndex(image->data(), image->size(), image->size(), p, ix, error))
        return false;
    p.credits.assign((const char*) image->data() + ix.textOff, ix.textBytes);
    const uint64_t n = image->size();
    packdetail::makeLazy(p, ix, {}, std::move(image), n);
    out = std::move(p);
    return true;
}

/// A LAZY pack from a file: reads the header, the zone table and the credits —
/// no samples (the file is read again, zone by zone, by ensureZones). Control
/// thread (I/O).
inline bool loadPackLazy(const std::filesystem::path& path, SamplePack& out, std::string& error)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (! f)
    {
        error = "cannot open " + path.u8string();
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size < 64 || size > (std::streamoff) 2000 * 1024 * 1024)
    {
        error = "bad size";
        return false;
    }
    uint8_t head[64];
    f.seekg(0);
    if (! f.read((char*) head, 64) || std::memcmp(head, "DCVP", 4) != 0)
    {
        error = "not a DCVP pack";
        return false;
    }
    // The prefix up to the end of the zone table (the header names it; capped
    // like the parser caps it: 100,000 zones of 40 bytes).
    const uint64_t zonesOff = packdetail::rd<uint64_t>(head + 24);
    const uint64_t zoneCount = packdetail::rd<uint32_t>(head + 12);
    const uint64_t prefix = zonesOff + std::min<uint64_t>(zoneCount, 100000) * 40;
    if (prefix > (uint64_t) size || zonesOff < 64)
    {
        error = "offsets outside the file";
        return false;
    }
    std::vector<uint8_t> bytes((size_t) prefix);
    f.seekg(0);
    if (! f.read((char*) bytes.data(), (std::streamsize) prefix))
    {
        error = "read failed";
        return false;
    }
    SamplePack p;
    packdetail::Index ix;
    if (! packdetail::parseIndex(bytes.data(), bytes.size(), (uint64_t) size, p, ix, error))
        return false;
    p.credits.resize(ix.textBytes);
    f.seekg((std::streamoff) ix.textOff);
    if (ix.textBytes > 0 && ! f.read(p.credits.data(), (std::streamsize) ix.textBytes))
    {
        error = "read failed";
        return false;
    }
    packdetail::makeLazy(p, ix, path, nullptr, (uint64_t) size);
    out = std::move(p);
    return true;
}

/// CONTROL thread: decodes every zone of a lazy pack with `want[i]` set that
/// is not decoded yet, and publishes it. Reads the file (or the image), may
/// take hundreds of milliseconds, serialises with any other ensureZones on the
/// same pack. Returns the number of zones published; a zone that does not
/// decode (a damaged or replaced file) stays absent and is never retried. A
/// no-op on an eager pack.
inline int ensureZones(const SamplePack& pack, const std::vector<uint8_t>& want)
{
    ZoneStore* s = pack.lazy.get();
    if (s == nullptr)
        return 0;
    std::lock_guard<std::mutex> lock(s->mutex);
    std::vector<uint32_t> todo;
    const size_t n = std::min(want.size(), pack.zones.size());
    for (size_t i = 0; i < n; ++i)
        if (want[i] != 0 && s->owned[i] == nullptr && s->broken[i] == 0)
            todo.push_back((uint32_t) i);
    if (todo.empty())
        return 0;

    // The stored bytes of every zone to decode, read in file order.
    std::vector<uint64_t> at(todo.size());
    uint64_t total = 0;
    for (size_t k = 0; k < todo.size(); ++k)
    {
        at[k] = total;
        total += s->srcBytes[todo[k]];
    }
    std::vector<uint8_t> src((size_t) total);
    bool readOk = true;
    if (s->image != nullptr)
    {
        for (size_t k = 0; k < todo.size(); ++k)
            std::memcpy(src.data() + at[k], s->image->data() + s->dataOff + s->srcOff[todo[k]], s->srcBytes[todo[k]]);
    }
    else
    {
        std::error_code ec;
        const uint64_t now = (uint64_t) std::filesystem::file_size(s->path, ec);
        std::ifstream f(s->path, std::ios::binary);
        readOk = ! ec && now == s->fileBytes && (bool) f;
        for (size_t k = 0; readOk && k < todo.size(); ++k)
        {
            f.seekg((std::streamoff) (s->dataOff + s->srcOff[todo[k]]));
            readOk = (bool) f.read((char*) src.data() + at[k], (std::streamsize) s->srcBytes[todo[k]]);
        }
    }
    if (! readOk)
    {
        for (uint32_t i : todo)
            s->broken[i] = 1;
        return 0;
    }
    std::vector<std::unique_ptr<int16_t[]>> out(todo.size());
    for (size_t k = 0; k < todo.size(); ++k)
        out[k].reset(new int16_t[pack.zones[todo[k]].length]);
    std::vector<uint8_t> ok(todo.size(), 0);
    packdetail::parallelFor((uint32_t) todo.size(), [&](uint32_t k) {
        const PackZone& z = pack.zones[todo[k]];
        ok[k] = packdetail::decodeOne(s->coded, src.data() + at[k], s->srcBytes[todo[k]], out[k].get(), z.length) ? 1 : 0;
    });
    int published = 0;
    for (size_t k = 0; k < todo.size(); ++k)
    {
        const uint32_t i = todo[k];
        if (ok[k] == 0)
        {
            s->broken[i] = 1;
            continue;
        }
        s->owned[i] = std::move(out[k]);
        s->bytes.fetch_add((int64_t) pack.zones[i].length * 2, std::memory_order_relaxed);
        s->ptr[i].store(s->owned[i].get(), std::memory_order_release);
        ++published;
    }
    return published;
}

/// CONTROL thread: decodes the zones the audio thread asked for (selectZone on
/// a miss). Returns the number published.
inline int serviceZoneRequests(const SamplePack& pack)
{
    ZoneStore* s = pack.lazy.get();
    if (s == nullptr || ! s->pending.exchange(false, std::memory_order_acq_rel))
        return 0;
    std::vector<uint8_t> want(pack.zones.size(), 0);
    for (size_t i = 0; i < want.size(); ++i)
        want[i] = s->requested[i].exchange(0, std::memory_order_acquire);
    return ensureZones(pack, want);
}

/// True when the audio thread is waiting for zones of this pack. Any thread.
inline bool zoneRequestsPending(const SamplePack& pack) noexcept
{
    return pack.lazy != nullptr && pack.lazy->pending.load(std::memory_order_acquire);
}

/// Decoded zones of a lazy pack, as a want-list (1 = decoded). Control thread.
inline std::vector<uint8_t> decodedZones(const SamplePack& pack)
{
    std::vector<uint8_t> have(pack.zones.size(), 1);
    if (pack.lazy != nullptr)
        for (size_t i = 0; i < have.size(); ++i)
            have[i] = pack.lazy->ptr[i].load(std::memory_order_acquire) != nullptr ? 1 : 0;
    return have;
}

namespace packdetail {
/// The pitch group selectZone settles on (see selectZone).
struct Group
{
    int onString = -1;
    double bestLog = 0.0;
    bool found = false;
};
inline constexpr double kGroup = 0.25 / 12.0;

/// Among every zone, or (readyOnly) only the decoded ones.
inline Group findGroup(const SamplePack& pack, uint8_t a, int variant, double lf, int string, bool readyOnly) noexcept
{
    Group g;
    // The charted string, when the pack recorded it within two semitones.
    if (string >= 0 && pack.multiString)
        for (const PackZone& z : pack.zones)
            if (z.art == a && z.variant == variant && z.string == string
                && std::fabs(std::log2((double) z.rootHz) - lf) < 2.05 / 12.0 && (! readyOnly || pack.ready(z)))
            {
                g.onString = string;
                break;
            }
    double bestDist = 1.0e9;
    for (const PackZone& z : pack.zones)
    {
        if (z.art != a || z.variant != variant || (g.onString >= 0 && z.string != g.onString) || (readyOnly && ! pack.ready(z)))
            continue;
        const double d = std::fabs(std::log2((double) z.rootHz) - lf);
        if (d < bestDist)
        {
            bestDist = d;
            g.bestLog = std::log2((double) z.rootHz);
            g.found = true;
        }
    }
    return g;
}

inline bool inGroup(const PackZone& z, uint8_t a, int variant, const Group& g) noexcept
{
    return z.art == a && z.variant == variant && (g.onString < 0 || z.string == g.onString)
           && std::fabs(std::log2((double) z.rootHz) - g.bestLog) < kGroup;
}

/// The nearest velocity layer of the group, then round robin.
inline const PackZone* pickInGroup(const SamplePack& pack, uint8_t a, int variant, const Group& g, int layer, uint32_t rr,
                                   bool readyOnly) noexcept
{
    auto member = [&](const PackZone& z) { return inGroup(z, a, variant, g) && (! readyOnly || pack.ready(z)); };
    int bestLayer = -1, layerDist = 1 << 20;
    for (const PackZone& z : pack.zones)
        if (member(z) && std::abs((int) z.layer - layer) < layerDist)
        {
            layerDist = std::abs((int) z.layer - layer);
            bestLayer = z.layer;
        }
    int count = 0;
    for (const PackZone& z : pack.zones)
        if (member(z) && z.layer == bestLayer)
            ++count;
    if (count == 0)
        return nullptr;
    int want = (int) (rr % (uint32_t) count);
    for (const PackZone& z : pack.zones)
        if (member(z) && z.layer == bestLayer && want-- == 0)
            return &z;
    return nullptr;
}
} // namespace packdetail

/// Marks in `want` every zone of the pitch group selectZone settles on for
/// (art, variant, hz, string) in a fully decoded pack — EVERY layer and round
/// robin of it, so whatever velocity and round robin a note brings, the render
/// is the fully decoded pack's. Control thread (demand planning, #417).
inline void markGroup(const SamplePack& pack, Art art, int variant, double hz, int string, std::vector<uint8_t>& want) noexcept
{
    const uint8_t a = (uint8_t) art;
    if (! pack.has(art, variant))
        variant = 0;
    if (! pack.has(art, variant) || hz <= 0.0 || want.size() < pack.zones.size())
        return;
    const packdetail::Group g = packdetail::findGroup(pack, a, variant, std::log2(hz), string, false);
    if (! g.found)
        return;
    for (size_t i = 0; i < pack.zones.size(); ++i)
        if (packdetail::inGroup(pack.zones[i], a, variant, g))
            want[i] = 1;
}

/// Marks every zone of an articulation (any variant). Control thread.
inline void markArt(const SamplePack& pack, Art art, std::vector<uint8_t>& want) noexcept
{
    for (size_t i = 0; i < pack.zones.size() && i < want.size(); ++i)
        if (pack.zones[i].art == (uint8_t) art)
            want[i] = 1;
}

/// Nearest PITCH GROUP (zones within a quarter semitone of the nearest root),
/// then the nearest velocity layer, then round robin. Falls back to variant 0
/// when the pack has no zone of `variant`. nullptr when the articulation is
/// absent. `string` >= 0 prefers the zones RECORDED ON THAT STRING (a pack with
/// every string and fret, like the Strat's) when one lies within two semitones;
/// otherwise every string competes. O(zones), no allocation — audio-thread safe.
///
/// A LAZY pack (#417): when every zone of that group is decoded the result is
/// exactly a fully decoded pack's. Otherwise the group is REQUESTED (a worker
/// decodes it, serviceZoneRequests) and the same rules run over the decoded
/// zones only — the nearest decoded recording, which the caller retunes — or
/// nullptr when none is decoded. Never blocks.
inline const PackZone* selectZone(const SamplePack& pack, Art art, int variant, double hz, int layer, uint32_t rr, int string = -1) noexcept
{
    const uint8_t a = (uint8_t) art;
    if (! pack.has(art, variant))
        variant = 0;
    if (! pack.has(art, variant) || hz <= 0.0)
        return nullptr;
    const double lf = std::log2(hz);
    const packdetail::Group all = packdetail::findGroup(pack, a, variant, lf, string, false);
    if (! all.found)
        return nullptr;
    if (pack.lazy == nullptr)
        return packdetail::pickInGroup(pack, a, variant, all, layer, rr, false);
    bool complete = true;
    for (const PackZone& z : pack.zones)
        if (packdetail::inGroup(z, a, variant, all) && ! pack.ready(z))
        {
            complete = false;
            pack.request(z);
        }
    if (complete)
        return packdetail::pickInGroup(pack, a, variant, all, layer, rr, false);
    const packdetail::Group got = packdetail::findGroup(pack, a, variant, lf, string, true);
    return got.found ? packdetail::pickInGroup(pack, a, variant, got, layer, rr, true) : nullptr;
}

} // namespace dcvoice
