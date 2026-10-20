// dcdsp_detail_pitch.h — private monophonic period tracker shared by the
// pitch-DRIVEN exotic cores (subharmonic, feedbacker). Requires the primitives
// header (OnePole, EnvelopeFollower) to be included BEFORE this one.
// Header-only, RT-safe: no allocation, no locks, no I/O, no branches on denormals.
//
// WHY a hysteresis zero-cross tracker and not autocorrelation / YIN / a spectral
// method: these cores need the period at ATTACK latency, not at ANALYSIS latency.
// Resolving a guitar's fundamental by correlation needs a window of several
// periods — ~48 ms for a low E — which is the same window theory that bans a
// phase vocoder from the live input everywhere else in this repo. A Schmitt
// comparator on a low-passed copy locks within ONE period (~12 ms on a low E,
// ~3 ms on the high strings) and costs two one-poles.
//
// It is MONOPHONIC-ONLY, which is exactly what a sub-octave / feedback pedal is
// (every real one is). A strummed chord gives it a period it cannot use; both
// consumers gate on `confident()` and hold the last valid period, so a chord
// degrades to "keeps ringing the last note" rather than to noise.
#ifndef DCDSP_DETAIL_PITCH_H
#define DCDSP_DETAIL_PITCH_H

#include <cmath>
#include <algorithm>

namespace dcdsp {
namespace detail {

// Hysteresis zero-cross period tracker for a monophonic guitar signal. Feed it
// one mono sample per frame; it reports the smoothed period, the tracked
// frequency, the input envelope, and a per-period rising "clock" edge (which is
// what a divider counts and what a phase-locked oscillator syncs to).
class PeriodTracker {
public:
    void prepare(double sampleRate) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        // The comparator sees a low-passed copy: the pick attack's HF hash would
        // otherwise clock several false edges per period (this is exactly what
        // dcdsp_octaver.h does, and for the same reason).
        track_.setLowpass(sr_, 1100.0);
        env_.prepare(sr_);
        env_.setTimes(2.0f, 80.0f);   // fast attack, moderate release
        minPeriod_ = static_cast<float>(sr_ / kMaxHz);
        maxPeriod_ = static_cast<float>(sr_ / kMinHz);
        reset();
    }

    void reset() {
        track_.reset();
        env_.reset();
        high_      = false;
        sinceRise_ = 0;
        period_    = 0.f;
        amp_       = 0.f;
        confident_ = false;
    }

    // One mono sample. Returns true on a RISING comparator edge.
    bool process(float x) {
        if (!std::isfinite(x)) x = 0.f;
        x = std::min(8.f, std::max(-8.f, x));

        amp_ = env_.process(x);
        const float t = track_.process(x);

        if (sinceRise_ < kSinceCap) ++sinceRise_;

        // Amplitude-relative hysteresis: no chatter as a note decays, no false
        // clocks in the noise floor.
        const float h = 0.08f * amp_ + 1.0e-4f;

        bool rise = false;
        if (!high_ && t > h) {
            high_ = true;
            rise  = true;
            const float p = static_cast<float>(sinceRise_);
            if (p >= minPeriod_ && p <= maxPeriod_ && amp_ > kAmpFloor) {
                if (!confident_ || std::fabs(p - period_) > 0.5f * period_)
                    period_ = p;                        // new note / octave jump
                else
                    period_ += 0.35f * (p - period_);   // same note: smooth
                confident_ = true;
            }
            sinceRise_ = 0;
        } else if (high_ && t < -h) {
            high_ = false;
        }

        // Real silence invalidates the period. A stale one would make the next
        // pick bloom at the PREVIOUS note's pitch.
        if (amp_ <= kAmpFloor) confident_ = false;

        return rise;
    }

    bool  confident()     const { return confident_ && period_ > 1.f; }
    float periodSamples() const { return period_; }
    float frequencyHz()   const {
        return (period_ > 1.f) ? static_cast<float>(sr_) / period_ : 0.f;
    }
    float amplitude()     const { return amp_; }

private:
    static constexpr double kMinHz    = 55.0;     // under a 7-string low B (61 Hz)
    static constexpr double kMaxHz    = 1400.0;   // over the 24th-fret high E
    static constexpr float  kAmpFloor = 3.0e-4f;  // ~-70 dBFS: silence
    static constexpr int    kSinceCap = 1 << 20;  // counter saturation

    double sr_ = 48000.0;
    OnePole          track_{};
    EnvelopeFollower env_{};
    float minPeriod_ = 34.f;
    float maxPeriod_ = 873.f;
    bool  high_      = false;
    int   sinceRise_ = 0;
    float period_    = 0.f;
    float amp_       = 0.f;
    bool  confident_ = false;
};

} // namespace detail
} // namespace dcdsp

#endif // DCDSP_DETAIL_PITCH_H
