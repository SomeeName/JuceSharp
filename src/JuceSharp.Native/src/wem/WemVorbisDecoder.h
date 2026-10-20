// Native Wwise RIFF/RIFX Vorbis (.wem) decoder (ABI v42, DotCharter #426).
//
// A .wem is Vorbis with the Ogg framing, the identification and comment headers
// and (usually) the codebooks stripped or packed. ww2ogg rebuilds a real Ogg
// file from it; this rebuilds only the three Vorbis HEADER packets in memory and
// feeds every audio packet straight to libvorbis's vorbis_synthesis (JUCE's
// bundled libvorbis 1.3.7) -- no Ogg container, no temporary file.
//
// The parsing and header rebuild are a port of ww2ogg's wwriff.cpp and
// codebook.cpp (Adam Gashlin, https://github.com/hcs64/ww2ogg, BSD-style
// licence -- see wem_codebooks.h for the codebook data's origin). What it
// reproduces ON PURPOSE, because the managed path (Ww2Ogg.Core + NVorbis) is the
// reference the output is compared against:
//   * the codebook variants DotCharter tries, in DotCharter's order: aoTuV 6.03,
//     the default library, inline codebooks, then default with mod packets
//     forced on and forced off. The first whose header rebuild AND libvorbis
//     header parse succeed wins;
//   * the "mod packet" rebuild (packet-type bit, mode number, previous/next
//     window flags) exactly as ww2ogg writes it, peek at the next packet included;
//   * the output LENGTH: the decoded total clipped to the end granule -- the
//     vorb chunk's sample count for granule-less files (every Rocksmith .wem),
//     the last packet's granule otherwise -- which is where the Ogg page
//     granules Ww2Ogg.Core writes make NVorbis stop.
//
// Threading: an instance is single-threaded (load thread / caller's thread).
// Nothing here may ever be reached from the audio callback: it allocates.
//
// PARALLEL DECODE (DotCharter #434), opt-in per instance (enableParallelDecode;
// the dca_wem_* exports opt in, the engine's juce::AudioFormat reader does not,
// because it seeks). A sequential read then decodes BATCHES of packets on up to
// `threads` worker threads: a batch is split into contiguous segments at packet
// boundaries, and each segment's worker restarts its own libvorbis state and
// decodes the packet BEFORE its first one with the output discarded. libvorbis's
// output for a packet depends only on that packet and its predecessor (the same
// fact seek() relies on), so the joined segments are BIT-IDENTICAL to the serial
// decode. The next batch is decoded while the caller drains the current one.
// A segment that cannot reproduce its packets exactly (a corrupt packet, a frame
// count the packet table did not predict) ends parallel mode at that packet and
// the rest is decoded serially, so even the failure behaviour is the serial one.
// Workers copy the caller's MXCSR (flush-to-zero / denormals-are-zero / rounding)
// so a caller that set them still gets its own arithmetic. The serial path stays
// for small files and as the reference.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dcwem
{

/// The largest .wem any entry point accepts (dca_wem_open_*, the engine's
/// juce::AudioFormat). A Rocksmith song is a few MB; past this the file is
/// refused before it is buffered, rather than copied into memory on its own say-so.
inline constexpr size_t kMaxWemBytes = (size_t) 256 << 20;

/// Upper bound on the decoded PCM one parallel batch holds. Segment buffers are
/// sized from the packet table, i.e. from the file; past this a batch is split
/// into fewer packets instead of allocated.
inline constexpr size_t kMaxBatchPcmBytes = (size_t) 64 << 20;

/// Which codebook source / packet format to use. `Auto` tries DotCharter's
/// variant list in order (see the file comment).
enum class Variant : int
{
    Auto            = 0,
    AoTuV           = 1,   // packed aoTuV 6.03 library, packet format from the header
    Default         = 2,   // packed default library, packet format from the header
    Inline          = 3,   // codebooks stored (stripped) inline in the setup packet
    DefaultMod      = 4,   // default library, mod packets forced ON
    DefaultStandard = 5,   // default library, mod packets forced OFF
    HeaderTriad     = 6    // old Wwise: real Vorbis headers present (vorb 0x28/0x2C)
};

struct WemInfo
{
    int      channels = 0;
    int      sampleRate = 0;
    int64_t  frames = 0;            // output length per channel (what read() delivers in total)
    int64_t  headerSampleCount = 0; // the vorb chunk's sample count
    int64_t  decodedFrames = 0;     // what the packets decode to before the end-granule clip
    int      packetCount = 0;       // audio packets
    Variant  variant = Variant::Auto;
    bool     modPackets = false;
    bool     granulePresent = false;
    bool     bigEndian = false;     // RIFX
};

class WemVorbisDecoder
{
public:
    /// Parses and prepares a decoder over `bytes` (the whole .wem). Returns null
    /// and a human-readable `error` for anything it cannot decode; never throws
    /// for malformed input.
    static std::unique_ptr<WemVorbisDecoder> open(std::shared_ptr<const std::vector<uint8_t>> bytes,
                                                  Variant variant,
                                                  std::string& error);

    ~WemVorbisDecoder();
    WemVorbisDecoder(const WemVorbisDecoder&) = delete;
    WemVorbisDecoder& operator=(const WemVorbisDecoder&) = delete;

    const WemInfo& info() const noexcept { return info_; }

    /// Decodes up to `frames` frames into `dest` (one float array per channel,
    /// `numChannels` of them; extra channels are zero-filled, missing ones are
    /// decoded and dropped). Returns the frames written; 0 at the end. -1 on a
    /// corrupt packet (the decoder stays usable after a seek).
    int64_t read(float* const* dest, int numChannels, int64_t frames);

    /// Positions the next read() at `frame` (clamped to [0, frames]). Bit-exact
    /// with a continuous decode: libvorbis's output for a packet depends only on
    /// it and its predecessor, so this restarts the synthesis one packet early.
    bool seek(int64_t frame);

    int64_t position() const noexcept { return position_; }

    /// The default worker count: min(4, hardware threads / 2), at least 1.
    static int defaultDecodeThreads() noexcept;
    /// The default packets per worker per batch.
    static constexpr int kDefaultLanePackets = 256;

    /// Opts this decoder into the parallel batch decode (see the file comment).
    /// Call right after open(), before the first read. A no-op (stays serial)
    /// when `threads` < 2 or the file has fewer than 2 * `lanePackets` packets.
    /// `threads` is clamped to [1, 16], `lanePackets` to [2, 65536].
    void enableParallelDecode(int threads, int lanePackets);

    /// True while reads go through the parallel batches.
    bool parallelDecode() const noexcept { return parallel_; }
    /// Batches decoded so far, and the most segments any of them used.
    int parallelBatches() const noexcept { return batches_; }
    int parallelMaxSegments() const noexcept { return maxSegments_; }

private:
    struct Packet
    {
        uint32_t payloadOffset;  // absolute offset of the payload in bytes_
        uint32_t size;
        uint32_t granule;
        uint8_t  mode;
        uint8_t  blockflag;
        uint8_t  nextBlockflag;  // ww2ogg's peek at the following packet (mod packets only)
        uint8_t  prevBlockflag;  // ww2ogg's running previous flag (mod packets only)
        int64_t  outStart;       // first output frame this packet produces
        int64_t  outCount;       // frames it produces (0 for the first packet)
    };

    struct Vorbis;               // libvorbis state, kept out of this header

    WemVorbisDecoder();

    bool decodePacketAt(size_t index, bool keepOutput);
    void rebuildAudioPacket(const Packet& p, std::vector<uint8_t>& out) const;

    std::shared_ptr<const std::vector<uint8_t>> bytes_;
    WemInfo info_;
    std::vector<Packet> packets_;
    std::vector<uint8_t> modeBlockflag_;
    int modeBits_ = 0;
    std::unique_ptr<Vorbis> vorbis_;
    std::vector<uint8_t> scratch_;

    // Read cursor: the packet whose output is pending, how much of it was
    // already handed out, and the output frame position.
    size_t  nextPacket_ = 0;
    int64_t pendingFrames_ = 0;      // frames available in libvorbis's pcm buffer
    int64_t pendingSkip_ = 0;        // leading frames of the pending output to drop (seek)
    int64_t position_ = 0;

    // ---- parallel decode (#434) ----
    struct Lane;                     // one worker's libvorbis state over vorbis_->info
    struct Segment;                  // a contiguous packet range and its planar output
    struct Batch;                    // the segments of one batch + their worker threads

    int64_t outStartAt(size_t index) const noexcept;
    bool lanePacket(Lane& lane, size_t index) const;
    void decodeSegment(Lane& lane, Segment& seg) const noexcept;
    void startBatch(Batch& b, size_t first, bool async);
    static void joinBatch(Batch& b) noexcept;
    bool advanceBatch();
    void discardBatches() noexcept;

    // Declared after vorbis_ so they are destroyed first (lanes point at its info).
    std::vector<std::unique_ptr<Lane>> lanes_;
    std::unique_ptr<Batch> ready_;   // the batch reads are served from
    std::unique_ptr<Batch> next_;    // the batch being decoded ahead
    size_t  readySegment_ = 0;
    int64_t readyOffset_ = 0;        // frames of ready_->segments[readySegment_] handed out
    bool    parallel_ = false;
    int     threads_ = 1;
    int     lanePackets_ = kDefaultLanePackets;
    int     batches_ = 0;
    int     maxSegments_ = 0;
};

/// Ww2ogg-compatible codebook library over one of the embedded packed tables.
/// Exposed for tests: rebuilds codebook `id` into a full Vorbis codebook bit
/// string and reports whether it consumed exactly its stored size.
bool rebuildLibraryCodebookForTest(bool aoTuV, int id, std::vector<uint8_t>& out, std::string& error);
int  libraryCodebookCount(bool aoTuV);

} // namespace dcwem
