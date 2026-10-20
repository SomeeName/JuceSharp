// dcdsp_acpre.h — acoustic guitar PREAMP (the DI box in front of the body sim).
//
// Models: the preamp strip of a stage acoustic DI — a swept anti-feedback
// notch, a phase-invert switch, a three-band EQ, an air/presence shelf and a
// "quack" corrector for under-saddle piezo pickups.
// Topology: notch -> [phase] -> bass shelf -> mid bell -> treble shelf ->
// presence blend -> piezo-fix blend -> level. Entirely LINEAR: no waveshaper,
// no DC blocker, no oversampling.
//
// Easy to get wrong: at flat settings this core must be BIT-TRANSPARENT, so
// that `phase = Inverted` is an exact negation and a player can null the box
// against the dry signal. That is why every knob whose neutral position is
// "off" is a blend `x + w * (f(x) - x)` with w smoothed from zero (w == 0 is
// exactly x), and why the EQ bands are RBJ sections at 0 dB, which normalise
// to numerator == denominator and cancel exactly. Adding a DC blocker or a
// makeup gain "for safety" here breaks that.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct AcPreParams {
    float notchHz      = 120.f; // 60..500 Hz
    float notchDepthDb = 0.f;   // 0..30 dB of cut
    float notchQ       = 10.f;  // 2..30
    int   phase        = 0;     // 0 Normal / 1 Inverted
    float bass         = 0.f;   // -12..12 dB, shelf 100 Hz
    float mid          = 0.f;   // -12..12 dB, bell 800 Hz
    float treble       = 0.f;   // -12..12 dB, shelf 5 kHz
    float presence     = 0.3f;  // 0..1 air shelf + tilt
    float piezoFix     = 0.f;   // 0..1 the under-saddle "quack" corrector
    float level        = 0.7f;  // 0..1
};

class AcPreCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) c.reset();

        levelBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        phaseBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        presBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        piezoBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);

        level_.reset(sr_, kRampSeconds);
        phase_.reset(sr_, kRampSeconds);
        pres_.reset(sr_, kRampSeconds);
        piezo_.reset(sr_, kRampSeconds);

        setParams(params_);
        level_.snapToTarget();
        phase_.snapToTarget();
        pres_.snapToTarget();
        piezo_.snapToTarget();
    }

    void reset() { for (auto& c : ch_) c.reset(); }

    void setParams(const AcPreParams& p) {
        params_ = p;
        level_.setTarget(clamp01(p.level) * 1.3f);
        // A phase FLIP is a full-scale discontinuity, so it walks through zero
        // over the same 10 ms every other switch in the rig uses.
        phase_.setTarget(p.phase > 0 ? -1.f : 1.f);
        pres_.setTarget(clamp01(p.presence));
        piezo_.setTarget(clamp01(p.piezoFix));

        const double nHz = clampRange(p.notchHz, 60.0, 500.0);
        const double nQ  = clampRange(p.notchQ, 2.0, 30.0);
        const double nDb = clampRange(p.notchDepthDb, 0.0, 30.0);
        for (auto& c : ch_) {
            // The notch is a deep PEAKING cut, not setNotch(): a true notch is
            // infinitely deep and has no depth control at all, and the knob a
            // player reaches for is "how much".
            c.notch.setPeaking(sr_, nHz, nQ, -nDb);
            // The three tone controls are SYMMETRIC, so a rejected NaN lands
            // on 0 dB (flat) rather than on `lo`, which for these is a full
            // -12 dB cut — the most extreme setting the knob has.
            c.bassSh.setLowShelf(sr_, kBassHz, kShelfQ, clampRange(p.bass, -12.0, 12.0, 0.0));
            c.midBq.setPeaking(sr_, kMidHz, kMidQ, clampRange(p.mid, -12.0, 12.0, 0.0));
            c.trebSh.setHighShelf(sr_, kTrebleHz, kShelfQ, clampRange(p.treble, -12.0, 12.0, 0.0));
            // Fixed-shape branches; the KNOB is the blend weight, which is what
            // makes them ramp and makes zero exactly transparent.
            c.airSh.setHighShelf(sr_, kAirHz, 0.707, kAirMaxDb);
            c.tiltSh.setLowShelf(sr_, kTiltHz, 0.707, kTiltMaxDb);
            c.quackBq.setPeaking(sr_, kQuackHz, kQuackQ, kQuackMaxDb);
            c.quackLo.setLowShelf(sr_, kQuackLoHz, 0.707, kQuackLoMaxDb);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // skip whole, never half-process
        const int nch    = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        for (int i = 0; i < frames; ++i) {
            const size_t b = static_cast<size_t>(i);
            levelBuf_[b] = level_.next();
            phaseBuf_[b] = phase_.next();
            presBuf_[b]  = pres_.next();
            piezoBuf_[b] = piezo_.next();
        }

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (x == nullptr) continue;
            Channel& st = ch_[static_cast<size_t>(c)];

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                float s = detail::sanitize(x[i]);
                s = st.notch.process(s);
                s *= phaseBuf_[b];
                s = st.bassSh.process(s);
                s = st.midBq.process(s);
                s = st.trebSh.process(s);

                // Presence: an air shelf plus a small tilt away from the low
                // mids, both blended in by the knob. At 0 this is `s` exactly.
                const float pw  = presBuf_[b];
                const float air = st.tiltSh.process(st.airSh.process(s));
                s = s + pw * (air - s);

                // Piezo fix: the "quack" is a resonance around 1.8 kHz plus a
                // thin low end. A BOUNDED bell cut and a BOUNDED low shelf, so
                // the corrector cannot become a high-pass at the knob's top.
                const float qw = piezoBuf_[b];
                const float fixd = st.quackLo.process(st.quackBq.process(s));
                s = s + qw * (fixd - s);

                x[i] = detail::sanitize(s * levelBuf_[b]);
            }
        }
    }

    int latencySamples() const { return 0; }

    float levelCurrentForTest() const { return level_.current(); }

private:
    static constexpr float  kRampSeconds  = 0.010f;
    static constexpr double kBassHz       = 100.0;
    static constexpr double kMidHz        = 800.0;
    static constexpr double kTrebleHz     = 5000.0;
    static constexpr double kShelfQ       = 0.8;
    static constexpr double kMidQ         = 1.1;
    static constexpr double kAirHz        = 10000.0;
    static constexpr double kAirMaxDb     = 8.0;
    static constexpr double kTiltHz       = 350.0;
    static constexpr double kTiltMaxDb    = -3.0;
    static constexpr double kQuackHz      = 1800.0;
    static constexpr double kQuackQ       = 1.2;
    static constexpr double kQuackMaxDb   = -6.0;
    static constexpr double kQuackLoHz    = 160.0;
    static constexpr double kQuackLoMaxDb = 2.5;

    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.f;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }
    static double clampRange(float v, double lo, double hi) { return clampRange(v, lo, hi, lo); }
    static double clampRange(float v, double lo, double hi, double fallback) {
        if (!std::isfinite(v)) return fallback;
        const double d = static_cast<double>(v);
        return d < lo ? lo : (d > hi ? hi : d);
    }

    struct Channel {
        Biquad notch, bassSh, midBq, trebSh, airSh, tiltSh, quackBq, quackLo;
        void reset() {
            notch.reset(); bassSh.reset(); midBq.reset(); trebSh.reset();
            airSh.reset(); tiltSh.reset(); quackBq.reset(); quackLo.reset();
        }
    };

    double sr_       = 48000.0;
    int    maxBlock_ = 512;
    AcPreParams params_{};
    SmoothedValue level_, phase_, pres_, piezo_;
    std::vector<float> levelBuf_, phaseBuf_, presBuf_, piezoBuf_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
