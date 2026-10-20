// NamAmpModule.cpp — see NamAmpModule.h. This is the ONLY TU that includes
// NeuralAudio headers (its nlohmann-json include dir is scoped to this file
// in CMakeLists.txt, mirroring the demucs Eigen scoping).
#include "NamAmpModule.h"

#include "Retirement.h"
#include "Scrub.h"
#include "cores/dcdsp_ampface.h"
#include "cores/dcdsp_detail_src.h"

#include "NeuralAudio/NeuralModel.h"

#include <cstdio>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dcrig
{
#if DCA_NAM_TEST_HOOKS
// Private recompiled-test synchronization; absent from the shipping binary.
void (*namLoadTestHook)(int stage) = nullptr;
// How many further blocks should have one non-finite sample injected straight
// out of the model, i.e. before the scrub that guards AmpFace's five recursive
// biquads. Set by juce_sharp_nam_lifetime; absent from the shipping binary.
int namNonFiniteTestBlocks = 0;
#endif

// ---- log sink (W4-D38) -----------------------------------------------------
// See NamAmpModule.h for why this is a sink of its own and not DspChain's:
// juce_sharp_nam_lifetime links two TUs and DspChain.h reaches moduleCatalogue()
// in a third. std::atomic because the WORKER thread emits and the message
// thread installs; relaxed is enough — a warning landing one line late is
// immaterial, and the pointer is written once at static-init time.
std::atomic<NamLogSink> g_namLogSink{ nullptr };
std::atomic<unsigned long> g_namOverflowWarnings{ 0 };

void setNamLogSink(NamLogSink sink) noexcept
{
    g_namLogSink.store(sink, std::memory_order_relaxed);
}

unsigned long namOverflowWarningsForTest() noexcept
{
    return g_namOverflowWarnings.load(std::memory_order_relaxed);
}

namespace
{

// ONCE PER EXCURSION, and the de-duplication lives HERE rather than in the
// queue (W4-D38).
//
// RetirementQueue::takeOverflowWarning()'s own doc says "edge-triggered, so a
// caller can log once per excursion instead of once per retire" — and its
// retire() sets the flag on EVERY retire while size > kSoftCap, so taking it
// per retire logs per retire. Measured, by the test that was written expecting
// the doc to be true: it fired again one retire into a single excursion. The
// queue is not this module's to change, and the layering is better this way
// anyway — the queue reports a STATE ("you are over the cap") and the logger
// decides how often to speak about it.
//
// `wasOver` is the caller's per-module latch: emit on the rising edge, and
// re-arm only once the queue has actually drained back to the cap. The counter
// moves even when no sink is installed — the native test executables install
// none, and the property worth asserting there is "the warning FIRED once", not
// "a string was formatted".
void reportRetirementOverflowIfAny(int pending, bool overflowed, bool& wasOver)
{
    if (pending <= dcdsp::RetirementQueue<int>::kSoftCap) wasOver = false;  // excursion over
    if (! overflowed || wasOver) return;
    wasOver = true;
    g_namOverflowWarnings.fetch_add(1, std::memory_order_relaxed);

    auto sink = g_namLogSink.load(std::memory_order_relaxed);
    if (sink == nullptr) return;

    // Fixed buffer, no std::string: this runs on the worker thread, which is
    // also the sweeper, and the surrounding code is careful about allocation.
    char msg[224];
    std::snprintf(msg, sizeof msg,
                  "namamp: %d retired captures are parked awaiting reclaim (soft cap %d). "
                  "The retirement queue is unbounded by design — its only sweeper is the "
                  "worker, which can sit inside a cold model parse for seconds — so this is "
                  "a signal, not a cap: something is arrowing through models faster than "
                  "they can be reclaimed.",
                  pending, dcdsp::RetirementQueue<int>::kSoftCap);
    sink(kNamLogWarn, msg);
}

#if DCA_NAM_TEST_HOOKS
} // namespace (reopened below)

// Test seam for the once-per-excursion latch. The real caller is
// NamAmpModule::retire, which needs a loaded capture and a worker thread to
// reach; the LATCH is the thing worth asserting and it is pure logic, so
// juce_sharp_nam_lifetime drives it directly. Absent from the shipping binary.
void namReportRetirementOverflowForTest(int pending, bool overflowed, bool& wasOver)
{
    reportRetirementOverflowIfAny(pending, overflowed, wasOver);
}

namespace
{
#endif


constexpr float kTrimMinDb = -24.0f;
constexpr float kTrimMaxDb =  24.0f;

// Host frames of silence pre-loaded into the rate-converted output FIFO. The
// two converters each carry a bounded sub-sample residue, so a block can
// legitimately yield n-1 or n+1 host frames; without a cushion the FIFO would
// sit at zero and pad with silence every few blocks, which is a click train.
// 32 frames is ~0.7 ms at 44.1 kHz and swamps a residue of ~3.
constexpr int kSrcPrimeFrames = 32;

// The capture rates the module will convert from. A .nam / .aidax names its own
// rate, and a shared capture is untrusted input: a huge finite rate used to
// overflow the frame arithmetic below and size buffers from the overflow.
constexpr double kMinModelRate = 8000.0;
constexpr double kMaxModelRate = 384000.0;
// Upper bound on one block's frames at the MODEL rate. With the rates above and
// any block the engine negotiates this is never reached; past it the capture
// dry-bypasses instead of allocating.
constexpr int kMaxModelFrames = 1 << 20;

// How often the worker wakes to collect retired models while any are parked.
// It only has to outlast one audio block, so this is generous.
constexpr int kSweepIntervalMs = 4;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float dbToLin(float db) { return db == 0.0f ? 1.0f : std::pow(10.0f, db / 20.0f); }

// One loaded neural capture, together with everything needed to run it at the
// engine rate that was current when it loaded. Published to the audio thread
// via an atomic shared_ptr; built on the worker thread and destroyed on the
// worker thread (via RetirementQueue — see setStringParam), so the audio thread
// never allocates, locks or frees here.
struct LoadedModel
{
    std::unique_ptr<NeuralAudio::NeuralModel> model;
    float outputAdjustDb = 0.0f; // model's recommended output make-up
    float sampleRate     = 48000.0f;

    // The engine configuration this instance was built for. process() compares
    // both against the live values and bypasses if they moved: prepare() kicks
    // off a reload, and until it lands the converters would be running the
    // wrong ratio (i.e. the wrong pitch) and the model the wrong block size.
    double hostRate  = 48000.0;
    int    hostBlock = 512;

    // Rate adaptation. Empty (and untouched) when the engine already runs at
    // the capture's own rate — that path stays bit-identical to the pre-0.36
    // signal chain.
    bool                        resample = false;
    dcdsp::detail::SincResampler toModel;   // engine rate -> capture rate
    dcdsp::detail::SincResampler toHost;    // capture rate -> engine rate
    std::vector<float>          modelScratch;
    std::vector<float>          outFifo;    // engine-rate, holds `outCount` frames
    int                         modelCap = 0;
    int                         outCount = 0;
    int                         latency  = 0; // engine-rate frames
};

class NamAmpModule : public DspModule
{
public:
    NamAmpModule() = default;

    ~NamAmpModule() override
    {
        shutdownWorker();
        // The worker is gone, so nothing else will sweep. The chain's remove
        // barrier SHOULD mean no audio block still references this module, so
        // this sweep normally clears the queue — but that is a safety claim
        // about another class with no assertion behind it, and it is exactly the
        // claim that must not be load-bearing. Anything the sweep cannot claim
        // is LEAKED by ~RetirementQueue (and counted), never dropped: a leak
        // beats handing a NeuralAudio destructor to the audio thread.
        retired_.sweep();
    }

    // ---- Message thread -----------------------------------------------------

    void prepare(double sampleRate, int maxBlockSize, int numChannels) override
    {
        (void) numChannels;
        sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_   = (maxBlockSize > 0) ? maxBlockSize : 512;
        liveRate_.store(sampleRate_, std::memory_order_relaxed);
        liveBlock_.store(maxBlock_, std::memory_order_relaxed);
        trimPrimed_ = false; // start AT the trims, not ramping to them
        inScratch_.assign((size_t) maxBlock_, 0.0f);
        outScratch_.assign((size_t) maxBlock_, 0.0f);
        face_.prepare(sampleRate_, maxBlock_, 1);
        // The model's internal buffers AND both rate converters are sized for
        // the engine rate / block size that were current at load time — reload
        // for the new ones.
        std::string path;
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            path = modelPath_;
        }
        if (!path.empty())
        {
            // P2-14 (2026-08-29): between this prepare and the worker's publish,
            // process() refuses the published snapshot (its hostRate/hostBlock
            // no longer match liveRate_/liveBlock_) and passes DRY — while
            // "loaded" still read 1 and "rateMismatch" read 0, so the UI showed
            // a working amp that was silently a wire. The renegotiation window
            // is reported through the same poll the UI already honours.
            if (loaded_.load(std::memory_order_acquire))
                reloading_.store(true, std::memory_order_relaxed);
            requestLoad(path);
        }
        prepared_ = true;
    }

    void reset() override
    {
        // NeuralAudio exposes no state-reset; the capture's internal state
        // decays naturally. The face's biquads we can and do clear.
        face_.reset();
    }

    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "inputDb"))  { inputDb_.store(clampf(v, kTrimMinDb, kTrimMaxDb), std::memory_order_relaxed); return true; }
        if (!std::strcmp(id, "outputDb")) { outputDb_.store(clampf(v, kTrimMinDb, kTrimMaxDb), std::memory_order_relaxed); return true; }

        // The amp face (0.36). Multi-word biquad coefficients, so it goes
        // through a ParamMailbox — never core_.setParams from this thread.
        if      (!std::strcmp(id, "gain"))      face_p_.gain      = clampf(v, 0.0f, 1.0f);
        else if (!std::strcmp(id, "bass"))      face_p_.bass      = clampf(v, 0.0f, 1.0f);
        else if (!std::strcmp(id, "mid"))       face_p_.mid       = clampf(v, 0.0f, 1.0f);
        else if (!std::strcmp(id, "treble"))    face_p_.treble    = clampf(v, 0.0f, 1.0f);
        else if (!std::strcmp(id, "presence"))  face_p_.presence  = clampf(v, 0.0f, 1.0f);
        else if (!std::strcmp(id, "resonance")) face_p_.resonance = clampf(v, 0.0f, 1.0f);
        else if (!std::strcmp(id, "master"))    face_p_.master    = clampf(v, 0.0f, 1.0f);
        else return false;
        faceMail_.publish(face_p_);
        return true;
    }

    // The three status ids below answer getParam and NOT setParam, which is
    // exactly why this override exists: DspChain::hasParam is the automated
    // render's pre-pass, and built on getParam it accepted an event naming
    // "loaded" that applyAutomationEvent then rejected MID-RENDER.
    bool hasSettableParam(const char* id) const override
    {
        if (id == nullptr) return false;
        return !std::strcmp(id, "inputDb")   || !std::strcmp(id, "outputDb")
            || !std::strcmp(id, "gain")      || !std::strcmp(id, "bass")
            || !std::strcmp(id, "mid")       || !std::strcmp(id, "treble")
            || !std::strcmp(id, "presence")  || !std::strcmp(id, "resonance")
            || !std::strcmp(id, "master");
    }

    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "inputDb"))  { out = inputDb_.load(std::memory_order_relaxed); return true; }
        if (!std::strcmp(id, "outputDb")) { out = outputDb_.load(std::memory_order_relaxed); return true; }

        if (!std::strcmp(id, "gain"))      { out = face_p_.gain;      return true; }
        if (!std::strcmp(id, "bass"))      { out = face_p_.bass;      return true; }
        if (!std::strcmp(id, "mid"))       { out = face_p_.mid;       return true; }
        if (!std::strcmp(id, "treble"))    { out = face_p_.treble;    return true; }
        if (!std::strcmp(id, "presence"))  { out = face_p_.presence;  return true; }
        if (!std::strcmp(id, "resonance")) { out = face_p_.resonance; return true; }
        if (!std::strcmp(id, "master"))    { out = face_p_.master;    return true; }

        // Poll-only status (deliberately NOT in the catalogue — no knobs):
        // "loaded"       1 when a capture is active, else 0
        // "rateMismatch" 1 when the engine rate can't drive the capture at all
        //                (module bypasses). Since 0.36 the module resamples
        //                around the capture, so this only fires if the
        //                converters could not be built (a capture claiming a
        //                nonsense rate) or while a device rate change is still
        //                in flight — it is no longer the normal 44.1 kHz case.
        // "resampling"   1 when the rate converters are in the path (poll-only,
        //                diagnostic: it is what a managed test asserts on)
        // Deliberately a plain atomic<bool> and NOT atomic_load(&active_): on the
        // MSVC STL the shared_ptr atomics take a PROCESS-WIDE CRT spin lock
        // (_Shared_ptr_spin_lock), and the audio thread takes that same lock in
        // process() below. This getter is polled by the managed UI, so reading
        // the snapshot here put a UI-rate contender on a lock the callback needs.
        if (!std::strcmp(id, "loaded"))       { out = loaded_.load(std::memory_order_acquire) ? 1.0f : 0.0f; return true; }
        // "rateMismatch" is ALSO 1 while a device-rate / block-size
        // renegotiation is in flight (reloading_): the doc above promised that
        // and the flag never did it until P2-14.
        if (!std::strcmp(id, "rateMismatch"))
        {
            out = (rateMismatch_.load(std::memory_order_relaxed) || reloading_.load(std::memory_order_relaxed)) ? 1.0f : 0.0f;
            return true;
        }
        if (!std::strcmp(id, "resampling"))   { out = resampling_.load(std::memory_order_relaxed) ? 1.0f : 0.0f; return true; }
        // "retiredPending" — captures parked for the worker to reclaim. The
        // retirement queue is deliberately UNBOUNDED (its only sweeper is the
        // worker, which can sit inside a cold NeuralModelLoader parse for
        // seconds, and blocking the retiring thread instead would be worse), so
        // a user arrowing through a model list parks one whole capture plus its
        // FIFOs per selection with nothing reclaiming them until that parse
        // returns. Unbounded with no way to SEE it is the part that was wrong;
        // past RetirementQueue::kSoftCap the queue also raises an edge-triggered
        // overflow flag.
        if (!std::strcmp(id, "retiredPending")) { out = (float) retired_.pending(); return true; }
        return false;
    }

    bool setStringParam(const char* id, const char* utf8Value) override
    {
        if (std::strcmp(id, "modelPath") != 0) return false;
        const std::string path = (utf8Value != nullptr) ? utf8Value : "";
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            modelPath_ = path;
        }
        if (path.empty())
        {
            std::shared_ptr<LoadedModel> old;
            {
                std::lock_guard<std::mutex> lk(workMutex_);
                ++pendingGen_;
                workRequested_ = false;
                std::shared_ptr<LoadedModel> none;
                old = std::atomic_exchange(&active_, none);
                loaded_.store(false, std::memory_order_release);
                resampling_.store(false, std::memory_order_relaxed);
                rateMismatch_.store(false, std::memory_order_relaxed);
                reloading_.store(false, std::memory_order_relaxed);
                latency_.store(0, std::memory_order_relaxed);
            }
            // Park it for the worker. Clearing a model used to block the
            // MESSAGE thread here for up to 500 ms.
            retire(std::move(old));
        }
        else
        {
            requestLoad(path);
        }
        return true;
    }

    bool getStringParam(const char* id, std::string& out) const override
    {
        if (std::strcmp(id, "modelPath") != 0) return false;
        std::lock_guard<std::mutex> lk(stateMutex_);
        out = modelPath_;
        return true;
    }

    // ---- Audio thread -------------------------------------------------------

    void process(float* const* channels, int numChannels, int numFrames) override
    {
        // RT contract: atomic snapshot load (same pattern as DspChain), no
        // alloc / lock / throw. Bypass (dry passthrough) when no model is
        // loaded, the snapshot was built for a different engine configuration
        // (a reload is already in flight), or the block is oversized.
        auto snap = std::atomic_load_explicit(&active_, std::memory_order_acquire);
        if (snap == nullptr || rateMismatch_.load(std::memory_order_relaxed)) return;
        if (numChannels <= 0 || numFrames <= 0 || numFrames > maxBlock_) return;
        if (snap->hostRate  != liveRate_.load(std::memory_order_relaxed)
            || snap->hostBlock != liveBlock_.load(std::memory_order_relaxed)) return;
        float* x0 = channels[0];
        if (x0 == nullptr) return;

        // Params reach the face here, never from setParam (audit #4).
        faceMail_.applyPending(face_);

        // Both trims RAMP across the block from where the previous block
        // ended (AudioEngine::rampOrScale's rule); the face's own pre-gain
        // walks its SmoothedValue per sample. One constant per block was a
        // zipper on every slider tick of the two knobs a player touches most.
        const float inTarget  = dbToLin(inputDb_.load(std::memory_order_relaxed));
        const float outTarget = dbToLin(outputDb_.load(std::memory_order_relaxed) + snap->outputAdjustDb);
        if (! trimPrimed_) { lastInG_ = inTarget; lastOutG_ = outTarget; trimPrimed_ = true; }
        const float inInc  = (inTarget  - lastInG_)  / (float) numFrames;
        const float outInc = (outTarget - lastOutG_) / (float) numFrames;

        float inG = lastInG_;
        for (int i = 0; i < numFrames; ++i)
        {
            inG += inInc;
            const float v = x0[i];
            inScratch_[(size_t) i] = std::isfinite(v) ? v * inG * face_.nextPreGain() : 0.0f;
        }
        lastInG_ = inTarget;

        if (!snap->resample)
        {
            snap->model->Process(inScratch_.data(), outScratch_.data(), (size_t) numFrames);
        }
        else
        {
            runResampled(*snap, numFrames);
        }

#if DCA_NAM_TEST_HOOKS
        // Stands in for "a bad .nam / an RTNeural LSTM excursion emitted a
        // non-finite sample" — the fault the scrub below exists for, which
        // cannot otherwise be provoked from a real capture.
        if (namNonFiniteTestBlocks > 0 && numFrames > 0)
        {
            --namNonFiniteTestBlocks;
            outScratch_[0] = std::numeric_limits<float>::quiet_NaN();
        }
#endif
        // THE NEURAL NET IS AN UNTRUSTED STAGE by this engine's own definition,
        // and AmpFace::processPost below is FIVE recursive Biquads
        // (bass/mid/treble/resonance/presence). A non-finite sample out of
        // NeuralAudio latched all five permanently — and AmpFace::reset() is
        // only reached from NamAmpModule::reset(), which by this repo's rule has
        // exactly one caller (an un-bypass off a fully parked fade) — so the amp
        // went silent for the session with nothing saying why. The module's
        // INPUT was already scrubbed (above) and the chain scrubs after the
        // module; the gap was precisely between the model and the face. The
        // final clamp stays: it bounds magnitude, which a scrub says nothing
        // about.
        {
            float* out = outScratch_.data();
            dcdsp::scrubNonFinite(&out, 1, numFrames);
        }

        // Post half of the face runs at the ENGINE rate, so its response is
        // identical whether or not the converters are in the path.
        if (outTarget != 1.0f || lastOutG_ != outTarget)
        {
            float outG = lastOutG_;
            for (int i = 0; i < numFrames; ++i) { outG += outInc; outScratch_[(size_t) i] *= outG; }
        }
        lastOutG_ = outTarget;
        face_.processPost(outScratch_.data(), numFrames);

        for (int c = 0; c < numChannels; ++c)
        {
            float* x = channels[c];
            if (x == nullptr) continue;
            for (int i = 0; i < numFrames; ++i)
            {
                float y = outScratch_[(size_t) i];
                if (!std::isfinite(y)) y = 0.0f;
                else if (y >  16.0f) y =  16.0f;
                else if (y < -16.0f) y = -16.0f;
                x[i] = y;
            }
        }
    }

    int latencySamples() const override { return latency_.load(std::memory_order_relaxed); }

    const char* typeId() const override { return "namamp"; }

private:
    // ---- Audio thread: rate-converted capture path --------------------------

    /// inScratch_[0..n) at the engine rate -> outScratch_[0..n) at the engine
    /// rate, with the capture running at its own native rate in between.
    /// Alloc-free: every buffer was sized on the worker thread at load.
    void runResampled(LoadedModel& lm, int n)
    {
        lm.toModel.push(inScratch_.data(), n);
        int k = lm.toModel.available();
        if (k > lm.modelCap) k = lm.modelCap;
        if (k > 0)
        {
            lm.toModel.pull(lm.modelScratch.data(), k);
            lm.model->Process(lm.modelScratch.data(), lm.modelScratch.data(), (size_t) k);
            lm.toHost.push(lm.modelScratch.data(), k);
        }

        int room = (int) lm.outFifo.size() - lm.outCount;
        int h    = lm.toHost.available();
        if (h > room) h = room;
        if (h > 0)
        {
            lm.toHost.pull(lm.outFifo.data() + lm.outCount, h);
            lm.outCount += h;
        }

        if (lm.outCount >= n)
        {
            std::memcpy(outScratch_.data(), lm.outFifo.data(), sizeof(float) * (size_t) n);
            const int rest = lm.outCount - n;
            if (rest > 0)
                std::memmove(lm.outFifo.data(), lm.outFifo.data() + n, sizeof(float) * (size_t) rest);
            lm.outCount = rest;
        }
        else
        {
            // Only reachable while the pipeline primes (or after an xrun long
            // enough to drain the cushion). Emit what exists and pad.
            const int have = lm.outCount;
            if (have > 0) std::memcpy(outScratch_.data(), lm.outFifo.data(), sizeof(float) * (size_t) have);
            std::memset(outScratch_.data() + have, 0, sizeof(float) * (size_t) (n - have));
            lm.outCount = 0;
        }
    }

    // ---- Worker thread (model load / parse / prewarm) -----------------------

    void requestLoad(std::string path)
    {
        {
            std::lock_guard<std::mutex> lk(workMutex_);
            pendingPath_  = std::move(path);
            pendingBlock_ = maxBlock_;
            pendingRate_  = sampleRate_;
            ++pendingGen_;
            workRequested_ = true;
            ensureWorkerLocked();
        }
        workCv_.notify_all();
    }

    void workerLoop()
    {
        for (;;)
        {
            std::string path;
            int         block = 512;
            double      host  = 48000.0;
            uint64_t    gen = 0;
            {
                std::unique_lock<std::mutex> lk(workMutex_);
                // While anything is parked for retirement, wake periodically to
                // sweep it: the audio thread may still have been mid-block when
                // it was parked, and nobody else is allowed to free it.
                if (retired_.pending() > 0)
                    workCv_.wait_for(lk, std::chrono::milliseconds(kSweepIntervalMs),
                                     [this] { return workRequested_ || shutdown_; });
                else
                    workCv_.wait(lk, [this] { return workRequested_ || shutdown_; });
                if (shutdown_) return;
                if (!workRequested_)
                {
                    lk.unlock();
                    retired_.sweep();
                    continue;
                }
                workRequested_ = false;
                path  = pendingPath_;
                block = pendingBlock_;
                host  = pendingRate_;
                gen   = pendingGen_;
            }
            retired_.sweep();

            auto holder = std::make_shared<LoadedModel>();
            holder->hostRate  = host;
            holder->hostBlock = block;
            try
            {
                NeuralAudio::NeuralModelLoader loader;
                // The capture always runs at its OWN rate; we convert around it
                // rather than telling NeuralAudio about the engine rate.
                // SetExternalSampleRate only rescales WaveNet dilations when
                // the engine rate is an exact INTEGER multiple of the model
                // rate, and it does nothing at all for the RTNeural (.aidax)
                // path — so it would leave exactly the users this work exists
                // for (44.1 / 88.2 / 176.4 kHz) unserved while adding a second,
                // format-dependent code path.
                loader.SetDefaultMaxAudioBufferSize(maxModelFrames(block, host, 48000.0));
                NeuralAudio::NeuralModel* m =
                    loader.CreateFromFile(std::filesystem::u8path(path), /*doPrewarm*/ true);
                if (m != nullptr)
                {
                    holder->model.reset(m);
                    holder->outputAdjustDb = m->GetRecommendedOutputDBAdjustment();
                    holder->sampleRate     = m->GetSampleRate();
                }
            }
            catch (...)
            {
                holder->model.reset();
            }

#if DCA_NAM_TEST_HOOKS
            struct CompletedAttempt
            {
                ~CompletedAttempt() { if (namLoadTestHook) namLoadTestHook(2); }
            } completedAttempt;
#endif
            {
                // A newer request superseded this load — discard it.
                std::lock_guard<std::mutex> lk(workMutex_);
                if (gen != pendingGen_ || shutdown_) continue;
            }

            if (holder->model != nullptr)
            {
                // Everything from here to the publish runs on this std::thread,
                // where an escaping exception is std::terminate for the whole
                // host. Arming allocates sizes that come from the capture file,
                // so a failure there is a capture that dry-bypasses with
                // "rateMismatch", never a crash.
                bool ok = false;
                try
                {
                    ok = armRateAdaptation(*holder);
                    if (ok)
                    {
                        // The loader was told a default buffer size derived from an
                        // assumed 48 kHz capture; re-size for what the capture
                        // actually reported before it ever sees a block.
                        holder->model->SetMaxAudioBufferSize(
                            holder->resample ? holder->modelCap : holder->hostBlock);
                    }
                }
                catch (...)
                {
                    ok = false;
                    holder->resample = false;
                    holder->latency  = 0;
                    std::vector<float>().swap(holder->modelScratch);
                    std::vector<float>().swap(holder->outFifo);
                }
#if DCA_NAM_TEST_HOOKS
                if (namLoadTestHook) namLoadTestHook(1);
#endif
                std::shared_ptr<LoadedModel> old;
                {
                    // The request can change during rate adaptation/preparation.
                    // Check and publish in the same transaction as clear/requestLoad.
                    std::lock_guard<std::mutex> lk(workMutex_);
                    if (gen != pendingGen_ || shutdown_) continue;
                    rateMismatch_.store(!ok, std::memory_order_relaxed);
                    resampling_.store(ok && holder->resample, std::memory_order_relaxed);
                    latency_.store(ok ? holder->latency : 0, std::memory_order_relaxed);
                    old = std::atomic_exchange(&active_, std::shared_ptr<LoadedModel>(holder));
                    loaded_.store(true, std::memory_order_release);
                    reloading_.store(false, std::memory_order_relaxed); // the new snapshot matches the live rate/block
                }
                retired_.retire(std::move(old));   // swept below, on this thread
            }
            // On failure the previous model (if any) stays active; the
            // managed side detects it by comparing modelPath vs "loaded".
            retired_.sweep();
        }
    }

    /// Worker thread. Size and build the converter pair for `lm`. Returns false
    /// only when the capture reports a rate we cannot convert from, which is
    /// the one remaining case that still dry-bypasses.
    static bool armRateAdaptation(LoadedModel& lm)
    {
        const double model = (double) lm.sampleRate;
        if (!std::isfinite(model) || model < kMinModelRate || model > kMaxModelRate) return false;

        if (std::fabs(lm.hostRate - model) <= 1.0)
        {
            lm.resample = false;
            lm.modelCap = lm.hostBlock;
            lm.latency  = 0;
            return true;
        }

        if (modelFramesFor(lm.hostBlock, lm.hostRate, model) > (double) kMaxModelFrames) return false;
        lm.modelCap = maxModelFrames(lm.hostBlock, lm.hostRate, model);
        lm.toModel.prepare(lm.hostRate, model, lm.hostBlock);
        lm.toHost.prepare(model, lm.hostRate, lm.modelCap);
        lm.modelScratch.assign((size_t) lm.modelCap, 0.0f);
        // Room for the priming cushion plus a couple of blocks of jitter.
        lm.outFifo.assign((size_t) (kSrcPrimeFrames + 4 * lm.hostBlock + 64), 0.0f);
        lm.outCount = kSrcPrimeFrames;   // the cushion IS the reported latency
        lm.resample = true;
        lm.latency  = kSrcPrimeFrames
                      + lm.toModel.latencyInSamples()
                      + (int) std::lround((double) lm.toHost.latencyInSamples() * lm.hostRate / model);
        return true;
    }

    /// One host block's frames at the model rate, in double so no rate can
    /// overflow it.
    static double modelFramesFor(int hostBlock, double hostRate, double modelRate)
    {
        if (!(hostRate > 0.0) || !(modelRate > 0.0) || !std::isfinite(hostRate) || !std::isfinite(modelRate))
            return (double) hostBlock;
        return std::ceil((double) hostBlock * (modelRate / hostRate)) + 32.0;
    }

    /// modelFramesFor, clamped to [1, kMaxModelFrames] before the cast.
    static int maxModelFrames(int hostBlock, double hostRate, double modelRate)
    {
        const double frames = modelFramesFor(hostBlock, hostRate, modelRate);
        if (!(frames >= 1.0)) return 1;
        return frames >= (double) kMaxModelFrames ? kMaxModelFrames : (int) frames;
    }

    void shutdownWorker()
    {
        {
            std::lock_guard<std::mutex> lk(workMutex_);
            shutdown_ = true;
        }
        workCv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // Park a retired model and make sure a thread that MAY free will come and
    // collect it. This replaced `drainRelease`, which spun up to 500 ms on
    // use_count() > 1 and then `reset()` anyway: past the bound the audio
    // thread's copy became the last owner, so a slow block handed the callback
    // a ~free() plus a whole NeuralAudio model destructor — exactly when the
    // callback could least afford it. Nothing is ever destroyed here.
    void retire(std::shared_ptr<LoadedModel>&& old)
    {
        if (old == nullptr) return;
        retired_.retire(std::move(old));
        // W4-D38: the queue has raised this flag since it existed and NOTHING
        // read it, so "unbounded with no way to SEE it" — the fault the soft cap
        // was added for — was still true. Read here rather than in the sweeper:
        // this is the message/worker thread, the excursion is caused by a
        // retire, and the sweeper may not run for seconds.
        // takeOverflowWarning() is CLEARED unconditionally, even when the latch
        // suppresses the message: leaving it set would make the next excursion's
        // rising edge indistinguishable from this one's tail.
        reportRetirementOverflowIfAny(retired_.pending(), retired_.takeOverflowWarning(),
                                      retirementOverflowReported_);
        {
            std::lock_guard<std::mutex> lk(workMutex_);
            ensureWorkerLocked();
        }
        workCv_.notify_all();
    }

    /// Call with workMutex_ held. The worker is also the sweeper, so a module
    /// that only ever CLEARED a capture still needs one.
    void ensureWorkerLocked()
    {
        if (!worker_.joinable() && !shutdown_)
            worker_ = std::thread([this] { workerLoop(); });
    }

    // Audio-thread state
    std::shared_ptr<LoadedModel> active_;                 // atomic_* access only
    std::vector<float>           inScratch_, outScratch_; // sized in prepare()
    std::atomic<float>           inputDb_{ 0.0f };
    std::atomic<float>           outputDb_{ 0.0f };
    // Audio-thread-only: where the last block's trim ramps ended.
    float lastInG_  = 1.0f;
    float lastOutG_ = 1.0f;
    bool  trimPrimed_ = false;
    // Mirrors "is active_ non-null", so the UI's `loaded` poll never has to
    // touch the shared_ptr atomics (see getParam).
    std::atomic<bool>            loaded_{ false };
    std::atomic<bool>            rateMismatch_{ false };
    // 1 from a prepare() that invalidated a published snapshot until the
    // worker publishes its replacement (or the model is cleared): the window
    // in which process() passes dry. Reported through "rateMismatch".
    std::atomic<bool>            reloading_{ false };
    std::atomic<bool>            resampling_{ false };
    std::atomic<int>             latency_{ 0 };
    int                          maxBlock_ = 512;

    dcdsp::AmpFace                       face_;
    ParamMailbox<dcdsp::AmpFaceParams>   faceMail_;

    // Message-thread state
    mutable std::mutex    stateMutex_;
    std::string           modelPath_;
    double                sampleRate_ = 48000.0;
    bool                  prepared_ = false;
    dcdsp::AmpFaceParams  face_p_{};   // getParam read-back mirror
    std::atomic<double>   liveRate_{ 48000.0 };
    std::atomic<int>      liveBlock_{ 512 };

    // Retired models awaiting a thread that may free them (never the callback).
    dcdsp::RetirementQueue<LoadedModel> retired_;
    /// Worker/message thread only: are we INSIDE an excursion past the queue's
    /// soft cap? See reportRetirementOverflowIfAny — the queue re-raises its
    /// flag per retire, so the once-per-excursion promise is kept here.
    bool                                retirementOverflowReported_ = false;

    // Worker
    std::mutex              workMutex_;
    std::condition_variable workCv_;
    std::thread             worker_;
    std::string             pendingPath_;
    int                     pendingBlock_ = 512;
    double                  pendingRate_  = 48000.0;
    uint64_t                pendingGen_ = 0;
    bool                    workRequested_ = false;
    bool                    shutdown_ = false;
};

} // namespace

ModuleTypeInfo namAmpTypeInfo()
{
    // The amp face (0.36). Every knob's DEFAULT is its neutral value and the
    // module skips the corresponding filter/gain entirely there, so a preset
    // that omits them — every preset authored before 0.36 — is bit-identical to
    // the pre-face signal path. Ranges mirror `DcRigTargetCatalog` on the
    // managed side; change one, change the other.
    return ModuleTypeInfo{
        "namamp", "Neural Amp (NAM)",
        { { "gain",      "Gain",        0.0f,       1.0f,      0.5f },
          { "bass",      "Bass",        0.0f,       1.0f,      0.5f },
          { "mid",       "Mid",         0.0f,       1.0f,      0.5f },
          { "treble",    "Treble",      0.0f,       1.0f,      0.5f },
          { "presence",  "Presence",    0.0f,       1.0f,      0.5f },
          { "resonance", "Resonance",   0.0f,       1.0f,      0.5f },
          { "master",    "Master",      0.0f,       1.0f,      0.5f },
          { "inputDb",   "Input (dB)",  kTrimMinDb, kTrimMaxDb, 0.0f },
          { "outputDb",  "Output (dB)", kTrimMinDb, kTrimMaxDb, 0.0f } },
        { { "modelPath", "NAM Model", "*.nam;*.aidax" } }
    };
}

std::unique_ptr<DspModule> createNamAmpModule()
{
    return std::make_unique<NamAmpModule>();
}

} // namespace dcrig
