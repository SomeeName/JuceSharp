// dcdsp_whammy.h — expression-swept pitch shifter (WSOLA), the wet-only sibling
// of dcdsp_harmonizer.h.  Pure dcdsp core, header-only, C++17, RT-safe.
//
// A treadle sweeps the shift between `heelSemis` and `toeSemis` over +/- 24
// semitones, which is the whole DigiTech-Whammy vocabulary:
//
//     heel   0 / toe +12  ->  1 octave up      heel   0 / toe -24  ->  dive bomb
//     heel   0 / toe +24  ->  2 octaves up     heel -12 / toe   0  ->  drop/return
//
// `mix` defaults to 1 (WET ONLY — the hardware's Whammy mode). Pull it back for
// the pedal's Harmony mode, where the shifted voice sits under the dry note.
//
// ===========================================================================
// Relationship to dcdsp_harmonizer.h — WHY THIS IS A SEPARATE CORE
// ===========================================================================
// The WSOLA machinery below (Hann grains at 50 % overlap, cross-correlation
// splice search with a centre bias, transient hold on picks, Lagrange-3
// fractional resample) is LIFTED FROM dcdsp_harmonizer.h and behaves
// identically at a static shift. It is copied rather than reused because a
// swept shifter needs three changes that reach into the middle of that
// pipeline, and bolting them onto HarmonizerCore would change the harmonizer's
// sound:
//
//   1. RANGE. HarmonizerCore hard-clamps to +/- 12 semitones (ratio 0.5 .. 2.0)
//      because that is the harmony vocabulary. A whammy needs 0.25 .. 4.0.
//      Widening the harmonizer's clamp would silently change what its existing
//      presets do at their rails.
//
//   2. THE ANALYSIS POINTER MUST ACCUMULATE, NOT MULTIPLY. HarmonizerCore
//      computes each grain's analysis position as `ha * grainM` — a closed form
//      that is only correct while `ha` is CONSTANT. The instant the ratio moves
//      (which is the entire point of a treadle) that expression re-scales the
//      whole history: the analysis pointer teleports by (ha_new - ha_old) *
//      grainM, and the splice search cannot recover. Here the ideal position is
//      ACCUMULATED (`idealAcc_ += ha_` per grain), which is identical while the
//      ratio is static and continuous while it moves. This is the one change
//      that makes a swept WSOLA work at all.
//
//   3. THE RATIO IS SMOOTHED PER SAMPLE. Params arrive at UI rate; a treadle
//      stepping in 30 Hz jumps is the DigiTech's famous zipper. A one-pole glide
//      (`glideMs`) turns it into a portamento. Set glideMs = 0 for the hardware's
//      steppy feel.
//
// The analysis pointer still advances at exactly 1 input sample per input
// sample for ANY ratio — d(ideal)/dn = ha * (grains per sample) = (Hs/r)*(r/Hs)
// = 1 — so the pipeline delay is ratio-independent and the sweep never drifts.
//
// ===========================================================================
// FORMANT PRESERVATION (`formant` = 1) — real, and NOT a phase vocoder
// ===========================================================================
// Shifting a guitar up an octave moves its body/pickup resonances up with it —
// the "chipmunk" colour. Preserving them means moving the harmonics without
// moving the spectral ENVELOPE. The textbook fix is a phase vocoder, which is
// BANNED on the live input here (resolving a low E's harmonics needs N >
// 4*fs/f0 = ~48 ms of window; see JuceSharp/AGENTS.md). So we do it the way
// speech coders do, entirely in the time domain:
//
//     x --> A(z) --> residual --> [WSOLA pitch shift] --> 1/A(z) --> y
//           whiten                                        re-dress
//
//   * A(z) is a 16th-order LPC analysis filter, recomputed every 12 ms from a
//     24 ms Hann-windowed autocorrelation (lag-windowed + white-noise
//     corrected; Levinson-Durbin, so A is minimum-phase BY CONSTRUCTION and
//     1/A is stable). Bandwidth expansion (a[i] *= 0.985^i) pulls the poles in
//     so a coefficient swap at a frame boundary cannot ring.
//   * A(z) flattens the spectral envelope, leaving an excitation residual that
//     is essentially the pick + string pulse train. WSOLA shifts THAT.
//   * 1/A(z) then re-imposes the ORIGINAL envelope on the shifted excitation.
//     The formants stay put; only the harmonic spacing moves.
//
// NO EXTRA LATENCY. The whitening filter is causal (it filters x[n] with
// coefficients from the preceding 24 ms — an analysis window, not a look-ahead).
// The synthesis filter needs the envelope that BELONGS to the audio coming out
// of the pipeline, i.e. the envelope of the input `latency_` samples ago — so
// the coefficient sets are kept in a small ring and the synthesis reads the set
// from `synthLag_` frames back. Time-aligned, and nothing is buffered for it.
//
// Because analysis and synthesis use the SAME (expanded) coefficient set, the
// two filters are exact inverses: at ratio 1 the formant path collapses to a
// pass-through, which is what the harness measures it doing.
//
// ===========================================================================
// COST. The WSOLA splice search runs once per GRAIN, and grains are produced at
// r/Hs per sample — so CPU scales with the shift ratio. Two octaves UP (r = 4)
// costs ~4x what a unison/harmony setting does; two octaves DOWN (r = 0.25)
// costs a quarter. Budget for the toe-down case. `formant` adds a flat ~70
// MAC/sample/channel on top (autocorrelation + two 16-tap filters).
//
// Allocation happens ONLY in prepare(). process() never allocates, locks, or
// throws. Output is always finite and bounded.
#ifndef DCDSP_WHAMMY_H
#define DCDSP_WHAMMY_H

#include "dcdsp_primitives.h"   // SmoothedValue (the mix ramp)

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dcdsp {

// ---------------------------------------------------------------------------
// Public parameter block — DO NOT rename/reorder (factory adapter + catalogue
// mirror these fields by name).
// ---------------------------------------------------------------------------
struct WhammyParams {
    float position   = 0.0f;  // treadle [0..1]: 0 = heel, 1 = toe
    float heelSemis  = 0.0f;  // shift at heel-down [-24 .. +24]
    float toeSemis   = 12.0f; // shift at toe-down  [-24 .. +24]
    float mix        = 1.0f;  // [0..1]; 1 = wet only (the pedal's Whammy mode)
    float glideMs    = 20.0f; // [0..200] treadle smoothing; 0 = the hardware's steps
    float formant    = 0.0f;  // 0/1; 1 = preserve formants (LPC whiten / re-dress)
    float lowLatency = 0.0f;  // 0/1; 1 = shorter window + search (snappier, less low-end)
};

class WhammyCore {
public:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    void prepare(double sampleRate, int maxBlock, int nch) {
        (void)maxBlock;
        sampleRate_  = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numChannels_ = (nch > 0) ? nch : 1;
        if (numChannels_ > kMaxChannels) numChannels_ = kMaxChannels;

        // Size the rings — and the Hann table — for the WORST CASE geometry,
        // which is the NON-low-latency mode (a 32 ms window against 12 ms).
        //
        // `lowLatency` is a runtime param, and setParams re-runs buildGeometry()
        // when it flips. Its comment claimed "we only ever shrink within the
        // buffers allocated in prepare()", which is only true if prepare() ran
        // in the LARGE mode: prepare with lowLatency already on, then turn it
        // OFF, and inSize_/stSize_ grow to 4x the vectors that were actually
        // allocated. Every ring access masks with those members
        // (`in[w & (inSz-1)]`, `st[p & (stSz-1)]`), so the writes ran off the
        // end — inside the retained capacity at one rate, genuinely past it
        // after a rate change had reallocated smaller. buildHann() would have
        // reallocated on the audio thread in the same step.
        const bool wantLowLatency = lowLatency_;
        lowLatency_ = false;
        buildGeometry();              // largest window => largest rings + Hann table
        allocIn_ = inSize_;
        allocSt_ = stSize_;
        lowLatency_ = wantLowLatency;
        buildGeometry();              // the geometry actually in force
        // …but the RINGS stay at the worst-case size for the whole session, so
        // a mid-session mode flip can never outgrow them. A larger power-of-two
        // ring is a pure superset: the masking is identical, there is just more
        // unused history behind the read heads.
        inSize_ = allocIn_;
        stSize_ = allocSt_;
        // …and the same argument for the Hann TABLE, which the two passes above
        // only cover in one direction. Both modes' tables are built here, so
        // the flip in setParams — which runs on the audio callback — swaps a
        // `const float*` instead of running up to 8,192 std::cos inside the one
        // block that saw the toggle.
        buildHannTables();

        // LPC geometry (formant path). Sized here so process() never allocates.
        lpcWin_ = evenRound(sampleRate_ * kLpcWinMs / 1000.0);
        if (lpcWin_ < 128) lpcWin_ = 128;
        if (lpcWin_ > kMaxLpcWin) lpcWin_ = kMaxLpcWin;
        lpcHop_ = static_cast<int>(sampleRate_ * kLpcHopMs / 1000.0);
        if (lpcHop_ < 32) lpcHop_ = 32;
        xSize_ = nextPow2(lpcWin_ + 8);
        buildHann(lpcWindow_, lpcWin_);
        buildLagWindow();
        frame_.assign(static_cast<std::size_t>(lpcWin_), 0.f);

        // Synthesis reads the coefficient set that belongs to the audio leaving
        // the pipeline, i.e. the input from latency_ samples ago.
        synthLag_ = static_cast<int>(std::lround(
            static_cast<double>(latency_) / static_cast<double>(lpcHop_)));
        if (synthLag_ < 1) synthLag_ = 1;
        if (synthLag_ > kLpcSets - 2) synthLag_ = kLpcSets - 2;

        for (int c = 0; c < kMaxChannels; ++c) ch_[c].alloc(inSize_, stSize_, xSize_);
        // The one rate-derived value setParams owns. fastRel_, slowA_, latency_,
        // synthLag_, lpcWin_/lpcHop_ and the lag window are all re-derived
        // above; without this the treadle glide keeps the OLD rate's
        // coefficient, i.e. half the requested milliseconds at twice the rate,
        // until the next knob write.
        applyGlide();
        // `mix` RAMPS (kMixRampSeconds). The core already glides the treadle
        // RATIO to kill that zipper and left the mix stepped; the wet here is a
        // pitch-shifted, phase-uncorrelated copy, so a mix step splices its full
        // amplitude in or out at a block edge rather than tilting a level.
        mixS_.reset(sampleRate_, kMixRampSeconds);
        mixS_.setTarget(mix_);
        mixS_.snapToTarget();   // a freshly prepared whammy starts AT its mix
        reset();
        applyDerived();
    }

    /// TEST SURFACE, same contract as HarmonizerCore::ringsCoverGeometry():
    /// true while the ring masks the audio path uses are inside what prepare()
    /// actually allocated. This core already sizes for the worst case; the
    /// accessor exists so the gate pins it rather than trusting the comment.
    bool ringsCoverGeometry() const noexcept {
        for (int c = 0; c < kMaxChannels; ++c)
            if (static_cast<int>(ch_[c].in.size()) < inSize_
             || static_cast<int>(ch_[c].st.size()) < stSize_)
                return false;
        return true;
    }


    /// TEST SURFACE. How many analysis-window Hann tables have been BUILT since
    /// construction. prepare() builds two (one per latency mode); a runtime
    /// `lowLatency` flip must never raise this, because it runs on the audio
    /// callback and a build is up to 8,192 std::cos in one block.
    long long hannBuilds() const noexcept { return hannBuilds_; }

    /// TEST SURFACE. How many per-CHANNEL ring clears reprimeSoft() has done
    /// since construction. A flip must cost numChannels_, not kMaxChannels: a
    /// stereo rig paid 4x for the eight-channel worst case.
    long long reprimeChannelClears() const noexcept { return reprimeChannelClears_; }

    /// TEST SURFACE. True while BOTH latency modes' Hann tables are sized for
    /// this rate, i.e. a mode flip cannot reach buildHann() on the callback.
    bool hannTablesCoverBothModes() const noexcept {
        return static_cast<int>(hannLarge_.size()) == windowSamplesFor(false)
            && static_cast<int>(hannSmall_.size()) == windowSamplesFor(true);
    }

    void reset() {
        stRead_   = 0.0;
        stEnd_    = 0;
        grainM_   = 0;
        idealAcc_ = 0.0;
        inCount_  = 0;
        primed_   = false;
        lpcCount_ = 0;
        setCount_ = 0;
        ratio_    = ratioTarget_;
        for (int c = 0; c < kMaxChannels; ++c) ch_[c].reset();
    }

    void setParams(const WhammyParams& p) {
        // ---- treadle -> semitones -> ratio target.
        float pos  = finite(p.position, 0.f);
        pos  = std::max(0.f, std::min(1.f, pos));
        float heel = std::max(-24.f, std::min(24.f, finite(p.heelSemis, 0.f)));
        float toe  = std::max(-24.f, std::min(24.f, finite(p.toeSemis, 0.f)));
        const float semis = heel + pos * (toe - heel);
        ratioTarget_ = std::pow(2.f, semis / 12.f);           // 0.25 .. 4.0
        ratioTarget_ = std::max(kMinRatio, std::min(kMaxRatio, ratioTarget_));

        mix_ = std::max(0.f, std::min(1.f, finite(p.mix, 1.f)));
        mixS_.setTarget(mix_);

        // ---- treadle glide (one-pole toward the ratio target, per sample).
        glideMs_ = std::max(0.f, std::min(200.f, finite(p.glideMs, 0.f)));
        applyGlide();

        const bool wantFormant = (p.formant >= 0.5f);
        const bool lowLat      = (p.lowLatency >= 0.5f);

        if (lowLat != lowLatency_) {
            lowLatency_ = lowLat;
            // Geometry (window/hop/search) depends on the latency mode. The
            // rings were allocated for the WORST case in prepare() and stay
            // there, so this is allocation-free AND in-bounds in both
            // directions — see the comment in prepare().
            buildGeometry();
            inSize_ = allocIn_;
            stSize_ = allocSt_;
            synthLag_ = static_cast<int>(std::lround(
                static_cast<double>(latency_) / static_cast<double>(lpcHop_)));
            if (synthLag_ < 1) synthLag_ = 1;
            if (synthLag_ > kLpcSets - 2) synthLag_ = kLpcSets - 2;
            reprimeSoft();
        }
        if (wantFormant != formant_) {
            // The WSOLA input ring holds the RESIDUAL when formant is on and the
            // raw signal when it is off — the two are not interchangeable, so a
            // toggle re-primes rather than splicing one onto the other.
            formant_ = wantFormant;
            reprimeSoft();
        }
        applyDerived();
    }

    // -----------------------------------------------------------------------
    // In-place block processing. wet = pitch-shifted; blended with dry by mix.
    // -----------------------------------------------------------------------
    void process(float* const* channels, int nch, int nframes) {
        if (channels == nullptr || nframes <= 0) return;
        const int n = std::min(nch, numChannels_);
        if (n <= 0) return;

        for (int i = 0; i < nframes; ++i) {
            // One walk per FRAME, above the channel loop below.
            const float wetG = mixS_.next();
            const float dryG = 1.f - wetG;
            // ---- 1) glide the ratio, then push this frame's input.
            ratio_ = (glideCoef_ <= 0.f)
                ? ratioTarget_
                : (glideCoef_ * ratio_ + (1.f - glideCoef_) * ratioTarget_);
            applyDerived();

            const bool lpcTick = formant_ && (++lpcCount_ >= lpcHop_);
            if (lpcTick) lpcCount_ = 0;

            // (a) scrub + record the raw input for every channel FIRST: the LPC
            //     analysis window ends on the sample just pushed.
            for (int c = 0; c < n; ++c) {
                float* buf = channels[c];
                float x = (buf != nullptr) ? buf[i] : 0.f;
                // NaN/Inf scrub at input, written back so the DRY blend below
                // reads the scrubbed sample too.
                if (!std::isfinite(x)) x = 0.f;
                if (buf != nullptr) buf[i] = x;

                Ch& cc = ch_[c];
                cc.pushRaw(x, xSize_);                 // raw history (LPC analysis)
                cc.updateOnset(x, fastRel_, slowA_);   // onset detect on the RAW signal
            }

            // (b) one new coefficient set per channel per hop. setCount_ is a
            //     SHARED frame clock, so it advances once — after every channel
            //     has written its set — not once per channel.
            if (lpcTick) {
                for (int c = 0; c < n; ++c) computeLpcSet(ch_[c]);
                ++setCount_;
            }

            // (c) feed WSOLA: the residual when preserving formants, else the raw.
            for (int c = 0; c < n; ++c) {
                float* buf = channels[c];
                const float x = (buf != nullptr) ? buf[i] : 0.f;
                Ch& cc = ch_[c];
                cc.pushIn(formant_ ? cc.whiten(x, setCount_) : x, inSize_);
            }
            ++inCount_;

            // ---- 2) produce WSOLA grains until the finalised stretched stream
            //         keeps a full grain of look-ahead over the resample read.
            const long long need = static_cast<long long>(std::floor(stRead_)) + hs_ + 3;
            long long frontier = static_cast<long long>(grainM_) * hs_;
            int guard = 0;
            while (frontier < need && guard++ < kMaxGrainsPerSample) {
                if (!produceGrain(n)) break;           // not enough input yet
                frontier = static_cast<long long>(grainM_) * hs_;
            }

            // ---- 3) once enough grains are buffered, start reading.
            if (!primed_ && grainM_ >= 2) {
                primed_ = true;
                stRead_ = static_cast<double>(frontier) - static_cast<double>(hs_ + 3);
                if (stRead_ < 1.0) stRead_ = 1.0;
            }

            for (int c = 0; c < n; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                Ch& cc = ch_[c];
                const float x = buf[i];

                float w;
                if (primed_) {
                    w = cc.readSt(stRead_, stSize_);
                    if (!std::isfinite(w)) w = 0.f;
                    w = std::max(-8.f, std::min(8.f, w));
                    // Re-dress the shifted excitation with the envelope that
                    // belongs to it (the input from latency_ samples ago).
                    if (formant_) w = cc.synth(w, setCount_, synthLag_);
                } else {
                    w = x;                              // pass-through while priming
                }
                if (!std::isfinite(w)) w = 0.f;
                w = std::max(-8.f, std::min(8.f, w));

                float out = dryG * x + wetG * w;
                if (!std::isfinite(out)) out = 0.f;
                buf[i] = std::max(-16.f, std::min(16.f, out));
            }

            if (primed_) stRead_ += static_cast<double>(ratio_);
        }
    }

    // Algorithmic latency of the wet path (window + splice search). The dry
    // blend is summed at the same instant, exactly as in dcdsp_harmonizer.h.
    int latencySamples() const { return latency_; }

private:
    // =======================================================================
    // Tunables. Declared FIRST: the nested Ch below sizes members with them,
    // and a member declaration does not get complete-class context.
    // =======================================================================
    static constexpr int    kMaxChannels        = 8;
    static constexpr int    kMaxWindow          = 8192;  // 32 ms up to 192 kHz
    static constexpr int    kTransientGrains    = 2;
    static constexpr int    kMaxGrainsPerSample = 12;    // r <= 4 needs << 1 steady state
    static constexpr float  kMinRatio           = 0.25f; // -24 semitones
    static constexpr float  kMaxRatio           = 4.0f;  // +24 semitones

    static constexpr int    kLpcOrder        = 16;
    static constexpr int    kLpcSets         = 16;       // pow2 ring of coefficient sets
    static constexpr int    kMaxLpcWin       = 8192;
    static constexpr double kLpcWinMs        = 24.0;
    static constexpr double kLpcHopMs        = 12.0;
    static constexpr double kGamma           = 0.985;    // bandwidth expansion
    static constexpr double kWhiteNoiseCorr  = 1e-4;
    static constexpr double kLagWinHz        = 40.0;
    static constexpr double kTwoPiLocal      = 6.28318530717958647692;

    struct Ch;   // defined below; declared here so computeLpcSet can take one

    // =======================================================================
    // Geometry (identical law to dcdsp_harmonizer.h)
    // =======================================================================
    /// glideCoef_ from the last requested milliseconds. Called from both
    /// setParams and prepare, so the glide survives a sample-rate change.
    void applyGlide() {
        glideCoef_ = (glideMs_ <= 0.01f)
            ? 0.f   // no smoothing: the hardware's stepped feel
            : static_cast<float>(std::exp(-1.0 / (glideMs_ * 0.001 * sampleRate_)));
    }

    /// Point `hann_` at the table for the geometry now in force. The two tables
    /// are BUILT in prepare() (buildHannTables), never here: setParams runs on
    /// the audio callback (the adapter drains its ParamMailbox from the top of
    /// process()), and a `lowLatency` flip used to land up to 8,192 std::cos
    /// calls in the one block that saw the toggle — the burst `glitch` was
    /// rewritten to remove, invisible to the RT gate because a
    /// capacity-retaining `assign` is not an allocation. The size guard only
    /// ever fires on the FIRST buildGeometry() of a prepare(), before the
    /// tables have been sized for this rate.
    void selectHann() {
        std::vector<float>& tbl = lowLatency_ ? hannSmall_ : hannLarge_;
        if (static_cast<int>(tbl.size()) != n_) { buildHann(tbl, n_); ++hannBuilds_; }
        hann_ = tbl.data();
    }

    /// Build BOTH modes' Hann tables for this sample rate. prepare() only.
    void buildHannTables() {
        buildHann(hannLarge_, windowSamplesFor(false));
        buildHann(hannSmall_, windowSamplesFor(true));
        hannBuilds_ += 2;
        selectHann();
    }

    /// The analysis window length for a latency mode. One law, two callers —
    /// buildGeometry() for the geometry in force and buildHannTables() for the
    /// table of the mode that is NOT in force.
    int windowSamplesFor(bool lowLat) const {
        const double winMs = lowLat ? 12.0 : 32.0;
        int N = evenRound(sampleRate_ * winMs / 1000.0);
        if (N < 128) N = 128;
        if (N > kMaxWindow) N = kMaxWindow;
        return N;
    }

    void buildGeometry() {
        // Normal mode must span a full low-string period (drop C# = 69.3 Hz =
        // 14.4 ms) INSIDE the overlap region, or the splice search cannot find a
        // period-aligned lag and the drop register warbles. 32 ms => 16 ms overlap.
        const int N = windowSamplesFor(lowLatency_);
        n_  = N;
        hs_ = N / 2;                 // 50 % overlap (constant-overlap-add Hann)
        lc_ = hs_;                   // correlation region = the overlap length

        delta_ = lowLatency_ ? (N / 8)
                             : std::max(N / 4, (int)std::ceil(0.008 * sampleRate_));
        if (delta_ < 8) delta_ = 8;

        selectHann();

        fastRel_ = static_cast<float>(std::exp(-1.0 / (0.005 * sampleRate_)));
        slowA_   = static_cast<float>(1.0 - std::exp(-1.0 / (0.080 * sampleRate_)));

        latency_ = n_ + delta_;

        inSize_ = nextPow2(n_ + delta_ + hs_ + 8);
        stSize_ = nextPow2(4 * n_ + 8);
    }

    void applyDerived() {
        // Analysis hop Ha = Hs / r (time-scale factor r). Called per sample while
        // the treadle glides, so it early-outs once the ratio has settled (which
        // the one-pole reaches EXACTLY in float) — a parked treadle costs nothing.
        if (ratio_ == haRatio_ && hs_ == haHs_) return;
        ha_      = static_cast<double>(hs_) / std::max(1e-6, static_cast<double>(ratio_));
        haRatio_ = ratio_;
        haHs_    = hs_;
    }

    void reprimeSoft() {
        stRead_   = 0.0;
        stEnd_    = 0;
        grainM_   = 0;
        idealAcc_ = 0.0;
        primed_   = false;
        inCount_  = 0;
        lpcCount_ = 0;
        setCount_ = 0;
        // numChannels_, not kMaxChannels: this runs on the AUDIO CALLBACK (a
        // lowLatency/formant flip), and process() never touches a channel past
        // numChannels_, so clearing all eight made a stereo rig pay 4x for the
        // one block that saw the toggle.
        for (int c = 0; c < numChannels_; ++c) ch_[c].softReset();
        reprimeChannelClears_ += numChannels_;
        // Both clocks (inCount_ here, writeIdx_ in softReset) restart together —
        // they address the same absolute input timeline and a desync corrupts the
        // wet path permanently. The dry pass-through while priming covers the gap.
    }

    // =======================================================================
    // Grain production (WSOLA time-scale; resampling happens on read).
    // Returns false if not enough buffered input is available yet.
    // =======================================================================
    bool produceGrain(int nActive) {
        // ACCUMULATED ideal analysis position (see the header note): with a
        // swept ratio, ha_ * grainM_ would teleport the pointer on every change.
        const double ideal = idealAcc_;

        for (int c = 0; c < nActive; ++c) {
            Ch& cc = ch_[c];
            if (cc.onsetPending && cc.transientHold == 0) cc.transientHold = kTransientGrains;
            cc.onsetPending = false;
        }

        const long long highest = static_cast<long long>(std::floor(ideal)) + n_ + delta_ + 2;
        if (highest > inCount_) return false;               // wait for more input

        const long long base = static_cast<long long>(grainM_) * hs_;
        long long zeroTo = base + n_;
        for (long long p = stEnd_; p < zeroTo; ++p)
            for (int c = 0; c < nActive; ++c) ch_[c].stZero(p, stSize_);
        if (zeroTo > stEnd_) stEnd_ = zeroTo;

        const float* win = hann_;
        for (int c = 0; c < nActive; ++c) {
            Ch& ci = ch_[c];

            int bestD = 0;
            if (ci.hasHistory && ci.transientHold == 0 && grainM_ > 0)
                bestD = ci.searchBestLag(ideal, lc_, delta_, inSize_);

            const long long analysis = static_cast<long long>(std::floor(ideal)) + bestD;
            ci.addGrain(base, analysis, n_, win, stSize_, inSize_);

            ci.lastAnalysis = analysis;
            ci.hasHistory   = true;
            if (ci.transientHold > 0) --ci.transientHold;
        }

        ++grainM_;
        idealAcc_ += ha_;      // <-- accumulate, never ha_ * grainM_
        return true;
    }

    // =======================================================================
    // LPC (formant path). One coefficient set per channel per 12 ms hop, kept
    // in a small ring so the synthesis can read the set from synthLag_ frames
    // back (time-aligning it with the audio leaving the WSOLA pipeline).
    // =======================================================================
    void computeLpcSet(Ch& cc) {
        // Hann-windowed frame ending at the newest raw sample.
        const int W = lpcWin_;
        for (int i = 0; i < W; ++i)
            frame_[static_cast<std::size_t>(i)] =
                lpcWindow_[static_cast<std::size_t>(i)] * cc.rawAt(i - W + 1, xSize_);

        // Autocorrelation r[0..P].
        double r[kLpcOrder + 1];
        for (int k = 0; k <= kLpcOrder; ++k) {
            double acc = 0.0;
            for (int i = k; i < W; ++i)
                acc += static_cast<double>(frame_[static_cast<std::size_t>(i)])
                     * static_cast<double>(frame_[static_cast<std::size_t>(i - k)]);
            r[k] = acc;
        }

        float* a = cc.setAt(setCount_);          // where this frame's set lands

        if (r[0] <= 1e-12) {                     // silence -> flat envelope (A = 1)
            for (int i = 0; i < kLpcOrder; ++i) a[i] = 0.f;
            return;
        }

        // Conditioning: white-noise correction + Gaussian lag window. Both keep
        // the Toeplitz system positive-definite, which is what guarantees
        // Levinson-Durbin returns a MINIMUM-PHASE A(z) (=> 1/A(z) is stable).
        r[0] *= (1.0 + kWhiteNoiseCorr);
        r[0] += 1e-9;
        for (int k = 1; k <= kLpcOrder; ++k) r[k] *= lagWin_[static_cast<std::size_t>(k)];

        // Levinson-Durbin.
        double aa[kLpcOrder + 1] = { 0.0 };
        double tmp[kLpcOrder + 1] = { 0.0 };
        double e = r[0];
        for (int i = 1; i <= kLpcOrder; ++i) {
            double acc = r[i];
            for (int j = 1; j < i; ++j) acc += aa[j] * r[i - j];
            double k = -acc / e;
            if (!(k > -0.9999 && k < 0.9999)) k = (k > 0.0) ? 0.9999 : -0.9999;

            for (int j = 1; j < i; ++j) tmp[j] = aa[j] + k * aa[i - j];
            for (int j = 1; j < i; ++j) aa[j] = tmp[j];
            aa[i] = k;

            e *= (1.0 - k * k);
            if (e <= 1e-12) break;
        }

        // Bandwidth expansion: pull the poles inside the unit circle so a
        // coefficient swap at a frame boundary cannot ring, and so the same set
        // can safely drive BOTH the analysis and the synthesis filter (making
        // them exact inverses => ratio 1 is a pass-through).
        double g = kGamma;
        for (int i = 1; i <= kLpcOrder; ++i) {
            a[i - 1] = static_cast<float>(aa[i] * g);
            g *= kGamma;
        }
    }

    void buildLagWindow() {
        // Gaussian lag window, ~kLagWinHz of formant broadening.
        for (int k = 0; k <= kLpcOrder; ++k) {
            const double t = kTwoPiLocal * kLagWinHz * static_cast<double>(k) / sampleRate_;
            lagWin_[static_cast<std::size_t>(k)] = std::exp(-0.5 * t * t);
        }
    }

    // =======================================================================
    // Per-channel state
    // =======================================================================
    struct Ch {
        std::vector<float> in;      // WSOLA input ring (raw OR residual) — pow2
        std::vector<float> st;      // stretched OLA ring — pow2
        std::vector<float> raw;     // raw input ring (LPC analysis) — pow2
        std::vector<float> sets;    // kLpcSets * kLpcOrder coefficient ring

        long long lastAnalysis = 0;
        bool      hasHistory   = false;

        // onset detector (per sample, on the RAW signal)
        float fastEnv = 0.f, slowEnv = 0.f;
        bool  armed = true;
        bool  onsetPending = false;
        int   transientHold = 0;

        // LPC filter memories
        std::array<float, kLpcOrder> xh{};   // whitening (FIR) history
        std::array<float, kLpcOrder> yh{};   // synthesis (IIR) history

        void alloc(int inSz, int stSz, int xSz) {
            in.assign(static_cast<std::size_t>(inSz), 0.f);
            st.assign(static_cast<std::size_t>(stSz), 0.f);
            raw.assign(static_cast<std::size_t>(xSz), 0.f);
            sets.assign(static_cast<std::size_t>(kLpcSets * kLpcOrder), 0.f);
            reset();
        }
        void reset() {
            std::fill(in.begin(), in.end(), 0.f);
            std::fill(st.begin(), st.end(), 0.f);
            std::fill(raw.begin(), raw.end(), 0.f);
            std::fill(sets.begin(), sets.end(), 0.f);
            xh.fill(0.f); yh.fill(0.f);
            lastAnalysis = 0; hasHistory = false;
            fastEnv = slowEnv = 0.f; armed = true; onsetPending = false; transientHold = 0;
            // The ring write clocks MUST restart with the shared input clock
            // (inCount_): the analysis grid addresses input by inCount_-based
            // absolute positions, while inAt() clamps to the writeIdx_ window.
            // Leaving them running desyncs the two clocks permanently.
            writeIdx_ = 0;
            rawIdx_   = 0;
        }
        void softReset() {
            std::fill(in.begin(), in.end(), 0.f);
            std::fill(st.begin(), st.end(), 0.f);
            std::fill(raw.begin(), raw.end(), 0.f);
            std::fill(sets.begin(), sets.end(), 0.f);
            xh.fill(0.f); yh.fill(0.f);
            lastAnalysis = 0; hasHistory = false; transientHold = 0; onsetPending = false;
            writeIdx_ = 0;
            rawIdx_   = 0;
            // keep the onset envelopes + armed state
        }

        // Per-sample onset detector: fast peak envelope vs slow mean, edge
        // triggered with hysteresis, so a sustained tone flags exactly once.
        void updateOnset(float x, float fastRel, float slowA) {
            const float ax = std::fabs(x);
            fastEnv = (ax > fastEnv) ? ax : (fastRel * fastEnv);
            slowEnv += slowA * (ax - slowEnv);
            if (armed) {
                if (fastEnv > 0.01f && fastEnv > 3.0f * (slowEnv + 1e-6f)) {
                    onsetPending = true;
                    armed = false;
                }
            } else if (fastEnv < 1.5f * (slowEnv + 1e-6f)) {
                armed = true;
            }
        }

        // ---- raw ring (LPC analysis) ----
        void pushRaw(float x, int xSz) {
            raw[static_cast<std::size_t>(rawIdx_ & (xSz - 1))] = x;
            ++rawIdx_;
        }
        // Sample at relative offset o from the newest (o = 0 -> newest).
        float rawAt(int o, int xSz) const {
            long long a = rawIdx_ - 1 + static_cast<long long>(o);
            if (a < 0) return 0.f;
            if (a >= rawIdx_) a = rawIdx_ - 1;
            if (a < rawIdx_ - xSz) return 0.f;
            return raw[static_cast<std::size_t>(a & (xSz - 1))];
        }

        // ---- LPC coefficient ring ----
        float* setAt(long long idx) {
            return sets.data()
                 + static_cast<std::size_t>((idx & (kLpcSets - 1)) * kLpcOrder);
        }
        const float* setAt(long long idx) const {
            return sets.data()
                 + static_cast<std::size_t>((idx & (kLpcSets - 1)) * kLpcOrder);
        }

        // e[n] = x[n] + sum a_i x[n-i]   (A(z), FIR, minimum-phase)
        float whiten(float x, long long setCount) {
            if (setCount <= 0) { shift(xh, x); return x; }   // no set yet: pass through
            const float* a = setAt(setCount - 1);
            float e = x;
            for (int i = 0; i < kLpcOrder; ++i) e += a[i] * xh[static_cast<std::size_t>(i)];
            shift(xh, x);
            return e;
        }

        // y[n] = e[n] - sum a_i y[n-i]   (1/A(z), IIR, stable because A is
        // minimum-phase and bandwidth-expanded). `lag` picks the set that
        // belongs to the audio now leaving the WSOLA pipeline.
        float synth(float e, long long setCount, int lag) {
            const long long idx = setCount - 1 - static_cast<long long>(lag);
            if (idx < 0) { shift(yh, e); return e; }         // not enough history yet
            const float* a = setAt(idx);
            float y = e;
            for (int i = 0; i < kLpcOrder; ++i) y -= a[i] * yh[static_cast<std::size_t>(i)];
            if (!std::isfinite(y) || y > 32.f || y < -32.f) {
                // Anti-latch: a pathological set + transient can never poison the
                // recursion permanently.
                yh.fill(0.f);
                return 0.f;
            }
            shift(yh, y);
            return y;
        }

        static void shift(std::array<float, kLpcOrder>& h, float v) {
            for (int i = kLpcOrder - 1; i > 0; --i)
                h[static_cast<std::size_t>(i)] = h[static_cast<std::size_t>(i - 1)];
            h[0] = v;
        }

        // ---- WSOLA input ring (absolute indexing by inCount_) ----
        void pushIn(float x, int inSz) {
            in[static_cast<std::size_t>(writeIdx_ & (inSz - 1))] = x;
            ++writeIdx_;
        }
        float inAt(long long a, int inSz) const {
            const long long lo = writeIdx_ - inSz;
            if (a < lo) a = lo;
            if (a >= writeIdx_) a = writeIdx_ - 1;
            if (a < 0) return 0.f;
            return in[static_cast<std::size_t>(a & (inSz - 1))];
        }

        // ---- stretched OLA ring ----
        void stZero(long long p, int stSz) {
            st[static_cast<std::size_t>(p & (stSz - 1))] = 0.f;
        }
        void stAdd(long long p, float v, int stSz) {
            st[static_cast<std::size_t>(p & (stSz - 1))] += v;
        }
        float stAt(long long p, int stSz) const {
            return st[static_cast<std::size_t>(p & (stSz - 1))];
        }
        // 4-point 3rd-order Lagrange read at a fractional stretched position.
        float readSt(double pos, int stSz) const {
            const long long i0 = static_cast<long long>(std::floor(pos));
            const float f = static_cast<float>(pos - static_cast<double>(i0));
            const float ym1 = stAt(i0 - 1, stSz);
            const float y0  = stAt(i0,     stSz);
            const float y1  = stAt(i0 + 1, stSz);
            const float y2  = stAt(i0 + 2, stSz);
            const float fm1 = f - 1.f, fm2 = f - 2.f, fp1 = f + 1.f;
            const float c_m1 = f   * fm1 * fm2 * (-1.f / 6.f);
            const float c_0  = fp1 * fm1 * fm2 * ( 0.5f);
            const float c_1  = fp1 * f   * fm2 * (-0.5f);
            const float c_2  = fp1 * f   * fm1 * ( 1.f / 6.f);
            return c_m1 * ym1 + c_0 * y0 + c_1 * y1 + c_2 * y2;
        }

        // Cross-correlation splice search (coarse decimated pass + full-rate
        // refine), with a centre bias so a periodic signal picks a CONSISTENT
        // lag grain-to-grain instead of jittering between pitch periods.
        int searchBestLag(double ideal, int lc, int delta, int inSz) const {
            const long long baseIdeal = static_cast<long long>(std::floor(ideal));
            const long long tgt = lastAnalysis + lc;
            const float invDelta = 1.f / static_cast<float>(delta);

            auto scoreAt = [&](int d, int stride) -> float {
                const long long cand = baseIdeal + d;
                float dot = 0.f, ec = 0.f, et = 0.f;
                for (int j = 0; j < lc; j += stride) {
                    const float a = inAt(cand + j, inSz);
                    const float b = inAt(tgt + j,  inSz);
                    dot += a * b; ec += a * a; et += b * b;
                }
                const float norm = std::sqrt(ec * et) + 1e-9f;
                return dot / norm - 0.03f * (static_cast<float>(std::abs(d)) * invDelta);
            };

            float bestScore = -1e30f;
            int   bestD = 0;
            for (int d = -delta; d <= delta; d += 2) {
                const float s = scoreAt(d, 2);
                if (s > bestScore) { bestScore = s; bestD = d; }
            }
            float refScore = -1e30f;
            int   refD = bestD;
            const int dLo = (bestD - 1 < -delta) ? -delta : bestD - 1;
            const int dHi = (bestD + 1 >  delta) ?  delta : bestD + 1;
            for (int d = dLo; d <= dHi; ++d) {
                const float s = scoreAt(d, 1);
                if (s > refScore) { refScore = s; refD = d; }
            }
            return refD;
        }

        void addGrain(long long base, long long analysis, int curN,
                      const float* win, int stSz, int inSz) {
            for (int i = 0; i < curN; ++i)
                stAdd(base + i, win[i] * inAt(analysis + i, inSz), stSz);
        }

        long long writeIdx_ = 0;
        long long rawIdx_   = 0;
    };

    // =======================================================================
    // Small helpers
    // =======================================================================
    static float finite(float v, float fallback) { return (v == v) ? v : fallback; }
    static int evenRound(double v) {
        int n = static_cast<int>(v + 0.5);
        if (n & 1) ++n;
        return n;
    }
    static int nextPow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    static void buildHann(std::vector<float>& w, int N) {
        w.assign(static_cast<std::size_t>(N), 0.f);
        const double twoPiOverN = 6.28318530717958647692 / static_cast<double>(N);
        for (int i = 0; i < N; ++i)
            w[static_cast<std::size_t>(i)] = static_cast<float>(
                0.5 - 0.5 * std::cos(twoPiOverN * (static_cast<double>(i) + 0.5)));
    }

    // =======================================================================
    // Members
    // =======================================================================
    double sampleRate_  = 48000.0;
    int    numChannels_ = 1;
    bool   lowLatency_  = false;
    bool   formant_     = false;

    // WSOLA geometry
    int n_ = 1024, hs_ = 512, lc_ = 512, delta_ = 256;
    int inSize_ = 4096, stSize_ = 8192;
    int latency_ = 1320;
    // Audit counters for the RT gate (see hannBuilds() / reprimeChannelClears()).
    long long hannBuilds_ = 0;
    long long reprimeChannelClears_ = 0;
    // BOTH latency modes' Hann tables, built once per prepare(). `hann_` points
    // at the one in force; a mid-session mode flip swaps the pointer and builds
    // nothing (see selectHann()).
    std::vector<float> hannLarge_;
    std::vector<float> hannSmall_;
    const float*       hann_ = nullptr;

    // params / derived
    float  ratio_       = 1.f;
    float  ratioTarget_ = 1.f;
    float  glideCoef_   = 0.f;
    float  glideMs_     = 20.f;  // last requested; re-derived on every prepare
    float  mix_         = 1.f;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixS_;
    double ha_          = 512.0;
    float  haRatio_     = -1.f;   // memo: the ratio ha_ was derived from
    int    haHs_        = -1;     // memo: the hop  ha_ was derived from
    float  fastRel_ = 0.998f;
    float  slowA_   = 0.0003f;

    // LPC geometry
    int lpcWin_ = 1152, lpcHop_ = 576, xSize_ = 2048, synthLag_ = 3;
    // Ring sizes actually ALLOCATED (worst-case geometry). inSize_/stSize_ are
    // pinned to these; a mode flip may never raise them past the allocation.
    int allocIn_ = 0, allocSt_ = 0;
    int lpcCount_ = 0;
    long long setCount_ = 0;
    std::vector<float> lpcWindow_;
    std::vector<float> frame_;                       // windowed analysis frame
    std::array<double, kLpcOrder + 1> lagWin_{};

    // shared clock (all channels stay on one grid)
    double    stRead_   = 0.0;
    long long stEnd_    = 0;
    long long grainM_   = 0;
    double    idealAcc_ = 0.0;   // ACCUMULATED analysis position (see header)
    long long inCount_  = 0;
    bool      primed_   = false;

    std::array<Ch, kMaxChannels> ch_;
};

} // namespace dcdsp

#endif // DCDSP_WHAMMY_H
