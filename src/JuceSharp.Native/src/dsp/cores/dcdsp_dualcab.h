// Two independently loaded IRs over the existing cabinet FIR/filter core.
// All storage is allocated in prepare; mailboxes adopt files on the audio thread.
#pragma once
#include "dcdsp_cab.h"
#include <array>

namespace dcdsp {
struct DualCabParams : CabParams {
    float blend = 0.f;
    float gainADb = 0.f, gainBDb = 0.f;
    bool invertA = false, invertB = false;
    float delayAMs = 0.f, delayBMs = 0.f;
    float outputDb = 0.f;
};

class DualCabCore {
public:
    // Slot B is active ONLY when a real user IR is loaded into it (see the
    // `hasB` guard in process()), so it must never carry a bundled model: this
    // one CabParams — `model` included — is forwarded to BOTH slots, and B
    // therefore re-converted a 2,048-tap table to the engine rate on every
    // model switch of slot A, for a kernel nothing would ever read.
    DualCabCore() { b_.useBundledBank(false); }

    static constexpr int kMaxChannels = 8;
    static constexpr int kMaxBlockFrames = 65536;
    static constexpr float kMaxDelayMs = 20.f;
    // Matches the engine's supported device rates; limits delay-ring memory
    // even for a malformed direct caller. CabCore retains its 4096-tap IR bound.
    static constexpr double kMaxSampleRate = 384000.;

    void prepare(double sr, int block, int channels) {
        sr_ = std::isfinite(sr) ? std::clamp(sr, 8000., kMaxSampleRate) : 48000.;
        block_ = std::clamp(block, 1, kMaxBlockFrames);
        count_ = std::clamp(channels, 1, kMaxChannels);
        a_.prepare(sr_, block_, count_);
        b_.prepare(sr_, block_, count_);
        channel_.resize(count_);
        for (auto& c : channel_) {
            c.dry.assign(block_, 0.f); c.a.assign(block_, 0.f); c.b.assign(block_, 0.f);
            const int delay = static_cast<int>(std::ceil(sr_ * kMaxDelayMs / 1000.)) + 1;
            c.delayA.prepare(delay); c.delayB.prepare(delay);
        }
        for (auto& ramp : ramps_) ramp.reset(sr_, .010f);
        setParams(params_);
        for (auto& ramp : ramps_) ramp.snapToTarget();
    }
    void reset() {
        a_.reset(); b_.reset();
        for (auto& c : channel_) { c.delayA.reset(); c.delayB.reset(); }
    }
    void setParams(const DualCabParams& p) {
        params_ = p;
        CabParams wet = p; wet.mix = 1.f;
        a_.setParams(wet); b_.setParams(wet);
        ramps_[Mix].setTarget(finiteClamp(p.mix, 0.f, 1.f, 1.f));
        ramps_[Blend].setTarget(finiteClamp(p.blend, 0.f, 1.f, 0.f));
        ramps_[GainA].setTarget(gain(p.gainADb) * (p.invertA ? -1.f : 1.f));
        ramps_[GainB].setTarget(gain(p.gainBDb) * (p.invertB ? -1.f : 1.f));
        ramps_[Output].setTarget(gain(p.outputDb));
        ramps_[DelayA].setTarget(finiteClamp(p.delayAMs, 0.f, kMaxDelayMs, 0.f) * static_cast<float>(sr_ / 1000.));
        ramps_[DelayB].setTarget(finiteClamp(p.delayBMs, 0.f, kMaxDelayMs, 0.f) * static_cast<float>(sr_ / 1000.));
    }
    // Independent mailbox adoption sinks. A keeps its legacy bundled fallback;
    // B is active only with a real custom IR, so an empty B never attenuates A.
    CabCore& slotA() { return a_; }
    CabCore& slotB() { return b_; }

    void process(float* const* channels, int nch, int frames) {
        if (!channels || frames <= 0 || block_ < 1) return;
        const int count = std::min(nch, count_);
        if (count < 1) return;
        const bool hasB = b_.hasCustomIr();
        if (hasB != processedB_) {
            b_.reset();
            for (auto& state : channel_) state.delayB.reset();
            processedB_ = hasB;
        }
        for (int offset = 0; offset < frames;) {
            const int n = std::min(block_, frames - offset);
            std::array<float*, kMaxChannels> ap{}, bp{};
            for (int c = 0; c < count; ++c) {
                auto& state = channel_[c];
                for (int i = 0; i < n; ++i) {
                    const float dry = channels[c] ? channels[c][offset + i] : 0.f;
                    state.dry[i] = dry;
                    state.a[i] = state.b[i] = dry;
                }
                ap[c] = state.a.data(); bp[c] = state.b.data();
            }
            a_.processWet(ap.data(), count, n);
            if (hasB) b_.processWet(bp.data(), count, n);
            std::array<float, RampCount> start{}, step{};
            for (int r = 0; r < RampCount; ++r) start[r] = ramps_[r].blockStart(n, step[r]);
            for (int c = 0; c < count; ++c) {
                auto& state = channel_[c];
                for (int i = 0; i < n; ++i) {
                    auto value = [&](int r) { return start[r] + step[r] * static_cast<float>(i); };
                    state.delayA.push(state.a[i]);
                    const float wa = state.delayA.readAt(value(DelayA)) * value(GainA);
                    float w = wa;
                    if (hasB) {
                        state.delayB.push(state.b[i]);
                        const float wb = state.delayB.readAt(value(DelayB)) * value(GainB);
                        const float blend = value(Blend);
                        w = wa * (1.f - blend) + wb * blend;
                    }
                    const float m = value(Mix);
                    const float y = (state.dry[i] * (1.f - m) + w * m) * value(Output);
                    if (channels[c]) channels[c][offset + i] = std::isfinite(y) ? std::clamp(y, -16.f, 16.f) : 0.f;
                }
            }
            for (auto& ramp : ramps_) ramp.advanceBlock(n);
            offset += n;
        }
    }
    // Manual alignment is a wet-path effect, not fixed processing latency.
    int latencySamples() const { return 0; }

private:
    enum { Mix, Blend, GainA, GainB, Output, DelayA, DelayB, RampCount };
    static float finiteClamp(float value, float lo, float hi, float fallback) {
        return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    }
    static float gain(float db) { return std::pow(10.f, finiteClamp(db, -24.f, 24.f, 0.f) / 20.f); }
    struct Channel {
        std::vector<float> dry, a, b;
        InterpolatedDelayLine delayA, delayB;
    };
    CabCore a_, b_;
    DualCabParams params_;
    std::array<SmoothedValue, RampCount> ramps_;
    std::vector<Channel> channel_;
    double sr_ = 48000.;
    int block_ = 0, count_ = 0;
    bool processedB_ = false;
};
} // namespace dcdsp
