// dcdsp_univibe.h
// UNI-VIBE: photocell-style pitch-vibrato / chorus.
//
// Four STAGGERED (unequal-spaced) first-order allpass stages swept by a
// throbbing, slightly-asymmetric LFO. This is the classic Uni-Vibe voicing --
// distinct from a phaser: the stages sit at wide, non-harmonically-related
// centres and are swept together, producing a warbly phase/pitch wobble rather
// than the evenly-spaced comb notches of a phaser.
//
// Chorus mode blends wet + dry (governed by `mix`); vibrato mode is 100% wet.
//
// PURE header-only. namespace dcdsp. C++17. Deps only:
//   <cmath> <vector> <array> <algorithm> <cstddef> <cstdint>
//
// RT-safe: all allocation happens in prepare(); process() never allocates,
// locks, or throws. Output is ALWAYS finite and bounded (|x| < 32) -- every
// feedback path uses a bounded (|coeff| < 1) allpass and there is a final
// safety clamp on the way out.
//
// The DotCharter native engine ships a shared primitives header
// (Biquad[double coeffs], OnePole, StateVariableTPT, EnvelopeFollower, Lfo,
// InterpolatedDelayLine, DcBlocker, SmoothedValue, adaaTanh). This core is
// written against those signatures conceptually; for a STANDALONE compile it
// carries a minimal local stub (dcdsp::detail) of just the primitives it uses.

#ifndef DCDSP_UNIVIBE_H
#define DCDSP_UNIVIBE_H

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared engine primitives (real SmoothedValue + DcBlocker). No local stubs.
#include "dcdsp_primitives.h"

namespace dcdsp {

// ---------------------------------------------------------------------------
// The core's own modulated first-order allpass stage (per-sample coefficient)
// lives in a per-core namespace so it cannot clash with other cores' helpers.
// ---------------------------------------------------------------------------
namespace univibe_detail {

// First-order allpass, transposed-DF-II, single state. Coefficient c must
// satisfy |c| < 1 for stability; transfer H(z) = (c + z^-1) / (1 + c z^-1),
// unity magnitude -> the wet signal cannot grow.
class Allpass1 {
 public:
  void reset() { s_ = 0.0f; }
  // c: bounded coefficient in (-1, 1).
  inline float process(float x, float c) {
    const float y = c * x + s_;
    s_ = x - c * y;
    return y;
  }

 private:
  float s_ = 0.0f;
};

}  // namespace univibe_detail

// ---------------------------------------------------------------------------
// Parameters. Each field documents [min, max, default].
// ---------------------------------------------------------------------------
struct UniVibeParams {
  // LFO speed of the throb, in Hz.                 [0.1, 8.0, 2.0]
  float rateHz = 2.0f;
  // Sweep intensity (photocell excursion). 0 = static, 1 = full.
  //                                                [0.0, 1.0, 0.7]
  float depth = 0.7f;
  // Voicing morph: 0 = chorus (wet+dry blend), 1 = vibrato (100% wet).
  //                                                [0.0, 1.0, 0.0]
  float mode = 0.0f;
  // Wet/dry balance used in CHORUS voicing (ignored as you morph to vibrato).
  // Default 0.5 so the default chorus voicing blends dry (not 100% wet).
  //                                                [0.0, 1.0, 0.5]
  float mix = 0.5f;
};

// ---------------------------------------------------------------------------
// UniVibeCore
// ---------------------------------------------------------------------------
class UniVibeCore {
 public:
  void prepare(double sr, int maxBlock, int nch) {
    sr_ = (sr > 0.0) ? sr : 48000.0;
    (void) maxBlock;   // no per-block scratch: nothing here is sized by it
    nch_ = (nch > 0) ? nch : 1;

    chans_.assign(static_cast<std::size_t>(nch_), ChannelState{});
    for (auto& c : chans_) {
      for (auto& ap : c.ap) ap.reset();
      c.dc.prepare(sr_);
      c.dc.reset();
    }

    rateSm_.reset(sr_, 0.040f);
    depthSm_.reset(sr_, 0.040f);
    modeSm_.reset(sr_, 0.040f);
    mixSm_.reset(sr_, 0.040f);

    setParams(params_);
    rateSm_.setCurrentAndTarget(params_.rateHz);
    depthSm_.setCurrentAndTarget(params_.depth);
    modeSm_.setCurrentAndTarget(params_.mode);
    mixSm_.setCurrentAndTarget(params_.mix);

    phase_ = 0.0;
  }

  void reset() {
    for (auto& c : chans_) {
      for (auto& ap : c.ap) ap.reset();
      c.dc.reset();
    }
    phase_ = 0.0;
    rateSm_.setCurrentAndTarget(params_.rateHz);
    depthSm_.setCurrentAndTarget(params_.depth);
    modeSm_.setCurrentAndTarget(params_.mode);
    mixSm_.setCurrentAndTarget(params_.mix);
  }

  void setParams(const UniVibeParams& p) {
    params_.rateHz = clampf(p.rateHz, 0.1f, 8.0f);
    params_.depth = clampf(p.depth, 0.0f, 1.0f);
    params_.mode = clampf(p.mode, 0.0f, 1.0f);
    params_.mix = clampf(p.mix, 0.0f, 1.0f);
    rateSm_.setTarget(params_.rateHz);
    depthSm_.setTarget(params_.depth);
    modeSm_.setTarget(params_.mode);
    mixSm_.setTarget(params_.mix);
  }

  // IN-PLACE. ch[c] points at nframes floats for channel c.
  void process(float* const* ch, int nch, int nframes) {
    if (ch == nullptr || nch <= 0 || nframes <= 0) return;
    const int cN = std::min(nch, nch_);

    for (int n = 0; n < nframes; ++n) {
      // --- advance the throbbing, slightly asymmetric LFO (shared, mono) ---
      const float rate = rateSm_.next();
      const float depth = depthSm_.next();
      const float mode = modeSm_.next();
      const float mix = mixSm_.next();

      const float lfo = asymLfo(static_cast<float>(phase_));  // [-1, 1]
      phase_ += static_cast<double>(rate) / sr_;
      if (phase_ >= 1.0) phase_ -= std::floor(phase_);

      // Effective wet amount: chorus uses `mix`, vibrato forces full wet.
      // wet = mix + mode * (1 - mix)  -> mode=0 => mix, mode=1 => 1.
      const float wetAmt = mix + mode * (1.0f - mix);
      const float dryAmt = 1.0f - wetAmt;

      // Precompute the four bounded allpass coefficients for this sample.
      // Each staggered stage centre is swept multiplicatively by the LFO.
      // `octaves` does not depend on the stage, so the multiplier is ONE
      // fastPow2 per sample, not four identical ones. Bit-identical output.
      const float octaves = kSweepOctaves * depth * lfo;  // +/- excursion
      const float sweep   = fastPow2(octaves);
      float coeff[kStages];
      for (int s = 0; s < kStages; ++s) {
        float fc = kBaseHz[s] * sweep;
        fc = clampf(fc, 20.0f, static_cast<float>(0.45 * sr_));
        coeff[s] = allpassCoeff(fc);
      }

      for (int c = 0; c < cN; ++c) {
        ChannelState& st = chans_[static_cast<std::size_t>(c)];
        const float dry = ch[c][n];

        float w = dry;
        for (int s = 0; s < kStages; ++s) w = st.ap[s].process(w, coeff[s]);
        w = st.dc.process(w);  // strip any DC the cascade could accrue

        float out = dryAmt * dry + wetAmt * w;

        // Final safety clamp -- guarantees finite + |x| < 32 no matter what.
        if (!std::isfinite(out)) out = 0.0f;
        out = clampf(out, -32.0f, 32.0f);
        ch[c][n] = out;
      }
    }
  }

  int latencySamples() const { return 0; }

 private:
  static constexpr int kStages = 4;
  // Staggered / unequal-spaced stage centres (Hz). NOT harmonically related --
  // this wide, irregular spacing is what gives the vibe its warble vs a phaser.
  static constexpr float kBaseHz[kStages] = {60.0f, 200.0f, 750.0f, 2200.0f};
  // Peak sweep excursion in octaves at depth = 1, |lfo| = 1.
  static constexpr float kSweepOctaves = 1.35f;
  // Phase skew for the asymmetric photocell throb (rise faster than fall).
  static constexpr float kSkew = 0.42f;

  struct ChannelState {
    std::array<univibe_detail::Allpass1, kStages> ap;
    DcBlocker dc;
  };

  static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  // Tunable first-order allpass coefficient with 90-deg phase at fc.
  // c = (t - 1)/(t + 1), t = tan(pi*fc/sr). |c| < 1 for fc in (0, sr/2).
  inline float allpassCoeff(float fc) const {
    const double t = std::tan(3.14159265358979 * static_cast<double>(fc) / sr_);
    double c = (t - 1.0) / (t + 1.0);
    if (c > 0.9995) c = 0.9995;      // hard stability guard
    if (c < -0.9995) c = -0.9995;
    return static_cast<float>(c);
  }

  // Slightly-asymmetric bipolar LFO in [-1, 1]. A phasor is skewed so the rise
  // is quicker than the fall (photocell lamp warm-up vs cool-down), then shaped
  // by a sine. Bounded by construction.
  static inline float asymLfo(float p) {
    // p in [0, 1). Warp to p' so the waveform is asymmetric about its peak.
    float pw;
    if (p < kSkew)
      pw = 0.5f * (p / kSkew);
    else
      pw = 0.5f + 0.5f * ((p - kSkew) / (1.0f - kSkew));
    return std::sin(6.28318530717959f * pw);
  }

  // 2^x via std::exp2 (cheap, finite for the bounded x range we feed it).
  static inline float fastPow2(float x) { return std::exp2(x); }

  double sr_ = 48000.0;
  int nch_ = 1;
  double phase_ = 0.0;

  UniVibeParams params_{};
  SmoothedValue rateSm_, depthSm_, modeSm_, mixSm_;
  std::vector<ChannelState> chans_;
};

// out-of-line storage for the constexpr array (C++17 friendliness)
inline constexpr float UniVibeCore::kBaseHz[];

}  // namespace dcdsp

#endif  // DCDSP_UNIVIBE_H
