// dcdsp_detail_os.h — private helpers shared by the drive-based cores
// (overdrive/distortion/amp/fuzz/bigmuff/klon). Requires the primitives header
// (kPi) to be included BEFORE this one. Header-only, RT-safe: the only
// allocation is the scratch buffer sized in prepare().
#ifndef DCDSP_DETAIL_OS_H
#define DCDSP_DETAIL_OS_H

#include <vector>
#include <cmath>
#include <complex>
#include <algorithm>

namespace dcdsp {
namespace detail {

// Hard NaN/Inf + overflow guard. The clamp bound (±16) is far above any musical
// level (~±1) so it never engages on a real signal — it only defuses a runaway
// feedback/denormal blow-up, keeping output well under the test's |x|<32 bound.
inline float sanitize(float x) {
    if (!std::isfinite(x)) return 0.f;
    if (x >  16.f) return  16.f;
    if (x < -16.f) return -16.f;
    return x;
}

// N-times oversampler: zero-stuff upsample + steep elliptic low-pass on the way
// up (anti-imaging) and down (anti-aliasing). The caller upsamples a block into
// `buf`, applies its nonlinearity to every sample of buf[0..n*factor), then
// downsamples back in place. This is TRUE oversampling (the nonlinearity runs
// at factor*sr).
//
// Filter design: each direction is an 8th-order elliptic lowpass (four biquad
// sections). Normalized to the BASE rate, the passband is 0..0.40*fsBase with
// 0.25 dB ripple and the stopband starts at 0.60*fsBase with >= 90 dB
// attenuation. 0.60*fsBase = fsBase - 0.40*fsBase is exactly the lowest
// frequency whose decimation fold lands back INSIDE the passband, so — like a
// classic half-band design — the transition band [0.40, 0.60]*fsBase is
// "don't care": its residual alias products can only land in the inaudible
// 0.40..0.50*fsBase region (19.2..24 kHz at 48 kHz). The previous 4th-order
// Butterworth managed only ~10-24 dB over the fold band; this design measures
// >= 90 dB (see the elliptic degree equation numbers at kProto below).
//
// IIR, so the wrap adds no buffering latency (latencySamples() of the host
// cores stays 0); the small frequency-dependent group delay of the two chains
// is exposed via groupDelayBaseSamples() so cores that mix a parallel DRY path
// against the oversampled wet path (e.g. the Klon clean blend) can time-align
// it and avoid combing.
//
// Sections keep DOUBLE coefficients AND double states: at factor 4 the
// passband edge sits at 0.1*fsOs where float-state biquads lose precision
// (same reason the shared RBJ Biquad must not run low-frequency filters at
// oversampled rates).
class OversamplerNx {
public:
    void prepare(double sampleRate, int maxFrames, int factor) {
        (void)sampleRate; // normalized design depends only on the factor
        factor_    = std::max(1, factor);
        maxFrames_ = std::max(1, maxFrames);
        buf_.assign((size_t)maxFrames_ * factor_, 0.f);
        gdBase_ = 0.0;
        if (factor_ > 1) {
            designSections();
            gdBase_ = computeGroupDelayBaseSamples();
        }
        reset();
    }
    void reset() {
        for (int i = 0; i < kSections; ++i) { up_[i].reset(); dn_[i].reset(); }
        std::fill(buf_.begin(), buf_.end(), 0.f);
    }
    int    factor()   const { return factor_; }
    float* data()           { return buf_.data(); }
    int    upSize(int n) const { return n * factor_; }

    // Low-frequency group delay of the full upsample->downsample round trip,
    // in BASE-rate samples (~1.7 at 2x, ~3.4 at 4x). For parallel dry paths.
    double groupDelayBaseSamples() const { return gdBase_; }

    // Zero-stuff by `factor`, then anti-imaging low-pass. Gain-compensated by
    // `factor` (zero stuffing drops passband level by 1/factor).
    void upsample(const float* x, int n) {
        if (factor_ == 1) { std::copy(x, x + n, buf_.begin()); return; }
        int m = 0;
        for (int i = 0; i < n; ++i) {
            buf_[m++] = runUp(x[i]);
            for (int k = 1; k < factor_; ++k)
                buf_[m++] = runUp(0.f);
        }
    }
    // Anti-alias low-pass then decimate by `factor` (keep last filtered sub-sample).
    void downsample(float* x, int n) {
        if (factor_ == 1) { std::copy(buf_.begin(), buf_.begin() + n, x); return; }
        for (int i = 0; i < n; ++i) {
            float last = 0.f;
            const int base = i * factor_;
            for (int k = 0; k < factor_; ++k)
                last = runDn(buf_[base + k]);
            x[i] = last;
        }
    }

private:
    // One biquad section, TDF-II, double coefficients + double states.
    struct Section {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        inline double process(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() { z1 = z2 = 0.0; }
    };

    static constexpr int kSections = 4;

    // 8th-order elliptic analog prototype (passband edge wp = 1, ripple
    // Ap = 0.25 dB; stopband edge ws = 1.5; the elliptic degree equation then
    // gives As = 90.5 dB). Designed offline (scipy.signal.ellipap) and verified:
    // |H(j1)| = -0.25 dB, max |H(jw)| for all w >= 1.5 is -90.54 dB.
    // Each row: { wz^2 (zero pair s^2 + wz^2), Re(pole), |pole|^2 }, pole Q
    // ascending. Bilinear-transformed per factor in designSections().
    static constexpr double kProto[kSections][3] = {
        { 2.315697301179043,  -0.30046231687210406,  0.14721507515594584 },
        { 2.995660406742242,  -0.22218866387560113,  0.4637254587559434  },
        { 6.021824244078745,  -0.12320357977042609,  0.8219086673010635  },
        { 45.05997772397408,  -0.038059831112971496, 1.025987431318757   },
    };
    static constexpr double kProtoGain    = 2.9715661867174975e-05;
    static constexpr double kPassbandFrac = 0.40;  // passband edge / fsBase

    void designSections() {
        // Bilinear transform with the analog passband edge (w = 1) prewarped to
        // the digital passband edge kPassbandFrac/factor of the OS rate.
        const double c  = 1.0 / std::tan(kPi * (kPassbandFrac / (double)factor_));
        const double c2 = c * c;
        // Spread the tiny prototype gain evenly across the sections so no
        // intermediate state sits ~5 orders of magnitude below the signal.
        // 10^(Ap/40) per chain lifts the even-order elliptic's -Ap DC point
        // back to exact unity (low-frequency gain match with the old design).
        const double gSec = std::pow(kProtoGain * std::pow(10.0, 0.25 / 20.0),
                                     1.0 / (double)kSections);
        for (int i = 0; i < kSections; ++i) {
            const double wz2 = kProto[i][0];
            const double a1a = -2.0 * kProto[i][1];
            const double a0a = kProto[i][2];
            const double A0  = c2 + a1a * c + a0a;
            Section s;
            s.b0 = gSec * (c2 + wz2) / A0;
            s.b1 = gSec * 2.0 * (wz2 - c2) / A0;
            s.b2 = s.b0;
            s.a1 = 2.0 * (a0a - c2) / A0;
            s.a2 = (c2 - a1a * c + a0a) / A0;
            up_[i] = s;
            dn_[i] = s;
        }
        // Zero-stuff make-up lives in the up chain only.
        const double f = (double)factor_;
        up_[0].b0 *= f; up_[0].b1 *= f; up_[0].b2 *= f;
    }

    // Phase slope of the up+down cascade near DC -> group delay. The decimator
    // keeps the LAST sub-sample of each group, which advances the output grid
    // by (factor-1) OS samples relative to the stuffing grid; subtract it.
    double computeGroupDelayBaseSamples() const {
        const double w1 = 0.001, w2 = 0.003;    // rad/sample at the OS rate
        const double tauChain = -(phaseAt(w2) - phaseAt(w1)) / (w2 - w1);
        return (2.0 * tauChain - (double)(factor_ - 1)) / (double)factor_;
    }
    double phaseAt(double w) const {
        const std::complex<double> zi = std::exp(std::complex<double>(0.0, -w));
        double phase = 0.0;
        for (int i = 0; i < kSections; ++i) {
            const Section& s = up_[i];
            const std::complex<double> num = s.b0 + (s.b1 + s.b2 * zi) * zi;
            const std::complex<double> den = 1.0  + (s.a1 + s.a2 * zi) * zi;
            phase += std::arg(num / den);        // each section ~real-positive near DC: no wrap
        }
        return phase;
    }

    inline float runUp(float v) {
        double d = (double)v + 1.0e-30;          // keep IIR states off subnormals
        for (int i = 0; i < kSections; ++i) d = up_[i].process(d);
        return (float)d;
    }
    inline float runDn(float v) {
        double d = (double)v + 1.0e-30;
        for (int i = 0; i < kSections; ++i) d = dn_[i].process(d);
        return (float)d;
    }

    Section up_[kSections], dn_[kSections];
    std::vector<float> buf_;
    int factor_ = 2, maxFrames_ = 1;
    double gdBase_ = 0.0;
};

} // namespace detail
} // namespace dcdsp

#endif // DCDSP_DETAIL_OS_H
