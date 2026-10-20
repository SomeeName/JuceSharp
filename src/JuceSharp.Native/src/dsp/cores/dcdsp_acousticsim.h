// dcdsp_acousticsim.h
//
// Acoustic Simulator — turns an electric guitar tone into an acoustic-
// flavoured one. Rebuilt (0.16 W2A) on the shared FIR engine: instead of two
// peaking EQs ("EQ honk"), the wet path now runs
//
//   low cut  ->  piezo-ize pre-stage  ->  body IR convolution  ->
//   string-shimmer shelf  ->  gentle bus compression  ->  DC block
//
// * PIEZO-IZE: a magnetic pickup is dark and mid-forward next to an
//   under-saddle piezo; an AC-3-style fixed pre-voicing (low-shelf cut tilt +
//   3-6 kHz presence peak) brightens the source before it excites the body.
// * BODY IR: three bundled 2048-tap (42.7 ms) minimum-phase guitar-BODY IRs
//   (parlor / OM / dreadnought — Helmholtz air peak, top/back plate modes,
//   string-body reflection combs; see dcdsp_acoustic_irs.h for the authoring
//   recipe). The `body` knob morphs small -> large body while raising the
//   IR blend against a unit delta, so 0 stays the old "flat" meaning and 1 is
//   a full dreadnought. Minimum-phase => latencySamples() stays 0.
// * `brightness` still drives the string-shimmer high shelf ("top" macro),
//   `lowCutHz` and `mix` are unchanged.
//
// PURE header-only. Namespace dcdsp. C++17. Standard-library only (plus SSE2
// intrinsics inside dcdsp_detail_fir.h on x86-64).
//
// RT-safety contract:
//   * All allocation happens in prepare().
//   * process() never allocates, locks, or throws.
//   * Output is ALWAYS finite and bounded: |x| < 32 (hard final clamp,
//     plus a NaN/Inf sanitiser on every sample).

#ifndef DCDSP_ACOUSTICSIM_H
#define DCDSP_ACOUSTICSIM_H

#include <atomic>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared engine primitives (Biquad/OnePole/EnvelopeFollower/DcBlocker/Lfo/
// InterpolatedDelayLine/SmoothedValue/...). Real definitions — no local stubs.
#include "dcdsp_primitives.h"
#include "dcdsp_detail_fir.h"
#include "dcdsp_acoustic_irs.h"

// ---------------------------------------------------------------------------
// Acoustic simulator core.
// ---------------------------------------------------------------------------
namespace dcdsp {

struct AcousticSimParams {
    // Body size/amount macro. Morphs the body IR small -> large
    // (parlor -> OM -> dreadnought) while raising the IR blend against a
    // unit delta: 0 = flat (no body), 0.5 = half-blended OM, 1 = full
    // dreadnought. [min 0, max 1, default 0.5]
    float body = 0.5f;

    // String-shimmer high shelf amount (boost above ~3.5 kHz).
    // [min 0, max 1, default 0.5]
    float brightness = 0.5f;

    // Low-cut (high-pass) corner in Hz — tames piezo boom / mud.
    // [min 40, max 200, default 90]
    float lowCutHz = 90.0f;

    // Dry/wet mix. 0 = fully dry (bypassed tone), 1 = fully processed.
    // [min 0, max 1, default 1]
    float mix = 1.0f;
};

class AcousticSimCore {
public:
    void prepare(double sr, int maxBlock, int nch) {
        sampleRate_ = (sr > 0.0) ? sr : 48000.0;
        (void)maxBlock;                       // per-sample convolution; no scratch
        if (nch < 1) nch = 1;
        chans_.assign(static_cast<std::size_t>(nch), ChannelState{});
        for (auto& c : chans_) { c.fir.prepare(kMaxTaps); c.dc.prepareLegacy48k(sampleRate_); }

        // Resample the three 48 kHz body IRs to the engine rate once; the
        // `body` knob then blends them allocation-free in updateCoeffs().
        for (int b = 0; b < bodyir::kNumBodies; ++b) {
            bodyIr_[b].assign(kMaxTaps, 0.f);
            bodyTaps_ = detail::resampleIr(bodyir::kIr48k[b], bodyir::kIrLen,
                                           bodyir::kIrRate, bodyIr_[b].data(),
                                           kMaxTaps, sampleRate_);
        }
        if (bodyTaps_ < 1) bodyTaps_ = 1;
        irLin_.assign(kMaxTaps, 0.f);
        customIr_.reserve(kMaxTaps);
        customEng_.reserve(kMaxTaps);
        for (auto& s : slots_) { s.rev.assign(kMaxTaps, 0.f); s.taps = 1; }
        activeSlot_.store(0, std::memory_order_release);

        // Re-apply a user body IR across a re-prepare (rate change).
        if (!customIr_.empty()) {
            customEng_.assign(kMaxTaps, 0.f);
            customTaps_ = detail::resampleIr(customIr_.data(), (int) customIr_.size(),
                                             customRate_, customEng_.data(),
                                             kMaxTaps, sampleRate_);
            if (customTaps_ < 1) customTaps_ = 0;
        }

        prepared_ = true;
        // `mix` RAMPS (kMixRampSeconds); as a per-block constant a mix drag
        // stepped a full dry/wet crossfade at every block edge.
        mixSmooth_.reset(sampleRate_, kMixRampSeconds);
        mixSmooth_.setTarget(params_.mix);
        mixSmooth_.snapToTarget();   // a freshly prepared sim starts AT its mix
        updateCoeffs();
        reset();
    }

    // Replace the bundled body-IR bank with a USER impulse response (0.16
    // user-IR loading, rides ABI v22). The `body` knob keeps its meaning as
    // the wet-IR blend amount (0 = flat, 1 = full IR); the parlor/OM/dread
    // morph is bypassed while a custom IR is active. Processing-thread only.
    //
    // Called only by the audio-thread IR mailbox (or while processing is
    // stopped). prepare() reserves all storage; adoption never allocates.
    void setCustomIr(const float* samples, int count, double rate) {
        if (samples == nullptr || count < 1 || rate <= 0.0) return;
        customIr_.assign(samples, samples + std::min(count, kMaxTaps));
        customRate_ = rate;
        if (prepared_) {
            customEng_.assign(kMaxTaps, 0.f);
            customTaps_ = detail::resampleIr(customIr_.data(), (int) customIr_.size(),
                                             customRate_, customEng_.data(),
                                             kMaxTaps, sampleRate_);
            if (customTaps_ < 1) customTaps_ = 0;
            updateCoeffs();
        }
    }

    void clearCustomIr() {
        customIr_.clear();
        customTaps_ = 0;
        if (prepared_) updateCoeffs();
    }

    bool hasCustomIr() const { return !customIr_.empty(); }

    void reset() {
        for (auto& c : chans_) {
            c.lowcut.reset();
            c.tilt.reset();
            c.presence.reset();
            c.fir.reset();
            c.shelf.reset();
            c.env.reset();
            c.dc.reset();
        }
    }

    /// TEST SURFACE. How many times the 4,096-tap convolution kernel has been
    /// rebuilt, and how many times the per-channel biquad voicing has been
    /// redesigned. A `mix` tick must raise NEITHER: setParams runs on the audio
    /// callback.
    long long kernelRebuilds() const noexcept { return kernelRebuilds_; }
    /// TEST SURFACE. See kernelRebuilds().
    long long voicingRebuilds() const noexcept { return voicingRebuilds_; }

    void setParams(const AcousticSimParams& p) {
        params_.body       = clamp01(p.body);
        params_.brightness = clamp01(p.brightness);
        params_.lowCutHz   = clampf(p.lowCutHz, 40.0f, 200.0f);
        params_.mix        = clamp01(p.mix);
        mixSmooth_.setTarget(params_.mix);
        // setParams is drained on the AUDIO CALLBACK (the adapter's
        // ParamMailbox is applied from the top of process()), so a `mix` tick
        // used to cost two passes over up to 4,096 IR taps plus five biquad
        // designs per channel -- each with a cos/sin/pow. Allocation-free,
        // which is exactly why the RT gate could not see it. dcdsp_cab.h
        // already guards the equivalent. Split the rebuild in two and run each
        // half only when its OWN inputs moved.
        if (prepared_) {
            const bool kernelMoved = (params_.body != lastKernelBody_)
                                  || (customTaps_ != lastKernelTaps_);
            const bool voicingMoved = (params_.brightness != lastVoicingBright_)
                                   || (params_.lowCutHz  != lastVoicingLowCut_);
            if (kernelMoved)  updateKernel();
            if (voicingMoved) updateVoicing();
        }
    }

    // IN-PLACE. ch[c] points at nframes floats; nch channels processed.
    void process(float* const* ch, int nch, int nframes) {
        if (!prepared_ || ch == nullptr || nch <= 0 || nframes <= 0) return;
        const int nc = std::min(nch, static_cast<int>(chans_.size()));

        // Channel-OUTER loop: one ramp line for the block, handed to every
        // channel, advanced once after the loop.
        float mixStep = 0.0f;
        const float mixBase = mixSmooth_.blockStart(nframes, mixStep);
        // One acquire load pins the kernel and its length to the SAME slot;
        // they used to be published independently while process() read both.
        const Slot&  slot   = slots_[activeSlot_.load(std::memory_order_acquire)];
        const float* rev    = slot.rev.data();
        const int    taps   = slot.taps;

        for (int c = 0; c < nc; ++c) {
            float* x = ch[c];
            if (x == nullptr) continue;
            ChannelState& s = chans_[static_cast<std::size_t>(c)];

            for (int i = 0; i < nframes; ++i) {
                const float dry = sanitize(x[i]);

                // --- wet chain -------------------------------------------
                float w = dry;
                w = s.lowcut.process(w);    // low cut
                w = s.tilt.process(w);      // piezo-ize: bright tilt
                w = s.presence.process(w);  // piezo-ize: 3-6 kHz presence
                // Body convolution. Input is already sanitized, but one
                // non-finite sample in the history ring would poison the
                // next `taps` outputs — guard the filter output too.
                if (!std::isfinite(w)) w = 0.0f;
                w = s.fir.processSample(w, rev, taps);
                w = s.shelf.process(w);     // string shimmer high shelf

                // gentle compression (dB-domain static curve on a smoothed
                // envelope; only reduces above threshold, small makeup).
                const double envLin = s.env.process(w);
                const double envDb  = 20.0 * std::log10(envLin + 1e-9);
                double gainDb = compMakeupDb_;
                if (envDb > compThreshDb_) {
                    const double over = envDb - compThreshDb_;
                    gainDb += -over * compSlope_;    // slope = 1 - 1/ratio
                }
                if (gainDb > compMaxGainDb_) gainDb = compMaxGainDb_;
                const float g = static_cast<float>(dbToGain(gainDb));   // exp2 form
                w *= g;

                w = s.dc.process(w);       // strip any DC the chain added

                // --- mix + hard safety bound -----------------------------
                const float mix    = mixBase + mixStep * static_cast<float>(i);
                const float dryAmt = 1.0f - mix;
                float out = mix * w + dryAmt * dry;
                x[i] = clampOut(out);
            }
        }

        mixSmooth_.advanceBlock(nframes);
    }

    int latencySamples() const { return 0; }   // minimum-phase body IRs

private:
    static const int kMaxTaps = 4096;   // full 42.7 ms IR up to 96 kHz

    struct ChannelState {
        Biquad               lowcut;
        Biquad               tilt;      // piezo-ize low-shelf cut
        Biquad               presence;  // piezo-ize presence peak
        detail::FirConvolver fir;
        Biquad               shelf;
        EnvelopeFollower     env;
        DcBlocker            dc;
    };

    /// Both halves. prepare() and any caller that needs a full refresh.
    void updateCoeffs() {
        updateKernel();
        updateVoicing();
    }

    /// The convolution kernel. Depends on `body` and the custom IR, nothing
    /// else -- two passes over up to 4,096 taps.
    void updateKernel() {
        lastKernelBody_ = params_.body;
        lastKernelTaps_ = customTaps_;
        ++kernelRebuilds_;

        // Body morph: 0..0.5 parlor->OM, 0.5..1 OM->dreadnought, and the
        // whole body IR blends against a unit delta with weight `body` so 0
        // keeps the historical "flat" meaning. Allocation-free (buffers sized
        // in prepare()); O(taps), param-change time only.
        const float body = params_.body;
        // Double-buffered: build into the INACTIVE slot, publish the index.
        const int nextSlot = 1 - activeSlot_.load(std::memory_order_relaxed);
        Slot& dst = slots_[nextSlot];
        if (customTaps_ > 0) {
            // User IR active: blend it against the unit delta with the body
            // knob; the bundled small->large morph is bypassed.
            const int n = customTaps_;
            for (int k = 0; k < n; ++k) irLin_[k] = body * customEng_[k];
            irLin_[0] += 1.0f - body;
            for (int k = 0; k < n; ++k) dst.rev[k] = irLin_[n - 1 - k];
            dst.taps = n;
        } else {
            const int   a = body <= 0.5f ? 0 : 1;
            const int   b = a + 1;
            const float t = body <= 0.5f ? body * 2.0f : (body - 0.5f) * 2.0f;
            const int   n = bodyTaps_;
            const float wa = body * (1.0f - t), wb = body * t;
            for (int k = 0; k < n; ++k)
                irLin_[k] = wa * bodyIr_[a][k] + wb * bodyIr_[b][k];
            irLin_[0] += 1.0f - body;                   // unit-delta blend
            for (int k = 0; k < n; ++k) dst.rev[k] = irLin_[n - 1 - k];
            dst.taps = n;
        }
        activeSlot_.store(nextSlot, std::memory_order_release);
    }

    /// The per-channel filter voicing. Depends on `brightness` and `lowCutHz`
    /// -- five biquad designs per channel, each with a cos/sin/pow.
    void updateVoicing() {
        const double sr = sampleRate_;
        lastVoicingBright_ = params_.brightness;
        lastVoicingLowCut_ = params_.lowCutHz;
        ++voicingRebuilds_;

        // High shelf: shimmer boost 0..~8 dB with `brightness`.
        const double shelfDb = static_cast<double>(params_.brightness) * 8.0;
        const double shelfHz = 3500.0;

        for (auto& c : chans_) {
            c.lowcut.setHighpass(sr, static_cast<double>(params_.lowCutHz), 0.707);
            // Fixed AC-3-style piezo-ize voicing: tame the magnetic pickup's
            // low-mid weight, add the under-saddle 3-6 kHz snap.
            c.tilt.setLowShelf (sr,  350.0, 0.707, -3.5);
            c.presence.setPeaking(sr, 4200.0, 1.0,   4.5);
            c.shelf.setHighShelf(sr, shelfHz, 0.707, shelfDb);
            c.env.prepare(sr);
            c.env.setTimes(static_cast<float>(compAtkMs_), static_cast<float>(compRelMs_));
        }
    }

    long long kernelRebuilds_  = 0;
    long long voicingRebuilds_ = 0;
    float lastKernelBody_     = -1.0f;
    int   lastKernelTaps_     = -1;
    float lastVoicingBright_  = -1.0f;
    float lastVoicingLowCut_  = -1.0f;

    static inline float sanitize(float v) noexcept {
        return std::isfinite(v) ? v : 0.0f;
    }
    static inline float clampOut(float v) noexcept {
        if (!std::isfinite(v)) return 0.0f;
        const float lim = 31.9999f;      // strictly inside |x| < 32
        if (v >  lim) return  lim;
        if (v < -lim) return -lim;
        return v;
    }
    static inline float clamp01(float v) noexcept {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    static inline float clampf(float v, float lo, float hi) noexcept {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Fixed gentle-compression constants (ratio 2:1).
    static constexpr double compThreshDb_  = -18.0;
    static constexpr double compRatio_     = 2.0;
    static constexpr double compSlope_     = 1.0 - 1.0 / compRatio_;   // 0.5
    static constexpr double compMakeupDb_  = 2.0;
    static constexpr double compMaxGainDb_ = 3.0;   // cap net boost (~1.41x)
    static constexpr double compAtkMs_     = 10.0;
    static constexpr double compRelMs_     = 120.0;

    AcousticSimParams         params_{};
    static constexpr float    kMixRampSeconds = 0.010f;
    SmoothedValue             mixSmooth_;
    std::vector<ChannelState> chans_;
    std::array<std::vector<float>, bodyir::kNumBodies> bodyIr_;  // engine rate
    // One published kernel: reversed taps + the length describing them.
    struct Slot { std::vector<float> rev; int taps = 1; };
    Slot                      slots_[2];
    std::atomic<int>          activeSlot_{0};
    std::vector<float>        irLin_;           // rebuild scratch
    std::vector<float>        customIr_;        // user IR at its source rate
    std::vector<float>        customEng_;       // user IR at the engine rate
    double                    customRate_ = 48000.0;
    int                       customTaps_ = 0;
    int                       bodyTaps_  = 1;
    double                    sampleRate_ = 48000.0;
    bool                      prepared_   = false;
};

} // namespace dcdsp

#endif // DCDSP_ACOUSTICSIM_H
