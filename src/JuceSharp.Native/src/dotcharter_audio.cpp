// dotcharter_audio.cpp
//
// C ABI bridge over the JUCE-based engine. Implements include/dotcharter_audio.h.

#include "dotcharter_audio.h"
#include "AudioEngine.h"
#include "MidiOutputDevice.h"
#include "RubberBandAudioSource.h"
#include "StemSeparator.h"
#include "AbiArgs.h"
#include "Utf8Truncate.h"
#include "wem/WemAudioFormat.h"
#include "DeviceRateWav.h"
#include "wem/WemVorbisDecoder.h"
#include "wem/WemVorbisEncoder.h"
#include "dsp/voice/dcvoice_pickup.h"
#include "dsp/cores/dcdsp_rsamp_table.h"
#if DOTCHARTER_DSP_NEURAL
  // For dcrig::setNamLogSink only — NeuralAudio's own headers stay scoped to
  // NamAmpModule.cpp, exactly as DspModuleFactory.cpp includes this header.
  #include "dsp/NamAmpModule.h"
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// Cross-platform replacement for std::getenv that uses _dupenv_s on MSVC
// (where std::getenv triggers C4996). Caller must free the returned string
// with std::free when non-null.
std::string safeGetEnv(const char* name)
{
   #if defined(_MSC_VER)
    char*  buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr)
        return {};
    std::string value(buf);
    std::free(buf);
    return value;
   #else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
   #endif
}

std::atomic<dca_log_callback> g_logCallback{nullptr};
std::atomic<void*>            g_logUser{nullptr};

// Console fallback level — read once from JUCESHARP_CONSOLE_LOG env var on
// first emit. 0 = disabled. 1=trace,2=debug,3=info,4=warn,5=error.
// Even when a managed callback is registered, stderr fallback also writes if
// JUCESHARP_FORCE_CONSOLE_LOG=1 — useful when diagnosing managed-side issues.
int consoleMinLevel()
{
    static int cached = []
    {
        // p3-46: lowercase + trim the env var so common spellings work
        // (TRACE, Debug, " info "). Previously case-sensitive equality
        // against bare lowercase tags meant a CI override of e.g.
        // JUCESHARP_CONSOLE_LOG=INFO silently fell into the boolean
        // branch instead of selecting the info threshold.
        auto env = safeGetEnv("JUCESHARP_CONSOLE_LOG");
        // trim
        while (! env.empty() && std::isspace((unsigned char) env.front())) env.erase(env.begin());
        while (! env.empty() && std::isspace((unsigned char) env.back()))  env.pop_back();
        // tolower
        for (auto& c : env) c = (char) std::tolower((unsigned char) c);
        if (! env.empty())
        {
            if (env == "trace" || env == "5") return 1;
            if (env == "debug" || env == "4") return 2;
            if (env == "info"  || env == "3") return 3;
            if (env == "warn"  || env == "warning" || env == "2") return 4;
            if (env == "error" || env == "1") return 5;
            if (env == "off"   || env == "none" || env == "0" ||
                env == "false" || env == "no" || env == "n") return 0;
            if (env[0] == '1' || env[0] == 'y' || env[0] == 't') return 3; // default to info
        }
        return 0;
    }();
    return cached;
}

bool forceConsole()
{
    static bool cached = []
    {
        const auto env = safeGetEnv("JUCESHARP_FORCE_CONSOLE_LOG");
        return ! env.empty() && (env[0] == '1' || env[0] == 'y' || env[0] == 'Y');
    }();
    return cached;
}

const char* levelTag(dca_log_level lvl)
{
    switch (lvl) {
        case DCA_LOG_TRACE: return "TRACE";
        case DCA_LOG_DEBUG: return "DEBUG";
        case DCA_LOG_INFO:  return "INFO ";
        case DCA_LOG_WARN:  return "WARN ";
        case DCA_LOG_ERROR: return "ERROR";
    }
    return "LOG  ";
}

void emitLog(dca_log_level level, const char* msg)
{
    if (msg == nullptr) msg = "(null)";

    auto cb = g_logCallback.load(std::memory_order_acquire);
    if (cb != nullptr)
    {
        try { cb(g_logUser.load(std::memory_order_acquire), level, msg); }
        catch (...) { /* never let managed exceptions cross back */ }
    }

    const int floor = consoleMinLevel();
    const bool fallback = (cb == nullptr) || forceConsole();
    if (fallback && floor > 0 && (static_cast<int>(level) + 1) >= floor)
    {
        std::FILE* out = (level >= DCA_LOG_WARN) ? stderr : stdout;
        std::fprintf(out, "juce_sharp  %s  %s\n", levelTag(level), msg);
        std::fflush(out);
    }
}

// Report how a freshly opened source will be read. A preloaded source plays
// from RAM with no BufferingAudioSource, which is what keeps its decode off the
// audio thread's lock (see TransportSource::wireReaderToTransport); a streaming
// one is back on the old path, and when a dropout report arrives this line is
// how anyone tells which.
void logSourceReadPath(const dotcharter::TransportSource& t, const char* what)
{
    char buf[448];
    // The rate conversion is the single most expensive thing a preload does
    // (the windowed-sinc path cost ~6.4 s for a 183 s stereo 44.1 kHz file,
    // sequentially per stem), so its path and cost go in the same line the
    // owner already reads when a load feels slow. See src/LoadResampler.h.
    //
    // JuceSharp #33: the line names the CONVERTER every source ended up on --
    // offline polyphase / windowed-sinc, the streaming polyphase stage, or
    // JUCE's linear one (a ratio with no rational form) -- so a dull or
    // aliasing report can be matched to its path.
    char conv[160] = { 0 };
    const auto& r = t.lastResampleReport();
    if (r.converter != dcload::Converter::None)
    {
        char ratio[32] = { 0 };
        if (r.interpolate > 0 && r.decimate > 0)
            std::snprintf(ratio, sizeof ratio, " (%d:%d)", r.interpolate, r.decimate);
        char cost[32] = { 0 };
        if (r.converter == dcload::Converter::Polyphase || r.converter == dcload::Converter::WindowedSinc)
            std::snprintf(cost, sizeof cost, " in %.0f ms", r.elapsedMs);
        std::snprintf(conv, sizeof conv, ", rate %.0f->%.0f Hz via %s%s%s",
                      r.fromRate, r.toRate, dcload::converterName(r.converter), ratio, cost);
    }
    if (t.isPreloaded())
        std::snprintf(buf, sizeof buf, "%s: preloaded into RAM (%.1f MB)%s, no read-ahead lock",
                      what, (double) t.preloadedBytes() / (1024.0 * 1024.0), conv);
    else
        std::snprintf(buf, sizeof buf,
                      "%s: streaming (did not fit the preload budget) - decode shares a lock "
                      "with the audio thread%s", what, conv);
    emitLog(DCA_LOG_INFO, buf);
}

// Convenience: wrap an exported DCA_API entry point in try/catch and turn any
// stray C++ exception into DCA_ERR_INTERNAL plus a logged message. Must be
// used only at the ABI boundary — never around internal helpers.
template <typename Fn>
dca_status guard(const char* op, Fn&& fn)
{
    try { return fn(); }
    catch (const std::exception& ex)
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "[%s] std::exception: %s", op, ex.what());
        emitLog(DCA_LOG_ERROR, buf);
        return DCA_ERR_INTERNAL;
    }
    catch (...)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[%s] unknown C++ exception", op);
        emitLog(DCA_LOG_ERROR, buf);
        return DCA_ERR_INTERNAL;
    }
}

// The same protection for an export whose status channel is its RETURN VALUE
// (a node id, a byte count). `onError` is that export's documented failure
// sentinel.
template <typename T, typename Fn>
T guardValue(const char* op, T onError, Fn&& fn)
{
    try { return fn(); }
    catch (const std::exception& ex)
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "[%s] std::exception: %s", op, ex.what());
        emitLog(DCA_LOG_ERROR, buf);
        return onError;
    }
    catch (...)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[%s] unknown C++ exception", op);
        emitLog(DCA_LOG_ERROR, buf);
        return onError;
    }
}

// The engine cannot reach emitLog (it is local to this translation unit), so
// it used to write its two diagnostics straight to stderr — which the managed
// side never sees. Install a trampoline once, at static-init time, so a failed
// device open reaches the app's log with the full list of device names the
// driver actually offers. Static-init order is safe: emitLog only touches
// constant-initialised atomics.
static_assert((int) DCA_LOG_WARN  == dotcharter::AudioEngine::kLogWarn,
              "AudioEngine::kLogWarn must mirror dca_log_level");
static_assert((int) DCA_LOG_ERROR == dotcharter::AudioEngine::kLogError,
              "AudioEngine::kLogError must mirror dca_log_level");

static_assert((int) DCA_LOG_WARN == dcrig::DspChain::kLogWarn,
              "DspChain::kLogWarn must mirror dca_log_level");

#if DOTCHARTER_DSP_NEURAL
static_assert((int) DCA_LOG_WARN == dcrig::kNamLogWarn,
              "kNamLogWarn must mirror dca_log_level");
#endif

const bool g_engineLogBridgeInstalled = []
{
    dotcharter::AudioEngine::setLogSink([](int level, const char* message)
    {
        emitLog((dca_log_level) level, message);
    });
    // The rig has exactly one diagnostic — beginEdit found a scope already
    // open and aborted it, i.e. somebody LEAKED an edit scope — and it is the
    // only tell that a seat's rig was about to freeze, so it must not go to
    // stderr where the managed side never sees it.
    dcrig::DspChain::setLogSink([](int level, const char* message)
    {
        emitLog((dca_log_level) level, message);
    });
   #if DOTCHARTER_DSP_NEURAL
    // namamp's retirement queue is UNBOUNDED by design — its only sweeper is
    // the module's worker, which can sit inside a cold NeuralAudio parse for
    // seconds — so past kSoftCap it raises an edge-triggered warning. That flag
    // had no reader at all (W4-D38), which left "unbounded with no way to SEE
    // it" exactly as true as before the soft cap was added. It has its own sink
    // rather than the chain's because juce_sharp_nam_lifetime links two TUs and
    // cannot see DspChain.h; see NamAmpModule.h.
    dcrig::setNamLogSink([](int level, const char* message)
    {
        emitLog((dca_log_level) level, message);
    });
   #endif
    return true;
}();

struct Plugin;
struct Source;

struct Engine
{
    dotcharter::AudioEngine impl;

    // Plugins loaded against this engine. Used by dca_engine_destroy to force
    // a clean editor + plugin teardown on the JuceHost message thread if the
    // .NET side forgets to unload (e.g. the user clicks the window's X while
    // a VST3 editor window is still open). Strict shutdown order is critical
    // — many plugins (BIAS FX, Helix, Neural DSP) crash hard on the audio
    // thread / on DLL detach if their editor outlives the engine.
    std::mutex            pluginsMutex;
    std::vector<Plugin*>  plugins;

    // Sources opened against this engine (2026-07-16 audit #9). Same safety
    // net the plugin registry provides: a Source that outlives its Engine used
    // to be an unguarded UAF — ~TransportSource calls engine_.removeSource on
    // a freed engine. dca_engine_destroy now ORPHANS every still-registered
    // source (destroys its transport while the engine is still alive and nulls
    // its engine pointer); a later dca_source_destroy just frees the shell.
    // Same D-06-style contract as plugins: dca_engine_destroy must not race a
    // CONCURRENT dca_source_* call on the same source.
    //
    // THE MANAGED SIDE REALLY DOES GUARANTEE THAT NOW, and it did not when this
    // comment was written (wrapper 0.61). NativeHandleOwner's drain used to
    // wait 2 s and then hand the handle back ANYWAY, so every caller destroyed
    // it mid-call; it now waits on the last Release and, on expiry, logs at
    // Error and LEAKS the handle. And the drain is PER HANDLE, so
    // JuceAudioEngine keeps a registry of every child it hands out and disposes
    // them before dca_engine_destroy. Neither half existed when this file was
    // written, so treat the guarantee as real but VERSIONED: it is a property
    // of the managed wrapper shipped alongside this DLL, not of the ABI.
    std::mutex            sourcesMutex;
    std::vector<Source*>  sources;
};

struct Source
{
    Engine*                              engine = nullptr;
    std::unique_ptr<dotcharter::TransportSource> transport;
};

// Forward decl - defined further down once JUCE windowing types are visible.
struct EditorWindow;

struct Plugin
{
    Engine*                                       engine   = nullptr;
    std::unique_ptr<juce::AudioPluginInstance>    instance;
    std::shared_ptr<EditorWindow>                 editor;   // lives on MM thread; shared_ptr so weak_ptr can outlive Plugin teardown safely
    // p2-19: attached / attachedAsInstrument are read by the audio thread
    // (renderInstrumentBus / processInputChain via instrumentPlugin_ /
    // inputChain_ atomics) and written by attach/detach exports on the MM
    // thread. Mark them std::atomic<bool> for explicit cross-thread
    // visibility instead of relying on the surrounding plugin/chain pointer
    // atomics to carry the ordering of these scalar flags.
    std::atomic<bool>                             attached{false};
    std::atomic<bool>                             attachedAsInstrument{false};
    // ABI v28: WHICH seat's input chain holds this plugin (-1 = none). Same
    // cross-thread reasoning as `attached` — written by the attach/detach
    // exports on the MM thread, read by anything reporting the wiring back.
    std::atomic<int>                              attachedPlayer{-1};
    // p0-5: lifetime token. Async editor-close callbacks observe this via a
    // weak_ptr<bool>; if Plugin has been torn down (alive set to false in
    // teardownPluginAndDelete) they no-op instead of dereferencing this.
    std::shared_ptr<std::atomic<bool>>            alive = std::make_shared<std::atomic<bool>>(true);
    // ABI v14: host notification for user-initiated editor close (native X
    // button). Written from any thread via the set_callback export, read on
    // the MM thread when the deferred close lands.
    std::atomic<dca_editor_closed_callback>       editorClosedCb{nullptr};
    std::atomic<void*>                            editorClosedUser{nullptr};
};

// ---------------------------------------------------------------------------
// JuceHost: a dedicated thread that owns the JUCE MessageManager. All VST3
// hosting work (loading, parameter ops, editor windows) must happen on the
// message thread to avoid deadlocks with MessageManagerLock - which on a
// foreign thread waits for the MM thread to cooperatively dispatch. By
// owning our own MM thread inside the DLL we can serve any caller without
// requiring the Avalonia UI thread to pump JUCE messages.
// ---------------------------------------------------------------------------
class JuceHost
{
public:
    static JuceHost& instance()
    {
        // Deliberately leak the singleton via heap allocation. A function-
        // local static would have its destructor called during DLL unload,
        // at which point the message-thread join would either deadlock the
        // loader lock (the MM thread can call back into the DLL) or destroy
        // a still-joinable std::thread and call std::terminate(). Leaking
        // is the only safe option for the lifetime of a hosted DLL.
        static JuceHost* h = new JuceHost();
        return *h;
    }

    void ensureStarted()
    {
        std::call_once(startOnce_, [this]
        {
            thread_ = std::thread([this] { runLoop(); });
            ready_.wait(); // block caller until MM is alive on host thread.
        });
    }

    juce::VST3PluginFormat& vst3() { return *vst3_; }

    // Run a callable on the host (message) thread and block until it returns.
    // If we are already on the message thread we just call directly.
    //
    // KNOWN LIMITATION (D-07): callFunctionOnMessageThread blocks the caller
    // until the MM thread picks up the job. If `f` triggers a plugin load
    // whose internal worker thread calls back into host code that itself
    // tries to callSync(...), that worker thread will wait on the MM thread
    // which is busy executing this very lambda — a circular wait.
    // The MM thread's runDispatchLoopUntil(20) pump usually services nested
    // posted messages, but it cannot service a worker thread that is itself
    // already blocked on a *new* callSync. No plugin currently exercised by
    // DotCharter has been observed to do this, but treat callSync bodies as
    // "must not provoke nested host re-entry" until a timeout/escape path
    // is added here.
    template <class F>
    auto callSync(F&& f) -> std::invoke_result_t<F>
    {
        using R = std::invoke_result_t<F>;
        ensureStarted();

        if (juce::MessageManager::existsAndIsCurrentThread())
            return f();

        if constexpr (std::is_void_v<R>)
        {
            std::function<void()> fn = std::forward<F>(f);
            void* user = &fn;
            juce::MessageManager::getInstance()->callFunctionOnMessageThread(
                [](void* u) -> void* { (*static_cast<std::function<void()>*>(u))(); return nullptr; },
                user);
        }
        else
        {
            R result{};
            auto wrapper = [&]() { result = f(); };
            std::function<void()> fn = wrapper;
            void* user = &fn;
            juce::MessageManager::getInstance()->callFunctionOnMessageThread(
                [](void* u) -> void* { (*static_cast<std::function<void()>*>(u))(); return nullptr; },
                user);
            return result;
        }
    }

private:
    JuceHost()  = default;
    ~JuceHost() = default; // intentionally never destroyed - leaks on DLL unload.

    void runLoop()
    {
        // ScopedJuceInitialiser_GUI registers THIS thread as the JUCE message
        // thread. We then pump the dispatch loop until shutdown is requested
        // (which currently never happens; the host outlives the DLL).
        juce::ScopedJuceInitialiser_GUI juce;
        vst3_ = std::make_unique<juce::VST3PluginFormat>();

        ready_.signal();

        // Forever, and that is the design: the singleton is deliberately leaked
        // (see instance()) because the host process outlives this DLL, so there
        // is no shutdown to request. This used to spin on a `shutdownRequested_`
        // flag that nothing could ever set — a loop condition that read as an
        // escape hatch and was not one.
        for (;;)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);

        vst3_.reset();
    }

    std::once_flag                          startOnce_;
    std::thread                             thread_;
    juce::WaitableEvent                     ready_{false};
    std::unique_ptr<juce::VST3PluginFormat> vst3_;
};

// ---------------------------------------------------------------------------
// EditorWindow: top-level JUCE window that hosts a plugin's editor view.
// Must be constructed and destroyed on the JuceHost message thread.
// ---------------------------------------------------------------------------
struct EditorWindow : public juce::DocumentWindow
{
    EditorWindow(juce::AudioPluginInstance& plug, juce::AudioProcessorEditor* ed,
                 std::function<void()> onClose)
        : juce::DocumentWindow(plug.getName(), juce::Colours::black,
                               juce::DocumentWindow::closeButton)
        , editor_(ed)
        , onClose_(std::move(onClose))
    {
        setUsingNativeTitleBar(true);
        if (editor_ != nullptr)
        {
            setResizable(editor_->isResizable(), false);
            setContentNonOwned(editor_.get(), true); // sizes window to the editor
        }
        else
        {
            setSize(400, 200);
        }
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        toFront(true);
    }

    ~EditorWindow() override
    {
        setVisible(false);
        clearContentComponent();
        // The HOST owns a hosted plugin editor — deleting it is mandatory.
        // JUCE's VST3 wrapper editor calls processor.editorBeingDeleted()
        // from its own destructor, which clears the plugin's activeEditor.
        // The previous no-op deleter leaked the editor AND left activeEditor
        // set, so the next createEditorAndMakeActive() returned nullptr in
        // release builds ("can't reopen after X") and the leaked editor's
        // dangling native view eventually access-violated (0xC0000005).
        editor_.reset();
    }

    void closeButtonPressed() override
    {
        // Hide immediately so the window vanishes; then schedule a full
        // teardown of the editor + this window on the message thread. We
        // can't delete `this` synchronously from inside our own callback,
        // and a plain setVisible(false) was leaving the plugin's child
        // native HWND in a half-destroyed state (the Windows taskbar X
        // closes the plugin's own top-level window directly on some VST3s
        // like Ample / BIAS, which then access-violates the next time we
        // poke the editor).
        setVisible(false);
        if (closeScheduled_) return;
        closeScheduled_ = true;
        auto cb = onClose_;
        juce::MessageManager::callAsync([cb] { if (cb) cb(); });
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor_;
    std::function<void()> onClose_;
    bool closeScheduled_ = false;
};

// Tear a single Plugin down. MUST NOT be called from the JuceHost message
// thread (we block on it). Editor is reset first to keep VST3s like BIAS FX,
// Helix and Neural DSP — which crash if their editor outlives their
// AudioPluginInstance — happy. Deletes `p`. Safe with p == nullptr.
inline void teardownPluginAndDelete(Plugin* p)
{
    if (p == nullptr) return;

    // Both detaches go through the message thread, for the same reason the
    // attach exports do: they are a check, a graph edit and an attachment-state
    // flip, and running them on the caller's thread let a teardown interleave
    // with an attach into the same slot. (callSync calls straight through when
    // it is already on the message thread, so this is also what makes the
    // "MUST NOT be called from the message thread" note above conservative
    // rather than load-bearing.)
    if ((p->attached || p->attachedAsInstrument) && p->engine && p->instance)
    {
        JuceHost::instance().callSync([p]
        {
            if (p->attached)
            {
                try { p->engine->impl.detachInputPlugin(p->instance.get()); }
                catch (...) {}
                p->attached = false;
            }
            if (p->attachedAsInstrument)
            {
                try { p->engine->impl.detachInstrumentPlugin(p->instance.get()); }
                catch (...) {}
                p->attachedAsInstrument = false;
            }
        });
    }

    JuceHost::instance().callSync([p]
    {
        try
        {
            // p0-5: invalidate the alive token BEFORE destroying the editor.
            // Any pending closeButtonPressed async callback that fires after
            // this point will see alive==false via its weak_ptr and no-op.
            if (p->alive) p->alive->store(false, std::memory_order_release);
            p->editor.reset();
            if (p->instance)
            {
                p->instance->releaseResources();
                p->instance.reset();
            }
        }
        catch (...)
        {
            emitLog(DCA_LOG_ERROR, "VST3 unload: plugin threw during teardown (ignored)");
        }
    });

    if (p->engine != nullptr)
    {
        std::lock_guard<std::mutex> lk(p->engine->pluginsMutex);
        auto& v = p->engine->plugins;
        v.erase(std::remove(v.begin(), v.end(), p), v.end());
    }

    delete p;
}

} // namespace

// p3-48: ABI marshalling invariants. Underlying-type assertions ensure
// dca_status / dca_driver / dca_log_level always cross the ABI as 32-bit
// ints (DllImport on the managed side assumes this). Update both these
// assertions and ExpectedAbiVersion in NativeMethods.cs when intentionally
// changing layout, and bump DCA_ABI_VERSION.
static_assert(sizeof(dca_status)    == sizeof(int32_t), "dca_status must marshal as 32-bit int");
static_assert(sizeof(dca_driver)    == sizeof(int32_t), "dca_driver must marshal as 32-bit int");
static_assert(sizeof(dca_log_level) == sizeof(int32_t), "dca_log_level must marshal as 32-bit int");
static_assert(sizeof(dca_device_info)   >= 128 + 256, "dca_device_info missing id/name buffers");
static_assert(sizeof(dca_engine_config) >= 128 + 128, "dca_engine_config missing device-id buffers");

extern "C" {

DCA_API uint32_t    DCA_CALL dca_abi_version(void)    { return DCA_ABI_VERSION; }
// GENERATED FROM THE BUILD, not typed twice. This string said
// "DotCharter.Audio.Native 0.3.0" for five releases — a second hard-coded copy
// of a CMake `project(VERSION)` that was itself stale, under a name the library
// no longer goes by. The ABI version is the part a caller can act on, so it is
// in here too; JUCE's tag identifies the host framework a plugin is running in.
#ifndef JUCESHARP_NATIVE_VERSION
  #define JUCESHARP_NATIVE_VERSION "0.0.0-nocmake"
#endif
#ifndef JUCESHARP_JUCE_TAG
  #define JUCESHARP_JUCE_TAG "unknown"
#endif
#define DCA_STRINGIFY_2(x) #x
#define DCA_STRINGIFY(x)   DCA_STRINGIFY_2(x)

DCA_API const char* DCA_CALL dca_version_string(void)
{
    return "JuceSharp.Native " JUCESHARP_NATIVE_VERSION
           " (ABI " DCA_STRINGIFY(DCA_ABI_VERSION) ", JUCE " JUCESHARP_JUCE_TAG ")";
}

DCA_API void DCA_CALL dca_set_log_callback(dca_log_callback cb, void* user)
{
    g_logCallback.store(cb,   std::memory_order_release);
    g_logUser    .store(user, std::memory_order_release);
}

// --- Devices ---------------------------------------------------------------

DCA_API dca_status DCA_CALL dca_enumerate_output_devices(dca_device_info* devs, int32_t cap, int32_t* out_count)
{
    if (!out_count) return DCA_ERR_INVALID_ARG;
    return guard("dca_enumerate_output_devices", [&]() -> dca_status {
        // Static enumeration constructs a juce::AudioDeviceManager and
        // scanForDevices() on every backend. ASIO / WASAPI / DirectSound all
        // require a live JUCE MessageManager on the calling thread; .NET
        // worker threads have none. Marshal onto the JuceHost message thread
        // (and lazily spin it up) so enumeration works regardless of which
        // managed thread called us.
        auto& host = JuceHost::instance();
        host.ensureStarted();
        int n = 0;
        host.callSync([&] { n = dotcharter::AudioEngine::enumerateOutputDevices(devs, cap); });
        *out_count = n;
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_enumerate_input_devices(dca_device_info* devs, int32_t cap, int32_t* out_count)
{
    if (!out_count) return DCA_ERR_INVALID_ARG;
    return guard("dca_enumerate_input_devices", [&]() -> dca_status {
        auto& host = JuceHost::instance();
        host.ensureStarted();
        int n = 0;
        host.callSync([&] { n = dotcharter::AudioEngine::enumerateInputDevices(devs, cap); });
        *out_count = n;
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_enumerate_host_devices(
    const char* type_name, int32_t is_input,
    dca_device_info* devs, int32_t cap, int32_t* out_count)
{
    if (!out_count) return DCA_ERR_INVALID_ARG;
    return guard("dca_enumerate_host_devices", [&]() -> dca_status {
        auto& host = JuceHost::instance();
        host.ensureStarted();
        int n = 0;
        host.callSync([&] {
            n = dotcharter::AudioEngine::enumerateHostDevices(type_name, is_input != 0, devs, cap);
        });
        *out_count = n;
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_enumerate_host_types(
    char* type_names, int32_t capacity_entries, int32_t* out_count)
{
    if (!out_count) return DCA_ERR_INVALID_ARG;
    return guard("dca_enumerate_host_types", [&]() -> dca_status {
        auto& host = JuceHost::instance();
        host.ensureStarted();
        int n = 0;
        host.callSync([&] {
            n = dotcharter::AudioEngine::enumerateHostTypes(type_names, capacity_entries);
        });
        *out_count = n;
        return DCA_OK;
    });
}

// --- Engine ----------------------------------------------------------------

DCA_API dca_status DCA_CALL dca_engine_create(const dca_engine_config* cfg, dca_engine_t* out)
{
    return dca_engine_create2(cfg, 0u, out);
}

// ABI v41 (DotCharter #85): the Null-device options travel with the engine, not
// through the process environment. They are applied BEFORE start() so the very
// first device-type registration sees them, and before the handle is published.
DCA_API dca_status DCA_CALL dca_engine_create2(const dca_engine_config* cfg, uint32_t flags, dca_engine_t* out)
{
    if (!out) return DCA_ERR_INVALID_ARG;
    *out = nullptr;   // defined before any failure path
    if ((flags & ~static_cast<uint32_t>(DCA_ENGINE_CREATE_KNOWN_FLAGS)) != 0u)
        return DCA_ERR_INVALID_ARG;   // an unknown bit is rejected, never ignored
    return guard("dca_engine_create", [&]() -> dca_status {
        auto* e = new (std::nothrow) Engine();
        if (!e) return DCA_ERR_INTERNAL;
        e->impl.setNullDeviceOptions((flags & DCA_ENGINE_CREATE_NULL_DEVICE) != 0u,
                                     (flags & DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL) != 0u);

        if (cfg != nullptr)
        {
            // ASIO drivers (and many WASAPI/CoreAudio code paths) require the
            // JUCE MessageManager to exist on the thread that opens them.
            juce::String err;
            const dca_engine_config cfgCopy = *cfg;
            JuceHost::instance().callSync([&]
            {
                try { err = e->impl.start(cfgCopy); }
                catch (const std::exception& ex) { err = juce::String(ex.what()); }
                catch (...) { err = "unknown exception in engine.start()"; }
            });
            if (err.isNotEmpty())
            {
                emitLog(DCA_LOG_ERROR, err.toRawUTF8());
                // On the MM thread, like dca_engine_destroy. start() created the
                // device types there, and WASAPI's DeviceChangeDetector owns a
                // hidden window that only its creating thread can DestroyWindow.
                // Deleted from this (managed) thread the window survived with
                // GWLP_USERDATA pointing at the freed detector, and the next
                // WM_DEVICECHANGE (an interface unplugged or replugged) crashed
                // the MM thread in Timer::startTimer (DotCharter #273).
                JuceHost::instance().callSync([e] { delete e; });
                return DCA_ERR_DEVICE;
            }
        }

        *out = reinterpret_cast<dca_engine_t>(e);
        emitLog(DCA_LOG_INFO, "engine created");
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_engine_destroy(dca_engine_t handle)
{
    return guard("dca_engine_destroy", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (e == nullptr) return DCA_OK;

    // Stop the audio engine BEFORE tearing plugins down. Otherwise the audio
    // thread can still be calling processBlock() on a plugin instance whose
    // editor / state we're about to destroy on the message thread, which
    // crashes badly-behaved hosts (BIAS FX, AGPF, ...) with a stack-buffer
    // overrun (0xC0000409). The stop() removes the audio callback and waits
    // for the device thread to drain.
    //
    // Run stop() on the JuceHost message thread for the same reason as
    // start(): the AudioDeviceManager's close path calls back into the
    // driver (especially ASIO) which expects MM-thread affinity.
    JuceHost::instance().callSync([e] { try { e->impl.stop(); } catch (...) {} });

    // Force-unload any plugins the caller forgot about (typical when the user
    // closes the app window with VST3 editors still open). Strict teardown
    // order — editor first on MM thread, then instance — keeps plugins like
    // BIAS FX, Helix and Neural DSP from access-violating during DLL detach.
    //
    // CONTRACT (D-06): dca_engine_destroy must not race with concurrent
    // dca_plugin_unload calls on plugins owned by the same engine. The
    // managed-side wrapper enforces this by draining and disposing every child
    // handle it has handed out — sources, plugins, MIDI inputs — before
    // invoking dca_engine_destroy, and by LEAKING rather than freeing a handle
    // whose drain times out. (Both of those are wrapper 0.61 behaviours; the
    // drain previously expired after 2 s and returned the handle anyway, which
    // made this contract a hope rather than a guarantee.) The
    // `p->engine = nullptr` write below is therefore unsynchronised by design —
    // there must be no other thread reading `p->engine` at this point.
    // Orphan any sources the caller forgot about (audit #9) — mirror of the
    // plugin force-cleanup below. The transport must die while the engine is
    // still alive (~TransportSource calls engine_.removeSource); the device is
    // already stopped, so the direct delete here is the same safe path
    // dca_source_destroy has always used. The Source shell itself stays
    // allocated: the managed wrapper still holds its handle and will free it
    // via dca_source_destroy, which null-checks `transport` everywhere.
    std::vector<Source*> orphanedSources;
    {
        std::lock_guard<std::mutex> lk(e->sourcesMutex);
        orphanedSources.swap(e->sources);
    }
    if (!orphanedSources.empty())
        emitLog(DCA_LOG_WARN, "engine_destroy: orphaning sources that outlived the engine");
    for (auto* s : orphanedSources)
    {
        s->transport.reset();
        s->engine = nullptr; // skip the registry-remove in dca_source_destroy
    }

    std::vector<Plugin*> leftovers;
    {
        std::lock_guard<std::mutex> lk(e->pluginsMutex);
        leftovers.swap(e->plugins);
    }
    if (!leftovers.empty())
        emitLog(DCA_LOG_WARN, "engine_destroy: force-unloading orphaned plugins");
    for (auto* p : leftovers)
    {
        p->engine = nullptr; // skip the registry-remove in teardownPluginAndDelete
        teardownPluginAndDelete(p);
    }

    // Destroy the engine (and its AudioDeviceManager) on the MM thread too:
    // ~AudioDeviceManager() closes any still-open devices, which for ASIO
    // re-enters the driver's COM apartment and can hang/crash otherwise.
    JuceHost::instance().callSync([e] { delete e; });
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_start(dca_engine_t handle)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    dca_engine_config defaults{};
    defaults.preferred_driver = DCA_DRIVER_DEFAULT;
    defaults.output_channels  = 2;
    // ASIO and many WASAPI/CoreAudio drivers require the JUCE
    // MessageManager to exist on the thread that opens the device. Mirror
    // dca_engine_create / dca_engine_stop and marshal onto the JuceHost
    // message thread.
    juce::String err;
    {
        // W4-D73: the SAME lock the offline rig renders take, so their "no
        // device is open" check cannot go stale under a multi-minute bounce.
        // Try-lock and REFUSE rather than wait: a user pressing play must never
        // block behind an offline render.
        dotcharter::AudioEngine::DeviceLifecycleLock lifecycle(e->impl);
        if (! lifecycle.held())
        {
            emitLog(DCA_LOG_WARN, "dca_engine_start refused: an offline render is in progress");
            return DCA_ERR_DEVICE;
        }
        JuceHost::instance().callSync([&] { try { err = e->impl.start(defaults); } catch (const std::exception& ex) { err = juce::String(ex.what()); } catch (...) { err = "unknown exception in engine.start()"; } });
    }
    if (err.isNotEmpty())
    {
        emitLog(DCA_LOG_ERROR, err.toRawUTF8());
        return DCA_ERR_DEVICE;
    }
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_stop(dca_engine_t handle)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    // Marshal onto the JuceHost message thread for symmetry with
    // dca_engine_destroy / dca_engine_create. ASIO and WASAPI exclusive
    // device-close paths re-enter the driver's COM apartment / message
    // hooks and require MM-thread affinity. Called direct from a .NET
    // worker thread we have observed ASIO drivers hang on close.
    JuceHost::instance().callSync([e] { try { e->impl.stop(); } catch (...) {} });
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_sample_rate(dca_engine_t handle, int32_t* out)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out) return DCA_ERR_INVALID_ARG;
    // The CACHED number, not the device pointer: setAudioDeviceSetup (i.e.
    // dca_engine_reconfigure / dca_engine_set_device_type) DELETES the device,
    // and these getters are polled from arbitrary managed threads.
    *out = (int32_t) e->impl.deviceSnapshot().sampleRate;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_buffer_frames(dca_engine_t handle, int32_t* out)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out) return DCA_ERR_INVALID_ARG;
    *out = (int32_t) e->impl.deviceSnapshot().blockFrames;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_output_latency_samples(dca_engine_t handle, int32_t* out)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out) return DCA_ERR_INVALID_ARG;
    // ABI v34: the device's own figure PLUS the master-bus ceiling's look-ahead.
    // This export answers "how late does audio handed to the engine reach the
    // ear", which is what every consumer uses it for (click alignment, the
    // render-clock anchor, the health readout) — and the limiter is a real stage
    // in that path while it is switched ON.
    //
    // **IT FOLLOWS THE `enabled` FLAG. DO NOT CACHE THIS NUMBER ACROSS A
    // TOGGLE.** Switched off the stage is not in the path at all, so this drops
    // by the look-ahead; switched on it rises by it. The first implementation
    // kept the delay line running when disabled precisely so the figure would
    // stay stable, and the RT gate rejected it — 48 samples of unconditional
    // delay re-timed the whole engine and broke
    // RtEqTrimRampsAndSparseInputIsSilent. The comment here still described that
    // REVERTED design until ABI v35, on the one export click alignment, the
    // render-clock anchor and the health readout all consume, and it told a
    // reader they could cache the value. See rule 1 in MasterLimiter.h, the
    // header at dca_engine_get_master_limiter_latency_samples, and
    // docs/guides/health.md — all three of which always said the opposite.
    // dca_engine_get_master_limiter_latency_samples reports the limiter's own
    // component if a caller needs to decompose it.
    // Still 0 with no device open: the limiter is prepared by prepareMasterBus.
    *out = (int32_t) e->impl.deviceSnapshot().outputLatency
         + (int32_t) e->impl.masterLimiterLatencySamples();
    return DCA_OK;
}

// ---- Master-bus ceiling (v34 (engine)) -----------------------------------
DCA_API dca_status DCA_CALL dca_engine_set_master_limiter(dca_engine_t handle,
                                                          int32_t enabled,
                                                          float   ceiling_db,
                                                          float   release_ms)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    // A NaN is REJECTED, never clamped (the repo rule): every `v < lo` / `v > hi`
    // comparison a clamp is built from is false for a NaN, so a clamp would let
    // it through into the ceiling and make the limiter silent for the session.
    if (!dcabi::finite(ceiling_db) || !dcabi::finite(release_ms)) return DCA_ERR_INVALID_ARG;
    return guard("dca_engine_set_master_limiter", [&]() -> dca_status {
        // The in-process setter REJECTS a non-finite pair too, so the same NaN
        // means the same thing through either door. It used to substitute the
        // defaults there while this export rejected.
        return e->impl.setMasterLimiter(enabled != 0, ceiling_db, release_ms)
                   ? DCA_OK : DCA_ERR_INVALID_ARG;
    });
}

DCA_API dca_status DCA_CALL dca_engine_get_master_limiter(dca_engine_t handle,
                                                          int32_t* out_enabled,
                                                          float*   out_ceiling_db,
                                                          float*   out_release_ms)
{
    // W4-N9: its two siblings below both pre-zero and both reject a call that
    // asked for nothing; this one did neither, so a failing call left the
    // caller's three variables at whatever its stack held and an all-null call
    // returned DCA_OK. Every out-param is zeroed BEFORE the first possible
    // failure — the contract stated at dca_engine_dsp_get_param and honoured by
    // all thirteen v33/v34 getters.
    //
    // Unlike the siblings this one has THREE optional outs, so "asked for
    // nothing" is all three null, not any one of them: a caller polling only the
    // reduction of them is legitimate and must keep working.
    if (out_enabled    != nullptr) *out_enabled    = 0;
    if (out_ceiling_db != nullptr) *out_ceiling_db = 0.0f;
    if (out_release_ms != nullptr) *out_release_ms = 0.0f;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (out_enabled == nullptr && out_ceiling_db == nullptr && out_release_ms == nullptr)
        return DCA_ERR_INVALID_ARG;
    if (out_enabled    != nullptr) *out_enabled    = e->impl.masterLimiterEnabled() ? 1 : 0;
    if (out_ceiling_db != nullptr) *out_ceiling_db = e->impl.masterLimiterCeilingDb();
    if (out_release_ms != nullptr) *out_release_ms = e->impl.masterLimiterReleaseMs();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_master_limiter_reduction_db(dca_engine_t handle,
                                                                       float* out_db)
{
    // *out is zeroed FIRST so a caller that discards the status is no worse off
    // than it was — the same contract every other poll-only getter here honours.
    if (out_db != nullptr) *out_db = 0.0f;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_db) return DCA_ERR_INVALID_ARG;
    *out_db = e->impl.masterLimiterReductionDb();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_master_limiter_latency_samples(dca_engine_t handle,
                                                                          int32_t* out_samples)
{
    if (out_samples != nullptr) *out_samples = 0;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_samples) return DCA_ERR_INVALID_ARG;
    *out_samples = (int32_t) e->impl.masterLimiterLatencySamples();
    return DCA_OK;
}

// In-place reconfigure: wraps AudioDeviceManager::setAudioDeviceSetup so we
// don't have to tear the engine down for buffer/SR/device-selection changes.
// Loaded plugins, transports and mixer state are preserved.
DCA_API dca_status DCA_CALL dca_engine_reconfigure(
    dca_engine_t handle,
    int32_t      sample_rate,
    int32_t      buffer_frames,
    const char*  output_device_id,
    const char*  input_device_id)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;

    // W4-D73: see dca_engine_start. setAudioDeviceSetup OPENS a device, which
    // is exactly what an offline render checked was not happening.
    dotcharter::AudioEngine::DeviceLifecycleLock lifecycle(e->impl);
    if (! lifecycle.held())
    {
        emitLog(DCA_LOG_WARN, "dca_engine_reconfigure refused: an offline render is in progress");
        return DCA_ERR_DEVICE;
    }

    juce::String err;
    JuceHost::instance().callSync([&]
    {
        try
        {
            auto& dm = e->impl.deviceManager();
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            dm.getAudioDeviceSetup(setup);

            const auto prevSetup = setup; // for rollback on failure

            if (sample_rate   > 0) setup.sampleRate = sample_rate;
            if (buffer_frames > 0) setup.bufferSize = buffer_frames;
            if (output_device_id != nullptr && output_device_id[0] != '\0')
                setup.outputDeviceName = juce::String::fromUTF8(output_device_id);
            if (input_device_id != nullptr && input_device_id[0] != '\0')
                setup.inputDeviceName  = juce::String::fromUTF8(input_device_id);

            err = dm.setAudioDeviceSetup(setup, true);
            if (err.isNotEmpty())
            {
                // Roll back to the previous working setup so we don't leave
                // the engine in a half-open state.
                auto rollback = dm.setAudioDeviceSetup(prevSetup, true);
                if (rollback.isNotEmpty())
                    emitLog(DCA_LOG_ERROR,
                            (juce::String("reconfigure rollback also failed: ") + rollback).toRawUTF8());
                return;
            }
            // Sanity: confirm a device is actually open after the swap.
            if (dm.getCurrentAudioDevice() == nullptr)
                err = "Reconfigure left no audio device open.";
        }
        catch (const std::exception& ex) { err = juce::String("reconfigure threw: ") + ex.what(); }
        catch (...)                      { err = "reconfigure threw unknown exception"; }
    });

    if (err.isNotEmpty())
    {
        emitLog(DCA_LOG_ERROR, err.toRawUTF8());
        return DCA_ERR_DEVICE;
    }
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_monitor_gain(dca_engine_t handle, float g)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(g)) return DCA_ERR_INVALID_ARG;
    e->impl.setMonitorGain(g);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_input_pitch_shift(dca_engine_t handle, float semitones)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(semitones)) return DCA_ERR_INVALID_ARG;
    e->impl.setInputPitchShift(semitones);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_input_pitch_shift_mode(dca_engine_t handle, int32_t mode)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    // Three modes since v25 (Tight / Balanced / Smooth). This guard still read
    // {0,1} from the two-engine era, so every "Smooth" push was rejected here
    // and the 35 ms path was unreachable — a shipped, selectable setting that
    // silently did nothing. Keep in sync with AudioEngine::setInputPitchShiftMode
    // and the managed DropTunerEngineMap.
    if (mode < 0 || mode > 2) return DCA_ERR_INVALID_ARG;
    e->impl.setInputPitchShiftMode((int) mode);
    return DCA_OK;
}

// ---- Per-seat drop tune (ABI v31) -----------------------------------------
// A player index past the ceiling is REJECTED, never clamped — the v28 node-id
// rule, for the same reason: clamping would silently detune somebody else's
// guitar.

DCA_API dca_status DCA_CALL dca_engine_set_player_input_pitch_shift(dca_engine_t handle,
                                                                     int32_t player,
                                                                     float semitones)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(semitones)) return DCA_ERR_INVALID_ARG;
    e->impl.setPlayerInputPitchShift((int) player, semitones);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_player_input_pitch_shift_mode(dca_engine_t handle,
                                                                          int32_t player,
                                                                          int32_t mode)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    // Same three modes and the same guard as the seat-0 export above; keep the
    // two in sync with AudioEngine::setPlayerInputPitchShiftMode and the managed
    // DropTunerEngineMap.
    if (mode < 0 || mode > 2) return DCA_ERR_INVALID_ARG;
    e->impl.setPlayerInputPitchShiftMode((int) player, (int) mode);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_device_type(
    dca_engine_t handle,
    const char*  type_name,
    const char*  output_device_id,
    const char*  input_device_id,
    uint32_t     output_channel_mask,
    uint32_t     input_channel_mask)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;

    // W4-D73: see dca_engine_start. This opens a device too.
    dotcharter::AudioEngine::DeviceLifecycleLock lifecycle(e->impl);
    if (! lifecycle.held())
    {
        emitLog(DCA_LOG_WARN, "dca_engine_set_device_type refused: an offline render is in progress");
        return DCA_ERR_DEVICE;
    }

    juce::String err;
    JuceHost::instance().callSync([&]
    {
        try
        {
            auto& dm = e->impl.deviceManager();

            // If a type name was supplied, swap the active AudioIODeviceType
            // first. JUCE 8 needs an explicit scanForDevices() after the
            // swap or the new type's device list is empty and the subsequent
            // setAudioDeviceSetup fails with "No such device".
            if (type_name != nullptr && type_name[0] != '\0')
            {
                bool matched = false;
                for (auto* type : dm.getAvailableDeviceTypes())
                {
                    if (type->getTypeName().equalsIgnoreCase(type_name))
                    {
                        dm.setCurrentAudioDeviceType(type->getTypeName(), true);
                        matched = true;
                        try { type->scanForDevices(); } catch (...) {}
                        break;
                    }
                }
                if (!matched) { err = juce::String("Audio driver not available: ") + type_name; return; }
            }

            juce::AudioDeviceManager::AudioDeviceSetup setup;
            dm.getAudioDeviceSetup(setup);
            const auto prevSetup = setup;

            // Empty device id => pick the host default. Otherwise pin the
            // requested device. Same gotcha as in start(): JUCE's
            // setAudioDeviceSetup() with empty device names falls back to
            // OS-level "default device" which only exists for WASAPI /
            // CoreAudio — ASIO would silently bounce to a different host.
            // Always substitute the active host's first enumerated device
            // when the caller didn't pin one.
            if (output_device_id != nullptr && output_device_id[0] != '\0')
                setup.outputDeviceName = juce::String::fromUTF8(output_device_id);
            else
                setup.outputDeviceName = juce::String();

            if (input_device_id != nullptr && input_device_id[0] != '\0')
                setup.inputDeviceName = juce::String::fromUTF8(input_device_id);
            else
                setup.inputDeviceName = juce::String();

            if (auto* curType = dm.getCurrentDeviceTypeObject())
            {
                if (setup.outputDeviceName.isEmpty())
                {
                    const auto outs = curType->getDeviceNames(false);
                    if (outs.size() > 0) setup.outputDeviceName = outs[0];
                }
                if (setup.inputDeviceName.isEmpty())
                {
                    const auto ins = curType->getDeviceNames(true);
                    if (ins.size() > 0) setup.inputDeviceName = ins[0];
                }
            }

            if (input_channel_mask != 0)
            {
                juce::BigInteger b;
                for (int i = 0; i < 32; ++i) if (input_channel_mask & (1u << i)) b.setBit(i);
                setup.inputChannels           = b;
                setup.useDefaultInputChannels = false;
            }
            else
            {
                setup.useDefaultInputChannels = true;
            }

            if (output_channel_mask != 0)
            {
                juce::BigInteger b;
                for (int i = 0; i < 32; ++i) if (output_channel_mask & (1u << i)) b.setBit(i);
                setup.outputChannels           = b;
                setup.useDefaultOutputChannels = false;
            }
            else
            {
                setup.useDefaultOutputChannels = true;
            }

            // First attempt with the caller-pinned (or auto-picked-first) device.
            err = dm.setAudioDeviceSetup(setup, true);

            // Always walk on failure — even pinned devices get superseded if
            // they're offline (UX > strict pinning). Resets SR / buffer to
            // host-default so cross-host swaps don't get rejected.
            if (err.isNotEmpty())
            {
                if (auto* curType = dm.getCurrentDeviceTypeObject())
                {
                    const auto outs = curType->getDeviceNames(false);
                    const auto ins  = curType->getDeviceNames(true);
                    const int  outN = juce::jmax(1, outs.size());
                    const int  inN  = juce::jmax(1, ins.size());
                    // Some hosts (WASAPI exclusive) reject cross-host buffer
                    // sizes (e.g. ASIO 480 -> WASAPI excl can only do 96/160/...)
                    // Reset SR / buffer to "host default" for the fallback so
                    // the new host gets to pick what it actually supports.
                    auto attemptBase = setup;
                    attemptBase.sampleRate = 0;
                    attemptBase.bufferSize = 0;
                    for (int o = 0; o < outN && err.isNotEmpty(); ++o)
                    {
                        for (int i = 0; i < inN && err.isNotEmpty(); ++i)
                        {
                            auto attempt = attemptBase;
                            attempt.outputDeviceName = (o < outs.size()) ? outs[o] : juce::String();
                            attempt.inputDeviceName  = (i < ins .size()) ? ins [i] : juce::String();
                            if (attempt.outputDeviceName == setup.outputDeviceName
                             && attempt.inputDeviceName  == setup.inputDeviceName) continue;
                            err = dm.setAudioDeviceSetup(attempt, true);
                            if (err.isEmpty())
                            {
                                emitLog(DCA_LOG_INFO,
                                    (juce::String("set_device_type fallback opened '")
                                     + attempt.outputDeviceName + "' / '" + attempt.inputDeviceName + "'").toRawUTF8());
                            }
                        }
                    }
                }
            }

            if (err.isNotEmpty())
            {
                auto rb = dm.setAudioDeviceSetup(prevSetup, true);
                if (rb.isNotEmpty())
                    emitLog(DCA_LOG_ERROR, (juce::String("set_device_type rollback also failed: ") + rb).toRawUTF8());
                return;
            }
            if (dm.getCurrentAudioDevice() == nullptr)
                err = "set_device_type left no audio device open.";
        }
        catch (const std::exception& ex) { err = juce::String("set_device_type threw: ") + ex.what(); }
        catch (...)                      { err = "set_device_type threw unknown exception"; }
    });

    if (err.isNotEmpty())
    {
        emitLog(DCA_LOG_ERROR, err.toRawUTF8());
        return DCA_ERR_DEVICE;
    }
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_input_gain(dca_engine_t handle, float g)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(g)) return DCA_ERR_INVALID_ARG;
    e->impl.setInputGain(g);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_input_peak(dca_engine_t handle, float* out)
{
    if (!out) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    *out = e->impl.getAndResetInputPeak();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_input_capture_channel(dca_engine_t handle, int32_t channel)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    e->impl.setInputCaptureChannel((int)channel);
    return DCA_OK;
}

// W4-N10. PURE ALIASES FOR SEAT 0; the guards live in the exports they delegate
// to — the v35 convention every other pre-v28 export follows. These four were
// COPIES of their _for_player twins instead: eight near-identical bodies, two
// independent drift surfaces for one behaviour, and the copies also left
// *out_written at whatever the caller's stack held on every failure path,
// contrary to the "zero every out-param before the first possible failure" rule
// the rest of this file states and honours.
DCA_API dca_status DCA_CALL dca_engine_read_input_capture(dca_engine_t handle,
                                                          float* dest,
                                                          int32_t count,
                                                          int32_t* out_written)
{
    return dca_engine_read_input_capture_for_player(handle, 0, dest, count, out_written);
}

DCA_API dca_status DCA_CALL dca_engine_read_input_capture_stream(dca_engine_t handle,
                                                                 float* dest,
                                                                 int32_t count,
                                                                 int32_t* out_written)
{
    return dca_engine_read_input_capture_stream_for_player(handle, 0, dest, count, out_written);
}

DCA_API dca_status DCA_CALL dca_engine_read_input_capture_stream2(dca_engine_t handle,
                                                                  float* dest,
                                                                  int32_t count,
                                                                  int32_t* out_written,
                                                                  uint64_t* out_stream_position)
{
    return dca_engine_read_input_capture_stream2_for_player(handle, 0, dest, count,
                                                            out_written, out_stream_position);
}

DCA_API dca_status DCA_CALL dca_engine_get_input_capture_sync(dca_engine_t handle,
                                                              uint64_t* out_captured_samples,
                                                              int64_t*  out_host_ticks)
{
    return dca_engine_get_input_capture_sync_for_player(handle, 0,
                                                        out_captured_samples, out_host_ticks);
}

// ---- Per-player seats: routing + capture (ABI v28) ------------------------

DCA_API dca_status DCA_CALL dca_engine_set_player_count(dca_engine_t handle, int32_t count)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (count < 1 || count > DCA_MAX_PLAYERS) return DCA_ERR_INVALID_ARG;
    e->impl.setPlayerCount((int) count);
    return DCA_OK;
}

// 0 for a null handle is DELIBERATE and is the documented sentinel (see the
// wrapper's JuceAudioEngine.GetPlayerCount, which returns 0 for a disposed
// engine of its own accord). Unlike the two source getters above it is not a
// plausible answer: a live engine clamps playerCount_ to 1..DCA_MAX_PLAYERS and
// can never report 0. This export has no status to return, so do not "fix" it
// to -1 without changing the wrapper's contract too. Pinned by
// NullHandleRejectionTests.
DCA_API int32_t DCA_CALL dca_engine_get_player_count(dca_engine_t handle)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    return e ? (int32_t) e->impl.playerCount() : 0;
}

DCA_API dca_status DCA_CALL dca_engine_set_player_input_channel(dca_engine_t handle,
                                                                 int32_t player, int32_t channel)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    e->impl.setPlayerInputChannel((int) player, (int) channel);
    return DCA_OK;
}

DCA_API int32_t DCA_CALL dca_engine_get_player_input_channel(dca_engine_t handle, int32_t player)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return -1;
    return (int32_t) e->impl.playerInputChannel((int) player);
}

DCA_API dca_status DCA_CALL dca_engine_set_player_monitor_gain(dca_engine_t handle,
                                                                int32_t player, float linear_gain)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(linear_gain)) return DCA_ERR_INVALID_ARG;
    e->impl.setPlayerMonitorGain((int) player, linear_gain);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_player_input_peak(dca_engine_t handle,
                                                              int32_t player, float* out_peak)
{
    if (!out_peak) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    *out_peak = e->impl.getAndResetPlayerInputPeak((int) player);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_read_input_capture_for_player(dca_engine_t handle,
                                                                     int32_t player,
                                                                     float* dest,
                                                                     int32_t count,
                                                                     int32_t* out_written)
{
    // Zeroed BEFORE the first possible failure. See dca_engine_dsp_get_param.
    if (out_written) *out_written = 0;
    if (!dest || count <= 0) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    const int written = e->impl.readInputCapture((int) player, dest, (int) count);
    if (out_written) *out_written = written;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_read_input_capture_stream_for_player(dca_engine_t handle,
                                                                            int32_t player,
                                                                            float* dest,
                                                                            int32_t count,
                                                                            int32_t* out_written)
{
    if (out_written) *out_written = 0;
    if (!dest || count <= 0) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    const int written = e->impl.readInputCaptureStream((int) player, dest, (int) count);
    if (out_written) *out_written = written;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_read_input_capture_stream2_for_player(dca_engine_t handle,
                                                                             int32_t player,
                                                                             float* dest,
                                                                             int32_t count,
                                                                             int32_t* out_written,
                                                                             uint64_t* out_stream_position)
{
    if (out_written) *out_written = 0;
    if (out_stream_position) *out_stream_position = 0;
    if (!dest || count <= 0) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    uint64_t pos = 0;
    const int written = e->impl.readInputCaptureStream2((int) player, dest, (int) count, pos);
    if (out_written) *out_written = written;
    if (out_stream_position) *out_stream_position = pos;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_input_capture_sync_for_player(dca_engine_t handle,
                                                                         int32_t player,
                                                                         uint64_t* out_captured_samples,
                                                                         int64_t*  out_host_ticks)
{
    if (out_captured_samples) *out_captured_samples = 0;
    if (out_host_ticks)       *out_host_ticks       = 0;
    if (!out_captured_samples || !out_host_ticks) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    uint64_t samples = 0;
    int64_t  ticks   = 0;
    if (!e->impl.getInputCaptureSync((int) player, samples, ticks)) return DCA_ERR_INVALID_ARG;
    *out_captured_samples = samples;
    *out_host_ticks       = ticks;
    return DCA_OK;
}

// ---- Positional capture read + input injection (ABI v37) --------------------

DCA_API dca_status DCA_CALL dca_engine_read_input_capture_from_for_player(dca_engine_t handle,
                                                                          int32_t player,
                                                                          uint64_t from,
                                                                          float* dest,
                                                                          int32_t count,
                                                                          int32_t* out_written,
                                                                          uint64_t* out_first)
{
    if (out_written) *out_written = 0;
    if (out_first)   *out_first   = 0;
    if (!dest || count <= 0 || !out_written || !out_first) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    uint64_t first = 0;
    const int written = e->impl.readInputCaptureFrom((int) player, from, dest, (int) count, first);
    *out_written = written;
    *out_first   = first;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_input_injection_for_player(dca_engine_t handle,
                                                                      int32_t player,
                                                                      const float* mono,
                                                                      int64_t frames,
                                                                      int32_t loop)
{
    return guard("dca_engine_set_input_injection_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !mono || frames <= 0 || !dotcharter::AudioEngine::validPlayer((int) player))
        return DCA_ERR_INVALID_ARG;
    return e->impl.setInputInjection((int) player, mono, frames, loop != 0) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_clear_input_injection_for_player(dca_engine_t handle, int32_t player)
{
    return guard("dca_engine_clear_input_injection_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.clearInputInjection((int) player) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_set_input_injection_playing_for_player(dca_engine_t handle,
                                                                              int32_t player,
                                                                              int32_t playing)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.setInputInjectionPlaying((int) player, playing != 0) ? DCA_OK : DCA_ERR_INVALID_ARG;
}

DCA_API dca_status DCA_CALL dca_engine_set_input_injection_loop_for_player(dca_engine_t handle,
                                                                           int32_t player,
                                                                           int32_t loop)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.setInputInjectionLoop((int) player, loop != 0) ? DCA_OK : DCA_ERR_INVALID_ARG;
}

DCA_API dca_status DCA_CALL dca_engine_seek_input_injection_for_player(dca_engine_t handle,
                                                                       int32_t player,
                                                                       int64_t frame)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.seekInputInjection((int) player, frame) ? DCA_OK : DCA_ERR_INVALID_ARG;
}

DCA_API dca_status DCA_CALL dca_engine_get_input_injection_status_for_player(dca_engine_t handle,
                                                                             int32_t player,
                                                                             int64_t* out_position,
                                                                             int64_t* out_length,
                                                                             int32_t* out_playing,
                                                                             int32_t* out_loop)
{
    if (out_position) *out_position = 0;
    if (out_length)   *out_length   = 0;
    if (out_playing)  *out_playing  = 0;
    if (out_loop)     *out_loop     = 0;
    if (!out_position || !out_length || !out_playing || !out_loop) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    int64_t pos = 0, len = 0;
    bool playing = false, loop = false;
    if (!e->impl.getInputInjectionStatus((int) player, pos, len, playing, loop)) return DCA_ERR_INVALID_ARG;
    *out_position = pos;
    *out_length   = len;
    *out_playing  = playing ? 1 : 0;
    *out_loop     = loop ? 1 : 0;
    return DCA_OK;
}

// ---- Chart source + instrument route (ABI v38) ------------------------------

DCA_API dca_status DCA_CALL dca_engine_set_song_clock_source(dca_engine_t handle, dca_source_t source)
{
    return guard("dca_engine_set_song_clock_source", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    dotcharter::TransportSource* ts = nullptr;
    if (source != nullptr)
    {
        auto* src = reinterpret_cast<Source*>(source);
        // A source of ANOTHER engine (or one orphaned by its engine's destroy)
        // is rejected: the clock is read on this engine's audio thread.
        if (src->engine != e || !src->transport) return DCA_ERR_INVALID_ARG;
        ts = src->transport.get();
    }
    return e->impl.setSongClockSource(ts) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_chart_source_set_notes_for_player(dca_engine_t handle,
                                                                         int32_t player,
                                                                         const dca_chart_note* notes,
                                                                         int32_t note_count,
                                                                         int32_t note_stride_bytes,
                                                                         const dca_chart_bend_point* bends,
                                                                         int32_t bend_count)
{
    return guard("dca_engine_chart_source_set_notes_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    std::string error;
    if (!e->impl.setChartNotes((int) player, notes, note_count, note_stride_bytes, bends, bend_count, error))
    {
        emitLog(DCA_LOG_WARN, ("chart_source_set_notes rejected: " + error).c_str());
        return DCA_ERR_INVALID_ARG;
    }
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_chart_source_set_notes_for_player2(dca_engine_t handle,
                                                                          int32_t player,
                                                                          const dca_chart_note* notes,
                                                                          int32_t note_count,
                                                                          int32_t note_stride_bytes,
                                                                          int64_t notes_byte_length,
                                                                          const dca_chart_bend_point* bends,
                                                                          int32_t bend_count)
{
    return guard("dca_engine_chart_source_set_notes_for_player2", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    if (notes_byte_length < 0) return DCA_ERR_INVALID_ARG;
    std::string error;
    if (!e->impl.setChartNotes((int) player, notes, note_count, note_stride_bytes, bends, bend_count, error,
                               notes_byte_length))
    {
        emitLog(DCA_LOG_WARN, ("chart_source_set_notes rejected: " + error).c_str());
        return DCA_ERR_INVALID_ARG;
    }
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_chart_source_set_enabled_for_player(dca_engine_t handle,
                                                                           int32_t player,
                                                                           int32_t enabled)
{
    return guard("dca_engine_chart_source_set_enabled_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.setChartSourceEnabled((int) player, enabled != 0) ? DCA_OK : DCA_ERR_INTERNAL;
});
}

DCA_API dca_status DCA_CALL dca_engine_chart_source_set_voice_for_player(dca_engine_t handle,
                                                                         int32_t player,
                                                                         int32_t voice)
{
    return guard("dca_engine_chart_source_set_voice_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.setChartVoice((int) player, (int) voice) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

// ---- Instrument models (ABI v40, #50) -----------------------------------------
// The ABI values ARE the voice's enums: one table (dcvoice_models.h).
static_assert(DCA_CHART_MODEL_SEMI_HOLLOW == dcvoice::kModelSemiHollow && DCA_CHART_MODEL_STRAT_SSS == dcvoice::kModelStratSSS
              && DCA_CHART_MODEL_STRAT_HSS == dcvoice::kModelStratHSS && DCA_CHART_MODEL_TELE == dcvoice::kModelTele
              && DCA_CHART_MODEL_LES_PAUL == dcvoice::kModelLesPaul && DCA_CHART_MODEL_JAZZ_BASS == dcvoice::kModelJazzBass
              && DCA_CHART_MODEL_P_BASS == dcvoice::kModelPBass && DCA_CHART_MODEL_PJ_BASS == dcvoice::kModelPJBass
              && DCA_CHART_MODEL_HOLLOW_BASS == dcvoice::kModelHollowBass, "DCA_CHART_MODEL_* drifted from dcvoice::ModelId");
static_assert(DCA_CHART_PICKUP_NECK == dcvoice::kPickNeck && DCA_CHART_PICKUP_NECK_MIDDLE == dcvoice::kPickNeckMiddle
              && DCA_CHART_PICKUP_MIDDLE == dcvoice::kPickMiddle && DCA_CHART_PICKUP_MIDDLE_BRIDGE == dcvoice::kPickMiddleBridge
              && DCA_CHART_PICKUP_BRIDGE == dcvoice::kPickBridge && DCA_CHART_PICKUP_NECK_BRIDGE == dcvoice::kPickNeckBridge,
              "DCA_CHART_PICKUP_* drifted from dcvoice::PickupSel");
static_assert(sizeof(dca_chart_source_status) == 112, "dca_chart_source_status is append-only");
static_assert(sizeof(dca_chart_model_info) == 48, "dca_chart_model_info layout");

DCA_API dca_status DCA_CALL dca_engine_chart_source_set_instrument_for_player(dca_engine_t handle,
                                                                              int32_t player,
                                                                              int32_t model,
                                                                              int32_t pickup,
                                                                              uint32_t flags)
{
    return guard("dca_engine_chart_source_set_instrument_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    if ((flags & ~DCA_CHART_INSTRUMENT_COIL_SPLIT) != 0u) return DCA_ERR_INVALID_ARG;
    dcvoice::InstrumentConfig c;
    c.model = model;
    c.pickup = pickup;
    c.coilSplit = (flags & DCA_CHART_INSTRUMENT_COIL_SPLIT) != 0u;
    return e->impl.setChartInstrument((int) player, c) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_chart_source_get_instrument_for_player(dca_engine_t handle,
                                                                              int32_t player,
                                                                              int32_t bass,
                                                                              int32_t* out_model,
                                                                              int32_t* out_pickup,
                                                                              uint32_t* out_flags)
{
    return guard("dca_engine_chart_source_get_instrument_for_player", [&]() -> dca_status {
    if (out_model) *out_model = 0;
    if (out_pickup) *out_pickup = 0;
    if (out_flags) *out_flags = 0u;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_model || !out_pickup || !out_flags || !dotcharter::AudioEngine::validPlayer((int) player))
        return DCA_ERR_INVALID_ARG;
    dcvoice::InstrumentConfig c;
    if (!e->impl.getChartInstrument((int) player, bass != 0, c)) return DCA_ERR_INVALID_ARG;
    *out_model = c.model;
    *out_pickup = c.pickup;
    *out_flags = c.coilSplit ? DCA_CHART_INSTRUMENT_COIL_SPLIT : 0u;
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_chart_model_get_info(int32_t model, dca_chart_model_info* out_info)
{
    return guard("dca_chart_model_get_info", [&]() -> dca_status {
    if (!out_info) return DCA_ERR_INVALID_ARG;
    const int32_t callerBytes = out_info->struct_bytes;
    if (callerBytes < (int32_t) (2 * sizeof(int32_t))) return DCA_ERR_INVALID_ARG;
    const dcvoice::ModelSpec* m = dcvoice::findModel(model);
    if (m == nullptr) return DCA_ERR_INVALID_ARG;
    dca_chart_model_info local{};
    local.bass = m->bass ? 1 : 0;
    local.default_pickup = m->defaultSel;
    local.has_coil_split = dcvoice::hasCoilSplit(*m) ? 1 : 0;
    local.installed = 1;
    for (int i = 0; i < m->positionCount; ++i)
    {
        const dcvoice::PositionSpec& pos = m->pos[i];
        local.pickup_mask |= 1u << pos.sel;
        if (!dcsynth::voicePackInstalled(pos.sourcePack)) local.installed = 0;
        bool all = true, any = false;
        for (int variant = 0; variant < (m->bass ? 2 : 1); ++variant)
        {
            const bool same = dcvoice::makeConversionPlan(*m, pos.full, pos.sourcePack, variant).identity;
            all = all && same;
            any = any || same;
        }
        if (all) local.recorded_mask |= 1u << pos.sel;
        if (any) local.recorded_any_mask |= 1u << pos.sel;
    }
    const int32_t copyBytes = juce::jmin(callerBytes, (int32_t) sizeof(dca_chart_model_info));
    local.struct_bytes = copyBytes;
    std::memcpy(out_info, &local, (size_t) copyBytes);
    return DCA_OK;
});
}

DCA_API int64_t DCA_CALL dca_chart_voice_pack_bytes(void)
{
    try { return dcsynth::livePackBytes(); }
    catch (...) { return 0; }
}

DCA_API dca_status DCA_CALL dca_engine_chart_source_set_params_for_player(dca_engine_t handle,
                                                                          int32_t player,
                                                                          float gain_linear,
                                                                          double offset_seconds,
                                                                          int32_t mode)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.setChartParams((int) player, gain_linear, offset_seconds, (int) mode)
               ? DCA_OK : DCA_ERR_INVALID_ARG;
}

DCA_API dca_status DCA_CALL dca_engine_chart_source_get_status_for_player(dca_engine_t handle,
                                                                          int32_t player,
                                                                          dca_chart_source_status* out_status)
{
    return guard("dca_engine_chart_source_get_status_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_status) return DCA_ERR_INVALID_ARG;
    // cbSize contract, exactly as dca_engine_get_xrun_stats: fill a local of
    // OUR size, copy the smaller of the two, report what was written.
    const int32_t callerBytes = out_status->struct_bytes;
    constexpr int32_t kMinBytes = static_cast<int32_t>(4 * sizeof(int32_t));
    if (callerBytes < kMinBytes) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    dca_chart_source_status local{};
    local.struct_bytes = static_cast<int32_t>(sizeof(dca_chart_source_status));
    if (!e->impl.getChartStatus((int) player, local)) return DCA_ERR_INVALID_ARG;
    const int32_t copyBytes = juce::jmin(callerBytes, static_cast<int32_t>(sizeof(dca_chart_source_status)));
    local.struct_bytes = copyBytes;
    std::memcpy(out_status, &local, static_cast<size_t>(copyBytes));
    return DCA_OK;
});
}

DCA_API int32_t DCA_CALL dca_chart_voice_is_available(int32_t voice)
{
    return (int32_t) dcsynth::voiceAvailability((int) voice);
}

DCA_API dca_status DCA_CALL dca_engine_set_instrument_route(dca_engine_t handle, int32_t player)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    return e->impl.setInstrumentRoute((int) player) ? DCA_OK : DCA_ERR_INVALID_ARG;
}

DCA_API dca_status DCA_CALL dca_engine_get_instrument_route(dca_engine_t handle, int32_t* out_player)
{
    if (out_player) *out_player = -1;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_player) return DCA_ERR_INVALID_ARG;
    *out_player = (int32_t) e->impl.instrumentRoute();
    return DCA_OK;
}

// W4-N5. Two faults, both of which this file had already ruled on elsewhere:
//
//  1. showAsioControlPanel dereferences getCurrentAudioDevice() — a raw pointer
//     setAudioDeviceSetup DELETES — and it holds it across showControlPanel(),
//     a MODAL vendor dialog that can sit open for minutes. Every other reader in
//     the engine was moved to deviceSnapshot() when that mechanism landed; this
//     one was missed, and it is the worst of the set, because the window in
//     which dca_engine_reconfigure / dca_engine_set_device_type can free the
//     pointer under it is the whole time the dialog is up.
//
//     The snapshot cannot serve this call — a control panel needs the DEVICE,
//     not its numbers — so the answer is the other half of the rule: run on the
//     thread that OWNS the pointer. reconfigure and set_device_type both go
//     through callSync too, and callSync serialises, so the read, the panel and
//     any teardown can no longer interleave at all.
//
//  2. It opened a native window from whatever managed thread called it.
//     create / destroy / start / stop / reconfigure / set_device_type are ALL
//     marshalled because ASIO re-enters the driver's COM apartment; a driver's
//     control panel is the single most apartment-sensitive thing it exposes.
//
// Guarded, and it was one of the unguarded exports: an ASIO control panel is
// third-party UI code, which is exactly the population guard() exists for.
//
// ---- W5-E9: WHAT THIS COSTS, STATED RATHER THAN FIXED --------------------
//
// THIS IS THE ONE EXPORT IN THIS FILE THAT CAN OCCUPY THE JuceHost MESSAGE
// THREAD INDEFINITELY. showControlPanel() is a MODAL vendor dialog and can sit
// open for minutes, and while it is up EVERY other callSync export queues
// behind it — dca_engine_start, _stop, _reconfigure, _set_device_type, both
// device enumerations, and dca_engine_destroy. Before the marshalling above
// only the CALLING thread blocked; the trade was made knowingly and the
// alternative is worse (a raw device pointer held across a modal dialog while
// reconfigure is free to delete it, which is the fault this was written for).
//
// It is NOT a self-deadlock: callSync short-circuits through
// MessageManager::existsAndIsCurrentThread(), so calling from the message
// thread runs the body inline.
//
// THE MANAGED CONSEQUENCE, because it is the one a user meets: closing the app
// with the panel open means NativeHandleOwner's drain sees the in-flight call,
// times out at 60 s, logs at Error and LEAKS the engine. A leak beats a
// use-after-free, and that is the documented behaviour — not a bug report.
//
// Deliberately NOT "fixed" by making it async. callAsync would unblock this
// caller and nothing else: the panel still owns the message thread, so every
// other callSync queues exactly as it does now, and the export would lose the
// three error answers it returns TODAY ("no device", "no control panel", "the
// device declined") which the managed TryShowAsioControlPanel surfaces. Running
// the panel on its own thread gives the device pointer back to a thread that
// does not own the driver's COM apartment — i.e. it reopens W4-N5. If this is
// ever revisited, the shape to reach for is a host-owned modal-loop escape in
// JuceHost, not a change here.
DCA_API dca_status DCA_CALL dca_engine_show_asio_panel(dca_engine_t handle, void* parent)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    return guard("dca_engine_show_asio_panel", [&]() -> dca_status
    {
        juce::String err;
        // The try lives INSIDE the lambda, exactly as dca_engine_start's does:
        // callFunctionOnMessageThread runs the body on the MM thread, so an
        // exception out of it unwinds THERE and the guard() out here would
        // never see it. The guard still covers the marshalling itself.
        JuceHost::instance().callSync([&]
        {
            try { err = e->impl.showAsioControlPanel(parent); }
            catch (const std::exception& ex) { err = juce::String(ex.what()); }
            catch (...) { err = "unknown exception in showAsioControlPanel()"; }
        });
        if (err.isNotEmpty())
        {
            emitLog(DCA_LOG_WARN, err.toRawUTF8());
            return DCA_ERR_DEVICE;
        }
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_engine_set_master_volume(dca_engine_t handle, float g)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(g)) return DCA_ERR_INVALID_ARG;
    e->impl.setMasterGain(g);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_high_pass(dca_engine_t handle, float freq, float q)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(freq, q)) return DCA_ERR_INVALID_ARG;
    e->impl.setHighPass(freq, q);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_low_pass(dca_engine_t handle, float freq, float q)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(freq, q)) return DCA_ERR_INVALID_ARG;
    e->impl.setLowPass(freq, q);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_band_pass(dca_engine_t handle, float freq, float q)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(freq, q)) return DCA_ERR_INVALID_ARG;
    e->impl.setBandPass(freq, q);
    return DCA_OK;
}

// ---- Parametric EQ (ABI v29) --------------------------------------------

DCA_API dca_status DCA_CALL dca_engine_set_eq_band(dca_engine_t handle, int32_t index, int32_t type,
                                                   float freq_hz, float q, float gain_db, int32_t enabled)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (index < 0 || index >= DCA_EQ_BAND_COUNT) return DCA_ERR_INVALID_ARG;
    if (type < 0 || type > DCA_EQ_NOTCH) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(freq_hz, q, gain_db)) return DCA_ERR_INVALID_ARG;
    e->impl.setEqBand(index, type, freq_hz, q, gain_db, enabled != 0);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_eq_enabled(dca_engine_t handle, int32_t enabled)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    e->impl.setEqEnabled(enabled != 0);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_eq_solo(dca_engine_t handle, int32_t band_index)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    // A band index past the end is "no solo", not an error: the managed side
    // clears a solo by passing -1 and nothing should have to know the count.
    e->impl.setEqSolo(band_index);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_eq_route(dca_engine_t handle, int32_t route)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (route < DCA_EQ_ROUTE_SONG || route > DCA_EQ_ROUTE_BOTH) return DCA_ERR_INVALID_ARG;
    e->impl.setEqRoute(route);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_eq_trim(dca_engine_t handle, float trim_db)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(trim_db)) return DCA_ERR_INVALID_ARG;
    e->impl.setEqTrim(trim_db);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_eq_active_count(dca_engine_t handle, int32_t* out_count)
{
    if (!out_count) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) { *out_count = 0; return DCA_ERR_INVALID_ARG; }
    *out_count = e->impl.eqActiveCount();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_read_eq_spectrum(dca_engine_t handle, float* dest_db,
                                                        int32_t bin_count, int32_t* out_written)
{
    if (!out_written) return DCA_ERR_INVALID_ARG;
    *out_written = 0;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dest_db || bin_count < 8 || bin_count > 512) return DCA_ERR_INVALID_ARG;
    *out_written = e->impl.readEqSpectrum(dest_db, bin_count);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_enumerate_output_devices(dca_engine_t handle, dca_device_info* devs, int32_t cap, int32_t* out_count)
{
    return guard("dca_engine_enumerate_output_devices", [&]() -> dca_status {
    if (!out_count) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) { *out_count = 0; return DCA_ERR_INVALID_ARG; }
    *out_count = e->impl.enumerateOutputDevicesLive(devs, cap);
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_enumerate_input_devices(dca_engine_t handle, dca_device_info* devs, int32_t cap, int32_t* out_count)
{
    return guard("dca_engine_enumerate_input_devices", [&]() -> dca_status {
    if (!out_count) return DCA_ERR_INVALID_ARG;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) { *out_count = 0; return DCA_ERR_INVALID_ARG; }
    *out_count = e->impl.enumerateInputDevicesLive(devs, cap);
    return DCA_OK;
});
}

// ABI v49: the open device's channel names. On the message thread, because the
// device pointer is only stable there (see AudioEngine::channelNames).
DCA_API dca_status DCA_CALL dca_engine_get_channel_names(dca_engine_t handle, int32_t is_input,
                                                         const char* device_name, char* names,
                                                         int32_t capacity_entries, int32_t* out_count)
{
    if (!out_count) return DCA_ERR_INVALID_ARG;
    *out_count = 0;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || capacity_entries < 0 || (names == nullptr && capacity_entries > 0)) return DCA_ERR_INVALID_ARG;
    return guard("dca_engine_get_channel_names", [&]() -> dca_status {
        int n = 0;
        JuceHost::instance().callSync([&] {
            n = e->impl.channelNames(is_input != 0, device_name, names, capacity_entries);
        });
        *out_count = n;
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_engine_set_mirror_device(dca_engine_t handle, const char* device_id)
{
    return guard("dca_engine_set_mirror_device", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    const juce::String name = (device_id != nullptr) ? juce::String::fromUTF8(device_id) : juce::String();

    // Opening / closing a WASAPI device pulls in COM apartments; run on the
    // dedicated JUCE message thread so it is always the same one across calls.
    juce::String err;
    JuceHost::instance().callSync([&]
    {
        try { err = e->impl.setMirrorDevice(name); }
        catch (const std::exception& ex) { err = juce::String("mirror exception: ") + ex.what(); }
        catch (...)                      { err = "mirror: unknown exception"; }
    });

    if (err.isNotEmpty())
        return DCA_ERR_DEVICE;
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_play_click(dca_engine_t handle, int32_t click_type)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (click_type < 0 || click_type > 2) return DCA_ERR_INVALID_ARG;
    e->impl.triggerClick(click_type);
    return DCA_OK;
}

// ---- Sample-accurate click scheduling (ABI v26) --------------------------

DCA_API dca_status DCA_CALL dca_engine_get_render_clock(dca_engine_t handle,
                                                        int64_t* out_render_frames,
                                                        int64_t* out_host_ticks)
{
    if (out_render_frames) *out_render_frames = 0;
    if (out_host_ticks)    *out_host_ticks    = 0;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_render_frames || !out_host_ticks) return DCA_ERR_INVALID_ARG;
    int64_t frames = 0, ticks = 0;
    if (!e->impl.getRenderClock(frames, ticks)) return DCA_ERR_INVALID_ARG;
    *out_render_frames = frames;
    *out_host_ticks    = ticks;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_schedule_click(dca_engine_t handle,
                                                      int32_t click_type,
                                                      int64_t render_frame)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (click_type < 0 || click_type > 2) return DCA_ERR_INVALID_ARG;
    e->impl.scheduleClick(click_type, render_frame);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_clear_scheduled_clicks(dca_engine_t handle)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    e->impl.clearScheduledClicks();
    return DCA_OK;
}

// ---- Dropout / xrun counters + device health ----------------------------

DCA_API dca_status DCA_CALL dca_engine_get_xrun_stats(dca_engine_t handle, dca_xrun_stats* out)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out) return DCA_ERR_INVALID_ARG;

    // cbSize contract: the CALLER declares how big its struct is, we fill a
    // local of OUR size and copy across the smaller of the two. That is what
    // makes appending a field at the end of dca_xrun_stats additive rather than
    // an ABI break — an older caller simply never sees the new tail, and a
    // NEWER caller against an older DLL gets struct_bytes back saying so.
    const int32_t callerBytes = out->struct_bytes;
    // The four leading int32s (struct_bytes, device_open, device_running,
    // device_xruns) are the minimum useful answer; anything smaller is a caller
    // that filled the field in wrong.
    constexpr int32_t kMinBytes = static_cast<int32_t>(4 * sizeof(int32_t));
    if (callerBytes < kMinBytes) return DCA_ERR_INVALID_ARG;

    dca_xrun_stats local{};
    local.struct_bytes = static_cast<int32_t>(sizeof(dca_xrun_stats));
    e->impl.fillXrunStats(local);

    const int32_t copyBytes = juce::jmin(callerBytes, static_cast<int32_t>(sizeof(dca_xrun_stats)));
    std::memcpy(out, &local, static_cast<size_t>(copyBytes));
    out->struct_bytes = copyBytes;   // OUT: how much we actually wrote
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_reset_xrun_stats(dca_engine_t handle)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    e->impl.resetXrunStats();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_callback_load(dca_engine_t handle,
                                                         float* out_average,
                                                         float* out_peak)
{
    if (out_average) *out_average = 0.0f;
    if (out_peak)    *out_peak    = 0.0f;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || (!out_average && !out_peak)) return DCA_ERR_INVALID_ARG;
    float average = 0.0f, peak = 0.0f;
    e->impl.getCallbackLoad(average, peak);
    if (out_average) *out_average = average;
    if (out_peak)    *out_peak    = peak;
    return DCA_OK;
}

// ---- External process insert (v47) --------------------------------------

DCA_API dca_status DCA_CALL dca_engine_extproc_open_for_player(dca_engine_t handle, int32_t player,
                                                                 const char* mapping_name,
                                                                 const char* magic)
{
    return guard("dca_engine_extproc_open_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !mapping_name || !dotcharter::AudioEngine::validPlayer((int) player))
        return DCA_ERR_INVALID_ARG;
    return e->impl.openExternalInsert((int) player, mapping_name, magic);
});
}

DCA_API dca_status DCA_CALL dca_engine_extproc_open(dca_engine_t handle,
                                                    const char* mapping_name,
                                                    const char* magic)
{
    return dca_engine_extproc_open_for_player(handle, 0, mapping_name, magic);
}

DCA_API dca_status DCA_CALL dca_engine_extproc_close_for_player(dca_engine_t handle, int32_t player)
{
    return guard("dca_engine_extproc_close_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    e->impl.closeExternalInsert((int) player);
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_extproc_close(dca_engine_t handle)
{
    return dca_engine_extproc_close_for_player(handle, 0);
}

DCA_API dca_status DCA_CALL dca_engine_extproc_get_stats_for_player(dca_engine_t handle, int32_t player,
                                                                      dca_extproc_stats* out_stats)
{
    return guard("dca_engine_extproc_get_stats_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!out_stats) return DCA_ERR_INVALID_ARG;
    // cbSize contract, exactly as dca_engine_get_xrun_stats: fill a local of OUR
    // size, copy the smaller of the two, report what was written.
    const int32_t callerBytes = out_stats->struct_bytes;
    constexpr int32_t kMinBytes = static_cast<int32_t>(2 * sizeof(int32_t));
    if (callerBytes < kMinBytes) return DCA_ERR_INVALID_ARG;
    const int32_t copyBytes = juce::jmin(callerBytes, static_cast<int32_t>(sizeof(dca_extproc_stats)));
    std::memset(out_stats, 0, static_cast<size_t>(copyBytes));
    out_stats->struct_bytes = callerBytes;
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    dca_extproc_stats local{};
    e->impl.getExternalInsertStats((int) player, local);
    local.struct_bytes = copyBytes;
    std::memcpy(out_stats, &local, static_cast<size_t>(copyBytes));
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_extproc_get_stats(dca_engine_t handle, dca_extproc_stats* out_stats)
{
    return dca_engine_extproc_get_stats_for_player(handle, 0, out_stats);
}

// ---- In-house DSP tone rig (0.16) --------------------------------------

namespace {
// Bounded, always-null-terminated copy of a std::string into a caller buffer
// (the ABI cpp has no access to AudioEngine.cpp's copyTruncated). Truncates at
// a UTF-8 CODE POINT boundary — a torn sequence decodes to U+FFFD on the
// managed side, so a short buffer would CORRUPT the tail rather than clip it.
void copyStdTruncated(char* dest, int32_t cap, const std::string& src)
{
    if (dest == nullptr || cap <= 0) return;
    const size_t n = dcabi::utf8TruncatedLength(src.data(), src.size(), (size_t) cap - 1);
    if (n > 0) std::memcpy(dest, src.data(), n);
    dest[n] = '\0';
}
} // namespace

DCA_API int32_t DCA_CALL dca_engine_dsp_add_node(dca_engine_t handle, const char* module_type_id)
{
    // Pure alias for seat 0; the guard lives in the export it delegates to.
    return dca_engine_dsp_add_node_for_player(handle, 0, module_type_id);
}

DCA_API int32_t DCA_CALL dca_engine_dsp_add_node_for_player(dca_engine_t handle, int32_t player,
                                                            const char* module_type_id)
{
    return guardValue<int32_t>("dca_engine_dsp_add_node_for_player", -1, [&]() -> int32_t {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !module_type_id) return -1;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return -1;
    return e->impl.dspAddNode((int) player, module_type_id);
});
}

DCA_API int32_t DCA_CALL dca_engine_dsp_add_node_bypassed_for_player(dca_engine_t handle, int32_t player,
                                                                     const char* module_type_id)
{
    return guardValue<int32_t>("dca_engine_dsp_add_node_bypassed_for_player", -1, [&]() -> int32_t {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !module_type_id || !dotcharter::AudioEngine::validPlayer((int) player)) return -1;
    return e->impl.dspAddNode((int) player, module_type_id, true);
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_remove_node(dca_engine_t handle, int32_t node_id)
{
    return guard("dca_engine_dsp_remove_node", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    return e->impl.dspRemoveNode(node_id) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_move_node(dca_engine_t handle, int32_t node_id, int32_t new_index)
{
    // GUARDED for the same reason dca_engine_dsp_remove_node three lines above
    // is: this reaches DspChain::publishListLocked, which does
    // std::make_shared<Order>() + reserve(), and moveNode additionally does
    // idOrder_.insert(). Every one of those can throw std::bad_alloc, and an
    // exception unwinding through extern "C" is std::terminate, not an error
    // code.
    return guard("dca_engine_dsp_move_node", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    return e->impl.dspMoveNode(node_id, new_index) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_set_bypass(dca_engine_t handle, int32_t node_id, int32_t bypassed)
{
    // Guarded: publishes a heap-allocated ordering snapshot. See move_node.
    return guard("dca_engine_dsp_set_bypass", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    return e->impl.dspSetBypass(node_id, bypassed != 0) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_set_param(dca_engine_t handle, int32_t node_id,
                                                     const char* param_id, float value)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !param_id) return DCA_ERR_INVALID_ARG;
    // DspChain::setParam rejects NaN of its own accord (every core's clamp is
    // NaN-transparent); this adds the infinities, so the whole ABI answers a
    // non-finite float the same way.
    if (!dcabi::finite(value)) return DCA_ERR_INVALID_ARG;
    return e->impl.dspSetParam(node_id, param_id, value) ? DCA_OK : DCA_ERR_INVALID_ARG;
}

DCA_API dca_status DCA_CALL dca_engine_dsp_get_param(dca_engine_t handle, int32_t node_id,
                                                     const char* param_id, float* out_value)
{
    // Zero the out-param BEFORE anything can fail, the way every other getter
    // in this ABI does (dca_source_get_position_seconds, _get_eq_active_count,
    // _read_eq_spectrum …). A caller that ignores the status is then no worse
    // off than it was, and a caller that checks it sees a defined value either
    // way — the return value is the only thing that changes.
    if (out_value) *out_value = 0.0f;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !param_id || !out_value) return DCA_ERR_INVALID_ARG;
    float v = 0.0f;
    if (!e->impl.dspGetParam(node_id, param_id, v)) return DCA_ERR_INVALID_ARG;
    *out_value = v;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_dsp_set_node_string(dca_engine_t handle, int32_t node_id,
                                                           const char* param_id, const char* utf8_value)
{
    return guard("dca_engine_dsp_set_node_string", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !param_id) return DCA_ERR_INVALID_ARG;
    // NULL value is treated as "" (clear back to the built-in default).
    return e->impl.dspSetStringParam(node_id, param_id, utf8_value ? utf8_value : "")
               ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API int32_t DCA_CALL dca_engine_dsp_get_node_string(dca_engine_t handle, int32_t node_id,
                                                        const char* param_id, char* out_utf8, int32_t capacity)
{
    return guardValue<int32_t>("dca_engine_dsp_get_node_string", -1, [&]() -> int32_t {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !param_id) return -1;
    std::string v;
    if (!e->impl.dspGetStringParam(node_id, param_id, v)) return -1;
    copyStdTruncated(out_utf8, capacity, v);
    return (int32_t) v.size();
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_set_node_branch(dca_engine_t handle, int32_t node_id,
                                                           int32_t branch_id)
{
    // Guarded: publishes a heap-allocated ordering snapshot. See move_node.
    return guard("dca_engine_dsp_set_node_branch", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    return e->impl.dspSetNodeBranch(node_id, branch_id) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

// ---- Node introspection (ABI v33) ------------------------------------------
// The chain has always known both of these; nothing exported them, so the
// managed wrapper had to remember the type id of every node IT created and
// report an empty string for anything else. Message-thread reads under the
// chain's write mutex, like every other node getter here.

DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_type(dca_engine_t handle, int32_t node_id,
                                                         char* out_utf8, int32_t capacity,
                                                         int32_t* out_len)
{
    // Out-param zeroed BEFORE the handle check, so a caller that ignores the
    // status reads "no type" rather than whatever was in its stack.
    if (!out_len) return DCA_ERR_INVALID_ARG;
    *out_len = 0;
    return guard("dca_engine_dsp_get_node_type", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    std::string t;
    // rigOfNode REJECTS a node id naming a seat past DCA_MAX_PLAYERS; it never
    // clamps onto seat 0, which would name a DIFFERENT pedal's type.
    if (!e->impl.dspGetNodeTypeId(node_id, t)) return DCA_ERR_INVALID_ARG;
    copyStdTruncated(out_utf8, capacity, t);
    *out_len = (int32_t) t.size();
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_get_bypass(dca_engine_t handle, int32_t node_id,
                                                      int32_t* out_bypassed)
{
    if (!out_bypassed) return DCA_ERR_INVALID_ARG;
    *out_bypassed = 0;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    bool b = false;
    if (!e->impl.dspGetBypass(node_id, b)) return DCA_ERR_INVALID_ARG;
    *out_bypassed = b ? 1 : 0;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_branch(dca_engine_t handle, int32_t node_id,
                                                           int32_t* out_branch_id)
{
    // Zeroed BEFORE anything can fail, the way every v33/v34 getter beside it
    // does: a caller that ignores the status reads a defined value either way.
    if (out_branch_id) *out_branch_id = 0;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_branch_id) return DCA_ERR_INVALID_ARG;
    int b = 0;
    if (!e->impl.dspGetNodeBranch(node_id, b)) return DCA_ERR_INVALID_ARG;
    *out_branch_id = b;
    return DCA_OK;
}

DCA_API int32_t DCA_CALL dca_engine_dsp_node_count(dca_engine_t handle)
{
    return dca_engine_dsp_node_count_for_player(handle, 0);
}

// THE FAILURE SENTINEL OF EVERY VALUE-RETURNING EXPORT IS -1 (W4-N8).
//
// This family answered 0 for "you passed a dead engine / a seat that does not
// exist" AND for the perfectly normal "this seat's rig is empty", while
// dca_engine_dsp_get_chain_latency_for_player two hundred lines down used -1 for
// exactly the same fault, as do dsp_add_node, dsp_get_node_string,
// plugin_get_input_player and the module-label pair. So the ABI carried two
// conventions for one concept with no rule saying which applied where, and a
// managed caller polling a rig could not tell a disposed handle from an empty
// board — which is how a lifetime bug reads as "the pedals disappeared".
//
// -1 everywhere; 0 stays a legal ANSWER everywhere. Source-compatible for any
// caller that only ever passed live handles, and for any caller that tested
// `<= 0`. The wrapper's three call sites do exactly that
// (JuceDspChain.NodeIds' `count <= 0`, and Math.Max(0, ...) at the other two),
// which is why this needs no ABI version bump.
//
// dca_engine_dsp_get_metering is DELIBERATELY NOT in this family and keeps 0:
// it is a PREDICATE, the wrapper reads it as `!= 0`, and -1 would make a dead
// handle answer "metering is on".
DCA_API int32_t DCA_CALL dca_engine_dsp_node_count_for_player(dca_engine_t handle, int32_t player)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return -1;
    return e->impl.dspNodeCount((int) player);
}

DCA_API int32_t DCA_CALL dca_engine_dsp_get_node_ids(dca_engine_t handle, int32_t* out_ids, int32_t capacity)
{
    return dca_engine_dsp_get_node_ids_for_player(handle, 0, out_ids, capacity);
}

DCA_API int32_t DCA_CALL dca_engine_dsp_get_node_ids_for_player(dca_engine_t handle, int32_t player,
                                                                 int32_t* out_ids, int32_t capacity)
{
    // -1 for a bad handle or a bad seat; see dca_engine_dsp_node_count_for_player.
    // A NULL out_ids with capacity 0 is the SIZING call, not a failure.
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return -1;
    if (out_ids == nullptr || capacity <= 0) return e->impl.dspNodeCount((int) player);
    return e->impl.dspGetNodeIds((int) player, out_ids, capacity);
}

DCA_API dca_status DCA_CALL dca_engine_dsp_clear(dca_engine_t handle)
{
    return guard("dca_engine_dsp_clear", [&]() -> dca_status {
    return dca_engine_dsp_clear_for_player(handle, 0);
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_clear_for_player(dca_engine_t handle, int32_t player)
{
    return guard("dca_engine_dsp_clear_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    e->impl.dspClear((int) player);
    return DCA_OK;
});
}

// ---- Edit scopes (ABI v34) -------------------------------------------------
// Suppress publication for the length of a preset change; see the header.
//
// A NESTED begin is NOT rejected: the chain aborts the stale scope, logs a
// Warning and always returns true, because refusing left the dead scope open and
// an open scope publishes nothing. The failures that DO arrive here as `false`
// are a commit or abort with nothing open, and an abort racing a commit that is
// already in flight on another thread.

DCA_API dca_status DCA_CALL dca_engine_dsp_begin_edit_for_player(dca_engine_t handle, int32_t player)
{
    return guard("dca_engine_dsp_begin_edit_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.dspBeginEdit((int) player) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_commit_edit_for_player(dca_engine_t handle, int32_t player)
{
    return guard("dca_engine_dsp_commit_edit_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.dspCommitEdit((int) player) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_abort_edit_for_player(dca_engine_t handle, int32_t player)
{
    return guard("dca_engine_dsp_abort_edit_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    return e->impl.dspAbortEdit((int) player) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_get_editing_for_player(dca_engine_t handle, int32_t player,
                                                                  int32_t* out_editing)
{
    // Out-param zeroed before anything can fail, like every other getter here.
    if (out_editing) *out_editing = 0;
    return guard("dca_engine_dsp_get_editing_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_editing || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    *out_editing = e->impl.dspEditing((int) player) ? 1 : 0;
    return DCA_OK;
});
}

// SEAT-0 ALIASES (ABI v35). `native-engine-abi.md` states as a rule that every
// pre-v28 export is an alias for seat 0, and the v34 edit-scope, branch-PDC and
// automated-render exports broke that shape — most visibly the last one, whose
// NON-automated sibling dca_engine_dsp_process_buffer does have its alias. Each
// of these is one line and purely ADDITIVE.

DCA_API dca_status DCA_CALL dca_engine_dsp_begin_edit(dca_engine_t handle)
{ return dca_engine_dsp_begin_edit_for_player(handle, 0); }

DCA_API dca_status DCA_CALL dca_engine_dsp_commit_edit(dca_engine_t handle)
{ return dca_engine_dsp_commit_edit_for_player(handle, 0); }

DCA_API dca_status DCA_CALL dca_engine_dsp_abort_edit(dca_engine_t handle)
{ return dca_engine_dsp_abort_edit_for_player(handle, 0); }

DCA_API dca_status DCA_CALL dca_engine_dsp_get_editing(dca_engine_t handle, int32_t* out_editing)
{ return dca_engine_dsp_get_editing_for_player(handle, 0, out_editing); }

// ---- Node sidechain (ABI v34) ----------------------------------------------

DCA_API dca_status DCA_CALL dca_engine_dsp_set_node_sidechain(dca_engine_t handle, int32_t node_id,
                                                              int32_t source)
{
    return guard("dca_engine_dsp_set_node_sidechain", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    // The chain rejects an out-of-range source and a module that does not
    // declare the capability; both arrive here as false.
    return e->impl.dspSetNodeSidechain(node_id, (int) source) ? DCA_OK : DCA_ERR_INVALID_ARG;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_sidechain(dca_engine_t handle, int32_t node_id,
                                                              int32_t* out_source)
{
    if (out_source) *out_source = 0;
    return guard("dca_engine_dsp_get_node_sidechain", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_source) return DCA_ERR_INVALID_ARG;
    int v = 0;
    if (!e->impl.dspGetNodeSidechain(node_id, v)) return DCA_ERR_INVALID_ARG;
    *out_source = (int32_t) v;
    return DCA_OK;
});
}

// ---- Branch delay compensation (ABI v34) -----------------------------------

DCA_API dca_status DCA_CALL dca_engine_dsp_set_branch_pdc_for_player(dca_engine_t handle, int32_t player,
                                                                     int32_t enabled)
{
    return guard("dca_engine_dsp_set_branch_pdc_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    e->impl.dspSetBranchPdc((int) player, enabled != 0);
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_get_branch_pdc_for_player(dca_engine_t handle, int32_t player,
                                                                     int32_t* out_enabled)
{
    if (out_enabled) *out_enabled = 0;
    return guard("dca_engine_dsp_get_branch_pdc_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_enabled || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    *out_enabled = e->impl.dspBranchPdc((int) player) ? 1 : 0;
    return DCA_OK;
});
}

// Seat-0 aliases (ABI v35); see the block above the edit-scope aliases.

DCA_API dca_status DCA_CALL dca_engine_dsp_set_branch_pdc(dca_engine_t handle, int32_t enabled)
{ return dca_engine_dsp_set_branch_pdc_for_player(handle, 0, enabled); }

DCA_API dca_status DCA_CALL dca_engine_dsp_get_branch_pdc(dca_engine_t handle, int32_t* out_enabled)
{ return dca_engine_dsp_get_branch_pdc_for_player(handle, 0, out_enabled); }

// ---- Spillover (ABI v37) ---------------------------------------------------

DCA_API dca_status DCA_CALL dca_engine_dsp_set_spillover_for_player(dca_engine_t handle, int32_t player,
                                                                    int32_t enabled)
{
    return guard("dca_engine_dsp_set_spillover_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    e->impl.dspSetSpillover((int) player, enabled != 0);
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_get_spillover_for_player(dca_engine_t handle, int32_t player,
                                                                    int32_t* out_enabled)
{
    if (out_enabled) *out_enabled = 0;
    return guard("dca_engine_dsp_get_spillover_for_player", [&]() -> dca_status {
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_enabled || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    *out_enabled = e->impl.dspSpillover((int) player) ? 1 : 0;
    return DCA_OK;
});
}

DCA_API int32_t DCA_CALL dca_engine_dsp_get_spill_tail_count_for_player(dca_engine_t handle, int32_t player)
{
    // Value-returning: -1 is the failure sentinel, 0 a legal answer. The count
    // RETIRES finished tails first, which publishes (and so can allocate), hence
    // the guard — a throw here must be -1, never an unwind into the caller.
    return guardValue<int32_t>("dca_engine_dsp_get_spill_tail_count_for_player", -1, [&]() -> int32_t {
        auto* e = reinterpret_cast<Engine*>(handle);
        if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return -1;
        return (int32_t) e->impl.dspSpillTailCount((int) player);
    });
}

DCA_API dca_status DCA_CALL dca_engine_dsp_process_buffer_automated(
    dca_engine_t handle,
    float* interleaved, int32_t channels, int32_t frames, int32_t sample_rate,
    const dca_dsp_automation_event* events, int32_t event_count,
    int32_t* out_bad_event_index)
{
    return dca_engine_dsp_process_buffer_automated_for_player(
        handle, 0, interleaved, channels, frames, sample_rate,
        events, event_count, out_bad_event_index);
}

// ---- Per-node meters + latency (ABI v30) -----------------------------------
// Poll-only, allocation-free on the audio thread: the callback folds each block
// into a decaying envelope and these just read it. See dotcharter_audio.h.

DCA_API dca_status DCA_CALL dca_engine_dsp_set_metering(dca_engine_t handle, int32_t enabled)
{
    return dca_engine_dsp_set_metering_for_player(handle, 0, enabled);
}

DCA_API dca_status DCA_CALL dca_engine_dsp_set_metering_for_player(dca_engine_t handle, int32_t player,
                                                                    int32_t enabled)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    e->impl.dspSetMetering((int) player, enabled != 0);
    return DCA_OK;
}

DCA_API int32_t DCA_CALL dca_engine_dsp_get_metering(dca_engine_t handle, int32_t player)
{
    // 0, NOT the -1 its neighbours use, and that is deliberate (W4-N8): this is
    // a PREDICATE with exactly two legal answers, every caller reads it as
    // `!= 0`, and answering -1 for a dead handle would say "metering is ON".
    // "Could not ask" and "off" are the same actionable answer here.
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return 0;
    return e->impl.dspMetering((int) player) ? 1 : 0;
}

DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_meter(dca_engine_t handle, int32_t node_id,
                                                           float* out_in_peak, float* out_in_rms,
                                                           float* out_out_peak, float* out_out_rms)
{
    // Zeroed BEFORE anything can fail. See dca_engine_dsp_get_param.
    if (out_in_peak)  *out_in_peak  = 0.0f;
    if (out_in_rms)   *out_in_rms   = 0.0f;
    if (out_out_peak) *out_out_peak = 0.0f;
    if (out_out_rms)  *out_out_rms  = 0.0f;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    float ip = 0.0f, ir = 0.0f, op = 0.0f, orms = 0.0f;
    if (!e->impl.dspGetNodeMeter(node_id, ip, ir, op, orms)) return DCA_ERR_INVALID_ARG;
    if (out_in_peak)  *out_in_peak  = ip;
    if (out_in_rms)   *out_in_rms   = ir;
    if (out_out_peak) *out_out_peak = op;
    if (out_out_rms)  *out_out_rms  = orms;
    return DCA_OK;
}

DCA_API int32_t DCA_CALL dca_engine_dsp_read_node_meters(dca_engine_t handle, int32_t player,
                                                          float* out_values, int32_t capacity)
{
    // -1 for a bad handle or a bad seat; see dca_engine_dsp_node_count_for_player.
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return -1;
    if (out_values == nullptr || capacity <= 0) return e->impl.dspNodeCount((int) player);
    return e->impl.dspReadMeters((int) player, out_values, capacity);
}

DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_latency(dca_engine_t handle, int32_t node_id,
                                                             int32_t* out_samples)
{
    // Zeroed BEFORE anything can fail. See dca_engine_dsp_get_param.
    if (out_samples) *out_samples = 0;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !out_samples) return DCA_ERR_INVALID_ARG;
    int lat = 0;
    if (!e->impl.dspGetNodeLatency(node_id, lat)) return DCA_ERR_INVALID_ARG;
    *out_samples = (int32_t) lat;
    return DCA_OK;
}

DCA_API int32_t DCA_CALL dca_engine_dsp_get_chain_latency(dca_engine_t handle)
{
    return dca_engine_dsp_get_chain_latency_for_player(handle, 0);
}

DCA_API int32_t DCA_CALL dca_engine_dsp_get_chain_latency_for_player(dca_engine_t handle, int32_t player)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return -1;
    return (int32_t) e->impl.dspChainLatency((int) player);
}

// ---- Offline test / tooling utilities (see header) -------------------------

DCA_API dca_status DCA_CALL dca_engine_dsp_process_buffer(dca_engine_t handle,
                                                          float* left, float* right,
                                                          int32_t num_frames, int32_t sample_rate)
{
    return guard("dca_engine_dsp_process_buffer", [&]() -> dca_status {
    return dca_engine_dsp_process_buffer_for_player(handle, 0, left, right, num_frames, sample_rate);
});
}

DCA_API dca_status DCA_CALL dca_engine_dsp_process_buffer_for_player(dca_engine_t handle, int32_t player,
                                                                      float* left, float* right,
                                                                      int32_t num_frames, int32_t sample_rate)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !left || !right || num_frames <= 0) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    // A live sample rate is a plausible one (droptune's own bound), and a rate
    // this far off would have re-derived every coefficient in the rig wrong.
    if (sample_rate < 8000 || sample_rate > 192000) return DCA_ERR_INVALID_ARG;
    // OFFLINE MEANS OFFLINE. dspProcessOffline takes the chain's processMutex_
    // once per 512-frame chunk, and that mutex is the audio callback's own
    // wait-one-block barrier -- so a caller that renders a three-minute take
    // while a device is running contends the callback ~16,000 times for the
    // length of a whole rig pass each. That is not a subtle stall; it is the
    // callback waiting on a WORKER thread's DSP.
    //
    // Every real caller is already device-less on purpose and says so in a
    // comment: DotCharter's TakeReampService drives a dedicated engine and
    // calls Stop() first ("never a device -- the offline path must be the only
    // processor"), and all four DSP test classes in this repo plus
    // DotCharter's DspOfflineProcessingTests do the same. So this rejects
    // rather than degrading, and no caller had to change.
    //
    // The GUARD IS ON THE EXPORT, not on AudioEngine::dspProcessOffline: the
    // native RT gate drives that method directly with the MANUAL-PUMP null
    // device open, where the callback only runs when the test thread pumps it,
    // and RtEveryCatalogueModuleIsAllocationFree (100 modules) would go silently
    // vacuous if the method itself refused. An in-process caller that holds the
    // callback still can keep doing this; a caller across the ABI cannot.
    //
    // THE SNAPSHOT, not the device pointer (W4-D73). deviceSnapshot().open is
    // published on the device thread by audioDeviceAboutToStart / stopped, so
    // reading it here costs one relaxed atomic load and cannot touch an object
    // setAudioDeviceSetup is deleting — the rule every other reader in this file
    // follows.
    //
    // ...AND THE CHECK IS NO LONGER A TOCTOU (W4-D73, closed 2026-09-11). The
    // lifecycle lock is taken FIRST and held for the whole render, and
    // dca_engine_start / _reconfigure / _set_device_type take the same lock, so
    // the snapshot read below cannot be invalidated behind us. Both sides
    // TRY-lock: whichever arrives second is refused rather than made to wait,
    // because blocking a user's play button behind a multi-minute bounce is a
    // worse answer than an error code. Order matters — lock, THEN read.
    dotcharter::AudioEngine::DeviceLifecycleLock lifecycle(e->impl);
    if (! lifecycle.held()) return DCA_ERR_DEVICE;   // a device op is in flight
    if (e->impl.deviceSnapshot().open) return DCA_ERR_DEVICE;
    return guard("dca_engine_dsp_process_buffer_for_player", [&]() -> dca_status {
        e->impl.dspProcessOffline((int) player, left, right, num_frames, sample_rate);
        return DCA_OK;
    });
}

// ---- Offline render with automation (v34 (engine)) -----------------------
//
// Pinned like the other two by-value structs on this ABI. A managed layout test
// mirrors these exact numbers; an accidental reorder or a padding change here
// would marshal garbage with no diagnostic at all.
static_assert(sizeof(dca_dsp_automation_event) == 48,
              "dca_dsp_automation_event must stay 48 bytes");
static_assert(offsetof(dca_dsp_automation_event, frame)   == 0,  "frame offset");
static_assert(offsetof(dca_dsp_automation_event, node_id) == 8,  "node_id offset");
static_assert(offsetof(dca_dsp_automation_event, param)   == 12, "param offset");
static_assert(offsetof(dca_dsp_automation_event, value)   == 44, "value offset");

/// Upper bound on `event_count`. See the rejection in the export below.
static constexpr int32_t kMaxAutomationEvents = 262144;

DCA_API dca_status DCA_CALL dca_engine_dsp_process_buffer_automated_for_player(
    dca_engine_t handle, int32_t player,
    float* interleaved, int32_t channels, int32_t frames, int32_t sample_rate,
    const dca_dsp_automation_event* events, int32_t event_count,
    int32_t* out_bad_event_index)
{
    if (out_bad_event_index) *out_bad_event_index = -1;

    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e || !interleaved || frames <= 0) return DCA_ERR_INVALID_ARG;
    if (channels != 1 && channels != 2) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    if (sample_rate < 8000 || sample_rate > 192000) return DCA_ERR_INVALID_ARG;
    if (event_count < 0) return DCA_ERR_INVALID_ARG;
    if (event_count > 0 && !events) return DCA_ERR_INVALID_ARG;
    // CAP THE LIST. Events at consecutive frames produce a chain of one-sample
    // rig.process calls, each taking the chain's process barrier and each paying
    // the whole per-block overhead (the scrub, the branch-PDC read, the meter
    // passes). A caller that resamples an automation curve per sample — the
    // obvious naive way to write a wah sweep — turned a 5-minute render into
    // 14 M single-sample process calls with no cap and no diagnostic: it simply
    // appeared to hang. 262,144 is ~90 minutes of one event every 10 ms.
    if (event_count > kMaxAutomationEvents) return DCA_ERR_INVALID_ARG;

    // OFFLINE MEANS OFFLINE -- see dca_engine_dsp_process_buffer_for_player for
    // the full reasoning. The chunk loop takes the chain's process barrier, and
    // this variant takes it MORE often (once per event boundary as well as once
    // per 512 frames), so the argument is if anything stronger here.
    // ...and the same lifecycle lock + snapshot read, for the same reason
    // (W4-D73). Lock FIRST, then read.
    dotcharter::AudioEngine::DeviceLifecycleLock lifecycle(e->impl);
    if (! lifecycle.held()) return DCA_ERR_DEVICE;
    if (e->impl.deviceSnapshot().open) return DCA_ERR_DEVICE;

    // VALIDATE THE WHOLE EVENT LIST FIRST, before a single sample is rendered.
    // Rejecting an event half way through would leave the caller holding a
    // partially processed buffer that it has no way to tell apart from a good
    // one -- and the failure it most wants to catch, a NaN, is exactly the one
    // that would already have been written into the coefficients by then.
    //
    // ...and since ABI v35 that pass also RESOLVES every event: the seat the
    // node id names, the node itself and its parameter id. Until then this loop
    // checked only frame / finiteness / sortedness / non-empty id, so a bad node
    // or a misspelt param id was found MID-RENDER -- every event before it had
    // already gone through DspChain::setParam, leaving the seat's chain at an
    // arbitrary mid-sweep state (a wah parked at 0.37, a pedal bypassed) that the
    // caller could neither see nor undo. The buffer guarantee below is what the
    // comment above reasoned about; the RIG had no guarantee at all.
    //
    // canApplyAutomationEvent is a resolve-only probe through the SAME rigOfNode
    // + module lookup the setter uses -- not the second source of truth this
    // comment used to reject it as.
    for (int32_t i = 0; i < event_count; ++i)
    {
        const auto& ev = events[i];
        if (ev.frame < 0 || ev.frame >= (int64_t) frames
            || !dcabi::finite(ev.value)
            || ev.param[0] == '\0'
            || (i > 0 && ev.frame < events[i - 1].frame)    // sorted, not scrambled
            || !e->impl.canApplyAutomationEvent((int) player, ev))
        {
            if (out_bad_event_index) *out_bad_event_index = i;
            return DCA_ERR_INVALID_ARG;
        }
    }

    return guard("dca_engine_dsp_process_buffer_automated_for_player", [&]() -> dca_status {
        const int bad = e->impl.dspProcessOfflineAutomated(
            (int) player, interleaved, (int) channels, (int) frames, (int) sample_rate,
            events, (int) event_count);
        if (bad >= 0)
        {
            // Defensive: the pre-pass above resolved every event, so reaching
            // here means the rig changed under us between the two. Reported by
            // INDEX like every other rejection.
            if (out_bad_event_index) *out_bad_event_index = bad;
            return DCA_ERR_INVALID_ARG;
        }
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_droptune_process_offline(
    int32_t sample_rate, float max_lag_ms,
    const float* semitones_per_segment, const int32_t* segment_lengths, int32_t num_segments,
    const float* input, float* output, int32_t total_samples)
{
    if (sample_rate < 8000 || sample_rate > 192000) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(max_lag_ms)) return DCA_ERR_INVALID_ARG;
    if (!semitones_per_segment || !segment_lengths || num_segments <= 0) return DCA_ERR_INVALID_ARG;
    if (!input || !output || total_samples <= 0) return DCA_ERR_INVALID_ARG;

    long long sum = 0;
    for (int32_t i = 0; i < num_segments; ++i)
    {
        if (segment_lengths[i] < 0) return DCA_ERR_INVALID_ARG;
        if (!dcabi::finite(semitones_per_segment[i])) return DCA_ERR_INVALID_ARG;
        sum += segment_lengths[i];
    }
    if (sum != total_samples) return DCA_ERR_INVALID_ARG;

    return guard("dca_droptune_process_offline", [&]() -> dca_status {
        // The device callback establishes FTZ/DAZ for the live path; this one
        // has none, and CSVD is a lag-domain shifter whose ring, its LPC-ish
        // detector state and the crossfade tails all decay toward subnormals
        // between segments. Subnormal arithmetic costs ~100x per sample on x86
        // without hardware FTZ, for the rest of the render. Same reasoning, and
        // the same one line, as AudioEngine::dspProcessOffline.
        const juce::ScopedNoDenormals noDenormals;

        dcdsp::DropTune dt;
        dt.prepare((double) sample_rate, 512, 1);

        if (output != input)
            std::memcpy(output, input, sizeof(float) * (size_t) total_samples);

        // Process in place, per segment, in bounded chunks — the same
        // block-at-a-time cadence the live callback drives, so engage /
        // disengage transitions land mid-stream against a primed ring.
        int32_t pos = 0;
        for (int32_t seg = 0; seg < num_segments; ++seg)
        {
            dcdsp::DropTuneParams p;
            p.semitones = semitones_per_segment[seg];
            p.maxLagMs  = max_lag_ms;
            dt.setParams(p);

            int32_t remaining = segment_lengths[seg];
            while (remaining > 0)
            {
                const int32_t n = remaining < 512 ? remaining : 512;
                float* chans[1] = { output + pos };
                dt.process(chans, 1, n);
                pos       += n;
                remaining -= n;
            }
        }
        return DCA_OK;
    });
}

DCA_API int32_t DCA_CALL dca_dsp_module_type_count(void)
{
    return (int32_t) dcrig::moduleCatalogue().size();
}

DCA_API dca_status DCA_CALL dca_dsp_module_type_info(int32_t index, char* out_id, int32_t id_cap,
                                                     char* out_name, int32_t name_cap)
{
    const auto& cat = dcrig::moduleCatalogue();
    if (index < 0 || index >= (int32_t) cat.size()) return DCA_ERR_INVALID_ARG;
    const auto& m = cat[(size_t) index];
    if (out_id) copyStdTruncated(out_id, id_cap, m.id);
    if (out_name) copyStdTruncated(out_name, name_cap, m.displayName);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_dsp_module_type_flags(int32_t type_index, int32_t* out_flags)
{
    // ABI v34. dca_dsp_module_type_info could not grow a field without changing
    // its signature, so capabilities live here — one export, one bit per
    // capability, room for the next one.
    if (!out_flags) return DCA_ERR_INVALID_ARG;
    *out_flags = 0;
    const auto& cat = dcrig::moduleCatalogue();
    if (type_index < 0 || type_index >= (int32_t) cat.size()) return DCA_ERR_INVALID_ARG;
    *out_flags = (int32_t) dcrig::moduleTypeFlags((int) type_index);
    return DCA_OK;
}

// ABI v33. The catalogue's own category string — a short fixed set the ENGINE
// owns. It is exported rather than mirrored managed-side on purpose: a managed
// guess table would be a second source of truth for something only the factory
// knows, and it would drift the first time a module was added.
DCA_API dca_status DCA_CALL dca_dsp_module_type_category(int32_t type_index,
                                                         char* out_utf8, int32_t capacity,
                                                         int32_t* out_len)
{
    if (!out_len) return DCA_ERR_INVALID_ARG;
    *out_len = 0;
    const auto& cat = dcrig::moduleCatalogue();
    if (type_index < 0 || type_index >= (int32_t) cat.size()) return DCA_ERR_INVALID_ARG;
    const std::string& c = cat[(size_t) type_index].category;
    copyStdTruncated(out_utf8, capacity, c);
    *out_len = (int32_t) c.size();
    return DCA_OK;
}

namespace {
const dcrig::ModuleTypeInfo* findModuleType(const char* typeId)
{
    if (!typeId) return nullptr;
    for (const auto& m : dcrig::moduleCatalogue())
        if (m.id == typeId) return &m;
    return nullptr;
}
} // namespace

DCA_API int32_t DCA_CALL dca_dsp_module_param_count(const char* module_type_id)
{
    const auto* m = findModuleType(module_type_id);
    return m ? (int32_t) m->params.size() : -1;
}

DCA_API dca_status DCA_CALL dca_dsp_module_param_info(const char* module_type_id, int32_t index,
                                                      char* out_id, int32_t id_cap,
                                                      char* out_name, int32_t name_cap,
                                                      float* out_min, float* out_max, float* out_default)
{
    const auto* m = findModuleType(module_type_id);
    if (!m || index < 0 || index >= (int32_t) m->params.size()) return DCA_ERR_INVALID_ARG;
    const auto& p = m->params[(size_t) index];
    if (out_id) copyStdTruncated(out_id, id_cap, p.id);
    if (out_name) copyStdTruncated(out_name, name_cap, p.displayName);
    if (out_min) *out_min = p.minValue;
    if (out_max) *out_max = p.maxValue;
    if (out_default) *out_default = p.defaultValue;
    return DCA_OK;
}

DCA_API int32_t DCA_CALL dca_dsp_module_string_param_count(const char* module_type_id)
{
    const auto* m = findModuleType(module_type_id);
    return m ? (int32_t) m->stringParams.size() : -1;
}

DCA_API dca_status DCA_CALL dca_dsp_module_string_param_info(const char* module_type_id, int32_t index,
                                                             char* out_id, int32_t id_cap,
                                                             char* out_name, int32_t name_cap,
                                                             char* out_file_filter, int32_t filter_cap)
{
    const auto* m = findModuleType(module_type_id);
    if (!m || index < 0 || index >= (int32_t) m->stringParams.size()) return DCA_ERR_INVALID_ARG;
    const auto& p = m->stringParams[(size_t) index];
    if (out_id) copyStdTruncated(out_id, id_cap, p.id);
    if (out_name) copyStdTruncated(out_name, name_cap, p.displayName);
    if (out_file_filter) copyStdTruncated(out_file_filter, filter_cap, p.fileFilter);
    return DCA_OK;
}

namespace {
// Find one value param by id inside a module type; nullptr when unknown.
const dcrig::ParamInfo* findParam(const char* moduleTypeId, const char* paramId)
{
    if (!paramId) return nullptr;
    const auto* m = findModuleType(moduleTypeId);
    if (!m) return nullptr;
    for (const auto& p : m->params)
        if (p.id == paramId) return &p;
    return nullptr;
}
} // namespace

DCA_API int32_t DCA_CALL dca_dsp_module_param_label_count(const char* module_type_id,
                                                          const char* param_id)
{
    const auto* p = findParam(module_type_id, param_id);
    if (!p || p->valueLabels.empty()) return -1;   // continuous / unknown
    return (int32_t) p->valueLabels.size();
}

DCA_API int32_t DCA_CALL dca_dsp_module_param_label(const char* module_type_id,
                                                    const char* param_id,
                                                    int32_t value_index,
                                                    char* out_utf8, int32_t capacity)
{
    const auto* p = findParam(module_type_id, param_id);
    if (!p || p->valueLabels.empty()) return -1;
    if (value_index < 0 || value_index >= (int32_t) p->valueLabels.size()) return -1;
    const std::string& label = p->valueLabels[(size_t) value_index];
    if (out_utf8 != nullptr && capacity > 0) copyStdTruncated(out_utf8, capacity, label);
    return (int32_t) label.size();
}

DCA_API float DCA_CALL dca_dsp_module_param_step(const char* module_type_id,
                                                 const char* param_id)
{
    const auto* p = findParam(module_type_id, param_id);
    if (!p) return -1.0f;              // unknown module / param
    return p->step > 0.0f ? p->step : 0.0f;  // 0 = continuous
}

// ABI v45: the rsamp chain table, built by the host from the user's own
// Rocksmith install (src/dsp/cores/dcdsp_rsamp_table.h). Process-wide.
DCA_API dca_status DCA_CALL dca_rsamp_set_table(const void* data, int32_t length)
{
    return guard("dca_rsamp_set_table", [&]() -> dca_status {
        if (length < 0 || (data == nullptr) != (length == 0)) return DCA_ERR_INVALID_ARG;
        auto& registry = dcdsp::rsamp::TableRegistry::instance();
        if (data == nullptr)
        {
            registry.clear();
            emitLog(DCA_LOG_INFO, "rsamp: chain table unloaded; rsamp modules pass audio through");
            return DCA_OK;
        }
        const char* error = nullptr;
        if (!registry.load(static_cast<const uint8_t*>(data), (size_t) length, &error))
        {
            emitLog(DCA_LOG_WARN, error != nullptr ? error : "rsamp: chain table refused");
            return DCA_ERR_INVALID_ARG;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "rsamp: chain table loaded (%d rows)", dcdsp::rsamp::TableRegistry::rowCount());
        emitLog(DCA_LOG_INFO, buf);
        return DCA_OK;
    });
}

DCA_API int32_t DCA_CALL dca_rsamp_table_row_count(void)
{
    return (int32_t) dcdsp::rsamp::TableRegistry::rowCount();
}

DCA_API dca_status DCA_CALL dca_generate_waveform(const char* path,
                                                  double      block_ms,
                                                  float*      out_levels,
                                                  int32_t     capacity,
                                                  int32_t*    out_count)
{
    if (!path || !dcabi::finite(block_ms) || block_ms <= 0.0 || !out_count) return DCA_ERR_INVALID_ARG;
    *out_count = 0;
    juce::File file(juce::String::fromUTF8(path));
    if (!file.existsAsFile()) return DCA_ERR_DECODE;
    try
    {
        const int total = dotcharter::AudioEngine::generateWaveform(file, block_ms, out_levels, capacity);
        *out_count = total;
        return total > 0 ? DCA_OK : DCA_ERR_DECODE;
    }
    catch (const std::exception& ex)
    {
        emitLog(DCA_LOG_ERROR, ex.what());
        return DCA_ERR_INTERNAL;
    }
    catch (...) { return DCA_ERR_INTERNAL; }
}

DCA_API dca_status DCA_CALL dca_generate_waveform_from_memory(const void* data,
                                                              int64_t     data_size,
                                                              double      block_ms,
                                                              float*      out_levels,
                                                              int32_t     capacity,
                                                              int32_t*    out_count)
{
    if (!data || data_size <= 0 || !dcabi::finite(block_ms) || block_ms <= 0.0 || !out_count) return DCA_ERR_INVALID_ARG;
    *out_count = 0;
    try
    {
        const int total = dotcharter::AudioEngine::generateWaveformFromMemory(
            data, (size_t) data_size, block_ms, out_levels, capacity);
        *out_count = total;
        return total > 0 ? DCA_OK : DCA_ERR_DECODE;
    }
    catch (const std::exception& ex)
    {
        emitLog(DCA_LOG_ERROR, ex.what());
        return DCA_ERR_INTERNAL;
    }
    catch (...) { return DCA_ERR_INTERNAL; }
}

DCA_API dca_status DCA_CALL dca_decode_audio_file(const char* path,
                                                  float**     out_samples,
                                                  int32_t*    out_sample_rate,
                                                  int32_t*    out_channels,
                                                  int64_t*    out_frames)
{
    if (!path || !out_sample_rate || !out_channels || !out_frames) return DCA_ERR_INVALID_ARG;
    *out_sample_rate = 0;
    *out_channels    = 0;
    *out_frames      = 0;
    if (out_samples) *out_samples = nullptr;

    juce::File file(juce::String::fromUTF8(path));
    if (!file.existsAsFile()) return DCA_ERR_DECODE;
    try
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        fm.registerFormat(new dcwem::WemAudioFormat(), false);   // v42
        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
        if (!reader) return DCA_ERR_DECODE;

        const auto frames   = (juce::int64) reader->lengthInSamples;
        const auto channels = (int)         reader->numChannels;
        const auto rate     = (int)         reader->sampleRate;
        if (frames <= 0 || channels <= 0 || rate <= 0) return DCA_ERR_DECODE;

        *out_sample_rate = rate;
        *out_channels    = channels;
        *out_frames      = (int64_t) frames;

        if (!out_samples) return DCA_OK;  // sizing call only

        // Reject inputs that overflow either juce::AudioBuffer's int sample
        // count (~2^31, ~13.5 h @ 44.1 kHz) or the malloc size in floats.
        // Without these checks D-03 truncates frames silently and D-04
        // overflows the buffer allocation on 32-bit builds, both of which
        // lead to large out-of-bounds writes when the read loop runs.
        if (frames > (juce::int64) std::numeric_limits<int>::max())
        {
            emitLog(DCA_LOG_ERROR, "decode: file too long for AudioBuffer<int>");
            return DCA_ERR_DECODE;
        }
        const size_t maxFloats = std::numeric_limits<size_t>::max() / sizeof(float);
        if ((size_t) channels == 0 || (size_t) frames > maxFloats / (size_t) channels)
        {
            emitLog(DCA_LOG_ERROR, "decode: frames * channels would overflow size_t");
            return DCA_ERR_INTERNAL;
        }

        juce::AudioBuffer<float> buf(channels, (int) frames);
        if (!reader->read(&buf, 0, (int) frames, 0, true, channels > 1))
            return DCA_ERR_DECODE;
        // ABI v43: a device-rate WAV's samples are worth v x its stored step.
        auto stored = dotcharter::devwav::probeFile(file);
        dotcharter::devwav::checkAgainstReader(stored, *reader);
        if (stored.decodeGain() != 1.0) buf.applyGain((float) stored.decodeGain());

        const auto totalFloats = (size_t) frames * (size_t) channels;
        float* out = (float*) std::malloc(totalFloats * sizeof(float));
        if (!out) return DCA_ERR_INTERNAL;

        // Interleave: out[frame*channels + ch] = buf.getReadPointer(ch)[frame]
        for (int ch = 0; ch < channels; ++ch)
        {
            const float* src = buf.getReadPointer(ch);
            for (juce::int64 f = 0; f < frames; ++f)
                out[(size_t) f * (size_t) channels + (size_t) ch] = src[f];
        }
        *out_samples = out;
        return DCA_OK;
    }
    catch (const std::exception& ex)
    {
        emitLog(DCA_LOG_ERROR, ex.what());
        return DCA_ERR_INTERNAL;
    }
    catch (...) { return DCA_ERR_INTERNAL; }
}

DCA_API void DCA_CALL dca_decode_free(float* samples)
{
    if (samples) std::free(samples);
}

// ---- Wwise Vorbis (.wem) decoding (ABI v42, DotCharter #426) -----------------
// The decoder itself is src/wem/WemVorbisDecoder.cpp; this is marshalling only.
// dca_wem is a plain heap object: no engine, no device, no callback.
struct dca_wem
{
    std::unique_ptr<dcwem::WemVorbisDecoder> decoder;
    std::vector<float> planar;            // read scratch, channels * capacity
    bool reportedEnd = false;             // the end-of-decode debug line was logged
};

namespace
{
bool validWemVariant(int32_t v) noexcept
{
    return v >= DCA_WEM_VARIANT_AUTO && v <= DCA_WEM_VARIANT_HEADER_TRIAD;
}

dca_status openWem(std::shared_ptr<const std::vector<uint8_t>> bytes, int32_t variant, dca_wem_t* out)
{
    std::string error;
    auto decoder = dcwem::WemVorbisDecoder::open(std::move(bytes), (dcwem::Variant) variant, error);
    if (decoder == nullptr)
    {
        const std::string msg = "wem_open: " + error;
        emitLog(DCA_LOG_ERROR, msg.c_str());
        return DCA_ERR_DECODE;
    }
    // #434: a dca_wem is read front to back (the WAV cache, a whole-file
    // decode), so it decodes on worker threads, bit-identical to the serial
    // decode. DCA_WEM_DECODE_THREADS (1 = serial) and DCA_WEM_LANE_PACKETS
    // override the defaults for tests and measurements; read with
    // GetEnvironmentVariable (juce::SystemStats) so a runtime-set value counts.
    auto envInt = [](const char* name, int fallback) {
        const auto v = juce::SystemStats::getEnvironmentVariable(name, {}).trim();
        return v.isNotEmpty() && v.containsOnly("0123456789") ? v.getIntValue() : fallback;
    };
    decoder->enableParallelDecode(envInt("DCA_WEM_DECODE_THREADS", dcwem::WemVorbisDecoder::defaultDecodeThreads()),
                                  envInt("DCA_WEM_LANE_PACKETS", dcwem::WemVorbisDecoder::kDefaultLanePackets));
    auto w = std::make_unique<dca_wem>();
    w->decoder = std::move(decoder);
    *out = w.release();
    return DCA_OK;
}

// Decodes up to `capacity` frames as planar float into w.planar and returns
// the frame count, or -1 on a corrupt packet.
int64_t readWemPlanar(dca_wem& w, int32_t capacity)
{
    const int channels = w.decoder->info().channels;
    const size_t need = (size_t) channels * (size_t) capacity;
    if (w.planar.size() < need) w.planar.resize(need);
    std::vector<float*> ptrs((size_t) channels);
    for (int ch = 0; ch < channels; ++ch) ptrs[(size_t) ch] = w.planar.data() + (size_t) ch * (size_t) capacity;
    const int64_t got = w.decoder->read(ptrs.data(), channels, capacity);
    if (got == 0 && ! w.reportedEnd)
    {
        w.reportedEnd = true;
        const auto& d = *w.decoder;
        const std::string msg = "wem: decoded " + std::to_string(d.info().packetCount) + " packets; parallel batches="
                              + std::to_string(d.parallelBatches()) + " max segments=" + std::to_string(d.parallelMaxSegments());
        emitLog(DCA_LOG_DEBUG, msg.c_str());
    }
    return got;
}
} // namespace

DCA_API dca_status DCA_CALL dca_wem_open_file(const char* path, int32_t variant, dca_wem_t* out)
{
    if (out) *out = nullptr;
    if (!path || !out || !validWemVariant(variant)) return DCA_ERR_INVALID_ARG;
    return guard("dca_wem_open_file", [&]() -> dca_status {
        juce::File file(juce::String::fromUTF8(path));
        if (!file.existsAsFile())
        {
            emitLog(DCA_LOG_ERROR, "wem_open_file: file not found");
            return DCA_ERR_DECODE;
        }
        // Capped before a byte is read, and read ONCE, straight into the
        // buffer the decoder keeps (it used to go through a MemoryBlock first).
        const juce::int64 size = file.getSize();
        if (size <= 0) return DCA_ERR_DECODE;
        if ((uint64_t) size > (uint64_t) dcwem::kMaxWemBytes)
        {
            emitLog(DCA_LOG_ERROR, "wem_open_file: file larger than the 256 MiB cap");
            return DCA_ERR_DECODE;
        }
        juce::FileInputStream in(file);
        if (in.failedToOpen()) return DCA_ERR_DECODE;
        auto bytes = std::make_shared<std::vector<uint8_t>>((size_t) size);
        if (in.read(bytes->data(), (int) size) != (int) size) return DCA_ERR_DECODE;
        return openWem(std::move(bytes), variant, out);
    });
}

DCA_API dca_status DCA_CALL dca_wem_open_memory(const uint8_t* data, size_t length, int32_t variant, dca_wem_t* out)
{
    if (out) *out = nullptr;
    if (!data || length == 0 || !out || !validWemVariant(variant)) return DCA_ERR_INVALID_ARG;
    return guard("dca_wem_open_memory", [&]() -> dca_status {
        if (length > dcwem::kMaxWemBytes)
        {
            emitLog(DCA_LOG_ERROR, "wem_open_memory: buffer larger than the 256 MiB cap");
            return DCA_ERR_DECODE;
        }
        auto bytes = std::make_shared<std::vector<uint8_t>>(data, data + length);
        return openWem(std::move(bytes), variant, out);
    });
}

DCA_API dca_status DCA_CALL dca_wem_get_info(dca_wem_t wem, dca_wem_info* out_info)
{
    if (!wem || !out_info) return DCA_ERR_INVALID_ARG;
    const auto& i = wem->decoder->info();
    dca_wem_info r{};
    r.channels            = i.channels;
    r.sample_rate         = i.sampleRate;
    r.frames              = i.frames;
    r.header_sample_count = i.headerSampleCount;
    r.decoded_frames      = i.decodedFrames;
    r.variant             = (int32_t) i.variant;
    r.mod_packets         = i.modPackets ? 1 : 0;
    r.granule_present     = i.granulePresent ? 1 : 0;
    r.packet_count        = i.packetCount;
    *out_info = r;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_wem_read_f32(dca_wem_t wem, float* interleaved, int32_t capacity_frames, int32_t* out_frames)
{
    if (out_frames) *out_frames = 0;
    if (!wem || !interleaved || !out_frames || capacity_frames < 0) return DCA_ERR_INVALID_ARG;
    if (capacity_frames == 0) return DCA_OK;
    return guard("dca_wem_read_f32", [&]() -> dca_status {
        const int64_t got = readWemPlanar(*wem, capacity_frames);
        if (got < 0) return DCA_ERR_DECODE;
        const int channels = wem->decoder->info().channels;
        for (int ch = 0; ch < channels; ++ch)
        {
            const float* src = wem->planar.data() + (size_t) ch * (size_t) capacity_frames;
            for (int64_t f = 0; f < got; ++f)
                interleaved[(size_t) f * (size_t) channels + (size_t) ch] = src[f];
        }
        *out_frames = (int32_t) got;
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_wem_read_s16(dca_wem_t wem, int16_t* interleaved, int32_t capacity_frames, int32_t* out_frames)
{
    if (out_frames) *out_frames = 0;
    if (!wem || !interleaved || !out_frames || capacity_frames < 0) return DCA_ERR_INVALID_ARG;
    if (capacity_frames == 0) return DCA_OK;
    return guard("dca_wem_read_s16", [&]() -> dca_status {
        const int64_t got = readWemPlanar(*wem, capacity_frames);
        if (got < 0) return DCA_ERR_DECODE;
        const int channels = wem->decoder->info().channels;
        for (int ch = 0; ch < channels; ++ch)
        {
            const float* src = wem->planar.data() + (size_t) ch * (size_t) capacity_frames;
            for (int64_t f = 0; f < got; ++f)
            {
                // DotCharter's AudioFormatConverter.ConvertToPcm16, bit for bit:
                // clamp, -1 -> -32768, else the FLOAT product (float * int in
                // C#) widened to double and rounded half-to-even (Math.Round's
                // default; nearbyint under the default FE_TONEAREST mode).
                const float v = std::clamp(src[f], -1.0f, 1.0f);
                const int16_t s = v <= -1.0f ? (int16_t) -32768
                                             : (int16_t) std::nearbyint((double) (v * 32767.0f));
                interleaved[(size_t) f * (size_t) channels + (size_t) ch] = s;
            }
        }
        *out_frames = (int32_t) got;
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_wem_seek(dca_wem_t wem, int64_t frame)
{
    if (!wem) return DCA_ERR_INVALID_ARG;
    return guard("dca_wem_seek", [&]() -> dca_status {
        return wem->decoder->seek(frame) ? DCA_OK : DCA_ERR_DECODE;
    });
}

DCA_API dca_status DCA_CALL dca_wem_destroy(dca_wem_t wem)
{
    delete wem;
    return DCA_OK;
}

// ---- Wwise Vorbis (.wem) encoding (ABI v44, DotCharter #317) -----------------
// The encoder is src/wem/WemVorbisEncoder.cpp; this is marshalling only.
DCA_API dca_status DCA_CALL dca_wem_encode_vorbis(const float* interleaved, int64_t frames, int32_t channels,
                                                  int32_t sample_rate, uint8_t** out_data, int64_t* out_size)
{
    if (out_data) *out_data = nullptr;
    if (out_size) *out_size = 0;
    if (!interleaved || !out_data || !out_size || frames <= 0) return DCA_ERR_INVALID_ARG;
    if (channels != dcwem::kWemEncodeChannels || sample_rate != dcwem::kWemEncodeSampleRate) return DCA_ERR_INVALID_ARG;
    return guard("dca_wem_encode_vorbis", [&]() -> dca_status {
        std::vector<uint8_t> wem;
        dcwem::WemEncodeStats stats;
        std::string error;
        if (!dcwem::encodeVorbisWem(interleaved, frames, channels, sample_rate, wem, stats, error))
        {
            const std::string msg = "wem_encode: " + error;
            emitLog(DCA_LOG_ERROR, msg.c_str());
            return DCA_ERR_INTERNAL;
        }
        auto* buffer = static_cast<uint8_t*>(std::malloc(wem.size()));
        if (!buffer) return DCA_ERR_INTERNAL;
        std::memcpy(buffer, wem.data(), wem.size());
        *out_data = buffer;
        *out_size = (int64_t) wem.size();
        return DCA_OK;
    });
}

DCA_API void DCA_CALL dca_wem_encode_free(uint8_t* data)
{
    if (data) std::free(data);
}

DCA_API dca_status DCA_CALL dca_source_open_file(dca_engine_t engine, const char* path, dca_source_t* out)
{
    if (out) *out = nullptr;   // defined before any failure path, like open_memory
    if (!engine || !out || !path) return DCA_ERR_INVALID_ARG;
    return guard("dca_source_open_file", [&]() -> dca_status {
        auto* e = reinterpret_cast<Engine*>(engine);

        auto s = std::make_unique<Source>();
        s->engine    = e;
        s->transport = std::make_unique<dotcharter::TransportSource>(e->impl);

        auto err = s->transport->openFile(juce::File(juce::String::fromUTF8(path)));
        if (err.isNotEmpty())
        {
            emitLog(DCA_LOG_ERROR, err.toRawUTF8());
            return DCA_ERR_DECODE;
        }

        logSourceReadPath(*s->transport, "source_open_file");

        {
            std::lock_guard<std::mutex> lk(e->sourcesMutex);
            e->sources.push_back(s.get());
        }
        *out = reinterpret_cast<dca_source_t>(s.release());
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_source_open_memory(dca_engine_t engine, const uint8_t* bytes, size_t numBytes, const char* formatHint, dca_source_t* out)
{
    if (!engine || !out || !bytes || numBytes == 0) return DCA_ERR_INVALID_ARG;
    return guard("dca_source_open_memory", [&]() -> dca_status {
        auto* e = reinterpret_cast<Engine*>(engine);

        auto s = std::make_unique<Source>();
        s->engine    = e;
        s->transport = std::make_unique<dotcharter::TransportSource>(e->impl);

        const juce::String hint = formatHint ? juce::String::fromUTF8(formatHint) : juce::String();
        auto err = s->transport->openMemory(bytes, numBytes, hint);
        if (err.isNotEmpty())
        {
            emitLog(DCA_LOG_ERROR, err.toRawUTF8());
            *out = nullptr;
            return DCA_ERR_DECODE;
        }

        logSourceReadPath(*s->transport, "source_open_memory");

        {
            std::lock_guard<std::mutex> lk(e->sourcesMutex);
            e->sources.push_back(s.get());
        }
        *out = reinterpret_cast<dca_source_t>(s.release());
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_source_destroy(dca_source_t s)
{
    auto* src = reinterpret_cast<Source*>(s);
    if (src == nullptr) return DCA_OK;
    // Unregister from the owning engine (audit #9). engine == nullptr means
    // dca_engine_destroy already orphaned us (transport is gone too) — only
    // the shell is left to free. The unsynchronised engine read mirrors the
    // plugin D-06 contract: destroy must not race engine_destroy.
    if (auto* e = src->engine)
    {
        std::lock_guard<std::mutex> lk(e->sourcesMutex);
        auto& v = e->sources;
        v.erase(std::remove(v.begin(), v.end(), src), v.end());
    }
    delete src;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_play(dca_source_t s)
{
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    p->transport->play();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_pause(dca_source_t s)
{
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    p->transport->pause();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_stop(dca_source_t s)
{
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    p->transport->stop();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_seek_seconds(dca_source_t s, double seconds)
{
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(seconds)) return DCA_ERR_INVALID_ARG;
    p->transport->seekSeconds(seconds);
    return DCA_OK;
}

// A dead source answered "0.0 seconds, DCA_OK" here until 0.48: a plausible
// number, not an error, from the two exports a playhead is built out of. The
// third member of the family (dca_source_get_audible_position_seconds, below)
// always rejected, so the pair was an inconsistency AND the dangerous half --
// position 0 reads as "the song is at the start" and length 0 as "an empty
// file", which is a stuck playhead or a zero-length chart with nothing logged.
// `*out` is still written to 0 first, so a caller that ignores the status is no
// worse off than it was.
DCA_API dca_status DCA_CALL dca_source_get_position_seconds(dca_source_t s, double* out)
{
    if (!out) return DCA_ERR_INVALID_ARG;
    *out = 0.0;
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    *out = p->transport->getPositionSeconds();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_get_length_seconds(dca_source_t s, double* out)
{
    if (!out) return DCA_ERR_INVALID_ARG;
    *out = 0.0;
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    *out = p->transport->getLengthSeconds();
    return DCA_OK;
}

// ABI v43 (DotCharter #435). The work and the bit-exactness argument live in
// dotcharter::convertFileForDevice (AudioEngine.cpp); this is the boundary:
// argument checks, the guard, the status mapping and one log line a slow open
// can be read against.
DCA_API dca_status DCA_CALL dca_convert_audio_for_device(const char* src_path, const char* dst_wav_path,
                                                         double device_rate, int32_t* out_bits)
{
    if (out_bits) *out_bits = 0;
    if (!src_path || !dst_wav_path || !out_bits) return DCA_ERR_INVALID_ARG;
    // An integral rate only: the WAV header stores an integer, and a file whose
    // header rate differs from the device's by a fraction would convert again.
    if (!dcabi::finite(device_rate) || device_rate <= 0.0 || device_rate > 1.0e6
        || device_rate != std::floor(device_rate))
        return DCA_ERR_INVALID_ARG;
    return guard("dca_convert_audio_for_device", [&]() -> dca_status {
        const juce::File src(juce::String::fromUTF8(src_path));
        const juce::File dst(juce::String::fromUTF8(dst_wav_path));
        if (src == dst) return DCA_ERR_INVALID_ARG;
        if (!src.existsAsFile()) return DCA_ERR_DECODE;

        dotcharter::DeviceRateConversion result;
        dotcharter::convertFileForDevice(src, dst, device_rate, result);
        if (result.error.isNotEmpty())
        {
            emitLog(DCA_LOG_ERROR, result.error.toRawUTF8());
            return result.writeFailed ? DCA_ERR_INTERNAL : DCA_ERR_DECODE;
        }

        *out_bits = result.bits;
        if (result.bits != 0)
        {
            char buf[320];
            std::snprintf(buf, sizeof buf,
                          "convert_for_device: %lld frames at %.0f Hz as %d-bit, step %.9g, rate-converted %s in %.0f ms",
                          (long long) result.frames, device_rate, result.bits, (double) result.step,
                          result.report.polyphase ? "polyphase" : "windowed-sinc", result.report.elapsedMs);
            emitLog(DCA_LOG_INFO, buf);
        }
        return DCA_OK;
    });
}

// ABI v43 test seam: what a preloaded source PLAYS, as one number. Two opens
// that hash alike hold the same int16 samples at the same step, so they are
// audibly -- bit -- identical; frames and rate pin the alignment.
DCA_API dca_status DCA_CALL dca_source_get_preload_digest(dca_source_t s, uint64_t* out_hash, float* out_step,
                                                          int64_t* out_frames, double* out_rate,
                                                          int32_t* out_converted)
{
    if (out_hash)      *out_hash = 0;
    if (out_step)      *out_step = 0.0f;
    if (out_frames)    *out_frames = 0;
    if (out_rate)      *out_rate = 0.0;
    if (out_converted) *out_converted = 0;
    if (!out_hash || !out_step || !out_frames || !out_rate || !out_converted) return DCA_ERR_INVALID_ARG;
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    return guard("dca_source_get_preload_digest", [&]() -> dca_status {
        const auto* held = p->transport->preloadedSource();
        if (held == nullptr) return DCA_ERR_INVALID_ARG;   // streaming: nothing held

        // FNV-1a 64: channel count, frame count, then every sample, channel by
        // channel, little-endian.
        uint64_t h = 0xcbf29ce484222325ull;
        auto mix = [&h](uint8_t b) { h ^= b; h *= 0x100000001b3ull; };
        auto mix64 = [&mix](uint64_t v) { for (int i = 0; i < 8; ++i) mix((uint8_t) (v >> (8 * i))); };
        mix64((uint64_t) held->numChannels());
        mix64((uint64_t) held->numFrames());
        for (int ch = 0; ch < held->numChannels(); ++ch)
        {
            const int16_t* x = held->channelSamples(ch);
            for (int i = 0; i < held->numFrames(); ++i)
            {
                const auto u = (uint16_t) x[i];
                mix((uint8_t) (u & 0xFF));
                mix((uint8_t) (u >> 8));
            }
        }

        *out_hash      = h;
        *out_step      = held->step();
        *out_frames    = held->numFrames();
        *out_rate      = p->transport->playbackSampleRate();
        *out_converted = p->transport->lastResampleReport().converted ? 1 : 0;
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_source_set_volume(dca_source_t s, float gain)
{
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(gain)) return DCA_ERR_INVALID_ARG;
    // A NEGATIVE gain is not "quieter", it is a PHASE INVERSION: the source
    // then cancels against anything correlated in the mix (the same take on
    // another stem, the click, a doubled guitar), which sounds like a hollow
    // mix rather than like a wrong volume. Nothing has ever wanted it here, so
    // it is clamped away at the boundary. There is deliberately NO upper bound:
    // boost above unity is a shipped feature (DotCharter's stem faders reach
    // +6 dB and its per-stem gain contract is documented 0..4, which the
    // stem x master product can carry to 16), and a ceiling invented here would
    // silently cap it.
    p->transport->setVolume(gain < 0.0f ? 0.0f : gain);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_set_tempo(dca_source_t s, float factor)
{
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(factor)) return DCA_ERR_INVALID_ARG;
    p->transport->setTempoFactor(factor);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_set_pitch_semitones(dca_source_t s, float semitones)
{
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(semitones)) return DCA_ERR_INVALID_ARG;
    p->transport->setPitchSemitones(semitones);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_set_pan(dca_source_t s, float pan)
{
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(pan)) return DCA_ERR_INVALID_ARG;
    // Clamped HERE as well as inside PanningAudioSource::setPan, and that
    // redundancy is the point: the ABI is the boundary, and the value a caller
    // caches must be the value the engine runs. Without it a managed wrapper
    // that stores what it wrote reports pan 4.0 while the engine plays hard
    // right — the same "drawn curve disagrees with the audible curve" fault the
    // v29 EQ clamps on both sides to avoid. It also stops being silently
    // correct if `panner_` is ever replaced. Clamped, not rejected: a caller
    // nudging a slider past the end wants the hard left it asked for.
    p->transport->setPan(pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan));
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_stretch_quality(dca_engine_t engine, int32_t quality)
{
    auto* e = reinterpret_cast<Engine*>(engine);
    if (!e) return DCA_ERR_INVALID_ARG;
    // PER ENGINE. This wrote a process-global flag and ignored the handle it
    // was given, so one engine loading stems re-voiced every stretcher in the
    // process — including another engine's, in a host that runs two.
    e->impl.setStretchFaster(quality != 0);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_set_stem_grouped(dca_source_t s, int32_t grouped)
{
    auto* p = reinterpret_cast<Source*>(s);
    if (!p || !p->transport || !p->engine) return DCA_ERR_INVALID_ARG;
    return guard("dca_source_set_stem_grouped", [&]() -> dca_status {
        p->engine->impl.setSourceStemGrouped(p->transport.get(), grouped != 0);
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_engine_set_stem_group_tempo(dca_engine_t engine, float factor)
{
    auto* eng = reinterpret_cast<Engine*>(engine);
    if (!eng) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(factor)) return DCA_ERR_INVALID_ARG;
    eng->impl.setStemGroupTempo(factor);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_stem_group_pitch(dca_engine_t engine, float semitones)
{
    auto* eng = reinterpret_cast<Engine*>(engine);
    if (!eng) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(semitones)) return DCA_ERR_INVALID_ARG;
    eng->impl.setStemGroupPitch(semitones);
    return DCA_OK;
}

// --- Plugins ---------------------------------------------------------------

// ABI v33. A .vst3 bundle may declare several plugins; this counts them so a
// host can offer a picker instead of silently taking the first.
DCA_API dca_status DCA_CALL dca_plugin_count_vst3_components(const char* path, int32_t* out_count)
{
    if (!out_count) return DCA_ERR_INVALID_ARG;
    *out_count = 0;
    if (path == nullptr) return DCA_ERR_INVALID_ARG;
    return guard("dca_plugin_count_vst3_components", [&]() -> dca_status {
    auto& host = JuceHost::instance();
    host.ensureStarted();
    juce::String pluginPath(juce::CharPointer_UTF8{path});
    int found = 0;
    host.callSync([&]
    {
        juce::OwnedArray<juce::PluginDescription> descs;
        host.vst3().findAllTypesForFile(descs, pluginPath);
        found = descs.size();
    });
    if (found <= 0) return DCA_ERR_PLUGIN;
    *out_count = (int32_t) found;
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_plugin_load_vst3(dca_engine_t engine, const char* path, dca_plugin_t* out)
{
    // Pure alias for component 0 — the behaviour every pre-v33 caller has.
    return dca_plugin_load_vst3_component(engine, path, 0, out);
}

DCA_API dca_status DCA_CALL dca_plugin_load_vst3_component(dca_engine_t engine, const char* path,
                                                           int32_t component_index, dca_plugin_t* out)
{
    if (!engine || !out || path == nullptr || component_index < 0) return DCA_ERR_INVALID_ARG;
    *out = nullptr;
    return guard("dca_plugin_load_vst3_component", [&]() -> dca_status {

    auto* eng = reinterpret_cast<Engine*>(engine);
    auto& host = JuceHost::instance();
    host.ensureStarted();

    juce::String pluginPath(juce::CharPointer_UTF8{path});
    const double sr  = eng->impl.currentSampleRate() > 0 ? eng->impl.currentSampleRate() : 44100.0;
    const int    blk = eng->impl.currentBlockSize()  > 0 ? eng->impl.currentBlockSize()  : 512;

    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> inst;
    bool badIndex = false;

    host.callSync([&]
    {
        juce::OwnedArray<juce::PluginDescription> descs;
        host.vst3().findAllTypesForFile(descs, pluginPath);
        if (descs.isEmpty())
        {
            err = "VST3: no descriptors for " + pluginPath;
            return;
        }
        // REJECTED, never clamped: a bundle that declares two plugins and is
        // asked for the third must not quietly hand back the first, or a saved
        // rig reloads as a different effect with nothing saying so.
        if (component_index >= descs.size())
        {
            badIndex = true;
            err = "VST3: component " + juce::String((int) component_index) + " of " + pluginPath
                + " does not exist (" + juce::String(descs.size()) + " declared)";
            return;
        }
        inst = host.vst3().createInstanceFromDescription(*descs[(int) component_index], sr, blk, err);
    });

    if (badIndex)
    {
        emitLog(DCA_LOG_ERROR, err.toRawUTF8());
        return DCA_ERR_INVALID_ARG;
    }

    if (inst == nullptr)
    {
        emitLog(DCA_LOG_ERROR, ("VST3 load failed: " + err).toRawUTF8());
        return DCA_ERR_PLUGIN;
    }

    auto* p = new (std::nothrow) Plugin();
    if (!p) return DCA_ERR_INTERNAL;
    p->engine   = eng;
    p->instance = std::move(inst);
    {
        std::lock_guard<std::mutex> lk(eng->pluginsMutex);
        eng->plugins.push_back(p);
    }
    *out = reinterpret_cast<dca_plugin_t>(p);
    emitLog(DCA_LOG_INFO, ("VST3 loaded: " + pluginPath).toRawUTF8());
    return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_plugin_unload(dca_plugin_t handle)
{
    return guard("dca_plugin_unload", [&]() -> dca_status {
    teardownPluginAndDelete(reinterpret_cast<Plugin*>(handle));
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_plugin_attach_to_input(dca_plugin_t handle)
{
    return guard("dca_plugin_attach_to_input", [&]() -> dca_status {
    return dca_plugin_attach_to_input_for_player(handle, 0);
});
}

DCA_API dca_status DCA_CALL dca_plugin_attach_to_input_for_player(dca_plugin_t handle, int32_t player)
{
    return guard("dca_plugin_attach_to_input_for_player", [&]() -> dca_status {
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->engine || !p->instance) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::AudioEngine::validPlayer((int) player)) return DCA_ERR_INVALID_ARG;
    dca_status result = DCA_OK;
    JuceHost::instance().callSync([&]
    {
        // Serialize the whole transition, including the idempotence check.
        // Two callers must never prepare/append the same instance twice.
        if (p->attached.load() && p->attachedPlayer.load() == (int) player) return;
        if (p->attached.load())
        {
            p->engine->impl.detachInputPlugin(p->instance.get());
            p->attached = false;
            p->attachedPlayer = -1;
        }
        if (!p->engine->impl.attachInputPlugin(p->instance.get(), (int) player))
        {
            result = DCA_ERR_PLUGIN;
            return;
        }
        p->attached = true;
        p->attachedPlayer = (int) player;
    });
    return result;
});
}

DCA_API int32_t DCA_CALL dca_plugin_get_input_player(dca_plugin_t handle)
{
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p) return -1;
    return p->attached.load() ? (int32_t) p->attachedPlayer.load() : -1;
}

DCA_API dca_status DCA_CALL dca_plugin_detach_from_input(dca_plugin_t handle)
{
    return guard("dca_plugin_detach_from_input", [&]() -> dca_status {
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->engine || !p->instance) return DCA_ERR_INVALID_ARG;
    JuceHost::instance().callSync([&]
    {
        if (!p->attached.load()) return;
        p->engine->impl.detachInputPlugin(p->instance.get());
        p->attached = false;
        p->attachedPlayer = -1;
    });
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_plugin_show_editor(dca_plugin_t handle, void* /*parent*/)
{
    return guard("dca_plugin_show_editor", [&]() -> dca_status {
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->instance) return DCA_ERR_INVALID_ARG;

    JuceHost::instance().callSync([&]
    {
        if (p->editor == nullptr)
        {
            if (! p->instance->hasEditor())
                return;
            auto* ed = p->instance->createEditorAndMakeActive();
            if (ed == nullptr)
            {
                // In release builds createEditorAndMakeActive() returns null
                // when the plugin still tracks an active editor (a stale one
                // from an aborted teardown). Drop it and retry once instead
                // of silently showing nothing.
                if (auto* stale = p->instance->getActiveEditor())
                {
                    delete stale;
                    ed = p->instance->createEditorAndMakeActive();
                }

                if (ed == nullptr) return;
            }
            // p0-5: capture a weak alive token. If the plugin is torn down
            // before the async close callback fires, we no-op safely instead
            // of dereferencing a deleted Plugin.
            std::weak_ptr<std::atomic<bool>> aliveWeak = p->alive;
            p->editor = std::make_shared<EditorWindow>(*p->instance, ed, [p, aliveWeak]
            {
                auto alive = aliveWeak.lock();
                if (!alive || !alive->load(std::memory_order_acquire)) return;
                // A reopen between closeButtonPressed and this deferred
                // callback cancels the close (clears closeScheduled_) — in
                // that case the window must survive. Only tear down while
                // the tracked editor still wants to close.
                if (p->editor == nullptr || ! p->editor->closeScheduled_) return;
                p->editor.reset();
                // Tell the host the user closed the window, otherwise its
                // "editor open" state goes stale and the next open request
                // toggles the wrong way ("can't reopen after X").
                if (auto cb = p->editorClosedCb.load(std::memory_order_acquire))
                    cb(p->editorClosedUser.load(std::memory_order_acquire));
            });
        }
        else
        {
            // Reopen. If the native X was clicked a moment ago the teardown
            // is still queued behind us on this thread — cancel it, or the
            // window we are about to show would be destroyed underneath the
            // user as soon as the queued callback runs.
            p->editor->closeScheduled_ = false;
            p->editor->setVisible(true);
            p->editor->toFront(true);
        }
    });
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_plugin_set_editor_closed_callback(dca_plugin_t handle, dca_editor_closed_callback cb, void* user)
{
    return guard("dca_plugin_set_editor_closed_callback", [&]() -> dca_status {
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p) return DCA_ERR_INVALID_ARG;
    p->editorClosedUser.store(user, std::memory_order_release);
    p->editorClosedCb.store(cb, std::memory_order_release);
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_plugin_hide_editor(dca_plugin_t handle)
{
    return guard("dca_plugin_hide_editor", [&]() -> dca_status {
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p) return DCA_ERR_INVALID_ARG;
    JuceHost::instance().callSync([&]
    {
        if (p->editor) p->editor->setVisible(false);
    });
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_plugin_save_state(dca_plugin_t handle, uint8_t* buffer, int32_t capacity, int32_t* out)
{
    return guard("dca_plugin_save_state", [&]() -> dca_status {
    if (!out) return DCA_ERR_INVALID_ARG;
    *out = 0;
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p) return DCA_ERR_INVALID_ARG;
    // A live plugin that has not been instantiated yet legitimately HAS no
    // state: *out stays 0 and that is a successful answer, not a bad argument.
    if (!p->instance) return DCA_OK;

    juce::MemoryBlock block;
    JuceHost::instance().callSync([&]
    {
        p->instance->getStateInformation(block);
    });
    // A state past INT32_MAX cannot be reported through the int32 out-param;
    // the cast used to make it negative and memcpy then read it as a huge size.
    if (! dcabi::copyBlockOut(block.getData(), block.getSize(), buffer, capacity, out))
    {
        emitLog(DCA_LOG_ERROR, "plugin_save_state: the plugin's state is larger than 2 GiB");
        return DCA_ERR_PLUGIN;
    }
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_plugin_load_state(dca_plugin_t handle, const uint8_t* data, int32_t length)
{
    return guard("dca_plugin_load_state", [&]() -> dca_status {
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->instance) return DCA_ERR_INVALID_ARG;
    if (data == nullptr || length <= 0) return DCA_OK;
    JuceHost::instance().callSync([&]
    {
        p->instance->setStateInformation(data, length);
    });
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_plugin_attach_to_instrument(dca_plugin_t handle)
{
    return guard("dca_plugin_attach_to_instrument", [&]() -> dca_status {
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->engine || !p->instance) return DCA_ERR_INVALID_ARG;
    if (p->attachedAsInstrument) return DCA_OK;

    // Detaching any previously attached instrument from the same engine is
    // the caller's responsibility — we replace the slot here either way.
    bool ok = false;
    JuceHost::instance().callSync([&]
    {
        ok = p->engine->impl.attachInstrumentPlugin(p->instance.get());
    });
    if (!ok) return DCA_ERR_PLUGIN;
    p->attachedAsInstrument = true;
    return DCA_OK;
});
}

DCA_API dca_status DCA_CALL dca_plugin_detach_from_instrument(dca_plugin_t handle)
{
    return guard("dca_plugin_detach_from_instrument", [&]() -> dca_status {
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->engine || !p->instance) return DCA_ERR_INVALID_ARG;
    // SAME TRANSACTION BOUNDARY AS THE ATTACH. attach_to_instrument serialises
    // the whole check-and-swap on the message thread; this ran the detach and
    // flipped `attachedAsInstrument` on the CALLER's thread, so a detach could
    // interleave with an attach of another plugin into the same slot and leave
    // the flags describing a slot state that never existed. Same shape as
    // detach_from_input, which has always taken this route.
    //
    // callSync calls straight through when it is already ON the message thread
    // (existsAndIsCurrentThread), so this cannot deadlock a caller that arrives
    // from a JUCE callback — see the D-07 note on callSync.
    JuceHost::instance().callSync([&]
    {
        if (! p->attachedAsInstrument) return;
        p->engine->impl.detachInstrumentPlugin(p->instance.get());
        p->attachedAsInstrument = false;
    });
    return DCA_OK;
});
}

// --- Out-of-process VST bridge (ABI v19) -----------------------------------

DCA_API dca_status DCA_CALL dca_engine_set_remote_bridge(
    dca_engine_t handle,
    const char*  shm_name,
    const char*  request_event_name,
    const char*  response_event_name,
    int32_t      enable)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    if (enable != 0 && (shm_name == nullptr || shm_name[0] == '\0'
                        || request_event_name == nullptr || request_event_name[0] == '\0'
                        || response_event_name == nullptr || response_event_name[0] == '\0'))
        return DCA_ERR_INVALID_ARG;
    return guard("dca_engine_set_remote_bridge", [&]() -> dca_status {
        const bool ok = e->impl.setRemoteBridge(shm_name, request_event_name,
                                                response_event_name, enable != 0);
        if (!ok)
        {
            emitLog(DCA_LOG_ERROR, "remote bridge: failed to create shared memory / events");
            return DCA_ERR_DEVICE;
        }
        emitLog(DCA_LOG_INFO, enable != 0 ? "remote VST bridge armed" : "remote VST bridge disarmed");
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_engine_get_remote_bridge_stats(
    dca_engine_t handle,
    uint32_t*    out_processed,
    uint32_t*    out_timeouts,
    uint32_t*    out_heartbeat)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    uint32_t processed = 0, timeouts = 0, heartbeat = 0;
    e->impl.getRemoteBridgeStats(processed, timeouts, heartbeat);
    if (out_processed) *out_processed = processed;
    if (out_timeouts)  *out_timeouts  = timeouts;
    if (out_heartbeat) *out_heartbeat = heartbeat;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_send_midi_short(dca_engine_t handle, uint8_t status, uint8_t data1, uint8_t data2)
{
    if (!handle) return DCA_ERR_INVALID_ARG;
    auto* eng = reinterpret_cast<Engine*>(handle);
    eng->impl.sendMidiShort(status, data1, data2);
    return DCA_OK;
}

// ---- Sample-accurate MIDI (v34 (engine)) ---------------------------------
DCA_API dca_status DCA_CALL dca_engine_send_midi_at(dca_engine_t handle,
                                                    uint32_t     packed,
                                                    int64_t      render_frame)
{
    if (!handle) return DCA_ERR_INVALID_ARG;
    // The status byte is the low byte and MUST have its high bit set. 0 is also
    // this queue's empty-slot marker, so accepting one would make the audio
    // thread's parked set unreadable — reject rather than forward garbage to
    // somebody's sampler.
    if ((packed & 0xFFu) < 0x80u) return DCA_ERR_INVALID_ARG;
    auto* eng = reinterpret_cast<Engine*>(handle);
    eng->impl.sendMidiAt(packed, render_frame);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_get_midi_timing_stats(dca_engine_t handle,
                                                             int64_t* out_scheduled,
                                                             int64_t* out_late,
                                                             int64_t* out_dropped)
{
    if (out_scheduled) *out_scheduled = 0;
    if (out_late)      *out_late      = 0;
    if (out_dropped)   *out_dropped   = 0;
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    int64_t scheduled = 0, late = 0, dropped = 0;
    e->impl.getMidiTimingStats(scheduled, late, dropped);
    if (out_scheduled) *out_scheduled = scheduled;
    if (out_late)      *out_late      = late;
    if (out_dropped)   *out_dropped   = dropped;
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_reset_midi_timing_stats(dca_engine_t handle)
{
    auto* e = reinterpret_cast<Engine*>(handle);
    if (!e) return DCA_ERR_INVALID_ARG;
    e->impl.resetMidiTimingStats();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_engine_set_instrument_gain(dca_engine_t handle, float g)
{
    if (!handle) return DCA_ERR_INVALID_ARG;
    if (!dcabi::finite(g)) return DCA_ERR_INVALID_ARG;
    auto* eng = reinterpret_cast<Engine*>(handle);
    eng->impl.setInstrumentGain(g);
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_source_get_audible_position_seconds(dca_source_t s, double* out)
{
    if (!out) return DCA_ERR_INVALID_ARG;
    *out = 0.0;
    auto* src = reinterpret_cast<Source*>(s);
    if (!src || !src->transport) return DCA_ERR_INVALID_ARG;
    *out = src->transport->getAudiblePositionSeconds();
    return DCA_OK;
}

// =========================================================================
// ABI v4 additions
// =========================================================================

// ---- VST3 plugin parameters --------------------------------------------

DCA_API dca_status DCA_CALL dca_plugin_get_parameter_count(dca_plugin_t handle, int32_t* out)
{
    if (!out) return DCA_ERR_INVALID_ARG;
    *out = 0;
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->instance) return DCA_ERR_INVALID_ARG;
    return guard("plugin_get_parameter_count", [&]
    {
        *out = p->instance->getParameters().size();
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_plugin_get_parameter_info(dca_plugin_t handle,
                                                          int32_t idx,
                                                          char* nameBuf,
                                                          int32_t nameCap,
                                                          int32_t* outLen,
                                                          float* outDefault,
                                                          int32_t* outSteps)
{
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->instance || idx < 0) return DCA_ERR_INVALID_ARG;
    return guard("plugin_get_parameter_info", [&]
    {
        const auto& params = p->instance->getParameters();
        if (idx >= params.size()) return DCA_ERR_INVALID_ARG;
        auto* par = params[idx];
        if (par == nullptr) return DCA_ERR_INVALID_ARG;

        const juce::String name = par->getName(256);
        const auto utf8 = name.toRawUTF8();
        const int32_t needed = (int32_t) std::strlen(utf8);
        if (outLen) *outLen = needed;
        if (nameBuf && nameCap > 0)
        {
            // Code-point boundary, never a byte one: a VST3 parameter name is
            // routinely non-ASCII ("Frequenz", "ディレイ", "µ-Law"), and a torn
            // sequence decodes to U+FFFD — a short buffer CORRUPTS the tail
            // rather than clipping it. dcabi::utf8TruncatedLength is the one
            // implementation behind every fixed out buffer in this ABI.
            const size_t n = dcabi::utf8TruncatedLength(utf8, (size_t) needed,
                                                        (size_t) nameCap - 1);
            if (n > 0) std::memcpy(nameBuf, utf8, n);
            nameBuf[n] = '\0';
        }
        if (outDefault) *outDefault = par->getDefaultValue();
        if (outSteps)   *outSteps   = par->getNumSteps();
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_plugin_get_parameter(dca_plugin_t handle, int32_t idx, float* out)
{
    if (!out) return DCA_ERR_INVALID_ARG;
    *out = 0.0f;
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->instance || idx < 0) return DCA_ERR_INVALID_ARG;
    return guard("plugin_get_parameter", [&]
    {
        const auto& params = p->instance->getParameters();
        if (idx >= params.size()) return DCA_ERR_INVALID_ARG;
        *out = params[idx]->getValue();
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_plugin_set_parameter(dca_plugin_t handle, int32_t idx, float value)
{
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->instance || idx < 0) return DCA_ERR_INVALID_ARG;
    // std::clamp(NaN, 0, 1) returns NaN, so the clamp below is not a guard.
    if (!dcabi::finite(value)) return DCA_ERR_INVALID_ARG;
    return guard("plugin_set_parameter", [&]
    {
        const auto& params = p->instance->getParameters();
        if (idx >= params.size()) return DCA_ERR_INVALID_ARG;
        auto* par = params[idx];
        const float clamped = std::clamp(value, 0.0f, 1.0f);
        // setValueNotifyingHost is the host-side path; safe from any thread
        // for VST3 because juce wraps it in an atomic + host notification.
        par->setValueNotifyingHost(clamped);
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_plugin_get_latency_samples(dca_plugin_t handle, int32_t* out)
{
    if (!out) return DCA_ERR_INVALID_ARG;
    *out = 0;
    auto* p = reinterpret_cast<Plugin*>(handle);
    if (!p || !p->instance) return DCA_ERR_INVALID_ARG;
    return guard("plugin_get_latency_samples", [&]
    {
        // AudioProcessor::getLatencySamples() is plain int; JUCE callers
        // treat negative values as "unknown" so clamp to >= 0.
        const int s = p->instance->getLatencySamples();
        *out = s > 0 ? s : 0;
        return DCA_OK;
    });
}

// ---- MIDI hardware input -------------------------------------------------

namespace {

struct MidiInputBridge : public juce::MidiInputCallback
{
    dca_midi_callback callback = nullptr;
    void*             user     = nullptr;
    std::unique_ptr<juce::MidiInput> device;

    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override
    {
        if (callback)
        {
            callback(user,
                     m.getTimeStamp(),
                     m.getRawData(),
                     m.getRawDataSize());
        }
    }
};

} // namespace

DCA_API dca_status DCA_CALL dca_midi_input_enumerate(char* buffer, int32_t capacity, int32_t* outSize, int32_t* outCount)
{
    if (!outSize || !outCount) return DCA_ERR_INVALID_ARG;
    *outSize = 0; *outCount = 0;
    return guard("midi_input_enumerate", [&]
    {
        auto devices = juce::MidiInput::getAvailableDevices();
        juce::String concat;
        for (int i = 0; i < devices.size(); ++i)
        {
            if (i > 0) concat += "\n";
            concat += devices[i].name;
        }
        const auto utf8 = concat.toRawUTF8();
        const int32_t needed = (int32_t) std::strlen(utf8) + 1; // include NUL
        *outSize  = needed;
        *outCount = devices.size();
        if (buffer && capacity > 0)
        {
            // `needed` counts the terminator; the truncator works in payload
            // bytes. MIDI device names are user-visible hardware names and are
            // frequently non-ASCII, so this cut has to land on a code point.
            const size_t n = dcabi::utf8TruncatedLength(utf8, (size_t) needed - 1,
                                                        (size_t) capacity - 1);
            if (n > 0) std::memcpy(buffer, utf8, n);
            buffer[n] = '\0';
        }
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_midi_input_open(int32_t deviceIndex,
                                                dca_midi_callback cb,
                                                void* user,
                                                dca_midi_in_t* out)
{
    if (!out || cb == nullptr || deviceIndex < 0) return DCA_ERR_INVALID_ARG;
    *out = nullptr;
    return guard("midi_input_open", [&]
    {
        auto devices = juce::MidiInput::getAvailableDevices();
        if (deviceIndex >= devices.size()) return DCA_ERR_INVALID_ARG;

        auto bridge = std::make_unique<MidiInputBridge>();
        bridge->callback = cb;
        bridge->user     = user;

        bridge->device = juce::MidiInput::openDevice(devices[deviceIndex].identifier, bridge.get());
        if (bridge->device == nullptr)
        {
            emitLog(DCA_LOG_ERROR, ("MIDI input open failed: " + devices[deviceIndex].name).toRawUTF8());
            return DCA_ERR_DEVICE;
        }
        bridge->device->start();
        *out = reinterpret_cast<dca_midi_in_t>(bridge.release());
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_midi_input_close(dca_midi_in_t handle)
{
    auto* b = reinterpret_cast<MidiInputBridge*>(handle);
    if (!b) return DCA_OK;
    return guard("midi_input_close", [&]
    {
        if (b->device) b->device->stop();
        b->device.reset();
        delete b;
        return DCA_OK;
    });
}

// ---- MIDI hardware OUTPUT (v34 (engine)) ---------------------------------

/// A queue-full DROP is not a bad argument. Until ABI v35 both surfaced as
/// DCA_ERR_INVALID_ARG, so a saturated dispatcher told the caller its
/// ARGUMENTS were wrong — which is what the caller would then believe, while
/// the note-off it lost became a stuck note. DCA_ERR_DEVICE says "the device
/// could not take it"; the count is in dca_midi_output_get_counts.
static dca_status midiOutStatus(dotcharter::MidiOutputDevice::SendResult r)
{
    switch (r)
    {
        case dotcharter::MidiOutputDevice::SendResult::Ok:        return DCA_OK;
        case dotcharter::MidiOutputDevice::SendResult::QueueFull: return DCA_ERR_DEVICE;
        case dotcharter::MidiOutputDevice::SendResult::BadArgs:   break;
    }
    return DCA_ERR_INVALID_ARG;
}

DCA_API dca_status DCA_CALL dca_midi_output_enumerate(char* buffer, int32_t capacity,
                                                      int32_t* outSize, int32_t* outCount)
{
    if (!outSize || !outCount) return DCA_ERR_INVALID_ARG;
    *outSize = 0; *outCount = 0;
    return guard("midi_output_enumerate", [&]
    {
        auto devices = dotcharter::MidiOutputDevice::enumerate();
        juce::String concat;
        for (int i = 0; i < devices.size(); ++i)
        {
            if (i > 0) concat += "\n";
            concat += devices[i].name;
        }
        const auto utf8 = concat.toRawUTF8();
        const int32_t needed = (int32_t) std::strlen(utf8) + 1; // include NUL
        *outSize  = needed;
        *outCount = devices.size();
        if (buffer && capacity > 0)
        {
            // Same code-point-safe cut as dca_midi_input_enumerate: MIDI device
            // names are user-visible hardware names and are frequently
            // non-ASCII, and a byte-wise truncation decodes to U+FFFD, i.e. it
            // CORRUPTS the tail rather than clipping it.
            const size_t n = dcabi::utf8TruncatedLength(utf8, (size_t) needed - 1,
                                                        (size_t) capacity - 1);
            if (n > 0) std::memcpy(buffer, utf8, n);
            buffer[n] = '\0';
        }
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_midi_output_open(int32_t deviceIndex, dca_midi_out_t* out)
{
    if (!out || deviceIndex < 0) return DCA_ERR_INVALID_ARG;
    *out = nullptr;
    return guard("midi_output_open", [&]
    {
        auto device = dotcharter::MidiOutputDevice::open(deviceIndex);
        if (device == nullptr)
        {
            emitLog(DCA_LOG_ERROR, "MIDI output open failed");
            return DCA_ERR_DEVICE;
        }
        *out = reinterpret_cast<dca_midi_out_t>(device.release());
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_midi_output_send(dca_midi_out_t handle,
                                                 const uint8_t* data, int32_t length)
{
    auto* d = reinterpret_cast<dotcharter::MidiOutputDevice*>(handle);
    if (!d || !data || length <= 0) return DCA_ERR_INVALID_ARG;
    return guard("midi_output_send", [&]
    {
        // deadline 0 = "as soon as the dispatcher can". Still OFF the caller's
        // thread: a wedged MIDI interface must never block a UI.
        return midiOutStatus(d->send(data, length, 0));
    });
}

DCA_API dca_status DCA_CALL dca_midi_output_send_at(dca_midi_out_t handle,
                                                    dca_engine_t   engine,
                                                    const uint8_t* data,
                                                    int32_t        length,
                                                    int64_t        render_frame)
{
    auto* d = reinterpret_cast<dotcharter::MidiOutputDevice*>(handle);
    auto* e = reinterpret_cast<Engine*>(engine);
    if (!d || !e || !data || length <= 0) return DCA_ERR_INVALID_ARG;
    return guard("midi_output_send_at", [&]() -> dca_status
    {
        // THE FRAME -> DEADLINE CONVERSION HAPPENS HERE, on the CALLER's thread,
        // and that is the whole reason the engine is a parameter of this call
        // and not a member of the output handle. A MidiOutputDevice that cached
        // an AudioEngine* would outlive it the moment a caller destroyed the
        // engine first — and the frame domain belongs to the engine anyway.
        int64_t clockFrames = 0, clockTicks = 0;
        if (! e->impl.getRenderClock(clockFrames, clockTicks))
            return midiOutStatus(d->send(data, length, 0));

        const double sr = (double) e->impl.deviceSnapshot().sampleRate;
        if (! (sr > 0.0))
            return midiOutStatus(d->send(data, length, 0));

        const double ticksPerSecond = (double) juce::Time::getHighResolutionTicksPerSecond();
        // The render clock's frame F is the sample the callback has just
        // RENDERED, not the one being heard: F becomes audible one output
        // latency later. A hardware synth's note has to line up with what the
        // user hears, so the deadline carries that offset — the same figure
        // dca_engine_get_output_latency_samples reports, master limiter
        // included.
        const double latencySeconds =
            ((double) e->impl.deviceSnapshot().outputLatency
             + (double) e->impl.masterLimiterLatencySamples()) / sr;
        const double aheadSeconds = (double) (render_frame - clockFrames) / sr + latencySeconds;

        int64_t deadline = clockTicks + (int64_t) (aheadSeconds * ticksPerSecond);
        if (render_frame < 0) deadline = 0;   // "as soon as possible", same as _send
        return midiOutStatus(d->send(data, length, deadline));
    });
}

DCA_API dca_status DCA_CALL dca_midi_output_get_sent_count(dca_midi_out_t handle, int64_t* out_count)
{
    if (out_count) *out_count = 0;
    auto* d = reinterpret_cast<dotcharter::MidiOutputDevice*>(handle);
    if (!d || !out_count) return DCA_ERR_INVALID_ARG;
    *out_count = d->sentCount();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_midi_output_get_counts(dca_midi_out_t handle,
                                                       int64_t* out_sent,
                                                       int64_t* out_dropped)
{
    if (out_sent)    *out_sent    = 0;
    if (out_dropped) *out_dropped = 0;
    auto* d = reinterpret_cast<dotcharter::MidiOutputDevice*>(handle);
    if (!d || (!out_sent && !out_dropped)) return DCA_ERR_INVALID_ARG;
    if (out_sent)    *out_sent    = d->sentCount();
    if (out_dropped) *out_dropped = d->droppedCount();
    return DCA_OK;
}

// v36 (W4-N15). All three counters, because `failed` had no way out at all: the
// dispatcher has counted messages the DRIVER threw on since v35 and nothing
// could read it. Distinct from `dropped` — dropped is "our queue was full and
// we never handed it over", failed is "it reached sendMessageNow and did not
// come out the other side" — and before those two were split, `sent` moved on a
// throw, so a wedged interface read as working.
DCA_API dca_status DCA_CALL dca_midi_output_get_counts3(dca_midi_out_t handle,
                                                        int64_t* out_sent,
                                                        int64_t* out_dropped,
                                                        int64_t* out_failed)
{
    // Zeroed BEFORE the first possible failure, like every other getter here.
    if (out_sent)    *out_sent    = 0;
    if (out_dropped) *out_dropped = 0;
    if (out_failed)  *out_failed  = 0;
    auto* d = reinterpret_cast<dotcharter::MidiOutputDevice*>(handle);
    if (!d || (!out_sent && !out_dropped && !out_failed)) return DCA_ERR_INVALID_ARG;
    if (out_sent)    *out_sent    = d->sentCount();
    if (out_dropped) *out_dropped = d->droppedCount();
    if (out_failed)  *out_failed  = d->failedCount();
    return DCA_OK;
}

DCA_API dca_status DCA_CALL dca_midi_output_close(dca_midi_out_t handle)
{
    auto* d = reinterpret_cast<dotcharter::MidiOutputDevice*>(handle);
    if (!d) return DCA_OK;
    return guard("midi_output_close", [&]() -> dca_status
    {
        // BOUNDED join, then LEAK. shutdown() never calls stopThread(), which
        // would TerminateThread a dispatcher parked inside the OS MIDI API and
        // then destroy the juce::MidiOutput under it. On expiry the device
        // object (its thread and its juce::MidiOutput) is leaked on purpose:
        // this is the same trade NativeHandleOwner's drain makes, and a leak
        // beats a use-after-free.
        if (! d->shutdown())
        {
            emitLog(DCA_LOG_ERROR,
                    "MIDI output close: the dispatcher thread did not exit. LEAKING the device "
                    "instead of destroying it — terminating a thread inside the driver and then "
                    "freeing the device under it is process-wide corruption, not a lost message.");
            return DCA_ERR_DEVICE;
        }
        delete d;
        return DCA_OK;
    });
}

// ---- Offline render -------------------------------------------------------

// p3-53 NOTE: tempo and semitones arguments are accepted for ABI stability
// but currently treated as pass-through — the offline render uses a simple
// 1:1 read/write loop without Rubber Band stretch. Pitch/tempo support is
// tracked in JuceSharp issue #render-dsp. Live playback (dca_source_play
// path) does honour these via RubberBandAudioSource.
//
// p2-18: also validates sampleRate / tempo ranges instead of silently
// clamping nonsense (negative SR, NaN tempo) to a default.
DCA_API dca_status DCA_CALL dca_render_to_wav(const char* srcPath,
                                              const char* dstPath,
                                              float tempo,
                                              float semitones,
                                              int32_t sampleRate)
{
    if (!srcPath || !dstPath) return DCA_ERR_INVALID_ARG;
    // p2-18: explicit range validation. tempo==0 / sampleRate<=0 mean "use
    // source default"; negative, NaN, or Inf values are nonsense.
    if (!(tempo >= 0.0f) || !std::isfinite(tempo))       return DCA_ERR_INVALID_ARG;
    if (!std::isfinite(semitones))                       return DCA_ERR_INVALID_ARG;
    if (sampleRate < 0 || sampleRate > 384000)           return DCA_ERR_INVALID_ARG;
    // A CAPABILITY THIS EXPORT DOES NOT HAVE MUST NOT LOOK LIVE. The header
    // promised "the same Rubber Band stretcher used by the live engine"; the
    // body has always been `(void) t; (void) st;` behind a comment reading
    // "real DSP TBD", so a caller asking for -2 semitones got a bit-exact copy
    // of the source at the original pitch, with DCA_OK. That is the worst kind
    // of wrong answer: a practice file that is silently NOT transposed reads as
    // "the render worked and my ears are wrong".
    //
    // DCA_ERR_INVALID_ARG rather than a new code: this ABI has no
    // NOT_SUPPORTED, and adding one to the enum is an ABI change for a
    // rejection. tempo == 0 still means "source default" (i.e. 1.0), so it is
    // accepted; anything else is refused until the stretcher is really wired in.
    if ((tempo != 0.0f && tempo != 1.0f) || semitones != 0.0f) return DCA_ERR_INVALID_ARG;
    return guard("render_to_wav", [&]
    {
        juce::File src(juce::String(juce::CharPointer_UTF8{srcPath}));
        juce::File dst(juce::String(juce::CharPointer_UTF8{dstPath}));
        if (! src.existsAsFile()) return DCA_ERR_INVALID_ARG;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(src));
        if (reader == nullptr) return DCA_ERR_DECODE;

        const double srcSr = reader->sampleRate;
        const int outSr    = sampleRate > 0 ? sampleRate : (int) srcSr;
        const int outCh    = (int) reader->numChannels;

        dst.deleteFile();
        std::unique_ptr<juce::FileOutputStream> stream(dst.createOutputStream());
        if (stream == nullptr) return DCA_ERR_INTERNAL;

        juce::WavAudioFormat wav;
        // NOTE (D-01): juce::AudioFormatWriterOptions and the matching
        // createWriterFor(unique_ptr<OutputStream>&, const AudioFormatWriterOptions&)
        // overload were introduced in JUCE 8 (see juce_AudioFormatWriterOptions.h).
        // Earlier JUCE versions only expose the variadic-arg createWriterFor(...).
        // If this file is ever ported back to a pre-JUCE-8 tree, replace the
        // call below with the legacy form.
        const auto writerOptions = juce::AudioFormatWriterOptions{}
            .withSampleRate ((double) outSr)
            .withNumChannels (outCh)
            .withBitsPerSample (16);
        auto streamForWriter = std::unique_ptr<juce::OutputStream> (stream.release());
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (streamForWriter, writerOptions));
        if (writer == nullptr) return DCA_ERR_INTERNAL;

        const juce::int64 srcSamples = reader->lengthInSamples;
        const int blockSize = 4096;
        const bool converting = srcSr > 0.0 && std::llround(srcSr) != (juce::int64) outSr;

        // A RATE THE CALLER ASKED FOR IS A RATE THE FILE MUST COME OUT AT.
        // `outSr` went onto the WAV header while the samples were copied 1:1,
        // so a 44.1 kHz source "rendered at 48000" was written as a 48 kHz file
        // containing 44.1 kHz audio: it plays 8.8% fast and 1.5 semitones sharp,
        // with DCA_OK and a header (dotcharter_audio.h) promising conversion.
        // The same class of wrong answer as the stretch guard above — a
        // capability that cannot be honoured must never look live.
        //
        // juce::ResamplingAudioSource, not src/LoadResampler.h: dcload converts
        // a whole decoded buffer in RAM, and this export streams so that it can
        // render a file of any length. JUCE's converter pre-filters on the
        // downward direction, which is the one dcload deliberately refuses.
        if (converting)
        {
            juce::AudioFormatReaderSource readerSource(reader.release(), true);
            juce::ResamplingAudioSource rs(&readerSource, false, outCh);
            rs.setResamplingRatio(srcSr / (double) outSr);
            rs.prepareToPlay(blockSize, (double) outSr);

            const juce::int64 outFrames =
                (juce::int64) std::llround((double) srcSamples * (double) outSr / srcSr);
            juce::AudioBuffer<float> buffer(outCh, blockSize);
            for (juce::int64 pos = 0; pos < outFrames; pos += blockSize)
            {
                const int n = (int) std::min<juce::int64>(blockSize, outFrames - pos);
                buffer.clear();
                juce::AudioSourceChannelInfo info(&buffer, 0, n);
                rs.getNextAudioBlock(info);
                writer->writeFromAudioSampleBuffer(buffer, 0, n);
            }
            rs.releaseResources();
            writer.reset();
            return DCA_OK;
        }

        juce::AudioBuffer<float> buffer(outCh, blockSize);
        for (juce::int64 pos = 0; pos < srcSamples; pos += blockSize)
        {
            const int n = (int) std::min<juce::int64>(blockSize, srcSamples - pos);
            buffer.clear();
            reader->read(&buffer, 0, n, pos, true, true);

            // No stretch, no shift: the argument check above has already
            // refused every request for one, so this loop only ever runs at
            // tempo 1.0 / 0 semitones and is an honest format conversion.
            // Wiring RubberBandAudioSource in here is the fix that would let
            // the guard be relaxed — do that before touching the guard.
            writer->writeFromAudioSampleBuffer(buffer, 0, n);
        }
        writer.reset();
        return DCA_OK;
    });
}

// ---- AI stem separation (ABI v5) ----------------------------------------
DCA_API int32_t DCA_CALL dca_stems_is_supported(void)
{
    return dotcharter::stems::is_supported() ? 1 : 0;
}

DCA_API dca_status DCA_CALL dca_stems_describe_backend(char* buffer, int32_t capacity, int32_t* outSize)
{
    return guard("stems_describe_backend", [&]
    {
        const auto desc = dotcharter::stems::describe_backend();
        // `required` is the FULL size including the terminator, so a caller can
        // size its buffer from one probing call with capacity 0.
        if (outSize) *outSize = (int32_t) desc.size() + 1;
        if (!buffer || capacity <= 0) return DCA_OK;
        // A byte-wise cut TEARS a UTF-8 sequence, and a torn sequence decodes to
        // U+FFFD on the managed side -- it CORRUPTS the tail rather than clipping
        // it. dcabi::utf8TruncatedLength is the one implementation behind every
        // fixed out buffer in this ABI (src/Utf8Truncate.h), and this export was
        // the last hand-rolled memcpy. The description is ASCII today, which is
        // exactly why it survived -- so did the LPStr marshalling bug, for six
        // ABI versions. (It was NOT the last one: three more hand-rolled cuts
        // outlived this comment and went 2026-09-10 —
        // dca_plugin_get_parameter_info, dca_midi_input_enumerate and
        // AudioEngine::enumerateHostTypes.)
        copyStdTruncated(buffer, capacity, desc);
        return DCA_OK;
    });
}

DCA_API dca_status DCA_CALL dca_stems_separate_file(const char*           srcPath,
                                                    const char*           outDir,
                                                    const char*           modelPath,
                                                    int32_t               numThreads,
                                                    dca_stems_progress_cb progressCb,
                                                    void*                 progressUser)
{
    if (!srcPath || !outDir || !modelPath) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::stems::is_supported()) return DCA_ERR_NOT_INITIALIZED;
    return guard("stems_separate_file", [&]
    {
        dotcharter::stems::ProgressCallback cb;
        if (progressCb)
            cb = [progressCb, progressUser](float frac, const char* stage)
            {
                // The progress callback originates in managed (.NET) code.
                // If the managed delegate throws or has been collected by
                // the runtime, the exception would unwind through the
                // native inference stack with no catch frame at the ABI
                // boundary. Swallow everything here.
                try { progressCb(progressUser, frac, stage ? stage : ""); }
                catch (...) { /* never let managed exceptions cross back */ }
            };
        const int rc = dotcharter::stems::separate_file(
            std::string(srcPath), std::string(outDir),
            std::string(modelPath), numThreads, cb);
        if (rc == 0)  return DCA_OK;
        if (rc == 1)  return DCA_ERR_INVALID_ARG;      // an empty path argument
        // W4-N19: NOT DCA_ERR_INVALID_ARG. "the weights are not on disk" and
        // "you passed an empty string" were the same answer to the caller, and
        // they are not the same problem: one is a caller bug, the other is a
        // DOWNLOAD the host can go and perform — which is the whole reason
        // DemucsGgmlModelCatalog exists. NOT_INITIALIZED is already this
        // export's code for "the backend is absent", i.e. "this feature is not
        // ready to run yet", which is exactly what a missing model file means.
        if (rc == 2)  return DCA_ERR_NOT_INITIALIZED;  // model file missing
        if (rc == 3)  return DCA_ERR_DECODE;      // model load failed
        if (rc == 4)  return DCA_ERR_DECODE;      // audio decode failed
        return DCA_ERR_INTERNAL;
    });
}

// v36 (W4-N19). The cancellable twin. Everything about it is the export above
// plus one answer the v35 callback had no room for; the status mapping is
// shared with it through separate_file_cancellable, which is the single
// implementation both entry points run.
DCA_API dca_status DCA_CALL dca_stems_separate_file2(const char*            srcPath,
                                                     const char*            outDir,
                                                     const char*            modelPath,
                                                     int32_t                numThreads,
                                                     dca_stems_progress_cb2 progressCb,
                                                     void*                  progressUser)
{
    if (!srcPath || !outDir || !modelPath) return DCA_ERR_INVALID_ARG;
    if (!dotcharter::stems::is_supported()) return DCA_ERR_NOT_INITIALIZED;
    return guard("stems_separate_file2", [&]
    {
        dotcharter::stems::CancellableProgressCallback cb;
        if (progressCb)
            cb = [progressCb, progressUser](float frac, const char* stage) -> bool
            {
                // The progress callback originates in managed (.NET) code and
                // is invoked ON THE THREAD THAT CALLED THIS EXPORT — every
                // checked point in separate_file_cancellable (the coarse phases
                // and the per-target WAV writes) is on the caller's thread, and
                // demucscppthreaded::threaded_inference takes no progress hook
                // at all, so nothing ever calls it from a demucs worker. (This
                // comment said "worker thread" until W5-E10; the guarantee
                // below is required either way, but a binding author reads this
                // for the threading contract.) A throw here would unwind
                // through C++ frames: a process kill, not an error.
                //
                // AND A THROW MEANS KEEP GOING, not cancel. A handler that
                // faults is a broken OBSERVER, and tearing down a multi-minute
                // separation because somebody's progress bar threw would be the
                // wrong blast radius — the managed wrapper logs it and the run
                // completes, exactly as it does for the v35 callback.
                try { return progressCb(progressUser, frac, stage ? stage : "") != 0; }
                catch (...) { return true; }
            };
        const int rc = dotcharter::stems::separate_file_cancellable(
            std::string(srcPath), std::string(outDir),
            std::string(modelPath), numThreads, cb);
        if (rc == 0)  return DCA_OK;
        if (rc == 1)  return DCA_ERR_INVALID_ARG;      // an empty path argument
        if (rc == 2)  return DCA_ERR_NOT_INITIALIZED;  // model file missing
        if (rc == 3)  return DCA_ERR_DECODE;           // model load failed
        if (rc == 4)  return DCA_ERR_DECODE;           // audio decode failed
        if (rc == 5)  return DCA_ERR_CANCELLED;        // the caller said stop
        return DCA_ERR_INTERNAL;
    });
}

} // extern "C"

