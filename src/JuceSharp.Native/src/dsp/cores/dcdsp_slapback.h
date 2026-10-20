// dcdsp_slapback.h — rockabilly slapback echo (pure dcdsp core, RT-safe).
//
// The tight single repeat of a Sun Studio tape machine: one distinct echo
// 60-140 ms behind you, darker and softer than the note that made it. Scotty
// Moore, Setzer, Cochran, every rock'n'roll and psychobilly rhythm part.
//
// ---------------------------------------------------------------------------
// Why this is NOT `delay`/`tapedelay` with the time knob turned down
// ---------------------------------------------------------------------------
// Both existing echoes are IIR: one delay line, one feedback coefficient, so
// the repeats are an infinite geometric tail through the SAME filter applied
// over and over (repeat k = fb^k with the tone filter cascaded k times). You
// set "one repeat" by setting feedback near 0 and hoping.
//
// A slapback is FIR. There is no feedback path at all:
//
//     y = x + sum_{k=1..repeats} g_k * LP_k( tape( x[n - k*D - wow_k] ) )
//
//   * `repeats` is an exact COUNT (1..3), not a decay coefficient. One repeat
//     means one repeat — the defining feature of the sound.
//   * each tap has its OWN low-pass corner and gain, so the voicing is authored
//     per repeat instead of falling out of a cascade. The tape machines this
//     imitates lose ~an octave of top per pass; taps 2 and 3 are set to that
//     curve rather than to whatever fb^k * LP^k happens to give.
//   * no recirculation => unconditionally stable at every setting. There is no
//     runaway, no self-oscillation, and no need for a limiter in a loop.
//   * the record path is tanh-saturated ONCE (a real tape machine only records
//     once), not compounded per lap.
//
// Wow is a slow, per-tap-decorrelated pitch wobble from the shared LFO pair, at
// the tiny depth of a well-maintained machine (<= 0.35 ms at wobble = 1), not
// the seasick sweep of `tapedelay`'s echo chamber.
//
// Honest overlap statement: at repeats = 1, wobble = 0, drive = 0, this lands in
// the same neighbourhood as `tapedelay` @ timeMs 110 / feedback 0. What it adds
// is the exact-count FIR structure, the per-tap voicing, and a one-pedal
// control surface for the sound.
//
// Pure processing core — NO JUCE, NO I/O. Allocation only in prepare().
// process() never allocates, locks, or throws. latencySamples() == 0.
#pragma once

#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>

namespace dcdsp {

// All ranges are clamped internally; out-of-range values are safe.
struct SlapbackParams {
    float timeMs  = 110.0f; // 40 .. 300 — the slap (classic window is 60-140)
    int   repeats = 1;      // 1 .. 3    — EXACT echo count (no feedback loop)
    float mix     = 0.35f;  // 0 .. 1    — dry .. wet
    float tone    = 0.5f;   // 0 .. 1    — tape top-end of the repeats (0 dark .. 1 bright)
    float drive   = 0.25f;  // 0 .. 1    — record-head saturation (applied once)
    float wobble  = 0.20f;  // 0 .. 1    — head wow depth (<= 0.35 ms at 1)
};

class SlapbackCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_    = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numCh_ = (numChannels > 0) ? numChannels : 1;

        wowMaxSamples_ = static_cast<float>(kWowMaxMs * sr_ / 1000.0);

        // Longest tap: 3 * max time + full wow excursion + guard.
        maxDelaySamples_ = static_cast<int>(std::ceil(
            kMaxTaps * kMaxTimeMs * sr_ / 1000.0 + wowMaxSamples_)) + 8;

        ch_.assign(static_cast<std::size_t>(numCh_), Channel{});
        for (auto& c : ch_) { c.line.prepare(maxDelaySamples_); c.dc.prepareLegacy48k(sr_); }

        // Two decorrelated wow oscillators; each tap mixes them at its own
        // weights so no two repeats wobble in lockstep.
        wowA_.prepare(sr_); wowA_.setShape(Lfo::Sine); wowA_.setFrequency(kWowAHz);
        wowB_.prepare(sr_); wowB_.setShape(Lfo::Sine); wowB_.setFrequency(kWowBHz);
        wowB_.setPhase(0.41f);

        smTime_.reset(sr_, 0.050f);
        smMix_.reset(sr_, 0.015f);
        // `drive` is normalised (satInvGain_), so it does not zipper quiet -- but
        // at working levels it is a level step AND it displaces the ADAA state's
        // operating point in one sample. `wobble` is worse in kind: it is added
        // to a read TAP that `timeMs` already glides for exactly this reason.
        smSat_.reset(sr_, 0.015f);
        smWow_.reset(sr_, 0.020f);
        // 30 ms, the window dcdsp_chorus.h fades its voice COUNT over: adding
        // or removing a tap is a read PATH, not a level (W4-D22).
        smTaps_.reset(sr_, kTapFadeSeconds);
        smTaps_.setCurrentAndTarget(1.0f);
        tapsPrev_ = taps_;

        firstParams_ = true;
        setParams(p_);
        smTime_.setCurrentAndTarget(timeTgt_);
        smMix_.setCurrentAndTarget(mixTgt_);
        smSat_.snapToTarget();
        smWow_.snapToTarget();
        reset();
    }

    void reset() {
        for (auto& c : ch_) {
            c.line.reset();
            for (auto& f : c.tap) f.reset();
            c.satState = 0.0f;
            c.dc.reset();
        }
        wowA_.reset();
        wowB_.reset();
        wowB_.setPhase(0.41f);
        smTime_.setCurrentAndTarget(timeTgt_);
        smMix_.setCurrentAndTarget(mixTgt_);
    }

    void setParams(const SlapbackParams& p) {
        p_ = p;

        timeTgt_ = clampf(p.timeMs, kMinTimeMs, kMaxTimeMs) * 0.001f
                   * static_cast<float>(sr_);
        mixTgt_  = clampf(p.mix, 0.0f, 1.0f);
        setTapCount((p.repeats < 1) ? 1 : (p.repeats > kMaxTaps ? kMaxTaps : p.repeats));
        wowDepth_ = clampf(p.wobble, 0.0f, 1.0f) * wowMaxSamples_;
        smWow_.setTarget(wowDepth_);

        // Record-head saturation: 1 (clean) .. 5 (slammed), normalised so a
        // quiet signal stays unity-gain (same law as dcdsp_tapedelay).
        satGain_    = 1.0f + 4.0f * clampf(p.drive, 0.0f, 1.0f);
        satInvGain_ = 1.0f / satGain_;
        smSat_.setTarget(satGain_);

        const float tone = clampf(p.tone, 0.0f, 1.0f);
        if (firstParams_ || tone != tone_) {
            tone_ = tone;
            applyTapTone();
        }
        if (firstParams_) {
            smTime_.setCurrentAndTarget(timeTgt_);
            smMix_.setCurrentAndTarget(mixTgt_);
        }
        smTime_.setTarget(timeTgt_);
        smMix_.setTarget(mixTgt_);
        firstParams_ = false;
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        const int nch = (numChannels < numCh_) ? numChannels : numCh_;

        const float minTap = 1.0f;
        const float maxTap = static_cast<float>(maxDelaySamples_) - 2.0f;

        for (int n = 0; n < numFrames; ++n) {
            const float base   = smTime_.next();
            const float mix    = smMix_.next();
            const float satG   = smSat_.next();
            const float satIG  = 1.0f / satG;
            const float wowDep = smWow_.next();
            const float dry  = 1.0f - mix;

            // Shared wow oscillators; per-tap weights below decorrelate them.
            const float wa = wowA_.process();
            const float wb = wowB_.process();

            // W4-D22: `repeats` used to STEP the tap set — 1 -> 2 spliced a
            // second echo in at gain 0.42 in one sample, and 2 -> 1 removed it
            // the same way. Same shape and same fix as dcdsp_chorus.h's voice
            // COUNT: run max(prev, cur) taps and weight the ones only ONE of the
            // two counts has by the fade. At rest tf == 1 and tLo == tActive_,
            // so this is the pre-fade expression exactly.
            const float tf    = smTaps_.next();
            const int   tLo   = tapsPrev_ < taps_ ? tapsPrev_ : taps_;
            const int   tHi   = tapsPrev_ > taps_ ? tapsPrev_ : taps_;
            const float edgeW = (taps_ >= tapsPrev_) ? tf : (1.0f - tf);

            // Per-tap read positions, computed once for all channels.
            float taps[kMaxTaps];
            for (int k = 0; k < tHi; ++k) {
                const float wow = kWowW[k][0] * wa + kWowW[k][1] * wb;
                float d = base * static_cast<float>(k + 1) + wowDep * wow;
                taps[k] = clampf(d, minTap, maxTap);
            }

            for (int c = 0; c < nch; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                Channel& st = ch_[static_cast<std::size_t>(c)];

                float x = buf[n];
                if (!std::isfinite(x)) x = 0.0f;

                // Read every tap BEFORE writing, so tap 1 is a full period old.
                float wet = 0.0f;
                for (int k = 0; k < tHi; ++k) {
                    const float w = (k < tLo) ? 1.0f : edgeW;
                    wet += w * kTapGain[k] * st.tap[k].process(st.line.readAt(taps[k]));
                }

                // --- record path: saturate ONCE, DC-block, store. No feedback.
                const float rec = adaaTanh(x * satG, st.satState) * satIG;
                st.line.push(st.dc.process(rec));

                float out = dry * x + mix * wet;
                if (!std::isfinite(out)) out = 0.0f;
                buf[n] = clampf(out, -kOutClamp, kOutClamp);
            }
            if (tapsPrev_ != taps_ && !smTaps_.isRamping())
                tapsPrev_ = taps_;   // the fade landed; retire the old count
        }
    }

    int latencySamples() const { return 0; }

    /// Test-only: the tap-count crossfade weight (1 = settled on `taps_`).
    float tapFadeForTest() const { return smTaps_.current(); }

private:
    static constexpr float kTapFadeSeconds = 0.030f;

    /// Crossfade the tap COUNT, re-entrant the same way dcdsp_chorus.h's
    /// setVoiceCount is: going back inside the window swaps the two ends and
    /// keeps the blend rather than discarding one in flight.
    void setTapCount(int want) {
        if (want == taps_) return;
        if (want == tapsPrev_ && smTaps_.isRamping()) {
            const float t = smTaps_.current();
            tapsPrev_ = taps_;
            smTaps_.setCurrentAndTarget(1.0f - t);
        } else {
            tapsPrev_ = taps_;
            smTaps_.setCurrentAndTarget(0.0f);
        }
        taps_ = want;
        smTaps_.setTarget(1.0f);
    }

    void applyTapTone() {
        // Tap 1 sits where the `tone` knob puts it (2.0 kHz dark .. 9.0 kHz
        // bright). Each further repeat loses ~an octave of top, like another
        // pass over a real record/replay head pair.
        const double base = 2000.0 + 7000.0 * static_cast<double>(tone_);
        for (auto& c : ch_) {
            double fc = base;
            for (int k = 0; k < kMaxTaps; ++k) {
                c.tap[static_cast<std::size_t>(k)].setLowpass(sr_, fc);
                fc *= 0.55;   // ~ -0.86 octave per repeat
            }
        }
    }
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr int   kMaxTaps    = 3;
    static constexpr float kMinTimeMs  = 40.0f;
    static constexpr float kMaxTimeMs  = 300.0f;
    static constexpr double kWowMaxMs  = 0.35;   // a well-kept machine, not a wobbler
    static constexpr float kWowAHz     = 0.62f;
    static constexpr float kWowBHz     = 3.10f;
    static constexpr float kOutClamp   = 16.0f;

    // Per-repeat amplitude: a slap dies fast (this is what stops it reading as
    // a delay). Row k of kWowW mixes the two wow oscillators so no two repeats
    // wobble together; weights per row sum to 1 => |wow| <= 1.
    static constexpr float kTapGain[kMaxTaps]   = { 0.85f, 0.42f, 0.18f };
    static constexpr float kWowW[kMaxTaps][2]   = {
        { 0.70f, 0.30f }, { 0.35f, 0.65f }, { 0.55f, 0.45f }
    };

    struct Channel {
        InterpolatedDelayLine        line;
        std::array<OnePole, kMaxTaps> tap{};
        DcBlocker                    dc;
        float                        satState = 0.0f;
    };

    SlapbackParams p_{};
    bool   firstParams_ = true;
    double sr_ = 48000.0;
    int    numCh_ = 2;
    int    maxDelaySamples_ = 1;
    int    taps_ = 1;
    int    tapsPrev_ = 1;        // the count being faded OUT of
    SmoothedValue smTaps_;       // 0 -> 1 toward the new count

    std::vector<Channel> ch_;
    Lfo wowA_, wowB_;
    SmoothedValue smTime_, smMix_;
    SmoothedValue smSat_, smWow_;   // satGain_/wowDepth_ stay the TARGETS

    float timeTgt_ = 0.0f, mixTgt_ = 0.35f;
    float tone_ = 0.5f;
    float satGain_ = 2.0f, satInvGain_ = 0.5f;
    float wowMaxSamples_ = 0.0f, wowDepth_ = 0.0f;
};

} // namespace dcdsp
