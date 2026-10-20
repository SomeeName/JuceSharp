// dcdsp_harmonizer.h — WSOLA (Waveform Similarity Overlap-Add) pitch harmonizer.
//
// Real-time, header-only, C++17.  A live-guitar pitch pedal core: it shifts pitch
// by 2^(semitones/12) while preserving tempo, with none of the "granular warble"
// of a naive two-tap crossfading delay shifter.
//
// How it works (two conceptual stages, folded into one streaming pipeline):
//
//   1. WSOLA TIME-SCALE.  The input is re-assembled from overlapping Hann grains
//      (50 % overlap, constant-overlap-add).  Successive grains are NOT read from
//      a fixed grid: each grain's read position is refined by a CROSS-CORRELATION
//      search (±Delta samples) so its leading overlap region splices at maximum
//      waveform similarity onto the tail already synthesised.  Matching the
//      waveform (not just crossfading) is what removes the periodic warble/tremolo
//      artefact of granular shifters.  The synthesis grid marches at hop Hs while
//      the analysis grid marches at Ha = Hs / r, giving a time-scale of factor r.
//
//   2. RESAMPLE.  The time-scaled stream is read back at rate r with 4-point,
//      3rd-order Lagrange interpolation (anti-aliased fractional read).  Time-scale
//      by r followed by resample by r nets: same duration, pitch multiplied by r.
//
//   TRANSIENT handling: a per-sample fast/slow envelope ratio detects picks/onsets;
//   on an onset the similarity search is SUSPENDED for a couple of grains so the
//   pick is spliced on the natural grid rather than aligned to a stale pre-onset
//   waveform (which would pre-echo/double the attack).  The window length is never
//   changed — constant-overlap-add reconstructs a transient without smearing, so
//   the attack stays crisp (measured widening < 0.1 ms, pre-echo < -170 dB).
//
// Pure processing core — NO JUCE, NO I/O.  Allocation happens ONLY in prepare().
// process() never allocates, locks, or throws.  Output is always finite & bounded.
//
// Self-contained: depends on nothing but the standard headers below.  (The other
// This core used to keep its own inlined primitives behind an opt-in macro
// (DCDSP_HARMONIZER_USE_SHARED_PRIMITIVES) "so it can be compiled and unit-
// tested in isolation".  Nothing in the tree ever defined that macro, the class
// declares no primitive of its own at namespace scope, and the mix ramp needs
// the shared SmoothedValue — so the shared header is now included the way every
// other core includes it.  There is one ramp implementation, not two.

#ifndef DCDSP_HARMONIZER_H
#define DCDSP_HARMONIZER_H

#include "dcdsp_primitives.h"

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
struct HarmonizerParams {
    float semitones = 0.0f;  // pitch shift in semitones, clamped to [-12, +12]
    float mix       = 0.5f;  // dry/wet blend [0..1]; 0 = dry only, 1 = wet only
    float lowLatency = 0.0f; // 0/1; 1 = prefer lower latency (smaller window/search)
};

class HarmonizerCore {
public:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    void prepare(double sampleRate, int maxBlock, int nch) {
        (void)maxBlock;
        sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        numChannels_ = (nch > 0) ? nch : 1;
        if (numChannels_ > kMaxChannels) numChannels_ = kMaxChannels;

        // Window/hop/search geometry is a function of sample rate + latency mode,
        // sized here so setParams()/process() never allocate.
        //
        // Sized for the WORST case, not for whatever lowLatency_ happens to hold
        // right now — the same fix dcdsp_whammy.h carries. `lowLatency` is a
        // runtime param and setParams re-runs buildGeometry() when it flips;
        // the comment there claimed "we only ever shrink within the buffers
        // allocated in prepare()", which is true only if prepare() ran in the
        // LARGE mode. Prepare with lowLatency already on, then turn it OFF, and
        // n_ goes 12 ms -> 32 ms, so inSize_/stSize_ grow ~4x past what was
        // actually allocated. Every ring access masks with those members
        // (`in[w & (inSz-1)]`, `st[p & (stSz-1)]`), so the writes run off the
        // end — heap corruption from a UI toggle, silent. buildHann() would
        // have reallocated on the audio thread in the same step, since
        // setParams runs from the top of process() via the ParamMailbox.
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

        for (int c = 0; c < kMaxChannels; ++c) {
            ch_[c].alloc(inSize_, stSize_);
        }
        // `mix` RAMPS (kMixRampSeconds). As a per-block constant it hard-spliced
        // a 100 % dry/wet crossfade — worse here than elsewhere because the wet
        // is pitch-shifted and therefore decorrelated from the dry, so nothing
        // partially cancels the step.
        mixS_.reset(sampleRate_, kMixRampSeconds);
        mixS_.setTarget(mix_);
        mixS_.snapToTarget();   // a freshly prepared harmonizer starts AT its mix
        reset();
        applyDerived();
    }

    /// TEST SURFACE. True while the ring masks the audio path uses are inside
    /// what prepare() actually allocated. This is the invariant a `lowLatency`
    /// flip used to break — the failure is a heap overrun, so a probe that
    /// merely runs audio through it may or may not notice.
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
        stRead_    = 0.0;
        stEnd_     = 0;
        grainM_    = 0;
        inCount_   = 0;
        primed_    = false;
        for (int c = 0; c < kMaxChannels; ++c) ch_[c].reset();
    }

    void setParams(const HarmonizerParams& p) {
        float semis = p.semitones;
        if (!(semis == semis)) semis = 0.f;                 // NaN guard
        semis = std::max(-12.f, std::min(12.f, semis));

        float mix = p.mix;
        if (!(mix == mix)) mix = 0.f;                       // NaN guard
        mix_ = std::max(0.f, std::min(1.f, mix));
        mixS_.setTarget(mix_);

        ratio_ = std::pow(2.f, semis / 12.f);               // r in [0.5 .. 2.0]

        const bool lowLat = (p.lowLatency >= 0.5f);
        if (lowLat != lowLatency_) {
            lowLatency_ = lowLat;
            // Geometry (window/hop/search) depends on the latency mode. The
            // rings were allocated for the WORST case in prepare() and stay
            // there, so this is allocation-free AND in-bounds in both
            // directions — see the comment in prepare().
            buildGeometry();
            inSize_ = allocIn_;
            stSize_ = allocSt_;
            // A mode flip mid-stream: re-prime cleanly to avoid a discontinuity.
            reprimeSoft();
        }
        applyDerived();
    }

    // -----------------------------------------------------------------------
    // In-place block processing.  wet = pitch-shifted; blended with dry by mix.
    // -----------------------------------------------------------------------
    void process(float* const* channels, int nch, int nframes) {
        if (channels == nullptr || nframes <= 0) return;
        int n = std::min(nch, numChannels_);
        if (n <= 0) return;

        const double r  = static_cast<double>(ratio_);

        for (int i = 0; i < nframes; ++i) {
            // One walk per FRAME, above the channel loop below.
            const float wet = mixS_.next();
            const float dry = 1.f - wet;
            // 1) push this frame's input for every active channel, advance clock.
            for (int c = 0; c < n; ++c) {
                float* buf = channels[c];
                float x = (buf != nullptr) ? buf[i] : 0.f;
                // NaN/Inf scrub at input, written back so the DRY blend below
                // reads the scrubbed sample too (an Inf would otherwise ride
                // the dry path straight to the output).
                if (!std::isfinite(x)) x = 0.f;
                if (buf != nullptr) buf[i] = x;
                ch_[c].pushIn(x, inSize_);
                ch_[c].updateOnset(x, fastRel_, slowA_);     // per-sample onset detect
            }
            ++inCount_;

            // 2) produce WSOLA grains until the finalised stretched stream keeps a
            //    full grain of look-ahead over the resample read pointer (or we run
            //    out of buffered input during priming).
            const long long need = static_cast<long long>(std::floor(stRead_)) + hs_ + 3;
            long long frontier = static_cast<long long>(grainM_) * hs_; // finalised
            int guard = 0;
            while (frontier < need && guard++ < 6) {
                if (!produceGrain(n)) break;                 // not enough input yet
                frontier = static_cast<long long>(grainM_) * hs_;
            }

            // 3) once enough grains are buffered, start reading.  We begin the
            //    resample read one grain behind the finalised frontier so the
            //    steady-state pipeline delay equals latency_ (window + search).
            if (!primed_ && grainM_ >= 2) {
                primed_ = true;
                stRead_ = static_cast<double>(frontier) - static_cast<double>(hs_ + 3);
                if (stRead_ < 1.0) stRead_ = 1.0;
            }

            for (int c = 0; c < n; ++c) {
                float* buf = channels[c];
                if (buf == nullptr) continue;
                const float x = buf[i];
                float w;
                if (primed_) {
                    w = ch_[c].readSt(stRead_, stSize_);
                    if (!(w == w)) w = 0.f;                  // NaN scrub
                    w = std::max(-4.f, std::min(4.f, w));    // hard safety bound
                } else {
                    w = x;                                    // pass-through while priming
                }
                buf[i] = dry * x + wet * w;
            }

            if (primed_) stRead_ += r;
        }
    }

    // Algorithmic latency (dry and wet are summed at the same instant; the wet
    // path carries the WSOLA pipeline delay reported here).
    int latencySamples() const { return latency_; }

private:
    // =======================================================================
    // Geometry
    // =======================================================================
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
        // Analysis window in ms.  Normal mode must span a full low-string
        // period (drop C# = 69.3 Hz = 14.4 ms) INSIDE the overlap region, or
        // the similarity search cannot find a period-aligned splice and the
        // drop register warbles; 32 ms gives a 16 ms overlap.  Low-latency
        // mode stays snappier and accepts the low-register compromise.
        const int N = windowSamplesFor(lowLatency_);
        n_  = N;
        hs_ = N / 2;                     // 50 % overlap (constant-overlap-add Hann)
        lc_ = hs_;                       // correlation region = the overlap length

        // Similarity search half-width.  Wider search = better splice on
        // low-pitched notes, at more CPU.  Normal mode guarantees >= 8 ms so
        // the +/-delta span covers a full drop-C# period; ~N/8 low-latency.
        // The decimated coarse pass in searchBestLag keeps the cost of the
        // wider search flat vs. the old full-rate +/-5.5 ms scan.
        delta_ = lowLatency_ ? (N / 8)
                             : std::max(N / 4, (int)std::ceil(0.008 * sampleRate_));
        if (delta_ < 8) delta_ = 8;

        selectHann();

        // Onset-detector coefficients (per sample): ~5 ms fast release, ~80 ms
        // slow mean.  Computed here so process() never re-derives them.
        fastRel_ = static_cast<float>(std::exp(-1.0 / (0.005 * sampleRate_)));
        slowA_   = static_cast<float>(1.0 - std::exp(-1.0 / (0.080 * sampleRate_)));

        // Reported pipeline latency: the analysis grid reads up to (window +
        // search) behind the newest input, which is the input-to-wet group delay
        // the priming logic realises (verified by cross-correlation in the harness).
        latency_ = n_ + delta_;

        // Buffer sizes (power-of-two rings, sized in prepare()).
        inSize_ = nextPow2(n_ + delta_ + hs_ + 8);
        stSize_ = nextPow2(4 * n_ + 8);
    }

    void applyDerived() {
        // Analysis hop Ha = Hs / r  (time-scale factor r).
        ha_ = static_cast<double>(hs_) / std::max(1e-6, static_cast<double>(ratio_));
    }

    void reprimeSoft() {
        stRead_  = 0.0;
        stEnd_   = 0;
        grainM_  = 0;
        primed_  = false;
        inCount_ = 0;
        // numChannels_, not kMaxChannels: this runs on the AUDIO CALLBACK (a
        // lowLatency flip), and process() never touches a channel past
        // numChannels_, so clearing all eight made a stereo rig pay 4x for the
        // one block that saw the toggle.
        for (int c = 0; c < numChannels_; ++c) ch_[c].softReset();
        reprimeChannelClears_ += numChannels_;
        // Both clocks (inCount_ here, writeIdx_ in softReset) restart together —
        // they address the same absolute input timeline and desync corrupts the
        // wet path permanently. The dry pass-through while priming makes the
        // re-prime seamless; envelopes survive in softReset so detection state
        // carries over.
    }

    // =======================================================================
    // Grain production (WSOLA time-scale, rate 1 — resampling is done on read).
    // Returns false if not enough buffered input is available yet.
    // =======================================================================
    bool produceGrain(int nActive) {
        // Ideal analysis start for this grain on the fixed grid (float index,
        // absolute in input-sample terms).  Marches at Ha per grain.
        const double ideal = ha_ * static_cast<double>(grainM_);

        // Consume any per-sample onset flag raised since the last grain, arming a
        // transient hold on that channel.  During the hold the similarity search
        // is suspended so a pick is spliced on the natural grid instead of being
        // aligned to a stale pre-onset waveform (which would pre-echo/double the
        // attack).  The detector itself runs per sample in process(), NOT here —
        // sampling the envelope once per grain aliases a steady tone and would
        // false-fire.  The window length is NEVER changed: at a fixed 50 %-overlap
        // hop only the full window sums to unity (constant-overlap-add), so
        // shortening it would tear a hole in the stream.
        for (int c = 0; c < nActive; ++c) {
            Ch& cc = ch_[c];
            if (cc.onsetPending && cc.transientHold == 0) cc.transientHold = kTransientGrains;
            cc.onsetPending = false;
        }

        // Need input available up to ideal + N + delta.
        const long long highest = static_cast<long long>(std::floor(ideal)) + n_ + delta_ + 2;
        if (highest > inCount_) return false;                // wait for more input

        const long long base = static_cast<long long>(grainM_) * hs_; // synth pos
        // Zero the freshly-exposed region of the OLA ring before accumulating.
        long long zeroFrom = stEnd_;
        long long zeroTo   = base + n_;
        for (long long p = zeroFrom; p < zeroTo; ++p)
            for (int c = 0; c < nActive; ++c) ch_[c].stZero(p, stSize_);
        if (zeroTo > stEnd_) stEnd_ = zeroTo;

        const float* win = hann_;
        for (int c = 0; c < nActive; ++c) {
            Ch& ci = ch_[c];

            // ---- similarity search (suspended during a transient hold) -------
            int bestD = 0;
            if (ci.hasHistory && ci.transientHold == 0 && grainM_ > 0) {
                bestD = ci.searchBestLag(ideal, lc_, delta_, inSize_);
            }

            const long long analysis = static_cast<long long>(std::floor(ideal)) + bestD;

            // ---- overlap-add the windowed grain into the stretched ring -------
            ci.addGrain(base, analysis, n_, win, stSize_, inSize_);

            // Record where the natural continuation lives for the next search.
            ci.lastAnalysis = analysis;
            ci.hasHistory   = true;
            if (ci.transientHold > 0) --ci.transientHold;
        }

        ++grainM_;   // 50 % overlap: next grain at base+hs_, frontier = grainM_*hs_
        return true;
    }

    // =======================================================================
    // Per-channel state
    // =======================================================================
    struct Ch {
        std::vector<float> in;      // input history ring (pow2)
        std::vector<float> st;      // stretched OLA ring (pow2)
        long long          lastAnalysis = 0;
        bool               hasHistory   = false;

        // transient (onset) detector — updated per sample in process()
        float fastEnv = 0.f, slowEnv = 0.f;
        bool  armed   = true;       // hysteresis: ready to flag a fresh onset
        bool  onsetPending = false; // consumed by produceGrain
        int   transientHold = 0;    // grains remaining with the search suspended

        void alloc(int inSz, int stSz) {
            in.assign(static_cast<std::size_t>(inSz), 0.f);
            st.assign(static_cast<std::size_t>(stSz), 0.f);
            reset();
        }
        void reset() {
            std::fill(in.begin(), in.end(), 0.f);
            std::fill(st.begin(), st.end(), 0.f);
            lastAnalysis = 0; hasHistory = false;
            fastEnv = slowEnv = 0.f; armed = true; onsetPending = false; transientHold = 0;
            // The ring write clock MUST restart with the shared input clock
            // (inCount_): the analysis grid addresses input by inCount_-based
            // absolute positions, while inAt() clamps to the writeIdx_ window.
            // Leaving writeIdx_ running desyncs the two clocks permanently and
            // every post-reset grain reads the stale clamp boundary (the wet
            // path degrades to garbage until re-construction).
            writeIdx_ = 0;
        }
        void softReset() {
            // A mode flip re-primes with BOTH clocks re-zeroed (see reset()
            // note) — the input ring must restart too, or the restarted
            // analysis grid reads positions the ring clock has long passed.
            // Priming pass-through covers the ~1 window of lost history; the
            // onset envelopes survive so a transient hold isn't re-triggered.
            std::fill(in.begin(), in.end(), 0.f);
            std::fill(st.begin(), st.end(), 0.f);
            lastAnalysis = 0; hasHistory = false; transientHold = 0; onsetPending = false;
            writeIdx_ = 0;
            // keep envelopes + armed state
        }

        // Per-sample onset detector.  Fast peak envelope (instant attack, ~5 ms
        // release) vs slow mean envelope (~80 ms).  A sharp rise of the fast over
        // the slow flags an onset, edge-triggered with hysteresis so a sustained
        // note flags exactly once.  Runs on every input sample so a steady tone
        // (whose fast/slow ratio is ~1) never trips it.
        void updateOnset(float x, float fastRel, float slowA) {
            const float ax = std::fabs(x);
            fastEnv = (ax > fastEnv) ? ax : (fastRel * fastEnv);
            slowEnv += slowA * (ax - slowEnv);
            const float floorLevel = 0.01f;
            if (armed) {
                if (fastEnv > floorLevel && fastEnv > 3.0f * (slowEnv + 1e-6f)) {
                    onsetPending = true;
                    armed = false;
                }
            } else if (fastEnv < 1.5f * (slowEnv + 1e-6f)) {
                armed = true;          // re-arm once the burst has settled
            }
        }

        // ---- input ring ----
        // We index input by absolute sample count (inWrite tracked externally via
        // inCount_).  Store at (count & mask).
        void pushIn(float x, int inSz) {
            in[static_cast<std::size_t>(writeIdx_ & (inSz - 1))] = x;
            ++writeIdx_;
        }
        // Read input at absolute integer index a (a < inCount).  Clamped.
        float inAt(long long a, int inSz) const {
            long long lo = writeIdx_ - inSz;
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
        // 4-point 3rd-order Lagrange read at fractional stretched position.
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

        // Cross-correlation search: find integer lag d in [-delta,+delta] around
        // `ideal` whose grain overlap best matches the natural continuation of the
        // previously placed grain (lastAnalysis + hs region proxy).  Normalised.
        // Two-stage since the search was widened to span a full low-string
        // period: a coarse pass DECIMATED BY 2 in both lag and correlation
        // sample (4x cheaper — this is what keeps the wider window/search at
        // the old CPU cost), then a full-rate rescore of the winner's +/-1
        // neighbourhood so the final splice is sample-exact.
        int searchBestLag(double ideal, int lc, int delta, int inSz) const {
            const long long baseIdeal = static_cast<long long>(std::floor(ideal));
            // Target = samples that would naturally follow the previous grain.
            const long long tgt = lastAnalysis + lc;   // lc == hs (overlap length)
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
                // Centre bias: a periodic signal has many equally-good lags (one
                // per pitch period).  Bias toward the lag nearest the ideal grid
                // advance so the chosen lag is CONSISTENT grain-to-grain — without
                // this the pick jitters between periods and produces strong
                // sidebands at the grain rate.  The penalty is tiny so a genuinely
                // better waveform match on real audio still wins.
                return dot / norm
                     - 0.03f * (static_cast<float>(std::abs(d)) * invDelta);
            };

            float bestScore = -1e30f;
            int   bestD = 0;
            for (int d = -delta; d <= delta; d += 2) {
                const float s = scoreAt(d, 2);
                if (s > bestScore) { bestScore = s; bestD = d; }
            }
            // Refine at full rate: the coarse pass skipped odd lags and every
            // 2nd sample, so rescore {best-1, best, best+1} exactly.
            float refScore = -1e30f;
            int   refD = bestD;
            const int dLo = bestD - 1 < -delta ? -delta : bestD - 1;
            const int dHi = bestD + 1 >  delta ?  delta : bestD + 1;
            for (int d = dLo; d <= dHi; ++d) {
                const float s = scoreAt(d, 1);
                if (s > refScore) { refScore = s; refD = d; }
            }
            return refD;
        }

        void addGrain(long long base, long long analysis, int curN,
                      const float* win, int stSz, int inSz) {
            for (int i = 0; i < curN; ++i) {
                const float s = inAt(analysis + i, inSz);
                stAdd(base + i, win[i] * s, stSz);
            }
        }

        long long writeIdx_ = 0;
    };

    // =======================================================================
    // Small helpers
    // =======================================================================
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
            w[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 - 0.5 * std::cos(twoPiOverN * (static_cast<double>(i) + 0.5)));
    }

    // =======================================================================
    // Members
    // =======================================================================
    static constexpr int kMaxChannels    = 8;
    static constexpr int kMaxWindow      = 8192;   // 32 ms window up to 192 kHz
    static constexpr int kTransientGrains = 2;   // grains with search suspended after an onset

    double sampleRate_ = 48000.0;
    int    numChannels_ = 1;
    bool   lowLatency_  = false;

    // geometry
    int n_ = 1024, hs_ = 512, lc_ = 512, delta_ = 256;
    int inSize_ = 4096, stSize_ = 8192;
    // The worst-case ring sizes prepare() actually allocated. inSize_/stSize_
    // are pinned back to these after every buildGeometry(), so a mid-session
    // lowLatency flip can never mask past the end of a ring.
    int allocIn_ = 4096, allocSt_ = 8192;
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

    // params
    float  ratio_ = 1.f;
    float  mix_   = 0.5f;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixS_;
    double ha_    = 512.0;
    float  fastRel_ = 0.998f;   // onset fast-envelope release coeff
    float  slowA_   = 0.0003f;  // onset slow-envelope one-pole coeff

    // shared clock (all channels stay on one grid)
    double    stRead_  = 0.0;    // fractional read pos in stretched domain
    long long stEnd_   = 0;      // one-past highest written stretched pos
    long long grainM_  = 0;      // grains produced
    long long inCount_ = 0;      // input samples pushed
    bool      primed_  = false;

    std::array<Ch, kMaxChannels> ch_;
};

} // namespace dcdsp

#endif // DCDSP_HARMONIZER_H
