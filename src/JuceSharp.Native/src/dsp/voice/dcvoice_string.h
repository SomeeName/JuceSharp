// dcvoice_string.h — StringModel: ONE plucked string as a single-delay-loop
// digital waveguide (Karplus-Strong / Jaffe-Smith / Valimaki lineage).
//
// TOPOLOGY (per sample):
//
//   excitation ----------------------------------------------+
//                                                            v
//   buf --> Lagrange-3 read (delay D) --> M x allpass --> loss --> node damper
//    ^         (pitch, continuous)      (stiffness)     (T60(f))  (harmonics)
//    |                                                            |
//    +---- write <-- + <-- pick-stop gain <-- collision clip <----+
//                    |
//                    +--> output
//
// TUNING IS EXACT AT THE FUNDAMENTAL. The loop's total phase delay at the
// target frequency f is D + M*tauAllpass(f) + tauLoss(f); the control step
// solves for D so that sum equals fs/f, every control period, for the CURRENT
// (bent, slid, vibrato'd) pitch. That is what keeps a bend landing on its
// target and the top frets in tune even though the dispersion allpasses add
// tens of samples of frequency-dependent delay.
//
// STIFFNESS: a stiff string's partials are stretched, f_n = n f0 sqrt(1+B n^2)
// / sqrt(1+B). fitDispersion() picks the allpass coefficient that best
// reproduces that series for a given B (least squares over the audible
// partials), at prepare time, per string and fret. Bass gets more stages and
// a larger B than guitar.
//
// REAL-TIME: prepare() allocates (control thread); everything else is
// allocation-free, lock-free and I/O-free. The coefficient ramps are linear
// per sample across one control period, so nothing steps.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dcvoice {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

/// Phase delay in samples of the first-order allpass (a + z^-1) / (1 + a z^-1)
/// at w rad/sample. (1-a)/(1+a) at DC, exactly 1 at Nyquist; a < 0 delays the
/// low end more than the top, which is what stretches the partials.
inline double allpassPhaseDelay(double a, double w)
{
    if (w < 1.0e-9)
        return (1.0 - a) / (1.0 + a);
    return 1.0 - 2.0 * std::atan2(a * std::sin(w), 1.0 + a * std::cos(w)) / w;
}

/// The loop's loss filter: g * (b0 + b1 z^-1) / (1 + a1 z^-1), a first-order
/// SHELF — gain 1 at DC, `k` above the corner — times the DC loss g. A real
/// string's T60 falls steeply over the first few hundred Hz and then levels
/// off (measured on the Karoryfer DI recordings: ~11 s at 110 Hz, ~2.5 s at
/// 440 Hz, ~0.8 s from 800 Hz up). A one-pole low-pass cannot follow that: its
/// attenuation keeps rising, so matching the top end lets the mids ring far too
/// long, which is a buzzy, synthetic sustain. The shelf is passive (|H| <= g)
/// and costs the same per sample.
struct LossCoeffs
{
    double g = 0.999;
    double b0 = 1.0;
    double b1 = 0.0;
    double a1 = 0.0;
};

/// Phase delay (samples) of the loss filter at w rad/sample.
inline double lossPhaseDelay(const LossCoeffs& c, double w)
{
    if (w < 1.0e-9)
        return c.b1 / (c.b0 + c.b1) - c.a1 / (1.0 + c.a1);
    const double num = std::atan2(-c.b1 * std::sin(w), c.b0 + c.b1 * std::cos(w));
    const double den = std::atan2(-c.a1 * std::sin(w), 1.0 + c.a1 * std::cos(w));
    return -(num - den) / w;
}

/// |shelf(e^jw)| — 1 at DC, k at Nyquist.
inline double lossShape(const LossCoeffs& c, double w)
{
    const double nr = c.b0 + c.b1 * std::cos(w), ni = -c.b1 * std::sin(w);
    const double dr = 1.0 + c.a1 * std::cos(w), di = -c.a1 * std::sin(w);
    return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

/// Designs the loss filter from the string's decay profile: t60Low at the
/// fundamental f0, a plateau t60High above `cornerHz` (the geometric centre of
/// the shelf's transition). Per round trip (one period of f0) the loop must
/// attenuate a component of frequency f by 10^(-3 / (f0 T60(f))).
inline LossCoeffs designLoss(double f0, double fs, double t60Low, double t60High, double cornerHz)
{
    f0 = std::max(f0, 1.0);
    t60Low = std::max(t60Low, 1.0e-3);
    t60High = std::min(std::max(t60High, 1.0e-3), t60Low);
    const double g0 = std::pow(10.0, -3.0 / (f0 * t60Low));
    const double gh = std::pow(10.0, -3.0 / (f0 * t60High));
    const double k = std::clamp(gh / g0, 1.0e-4, 1.0);
    LossCoeffs c;
    if (k < 0.999999)
    {
        const double sq = std::sqrt(k);
        const double top = 0.45 * fs;
        double fc = std::clamp(cornerHz, 1.0, top);
        // pole at fc*sqrt(k), zero at fc/sqrt(k); keep the zero below the top.
        double fz = fc / sq;
        if (fz > top)
        {
            fz = top;
            fc = fz * sq;
        }
        const double fp = fc * sq;
        const double cc = 2.0 * fs;
        const double wp = cc * std::tan(kPi * fp / fs);
        const double wz = cc * std::tan(kPi * fz / fs);
        const double kk = wp / wz;   // DC gain 1, Nyquist gain kk
        c.b0 = kk * (cc + wz) / (cc + wp);
        c.b1 = kk * (wz - cc) / (cc + wp);
        c.a1 = (wp - cc) / (cc + wp);
    }
    const double w0 = kTwoPi * f0 / fs;
    c.g = std::min(g0 / lossShape(c, w0), 0.999995);
    return c;
}

/// Least-squares fit of the dispersion allpass coefficient (M identical
/// first-order stages) to a stiff string's partial series. `maxDelay` bounds
/// the allpasses' share of the loop so the delay line keeps >= a few samples.
///
/// A cascade of first-order allpasses cannot follow the quadratic stretch of
/// a stiff string at low f0 — fitted over everything up to several kHz it
/// over-stretches partials 2-5 by 5-20 cents (measured: +6 / +12 / +19 cents
/// on the natural harmonics of an A string) to get the top right. Those low
/// partials are what beats in a chord and roughens through a distorted amp,
/// so the fit covers the first kFitPartials only, weighted 1/n^2: partials
/// 2-5 land within ~1-2 cents and the top end is under-stretched instead.
inline double fitDispersion(double f0, double fs, double B, int stages, const LossCoeffs& loss, double maxDelay)
{
    constexpr int kFitPartials = 12;
    if (B <= 0.0 || stages <= 0)
        return 0.0;
    const double period = fs / f0;
    int partials = 0;
    double fn[kFitPartials];
    double tauTarget[kFitPartials];
    for (int n = 1; n <= kFitPartials; ++n)
    {
        const double f = n * f0 * std::sqrt((1.0 + B * n * n) / (1.0 + B));
        if (f > 0.40 * fs)
            break;
        fn[partials] = f;
        tauTarget[partials] = n * fs / f;
        ++partials;
    }
    if (partials < 3)
        return 0.0;

    auto cost = [&](double a) {
        const double w1 = kTwoPi * fn[0] / fs;
        const double d = period - stages * allpassPhaseDelay(a, w1) - lossPhaseDelay(loss, w1);
        double err = 0.0;
        for (int i = 1; i < partials; ++i)
        {
            const double w = kTwoPi * fn[i] / fs;
            const double tau = d + stages * allpassPhaseDelay(a, w) + lossPhaseDelay(loss, w);
            const double rel = (tau - tauTarget[i]) / tauTarget[i];
            err += rel * rel / ((double) (i + 1) * (i + 1));
        }
        return err;
    };

    // The allpass DC delay (1-a)/(1+a) per stage must fit inside maxDelay.
    const double perStage = std::max(maxDelay / stages, 1.0);
    const double lo0 = std::max(-0.98, (1.0 - perStage) / (1.0 + perStage));
    if (lo0 >= 0.0)
        return 0.0;
    // Coarse grid, then golden section around the best cell.
    constexpr int kGrid = 64;
    int bestK = kGrid;
    double bestC = cost(0.0);
    for (int k = 0; k < kGrid; ++k)
    {
        const double c = cost(lo0 * (1.0 - (double) k / kGrid));
        if (c < bestC)
        {
            bestC = c;
            bestK = k;
        }
    }
    double lo = lo0 * (1.0 - (double) std::max(bestK - 1, 0) / kGrid);
    double hi = std::min(0.0, lo0 * (1.0 - (double) (bestK + 1) / kGrid));
    const double gr = 0.6180339887498949;
    double x1 = hi - gr * (hi - lo);
    double x2 = lo + gr * (hi - lo);
    double c1 = cost(x1);
    double c2 = cost(x2);
    for (int it = 0; it < 40; ++it)
    {
        if (c1 < c2)
        {
            hi = x2; x2 = x1; c2 = c1;
            x1 = hi - gr * (hi - lo); c1 = cost(x1);
        }
        else
        {
            lo = x1; x1 = x2; c1 = c2;
            x2 = lo + gr * (hi - lo); c2 = cost(x2);
        }
    }
    const double a = 0.5 * (lo + hi);
    return cost(a) < cost(0.0) ? a : 0.0;
}

/// The string's continuously-variable loop parameters, ramped per sample.
struct LoopTargets
{
    double delay = 100.0;   // D, samples (Lagrange read)
    double apA = 0.0;       // dispersion coefficient
    double g = 0.999;       // loss DC gain
    double b0 = 1.0;        // loss shelf
    double b1 = 0.0;
    double a1 = 0.0;
    double period = 100.0;  // fs / f, for the node damper's taps
};

class StringModel
{
public:
    static constexpr int kMaxStages = 4;
    static constexpr int kMaxNodeTaps = 10;

    /// Control thread. `lowestHz` bounds the delay line; `maxExcitation` is the
    /// longest excitation (samples) a note-on may queue.
    void prepare(double sampleRate, double lowestHz, int stages, int maxExcitation)
    {
        fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        stages_ = std::clamp(stages, 0, kMaxStages);
        const double longest = fs_ / std::max(lowestHz, 8.0) * 1.6 + 64.0;
        size_t size = 64;
        while ((double) size < longest)
            size <<= 1;
        buf_.assign(size, 0.0f);
        mask_ = (uint32_t) size - 1;
        maxDelay_ = (double) size - 8.0;
        exc_.assign((size_t) std::max(maxExcitation, 16), 0.0f);
        reset();
    }

    /// Audio-thread safe hard clear.
    void reset() noexcept
    {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        std::fill(exc_.begin(), exc_.end(), 0.0f);
        excLen_ = excPos_ = 0;
        w_ = 0;
        for (double& s : ap_) s = 0.0;
        loss_ = lossX1_ = 0.0;
        cur_ = LoopTargets {};
        step_ = LoopTargets {};
        primed_ = false;
        rampLeft_ = 0;
        nodeAlpha_ = nodeStep_ = 0.0;
        nodeK_ = 1;
        stopGain_ = 1.0;
        stopStep_ = 0.0;
        stopHold_ = 0;
        collide_ = collideStep_ = 0.0;
        collideLimit_ = 1.0;
        peak_ = 0.0f;
    }

    double sampleRate() const noexcept { return fs_; }
    int stages() const noexcept { return stages_; }
    int maxExcitation() const noexcept { return (int) exc_.size(); }

    /// Solves D for frequency f with the given dispersion + loss, then ramps
    /// every loop coefficient to it over `rampSamples` (>= 1). The first call
    /// after reset() jumps.
    void setTargets(double freqHz, double apA, const LossCoeffs& loss, int rampSamples) noexcept
    {
        const double f = std::clamp(freqHz, 8.0, fs_ * 0.45);
        const double w = kTwoPi * f / fs_;
        const double period = fs_ / f;
        LoopTargets t;
        t.apA = apA;
        t.g = loss.g;
        t.b0 = loss.b0;
        t.b1 = loss.b1;
        t.a1 = loss.a1;
        t.period = period;
        t.delay = period - stages_ * allpassPhaseDelay(apA, w) - lossPhaseDelay(loss, w);
        t.delay = std::clamp(t.delay, 2.0, maxDelay_);
        if (! primed_ || rampSamples <= 1)
        {
            cur_ = t;
            step_ = LoopTargets { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            rampLeft_ = 0;
            primed_ = true;
            return;
        }
        const double inv = 1.0 / rampSamples;
        step_.delay = (t.delay - cur_.delay) * inv;
        step_.apA = (t.apA - cur_.apA) * inv;
        step_.g = (t.g - cur_.g) * inv;
        step_.b0 = (t.b0 - cur_.b0) * inv;
        step_.b1 = (t.b1 - cur_.b1) * inv;
        step_.a1 = (t.a1 - cur_.a1) * inv;
        step_.period = (t.period - cur_.period) * inv;
        rampLeft_ = rampSamples;
    }

    /// Queues an excitation. Whatever is still unplayed of the previous one is
    /// kept (tremolo picks overlap their own attacks), so the caller must ADD
    /// (+=) its `length` samples into the returned buffer; index 0 is "now"
    /// and everything past the kept tail is zeroed.
    float* beginExcitation(int length) noexcept
    {
        length = std::clamp(length, 0, (int) exc_.size());
        const int remaining = excLen_ - excPos_;
        if (remaining > 0 && excPos_ > 0)
            std::copy(exc_.begin() + excPos_, exc_.begin() + excLen_, exc_.begin());
        const int keep = std::max(remaining, 0);
        std::fill(exc_.begin() + keep, exc_.begin() + std::max(keep, length), 0.0f);
        excPos_ = 0;
        excLen_ = std::max(keep, length);
        return exc_.data();
    }

    /// The real pick stops the string before it plucks it again: the loop's
    /// content is scaled to `residual` for exactly ONE pass of the current
    /// period (ramp down, hold, ramp up all inside it), so every sample of
    /// the old vibration is attenuated once and the new excitation — which
    /// comes back round one period later — is not touched at all. Call it
    /// AFTER the delay has been set to the new note.
    void pickStop(double residual) noexcept
    {
        const int period = (int) cur_.period;
        const int ramp = std::clamp((int) (0.0003 * fs_), 2, std::max(2, period / 8));
        stopTarget_ = std::clamp(residual, 0.0, 1.0);
        stopStep_ = (stopTarget_ - stopGain_) / ramp;
        stopRamp_ = ramp;
        stopHold_ = std::max(period - 2 * ramp - 4, 1);
        stopUpRamp_ = ramp;
    }

    /// A finger touching the string at 1/k of its length: only modes with a
    /// node there survive. alpha 1 = full touch; it is held for `holdSamples`
    /// then lifted over `releaseSamples`.
    void touchNode(int k, double alpha, int holdSamples, int releaseSamples) noexcept
    {
        nodeK_ = std::clamp(k, 1, kMaxNodeTaps);
        nodeAlpha_ = nodeK_ > 1 ? std::clamp(alpha, 0.0, 1.0) : 0.0;
        nodeHold_ = std::max(holdSamples, 0);
        nodeStep_ = nodeAlpha_ / std::max(releaseSamples, 1);
    }

    /// String-against-fret collision (slap/pop): a soft symmetric limit on the
    /// loop signal at `limit`, mixed by `amount`, fading over `fadeSamples`.
    void collide(double amount, double limit, int fadeSamples) noexcept
    {
        collide_ = std::clamp(amount, 0.0, 1.0);
        collideLimit_ = std::max(limit, 1.0e-4);
        collideStep_ = collide_ / std::max(fadeSamples, 1);
    }

    bool excitationPending() const noexcept { return excPos_ < excLen_; }
    int excitationRemaining() const noexcept { return std::max(excLen_ - excPos_, 0); }

    /// Peak |sample| of the excitation still queued (O(queued length)).
    float excitationPeak() const noexcept
    {
        float p = 0.0f;
        for (int i = excPos_; i < excLen_; ++i)
            p = std::max(p, std::fabs(exc_[(size_t) i]));
        return p;
    }
    double currentPeriod() const noexcept { return cur_.period; }

    /// Peak |output| since the last call — the caller's inactivity detector.
    float takePeak() noexcept
    {
        const float p = peak_;
        peak_ = 0.0f;
        return p;
    }

    /// Renders n samples, ADDING gain * output into out.
    void render(float* out, int n, float gain) noexcept
    {
        float* buf = buf_.data();
        const uint32_t mask = mask_;
        float peak = peak_;
        for (int i = 0; i < n; ++i)
        {
            if (rampLeft_ > 0)
            {
                cur_.delay += step_.delay;
                cur_.apA += step_.apA;
                cur_.g += step_.g;
                cur_.b0 += step_.b0;
                cur_.b1 += step_.b1;
                cur_.a1 += step_.a1;
                cur_.period += step_.period;
                --rampLeft_;
            }

            // Lagrange-3 fractional read, delay D in [2, size-8].
            const double D = cur_.delay;
            const int di = (int) D;
            const double f = D - di;
            const double d = 1.0 + f;   // position inside the 4-tap window
            const double h0 = -(d - 1.0) * (d - 2.0) * (d - 3.0) * (1.0 / 6.0);
            const double h1 = d * (d - 2.0) * (d - 3.0) * 0.5;
            const double h2 = -d * (d - 1.0) * (d - 3.0) * 0.5;
            const double h3 = d * (d - 1.0) * (d - 2.0) * (1.0 / 6.0);
            const uint32_t base = (w_ - (uint32_t) (di - 1)) & mask;
            double x = h0 * buf[base] + h1 * buf[(base - 1) & mask]
                     + h2 * buf[(base - 2) & mask] + h3 * buf[(base - 3) & mask];

            // Stiffness.
            const double a = cur_.apA;
            for (int s = 0; s < stages_; ++s)
            {
                const double y = a * x + ap_[s];
                ap_[s] = x - a * y;
                x = y;
            }

            // Frequency-dependent loss (shelf).
            loss_ = cur_.g * (cur_.b0 * x + cur_.b1 * lossX1_) - cur_.a1 * loss_;
            lossX1_ = x;
            double y = loss_;

            // Node damper (natural / pinch harmonics).
            if (nodeAlpha_ > 0.0)
            {
                const double step = cur_.period / nodeK_;
                double sum = y;
                for (int m = 1; m < nodeK_; ++m)
                {
                    const double dly = step * m;
                    const int ii = (int) dly;
                    const float fr = (float) (dly - ii);
                    const float s0 = buf[(w_ - (uint32_t) ii) & mask];
                    const float s1 = buf[(w_ - (uint32_t) ii - 1u) & mask];
                    sum += s0 + fr * (s1 - s0);
                }
                y = (1.0 - nodeAlpha_) * y + nodeAlpha_ * (sum / nodeK_);
                if (nodeHold_ > 0)
                    --nodeHold_;
                else
                    nodeAlpha_ = std::max(0.0, nodeAlpha_ - nodeStep_);
            }

            // String-fret collision: a smooth symmetric limiter in the loop.
            if (collide_ > 0.0)
            {
                const double lim = collideLimit_;
                const double c = lim * std::tanh(y / lim);
                y += collide_ * (c - y);
                collide_ = std::max(0.0, collide_ - collideStep_);
            }

            // Pick stop.
            if (stopRamp_ > 0)
            {
                stopGain_ += stopStep_;
                if (--stopRamp_ == 0)
                    stopGain_ = stopTarget_;
            }
            else if (stopHold_ > 0)
            {
                if (--stopHold_ == 0)
                {
                    stopTarget_ = 1.0;
                    stopStep_ = (1.0 - stopGain_) / stopUpRamp_;
                    stopRamp_ = stopUpRamp_;
                }
            }
            if (stopGain_ != 1.0)
                y *= stopGain_;

            if (excPos_ < excLen_)
                y += exc_[(size_t) excPos_++];

            const float v = (float) y;
            buf[w_] = v;
            w_ = (w_ + 1) & mask;
            out[i] += gain * v;
            const float av = v < 0.0f ? -v : v;
            if (av > peak)
                peak = av;
        }
        peak_ = peak;
        // Flush the recursive states out of the denormal range.
        if (std::fabs(loss_) < 1.0e-20) loss_ = 0.0;
        if (std::fabs(lossX1_) < 1.0e-20) lossX1_ = 0.0;
        for (int s = 0; s < stages_; ++s)
            if (std::fabs(ap_[s]) < 1.0e-20) ap_[s] = 0.0;
    }

    /// Zero the delay line and states (the string fell silent). O(size).
    void clearState() noexcept
    {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        for (double& s : ap_) s = 0.0;
        loss_ = lossX1_ = 0.0;
        nodeAlpha_ = 0.0;
        collide_ = 0.0;
        stopGain_ = 1.0;
        stopRamp_ = stopHold_ = 0;
    }

private:
    double fs_ = 48000.0;
    int stages_ = 2;
    std::vector<float> buf_;
    uint32_t mask_ = 0;
    uint32_t w_ = 0;
    double maxDelay_ = 64.0;
    double ap_[kMaxStages] {};
    double loss_ = 0.0;
    double lossX1_ = 0.0;
    LoopTargets cur_;
    LoopTargets step_;
    int rampLeft_ = 0;
    bool primed_ = false;

    std::vector<float> exc_;
    int excLen_ = 0;
    int excPos_ = 0;

    double nodeAlpha_ = 0.0;
    double nodeStep_ = 0.0;
    int nodeHold_ = 0;
    int nodeK_ = 1;

    double stopGain_ = 1.0;
    double stopStep_ = 0.0;
    double stopTarget_ = 1.0;
    int stopRamp_ = 0;
    int stopHold_ = 0;
    int stopUpRamp_ = 2;

    double collide_ = 0.0;
    double collideStep_ = 0.0;
    double collideLimit_ = 1.0;

    float peak_ = 0.0f;
};

} // namespace dcvoice
