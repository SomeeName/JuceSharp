// dcdsp_reversereverb.h — reverse (swelled) reverb (pure dcdsp core).
//
// Models the tape trick: reverse the tape, print a reverb, reverse it back, and
// the tail SWELLS into the note instead of decaying out of it. Topology:
// predelay -> diffusers -> 8-line FDN (dcdsp_hallreverb.h's tank) -> capture
// ring -> two backwards readers -> mix.
//
// The reader is the whole idea and it is one line: a head that walks BACKWARDS
// through the ring moves away from the write head at 2x, so the sample to read
// at window phase p is simply `lookback = 2p + 1`. A decaying tail read that
// way rises.
//
// Easy to get wrong: one reader cannot be continuous — it has to jump from the
// oldest sample of its window back to the newest, which is a full-scale step
// every windowMs. So there are TWO readers, half a window apart, each under a
// periodic Hann envelope (which sums to unity at 50 % overlap), and a window
// LENGTH change is latched only where a reader's envelope is exactly zero.
// Changing it anywhere else is the same click by another name.
//
// The envelope applied is NOT the Hann alone: it is hann(x) * x^gamma, and that
// PRODUCT is not COLA -- at swellShape 1 the two readers' sum walked 0.088 to
// 0.187, a 6.6 dB pump at 2/windowMs (2.5 Hz for the default 800 ms window), and
// the mean wet level fell ~9 dB from swellShape 0.5 to 1, which made the shape
// knob a large volume control. The table is normalised so the two shifted copies
// sum to a CONSTANT, and to the same constant at every shape.
//
// The two readers must also STAY half a window apart. They latch a new length at
// their own wraps, which are half a window apart, so a change left the offset at
// whatever it happened to become -- and near zero both envelopes are at their
// zero together and the wet path NULLS once per window. Reader 1 therefore runs
// ONE transitional window of a different length after a change, whose two ends
// are both at envelope zero, chosen so its next wrap lands back on the half.
#pragma once
#include "dcdsp_hallreverb.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

struct ReverseReverbParams {
    float decayS     = 2.0f;   // 0.5 .. 5 s (the tank's RT60 before reversal)
    float windowMs   = 800.f;  // 100 .. 2000 ms capture window played backwards
    float predelayMs = 0.f;    // 0 .. 200 ms
    float dampingKHz = 8.f;    // 1 .. 16 kHz
    float mix        = 0.5f;   // 0 .. 1
    float swellShape = 0.5f;   // 0 flat .. 1 long slow swell
};

class ReverseReverbCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int /*numChannels*/)
    {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 512;

        pre_.prepare(sr_, kMaxPredelayMs);
        diff_.prepare(sr_);
        tank_.prepare(sr_);
        tank_.setGeometry(kGeometryScale);

        // WORST CASE: a backwards head reaches 2 * windowMs behind the write
        // head, so the ring is sized for twice the LONGEST window this core can
        // ever be asked for. Nothing here is ever resized again.
        maxWindow_ = std::max(8, (int) std::ceil(kMaxWindowMs * 0.001 * sr_));
        ringSize_  = 2 * maxWindow_ + 4;
        ringL_.assign((std::size_t) ringSize_, 0.f);
        ringR_.assign((std::size_t) ringSize_, 0.f);
        w_ = 0;

        wet_.reset(sr_, kGainRampSeconds);
        dry_.reset(sr_, kGainRampSeconds);

        shapeBuilt_ = -1.f;                 // force the envelope table to build
        applyAll();
        wet_.snapToTarget();
        dry_.snapToTarget();
        pre_.snap();
        // The readers' window lengths are latched at a wrap; a fresh prepare has
        // no wrap to wait for.
        win_[0] = win_[1] = pendingWindow_;
        reset();
    }

    void reset()
    {
        pre_.reset(); diff_.reset(); tank_.reset();
        std::fill(ringL_.begin(), ringL_.end(), 0.f);
        std::fill(ringR_.begin(), ringR_.end(), 0.f);
        w_ = 0;
        phase_[0] = 0;
        phase_[1] = win_[1] / 2;            // half a window apart
        needRephase_ = false;
    }

    /// Runs on the AUDIO thread (through the adapter's ParamMailbox): no
    /// allocation, and the one loop here is a 512-entry table rebuilt only when
    /// `swellShape` actually moved.
    void setParams(const ReverseReverbParams& p)
    {
        ReverseReverbParams q;
        q.decayS     = revdetail::clampNaN(p.decayS, 0.5f, 5.f);
        q.windowMs   = revdetail::clampNaN(p.windowMs, 100.f, kMaxWindowMs);
        q.predelayMs = revdetail::clampNaN(p.predelayMs, 0.f, kMaxPredelayMs);
        q.dampingKHz = revdetail::clampNaN(p.dampingKHz, 1.f, 16.f);
        q.mix        = revdetail::clampNaN(p.mix, 0.f, 1.f);
        q.swellShape = revdetail::clampNaN(p.swellShape, 0.f, 1.f);
        p_ = q;
        applyAll();
    }

    void process(float* const* channels, int numChannels, int numFrames)
    {
        if (numChannels <= 0 || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;          // oversized: skipped
        float* L = channels[0];
        float* R = numChannels > 1 ? channels[1] : nullptr;
        if (L == nullptr) return;

        for (int n = 0; n < numFrames; ++n)
        {
            const float inL  = L[n];
            const float inR  = R != nullptr ? R[n] : inL;
            const float mono = 0.5f * (inL + inR);

            float tL = 0.f, tR = 0.f;
            tank_.process(diff_.process(pre_.process(mono)), tL, tR);
            ringL_[(std::size_t) w_] = tL;
            ringR_[(std::size_t) w_] = tR;

            float wL = 0.f, wR = 0.f;
            for (int k = 0; k < 2; ++k)
            {
                const int   win = win_[k];
                const int   p   = phase_[k];
                const float g   = envelopeAt((float) p / (float) win);
                int idx = w_ - (2 * p + 1);
                idx %= ringSize_;
                if (idx < 0) idx += ringSize_;
                wL += g * ringL_[(std::size_t) idx];
                wR += g * ringR_[(std::size_t) idx];
                if (++phase_[k] >= win)
                {
                    // The ONE place a window-length change may land: the
                    // envelope is exactly zero here, so the new geometry costs
                    // nothing audible.
                    phase_[k] = 0;
                    if (k == 0)
                    {
                        if (win_[0] != pendingWindow_)
                        {
                            win_[0]      = pendingWindow_;
                            needRephase_ = true;
                        }
                    }
                    else if (needRephase_)
                    {
                        // Reader 1 wraps half a window after reader 0, so it
                        // latches LATER and the offset between the two settles
                        // wherever it lands -- possibly near zero, where both
                        // envelopes are at their zero together and the wet path
                        // NULLS once per window. One transitional window fixes
                        // it: both its ends are at envelope zero, and its
                        // length is chosen so the NEXT wrap lands where reader
                        // 0's phase is exactly half.
                        const int w0 = win_[0];
                        int d = ((w0 / 2) - phase_[0] + w0) % w0;
                        if (d < w0 / 4) d += w0;          // never a hurried window
                        win_[1]      = std::min(std::max(d, 8), maxWindow_);
                        needRephase_ = false;
                    }
                    else
                    {
                        win_[1] = pendingWindow_;
                    }
                }
            }

            if (++w_ >= ringSize_) w_ = 0;

            const float wg = wet_.next(), d = dry_.next();
            const float outL = wL * wg + inL * d;
            const float outR = wR * wg + inR * d;
            if (R != nullptr) { L[n] = outL; R[n] = outR; }
            else              L[n] = 0.5f * (outL + outR);
        }
    }

    int latencySamples() const { return 0; }   // the DRY path is untouched

    /// Test hook (native only): the window each reader is currently running.
    int windowSamplesForTest(int reader) const
    { return (reader >= 0 && reader < 2) ? win_[reader] : 0; }

private:
    /// Hann (periodic, sums to unity at 50 % overlap) times the swell curve.
    /// `x` is the reader's phase, 0 at the newest captured sample and 1 at the
    /// oldest — so a rising curve in x is a swell in time.
    float envelopeAt(float x) const noexcept
    {
        if (!(x >= 0.f)) x = 0.f;
        if (x > 1.f) x = 1.f;
        const float  f = x * (float) (kEnvTable - 1);
        const int    i = (int) f;
        const int    j = std::min(i + 1, kEnvTable - 1);
        const float  t = f - (float) i;
        return env_[(std::size_t) i] + t * (env_[(std::size_t) j] - env_[(std::size_t) i]);
    }

    void applyAll()
    {
        tank_.setDecaySeconds(p_.decayS);
        tank_.setDampingHz(p_.dampingKHz * 1000.f);
        pre_.setDelayMs(p_.predelayMs);
        wet_.setTarget(p_.mix);
        dry_.setTarget(1.f - p_.mix);

        int win = (int) std::lround((double) p_.windowMs * 0.001 * sr_);
        pendingWindow_ = std::min(std::max(win, 8), maxWindow_);

        if (p_.swellShape != shapeBuilt_)
        {
            // 0 -> sqrt (arrives early and sits there), 1 -> x^3.5 (a long
            // slow swell). Fixed size, so this never allocates on the callback.
            const float gamma = 0.5f + 3.0f * p_.swellShape;
            for (int i = 0; i < kEnvTable; ++i)
            {
                const float x    = (float) i / (float) (kEnvTable - 1);
                const float hann = 0.5f - 0.5f * std::cos(6.283185307179586f * x);
                env_[(std::size_t) i] = hann * std::pow(x, gamma);
            }
            // …and then normalised so the two readers SUM to a constant. The
            // Hann factor alone is COLA at 50 % overlap; hann(x) * x^gamma is
            // not, and the header used to claim the property of the factor for
            // the product. Normalising to a FIXED constant (rather than to each
            // shape's own mean) also takes the ~9 dB of level change out of
            // `swellShape`, which is a SHAPE control and was a volume one.
            normaliseToConstantOverlap();
            shapeBuilt_ = p_.swellShape;
        }
    }

    /// Scale `env_` so env[i] + env[(i + N/2) % N] is exactly kOverlapSum for
    /// every i. The two readers are half a window apart, so that sum IS the
    /// wet gain the pair applies, and a constant sum is what "no pumping"
    /// means. Neither term can be zero at the same i: the Hann pair sums to
    /// exactly 1, so at least one factor is non-zero everywhere.
    void normaliseToConstantOverlap() noexcept
    {
        constexpr int half = kEnvTable / 2;
        for (int i = 0; i < half; ++i)
        {
            const float sum = env_[(std::size_t) i] + env_[(std::size_t) (i + half)];
            const float g   = (sum > 1.0e-9f) ? (kOverlapSum / sum) : 0.f;
            env_[(std::size_t) i]          *= g;
            env_[(std::size_t) (i + half)] *= g;
        }
        env_[(std::size_t) (kEnvTable - 1)] = env_[0];   // the table is periodic
    }

    static constexpr float kMaxWindowMs     = 2000.f;
    /// What the pair sums to after normalisation. It is the MEAN of the
    /// un-normalised sum at the default swellShape 0.5, so the shipping voicing
    /// keeps its level and the other shapes are matched to it.
    static constexpr float kOverlapSum      = 0.281f;
    static constexpr float kMaxPredelayMs   = 200.f;
    static constexpr float kGeometryScale   = 2.0f;
    static constexpr float kGainRampSeconds = 0.010f;
    static constexpr int   kEnvTable        = 512;

    revdetail::RampedPredelay     pre_;
    revdetail::AllpassDiffuser<4> diff_;
    revdetail::Fdn8               tank_;
    std::vector<float>            ringL_, ringR_;
    SmoothedValue                 wet_, dry_;
    ReverseReverbParams           p_{};
    std::array<float, kEnvTable>  env_{};
    float                         shapeBuilt_    = -1.f;
    double                        sr_            = 48000.0;
    int                           maxBlock_      = 512;
    int                           maxWindow_     = 8;
    int                           ringSize_      = 20;
    int                           w_             = 0;
    int                           phase_[2]      = { 0, 0 };
    int                           win_[2]        = { 8, 8 };
    int                           pendingWindow_ = 8;
    /// Set when reader 0 latches a NEW window; consumed by reader 1 at its next
    /// wrap, where it runs one transitional window to restore the half-window
    /// offset. Audio thread only.
    bool                          needRephase_   = false;
};

} // namespace dcdsp
