// dcdsp_vocoder.h — CHANNEL VOCODER ("vocoder").
//
// Models the classic analogue channel vocoder (EMS / Roland VP-330 family): the
// MODULATOR (this module's own input — your guitar) is split by a bank of
// log-spaced bandpass filters, each band's level is tracked by an envelope
// follower, and those envelopes are imposed on the same bands of a CARRIER (an
// internal saw / pulse / noise oscillator at a fixed pitch, or an external
// sidechain key). There is no pitch tracking anywhere: the internal carrier runs
// at `pitchHz` and nothing follows the input.
//
// Topology: 8..24 bands, 80 Hz .. 8 kHz, 4th-order (two cascaded RBJ bandpass
// sections) on both the analysis and the synthesis side, constant-Q derived from
// the band spacing. The Q is the standard sqrt(r)/(r-1) that puts ONE section at
// -3 dB at the geometric midpoint between neighbours; the bank runs TWO of them
// in cascade, so each band is -6 dB there and the coverage ripples rather than
// summing flat. That is deliberate - a 4th-order bank is what makes the bands
// distinct enough to read as a vocoder rather than as a filtered mush - and the
// ripple is MEASURED, not assumed: see
// GearFilterTests.Vocoder_BandCoverageRipples_ByTheAmountItsFourthOrderBankImplies. Each band divides by a slow envelope of its own CARRIER
// content before multiplying by the modulator's, so the output level follows the
// player rather than the oscillator's spectral tilt. The whole effect is MONO —
// one analysis, one synthesis, the same wet mixed against each channel's dry.
//
// Easy to get wrong: changing the band COUNT retunes every filter at once, which
// is a full-scale discontinuity in every band. Two complete banks are held and a
// count change configures the idle one and CROSSFADES over 10 ms; a swap
// requested while a fade is still running is deferred to the callback that sees
// the fade land, so the pending count is never applied under an in-flight fade.
// `formantShift` is the SECOND caller of that mechanism and was the reason it
// was under-used: it re-designs all 24 SYNTHESIS sections, at Q up to 24, by up
// to a full octave, with their TDF-II state retained - the same discontinuity a
// count change makes, on a knob a user drags. It goes through the same two-bank
// crossfade.
// The third trap is COST, not sound: `setParams` used to re-design the whole
// bank on EVERY param write, `mix` included - ~300 transcendentals per write on
// the audio thread. The design half now runs only when `bands` or
// `formantShift` moved (and then on the IDLE bank), and the follower half only
// when `attackMs` / `releaseMs` did.
// The fourth: `carrier` selects between four full-scale waveforms. Saw and
// Pulse differ by 1.7 at phase 0, so the switch is a splice unless the outgoing
// and incoming waveforms are evaluated from the SAME phase and crossfaded -
// dcdsp_ringmod.h's Lfo::peek/advance shape, spelled here as "every candidate
// is computed every sample and two of them are blended".
#pragma once

#include "dcdsp_primitives.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dcdsp {

struct VocoderParams {
    // 8 .. 24 in steps of 4. ORDINAL discrete.
    int   bands = 16;
    // 0 = Sidechain (the KEY is the carrier, this module's input is the
    // modulator), 1 = Saw, 2 = Pulse, 3 = Noise. NAMED discrete.
    int   carrier = 1;
    float pitchHz = 110.0f;     // 30 .. 500; the internal oscillators only
    float attackMs = 5.0f;      // 1 .. 100
    float releaseMs = 40.0f;    // 5 .. 500
    // -12 .. +12 semitones between the ANALYSIS centre and the SYNTHESIS centre
    // of each band: the vocal-tract size control.
    float formantShift = 0.0f;
    // 0 .. 1. Pre-emphasis on the modulator (a high shelf at 2 kHz, up to
    // +12 dB), which is what makes consonants legible.
    float emphasis = 0.5f;
    float mix = 1.0f;
};

class VocoderCore {
public:
    static constexpr int kMaxBands = 24;
    static constexpr int kMinBands = 8;
    static constexpr int kCarrierSidechain = 0;
    static constexpr int kCarrierSaw       = 1;
    static constexpr int kCarrierPulse     = 2;
    static constexpr int kCarrierNoise     = 3;
    static constexpr float kLowHz  = 80.0f;
    static constexpr float kHighHz = 8000.0f;

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = (maxBlockSize > 0) ? maxBlockSize : 1;
        nch_      = (numChannels > 0) ? numChannels : 1;

        mixSmooth_.reset(sr_, kRampSeconds);
        mixSmooth_.setTarget(params_.mix);
        mixSmooth_.snapToTarget();
        bankFade_.reset(sr_, kRampSeconds);
        bankFade_.setCurrentAndTarget(1.0f);

        // Everything below is rate-owned and must be re-derived here: the
        // filter coefficients, the followers' time constants and the carrier
        // oscillator's phase increment.
        active_ = 0;
        configureBank(bank_[0], quantiseBands(params_.bands), params_.formantShift);
        configureBank(bank_[1], quantiseBands(params_.bands), params_.formantShift);
        applyEmphasis();
        carrierInc_ = static_cast<float>(static_cast<double>(params_.pitchHz) / sr_);
        lastAttackMs_  = params_.attackMs;
        lastReleaseMs_ = params_.releaseMs;
        curCarrier_    = params_.carrier;
        prevCarrier_   = params_.carrier;
        carrierFade_.reset(sr_, kRampSeconds);
        carrierFade_.setCurrentAndTarget(1.0f);
        reset();
    }

    void reset() {
        for (auto& b : bank_) resetBank(b);
        emphasis_.reset();
        carrierPhase_ = 0.0f;
        rng_ = 22222u;
    }

    void setParams(const VocoderParams& p) {
        params_.bands        = quantiseBands(p.bands);
        params_.carrier      = clampi(p.carrier, 0, 3);
        params_.pitchHz      = clampf(p.pitchHz, 30.0f, 500.0f);
        params_.attackMs     = clampf(p.attackMs, 1.0f, 100.0f);
        params_.releaseMs    = clampf(p.releaseMs, 5.0f, 500.0f);
        // A NaN lands on the NEUTRAL value, never on `lo`: formantShift's
        // neutral is the MIDDLE of its range, and a rejected NaN must not be a
        // full octave down.
        params_.formantShift = clampf(p.formantShift, -12.0f, 12.0f, 0.0f);
        params_.emphasis     = clampf(p.emphasis, 0.0f, 1.0f);
        params_.mix          = clampf(p.mix, 0.0f, 1.0f);

        mixSmooth_.setTarget(params_.mix);
        carrierInc_ = static_cast<float>(static_cast<double>(params_.pitchHz) / sr_);
        applyEmphasis();

        // A carrier WAVEFORM change is a full-scale discontinuity, so it is
        // ADOPTED on the callback and crossfaded there - the same deferral the
        // band count uses, and for the same reason: restarting a fade that is
        // still in flight would splice inside the mechanism that exists to
        // prevent splices, and a three-way blend is not representable with one
        // weight.

        // Only the half whose inputs moved. The DESIGN half (band centres, Q,
        // the formant shift) is a discontinuity, so it goes through the two-bank
        // crossfade in process() rather than retuning the live bank under the
        // callback; the FOLLOWER half is a time constant, which is continuous by
        // construction and may be written in place.
        if (params_.attackMs != lastAttackMs_ || params_.releaseMs != lastReleaseMs_) {
            lastAttackMs_  = params_.attackMs;
            lastReleaseMs_ = params_.releaseMs;
            applyFollowerTimes(bank_[active_]);
            applyFollowerTimes(bank_[1 - active_]);
        }
    }

    // Keyed form. `key` may be null (the unkeyed overload below forwards that
    // way), in which case a Sidechain carrier falls back to this module's OWN
    // input — the engine's contract for an unavailable key — which makes the
    // module a near-wire rather than silencing it.
    void process(float* const* channels, int numChannels, int numFrames,
                 const float* const* key, int keyChannels) {
        if (channels == nullptr || numFrames <= 0 || numChannels <= 0) return;
        if (numFrames > maxBlock_) return;     // skipped WHOLE
        const int nch = std::min(numChannels, nch_);
        if (nch <= 0) return;

        // A pending band count is adopted HERE, once the previous crossfade has
        // landed, so two fades can never overlap.
        if (!bankFade_.isRamping()
            && (bank_[active_].nBands != params_.bands
                || bank_[active_].shift != params_.formantShift)) {
            const int idle = 1 - active_;
            configureBank(bank_[idle], params_.bands, params_.formantShift);
            active_ = idle;
            bankFade_.setCurrentAndTarget(0.0f);
            bankFade_.setTarget(1.0f);
        }

        // A pending CARRIER waveform is adopted the same way, once its own
        // crossfade has landed.
        if (!carrierFade_.isRamping() && curCarrier_ != params_.carrier) {
            prevCarrier_ = curCarrier_;
            curCarrier_  = params_.carrier;
            carrierFade_.setCurrentAndTarget(0.0f);
            carrierFade_.setTarget(1.0f);
        }

        Bank& live = bank_[active_];
        Bank& old  = bank_[1 - active_];
        const int keyCh = (key != nullptr) ? keyChannels : 0;

        for (int n = 0; n < numFrames; ++n) {
            const float mix  = mixSmooth_.next();
            const float dryG = 1.0f - mix;
            const float fade = bankFade_.next();

            // --- the modulator: mono sum of this module's own input ----------
            float mod = 0.0f;
            for (int c = 0; c < nch; ++c) mod += channels[c][n];
            mod *= 1.0f / static_cast<float>(nch);
            if (!std::isfinite(mod)) mod = 0.0f;
            float shaped = emphasis_.process(mod);
            if (!std::isfinite(shaped)) { shaped = 0.0f; emphasis_.reset(); }

            // --- the carrier -------------------------------------------------
            // Every candidate is evaluated from the SAME phase, so the switch is
            // a crossfade between two continuous waveforms rather than a splice.
            // The noise generator is stepped unconditionally for the same
            // reason: a stream that only advances while Noise is selected
            // restarts from wherever it stopped when the knob comes back.
            float sidechain;
            if (keyCh > 0) {
                sidechain = 0.0f;
                for (int c = 0; c < keyCh; ++c) sidechain += key[c][n];
                sidechain *= 1.0f / static_cast<float>(keyCh);
            } else {
                sidechain = mod;   // unavailable key -> own input, never silence
            }
            const float noise = nextNoise();
            const float saw   = 2.0f * carrierPhase_ - 1.0f;
            const float pulse = (carrierPhase_ < 0.5f) ? 0.7f : -0.7f;

            const float cw = carrierFade_.next();
            float carrier = carrierOf(curCarrier_, sidechain, saw, pulse, noise);
            if (cw < 1.0f) {
                const float outgoing = carrierOf(prevCarrier_, sidechain, saw, pulse, noise);
                carrier = cw * carrier + (1.0f - cw) * outgoing;
            }
            if (!std::isfinite(carrier)) carrier = 0.0f;
            carrierPhase_ += carrierInc_;
            if (carrierPhase_ >= 1.0f) carrierPhase_ -= std::floor(carrierPhase_);

            float wet = fade * bankOutput(live, shaped, carrier);
            if (fade < 1.0f) wet += (1.0f - fade) * bankOutput(old, shaped, carrier);
            if (!std::isfinite(wet)) wet = 0.0f;
            wet = clampf(wet * kMakeup, -8.0f, 8.0f);

            for (int c = 0; c < nch; ++c) {
                float x = channels[c][n];
                if (!std::isfinite(x)) x = 0.0f;
                float y = dryG * x + mix * wet;
                if (!std::isfinite(y)) y = 0.0f;
                channels[c][n] = clampf(y, -24.0f, 24.0f);
            }
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        process(channels, numChannels, numFrames, nullptr, 0);
    }

    int latencySamples() const { return 0; }

private:
    struct Band {
        Biquad ana1, ana2;          // 4th-order analysis bandpass
        Biquad syn1, syn2;          // 4th-order synthesis bandpass
        EnvelopeFollower modEnv;    // follows the modulator in this band
        EnvelopeFollower carEnv;    // normalises the carrier's own band level
    };

    struct Bank {
        int   nBands = 16;
        float shift  = 0.0f;   // the formantShift this bank was DESIGNED for
        std::array<Band, kMaxBands> bands{};
    };

    static float carrierOf(int type, float sidechain, float saw, float pulse, float noise) {
        switch (type) {
            case kCarrierSidechain: return sidechain;
            case kCarrierPulse:     return pulse;
            case kCarrierNoise:     return noise;
            default:                return saw;
        }
    }

    // One sample through one bank. Kept out of process() so the crossfade runs
    // both banks through the identical path.
    float bankOutput(Bank& b, float modulator, float carrier) {
        float sum = 0.0f;
        for (int i = 0; i < b.nBands; ++i) {
            Band& band = b.bands[static_cast<std::size_t>(i)];
            const float m = band.ana2.process(band.ana1.process(modulator));
            const float c = band.syn2.process(band.syn1.process(carrier));
            const float me = band.modEnv.process(m);
            const float ce = band.carEnv.process(c);
            // Divide out the carrier's own band level so the output follows the
            // PLAYER; the floor is what stops a silent carrier band from
            // amplifying its own numerical dirt.
            const float norm = (ce > kCarrierFloor) ? (1.0f / ce) : 0.0f;
            sum += c * norm * me;
        }
        return std::isfinite(sum) ? sum : 0.0f;
    }

    void configureBank(Bank& b, int nBands, float formantShift) {
        b.nBands = nBands;
        b.shift  = formantShift;
        designBank(b);
        applyFollowerTimes(b);
        resetBank(b);
    }

    // The DESIGN half: band centres, Q and the formant shift. Only `bands` and
    // `formantShift` reach it, and only on an IDLE bank.
    void designBank(Bank& b) {
        const int n = b.nBands;
        const double ratio = (n > 1)
            ? std::pow(static_cast<double>(kHighHz) / kLowHz, 1.0 / static_cast<double>(n - 1))
            : 1.0;
        // Constant-Q from the band spacing: ONE section is -3 dB at the
        // geometric midpoint between neighbouring centres. The bank cascades
        // two of them, so a band is -6 dB there and the coverage ripples; see
        // the header.
        double Q = (ratio > 1.0000001) ? std::sqrt(ratio) / (ratio - 1.0) : 4.0;
        if (Q < 1.0) Q = 1.0;
        if (Q > 24.0) Q = 24.0;
        const double shift = std::pow(2.0, static_cast<double>(b.shift) / 12.0);

        for (int i = 0; i < n; ++i) {
            const double fc = static_cast<double>(kLowHz) * std::pow(ratio, static_cast<double>(i));
            Band& band = b.bands[static_cast<std::size_t>(i)];
            band.ana1.setBandpass(sr_, fc, Q);
            band.ana2.setBandpass(sr_, fc, Q);
            band.syn1.setBandpass(sr_, fc * shift, Q);
            band.syn2.setBandpass(sr_, fc * shift, Q);
        }
    }

    // The FOLLOWER half: time constants only. Continuous by construction, so
    // this one may be written to a LIVE bank.
    void applyFollowerTimes(Bank& b) {
        for (int i = 0; i < b.nBands; ++i) {
            Band& band = b.bands[static_cast<std::size_t>(i)];
            band.modEnv.prepare(sr_);
            band.modEnv.setTimes(params_.attackMs, params_.releaseMs);
            band.carEnv.prepare(sr_);
            band.carEnv.setTimes(kCarrierEnvAttackMs, kCarrierEnvReleaseMs);
        }
    }

    static void resetBank(Bank& b) {
        for (auto& band : b.bands) {
            band.ana1.reset(); band.ana2.reset();
            band.syn1.reset(); band.syn2.reset();
            band.modEnv.reset(); band.carEnv.reset();
        }
    }

    void applyEmphasis() {
        emphasis_.setHighShelf(sr_, 2000.0, 0.707, static_cast<double>(params_.emphasis) * 12.0);
    }

    float nextNoise() {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return (static_cast<float>(rng_ & 0xFFFFFFu) * (1.0f / 8388608.0f) - 1.0f) * 0.5f;
    }

    static int quantiseBands(int v) {
        if (v < kMinBands) v = kMinBands;
        if (v > kMaxBands) v = kMaxBands;
        return kMinBands + ((v - kMinBands + 2) / 4) * 4;   // snap to the step of 4
    }
    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float clampf(float v, float lo, float hi) { return clampf(v, lo, hi, lo); }
    static float clampf(float v, float lo, float hi, float fallback) {
        if (v != v) return fallback;          // NaN
        if (v < lo) return lo;
        return (v > hi) ? hi : v;
    }

    static constexpr float kRampSeconds = 0.010f;
    static constexpr float kCarrierFloor = 1.0e-3f;
    static constexpr float kCarrierEnvAttackMs = 10.0f;
    static constexpr float kCarrierEnvReleaseMs = 60.0f;
    static constexpr float kMakeup = 1.6f;

    VocoderParams params_{};
    double sr_ = 48000.0;
    int    maxBlock_ = 1;
    int    nch_ = 1;
    Bank   bank_[2];
    int    active_ = 0;
    SmoothedValue bankFade_;
    SmoothedValue mixSmooth_;
    SmoothedValue carrierFade_;
    int    curCarrier_  = 1;
    int    prevCarrier_ = 1;
    float  lastAttackMs_  = 5.0f;
    float  lastReleaseMs_ = 40.0f;
    Biquad emphasis_;
    float  carrierPhase_ = 0.0f;
    float  carrierInc_ = 0.0f;
    std::uint32_t rng_ = 22222u;
};

}  // namespace dcdsp
