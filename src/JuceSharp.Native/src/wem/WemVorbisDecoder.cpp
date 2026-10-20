// Native Wwise RIFF/RIFX Vorbis (.wem) decoder -- see WemVorbisDecoder.h.
//
// Port of ww2ogg's wwriff.cpp / codebook.cpp (Adam Gashlin, hcs64/ww2ogg,
// BSD-style licence). Structure and error checks follow ww2ogg so that a file it
// refuses is refused here too; the output side writes Vorbis header PACKETS for
// libvorbis instead of an Ogg file.

#include "WemVorbisDecoder.h"
#include "WemVorbisCommon.h"

// JUCE compiles its bundled libogg/libvorbis inside juce_audio_formats, with
// ogg.h at global scope and the vorbis headers inside juce::OggVorbisNamespace
// (juce_OggVorbisAudioFormat.cpp). The vorbis functions are declared extern "C"
// there, so declaring them the same way here links against JUCE's copy -- one
// libvorbis in the DLL, the same one the Ogg reader uses.
extern "C"
{
 #include <juce_audio_formats/codecs/ogg/include/ogg/ogg.h>
}

namespace juce::OggVorbisNamespace
{
 #include <juce_audio_formats/codecs/vorbis/include/vorbis/codec.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
 #include <xmmintrin.h>
 #define DCWEM_HAS_MXCSR 1
#else
 #define DCWEM_HAS_MXCSR 0
#endif

namespace dcwem
{
namespace ov = juce::OggVorbisNamespace;

namespace
{

using namespace detail;

// ---- RIFF header ----------------------------------------------------------

enum class ForcePackets { None, Mod, Standard };

struct Header
{
    bool     bigEndian = false;
    size_t   dataOffset = 0, dataSize = 0;
    int      channels = 0;
    uint32_t sampleRate = 0;
    uint32_t avgBytesPerSecond = 0;
    int      vorbSize = -1;
    uint32_t sampleCount = 0;
    uint32_t setupPacketOffset = 0;
    uint32_t firstAudioPacketOffset = 0;
    int      blocksize0Pow = 0, blocksize1Pow = 0;
    bool     noGranule = false;
    bool     modPackets = false;
    bool     headerTriad = false;
    bool     oldPacketHeaders = false;
};

class ByteView
{
public:
    ByteView(const std::vector<uint8_t>& b, bool be) : b_(b), be_(be) {}
    void setBigEndian(bool be) { be_ = be; }

    uint32_t u32(size_t at) const
    {
        if (at + 4 > b_.size() || at + 4 < at) fail("file truncated");
        const uint8_t* p = b_.data() + at;
        return be_ ? ((uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3])
                   : ((uint32_t) p[3] << 24 | (uint32_t) p[2] << 16 | (uint32_t) p[1] << 8 | p[0]);
    }
    uint16_t u16(size_t at) const
    {
        if (at + 2 > b_.size() || at + 2 < at) fail("file truncated");
        const uint8_t* p = b_.data() + at;
        return be_ ? (uint16_t) (p[0] << 8 | p[1]) : (uint16_t) (p[1] << 8 | p[0]);
    }
    uint8_t u8(size_t at) const
    {
        if (at >= b_.size()) fail("file truncated");
        return b_[at];
    }
    bool tag(size_t at, const char* t) const
    {
        return at + 4 <= b_.size() && std::memcmp(b_.data() + at, t, 4) == 0;
    }

private:
    const std::vector<uint8_t>& b_;
    bool be_;
};

Header parseHeader(const std::vector<uint8_t>& bytes, ForcePackets force)
{
    Header h;
    ByteView v(bytes, false);
    if (v.tag(0, "RIFX")) { h.bigEndian = true; v.setBigEndian(true); }
    else if (! v.tag(0, "RIFF")) fail("missing RIFF");
    if (! v.tag(8, "WAVE")) fail("missing WAVE");

    const uint64_t riffSize = (uint64_t) v.u32(4) + 8;
    if (riffSize > bytes.size()) fail("RIFF truncated");

    int64_t fmtOffset = -1, vorbOffset = -1, dataOffset = -1;
    int64_t fmtSize = -1, vorbSize = -1, dataSize = -1;
    uint64_t chunk = 12;
    while (chunk < riffSize)
    {
        if (chunk + 8 > riffSize) fail("chunk header truncated");
        const uint64_t size = v.u32((size_t) chunk + 4);
        const uint64_t body = chunk + 8;
        if (v.tag((size_t) chunk, "fmt ")) { fmtOffset = (int64_t) body; fmtSize = (int64_t) size; }
        else if (v.tag((size_t) chunk, "vorb")) { vorbOffset = (int64_t) body; vorbSize = (int64_t) size; }
        else if (v.tag((size_t) chunk, "data")) { dataOffset = (int64_t) body; dataSize = (int64_t) size; }
        chunk = body + size;
    }
    if (chunk > riffSize) fail("chunk truncated");
    if (fmtOffset < 0 || dataOffset < 0) fail("expected fmt, data chunks");
    if ((uint64_t) dataOffset + (uint64_t) dataSize > bytes.size()) fail("data chunk truncated");

    if (vorbOffset < 0)
    {
        if (fmtSize != 0x42) fail("expected 0x42 fmt if vorb missing");
        vorbOffset = fmtOffset + 0x18;
    }
    else if (fmtSize != 0x28 && fmtSize != 0x18 && fmtSize != 0x12)
    {
        fail("bad fmt size");
    }

    const size_t fo = (size_t) fmtOffset;
    if (v.u16(fo) != 0xFFFF) fail("bad codec id");
    h.channels          = v.u16(fo + 2);
    h.sampleRate        = v.u32(fo + 4);
    h.avgBytesPerSecond = v.u32(fo + 8);
    if (v.u16(fo + 12) != 0) fail("bad block align");
    if (v.u16(fo + 14) != 0) fail("expected 0 bps");
    if (v.u16(fo + 16) != (uint32_t) (fmtSize - 0x12)) fail("bad extra fmt length");
    if (h.channels <= 0 || h.channels > 255) fail("bad channel count");
    if (h.sampleRate == 0) fail("bad sample rate");

    h.vorbSize = (int) vorbSize;
    switch (vorbSize)
    {
        case -1: case 0x28: case 0x2A: case 0x2C: case 0x32: case 0x34: break;
        default: fail("bad vorb size");
    }

    const size_t vo = (size_t) vorbOffset;
    h.sampleCount = v.u32(vo);
    size_t at = 0;
    if (vorbSize == -1 || vorbSize == 0x2A)
    {
        h.noGranule = true;
        const uint32_t modSignal = v.u32(vo + 4);
        // ww2ogg: 0x4A, 0x4B, 0x69, 0x70 mean "no mod packets"; anything else
        // (seen: 0xD9, 0xCB, 0xBC, 0xB2) means mod packets.
        if (modSignal != 0x4A && modSignal != 0x4B && modSignal != 0x69 && modSignal != 0x70)
            h.modPackets = true;
        at = vo + 0x10;
    }
    else
    {
        at = vo + 0x18;
    }
    if (force == ForcePackets::Standard) h.modPackets = false;
    else if (force == ForcePackets::Mod) h.modPackets = true;

    h.setupPacketOffset      = v.u32(at);
    h.firstAudioPacketOffset = v.u32(at + 4);

    if (vorbSize == 0x28 || vorbSize == 0x2C)
    {
        h.headerTriad = true;
        h.oldPacketHeaders = true;
    }
    else
    {
        const size_t bo = (vorbSize == -1 || vorbSize == 0x2A) ? vo + 0x24 : vo + 0x2C;
        // +0 is the uid
        h.blocksize0Pow = v.u8(bo + 4);
        h.blocksize1Pow = v.u8(bo + 5);
    }

    h.dataOffset = (size_t) dataOffset;
    h.dataSize   = (size_t) dataSize;
    return h;
}

struct RawPacket
{
    size_t   offset;       // packet header
    size_t   headerSize;
    uint32_t size;
    uint32_t granule;
    size_t payload() const noexcept { return offset + headerSize; }
    size_t next() const noexcept { return offset + headerSize + size; }
};

RawPacket readPacket(const ByteView& v, const Header& h, size_t offset)
{
    RawPacket p{};
    p.offset = offset;
    if (h.oldPacketHeaders)
    {
        p.size = v.u32(offset);
        p.granule = v.u32(offset + 4);
        p.headerSize = 8;
    }
    else
    {
        p.size = v.u16(offset);
        p.granule = h.noGranule ? 0 : v.u32(offset + 2);
        p.headerSize = h.noGranule ? 2 : 6;
    }
    return p;
}

// ww2ogg generate_ogg_header (no triad): identification, comment, setup.
struct Headers
{
    std::vector<uint8_t> ident, comment, setup;
    std::vector<uint8_t> modeBlockflag;
    int modeBits = 0;
};

void buildIdentAndComment(const Header& h, Headers& out)
{
    BitWriter w(out.ident);
    w.writeHeader(1);
    w.write(0, 32);                              // version
    w.write((uint32_t) h.channels, 8);
    w.write(h.sampleRate, 32);
    w.write(0, 32);                              // bitrate max
    w.write(h.avgBytesPerSecond * 8u, 32);       // nominal
    w.write(0, 32);                              // min
    w.write((uint32_t) h.blocksize0Pow, 4);
    w.write((uint32_t) h.blocksize1Pow, 4);
    w.write(1, 1);                               // framing

    BitWriter c(out.comment);
    c.writeHeader(3);
    static const char vendor[] = "JuceSharp native WEM decoder (ww2ogg header rebuild)";
    const uint32_t len = (uint32_t) (sizeof(vendor) - 1);
    c.write(len, 32);
    for (uint32_t i = 0; i < len; ++i) c.write((uint8_t) vendor[i], 8);
    c.write(0, 32);                              // no user comments
    c.write(1, 1);
}

void buildSetup(const std::vector<uint8_t>& bytes, const Header& h, bool inlineCodebooks,
                const CodebookLibrary* lib, Headers& out)
{
    ByteView v(bytes, h.bigEndian);
    const size_t dataEnd = h.dataOffset + h.dataSize;
    const RawPacket sp = readPacket(v, h, h.dataOffset + h.setupPacketOffset);
    if (sp.granule != 0) fail("setup packet granule != 0");
    if (sp.payload() > dataEnd) fail("setup packet truncated");

    BitReader r(bytes.data() + sp.payload(), dataEnd - sp.payload());
    BitWriter w(out.setup);
    w.writeHeader(5);

    const uint32_t codebookCount = r.read(8) + 1;
    w.write(codebookCount - 1, 8);
    for (uint32_t i = 0; i < codebookCount; ++i)
    {
        if (inlineCodebooks)
            rebuildCodebook(r, 0, w);
        else
            lib->rebuild((int) r.read(10), w);
    }

    w.write(0, 6);    // time count - 1
    w.write(0, 16);   // dummy time value

    const uint32_t floorCount = r.read(6) + 1;
    w.write(floorCount - 1, 6);
    for (uint32_t i = 0; i < floorCount; ++i)
    {
        w.write(1, 16);   // floor type 1
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
        std::vector<uint32_t> classDims(maxClass + 1);
        for (uint32_t j = 0; j <= maxClass; ++j)
        {
            classDims[j] = r.read(3) + 1;
            w.write(classDims[j] - 1, 3);
            const uint32_t subclasses = r.read(2);
            w.write(subclasses, 2);
            if (subclasses != 0)
            {
                const uint32_t master = r.read(8);
                w.write(master, 8);
                if (master >= codebookCount) fail("invalid floor1 masterbook");
            }
            for (uint32_t k = 0; k < (1u << subclasses); ++k)
            {
                const uint32_t bookPlus1 = r.read(8);
                w.write(bookPlus1, 8);
                if (bookPlus1 >= 1 && bookPlus1 - 1 >= codebookCount) fail("invalid floor1 subclass book");
            }
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
        const uint32_t type = r.read(2);
        w.write(type, 16);
        if (type > 2) fail("invalid residue type");
        const uint32_t begin = r.read(24), end = r.read(24), partSize = r.read(24);
        const uint32_t classifications = r.read(6) + 1;
        const uint32_t classbook = r.read(8);
        w.write(begin, 24); w.write(end, 24); w.write(partSize, 24);
        w.write(classifications - 1, 6); w.write(classbook, 8);
        if (classbook >= codebookCount) fail("invalid residue classbook");

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
                if (cascade[j] & (1u << k))
                {
                    const uint32_t book = r.read(8);
                    w.write(book, 8);
                    if (book >= codebookCount) fail("invalid residue book");
                }
    }

    const uint32_t mappingCount = r.read(6) + 1;
    w.write(mappingCount - 1, 6);
    for (uint32_t i = 0; i < mappingCount; ++i)
    {
        w.write(0, 16);   // mapping type 0
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
            const int bits = ilog((uint32_t) h.channels - 1);
            for (uint32_t j = 0; j < steps; ++j)
            {
                const uint32_t magnitude = r.read(bits), angle = r.read(bits);
                w.write(magnitude, bits); w.write(angle, bits);
                if (angle == magnitude || magnitude >= (uint32_t) h.channels || angle >= (uint32_t) h.channels)
                    fail("invalid coupling");
            }
        }

        const uint32_t reserved = r.read(2);
        w.write(reserved, 2);
        if (reserved != 0) fail("mapping reserved field nonzero");

        if (submaps > 1)
            for (int j = 0; j < h.channels; ++j)
            {
                const uint32_t mux = r.read(4);
                w.write(mux, 4);
                if (mux >= submaps) fail("mapping_mux >= submaps");
            }

        for (uint32_t j = 0; j < submaps; ++j)
        {
            w.write(r.read(8), 8);   // time config
            const uint32_t floorNumber = r.read(8);
            w.write(floorNumber, 8);
            if (floorNumber >= floorCount) fail("invalid floor mapping");
            const uint32_t residueNumber = r.read(8);
            w.write(residueNumber, 8);
            if (residueNumber >= residueCount) fail("invalid residue mapping");
        }
    }

    const uint32_t modeCount = r.read(6) + 1;
    w.write(modeCount - 1, 6);
    out.modeBlockflag.assign(modeCount, 0);
    out.modeBits = ilog(modeCount - 1);
    for (uint32_t i = 0; i < modeCount; ++i)
    {
        const uint32_t blockflag = r.read(1);
        w.write(blockflag, 1);
        out.modeBlockflag[i] = (uint8_t) blockflag;
        w.write(0, 16);   // window type
        w.write(0, 16);   // transform type
        const uint32_t mapping = r.read(8);
        w.write(mapping, 8);
        if (mapping >= mappingCount) fail("invalid mode mapping");
    }
    w.write(1, 1);   // framing

    if ((r.bitsRead() + 7) / 8 != sp.size) fail("didn't read exactly setup packet");
    if (sp.next() != h.dataOffset + h.firstAudioPacketOffset) fail("first audio packet doesn't follow setup packet");
}

// Old Wwise: the three real header packets sit in the data chunk (8-byte packet
// headers). ww2ogg copies them bit for bit, which is what this does.
void readTriad(const std::vector<uint8_t>& bytes, const Header& h, Headers& out)
{
    ByteView v(bytes, h.bigEndian);
    const size_t dataEnd = h.dataOffset + h.dataSize;
    size_t offset = h.dataOffset + h.setupPacketOffset;
    std::vector<uint8_t>* dest[] = { &out.ident, &out.comment, &out.setup };
    const int types[] = { 1, 3, 5 };
    for (int i = 0; i < 3; ++i)
    {
        const RawPacket p = readPacket(v, h, offset);
        if (p.next() > dataEnd) fail("header packet truncated");
        if (p.size == 0 || bytes[p.payload()] != types[i]) fail("wrong type for header packet");
        dest[i]->assign(bytes.begin() + (std::ptrdiff_t) p.payload(), bytes.begin() + (std::ptrdiff_t) p.next());
        offset = p.next();
    }
}

} // namespace

// ---- libvorbis state -------------------------------------------------------

struct WemVorbisDecoder::Vorbis
{
    ov::vorbis_info    info{};
    ov::vorbis_comment comment{};
    ov::vorbis_dsp_state dsp{};
    ov::vorbis_block   block{};
    bool dspReady = false;
    bool blockReady = false;

    Vorbis()
    {
        ov::vorbis_info_init(&info);
        ov::vorbis_comment_init(&comment);
    }

    ~Vorbis()
    {
        if (blockReady) ov::vorbis_block_clear(&block);
        if (dspReady) ov::vorbis_dsp_clear(&dsp);
        ov::vorbis_comment_clear(&comment);
        ov::vorbis_info_clear(&info);
    }

    bool headerIn(std::vector<uint8_t>& packet, int index)
    {
        ogg_packet op{};
        op.packet = packet.data();
        op.bytes = (long) packet.size();
        op.b_o_s = index == 0 ? 1 : 0;
        op.granulepos = 0;
        op.packetno = index;
        return ov::vorbis_synthesis_headerin(&info, &comment, &op) == 0;
    }

    bool start()
    {
        if (ov::vorbis_synthesis_init(&dsp, &info) != 0) return false;
        dspReady = true;
        if (ov::vorbis_block_init(&dsp, &block) != 0) return false;
        blockReady = true;
        return true;
    }
};

// ---- parallel decode state (#434) -------------------------------------------

// One worker's libvorbis synthesis state. It shares the decoder's vorbis_info:
// after the decoder's own vorbis_synthesis_init built the codebooks
// (ci->fullbooks), libvorbis only READS the info during init and synthesis.
// Lanes are created on the caller's thread, never on a worker.
struct WemVorbisDecoder::Lane
{
    ov::vorbis_dsp_state dsp{};
    ov::vorbis_block block{};
    bool dspReady = false;
    bool blockReady = false;
    std::vector<uint8_t> scratch;

    ~Lane()
    {
        if (blockReady) ov::vorbis_block_clear(&block);
        if (dspReady) ov::vorbis_dsp_clear(&dsp);
    }

    bool start(ov::vorbis_info& info)
    {
        if (ov::vorbis_synthesis_init(&dsp, &info) != 0) return false;
        dspReady = true;
        if (ov::vorbis_block_init(&dsp, &block) != 0) return false;
        blockReady = true;
        return true;
    }
};

struct WemVorbisDecoder::Segment
{
    static constexpr size_t kNone = SIZE_MAX;
    size_t  first = 0;               // packets [first, end)
    size_t  end = 0;
    int64_t capacity = 0;            // frames the packet table says the range produces
    int64_t frames = 0;              // frames decoded (== capacity unless it failed)
    size_t  failedAt = kNone;        // first packet it could not reproduce
    std::vector<float> pcm;          // planar: channel c at c * capacity
};

struct WemVorbisDecoder::Batch
{
    std::vector<Segment> segments;
    std::vector<std::thread> workers;
    size_t end = 0;                  // first packet after the batch
    bool   started = false;
};

WemVorbisDecoder::WemVorbisDecoder() = default;

WemVorbisDecoder::~WemVorbisDecoder()
{
    // A batch decoding ahead must finish before its lanes and buffers go.
    discardBatches();
    lanes_.clear();
}

std::unique_ptr<WemVorbisDecoder> WemVorbisDecoder::open(std::shared_ptr<const std::vector<uint8_t>> bytes,
                                                         Variant variant, std::string& error)
{
    error.clear();
    if (! bytes || bytes->size() < 12) { error = "not a WEM file (too short)"; return nullptr; }
    if (bytes->size() > kMaxWemBytes) { error = "WEM file larger than the 256 MiB cap"; return nullptr; }

    struct Attempt { Variant variant; ForcePackets force; };
    std::vector<Attempt> attempts;
    switch (variant)
    {
        case Variant::Auto:
            attempts = { { Variant::AoTuV, ForcePackets::None }, { Variant::Default, ForcePackets::None },
                         { Variant::Inline, ForcePackets::None }, { Variant::DefaultMod, ForcePackets::Mod },
                         { Variant::DefaultStandard, ForcePackets::Standard } };
            break;
        case Variant::AoTuV:           attempts = { { variant, ForcePackets::None } }; break;
        case Variant::Default:         attempts = { { variant, ForcePackets::None } }; break;
        case Variant::Inline:          attempts = { { variant, ForcePackets::None } }; break;
        case Variant::DefaultMod:      attempts = { { variant, ForcePackets::Mod } }; break;
        case Variant::DefaultStandard: attempts = { { variant, ForcePackets::Standard } }; break;
        case Variant::HeaderTriad:     attempts = { { variant, ForcePackets::None } }; break;
        default: error = "unknown WEM variant"; return nullptr;
    }

    std::string lastError;
    for (const auto& attempt : attempts)
    {
        try
        {
            const Header h = parseHeader(*bytes, attempt.force);
            // A header-triad file carries its own headers; every codebook variant
            // decodes it the same way (ww2ogg ignores the codebook choice there).
            if (variant == Variant::HeaderTriad && ! h.headerTriad) fail("not a header-triad WEM");

            Headers hdr;
            if (h.headerTriad)
            {
                readTriad(*bytes, h, hdr);
            }
            else
            {
                buildIdentAndComment(h, hdr);
                const bool inl = attempt.variant == Variant::Inline;
                const CodebookLibrary* lib = inl ? nullptr : &library(attempt.variant == Variant::AoTuV);
                buildSetup(*bytes, h, inl, lib, hdr);
            }

            auto dec = std::unique_ptr<WemVorbisDecoder>(new WemVorbisDecoder());
            dec->vorbis_ = std::make_unique<Vorbis>();
            if (! dec->vorbis_->headerIn(hdr.ident, 0)) fail("libvorbis rejected the identification header");
            if (! dec->vorbis_->headerIn(hdr.comment, 1)) fail("libvorbis rejected the comment header");
            if (! dec->vorbis_->headerIn(hdr.setup, 2)) fail("libvorbis rejected the rebuilt setup header");
            if (dec->vorbis_->info.channels != h.channels && ! h.headerTriad) fail("channel count mismatch");
            if (! dec->vorbis_->start()) fail("libvorbis synthesis init failed");

            dec->bytes_ = bytes;
            dec->modeBlockflag_ = hdr.modeBlockflag;
            dec->modeBits_ = hdr.modeBits;
            dec->info_.channels = dec->vorbis_->info.channels;
            dec->info_.sampleRate = (int) dec->vorbis_->info.rate;
            dec->info_.headerSampleCount = h.sampleCount;
            dec->info_.variant = h.headerTriad ? Variant::HeaderTriad : attempt.variant;
            dec->info_.modPackets = h.modPackets && ! h.headerTriad;
            dec->info_.granulePresent = ! h.noGranule;
            dec->info_.bigEndian = h.bigEndian;

            // ---- packet table (ww2ogg's audio loop, without the copying) ----
            ByteView v(*bytes, h.bigEndian);
            const size_t dataEnd = h.dataOffset + h.dataSize;
            const int64_t bs0 = ov::vorbis_info_blocksize(&dec->vorbis_->info, 0);
            const int64_t bs1 = ov::vorbis_info_blocksize(&dec->vorbis_->info, 1);
            std::vector<RawPacket> raw;
            size_t offset = h.dataOffset + h.firstAudioPacketOffset;
            while (offset < dataEnd)
            {
                const RawPacket p = readPacket(v, h, offset);
                if (offset + p.headerSize > dataEnd) fail("page header truncated");
                if (p.next() > dataEnd) fail("page truncated");
                raw.push_back(p);
                offset = p.next();
            }

            const bool mod = dec->info_.modPackets;
            const int modeBits = dec->modeBits_;
            auto modeOf = [&](const RawPacket& p) -> int {
                if (p.size == 0) return -1;
                const uint8_t first = (*bytes)[p.payload()];
                int mode;
                if (mod) mode = first & ((1 << modeBits) - 1);
                else
                {
                    if (first & 1) return -2;   // not an audio packet
                    // A mode number may straddle the first byte when modeBits == 8;
                    // read it with the bit reader.
                    BitReader br(bytes->data() + p.payload(), p.size);
                    br.read(1);
                    mode = (int) br.read(modeBits);
                }
                return mode;
            };

            dec->packets_.reserve(raw.size());
            bool prevFlag = false;
            bool havePrev = false;
            int prevBlockflagForCount = 0;
            int64_t out = 0;
            for (size_t i = 0; i < raw.size(); ++i)
            {
                const RawPacket& p = raw[i];
                if (p.size == 0) continue;   // libvorbis would reject it as not-audio; it yields nothing
                Packet pk{};
                pk.payloadOffset = (uint32_t) p.payload();
                pk.size = p.size;
                pk.granule = p.granule;
                int blockflag;
                if (h.headerTriad)
                {
                    ogg_packet op{};
                    op.packet = const_cast<unsigned char*>(bytes->data() + p.payload());
                    op.bytes = (long) p.size;
                    const long bs = ov::vorbis_packet_blocksize(&dec->vorbis_->info, &op);
                    if (bs <= 0) fail("audio packet has no valid mode");
                    blockflag = bs == bs1 && bs1 != bs0 ? 1 : 0;
                    pk.mode = 0;
                }
                else
                {
                    const int mode = modeOf(p);
                    if (mode < 0 || (size_t) mode >= dec->modeBlockflag_.size()) fail("audio packet has an invalid mode");
                    pk.mode = (uint8_t) mode;
                    blockflag = dec->modeBlockflag_[(size_t) mode];
                }
                pk.blockflag = (uint8_t) blockflag;
                pk.prevBlockflag = (uint8_t) (prevFlag ? 1 : 0);
                prevFlag = blockflag != 0;

                // ww2ogg peeks at the very next packet in the file (header size of
                // THIS packet's format) and reads its mode bits if it has a payload.
                pk.nextBlockflag = 0;
                if (mod && blockflag && i + 1 < raw.size())
                {
                    const RawPacket& n = raw[i + 1];
                    if (n.size > 0)
                    {
                        const int nm = (*bytes)[n.payload()] & ((1 << modeBits) - 1);
                        if ((size_t) nm < dec->modeBlockflag_.size())
                            pk.nextBlockflag = dec->modeBlockflag_[(size_t) nm];
                    }
                }

                pk.outStart = out;
                pk.outCount = havePrev ? ((prevBlockflagForCount ? bs1 : bs0) / 4 + (blockflag ? bs1 : bs0) / 4) : 0;
                out += pk.outCount;
                havePrev = true;
                prevBlockflagForCount = blockflag;
                dec->packets_.push_back(pk);
            }

            dec->info_.packetCount = (int) dec->packets_.size();
            dec->info_.decodedFrames = out;
            int64_t endGranule = out;
            if (h.noGranule)
                endGranule = h.sampleCount;
            else if (! dec->packets_.empty() && dec->packets_.back().granule != 0xFFFFFFFFu)
                endGranule = dec->packets_.back().granule;
            dec->info_.frames = std::min<int64_t>(out, endGranule);
            if (dec->info_.frames <= 0) fail("WEM decodes to no audio");
            return dec;
        }
        catch (const std::exception& ex)
        {
            lastError = ex.what();
        }
    }

    error = lastError.empty() ? "no WEM variant decoded" : lastError;
    return nullptr;
}

void WemVorbisDecoder::rebuildAudioPacket(const Packet& p, std::vector<uint8_t>& out) const
{
    const uint8_t* src = bytes_->data() + p.payloadOffset;
    if (! info_.modPackets)
    {
        out.assign(src, src + p.size);
        return;
    }

    BitWriter w(out);
    w.write(0, 1);                                   // packet type: audio
    w.write(p.mode, modeBits_);
    if (p.blockflag)
    {
        w.write(p.prevBlockflag, 1);
        w.write(p.nextBlockflag, 1);
    }
    w.write((uint32_t) (src[0] >> modeBits_), 8 - modeBits_);   // rest of the first byte
    w.writeBytes(src + 1, p.size - 1);
}

bool WemVorbisDecoder::decodePacketAt(size_t index, bool keepOutput)
{
    rebuildAudioPacket(packets_[index], scratch_);
    ogg_packet op{};
    op.packet = scratch_.data();
    op.bytes = (long) scratch_.size();
    op.granulepos = -1;   // the length clip is ours (info_.frames); libvorbis must not trim
    op.packetno = (ogg_int64_t) index + 3;
    if (ov::vorbis_synthesis(&vorbis_->block, &op) != 0) return false;
    if (ov::vorbis_synthesis_blockin(&vorbis_->dsp, &vorbis_->block) != 0) return false;
    pendingFrames_ = ov::vorbis_synthesis_pcmout(&vorbis_->dsp, nullptr);
    if (! keepOutput && pendingFrames_ > 0)
    {
        ov::vorbis_synthesis_read(&vorbis_->dsp, (int) pendingFrames_);
        pendingFrames_ = 0;
    }
    return true;
}

int64_t WemVorbisDecoder::read(float* const* dest, int numChannels, int64_t frames)
{
    if (frames <= 0 || numChannels <= 0) return 0;
    int64_t written = 0;
    while (written < frames && position_ < info_.frames)
    {
        // Frames a parallel batch already decoded come first -- also after a
        // segment failure switched the rest of the file to the serial path.
        if (ready_ != nullptr && readySegment_ < ready_->segments.size())
        {
            const Segment& s = ready_->segments[readySegment_];
            const int64_t avail = s.frames - readyOffset_;
            if (avail <= 0) { ++readySegment_; readyOffset_ = 0; continue; }
            if (pendingSkip_ > 0)
            {
                const int64_t n = std::min(avail, pendingSkip_);
                readyOffset_ += n;
                pendingSkip_ -= n;
                continue;
            }
            const int64_t n = std::min({ avail, frames - written, info_.frames - position_ });
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* d = dest[ch];
                if (d == nullptr) continue;
                if (ch < info_.channels)
                    std::memcpy(d + written, s.pcm.data() + (size_t) ch * (size_t) s.capacity + (size_t) readyOffset_,
                                sizeof(float) * (size_t) n);
                else
                    std::memset(d + written, 0, sizeof(float) * (size_t) n);
            }
            readyOffset_ += n;
            written += n;
            position_ += n;
            continue;
        }

        if (parallel_)
        {
            if (! advanceBatch()) break;
            continue;
        }

        if (pendingFrames_ == 0)
        {
            if (nextPacket_ >= packets_.size()) break;
            if (! decodePacketAt(nextPacket_++, true)) return written > 0 ? written : -1;
            continue;
        }

        float** pcm = nullptr;
        const int avail = ov::vorbis_synthesis_pcmout(&vorbis_->dsp, &pcm);
        if (avail <= 0) { pendingFrames_ = 0; continue; }

        if (pendingSkip_ > 0)
        {
            const int n = (int) std::min<int64_t>(avail, pendingSkip_);
            ov::vorbis_synthesis_read(&vorbis_->dsp, n);
            pendingSkip_ -= n;
            pendingFrames_ = avail - n;
            continue;
        }

        const int n = (int) std::min<int64_t>({ (int64_t) avail, frames - written, info_.frames - position_ });
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* d = dest[ch];
            if (d == nullptr) continue;
            if (ch < info_.channels) std::memcpy(d + written, pcm[ch], sizeof(float) * (size_t) n);
            else std::memset(d + written, 0, sizeof(float) * (size_t) n);
        }
        ov::vorbis_synthesis_read(&vorbis_->dsp, n);
        pendingFrames_ = avail - n;
        written += n;
        position_ += n;
    }
    return written;
}

bool WemVorbisDecoder::seek(int64_t frame)
{
    frame = std::clamp<int64_t>(frame, 0, info_.frames);
    if (frame == position_ && pendingSkip_ == 0) return true;

    discardBatches();
    ov::vorbis_synthesis_restart(&vorbis_->dsp);
    pendingFrames_ = 0;
    pendingSkip_ = 0;

    if (frame >= info_.frames || packets_.size() < 2)
    {
        nextPacket_ = packets_.size();
        position_ = frame;
        return true;
    }

    // The packet whose output contains `frame` (outStart is non-decreasing;
    // packet 0 produces nothing).
    auto it = std::upper_bound(packets_.begin() + 1, packets_.end(), frame,
                               [](int64_t f, const Packet& p) { return f < p.outStart; });
    size_t k = (size_t) (it - packets_.begin()) - 1;
    if (k < 1) k = 1;
    // Parallel mode: the next batch's first segment primes packet k - 1 itself.
    // Serial: skip packets that produce nothing at this position (cannot happen
    // for k >= 1, but keep the walk honest).
    if (! parallel_ && ! decodePacketAt(k - 1, false)) return false;
    nextPacket_ = k;
    pendingSkip_ = frame - packets_[k].outStart;
    position_ = frame;
    return true;
}

// ---- parallel decode (#434) ---------------------------------------------------

int WemVorbisDecoder::defaultDecodeThreads() noexcept
{
    const unsigned hw = std::thread::hardware_concurrency();
    return (int) std::clamp<unsigned>(hw / 2, 1u, 4u);
}

void WemVorbisDecoder::enableParallelDecode(int threads, int lanePackets)
{
    threads_ = std::clamp(threads, 1, 16);
    lanePackets_ = std::clamp(lanePackets, 2, 65536);
    parallel_ = threads_ >= 2
             && packets_.size() >= (size_t) lanePackets_ * 2
             && position_ == 0 && nextPacket_ == 0 && pendingFrames_ == 0;
    if (! parallel_) return;
    ready_ = std::make_unique<Batch>();
    next_ = std::make_unique<Batch>();
}

int64_t WemVorbisDecoder::outStartAt(size_t index) const noexcept
{
    return index < packets_.size() ? packets_[index].outStart : info_.decodedFrames;
}

bool WemVorbisDecoder::lanePacket(Lane& lane, size_t index) const
{
    rebuildAudioPacket(packets_[index], lane.scratch);
    ogg_packet op{};
    op.packet = lane.scratch.data();
    op.bytes = (long) lane.scratch.size();
    op.granulepos = -1;   // as decodePacketAt: the length clip is ours
    op.packetno = (ogg_int64_t) index + 3;
    if (ov::vorbis_synthesis(&lane.block, &op) != 0) return false;
    return ov::vorbis_synthesis_blockin(&lane.dsp, &lane.block) == 0;
}

// Runs on a worker (or the caller). Touches only `lane`, `seg` and read-only
// decoder state (bytes_, packets_, the shared vorbis_info).
void WemVorbisDecoder::decodeSegment(Lane& lane, Segment& seg) const noexcept
{
    size_t at = seg.first;
    try
    {
        ov::vorbis_synthesis_restart(&lane.dsp);
        if (seg.first > 0)
        {
            // Prime the overlap with the previous packet; its own output (the
            // overlap with ITS predecessor) belongs to the previous segment.
            if (! lanePacket(lane, seg.first - 1)) { seg.failedAt = seg.first; return; }
            const int discard = ov::vorbis_synthesis_pcmout(&lane.dsp, nullptr);
            if (discard > 0) ov::vorbis_synthesis_read(&lane.dsp, discard);
        }
        const int channels = info_.channels;
        for (; at < seg.end; ++at)
        {
            if (! lanePacket(lane, at)) { seg.failedAt = at; return; }
            float** pcm = nullptr;
            const int avail = ov::vorbis_synthesis_pcmout(&lane.dsp, &pcm);
            // The packet table predicts every packet's frame count; a packet
            // that disagrees is left to the serial path.
            if (avail < 0 || avail != packets_[at].outCount || seg.frames + avail > seg.capacity)
            {
                seg.failedAt = at;
                return;
            }
            for (int ch = 0; avail > 0 && ch < channels; ++ch)
                std::memcpy(seg.pcm.data() + (size_t) ch * (size_t) seg.capacity + (size_t) seg.frames,
                            pcm[ch], sizeof(float) * (size_t) avail);
            if (avail > 0) ov::vorbis_synthesis_read(&lane.dsp, avail);
            seg.frames += avail;
        }
    }
    catch (...)
    {
        seg.failedAt = at;
    }
}

void WemVorbisDecoder::startBatch(Batch& b, size_t first, bool async)
{
    joinBatch(b);
    const size_t total = packets_.size();
    size_t end = std::min(total, first + (size_t) threads_ * (size_t) lanePackets_);
    // The packet table is the file's own claim; keep a batch's PCM bounded
    // whatever it says (one packet is at most 4096 frames, far below the cap).
    const auto batchBytes = [&](size_t e) {
        return (size_t) (outStartAt(e) - outStartAt(first)) * (size_t) info_.channels * sizeof(float);
    };
    while (end > first + 1 && batchBytes(end) > kMaxBatchPcmBytes)
        end = first + (end - first) / 2;
    const size_t count = end - first;
    // A short tail uses fewer segments rather than slivers of a few packets.
    const size_t minSegment = std::max<size_t>(1, (size_t) lanePackets_ / 4);
    const size_t n = std::clamp<size_t>((count + minSegment - 1) / minSegment, 1, (size_t) threads_);

    while (lanes_.size() < n)
    {
        auto lane = std::make_unique<Lane>();
        if (! lane->start(vorbis_->info)) throw std::runtime_error("libvorbis synthesis init failed for a decode lane");
        lanes_.push_back(std::move(lane));
    }

    b.segments.resize(n);
    const size_t base = count / n;
    const size_t extra = count % n;
    size_t at = first;
    for (size_t i = 0; i < n; ++i)
    {
        Segment& s = b.segments[i];
        s.first = at;
        s.end = at + base + (i < extra ? 1 : 0);
        at = s.end;
        s.capacity = outStartAt(s.end) - outStartAt(s.first);
        s.frames = 0;
        s.failedAt = Segment::kNone;
        s.pcm.resize((size_t) info_.channels * (size_t) s.capacity);
    }
    b.end = end;
    b.started = true;
    ++batches_;
    maxSegments_ = std::max(maxSegments_, (int) n);

#if DCWEM_HAS_MXCSR
    const unsigned csr = _mm_getcsr();
#endif
    // Synchronous: segment 0 runs on the caller, which would only wait anyway.
    const size_t firstThreaded = async ? 0 : 1;
    for (size_t i = firstThreaded; i < n; ++i)
    {
        Lane* lane = lanes_[i].get();
        Segment* seg = &b.segments[i];
        try
        {
            b.workers.emplace_back([this, lane, seg
#if DCWEM_HAS_MXCSR
                                    , csr
#endif
                                   ] {
#if DCWEM_HAS_MXCSR
                _mm_setcsr(csr);
#endif
                decodeSegment(*lane, *seg);
            });
        }
        catch (...)
        {
            decodeSegment(*lane, *seg);   // no thread to be had: decode it here
        }
    }
    if (! async)
    {
        decodeSegment(*lanes_[0], b.segments[0]);
        joinBatch(b);
    }
}

void WemVorbisDecoder::joinBatch(Batch& b) noexcept
{
    for (auto& t : b.workers)
        if (t.joinable()) t.join();
    b.workers.clear();
}

// Makes the decoded-ahead batch the ready one and starts the batch after it.
// False at the end of the packets.
bool WemVorbisDecoder::advanceBatch()
{
    if (! next_->started)
    {
        if (nextPacket_ >= packets_.size()) return false;
        startBatch(*next_, nextPacket_, false);
    }
    joinBatch(*next_);
    std::swap(ready_, next_);
    next_->started = false;
    readySegment_ = 0;
    readyOffset_ = 0;

    size_t failedAt = Segment::kNone;
    for (size_t i = 0; i < ready_->segments.size(); ++i)
    {
        if (ready_->segments[i].failedAt == Segment::kNone) continue;
        failedAt = ready_->segments[i].failedAt;
        ready_->segments.resize(i + 1);   // keep what it decoded before the failure
        break;
    }

    if (failedAt != Segment::kNone)
    {
        // Serial from the failing packet on, primed exactly as seek() primes, so
        // the packet fails (or decodes) here just as in a serial decode.
        parallel_ = false;
        ov::vorbis_synthesis_restart(&vorbis_->dsp);
        pendingFrames_ = 0;
        if (failedAt > 0) decodePacketAt(failedAt - 1, false);
        pendingFrames_ = 0;
        nextPacket_ = failedAt;
        return true;
    }

    nextPacket_ = ready_->end;
    if (nextPacket_ < packets_.size()) startBatch(*next_, nextPacket_, true);
    return true;
}

void WemVorbisDecoder::discardBatches() noexcept
{
    if (next_) { joinBatch(*next_); next_->started = false; next_->segments.clear(); }
    if (ready_) { joinBatch(*ready_); ready_->started = false; ready_->segments.clear(); }
    readySegment_ = 0;
    readyOffset_ = 0;
}

int libraryCodebookCount(bool aoTuV) { return library(aoTuV).count() - 1; }

bool rebuildLibraryCodebookForTest(bool aoTuV, int id, std::vector<uint8_t>& out, std::string& error)
{
    try
    {
        BitWriter w(out);
        library(aoTuV).rebuild(id, w);
        return true;
    }
    catch (const std::exception& ex)
    {
        error = ex.what();
        return false;
    }
}

} // namespace dcwem
