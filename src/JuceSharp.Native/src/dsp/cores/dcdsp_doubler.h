// dcdsp_doubler.h — Detune Doubler core (header-only, C++17)
//
// Two pitch-detuned voices (+/- detuneCents) panned apart by `spread` to
// thicken and widen a single guitar, with a small base delay offset.
//
// 0.16: the voices are CONSTANT-RATIO granular detuners, not LFO vibrato.
// The old sinusoidally-modulated tap produced a pitch that oscillated
// +/-detune around unity (mean ratio 1.0 — vibrato, not detune). Each voice
// now reads a linearly ramping tap (slope 1 - ratio per sample, ratio =
// 2^(+/-cents/1200)) through two grains half a window apart, crossfaded with
// a constant-power sin/cos envelope — the classic dual-tap pitch shifter, so
// the detune is a constant cents offset like a real doubler.
//
// RT-safety contract:
//   * All allocation happens in prepare(). reset()/setParams()/process() never
//     allocate, lock, or throw.
//   * process() is in-place and always produces finite, bounded output
//     (|x| < 32) — every read is index-clamped and a final safety clamp is
//     applied per sample.
//
// Dependencies: shared dcdsp primitives + <cmath> <vector> <array>
// <algorithm> <cstddef> <cstdint>.

#ifndef DCDSP_DOUBLER_H
#define DCDSP_DOUBLER_H

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared engine primitives (Biquad/OnePole/EnvelopeFollower/DcBlocker/Lfo/
// InterpolatedDelayLine/SmoothedValue/...). Real definitions — no local stubs.
#include "dcdsp_primitives.h"

// ---------------------------------------------------------------------------
// Doubler core
// ---------------------------------------------------------------------------
namespace dcdsp {

struct DoublerParams {
  // Pitch detune of each voice, in cents. Voice A = +cents, B = -cents.
  // [min=0, max=30, default=12]
  float detuneCents = 12.0f;
  // Stereo spread of the two voices. 0 = both centred (mono-ish), 1 = hard
  // apart (voice A left, voice B right). [min=0, max=1, default=0.7]
  float spread = 0.7f;
  // Base delay offset of the doubled voices, in milliseconds.
  // [min=2, max=40, default=12]
  float delayMs = 12.0f;
  // Dry/wet blend. 0 = dry only, 1 = doubled voices only.
  // [min=0, max=1, default=0.5]
  float mix = 0.5f;
};

class DoublerCore {
 public:
  void prepare(double sampleRate, int maxBlock, int numChannels) {
    (void)maxBlock;
    (void)numChannels;   // the grain geometry is channel-independent
    sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;

    grainSamples_ = static_cast<float>(kGrainMs * 0.001 * sr_);

    // Worst-case read distance: max base delay + grain window + margin.
    const double maxDelayMs = 40.0;
    const double marginMs = 4.0;
    int maxDelaySamples = static_cast<int>(
        std::ceil((maxDelayMs + kGrainMs + marginMs) * 0.001 * sr_)) + 4;

    for (auto& d : delays_) d.prepare(maxDelaySamples);

    smMix_.reset(sr_, 0.015f);
    smDelay_.reset(sr_, 0.020f);
    smGAL_.reset(sr_, 0.015f);
    smGAR_.reset(sr_, 0.015f);
    smGBL_.reset(sr_, 0.015f);
    smGBR_.reset(sr_, 0.015f);

    setParams(params_);
    // Snap smoothers to their targets on (re)prepare.
    smMix_.setCurrentAndTarget(mixTgt_);
    smDelay_.setCurrentAndTarget(baseDelayTgt_);
    smGAL_.setCurrentAndTarget(gALTgt_);
    smGAR_.setCurrentAndTarget(gARTgt_);
    smGBL_.setCurrentAndTarget(gBLTgt_);
    smGBR_.setCurrentAndTarget(gBRTgt_);

    reset();
  }

  void reset() {
    for (auto& d : delays_) d.reset();
    // Stagger the two voices' grain phases so their crossfade dips never line up.
    phaseA_ = 0.0f;
    phaseB_ = 0.25f;
    // Fresh state starts at the current param targets (no stale ramp).
    smMix_.setCurrentAndTarget(mixTgt_);
    smDelay_.setCurrentAndTarget(baseDelayTgt_);
    smGAL_.setCurrentAndTarget(gALTgt_);
    smGAR_.setCurrentAndTarget(gARTgt_);
    smGBL_.setCurrentAndTarget(gBLTgt_);
    smGBR_.setCurrentAndTarget(gBRTgt_);
  }

  void setParams(const DoublerParams& p) {
    params_ = p;

    float detune = clampf(p.detuneCents, 0.0f, 30.0f);
    float spread = clampf(p.spread, 0.0f, 1.0f);
    float delayMs = clampf(p.delayMs, 2.0f, 40.0f);
    float mix = clampf(p.mix, 0.0f, 1.0f);

    // Constant pitch ratios; the tap slews by (1 - ratio) samples per sample,
    // so the grain phase advances by |1 - ratio| / grainLen each sample.
    const float ratioUp   = std::pow(2.0f,  detune / 1200.0f);
    const float ratioDown = std::pow(2.0f, -detune / 1200.0f);
    phaseIncA_ = std::fabs(1.0f - ratioUp)   / grainSamples_;
    phaseIncB_ = std::fabs(1.0f - ratioDown) / grainSamples_;
    upA_ = true;    // voice A reads toward the write head (pitch up)
    upB_ = false;   // voice B falls behind (pitch down)

    baseDelayTgt_ = delayMs * 0.001f * static_cast<float>(sr_);
    mixTgt_ = mix;

    // Equal-power pan. Voice A pans left, voice B pans right, opening up with
    // `spread`. pan in [0,1]: 0 = hard left, 1 = hard right, 0.5 = centre.
    float panA = 0.5f - 0.5f * spread;
    float panB = 0.5f + 0.5f * spread;
    const float half_pi = 1.57079632679f;
    gALTgt_ = std::cos(panA * half_pi);
    gARTgt_ = std::sin(panA * half_pi);
    gBLTgt_ = std::cos(panB * half_pi);
    gBRTgt_ = std::sin(panB * half_pi);

    smMix_.setTarget(mixTgt_);
    smDelay_.setTarget(baseDelayTgt_);
    smGAL_.setTarget(gALTgt_);
    smGAR_.setTarget(gARTgt_);
    smGBL_.setTarget(gBLTgt_);
    smGBR_.setTarget(gBRTgt_);
  }

  // In-place. ch[c][0..nframes) per channel. Handles 1..N channels; the doubled
  // stereo image is folded down for mono and applied to the first two channels
  // for stereo, extra channels get the mono-folded wet.
  void process(float* const* ch, int nch, int nframes) {
    if (nch <= 0 || nframes <= 0) return;

    const float W = grainSamples_;

    for (int n = 0; n < nframes; ++n) {
      // Mono source = average of input channels.
      float dryMono = 0.0f;
      for (int c = 0; c < nch; ++c) dryMono += ch[c][n];
      dryMono *= (1.0f / static_cast<float>(nch));

      float baseDelay = smDelay_.next();
      float mix = smMix_.next();

      // Push the shared source, read each voice's dual crossfaded grains.
      delays_[0].push(dryMono);
      delays_[1].push(dryMono);

      phaseA_ += phaseIncA_; if (phaseA_ >= 1.0f) phaseA_ -= 1.0f;
      phaseB_ += phaseIncB_; if (phaseB_ >= 1.0f) phaseB_ -= 1.0f;
      float voiceA = readVoice(delays_[0], baseDelay, W, phaseA_, upA_);
      float voiceB = readVoice(delays_[1], baseDelay, W, phaseB_, upB_);

      float gAL = smGAL_.next();
      float gAR = smGAR_.next();
      float gBL = smGBL_.next();
      float gBR = smGBR_.next();

      float wetL = voiceA * gAL + voiceB * gBL;
      float wetR = voiceA * gAR + voiceB * gBR;
      float wetMono = 0.5f * (wetL + wetR);

      float dryG = 1.0f - mix;

      if (nch >= 2) {
        for (int c = 0; c < nch; ++c) {
          float wet = (c == 0) ? wetL : (c == 1) ? wetR : wetMono;
          ch[c][n] = safeClamp(ch[c][n] * dryG + wet * mix);
        }
      } else {
        ch[0][n] = safeClamp(ch[0][n] * dryG + wetMono * mix);
      }
    }
  }

  int latencySamples() const { return 0; }

 private:
  // Dual-grain constant-power read. Grain g1 sits at phase p, grain g2 half a
  // window later; each tap ramps across [base, base+W] (direction depends on
  // pitch up/down) and is faded by sin(pi*p) so the crossfade power is
  // sin^2 + cos^2 = 1.
  static float readVoice(InterpolatedDelayLine& line, float base, float W,
                         float p1, bool up) {
    float p2 = p1 + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
    const float d1 = up ? (base + (1.0f - p1) * W) : (base + p1 * W);
    const float d2 = up ? (base + (1.0f - p2) * W) : (base + p2 * W);
    // Power-complementary grain windows: sin(pi * p) == fastSinTurns(p / 2).
    // A window gain is a modulator, which is what fastSinTurns is for.
    const float g1 = fastSinTurns(0.5f * p1);
    const float g2 = fastSinTurns(0.5f * p2);
    return g1 * line.readAt(d1) + g2 * line.readAt(d2);
  }

  static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }
  // Final safety net: NaN/Inf -> 0, hard bound to |x| < 32.
  static float safeClamp(float x) {
    if (!std::isfinite(x)) return 0.0f;
    if (x > 31.9f) return 31.9f;
    if (x < -31.9f) return -31.9f;
    return x;
  }

  // Grain window (ms). Long enough that 30-cent grains repeat at ~0.5 Hz per
  // grain pair (no flutter), short enough to stay a "tight" double.
  static constexpr double kGrainMs = 35.0;

  double sr_ = 48000.0;
  DoublerParams params_{};

  std::array<InterpolatedDelayLine, 2> delays_{};

  SmoothedValue smMix_{}, smDelay_{};
  SmoothedValue smGAL_{}, smGAR_{}, smGBL_{}, smGBR_{};

  float grainSamples_ = 1.0f;
  float phaseA_ = 0.0f, phaseB_ = 0.25f;
  float phaseIncA_ = 0.0f, phaseIncB_ = 0.0f;
  bool upA_ = true, upB_ = false;
  float baseDelayTgt_ = 0.0f, mixTgt_ = 0.5f;
  float gALTgt_ = 1.0f, gARTgt_ = 0.0f, gBLTgt_ = 0.0f, gBRTgt_ = 1.0f;
};

}  // namespace dcdsp

#endif  // DCDSP_DOUBLER_H
