// dcdsp_envfilter.h
// ---------------------------------------------------------------------------
// Envelope filter (funk auto-wah) for the DotCharter native DSP core.
//
// PURE header-only. namespace dcdsp. C++17.
// Dependencies: <cmath> <vector> <array> <algorithm> <cstddef> <cstdint> only.
//
// RT-safety contract:
//   * All allocation happens in prepare().
//   * process() never allocates, locks, or throws.
//   * Output is ALWAYS finite and bounded: |x| < 32 (guaranteed by a final
//     safety clamp; feedback state is scrubbed if it ever goes non-finite).
//
// Effect: an envelope follower tracks the input level and sweeps a resonant
// StateVariableTPT band-pass between rangeLoHz and rangeHiHz. `direction`
// flips the sweep (louder -> up, or louder -> down). The TPT topology is
// unconditionally stable under audio-rate cutoff modulation, so the sweep can
// move every sample without blowing up. More vocal/funky than a plain auto-wah
// thanks to the resonant, normalized band-pass.
//
// This header uses the SHARED primitives (dcdsp::EnvelopeFollower,
// dcdsp::StateVariableTPT) directly — it includes dcdsp_primitives.h
// unconditionally and carries no dcdsp::detail stubs. (It used to describe a
// standalone-stub arrangement that no longer exists, and told the next reader
// to do a swap that was already done.)
// ---------------------------------------------------------------------------
#pragma once

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared engine primitives (real EnvelopeFollower + StateVariableTPT).
#include "dcdsp_primitives.h"

namespace dcdsp {

// Uses the shared engine primitives (real dcdsp::EnvelopeFollower and
// dcdsp::StateVariableTPT).

// -------------------------------- params -----------------------------------
struct EnvFilterParams {
  // How strongly the input level drives the sweep. [0, 1] default 0.5
  float sensitivity = 0.5f;
  // Low end of the sweep (band-pass centre when quiet, up-direction). Hz.
  // Clamped to [20, 18000] by setParams; default 200. (This said [100, 800],
  // which is neither the core's clamp nor the catalogue row.)
  float rangeLoHz = 200.0f;
  // High end of the sweep (band-pass centre when loud, up-direction). Hz.
  // Clamped to [20, 18000]; if hi < lo the two are SWAPPED. Default 2500.
  float rangeHiHz = 2500.0f;
  // Filter resonance / Q. [0, 1] default 0.7
  float resonance = 0.7f;
  // Sweep direction blend: 0 = up (louder -> higher), 1 = down (louder ->
  // lower). [0, 1] default 0
  float direction = 0.0f;
  // Dry/wet blend. 0 = dry, 1 = wet. [0, 1] default 1
  float mix = 1.0f;
};

// --------------------------------- core ------------------------------------
class EnvFilterCore {
 public:
  void prepare(double sr, int maxBlock, int nch) {
    sr_       = (sr > 0.0) ? sr : 48000.0;
    (void) maxBlock;   // no per-block scratch: nothing here is sized by it
    nch_      = (nch > 0) ? nch : 1;

    env_.prepare(sr_);
    env_.setTimes(5.0f, 80.0f);          // 5 ms attack, 80 ms release
    filters_.assign(static_cast<std::size_t>(nch_), StateVariableTPT{});
    // `mix` RAMPS (kMixRampSeconds); as a per-block constant a mix drag stepped
    // a full dry/wet crossfade at every block edge.
    mixSmooth_.reset(sr_, kMixRampSeconds);
    mixSmooth_.setTarget(params_.mix);
    mixSmooth_.snapToTarget();   // a freshly prepared filter starts AT its mix
    resSmooth_.reset(sr_, kMixRampSeconds);
    resSmooth_.setTarget(params_.resonance);
    resSmooth_.snapToTarget();   // ...and AT its resonance
    reset();
  }

  void reset() {
    env_.reset();
    for (auto& f : filters_) f.reset();
  }

  void setParams(const EnvFilterParams& p) {
    params_ = p;
    // Sanitize once, off the hot path.
    params_.sensitivity = clamp(params_.sensitivity, 0.0f, 1.0f);
    params_.rangeLoHz   = clamp(params_.rangeLoHz, 20.0f, 18000.0f);
    params_.rangeHiHz   = clamp(params_.rangeHiHz, 20.0f, 18000.0f);
    if (params_.rangeHiHz < params_.rangeLoHz)
      std::swap(params_.rangeHiHz, params_.rangeLoHz);
    params_.resonance = clamp(params_.resonance, 0.0f, 1.0f);
    resSmooth_.setTarget(params_.resonance);
    params_.direction = clamp(params_.direction, 0.0f, 1.0f);
    params_.mix       = clamp(params_.mix, 0.0f, 1.0f);
    mixSmooth_.setTarget(params_.mix);
  }

  // In-place processing. `ch` is an array of nch channel pointers.
  void process(float* const* ch, int nch, int nframes) {
    if (ch == nullptr || nframes <= 0) return;
    const int C = std::min(nch, static_cast<int>(filters_.size()));
    if (C <= 0) return;

    const double lo  = params_.rangeLoHz;
    const double hi  = params_.rangeHiHz;
    const double span = hi - lo;
    // Q is walked PER FRAME below. The wet is `bp() / Q`, so this knob is an
    // OUTPUT multiply of 1.43 -> 0.13 (20 dB) as well as a filter coefficient,
    // and it sat one line from the `mix` smoother that exists for less.
    const float  dir = params_.direction;                 // 0 up .. 1 down
    const float  sensGain = params_.sensitivity * 12.0f;  // envelope drive

    for (int n = 0; n < nframes; ++n) {
      const float mix  = mixSmooth_.next();
      const float dryG = 1.0f - mix;
      const double Q   = 0.7 + static_cast<double>(resSmooth_.next()) * 7.0; // 0.7 .. 7.7

      // Mono detector so all channels sweep coherently.
      float mono = 0.0f;
      for (int c = 0; c < C; ++c) mono += ch[c][n];
      mono *= (1.0f / static_cast<float>(C));

      const float env   = env_.process(mono);
      const float drive = env * sensGain;
      // Soft-saturate to a bounded 0..1 modulation amount.
      float mod = drive / (1.0f + drive);
      if (!std::isfinite(mod)) mod = 0.0f;
      mod = clamp(mod, 0.0f, 1.0f);

      // Up sweep: lo -> hi. Down sweep: hi -> lo. Blend by direction.
      const double cutUp   = lo + static_cast<double>(mod) * span;
      const double cutDown = hi - static_cast<double>(mod) * span;
      double cutoff = cutUp + static_cast<double>(dir) * (cutDown - cutUp);

      for (int c = 0; c < C; ++c) {
        const float dry = ch[c][n];
        // Real StateVariableTPT: set() then process(), read normalized band-pass
        // (k * bp, k = 1/Q) so the resonant peak stays ~unity gain.
        // `cutoff` and Q do not depend on the channel, so the DESIGN (a std::tan
        // plus a divide) is done once and copied; each channel keeps its own
        // STATE. Bit-identical to calling set() per channel.
        StateVariableTPT& f = filters_[static_cast<std::size_t>(c)];
        if (c == 0) f.set(sr_, cutoff, Q);
        else        f.copyCoeffsFrom(filters_[0]);
        f.process(dry);
        float wet = static_cast<float>(f.bp() / Q);
        if (!std::isfinite(wet)) { wet = 0.0f; f.reset(); }
        float out = dryG * dry + mix * wet;
        // Final safety clamp: output is always finite and |out| < 32.
        if (!std::isfinite(out)) out = 0.0f;
        out = clamp(out, -24.0f, 24.0f);
        ch[c][n] = out;
      }
    }
  }

  int latencySamples() const { return 0; }

 private:
  static float clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  double sr_       = 48000.0;
  int    nch_      = 1;

  EnvFilterParams params_{};
  static constexpr float kMixRampSeconds = 0.010f;
  SmoothedValue mixSmooth_;
  SmoothedValue resSmooth_;   // params_.resonance stays the TARGET
  EnvelopeFollower env_{};
  std::vector<StateVariableTPT> filters_{};
};

}  // namespace dcdsp
