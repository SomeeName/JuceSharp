// ExternalProcessInsert.h — ABI v47: a seat's live input sent to ANOTHER PROCESS
// through a named shared-memory block, and that process's processed audio played
// back in place of the seat's own rig.
//
// Generic on purpose: nothing here knows which program sits on the other side. The
// block's layout is a fixed ring format (below); the caller names the mapping and,
// optionally, the 8-byte magic its header must start with.
//
// Shared memory, format version 1. Little-endian, 32-bit float samples:
//   offset  0  char     magic[8]
//           8  uint32   version        1
//          12  uint32   sampleRate     the other side's rate; the device must run at it
//          16  uint32   blockFrames    frames per ring block; 0 = not published yet
//          20  uint32   ringBlocks     blocks per ring
//          24  uint32   inChannels     1 (the dry seat input, mono)
//          28  uint32   outChannels    1 or 2 (processed, INTERLEAVED)
//          32  uint32   inWrite        blocks written by this engine   (we increment)
//          36  uint32   inRead         blocks consumed by the other side
//          40  uint32   outWrite       blocks produced by the other side
//          44  uint32   outRead        blocks taken by this engine     (we increment)
//          48  uint32   otherUnderruns (the other side's counter; never written here)
//          52  uint32   hostUnderruns  ours, mirrored for the other side's log
//          56  uint32   hostOverruns   ours, mirrored for the other side's log
//          60  uint32   otherOverruns  (the other side's counter; never written here)
//   in  ring at 64:                                 float[ringBlocks][blockFrames * inChannels]
//   out ring at 64 + align64(ringBlocks * blockFrames * inChannels * 4):
//                                                   float[ringBlocks][blockFrames * outChannels]
// Rings are single-producer / single-consumer with free-running 32-bit block
// counters: the producer writes the block and then RELEASE-stores its counter, the
// consumer ACQUIRE-loads it. No mutex, no kernel object, no event.
//
// Real-time contract (RtExternalProcessInsert* in the native gate):
//   * the audio callback never allocates, locks, waits or makes a system call here;
//     it never waits on the other process. A block that is not back yet is an
//     UNDERRUN and the seat hears its DRY input for that block (never silence).
//   * the mapping is opened (OpenFileMapping + MapViewOfFile) and closed on the
//     CONTROL thread. A session is published with one seq_cst store; the audio
//     thread takes it at the silent bottom of a 5 ms dip (the dcdsp_rsamp_table.h
//     pattern) and announces what it uses in a hazard slot (inUse_), so a closed
//     session is unmapped only once the audio thread has provably let go of it.
#pragma once

#include "dotcharter_audio.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dotcharter {

namespace extproc {

inline constexpr uint32_t kFormatVersion   = 1;
inline constexpr size_t   kHeaderBytes     = 64;
inline constexpr int      kMaxBlockFrames  = 4096;
inline constexpr int      kMaxRingBlocks   = 4096;
inline constexpr int      kMaxOutChannels  = 2;
/// The dip a session change runs through (seconds): long enough to be inaudible
/// as a click, short enough to be inaudible as a gap.
inline constexpr double   kDipSeconds      = 0.005;
/// Processed blocks allowed to pile up beyond the priming target before the
/// oldest are dropped as LATE (bounds the latency a burst can add).
inline constexpr int      kLateSlackBlocks = 2;
/// Frames of crossfade when the output switches between processed and dry.
inline constexpr int      kSwitchFadeFrames = 64;

// Header field offsets (bytes).
inline constexpr size_t kOffMagic       = 0;
inline constexpr size_t kOffVersion     = 8;
inline constexpr size_t kOffSampleRate  = 12;
inline constexpr size_t kOffBlockFrames = 16;
inline constexpr size_t kOffRingBlocks  = 20;
inline constexpr size_t kOffInChannels  = 24;
inline constexpr size_t kOffOutChannels = 28;
inline constexpr size_t kOffInWrite     = 32;
inline constexpr size_t kOffInRead      = 36;
inline constexpr size_t kOffOutWrite    = 40;
inline constexpr size_t kOffOutRead     = 44;
inline constexpr size_t kOffHostUnder   = 52;
inline constexpr size_t kOffHostOver    = 56;

/// The header exactly as the format declares it, field for field. Only used to
/// pin the offsets above: a header 8 bytes too long makes both sides agree with
/// each other and both be wrong, so every offset is checked against offsetof.
struct SharedHeader
{
    char     magic[8];
    uint32_t version, sampleRate, blockFrames, ringBlocks, inChannels, outChannels;
    uint32_t inWrite, inRead, outWrite, outRead;
    uint32_t otherUnderruns, hostUnderruns, hostOverruns, otherOverruns;
};
static_assert(sizeof(SharedHeader) == kHeaderBytes, "the shared header is 64 bytes");
static_assert(offsetof(SharedHeader, magic)       == kOffMagic,       "magic");
static_assert(offsetof(SharedHeader, version)     == kOffVersion,     "version");
static_assert(offsetof(SharedHeader, sampleRate)  == kOffSampleRate,  "sampleRate");
static_assert(offsetof(SharedHeader, blockFrames) == kOffBlockFrames, "blockFrames");
static_assert(offsetof(SharedHeader, ringBlocks)  == kOffRingBlocks,  "ringBlocks");
static_assert(offsetof(SharedHeader, inChannels)  == kOffInChannels,  "inChannels");
static_assert(offsetof(SharedHeader, outChannels) == kOffOutChannels, "outChannels");
static_assert(offsetof(SharedHeader, inWrite)     == kOffInWrite,     "inWrite");
static_assert(offsetof(SharedHeader, inRead)      == kOffInRead,      "inRead");
static_assert(offsetof(SharedHeader, outWrite)    == kOffOutWrite,    "outWrite");
static_assert(offsetof(SharedHeader, outRead)     == kOffOutRead,     "outRead");
static_assert(offsetof(SharedHeader, hostUnderruns) == kOffHostUnder, "hostUnderruns");
static_assert(offsetof(SharedHeader, hostOverruns)  == kOffHostOver,  "hostOverruns");

constexpr size_t align64(size_t v) noexcept { return (v + 63u) & ~size_t(63u); }

/// The ring geometry a header describes, validated against the mapped size.
struct Layout
{
    int    blockFrames = 0;
    int    ringBlocks  = 0;
    int    outChannels = 0;
    size_t inOffset    = 0;
    size_t outOffset   = 0;

    bool operator==(const Layout& o) const noexcept
    {
        return blockFrames == o.blockFrames && ringBlocks == o.ringBlocks && outChannels == o.outChannels;
    }
};

/// Pure: the layout for these header numbers, or false when it is not one this
/// build runs (blockFrames 0 is "not published yet" and also false).
bool computeLayout(uint32_t blockFrames, uint32_t ringBlocks, uint32_t inChannels,
                   uint32_t outChannels, size_t mappedBytes, Layout& out) noexcept;

} // namespace extproc

/// One open mapping. Created and destroyed on the control thread only; between
/// its publication and its retirement the AUDIO thread owns every non-atomic field.
struct ExtProcSession
{
    ExtProcSession() = default;
    ~ExtProcSession();
    ExtProcSession(const ExtProcSession&)            = delete;
    ExtProcSession& operator=(const ExtProcSession&) = delete;

    // --- control thread (fixed after open) ------------------------------
    void*    mapping    = nullptr; // HANDLE
    uint8_t* view       = nullptr;
    size_t   viewBytes  = 0;
    int      sampleRate = 0;

    // --- audio thread ---------------------------------------------------
    std::vector<float> inBlock;   // kMaxBlockFrames, preallocated at open
    std::vector<float> outBlock;  // kMaxBlockFrames * 2 (stereo, interleaved)
    extproc::Layout layout;
    bool     synced      = false; // counters taken over for the current layout
    bool     primed      = false;
    bool     everPrimed  = false;
    bool     outIsDry    = true;
    int      accPos      = 0;
    int      maxCallback = 0;
    uint32_t inWrite     = 0;
    uint32_t outRead     = 0;
    float    lastOut[2]  = { 0.0f, 0.0f };
    int64_t  sentSinceSync     = 0;
    int64_t  receivedSinceSync = 0;
    int64_t  lateSinceSync     = 0;

    // --- stats: written by the audio thread, read by anyone --------------
    std::atomic<int32_t> state          { DCA_EXTPROC_STATE_WAITING };
    std::atomic<int32_t> connected      { 0 };
    std::atomic<int32_t> blockFrames    { 0 };
    std::atomic<int32_t> ringBlocks     { 0 };
    std::atomic<int32_t> latencySamples { 0 };
    std::atomic<int64_t> sent           { 0 };
    std::atomic<int64_t> received       { 0 };
    std::atomic<int64_t> underruns      { 0 };
    std::atomic<int64_t> overruns       { 0 };
    std::atomic<int64_t> late           { 0 };

    std::atomic<uint32_t>& header32(size_t offset) const noexcept
    {
        return *reinterpret_cast<std::atomic<uint32_t>*>(view + offset);
    }

    /// Audio thread. Exchanges `numSamples` frames: `chain[0..chainChannels)`
    /// carries the dry input in and the processed (or dry) output out. `monoCh`
    /// picks the input channel the other side hears (-1 = the average).
    void process(float* const* chain, int chainChannels, int numSamples, int monoCh) noexcept;

private:
    void boundary() noexcept;
    void resync() noexcept;
    void fillDryFromInput() noexcept;
};

/// One seat's insert. The engine owns kMaxPlayers of them.
class ExternalProcessInsert
{
public:
    ExternalProcessInsert() = default;
    ~ExternalProcessInsert();
    ExternalProcessInsert(const ExternalProcessInsert&)            = delete;
    ExternalProcessInsert& operator=(const ExternalProcessInsert&) = delete;

    // --- control thread -------------------------------------------------
    /// Maps `mappingName` (UTF-8) and publishes it. `magic` (NULL = not checked)
    /// is compared with the header's first 8 bytes, zero-padded. Statuses:
    /// DCA_ERR_NOT_FOUND (no such mapping), DCA_ERR_DECODE (the header is not a
    /// format this build runs), DCA_ERR_NOT_INITIALIZED (no device running),
    /// DCA_ERR_SAMPLE_RATE (the device does not run at the block's rate; nothing
    /// is opened), DCA_ERR_DEVICE (the OS refused to map it). A successful open
    /// REPLACES a session already open on the seat.
    dca_status open(const char* mappingName, const char* magic);
    /// Unpublishes the session and unmaps it once the audio thread let go (a
    /// bounded wait of at most ~200 ms; a session still held after that is
    /// unmapped by the next control call or the destructor). False when nothing
    /// was open.
    bool close();
    void fillStats(dca_extproc_stats& out);

    // --- device thread, no callback in flight ----------------------------
    void prepare(double sampleRate) noexcept;
    void deviceStopped() noexcept;

    // --- audio thread -----------------------------------------------------
    /// Takes a newly published session at the bottom of the dip. True when this
    /// block's seat output is the insert's (the caller skips the seat's rig).
    bool beginBlock() noexcept;
    /// Exchanges the block. Only after beginBlock() returned true.
    void process(float* const* chain, int chainChannels, int numSamples, int monoCh) noexcept
    {
        active_->process(chain, chainChannels, numSamples, monoCh);
    }
    /// Applies the dip ramp to whatever path produced the block. A no-op in the
    /// steady state (no session change pending).
    void endBlock(float* const* chain, int chainChannels, int numSamples) noexcept;

    /// True while a session is published (cheap; any thread).
    bool isOpen() const noexcept { return published_.load(std::memory_order_acquire) != nullptr; }

private:
    void retireLocked(ExtProcSession* s);
    void reapLocked();

    std::mutex controlMutex_;  // control thread only; the audio thread never takes it
    std::unique_ptr<ExtProcSession>              current_;
    std::vector<std::unique_ptr<ExtProcSession>> retired_;

    std::atomic<ExtProcSession*> published_ { nullptr };
    std::atomic<ExtProcSession*> inUse_     { nullptr }; // hazard: what the audio thread may touch
    std::atomic<int>             deviceRate_ { 0 };

    // audio thread only
    ExtProcSession* active_     = nullptr;
    bool            engaged_    = false;
    bool            fadingOut_  = false;
    float           gain_       = 1.0f;
    float           gainStep_   = 1.0f / 240.0f;
};

} // namespace dotcharter
