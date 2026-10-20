// Wwise-free Wwise Vorbis (.wem) encoder -- see WemVorbisEncoder.h.
//
// The writer is the inverse of WemVorbisDecoder.cpp's buildSetup / packet
// rebuild (a ww2ogg port), so every file it writes is pinned by a round trip
// through that decoder (JuceSharp.Tests WemEncoderTests).

#include "WemVorbisEncoder.h"
#include "WemVorbisCommon.h"

// The same bundled libogg/libvorbis the decoder links against (see the note in
// WemVorbisDecoder.cpp); vorbisenc.c is compiled into juce_audio_formats too.
extern "C"
{
 #include <juce_audio_formats/codecs/ogg/include/ogg/ogg.h>
}

namespace juce::OggVorbisNamespace
{
 #include <juce_audio_formats/codecs/vorbis/include/vorbis/codec.h>
 #include <juce_audio_formats/codecs/vorbis/include/vorbis/vorbisenc.h>
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

namespace dcwem
{
namespace ov = juce::OggVorbisNamespace;

namespace
{
using namespace detail;

// ---- the configuration Wwise uses for Rocksmith ------------------------------
constexpr float    kQuality     = 0.4f;   // libvorbis VBR quality (ffmpeg -q:a 4)
constexpr double   kLowpassKhz  = 18.0;   // gives Wwise's residue ends (1536 / 192)

// The Wwise-form setup packet that configuration produces, identical in all 26
// Wwise files measured and in the prototype: 215 bytes, FNV-1a-64 below. The
// constants copied from Wwise further down are valid for exactly this setup.
constexpr size_t   kWwiseSetupBytes = 215;
constexpr uint64_t kWwiseSetupFnv64 = 0xF0FD447FF724785EULL;

// fmt-chunk constants every measured Wwise file carries for that setup.
constexpr uint32_t kDecodeAllocSize = 16080;   // fmt +0x34 (vgmstream: dwDecodeAllocSize)
constexpr uint32_t kField0x38       = 16560;   // fmt +0x38 (constant, meaning unknown)
constexpr uint32_t kCodebookHash    = 0xEC69CB18u;   // fmt +0x3C (vgmstream: uHashCodebook?)
constexpr uint32_t kChannelMask     = 3;       // fmt +0x14 = absolute offset 40 (see WemWriter)
constexpr uint32_t kSeekGranularity = 16384;   // frames between seek-table entries

uint64_t fnv1a64(const std::vector<uint8_t>& b)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    for (const uint8_t x : b) { h ^= x; h *= 0x100000001B3ULL; }
    return h;
}

void putU16(std::vector<uint8_t>& o, uint32_t v)
{
    if (v > 0xFFFFu) fail("a 16-bit .wem field overflowed");
    o.push_back((uint8_t) v); o.push_back((uint8_t) (v >> 8));
}

void putU32(std::vector<uint8_t>& o, uint32_t v)
{
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t) (v >> (8 * i)));
}

void putTag(std::vector<uint8_t>& o, const char* t) { o.insert(o.end(), t, t + 4); }

void skip(BitReader& r, size_t bits)
{
    while (bits > 0)
    {
        const int n = (int) std::min<size_t>(bits, 32);
        r.read(n);
        bits -= (size_t) n;
    }
}

// Bits [from, to) of `data`, re-packed from bit 0 (zero-padded last byte).
std::string bitSlice(const uint8_t* data, size_t size, size_t from, size_t to)
{
    BitReader r(data, size);
    skip(r, from);
    std::vector<uint8_t> bytes;
    BitWriter w(bytes);
    for (size_t left = to - from; left > 0;)
    {
        const int n = (int) std::min<size_t>(left, 32);
        w.write(r.read(n), n);
        left -= (size_t) n;
    }
    std::string key(bytes.begin(), bytes.end());
    key += '|' + std::to_string(to - from);
    return key;
}

// Every packed aoTuV 6.03 library codebook, rebuilt to its standard form, keyed
// by those bits -> the ids carrying it (the library has 26 duplicate groups).
const std::unordered_map<std::string, std::vector<int>>& libraryIndex()
{
    static const auto index = [] {
        std::unordered_map<std::string, std::vector<int>> m;
        const CodebookLibrary& lib = library(true);
        for (int id = 0; id < lib.count() - 1; ++id)
        {
            std::vector<uint8_t> bytes;
            BitWriter w(bytes);
            try { lib.rebuild(id, w); }
            catch (const std::exception&) { continue; }   // an entry ww2ogg cannot rebuild either
            std::string key(bytes.begin(), bytes.end());
            key += '|' + std::to_string(w.bitsWritten());
            m[key].push_back(id);
        }
        return m;
    }();
    return index;
}

// Skip one standard (Vorbis I spec 3.2.1) codebook.
void skipStandardCodebook(BitReader& r)
{
    if (r.read(24) != 0x564342) fail("codebook sync pattern missing");
    const uint32_t dims = r.read(16), entries = r.read(24);
    if (r.read(1))   // ordered
    {
        r.read(5);
        uint32_t cur = 0;
        while (cur < entries)
        {
            const int bits = ilog(entries - cur);
            cur += r.read(bits);
            if (bits == 0) break;
        }
        if (cur > entries) fail("codebook ordered lengths overrun");
    }
    else
    {
        const uint32_t sparse = r.read(1);
        for (uint32_t e = 0; e < entries; ++e)
            if (! sparse || r.read(1)) r.read(5);
    }
    const uint32_t lookup = r.read(4);
    if (lookup == 1 || lookup == 2)
    {
        r.read(32); r.read(32);
        const int valueBits = (int) r.read(4) + 1;
        r.read(1);
        const uint64_t values = lookup == 1 ? maptype1Quantvals(entries, dims) : (uint64_t) entries * dims;
        skip(r, values * (uint64_t) valueBits);
    }
    else if (lookup != 0)
    {
        fail("codebook lookup type > 2");
    }
}

struct WwiseSetup
{
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> modeBlockflag;
    int modeBits = 0;
};

// Standard setup header packet (type 5) -> Wwise setup payload: the inverse of
// WemVorbisDecoder.cpp buildSetup, field for field.
WwiseSetup toWwiseSetup(const uint8_t* setup, size_t size, int channels)
{
    if (size < 7 || setup[0] != 5 || std::memcmp(setup + 1, "vorbis", 6) != 0) fail("not a Vorbis setup header");
    const size_t headerBits = 7 * 8;

    // Pass 1: where each codebook sits, and the library ids carrying it.
    BitReader scan(setup, size);
    skip(scan, headerBits);
    const uint32_t codebookCount = scan.read(8) + 1;
    std::vector<std::vector<int>> candidates;
    candidates.reserve(codebookCount);
    const auto& index = libraryIndex();
    for (uint32_t i = 0; i < codebookCount; ++i)
    {
        const size_t from = scan.bitsRead();
        skipStandardCodebook(scan);
        const auto it = index.find(bitSlice(setup, size, from, scan.bitsRead()));
        if (it == index.end()) fail("a codebook libvorbis wrote is not in Wwise's packed library");
        candidates.push_back(it->second);
    }

    // A duplicate takes the id that continues its neighbours' run -- the choice
    // Wwise makes (417 between 430 and 418, 420 before 421, 423 before 424).
    std::vector<int> ids(codebookCount, -1);
    for (uint32_t i = 0; i < codebookCount; ++i)
        if (candidates[i].size() == 1) ids[i] = candidates[i][0];
    for (uint32_t i = 0; i < codebookCount; ++i)
    {
        if (ids[i] >= 0) continue;
        std::vector<int> want;
        for (uint32_t j = i + 1; j < codebookCount; ++j)
            if (ids[j] >= 0) { want.push_back(ids[j] - 1); break; }
        if (i > 0) want.push_back(ids[i - 1] + 1);
        int best = candidates[i][0];
        int bestDistance = std::numeric_limits<int>::max();
        for (const int c : candidates[i])
            for (const int w : want)
                if (std::abs(c - w) < bestDistance) { bestDistance = std::abs(c - w); best = c; }
        ids[i] = best;
    }

    // Pass 2: write.
    WwiseSetup out;
    BitWriter w(out.bytes);
    w.write(codebookCount - 1, 8);
    for (const int id : ids) w.write((uint32_t) id, 10);

    BitReader r(setup, size);
    skip(r, scan.bitsRead());   // continue right after the last codebook

    const uint32_t timeCount = r.read(6) + 1;
    for (uint32_t i = 0; i < timeCount; ++i)
        if (r.read(16) != 0) fail("time domain transform type != 0");

    const uint32_t floorCount = r.read(6) + 1;
    w.write(floorCount - 1, 6);
    for (uint32_t i = 0; i < floorCount; ++i)
    {
        if (r.read(16) != 1) fail("floor type 0 has no Wwise form");
        const uint32_t partitions = r.read(5);
        w.write(partitions, 5);
        std::vector<uint32_t> classList(partitions);
        uint32_t maxClass = 0;
        for (uint32_t j = 0; j < partitions; ++j)
        {
            classList[j] = r.read(4);
            w.write(classList[j], 4);
            maxClass = std::max(maxClass, classList[j]);
        }
        std::vector<uint32_t> classDims(partitions ? maxClass + 1 : 0);
        for (uint32_t j = 0; j < classDims.size(); ++j)
        {
            classDims[j] = r.read(3) + 1;
            w.write(classDims[j] - 1, 3);
            const uint32_t subclasses = r.read(2);
            w.write(subclasses, 2);
            if (subclasses != 0) w.write(r.read(8), 8);   // masterbook
            for (uint32_t k = 0; k < (1u << subclasses); ++k) w.write(r.read(8), 8);
        }
        w.write(r.read(2), 2);   // multiplier - 1
        const uint32_t rangeBits = r.read(4);
        w.write(rangeBits, 4);
        for (uint32_t j = 0; j < partitions; ++j)
            for (uint32_t k = 0; k < classDims[classList[j]]; ++k)
                w.write(r.read((int) rangeBits), (int) rangeBits);
    }

    const uint32_t residueCount = r.read(6) + 1;
    w.write(residueCount - 1, 6);
    for (uint32_t i = 0; i < residueCount; ++i)
    {
        const uint32_t type = r.read(16);
        if (type > 2) fail("residue type > 2");
        w.write(type, 2);
        for (int k = 0; k < 3; ++k) w.write(r.read(24), 24);   // begin, end, partition size
        const uint32_t classifications = r.read(6) + 1;
        w.write(classifications - 1, 6);
        w.write(r.read(8), 8);   // classbook
        std::vector<uint32_t> cascade(classifications);
        for (uint32_t j = 0; j < classifications; ++j)
        {
            const uint32_t low = r.read(3);
            w.write(low, 3);
            const uint32_t flag = r.read(1);
            w.write(flag, 1);
            uint32_t high = 0;
            if (flag) { high = r.read(5); w.write(high, 5); }
            cascade[j] = high * 8 + low;
        }
        for (uint32_t j = 0; j < classifications; ++j)
            for (int k = 0; k < 8; ++k)
                if (cascade[j] & (1u << k)) w.write(r.read(8), 8);
    }

    const uint32_t mappingCount = r.read(6) + 1;
    w.write(mappingCount - 1, 6);
    for (uint32_t i = 0; i < mappingCount; ++i)
    {
        if (r.read(16) != 0) fail("mapping type != 0");
        const uint32_t submapsFlag = r.read(1);
        w.write(submapsFlag, 1);
        uint32_t submaps = 1;
        if (submapsFlag) { const uint32_t s = r.read(4); w.write(s, 4); submaps = s + 1; }
        const uint32_t squarePolar = r.read(1);
        w.write(squarePolar, 1);
        if (squarePolar)
        {
            const uint32_t steps = r.read(8) + 1;
            w.write(steps - 1, 8);
            const int bits = ilog((uint32_t) channels - 1);
            for (uint32_t j = 0; j < steps; ++j) { w.write(r.read(bits), bits); w.write(r.read(bits), bits); }
        }
        const uint32_t reserved = r.read(2);
        if (reserved != 0) fail("mapping reserved field nonzero");
        w.write(reserved, 2);
        if (submaps > 1)
            for (int j = 0; j < channels; ++j) w.write(r.read(4), 4);
        for (uint32_t j = 0; j < submaps; ++j)
            for (int k = 0; k < 3; ++k) w.write(r.read(8), 8);   // time config, floor, residue
    }

    const uint32_t modeCount = r.read(6) + 1;
    w.write(modeCount - 1, 6);
    out.modeBits = ilog(modeCount - 1);
    for (uint32_t i = 0; i < modeCount; ++i)
    {
        const uint32_t blockflag = r.read(1);
        w.write(blockflag, 1);
        out.modeBlockflag.push_back((uint8_t) blockflag);
        if (r.read(16) != 0 || r.read(16) != 0) fail("mode window/transform type != 0");
        w.write(r.read(8), 8);   // mapping
    }
    if (r.read(1) != 1) fail("setup framing bit missing");
    return out;
}

// Standard audio packet -> Wwise "mod" packet: no packet-type bit, and on a long
// block no previous/next window flags (the decoder recomputes them from the
// neighbouring packets). Every other bit is kept, trailing padding included.
void toModPacket(const uint8_t* p, size_t n, const WwiseSetup& s, std::vector<uint8_t>& out, uint8_t& blockflag)
{
    if (n == 0) fail("empty audio packet");
    BitReader r(p, n);
    if (r.read(1) != 0) fail("not an audio packet");
    const uint32_t mode = r.read(s.modeBits);
    if (mode >= s.modeBlockflag.size()) fail("audio packet has an invalid mode");
    blockflag = s.modeBlockflag[mode];
    if (blockflag) r.read(2);

    BitWriter w(out);
    w.write(mode, s.modeBits);
    size_t pos = r.bitsRead();
    const size_t total = n * 8;
    while (pos < total && (pos & 7) != 0) { w.write(r.read(1), 1); ++pos; }
    if (pos < total) w.writeBytes(p + pos / 8, n - pos / 8);
}

// libvorbis encoder state, cleared in reverse order.
struct Encoder
{
    ov::vorbis_info info{};
    ov::vorbis_comment comment{};
    ov::vorbis_dsp_state dsp{};
    ov::vorbis_block block{};
    bool infoInit = false, commentInit = false, dspInit = false, blockInit = false;

    ~Encoder()
    {
        if (blockInit) ov::vorbis_block_clear(&block);
        if (dspInit) ov::vorbis_dsp_clear(&dsp);
        if (commentInit) ov::vorbis_comment_clear(&comment);
        if (infoInit) ov::vorbis_info_clear(&info);
    }
};

struct VorbisStream
{
    std::vector<uint8_t> setup;
    std::vector<std::vector<uint8_t>> audio;
    int64_t granule = 0;
    int blocksize0 = 0, blocksize1 = 0;
};

void drain(Encoder& e, VorbisStream& s)
{
    while (ov::vorbis_analysis_blockout(&e.dsp, &e.block) == 1)
    {
        if (ov::vorbis_analysis(&e.block, nullptr) != 0) fail("vorbis_analysis failed");
        if (ov::vorbis_bitrate_addblock(&e.block) != 0) fail("vorbis_bitrate_addblock failed");
        ogg_packet op{};
        while (ov::vorbis_bitrate_flushpacket(&e.dsp, &op) == 1)
        {
            s.audio.emplace_back(op.packet, op.packet + op.bytes);
            if (op.granulepos >= 0) s.granule = op.granulepos;
        }
    }
}

void encodeStream(const float* interleaved, int64_t frames, int channels, int rate, VorbisStream& s)
{
    Encoder e;
    ov::vorbis_info_init(&e.info);
    e.infoInit = true;
    if (ov::vorbis_encode_setup_vbr(&e.info, channels, rate, kQuality) != 0) fail("vorbis_encode_setup_vbr failed");
    double lowpass = kLowpassKhz;
    if (ov::vorbis_encode_ctl(&e.info, OV_ECTL_LOWPASS_SET, &lowpass) != 0) fail("OV_ECTL_LOWPASS_SET failed");
    if (ov::vorbis_encode_setup_init(&e.info) != 0) fail("vorbis_encode_setup_init failed");
    ov::vorbis_comment_init(&e.comment);
    e.commentInit = true;
    if (ov::vorbis_analysis_init(&e.dsp, &e.info) != 0) fail("vorbis_analysis_init failed");
    e.dspInit = true;
    if (ov::vorbis_block_init(&e.dsp, &e.block) != 0) fail("vorbis_block_init failed");
    e.blockInit = true;

    ogg_packet ident{}, comment{}, setup{};
    if (ov::vorbis_analysis_headerout(&e.dsp, &e.comment, &ident, &comment, &setup) != 0)
        fail("vorbis_analysis_headerout failed");
    s.setup.assign(setup.packet, setup.packet + setup.bytes);
    s.blocksize0 = (int) ov::vorbis_info_blocksize(&e.info, 0);
    s.blocksize1 = (int) ov::vorbis_info_blocksize(&e.info, 1);

    constexpr int64_t chunk = 4096;
    for (int64_t at = 0; at < frames; at += chunk)
    {
        const int64_t n = std::min(chunk, frames - at);
        float** buffer = ov::vorbis_analysis_buffer(&e.dsp, (int) n);
        for (int c = 0; c < channels; ++c)
        {
            float* dst = buffer[c];
            const float* src = interleaved + at * channels + c;
            for (int64_t i = 0; i < n; ++i) dst[i] = src[i * channels];
        }
        if (ov::vorbis_analysis_wrote(&e.dsp, (int) n) != 0) fail("vorbis_analysis_wrote failed");
        drain(e, s);
    }
    if (ov::vorbis_analysis_wrote(&e.dsp, 0) != 0) fail("vorbis_analysis_wrote(end) failed");
    drain(e, s);
}

int log2Exact(int v)
{
    int p = 0;
    while ((1 << p) < v) ++p;
    if ((1 << p) != v) fail("block size is not a power of two");
    return p;
}

WwiseSetup checkedSetup(const VorbisStream& s, int channels)
{
    WwiseSetup ws = toWwiseSetup(s.setup.data(), s.setup.size(), channels);
    if (ws.bytes.size() != kWwiseSetupBytes || fnv1a64(ws.bytes) != kWwiseSetupFnv64)
        fail("the setup header is not the pinned Wwise one (libvorbis changed?); the copied Wwise constants would be wrong");
    return ws;
}

} // namespace

bool encodeVorbisWem(const float* interleaved, int64_t frames, int channels, int sampleRate,
                     std::vector<uint8_t>& out, WemEncodeStats& stats, std::string& error)
{
    out.clear();
    stats = {};
    error.clear();
    if (interleaved == nullptr || frames <= 0) { error = "no audio"; return false; }
    if (channels != kWemEncodeChannels || sampleRate != kWemEncodeSampleRate)
    {
        error = "only 48 kHz stereo can be encoded (the pinned Wwise configuration)";
        return false;
    }

    try
    {
        VorbisStream s;
        encodeStream(interleaved, frames, channels, sampleRate, s);
        const WwiseSetup ws = checkedSetup(s, channels);
        const int bs0 = s.blocksize0, bs1 = s.blocksize1;

        // Packets, and the frames each one decodes to (the decoder's rule).
        std::vector<std::vector<uint8_t>> packets;
        std::vector<uint32_t> packetFrames;
        packets.reserve(s.audio.size());
        bool havePrev = false;
        uint8_t prevFlag = 0;
        int64_t decoded = 0;
        for (const auto& p : s.audio)
        {
            if (p.empty()) continue;
            std::vector<uint8_t> mod;
            uint8_t flag = 0;
            toModPacket(p.data(), p.size(), ws, mod, flag);
            const uint32_t f = havePrev ? (uint32_t) ((prevFlag ? bs1 : bs0) / 4 + (flag ? bs1 : bs0) / 4) : 0;
            havePrev = true;
            prevFlag = flag;
            decoded += f;
            packets.push_back(std::move(mod));
            packetFrames.push_back(f);
        }
        if (packets.empty()) fail("libvorbis produced no audio packets");
        const int64_t samples = s.granule;
        if (samples != frames) fail("the final granule does not match the input length");
        if (decoded < samples) fail("the packets decode to fewer frames than the header claims");

        // data chunk: seek table, setup packet, audio packets. Seek offsets are
        // relative to the setup packet; an entry lands when a packet's END frame
        // is >= kSeekGranularity past the previous entry (26 of 26 Wwise files).
        std::vector<uint8_t> body;
        std::vector<uint8_t> seek;
        int seekEntries = 0;
        putU16(body, (uint32_t) ws.bytes.size());
        body.insert(body.end(), ws.bytes.begin(), ws.bytes.end());
        const uint32_t setupPacketSize = (uint32_t) body.size();
        int64_t posFrames = 0, lastFrames = 0;
        size_t lastOffset = 0;
        size_t maxPacket = 0;
        for (size_t i = 0; i < packets.size(); ++i)
        {
            const int64_t endFrames = posFrames + packetFrames[i];
            if (endFrames - lastFrames >= kSeekGranularity)
            {
                putU16(seek, (uint32_t) (endFrames - lastFrames));
                putU16(seek, (uint32_t) (body.size() - lastOffset));
                ++seekEntries;
                lastFrames = endFrames;
                lastOffset = body.size();
            }
            putU16(body, (uint32_t) packets[i].size());
            body.insert(body.end(), packets[i].begin(), packets[i].end());
            maxPacket = std::max(maxPacket, packets[i].size());
            posFrames = endFrames;
        }

        const uint32_t seekSize = (uint32_t) seek.size();
        const uint32_t dataAfterSeek = (uint32_t) body.size();
        const uint32_t extraFrames = (uint32_t) (decoded - samples);
        const auto avgBytes = (uint32_t) ((double) dataAfterSeek / ((double) samples / sampleRate));

        std::vector<uint8_t> fmt;
        putU16(fmt, 0xFFFF);                        // Wwise codec id
        putU16(fmt, (uint32_t) channels);
        putU32(fmt, (uint32_t) sampleRate);
        putU32(fmt, avgBytes);
        putU16(fmt, 0);                             // block align
        putU16(fmt, 0);                             // bits per sample
        putU16(fmt, 0x30);                          // extra size
        putU16(fmt, 0);
        putU32(fmt, kChannelMask);                  // +0x14 (absolute offset 40)
        putU32(fmt, (uint32_t) samples);            // +0x18 dwTotalPCMFrames
        putU32(fmt, setupPacketSize);               // +0x1C loop start = data start after the seek table
        putU32(fmt, dataAfterSeek);                 // +0x20 loop end = data end
        putU16(fmt, 0);                             // +0x24
        putU16(fmt, extraFrames);                   // +0x26 decoded - header frames
        putU32(fmt, seekSize);                      // +0x28 dwSeekTableSize
        putU32(fmt, seekSize + setupPacketSize);    // +0x2C dwVorbisDataOffset
        putU16(fmt, (uint32_t) maxPacket);          // +0x30 uMaxPacketSize
        putU16(fmt, extraFrames);                   // +0x32 (= +0x26 in every measured file)
        putU32(fmt, kDecodeAllocSize);              // +0x34
        putU32(fmt, kField0x38);                    // +0x38
        putU32(fmt, kCodebookHash);                 // +0x3C
        fmt.push_back((uint8_t) log2Exact(bs0));    // +0x40
        fmt.push_back((uint8_t) log2Exact(bs1));    // +0x41
        if (fmt.size() != 0x42) fail("fmt chunk size");

        const uint64_t dataSize = (uint64_t) seek.size() + body.size();
        const uint64_t riffSize = 4 + 8 + fmt.size() + 8 + dataSize;
        if (riffSize > 0xFFFFFFFFull) fail("the .wem would exceed 4 GiB");
        out.reserve((size_t) riffSize + 8);
        putTag(out, "RIFF");
        putU32(out, (uint32_t) riffSize);
        putTag(out, "WAVE");
        putTag(out, "fmt ");
        putU32(out, (uint32_t) fmt.size());
        out.insert(out.end(), fmt.begin(), fmt.end());
        putTag(out, "data");
        putU32(out, (uint32_t) dataSize);
        out.insert(out.end(), seek.begin(), seek.end());
        out.insert(out.end(), body.begin(), body.end());

        stats.frames = samples;
        stats.decodedFrames = decoded;
        stats.packets = (int) packets.size();
        stats.seekEntries = seekEntries;
        stats.maxPacket = (int) maxPacket;
        stats.setupBytes = (int) ws.bytes.size();
        return true;
    }
    catch (const std::exception& ex)
    {
        out.clear();
        error = ex.what();
        return false;
    }
}

} // namespace dcwem
