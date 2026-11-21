#include "RemoteVstBridge.h"

#include "RtDiag.h"

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <utility>

namespace dotcharter {

#if defined(_WIN32)

// The QPC frequency is fixed for the life of the process (documented since
// Windows XP SP2). Read it here, on the message thread, so collect() never
// makes the call on the audio thread.
RemoteVstBridge::RemoteVstBridge()
{
    LARGE_INTEGER f{};
    ::QueryPerformanceFrequency(&f);
    qpcFreq_ = f.QuadPart > 0 ? f.QuadPart : 1;
}

bool RemoteVstBridge::prepare(const char* shmName, const char* requestEventName,
                              const char* responseEventName, int sampleRate, int blockFrames,
                              Prepared& out)
{
    out = Prepared{};
    if (shmName == nullptr || requestEventName == nullptr || responseEventName == nullptr)
        return false;

    Prepared p;
    p.mapping = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                     0, (DWORD) dcvb::kTotalBytes, shmName);
    if (p.mapping == nullptr) return false;
    const bool freshMapping = (::GetLastError() != ERROR_ALREADY_EXISTS);

    // A mapping that already exists keeps the size it was CREATED with, so one
    // left by an older-layout child (v3 is 8 bytes shorter) cannot be viewed
    // at this build's size and fails here: refused, like the version check
    // below, and the caller degrades to in-process hosting.
    p.view = ::MapViewOfFile(p.mapping, FILE_MAP_ALL_ACCESS, 0, 0, dcvb::kTotalBytes);
    if (p.view == nullptr) { release(p); return false; }

    p.reqEvent  = ::CreateEventA(nullptr, FALSE, FALSE, requestEventName);
    p.respEvent = ::CreateEventA(nullptr, FALSE, FALSE, responseEventName);
    if (p.reqEvent == nullptr || p.respEvent == nullptr) { release(p); return false; }

    auto* h = dcvb::header(p.view);
    if (freshMapping)
    {
        std::memset(p.view, 0, dcvb::kTotalBytes);
    }
    else if (h->magic == dcvb::kMagic &&
             (h->version        != dcvb::kBridgeVersion ||
              h->maxBlockFrames != (uint32_t) dcvb::kMaxBlockFrames ||
              h->maxChannels    != (uint32_t) dcvb::kMaxChannels))
    {
        // Someone else already stamped this mapping with a DIFFERENT layout,
        // i.e. a juce_vst_host.exe from another build is still attached to it.
        // Writing these fields would make the two processes read and write the
        // same bytes at different offsets: silent cross-process memory
        // corruption on the audio path. Refused; the caller degrades to
        // in-process hosting.
        release(p);
        return false;
    }

    h->magic          = dcvb::kMagic;
    h->version        = dcvb::kBridgeVersion;
    h->maxBlockFrames = (uint32_t) dcvb::kMaxBlockFrames;
    h->maxChannels    = (uint32_t) dcvb::kMaxChannels;
    h->sampleRate.store(sampleRate, std::memory_order_relaxed);
    if (blockFrames > 0)
        h->prepareBlockFrames.store((uint32_t) blockFrames, std::memory_order_relaxed);
    // Publishes the pair. The child latches this generation in openBridge, so
    // arming the bridge is not itself mistaken for a device change.
    h->prepareGeneration.fetch_add(1, std::memory_order_release);
#if DCA_RTCHECK
    if (preparedSeam_ != nullptr) preparedSeam_(preparedSeamUser_);
#endif

    out = p;
    return true;
}

void RemoteVstBridge::arm(Prepared& p) noexcept
{
    enabled_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(viewMutex_);
    std::swap(mapping_,   p.mapping);
    std::swap(view_,      p.view);
    std::swap(reqEvent_,  p.reqEvent);
    std::swap(respEvent_, p.respEvent);

    // Nothing posted to the replaced mapping is ever read back.
    pending_          = false;
    pendingBuf_       = nullptr;
    answered_         = false;
    chainActive_      = false;
    instrumentActive_ = false;
    instrL_           = nullptr;
    instrR_           = nullptr;

    // Resume the sequence from whatever is already in the header so a re-arm
    // against a surviving child (or a child restart against our surviving
    // mapping) never re-uses an in-flight sequence number. Read here, under
    // the barrier, because the audio thread may have advanced it on this same
    // mapping since prepare(). A kRequestWriting left there resumes at 1
    // (nextRequestSeq skips it).
    seq_ = dcvb::header(view_)->requestSeq.load(std::memory_order_acquire);
    consecutiveTimeouts_ = 0;
    pollOnlyBlocks_ = 0;
    cbOpen_ = false;

    enabled_.store(true, std::memory_order_release);
}

void RemoteVstBridge::release(Prepared& p) noexcept
{
    if (p.view      != nullptr) { ::UnmapViewOfFile(p.view); p.view = nullptr; }
    if (p.mapping   != nullptr) { ::CloseHandle(p.mapping);  p.mapping = nullptr; }
    if (p.reqEvent  != nullptr) { ::CloseHandle(p.reqEvent); p.reqEvent = nullptr; }
    if (p.respEvent != nullptr) { ::CloseHandle(p.respEvent); p.respEvent = nullptr; }
}

bool RemoteVstBridge::enable(const char* shmName, const char* requestEventName,
                             const char* responseEventName, int sampleRate, int blockFrames)
{
    Prepared p;
    if (! prepare(shmName, requestEventName, responseEventName, sampleRate, blockFrames, p))
    {
        disable();
        return false;
    }
    arm(p);
    release(p);
    return true;
}

void RemoteVstBridge::disable()
{
    enabled_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(viewMutex_);
    closeHandlesLocked();
    // A request posted to the mapping that is now gone is never read back:
    // the engine holds its bridge barrier here, so no collect() is running,
    // and the one after this sees nothing pending (the seat stays dry).
    pending_          = false;
    pendingBuf_       = nullptr;
    answered_         = false;
    chainActive_      = false;
    instrumentActive_ = false;
    instrL_           = nullptr;
    instrR_           = nullptr;
}

void RemoteVstBridge::closeHandlesLocked()
{
    if (view_      != nullptr) { ::UnmapViewOfFile(view_); view_ = nullptr; }
    if (mapping_   != nullptr) { ::CloseHandle(mapping_);  mapping_ = nullptr; }
    if (reqEvent_  != nullptr) { ::CloseHandle(reqEvent_); reqEvent_ = nullptr; }
    if (respEvent_ != nullptr) { ::CloseHandle(respEvent_); respEvent_ = nullptr; }
}

void RemoteVstBridge::setDevicePrepare(int sr, int blockFrames) noexcept
{
    // Device thread, outside the AudioEngine's bridge barrier.
    std::lock_guard<std::mutex> lk(viewMutex_);
    if (view_ == nullptr || ! enabled_.load(std::memory_order_acquire)) return;
    auto* h = dcvb::header(view_);
    if (sr > 0)          h->sampleRate.store(sr, std::memory_order_relaxed);
    if (blockFrames > 0) h->prepareBlockFrames.store((uint32_t) blockFrames, std::memory_order_relaxed);
    // The generation is the RELEASE that publishes both fields at once. A child
    // that saw a torn pair would re-prepare its plugins for a configuration
    // that never existed.
    h->prepareGeneration.fetch_add(1, std::memory_order_release);
}

uint32_t RemoteVstBridge::lastHeartbeat() const noexcept
{
    // Polled by the managed sandbox health timer, outside the barrier — this
    // is the one that would actually have crashed: disabling the bridge (VST
    // sandbox off, or engine teardown) unmaps the view while the timer is
    // reading the child's heartbeat out of it.
    std::lock_guard<std::mutex> lk(viewMutex_);
    if (view_ == nullptr || ! enabled_.load(std::memory_order_acquire)) return 0;
    return dcvb::header(view_)->heartbeat.load(std::memory_order_acquire);
}

bool RemoteVstBridge::post(juce::AudioBuffer<float>& chainBuf, int numSamples,
                           const uint32_t* midi, const uint32_t* midiOffsets, int midiCount,
                           int64_t blockStartFrame, int seat, uint32_t callbackSerial) noexcept
{
    if (view_ == nullptr || ! enabled_.load(std::memory_order_acquire))
        return false;
    if (numSamples <= 0 || numSamples > dcvb::kMaxBlockFrames)
        return false;
    if (seat < 0 || seat >= dcvb::kMaxSeats)
        return false;

    auto* h = dcvb::header(view_);

    // ---- one request per CALLBACK (#646, #696) ------------------------------
    // The miss count is per callback, so poll-only starts after the usual
    // number of missed CALLBACKS. A bridge serves one seat's child, so one
    // request per callback is all it ever has to carry; a second would
    // overwrite the first under a child that may still be reading it.
    beginCallbackIfNew(callbackSerial);
    if (cbPosted_) return false;
    // A request collect() never settled (the engine always collects what it
    // posted, so this is a guard, not a path) is a miss of the callback it
    // belonged to, which beginCallbackIfNew has just folded.
    if (pending_) settle(false);
    cbPosted_ = true;
    answered_ = chainActive_ = instrumentActive_ = false;
    instrL_ = instrR_ = nullptr;

    // ---- write request ----------------------------------------------------
    // requestSeq reads kRequestWriting for the whole overwrite, so a late child
    // still copying the previous request drops it (dotcharter_vst_bridge.h).
    dcvb::beginRequestWrite(h);
    const int channels = std::min(chainBuf.getNumChannels(), dcvb::kMaxChannels);
    for (int ch = 0; ch < channels; ++ch)
        std::memcpy(dcvb::inputChannel(view_, ch), chainBuf.getReadPointer(ch),
                    (size_t) numSamples * sizeof(float));

    h->blockFrames   = (uint32_t) numSamples;
    h->inputChannels = (uint32_t) channels;
    // TRUNCATION IS COUNTED (same family as W4-N7). The shared mapping carries
    // kMaxMidiEvents slots; anything past that is discarded, and a discarded
    // note-off is a stuck note in the child's instrument. The ENGINE also caps
    // its carry buffer at the same figure and counts there, so in the normal
    // path this can only fire if a caller hands `post` more than the mapping
    // holds directly — which is exactly the case nothing would otherwise say a
    // word about.
    const int n = std::min(midiCount, dcvb::kMaxMidiEvents);
    if (midiCount > n)
        midiTruncated_.fetch_add((uint32_t) (midiCount - n), std::memory_order_relaxed);
    // Bridge v3: each event's sample offset travels beside it, so a sandboxed
    // instrument plays a scheduled note on the sample the in-process one would
    // (JuceSharp #28). Written before requestSeq's release, like midi[].
    auto* timing = dcvb::midiTiming(view_);
    timing->blockStartFrame = blockStartFrame;
    for (int i = 0; i < n; ++i)
    {
        h->midi[i] = midi[i];
        const uint32_t o = midiOffsets != nullptr ? midiOffsets[i] : 0u;
        timing->midiOffset[i] = o < (uint32_t) numSamples ? o : (uint32_t) (numSamples - 1);
    }
    h->midiCount = (uint32_t) n;
    // Bridge v4 (#646): whose chain this block is. Written before requestSeq's
    // release, like the rest of the request.
    dcvb::seatRouting(view_)->requestSeat = (uint32_t) seat;

    seq_ = dcvb::nextRequestSeq(seq_);
    dcvb::publishRequest(h, seq_);
    ::SetEvent((HANDLE) reqEvent_);

    // ONE block of wall clock, clamped to [0.5, 20] ms: what collect() may
    // wait for this request (see there).
    const int sr = std::max(8000, (int) h->sampleRate.load(std::memory_order_relaxed));
    const double budgetMs = std::clamp((1000.0 * (double) numSamples) / (double) sr, 0.5, 20.0);
    budgetTicks_     = (long long) ((budgetMs / 1000.0) * (double) qpcFreq_);
    pendingSeq_      = seq_;
    pendingFrames_   = numSamples;
    pendingChannels_ = channels;
    pendingBuf_      = &chainBuf;
    pending_         = true;
    return true;
}

bool RemoteVstBridge::responseArrived() const noexcept
{
    return dcvb::header(view_)->responseSeq.load(std::memory_order_acquire) == pendingSeq_;
}

void RemoteVstBridge::settle(bool gotResponse) noexcept
{
    pending_ = false;
    auto* buf = pendingBuf_;
    pendingBuf_ = nullptr;
    if (! gotResponse)
    {
        markCallbackMissed();
        return; // the seat keeps its dry buffer
    }

    // Drain the auto-reset response event without ever blocking, so this
    // request's SetEvent cannot leak into the next one. Nothing waits on it
    // with a timeout; it is purely a wake hint.
    ::WaitForSingleObject((HANDLE) respEvent_, 0);

    cbAnswered_ = true;
    answered_   = true;
    processed_.fetch_add(1, std::memory_order_relaxed);

    // ---- read response ------------------------------------------------------
    auto* h = dcvb::header(view_);
    chainActive_      = h->chainActive != 0;
    instrumentActive_ = h->instrumentActive != 0;
    if (chainActive_ && buf != nullptr)
    {
        for (int ch = 0; ch < pendingChannels_; ++ch)
            std::memcpy(buf->getWritePointer(ch), dcvb::chainOutChannel(view_, ch),
                        (size_t) pendingFrames_ * sizeof(float));
    }
    if (instrumentActive_)
    {
        instrL_ = dcvb::instrOutChannel(view_, 0);
        instrR_ = dcvb::instrOutChannel(view_, 1);
    }
}

void RemoteVstBridge::collect(RemoteVstBridge* const* bridges, int count) noexcept
{
    // ---- one deadline for every seat's child (#696) --------------------------
    // Each seat has a child of its own, and every seat's request is posted
    // before this runs, so the children work at the same time and the callback
    // waits ONE block for all of them: the deadline is one block budget after
    // the collection starts, and every pass of the spin checks every request
    // still outstanding. A child that has not answered by the deadline costs
    // its OWN seat a dry block and a miss; it never stops the others being
    // collected, and it never makes the callback wait a second block.
    //
    // A bridge in poll-only exists so a DEAD child costs the audio thread
    // nothing: it is checked on every pass like the others but never holds the
    // collection open. Poll-only is not a one-way door either: the child holds
    // graphMutex_ across prepareToPlay, so LOADING an instrument stalls several
    // blocks by construction and three misses latch poll-only at once (the
    // 2026-08-21 report: the VSTi loads, MIDI still reaches it, no audio ever
    // came back). A poll that is answered — while the collection waits for
    // another seat, or on the full synchronous wait poll-only retries every
    // kRetrySyncEveryBlocks callbacks — counts as an answered callback, which
    // resets the miss count, and normal operation resumes. A child that is
    // genuinely wedged costs a single bounded spin every ~0.7 s.
    //
    // The wait was once a re-arming WaitForSingleObject loop paced by
    // GetTickCount, and that was wrong twice over (finding E-1):
    //
    //  * GetTickCount advances in whole system timer ticks (~15.6 ms by
    //    default, 10 ms at best). EVERY timeout here is below one tick, so
    //    `elapsed` read 0 for the first several iterations and each one
    //    re-armed the FULL timeout instead of the remainder.
    //  * A TIMED kernel wait has that same tick granularity, so even a single
    //    WaitForSingleObject(respEvent_, 10) sleeps to the next tick. Switching
    //    to a high-resolution clock alone would NOT have fixed this.
    //
    // Together they turned a documented "~2 ms" bound into a stall the RT gate
    // measured at 20.5 ms against a 5.33 ms block budget — 4x over, on the
    // first three blocks of every stall, which is exactly what happens on every
    // plugin load in the child (it holds graphMutex_ across prepareToPlay).
    //
    // So: no timed kernel wait at all. Spin on the response SEQUENCES, which
    // are the real completion signals and are already published
    // release/acquire, against a QPC deadline; then drain each answered
    // auto-reset event non-blockingly. The healthy round trip measures ~14 us,
    // so the spin barely runs, and wedged children cost at most one block in
    // total instead of a timer tick.
    if (bridges == nullptr || count <= 0) return;

    int       waiting     = 0;
    long long budgetTicks = 0;
    for (int i = 0; i < count; ++i)
    {
        auto* b = bridges[i];
        if (b == nullptr || ! b->pending_) continue;
        if (b->responseArrived()) { b->settle(true); continue; }
        if (! b->cbPollOnly_)
        {
            ++waiting;
            budgetTicks = std::max(budgetTicks, b->budgetTicks_);
        }
    }

    if (waiting > 0)
    {
        // The RT gate measures how long the audio thread actually spends here
        // (assertion #10): one sample per callback, the whole collection.
        // Compiles to nothing in the shipped DLL.
        DCA_RT_WAIT_SCOPE("bridge/responseWait");

        // qpcFreq_ was read ONCE, in each bridge's constructor: the frequency
        // is fixed for the life of the process.
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);
        const long long deadline = now.QuadPart + budgetTicks;

        // Read the (cheap, user-mode) clock every 256 passes rather than every
        // one: the sequence loads are the latency-critical checks.
        for (int pass = 0; waiting > 0; ++pass)
        {
            for (int i = 0; i < count; ++i)
            {
                auto* b = bridges[i];
                if (b == nullptr || ! b->pending_ || ! b->responseArrived()) continue;
                if (! b->cbPollOnly_) --waiting;
                b->settle(true);
            }
            if (waiting == 0) break;
            if ((pass & 0xFF) == 0xFF)
            {
                ::QueryPerformanceCounter(&now);
                if (now.QuadPart >= deadline) break;
            }
            YieldProcessor();
        }
    }

    // ---- at the deadline: one last scan --------------------------------------
    // Whatever answered in the meantime is taken; the rest stay dry for this
    // callback and count a miss on their own bridge.
    for (int i = 0; i < count; ++i)
    {
        auto* b = bridges[i];
        if (b == nullptr || ! b->pending_) continue;
        b->settle(b->responseArrived());
    }
}

void RemoteVstBridge::beginCallbackIfNew(uint32_t callbackSerial) noexcept
{
    if (cbOpen_ && callbackSerial == cbSerial_) return;
    if (cbOpen_)
    {
        // The previous callback's outcome, folded once: a missed request is
        // ONE missed callback; an answered one (a poll included) is a success
        // and ends a poll-only stretch.
        if (cbMissed_)        ++consecutiveTimeouts_;
        else if (cbAnswered_) { consecutiveTimeouts_ = 0; pollOnlyBlocks_ = 0; }
    }
    cbOpen_          = true;
    cbSerial_        = callbackSerial;
    cbMissed_        = false;
    cbAnswered_      = false;
    cbPosted_        = false;
    cbPollOnly_      = consecutiveTimeouts_ >= kPollOnlyAfterMissedCallbacks
                       && (++pollOnlyBlocks_ % kRetrySyncEveryBlocks) != 0;
}

void RemoteVstBridge::markCallbackMissed() noexcept
{
    if (cbMissed_) return;
    cbMissed_ = true;
    timeouts_.fetch_add(1, std::memory_order_relaxed);
}

bool RemoteVstBridge::seatHasChain(int seat) const noexcept
{
    if (view_ == nullptr || ! enabled_.load(std::memory_order_acquire)) return false;
    if (seat < 0 || seat >= dcvb::kMaxSeats) return false;
    const uint32_t mask = dcvb::seatRouting(view_)->seatChainMask.load(std::memory_order_acquire);
    return (mask & (1u << (uint32_t) seat)) != 0;
}

#else // non-Windows: bridge unavailable (sandboxing is a Windows feature for now)

RemoteVstBridge::RemoteVstBridge() = default;
bool RemoteVstBridge::enable(const char*, const char*, const char*, int, int) { return false; }
bool RemoteVstBridge::prepare(const char*, const char*, const char*, int, int, Prepared& out) { out = Prepared{}; return false; }
void RemoteVstBridge::arm(Prepared&) noexcept {}
void RemoteVstBridge::release(Prepared&) noexcept {}
void RemoteVstBridge::disable() { enabled_.store(false, std::memory_order_release); }
void RemoteVstBridge::closeHandlesLocked() {}
void RemoteVstBridge::setDevicePrepare(int, int) noexcept {}
uint32_t RemoteVstBridge::lastHeartbeat() const noexcept { return 0; }
bool RemoteVstBridge::post(juce::AudioBuffer<float>&, int, const uint32_t*, const uint32_t*, int,
                           int64_t, int, uint32_t) noexcept { return false; }
void RemoteVstBridge::collect(RemoteVstBridge* const*, int) noexcept {}
bool RemoteVstBridge::responseArrived() const noexcept { return false; }
void RemoteVstBridge::settle(bool) noexcept { pending_ = false; }
bool RemoteVstBridge::seatHasChain(int) const noexcept { return false; }
void RemoteVstBridge::beginCallbackIfNew(uint32_t) noexcept {}
void RemoteVstBridge::markCallbackMissed() noexcept {}

#endif

} // namespace dotcharter
