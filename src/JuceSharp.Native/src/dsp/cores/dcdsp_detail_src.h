// dcdsp_detail_src.h — arbitrary-ratio windowed-sinc sample-rate converter.
//
// A DETAIL header: no module, no catalogue entry. It exists for `namamp`, whose
// neural captures are trained at ONE rate (48 kHz for every .nam; whatever the
// .aidax was trained at) and are simply the wrong filter at any other rate. The
// module used to answer that by DRY-BYPASSING off its native rate, which is
// survivable for an optional pedal and fatal for a default amp: every user whose
// device refuses 48 kHz would hear clean DI.
//
// WHY NOT THE EXISTING TABLE. `dcdsp_droptune.h`'s SincTable is a shared static
// 512x32 fractional-DELAY table with a fixed 0.475 cutoff, correct because the
// drop tuner only ever reads at r <= 1 (interpolation, nothing to alias). A
// converter has to DECIMATE too — 88.2 kHz -> 48 kHz folds everything above
// 24 kHz back into the band — so the kernel cutoff has to track the ratio, and a
// lower cutoff needs a proportionally wider time support for the same transition
// width. The table is therefore per-instance and built in prepare().
//
// WHY NOT juce::Interpolators::WindowedSinc. It would work, but (a) it would put
// a framework dependency inside the dsp tree, which nothing else here has, (b)
// its algorithmic latency is 100 samples per direction — 4.5 ms round trip at
// 44.1 kHz on a live guitar path, where this design costs ~0.7 ms, and (c) the
// `tools/tone_response.cpp` harness compiles standalone with `cl` and cannot
// link JUCE, so a JUCE-based converter could not be measured with the same
// instrument as the rest of the rig.
//
// REAL-TIME CONTRACT. prepare() allocates (message thread). push()/pull() never
// allocate, lock or throw: the history buffer is sized for the worst case and
// compacted with a memmove.
#ifndef DCDSP_DETAIL_SRC_H
#define DCDSP_DETAIL_SRC_H

#include "dcdsp_detail_fir.h"   // firDot

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace dcdsp {
namespace detail {

class SincResampler {
public:
    static constexpr int kPhases   = 512;  // sub-sample phase resolution
    static constexpr int kBaseTaps = 32;   // taps at ratio <= 1 (interpolation)
    static constexpr int kMaxTaps  = 256;  // ceiling for extreme decimation

    /// Message thread. `inRate`/`outRate` in Hz; `maxPush` is the largest run a
    /// single push() will ever carry. Safe to call repeatedly.
    void prepare(double inRate, double outRate, int maxPush)
    {
        ratio_ = (inRate > 0.0 && outRate > 0.0) ? (inRate / outRate) : 1.0;
        if (!(ratio_ > 0.0) || !std::isfinite(ratio_)) ratio_ = 1.0;

        // Kernel cutoff sits at the LOWER of the two Nyquists (anti-imaging on
        // the way up, anti-aliasing on the way down), expressed as a fraction of
        // the INPUT rate: 0.475 when interpolating, 0.475/ratio when decimating.
        const double dec = std::max(1.0, ratio_);
        int taps = (int) std::lround((double) kBaseTaps * dec);
        taps = (taps + 3) & ~3;                       // keep the dot product tidy
        taps_ = std::clamp(taps, kBaseTaps, kMaxTaps);
        half_ = taps_ / 2;

        buildTable(dec);

        const int push = std::max(1, maxPush);
        cap_ = taps_ + 2 * push + 16;
        buf_.assign((size_t) cap_, 0.0f);
        reset();
    }

    /// Clear the history. The next output resumes from a cold kernel.
    void reset()
    {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        filled_ = 0;
        pos_    = (double) (half_ - 1);
    }

    /// Delay this converter adds, in INPUT samples.
    int latencyInSamples() const { return half_ - 1; }

    /// Input samples consumed per output sample.
    double ratio() const { return ratio_; }

    /// AUDIO THREAD. Append `n` input samples. Never grows the buffer: an
    /// over-long run drops the OLDEST history rather than allocate (it cannot
    /// happen with a correctly-sized prepare, and a dropout beats a malloc).
    void push(const float* x, int n)
    {
        if (x == nullptr || n <= 0 || cap_ <= 0) return;
        if (n > cap_) { x += (n - cap_); n = cap_; }
        if (filled_ + n > cap_)
        {
            const int drop = filled_ + n - cap_;
            std::memmove(buf_.data(), buf_.data() + drop, sizeof(float) * (size_t) (filled_ - drop));
            filled_ -= drop;
            pos_ -= (double) drop;
            if (pos_ < (double) (half_ - 1)) pos_ = (double) (half_ - 1);
        }
        std::memcpy(buf_.data() + filled_, x, sizeof(float) * (size_t) n);
        filled_ += n;
    }

    /// AUDIO THREAD. Output samples producible from what has been pushed.
    int available() const
    {
        if (filled_ < taps_) return 0;
        const double lastCentre = (double) (filled_ - 1 - half_);
        if (pos_ > lastCentre) return 0;
        const int k = (int) std::floor((lastCentre - pos_) / ratio_) + 1;
        return k > 0 ? k : 0;
    }

    /// AUDIO THREAD. Produce up to `n` output samples; returns how many were
    /// written. Alloc-free.
    int pull(float* out, int n)
    {
        if (out == nullptr || n <= 0) return 0;
        const int k = std::min(n, available());
        for (int j = 0; j < k; ++j)
        {
            const int    i0 = (int) pos_;
            const double f  = pos_ - (double) i0;
            int ph = (int) (f * (double) kPhases);
            if (ph < 0) ph = 0;
            else if (ph >= kPhases) ph = kPhases - 1;

            out[j] = firDot(buf_.data() + (i0 - half_ + 1),
                            table_.data() + (size_t) ph * (size_t) taps_,
                            taps_);
            pos_ += ratio_;
        }
        compact();
        return k;
    }

private:
    void compact()
    {
        const int drop = (int) pos_ - (half_ - 1);
        if (drop <= 0) return;
        const int keep = filled_ - drop;
        if (keep > 0)
            std::memmove(buf_.data(), buf_.data() + drop, sizeof(float) * (size_t) keep);
        filled_ = keep > 0 ? keep : 0;
        pos_ -= (double) drop;
    }

    void buildTable(double dec)
    {
        table_.assign((size_t) kPhases * (size_t) taps_, 0.0f);

        const double beta   = 8.6;              // Kaiser, ~ -90 dB sidelobes
        const double cutoff = 0.475 / dec;      // fraction of the INPUT rate
        const double i0beta = besselI0(beta);

        for (int p = 0; p < kPhases; ++p)
        {
            const double frac = (double) p / (double) kPhases;
            double sum = 0.0;
            for (int i = 0; i < taps_; ++i)
            {
                // Tap i sits at offset (i - taps/2 + 1) - frac from the read point.
                const double x = (double) (i - taps_ / 2 + 1) - frac;
                const double s = sinc(2.0 * cutoff * x) * 2.0 * cutoff;

                const double r   = (2.0 * (double) i / (double) (taps_ - 1)) - 1.0;
                const double arg = 1.0 - r * r;
                const double w   = besselI0(beta * std::sqrt(arg > 0.0 ? arg : 0.0)) / i0beta;

                const double v = s * w;
                table_[(size_t) p * (size_t) taps_ + (size_t) i] = (float) v;
                sum += v;
            }
            // Unity DC gain at EVERY phase, or the table amplitude-modulates the
            // signal as the read phase sweeps (an audible warble of its own).
            if (sum > 1e-12)
            {
                const float inv = (float) (1.0 / sum);
                for (int i = 0; i < taps_; ++i)
                    table_[(size_t) p * (size_t) taps_ + (size_t) i] *= inv;
            }
        }
    }

    static double sinc(double x)
    {
        if (std::abs(x) < 1e-9) return 1.0;
        const double pix = 3.14159265358979323846 * x;
        return std::sin(pix) / pix;
    }

    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (x * x) / (4.0 * (double) k * (double) k);
            sum += term;
            if (term < 1e-14 * sum) break;
        }
        return sum;
    }

    std::vector<float> table_;
    std::vector<float> buf_;
    double ratio_  = 1.0;
    double pos_    = 0.0;
    int    taps_   = kBaseTaps;
    int    half_   = kBaseTaps / 2;
    int    cap_    = 0;
    int    filled_ = 0;
};

} // namespace detail
} // namespace dcdsp

#endif // DCDSP_DETAIL_SRC_H
