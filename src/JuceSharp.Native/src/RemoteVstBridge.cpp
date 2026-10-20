#include "RemoteVstBridge.h"

#include "RtDiag.h"

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

#include <algorithm>
#include <cstring>

namespace dotcharter {

#if defined(_WIN32)

// The QPC frequency is fixed for the life of the process (documented since
// Windows XP SP2). Read it here, on the message thread, so process() never
// makes the call on the audio thread — it used to, on every waiting block.
RemoteVstBridge::RemoteVstBridge()
{
    LARGE_INTEGER f{};
    ::QueryPerformanceFrequency(&f);
    qpcFreq_ = f.QuadPart > 0 ? f.QuadPart : 1;
}

bool RemoteVstBridge::enable(const char* shmName, const char* requestEventName,
                             const char* responseEventName, int sampleRate, int blockFrames)
{
    if (shmName == nullptr || requestEventName == nullptr || responseEventName == nullptr)
        return false;

    disable(); // takes viewMutex_ itself, so it must not be held here

    std::lock_guard<std::mutex> lk(viewMutex_);

    mapping_ = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, (DWORD) dcvb::kTotalBytes, shmName);
    if (mapping_ == nullptr) return false;
    const bool freshMapping = (::GetLastError() != ERROR_ALREADY_EXISTS);

    // A mapping that already exists keeps the size it was CREATED with, so one
    // left by an older-layout child (v2 is 520 bytes shorter) cannot be viewed
    // at this build's size and fails here: refused, like the version check
    // below, and the caller degrades to in-process hosting.
    view_ = ::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, dcvb::kTotalBytes);
    if (view_ == nullptr) { closeHandlesLocked(); return false; }

    reqEvent_  = ::CreateEventA(nullptr, FALSE, FALSE, requestEventName);
    respEvent_ = ::CreateEventA(nullptr, FALSE, FALSE, responseEventName);
    if (reqEvent_ == nullptr || respEvent_ == nullptr) { closeHandlesLocked(); return false; }

    auto* h = dcvb::header(view_);
    if (freshMapping)
    {
        std::memset(view_, 0, dcvb::kTotalBytes);
    }
    else if (h->magic == dcvb::kMagic &&
             (h->version        != dcvb::kBridgeVersion ||
              h->maxBlockFrames != (uint32_t) dcvb::kMaxBlockFrames ||
              h->maxChannels    != (uint32_t) dcvb::kMaxChannels))
    {
        // Someone else already stamped this mapping with a DIFFERENT layout —
        // i.e. a juce_vst_host.exe from another build is still attached to it.
        // We used to overwrite these fields unconditionally, so the two
        // processes then read and wrote the same bytes at different offsets:
        // silent cross-process memory corruption on the audio path. Refuse
        // instead; the caller degrades to in-process hosting.
        closeHandlesLocked();
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

    // Resume the sequence from whatever is already in the header so a
    // re-enable against a surviving child (or a child restart against our
    // surviving mapping) never re-uses an in-flight sequence number.
    seq_ = h->requestSeq.load(std::memory_order_acquire);
    consecutiveTimeouts_ = 0;

    enabled_.store(true, std::memory_order_release);
    return true;
}

void RemoteVstBridge::disable()
{
    enabled_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(viewMutex_);
    closeHandlesLocked();
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

bool RemoteVstBridge::process(juce::AudioBuffer<float>& chainBuf, int numSamples,
                              const uint32_t* midi, const uint32_t* midiOffsets, int midiCount,
                              int64_t blockStartFrame,
                              const float** instrOutL, const float** instrOutR,
                              bool& chainActive, bool& instrumentActive) noexcept
{
    chainActive = false;
    instrumentActive = false;
    if (instrOutL) *instrOutL = nullptr;
    if (instrOutR) *instrOutR = nullptr;

    if (view_ == nullptr || ! enabled_.load(std::memory_order_acquire))
        return false;
    if (numSamples <= 0 || numSamples > dcvb::kMaxBlockFrames)
        return false;

    auto* h = dcvb::header(view_);

    // ---- write request ----------------------------------------------------
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
    // path this can only fire if a caller hands `process` more than the mapping
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

    const uint32_t seq = ++seq_;
    h->requestSeq.store(seq, std::memory_order_release);
    ::SetEvent((HANDLE) reqEvent_);

    // ---- bounded wait -------------------------------------------------------
    // ONE block of wall clock, clamped to [0.5, 20] ms. After 3 consecutive
    // misses we stop waiting entirely (poll-only) so a dead/wedged child costs
    // the audio thread nothing while managed code restarts it.
    //
    // This used to be a re-arming WaitForSingleObject loop paced by
    // GetTickCount, and that was wrong twice over (finding E-1):
    //
    //  * GetTickCount advances in whole system timer ticks (~15.6 ms by
    //    default, 10 ms at best). EVERY timeout this function computes is below
    //    one tick, so `elapsed` read 0 for the first several iterations and each
    //    one re-armed the FULL timeout instead of the remainder.
    //  * A TIMED kernel wait has that same tick granularity, so even a single
    //    WaitForSingleObject(respEvent_, 10) sleeps to the next tick. Switching
    //    to a high-resolution clock alone would NOT have fixed this.
    //
    // Together they turned a documented "~2 ms" bound into a stall the RT gate
    // measured at 20.5 ms against a 5.33 ms block budget — 4x over, on the
    // first three blocks of every stall, which is exactly what happens on every
    // plugin load in the child (it holds graphMutex_ across prepareToPlay).
    //
    // So: no timed kernel wait at all. Spin on the response SEQUENCE, which is
    // the real completion signal and is already published release/acquire,
    // against a QPC deadline; then drain the auto-reset event non-blockingly.
    // The healthy round trip measures ~14 us, so the spin barely runs, and a
    // wedged child now costs at most one block instead of a timer tick — the
    // callback stays inside its deadline instead of sleeping past it.
    //
    // Waiting two blocks was never useful either: by the time a two-block wait
    // returns, this block's deadline has already gone.
    const int sr = std::max(8000, (int) h->sampleRate.load(std::memory_order_relaxed));
    const double budgetMs = std::clamp((1000.0 * (double) numSamples) / (double) sr, 0.5, 20.0);
    // Poll-only exists so a DEAD child costs the audio thread nothing. It was
    // also a one-way door, and that is the bug: the check below tests the
    // response sequence immediately after posting this block's request, which
    // the child cannot possibly have answered yet, so a poll can never succeed
    // — and consecutiveTimeouts_ is only ever reset ON SUCCESS. Once it latched
    // the bridge was silent for the rest of the session.
    //
    // It latches trivially. The child holds graphMutex_ across prepareToPlay,
    // so LOADING an instrument stalls several blocks by construction; at 128
    // frames / 48 kHz the budget is 2.67 ms and three misses are immediate.
    // That is the reported failure exactly (2026-08-21): the VSTi loads, MIDI
    // still reaches it — the request is written and the event set regardless —
    // and no audio ever comes back. A merely SLOW child also keeps its
    // heartbeat alive, so the managed health timer never restarts it either.
    //
    // So poll-only now re-tries the real synchronous wait periodically. A child
    // that has finished loading answers it, consecutiveTimeouts_ resets, and
    // normal operation resumes; one that is genuinely wedged costs a single
    // bounded spin every ~0.7 s.
    const bool   pollOnly = consecutiveTimeouts_ >= 3
                            && (++pollOnlyBlocks_ % kRetrySyncEveryBlocks) != 0;

    bool gotResponse = h->responseSeq.load(std::memory_order_acquire) == seq;
    if (! gotResponse && ! pollOnly)
    {
        // The RT gate measures how long the audio thread actually spends here
        // (assertion #10). Compiles to nothing in the shipped DLL.
        DCA_RT_WAIT_SCOPE("bridge/responseWait");

        // qpcFreq_ is read ONCE, in the constructor: the frequency is fixed
        // for the life of the process, and this used to query it on every
        // waiting block — on the audio thread, for a constant.
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);
        const long long deadline = now.QuadPart
            + (long long) ((budgetMs / 1000.0) * (double) qpcFreq_);

        // Read the (cheap, user-mode) clock every 256 spins rather than every
        // one: the sequence load is the latency-critical check.
        for (int i = 0; ; ++i)
        {
            if (h->responseSeq.load(std::memory_order_acquire) == seq) { gotResponse = true; break; }
            if ((i & 0xFF) == 0xFF)
            {
                ::QueryPerformanceCounter(&now);
                if (now.QuadPart >= deadline) break;
            }
            YieldProcessor();
        }
    }

    if (gotResponse)
    {
        // Drain the auto-reset response event without ever blocking, so this
        // block's SetEvent cannot leak into the next one. Nothing waits on it
        // with a timeout any more; it is now purely a wake hint.
        ::WaitForSingleObject((HANDLE) respEvent_, 0);
    }

    if (! gotResponse)
    {
        ++consecutiveTimeouts_;
        timeouts_.fetch_add(1, std::memory_order_relaxed);
        return false; // caller keeps the dry buffer
    }

    consecutiveTimeouts_ = 0;
    pollOnlyBlocks_ = 0;
    processed_.fetch_add(1, std::memory_order_relaxed);

    // ---- read response ------------------------------------------------------
    chainActive      = h->chainActive != 0;
    instrumentActive = h->instrumentActive != 0;
    if (chainActive)
    {
        for (int ch = 0; ch < channels; ++ch)
            std::memcpy(chainBuf.getWritePointer(ch), dcvb::chainOutChannel(view_, ch),
                        (size_t) numSamples * sizeof(float));
    }
    if (instrumentActive)
    {
        if (instrOutL) *instrOutL = dcvb::instrOutChannel(view_, 0);
        if (instrOutR) *instrOutR = dcvb::instrOutChannel(view_, 1);
    }
    return true;
}

#else // non-Windows: bridge unavailable (sandboxing is a Windows feature for now)

RemoteVstBridge::RemoteVstBridge() = default;
bool RemoteVstBridge::enable(const char*, const char*, const char*, int, int) { return false; }
void RemoteVstBridge::disable() { enabled_.store(false, std::memory_order_release); }
void RemoteVstBridge::closeHandlesLocked() {}
void RemoteVstBridge::setDevicePrepare(int, int) noexcept {}
uint32_t RemoteVstBridge::lastHeartbeat() const noexcept { return 0; }
bool RemoteVstBridge::process(juce::AudioBuffer<float>&, int, const uint32_t*, const uint32_t*, int,
                              int64_t, const float**, const float**, bool&, bool&) noexcept { return false; }

#endif

} // namespace dotcharter
