// dotcharter_vst_bridge.h
//
// Shared-memory audio bridge layout between the DotCharter host process
// (juce_sharp.dll, "parent") and the sandboxed VST host process
// (juce_vst_host.exe, "child").
//
// One named file mapping carries a fixed header plus three planar float
// regions:
//   input    : parent -> child   (live input feeding the guitar FX chain)
//   chainOut : child  -> parent  (processed FX-chain output)
//   instrOut : child  -> parent  (rendered VSTi instrument output)
// and, since v3, a small MIDI-timing block after them (parent -> child): the
// sample offset of every event in midi[] plus the block's render-clock frame.
//
// Handshake per audio block (single-producer / single-consumer):
//   1. parent stores kRequestWriting in requestSeq, writes
//      blockFrames/inputChannels/midi/seat + input samples, then
//      requestSeq.store(seq, release) and signals the request event. The
//      child drops a request whose requestSeq changed while it copied it
//      (takeRequest below).
//   2. child wakes, processes, writes chainOut/instrOut,
//      then responseSeq.store(seq, release) and signals the response event.
//   3. parent spins on responseSeq against a ONE-BLOCK deadline until
//      responseSeq == seq. On timeout the parent passes audio through DRY.
//      The response event is a wake hint that the parent drains without ever
//      blocking: a TIMED kernel wait rounds up to the ~15.6 ms system timer
//      tick, which is three times a typical block budget (finding E-1).
//
// The layout is pinned by static_asserts below; the managed test-suite
// mirrors the same constants (VstSandboxLayout in DotCharter.Services).
// Any change here MUST bump kBridgeVersion.

#ifndef DOTCHARTER_VST_BRIDGE_H
#define DOTCHARTER_VST_BRIDGE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dcvb {

constexpr uint32_t kMagic         = 0x44435642u; // 'DCVB'
constexpr uint32_t kBridgeVersion = 4;
// v4 (2026-11-14, DotCharter #646): sandboxed chain plugins belong to a SEAT.
// A SeatRouting block is APPENDED after MidiTiming (kSeatRoutingOffset): the
// parent names the seat each request is for, and the child publishes which
// seats hold at least one chain plugin. The child runs only that seat's chain
// on a request, and renders the instrument only on a seat-0 request. Every v3
// offset is unchanged; the mapping grows by 8 bytes. A v3 child would ignore
// the seat and run every seat's plugins over whichever input a request
// carries, so both sides refuse the mismatch.
//
// v3 (2026-10-13, JuceSharp #28): MIDI to a sandboxed instrument is
// SAMPLE-ACCURATE. A MidiTiming block is APPENDED after the instrOut region
// (kMidiTimingOffset): the sample offset inside this block of every event in
// midi[], and the render-clock frame of the block's first sample. The header
// and every v2 offset are unchanged; the mapping grows by 520 bytes. A v2
// child would read the events and ignore the offsets, i.e. play every
// scheduled note at sample 0 of whichever block carried it — now that the
// parent PARKS a scheduled event until the block that contains its frame,
// that is up to a block early or late, silently. Both sides refuse it.
//
// v2 (2026-09-10): `reserved0` and `reserved1` became `prepareBlockFrames` and
// `prepareGeneration`. Same offsets, same 576-byte header — but the child now
// RE-READS the device configuration, and a v1 child would ignore the
// generation counter and keep running at the rate it was launched with. The
// version check on both sides refuses the mismatch rather than letting that
// happen silently.

constexpr int kMaxChannels    = 2;
constexpr int kMaxBlockFrames = 8192;
constexpr int kMaxMidiEvents  = 128;
constexpr int kMaxSeats       = 4;   // the engine's kMaxPlayers

// Child lifecycle states published in SharedHeader::childState.
constexpr uint32_t kChildStarting = 0;
constexpr uint32_t kChildReady    = 1;

#pragma pack(push, 8)
struct SharedHeader
{
    // --- immutable after creation (parent writes once) -------------------
    uint32_t magic;             // offset 0
    uint32_t version;           // offset 4
    uint32_t maxBlockFrames;    // offset 8
    uint32_t maxChannels;       // offset 12

    // --- sequence / liveness ---------------------------------------------
    std::atomic<uint32_t> requestSeq;   // offset 16: parent bumps after writing a request
    std::atomic<uint32_t> responseSeq;  // offset 20: child sets = requestSeq after responding
    std::atomic<uint32_t> heartbeat;    // offset 24: child bumps every worker wakeup
    std::atomic<int32_t>  sampleRate;   // offset 28: parent's device sample rate

    // --- request payload (parent writes, child reads) ---------------------
    uint32_t blockFrames;       // offset 32: frames valid in this block (<= maxBlockFrames)
    uint32_t inputChannels;     // offset 36: valid input channels (0 = no live input this block)
    uint32_t midiCount;         // offset 40: events valid in midi[]
    // The block size the PARENT has prepared for. The child was told its rate
    // and block ONCE, as launch arguments, and `sampleRate` (offset 28) was
    // written by the parent but read only in openBridge — so after
    // dca_engine_reconfigure every sandboxed VST3 went on running at the old
    // rate, and a plugin prepared for 256 frames could be handed 1024. That is
    // exactly the bug reprepareAttachedPlugins closed for IN-PROCESS plugins;
    // this is the same bug one process boundary away.
    std::atomic<uint32_t> prepareBlockFrames;  // offset 44
    uint32_t midi[kMaxMidiEvents]; // offset 48: packed (status | d1<<8 | d2<<16)

    // --- response payload (child writes, parent reads) --------------------
    std::atomic<uint32_t> childState;  // offset 560: kChildStarting / kChildReady
    uint32_t chainActive;       // offset 564: nonzero when >=1 chain plugin processed the block
    uint32_t instrumentActive;  // offset 568: nonzero when the instrument rendered audio
    // Bumped by the parent AFTER writing sampleRate + prepareBlockFrames, so the
    // child sees one coherent configuration rather than two half-written
    // fields. Parent-written despite sitting in the response block: it is the
    // only 4-byte slot left in a header whose offsets are pinned. The child
    // re-prepares on ITS message thread and answers silence until that lands.
    std::atomic<uint32_t> prepareGeneration;   // offset 572
};
#pragma pack(pop)

static_assert(sizeof(std::atomic<uint32_t>) == 4, "shared atomics must be 4 bytes");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "shared atomics must be lock-free");
static_assert(sizeof(SharedHeader) == 576, "bridge header layout is ABI-pinned (mirror: VstSandboxLayout.cs)");

// Planar float regions follow the header. 576 is 64-byte aligned already.
constexpr size_t kHeaderBytes   = 576;
constexpr size_t kChannelBytes  = (size_t) kMaxBlockFrames * sizeof(float);
constexpr size_t kRegionBytes   = (size_t) kMaxChannels * kChannelBytes;
constexpr size_t kInputOffset   = kHeaderBytes;
constexpr size_t kChainOutOffset= kInputOffset + kRegionBytes;
constexpr size_t kInstrOutOffset= kChainOutOffset + kRegionBytes;

static_assert(kInputOffset    == 576,    "layout pinned");
static_assert(kChainOutOffset == 66112,  "layout pinned");
static_assert(kInstrOutOffset == 131648, "layout pinned");

// v3: per-event sample offsets for midi[] (parent writes, child reads). Lives
// AFTER the audio regions so no v2 offset moved; written in the same request,
// before requestSeq's release, exactly like midi[] itself.
#pragma pack(push, 8)
struct MidiTiming
{
    int64_t  blockStartFrame;               // +0: render-clock frame of sample 0 of this block
    uint32_t midiOffset[kMaxMidiEvents];    // +8: sample offset of midi[i], < blockFrames
};
#pragma pack(pop)
static_assert(sizeof(MidiTiming) == 520, "bridge MIDI-timing layout is ABI-pinned (mirror: VstSandboxLayout.cs)");

constexpr size_t kMidiTimingOffset = kInstrOutOffset + kRegionBytes;

// v4: the seat a request is for (parent writes, child reads; written before
// requestSeq's release like the rest of the request) and the seats whose chain
// is not empty (child writes after every load / unload / reorder, parent reads
// to skip a round trip for a seat with nothing to run).
#pragma pack(push, 8)
struct SeatRouting
{
    uint32_t              requestSeat;    // +0: 0 .. kMaxSeats-1
    std::atomic<uint32_t> seatChainMask;  // +4: bit s set = seat s has a chain plugin
};
#pragma pack(pop)
static_assert(sizeof(SeatRouting) == 8, "bridge seat-routing layout is ABI-pinned (mirror: VstSandboxLayout.cs)");

constexpr size_t kSeatRoutingOffset = kMidiTimingOffset + sizeof(MidiTiming);
constexpr size_t kTotalBytes        = kSeatRoutingOffset + sizeof(SeatRouting);

static_assert(kMidiTimingOffset  == 197184, "layout pinned");
static_assert(kSeatRoutingOffset == 197704, "layout pinned");
static_assert(kTotalBytes        == 197712, "layout pinned");

inline float* inputChannel(void* base, int ch) noexcept
{ return reinterpret_cast<float*>(static_cast<char*>(base) + kInputOffset + (size_t) ch * kChannelBytes); }

inline float* chainOutChannel(void* base, int ch) noexcept
{ return reinterpret_cast<float*>(static_cast<char*>(base) + kChainOutOffset + (size_t) ch * kChannelBytes); }

inline float* instrOutChannel(void* base, int ch) noexcept
{ return reinterpret_cast<float*>(static_cast<char*>(base) + kInstrOutOffset + (size_t) ch * kChannelBytes); }

inline MidiTiming* midiTiming(void* base) noexcept
{ return reinterpret_cast<MidiTiming*>(static_cast<char*>(base) + kMidiTimingOffset); }

/// Child side: the sample offset of midi[i] inside a block of `blockFrames`,
/// CLAMPED into the block. The parent never writes one outside it, but this is
/// shared memory another process writes, and JUCE's MidiBuffer would happily
/// carry an event at sample 9000 of a 128-frame block that the plugin then
/// never plays (a lost note-off is a stuck note).
inline int midiSampleOffset(const MidiTiming& t, int i, int blockFrames) noexcept
{
    if (blockFrames <= 0 || i < 0 || i >= kMaxMidiEvents) return 0;
    const uint32_t o = t.midiOffset[i];
    return o >= (uint32_t) blockFrames ? blockFrames - 1 : (int) o;
}

inline SeatRouting* seatRouting(void* base) noexcept
{ return reinterpret_cast<SeatRouting*>(static_cast<char*>(base) + kSeatRoutingOffset); }

inline SharedHeader* header(void* base) noexcept
{ return static_cast<SharedHeader*>(base); }

inline uint32_t packMidi(uint8_t status, uint8_t d1, uint8_t d2) noexcept
{ return (uint32_t) status | ((uint32_t) d1 << 8) | ((uint32_t) d2 << 16); }

// ---- request sequence: a payload is only ever read whole -------------------
//
// The parent posts a new request only after the previous one was answered or
// given up on, so a LATE child can still be copying request N when the parent
// starts writing request N+1 (another seat's input, another seat number). A
// child that ran N's seat over a half-overwritten payload would put one
// player's audio through the other player's plugins, and a delay or reverb
// tail keeps it there long after the response is thrown away.
//
// So requestSeq doubles as a sequence lock. The parent stores kRequestWriting
// BEFORE the first payload byte (beginRequestWrite) and the new sequence after
// the last (publishRequest), so requestSeq differs from N for the whole of an
// overwrite and not only once it is finished. The child reads the seat right
// after acquiring N, copies the payload, and keeps the request only when
// requestSeq still reads N (requestStillCurrent). A dropped request is one the
// parent has already counted as a miss, because it only overwrites a request
// it stopped waiting for; nothing is processed or published for it.
//
// What the child publishes, by path: a request overwritten DURING its copy
// (takeRequest's Dropped) gets nothing; the silent answer of a re-preparing
// child (answerInactive) is published only while requestSeq still names the
// request; a request copied whole is processed and answered with its own
// sequence even when the parent overwrote the slot while the plugins ran.
// That late answer is harmless: the parent compares responseSeq with the
// exact sequence it waits for (or polled), so N never answers N+1, and the
// child, which serves one request at a time, writes N+1's output over N's
// before it publishes N+1.
constexpr uint32_t kRequestWriting = 0xFFFFFFFFu;

/// The sequence after `seq`: never 0 (a mapping nobody posted to) and never
/// kRequestWriting.
inline uint32_t nextRequestSeq(uint32_t seq) noexcept
{
    ++seq;
    return (seq == 0u || seq == kRequestWriting) ? 1u : seq;
}

/// Parent: called before the first payload byte of a request is written.
inline void beginRequestWrite(SharedHeader* h) noexcept
{
    h->requestSeq.store(kRequestWriting, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

/// Parent: called after the last payload byte; publishes the request.
inline void publishRequest(SharedHeader* h, uint32_t seq) noexcept
{
    h->requestSeq.store(seq, std::memory_order_release);
}

/// Child: whether the request acquired as `seq` is still the one in the
/// mapping once its payload has been copied out.
inline bool requestStillCurrent(const SharedHeader* h, uint32_t seq) noexcept
{
    std::atomic_thread_fence(std::memory_order_acquire);
    return h->requestSeq.load(std::memory_order_relaxed) == seq;
}

/// Child: answer request `seq` with nothing active, without running a plugin
/// (juce_vst_host does this while it re-prepares). Like a copied request, it
/// is answered only while requestSeq still names it: one the parent has begun
/// overwriting is a request it already gave up on, and nothing is published
/// for it. Returns whether the answer was published (the caller then signals
/// the response event).
inline bool answerInactive(SharedHeader* h, uint32_t seq) noexcept
{
    if (! requestStillCurrent(h, seq)) return false;
    h->chainActive      = 0;
    h->instrumentActive = 0;
    h->responseSeq.store(seq, std::memory_order_release);
    return true;
}

enum class RequestTake { None, Taken, Dropped };

/// Child: take the next request, the way juce_vst_host does and the native
/// tests' stub child does. `None`: nothing new (or a write in progress).
/// `Dropped`: the parent overwrote the request during `copyPayload`, so the
/// copy is unusable and nothing may be processed or published for it.
/// `Taken`: `seq` and `seat` describe the request `copyPayload` copied whole.
/// `beforeCopy` runs between the acquire and the copy and may answer the
/// request itself (returning false skips the copy and reports `None`).
template <typename BeforeCopy, typename CopyPayload>
inline RequestTake takeRequest(void* base, uint32_t& lastSeq, uint32_t& seq, uint32_t& seat,
                               BeforeCopy&& beforeCopy, CopyPayload&& copyPayload)
{
    auto* h = header(base);
    seq = h->requestSeq.load(std::memory_order_acquire);
    if (seq == lastSeq || seq == kRequestWriting) return RequestTake::None;
    lastSeq = seq;
    // The seat is read with the acquire, not after the copy: by then the
    // parent may have named the next request's seat.
    seat = seatRouting(base)->requestSeat;
    if (! beforeCopy()) return RequestTake::None;
    copyPayload();
    return requestStillCurrent(h, seq) ? RequestTake::Taken : RequestTake::Dropped;
}

} // namespace dcvb

#endif // DOTCHARTER_VST_BRIDGE_H
