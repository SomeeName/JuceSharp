#include "ExternalProcessInsert.h"

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace dotcharter {

static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "the shared header's counters are viewed as std::atomic<uint32_t>");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "a cross-process counter must be lock-free");

namespace extproc {

bool computeLayout(uint32_t blockFrames, uint32_t ringBlocks, uint32_t inChannels,
                   uint32_t outChannels, size_t mappedBytes, Layout& out) noexcept
{
    if (blockFrames == 0 || blockFrames > (uint32_t) kMaxBlockFrames) return false;
    if (ringBlocks < 2 || ringBlocks > (uint32_t) kMaxRingBlocks) return false;
    if (inChannels != 1) return false;
    if (outChannels < 1 || outChannels > (uint32_t) kMaxOutChannels) return false;
    const size_t inBytes  = (size_t) ringBlocks * blockFrames * inChannels * sizeof(float);
    const size_t outBytes = (size_t) ringBlocks * blockFrames * outChannels * sizeof(float);
    const size_t inOffset  = align64(kHeaderBytes);
    const size_t outOffset = align64(inOffset + inBytes);
    if (outOffset + outBytes > mappedBytes) return false;
    out.blockFrames = (int) blockFrames;
    out.ringBlocks  = (int) ringBlocks;
    out.outChannels = (int) outChannels;
    out.inOffset    = inOffset;
    out.outOffset   = outOffset;
    return true;
}

} // namespace extproc

// ---------------------------------------------------------------------------
// ExtProcSession
// ---------------------------------------------------------------------------

ExtProcSession::~ExtProcSession()
{
#if defined(_WIN32)
    if (view != nullptr)    ::UnmapViewOfFile(view);
    if (mapping != nullptr) ::CloseHandle((HANDLE) mapping);
#endif
    view    = nullptr;
    mapping = nullptr;
}

void ExtProcSession::resync() noexcept
{
    // Discard whatever the other side produced before we were listening (it may
    // have rendered blocks of its own), and continue our input counter from
    // where the header has it — we are its only writer.
    outRead = header32(extproc::kOffOutWrite).load(std::memory_order_acquire);
    header32(extproc::kOffOutRead).store(outRead, std::memory_order_release);
    inWrite = header32(extproc::kOffInWrite).load(std::memory_order_relaxed);
    accPos  = 0;
    primed  = false;
    outIsDry = true;
    sentSinceSync = receivedSinceSync = lateSinceSync = 0;
    std::fill(outBlock.begin(), outBlock.begin() + (size_t) layout.blockFrames * 2, 0.0f);
    synced = true;
}

void ExtProcSession::fillDryFromInput() noexcept
{
    const int B = layout.blockFrames;
    for (int j = 0; j < B; ++j)
    {
        const float s = inBlock[(size_t) j];
        outBlock[(size_t) (2 * j)]     = s;
        outBlock[(size_t) (2 * j + 1)] = s;
    }
}

// One ring block is complete: hand it over, and take the next processed block
// (or fall back to the dry one) for the NEXT blockFrames of output. Everything
// here is bounded integer and memcpy work on preallocated memory.
void ExtProcSession::boundary() noexcept
{
    const int B = layout.blockFrames;
    const int R = layout.ringBlocks;
    const int C = layout.outChannels;

    // --- send ------------------------------------------------------------
    const uint32_t inRead = header32(extproc::kOffInRead).load(std::memory_order_acquire);
    if ((uint32_t) (inWrite - inRead) < (uint32_t) R)
    {
        auto* slot = reinterpret_cast<float*>(view + layout.inOffset)
                   + (size_t) (inWrite % (uint32_t) R) * (size_t) B;
        std::memcpy(slot, inBlock.data(), sizeof(float) * (size_t) B);
        ++inWrite;
        header32(extproc::kOffInWrite).store(inWrite, std::memory_order_release);
        ++sentSinceSync;
        sent.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        // Full: never overwrite a block the other side has not read.
        const auto n = overruns.fetch_add(1, std::memory_order_relaxed) + 1;
        header32(extproc::kOffHostOver).store((uint32_t) n, std::memory_order_relaxed);
    }

    // --- receive ---------------------------------------------------------
    const uint32_t outWrite = header32(extproc::kOffOutWrite).load(std::memory_order_acquire);
    uint32_t avail = outWrite - outRead;
    if (avail > (uint32_t) R)
    {
        // The other side reset or lapped us: nothing in the ring is ours.
        outRead = outWrite;
        avail   = 0;
        primed  = false;
    }

    // The cushion: enough processed blocks to cover the largest callback this
    // device has asked for, plus ONE block of headroom so a block that is a
    // little late is not an underrun.
    const int perCallback = (maxCallback + B - 1) / B;
    const uint32_t target = (uint32_t) std::min(std::max(perCallback, 1) + 1, R - 1);

    if (! primed && avail >= target)
    {
        primed     = true;
        everPrimed = true;
    }

    bool gotBlock = false;
    if (primed)
    {
        if (avail > target + (uint32_t) extproc::kLateSlackBlocks)
        {
            const uint32_t drop = avail - target;
            outRead += drop;
            avail    = target;
            lateSinceSync += drop;
            late.fetch_add((int64_t) drop, std::memory_order_relaxed);
        }
        if (avail >= 1)
        {
            const auto* slot = reinterpret_cast<const float*>(view + layout.outOffset)
                             + (size_t) (outRead % (uint32_t) R) * (size_t) B * (size_t) C;
            if (C == 2)
            {
                std::memcpy(outBlock.data(), slot, sizeof(float) * (size_t) B * 2);
            }
            else
            {
                for (int j = 0; j < B; ++j)
                {
                    outBlock[(size_t) (2 * j)]     = slot[j];
                    outBlock[(size_t) (2 * j + 1)] = slot[j];
                }
            }
            ++outRead;
            header32(extproc::kOffOutRead).store(outRead, std::memory_order_release);
            ++receivedSinceSync;
            received.fetch_add(1, std::memory_order_relaxed);
            gotBlock = true;
            // Blocks sent whose processed block has not been played yet, plus
            // the block of input being gathered: the whole added delay.
            const int64_t inFlight = sentSinceSync - receivedSinceSync - lateSinceSync;
            latencySamples.store((int32_t) std::max<int64_t>(0, (inFlight + 1) * B),
                                 std::memory_order_relaxed);
        }
        else
        {
            primed = false; // re-prime: wait for the cushion again
        }
    }

    if (! gotBlock)
    {
        // DRY, not silence: a player who loses the other process for a block
        // hears their guitar, not a dropout. Counted once primed at least once
        // (the wait for the very first cushion is not a fault).
        if (everPrimed)
        {
            const auto n = underruns.fetch_add(1, std::memory_order_relaxed) + 1;
            header32(extproc::kOffHostUnder).store((uint32_t) n, std::memory_order_relaxed);
        }
        fillDryFromInput();
    }

    // A switch between processed and dry fades from the last sample played, so
    // the seam is a short ramp rather than a step.
    if (gotBlock == outIsDry)
    {
        const int fade = std::min(extproc::kSwitchFadeFrames, B);
        for (int j = 0; j < fade; ++j)
        {
            const float t = (float) (j + 1) / (float) (fade + 1);
            for (int c = 0; c < 2; ++c)
            {
                float& s = outBlock[(size_t) (2 * j + c)];
                s = lastOut[c] + (s - lastOut[c]) * t;
            }
        }
    }
    outIsDry = ! gotBlock;
    connected.store(primed ? 1 : 0, std::memory_order_relaxed);
}

void ExtProcSession::process(float* const* chain, int chainChannels, int numSamples, int monoCh) noexcept
{
    if (chain == nullptr || chainChannels <= 0 || numSamples <= 0) return;

    // Re-read the geometry every block (four relaxed loads): the other side
    // publishes blockFrames after its first rendered block, and may change it.
    extproc::Layout now;
    const bool valid = extproc::computeLayout(
        header32(extproc::kOffBlockFrames).load(std::memory_order_acquire),
        header32(extproc::kOffRingBlocks).load(std::memory_order_relaxed),
        header32(extproc::kOffInChannels).load(std::memory_order_relaxed),
        header32(extproc::kOffOutChannels).load(std::memory_order_relaxed),
        viewBytes, now);
    if (! valid)
    {
        // Not published yet (blockFrames 0) or not a layout this build runs:
        // the seat plays its dry input, exactly what an underrun plays.
        const bool waiting = header32(extproc::kOffBlockFrames).load(std::memory_order_relaxed) == 0;
        state.store(waiting ? DCA_EXTPROC_STATE_WAITING : DCA_EXTPROC_STATE_BAD_LAYOUT,
                    std::memory_order_relaxed);
        connected.store(0, std::memory_order_relaxed);
        synced = false;
        return;
    }
    if (! synced || ! (now == layout))
    {
        layout = now;
        blockFrames.store(now.blockFrames, std::memory_order_relaxed);
        ringBlocks.store(now.ringBlocks, std::memory_order_relaxed);
        resync();
    }
    state.store(DCA_EXTPROC_STATE_ACTIVE, std::memory_order_relaxed);
    maxCallback = std::max(maxCallback, numSamples);

    float* L = chain[0];
    float* R = chainChannels > 1 ? chain[1] : nullptr;
    const float* src = (monoCh == 1 && R != nullptr) ? R : L;
    const bool average = monoCh < 0 && R != nullptr;
    const int B = layout.blockFrames;

    int i = 0;
    while (i < numSamples)
    {
        const int k = std::min(numSamples - i, B - accPos);
        float* in = inBlock.data() + accPos;
        if (average)
        {
            for (int j = 0; j < k; ++j) in[j] = 0.5f * (L[i + j] + R[i + j]);
        }
        else
        {
            std::memcpy(in, src + i, sizeof(float) * (size_t) k);
        }
        const float* o = outBlock.data() + (size_t) accPos * 2;
        for (int j = 0; j < k; ++j)
        {
            L[i + j] = o[2 * j];
            if (R != nullptr) R[i + j] = o[2 * j + 1];
        }
        for (int c = 2; c < chainChannels; ++c)
            if (chain[c] != nullptr) std::memcpy(chain[c] + i, L + i, sizeof(float) * (size_t) k);
        lastOut[0] = o[2 * (k - 1)];
        lastOut[1] = o[2 * (k - 1) + 1];
        accPos += k;
        i      += k;
        if (accPos == B)
        {
            boundary();
            accPos = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// ExternalProcessInsert
// ---------------------------------------------------------------------------

ExternalProcessInsert::~ExternalProcessInsert()
{
    // The engine is torn down after its device: no callback can be in flight.
    published_.store(nullptr, std::memory_order_seq_cst);
    inUse_.store(nullptr, std::memory_order_seq_cst);
    std::lock_guard<std::mutex> lk(controlMutex_);
    retired_.clear();
    current_.reset();
}

void ExternalProcessInsert::prepare(double sampleRate) noexcept
{
    const int sr = sampleRate > 0.0 ? (int) (sampleRate + 0.5) : 0;
    deviceRate_.store(sr, std::memory_order_relaxed);
    gainStep_ = sr > 0 ? (float) (1.0 / (extproc::kDipSeconds * (double) sr)) : 1.0f;
}

void ExternalProcessInsert::deviceStopped() noexcept
{
    // No callback in flight and none until the next prepare: let go of the
    // session so a close in between can unmap it. The next session start takes
    // the published one at once (a session pending means we start at the
    // bottom of the dip and fade in).
    active_  = nullptr;
    engaged_ = false;
    inUse_.store(nullptr, std::memory_order_seq_cst);
    gain_ = published_.load(std::memory_order_acquire) != nullptr ? 0.0f : 1.0f;
    deviceRate_.store(0, std::memory_order_relaxed);
}

bool ExternalProcessInsert::beginBlock() noexcept
{
    ExtProcSession* target = published_.load(std::memory_order_seq_cst);
    if (target != active_ && gain_ <= 0.0f)
    {
        // The bottom of the dip: switch. Hazard protocol — announce, then check
        // the session is STILL the published one; a close that raced us has
        // already unpublished it and may be about to unmap it.
        inUse_.store(target, std::memory_order_seq_cst);
        if (published_.load(std::memory_order_seq_cst) != target)
        {
            inUse_.store(nullptr, std::memory_order_seq_cst);
            active_ = nullptr;
        }
        else
        {
            active_ = target;
            if (active_ != nullptr)
            {
                active_->synced      = false;
                active_->everPrimed  = false;
                active_->maxCallback = 0;
            }
        }
        target = published_.load(std::memory_order_relaxed);
    }
    fadingOut_ = target != active_;

    const int sr = deviceRate_.load(std::memory_order_relaxed);
    engaged_ = active_ != nullptr && sr > 0 && sr == active_->sampleRate;
    if (active_ != nullptr && ! engaged_)
    {
        active_->state.store(DCA_EXTPROC_STATE_RATE_MISMATCH, std::memory_order_relaxed);
        active_->connected.store(0, std::memory_order_relaxed);
    }
    return engaged_;
}

void ExternalProcessInsert::endBlock(float* const* chain, int chainChannels, int numSamples) noexcept
{
    if (! fadingOut_ && gain_ >= 1.0f) return;
    float g = gain_;
    const float step = fadingOut_ ? -gainStep_ : gainStep_;
    for (int i = 0; i < numSamples; ++i)
    {
        for (int c = 0; c < chainChannels; ++c)
            if (chain[c] != nullptr) chain[c][i] *= g;
        g = std::min(1.0f, std::max(0.0f, g + step));
    }
    gain_ = g;
}

void ExternalProcessInsert::retireLocked(ExtProcSession* s)
{
    if (s == nullptr) return;
    if (current_.get() == s) retired_.push_back(std::move(current_));
}

void ExternalProcessInsert::reapLocked()
{
    const auto* used = inUse_.load(std::memory_order_seq_cst);
    const auto* pub  = published_.load(std::memory_order_seq_cst);
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [&](const std::unique_ptr<ExtProcSession>& s)
                                  { return s.get() != used && s.get() != pub; }),
                   retired_.end());
}

dca_status ExternalProcessInsert::open(const char* mappingName, const char* magic)
{
    if (mappingName == nullptr || mappingName[0] == '\0') return DCA_ERR_INVALID_ARG;
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(controlMutex_);
    reapLocked();

    const int wlen = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mappingName, -1, nullptr, 0);
    if (wlen <= 0) return DCA_ERR_INVALID_ARG;
    std::vector<wchar_t> wide((size_t) wlen);
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mappingName, -1, wide.data(), wlen);

    auto session = std::make_unique<ExtProcSession>();
    session->mapping = ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, wide.data());
    if (session->mapping == nullptr) return DCA_ERR_NOT_FOUND;
    session->view = static_cast<uint8_t*>(::MapViewOfFile((HANDLE) session->mapping,
                                                          FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
    if (session->view == nullptr) return DCA_ERR_DEVICE;
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(session->view, &mbi, sizeof(mbi)) == 0) return DCA_ERR_DEVICE;
    session->viewBytes = (size_t) mbi.RegionSize;
    if (session->viewBytes < extproc::kHeaderBytes) return DCA_ERR_DECODE;

    // Header: magic (when the caller names one), format, channel counts. The
    // block size may still be 0 — the other side publishes it with its first
    // rendered block, and the audio thread waits for it.
    if (magic != nullptr)
    {
        char want[8] = {};
        for (size_t i = 0; i < sizeof(want) && magic[i] != '\0'; ++i) want[i] = magic[i];
        if (std::memcmp(session->view + extproc::kOffMagic, want, sizeof(want)) != 0)
            return DCA_ERR_DECODE;
    }
    const uint32_t version = session->header32(extproc::kOffVersion).load(std::memory_order_acquire);
    const uint32_t rate    = session->header32(extproc::kOffSampleRate).load(std::memory_order_relaxed);
    const uint32_t inCh    = session->header32(extproc::kOffInChannels).load(std::memory_order_relaxed);
    const uint32_t outCh   = session->header32(extproc::kOffOutChannels).load(std::memory_order_relaxed);
    if (version != extproc::kFormatVersion || rate < 8000 || rate > 384000 || inCh != 1
        || outCh < 1 || outCh > (uint32_t) extproc::kMaxOutChannels)
        return DCA_ERR_DECODE;
    const uint32_t bf = session->header32(extproc::kOffBlockFrames).load(std::memory_order_relaxed);
    if (bf != 0)
    {
        extproc::Layout probe;
        if (! extproc::computeLayout(bf, session->header32(extproc::kOffRingBlocks).load(std::memory_order_relaxed),
                                     inCh, outCh, session->viewBytes, probe))
            return DCA_ERR_DECODE;
    }

    // The ring carries audio at ITS rate; this build does not convert. The
    // caller requests that rate from the device and opens again.
    const int deviceRate = deviceRate_.load(std::memory_order_relaxed);
    if (deviceRate <= 0) return DCA_ERR_NOT_INITIALIZED;
    if ((uint32_t) deviceRate != rate) return DCA_ERR_SAMPLE_RATE;

    session->sampleRate = (int) rate;
    session->inBlock.assign((size_t) extproc::kMaxBlockFrames, 0.0f);
    session->outBlock.assign((size_t) extproc::kMaxBlockFrames * 2, 0.0f);

    ExtProcSession* old = current_.get();
    retireLocked(old);
    current_ = std::move(session);
    published_.store(current_.get(), std::memory_order_seq_cst);
    reapLocked();
    return DCA_OK;
#else
    (void) magic;
    return DCA_ERR_NOT_FOUND;
#endif
}

bool ExternalProcessInsert::close()
{
    std::unique_lock<std::mutex> lk(controlMutex_);
    ExtProcSession* old = current_.get();
    if (old == nullptr) { reapLocked(); return false; }
    published_.store(nullptr, std::memory_order_seq_cst);
    retireLocked(old);

    // Wait (bounded, on THIS thread — never the audio thread) for the audio
    // thread to dip out of it, so the other process's mapping is released now
    // rather than at the next call. A device that is not calling back leaves
    // it retired; the next control call or the destructor unmaps it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (inUse_.load(std::memory_order_seq_cst) == old
           && deviceRate_.load(std::memory_order_relaxed) > 0
           && std::chrono::steady_clock::now() < deadline)
    {
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lk.lock();
    }
    reapLocked();
    return true;
}

void ExternalProcessInsert::fillStats(dca_extproc_stats& out)
{
    std::lock_guard<std::mutex> lk(controlMutex_);
    reapLocked();
    const ExtProcSession* s = current_.get();
    if (s == nullptr)
    {
        out.state = DCA_EXTPROC_STATE_CLOSED;
        return;
    }
    out.state           = s->state.load(std::memory_order_relaxed);
    out.connected       = s->connected.load(std::memory_order_relaxed);
    out.sample_rate     = s->sampleRate;
    out.block_frames    = s->blockFrames.load(std::memory_order_relaxed);
    out.ring_blocks     = s->ringBlocks.load(std::memory_order_relaxed);
    out.latency_samples = s->latencySamples.load(std::memory_order_relaxed);
    out.blocks_sent     = s->sent.load(std::memory_order_relaxed);
    out.blocks_received = s->received.load(std::memory_order_relaxed);
    out.underruns       = s->underruns.load(std::memory_order_relaxed);
    out.overruns        = s->overruns.load(std::memory_order_relaxed);
    out.late_blocks     = s->late.load(std::memory_order_relaxed);
}

} // namespace dotcharter
