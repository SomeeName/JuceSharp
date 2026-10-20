// dcdsp_eq.h — EqCore: 3-band parametric EQ (low shelf + mid peak + high shelf)
// with a global output trim. All bands are dcdsp::Biquad (RBJ, TDF-II).
//
// Pure dcdsp core. RT-safe: the only allocation is the per-channel Biquad banks
// in prepare(); process() never allocates. Coefficients are recomputed only when
// the params actually change (dirty check), then pushed to every channel's bank
// (identical coefficients, independent state). Shelf slope uses a fixed Butterworth
// Q (kShelfQ = 1/sqrt2) since the contract exposes only freq + gain for shelves.
//
// Multi-channel: each channel runs its own low->mid->high Biquad chain, then the
// shared output-trim gain. Filtering is independent per channel (EQ is not a
// dynamics processor, so no cross-channel linking is needed).
#pragma once

#ifndef DCDSP_PRIMITIVES_INCLUDED
#include "dcdsp_primitives.h"
#endif

#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

// Frequency ranges (2026-10, DotCharter #68): wide enough for a four-band studio
// parametric over the whole audible band — a bass shelf down to 20 Hz, a
// low-mid bell down to 100 Hz, a high-mid bell up to 8 kHz and a treble shelf
// up to 20 kHz. They were 40-400 /
// 200-4000 / 2000-12000 Hz. The params are Hz, so a value inside the old ranges
// renders as before — with one exception: a treble corner above a quarter of
// the device rate (11.0 kHz at 44.1 kHz), which the shared filter ceiling used
// to hold there; it now lands where it says (see applyCoeffs).
struct EqParams {
    // Low shelf.
    float lowFreq    = 100.0f;  // [20 .. 500] Hz
    float lowGainDb  = 0.0f;    // [-18 .. +18] dB
    // Mid peak.
    float midFreq    = 1000.0f; // [100 .. 8000] Hz
    float midQ       = 1.0f;    // [0.3 .. 8]
    float midGainDb  = 0.0f;    // [-18 .. +18] dB
    // High shelf.
    float highFreq   = 6000.0f; // [2000 .. 20000] Hz, designed under 0.9 x Nyquist
    float highGainDb = 0.0f;    // [-18 .. +18] dB
    // Global output trim.
    float outputTrimDb = 0.0f;  // [-24 .. +24] dB
};

class EqCore {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        numCh_ = numChannels > 0 ? numChannels : 1;
        low_.assign(numCh_, Biquad{});
        mid_.assign(numCh_, Biquad{});
        high_.assign(numCh_, Biquad{});
        dirty_ = true;
        applyCoeffs();
        // Full-scale traverse of the +/-24 dB trim in ~10 ms. See process().
        trimSlew_ = static_cast<float>(kTrimMaxLin / (0.010 * sr_));
        trimCur_ = trimLin_;
        reset();
    }

    void reset() {
        for (int ch = 0; ch < numCh_; ++ch) {
            low_[ch].reset();
            mid_[ch].reset();
            high_[ch].reset();
        }
    }

    void setParams(const EqParams& p) {
        if (!same(p, p_)) { p_ = p; clamp(); dirty_ = true; }
        trimLin_ = static_cast<float>(std::pow(10.0, p_.outputTrimDb / 20.0));
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (numChannels <= 0 || numFrames <= 0) return;
        if (dirty_) applyCoeffs();

        const int nc = numChannels < numCh_ ? numChannels : numCh_;
        // The +/-24 dB output trim used to be a raw per-sample multiply read
        // straight from the params struct, so a preset switch from -24 to +24
        // stepped the output by a factor of 251 between two adjacent samples —
        // a full-scale click. It now glides, slew-limited to a full-scale
        // traverse in ~10 ms.
        //
        // The ramp is computed ONCE for the block, not held in a per-channel
        // smoother: this loop is channel-OUTER, so a stateful smoother advanced
        // inside it would run a different part of the ramp on each channel and
        // tilt the stereo image while a knob moved. (Same shape as
        // MergerModule::mergeRt's g0 + step*i.)
        const float gStart = trimCur_;
        float gEnd = trimLin_;
        const float maxStep = trimSlew_ * static_cast<float>(numFrames);
        if (gEnd > gStart + maxStep) gEnd = gStart + maxStep;
        else if (gEnd < gStart - maxStep) gEnd = gStart - maxStep;
        const float gStep = (gEnd - gStart) / static_cast<float>(numFrames);
        trimCur_ = gEnd;
        for (int ch = 0; ch < nc; ++ch) {
            Biquad& lo = low_[ch];
            Biquad& md = mid_[ch];
            Biquad& hi = high_[ch];
            float* x = channels[ch];
            for (int n = 0; n < numFrames; ++n) {
                float s = x[n];
                s = lo.process(s);
                s = md.process(s);
                s = hi.process(s);
                x[n] = s * (gStart + gStep * static_cast<float>(n + 1));
            }
        }
    }

    int latencySamples() const { return 0; }

private:
    void applyCoeffs() {
        // The treble shelf designs with the NEAR-NYQUIST ceiling (0.9 x Nyquist,
        // dcdsp_primitives.h clampCutoffNearNyquist) instead of the shared
        // quarter-rate one, which held every corner under 12 kHz at 48 kHz —
        // the 2-12 kHz range this band had until 2026-10 was that ceiling. The
        // guard sits on the rate-derived corner, never the stored param: a 20 kHz
        // shelf runs at 19.845 kHz on a 44.1 kHz device and gets its 20 kHz back
        // on a 48 kHz one (prepare() re-derives the coefficients). The low shelf
        // (<= 500 Hz) and the bell (<= 8 kHz) sit under the shared ceiling at
        // every rate the engine runs, so they keep it.
        for (int ch = 0; ch < numCh_; ++ch) {
            low_[ch].setLowShelf(sr_, p_.lowFreq, kShelfQ, p_.lowGainDb);
            mid_[ch].setPeaking(sr_, p_.midFreq, p_.midQ, p_.midGainDb);
            high_[ch].setHighShelf(sr_, p_.highFreq, kShelfQ, p_.highGainDb, /*nearNyquist*/ true);
        }
        dirty_ = false;
    }
    void clamp() {
        p_.lowFreq      = clampf(p_.lowFreq, 20.0f, 500.0f);
        p_.lowGainDb    = clampf(p_.lowGainDb, -18.0f, 18.0f);
        p_.midFreq      = clampf(p_.midFreq, 100.0f, 8000.0f);
        p_.midQ         = clampf(p_.midQ, 0.3f, 8.0f);
        p_.midGainDb    = clampf(p_.midGainDb, -18.0f, 18.0f);
        p_.highFreq     = clampf(p_.highFreq, 2000.0f, 20000.0f);
        p_.highGainDb   = clampf(p_.highGainDb, -18.0f, 18.0f);
        p_.outputTrimDb = clampf(p_.outputTrimDb, -24.0f, 24.0f);
    }
    static bool same(const EqParams& a, const EqParams& b) {
        return a.lowFreq == b.lowFreq && a.lowGainDb == b.lowGainDb &&
               a.midFreq == b.midFreq && a.midQ == b.midQ && a.midGainDb == b.midGainDb &&
               a.highFreq == b.highFreq && a.highGainDb == b.highGainDb &&
               a.outputTrimDb == b.outputTrimDb;
    }
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static constexpr double kShelfQ = 0.70710678118654752; // 1/sqrt(2)
    // 10^(24/20): the widest the trim can be, used to set the slew rate.
    static constexpr double kTrimMaxLin = 15.848931924611133;

    EqParams p_{};
    std::vector<Biquad> low_, mid_, high_;
    double sr_ = 48000.0;
    int numCh_ = 1;
    float trimLin_ = 1.0f;   // target
    float trimCur_ = 1.0f;   // where the ramp has got to
    float trimSlew_ = 1.0f;  // max linear change per sample
    bool dirty_ = true;
};

} // namespace dcdsp
