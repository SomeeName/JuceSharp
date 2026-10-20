// VstHostMain.cpp
//
// juce_vst_host.exe — DotCharter's sandboxed out-of-process VST3 host.
//
// One instance hosts the guitar FX chain and/or the MIDI instrument VSTi for
// a single parent process. Audio is bridged over the shared-memory block
// described in dotcharter_vst_bridge.h; control (load/unload/state/editor/
// prepare/quit) arrives as newline-delimited UTF-8 JSON over a named pipe
// the parent created before spawning us. Plugin editor windows are owned by
// THIS process (they are ordinary top-level HWNDs).
//
// Crash philosophy: if a plugin access-violates, this process dies — that is
// the entire point. The parent detects the exit (plus a stalled shared-memory
// heartbeat), passes audio through dry, and respawns us with the persisted
// chain. We therefore never install crash handlers that "keep going".
//
// Command line (all key=value, order-free):
//   shm=<mapping> req=<event> resp=<event> pipe=<pipe-name>
//   sr=<sample-rate> block=<frames> parent=<pid>

#include <limits>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "dotcharter_vst_bridge.h"

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

void logLine(const juce::String& msg)
{
    // stderr is captured by the parent and forwarded into its app log.
    std::fprintf(stderr, "[vst_host] %s\n", msg.toRawUTF8());
    std::fflush(stderr);
}

juce::String argValue(const juce::StringArray& args, const juce::String& key)
{
    for (const auto& a : args)
        if (a.startsWith(key + "="))
            return a.fromFirstOccurrenceOf("=", false, false).unquoted();
    return {};
}

// ---------------------------------------------------------------------------
// Editor window (top-level, owned by this process).
// ---------------------------------------------------------------------------
struct HostEditorWindow : public juce::DocumentWindow
{
    HostEditorWindow(juce::AudioPluginInstance& plug, juce::AudioProcessorEditor* ed,
                     std::function<void()> onUserClose)
        : juce::DocumentWindow(plug.getName(), juce::Colours::black,
                               juce::DocumentWindow::closeButton)
        , editor_(ed)
        , onUserClose_(std::move(onUserClose))
    {
        setUsingNativeTitleBar(true);
        if (editor_ != nullptr)
        {
            setResizable(editor_->isResizable(), false);
            setContentNonOwned(editor_.get(), true);
        }
        else
        {
            setSize(400, 200);
        }
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        toFront(true);
    }

    ~HostEditorWindow() override
    {
        setVisible(false);
        clearContentComponent();
        editor_.reset(); // host-owned editor MUST be deleted (clears activeEditor)
    }

    void closeButtonPressed() override
    {
        setVisible(false);
        if (onUserClose_) onUserClose_();
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor_;
    std::function<void()> onUserClose_;
};

// ---------------------------------------------------------------------------
// One hosted chain slot / the instrument.
// ---------------------------------------------------------------------------
struct HostedPlugin
{
    juce::String                                slotId;
    juce::String                                path;
    std::unique_ptr<juce::AudioPluginInstance>  instance;
    std::unique_ptr<HostEditorWindow>           editor;   // MM thread only
    std::atomic<bool>                           bypass{false};
};

// ---------------------------------------------------------------------------
// The host application.
// ---------------------------------------------------------------------------
class VstHostApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "DotCharter VST Host"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise(const juce::String&) override
    {
        const auto args = juce::StringArray::fromTokens(
            juce::JUCEApplication::getCommandLineParameters(), true);

        shmName_   = argValue(args, "shm");
        reqName_   = argValue(args, "req");
        respName_  = argValue(args, "resp");
        pipeName_  = argValue(args, "pipe");
        sampleRate_ = argValue(args, "sr").getIntValue();
        blockSize_  = argValue(args, "block").getIntValue();
        parentPid_  = (uint32_t) argValue(args, "parent").getLargeIntValue();
        if (sampleRate_ <= 0) sampleRate_ = 44100;
        if (blockSize_  <= 0) blockSize_  = 512;

        if (shmName_.isEmpty() || reqName_.isEmpty() || respName_.isEmpty() || pipeName_.isEmpty())
        {
            logLine("missing required arguments (shm/req/resp/pipe)");
            setApplicationReturnValue(2);
            quit();
            return;
        }

        vst3_ = std::make_unique<juce::VST3PluginFormat>();

        if (! openBridge())
        {
            logLine("failed to open shared-memory bridge '" + shmName_ + "'");
            setApplicationReturnValue(2);
            quit();
            return;
        }

        if (! openPipe())
        {
            logLine("failed to connect control pipe '" + pipeName_ + "'");
            setApplicationReturnValue(2);
            quit();
            return;
        }

        startParentWatchdog();

        running_.store(true, std::memory_order_release);
        audioThread_ = std::thread([this] { audioWorkerLoop(); });

        dcvb::header(view_)->childState.store(dcvb::kChildReady, std::memory_order_release);

        // `hello` goes out BEFORE the pipe reader starts. The control pipe is a
        // SYNCHRONOUS handle and Windows serialises synchronous I/O on one
        // handle: with the reader already parked in its blocking ReadFile, this
        // WriteFile waited for a read only the parent could complete — and a
        // parent that waits for `hello` before it speaks (the lifecycle test,
        // and any version-checking parent) deadlocked with it (4 launches in 70,
        // docs/agents/rt-safety-and-tests.md). Nothing reads the pipe yet, so
        // the write completes into the pipe buffer.
        auto hello = std::make_unique<juce::DynamicObject>();
        hello->setProperty("op",  "hello");
       #if defined(_WIN32)
        hello->setProperty("pid", (juce::int64) ::GetCurrentProcessId());
       #else
        hello->setProperty("pid", 0);
       #endif
        hello->setProperty("bridgeVersion", (int) dcvb::kBridgeVersion);
        writePipeLine(juce::JSON::toString(juce::var(hello.release()), true));

        pipeThread_ = std::thread([this] { pipeReaderLoop(); });

        logLine("ready (sr=" + juce::String(sampleRate_) + ", block=" + juce::String(blockSize_) + ")");
    }

    void shutdown() override
    {
        running_.store(false, std::memory_order_release);
       #if defined(_WIN32)
        if (reqEvent_ != nullptr) ::SetEvent((HANDLE) reqEvent_); // wake the worker
       #endif
        if (audioThread_.joinable()) audioThread_.join();
        closePipe();
        if (pipeThread_.joinable()) pipeThread_.join();

        // Strict teardown order on the message thread (we ARE the message
        // thread here): editors first, then instances.
        teardownAllPlugins();
        closeBridge();
        logLine("shut down cleanly");
    }

    void systemRequestedQuit() override { quit(); }

private:
    // ---- bridge ------------------------------------------------------------
    bool openBridge()
    {
       #if defined(_WIN32)
        mapping_ = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, (DWORD) dcvb::kTotalBytes, shmName_.toRawUTF8());
        if (mapping_ == nullptr) return false;
        view_ = ::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, dcvb::kTotalBytes);
        if (view_ == nullptr)
        {
            // An existing mapping keeps the size it was CREATED with, so a
            // parent built against a shorter layout (v2 has no MIDI-timing
            // block) cannot be viewed at this build's size. Say so with the
            // version it carries instead of a bare "failed to open".
            if (void* head = ::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, dcvb::kHeaderBytes))
            {
                const auto* hh = dcvb::header(head);
                if (hh->magic == dcvb::kMagic)
                    logLine("bridge layout mismatch: header v" + juce::String((int) hh->version)
                            + " vs this build v" + juce::String((int) dcvb::kBridgeVersion)
                            + " (mapping too small for this layout)");
                ::UnmapViewOfFile(head);
            }
            return false;
        }
        reqEvent_  = ::CreateEventA(nullptr, FALSE, FALSE, reqName_.toRawUTF8());
        respEvent_ = ::CreateEventA(nullptr, FALSE, FALSE, respName_.toRawUTF8());
        if (reqEvent_ == nullptr || respEvent_ == nullptr) return false;
        auto* h = dcvb::header(view_);
        if (h->magic != dcvb::kMagic)
        {
            // Parent should have initialised the header before spawning us;
            // tolerate a race by stamping the immutable fields ourselves.
            h->magic          = dcvb::kMagic;
            h->version        = dcvb::kBridgeVersion;
            h->maxBlockFrames = (uint32_t) dcvb::kMaxBlockFrames;
            h->maxChannels    = (uint32_t) dcvb::kMaxChannels;
        }
        else if (h->version        != dcvb::kBridgeVersion ||
                 h->maxBlockFrames != (uint32_t) dcvb::kMaxBlockFrames ||
                 h->maxChannels    != (uint32_t) dcvb::kMaxChannels)
        {
            // The parent that owns this mapping was built against a different
            // bridge layout. Nothing checked this before, so a stale host exe
            // would happily read and write the shared block at the wrong
            // offsets. Bail out — the parent falls back to in-process hosting.
            logLine("bridge layout mismatch: header v" + juce::String((int) h->version)
                    + " (block=" + juce::String((int) h->maxBlockFrames)
                    + ", ch=" + juce::String((int) h->maxChannels)
                    + ") vs this build v" + juce::String((int) dcvb::kBridgeVersion)
                    + " (block=" + juce::String((int) dcvb::kMaxBlockFrames)
                    + ", ch=" + juce::String((int) dcvb::kMaxChannels) + ")");
            return false;
        }
        // Bridge v2: the parent publishes rate AND block, with a generation
        // counter as the release. Latch both here so ARMING the bridge is not
        // itself mistaken for a device change by the worker below.
        lastPrepareGen_ = h->prepareGeneration.load(std::memory_order_acquire);
        const int sr  = h->sampleRate.load(std::memory_order_relaxed);
        const int blk = (int) h->prepareBlockFrames.load(std::memory_order_relaxed);
        if (sr  > 0) sampleRate_ = sr;
        if (blk > 0) blockSize_  = blk;
        return true;
       #else
        return false;
       #endif
    }

    void closeBridge()
    {
       #if defined(_WIN32)
        if (view_      != nullptr) { ::UnmapViewOfFile(view_); view_ = nullptr; }
        if (mapping_   != nullptr) { ::CloseHandle(mapping_);  mapping_ = nullptr; }
        if (reqEvent_  != nullptr) { ::CloseHandle(reqEvent_); reqEvent_ = nullptr; }
        if (respEvent_ != nullptr) { ::CloseHandle(respEvent_); respEvent_ = nullptr; }
       #endif
    }

    // ---- audio worker --------------------------------------------------------
    void audioWorkerLoop()
    {
       #if defined(_WIN32)
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
       #endif
        // Flush-to-zero / denormals-are-zero for the whole worker's lifetime
        // (MXCSR is per-thread). Plugin tails decay into subnormals, whose
        // arithmetic is orders of magnitude slower — and the PARENT audio
        // thread blocks on this thread's response, so a denormal storm here
        // is an RT stall by proxy (2026-07-16 audit #10). Mirrors the
        // ScopedNoDenormals the parent's own callback establishes.
        const juce::ScopedNoDenormals noDenormals;
        auto* h = dcvb::header(view_);
        uint32_t lastSeq = h->requestSeq.load(std::memory_order_acquire);
        juce::AudioBuffer<float> chainBuf(dcvb::kMaxChannels, dcvb::kMaxBlockFrames);
        juce::MidiBuffer midi, emptyMidi;
        // Room for a full block of events up front (a 3-byte short is 16 bytes
        // in a MidiBuffer, header included), so addEvent below never grows the
        // buffer on the worker the parent's audio callback is waiting for.
        midi.ensureSize((size_t) dcvb::kMaxMidiEvents * 16);
        const auto& timing = *dcvb::midiTiming(view_);

        while (running_.load(std::memory_order_acquire))
        {
           #if defined(_WIN32)
            ::WaitForSingleObject((HANDLE) reqEvent_, 100);
           #endif
            h->heartbeat.fetch_add(1, std::memory_order_relaxed);

            // THE DEVICE CONFIGURATION CAN CHANGE UNDER US. It was read once,
            // in openBridge, so after a parent-side dca_engine_reconfigure
            // every plugin in this process went on running at the rate the
            // child was LAUNCHED with, and one prepared for 256 frames could be
            // handed 1024 — which entitles it to allocate, on the very thread
            // the parent's audio callback is blocked waiting for.
            //
            // prepareToPlay is NEVER called from this worker: it takes
            // graphMutex_ (which this loop holds while processing) and can take
            // SECONDS for a big amp sim. It is posted to the message thread,
            // and until it lands every block is answered with silence, which
            // the parent already handles as "pass audio through dry".
            const uint32_t gen = h->prepareGeneration.load(std::memory_order_acquire);
            if (gen != lastPrepareGen_)
            {
                lastPrepareGen_ = gen;
                const int sr  = h->sampleRate.load(std::memory_order_relaxed);
                int       blk = (int) h->prepareBlockFrames.load(std::memory_order_relaxed);
                // CLAMP. RemoteVstBridge::enable only stores blockFrames when
                // it is positive but bumps the generation regardless, so a
                // parent that never published one gets us here with blk == 0 —
                // and prepareAll(sr, 0) re-prepares every plugin for a
                // zero-frame block. openBridge guards this with `if (blk > 0)`;
                // this path did not. Keeping the last known good size is what
                // that guard means.
                if (blk <= 0) blk = blockSize_;
                if (! shuttingDown_.load(std::memory_order_acquire))
                {
                    repreparing_.store(true, std::memory_order_release);
                    // Cleared on EVERY exit of the posted lambda, not only the
                    // happy one: `repreparing_` answers every block with
                    // silence while it is set, so a prepareAll that throws left
                    // a wedged child answering silence for ever.
                    juce::MessageManager::callAsync([this, sr, blk]
                    {
                        struct ClearOnExit
                        {
                            std::atomic<bool>& flag;
                            ~ClearOnExit() { flag.store(false, std::memory_order_release); }
                        } clear{ repreparing_ };
                        prepareAll(sr, blk);
                        logLine("re-prepared for sr=" + juce::String(sr)
                                + ", block=" + juce::String(blk));
                    });
                }
            }

            const uint32_t seq = h->requestSeq.load(std::memory_order_acquire);
            if (seq == lastSeq) continue;
            lastSeq = seq;

            const int n  = (int) std::min<uint32_t>(h->blockFrames, dcvb::kMaxBlockFrames);
            const int ic = (int) std::min<uint32_t>(h->inputChannels, dcvb::kMaxChannels);
            // A re-prepare is in flight: answer, but with nothing active. The
            // alternative is running plugins whose prepared block size no
            // longer matches the one they are being handed.
            if (n <= 0 || repreparing_.load(std::memory_order_acquire))
            {
                h->chainActive = 0;
                h->instrumentActive = 0;
                h->responseSeq.store(seq, std::memory_order_release);
               #if defined(_WIN32)
                ::SetEvent((HANDLE) respEvent_);
               #endif
                continue;
            }

            // Copy the request payload out of the mapping first.
            for (int ch = 0; ch < dcvb::kMaxChannels; ++ch)
            {
                float* dst = chainBuf.getWritePointer(ch);
                if (ch < ic)
                    std::memcpy(dst, dcvb::inputChannel(view_, ch), (size_t) n * sizeof(float));
                else if (ic > 0)
                    std::memcpy(dst, dcvb::inputChannel(view_, ic - 1), (size_t) n * sizeof(float));
                else
                    juce::FloatVectorOperations::clear(dst, n);
            }
            midi.clear();
            const int midiCount = (int) std::min<uint32_t>(h->midiCount, dcvb::kMaxMidiEvents);
            for (int i = 0; i < midiCount; ++i)
            {
                const uint32_t p = h->midi[i];
                const juce::uint8 bytes[3] = { (juce::uint8)(p & 0xFF),
                                               (juce::uint8)((p >> 8) & 0xFF),
                                               (juce::uint8)((p >> 16) & 0xFF) };
                // Bridge v3 (JuceSharp #28): at the sample the parent placed it
                // on, not at the head of the block. MidiBuffer keeps events
                // sorted by position, so carry order does not matter.
                midi.addEvent(bytes, 3, dcvb::midiSampleOffset(timing, i, n));
            }

            bool chainActive = false;
            bool instrActive = false;
            {
                std::lock_guard<std::mutex> lk(graphMutex_);

                // FX chain in order; bypassed slots pass through untouched.
                juce::AudioBuffer<float> sub(chainBuf.getArrayOfWritePointers(),
                                             dcvb::kMaxChannels, 0, n);
                emptyMidi.clear();
                for (auto* slot : chainOrder_)
                {
                    if (slot == nullptr || slot->instance == nullptr) continue;
                    if (slot->bypass.load(std::memory_order_relaxed)) continue;
                    slot->instance->processBlock(sub, emptyMidi);
                    chainActive = true;
                }

                // Instrument render. instrBuf_ is grown at LOAD time (message
                // thread, under this same graphMutex_) — never here on the
                // audio worker (parent audit #5: the parent blocks on this
                // thread's response, so an allocation here is an RT stall by
                // proxy). The channel guard is belt-and-braces: it can only
                // fire if a load somehow raced, and skipping one block beats
                // allocating.
                if (auto* instr = instrument_.get(); instr != nullptr && instr->instance != nullptr
                    && instrBuf_.getNumChannels() >= std::max(1, instrumentChannels_))
                {
                    const int outCh = std::max(1, instrumentChannels_);
                    juce::AudioBuffer<float> iw(instrBuf_.getArrayOfWritePointers(), outCh, 0, n);
                    iw.clear();
                    instr->instance->processBlock(iw, midi);
                    for (int ch = 0; ch < dcvb::kMaxChannels; ++ch)
                    {
                        const int srcCh = std::min(ch, outCh - 1);
                        std::memcpy(dcvb::instrOutChannel(view_, ch), iw.getReadPointer(srcCh),
                                    (size_t) n * sizeof(float));
                    }
                    instrActive = true;
                }
            }

            if (chainActive)
                for (int ch = 0; ch < dcvb::kMaxChannels; ++ch)
                    std::memcpy(dcvb::chainOutChannel(view_, ch), chainBuf.getReadPointer(ch),
                                (size_t) n * sizeof(float));

            h->chainActive      = chainActive ? 1u : 0u;
            h->instrumentActive = instrActive ? 1u : 0u;
            h->responseSeq.store(seq, std::memory_order_release);
           #if defined(_WIN32)
            ::SetEvent((HANDLE) respEvent_);
           #endif
        }
    }

    // ---- control pipe --------------------------------------------------------
    bool openPipe()
    {
       #if defined(_WIN32)
        const juce::String full = "\\\\.\\pipe\\" + pipeName_;
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            HANDLE hp = ::CreateFileA(full.toRawUTF8(), GENERIC_READ | GENERIC_WRITE,
                                      0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hp != INVALID_HANDLE_VALUE) { pipe_ = hp; return true; }
            if (::GetLastError() == ERROR_PIPE_BUSY) ::WaitNamedPipeA(full.toRawUTF8(), 200);
            else ::Sleep(100);
        }
        return false;
       #else
        return false;
       #endif
    }

    void closePipe()
    {
       #if defined(_WIN32)
        HANDLE hp = (HANDLE) pipe_.exchange(nullptr);
        if (hp != nullptr)
        {
            ::CancelIoEx(hp, nullptr); // unblock a pending synchronous ReadFile
            ::CloseHandle(hp);
        }
       #endif
    }

    void writePipeLine(const juce::String& line)
    {
       #if defined(_WIN32)
        std::lock_guard<std::mutex> lk(pipeWriteMutex_);
        HANDLE hp = (HANDLE) pipe_.load();
        if (hp == nullptr) return;
        const juce::String withNl = line + "\n";
        const char* data = withNl.toRawUTF8();
        DWORD written = 0;
        ::WriteFile(hp, data, (DWORD) std::strlen(data), &written, nullptr);
       #endif
    }

    void pipeReaderLoop()
    {
       #if defined(_WIN32)
        std::string pending;
        char buf[4096];
        while (running_.load(std::memory_order_acquire))
        {
            HANDLE hp = (HANDLE) pipe_.load();
            if (hp == nullptr) break;
            DWORD got = 0;
            if (! ::ReadFile(hp, buf, sizeof(buf), &got, nullptr) || got == 0)
            {
                // Pipe broken = parent gone (or closing). Exit.
                if (running_.load(std::memory_order_acquire))
                {
                    logLine("control pipe closed; exiting");
                    requestAsyncQuit();
                }
                break;
            }
            pending.append(buf, got);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos)
            {
                const std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (! line.empty()) handleCommandLine(juce::String::fromUTF8(line.c_str()));
            }
        }
       #endif
    }

    void requestAsyncQuit()
    {
        shuttingDown_.store(true, std::memory_order_release);
        // CLOSE THE PIPE FIRST. shutdown() runs on the message thread and joins
        // pipeThread_; the reader only leaves its loop when ReadFile returns,
        // and it will not return while the parent holds the pipe open. Closing
        // here makes the read fail immediately, so the join has something to
        // join. (It is not sufficient on its own — see onMessageThread below —
        // but without it the reader is asleep in the kernel.)
        closePipe();
        juce::MessageManager::callAsync([] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
    }

    // Run `fn` on the message thread and block until done.
    //
    // THIS USED TO BE AN UNCONDITIONAL BLOCK, AND THAT DEADLOCKED THE CHILD.
    // `shuttingDown_` only filters commands PARSED after quit; a command
    // already inside this call keeps waiting on a message loop that is winding
    // down, while shutdown() — on that very message thread — joins the pipe
    // thread this call is running on. A `quit` arriving during a multi-second
    // `load` (a cold VST3 scan is 25 s+) hung the process, and the parent then
    // had to kill it.
    //
    // So: post, wait in slices, and BAIL when the process is shutting down. The
    // job owns its own state — a shared_ptr with a mutex — because bailing
    // means this thread's stack frame (and the `fn` capturing it by reference)
    // may be gone before the message thread gets to it. Taking `m` in both
    // places is what makes the abandon airtight: if `fn` is already running we
    // block until it finishes, and if it has not started it never will.
    template <class Fn>
    void onMessageThread(Fn&& fn)
    {
        if (juce::MessageManager::existsAndIsCurrentThread()) { fn(); return; }

        struct Job
        {
            std::function<void()> fn;
            std::mutex            m;
            bool                  abandoned = false;
            juce::WaitableEvent   done;
        };
        auto job = std::make_shared<Job>();
        job->fn = std::forward<Fn>(fn);

        juce::MessageManager::callAsync([job]
        {
            {
                std::lock_guard<std::mutex> lk(job->m);
                if (! job->abandoned) job->fn();
            }
            job->done.signal();
        });

        while (! job->done.wait(50))
        {
            if (! shuttingDown_.load(std::memory_order_acquire)) continue;
            std::lock_guard<std::mutex> lk(job->m);
            job->abandoned = true;
            return;
        }
    }

    // ---- command handling ------------------------------------------------------
    void handleCommandLine(const juce::String& line)
    {
        juce::var v = juce::JSON::parse(line);
        auto* obj = v.getDynamicObject();
        if (obj == nullptr) return;

        const juce::String op   = obj->getProperty("op").toString();
        const juce::var    id   = obj->getProperty("id");

        // After quit was requested, the message loop is winding down —
        // blocking on it from here would deadlock shutdown()'s join of this
        // very thread. Drop everything except ping.
        if (shuttingDown_.load(std::memory_order_acquire) && op != "ping") return;

        auto reply = [&](bool ok, std::function<void(juce::DynamicObject&)> extra = nullptr,
                         const juce::String& error = {})
        {
            auto r = std::make_unique<juce::DynamicObject>();
            if (! id.isVoid()) r->setProperty("id", id);
            r->setProperty("ok", ok);
            if (error.isNotEmpty()) r->setProperty("error", error);
            if (extra) extra(*r);
            writePipeLine(juce::JSON::toString(juce::var(r.release()), true));
        };

        try
        {
            if (op == "ping")            { reply(true); return; }
            if (op == "quit")
            {
                reply(true);
                requestAsyncQuit();
                return;
            }
            if (op == "prepare")
            {
                const int sr  = (int) obj->getProperty("sr");
                const int blk = (int) obj->getProperty("block");
                juce::String err;
                onMessageThread([&] { err = prepareAll(sr, blk); });
                reply(err.isEmpty(), nullptr, err);
                return;
            }
            if (op == "load")
            {
                const juce::String slot   = obj->getProperty("slot").toString();
                const juce::String path   = obj->getProperty("path").toString();
                const bool         bypass = (bool) obj->getProperty("bypass");
                // ABI v33, and OPTIONAL: an absent "component" property parses
                // to a void var whose int conversion is 0, so a pre-v33 parent
                // keeps loading descriptor 0 exactly as before. The pipe
                // protocol is JSON, so this is NOT a shared-memory layout
                // change and kBridgeVersion does not move.
                const int          component = (int) obj->getProperty("component");
                juce::String err, name;
                int latency = 0;
                onMessageThread([&] { err = loadChainSlot(slot, path, bypass, latency, name, component); });
                reply(err.isEmpty(), [&](juce::DynamicObject& r)
                {
                    r.setProperty("latency", latency);
                    r.setProperty("name", name);
                }, err);
                return;
            }
            if (op == "unload")
            {
                const juce::String slot = obj->getProperty("slot").toString();
                onMessageThread([&] { unloadChainSlot(slot); });
                reply(true);
                return;
            }
            if (op == "bypass")
            {
                const juce::String slot = obj->getProperty("slot").toString();
                const bool value = (bool) obj->getProperty("value");
                std::lock_guard<std::mutex> lk(graphMutex_);
                auto it = slots_.find(slot);
                if (it != slots_.end()) it->second->bypass.store(value, std::memory_order_relaxed);
                reply(it != slots_.end());
                return;
            }
            if (op == "reorder")
            {
                if (auto* arr = obj->getProperty("slots").getArray())
                {
                    juce::StringArray order;
                    for (auto& s : *arr) order.add(s.toString());
                    onMessageThread([&] { reorderChain(order); });
                }
                reply(true);
                return;
            }
            if (op == "loadInstrument")
            {
                const juce::String path = obj->getProperty("path").toString();
                const int component = (int) obj->getProperty("component");   // optional, v33
                juce::String err, name;
                int latency = 0;
                onMessageThread([&] { err = loadInstrument(path, latency, name, component); });
                reply(err.isEmpty(), [&](juce::DynamicObject& r)
                {
                    r.setProperty("latency", latency);
                    r.setProperty("name", name);
                }, err);
                return;
            }
            if (op == "unloadInstrument")
            {
                onMessageThread([&] { unloadInstrument(); });
                reply(true);
                return;
            }
            if (op == "showEditor" || op == "hideEditor")
            {
                const juce::String slot = obj->getProperty("slot").toString();
                const bool show = (op == "showEditor");
                bool found = false;
                onMessageThread([&] { found = toggleEditor(slot, show); });
                reply(found);
                return;
            }
            if (op == "getState")
            {
                const juce::String slot = obj->getProperty("slot").toString();
                juce::String state;
                bool found = false;
                onMessageThread([&] { found = saveState(slot, state); });
                reply(found, [&](juce::DynamicObject& r) { r.setProperty("state", state); });
                return;
            }
            if (op == "setState")
            {
                const juce::String slot  = obj->getProperty("slot").toString();
                const juce::String state = obj->getProperty("state").toString();
                bool found = false;
                onMessageThread([&] { found = loadState(slot, state); });
                reply(found);
                return;
            }
            reply(false, nullptr, "unknown op: " + op);
        }
        catch (const std::exception& ex) { reply(false, nullptr, juce::String("exception: ") + ex.what()); }
        catch (...)                      { reply(false, nullptr, "unknown exception"); }
    }

    // ---- plugin graph (message thread unless noted) ------------------------------
    // ABI v33: `component` selects one plugin out of a MULTI-COMPONENT bundle,
    // exactly as dca_plugin_load_vst3_component does in-process. It defaults to
    // 0, which is what a pipe message that omits the property decodes to — so
    // an older parent keeps the pre-v33 behaviour byte for byte. An index the
    // bundle does not have is REJECTED, never clamped onto component 0: a
    // silently substituted plugin is a saved rig that reloads as a different
    // effect.
    std::unique_ptr<juce::AudioPluginInstance> createInstance(const juce::String& path, juce::String& err,
                                                              int component = 0)
    {
        juce::OwnedArray<juce::PluginDescription> descs;
        vst3_->findAllTypesForFile(descs, path);
        if (descs.isEmpty()) { err = "VST3: no descriptors for " + path; return nullptr; }
        if (component < 0 || component >= descs.size())
        {
            err = "VST3: component " + juce::String(component) + " of " + path + " does not exist ("
                + juce::String(descs.size()) + " declared)";
            return nullptr;
        }
        return vst3_->createInstanceFromDescription(*descs[component], (double) sampleRate_, blockSize_, err);
    }

    juce::String loadChainSlot(const juce::String& slotId, const juce::String& path,
                               bool bypass, int& latencyOut, juce::String& nameOut,
                               int component = 0)
    {
        unloadChainSlot(slotId); // replace-in-place semantics

        juce::String err;
        auto inst = createInstance(path, err, component);
        if (inst == nullptr) return err.isNotEmpty() ? err : "load failed";

        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses .add(juce::AudioChannelSet::stereo());
        layout.outputBuses.add(juce::AudioChannelSet::stereo());
        inst->setBusesLayout(layout);
        inst->enableAllBuses();
        inst->setNonRealtime(false);
        inst->prepareToPlay((double) sampleRate_, blockSize_);

        auto slot = std::make_unique<HostedPlugin>();
        slot->slotId = slotId;
        slot->path   = path;
        slot->instance = std::move(inst);
        slot->bypass.store(bypass, std::memory_order_relaxed);
        latencyOut = std::max(0, slot->instance->getLatencySamples());
        nameOut    = slot->instance->getName();

        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            chainOrder_.push_back(slot.get());
            slots_[slotId] = std::move(slot);
        }
        logLine("chain slot loaded: " + nameOut + " (" + slotId + ")");
        return {};
    }

    void unloadChainSlot(const juce::String& slotId)
    {
        std::unique_ptr<HostedPlugin> victim;
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            auto it = slots_.find(slotId);
            if (it == slots_.end()) return;
            chainOrder_.erase(std::remove(chainOrder_.begin(), chainOrder_.end(), it->second.get()),
                              chainOrder_.end());
            victim = std::move(it->second);
            slots_.erase(it);
        }
        // Editor first, then instance (strict order — BIAS/Helix crash otherwise).
        victim->editor.reset();
        if (victim->instance) { victim->instance->releaseResources(); victim->instance.reset(); }
        logLine("chain slot unloaded: " + slotId);
    }

    void reorderChain(const juce::StringArray& order)
    {
        std::lock_guard<std::mutex> lk(graphMutex_);
        std::vector<HostedPlugin*> next;
        next.reserve(chainOrder_.size());
        for (const auto& id : order)
        {
            auto it = slots_.find(id);
            if (it != slots_.end()) next.push_back(it->second.get());
        }
        // Keep any slots the order list forgot (defensive) at the tail.
        for (auto* p : chainOrder_)
            if (std::find(next.begin(), next.end(), p) == next.end()) next.push_back(p);
        chainOrder_ = std::move(next);
    }

    juce::String loadInstrument(const juce::String& path, int& latencyOut, juce::String& nameOut,
                                int component = 0)
    {
        unloadInstrument();

        juce::String err;
        auto inst = createInstance(path, err, component);
        if (inst == nullptr) return err.isNotEmpty() ? err : "load failed";

        juce::AudioProcessor::BusesLayout layout;
        layout.outputBuses.add(juce::AudioChannelSet::stereo());
        inst->setBusesLayout(layout);
        inst->enableAllBuses();
        inst->setNonRealtime(false);
        inst->prepareToPlay((double) sampleRate_, blockSize_);

        auto slot = std::make_unique<HostedPlugin>();
        slot->slotId = "instrument";
        slot->path   = path;
        slot->instance = std::move(inst);
        latencyOut = std::max(0, slot->instance->getLatencySamples());
        nameOut    = slot->instance->getName();

        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            instrumentChannels_ = std::max(1, slot->instance->getTotalNumOutputChannels());
            // Grow the render scratch HERE (message thread) — the audio worker
            // must never setSize (parent audit #5). graphMutex_ serializes us
            // against the worker's instrument-render section.
            if (instrBuf_.getNumChannels() < instrumentChannels_)
                instrBuf_.setSize(instrumentChannels_, dcvb::kMaxBlockFrames, false, true, true);
            instrument_ = std::move(slot);
        }
        logLine("instrument loaded: " + nameOut);
        return {};
    }

    void unloadInstrument()
    {
        std::unique_ptr<HostedPlugin> victim;
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            victim = std::move(instrument_);
        }
        if (victim == nullptr) return;
        victim->editor.reset();
        if (victim->instance) { victim->instance->releaseResources(); victim->instance.reset(); }
        logLine("instrument unloaded");
    }

    juce::String prepareAll(int sr, int blk)
    {
        if (sr > 0)  sampleRate_ = sr;
        if (blk > 0) blockSize_  = blk;
        std::lock_guard<std::mutex> lk(graphMutex_);
        for (auto& kv : slots_)
            if (kv.second->instance)
            {
                kv.second->instance->releaseResources();
                kv.second->instance->prepareToPlay((double) sampleRate_, blockSize_);
            }
        if (instrument_ != nullptr && instrument_->instance != nullptr)
        {
            instrument_->instance->releaseResources();
            instrument_->instance->prepareToPlay((double) sampleRate_, blockSize_);
        }
        return {};
    }

    HostedPlugin* findSlot(const juce::String& slotId)
    {
        if (slotId == "instrument") return instrument_.get();
        auto it = slots_.find(slotId);
        return it != slots_.end() ? it->second.get() : nullptr;
    }

    bool toggleEditor(const juce::String& slotId, bool show)
    {
        auto* slot = findSlot(slotId);
        if (slot == nullptr || slot->instance == nullptr) return false;

        if (! show)
        {
            if (slot->editor) slot->editor->setVisible(false);
            return true;
        }

        if (slot->editor == nullptr)
        {
            if (! slot->instance->hasEditor()) return false;
            auto* ed = slot->instance->createEditorAndMakeActive();
            if (ed == nullptr)
            {
                if (auto* stale = slot->instance->getActiveEditor())
                {
                    delete stale;
                    ed = slot->instance->createEditorAndMakeActive();
                }
                if (ed == nullptr) return false;
            }
            const juce::String id = slot->slotId;
            slot->editor = std::make_unique<HostEditorWindow>(*slot->instance, ed, [this, id]
            {
                // Defer the actual teardown; we cannot delete the window from
                // inside its own close callback. Then tell the parent so its
                // editor-toggle state doesn't go stale.
                juce::MessageManager::callAsync([this, id]
                {
                    if (auto* s = findSlot(id)) s->editor.reset();
                    auto ev = std::make_unique<juce::DynamicObject>();
                    ev->setProperty("op", "event");
                    ev->setProperty("event", "editorClosed");
                    ev->setProperty("slot", id);
                    writePipeLine(juce::JSON::toString(juce::var(ev.release()), true));
                });
            });
        }
        else
        {
            slot->editor->setVisible(true);
        }
        slot->editor->toFront(true);
       #if defined(_WIN32)
        if (auto* peer = slot->editor->getPeer())
            ::SetForegroundWindow((HWND) peer->getNativeHandle());
       #endif
        return true;
    }

    bool saveState(const juce::String& slotId, juce::String& base64Out)
    {
        auto* slot = findSlot(slotId);
        if (slot == nullptr || slot->instance == nullptr) return false;
        juce::MemoryBlock block;
        slot->instance->getStateInformation(block);
        base64Out = juce::Base64::toBase64(block.getData(), block.getSize());
        return true;
    }

    bool loadState(const juce::String& slotId, const juce::String& base64)
    {
        auto* slot = findSlot(slotId);
        if (slot == nullptr || slot->instance == nullptr) return false;
        juce::MemoryOutputStream decoded;
        if (! juce::Base64::convertFromBase64(decoded, base64)) return false;
        // setStateInformation takes an int: refuse what does not fit rather than
        // hand the plugin a negative size.
        if (decoded.getDataSize() > (size_t) std::numeric_limits<int>::max()) return false;
        slot->instance->setStateInformation(decoded.getData(), (int) decoded.getDataSize());
        return true;
    }

    void teardownAllPlugins()
    {
        std::lock_guard<std::mutex> lk(graphMutex_);
        chainOrder_.clear();
        for (auto& kv : slots_)
        {
            try
            {
                kv.second->editor.reset();
                if (kv.second->instance)
                {
                    kv.second->instance->releaseResources();
                    kv.second->instance.reset();
                }
            }
            catch (...) { /* dying plugin — we're exiting anyway */ }
        }
        slots_.clear();
        try
        {
            if (instrument_ != nullptr)
            {
                instrument_->editor.reset();
                if (instrument_->instance)
                {
                    instrument_->instance->releaseResources();
                    instrument_->instance.reset();
                }
                instrument_.reset();
            }
        }
        catch (...) {}
    }

    // ---- parent watchdog --------------------------------------------------------
    void startParentWatchdog()
    {
       #if defined(_WIN32)
        if (parentPid_ == 0) return;
        HANDLE hParent = ::OpenProcess(SYNCHRONIZE, FALSE, (DWORD) parentPid_);
        if (hParent == nullptr) return;
        watchdogThread_ = std::thread([hParent]
        {
            ::WaitForSingleObject(hParent, INFINITE);
            ::CloseHandle(hParent);
            // Parent is gone. A graceful plugin teardown can itself crash
            // (that is why we exist); with no parent to report to, a hard
            // exit is the honest, safe option.
            ::ExitProcess(0);
        });
        watchdogThread_.detach();
       #endif
    }

    // ---- state --------------------------------------------------------------
    juce::String shmName_, reqName_, respName_, pipeName_;
    int      sampleRate_ = 44100;
    int      blockSize_  = 512;
    uint32_t parentPid_  = 0;

    std::unique_ptr<juce::VST3PluginFormat> vst3_;

    void* mapping_   = nullptr;
    void* view_      = nullptr;
    void* reqEvent_  = nullptr;
    void* respEvent_ = nullptr;
    std::atomic<void*> pipe_{nullptr};
    std::mutex pipeWriteMutex_;

    std::atomic<bool> running_{false};
    std::atomic<bool> shuttingDown_{false};
    // Bridge v2 device-configuration replay. `lastPrepareGen_` is worker-owned
    // (latched in openBridge); `repreparing_` is set by the worker and cleared
    // by the message thread that finished the prepare.
    uint32_t          lastPrepareGen_ = 0;
    std::atomic<bool> repreparing_{false};
    std::thread audioThread_;
    std::thread pipeThread_;
    std::thread watchdogThread_;

    // graphMutex_ guards chainOrder_/slots_/instrument_ between the audio
    // worker and the message-thread mutators. Loads happen OUTSIDE the lock
    // (prepareToPlay can take seconds for big amp sims); only the vector /
    // map swaps run under it, so the audio worker keeps flowing during loads.
    std::mutex graphMutex_;
    std::vector<HostedPlugin*> chainOrder_;
    std::map<juce::String, std::unique_ptr<HostedPlugin>> slots_;
    std::unique_ptr<HostedPlugin> instrument_;
    int instrumentChannels_ = 2;
    // Instrument render scratch. Sized at LOAD time under graphMutex_; the
    // audio worker only reads it (see audioWorkerLoop — it never setSizes).
    juce::AudioBuffer<float> instrBuf_{ 8, dcvb::kMaxBlockFrames };
};

} // namespace

START_JUCE_APPLICATION(VstHostApplication)
