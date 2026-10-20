// dcdsp_droptune.h — Correlation-Spliced Variable Delay (CSVD)
//
// A DOWN-ONLY pitch shifter for the live guitar input: a drop-tune pedal.
//
// ===========================================================================
// WHY NOT A PHASE VOCODER (do not "upgrade" this to one)
//
// Resolving a guitar's harmonics in an STFT needs a window of N > 4*fs/f0:
// 2330 samples (48 ms) for a low E, 3112 (65 ms) for a 7-string low B. That is
// window theory, not implementation quality. It is why Rubber Band's own docs
// call LiveShifter "not a low-latency effect, with a delay of 50ms or more" and
// why Signalsmith's default block is 120 ms. Both are excellent engines. Neither
// can be PLAYED through. (They remain the right tool for offline work.)
//
// ===========================================================================
// WHY DOWN-ONLY IS FREE
//
// Read a delay line at rate r < 1 and everything moves down by r: that IS the
// pitch shift. The read head therefore falls BEHIND the write head, and when the
// delay grows too far we jump it FORWARD — into audio that is already written.
// Nothing is ever looked ahead for.
//
// Up-shifting is the direction that needs a buffered period (the read head must
// jump BACKWARD). Same asymmetry in the wild: a Line 6 HX FX measures <3 ms
// octave-DOWN against 12 ms octave-UP on the same algorithm.
//
//     W = write index (advances 1 per input sample)
//     R = fractional read position (advances r per output sample)
//     D = W - R                       dD/dn = 1 - r  > 0
//
//     splice when D outgrows its ceiling, jumping R forward by a waveform-matched
//     lag L:   D stays in [C, C+L],  splice rate = (1-r)*fs/L
//
// At -1 semitone that is 2.8 splices/second: the output is a pure resampled copy
// of the input 99.4% of the time.
//
// ===========================================================================
// WHAT MAKES IT SOUND GOOD (this is the part that matters)
//
// Latency was never the hard problem — the SOUND is. Six things carry it:
//
//  1. WINDOWED-SINC INTERPOLATION, not Lagrange/linear. Reading the line at r < 1
//     is interpolation, so the interpolator IS the reconstruction filter and its
//     error lands directly on the output as a rate-dependent HF droop — i.e. the
//     shifted signal sounds dull and veiled, worst at the octave (r = 0.5) where
//     you need it most. A 32-tap Kaiser-windowed sinc with 512 phases is flat to
//     ~0.45*fs and costs 32 MACs/sample. This alone is most of the "why does it
//     sound worse than the pedal" gap.
//
//  2. BAND-LIMITED CORRELATION. Guitar periodicity lives below ~1.2 kHz; the
//     upper harmonics only make the similarity surface noisy and pull the splice
//     onto a wrong (harmonic-multiple) lag. We correlate on a low-passed, x4
//     decimated MID signal, so the splice locks to the true period of the material.
//
//     A POWER CHORD IS *NOT* PERIODIC AT THE ROOT PERIOD. This comment used to
//     claim it was, and everything downstream of that claim was wrong. A fifth
//     sits at 1.5*f0, which is not a harmonic of f0: lcm(T, 2T/3) = 2T. So a
//     root-period splice returns the fifth INVERTED — measured NCC 0.553 at
//     12.17 ms against 0.999 at 24.25 ms on an E5. The composite period of real
//     material, measured:
//         low E, one note ....... 12.1 ms  (= the root period)
//         E5 power / E maj / E min  24.3 ms  (2x the root period)
//         open G ................ 40.8 ms  (4x the root period)
//         a bare major third .... 48.3 ms  (4x the root period)
//     A ceiling below the composite period cannot align the chord AT ALL, and a
//     10 ms ceiling is below one period of even a single low E. That is why the
//     ceiling is ADAPTIVE (see 5).
//
//  3. ONE READ HEAD FOR BOTH CHANNELS. Splicing each channel independently gives
//     them different lags, so the stereo image wobbles and partially collapses at
//     every splice. Both channels share one read position, one lag, one crossfade.
//
//  4. SPLICE AT THE BEST MOMENT, NOT THE FIRST. Once D passes the ceiling we have
//     until D_hard before we MUST splice. Spend that window looking for the moment
//     with the best waveform match and the least energy — a splice under a quiet
//     moment is inaudible; the same splice under a loud sustain is not. This is
//     free look-ahead: it costs no latency because it is all in the past.
//
//  5. AN ADAPTIVE LAG CEILING. A FIXED ceiling is a guess about the material, and
//     the numbers above show every fixed guess is wrong for something. So we do not
//     guess: a periodicity probe measures the composite period of what is actually
//     playing — backward from the WRITE head, on the same decimated mid, so it costs
//     no latency and is bounded by nothing — and the ceiling is raised to it. The
//     `maxLagMs` knob becomes a FLOOR: it still buys a longer (smoother, less
//     frequent) splice on material that does not need one, but it can no longer sit
//     BELOW the period and force every splice to be a mismatch.
//     The probe takes the SMALLEST lag that correlates as well as the best one: a
//     periodic signal matches at every multiple of its period, and without that rule
//     the probe would happily return 5x the period and balloon the sustain delay.
//
//  6. THE CROSSFADE LAW FOLLOWS THE SPLICE'S OWN CORRELATION. Neither classic law
//     is right for both cases: amplitude-complementary (1-t, t) sums CORRELATED
//     content to exactly unity but dips -3 dB mid-fade on uncorrelated content;
//     equal-power (cos/sin) is the reverse. A matched splice is the first case and
//     a forced jump-cut is the second — and we KNOW which we are doing, because
//     the correlator just measured the NCC. Holding output power at unity for
//     correlation rho gives  gA = -rho*gB + sqrt(rho^2*gB^2 + 1 - gB^2),  which
//     reduces EXACTLY to linear at rho=1 and to equal-power at rho=0. (The old
//     always-linear law measured up to 13.8 dB of envelope dent on real palm-muted
//     material, where forced splices are the norm; the always-cos/sin law before
//     it was worth 9.6-20.8 dB of sine warble. Each was right about the other.)
//
//  7. A POOR MATCH HUNTS LONGER, AND A HOPELESS ONE CUTS ONCE. The 30 ms splice
//     window is a good bound only when a decent match exists. When it does not
//     (palm mutes; wide-interval chords — the poly harness's oracle PROVES no
//     good lag exists there), forcing a bad splice every window just schedules
//     the next bad splice. So: below kDecentScore the hunt continues while the
//     delay has headroom (ceil + ~22 ms — on a riff the next pick's anchor
//     usually discharges everything under the attack first, which is the perfect
//     splice), and a still-hopeless match (kHopelessScore) discharges the WHOLE
//     delay in one equal-power jump-cut instead of a chain of capped bad cuts.
//     The headroom bound (not a time bound) is what keeps a MISSED pick's attack
//     from ever coming out later than ceil + 22 ms.
//
// LATENCY IS FELT AT THE ATTACK, NOT DURING SUSTAIN. An onset detector snaps the read
// head forward at every pick (~1.5-4 ms), so palm mutes and fast riffs are the BEST
// case, not the worst. The attack comes out ALREADY SHIFTED — we never punch dry,
// unshifted audio through, which is what the old "transient duck" did and is the
// DigiTech Drop's most-complained-about artifact ("I expect BAM, I hear BA-BAM": the
// attack at the wrong pitch, the shifted sustain behind it). Raising the lag ceiling
// therefore costs SUSTAIN delay, never attack latency — the anchor sets that.
//
// ===========================================================================
// THE ONSET DETECTOR IS THE WHOLE BALL GAME — AND IT MUST COMPARE AGAINST THE PAST
//
// This is the bug that made the pedal sound bad for a year, so it is written down.
//
// The detector used to compare a 0.5 ms-release PEAK follower of the >1.2 kHz
// residual against 2.5x a 200 ms MEAN *of the same signal at the same instant*.
// Nothing in that asks whether the level went UP. It is a CREST-FACTOR detector, and
// a ringing guitar chord has an HF crest of ~2.5-2.9 — right on the threshold. Worse,
// a 0.5 ms release falls back to the instantaneous value between waveform peaks, so it
// re-armed every period of any note below 500 Hz. Measured on synthesized chords:
// ~20 anchors/second against <=2 real picks/second. Each false anchor does an
// UNMATCHED jump *and* clears `pending_`, cancelling the pending correlation splice —
// so the mechanism section "WHAT MAKES IT SOUND GOOD" is about ran at literally
// 0.0 splices/s on every chord tested. A sine has no HF crest and cannot trigger it,
// which is precisely why a single-sine harness measured -43 dBc warble and declared
// victory. (tools/droptune_poly.cpp exists so that can never happen again.)
//
// A PICK IS A RISE. A rise exists only relative to the PAST, so the detector compares
// the fast HF envelope against the SAME envelope ~35 ms ago and requires an actual
// increase (>1.5x). (`mikustomp` had already learned the other half of this — "compare
// against the pitch ~40 ms ago" — and this file took the HF-envelope half and skipped
// it.) The fast follower's RELEASE is load-bearing too: at 50 ms it holds the pick's
// peak instead of collapsing back onto the waveform every period. And the lookback
// MUST stay below the retrigger lock: if `past` is still the pre-pick level when the
// lock expires, the detector re-fires forever — the old bug in a new costume.
//
// ===========================================================================
// HONEST LIMIT
//
// A sustained, ringing chord whose composite period exceeds the 65 ms hard ceiling
// (a very wide voicing, or a very low tuning) still cannot be spliced cleanly, and
// no sub-20 ms time-domain algorithm solves that — the Drop does not either, and it
// is precisely what its users complain about. Everything reachable under 65 ms now is.
//
// MINOR-THIRD CHORDS ARE THE CEILING, AND MULTIBAND DOES NOT RAISE IT. An open
// E minor's best full-band alignment is NCC ~0.81 at ANY lag <= 70 ms (measured:
// string stiffness makes the partials inharmonic, so the 6:5 interval never
// re-phases exactly), and every splice phase-rotates the misaligned partials —
// periodic phase jumps splatter each partial's spectral line into splice-rate
// sidebands, which is why Em measures ~14 dB more artifact than a native
// down-tuned reference while E major measures ~5 dB. The obvious "fix" — split
// into bands and give each band its own lag — was MEASURED DEAD before being
// built: per-band best NCC (LR4 splits at 400/1600 Hz, and 2-band at 800 Hz)
// does not beat full-band on any worst-case chord (Em 0.811 full vs
// 0.865/0.823/0.776 per band; a bare major third is 0.988 full and the bands
// are WORSE, hi>1.6k = 0.442). The misalignment is PARTIAL-level, not
// band-level; per-partial realignment is a phase vocoder, and the top of this
// file explains why that cannot be low-latency. For scale: the real DigiTech
// Drop measures 12-17 ms of flat latency and its own users report warble on
// exactly this material ("exaggerates an out-of-tune string", worst on clean
// low-register chords, masked by high gain) — the ceiling is the field's, not
// this implementation's.
//
// Prior art, all EXPIRED: Eventide US4,464,784 (autocorrelation splice search),
// Motorola US5,644,677 (variable buffer + interpolator), PSOLA (CNET), IVL
// US5,567,901 (the Whammy lineage). WSOLA was never patented.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace dcdsp {

// ---------------------------------------------------------------------------
// Windowed-sinc polyphase fractional-delay table.
//
// Built once, shared by every instance. 512 phases x 32 taps: the phase
// quantisation error is then far below the 32-tap sinc's own stopband, so the
// table is transparent for our purposes.
// ---------------------------------------------------------------------------
class SincTable {
public:
    static constexpr int kTaps = 32;
    static constexpr int kPhases = 512;

    static const SincTable& get() {
        static const SincTable t;
        return t;
    }

    /// taps for phase p (0..kPhases-1), kTaps consecutive floats.
    const float* at(int phase) const { return &t_[(size_t) phase * kTaps]; }

private:
    SincTable() {
        t_.resize((size_t) kPhases * kTaps);

        // Kaiser window, beta 8.6 (~ -90 dB sidelobes). Cutoff just under Nyquist:
        // we only ever READ at r <= 1, so there is nothing to alias — the job is
        // image suppression / flat passband, not decimation.
        const double beta = 8.6;
        const double cutoff = 0.475;  // of fs
        const double i0beta = besselI0(beta);

        for (int p = 0; p < kPhases; ++p) {
            const double frac = (double) p / kPhases;
            double sum = 0.0;
            for (int i = 0; i < kTaps; ++i) {
                // Tap i sits at offset (i - kTaps/2 + 1) - frac from the read point.
                const double x = (double) (i - kTaps / 2 + 1) - frac;
                const double s = sinc(2.0 * cutoff * x) * 2.0 * cutoff;

                const double r = (2.0 * (double) i / (kTaps - 1)) - 1.0;
                const double arg = 1.0 - r * r;
                const double w = besselI0(beta * std::sqrt(arg > 0.0 ? arg : 0.0)) / i0beta;

                const double v = s * w;
                t_[(size_t) p * kTaps + i] = (float) v;
                sum += v;
            }
            // Normalise to unity DC gain so a constant input passes at exactly 1.0
            // at every phase — otherwise the table itself amplitude-modulates the
            // signal as the read phase sweeps, which is a warble all of its own.
            if (sum > 1e-12) {
                const float inv = (float) (1.0 / sum);
                for (int i = 0; i < kTaps; ++i) t_[(size_t) p * kTaps + i] *= inv;
            }
        }
    }

    static double sinc(double x) {
        if (std::abs(x) < 1e-9) return 1.0;
        const double pix = 3.14159265358979323846 * x;
        return std::sin(pix) / pix;
    }

    static double besselI0(double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k) {
            term *= (x * x) / (4.0 * k * k);
            sum += term;
            if (term < 1e-14 * sum) break;
        }
        return sum;
    }

    std::vector<float> t_;
};

struct DropTuneParams {
    float semitones = 0.0f;  // <= 0. 0 == bypass.
    /// Splice-lag FLOOR (it used to be a ceiling — see note 5 in the header).
    /// The core raises the real ceiling to the composite period it measures in the
    /// material, because a ceiling below that period cannot align the chord at all.
    /// This knob only says how much SMOOTHER than strictly necessary you want it:
    /// bigger = a longer splice lag where the material allows one = fewer splices
    /// and more sustain delay. Attack latency does NOT depend on it — the transient
    /// anchor sets that.
    float maxLagMs = 20.0f;
};

class DropTune {
public:
    void prepare(double sampleRate, int maxBlock, int channels) {
        // Warm the shared sinc table HERE (message/device thread), not on the
        // audio thread. The table is a Meyers singleton whose first get()
        // builds 512x32 taps of Kaiser/Bessel/sinc math behind the
        // thread-safe-static init lock — and the only other call site is
        // process() after the pedal is FIRST engaged, i.e. the audio callback
        // (2026-07-16 audit #2: a guaranteed dropout on first engage).
        (void) SincTable::get();

        sr_ = sampleRate > 0 ? (float) sampleRate : 48000.0f;
        (void) maxBlock;
        channels_ = std::clamp(channels, 1, 2);

        ringSize_ = 1;
        while (ringSize_ < (int) (sr_ * 0.25f)) ringSize_ <<= 1;  // >= 250 ms
        mask_ = ringSize_ - 1;
        for (auto& b : buf_) b.assign((size_t) ringSize_, 0.0f);

        // Analysis ring: low-passed, x4-decimated mid. Correlating here rather than
        // on the full-band signal is what makes the splice lock to the real period.
        decim_ = 4;
        aRate_ = sr_ / decim_;
        aSize_ = 1;
        while (aSize_ < (int) (aRate_ * 0.25f)) aSize_ <<= 1;
        aMask_ = aSize_ - 1;
        ana_.assign((size_t) aSize_, 0.0f);

        // 2-pole Butterworth-ish LPF at 1.2 kHz on the mid, pre-decimation.
        {
            const float w = std::tan(3.14159265f * 1200.0f / sr_);
            const float k = 1.41421356f;  // Q = 1/sqrt(2)
            const float n = 1.0f / (1.0f + k * w + w * w);
            lpB0_ = w * w * n;
            lpB1_ = 2.0f * lpB0_;
            lpB2_ = lpB0_;
            lpA1_ = 2.0f * (w * w - 1.0f) * n;
            lpA2_ = (1.0f - k * w + w * w) * n;
        }

        xfade_ = (int) (sr_ * 0.0053f);      // 5.3 ms splice crossfade
        snapXfade_ = (int) (sr_ * 0.001f);   // 1 ms at an attack
        anchorDelay_ = (int) (sr_ * 0.0013f);// 1.3 ms — the attack's own latency
        minLag_ = (int) (sr_ * 0.003f);
        spliceWindow_ = (int) (sr_ * 0.030f);// at most 30 ms hunting, for a DECENT match
        huntExtra_ = (int) (sr_ * 0.022f);   // a POOR match hunts until delay = ceil + this

        // The hard ceiling on the adaptive lag: 65 ms reaches the composite period of
        // every chord shape measured (the worst, a bare major third, is 48.3 ms) with
        // room to spare, and still leaves the delay far inside the ring.
        absMaxLag_ = std::min((int) (sr_ * kAbsMaxLagMs * 0.001f), ringSize_ / 4);

        // The delay must be allowed to grow past the ceiling plus the crossfade plus
        // whatever it accrues while we hunt for a good moment; anything less and
        // `mustGo` fires before the correlator has had its window.
        hardDelay_ = absMaxLag_ + xfade_ + spliceWindow_ + (int) (sr_ * 0.010f);

        // Correlation window: two periods of the lowest note we care about
        // (7-string low B ~61 Hz => 33 ms). It looks BACKWARD from the read head, so
        // every sample it needs is already written: it costs no latency at all.
        aCorr_ = (int) (aRate_ * 0.033f);

        // Periodicity probe (see header note 5): a backward correlation from the
        // WRITE head over the whole legal lag range. Throttled — the material's
        // period does not change on a millisecond timescale.
        aProbeMax_ = absMaxLag_ / decim_;
        aProbeMin_ = std::max(1, minLag_ / decim_);
        probeScore_.assign((size_t) (aProbeMax_ + 1), 0.0f);
        probeInterval_ = (int) (sr_ * 0.032f);

        // Onset: the HF envelope, not a broadband one. Re-picking a ringing string
        // barely moves the broadband envelope but jumps the >1.2 kHz envelope
        // several-fold, and peaks 1-5 ms after the pick rather than 6-20 ms.
        hpCoef_ = std::exp(-2.0f * 3.14159265f * 1200.0f / sr_);
        fastAtk_ = std::exp(-1.0f / (kFastAttackMs * 0.001f * sr_));
        fastRel_ = std::exp(-1.0f / (kFastReleaseMs * 0.001f * sr_));
        retrigger_ = (int) (sr_ * 0.040f);

        // The delayed envelope the rise test compares against: "the level 30 ms ago".
        onsetLookback_ = (int) (sr_ * kOnsetLookbackMs * 0.001f);
        oSize_ = 1;
        while (oSize_ <= onsetLookback_ + 1) oSize_ <<= 1;
        oMask_ = oSize_ - 1;
        onsetEnv_.assign((size_t) oSize_, 0.0f);

        // maxLag_ is a SAMPLE COUNT derived from the rate, and prepare() is
        // where every other one of those is re-derived. It used to be written
        // only by setParams, so a device change to another rate left the splice
        // lag at the old sample count — half the requested milliseconds at
        // twice the rate — until the next knob write. (On a very first prepare
        // it was 0, which effLag() then clamped; harmless only because rate_
        // defaults to bypassed.)
        applyMaxLag();

        reset();
    }

    void reset() {
        for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
        std::fill(ana_.begin(), ana_.end(), 0.0f);
        std::fill(onsetEnv_.begin(), onsetEnv_.end(), 0.0f);
        w_ = 0; aw_ = 0; decCount_ = 0; aFill_ = 0;
        r_ = 0.0; rOld_ = 0.0;
        fade_ = 0; fadeLen_ = 0;
        unityFade_ = 0; unityFadeLen_ = 0;
        lpZ1_ = lpZ2_ = 0.0f;
        hpZ_ = fastEnv_ = 0.0f;
        ow_ = 0;
        armed_ = true; sinceOnset_ = 1 << 20;
        lastLag_ = 0; pending_ = false; pendingFor_ = 0; searchTick_ = 0; bestLag_ = 0; bestScore_ = -1e30f;
        bestNcc_ = 0.0f; fadeRho_ = 1.0f;
        periodLag_ = 0; probeTick_ = 0;
        bypassed_ = true;
    }

    void setParams(const DropTuneParams& p) {
        // Up-shifting is not supported and is not what this is for: the entire
        // no-look-ahead argument rests on r < 1.
        rate_ = std::exp2(std::clamp(p.semitones, -24.0f, 0.0f) / 12.0f);
        maxLagMs_ = p.maxLagMs;
        applyMaxLag();
    }

    /// maxLag_ in SAMPLES from the last requested milliseconds. Called from
    /// both setParams and prepare, so the lag survives a sample-rate change.
    void applyMaxLag() {
        const int hi = std::max(minLag_ + 1, absMaxLag_);
        maxLag_ = std::clamp((int) (sr_ * maxLagMs_ * 0.001f), minLag_ + 1, hi);
    }

    bool isBypassed() const { return bypassed_; }

    /// In-place, `channels` pointers of `numSamples`.
    void process(float* const* data, int channels, int numSamples) {
        const int nch = std::min(channels, channels_);

        if (std::abs(rate_ - 1.0f) < 1e-4f) {
            if (!bypassed_) {
                // DISENGAGING is a splice, and it was an uncrossfaded one. The
                // output up to this sample was a sinc read up to a full ceiling
                // behind the write head; the next sample was the dry input.
                // Every other head movement in this file is crossfaded
                // (fadeLen_/fadeRho_), and `semitones` is written per block from
                // the callback, so a knob crossing 0 cut at one sample. Arm the
                // same snapXfade_ the transient anchor uses and let the loop
                // below drain it against the LIVE dry input.
                engageState();              // clears the splice scheduler
                unityFadeLen_ = snapXfade_;
                unityFade_    = snapXfade_;
                bypassed_     = true;
            }
            const SincTable& tbl = SincTable::get();
            // Keep the rings primed with dry audio while bypassed, so ENGAGING
            // mid-note splices against real signal instead of against silence.
            for (int n = 0; n < numSamples; ++n) {
                const float l = data[0][n];
                const float rr = nch > 1 ? data[1][n] : l;
                pushAudio(l, rr);
                pushAnalysis(0.5f * (l + rr));
                maybeProbe();   // keep the period estimate warm, so ENGAGING is right

                if (unityFade_ > 0) {
                    // t walks 0 -> 1 INTO the dry. Amplitude-complementary, not
                    // equal-power: the two sides are the same material at most
                    // one ceiling apart, i.e. rho ~= 1, which is the case the
                    // file's own crossfade law reduces to the linear one for.
                    const float t = 1.0f - (float) unityFade_ / (float) unityFadeLen_;
                    data[0][n] = (1.0f - t) * read(tbl, 0, r_) + t * l;
                    if (nch > 1) data[1][n] = (1.0f - t) * read(tbl, 1, r_) + t * rr;
                    r_ += 1.0;   // no shift left to apply: the head coasts at unity
                    --unityFade_;
                }
            }
            return;
        }

        if (bypassed_) {
            engageState();
            r_ = (double) w_ - anchorDelay_;
            // ENGAGING is the same splice in the other direction: the dry sample
            // just emitted and a read anchorDelay_ (1.3 ms) back are not the same
            // sample. Fade the shifted read IN over snapXfade_ against the live
            // dry rather than swapping in one sample. This is NOT the attack rule
            // being relaxed — the transient anchor still emits an attack already
            // shifted; this fade only covers a knob crossing unity.
            unityFadeLen_ = snapXfade_;
            unityFade_    = snapXfade_;
            bypassed_     = false;
        }

        const SincTable& tbl = SincTable::get();

        for (int n = 0; n < numSamples; ++n) {
            const float l = data[0][n];
            const float rr = nch > 1 ? data[1][n] : l;
            const float mid = 0.5f * (l + rr);

            pushAudio(l, rr);
            pushAnalysis(mid);
            updateOnset(mid);

            // Re-measure the material's period. This MUST run unconditionally: gating
            // it on "not currently hunting for a splice" starves it exactly when the
            // ceiling is too small (a small ceiling + a big shift means the delay
            // re-crosses the trigger immediately, so the scheduler is always pending)
            // — i.e. precisely the case the probe exists to rescue. It is throttled to
            // once per 32 ms and skips the sample findLag fires on, so the two
            // correlation bursts never land together.
            const bool probedThisSample = (!pending_ || searchTick_ > 1);
            if (probedThisSample) maybeProbe();

            const double delay = (double) w_ - r_;
            const int    ceil_ = effLag();

            // --- transient anchor: emit the attack now, already shifted ----------
            if (sinceOnset_ == 0 && delay > anchorDelay_ * 2.0) {
                rOld_ = r_;
                r_ = (double) w_ - anchorDelay_;
                fadeLen_ = snapXfade_;
                fade_ = snapXfade_;
                fadeRho_ = 0.0f;    // an anchor jump is unmatched by definition
                pending_ = false;   // the anchor discharged the delay; forget the splice
                bestScore_ = -1e30f;
            }
            // --- ordinary splice ------------------------------------------------
            else if (fade_ == 0) {
                if (!pending_ && delay >= ceil_ + xfade_) {
                    pending_ = true;
                    pendingFor_ = 0;
                    // kSearchInterval, NOT 0. On this very sample `pending_` was
                    // still false when the probe guard above ran, so maybeProbe()
                    // already fired; `searchTick_ = 0` then made `--searchTick_`
                    // below fire findLag in the SAME sample — the interleaving
                    // the comment above says cannot happen, and double the
                    // worst-case callback burst. The first search now lands one
                    // interval (1.3 ms at 48 kHz) into a window worth many.
                    searchTick_ = kSearchInterval;
                    bestScore_ = -1e30f;
                    bestLag_ = 0;
                }

                if (pending_) {
                    ++pendingFor_;

                    // We do NOT have to splice the instant the delay overruns — we have
                    // a window. Spend it hunting for the QUIETEST, best-matching moment:
                    // a splice under a quiet passage is inaudible, the same splice under
                    // a loud sustain is not.
                    //
                    // The search is throttled: correlating every sample would be an
                    // audio-thread blowout (hundreds of lags x hundreds of taps at 48 kHz).
                    // Every 64 samples is 1.3 ms — far finer than the moment we are
                    // choosing between — and keeps the cost around 1% of a core even at
                    // the octave's 25 splices/second.
                    if (--searchTick_ <= 0) {
                        searchTick_ = kSearchInterval;
                        // Counts the interleaving this throttle exists to prevent:
                        // both correlation bursts in ONE sample doubles the
                        // worst-case callback cost. Test-only, and only ever
                        // touched on the (already rare) search sample.
                        if (probedThisSample) ++doubleBurstCount_;
                        const int lag = findLag(delay, ceil_);  // updates bestScore_ when it improves
                        if (lag > 0) bestLag_ = lag;
                    }

                    // Splice on an excellent match, or when the window runs out —
                    // whichever comes first. Without the window bound, a noisy or
                    // inharmonic passage could keep "waiting for better" indefinitely
                    // and let the delay run away.
                    //
                    // THE WINDOW GRADUATES WITH THE MATCH (see header note 7): 30 ms
                    // is only a guess about how long a better moment takes to come
                    // around, and on material where the best existing match is poor
                    // (palm mutes; wide-interval chords — the oracle in
                    // tools/droptune_poly.cpp proves no good lag EXISTS there) that
                    // guess forced an audible splice every window. A poor match keeps
                    // hunting: on a riff the next PICK usually arrives first and the
                    // anchor discharges the whole delay under the attack — the perfect
                    // splice — and on a ring-out the extra time finds the quiet moment.
                    // hardDelay_ still bounds the delay absolutely.
#ifdef DCDSP_DT_EXP_SHORT_HUNT
                    const bool mustGo = delay >= hardDelay_ || pendingFor_ >= spliceWindow_;
#else
                    // The poor-match hunt is bounded by DELAY HEADROOM, not by time:
                    // a time bound would let the delay ride up toward hardDelay_ on
                    // hopeless material, and a pick the onset detector MISSES would
                    // then come out that much later. Headroom of ~22 ms past the
                    // ceiling means the hunt lasts 22ms/(1-r): a long, patient hunt
                    // at -1/-2 (where splices are rare and a bad one is exposed) and
                    // a short one at -12 (where they are frequent and masked) — and
                    // the worst-case sustain delay stays BELOW the old time-bound's.
                    const bool decent = bestScore_ > kDecentScore;
                    const bool mustGo = delay >= hardDelay_
                                     || (pendingFor_ >= spliceWindow_ && decent)
                                     || delay >= ceil_ + huntExtra_;
#endif
                    const bool goodEnough = bestScore_ > 0.90f;

#ifndef DCDSP_DT_EXP_SHORT_HUNT
                    // A forced splice on a HOPELESS match is a bad cut that will be
                    // followed by another bad cut one window later. Take ONE jump-cut
                    // instead: discharge the whole delay (like an anchor, but with the
                    // full crossfade), so the artifact happens once, not repeatedly.
                    if (mustGo && bestLag_ > 0 && bestScore_ < kHopelessScore) bestLag_ = 0;
#endif

                    if ((mustGo || goodEnough) && bestLag_ > 0) {
                        rOld_ = r_;
                        r_ += bestLag_;
                        fadeLen_ = xfade_;
                        fade_ = xfade_;
                        fadeRho_ = std::clamp(bestNcc_, 0.0f, 1.0f);
                        lastLag_ = bestLag_;
                        pending_ = false;
                        bestScore_ = -1e30f;
                    } else if (mustGo && bestLag_ <= 0) {
                        // Nothing correlated at all (silence, noise, a hopeless match).
#ifdef DCDSP_DT_EXP_SHORT_HUNT
                        const int lag = std::min(ceil_, (int) delay - xfade_);
#else
                        // Discharge the FULL delay in one cut (not capped at the
                        // ceiling): a capped bad splice guarantees the next one is
                        // due ceiling/(1-r) later, and a chain of bad cuts is worse
                        // than one. lastLag_ stays untouched — an unmatched jump is
                        // not a period estimate and must not feed the lag hysteresis.
                        const int lag = (int) delay - 2 * anchorDelay_;
#endif
                        if (lag > minLag_) {
                            rOld_ = r_;
                            r_ += lag;
                            fadeLen_ = xfade_;
                            fade_ = xfade_;
                            fadeRho_ = 0.0f;
#ifdef DCDSP_DT_EXP_SHORT_HUNT
                            lastLag_ = lag;
#endif
                        }
                        pending_ = false;
                        bestScore_ = -1e30f;
                    }
                }
            }

            float outL, outR;
            if (fade_ > 0) {
                const float t = 1.0f - (float) fade_ / (float) fadeLen_;
                // THE CROSSFADE LAW FOLLOWS THE MEASURED CORRELATION (header note 6).
                // Summing two segments with correlation rho at gains gA/gB has power
                // gA^2 + 2*rho*gA*gB + gB^2; holding that at unity gives
                //     gA = -rho*gB + sqrt(rho^2*gB^2 + 1 - gB^2).
                // rho = 1 reduces EXACTLY to the linear 1-t/t law (right for a
                // well-matched splice: correlated content must sum amplitude-
                // complementary or it bumps +3 dB mid-fade — that bump IS warble).
                // rho = 0 reduces EXACTLY to equal-power cos/sin (right for an
                // unmatched jump-cut: amplitude-complementary would DIP -3 dB
                // mid-fade, and those dips measured up to 13.8 dB of envelope dent
                // on real palm-muted material). Every splice knows its own NCC, so
                // every splice gets the law that keeps its power flat.
                const float gB = t;
#ifdef DCDSP_DT_EXP_LINEAR_FADE
                const float gA = 1.0f - t;
#else
                const float rho = fadeRho_;
                const float gA = -rho * gB
                               + std::sqrt(std::max(0.0f, rho * rho * gB * gB + 1.0f - gB * gB));
#endif
                outL = gA * read(tbl, 0, rOld_) + gB * read(tbl, 0, r_);
                outR = gA * read(tbl, 1, rOld_) + gB * read(tbl, 1, r_);
                rOld_ += rate_;
                --fade_;
            } else {
                outL = read(tbl, 0, r_);
                outR = read(tbl, 1, r_);
            }

            r_ += rate_;

            if (unityFade_ > 0) {
                // t walks 0 -> 1 INTO the shifted read; see the disengage half.
                const float t = 1.0f - (float) unityFade_ / (float) unityFadeLen_;
                outL = (1.0f - t) * l + t * outL;
                outR = (1.0f - t) * rr + t * outR;
                --unityFade_;
            }

            data[0][n] = outL;
            if (nch > 1) data[1][n] = outR;
        }
    }

    /// Mean added delay DURING SUSTAIN, at the ceiling the material is currently
    /// driving. NOT the attack latency, which is far lower and constant — the
    /// transient anchor sets that. See the header. (Informational: nothing does PDC
    /// with it — you cannot latency-compensate a live monitor path.)
    int latencySamples() const { return bypassed_ ? 0 : xfade_ + effLag() / 2; }

    // Test-only: how many samples ran BOTH correlation bursts (maybeProbe and
    // findLag). Must stay 0 — see the throttle comment in process().
    long long doubleBurstCountForTest() const { return doubleBurstCount_; }

private:
    /// The ceiling the splice scheduler actually uses: the knob is a FLOOR, and the
    /// measured composite period of the material raises it. Header note 5.
    int effLag() const {
        const int want = periodLag_ > 0 ? std::max(maxLag_, periodLag_) : maxLag_;
        const int hi = std::max(minLag_ + 1, absMaxLag_);
        return std::clamp(want, minLag_ + 1, hi);
    }

    void engageState() {
        fade_ = 0;
        pending_ = false;
        pendingFor_ = 0;
        searchTick_ = 0;
        bestScore_ = -1e30f;
        rOld_ = 0.0;
    }

    void pushAudio(float l, float r) {
        buf_[0][(size_t) (w_ & mask_)] = l;
        buf_[1][(size_t) (w_ & mask_)] = r;
        ++w_;
    }

    /// Low-pass then decimate the mid into the analysis ring. Correlating here is
    /// what makes the splice lock to the true period rather than to whichever
    /// harmonic happened to look self-similar.
    void pushAnalysis(float mid) {
        const float y = lpB0_ * mid + lpZ1_;
        lpZ1_ = lpB1_ * mid - lpA1_ * y + lpZ2_;
        lpZ2_ = lpB2_ * mid - lpA2_ * y;

        if (++decCount_ >= decim_) {
            decCount_ = 0;
            ana_[(size_t) (aw_ & aMask_)] = y;
            ++aw_;
            if (aFill_ < (1 << 28)) ++aFill_;   // saturating: how much history is real
        }
    }

    // Mask on the 64-bit value directly — truncating through (int) first would
    // re-introduce the 32-bit wrap the wide counters exist to avoid.
    float anaAt(long long i) const { return ana_[(size_t) (i & (long long) aMask_)]; }

    // -----------------------------------------------------------------------
    // PERIODICITY PROBE — what makes the lag ceiling adaptive (header note 5).
    //
    // Correlates the analysis ring against itself, BACKWARD FROM THE WRITE HEAD.
    // That anchor is the point: measured from the read head it would be bounded by
    // the current delay (we may only look at samples that exist), and the whole
    // problem is that the delay never GETS big enough to discover that the chord
    // needs a 40 ms lag. Measured from the write head, every sample it touches is
    // already written, so the full range is always available and it costs nothing.
    // -----------------------------------------------------------------------
    void maybeProbe() {
        if (probeTick_ > 0) { --probeTick_; return; }
        probeTick_ = probeInterval_;
        updatePeriod();
    }

    void updatePeriod() {
        if (aProbeMax_ <= aProbeMin_) return;
        if (aFill_ < aProbeMax_ + aCorr_ + 2) return;   // not enough history yet

        const long long base = (long long) aw_ - 1;

        // Coarse: every 2nd analysis lag (0.17 ms — far finer than the period we are
        // resolving), correlated on every 4th sample of the 33 ms window.
        auto nccAt = [&](int lag, int stride) -> float {
            float dot = 0.f, ea = 0.f, eb = 0.f;
            for (int j = 0; j < aCorr_; j += stride) {
                const float a = anaAt(base - j);
                const float b = anaAt(base - lag - j);
                dot += a * b; ea += a * a; eb += b * b;
            }
            return dot / (std::sqrt(ea * eb) + 1e-9f);
        };

        float bestN = -1e30f;
        for (int lag = aProbeMin_; lag <= aProbeMax_; lag += 2) {
            const float s = nccAt(lag, 4);
            probeScore_[(size_t) lag] = s;
            if (s > bestN) bestN = s;
        }

        // Nothing periodic here (silence, noise, a palm-muted thud). Keep whatever we
        // last knew rather than throwing the ceiling around on a bad frame.
        if (bestN < kMinPeriodNcc) return;

        // The SMALLEST lag that is essentially as good as the best one. A periodic
        // signal correlates at EVERY multiple of its period, so a plain argmax would
        // sometimes return 5x the period and balloon the sustain delay for nothing.
        int chosen = aProbeMin_;
        for (int lag = aProbeMin_; lag <= aProbeMax_; lag += 2) {
            if (probeScore_[(size_t) lag] >= bestN - kPeriodSlack) { chosen = lag; break; }
        }

        // Refine at full analysis resolution around the winner.
        float refN = -1e30f;
        int refined = chosen;
        for (int lag = std::max(aProbeMin_, chosen - 2); lag <= std::min(aProbeMax_, chosen + 2); ++lag) {
            const float s = nccAt(lag, 1);
            if (s > refN) { refN = s; refined = lag; }
        }
        periodLag_ = refined * decim_;
    }

    float read(const SincTable& tbl, int ch, double pos) const {
        const long long i = (long long) std::floor(pos);
        const double frac = pos - (double) i;

        int phase = (int) (frac * SincTable::kPhases);
        if (phase < 0) phase = 0;
        if (phase >= SincTable::kPhases) phase = SincTable::kPhases - 1;

        const float* h = tbl.at(phase);
        const auto& b = buf_[(size_t) ch];

        float acc = 0.0f;
        const long long base = i - (SincTable::kTaps / 2 - 1);
        for (int k = 0; k < SincTable::kTaps; ++k)
            acc += h[k] * b[(size_t) ((base + k) & (long long) mask_)];

        return acc;
    }

    /// Best forward jump: the lag whose PAST waveform best continues the past
    /// waveform at the read head. Backward-looking, so it is free.
    /// Also updates bestScore_/bestLag_ (the "best moment so far" tracker).
    int findLag(double delay, int ceiling) {
        const int lagCeil = std::min(ceiling, (int) delay - xfade_);
        if (lagCeil <= minLag_) return 0;

        // Everything below is in the decimated analysis domain.
        const int aLagMin = minLag_ / decim_;
        const int aLagMax = lagCeil / decim_;
        if (aLagMax <= aLagMin) return 0;

        const long long aBase = (long long) (r_ / decim_);

        // Energy at the splice point: prefer to cut where it is QUIET.
        float energy = 0.0f;
        for (int j = 0; j < aCorr_; j += 4) {
            const float v = anaAt(aBase - j);
            energy += v * v;
        }
        energy = std::sqrt(energy / (float) std::max(1, aCorr_ / 4));

        auto score = [&](int lag, int stride) -> float {
            float dot = 0.f, ea = 0.f, eb = 0.f;
            for (int j = 0; j < aCorr_; j += stride) {
                const float a = anaAt(aBase - j);
                const float b = anaAt(aBase + lag - j);
                dot += a * b; ea += a * a; eb += b * b;
            }
            const float ncc = dot / (std::sqrt(ea * eb) + 1e-9f);

            // Prefer a BIG lag: it buys more time before the next splice.
            const float longBonus = 0.02f * (float) lag / (float) std::max(1, aLagMax);

            // Lag hysteresis. A periodic signal has many equally good lags — one per
            // period — and without this the choice jitters between period multiples
            // and sprays sidebands at the splice rate. The harmonizer core learned
            // this the hard way; keep the lesson.
            const int aLast = lastLag_ / decim_;
            const float stick = (aLast > 0 && std::abs(lag - aLast) <= 1) ? 0.05f : 0.0f;

            return ncc + longBonus + stick;
        };

        // Coarse sweep: every 2nd lag (0.17 ms at the 12 kHz analysis rate), on every
        // 4th sample of the window. The ceiling is now adaptive and can reach 65 ms,
        // so the old every-lag/every-2nd-sample sweep would have grown the worst-case
        // burst ~4x; this keeps it BELOW where it was, with the ±2 refine below
        // recovering the resolution.
        int best = 0;
        float bestS = -1e30f;
        for (int lag = aLagMin; lag <= aLagMax; lag += 2) {
            const float s = score(lag, 4);
            if (s > bestS) { bestS = s; best = lag; }
        }
        if (best <= 0) return 0;

        // Refine at full analysis rate around the winner.
        float refS = -1e30f;
        int refined = best;
        for (int lag = std::max(aLagMin, best - 2); lag <= std::min(aLagMax, best + 2); ++lag) {
            const float s = score(lag, 1);
            if (s > refS) { refS = s; refined = lag; }
        }

        // Quiet moments are better moments: a splice under a quiet passage is
        // inaudible, the same splice under a loud sustain is not.
        const float moment = refS - 0.15f * std::min(1.0f, energy * 4.0f);
        if (moment > bestScore_) {
            bestScore_ = moment;
            // The RAW correlation of the chosen lag (no bonuses, no energy penalty):
            // this is the rho the crossfade law needs.
            {
                float dot = 0.f, ea = 0.f, eb = 0.f;
                for (int j = 0; j < aCorr_; ++j) {
                    const float a = anaAt(aBase - j);
                    const float b = anaAt(aBase + refined - j);
                    dot += a * b; ea += a * a; eb += b * b;
                }
                bestNcc_ = dot / (std::sqrt(ea * eb) + 1e-9f);
            }
            return refined * decim_;
        }

        return 0;
    }

    /// A RISE detector. Read the header section on it before changing a constant —
    /// the obvious-looking crest-factor version of this cost the pedal a year.
    void updateOnset(float x) {
        // The pick lives above ~1.2 kHz. Re-picking a ringing string barely moves the
        // broadband envelope but jumps this one 2-4x, and it peaks 1-5 ms after the
        // pick rather than 6-20 ms.
        hpZ_ = hpCoef_ * hpZ_ + (1.0f - hpCoef_) * x;
        const float hp = std::abs(x - hpZ_);

        // Peak follower, fast attack / SLOW release. The release is not a detail: at
        // 0.5 ms it collapsed back onto the instantaneous value between waveform
        // peaks, so the detector re-armed every period of every note below 500 Hz.
        // At 50 ms it holds the pick.
        const float c = (hp > fastEnv_) ? fastAtk_ : fastRel_;
        fastEnv_ = c * fastEnv_ + (1.0f - c) * hp;

        // The SAME envelope ~35 ms ago. This is the entire fix: a pick is a RISE, and
        // a rise only exists relative to the past. Comparing the fast envelope against
        // a statistic of the same instant measures crest factor, not onset.
        const float past = onsetEnv_[(size_t) (((ow_ - onsetLookback_) + oSize_) & oMask_)];
        onsetEnv_[(size_t) ow_] = fastEnv_;
        ow_ = (ow_ + 1) & oMask_;

        if (sinceOnset_ < (1 << 20)) ++sinceOnset_;

        const float ref = std::max(past, kOnsetFloor);   // ~ -55 dBFS: do not chase noise

        if (armed_ && fastEnv_ > kRiseRatio * ref && fastEnv_ > kOnsetFloor
            && sinceOnset_ > retrigger_) {
            sinceOnset_ = 0;
            armed_ = false;
        } else if (!armed_ && fastEnv_ < kRearmRatio * ref) {
            armed_ = true;
        }
    }

    // audio rings (one read head for BOTH: independent lags per channel would make
    // the stereo image wobble and partially collapse at every splice)
    //
    // The write counters are 64-BIT on purpose (2026-07-16 audit #6): as 32-bit
    // ints, w_ overflowed after ~12.4 h of continuous monitoring @48k (aw_ after
    // ~49.7 h at the 12 kHz analysis rate), at which point `delay = w_ - r_`
    // went hugely negative and the splice scheduler broke. 64-bit pushes that
    // past any conceivable session; r_ stays a double, whose 2^53 integer range
    // is equally unreachable.
    std::vector<float> buf_[2];
    int ringSize_ = 0, mask_ = 0;
    long long w_ = 0;

    // decimated, low-passed analysis ring (mid)
    std::vector<float> ana_;
    long long aw_ = 0;
    int aSize_ = 0, aMask_ = 0, decim_ = 4, decCount_ = 0, aCorr_ = 0, aFill_ = 0;
    float aRate_ = 12000.0f;
    float lpB0_ = 0, lpB1_ = 0, lpB2_ = 0, lpA1_ = 0, lpA2_ = 0, lpZ1_ = 0, lpZ2_ = 0;

    double r_ = 0.0, rOld_ = 0.0;
    float  rate_ = 1.0f;
    bool   bypassed_ = true;

    int fade_ = 0, fadeLen_ = 0;
    // The ENGAGE/DISENGAGE crossfade (rate crossing unity), which is a splice
    // between the shifted read and the LIVE dry input rather than between two
    // read heads — so it is counted separately from fade_/rOld_, which the
    // splice scheduler owns.
    int unityFade_ = 0, unityFadeLen_ = 0;
    int xfade_ = 0, snapXfade_ = 0, anchorDelay_ = 0;
    int minLag_ = 0, maxLag_ = 0, absMaxLag_ = 0, hardDelay_ = 0;
    float maxLagMs_ = 20.0f;   // last requested lag; re-derived on every prepare
    int lastLag_ = 0;
    float fadeRho_ = 1.0f;      // correlation of the two heads across the CURRENT fade

    // "best moment so far" splice scheduler
    static constexpr int kSearchInterval = 64;   // ~1.3 ms between searches
    static constexpr float kDecentScore   = 0.60f; // below this, keep hunting past the short window
    static constexpr float kHopelessScore = 0.35f; // below this, one jump-cut beats a chain of bad splices
    bool  pending_ = false;
    int   pendingFor_ = 0;      // samples spent waiting for a good moment
    int   spliceWindow_ = 0;    // bound on the wait when the match is DECENT
    int   huntExtra_ = 0;       // poor-match hunt: delay headroom past the ceiling
    int   searchTick_ = 0;
    long long doubleBurstCount_ = 0;   // test-only, see doubleBurstCountForTest()
    int   bestLag_ = 0;
    float bestScore_ = -1e30f;
    float bestNcc_ = 0.0f;      // raw NCC of bestLag_ (no bonuses) -> the fade's rho

    // periodicity probe -> the adaptive lag ceiling (header note 5)
    static constexpr float kAbsMaxLagMs  = 65.0f;  // reaches every measured chord period
    static constexpr float kMinPeriodNcc = 0.55f;  // below this the material is not periodic
    static constexpr float kPeriodSlack  = 0.03f;  // "as good as the best" band
    std::vector<float> probeScore_;
    int   aProbeMin_ = 0, aProbeMax_ = 0;
    int   probeInterval_ = 0, probeTick_ = 0;
    int   periodLag_ = 0;       // full-rate samples; 0 = not measured yet

    // onset detector — a RISE detector (see updateOnset and the header).
    // The thresholds were swept against synthesized picks/ring-out/held material (5
    // seeds, 6 chord shapes each). The binding constraint is lookback < refractory:
    // once the lookback reaches the refractory, `past` is still the PRE-pick level
    // when the refractory expires, so the detector re-fires immediately and free-runs
    // (exactly the pathology of the old crest detector). 35 ms under a 40 ms lock
    // caught ~58% of re-picks over a still-ringing chord — the WORST case, real single
    // notes and palm mutes are far higher — at ~0 false anchors on sustained material.
    static constexpr float kFastAttackMs     = 0.5f;   // sees the pick in 1-2 ms
    static constexpr float kFastReleaseMs    = 50.0f;  // holds it, instead of re-arming every period
    static constexpr float kOnsetLookbackMs  = 35.0f;  // "the level 35 ms ago"; MUST stay < 40 ms refractory
    static constexpr float kRiseRatio        = 1.5f;   // a pick out-runs its own recent past by >1.5x
    static constexpr float kRearmRatio       = 1.2f;
    static constexpr float kOnsetFloor       = 1.8e-3f;// ~ -55 dBFS
    float hpCoef_ = 0, fastAtk_ = 0, fastRel_ = 0;
    float hpZ_ = 0, fastEnv_ = 0;
    std::vector<float> onsetEnv_;   // history of fastEnv_, for the delayed compare
    int   oSize_ = 0, oMask_ = 0, ow_ = 0, onsetLookback_ = 0;
    bool  armed_ = true;
    int   sinceOnset_ = 1 << 20, retrigger_ = 0;

    float sr_ = 48000.0f;
    int   channels_ = 2;
};

}  // namespace dcdsp
