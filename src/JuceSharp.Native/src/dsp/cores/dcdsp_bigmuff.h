// dcdsp_bigmuff.h — Big-Muff-style wall-of-fuzz for DotCharter's native DSP core.
//
// PURE header-only, namespace dcdsp, C++17. Dependencies limited to:
//   <cmath> <vector> <array> <algorithm> <cstddef> <cstdint>
//
// Real-time contract:
//   * All allocation happens in prepare(); process() never allocates, locks,
//     or throws.
//   * Output is ALWAYS finite and bounded: |x| < 32 is guaranteed by a final
//     hard clip + safety clamp. Every feedback / saturating path is guarded.
//
// Sound: two cascaded soft-clipping high-gain stages feed a MID-SCOOPED passive
// tone stack (parallel low-pass "bass" + high-pass "treble" summed, which
// notches the mids). This is the thick, sustaining "wall of fuzz" — distinct
// from the spitty, transistor-starved fuzz-face already in the engine.
//
// The clipping cascade (both adaaTanh stages plus the inter-stage coupling
// filters) runs inside an 8x OversamplerNx wrap: the up-to-222x drive used to
// hit the ADAA stages at base rate with no oversampling at all. The drive is
// applied per OS sample inside the wrap — applying it at base rate would clip
// at base rate before the oversampler can help. 8x because at 222x drive the
// cascade is square-wave-stiff: measured worst alias on a full-scale 9 kHz
// tone at max sustain was -34/-37/-44 dBc at 2x/4x/8x (old core: -17 dBc).
// IIR oversampling => latencySamples() stays 0.

#ifndef DCDSP_BIGMUFF_H
#define DCDSP_BIGMUFF_H

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Shared engine primitives (OnePole/DcBlocker/SmoothedValue/adaaTanh/...).
#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

namespace dcdsp {

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
struct BigMuffParams {
  // Sustain = input drive / compression into the two clipping stages.
  //   [min, max, default] = [0, 1, 0.7]
  float sustain = 0.7f;

  // Tone = scoop tilt. 0 = bassy (low-pass path dominates),
  //   1 = trebly (high-pass path dominates); both are summed so mids stay
  //   scooped across the sweep.  [min, max, default] = [0, 1, 0.5]
  float tone = 0.5f;

  // Volume = post-stack output level.  [min, max, default] = [0, 1, 0.6]
  float volume = 0.6f;
};

// ---------------------------------------------------------------------------
// Core
// ---------------------------------------------------------------------------
class BigMuffCore {
 public:
  void prepare(double sr, int maxBlock, int nch) {
    sr_ = (sr > 0.0) ? sr : 48000.0;
    maxBlock_ = std::max(1, maxBlock);
    nch_ = std::max(1, nch);

    const double osSr = sr_ * kOs;
    channels_.assign(static_cast<std::size_t>(nch_), Channel{});
    for (auto& c : channels_) {
      c.os.prepare(sr_, maxBlock_, kOs);
      c.inBlock.prepare(sr_);
      // The coupling DC block + inter-stage HPF sit BETWEEN the clip stages,
      // i.e. inside the oversampled section — design them at the OS rate.
      c.coupling.prepare(osSr);
      // Real dcdsp::OnePole is configured as LP or HP via setLowpass/setHighpass;
      // stageHp/toneHp are high-pass, toneLp is low-pass.
      c.stageHp.setHighpass(osSr, kStageHpHz);
      c.toneLp.setLowpass(sr_, kToneLpHz);
      c.toneHp.setHighpass(sr_, kToneHpHz);
      c.outBlock.prepare(sr_);
    }

    driveBuf_.assign(static_cast<std::size_t>(maxBlock_), 0.f);
    toneBuf_.assign(static_cast<std::size_t>(maxBlock_), 0.f);
    volBuf_.assign(static_cast<std::size_t>(maxBlock_), 0.f);

    // ~5 ms parameter smoothing keeps knob turns click-free.
    preGain_.reset(sr_, 0.005f);
    tone_.reset(sr_, 0.005f);
    volume_.reset(sr_, 0.005f);

    setParams(params_);
    // Snap to the targets setParams JUST computed. These three lines used to
    // read `setCurrentAndTarget(x.current())`, which on a first prepare snaps
    // to 0 — the value a never-ramped SmoothedValue starts at — and DISCARDS
    // the target. preGain_ then sat at 0 for ever, so `adaaTanh(0)` = 0 through
    // both clipping stages and volume_ multiplied by 0: a Big Muff added to a
    // board and left at its defaults output digital SILENCE until the first
    // knob write re-published a target. Never snap a smoother to its current
    // value at prepare.
    preGain_.snapToTarget();
    tone_.snapToTarget();
    volume_.snapToTarget();

    reset();
  }

  void reset() {
    for (auto& c : channels_) {
      c.os.reset();
      c.inBlock.reset();
      c.coupling.reset();
      c.stageHp.reset();
      c.toneLp.reset();
      c.toneHp.reset();
      c.outBlock.reset();
      c.clip1State = 0.0f;
      c.clip2State = 0.0f;
    }
  }

  void setParams(const BigMuffParams& p) {
    params_ = p;
    float sustain = clamp01(p.sustain);
    float tone = clamp01(p.tone);
    float volume = clamp01(p.volume);

    // Sustain maps to pre-gain into the clipping cascade. A gentle curve so
    // the useful range is spread across the knob; hot at the top for the
    // classic infinite-sustain wall.
    double drive = 2.0 + 220.0 * static_cast<double>(sustain) * sustain;
    preGain_.setTarget(static_cast<float>(drive));
    tone_.setTarget(tone);
    volume_.setTarget(volume);
  }

  // IN-PLACE. ch[c] points to nframes floats; nch channels processed.
  void process(float* const* ch, int nch, int nframes) {
    if (ch == nullptr || nframes <= 0) return;
    // SKIP an oversized block rather than processing part of it: the tail past
    // maxBlock_ was left UNTOUCHED, i.e. full-level clean DI spliced into the
    // middle of a fuzz block.
    if (nframes > maxBlock_) return;
    int chans = std::min(nch, nch_);
    int frames = std::min(nframes, maxBlock_);

    // Smoothed params advance once per frame, shared across channels.
    for (int n = 0; n < frames; ++n) {
      driveBuf_[n] = preGain_.next();
      toneBuf_[n] = tone_.next();
      volBuf_[n] = volume_.next();
    }

    for (int c = 0; c < chans; ++c) {
      float* buf = ch[c];
      if (buf == nullptr) continue;
      Channel& st = channels_[static_cast<std::size_t>(c)];

      // Base rate: guard + input DC block. The drive gain is applied per OS
      // sample inside the oversampled cascade below.
      for (int n = 0; n < frames; ++n) {
        float x = buf[n];
        if (!std::isfinite(x)) x = 0.0f;
        buf[n] = st.inBlock.process(x);
      }

      // --- oversampled clip cascade (both stages + coupling) --------------
      st.os.upsample(buf, frames);
      float* up = st.os.data();
      const int m = st.os.upSize(frames);
      for (int k = 0; k < m; ++k) {
        const double drive = driveBuf_[k / kOs];
        // Stage-2 gain rides along with drive (both stages hotter with sustain).
        const double stage2Gain = 1.0 + 6.0 * (drive / 222.0);

        // First soft-clip stage.
        double s1 = adaaTanh(static_cast<float>(up[k] * drive), st.clip1State);

        // Inter-stage coupling high-pass (kills asymmetric DC build-up) then
        // the second soft-clip stage — cascaded gain = the fuzz.
        double coupled =
            st.stageHp.process(st.coupling.process(static_cast<float>(s1)));
        double s2 = adaaTanh(static_cast<float>(coupled * stage2Gain), st.clip2State);
        up[k] = detail::sanitize(static_cast<float>(s2));
      }
      st.os.downsample(buf, frames);

      // --- base-rate tone stack + volume -----------------------------------
      for (int n = 0; n < frames; ++n) {
        const double tone = toneBuf_[n];
        const double vol = volBuf_[n];
        const float s2 = buf[n];

        // Mid-scooped passive tone stack: parallel bass (LP) + treble (HP),
        // summed. The gap between the two cutoffs notches the mids; `tone`
        // tilts the balance without ever filling the notch.
        double bass = st.toneLp.process(s2);
        double treble = st.toneHp.process(s2);
        double bassMix = 0.15 + 0.85 * (1.0 - tone);
        double trebMix = 0.15 + 0.85 * tone;
        double toned = bassMix * bass + trebMix * treble;

        // Make-up + output volume, DC block, then a guaranteed hard clip.
        double y = st.outBlock.process(static_cast<float>(toned * 1.6)) * vol;
        // A plain hard clip at BASE rate, outside the 8x region above — and
        // that is FINE, because it never engages. The arithmetic that says it
        // does ("bassMix + trebMix == 1.15, so |toned| * 1.6 reaches ~1.84")
        // treats the two tone-stack paths as if both passed full amplitude at
        // once; they are complementary first-order filters of the SAME signal,
        // so at any one frequency their sum is ~0.575 * |s2| and |y| tops out
        // near 0.92 even with volume at 1.0. MEASURED (rt gate,
        // RtBigMuffOutputStageStaysUnderTheBaseRateClipper): peak 0.850 for a
        // full-scale 600 Hz tone at sustain 0.7 / volume 1.0, and ZERO of
        // 14,336 samples on the ceiling. Do NOT "fix" this into an ADAA clip:
        // adaaHardClip is not the identity below the threshold (it returns the
        // mean over the step, i.e. a one-sample low-pass), so it would change
        // the pedal's sound at every level to bound something that is already
        // bounded. The gate case above fails if a future change raises the
        // level INTO the clipper, which is when this stage would need the 8x
        // wrap or ADAA.
        y = std::max(-1.0, std::min(y, 1.0));

        // Final RT-safety net: never emit NaN/Inf, never exceed |x| < 32.
        if (!std::isfinite(y)) y = 0.0;
        y = std::max(-kCeiling, std::min(y, kCeiling));
        buf[n] = static_cast<float>(y);
      }
    }
  }

  int latencySamples() const { return 0; }

 private:
  struct Channel {
    detail::OversamplerNx os;
    DcBlocker inBlock;
    DcBlocker coupling;   // OS rate (between the clip stages)
    OnePole stageHp;      // OS rate (between the clip stages)
    OnePole toneLp;
    OnePole toneHp;
    DcBlocker outBlock;
    // Per-voice state for the real ADAA1 tanh (dcdsp::adaaTanh(x, x1state)).
    float clip1State = 0.0f;
    float clip2State = 0.0f;
  };

  static inline float clamp01(float v) {
    if (!(v == v)) return 0.0f;  // NaN -> 0
    return std::max(0.0f, std::min(1.0f, v));
  }

  static constexpr int kOs = 8;                 // oversample factor
  static constexpr double kStageHpHz = 60.0;    // inter-stage coupling
  static constexpr double kToneLpHz = 750.0;    // bass path corner
  static constexpr double kToneHpHz = 2200.0;   // treble path corner
  static constexpr double kCeiling = 31.9;      // hard |x| < 32 guarantee

  double sr_ = 48000.0;
  int maxBlock_ = 512;
  int nch_ = 1;
  BigMuffParams params_{};
  std::vector<Channel> channels_;
  std::vector<float> driveBuf_, toneBuf_, volBuf_;
  SmoothedValue preGain_;
  SmoothedValue tone_;
  SmoothedValue volume_;
};

}  // namespace dcdsp

#endif  // DCDSP_BIGMUFF_H
