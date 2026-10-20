// dcdsp_hallreverb.h — feedback-delay-network room / hall reverb (pure dcdsp core).
//
// Models a real acoustic space rather than a plate or a spring: a short
// multitap of EARLY REFLECTIONS (the discrete first bounces that tell the ear
// how big the room is) in front of a LATE TANK that is an 8-line FDN with
// Householder/Hadamard feedback, per-line damping and a per-line DC blocker.
// Topology: predelay -> 4 input-diffusion allpasses -> { early multitap, FDN }
// -> bounded low shelf -> width matrix -> mix.
//
// Easy to get wrong: `space` is NOT a knob. It re-derives every delay-line
// length, so writing the new lengths into the live tank splices eight delay
// lines at once — a click plus a burst of garbage. This core keeps TWO tanks
// and crossfades between them (kSpaceFadeSeconds), which is also why the
// worst-case allocation in prepare() is two full Cathedral-sized tanks and not
// one. The second thing: the feedback gain must be derived per LINE from that
// line's own length (g_i = 10^(-3 L_i / (RT60 fs))), or RT60 is a different
// number for every geometry.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dcdsp {

// Shared building blocks for the four `reverb`-category cores (hall, gated,
// reverse; the convolution reverb reuses the predelay only). Namespaced away
// from dcdsp::detail so a sibling core file cannot collide with it.
namespace revdetail {

/// NaN-safe clamp. `v < lo` and `v > hi` are BOTH false for a NaN, so the
/// obvious form lets a NaN through into a coefficient — and a NaN in an FDN is
/// permanent, because every line feeds itself.
inline float clampNaN(float v, float lo, float hi) noexcept
{
    if (!(v >= lo)) return lo;   // NaN lands here
    if (v > hi)     return hi;
    return v;
}

/// A predelay whose tap GLIDES. A predelay in samples read straight off a knob
/// teleports the read head — platereverb shipped exactly that (W4-D14) — so the
/// tap is a SmoothedValue and the line is fractional.
class RampedPredelay
{
public:
    void prepare(double sr, float maxMs, float rampSeconds = 0.020f)
    {
        maxSamples_ = (float) std::max(1.0, (double) maxMs * 0.001 * sr);
        line_.prepare((int) std::ceil(maxSamples_) + 4);
        tap_.reset(sr, rampSeconds);
        sr_ = sr > 0.0 ? sr : 48000.0;
    }
    /// Message thread (through the core's setParams): retarget, never jump.
    void setDelayMs(float ms) noexcept
    {
        const float s = clampNaN(ms, 0.f, 1.0e6f) * (float) sr_ * 0.001f;
        tap_.setTarget(std::min(s, maxSamples_));
    }
    /// prepare() only: a freshly prepared core starts AT its predelay.
    void snap() noexcept { tap_.snapToTarget(); }
    void reset() { line_.reset(); }
    inline float process(float x) noexcept
    {
        line_.push(x);
        return line_.readAt(tap_.next());
    }
    float currentSamples() const noexcept { return tap_.current(); }

private:
    InterpolatedDelayLine line_;
    SmoothedValue         tap_;
    float                 maxSamples_ = 1.f;
    double                sr_         = 48000.0;
};

/// Series Schroeder allpass diffusers on the tank INPUT. Smears a transient
/// into the tank so the first milliseconds are dense instead of a row of
/// discrete echoes.
template <int kStages>
class AllpassDiffuser
{
public:
    void prepare(double sr)
    {
        // Mutually prime-ish lengths in ms; short, this is input diffusion.
        static constexpr double kMs[8] = { 4.77, 3.59, 12.73, 9.31, 6.13, 2.71, 8.09, 5.39 };
        for (int i = 0; i < kStages; ++i)
        {
            const int n = std::max(1, (int) std::lround(kMs[i % 8] * 0.001 * (sr > 0.0 ? sr : 48000.0)));
            buf_[i].assign((std::size_t) n, 0.f);
            idx_[i] = 0;
        }
    }
    void reset()
    {
        for (int i = 0; i < kStages; ++i) { std::fill(buf_[i].begin(), buf_[i].end(), 0.f); idx_[i] = 0; }
    }
    /// 0 .. 1; mapped into a musically useful allpass coefficient range.
    void setDiffusion(float d) noexcept { k_ = 0.20f + 0.55f * clampNaN(d, 0.f, 1.f); }
    inline float process(float x) noexcept
    {
        for (int i = 0; i < kStages; ++i)
        {
            const std::size_t p = (std::size_t) idx_[i];
            const float       v = buf_[i][p];
            const float       y = v - k_ * x;
            buf_[i][p] = x + k_ * y;
            if (++idx_[i] >= (int) buf_[i].size()) idx_[i] = 0;
            x = y;
        }
        return x;
    }

private:
    std::vector<float> buf_[kStages];
    int                idx_[kStages] = { 0 };
    float              k_            = 0.6f;
};

/// The discrete early reflections. One shared delay line, a different tap
/// pattern for L and R (which is most of what "a room" sounds like before the
/// tail arrives), and TWO tap SETS — because the pattern stretches with the
/// room size, and moving an integer tap is a splice exactly like moving a
/// delay line is. The caller crossfades the two sets on the same ramp it
/// crossfades the two tanks on.
class EarlyReflections
{
public:
    static constexpr int kTaps = 8;
    static constexpr int kSets = 2;

    void prepare(double sr)
    {
        sr_ = sr > 0.0 ? sr : 48000.0;
        // Worst case: the longest tap at the largest space scale.
        const int maxN = (int) std::ceil(kTapMs[kTaps - 1] * 1.17 * 0.001 * sr_ * kMaxScale) + 4;
        line_.assign((std::size_t) std::max(8, maxN), 0.f);
        w_ = 0;
        setScale(0, 1.f);
        setScale(1, 1.f);
    }
    void reset() { std::fill(line_.begin(), line_.end(), 0.f); w_ = 0; }

    /// Message thread. `scale` stretches the pattern with the room size.
    void setScale(int set, float scale) noexcept
    {
        if (set < 0 || set >= kSets) return;
        const float s = clampNaN(scale, 0.25f, (float) kMaxScale);
        const int   n = (int) line_.size();
        for (int i = 0; i < kTaps; ++i)
        {
            int d = (int) std::lround(kTapMs[i] * 0.001 * sr_ * (double) s);
            delayL_[set][i] = std::min(std::max(d, 1), n - 1);
            d = (int) std::lround(kTapMs[i] * 1.17 * 0.001 * sr_ * (double) s); // R pattern
            delayR_[set][i] = std::min(std::max(d, 1), n - 1);
        }
    }

    inline void process(int set, float in, float& outL, float& outR) noexcept
    {
        if (set == 0) line_[(std::size_t) w_] = in;   // written once per sample
        const int n = (int) line_.size();
        float l = 0.f, r = 0.f;
        for (int i = 0; i < kTaps; ++i)
        {
            int a = w_ - delayL_[set][i]; if (a < 0) a += n;
            int b = w_ - delayR_[set][i]; if (b < 0) b += n;
            l += kTapGain[i] * line_[(std::size_t) a];
            r += kTapGain[i] * line_[(std::size_t) b];
        }
        outL = l; outR = r;
    }
    /// Call once per sample, AFTER every set has been read.
    inline void advance() noexcept { if (++w_ >= (int) line_.size()) w_ = 0; }

private:
    static constexpr double kMaxScale     = 5.0;
    static constexpr double kTapMs[kTaps] = { 7.3, 11.9, 17.1, 23.7, 31.3, 41.1, 52.9, 67.3 };
    static constexpr float  kTapGain[kTaps] = { 0.84f, -0.71f, 0.62f, -0.53f, 0.44f, -0.36f, 0.29f, -0.22f };

    std::vector<float> line_;
    int                delayL_[kSets][kTaps] = { { 0 } }, delayR_[kSets][kTaps] = { { 0 } };
    int                w_  = 0;
    double             sr_ = 48000.0;
};

/// The late tank: 8 delay lines, unitary (Hadamard) feedback mixing, one damping
/// one-pole and one DC blocker per line.
///
/// Unitary mixing is what makes the decay predictable: the mixing matrix
/// preserves energy exactly, so the loop gain is the PRODUCT of three things and
/// nothing else -- g_i, the damping one-pole, and the in-loop DC blocker. It is
/// the product that is below unity, not each factor: `DcBlocker` is
/// y = x - x1 + r*y1, whose magnitude is 2/(1+r) at Nyquist -- about 1.001 for
/// the 15 Hz corner used here, i.e. strictly ABOVE 1. Since the damping corner
/// reaches past a quarter of the rate (DotCharter #87: 16 kHz is designed at
/// 16 kHz, not 12) the damping-times-blocker product is NOT below unity
/// everywhere either: its maximum, swept over the whole band, is 0.99985 at the
/// old 12 kHz ceiling but 1.000196 at 16 kHz / 48 kHz and 1.000308 at 16 kHz /
/// 44.1 kHz (both near 540 Hz). The tank stays contractive because g_i carries
/// the margin: the shortest line is 10.97 ms at the smallest geometry (Room,
/// scale 1) and setDecaySeconds clamps RT60 at 60 s, so g_i <= 0.998738 and the
/// worst loop gain is 0.99905. The catalogue's own ceilings (hall 20 s, gated
/// 10 s, reverse 5 s) leave far more. Pinned by the `filter_ceiling` native
/// test, which rings the worst case for 30 s and requires it to decay. This comment said "the ONLY loss in the loop is g_i (and the damping
/// filter)" and called the network strictly contractive for any g_i < 1, which
/// is the shape of safety claim that stops the next reader looking.
class Fdn8
{
public:
    static constexpr int kLines = 8;
    /// Line lengths in seconds at scale 1 (the "Room" geometry). Mutually
    /// prime-ish so the modes do not pile up.
    static constexpr double kBaseSec[kLines] =
        { 0.01097, 0.01399, 0.01669, 0.01913, 0.02237, 0.02503, 0.02819, 0.03109 };
    static constexpr double kMaxScale = 5.0;

    void prepare(double sr)
    {
        sr_ = sr > 0.0 ? sr : 48000.0;
        for (int i = 0; i < kLines; ++i)
        {
            // WORST CASE: the largest geometry this tank can ever be asked for,
            // so setGeometry() never reallocates and never runs on the callback.
            const int maxN = (int) std::ceil(kBaseSec[i] * kMaxScale * sr_) + 4;
            buf_[i].assign((std::size_t) std::max(8, maxN), 0.f);
            w_[i] = 0;
            dc_[i].prepare(sr_, 15.0);
            lp_[i].setLowpass(sr_, 6000.0);
        }
        setGeometry(1.0f);
        setDecaySeconds(2.5f);
        setDampingHz(6000.f);
        reset();
    }

    void reset()
    {
        for (int i = 0; i < kLines; ++i)
        {
            std::fill(buf_[i].begin(), buf_[i].end(), 0.f);
            w_[i] = 0;
            dc_[i].reset();
            lp_[i].reset();
        }
    }

    /// Message thread. `scale` multiplies every line length (the geometry).
    void setGeometry(float scale) noexcept
    {
        scale_ = clampNaN(scale, 0.25f, (float) kMaxScale);
        for (int i = 0; i < kLines; ++i)
        {
            const int n = (int) buf_[i].size();
            len_[i] = std::min(std::max((int) std::lround(kBaseSec[i] * (double) scale_ * sr_), 8), n - 1);
        }
        refreshGains();
    }

    /// Message thread. RT60 in seconds, derived PER LINE from its own length.
    void setDecaySeconds(float rt60) noexcept
    {
        rt60_ = clampNaN(rt60, 0.05f, 60.f);
        refreshGains();
    }

    /// Message thread. In-loop damping corner; the tail's HF decays faster.
    void setDampingHz(float hz) noexcept
    {
        // Near-Nyquist ceiling: `dampingKHz` advertises 1-16 kHz, and until
        // DotCharter #87 everything past 12 kHz (48 kHz device) damped at 12.
        // See the class note for why the loop stays contractive.
        const double fc = clampCutoffNearNyquist(sr_, (double) clampNaN(hz, 20.f, 400000.f));
        for (int i = 0; i < kLines; ++i) lp_[i].setLowpass(sr_, fc, true);
    }

    /// One sample in (mono tank feed), two decorrelated taps out.
    inline void process(float in, float& outL, float& outR) noexcept
    {
        float f[kLines];
        float l = 0.f, r = 0.f;
        for (int i = 0; i < kLines; ++i)
        {
            int p = w_[i] - len_[i];
            if (p < 0) p += (int) buf_[i].size();
            const float o = buf_[i][(std::size_t) p];
            ((i & 1) == 0 ? l : r) += (i & 2) ? -o : o;
            f[i] = g_[i] * dc_[i].process(lp_[i].process(o));
        }
        hadamard8(f);
        for (int i = 0; i < kLines; ++i)
        {
            buf_[i][(std::size_t) w_[i]] = in + f[i];
            if (++w_[i] >= (int) buf_[i].size()) w_[i] = 0;
        }
        outL = 0.5f * l;
        outR = 0.5f * r;
    }

    float geometryScale() const noexcept { return scale_; }
    /// Longest line, in seconds — what "the tank has flushed" means.
    double longestLineSeconds() const noexcept { return kBaseSec[kLines - 1] * (double) scale_; }

private:
    /// Normalised 8-point Walsh-Hadamard, in place. Unitary: 1/sqrt(8) scaling.
    static inline void hadamard8(float* x) noexcept
    {
        for (int span = 1; span < kLines; span <<= 1)
            for (int i = 0; i < kLines; i += span << 1)
                for (int j = i; j < i + span; ++j)
                {
                    const float a = x[j], b = x[j + span];
                    x[j] = a + b; x[j + span] = a - b;
                }
        constexpr float norm = 0.35355339059327373f; // 1 / sqrt(8)
        for (int i = 0; i < kLines; ++i) x[i] *= norm;
    }

    void refreshGains() noexcept
    {
        // 10^(-3 L / (RT60 fs)) — 60 dB of decay in exactly RT60 seconds for a
        // unitary mixing matrix, whatever each line's length happens to be.
        for (int i = 0; i < kLines; ++i)
        {
            const double e = -6.907755278982137 * (double) len_[i] / ((double) rt60_ * sr_);
            double       g = std::exp(e);
            if (!(g < 0.9999)) g = 0.9999;   // NaN-safe ceiling: never a ring-up
            if (!(g > 0.0))    g = 0.0;
            g_[i] = (float) g;
        }
    }

    std::vector<float> buf_[kLines];
    OnePole            lp_[kLines];
    DcBlocker          dc_[kLines];
    int                w_[kLines]   = { 0 };
    int                len_[kLines] = { 0 };
    float              g_[kLines]   = { 0.f };
    float              scale_       = 1.f;
    float              rt60_        = 2.5f;
    double             sr_          = 48000.0;
};

} // namespace revdetail

struct HallReverbParams {
    int   space       = 2;      // 0 Room, 1 Chamber, 2 Hall, 3 Cathedral
    float decayS      = 2.5f;   // 0.2 .. 20 s  (RT60)
    float predelayMs  = 20.f;   // 0 .. 200 ms
    float dampingKHz  = 6.f;    // 1 .. 16 kHz
    float lowCutHz    = 80.f;   // 20 .. 500 Hz (bounded shelf)
    float erLevel     = 0.5f;   // 0 .. 1
    float diffusion   = 0.7f;   // 0 .. 1
    float width       = 1.f;    // 0 .. 1
    float mix         = 0.3f;   // 0 .. 1
};

class HallReverbCore {
public:
    /// Room / Chamber / Hall / Cathedral as a geometry multiplier.
    static constexpr float kSpaceScale[4] = { 1.0f, 1.7f, 3.0f, 5.0f };

    void prepare(double sampleRate, int maxBlockSize, int /*numChannels*/)
    {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 512;

        pre_.prepare(sr_, kMaxPredelayMs);
        diff_.prepare(sr_);
        er_.prepare(sr_);
        for (int t = 0; t < 2; ++t) tank_[t].prepare(sr_);

        // Every gain that reaches the output RAMPS.
        wet1_.reset(sr_, kGainRampSeconds);
        wet2_.reset(sr_, kGainRampSeconds);
        dry_.reset(sr_, kGainRampSeconds);
        er_g_.reset(sr_, kGainRampSeconds);
        fade_ = 1.f;                      // no crossfade in flight
        fadeStep_ = 1.f / std::max(1.f, (float) (kSpaceFadeSeconds * sr_));
        warmSamples_ = (int) (kSpaceWarmSeconds * sr_);
        warm_ = 0;
        cur_ = 0; nxt_ = -1;

        applyAll();                       // re-derive everything the RATE owns
        // Geometry is the ONE thing applyAll() does not touch (see there); a
        // fresh prepare sets the live tank directly, with no fade to arm.
        const float scale = kSpaceScale[std::min(std::max(p_.space, 0), 3)];
        tank_[0].setGeometry(scale);
        tank_[0].setDecaySeconds(p_.decayS);
        tank_[0].setDampingHz(p_.dampingKHz * 1000.f);
        er_.setScale(0, scale);
        er_.setScale(1, scale);
        wet1_.snapToTarget(); wet2_.snapToTarget(); dry_.snapToTarget(); er_g_.snapToTarget();
        pre_.snap();
        reset();
    }

    /// State only — prepare() calls it, and a bypass-park clear comes through
    /// DspModule::reset(). No user content lives in this core.
    void reset()
    {
        pre_.reset(); diff_.reset(); er_.reset();
        for (int t = 0; t < 2; ++t) tank_[t].reset();
        shelfL_.reset(); shelfR_.reset();
        fade_ = 1.f; nxt_ = -1; warm_ = 0;
    }

    void setParams(const HallReverbParams& p)
    {
        HallReverbParams q = p;
        q.space      = std::min(std::max(p.space, 0), 3);
        q.decayS     = revdetail::clampNaN(p.decayS, 0.2f, 20.f);
        q.predelayMs = revdetail::clampNaN(p.predelayMs, 0.f, kMaxPredelayMs);
        q.dampingKHz = revdetail::clampNaN(p.dampingKHz, 1.f, 16.f);
        q.lowCutHz   = revdetail::clampNaN(p.lowCutHz, 20.f, 500.f);
        q.erLevel    = revdetail::clampNaN(p.erLevel, 0.f, 1.f);
        q.diffusion  = revdetail::clampNaN(p.diffusion, 0.f, 1.f);
        q.width      = revdetail::clampNaN(p.width, 0.f, 1.f);
        q.mix        = revdetail::clampNaN(p.mix, 0.f, 1.f);

        const bool spaceMoved = q.space != p_.space;
        p_ = q;
        applyAll();
        // A GEOMETRY change is a topology switch: arm the second tank and
        // crossfade, never splice the live one.
        if (spaceMoved) armSpaceFade();
    }

    void process(float* const* channels, int numChannels, int numFrames)
    {
        if (numChannels <= 0 || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;          // oversized: skipped, not half-done
        float* L = channels[0];
        float* R = numChannels > 1 ? channels[1] : nullptr;
        if (L == nullptr) return;

        for (int n = 0; n < numFrames; ++n)
        {
            const float inL = L[n];
            const float inR = R != nullptr ? R[n] : inL;
            // Mono fold-down that SUMS: one tank feed, 0.5 so a mono source
            // panned centre is not 6 dB hotter than the same source in one ear.
            const float mono = 0.5f * (inL + inR);

            const float fed = diff_.process(pre_.process(mono));

            // The ER line is written by set 0's call and read by both, so it is
            // always read in the order 0, 1 and advanced once.
            float e0L = 0.f, e0R = 0.f, e1L = 0.f, e1R = 0.f;
            er_.process(0, fed, e0L, e0R);
            er_.process(1, fed, e1L, e1R);
            er_.advance();
            float eL = cur_ == 0 ? e0L : e1L;
            float eR = cur_ == 0 ? e0R : e1R;

            float aL = 0.f, aR = 0.f;
            tank_[cur_].process(fed, aL, aR);
            float wL = aL, wR = aR;
            if (nxt_ >= 0)
            {
                float bL = 0.f, bR = 0.f;
                tank_[nxt_].process(fed, bL, bR);
                if (warm_ > 0)
                {
                    // WARM-UP, and it is not an optimisation. A tank that has
                    // just been reset emits NOTHING until its shortest line
                    // (11 ms at Room) first wraps, and then each of the eight
                    // switches on in turn — eight little steps, straight into a
                    // rising crossfade. Measured: 0.089 against 0.0046 for the
                    // same audio with no switch, a 19x step the fade did not
                    // hide. So the new tank runs SILENT until every line has
                    // filled, and only then becomes audible.
                    --warm_;
                }
                else
                {
                    const float nL = nxt_ == 0 ? e0L : e1L;
                    const float nR = nxt_ == 0 ? e0R : e1R;
                    // Equal-gain crossfade. `fade_` walks 0 -> 1 over
                    // kSpaceFadeSeconds; its per-sample step IS the slope a
                    // click test measures against.
                    wL = aL + (bL - aL) * fade_;
                    wR = aR + (bR - aR) * fade_;
                    eL += (nL - eL) * fade_;
                    eR += (nR - eR) * fade_;
                    fade_ += fadeStep_;
                    if (fade_ >= 1.f) { cur_ = nxt_; nxt_ = -1; fade_ = 1.f; }
                }
            }

            const float eg = er_g_.next();
            wL += eg * eL;
            wR += eg * eR;

            wL = shelfL_.process(wL);
            wR = shelfR_.process(wR);

            const float w1 = wet1_.next(), w2 = wet2_.next(), d = dry_.next();
            const float outL = wL * w1 + wR * w2 + inL * d;
            const float outR = wR * w1 + wL * w2 + inR * d;
            if (R != nullptr) { L[n] = outL; R[n] = outR; }
            else              L[n] = 0.5f * (outL + outR);
        }
        // Channels past the first stereo pair are left alone (dry).
    }

    int latencySamples() const { return 0; }

    /// Test hook: the geometry scale currently AUDIBLE (the tank being faded
    /// out still is, which is the point of having two).
    float activeGeometryScale() const { return tank_[cur_].geometryScale(); }

private:
    /// Everything EXCEPT the geometry. Geometry (the tank line lengths and the
    /// early-reflection tap pattern) may only be written into a tank/tap set
    /// that nobody is listening to — see armSpaceFade(). Writing it here would
    /// splice the LIVE tank on every unrelated knob tick, which is the bug the
    /// second tank exists to avoid.
    void applyAll()
    {
        diff_.setDiffusion(p_.diffusion);
        for (int t = 0; t < 2; ++t)
        {
            tank_[t].setDecaySeconds(p_.decayS);
            tank_[t].setDampingHz(p_.dampingKHz * 1000.f);
        }
        pre_.setDelayMs(p_.predelayMs);

        // A BOUNDED SHELF, never an unbounded high-pass: three of these in a
        // chain SUM, and an unbounded one takes the low E with it.
        for (Biquad* b : { &shelfL_, &shelfR_ })
            b->setLowShelf(sr_, (double) p_.lowCutHz, 0.707, kLowShelfDb);

        const float wet = p_.mix;
        wet1_.setTarget(wet * (p_.width * 0.5f + 0.5f));
        wet2_.setTarget(wet * ((1.f - p_.width) * 0.5f));
        dry_.setTarget(1.f - p_.mix);
        er_g_.setTarget(p_.erLevel * kErScale);
    }

    void armSpaceFade()
    {
        const float scale = kSpaceScale[std::min(std::max(p_.space, 0), 3)];
        if (nxt_ >= 0)
        {
            // A second switch inside a fade: retarget the tank already fading
            // IN rather than starting a third. It is still near-silent (the
            // ramp has barely begun on any realistic click rate), so its
            // geometry write is not audible; restarting the ramp is not, either.
            tank_[nxt_].reset();
            tank_[nxt_].setGeometry(scale);
            er_.setScale(nxt_, scale);
            fade_ = 0.f;
            warm_ = warmSamples_;
            return;
        }
        const int other = 1 - cur_;
        tank_[other].reset();
        tank_[other].setGeometry(scale);
        tank_[other].setDecaySeconds(p_.decayS);
        tank_[other].setDampingHz(p_.dampingKHz * 1000.f);
        er_.setScale(other, scale);
        nxt_  = other;
        fade_ = 0.f;
        warm_ = warmSamples_;
    }

    static constexpr float  kMaxPredelayMs     = 200.f;
    static constexpr double kSpaceFadeSeconds  = 0.040;   // 40 ms: eight lines swap
    /// Longer than the LONGEST line of the largest geometry (Cathedral, 155 ms),
    /// so every line of the incoming tank has filled before it is audible.
    static constexpr double kSpaceWarmSeconds  = 0.200;
    static constexpr float  kGainRampSeconds   = 0.010f;
    static constexpr double kLowShelfDb        = -15.0;   // bounded, not a high-pass
    static constexpr float  kErScale           = 0.7f;

    revdetail::RampedPredelay      pre_;
    revdetail::AllpassDiffuser<4>  diff_;
    revdetail::EarlyReflections    er_;
    revdetail::Fdn8                tank_[2];
    Biquad                         shelfL_, shelfR_;
    SmoothedValue                  wet1_, wet2_, dry_, er_g_;
    HallReverbParams               p_{};
    double                         sr_       = 48000.0;
    int                            maxBlock_ = 512;
    int                            cur_      = 0;
    int                            nxt_      = -1;
    float                          fade_     = 1.f;
    float                          fadeStep_ = 1.f;
    int                            warm_        = 0;
    int                            warmSamples_ = 0;
};

} // namespace dcdsp
