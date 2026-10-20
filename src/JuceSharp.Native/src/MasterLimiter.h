#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace dotcharter {

/// Look-ahead soft limiter over the FINAL master bus (ABI v34).
///
/// WHY IT EXISTS. The device buffer the engine hands the driver is the SUM of
/// the song bus (post master gain, post EQ), every seat's monitor mix (post
/// rig, and a rig ending in a boost pedal legitimately leaves 1.0 behind), the
/// click bus and both instrument legs. Nothing downstream of that sum bounded
/// it: `dcdsp::scrubNonFinite` repairs NaN/Inf and says nothing at all about
/// magnitude, so a loud rig plus a loud backing track plus a click handed the
/// driver samples past +-1.0. A hardware DAC clips them; the STREAMING MIRROR
/// (`mirror_.pushBlock`, i.e. what OBS / Discord capture) carries float, so the
/// over-range samples survive into the encoder and WRAP there. That is the
/// reported artefact, and it is why this stage sits before the mirror pickup
/// and not merely before the device.
///
/// WHAT IT IS. A gain computer over the block's own peak envelope, smoothed
/// per sample, applied to a look-ahead-DELAYED copy of the signal, with a hard
/// clamp at the ceiling as a backstop. The look-ahead is what makes it a
/// limiter rather than a clipper: the gain is already down by the time the
/// peak it is reacting to reaches the output.
///
/// FIVE THINGS ARE LOAD-BEARING.
///
/// 1. **SWITCHED OFF, IT IS NOT IN THE PATH AT ALL** — not a delay line
///    running at unity gain, not a memcpy, nothing. The output is
///    bit-for-bit what it was before v34 and the reported latency is 0. That
///    is the same rule `playerCount_ == 1` follows for the v28 seats and the
///    v29 EQ follows at engine start: a user must not pay for, or hear any
///    change from, a feature they are not using — and here "pay" would have
///    meant 1 ms on their own monitored playing.
///
///    **AND IT IS OFF AT ENGINE START (v35).** v34 shipped it default ON, which
///    made the sentence above true only for a user who had switched it off: the
///    live monitor path runs through this stage, so every guitarist gained 1 ms
///    on their own playing by upgrading and every existing consumer's
///    click-alignment arithmetic shifted by 48 samples at 48 kHz. The reason for
///    ON — the streaming mirror's float path wrapping in an encoder — is real,
///    but the mirror does not need the live monitor delayed. The HOST opts in.
///
///    The first version of this DID keep the delay running when disabled, so
///    that the latency stayed constant across a toggle and the v26 render
///    clock's click anchor could not move. The RT gate settled it: 48 samples
///    of unconditional delay shifted every output block and broke
///    `RtEqTrimRampsAndSparseInputIsSilent`, which measures an EQ trim ramp
///    sample by sample. A stage that silently re-times the whole engine when
///    it is switched OFF is worse than a click scheduler that has to re-read
///    `dca_engine_get_output_latency_samples` after a toggle — which it does
///    anyway, because that export now reports what is actually there.
///
///    The consequence to know about: toggling `enabled` mid-song inserts or
///    removes 1 ms in one block, i.e. a click. It is a user gesture, it is
///    rare, and the alternative was making everyone pay for it always. The
///    delay line is CLEARED on the OFF -> ON edge so the stage cannot replay
///    audio from whenever it was last on.
///
/// 2. **The applied gain is PER-SAMPLE SMOOTHED, never a per-block constant.**
///    Same rule as `AudioEngine::rampOrScale`: a gain that steps at the block
///    boundary is a zipper, and this one moves on every transient rather than
///    only while a user drags a slider.
///
/// 3. **The gain computer is a SLIDING MINIMUM over the look-ahead window,
///    not the instantaneous target.** With the instantaneous target the
///    smoother only starts moving when the peak is already at the output, so
///    every attack punches through and the backstop clamp does all the work —
///    i.e. it degrades to a clipper. The window minimum sees the peak `L`
///    samples early, which is exactly the advance the delay line buys.
///    Computed with a monotonic deque over pre-sized arrays: O(1) amortised,
///    no allocation, no per-sample O(L) scan (L is up to 288 samples at
///    192 kHz, which a naive rescan would turn into 288 ops per sample).
///
/// 4. **No transcendental on the callback.** Both smoothing coefficients are
///    the first-order `k = dt/tau` form the ABI v30 node meters use, so a
///    release-time change costs a divide and not an `exp`. The `release_ms`
///    -> coefficient conversion is cached against the last value seen.
///
/// 5. **It SKIPS the block rather than growing.** `prepare()` sizes for the
///    device's channel count and there is no `resize` anywhere below it. A
///    variable-block driver that hands more channels than were prepared gets
///    an untouched, unlimited block — the same "skip, never allocate" rule
///    every other audio-thread buffer in this engine follows. **A skip is
///    COUNTED** (`skippedBlocks()`, cumulative) and `latencySamples()` reports
///    0 **for a block that skipped**: that block reached the driver and the
///    streaming mirror unbounded, which is the exact artefact this stage exists
///    to prevent, and it used to happen with nothing said anywhere while the
///    reported output latency stayed wrong by 1 ms.
///
///    **The latency figure describes the LAST BLOCK, not the session** (W4-N13).
///    It briefly gated on the cumulative counter instead, which made one skipped
///    block report 0 for ever — including for every later block that WAS
///    delayed, i.e. the same 1 ms error with the sign flipped. See
///    `latencySamples()`.
class MasterLimiter
{
public:
    /// Look-ahead window. 1.0 ms at 48 kHz is 48 samples; the ABI caps the
    /// concept at 1.5 ms and this stays under it deliberately, because the
    /// live monitor path runs THROUGH here and a guitarist pays this latency
    /// on their own playing.
    static constexpr double kLookaheadMs = 1.0;

    /// Ceiling and release defaults. -0.3 dBFS is the usual "true peak"
    /// headroom allowance for a lossy encoder downstream (which is exactly
    /// what the streaming mirror feeds).
    static constexpr float  kDefaultCeilingDb = -0.3f;
    static constexpr float  kDefaultReleaseMs = 80.0f;

    /// Message thread, with no callback in flight (prepareMasterBus).
    /// Allocates; nothing below it ever does.
    void prepare(double sampleRate, int numChannels)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels_   = std::max(1, numChannels);

        lookahead_ = (int) std::lround(sampleRate_ * kLookaheadMs / 1000.0);
        lookahead_ = std::max(1, std::min(lookahead_, 4096));

        delay_.assign((size_t) channels_ * (size_t) lookahead_, 0.0f);
        dqVal_.assign((size_t) lookahead_ + 1, 0.0f);
        dqIdx_.assign((size_t) lookahead_ + 1, 0LL);

        // tau = L/4 samples: the one-pole is within ~2 % of the window minimum
        // after a full look-ahead window, and the clamp covers the remainder.
        attackCoef_ = std::min(1.0f, 4.0f / (float) lookahead_);
        // A re-prepare IS the fix for the skip: the stage is now sized for this
        // device, so the count starts again.
        skipped_.store(0, std::memory_order_relaxed);
        // Seeded, not zeroed: a prepared stage that has not run a block yet DOES
        // contribute its look-ahead to the next one, and a click scheduler that
        // reads the figure between prepare() and the first callback must not be
        // told otherwise.
        lastAppliedLatency_.store(lookahead_, std::memory_order_relaxed);
        cachedReleaseMs_   = -1.0f;
        cachedReleaseCoef_ = 0.0f;

        resetState();
    }

    /// Message thread. Clears the delay line and unwinds the gain.
    void resetState() noexcept
    {
        std::fill(delay_.begin(), delay_.end(), 0.0f);
        writePos_  = 0;
        sampleIdx_ = 0;
        dqHead_ = dqCount_ = 0;
        gain_   = 1.0f;
        wasEnabled_ = false;
        reductionEnv_.store(1.0f, std::memory_order_relaxed);
    }

    /// The look-ahead this stage would contribute IF it is enabled. The
    /// enabled-ness is the ENGINE's, so `AudioEngine::masterLimiterLatencySamples`
    /// gates on it there — on the message thread, where the flag is written —
    /// rather than mirroring it into a second atomic here that the audio thread
    /// would have to publish and a caller could read before the first block.
    /// ...and 0 while the stage is SKIPPING. A skip means the block reached the
    /// driver and the mirror unbounded, i.e. the stage is not in the path
    /// however the flag reads, so reporting 1 ms of look-ahead would be wrong by
    /// 48 samples at 48 kHz on the one path that consumes it (click alignment,
    /// the render-clock anchor, the MIDI-out deadline).
    ///
    /// THIS DESCRIBES THE LAST BLOCK, NOT THE SESSION (W4-N13). It used to read
    /// `skipped_ > 0`, and `skipped_` is a cumulative counter cleared only by
    /// prepare() — so ONE skipped block made this answer 0 for ever, including
    /// for every subsequent block the stage did delay. That is the same 1 ms
    /// error the branch was added to prevent, with the sign flipped, and it is
    /// the easier one to hit: a skip is per-block (numChannels > channels_), and
    /// the very next block at the prepared width is limited normally.
    ///
    /// `lastAppliedLatency_` is seeded to lookahead_ by prepare() and written by
    /// the audio thread on the skip and process branches ONLY — deliberately not
    /// on the true-bypass branch. The enabled/disabled gate stays where the class
    /// comment puts it, on the message thread; mirroring it here would make a
    /// freshly enabled limiter report 0 until its first callback had run.
    int latencySamples() const noexcept
    {
        if (delay_.empty()) return 0;
        return lastAppliedLatency_.load(std::memory_order_relaxed);
    }

    /// How many blocks this stage SKIPPED — never prepared, or wider than the
    /// prepared channel count. Every other skip in this engine is either
    /// harmless (a shifter skipped for one block) or counted; this one let the
    /// exact artefact the stage exists to prevent through to the driver AND the
    /// streaming mirror with nothing said anywhere. Poll-only, folded into the
    /// health readout. `prepare()` resets it — a wider device is a new device.
    unsigned long skippedBlocks() const noexcept
    { return skipped_.load(std::memory_order_relaxed); }

    /// The channel count prepare() sized the delay line for; 0 before the
    /// first prepare. Message thread. DotCharter/JuceSharp #31: every
    /// master-bus stage must report the same width as this.
    int preparedChannels() const noexcept { return delay_.empty() ? 0 : channels_; }

    /// Poll-only, like the ABI v30 node meters: a decaying hold of the LOWEST
    /// gain the limiter applied, as a positive number of dB of reduction.
    float reductionDb() const noexcept
    {
        const float g = reductionEnv_.load(std::memory_order_relaxed);
        if (! (g > 0.0f) || g >= 1.0f) return 0.0f;
        return -20.0f * std::log10(g);
    }

    /// Audio thread. `out` is the final device buffer; channels JUCE did not
    /// provide are null and are skipped, exactly as every sibling stage here
    /// does. Returns false when the block was skipped (never prepared, or
    /// wider than the prepared width).
    bool process(float* const* out, int numChannels, int numSamples,
                 bool enabled, float ceilingLin, float releaseMs) noexcept
    {
        if (out == nullptr || numSamples <= 0 || numChannels <= 0) return false;
        if (delay_.empty() || numChannels > channels_)
        {
            // COUNTED. This is the one skip in the engine that is neither
            // harmless nor previously counted: the block goes to the driver and
            // to the streaming mirror UNBOUNDED, which is precisely the artefact
            // this stage exists to prevent, and latencySamples() then reports 0
            // so the figure a click scheduler reads stops disagreeing with what
            // is in the path.
            skipped_.fetch_add(1, std::memory_order_relaxed);
            // THIS block contributed no delay. Cleared per-block, not latched.
            lastAppliedLatency_.store(0, std::memory_order_relaxed);
            return false;
        }

        // TRUE BYPASS. Not a delay line at unity, not a memcpy: the block is
        // untouched and the reported latency is 0, so an engine with the
        // limiter off is byte-for-byte the pre-v34 engine. See rule 1 in the
        // class comment for why the always-delayed version was wrong.
        if (! enabled)
        {
            if (wasEnabled_)
            {
                // Clear on the way OUT, so the next OFF -> ON edge cannot fade
                // in 1 ms of audio from whenever the limiter was last on. Doing
                // it here rather than on the ON edge keeps the enable path free
                // of a std::fill over the delay line on the callback.
                std::fill(delay_.begin(), delay_.end(), 0.0f);
                writePos_ = 0; sampleIdx_ = 0; dqHead_ = dqCount_ = 0;
                gain_ = 1.0f;
                wasEnabled_ = false;
            }
            reductionEnv_.store(1.0f, std::memory_order_relaxed);
            return true;
        }
        wasEnabled_ = true;
        // This block IS going through the delay line: restore the figure a skip
        // may have cleared. Unconditional rather than edge-triggered — one
        // relaxed store on a path that is about to do L multiply-accumulates.
        lastAppliedLatency_.store(lookahead_, std::memory_order_relaxed);

        if (! (ceilingLin > 0.0f) || ! std::isfinite(ceilingLin)) ceilingLin = 1.0f;

        // dt/tau, cached: a release-time change is rare and an exp() on the
        // callback for a smoothing coefficient is not worth its cycles.
        if (releaseMs != cachedReleaseMs_)
        {
            const float tauSamples =
                std::max(1.0f, (std::isfinite(releaseMs) ? releaseMs : kDefaultReleaseMs)
                                   * (float) sampleRate_ * 0.001f);
            cachedReleaseCoef_ = std::min(1.0f, 1.0f / tauSamples);
            cachedReleaseMs_   = releaseMs;
        }
        const float releaseCoef = cachedReleaseCoef_;

        float blockMinGain = 1.0f;
        const int L = lookahead_;

        for (int i = 0; i < numSamples; ++i)
        {
            // --- peak of the INCOMING (undelayed) frame -------------------
            float peak = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* p = out[ch];
                if (p == nullptr) continue;
                const float a = std::abs(p[i]);
                if (a > peak) peak = a;
            }

            // A non-finite sample cannot reach here in the shipping path (the
            // scrub runs immediately above), but a target computed from one
            // would poison the smoother permanently, so it is spelled out.
            float target = 1.0f;
            if (std::isfinite(peak) && peak > ceilingLin)
                target = ceilingLin / peak;

            // --- sliding minimum of the last L targets --------------------
            pushTarget(target, L);
            const float windowMin = dqVal_[(size_t) dqHead_];

            // --- one-pole toward it, per SAMPLE ---------------------------
            const float coef = (windowMin < gain_) ? attackCoef_ : releaseCoef;
            gain_ += (windowMin - gain_) * coef;
            if (! std::isfinite(gain_)) gain_ = 1.0f;
            gain_ = std::min(1.0f, std::max(0.0f, gain_));
            // SNAP to unity once the release is within half a thousandth of unity of
            // it. A one-pole approaches its target asymptotically and never
            // arrives, so without this the limiter multiplies EVERY later
            // sample by 0.99989 for the rest of the session — and the OFF
            // state, which is just this release running to unity, would never
            // become the bit-exact pass-through it is documented to be.
            if (windowMin >= 1.0f && gain_ > 0.9995f) gain_ = 1.0f;
            if (gain_ < blockMinGain) blockMinGain = gain_;

            // --- delay line, then the gain, then the backstop -------------
            const size_t slot = (size_t) writePos_;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* p = out[ch];
                if (p == nullptr) continue;
                float* line = delay_.data() + (size_t) ch * (size_t) L;
                const float delayed = line[slot];
                line[slot] = p[i];
                // The one-pole never reaches its target exactly, so a fast
                // transient can still leave a few tenths of a dB of overshoot.
                // Clamping is what makes the ceiling a GUARANTEE rather than a
                // tendency — and by construction it only ever touches samples
                // the smoother nearly caught.
                float v = delayed * gain_;
                if (v >  ceilingLin) v =  ceilingLin;
                if (v < -ceilingLin) v = -ceilingLin;
                p[i] = v;
            }
            if (++writePos_ >= L) writePos_ = 0;
        }

        // Decaying hold, same shape and the same 0.25 s tau as the v30 node
        // meters: one coefficient per BLOCK, no transcendental, and a UI
        // polling at 30 Hz still catches a single transient.
        const float dt   = (float) numSamples / (float) sampleRate_;
        const float k    = std::min(1.0f, dt / 0.25f);
        float env = reductionEnv_.load(std::memory_order_relaxed);
        env += (1.0f - env) * k;
        // SNAP to rest. A one-pole approaches unity asymptotically and never
        // arrives, so without this the meter parks at a few hundredths of a dB
        // for ever — a UI mirroring it shows a permanently lit limiter over
        // silence, which is the same "frozen lit meter" fault ABI v30's
        // setMetering(false) exists to prevent, one decimal place down.
        if (env > 0.9995f) env = 1.0f;
        if (blockMinGain < env) env = blockMinGain;
        reductionEnv_.store(env, std::memory_order_relaxed);
        return true;
    }

private:
    /// Monotonic-deque push for the sliding window minimum. Amortised O(1);
    /// every array is sized in prepare().
    void pushTarget(float t, int L) noexcept
    {
        // Drop everything at the back that this value dominates.
        const int cap = (int) dqVal_.size();
        while (dqCount_ > 0)
        {
            const int back = (dqHead_ + dqCount_ - 1) % cap;
            if (dqVal_[(size_t) back] < t) break;
            --dqCount_;
        }
        const int slot = (dqHead_ + dqCount_) % cap;
        dqVal_[(size_t) slot] = t;
        dqIdx_[(size_t) slot] = sampleIdx_;
        ++dqCount_;

        // Retire the front once it has fallen out of the window.
        while (dqCount_ > 0 && dqIdx_[(size_t) dqHead_] <= sampleIdx_ - (long long) L)
        {
            dqHead_ = (dqHead_ + 1) % cap;
            --dqCount_;
        }
        ++sampleIdx_;
    }

    double             sampleRate_ = 48000.0;
    int                channels_   = 0;
    int                lookahead_  = 0;

    std::vector<float> delay_;
    int                writePos_   = 0;

    std::vector<float>     dqVal_;
    std::vector<long long> dqIdx_;
    int                    dqHead_  = 0;
    int                    dqCount_ = 0;
    long long              sampleIdx_ = 0;

    bool  wasEnabled_ = false;   // audio thread only: the OFF-edge clear latch
    float gain_       = 1.0f;
    float attackCoef_ = 0.1f;
    float cachedReleaseMs_   = -1.0f;
    float cachedReleaseCoef_ = 0.0f;

    std::atomic<float> reductionEnv_{ 1.0f };
    /// Blocks the stage skipped (see skippedBlocks()). Audio thread writes,
    /// message thread polls.
    std::atomic<unsigned long> skipped_{ 0 };
    /// The look-ahead the LAST block actually went through (W4-N13). Audio
    /// thread writes, message thread polls; seeded by prepare().
    std::atomic<int> lastAppliedLatency_{ 0 };
};

} // namespace dotcharter
