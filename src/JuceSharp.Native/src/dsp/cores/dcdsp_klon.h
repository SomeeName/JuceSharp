// dcdsp_klon.h
// -----------------------------------------------------------------------------
// KlonCore — a Klon-Centaur-style *transparent* overdrive.
//
// The defining Klon trick: the distorted (clipped) path is summed with a
// FULL-RANGE CLEAN path, so the bass/low-mids stay dry and articulate while the
// clipped path adds grind on top. This is deliberately DISTINCT from a
// Tube-Screamer-style overdrive (mid-humped band-pass into the clipper, no clean
// blend). Here: gentle *asymmetric* soft clip (adds even harmonics), a treble
// tilt (high shelf), and a clean/clipped blend.
//
// Like the real pedal's dual-gang gain pot, the clean/dirty proportion is
// GANGED to the gain knob: nearly all clean at min gain, mostly dirty at max.
// The `blend` param is a +-trim around that law (0.5 = stock law), not an
// absolute clean fraction — see cleanFractionFor().
//
// The clipper runs at 4x inside the shared elliptic OversamplerNx with an
// ADAA tanh stage. Because the wet path goes through the oversampler's filters
// while the clean path does not, the dry path is delayed by the oversampler's
// measured round-trip group delay (~3.4 samples, fractional, Lagrange
// interpolated) before the blend — otherwise the sum combs in the top octaves.
// That delay is the module latency reported by latencySamples().
//
// RT-safety contract:
//   * All allocation happens in prepare(); process() never allocates, locks or
//     throws.
//   * Output is ALWAYS finite and bounded (|x| < 32); every nonlinear path is
//     guarded and a final safety clamp backstops everything.
//
// Depends on the engine DSP primitives (dcdsp::Biquad [FLOAT coeffs and float
// state — dcdsp::BiquadD is the double one; klon's shelf is at 1.5 kHz, where
// float is correct],
// dcdsp::DcBlocker, dcdsp::SmoothedValue, dcdsp::InterpolatedDelayLine,
// dcdsp::adaaTanh) + dcdsp_detail_os.h (OversamplerNx, sanitize).
// -----------------------------------------------------------------------------
#ifndef DCDSP_KLON_H
#define DCDSP_KLON_H

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

namespace dcdsp {

// -----------------------------------------------------------------------------
// Parameters. All normalised 0..1 unless noted; ranges as [min, max, default].
// -----------------------------------------------------------------------------
struct KlonParams {
  // Drive amount into the clipper; also gangs the clean/dirty proportion
  // (dual-gang pot: min gain = nearly all clean, max = mostly dirty). [0, 1, 0.5]
  float gain = 0.5f;
  // Treble tilt of the wet/clipped path: 0.5 = flat, >0.5 boost, <0.5 cut
  // (high shelf, +/-12 dB). [0, 1, 0.5]
  float treble = 0.5f;
  // Output level trim. [0, 1, 0.6]
  float output = 0.6f;
  // Clean-blend TRIM around the gain-ganged law: 0.5 = stock dual-gang law,
  // 0 = -0.25 clean fraction, 1 = +0.25. [0, 1, 0.6]
  float blend = 0.6f;
};

class KlonCore {
 public:
  // ---- lifecycle ----------------------------------------------------------
  void prepare(double sampleRate, int maxBlock, int numChannels) {
    sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
    maxBlock_ = (maxBlock > 0) ? maxBlock : 512;
    nch_ = (numChannels > 0) ? numChannels : 2;

    chans_.assign(static_cast<size_t>(nch_), Channel{});
    for (auto& c : chans_) {
      c.os.prepare(sr_, maxBlock_, kOs);
      c.dc.prepare(sr_, 12.0);
      // Real dcdsp::Biquad::setHighShelf(sr, fc, Q, gainDb) — Q 3rd, gainDb 4th.
      c.treble.setHighShelf(sr_, kTrebleHz, kShelfQ, 0.0);
    }

    // Dry path alignment: delay the clean path by the oversampler's measured
    // wet-path group delay so the blend does not comb.
    dryDelaySamples_ = chans_.empty() ? 0.0
                     : chans_[0].os.groupDelayBaseSamples();
    if (!(dryDelaySamples_ >= 0.0)) dryDelaySamples_ = 0.0;   // NaN guard
    latency_ = (int)std::lround(dryDelaySamples_);
    const int maxDelay = (int)std::ceil(dryDelaySamples_) + 4;
    for (auto& c : chans_) c.dryDelay.prepare(maxDelay);

    driveSm_.reset(sr_, (float)kSmoothSec);
    cleanSm_.reset(sr_, (float)kSmoothSec);
    outSm_.reset(sr_, (float)kSmoothSec);

    driveBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
    cleanBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
    outBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
    dryBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);

    // Prime smoothers with the current param mapping and design the shelves.
    applyParamsImmediate();
    reset();
  }

  void reset() {
    for (auto& c : chans_) {
      c.os.reset();
      c.dc.reset();
      c.treble.reset();
      c.dryDelay.reset();
      c.adaaState = 0.f;
    }
  }

  void setParams(const KlonParams& p) {
    params_.gain   = clamp01(p.gain);
    params_.treble = clamp01(p.treble);
    params_.output = clamp01(p.output);
    params_.blend  = clamp01(p.blend);

    // Control-rate mapping.
    drive_    = 1.0 + params_.gain * 40.0;      // 1x .. 41x into the clipper
    cleanTgt_ = cleanFractionFor(params_.gain, params_.blend);
    outTgt_   = params_.output * 1.2;           // up to +1.6 dB-ish trim

    // Treble tilt: 0.5 -> 0 dB, 0/1 -> -/+12 dB.
    const double shelfDb = (static_cast<double>(params_.treble) - 0.5) * 24.0;
    for (auto& c : chans_)
      c.treble.setHighShelf(sr_, kTrebleHz, kShelfQ, shelfDb);
    driveSm_.setTarget(static_cast<float>(drive_));
    cleanSm_.setTarget(static_cast<float>(cleanTgt_));
    outSm_.setTarget(static_cast<float>(outTgt_));
  }

  // ---- audio --------------------------------------------------------------
  // In-place: ch[c] points at nframes floats for channel c.
  void process(float* const* ch, int nch, int nframes) {
    if (!ch || nframes <= 0) return;
    const int chCount = std::min(nch, static_cast<int>(chans_.size()));
    const int frames = std::min(nframes, maxBlock_);

    for (int n = 0; n < frames; ++n) {
      driveBuf_[n] = driveSm_.next();
      cleanBuf_[n] = cleanSm_.next();
      outBuf_[n]   = outSm_.next();
    }

    for (int c = 0; c < chCount; ++c) {
      float* buf = ch[c];
      if (!buf) continue;
      Channel& st = chans_[static_cast<size_t>(c)];

      // Split: keep the clean path, guard the wet input.
      for (int n = 0; n < frames; ++n) {
        float x = buf[n];
        if (!std::isfinite(x)) x = 0.f;
        dryBuf_[n] = x;
        buf[n] = x;
      }

      // --- clipped (wet) path, 4x oversampled ADAA soft clip -------------
      // Drive is applied per OS sample inside the wrap (base-rate drive +
      // clamp would clip at base rate before the oversampler can help).
      st.os.upsample(buf, frames);
      float* up = st.os.data();
      const int m = st.os.upSize(frames);
      for (int k = 0; k < m; ++k) {
        const float v = driveBuf_[k / kOs] * up[k];
        // Asymmetric soft clip: tanh shifted by kBias adds even harmonics;
        // subtracting tanh(kBias) recentres so DC stays small. ADAA of a
        // fixed function of (v + kBias), so the shared adaaTanh applies.
        const float s = adaaTanh(v + (float)kBias, st.adaaState) - (float)kTanhBias;
        up[k] = detail::sanitize(s);
      }
      st.os.downsample(buf, frames);

      for (int n = 0; n < frames; ++n) {
        // Treble tilt applies to the wet path (voices the grind).
        const double wet = st.treble.process(buf[n]);

        // Clean full-range path, time-aligned to the wet path.
        st.dryDelay.push(dryBuf_[n]);
        const double dry = st.dryDelay.readAt((float)dryDelaySamples_);

        // Klon blend: clean full-range + clipped.
        const double clean = cleanBuf_[n];
        double y = clean * dry + (1.0 - clean) * wet;

        // Output trim + DC removal (asymmetry injects DC).
        y = st.dc.process(static_cast<float>(y * outBuf_[n]));

        // Final safety: guarantee finite & bounded well under |x| < 32.
        if (!std::isfinite(y)) y = 0.0;
        y = std::min(std::max(y, -kSafe), kSafe);
        buf[n] = static_cast<float>(y);
      }
    }
  }

  // Both paths are aligned to the delayed dry path, so the module as a whole
  // is late by the (rounded) dry-compensation delay.
  int latencySamples() const { return latency_; }

 private:
  // ---- constants ----------------------------------------------------------
  static constexpr int    kOs         = 4;       // oversample factor
  static constexpr double kTrebleHz   = 1500.0;
  static constexpr double kShelfQ     = 0.7;     // real Biquad shelf Q (Butterworth-ish)
  static constexpr double kSmoothSec  = 0.010;   // 10 ms param smoothing ramp
  static constexpr double kBias       = 0.28;    // asymmetric clip offset
  static constexpr double kSafe       = 24.0;    // final clamp (< 32)

  static inline float clamp01(float v) {
    if (!(v == v)) return 0.0f;  // NaN guard
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  }

  // Dual-gang law: clean fraction slides from 0.9 (min gain) to 0.15 (max
  // gain); `blend` trims the law by +-0.25. At the catalogue defaults
  // (gain 0.5, blend 0.6) this lands at 0.575 — matching the old core's
  // absolute default blend of 0.6 within a couple of percent.
  static double cleanFractionFor(float gain, float blend) {
    const double base = 0.9 - 0.75 * static_cast<double>(gain);
    const double trimmed = base + (static_cast<double>(blend) - 0.5) * 0.5;
    return std::min(std::max(trimmed, 0.0), 1.0);
  }

  // ---- per-channel state --------------------------------------------------
  struct Channel {
    detail::OversamplerNx os;
    Biquad treble;                   // high-shelf tilt on the wet path
    DcBlocker dc;
    InterpolatedDelayLine dryDelay;  // clean-path alignment vs the wet path
    float adaaState = 0.f;
  };

  void applyParamsImmediate() {
    setParams(params_);
    driveSm_.setCurrentAndTarget(static_cast<float>(drive_));
    cleanSm_.setCurrentAndTarget(static_cast<float>(cleanTgt_));
    outSm_.setCurrentAndTarget(static_cast<float>(outTgt_));
  }

  // tanh(kBias) — recentres the shifted-tanh clipper (computed once).
  const double kTanhBias = std::tanh(kBias);

  double sr_ = 48000.0;
  int maxBlock_ = 512;
  int nch_ = 2;

  KlonParams params_{};
  double drive_ = 1.0;
  double cleanTgt_ = 0.575;
  double outTgt_ = 0.72;
  double dryDelaySamples_ = 0.0;
  int latency_ = 0;

  SmoothedValue driveSm_, cleanSm_, outSm_;
  std::vector<float> driveBuf_, cleanBuf_, outBuf_, dryBuf_;
  std::vector<Channel> chans_;
};

}  // namespace dcdsp

#endif  // DCDSP_KLON_H
