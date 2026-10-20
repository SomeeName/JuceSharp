// dcdsp_cab.h — cabinet simulator: zero-latency minimum-phase FIR convolution.
// Each model is a bundled 2048-tap (42.7 ms @ 48 kHz) synthetic close-mic IR
// (dcdsp_cab_irs.h documents the authoring recipe): the old parametric macro
// curves survive as the magnitude envelope, with modal cone-breakup ripple and
// short edge/depth reflection combs layered on top — the dense notch structure
// a biquad cascade cannot produce. Minimum-phase reconstruction keeps all
// energy at the IR front, so latencySamples() stays 0. User low-cut/high-cut
// biquads and the dry/wet mix are unchanged and run post-convolution at base
// rate. Pure dcdsp core, RT-safe (alloc only in prepare()).
//
// 0.35 LOW-END FIX — THE BANK HAD NO SPEAKER RESONANCE. The generator recipe
// is explicit about what it contains ("a box LF rolloff, a steeper cone-breakup
// HF rolloff, log-Gaussian ripple, reflection combs") and a driver's Fs
// resonance is not on that list. Measured on model 0 (sine-probe, dB re its own
// 200 Hz-4 kHz log-mean) the IR decays MONOTONICALLY below 200 Hz: -0.4 @ 200,
// -1.8 @ 125, -3.2 @ 100, -4.9 @ 82, -8.2 @ 63. A real close-miked guitar cab
// does the opposite in that octave — the driver's Fs (75-110 Hz for a 12",
// higher for small speakers) plus the box loading and the mic's proximity rise
// put a PEAK there, which is why everyone high-passes a real IR rather than
// boosting it. Without it, every amp+cab preset measured 8-18 dB down at the
// low E and the whole rig read as thin (owner report).
//
// `kResonance` restores exactly that one missing component: a per-model peaking
// section at the driver's Fs, in the wet path, ahead of the user low-cut. The
// IR TABLES are untouched (models 0-3 are still byte-identical, so a persisted
// `model` int still selects the same impulse response) — this is a modelled
// stage the recipe omitted, not a re-EQ of the bank. It is SKIPPED for a user
// IR: a real capture already contains its own driver resonance and doubling it
// would be a lie about someone else's cab.
//
// The catalogue default `lowCutHz` also dropped 80 -> 60 Hz. At 80 Hz the
// Butterworth took another 2.8 dB at the low E and 4.9 dB at C2 on top of the
// IR's own rolloff — double-counting a job the IR already does. The control
// still spans 40..200 Hz and still means what it says.
#pragma once

#ifdef DCDSP_TEST_STUBS
  #include "test_stubs.h"
#else
  #include "dcdsp_primitives.h"
#endif

#include "dcdsp_detail_fir.h"
#include "dcdsp_cab_irs.h"

#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct CabParams {
    int   model     = 0;      // 0..25 cab model (see dcdsp_cab_irs.h + the ABI
                              // v24 label table); 0-3 unchanged for tone stability
    float mix       = 1.0f;   // 0..1 dry/wet
    float lowCutHz  = 60.f;   // 40..200 (was 80 — see the header note)
    float highCutHz = 8000.f; // 3000..12000
};

class CabCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        (void)maxBlockSize; // per-sample convolution; no per-block scratch needed
        sr_ = sampleRate;
        int n = std::max(1, numChannels);
        ch_.resize(n);
        // kMaxTaps covers the full 42.7 ms IR up to 96 kHz; above that the
        // resampler truncates the (quiet) tail to the cap.
        irLin_.assign(kMaxTaps, 0.f);
        modelLin_.assign(kMaxTaps, 0.f);
        customIr_.reserve(kMaxTaps);
        for (auto& s : slots_) { s.rev.assign(kMaxTaps, 0.f); s.taps = 1; }
        activeSlot_.store(0, std::memory_order_release);
        for (auto& c : ch_) { c.fir.prepare(kMaxTaps); c.reset(); }

        // EVERY RATE CONVERSION IN THIS CORE HAPPENS HERE, ON THE OWNER THREAD.
        // prepare() is the one entry point that is allowed to spend 4,096 x 6
        // Lanczos taps (two std::sin apiece) — the audio thread never is, and it
        // used to, on every model switch and every user-IR adoption at any
        // device rate other than 48 kHz. Both stores below are held AT THE
        // ENGINE RATE from here on, so rebuildIr() is a reverse copy.
        model_ = clampModel(params_.model);
        if (bundledBank_) resampleModelIntoCache();
        else { modelTaps_ = 0; }
        // A user IR is USER CONTENT and survives a device change; only its rate
        // is re-derived. (customRate_ is whatever it was last converted to.)
        if (!customIr_.empty() && std::fabs(customRate_ - sr_) >= 1.0)
        {
            const int converted = detail::resampleIr(customIr_.data(), (int) customIr_.size(),
                                                     customRate_, irLin_.data(), kMaxTaps, sr_);
            customIr_.assign(irLin_.data(), irLin_.data() + std::max(converted, 0));
            customRate_ = sr_;
        }
        rebuildIr();

        // `mix` RAMPS (kMixRampSeconds) — it is the only way to dial a cab back,
        // so it gets dragged, and as a per-block constant it stepped a full
        // dry/wet crossfade at every block edge.
        mixSmooth_.reset(sr_, kMixRampSeconds);
        setParams(params_);
        mixSmooth_.snapToTarget();   // a freshly prepared cab starts AT its mix
    }

    void reset() { for (auto& c : ch_) c.reset(); }

    // A model switch is a RATE CONVERSION of a 2,048-tap table (up to 4,096
    // output taps, six Lanczos taps each, two std::sin per tap) and setParams
    // is drained from the audio CALLBACK by the adapter's ParamMailbox — so on
    // any device that is not at 48 kHz this ran the whole conversion inside one
    // block, on every model knob tick and every preset load. It allocates
    // nothing, which is exactly why the RT gate never saw it.
    //
    // In the shipping path the kernel now arrives ALREADY CONVERTED through
    // adoptModelKernel(), published by dcrig::ModelIrMailbox on the owner
    // thread and drained BEFORE this mailbox, so `model_` already agrees with
    // `p.model` and the branch below is dead. It is kept for DIRECT callers
    // (tools/tone_response.cpp, the native tone tests, offline tooling) which
    // are not on a callback and for which this is the whole API — silently
    // ignoring their model would be far worse than a conversion they can
    // afford. `conversionsForTest()` is how the RT gate proves the callback
    // does not take it.
    void setParams(const CabParams& p) {
        params_ = p;
        mixSmooth_.setTarget(clamp01(p.mix));
        float lowCut  = std::min(std::max(p.lowCutHz, 40.f), 200.f);
        float highCut = std::min(std::max(p.highCutHz, 3000.f), 12000.f);

        const int model = clampModel(p.model);
        if (model != model_ && bundledBank_ && modelLin_.size() == (size_t) kMaxTaps) {
            model_ = model;
            resampleModelIntoCache();
            if (customIr_.empty()) rebuildIr();
        }
        // Driver resonance — skipped for a user IR, which carries its own.
        const Resonance& r = kResonance[model];
        const float resDb = customIr_.empty() ? r.gainDb : 0.f;
        for (auto& c : ch_) {
            c.res.setPeaking(sr_, r.fcHz, r.q, resDb);
            c.lowCut.setHighpass(sr_, lowCut, 0.707);
            c.highCut.setLowpass(sr_, highCut, 0.707);
        }
    }

    // Replace the bundled IR bank with a USER impulse response (0.16 user-IR
    // loading, rides ABI v22). `samples` (count taps at `rate` Hz) are copied
    // and survive re-prepare; the active IR is rebuilt at the engine rate.
    // Processing-thread only after prepare(). Pass through clearCustomIr() to revert
    // to the bundled bank.
    //
    // Called only by the audio-thread IR mailbox (or while processing is
    // stopped). prepare() reserves all storage; adoption never allocates.
    //
    // `rate` MUST already be the engine rate when this is reached from the
    // audio thread: dcrig::IrMailbox converts the parsed WAV on the OWNER
    // thread precisely so this stays a copy. The conversion below is the
    // off-callback fallback for a direct caller (tests, offline tooling); it is
    // never taken from the mailbox, and `RtCabKernelSwitchDoesNotBurstOneCallback`
    // pins that. (Until 2026-09-11 this comment cited two test names that had
    // never existed; Wave4DspSourceLintTests now fails on a dead citation.)
    void setCustomIr(const float* samples, int count, double rate) {
        if (samples == nullptr || count < 1 || rate <= 0.0) return;
        if (std::fabs(rate - sr_) < 1.0 || irLin_.size() != (size_t) kMaxTaps) {
            customIr_.assign(samples, samples + std::min(count, kMaxTaps));
            customRate_ = rate;
        } else {
            const int n = detail::resampleIr(samples, std::min(count, kMaxTaps), rate,
                                             irLin_.data(), kMaxTaps, sr_);
            customIr_.assign(irLin_.data(), irLin_.data() + std::max(n, 0));
            customRate_ = sr_;
        }
        if (irLin_.size() == (size_t) kMaxTaps) rebuildIr();  // else prepare() will
        setParams(params_);   // re-evaluates the resonance bypass (see kResonance)
    }

    void clearCustomIr() {
        customIr_.clear();
        if (irLin_.size() == (size_t) kMaxTaps) rebuildIr();
        setParams(params_);
    }

    // Adopt a bundled model whose kernel has ALREADY been converted to the
    // engine rate by the publisher (dcrig::ModelIrMailbox, on the owner
    // thread). Audio-thread safe by construction: a bounded copy, a reverse
    // copy and one release store, with no transcendental and no allocation.
    //
    // A user IR still wins — it overrides the bank — so this updates the cached
    // model kernel and the resonance row, and only republishes the live kernel
    // when no custom IR is loaded.
    void adoptModelKernel(const float* linearTaps, int taps, int model) {
        model_ = clampModel(model);
        if (linearTaps != nullptr && taps > 0 && modelLin_.size() == (size_t) kMaxTaps) {
            modelTaps_ = std::min(taps, kMaxTaps);
            std::copy(linearTaps, linearTaps + modelTaps_, modelLin_.begin());
            if (customIr_.empty()) rebuildIr();
        }
        // setParams() would otherwise see the OLD model in params_ and convert
        // it back — on this very call, which is the audio thread.
        params_.model = model_;
        setParams(params_);   // re-evaluates the resonance row for the new model
    }

    /// The bundled model this core is currently voiced for.
    int model() const { return model_; }

    /// Turn the bundled cabinet bank OFF for this instance. DualCabCore's
    /// second slot is active ONLY when a real user IR is loaded into it, so
    /// converting a bundled model for it was pure waste — and because
    /// DualCabCore::setParams forwards one CabParams (including `model`) to
    /// both slots, slot B re-converted a 2,048-tap table on every model switch
    /// of slot A, for a kernel nothing would ever read. Owner thread only.
    void useBundledBank(bool enabled) { bundledBank_ = enabled; }

    // Which kernel the audio thread is currently reading. Test/diagnostic hook —
    // the swap is otherwise invisible from outside.
    int activeSlotForTest() const { return activeSlot_.load(std::memory_order_acquire); }

    bool hasCustomIr() const { return !customIr_.empty(); }

    void process(float* const* channels, int numChannels, int numFrames) {
        processInternal(channels, numChannels, numFrames, false);
    }

    // Composition seam for DualCabCore: same FIR/resonance/cuts, with dry/wet
    // and final safety clamp deferred until after both slots have been blended.
    void processWet(float* const* channels, int numChannels, int numFrames) {
        processInternal(channels, numChannels, numFrames, true);
    }

private:
    void processInternal(float* const* channels, int numChannels, int numFrames, bool wetOnly) {
        int nch = std::min(numChannels, (int)ch_.size());
        // One acquire load pins BOTH the kernel and its length to the same
        // published slot. Reading them from a buffer being rewritten in place
        // used to mix two IRs mid-block, and `taps` could describe a kernel the
        // samples no longer matched.
        const Slot& slot = slots_[activeSlot_.load(std::memory_order_acquire)];
        const float* rev = slot.rev.data();
        const int taps = slot.taps;

        // Channel-OUTER loop: one ramp line for the block, handed to every
        // channel, advanced once after the loop.
        float mixStep = 0.f;
        const float mixBase = mixSmooth_.blockStart(numFrames, mixStep);

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (!x) continue;
            Channel& st = ch_[c];
            for (int i = 0; i < numFrames; ++i) {
                float dry = x[i];
                // Scrub before the FIR: one NaN in the history ring would
                // poison the next `taps` output samples.
                float in = std::isfinite(dry) ? dry : 0.f;
                float w = st.fir.processSample(in, rev, taps);
                w = st.res.process(w);       // driver Fs resonance
                w = st.lowCut.process(w);
                w = st.highCut.process(w);
                if (!std::isfinite(w)) w = 0.f;
                const float m = mixBase + mixStep * static_cast<float>(i);
                float y = wetOnly ? w : dry * (1.f - m) + w * m;
                x[i] = wetOnly ? y : clampSafe(y);
            }
        }

        mixSmooth_.advanceBlock(numFrames);
    }

public:
    int latencySamples() const { return 0; }   // minimum-phase IR

private:
    static float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    static float clampSafe(float x) {
        if (!std::isfinite(x)) return 0.f;
        if (x >  16.f) return  16.f;
        if (x < -16.f) return -16.f;
        return x;
    }

    static const int kMaxTaps = 4096;

    struct Channel {
        detail::FirConvolver fir;
        Biquad res, lowCut, highCut;
        void reset() { fir.reset(); res.reset(); lowCut.reset(); highCut.reset(); }
    };

    // Per-model driver resonance (see the header note). Fs tracks cone size and
    // box type — a 12" in an open-back combo sits ~100 Hz, a closed 4x12 with
    // modern drivers ~78 Hz, an 8" tweed ~140 Hz, bass drivers 50-60 Hz — and
    // the depth tracks how much of that peak survives close-miking (an open
    // back keeps more; a tight modern 4x12 is deliberately the flattest). These
    // are the only numbers here that are voicing rather than physics; they are
    // tuned so every guitar model lands within a few dB of its own midband at
    // the low E instead of 5-10 dB under it.
    struct Resonance { float fcHz, q, gainDb; };
    static constexpr Resonance kResonance[cabir::kNumModels] = {
        { 100.f, 1.0f, 4.5f },  //  0 1x12 Combo (open back)
        {  95.f, 1.0f, 1.5f },  //  1 2x12 Combo
        {  90.f, 1.1f, 0.0f },  //  2 4x12 Scooped      — bank already carries it
        {  85.f, 1.1f, 1.0f },  //  3 Vintage 4x12
        { 140.f, 0.9f, 5.0f },  //  4 1x8 Tweed (small cone, high Fs)
        { 105.f, 1.0f, 5.5f },  //  5 2x12 AC Open
        {  90.f, 1.1f, 1.5f },  //  6 2x12 Boutique Closed
        {  80.f, 1.1f, 0.5f },  //  7 4x12 Greenback
        {  78.f, 1.1f, 0.0f },  //  8 4x12 V30 Modern   — bank already carries it
        {  60.f, 1.0f, 0.5f },  //  9 4x10 Bass
        {  50.f, 0.9f, 0.0f },  // 10 8x10 Bass Fridge  — bank already carries it
        {  55.f, 0.9f, 0.0f },  // 11 1x15 Vintage Bass — bank already carries it
        // 12-25 (2026-10, DotCharter #74) are built from MEASURED driver
        // responses with the box's own Thiele-Small resonance in them
        // (tools/cab_irs/gen_cab_irs.py), so none of them is missing the Fs
        // peak this table restores: every row is 0 dB, i.e. no stage at all.
        // The corner is only the documented resonance, for a reader.
        {  97.f, 1.0f, 0.0f },  // 12 4x10 Tweed Open   — P10R Fs 97 Hz, open back
        {  97.f, 1.0f, 0.0f },  // 13 1x10 Combo        — P10R Fs 97 Hz, open back
        {  99.f, 1.0f, 0.0f },  // 14 2x15 Guitar       — P15N closed box fc 99 Hz
        {  69.f, 0.7f, 0.0f },  // 15 PA 15 + Horn      — BS-15N350A, Qtc 0.707 fc 69 Hz
        { 125.f, 1.0f, 0.0f },  // 16 PA Column         — MOD 5-30 Fs 125 Hz
        {  48.f, 0.7f, 0.0f },  // 17 Hi-Fi Tower       — -3 dB at 48 Hz (spec)
        {  68.f, 0.7f, 0.0f },  // 18 2x10 Bass         — BP10150 QB3 F3 68 Hz
        {  68.f, 0.7f, 0.0f },  // 19 3x10 Bass
        {  68.f, 0.7f, 0.0f },  // 20 6x10 Bass
        {  64.f, 0.7f, 0.0f },  // 21 1x12 Bass         — BP12250 QB3 F3 64 Hz
        {  64.f, 0.7f, 0.0f },  // 22 2x12 Bass
        {  65.f, 0.7f, 0.0f },  // 23 2x15 Bass         — BP15250, Qtc 0.707 fc 65 Hz
        {  65.f, 0.7f, 0.0f },  // 24 4x15 Bass
        {  65.f, 0.7f, 0.0f },  // 25 1x15 + 1x10 Bass
    };
    static_assert(sizeof(kResonance) / sizeof(kResonance[0]) == (size_t) cabir::kNumModels,
                  "one resonance row per bundled cab model");

    // Resample the active IR (user IR when loaded, else the selected bundled
    // model) to the engine rate and store it time-reversed for the contiguous
    // dot product. Prepare/param-change time only — never on the per-sample
    // path.
    //
    // DOUBLE-BUFFERED (was an in-place rewrite of the live kernel). The old
    // comment called the resulting mid-block IR mix an accepted glitch, but it
    // also left `taps` and the samples it describes published independently
    // from the message thread while process() read both — a formal data race,
    // and audibly a torn kernel on every model switch or user-IR load. Writing
    // to the INACTIVE slot and publishing the index costs one release store and
    // 16 KB per instance, and makes the swap atomic from the audio thread's
    // point of view. Same shape as MikuCore::setPhrase.
    // NO RATE CONVERSION HAPPENS HERE any more. Both sources — the cached model
    // kernel and the user IR — are held at the engine rate (see prepare(),
    // setCustomIr() and adoptModelKernel()), so this is a bounded reverse copy
    // plus one release store, which is what makes it safe to reach from the
    // audio thread's mailbox drain.
    void rebuildIr() {
        const int next = 1 - activeSlot_.load(std::memory_order_relaxed);
        Slot& dst = slots_[next];

        const float* src = nullptr;
        int taps = 0;
        if (!customIr_.empty()) {
            src  = customIr_.data();
            taps = std::min((int) customIr_.size(), kMaxTaps);
        } else if (modelLin_.size() == (size_t) kMaxTaps) {
            src  = modelLin_.data();
            taps = modelTaps_;
        }
        if (src == nullptr || taps < 1) { dst.rev[0] = 1.f; dst.taps = 1; }
        else {
            for (int i = 0; i < taps; ++i) dst.rev[i] = src[taps - 1 - i];
            dst.taps = taps;
        }

        activeSlot_.store(next, std::memory_order_release);
    }

    // Owner thread. The one place a bundled model is converted inside this
    // core; every call is counted so a test can assert the AUDIO thread never
    // reaches it.
    void resampleModelIntoCache() {
        ++conversions_;
        modelTaps_ = detail::resampleIr(cabir::kIr48k[model_], cabir::kIrLen,
                                        cabir::kIrRate, modelLin_.data(), kMaxTaps, sr_);
        if (modelTaps_ < 1) { modelTaps_ = 1; modelLin_[0] = 1.f; }
    }

public:
    /// How many times this core has converted a bundled model to the engine
    /// rate. prepare() is one; anything else during playback is the bug this
    /// counter exists to catch. Test/diagnostic hook only.
    long conversionsForTest() const { return conversions_; }

private:

    static int clampModel(int m) {
        return m < 0 ? 0 : (m >= cabir::kNumModels ? cabir::kNumModels - 1 : m);
    }

    // One published kernel: the time-reversed taps and the length that
    // describes them, so the audio thread can never pair one with the other's.
    struct Slot {
        std::vector<float> rev;
        int taps = 1;
    };

    double sr_ = 48000.0;
    int model_ = 0;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixSmooth_;
    CabParams params_;
    Slot slots_[2];
    std::atomic<int> activeSlot_{0};
    std::vector<float> irLin_;         // owner-thread conversion scratch
    std::vector<float> modelLin_;      // the selected bundled model AT THE ENGINE RATE
    int                modelTaps_ = 0;
    bool               bundledBank_ = true;
    long               conversions_ = 0;
    std::vector<float> customIr_;      // user IR AT THE ENGINE RATE ("" = empty)
    double customRate_ = 48000.0;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
