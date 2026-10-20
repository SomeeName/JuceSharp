#pragma once
#include "DspModule.h"
#include "cores/dcdsp_detail_fir.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dcrig {
// WAV parsing happens on the owner thread. Only process() adopts the resulting
// bounded snapshot; no published buffer can be recycled underneath a reader.
// Cores reserve their custom-IR storage in prepare() before adoption is possible.
//
// THE RATE CONVERSION HAPPENS HERE, ON THE OWNER THREAD (2026-09-10). It used
// to happen inside the core, on the audio thread, at adoption time: an IR
// authored at any rate other than the device's went through
// detail::resampleIr — up to 4,096 output taps, six Lanczos taps each, two
// std::sin per tap — inside the single callback that drained the mailbox, plus
// three biquad redesigns per channel. It allocates nothing, so the RT gate
// never saw it, and 48 kHz devices short-circuit the conversion entirely, so
// the owner's own machine never showed it either. `setEngineRate` is what makes
// this possible; a core reached without it still converts on adoption, which is
// correct but is the behaviour this class exists to avoid.
class IrMailbox {
public:
    static constexpr int kMaxTaps = 4096;

    /// The device rate every published IR is converted TO. Owner thread only,
    /// from the module's prepare(); re-publishes the held IR when the rate
    /// moves, so a device change does not leave the core on a stale kernel.
    void setEngineRate(double rate) noexcept {
        if (!(rate > 0.0) || std::fabs(rate - engineRate_) < 1.0) return;
        engineRate_ = rate;
        if (haveSource_) republish();
    }

    void publish(const float* samples, int count, double rate) noexcept {
        if (samples == nullptr || count <= 0) {
            haveSource_ = false;
            srcCount_ = 0;
            mailbox_.publish(Payload{});
            return;
        }
        srcCount_ = std::max(0, std::min(count, kMaxTaps));
        // Fixed array, same reason as scratch_: this is `noexcept`.
        std::copy_n(samples, (std::size_t) srcCount_, src_.begin());
        srcRate_ = rate > 0.0 ? rate : engineRate_;
        haveSource_ = true;
        republish();
    }

    template<class Core> void applyPending(Core& core) {
        if (!mailbox_.consume(applied_)) return;
        const Payload& p = applied_;
        if (p.count == 0) core.clearCustomIr();
        else core.setCustomIr(p.samples.data(), p.count, p.rate);
    }

private:
    struct Payload {
        std::array<float, kMaxTaps> samples{};
        double rate = 48000.;
        int count = 0;
    };

    /// Owner thread. Converts the held source IR to the engine rate and posts
    /// it, so the core's own `setCustomIr` sees matching rates and copies.
    void republish() noexcept {
        Payload p{};
        p.rate = engineRate_;
        if (std::fabs(srcRate_ - engineRate_) < 1.0) {
            p.count = srcCount_;
            std::copy_n(src_.data(), p.count, p.samples.begin());
        } else {
            // std::array, never a vector::assign. This function is `noexcept`
            // and a bad_alloc inside a noexcept function is std::terminate, not
            // an error — a `noexcept` is a CLAIM, and "bounded at 4,096 floats
            // so it cannot realistically fail" is not the same as cannot.
            scratch_.fill(0.f);
            const int n = dcdsp::detail::resampleIr(src_.data(), srcCount_, srcRate_,
                                                    scratch_.data(), kMaxTaps, engineRate_);
            p.count = std::max(0, std::min(n, kMaxTaps));
            std::copy_n(scratch_.data(), p.count, p.samples.begin());
        }
        mailbox_.publish(p);
    }

    ParamMailbox<Payload> mailbox_;
    Payload applied_{}; // audio-owned; no large zero-filled temporary per idle block

    // Owner-thread only: the IR as it was parsed, so a device change can be
    // re-converted from the ORIGINAL rather than from an already-converted copy.
    // Fixed, so publish()/republish() allocate nothing and their `noexcept` is
    // true. A `noexcept` that can throw is std::terminate, not an error code.
    std::array<float, (std::size_t) kMaxTaps> src_{}, scratch_{};
    int    srcCount_   = 0;
    double srcRate_    = 48000.;
    double engineRate_ = 48000.;
    bool   haveSource_ = false;
};

// The bundled cabinet bank, converted to the engine rate on the OWNER thread
// and adopted by a bounded copy. A model switch is a 2,048-tap rate conversion
// exactly like a user IR is, and it used to run inside the callback that
// drained the adapter's ParamMailbox — the more common of the two, because
// every preset change and every model knob tick goes through it.
class ModelIrMailbox {
public:
    static constexpr int kMaxTaps = IrMailbox::kMaxTaps;

    /// Owner thread. `src`/`srcLen`/`srcRate` describe the bundled table.
    void publish(int model, const float* src, int srcLen, double srcRate, double engineRate) noexcept {
        Payload p{};
        p.model = model;
        p.rate  = engineRate;
        if (src == nullptr || srcLen <= 0) { p.count = 0; mailbox_.publish(p); return; }
        if (std::fabs(srcRate - engineRate) < 1.0) {
            p.count = std::min(srcLen, kMaxTaps);
            std::copy_n(src, p.count, p.samples.begin());
        } else {
            // std::array + fill, never a vector::assign — the same reason
            // IrMailbox::republish gives 40 lines up: this function is
            // `noexcept`, and a bad_alloc inside a noexcept function is
            // std::terminate, not an error. This sibling is the MORE reached of
            // the two (every model knob tick and every preset change) and it is
            // the one that did it.
            scratch_.fill(0.f);
            const int n = dcdsp::detail::resampleIr(src, srcLen, srcRate,
                                                    scratch_.data(), kMaxTaps, engineRate);
            p.count = std::max(0, std::min(n, kMaxTaps));
            std::copy_n(scratch_.data(), p.count, p.samples.begin());
        }
        mailbox_.publish(p);
    }

    template<class Core> void applyPending(Core& core) {
        if (!mailbox_.consume(applied_)) return;
        core.adoptModelKernel(applied_.samples.data(), applied_.count, applied_.model);
    }

private:
    struct Payload {
        std::array<float, kMaxTaps> samples{};
        double rate = 48000.;
        int count = 0;
        int model = 0;
    };
    ParamMailbox<Payload> mailbox_;
    Payload applied_{};
    std::array<float, (std::size_t) kMaxTaps> scratch_{};
};
} // namespace dcrig
